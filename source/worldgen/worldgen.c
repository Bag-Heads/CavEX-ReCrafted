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

#include "../tps_observer.h"
tps_callback worldgen_tps_observe;


/*
	Beta 1.7.3 overworld terrain generator, ported to match
	ChunkProviderGenerate byte-for-byte.

	Pipeline per chunk (exactly Beta's order):
	  1. biome temperature/humidity (Simplex octaves) -- the terrain density
	     couples to humidity, so this is part of terrain, not an afterthought.
	  2. density field: a 5x17x5 low-resolution noise grid blending two Perlin
	     fields with a selector, biased by an elevation/continentalness term.
	  3. block fill: trilinearly interpolate the grid up to 16x128x16 and emit
	     stone (density > 0), else water/ice below sea level, else air.
	  4. surface replacement: cap stone with grass/dirt, carve sand/gravel
	     beaches near sea level, lay bedrock at the bottom (uses a per-chunk
	     java.util.Random seeded the way Beta does).

	Faithfulness notes:
	  - The eleven noise generators are built from the seed in Beta's exact
	    order (see worldgen_create); that order fixes every octave's offsets.
	  - Caves, ores, and decorations (trees/flowers/etc.) are applied AFTER
	    this stage in Beta (cave carver + populate()); they are the next
	    worldgen phase and are intentionally not done here. A chunk out of this
	    function is "terrain before caves", which is a real Beta intermediate.
	  - Biome-specific surface blocks (desert sand, etc.) and the biome map come
	    with biome classification in the following phase; the temperate
	    grass-over-dirt surface emitted here is what most of a Beta world uses.
*/

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h> // CLOCK_SEED_TIME: the spawn World.rand seed

#include "../block/blocks.h"
#include "../lighting.h"
#include "../network/server_world.h"
#include "../platform/mem2.h"
#include "../util.h"
#include "biome.h"
#include "cave.h"
#include "jmath.h"
#include "noise.h"
#include "simplex.h"
#include "worldgen.h"
#include "worldgen_internal.h" // struct worldgen + WG_IDX/WG_GRID_* (shared with worldgen_hell.c)

struct worldgen* worldgen_create(int64_t seed, enum world_dim dimension) {
	struct worldgen* wg = calloc(1, sizeof(*wg));
	if(!wg)
		return NULL;

	wg->seed = seed;
	wg->dimension = dimension;

	struct java_random r;
	jrand_set_seed(&r, seed);
	if(!octave_init(&wg->low, &r, 16)
	   || !octave_init(&wg->high, &r, 16)
	   || !octave_init(&wg->selector, &r, 8)
	   || !octave_init(&wg->sand_gravel, &r, 4)
	   || !octave_init(&wg->stone, &r, 4)
	   || !octave_init(&wg->continentalness, &r, 10)
	   || !octave_init(&wg->depth, &r, 16)) {
		worldgen_destroy(wg);
		return NULL;
	}
	wg->hell_initial_rng = r;
	if(!octave_init(&wg->tree_density, &r, 8)) {
		worldgen_destroy(wg);
		return NULL;
	}

	// Biome noise uses three independent Randoms seeded with Beta's magic
	// multipliers (computed in wrapping 64-bit, like Java's long arithmetic).
	struct java_random rt, rh, rw;
	jrand_set_seed(&rt, (int64_t)((uint64_t)seed * 9871ULL));
	jrand_set_seed(&rh, (int64_t)((uint64_t)seed * 39811ULL));
	jrand_set_seed(&rw, (int64_t)((uint64_t)seed * 543321ULL));
	if(!simplex_octaves_init(&wg->temperature, &rt, 4)
	   || !simplex_octaves_init(&wg->humidity, &rh, 4)
	   || !simplex_octaves_init(&wg->weirdness, &rw, 2)) {
		worldgen_destroy(wg);
		return NULL;
	}

	// Biome lookup table is seed-independent; build it once per world.
	biome_lut_init();

	return wg;
}

void worldgen_hell_initial_rng(struct worldgen* wg, struct java_random* rng) {
	assert(wg && rng);
	*rng = wg->hell_initial_rng;
}

void worldgen_destroy(struct worldgen* wg) {
	if(!wg)
		return;
	octave_free(&wg->low);
	octave_free(&wg->high);
	octave_free(&wg->selector);
	octave_free(&wg->sand_gravel);
	octave_free(&wg->stone);
	octave_free(&wg->continentalness);
	octave_free(&wg->depth);
	octave_free(&wg->tree_density);
	simplex_octaves_free(&wg->temperature);
	simplex_octaves_free(&wg->humidity);
	simplex_octaves_free(&wg->weirdness);
	free(wg);
}

// Beta WorldChunkManager / Beta173Biome: build the 16x16 temperature and
// humidity fields the terrain reads. Three Simplex octave fields are combined
// and clamped to [0,1]. (Biome classification from these values is a later
// phase; the terrain only needs temperature + humidity.)
static void worldgen_biome(struct worldgen* wg, w_coord_t chunk_x,
						   w_coord_t chunk_z) {
	double bx = (double)((w_coord_t)(chunk_x * CHUNK_SIZE));
	double bz = (double)((w_coord_t)(chunk_z * CHUNK_SIZE));

	simplex_octaves_generate(&wg->temperature, wg->temp, bx, bz, 16, 16, 0.025,
							 0.025, 0.25);
	simplex_octaves_generate(&wg->humidity, wg->humi, bx, bz, 16, 16, 0.05, 0.05,
							 1.0 / 3.0);
	simplex_octaves_generate(&wg->weirdness, wg->weird, bx, bz, 16, 16, 0.25,
							 0.25, 0.5882352941176471);

	int index = 0;
	for(int ix = 0; ix < 16; ix++) {
		for(int iz = 0; iz < 16; iz++) {
			double weird = wg->weird[index] * 1.1 + 0.5;

			double t = (wg->temp[index] * 0.15 + 0.7) * (1.0 - 0.01)
				+ weird * 0.01;
			double h = (wg->humi[index] * 0.15 + 0.5) * (1.0 - 0.002)
				+ weird * 0.002;

			t = 1.0 - (1.0 - t) * (1.0 - t);
			if(t < 0.0)
				t = 0.0;
			if(h < 0.0)
				h = 0.0;
			if(t > 1.0)
				t = 1.0;
			if(h > 1.0)
				h = 1.0;

			wg->temp[index] = t;
			wg->humi[index] = h;
			wg->biome_map[index] = (uint8_t)biome_classify(t, h);
			index++;
		}
	}
}

// Beta WorldChunkManager.getBiomeGenAt(x, z): the biome at a single world column,
// sampled as a 1x1 climate grid (byte-identical to one cell of the 16x16 field
// above -- the simplex generator samples cell i at world coordinate
// (off + i)*scale). The populate pass uses this for the chunk's +16,+16 corner
// to pick tree counts/types.
enum biome worldgen_biome_at(struct worldgen* wg, int world_x, int world_z) {
	double tn, hn, wn;
	simplex_octaves_generate(&wg->temperature, &tn, (double)world_x,
							 (double)world_z, 1, 1, 0.025, 0.025, 0.25);
	simplex_octaves_generate(&wg->humidity, &hn, (double)world_x, (double)world_z,
							 1, 1, 0.05, 0.05, 1.0 / 3.0);
	simplex_octaves_generate(&wg->weirdness, &wn, (double)world_x,
							 (double)world_z, 1, 1, 0.25, 0.25,
							 0.5882352941176471);

	double weird = wn * 1.1 + 0.5;
	double t = (tn * 0.15 + 0.7) * (1.0 - 0.01) + weird * 0.01;
	double h = (hn * 0.15 + 0.5) * (1.0 - 0.002) + weird * 0.002;
	t = 1.0 - (1.0 - t) * (1.0 - t);
	if(t < 0.0)
		t = 0.0;
	if(h < 0.0)
		h = 0.0;
	if(t > 1.0)
		t = 1.0;
	if(h > 1.0)
		h = 1.0;
	return biome_classify(t, h);
}

/* CFR NoiseGeneratorOctaves.java:28-35 / NoiseGeneratorPerlin.java:91-92:
 * the two-argument sample is generateNoise(x, z, 0), with z on the MIDDLE
 * Perlin axis. Original-JAR JNI doubles guard this independently. */
double worldgen_tree_density(struct worldgen* wg, int block_x, int block_z) {
	return octave_sample(&wg->tree_density, (double)block_x * 0.5,
		(double)block_z * 0.5, 0.0);
}

// Beta Beta173Biome.GenerateTemperature: the 16x16 temperature field the snow
// pass reads, recomputed at world block (bx, bz). Same Simplex temp + weirdness
// blend + clamp as worldgen_biome but TEMPERATURE ONLY (no humidity). Output is
// 256 doubles laid out [localX*16 + localZ], in [0,1].
void worldgen_temperature_grid(struct worldgen* wg, int bx, int bz,
							   double* out) {
	double temp[256], weird[256];
	simplex_octaves_generate(&wg->temperature, temp, (double)bx, (double)bz, 16,
							 16, 0.025, 0.025, 0.25);
	simplex_octaves_generate(&wg->weirdness, weird, (double)bx, (double)bz, 16,
							 16, 0.25, 0.25, 0.5882352941176471);
	for(int i = 0; i < 256; i++) {
		double w = weird[i] * 1.1 + 0.5;
		double t = (temp[i] * 0.15 + 0.7) * (1.0 - 0.01) + w * 0.01;
		t = 1.0 - (1.0 - t) * (1.0 - t);
		if(t < 0.0)
			t = 0.0;
		if(t > 1.0)
			t = 1.0;
		out[i] = t;
	}
}

// Beta ChunkProviderGenerate.initializeNoiseField: fill the 5x17x5 density grid.
static void worldgen_density(struct worldgen* wg, w_coord_t chunk_x,
							 w_coord_t chunk_z) {
	const int mx = WG_GRID_X, my = WG_GRID_Y, mz = WG_GRID_Z;
	double nx = (double)((w_coord_t)(chunk_x * 4));
	double nz = (double)((w_coord_t)(chunk_z * 4));
	const double hori = 684.412;
	const double vert = 684.412;

	// continentalness + depth are "2D" (size_y == 1); Beta still drives them
	// through the 3D octave path with y-offset 10.0 and y-scale 1.0.
	octave_generate(&wg->continentalness, wg->f_cont, nx, 10.0, nz, mx, 1, mz,
					1.121, 1.0, 1.121);
	octave_generate(&wg->depth, wg->f_depth, nx, 10.0, nz, mx, 1, mz, 200.0, 1.0,
					200.0);
	octave_generate(&wg->selector, wg->f_sel, nx, 0.0, nz, mx, my, mz,
					hori / 80.0, vert / 160.0, hori / 80.0);
	octave_generate(&wg->low, wg->f_low, nx, 0.0, nz, mx, my, mz, hori, vert,
					hori);
	octave_generate(&wg->high, wg->f_high, nx, 0.0, nz, mx, my, mz, hori, vert,
					hori);

	int xyz = 0; // index into the 3D fields (selector/low/high/density)
	int xz = 0;	 // index into the 2D fields (continentalness/depth)
	int scale_fraction = 16 / mx; // = 3

	for(int ix = 0; ix < mx; ix++) {
		int sample_x = ix * scale_fraction + scale_fraction / 2;

		for(int iz = 0; iz < mz; iz++) {
			int sample_z = iz * scale_fraction + scale_fraction / 2;

			double temp = wg->temp[sample_x * 16 + sample_z];
			double humi = wg->humi[sample_x * 16 + sample_z] * temp;
			humi = 1.0 - humi;
			humi *= humi;
			humi *= humi;
			humi = 1.0 - humi;

			double cont = (wg->f_cont[xz] + 256.0) / 512.0;
			cont *= humi;
			if(cont > 1.0)
				cont = 1.0;

			double depth = wg->f_depth[xz] / 8000.0;
			if(depth < 0.0)
				depth = -depth * 0.3;
			depth = depth * 3.0 - 2.0;
			if(depth < 0.0) {
				depth /= 2.0;
				if(depth < -1.0)
					depth = -1.0;
				depth /= 1.4;
				depth /= 2.0;
				cont = 0.0;
			} else {
				if(depth > 1.0)
					depth = 1.0;
				depth /= 8.0;
			}

			if(cont < 0.0)
				cont = 0.0;
			cont += 0.5;
			depth = depth * (double)my / 16.0;
			double elevation = (double)my / 2.0 + depth * 4.0;
			xz++;

			for(int iy = 0; iy < my; iy++) {
				double offset = ((double)iy - elevation) * 12.0 / cont;
				if(offset < 0.0)
					offset *= 4.0;

				double low = wg->f_low[xyz] / 512.0;
				double high = wg->f_high[xyz] / 512.0;
				double sel = (wg->f_sel[xyz] / 10.0 + 1.0) / 2.0;

				double d;
				if(sel < 0.0)
					d = low;
				else if(sel > 1.0)
					d = high;
				else
					d = low + (high - low) * sel;

				d -= offset;

				// Fade the top 3 grid rows toward solid-air so terrain never
				// reaches the build ceiling.
				if(iy > my - 4) {
					double fade = (double)((float)(iy - (my - 4)) / 3.0f);
					d = d * (1.0 - fade) + (-10.0 * fade);
				}

				wg->density[xyz] = d;
				xyz++;
			}
		}
	}
}

// Beta ChunkProviderGenerate.generateTerrain: trilinearly interpolate the
// density grid to full resolution and emit stone / water / ice / air.
static void worldgen_fill_blocks(struct worldgen* wg, struct server_chunk* sc) {
	const int my = WG_GRID_Y, mz = WG_GRID_Z;

	for(int mxi = 0; mxi < 4; mxi++) {		   // macro X: 4 blocks each
		for(int mzi = 0; mzi < 4; mzi++) {	   // macro Z
			for(int myi = 0; myi < 16; myi++) { // macro Y: 8 blocks each
				const double vstep = 0.125;

				double c000 = wg->density[((mxi + 0) * mz + mzi + 0) * my + myi + 0];
				double c010 = wg->density[((mxi + 0) * mz + mzi + 1) * my + myi + 0];
				double c100 = wg->density[((mxi + 1) * mz + mzi + 0) * my + myi + 0];
				double c110 = wg->density[((mxi + 1) * mz + mzi + 1) * my + myi + 0];
				double c001 = (wg->density[((mxi + 0) * mz + mzi + 0) * my + myi + 1] - c000) * vstep;
				double c011 = (wg->density[((mxi + 0) * mz + mzi + 1) * my + myi + 1] - c010) * vstep;
				double c101 = (wg->density[((mxi + 1) * mz + mzi + 0) * my + myi + 1] - c100) * vstep;
				double c111 = (wg->density[((mxi + 1) * mz + mzi + 1) * my + myi + 1] - c110) * vstep;

				for(int sy = 0; sy < 8; sy++) {
					const double hstep = 0.25;
					double tx0 = c000;
					double tx1 = c010;
					double sx0 = (c100 - c000) * hstep;
					double sx1 = (c110 - c010) * hstep;

					for(int sx = 0; sx < 4; sx++) {
						int bx = mxi * 4 + sx;
						int by = myi * 8 + sy;
						double d = tx0;
						double dstep = (tx1 - tx0) * hstep;

						for(int szi = 0; szi < 4; szi++) {
							int bz = mzi * 4 + szi;
							uint8_t type = BLOCK_AIR;

							if(by < WG_WATER_LEVEL) {
								double t = wg->temp[bx * 16 + bz];
								if(t < 0.5 && by >= WG_WATER_LEVEL - 1)
									type = BLOCK_ICE;
								else
									type = BLOCK_WATER_STILL;
							}
							if(d > 0.0)
								type = BLOCK_STONE;

							G9B_BYTE(sc->ids, WG_IDX(bx, by, bz), type);
							d += dstep;
						}

						tx0 += sx0;
						tx1 += sx1;
					}

					c000 += c001;
					c010 += c011;
					c100 += c101;
					c110 += c111;
				}
			}
		}
	}
}

// Beta ChunkProviderGenerate.replaceBlocksForBiome: cap stone columns with the
// biome surface, carve beaches, and lay bedrock.
//
// Index note (this is a real Beta subtlety, reproduced deliberately): Beta's
// surface loop variables are named (x, z) but its block index (z*16+x)*128+y
// resolves -- under the canonical x<<11|z<<7|y store -- to world column
// (worldX = z, worldZ = x). The sand/gravel/stone noise is read at the matching
// swapped index, so each world column ends up using the noise sampled at its
// OWN position; the swap cancels out and the result is per-column correct, just
// with the Random stream consumed in Beta's exact order. We therefore keep
// Beta's loop names and write WG_IDX(z, y, x).
static void worldgen_surface(struct worldgen* wg, struct server_chunk* sc,
							 w_coord_t chunk_x, w_coord_t chunk_z) {
	const double one32 = 1.0 / 32.0;
	double bx = (double)((w_coord_t)(chunk_x * CHUNK_SIZE));
	double bz = (double)((w_coord_t)(chunk_z * CHUNK_SIZE));

	octave_generate(&wg->sand_gravel, wg->sand_noise, bx, bz, 0.0, 16, 16, 1,
					one32, one32, 1.0);
	octave_generate(&wg->sand_gravel, wg->gravel_noise, bx, 109.0134, bz, 16, 1,
					16, one32, 1.0, one32);
	octave_generate(&wg->stone, wg->stone_noise, bx, bz, 0.0, 16, 16, 1,
					one32 * 2.0, one32 * 2.0, one32 * 2.0);

	struct java_random rand;
	jrand_set_seed(&rand,
				   (int64_t)chunk_x * 341873128712LL
					   + (int64_t)chunk_z * 132897987541LL);

	for(int x = 0; x < CHUNK_SIZE; x++) {
		for(int z = 0; z < CHUNK_SIZE; z++) {
			bool gen_sand
				= wg->sand_noise[x + z * 16] + jrand_next_double(&rand) * 0.2
				> 0.0;
			bool gen_gravel
				= wg->gravel_noise[x + z * 16] + jrand_next_double(&rand) * 0.2
				> 3.0;
			int stone_active = java_double_to_int(
				wg->stone_noise[x + z * 16] / 3.0 + 3.0
				+ jrand_next_double(&rand) * 0.25);
			int stone_depth = -1;

			// Beta surface blocks for this column's biome (desert -> sand,
			// otherwise grass over dirt). Read at [x + z*16] to match the
			// surface pass's x/z transpose, same as the sand/gravel noise.
			enum biome b = (enum biome)wg->biome_map[x + z * 16];
			uint8_t top_block = biome_top_block(b);
			uint8_t filler_block = biome_filler_block(b);

			for(int y = WORLD_HEIGHT - 1; y >= 0; y--) {
				size_t idx = WG_IDX(z, y, x);

				if(y <= jrand_next_int_bound(&rand, 5)) {
					G9B_BYTE(sc->ids, idx, BLOCK_BEDROCK);
					continue;
				}

				uint8_t cur = sc->ids[idx];
				if(cur == BLOCK_AIR) {
					stone_depth = -1;
					continue;
				}
				if(cur != BLOCK_STONE)
					continue;

				if(stone_depth == -1) {
					if(stone_active <= 0) {
						top_block = BLOCK_AIR;
						filler_block = BLOCK_STONE;
					} else if(y >= WG_WATER_LEVEL - 4
							  && y <= WG_WATER_LEVEL + 1) {
						top_block = biome_top_block(b);
						filler_block = biome_filler_block(b);
						if(gen_gravel) {
							top_block = BLOCK_AIR;
							filler_block = BLOCK_GRAVEL;
						}
						if(gen_sand) {
							top_block = BLOCK_SAND;
							filler_block = BLOCK_SAND;
						}
					}

					if(y < WG_WATER_LEVEL && top_block == BLOCK_AIR)
						top_block = BLOCK_WATER_STILL;

					stone_depth = stone_active;
					if(y >= WG_WATER_LEVEL - 1)
						G9B_BYTE(sc->ids, idx, top_block);
					else
						G9B_BYTE(sc->ids, idx, filler_block);
				} else if(stone_depth > 0) {
					stone_depth--;
					G9B_BYTE(sc->ids, idx, filler_block);
					// Sand beaches turn into sandstone underneath.
					if(stone_depth == 0 && filler_block == BLOCK_SAND) {
						stone_depth = jrand_next_int_bound(&rand, 4);
						filler_block = BLOCK_SANDSTONE;
					}
				}
			}
		}
	}
}

static bool worldgen_is_solid_surface(uint8_t type) {
	return blocks[type]
		&& (!blocks[type]->can_see_through || blocks[type]->opacity > 0);
}

/* CFR server Chunk.java:90-116 (func_353_b): fresh Overworld arrays are
 * already zero. Store the column height first, then only positive Sky from
 * y127 downward; Block is never written here. The neighbour scheduling pass
 * (:117-141) belongs to the owning world's provider, not this raw generator. */
static void worldgen_initial_lighting(struct server_chunk* sc) {
	for(int x = 0; x < CHUNK_SIZE; x++)
		for(int z = 0; z < CHUNK_SIZE; z++) {
			int height = WORLD_HEIGHT - 1;
			while(height > 0
				&& !worldgen_is_solid_surface(sc->ids[WG_IDX(x, height - 1, z)]))
				height--;
			G9B_BYTE(sc->heightmap, x + z * CHUNK_SIZE, (uint8_t)height);
			int sky = 15, y = WORLD_HEIGHT - 1;
			do {
				uint8_t id = sc->ids[WG_IDX(x, y, z)];
				int opacity = blocks[id] ? blocks[id]->opacity : 0;
				if(blocks[id] && !blocks[id]->can_see_through && !opacity)
					opacity = 15;
				sky -= opacity;
				if(sky > 0)
					G9B_NIBBLE(sc->lighting_sky, WG_IDX(x, y, z), (uint8_t)sky);
			} while(--y > 0 && sky > 0);
		}
}

// Per-column dimension lighting + heightmap. Horizontal sky-light bleed into
// overworld cave mouths is handled incrementally by the runtime lighting
// system. Reused after populate to refresh heightmap+sky for trees/leaves/snow:
// pass reset_torch=false there so it does NOT wipe block light already
// propagated from lava springs. The Nether still needs the heightmap/torch
// work, but its saved skylight must remain zero everywhere.
void worldgen_lighting(struct server_chunk* sc, bool reset_torch,
					   enum world_dim dimension) {
	assert(dimension == WORLD_DIM_OVERWORLD || dimension == WORLD_DIM_NETHER);
	const bool has_skylight = dimension == WORLD_DIM_OVERWORLD;
	for(int x = 0; x < CHUNK_SIZE; x++) {
		for(int z = 0; z < CHUNK_SIZE; z++) {
			int column_height = 0;
			int sky = has_skylight ? 15 : 0;

			for(int y = WORLD_HEIGHT - 1; y >= 0; y--) {
				size_t idx = WG_IDX(x, y, z);
				uint8_t type = sc->ids[idx];

				if(reset_torch)
					G9B_NIBBLE(sc->lighting_torch, idx, 0);

				if(!column_height && worldgen_is_solid_surface(type))
					column_height = y + 1;

				// Attenuate by THIS cell's opacity BEFORE storing, so the stored
				// skylight is the value the runtime lighting_flood recomputes (light
				// from above MINUS this cell's opacity): opaque surface -> 0, water
				// surface -> 12. The old order stored the PRE-attenuation value (every
				// opaque surface cell held 15), which DISAGREED with the flood -- so any
				// edit near a large open surface seeded a skylight "correction" that
				// cascaded across the whole region (the runaway relight behind the
				// far-ring 'lfc' ceiling hits; the far ring just streams more surface
				// into view). Beta subtracts first; oceans are now correctly dimmer.
				if(sky > 0 && blocks[type]) {
					int opacity = blocks[type]->can_see_through ?
						blocks[type]->opacity :
						(blocks[type]->opacity > 0 ? blocks[type]->opacity : 15);
					if(opacity > 0) {
						sky -= opacity;
						if(sky < 0)
							sky = 0;
					}
				}

				G9B_NIBBLE(sc->lighting_sky, idx, (uint8_t)sky);
			}

			G9B_BYTE(sc->heightmap, x + z * CHUNK_SIZE, (uint8_t)column_height);
		}
	}
}

static bool worldgen_alloc_chunk(struct server_chunk* sc) {
	size_t blocks_len = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	size_t nibble_len = blocks_len / 2;

	// MEM1-RELIEF (P0 crash fix): the 5 server-chunk arrays are ~80KB EACH (ids 32KB +
	// 3 nibble arrays 16KB + heightmap), and at view distance the resident set (~120
	// columns -> ~9.7MB) FAR exceeds the ~6MB MEM1 newlib heap they used to malloc from
	// -> OOM -> NULL deref crash a few frames into a real (non-superflat) world. Route
	// them to the idle 36MB MEM2 heap (mem2_alloc falls back to MEM1 only if MEM2 is
	// full, and mem2_free's address-range router handles either; on PC mem2_* == plain
	// malloc/free, so PC is byte-unchanged). EVERY free of these arrays MUST be mem2_free
	// (see server_world_chunk_destroy + the find_spawn/far-skin temps) or a plain free()
	// on a MEM2 pointer corrupts the heap.
	/* One MEM2_POOL_COLUMN slot for the five arrays (server_world.h, 2026-09-11). */
	if(!server_chunk_arrays_alloc(sc))
		return false;

	memset(sc->ids, BLOCK_AIR, blocks_len);
	memset(sc->metadata, 0, nibble_len);
	memset(sc->lighting_sky, 0, nibble_len);
	memset(sc->lighting_torch, 0, nibble_len);
	memset(sc->heightmap, 0, (size_t)CHUNK_SIZE * CHUNK_SIZE);
	G9B_ALLOC(sc);
	return true;
}

bool worldgen_find_nether_spawn(int64_t seed, float out[3]) {
	assert(out);

	// Spin up a temporary Nether generator to probe for a standable spawn -- a
	// column cell that is AIR with >=2 air of headroom sitting on solid netherrack
	// (or soulsand/gravel/bedrock), in the open band between the lava sea (y<32)
	// and the bedrock roof. Used only by the debug.start_nether boot flag.
	struct worldgen* wg = worldgen_create(seed, WORLD_DIM_NETHER);
	if(!wg)
		return false;

	bool found = false;
	for(int r = 0; r <= 4 && !found; r++) {
		for(int cx = -r; cx <= r && !found; cx++) {
			for(int cz = -r; cz <= r && !found; cz++) {
				if(r > 0 && abs(cx) != r && abs(cz) != r)
					continue; // perimeter ring only

				struct server_chunk sc;
				if(!worldgen_generate_chunk(wg, &sc, cx, cz))
					continue;

				for(int lx = 0; lx < CHUNK_SIZE && !found; lx++) {
					for(int lz = 0; lz < CHUNK_SIZE && !found; lz++) {
						// Search downward from just under the roof; first floor with
						// two air cells above it wins.
						for(int y = 100; y >= 33; y--) {
							uint8_t below = sc.ids[WG_IDX(lx, y - 1, lz)];
							uint8_t here = sc.ids[WG_IDX(lx, y, lz)];
							uint8_t above = sc.ids[WG_IDX(lx, y + 1, lz)];
							bool solid_floor = below == BLOCK_NETHERRACK
								|| below == BLOCK_SOULSAND || below == BLOCK_BEDROCK
								|| below == BLOCK_GRAVEL;
							if(solid_floor && here == BLOCK_AIR
							   && above == BLOCK_AIR) {
								out[0] = (float)(cx * CHUNK_SIZE + lx) + 0.5F;
								out[1] = (float)y;
								out[2] = (float)(cz * CHUNK_SIZE + lz) + 0.5F;
								found = true;
								break;
							}
						}
					}
				}

				/* One MEM2_POOL_COLUMN slot (worldgen_alloc_chunk, 2026-09-11). */
				server_chunk_arrays_free(&sc);
			}
		}
	}

	worldgen_destroy(wg);
	return found;
}

/* REL-03: Beta's new-world spawn search on raw generated terrain.
 * World.java:215-227 getInitialSpawnLocation: from (0, 0), while the column
 * is not spawnable, x += nextInt(64) - nextInt(64), then the same for z
 * (each draw its own statement: C leaves `a() - b()` order unspecified).
 * WorldProvider.java:37-39: spawnable = getFirstUncoveredBlock is sand;
 * World.java:245-250: y = 63, climb while the cell above is not air.
 * Like Beta the walk has no step limit. Only chunk-generation failure stops
 * it, and that is reported as a failure, never as a found spawn. */
#define WG_SPAWN_CACHE 4

struct wg_spawn_probe {
	struct worldgen* wg;
	struct server_chunk sc[WG_SPAWN_CACHE];
	w_coord_t cx[WG_SPAWN_CACHE], cz[WG_SPAWN_CACHE];
	bool used[WG_SPAWN_CACHE];
	int next;
};

/* World.getBlockId(x, y, z) on raw generated terrain; -1 if generation
 * failed. The caller keeps y inside [0, 128). */
static int wg_spawn_first_uncovered_block(struct wg_spawn_probe* p, int x,
										  int y, int z) {
	const w_coord_t cx = (w_coord_t)(x >> 4), cz = (w_coord_t)(z >> 4);
	int slot = -1;
	for(int i = 0; i < WG_SPAWN_CACHE && slot < 0; i++)
		if(p->used[i] && p->cx[i] == cx && p->cz[i] == cz)
			slot = i;
	if(slot < 0) {
		slot = p->next;
		p->next = (p->next + 1) % WG_SPAWN_CACHE;
		if(p->used[slot]) {
			server_chunk_arrays_free(&p->sc[slot]);
			p->used[slot] = false;
		}
		if(!worldgen_generate_chunk(p->wg, &p->sc[slot], cx, cz))
			return -1;
		p->used[slot] = true;
		p->cx[slot] = cx;
		p->cz[slot] = cz;
	}
	const int lx = x & (CHUNK_SIZE - 1), lz = z & (CHUNK_SIZE - 1);
	return p->sc[slot].ids[WG_IDX(lx, y, lz)];
}

/* World.getFirstUncoveredBlock over any column provider (World.java:245-250):
 * y = 63, climb while getBlockId(x, y+1, z) != air; y >= 128 reads as air.
 * -1 when the provider could not supply the chunk. */
static int wg_spawn_first_uncovered_in(const struct worldgen_spawn_world* world,
									   int x, int z) {
	int y = 63;
	for(;;) {
		const int above = y + 1 < WORLD_HEIGHT
			? world->block_id(world->ctx, x, y + 1, z)
			: BLOCK_AIR;
		if(above < 0)
			return -1;
		if(above == BLOCK_AIR)
			break;
		y++;
	}
	return world->block_id(world->ctx, x, y, z);
}

bool worldgen_beta_spawn_search(const struct worldgen_spawn_world* world,
								struct java_random* rng, int* out_x,
								int* out_z) {
	assert(world && world->block_id && rng && out_x && out_z);
	bool found = false;
	int x = 0, z = 0;
	for(;;) {
		const int id = wg_spawn_first_uncovered_in(world, x, z);
		if(id < 0)
			break;
		if(id == BLOCK_SAND) {
			found = true;
			break;
		}
		int a = jrand_next_int_bound(rng, 64);
		int b = jrand_next_int_bound(rng, 64);
		x += a - b;
		a = jrand_next_int_bound(rng, 64);
		b = jrand_next_int_bound(rng, 64);
		z += a - b;
	}
	if(!found)
		fprintf(stderr, "[worldgen] spawn search FAILED: chunk generation "
						"failed at (%d, %d); no Beta spawn found\n", x, z);
	*out_x = x;
	*out_z = z;
	return found;
}

static int wg_spawn_probe_block_id(void* ctx, int x, int y, int z) {
	struct wg_spawn_probe* p = ctx;
	return wg_spawn_first_uncovered_block(p, x, y, z);
}

/* The live-world search (server_local_beta_spawn_search) when a session
 * installed one; NULL in standalone worldgen tools and front-end rigs. */
static worldgen_spawn_search_fn s_spawn_search_override;

void worldgen_set_spawn_search_override(worldgen_spawn_search_fn fn) {
	s_spawn_search_override = fn;
}

static bool wg_beta_spawn_search(int64_t seed, struct java_random* rng,
								 int* out_x, int* out_z) {
	if(s_spawn_search_override)
		return s_spawn_search_override(seed, rng, out_x, out_z);
	/* Standalone fallback: raw generated terrain, BEFORE population. Beta's
	 * search reads chunks that ChunkProviderServer.loadChunk has already
	 * populated when their neighbours were resident, and that population
	 * draws World.rand (lava/water hardening fizz), so this probe can walk a
	 * different path than Beta for some seeds (sweep seed 3141592653589793238).
	 * Said out loud rather than hidden: the live search is the Beta one. */
	fprintf(stderr, "[worldgen] spawn search on unpopulated terrain (no live "
					"world installed): may differ from Beta\n");
	struct wg_spawn_probe p;
	memset(&p, 0, sizeof(p));
	p.wg = worldgen_create(seed, WORLD_DIM_OVERWORLD);
	if(!p.wg) {
		fprintf(stderr, "[worldgen] spawn search FAILED: could not create "
						"the generator; no Beta spawn found\n");
		return false;
	}
	const struct worldgen_spawn_world world
		= {.ctx = &p, .block_id = wg_spawn_probe_block_id};
	const bool found = worldgen_beta_spawn_search(&world, rng, out_x, out_z);
	for(int i = 0; i < WG_SPAWN_CACHE; i++)
		if(p.used[i])
			server_chunk_arrays_free(&p.sc[i]);
	worldgen_destroy(p.wg);
	return found;
}

/* World.rand: clock-seeded like Beta (World.java:169 new Random(), not the
 * terrain seed; maintainer ruling 2026-10-17), then the constructor's
 * nextInt(12000) soundCounter draw (World.java:177). Fixed under the test
 * boundary so suite runs reproduce (CW_TEST_SEED is not read: standalone
 * worldgen targets build without util.c). */
#ifdef CAVEWII_RAND_SEED_TEST
#define WG_SPAWN_CLOCK_SEED() ((int64_t)RAND_TEST_SEED_DEFAULT)
#else
#define WG_SPAWN_CLOCK_SEED() ((int64_t)CLOCK_SEED_TIME())
#endif
/* REL-03b: outcome of the most recent Overworld spawn search (see
 * worldgen_last_spawn_search_ok in worldgen.h). Written and read on the
 * thread that creates the world. */
static bool s_last_spawn_search_ok = true;

bool worldgen_last_spawn_search_ok(void) { return s_last_spawn_search_ok; }

#ifdef CAVEWII_RAND_SEED_TEST
/* Test pin: the next clock-seeded spawn search starts from this exact 48-bit
 * World.rand state (an oracle fixture's pinned state after soundCounter). */
static bool s_test_spawn_state_pinned;
static uint64_t s_test_spawn_state48;
void worldgen_test_pin_spawn_rng_state(bool pinned, uint64_t state48) {
	s_test_spawn_state_pinned = pinned;
	s_test_spawn_state48 = state48;
}
#endif

static void wg_spawn_rng_init(struct java_random* rng) {
	memset(rng, 0, sizeof(*rng));
#ifdef CAVEWII_RAND_SEED_TEST
	if(s_test_spawn_state_pinned) {
		rng->seed = s_test_spawn_state48;
		return;
	}
#endif
	jrand_set_seed(rng, WG_SPAWN_CLOCK_SEED());
	(void)jrand_next_int_bound(rng, 12000);
}

/* setSpawn(x, 64, z) as SpawnXYZ (x+0.5, 64, z+0.5) and first-join feet
 * (x+0.5, 65, z+0.5) (EntityPlayer.java:40-48). The push-up runs after
 * population on load (Entity.java:96-109). Outputs are untouched on failure. */
static bool wg_overworld_spawns(int64_t seed, struct java_random* rng,
								float world_spawn[3], float player_spawn[3]) {
	int x = 0, z = 0;
	s_last_spawn_search_ok = wg_beta_spawn_search(seed, rng, &x, &z);
	if(!s_last_spawn_search_ok)
		return false;
	world_spawn[0] = player_spawn[0] = (float)x + 0.5F;
	world_spawn[1] = 64.0F;
	player_spawn[1] = 65.0F;
	world_spawn[2] = player_spawn[2] = (float)z + 0.5F;
	return true;
}

bool worldgen_find_spawn(int64_t seed, float out[3]) {
	assert(out);
	struct java_random rng;
	float player[3];
	wg_spawn_rng_init(&rng);
	return wg_overworld_spawns(seed, &rng, out, player);
}

void worldgen_find_initial_spawns(int64_t seed, bool superflat,
								  bool start_nether, float world_spawn[3],
								  float player_spawn[3]) {
	struct java_random rng;
	wg_spawn_rng_init(&rng);
	worldgen_find_initial_spawns_rng(seed, superflat, start_nether, &rng,
									 world_spawn, player_spawn);
}

void worldgen_find_initial_spawns_rng(int64_t seed, bool superflat,
	bool start_nether, struct java_random* spawn_rng, float world_spawn[3],
	float player_spawn[3]) {
	assert(world_spawn && player_spawn && spawn_rng);

	world_spawn[0] = player_spawn[0] = 0.5F;
	world_spawn[1] = player_spawn[1] = superflat ? 5.0F : 64.0F;
	world_spawn[2] = player_spawn[2] = 0.5F;
	if(superflat) {
		s_last_spawn_search_ok = true; /* fixed y=5 start, no search */
		return;
	}

	// SpawnXYZ always belongs to the Overworld, including when the debug profile
	// initially places Player.Pos in the Nether.
	// On generation failure (logged) the (0.5, 64, 0.5) defaults stay in the
	// outputs, but worldgen_last_spawn_search_ok() reports false: the caller
	// must not create the world (REL-03b, maintainer ruling 2026-10-17).
	wg_overworld_spawns(seed, spawn_rng, world_spawn, player_spawn);
	if(start_nether && !worldgen_find_nether_spawn(seed, player_spawn)) {
		player_spawn[0] = 0.5F;
		player_spawn[1] = 65.0F;
		player_spawn[2] = 0.5F;
	}
}

void worldgen_set_superflat(struct worldgen* wg, bool superflat) {
	assert(wg);
	wg->superflat = superflat;
}

// Sub-phase profiler for the terrain-gen hotspot hunt. Compile-gated behind
// WORLDGEN_PROFILE (set only by bench_worldgen), so production pays nothing and
// never depends on clock_gettime. WG_PHASE(idx, call) times one phase into a
// per-phase accumulator; worldgen_phase_ms reads them.
#ifdef WORLDGEN_PROFILE
#include <time.h>
static double s_wg_phase_ms[6]; // biome, density, fill, surface, cave, lighting
static double wg_now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6;
}
void worldgen_phase_ms(double out[6]) {
	for(int i = 0; i < 6; i++)
		out[i] = s_wg_phase_ms[i];
}
#define WG_PHASE(idx, call)                                                    \
	do {                                                                       \
		double _a = wg_now_ms();                                               \
		call;                                                                  \
		s_wg_phase_ms[idx] += wg_now_ms() - _a;                                \
	} while(0)
#else
#define WG_PHASE(idx, call)                                                    \
	do {                                                                       \
		call;                                                                  \
	} while(0)
#endif

// W1-d (far-reach program Wave 1): shared body for worldgen_generate_chunk
// and worldgen_generate_chunk_far_skin, differing ONLY in whether phase 5
// (worldgen_lighting) runs. compute_lighting=true reproduces the original
// worldgen_generate_chunk byte-for-byte (both public entry points below are
// thin wrappers over this). The Nether dispatch ignores compute_lighting --
// out of this item's scope, see worldgen_generate_chunk_far_skin's header
// comment.
static bool worldgen_generate_chunk_impl(struct worldgen* wg,
										 struct server_chunk* sc,
										 w_coord_t chunk_x, w_coord_t chunk_z,
										 bool compute_lighting,
										 struct java_random* hell_rng) {
	assert(wg && sc);

	// NOT server_chunk_init(sc) here: that lives in server_world.c, which
	// test_worldgen's own minimal link set (worldgen.c + its direct deps,
	// no server_world.c) deliberately excludes. This inline memset is the
	// one already-safe site the boot-hang audit found (see server_chunk_
	// init's doc comment) -- keep it self-contained.
	memset(sc, 0, sizeof(*sc));
	if(!worldgen_alloc_chunk(sc))
		return false;

	if(wg->superflat) {
		// Flat debug stage: bedrock, two dirt, grass; everything above stays AIR
		// (already memset). No caves/ores here; trees/decoration are skipped by
		// the populate pass (see server_world_populate_quadrant). This debug
		// override applies to any dimension.
		for(int x = 0; x < CHUNK_SIZE; x++)
			for(int z = 0; z < CHUNK_SIZE; z++) {
				G9B_BYTE(sc->ids, WG_IDX(x, 0, z), BLOCK_BEDROCK);
				G9B_BYTE(sc->ids, WG_IDX(x, 1, z), BLOCK_DIRT);
				G9B_BYTE(sc->ids, WG_IDX(x, 2, z), BLOCK_DIRT);
				G9B_BYTE(sc->ids, WG_IDX(x, 3, z), BLOCK_GRASS);
			}
		if(compute_lighting) {
			tps_note(worldgen_tps_observe, TPS_ENTER, TPS_INITIAL_LIGHT);
			worldgen_lighting(sc, true, wg->dimension);
			tps_note(worldgen_tps_observe, TPS_LEAVE, TPS_INITIAL_LIGHT);
		}
		sc->modified = true;
		return true;
	}

	// The Nether is a distinct Beta provider (ChunkProviderHell): its whole
	// terrain+surface+cave+lighting pass lives in worldgen_hell.c and reuses this
	// generator's octaves (Beta builds Hell's from the same Random(seed) in the
	// same order). WORLD_DIM_NETHER and WORLD_DIM_OVERWORLD are the only dims.
	// compute_lighting is NOT threaded through here (see worldgen_generate_
	// chunk_far_skin's header comment) -- Nether height/light arrays stay zero.
	if(wg->dimension == WORLD_DIM_NETHER)
		return worldgen_hell_generate_chunk(wg, sc, chunk_x, chunk_z, hell_rng);

	WG_PHASE(0, worldgen_biome(wg, chunk_x, chunk_z));
	WG_PHASE(1, worldgen_density(wg, chunk_x, chunk_z));
	WG_PHASE(2, worldgen_fill_blocks(wg, sc));
	WG_PHASE(3, worldgen_surface(wg, sc, chunk_x, chunk_z));
	// Carve caves into the finished terrain (Beta runs this after surface
	// replacement, before the heightmap/lighting).
	WG_PHASE(4, cave_generate(wg->seed, chunk_x, chunk_z, sc->ids));
	if(compute_lighting) {
		tps_note(worldgen_tps_observe, TPS_ENTER, TPS_INITIAL_LIGHT);
		WG_PHASE(5, worldgen_initial_lighting(sc));
		tps_note(worldgen_tps_observe, TPS_LEAVE, TPS_INITIAL_LIGHT);
	}

	sc->modified = true;
	return true;
}

bool worldgen_generate_chunk(struct worldgen* wg, struct server_chunk* sc,
							 w_coord_t chunk_x, w_coord_t chunk_z) {
	return worldgen_generate_chunk_impl(wg, sc, chunk_x, chunk_z, true, NULL);
}

bool worldgen_generate_chunk_provider(struct worldgen* wg,
	struct server_chunk* sc, w_coord_t chunk_x, w_coord_t chunk_z,
	struct java_random* hell_rng) {
	return worldgen_generate_chunk_impl(wg, sc, chunk_x, chunk_z, true, hell_rng);
}

bool worldgen_generate_chunk_far_skin(struct worldgen* wg,
									  struct server_chunk* sc,
									  w_coord_t chunk_x, w_coord_t chunk_z) {
	return worldgen_generate_chunk_impl(wg, sc, chunk_x, chunk_z, false, NULL);
}

void worldgen_climate(struct worldgen* wg, w_coord_t chunk_x, w_coord_t chunk_z,
					  uint8_t* grass_i, uint8_t* grass_j) {
	assert(wg && grass_i && grass_j);

	// Reuse the exact biome temperature/humidity computation (transformed and
	// clamped to [0,1]); this also refreshes the scratch biome_map harmlessly.
	worldgen_biome(wg, chunk_x, chunk_z);

	// Beta ColorizerGrass/Foliage index math, per column: humidity *=
	// temperature, then i = (1-temperature)*255 and j = (1-humidity)*255. The
	// same (i,j) pair indexes both grasscolor.png and foliagecolor.png on the
	// client. Layout is [localX*16 + localZ], matching temp/humidity above.
	for(int i = 0; i < 256; i++) {
		double temp = wg->temp[i];
		double humi = wg->humi[i] * temp;
		int ci = (int)((1.0 - temp) * 255.0);
		int cj = (int)((1.0 - humi) * 255.0);
		if(ci < 0)
			ci = 0;
		if(ci > 255)
			ci = 255;
		if(cj < 0)
			cj = 0;
		if(cj > 255)
			cj = 255;
		grass_i[i] = (uint8_t)ci;
		grass_j[i] = (uint8_t)cj;
	}
}
