/*
	Golden test for Beta 1.7.3 hoe tilling (ItemHoe.onItemUse) via the item onUseOnBlock hook.

	Pins the decompile's gate `(side==0 || above!=0 || block!=grass) && block!=dirt`:
	  - DIRT tills to farmland UNCONDITIONALLY (any face, even with a block above -- the
	    `&& block!=dirt` short-circuits the side/above checks);
	  - GRASS tills only when the clicked face is NOT the bottom AND the cell above is air;
	  - anything else (stone, etc.) -> no-op;
	  - the hoe is NOT consumed (durability damage deferred -- no tool-wear system yet).

	Decompile: 740c583:1.7.3-LTS ItemHoe.onItemUse.
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
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_hoe_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern bool item_hoe_onUseOnBlock(struct server_local*, struct server_player*, struct item_data*,
								  struct block_info*, enum side);

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

static void set(int x, int y, int z, int type) {
	struct block_data expected = {.type = (uint8_t)type, .metadata = 0};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	return blk.type;
}

// 292 == iron hoe; the till logic ignores the item id (no consume/damage yet).
static bool hoe_use(int x, int y, int z, enum side side) {
	struct item_data hoe = {.id = 292, .durability = 0, .count = 1};
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	struct block_info on = {
		.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
	bool used = item_hoe_onUseOnBlock(&s, &s.player, &hoe, &on, side);
	assert(hoe.id == 292 && hoe.count == 1); // hoe never consumed
	return used;
}

static void test_dirt_tills_any_face(void) {
	set(8, 70, 8, BLOCK_DIRT);
	set(8, 71, 8, BLOCK_AIR);
	assert(hoe_use(8, 70, 8, SIDE_TOP) == true);
	assert(type_at(8, 70, 8) == BLOCK_FARMLAND);

	// dirt tills even from the bottom face and even with a block above (Beta quirk).
	set(8, 70, 9, BLOCK_DIRT);
	set(8, 71, 9, BLOCK_STONE);
	assert(hoe_use(8, 70, 9, SIDE_BOTTOM) == true);
	assert(type_at(8, 70, 9) == BLOCK_FARMLAND);
	printf("  dirt: tills to farmland from any face, even with a block above OK\n");
}

static void test_grass_tills_top_air_above(void) {
	set(8, 70, 10, BLOCK_GRASS);
	set(8, 71, 10, BLOCK_AIR);
	assert(hoe_use(8, 70, 10, SIDE_TOP) == true);
	assert(type_at(8, 70, 10) == BLOCK_FARMLAND);
	printf("  grass + top face + air above: tills OK\n");
}

static void test_grass_bottom_face_noop(void) {
	set(8, 70, 11, BLOCK_GRASS);
	set(8, 71, 11, BLOCK_AIR);
	assert(hoe_use(8, 70, 11, SIDE_BOTTOM) == false);
	assert(type_at(8, 70, 11) == BLOCK_GRASS);
	printf("  grass + BOTTOM face: no-op OK\n");
}

static void test_grass_block_above_noop(void) {
	set(8, 70, 12, BLOCK_GRASS);
	set(8, 71, 12, BLOCK_STONE); // not air above
	assert(hoe_use(8, 70, 12, SIDE_TOP) == false);
	assert(type_at(8, 70, 12) == BLOCK_GRASS);
	printf("  grass + block above: no-op OK\n");
}

static void test_non_tillable_noop(void) {
	set(8, 70, 13, BLOCK_STONE);
	assert(hoe_use(8, 70, 13, SIDE_TOP) == false);
	assert(type_at(8, 70, 13) == BLOCK_STONE);
	printf("  stone: no-op OK\n");
}

int main(void) {
	test_fixture_dir("test_hoe_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
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
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	printf("test_hoe:\n");
	test_dirt_tills_any_face();
	test_grass_tills_top_air_above();
	test_grass_bottom_face_noop();
	test_grass_block_above_noop();
	test_non_tillable_noop();

	// Regression guard for the dirt/grass place-consume item dupe: neither block may
	// define a block-level onRightClick. The server dispatch (SRPC_BLOCK_PLACE) calls a
	// block's onRightClick INSTEAD of the held item's onUseOnBlock/onItemPlace+consume,
	// so a shadowing onRightClick here placed a held block without consuming it (a dupe)
	// and pre-empted bonemeal-on-grass. Tilling is the item verb tested above.
	assert(blocks[BLOCK_DIRT]->onRightClick == NULL);
	assert(blocks[BLOCK_GRASS]->onRightClick == NULL);
	printf("  dirt/grass: no block onRightClick (place+consume not shadowed) OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_hoe\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
