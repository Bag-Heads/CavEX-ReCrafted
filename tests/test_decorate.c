/*
	Tests source/worldgen/decorate.c -- worldgen_decorate, the Beta 1.7.3 small-
	decoration sequence (flowers/grass/mushrooms/sugar cane/pumpkins/cacti/springs).

	The per-step draw order is ported byte-for-byte; this checks the contract:
	  - deterministic (same seed + biome -> identical world AND identical final
	    RNG state -- the regression guard for the load-bearing draw order),
	  - it consumes a large, fixed amount of randomness (the fixed-count bodies
	    always run -- e.g. flowers 384 draws, tall grass 768),
	  - it actually places tall grass in a grass biome (tall grass descends to the
	    ground, so it lands on the surface regardless of the random start-y), with
	    the right block + grass/fern metadata.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/biome.h"
#include "worldgen/decorate.h"
#include "worldgen/java_random.h"
#include "worldgen/populate.h"

#define AIR 0
#define STONE 1
#define GRASS 2
#define DIRT 3
#define WATER_FLOW 8
#define WATER_STILL 9
#define SAND 12
#define REED 83
#define TALLGRASS 31
#define DANDELION 37
#define SNOW_LAYER 78
#define CACTUS 81

#define FW_W 48
#define FW_SURFACE 64
struct flatworld {
	uint8_t ids[FW_W * FW_W * 128];
	uint8_t meta[FW_W * FW_W * 128];
};

static int fw_index(int x, int y, int z, int* idx) {
	if(y < 0 || y >= 128 || x < 0 || x >= FW_W || z < 0 || z >= FW_W)
		return 0;
	*idx = (x * FW_W + z) * 128 + y;
	return 1;
}
static uint8_t fw_get(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(x, y, z, &idx) ? f->ids[idx] : AIR;
}
static void fw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct flatworld* f = ctx;
	int idx;
	if(fw_index(x, y, z, &idx)) {
		f->ids[idx] = id;
		f->meta[idx] = meta;
	}
}
static int fw_height(void* ctx, int x, int z) {
	(void)ctx;
	(void)x;
	(void)z;
	return FW_SURFACE + 1;
}

static void fw_fill(struct flatworld* f) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++) {
				uint8_t b = AIR;
				if(y < FW_SURFACE)
					b = (y == FW_SURFACE - 1) ? DIRT : STONE;
				else if(y == FW_SURFACE)
					b = GRASS;
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
	memset(f->meta, 0, sizeof(f->meta));
}

// A flat desert: uniform sand surface, everything above it open air -- the
// permissive case (cacti should still place normally; below == SAND).
static void fw_fill_sand(struct flatworld* f) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++) {
				uint8_t b = AIR;
				if(y < FW_SURFACE)
					b = (y == FW_SURFACE - 1) ? SAND : STONE;
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
	memset(f->meta, 0, sizeof(f->meta));
}

// A desert where the surface CHECKERBOARDS sand/stone at FULL HEIGHT (the
// "stone" columns are solid all the way up through the open-air region
// cacti generate into, not just at FW_SURFACE-1): every sand column's 4
// lateral neighbours (x+-1,z / x,z+-1) are guaranteed stone. Under the OLD
// below-only canBlockStay, every sand column is still a valid cactus base;
// under the FIXED 4-lateral gate, NONE of them can ever hold a cactus.
static void fw_fill_sand_checkerboard(struct flatworld* f) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++) {
			bool stone_column = ((x + z) & 1) == 0;
			for(int y = 0; y < 128; y++) {
				uint8_t b = AIR;
				if(y < FW_SURFACE)
					b = STONE;
				else if(y == FW_SURFACE)
					b = stone_column ? STONE : SAND;
				else if(stone_column)
					b = STONE; // solid all the way up -- a real lateral wall
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
		}
	memset(f->meta, 0, sizeof(f->meta));
}

// A shore for WorldGenReed: the surface row (FW_SURFACE) alternates by x
// between a `water_id` column (even x) and a `ground_id` column (odd x), so
// EVERY air cell at FW_SURFACE+1 over an odd column has water next to the
// block under it -- the WorldGenReed.generate spot gate. The reed base then
// stands on `ground_id`: GRASS (Beta-valid) or SAND (Beta-invalid, since
// BlockReed.canPlaceBlockAt [BlockReed.java:33-36] accepts only reed, or
// grass/dirt with Material.water beside the base). `water_id` is 9 (still) or
// 8 (flowing) -- both are Material.water [Block.java:31-32].
static void fw_fill_shore(struct flatworld* f, uint8_t ground_id,
						  uint8_t water_id) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++) {
				uint8_t b = AIR;
				if(y < FW_SURFACE)
					b = (y == FW_SURFACE - 1) ? DIRT : STONE;
				else if(y == FW_SURFACE)
					b = ((x & 1) == 0) ? water_id : ground_id;
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
	memset(f->meta, 0, sizeof(f->meta));
}

// Count reeds in the fixture; every reed base must satisfy Beta's
// canPlaceBlockAt (asserted here, per block). Water = Material.water = 8|9.
static bool fw_is_water(const struct flatworld* f, int x, int y, int z) {
	int idx;
	if(!fw_index(x, y, z, &idx))
		return false;
	return f->ids[idx] == WATER_FLOW || f->ids[idx] == WATER_STILL;
}
static long fw_count_reeds_checked(const struct flatworld* f) {
	long reeds = 0;
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 1; y < 128; y++) {
				int idx = (x * FW_W + z) * 128 + y;
				if(f->ids[idx] != REED)
					continue;
				reeds++;
				uint8_t below = f->ids[idx - 1];
				if(below == REED)
					continue;
				assert((below == GRASS || below == DIRT)
					   && "reed base on a block Beta never allows");
				assert((fw_is_water(f, x - 1, y - 1, z)
						|| fw_is_water(f, x + 1, y - 1, z)
						|| fw_is_water(f, x, y - 1, z - 1)
						|| fw_is_water(f, x, y - 1, z + 1))
					   && "reed base with no Material.water beside it");
			}
	return reeds;
}

int main(void) {
	struct flatworld* fa = malloc(sizeof *fa);
	struct flatworld* fb = malloc(sizeof *fb);
	const int64_t SEED = 0xDEC0DE12LL;

	struct wgen_world wa = {fa, fw_get, fw_set, fw_height, NULL};
	struct wgen_world wb = {fb, fw_get, fw_set, fw_height, NULL};

	// Forest: dandelions(2) + tall grass(2) + probabilistic roses/mushrooms +
	// sugar cane + springs. Run twice -> identical world + identical RNG state.
	fw_fill(fa);
	fw_fill(fb);
	struct java_random ra, rb;
	jrand_set_seed(&ra, SEED);
	jrand_set_seed(&rb, SEED);
	worldgen_decorate(&wa, &ra, BIOME_FOREST, 0, 0);
	worldgen_decorate(&wb, &rb, BIOME_FOREST, 0, 0);
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0);
	assert(memcmp(fa->meta, fb->meta, sizeof(fa->meta)) == 0);
	assert(ra.seed == rb.seed);		   // byte-exact: same randomness consumed
	assert(ra.seed != (uint64_t)SEED); // and a lot of it was consumed

	long tallgrass = 0, dandelions = 0;
	for(int i = 0; i < FW_W * FW_W * 128; i++) {
		if(fa->ids[i] == TALLGRASS) {
			tallgrass++;
			assert(fa->meta[i] == 1 || fa->meta[i] == 2); // grass or fern
		}
		if(fa->ids[i] == DANDELION)
			dandelions++;
	}
	// Tall grass descends to the surface before placing, so a grass biome over a
	// flat surface always grows some.
	assert(tallgrass > 0);

	// Desert: same seed must still be deterministic (different decorations: dead
	// bush + cacti instead of flowers/grass; none can stay on plain grass, but the
	// RNG must still be consumed reproducibly).
	struct java_random rd1, rd2;
	fw_fill(fa);
	fw_fill(fb);
	jrand_set_seed(&rd1, SEED);
	jrand_set_seed(&rd2, SEED);
	worldgen_decorate(&wa, &rd1, BIOME_DESERT, 0, 0);
	worldgen_decorate(&wb, &rd2, BIOME_DESERT, 0, 0);
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0);
	assert(rd1.seed == rd2.seed);

	// Batch H (2026-08-25): worldgen cactus canBlockStay omitted Beta's
	// 4-lateral solid-neighbour test (BlockCactus.canBlockStay [decompile
	// 740c583] checks all 4 horizontal neighbours' Material.isSolid() BEFORE
	// the below-block test) -- a worldgen cactus could generate flush
	// against solid terrain, a state impossible in real Beta.
	//
	// gen_cacti's seed y is drawn from the FULL 0..127 range (worldgen_
	// decorate's `int y = di(r, 128);`), not clamped to the surface, so a
	// SINGLE populate call's 10 tries only very rarely land the +-3 y-jitter
	// exactly on the one row (FW_SURFACE+1) with sand support below --
	// mirrors real Beta, where cacti are a genuinely sparse decoration, not
	// a per-chunk guarantee. Both cases below therefore run across MANY
	// independent seeds/origins and check the AGGREGATE, so the assertions
	// are robust rather than flaky on any one draw.
	{
		enum { TRIALS = 60 };

		// (a) Permissive case: a uniform flat sand desert (no lateral
		// obstacles anywhere) must still place cacti normally over enough
		// tries -- the new gate is a real constraint, not an accidental
		// blanket block. Also checks the general invariant directly: not one
		// placed cactus, across any trial, may have a solid lateral
		// neighbour (here: STONE is the only solid id fw_fill_sand can
		// produce besides air/sand/cactus itself).
		long cacti_open = 0;
		long lateral_violations = 0;
		for(int trial = 0; trial < TRIALS; trial++) {
			fw_fill_sand(fa);
			struct java_random rs;
			jrand_set_seed(&rs, SEED + trial * 7919LL); // distinct seed/trial
			worldgen_decorate(&wa, &rs, BIOME_DESERT, 0, 0);
			for(int x = 1; x < FW_W - 1; x++)
				for(int z = 1; z < FW_W - 1; z++)
					for(int y = 0; y < 128; y++) {
						int idx = (x * FW_W + z) * 128 + y;
						if(fa->ids[idx] != CACTUS)
							continue;
						cacti_open++;
						uint8_t n0 = fa->ids[((x - 1) * FW_W + z) * 128 + y];
						uint8_t n1 = fa->ids[((x + 1) * FW_W + z) * 128 + y];
						uint8_t n2 = fa->ids[(x * FW_W + (z - 1)) * 128 + y];
						uint8_t n3 = fa->ids[(x * FW_W + (z + 1)) * 128 + y];
						if(n0 == STONE || n1 == STONE || n2 == STONE
						   || n3 == STONE)
							lateral_violations++;
					}
		}
		assert(cacti_open > 0
			   && "no cacti placed across 60 trials on open sand -- gate too strict");
		assert(lateral_violations == 0
			   && "a placed cactus had a solid lateral neighbour -- gate not enforced");

		// (b) Adversarial case: a sand/stone CHECKERBOARD where every sand
		// column's 4 lateral neighbours are solid stone walls. Under the OLD
		// below-only check every one of those columns was still a valid
		// cactus base (this would have failed pre-fix, every trial); under
		// the FIXED 4-lateral gate NONE of them can EVER hold a cactus,
		// regardless of seed -- a structural guarantee, not a probabilistic
		// one, but still swept across the same TRIALS seeds for parity with
		// case (a).
		long cacti_walled = 0;
		for(int trial = 0; trial < TRIALS; trial++) {
			fw_fill_sand_checkerboard(fa);
			struct java_random rc;
			jrand_set_seed(&rc, SEED + trial * 7919LL);
			worldgen_decorate(&wa, &rc, BIOME_DESERT, trial * 32, 0);
			for(int i = 0; i < FW_W * FW_W * 128; i++)
				if(fa->ids[i] == CACTUS)
					cacti_walled++;
		}
		assert(cacti_walled == 0);

		printf("  cactus 4-lateral gate: %ld cacti across %d trials on open "
			   "sand (0 lateral-solid violations), 0 across %d trials on a "
			   "solid checkerboard OK\n",
			   cacti_open, TRIALS, TRIALS);
	}

	// WORLDGEN/REED (2026-09-26, REED-SAND-DIAG-0926): worldgen sugar cane must
	// obey BlockReed.canPlaceBlockAt EXACTLY [BlockReed.java:33-36, gated per
	// segment by WorldGenReed.generate:14-16 via canBlockStay]:
	//   below == reed, OR (below == grass|dirt AND Material.water beside the
	//   block below). Material.water is ids 8 (waterMoving) AND 9 (waterStill)
	//   [Block.java:31-32].
	// Two defects: dec_can_stay accepted SAND as a base (reeds generated on
	// beaches -- seed 12345 (119,71,-78), seed 42 (-16,64,43)); and the spot
	// gate in gen_sugarcane tested only still water, so a spot next to
	// FLOWING water only skipped Beta's two height draws and shifted every
	// later decoration of that chunk (RNG-stream divergence).
	//
	// gen_sugarcane's seed y is drawn from 0..127 (not clamped to the
	// surface), so a single populate call only rarely lands y on the one row
	// with a shore below; like the cactus cases above, this sweeps many
	// independent seeds and checks aggregates + per-trial RNG state, which
	// is deterministic for the fixed seed list.
	//
	// Biome: SWAMPLAND runs no dandelions / tall grass / dead bush / cacti,
	// so reeds are the dominant surface writer. NOTE the sand and grass
	// shores do NOT consume identical randomness, in Beta or here: a reed
	// placed by an earlier try makes a later try's spot non-air (no height
	// draws) on the grass shore while the sand shore leaves it open. The
	// RNG invariant proper is flowing-vs-still water on IDENTICAL ground:
	// same placements, so the same height draws and the same end state.
	{
		enum { REED_TRIALS = 256 };
		long reeds_grass = 0, reeds_sand = 0, reeds_flow = 0;
		int trials_with_reeds = 0;

		for(int trial = 0; trial < REED_TRIALS; trial++) {
			const int64_t s = SEED + 104729LL * trial;
			struct java_random rg, rs, rf;

			// (1) Beta-valid shore: grass base, still water beside it.
			fw_fill_shore(fa, GRASS, WATER_STILL);
			jrand_set_seed(&rg, s);
			worldgen_decorate(&wa, &rg, BIOME_SWAMPLAND, 0, 0);
			long ng = fw_count_reeds_checked(fa);
			reeds_grass += ng;
			if(ng > 0)
				trials_with_reeds++;

			// (2) Sand shore, still water: the same spot gate passes (water
			// beside the base), but Beta writes NO reed on sand.
			fw_fill_shore(fb, SAND, WATER_STILL);
			jrand_set_seed(&rs, s);
			worldgen_decorate(&wb, &rs, BIOME_SWAMPLAND, 0, 0);
			reeds_sand += fw_count_reeds_checked(fb);

			// (3) Grass shore next to FLOWING water only: Beta's spot gate is
			// Material.water (8 or 9), so the height draws happen exactly as
			// in (1) -- same RNG position after the call, same reeds placed.
			fw_fill_shore(fb, GRASS, WATER_FLOW);
			jrand_set_seed(&rf, s);
			worldgen_decorate(&wb, &rf, BIOME_SWAMPLAND, 0, 0);
			long nf = fw_count_reeds_checked(fb);
			reeds_flow += nf;
			assert(rf.seed == rg.seed
				   && "flowing-water shore skipped/added height draws vs still water");
			assert(nf == ng && "flowing-water shore placed a different reed count");
		}
		assert(reeds_grass > 0
			   && "no reeds placed across 256 trials on a grass shore -- gate too strict");
		assert(reeds_sand == 0 && "a worldgen reed stands on sand (not Beta 1.7.3)");
		assert(reeds_flow == reeds_grass);
		printf("  reed canPlaceBlockAt gate: %ld reeds in %d/%d trials on a grass "
			   "shore (all bases grass with water beside), 0 on sand, %ld beside "
			   "flowing water with identical RNG state OK\n",
			   reeds_grass, trials_with_reeds, REED_TRIALS, reeds_flow);
	}

	// Snow pass (no RNG): a cold temperature field lays exactly one snow layer on
	// each of the 16x16 exposed grass columns; a warm field lays none.
	double cold[256], warm[256];
	for(int i = 0; i < 256; i++) {
		cold[i] = 0.2;
		warm[i] = 0.8;
	}
	fw_fill(fa);
	worldgen_snow(&wa, cold, 0, 0);
	long snow = 0;
	for(int i = 0; i < FW_W * FW_W * 128; i++)
		if(fa->ids[i] == SNOW_LAYER)
			snow++;
	assert(snow == 256);
	fw_fill(fb);
	worldgen_snow(&wb, warm, 0, 0);
	for(int i = 0; i < FW_W * FW_W * 128; i++)
		assert(fb->ids[i] != SNOW_LAYER);

	free(fa);
	free(fb);
	printf("test_decorate: all assertions passed (determinism + byte-exact RNG; "
		   "%ld tall grass, %ld dandelions in forest; snow on cold columns, none "
		   "when warm)\n",
		   tallgrass, dandelions);
	return 0;
}
