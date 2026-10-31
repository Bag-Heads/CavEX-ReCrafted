/*
	Golden test for Beta 1.7.3 bone meal (ItemDye.onItemUse, damage 15) on crops.

	Drives item_dye.onUseOnBlock over a real server world and pins:
	  - white dye (durability 15) on a crop -> BlockCrops.fertilize == metadata jumps to 7
	    (full growth), returns true (consume one);
	  - on an already-full crop -> still returns true (Beta consumes even when grown);
	  - on a non-crop block -> false, no change;
	  - a non-white dye (e.g. lapis, durability 4) on a crop -> false, no change.

	This also exercises the new item onUseOnBlock dispatch hook (clicked-block item use,
	distinct from onItemPlace).

	Decompile: 740c583:1.7.3-LTS ItemDye.onItemUse, BlockCrops.fertilize.
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
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_bonemeal_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct item item_dye;	 // modular item (source/item/items/item_dye.c)
extern struct block block_sapling; // for the random-tick cadence test

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

static struct block_data at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	return blk;
}

// Right-click block (x,y,z) holding *it. The dispatch passes the REAL held stack, so
// onUseOnBlock mutates *it directly (self-consume) -- the test reads it back to verify.
static bool use_on(struct item_data* it, int x, int y, int z) {
	struct block_data blk = at(x, y, z);
	struct block_info on = {
		.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
	return item_dye.onUseOnBlock(&s, &s.player, it, &on, SIDE_TOP);
}

// crop on farmland so server_world_set_block's neighbour notify can't pop it before the test.
static void plant_crop(int x, int y, int z, int meta) {
	set(x, y - 1, z, BLOCK_FARMLAND, 0);
	set(x, y, z, BLOCK_CROPS, meta);
}

static void test_bonemeal_grows_crop(void) {
	plant_crop(8, 71, 8, 0);
	assert(at(8, 71, 8).type == BLOCK_CROPS && at(8, 71, 8).metadata == 0);
	struct item_data bm = {.id = ITEM_DYE, .durability = 15, .count = 3};
	assert(use_on(&bm, 8, 71, 8) == true);
	assert(at(8, 71, 8).type == BLOCK_CROPS && at(8, 71, 8).metadata == 7);
	assert(bm.count == 2); // self-consumed one
	printf("  bonemeal on crop(0): -> metadata 7, used=true, count 3->2 OK\n");
}

static void test_bonemeal_last_of_stack_clears_slot(void) {
	plant_crop(8, 71, 12, 0);
	struct item_data bm = {.id = ITEM_DYE, .durability = 15, .count = 1};
	assert(use_on(&bm, 8, 71, 12) == true);
	assert(bm.id == 0 && bm.count == 0); // last one consumed -> slot cleared
	printf("  bonemeal last-of-stack: slot cleared OK\n");
}

static void test_bonemeal_on_full_crop_still_consumes(void) {
	plant_crop(8, 71, 9, 7);
	struct item_data bm = {.id = ITEM_DYE, .durability = 15, .count = 2};
	assert(use_on(&bm, 8, 71, 9) == true); // Beta consumes even when already grown
	assert(at(8, 71, 9).metadata == 7);
	assert(bm.count == 1);
	printf("  bonemeal on crop(7): still used=true, stays 7, count 2->1 OK\n");
}

static void test_bonemeal_on_non_crop_noop(void) {
	set(8, 71, 10, BLOCK_STONE, 0);
	struct item_data bm = {.id = ITEM_DYE, .durability = 15, .count = 2};
	assert(use_on(&bm, 8, 71, 10) == false);
	assert(at(8, 71, 10).type == BLOCK_STONE);
	assert(bm.count == 2); // not consumed on a no-op
	printf("  bonemeal on stone: used=false, unchanged, count 2 OK\n");
}

static void test_non_white_dye_noop(void) {
	plant_crop(8, 71, 11, 0);
	struct item_data lapis = {.id = ITEM_DYE, .durability = 4, .count = 2};
	assert(use_on(&lapis, 8, 71, 11) == false); // only damage 15 is bone meal
	assert(at(8, 71, 11).metadata == 0);
	assert(lapis.count == 2);
	printf("  lapis dye(4) on crop: used=false, crop unchanged OK\n");
}

// Beta growTree on a sapling: clears the sapling, drops a faithful WorldGen* tree
// (trunk base log in the sapling's cell), and consumes one. Birch (species 2) is a
// small deterministic tree; the seed source is pinned so growTree is reproducible.
static void test_bonemeal_grows_sapling(void) {
	set(20, 70, 20, BLOCK_DIRT, 0);
	set(20, 71, 20, BLOCK_SAPLING, 2);
	s.rand_src.seed = 0x00C0FFEEu;
	struct item_data bm = {.id = ITEM_DYE, .durability = 15, .count = 5};
	assert(use_on(&bm, 20, 71, 20) == true);
	assert(at(20, 71, 20).type == BLOCK_LOG); // sapling -> trunk base log
	assert(bm.count == 4);					  // consumed one
	printf("  bonemeal on sapling: grew a tree (trunk base log), count 5->4 OK\n");
}

// Beta grass-block scatter. (Exact positions can't be Beta-byte-exact -- itemRand
// is time-seeded -- so this pins the observable contract with fixed runtime seeds.)
// Covers three load-bearing parts: (a) the n<16 no-walk plant directly above the
// clicked grass; (b) the target==air gate (a solid cell above is never overwritten,
// even though Beta still consumes the bonemeal); (c) the n>=16 random walk reaching
// neighbouring grass and the 9-in-10 tall-grass : flower split.
static void test_bonemeal_scatters_on_grass(void) {
	// (a) directly above the clicked grass block.
	set(24, 70, 24, BLOCK_GRASS, 0);
	set(24, 71, 24, BLOCK_AIR, 0);
	s.rand_src.seed = 0x0000BEEFu;
	struct item_data bm = {.id = ITEM_DYE, .durability = 15, .count = 2};
	assert(use_on(&bm, 24, 70, 24) == true);
	assert(bm.count == 1); // consumed one
	uint8_t t = at(24, 71, 24).type;
	assert(t == BLOCK_TALL_GRASS || t == BLOCK_FLOWER || t == BLOCK_ROSE);

	// (b) target==air gate: a solid block above is left intact, bonemeal still spent.
	set(10, 70, 10, BLOCK_GRASS, 0);
	set(10, 71, 10, BLOCK_STONE, 0);
	struct item_data bm2 = {.id = ITEM_DYE, .durability = 15, .count = 3};
	assert(use_on(&bm2, 10, 70, 10) == true);
	assert(bm2.count == 2);						// Beta --stackSize happens regardless
	assert(at(10, 71, 10).type == BLOCK_STONE); // gate skipped it

	// (c) the walk reaches a 5x5 grass platform: many cells planted (not just the
	// one above), tall grass dominating the split. Plants only land at y=71 (the
	// only level whose block below is platform grass), so counting y=71 is complete.
	for(int dx = -2; dx <= 2; dx++)
		for(int dz = -2; dz <= 2; dz++) {
			set(11 + dx, 70, 15 + dz, BLOCK_GRASS, 0);
			set(11 + dx, 71, 15 + dz, BLOCK_AIR, 0);
		}
	s.rand_src.seed = 0x5CA77E12u;
	struct item_data bm3 = {.id = ITEM_DYE, .durability = 15, .count = 1};
	assert(use_on(&bm3, 11, 70, 15) == true);
	int tg = 0, fl = 0, ro = 0;
	for(int dx = -2; dx <= 2; dx++)
		for(int dz = -2; dz <= 2; dz++) {
			uint8_t pt = at(11 + dx, 71, 15 + dz).type;
			if(pt == BLOCK_TALL_GRASS)
				tg++;
			else if(pt == BLOCK_FLOWER)
				fl++;
			else if(pt == BLOCK_ROSE)
				ro++;
		}
	assert(tg + fl + ro >= 2); // the walk planted more than just the centre cell
	assert(tg >= fl + ro);	   // 9-in-10 tall grass dominates the split
	assert(tg > 0);
	printf("  bonemeal on grass: above=%s; solid-above skipped; platform planted "
		   "%d tall-grass/%d flower/%d rose OK\n",
		   t == BLOCK_TALL_GRASS ? "tall grass"
		   : t == BLOCK_FLOWER	 ? "dandelion"
								 : "rose",
		   tg, fl, ro);
}

// Beta BlockSapling.updateTick cadence: light(y+1)>=9 && nextInt(30)==0; the 1st
// qualifying roll ages the sapling (meta|8), the 2nd grows it. Deterministic via a
// seed whose first two nextInt(30) rolls are both 0 (the two qualifying ticks --
// nothing else draws from rand_src between them on the gate path).
static void test_sapling_random_tick_cadence(void) {
	uint32_t seed = 0;
	for(uint32_t cand = 1; cand != 0; cand++) {
		struct random_gen tmp = {.seed = cand};
		if(rand_gen_int(&tmp, 30) == 0 && rand_gen_int(&tmp, 30) == 0) {
			seed = cand;
			break;
		}
	}
	assert(seed != 0);

	set(28, 70, 28, BLOCK_DIRT, 0);
	set(28, 71, 28, BLOCK_SAPLING, 0);	   // oak, age bit clear
	assert(at(28, 72, 28).sky_light >= 9); // precondition: open sky above
	s.rand_src.seed = seed;

	struct block_data blk = at(28, 71, 28);
	struct block_info info
		= {.block = &blk, .neighbours = NULL, .x = 28, .y = 71, .z = 28};

	// Tick 1: roll 0, age bit clear -> set meta|8, sapling stays (not grown yet).
	block_sapling.onRandomTick(&s, &info);
	blk = at(28, 71, 28); // refresh (info.block still points at blk)
	assert(blk.type == BLOCK_SAPLING);
	assert((blk.metadata & 8) != 0); // aged
	assert((blk.metadata & 3) == 0); // species preserved

	// Tick 2: roll 0, age bit set -> growTree (trunk base log in the sapling cell).
	block_sapling.onRandomTick(&s, &info);
	assert(at(28, 71, 28).type == BLOCK_LOG);
	printf("  sapling random-tick: tick1 ages (meta&8), tick2 grows tree OK\n");
}

int main(void) {
	test_fixture_dir("test_bonemeal_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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

	printf("test_bonemeal:\n");
	test_bonemeal_grows_crop();
	test_bonemeal_last_of_stack_clears_slot();
	test_bonemeal_on_full_crop_still_consumes();
	test_bonemeal_on_non_crop_noop();
	test_non_white_dye_noop();
	test_bonemeal_grows_sapling();
	test_bonemeal_scatters_on_grass();
	test_sapling_random_tick_cadence();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_bonemeal\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
