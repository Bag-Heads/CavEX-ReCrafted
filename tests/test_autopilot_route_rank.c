/*
	push-0921/B18 -- review B17 finding H4: ap_script_route_rank IS THE ONE
	THING THE SCRIPT SEAM CHANGES INSIDE A SHARED PRODUCTION FUNCTION, AND IT
	WAS UNPROVEN IN BOTH DIRECTIONS.

	autopilot_probe_route (source/game/autopilot.c:213) is walked by every
	terrain_safe consumer -- the CONFIG profiles far_walk_6m / split_anchor_6m /
	split_play_2m|8m / split_actions_6m -- and by the scripted mp_play driver.
	`unsigned cost = i;` (the table's own +Z-first order) was replaced by
	`ap_script_route_rank(dirs, 8u, i)`, guarded only by
	`if(!s_script_walk) return i;`. Review B17 ran two mutations past the suite
	and neither was caught:

	  A. `return i;` unconditionally -- the seam's walk then ignores the
	     requested heading entirely, the exact defect the seam exists to fix.
	  B. delete the `if(!s_script_walk)` guard -- config walkers get the
	     alignment ranking too, and every config trajectory moves.

	This file closes both. tests/test_autopilot_death_recovery.c's route cases
	cover the seam's WATER rule (a candidate is rejected), which is a different
	question: there the due--Z cell wins because the other seven are refused,
	not because of any ranking. Here every candidate is legal, so the ONLY
	thing that can decide the pick is the cost function.

	NOT A POISONED GOLDEN. The expected cells are not recorded output. They are
	derived, in the test, from two literals:
	  * off the seam, from autopilot.c's own `dirs` table, whose first entry is
	    {0,1} and whose cost is the index -- so the minimum cost is the FIRST
	    row, +Z, whenever no candidate carries the recent-cell penalty. The
	    table is restated below and DIRS_EXPECTED_FIRST is dirs[0], written as
	    the literal pair, not as "whatever the walker did last time".
	  * on the seam, from the heading itself: heading H must pick the unit cell
	    lying on H. That is the seam's contract (autopilot.h's
	    autopilot_script_walk comment: "ap_script_route_rank makes it propose
	    them in the order the requested heading asks for"), stated as
	    trigonometry rather than as an observation.

	THE ENGINE'S YAW CONVENTION, from ap_script_route_rank itself:
	    hx = sinf(yaw), hz = cosf(yaw), yaw = deg * PI/180
	so 0 deg = +Z, 90 deg = +X, 180 deg = -Z, 270 deg = -X.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "entity/entity.h"
#include "game/autopilot.h"
#include "game/game_state.h"
#include "game/gui/screen.h"

/* autopilot.c's candidate table, restated. The order IS the config walkers'
   cost function (`cost = i`), so this literal -- not a recording -- is what
   the off-seam expectations below are derived from. */
static const int dirs[8][2]
	= {{0, 1}, {-1, 1}, {1, 1}, {-1, 0}, {1, 0}, {-1, -1}, {1, -1}, {0, -1}};

/* ---- fixture ------------------------------------------------------------
   A flat stone plateau: surface at y=70, so a body whose eye is at 72.62 has
   its feet at 71.0 and all eight neighbours are legal, dry, level landings.
   Deliberately featureless: with no hazard, no fluid and no height change,
   autopilot_route_landing accepts all eight and the pick is decided purely by
   cost. (The same world shape test_autopilot_death_recovery.c's route_world()
   builds for its non-lake, non-pocket cases.) */
static void flat_world(void) {
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	static uint8_t ids[32768], nibble[16384], heights[256];
	memset(ids, 0, sizeof(ids));
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y <= 70; y++)
				ids[x * 2048 + z * 128 + y] = BLOCK_STONE;
	assert(world_build_full_section(&gstate.world, 16, 16, ids, nibble, nibble,
									nibble, heights, heights, heights));
	gstate.players[0].current_screen = &screen_ingame;
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 2; /* the fixed-heading walker both paths use */
	gstate.autopilot.tick = 21;
	autopilot_keepup_reset(&gstate.autopilot.keepup, true);
}

/* A TERRACE: the plateau (surface y=70) for world z <= 24, and a lower shelf
   whose surface is `south_top` for z >= 25 -- a drop of 70 - south_top blocks
   straight ahead of a body at (24, 24) facing +Z. */
static void terrace_world(int south_top) {
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	static uint8_t ids[32768], nibble[16384], heights[256];
	memset(ids, 0, sizeof(ids));
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			int top = (z + 16 >= 25) ? south_top : 70;
			for(int y = 0; y <= top; y++)
				ids[x * 2048 + z * 128 + y] = BLOCK_STONE;
		}
	assert(world_build_full_section(&gstate.world, 16, 16, ids, nibble, nibble,
									nibble, heights, heights, heights));
	gstate.players[0].current_screen = &screen_ingame;
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 2;
	gstate.autopilot.tick = 21;
	autopilot_keepup_reset(&gstate.autopilot.keepup, true);
}

static void flat_world_destroy(void) {
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);
}

/* Place the body at the CENTRE of cell (cx, cz) standing on the plateau, and
   wipe the route state so each probe starts from a clean history. */
static void body_at(struct entity* e, int cx, int cz) {
	memset(e, 0, sizeof(*e));
	entity_local_player(1, e, &gstate.world);
	e->pos[0] = (float)cx + 0.5F;
	e->pos[1] = 72.62F;
	e->pos[2] = (float)cz + 0.5F;
	e->data.local_player.capture_input = true;
	e->on_ground = true;
	gstate.players[0].local_player = e;
	dict_entity_set_at(gstate.entities, e->id, e);
	gstate.autopilot.terrain_safe = true;
	memset(&gstate.autopilot.route, 0, sizeof(gstate.autopilot.route));
}

/* One probe from (cx, cz) with a CLEARED route history; returns the chosen
   cell as an offset from it. */
static void probe_from(struct entity* e, int cx, int cz, int* dx, int* dz) {
	body_at(e, cx, cz);
	autopilot_tick();
	assert(gstate.autopilot.route.has_target
		   && "a flat plateau always offers eight legal landings");
	*dx = (int)floorf(gstate.autopilot.route.x) - cx;
	*dz = (int)floorf(gstate.autopilot.route.z) - cz;
}

/* Walk the body onto the cell the probe just chose, WITHOUT clearing the route
   history, and probe again. Standing exactly on the target centre satisfies
   autopilot_probe_route's arrival test (dx*dx+dz*dz < 0.12, on_ground, and the
   landing height equal to the feet), so the probe retires the target, records
   the cell it just reached, and picks a fresh one -- a real walk, with the real
   recent-cell penalty accumulating behind it. */
static void step_and_probe(struct entity* e, int cx, int cz, int* dx, int* dz) {
	e->pos[0] = (float)cx + 0.5F;
	e->pos[1] = 72.62F;
	e->pos[2] = (float)cz + 0.5F;
	e->on_ground = true;
	autopilot_tick();
	assert(gstate.autopilot.route.has_target);
	*dx = (int)floorf(gstate.autopilot.route.x) - cx;
	*dz = (int)floorf(gstate.autopilot.route.z) - cz;
}

/* ---- (a) OFF THE SEAM: the table's order, unchanged --------------------- */

/* With every candidate legal and none of them in the recent-cell history, the
   config walker's cost is its table index, so the minimum is dirs[0] -- +Z.
   Derived from the table literal at the top of this file. */
static void test_config_walker_follows_table_order(void) {
	struct entity e;
	int dx, dz;

	flat_world();
	probe_from(&e, 24, 24, &dx, &dz);
	if(dx != dirs[0][0] || dz != dirs[0][1])
		printf("  RED config walker picked (%+d,%+d); dirs[0] is (%+d,%+d) and "
			   "cost==index makes it the unique minimum\n",
			   dx, dz, dirs[0][0], dirs[0][1]);
	assert(dx == dirs[0][0] && dz == dirs[0][1]);
	flat_world_destroy();

	/* A SEQUENCE, not one probe, with the route history LEFT INTACT so the
	   recent-cell penalty is really in play. Every cell the walk has already
	   stood on is behind it, and dirs[0] (+Z) is never one of them -- it alone
	   keeps cost 0, so the walk is a straight +Z line. That expectation comes
	   from the table literal plus the penalty rule, not from a recording. */
	enum { STEPS = 6 };
	flat_world();
	int cx = 20, cz = 20;
	probe_from(&e, cx, cz, &dx, &dz); /* step 0 seeds the history */
	for(int k = 0; k < STEPS; k++) {
		if(dx != dirs[0][0] || dz != dirs[0][1])
			printf("  RED config walk step %d picked (%+d,%+d), want the "
				   "table's first row (%+d,%+d)\n",
				   k, dx, dz, dirs[0][0], dirs[0][1]);
		assert(dx == dirs[0][0] && dz == dirs[0][1]);
		cx += dx;
		cz += dz;
		step_and_probe(&e, cx, cz, &dx, &dz);
	}
	assert(cx == 20 && cz == 20 + STEPS);
	flat_world_destroy();

	printf("test_autopilot_route_rank: off the seam, %d consecutive probes all "
		   "take dirs[0] = (%+d,%+d) -- the table's own order\n",
		   STEPS + 1, dirs[0][0], dirs[0][1]);
}

/* THE GUARD ITSELF. `ap_script_route_rank` reads the static s_script_walk_yaw
   DIRECTLY, not through ap_script_walk_yaw()'s `s_script_walk ? ... : 0.0F`.
   So once any scripted leg has run, the yaw the guard is protecting the config
   walkers from is a LIVE, non-zero heading -- and dropping
   `if(!s_script_walk) return i;` re-ranks their candidates by it. Assert the
   seam at 90 degrees, release it, and require the config walker to be exactly
   what it was before the seam ever existed.

   (This ordering is what makes mutation B visible at all: at yaw 0 the
   alignment ranking happens to reproduce the table order exactly -- dirs is
   already sorted by alignment with +Z with the index as tie-break -- so a
   fresh-process config probe cannot tell the two apart. The guard earns its
   keep precisely in the after-a-leg case, which is the one a real mp_play run
   is in.) */
static void test_released_seam_leaves_config_walker_alone(void) {
	struct entity e;
	int dx, dz;

	flat_world();
	body_at(&e, 24, 24);
	autopilot_script_walk(true, 90); /* +X: the worst case for the guard */
	autopilot_tick();
	autopilot_script_walk(false, 90);
	autopilot_tick(); /* let ap_script_apply hand the config state back */

	probe_from(&e, 24, 24, &dx, &dz);
	if(dx != dirs[0][0] || dz != dirs[0][1])
		printf("  RED after a released 90-degree leg the config walker picked "
			   "(%+d,%+d); it must still take dirs[0] = (%+d,%+d). The "
			   "`if(!s_script_walk) return i;` guard is what keeps the stale "
			   "heading out of the config cost.\n",
			   dx, dz, dirs[0][0], dirs[0][1]);
	assert(dx == dirs[0][0] && dz == dirs[0][1]);
	flat_world_destroy();

	puts("test_autopilot_route_rank: a released 90-degree seam leg leaves the "
		 "config walker byte-identical (the !s_script_walk guard)");
}

/* ---- (b) ON THE SEAM: the heading decides ------------------------------- */

/* The cell lying on `deg`, in the engine's convention (hx = sin, hz = cos),
   computed from the heading -- not looked up in a recorded table. */
static void unit_cell_for(int deg, int* ux, int* uz) {
	/* The same GLM_PI autopilot_script_walk converts with; spelled locally so
	   this file does not depend on M_PI being visible under -std=c99. */
	const float pi = 3.14159265358979323846F;
	float yaw = (float)deg * pi / 180.0F;
	float hx = sinf(yaw), hz = cosf(yaw);
	*ux = (fabsf(hx) > 0.5F) ? (hx > 0.0F ? 1 : -1) : 0;
	*uz = (fabsf(hz) > 0.5F) ? (hz > 0.0F ? 1 : -1) : 0;
	assert((*ux != 0) != (*uz != 0) && "the four cardinals are axis-aligned");
}

static void test_seam_first_pick_is_the_heading(void) {
	static const int headings[4] = {0, 90, 180, 270};
	struct entity e;

	for(unsigned h = 0; h < 4; h++) {
		int want_x, want_z, dx, dz;
		unit_cell_for(headings[h], &want_x, &want_z);

		flat_world();
		body_at(&e, 24, 24);
		autopilot_script_walk(true, headings[h]);
		autopilot_tick();
		assert(gstate.autopilot.route.has_target
			   && "all eight neighbours are legal here");
		dx = (int)floorf(gstate.autopilot.route.x) - 24;
		dz = (int)floorf(gstate.autopilot.route.z) - 24;
		if(dx != want_x || dz != want_z)
			printf("  RED heading %d deg: first pick (%+d,%+d), want the cell "
				   "on that heading (%+d,%+d). A cost function that ignores "
				   "the heading picks the table's (+0,+1) for all four.\n",
				   headings[h], dx, dz, want_x, want_z);
		assert(dx == want_x && dz == want_z);
		autopilot_script_walk(false, headings[h]);
		flat_world_destroy();
	}

	puts("test_autopilot_route_rank: on the seam, headings 0/90/180/270 each "
		 "make the FIRST chosen neighbour the cell on that heading");
}

/* The diagonals too -- 45 degrees must reach the (+1,+1) corner, which is
   dirs[2] and would be the THIRD choice under `cost = i`. This is the cheapest
   possible witness that the ranking is a real permutation of the heading and
   not "swap two cardinals". */
static void test_seam_diagonal_heading(void) {
	struct entity e;

	flat_world();
	body_at(&e, 24, 24);
	autopilot_script_walk(true, 45);
	autopilot_tick();
	assert(gstate.autopilot.route.has_target);
	int dx = (int)floorf(gstate.autopilot.route.x) - 24;
	int dz = (int)floorf(gstate.autopilot.route.z) - 24;
	if(dx != 1 || dz != 1)
		printf("  RED heading 45 deg: first pick (%+d,%+d), want (+1,+1) -- "
			   "dirs[2], which cost==index would rank third\n",
			   dx, dz);
	assert(dx == 1 && dz == 1);
	autopilot_script_walk(false, 45);
	flat_world_destroy();

	puts("test_autopilot_route_rank: heading 45 deg reaches the (+1,+1) "
		 "diagonal, which the table order ranks third");
}

/* ---- (c) THE SCRIPTED WALKER ONLY TAKES STEPS IT CAN TAKE BACK -----------
   The route probe climbs ONE block and drops TWO. A walker that drops two can
   enter ground it can never leave: PC proof runs pc_mp_play_20260921_185536
   (a cliff pocket, south) and _194515 (a pit in a forest, north; y 71 -> 67)
   both ended "walker made no progress on four headings in a row" with nothing
   dug nearby. A config walker (far_walk_6m and friends) keeps its two-block
   drops, byte for byte -- it has no proof to finish and its trajectory is a
   recorded baseline. Under the script seam a drop is accepted only if the
   climb back exists: one block. */
static void test_seam_refuses_a_one_way_drop(void) {
	struct entity e;
	int dx, dz;

	/* OFF the seam, a two-block drop straight ahead is still dirs[0]. */
	terrace_world(68);
	probe_from(&e, 24, 24, &dx, &dz);
	if(dx != 0 || dz != 1)
		printf("  RED config walker at a 2-drop picked (%+d,%+d), want the "
			   "table's (+0,+1): its trajectory must not change\n", dx, dz);
	assert(dx == 0 && dz == 1);
	flat_world_destroy();

	/* ON the seam, heading +Z: none of the three two-block drops is taken. */
	terrace_world(68);
	body_at(&e, 24, 24);
	autopilot_script_walk(true, 0);
	autopilot_tick();
	assert(gstate.autopilot.route.has_target
		   && "the plateau behind and beside the body is still legal");
	dz = (int)floorf(gstate.autopilot.route.z) - 24;
	if(dz > 0)
		printf("  RED scripted walker took a one-way 2-block drop (dz=%+d)\n",
			   dz);
	assert(dz <= 0);
	autopilot_script_walk(false, 0);
	flat_world_destroy();

	/* ...while a ONE-block drop (the climb back exists) is taken at once. */
	terrace_world(69);
	body_at(&e, 24, 24);
	autopilot_script_walk(true, 0);
	autopilot_tick();
	assert(gstate.autopilot.route.has_target);
	dx = (int)floorf(gstate.autopilot.route.x) - 24;
	dz = (int)floorf(gstate.autopilot.route.z) - 24;
	assert(dx == 0 && dz == 1);
	autopilot_script_walk(false, 0);
	flat_world_destroy();

	puts("test_autopilot_route_rank: the scripted walker refuses a two-block "
		 "drop it could never climb back, takes a one-block drop, and the "
		 "config walker's two-block drop is unchanged");
}

static void test_probe_uses_body_world(void) {
	struct entity e;
	struct world secondary;
	flat_world();
	body_at(&e, 24, 24);
	world_create(&secondary);
	e.world = &secondary;
	autopilot_script_walk(true, 0);
	autopilot_tick();
	assert(!gstate.autopilot.route.has_target
		&& "primary floor cannot authorize a step in an empty secondary world");
	static uint8_t ids[32768], nibble[16384], heights[256];
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y <= 70; y++)
				ids[x * 2048 + z * 128 + y] = BLOCK_STONE;
	assert(world_build_full_section(&secondary, 16, 16, ids, nibble, nibble,
		nibble, heights, heights, heights));
	world_destroy(&gstate.world);
	world_create(&gstate.world);
	memset(&gstate.autopilot.route, 0, sizeof(gstate.autopilot.route));
	autopilot_tick();
	assert(gstate.autopilot.route.has_target
		&& "secondary floor remains usable when the primary world is empty");
	autopilot_script_walk(false, 0);
	world_destroy(&secondary);
	flat_world_destroy();
	puts("test_autopilot_route_rank: terrain probes follow the client body's world");
}

static void test_portal_geometry_reads_owner_and_blocked_corridor(void) {
	struct entity e;
	struct world secondary;
	flat_world();
	body_at(&e, 24, 24);
	world_create(&secondary);
	static uint8_t ids[32768], nibble[16384], heights[256];
	memset(ids, 0, sizeof(ids));
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y <= 70; y++)
				ids[x * 2048 + z * 128 + y] = BLOCK_STONE;
	for(int x = 23; x <= 26; x++)
		for(int y = 71; y <= 73; y++)
			ids[(x - 16) * 2048 + (24 - 16) * 128 + y]
				= x == 23 || x == 26 ? BLOCK_OBSIDIAN : BLOCK_PORTAL;
	for(int x = 24; x <= 25; x++)
		for(int y = 71; y <= 72; y++)
			ids[(x - 16) * 2048 + (23 - 16) * 128 + y] = BLOCK_STONE;
	assert(world_build_full_section(&secondary, 16, 16, ids, nibble, nibble,
		nibble, heights, heights, heights));
	e.world = &secondary;
	struct autopilot_portal_geometry geom;
	assert(autopilot_portal_geometry_sample(&e, &geom));
	assert(geom.x == 24 && geom.z == 24 && geom.feet_y == 71
		&& geom.portal_axis == 'X' && !geom.body_collision);
	assert(geom.cells[1][2][2] == 'P' && geom.cells[1][2][3] == 'P'
		&& geom.cells[1][2][1] == 'O'
		&& geom.cells[1][1][2] == '#');
	e.pos[2] = 23.5F;
	assert(autopilot_portal_geometry_sample(&e, &geom)
		&& geom.body_collision && geom.cells[1][2][2] == '#');
	world_destroy(&secondary);
	flat_world_destroy();
	puts("test_autopilot_route_rank: owning portal geometry and blocked approach");
}

static void return_block(int x, int y, int z, uint8_t id) {
	world_set_block(&gstate.world, x, y, z,
		(struct block_data) {.type = id}, false);
}

static void test_portal_return_only_on_safe_rearmed_route(void) {
	struct entity e;
	flat_world();
	body_at(&e, 24, 24);
	for(int z = 23; z <= 26; z++) {
		for(int y = 71; y <= 73; y++)
			return_block(24, y, z,
				z == 23 || z == 26 || y == 73
					? BLOCK_OBSIDIAN : BLOCK_PORTAL);
	}
	struct autopilot_portal_return_anchor a;
	struct autopilot_portal_return_step step;
	assert(autopilot_portal_return_anchor(&e, &a));
	assert(a.world == &gstate.world && a.feet_y == 71
		&& a.x[0] == 24 && a.x[1] == 24
		&& a.z[0] == 24 && a.z[1] == 25);
	assert(autopilot_portal_return_step(&e, &a, &step) && step.inside);
	e.pos[2] = 25.9F; /* center in P, body overlaps obsidian end jamb */
	assert(!autopilot_portal_return_step(&e, &a, &step));
	e.pos[2] = 24.5F;
	assert(!autopilot_portal_return_rearmed(400, 1599));
	assert(autopilot_portal_return_rearmed(400, 1600));
	assert(!autopilot_portal_return_rearmed(400, 400));
	e.pos[0] = 27.2F;
	assert(autopilot_portal_return_step(&e, &a, &step) && !step.inside);
	assert(step.target_x == 24.5F && step.target_z == 24.5F
		&& step.heading_deg == -90);
	autopilot_script_portal_walk(true, &a);
	autopilot_tick();
	bool forward = false;
	assert(autopilot_force_button(IB_FORWARD, &forward) && forward);
	assert(fabsf(gstate.autopilot.yaw + (float)GLM_PI * 0.5F) < 0.01F);
	assert(autopilot_force_button(IB_JUMP, &forward) && !forward);
	return_block(26, 71, 24, BLOCK_STONE);
	assert(!autopilot_portal_return_step(&e, &a, &step));
	autopilot_tick();
	assert(autopilot_force_button(IB_FORWARD, &forward) && !forward);
	return_block(26, 71, 24, BLOCK_AIR);
	return_block(24, 72, 24, BLOCK_AIR);
	assert(!autopilot_portal_return_step(&e, &a, &step));
	return_block(24, 72, 24, BLOCK_PORTAL);
	assert(autopilot_portal_return_step(&e, &a, &step));
	autopilot_tick();
	assert(autopilot_force_button(IB_FORWARD, &forward) && forward);
	e.pos[0] = 24.5F;
	autopilot_tick();
	assert(autopilot_force_button(IB_FORWARD, &forward) && !forward);
	autopilot_script_portal_patrol(&a, 1);
	autopilot_tick();
	assert(autopilot_force_button(IB_FORWARD, &forward) && forward);
	assert(fabsf(gstate.autopilot.yaw) < 0.01F);
	e.pos[2] = 25.5F;
	autopilot_tick();
	assert(autopilot_force_button(IB_FORWARD, &forward) && !forward);
	autopilot_script_portal_patrol(&a, 0);
	autopilot_tick();
	assert(autopilot_force_button(IB_FORWARD, &forward) && forward);
	assert(fabsf(fabsf(gstate.autopilot.yaw) - GLM_PI) < 0.01F);
	return_block(24, 72, 24, BLOCK_STONE);
	autopilot_tick();
	assert(autopilot_force_button(IB_FORWARD, &forward) && !forward);
	return_block(24, 72, 24, BLOCK_PORTAL);
	autopilot_script_portal_walk(false, NULL);
	struct world other;
	world_create(&other);
	e.world = &other;
	assert(!autopilot_portal_return_step(&e, &a, &step));
	world_destroy(&other);
	flat_world_destroy();
	puts("test_autopilot_route_rank: rearmed owning-world portal route and refusals");
}

/* R11 dims needs margin over 320 admitted dm. Exercise the 61-leg patrol with
 * real owned walking physics, including heading changes and inertial coast;
 * the main-driver/arbitration regression lives in test_r7_profile_routes.py. */
static void test_portal_patrol_physics(void) {
	struct entity e;
	flat_world();
	body_at(&e, 24, 24);
	for(int z = 23; z <= 26; z++)
		for(int y = 71; y <= 73; y++)
			return_block(24, y, z,
				z == 23 || z == 26 || y == 73 ? BLOCK_OBSIDIAN : BLOCK_PORTAL);
	struct autopilot_portal_return_anchor a;
	assert(autopilot_portal_return_anchor(&e, &a));
	unsigned endpoint = 0, legs = 0, ticks = 0;
	float moved = 0;
	unsigned moved_dm = 0;
	autopilot_script_portal_patrol(&a, endpoint);
	for(; ticks < 1200 && legs < 61; ticks++) {
		autopilot_tick();
		struct input_player_frame frame = {.connected = true};
		assert(autopilot_force_button(IB_FORWARD, &frame.down[IB_FORWARD]));
		assert(autopilot_force_button(IB_SNEAK, &frame.down[IB_SNEAK]));
		float x = e.pos[0], z = e.pos[2];
		(void)entity_local_player_walk_owned(&e, &gstate.players[0], &frame);
		moved += hypotf(e.pos[0] - x, e.pos[2] - z);
		/* Match residency's per-admitted-sample rounding, not just distance. */
		moved_dm += (unsigned)llround(hypot((double)e.pos[0] - x,
			(double)e.pos[2] - z) * 10.0);
		float dx = e.pos[0] - (a.x[endpoint] + 0.5F);
		float dz = e.pos[2] - (a.z[endpoint] + 0.5F);
		if(dx * dx + dz * dz < 0.01F) { legs++; endpoint ^= 1; }
		autopilot_script_portal_patrol(&a, endpoint);
		camera_attach(&gstate.players[0].camera, &e, 1.0F, 0.05F);
	}
	printf("R11 real patrol: %u legs, %.2f blocks, %u dm, %u/1200 ticks\n",
		legs, moved, moved_dm, ticks);
	assert(legs == 61 && moved >= 48.0F && moved_dm >= 400 && ticks < 1200);
	autopilot_script_portal_walk(false, NULL);
	flat_world_destroy();
}

static void test_portal_return_transverse_axis_and_unknown_corridor(void) {
	struct entity e;
	flat_world();
	body_at(&e, 24, 24);
	for(int x = 23; x <= 26; x++)
		for(int y = 71; y <= 73; y++)
			return_block(x, y, 24,
				x == 23 || x == 26 || y == 73
					? BLOCK_OBSIDIAN : BLOCK_PORTAL);
	struct autopilot_portal_return_anchor a;
	struct autopilot_portal_return_step step;
	assert(autopilot_portal_return_anchor(&e, &a));
	assert(a.x[0] == 24 && a.x[1] == 25
		&& a.z[0] == 24 && a.z[1] == 24);
	e.pos[2] = 27.2F;
	assert(autopilot_portal_return_step(&e, &a, &step)
		&& !step.inside && abs(step.heading_deg) == 180);
	flat_world_destroy();

	flat_world();
	body_at(&e, 30, 24);
	for(int z = 23; z <= 26; z++)
		for(int y = 71; y <= 73; y++)
			return_block(30, y, z,
				z == 23 || z == 26 || y == 73
					? BLOCK_OBSIDIAN : BLOCK_PORTAL);
	assert(autopilot_portal_return_anchor(&e, &a));
	e.pos[0] = 32.2F; /* adjacent column not installed */
	assert(!autopilot_portal_return_step(&e, &a, &step));
	flat_world_destroy();
	puts("test_autopilot_route_rank: transverse pane and unknown terrain refused");
}

int main(void) {
	/* Unbuffered, like test_autopilot_death_recovery.c: assert() aborts, and an
	   abort does not flush stdio -- without this the RED diagnostics printed
	   just before a failing assertion are lost and all a reader gets is the
	   file:line. */
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();

	test_config_walker_follows_table_order();
	test_released_seam_leaves_config_walker_alone();
	test_seam_first_pick_is_the_heading();
	test_seam_diagonal_heading();

	puts("test_autopilot_route_rank: all assertions passed");
	test_seam_refuses_a_one_way_drop();
	test_probe_uses_body_world();
	test_portal_geometry_reads_owner_and_blocked_corridor();
	test_portal_return_only_on_safe_rearmed_route();
	test_portal_return_transverse_axis_and_unknown_corridor();
	test_portal_patrol_physics();
	return 0;
}
