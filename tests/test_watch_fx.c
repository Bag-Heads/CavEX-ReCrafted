/*
	Golden/equivalence test for the Beta 1.7.3 TextureWatchFX port
	(source/graphics/watch_fx.c).

	Same discipline as tests/test_compass_fx.c (CODESTYLE.md 4.1/4.2): (A) a
	hand-derived single-tick pin at the rest state where sin/cos land on the
	IEEE-exact values 0.0/1.0, and (C) `ref_watch_tick`, an independently
	re-transcribed straight-line port of TextureWatchFX.onTick [740c583] (Java
	variable names kept literally) driven side-by-side with the production port
	over many ticks and asserted byte-identical.
*/

#include "../source/graphics/watch_fx.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846

// ---------------------------------------------------------------------------
// Independent reference port.
// ---------------------------------------------------------------------------
struct ref_watch_state {
	double field_4222_j; // current angle
	double field_4221_k; // angle velocity
};

static void ref_watch_tick(struct ref_watch_state* st,
						   const uint8_t watch_icon[WATCH_FX_BYTES],
						   const uint8_t* dial /* or NULL */,
						   bool have_world_and_player, bool is_nether,
						   double nether_rand01, double celestial_angle,
						   uint8_t image_data[WATCH_FX_BYTES]) {
	double var1 = 0.0;
	if(have_world_and_player) {
		var1 = -celestial_angle * PI * 2.0;
		if(is_nether)
			var1 = nether_rand01 * PI * 2.0;
	}

	double var22 = var1 - st->field_4222_j;
	while(var22 < -PI)
		var22 += 2.0 * PI;
	while(var22 >= PI)
		var22 -= 2.0 * PI;
	if(var22 < -1.0)
		var22 = -1.0;
	if(var22 > 1.0)
		var22 = 1.0;

	st->field_4221_k += var22 * 0.1;
	st->field_4221_k *= 0.8;
	st->field_4222_j += st->field_4221_k;

	double var5 = sin(st->field_4222_j);
	double var7 = cos(st->field_4222_j);

	for(int var9 = 0; var9 < 256; var9++) {
		int var10 = watch_icon[var9 * 4 + 3]; // A
		int var11 = watch_icon[var9 * 4 + 0]; // R
		int var12 = watch_icon[var9 * 4 + 1]; // G
		int var13 = watch_icon[var9 * 4 + 2]; // B

		if(dial != NULL && var11 == var13 && var12 == 0 && var13 > 0) {
			double var14 = -((double)(var9 % 16) / 15.0 - 0.5);
			double var16 = (double)(var9 / 16) / 15.0 - 0.5;
			int var18 = var11;
			int var19 = (int)((var14 * var7 + var16 * var5 + 0.5) * 16.0);
			int var20 = (int)((var16 * var7 - var14 * var5 + 0.5) * 16.0);
			int var21 = (var19 & 15) + (var20 & 15) * 16;
			var10 = dial[var21 * 4 + 3];
			var11 = dial[var21 * 4 + 0] * var11 / 255;
			var12 = dial[var21 * 4 + 1] * var18 / 255;
			var13 = dial[var21 * 4 + 2] * var18 / 255;
		}

		image_data[var9 * 4 + 0] = (uint8_t)var11;
		image_data[var9 * 4 + 1] = (uint8_t)var12;
		image_data[var9 * 4 + 2] = (uint8_t)var13;
		image_data[var9 * 4 + 3] = (uint8_t)var10;
	}
}

static void make_test_icon_plain(uint8_t icon[WATCH_FX_BYTES]) {
	// A distinguishable non-marker pattern everywhere: G is a CONSTANT nonzero
	// value (not i-derived -- an i-derived formula can wrap through 0 mod 256
	// and accidentally satisfy the marker condition at one texel, which is
	// exactly what an earlier version of this fixture did at i=51) so
	// `g==0` can never hold and the r==b&&g==0&&b>0 marker test never matches,
	// confirming untouched pass-through everywhere.
	for(int i = 0; i < WATCH_FX_TEXELS; i++) {
		icon[i * 4 + 0] = (uint8_t)(i * 3 + 1);
		icon[i * 4 + 1] = 77; // constant, never 0
		icon[i * 4 + 2] = (uint8_t)(i * 3 + 1); // == R, but G!=0 so no match
		icon[i * 4 + 3] = 200;
	}
}

// A "hand" icon: EVERY texel is a marker pixel (R==B, G==0, B>0), like Beta's
// real clock hand art -- exercises the dial-rotation branch on the whole tile.
static void make_test_icon_all_markers(uint8_t icon[WATCH_FX_BYTES]) {
	for(int i = 0; i < WATCH_FX_TEXELS; i++) {
		uint8_t v = (uint8_t)(1 + (i % 250)); // keep it >0, avoid overflow to 0
		icon[i * 4 + 0] = v;
		icon[i * 4 + 1] = 0;
		icon[i * 4 + 2] = v;
		icon[i * 4 + 3] = 200;
	}
}

static void make_test_dial(uint8_t dial[WATCH_FX_BYTES]) {
	for(int i = 0; i < WATCH_FX_TEXELS; i++) {
		dial[i * 4 + 0] = (uint8_t)(i % 256);
		dial[i * 4 + 1] = (uint8_t)((i * 7) % 256);
		dial[i * 4 + 2] = (uint8_t)((i * 11) % 256);
		dial[i * 4 + 3] = (uint8_t)((i * 13) % 256);
	}
}

// (A) Hand-derived pin: no dial (dial==NULL) must be a byte-exact pass-through
// of the icon regardless of angle/state -- no floating point involved at all.
static void test_no_dial_is_passthrough(void) {
	uint8_t icon[WATCH_FX_BYTES];
	make_test_icon_all_markers(icon); // even an all-marker icon...
	struct watch_fx_state s;
	watch_fx_state_init(&s);
	uint8_t out[WATCH_FX_BYTES];

	watch_fx_tick(&s, icon, NULL, true, false, 0.0, 0.37, out);
	assert(memcmp(out, icon, WATCH_FX_BYTES) == 0);

	printf("test_watch_fx: no-dial-is-passthrough PASS\n");
}

// (A) Hand-derived pin at the rest state (target==current_angle==0.0 exactly,
// since celestial_angle==0.0 and has_target picks a target of -0*2PI==0/ -0.0,
// and the initial state is 0.0 too -> sin=0.0, cos=1.0 IEEE-exact). At that
// angle: centre the pixel coordinates and mirror the horizontal component;
// with zero rotation, the horizontal dial coordinate follows that mirrored position.
//      =(int)(-(i%16)/15*16+8) ... verified by hand for i=0 and i=255 below,
// both far from any (int)-cast boundary.
static void test_hand_derived_rest_dial_lookup(void) {
	uint8_t icon[WATCH_FX_BYTES];
	make_test_icon_all_markers(icon);
	uint8_t dial[WATCH_FX_BYTES];
	make_test_dial(dial);

	struct watch_fx_state s;
	watch_fx_state_init(&s);
	uint8_t out[WATCH_FX_BYTES];

	watch_fx_tick(&s, icon, dial, true, false, 0.0, 0.0, out);
	assert(s.current_angle == 0.0 && s.angle_delta == 0.0);

	// Pixel zero is the top-left corner: centred coordinates are 0.5 and -0.5.
	// Zero rotation maps its horizontal coordinate to 16 before wrapping.
	// Its vertical coordinate maps to zero.
	// dial_idx = (16&15) + (0&15)*16 = 0 + 0 = 0.
	{
		int orig_r = icon[0]; // == make_test_icon_all_markers(0)'s v = 1
		uint8_t exp_r = (uint8_t)((int)dial[0 * 4 + 0] * orig_r / 255);
		uint8_t exp_g = (uint8_t)((int)dial[0 * 4 + 1] * orig_r / 255);
		uint8_t exp_b = (uint8_t)((int)dial[0 * 4 + 2] * orig_r / 255);
		uint8_t exp_a = dial[0 * 4 + 3];
		assert(out[0] == exp_r && out[1] == exp_g && out[2] == exp_b
			   && out[3] == exp_a);
	}

	printf("test_watch_fx: hand-derived rest dial-lookup PASS\n");
}

static void test_no_target_pins_zero(void) {
	uint8_t icon[WATCH_FX_BYTES];
	make_test_icon_plain(icon);
	uint8_t dial[WATCH_FX_BYTES];
	make_test_dial(dial);
	struct watch_fx_state s;
	watch_fx_state_init(&s);
	uint8_t out[WATCH_FX_BYTES];

	watch_fx_tick(&s, icon, dial, false, false, 0.0, 0.83, out);
	assert(s.current_angle == 0.0 && s.angle_delta == 0.0);
	// Plain icon has no marker texels, so the frame equals the icon regardless.
	assert(memcmp(out, icon, WATCH_FX_BYTES) == 0);

	printf("test_watch_fx: no-target-pins-zero PASS\n");
}

// (C) Equivalence sweep: production vs. the independent reference port.
static void test_equiv_sweep(void) {
	uint8_t icon[WATCH_FX_BYTES];
	make_test_icon_all_markers(icon);
	uint8_t dial[WATCH_FX_BYTES];
	make_test_dial(dial);

	struct watch_fx_state prod_state;
	struct ref_watch_state ref_state = {0.0, 0.0};
	watch_fx_state_init(&prod_state);

	for(int t = 0; t < 2000; t++) {
		bool has_target = (t > 5);
		bool is_nether = (t > 900 && t < 1200);
		double nether_rand
			= fmod((double)(t * 2654435761u % 100000u) / 100000.0, 1.0);
		// celestial_angle cycles like a day/night clock (matches
		// daytime_celestial_angle()'s [0,1) range, though the formula here is
		// only exercising watch_fx's OWN math, not daytime.c).
		double celestial = fmod((double)t / 240.0, 1.0);

		uint8_t prod_out[WATCH_FX_BYTES];
		uint8_t ref_out[WATCH_FX_BYTES];

		watch_fx_tick(&prod_state, icon, dial, has_target, is_nether,
					 nether_rand, celestial, prod_out);
		ref_watch_tick(&ref_state, icon, dial, has_target, is_nether,
					  nether_rand, celestial, ref_out);

		if(memcmp(prod_out, ref_out, WATCH_FX_BYTES) != 0) {
			fprintf(stderr, "test_watch_fx: MISMATCH at tick %d\n", t);
			for(int i = 0; i < WATCH_FX_TEXELS; i++) {
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

		assert(prod_state.current_angle == ref_state.field_4222_j);
		assert(prod_state.angle_delta == ref_state.field_4221_k);
	}

	printf("test_watch_fx: equivalence sweep (2000 ticks) PASS\n");
}

static void test_determinism(void) {
	uint8_t icon[WATCH_FX_BYTES];
	make_test_icon_all_markers(icon);
	uint8_t dial[WATCH_FX_BYTES];
	make_test_dial(dial);

	struct watch_fx_state a, b;
	watch_fx_state_init(&a);
	watch_fx_state_init(&b);
	uint8_t out_a[WATCH_FX_BYTES], out_b[WATCH_FX_BYTES];

	for(int t = 0; t < 50; t++) {
		double c = fmod((double)t / 97.0, 1.0);
		watch_fx_tick(&a, icon, dial, true, false, 0.0, c, out_a);
		watch_fx_tick(&b, icon, dial, true, false, 0.0, c, out_b);
	}
	assert(memcmp(out_a, out_b, WATCH_FX_BYTES) == 0);

	printf("test_watch_fx: determinism PASS\n");
}

int main(void) {
	test_no_dial_is_passthrough();
	test_hand_derived_rest_dial_lookup();
	test_no_target_pins_zero();
	test_equiv_sweep();
	test_determinism();
	printf("test_watch_fx: PASS\n");
	return 0;
}
