/*
	Golden test for the Beta 1.7.3 TextureWaterFX port (source/graphics/water_fx.c).

	Pins the load-bearing faithfulness invariants so a refactor can't silently
	drift the water look:
	  (A) the exact per-texel colour mapping (clamp -> v^2 -> R/G/B/A), including
	      Blue pinned at 255 (the guaranteed-blue invariant) and the clamps;
	  (B) the resting (zero-state) tile is the flat (32,50,255,146) everywhere;
	  (C) across many live ticks: B==255 for every texel, R/G/A stay in range,
	      nothing goes NaN/unbounded, the tile actually animates, and the sim is
	      deterministic for a fixed seed (so this golden is reproducible).
*/

#include "../source/graphics/water_fx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_color(float v, int r, int g, int b, int a) {
	uint8_t o[4];
	water_fx_color(v, o);
	assert(o[0] == r && o[1] == g && o[2] == b && o[3] == a);
}

int main(void) {
	// (A) per-texel formula: endpoints, midpoint, and clamping both ways.
	check_color(0.0f, 32, 50, 255, 146);   // v=0
	check_color(1.0f, 64, 114, 255, 196);  // v=1
	check_color(0.5f, 40, 66, 255, 158);   // v2=0.25
	check_color(2.0f, 64, 114, 255, 196);  // clamp high == v=1
	check_color(-1.0f, 32, 50, 255, 146);  // clamp low == v=0

	// (B) resting tile: a fresh (zero) field colours flat everywhere.
	struct water_fx w;
	water_fx_init(&w, 12345u);
	const uint8_t* px = water_fx_pixels(&w);
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		assert(px[i * 4 + 0] == 32 && px[i * 4 + 1] == 50 && px[i * 4 + 2] == 255
			   && px[i * 4 + 3] == 146);
	}

	// (C) live ticks: B pinned, channels in range, no overflow, every tick.
	for(int t = 0; t < 300; t++) {
		water_fx_tick(&w);
		px = water_fx_pixels(&w);
		for(int i = 0; i < WATER_FX_TEXELS; i++) {
			assert(px[i * 4 + 2] == 255);                            // blue pinned
			assert(px[i * 4 + 0] >= 32 && px[i * 4 + 0] <= 64);      // R range
			assert(px[i * 4 + 1] >= 50 && px[i * 4 + 1] <= 114);     // G range
			assert(px[i * 4 + 3] >= 146 && px[i * 4 + 3] <= 196);    // A range
		}
	}

	// the tile must actually ANIMATE (not stay the resting flat tile).
	int changed = 0;
	px = water_fx_pixels(&w);
	for(int i = 0; i < WATER_FX_TEXELS; i++)
		if(px[i * 4 + 0] != 32 || px[i * 4 + 1] != 50 || px[i * 4 + 3] != 146)
			changed = 1;
	assert(changed);

	// determinism: same seed -> byte-identical output (golden reproducibility).
	struct water_fx a, b;
	water_fx_init(&a, 777u);
	water_fx_init(&b, 777u);
	for(int t = 0; t < 150; t++) {
		water_fx_tick(&a);
		water_fx_tick(&b);
	}
	assert(memcmp(water_fx_pixels(&a), water_fx_pixels(&b), WATER_FX_BYTES) == 0);

	// (D) GX 4x4-swizzle index (Wii upload address math), atlas_w=256 -> 64 blocks/row.
	assert(water_fx_swizzle_index(0, 0, 256) == 0);
	assert(water_fx_swizzle_index(1, 0, 256) == 1);
	assert(water_fx_swizzle_index(0, 1, 256) == 4);
	assert(water_fx_swizzle_index(3, 3, 256) == 15);   // last texel of block 0
	assert(water_fx_swizzle_index(4, 0, 256) == 16);   // first texel of block 1
	assert(water_fx_swizzle_index(0, 4, 256) == 64 * 16); // first block of row 1
	assert(water_fx_swizzle_index(5, 2, 256) == 25);   // block 1, within 9
	// non-4-aligned slot origin (atlas border offset, e.g. x=3) still resolves right
	assert(water_fx_swizzle_index(3, 0, 256) == 3);
	assert(water_fx_swizzle_index(3, 1, 256) == 7);

	printf("test_water_fx: PASS\n");
	return 0;
}
