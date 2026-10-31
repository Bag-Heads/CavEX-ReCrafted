/*
	FAITHFULNESS env test (Beta 1.7.3, decompile 740c583): the canBlockStay/support-drop and
	growth-gate residuals fixed in the Lane-B batch, over a live superflat server_world.

	PRESSURE PLATE (BlockPressurePlate.onNeighborBlockChange): the plate drops when the block
	below stops being a normal cube -- and a PRESSED plate that drops stops powering the wire
	beside it (the float + phantom-power bug).

	CACTUS/REED growth gate (BlockCactus/BlockReed.updateTick): the ENTIRE growth body --
	including the metadata counter -- is gated on AIR ABOVE. A blocked meta-15 cactus/reed keeps
	its ready-to-grow state (the old code incremented unconditionally, wrapping 15 -> 0), and
	grows the moment the ceiling is removed. At meta 15 with air above: new segment + counter
	reset to 0.

	CACTUS canBlockStay (BlockCactus.onNeighborBlockChange, NEW): a solid-material horizontal
	neighbour pops the cactus; the popped base also pops the segment above it (cascade).

	FLOWER/ROSE (BlockFlower.onNeighborBlockChange -> checkFlowerChange, NEW): the flower pops
	when its soil (grass/dirt/farmland -- farmland was missing from the port) is removed; the
	same soil set now also gates placement.
*/

#include <assert.h>
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
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_block_stay_growth_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define Y 101

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
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) :
															 -1;
}

static struct block_data g_bd;
static struct block_info info_at(int x, int y, int z) {
	assert(server_world_get_block(&s.world, x, y, z, &g_bd));
	return (struct block_info) {
		.block = &g_bd, .neighbours = NULL, .x = x, .y = y, .z = z};
}
// Drive one Beta updateTick (the random-tick handler) directly on the block at (x,y,z).
static void random_tick_at(int x, int y, int z) {
	struct block_info i = info_at(x, y, z);
	assert(blocks[i.block->type] && blocks[i.block->type]->onRandomTick);
	blocks[i.block->type]->onRandomTick(&s, &i);
}

int main(void) {
	test_fixture_dir("test_block_stay_growth_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	// ================== PRESSURE PLATE support-drop ==================
	set(2, YF, 3, BLOCK_STONE, 0);
	set(3, YF, 3, BLOCK_STONE, 0);
	set(3, Y, 3, BLOCK_REDSTONE_WIRE, 0);
	set(2, Y, 3, BLOCK_STONE_PRESSURE_PLATE, 1); // PRESSED plate beside the wire
	assert(getmeta(3, Y, 3) == 15); // BlockPressurePlate.isPoweringTo: meta>0
	set(2, YF, 3, BLOCK_AIR, 0);	// mine the support
	assert(get(2, Y, 3) == BLOCK_AIR); // the plate popped (no float)
	assert(getmeta(3, Y, 3) == 0);	   // ...and no phantom power

	// ================== CACTUS growth gate + canBlockStay ==================
	set(5, YF, 5, BLOCK_SAND, 0);
	set(5, Y, 5, BLOCK_CACTUS, 15);		// ready to grow
	set(5, Y + 1, 5, BLOCK_STONE, 0);	// ...but blocked above
	random_tick_at(5, Y, 5);
	assert(get(5, Y, 5) == BLOCK_CACTUS);
	assert(getmeta(5, Y, 5) == 15);		// counter UNTOUCHED (old code wrapped to 0)
	assert(get(5, Y + 1, 5) == BLOCK_STONE);
	set(5, Y + 1, 5, BLOCK_AIR, 0);		// remove the ceiling
	assert(get(5, Y, 5) == BLOCK_CACTUS); // survives its own neighbour change
	random_tick_at(5, Y, 5);
	assert(get(5, Y + 1, 5) == BLOCK_CACTUS); // grew immediately
	assert(getmeta(5, Y, 5) == 0);			  // counter reset with the growth
	// Solid neighbour pops the base -- and the orphaned segment above cascades.
	set(4, Y, 5, BLOCK_STONE, 0);
	assert(get(5, Y, 5) == BLOCK_AIR);
	assert(get(5, Y + 1, 5) == BLOCK_AIR);

	// ================== REED growth gate ==================
	set(8, YF, 5, BLOCK_DIRT, 0);
	set(7, YF, 5, BLOCK_WATER_STILL, 0); // adjacent water for the base
	set(8, Y, 5, BLOCK_REED, 15);
	set(8, Y + 1, 5, BLOCK_STONE, 0);
	random_tick_at(8, Y, 5);
	assert(get(8, Y, 5) == BLOCK_REED);
	assert(getmeta(8, Y, 5) == 15); // counter UNTOUCHED under a ceiling
	set(8, Y + 1, 5, BLOCK_AIR, 0);
	random_tick_at(8, Y, 5);
	assert(get(8, Y + 1, 5) == BLOCK_REED); // grew
	assert(getmeta(8, Y, 5) == 0);

	// ================== FLOWER / ROSE canBlockStay + farmland soil ==================
	set(2, YF, 8, BLOCK_DIRT, 0);
	set(2, Y, 8, BLOCK_FLOWER, 0);
	assert(get(2, Y, 8) == BLOCK_FLOWER);
	set(2, YF, 8, BLOCK_AIR, 0); // soil gone
	assert(get(2, Y, 8) == BLOCK_AIR); // popped (BlockFlower.checkFlowerChange)

	set(3, YF, 8, BLOCK_DIRT, 0);
	set(3, Y, 8, BLOCK_ROSE, 0);
	set(3, YF, 8, BLOCK_AIR, 0);
	assert(get(3, Y, 8) == BLOCK_AIR);

	// Farmland is valid flower soil (Beta canThisPlantGrowOnThisBlockID incl. tilledField;
	// the port only allowed dirt/grass).
	set(4, YF, 8, BLOCK_FARMLAND, 0);
	{
		struct block_info on = info_at(4, YF, 8);
		struct block_info where = {
			.block = NULL, .neighbours = NULL, .x = 4, .y = Y, .z = 8};
		struct item_data it = {.id = BLOCK_FLOWER, .durability = 0, .count = 1};
		assert(block_flower.block_item.onItemPlace(&s, &s.player, &it, &where, &on,
												   SIDE_TOP));
	}
	assert(get(4, Y, 8) == BLOCK_FLOWER);

	// LIGHT clause (the other canBlockStay direction): a flower sealed in a dark box
	// (sky 0, torch 0 -- light < 8) pops on its random tick. The daylight flowers above
	// (sky 15) already prove the >=8 branch KEEPS.
	set(6, YF, 8, BLOCK_DIRT, 0);
	set(5, Y, 8, BLOCK_STONE, 0);
	set(7, Y, 8, BLOCK_STONE, 0);
	set(6, Y, 7, BLOCK_STONE, 0);
	set(6, Y, 9, BLOCK_STONE, 0);
	set(6, Y + 1, 8, BLOCK_STONE, 0);
	set(6, Y, 8, BLOCK_FLOWER, 0);
	assert(get(6, Y, 8) == BLOCK_FLOWER);
	random_tick_at(6, Y, 8);
	assert(get(6, Y, 8) == BLOCK_AIR); // dark: popped

	// ================== CACTUS growth counter + cap + solidity proxy controls ==========
	set(11, YF, 3, BLOCK_SAND, 0);
	set(11, Y, 3, BLOCK_CACTUS, 5); // mid-count, air above
	random_tick_at(11, Y, 3);
	assert(get(11, Y + 1, 3) == BLOCK_AIR); // no growth before 15
	assert(getmeta(11, Y, 3) == 6);			// counter incremented by exactly 1
	// A TORCH beside it (no entity box; Beta Material.circuits, non-solid): survives.
	set(10, YF, 3, BLOCK_STONE, 0);
	set(10, Y, 3, BLOCK_TORCH, 5);
	assert(get(11, Y, 3) == BLOCK_CACTUS);
	set(10, Y, 3, BLOCK_AIR, 0);
	// A PRESSURE PLATE beside it (Beta Material.rock, SOLID despite no entity box -- the
	// id-corrected proxy path): pops the cactus.
	set(12, YF, 3, BLOCK_STONE, 0);
	set(12, Y, 3, BLOCK_STONE_PRESSURE_PLATE, 0);
	assert(get(11, Y, 3) == BLOCK_AIR);

	// Height cap: a full 3-stack's TOP segment at meta 15 with air above must NOT grow a
	// 4th segment (Beta: the whole body is inside the height<3 gate; the counter holds).
	set(11, YF, 6, BLOCK_SAND, 0);
	set(11, Y, 6, BLOCK_CACTUS, 0);
	set(11, Y + 1, 6, BLOCK_CACTUS, 0);
	set(11, Y + 2, 6, BLOCK_CACTUS, 15);
	random_tick_at(11, Y + 2, 6);
	assert(get(11, Y + 3, 6) == BLOCK_AIR);
	assert(getmeta(11, Y + 2, 6) == 15);

	// ================== PLATE press-poll lifecycle (audit #24) ==========================
	// Beta onBlockAdded is a no-op (audit #24), so placing through the REAL item path arms
	// nothing; press comes from the real per-tick arrival trigger (server_world_tick's player
	// feet-cell probe -> block_pressure_plate_on_entity_tick), release only from the
	// tickRate()==20 scheduled recheck (armed by the press itself). Byte-exact tick-by-tick
	// coverage of this cadence (plus mob/item/arrow sensing) lives in
	// tests/test_pressure_plate_golden.c SECTION A; this scene keeps its original
	// regression-guard role: the self-sustaining while-pressed schedule chain must not die on
	// a state change.
	set(2, YF, 6, BLOCK_STONE, 0);
	set(3, YF, 6, BLOCK_STONE, 0);
	set(3, Y, 6, BLOCK_REDSTONE_WIRE, 0);
	{
		struct block_info on = info_at(2, YF, 6);
		struct block_info where = {
			.block = NULL, .neighbours = NULL, .x = 2, .y = Y, .z = 6};
		struct item_data it = {
			.id = BLOCK_STONE_PRESSURE_PLATE, .durability = 0, .count = 1};
		assert(block_stone_pressure_plate.block_item.onItemPlace(&s, &s.player, &it, &where,
																 &on, SIDE_TOP));
	}
	assert(get(2, Y, 6) == BLOCK_STONE_PRESSURE_PLATE);
	assert(getmeta(2, Y, 6) == 0);
	assert(s.world.current_tick == 0);
	// Step ON (footY = player.y - 1.62 must land in [Y, Y+0.1]).
	s.player.x = 2.5;
	s.player.z = 6.5;
	s.player.y = Y + 1.67;
	server_world_tick(&s.world, &s); // tick 1: immediate arrival, no poll delay
	assert(getmeta(2, Y, 6) == 1);	 // pressed
	assert(getmeta(3, Y, 6) == 15); // powering the wire
	// The tickRate()==20 recheck armed by that press must not fire (or clear anything)
	// before tick 21 -- audit #24 fix (b): the pre-fix code re-armed every single tick.
	for(int t = 2; t <= 20; t++) {
		server_world_tick(&s.world, &s);
		assert(getmeta(2, Y, 6) == 1);
		assert(getmeta(3, Y, 6) == 15);
	}
	// Step OFF: the poll must still be alive to release it -- audit #24 fix (c): the recheck
	// reschedules ONLY while pressed, so the very next tick (21, already armed at press time)
	// is exactly when Beta notices the player is gone.
	s.player.x = 1000.0;
	s.player.z = 1000.0;
	assert(s.world.current_tick == 20);
	server_world_tick(&s.world, &s); // tick 21: the scheduled recheck fires
	assert(getmeta(2, Y, 6) == 0); // released (pre-fix: latched pressed forever)
	assert(getmeta(3, Y, 6) == 0);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_block_stay_growth_env: OK (plate support-drop + live press/release poll; "
		   "cactus/reed growth counter persists under a ceiling, +1 per tick, 3-high cap; "
		   "cactus solid-side pop cascades with torch/plate proxy controls; flower/rose pop "
		   "with soil or darkness and accept farmland)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
