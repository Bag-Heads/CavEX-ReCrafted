/*
	Tests source/worldgen/minable.c -- the Beta 1.7.3 WorldGenMinable ore vein.

	The byte-exact PRNG + Beta sin/cos table it relies on are proven elsewhere
	(test_java_random); this checks the vein algorithm's contract:
	  - deterministic (same seed + position -> identical carving),
	  - replaces STONE only (never disturbs any other block id),
	  - it actually carves (a non-empty vein for a normal size),
	  - every placed block lies inside the geometric bounding box of the vein
	    (i.e. nothing is written wildly out of range), and
	  - CRUCIALLY: the PRNG consumption order/count is EXACTLY nextFloat,
	    nextInt(3), nextInt(3), then one nextDouble per step l in [0,size] --
	    proven by hand-stepping an identical RNG and comparing internal state.
	    (A wrong Random order would desync every later decoration in the chunk.)
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/java_random.h"
#include "worldgen/populate.h"

#define STONE 1
#define DIRT 3
#define AIR 0
#define COAL_ORE 16

// Flat world-view: a buffer of W x 128 x W blocks with world-coord origin
// (ox,0,oz). Out-of-buffer / out-of-world reads return AIR; writes are ignored.
#define FW_W 48
struct flatworld {
	uint8_t ids[FW_W * FW_W * 128];
	uint8_t meta[FW_W * FW_W * 128];
	int ox, oz;
};

static int fw_index(struct flatworld* f, int x, int y, int z, int* idx) {
	if(y < 0 || y >= 128)
		return 0;
	int lx = x - f->ox, lz = z - f->oz;
	if(lx < 0 || lx >= FW_W || lz < 0 || lz >= FW_W)
		return 0;
	*idx = (lx * FW_W + lz) * 128 + y;
	return 1;
}

static uint8_t fw_get(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(f, x, y, z, &idx) ? f->ids[idx] : AIR;
}

static void fw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct flatworld* f = ctx;
	int idx;
	if(fw_index(f, x, y, z, &idx)) {
		f->ids[idx] = id;
		f->meta[idx] = meta;
	}
}

// Fill solid stone, then stripe every 4th column in x with dirt so we can prove
// non-stone is never touched.
static void fw_fill(struct flatworld* f) {
	for(int lx = 0; lx < FW_W; lx++)
		for(int lz = 0; lz < FW_W; lz++)
			for(int y = 0; y < 128; y++)
				f->ids[(lx * FW_W + lz) * 128 + y]
					= (lx % 4 == 0) ? DIRT : STONE;
	memset(f->meta, 0, sizeof(f->meta));
}

int main(void) {
	struct flatworld* fa = malloc(sizeof(struct flatworld));
	struct flatworld* fb = malloc(sizeof(struct flatworld));
	fa->ox = fb->ox = 0;
	fa->oz = fb->oz = 0;

	struct wgen_world wa = {fa, fw_get, fw_set, NULL, NULL};
	struct wgen_world wb = {fb, fw_get, fw_set, NULL, NULL};

	// Seed the vein near the buffer middle so it fits comfortably.
	const int VI = 4, VJ = 40, VK = 4, VSIZE = 32;
	const int64_t VSEED = 0x51EDull;

	// --- Determinism: same seed + position -> identical buffer.
	fw_fill(fa);
	fw_fill(fb);
	struct java_random ra, rb;
	jrand_set_seed(&ra, VSEED);
	jrand_set_seed(&rb, VSEED);
	worldgen_minable(&wa, &ra, VI, VJ, VK, VSIZE, COAL_ORE, 0);
	worldgen_minable(&wb, &rb, VI, VJ, VK, VSIZE, COAL_ORE, 0);
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0);

	// --- Stone-only + actually-carves + bounded. Compare against the pristine
	// fill: every changed cell must have been STONE and become COAL_ORE, must
	// sit within the vein's generous bounding box, and there must be some.
	struct flatworld* pristine = malloc(sizeof(struct flatworld));
	fw_fill(pristine);
	long carved = 0;
	for(int lx = 0; lx < FW_W; lx++) {
		for(int lz = 0; lz < FW_W; lz++) {
			for(int y = 0; y < 128; y++) {
				int idx = (lx * FW_W + lz) * 128 + y;
				uint8_t was = pristine->ids[idx];
				uint8_t now = fa->ids[idx];
				if(was != now) {
					assert(was == STONE && now == COAL_ORE);
					// Vein centre is (VI+8, VJ, VK+8); horizontal reach <=
					// size/8 + a few blocks of sphere radius, y within a small
					// band around VJ. Use a loose box that still excludes wild
					// writes.
					int wx = lx + fa->ox, wz = lz + fa->oz;
					assert(wx >= VI + 8 - VSIZE && wx <= VI + 8 + VSIZE);
					assert(wz >= VK + 8 - VSIZE && wz <= VK + 8 + VSIZE);
					assert(y >= VJ - 16 && y <= VJ + 16);
					carved++;
				}
			}
		}
	}
	assert(carved > 0);

	// Dirt columns (lx % 4 == 0) must be byte-identical to the fill.
	for(int lx = 0; lx < FW_W; lx += 4)
		for(int lz = 0; lz < FW_W; lz++)
			assert(memcmp(&fa->ids[(lx * FW_W + lz) * 128],
						  &pristine->ids[(lx * FW_W + lz) * 128], 128)
				   == 0);

	// --- PRNG order/count: hand-step an identical RNG through exactly the
	// draws minable should make and assert the internal state matches.
	struct java_random rcheck, rexpect;
	jrand_set_seed(&rcheck, VSEED);
	jrand_set_seed(&rexpect, VSEED);
	struct flatworld* fc = malloc(sizeof(struct flatworld));
	fw_fill(fc);
	struct wgen_world wc = {fc, fw_get, fw_set, NULL, NULL};
	worldgen_minable(&wc, &rcheck, VI, VJ, VK, VSIZE, COAL_ORE, 0);
	jrand_next_float(&rexpect);			   // angle
	jrand_next_int_bound(&rexpect, 3);	   // y_start
	jrand_next_int_bound(&rexpect, 3);	   // y_end
	for(int l = 0; l <= VSIZE; l++)
		jrand_next_double(&rexpect);	   // one scale draw per step
	assert(rcheck.seed == rexpect.seed);

	free(fa);
	free(fb);
	free(fc);
	free(pristine);

	printf("test_minable: all assertions passed (determinism + stone-only + "
		   "bounded; %ld ore placed; PRNG order verified)\n",
		   carved);
	return 0;
}
