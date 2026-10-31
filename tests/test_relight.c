/*
	Targeted test for the post-populate relight (server_world.c calls these after
	worldgen_populate). Proves the behaviours the review asked for:

	  1. A tree (log + leaf canopy) RAISES the column heightmap to the canopy top
	     (so snow lands on it, not the ground), where bare terrain stops at grass.
	  2. Skylight is SHADOWED under the canopy (attenuated through the leaves)
	     vs full daylight in an open column at the same height.
	  3. worldgen_lighting(reset_torch=false) PRESERVES already-propagated block
	     light, while reset_torch=true zeroes it (the split that lets the relight
	     re-run after populate without wiping lava-spring light).
	  4. The lighting engine (lighting_update_at_block, which the server block-
	     light flood drives) seeds block light from a lava source to its
	     neighbour.

	Links the real worldgen_lighting + lighting engine with a MINIMAL real block
	table (terrain solids opaque, leaves see-through opacity 1, lava luminous) so
	the heightmap/attenuation are real.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "lighting.h"
#include "network/server_world.h"
#include "worldgen/worldgen.h"

#define N 16
#define H 128
#define IDX(x, y, z) ((size_t)(y) + ((size_t)(z) + (size_t)(x) * N) * H)
#define HM(x, z) ((x) + (z) * N)

// Minimal block table (worldgen reads opacity/can_see_through/luminance only).
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_leaves = {.can_see_through = true, .opacity = 1};
static struct block b_water = {.can_see_through = true, .opacity = 3};
static struct block b_lava
	= {.can_see_through = false, .opacity = 15, .luminance = 15};
struct block* blocks[256] = {0};
static void setup_blocks(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_DIRT] = blocks[BLOCK_GRASS]
		= blocks[BLOCK_BEDROCK] = blocks[BLOCK_LOG] = &b_solid;
	blocks[BLOCK_LEAVES] = &b_leaves;
	blocks[BLOCK_WATER_STILL] = blocks[BLOCK_WATER_FLOW] = &b_water;
	blocks[BLOCK_LAVA_STILL] = blocks[BLOCK_LAVA_FLOW] = &b_lava;
}

// lighting.c's flood asks the block registry for the six neighbour offsets; the
// real blocks_side_offset lives in the heavy block table, so provide the offsets
// directly. The flood visits all six neighbours, so their order is irrelevant to
// the result (every adjacent cell is reached regardless of enum ordering).
void blocks_side_offset(enum side s, int* x, int* y, int* z) {
	static const int off[6][3]
		= {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	*x = off[s][0];
	*y = off[s][1];
	*z = off[s][2];
}

uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t v) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((v & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (v & 0x0F));
}

// ---- block-light engine harness (a flat block_data grid) -------------------
#define LW 8 // a small LWxLWxH world centred for the lava test
struct cell {
	uint8_t type, sky, torch, meta;
};
static struct cell gworld[LW * LW * H];
static uint8_t gheight[LW * LW];
static size_t gidx(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * LW) * H;
}
static bool g_get(void* u, w_coord_t x, w_coord_t y, w_coord_t z,
				  struct block_data* blk, uint8_t* height) {
	(void)u;
	if(x < 0 || x >= LW || z < 0 || z >= LW || y < 0 || y >= H)
		return false;
	struct cell* c = &gworld[gidx(x, y, z)];
	// lighting.c calls get_block(..., NULL, NULL) purely as an existence/bounds
	// probe (line 133) when enqueuing neighbours, so blk/height may be NULL.
	if(blk) {
		blk->type = c->type;
		blk->metadata = c->meta;
		blk->sky_light = c->sky;
		blk->torch_light = c->torch;
	}
	if(height)
		*height = gheight[x + z * LW];
	return true;
}
static void g_set(void* u, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	(void)u;
	if(x < 0 || x >= LW || z < 0 || z >= LW || y < 0 || y >= H)
		return;
	struct cell* c = &gworld[gidx(x, y, z)];
	c->sky = light & 0x0F;
	c->torch = light >> 4;
}

int main(void) {
	setup_blocks();

	// ---- skylight / heightmap / reset_torch (worldgen_lighting) ----
	struct server_chunk sc;
	memset(&sc, 0, sizeof(sc));
	size_t cells = (size_t)N * N * H;
	sc.ids = calloc(cells, 1);
	sc.metadata = calloc(cells / 2, 1);
	sc.lighting_sky = calloc(cells / 2, 1);
	sc.lighting_torch = calloc(cells / 2, 1);
	sc.heightmap = calloc(N * N, 1);
	assert(sc.ids && sc.lighting_sky && sc.lighting_torch && sc.heightmap);

	// Flat terrain: bedrock@0, stone 1..62, dirt 63, grass 64, air above.
	for(int x = 0; x < N; x++)
		for(int z = 0; z < N; z++) {
			sc.ids[IDX(x, 0, z)] = BLOCK_BEDROCK;
			for(int y = 1; y <= 62; y++)
				sc.ids[IDX(x, y, z)] = BLOCK_STONE;
			sc.ids[IDX(x, 63, z)] = BLOCK_DIRT;
			sc.ids[IDX(x, 64, z)] = BLOCK_GRASS;
		}
	// A tree at (8,8): log 65..68, leaf canopy 69..72 (top leaf y=72).
	for(int y = 65; y <= 68; y++)
		sc.ids[IDX(8, y, 8)] = BLOCK_LOG;
	for(int y = 69; y <= 72; y++)
		sc.ids[IDX(8, y, 8)] = BLOCK_LEAVES;

	// A water column at (4,4): a pond y=60..64 (water surface y=64) over stone, open
	// air above. Water counts in the heightmap (opacity 3 > 0), so the runtime flood
	// attenuates it -- the stored skylight MUST match (the pre-attenuation bug stored
	// the surface at 15; the flood recomputes 12, so every water edit cascaded).
	for(int y = 60; y <= 64; y++)
		sc.ids[IDX(4, y, 4)] = BLOCK_WATER_STILL;

	// Pre-seed a torch value to prove reset_torch=false preserves it.
	nibble_write(sc.lighting_torch, IDX(0, 64, 0), 9);

	worldgen_lighting(&sc, false,
					  WORLD_DIM_OVERWORLD); // the post-populate relight

	// (0) WORLDGEN-vs-FLOOD SKYLIGHT AGREEMENT (the audit's missing test; the
	// far-ring 'lfc' relight cascade). worldgen must STORE the skylight the runtime
	// lighting_flood RECOMPUTES = light from above MINUS the cell's own opacity, so an
	// edit anywhere finds old==new instead of cascading a "correction" across the
	// surface. The old code stored the PRE-attenuation value (surfaces held 15).
	assert(nibble_read(sc.lighting_sky, IDX(0, 65, 0)) == 15); // open air above grass
	assert(nibble_read(sc.lighting_sky, IDX(0, 64, 0)) == 0);  // opaque grass surface
	// Water counts in the heightmap (opacity 3>0) -> below column_height -> the flood
	// propagate-attenuates it 3/block; the store must match (pre-fix this was 15).
	assert(sc.heightmap[HM(4, 4)] == 65);					   // water surface y=64 -> 65
	assert(nibble_read(sc.lighting_sky, IDX(4, 65, 4)) == 15);  // open air above water
	assert(nibble_read(sc.lighting_sky, IDX(4, 64, 4)) == 12);  // water surface (15-3)
	assert(nibble_read(sc.lighting_sky, IDX(4, 63, 4)) == 9);   // 1 below (12-3)
	assert(nibble_read(sc.lighting_sky, IDX(4, 60, 4)) == 0);   // bottom water (3-3)

	// (1) heightmap: tree column raised to canopy top+1, bare column at grass+1.
	assert(sc.heightmap[HM(8, 8)] == 73); // top leaf y=72 -> 73
	assert(sc.heightmap[HM(0, 0)] == 65); // grass y=64 -> 65

	// (2) skylight shadow: under the canopy (just below top leaf) the tree column
	// is darker than an open column at the same y (open is full 15 above grass).
	uint8_t tree_sky = nibble_read(sc.lighting_sky, IDX(8, 70, 8)); // inside leaves
	uint8_t open_sky = nibble_read(sc.lighting_sky, IDX(0, 70, 0)); // open air
	assert(open_sky == 15);
	assert(tree_sky < 15); // leaves attenuated the column

	// (3) reset_torch=false preserved the pre-seeded torch.
	assert(nibble_read(sc.lighting_torch, IDX(0, 64, 0)) == 9);
	worldgen_lighting(&sc, true, WORLD_DIM_OVERWORLD); // now zero it
	assert(nibble_read(sc.lighting_torch, IDX(0, 64, 0)) == 0);

	// ---- block-light flood from a lava source (lighting engine) ----
	memset(gworld, 0, sizeof(gworld));
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++) {
			for(int y = 0; y <= 64; y++)
				gworld[gidx(x, y, z)].type = BLOCK_STONE;
			gheight[x + z * LW] = 65;
		}
	// Carve an air pocket around a lava cell at (4,65,4).
	for(int x = 3; x <= 5; x++)
		for(int z = 3; z <= 5; z++)
			gworld[gidx(x, 65, z)].type = BLOCK_AIR;
	gworld[gidx(4, 65, 4)].type = BLOCK_LAVA_STILL;

	struct block_data lava = {.type = BLOCK_LAVA_STILL};
	lighting_update_at_block(
		(struct world_modification_entry) {.x = 4, .y = 65, .z = 4, .blk = lava},
		false, g_get, g_set, NULL);

	// Lava cell glows at its luminance; an adjacent air cell one step down.
	assert(gworld[gidx(4, 65, 4)].torch == 15);
	assert(gworld[gidx(3, 65, 4)].torch == 14);

	// ---- EQUIVALENCE: one multi-source flood == N single-source floods ----
	// The server relight now seeds EVERY luminous block and floods once, instead
	// of flooding per block. Prove the two produce a byte-identical light field
	// (a clustered lava "lake" in an air cavern, where the floods overlap most).
	memset(gworld, 0, sizeof(gworld));
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++) {
			for(int y = 0; y <= 64; y++)
				gworld[gidx(x, y, z)].type = BLOCK_STONE;
			gheight[x + z * LW] = 67; // open sky at y >= 67; cavern at 65..66
		}
	for(int x = 1; x < LW - 1; x++)
		for(int z = 1; z < LW - 1; z++)
			for(int y = 65; y <= 66; y++)
				gworld[gidx(x, y, z)].type = BLOCK_AIR; // hollow cavern
	struct world_modification_entry lava_src[4];
	int nlava = 0;
	for(int x = 3; x <= 4; x++)
		for(int z = 3; z <= 4; z++) { // 2x2 lava lake
			gworld[gidx(x, 65, z)].type = BLOCK_LAVA_STILL;
			lava_src[nlava++] = (struct world_modification_entry) {
				.x = x, .y = 65, .z = z, .blk = {.type = BLOCK_LAVA_STILL}};
		}

	// (a) old way: flood from each lava block in turn
	for(int i = 0; i < nlava; i++)
		lighting_update_at_block(lava_src[i], false, g_get, g_set, NULL);
	static struct cell snap[LW * LW * H];
	memcpy(snap, gworld, sizeof(gworld));

	// (b) new way: zero the light (keep the blocks), then one multi-source flood
	for(size_t i = 0; i < (size_t)LW * LW * H; i++) {
		gworld[i].sky = 0;
		gworld[i].torch = 0;
	}
	lighting_update_at_blocks(lava_src, nlava, false, g_get, g_set, NULL);

	for(size_t i = 0; i < (size_t)LW * LW * H; i++)
		assert(gworld[i].sky == snap[i].sky && gworld[i].torch == snap[i].torch);
	printf("test_relight: multi-source flood == %d per-block floods "
		   "(byte-identical light field)\n",
		   nlava);

	free(sc.ids);
	free(sc.metadata);
	free(sc.lighting_sky);
	free(sc.lighting_torch);
	free(sc.heightmap);
	printf("test_relight: all assertions passed (canopy raises heightmap 65->73; "
		   "skylight shadowed under leaves; reset_torch=false preserves torch; "
		   "lava seeds block light 15->14)\n");
	return 0;
}
