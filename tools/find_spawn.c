/*
	Offline spawn finder: drives the byte-exact Beta worldgen to locate a
	dramatic ON-LAND spawn (the highest land surface in a scanned area -- a
	mountain peak), so a region-less world spawns the player on a demanding
	vista instead of in the ocean. Because the generator is byte-exact, the Wii
	regenerates the identical terrain from the same seed, so the coordinates
	computed here on PC are valid there too.

	Usage: find_spawn <seed> [radius_chunks]
	Prints: seed, the highest land column (x,y,z), the spawn Y (surface+1), and
	how much of the scan was land (to gauge "is this a watery seed").
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "block/blocks_data.h"
#include "network/server_world.h" // struct server_chunk
#include "worldgen/worldgen.h"

#define IDX(x, y, z) ((size_t)(y) + ((size_t)(z) + (size_t)(x)*16) * 128)

// Stubbed externals worldgen.c links against. The block table is only read by
// the sky-light opacity pass (irrelevant to finding the surface); the nibble
// helpers just pack metadata/light. Same stubs as tests/test_worldgen.c.
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

static int is_land(uint8_t b) {
	return b == BLOCK_STONE || b == BLOCK_GRASS || b == BLOCK_DIRT
		|| b == BLOCK_SAND || b == BLOCK_GRAVEL;
}

int main(int argc, char** argv) {
	int64_t seed = (argc > 1) ? strtoll(argv[1], NULL, 10) : 1;
	int R = (argc > 2) ? atoi(argv[2]) : 16;

	struct worldgen* wg = worldgen_create(seed, WORLD_DIM_OVERWORLD);
	if(!wg) {
		printf("worldgen_create failed\n");
		return 1;
	}

	struct server_chunk sc;
	int best_y = -1, best_x = 0, best_z = 0;
	long land = 0, total = 0;

	for(int cx = -R; cx <= R; cx++) {
		for(int cz = -R; cz <= R; cz++) {
			if(!worldgen_generate_chunk(wg, &sc, cx, cz))
				continue;
			for(int lx = 0; lx < 16; lx++) {
				for(int lz = 0; lz < 16; lz++) {
					total++;
					for(int y = 127; y >= 0; y--) {
						uint8_t b = sc.ids[IDX(lx, y, lz)];
						if(b != BLOCK_AIR) {
							if(is_land(b)) {
								land++;
								if(y > best_y) {
									best_y = y;
									best_x = cx * 16 + lx;
									best_z = cz * 16 + lz;
								}
							}
							break;
						}
					}
				}
			}
			free(sc.ids);
			free(sc.metadata);
			free(sc.lighting_sky);
			free(sc.lighting_torch);
			free(sc.heightmap);
		}
	}

	printf("seed=%lld R=%d highest_land=(%d,%d,%d) spawnY=%d land=%ld/%ld (%.0f%%)\n",
		   (long long)seed, R, best_x, best_y, best_z, best_y + 1, land, total,
		   total ? 100.0 * land / total : 0.0);

	worldgen_destroy(wg);
	return 0;
}
