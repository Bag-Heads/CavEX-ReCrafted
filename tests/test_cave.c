/*
	Tests source/worldgen/cave.c -- the Beta 1.7.3 cave carver.

	The byte-exact PRNG + Beta sin/cos table it relies on are proven elsewhere
	(test_java_random); this checks the carver's structural contract:
	  - deterministic (same seed + chunk -> identical carving),
	  - bedrock-safe (y=0 never carved; carving clamped to y in [1,120]),
	  - it only ever turns stone/dirt/grass into air or lava (never corrupts
	    other ids), and air above the terrain is left untouched,
	  - it actually carves (caves are sparse per chunk, so accumulate over a
	    region to prove tunnels appear).
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/cave.h"

#define CH_BYTES (16 * 16 * 128)
#define IDX(x, y, z) (((x) * 16 + (z)) * 128 + (y))

// Simple terrain: bedrock(7) at y=0, stone(1) y=1..70, air(0) above.
static void fill_terrain(uint8_t* a) {
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y < 128; y++)
				a[IDX(x, y, z)] = (y == 0) ? 7 : (y <= 70 ? 1 : 0);
}

int main(void) {
	uint8_t* a = malloc(CH_BYTES);
	uint8_t* b = malloc(CH_BYTES);

	// Determinism: same seed + chunk -> identical result.
	fill_terrain(a);
	cave_generate(1337, 3, -5, a);
	fill_terrain(b);
	cave_generate(1337, 3, -5, b);
	assert(memcmp(a, b, CH_BYTES) == 0);

	// Sweep a region (caves are sparse per chunk) and check invariants.
	long carved_air = 0, carved_lava = 0;
	int chunks_with_caves = 0;
	for(int cx = 0; cx < 24; cx++) {
		for(int cz = 0; cz < 24; cz++) {
			fill_terrain(a);
			cave_generate(2024, cx, cz, a);
			int local = 0;
			for(int x = 0; x < 16; x++) {
				for(int z = 0; z < 16; z++) {
					assert(a[IDX(x, 0, z)] == 7); // bedrock survives
					for(int y = 1; y <= 70; y++) {
						uint8_t t = a[IDX(x, y, z)];
						// former stone is now stone, air, lava, or grass
						// (grass only via the dirt-restore, absent here)
						assert(t == 1 || t == 0 || t == 10 || t == 2);
						if(t == 0)
							local++, carved_air++;
						if(t == 10)
							carved_lava++;
					}
					for(int y = 71; y < 128; y++)
						assert(a[IDX(x, y, z)] == 0); // air above untouched
				}
			}
			if(local)
				chunks_with_caves++;
		}
	}
	assert(carved_air > 0);
	assert(chunks_with_caves > 0);

	free(a);
	free(b);
	printf("test_cave: all assertions passed (carve determinism + bedrock-safe;"
		   " %ld air + %ld lava carved across %d caved chunks)\n",
		   carved_air, carved_lava, chunks_with_caves);
	return 0;
}
