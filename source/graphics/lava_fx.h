/*
	Beta 1.7.3 TextureLavaFX + TextureLavaFlowFX port -- procedural ANIMATED lava.

	Faithful to net.minecraft.src.TextureLavaFX (still lava) and TextureLavaFlowFX
	(flowing lava) (decompile 740c583): a per-tick sin-distorted diffusion field that
	writes two 16x16 RGBA8 tiles (still + flowing), advanced once per 20Hz client tick
	and blitted in place into the terrain-atlas lava slots.

	DEVIATION (documented, render-layer only): Beta runs TextureLavaFX and
	TextureLavaFlowFX as two INDEPENDENT instances (separate fields, separate RNG
	draws). CaveWii shares ONE field between them -- the still tile reads it directly,
	the flow tile reads it with Beta's vertical scroll `(i - k/3*16) & 255` -- because
	both are render-layer / non-parity (like water_fx's non-Beta LCG) and one field
	halves the per-frame cost on Broadway. Appearance matches Beta; the exact per-tile
	RNG stream does not (never was parity-checked).

	Algorithm (every constant load-bearing, matched to TextureLavaFX.onTick):
	  - per texel: sin-distort the sample centre by (int)(sin(i*2PI/16)*1.2) on each
	    axis, sum the distorted 3x3 neighbourhood (X/Y wrap), /10, plus 0.8 * the
	    2x2 displacement average -> field_next.
	  - disp += vel*0.01 (floored >=0); vel -= 0.06; 0.5% chance/texel vel = 1.5.
	  - swap field/field_next.
	  - colour: v = clamp(field*2, 0, 1); R=v*100+155, G=v*v*255, B=v^4*128, A=255.
	    The flow tile reads field at the scrolled index instead of the direct one.
*/

#ifndef GRAPHICS_LAVA_FX_H
#define GRAPHICS_LAVA_FX_H

#include <stdint.h>

#define LAVA_FX_DIM 16
#define LAVA_FX_TEXELS (LAVA_FX_DIM * LAVA_FX_DIM)
#define LAVA_FX_BYTES (LAVA_FX_TEXELS * 4)

struct lava_fx {
	float field[LAVA_FX_TEXELS];      // current diffusion field (read for colour)
	float field_next[LAVA_FX_TEXELS]; // scratch written each tick
	float disp[LAVA_FX_TEXELS];       // accumulated displacement (>=0)
	float vel[LAVA_FX_TEXELS];        // velocity (decays, randomly kicked)
	uint32_t prng;                    // self-contained LCG state
	uint32_t k;                       // frame counter (flow scroll: k/3)
	uint8_t rgba_still[LAVA_FX_BYTES]; // still-lava tile, refreshed every tick
	uint8_t rgba_flow[LAVA_FX_BYTES];  // flowing-lava tile (scrolled), refreshed too
};

// Pure sim (engine-free).
void lava_fx_init(struct lava_fx* w, uint32_t seed);
void lava_fx_tick(struct lava_fx* w); // advance one 20Hz tick + refresh both tiles
const uint8_t* lava_fx_pixels_still(const struct lava_fx* w);
const uint8_t* lava_fx_pixels_flow(const struct lava_fx* w);
// Read-only colour input for still texel i (0..LAVA_FX_TEXELS-1).
float lava_fx_field_value(const struct lava_fx* w, int i);

// Pure per-texel colour mapping (exposed for the golden test): clamp v=field*2 to
// [0,1], then R=v*100+155, G=v*v*255, B=v^4*128, A=255. out must hold 4 bytes.
void lava_fx_color(float field_value, uint8_t out[4]);

#endif
