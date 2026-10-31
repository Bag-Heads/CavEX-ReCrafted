/*
	Throwaway PC capture tool (NOT part of the build graph -- lives under tools/,
	compiled ad hoc) that drives the REAL production compass_fx_tick() with the
	REAL captured items.png compass icon, from two different player positions
	relative to spawn, and dumps each settled 16x16 frame as an upscaled PNG for
	visual proof. See PROJECT_MEMORY / the compass-clock lane report for how this
	was invoked; it is not wired into CMakeLists.txt on purpose (a one-off dev
	aid, like tools/gen_anim_fx's offline bakers).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/graphics/compass_fx.h"
#include "../source/lodepng/lodepng.h"

static void upscale_nn(const uint8_t* src16, uint8_t* dst, int scale) {
	int dim = 16 * scale;
	for(int y = 0; y < dim; y++) {
		for(int x = 0; x < dim; x++) {
			int sx = x / scale, sy = y / scale;
			memcpy(dst + (y * dim + x) * 4, src16 + (sy * 16 + sx) * 4, 4);
		}
	}
}

static void run_scenario(const char* label, const uint8_t icon[COMPASS_FX_BYTES],
						 double spawn_x, double spawn_z, double player_x,
						 double player_z, double yaw_rad,
						 const char* out_path) {
	struct compass_fx_state st;
	compass_fx_state_init(&st);
	uint8_t frame[COMPASS_FX_BYTES];

	// Settle the damped-spring filter (200 ticks @ 20Hz = 10s, far more than
	// enough for the 0.1/0.8 filter to converge from rest).
	for(int t = 0; t < 200; t++) {
		compass_fx_tick(&st, icon, true, false, 0.0, spawn_x, spawn_z, player_x,
						player_z, yaw_rad, frame);
	}

	printf("[%s] spawn=(%.1f,%.1f) player=(%.1f,%.1f) yaw_rad=%.4f -> "
		   "settled current_angle=%.4f rad (%.1f deg)\n",
		   label, spawn_x, spawn_z, player_x, player_z, yaw_rad,
		   st.current_angle, st.current_angle * 180.0 / 3.14159265358979323846);

	int scale = 16;
	int dim = 16 * scale;
	uint8_t* big = malloc((size_t)dim * dim * 4);
	upscale_nn(frame, big, scale);

	unsigned err = lodepng_encode32_file(out_path, big, dim, dim);
	if(err)
		fprintf(stderr, "encode error %u: %s\n", err, lodepng_error_text(err));
	else
		printf("wrote %s (%dx%d)\n", out_path, dim, dim);
	free(big);
}

int main(int argc, char** argv) {
	if(argc < 4) {
		fprintf(stderr, "usage: %s <items.png> <out_a.png> <out_b.png>\n",
				argv[0]);
		return 1;
	}

	unsigned char* items_rgba;
	unsigned iw, ih;
	unsigned err = lodepng_decode32_file(&items_rgba, &iw, &ih, argv[1]);
	if(err) {
		fprintf(stderr, "decode error %u: %s\n", err, lodepng_error_text(err));
		return 1;
	}
	if(iw != 256 || ih != 256) {
		fprintf(stderr, "unexpected items.png size %ux%u (want 256x256)\n", iw,
				ih);
		return 1;
	}

	// Capture the compass cell exactly like item_needle_fx_init's
	// item_needle_capture_cell (texture.c): icon (6,3) -> pixel origin (96,48).
	uint8_t icon[COMPASS_FX_BYTES];
	for(int y = 0; y < 16; y++) {
		for(int x = 0; x < 16; x++) {
			int sx = 96 + x, sy = 48 + y;
			memcpy(icon + (y * 16 + x) * 4, items_rgba + (sy * iw + sx) * 4, 4);
		}
	}
	free(items_rgba);

	// Scenario A: player due SOUTH of spawn (spawn is north), facing NORTH
	// (toward spawn) -- rotationYaw such that the needle should settle
	// pointing straight "up" the icon (toward spawn, i.e. toward the player's
	// own forward direction here). yaw_rad = PI/2 matches the hand-derived
	// golden in tests/test_compass_fx.c (player facing yaw=90 deg in this
	// engine's atan2(dz,dx) convention).
	run_scenario("A: player south of spawn, facing spawn", icon, 0.0, 0.0, 0.0,
				20.0, 3.14159265358979323846 / 2.0, argv[2]);

	// Scenario B: player far EAST of spawn, facing WEST (yaw=PI). Hand-check:
	// dx=spawn_x-player_x=-30, dz=0 -> atan2(0,-30)=PI (C's atan2(+0,neg)=+PI);
	// target = PI - PI/2 - PI = -PI/2, i.e. -90 deg -- exactly opposite
	// scenario A's +90 deg, so the two PNGs show visibly different needle
	// angles for a different (spawn, player, yaw) triple, not a coincidence.
	run_scenario("B: player east of spawn, facing west", icon, 0.0, 0.0, 30.0,
				0.0, 3.14159265358979323846, argv[3]);

	return 0;
}
