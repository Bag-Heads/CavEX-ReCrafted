/*
	Beta 1.7.3 SLOPE goldens for the minecart: track geometry first, then the
	gravity impulse.

	=== GROUND TRUTH ==========================================================

	Which side of a sloped rail is HIGH is fixed by the code that ASSIGNS the
	metadata, and CaveWii's own placement path already states it
	(block_rail.c:129-132, calcRailMeta):

	    if (rail at (x+1, y+1, z)) return 2;   // a higher rail EAST  -> ascends +X
	    if (rail at (x-1, y+1, z)) return 3;   // a higher rail WEST  -> ascends -X
	    if (rail at (x, y+1, z-1)) return 4;   // a higher rail NORTH -> ascends -Z
	    if (rail at (x, y+1, z+1)) return 5;   // a higher rail SOUTH -> ascends +Z

	A tile is given shape 2 precisely BECAUSE the track continues one block up to
	the east, so shape 2's HIGH side is EAST. Beta's own gravity block agrees
	independently -- gravity must push DOWNHILL
	[740c583 EntityMinecart.onUpdate:209-246]:

	    push = 0.0078125D;                      // 1/128, per tick
	    shape 2: motionX -= push;                   // pushes -X => downhill is WEST => HIGH = EAST
	    shape 3: motionX += push;                   // pushes +X => downhill is EAST => HIGH = WEST
	    shape 4: motionZ += push;                   // pushes +Z => downhill is SOUTH => HIGH = NORTH
	    shape 5: motionZ -= push;                   // pushes -Z => downhill is NORTH => HIGH = SOUTH

	Two independent sources, same answer:
	    shape 2 -> HIGH EAST (+X)      shape 3 -> HIGH WEST (-X)
	    shape 4 -> HIGH NORTH (-Z)     shape 5 -> HIGH SOUTH (+Z)

	=== WHAT THIS FILE PINS ===================================================

	(A) TRACK GEOMETRY. Moving toward a shape's HIGH side must RAISE the cart and
	    moving toward its LOW side must LOWER it. Consumed from
	    entity_minecart.c's slope_height().

	    entity_minecart.c's slope_height labels shapes 2 and 3 the other way
	    round ("case 2: up to W (-X)"), and entity_minecart_rail_pose repeats
	    those labels. Shapes 4 and 5 agree with calcRailMeta; only the EW pair
	    disagrees. This file takes calcRailMeta + the decompile as the authority
	    -- the placement code and Beta cannot both be wrong about which
	    neighbour made the tile a slope.

	    The fixture does NOT hand-write the metadata: it builds a real two-rail
	    ramp and lets the production calcRailMeta assign the shape, then asserts
	    which shape it got. So the test cannot silently agree with a wrong
	    consumer -- the geometry is established by world layout, not by a literal.

	(B) GRAVITY. Beta accelerates a cart downhill by 1/128 per tick on every
	    sloped shape. These goldens originally exposed CaveWii's missing impulse;
	    they now pin the production fix and its ordering with powered-rail
	    braking, movement, friction, height-energy exchange, and boost.

	    ENGINE MAPPING, stated so the expected values are not mistaken for
	    Beta's own: this port models rail motion as a SIGNED SCALAR `speed` along
	    a unit heading (hx,hz) rather than Beta's (motionX, motionZ) vector.
	    World motion m = speed * h, and h is +/-1, so a world-axis impulse dm
	    maps to d(speed) = dm * h. The MAGNITUDE 0.0078125 and the DOWNHILL
	    DIRECTION are Beta's and are what this file asserts; the scalar
	    projection is the port's existing model.

	Drives the real production path end to end (server_local_test_tick_entities
	-> minecart_server_tick), like test_minecart_place / test_minecart_rail_beta
	/ test_minecart_curve_traversal.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_minecart_slope_beta_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

/* Beta empty-cart friction, applied after movement.
 * [740c583 EntityMinecart.onUpdate:352-358] */
#define BETA_EMPTY_FRICTION 0.9599999785423279f
/* [740c583 EntityMinecart.onUpdate:211] var7 = 0.0078125D */
#define BETA_SLOPE_IMPULSE 0.0078125f
/* [740c583 EntityMinecart.onUpdate:396-401] */
#define BETA_POWERED_BOOST 0.06f

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

static void clear_cell(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	if(b.type != BLOCK_AIR)
		assert(server_world_set_block_silent(
			&s.worlds[0], x, y, z,
			(struct block_data) {.type = BLOCK_AIR, .metadata = 0}));
}

static uint8_t getmeta(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b.metadata & 0x0F;
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

/* Place a rail through the REAL player path (BlockRail.onItemPlace), which is
 * the only thing that computes a shape. Rails have NO onBlockAdded in this
 * engine -- calcRailMeta is reached exclusively from onItemPlace
 * (block_rail.c:191) and onNeighbourBlockChange (:359) -- so a plain
 * server_world_set_block writes the cell with metadata 0 and leaves it there.
 * (test_minecart_place.c:179 documents the same normalisation.) */
static bool place_rail(int x, int y, int z) {
	struct block_data blk_on, blk_where;
	assert(server_world_get_block(&s.world, x, y - 1, z, &blk_on));
	assert(server_world_get_block(&s.world, x, y, z, &blk_where));
	struct block_info on = {
		.block = &blk_on, .neighbours = NULL, .x = x, .y = y - 1, .z = z};
	struct block_info where = {
		.block = &blk_where, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct item_data held = {.id = BLOCK_RAIL, .durability = 0, .count = 1};
	struct item* it = item_get(&held);
	assert(it && it->onItemPlace);
	return it->onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP);
}

/* ---------------------------------------------------------------------------
 * A real two-rail ramp, built by the PRODUCTION placement path so the shape is
 * whatever the rail logic decides from world layout -- never a hand-written
 * literal. `hx,hz` is the unit direction from the low tile toward the high
 * neighbour. Returns the shape the low tile ended up with.
 *
 * Layout, for hx=+1 (ascending east) at low tile (lx,ly,lz):
 *     stone (lx,   ly-1, lz)    rail LOW  (lx,   ly,   lz)
 *     stone (lx+1, ly,   lz)    rail HIGH (lx+1, ly+1, lz)
 *
 * ORDER MATTERS, and it is the LOW rail that is placed first. The shape is
 * conferred by the HIGH rail's placement: onItemPlace probes for a rail one
 * block down-and-over and promotes it to the matching slope
 * (block_rail.c:216-227), e.g.
 *     if (lower_w) updateRailToSlope(s, where->x - 1, where->y - 1, where->z, 2);
 * -- a new rail with a lower rail to its WEST makes that lower rail shape 2.
 * Placing the high rail first instead would leave the low rail flat, because
 * the two tiles are DIAGONAL neighbours and no 6-adjacent neighbour update ever
 * fires between them.
 * ------------------------------------------------------------------------- */
static uint8_t build_ramp(int lx, int ly, int lz, int hx, int hz) {
	const int hxx = lx + hx, hzz = lz + hz;
	set(lx, ly - 1, lz, BLOCK_STONE, 0); /* support for the low rail  */
	set(hxx, ly, hzz, BLOCK_STONE, 0);   /* support for the high rail */
	assert(place_rail(lx, ly, lz));      /* LOW rail first            */
	assert(place_rail(hxx, ly + 1, hzz)); /* HIGH promotes it to a slope */
	return getmeta(lx, ly, lz);
}

static void tear_down_ramp(int lx, int ly, int lz, int hx, int hz) {
	const int hxx = lx + hx, hzz = lz + hz;
	clear_carts();
	clear_cell(lx, ly, lz);
	clear_cell(hxx, ly + 1, hzz);
	clear_cell(hxx, ly, hzz);
	clear_cell(lx, ly - 1, lz);
}

#define EPS 1e-5f
static void near_eq(float got, float want, const char* what) {
	if(fabsf(got - want) > EPS) {
		fprintf(stderr, "FAIL %s: got %.6f want %.6f\n", what, got, want);
		assert(0 && "value mismatch");
	}
}

/* ---------------------------------------------------------------------------
 * (A) TRACK GEOMETRY -- moving toward the HIGH side must RAISE the cart.
 *
 * `toward_high` selects which way the cart travels: +1 = toward the ramp's high
 * neighbour, -1 = away from it (downhill). The cart is armed to travel that way
 * and ticked once; the assertion is purely on the SIGN of the height change, so
 * it does not depend on the port's step magnitude at all.
 * ------------------------------------------------------------------------- */
static void assert_height_follows_track(int lx, int ly, int lz, int hx, int hz,
										int toward_high, const char* name) {
	clear_carts();
	assert(place_cart_on(lx, ly, lz));
	assert(server_local_test_tick_entities(&s)); /* settle onto the track */
	struct entity* e = only_minecart();

	const float y_before = e->pos[1];

	/* travel direction d = heading h * sgn(speed); use speed > 0 so h == d. */
	const int dx = hx * toward_high, dz = hz * toward_high;
	e->data.minecart.speed = +0.10f;
	e->data.minecart.hx = (int8_t)dx;
	e->data.minecart.hz = (int8_t)dz;

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();
	const float y_after = e->pos[1];

	/* The cart must actually have moved along the intended axis, or the height
	 * assertion below would be vacuous. */
	assert((dx != 0 ? fabsf(e->pos[0] - ((float)lx + 0.5f))
					: fabsf(e->pos[2] - ((float)lz + 0.5f)))
			   > 0.0f
		   && "cart must have moved along the slope axis");

	if(toward_high > 0) {
		if(!(y_after > y_before)) {
			fprintf(stderr,
					"FAIL %s: moving toward the HIGH side LOWERED the cart "
					"(%.6f -> %.6f). calcRailMeta gave this tile its shape "
					"BECAUSE the track continues one block UP that way "
					"(block_rail.c:129-132), and Beta's gravity for that shape "
					"pushes the OTHER way -- so the track must rise here.\n",
					name, (double)y_before, (double)y_after);
			assert(0 && "slope geometry is inverted for this shape");
		}
	} else {
		if(!(y_after < y_before)) {
			fprintf(stderr,
					"FAIL %s: moving toward the LOW side RAISED the cart "
					"(%.6f -> %.6f)\n",
					name, (double)y_before, (double)y_after);
			assert(0 && "slope geometry is inverted for this shape");
		}
	}
}

/* ---------------------------------------------------------------------------
 * (B) GRAVITY -- a cart at rest on a slope must accelerate DOWNHILL by Beta's
 * 1/128 per tick, and never uphill.
 * ------------------------------------------------------------------------- */
static void assert_rolls_downhill(int lx, int ly, int lz, int hx, int hz,
								  const char* name) {
	clear_carts();
	assert(place_cart_on(lx, ly, lz));
	assert(server_local_test_tick_entities(&s));
	struct entity* e = only_minecart();

	/* Arm the heading along the slope axis pointing UPHILL, and leave the cart
	 * at rest. With Beta's impulse the cart must end the tick moving DOWNHILL,
	 * i.e. speed must go NEGATIVE against an uphill heading. */
	e->data.minecart.speed = 0.0f;
	e->data.minecart.hx = (int8_t)hx;
	e->data.minecart.hz = (int8_t)hz;

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float got = e->data.minecart.speed;
	if(!(got < 0.0f)) {
		fprintf(stderr,
				"FAIL %s: a cart at rest on a slope did not roll downhill "
				"(speed %.9f). Beta applies motion -= 0.0078125 per tick on "
				"every sloped shape [740c583 EntityMinecart.onUpdate:209-246]; "
				"CaveWii applies no slope impulse at all.\n",
				name, (double)got);
		assert(0 && "missing Beta slope gravity");
	}
	/* Full Beta order: the impulse moves the cart by 1/128 first, empty-cart
	 * friction then retains 0.96 of its momentum, and the 45-degree track drop
	 * adds 0.05 of the PRE-friction displacement as rail-height energy. */
	near_eq(got,
			-BETA_SLOPE_IMPULSE * (BETA_EMPTY_FRICTION + 0.05f), name);
}

/* Idempotent silent block swap -- used to turn a ramp's low tile into a powered
 * rail while KEEPING the slope shape the placement path conferred. Legitimate
 * for a fixture: what is under test is the CART TICK, not rail placement. */
static void force_block(int x, int y, int z, int type, int meta) {
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
	assert(b.type == (uint8_t)type && (b.metadata & 0x0F) == (meta & 0x0F));
}

/* ---------------------------------------------------------------------------
 * ORDER GUARD: an UNPOWERED powered rail on a slope must HOLD a resting cart.
 *
 * This is the case that discriminates where the slope impulse sits relative to
 * the powered-rail branch, and it is reachable: calcRailMeta's slope tests
 * (block_rail.c:129-132) run BEFORE the allow_curves gate, so a powered rail can
 * legally carry shapes 2-5.
 *
 * Beta's on-rail order is impulse (:232-246) THEN brake (:262-273):
 *     impulse  -> speed = -0.0078125
 *     brake    -> |speed| < 0.03, so ALL motion is zeroed
 *     friction -> 0
 *   => a resting cart stays pinned. An unpowered powered rail is a STOPPER, and
 *      it stops a cart on a hill too.
 *
 * With the two swapped, the brake would run first against a speed of 0 (a no-op)
 * and the impulse would then be applied AFTER it, leaving the cart creeping
 * downhill at -0.0078125 * 0.96 every tick -- a brake that cannot hold a hill.
 * That is exactly the bug this asserts against.
 * ------------------------------------------------------------------------- */
static void test_unpowered_slope_brake_holds(int lx, int ly, int lz) {
	/* Build a plain ascending-east ramp so the placement path confers shape 2,
	 * then swap the low tile to an UNPOWERED powered rail keeping that shape. */
	const uint8_t shape = build_ramp(lx, ly, lz, +1, 0);
	assert(shape == 2);
	force_block(lx, ly, lz, BLOCK_POWERED_RAIL, 2); /* bit 8 CLEAR => brake */

	clear_carts();
	assert(place_cart_on(lx, ly, lz));
	assert(server_local_test_tick_entities(&s));
	struct entity* e = only_minecart();

	e->data.minecart.speed = 0.0f;
	e->data.minecart.hx = +1; /* heading along the slope axis, pointing uphill */
	e->data.minecart.hz = 0;

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float got = e->data.minecart.speed;
	if(got != 0.0f) {
		fprintf(stderr,
				"FAIL unpowered powered-rail slope: a resting cart crept at "
				"%.9f. Beta applies the slope impulse BEFORE the brake, so the "
				"brake's `< 0.03 => zero everything` swallows it and the cart "
				"is held. A nonzero value here means the impulse is being "
				"applied AFTER the brake.\n",
				(double)got);
		assert(0 && "brake must hold a resting cart on a slope");
	}

	tear_down_ramp(lx, ly, lz, +1, 0);
}

/* ---------------------------------------------------------------------------
 * (D) RAIL-HEIGHT ENERGY EXCHANGE -- the second half of Beta's slope model.
 *
 * [740c583 EntityMinecart.onUpdate:360-370] rescales the motion to
 * magnitude (speed + (y_before - y_after) * 0.05) after the move, so a
 * descending cart keeps gaining speed instead of settling at whatever the
 * 1/128 impulse alone supports.
 *
 * On this engine's slope track the height changes 1:1 with the PRE-friction
 * horizontal step (a 45-degree ramp). The energy term is therefore 5% of S1,
 * while friction is independently applied to stored momentum:
 *
 * Full expected tick, downhill on shape 2 (high side EAST, so travelling -X is
 * downhill) starting from a signed speed S < 0 with heading hx = +1:
 *     impulse   S1 = S - 0.0078125      (pushes -X, i.e. more negative)
 *     move      step = |S1| toward -X, dropping the cart by |S1|
 *     friction  S2 = S1 * 0.9599999785423279
 *     energy    S3 = S2 - |S1| * 0.05
 * and uphill (S > 0) the same chain with the impulse still -0.0078125 and the
 * energy term subtracting |S1| * 0.05.
 *
 * The 0.05 is Beta's literal rail-height exchange constant, not tuning.
 * ------------------------------------------------------------------------- */
static void test_slope_energy_exchange(int lx, int ly, int lz) {
	const uint8_t shape = build_ramp(lx, ly, lz, +1, 0); /* ascends EAST */
	assert(shape == 2);

	/* --- DOWNHILL: heading +X with a NEGATIVE speed travels -X, downhill. --- */
	clear_carts();
	assert(place_cart_on(lx, ly, lz));
	assert(server_local_test_tick_entities(&s));
	struct entity* e = only_minecart();

	const float S_down = -0.10f;
	e->data.minecart.speed = S_down;
	e->data.minecart.hx = +1;
	e->data.minecart.hz = 0;
	const float y0_down = e->pos[1];

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float s1_down = S_down - BETA_SLOPE_IMPULSE;
	const float s2_down = s1_down * BETA_EMPTY_FRICTION;
	near_eq(e->data.minecart.speed, s2_down - fabsf(s1_down) * 0.05f,
			"downhill: impulse, move, friction, then +5% height energy");
	assert(e->pos[1] < y0_down && "a downhill tick must LOWER the cart");
	/* And it must be strictly faster than the impulse+friction alone would
	 * give -- i.e. the energy term is actually doing something. */
	assert(fabsf(e->data.minecart.speed) > fabsf(s2_down)
		   && "descending must convert height into speed");

	/* --- UPHILL: heading +X with a POSITIVE speed travels +X, uphill. ------ */
	clear_carts();
	assert(place_cart_on(lx, ly, lz));
	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float S_up = +0.10f;
	e->data.minecart.speed = S_up;
	e->data.minecart.hx = +1;
	e->data.minecart.hz = 0;
	const float y0_up = e->pos[1];

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float s1_up = S_up - BETA_SLOPE_IMPULSE; /* gravity still pushes -X */
	const float s2_up = s1_up * BETA_EMPTY_FRICTION;
	near_eq(e->data.minecart.speed, s2_up - fabsf(s1_up) * 0.05f,
			"uphill: impulse, move, friction, then -5% height energy");
	assert(e->pos[1] > y0_up && "an uphill tick must RAISE the cart");
	assert(fabsf(e->data.minecart.speed) < fabsf(s2_up)
		   && "climbing must cost speed");

	tear_down_ramp(lx, ly, lz, +1, 0);
}

/* A powered slope reaches every order-sensitive stage in one tick. Starting
 * downhill on shape 2 must compose as:
 *   slope impulse -> pre-friction move -> empty friction -> height energy
 *   -> exact sign-preserving 0.06 powered boost.
 * Moving the boost earlier changes both the displacement/height term and the
 * final scalar, so this is stronger than an isolated flat-track constant test. */
static void test_powered_slope_boost_order(int lx, int ly, int lz) {
	const uint8_t shape = build_ramp(lx, ly, lz, +1, 0); /* ascends EAST */
	assert(shape == 2);

	clear_carts();
	assert(place_cart_on(lx, ly, lz));
	assert(server_local_test_tick_entities(&s));
	struct entity* e = only_minecart();

	/* Reset to the low tile centre after the ordinary-rail settle tick, then
	 * preserve the legal slope shape while enabling powered bit 8. */
	e->pos[0] = (float)lx + 0.5f;
	e->pos[1] = (float)ly + 0.5f;
	e->pos[2] = (float)lz + 0.5f;
	e->data.minecart.speed = -0.10f;
	e->data.minecart.hx = +1;
	e->data.minecart.hz = 0;
	force_block(lx, ly, lz, BLOCK_POWERED_RAIL, 0x8 | shape);

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	const float after_impulse = -0.10f - BETA_SLOPE_IMPULSE;
	const float after_friction = after_impulse * BETA_EMPTY_FRICTION;
	const float expected = after_friction
		- fabsf(after_impulse) * 0.05f - BETA_POWERED_BOOST;
	near_eq(e->data.minecart.speed, expected,
			"powered slope must boost after impulse, move, friction, and energy");
	near_eq(e->pos[0], (float)lx + 0.5f - fabsf(after_impulse),
			"powered boost must not affect this tick's pre-friction displacement");
	near_eq(e->pos[1], (float)ly + 0.5f - fabsf(after_impulse),
			"powered slope height must follow the pre-friction displacement");

	tear_down_ramp(lx, ly, lz, +1, 0);
}

/* A FLAT tile changes no height, so the energy term must be a strict no-op
 * there. This is the regression guard for every existing flat-track golden. */
static void test_flat_track_unaffected(int x, int y, int z) {
	set(x, y - 1, z, BLOCK_STONE, 0);
	assert(place_rail(x, y, z));
	assert(getmeta(x, y, z) == 0); /* a lone rail normalises to flat NS */

	clear_carts();
	assert(place_cart_on(x, y, z));
	assert(server_local_test_tick_entities(&s));
	struct entity* e = only_minecart();

	const float S = +0.10f;
	e->data.minecart.speed = S;
	e->data.minecart.hx = 0;
	e->data.minecart.hz = +1;
	const float y0 = e->pos[1];

	assert(server_local_test_tick_entities(&s));
	e = only_minecart();

	near_eq(e->pos[1], y0, "a flat tile must not change the cart's height");
	near_eq(e->data.minecart.speed, S * BETA_EMPTY_FRICTION,
			"on flat track the tick must still be friction and nothing else");

	clear_carts();
	clear_cell(x, y, z);
	clear_cell(x, y - 1, z);
}

int main(void) {
	test_fixture_dir("test_minecart_slope_beta_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
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

	/* The four ramps, each built by the production placement path. The asserted
	 * shape is the CONTRACT between calcRailMeta and the cart tick: if these
	 * four ever change, every expectation below is void. */
	const struct {
		int hx, hz;
		uint8_t want_shape;
		const char* name;
	} RAMPS[] = {
		{+1, 0, 2, "shape 2 (ascends EAST)"},
		{-1, 0, 3, "shape 3 (ascends WEST)"},
		{0, -1, 4, "shape 4 (ascends NORTH)"},
		{0, +1, 5, "shape 5 (ascends SOUTH)"},
	};

	const int LX = 8, LY = 101, LZ = 8;

	for(size_t i = 0; i < sizeof(RAMPS) / sizeof(RAMPS[0]); i++) {
		const uint8_t shape
			= build_ramp(LX, LY, LZ, RAMPS[i].hx, RAMPS[i].hz);
		if(shape != RAMPS[i].want_shape) {
			fprintf(stderr,
					"FAIL fixture: a ramp ascending toward (%+d,%+d) was given "
					"rail shape %u, expected %u (block_rail.c:129-132)\n",
					RAMPS[i].hx, RAMPS[i].hz, (unsigned)shape,
					(unsigned)RAMPS[i].want_shape);
			assert(0 && "calcRailMeta shape contract changed");
		}

		/* (A) geometry, both directions of travel */
		assert_height_follows_track(LX, LY, LZ, RAMPS[i].hx, RAMPS[i].hz, +1,
									RAMPS[i].name);
		assert_height_follows_track(LX, LY, LZ, RAMPS[i].hx, RAMPS[i].hz, -1,
									RAMPS[i].name);

		/* (B) gravity */
		assert_rolls_downhill(LX, LY, LZ, RAMPS[i].hx, RAMPS[i].hz,
							  RAMPS[i].name);

		tear_down_ramp(LX, LY, LZ, RAMPS[i].hx, RAMPS[i].hz);
	}

	/* (C) the impulse-vs-brake ordering guard */
	test_unpowered_slope_brake_holds(LX, LY, LZ);

	/* (D) the rail-height energy exchange, and its no-op on flat track */
	test_slope_energy_exchange(LX, LY, LZ);
	test_powered_slope_boost_order(LX, LY, LZ);
	test_flat_track_unaffected(LX + 4, LY, LZ);

	printf("test_minecart_slope_beta: slope geometry follows the placed track, "
		   "carts roll downhill at Beta's 1/128 per tick, and powered-rail "
		   "brake/boost ordering is exact OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
