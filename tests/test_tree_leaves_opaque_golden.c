/*
	Golden: a tree canopy never overwrites an existing leaf, so where two trees
	of different species overlap the FIRST tree's leaves keep their species
	metadata (source/worldgen/tree.c tree_is_opaque).

	Beta 1.7.3 (decompile 740c583), derived by hand -- never from CaveWii:
	  Every tree generator writes a leaf only where
	  `!Block.opaqueCubeLookup[world.getBlockId(x,y,z)]`
	  (WorldGenTrees.java:55, WorldGenForest.java:55, WorldGenTaiga1.java:54,
	  WorldGenTaiga2.java:59). opaqueCubeLookup is a STATIC table filled once in
	  Block's constructor [Block.java:151, opaqueCubeLookup[id] =
	  the block's isOpaqueCube()]; for id 18 that virtual call is
	  BlockLeaves.isOpaqueCube [BlockLeaves.java:173-175, the inverse of graphicsLevel]
	  and runs BEFORE BlockLeavesBase's constructor assigns graphicsLevel
	  [BlockLeavesBase.java:6-9], so the field still reads false and
	  opaqueCubeLookup[18] == true forever (RenderGlobal.java:188's
	  setGraphicsLevel changes only the instance field). => leaves are opaque to
	  canopy placement: a later tree's canopy skips every cell that already
	  holds a leaf, and the earlier tree's species bits survive the overlap.
	  The skipped write is the same block id either way and the corner roll
	  nextInt(2) sits in the LEFT conjunct of the placement test, so block ids
	  and the Random stream are identical with or without the fix -- only the
	  overlap cells' metadata differs (the old rule copied mc173's
	  material.rs:167 "leaves non-opaque", stamping the second species over
	  the first: wrong textures in mixed forests).

	Fixture: the production worldgen_grow_sapling (BlockSapling.growTree) on a
	flat world -- a spruce sapling (meta 1 -> WorldGenTaiga2, leaves meta 1)
	first, then an oak sapling (meta 0 -> WorldGenTrees, leaves meta 0) two
	cells away (three, so the oak's clear-space check misses the spruce
	trunk) so the canopies overlap. Seeds are scanned until both trees
	generate as simple trees with an actual overlap; the pin: every overlap
	leaf still reads meta 1, every leaf outside the spruce canopy reads 0, and
	the block-id grid is identical to the id grid of the same growth under the
	old rule (recomputed here by re-running the second tree with its target
	cells forced to air first: same ids either way).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks_data.h"
#include "worldgen/java_random.h"
#include "worldgen/populate.h"
#include "worldgen/tree.h"

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
	return fw_index(x, y, z, &idx) ? f->ids[idx] : BLOCK_AIR;
}
static uint8_t fw_get_meta(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(x, y, z, &idx) ? f->meta[idx] : 0;
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
	struct flatworld* f = ctx;
	if(x < 0 || x >= FW_W || z < 0 || z >= FW_W)
		return FW_SURFACE + 1;
	for(int y = 127; y >= 0; y--) {
		uint8_t b = f->ids[(x * FW_W + z) * 128 + y];
		if(b == BLOCK_GRASS || b == BLOCK_DIRT || b == BLOCK_STONE)
			return y + 1;
	}
	return 0;
}
static void fw_fill(struct flatworld* f) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++) {
				uint8_t b = BLOCK_AIR;
				if(y < FW_SURFACE)
					b = (y == FW_SURFACE - 1) ? BLOCK_DIRT : BLOCK_STONE;
				else if(y == FW_SURFACE)
					b = BLOCK_GRASS;
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
	memset(f->meta, 0, sizeof(f->meta));
}

/* a simple (non-big) tree: a straight log column on dirt, no other log in a
 * horizontal radius of 2 (a big oak has branch logs off the column) */
static int simple_height(struct flatworld* f, int tx, int tz) {
	int y0 = FW_SURFACE + 1;
	if(fw_get(f, tx, y0, tz) != BLOCK_LOG)
		return 0;
	int h = 0;
	while(fw_get(f, tx, y0 + h, tz) == BLOCK_LOG)
		h++;
	for(int x = tx - 2; x <= tx + 2; x++)
		for(int z = tz - 2; z <= tz + 2; z++) {
			if(x == tx && z == tz)
				continue;
			for(int y = y0; y < y0 + 20; y++)
				if(fw_get(f, x, y, z) == BLOCK_LOG)
					return 0;
		}
	return h;
}

int main(void) {
	struct flatworld* f = malloc(sizeof *f);
	struct flatworld* spruce_only = malloc(sizeof *spruce_only);
	assert(f && spruce_only);
	/* no get_meta: this golden never runs the removal pass, and the literal
	 * must compile against the pre-W1E-c wgen_world for the RED run */
	struct wgen_world w = {.ctx = f, .get = fw_get, .set = fw_set, .height = fw_height};
	/* trunks THREE cells apart: WorldGenTrees' clear-space check [:20-31] scans
	 * radius 2 around the top rows and refuses a LOG there, so 2 apart never
	 * grows; at 3 the oak's +-2 canopy (x 21..25) still meets the spruce canopy */
	const int SX = 20, SZ = 20, OX = 23, OZ = 20;
	int verified = 0, overlap_total = 0, attempts = 0;
	for(int64_t seed = 1; seed < 2000 && verified < 6; seed++) {
		attempts++;
		fw_fill(f);
		struct java_random r;
		jrand_set_seed(&r, seed);
		/* spruce first (BlockSapling.growTree meta 1 -> WorldGenTaiga2) */
		fw_set(f, SX, FW_SURFACE + 1, SZ, BLOCK_SAPLING, 1);
		if(!worldgen_grow_sapling(&w, &r, 1, SX, FW_SURFACE + 1, SZ))
			continue;
		if(simple_height(f, SX, SZ) < 4)
			continue;
		memcpy(spruce_only, f, sizeof *f);
		/* then oak (meta 0 -> WorldGenTrees, 1-in-10 big oak) beside it */
		fw_set(f, OX, FW_SURFACE + 1, OZ, BLOCK_SAPLING, 0);
		struct java_random r_oak = r; /* the same stream position the oak drew from */
		if(!worldgen_grow_sapling(&w, &r, 0, OX, FW_SURFACE + 1, OZ))
			continue;
		if(simple_height(f, OX, OZ) < 4)
			continue;
		/* count the overlap: spruce leaves the oak canopy would have covered */
		int overlap = 0;
		for(int x = OX - 3; x <= OX + 3; x++)
			for(int z = OZ - 3; z <= OZ + 3; z++)
				for(int y = FW_SURFACE + 1; y < FW_SURFACE + 16; y++) {
					if(fw_get(spruce_only, x, y, z) != BLOCK_LEAVES)
						continue;
					/* a spruce leaf inside the oak canopy's bounding box: the
					 * oak wrote around it. Whether or not the oak's own shape
					 * reached this exact cell, its metadata must still be 1. */
					if(fw_get(f, x, y, z) == BLOCK_LEAVES) {
						assert(fw_get_meta(f, x, y, z) == 1
							   && "an earlier spruce leaf must keep species 1 under an oak canopy (Block.java:151)");
						overlap++;
					}
				}
		if(overlap == 0)
			continue;
		/* every leaf that is NOT a spruce leaf is a fresh oak leaf: meta 0 */
		for(int x = 0; x < FW_W; x++)
			for(int z = 0; z < FW_W; z++)
				for(int y = FW_SURFACE + 1; y < FW_SURFACE + 20; y++)
					if(fw_get(f, x, y, z) == BLOCK_LEAVES
					   && fw_get(spruce_only, x, y, z) != BLOCK_LEAVES)
						assert(fw_get_meta(f, x, y, z) == 0);
		/* block ids are unchanged by the rule: re-grow the oak on the spruce
		 * world with leaves treated as writable (the old behaviour) and compare
		 * the id grid cell for cell -- the skipped writes were same-id. */
		{
			struct flatworld* old_rule = malloc(sizeof *old_rule);
			assert(old_rule);
			memcpy(old_rule, spruce_only, sizeof *old_rule);
			/* old rule == write leaves over leaves: emulate by growing on a copy
			 * whose spruce leaves are AIR (so the oak writes them), then putting
			 * the spruce leaves back where the oak did not write */
			for(int x = 0; x < FW_W; x++)
				for(int z = 0; z < FW_W; z++)
					for(int y = 0; y < 128; y++) {
						int idx = (x * FW_W + z) * 128 + y;
						if(old_rule->ids[idx] == BLOCK_LEAVES)
							old_rule->ids[idx] = BLOCK_AIR;
					}
			struct wgen_world wo = w;
			wo.ctx = old_rule;
			old_rule->ids[(OX * FW_W + OZ) * 128 + FW_SURFACE + 1] = BLOCK_SAPLING;
			struct java_random r2 = r_oak;
			bool ok = worldgen_grow_sapling(&wo, &r2, 0, OX, FW_SURFACE + 1, OZ);
			assert(ok);
			assert(r2.seed == r.seed && "the Random stream is identical under both rules");
			for(int x = 0; x < FW_W; x++)
				for(int z = 0; z < FW_W; z++)
					for(int y = 0; y < 128; y++) {
						int idx = (x * FW_W + z) * 128 + y;
						uint8_t a = f->ids[idx];
						uint8_t b = old_rule->ids[idx];
						if(b == BLOCK_AIR && spruce_only->ids[idx] == BLOCK_LEAVES)
							b = BLOCK_LEAVES; /* the spruce leaf the oak left alone */
						assert(a == b && "block ids identical under both rules");
					}
			free(old_rule);
		}
		verified++;
		overlap_total += overlap;
		printf("  seed %lld: spruce h=%d + oak h=%d, %d overlap leaves keep meta 1\n",
			   (long long)seed, simple_height(f, SX, SZ), simple_height(f, OX, OZ), overlap);
	}
	printf("test_tree_leaves_opaque_golden: %d seeds verified of %d tried, %d overlap leaves kept their species\n",
		   verified, attempts, overlap_total);
	assert(verified >= 6 && overlap_total >= 30);
	free(f);
	free(spruce_only);
	return 0;
}
