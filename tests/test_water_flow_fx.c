/*
	Golden test for the Beta 1.7.3 TextureWaterFlowFX port
	(source/graphics/water_flow_fx.c).

	THE BUG THIS EXISTS FOR (2026-07-31). Beta colours flowing water with the
	byte-identical mapping it uses for still water -- blue channel pinned to
	255. We had no flow port at all, so every horizontal face of every water
	block (BlockFluid.getBlockTextureFromSide sends the sides to the flow tile)
	drew the fork's placeholder art, whose blue channel is 75..91. Blue tops,
	grey sides: "water textures still not beta accurate".

	Pins:
	  (A) flow shares the still colour mapping exactly, B==255 included;
	  (B) the resting tile is flat (32,50,255,146);
	  (C) over many live ticks: B==255 everywhere, R/G/A in Beta's ranges,
	      no NaN, the tile animates, and a fixed seed is deterministic;
	  (D) the SCROLL -- Beta reads field[(i - ticks*16) & 255], so after N
	      ticks the emitted tile is the field rotated by N rows;
	  (E) the flow sim is genuinely DIFFERENT from the still sim (the five
	      kernel deltas), so a copy-paste of water_fx.c cannot pass.
*/

#include "../source/graphics/water_flow_fx.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../source/graphics/water_fx.h"

int main(void) {
	// (A) the colour mapping is literally water_fx_color -- the same eight lines
	// in both decompiled classes. Endpoints + clamps, with B pinned.
	uint8_t o[4];
	water_fx_color(0.0f, o);
	assert(o[0] == 32 && o[1] == 50 && o[2] == 255 && o[3] == 146);
	water_fx_color(1.0f, o);
	assert(o[0] == 64 && o[1] == 114 && o[2] == 255 && o[3] == 196);
	water_fx_color(-5.0f, o);
	assert(o[2] == 255 && o[0] == 32); // clamped low
	water_fx_color(5.0f, o);
	assert(o[2] == 255 && o[0] == 64); // clamped high

	// (B) resting tile.
	struct water_flow_fx w;
	water_flow_fx_init(&w, 12345u);
	const uint8_t* px = water_flow_fx_pixels(&w);
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		assert(px[i * 4 + 0] == 32 && px[i * 4 + 1] == 50);
		assert(px[i * 4 + 2] == 255 && px[i * 4 + 3] == 146);
	}
	assert(w.ticks == 0);

	// (C) live ticks: the invariant that was broken in the ART is enforced here
	// in the SOURCE -- flowing water is blue, always.
	bool animated = false;
	uint8_t first[WATER_FX_BYTES];
	memcpy(first, px, WATER_FX_BYTES);
	for(int t = 0; t < 400; t++) {
		water_flow_fx_tick(&w);
		px = water_flow_fx_pixels(&w);
		for(int i = 0; i < WATER_FX_TEXELS; i++) {
			assert(px[i * 4 + 2] == 255 && "flowing water MUST be blue (B=255)");
			assert(px[i * 4 + 0] >= 32 && px[i * 4 + 0] <= 64);
			assert(px[i * 4 + 1] >= 50 && px[i * 4 + 1] <= 114);
			assert(px[i * 4 + 3] >= 146 && px[i * 4 + 3] <= 196);
		}
		for(int i = 0; i < WATER_FX_TEXELS; i++)
			assert(!isnan(w.field[i]) && !isinf(w.field[i]));
		if(memcmp(px, first, WATER_FX_BYTES) != 0)
			animated = true;
	}
	assert(animated && "the flow tile must actually animate");
	assert(w.ticks == 400);

	// determinism for a fixed seed (this golden must be reproducible)
	struct water_flow_fx a, b;
	water_flow_fx_init(&a, 777u);
	water_flow_fx_init(&b, 777u);
	for(int t = 0; t < 50; t++) {
		water_flow_fx_tick(&a);
		water_flow_fx_tick(&b);
	}
	assert(memcmp(water_flow_fx_pixels(&a), water_flow_fx_pixels(&b),
				  WATER_FX_BYTES)
		   == 0);

	// (D) THE SCROLL. Beta taps field[(i - ticks*16) & 255]; 16 texels is one
	// row, so the emitted tile is the field rotated up by `ticks` rows. Verify
	// the emitted pixels equal the colour of the correspondingly rotated field.
	struct water_flow_fx s;
	water_flow_fx_init(&s, 4242u);
	for(int t = 0; t < 37; t++)
		water_flow_fx_tick(&s);
	const uint8_t* sp = water_flow_fx_pixels(&s);
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		uint32_t src = ((uint32_t)i - s.ticks * 16u) & 255u;
		uint8_t want[4];
		water_fx_color(s.field[src], want);
		assert(memcmp(sp + i * 4, want, 4) == 0
			   && "emitted texel must be the field tapped at the scrolled index");
	}
	// and the scroll must be a real displacement: with ticks not a multiple of
	// 16 rows the tile cannot equal the unscrolled colouring (unless the field
	// is flat, which after 37 ticks it is not).
	{
		bool differs = false;
		for(int i = 0; i < WATER_FX_TEXELS; i++) {
			uint8_t unscrolled[4];
			water_fx_color(s.field[i], unscrolled);
			if(memcmp(sp + i * 4, unscrolled, 4) != 0)
				differs = true;
		}
		assert(differs && "a non-zero scroll must move the emitted tile");
	}

	// (E) the flow kernel must NOT be the still kernel. Drive both sims from the
	// same seed and require the fields to diverge -- this fails if someone
	// copy-pastes water_fx.c (centred horizontal blur /3.3, decay 0.1, kick
	// 0.05) into the flow port instead of the trailing vertical /3.2, 0.3, 0.2.
	struct water_flow_fx f;
	struct water_fx st;
	water_flow_fx_init(&f, 999u);
	water_fx_init(&st, 999u);
	for(int t = 0; t < 60; t++) {
		water_flow_fx_tick(&f);
		water_fx_tick(&st);
	}
	bool kernels_differ = false;
	for(int i = 0; i < WATER_FX_TEXELS; i++)
		if(fabsf(f.field[i] - st.field[i]) > 1e-6f)
			kernels_differ = true;
	assert(kernels_differ
		   && "flow must use its own kernel (vertical /3.2, decay .3, kick .2)");

	printf("test_water_flow_fx: colour mapping, B=255 invariant, scroll and "
		   "kernel-distinctness OK\n");
	return 0;
}
