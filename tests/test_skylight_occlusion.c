/*
	Equivalence + correctness test for the D1 gen-time horizontal skylight bleed
	(server_world.c server_world_skylight_bleed / _cell_skylight_underlit).

	worldgen_lighting sets each column's skylight TOP-DOWN with no horizontal bleed,
	so a cell under an overhang / just inside a cave mouth reads sky 0 even though
	open sky is one block to the side -> hostiles spawn there at noon. D1 seeds the
	runtime lighting flood from the "under-lit frontier" so it bleeds skylight in
	horizontally to Beta's fixed point.

	This drives the REAL vertical pass (worldgen_lighting) and the REAL flood
	(lighting_update_at_blocks from lighting.c), seeded by a frontier predicate
	transcribed from the production server_world_cell_skylight_underlit, and proves:

	  (A) CORRECTNESS: the seeded flood reaches the independent brute-force skylight
	      fixed point at EVERY cell (so the minimal frontier seed is complete -- the
	      flood cascades inward from the rim to fill deep tunnels).
	  (B) EQUIVALENCE: open columns (no transparent cell below their heightmap) are
	      byte-unchanged -- their vertical-pass skylight already IS the fixed point.
	  (C) the under-overhang cells that read 0 before now read the correct bled value.

	Links the real worldgen_lighting + lighting engine with a minimal real block table
	(mirrors tests/test_relight.c).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "lighting.h"
#include "network/server_world.h"
#include "worldgen/worldgen.h"

#define CW 16
#define CH 128
#define IDX(x, y, z) ((size_t)(y) + ((size_t)(z) + (size_t)(x) * CW) * CH)
#define HM(x, z) ((x) + (z) * CW)

// Minimal real block table (the flood reads can_see_through/opacity only here).
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_glass = {.can_see_through = true, .opacity = 0};
static struct block b_water = {.can_see_through = true, .opacity = 3};
struct block* blocks[256] = {0};
static void setup_blocks(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_DIRT] = blocks[BLOCK_GRASS]
		= blocks[BLOCK_BEDROCK] = &b_solid;
	blocks[BLOCK_GLASS] = &b_glass;
	blocks[BLOCK_WATER_STILL] = blocks[BLOCK_WATER_FLOW] = &b_water;
}

// lighting.c asks the registry for the six neighbour offsets; provide them directly
// (the flood visits all six, so order is irrelevant to the fixed point).
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

// ---- single-chunk flood harness over a struct server_chunk -----------------
static struct server_chunk* g_sc;
static bool sc_get(void* u, w_coord_t x, w_coord_t y, w_coord_t z,
				   struct block_data* blk, uint8_t* height) {
	(void)u;
	if(x < 0 || x >= CW || z < 0 || z >= CW || y < 0 || y >= CH)
		return false; // edge of the lone chunk -> flood stops (Beta lazy edge)
	size_t idx = IDX(x, y, z);
	if(blk) {
		blk->type = g_sc->ids[idx];
		blk->metadata = 0;
		blk->sky_light = nibble_read(g_sc->lighting_sky, idx);
		blk->torch_light = nibble_read(g_sc->lighting_torch, idx);
	}
	if(height)
		*height = g_sc->heightmap[HM(x, z)];
	return true;
}
static void sc_set(void* u, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	(void)u;
	if(x < 0 || x >= CW || z < 0 || z >= CW || y < 0 || y >= CH)
		return;
	size_t idx = IDX(x, y, z);
	nibble_write(g_sc->lighting_sky, idx, light & 0x0F);
	nibble_write(g_sc->lighting_torch, idx, light >> 4);
}

// Frontier predicate. Fetches the cell + its in-world neighbour sky values and defers
// the decision to the SHARED production skylight_cell_underlit (lighting.h) -- the same
// function server_world_cell_skylight_underlit calls -- so the seed-selection logic is
// pinned, not transcribed. (A bug in it that under-seeds makes assertion (A) below fail
// against the independent brute-force fixed point.)
static bool underlit(int lx, int y, int lz) {
	size_t idx = IDX(lx, y, lz);
	uint8_t id = g_sc->ids[idx];
	int sky = nibble_read(g_sc->lighting_sky, idx);
	int neighbour_sky[SIDE_MAX];
	int nc = 0;
	for(enum side s = 0; s < SIDE_MAX; s++) {
		int dx, dy, dz;
		blocks_side_offset(s, &dx, &dy, &dz);
		struct block_data nb;
		if(sc_get(NULL, lx + dx, y + dy, lz + dz, &nb, NULL))
			neighbour_sky[nc++] = nb.sky_light;
	}
	return skylight_cell_underlit(id, sky, neighbour_sky, nc);
}

// Seed the frontier and run the REAL production flood over the chunk.
static void run_skylight_bleed(void) {
	size_t n = 0;
	for(int lx = 0; lx < CW; lx++)
		for(int lz = 0; lz < CW; lz++)
			for(int y = 0; y < CH; y++)
				if(underlit(lx, y, lz))
					n++;
	if(n == 0)
		return;
	struct world_modification_entry* src = malloc(n * sizeof(*src));
	assert(src);
	size_t k = 0;
	for(int lx = 0; lx < CW && k < n; lx++)
		for(int lz = 0; lz < CW && k < n; lz++)
			for(int y = 0; y < CH && k < n; y++)
				if(underlit(lx, y, lz))
					src[k++] = (struct world_modification_entry) {
						.x = lx, .y = y, .z = lz};
	lighting_update_at_blocks(src, k, false, sc_get, sc_set, NULL);
	free(src);
}

// Independent brute-force skylight fixed point (Beta's flood recompute, iterated to
// stability) over the chunk's blocks + heightmap. THE reference the flood must hit.
static uint8_t bf[CW * CW * CH];
static void brute_force_skylight(void) {
	for(int lx = 0; lx < CW; lx++)
		for(int lz = 0; lz < CW; lz++) {
			uint8_t h = g_sc->heightmap[HM(lx, lz)];
			for(int y = 0; y < CH; y++)
				bf[IDX(lx, y, lz)] = (y >= h) ? 15 : 0;
		}
	bool changed = true;
	while(changed) {
		changed = false;
		for(int lx = 0; lx < CW; lx++)
			for(int lz = 0; lz < CW; lz++) {
				uint8_t h = g_sc->heightmap[HM(lx, lz)];
				for(int y = 0; y < CH; y++) {
					size_t idx = IDX(lx, y, lz);
					uint8_t id = g_sc->ids[idx];
					int want = (y >= h) ? 15 : 0;
					if(!blocks[id] || blocks[id]->can_see_through) {
						int opacity = blocks[id] ? blocks[id]->opacity : 1;
						if(opacity < 1)
							opacity = 1;
						for(enum side s = 0; s < SIDE_MAX; s++) {
							int dx, dy, dz;
							blocks_side_offset(s, &dx, &dy, &dz);
							int nx = lx + dx, ny = y + dy, nz = lz + dz;
							if(nx < 0 || nx >= CW || nz < 0 || nz >= CW || ny < 0
							   || ny >= CH)
								continue;
							int v = (int)bf[IDX(nx, ny, nz)] - opacity;
							if(v > want)
								want = v;
						}
					}
					if(bf[idx] != (uint8_t)want) {
						bf[idx] = (uint8_t)want;
						changed = true;
					}
				}
			}
	}
}

static struct server_chunk* alloc_chunk(void) {
	struct server_chunk* sc = calloc(1, sizeof(*sc));
	size_t cells = (size_t)CW * CW * CH;
	sc->ids = calloc(cells, 1);
	sc->metadata = calloc(cells / 2, 1);
	sc->lighting_sky = calloc(cells / 2, 1);
	sc->lighting_torch = calloc(cells / 2, 1);
	sc->heightmap = calloc(CW * CW, 1);
	assert(sc->ids && sc->lighting_sky && sc->lighting_torch && sc->heightmap);
	return sc;
}
static void flat_ground(struct server_chunk* sc) {
	for(int x = 0; x < CW; x++)
		for(int z = 0; z < CW; z++) {
			sc->ids[IDX(x, 0, z)] = BLOCK_BEDROCK;
			for(int y = 1; y <= 62; y++)
				sc->ids[IDX(x, y, z)] = BLOCK_STONE;
			sc->ids[IDX(x, 63, z)] = BLOCK_DIRT;
			sc->ids[IDX(x, 64, z)] = BLOCK_GRASS;
		}
}

int main(void) {
	setup_blocks();

	// ===== Scenario 1: an overhang roof over a covered air pocket =====
	g_sc = alloc_chunk();
	flat_ground(g_sc);
	// Stone roof at y=70 over x,z in [4,11]; air at y=65..69 beneath it. The roof
	// raises those columns' heightmap to 71, so the vertical pass leaves the covered
	// air at sky 0. The pocket is open on all four sides (x=3/12, z=3/12 columns are
	// bare ground -> their y=65..69 air is full sky 15) -> skylight should bleed in.
	for(int x = 4; x <= 11; x++)
		for(int z = 4; z <= 11; z++)
			g_sc->ids[IDX(x, 70, z)] = BLOCK_STONE; // the roof (y=65..69 stay air)

	// A water column at (1,1) in the open border: heightmap 65 (water counts in the
	// heightmap), but its sub-surface cells are TRANSPARENT and carry a NON-trivial
	// vertical skylight (12/9/6/3/0). The bleed must leave them byte-unchanged -- this
	// is the transparent-open-column case the (B) equivalence assertion exercises; an
	// all-solid open column is trivially pinned to 0/15 and proves nothing.
	for(int y = 60; y <= 64; y++)
		g_sc->ids[IDX(1, y, 1)] = BLOCK_WATER_STILL;

	worldgen_lighting(g_sc, true,
					  WORLD_DIM_OVERWORLD); // vertical pass: covered air -> sky 0

	// Roofed columns' heightmap is the roof top + 1; open columns stop at grass; the
	// water surface (y=64) counts, so its column heightmap is 65 with attenuated sky.
	assert(g_sc->heightmap[HM(8, 8)] == 71);
	assert(g_sc->heightmap[HM(0, 0)] == 65);
	assert(g_sc->heightmap[HM(1, 1)] == 65);
	assert(nibble_read(g_sc->lighting_sky, IDX(1, 64, 1)) == 12); // 15 - water opacity 3
	assert(nibble_read(g_sc->lighting_sky, IDX(1, 62, 1)) == 6);	 // 12 - 3 - 3
	// (C) BEFORE the bleed: the centre of the pocket is pitch dark (the D1 bug --
	// noon mob spawns), while the open column beside it is full daylight.
	assert(nibble_read(g_sc->lighting_sky, IDX(8, 67, 8)) == 0);
	assert(nibble_read(g_sc->lighting_sky, IDX(3, 67, 3)) == 15);

	// Snapshot the OPEN columns (x=0,15 and z=0,15 borders are all bare ground) to
	// prove the bleed leaves them byte-unchanged.
	uint8_t* snap = malloc((size_t)CW * CW * CH / 2);
	memcpy(snap, g_sc->lighting_sky, (size_t)CW * CW * CH / 2);

	run_skylight_bleed();
	brute_force_skylight();

	// (A) CORRECTNESS: every cell equals the independent brute-force fixed point.
	for(int x = 0; x < CW; x++)
		for(int z = 0; z < CW; z++)
			for(int y = 0; y < CH; y++) {
				uint8_t got = nibble_read(g_sc->lighting_sky, IDX(x, y, z));
				assert(got == bf[IDX(x, y, z)]);
			}
	// (C) AFTER: the pocket centre is lit (bled in from the open sides) and the rim
	// is brighter than the centre (1/block attenuation).
	uint8_t centre = nibble_read(g_sc->lighting_sky, IDX(8, 67, 8));
	uint8_t rim = nibble_read(g_sc->lighting_sky, IDX(4, 67, 8)); // 1 in from open x=3
	assert(centre > 0);
	assert(rim == 14); // one step (opacity 1) from the open sky-15 column at x=3
	assert(rim > centre);

	// (B) EQUIVALENCE: every OPEN column (heightmap 65) is byte-unchanged by the bleed.
	// This now includes the (1,1) WATER column, whose transparent sub-surface cells
	// carry attenuated skylight (12/9/6/3/0) -- the bleed must NOT touch them (an
	// all-solid open column is trivially 0/15 and could not catch a regression here).
	for(int x = 0; x < CW; x++)
		for(int z = 0; z < CW; z++) {
			bool open_col = g_sc->heightmap[HM(x, z)] == 65;
			if(!open_col)
				continue;
			for(int y = 0; y < CH; y++) {
				size_t idx = IDX(x, y, z);
				assert(nibble_read(g_sc->lighting_sky, idx)
					   == nibble_read(snap, idx));
			}
		}
	// And explicitly: the water column's attenuated skylight survived the bleed.
	assert(nibble_read(g_sc->lighting_sky, IDX(1, 64, 1)) == 12);
	assert(nibble_read(g_sc->lighting_sky, IDX(1, 62, 1)) == 6);
	free(snap);
	printf("test_skylight_occlusion: overhang pocket lit to the brute-force fixed "
		   "point (centre %u, rim %u); open columns byte-unchanged\n",
		   centre, rim);

	// ===== Scenario 2: a fully-flat OPEN world -> the bleed must be a no-op =====
	free(g_sc->ids);
	free(g_sc->metadata);
	free(g_sc->lighting_sky);
	free(g_sc->lighting_torch);
	free(g_sc->heightmap);
	free(g_sc);
	g_sc = alloc_chunk();
	flat_ground(g_sc);
	worldgen_lighting(g_sc, true, WORLD_DIM_OVERWORLD);
	uint8_t* before = malloc((size_t)CW * CW * CH / 2);
	memcpy(before, g_sc->lighting_sky, (size_t)CW * CW * CH / 2);
	run_skylight_bleed();
	assert(memcmp(before, g_sc->lighting_sky, (size_t)CW * CW * CH / 2) == 0);
	free(before);
	printf("test_skylight_occlusion: flat open world unchanged by the bleed "
		   "(strict equivalence)\n");

	free(g_sc->ids);
	free(g_sc->metadata);
	free(g_sc->lighting_sky);
	free(g_sc->lighting_torch);
	free(g_sc->heightmap);
	free(g_sc);
	printf("test_skylight_occlusion: all assertions passed\n");
	return 0;
}
