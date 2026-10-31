/*
	find_ocean_seed -- a one-off worldgen tool. Scans seeds for an OCEAN-SHORE world:
	the spawn chunk is land (so the player stands), with a large ocean within the far
	ring (chunks 2..8) in some cardinal direction. Prints the seed + the ocean
	direction/distance so a deterministic far-water capture can pin it via
	debug.world_seed (screen_select_world.c) and aim toward the ocean.

	Why: the far ring regenerates from worldgen, so a placed demo can't fake distant
	ocean -- only natural ocean proves the far-water-surface fix, and +new spawns are
	random (usually landlocked). Links the worldgen exactly like test_far_skin_equiv.

	Usage: find_ocean_seed [count] [max_seed]   (defaults: 6 seeds, scan 1..4000)
*/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/server_world.h" // struct server_chunk
#include "worldgen/worldgen.h"

#define WGH 128
#define SEA 63
#define WG_IDX(x, y, z) ((size_t)(y) + ((size_t)(z) + (size_t)(x)*16) * WGH)

// Minimal real block table (worldgen surface/cave logic reads can_see_through).
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_leaves = {.can_see_through = true, .opacity = 1};
static struct block b_water = {.can_see_through = true, .opacity = 3};
static struct block b_lava = {.can_see_through = false, .opacity = 15, .luminance = 15};
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

// Columns (0..256) in chunk (cx,cz) that are water at sea level -> ocean/lake.
static int chunk_water(struct worldgen* wg, int cx, int cz) {
	struct server_chunk sc = {0};
	if(!worldgen_generate_chunk(wg, &sc, cx, cz))
		return -1;
	int w = 0;
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			uint8_t id = sc.ids[WG_IDX(x, SEA, z)];
			if(id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW)
				w++;
		}
	free(sc.ids);
	free(sc.metadata);
	free(sc.lighting_sky);
	free(sc.lighting_torch);
	free(sc.heightmap);
	return w;
}

int main(int argc, char** argv) {
	setup_blocks();
	int want = argc > 1 ? atoi(argv[1]) : 6;
	int64_t maxseed = argc > 2 ? atoll(argv[2]) : 4000;
	int found = 0;

	int dx[4] = {1, -1, 0, 0}, dz[4] = {0, 0, 1, -1};
	const char* dn[4] = {"+X", "-X", "+Z", "-Z"};
	// yaw to FACE that direction (yaw 0 looks +Z; +X renders screen-left).
	const int yaw[4] = {270, 90, 0, 180};

	for(int64_t seed = 1; seed <= maxseed && found < want; seed++) {
		// The game's worldgen_find_spawn lands the player on the first LAND column
		// near the origin, so chunk (0,0) is a good spawn proxy -- we look for ocean
		// within the far ring of it. (Calling find_spawn directly here tripped an
		// opaque linker crash in the tool build; the origin proxy is sufficient.)
		int scx = 0, scz = 0;
		struct worldgen* wg = worldgen_create(seed, WORLD_DIM_OVERWORLD);
		if(!wg)
			continue;
		int spawn_w = chunk_water(wg, scx, scz);
		int best = 0, bestk = 0, bestd = 0;
		for(int d = 0; d < 4; d++)
			for(int k = 2; k <= 8; k++) {
				int w = chunk_water(wg, scx + dx[d] * k, scz + dz[d] * k);
				if(w > best) {
					best = w;
					bestk = k;
					bestd = d;
				}
			}
		worldgen_destroy(wg);
		// Spawn mostly land + a far chunk mostly ocean -> a clean shore vista.
		if(spawn_w >= 0 && spawn_w < 60 && best > 170) {
			printf("SEED %lld  origin land(water %d/256)  OCEAN %s @%d chunks "
				   "(water %d/256)  -> debug.world_seed=%lld autopilot_yaw_deg=%d\n",
				   (long long)seed, spawn_w, dn[bestd], bestk, best,
				   (long long)seed, yaw[bestd]);
			found++;
		}
	}
	if(!found)
		printf("no ocean-shore seed found in 1..%lld\n", (long long)maxseed);
	return 0;
}
