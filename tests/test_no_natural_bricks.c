/*
	FAITHFULNESS GUARD (project rule, set by the maintainer 2026-06-05):

	  BRICK blocks (id 45) -- and other crafted-only blocks -- NEVER occur in
	  Beta 1.7.3 NATURAL worldgen. They are not produced by terrain generation
	  (already guarded by test_worldgen's valid-id set) and must never be placed
	  by the populate pass (lakes / dungeons / trees / decorations). A brick (or
	  any crafted block) appearing in a freshly generated world is therefore a
	  BUG -- and, since worldgen never emits one, a bug on the CLIENT render /
	  block-registry side, NOT in worldgen. (See the cavewii memory note.)

	This sweeps worldgen_populate over a grid of chunks x seeds x biomes, and
	hammers worldgen_dungeon (the only structure Beta populate builds) directly,
	with a recording set-callback that trips the moment a never-natural block is
	written. If a future worldgen change places one, this fails loudly.

	Dungeons legitimately place CHEST (54) + MOB_SPAWNER (52), so those are NOT
	flagged; only genuinely crafted-only blocks are.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/biome.h"
#include "worldgen/java_random.h"
#include "worldgen/populate.h"

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
	return fw_index(x, y, z, &idx) ? f->ids[idx] : 0;
}
static int fw_height(void* ctx, int x, int z) {
	(void)ctx;
	(void)x;
	(void)z;
	return FW_SURFACE + 1;
}

// Crafted-only blocks that NEVER appear in Beta 1.7.3 natural worldgen.
static int never_natural(uint8_t id) {
	switch(id) {
		case 45: // BLOCK_BRICKS  <-- the reported invariant
		case 5:	 // BLOCK_PLANKS
		case 47: // BLOCK_BOOKSHELF
		case 61: // BLOCK_FURNACE
		case 93: // BLOCK_REDSTONE_REPEATER (idle) -- crafted-only (Item.redstoneRepeater)
		case 94: // BLOCK_REDSTONE_REPEATER_LIT (active) -- crafted-only, never in worldgen
		case 29: // BLOCK_PISTON_STICKY -- crafted-only (slimeball + piston)
		case 33: // BLOCK_PISTON_BASE -- crafted-only (planks/cobble/iron/redstone)
		case 34: // BLOCK_PISTON_HEAD -- only emitted by a piston extend, never worldgen
		case 36: // BLOCK_PISTON_MOVING -- transient moving-block TE, never worldgen
			return 1;
		default:
			return 0;
	}
}

static int g_bad_id = -1, g_bad_x, g_bad_y, g_bad_z;
static void fw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct flatworld* f = ctx;
	int idx;
	if(never_natural(id) && g_bad_id < 0) {
		g_bad_id = id;
		g_bad_x = x;
		g_bad_y = y;
		g_bad_z = z;
	}
	if(fw_index(x, y, z, &idx)) {
		f->ids[idx] = id;
		f->meta[idx] = meta;
	}
}

static void fw_fill(struct flatworld* f) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++) {
				uint8_t b = 0; // air
				if(y < FW_SURFACE)
					b = (y == FW_SURFACE - 1) ? 3 : 1; // dirt under grass else stone
				else if(y == FW_SURFACE)
					b = 2; // grass
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
	memset(f->meta, 0, sizeof(f->meta));
}

int main(void) {
	struct flatworld* f = malloc(sizeof *f);
	assert(f);

	const enum biome biomes[] = {BIOME_FOREST,	 BIOME_PLAINS,
								 BIOME_DESERT,	 BIOME_TAIGA,
								 BIOME_RAINFOREST, BIOME_SEASONAL_FOREST};
	const int64_t seeds[] = {0x0LL, 0xBEEFLL, 0xCAFEF00DLL, -12345LL};
	int populate_runs = 0, dungeon_runs = 0;

	// (1) Full populate sweep: lakes/dungeons/trees/decorations over many
	// chunks x seeds x biomes. tree_density high so forest biomes get dense trees.
	for(size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++)
		for(size_t bi = 0; bi < sizeof(biomes) / sizeof(biomes[0]); bi++)
			for(int cx = -3; cx <= 3; cx++)
				for(int cz = -3; cz <= 3; cz++) {
					fw_fill(f);
					struct wgen_world w
						= {f, fw_get, fw_set, fw_height, NULL, NULL};
					worldgen_populate(seeds[s], cx, cz, &w, biomes[bi], 0.8);
					populate_runs++;
					assert(g_bad_id < 0); // tripped in fw_set on a crafted block
				}

	// (2) Hammer the dungeon generator directly at many positions/seeds. It must
	// build cobblestone/mossy/chest/spawner/air ONLY -- never a brick.
	for(size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++)
		for(int px = 6; px <= 40; px += 6)
			for(int pz = 6; pz <= 40; pz += 6) {
				fw_fill(f);
				// carve an air pocket so the room has somewhere to sit
				for(int x = px - 5; x <= px + 5; x++)
					for(int z = pz - 5; z <= pz + 5; z++)
						for(int y = 56; y <= 64; y++) {
							int idx;
							if(fw_index(x, y, z, &idx))
								f->ids[idx] = 0;
						}
				struct wgen_world w = {f, fw_get, fw_set, fw_height, NULL, NULL};
				struct java_random r;
				jrand_set_seed(&r, seeds[s] ^ ((int64_t)px * 341 + pz));
				(void)worldgen_dungeon(&w, &r, px, 60, pz);
				dungeon_runs++;
				assert(g_bad_id < 0);
			}

	if(g_bad_id >= 0) {
		printf("FAIL: never-natural block id %d placed at (%d,%d,%d)\n", g_bad_id,
			   g_bad_x, g_bad_y, g_bad_z);
		free(f);
		return 1;
	}
	printf("OK: no bricks/crafted blocks across %d populate + %d dungeon runs\n",
		   populate_runs, dungeon_runs);
	free(f);
	return 0;
}
