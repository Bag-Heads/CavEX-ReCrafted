/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	Regression test for the mp_lan_walk_10m spawn-pocket "walker immobility"
	root cause (task_c75a3ed4, PC diagnosis 2026-08-31): a scripted autopilot
	walk can die exactly like a human player (fall/drown/mob damage), and
	Beta's GuiGameOver (screen_death.c) clears local_player.capture_input --
	so once dead, autopilot_force_button's forced FORWARD/JUMP can never
	reach entity_local_player_tick's movement code again. The two real-Wii
	mp_lan_walk_10m runs that motivated this test both pinned ~4 blocks from
	the mc173 spawn for 9+ minutes because of exactly this: a fall shortly
	after MP connect killed the player and nothing ever pressed Respawn.

	This test proves the fix's pure state-machine piece (autopilot.c's
	death_screen_ticks tracking + the release-then-hold IB_GUI_CLICK forcing)
	in isolation, with a minimal zeroed gstate (test_options_env.c's
	test_pause_edges precedent) -- no gfx/world/network bootstrap needed
	since autopilot_tick/autopilot_force_button touch only gstate.autopilot,
	gstate.players[0].current_screen, and (dead code with unstick disarmed)
	gstate.players[0].local_player.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/autopilot.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "block/blocks.h"
#include "support/anchor_seed42_pocket.h"
#include "support/anchor_route_ledges.h"

static void reset_gstate(void) {
	memset(&gstate, 0, sizeof(gstate));
}

/* death_screen_ticks climbs by exactly 1 every autopilot_tick() call while
 * screen_death is current, and snaps back to 0 the instant it is not --
 * including the very first tick after the death screen closes (no lingering
 * "still counting down" state that could re-arm a stale hold). */
static void test_death_screen_ticks_tracks_screen(void) {
	reset_gstate();
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 0; /* walk */

	gstate.players[0].current_screen = &screen_ingame;
	autopilot_tick();
	assert(gstate.autopilot.death_screen_ticks == 0);
	autopilot_tick();
	assert(gstate.autopilot.death_screen_ticks == 0);

	gstate.players[0].current_screen = &screen_death;
	autopilot_tick();
	assert(gstate.autopilot.death_screen_ticks == 1);
	autopilot_tick();
	assert(gstate.autopilot.death_screen_ticks == 2);
	autopilot_tick();
	assert(gstate.autopilot.death_screen_ticks == 3);

	/* Respawn lands: the screen swaps back to ingame the very next tick
	 * (screen_death_activate -> CRPC_BEGIN_WORLD_SWITCH / the health>0
	 * backstop) -- the counter must not carry over into a later death. */
	gstate.players[0].current_screen = &screen_ingame;
	autopilot_tick();
	assert(gstate.autopilot.death_screen_ticks == 0);

	puts("  autopilot: death_screen_ticks tracks current_screen 1/1");
}

/* IB_GUI_CLICK forcing: unclaimed off the death screen; RELEASED (false) for
 * exactly one tick at tick 1 (arms screen_death.c's fresh-press latch so an
 * already-held click from mid-combat can't insta-confirm) AND at every multiple
 * of AUTOPILOT_DEATH_CLICK_RETRY_TICKS after it (each release produces a fresh
 * press edge on the next tick -- the one at tick 1+P is the first to land after
 * screen_death's entry grace, which swallows the tick-2 edge); HELD (true) on
 * every other tick, until the screen closes, at which point the button is no
 * longer autopilot-owned at all (falls through to real/unheld input). */
static void test_gui_click_release_then_hold(void) {
	reset_gstate();
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 0;

	const int P = AUTOPILOT_DEATH_CLICK_RETRY_TICKS;
	bool out;
	gstate.players[0].current_screen = &screen_ingame;
	assert(!autopilot_force_button(IB_GUI_CLICK, &out));

	gstate.players[0].current_screen = &screen_death;

	/* A frame before the first autopilot tick on the screen: released, so the
	 * latch arms even then. */
	gstate.autopilot.death_screen_ticks = 0;
	assert(autopilot_force_button(IB_GUI_CLICK, &out));
	assert(out == false);

	/* The three release ticks of the first two retry periods, and the held
	 * tick right after each of them (the one that carries the press edge). */
	for(int k = 0; k < 3; k++) {
		gstate.autopilot.death_screen_ticks = 1 + k * P;
		assert(autopilot_force_button(IB_GUI_CLICK, &out));
		assert(out == false);

		gstate.autopilot.death_screen_ticks = 2 + k * P;
		assert(autopilot_force_button(IB_GUI_CLICK, &out));
		assert(out == true);
	}

	/* Every other tick across those periods is HELD -- the hold each edge
	 * seeds runs P-1 ticks, uninterrupted, which is what lets it reach the
	 * screen's 750 ms requirement before the next release. */
	for(int t = 1; t <= 1 + 3 * P; t++) {
		gstate.autopilot.death_screen_ticks = t;
		assert(autopilot_force_button(IB_GUI_CLICK, &out));
		assert(out == (((t - 1) % P) != 0));
	}

	/* Screen closes (respawned) -> unclaimed again, regardless of the stale
	 * counter value until the next autopilot_tick() zeroes it. */
	gstate.autopilot.death_screen_ticks = 50;
	gstate.players[0].current_screen = &screen_ingame;
	assert(!autopilot_force_button(IB_GUI_CLICK, &out));

	/* The period must outrun the grace + hold it has to clear (autopilot.h's
	 * derivation): 400 ms grace + 750 ms default hold + 250 ms margin at
	 * 50 ms/tick. Pinned here so a future retune cannot silently go below it. */
	assert(P * 50 > 400 + 750 + 250);
	assert((P - 1) * 50 > 750); /* the seeded hold outlasts what the screen needs */

	puts("  autopilot: IB_GUI_CLICK release/re-press retry on death screen 1/1");
}

/* Regression guard: the existing FORWARD/JUMP forcing (far_walk_6m's proven
 * hardware-evidence trajectory) is untouched by the death-screen branch --
 * same t>20 / t%12==0 pulse contract, and identical whether or not the death
 * screen happens to be current (the new branch only claims IB_GUI_CLICK). */
static void test_forward_jump_contract_unchanged(void) {
	reset_gstate();
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 0;

	bool fwd, jmp;
	gstate.autopilot.brake = true; /* inactive route state cannot affect legacy input */
	for(int t = 0; t <= 40; t++) {
		gstate.autopilot.tick = t;

		gstate.players[0].current_screen = &screen_ingame;
		assert(autopilot_force_button(IB_FORWARD, &fwd));
		assert(fwd == (t > 20));
		assert(autopilot_force_button(IB_JUMP, &jmp));
		assert(jmp == ((t > 20) && (t % 12 == 0)));

		/* Same tick, only the screen differs -> identical FORWARD/JUMP
		 * results (the death branch claims IB_GUI_CLICK only). */
		gstate.players[0].current_screen = &screen_death;
		bool fwd2, jmp2;
		assert(autopilot_force_button(IB_FORWARD, &fwd2));
		assert(autopilot_force_button(IB_JUMP, &jmp2));
		assert(fwd2 == fwd);
		assert(jmp2 == jmp);
	}
	puts("  autopilot: FORWARD/JUMP contract unchanged by death branch 1/1");
}

/* Aim mode (debug.autopilot=aim) forces NOTHING, by contract -- including on
 * the death screen. The early return in autopilot_force_button must still
 * win over the new death branch. */
static void test_aim_mode_forces_nothing_on_death_screen(void) {
	reset_gstate();
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 1; /* aim */
	gstate.players[0].current_screen = &screen_death;
	gstate.autopilot.death_screen_ticks = 5;

	bool out;
	assert(!autopilot_force_button(IB_GUI_CLICK, &out));
	assert(!autopilot_force_button(IB_FORWARD, &out));
	assert(!autopilot_force_button(IB_JUMP, &out));

	puts("  autopilot: aim mode forces nothing on death screen 1/1");
}

/* NO AUTOMATIC RESPAWN UNDER THE SCRIPT SEAM (push-0921/B9; Astra gate 1 Lane
   B item 5, docs/mp_play_profile.md section 3: "No automatic respawn is armed
   for this profile"). The walk assertion makes the autopilot ACTIVE in mode 2,
   which is NOT the aim mode's early return above -- so the death-screen Respawn
   script was live underneath a scripted driver whose contract says a death ENDS
   the run. The gate must hold for EVERY death_screen_ticks value (a run that
   died at any tick must not be rescued) and must leave the movement forcing the
   seam exists for completely alone. */
static void test_script_seam_never_respawns(void) {
	reset_gstate();
	const int P = AUTOPILOT_DEATH_CLICK_RETRY_TICKS;
	bool out, fwd, jmp;

	/* The BASELINE, on the very same zeroed state: a config/profile consumer
	   (nothing calls the seam) still gets the Respawn script. Asserted first so
	   the "never forced" below cannot pass by accident on a broken fixture. */
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 0;
	gstate.players[0].current_screen = &screen_death;
	gstate.autopilot.death_screen_ticks = 2;
	assert(autopilot_force_button(IB_GUI_CLICK, &out) && out == true);

	/* Seam asserted (walk): the driver owns the walker, the autopilot is active
	   in mode 2 -- and the death click is UNCLAIMED at every tick of three full
	   retry periods, including the release ticks and tick 0. */
	autopilot_script_walk(true, 0);
	assert(gstate.autopilot.active && gstate.autopilot.mode == 2);
	for(int t = 0; t <= 1 + 3 * P; t++) {
		gstate.autopilot.death_screen_ticks = t;
		assert(!autopilot_force_button(IB_GUI_CLICK, &out));
	}

	/* ... and the walk itself is untouched: FORWARD / JUMP are still claimed on
	   the death screen exactly as they are off it, so the gate cannot be mistaken
	   for "the seam stops forcing everything". */
	gstate.autopilot.tick = 21;
	gstate.autopilot.brake = false;
	gstate.autopilot.terrain_safe = false;
	assert(autopilot_force_button(IB_FORWARD, &fwd) && fwd == true);
	assert(autopilot_force_button(IB_JUMP, &jmp));

	/* The AIM half of the seam, asserted ALONE (the walk released first, or the
	   walk branch would be what answers). Mode 1's early return already forces
	   nothing, so this is belt-and-braces -- pinned so a future reorder of that
	   return cannot reopen the hole through the other assertion. */
	autopilot_script_walk(false, 0);
	autopilot_script_aim(true, 0, 35);
	assert(gstate.autopilot.mode == 1);
	gstate.autopilot.death_screen_ticks = 2;
	assert(!autopilot_force_button(IB_GUI_CLICK, &out));
	autopilot_script_aim(false, 0, 0);

	/* RELEASED: the seam hands the pre-seam state back, and with it the Respawn
	   script. This is what keeps every existing consumer byte-identical -- the
	   gate is a property of the seam being held, not a permanent removal. */
	assert(gstate.autopilot.mode == 0 && gstate.autopilot.active);
	gstate.autopilot.death_screen_ticks = 2;
	assert(autopilot_force_button(IB_GUI_CLICK, &out) && out == true);
	gstate.autopilot.death_screen_ticks = 1;
	assert(autopilot_force_button(IB_GUI_CLICK, &out) && out == false);

	puts("  autopilot: script seam never forces the death-screen respawn 1/1");
}

static void route_world(bool pocket, bool lake) {
	reset_gstate();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	static uint8_t ids[32768], nibble[16384], heights[256];
	memset(ids, 0, sizeof(ids));
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y <= (lake ? 62 : 70); y++)
				ids[x * 2048 + z * 128 + y]
					= lake && y > 54 ? BLOCK_WATER_STILL : BLOCK_STONE;
	if(pocket)
		for(int y = 0; y < 7; y++)
			for(int z = 0; z < 7; z++)
				for(int x = 0; x < 8; x++)
					ids[(x + 5) * 2048 + (z + 3) * 128 + y + 70]
						= seed42_pocket[y][z][x];
	assert(world_build_full_section(&gstate.world, 16, 16, ids, nibble,
		nibble, nibble, heights, heights, heights));
	gstate.players[0].current_screen = &screen_ingame;
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 2;
	gstate.autopilot.tick = 21;
	autopilot_keepup_reset(&gstate.autopilot.keepup, true);
}

static void route_world_destroy(void) {
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);
}

static void route_body(struct entity* e, float x, float y, float z, bool safe) {
	memset(e, 0, sizeof(*e));
	entity_local_player(1, e, &gstate.world);
	e->pos[0] = x; e->pos[1] = y; e->pos[2] = z;
	e->data.local_player.capture_input = true;
	e->on_ground = true;
	gstate.players[0].local_player = e;
	dict_entity_set_at(gstate.entities, e->id, e);
	gstate.autopilot.terrain_safe = safe;
	memset(&gstate.autopilot.route, 0, sizeof(gstate.autopilot.route));
}

static uint32_t route_physics_tick(void) {
	autopilot_tick();
	uint32_t buttons = 0;
	const enum input_button keys[] = {IB_FORWARD, IB_JUMP, IB_SNEAK};
	for(unsigned i = 0; i < 3; i++)
		buttons |= (uint32_t)input_held(keys[i]) << i;
	/* Exercise the production dispatch and input_held path. The fixture never
	 * assigns a heading or movement frame on behalf of the route. */
	entities_client_tick(gstate.entities);
	return buttons;
}

static void route_tick(struct entity* e) {
	route_physics_tick();
	camera_attach(&gstate.players[0].camera, e, 1.0F, 0.05F);
}

static void test_route_diagonal_hazards(void) {
	const uint8_t hazards[] = {BLOCK_LAVA_STILL, BLOCK_LAVA_FLOW, BLOCK_FIRE};
	for(unsigned k = 0; k < sizeof(hazards); k++) {
		for(int y = 70; y <= 72; y++) {
			route_world(false, false);
			struct entity e;
			route_body(&e, 24.5F, 72.62F, 24.5F, true);
			world_set_block(&gstate.world, 24, y, 25,
				(struct block_data){.type = hazards[k]}, false);
			autopilot_tick();
			/* Both diagonal destinations are clear, but their swept bodies
			 * cross the cardinal hazard. A safe sideways exit exists. */
			assert(gstate.autopilot.route.has_target);
			assert(gstate.autopilot.route.z <= 24.5F);
			assert(gstate.autopilot.route.x != 24.5F);
			route_world_destroy();
		}
		/* The body clips this corner only before t=0.20 of the diagonal
		 * segment; checks at t=0, .25, .5, .75, 1 all miss it. */
		route_world(false, false);
		struct entity e;
		route_body(&e, 24.99F, 72.62F, 24.69F, true);
		world_set_block(&gstate.world, 25, 71, 25,
			(struct block_data){.type = hazards[k]}, false);
		autopilot_tick();
		assert(gstate.autopilot.route.has_target);
		assert(gstate.autopilot.route.z <= 24.5F);
		route_world_destroy();
	}
	puts("PASS diagonal lava/fire: support, feet, head and between-probe corners");
}

struct route_trace {
	vec3 pos, vel;
	vec2 orient;
	float target[3];
	uint32_t buttons, on_ground, has_target;
};

static void test_route_catchup_equiv(void) {
	static struct route_trace traces[2][600];
	const unsigned batched[] = {3, 1, 2, 0, 3, 2};
	for(unsigned schedule = 0; schedule < 2; schedule++) {
		route_world(true, false);
		struct entity e;
		route_body(&e, 24.3F, 72.62F, 22.7F, true);
		for(unsigned frame = 0, tick = 0; tick < 600; frame++) {
			unsigned count = schedule ? batched[frame % 6] : 1;
			for(unsigned n = 0; n < count && tick < 600; n++, tick++) {
				struct route_trace* out = &traces[schedule][tick];
				out->buttons = route_physics_tick();
				glm_vec3_copy(e.pos, out->pos);
				glm_vec3_copy(e.vel, out->vel);
				glm_vec2_copy(e.orient, out->orient);
				out->target[0] = gstate.autopilot.route.x;
				out->target[1] = gstate.autopilot.route.y;
				out->target[2] = gstate.autopilot.route.z;
				out->on_ground = e.on_ground;
				out->has_target = gstate.autopilot.route.has_target;
			}
			/* Production camera attachment occurs once per rendered frame,
			 * including frames with no physics tick. It must not steer ticks. */
			camera_attach(&gstate.players[0].camera, &e, 0.5F,
				count ? count * 0.05F : 0.01F);
		}
		assert(fabsf(e.pos[0] - 24.3F) + fabsf(e.pos[2] - 22.7F) > 3.0F);
		route_world_destroy();
	}
	for(unsigned t = 0; t < 600; t++) {
		if(memcmp(&traces[0][t], &traces[1][t], sizeof(traces[0][t]))) {
			fprintf(stderr, "route trace diverged at physics tick %u\n", t);
			assert(!"route depends on rendered-frame catch-up schedule");
		}
	}
	puts("PASS 600 route ticks byte-identical: one tick/frame vs 3,1,2,0,3,2 catch-up");
}

static void test_seed42_tree_pocket(void) {
	for(int safe = 0; safe < 2; safe++) {
		route_world(true, false);
		struct entity e;
		route_body(&e, 24.3F, 72.62F, 22.7F, safe);
		int escaped = 0;
		for(int t = 0; t < 1200; t++) {
			route_tick(&e);
			if(e.pos[0] < 23.0F && e.pos[2] < 21.7F) {
				escaped = t + 1;
				break;
			}
		}
		printf("  seed42 tree pocket: safe=%d exit_tick=%d pos=%.2f,%.2f,%.2f\n",
			safe, escaped, e.pos[0], e.pos[1], e.pos[2]);
		assert(safe ? escaped > 0 && escaped < 400 : escaped == 0);
		route_world_destroy();
	}
}

/* A valid one-block lower waypoint is already horizontally close while the
 * body still touches the old ledge. The walker must cross that final edge
 * before waiting for gravity to settle it on the verified lower floor. */
static void test_script_lower_landing_edge(void) {
	route_world(false, false); /* stone through y=70 */
	world_set_block(&gstate.world, 24, 71, 24,
		(struct block_data){.type = BLOCK_LEAVES}, false);
	struct entity e;
	route_body(&e, 25.289F, 73.62F, 24.5F, true); /* eye; feet=72 */
	/* Previously selected from the high cell; the player center crossed into
	 * the lower cell but its left edge still rests on the high leaf. */
	gstate.autopilot.route.has_target = true;
	gstate.autopilot.route.x = 25.5F;
	gstate.autopilot.route.y = 71.0F;
	gstate.autopilot.route.z = 24.5F;
	assert(world_get_block(&gstate.world, 25, 70, 24).type == BLOCK_STONE);
	assert(world_get_block(&gstate.world, 25, 71, 24).type == 0);
	/* The ordinary config walker retains its frozen near-lower-waypoint
	 * braking contract. This same body/target is advanced only when a live
	 * scripted walk owns the route below. */
	autopilot_tick();
	assert(gstate.autopilot.brake && !input_held(IB_FORWARD));
	assert(input_held(IB_SNEAK));
	autopilot_script_walk(true, 90); /* +X toward dry, supported y=71 */
	/* In the air above that same lower landing, wait for gravity rather than
	 * running forward across it. No physics step is needed for this assertion. */
	e.on_ground = false;
	autopilot_tick();
	assert(gstate.autopilot.brake && !input_held(IB_FORWARD));
	e.on_ground = true;
	float start_x = e.pos[0];
	uint32_t first = route_physics_tick();
	assert(!gstate.autopilot.brake && (first & 1u));
	for(int t = 1; t < 40; t++)
		route_physics_tick();
	printf("  lower landing edge: x=%.3f feet=%.3f target=%.2f,%.2f,%.2f brake=%d\n",
		e.pos[0], e.pos[1] - 1.62F, gstate.autopilot.route.x,
		gstate.autopilot.route.y, gstate.autopilot.route.z,
		gstate.autopilot.brake);
	assert(e.pos[0] - start_x > 1.0F);
	assert(e.on_ground);
	assert(e.pos[1] - 1.62F < 71.3F);
	autopilot_script_walk(false, 0);
	route_world_destroy();
}

static void test_route_water_and_cliff(void) {
	route_world(false, true);
	struct entity e;
	route_body(&e, 24.5F, 58.62F, 24.5F, true);
	/* Stay above the same lake bottom: ordinary jump input must surface the
	 * body before its normal air reserve expires, no coordinate correction. */
	for(int t = 0; t < 160 && e.pos[1] < 63.0F; t++)
		route_tick(&e);
	assert(e.pos[1] >= 63.0F);
	route_world_destroy();
	/* A submerged shaft has no horizontal waypoint. Braking must not cancel
	 * the independent swim input and drain the player's oxygen in place. */
	route_world(false, true);
	for(int x = 23; x <= 25; x++)
		for(int z = 23; z <= 25; z++)
			if(x != 24 || z != 24)
				for(int y = 55; y <= 62; y++)
					world_set_block(&gstate.world, x, y, z,
						(struct block_data){.type = BLOCK_STONE}, false);
	route_body(&e, 24.5F, 58.62F, 24.5F, true);
	for(int t = 0; t < 160 && e.pos[1] < 63.0F; t++)
		route_tick(&e);
	assert(e.pos[1] >= 63.0F);
	route_world_destroy();
	route_world(false, false);
	for(int x = 16; x < 32; x++)
		for(int z = 27; z < 32; z++)
			for(int y = 65; y <= 70; y++)
				world_set_block(&gstate.world, x, y, z,
					(struct block_data){.type = BLOCK_AIR}, false);
	route_body(&e, 24.5F, 72.62F, 25.5F, true);
	for(int t = 0; t < 200; t++) {
		route_tick(&e);
		assert(e.pos[1] >= 72.6F);
	}
	/* Unknown columns use stone as a collision fallback. They must still
	 * brake the route, rather than masquerading as installed support. */
	route_body(&e, 24.5F, 72.62F, 31.0F, true);
	autopilot_keepup_reset(&gstate.autopilot.keepup, true);
	autopilot_tick();
	assert(!gstate.autopilot.route.has_target || gstate.autopilot.route.z < 32.0F);
	route_world_destroy();
	puts("PASS anchor route swims with jump, brakes cliffs and unknown terrain");
}

/* ---- the seam keeps the scripted walker OUT OF THE WATER (push-0921/B11) --
 *
 * autopilot_route_landing accepts a column whose footing cell is WATER (the
 * `|| !autopilot_water(...)` arm): for the config walkers that is deliberate --
 * a far_walk_6m run that refused to swim would pin itself on the first shore.
 * For a SCRIPTED driver it is the opposite of what is wanted. Run
 * pc_mp_play_20260921_144523 walked into a lake and stayed there: cycles 5-9
 * all skipped "in water" (block 9 under the feet) for 250 s, and the leg that
 * finally ran died on "crosshair never on the dig cell" because a floating body
 * cannot hold a crosshair on the cell beneath it. The run before it dug FROM
 * water and the server refused the break outright.
 *
 * So while -- and only while -- the seam asserts the walk, a column is
 * unwalkable if its FOOTING or either BODY cell is a fluid. A dry candidate is
 * preferred; if every candidate is wet the probe takes no target, which brakes,
 * and the core's own no-progress rule then turns the heading 90 degrees. */
static void wet_ring(int cy) {
	for(int dx = -1; dx <= 1; dx++)
		for(int dz = -1; dz <= 1; dz++)
			if(dx || dz)
				world_set_block(&gstate.world, 24 + dx, cy, 24 + dz,
								(struct block_data) {.type = BLOCK_WATER_STILL},
								false);
}

static void test_script_seam_avoids_water(void) {
	struct entity e;

	/* (a) EVERY neighbour wet -> no target, and the probe brakes. The body's
	 * own column stays dry stone, so this is purely about where it may GO. */
	route_world(false, false);
	route_body(&e, 24.5F, 72.62F, 24.5F, true);
	wet_ring(70); /* the footing cell of all eight neighbours */
	autopilot_script_walk(true, 0);
	autopilot_tick();
	assert(!gstate.autopilot.route.has_target);
	assert(gstate.autopilot.brake);
	autopilot_script_walk(false, 0);
	route_world_destroy();

	/* (b) OFF THE SEAM, the identical world answers exactly as it always has:
	 * the water-footing arm accepts a candidate and the walker swims. This is
	 * the byte-identity half -- the rule is a property of the seam being
	 * asserted, not a change to the probe. */
	route_world(false, false);
	route_body(&e, 24.5F, 72.62F, 24.5F, true);
	wet_ring(70);
	autopilot_tick();
	assert(gstate.autopilot.route.has_target);
	route_world_destroy();

	/* (c) ONE dry neighbour among eight: the seam must PREFER it rather than
	 * brake, even though it is not the candidate the requested heading points
	 * at. Heading 0 is +Z; the dry cell is due -Z, the worst-aligned of the
	 * eight, so a probe that merely re-ranked candidates without rejecting the
	 * wet ones would still pick a wet cell here. */
	route_world(false, false);
	route_body(&e, 24.5F, 72.62F, 24.5F, true);
	wet_ring(70);
	world_set_block(&gstate.world, 24, 70, 23,
					(struct block_data) {.type = BLOCK_STONE}, false);
	autopilot_script_walk(true, 0);
	autopilot_tick();
	assert(gstate.autopilot.route.has_target);
	assert((int)floorf(gstate.autopilot.route.x) == 24);
	assert((int)floorf(gstate.autopilot.route.z) == 23);
	assert(!gstate.autopilot.brake);
	autopilot_script_walk(false, 0);
	route_world_destroy();

	/* (d) A column whose footing is solid but whose BODY cell is a fluid is
	 * wet too -- standing chest-deep is what produced the "in water" skips.
	 * Here the footing (y=70) is stone and the fluid sits at the feet cell
	 * (y=71), which the footing test alone would miss. */
	route_world(false, false);
	route_body(&e, 24.5F, 72.62F, 24.5F, true);
	wet_ring(71);
	autopilot_script_walk(true, 0);
	autopilot_tick();
	assert(!gstate.autopilot.route.has_target);
	assert(gstate.autopilot.brake);
	autopilot_script_walk(false, 0);
	route_world_destroy();

	/* (e) A BODY ALREADY IN THE WATER MUST STILL GET OUT. The rule above is
	 * about not ENTERING a fluid; applied to a body that is already swimming it
	 * would reject all eight neighbours, brake, and hold it there -- and
	 * braking mid-lake is worse than the behaviour being fixed, because turning
	 * the heading 90 degrees does not help when every heading is wet. A fall or
	 * a push can put the body in a lake whatever the probe prefers, so the
	 * escape has to exist. Same lake and same body as the config-path
	 * swim-out case above, with the seam asserted. */
	route_world(false, true); /* stone to y=54, water 55..62 */
	route_body(&e, 24.5F, 58.62F, 24.5F, true);
	autopilot_script_walk(true, 0);
	for(int t = 0; t < 160 && e.pos[1] < 63.0F; t++)
		route_tick(&e);
	assert(gstate.autopilot.water); /* it really was swimming */
	assert(e.pos[1] >= 63.0F);		/* and it really did surface */
	autopilot_script_walk(false, 0);
	route_world_destroy();

	/* (f) AND IT MUST GET OUT SIDEWAYS, which is the escape the runs actually
	 * need. (e) only proves the body rises: route_jump follows
	 * gstate.autopilot.water, so the vertical escape never depended on the
	 * probe. Horizontal movement DOES -- FORWARD is gated on !brake -- so a
	 * body dropped in a pond with every neighbour wet would bob in place
	 * forever. A 5x5 pond in the middle of dry land: the shore is three cells
	 * away, and all eight of the body's neighbours are water. */
	route_world(false, false); /* stone to y=70, dry land everywhere */
	for(int px = 22; px <= 26; px++)
		for(int pz = 22; pz <= 26; pz++)
			world_set_block(&gstate.world, px, 70, pz,
							(struct block_data) {.type = BLOCK_WATER_STILL},
							false);
	route_body(&e, 24.5F, 71.62F, 24.5F, true);
	autopilot_script_walk(true, 0);
	{
		bool escaped = false;
		for(int t = 0; t < 600 && !escaped; t++) {
			route_tick(&e);
			int bx = (int)floorf(e.pos[0]), bz = (int)floorf(e.pos[2]);
			escaped = (bx < 22 || bx > 26 || bz < 22 || bz > 26);
		}
		if(!escaped)
			printf("  -- pond trap: pos=%.2f,%.2f,%.2f water=%d brake=%d "
				   "has_target=%d\n",
				   (double)e.pos[0], (double)e.pos[1], (double)e.pos[2],
				   (int)gstate.autopilot.water, (int)gstate.autopilot.brake,
				   (int)gstate.autopilot.route.has_target);
		assert(escaped && "a scripted body dropped in a pond never got out");
	}
	autopilot_script_walk(false, 0);
	route_world_destroy();

	puts("PASS seam water avoidance: all-wet brakes, a dry candidate wins over "
		 "the heading, a flooded body cell counts as wet, a body already in the "
		 "water gets out both upwards and sideways, and the config path still "
		 "swims");
}

static void test_route_ledge_recovery(void) {
	for(unsigned f = 0; f < sizeof(route_ledge_fixtures)/sizeof(route_ledge_fixtures[0]); f++) {
		route_world(false, false);
		int x0 = route_ledge_fixtures[f].x, y0 = route_ledge_fixtures[f].y;
		int z0 = route_ledge_fixtures[f].z;
		static uint8_t ids[32768], nibble[16384], heights[256];
		for(int cx = WCOORD_CHUNK_OFFSET(x0); cx <= WCOORD_CHUNK_OFFSET(x0+8); cx++)
			for(int cz = WCOORD_CHUNK_OFFSET(z0); cz <= WCOORD_CHUNK_OFFSET(z0+8); cz++) {
				memset(ids, 0, sizeof(ids));
				for(int x = 0; x < 16; x++) for(int z = 0; z < 16; z++)
					for(int y = 0; y < y0; y++)
						ids[x*2048+z*128+y] = BLOCK_STONE;
				assert(world_build_full_section(&gstate.world, cx*16, cz*16,
					ids, nibble, nibble, nibble, heights, heights, heights));
			}
		for(int y = 0; y < 9; y++) for(int z = 0; z < 9; z++) for(int x = 0; x < 9; x++)
			world_set_block(&gstate.world, x0+x, y0+y, z0+z,
				(struct block_data){.type=route_ledge_fixtures[f].blocks[y][z][x]}, false);
		struct entity e;
		route_body(&e, route_ledge_fixtures[f].sx, route_ledge_fixtures[f].sy,
			route_ledge_fixtures[f].sz, true);
		float fall = 0;
		int escaped = 0;
		for(int t = 0; t < 400; t++) {
			float old_y = e.pos[1];
			route_tick(&e);
			if(e.pos[1] < old_y) fall += old_y - e.pos[1];
			if(e.on_ground) { assert(fall < 3.0F); fall = 0; }
			float dx = e.pos[0]-route_ledge_fixtures[f].sx;
			float dz = e.pos[2]-route_ledge_fixtures[f].sz;
			if(dx*dx+dz*dz > 9.0F) { escaped = t+1; break; }
		}
		printf("  observed ledge %u: safe exit_tick=%d\n", f, escaped);
		assert(escaped > 0);
		route_world_destroy();
	}
}

/* REL-11-seam: OPEN_INVENTORY / INVENTORY / GUI_QUICK_MOVE are script-owned
   presses. Unarmed they are NOT claimed (input resolves them normally); armed
   they follow the script level and, through the caller-owned edge latch input.c
   uses, read as exactly one rising edge per press. */
static void test_script_inventory_presses(void) {
	reset_gstate();
	const enum input_button btn[] = {IB_OPEN_INVENTORY, IB_INVENTORY, IB_GUI_QUICK_MOVE};
	bool out = true, prev[IB_COUNT] = {0}, levels[IB_COUNT] = {0};
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 0;
	for(int i = 0; i < 3; i++)
		assert(!autopilot_force_button(btn[i], &out));
	/* Arming with no level set claims the button LOW; unarmed with a level
	   array still claims nothing. */
	for(int i = 0; i < 3; i++) levels[btn[i]] = true;
	autopilot_script_buttons(false, levels);
	for(int i = 0; i < 3; i++)
		assert(!autopilot_force_button(btn[i], &out));
	for(int i = 0; i < 3; i++) {
		memset(levels, 0, sizeof(levels));
		autopilot_script_buttons(true, levels);
		assert(autopilot_force_button(btn[i], &out) && !out);
		levels[btn[i]] = true;
		autopilot_script_buttons(true, levels);
		int edges = 0;
		for(int frame = 0; frame < 3; frame++) {
			assert(autopilot_force_button(btn[i], &out) && out);
			if(autopilot_force_edge_pressed(prev, btn[i], out)) edges++;
			autopilot_force_edge_advance(prev, btn[i], out);
		}
		assert(edges == 1);
		autopilot_script_buttons(true, (bool[IB_COUNT]){0});
		assert(autopilot_force_button(btn[i], &out) && !out);
		autopilot_force_edge_advance(prev, btn[i], out);
	}
	autopilot_script_buttons(false, NULL);
	for(int i = 0; i < 3; i++)
		assert(!autopilot_force_button(btn[i], &out));
	puts("  autopilot: script inventory presses no-op unarmed, one edge armed 1/1");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_death_screen_ticks_tracks_screen();
	test_gui_click_release_then_hold();
	test_forward_jump_contract_unchanged();
	test_aim_mode_forces_nothing_on_death_screen();
	test_script_seam_never_respawns();
	test_script_inventory_presses();
	blocks_init();
	test_route_diagonal_hazards();
	test_route_catchup_equiv();
	test_seed42_tree_pocket();
	test_script_lower_landing_edge();
	test_route_water_and_cliff();
	test_script_seam_avoids_water();
	test_route_ledge_recovery();
	puts("test_autopilot_death_recovery: all tests passed");
	return 0;
}
