/*
	Tests source/worldgen/biome.c -- the Beta 1.7.3 biome classifier.

	Classification is a pure function of (transformed) temperature and humidity:
	Beta builds a 64x64 lookup table from a decision tree, then quantizes
	temp/humidity to index it. We verify the table only yields real overworld
	biomes, that documented decision-tree branches resolve correctly at known
	points (recomputed by hand INCLUDING the 0..63 quantization), that a healthy
	variety of biomes is reachable, and that the per-biome surface blocks match
	Beta (desert -> sand, everything else grass over dirt).
*/

#include <assert.h>
#include <stdio.h>

#include "worldgen/biome.h"

// Beta block ids (enum block_type).
#define B_GRASS 2
#define B_DIRT 3
#define B_SAND 12

int main(void) {
	biome_lut_init();

	// The tree only ever yields RAINFOREST..TUNDRA (1..11) -- never NONE(0),
	// HELL(12) or SKY(13). Sweep the whole [0,1]^2 input space.
	for(int ti = 0; ti <= 100; ti++)
		for(int hi = 0; hi <= 100; hi++) {
			enum biome b = biome_classify(ti / 100.0, hi / 100.0);
			assert(b >= BIOME_RAINFOREST && b <= BIOME_TUNDRA);
		}

	// Deterministic.
	assert(biome_classify(0.6, 0.7) == biome_classify(0.6, 0.7));

	// Known points (hand-computed with the 64-grid quantization):
	assert(biome_classify(0.0, 0.0) == BIOME_TUNDRA);	 // temp<0.1 short-circuit
	assert(biome_classify(0.05, 0.5) == BIOME_TUNDRA);	 // temp<0.1 short-circuit
	assert(biome_classify(1.0, 0.0) == BIOME_DESERT);	 // hot + bone dry
	assert(biome_classify(1.0, 1.0) == BIOME_RAINFOREST); // hot + wet
	assert(biome_classify(0.6, 0.7) == BIOME_FOREST);	 // temperate + damp
	assert(biome_classify(0.3, 0.5) == BIOME_TUNDRA);	 // cool, rainfall*temp<0.2

	// Surface blocks: desert/ice-desert sandy, everything else grass/dirt.
	assert(biome_top_block(BIOME_DESERT) == B_SAND);
	assert(biome_filler_block(BIOME_DESERT) == B_SAND);
	assert(biome_top_block(BIOME_ICE_DESERT) == B_SAND);
	assert(biome_filler_block(BIOME_ICE_DESERT) == B_SAND);
	assert(biome_top_block(BIOME_FOREST) == B_GRASS);
	assert(biome_filler_block(BIOME_FOREST) == B_DIRT);
	assert(biome_top_block(BIOME_TUNDRA) == B_GRASS);
	assert(biome_filler_block(BIOME_PLAINS) == B_DIRT);

	// A healthy variety of biomes is reachable (not collapsing to one).
	int seen[16] = {0};
	int distinct = 0;
	for(int ti = 0; ti <= 63; ti++)
		for(int hi = 0; hi <= 63; hi++) {
			enum biome b = biome_classify(ti / 63.0, hi / 63.0);
			if(b >= 1 && b <= 11 && !seen[b]) {
				seen[b] = 1;
				distinct++;
			}
		}
	assert(distinct >= 8);

	printf("test_biome: all assertions passed (Beta biome classification + "
		   "surface blocks)\n");
	return 0;
}
