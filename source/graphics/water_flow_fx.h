/*
	Beta 1.7.3 TextureWaterFlowFX port -- the FLOWING-water filmstrip source.

	Companion to water_fx.h (TextureWaterFX, the STILL tile). Beta runs two
	separate ripple sims with the same colour mapping but different kernels, and
	BlockFluid.getBlockTextureFromSide hands the still tile to the top/bottom
	faces and the flow tile to every horizontal face. Ours only ever had the
	still port, so flowing water -- i.e. the SIDE of every water block -- fell
	back to the fork's placeholder art, which is grey (blue channel 75..91)
	where Beta's is a hard 255. That grey-sides-under-a-blue-top mismatch is the
	"water textures still not beta accurate" report of 2026-07-31.
*/

#ifndef WATER_FLOW_FX_H
#define WATER_FLOW_FX_H

#include <stdint.h>

#include "water_fx.h" // WATER_FX_DIM / _TEXELS / _BYTES and water_fx_color

struct water_flow_fx {
	float field[WATER_FX_TEXELS];      // current height field (read for colour)
	float field_next[WATER_FX_TEXELS]; // scratch height written each tick
	float disp[WATER_FX_TEXELS];       // accumulated displacement (>=0)
	float vel[WATER_FX_TEXELS];        // velocity (decays, randomly kicked)
	uint32_t ticks;                    // Beta's field_1134_k -- drives the scroll
	uint32_t prng;                     // self-contained LCG state
	uint8_t rgba[WATER_FX_BYTES];      // RGBA8 output, refreshed every tick
};

void water_flow_fx_init(struct water_flow_fx* w, uint32_t seed);
void water_flow_fx_tick(struct water_flow_fx* w);
const uint8_t* water_flow_fx_pixels(const struct water_flow_fx* w);
// Read-only colour input at the scrolled tap for texel i (0..255).
float water_flow_fx_field_value(const struct water_flow_fx* w, int i);

#endif
