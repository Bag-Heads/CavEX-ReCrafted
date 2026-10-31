/*
	Golden test for the Beta 1.7.3 TextureLavaFX + TextureLavaFlowFX port
	(source/graphics/lava_fx.c). Pins the load-bearing faithfulness invariants:
	  (A) the exact per-texel colour mapping (v=clamp(field*2,0,1); R=v*100+155,
	      G=v^2*255, B=v^4*128, A=255) at the clean endpoints + both clamps;
	  (B) the resting (zero-state) tile colours flat everywhere;
	  (C) across live ticks: channels stay in range, no NaN/overflow, the still AND
	      flow tiles both animate, and the sim is deterministic for a fixed seed.
*/

#include "../source/graphics/lava_fx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_color(float field, int r, int g, int b, int a) {
	uint8_t o[4];
	lava_fx_color(field, o);
	assert(o[0] == r && o[1] == g && o[2] == b && o[3] == a);
}

int main(void) {
	// (A) colour formula endpoints + clamps (v = field*2).
	check_color(0.0f, 155, 0, 0, 255);   // v=0
	check_color(0.5f, 255, 255, 128, 255); // v=1
	check_color(2.0f, 255, 255, 128, 255); // v=4 -> clamp high == v=1
	check_color(-1.0f, 155, 0, 0, 255);  // v<0 -> clamp low == v=0

	// (B) resting tile: fresh (zero) field colours flat (155,0,0,255).
	struct lava_fx w;
	lava_fx_init(&w, 12345u);
	const uint8_t* s = lava_fx_pixels_still(&w);
	const uint8_t* f = lava_fx_pixels_flow(&w);
	for(int i = 0; i < LAVA_FX_TEXELS; i++) {
		assert(s[i * 4] == 155 && s[i * 4 + 1] == 0 && s[i * 4 + 2] == 0
			   && s[i * 4 + 3] == 255);
		assert(f[i * 4] == 155 && f[i * 4 + 1] == 0 && f[i * 4 + 2] == 0
			   && f[i * 4 + 3] == 255);
	}

	// (C) live ticks: channels in range, no overflow, every tick, both tiles.
	for(int t = 0; t < 300; t++) {
		lava_fx_tick(&w);
		s = lava_fx_pixels_still(&w);
		f = lava_fx_pixels_flow(&w);
		for(int i = 0; i < LAVA_FX_TEXELS; i++) {
			assert(s[i * 4] >= 155 && s[i * 4] <= 255);      // R = v*100+155
			assert(s[i * 4 + 2] <= 128);                     // B = v^4*128
			assert(s[i * 4 + 3] == 255);                     // A opaque
			assert(f[i * 4] >= 155 && f[i * 4] <= 255);
			assert(f[i * 4 + 2] <= 128);
			assert(f[i * 4 + 3] == 255);
		}
	}

	// both tiles must actually ANIMATE (not the resting flat tile).
	int s_changed = 0, f_changed = 0;
	s = lava_fx_pixels_still(&w);
	f = lava_fx_pixels_flow(&w);
	for(int i = 0; i < LAVA_FX_TEXELS; i++) {
		if(s[i * 4] != 155 || s[i * 4 + 1] != 0)
			s_changed = 1;
		if(f[i * 4] != 155 || f[i * 4 + 1] != 0)
			f_changed = 1;
	}
	assert(s_changed && f_changed);

	// determinism: same seed -> byte-identical (golden reproducibility).
	struct lava_fx a, b;
	lava_fx_init(&a, 777u);
	lava_fx_init(&b, 777u);
	for(int t = 0; t < 150; t++) {
		lava_fx_tick(&a);
		lava_fx_tick(&b);
	}
	assert(memcmp(lava_fx_pixels_still(&a), lava_fx_pixels_still(&b),
				  LAVA_FX_BYTES)
		   == 0);
	assert(memcmp(lava_fx_pixels_flow(&a), lava_fx_pixels_flow(&b), LAVA_FX_BYTES)
		   == 0);

	printf("test_lava_fx: PASS\n");
	return 0;
}
