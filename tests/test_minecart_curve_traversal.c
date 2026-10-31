/*
	Rail CURVE traversal goldens for the minecart -- the 16-case sign table.

	WHY THIS EXISTS. Curve and slope traversal had ZERO test coverage: no test in
	the tree ever drove minecart_server_tick over a meta 2..9 tile. The only
	minecart tests are test_minecart_place (render pose + settle height, fixture
	pinned to a flat NS rail) and test_minecart_rail_beta (powered-rail sign
	semantics, metas 0 and 1 only). A regression that flipped a curve exit, or
	stalled a cart dead on a curve, would have shipped green.

	THE INVARIANT UNDER TEST. The cart stores a signed scalar `speed` along a unit
	heading (hx,hz); the TRAVEL direction is d = h * sgn(speed). The curve branch
	(entity_minecart.c:391-416) turns the travel direction through curve_turn and
	then stores the exit heading RE-MULTIPLIED by sgn:

	    e->data.minecart.hx = (int8_t)(out_dx * sgn);   // :413
	    e->data.minecart.hz = (int8_t)(out_dz * sgn);   // :414

	That `* sgn` looks redundant and is not. The NEXT tick recomputes
	cur_dx = hx * sgn, so with the sgn stored the round trip is
	out_dx * sgn * sgn == out_dx -- the exit travel direction is recovered
	exactly, for EITHER speed sign. Drop it and a negative-speed cart exits every
	curve BACKWARD. That is the same class as the double-applied sgn in
	seed_heading which 5fabbb5 fixed as the "minecarts only go one direction"
	report of 2026-07-31; this file is the guard that stops a future "cleanup" of
	:413-414 from reintroducing it.

	Note the asymmetry that makes the negative rows the load-bearing ones: with
	sgn = +1, `out_dx * sgn == out_dx`, so ALL EIGHT positive-speed cases pass
	even with the multiplication deleted. Only the eight negative-speed cases
	discriminate. Both halves are asserted anyway so the table is complete.

	CURVE META NAMING. curve_turn's table (entity_minecart.c:64-89) and
	BlockRail's calcRailMeta (block_rail.c:145-148, `if (s_ && e) return 6; //
	SE`) independently agree: 6=SE, 7=SW, 8=NW, 9=NE, where the letters name the
	two sides the curve CONNECTS. entity_minecart_rail_pose's comments
	(entity_minecart.c:481-484) label these reversed (6<->9, 7<->8); two code
	sites outrank one comment block, so this file takes curve_turn/block_rail as
	the naming authority. Whether the pose YAW VALUES are also swapped is a
	separate render question and is deliberately NOT asserted here.

	Also note curve_turn's own inline comments ("N -> E") name the DIRECTION OF
	TRAVEL, not the entry side: "N" there means travelling toward -Z, i.e. having
	entered from the SOUTH side. A test that names cases by entry side and does
	not invert them silently exercises curve_turn's no-turn default and passes on
	a broken table. This file therefore keys every case on the TRAVEL vector
	d_in, never on a compass "entry" word.

	NOT ASSERTED HERE, deliberately: per-tick curve POSITIONS beyond the single
	post-turn step. This engine models a curve as "run to the tile centre, then
	rotate instantaneously", whereas Beta drives a MATRIX path with a real turn
	radius. Pinning the intermediate positions would freeze a known approximation
	as if it were parity. What IS pinned is the exit HEADING (a discrete fact the
	engine and Beta must agree on) and that the post-turn step lies along the exit
	direction with Beta's pre-friction movement magnitude.

	Drives the real production path end to end (server_local_test_tick_entities ->
	minecart_server_tick), like test_minecart_place and test_minecart_rail_beta.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_minecart_curve_traversal_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 /* stone floor */
#define RY 101 /* rail plane  */

/* Beta moves from the pre-friction momentum copy, then decays stored motion.
 * [740c583 EntityMinecart.onUpdate:303-358] */

extern struct item item_minecart;

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {
									  .type = (uint8_t)type,
									  .metadata = (uint8_t)(meta & 0x0F),
								  }));
}

/* Force EXACT rail metadata. server_world_set_block runs the rail's
 * onBlockAdded/onNeighbourBlockChange, which RECOMPUTES the shape from
 * neighbours (block_rail.c calcRailMeta) -- a lone rail always normalises to NS
 * (0), so a plain set() can never build a CURVE fixture. Curve shapes 6..9 are
 * additionally gated behind `allow_curves = !rail_is_powerable(id)`
 * (block_rail.c:191-192), so a curve fixture must use PLAIN BLOCK_RAIL; a
 * powered or detector rail can never legally hold 6..9. The silent setter writes
 * the cell without the lifecycle, which is exactly what a fixture wants.
 *
 * IDEMPOTENT ON PURPOSE. server_world_write_block_raw returns NULL when the
 * write would not CHANGE the cell (`if(!sc || !changed) return NULL;`,
 * server_world.c:734), so the silent setter reports false for a no-op. This is
 * called once per table row and consecutive rows share a meta, so re-forcing an
 * already-correct cell must be a success, not an assertion. Skip the write when
 * the cell already matches, and assert the POST-CONDITION either way -- the
 * fixture guarantee is "the cell holds exactly this shape", not "a write
 * happened". */
static void force_rail(int x, int y, int z, int type, int meta) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	if(b.type != (uint8_t)type || (b.metadata & 0x0F) != (meta & 0x0F))
		assert(server_world_set_block_silent(
			&s.worlds[0], x, y, z,
			(struct block_data) {
				.type = (uint8_t)type,
				.metadata = (uint8_t)(meta & 0x0F),
			}));
	assert(server_world_get_block(&s.world, x, y, z, &b));
	assert(b.type == (uint8_t)type && (b.metadata & 0x0F) == (meta & 0x0F)
		   && "fixture rail must keep the EXACT shape metadata asked for");
}

static bool place_cart_on(int x, int y, int z) {
	struct block_data blk_on, blk_where;
	assert(server_world_get_block(&s.world, x, y, z, &blk_on));
	assert(server_world_get_block(&s.world, x, y + 1, z, &blk_where));
	struct block_info on = {
		.block = &blk_on, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct block_info where = {
		.block = &blk_where, .neighbours = NULL, .x = x, .y = y + 1, .z = z};
	struct item_data held = {.id = ITEM_MINECART, .durability = 0, .count = 1};
	struct item* it = item_get(&held);
	assert(it == &item_minecart && it->onItemPlace);
	return it->onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP);
}

static struct entity* only_minecart(void) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MINECART) {
			assert(!found);
			found = e;
		}
		dict_entity_next(it);
	}
	assert(found);
	return found;
}

static void clear_carts(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_MINECART) {
				uint32_t id = e->id;
				dict_entity_erase(s.entities, id);
				again = true;
				break;
			}
			dict_entity_next(it);
		}
	}
}

/* Place a cart on (x,RY,z), settle it with one tick, then arm it with an exact
 * signed speed and heading. Returns the live cart; the caller ticks it. */
static struct entity* arm_cart(int x, int z, float speed, int8_t hx, int8_t hz) {
	clear_carts();
	assert(place_cart_on(x, RY, z));
	assert(server_local_test_tick_entities(&s)); /* settle onto the rail */
	struct entity* e = only_minecart();
	/* The settle tick must leave the cart exactly on the tile centre -- the
	 * whole 16-case table assumes dist_to_center == 0 so the turn fires on the
	 * very first moving tick (entity_minecart.c:400, `> MC_EPS`). */
	assert(e->pos[0] == (float)x + 0.5f && e->pos[2] == (float)z + 0.5f);
	e->data.minecart.speed = speed;
	e->data.minecart.hx = hx;
	e->data.minecart.hz = hz;
	return e;
}

#define EPS 1e-5f
static void near_eq(float got, float want, const char* what) {
	if(fabsf(got - want) > EPS) {
		fprintf(stderr, "FAIL %s: got %.6f want %.6f\n", what, got, want);
		assert(0 && "value mismatch");
	}
}

/* ---------------------------------------------------------------------------
 * The table. Keyed on TRAVEL vectors, never on compass entry words.
 *
 * meta 6 connects South(+z) and East(+x); 7 South+West; 8 North+West;
 * 9 North+East.  [entity_minecart.c:64-67; block_rail.c:145-148]
 * ------------------------------------------------------------------------- */
struct curve_case {
	uint8_t meta;
	int in_dx, in_dz;   /* travel direction ENTERING the curve tile */
	int out_dx, out_dz; /* travel direction the curve must produce   */
	const char* name;
};

static const struct curve_case CURVES[] = {
	/* meta 6 (SE): arriving toward -Z turns to +X; arriving toward -X turns to +Z */
	{6, 0, -1, +1, 0, "meta6 SE: travel -Z -> +X"},
	{6, -1, 0, 0, +1, "meta6 SE: travel -X -> +Z"},
	/* meta 7 (SW): arriving toward -Z turns to -X; arriving toward +X turns to +Z */
	{7, 0, -1, -1, 0, "meta7 SW: travel -Z -> -X"},
	{7, +1, 0, 0, +1, "meta7 SW: travel +X -> +Z"},
	/* meta 8 (NW): arriving toward +Z turns to -X; arriving toward +X turns to -Z */
	{8, 0, +1, -1, 0, "meta8 NW: travel +Z -> -X"},
	{8, +1, 0, 0, -1, "meta8 NW: travel +X -> -Z"},
	/* meta 9 (NE): arriving toward +Z turns to +X; arriving toward -X turns to -Z */
	{9, 0, +1, +1, 0, "meta9 NE: travel +Z -> +X"},
	{9, -1, 0, 0, -1, "meta9 NE: travel -X -> -Z"},
};

/* Run one (case, speed-sign) row: arm the cart so it TRAVELS in_d, tick once,
 * and assert both the stored exit heading and the post-turn step. */
static void run_curve(const struct curve_case* c, float speed) {
	const int CX = 4, CZ = 4;
	force_rail(CX, RY, CZ, BLOCK_RAIL, c->meta);

	const int sgn = (speed >= 0.0f) ? +1 : -1;
	/* travel d = heading h * sgn  =>  the heading that produces d is h = d*sgn. */
	struct entity* e = arm_cart(CX, CZ, speed, (int8_t)(c->in_dx * sgn),
								(int8_t)(c->in_dz * sgn));

	const float x0 = e->pos[0], z0 = e->pos[2];
	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float step = fabsf(speed);
	char what[192];

	/* (a) the stored heading must be the EXIT direction carrying sgn, so that
	 *     next tick's cur_d = h * sgn recovers out_d exactly. This is the
	 *     assertion that fails if `* sgn` is dropped from :413-414. */
	snprintf(what, sizeof(what), "%s (speed %+.2f) exit heading hx", c->name,
			 (double)speed);
	near_eq((float)e->data.minecart.hx, (float)(c->out_dx * sgn), what);
	snprintf(what, sizeof(what), "%s (speed %+.2f) exit heading hz", c->name,
			 (double)speed);
	near_eq((float)e->data.minecart.hz, (float)(c->out_dz * sgn), what);

	/* (b) the heading must ROUND-TRIP to the exit travel direction. Stated
	 *     separately from (a) because this is the property that actually
	 *     matters; (a) only pins the representation that delivers it. */
	assert(e->data.minecart.hx * sgn == c->out_dx
		   && e->data.minecart.hz * sgn == c->out_dz
		   && "heading * sgn(speed) must recover the exit TRAVEL direction");

	/* (c) the cart must physically move ALONG the exit direction this tick,
	 *     by Beta's pre-friction step, with the cross axis locked to centre. */
	snprintf(what, sizeof(what), "%s (speed %+.2f) step X", c->name,
			 (double)speed);
	near_eq(e->pos[0] - x0, step * (float)c->out_dx, what);
	snprintf(what, sizeof(what), "%s (speed %+.2f) step Z", c->name,
			 (double)speed);
	near_eq(e->pos[2] - z0, step * (float)c->out_dz, what);

	/* (d) speed keeps its sign through a turn -- a curve is not a brake and
	 *     must never reverse the cart. */
	assert((e->data.minecart.speed > 0.0f) == (speed > 0.0f)
		   && "a curve must not flip the sign of speed");
}

/* A cart travelling ALONG a direction the curve does not connect must pass
 * straight through with its heading untouched (curve_turn's default, :69).
 * These are the cart's EXIT directions -- e.g. an SE curve does not turn a cart
 * already travelling +Z, it is leaving down the south leg. */
static void run_no_turn(uint8_t meta, int in_dx, int in_dz, float speed,
						const char* name) {
	const int CX = 4, CZ = 4;
	force_rail(CX, RY, CZ, BLOCK_RAIL, meta);

	const int sgn = (speed >= 0.0f) ? +1 : -1;
	struct entity* e = arm_cart(CX, CZ, speed, (int8_t)(in_dx * sgn),
								(int8_t)(in_dz * sgn));
	const float x0 = e->pos[0], z0 = e->pos[2];
	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float step = fabsf(speed);
	char what[192];
	snprintf(what, sizeof(what), "%s: no-turn keeps travel X", name);
	near_eq(e->pos[0] - x0, step * (float)in_dx, what);
	snprintf(what, sizeof(what), "%s: no-turn keeps travel Z", name);
	near_eq(e->pos[2] - z0, step * (float)in_dz, what);
	assert(e->data.minecart.hx * sgn == in_dx
		   && e->data.minecart.hz * sgn == in_dz
		   && "a non-connected travel direction must pass through unturned");
}

/* ---------------------------------------------------------------------------
 * Anti-stall integration: after a curve dumps a cart onto the next tile, the
 * heading AXIS must match that tile's axis.
 *
 * The straight branches lock a FIXED axis regardless of heading -- case 0 does
 * `pos[0] = cx; MOVE_ALONG_Z(...)` and case 1 does `pos[2] = cz;
 * MOVE_ALONG_X(...)` (entity_minecart.c:351-360). If a cart ever arrives on an
 * EW tile carrying a Z-only heading, cur_dx == 0 and it moves ZERO while
 * retaining nonzero speed -- a silent dead stall. seed_heading only re-seeds
 * when BOTH hx and hz are 0 (:337-339), so it cannot rescue that state: the
 * curve branch's turn is the ONLY thing keeping the heading axis aligned with
 * the track. Nothing else in the suite checks this.
 * ------------------------------------------------------------------------- */
static void test_curve_exit_feeds_straight(float speed) {
	const int CX = 4, CZ = 4;
	/* SE curve at (4,4); the +X leg continues as a flat EW rail at (5,4). */
	force_rail(CX, RY, CZ, BLOCK_RAIL, 6);
	force_rail(CX + 1, RY, CZ, BLOCK_RAIL, 1);

	const int sgn = (speed >= 0.0f) ? +1 : -1;
	/* travel -Z into the SE curve -> must exit travelling +X onto (5,4). */
	struct entity* e = arm_cart(CX, CZ, speed, (int8_t)(0 * sgn),
								(int8_t)(-1 * sgn));

	bool reached_next_tile = false;
	for(int t = 0; t < 64; t++) {
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		if(e->data.minecart.speed == 0.0f)
			break;
		if((int)floorf(e->pos[0]) == CX + 1) {
			reached_next_tile = true;
			break;
		}
	}
	assert(reached_next_tile
		   && "a cart leaving an SE curve on the +X leg must reach the EW tile "
			  "-- if it stalls, the exit heading axis is wrong");

	/* On the EW tile the heading must be X-aligned, or the straight branch
	 * would move the cart by zero forever while it still carries speed. */
	assert(e->data.minecart.hx != 0 && e->data.minecart.hz == 0
		   && "heading axis must match the EW tile it was fed onto");
	assert(e->data.minecart.speed != 0.0f);

	/* And it must keep going in +X, not stall or reverse. */
	const float x_before = e->pos[0];
	assert(server_local_test_tick_entities(&s));
	e = only_minecart();
	assert(e->pos[0] > x_before
		   && "cart must continue travelling +X off the curve's east leg");
}

/* The idle fast path now excludes powered rails so they can assign Beta's
 * wall-directed launch after movement/friction. Ordinary rails can be curves,
 * though, and must retain the fast path: a zero-displacement tick cannot rotate
 * their scalar heading as if the cart had traversed the curve. */
static void test_idle_curve_keeps_heading(void) {
	const int CX = 4, CZ = 4;
	force_rail(CX, RY, CZ, BLOCK_RAIL, 6);
	struct entity* e = arm_cart(CX, CZ, 0.0f, 0, -1);
	const float x0 = e->pos[0], z0 = e->pos[2];

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();
	assert(e->data.minecart.hx == 0 && e->data.minecart.hz == -1
		   && "an idle curve tick must not rotate the cart's internal heading");
	near_eq(e->pos[0], x0, "an idle curve cart must not move on X");
	near_eq(e->pos[2], z0, "an idle curve cart must not move on Z");
}

int main(void) {
	test_fixture_dir("test_minecart_curve_traversal_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); /* repeatable clean-slate fixture */
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;

	/* Complete Beta position +/-32 entity-activation neighborhood. */
	struct server_chunk* sc = NULL;
	for(int lcx = -2; lcx <= 2; lcx++)
		for(int lcz = -2; lcz <= 2; lcz++) {
			sc = NULL;
			assert(server_world_load_chunk(&s.world, lcx, lcz, &sc) && sc);
		}

	/* Stone under the whole rail footprint used below. */
	for(int x = 3; x <= 6; x++)
		for(int z = 3; z <= 6; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	/* ===================================================================
	 * (1) The 16-case curve table: 8 (meta, travel-in) pairs x 2 speed signs.
	 *     The 8 NEGATIVE rows are the ones that discriminate the `* sgn` at
	 *     entity_minecart.c:413-414; the positive rows pass without it.
	 * =================================================================== */
	const size_t n_curves = sizeof(CURVES) / sizeof(CURVES[0]);
	for(size_t i = 0; i < n_curves; i++) {
		run_curve(&CURVES[i], +0.10f);
		run_curve(&CURVES[i], -0.10f);
	}

	/* ===================================================================
	 * (2) Non-connected travel directions pass through unturned
	 *     (curve_turn's default arm, entity_minecart.c:69). These are the
	 *     reverse of each curve's two connected legs.
	 * =================================================================== */
	run_no_turn(6, 0, +1, +0.10f, "meta6 SE travelling +Z");
	run_no_turn(6, 0, +1, -0.10f, "meta6 SE travelling +Z (neg speed)");
	run_no_turn(7, -1, 0, +0.10f, "meta7 SW travelling -X");
	run_no_turn(8, 0, -1, +0.10f, "meta8 NW travelling -Z");
	run_no_turn(9, +1, 0, +0.10f, "meta9 NE travelling +X");

	/* ===================================================================
	 * (3) Anti-stall: a curve must hand the next tile a heading on that
	 *     tile's own axis, for BOTH speed signs.
	 * =================================================================== */
	test_curve_exit_feeds_straight(+0.10f);
	test_curve_exit_feeds_straight(-0.10f);
	test_idle_curve_keeps_heading();

	printf("test_minecart_curve_traversal: 16-case curve sign table, idle/no-turn "
		   "guards and curve->straight hand-off OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
