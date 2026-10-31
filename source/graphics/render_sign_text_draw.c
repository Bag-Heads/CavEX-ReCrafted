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

/*
	THIN GX/GL draw pass for render_sign_text.c's pure quad builder -- see
	that file for every decompile citation on the geometry itself. This file
	owns only: binding the right texture, submitting the quads through the
	shared gfx abstraction, and restoring state exactly like the established
	precedents below. It intentionally does NOT duplicate transform math.

	STATE-DISCIPLINE PRECEDENTS MIRRORED (two, for two different reasons):

	1. particle.c's particle_render -- the closest match for "a batch of
	   world-space textured quads with an explicit flat colour, submitted via
	   gfx_draw_quads_flt under one shared matrix for the whole batch":

			gfx_matrix_modelview(view);
			gfx_lighting(false);
			... per-particle gfx_draw_quads_flt(...) with absolute
			    world-space vertex positions (NOT a per-particle
			    gfx_matrix_modelview) ...
			gfx_lighting(true);

	   Mirrored 1:1 below: `view` is set ONCE for the whole sign batch (never
	   per-sign), and each sign's quads get its world (x,y,z) added directly
	   to the vertex positions build_quads already returns in the sign's own
	   BLOCK-LOCAL space -- avoiding a per-sign matrix upload entirely.
	   gfx_lighting(false) is required, not optional: gfx_draw_quads_flt only
	   feeds the shader's a_pos/a_color/a_texcoord attributes (see
	   resources/vertex.shader) -- a_light is never set on this path, so
	   leaving lighting ON would multiply the explicit black colour below by
	   whatever STALE a_light content the previous draw call left behind.

	2. render_item.c's render_item_flat (the held-item / dropped-item draw)
	   -- WHY gfx_lighting must be explicitly turned back on afterward, not
	   just left off: its own comment spells out that gfx_lighting(false)
	   leaves the GX vertex descriptor's CLR0 in DIRECT mode, but the very
	   next entity/world draw in the SAME per-frame pass submits an INDEXED
	   (GX_Color1x8) colour -- a stale DIRECT descriptor there desyncs the GX
	   FIFO and HANGS real hardware (this project's real-Wii piston/dropped-
	   item freeze class). This pass sits in exactly that same per-frame
	   entity/particle bracket (see main.c's insertion point, reported in the
	   porting task), so it restores gfx_lighting(true) before returning for
	   the identical reason, not just to match particle_render's end state.

	Depth-write bracket -- [740c583
	TileEntitySignRenderer.renderTileEntitySignAt]: `GL11.glDepthMask(false)`
	brackets the per-line drawString loop, `GL11.glDepthMask(true)` restores
	it right after. Text is depth-TESTED against the world (so it can't show
	through a block in front of the sign) but never WRITES depth (so two
	glyphs/lines that overlap in screen-space on a steep viewing angle can't
	self-occlude via a stray depth write). Mirrored via
	gfx_write_buffers(true, false, true) .. gfx_write_buffers(true, true,
	true) -- (true,true,true) is this codebase's own standing "back to
	normal" restore value after any depth-write-disabling pass (see
	world.c's two-phase water prepass or gfx_util.c's fog-plane draw for the
	same restore literal).

	Text colour -- [same method] `byte var13 = 0;` is drawString's colour arg
	for every line; FontRenderer.renderString decodes 0 as
	((0>>16&255)/255, (0>>8&255)/255, (0&255)/255) = solid BLACK, with alpha
	(0>>24&255)/255 == 0 explicitly forced to 1.0 by
	`if (var9 == 0.0F) var9 = 1.0F;`. gfx_draw_quads_flt has no separate
	glColor-style uniform, so that decoded (0,0,0,255) is passed here as a
	literal per-vertex colour array instead.
*/

#include <assert.h>
#include <string.h>

#include "../block/blocks_data.h"
#include "../platform/gfx.h"
#include "../platform/texture.h"
#include "gui_util.h"
#include "render_sign_text.h"
#include "render_sign_text_draw.h"

void render_sign_text_pass(mat4 view, sign_text_iter_fn iter, void* ctx) {
	assert(iter);

	const uint8_t* glyph_width = gutil_font_glyph_widths();

	struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
	bool any = false;

	int32_t x, y, z;
	uint8_t block_type;
	uint8_t meta;
	char grid[64];

	while(iter(ctx, &x, &y, &z, &block_type, &meta, grid)) {
		// Beta chooses the BlockSign variant by block id. Post yaw metadata
		// 0..15 overlaps the wall's 2..5 metadata, so metadata cannot decide.
		bool is_wall_sign = block_type == BLOCK_SIGN;

		size_t n = sign_text_build_quads(grid, meta, is_wall_sign,
										 glyph_width, verts,
										 SIGN_TEXT_MAX_VERTS);
		if(n == 0)
			continue; // blank sign -- draws nothing, costs nothing

		if(!any) {
			// Deferred lazy-init (see header comment): only paid for once
			// the frame has at least one sign with visible text.
			gfx_matrix_modelview(view);
			gfx_bind_texture(&texture_font);
			gfx_lighting(false);
			gfx_write_buffers(true, false, true);
			any = true;
		}

		float pos[SIGN_TEXT_MAX_VERTS * 3];
		float uv[SIGN_TEXT_MAX_VERTS * 2];
		uint8_t col[SIGN_TEXT_MAX_VERTS * 4];

		for(size_t i = 0; i < n; i++) {
			// Add the sign's world position directly to the block-local
			// vertex render_sign_text.c already returned -- see precedent
			// (1) above for why this is a vertex add, not a matrix upload.
			pos[i * 3 + 0] = verts[i].x + (float)x;
			pos[i * 3 + 1] = verts[i].y + (float)y;
			pos[i * 3 + 2] = verts[i].z + (float)z;
			uv[i * 2 + 0] = verts[i].u;
			uv[i * 2 + 1] = verts[i].v;
			// Solid black, alpha 1 -- see the "Text colour" citation above.
			col[i * 4 + 0] = 0;
			col[i * 4 + 1] = 0;
			col[i * 4 + 2] = 0;
			col[i * 4 + 3] = 255;
		}

		gfx_draw_quads_flt(n, pos, col, uv);
	}

	if(any) {
		gfx_write_buffers(true, true, true);
		gfx_lighting(true);
	}
}
