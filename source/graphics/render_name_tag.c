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

#include <assert.h>
#include <string.h>

#include "render_name_tag.h"

// Same font-pixel convention as render_sign_text.c (y increasing DOWNWARD,
// glyph top at the pen position) -- mirrored so the same 16x16-cell
// texture_font atlas layout applies unchanged here. See render_sign_text.c's
// own citation for why GLYPH_CELL_PX is a clean 8 rather than Beta's raw
// 7.99F texel-bleed dodge.
#define GLYPH_CELL_PX 8.0f
#define FONT_ATLAS_CELL_PX 16.0f
#define FONT_ATLAS_DIM_PX 256.0f

// [740c583, header comment]: translate(anchor) -> rotate_y(+camera_rx) ->
// rotate_x(camera_ry - PI/2) -> scale(-NAME_TAG_SCALE,-NAME_TAG_SCALE,
// +NAME_TAG_SCALE). Each cglm call below post-multiplies `dest`, matching
// OpenGL's glTranslatef/glRotatef/glScalef call-order semantics exactly like
// render_sign_text.c's build_model_matrix.
static void build_model_matrix(const vec3 anchor, float camera_rx,
							   float camera_ry, mat4 dest) {
	glm_translate_make(dest, (vec3) {anchor[0], anchor[1], anchor[2]});

	// -playerViewY == +camera_rx (see render_name_tag.h's header comment for
	// the full yaw-convention derivation).
	glm_rotate_y(dest, camera_rx, dest);
	// +playerViewX == camera_ry - PI/2.
	glm_rotate_x(dest, camera_ry - GLM_PI_2f, dest);

	glm_scale(dest, (vec3) {-NAME_TAG_SCALE, -NAME_TAG_SCALE, NAME_TAG_SCALE});
}

size_t name_tag_build_quads(const char* username, const vec3 entity_pos,
							const vec3 camera_pos, float camera_rx,
							float camera_ry, const uint8_t glyph_width[256],
							bool is_sneaking,
							struct name_tag_vertex text_out[NAME_TAG_MAX_TEXT_VERTS],
							struct name_tag_vertex bg_out[NAME_TAG_BG_VERTS]) {
	assert(username && entity_pos && camera_pos && glyph_width && text_out
		   && bg_out);

	size_t len = strlen(username);
	if(len == 0)
		return 0; // nothing to draw -- no name, no tag (also guards an
				  // unset/zeroed username slot)
	if(len > NAME_TAG_MAX_CHARS)
		len = NAME_TAG_MAX_CHARS; // defensive only: username[17] never
								  // actually exceeds this

	// [740c583 RenderPlayer.renderName distance gate]: 64-block standing
	// cap, halved to 32 while sneaking (MP visual-parity pass item 2 --
	// see render_name_tag.h's NAME_TAG_RENDER_DISTANCE_SNEAK comment).
	float cap = is_sneaking ? NAME_TAG_RENDER_DISTANCE_SNEAK
							: NAME_TAG_RENDER_DISTANCE;
	float dx = entity_pos[0] - camera_pos[0];
	float dy = entity_pos[1] - camera_pos[1];
	float dz = entity_pos[2] - camera_pos[2];
	float dist2 = dx * dx + dy * dy + dz * dz;
	if(dist2 >= cap * cap)
		return 0;

	// [740c583]: `x, y+2.3F, z` -- see render_name_tag.h's NAME_TAG_EYE_OFFSET
	// comment for the not-yet-tracked sleeping -1.5F branch.
	vec3 anchor = {entity_pos[0], entity_pos[1] + NAME_TAG_EYE_OFFSET,
				   entity_pos[2]};

	mat4 m;
	build_model_matrix(anchor, camera_rx, camera_ry, m);

	// [FontRenderer.getStringWidth]: sum of per-char charWidth, where
	// charWidth[ch] == glyph_width[ch]+1 (this engine's split representation
	// -- see render_sign_text.c's identical citation).
	int total_width = 0;
	for(size_t i = 0; i < len; i++)
		total_width += (int) glyph_width[(unsigned char) username[i]] + 1;

	// [RenderLiving.renderLivingLabel]: `-fontrenderer.getStringWidth(str)/2`
	// (Java int division truncates toward zero, matched by C's `/` on signed
	// ints) for the text pen origin, and an INDEPENDENTLY computed
	// `i = fontrenderer.getStringWidth(str)/2` (note: no unary minus) for the
	// background box half-width. Both truncate the SAME magnitude, so
	// pen_x == -half exactly -- kept as two separate literals anyway to
	// mirror Beta's own two separate expressions bit-for-bit rather than
	// algebraically simplifying them into one.
	int pen_x = -total_width / 2;
	int half = total_width / 2;

	// Background: untextured, spans [-half-1,-1]..[half+1,8] in label space
	// -- [RenderLiving.renderLivingLabel] tessellator quad, corner order
	// matching render_sign_text.c's own (top-left, top-right, bottom-right,
	// bottom-left) GL_QUADS winding.
	float bx0 = (float) (-half - 1), by0 = -1.0f;
	float bx1 = (float) (half + 1), by1 = 8.0f;
	float bg_local[4][3] = {
		{bx0, by0, 0.0f},
		{bx1, by0, 0.0f},
		{bx1, by1, 0.0f},
		{bx0, by1, 0.0f},
	};
	for(int k = 0; k < 4; k++) {
		vec3 world;
		glm_mat4_mulv3(m, bg_local[k], 1.0f, world);
		bg_out[k].x = world[0];
		bg_out[k].y = world[1];
		bg_out[k].z = world[2];
		bg_out[k].u = 0.0f;
		bg_out[k].v = 0.0f;
	}

	size_t count = 0;
	for(size_t i = 0; i < len; i++) {
		unsigned char ch = (unsigned char) username[i];
		uint8_t w = glyph_width[ch];

		// Space glyphs are ink-free: skip the quad (T1, matches
		// render_sign_text.c's identical space-skip) but still advance pen_x
		// by its width, exactly like Beta.
		if(ch != ' ') {
			if(count + 4 > NAME_TAG_MAX_TEXT_VERTS)
				break;

			float px0 = (float) pen_x;
			float py0 = 0.0f; // pen_y == 0: single-line label, no sleeping
							  // offset (not tracked -- see header).
			float px1 = px0 + GLYPH_CELL_PX;
			float py1 = py0 + GLYPH_CELL_PX;

			float tex_x = (float) (ch % 16) * FONT_ATLAS_CELL_PX;
			float tex_y = (float) (ch / 16) * FONT_ATLAS_CELL_PX;
			float u0 = tex_x / FONT_ATLAS_DIM_PX;
			float v0 = tex_y / FONT_ATLAS_DIM_PX;
			float u1 = (tex_x + FONT_ATLAS_CELL_PX) / FONT_ATLAS_DIM_PX;
			float v1 = (tex_y + FONT_ATLAS_CELL_PX) / FONT_ATLAS_DIM_PX;

			float local[4][3] = {
				{px0, py0, 0.0f},
				{px1, py0, 0.0f},
				{px1, py1, 0.0f},
				{px0, py1, 0.0f},
			};
			float uv[4][2] = {
				{u0, v0},
				{u1, v0},
				{u1, v1},
				{u0, v1},
			};

			for(int k = 0; k < 4; k++) {
				vec3 world;
				glm_mat4_mulv3(m, local[k], 1.0f, world);
				text_out[count].x = world[0];
				text_out[count].y = world[1];
				text_out[count].z = world[2];
				text_out[count].u = uv[k][0];
				text_out[count].v = uv[k][1];
				count++;
			}
		}

		pen_x += (int) w + 1;
	}

	return count;
}
