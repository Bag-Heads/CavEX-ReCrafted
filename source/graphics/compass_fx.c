/*
	Beta 1.7.3 TextureCompassFX port (see compass_fx.h).

	Byte-faithful to net.minecraft.src.TextureCompassFX.onTick (decompile 740c583)
	apart from the two documented, cosmetic-only deviations in compass_fx.h's
	header comment (nether RNG source, and a single PI literal). Engine-free:
	pulls in only <math.h>/<stdint.h>/<stdbool.h> so the golden/equiv tests
	(tests/test_compass_fx.c) link standalone.
*/

#include "compass_fx.h"

#include <math.h>

#define COMPASS_FX_PI 3.14159265358979323846

void compass_fx_state_init(struct compass_fx_state* s) {
	s->current_angle = 0.0;
	s->angle_delta = 0.0;
}

// Beta computes idx=var11*16+var10 with no bounds check at all (an OOB write would
// throw ArrayIndexOutOfBoundsException in Java); the needle geometry above keeps
// x/y within [3,13]x[3,13] for every real angle, so this clamp never actually
// fires -- it exists only so a C buffer overrun can't happen if that geometric
// guarantee is ever wrong, and is provably a no-op today (see
// tests/test_compass_fx.c's in-bounds sweep).
static void compass_fx_put(uint8_t out[COMPASS_FX_BYTES], int x, int y,
						   uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	if(x < 0)
		x = 0;
	if(x > 15)
		x = 15;
	if(y < 0)
		y = 0;
	if(y > 15)
		y = 15;
	int idx = (y * COMPASS_FX_DIM + x) * 4;
	out[idx + 0] = r;
	out[idx + 1] = g;
	out[idx + 2] = b;
	out[idx + 3] = a;
}

void compass_fx_tick(struct compass_fx_state* s,
					 const uint8_t base_icon[COMPASS_FX_BYTES], bool has_target,
					 bool is_nether, double nether_rand01, double spawn_x,
					 double spawn_z, double player_x, double player_z,
					 double player_yaw_rad, uint8_t out[COMPASS_FX_BYTES]) {
	// Step 1: verbatim copy of the static icon (do this first so `out == base_icon`
	// aliasing is safe -- the needle pixels below overwrite a subset of it).
	if(out != base_icon) {
		for(int i = 0; i < COMPASS_FX_BYTES; i++)
			out[i] = base_icon[i];
	}

	// Step 2: target angle [740c583: TextureCompassFX.onTick].
	double target = 0.0;
	if(has_target) {
		double dx = spawn_x - player_x;
		double dz = spawn_z - player_z;
		target = player_yaw_rad - (COMPASS_FX_PI / 2.0) - atan2(dz, dx);
		if(is_nether)
			target = nether_rand01 * COMPASS_FX_PI * 2.0;
	}

	// Step 3: damped-spring smoothing (state persists across ticks).
	double delta = target - s->current_angle;
	while(delta < -COMPASS_FX_PI)
		delta += 2.0 * COMPASS_FX_PI;
	while(delta >= COMPASS_FX_PI)
		delta -= 2.0 * COMPASS_FX_PI;
	if(delta < -1.0)
		delta = -1.0;
	if(delta > 1.0)
		delta = 1.0;

	s->angle_delta += delta * 0.1;
	s->angle_delta *= 0.8;
	s->current_angle += s->angle_delta;

	double sin_a = sin(s->current_angle);
	double cos_a = cos(s->current_angle);

	// Step 4a: tail stroke, gray, var9 in [-4, 4].
	for(int v9 = -4; v9 <= 4; v9++) {
		int x = (int)(8.5 + cos_a * (double)v9 * 0.3);
		int y = (int)(7.5 - sin_a * (double)v9 * 0.3 * 0.5);
		compass_fx_put(out, x, y, 100, 100, 100, 255);
	}

	// Step 4b: head stroke, var9 in [-8, 16]; red half is the north tip (var9>=0).
	for(int v9 = -8; v9 <= 16; v9++) {
		int x = (int)(8.5 + sin_a * (double)v9 * 0.3);
		int y = (int)(7.5 + cos_a * (double)v9 * 0.3 * 0.5);
		if(v9 >= 0)
			compass_fx_put(out, x, y, 255, 20, 20, 255);
		else
			compass_fx_put(out, x, y, 100, 100, 100, 255);
	}
}
