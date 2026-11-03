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

#ifndef RENDER_ITEM_H
#define RENDER_ITEM_H

#include "../cglm/cglm.h"

#include "../item/item_types.h"

void render_item_init(void);
void render_item_update_light(uint8_t light);
// Held map (render slice part 2): first-person live map draw; every other
// env falls through to render_item_flat (Beta keeps the static sprite there).
// Upload the requested plane before drawing; false on scratch allocation failure.
// Single-player entry: viewport 0 (texture_map_live).
bool render_item_map_upload(int32_t map_id);
// ci-1101/23: split screen draws both viewports in ONE GX frame, and a texture
// upload never waits for the other viewport's queued draw. Each viewport owns
// its own live surface and its own id/version change gate; `viewport` is the
// owner's index in gstate.players (0 = primary, 1 = split partner).
bool render_item_map_upload_viewport(unsigned viewport, int32_t map_id);
struct tex_gfx* render_item_map_live_texture(unsigned viewport);
struct item_map_marker;
struct client_session;
void render_item_map_marker_transform(const struct item_map_marker* marker,
	mat4 view, mat4 out, uint8_t uv[4][2]);
// Beta ItemRenderer:154-220; pitch in degrees, swing/equip in [0,1].
// Outputs include the engine's map-vertex and arm-box coordinate conversions.
void render_item_map_transforms(float pitch, float swing, float equip,
	mat4 surface, mat4 hands[2]);
void render_item_map(struct item* item, struct item_data* stack, mat4 view,
	bool fullbright, enum render_item_env env, const struct client_session* owner,
	float pitch, float swing, float equip);
// ci-1101/31: the split viewport being drawn (0 = primary, 1 = partner).
// main.c sets it per viewport pass and resets it to 0 after the loop; a
// compass/clock drawn while it is 1 samples that player's private needle
// texture (item_needle_fx_split_texture) instead of the atlas cell.
void render_item_set_viewport(unsigned viewport);
unsigned render_item_viewport(void);
// The u8 texcoord of strip boundary k (0..16) of a flat item icon whose
// origin is `origin` and whose extent is `cell`: 16 for an items-atlas cell
// (k = texel), 256 for a whole private 16x16 needle surface (k = 16 texels),
// clamped to 255 at the far edge. k = 16 is the face's far edge (s1/t1).
uint8_t render_item_flat_uv(uint8_t origin, int cell, int k);
void render_item_flat(struct item* item, struct item_data* stack, mat4 view,
					  bool fullbright, enum render_item_env env);
void render_item_block(struct item* item, struct item_data* stack, mat4 view,
					   bool fullbright, enum render_item_env env);

#endif
