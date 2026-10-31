/*
	Tests source/worldgen/populate.c -- worldgen_dungeon, the Beta 1.7.3
	WorldGenDungeons carver.

	Checks:
	  - abort path byte-exactness: in solid rock (no wall openings) the dungeon
	    draws EXACTLY its two width values (nextInt(2)+2 each) then aborts,
	    writing nothing,
	  - build path: in a chamber with a solid floor/ceiling and one wall opening
	    it generates -- a cobblestone shell + air interior + a centre mob
	    spawner (id 52),
	  - deterministic (same seed + terrain -> identical buffer + RNG state).

	The widths are drawn first, so the build-path terrain is shaped AFTER peeking
	them from an identical RNG (the dungeon then re-draws the same values).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/java_random.h"
#include "worldgen/populate.h"

#define AIR 0
#define STONE 1
#define COBBLE 4
#define MOSSY 48
#define CHEST 54
#define SPAWNER 52

#define FW_W 48
struct flatworld {
	uint8_t ids[FW_W * FW_W * 128];
	uint8_t meta[FW_W * FW_W * 128];
};

static int fw_index(int x, int y, int z, int* idx) {
	if(y < 0 || y >= 128 || x < 0 || x >= FW_W || z < 0 || z >= FW_W)
		return 0;
	*idx = (x * FW_W + z) * 128 + y;
	return 1;
}
static uint8_t fw_get(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(x, y, z, &idx) ? f->ids[idx] : AIR;
}
static void fw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct flatworld* f = ctx;
	int idx;
	if(fw_index(x, y, z, &idx)) {
		f->ids[idx] = id;
		f->meta[idx] = meta;
	}
}
static void put(struct flatworld* f, int x, int y, int z, uint8_t id) {
	int idx;
	if(fw_index(x, y, z, &idx))
		f->ids[idx] = id;
}

int main(void) {
	const int PX = 24, PY = 64, PZ = 24;
	const int64_t SEED = 0xD0Eull;

	// --- Abort path: solid rock -> no wall openings -> abort after 2 draws.
	struct flatworld* fs = malloc(sizeof *fs);
	struct flatworld* fs0 = malloc(sizeof *fs0);
	memset(fs->ids, STONE, sizeof(fs->ids));
	memset(fs->meta, 0, sizeof(fs->meta));
	memcpy(fs0->ids, fs->ids, sizeof(fs->ids));
	struct wgen_world ws = {fs, fw_get, fw_set, NULL, NULL};
	struct java_random rs, rs_exp;
	jrand_set_seed(&rs, SEED);
	jrand_set_seed(&rs_exp, SEED);
	bool oks = worldgen_dungeon(&ws, &rs, PX, PY, PZ);
	assert(!oks);
	jrand_next_int_bound(&rs_exp, 2); // width X
	jrand_next_int_bound(&rs_exp, 2); // width Z
	assert(rs.seed == rs_exp.seed);							  // exactly 2 draws
	assert(memcmp(fs->ids, fs0->ids, sizeof(fs->ids)) == 0); // nothing written

	// --- Build path: peek the widths from an identical RNG, then carve a
	// chamber (solid everywhere with one wall opening at -X).
	struct java_random rpeek;
	jrand_set_seed(&rpeek, SEED);
	int wx = jrand_next_int_bound(&rpeek, 2) + 2;
	int wz = jrand_next_int_bound(&rpeek, 2) + 2;

	struct flatworld* fa = malloc(sizeof *fa);
	struct flatworld* fb = malloc(sizeof *fb);
	memset(fa->ids, STONE, sizeof(fa->ids));
	memset(fa->meta, 0, sizeof(fa->meta));
	// one opening: perimeter column at (PX-wx-1, PY/PY+1, PZ) is air-over-air.
	put(fa, PX - wx - 1, PY, PZ, AIR);
	put(fa, PX - wx - 1, PY + 1, PZ, AIR);
	memcpy(fb->ids, fa->ids, sizeof(fa->ids));
	memset(fb->meta, 0, sizeof(fb->meta));

	struct wgen_world wa = {fa, fw_get, fw_set, NULL, NULL};
	struct wgen_world wb = {fb, fw_get, fw_set, NULL, NULL};
	struct java_random ra, rb;
	jrand_set_seed(&ra, SEED);
	jrand_set_seed(&rb, SEED);
	bool oka = worldgen_dungeon(&wa, &ra, PX, PY, PZ);
	bool okb = worldgen_dungeon(&wb, &rb, PX, PY, PZ);
	assert(oka && okb);								   // generated
	assert(ra.seed == rb.seed);						   // identical randomness
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0); // determinism

	// Structure: a mob spawner sits at the centre; the room is wrapped in
	// cobblestone (plain or mossy) and hollowed to air inside.
	assert(fw_get(fa, PX, PY, PZ) == SPAWNER);
	long cobble = 0, air = 0, chest = 0;
	for(int x = PX - wx - 1; x <= PX + wx + 1; x++)
		for(int y = PY - 1; y <= PY + 4; y++)
			for(int z = PZ - wz - 1; z <= PZ + wz + 1; z++) {
				uint8_t b = fw_get(fa, x, y, z);
				if(b == COBBLE || b == MOSSY)
					cobble++;
				else if(b == AIR)
					air++;
				else if(b == CHEST)
					chest++;
			}
	assert(cobble > 0); // a stone shell was laid
	assert(air > 0);	// the interior was hollowed

	free(fs);
	free(fs0);
	free(fa);
	free(fb);
	printf("test_dungeon: all assertions passed (abort byte-exact + build "
		   "structure + determinism; %ld cobble, %ld air, %ld chest, "
		   "spawner@centre)\n",
		   cobble, air, chest);
	return 0;
}
