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

#ifndef RENDER_NAME_TAG_DRAW_H
#define RENDER_NAME_TAG_DRAW_H

#include "../entity/entity.h" // dict_entity_t

// Forward-declared only -- this header stays a pointer-only dependency on
// game/camera.h, same as entity.h's own entities_client_render(dict_entity_t,
// struct camera*, float) prototype. render_name_tag_draw.c includes the real
// camera.h for the field accesses (c->view/x/y/z/rx/ry).
struct camera;

/*
	Batched per-frame name-tag pass for every ENTITY_REMOTE_PLAYER in
	`entities` within NAME_TAG_RENDER_DISTANCE of `c`. Call once per frame,
	NOT per-entity -- mirrors sign_text_render_feed's deferred/lazy state
	discipline: a frame with no remote player in range costs exactly one
	dict walk and nothing else (no GX/GL state touched at all).

	tick_delta is the same render-frame interpolation factor
	entities_client_render already receives, used to lerp each remote
	player's pos_old->pos so the tag tracks the SAME interpolated position
	the model itself rendered at this frame (entity_remote_player_render's
	own lerp, mirrored here rather than shared, since that renderer is
	static to entity_remote_player.c).

	See render_name_tag_draw.c's header comment for the exact state-
	discipline bracket (background quad + two-pass text, the through-walls
	dim pass and the opaque depth-tested pass) and why it is mandatory to
	restore GX state before returning.
*/
void render_name_tag_draw_pass(dict_entity_t entities, struct camera* c,
							   float tick_delta);

#endif
