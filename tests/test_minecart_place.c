/*
	Beta 1.7.3 minecart placement + rail-settle golden (the "hover" fix).

	Ground truth (decompile 740c583):
	  ItemMinecart.onItemUse -> spawns an EntityMinecart(world, railX+.5, railY+.5,
	  railZ+.5, 0); the coordinate constructor calls setPosition(y + yOffset)
	  with yOffset = height/2 = .35, so the cart SPAWNS at railY + 0.85.
	  EntityMinecart.onUpdate's rail branch then pins posY EVERY tick, even at
	  zero speed: posY is set to the rail cell y up front, and the branch's final
	  setPosition(posX, the getPos Y, posZ) uses getPos()'s
	  track height, which on every flat-profile tile (straights 0/1, curves
	  6-9: both MATRIX rows carry dy 0) is railY + 0.5. The TRUE Beta resting
	  posY of a cart on a flat rail at cell y = railY is therefore
	  railY + 0.5 (bbox bottom railY + 0.15), reached on the FIRST tick after
	  placement -- a placed cart must NOT hover at its railY + 0.85 spawn
	  height. ItemMinecart accepts ANY rail (BlockRail.isRailBlock: rail,
	  powered rail, detector rail) and nothing else.

	Drives the REAL paths end to end: item_get(ITEM_MINECART)->onItemPlace over
	live server_world block_infos -> server_local_spawn_minecart -> the
	production entity pass (server_local_test_tick_entities) running the real
	minecart_server_tick.
*/

#include <assert.h>
#include <math.h>
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_minecart_place_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 /* stone floor */
#define RY 101 /* rail plane */

extern struct item item_minecart;

static struct server_local s;

static void assert_render_pose(float actual, float expected) {
	assert(fabsf(actual - expected) < 0.0001f);
}

static void test_rail_render_pose(void) {
	struct minecart_rail_pose pose;
	entity_minecart_rail_pose(BLOCK_AIR, 0xF, &pose);
	assert_render_pose(pose.yaw_deg, 0.0f);
	assert_render_pose(pose.pitch_deg, 0.0f);
	assert_render_pose(pose.y_offset, 0.0f);

	/* Detector rails are powerable BlockRail instances just like powered
	 * rails. Their bit 8 is state, not shape, and they must not fall through
	 * to the renderer's default north/south pose. */
	entity_minecart_rail_pose(BLOCK_DETECTOR_RAIL, 0x8 | 2, &pose);
	assert_render_pose(pose.yaw_deg, 90.0f);
	assert_render_pose(pose.pitch_deg, 45.0f);
	assert_render_pose(pose.y_offset, 0.5f);

	entity_minecart_rail_pose(BLOCK_DETECTOR_RAIL, 0x8 | 1, &pose);
	assert_render_pose(pose.yaw_deg, 90.0f);
	assert_render_pose(pose.pitch_deg, 0.0f);

	/* Bit 8 remains the southwest CURVE for a plain rail. */
	entity_minecart_rail_pose(BLOCK_RAIL, 8, &pose);
	assert_render_pose(pose.yaw_deg, -135.0f);
	assert_render_pose(pose.pitch_deg, 0.0f);
}

/* RPC drain thread: keeps the client inbox empty so the server-side spawn /
 * set_block sends never block (same harness as the other env tests). */
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

static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) :
														   -1;
}

/* Place ITEM_MINECART on the block at (x,y,z) through the production item
 * dispatch shape (server_local's SRPC_BLOCK_PLACE handler builds exactly these
 * two block_infos before calling onItemPlace). Returns the handler's verdict. */
static bool place_cart_on(int x, int y, int z) {
	struct block_data blk_on, blk_where;
	assert(server_world_get_block(&s.world, x, y, z, &blk_on));
	assert(server_world_get_block(&s.world, x, y + 1, z, &blk_where));
	struct block_info on = {
		.block = &blk_on, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct block_info where = {
		.block = &blk_where, .neighbours = NULL, .x = x, .y = y + 1, .z = z};
	struct item_data held
		= {.id = ITEM_MINECART, .durability = 0, .count = 1};
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

int main(void) {
	test_fixture_dir("test_minecart_place_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	test_rail_render_pose();

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

	/* Fixture: a lone flat NS rail on stone. */
	const int X = 4, Z = 4;
	set(X, YF, Z, BLOCK_STONE, 0);
	set(X, RY, Z, BLOCK_RAIL, 0);
	assert(getmeta(X, RY, Z) == 0); /* flat NS -- the fixture must stay flat */

	/* NOT a rail -> Beta ItemMinecart refuses (isRailBlock false for stone). */
	assert(!place_cart_on(X, YF, Z));
	assert(dict_entity_size(s.entities) == 0);

	/* Real placement path. Spawn height is Beta's railY + 0.85: ItemMinecart
	 * passes railY + .5 and the coordinate ctor adds yOffset .35. Compare
	 * against the production spawn-position helper (identical float ops). */
	assert(place_cart_on(X, RY, Z));
	assert(dict_entity_size(s.entities) == 1);
	struct entity* cart = only_minecart();
	vec3 expected_spawn;
	struct block_info rail_info = {.x = X, .y = RY, .z = Z};
	item_minecart_spawn_position(&rail_info, expected_spawn);
	assert(expected_spawn[1] == (float)RY + 0.5F + 0.35F);
	assert(cart->pos[0] == expected_spawn[0]
		   && cart->pos[1] == expected_spawn[1]
		   && cart->pos[2] == expected_spawn[2]);
	assert(cart->pos[0] == (float)X + 0.5F && cart->pos[2] == (float)Z + 0.5F);
	assert(cart->data.minecart.speed == 0.0F);
	float spawn_y = cart->pos[1];

	/* ONE production entity tick settles the cart to the decompile-derived
	 * resting height railY + 0.5 -- Beta's rail branch pins posY even at zero
	 * speed; the cart may not keep hovering at spawn height. pos_old keeps the
	 * spawn height for this tick's interpolation (Beta prevPosY). */
	assert(server_local_test_tick_entities(&s));
	assert(cart->pos_old[1] == spawn_y);
	assert(cart->pos[1] == (float)RY + 0.5F);
	assert(cart->pos[0] == (float)X + 0.5F && cart->pos[2] == (float)Z + 0.5F);

	/* The resting height is a fixed point: further ticks hold railY + 0.5
	 * exactly, at rest, at the rail center. Beta bbox bottom = posY - yOffset
	 * = railY + 0.15. */
	for(int k = 0; k < 10; k++)
		assert(server_local_test_tick_entities(&s));
	assert(cart->pos[1] == (float)RY + 0.5F);
	assert(cart->pos_old[1] == (float)RY + 0.5F);
	assert(cart->pos[0] == (float)X + 0.5F && cart->pos[2] == (float)Z + 0.5F);
	assert(cart->data.minecart.speed == 0.0F);
	struct AABB box;
	assert(cart->getBoundingBox(cart, &box) == 1);
	assert(fabsf(box.y1 - ((float)RY + 0.15F)) < 1.0e-5F);
	assert(fabsf(box.y2 - ((float)RY + 0.85F)) < 1.0e-5F);

	/* Beta isRailBlock also accepts the powered rail (railPowered). The same
	 * first-tick settle applies: every flat-profile getPos() height is
	 * railY + 0.5 regardless of rail type ("var11 &= 7" strips the powered
	 * bit before shape logic). Y stays pinned on later ticks too, whatever
	 * the (still-TODO) powered-rail boost does along the track. */
	const int PX = 10, PZ = 10;
	set(PX, YF, PZ, BLOCK_STONE, 0);
	set(PX, RY, PZ, BLOCK_POWERED_RAIL, 0);
	{ /* server_entity_alloc heap-owns the cart: free before erasing */
		uint32_t id = cart->id;
		free(cart);
		dict_entity_erase(s.entities, id);
	}
	assert(place_cart_on(PX, RY, PZ));
	struct entity* powered_cart = only_minecart();
	assert(powered_cart->pos[1] == (float)RY + 0.5F + 0.35F);
	assert(server_local_test_tick_entities(&s));
	assert(powered_cart->pos[1] == (float)RY + 0.5F);
	for(int k = 0; k < 3; k++)
		assert(server_local_test_tick_entities(&s));
	assert(powered_cart->pos[1] == (float)RY + 0.5F);
	{
		uint32_t id = powered_cart->id;
		free(powered_cart);
		dict_entity_erase(s.entities, id);
	}

	/* Detector rail (id 28): Beta BlockRail.isRailBlock accepts it too, and
	 * getIsPowered() is true for it, so the same bit-8 strip + railY + 0.5
	 * settle apply. Placement parity is pinned here; cart TRAVERSAL of the
	 * detector rail remains the engine's declared TODO.
	 * [740c583: BlockRail.isRailBlock/.getIsPowered] */
	set(PX, RY, PZ, BLOCK_DETECTOR_RAIL, 0);
	assert(place_cart_on(PX, RY, PZ));
	struct entity* detector_cart = only_minecart();
	assert(detector_cart->pos[1] == (float)RY + 0.5F + 0.35F);
	assert(server_local_test_tick_entities(&s));
	assert(detector_cart->pos[1] == (float)RY + 0.5F);
	{
		uint32_t id = detector_cart->id;
		free(detector_cart);
		dict_entity_erase(s.entities, id);
	}

	/* P1(a) boost gate + Beta BRAKE (EntityMinecart.java:219-222, :262-273, verified
	 * verbatim at 740c583: on railPowered, var12 = (meta & 8) != 0 boosts, var13 = !var12
	 * BRAKES -- speed < 0.03 stops dead, else halves per tick). Both read wrong before
	 * the fix: the boost fired on block TYPE alone and no brake existed, so the
	 * maintainer's looping cart was accelerated by an UNPOWERED rail. */
	set(PX, RY, PZ, BLOCK_POWERED_RAIL, 0x8 | 1); // powered rail
	assert(place_cart_on(PX, RY, PZ));
	struct entity* boost_cart = only_minecart();
	/* CORRECTED 2026-07-31. This used to start the cart at speed 0 and assert
	 * only `speed > 0`, which ENCODED THE BUG: the implementation launched every
	 * resting cart at an unconditional +0.05, and this assertion passed because
	 * of it. Beta does no such thing -- EntityMinecart.onUpdate:402-414 gives a
	 * resting cart a WALL-DIRECTED +-0.02 and, with no wall on either side, has
	 * no else-branch at all, so the cart simply does not move. This fixture has
	 * open track on both sides, so the Beta-correct outcome for a RESTING cart
	 * here is NO motion; a MOVING cart is what the boost applies to. Both are
	 * asserted, and the signed/wall-kick cases live in
	 * tests/test_minecart_rail_beta.c. */
	boost_cart->data.minecart.speed = 0.0F;
	assert(server_local_test_tick_entities(&s));
	assert(boost_cart->data.minecart.speed == 0.0F
		   && "a resting cart with NO adjacent wall must NOT be launched "
			  "(Beta has no else-branch; it leaves motion untouched)");
	boost_cart = only_minecart();
	boost_cart->data.minecart.speed = 0.05F; /* now MOVING */
	assert(server_local_test_tick_entities(&s));
	boost_cart = only_minecart();
	assert(boost_cart->data.minecart.speed > 0.05F
		   && "a POWERED rail must boost a MOVING cart (Beta :398-401)");
	{
		uint32_t id = boost_cart->id;
		free(boost_cart);
		dict_entity_erase(s.entities, id);
	}
	set(PX, RY, PZ, BLOCK_POWERED_RAIL, 1); // same rail, bit 8 CLEAR
	assert(place_cart_on(PX, RY, PZ));
	struct entity* brake_cart = only_minecart();
	brake_cart->data.minecart.speed = 0.1F;
	assert(server_local_test_tick_entities(&s));
	/* one tick: brake halves (0.05) then Beta empty-cart friction (*0.96) -> 0.048, decisively below
	 * any boosted value and above the stop threshold. */
	assert(brake_cart->data.minecart.speed < 0.06F
		   && brake_cart->data.minecart.speed > 0.03F
		   && "an UNPOWERED powered rail must BRAKE, not boost -- Beta's stopper");
	for(int k = 0; k < 3; k++)
		assert(server_local_test_tick_entities(&s));
	assert(brake_cart->data.minecart.speed == 0.0F
		   && "below 0.03 the brake stops the cart dead (EntityMinecart.java:262-273)");
	{
		uint32_t id = brake_cart->id;
		free(brake_cart);
		dict_entity_erase(s.entities, id);
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	dict_entity_clear(s.entities);
	string_clear(level);
	printf("test_minecart_place: OK (Beta railY+0.85 spawn settles to the "
		   "railY+0.5 resting posY on the first tick via the real item-place "
		   "+ entity-tick paths; stone refused, powered + detector rails "
		   "accepted)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
