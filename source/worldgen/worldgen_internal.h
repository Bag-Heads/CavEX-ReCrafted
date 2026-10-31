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
	Private worldgen internals shared between the overworld generator
	(worldgen.c) and the Nether/Hell generator (worldgen_hell.c).

	`struct worldgen` and the chunk-index / grid macros used to live in
	worldgen.c; they were lifted here verbatim (pure code motion, proven
	byte-identical by test_worldgen + test_oracle_worldgen) so the Hell
	provider can reuse the SAME NoiseGeneratorOctaves objects. This is not a
	convenience: Beta's ChunkProviderHell builds its first seven octaves from
	`new Random(seed)` in the exact order ChunkProviderGenerate builds its
	first seven, so wg->{low,high,selector,sand_gravel,stone,continentalness,
	depth} are byte-for-byte the Hell provider's field_4169_i..field_4176_b.
	Reusing them (rather than reconstructing) keeps the RNG stream identical
	and avoids a second octave allocation. This header is NOT public API --
	only the two generator .c files include it.
*/

#ifndef WORLDGEN_INTERNAL_H
#define WORLDGEN_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "../world.h"
#include "noise.h"
#include "simplex.h"
#include "worldgen.h"

// Block index within a chunk: matches server_world's S_CHUNK_IDX and Beta's
// x<<11 | z<<7 | y storage order.
#define WG_IDX(x, y, z) ((size_t)(y) + ((size_t)(z) + (size_t)(x) * CHUNK_SIZE) * WORLD_HEIGHT)

#define WG_WATER_LEVEL 64 // Beta sea level (CHUNK_HEIGHT / 2)

// Low-resolution density grid: 5x17x5 samples (4 blocks apart horizontally, 8
// apart vertically), interpolated to the full 16x128x16 chunk. The Nether
// provider uses the SAME grid dimensions.
#define WG_GRID_X 5
#define WG_GRID_Y 17
#define WG_GRID_Z 5
#define WG_GRID_3D (WG_GRID_X * WG_GRID_Y * WG_GRID_Z)
#define WG_GRID_2D (WG_GRID_X * WG_GRID_Z)

struct worldgen {
	int64_t seed;
	enum world_dim dimension;
	// CFR ChunkProviderHell.java:61-71: shared Random after seven octaves.
	struct java_random hell_initial_rng;

	// Terrain Perlin octave generators, built from one Random(seed) IN THIS
	// ORDER -- the order is load-bearing, it determines each octave's offsets.
	// The Nether provider (worldgen_hell.c) reuses the first SEVEN of these;
	// Beta's ChunkProviderHell constructs exactly these seven in this order.
	struct octave_noise low;			 // 16 octaves  (Hell field_4169_i)
	struct octave_noise high;			 // 16          (Hell field_4168_j)
	struct octave_noise selector;		 // 8           (Hell field_4167_k)
	struct octave_noise sand_gravel;	 // 4           (Hell field_4166_l)
	struct octave_noise stone;			 // 4           (Hell field_4165_m)
	struct octave_noise continentalness; // 10          (Hell field_4177_a)
	struct octave_noise depth;			 // 16          (Hell field_4176_b)
	struct octave_noise tree_density;	 // 8 (overworld decoration only; built
										 //    here so the seed's Random stream
										 //    advances exactly as Beta's does)

	// Biome Simplex octave generators (separate seeds; overworld only).
	struct simplex_octaves temperature; // 4
	struct simplex_octaves humidity;	// 4
	struct simplex_octaves weirdness;	// 2

	// Per-chunk scratch, reused across chunks to avoid reallocation. The Nether
	// density/surface pass reuses these buffers (its grids are the same size:
	// 5x17x5 3D and 16x16 2D fit density/f_sel/f_low/f_high and
	// sand_noise/gravel_noise/stone_noise respectively).
	double density[WG_GRID_3D];
	double f_cont[WG_GRID_2D];
	double f_depth[WG_GRID_2D];
	double f_sel[WG_GRID_3D];
	double f_low[WG_GRID_3D];
	double f_high[WG_GRID_3D];
	double temp[256];
	double humi[256];
	double weird[256];
	double sand_noise[256];
	double gravel_noise[256];
	double stone_noise[256];
	// Beta biome id per column, classified from the temp/humidity above.
	uint8_t biome_map[256];

	// Debug ("debug.superflat"): when set, worldgen_generate_chunk emits flat
	// bedrock/dirt/grass layers instead of Beta terrain, and the server skips the
	// populate pass (no caves/trees/ores/snow). A clean flat stage for testing
	// mobs/entities -- NOT Beta-faithful, strictly debug-gated.
	bool superflat;
};

// Beta ChunkProviderHell provider, implemented in worldgen_hell.c. Fills sc
// (already allocated + AIR-cleared by the caller) with Nether terrain: the
// roof/floor/lava-sea density, the netherrack/soulsand/gravel surface pass, the
// MapGenCavesHell carve, and sky-light 0 everywhere (the Nether has no sky).
// Returns false only on an internal invariant failure. Deterministic per
// (seed, chunk_x, chunk_z) -- the surface RNG is (chunk_x,chunk_z)-seeded and the
// caves use the world seed, exactly as Beta's provideChunk does.
bool worldgen_hell_generate_chunk(struct worldgen* wg, struct server_chunk* sc,
								  w_coord_t chunk_x, w_coord_t chunk_z,
								  struct java_random* rng);

// worldgen_hell_populate is the Nether decorate entry point; it is a PUBLIC
// entry (the server calls it), so it is declared in worldgen.h, not here.

#endif
