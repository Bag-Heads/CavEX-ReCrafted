/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DAYTIME_H
#define DAYTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "cglm/cglm.h"

#define DAY_TICK_MS 50
#define DAY_LENGTH_TICKS 24000

float daytime_brightness(float time);
float daytime_brightness_dimension(float time, int dimension);
float daytime_celestial_angle(float time);
/* next-gaps-0924/w2 -- the ONE Beta day/night curve (daytime.c), MathHelper.cos included.
 * WorldProvider.calculateCelestialAngle(worldTime, partial) [WorldProvider.java:44-56]. */
float daytime_beta_celestial_angle(int64_t world_time, float partial);
/* World.calculateSkylightSubtracted's day/night factor (World.java:992-999) for a celestial angle:
 * the `var3` fed to the weather multiply, i.e. the `sun_brightness` input below. */
float daytime_sun_from_angle(float angle);
/* Beta's World.skylightSubtracted while the clock reads `clock`: calculateSkylightSubtracted(1.0F)
 * evaluated by World.tick at the tick-start time clock-1. Every server reader and the client
 * lightmap derive from this; the server caches it once per tick (server_local.c). */
int daytime_skylight_subtracted_at(uint64_t clock, float rain_strength,
								   float thunder_strength);
/* The client lightmap's sun term for a day fraction (0 outside the overworld). */
float daytime_light_sun(float time);
float daytime_light_sun_dimension(float time, int dimension);
void daytime_sky_colors(float time, vec3 top_plane, vec3 bottom_plane,
						vec3 atmosphere);
void daytime_sky_colors_dimension(float time, int dimension, vec3 top_plane,
									 vec3 bottom_plane, vec3 atmosphere);
// Same, plus Beta's lightning sky flash amount (0 = none; see daytime.c).
void daytime_sky_colors_flash(float time, int dimension, float flash,
							  vec3 top_plane, vec3 bottom_plane, vec3 atmosphere);
float daytime_get_time(void);

// Beta 1.7.3 World.calculateSkylightSubtracted() [decompile 740c583, World.java:999],
// COMPLETE -- day/night curve AND the two weather darkening factors:
//     f = clamp(cos(angle*2pi)*2 + 0.5, 0, 1), the sun-brightness factor;
//     f is scaled in double by 1 - (float)(rainStrength * 5.0F) / 16, narrowed to float;
//     then the same way by 1 - (float)(weightedThunderStrength * 5.0F) / 16;
//     the result is (int)((1.0F - f) * 11.0F).
// `sun_brightness` is EXACTLY that clamped term -- it is what daytime_brightness()
// returns (see daytime.c) and what main.c already feeds gfx_update_light. So the
// per-cell sky light is dimmed by an INTEGER step count (0 at clear noon .. 11 at
// deep night, and up to 5 at noon in a full thunderstorm), matching Beta's single
// lightBrightnessTable lookup rather than a continuous multiply. Defined inline so
// PC and Wii gfx_update_light cannot drift; pinned byte-exact (weather included) by
// tests/test_daylight_curve.c.
//
// PARTIAL TICKS: Beta calls this from exactly two sites (World.java:1733 and :1763),
// BOTH with partial == 1.0F, and both store the result in the one
// `World.skylightSubtracted` field that getBlockLightValue and isDaytime read.
// b1.7.3 has no per-frame client lightmap (no EntityRenderer.updateLightmap yet). At
// partial == 1.0F both strength getters collapse to the CURRENT value
// (prev + (cur-prev)*1 == cur), so this takes the raw current strengths and does NOT
// interpolate -- the same reduction already documented on server_weather_is_raining().
// Only the precipitation CURTAIN alpha (render_rain_snow) interpolates prev/cur,
// because Beta interpolates there and only there.
//
// NETHER: Beta's Nether is a SEPARATE World instance whose strengths are never set,
// so weather never dims it. This engine shares one weather struct across a dimension
// switch and server_weather_tick leaves the strengths FROZEN (not zeroed) when you
// portal in mid-storm, so overworld-only callers must pass 0/0 in the Nether -- see
// the gate at main.c's gfx_update_light call. [740c583: WorldProviderHell.hasNoSky]
static inline int daytime_skylight_subtracted(float sun_brightness,
											  float rain_strength,
											  float thunder_strength) {
	// getWeightedThunderStrength(p) = lerp(prevThunder, thunder, p) * getRainStrength(p)
	// -- thunder is WEIGHTED BY RAIN, so full thunder with no rain darkens NOTHING.
	// Folded in here rather than asked of callers so no call site can forget it.
	// [740c583: World.java:2428-2430]
	float weighted_thunder = thunder_strength * rain_strength;
	// Beta promotes to DOUBLE for each factor multiply and narrows back to float --
	// NOT a plain float chain. Same 1-ULP trap as updateWeather's +-0.01 step
	// (server_weather.c F4), and MEASURED to matter here: an exhaustive scan (every
	// float sun in [~5e-10, 1] x a 201x201 strength grid, 1.28e9 triples) found 22
	// where the plain-float chain lands on the far side of an integer boundary of
	// (1-var3)*11 and shifts the result by a WHOLE light level, in both directions.
	// Three are pinned as hex-float landmarks in tests/test_daylight_curve.c (2c).
	float var3 = (float)((double)sun_brightness
						 * (1.0 - (double)(rain_strength * 5.0F) / 16.0));
	var3 = (float)((double)var3
				   * (1.0 - (double)(weighted_thunder * 5.0F) / 16.0));
	var3 = 1.0F - var3;
	return (int)(var3 * 11.0F);
}

// Beta 1.7.3 World.isDaytime() == skylightSubtracted < 4, evaluated from a world
// time in TICKS for the OVERWORLD plus the current weather strengths. Self-contained
// (no gstate) so server-side block code (e.g. bed sleep) can gate on it; mirrors
// WorldProvider.calculateCelestialAngle + World.calculateSkylightSubtracted +
// daytime_brightness's overworld clamp. Cross-checked tick-for-tick against the
// independent Beta reference in tests/test_daylight_curve.c.
//
// Weather matters HERE, not just for rendering: Beta's isDaytime() reads the same
// weather-darkened `skylightSubtracted` field. At noon a full thunderstorm gives
// subtract 5 (>= 4, so "not daytime" -> you MAY sleep), while even full rain only
// reaches 3 (< 4, so still daytime -> you may NOT sleep). That is exactly Beta's
// observable rule, and it falls straight out of the arithmetic -- see the landmark
// table in tests/test_daylight_curve.c section (2b).
// Callers pass the CURRENT strengths (Beta's partial == 1.0F; see above), i.e.
// s->weather.rain_strength / s->weather.thunder_strength.
// [decompile 740c583 World.isDaytime / calculateSkylightSubtracted /
//  WorldProvider.calculateCelestialAngle]
static inline bool daytime_is_daytime_ticks(uint64_t world_time,
											float rain_strength,
											float thunder_strength) {
	/* w2: the one Beta curve (MathHelper.cos). Server simulation reads the per-tick cached
	 * value instead (server_local_is_daytime); this stays for clock-only callers/tests. */
	return daytime_skylight_subtracted_at(world_time, rain_strength, thunder_strength) < 4;
}

#endif
