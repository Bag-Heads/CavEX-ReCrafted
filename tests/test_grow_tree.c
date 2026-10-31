/*
	Golden test for Beta 1.7.3 BlockSapling.growTree (worldgen_grow_sapling): the
	pure worldgen entry the runtime bone-meal / random-tick growth drives. Over a
	flat buffer + a seeded java_random it pins:
	  - species selection: meta & 3 -> oak(0) / spruce(1) / birch(2) wood + leaf
	    metadata, and a tree actually generates (logs + leaves placed; the sapling's
	    own cell becomes the trunk base, the cell below becomes dirt);
	  - restore-on-fail: a sapling with no headroom is put back with its species
	    metadata and NO tree is placed (Beta setBlockAndMetadata fallback);
	  - determinism: same seed -> identical world + final RNG state WITHIN a build
	    (the regression guard for the load-bearing draw order; the big-tree path also
	    flows through libm sin/cos/floor, so cross-platform byte-exactness is NOT
	    claimed -- only same-build reproducibility);
	  - the spruce variant is the conical WorldGenTaiga2 (total nextInt(4)+6 -> top
	    extent in [6,9]), NOT the taller WorldGenTaiga1 -- the regression guard for
	    the inverted-taiga-name trap that growTree's first cut fell into;
	  - the species-0 1-in-10 big-oak dispatch is actually exercised.

	Geometry + draw-order faithfulness of the builders themselves is covered by
	test_tree.c; this adds the growTree species dispatch + clear/restore on top.

	Decompile: 740c583:1.7.3-LTS BlockSapling.growTree.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/java_random.h"
#include "worldgen/populate.h"
#include "worldgen/tree.h"

#define AIR 0
#define STONE 1
#define GRASS 2
#define DIRT 3
#define SAPLING 6
#define LOG 17
#define LEAVES 18

#define FW_W 32
#define FW_SURFACE 64
#define TX 16
#define TZ 16
#define TY (FW_SURFACE + 1) // sapling sits on top of the grass surface

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
static uint8_t fw_meta_at(struct flatworld* f, int x, int y, int z) {
	int idx;
	return fw_index(x, y, z, &idx) ? f->meta[idx] : 0;
}

static void fw_fill(struct flatworld* f) {
	memset(f->ids, AIR, sizeof(f->ids));
	memset(f->meta, 0, sizeof(f->meta));
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++) {
				uint8_t b = AIR;
				if(y < FW_SURFACE - 1)
					b = STONE;
				else if(y == FW_SURFACE - 1)
					b = DIRT;
				else if(y == FW_SURFACE)
					b = GRASS;
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
}

static void count_wood(struct flatworld* f, long* logs, long* leaves,
					   long* bad_meta, uint8_t species_meta) {
	*logs = *leaves = *bad_meta = 0;
	for(int i = 0; i < FW_W * FW_W * 128; i++) {
		if(f->ids[i] == LOG) {
			(*logs)++;
			if(f->meta[i] != species_meta)
				(*bad_meta)++;
		}
		if(f->ids[i] == LEAVES) {
			(*leaves)++;
			if(f->meta[i] != species_meta)
				(*bad_meta)++;
		}
	}
}

// Highest y occupied by LOG/LEAVES minus the grow position TY. For the conical
// spruce this equals the builder's `total` (the canopy's top leaf sits at py+total),
// so it distinguishes WorldGenTaiga2 (total nextInt(4)+6 -> [6,9]) from the taller
// WorldGenTaiga1 (nextInt(5)+7 -> up to 11).
static int tree_top_extent(struct flatworld* f) {
	int top = -1;
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 127; y >= 0; y--) {
				uint8_t b = f->ids[(x * FW_W + z) * 128 + y];
				if(b == LOG || b == LEAVES) {
					if(y > top)
						top = y;
					break;
				}
			}
	return top - TY;
}

// Grow a sapling of `species` at (TX,TY,TZ) over a fresh flat world.
static bool grow_species(struct flatworld* f, struct java_random* r, int species,
						 int64_t seed) {
	fw_fill(f);
	fw_set(f, TX, TY, TZ, SAPLING, (uint8_t)species);
	struct wgen_world w = {f, fw_get, fw_set, NULL, NULL};
	jrand_set_seed(r, seed);
	return worldgen_grow_sapling(&w, r, species, TX, TY, TZ);
}

int main(void) {
	struct flatworld* f = malloc(sizeof *f);
	struct flatworld* g = malloc(sizeof *g);
	assert(f && g);
	const int64_t SEED = 0x5AB1A2B3LL;
	struct java_random r;
	long logs, leaves, bad;

	// --- oak (species 0): meta-0 wood, a tree grows, trunk base placed.
	// (May roll the big oak 1-in-10; both put a meta-0 log at the sapling cell.) ---
	assert(grow_species(f, &r, 0, SEED) == true);
	count_wood(f, &logs, &leaves, &bad, 0);
	assert(logs > 0 && leaves > 0 && bad == 0);
	assert(fw_get(f, TX, TY, TZ) == LOG); // sapling cell became the trunk base

	// --- spruce (species 1): meta-1 wood, and the CONICAL WorldGenTaiga2 (short),
	// NOT the tall WorldGenTaiga1 -- the regression guard for the inverted-name bug ---
	assert(grow_species(f, &r, 1, SEED) == true);
	count_wood(f, &logs, &leaves, &bad, 1);
	assert(logs > 0 && leaves > 0 && bad == 0);
	assert(fw_get(f, TX, TY, TZ) == LOG);
	assert(fw_get(f, TX, TY - 1, TZ) == DIRT); // builder roots the tree on dirt
	int spruce_extent = tree_top_extent(f);
	assert(spruce_extent >= 6 && spruce_extent <= 9); // Taiga2 total = nextInt(4)+6

	// --- birch (species 2): meta-2 wood, deterministic (no big-tree branch) ---
	assert(grow_species(f, &r, 2, SEED) == true);
	count_wood(f, &logs, &leaves, &bad, 2);
	assert(logs > 0 && leaves > 0 && bad == 0);
	assert(fw_get(f, TX, TY, TZ) == LOG);
	assert(fw_get(f, TX, TY - 1, TZ) == DIRT);

	// --- species-0 BIG-OAK dispatch: a seed whose first nextInt(10)==0 forces the
	// 1-in-10 WorldGenBigTree branch that the SEED above never reaches ---
	int64_t big_seed = -1;
	for(int64_t cand = 0; cand < 65536; cand++) {
		struct java_random tj;
		jrand_set_seed(&tj, cand);
		if(jrand_next_int_bound(&tj, 10) == 0) {
			big_seed = cand;
			break;
		}
	}
	assert(big_seed >= 0);
	assert(grow_species(f, &r, 0, big_seed) == true);
	count_wood(f, &logs, &leaves, &bad, 0);
	assert(logs > 0 && leaves > 0 && bad == 0); // big oak: meta-0 logs + leaves
	assert(fw_get(f, TX, TY, TZ) == LOG);		// trunk base log

	// --- determinism: same seed + species -> identical world + RNG state ---
	struct java_random ra, rb;
	grow_species(f, &ra, 0, SEED);
	grow_species(g, &rb, 0, SEED);
	assert(memcmp(f->ids, g->ids, sizeof(f->ids)) == 0);
	assert(memcmp(f->meta, g->meta, sizeof(f->meta)) == 0);
	assert(ra.seed == rb.seed); // byte-exact randomness consumed each run

	// --- restore-on-fail: a birch sapling boxed in by a ceiling stays a sapling ---
	fw_fill(f);
	fw_set(f, TX, TY, TZ, SAPLING, 2);
	fw_set(f, TX, TY + 1, TZ, STONE, 0); // no headroom -> clear-space check fails
	{
		struct wgen_world w = {f, fw_get, fw_set, NULL, NULL};
		jrand_set_seed(&r, SEED);
		assert(worldgen_grow_sapling(&w, &r, 2, TX, TY, TZ) == false);
	}
	assert(fw_get(f, TX, TY, TZ) == SAPLING);	// restored
	assert(fw_meta_at(f, TX, TY, TZ) == 2);		// with its species metadata
	assert(fw_get(f, TX, TY + 1, TZ) == STONE); // ceiling untouched
	count_wood(f, &logs, &leaves, &bad, 0);
	assert(logs == 0 && leaves == 0); // no tree placed

	free(f);
	free(g);
	printf("test_grow_tree: all assertions passed (oak/spruce/birch species + "
		   "trunk/root + determinism + restore-on-fail)\n");
	return 0;
}
