/*
	Beta 1.7.3 TextureLavaFX + TextureLavaFlowFX port (see lava_fx.h).

	Byte-faithful to net.minecraft.src.TextureLavaFX.onTick and
	TextureLavaFlowFX.onTick (decompile 740c583), sharing one diffusion field (the
	documented render-layer deviation in lava_fx.h). Engine-free: pulls in only
	<math.h>/<stdint.h> so the golden test (tests/test_lava_fx.c) links standalone.
*/

#include <math.h>

#include "lava_fx.h"

// Self-contained LCG (same rationale as water_fx: the FX is render-layer, never
// parity-checked; a fixed seed keeps the golden test reproducible). Beta uses
// Math.random().
static inline float lfx_rand(struct lava_fx* w) {
	w->prng = w->prng * 1664525u + 1013904223u;
	return (float)(w->prng >> 8) / (float)(1u << 24); // [0, 1)
}

void lava_fx_color(float field_value, uint8_t out[4]) {
	float v = field_value * 2.0f; // TextureLavaFX: var3 = field * 2
	if(v > 1.0f)
		v = 1.0f;
	if(v < 0.0f)
		v = 0.0f;
	out[0] = (uint8_t)(int)(v * 100.0f + 155.0f);       // R 155..255
	out[1] = (uint8_t)(int)(v * v * 255.0f);            // G 0..255 (v^2)
	out[2] = (uint8_t)(int)(v * v * v * v * 128.0f);    // B 0..128 (v^4)
	out[3] = 255;                                       // A opaque
}

void lava_fx_init(struct lava_fx* w, uint32_t seed) {
	for(int i = 0; i < LAVA_FX_TEXELS; i++) {
		w->field[i] = 0.0f;
		w->field_next[i] = 0.0f;
		w->disp[i] = 0.0f;
		w->vel[i] = 0.0f;
	}
	w->prng = seed ? seed : 1u;
	w->k = 0;
	for(int i = 0; i < LAVA_FX_TEXELS; i++) {
		lava_fx_color(w->field[i], w->rgba_still + i * 4);
		lava_fx_color(w->field[i], w->rgba_flow + i * 4);
	}
}

void lava_fx_tick(struct lava_fx* w) {
	w->k++; // TextureLavaFlowFX.field_1139_k (drives the flow scroll)

	for(int var1 = 0; var1 < 16; var1++) {
		for(int var2 = 0; var2 < 16; var2++) {
			// Beta sin-distorts the sample centre by +-1 texel on each axis.
			int var4 = (int)(sinf((float)var2 * 3.1415927f * 2.0f / 16.0f) * 1.2f);
			int var5 = (int)(sinf((float)var1 * 3.1415927f * 2.0f / 16.0f) * 1.2f);

			float sum = 0.0f;
			for(int var6 = var1 - 1; var6 <= var1 + 1; var6++) {
				for(int var7 = var2 - 1; var7 <= var2 + 1; var7++) {
					int var8 = (var6 + var4) & 15;
					int var9 = (var7 + var5) & 15;
					sum += w->field[var8 + var9 * 16];
				}
			}

			w->field_next[var1 + var2 * 16] = sum / 10.0f
				+ (w->disp[(var1 & 15) + (var2 & 15) * 16]
				   + w->disp[((var1 + 1) & 15) + (var2 & 15) * 16]
				   + w->disp[((var1 + 1) & 15) + ((var2 + 1) & 15) * 16]
				   + w->disp[(var1 & 15) + ((var2 + 1) & 15) * 16])
					/ 4.0f * 0.8f;

			w->disp[var1 + var2 * 16] += w->vel[var1 + var2 * 16] * 0.01f;
			if(w->disp[var1 + var2 * 16] < 0.0f)
				w->disp[var1 + var2 * 16] = 0.0f;
			w->vel[var1 + var2 * 16] -= 0.06f;
			if(lfx_rand(w) < 0.005f)
				w->vel[var1 + var2 * 16] = 1.5f;
		}
	}

	// Beta swaps field_1146_h <-> field_1147_g; a copy is equivalent (field_next is
	// fully rewritten by PASS 1 each tick, so the old-field content is dead).
	for(int i = 0; i < LAVA_FX_TEXELS; i++)
		w->field[i] = w->field_next[i];

	// Colour both tiles from the shared field. The flow tile reads the field with
	// Beta's vertical scroll `(i - k/3*16) & 255` (TextureLavaFlowFX).
	int scroll = (int)((w->k / 3u) * 16u);
	for(int i = 0; i < LAVA_FX_TEXELS; i++) {
		lava_fx_color(w->field[i], w->rgba_still + i * 4);
		int fi = (i - scroll) & 255; // low 8 bits (0..255), matches Java's & 255
		lava_fx_color(w->field[fi], w->rgba_flow + i * 4);
	}
}

const uint8_t* lava_fx_pixels_still(const struct lava_fx* w) {
	return w->rgba_still;
}
float lava_fx_field_value(const struct lava_fx* w, int i) {
	return w->field[i];
}
const uint8_t* lava_fx_pixels_flow(const struct lava_fx* w) {
	return w->rgba_flow;
}
