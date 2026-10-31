/*
	Beta 1.7.3 TextureWatchFX port (see watch_fx.h).

	Byte-faithful to net.minecraft.src.TextureWatchFX.onTick (decompile 740c583)
	apart from the documented cosmetic-only deviations in watch_fx.h's header
	comment. Engine-free: pulls in only <math.h>/<stdint.h>/<stdbool.h>/<stddef.h>
	so tests/test_watch_fx.c links standalone.

	Tint citation [740c583, TextureWatchFX.onTick]:
	  r = (marker >> 16) & 255             (original marker R);
	  ...
	  saved_r = r                          (the original R, saved);
	  ...
	  r = dialR * r / 255;                  // RHS reads r BEFORE this
	                                         // assignment overwrites it, i.e.
	                                         // still the original R -- so this
	                                         // is numerically dialR*origR/255,
	                                         // the same origR that saved_r holds.
	  g = dialG * saved_r / 255;            // dialG*origR/255
	  b = dialB * saved_r / 255;            // dialB*origR/255
	All three channels therefore tint by the same origR/255 factor; this port
	names that factor `orig_r` once instead of replaying the r-then-saved_r
	shuffle.
*/

#include "watch_fx.h"

#include <math.h>
#include <stddef.h>

#define WATCH_FX_PI 3.14159265358979323846

void watch_fx_state_init(struct watch_fx_state* s) {
	s->current_angle = 0.0;
	s->angle_delta = 0.0;
}

void watch_fx_tick(struct watch_fx_state* s,
				   const uint8_t base_icon[WATCH_FX_BYTES],
				   const uint8_t* dial, bool has_target, bool is_nether,
				   double nether_rand01, double celestial_angle,
				   uint8_t out[WATCH_FX_BYTES]) {
	// Target angle [740c583: TextureWatchFX.onTick].
	double target = 0.0;
	if(has_target) {
		target = -celestial_angle * WATCH_FX_PI * 2.0;
		if(is_nether)
			target = nether_rand01 * WATCH_FX_PI * 2.0;
	}

	// Damped-spring smoothing (state persists across ticks; same shape as
	// compass_fx_tick, but a physically separate state struct/instance).
	double delta = target - s->current_angle;
	while(delta < -WATCH_FX_PI)
		delta += 2.0 * WATCH_FX_PI;
	while(delta >= WATCH_FX_PI)
		delta -= 2.0 * WATCH_FX_PI;
	if(delta < -1.0)
		delta = -1.0;
	if(delta > 1.0)
		delta = 1.0;

	s->angle_delta += delta * 0.1;
	s->angle_delta *= 0.8;
	s->current_angle += s->angle_delta;

	double sin_a = sin(s->current_angle);
	double cos_a = cos(s->current_angle);

	for(int i = 0; i < WATCH_FX_TEXELS; i++) {
		uint8_t r = base_icon[i * 4 + 0];
		uint8_t g = base_icon[i * 4 + 1];
		uint8_t b = base_icon[i * 4 + 2];
		uint8_t a = base_icon[i * 4 + 3];

		if(dial != NULL && r == b && g == 0 && b > 0) {
			double u = -((double)(i % WATCH_FX_DIM) / 15.0 - 0.5);
			double v = (double)(i / WATCH_FX_DIM) / 15.0 - 0.5;
			int orig_r = r;

			int ix = (int)((u * cos_a + v * sin_a + 0.5) * 16.0);
			int iy = (int)((v * cos_a - u * sin_a + 0.5) * 16.0);
			int dial_idx = (ix & 15) + (iy & 15) * WATCH_FX_DIM;

			uint8_t dr = dial[dial_idx * 4 + 0];
			uint8_t dg = dial[dial_idx * 4 + 1];
			uint8_t db = dial[dial_idx * 4 + 2];
			uint8_t da = dial[dial_idx * 4 + 3];

			r = (uint8_t)((int)dr * orig_r / 255);
			g = (uint8_t)((int)dg * orig_r / 255);
			b = (uint8_t)((int)db * orig_r / 255);
			a = da;
		}

		out[i * 4 + 0] = r;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = b;
		out[i * 4 + 3] = a;
	}
}
