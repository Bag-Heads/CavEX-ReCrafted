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
bool render_item_map_upload(int32_t map_id);
struct item_map_marker;
void render_item_map_marker_transform(const struct item_map_marker* marker,
	mat4 view, mat4 out, uint8_t uv[4][2]);
void render_item_map(struct item* item, struct item_data* stack, mat4 view,
					 bool fullbright, enum render_item_env env);
void render_item_flat(struct item* item, struct item_data* stack, mat4 view,
					  bool fullbright, enum render_item_env env);
void render_item_block(struct item* item, struct item_data* stack, mat4 view,
					   bool fullbright, enum render_item_env env);

#endif
