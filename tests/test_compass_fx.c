/*
	Golden/equivalence test for the Beta 1.7.3 TextureCompassFX port
	(source/graphics/compass_fx.c).

	Per CODESTYLE.md's poisoned-golden rule (4.2), the "expected" values below do
	NOT come from calling compass_fx_tick and trusting it -- they come from either
	(A) hand arithmetic performed independently against the decompile, choosing
	inputs where every intermediate lands far from an (int)-cast integer boundary
	so IEEE-754 rounding noise cannot flip the answer, or (B) `ref_compass_tick`,
	a SEPARATE straight-line re-transcription of TextureCompassFX.onTick [740c583]
	written fresh from the decompile text (Java variable names var9/var10/... kept
	literally, deliberately NOT sharing a single line with compass_fx.c) that this
	test drives side-by-side with the production port over many ticks and asserts
	byte-identical against, per the test_*_equiv recipe (CODESTYLE.md 4.1).
*/

#include "../source/graphics/compass_fx.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846

// ---------------------------------------------------------------------------
// (B) Independent reference port -- re-derived from the decompile text, not
// copied from source/graphics/compass_fx.c. Mirrors TextureCompassFX.onTick's
// own variable names/order so a reviewer can diff it against the Java 1:1.
// ---------------------------------------------------------------------------
struct ref_compass_state {
	double field_4229_i; // current angle
	double field_4228_j; // angle velocity
};

static void ref_compass_tick(struct ref_compass_state* st,
							 const uint8_t icon[COMPASS_FX_BYTES],
							 bool have_world_and_player, bool is_nether,
							 double nether_rand01, double spawn_x,
							 double spawn_z, double player_x, double player_z,
							 double rotation_yaw_rad,
							 uint8_t image_data[COMPASS_FX_BYTES]) {
	for(int i = 0; i < COMPASS_FX_BYTES; i++)
		image_data[i] = icon[i];

	double var20 = 0.0;
	if(have_world_and_player) {
		double var23 = spawn_x - player_x;
		double var25 = spawn_z - player_z;
		var20 = rotation_yaw_rad - PI / 2.0 - atan2(var25, var23);
		if(is_nether)
			var20 = nether_rand01 * PI * 2.0;
	}

	double var22 = var20 - st->field_4229_i;
	while(var22 < -PI)
		var22 += 2.0 * PI;
	while(var22 >= PI)
		var22 -= 2.0 * PI;
	if(var22 < -1.0)
		var22 = -1.0;
	if(var22 > 1.0)
		var22 = 1.0;

	st->field_4228_j += var22 * 0.1;
	st->field_4228_j *= 0.8;
	st->field_4229_i += st->field_4228_j;

	double var24 = sin(st->field_4229_i);
	double var26 = cos(st->field_4229_i);

	for(int var9 = -4; var9 <= 4; var9++) {
		int var10 = (int)(8.5 + var26 * (double)var9 * 0.3);
		int var11 = (int)(7.5 - var24 * (double)var9 * 0.3 * 0.5);
		if(var10 < 0)
			var10 = 0;
		if(var10 > 15)
			var10 = 15;
		if(var11 < 0)
			var11 = 0;
		if(var11 > 15)
			var11 = 15;
		int var12 = (var11 * 16 + var10) * 4;
		image_data[var12 + 0] = 100;
		image_data[var12 + 1] = 100;
		image_data[var12 + 2] = 100;
		image_data[var12 + 3] = 255;
	}

	for(int var9 = -8; var9 <= 16; var9++) {
		int var10 = (int)(8.5 + var24 * (double)var9 * 0.3);
		int var11 = (int)(7.5 + var26 * (double)var9 * 0.3 * 0.5);
		if(var10 < 0)
			var10 = 0;
		if(var10 > 15)
			var10 = 15;
		if(var11 < 0)
			var11 = 0;
		if(var11 > 15)
			var11 = 15;
		int var12 = (var11 * 16 + var10) * 4;
		if(var9 >= 0) {
			image_data[var12 + 0] = 255;
			image_data[var12 + 1] = 20;
			image_data[var12 + 2] = 20;
		} else {
			image_data[var12 + 0] = 100;
			image_data[var12 + 1] = 100;
			image_data[var12 + 2] = 100;
		}
		image_data[var12 + 3] = 255;
	}
}

// A synthetic "static icon": a distinguishable, non-flat pattern so a pixel
// that ISN'T touched by the needle can be told apart from one that is.
static void make_test_icon(uint8_t icon[COMPASS_FX_BYTES]) {
	for(int i = 0; i < COMPASS_FX_TEXELS; i++) {
		icon[i * 4 + 0] = (uint8_t)(i * 3);
		icon[i * 4 + 1] = (uint8_t)(i * 5);
		icon[i * 4 + 2] = (uint8_t)(i * 7);
		icon[i * 4 + 3] = 200;
	}
}

// ---------------------------------------------------------------------------
// (A) Hand-derived pinned single-tick values. Chosen so sin/cos land on the
// EXACT IEEE-754 values 0.0/1.0 (yaw - spawn/player all zero-delta -> angle
// stays at the rest state 0.0 the very first tick) and every needle pixel
// coordinate below is hand-verified to be at least 0.3 away from the nearest
// integer boundary, so no floating-point rounding direction can move it.
// ---------------------------------------------------------------------------
static void test_hand_derived_rest_frame(void) {
	uint8_t icon[COMPASS_FX_BYTES];
	make_test_icon(icon);

	struct compass_fx_state s;
	compass_fx_state_init(&s);

	uint8_t out[COMPASS_FX_BYTES];
	// spawn == player (dx=dz=0, atan2(0,0)==0 exactly) and rotation_yaw_rad ==
	// PI/2 -> target = PI/2 - PI/2 - 0 = 0.0 exactly == the initial state, so
	// current_angle stays EXACTLY 0.0 (delta==0 -> angle_delta stays 0 ->
	// current_angle += 0). sin(0)=0.0, cos(0)=1.0, both IEEE-exact.
	compass_fx_tick(&s, icon, true, false, 0.0, 0.0, 0.0, 0.0, 0.0, PI / 2.0,
					out);

	assert(s.current_angle == 0.0);
	assert(s.angle_delta == 0.0);

	// Tail loop (gray): x=(int)(8.5+1*v9*0.3), y=(int)7.5=7 for every v9.
	// v9=-4 -> x=(int)7.3=7 (0.3 clear of both 7 and 8).
	assert(out[(7 * 16 + 7) * 4 + 0] == 100
		   && out[(7 * 16 + 7) * 4 + 1] == 100
		   && out[(7 * 16 + 7) * 4 + 2] == 100
		   && out[(7 * 16 + 7) * 4 + 3] == 255);
	// v9=2 -> x=(int)9.1=9.
	assert(out[(7 * 16 + 9) * 4 + 0] == 100);

	// Head loop: x=(int)8.5=8 for every v9 (sin=0). y=(int)(7.5+0.15*v9).
	// v9=-8 -> y=(int)6.3=6, gray.
	assert(out[(6 * 16 + 8) * 4 + 0] == 100
		   && out[(6 * 16 + 8) * 4 + 1] == 100
		   && out[(6 * 16 + 8) * 4 + 2] == 100);
	// (7,8) is written by SEVEN head-loop iterations (v9 in -3..3 all give
	// y=(int)(7.5+0.15*v9)==7); the loop runs v9 ascending, so the LAST write
	// wins, which is v9=3 (still >=0 -> RED) -- every v9 in {0,1,2,3} agrees on
	// RED anyway, so the final colour is unambiguously RED regardless of which
	// of those four iterations is treated as "the" writer.
	assert(out[(7 * 16 + 8) * 4 + 0] == 255
		   && out[(7 * 16 + 8) * 4 + 1] == 20
		   && out[(7 * 16 + 8) * 4 + 2] == 20
		   && out[(7 * 16 + 8) * 4 + 3] == 255);

	// A texel nowhere near the needle keeps the untouched icon colour.
	int untouched = 0 * 16 + 0; // (x=0,y=0)
	assert(out[untouched * 4 + 0] == icon[untouched * 4 + 0]);
	assert(out[untouched * 4 + 1] == icon[untouched * 4 + 1]);
	assert(out[untouched * 4 + 2] == icon[untouched * 4 + 2]);
	assert(out[untouched * 4 + 3] == icon[untouched * 4 + 3]);

	printf("test_compass_fx: hand-derived rest frame PASS\n");
}

static void test_no_target_pins_zero(void) {
	uint8_t icon[COMPASS_FX_BYTES];
	make_test_icon(icon);
	struct compass_fx_state s;
	compass_fx_state_init(&s);
	uint8_t out[COMPASS_FX_BYTES];

	// has_target=false must behave exactly like target==0.0 regardless of the
	// (garbage) position/yaw arguments passed alongside it.
	compass_fx_tick(&s, icon, false, false, 0.0, 999.0, -999.0, 5.0, 5.0, 3.5,
					out);
	assert(s.current_angle == 0.0 && s.angle_delta == 0.0);

	printf("test_compass_fx: no-target-pins-zero PASS\n");
}

// (C) Equivalence sweep: production vs. the independent reference port, over
// many ticks, a moving player/spawn, changing yaw, and both nether states.
static void test_equiv_sweep(void) {
	uint8_t icon[COMPASS_FX_BYTES];
	make_test_icon(icon);

	struct compass_fx_state prod_state;
	struct ref_compass_state ref_state = {0.0, 0.0};
	compass_fx_state_init(&prod_state);

	for(int t = 0; t < 2000; t++) {
		bool has_target = (t > 5); // a few ticks with no world yet, like boot
		bool is_nether = (t > 900 && t < 1200);
		// A cheap deterministic "random" source shared by both sides -- this
		// is testing the FORMULA, not any particular RNG.
		double nether_rand = fmod((double)(t * 2654435761u % 100000u) / 100000.0,
								  1.0);
		double spawn_x = 12.0, spawn_z = -30.0;
		double player_x = 4.0 * sin((double)t * 0.013);
		double player_z = 4.0 * cos((double)t * 0.017) - 2.0;
		double yaw = fmod((double)t * 0.041, 2.0 * PI);

		uint8_t prod_out[COMPASS_FX_BYTES];
		uint8_t ref_out[COMPASS_FX_BYTES];

		compass_fx_tick(&prod_state, icon, has_target, is_nether, nether_rand,
						spawn_x, spawn_z, player_x, player_z, yaw, prod_out);
		ref_compass_tick(&ref_state, icon, has_target, is_nether, nether_rand,
						 spawn_x, spawn_z, player_x, player_z, yaw, ref_out);

		if(memcmp(prod_out, ref_out, COMPASS_FX_BYTES) != 0) {
			fprintf(stderr, "test_compass_fx: MISMATCH at tick %d\n", t);
			for(int i = 0; i < COMPASS_FX_TEXELS; i++) {
				if(memcmp(prod_out + i * 4, ref_out + i * 4, 4) != 0) {
					fprintf(stderr,
							"  texel %d prod=(%d,%d,%d,%d) ref=(%d,%d,%d,%d)\n",
							i, prod_out[i * 4], prod_out[i * 4 + 1],
							prod_out[i * 4 + 2], prod_out[i * 4 + 3],
							ref_out[i * 4], ref_out[i * 4 + 1],
							ref_out[i * 4 + 2], ref_out[i * 4 + 3]);
				}
			}
			assert(0 && "production vs. independent reference diverged");
		}

		// Cross-check the smoothing state itself stays bit-identical too --
		// otherwise a divergence could hide until a later tick's rendering
		// happens to coincide again.
		assert(prod_state.current_angle == ref_state.field_4229_i);
		assert(prod_state.angle_delta == ref_state.field_4228_j);
	}

	printf("test_compass_fx: equivalence sweep (2000 ticks) PASS\n");
}

// Determinism: identical inputs from a fresh state must produce identical
// output byte-for-byte (regression guard on the pure-function contract).
static void test_determinism(void) {
	uint8_t icon[COMPASS_FX_BYTES];
	make_test_icon(icon);

	struct compass_fx_state a, b;
	compass_fx_state_init(&a);
	compass_fx_state_init(&b);
	uint8_t out_a[COMPASS_FX_BYTES], out_b[COMPASS_FX_BYTES];

	for(int t = 0; t < 50; t++) {
		compass_fx_tick(&a, icon, true, false, 0.0, 1.0, 2.0, (double)t * 0.1,
						0.0, 0.7, out_a);
		compass_fx_tick(&b, icon, true, false, 0.0, 1.0, 2.0, (double)t * 0.1,
						0.0, 0.7, out_b);
	}
	assert(memcmp(out_a, out_b, COMPASS_FX_BYTES) == 0);

	printf("test_compass_fx: determinism PASS\n");
}

int main(void) {
	test_hand_derived_rest_frame();
	test_no_target_pins_zero();
	test_equiv_sweep();
	test_determinism();
	printf("test_compass_fx: PASS\n");
	return 0;
}
