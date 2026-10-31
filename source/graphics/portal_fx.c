/*
	Beta 1.7.3 TexturePortalFX port -- see portal_fx.h for the algorithm notes.

	Exactness notes:
	  - The sparkle noise uses the project's java_random (bit-exact java.util.Random)
	    with Beta's FIXED seed 100, one nextFloat per texel in the exact draw order
	    (frame outer, x middle, y inner).
	  - The spiral sine goes through mh_sin (the 65536-entry MathHelper table --
	    NOT libm sinf), matching Beta's TextureFX math.
	  - atan2 is computed in double and cast to float, as Beta does
	    ((float)Math.atan2((double)v, (double)u)). libm atan2 is not formally
	    guaranteed bit-identical to Java's StrictMath-backed result, so the golden
	    is a SELF-golden (pins OUR bytes for regression); every other term is
	    draw-order and table exact. Render-layer only -- no sim parity at stake.

	This file is part of CaveWii_v2 (GPLv3, like the rest of source/).
*/

#include <math.h>
#include <stddef.h>

#include "portal_fx.h"
#include "../worldgen/beta_math.h"
#include "../worldgen/java_random.h"

void portal_fx_color(float intensity, uint8_t out[4]) {
	float i2 = intensity * intensity;
	out[0] = (uint8_t)(int)(i2 * 200.0f + 55.0f);				  // R
	out[1] = (uint8_t)(int)(i2 * i2 * 255.0f);					  // G
	out[2] = (uint8_t)(int)(intensity * 100.0f + 155.0f);		  // B
	out[3] = (uint8_t)(int)(intensity * 100.0f + 155.0f);		  // A
}

void portal_fx_generate(uint8_t* out) {
	/* static: 32 KB, offline bake only -- never on a Wii thread stack. */
	static float intensity[PORTAL_FX_FRAMES * PORTAL_FX_TEXELS];
	portal_fx_generate_intensity(intensity);
	for(size_t i = 0; i < PORTAL_FX_FRAMES * PORTAL_FX_TEXELS; i++)
		portal_fx_color(intensity[i], out + i * 4);
}

void portal_fx_generate_intensity(float* out) {
	beta_math_init(); // idempotent; mh_sin needs the table

	struct java_random rng;
	jrand_set_seed(&rng, 100); // Beta: new Random(100L) -- fixed, reproducible

	for(int f = 0; f < PORTAL_FX_FRAMES; f++) {
		for(int x = 0; x < PORTAL_FX_DIM; x++) {
			for(int y = 0; y < PORTAL_FX_DIM; y++) {
				float intensity = 0.0f;

				for(int l = 0; l < 2; l++) {
					// Wrap the layer-centred coords into [-1, 1).
					float cx = (float)(l * 8);
					float cy = (float)(l * 8);
					float u = ((float)x - cx) / 16.0f * 2.0f;
					float v = ((float)y - cy) / 16.0f * 2.0f;
					if(u < -1.0f)
						u += 2.0f;
					if(u >= 1.0f)
						u -= 2.0f;
					if(v < -1.0f)
						v += 2.0f;
					if(v >= 1.0f)
						v -= 2.0f;

					float r2 = u * u + v * v;
					// Layer 0 counter-rotates (factor -1), layer 1 rotates
					// (factor +1); -r2*10 twists the arms outward.
					float a = (float)atan2((double)v, (double)u)
						+ ((float)f / 32.0f * 3.1415927f * 2.0f - r2 * 10.0f
						   + (float)(l * 2))
							* (float)(l * 2 - 1);
					float s = (mh_sin(a) + 1.0f) / 2.0f;
					s /= r2 + 1.0f;
					intensity += s * 0.5f;
				}

				intensity += jrand_next_float(&rng) * 0.1f;

				out[(size_t)f * PORTAL_FX_TEXELS
					+ (size_t)(y * PORTAL_FX_DIM + x)] = intensity;
			}
		}
	}
}
