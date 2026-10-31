/*
	Tests source/worldgen/populate.c -- worldgen_clay, the Beta 1.7.3
	WorldGenClay blob (a minable-geometry swept blob that replaces SAND with
	CLAY, but only when its seed block is water).

	Checks:
	  - water gate: a DRY seed returns false and consumes NO randomness (the
	    check precedes every draw),
	  - PRNG byte-exactness on water: exactly nextFloat + 2x nextInt(3) + one
	    nextDouble per step l in [0,size] -- proven by hand-stepping a parallel
	    RNG (a wrong order desyncs every later decoration),
	  - replaces SAND only (-> CLAY), within the +8-offset blob,
	  - deterministic (same seed -> identical buffer + RNG state).
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
#define SAND 12
#define WATER_STILL 9
#define CLAY 82

// Flat world spanning the clay blob (centred at seed+8). World-coord buffer.
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

// All sand (so the blob has sand to convert) with a single water block at the
// clay seed (so the water gate opens).
static void fw_fill(struct flatworld* f, int wx, int wy, int wz) {
	memset(f->ids, SAND, sizeof(f->ids));
	memset(f->meta, 0, sizeof(f->meta));
	int idx;
	if(fw_index(wx, wy, wz, &idx))
		f->ids[idx] = WATER_STILL;
}

int main(void) {
	struct flatworld* fa = malloc(sizeof *fa);
	struct flatworld* fb = malloc(sizeof *fb);
	struct flatworld* pristine = malloc(sizeof *pristine);
	struct wgen_world wa = {fa, fw_get, fw_set, NULL, NULL};
	struct wgen_world wb = {fb, fw_get, fw_set, NULL, NULL};

	const int I = 4, J = 64, K = 4, SIZE = 32; // seed; blob centres at (12,~,12)
	const int64_t SEED = 0xC1A4ull;

	// --- Water gate: a dry (stone) seed consumes no randomness and writes
	// nothing.
	struct flatworld* dry = malloc(sizeof *dry);
	memset(dry->ids, STONE, sizeof(dry->ids));
	memset(dry->meta, 0, sizeof(dry->meta));
	struct flatworld* dry0 = malloc(sizeof *dry0);
	memcpy(dry0->ids, dry->ids, sizeof(dry->ids));
	struct wgen_world wdry = {dry, fw_get, fw_set, NULL, NULL};
	struct java_random rdry;
	jrand_set_seed(&rdry, SEED);
	int64_t before = rdry.seed;
	bool okdry = worldgen_clay(&wdry, &rdry, I, J, K, SIZE);
	assert(!okdry);
	assert(rdry.seed == before); // NO draw consumed off-water
	assert(memcmp(dry->ids, dry0->ids, sizeof(dry->ids)) == 0);

	// --- Determinism on water.
	fw_fill(fa, I, J, K);
	fw_fill(fb, I, J, K);
	fw_fill(pristine, I, J, K);
	struct java_random ra, rb;
	jrand_set_seed(&ra, SEED);
	jrand_set_seed(&rb, SEED);
	bool oka = worldgen_clay(&wa, &ra, I, J, K, SIZE);
	bool okb = worldgen_clay(&wb, &rb, I, J, K, SIZE);
	assert(oka && okb);
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0);
	assert(ra.seed == rb.seed);

	// --- PRNG byte-exactness: nextFloat + 2x nextInt(3) + (size+1) nextDouble.
	struct java_random rcheck, rexpect;
	jrand_set_seed(&rcheck, SEED);
	jrand_set_seed(&rexpect, SEED);
	struct flatworld* fc = malloc(sizeof *fc);
	fw_fill(fc, I, J, K);
	struct wgen_world wc = {fc, fw_get, fw_set, NULL, NULL};
	bool okc = worldgen_clay(&wc, &rcheck, I, J, K, SIZE);
	assert(okc);
	jrand_next_float(&rexpect);		   // angle
	jrand_next_int_bound(&rexpect, 3); // y_start
	jrand_next_int_bound(&rexpect, 3); // y_end
	for(int l = 0; l <= SIZE; l++)
		jrand_next_double(&rexpect);   // one scale draw per step
	assert(rcheck.seed == rexpect.seed);

	// --- Replaces SAND only, with CLAY; the water seed itself is untouched;
	// some clay was placed.
	long clay = 0;
	for(int i = 0; i < FW_W * FW_W * 128; i++) {
		uint8_t was = pristine->ids[i], now = fa->ids[i];
		if(was != now) {
			assert(was == SAND && now == CLAY);
			clay++;
		}
	}
	assert(clay > 0);
	int widx;
	fw_index(I, J, K, &widx);
	assert(fa->ids[widx] == WATER_STILL); // gate block not overwritten

	free(fa);
	free(fb);
	free(fc);
	free(pristine);
	free(dry);
	free(dry0);
	printf("test_clay: all assertions passed (water gate + PRNG order + "
		   "sand->clay only + determinism; %ld clay placed)\n",
		   clay);
	return 0;
}
