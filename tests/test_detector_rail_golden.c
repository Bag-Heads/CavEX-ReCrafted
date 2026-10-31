/*
	Golden test for CR-05 (audit docs/audit_2026-08-20_code_review.md): the detector rail
	never set metadata bit 8 for an occupying minecart and never emitted redstone power.
	Authored BEFORE the implementation, per the beta-tick-golden skill's discipline.

	GROUND TRUTH -- decompile 740c583, BlockDetectorRail.java (verified this
	session; behavioral summary of the transcribed methods):

	The constructor enables tick-on-load; tickRate specifies 20 ticks and
	canProvidePower enables redstone connectivity. onEntityCollidedWithBlock
	rechecks occupancy only on the server and only while metadata bit 8 is clear.
	updateTick also runs only on the server, rechecking only while bit 8 is set.

	isPoweringTo reports bit 8 independently of side. isIndirectlyPoweringTo
	requires bit 8 and side 1. Side numbering is 0=-Y, 1=+Y, 2=-Z, 3=+Z,
	4=-X, 5=+X (redstone_power.h): side 1 means the rail is above the querying
	cell, so only the supporting block below receives that power.

	setStateIfMinecartInteractsWithRail senses minecarts in a box inset by
	0.125 on both horizontal axes, from rail Y through Y+0.25. Occupancy
	sets bit 8; absence clears it while preserving the lower three bits.
	On either state transition, metadata notification comes first, followed
	by neighbor notification at the rail, then at the block one cell below,
	then marking the rail cell dirty. Any occupied recheck schedules another
	update 20 ticks later; an empty recheck does not reschedule.




































	Cross-checked: AxisAlignedBB.intersectsWith (740c583) is the STRICT >/< test (a shared
	face/edge is NOT an intersection) -- aabb_intersection_strict (source/block/aabb.c) is
	already the verbatim transcription of that method, cited there.

	COVERAGE (two independent sections):

	  SECTION A -- pure redstone_power.h dispatch, no server (mirrors
	  tests/test_redstone_wire_render_connections.c's stub-grid style): the isPoweringTo
	  truth table (side-independent, powered vs unpowered) and the isIndirectlyPoweringTo
	  truth table (all 6 faces, only side==1 / "the block below" true), plus the
	  can_provide_power wire-connectivity fix (BlockDetectorRail.canProvidePower()==true).

	  SECTION B -- full production server (mirrors tests/test_minecart_rail_beta.c and
	  tests/test_powered_rail_power.c): a real minecart entity settling onto a real
	  detector rail drives the actual arrival hook
	  (block_detector_rail_on_minecart_tick, called from entity_minecart.c's
	  minecart_server_tick) and the actual scheduled-tick recheck
	  (block_detector_rail's onWorldTick, tick_delay=20) through the REAL scheduled-tick
	  queue -- proving the tick-by-tick cadence, not just the end state:
	    1. arrival sets bit 8 on the very tick the cart settles, and the "below" notify
	       actually reaches a wire two cells down through the rail's support cube (the
	       classic "power the block a wire sits under" pass-through), lighting it to 15;
	    2. the bit stays set every one of the 19 ticks before the first scheduled recheck;
	    3. the tick-20 recheck (cart still present) leaves the bit set and re-arms another
	       recheck 20 ticks later -- proving the self-sustaining while-occupied chain, not
	       a one-shot;
	    4. removing the cart leaves the bit set until the NEXT scheduled recheck (tick 40),
	       not the instant the cart is gone -- pinning the exact Beta latency;
	    5. the tick-40 recheck (cart now absent) clears the bit, the wire de-powers back to
	       0 through the same below-notify path, and NO further reschedule occurs.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "block/redstone_power.h"
#include "block/redstone_server.h"
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

/* =========================================================================
   SECTION A -- pure power-dispatch truth table (redstone_power.c), no server.
   ========================================================================= */

#define AG 16 /* stub grid extent */
static int a_id[AG][AG][AG];
static int a_meta[AG][AG][AG];

static bool a_in(int x, int y, int z) {
	return x >= 0 && x < AG && y >= 0 && y < AG && z >= 0 && z < AG;
}
static int a_get_type(void* c, int x, int y, int z) {
	(void)c;
	return a_in(x, y, z) ? a_id[x][y][z] : -1;
}
static int a_get_meta(void* c, int x, int y, int z) {
	(void)c;
	return a_in(x, y, z) ? a_meta[x][y][z] : 0;
}
static bool a_is_normal_cube(void* c, int x, int y, int z) {
	(void)c;
	return a_in(x, y, z) && a_id[x][y][z] == BLOCK_STONE;
}
static struct redstone_power_world AW = {
	.ctx = NULL,
	.get_type = a_get_type,
	.get_meta = a_get_meta,
	.is_normal_cube = a_is_normal_cube,
};

static void a_clear(void) {
	memset(a_id, 0, sizeof(a_id));
	memset(a_meta, 0, sizeof(a_meta));
}

#define RX 8
#define RY 8
#define RZ 8

static void section_a(void) {
	a_clear();
	a_id[RX][RY][RZ] = BLOCK_DETECTOR_RAIL;
	a_meta[RX][RY][RZ] = 0; /* shape 0 (NS), bit 8 clear */

	/* --- isPoweringTo (STRONG): side-independent, false while unpowered ---------------- */
	for(int side = 0; side <= 5; side++)
		assert(!redstone_is_indirectly_providing_power_to(&AW, RX, RY, RZ, side, true)
			   && "unpowered detector rail must not strongly power any face");

	/* --- isIndirectlyPoweringTo (WEAK): false on all 6 faces while unpowered ------------
	   Probe from each of the rail's 6 neighbour cells via the exported WEAK aggregate
	   (redstone_is_getting_powered): probe = rail - offset[i], which internally queries
	   the rail with the SAME side numbering redstone_is_getting_powered's own body uses
	   (0=-Y,1=+Y,2=-Z,3=+Z,4=-X,5=+X) -- e.g. the probe BELOW the rail queries it with
	   side=1, exactly BlockDetectorRail.isIndirectlyPoweringTo's "side==1" case. */
	struct { int dx, dy, dz, side; const char* name; } faces[6] = {
		{0, -1, 0, 1, "below (side=1, the ONLY true face when powered)"},
		{0, +1, 0, 0, "above"},
		{0, 0, -1, 3, "north"},
		{0, 0, +1, 2, "south"},
		{-1, 0, 0, 5, "west"},
		{+1, 0, 0, 4, "east"},
	};
	for(int i = 0; i < 6; i++)
		assert(!redstone_is_getting_powered(&AW, RX + faces[i].dx, RY + faces[i].dy,
											RZ + faces[i].dz, true)
			   && "unpowered detector rail must not weakly power any neighbour");

	/* --- power on: bit 8 set ------------------------------------------------------------ */
	a_meta[RX][RY][RZ] = 0x8;

	for(int side = 0; side <= 5; side++)
		assert(redstone_is_indirectly_providing_power_to(&AW, RX, RY, RZ, side, true)
			   && "a powered detector rail must strongly power EVERY face -- side is not "
				  "consulted by BlockDetectorRail.isPoweringTo");

	assert(redstone_is_getting_powered(&AW, RX, RY - 1, RZ, true)
		   && "the block directly below a powered detector rail must be weakly powered "
			  "(side==1)");
	for(int i = 1; i < 6; i++) /* every OTHER face must stay false */
		assert(!redstone_is_getting_powered(&AW, RX + faces[i].dx, RY + faces[i].dy,
											RZ + faces[i].dz, true)
			   && "a powered detector rail must NOT weakly power any face except below");

	/* --- can_provide_power (wire connectivity): BlockDetectorRail.canProvidePower()==true,
	   consumed by BlockRedstoneWire.isPowerProviderOrWire -- a wire beside a detector rail
	   (any power state) must treat it as a connection, matching
	   tests/test_redstone_wire_render_connections.c's own style. --------------------------- */
	assert(redstone_id_can_provide_power(RSP_DETECTOR_RAIL_ID)
		   && "redstone_id_can_provide_power must include the detector rail (CR-05)");
	a_clear();
	a_id[RX][RY][RZ] = BLOCK_REDSTONE_WIRE;
	a_id[RX + 1][RY][RZ] = BLOCK_DETECTOR_RAIL; /* east neighbour, unpowered */
	bool conn[4];
	redstone_wire_connections(&AW, RX, RY, RZ, conn);
	assert(conn[1]
		   && "a wire must connect to an adjacent detector rail (canProvidePower()==true) "
			  "regardless of its power state");

	printf("  [A] isPoweringTo/isIndirectlyPoweringTo truth table + wire connectivity: OK\n");
}

/* =========================================================================
   SECTION B -- full production occupancy / scheduled-tick / notify test.
   ========================================================================= */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_detector_rail_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 90			/* rail support (also the "below" notify target)   */
#define RAIL_Y (YF + 1) /* the detector rail itself                          */
#define WIRE_Y (YF - 1) /* wire sitting one level below the rail's support   */
#define WIRE_SUPPORT_Y (YF - 2)
#define BX 10
#define BZ 10

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
	assert(server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
}

static uint8_t meta_at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b.metadata;
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b.type;
}

static bool place_cart_on(int x, int y, int z) {
	struct block_data blk_on, blk_where;
	assert(server_world_get_block(&s.world, x, y, z, &blk_on));
	assert(server_world_get_block(&s.world, x, y + 1, z, &blk_where));
	struct block_info on = {.block = &blk_on, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct block_info where
		= {.block = &blk_where, .neighbours = NULL, .x = x, .y = y + 1, .z = z};
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

static void remove_the_minecart(void) {
	struct entity* e = only_minecart();
	dict_entity_erase(s.entities, e->id);
}

static void world_tick(void) { server_world_tick(&s.world, &s); }

static void section_b(void) {
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0; /* unlimited: this golden pins exact tick counts */

	/* Beta gates scheduled updates at radius 8 and entity updates at radius 32.
	 * BX/BZ=10 therefore needs chunks -2..2 on each axis for the production
	 * cart tick below; the larger flat neighborhood preserves the same rail
	 * geometry while satisfying both activation preconditions. */
	struct server_chunk* sc = NULL;
	for(int lcx = -2; lcx <= 2; lcx++)
		for(int lcz = -2; lcz <= 2; lcz++) {
			sc = NULL;
			assert(server_world_load_chunk(&s.world, lcx, lcz, &sc) && sc);
		}

	/* Vertical stack: wire's own support -> wire -> the rail's support (the "below" notify
	   TARGET) -> the detector rail. */
	set(BX, WIRE_SUPPORT_Y, BZ, BLOCK_STONE, 0);
	set(BX, WIRE_Y, BZ, BLOCK_REDSTONE_WIRE, 0);
	set(BX, YF, BZ, BLOCK_STONE, 0);
	set(BX, RAIL_Y, BZ, BLOCK_DETECTOR_RAIL, 0);

	assert(!(meta_at(BX, RAIL_Y, BZ) & 0x8) && "fixture must start unpowered");
	assert(meta_at(BX, WIRE_Y, BZ) == 0 && "fixture wire must start unpowered");
	assert(!redstone_server_indirect_power(&s, BX, RAIL_Y, BZ, 4, true));

	/* --- (1) ARRIVAL: settle the cart onto the rail with ONE production entity tick ----- */
	assert(place_cart_on(BX, RAIL_Y, BZ));
	assert(server_local_test_tick_entities(&s));
	assert(type_at(BX, RAIL_Y, BZ) == BLOCK_DETECTOR_RAIL
		   && "the cart's own settle must not disturb the rail cell");
	assert((meta_at(BX, RAIL_Y, BZ) & 0x8)
		   && "bit 8 must be set the SAME tick the cart settles onto the rail");
	assert(redstone_server_indirect_power(&s, BX, RAIL_Y, BZ, 4, true)
		   && "a powered detector rail must strongly power every face (production glue)");
	assert(meta_at(BX, WIRE_Y, BZ) == 15
		   && "the 'below' notify must reach the wire two cells down through the rail's "
			  "support cube (BlockDetectorRail.notifyBlocksOfNeighborChange(x,y-1,z,...))");

	/* --- (2)+(3) the 20-tick recheck cadence WHILE occupied ----------------------------- */
	assert(s.world.current_tick == 0);
	for(int t = 1; t <= 19; t++) {
		world_tick();
		assert((meta_at(BX, RAIL_Y, BZ) & 0x8)
			   && "bit 8 must not clear before the 20-tick scheduled recheck");
		assert(meta_at(BX, WIRE_Y, BZ) == 15);
	}
	world_tick(); /* current_tick == 20: the FIRST scheduled recheck fires */
	assert(s.world.current_tick == 20);
	assert((meta_at(BX, RAIL_Y, BZ) & 0x8)
		   && "the tick-20 recheck must leave bit 8 set while the cart is still present");
	assert(meta_at(BX, WIRE_Y, BZ) == 15);

	/* --- (4) cart leaves: bit 8 must persist until the NEXT recheck (tick 40), not clear
	   the instant the entity is gone -------------------------------------------------------- */
	remove_the_minecart();
	for(int t = 21; t <= 39; t++) {
		world_tick();
		assert((meta_at(BX, RAIL_Y, BZ) & 0x8)
			   && "bit 8 must stay set until the SCHEDULED recheck notices the cart is gone "
				  "-- Beta has no immediate departure hook");
		assert(meta_at(BX, WIRE_Y, BZ) == 15);
	}

	/* --- (5) the tick-40 recheck (cart absent) clears the bit and de-powers the wire ---- */
	world_tick();
	assert(s.world.current_tick == 40);
	assert(!(meta_at(BX, RAIL_Y, BZ) & 0x8)
		   && "the tick-40 recheck must clear bit 8 once no cart remains in the box");
	assert(!redstone_server_indirect_power(&s, BX, RAIL_Y, BZ, 4, true));
	assert(meta_at(BX, WIRE_Y, BZ) == 0
		   && "the clear-path 'below' notify must de-power the wire back to 0");

	/* --- no dangling reschedule: further ticks change nothing further ------------------- */
	for(int t = 41; t <= 60; t++) {
		world_tick();
		assert(!(meta_at(BX, RAIL_Y, BZ) & 0x8));
		assert(meta_at(BX, WIRE_Y, BZ) == 0);
	}

	printf("  [B] arrival + 20-tick recheck cadence + departure, through the real scheduled-"
		   "tick queue: OK\n");
}

int main(void) {
	test_fixture_dir("test_detector_rail_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_detector_rail_golden: CR-05 detector rail occupancy + redstone (Beta "
		   "1.7.3 BlockDetectorRail, 740c583)\n");

	section_a();

	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	section_b();

	g_drain_run = 0;
	printf("test_detector_rail_golden: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
