/* next-gaps-0924/GROWTH-A: Beta 1.7.3 crop stay rule in low light.
 *
 * BlockCrops extends BlockFlower and does not override canBlockStay:
 *   BlockFlower.canBlockStay [BlockFlower.java:39-41]
 *     (getFullBlockLightValue(x,y,z) >= 8 || canBlockSeeTheSky(x,y,z))
 *       && canThisPlantGrowOnThisBlockID(below)       -- crops: tilledField only
 *   getFullBlockLightValue [World.java:521-531] = Chunk.getBlockLightValue(..., 0):
 *     raw max(sky nibble, block nibble), NO skylightSubtracted.
 *   BlockFlower.onNeighborBlockChange / updateTick -> func_268_h [:22-37]: pop
 *     (dropBlockAsItem, setBlockWithNotify 0) when it cannot stay.
 *   BlockCrops.updateTick [BlockCrops.java:18-31]: super.updateTick FIRST, and
 *     it does NOT return after a pop: light(y+1) >= 9 -> metadata (air: 0) < 7
 *     -> getGrowthRate -> nextInt((int)(100/rate)); a hit runs
 *     setBlockMetadataWithNotify(x,y,z,1) on the air cell.
 *
 * rand_gen_int is xorshift, not Java's LCG: these goldens pin the DRAW COUNT and
 * ORDER (drops first, then the one growth draw) and the decisions, via a shadow
 * of the same generator.
 *
 * Leaves (the other GROWTH-A item) need no golden here: all three decompiles
 * (client + server src_original, McBetaCpp srcorigjava) read the centre cell
 * OUTSIDE the checkChunksExist block [BlockLeaves.java:135], CaveWii matches,
 * and test_leaf_decay_golden case (e) already pins the missing-chunk verdict.
 */
#include <assert.h>
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
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

#define Y 100
static char fixture[128];
static struct server_local s;
static volatile int draining = 1;
static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if(!(cond)) {                                                          \
			fprintf(stderr, "RED: " __VA_ARGS__);                              \
			fprintf(stderr, "\n");                                             \
			failures++;                                                        \
		}                                                                      \
	} while(0)

static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	if(server_world_set_block(&s.worlds[0], x, y, z,
							  (struct block_data) {.type = type, .metadata = meta}))
		return;
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	assert(b.type == type && b.metadata == meta);
}

static struct block_data at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b;
}

static void settle(void) {
	for(int i = 0; i < 3; i++)
		server_world_tick(&s.worlds[0], &s);
	server_world_light_drain(&s.worlds[0]); /* G9: World.tick's light drain */
}

static void seed(uint32_t v) {
	s.rand_src.seed = v;
	s.rand_src.has_next_gaussian = false;
}

/* The World.rand state after dropBlockAsItem of a crop of `meta` at (x,Y,z):
 * enumerate the real drop path through server_local_plan_block_drops (same
 * World.rand draws -- chance float, three nextInt(15) seed trials, three
 * position floats per emitted unit -- but nothing is spawned, so the entity
 * cap never turns a shadow into a skipped cosmetic drop). */
static int s_planned;
static bool plan_sink(void* ctx, const struct server_local_planned_drop* drop) {
	(void)ctx;
	s_planned++;
	free(drop->reserved_entity);
	return true;
}
static struct random_gen after_drops(int x, int z, int meta, uint32_t seed_v) {
	seed(seed_v);
	struct block_data b = at(x, Y, z);
	b.type = BLOCK_CROPS;
	b.metadata = meta;
	struct block_info info = {.block = &b, .neighbours = NULL, .x = x, .y = Y, .z = z};
	s_planned = 0;
	assert(server_local_plan_block_drops(&s, &info, plan_sink, NULL));
	return s.rand_src;
}

/* Isolated crop on dry farmland: growthRate = 1 + 1 = 2 -> bound 50 (the
 * same value whether or not the crop is still standing: the centre farmland
 * weight is 1.0 unscaled, no crop neighbours). */
#define DRY_BOUND 50

/* One random tick on a crop whose snapshot light is (sky, torch) -- the
 * dispatcher hands onRandomTick a live chunk read, so this is the value the
 * crop's own cell holds. Returns the cell afterwards. */
static struct block_data tick_crop(int x, int z, int sky, int torch) {
	struct block_data b = at(x, Y, z);
	assert(b.type == BLOCK_CROPS);
	b.sky_light = sky;
	b.torch_light = torch;
	struct block_info info = {.block = &b, .neighbours = NULL, .x = x, .y = Y, .z = z};
	blocks[BLOCK_CROPS]->onRandomTick(&s, &info);
	return at(x, Y, z);
}

/* Crop on farmland with a stone roof at Y+3 (canBlockSeeTheSky false; the
 * y+1 cell stays air so its skylight keeps the growth gate open at noon). */
static void build_roofed(int x, int z, bool roof) {
	set(x, Y - 1, z, BLOCK_FARMLAND, 0);
	set(x, Y, z, BLOCK_CROPS, 0);
	set(x, Y + 3, z, roof ? BLOCK_STONE : BLOCK_AIR, 0);
	settle();
}

static void case_light_sweep(void) {
	s.world_time = 6000; /* noon: skylightSubtracted 0 */
	const struct {
		int x, torch, roof, stays;
		const char* name;
	} rows[] = {
		{2, 7, 1, 0, "light 7, no sky"},
		{4, 8, 1, 1, "light 8, no sky"},
		{6, 9, 1, 1, "light 9, no sky"},
		{8, 7, 0, 1, "light 7, sees sky"},
		{10, 0, 0, 1, "light 0, sees sky"},
	};
	for(size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		const int x = rows[i].x, z = 4;
		const int f0 = failures;
		build_roofed(x, z, rows[i].roof);
		int above = server_local_block_light_value(&s, x, Y + 1, z);
		assert(above >= 9 && "fixture: growth gate open above the crop");
		/* find a seed whose growth draw HITS (so the metadata write path is
		 * exercised on both the standing and the popped cell) */
		uint32_t sv = 0;
		struct random_gen expect;
		for(uint32_t t = 1; t < 400000; t++) {
			struct random_gen g;
			if(rows[i].stays) {
				g.seed = t;
				g.has_next_gaussian = false;
			} else {
				g = after_drops(x, z, 0, t);
			}
			struct random_gen h = g;
			if(rand_gen_int(&h, DRY_BOUND) == 0) {
				sv = t;
				expect = h;
				break;
			}
		}
		assert(sv && "no hit seed");
		seed(sv);
		struct block_data after = tick_crop(x, z, 0, rows[i].torch);
		if(rows[i].stays) {
			CHECK(after.type == BLOCK_CROPS && after.metadata == 1,
				  "%s: crop must stay and grow to 1 (got id %d meta %d)",
				  rows[i].name, after.type, after.metadata);
		} else {
			/* popped, then the post-pop growth hit writes meta 1 on air */
			CHECK(after.type == BLOCK_AIR,
				  "%s: Beta canBlockStay pops the crop (got id %d meta %d)",
				  rows[i].name, after.type, after.metadata);
			CHECK(after.type != BLOCK_AIR || after.metadata == 1,
				  "%s: post-pop growth hit = setBlockMetadataWithNotify(1) on air "
				  "(got meta %d)",
				  rows[i].name, after.metadata);
		}
		CHECK(s.rand_src.seed == expect.seed,
			  "%s: draw sequence (%s + 1 growth draw)", rows[i].name,
			  rows[i].stays ? "no drops" : "crop drops");
		printf("%s: %s %s\n", rows[i].name, rows[i].stays ? "stays" : "pops", failures == f0 ? "PASS" : "FAIL");
	}
}

/* Post-pop MISS: the draw is still consumed and the air cell keeps meta 0. */
static void case_popped_miss(void) {
	const int f0 = failures;
	const int x = 12, z = 4;
	s.world_time = 6000;
	build_roofed(x, z, true);
	uint32_t sv = 0;
	struct random_gen expect;
	for(uint32_t t = 1; t < 1000; t++) {
		struct random_gen h = after_drops(x, z, 0, t);
		if(rand_gen_int(&h, DRY_BOUND) != 0) {
			sv = t;
			expect = h;
			break;
		}
	}
	assert(sv);
	seed(sv);
	struct block_data after = tick_crop(x, z, 0, 3);
	CHECK(after.type == BLOCK_AIR && after.metadata == 0,
		  "dark pop, growth miss: air meta 0 (got id %d meta %d)", after.type,
		  after.metadata);
	CHECK(s.rand_src.seed == expect.seed,
		  "dark pop, growth miss: drops + 1 growth draw");
	printf("light 3, no sky, growth miss: pops, draw consumed %s\n", failures == f0 ? "PASS" : "FAIL");
}

/* Real lighting: a crop sealed in stone (4 sides + roof directly above) holds
 * light 0 and no sky; its random tick pops it, and the y+1 stone keeps the
 * growth gate shut (no growth draw: only the drop draws). */
static void case_sealed_random_tick(void) {
	const int f0 = failures;
	const int x = 3, z = 10;
	s.world_time = 6000;
	/* Seal the cell FIRST, then place the crop: CaveWii relights synchronously
	 * inside setBlock, so sealing around a standing crop would already pop it
	 * through the roof's neighbour notify (Beta queues that relight). */
	set(x, Y - 1, z, BLOCK_FARMLAND, 0);
	set(x - 1, Y, z, BLOCK_STONE, 0);
	set(x + 1, Y, z, BLOCK_STONE, 0);
	set(x, Y, z - 1, BLOCK_STONE, 0);
	set(x, Y, z + 1, BLOCK_STONE, 0);
	set(x, Y + 1, z, BLOCK_STONE, 0);
	settle();
	set(x, Y, z, BLOCK_CROPS, 0);
	assert(at(x, Y, z).type == BLOCK_CROPS);
	struct block_data c = at(x, Y, z);
	assert(c.sky_light < 8 && c.torch_light < 8 && "fixture: sealed crop is dark");
	assert(server_local_block_light_value(&s, x, Y + 1, z) < 9);
	struct random_gen expect = after_drops(x, z, 0, 777);
	const int planned = s_planned;
	const size_t ents = dict_entity_size(s.entities);
	seed(777);
	struct block_info info = {.block = &c, .neighbours = NULL, .x = x, .y = Y, .z = z};
	blocks[BLOCK_CROPS]->onRandomTick(&s, &info);
	CHECK(at(x, Y, z).type == BLOCK_AIR,
		  "sealed dark crop (sky %d block %d): random tick pops it (got id %d)",
		  c.sky_light, c.torch_light, at(x, Y, z).type);
	{
		int ahead = -1, behind = -1;
		struct random_gen d = expect, r = s.rand_src;
		for(int k = 0; k < 64 && ahead < 0; k++) { if(d.seed == s.rand_src.seed) ahead = k; rand_gen(&d); }
		for(int k = 0; k < 64 && behind < 0; k++) { if(r.seed == expect.seed) behind = k; rand_gen(&r); }
		CHECK(s.rand_src.seed == expect.seed,
			  "sealed dark crop: drop draws only (growth gate shut above) planned=%d ahead=%d behind=%d",
			  planned, ahead, behind);
	}
	CHECK(dict_entity_size(s.entities) == ents + (size_t)planned,
		  "sealed dark crop: %d seed item(s) spawned (got %d)", planned,
		  (int)(dict_entity_size(s.entities) - ents));
	printf("sealed dark crop, random tick: pops %s\n", failures == f0 ? "PASS" : "FAIL");
}

/* Real lighting + neighbour change: the sealed crop pops when a wall block is
 * swapped (BlockFlower.onNeighborBlockChange -> func_268_h), even though its
 * farmland is intact. */
static void case_sealed_neighbour_change(void) {
	const int f0 = failures;
	const int x = 7, z = 10;
	/* Seal the cell FIRST, then place the crop: CaveWii relights synchronously
	 * inside setBlock, so sealing around a standing crop would already pop it
	 * through the roof's neighbour notify (Beta queues that relight). */
	set(x, Y - 1, z, BLOCK_FARMLAND, 0);
	set(x - 1, Y, z, BLOCK_STONE, 0);
	set(x + 1, Y, z, BLOCK_STONE, 0);
	set(x, Y, z - 1, BLOCK_STONE, 0);
	set(x, Y, z + 1, BLOCK_STONE, 0);
	set(x, Y + 1, z, BLOCK_STONE, 0);
	settle();
	set(x, Y, z, BLOCK_CROPS, 0);
	assert(at(x, Y, z).type == BLOCK_CROPS);
	struct block_data c = at(x, Y, z);
	assert(c.sky_light < 8 && c.torch_light < 8);
	set(x + 1, Y, z, BLOCK_COBBLESTONE, 0); /* notifies the crop */
	CHECK(at(x, Y, z).type == BLOCK_AIR,
		  "sealed dark crop: neighbour change pops it (got id %d)",
		  at(x, Y, z).type);
	printf("sealed dark crop, neighbour change: pops %s\n", failures == f0 ? "PASS" : "FAIL");
}

/* Regression: bright crop, farmland removed -> still pops (support clause). */
static void case_support_loss(void) {
	const int f0 = failures;
	const int x = 12, z = 10;
	set(x, Y - 1, z, BLOCK_FARMLAND, 0);
	set(x, Y, z, BLOCK_CROPS, 0);
	settle();
	set(x, Y - 1, z, BLOCK_DIRT, 0);
	CHECK(at(x, Y, z).type == BLOCK_AIR, "farmland -> dirt: crop pops (got id %d)",
		  at(x, Y, z).type);
	printf("bright crop, farmland lost: pops %s\n", failures == f0 ? "PASS" : "FAIL");
}

int main(void) {
	test_fixture_dir("test_growth_leaves_crops_golden_tmp", fixture, sizeof fixture);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	remove(test_fixture_join(fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities); /* crop pops spawn seed/wheat items */
	s.player.x = 1000.0; /* far from every drop */
	s.player.y = 1000.0;
	s.player.z = 1000.0;
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);

	case_light_sweep();
	case_popped_miss();
	case_sealed_random_tick();
	case_sealed_neighbour_change();
	case_support_loss();

	draining = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	if(failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_growth_leaves_crops_golden: all PASS\n");
	return 0;
}
