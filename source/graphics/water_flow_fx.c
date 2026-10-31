/*
	Beta 1.7.3 TextureWaterFlowFX port -- pure ripple-field sim (see the header).

	ALGORITHM-faithful to net.minecraft.src.TextureWaterFlowFX.onTick (decompile
	740c583) -- deliberately NOT claiming byte-identity, and it could not have
	it: the RNG is a documented non-Beta LCG (below), and PASS 2 walks the 256
	texels in flat index order where Beta's nested loops walk them column-major
	(`var1` outer, index `var1 + var2*16`). Both differences only change WHICH
	texel receives a given random kick, never the value range or the physics, so
	the tile is visually equivalent -- but it is not the same bytes Beta would
	produce, and nothing here should be read as promising that. Same convention
	as water_fx.c, which this mirrors. Engine-free so the golden links it
	standalone.

	The FIVE deltas from TextureWaterFX (still water), each verified against the
	two decompiled sources side by side:

	  1. blur axis   still: dx in [x-1, x+1]  (horizontal, CENTRED)
	                 flow:  dy in [y-2, y]    (vertical, TRAILING)
	  2. divisor     3.3  ->  3.2
	  3. vel decay   0.1  ->  0.3
	  4. kick chance 0.05 ->  0.2
	  5. colour tap  field[i]  ->  field[(i - ticks*16) & 255]

	Delta 1 is why flowing water reads as downward streaks rather than isotropic
	ripples; delta 5 is the scroll -- 16 is one row, so the tile marches one row
	per tick. The colour mapping itself is IDENTICAL in both classes (verbatim
	the same eight lines), so this reuses water_fx_color rather than restating
	it -- the blue channel is pinned to 255 in BOTH.
*/

#include "water_flow_fx.h"

// Same self-contained LCG rationale as water_fx.c: Beta uses Math.random() here,
// the FX layer is render-only and never parity-checked, and a fixed seed keeps
// the golden deterministic. Documented non-Beta stream, not a faithfulness claim.
static inline float wffx_rand(struct water_flow_fx* w) {
	w->prng = w->prng * 1664525u + 1013904223u;
	return (float)(w->prng >> 8) / (float)(1u << 24); // [0, 1)
}

void water_flow_fx_init(struct water_flow_fx* w, uint32_t seed) {
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		w->field[i] = 0.0f;
		w->field_next[i] = 0.0f;
		w->disp[i] = 0.0f;
		w->vel[i] = 0.0f;
	}
	w->ticks = 0;
	w->prng = seed ? seed : 1u;
	// Resting tile: a flat 0 field colours to (32,50,255,146) everywhere.
	for(int i = 0; i < WATER_FX_TEXELS; i++)
		water_fx_color(w->field[i], w->rgba + i * 4);
}

void water_flow_fx_tick(struct water_flow_fx* w) {
	w->ticks++; // Beta: ++field_1134_k, FIRST statement of onTick

	// PASS 1: vertical 3-tap TRAILING blur (y-2..y, wrapping on the 16-torus),
	// /3.2 damping, plus 0.8 * accumulated displacement.
	for(int x = 0; x < WATER_FX_DIM; x++) {
		for(int y = 0; y < WATER_FX_DIM; y++) {
			float s = 0.0f;
			for(int dy = y - 2; dy <= y; dy++)
				s += w->field[(x & 15) + (dy & 15) * 16];
			w->field_next[x + y * 16] = s / 3.2f + w->disp[x + y * 16] * 0.8f;
		}
	}
	// PASS 2: integrate velocity into displacement (floored >=0), constant decay,
	// 20% chance/texel to kick velocity to 0.5 (overwrite, not add).
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		w->disp[i] += w->vel[i] * 0.05f;
		if(w->disp[i] < 0.0f)
			w->disp[i] = 0.0f;
		w->vel[i] -= 0.3f;
		if(wffx_rand(w) < 0.2f)
			w->vel[i] = 0.5f;
	}
	// PASS 3: ping-pong the two HEIGHT buffers only (disp/vel persist).
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		float t = w->field[i];
		w->field[i] = w->field_next[i];
		w->field_next[i] = t;
	}
	// PASS 4: colour through the SCROLLED tap. Beta indexes
	// field_1138_g[var2 - field_1134_k * 16 & 255]; Java's `-` binds tighter
	// than `&`, so the subtraction happens first and the mask wraps it.
	for(int i = 0; i < WATER_FX_TEXELS; i++) {
		uint32_t src = ((uint32_t)i - w->ticks * 16u) & 255u;
		water_fx_color(w->field[src], w->rgba + i * 4);
	}
}

const uint8_t* water_flow_fx_pixels(const struct water_flow_fx* w) {
	return w->rgba;
}
float water_flow_fx_field_value(const struct water_flow_fx* w, int i) {
	uint32_t src = ((uint32_t)i - w->ticks * 16u) & 255u;
	return w->field[src];
}
