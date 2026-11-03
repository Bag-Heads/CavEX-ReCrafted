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

#include "../cglm/cglm.h"
#include "beta_text_color.h"
#include "render_sign_text.h"

/*
	[740c583 TileEntitySignRenderer.renderTileEntitySignAt] -- the transform
	chain this function bakes into every returned vertex, summarized in call
	order (OpenGL's glTranslatef/glRotatef/glScalef post-multiply the CURRENT
	matrix, exactly like cglm's glm_translate/glm_rotate_y/glm_scale used
	below -- see render_entity.c's held-item chain for the same idiom already
	established in this codebase):

		push the matrix;
		model scale s = 0.6666667F                  (SIGN_TEXT_MODEL_SCALE);
		post sign:
			translate (x+0.5F, y+0.75F*s, z+0.5F);
			yaw = (float)(meta*360) / 16.0F          (int multiply, THEN cast);
			rotate by -yaw about Y;
		wall sign:
			yaw = 0, or 180 for meta 2, or 90 for meta 4,
			or -90 for meta 5                         (Beta's RAW wall meta);
			translate (x+0.5F, y+0.75F*s, z+0.5F);
			rotate by -yaw about Y;
			translate (0.0F, -0.3125F, -0.4375F)       (wall-only, stays live);
		(end of the sign-kind cases)
		bind /item/sign.png;
		push the matrix;
		scale (s, -s, -s);
		draw the sign model                            (board+stick -- NOT
		pop the matrix;                                 ours; see header)
		text scale t = 0.016666668F * s;
		translate (0.0F, 0.5F*s, 0.07F*s)               (text anchor);
		scale (t, -t, t);
		depth writes off;
		for each of the 4 lines:
			draw it centred at x = -width/2, y = line*10 - 4*5, colour 0;
		depth writes on;
		colour back to white (1,1,1,1);
		pop the matrix.

	Two things worth flagging explicitly:

	1. The wall-only `glTranslatef(0, -0.3125, -0.4375)` sits OUTSIDE the
	   signModel's own push/pop bracket, so it is still on the matrix stack
	   when the FontRenderer text transform runs below it -- it applies to
	   the TEXT too, not just the board model. Easy to miss reading the
	   decompile quickly; this port applies it before the text-anchor
	   translate for wall signs (see wall_extra_translate below).

	2. Wall signs use Beta's raw metadata {2,3,4,5}. Subtracting 2 gives the
	   physical {+Z,-Z,+X,-X} sequence used by block_sign.c and
	   render_block.c; wall_rotate_deg[] is Beta's quoted
	   {180,0,90,-90} result in that same order.

	SignModel's own board+stick render (step marked "NOT ours" above) is
	skipped entirely here -- the board already renders through the ordinary
	chunk block-mesh path (render_block.c), so only the FontRenderer tail is
	ported: the text-anchor translate, the text scale, and the per-line
	drawString loop it wraps.
*/

// [740c583 TileEntitySignRenderer var16 table], indexed by Beta wall metadata
// minus 2: raw 2/3/4/5 -> 180/0/90/-90 degrees.
// 2026-08-20 IN-GAME CORRECTION (+180 on every entry). The paper derivation
// below reasoned from "which FACE the sign is mounted on" but Beta's rotate
// turns the sign's local +Z (where the text anchor's +0.07 forward offset
// points) toward the direction the board LOOKS -- the opposite of the face
// it is attached to. The first capture proved it: a legacy meta-4 (now raw
// Beta meta 2, SIDE_FRONT) sign put its glyphs at z = block_z + 0.109,
// i.e. BEHIND the board, and rendered a blank plank. Empirical capture is the
// authority for a render-facing value; the golden's wall cases were updated
// to match this table, not the other way round.
static const float wall_rotate_deg[4] = {180.0f, 0.0f, 90.0f, -90.0f};

// Beta's FontRenderer draws each glyph as an 8x8-unit quad in "font pixel
// space" (y increasing DOWNWARD, glyph top at the pen position) -- the exact
// convention source/graphics/gui_util.c's gutil_text_rgb already uses on
// screen (its `x,y .. x+scale,y+scale` quad at scale==8). Mirrored here so
// the same 16x16-cell texture_font atlas layout applies unchanged in world
// space; Beta's own raw FontRenderer uses 7.99F (a 1/100px shrink to dodge
// texture bleed) where gui_util already simplified to a clean 8 -- this
// module follows gui_util's simplification, not Beta's raw float, per the
// "mirror this engine's GUI font" design brief.
#define GLYPH_CELL_PX 8.0f
// Font atlas: 16x16 glyph grid, 16px cells (matches gui_util.c's
// `tex_x = ch%16*16; tex_y = ch/16*16;` and the /256.0F normalization
// gfx_draw_quads/gfx_draw_quads_flt's backends already apply to texcoords --
// see particle.c's identical `/256.0f` UV convention for a plain
// gfx_draw_quads_flt world-space textured quad).
#define FONT_ATLAS_CELL_PX 16.0f
#define FONT_ATLAS_DIM_PX 256.0f

static void build_model_matrix(uint8_t block_meta, bool is_wall_sign,
							   mat4 dest) {
	float f = SIGN_TEXT_MODEL_SCALE; // [var10] 0.6666667F

	// Outer translate: (x+0.5, y+0.75*f, z+0.5) -- BLOCK-LOCAL (the integer
	// world x/y/z origin is the caller's job to add, see render_sign_text.h),
	// so only the 0.5/0.75*f sub-block offset lives here.
	glm_translate_make(dest, (vec3) {0.5f, 0.75f * f, 0.5f});

	float rotate_deg;
	if(!is_wall_sign) {
		// [var11] (float)(meta*360) / 16.0F -- int multiply, THEN cast; meta
		// is the post block's 0..15 yaw, selected by block id at the caller.
		// Equals meta*22.5 exactly.
		rotate_deg = (float)((int)block_meta * 360) / 16.0f;
	} else {
		rotate_deg = wall_rotate_deg[(block_meta - 2) & 0x03];
	}

	// glRotatef(-var11 / -var12, 0, 1, 0)
	glm_rotate_y(dest, glm_rad(-rotate_deg), dest);

	if(is_wall_sign) {
		// Wall-only, OUTSIDE the (unported) signModel push/pop -- see the
		// header comment's point (1). Still live for the text transform.
		glm_translate(dest, (vec3) {0.0f, -0.3125f, -0.4375f});
		// No extra forward nudge (2026-10-31): the old 0.115 "clearance" for
		// a 0.1875-thick board was stale -- the meshed wall board was a
		// 1/16 slab flush on the wall, so the text floated ~0.16 block in
		// front of it on real Wii (hands-7fd0d1b ss_000032/44). The board is
		// now Beta's SignModel board (render_block.c, SIGN_WALL_BOARD_*),
		// whose front face sits at 1/16*s here, so Beta's own 0.07*s anchor
		// below lands the text on that face exactly as in Beta.
	}

	// Text anchor: glTranslatef(0.0F, 0.5F*var10, 0.07F*var10)
	glm_translate(dest, (vec3) {0.0f, 0.5f * f, 0.07f * f});

	// Text scale: var12 = 0.016666668F * var10; glScalef(var12,-var12,var12)
	float s = 0.016666668f * f;
	glm_scale(dest, (vec3) {s, -s, s});
}

// Trailing-space trim (the grid is right-space-padded to SIGN_TEXT_QUAD_COLS
// per row -- sign_text_store.h's convention). Returns the trimmed length
// (0..SIGN_TEXT_QUAD_COLS); 0 means the whole row is blank.
static int trimmed_line_len(const char* row) {
	for(int i = SIGN_TEXT_QUAD_COLS - 1; i >= 0; i--) {
		if(row[i] != ' ')
			return i + 1;
	}
	return 0;
}

size_t sign_text_build_quads(const char grid[64], uint8_t block_meta,
							 bool is_wall_sign, const uint8_t glyph_width[256],
							 struct sign_text_vertex* out, size_t cap) {
	assert(grid && glyph_width && out);

	mat4 m;
	build_model_matrix(block_meta, is_wall_sign, m);

	size_t count = 0;

	for(int line = 0; line < SIGN_TEXT_QUAD_LINES; line++) {
		const char* row = grid + line * SIGN_TEXT_QUAD_COLS;
		int trimmed_len = trimmed_line_len(row);

		if(trimmed_len == 0)
			continue; // blank line -- no glyphs, nothing to centre

		// [FontRenderer.getStringWidth]: sum of per-char charWidth, where
		// charWidth[ch] == glyph_width[ch]+1 in this engine's split
		// representation (see gui_util.c's `(font_char_width[ch]+1)` at
		// every draw call site); a colour code (0xA7 + selector) adds 0
		// [FontRenderer.java:190-191].
		int total_width
			= beta_text_width(row, (size_t)trimmed_len, glyph_width);

		// [TileEntitySignRenderer]: `-var17.getStringWidth(var15) / 2` --
		// Java int division truncates toward zero, matched by C's `/` on
		// signed ints (C99+, same truncation rule).
		int pen_x = -total_width / 2;
		// [TileEntitySignRenderer]: `var14*10 - var1.signText.length*5`,
		// signText.length is always 4 == SIGN_TEXT_QUAD_LINES.
		int pen_y = line * 10 - SIGN_TEXT_QUAD_LINES * 5;

		// [TileEntitySignRenderer.java:56-58] drawString colour 0 = black;
		// [FontRenderer.java:147-161] each 0xA7 + selector switches colour
		// for the rest of the line and draws neither char.
		uint8_t rgb[3] = {0, 0, 0};
		for(int i = 0; i < trimmed_len; i++) {
			if(beta_text_is_code(row, (size_t)i, (size_t)trimmed_len)) {
				beta_text_code_rgb(beta_text_code_index(row[i + 1]), false,
								   rgb);
				i++;
				continue;
			}
			unsigned char ch = (unsigned char)row[i];
			if(ch == (unsigned char)BETA_TEXT_SECTION)
				continue; // lone trailing 167: not in font.txt, draws nothing
			uint8_t w = glyph_width[ch];

			// Space glyphs are definitionally ink-free: skip the quad
			// (free/bit-exact vertex-count win, CLAUDE.md's T1 tier) but
			// still advance the pen by its width, exactly like Beta.
			if(ch != ' ') {
				if(count + 4 > cap)
					return count;

				float px0 = (float)pen_x;
				float py0 = (float)pen_y;
				float px1 = px0 + GLYPH_CELL_PX;
				float py1 = py0 + GLYPH_CELL_PX;

				float tex_x = (float)(ch % 16) * FONT_ATLAS_CELL_PX;
				float tex_y = (float)(ch / 16) * FONT_ATLAS_CELL_PX;
				float u0 = tex_x / FONT_ATLAS_DIM_PX;
				float v0 = tex_y / FONT_ATLAS_DIM_PX;
				float u1 = (tex_x + FONT_ATLAS_CELL_PX) / FONT_ATLAS_DIM_PX;
				float v1 = (tex_y + FONT_ATLAS_CELL_PX) / FONT_ATLAS_DIM_PX;

				// Corner order matches gui_util.c's gutil_text_rgb quad
				// (top-left, top-right, bottom-right, bottom-left) --
				// same GL_QUADS winding this engine's gfx_draw_quads(_flt)
				// backends expect everywhere else.
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
					out[count].x = world[0];
					out[count].y = world[1];
					out[count].z = world[2];
					out[count].u = uv[k][0];
					out[count].v = uv[k][1];
					out[count].r = rgb[0];
					out[count].g = rgb[1];
					out[count].b = rgb[2];
					count++;
				}
			}

			pen_x += (int)w + 1;
		}
	}

	return count;
}
