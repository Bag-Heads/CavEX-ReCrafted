/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#ifndef SIGN_TEXT_RENDER_FEED_H
#define SIGN_TEXT_RENDER_FEED_H

#include "../cglm/cglm.h"

/*
	Draws the text of every nearby sign. Call once per frame from the world
	render bracket, AFTER the entity pass (Beta draws its
	TileEntitySpecialRenderers there) and before the GUI. Cheap early-out
	when no sign is in range. See sign_text_render_feed.c for the culling
	policy and why this glue file exists at all.
*/
struct camera;
void sign_text_render_feed_owned(mat4 view, const struct camera* camera);
void sign_text_render_feed(mat4 view);

#endif
