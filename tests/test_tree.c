/*
	Tests source/worldgen/tree.c -- worldgen_trees, the Beta 1.7.3 tree decorate
	step (oak/birch/taiga/taiga-alt, plus the big-tree nextLong reservation).

	The per-builder draw order is ported byte-for-byte from Beta; this checks the
	pass's observable contract:
	  - deterministic (same seed + biome -> identical world AND identical final
	    RNG state, i.e. the whole pass consumes randomness reproducibly -- the
	    regression guard for the load-bearing draw order),
	  - it actually grows trees in a tree biome (logs + leaves placed),
	  - wood/leaf metadata is per-species correct: forest = oak(0)/birch(2),
	    taiga = spruce(1),
	  - a no-tree biome (desert) grows nothing and still advances the RNG.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/biome.h"
#include "worldgen/java_random.h"
#include "worldgen/populate.h"
#include "worldgen/tree.h"

#define AIR 0
#define STONE 1
#define GRASS 2
#define DIRT 3
#define LOG 17
#define LEAVES 18

// Flat world spanning the chunk + its +X/+Z spill (trees place at col+8..+24).
#define FW_W 48
#define FW_SURFACE 64 // grass top; air above, dirt+stone below
struct flatworld {
	uint8_t ids[FW_W * FW_W * 128];
	uint8_t meta[FW_W * FW_W * 128];
};

static int fw_index(int x, int y, int z, int* idx) {
	if(y < 0 || y >= 128)
		return 0;
	if(x < 0 || x >= FW_W || z < 0 || z >= FW_W)
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
// Surface height = the air just above the terrain top (ignores any logs/leaves a
// tree already placed, like Beta's heightmap reads the ground, not foliage).
static int fw_height(void* ctx, int x, int z) {
	struct flatworld* f = ctx;
	if(x < 0 || x >= FW_W || z < 0 || z >= FW_W)
		return FW_SURFACE + 1;
	for(int y = 127; y >= 0; y--) {
		uint8_t b = f->ids[(x * FW_W + z) * 128 + y];
		if(b == GRASS || b == DIRT || b == STONE)
			return y + 1;
	}
	return 0;
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

static void grow(struct flatworld* f, struct java_random* r, enum biome b,
				 int64_t seed) {
	fw_fill(f);
	struct wgen_world w = {f, fw_get, fw_set, fw_height, NULL};
	jrand_set_seed(r, seed);
	worldgen_trees(&w, r, b, 2.0, 0, 0);
}

int main(void) {
	struct flatworld* fa = malloc(sizeof *fa);
	struct flatworld* fb = malloc(sizeof *fb);
	const int64_t SEED = 0x5EED1234LL;

	// Forest: dense oak/birch/big. Run twice -> identical world + RNG state.
	struct java_random ra, rb;
	grow(fa, &ra, BIOME_FOREST, SEED);
	grow(fb, &rb, BIOME_FOREST, SEED);
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0);
	assert(memcmp(fa->meta, fb->meta, sizeof(fa->meta)) == 0);
	assert(ra.seed == rb.seed); // byte-exact: same randomness consumed each run

	long logs = 0, leaves = 0;
	for(int i = 0; i < FW_W * FW_W * 128; i++) {
		if(fa->ids[i] == LOG) {
			logs++;
			assert(fa->meta[i] == 0 || fa->meta[i] == 2); // oak or birch
		}
		if(fa->ids[i] == LEAVES) {
			leaves++;
			assert(fa->meta[i] == 0 || fa->meta[i] == 2);
		}
	}
	assert(logs > 0 && leaves > 0);

	// Taiga: spruce metadata 1 throughout.
	struct java_random rt;
	grow(fb, &rt, BIOME_TAIGA, SEED);
	long spruce = 0;
	for(int i = 0; i < FW_W * FW_W * 128; i++) {
		if(fb->ids[i] == LOG) {
			spruce++;
			assert(fb->meta[i] == 1);
		}
		if(fb->ids[i] == LEAVES)
			assert(fb->meta[i] == 1);
	}
	assert(spruce > 0);

	// Desert: no trees, but the count draws (nextDouble + nextInt(10)) still fire
	// so the RNG advances exactly as Beta's does for a treeless biome.
	struct java_random rd;
	grow(fa, &rd, BIOME_DESERT, SEED);
	for(int i = 0; i < FW_W * FW_W * 128; i++)
		assert(fa->ids[i] != LOG && fa->ids[i] != LEAVES);
	assert(rd.seed != SEED); // the unconditional count draws advanced the RNG

	free(fa);
	free(fb);
	printf("test_tree: all assertions passed (determinism + byte-exact RNG; "
		   "%ld forest logs, %ld leaves, %ld spruce logs; desert treeless)\n",
		   logs, leaves, spruce);
	return 0;
}
