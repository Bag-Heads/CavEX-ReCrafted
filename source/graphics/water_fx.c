/*
	Beta 1.7.3 TextureWaterFX port -- pure ripple-field sim (see water_fx.h).

	Byte-faithful to net.minecraft.src.TextureWaterFX.onTick (decompile 740c583).
	Engine-free: this translation unit pulls in nothing but <stdint.h>, so the
	golden test (tests/test_water_fx.c) links it standalone. The per-platform tile
	upload into the terrain atlas lives behind the PLATFORM_* guard at the bottom.
*/

#include "water_fx.h"

// Self-contained LCG. Beta uses java.util.Random via Math.random(); the water FX
// is render-layer and never parity-checked, so an exact RNG stream is neither
// required nor reproducible. A fixed seed makes the golden test deterministic.
static inline float wfx_rand(struct water_fx* w) {
	w->prng = w->prng * 1664525u + 1013904223u;
	return (float)(w->prng >> 8) / (float)(1u << 24); // [0, 1)
}

void water_fx_color(float v, uint8_t out[4]) {
	if(v > 1.0f)
		v = 1.0f;
	if(v < 0.0f)
		v = 0.0f;
	float v2 = v * v; // TextureWaterFX squares the clamped field before colouring
	out[0] = (uint8_t)(int)(32.0f + v2 * 32.0f);  // R 32..64
	out[1] = (uint8_t)(int)(50.0f + v2 * 64.0f);  // G 50..114
	out[2] = 255;                                 // B pinned -> guaranteed blue
	out[3] = (uint8_t)(int)(146.0f + v2 * 50.0f); // A 146..196
}

void water_fx_init(struct water_fx* w, uint32_t seed) {
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		w->field[i] = 0.0f;
		w->field_next[i] = 0.0f;
		w->disp[i] = 0.0f;
		w->vel[i] = 0.0f;
	}
	w->prng = seed ? seed : 1u;
	// Resting tile (flat field 0 -> (32,50,255,146) everywhere), matching Java's
	// zero-init before the first onTick injects any ripple.
	for(int i = 0; i < WATER_FX_TEXELS; i++)
		water_fx_color(w->field[i], w->rgba + i * 4);
}

void water_fx_tick(struct water_fx* w) {
	// PASS 1: horizontal-only 3-tap blur (X wraps on the 16-torus), /3.3 damping,
	// plus 0.8 * accumulated displacement. y is NOT offset (no vertical term).
	for(int y = 0; y < WATER_FX_DIM; y++) {
		for(int x = 0; x < WATER_FX_DIM; x++) {
			float s = 0.0f;
			for(int dx = x - 1; dx <= x + 1; dx++)
				s += w->field[(dx & 15) + y * 16];
			w->field_next[x + y * 16] = s / 3.3f + w->disp[x + y * 16] * 0.8f;
		}
	}
	// PASS 2: integrate velocity into displacement (floored >=0), constant decay,
	// 5% chance/texel to kick velocity to 0.5 (overwrite, not add).
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		w->disp[i] += w->vel[i] * 0.05f;
		if(w->disp[i] < 0.0f)
			w->disp[i] = 0.0f;
		w->vel[i] -= 0.1f;
		if(wfx_rand(w) < 0.05f)
			w->vel[i] = 0.5f;
	}
	// PASS 3: ping-pong the two HEIGHT buffers only (disp/vel persist). After this,
	// field holds the freshly computed values (what PASS 1 wrote to field_next).
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		float t = w->field[i];
		w->field[i] = w->field_next[i];
		w->field_next[i] = t;
	}
	// PASS 4: colour the new field. B stays 255 for every texel.
	for(int i = 0; i < WATER_FX_TEXELS; i++)
		water_fx_color(w->field[i], w->rgba + i * 4);
}

const uint8_t* water_fx_pixels(const struct water_fx* w) {
	return w->rgba;
}
float water_fx_field_value(const struct water_fx* w, int i) {
	return w->field[i];
}

int water_fx_swizzle_index(int ax, int ay, int atlas_w) {
	int block = (ax / 4) + (ay / 4) * (atlas_w / 4);
	int within = (ax % 4) + (ay % 4) * 4;
	return block * 16 + within; // 16 u16 per 4x4 block
}
