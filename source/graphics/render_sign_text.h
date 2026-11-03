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

#ifndef RENDER_SIGN_TEXT_H
#define RENDER_SIGN_TEXT_H

/*
	Pure geometry for Beta 1.7.3 in-world sign text --
	[740c583:1.7.3-LTS/src/minecraft/net/minecraft/src/TileEntitySignRenderer
	.java] renderTileEntitySignAt. NO GL/GX calls and NO texture binds live in
	this file -- every numeric literal below is quoted straight from that
	decompile (or derived from it, clearly marked where), so
	tests/test_sign_text_quads.c can assert the exact transform on the CPU
	with no rendering context at all.

	The GX/GL-facing pass that actually binds texture_font and submits these
	quads under a live matrix lives in the SIBLING file
	render_sign_text_draw.c/.h -- kept in a separate translation unit on
	purpose, so the golden test can link this file ALONE with zero GL/GX
	symbols to resolve. See render_sign_text_draw.c's header comment for the
	state-discipline precedent that file mirrors.

	Only the TEXT is built here. The sign's board (the wood-textured slab)
	already renders through the ordinary chunk block-mesh path
	(render_block.c's render_block_sign/sign_side_helper) -- this module adds
	the four lines of text render_block_sign never drew.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One glyph-quad corner. Position is BLOCK-LOCAL: relative to the sign
// block's integer world origin (x, y, z), NOT yet translated by it -- so a
// golden test needs no world position, and the real draw pass (render_
// sign_text_draw.c) adds exactly that one translate on top. This function
// bakes Beta's ENTIRE per-glyph transform chain (the outer post/wall
// translate+rotate, the wall-only extra translate, the text-anchor translate
// and the text scale -- see render_sign_text.c for the cited chain) into
// this position already, so what's left for the caller is one vector add.
//
// UV is normalized to [0,1] over texture_font's 256-texel addressing
// convention -- the SAME 16x16 glyph-cell atlas layout
// source/graphics/gui_util.c's gutil_text_rgb uses (tex_x = ch%16*16,
// tex_y = ch/16*16, 16px cells), so the draw pass binds the one texture_font
// and shares the GUI font pipeline's atlas layout exactly.
struct sign_text_vertex {
	float x, y, z;
	float u, v;
	// Glyph colour: Beta's sign base colour 0 (black) until a colour code
	// (0xA7 + selector, beta_text_color.h) changes it for the rest of the line.
	uint8_t r, g, b;
};

#define SIGN_TEXT_QUAD_LINES 4 // TileEntitySign.signText.length -- always 4
#define SIGN_TEXT_QUAD_COLS 16 // sign_text_store.h's SIGN_TEXT_COLS(16)
#define SIGN_TEXT_MAX_QUADS (SIGN_TEXT_QUAD_LINES * SIGN_TEXT_QUAD_COLS)
#define SIGN_TEXT_MAX_VERTS (SIGN_TEXT_MAX_QUADS * 4)

// [TileEntitySignRenderer.renderTileEntitySignAt] `float var10 = 0.6666667F;`
// -- exported so render_sign_text.c and the golden test cite the identical
// literal instead of two hand-copies drifting apart.
#define SIGN_TEXT_MODEL_SCALE 0.6666667f

// Wall-sign (block 68) board, in render_block.c's 1/256-block vertex units,
// BEFORE the wall yaw: x/z relative to the block centre, y relative to the
// block bottom. [TileEntitySignRenderer.renderTileEntitySignAt] wall branch:
// translate(0.5, 0.75*s, 0.5), rotate, translate(0, -0.3125, -0.4375), then
// scale(s, -s, -s) of SignModel's board addBox(-12,-14,-1, 24,12,2) (1/16
// units). So x = +-12/16*s, y = 0.75*s - 0.3125 + [2,14]/16*s,
// z = -0.4375 +- 1/16*s; times 256, rounded once (the post board's rounding):
// x [-128,128], y [69.33,197.33] -> [69,197], z [-122.67,-101.33] ->
// [-123,-101]. Shared with tests/test_sign_text_quads.c so the board and the
// text plane are pinned against each other.
#define SIGN_WALL_BOARD_X0 (-128)
#define SIGN_WALL_BOARD_X1 128
#define SIGN_WALL_BOARD_Y0 69
#define SIGN_WALL_BOARD_Y1 197
#define SIGN_WALL_BOARD_Z0 (-123)
#define SIGN_WALL_BOARD_Z1 (-101)

/*
	Build the world-space (block-local) glyph quads for one sign's persisted
	4-line text grid. Pure math: safe to call with no GL/GX context.

	grid: 64 bytes, SIGN_TEXT_QUAD_LINES(4) rows of SIGN_TEXT_QUAD_COLS(16)
	      right-space-padded columns -- sign_text_store.h's packed grid
	      convention (row r's text is grid[r*16 .. r*16+15]).
	block_meta: the raw metadata at this position: all four bits select the
	      freestanding post's 0..15 yaw; block 68 uses Beta's raw wall-facing
	      values 2..5.
	is_wall_sign: picks the decompile's if(post)/else branch -- passed
	      explicitly rather than re-derived from block_meta here, so this
	      stays pure math with no block-registry dependency.
	glyph_width: 256-entry ink-width table. glyph_width[ch] must be in the
	      SAME 8-unit space as gui_util.c's static font_char_width[ch] (the
	      glyph's actual pixel width measured off the bound font texture,
	      NOT including the +1 inter-character gap FontRenderer.charWidth
	      bakes in -- this function adds that +1 itself, exactly like
	      gui_util's `(font_char_width[ch]+1)*scale/8` call sites do at
	      scale==8). Pass gutil_font_glyph_widths() in production; the golden
	      test injects a synthetic table so it never needs a loaded
	      texture_font.
	out/cap: caller-owned vertex buffer (SIGN_TEXT_MAX_VERTS covers the worst
	      case -- 4 full 16-column lines of non-space glyphs) and its
	      capacity in vertices.

	Returns the number of vertices written (always a multiple of 4: one quad
	per NON-SPACE glyph, across all 4 lines, trailing spaces trimmed per line
	before centering -- a space still advances the pen by its width, exactly
	like Beta, it just never emits a quad, since a space glyph is definitionally
	ink-free; this is a free/bit-exact vertex-count win, not a visual change).
	Returns 0 for an all-blank grid, or once `cap` cannot fit one more quad
	(stops cleanly at the last quad that fit; never overflows `out`).
*/
size_t sign_text_build_quads(const char grid[64], uint8_t block_meta,
							 bool is_wall_sign, const uint8_t glyph_width[256],
							 struct sign_text_vertex* out, size_t cap);

#endif
