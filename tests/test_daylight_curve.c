/*
	Golden faithfulness test for the Beta 1.7.3 daylight lightmap curve (audit D3).

	gfx_update_light builds the 16x16 (torch x sky) -> brightness lightmap. The fix
	this test guards replaced the old CavEX `max(table[torch], table[sky]*daytime)`
	continuous multiply (too bright at dawn/dusk, moonlit night crushed to pure
	black) with Beta's exact path:

	  skylightSubtracted = World.calculateSkylightSubtracted(angle, rain, thunder)
	  level              = Chunk.getBlockLightValue = max(block, sky - skylightSubtracted)
	  brightness         = WorldProvider.lightBrightnessTable[level]

	This test re-derives each step independently from the decompile (commit 740c583)
	and asserts the production code matches:
	  - light_lookup_overworld/_nether == generateLightBrightnessTable (floors 0.05/0.10)
	  - daytime_skylight_subtracted()  == calculateSkylightSubtracted, given the sun
	    brightness the engine feeds it (Beta's clamp(1-c,0,1)*11 reduces to (1-b)*11
	    for b = clamp(c,0,1), the value daytime_sun_from_angle() produces)
	  - ...INCLUDING the two weather darkening factors (rain, and thunder weighted by
	    rain), which dim the sky channel during a storm -- sections (2b), (4b), (6)
	  - the combine max(block, sky-sub) reproduces getBlockLightValue AND never leaves
	    [0,15] (a negative table index would be a hard crash, not a visual glitch)

	NOTE ON PARTIAL TICKS: calculateSkylightSubtracted is called from exactly two
	places in all of Beta (World.java:1733 and :1763), BOTH with partial == 1.0F, both
	in the tick loop, both storing into the single `World.skylightSubtracted` field
	that getBlockLightValue and isDaytime then read. b1.7.3 has no client-side
	per-frame lightmap (EntityRenderer.updateLightmap does not exist yet). At
	partial == 1.0F both getRainStrength and getWeightedThunderStrength collapse to
	the CURRENT strength (prev + (cur-prev)*1 == cur), so this path takes the raw
	current strengths and does NOT interpolate prev/cur -- the same reduction already
	documented on server_weather_is_raining(). Only the precipitation CURTAIN alpha
	(render_rain_snow) interpolates, because Beta interpolates it there and only there.

	SCOPE: the lightmap checks prove the level->brightness MAPPING (the D3 change)
	with the same sun input on both sides. The clock-based isDaytime checks also use
	an independent Beta angle and MathHelper table reference; they do not call the
	production daylight helpers to calculate expected values.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "daytime.h"		  // production: daytime_skylight_subtracted (inline)
#include "light_brightness.h" // production: light_lookup_overworld / _nether

// ---- Beta references (decompile 740c583) ----------------------------------

// WorldProvider.generateLightBrightnessTable(): floor 0.05F overworld, 0.1F nether.
static void beta_brightness_table(float floor, float out[16]) {
	for(int i = 0; i <= 15; ++i) {
		float v = 1.0F - (float)i / 15.0F;
		out[i] = (1.0F - v) / (v * 3.0F + 1.0F) * (1.0F - floor) + floor;
	}
}

// World.calculateSkylightSubtracted(), line-for-line, INCLUDING the two weather
// factors. `c` is the raw celestial term cos(angle*2pi)*2+0.5 (UNclamped) so both
// clamp arms are tested. rain/thunder are the RAW strengths (World's own
// rainingStrength / thunderingStrength); the rain-weighting of thunder is applied
// here exactly where Beta's getWeightedThunderStrength applies it.
//
//     calculateSkylightSubtracted(partialTicks):
//         angle = getCelestialAngle(partialTicks);
//         f = 1 - (MathHelper.cos(angle * 3.1415927F * 2) * 2 + 0.5), in float;
//         f is clamped below at 0
//         and above at 1;
//         f = 1 - f;
//         f is scaled in double by 1 - (float)(rainStrength * 5.0F) / 16, narrowed to float;
//         then the same way by 1 - (float)(weightedThunderStrength * 5.0F) / 16;
//         f = 1 - f;
//         the result is (int)(f * 11.0F).
// (end)
// [740c583 World.java:999-1015]
static int beta_skylight_subtracted_from_cos(float c, float rain, float thunder) {
	float var3 = 1.0F - c;
	if(var3 < 0.0F)
		var3 = 0.0F;
	if(var3 > 1.0F)
		var3 = 1.0F;
	var3 = 1.0F - var3;
	// getWeightedThunderStrength(1.0F) == thunderingStrength * rainingStrength.
	// [740c583 World.java:2428-2430]
	float weighted_thunder = thunder * rain;
	// The double promote-multiply-narrow is Beta's literal shape, NOT a plain float
	// chain -- same 1-ULP trap as updateWeather's +-0.01 step (server_weather.c F4).
	var3 = (float)((double)var3 * (1.0 - (double)(rain * 5.0F) / 16.0));
	var3 = (float)((double)var3 * (1.0 - (double)(weighted_thunder * 5.0F) / 16.0));
	var3 = 1.0F - var3;
	return (int)(var3 * 11.0F);
}

// Chunk.getBlockLightValue(x,y,z, skylightSubtracted).
static int beta_block_light_value(int sky, int block, int sub) {
	int v = sky - sub;
	if(block > v)
		v = block;
	return v;
}

// daytime_brightness()'s clamp: b = clamp(c, 0, 1), the value fed to the lightmap.
static float sun_brightness_from_cos(float c) {
	if(c < 0.0F)
		return 0.0F;
	if(c > 1.0F)
		return 1.0F;
	return c;
}

// Independent WorldProvider.calculateCelestialAngle + MathHelper.cos reference.
// World.tick calculates at tick-start clock-1 with partial 1.0F, then advances
// the clock; World.isDaytime reads that cached value. [Beta World.java:1757-1766,
// WorldProvider.java:42-56, MathHelper.java:4,10-12,56-59]
static float beta_sin_table[65536];
static void beta_init_sin_table(void) {
	for(int i = 0; i < 65536; i++)
		beta_sin_table[i] = (float)sin((double)i * 3.141592653589793 * 2.0 / 65536.0);
}
static float beta_cos_table(float x) {
	// This test's clock domain is 0..23999, so Java's float-to-int is truncation.
	return beta_sin_table[(int)(x * 10430.378F + 16384.0F) & 65535];
}
static float beta_celestial_cos_at_clock(uint64_t clock) {
	int var4 = (int)(((int64_t)clock - 1) % 24000);
	float var5 = ((float)var4 + 1.0F) / 24000.0F - 0.25F;
	if(var5 < 0.0F) ++var5;
	if(var5 > 1.0F) --var5;
	float var6 = var5;
	var5 = 1.0F - (float)((cos((double)var5 * 3.141592653589793) + 1.0) / 2.0);
	var5 = var6 + (var5 - var6) / 3.0F;
	return beta_cos_table(var5 * 3.1415927F * 2.0F) * 2.0F + 0.5F;
}

// Independent reference for the full 256-entry lightmap gfx_build_lightmap produces:
// table[ max(blockLight, skyLight - skylightSubtracted) ] at index torch*16+sky.
// sub comes from the full Beta formula above (for sun already in [0,1] the clamp
// arms are identity, so the sun value doubles as the raw `c`); the level uses
// beta_block_light_value.
static void beta_lightmap_ref(float out[256], float sun, float rain, float thunder,
							  const float* table) {
	int sub = beta_skylight_subtracted_from_cos(sun, rain, thunder);
	for(int sky = 0; sky < 16; sky++)
		for(int torch = 0; torch < 16; torch++)
			out[torch * 16 + sky] = table[beta_block_light_value(sky, torch, sub)];
}

// The exact strength values World.updateWeather actually produces: a
// (float)((double)x +- 0.01D) recurrence, NOT round decimals. These have the messy
// mantissas where the double promote-narrow above can differ from a plain float
// chain by 1 ULP, so the sweep below drives the production code with them rather
// than with tidy k/100.0F literals. [740c583 World.updateWeather]
static void beta_strength_ramp(float out[101]) {
	float v = 0.0F;
	for(int i = 0; i <= 100; i++) {
		out[i] = v;
		v = (float)((double)v + 0.01);
		if(v > 1.0F)
			v = 1.0F;
	}
}

int main(void) {
	beta_init_sin_table();
	// (1) Production tables == Beta generateLightBrightnessTable, to authored
	//     3-decimal precision (the literals are rounded Beta values; <5e-4 error).
	float ref_ow[16], ref_nether[16];
	beta_brightness_table(0.05F, ref_ow);
	beta_brightness_table(0.1F, ref_nether);
	for(int i = 0; i < 16; i++) {
		assert(fabsf(light_lookup_overworld[i] - ref_ow[i]) < 6e-4F);
		assert(fabsf(light_lookup_nether[i] - ref_nether[i]) < 6e-4F);
	}
	// Floor and ceiling are exact landmarks (the night floor + full daylight).
	assert(light_lookup_overworld[0] == 0.05F);
	assert(light_lookup_overworld[15] == 1.0F);
	assert(light_lookup_nether[0] == 0.1F);
	assert(light_lookup_nether[15] == 1.0F);

	// (2) daytime_skylight_subtracted == Beta calculateSkylightSubtracted, swept over
	//     the full celestial range (c in [-1.5, 2.5] = cos*2+0.5) incl. both clamp
	//     arms, plus the realistic time-of-day domain via Beta's celestial angle.
	for(int k = -2000; k <= 2000; k++) {
		float c = (float)k / 1000.0F; // -2.0 .. 2.0, brackets the real [-1.5,2.5]
		float b = sun_brightness_from_cos(c);
		assert(daytime_skylight_subtracted(b, 0.0F, 0.0F)
			   == beta_skylight_subtracted_from_cos(c, 0.0F, 0.0F));
	}
	for(int t = 0; t < 24000; t++) {
		float c = beta_celestial_cos_at_clock((uint64_t)t);
		float b = sun_brightness_from_cos(c);
		assert(daytime_skylight_subtracted(b, 0.0F, 0.0F)
			   == beta_skylight_subtracted_from_cos(c, 0.0F, 0.0F));
		// Beta World.isDaytime() == skylightSubtracted < 4. The production gate
		// daytime_is_daytime_ticks(ticks) -- the one block_bed uses to refuse daytime
		// sleep -- must match the independent reference at EVERY tick of the day.
		// [decompile 740c583 World.isDaytime]
		bool got_daytime = daytime_is_daytime_ticks((uint64_t)t, 0.0F, 0.0F);
		bool want_daytime = beta_skylight_subtracted_from_cos(c, 0.0F, 0.0F) < 4;
		if(got_daytime != want_daytime)
			fprintf(stderr, "daylight clock=%d rain=0 thunder=0 raw_c=%a got=%d want=%d\n",
				t, (double)c, got_daytime, want_daytime);
		assert(got_daytime == want_daytime);
	}

	// (2b) WEATHER: the two darkening factors calculateSkylightSubtracted applies on
	//      top of the sun term. Hand-computed landmarks first -- every one of these
	//      is EXACT in binary float (the factors land on 11/16 and 121/256), so they
	//      are written as literal integers, not tolerances:
	//
	//        f      = sun * (1 - rain*5/16) * (1 - thunder*rain*5/16)
	//        result = (int)((1 - f) * 11)
	//
	//   sun rain thu | f                      | (1-f)*11      | int
	//   --------------------------------------------------------------
	//   1.0  0.0  0.0 | 1.0                   | 0.0           |  0   (clear noon)
	//   0.0  0.0  0.0 | 0.0                   | 11.0          | 11   (clear midnight)
	//   1.0  1.0  0.0 | 0.6875                | 3.4375        |  3   (noon, full rain)
	//   1.0  1.0  1.0 | 0.6875^2 = 0.47265625 | 5.80078125    |  5   (noon, thunderstorm)
	//   0.0  1.0  1.0 | 0.0                   | 11.0          | 11   (already max dark)
	//   1.0  0.5  0.0 | 0.84375               | 1.71875       |  1   (noon, half rain)
	//   1.0  0.0  1.0 | 1.0                   | 0.0           |  0   (thunder w/o rain)
	assert(daytime_skylight_subtracted(1.0F, 0.0F, 0.0F) == 0);
	assert(daytime_skylight_subtracted(0.0F, 0.0F, 0.0F) == 11);
	assert(daytime_skylight_subtracted(1.0F, 1.0F, 0.0F) == 3);
	assert(daytime_skylight_subtracted(1.0F, 1.0F, 1.0F) == 5);
	assert(daytime_skylight_subtracted(0.0F, 1.0F, 1.0F) == 11);
	assert(daytime_skylight_subtracted(1.0F, 0.5F, 0.0F) == 1);
	// THE rain-weighting guard: getWeightedThunderStrength multiplies thunder BY
	// rain, so thunder at full strength with NO rain must darken nothing at all.
	// An implementation that forgot the `* rain` returns 3 here, not 0.
	// [740c583 World.getWeightedThunderStrength]
	assert(daytime_skylight_subtracted(1.0F, 0.0F, 1.0F) == 0);

	// Weather can never darken noon past 5, so PLAIN RAIN alone never reaches the
	// isDaytime threshold of 4 -- only a thunderstorm does. This is Beta's real,
	// observable rule ("you can sleep through a daytime thunderstorm, but not
	// through ordinary rain") falling out of the arithmetic, and it is the reason
	// the bed gate below must be weather-aware at all.
	assert(daytime_skylight_subtracted(1.0F, 1.0F, 0.0F) < 4);  // rain: still day
	assert(daytime_skylight_subtracted(1.0F, 1.0F, 1.0F) >= 4); // storm: counts as night
	assert(daytime_is_daytime_ticks(6000, 1.0F, 0.0F) == true);	 // noon, full rain
	assert(daytime_is_daytime_ticks(6000, 1.0F, 1.0F) == false); // noon, thunderstorm

	// (2c) The DOUBLE promote-multiply-narrow is load-bearing, not cosmetic. Beta
	//      multiplies f in double by (1 - (double)(strength * 5.0F) / 16) and narrows to float;
	//      the "obvious" plain-float chain
	//          f = f * (1.0F - (strength * 5.0F) / 16.0F)
	//      agrees almost everywhere, but a 1-ULP difference in f occasionally lands
	//      on the far side of an integer boundary of (1-f)*11 and shifts the result
	//      by a WHOLE light level. An exhaustive scan (every float sun in [~5e-10, 1]
	//      x a 201x201 strength grid, 1.28e9 triples) found 22 such triples; three are
	//      pinned below, chosen to cover both directions and the dark extreme. Written
	//      as C99 hex-float literals so the input is the exact bit pattern, not a
	//      decimal that re-rounds.
	//
	//      If these three ever start failing together, the promotion shape was
	//      "simplified" to a float chain -- put it back.
	assert(daytime_skylight_subtracted(0x1.d28p-1F, 1.0F / 200.0F, 87.0F / 200.0F)
		   == 1); // plain-float chain yields 0
	assert(daytime_skylight_subtracted(0x1.19p-1F, 2.0F / 200.0F, 194.0F / 200.0F)
		   == 4); // plain-float chain yields 5 (the other direction)
	assert(daytime_skylight_subtracted(0x1.8p-25F, 166.0F / 200.0F, 77.0F / 200.0F)
		   == 11); // plain-float chain yields 10 (near-black extreme)

	// Full 2-D sweep over the REAL strength ramp updateWeather produces (see
	// beta_strength_ramp) crossed with the whole celestial range -- production vs
	// the independent line-for-line reference, including both double narrowings.
	float ramp[101];
	beta_strength_ramp(ramp);
	for(int ri = 0; ri <= 100; ri++) {
		for(int ti = 0; ti <= 100; ti++) {
			float rain = ramp[ri], thunder = ramp[ti];
			for(int k = -200; k <= 200; k++) {
				float c = (float)k / 100.0F;
				float b = sun_brightness_from_cos(c);
				assert(daytime_skylight_subtracted(b, rain, thunder)
					   == beta_skylight_subtracted_from_cos(c, rain, thunder));
			}
		}
	}

	// The subtract stays inside the [0,11] table-index budget for EVERY weather
	// state -- section (3)'s no-OOB proof assumes that range.
	for(int ri = 0; ri <= 100; ri++)
		for(int ti = 0; ti <= 100; ti++)
			for(int k = 0; k <= 100; k++) {
				int sub = daytime_skylight_subtracted((float)k / 100.0F, ramp[ri],
													  ramp[ti]);
				assert(sub >= 0 && sub <= 11);
			}

	// isDaytime over every tick of the day x a coarse weather grid.
	for(int t = 0; t < 24000; t += 7) {
		float c = beta_celestial_cos_at_clock((uint64_t)t);
		for(int ri = 0; ri <= 100; ri += 5)
			for(int ti = 0; ti <= 100; ti += 5) {
				bool got_daytime = daytime_is_daytime_ticks((uint64_t)t, ramp[ri], ramp[ti]);
				bool want_daytime = beta_skylight_subtracted_from_cos(c, ramp[ri], ramp[ti]) < 4;
				if(got_daytime != want_daytime)
					fprintf(stderr, "daylight clock=%d rain=%a thunder=%a raw_c=%a got=%d want=%d\n",
						t, (double)ramp[ri], (double)ramp[ti], (double)c,
						got_daytime, want_daytime);
				assert(got_daytime == want_daytime);
			}
	}

	// (3) The level max(blockLight, skyLight-sub) stays a valid [0,15] table index for
	//     EVERY (sub, sky, torch) -- the no-OOB invariant that lets gfx_build_lightmap
	//     index light_lookup[level] with no clamp (Beta relies on blockLight>=0).
	for(int sub = 0; sub <= 11; sub++)
		for(int sky = 0; sky < 16; sky++)
			for(int torch = 0; torch < 16; torch++) {
				int level = beta_block_light_value(sky, torch, sub);
				assert(level >= 0 && level <= 15);
			}

	// (4) THE REAL production lightmap: drive gfx_build_lightmap (the exact function
	//     pc/gfx.c AND wii/gfx.c call) and assert its full 256-entry output is
	//     byte-identical to the independent Beta reference, for both dimensions, swept
	//     over the whole sun-brightness range. This pins the actual shipping combine +
	//     index order -- reverting it to the old continuous multiply, transposing the
	//     torch/sky index, or adding a stray clamp now FAILS here.
	for(int k = 0; k <= 1000; k++) {
		float sun = (float)k / 1000.0F; // 0 (deep night) .. 1 (noon)
		float prod[256], ref[256];

		gfx_build_lightmap(prod, sun, 0.0F, 0.0F, light_lookup_overworld);
		beta_lightmap_ref(ref, sun, 0.0F, 0.0F, light_lookup_overworld);
		for(int i = 0; i < 256; i++)
			assert(prod[i] == ref[i]);

		gfx_build_lightmap(prod, sun, 0.0F, 0.0F, light_lookup_nether);
		beta_lightmap_ref(ref, sun, 0.0F, 0.0F, light_lookup_nether);
		for(int i = 0; i < 256; i++)
			assert(prod[i] == ref[i]);
	}

	// (4b) The same real production lightmap under WEATHER -- this is the whole point
	//      of the fix: every block's rendered brightness, not just the precipitation
	//      quads, dims during a storm. Swept over the real strength ramp.
	for(int ri = 0; ri <= 100; ri += 3)
		for(int ti = 0; ti <= 100; ti += 3)
			for(int k = 0; k <= 100; k++) {
				float sun = (float)k / 100.0F;
				float rain = ramp[ri], thunder = ramp[ti];
				float prod[256], ref[256];

				gfx_build_lightmap(prod, sun, rain, thunder, light_lookup_overworld);
				beta_lightmap_ref(ref, sun, rain, thunder, light_lookup_overworld);
				for(int i = 0; i < 256; i++)
					assert(prod[i] == ref[i]);
			}

	// (5) Documented behaviour landmarks, read straight out of the REAL production
	//     lightmap (index = torch*16 + sky). These are the values the D3 fix produces.
	float noon[256], night[256], dusk[256];
	gfx_build_lightmap(noon, 1.0F, 0.0F, 0.0F, light_lookup_overworld);
	gfx_build_lightmap(night, 0.0F, 0.0F, 0.0F, light_lookup_overworld);
	gfx_build_lightmap(dusk, 0.5F, 0.0F, 0.0F, light_lookup_overworld);
	// Noon: open sky (sky=15, torch=0) reads full daylight.
	assert(noon[0 * 16 + 15] == 1.0F);
	// Deep night: moonlit open sky = table[4] = 0.129 (Beta floor; old curve -> ~0).
	assert(fabsf(night[0 * 16 + 15] - 0.129F) < 6e-4F);
	// Deep-night cave (sky=0, torch=0) -> table[0] = the 0.05 floor, not black.
	assert(night[0 * 16 + 0] == 0.05F);
	// Dusk: open sky (sky=15) = table[10] = 0.367 (integer step, not over-bright 0.5).
	assert(fabsf(dusk[0 * 16 + 15] - 0.367F) < 6e-4F);
	// Torchlight (sky=0, torch=14) is daytime-independent: table[14] = 0.789 always.
	assert(fabsf(noon[14 * 16 + 0] - 0.789F) < 6e-4F);
	assert(fabsf(night[14 * 16 + 0] - 0.789F) < 6e-4F);
	assert(fabsf(dusk[14 * 16 + 0] - 0.789F) < 6e-4F);

	// (6) Storm landmarks -- the user-visible effect of the weather fix, read out of
	//     the REAL production lightmap. Open sky at NOON, by weather state:
	//       clear        sub=0 -> level 15 -> 1.0
	//       full rain    sub=3 -> level 12 -> 0.525
	//       thunderstorm sub=5 -> level 10 -> 0.367
	//     i.e. a full thunderstorm at noon renders the open world at roughly a third
	//     of clear-noon brightness. Before this fix all three read 1.0.
	float rain_noon[256], storm_noon[256];
	gfx_build_lightmap(rain_noon, 1.0F, 1.0F, 0.0F, light_lookup_overworld);
	gfx_build_lightmap(storm_noon, 1.0F, 1.0F, 1.0F, light_lookup_overworld);
	assert(noon[0 * 16 + 15] == 1.0F);
	assert(fabsf(rain_noon[0 * 16 + 15] - 0.525F) < 6e-4F);
	assert(fabsf(storm_noon[0 * 16 + 15] - 0.367F) < 6e-4F);
	// Torchlight is weather-independent (it is the blockLight arm of the max, which
	// skylightSubtracted never touches) -- a storm must not dim a lit cave.
	assert(fabsf(storm_noon[14 * 16 + 0] - 0.789F) < 6e-4F);
	// Deep night is already at the floor, so a storm cannot darken it further.
	float storm_night[256];
	gfx_build_lightmap(storm_night, 0.0F, 1.0F, 1.0F, light_lookup_overworld);
	for(int i = 0; i < 256; i++)
		assert(storm_night[i] == night[i]);

	printf("test_daylight_curve: OK (real gfx_build_lightmap == Beta over full sweep, "
		   "clear + rain + thunder)\n");
	return 0;
}
