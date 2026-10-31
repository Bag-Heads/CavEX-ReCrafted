/*
	Tests source/worldgen/populate.c -- worldgen_ores, the Beta 1.7.3 ore-vein
	pass (dirt/gravel/coal/iron/gold/redstone/diamond/lapis).

	The per-vein PRNG order + geometry are proven in test_minable; this checks
	the ore PASS's contract:
	  - deterministic (same seed -> identical result AND identical final RNG
	    state, i.e. the whole pass consumes randomness reproducibly),
	  - replaces STONE only (a bedrock floor + a bedrock column placed in the
	    vein zone are never disturbed),
	  - every changed block was stone and became one of the eight ore ids,
	  - it actually places ore, including the high-count passes (dirt, coal).
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "worldgen/java_random.h"
#include "worldgen/populate.h"

#define STONE 1
#define BEDROCK 7
#define DIRT 3
#define GRAVEL 13
#define COAL 16
#define IRON 15
#define GOLD 14
#define REDSTONE 73
#define DIAMOND 56
#define LAPIS 21

// Flat world spanning the chunk + its +X/+Z spill (veins centre at col+8).
#define FW_W 48
struct flatworld {
	uint8_t ids[FW_W * FW_W * 128];
	uint8_t meta[FW_W * FW_W * 128];
};

static int fw_index(int x, int y, int z, int* idx) {
	if(y < 0 || y >= 128)
		return 0;
	if(x < 0 || x >= FW_W || z < 0 || z >= FW_W)
		return 0;
	*idx = (x * FW_W + z) * 128 + y;
	return 1;
}
static uint8_t fw_get(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(x, y, z, &idx) ? f->ids[idx] : 0;
}
static void fw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct flatworld* f = ctx;
	int idx;
	if(fw_index(x, y, z, &idx)) {
		f->ids[idx] = id;
		f->meta[idx] = meta;
	}
}

// Solid stone, with a bedrock floor (y=0) and a bedrock column at (10,10) sat
// in the middle of the vein zone -- both must survive (stone-only replacement).
static void fw_fill(struct flatworld* f) {
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++)
			for(int y = 0; y < 128; y++) {
				uint8_t b = STONE;
				if(y == 0 || (x == 10 && z == 10))
					b = BEDROCK;
				f->ids[(x * FW_W + z) * 128 + y] = b;
			}
	memset(f->meta, 0, sizeof(f->meta));
}

static int is_ore(uint8_t b) {
	return b == DIRT || b == GRAVEL || b == COAL || b == IRON || b == GOLD
		|| b == REDSTONE || b == DIAMOND || b == LAPIS;
}

int main(void) {
	struct flatworld* fa = malloc(sizeof *fa);
	struct flatworld* fb = malloc(sizeof *fb);
	struct flatworld* pristine = malloc(sizeof *pristine);
	struct wgen_world wa = {fa, fw_get, fw_set, NULL, NULL};
	struct wgen_world wb = {fb, fw_get, fw_set, NULL, NULL};

	const int64_t SEED = 0xABCDEFLL;

	fw_fill(fa);
	fw_fill(fb);
	fw_fill(pristine);
	struct java_random ra, rb;
	jrand_set_seed(&ra, SEED);
	jrand_set_seed(&rb, SEED);
	worldgen_ores(&wa, &ra, 0, 0);
	worldgen_ores(&wb, &rb, 0, 0);
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0); // deterministic
	assert(ra.seed == rb.seed); // identical randomness consumed

	long placed = 0, dirt = 0, coal = 0;
	for(int i = 0; i < FW_W * FW_W * 128; i++) {
		uint8_t was = pristine->ids[i], now = fa->ids[i];
		if(was == BEDROCK)
			assert(now == BEDROCK); // stone-only: bedrock never touched
		if(was != now) {
			assert(was == STONE && is_ore(now));
			placed++;
			if(now == DIRT)
				dirt++;
			if(now == COAL)
				coal++;
		}
	}
	assert(placed > 0);
	assert(dirt > 0); // 20 size-32 dirt blobs
	assert(coal > 0); // 20 size-16 coal veins

	free(fa);
	free(fb);
	free(pristine);
	printf("test_ores: all assertions passed (determinism + stone-only + "
		   "bedrock-safe; %ld ore blocks placed, %ld dirt, %ld coal)\n",
		   placed, dirt, coal);
	return 0;
}
