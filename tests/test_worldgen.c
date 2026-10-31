/*
	Tests source/worldgen/worldgen.c -- the Beta 1.7.3 overworld terrain
	generator (density field -> block fill -> surface replacement).

	The byte-exact noise foundations are proven in test_noise / test_simplex /
	test_java_random; this test exercises the full per-chunk pipeline for its
	structural invariants and determinism:
	  - every emitted block id is one of the small terrain set,
	  - y=0 is bedrock and the build ceiling is air,
	  - every column has solid ground (land or seabed),
	  - water never appears above sea level,
	  - regenerating a chunk is bit-identical; different chunk / different seed
	    differ; non-overworld dimensions are declined.

	A gold-standard chunk diff against real Beta for a known seed is the
	remaining external check and is tracked separately (it needs a Beta
	instance, which isn't available in this harness).

	worldgen.c references the global block table and nibble helpers; the terrain
	block ids do not depend on the block table (only the optional sky-light pass
	consults it), so a null table is sufficient here and lets the test stand
	alone without linking the whole block system.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network/server_world.h"
#include "worldgen/worldgen.h"

// --- stubbed external dependencies ---------------------------------------
struct block;
struct block* blocks[256] = {0};

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
// -------------------------------------------------------------------------

#define IDX(x, y, z)                                                           \
	((size_t)(y) + ((size_t)(z) + (size_t)(x) * CHUNK_SIZE) * WORLD_HEIGHT)

#define BLOCK_AIR 0
#define BLOCK_STONE 1
#define BLOCK_GRASS 2
#define BLOCK_DIRT 3
#define BLOCK_BEDROCK 7
#define BLOCK_WATER_STILL 9
#define BLOCK_SAND 12
#define BLOCK_GRAVEL 13
#define BLOCK_SANDSTONE 24
#define BLOCK_ICE 79

static const size_t CHUNK_BYTES
	= (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;

static bool valid_terrain_id(uint8_t id) {
	switch(id) {
		case BLOCK_AIR:
		case BLOCK_STONE:
		case BLOCK_GRASS:
		case BLOCK_DIRT:
		case BLOCK_BEDROCK:
		case BLOCK_WATER_STILL:
		case BLOCK_SAND:
		case BLOCK_GRAVEL:
		case BLOCK_SANDSTONE:
		case BLOCK_ICE: return true;
		default: return false;
	}
}

static void free_chunk(struct server_chunk* sc) {
	/* worldgen_alloc_chunk hands out ONE MEM2_POOL_COLUMN slot (2026-09-11). */
	server_chunk_arrays_free(sc);
}

// FNV-1a/64 over a byte range -- a stable digest for the golden regression guard.
static uint64_t fnv1a(const uint8_t* data, size_t n) {
	uint64_t h = 1469598103934665603ULL;
	for(size_t i = 0; i < n; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static void test_initial_spawns(int64_t seed) {
	float expected_world[3] = {0.5F, 64.0F, 0.5F};
	float expected_nether[3] = {0.5F, 65.0F, 0.5F};
	assert(worldgen_find_spawn(seed, expected_world));
	assert(worldgen_find_nether_spawn(seed, expected_nether));

	float world_spawn[3], player_spawn[3];
	worldgen_find_initial_spawns(seed, false, true, world_spawn, player_spawn);
	assert(memcmp(world_spawn, expected_world, sizeof(world_spawn)) == 0);
	assert(memcmp(player_spawn, expected_nether, sizeof(player_spawn)) == 0);
	assert(memcmp(world_spawn, player_spawn, sizeof(world_spawn)) != 0);

	// Superflat keeps the established fixed spawn, even for a debug Nether start.
	worldgen_find_initial_spawns(seed, true, true, world_spawn, player_spawn);
	const float flat_spawn[3] = {0.5F, 5.0F, 0.5F};
	assert(memcmp(world_spawn, flat_spawn, sizeof(world_spawn)) == 0);
	assert(memcmp(player_spawn, flat_spawn, sizeof(player_spawn)) == 0);
}

// Harness fixed-seed lane (2026-09-04, hw_run --cw-seed): worldgen_find_
// initial_spawns is the EXACT helper screen_select_world.c's sworld_finish_
// create calls to resolve the "+new" autoload sentinel's spawn (superflat=
// false, start_nether=false is its real-overworld case -- superflat/nether
// short-circuit to fixed constants and are already covered by
// test_initial_spawns above). It creates a fresh struct worldgen each call
// with no static state, so the same seed must produce a bit-identical
// initial spawn every time -- proof that a --cw-seed harness run reproduces
// the same spawn chunk across runs, cheaply, without touching the hw_run/
// screen_select_world plumbing itself.
static void test_same_seed_reproducible_initial_spawn(int64_t seed) {
	float world_a[3], player_a[3];
	float world_b[3], player_b[3];
	worldgen_find_initial_spawns(seed, false, false, world_a, player_a);
	worldgen_find_initial_spawns(seed, false, false, world_b, player_b);
	assert(memcmp(world_a, world_b, sizeof(world_a)) == 0);
	assert(memcmp(player_a, player_b, sizeof(player_a)) == 0);

	// A different seed is not REQUIRED to differ (Beta's spawn search can
	// coincidentally land on the same column for two seeds), but for this
	// seed pair it does -- a cheap sanity check that the test isn't
	// vacuously passing because both calls return some hardcoded constant.
	float world_c[3], player_c[3];
	worldgen_find_initial_spawns(seed + 1, false, false, world_c, player_c);
	assert(memcmp(world_a, world_c, sizeof(world_a)) != 0
		   || memcmp(player_a, player_c, sizeof(player_a)) != 0);
}

int main(void) {
	const int64_t SEED = 2024;
	test_initial_spawns(SEED);
	test_same_seed_reproducible_initial_spawn(SEED);

	struct worldgen* wg = worldgen_create(SEED, WORLD_DIM_OVERWORLD);
	assert(wg);

	struct server_chunk c;
	assert(worldgen_generate_chunk(wg, &c, 0, 0));

	int n_air = 0, n_solid = 0, n_water = 0;
	for(int x = 0; x < CHUNK_SIZE; x++) {
		for(int z = 0; z < CHUNK_SIZE; z++) {
			// y=0 is always bedrock; the ceiling is always air.
			assert(c.ids[IDX(x, 0, z)] == BLOCK_BEDROCK);
			assert(c.ids[IDX(x, WORLD_HEIGHT - 1, z)] == BLOCK_AIR);

			int top = -1; // highest non-air, non-water block
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				uint8_t id = c.ids[IDX(x, y, z)];
				assert(valid_terrain_id(id));
				if(id == BLOCK_AIR) {
					n_air++;
				} else if(id == BLOCK_WATER_STILL) {
					n_water++;
					assert(y < 64); // water never above sea level
				} else {
					n_solid++;
					if(y > top)
						top = y;
				}
			}
			assert(top >= 0); // every column has ground (at least bedrock)
		}
	}
	assert(n_air > 0 && n_solid > 0);

	// Determinism: regenerating the same chunk is bit-identical.
	struct server_chunk c2;
	assert(worldgen_generate_chunk(wg, &c2, 0, 0));
	assert(memcmp(c.ids, c2.ids, CHUNK_BYTES) == 0);

	// A different chunk coordinate differs.
	struct server_chunk c3;
	assert(worldgen_generate_chunk(wg, &c3, 5, -3));
	assert(memcmp(c.ids, c3.ids, CHUNK_BYTES) != 0);

	// A different seed differs for the same chunk.
	struct worldgen* wg2 = worldgen_create(SEED + 1, WORLD_DIM_OVERWORLD);
	assert(wg2);
	struct server_chunk c4;
	assert(worldgen_generate_chunk(wg2, &c4, 0, 0));
	assert(memcmp(c.ids, c4.ids, CHUNK_BYTES) != 0);

	// The Nether is now generated by ChunkProviderHell (worldgen_hell.c). A
	// nether chunk must succeed, be netherrack-dominant, carry NO overworld
	// surface grass, and have an always-bedrock roof (y=127) and floor (y=0)
	// (Beta func_4058_b: 127 >= 127-nextInt(5) and 0 <= nextInt(5) both always
	// hold). Deeper byte-exact coverage lives in test_worldgen_hell.
	struct worldgen* wgn = worldgen_create(SEED, WORLD_DIM_NETHER);
	assert(wgn);
	struct server_chunk cn;
	assert(worldgen_generate_chunk(wgn, &cn, 0, 0));
	{
		bool has_netherrack = false, has_bedrock = false, has_grass = false;
		for(size_t i = 0; i < CHUNK_BYTES; i++) {
			if(cn.ids[i] == 87) // BLOCK_NETHERRACK
				has_netherrack = true;
			else if(cn.ids[i] == 7) // BLOCK_BEDROCK
				has_bedrock = true;
			else if(cn.ids[i] == 2) // BLOCK_GRASS (overworld-only)
				has_grass = true;
		}
		assert(has_netherrack);
		assert(has_bedrock);
		assert(!has_grass);
		for(int x = 0; x < 16; x++)
			for(int z = 0; z < 16; z++) {
				size_t col = ((size_t)z + (size_t)x * 16) * 128;
				assert(cn.ids[col + 0] == 7);	// floor bedrock
				assert(cn.ids[col + 127] == 7); // roof bedrock
			}
	}

	// --- Golden regression guard (SELF-golden change-detector, NOT a Beta-
	// 1.7.3 parity proof) ---
	// A committed digest of the FULL generated terrain (caves included; all
	// deterministic) for fixed seed/chunks. It catches ANY unintended change to
	// CaveWii's OWN worldgen output -- e.g. it would have flagged the
	// -ffp-contract=off FP change that the structural asserts above do not -- and
	// it is the diff scaffold for the real Beta golden diff.
	// The real Beta-parity diff is BLOCKED on a reference oracle in this repo:
	// the bundled BetrockServer (_betrock_ref/betrock_ref) is source-only (no
	// build system), its GenerateChunk is a non-standalone Chunk/World method
	// with ~5 missing headers, and its biome classifier is float where CaveWii
	// uses double to match vanilla; no real Beta chunk dump is available here.
	// See docs/worldgen_design.md "Golden diff". When a Beta reference exists,
	// swap these self-digests for the Beta blob's (raw id diff, x<<11|z<<7|y).
	uint64_t g00 = fnv1a(c.ids, CHUNK_BYTES);	// seed 2024, chunk (0,0)
	uint64_t g5m3 = fnv1a(c3.ids, CHUNK_BYTES); // seed 2024, chunk (5,-3)
	printf("test_worldgen: golden digests (0,0)=0x%016llx (5,-3)=0x%016llx\n",
		   (unsigned long long)g00, (unsigned long long)g5m3);
	fflush(stdout);
	assert(g00 == 0x9979a3b2ff34e3b2ULL);  // seed 2024 (0,0) terrain digest
	assert(g5m3 == 0x95c16fd7db7aebddULL); // seed 2024 (5,-3) terrain digest

	free_chunk(&c);
	free_chunk(&c2);
	free_chunk(&c3);
	free_chunk(&c4);
	free_chunk(&cn);
	worldgen_destroy(wg);
	worldgen_destroy(wg2);
	worldgen_destroy(wgn);

	printf("test_worldgen: all assertions passed (terrain determinism + "
		   "structure)\n");
	return 0;
}
