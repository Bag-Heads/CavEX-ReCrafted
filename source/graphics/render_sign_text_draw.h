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

#ifndef RENDER_SIGN_TEXT_DRAW_H
#define RENDER_SIGN_TEXT_DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "../cglm/cglm.h"

/*
	GX/GL-facing draw pass for render_sign_text.c's pure quad builder. Kept
	in its OWN translation unit -- not merged into render_sign_text.c -- so
	tests/test_sign_text_quads.c can link the pure geometry alone with zero
	GL/GX symbols to resolve and stay a true no-context golden test. This
	file is the one that calls gfx_bind_texture/gfx_draw_quads_flt/etc, so it
	needs the real platform/gfx backend (a live GL context on PC, GX on Wii)
	to link and run; see render_sign_text_draw.c's header comment for the
	full state-discipline citation.
*/

// Enumerates the next sign to draw. The CONTRACT (source/game/
// sign_text_store.h) only exposes a by-position lookup
// (sign_text_store_get(x,y,z,grid)), not an enumerator, so this pass takes
// an iterator instead of owning any position source itself. Implementations
// own their own "visible/nearby" culling policy (distance, frustum, chunk
// residency, ...) -- render_sign_text_pass just draws whatever this yields.
// Write x/y/z (the sign's world position), *block_type (BLOCK_SIGN_POST/63
// or BLOCK_SIGN/68), *meta (its raw block metadata), and out_grid (64 bytes,
// the sign_text_store.h grid convention), then return true for one more sign;
// return false once exhausted. ctx is the iterator's own state, opaque here.
typedef bool (*sign_text_iter_fn)(void* ctx, int32_t* x, int32_t* y,
								  int32_t* z, uint8_t* block_type,
								  uint8_t* meta,
								  char out_grid[64]);

// Draws every sign the iterator yields under `view` -- the SAME camera-view
// matrix particle_render/render_piston_moving_all/entities_client_render
// already use in main.c's frame (ordinary 1-unit-per-block world space, no
// chunk-mesh BLK_LEN fixed-point scaling). Binds texture_font and sets
// GX/GL state ONCE for the whole batch (deferred until the first sign that
// actually has non-blank text, so a signless/text-less frame costs nothing),
// then per sign: resolves is_wall_sign from the block id, builds its quads
// (render_sign_text.c), and adds the sign's world (x,y,z) directly to each
// vertex before submitting. See
// render_sign_text_draw.c's header comment for the exact precedent this
// mirrors (particle_render's gfx_matrix_modelview(view); gfx_lighting(false)
// ... gfx_lighting(true) bracket) -- restores lighting/depth-write state
// before returning so the following draws in the frame (world/GUI/weather)
// stay FIFO-safe on real hardware.
void render_sign_text_pass(mat4 view, sign_text_iter_fn iter, void* ctx);

#endif
