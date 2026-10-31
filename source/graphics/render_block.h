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

#ifndef RENDER_BLOCK_H
#define RENDER_BLOCK_H

#include <stdbool.h>
#include <stddef.h>

#include "../cglm/cglm.h"

// Step 7 interface split: this header needs only the block VALUE types
// (struct block_data/block_info, enum side, w_coord_t -- all in
// blocks_data.h) and the displaylist type; pulling blocks.h/world.h here
// formed the blocks<->render_block half of the include cycle.
#include "../block/blocks_data.h"
#include "../platform/displaylist.h"

#define TEX_OFFSET(x) ((x)*18 + 3)

void render_block_init(void);
struct client_session;
void render_block_cracks_owned(struct client_session* owner,
	struct block_data* blk, mat4 view, w_coord_t x, w_coord_t y, w_coord_t z);
void render_block_cracks(struct block_data* blk, mat4 view, w_coord_t x,
						 w_coord_t y, w_coord_t z);

size_t render_block_pressure_plate(struct displaylist* d,
								   struct block_info* this, enum side side,
								   struct block_info* it, uint8_t* vertex_light,
								   bool count_only);

size_t render_block_fence(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

size_t render_block_fence_always(struct displaylist* d, struct block_info* this,
								 enum side side, struct block_info* it,
								 uint8_t* vertex_light, bool count_only);

size_t render_block_stairs(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only);

size_t render_block_stairs_always(struct displaylist* d,
								  struct block_info* this, enum side side,
								  struct block_info* it, uint8_t* vertex_light,
								  bool count_only);

size_t render_block_layer(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

size_t render_block_cross(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

// Far-ring LOD silhouette skirt (one vertical side quad spanning a column-to-lower-
// neighbour surface drop). See render_block.c for the geometry/atlas-safety notes.
size_t render_block_lod_skirt(struct displaylist* d, int16_t x, int16_t y,
							  int16_t z, uint16_t width, uint16_t height,
							  uint8_t tex, enum side side,
							  const uint8_t* vertex_light, uint8_t luminance);

// T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md):
// far-ring geomip merged patch top quad. One SIDE_TOP quad covering an NxN group
// of skin columns instead of N^2 individual 1x1 quads. x,y,z = the quad's lower
// corner in 1/256-block units (chunk-local, same convention as
// render_block_lod_skirt); `size` = the square footprint's edge length in 1/256-
// block units. The atlas tile STRETCHES over the whole footprint (TILE_CLAMP,
// same as the skirt) rather than tiling -- the expected LOD blur trade at this
// distance, not a texture bug. `vertex_light` is flat (every one of the 4 corners
// the same byte, sampled once at the merge anchor) -- see the design doc for why
// this spike does not extend the per-corner AO array across a merged footprint.
// Returns the quad count (1) for the vertex tally. lod_skin geomip path only.
size_t render_block_lod_patch_top(struct displaylist* d, int16_t x, int16_t y,
								  int16_t z, uint16_t size, uint8_t tex,
								  const uint8_t* vertex_light, uint8_t luminance);

size_t render_block_tree2d(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

size_t render_block_fluid(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

size_t render_block_full(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

// DC-GRASS G2: alpha reference for the grass tile-38 overlay draws (chunk.c's
// overlay list, render_piston_moving.c's moving grass), passed to
// gfx_alpha_test_ref(). Beta cuts with glAlphaFunc(GL_GREATER, 0.1F) (client
// EntityRenderer.java:773): a fragment survives iff alpha > 0.1F, STRICTLY.
// The backends compare differently, so the ref is chosen per backend to make
// each backend's own comparison exactly Beta's (no platform file changes):
//  - PC (resources/fragment.shader): `discard if gl_FragColor.a < alpha_ref`,
//    the ref uploaded unchanged by glUniform1f (pc/gfx.c gfx_alpha_test_ref).
//    With ref = nextafterf(0.1F, 1.0F) (the next float32 above 0.1F) there is
//    no float32 strictly between 0.1F and ref, so `a < ref` <=> `a <= 0.1F`
//    and the survivor test is exactly a > 0.1F -- also for the FRACTIONAL
//    alpha linear/trilinear filtering produces (0.0995 and 0.1F fail).
//  - Wii (wii/gfx.c gfx_alpha_test_ref): GX_SetAlphaCompare(GX_GREATER,
//    (uint8_t)roundf(ref * 255.0f)). TEV compares the 8-bit alpha, so the
//    only representable form of Beta's rule is integer a > 25 (a/255 > 0.1
//    <=> a >= 26 <=> a > 25); ref = 25/255 converts to byte 25 explicitly.
//    Filtered alpha is quantised to 8 bits before this compare; the visual
//    effect of Wii filtering stays a hardware gate (G3 owns sampling).
// Pinned by tests/test_mesher_nongrass_equiv.c (boundary outcomes + the
// textual backend comparison lines).
#define RENDER_GRASS_OVERLAY_ALPHA_REF_PC 0x1.99999cp-4F /* nextafterf(0.1F, 1) */
#define RENDER_GRASS_OVERLAY_ALPHA_BYTE_WII 25
#define RENDER_GRASS_OVERLAY_ALPHA_REF_WII ((float)RENDER_GRASS_OVERLAY_ALPHA_BYTE_WII / 255.0F)
#ifdef PLATFORM_WII
#define RENDER_GRASS_OVERLAY_ALPHA_REF RENDER_GRASS_OVERLAY_ALPHA_REF_WII
#else
#define RENDER_GRASS_OVERLAY_ALPHA_REF RENDER_GRASS_OVERLAY_ALPHA_REF_PC
#endif

// DC-GRASS G2: full-cube face `side` of `this` with atlas cell `tex` instead
// of the block's own texture (grass tile-38 side overlay). Same geometry,
// light and UV layout as render_block_full; returns 1 (quads) either way.
size_t render_block_full_tex(struct displaylist* d, struct block_info* this,
							 enum side side, uint8_t* vertex_light,
							 uint8_t tex, bool count_only);

size_t render_block_furnace(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

size_t render_block_slab(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

size_t render_block_bed(struct displaylist* d, struct block_info* this,
						enum side side, struct block_info* it,
						uint8_t* vertex_light, bool count_only);

size_t render_block_fire(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

size_t render_block_farmland(struct displaylist* d, struct block_info* this,
							 enum side side, struct block_info* it,
							 uint8_t* vertex_light, bool count_only);

size_t render_block_crops(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

size_t render_block_torch(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

size_t render_block_button(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only);

size_t render_block_lever(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only);

size_t render_block_cactus(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only);

size_t render_block_portal(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only);

size_t render_block_rail(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

size_t render_block_redstone_wire(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

size_t render_block_repeater(struct displaylist* d, struct block_info* this,
							 enum side side, struct block_info* it,
							 uint8_t* vertex_light, bool count_only);

size_t render_block_ladder(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only);

size_t render_block_cake(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

size_t render_block_trapdoor(struct displaylist* d, struct block_info* this,
							 enum side side, struct block_info* it,
							 uint8_t* vertex_light, bool count_only);

size_t render_block_sign(struct displaylist* d, struct block_info* this,
							 enum side side, struct block_info* it,
							 uint8_t* vertex_light, bool count_only);

size_t render_block_door(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only);

#endif
