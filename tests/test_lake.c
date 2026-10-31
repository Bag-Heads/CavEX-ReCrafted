/*
	Tests source/worldgen/populate.c -- worldgen_lake, the Beta 1.7.3
	WorldGenLakes pond carver (water + lava).

	Checks the feature's contract:
	  - deterministic (same seed + position -> identical buffer AND identical
	    final RNG state),
	  - PRNG byte-exactness: a water lake consumes EXACTLY nextInt(4)+4 for the
	    blob count then 6 nextDouble() per blob and nothing else -- proven by
	    hand-stepping an identical RNG and comparing internal state (a wrong
	    order would desync every later decoration in the chunk),
	  - structure: fluid is WATER_STILL only in the lower half (y<4 of the
	    8-tall mask) and within the 16x16 footprint, with air carved above,
	  - validation abort: a lake whose lower shell rests on non-solid blocks (an
	    all-air world) returns false and writes nothing,
	  - a lava lake also generates deterministically (its extra stone-rim
	    nextInt(2) edge pass runs reproducibly).
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
#define GRASS 2
#define DIRT 3
#define WATER_STILL 9
#define LAVA_STILL 11

// Flat world spanning the lake footprint (16x16 at origX-8..origX+8) with
// margin. World-coord buffer, origin (0,0,0); OOB reads return AIR.
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
static int fw_height(void* ctx, int x, int z) {
	(void)ctx;
	(void)x;
	(void)z;
	return 64;
}
// Full daylight everywhere, so the grass pass may convert exposed dirt at the
// waterline (its exact placement is mask-dependent, so observed, not asserted).
static int fw_sky(void* ctx, int x, int y, int z) {
	(void)ctx;
	(void)x;
	(void)y;
	(void)z;
	return 15;
}

// Solid stone with a dirt plane at y=63 (gives the grass pass dirt to act on at
// the waterline). The lower lake shell is therefore solid -> a lake generates.
static void fw_fill_stone(struct flatworld* f) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++)
				f->ids[(x * FW_W + z) * 128 + y] = (y == 63) ? DIRT : STONE;
	memset(f->meta, 0, sizeof(f->meta));
}
static void fw_fill_air(struct flatworld* f) {
	memset(f->ids, AIR, sizeof(f->ids));
	memset(f->meta, 0, sizeof(f->meta));
}

int main(void) {
	struct flatworld* fa = malloc(sizeof *fa);
	struct flatworld* fb = malloc(sizeof *fb);
	struct flatworld* pristine = malloc(sizeof *pristine);
	struct wgen_world wa = {fa, fw_get, fw_set, fw_height, fw_sky};
	struct wgen_world wb = {fb, fw_get, fw_set, fw_height, fw_sky};

	// Lake centre (pre -8 offset); footprint is x,z in [16,32). The descend in
	// all-stone stops at y=64, so the mask base is y=60: lower half [60,64),
	// upper half [64,68).
	const int OX = 24, OY = 64, OZ = 24;
	const int64_t SEED = 0x1A4Eull;

	// --- Determinism: same seed + position -> identical buffer + RNG state.
	fw_fill_stone(fa);
	fw_fill_stone(fb);
	fw_fill_stone(pristine);
	struct java_random ra, rb;
	jrand_set_seed(&ra, SEED);
	jrand_set_seed(&rb, SEED);
	bool oka = worldgen_lake(&wa, &ra, OX, OY, OZ, WATER_STILL);
	bool okb = worldgen_lake(&wb, &rb, OX, OY, OZ, WATER_STILL);
	assert(oka == okb);
	assert(oka); // solid shell, no nearby liquid -> this lake generates
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0);
	assert(ra.seed == rb.seed);				   // identical randomness consumed
	assert(memcmp(fa->ids, pristine->ids, sizeof(fa->ids)) != 0); // wrote sth

	// --- PRNG byte-exactness (water lake): blobCount + 6 doubles per blob, and
	// nothing else (validation/fill/grass consume no randomness).
	struct java_random rcheck, rexpect;
	jrand_set_seed(&rcheck, SEED);
	jrand_set_seed(&rexpect, SEED);
	struct flatworld* fc = malloc(sizeof *fc);
	fw_fill_stone(fc);
	struct wgen_world wc = {fc, fw_get, fw_set, fw_height, fw_sky};
	bool okc = worldgen_lake(&wc, &rcheck, OX, OY, OZ, WATER_STILL);
	assert(okc);
	int blobs = jrand_next_int_bound(&rexpect, 4) + 4;
	for(int b = 0; b < blobs; b++)
		for(int d = 0; d < 6; d++)
			jrand_next_double(&rexpect);
	assert(rcheck.seed == rexpect.seed);

	// --- Structure: WATER_STILL only in the lower half [60,64) and inside the
	// 16x16 footprint, with meta 0; air carved in the upper half; some water.
	long water = 0, carved_air = 0, grass = 0;
	for(int x = 0; x < FW_W; x++) {
		for(int z = 0; z < FW_W; z++) {
			for(int y = 0; y < 128; y++) {
				int idx = (x * FW_W + z) * 128 + y;
				uint8_t now = fa->ids[idx];
				if(now == WATER_STILL) {
					water++;
					assert(x >= 16 && x < 32 && z >= 16 && z < 32);
					assert(y >= 60 && y < 64);
					assert(fa->meta[idx] == 0);
				} else if(now == GRASS) {
					grass++;
				}
			}
		}
	}
	for(int x = 16; x < 32; x++)
		for(int z = 16; z < 32; z++)
			for(int y = 64; y < 68; y++)
				if(fa->ids[(x * FW_W + z) * 128 + y] == AIR)
					carved_air++;
	assert(water > 0);		// fluid actually placed in the bowl
	assert(carved_air > 0); // air carved above the waterline

	// --- Validation abort: an all-air world has a non-solid lower shell, so
	// the lake aborts during validation (after the blob draws, before any
	// write) and leaves the world untouched.
	struct flatworld* fd = malloc(sizeof *fd);
	struct flatworld* fd0 = malloc(sizeof *fd0);
	fw_fill_air(fd);
	fw_fill_air(fd0);
	struct wgen_world wd = {fd, fw_get, fw_set, fw_height, fw_sky};
	struct java_random rd;
	jrand_set_seed(&rd, SEED);
	bool okd = worldgen_lake(&wd, &rd, OX, OY, OZ, WATER_STILL);
	assert(!okd);
	assert(memcmp(fd->ids, fd0->ids, sizeof(fd->ids)) == 0);

	// --- Lava lake: generates deterministically (covers the stone-rim
	// nextInt(2) edge pass consuming randomness reproducibly).
	struct flatworld* fe = malloc(sizeof *fe);
	struct flatworld* ff = malloc(sizeof *ff);
	fw_fill_stone(fe);
	fw_fill_stone(ff);
	struct wgen_world we = {fe, fw_get, fw_set, fw_height, fw_sky};
	struct wgen_world wf = {ff, fw_get, fw_set, fw_height, fw_sky};
	struct java_random re, rf;
	jrand_set_seed(&re, SEED);
	jrand_set_seed(&rf, SEED);
	bool oke = worldgen_lake(&we, &re, OX, OY, OZ, LAVA_STILL);
	bool okf = worldgen_lake(&wf, &rf, OX, OY, OZ, LAVA_STILL);
	assert(oke == okf);
	assert(oke);
	assert(memcmp(fe->ids, ff->ids, sizeof(fe->ids)) == 0);
	assert(re.seed == rf.seed); // identical draws incl. the edge nextInt(2)s

	free(fa);
	free(fb);
	free(pristine);
	free(fc);
	free(fd);
	free(fd0);
	free(fe);
	free(ff);
	printf("test_lake: all assertions passed (determinism + PRNG order + "
		   "structure + validation abort; %ld water, %ld carved-air, %ld "
		   "grass placed; lava lake deterministic)\n",
		   water, carved_air, grass);
	return 0;
}
