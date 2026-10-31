/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	WORLDGEN INVARIANT (WORLDGEN/REED, 2026-09-26, REED-SAND-DIAG-0926):

	  Every naturally generated sugar cane satisfies Beta 1.7.3
	  BlockReed.canPlaceBlockAt [BlockReed.java:33-36] AT THE MOMENT IT IS
	  WRITTEN: the block below is reed, OR it is grass/dirt with Material.water
	  (waterMoving 8 or waterStill 9, Block.java:31-32) on one of the four
	  sides of that base block. WorldGenReed.generate:14-16 writes each segment
	  only when canBlockStay (== canPlaceBlockAt) passes, so a Beta world can
	  never generate a reed on sand.

	This is a REAL-TERRAIN sweep (not a flat fixture): the Beta overworld
	generator builds an R x R region of chunks for each seed, then
	worldgen_populate runs over the (R-1) x (R-1) interior in raster order
	with the same 2x2-quadrant-spill semantics as the live server (chunk
	population needs its +X/+Z/+X+Z neighbours present). The set() hook
	evaluates the Beta predicate against the current world state on every
	reed write and records the first violation.

	Seeds/regions: 12345 and 42 include the two beach sites the diagnosis
	found (seed 12345 reed at (119,71,-78) on lake-shore sand, seed 42 reed at
	(-16,64,43) on sea-level beach sand); the rest are retained regression
	seeds from tests/seed_corpus.tsv around the origin.

	The world-view mirrors the live quad view (heightmap + saved sky light
	from the generated chunk) except tick_fluid_immediate, which is the
	server's internal raw fluid adapter and stays NULL here: spring sources
	(id 8) are still written, so the flowing-water branch of the gate is
	exercised; only their spread is absent.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/server_world.h" // struct server_chunk
#include "worldgen/populate.h"
#include "worldgen/worldgen.h"

#define LIDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * 16) * 128)

// Minimal real block table: worldgen_lighting / the heightmap need opacity for
// terrain + decoration blocks (mirrors tests/test_far_skin_equiv.c).
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_leaves = {.can_see_through = true, .opacity = 1};
static struct block b_water = {.can_see_through = true, .opacity = 3};
static struct block b_lava
	= {.can_see_through = false, .opacity = 15, .luminance = 15};
static struct block b_plant = {.can_see_through = true, .opacity = 0};
struct block* blocks[256] = {0};

static void setup_blocks(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_GRASS] = blocks[BLOCK_DIRT]
		= blocks[BLOCK_COBBLESTONE] = blocks[BLOCK_BEDROCK] = blocks[BLOCK_SAND]
		= blocks[BLOCK_GRAVEL] = blocks[BLOCK_LOG] = blocks[BLOCK_SANDSTONE]
		= blocks[BLOCK_CLAY] = &b_solid;
	blocks[BLOCK_WATER_FLOW] = blocks[BLOCK_WATER_STILL] = blocks[BLOCK_ICE]
		= &b_water;
	blocks[BLOCK_LAVA_FLOW] = blocks[BLOCK_LAVA_STILL] = &b_lava;
	blocks[BLOCK_LEAVES] = &b_leaves;
	blocks[BLOCK_SNOW] = blocks[BLOCK_FLOWER] = blocks[BLOCK_ROSE]
		= blocks[BLOCK_TALL_GRASS] = blocks[BLOCK_REED] = blocks[BLOCK_CACTUS]
		= &b_plant;
}

uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((data & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (data & 0x0F));
}

// R x R region of generated chunks; populate covers the (R-1) x (R-1) interior.
#define R 8

struct region {
	int cx0, cz0;
	struct server_chunk ch[R][R];
	// current populate origin (for the violation report)
	int pop_cx, pop_cz;
	// tally
	long reeds, bases, bases_beside_flow_only;
	long violations;
	char first_violation[256];
};

static struct server_chunk* rg_chunk(struct region* r, int x, int z, int* lx,
									 int* lz) {
	int cx = (x >> 4) - r->cx0, cz = (z >> 4) - r->cz0;
	if(cx < 0 || cz < 0 || cx >= R || cz >= R)
		return NULL;
	*lx = x & 15;
	*lz = z & 15;
	return &r->ch[cx][cz];
}
static uint8_t rg_get(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= 128)
		return 0;
	int lx, lz;
	struct server_chunk* sc = rg_chunk(ctx, x, z, &lx, &lz);
	return sc ? sc->ids[LIDX(lx, y, lz)] : 0;
}
static void rg_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct region* r = ctx;
	if(y < 0 || y >= 128)
		return;
	int lx, lz;
	struct server_chunk* sc = rg_chunk(r, x, z, &lx, &lz);
	if(!sc)
		return;
	if(id == BLOCK_REED) {
		// Beta BlockReed.canPlaceBlockAt against the world AS IT IS at this
		// write (WorldGenReed gates each segment right before setBlock).
		r->reeds++;
		uint8_t below = rg_get(r, x, y - 1, z);
		if(below != BLOCK_REED) {
			r->bases++;
			bool w_still = false, w_flow = false;
			const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
			for(int i = 0; i < 4; i++) {
				uint8_t n = rg_get(r, x + dx[i], y - 1, z + dz[i]);
				if(n == BLOCK_WATER_STILL)
					w_still = true;
				else if(n == BLOCK_WATER_FLOW)
					w_flow = true;
			}
			bool base_ok = (below == BLOCK_GRASS || below == BLOCK_DIRT);
			bool ok = base_ok && (w_still || w_flow);
			if(ok && w_flow && !w_still)
				r->bases_beside_flow_only++;
			if(!ok) {
				if(r->violations == 0)
					snprintf(r->first_violation, sizeof r->first_violation,
							 "reed base (%d,%d,%d) below=%u water_beside=%s "
							 "during populate(%d,%d)",
							 x, y, z, below,
							 (w_still || w_flow) ? "yes" : "no", r->pop_cx,
							 r->pop_cz);
				r->violations++;
			}
		}
	}
	sc->ids[LIDX(lx, y, lz)] = id;
	nibble_write(sc->metadata, LIDX(lx, y, lz), meta);
}
static int rg_height(void* ctx, int x, int z) {
	int lx, lz;
	struct server_chunk* sc = rg_chunk(ctx, x, z, &lx, &lz);
	return sc ? sc->heightmap[lx + lz * 16] : 0;
}
static int rg_skylight(void* ctx, int x, int y, int z) {
	if(y >= 128)
		return 15;
	if(y < 0)
		return 0;
	int lx, lz;
	struct server_chunk* sc = rg_chunk(ctx, x, z, &lx, &lz);
	return sc ? nibble_read(sc->lighting_sky, LIDX(lx, y, lz)) : 0;
}
static uint8_t rg_get_meta(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= 128)
		return 0;
	int lx, lz;
	struct server_chunk* sc = rg_chunk(ctx, x, z, &lx, &lz);
	return sc ? nibble_read(sc->metadata, LIDX(lx, y, lz)) : 0;
}

static void free_chunk(struct server_chunk* sc) {
	if(sc->ids) {
		free(sc->ids);
		free(sc->metadata);
		free(sc->lighting_sky);
		free(sc->lighting_torch);
		free(sc->heightmap);
		memset(sc, 0, sizeof *sc);
	}
}

// End-state cross-check (weaker than the write-time gate, reported only):
// reeds whose base is neither reed nor grass/dirt after the whole region is
// populated. Later features never turn grass/dirt to sand in Beta populate.
static long rg_end_state_bad_bases(struct region* r) {
	long bad = 0;
	for(int i = 0; i < R; i++)
		for(int j = 0; j < R; j++)
			for(int lx = 0; lx < 16; lx++)
				for(int lz = 0; lz < 16; lz++)
					for(int y = 1; y < 128; y++) {
						uint8_t* ids = r->ch[i][j].ids;
						if(ids[LIDX(lx, y, lz)] != BLOCK_REED)
							continue;
						uint8_t below = ids[LIDX(lx, y - 1, lz)];
						if(below != BLOCK_REED && below != BLOCK_GRASS
						   && below != BLOCK_DIRT)
							bad++;
					}
	return bad;
}

struct site {
	int64_t seed;
	int cx0, cz0; // region origin (chunk coords)
	const char* why;
};

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); // keep the per-seed lines on an assert abort
	setup_blocks();

	// Regions are R x R with the site of interest off-centre (chunk cx0+4,
	// cz0+4), matching the diagnosis reconstruction.
	const struct site sites[] = {
		{12345LL, 3, -9, "REED-SAND-DIAG-0926 site A: (119,71,-78) lake-shore sand"},
		{42LL, -5, -2, "REED-SAND-DIAG-0926 site B: (-16,64,43) beach sand"},
		{0x3FB27D23LL, -4, -4, "seed_corpus.tsv (test_giant_box_golden)"},
		{101LL, -4, -4, "seed_corpus.tsv (test_lava_aabb_golden)"},
		{0xA93A7AE2LL, -4, -4, "seed_corpus.tsv (test_lava_aabb_golden)"},
		// G4 assembly 2026-09-26: tree-heightmap 9afafd67 (live getHeightValue)
		// re-rooted trees, shifting populate draws so the 0x3FB27D23 region no
		// longer grows its one reed (4 reeds / 1 base with the tree fix reverted,
		// 0 with it) and the sweep went vacuous. These three DECOR/1 sweep
		// regions carry natural cane on the merged tree (5+4+3 reeds, 5 bases).
		{42LL, -2, 5, "decoration sweep region (desert), cane after tree-heightmap"},
		{5LL, 1, 0, "decoration sweep region (desert), cane after tree-heightmap"},
		{37LL, 8, -7, "decoration sweep region (rainforest/swamp), cane after tree-heightmap"},
	};
	const size_t nsites = sizeof(sites) / sizeof(sites[0]);

	struct region* r = calloc(1, sizeof *r);
	assert(r);

	long total_reeds = 0, total_bases = 0, total_flow_only = 0,
		 total_violations = 0, total_end_bad = 0;
	int chunks_populated = 0;

	for(size_t si = 0; si < nsites; si++) {
		const struct site* s = &sites[si];
		memset(r, 0, sizeof *r);
		r->cx0 = s->cx0;
		r->cz0 = s->cz0;

		struct worldgen* wg = worldgen_create(s->seed, WORLD_DIM_OVERWORLD);
		assert(wg && "worldgen_create failed");
		for(int i = 0; i < R; i++)
			for(int j = 0; j < R; j++) {
				bool ok = worldgen_generate_chunk(wg, &r->ch[i][j], s->cx0 + i,
												  s->cz0 + j);
				assert(ok && "worldgen_generate_chunk failed");
			}

		struct wgen_world w = {0};
		w.ctx = r;
		w.get = rg_get;
		w.set = rg_set;
		w.height = rg_height;
		w.skylight = rg_skylight;
		w.get_meta = rg_get_meta;

		for(int i = 0; i < R - 1; i++)
			for(int j = 0; j < R - 1; j++) {
				r->pop_cx = s->cx0 + i;
				r->pop_cz = s->cz0 + j;
				worldgen_populate(s->seed, r->pop_cx, r->pop_cz, &w,
								  worldgen_biome_at(wg, r->pop_cx * 16 + 16,
													r->pop_cz * 16 + 16),
								  worldgen_tree_density(wg, r->pop_cx * 16,
													   r->pop_cz * 16));
				chunks_populated++;
			}

		long end_bad = rg_end_state_bad_bases(r);
		printf("  seed %lld region (%d..%d,%d..%d): %ld reeds / %ld bases "
			   "(%ld beside flowing water only), %ld write-time violations, "
			   "%ld bad bases at end -- %s\n",
			   (long long)s->seed, s->cx0, s->cx0 + R - 1, s->cz0,
			   s->cz0 + R - 1, r->reeds, r->bases, r->bases_beside_flow_only,
			   r->violations, end_bad, s->why);
		if(r->violations)
			printf("    FIRST: %s\n", r->first_violation);

		total_reeds += r->reeds;
		total_bases += r->bases;
		total_flow_only += r->bases_beside_flow_only;
		total_violations += r->violations;
		total_end_bad += end_bad;

		for(int i = 0; i < R; i++)
			for(int j = 0; j < R; j++)
				free_chunk(&r->ch[i][j]);
		worldgen_destroy(wg);
	}
	free(r);

	// The sweep must actually exercise the rule (reeds exist in these regions)
	// and every base must pass Beta's gate at write time; the end-state count
	// is a cross-check that nothing in populate later undermines a base.
	assert(total_bases > 0 && "sweep placed no sugar cane -- vacuous");
	assert(total_violations == 0
		   && "a natural reed failed BlockReed.canPlaceBlockAt at placement");
	assert(total_end_bad == 0);

	printf("test_reed_placement_sweep: %ld reeds / %ld bases across %d populated "
		   "chunks in %zu seeded regions, all on grass/dirt/reed with Material."
		   "water (8|9) beside the base (%ld beside flowing water only) OK\n",
		   total_reeds, total_bases, chunks_populated, nsites, total_flow_only);
	return 0;
}
