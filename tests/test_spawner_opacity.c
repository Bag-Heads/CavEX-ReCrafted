/* Copyright (c) 2026 CaveWii_v2 contributors
 * Spawner light-opacity parity (lane C, 2026-09-27).
 *
 * Beta 1.7.3 (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - Block.<init>, Block.java:151-152: `opaqueCubeLookup[id] =
 *     isOpaqueCube(); lightOpacity[id] = isOpaqueCube() ? 255 : 0;`
 *   - BlockMobSpawner.isOpaqueCube, BlockMobSpawner.java:22-24: `return
 *     false;` and the registration Block.java:75 `mobSpawner = (new
 *     BlockMobSpawner(52, 65)).setHardness(5.0F)...` never calls
 *     setLightOpacity -> lightOpacity[52] == 0 (like glass).
 *   - Chunk.generateSkylightMap / func_1003_g (heightMap stops only at
 *     lightOpacity != 0) and the skylight column subtract lightOpacity per
 *     cell: a spawner neither stops the heightmap nor dims skylight
 *     passing down through it.
 * In this engine opacity 0 + can_see_through is exactly that (the glass /
 * cactus encoding); the spawner was registered with opacity 1.
 *
 * Checks, each against worldgen_lighting (the production heightmap + column
 * skylight writer the relight passes and the runtime flood agree with):
 *   1. registry: the spawner reads Beta's isOpaqueCube()==false /
 *      lightOpacity 0 encoding (can_see_through, opacity 0);
 *   2. a spawner floating above grass under open sky does not raise the
 *      heightmap and passes full skylight to the cells below it;
 *   3. a spawner standing on the grass: heightmap stays grass+1 and the
 *      spawner cell itself holds skylight 15. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/server_world.h"
#include "worldgen/worldgen.h"

#define N 16
#define H 128
#define IDX(x, y, z) ((size_t)(y) + ((size_t)(z) + (size_t)(x) * N) * H)
#define HM(x, z) ((x) + (z) * N)

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

int main(void) {
	blocks_init();
	check(blocks[BLOCK_MOB_SPAWNER] && blocks[BLOCK_MOB_SPAWNER]->can_see_through
		&& blocks[BLOCK_MOB_SPAWNER]->opacity == 0,
		"registry: spawner = isOpaqueCube false, lightOpacity 0 "
		"(Block.java:151-152 + BlockMobSpawner.java:22-24)");

	struct server_chunk sc;
	memset(&sc, 0, sizeof(sc));
	size_t cells = (size_t)N * N * H;
	sc.ids = calloc(cells, 1);
	sc.metadata = calloc(cells / 2, 1);
	sc.lighting_sky = calloc(cells / 2, 1);
	sc.lighting_torch = calloc(cells / 2, 1);
	sc.heightmap = calloc(N * N, 1);
	assert(sc.ids && sc.metadata && sc.lighting_sky && sc.lighting_torch
		   && sc.heightmap);
	for(int x = 0; x < N; x++)
		for(int z = 0; z < N; z++) {
			sc.ids[IDX(x, 0, z)] = BLOCK_BEDROCK;
			for(int y = 1; y <= 63; y++)
				sc.ids[IDX(x, y, z)] = BLOCK_STONE;
			sc.ids[IDX(x, 64, z)] = BLOCK_GRASS;
		}
	sc.ids[IDX(3, 70, 3)] = BLOCK_MOB_SPAWNER; /* floating */
	sc.ids[IDX(9, 65, 9)] = BLOCK_MOB_SPAWNER; /* on the grass */
	worldgen_lighting(&sc, true, WORLD_DIM_OVERWORLD);

	check(sc.heightmap[HM(3, 3)] == 65,
		"floating spawner: the heightmap stays at grass+1 (lightOpacity 0 "
		"never stops Chunk.heightMap)");
	check(nibble_read(sc.lighting_sky, IDX(3, 70, 3)) == 15
		&& nibble_read(sc.lighting_sky, IDX(3, 69, 3)) == 15
		&& nibble_read(sc.lighting_sky, IDX(3, 65, 3)) == 15,
		"floating spawner: skylight 15 in its cell and all the way down to "
		"the grass (nothing subtracted)");
	check(sc.heightmap[HM(9, 9)] == 65,
		"spawner on grass: heightmap grass+1, not spawner+1");
	check(nibble_read(sc.lighting_sky, IDX(9, 65, 9)) == 15,
		"spawner on grass: its own cell holds skylight 15");

	free(sc.ids); free(sc.metadata); free(sc.lighting_sky);
	free(sc.lighting_torch); free(sc.heightmap);
	if(failures) {
		printf("test_spawner_opacity: %u FAILED\n", failures);
		return 1;
	}
	puts("test_spawner_opacity: PASS");
	return 0;
}
