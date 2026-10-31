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

#ifndef LIGHT_BRIGHTNESS_H
#define LIGHT_BRIGHTNESS_H

#include <stdint.h>

struct underwater_overlay_color {
	uint8_t r, g, b, a;
};

/* Beta Entity.getEntityBrightness (Entity.java:587-591): floor(posY -
 * yOffset + bbox height*0.66D). Shared by the submerged overlay, mob models,
 * remote-player models, and entity_local_player_render_other (split partner).
 * Pass current logical posY, not interpolated render posY (Beta ignores the
 * partial-tick argument), before any render-only sneak shift. Mob and
 * remote-player posY is feet (yOffset=0); local-player posY is eye
 * (pass 1.62F explicitly; its engine y_offset field remains zero).
 * Height is the entity AABB height, never eye height.
 * The method deliberately does not add Entity.ySize. */
int underwater_overlay_sample_y(float pos_y, float height, float y_offset);

/* The local player's pos[1] is the EYE (Beta posY with EntityPlayer.yOffset
 * 1.62F, EntityPlayer.java:44), so its sample cell subtracts 1.62F. Known
 * differences, both unreachable for a submerged local player: an unloaded
 * chunk reads light level 0 (Beta returns entityBrightness 0.0F), and the
 * slab/stairs/farmland neighbour-max of World.getBlockLightValue_do is not
 * applied (the player's own cell cannot be one of those solid blocks). */
int underwater_overlay_sample_y_local_player(float eye_y, float height);

/* Beta World.getLightBrightness -> lightBrightnessTable[max(block,sky-sub)].
 * ItemRenderer.renderWarpedTextureOverlay uses (b,b,b,0.5F). */
struct underwater_overlay_color underwater_overlay_tint(
	int sky_light, int block_light, int sky_subtract, const float* light_lookup);

// Per-dimension "light level -> screen brightness" tables, byte-faithful to
// Beta 1.7.3 WorldProvider.generateLightBrightnessTable() (decompile 740c583):
//
//     float floor = <0.05F overworld | 0.1F nether>;
//     for (int i = 0; i <= 15; ++i) {
//         float v = 1.0F - (float)i / 15.0F;
//         table[i] = (1.0F - v) / (v * 3.0F + 1.0F) * (1.0F - floor) + floor;
//     }
//
// The brightness floor lands at index 0 (the moonless-cave / deep-night minimum:
// 0.05 overworld, 0.10 nether) and index 15 is full daylight (1.0). The index is
// the effective light level  max(blockLight, skyLight - skylightSubtracted)
// (Beta Chunk.getBlockLightValue); see gfx_update_light + daytime_skylight_subtracted.
//
// Stored as the 3-decimal authored values (the historical fCavEX literals). They
// match the formula above to <5e-4 everywhere -- visually exact, and pinned to the
// formula byte-for-byte (within that authored precision) by tests/test_daylight_curve.c.
extern const float light_lookup_overworld[16];
extern const float light_lookup_nether[16];

// Build the 256-entry (torch x sky) -> brightness lightmap gfx_update_light uploads,
// the Beta-faithful way:
//   skylightSubtracted = daytime_skylight_subtracted(sun, rain, thunder);
//   level = max(blockLight, skyLight - skylightSubtracted) (Beta Chunk.getBlockLightValue,
//   blockLight>=0 keeps level in [0,15]); out[torch*16 + sky] = light_lookup[level].
// `sun_brightness` is daytime_brightness()'s clamped sun factor (the value main.c feeds
// gfx_update_light). `rain_strength`/`thunder_strength` are the world's CURRENT weather
// strengths (Beta evaluates this at partial == 1.0F, so no prev/cur interpolation --
// see daytime.h); they must be 0 in the Nether, which main.c gates. Shared by pc/gfx.c
// AND wii/gfx.c so their lightmaps cannot drift, and directly executed by
// tests/test_daylight_curve.c (so the pin is real, not a copy).
void gfx_build_lightmap(float out[256], float sun_brightness,
					   float rain_strength, float thunder_strength,
					   const float* light_lookup);

// Brightness of one packed light byte ((torch << 4) | sky -- the byte an entity
// render feeds render_entity_update_light) in the lightmap gfx_build_lightmap
// built LAST. Both GPU backends build the lightmap they draw with through
// gfx_build_lightmap (gfx_update_light, once per viewport before its entity
// pass), so this is the brightness the model's vertices are lit by: Beta's
// getEntityBrightness for RenderLiving's hurt/death wash. 1.0F until the first
// build (next-gaps-0924/R1b). Render thread only.
float light_brightness_current(uint8_t packed_light);

#endif
