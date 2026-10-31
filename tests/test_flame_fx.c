/*
	Golden test for the Beta 1.7.3 TextureFlamesFX port (source/graphics/flame_fx.c).
	Pins the load-bearing faithfulness invariants:
	  (A) the exact per-texel colour mapping (v=clamp(field*1.8,0,1); R=v*155+100,
	      G=v^2*255, B=v^10*255, A = v<0.5 ? 0 : 255) at endpoints + both clamps;
	  (B) the resting (zero-state) tile colours flat everywhere;
	  (C) across live ticks: channels in range, A is 0-or-255, the tile animates,
	      and the sim is deterministic for a fixed seed.
*/

#include "../source/graphics/flame_fx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_color(float field, int r, int g, int b, int a) {
	uint8_t o[4];
	flame_fx_color(field, o);
	assert(o[0] == r && o[1] == g && o[2] == b && o[3] == a);
}

int main(void) {
	// (A) colour formula endpoints + clamps (v = field*1.8).
	check_color(0.0f, 100, 0, 0, 0);       // v=0 -> A off
	check_color(1.0f, 255, 255, 255, 255); // v=1.8 -> clamp high
	check_color(2.0f, 255, 255, 255, 255); // clamp high
	check_color(-1.0f, 100, 0, 0, 0);      // clamp low

	// (A2) fire-anim-0924/F1: Beta's blue is TEN SUCCESSIVE float multiplies,
	// `var13*var13*...*var13*255.0F` left to right (TextureFlamesFX.java:54). The
	// old grouped ((v^2)^2)^2*v^2 rounds differently for 19 field floats in
	// [0, 1/1.8]; these two are pinned (values from the Java-order product; the
	// grouped form gave B=8 and B=12).
	check_color(0x1.926c34p-2f, 209, 127, 7, 255);  // v=0.707383573
	check_color(0x1.a67152p-2f, 215, 140, 13, 255); // v=0.74257499

	// (B) resting tile: fresh (zero) field colours flat (100,0,0,0).
	struct flame_fx w;
	flame_fx_init(&w, 12345u);
	const uint8_t* px = flame_fx_pixels(&w);
	for(int i = 0; i < FLAME_FX_TEXELS; i++) {
		assert(px[i * 4] == 100 && px[i * 4 + 1] == 0 && px[i * 4 + 2] == 0
			   && px[i * 4 + 3] == 0);
	}

	// (C) live ticks: R range, A in {0,255}, no overflow, every tick.
	for(int t = 0; t < 300; t++) {
		flame_fx_tick(&w);
		px = flame_fx_pixels(&w);
		for(int i = 0; i < FLAME_FX_TEXELS; i++) {
			assert(px[i * 4] >= 100 && px[i * 4] <= 255);    // R = v*155+100
			assert(px[i * 4 + 3] == 0 || px[i * 4 + 3] == 255); // hard alpha
		}
	}

	// the tile must actually ANIMATE (some texel now lit past the resting base).
	int changed = 0;
	px = flame_fx_pixels(&w);
	for(int i = 0; i < FLAME_FX_TEXELS; i++)
		if(px[i * 4] != 100 || px[i * 4 + 3] != 0)
			changed = 1;
	assert(changed);

	// determinism: same seed -> byte-identical (golden reproducibility).
	struct flame_fx a, b;
	flame_fx_init(&a, 777u);
	flame_fx_init(&b, 777u);
	for(int t = 0; t < 150; t++) {
		flame_fx_tick(&a);
		flame_fx_tick(&b);
	}
	assert(memcmp(flame_fx_pixels(&a), flame_fx_pixels(&b), FLAME_FX_BYTES) == 0);

	printf("test_flame_fx: PASS\n");
	return 0;
}
