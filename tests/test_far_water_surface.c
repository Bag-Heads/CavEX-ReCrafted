/*
	test_far_water_surface -- pins the distant-water-surface fix. The far-ring extract
	(server_world_extract_far_column) must report the WATER SURFACE (top water block y +
	BLOCK_WATER_STILL) for ocean/lake columns, NOT the seabed beneath the see-through
	water -- so distant oceans render Beta's flat blue plane instead of a biome-tinted
	seabed. Land columns stay the top non-see-through block.

	Standalone (server_world.c can't link in isolation): replicates the production
	per-column predicate EXACTLY. Keep in lockstep with server_world_extract_far_column
	and test_far_skin_equiv's mirror.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"

#define COL_H 128

static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_water = {.can_see_through = true, .opacity = 3};
struct block* blocks[256] = {0};

// Mirror of server_world_extract_far_column's per-column scan (server_world.c).
static void extract_col(const uint8_t* ids, int* skin_y, uint8_t* top_id) {
	int skin = -1, top_water = -1;
	for(int y = COL_H - 1; y >= 0; y--) {
		uint8_t id = ids[y];
		if(top_water < 0 && (id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW))
			top_water = y;
		if(blocks[id] && !blocks[id]->can_see_through) {
			skin = y;
			break;
		}
	}
	if(top_water >= 0 && top_water > skin) {
		*skin_y = top_water;
		*top_id = BLOCK_WATER_STILL;
	} else if(skin >= 0) {
		*skin_y = skin;
		*top_id = ids[skin];
	} else {
		*skin_y = 0xFF;
		*top_id = 0;
	}
}

int main(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_SAND] = blocks[BLOCK_GRASS]
		= blocks[BLOCK_DIRT] = &b_solid;
	blocks[BLOCK_WATER_STILL] = blocks[BLOCK_WATER_FLOW] = &b_water;

	uint8_t col[COL_H];
	int sy;
	uint8_t tid;

	// (1) Open ocean: sand seabed to y=50, still water y=51..63, air above.
	memset(col, BLOCK_AIR, COL_H);
	for(int y = 0; y <= 50; y++)
		col[y] = BLOCK_SAND;
	for(int y = 51; y <= 63; y++)
		col[y] = BLOCK_WATER_STILL;
	extract_col(col, &sy, &tid);
	assert(sy == 63 && tid == BLOCK_WATER_STILL); // surface, NOT the sand seabed

	// (2) Flowing water normalizes to still (the far plane is static).
	memset(col, BLOCK_AIR, COL_H);
	for(int y = 0; y <= 50; y++)
		col[y] = BLOCK_SAND;
	for(int y = 51; y <= 63; y++)
		col[y] = BLOCK_WATER_FLOW;
	extract_col(col, &sy, &tid);
	assert(sy == 63 && tid == BLOCK_WATER_STILL);

	// (3) Dry land: grass surface at y=70 -> the surface block, no water.
	memset(col, BLOCK_AIR, COL_H);
	for(int y = 0; y <= 70; y++)
		col[y] = (y == 70) ? BLOCK_GRASS : BLOCK_STONE;
	extract_col(col, &sy, &tid);
	assert(sy == 70 && tid == BLOCK_GRASS);

	// (4) Deep water with no captured seabed (water to y=0) -> still water surface.
	memset(col, BLOCK_AIR, COL_H);
	for(int y = 0; y <= 63; y++)
		col[y] = BLOCK_WATER_STILL;
	extract_col(col, &sy, &tid);
	assert(sy == 63 && tid == BLOCK_WATER_STILL);

	// (5) Shallow lake: one water block at y=64 over solid at y=63 (the same-level
	//     land->water case carve-out (e) in test_far_skin_equiv).
	memset(col, BLOCK_AIR, COL_H);
	for(int y = 0; y <= 63; y++)
		col[y] = BLOCK_STONE;
	col[64] = BLOCK_WATER_STILL;
	extract_col(col, &sy, &tid);
	assert(sy == 64 && tid == BLOCK_WATER_STILL);

	// (6) All air -> no surface.
	memset(col, BLOCK_AIR, COL_H);
	extract_col(col, &sy, &tid);
	assert(sy == 0xFF);

	printf("test_far_water_surface: PASS\n");
	return 0;
}
