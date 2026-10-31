/*
	Golden test for Beta 1.7.3 buckets (ItemBucket.onItemRightClick).

	Pins, over a real server world + player inventory:
	  - empty bucket FILLS from a water/lava SOURCE (material + metadata 0) hit by the bucket's OWN
	    eye ray, through the real SRPC_BLOCK_PLACE dispatch: the source clears and the held slot
	    becomes the matching filled bucket. The crosshair block/face is NOT used (ItemBucket has
	    no onItemUse), so a source in front of the clicked block is the one scooped.
	  - empty bucket does NOT fill from a non-source still block (metadata > 0)  [the source-check fix].
	  - water/lava bucket EMPTIES into the canonical adjacent air cell and reverts to an empty
	    bucket. The adjacent cell uses blocks_side_offset -- the FRONT/BACK cases pin the fix for
	    the old swapped hand-rolled switch.

	Decompile: 740c583:1.7.3-LTS ItemBucket.onItemRightClick.
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
#include "entity/entity.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_bucket_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct item item_bucket;
extern struct item item_bucket_water;
extern struct item item_bucket_lava;

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
	struct block_data expected = {
		.type = (uint8_t)type,
		.metadata = (uint8_t)meta,
	};
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

// Put a single `id` in hotbar slot 0 and select it.
static void give(int id) {
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0,
					   (struct item_data) {.id = (uint16_t)id, .count = 1});
	inventory_set_hotbar(&s.player.inventory, 0);
}

static int held_id(void) {
	struct item_data it;
	if(!inventory_get_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0, &it))
		return -1;
	return it.id;
}

// Right-click face `side` of (x,y,z) holding bucket item `*item` (already in slot 0).
static bool bucket_use(struct item* item, int x, int y, int z, enum side side) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	struct block_info on = {
		.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct item_data* held
		= &s.player.inventory.items[INVENTORY_SLOT_HOTBAR];
	assert(item && item->onUseOnBlock);
	return item->onUseOnBlock(&s, &s.player, held, &on, side);
}

/* Real at-a-block click: SRPC_BLOCK_PLACE on (x,y,z,side) with the eye at
 * (ex,ey,ez) looking straight down (Beta pitch 90). */
static void click_block(double ex, double ey, double ez, int x, int y, int z,
						enum side side) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = ex;
	s.player.y = ey;
	s.player.z = ez;
	s.player.rx = 0.0F;
	s.player.ry = 90.0F;
	struct server_rpc use = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side},
	};
	server_local_test_process_rpc(&s, &use);
}

static void test_fill_from_clicked_source(void) {
	give(ITEM_BUCKET);
	set(8, 70, 8, BLOCK_STONE, 0);
	set(8, 71, 8, BLOCK_WATER_STILL, 0); // a source
	set(8, 72, 8, BLOCK_AIR, 0);
	click_block(8.5, 74.0, 8.5, 8, 71, 8, SIDE_TOP);
	assert(type_at(8, 71, 8) == BLOCK_AIR);		 // source scooped
	assert(held_id() == ITEM_BUCKET_WATER);		 // slot now a water bucket
	printf("  empty bucket ray on a water source: scooped, slot -> water bucket OK\n");
}

static void test_fill_from_adjacent_source(void) {
	/* Crosshair on the stone UNDER the lava (the ordinary pick passes through
	 * liquids); the bucket's own ray stops on the lava source first. */
	give(ITEM_BUCKET);
	set(8, 71, 10, BLOCK_STONE, 0);			// clicked block (solid)
	set(8, 72, 10, BLOCK_LAVA_STILL, 0);	// source in front of it on the ray
	set(8, 73, 10, BLOCK_AIR, 0);
	click_block(8.5, 75.0, 10.5, 8, 71, 10, SIDE_TOP);
	assert(type_at(8, 72, 10) == BLOCK_AIR);	// the ray's source scooped
	assert(type_at(8, 71, 10) == BLOCK_STONE);
	assert(held_id() == ITEM_BUCKET_LAVA);
	printf("  empty bucket aimed at a block behind a lava source: ray scoops it OK\n");
}

static void test_no_fill_from_non_source(void) {
	give(ITEM_BUCKET);
	// Fully enclose the cell with stone so the fluid sim can't reflow/drain the non-source block
	// before we read it. metadata 3 != 0 is NOT a source: BlockFluid.canCollideCheck lets the ray
	// pass through it to the stone below, which is not a liquid -> no fill.
	set(8, 70, 12, BLOCK_STONE, 0); // below
	set(7, 71, 12, BLOCK_STONE, 0);
	set(9, 71, 12, BLOCK_STONE, 0);
	set(8, 71, 11, BLOCK_STONE, 0);
	set(8, 71, 13, BLOCK_STONE, 0);
	set(8, 72, 12, BLOCK_AIR, 0);
	set(8, 71, 12, BLOCK_WATER_STILL, 3); // non-source, enclosed
	click_block(8.5, 74.0, 12.5, 8, 71, 12, SIDE_TOP);
	assert(held_id() == ITEM_BUCKET);		 // did NOT fill (source-check rejected meta 3)
	assert(type_at(8, 71, 12) != BLOCK_AIR); // water not scooped
	printf("  empty bucket ray over enclosed non-source still water (meta 3): no fill OK\n");
}

static void test_empty_water_canonical_sides(void) {
	// SIDE_BACK -> +z (canonical). The old swapped switch would have placed at -z.
	give(ITEM_BUCKET_WATER);
	set(8, 71, 14, BLOCK_STONE, 0);
	set(8, 71, 15, BLOCK_AIR, 0); // canonical BACK cell
	bucket_use(&item_bucket_water, 8, 71, 14, SIDE_BACK);
	assert(type_at(8, 71, 15) == BLOCK_WATER_FLOW);
	assert(held_id() == ITEM_BUCKET); // reverted to empty bucket

	// SIDE_FRONT -> -z (canonical).
	give(ITEM_BUCKET_WATER);
	set(8, 71, 20, BLOCK_STONE, 0);
	set(8, 71, 19, BLOCK_AIR, 0); // canonical FRONT cell
	bucket_use(&item_bucket_water, 8, 71, 20, SIDE_FRONT);
	assert(type_at(8, 71, 19) == BLOCK_WATER_FLOW);
	assert(held_id() == ITEM_BUCKET);
	printf("  water bucket empties into canonical BACK=+Z / FRONT=-Z cell, slot -> empty OK\n");
}

static void test_empty_lava(void) {
	give(ITEM_BUCKET_LAVA);
	set(8, 71, 16, BLOCK_STONE, 0);
	set(8, 72, 16, BLOCK_AIR, 0); // TOP cell
	bucket_use(&item_bucket_lava, 8, 71, 16, SIDE_TOP);
	assert(type_at(8, 72, 16) == BLOCK_LAVA_FLOW);
	assert(held_id() == ITEM_BUCKET);
	printf("  lava bucket empties flowing lava above, slot -> empty OK\n");
}

static void test_filled_bucket_replaceability(void) {
	/* ItemBucket gates on Material.isSolid, not AIR. Circuits and snow-layer are
	 * replaceable; a full stone cube is not. */
	set(12, 71, 12, BLOCK_STONE, 0);
	set(12, 72, 12, BLOCK_TORCH, 5);
	give(ITEM_BUCKET_WATER);
	bucket_use(&item_bucket_water, 12, 71, 12, SIDE_TOP);
	assert(type_at(12, 72, 12) == BLOCK_WATER_FLOW);
	assert(held_id() == ITEM_BUCKET);

	set(14, 71, 12, BLOCK_STONE, 0);
	set(14, 72, 12, BLOCK_SNOW, 0);
	give(ITEM_BUCKET_LAVA);
	bucket_use(&item_bucket_lava, 14, 71, 12, SIDE_TOP);
	assert(type_at(14, 72, 12) == BLOCK_LAVA_FLOW);
	assert(held_id() == ITEM_BUCKET);

	set(16, 71, 12, BLOCK_STONE, 0);
	set(16, 72, 12, BLOCK_STONE, 0);
	give(ITEM_BUCKET_WATER);
	bucket_use(&item_bucket_water, 16, 71, 12, SIDE_TOP);
	assert(type_at(16, 72, 12) == BLOCK_STONE);
	assert(held_id() == ITEM_BUCKET_WATER);
	puts("  filled buckets replace non-solid torch/snow but reject solid stone OK");
}

static void test_nether_vapor_rng_ownership(void) {
	const int64_t math_seed = INT64_C(0x102030405060);
	server_local_test_seed_math_random(math_seed);
	struct java_random math_expected;
	jrand_set_seed(&math_expected, math_seed);
	for(int i = 0; i < 24; i++)
		(void)jrand_next_double(&math_expected);
	s.rand_src.seed = UINT32_C(0x2468ace1);
	struct random_gen world_expected = s.rand_src;
	(void)rand_gen_flt(&world_expected);
	(void)rand_gen_flt(&world_expected);

	set(18, 71, 12, BLOCK_STONE, 0);
	set(18, 72, 12, BLOCK_AIR, 0);
	give(ITEM_BUCKET_WATER);
	s.world.dimension = WORLD_DIM_NETHER;
	bucket_use(&item_bucket_water, 18, 71, 12, SIDE_TOP);
	s.world.dimension = WORLD_DIM_OVERWORLD;
	assert(type_at(18, 72, 12) == BLOCK_AIR);
	assert(held_id() == ITEM_BUCKET);
	assert(s.rand_src.seed == world_expected.seed);
	struct server_local_math_random_state math_after;
	server_local_math_random_capture(&math_after);
	assert(math_after.initialized && math_after.rng.seed == math_expected.seed);
	puts("  nether water vaporization owns 2 World.rand + 24 Math.random draws OK");
}

static void test_production_use_dispatch(void) {
	/* Regression for the former onItemPlace registration: production skipped
	 * the callback whenever the adjacent cell was a torch/lever/wire because
	 * those blocks are not place_ignore. Drive the actual SRPC path. The eye is
	 * off the torch's 0.4-0.6 ray box, so the bucket ray reaches the stone top
	 * and the (non-solid) torch cell is replaced. */
	const int x = 20, y = 71, z = 12;
	set(x, y, z, BLOCK_STONE, 0);
	set(x, y + 1, z, BLOCK_TORCH, 5);
	give(ITEM_BUCKET_WATER);
	click_block(x + 0.2, y + 1 + 1.62, z + 0.5, x, y, z, SIDE_TOP);
	assert(type_at(x, y + 1, z) == BLOCK_WATER_FLOW);
	assert(held_id() == ITEM_BUCKET);
	puts("  production SRPC use replaces torch despite player overlap and returns bucket OK");
}

int main(void) {
	test_fixture_dir("test_bucket_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;

	printf("test_bucket:\n");
	test_fill_from_clicked_source();
	test_fill_from_adjacent_source();
	test_no_fill_from_non_source();
	test_empty_water_canonical_sides();
	test_empty_lava();
	test_filled_bucket_replaceability();
	test_nether_vapor_rng_ownership();
	test_production_use_dispatch();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_bucket\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
