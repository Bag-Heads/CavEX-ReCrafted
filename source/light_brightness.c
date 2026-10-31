/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "light_brightness.h"

#include "daytime.h" // daytime_skylight_subtracted

#include <math.h>
#include <stddef.h>

int underwater_overlay_sample_y(float pos_y, float height, float y_offset) {
	/* [Beta 1.7.3 Entity.getEntityBrightness] bbox minY + 0.66D * height.
	 * Partial ticks are unused by Beta's method. */
	return (int)floor((double)pos_y - (double)y_offset
				  + (double)height * 0.66);
}

int underwater_overlay_sample_y_local_player(float eye_y, float height) {
	return underwater_overlay_sample_y(eye_y, height, 1.62F);
}

struct underwater_overlay_color underwater_overlay_tint(
	int sky_light, int block_light, int sky_subtract,
	const float* light_lookup) {
	int level = sky_light - sky_subtract;
	if(block_light > level)
		level = block_light;
	if(level < 0)
		level = 0;
	if(level > 15)
		level = 15;
	/* Existing model tint conversion: render_model.c uses roundf(b * 255). */
	uint8_t rgb = (uint8_t)roundf(light_lookup[level] * 255.0F);
	return (struct underwater_overlay_color){rgb, rgb, rgb, 0x80};
}

/* Inputs of the lightmap gfx_build_lightmap built last (light_brightness_current
   re-derives one entry from them; two words instead of a 1 KB copy). */
static int s_current_sky_subtract = 0;
static const float* s_current_light_lookup = NULL;

void gfx_build_lightmap(float out[256], float sun_brightness,
					   float rain_strength, float thunder_strength,
					   const float* light_lookup) {
	// Beta World.calculateSkylightSubtracted: dim the sky channel by an INTEGER step
	// count (0 at clear noon .. 11 at deep night), NOT a continuous multiply. Rain
	// and thunder add to that subtract, so a storm darkens the whole world's sky
	// channel -- torchlight (the blockLight arm of the max below) is untouched.
	int sky_subtract = daytime_skylight_subtracted(sun_brightness, rain_strength,
												   thunder_strength);
	for(int sky = 0; sky < 16; sky++) {
		for(int torch = 0; torch < 16; torch++) {
			// Beta Chunk.getBlockLightValue: max(blockLight, skyLight - subtract).
			// blockLight (torch) >= 0 keeps level in [0,15] with no separate clamp.
			int level = sky - sky_subtract;
			if(torch > level)
				level = torch;
			out[torch * 16 + sky] = light_lookup[level];
		}
	}
	s_current_sky_subtract = sky_subtract;
	s_current_light_lookup = light_lookup;
}

float light_brightness_current(uint8_t packed_light) {
	if(!s_current_light_lookup)
		return 1.0F;
	/* The same entry gfx_build_lightmap wrote at out[torch * 16 + sky]. */
	int torch = packed_light >> 4;
	int level = (packed_light & 0x0F) - s_current_sky_subtract;
	if(torch > level)
		level = torch;
	return s_current_light_lookup[level];
}

// Beta WorldProvider.generateLightBrightnessTable(), overworld floor 0.05F.
const float light_lookup_overworld[16] = {
	0.05F,	0.067F, 0.085F, 0.106F, 0.129F, 0.156F, 0.186F, 0.221F,
	0.261F, 0.309F, 0.367F, 0.437F, 0.525F, 0.638F, 0.789F, 1.0F,
};

// Beta WorldProviderHell, nether floor 0.1F.
const float light_lookup_nether[16] = {
	0.1F, 0.116F, 0.133F, 0.153F, 0.175F, 0.2F,	  0.229F, 0.262F,
	0.3F, 0.345F, 0.4F,	  0.467F, 0.55F,  0.657F, 0.8F,	  1.0F,
};
