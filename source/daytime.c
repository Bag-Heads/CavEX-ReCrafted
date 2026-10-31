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

#include <assert.h>

#include "daytime.h"
#include "game/game_state.h"
#include "util.h"
#include "worldgen/beta_math.h" // mh_cos: Beta MathHelper.cos (SIN_TABLE lookup)

/* next-gaps-0924/w2 (after Sol's e58b9a92 W3-r2 starting point): ONE Beta day/night curve for
 * every caller -- the client lightmap, the server's cached skylightSubtracted, isDaytime (bed gate,
 * sleep wake, undead sun-burn). Beta has exactly one World.calculateSkylightSubtracted
 * [740c583 World.java:991-1007] on WorldProvider.calculateCelestialAngle [WorldProvider.java:44-56];
 * the final cosine is MathHelper.cos (the 65536-entry SIN_TABLE), the angle's own cosine is double
 * Math.cos narrowed to float. -ffp-contract=off (Makefile/CMakeLists) keeps every float step Java's. */

// WorldProvider.calculateCelestialAngle's body from its `var5` (the day fraction - 0.25 before wrap).
static float beta_angle_from_var5(float var5) {
	if(var5 < 0.0F)
		++var5;
	if(var5 > 1.0F)
		--var5;
	float var6 = var5;
	var5 = 1.0F - (float)((cos((double)var5 * 3.141592653589793) + 1.0) / 2.0);
	return var6 + (var5 - var6) / 3.0F;
}

float daytime_beta_celestial_angle(int64_t world_time, float partial) {
	int var4 = (int)(world_time % 24000); // Java long % keeps the sign; (int) narrows
	return beta_angle_from_var5(((float)var4 + partial) / 24000.0F - 0.25F);
}

float daytime_sun_from_angle(float angle) {
	// World.calculateSkylightSubtracted:992-999, Java's float steps literally.
	float var3 = 1.0F - (mh_cos(angle * 3.1415927F * 2.0F) * 2.0F + 0.5F);
	if(var3 < 0.0F)
		var3 = 0.0F;
	if(var3 > 1.0F)
		var3 = 1.0F;
	return 1.0F - var3;
}

int daytime_skylight_subtracted_at(uint64_t clock, float rain_strength,
								   float thunder_strength) {
	// The value Beta holds while its clock reads `clock`: computed by World.tick:1757 at the
	// tick-start time clock-1 with calculateSkylightSubtracted(1.0F) -- the +1.0F partial makes
	// that exactly the angle of `clock` itself.
	float angle = daytime_beta_celestial_angle((int64_t)clock - 1, 1.0F);
	return daytime_skylight_subtracted(daytime_sun_from_angle(angle), rain_strength,
									   thunder_strength);
}

float daytime_light_sun_dimension(float time, int dimension) {
	return (dimension == WORLD_DIM_OVERWORLD)
		? daytime_sun_from_angle(daytime_celestial_angle(time))
		: 0.0F;
}

float daytime_light_sun(float time) {
	return daytime_light_sun_dimension(time, gstate.world.dimension);
}

float daytime_brightness_dimension(float time, int dimension) {
	if(dimension != WORLD_DIM_OVERWORLD)
		return 0.0F;
	/* WorldProvider/World sky colour: clamp(MathHelper.cos(angle * PI * 2) * 2 + 0.5). The lightmap
	 * uses daytime_light_sun (the calculateSkylightSubtracted inversion), not this. */
	return glm_clamp(mh_cos(daytime_celestial_angle(time) * 3.1415927F * 2.0F) * 2.0F
						 + 0.5F,
					 0.0F, 1.0F);
}

float daytime_brightness(float time) {
	return daytime_brightness_dimension(time, gstate.world.dimension);
}

float daytime_celestial_angle(float time) {
	// `time` is the client's integer-tick day fraction ((ticks % 24000) / 24000.0F) -- Java's
	// ((float)var4 + 0.0F) / 24000.0F -- so this is calculateCelestialAngle at partial 0.
	return beta_angle_from_var5(time - 0.25F);
}

float daytime_get_time(void) {
	return gstate.world_time
		+ time_diff_s(gstate.world_time_start, time_get()) * 1000.0f / DAY_TICK_MS;
}

void daytime_sky_colors_dimension(float time, int dimension, vec3 top_plane,
									 vec3 bottom_plane, vec3 atmosphere) {
	daytime_sky_colors_flash(time, dimension, 0.0F, top_plane, bottom_plane,
							 atmosphere);
}

void daytime_sky_colors_flash(float time, int dimension, float flash,
							  vec3 top_plane, vec3 bottom_plane,
							  vec3 atmosphere) {
	assert(top_plane && bottom_plane && atmosphere);

	if(dimension == WORLD_DIM_OVERWORLD) {
		float brightness_mul = daytime_brightness_dimension(time, dimension);

		vec3 world_sky_color = {
			0.6222222F - (0.7F / 3.0F) * 0.05F,
			0.5F + (0.7F / 3.0F) * 0.1F,
			1.0F,
		};

		hsv2rgb(world_sky_color + 0, world_sky_color + 1, world_sky_color + 2);
		glm_vec3_scale(world_sky_color, brightness_mul, world_sky_color);

		/* Lightning flash, the LAST term of World.func_4079_a (sky colour,
		 * World.java:1050-1060): sky = sky*(1-f) + (0.8,0.8,1.0)*f with
		 * f = min(field_27172_i - partial, 1) * 0.45F (caller computes it,
		 * graphics/render_lightning.c). Fog picks it up through the sky mix
		 * below, as EntityRenderer.updateFogColor does. f == 0 skips it, so
		 * the no-flash path is unchanged. */
		if(flash > 0.0F) {
			world_sky_color[0] = world_sky_color[0] * (1.0F - flash) + 0.8F * flash;
			world_sky_color[1] = world_sky_color[1] * (1.0F - flash) + 0.8F * flash;
			world_sky_color[2] = world_sky_color[2] * (1.0F - flash) + 1.0F * flash;
		}

		vec3 fog_color = {
			0.7529412F * brightness_mul * 0.94F + 0.06F,
			0.8470588F * brightness_mul * 0.94F + 0.06F,
			1.0F * brightness_mul * 0.91F + 0.09F,
		};

		vec3 atmosphere_color;
		glm_vec3_lerp(fog_color, world_sky_color, 0.29F, atmosphere_color);
		glm_vec3_scale(atmosphere_color,
					   powf(0.8F, (1.0F - brightness_mul) * 11.0F),
					   atmosphere_color);

		vec3 bottom_plane_color = {0.04F, 0.04F, 0.1F};
		glm_vec3_muladd(world_sky_color, (vec3) {0.2F, 0.2F, 0.6F},
						bottom_plane_color);

		glm_vec3_scale(atmosphere_color, 255.0F, atmosphere);
		glm_vec3_scale(world_sky_color, 255.0F, top_plane);
		glm_vec3_scale(bottom_plane_color, 255.0F, bottom_plane);
	} else {
		vec3 const_color = {0.2F, 0.03F, 0.03F};
		glm_vec3_scale(const_color, 255.0F, atmosphere);
		glm_vec3_scale(const_color, 255.0F, top_plane);
		glm_vec3_scale(const_color, 255.0F, bottom_plane);
	}
}

void daytime_sky_colors(float time, vec3 top_plane, vec3 bottom_plane,
						vec3 atmosphere) {
	daytime_sky_colors_dimension(time, gstate.world.dimension, top_plane,
								 bottom_plane, atmosphere);
}
