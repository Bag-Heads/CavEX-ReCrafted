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

#ifndef WORLDGEN_H
#define WORLDGEN_H

#include <stdbool.h>
#include <stdint.h>

#include "../world.h"
#include "biome.h"

struct server_chunk;
struct java_random;

// Minecraft Beta 1.7.3 overworld terrain generator (ChunkProviderGenerate).
// The generator owns the eleven NoiseGeneratorOctaves Beta builds once per
// world from the seed (eight Perlin for terrain, three Simplex for biome
// temperature/humidity/weirdness) plus per-chunk scratch, so it is created
// once when a world is opened and reused for every chunk -- matching Beta,
// where the noise generators are constructed in the provider's constructor.
struct worldgen;

struct worldgen* worldgen_create(int64_t seed, enum world_dim dimension);
void worldgen_destroy(struct worldgen* wg);

// CFR ChunkProviderHell.java:61-71: actual constructor Random state, before
// any terrain reseed. Useful when the first chunks come from disk.
void worldgen_hell_initial_rng(struct worldgen* wg, struct java_random* rng);

// Debug ("debug.superflat"): switch the generator to a flat bedrock/dirt/grass
// stage (no Beta terrain). The server also skips the populate pass when set, so
// there are no caves/trees/ores/snow -- a clean ground for testing mobs.
void worldgen_set_superflat(struct worldgen* wg, bool superflat);

// Generate one chunk. Allocates and fills sc->{ids,metadata,lighting_*,
// heightmap} (the caller must not have allocated them) and marks it modified.
// Returns false on allocation failure. Overworld and Nether dispatch to their
// respective Beta providers; debug superflat is dimension-lighting aware.
bool worldgen_generate_chunk(struct worldgen* wg, struct server_chunk* sc,
							 w_coord_t chunk_x, w_coord_t chunk_z);

// CFR server ChunkProviderHell.java:202-209,300-347: terrain reseeds and
// consumes the same Random as population. NULL retains standalone terrain
// generation semantics; Overworld generation never consumes this stream.
bool worldgen_generate_chunk_provider(struct worldgen* wg,
	struct server_chunk* sc, w_coord_t chunk_x, w_coord_t chunk_z,
	struct java_random* hell_rng);

// W1-d (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md
// "worldgen fast path (a)"): identical to worldgen_generate_chunk for the
// OVERWORLD (and the debug superflat stage) EXCEPT it skips phase 5
// (worldgen_lighting) -- sc->{lighting_sky,lighting_torch,heightmap} are
// still ALLOCATED (worldgen_alloc_chunk, uniform with every other chunk)
// but left at their post-memset zero fill, never computed. T1 dead work:
// nothing else this function computes reads those arrays back, and
// sc->ids -- the only thing a far-skin extraction (server_world_extract_
// far_skin) reads -- is written entirely by phases 0-4, before lighting
// ever runs. The Nether dispatch (worldgen_hell_generate_chunk) is
// unchanged: its provide-time height/light arrays remain zero (CFR server
// ChunkProviderHell.java:202-209). Proof: test_far_skin_equiv's skip CASE
// (byte-identical sc->ids to the full worldgen_generate_chunk path across
// the same seed matrix) plus its printed per-column timing delta.
bool worldgen_generate_chunk_far_skin(struct worldgen* wg,
									  struct server_chunk* sc,
									  w_coord_t chunk_x, w_coord_t chunk_z);

// Cumulative per-phase timings (ms) of worldgen_generate_chunk, for the bench's
// terrain-gen hotspot profiling. Order: biome, density, fill, surface, cave,
// lighting. Only populated when built with -DWORLDGEN_PROFILE (bench only).
void worldgen_phase_ms(double out[6]);

// Recompute a chunk's per-column dimension lighting + heightmap from its
// current blocks. Called once at terrain init (reset_torch=true: zero block
// light first) and again after populate (reset_torch=false: preserve
// lava-spring block light). The Nether has no skylight, so its sky array is
// forced to zero while the heightmap and optional torch reset still run.
void worldgen_lighting(struct server_chunk* sc, bool reset_torch,
					   enum world_dim dimension);

// Overworld SpawnXYZ (x+0.5, 64, z+0.5) from Beta's random walk to sand
// (worldgen_find_initial_spawns), with a clock-seeded spawn RNG. Returns
// false (out untouched) only if chunk generation fails.
bool worldgen_find_spawn(int64_t seed, float out[3]);

// Like worldgen_find_spawn but for the Nether: probes a temporary Nether
// generator for a standable air gap (>=2 air on a solid netherrack/soulsand/
// gravel/bedrock floor) in the open band below the roof. Used only by the
// debug.start_nether boot flag; returns false if none found (caller falls back).
bool worldgen_find_nether_spawn(int64_t seed, float out[3]);

// Choose the permanent Overworld SpawnXYZ and the player's initial position
// independently. A debug Nether start must not replace SpawnXYZ: death in the
// Nether respawns in the Overworld and therefore needs a safe Overworld spawn.
// Superflat preserves its fixed y=5 start in either dimension.
// Overworld: Beta's World.java:215-227 search; SpawnXYZ y=64, first-join
// feet y=65. World.rand is clock-seeded (World.java:169,177).
void worldgen_find_initial_spawns(int64_t seed, bool superflat,
								  bool start_nether, float world_spawn[3],
								  float player_spawn[3]);

// REL-03b: did the most recent Overworld spawn search (worldgen_find_spawn,
// worldgen_find_initial_spawns, worldgen_find_initial_spawns_rng) find
// Beta's sand spawn? false = chunk generation failed mid-search; the outputs
// then hold placeholder defaults that must NOT be saved as a world (Beta only
// sets the spawn once the sand predicate succeeds, World.java:215-227).
// Superflat (no search) reports true. The void search API is unchanged so
// existing stubs keep linking; same-thread use only (the create path).
bool worldgen_last_spawn_search_ok(void);

// World.getInitialSpawnLocation (World.java:215-227) over a column provider:
// block_id(ctx, x, y, z) is World.getBlockId for 0 <= y < 128 (y >= 128 is
// handled by the search as air), or -1 when the chunk cannot be supplied
// (generation failure: the search then reports false, never a found spawn).
// Beta's provider is ChunkProviderServer.loadChunk: it generates the chunk
// AND populates every resident 2x2 quadrant it completes (current, west,
// north, northwest), and that population draws World.rand (BlockFluid lava
// mix fizz) on the very rng this search walks with -- so the live
// server_world is the faithful provider (server_local_beta_spawn_search);
// raw terrain is the standalone fallback. Draws: nextInt(64) - nextInt(64)
// for x, then for z, in that order, per failed candidate.
struct worldgen_spawn_world {
	void* ctx;
	int (*block_id)(void* ctx, int x, int y, int z);
};
bool worldgen_beta_spawn_search(const struct worldgen_spawn_world* world,
								struct java_random* rng, int* out_x,
								int* out_z);

// A session's live-world search, used by every Overworld spawn search above
// instead of the raw-terrain probe once installed (NULL = uninstall). Same
// contract as the probe: Beta's World.rand in, SpawnX/Z out, false on chunk
// generation failure.
typedef bool (*worldgen_spawn_search_fn)(int64_t seed, struct java_random* rng,
										 int* out_x, int* out_z);
void worldgen_set_spawn_search_override(worldgen_spawn_search_fn fn);

#ifdef CAVEWII_RAND_SEED_TEST
// Test pin for the clock-seeded search (worldgen_find_initial_spawns /
// worldgen_find_spawn): start World.rand from this raw 48-bit state.
void worldgen_test_pin_spawn_rng_state(bool pinned, uint64_t state48);
#endif

// The same, drawing the search in place from *spawn_rng (Beta's World.rand).
#define WORLDGEN_HAS_SPAWN_RNG_SEAM 1
void worldgen_find_initial_spawns_rng(int64_t seed, bool superflat,
	bool start_nether, struct java_random* spawn_rng, float world_spawn[3],
	float player_spawn[3]);

// Compute Beta's per-column grass/foliage colormap indices for a chunk so the
// client can tint grass/leaves. For each column ci = (1-temperature)*255 and
// cj = (1-humidity*temperature)*255 (Beta ColorizerGrass index math); the same
// pair indexes both colormaps. Output arrays are 256 bytes each, laid out
// [localX*16 + localZ]. Works for any overworld chunk (generated or loaded),
// recomputing the biome temperature/humidity from the world seed.
void worldgen_climate(struct worldgen* wg, w_coord_t chunk_x, w_coord_t chunk_z,
					  uint8_t* grass_i, uint8_t* grass_j);

// Beta WorldChunkManager.getBiomeGenAt(x, z): the biome at one world column,
// recomputed from the seed (a 1x1 climate sample). Used by the populate pass to
// choose tree counts and types at the chunk's +16,+16 corner.
enum biome worldgen_biome_at(struct worldgen* wg, int world_x, int world_z);

// Beta treeDensityNoiseGen 2D octave sample at (block_x*0.5, block_z*0.5); feeds
// the per-chunk tree count. block_x/block_z are the chunk's block origin.
double worldgen_tree_density(struct worldgen* wg, int block_x, int block_z);

// Beta GenerateTemperature: the 16x16 temperature field (temperature-only, no
// humidity) at world block (bx, bz), used by the snow pass. `out` holds 256
// doubles [localX*16 + localZ], each in [0,1].
void worldgen_temperature_grid(struct worldgen* wg, int bx, int bz, double* out);

struct wgen_world;
// Beta ChunkProviderHell.populate: the Nether decorate pass (lava springs, fire,
// glowstone clusters, brown/red mushrooms) over the chunk's 2x2 quadrant via `w`
// (implemented in worldgen_hell.c). Beta does NOT reseed for the Nether populate
// (unlike the overworld) -- it draws from one shared, world-global hellRNG whose
// state depends on generation order -- so this takes the RNG explicitly and the
// caller (server_world) owns the shared stream used by constructor noise,
// terrain reseeding/surface and population. Per (seed,chunk)
// determinism is therefore NOT guaranteed (a faithful Beta property); block
// placement, draw order, and counts ARE byte-exact given the RNG state, with one
// documented decoration-layer deferral (lava updateTick draw) -- see
// worldgen_hell.c. The mushroom light gate (FH-16) is implemented; see
// worldgen_hell_mushroom_light_gate_blocks below.
void worldgen_hell_populate(struct java_random* rng, int chunk_x, int chunk_z,
							struct wgen_world* w);

// [740c583 BlockMushroom.canBlockStay]'s `getFullBlockLightValue(x,y,z) < 13`
// half of the Nether mushroom placement gate (the ground/opacity half is
// hell_is_opaque_cube, internal to worldgen_hell.c) -- exposed standalone so
// the light math (source/worldgen/worldgen_hell.c's hell_lit_13_or_more) has a
// direct, RNG-independent unit test rather than relying on an RNG draw
// happening to land a mushroom candidate next to a light source. Returns true
// when the gate REJECTS placement (light >= 13), i.e. the caller wants
// `!worldgen_hell_mushroom_light_gate_blocks(...)` to allow it.
bool worldgen_hell_mushroom_light_gate_blocks(struct wgen_world* w, int x,
											  int y, int z);

#endif
