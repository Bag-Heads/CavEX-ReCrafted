/*
	Beta 1.7.3 TextureFlamesFX port (see flame_fx.h).

	Byte-faithful to net.minecraft.src.TextureFlamesFX.onTick (decompile 740c583).
	Engine-free: pulls in only <stdint.h> so the golden test (tests/test_flame_fx.c)
	links standalone.
*/

#include "flame_fx.h"

// Self-contained LCG (same rationale as water_fx/lava_fx: render-layer, non-parity).
// Returned as a DOUBLE like Math.random(); the value is a 24-bit fraction, so it
// is the same number the old float return gave (exact in both types).
static inline double ffx_rand(struct flame_fx* w) {
	w->prng = w->prng * 1664525u + 1013904223u;
	return (double)(w->prng >> 8) / (double)(1u << 24); // [0, 1)
}

void flame_fx_color(float field_value, uint8_t out[4]) {
	float v = field_value * 1.8f; // TextureFlamesFX: var13 = field * 1.8
	if(v > 1.0f)
		v = 1.0f;
	if(v < 0.0f)
		v = 0.0f;
	// B: TextureFlamesFX.java:54 multiplies var13 ten times LEFT TO RIGHT, each
	// product rounded to float. The earlier grouped ((v^2)^2)^2 * v^2 rounds
	// differently for 19 field floats in [0, 1/1.8] (B off by one); the chain
	// below is Java's exact evaluation order (fire-anim-0924/F1).
	float v10 = v * v * v * v * v * v * v * v * v * v;
	out[0] = (uint8_t)(int)(v * 155.0f + 100.0f); // R 100..255
	out[1] = (uint8_t)(int)(v * v * 255.0f);      // G 0..255 (v^2)
	out[2] = (uint8_t)(int)(v10 * 255.0f);        // B 0..255 (v^10, only near white-hot)
	out[3] = (v < 0.5f) ? 0 : 255;                // hard alpha cutoff at 0.5
}

void flame_fx_init(struct flame_fx* w, uint32_t seed) {
	for(int i = 0; i < FLAME_FX_FIELD; i++) {
		w->field[i] = 0.0f;
		w->field_next[i] = 0.0f;
	}
	w->prng = seed ? seed : 1u;
	for(int i = 0; i < FLAME_FX_TEXELS; i++)
		flame_fx_color(w->field[i], w->rgba + i * 4);
}

void flame_fx_tick(struct flame_fx* w) {
	for(int var1 = 0; var1 < 16; var1++) {
		for(int var2 = 0; var2 < 20; var2++) {
			int var3 = 18;
			// The cell one row up (wrapping y over 20), weighted 18.
			float var4 = w->field[var1 + (var2 + 1) % 20 * 16] * (float)var3;

			for(int var5 = var1 - 1; var5 <= var1 + 1; var5++) {
				for(int var6 = var2; var6 <= var2 + 1; var6++) {
					if(var5 >= 0 && var6 >= 0 && var5 < 16 && var6 < 20)
						var4 += w->field[var5 + var6 * 16];
					var3++; // increments for every iteration (in-bounds or not)
				}
			}

			w->field_next[var1 + var2 * 16] = var4 / ((float)var3 * 1.06f);

			if(var2 >= 19) {
				// Bottom row = random source (4 ordered draws, Java left-to-right),
				// evaluated in DOUBLE and cast once, exactly as
				// TextureFlamesFX.java:33: (float)(Math.random()*Math.random()*
				// Math.random()*4.0D + Math.random()*(double)0.1F + (double)0.2F).
				double r1 = ffx_rand(w);
				double r2 = ffx_rand(w);
				double r3 = ffx_rand(w);
				double r4 = ffx_rand(w);
				w->field_next[var1 + var2 * 16] = (float)(r1 * r2 * r3 * 4.0
					+ r4 * (double)0.1F + (double)0.2F);
			}
		}
	}

	// Beta swaps field_1132_h <-> field_1133_g; a copy is equivalent (field_next is
	// fully rewritten each tick).
	for(int i = 0; i < FLAME_FX_FIELD; i++)
		w->field[i] = w->field_next[i];

	// Colour the visible top 16x16 (the first 256 texels of the 16x20 field).
	for(int i = 0; i < FLAME_FX_TEXELS; i++)
		flame_fx_color(w->field[i], w->rgba + i * 4);
}

const uint8_t* flame_fx_pixels(const struct flame_fx* w) {
	return w->rgba;
}
float flame_fx_field_value(const struct flame_fx* w, int i) {
	return w->field[i];
}

// --- Beta's two instances (flame_fx.h) --------------------------------------
// The pair only threads the ONE stream through each instance's prng field in
// Beta's order. Consequence: right after flame_fx_pair_init (the registration
// tick) instance 0 equals a lone flame_fx with the same seed ticked once; from
// the next tick on it does NOT, because instance 1 has consumed 64 draws of the
// shared stream in between.
void flame_fx_pair_tick(struct flame_fx_pair* p) {
	p->fx[0].prng = p->prng;
	flame_fx_tick(&p->fx[0]);
	p->fx[1].prng = p->fx[0].prng;
	flame_fx_tick(&p->fx[1]);
	p->prng = p->fx[1].prng;
}

void flame_fx_pair_init(struct flame_fx_pair* p, uint32_t seed) {
	flame_fx_init(&p->fx[0], seed);
	flame_fx_init(&p->fx[1], seed);
	p->prng = p->fx[0].prng; // same 0 -> 1 remap as flame_fx_init
	// RenderEngine.registerTextureFX: `textureList.add(fx); fx.onTick();` for
	// FlamesFX(0) then FlamesFX(1) -- nothing draws Math.random() in between.
	flame_fx_pair_tick(p);
}

const uint8_t* flame_fx_pair_pixels(const struct flame_fx_pair* p, int index) {
	return flame_fx_pixels(&p->fx[index ? 1 : 0]);
}
float flame_fx_pair_field_value(const struct flame_fx_pair* p, int index, int i) {
	return flame_fx_field_value(&p->fx[index ? 1 : 0], i);
}
