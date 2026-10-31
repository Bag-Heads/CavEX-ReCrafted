/*
	Beta 1.7.3 powered-rail SIGN/DIRECTION goldens for the minecart.

	WHY THIS EXISTS. 5fabbb5 fixed two sign bugs (the boost gated on SIGNED
	speed, and seed_heading double-applying sgn) but left the third: the at-rest
	branch still does `speed = 0.05f` unconditionally. That
	  - reverses any cart slower than 0.01 in the negative direction, and
	  - invents a positive launch for a resting cart with no wall beside it,
	neither of which Beta does. test_minecart_place cannot catch it: it starts a
	resting cart at speed 0 on a powered rail and asserts only `speed > 0`, so
	it PASSES on the buggy behaviour and would keep passing if the bug got worse.

	GROUND TRUTH -- decompile 740c583, EntityMinecart.onUpdate.

	Powered (the powered flag set), :396-415, in prose:
	    speed = sqrt(motionX^2 + motionZ^2);
	    when speed > 0.01D:                                   // MAGNITUDE, strict >
	        boost = 0.06D;
	        motionX += motionX / speed * boost;      // along existing motion
	        motionZ += motionZ / speed * boost;      // => sign-preserving
	    otherwise, on shape 1:                                  // flat EW rail only
	        a normal cube at (x - 1, y, z) -> motionX =  0.02D;
	        else one at (x + 1, y, z)      -> motionX = -0.02D;
	    otherwise, on shape 0:                                  // flat NS rail only
	        a normal cube at (x, y, z - 1) -> motionZ =  0.02D;
	        else one at (x, y, z + 1)      -> motionZ = -0.02D;
	    (nothing else)
	Three consequences this file pins:
	  (a) the gate is on MAGNITUDE and the boost cannot change sign;
	  (b) the rest kick is WALL-DIRECTED and SIGNED (+0.02 toward +axis when the
	      wall is on the -axis side), and applies ONLY to flat straights
	      (shape 0 or 1) -- slopes and curves get no kick;
	  (c) with NO wall on either side there is NO `else`, so Beta leaves the cart
	      exactly as it was. It never fabricates a direction.

	Unpowered powered rail = BRAKE (the unpowered flag), :263-272:
	    speed = sqrt(motionX*motionX + motionZ*motionZ);
	    if (speed < 0.03D) { motionX *= 0.0D; motionY *= 0.0D; motionZ *= 0.0D; }
	    else               { motionX *= 0.5D; motionY *= 0.0D; motionZ *= 0.5D; }
	  -- also magnitude-gated, and `*= 0.5` is sign-preserving, so braking must
	  be symmetric for +v and -v.

	ENGINE MAPPING. This port models rail motion as a SIGNED SCALAR `speed` along
	a unit `heading`, rather than Beta's (motionX, motionZ) vector. On one rail
	axis the mapping is exact: vector magnitude is fabs(speed), adding along the
	unit motion vector is a sign-preserving scalar addition, and Beta's friction
	multipliers apply directly to the scalar. The goldens below therefore pin the
	actual Beta constants and order, not a weaker approximation.

	Drives the real production path end to end (server_local_test_tick_entities
	-> minecart_server_tick), like test_minecart_place.
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
#include "entity/combat.h" /* PLAYER_EYE_HEIGHT for the 2b nudge section */
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_minecart_rail_beta_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 /* stone floor */
#define RY 101 /* rail plane  */

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

/* Force EXACT rail metadata (shape + the 0x8 power bit).
 * server_world_set_block runs the rail's onBlockAdded, which RECOMPUTES the
 * shape from neighbours -- a lone rail always normalises to NS (0), so a plain
 * set() can never build an EW fixture (test_minecart_place:179 documents the
 * same normalisation). The silent setter writes the cell without the lifecycle,
 * which is exactly what a fixture wants. Every caller asserts the result. */
static void force_rail(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block_silent(&s.worlds[0], x, y, z,
										 (struct block_data) {
											 .type = (uint8_t)type,
											 .metadata = (uint8_t)(meta & 0x0F),
										 }));
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	assert(b.type == (uint8_t)type && (b.metadata & 0x0F) == (meta & 0x0F)
		   && "fixture rail must keep the EXACT shape/power metadata asked for");
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

/* Remove every minecart so each scenario starts clean. */
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

/* Put a cart on the rail at (x,RY,z) with an exact starting speed+heading, then
 * run ONE production server tick. Returns the resulting speed. */
struct tick_result {
	float speed;
	float dx;
	float dz;
};

static struct tick_result tick_with_state(int x, int z, float start_speed,
										 int8_t hx, int8_t hz, bool occupied,
										 int32_t pending_throttle) {
	clear_carts();
	assert(place_cart_on(x, RY, z)); /* ItemMinecart must be clicked on the RAIL */
	struct entity* e = only_minecart();
	/* settle onto the rail first so the tick under test is a pure rail tick */
	assert(server_local_test_tick_entities(&s));
	e = only_minecart();
	e->data.minecart.speed = start_speed;
	e->data.minecart.hx = hx;
	e->data.minecart.hz = hz;
	e->data.minecart.occupied = occupied;
	e->data.minecart.occupant_id = occupied ? 1 : 0;
	e->data.minecart.ride_epoch = occupied ? 1 : 0;
	e->data.minecart.pending_throttle = pending_throttle;
	float x0 = e->pos[0];
	float z0 = e->pos[2];
	assert(server_local_test_tick_entities(&s));
	e = only_minecart();
	return (struct tick_result) {
		.speed = e->data.minecart.speed,
		.dx = e->pos[0] - x0,
		.dz = e->pos[2] - z0,
	};
}

static float tick_with_speed(int x, int z, float start_speed, int8_t hx,
							 int8_t hz) {
	return tick_with_state(x, z, start_speed, hx, hz, false, 0).speed;
}

/* 740c583 EntityMinecart.onUpdate: occupied carts retain 0.996999979019165,
 * ordinary empty carts retain 0.9599999785423279, then a moving powered rail
 * adds 0.06 along the already-normalized motion vector. */
#define BETA_EMPTY_FRICTION 0.9599999785423279f
#define BETA_RIDDEN_FRICTION 0.996999979019165f
#define BETA_POWERED_BOOST 0.06f

#define EPS 1e-5f
static void near_eq(float got, float want, const char* what) {
	if(fabsf(got - want) > EPS) {
		fprintf(stderr, "FAIL %s: got %.6f want %.6f\n", what, got, want);
		assert(0 && "value mismatch");
	}
}

int main(void) {
	test_fixture_dir("test_minecart_rail_beta_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;

	struct server_chunk* sc = NULL;
	/* Every rail scenario stays in chunk (0,0), through block 15. Load the
	 * complete Beta position +/-32 entity-activation neighborhood. */
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	/* Beta World.updateEntityWithOptionalForce uses fixed vertical bounds
	 * 0..128 for its radius-32 chunk-existence gate. An entity's own Y therefore
	 * cannot freeze it. Also pin the addedToChunk=false recovery order: the first
	 * active pass reattaches without onUpdate; only the following pass ticks. */
	struct entity* high = malloc(sizeof(*high));
	struct entity* low = malloc(sizeof(*low));
	assert(high && low);
	entity_item(900001, high, true, &s.world,
		(struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1});
	entity_item(900002, low, true, &s.world,
		(struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1});
	high->teleport(high, (vec3) {8.5f, 200.0f, 8.5f});
	low->teleport(low, (vec3) {10.5f, -40.0f, 10.5f});
	high->data.item.age = 30;
	low->data.item.age = 40;
	*dict_entity_safe_get(s.entities, high->id) = high;
	*dict_entity_safe_get(s.entities, low->id) = low;
	assert(!high->added_to_chunk);
	server_local_refresh_entity_membership(&s, low);
	assert(low->added_to_chunk);
	assert(server_local_test_tick_entities(&s));
	assert(high->added_to_chunk && high->data.item.age == 30);
	assert(low->data.item.age == 41);
	assert(server_local_test_tick_entities(&s));
	assert(high->data.item.age == 31);
	assert(low->data.item.age == 42);
	free(high);
	free(low);
	dict_entity_erase(s.entities, 900001);
	dict_entity_erase(s.entities, 900002);

	/* ---- fixture: an EW powered rail at (4,RY,4) and an NS one at (8,RY,8),
	 * both on stone, both with NO adjacent walls to begin with. ------------ */
	const int EX = 4, EZ = 4; /* EW rail, shape 1 */
	const int NX = 8, NZ = 8; /* NS rail, shape 0 */
	set(EX, YF, EZ, BLOCK_STONE, 0);
	set(NX, YF, NZ, BLOCK_STONE, 0);

	/* =====================================================================
	 * (1) POWERED, MOVING: sign preserved both ways, same magnitude change.
	 * Beta :398-401 -- boost follows the unit vector of existing motion.
	 * ===================================================================== */
	force_rail(EX, RY, EZ, BLOCK_POWERED_RAIL, 0x8 | 1); /* powered, flat EW */

	float pos_out = tick_with_speed(EX, EZ, +0.10f, +1, 0);
	float neg_out = tick_with_speed(EX, EZ, -0.10f, +1, 0);
	assert(pos_out > 0.0f && "a positive cart must stay positive on a boost");
	assert(neg_out < 0.0f
		   && "a NEGATIVE cart must stay NEGATIVE on a boost (Beta adds along "
			  "existing motion, which cannot flip the sign)");
	near_eq(fabsf(pos_out), fabsf(neg_out),
			"boost magnitude must be direction-symmetric");
	near_eq(pos_out, +0.10f * BETA_EMPTY_FRICTION + BETA_POWERED_BOOST,
			"moving empty cart: Beta friction then exact +0.06 powered boost");
	near_eq(neg_out, -0.10f * BETA_EMPTY_FRICTION - BETA_POWERED_BOOST,
			"negative moving cart gets the same exact boost along its motion");

	/* No hidden 0.2 momentum cap: Beta clamps only the displacement copy to
	 * 0.4, while the stored motion receives the full +0.06 after friction. */
	near_eq(tick_with_speed(EX, EZ, +0.50f, +1, 0),
			+0.50f * BETA_EMPTY_FRICTION + BETA_POWERED_BOOST,
			"powered rail must not cap stored momentum at the old 0.2 shortcut");

	/* A ridden cart is boosted without input. CaveWii's controller throttle is
	 * deliberately limited to ordinary rail; a powered rail remains autonomous,
	 * matching Beta's no-rider-force simulation on this track type. */
	struct tick_result ridden_auto
		= tick_with_state(EX, EZ, +0.10f, +1, 0, true, 0);
	struct tick_result ridden_press
		= tick_with_state(EX, EZ, +0.10f, +1, 0, true, +1);
	near_eq(ridden_auto.speed,
			+0.10f * BETA_RIDDEN_FRICTION + BETA_POWERED_BOOST,
			"ridden powered rail boosts automatically with Beta ridden friction");
	near_eq(ridden_press.speed, ridden_auto.speed,
			"rider throttle must not alter powered-rail acceleration");
	near_eq(ridden_auto.dx, +0.10f * 0.75f,
			"Beta moves with pre-friction momentum before applying the boost");

	/* The controller carve-out remains available on ordinary and detector rail.
	 * Detector rail has no autonomous boost/brake, so dropping one authored step
	 * at each detector tile would be a controller-visible throttle stutter. */
	force_rail(EX, RY, EZ, BLOCK_RAIL, 1);
	struct tick_result ordinary_press
		= tick_with_state(EX, EZ, +0.10f, +1, 0, true, +1);
	near_eq(ordinary_press.speed,
			(+0.10f + 0.01f) * BETA_RIDDEN_FRICTION,
			"ordinary rail keeps the documented +0.01 rider throttle carve-out");
	force_rail(EX, RY, EZ, BLOCK_DETECTOR_RAIL, 0x8 | 1);
	struct tick_result detector_press
		= tick_with_state(EX, EZ, +0.10f, +1, 0, true, +1);
	near_eq(detector_press.speed, ordinary_press.speed,
			"detector rail must not drop the controller throttle for one tile");

	/* Beta has no generic coasting cutoff. CaveWii's documented Wii-performance
	 * stop snap must stay far below live rail decisions: 1e-4 still coasts, so
	 * the old 0.001 cutoff cannot return. This assertion fails on the old path. */
	force_rail(EX, RY, EZ, BLOCK_RAIL, 1);
	near_eq(tick_with_speed(EX, EZ, +0.0001f, +1, 0),
			+0.0001f * BETA_EMPTY_FRICTION,
			"ordinary coasting at 1e-4 must survive the tiny terminal snap");

	/* The sub-1e-5 tail completes this tick's displacement, then snaps as the
	 * final speed write and publishes one exact terminal state. The next tick
	 * takes the exact-zero idle path and leaves the correction rotation. */
	clear_carts();
	assert(place_cart_on(EX, RY, EZ));
	struct entity* terminal = only_minecart();
	assert(server_local_test_tick_entities(&s)); /* settle spawn pose */
	terminal = only_minecart();
	terminal->data.minecart.speed = 0.000005f;
	terminal->data.minecart.hx = +1;
	terminal->data.minecart.hz = 0;
	terminal->data.minecart.snapshot_dirty = false;
	float terminal_x = terminal->pos[0];
	float terminal_z = terminal->pos[2];
	assert(server_local_test_tick_entities(&s));
	terminal = only_minecart();
	assert(terminal->data.minecart.speed == 0.0f
		   && "a sub-1e-5 residual must become the idle gate's exact zero");
	assert(terminal->data.minecart.snapshot_dirty
		   && "the nonzero-to-zero snap must leave one terminal snapshot due");
	assert(terminal->pos[0] > terminal_x
		   && "the terminal snap must be the final write after residual movement");
	near_eq(terminal->pos[2], terminal_z,
			"the terminal residual must stay on its rail axis");
	terminal_x = terminal->pos[0];
	terminal_z = terminal->pos[2];
	terminal->data.minecart.snapshot_dirty = false;
	assert(server_local_test_tick_entities(&s));
	assert(!terminal->data.minecart.snapshot_dirty
		   && "an exact-zero idle tick must leave the correction rotation");
	near_eq(terminal->pos[0], terminal_x,
			"the exact-zero idle tick must not move on X");
	near_eq(terminal->pos[2], terminal_z,
			"the exact-zero idle tick must not move on Z");

	/* An unpowered powered rail hard-stops before the generic final snap. The
	 * moving gate must still retain its exact-zero terminal snapshot. */
	force_rail(EX, RY, EZ, BLOCK_POWERED_RAIL, 1);
	terminal->data.minecart.speed = 0.000005f;
	terminal->data.minecart.snapshot_dirty = false;
	assert(server_local_test_tick_entities(&s));
	assert(terminal->data.minecart.speed == 0.0f);
	assert(terminal->data.minecart.snapshot_dirty
		   && "a Beta brake must retain the same terminal snapshot invariant");
	force_rail(EX, RY, EZ, BLOCK_POWERED_RAIL, 0x8 | 1);

	/* =====================================================================
	 * (2) NEAR-ZERO / NO WALL: never fabricate a direction.
	 * Beta :402-414 has no `else` -- with no wall the motion is UNTOUCHED.
	 * ===================================================================== */
	float slow_neg = tick_with_speed(EX, EZ, -0.009f, +1, 0);
	assert(slow_neg < 0.0f
		   && "a slow NEGATIVE cart must never be flipped positive by a powered "
			  "rail (this is the surviving `speed = 0.05f` bug)");

	float resting = tick_with_speed(EX, EZ, 0.0f, +1, 0);
	near_eq(resting, 0.0f,
			"a resting cart with NO adjacent wall must NOT be launched (Beta "
			"has no else-branch; it leaves motion untouched)");

	/* =====================================================================
	 * (3) RESTING WALL KICKS: signed, wall-directed, both axes.
	 * Beta EW :403-406 -- wall at x-1 => +0.02; wall at x+1 => -0.02.
	 * ===================================================================== */
	/* ORDER MATTERS: placing the wall runs the rail's onNeighbourBlockChange,
	 * which recomputes the rail SHAPE. So build the wall FIRST and force the
	 * rail's shape/power metadata afterwards, then assert it survived. */
	set(EX - 1, RY, EZ, BLOCK_STONE, 0); /* wall on the -X side */
	force_rail(EX, RY, EZ, BLOCK_POWERED_RAIL, 0x8 | 1);
	near_eq(tick_with_speed(EX, EZ, 0.0f, +1, 0), +0.02f,
			"EW rest kick with a wall at x-1 must be Beta's post-friction +0.02");
	set(EX - 1, RY, EZ, BLOCK_AIR, 0);

	set(EX + 1, RY, EZ, BLOCK_STONE, 0); /* wall on the +X side */
	force_rail(EX, RY, EZ, BLOCK_POWERED_RAIL, 0x8 | 1);
	near_eq(tick_with_speed(EX, EZ, 0.0f, +1, 0), -0.02f,
			"EW rest kick with a wall at x+1 must be Beta's post-friction -0.02");
	set(EX + 1, RY, EZ, BLOCK_AIR, 0);

	/* Beta NS :409-412 -- wall at z-1 => +0.02; wall at z+1 => -0.02. */
	set(NX, RY, NZ - 1, BLOCK_STONE, 0);
	force_rail(NX, RY, NZ, BLOCK_POWERED_RAIL, 0x8 | 0);
	near_eq(tick_with_speed(NX, NZ, 0.0f, 0, +1), +0.02f,
			"NS rest kick with a wall at z-1 must be Beta's post-friction +0.02");
	set(NX, RY, NZ - 1, BLOCK_AIR, 0);

	set(NX, RY, NZ + 1, BLOCK_STONE, 0);
	force_rail(NX, RY, NZ, BLOCK_POWERED_RAIL, 0x8 | 0);
	near_eq(tick_with_speed(NX, NZ, 0.0f, 0, +1), -0.02f,
			"NS rest kick with a wall at z+1 must be Beta's post-friction -0.02");
	set(NX, RY, NZ + 1, BLOCK_AIR, 0);

	/* =====================================================================
	 * (4) UNPOWERED POWERED RAIL = BRAKE, direction-symmetric.
	 * Beta :263-272 -- magnitude < 0.03 => hard zero; else *= 0.5.
	 * ===================================================================== */
	force_rail(EX, RY, EZ, BLOCK_POWERED_RAIL, 1); /* bit 8 CLEAR => brake, flat EW */

	near_eq(tick_with_speed(EX, EZ, +0.02f, +1, 0), 0.0f,
			"brake below the 0.03 threshold must stop a POSITIVE cart dead");
	near_eq(tick_with_speed(EX, EZ, -0.02f, +1, 0), 0.0f,
			"brake below the 0.03 threshold must stop a NEGATIVE cart dead");

	float brake_pos = tick_with_speed(EX, EZ, +0.10f, +1, 0);
	float brake_neg = tick_with_speed(EX, EZ, -0.10f, +1, 0);
	assert(brake_pos > 0.0f && brake_pos < 0.10f
		   && "a positive cart above the threshold is halved, not stopped");
	assert(brake_neg < 0.0f && brake_neg > -0.10f
		   && "a NEGATIVE cart above the threshold is halved and KEEPS its sign");
	near_eq(fabsf(brake_pos), fabsf(brake_neg),
			"braking must be direction-symmetric");
	near_eq(brake_pos, +0.10f * 0.5f * BETA_EMPTY_FRICTION,
			"unpowered rail halves before Beta empty-cart friction");
	near_eq(brake_neg, -0.10f * 0.5f * BETA_EMPTY_FRICTION,
			"negative unpowered brake uses the same exact constants and order");

	/* =====================================================================
	 * BETA'S WALL LAUNCHER ON A CART THAT HAS NEVER MOVED.
	 * Every other case here goes through tick_with_speed(), which ASSIGNS hx/hz
	 * before ticking -- so none of them can see a cart whose heading is still
	 * (0,0). That is the state a freshly placed cart is actually in, and it is
	 * the state Beta's launcher is used in: drop a cart on a powered rail with
	 * a block behind it, flip the lever, watch it go. Beta writes motionX/
	 * motionZ outright so it always works; our signed-scalar port multiplies the
	 * kick by the heading, so a missing seed silently yields kick * 0 == 0.
	 * ===================================================================== */
	{
		const int LX = 12, LZ = 12; /* inside the single loaded chunk (0..15) */
		set(LX, YF, LZ, BLOCK_STONE, 0);
		/* Wall on the -Z side FIRST (placing it re-triggers rail shape recompute,
		 * so the forced shape has to come after it). */
		set(LX, RY, LZ - 1, BLOCK_STONE, 0);
		force_rail(LX, RY, LZ, BLOCK_POWERED_RAIL, 0 | 0x8); /* flat NS, powered */

		clear_carts();
		assert(place_cart_on(LX, RY, LZ));
		struct entity* fresh = only_minecart();
		/* The cart as it really is on placement: no heading, at rest. */
		fresh->data.minecart.hx = 0;
		fresh->data.minecart.hz = 0;
		fresh->data.minecart.speed = 0.0f;
		assert(server_local_test_tick_entities(&s));
		fresh = only_minecart();
		assert(fresh->data.minecart.speed > 0.0f
			   && "a wall on the -Z side must launch a NEVER-MOVED cart toward "
				  "+Z, exactly as Beta's motionZ = 0.02 does");
		assert(fabsf(fresh->data.minecart.speed - 0.02f) < 1e-6f
			   && "the launch magnitude must be Beta's post-friction 0.02");
	}

	/* =====================================================================
	 * PUSHED CART, HEADING NEVER SEEDED: must depart in the PUSH direction.
	 * The second 5fabbb5 sign bug: seed_heading baked sgn(speed) into the
	 * stored heading and the tick multiplied that heading by sgn(speed) AGAIN
	 * -- sgn*sgn is always +1, so a freshly pushed cart departed +X (EW rail)
	 * or +Z (NS rail) whichever way it was pushed. The heading is the rail
	 * AXIS and must stay unsigned; the sign lives in `speed` alone.
	 * tick_with_speed cannot catch this: it ASSIGNS hx/hz before ticking, and
	 * the wall-launcher case above seeds AT REST. This drives seed_heading
	 * with a nonzero speed, the exact state the old bug corrupted.
	 * ===================================================================== */
	{
		static const struct {
			int x, z;
			uint8_t meta; /* rail shape: 1 = flat EW, 0 = flat NS */
			int axis;	 /* 0 = X, 1 = Z */
		} seeds[] = {
			{EX, EZ, 1, 0},
			{NX, NZ, 0, 1},
		};
		for(size_t r = 0; r < sizeof seeds / sizeof seeds[0]; r++) {
			force_rail(seeds[r].x, RY, seeds[r].z, BLOCK_RAIL, seeds[r].meta);
			for(size_t k = 0; k < 2; k++) {
				float push = (k == 0) ? +0.10f : -0.10f;
				clear_carts();
				assert(place_cart_on(seeds[r].x, RY, seeds[r].z));
				struct entity* e = only_minecart();
				/* settle onto the rail first so the tick under test is a
				 * pure rail tick */
				assert(server_local_test_tick_entities(&s));
				e = only_minecart();
				e->data.minecart.hx = 0; /* pushed state: speed, no heading */
				e->data.minecart.hz = 0;
				e->data.minecart.speed = push;
				float p0 = seeds[r].axis ? e->pos[2] : e->pos[0];
				assert(server_local_test_tick_entities(&s));
				e = only_minecart();
				float dp = seeds[r].axis ? e->pos[2] - p0 : e->pos[0] - p0;
				assert((push > 0.0f ? dp : -dp) > 0.01f
					   && "a pushed cart must depart in the PUSH direction "
						  "along the rail axis, not a sign-squared constant "
						  "+axis (seed_heading must not bake in sgn(speed))");
			}
		}
	}

	/* =====================================================================
	 * BETA'S 0.4 DISPLACEMENT CLAMP [740c583 EntityMinecart.onUpdate:209
	 * (var4 = 0.4D), :308-317]. Beta clamps a COPY of the motion used for the
	 * move; stored momentum stays unclamped for next tick's physics. Without
	 * it the slope energy exchange converges to ~1.9 blocks/tick and the
	 * per-tile movement switch corrupts (one tile's meta applied to a
	 * multi-tile displacement).
	 * ===================================================================== */
	{
		clear_carts();
		assert(place_cart_on(NX, RY, NZ)); /* flat NS rail fixture */
		struct entity* fast = only_minecart();
		assert(server_local_test_tick_entities(&s));
		fast = only_minecart();
		fast->data.minecart.speed = 2.0f; /* far beyond anything legit */
		fast->data.minecart.hx = 0;
		fast->data.minecart.hz = 1;
		float z0 = fast->pos[2];
		assert(server_local_test_tick_entities(&s));
		fast = only_minecart();
		assert(fabsf(fast->pos[2] - z0) <= 0.401f
			   && "one tick may DISPLACE at most Beta's 0.4 blocks");
		assert(fast->data.minecart.speed > 0.5f
			   && "stored momentum is NOT clamped -- Beta clamps only the "
				  "displacement copy (friction alone acts on storage)");
	}

	/* =====================================================================
	 * (batch 2c) FIXED-TRACK TIMING TRACE -- the 2026-08-07 handoff's
	 * "measure before changing speed" gate. A 30-tick integration over a
	 * straight fully-powered flat track, compared PER TICK against an
	 * independent reference recurrence built from the decompile order
	 * [740c583 EntityMinecart.onUpdate]:
	 *     displacement = clamp(speed, +-0.4)  (movement copy)
	 *     speed *= 0.9599999785423279         (empty-cart friction, after move)
	 *     if (|speed| > 0.01) speed += 0.06   (powered boost, after friction)
	 * A drift at ANY tick fails; the trace passing is the recorded verdict
	 * that cart speed is NOT divergent and no tuning change is warranted.
	 * ===================================================================== */
	{
		clear_carts();
		const int TZ = 13; /* fresh row (z=12 belongs to the launcher fixture);
							  the whole track stays inside resident chunk (0,0) */
		for(int x = 1; x <= 15; x++) {
			set(x, YF, TZ, BLOCK_STONE, 0);
			force_rail(x, RY, TZ, BLOCK_POWERED_RAIL, 0x8 | 1);
		}
		assert(place_cart_on(3, RY, TZ));
		struct entity* cart = only_minecart();
		assert(server_local_test_tick_entities(&s)); /* settle placement */
		cart = only_minecart();
		cart->data.minecart.speed = 0.10f;
		cart->data.minecart.hx = 1;
		cart->data.minecart.hz = 0;
		float ref = 0.10f;
		double ref_x = cart->pos[0];
		/* 24 ticks: enough to saturate the 0.4 displacement clamp while the
		 * cart stays on the 15-block in-chunk track (~10.4 blocks total). */
		for(int t = 0; t < 24; t++) {
			assert(server_local_test_tick_entities(&s));
			cart = only_minecart();
			/* Reference recurrence, decompile order, float arithmetic. */
			float disp = ref;
			if(disp > 0.4f)
				disp = 0.4f;
			if(disp < -0.4f)
				disp = -0.4f;
			ref_x += disp;
			ref *= BETA_EMPTY_FRICTION;
			if(fabsf(ref) > 0.01f)
				ref += BETA_POWERED_BOOST;
			near_eq(cart->data.minecart.speed, ref,
					"trace: per-tick speed must match the Beta recurrence");
			assert(fabs(cart->pos[0] - ref_x) < 0.05
				   && "trace: per-tick displacement must track the clamped "
					  "reference position");
		}
		/* The Beta signature: stored speed converges toward 0.06/(1-0.96)=1.5
		 * while the DISPLACEMENT saturates at 0.4 blocks/tick (8 m/s). */
		assert(cart->data.minecart.speed > 0.9f);
		printf("  2c: 24-tick powered fixed-track trace matches Beta -- "
			   "verdict: NOT divergent, no tuning change\n");
	}

	/* =====================================================================
	 * (batch 2b) PLAYER-TO-CART NUDGE -- Beta EntityMinecart.applyEntity-
	 * Collision: a player overlapping a parked cart shoves it away
	 * (normalize * min(1/d,1) * 0.1 * 0.5 projected on the heading) and takes
	 * the quarter counter-shove on the server knockback velocity. A player
	 * clear of the cart moves nothing.
	 * ===================================================================== */
	{
		clear_carts();
		/* (EX,RY,EZ) already holds plain flat-EW rail from the seed-heading
		 * section above -- re-forcing it would be an idempotent write, which
		 * the silent setter (correctly) reports as no-change. */
		assert(place_cart_on(EX, RY, EZ));
		struct entity* cart = only_minecart();
		assert(server_local_test_tick_entities(&s));
		cart = only_minecart();
		cart->data.minecart.speed = 0.0f;
		cart->data.minecart.hx = 1;
		cart->data.minecart.hz = 0;

		/* Player standing clear: nothing moves. */
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.x = EX + 6.0;
		s.player.y = RY + PLAYER_EYE_HEIGHT;
		s.player.z = EZ + 0.5;
		s.player.vel_x = s.player.vel_z = 0.0f;
		assert(server_local_test_tick_entities(&s));
		cart = only_minecart();
		assert(cart->data.minecart.speed == 0.0f
			   && "a clear player must not move a parked cart");

		/* Player overlapping from the -X side: the cart is shoved +X
		 * (speed * hx > 0) and the player picks up the -X quarter shove. */
		s.player.x = cart->pos[0] - 0.5;
		s.player.z = cart->pos[2];
		s.player.y = RY + PLAYER_EYE_HEIGHT;
		assert(server_local_test_tick_entities(&s));
		cart = only_minecart();
		assert(cart->data.minecart.speed * (float)cart->data.minecart.hx > 0.0f
			   && "an overlapping player must shove the cart AWAY (+X here)");
		assert(s.player.vel_x < 0.0f
			   && "the player takes the quarter counter-shove (-X here)");
		printf("  2b: player-to-cart nudge shoves the cart away + quarter "
			   "counter-shove OK\n");
	}

	printf("test_minecart_rail_beta: powered-rail sign/direction semantics "
		   "match Beta 1.7.3 OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
