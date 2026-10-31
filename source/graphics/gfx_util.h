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

#ifndef GFX_UTIL_H
#define GFX_UTIL_H

#include "../cglm/cglm.h"
#include "../platform/time.h"

#include "../world.h"

struct entity; // pointer-only (was transitive via world.h->camera.h pre-split)

struct camera;
void gutil_clouds_owned(mat4 view_matrix, float brightness, const struct camera* camera, ptime_t now);
void gutil_sky_box_owned(mat4 view_matrix, float celestial_angle, vec3 color_top,
	vec3 color_bottom, const struct camera* camera);
void gutil_clouds(mat4 view_matrix, float brightness);
void gutil_sky_box(mat4 view_matrix, float celestial_angle, vec3 color_top,
				   vec3 color_bottom);
void gutil_block_selection(mat4 view_matrix, struct block_info* this);
void gutil_entity_selection(mat4 view_matrix, const struct entity *e);


#endif
