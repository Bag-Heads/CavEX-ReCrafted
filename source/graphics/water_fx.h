/*
	Beta 1.7.3 TextureWaterFX port -- a 16x16 procedural ANIMATED water tile.

	Faithful to net.minecraft.src.TextureWaterFX (decompile 740c583): a per-tick
	ripple-field simulation that writes a 16x16 RGBA8 tile, advanced once per 20Hz
	client tick (gated by !paused), then blitted in place into the terrain atlas
	water slot. This header declares the PURE, engine-free sim (host-testable,
	links standalone) plus the per-platform upload (defined in water_fx.c behind
	a PLATFORM_WII / PLATFORM_PC split).

	Algorithm (every constant load-bearing, matched to TextureWaterFX.onTick):
	  - PASS 1: horizontal-only 3-tap blur with X wrap, /3.3 (built-in decay) plus
	            0.8 * displacement coupling -> field_next.
	  - PASS 2: disp += vel*0.05 (floored >=0); vel -= 0.1; 5% chance/texel vel=0.5.
	  - PASS 3: ping-pong the two HEIGHT buffers (disp/vel persist).
	  - PASS 4: v = clamp(field,0,1); v2 = v*v;
	            R=32+v2*32, G=50+v2*64, B=255 (pinned), A=146+v2*50.

	Render-layer only: changes no worldgen/redstone/fluid/lighting byte. The RNG is
	a self-contained LCG (water FX is non-deterministic by design and never parity-
	checked; a fixed seed keeps the golden test reproducible) -- the one documented
	deviation from Beta's java.util.Random.
*/

#ifndef GRAPHICS_WATER_FX_H
#define GRAPHICS_WATER_FX_H

#include <stdint.h>

#define WATER_FX_DIM 16
#define WATER_FX_TEXELS (WATER_FX_DIM * WATER_FX_DIM)
#define WATER_FX_BYTES (WATER_FX_TEXELS * 4)

struct water_fx {
	float field[WATER_FX_TEXELS];      // current height field (read for color)
	float field_next[WATER_FX_TEXELS]; // scratch height written each tick
	float disp[WATER_FX_TEXELS];       // accumulated displacement (>=0)
	float vel[WATER_FX_TEXELS];        // velocity (decays, randomly kicked)
	uint32_t prng;                     // self-contained LCG state
	uint8_t rgba[WATER_FX_BYTES];      // RGBA8 output, refreshed every tick
};

// Pure sim (engine-free).
void water_fx_init(struct water_fx* w, uint32_t seed);
void water_fx_tick(struct water_fx* w);                   // advance one 20Hz tick + refresh rgba
const uint8_t* water_fx_pixels(const struct water_fx* w); // the 1024-byte RGBA8 tile
// Read-only colour input for texel i (0..WATER_FX_TEXELS-1).
float water_fx_field_value(const struct water_fx* w, int i);

// Pure per-texel colour mapping (exposed for the golden test): clamp v to [0,1],
// square it, then write R,G,B,A. out must hold 4 bytes.
void water_fx_color(float v, uint8_t out[4]);

// GX 4x4-tiled swizzle: the uint16 index of atlas texel (ax, ay) within an
// `atlas_w`-wide RGB5A3/RGBA8-class LOD (16 texels per 4x4 block, blocks row-major,
// 32 bytes = 16 u16 per block). Pure integer math, host-testable -- used by the Wii
// water_fx upload to place the live tile, alignment-agnostic (the slot origin is not
// 4-aligned because of the atlas border).
int water_fx_swizzle_index(int ax, int ay, int atlas_w);

#endif
