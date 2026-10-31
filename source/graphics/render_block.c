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

#include <math.h>
#include <stdint.h>

#include "../block/blocks.h"
#include "../block/bed_meta.h"
#include "../block/redstone_power.h"
#include "../chunk.h"
#include "../game/game_state.h"
#include "../platform/gfx.h"
#include "../platform/time.h"
#include "../util.h"
#include "render_block.h"
#include "render_sign_text.h" // SIGN_WALL_BOARD_* (wall sign board = text plane)
#include "flame_fx.h" // Beta's two TextureFlamesFX cells (fire-anim-0924/F1)
#include "../item/items.h" // direct include (was transitive via blocks.h pre-split)

#define BLK_LEN 256

static inline uint8_t MIN_U8(uint8_t a, uint8_t b) {
	return a < b ? a : b;
}

static inline uint8_t MAX_U8(uint8_t a, uint8_t b) {
	return a > b ? a : b;
}

// Single choke point for every renderBlock/renderBlockAlways texture lookup in
// this file. Honors block_info::texture_override (set only by the crack
// overlay, render_block_cracks below) instead of the file's ~30 call sites
// each swapping blocks[type]->getTextureIndex -- that GLOBAL vtable slot is
// also read by the mesher thread (chunk_mesher.c), so mutating it from the
// render thread was a cross-thread data race on live gameplay state, not just
// a style issue. blocks[] itself is never written here or anywhere in the
// crack path now.
static uint8_t block_texture_index(struct block_info* this, enum side side) {
	if(this->texture_override)
		return this->texture_override(this, side);
	return blocks[this->block->type]->getTextureIndex(this, side);
}

// Map a side-quad extent `d` (in 1/256-block units) to its atlas texel span,
// saturating at one 16-texel tile. Replaces MIN_U8(16, d/16): that truncated d/16
// to uint8_t *before* the min, so a full 16-block-tall LOD skirt (d = 16*256 = 4096
// -> (uint8_t)(4096/16)==(uint8_t)256==0 -> MIN_U8(16,0)==0) collapsed its V-span to
// a single-texel smear. TILE_CLAMP does the >=16 test in uint16_t, so 4096 clamps to
// 16 (stretch one tile, atlas-safe under GX_CLAMP). For every existing caller
// (d <= BLK_LEN = 256, so d/16 <= 16) it returns exactly the old value -> the lod=0
// mesh stays byte-identical (guarded by test_displaylist_vertex_equiv).
static inline uint8_t TILE_CLAMP(uint16_t d) {
	uint16_t tiles = d / 16;
	return tiles >= 16 ? 16 : (uint8_t)tiles;
}

static uint8_t level_table_0[16] = {
	0, 0, 0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12,
};

static uint8_t level_table_1[16] = {
	0, 0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
};

static uint8_t level_table_2[16] = {
	0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};

static inline uint8_t DIM_LIGHT(uint8_t l, uint8_t* table, bool shade_sides,
								uint8_t luminance) {
	return shade_sides ?
		(table[MAX_U8(l >> 4, luminance)] << 4) | table[l & 0x0F] :
		(MAX_U8(l >> 4, luminance) << 4) | (l & 0x0F);
}

static inline void render_block_side_adv_v2(
	struct displaylist* d, int16_t x, int16_t y, int16_t z, uint16_t width,
	uint16_t height, int16_t inset_bottom, int16_t inset_top, uint8_t tex_x,
	uint8_t tex_y, bool tex_flip_h, int tex_rotate, bool shade_sides,
	enum side side, const uint8_t* vertex_light, uint8_t luminance) {
	const uint8_t tex_coords[2][4][2] = {
		{
			{tex_x, tex_y},
			{tex_x + TILE_CLAMP(width), tex_y},
			{tex_x + TILE_CLAMP(width), tex_y + TILE_CLAMP(height)},
			{tex_x, tex_y + TILE_CLAMP(height)},
		},
		{
			{tex_x + TILE_CLAMP(width), tex_y},
			{tex_x, tex_y},
			{tex_x, tex_y + TILE_CLAMP(height)},
			{tex_x + TILE_CLAMP(width), tex_y + TILE_CLAMP(height)},
		},
	};

	switch(side) {
		case SIDE_LEFT: { // x minus
			displaylist_vertex(d, x + inset_bottom, y, z,
				DIM_LIGHT(vertex_light[8], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][1]);
			displaylist_vertex(d, x + inset_top, y + height, z,
				DIM_LIGHT(vertex_light[9], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][1]);
			displaylist_vertex(d, x + inset_top, y + height, z + width,
				DIM_LIGHT(vertex_light[10], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][1]);
			displaylist_vertex(d, x + inset_bottom, y, z + width,
				DIM_LIGHT(vertex_light[11], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][1]);
		} break;
		case SIDE_RIGHT: { // x positive
			displaylist_vertex(d, x - inset_bottom, y, z,
				DIM_LIGHT(vertex_light[12], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][1]);
			displaylist_vertex(d, x - inset_bottom, y, z + width,
				DIM_LIGHT(vertex_light[15], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][1]);
			displaylist_vertex(d, x - inset_top, y + height, z + width,
				DIM_LIGHT(vertex_light[14], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][1]);
			displaylist_vertex(d, x - inset_top, y + height, z,
				DIM_LIGHT(vertex_light[13], level_table_1, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][1]);
		} break;
		case SIDE_TOP: { // y positive
			displaylist_vertex(d, x, y, z,
				DIM_LIGHT(vertex_light[4], NULL, false, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][1]);
			displaylist_vertex(d, x + width, y, z,
				DIM_LIGHT(vertex_light[5], NULL, false, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][1]);
			displaylist_vertex(d, x + width, y, z + height,
				DIM_LIGHT(vertex_light[6], NULL, false, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][1]);
			displaylist_vertex(d, x, y, z + height,
				DIM_LIGHT(vertex_light[7], NULL, false, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][1]);
		} break;
		case SIDE_BOTTOM: { // y negative
			displaylist_vertex(d, x, y + inset_bottom, z,
				DIM_LIGHT(vertex_light[0], level_table_0, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][1]);
			displaylist_vertex(d, x, y + inset_top, z + height,
				DIM_LIGHT(vertex_light[3], level_table_0, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][1]);
			displaylist_vertex(d, x + width, y + inset_top, z + height,
				DIM_LIGHT(vertex_light[2], level_table_0, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][1]);
			displaylist_vertex(d, x + width, y + inset_bottom, z,
				DIM_LIGHT(vertex_light[1], level_table_0, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][1]);
		} break;
		case SIDE_FRONT: { // z minus
			displaylist_vertex(d, x, y, z + inset_bottom,
				DIM_LIGHT(vertex_light[16], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][1]);
			displaylist_vertex(d, x + width, y, z + inset_bottom,
				DIM_LIGHT(vertex_light[17], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][1]);
			displaylist_vertex(d, x + width, y + height, z + inset_top,
				DIM_LIGHT(vertex_light[18], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][1]);
			displaylist_vertex(d, x, y + height, z + inset_top,
				DIM_LIGHT(vertex_light[19], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][1]);
		} break;
		case SIDE_BACK: { // z positive
			displaylist_vertex(d, x, y, z - inset_bottom,
				DIM_LIGHT(vertex_light[20], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 3) % 4][1]);
			displaylist_vertex(d, x, y + height, z - inset_top,
				DIM_LIGHT(vertex_light[23], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 0) % 4][1]);
			displaylist_vertex(d, x + width, y + height, z - inset_top,
				DIM_LIGHT(vertex_light[22], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 1) % 4][1]);
			displaylist_vertex(d, x + width, y, z - inset_bottom,
				DIM_LIGHT(vertex_light[21], level_table_2, shade_sides, luminance),
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][0],
				tex_coords[tex_flip_h][(tex_rotate + 2) % 4][1]);
		} break;
		default: break;
	}
}

static inline void
render_block_side_adv(struct displaylist* d, int16_t x, int16_t y, int16_t z,
					  uint16_t width, uint16_t height, uint8_t tex_x,
					  uint8_t tex_y, bool tex_flip_h, int tex_rotate,
					  bool shade_sides, enum side side,
					  const uint8_t* vertex_light, uint8_t luminance) {
	render_block_side_adv_v2(d, x, y, z, width, height, 0, 0, tex_x, tex_y,
							 tex_flip_h, tex_rotate, shade_sides, side,
							 vertex_light, luminance);
}

static inline void render_block_side(struct displaylist* d, int16_t x,
									 int16_t y, int16_t z, int16_t yoffset,
									 uint16_t height, uint8_t tex,
									 uint8_t luminance, bool shade_sides,
									 uint16_t inset, bool tex_flip_h,
									 int tex_rotate, enum side side,
									 const uint8_t* vertex_light) {
	uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
	uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));

	switch(side) {
		case SIDE_LEFT: // x minus
			render_block_side_adv(
				d, x * BLK_LEN + inset, y * BLK_LEN + yoffset, z * BLK_LEN,
				BLK_LEN, height, tex_x, tex_y + (16 - height / 16), tex_flip_h,
				tex_rotate, shade_sides, SIDE_LEFT, vertex_light, luminance);
			break;
		case SIDE_RIGHT: // x positive
			render_block_side_adv(d, x * BLK_LEN + BLK_LEN - inset,
								  y * BLK_LEN + yoffset, z * BLK_LEN, BLK_LEN,
								  height, tex_x, tex_y + (16 - height / 16),
								  tex_flip_h, tex_rotate, shade_sides,
								  SIDE_RIGHT, vertex_light, luminance);
			break;
		case SIDE_BOTTOM: // y minus
			render_block_side_adv(d, x * BLK_LEN, y * BLK_LEN + yoffset + inset,
								  z * BLK_LEN, BLK_LEN, BLK_LEN, tex_x, tex_y,
								  tex_flip_h, tex_rotate, shade_sides,
								  SIDE_BOTTOM, vertex_light, luminance);
			break;
		case SIDE_TOP: // y positive
			render_block_side_adv(
				d, x * BLK_LEN, y * BLK_LEN + yoffset + height - inset,
				z * BLK_LEN, BLK_LEN, BLK_LEN, tex_x, tex_y, tex_flip_h,
				tex_rotate, shade_sides, SIDE_TOP, vertex_light, luminance);
			break;
		case SIDE_FRONT: // z minus
			render_block_side_adv(
				d, x * BLK_LEN, y * BLK_LEN + yoffset, z * BLK_LEN + inset,
				BLK_LEN, height, tex_x, tex_y + (16 - height / 16), tex_flip_h,
				tex_rotate, shade_sides, SIDE_FRONT, vertex_light, luminance);
			break;
		case SIDE_BACK: // z positive
			render_block_side_adv(d, x * BLK_LEN, y * BLK_LEN + yoffset,
								  z * BLK_LEN + BLK_LEN - inset, BLK_LEN,
								  height, tex_x, tex_y + (16 - height / 16),
								  tex_flip_h, tex_rotate, shade_sides,
								  SIDE_BACK, vertex_light, luminance);
			break;
		default: break;
	}
}

// Far-ring LOD silhouette skirt: ONE vertical quad on a horizontal side
// (LEFT/RIGHT/FRONT/BACK) filling the gap from a column's surface down to a lower
// neighbour's surface. x,y,z = the quad's lower corner in 1/256-block units; `height`
// may span many blocks (up to a full 16-tall section). `tex` = the surface block's
// SIDE-face atlas index. Routed through render_block_side_adv_v2 (inset 0) -- NOT
// render_block_side, whose tex_y + (16 - height/16) origin underflows the uint8 for a
// multi-block height. The V-span clamp-and-stretches one tile (TILE_CLAMP), atlas-safe
// under GX_CLAMP. Returns the quad count (1) for the vertex tally. lod_skin path only.
size_t render_block_lod_skirt(struct displaylist* d, int16_t x, int16_t y,
							  int16_t z, uint16_t width, uint16_t height,
							  uint8_t tex, enum side side,
							  const uint8_t* vertex_light, uint8_t luminance) {
	render_block_side_adv_v2(d, x, y, z, width, height, 0, 0,
							 TEX_OFFSET(TEXTURE_X(tex)), TEX_OFFSET(TEXTURE_Y(tex)),
							 false, 0, true, side, vertex_light, luminance);
	return 1;
}

// T2 SPIKE (opt-in, PC-measured only -- see render_block.h + the design doc). SIDE_TOP
// only, ignores shade_sides (SIDE_TOP's DIM_LIGHT call never reads it -- see
// render_block_side_adv_v2 above), so passing false here is inert either way; kept
// false for a literal reading match with the flat (non-AO) vertex_light this spike
// hands it.
size_t render_block_lod_patch_top(struct displaylist* d, int16_t x, int16_t y,
								  int16_t z, uint16_t size, uint8_t tex,
								  const uint8_t* vertex_light, uint8_t luminance) {
	render_block_side_adv_v2(d, x, y, z, size, size, 0, 0, TEX_OFFSET(TEXTURE_X(tex)),
							 TEX_OFFSET(TEXTURE_Y(tex)), false, 0, false, SIDE_TOP,
							 vertex_light, luminance);
	return 1;
}

size_t render_block_cross(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	if(side != SIDE_TOP)
		return 0;

	if(!count_only) {
		int16_t x = W2C_COORD(this->x) * BLK_LEN;
		int16_t y = W2C_COORD(this->y) * BLK_LEN;
		int16_t z = W2C_COORD(this->z) * BLK_LEN;

		if(blocks[this->block->type]
			   ->render_block_data.cross_random_displacement) {
			uint32_t seed
				= hash_u32(this->x) ^ hash_u32(this->y) ^ hash_u32(this->z);
			x += (seed & 0xFFFF) % 129 - 64;
			z += (seed >> 16) % 129 - 64;
		}

		uint8_t tex
			= block_texture_index(this, SIDE_TOP);
		uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
		uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
		uint8_t light = (MAX_U8(this->block->torch_light,
								blocks[this->block->type]->luminance)
						 << 4)
			| this->block->sky_light;

		displaylist_vertex(d, x, y, z,
			light,
			tex_x,
			tex_y + 16);
		displaylist_vertex(d, x, y + BLK_LEN, z,
			light,
			tex_x,
			tex_y);
		displaylist_vertex(d, x + BLK_LEN, y + BLK_LEN, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y);
		displaylist_vertex(d, x + BLK_LEN, y, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y + 16);

		displaylist_vertex(d, x + BLK_LEN, y, z,
			light,
			tex_x,
			tex_y + 16);
		displaylist_vertex(d, x + BLK_LEN, y + BLK_LEN, z,
			light,
			tex_x,
			tex_y);
		displaylist_vertex(d, x, y + BLK_LEN, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y);
		displaylist_vertex(d, x, y, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y + 16);
	}

	return 2;
}

size_t render_block_tree2d(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	if(side != SIDE_TOP)
		return 0;

	if(!count_only) {
		int16_t x = W2C_COORD(this->x) * BLK_LEN;
		int16_t y = W2C_COORD(this->y) * BLK_LEN;
		int16_t z = W2C_COORD(this->z) * BLK_LEN;

		if(blocks[this->block->type]
			   ->render_block_data.cross_random_displacement) {
			uint32_t seed
				= hash_u32(this->x) ^ hash_u32(this->y) ^ hash_u32(this->z);
			x += (seed & 0xFFFF) % 129 - 64;
			z += (seed >> 16) % 129 - 64;
		}

		uint8_t tex
			= block_texture_index(this, SIDE_TOP);
		uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
		uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
		uint8_t light = (MAX_U8(this->block->torch_light,
								blocks[this->block->type]->luminance)
						 << 4)
			| this->block->sky_light;

		int16_t tree_height = BLK_LEN*(6+2*(this->block->metadata & 1));

		displaylist_vertex(d, x, y, z,
			light,
			tex_x,
			tex_y + 16);
		displaylist_vertex(d, x, y + tree_height, z,
			light,
			tex_x,
			tex_y);
		displaylist_vertex(d, x + BLK_LEN, y + BLK_LEN, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y);
		displaylist_vertex(d, x + BLK_LEN, y, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y + 16);

		displaylist_vertex(d, x + BLK_LEN, y, z,
			light,
			tex_x,
			tex_y + 16);
		displaylist_vertex(d, x + BLK_LEN, y + tree_height, z,
			light,
			tex_x,
			tex_y);
		displaylist_vertex(d, x, y + tree_height, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y);
		displaylist_vertex(d, x, y, z + BLK_LEN,
			light,
			tex_x + 16,
			tex_y + 16);
	}

	return 2;
}

size_t render_block_torch(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	if(side == SIDE_BOTTOM)
		return 0;

	if(!count_only) {
		int16_t x = W2C_COORD(this->x);
		int16_t y = W2C_COORD(this->y);
		int16_t z = W2C_COORD(this->z);
		uint8_t tex = block_texture_index(this, side);
		uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
		uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
		uint8_t light = (MAX_U8(this->block->torch_light,
								blocks[this->block->type]->luminance)
						 << 4)
			| this->block->sky_light;

		int s1x, s1y; // wall offset
		int s2x, s2y; // layer shift
		int s3 = 48;  // y offset

		switch(this->block->metadata) {
			case 1:
				s1x = -128;
				s1y = 0;
				s2x = 104;
				s2y = 0;
				break;
			case 2:
				s1x = 128;
				s1y = 0;
				s2x = -104;
				s2y = 0;
				break;
			case 3:
				s1x = 0;
				s1y = -128;
				s2x = 0;
				s2y = 104;
				break;
			case 4:
				s1x = 0;
				s1y = 128;
				s2x = 0;
				s2y = -104;
				break;
			default: s1x = s1y = s2x = s2y = s3 = 0; break;
		}

		switch(side) {
			case SIDE_LEFT:
				displaylist_vertex(d, x * BLK_LEN + 112 + s1x, y * BLK_LEN + s3, z * BLK_LEN + s1y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x,
					tex_y + 16);
				displaylist_vertex(d, x * BLK_LEN + 112 + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + s1y + s2y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x,
					tex_y);
				displaylist_vertex(d, x * BLK_LEN + 112 + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + BLK_LEN + s1y + s2y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x + 16,
					tex_y);
				displaylist_vertex(d, x * BLK_LEN + 112 + s1x, y * BLK_LEN + s3, z * BLK_LEN + BLK_LEN + s1y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x + 16,
					tex_y + 16);
				break;
			case SIDE_RIGHT:
				displaylist_vertex(d, x * BLK_LEN + 144 + s1x, y * BLK_LEN + s3, z * BLK_LEN + s1y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x + 16,
					tex_y + 16);
				displaylist_vertex(d, x * BLK_LEN + 144 + s1x, y * BLK_LEN + s3, z * BLK_LEN + BLK_LEN + s1y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x,
					tex_y + 16);
				displaylist_vertex(d, x * BLK_LEN + 144 + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + BLK_LEN + s1y + s2y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x,
					tex_y);
				displaylist_vertex(d, x * BLK_LEN + 144 + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + s1y + s2y,
					DIM_LIGHT(light, level_table_1, true, 0),
					tex_x + 16,
					tex_y);
				break;
			case SIDE_BACK:
				displaylist_vertex(d, x * BLK_LEN + s1x, y * BLK_LEN + s3, z * BLK_LEN + 144 + s1y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x,
					tex_y + 16);
				displaylist_vertex(d, x * BLK_LEN + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + 144 + s1y + s2y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x,
					tex_y);
				displaylist_vertex(d, x * BLK_LEN + BLK_LEN + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + 144 + s1y + s2y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x + 16,
					tex_y);
				displaylist_vertex(d, x * BLK_LEN + BLK_LEN + s1x, y * BLK_LEN + s3, z * BLK_LEN + 144 + s1y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x + 16,
					tex_y + 16);
				break;
			case SIDE_FRONT:
				displaylist_vertex(d, x * BLK_LEN + s1x, y * BLK_LEN + s3, z * BLK_LEN + 112 + s1y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x + 16,
					tex_y + 16);
				displaylist_vertex(d, x * BLK_LEN + BLK_LEN + s1x, y * BLK_LEN + s3, z * BLK_LEN + 112 + s1y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x,
					tex_y + 16);
				displaylist_vertex(d, x * BLK_LEN + BLK_LEN + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + 112 + s1y + s2y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x,
					tex_y);
				displaylist_vertex(d, x * BLK_LEN + s1x + s2x, y * BLK_LEN + BLK_LEN + s3, z * BLK_LEN + 112 + s1y + s2y,
					DIM_LIGHT(light, level_table_2, true, 0),
					tex_x + 16,
					tex_y);
				break;
			case SIDE_TOP:
				displaylist_vertex(d, x * BLK_LEN + 112 + s1x + s2x * 10 / 16, y * BLK_LEN + 160 + s3, z * BLK_LEN + 112 + s1y + s2y * 10 / 16,
					light,
					tex_x + 7,
					tex_y + 6);
				displaylist_vertex(d, x * BLK_LEN + 144 + s1x + s2x * 10 / 16, y * BLK_LEN + 160 + s3, z * BLK_LEN + 112 + s1y + s2y * 10 / 16,
					light,
					tex_x + 9,
					tex_y + 6);
				displaylist_vertex(d, x * BLK_LEN + 144 + s1x + s2x * 10 / 16, y * BLK_LEN + 160 + s3, z * BLK_LEN + 144 + s1y + s2y * 10 / 16,
					light,
					tex_x + 9,
					tex_y + 8);
				displaylist_vertex(d, x * BLK_LEN + 112 + s1x + s2x * 10 / 16, y * BLK_LEN + 160 + s3, z * BLK_LEN + 144 + s1y + s2y * 10 / 16,
					light,
					tex_x + 7,
					tex_y + 8);
				break;
			default: break;
		}
	}

	return 1;
}

// LEVER RENDER v2 -- Beta render-type-12, base box included (740c583 RenderBlocks.renderBlockLever).
// Beta draws TWO pieces per lever: (1) a small cobblestone-textured mounting box via
// renderStandardBlock with overrideBlockTexture = Block.cobblestone.blockIndexInTexture, sized/
// positioned per orientation by the method's own local var10/var11/var12 (0.25/0.1875/0.1875 --
// NOT the same numbers as BlockLever.setBlockBoundsBasedOnState's larger click/collision box), and
// (2) the handle: a 0.125 x 0.625 x 0.125 (X x Y x Z) box, local origin at its own base (Y=0),
// textured with the lever's own tile (this block's getTextureIndex, Beta idx 96). The ON/OFF bit
// tilts the handle ~40 degrees (0.698132 rad, Vec3D.rotateAroundX) after nudging Z by -+0.0625;
// wall orientations (meta&7 1..4) additionally rotate it 90 degrees about X (turning the vertical
// stick into one that protrudes horizontally from the wall) then about Y by a per-wall multiple of
// 90 degrees; floor orientation 6 gets an extra 90-degree Y rotation vs. 5. Two of the handle's six
// faces (Beta's local Y-caps -- the visible tip) use UV sub-rect (7,6)-(9,8) in the tile; the other
// four (the shaft) use (7,6)-(9,16). Beta lights the WHOLE handle with one flat
// getBlockBrightness() call (no per-face directional shading) -- unlike the base box, which goes
// through the normal per-face-shaded small-box path (matches render_block_button/_pressure_plate).
//
// This engine calls renderBlock once per world-cardinal SIDE_*, so which of the handle's 6 local
// faces belongs to a given call is orientation-dependent; s_lever_handle_face_side[][] resolves it
// (verified by transforming all 8 corners through the exact formula above for BOTH on/off and every
// orientation: the ~40-degree tilt never moves a face into a different cardinal octant, so the
// mapping only depends on meta&7). Every orientation covers all 6 sides exactly once, and the base
// box (a proper 6-face box, same shape as render_block_button) also contributes one quad per side
// -- so this renderer returns exactly 2 quads for every side, unconditionally (see
// tests/test_redstone_render_count.c).
//
// NOT reproduced: Beta's exact per-vertex winding is followed as decompiled, but this port's own
// SIDE_* winding convention for the handle box's world-cardinal assignment was derived from Beta's
// math, not from an independent second source -- verify visually (any inside-out face would show as
// a hole from the expected viewing angle, per orientation, on/off).

// 8 local corners of the handle box (Beta 0.0625/0.625 units), base at local Y=0.
static const float s_lever_handle_corner[8][3] = {
	{-0.0625f, 0.0f, -0.0625f}, {0.0625f, 0.0f, -0.0625f},
	{0.0625f, 0.0f, 0.0625f}, {-0.0625f, 0.0f, 0.0625f},
	{-0.0625f, 0.625f, -0.0625f}, {0.0625f, 0.625f, -0.0625f},
	{0.0625f, 0.625f, 0.0625f}, {-0.0625f, 0.625f, 0.0625f},
};

// Beta's own per-face corner-index winding (RenderBlocks.renderBlockLever's var29 0..5 loop).
static const uint8_t s_lever_handle_face[6][4] = {
	{0, 1, 2, 3}, {7, 6, 5, 4}, {1, 0, 4, 5}, {2, 1, 5, 6}, {3, 2, 6, 7}, {0, 3, 7, 4},
};

// Which world SIDE_* each of the 6 handle faces lands on, indexed [meta&7][face]. Index 0 unused
// (meta&7 is 1..6). See the comment above render_block_lever for how this was derived.
static const enum side s_lever_handle_face_side[7][6] = {
	/*0*/ {SIDE_MAX, SIDE_MAX, SIDE_MAX, SIDE_MAX, SIDE_MAX, SIDE_MAX},
	/*1*/ {SIDE_LEFT, SIDE_RIGHT, SIDE_BOTTOM, SIDE_BACK, SIDE_TOP, SIDE_FRONT},
	/*2*/ {SIDE_RIGHT, SIDE_LEFT, SIDE_BOTTOM, SIDE_FRONT, SIDE_TOP, SIDE_BACK},
	/*3*/ {SIDE_FRONT, SIDE_BACK, SIDE_BOTTOM, SIDE_LEFT, SIDE_TOP, SIDE_RIGHT},
	/*4*/ {SIDE_BACK, SIDE_FRONT, SIDE_BOTTOM, SIDE_RIGHT, SIDE_TOP, SIDE_LEFT},
	/*5*/ {SIDE_BOTTOM, SIDE_TOP, SIDE_FRONT, SIDE_RIGHT, SIDE_BACK, SIDE_LEFT},
	/*6*/ {SIDE_BOTTOM, SIDE_TOP, SIDE_LEFT, SIDE_FRONT, SIDE_RIGHT, SIDE_BACK},
};

// Beta cobblestone-textured base box bounds, per meta&7, in 1/256-block units (index 0 unused).
// Derived from RenderBlocks.renderBlockLever's local var10=0.25F/var11=0.1875F/var12=0.1875F.
static const int16_t s_lever_base_box[7][3][2] = {
	/*0*/ {{0, 0}, {0, 0}, {0, 0}},
	/*1 -X wall*/ {{0, 48}, {64, 192}, {80, 176}},
	/*2 +X wall*/ {{208, 256}, {64, 192}, {80, 176}},
	/*3 -Z wall*/ {{80, 176}, {64, 192}, {0, 48}},
	/*4 +Z wall*/ {{80, 176}, {64, 192}, {208, 256}},
	/*5 floor*/ {{80, 176}, {0, 48}, {64, 192}},
	/*6 floor*/ {{64, 192}, {0, 48}, {80, 176}},
};

static inline void lever_rotate_x(float* y, float* z, float theta) {
	float c = cosf(theta), s = sinf(theta);
	float ny = *y * c + *z * s;
	float nz = *z * c - *y * s;
	*y = ny;
	*z = nz;
}

static inline void lever_rotate_y(float* x, float* z, float theta) {
	float c = cosf(theta), s = sinf(theta);
	float nx = *x * c + *z * s;
	float nz = *z * c - *x * s;
	*x = nx;
	*z = nz;
}

// One handle corner (block-local 0..1 float space), fully transformed per the decompile sequence
// cited above. `orient` is meta&7 (1..6), `on` is meta&8.
static void lever_handle_corner(int orient, bool on, int idx, float* ox, float* oy,
								float* oz) {
	float x = s_lever_handle_corner[idx][0];
	float y = s_lever_handle_corner[idx][1];
	float z = s_lever_handle_corner[idx][2];

	if(on) {
		z -= 0.0625f;
		lever_rotate_x(&y, &z, 0.69813174f);
	} else {
		z += 0.0625f;
		lever_rotate_x(&y, &z, -0.69813174f);
	}

	if(orient == 6)
		lever_rotate_y(&x, &z, 1.5707964f);

	if(orient < 5) {
		y -= 0.375f;
		lever_rotate_x(&y, &z, 1.5707964f);
		switch(orient) {
			case 3: lever_rotate_y(&x, &z, 3.1415927f); break;
			case 2: lever_rotate_y(&x, &z, 1.5707964f); break;
			case 1: lever_rotate_y(&x, &z, -1.5707964f); break;
			default: break; // 4: rotateY(0) -- no-op, matches the decompile
		}
		x += 0.5f;
		y += 0.5f;
		z += 0.5f;
	} else {
		x += 0.5f;
		y += 0.125f;
		z += 0.5f;
	}

	*ox = x;
	*oy = y;
	*oz = z;
}

static void render_lever_handle_face(struct displaylist* d, int16_t bx, int16_t by,
									 int16_t bz, int orient, bool on, int face,
									 uint8_t tex_x, uint8_t tex_y, uint8_t light) {
	uint8_t u0 = tex_x + 7, u1 = tex_x + 9;
	uint8_t v0 = tex_y + 6, v1 = tex_y + (face < 2 ? 8 : 16);
	// Beta's per-vertex UV order within the quad: (u0,v1) (u1,v1) (u1,v0) (u0,v0).
	static const uint8_t uv_u[4] = {0, 1, 1, 0};
	static const uint8_t uv_v[4] = {1, 1, 0, 0};

	for(int i = 0; i < 4; i++) {
		float fx, fy, fz;
		lever_handle_corner(orient, on, s_lever_handle_face[face][i], &fx, &fy, &fz);
		displaylist_vertex(d, bx + (int16_t)lroundf(fx * BLK_LEN),
			by + (int16_t)lroundf(fy * BLK_LEN), bz + (int16_t)lroundf(fz * BLK_LEN),
			light, uv_u[i] ? u1 : u0, uv_v[i] ? v1 : v0);
	}
}

static void render_lever_base_face(struct displaylist* d, struct block_info* this,
								   enum side side, const uint8_t* vertex_light) {
	int orient = this->block->metadata & 7;
	if(orient < 1 || orient > 6)
		orient = 5; // defensive: keep an out-of-range save value rendering something sane

	uint8_t tex = tex_atlas_lookup(TEXAT_COBBLESTONE);
	uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex)) + 1;
	uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex)) + 1;
	uint8_t luminance = blocks[this->block->type]->luminance;

	int16_t bx = W2C_COORD(this->x) * BLK_LEN;
	int16_t by = W2C_COORD(this->y) * BLK_LEN;
	int16_t bz = W2C_COORD(this->z) * BLK_LEN;
	int16_t x0 = s_lever_base_box[orient][0][0], x1 = s_lever_base_box[orient][0][1];
	int16_t y0 = s_lever_base_box[orient][1][0], y1 = s_lever_base_box[orient][1][1];
	int16_t z0 = s_lever_base_box[orient][2][0], z1 = s_lever_base_box[orient][2][1];

	switch(side) {
		case SIDE_LEFT:
			render_block_side_adv(d, bx + x0, by + y0, bz + z0, z1 - z0, y1 - y0, tex_x,
								  tex_y, false, 0, true, SIDE_LEFT, vertex_light, luminance);
			break;
		case SIDE_RIGHT:
			render_block_side_adv(d, bx + x1, by + y0, bz + z0, z1 - z0, y1 - y0, tex_x,
								  tex_y, false, 0, true, SIDE_RIGHT, vertex_light, luminance);
			break;
		case SIDE_TOP:
			render_block_side_adv(d, bx + x0, by + y1, bz + z0, x1 - x0, z1 - z0, tex_x,
								  tex_y, false, 0, true, SIDE_TOP, vertex_light, luminance);
			break;
		case SIDE_BOTTOM:
			render_block_side_adv(d, bx + x0, by + y0, bz + z0, x1 - x0, z1 - z0, tex_x,
								  tex_y, false, 0, true, SIDE_BOTTOM, vertex_light, luminance);
			break;
		case SIDE_FRONT:
			render_block_side_adv(d, bx + x0, by + y0, bz + z0, x1 - x0, y1 - y0, tex_x,
								  tex_y, false, 0, true, SIDE_FRONT, vertex_light, luminance);
			break;
		case SIDE_BACK:
			render_block_side_adv(d, bx + x0, by + y0, bz + z1, x1 - x0, y1 - y0, tex_x,
								  tex_y, false, 0, true, SIDE_BACK, vertex_light, luminance);
			break;
		default: break;
	}
}

size_t render_block_lever(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	int orient = this->block->metadata & 7;
	if(orient < 1 || orient > 6)
		orient = 5;
	bool on = (this->block->metadata & 8) != 0;

	int face = -1;
	for(int f = 0; f < 6; f++) {
		if(s_lever_handle_face_side[orient][f] == side) {
			face = f;
			break;
		}
	}

	if(!count_only) {
		render_lever_base_face(d, this, side, vertex_light);

		if(face >= 0) {
			int16_t bx = W2C_COORD(this->x) * BLK_LEN;
			int16_t by = W2C_COORD(this->y) * BLK_LEN;
			int16_t bz = W2C_COORD(this->z) * BLK_LEN;
			uint8_t tex = block_texture_index(this, side);
			uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
			uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
			uint8_t light = (MAX_U8(this->block->torch_light,
									blocks[this->block->type]->luminance)
							 << 4)
				| this->block->sky_light;
			render_lever_handle_face(d, bx, by, bz, orient, on, face, tex_x, tex_y,
									 light);
		}
	}

	// Base box: one quad, every side (a proper 6-face box). Handle: one quad on whichever side its
	// currently-oriented model face lands on -- always exactly one of the 6 per orientation (see
	// s_lever_handle_face_side), so this returns 2 unconditionally; the `face >= 0` guard only
	// protects count_only/emit lockstep if metadata were ever out of range.
	return 1 + (face >= 0 ? 1 : 0);
}

size_t render_block_cactus(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only) {
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			BLK_LEN, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true,
			(side == SIDE_TOP || side == SIDE_BOTTOM) ? 0 : 16, false, 0, side,
			vertex_light);
	return 1;
}

// Beta BlockPortal.shouldSideBeRendered(access, x, y, z, side) [decompile 740c583]:
//
// Portal neighbours hide the face immediately.
// Otherwise visibility depends on portal cells around the neighbour.
// Probe the four horizontal directions, in negative-X, positive-X,
// negative-Z, positive-Z order. Each direction qualifies only when
// its adjacent cell is portal and its second cell is not portal.
// Either qualifying X direction enables the two X-facing sides.
// Either qualifying Z direction enables the two Z-facing sides.
// The face numbers are Beta's horizontal side identifiers:
// 4 is negative X, and 5 is positive X;
// 2 is negative Z, and 3 is positive Z.
// Neither vertical side is enabled by these probes.
// A neighbour that is itself portal never reaches the probes.
// The same rules apply to malformed and isolated portal blocks.
// Keep the probe order and neighbour-relative coordinates intact.
// The implementation below retains these visibility rules.
// See the cited method for their source provenance.
//
// RenderBlocks calls this with its x/y/z args == the ALREADY-OFFSET neighbour cell (this
// port's `it->x/y/z`) and its side arg == the face of the CURRENT block that faces that neighbour
// (Beta side 2=-Z, 3=+Z, 4=-X, 5=+X -- this port's SIDE_FRONT/BACK/LEFT/RIGHT, per
// blocks_side_offset). Beta never branches on side 0/1 (top/bottom) here, so those fall
// through every check to false, matching this port's SIDE_TOP/SIDE_BOTTOM early-out.
//
// The four neighbour probes are evaluated AROUND THE NEIGHBOUR CELL, not around `this`. For a
// valid Beta portal frame (always exactly 2 wide x 3 tall, fixed by tryToCreatePortal) that
// makes the two edge flags' LEFT/RIGHT/FRONT/BACK-axis "frame edge" branches dead in practice: the
// neighbour-is-portal early-out already kills every interior face between adjacent portal
// cells, and every genuine frame-edge probe resolves to false too (the outer neighbour is
// obsidian, and stepping the OTHER two cells away lands back on the two frame columns, so the
// "== portal && 2-away != portal" pattern never holds along the WIDTH axis). Only the
// THICKNESS axis (the axis with no portal neighbours, 1 cell thick) reliably fires the edge flags
// true, because the neighbour's own "2-away" probe in that axis lands back on `this` -- so
// both big pane faces (front AND back) always render. Transcribed verbatim rather than
// simplified to "thickness axis only" so a malformed/non-frame portal cell (mid-break, or a
// script-placed lone block) degrades exactly like Beta's client would, not like our guess.
static bool portal_cell_is(struct block_info* this, w_coord_t x, w_coord_t y, w_coord_t z) {
	struct block_data b;
	return this->get_block && this->get_block(this->get_block_ctx, x, y, z, &b)
		&& b.type == BLOCK_PORTAL;
}

static bool portal_should_side_be_rendered(struct block_info* this, w_coord_t nx,
										   w_coord_t ny, w_coord_t nz, enum side side) {
	// `this->get_block` is NULL only on the world-less item/inventory-icon render path
	// (render_item.c's neighbour_blk is a bare BLOCK_AIR with no accessor) -- moot for portal
	// in practice, since BLOCK_PORTAL has no ItemBlock (no items[] entry, matching Beta's
	// undroppable/uncraftable id 90: item_is_block() is false for it, so that path never
	// actually dispatches here). Default to rendering FRONT/BACK only, mirroring the always-
	// visible thickness-axis pane faces derived above, rather than going fully invisible.
	if(!this->get_block)
		return side == SIDE_FRONT || side == SIDE_BACK;

	if(portal_cell_is(this, nx, ny, nz))
		return false;

	bool var6 = portal_cell_is(this, nx - 1, ny, nz) && !portal_cell_is(this, nx - 2, ny, nz);
	bool var7 = portal_cell_is(this, nx + 1, ny, nz) && !portal_cell_is(this, nx + 2, ny, nz);
	bool var8 = portal_cell_is(this, nx, ny, nz - 1) && !portal_cell_is(this, nx, ny, nz - 2);
	bool var9 = portal_cell_is(this, nx, ny, nz + 1) && !portal_cell_is(this, nx, ny, nz + 2);
	bool var10 = var6 || var7;
	bool var11 = var8 || var9;

	if(var10 && side == SIDE_LEFT)	 // Beta side 4 (-X)
		return true;
	if(var10 && side == SIDE_RIGHT) // Beta side 5 (+X)
		return true;
	if(var11 && side == SIDE_FRONT) // Beta side 2 (-Z)
		return true;
	return var11 && side == SIDE_BACK; // Beta side 3 (+Z)
}

size_t render_block_portal(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only) {
	if(side == SIDE_TOP || side == SIDE_BOTTOM)
		return 0;

	if(!portal_should_side_be_rendered(this, it->x, it->y, it->z, side))
		return 0;

	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			BLK_LEN, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true, 96, false, 0, side,
			vertex_light);
	return 1;
}

/* ---- Beta 1.7.3 fluid rendering -----------------------------------------

	Ground truth: RenderBlocks.renderBlockFluids / func_1224_a (decompile
	740c583, lines 1484-1663) and BlockFluid.getPercentAir / getFlowVector /
	getFlowDirection (lines 18-24, 98-177, 226-236). The old renderer used one
	height for all four corners, made sources 14/16 high and falling liquid a
	full block. These helpers retain Beta's raw metadata weighting and order.
*/
enum fluid_render_material {
	FLUID_RENDER_NONE,
	FLUID_RENDER_WATER,
	FLUID_RENDER_LAVA,
};

static enum fluid_render_material fluid_render_material_for(uint8_t type) {
	if(type == BLOCK_WATER_FLOW || type == BLOCK_WATER_STILL)
		return FLUID_RENDER_WATER;
	if(type == BLOCK_LAVA_FLOW || type == BLOCK_LAVA_STILL)
		return FLUID_RENDER_LAVA;
	return FLUID_RENDER_NONE;
}

static bool fluid_render_same_material(enum fluid_render_material material,
								   uint8_t type) {
	return material != FLUID_RENDER_NONE
		&& fluid_render_material_for(type) == material;
}

static bool fluid_render_read(struct block_info* this, w_coord_t x, w_coord_t y,
							  w_coord_t z, struct block_data* out) {
	if(x == this->x && y == this->y && z == this->z) {
		*out = *this->block;
		return true;
	}
	if(this->get_block
	   && this->get_block(this->get_block_ctx, x, y, z, out))
		return true;
	*out = (struct block_data) {0};
	return false;
}

static float fluid_render_percent_air(uint8_t metadata) {
	if(metadata >= 8)
		metadata = 0;
	return (float)(metadata + 1) / 9.0F;
}

static uint16_t fluid_render_height_units(float height) {
	if(height < 0.0F)
		height = 0.0F;
	if(height > 1.0F)
		height = 1.0F;
	return (uint16_t)(height * (float)BLK_LEN + 0.5F);
}

/* RenderBlocks.func_1224_a: four columns touch each surface corner. */
static uint16_t fluid_render_corner_height(struct block_info* this,
									   w_coord_t corner_x,
									   w_coord_t corner_z,
									   enum fluid_render_material material) {
	/* Item rendering has no world accessor; use the block's own Beta height. */
	if(!this->get_block)
		return fluid_render_height_units(
			1.0F - fluid_render_percent_air(this->block->metadata));

	int count = 0;
	float air = 0.0F;
	for(int i = 0; i < 4; i++) {
		w_coord_t sx = corner_x - (i & 1);
		w_coord_t sz = corner_z - ((i >> 1) & 1);
		struct block_data block;
		fluid_render_read(this, sx, this->y + 1, sz, &block);
		if(fluid_render_same_material(material, block.type))
			return BLK_LEN;

		fluid_render_read(this, sx, this->y, sz, &block);
		if(fluid_render_same_material(material, block.type)) {
			float percent_air = fluid_render_percent_air(block.metadata);
			if(block.metadata >= 8 || block.metadata == 0) {
				air += percent_air * 10.0F;
				count += 10;
			}
			air += percent_air;
			count++;
		} else if(!block_beta_material_is_solid(block.type)) {
			/* Beta counts a non-solid, non-fluid column as 100% air. */
			air += 1.0F;
			count++;
		}
	}

	/* The current fluid cell touches every one of its own four corners. */
	assert(count > 0);
	return fluid_render_height_units(1.0F - air / (float)count);
}

static int fluid_render_effective_decay(struct block_info* this, w_coord_t x,
									w_coord_t y, w_coord_t z,
									enum fluid_render_material material) {
	struct block_data block;
	fluid_render_read(this, x, y, z, &block);
	if(!fluid_render_same_material(material, block.type))
		return -1;
	return block.metadata >= 8 ? 0 : block.metadata;
}

static bool fluid_render_flow_angle(struct block_info* this,
									enum fluid_render_material material,
									float* angle) {
	if(!this->get_block)
		return false;

	int decay = fluid_render_effective_decay(
		this, this->x, this->y, this->z, material);
	if(decay < 0)
		return false;

	/* BlockFluid.getFlowVector order: -X, -Z, +X, +Z. */
	static const int dx[4] = {-1, 0, 1, 0};
	static const int dz[4] = {0, -1, 0, 1};
	int vx = 0, vz = 0;
	for(int i = 0; i < 4; i++) {
		w_coord_t nx = this->x + dx[i];
		w_coord_t nz = this->z + dz[i];
		int neighbour_decay = fluid_render_effective_decay(
			this, nx, this->y, nz, material);
		int delta;
		if(neighbour_decay < 0) {
			struct block_data neighbour;
			fluid_render_read(this, nx, this->y, nz, &neighbour);
			if(block_beta_material_is_solid(neighbour.type))
				continue;
			neighbour_decay = fluid_render_effective_decay(
				this, nx, this->y - 1, nz, material);
			if(neighbour_decay < 0)
				continue;
			delta = neighbour_decay - (decay - 8);
		} else {
			delta = neighbour_decay - decay;
		}
		vx += dx[i] * delta;
		vz += dz[i] * delta;
	}

	if(vx == 0 && vz == 0)
		return false;
	*angle = atan2f((float)vz, (float)vx) - 1.5707963267948966F;
	return true;
}

static uint8_t fluid_render_uv(float uv) {
	int rounded = (int)lroundf(uv);
	if(rounded < 0)
		rounded = 0;
	if(rounded > 255)
		rounded = 255;
	return (uint8_t)rounded;
}

static uint8_t fluid_render_side_v(uint8_t texture_v, uint16_t height) {
	unsigned from_top
		= ((unsigned)(BLK_LEN - height) * 16U + BLK_LEN / 2U) / BLK_LEN;
	return (uint8_t)(texture_v + from_top);
}

static void fluid_render_top(struct displaylist* d, struct block_info* this,
							 uint16_t h00, uint16_t h01, uint16_t h11,
							 uint16_t h10, uint8_t* vertex_light) {
	struct block* block = blocks[this->block->type];
	float angle = 0.0F;
	bool flowing = fluid_render_flow_angle(
		this, fluid_render_material_for(this->block->type), &angle);

	/*
		Beta selects the side texture for a moving top. Water's block callback
		maps that to anim.png's flow cell; the current asset architecture has no
		separate lava-flow filmstrip, so lava retains its still art but receives
		the same direction-aware UV rotation.
	*/
	uint8_t texture
		= block_texture_index(this, flowing ? SIDE_FRONT : SIDE_TOP);
	float center_u = (float)TEX_OFFSET(TEXTURE_X(texture)) + 8.0F;
	float center_v = (float)TEX_OFFSET(TEXTURE_Y(texture)) + 8.0F;
	float sin8 = 0.0F, cos8 = 8.0F;
	if(flowing) {
		float sin_angle = sinf(angle);
		float cos_angle = cosf(angle);
		/* Beta rotates an 8-texel-radius sample inside a dedicated 32x32 flow
		   texture. Cave's anim atlas has one 16x16 flow cell; the unscaled
		   diagonal corners would cross into adjacent animation cells. Fit the
		   rotated square to this cell (axis flows remain exactly Beta's +/-8),
		   preserving direction without atlas bleed. */
		float fit = 8.0F / (fabsf(sin_angle) + fabsf(cos_angle));
		sin8 = sin_angle * fit;
		cos8 = cos_angle * fit;
	}

	/* Beta corner order: 00, 01, 11, 10. */
	uint8_t uv[4][2] = {
		{fluid_render_uv(center_u - cos8 - sin8),
		 fluid_render_uv(center_v - cos8 + sin8)},
		{fluid_render_uv(center_u - cos8 + sin8),
		 fluid_render_uv(center_v + cos8 + sin8)},
		{fluid_render_uv(center_u + cos8 + sin8),
		 fluid_render_uv(center_v + cos8 - sin8)},
		{fluid_render_uv(center_u + cos8 - sin8),
		 fluid_render_uv(center_v - cos8 - sin8)},
	};

	int16_t x = W2C_COORD(this->x) * BLK_LEN;
	int16_t y = W2C_COORD(this->y) * BLK_LEN;
	int16_t z = W2C_COORD(this->z) * BLK_LEN;
	uint8_t luminance = block->luminance;
	/* Cave's top-face winding is Beta corners 00,10,11,01. */
	displaylist_vertex(d, x, y + h00, z,
		DIM_LIGHT(vertex_light[4], NULL, false, luminance), uv[0][0], uv[0][1]);
	displaylist_vertex(d, x + BLK_LEN, y + h10, z,
		DIM_LIGHT(vertex_light[5], NULL, false, luminance), uv[3][0], uv[3][1]);
	displaylist_vertex(d, x + BLK_LEN, y + h11, z + BLK_LEN,
		DIM_LIGHT(vertex_light[6], NULL, false, luminance), uv[2][0], uv[2][1]);
	displaylist_vertex(d, x, y + h01, z + BLK_LEN,
		DIM_LIGHT(vertex_light[7], NULL, false, luminance), uv[1][0], uv[1][1]);
}

static void fluid_render_horizontal_side(
	struct displaylist* d, struct block_info* this, enum side side,
	uint16_t h00, uint16_t h01, uint16_t h11, uint16_t h10,
	uint8_t* vertex_light) {
	struct block* block = blocks[this->block->type];
	uint8_t texture = block_texture_index(this, side);
	uint8_t u0 = TEX_OFFSET(TEXTURE_X(texture));
	uint8_t v0 = TEX_OFFSET(TEXTURE_Y(texture));
	uint8_t u1 = u0 + 16;
	uint8_t vb = v0 + 16;
	uint8_t v00 = fluid_render_side_v(v0, h00);
	uint8_t v01 = fluid_render_side_v(v0, h01);
	uint8_t v11 = fluid_render_side_v(v0, h11);
	uint8_t v10 = fluid_render_side_v(v0, h10);
	int16_t x = W2C_COORD(this->x) * BLK_LEN;
	int16_t y = W2C_COORD(this->y) * BLK_LEN;
	int16_t z = W2C_COORD(this->z) * BLK_LEN;
	uint8_t luminance = block->luminance;

	/* Preserve Cave's established face winding/light indices; only the two top
	   vertices and their V coordinates vary with the matching Beta corners. */
	switch(side) {
		case SIDE_LEFT:
			displaylist_vertex(d, x, y, z,
				DIM_LIGHT(vertex_light[8], level_table_1, true, luminance), u0, vb);
			displaylist_vertex(d, x, y + h00, z,
				DIM_LIGHT(vertex_light[9], level_table_1, true, luminance), u0, v00);
			displaylist_vertex(d, x, y + h01, z + BLK_LEN,
				DIM_LIGHT(vertex_light[10], level_table_1, true, luminance), u1, v01);
			displaylist_vertex(d, x, y, z + BLK_LEN,
				DIM_LIGHT(vertex_light[11], level_table_1, true, luminance), u1, vb);
			break;
		case SIDE_RIGHT:
			displaylist_vertex(d, x + BLK_LEN, y, z,
				DIM_LIGHT(vertex_light[12], level_table_1, true, luminance), u1, vb);
			displaylist_vertex(d, x + BLK_LEN, y, z + BLK_LEN,
				DIM_LIGHT(vertex_light[15], level_table_1, true, luminance), u0, vb);
			displaylist_vertex(d, x + BLK_LEN, y + h11, z + BLK_LEN,
				DIM_LIGHT(vertex_light[14], level_table_1, true, luminance), u0, v11);
			displaylist_vertex(d, x + BLK_LEN, y + h10, z,
				DIM_LIGHT(vertex_light[13], level_table_1, true, luminance), u1, v10);
			break;
		case SIDE_FRONT:
			displaylist_vertex(d, x, y, z,
				DIM_LIGHT(vertex_light[16], level_table_2, true, luminance), u1, vb);
			displaylist_vertex(d, x + BLK_LEN, y, z,
				DIM_LIGHT(vertex_light[17], level_table_2, true, luminance), u0, vb);
			displaylist_vertex(d, x + BLK_LEN, y + h10, z,
				DIM_LIGHT(vertex_light[18], level_table_2, true, luminance), u0, v10);
			displaylist_vertex(d, x, y + h00, z,
				DIM_LIGHT(vertex_light[19], level_table_2, true, luminance), u1, v00);
			break;
		case SIDE_BACK:
			displaylist_vertex(d, x, y, z + BLK_LEN,
				DIM_LIGHT(vertex_light[20], level_table_2, true, luminance), u0, vb);
			displaylist_vertex(d, x, y + h01, z + BLK_LEN,
				DIM_LIGHT(vertex_light[23], level_table_2, true, luminance), u0, v01);
			displaylist_vertex(d, x + BLK_LEN, y + h11, z + BLK_LEN,
				DIM_LIGHT(vertex_light[22], level_table_2, true, luminance), u1, v11);
			displaylist_vertex(d, x + BLK_LEN, y, z + BLK_LEN,
				DIM_LIGHT(vertex_light[21], level_table_2, true, luminance), u1, vb);
			break;
		default: break;
	}
}

size_t render_block_fluid(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	if(!this || !this->block || side < SIDE_TOP || side >= SIDE_MAX)
		return 0;
	enum fluid_render_material material
		= fluid_render_material_for(this->block->type);
	if(material == FLUID_RENDER_NONE)
		return 0;

	/* BlockFluid.shouldSideBeRendered compares material (moving + still share
	   one material), explicitly suppresses ice, then delegates opaque culling. */
	if(it && (fluid_render_same_material(material, it->block->type)
			  || it->block->type == BLOCK_ICE))
		return 0;
	if(count_only)
		return 1;

	uint16_t h00 = fluid_render_corner_height(this, this->x, this->z, material);
	uint16_t h01
		= fluid_render_corner_height(this, this->x, this->z + 1, material);
	uint16_t h11
		= fluid_render_corner_height(this, this->x + 1, this->z + 1, material);
	uint16_t h10
		= fluid_render_corner_height(this, this->x + 1, this->z, material);

	if(side == SIDE_TOP) {
		fluid_render_top(d, this, h00, h01, h11, h10, vertex_light);
	} else if(side == SIDE_BOTTOM) {
		struct block* block = blocks[this->block->type];
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			BLK_LEN, block_texture_index(this, SIDE_BOTTOM), block->luminance,
			true, 0, false, 0, SIDE_BOTTOM, vertex_light);
	} else {
		fluid_render_horizontal_side(
			d, this, side, h00, h01, h11, h10, vertex_light);
	}
	return 1;
}


size_t render_block_rail(struct displaylist* dl,
                         struct block_info* this,
                         enum side side,
                         struct block_info* it,
                         uint8_t* vertex_light,
                         bool count_only) {
    if (side != SIDE_TOP) return 0;
    if (count_only)       return 1;

    int16_t x = W2C_COORD(this->x);
    int16_t y = W2C_COORD(this->y);
    int16_t z = W2C_COORD(this->z);

    uint8_t tex       = block_texture_index(this, side);
    uint8_t luminance = blocks[this->block->type]->luminance;

    // Powered/detector rails carry their redstone state in bit 0x8: a POWERED SLOPE is
    // raw meta 10-13, which fails both the slope (2..5) and curve (6..9) tests below and
    // used to render FLAT. Beta strips it in the renderer -- RenderBlocks.
    // renderBlockMinecartTrack masks metadata to three bits for BOTH powered rail
    // ids -- AFTER the texture lookup, which is why `tex` above is computed from the raw
    // metadata (the ON/OFF selector needs the bit) and only `shape` is stripped here.
    // [740c583: RenderBlocks.java:1188-1195; BlockRail getIsPowered]
    uint8_t shape     = this->block->metadata & 0xF;
    if (this->block->type == BLOCK_POWERED_RAIL
        || this->block->type == BLOCK_DETECTOR_RAIL)
        shape &= 0x7;
    bool    is_slope  = (shape >= 2 && shape <= 5);
    int     tex_rotate;

    if (shape == 0) {
        // NS
        tex_rotate = 0;
    } else if (shape == 1) {
        // EW
        tex_rotate = 1;
    } else if (is_slope) {
        // slopes: X‐slopes (2,3) 90°
        tex_rotate = (shape == 2 || shape == 3) ? 1 : 0;
    } else {
        // curves 6–9: map 6->0°, 7->270°, 8->180°, 9->90°
    	// `(shape-6)&3` is in [0..3], subtract from 4 then mod 4
    	tex_rotate = (4 - ((shape - 6) & 3)) & 3;
    }

    bool is_curve = (shape >= 6 && shape <= 9);

    if (is_curve && blocks[this->block->type]->render_block_data.rail_curved_possible) {
        tex = tex_atlas_lookup(TEXAT_RAIL_CURVED);
    }

    uint8_t tx = TEX_OFFSET(TEXTURE_X(tex));
    uint8_t ty = TEX_OFFSET(TEXTURE_Y(tex));
    uint8_t uv[4][2] = {
        { tx,      ty      },
        { tx + 16, ty      },
        { tx + 16, ty + 16 },
        { tx,      ty + 16 },
    };

    // vertex heights: flat or sloped (16 = flat, 272 = 16+256)
    uint16_t h[4] = {16, 16, 16, 16};
    if (is_slope) {
        switch (shape) {
            case 2: h[1] = h[2] = 272; break;
            case 3: h[0] = h[3] = 272; break;
            case 4: h[0] = h[1] = 272; break;
            case 5: h[2] = h[3] = 272; break;
        }
    }

    for (int i = 0; i < 4; i++) {
        int vi = (tex_rotate + i) & 3;
        displaylist_vertex(dl, x * BLK_LEN + ((i == 1 || i == 2) ? BLK_LEN : 0), y * BLK_LEN + h[i], z * BLK_LEN + ((i >= 2) ? BLK_LEN : 0),
        	DIM_LIGHT(vertex_light[4 + i], NULL, false, luminance),
        	uv[vi][0],
        	uv[vi][1]);
    }

    return 1;
}

// ---- Beta 1.7.3 redstone-wire rendering (RenderBlocks.renderBlockRedstoneWire, decompile
//      740c583; connection logic == BlockRedstoneWire, cross-checked vs mc173/power.rs). ----
//
// The dust is a flat quad on its cell floor that takes one of three shapes by its 4 horizontal
// connections (Beta's shape code): a CROSS tile (4,10) for junctions / T's / L-corners (trimmed inward
// from each unconnected edge) and the isolated "dot"; or a LINE tile (5,10) run East-West
// (shape 1) or North-South (shape 2, UV rotated 90 deg). Where a horizontal neighbour is a
// solid cube carrying wire on top, the dust also climbs that cube's wall as a vertical LINE quad.
//
// Power 0..15 picks a colour-baked tile -- the display-list vertex carries no per-vertex RGB, so
// Beta's per-vertex power tint is pre-baked into 16 LINE + 16 CROSS atlas tiles (texture_atlas.c).
// Connections come from redstone_wire_connections() -- the SAME code the power model runs (Beta
// side flags == the renderer's side flags) -- over a world view backed by this->get_block (the mesher's `bd`
// snapshot on the worker thread, or world_get_block on the main-thread cracks overlay).
//
// NOT replicated: Beta's second, full-bright quad pass (UV V += one tile). It samples the terrain
// tile directly BELOW the dust -- blank (transparent) in vanilla/Isabella/Beta (tiles 180/181) --
// AND that "tile below" relationship does not survive our repacked atlas. It is a vanilla no-op we
// correctly omit rather than sample an unrelated packed tile.

// Render-only geometry epsilons (1 block = BLK_LEN = 256 units).
#define WIRE_FLAT_H 16	   // flat dust height above the cell floor (project value, anti-z-fight)
#define WIRE_TRIM 80	   // CROSS edge trim, Beta 0.3125 block = 5/16 * 256
#define WIRE_FACE_INSET 4  // vertical-face inset from the wall, Beta 0.015625 * 256
#define WIRE_FACE_TOP 6	   // vertical-face top overshoot, Beta 0.021875 * 256 (~5.6)

// World view over block_info.get_block, feeding the shared Beta connection probe.
static bool wire_view_get(struct block_info* bi, int x, int y, int z, struct block_data* out) {
	return bi->get_block && bi->get_block(bi->get_block_ctx, x, y, z, out);
}
static int wire_view_type(void* c, int x, int y, int z) {
	struct block_data b;
	return wire_view_get((struct block_info*)c, x, y, z, &b) ? b.type : -1;
}
static int wire_view_meta(void* c, int x, int y, int z) {
	struct block_data b;
	return wire_view_get((struct block_info*)c, x, y, z, &b) ? (b.metadata & 0x0F) : 0;
}
static bool wire_view_normal_cube(void* c, int x, int y, int z) {
	struct block_data b;
	if(!wire_view_get((struct block_info*)c, x, y, z, &b))
		return false;
	return block_is_normal_cube(b.type);
}

// True when getTextureIndex was swapped to a breaking-overlay tile (the block-cracks path):
// there the dust shape is irrelevant, so we draw one flat quad with the break tile.
static bool wire_tex_is_break(uint8_t tex) {
	for(int i = 0; i < 10; i++)
		if(tex == tex_atlas_lookup(TEXAT_BREAK_0 + i))
			return true;
	return false;
}

size_t render_block_redstone_wire(struct displaylist* dl, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only) {
	(void)it;
	if(side != SIDE_TOP)
		return 0;

	uint8_t power = this->block->metadata & 0x0F;
	uint8_t luminance = blocks[this->block->type]->luminance;

	// Cracks overlay: getTextureIndex returns a break tile; draw the simple flat quad below.
	uint8_t override_tex = block_texture_index(this, side);
	bool cracks = wire_tex_is_break(override_tex);

	// Connections (Beta var26-29): conn[0..3] = west(-X)/east(+X)/north(-Z)/south(+Z).
	bool conn[4] = {false, false, false, false};
	if(!cracks) {
		struct redstone_power_world w = {
			.ctx = this,
			.get_type = wire_view_type,
			.get_meta = wire_view_meta,
			.is_normal_cube = wire_view_normal_cube,
		};
		redstone_wire_connections(&w, this->x, this->y, this->z, conn);
	}
	bool cw = conn[0], ce = conn[1], cn = conn[2], cs = conn[3];

	// Beta var35: 0 = cross (default), 1 = line E-W, 2 = line N-S.
	int shape = 0;
	if((cw || ce) && !cn && !cs)
		shape = 1;
	if((cn || cs) && !ce && !cw)
		shape = 2;

	// Vertical "climbing" faces: only when this wire's ceiling is open, on each horizontal side
	// that is a solid cube carrying wire on top (Beta's per-side vertical-quad condition).
	bool vface[4] = {false, false, false, false};
	int nvface = 0;
	if(!cracks && !wire_view_normal_cube(this, this->x, this->y + 1, this->z)) {
		static const int dx[4] = {-1, +1, 0, 0};
		static const int dz[4] = {0, 0, -1, +1};
		struct block_data above;
		for(int i = 0; i < 4; i++) {
			int nx = this->x + dx[i], nz = this->z + dz[i];
			if(wire_view_normal_cube(this, nx, this->y, nz)
			   && wire_view_get(this, nx, this->y + 1, nz, &above)
			   && above.type == BLOCK_REDSTONE_WIRE) {
				vface[i] = true;
				nvface++;
			}
		}
	}

	size_t quads = 1 + (size_t)nvface;
	if(count_only)
		return quads;

	int16_t x = W2C_COORD(this->x);
	int16_t y = W2C_COORD(this->y);
	int16_t z = W2C_COORD(this->z);

	// Tile selection: CROSS for var35==0, LINE for 1/2; power-coloured (or the break override).
	enum tex_atlas_entry line_e = (power == 0) ? TEXAT_REDSTONE_WIRE_OFF
											   : TEXAT_REDSTONE_WIRE_L1 + (power - 1);
	enum tex_atlas_entry cross_e = (power == 0) ? TEXAT_REDSTONE_WIRE_CROSS_OFF
												: TEXAT_REDSTONE_WIRE_CROSS_L1 + (power - 1);
	uint8_t tex_line = cracks ? override_tex : tex_atlas_lookup(line_e);
	uint8_t tex_cross = cracks ? override_tex : tex_atlas_lookup(cross_e);
	uint8_t tex_top = (shape == 0) ? tex_cross : tex_line;

	int16_t X0 = x * BLK_LEN, X1 = x * BLK_LEN + BLK_LEN;
	int16_t Z0 = z * BLK_LEN, Z1 = z * BLK_LEN + BLK_LEN;
	int16_t YF = y * BLK_LEN + WIRE_FLAT_H;

	uint8_t u0 = TEX_OFFSET(TEXTURE_X(tex_top));
	uint8_t v0 = TEX_OFFSET(TEXTURE_Y(tex_top));
	uint8_t u1 = u0 + 16, v1 = v0 + 16;

	if(shape == 0) {
		// CROSS: trim each UNCONNECTED edge inward 5/16 block with a matching 5-texel UV inset,
		// but only when at least one connection exists (an isolated dot keeps the full tile).
		// Beta var31/32/33/34 (geometry) + var15/17/19/21 (UV).
		int16_t xw = X0, xe = X1, zn = Z0, zs = Z1;
		uint8_t uw = u0, ue = u1, vn = v0, vs = v1;
		if(cw || ce || cn || cs) {
			if(!cw) { xw += WIRE_TRIM; uw += 5; }
			if(!ce) { xe -= WIRE_TRIM; ue -= 5; }
			if(!cn) { zn += WIRE_TRIM; vn += 5; }
			if(!cs) { zs -= WIRE_TRIM; vs -= 5; }
		}
		// Winding (minX,minZ)->(maxX,minZ)->(maxX,maxZ)->(minX,maxZ); X->U, Z->V.
		displaylist_vertex(dl, xw, YF, zn, DIM_LIGHT(vertex_light[4], NULL, false, luminance), uw, vn);
		displaylist_vertex(dl, xe, YF, zn, DIM_LIGHT(vertex_light[5], NULL, false, luminance), ue, vn);
		displaylist_vertex(dl, xe, YF, zs, DIM_LIGHT(vertex_light[6], NULL, false, luminance), ue, vs);
		displaylist_vertex(dl, xw, YF, zs, DIM_LIGHT(vertex_light[7], NULL, false, luminance), uw, vs);
	} else if(shape == 1) {
		// LINE E-W: full tile, default orientation (X->U, Z->V).
		displaylist_vertex(dl, X0, YF, Z0, DIM_LIGHT(vertex_light[4], NULL, false, luminance), u0, v0);
		displaylist_vertex(dl, X1, YF, Z0, DIM_LIGHT(vertex_light[5], NULL, false, luminance), u1, v0);
		displaylist_vertex(dl, X1, YF, Z1, DIM_LIGHT(vertex_light[6], NULL, false, luminance), u1, v1);
		displaylist_vertex(dl, X0, YF, Z1, DIM_LIGHT(vertex_light[7], NULL, false, luminance), u0, v1);
	} else {
		// LINE N-S: full tile, UV rotated 90 deg (Z->U, X->V) -- Beta var35==2.
		displaylist_vertex(dl, X0, YF, Z0, DIM_LIGHT(vertex_light[4], NULL, false, luminance), u0, v0);
		displaylist_vertex(dl, X1, YF, Z0, DIM_LIGHT(vertex_light[5], NULL, false, luminance), u0, v1);
		displaylist_vertex(dl, X1, YF, Z1, DIM_LIGHT(vertex_light[6], NULL, false, luminance), u1, v1);
		displaylist_vertex(dl, X0, YF, Z1, DIM_LIGHT(vertex_light[7], NULL, false, luminance), u1, v0);
	}

	// Vertical climbing faces (always the LINE tile). Span the neighbour wall from this wire's
	// floor to the upper wire (Beta var3+0 .. var3+1+0.021875); inset 0.015625 from the wall.
	if(nvface) {
		uint8_t lu0 = TEX_OFFSET(TEXTURE_X(tex_line));
		uint8_t lv0 = TEX_OFFSET(TEXTURE_Y(tex_line));
		uint8_t lu1 = lu0 + 16, lv1 = lv0 + 16;
		int16_t yb = y * BLK_LEN;
		int16_t yt = y * BLK_LEN + BLK_LEN + WIRE_FACE_TOP;
		if(vface[0]) { // West (-X) wall, Beta var2+0.015625
			int16_t xp = X0 + WIRE_FACE_INSET;
			displaylist_vertex(dl, xp, yt, Z1, DIM_LIGHT(vertex_light[4], NULL, false, luminance), lu1, lv0);
			displaylist_vertex(dl, xp, yb, Z1, DIM_LIGHT(vertex_light[5], NULL, false, luminance), lu0, lv0);
			displaylist_vertex(dl, xp, yb, Z0, DIM_LIGHT(vertex_light[6], NULL, false, luminance), lu0, lv1);
			displaylist_vertex(dl, xp, yt, Z0, DIM_LIGHT(vertex_light[7], NULL, false, luminance), lu1, lv1);
		}
		if(vface[1]) { // East (+X) wall, Beta var2+1-0.015625
			int16_t xp = X1 - WIRE_FACE_INSET;
			displaylist_vertex(dl, xp, yb, Z1, DIM_LIGHT(vertex_light[4], NULL, false, luminance), lu0, lv1);
			displaylist_vertex(dl, xp, yt, Z1, DIM_LIGHT(vertex_light[5], NULL, false, luminance), lu1, lv1);
			displaylist_vertex(dl, xp, yt, Z0, DIM_LIGHT(vertex_light[6], NULL, false, luminance), lu1, lv0);
			displaylist_vertex(dl, xp, yb, Z0, DIM_LIGHT(vertex_light[7], NULL, false, luminance), lu0, lv0);
		}
		if(vface[2]) { // North (-Z) wall, Beta var4+0.015625
			int16_t zp = Z0 + WIRE_FACE_INSET;
			displaylist_vertex(dl, X1, yb, zp, DIM_LIGHT(vertex_light[4], NULL, false, luminance), lu0, lv1);
			displaylist_vertex(dl, X1, yt, zp, DIM_LIGHT(vertex_light[5], NULL, false, luminance), lu1, lv1);
			displaylist_vertex(dl, X0, yt, zp, DIM_LIGHT(vertex_light[6], NULL, false, luminance), lu1, lv0);
			displaylist_vertex(dl, X0, yb, zp, DIM_LIGHT(vertex_light[7], NULL, false, luminance), lu0, lv0);
		}
		if(vface[3]) { // South (+Z) wall, Beta var4+1-0.015625
			int16_t zp = Z1 - WIRE_FACE_INSET;
			displaylist_vertex(dl, X1, yt, zp, DIM_LIGHT(vertex_light[4], NULL, false, luminance), lu1, lv0);
			displaylist_vertex(dl, X1, yb, zp, DIM_LIGHT(vertex_light[5], NULL, false, luminance), lu0, lv0);
			displaylist_vertex(dl, X0, yb, zp, DIM_LIGHT(vertex_light[6], NULL, false, luminance), lu0, lv1);
			displaylist_vertex(dl, X0, yt, zp, DIM_LIGHT(vertex_light[7], NULL, false, luminance), lu1, lv1);
		}
	}

	return quads;
}

size_t render_block_ladder(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only) {
	if((this->block->metadata == 2 && side != SIDE_FRONT)
	   || (this->block->metadata == 3 && side != SIDE_BACK)
	   || (this->block->metadata == 4 && side != SIDE_LEFT)
	   || (this->block->metadata == 5 && side != SIDE_RIGHT))
		return 0;

	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			BLK_LEN, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true, 240, false, 0, side,
			vertex_light);
	return 1;
}

size_t render_block_crops(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	if(side == SIDE_TOP || side == SIDE_BOTTOM)
		return 0;

	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), -16,
			BLK_LEN, block_texture_index(this, side),
			blocks[this->block->type]->luminance, false, 64, false, 0, side,
			vertex_light);
	return 1;
}

size_t render_block_cake(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only) {
	if(!count_only) {
		uint8_t tex = block_texture_index(this, side);
		uint8_t luminance = blocks[this->block->type]->luminance;

		switch(side) {
			case SIDE_FRONT:
				render_block_side_adv(d,
									  W2C_COORD(this->x) * BLK_LEN
										  + this->block->metadata * 32 + 16,
									  W2C_COORD(this->y) * BLK_LEN,
									  W2C_COORD(this->z) * BLK_LEN + 16,
									  (7 - this->block->metadata) * 32, 128,
									  TEX_OFFSET(TEXTURE_X(tex)) + 1,
									  TEX_OFFSET(TEXTURE_Y(tex)) + 8, false, 0,
									  true, side, vertex_light, luminance);
				break;
			case SIDE_BACK:
				render_block_side_adv(d,
									  W2C_COORD(this->x) * BLK_LEN
										  + this->block->metadata * 32 + 16,
									  W2C_COORD(this->y) * BLK_LEN,
									  W2C_COORD(this->z) * BLK_LEN + 240,
									  (7 - this->block->metadata) * 32, 128,
									  TEX_OFFSET(TEXTURE_X(tex)) + 1
										  + this->block->metadata * 2,
									  TEX_OFFSET(TEXTURE_Y(tex)) + 8, false, 0,
									  true, side, vertex_light, luminance);
				break;
			case SIDE_TOP:
				render_block_side_adv(d,
									  W2C_COORD(this->x) * BLK_LEN
										  + this->block->metadata * 32 + 16,
									  W2C_COORD(this->y) * BLK_LEN + 128,
									  W2C_COORD(this->z) * BLK_LEN + 16,
									  (7 - this->block->metadata) * 32, 224,
									  TEX_OFFSET(TEXTURE_X(tex)) + 1
										  + this->block->metadata * 2,
									  TEX_OFFSET(TEXTURE_Y(tex)) + 1, false, 0,
									  true, side, vertex_light, luminance);
				break;
			case SIDE_BOTTOM:
				render_block_side_adv(d,
									  W2C_COORD(this->x) * BLK_LEN
										  + this->block->metadata * 32 + 16,
									  W2C_COORD(this->y) * BLK_LEN,
									  W2C_COORD(this->z) * BLK_LEN + 16,
									  (7 - this->block->metadata) * 32, 224,
									  TEX_OFFSET(TEXTURE_X(tex)) + 1
										  + this->block->metadata * 2,
									  TEX_OFFSET(TEXTURE_Y(tex)) + 1, false, 0,
									  true, side, vertex_light, luminance);
				break;
			default:
				render_block_side(
					d, W2C_COORD(this->x), W2C_COORD(this->y),
					W2C_COORD(this->z), 0, 128, tex, luminance, true,
					(side == SIDE_LEFT) ? (16 + this->block->metadata * 32) :
										  16,
					false, 0, side, vertex_light);
		}
	}

	return 1;
}

size_t render_block_farmland(struct displaylist* d, struct block_info* this,
							 enum side side, struct block_info* it,
							 uint8_t* vertex_light, bool count_only) {
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			240, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true, 0, false, 0, side,
			vertex_light);
	return 1;
}

size_t render_block_stairs_always(struct displaylist* d,
								  struct block_info* this, enum side side,
								  struct block_info* it, uint8_t* vertex_light,
								  bool count_only) {
	enum side facing = (enum side[4]) {SIDE_RIGHT, SIDE_LEFT, SIDE_BACK,
									   SIDE_FRONT}[this->block->metadata & 3];

	if(side != SIDE_TOP && side != blocks_side_opposite(facing))
		return 0;

	if(!count_only) {
		uint8_t tex = block_texture_index(this, side);
		uint8_t luminance = blocks[this->block->type]->luminance;

		if(side == SIDE_TOP) {
			render_block_side(d, W2C_COORD(this->x), W2C_COORD(this->y),
							  W2C_COORD(this->z), 0, BLK_LEN / 2, tex,
							  luminance, true, 0, false, 0, side, vertex_light);
		} else if(side == blocks_side_opposite(facing)) {
			render_block_side(d, W2C_COORD(this->x), W2C_COORD(this->y),
							  W2C_COORD(this->z), 0, BLK_LEN, tex, luminance,
							  true, BLK_LEN / 2, false, 0, side, vertex_light);
		}
	}

	return 1;
}

size_t render_block_stairs(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only) {
	size_t k = 0;
	enum side facing = (enum side[4]) {SIDE_RIGHT, SIDE_LEFT, SIDE_BACK,
									   SIDE_FRONT}[this->block->metadata & 3];
	uint8_t tex = block_texture_index(this, side);
	uint8_t luminance = blocks[this->block->type]->luminance;

	// render "slab"
	if(side == facing) {
		if(!count_only)
			render_block_side(d, W2C_COORD(this->x), W2C_COORD(this->y),
							  W2C_COORD(this->z), 0, BLK_LEN, tex, luminance,
							  true, 0, false, 0, side, vertex_light);
		k++;
	} else if(side != SIDE_TOP) {
		if(!count_only)
			render_block_side(d, W2C_COORD(this->x), W2C_COORD(this->y),
							  W2C_COORD(this->z), 0, BLK_LEN / 2, tex,
							  luminance, true, 0, false, 0, side, vertex_light);
		k++;
	}

	// render top part
	if(side == SIDE_TOP) {
		int tx = (facing == SIDE_RIGHT) ? BLK_LEN / 2 : 0;
		int ty = (facing == SIDE_BACK) ? BLK_LEN / 2 : 0;
		int sx = (facing == SIDE_LEFT || facing == SIDE_RIGHT) ? BLK_LEN / 2 :
																 BLK_LEN;
		int sy = (facing == SIDE_FRONT || facing == SIDE_BACK) ? BLK_LEN / 2 :
																 BLK_LEN;
		if(!count_only)
			render_block_side_adv(d, W2C_COORD(this->x) * BLK_LEN + tx,
								  W2C_COORD(this->y) * BLK_LEN + BLK_LEN,
								  W2C_COORD(this->z) * BLK_LEN + ty, sx, sy,
								  TEX_OFFSET(TEXTURE_X(tex)) + tx / 16,
								  TEX_OFFSET(TEXTURE_Y(tex)) + ty / 16, false,
								  false, true, SIDE_TOP, vertex_light,
								  luminance);
		k++;
	} else if(side != facing && side != blocks_side_opposite(facing)
			  && side != SIDE_BOTTOM) {
		int off[][2] = {
			[SIDE_LEFT] = {0, 0},
			[SIDE_RIGHT] = {BLK_LEN, 0},
			[SIDE_FRONT] = {0, 0},
			[SIDE_BACK] = {0, BLK_LEN},
		};

		if(facing == SIDE_BACK) {
			off[SIDE_LEFT][1] = BLK_LEN / 2;
			off[SIDE_RIGHT][1] = BLK_LEN / 2;
		} else if(facing == SIDE_RIGHT) {
			off[SIDE_FRONT][0] = BLK_LEN / 2;
			off[SIDE_BACK][0] = BLK_LEN / 2;
		}

		// Beta RenderBlocks.renderEastFace/renderNorthFace [decompile 740c583] crop a
		// partial-bounds face's UV proportionally to the CURRENT block bounds, not from
		// a bare tile origin:
		//   renderEastFace/renderWestFace  (our FRONT/BACK, fixed-Z faces):
		//     u = (tileBaseU + minX*16) .. (tileBaseU + maxX*16)
		//   renderNorthFace/renderSouthFace (our LEFT/RIGHT, fixed-X faces):
		//     u = (tileBaseU + minZ*16) .. (tileBaseU + maxZ*16)
		//   both:  v = (tileBaseV + 16 - maxY*16) .. (tileBaseV + 16 - minY*16)
		// This corner-riser quad's bounds are always Y:[0.5,1] (v origin +0, top of the
		// tile -- already correct) and, in the horizontal in-plane axis, either the near
		// half [0,0.5] (u origin +0) or the far half [0.5,1] (u origin +8 texels) of a
		// 16-texel tile, selected by the SAME off[][] entry that already positions the
		// quad in world space: off[side][1] (Z) for the fixed-X LEFT/RIGHT faces, or
		// off[side][0] (X) for the fixed-Z FRONT/BACK faces.
		int in_plane_off
			= (side == SIDE_LEFT || side == SIDE_RIGHT) ? off[side][1] : off[side][0];

		if(!count_only)
			render_block_side_adv(
				d, W2C_COORD(this->x) * BLK_LEN + off[side][0],
				W2C_COORD(this->y) * BLK_LEN + BLK_LEN / 2,
				W2C_COORD(this->z) * BLK_LEN + off[side][1], BLK_LEN / 2,
				BLK_LEN / 2, TEX_OFFSET(TEXTURE_X(tex)) + in_plane_off / 16,
				TEX_OFFSET(TEXTURE_Y(tex)), false, false, true, side,
				vertex_light, luminance);
		k++;
	}

	return k;
}

size_t render_block_bed(struct displaylist* d, struct block_info* this,
						enum side side, struct block_info* it,
						uint8_t* vertex_light, bool count_only) {
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			144, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true,
			(side == SIDE_BOTTOM) ? 48 : 0,
			(side == SIDE_RIGHT && bed_meta_direction(this->block->metadata) == 0)
				|| (side == SIDE_BACK && bed_meta_direction(this->block->metadata) == 1)
				|| (side == SIDE_LEFT && bed_meta_direction(this->block->metadata) == 2)
				|| (side == SIDE_FRONT && bed_meta_direction(this->block->metadata) == 3),
			(side == SIDE_TOP || side == SIDE_BOTTOM) ?
				bed_meta_top_rotation(this->block->metadata) :
				0,
			side, vertex_light);
	return 1;
}

size_t render_block_fire(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only) {
	if(side == SIDE_TOP)
		return 0;

	size_t k = 0;
	bool is_floating = !blocks[this->neighbours[SIDE_BOTTOM].type]
		|| (blocks[this->neighbours[SIDE_BOTTOM].type]->can_see_through
			&& !blocks[this->neighbours[SIDE_BOTTOM].type]->flammable);

	int16_t x = W2C_COORD(this->x);
	int16_t y = W2C_COORD(this->y);
	int16_t z = W2C_COORD(this->z);
	uint8_t tex = block_texture_index(this, side);
	uint8_t luminance = blocks[this->block->type]->luminance;
	uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
	// Beta registers TWO TextureFlamesFX (fire, fire+16) and renderBlockFire
	// picks one per quad. anim.png bakes them as two strips (flame_fx.h):
	// instance 0 at the fire cell, instance 1 FLAME_FX_ANIM_COL1-COL0 columns
	// to its right, same rows, same matrix_anim scroll. Index with the
	// flame_fx_block_*_cell helpers, which cite the exact Beta lines. Always
	// cols 6/13 for fire+16: a pack without that strip had cols 3/10 copied in
	// at load (flame_fx_anim_prepare_pack_atlas), so baked lists stay valid
	// across an in-game pack switch.
	const uint8_t tex_xs[2] = {
		TEX_OFFSET(TEXTURE_X(tex)),
		TEX_OFFSET(TEXTURE_X(tex) + FLAME_FX_ANIM_COL1 - FLAME_FX_ANIM_COL0),
	};
	// RenderBlocks :798-803 parity for the flammable-neighbour branch (Java int
	// arithmetic on world coordinates).
	const uint8_t tex_x_side = tex_xs[flame_fx_block_side_cell(
		(int)this->x, (int)this->y, (int)this->z)];
	// RenderBlocks.renderBlockFire :805-809 swaps var10/var12 only for the
	// flammable-neighbour quads. Java int /2 truncates toward zero.
	const bool flip_side_u = (((int)this->x / 2 + (int)this->y / 2
		+ (int)this->z / 2) & 1) != 0;
	// RenderBlocks :812-815 (-X) puts var10 at z=0 and :834-837 (-Z) puts
	// var12 at x=0. render_block_side_adv_v2's SIDE_RIGHT/SIDE_BACK corner
	// order is U-mirrored against that relative to SIDE_LEFT/SIDE_FRONT (+X
	// :823-826, +Z :845-848), so the -X/-Z leaning quads take the opposite
	// flip to land Beta's per-position U (next-gaps-0924/R1a).
	const bool flip_side_u_neg = !flip_side_u;
	// RenderBlocks :907-944, free-standing: the per-side pair is (edge quad =
	// Beta's OUTER plane, foot on the block edge; inset quad = Beta's INNER
	// plane, foot 0.3 in, leaning 0.5). LEFT/RIGHT are constant-X planes,
	// FRONT/BACK constant-Z.
	const uint8_t tex_x_outer_x = tex_xs[flame_fx_block_free_cell(false, true)];
	const uint8_t tex_x_inner_x = tex_xs[flame_fx_block_free_cell(false, false)];
	const uint8_t tex_x_outer_z = tex_xs[flame_fx_block_free_cell(true, true)];
	const uint8_t tex_x_inner_z = tex_xs[flame_fx_block_free_cell(true, false)];
	// RenderBlocks.java:907-926 (RetroMCP b1.7.3 lines): each inset quad (foot
	// 0.31 in, top 0.81) is Beta's OTHER plane of its axis, whose var10/var12
	// sit at the opposite ends from the edge plane (:935-954). So the four
	// inset calls below pass tex_flip_h = true; the edge calls keep false
	// (next-gaps-0924/G1b, tests/test_fire_block_uv_flip.c free_inset_hi).
	uint16_t height = 360;
	// RenderBlocks.java:811-853 var37 = 0.2F: the leaning quads' tops sit 0.2
	// block in from the attached face. 0.2 * 256 = 51.2 -> 51 (was 48 = 3/16).
	// The ceiling branch's var18 = -0.2F uses the same 51 (1 - 0.2 -> 205).
	enum { FIRE_LEAN_0P2 = 51 };

	if(is_floating) {
		switch(side) {
			case SIDE_BOTTOM:
				if(blocks[this->neighbours[SIDE_TOP].type]
				   && blocks[this->neighbours[SIDE_TOP].type]->flammable) {
					if(!count_only) {
						/* RenderBlocks.java:855-897: ++var3 precedes the parity
						 * test. Even parity leans on X; odd parity leans on Z.
						 * Beta's var18 = -0.2F drop: 1 - 0.2 = 0.8 -> 205 in
						 * 1/256 units (the same 0.2F as FIRE_LEAN_0P2 = 51).
						 * corners[] lists Beta's LITERAL vertex order. CaveWii's
						 * front-face winding is the MIRROR of Beta's (Beta
						 * renderBottomFace (minX,maxZ)->(minX,minZ)->(maxX,minZ)
						 * ->(maxX,maxZ) vs SIDE_BOTTOM above (x,z)->(x,z+h)->
						 * (x+w,z+h)->(x+w,z); glFrontFace(GL_CW), pc/gfx.c), and
						 * fire is single-sided (block_fire.c double_sided false)
						 * under GX_CULL_BACK, so each quad is emitted in REVERSE
						 * (3,2,1,0) with its UV kept paired to its position, or
						 * it faces up and is culled from below. Vertex light is
						 * indexed by CORNER like SIDE_BOTTOM: (x,z) 0, (x+1,z) 1,
						 * (x+1,z+1) 2, (x,z+1) 3 -- never by emission slot. */
						static const uint16_t corners[2][2][4][3] = {
							{{{0, 205, 0}, {256, 256, 0}, {256, 256, 256}, {0, 205, 256}},
							 {{256, 205, 256}, {0, 256, 256}, {0, 256, 0}, {256, 205, 0}}},
							{{{0, 205, 256}, {0, 256, 0}, {256, 256, 0}, {256, 205, 256}},
							 {{256, 205, 0}, {256, 256, 256}, {0, 256, 256}, {0, 205, 0}}},
						};
						int parity = ((int)this->x + (int)this->y + 1 + (int)this->z) & 1;
						for(int q = 0; q < 2; q++) {
							uint8_t u = tex_xs[flame_fx_block_ceiling_cell(q)];
							for(int v = 3; v >= 0; v--) {
								const uint16_t* c = corners[parity][q][v];
								int corner = c[2] ? (c[0] ? 2 : 3) : (c[0] ? 1 : 0);
								displaylist_vertex(d, x * BLK_LEN + c[0],
									y * BLK_LEN + c[1], z * BLK_LEN + c[2],
									DIM_LIGHT(vertex_light[corner], level_table_0, false, luminance),
									u + (v < 2 ? 16 : 0), tex_y + (v == 1 || v == 2 ? 16 : 0));
							}
						}
					}
					k += 2;
				}
				break;
			case SIDE_LEFT:
				if(blocks[this->neighbours[SIDE_RIGHT].type]
				   && blocks[this->neighbours[SIDE_RIGHT].type]->flammable) {
					if(!count_only)
						render_block_side_adv_v2(
							d, x * BLK_LEN + BLK_LEN, y * BLK_LEN + 16,
							z * BLK_LEN, BLK_LEN, height, 0, -FIRE_LEAN_0P2, tex_x_side, tex_y,
							flip_side_u, false, false, SIDE_LEFT, vertex_light,
							luminance);
					k++;
				}
				break;
			case SIDE_RIGHT:
				if(blocks[this->neighbours[SIDE_LEFT].type]
				   && blocks[this->neighbours[SIDE_LEFT].type]->flammable) {
					if(!count_only)
						render_block_side_adv_v2(
							d, x * BLK_LEN, y * BLK_LEN + 16, z * BLK_LEN,
							BLK_LEN, height, 0, -FIRE_LEAN_0P2, tex_x_side, tex_y, flip_side_u_neg, false,
							false, SIDE_RIGHT, vertex_light, luminance);
					k++;
				}
				break;
			case SIDE_FRONT:
				if(blocks[this->neighbours[SIDE_BACK].type]
				   && blocks[this->neighbours[SIDE_BACK].type]->flammable) {
					if(!count_only)
						render_block_side_adv_v2(
							d, x * BLK_LEN, y * BLK_LEN + 16,
							z * BLK_LEN + BLK_LEN, BLK_LEN, height, 0, -FIRE_LEAN_0P2,
							tex_x_side, tex_y, flip_side_u, false, false, SIDE_FRONT,
							vertex_light, luminance);
					k++;
				}
				break;
			case SIDE_BACK:
				if(blocks[this->neighbours[SIDE_FRONT].type]
				   && blocks[this->neighbours[SIDE_FRONT].type]->flammable) {
					if(!count_only)
						render_block_side_adv_v2(
							d, x * BLK_LEN, y * BLK_LEN + 16, z * BLK_LEN,
							BLK_LEN, height, 0, -FIRE_LEAN_0P2, tex_x_side, tex_y, flip_side_u_neg, false,
							false, SIDE_BACK, vertex_light, luminance);
					k++;
				}
				break;
			default: break;
		}
	} else {
		switch(side) {
			case SIDE_LEFT:
				if(!count_only) {
					render_block_side_adv_v2(
						d, x * BLK_LEN, y * BLK_LEN, z * BLK_LEN, BLK_LEN,
						height, 0, 24, tex_x_outer_x, tex_y, false, false, false,
						SIDE_LEFT, vertex_light, luminance);

					render_block_side_adv_v2(
						d, x * BLK_LEN + 5 * 16, y * BLK_LEN, z * BLK_LEN,
						BLK_LEN, height, 0, BLK_LEN / 2, tex_x_inner_x, tex_y, true,
						false, false, SIDE_LEFT, vertex_light, luminance);
				}
				k += 2;
				break;
			case SIDE_RIGHT:
				if(!count_only) {
					render_block_side_adv_v2(
						d, x * BLK_LEN + BLK_LEN, y * BLK_LEN, z * BLK_LEN,
						BLK_LEN, height, 0, 24, tex_x_outer_x, tex_y, false, false,
						false, SIDE_RIGHT, vertex_light, luminance);

					render_block_side_adv_v2(
						d, x * BLK_LEN + BLK_LEN - 5 * 16, y * BLK_LEN,
						z * BLK_LEN, BLK_LEN, height, 0, BLK_LEN / 2, tex_x_inner_x,
						tex_y, true, false, false, SIDE_RIGHT, vertex_light,
						luminance);
				}
				k += 2;
				break;
			case SIDE_FRONT:
				if(!count_only) {
					render_block_side_adv_v2(
						d, x * BLK_LEN, y * BLK_LEN, z * BLK_LEN, BLK_LEN,
						height, 0, 24, tex_x_outer_z, tex_y, false, false, false,
						SIDE_FRONT, vertex_light, luminance);

					render_block_side_adv_v2(
						d, x * BLK_LEN, y * BLK_LEN, z * BLK_LEN + 5 * 16,
						BLK_LEN, height, 0, BLK_LEN / 2, tex_x_inner_z, tex_y, true,
						false, false, SIDE_FRONT, vertex_light, luminance);
				}
				k += 2;
				break;
			case SIDE_BACK:
				if(!count_only) {
					render_block_side_adv_v2(
						d, x * BLK_LEN, y * BLK_LEN, z * BLK_LEN + BLK_LEN,
						BLK_LEN, height, 0, 24, tex_x_outer_z, tex_y, false, false,
						false, SIDE_BACK, vertex_light, luminance);

					render_block_side_adv_v2(
						d, x * BLK_LEN, y * BLK_LEN,
						z * BLK_LEN + BLK_LEN - 5 * 16, BLK_LEN, height, 0,
						BLK_LEN / 2, tex_x_inner_z, tex_y, true, false, false,
						SIDE_BACK, vertex_light, luminance);
				}
				k += 2;
				break;
			default: break;
		}
	}

	return k;
}

size_t render_block_pressure_plate(struct displaylist* d,
								   struct block_info* this, enum side side,
								   struct block_info* it, uint8_t* vertex_light,
								   bool count_only) {
	if(!count_only) {
		uint8_t tex = block_texture_index(this, side);
		uint8_t luminance = blocks[this->block->type]->luminance;
		uint8_t height = this->block->metadata ? 8 : 16;

		switch(side) {
			case SIDE_TOP:
				render_block_side_adv(
					d, W2C_COORD(this->x) * BLK_LEN + 16,
					W2C_COORD(this->y) * BLK_LEN + height,
					W2C_COORD(this->z) * BLK_LEN + 16, BLK_LEN - 32,
					BLK_LEN - 32, TEX_OFFSET(TEXTURE_X(tex)) + 1,
					TEX_OFFSET(TEXTURE_Y(tex)) + 1, false, false, true,
					SIDE_TOP, vertex_light, luminance);
				break;
			case SIDE_BOTTOM:
				render_block_side_adv(
					d, W2C_COORD(this->x) * BLK_LEN + 16,
					W2C_COORD(this->y) * BLK_LEN,
					W2C_COORD(this->z) * BLK_LEN + 16, BLK_LEN - 32,
					BLK_LEN - 32, TEX_OFFSET(TEXTURE_X(tex)) + 1,
					TEX_OFFSET(TEXTURE_Y(tex)) + 1, false, false, true,
					SIDE_BOTTOM, vertex_light, luminance);
				break;
			case SIDE_FRONT:
			case SIDE_LEFT:
				render_block_side_adv(
					d, W2C_COORD(this->x) * BLK_LEN + 16,
					W2C_COORD(this->y) * BLK_LEN,
					W2C_COORD(this->z) * BLK_LEN + 16, BLK_LEN - 32, height,
					TEX_OFFSET(TEXTURE_X(tex)) + 1,
					TEX_OFFSET(TEXTURE_Y(tex)) + 1, false, false, true, side,
					vertex_light, luminance);
				break;
			case SIDE_RIGHT:
				render_block_side_adv(
					d, W2C_COORD(this->x) * BLK_LEN + BLK_LEN - 16,
					W2C_COORD(this->y) * BLK_LEN,
					W2C_COORD(this->z) * BLK_LEN + 16, BLK_LEN - 32, height,
					TEX_OFFSET(TEXTURE_X(tex)) + 1,
					TEX_OFFSET(TEXTURE_Y(tex)) + 1, false, false, true,
					SIDE_RIGHT, vertex_light, luminance);
				break;
			case SIDE_BACK:
				render_block_side_adv(
					d, W2C_COORD(this->x) * BLK_LEN + 16,
					W2C_COORD(this->y) * BLK_LEN,
					W2C_COORD(this->z) * BLK_LEN + BLK_LEN - 16, BLK_LEN - 32,
					height, TEX_OFFSET(TEXTURE_X(tex)) + 1,
					TEX_OFFSET(TEXTURE_Y(tex)) + 1, false, false, true,
					SIDE_BACK, vertex_light, luminance);
				break;
			default: break;
		}
	}

	return 1;
}

// Beta BlockButton: a small stone box hugging its mount wall (meta&7: 1=-X, 2=+X, 3=-Z, 4=+Z),
// y 0.375..0.625, wall-parallel 0.3125..0.6875, depth 0.125 (0.0625 pressed -- the press is
// visible as the button sinking into the wall). One face per mesher side call, each emitted at
// the box's boundary plane via render_block_side_adv (whose per-side span convention is:
// X faces width=Z/height=Y, Y faces width=X/height=Z, Z faces width=X/height=Y).
size_t render_block_button(struct displaylist* d, struct block_info* this,
						   enum side side, struct block_info* it,
						   uint8_t* vertex_light, bool count_only) {
	if(!count_only) {
		uint8_t tex = block_texture_index(this, side);
		uint8_t luminance = blocks[this->block->type]->luminance;
		uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex)) + 1;
		uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex)) + 1;
		int meta = this->block->metadata;
		int16_t depth = (meta & 8) ? 16 : 32;
		int16_t x0 = 80, x1 = 176, z0 = 80, z1 = 176;
		int16_t y0 = 96, y1 = 160;
		switch(meta & 7) {
			case 1:
				x0 = 0;
				x1 = depth;
				break;
			case 2:
				x0 = BLK_LEN - depth;
				x1 = BLK_LEN;
				break;
			case 3:
				z0 = 0;
				z1 = depth;
				break;
			case 4:
				z0 = BLK_LEN - depth;
				z1 = BLK_LEN;
				break;
			default: break;
		}
		int16_t bx = W2C_COORD(this->x) * BLK_LEN;
		int16_t by = W2C_COORD(this->y) * BLK_LEN;
		int16_t bz = W2C_COORD(this->z) * BLK_LEN;
		switch(side) {
			case SIDE_LEFT:
				render_block_side_adv(d, bx + x0, by + y0, bz + z0, z1 - z0,
									  y1 - y0, tex_x, tex_y, false, 0, true,
									  SIDE_LEFT, vertex_light, luminance);
				break;
			case SIDE_RIGHT:
				render_block_side_adv(d, bx + x1, by + y0, bz + z0, z1 - z0,
									  y1 - y0, tex_x, tex_y, false, 0, true,
									  SIDE_RIGHT, vertex_light, luminance);
				break;
			case SIDE_TOP:
				render_block_side_adv(d, bx + x0, by + y1, bz + z0, x1 - x0,
									  z1 - z0, tex_x, tex_y, false, 0, true,
									  SIDE_TOP, vertex_light, luminance);
				break;
			case SIDE_BOTTOM:
				render_block_side_adv(d, bx + x0, by + y0, bz + z0, x1 - x0,
									  z1 - z0, tex_x, tex_y, false, 0, true,
									  SIDE_BOTTOM, vertex_light, luminance);
				break;
			case SIDE_FRONT:
				render_block_side_adv(d, bx + x0, by + y0, bz + z0, x1 - x0,
									  y1 - y0, tex_x, tex_y, false, 0, true,
									  SIDE_FRONT, vertex_light, luminance);
				break;
			case SIDE_BACK:
				render_block_side_adv(d, bx + x0, by + y0, bz + z1, x1 - x0,
									  y1 - y0, tex_x, tex_y, false, 0, true,
									  SIDE_BACK, vertex_light, luminance);
				break;
			default: break;
		}
	}

	return 1;
}

// Beta render type 15 INTERIM: a faithful 0.125-block slab (BlockRedstoneRepeater
// setBlockBounds 0,0,0,1,0.125,1) wearing the real Beta repeater face textures
// (getBlockTextureFromSideAndMetadata: top idle 131/active 147, bottom 115/99 == the
// redstone-torch tiles, sides = stone). The two 3D redstone torches Beta's render type 15
// stands proud of the slab (renderTorchAtAngle at repeaterTorchOffset, the gap encoding the
// delay) are DEFERRED -- the Isabella II top tile depicts them + the delay dots in 2D, so the
// flat block is recognizable with correct facing/state. Documented render-layer carve-out
// (docs/wii_controller_playability.md); ZERO simulation impact (delay/power/facing all in the
// redstone_repeater core). The top quad is rotated by facing (meta & 3) so the notch points
// along the output side (the exact rotate->visual mapping is confirmed on Dolphin).
size_t render_block_repeater(struct displaylist* d, struct block_info* this,
							 enum side side, struct block_info* it,
							 uint8_t* vertex_light, bool count_only) {
	(void)it;
	if(!count_only) {
		uint8_t tex = block_texture_index(this, side);
		uint8_t luminance = blocks[this->block->type]->luminance;
		const uint16_t height = 32; // 0.125 block in 1/256 units

		switch(side) {
			case SIDE_TOP:
				render_block_side_adv(
					d, W2C_COORD(this->x) * BLK_LEN,
					W2C_COORD(this->y) * BLK_LEN + height,
					W2C_COORD(this->z) * BLK_LEN, BLK_LEN, BLK_LEN,
					TEX_OFFSET(TEXTURE_X(tex)), TEX_OFFSET(TEXTURE_Y(tex)), false,
					this->block->metadata & 0x03, true, SIDE_TOP, vertex_light,
					luminance);
				break;
			case SIDE_BOTTOM:
				render_block_side_adv(
					d, W2C_COORD(this->x) * BLK_LEN, W2C_COORD(this->y) * BLK_LEN,
					W2C_COORD(this->z) * BLK_LEN, BLK_LEN, BLK_LEN,
					TEX_OFFSET(TEXTURE_X(tex)), TEX_OFFSET(TEXTURE_Y(tex)), false, 0,
					true, SIDE_BOTTOM, vertex_light, luminance);
				break;
			case SIDE_LEFT:
			case SIDE_RIGHT:
			case SIDE_FRONT:
			case SIDE_BACK:
				render_block_side(d, W2C_COORD(this->x), W2C_COORD(this->y),
								  W2C_COORD(this->z), 0, height, tex, luminance, true,
								  0, false, 0, side, vertex_light);
				break;
			default: break;
		}
	}

	return 1;
}

size_t render_block_fence_always(struct displaylist* d, struct block_info* this,
								 enum side side, struct block_info* it,
								 uint8_t* vertex_light, bool count_only) {
	size_t k = 0;
	int16_t x = W2C_COORD(this->x);
	int16_t y = W2C_COORD(this->y);
	int16_t z = W2C_COORD(this->z);
	uint8_t tex = block_texture_index(this, side);
	uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
	uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
	uint8_t luminance = blocks[this->block->type]->luminance;
	bool connect_pos_x = this->neighbours[SIDE_RIGHT].type == this->block->type;
	bool connect_pos_z = this->neighbours[SIDE_BACK].type == this->block->type;

	// Beta RenderBlocks.renderBlockFence [decompile 740c583] draws each rail as a full
	// renderStandardBlock box with bounds Y:[0.75,0.9375] (upper) / [0.375,0.5625]
	// (lower), X or Z:[0.4375,0.5625] (cross-section) x [0,1] or a neighbour-clamped
	// stub along the rail's long axis -- this port's positions already match those Y
	// bounds exactly (12..15 and 6..9 sixteenths) and instead draws ONE quad per
	// connected gap (post-face to post-face, 10..22 sixteenths: 6 sixteenths of THIS
	// post's far half plus 6 of the neighbour's near half) rather than Beta's per-post
	// double-draw, so its long axis intentionally exceeds one block (>16 texels).
	//
	// Per-face UV, from renderEastFace/renderNorthFace/renderTopFace/renderBottomFace's
	// proportional crop (u = tileBase + minAxis*16, v = tileBase + 16 - maxY*16 for the
	// vertical side faces, v = tileBase + minZ*16 -- NOT inverted -- for top/bottom):
	// the SHORT (cross-section) axis stays within one block, so it crops normally (7 for
	// the rail's 0.4375 inner edge); the LONG (rail-length) axis exceeds a full block,
	// which is EXACTLY the case those four decompiled methods special-case themselves --
	// Beta uses the full tile, from 0 to 15.99 texels, when X bounds exceed [0,1].
	// (same guard, keyed on minZ/maxZ for the Z-axis rail) -- so Beta's own rule for an
	// out-of-[0,1]-bounds face is "use the full tile, unshifted", i.e. origin 0, not the
	// old hand-picked +2.
	//
	// For LEFT/RIGHT/FRONT/BACK the U parameter is ALWAYS Z-derived and V is ALWAYS
	// Y-derived (Y never overflows for these rails), so one pair of constants covers both
	// connected pieces. TOP/BOTTOM is different: u is ALWAYS X-derived and v is ALWAYS
	// Z-derived (per renderTopFace/renderBottomFace), but WHICH of X/Z is the long
	// (overflowing) axis SWAPS between the connect_pos_x piece (X is the rail's length,
	// overflows; Z is its cross-section) and the connect_pos_z piece (Z overflows; X is
	// the cross-section) -- so both an X-side and a Z-side pair of constants are needed,
	// each picking the overflow-fallback value on its own long axis and the +7 crop on
	// its own short axis.
	uint8_t rail_side_u_long = tex_x;		// long (rail-length) axis: Beta's >[0,1] fallback
	uint8_t rail_side_v_hi = tex_y + 1;	// upper rail: v = 16 - maxY*16 = 16 - 15 = 1
	uint8_t rail_side_v_lo = tex_y + 7;	// lower rail: v = 16 - maxY*16 = 16 - 9 = 7
	uint8_t rail_face_u_over = tex_x;		// u, X overflowing (connect_pos_x's long axis)
	uint8_t rail_face_u_in = tex_x + 7;	// u, X in-bounds cross-section: minX*16 = 7
	uint8_t rail_face_v_over = tex_y;		// v, Z overflowing (connect_pos_z's long axis)
	uint8_t rail_face_v_in = tex_y + 7;	// v, Z in-bounds cross-section: minZ*16 = 7

	switch(side) {
		case SIDE_LEFT:
			if(connect_pos_z && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 7 / 16,
									  y * BLK_LEN + BLK_LEN * 12 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_hi, false, 0,
									  true, SIDE_LEFT, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 7 / 16,
									  y * BLK_LEN + BLK_LEN * 6 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_lo, false, 0,
									  true, SIDE_LEFT, vertex_light, luminance);
			}

			k += connect_pos_z ? 2 : 0;
			break;
		case SIDE_RIGHT:
			if(connect_pos_z && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 9 / 16,
									  y * BLK_LEN + BLK_LEN * 12 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_hi, false, 0,
									  true, SIDE_RIGHT, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 9 / 16,
									  y * BLK_LEN + BLK_LEN * 6 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_lo, false, 0,
									  true, SIDE_RIGHT, vertex_light, luminance);
			}

			k += connect_pos_z ? 2 : 0;
			break;
		case SIDE_BOTTOM:
			if(connect_pos_x && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 12 / 16,
									  z * BLK_LEN + BLK_LEN * 7 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN / 8,
									  rail_face_u_over, rail_face_v_in, false, 0,
									  true, SIDE_BOTTOM, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 6 / 16,
									  z * BLK_LEN + BLK_LEN * 7 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN / 8,
									  rail_face_u_over, rail_face_v_in, false, 0,
									  true, SIDE_BOTTOM, vertex_light, luminance);
			}

			if(connect_pos_z && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 7 / 16,
									  y * BLK_LEN + BLK_LEN * 12 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN / 8, BLK_LEN * 3 / 4,
									  rail_face_u_in, rail_face_v_over, false, 0,
									  true, SIDE_BOTTOM, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 7 / 16,
									  y * BLK_LEN + BLK_LEN * 6 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN / 8, BLK_LEN * 3 / 4,
									  rail_face_u_in, rail_face_v_over, false, 0,
									  true, SIDE_BOTTOM, vertex_light, luminance);
			}

			k += connect_pos_x ? 2 : 0;
			k += connect_pos_z ? 2 : 0;
			break;
		case SIDE_TOP:
			if(connect_pos_x && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 15 / 16,
									  z * BLK_LEN + BLK_LEN * 7 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN / 8,
									  rail_face_u_over, rail_face_v_in, false, 0,
									  true, SIDE_TOP, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 9 / 16,
									  z * BLK_LEN + BLK_LEN * 7 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN / 8,
									  rail_face_u_over, rail_face_v_in, false, 0,
									  true, SIDE_TOP, vertex_light, luminance);
			}

			if(connect_pos_z && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 7 / 16,
									  y * BLK_LEN + BLK_LEN * 15 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN / 8, BLK_LEN * 3 / 4,
									  rail_face_u_in, rail_face_v_over, false, 0,
									  true, SIDE_TOP, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 7 / 16,
									  y * BLK_LEN + BLK_LEN * 9 / 16,
									  z * BLK_LEN + BLK_LEN * 10 / 16,
									  BLK_LEN / 8, BLK_LEN * 3 / 4,
									  rail_face_u_in, rail_face_v_over, false, 0,
									  true, SIDE_TOP, vertex_light, luminance);
			}

			k += connect_pos_x ? 2 : 0;
			k += connect_pos_z ? 2 : 0;
			break;
		case SIDE_FRONT:
			if(connect_pos_x && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 12 / 16,
									  z * BLK_LEN + BLK_LEN * 7 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_hi, false, 0,
									  true, SIDE_FRONT, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 6 / 16,
									  z * BLK_LEN + BLK_LEN * 7 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_lo, false, 0,
									  true, SIDE_FRONT, vertex_light, luminance);
			}

			k += connect_pos_x ? 2 : 0;
			break;
		case SIDE_BACK:
			if(connect_pos_x && !count_only) {
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 12 / 16,
									  z * BLK_LEN + BLK_LEN * 9 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_hi, false, 0,
									  true, SIDE_BACK, vertex_light, luminance);
				render_block_side_adv(d, x * BLK_LEN + BLK_LEN * 10 / 16,
									  y * BLK_LEN + BLK_LEN * 6 / 16,
									  z * BLK_LEN + BLK_LEN * 9 / 16,
									  BLK_LEN * 3 / 4, BLK_LEN * 3 / 16,
									  rail_side_u_long, rail_side_v_lo, false, 0,
									  true, SIDE_BACK, vertex_light, luminance);
			}

			k += connect_pos_x ? 2 : 0;
			break;
		default: break;
	}

	return k;
}

size_t render_block_fence(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	if(!count_only) {
		int16_t x = W2C_COORD(this->x);
		int16_t y = W2C_COORD(this->y);
		int16_t z = W2C_COORD(this->z);
		uint8_t tex = block_texture_index(this, side);
		uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
		uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
		uint8_t luminance = blocks[this->block->type]->luminance;

		// Beta RenderBlocks.renderBlockFence's post [decompile 740c583]: one
		// renderStandardBlock box, bounds X/Z:[0.375,0.625] (6..10 sixteenths), Y full.
		// Per renderEastFace/renderNorthFace, LEFT/RIGHT/FRONT/BACK crop u = minAxis*16
		// (6, already correct below) and v = 16 - maxY*16 = 16 - 16 = 0 (full height,
		// already correct). But renderTopFace/renderBottomFace crop u = minX*16 = 6 AND
		// v = minZ*16 = 6 (NOT inverted for the horizontal faces) -- the old code left
		// top/bottom at the tile's bare (0,0) origin instead of this post's actual
		// (6,6), the bug FH-06 flags.
		switch(side) {
			case SIDE_LEFT:
				render_block_side_adv(
					d, x * BLK_LEN + BLK_LEN * 3 / 8, y * BLK_LEN,
					z * BLK_LEN + BLK_LEN * 3 / 8, BLK_LEN / 4, BLK_LEN,
					tex_x + 6, tex_y, false, 0, true, SIDE_LEFT, vertex_light,
					luminance);
				break;
			case SIDE_RIGHT:
				render_block_side_adv(
					d, x * BLK_LEN + BLK_LEN - BLK_LEN * 3 / 8, y * BLK_LEN,
					z * BLK_LEN + BLK_LEN * 3 / 8, BLK_LEN / 4, BLK_LEN,
					tex_x + 6, tex_y, false, 0, true, SIDE_RIGHT, vertex_light,
					luminance);
				break;
			case SIDE_BOTTOM:
				render_block_side_adv(
					d, x * BLK_LEN + BLK_LEN * 3 / 8, y * BLK_LEN,
					z * BLK_LEN + BLK_LEN * 3 / 8, BLK_LEN / 4, BLK_LEN / 4,
					tex_x + 6, tex_y + 6, false, 0, true, SIDE_BOTTOM,
					vertex_light, luminance);
				break;
			case SIDE_TOP:
				render_block_side_adv(
					d, x * BLK_LEN + BLK_LEN * 3 / 8, y * BLK_LEN + BLK_LEN,
					z * BLK_LEN + BLK_LEN * 3 / 8, BLK_LEN / 4, BLK_LEN / 4,
					tex_x + 6, tex_y + 6, false, 0, true, SIDE_TOP, vertex_light,
					luminance);
				break;
			case SIDE_FRONT:
				render_block_side_adv(
					d, x * BLK_LEN + BLK_LEN * 3 / 8, y * BLK_LEN,
					z * BLK_LEN + BLK_LEN * 3 / 8, BLK_LEN / 4, BLK_LEN,
					tex_x + 6, tex_y, false, 0, true, SIDE_FRONT, vertex_light,
					luminance);
				break;
			case SIDE_BACK:
				render_block_side_adv(
					d, x * BLK_LEN + BLK_LEN * 3 / 8, y * BLK_LEN,
					z * BLK_LEN + BLK_LEN - BLK_LEN * 3 / 8, BLK_LEN / 4,
					BLK_LEN, tex_x + 6, tex_y, false, 0, true, SIDE_BACK,
					vertex_light, luminance);
				break;
			default: break;
		}
	}

	return 1;
}

static size_t door_side_helper(struct displaylist* d, struct block_info* this,
							   enum side front, enum side side,
							   uint8_t* vertex_light, bool flip_front,
							   bool flip_back, bool count_only) {
	size_t count = 0;
	uint8_t tex = block_texture_index(this, side);
	uint8_t luminance = blocks[this->block->type]->luminance;
	uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
	uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
	int16_t x = W2C_COORD(this->x);
	int16_t y = W2C_COORD(this->y);
	int16_t z = W2C_COORD(this->z);

	if(side == front) {
		if(!count_only)
			render_block_side(d, x, y, z, 0, 256, tex, luminance, true,
							  BLK_LEN / 16 * 13, flip_front, 0, side,
							  vertex_light);
		count++;
	} else if(side == blocks_side_opposite(front)) {
		if(!count_only)
			render_block_side(d, x, y, z, 0, 256, tex, luminance, true, 0,
							  flip_back, 0, side, vertex_light);
		count++;
	} else if(side == SIDE_TOP || side == SIDE_BOTTOM) {
		if(!count_only)
			render_block_side_adv(
				d, x * BLK_LEN + (front == SIDE_LEFT ? BLK_LEN / 16 * 13 : 0),
				y * BLK_LEN + (side == SIDE_TOP ? BLK_LEN : 0),
				z * BLK_LEN + (front == SIDE_FRONT ? BLK_LEN / 16 * 13 : 0),
				(front == SIDE_LEFT || front == SIDE_RIGHT) ? BLK_LEN / 16 * 3 :
															  BLK_LEN,
				(front == SIDE_FRONT || front == SIDE_BACK) ? BLK_LEN / 16 * 3 :
															  BLK_LEN,
				tex_x + (front == SIDE_LEFT ? 13 : 0), tex_y,
				side == SIDE_BOTTOM, 0, true, side, vertex_light, luminance);
		count++;
	} else {
		if(!count_only)
			render_block_side_adv(
				d,
				x * BLK_LEN + (side == SIDE_RIGHT ? BLK_LEN : 0)
					+ (front == SIDE_LEFT ? BLK_LEN / 16 * 13 : 0),
				y * BLK_LEN,
				z * BLK_LEN + (side == SIDE_BACK ? BLK_LEN : 0)
					+ (front == SIDE_FRONT ? BLK_LEN / 16 * 13 : 0),
				BLK_LEN / 16 * 3, BLK_LEN, tex_x, tex_y, false, 0, true, side,
				vertex_light, luminance);
		count++;
	}

	return count;
}

// Beta SignModel geometry after TileEntitySignRenderer's 0.6666667 scale and
// post translate, rounded once to this renderer's 1/256-block fixed-point
// vertex grid. SignModel board addBox(-12,-14,-1,24,12,2) becomes
// x=[-128,128], y=[149,277], z=[-11,11]; signStick
// addBox(-1,-2,-1,2,14,2) becomes x/z=[-11,11], y=[0,149]. The model is
// centred at (0.5, 0, 0.5) and rotated by -(metadata*360/16), exactly the
// post branch of [740c583 TileEntitySignRenderer].
struct sign_post_box {
	int16_t bounds[3][2]; // [axis x/y/z][low/high]
};

static const struct sign_post_box s_sign_post_board = {
	.bounds = {{-128, 128}, {149, 277}, {-11, 11}},
};
static const struct sign_post_box s_sign_post_stick = {
	.bounds = {{-11, 11}, {0, 149}, {-11, 11}},
};

// Low/high corner selectors, in winding order, for each enum side.
static const uint8_t s_sign_box_corners[SIDE_MAX][4][3] = {
	[SIDE_TOP] = {{0,1,0}, {1,1,0}, {1,1,1}, {0,1,1}},
	[SIDE_BOTTOM] = {{0,0,0}, {0,0,1}, {1,0,1}, {1,0,0}},
	[SIDE_LEFT] = {{0,0,0}, {0,1,0}, {0,1,1}, {0,0,1}},
	[SIDE_RIGHT] = {{1,0,0}, {1,0,1}, {1,1,1}, {1,1,0}},
	[SIDE_FRONT] = {{0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}},
	[SIDE_BACK] = {{0,0,1}, {0,1,1}, {1,1,1}, {1,0,1}},
};
static const uint8_t s_sign_box_uv_axes[SIDE_MAX][2] = {
	[SIDE_TOP] = {0, 2},
	[SIDE_BOTTOM] = {0, 2},
	[SIDE_LEFT] = {2, 1},
	[SIDE_RIGHT] = {2, 1},
	[SIDE_FRONT] = {0, 1},
	[SIDE_BACK] = {0, 1},
};
static const uint8_t s_sign_box_uv_order[SIDE_MAX][4][2] = {
	[SIDE_TOP] = {{0,0}, {1,0}, {1,1}, {0,1}},
	[SIDE_BOTTOM] = {{0,0}, {0,1}, {1,1}, {1,0}},
	[SIDE_LEFT] = {{0,1}, {0,0}, {1,0}, {1,1}},
	[SIDE_RIGHT] = {{0,1}, {1,1}, {1,0}, {0,0}},
	[SIDE_FRONT] = {{0,1}, {1,1}, {1,0}, {0,0}},
	[SIDE_BACK] = {{0,1}, {0,0}, {1,0}, {1,1}},
};

static void sign_post_box_side(struct displaylist* d, int16_t base_x,
							   int16_t base_y, int16_t base_z,
							   const struct sign_post_box* box,
							   float sin_angle, float cos_angle,
							   enum side side, enum side shade_side,
							   uint8_t tex_x, uint8_t tex_y,
							   uint8_t light) {
	if(side < SIDE_TOP || side >= SIDE_MAX)
		return;

	// side = the model-space box face drawn; shade_side = the world face it
	// is shaded as. The post passes side for both (output unchanged).
	uint8_t shade = light;
	if(shade_side == SIDE_LEFT || shade_side == SIDE_RIGHT)
		shade = DIM_LIGHT(light, level_table_1, true, 0);
	else if(shade_side == SIDE_FRONT || shade_side == SIDE_BACK)
		shade = DIM_LIGHT(light, level_table_2, true, 0);
	else if(shade_side == SIDE_BOTTOM)
		shade = DIM_LIGHT(light, level_table_0, true, 0);

	uint16_t extent[3];
	for(size_t axis = 0; axis < 3; axis++)
		extent[axis] = (uint16_t)(box->bounds[axis][1]
			- box->bounds[axis][0]);
	uint8_t u1 = tex_x + TILE_CLAMP(extent[s_sign_box_uv_axes[side][0]]);
	uint8_t v1 = tex_y + TILE_CLAMP(extent[s_sign_box_uv_axes[side][1]]);
	for(size_t i = 0; i < 4; i++) {
		int16_t x = box->bounds[0][s_sign_box_corners[side][i][0]];
		int16_t y = box->bounds[1][s_sign_box_corners[side][i][1]];
		int16_t z = box->bounds[2][s_sign_box_corners[side][i][2]];
		int16_t rx = (int16_t)lroundf(cos_angle * x + sin_angle * z);
		int16_t rz = (int16_t)lroundf(-sin_angle * x + cos_angle * z);
		displaylist_vertex(d, base_x + rx, base_y + y, base_z + rz, shade,
			s_sign_box_uv_order[side][i][0] ? u1 : tex_x,
			s_sign_box_uv_order[side][i][1] ? v1 : tex_y);
	}
}

static size_t sign_post_side_helper(struct displaylist* d,
									struct block_info* this, enum side side,
									bool count_only) {
	if(side < SIDE_TOP || side >= SIDE_MAX)
		return 0;
	if(count_only)
		return 2; // one board face plus one stick face for each model side

	int16_t base_x = W2C_COORD(this->x) * BLK_LEN + BLK_LEN / 2;
	int16_t base_y = W2C_COORD(this->y) * BLK_LEN;
	int16_t base_z = W2C_COORD(this->z) * BLK_LEN + BLK_LEN / 2;
	float angle = -(float)(this->block->metadata & 0x0F)
		* (2.0F * GLM_PIf / 16.0F);
	float sin_angle = sinf(angle);
	float cos_angle = cosf(angle);
	uint8_t tex = block_texture_index(this, side);
	uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
	uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
	uint8_t light = (MAX_U8(this->block->torch_light,
								blocks[this->block->type]->luminance) << 4)
		| this->block->sky_light;

	// Board, then the vertical signStick. Constants are the rounded Beta
	// model values documented above, not a second derivation from this code.
	sign_post_box_side(d, base_x, base_y, base_z, &s_sign_post_board,
						sin_angle, cos_angle, side, side, tex_x, tex_y,
						light);
	sign_post_box_side(d, base_x, base_y, base_z, &s_sign_post_stick,
						sin_angle, cos_angle, side, side, tex_x, tex_y,
						light);
	return 2;
}



size_t render_block_trapdoor(struct displaylist* d,
                             struct block_info* this,
                             enum side side,
                             struct block_info* it,
                             uint8_t* vertex_light,
                             bool count_only)
{
    uint8_t m      = this->block->metadata;
    bool    open   = (m & 0x04) != 0;   // open-flag
    uint8_t orient = m & 0x03;          //  0..3

    if (open) {
        static const enum side map[4] = {
            SIDE_FRONT, SIDE_BACK,
            SIDE_LEFT,  SIDE_RIGHT
        };
        enum side front = map[orient];

        return door_side_helper(
            d, this,
            front, side,
            vertex_light,
	        !open,
	        open,
            count_only
        );
    } else {
        if (!count_only) {
            uint8_t tex = block_texture_index(this, side);
            uint8_t lumi = blocks[this->block->type]->luminance;
            render_block_side(
                d,
                W2C_COORD(this->x),
                W2C_COORD(this->y),
                W2C_COORD(this->z),
                /*y-offset*/   0,            // height 0..3/16
                /*thickness*/  BLK_LEN / 16 * 3,
                tex, lumi,
                /*double-sided=*/true,
                /*u=*/0, /*flip_u=*/false,
                /*v=*/0, /*side=*/side,
                vertex_light
            );
        }
        return 1;
    }
}

// Wall sign (BLOCK_SIGN/68): Beta's SignModel board through the WALL branch
// of [740c583 TileEntitySignRenderer.renderTileEntitySignAt] (decompile
// TileEntitySignRenderer.java:35-58): yaw 180/0/90/-90 for raw meta 2/3/4/5,
// translate(0,-0.3125,-0.4375) after the rotate, stick hidden. Bounds are
// SIGN_WALL_BOARD_* (render_sign_text.h, derivation there), shared with the
// sign-text builder so text and board cannot drift apart again. The old
// world-aligned 1/16 slab (y 0.25..0.75, flush on the wall) disagreed with
// the text transform: text floated ~0.16 block off it on real Wii.
static const struct sign_post_box s_sign_wall_board = {
	.bounds = {{SIGN_WALL_BOARD_X0, SIGN_WALL_BOARD_X1},
			   {SIGN_WALL_BOARD_Y0, SIGN_WALL_BOARD_Y1},
			   {SIGN_WALL_BOARD_Z0, SIGN_WALL_BOARD_Z1}},
};
// cos/sin of the applied angle -yaw, exact, indexed (meta - 2) & 3.
static const int8_t s_sign_wall_cos[4] = {-1, 1, 0, 0};
static const int8_t s_sign_wall_sin[4] = {0, 0, -1, 1};

static size_t sign_wall_side_helper(struct displaylist* d,
									struct block_info* this, enum side side,
									bool count_only) {
	if(side < SIDE_TOP || side >= SIDE_MAX)
		return 0;
	if(count_only)
		return 1;

	unsigned k = ((unsigned)this->block->metadata - 2u) & 0x03u;
	int c = s_sign_wall_cos[k];
	int s = s_sign_wall_sin[k];

	// Draw the model face that FACES this world side after the yaw, so the
	// mesher's per-side visibility applies to the right face (the yaw is a
	// multiple of 90, so the inverse rotation maps sides to sides exactly).
	enum side local = side;
	if(side != SIDE_TOP && side != SIDE_BOTTOM) {
		int wx = side == SIDE_RIGHT ? 1 : (side == SIDE_LEFT ? -1 : 0);
		int wz = side == SIDE_BACK ? 1 : (side == SIDE_FRONT ? -1 : 0);
		int lx = c * wx - s * wz;
		int lz = s * wx + c * wz;
		local = lx > 0 ? SIDE_RIGHT
			: lx < 0   ? SIDE_LEFT
			: lz > 0   ? SIDE_BACK
					   : SIDE_FRONT;
	}

	int16_t base_x = W2C_COORD(this->x) * BLK_LEN + BLK_LEN / 2;
	int16_t base_y = W2C_COORD(this->y) * BLK_LEN;
	int16_t base_z = W2C_COORD(this->z) * BLK_LEN + BLK_LEN / 2;
	uint8_t tex = block_texture_index(this, side);
	uint8_t light = (MAX_U8(this->block->torch_light,
								blocks[this->block->type]->luminance) << 4)
		| this->block->sky_light;

	sign_post_box_side(d, base_x, base_y, base_z, &s_sign_wall_board,
						(float)s, (float)c, local, side,
						TEX_OFFSET(TEXTURE_X(tex)), TEX_OFFSET(TEXTURE_Y(tex)),
						light);
	return 1;
}

size_t render_block_sign(struct displaylist* d, struct block_info* this,
							 enum side side, struct block_info* it,
							 uint8_t* vertex_light, bool count_only) {
	(void)it;
	if(this->block->type == BLOCK_SIGN_POST)
		return sign_post_side_helper(d, this, side, count_only);

	(void)vertex_light;
	return sign_wall_side_helper(d, this, side, count_only);
}

size_t render_block_door(struct displaylist* d,
                         struct block_info* this,
                         enum side side,
                         struct block_info* it,
                         uint8_t* vertex_light,
                         bool count_only)
{
    uint8_t dir  = this->block->metadata & 0x03;
    bool    open = (this->block->metadata & 0x04) != 0;
    // Beta RenderBlocks.renderBlockDoor (RenderBlocks.java:2548) draws the block's CURRENT
    // bounds, i.e. BlockDoor.setDoorRotation(getState(meta)) (BlockDoor.java:60-83, 180-182),
    // the same box block_door.c's getBoundingBox returns for selection/collision/ray-trace.
    // This table's index is Beta getState + 1 (frontForDir[k] == setDoorRotation((k-1)&3)), and
    // getState is meta&3 when bit 2 is set, else (meta-1)&3 -- so the index is dir+1 open and
    // dir closed. critique-1028: the open branch used dir+3, drawing every bit-2 door (opened,
    // or hinge-flipped at placement by ItemDoor) on the OPPOSITE edge of the cell from its
    // click/collision box. Pinned by tests/test_door_bounds_golden.c.
    uint8_t state = open ? (uint8_t)((dir + 1) & 3) : dir;

    static const enum side frontForDir[4] = {
        SIDE_RIGHT,  // 0 = east
        SIDE_BACK,   // 1 = south
        SIDE_LEFT,   // 2 = west
        SIDE_FRONT   // 3 = north
    };

    enum side front = frontForDir[state];

    // The two broad faces (front + its opposite) carry the hinge mirror and already sample
    // Beta's texels (asserted by test_door_bounds_golden): unchanged.
    if(side == front || side == blocks_side_opposite(front) || side < SIDE_TOP
       || side >= SIDE_MAX)
        return door_side_helper(d, this, front, side, vertex_light, !open, open,
                                count_only);
    if(count_only)
        return 1;

    // P3 DOOR-EDGE-UV: the 3/16 edge strips and the top/bottom faces sample the
    // bounds-clipped texel columns, exactly as renderStandardBlock's face functions do for
    // the door's current box (decompile McBetaCpp/srcorigjava/RenderBlocks.java):
    //   renderBlockDoor :2733-2843 draws bottom/top with sides 0/1 and the east/west/north/
    //     south faces with sides 2-5; flipTexture is only set for a negative texture, which
    //     BlockDoor returns only for the broad faces, so these faces are never mirrored.
    //   renderBottomFace :2846-2937 / renderTopFace :2939+   U = j + minX..maxX*16,
    //     V = k + minZ..maxZ*16 (field_31082_l / field_31083_k == 0 for doors), with the
    //     vertex at (minX,minZ) sampling (U(minX), V(minZ)) on both faces.
    //   renderEastFace :3032 (z = minZ) / renderWestFace :3131 (z = maxZ)  U from minX*16;
    //   renderNorthFace :3230 (x = minX) / renderSouthFace :3329 (x = maxX) U from minZ*16;
    //   all four V = k + 16 - maxY*16 .. 16 - minY*16 = the full tile for a 0..1 door.
    // The old door_side_helper path always started U at texel 0 (and V at 0 on top/bottom),
    // so an edge strip at 13/16..1 drew texels 0..3 instead of 13..16, and the bottom face
    // was emitted rotated 180 degrees. Box minimum per front (same table as above):
    // front LEFT -> x in [13/16,1]; front FRONT -> z in [13/16,1]; else the min is 0.
    uint8_t tex = block_texture_index(this, side);
    uint8_t luminance = blocks[this->block->type]->luminance;
    uint8_t tex_x = TEX_OFFSET(TEXTURE_X(tex));
    uint8_t tex_y = TEX_OFFSET(TEXTURE_Y(tex));
    int16_t x = W2C_COORD(this->x);
    int16_t y = W2C_COORD(this->y);
    int16_t z = W2C_COORD(this->z);
    uint8_t min_x16 = front == SIDE_LEFT ? 13 : 0;
    uint8_t min_z16 = front == SIDE_FRONT ? 13 : 0;
    bool thin_x = front == SIDE_LEFT || front == SIDE_RIGHT;

    if(side == SIDE_TOP || side == SIDE_BOTTOM) {
        // render_block_side_adv_v2 maps TOP as U=+x, V=+z with (flip 0, rotate 0); its
        // BOTTOM corner order needs (flip 1, rotate 2) for the same U=+x, V=+z mapping.
        render_block_side_adv(
            d, x * BLK_LEN + min_x16 * (BLK_LEN / 16),
            y * BLK_LEN + (side == SIDE_TOP ? BLK_LEN : 0),
            z * BLK_LEN + min_z16 * (BLK_LEN / 16),
            thin_x ? BLK_LEN / 16 * 3 : BLK_LEN,
            thin_x ? BLK_LEN : BLK_LEN / 16 * 3,
            tex_x + min_x16, tex_y + min_z16,
            side == SIDE_BOTTOM, side == SIDE_BOTTOM ? 2 : 0, true, side,
            vertex_light, luminance);
    } else {
        // Edge strip: LEFT/RIGHT strips span z (U from minZ), FRONT/BACK span x (U from
        // minX). render_block_side_adv_v2's unflipped corner order already gives U(min) at
        // the face's min edge in Beta's orientation for all four sides.
        render_block_side_adv(
            d,
            x * BLK_LEN + (side == SIDE_RIGHT ? BLK_LEN : 0)
                + min_x16 * (BLK_LEN / 16),
            y * BLK_LEN,
            z * BLK_LEN + (side == SIDE_BACK ? BLK_LEN : 0)
                + min_z16 * (BLK_LEN / 16),
            BLK_LEN / 16 * 3, BLK_LEN,
            tex_x + ((side == SIDE_LEFT || side == SIDE_RIGHT) ? min_z16 : min_x16),
            tex_y, false, 0, true, side, vertex_light, luminance);
    }
    return 1;
}


size_t render_block_layer(struct displaylist* d, struct block_info* this,
						  enum side side, struct block_info* it,
						  uint8_t* vertex_light, bool count_only) {
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			(this->block->metadata + 1) * 32,
			block_texture_index(this, side),
			blocks[this->block->type]->luminance, true, 0, false, 0, side,
			vertex_light);
	return 1;
}

size_t render_block_slab(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only) {
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			128, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true, 0, false, 0, side,
			vertex_light);
	return 1;
}

size_t render_block_full(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only) {
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			BLK_LEN, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true, 0, false, 0, side,
			vertex_light);
	return 1;
}

// DC-GRASS G2: one full-cube face with an EXPLICIT atlas cell -- the grass
// side overlay (Beta tile 38) that the chunk mesher emits over the base side.
// Beta re-renders the SAME face with texture 38 (RenderBlocks.java:2276-2281
// flat, 1954-1970 AO): identical geometry, UVs and brightness, so this is
// render_block_full's exact render_block_side call with only the texture
// argument swapped (vertex positions, light bytes and texcoord layout are
// byte-for-byte those of the base quad). Returns 1 quad in count_only too, so
// count-only sizing and emission always agree.
size_t render_block_full_tex(struct displaylist* d, struct block_info* this,
							 enum side side, uint8_t* vertex_light,
							 uint8_t tex, bool count_only) {
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			BLK_LEN, tex, blocks[this->block->type]->luminance, true, 0, false,
			0, side, vertex_light);
	return 1;
}

size_t render_block_furnace(struct displaylist* d, struct block_info* this,
						 enum side side, struct block_info* it,
						 uint8_t* vertex_light, bool count_only) {
	//TODO: luminance equals to metadata
	if(!count_only)
		render_block_side(
			d, W2C_COORD(this->x), W2C_COORD(this->y), W2C_COORD(this->z), 0,
			BLK_LEN, block_texture_index(this, side),
			blocks[this->block->type]->luminance, true, 0, false, 0, side,
			vertex_light);
	return 1;
}


static struct displaylist block_cracks_dl;
static uint8_t block_cracks_light[24];

// Toward-the-eye depth bias of the crack overlay, as a fraction of the depth
// range (see render_block_cracks_owned): 2^-20 = 16 LSB of a 24-bit depth
// buffer (PC's GLFW default, the Wii EFB) at the far end of the range, less
// nearer the eye. 1 - 2^-20 is exactly representable in float on both
// backends (and (1 - 2^-20) * 16777215 is an integer-valued float, so the GX
// viewport z scale/offset registers carry it exactly).
#define BLOCK_CRACKS_DEPTH_BIAS (1.0F / 1048576.0F)

void render_block_init() {
	displaylist_init(&block_cracks_dl, 48, 3 * 2 + 2 * 1 + 1);
	memset(block_cracks_light, 0xFF, sizeof(block_cracks_light));
    for (int s = SIDE_TOP; s < SIDE_MAX; ++s) {
    }
}

static uint8_t block_cracks_texture(struct block_info* this, enum side side) {
	const struct client_session* owner = this->get_block_ctx;
	struct item_data it;
	inventory_get_hotbar_item(
		windowc_get_latest(owner->windows[WINDOWC_INVENTORY]), &it);
	// The stage rule itself lives in tool_dig_crack_stage() (item/tool.c) so the
	// overlay and the dig can never drift apart: one function, one delay.
	//
	// THE ENVIRONMENT COMES FROM THIS SESSION, not from a second copy of the
	// rule (review B17 H1). These are the very two fields the dig reads at
	// screen_ingame.c:563-567 for its own tool_dig_delay_env_ms call:
	// `in_water` is main.c's eye-cell water probe (main.c:4159-4170, the
	// engine's closest predicate to Beta's isInsideOfMaterial(Material.water))
	// and `on_ground` is the body's own flag, spelled exactly as the
	// SRPC_PLAYER_STATE send reads it. A no-body frame answers false, matching
	// the dig's `p->local_player ? ... : false`.
	bool dig_on_ground
		= owner->local_player ? owner->local_player->on_ground : false;
	int dt = tool_dig_crack_stage(blocks[this->block->type], item_get(&it),
								  owner->in_water, dig_on_ground,
								  time_diff_ms(owner->digging.start,
											   time_get()));

	return tex_atlas_lookup(TEXAT_BREAK_0 + dt);
}

// block_info.get_block backing for the cracks overlay -- MAIN thread, so reading the live world
// is safe (unlike the mesher worker). Lets a connection-aware renderer probe diagonals here too;
// world_get_block returns air for unloaded coords, so this always "succeeds".
static bool cracks_neighbour_get(void* vctx, w_coord_t x, w_coord_t y, w_coord_t z,
								 struct block_data* out) {
	(void)vctx;
	/* A2-W: the world of the view being drawn (gstate.world by default) */
	*out = world_get_block(world_view_current(), x, y, z);
	return true;
}

void render_block_cracks(struct block_data* blk, mat4 view, w_coord_t x,
						 w_coord_t y, w_coord_t z) {
	render_block_cracks_owned(&gstate.players[0], blk, view, x, y, z);
}

void render_block_cracks_owned(struct client_session* owner,
	struct block_data* blk, mat4 view, w_coord_t x, w_coord_t y, w_coord_t z) {
	assert(blk && view);
	struct block* b = blocks[blk->type];

	if(b && b->digging.hardness > 0) {
		displaylist_reset(&block_cracks_dl);

		struct block_data neighbours[6];
		struct block_info neighbours_info[6];

		for(int k = 0; k < SIDE_MAX; k++) {
			int ox, oy, oz;
			blocks_side_offset(k, &ox, &oy, &oz);

			neighbours[k]
				= world_get_block(world_view_current(), x + ox, y + oy, z + oz);

			neighbours_info[k] = (struct block_info) {
				.block = neighbours + k,
				.neighbours = NULL,
				.x = x + ox,
				.y = y + oy,
				.z = z + oz,
			};
		}

		struct block_info local_info = (struct block_info) {
			.block = blk,
			.neighbours = neighbours,
			.x = x,
			.y = y,
			.z = z,
			.get_block = cracks_neighbour_get,
			.get_block_ctx = owner,
			// Per-call override (block_texture_index() in this file consults
			// it) instead of swapping the block's GLOBAL getTextureIndex
			// vtable slot -- that slot is also read by the mesher thread
			// (chunk_mesher.c:lb->getTextureIndex), so mutating it here raced
			// live gameplay state. local_info is stack-local to this render-
			// thread call, so this override never touches blocks[] and is
			// never visible to any other thread.
			.texture_override = block_cracks_texture,
		};

		size_t vertices = 0;

		for(int k = 0; k < SIDE_MAX; k++) {
			vertices += b->renderBlock(&block_cracks_dl, &local_info, k,
									   neighbours_info + k, block_cracks_light,
									   false);
			if(b->renderBlockAlways)
				vertices += b->renderBlockAlways(&block_cracks_dl, &local_info,
												 k, neighbours_info + k,
												 block_cracks_light, false);
		}

		mat4 mv;
		glm_translate_to(view,
						 (vec3) {WCOORD_CHUNK_OFFSET(x) * CHUNK_SIZE,
								 y / CHUNK_SIZE * CHUNK_SIZE,
								 WCOORD_CHUNK_OFFSET(z) * CHUNK_SIZE},
						 mv);
		gfx_matrix_modelview(mv);

		// Beta RenderGlobal.drawBlockBreaking (decompile :1078-1106): blend
		// DST_COLOR/SRC_COLOR, alpha test ON (:1095, the tile's clear texels
		// drop), the depth FUNCTION untouched (GL_LEQUAL, EntityRenderer's
		// default), and the crack quad pulled toward the eye with
		// glPolygonOffset(-3,-3) + GL_POLYGON_OFFSET_FILL (:1086-1087) --
		// then the offset reset (:1103-1104).
		//
		// This used to draw with depth EQUAL, which only passes where the
		// face itself wrote depth: an alpha-tested face (glass, leaves)
		// never writes depth on its discarded see-through texels, so the
		// crack fragments there compared against whatever lay BEHIND the
		// pane and were rejected -- a full glass dig showed no crack at all
		// (push-0921/h1-capture runs 2-4). LEQUAL passes on the face's own
		// depth (the chunk pass and this pass build the identical modelview,
		// chunk.c:754 -- the bit-identity EQUAL relied on) AND on the farther
		// depth under a discarded texel, while a nearer wall still hides it.
		//
		// Neither backend has polygon offset (GX has none); the engine's
		// cross-platform depth-offset mechanism is the depth-range remap,
		// the same tool the held item (screen_ingame.c) and the GUI layers
		// (gui_util.c) use: glDepthRange on PC, GX_SetViewport near/far on
		// Wii, both the affine map z' = near + z * (far - near). Shrinking
		// the range by BLOCK_CRACKS_DEPTH_BIAS scales every crack depth by
		// (1 - bias): never behind its own face, and at most ~16 LSB of the
		// 24-bit buffer nearer -- the same order as Beta's 3-unit offset, far
		// too small to punch through a neighbouring block. Restored to (0,1)
		// right after, before the caller's selection box / held item.
		// Pinned by tests/test_crack_overlay_depth.c.
		// Beta also runs the damage pass with depth writes OFF (it restores
		// glDepthMask(true) at RenderGlobal.java:1106) and the world alpha cut
		// GL_GREATER 0.1F (EntityRenderer.java:773): a depth-writing crack
		// would hide water/mobs behind glass and cull the later ice pass.
		gfx_blending(MODE_BLEND3);
		gfx_alpha_test_ref(true, 0.1F);
		gfx_write_buffers(true, false, true);
		gfx_depth_func(MODE_LEQUAL);
		gfx_depth_range(0.0F, 1.0F - BLOCK_CRACKS_DEPTH_BIAS);

		if(b->double_sided)
			gfx_cull_func(MODE_NONE);

		gfx_bind_texture(&texture_terrain);
		displaylist_render_immediate(&block_cracks_dl, vertices * 4);

		if(b->double_sided)
			gfx_cull_func(MODE_BACK);

		gfx_depth_range(0.0F, 1.0F);
		gfx_depth_func(MODE_LEQUAL);
		gfx_write_buffers(true, true, true);
		gfx_alpha_test(true);
		gfx_blending(MODE_OFF);
	}
}
