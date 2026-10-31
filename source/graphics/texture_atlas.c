/*
	Copyright (c) 2023 ByteBit/xtreme8000

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
#include <stddef.h>
#include <stdio.h>

#include "../platform/texture.h"
#include "texture_atlas.h"

static uint8_t global_block_atlas[TEXAT_MAX];
static uint8_t global_particle_atlas[TEXAT_MAX];

bool tex_atlas_tile_uv(size_t width, size_t height, uint8_t tile, float uv[4]) {
	struct tex_atlas_grid grid;
	if(!uv || tile >= 14 * 14 || !tex_atlas_grid_init(&grid, width, height))
		return false;
	size_t x = grid.origin_x + grid.border_pixels
		+ TEXTURE_X(tile) * grid.cell_pitch;
	size_t y = grid.origin_y + grid.border_pixels
		+ TEXTURE_Y(tile) * grid.cell_pitch;
	if(x + grid.content_pixels > width || y + grid.content_pixels > height)
		return false;
	uv[0] = (float)x / (float)width;
	uv[1] = (float)y / (float)height;
	uv[2] = uv[0] + (float)grid.content_pixels / (float)width;
	uv[3] = uv[1] + (float)grid.content_pixels / (float)height;
	return true;
}

static int clamp_n(int x, int n) {
	if(x < 0)
		return 0;
	if(x > n - 1)
		return n - 1;
	return x;
}

static const uint8_t redstone_colors[16][3] = {
    {111,   0,  0},  // 0: off
    {120,   3,  0},  // 1
    {130,   7,  0},  // 2
    {139,  10,  0},  // 3
    {149,  13,  0},  // 4
    {158,  16,  0},  // 5
    {167,  20,  0},  // 6
    {177,  23,  0},  // 7
    {186,  26,  0},  // 8
    {196,  29,  0},  // 9
    {205,  33,  0},  // 10
    {214,  36,  0},  // 11
    {224,  39,  0},  // 12
    {233,  42,  0},  // 13
    {243,  46,  0},  // 14
    {252,  49,  0}   // 15: on
};

void tex_atlas_reg(dict_atlas_src_t atlas, enum tex_atlas_entry name, uint8_t x,
				   uint8_t y) {
	dict_atlas_src_push_back(atlas,
							 (struct texture_entry) {
								 .name = name,
								 .x = x,
								 .y = y,
								 .bg.enable = false,
								 .colorize.enable = false,
							 });
}

void tex_atlas_reg_col(dict_atlas_src_t atlas, enum tex_atlas_entry name,
					   uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b) {
	dict_atlas_src_push_back(atlas,
							 (struct texture_entry) {
								 .name = name,
								 .x = x,
								 .y = y,
								 .bg.enable = false,
								 .colorize.enable = true,
								 .colorize.r = r,
								 .colorize.g = g,
								 .colorize.b = b,
							 });
}

static void tex_atlas_reg_opaque_col(dict_atlas_src_t atlas,
									 enum tex_atlas_entry name, uint8_t x,
									 uint8_t y, uint8_t from_x, uint8_t from_y,
									 uint8_t r, uint8_t g, uint8_t b) {
	dict_atlas_src_push_back(atlas,
							 (struct texture_entry) {
								 .name = name,
								 .x = x,
								 .y = y,
								 .colorize.enable = true,
								 .colorize.r = r,
								 .colorize.g = g,
								 .colorize.b = b,
								 .opaque_from.enable = true,
								 .opaque_from.x = from_x,
								 .opaque_from.y = from_y,
							 });
}

static void tex_atlas_reg_opaque(dict_atlas_src_t atlas,
								 enum tex_atlas_entry name, uint8_t x, uint8_t y,
								 uint8_t from_x, uint8_t from_y) {
	dict_atlas_src_push_back(atlas,
							 (struct texture_entry) {
								 .name = name,
								 .x = x,
								 .y = y,
								 .opaque_from.enable = true,
								 .opaque_from.x = from_x,
								 .opaque_from.y = from_y,
							 });
}

void tex_atlas_reg_grass(dict_atlas_src_t atlas, enum tex_atlas_entry name,
						 uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b,
						 uint8_t bg_x, uint8_t bg_y) {
	dict_atlas_src_push_back(atlas,
							 (struct texture_entry) {
								 .name = name,
								 .x = x,
								 .y = y,
								 .bg.enable = true,
								 .bg.x = bg_x,
								 .bg.y = bg_y,
								 .colorize.enable = true,
								 .colorize.r = r,
								 .colorize.g = g,
								 .colorize.b = b,
							 });
}

// Live-water slot, captured when the block atlas is built so the animated water FX
// (water_fx, Beta TextureWaterFX) can blit its 16x16 tile into the EXACT terrain-atlas
// pixel rect each tick without re-deriving the packing formula. -1 until captured.
int g_water_atlas_x = -1, g_water_atlas_y = -1, g_water_atlas_tile = 0;

void* tex_atlas_compute(dict_atlas_src_t atlas, uint8_t* atlas_dst,
						uint8_t* image, size_t width, size_t height) {
	// This serves BOTH the 256x256 block atlas and the 128x128 particle atlas
	// (and HD packs at higher 16-multiples). It only needs a non-null, square
	// image whose side is a multiple of 16: one tile = width/16, and
	// border_scale = width/256 is simply 0 below 256 (no borders), which the
	// loops below handle correctly. The old `width >= 256` assert wrongly
	// rejected the 128px particle atlas on asserts-enabled builds (the PC
	// build) — the Wii build only "passed" because NDEBUG compiled it out.
	//
	// RUNTIME gate, not just an assert: this is USER texture-pack input and the
	// shipping Wii build is NDEBUG (asserts stripped) — a missing/corrupt
	// terrain.png arrives as image==NULL (with caller-uninitialized w/h), and a
	// non-square or non-16-multiple one would walk the tile loops out of
	// bounds: freeze-class on the Wii. Reject instead — the callers
	// (tex_atlas_block/particles -> tex_init) already tolerate NULL, so the
	// previous/unloaded texture is kept and the game stays alive.
	if(!image) {
		fprintf(stderr, "[texture] atlas source missing/unreadable -- rejected\n");
		return NULL;
	}
	if(width != height || width < 16 || (width % 16) != 0) {
		fprintf(stderr,
				"[texture] atlas source rejected: %ux%u (need square, >=16, "
				"multiple of 16)\n",
				(unsigned)width, (unsigned)height);
		return NULL;
	}
	if(height > SIZE_MAX / width || width * height > SIZE_MAX / 4) {
		fprintf(stderr, "[texture] atlas source dimensions overflow allocation\n");
		return NULL;
	}

	struct tex_atlas_grid grid;
	if(!tex_atlas_grid_init(&grid, width, height))
		return NULL;
	int tile_size = (int)grid.content_pixels;
	int border_scale = (int)grid.border_pixels;

	uint8_t* output = malloc(width * height * 4);

	if(!output)
		return NULL;

	memset(output, 255, width * height * 4);

	dict_atlas_src_it_t it;
	dict_atlas_src_it(it, atlas);

	int current = 0;

	while(!dict_atlas_src_end_p(it)) {
		struct texture_entry* e = dict_atlas_src_ref(it);

		size_t current_x = (current % 14) * (tile_size + 2 * border_scale)
			+ 3 * border_scale;
		size_t current_y = (current / 14) * (tile_size + 2 * border_scale)
			+ 3 * border_scale;

		// Record the live-water slot for water_fx (only the block atlas registers
		// TEXAT_WATER_STATIC, so this fires once, for the correct atlas).
		if(e->name == TEXAT_WATER_STATIC) {
			g_water_atlas_x = (int)current_x;
			g_water_atlas_y = (int)current_y;
			g_water_atlas_tile = tile_size;
		}

		// Capacity guard: once tiles fill past the atlas bottom edge, the dst_col write
		// below would run off the end of `output` (heap overflow). Stop blitting -- the
		// remaining tiles keep the memset default rather than corrupting memory.
		if(current_y + (size_t)(tile_size + border_scale) > (size_t)height) {
			fprintf(stderr,
					"[texture] atlas overflow: %d entries exceed %ux%u capacity; extras skipped\n",
					current, (unsigned)width, (unsigned)height);
			break;
		}

		/* opaque_from: a pack with no texel in the requested cell falls back
		 * to the named cell, holes filled with its darkened opaque mean. */
		uint8_t src_x = e->x, src_y = e->y;
		bool fill_holes = false;
		uint8_t fill[3] = {0, 0, 0};
		if(e->opaque_from.enable) {
			bool any_opaque = false;
			for(int v = 0; v < tile_size && !any_opaque; v++)
				for(int u = 0; u < tile_size; u++)
					if(image[((e->x * tile_size + u)
							  + (e->y * tile_size + v) * width) * 4 + 3]
					   >= 128) {
						any_opaque = true;
						break;
					}
			if(!any_opaque) {
				src_x = e->opaque_from.x;
				src_y = e->opaque_from.y;
				fill_holes = true;
				uint32_t sum[3] = {0, 0, 0}, n = 0;
				for(int v = 0; v < tile_size; v++)
					for(int u = 0; u < tile_size; u++) {
						const uint8_t* p = image
							+ ((src_x * tile_size + u)
							   + (src_y * tile_size + v) * width) * 4;
						if(p[3] >= 128) {
							for(int k = 0; k < 3; k++)
								sum[k] += p[k];
							n++;
						}
					}
				for(int k = 0; k < 3; k++) /* half brightness: a shadowed gap */
					fill[k] = n ? (uint8_t)(sum[k] / n / 2) : 0;
			}
		}

		// int64_t to prevent unreasonable gcc warning
		for(int64_t y = -border_scale; y < tile_size + border_scale; y++) {
			for(int64_t x = -border_scale; x < tile_size + border_scale; x++) {
				uint8_t* src_col = image
					+ ((clamp_n(x, tile_size) + src_x * tile_size)
					   + (clamp_n(y, tile_size) + src_y * tile_size) * width)
						* 4;
				uint8_t* dst_col
					= output + ((current_x + x) + (current_y + y) * width) * 4;
				uint8_t filled[4];
				if(fill_holes) {
					bool hole = src_col[3] < 128;
					for(int k = 0; k < 3; k++)
						filled[k] = hole ? fill[k] : src_col[k];
					filled[3] = 255;
					src_col = filled;
				}

				if(e->colorize.enable) {
					dst_col[0] = src_col[0] * e->colorize.r / 255;
					dst_col[1] = src_col[1] * e->colorize.g / 255;
					dst_col[2] = src_col[2] * e->colorize.b / 255;
					dst_col[3] = src_col[3];
				} else {
					for(size_t k = 0; k < 4; k++)
						dst_col[k] = src_col[k];
				}

				if(e->bg.enable && dst_col[3] < 128) {
					uint8_t* src_col2 = image
						+ ((clamp_n(x, tile_size) + e->bg.x * tile_size)
						   + (clamp_n(y, tile_size) + e->bg.y * tile_size)
							   * width)
							* 4;
					for(size_t k = 0; k < 4; k++)
						dst_col[k] = src_col2[k];
				}
			}
		}

		atlas_dst[e->name] = TEXTURE_INDEX(current % 14, current / 14);

		current++;
		dict_atlas_src_next(it);
	}

	return output;
}

void tex_atlas_tables_export(uint8_t* block, uint8_t* particle) {
	memcpy(block, global_block_atlas, sizeof(global_block_atlas));
	memcpy(particle, global_particle_atlas, sizeof(global_particle_atlas));
}

void tex_atlas_tables_import(const uint8_t* block, const uint8_t* particle) {
	memcpy(global_block_atlas, block, sizeof(global_block_atlas));
	memcpy(global_particle_atlas, particle, sizeof(global_particle_atlas));
}

uint8_t tex_atlas_lookup(enum tex_atlas_entry name) {
	return global_block_atlas[name];
}

uint8_t tex_atlas_lookup_particle(enum tex_atlas_entry name) {
	return global_particle_atlas[name];
}

void* tex_atlas_block(const char* filename, size_t* width, size_t* height) {
	assert(filename && width && height);
	if(!filename || !width || !height)
		return NULL;
	*width = 0;
	*height = 0;
	dict_atlas_src_t atlas;
	dict_atlas_src_init(atlas);

	tex_atlas_reg(atlas, TEXAT_STONE, 1, 0);
	tex_atlas_reg(atlas, TEXAT_DIRT, 2, 0);
	tex_atlas_reg(atlas, TEXAT_PLANKS, 4, 0);
	tex_atlas_reg(atlas, TEXAT_SLAB_STONE_SIDE, 5, 0);
	tex_atlas_reg(atlas, TEXAT_SLAB_STONE_TOP, 6, 0);
	tex_atlas_reg(atlas, TEXAT_BRICKS, 7, 0);
	tex_atlas_reg(atlas, TEXAT_TNT_SIDE, 8, 0);
	tex_atlas_reg(atlas, TEXAT_TNT_TOP, 9, 0);
	tex_atlas_reg(atlas, TEXAT_TNT_BOTTOM, 10, 0);
	tex_atlas_reg(atlas, TEXAT_COBWEB, 11, 0);
	tex_atlas_reg(atlas, TEXAT_ROSE, 12, 0);
	tex_atlas_reg(atlas, TEXAT_DANDELION, 13, 0);
	// P5(c): Beta water-still is terrain index 205 = (13,12) -- BlockFluid.java:7
	// `(material==lava?14:12)*16+13`. This was registered at (14,0), which is Beta's
	// PORTAL placeholder tile (a flat single colour), so the water_fx live upload was
	// overwriting the wrong cell. Verified against the decompile 2026-07-30.
	tex_atlas_reg(atlas, TEXAT_WATER_STATIC, 13, 12);
	tex_atlas_reg(atlas, TEXAT_SAPLING_OAK, 15, 0);

	tex_atlas_reg(atlas, TEXAT_COBBLESTONE, 0, 1);
	tex_atlas_reg(atlas, TEXAT_BEDROCK, 1, 1);
	tex_atlas_reg(atlas, TEXAT_SAND, 2, 1);
	tex_atlas_reg(atlas, TEXAT_GRAVEL, 3, 1);
	tex_atlas_reg(atlas, TEXAT_LOG_OAK_SIDE, 4, 1);
	tex_atlas_reg(atlas, TEXAT_LOG_OAK_TOP, 5, 1);
	tex_atlas_reg(atlas, TEXAT_CAST_BLOCK_IRON, 6, 1);
	tex_atlas_reg(atlas, TEXAT_CAST_BLOCK_GOLD, 7, 1);
	tex_atlas_reg(atlas, TEXAT_CAST_BLOCK_DIAMOND, 8, 1);
	tex_atlas_reg(atlas, TEXAT_CHEST_TOP, 9, 1);
	tex_atlas_reg(atlas, TEXAT_CHEST_SIDE, 10, 1);
	tex_atlas_reg(atlas, TEXAT_CHEST_FRONT_SINGLE, 11, 1);
	tex_atlas_reg(atlas, TEXAT_MUSHROOM_RED, 12, 1);
	tex_atlas_reg(atlas, TEXAT_MUSHROOM_BROWN, 13, 1);

	tex_atlas_reg(atlas, TEXAT_ORE_GOLD, 0, 2);
	tex_atlas_reg(atlas, TEXAT_ORE_IRON, 1, 2);
	tex_atlas_reg(atlas, TEXAT_ORE_COAL, 2, 2);
	tex_atlas_reg(atlas, TEXAT_BOOKSHELF, 3, 2);
	tex_atlas_reg(atlas, TEXAT_COBBLESTONE_MOSSY, 4, 2);
	tex_atlas_reg(atlas, TEXAT_OBSIDIAN, 5, 2);
	tex_atlas_reg(atlas, TEXAT_CHEST_FRONT_1, 9, 2);
	tex_atlas_reg(atlas, TEXAT_CHEST_FRONT_2, 10, 2);
	tex_atlas_reg(atlas, TEXAT_WORKBENCH_TOP, 11, 2);
	tex_atlas_reg(atlas, TEXAT_FURNACE_FRONT, 12, 2);
	tex_atlas_reg(atlas, TEXAT_FURNACE_SIDE, 13, 2);
	tex_atlas_reg(atlas, TEXAT_DISPENSER_FRONT, 14, 2);

	tex_atlas_reg(atlas, TEXAT_SPONGE, 0, 3);
	tex_atlas_reg(atlas, TEXAT_GLASS, 1, 3);
	tex_atlas_reg(atlas, TEXAT_ORE_DIAMOND, 2, 3);
	tex_atlas_reg(atlas, TEXAT_ORE_REDSTONE, 3, 3);
	tex_atlas_reg_col(atlas, TEXAT_LEAVES_BIRCH, 4, 3, 128, 167, 85);
	tex_atlas_reg(atlas, TEXAT_DEADBUSH, 7, 3);
	tex_atlas_reg(atlas, TEXAT_CHEST_BACK_1, 9, 3);
	tex_atlas_reg(atlas, TEXAT_CHEST_BACK_2, 10, 3);
	tex_atlas_reg(atlas, TEXAT_WORKBENCH_SIDE_1, 11, 3);
	tex_atlas_reg(atlas, TEXAT_WORKBENCH_SIDE_2, 12, 3);
	tex_atlas_reg(atlas, TEXAT_FURNACE_FRONT_LIT, 13, 3);
	tex_atlas_reg(atlas, TEXAT_FURNACE_TOP, 14, 3);
	tex_atlas_reg(atlas, TEXAT_SAPLING_SPRUCE, 15, 3);

	tex_atlas_reg(atlas, TEXAT_SPAWNER, 1, 4);
	tex_atlas_reg(atlas, TEXAT_SNOW, 2, 4);
	tex_atlas_reg(atlas, TEXAT_GRASS_SIDE_SNOW, 4, 4);
	tex_atlas_reg(atlas, TEXAT_CACTUS_TOP, 5, 4);
	tex_atlas_reg(atlas, TEXAT_CACTUS_SIDE, 6, 4);
	tex_atlas_reg(atlas, TEXAT_CACTUS_BOTTOM, 7, 4);
	tex_atlas_reg(atlas, TEXAT_CLAY, 8, 4);
	tex_atlas_reg(atlas, TEXAT_REED, 9, 4);
	tex_atlas_reg(atlas, TEXAT_JUKBEBOX_SIDE, 10, 4);
	tex_atlas_reg(atlas, TEXAT_JUKBEBOX_TOP, 11, 4);
	tex_atlas_reg(atlas, TEXAT_SAPLING_BIRCH, 15, 4);

	tex_atlas_reg(atlas, TEXAT_TORCH, 0, 5);
	tex_atlas_reg(atlas, TEXAT_DOOR_WOOD_TOP, 1, 5);
	tex_atlas_reg(atlas, TEXAT_DOOR_IRON_TOP, 2, 5);
	tex_atlas_reg(atlas, TEXAT_LADDER, 3, 5);
	tex_atlas_reg(atlas, TEXAT_TRAPDOOR, 4, 5);
	tex_atlas_reg(atlas, TEXAT_FARMLAND_WET, 6, 5);
	tex_atlas_reg(atlas, TEXAT_FARMLAND_DRY, 7, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_0, 8, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_1, 9, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_2, 10, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_3, 11, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_4, 12, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_5, 13, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_6, 14, 5);
	tex_atlas_reg(atlas, TEXAT_CROPS_7, 15, 5);

	tex_atlas_reg(atlas, TEXAT_LEVER, 0, 6); // Beta lever idx 96 = tile (0,6)
	tex_atlas_reg(atlas, TEXAT_DOOR_WOOD_BOTTOM, 1, 6);
	tex_atlas_reg(atlas, TEXAT_DOOR_IRON_BOTTOM, 2, 6);
	tex_atlas_reg(atlas, TEXAT_REDSTONE_TORCH_LIT, 3, 6);
	tex_atlas_reg(atlas, TEXAT_PUMPKIN_TOP, 6, 6);
	tex_atlas_reg(atlas, TEXAT_NETHERRACK, 7, 6);
	tex_atlas_reg(atlas, TEXAT_SOULSAND, 8, 6);
	tex_atlas_reg(atlas, TEXAT_GLOWSTONE, 9, 6);
	/*tex_atlas_reg(atlas, "piston_front_sticky", 10, 6);
	tex_atlas_reg(atlas, "piston_front", 11, 6);
	tex_atlas_reg(atlas, "piston_side", 12, 6);
	tex_atlas_reg(atlas, "piston_back", 13, 6);
	tex_atlas_reg(atlas, "piston_inner", 14, 6);*/

	tex_atlas_reg(atlas, TEXAT_RAIL_CURVED, 0, 7);
	tex_atlas_reg(atlas, TEXAT_REDSTONE_TORCH, 3, 7);
	// repeater top faces: idle (3,8)=idx131, active (3,9)=idx147 (Isabella II, CC BY 3.0)
	tex_atlas_reg(atlas, TEXAT_REPEATER_TOP_OFF, 3, 8);
	tex_atlas_reg(atlas, TEXAT_REPEATER_TOP_ON, 3, 9);
	// piston faces: Beta terrain idx 106-110 == tiles (10,6)..(14,6)
	tex_atlas_reg(atlas, TEXAT_PISTON_TOP_STICKY, 10, 6);
	tex_atlas_reg(atlas, TEXAT_PISTON_TOP, 11, 6);
	tex_atlas_reg(atlas, TEXAT_PISTON_SIDE, 12, 6);
	tex_atlas_reg(atlas, TEXAT_PISTON_BACK, 13, 6);
	tex_atlas_reg(atlas, TEXAT_PISTON_INNER, 14, 6);
	tex_atlas_reg(atlas, TEXAT_LOG_SPRUCE_SIDE, 4, 7);
	tex_atlas_reg(atlas, TEXAT_LOG_BIRCH_SIDE, 5, 7);
	tex_atlas_reg(atlas, TEXAT_PUMPKIN_SIDE, 6, 7);
	tex_atlas_reg(atlas, TEXAT_PUMPKIN_FRONT, 7, 7);
	tex_atlas_reg(atlas, TEXAT_PUMPKIN_FRONT_LIT, 8, 7);
	tex_atlas_reg(atlas, TEXAT_CAKE_TOP, 9, 7);
	tex_atlas_reg(atlas, TEXAT_CAKE_SIDE, 10, 7);
	tex_atlas_reg(atlas, TEXAT_CAKE_SIDE_CUT, 11, 7);
	tex_atlas_reg(atlas, TEXAT_CAKE_BOTTOM, 12, 7);

	tex_atlas_reg(atlas, TEXAT_RAIL, 0, 8);
	// tex_atlas_reg(atlas, "repeater_off", 3, 8);
	tex_atlas_reg_col(atlas, TEXAT_LEAVES_SPRUCE, 4, 8, 97, 153, 97);
	tex_atlas_reg(atlas, TEXAT_BED_TOP_1, 6, 8);
	tex_atlas_reg(atlas, TEXAT_BED_TOP_2, 7, 8);

	tex_atlas_reg(atlas, TEXAT_CAST_BLOCK_LAPIS, 0, 9);
	// tex_atlas_reg(atlas, "repeater_on", 3, 9);
	tex_atlas_reg(atlas, TEXAT_BED_BACK, 5, 9);
	tex_atlas_reg(atlas, TEXAT_BED_SIDE_1, 6, 9);
	tex_atlas_reg(atlas, TEXAT_BED_SIDE_2, 7, 9);
	tex_atlas_reg(atlas, TEXAT_BED_FRONT, 8, 9);

	tex_atlas_reg(atlas, TEXAT_ORE_LAPIS, 0, 10);
	tex_atlas_reg(atlas, TEXAT_RAIL_POWERED_OFF, 3, 10);
	
	// Redstone wire, power levels 0..15, colour-baked per level (Beta tints the
	// grayscale dust by power; we bake it because the display-list vertex carries
	// no per-vertex RGB -- see render_block_redstone_wire). Two base tiles, both
	// from real Beta terrain.png: the LINE / straight-run tile (5,10)=idx165 and
	// the CROSS / junction+dot tile (4,10)=idx164. render_block_redstone_wire
	// picks LINE vs CROSS per the wire's connections, exactly as Beta's
	// RenderBlocks does. (The former TEXAT_REDSTONE_WIRE_INTERSECT_OFF/ON pointed
	// at tile (5,11)=181 -- blank in BOTH the Isabella and real-Beta atlases -- and
	// were never consumed; removed so atlas_oracle reports 0 blank registrations.)
	for (int lvl = 0; lvl <= 15; ++lvl) {
	    uint8_t r = redstone_colors[lvl][0];
	    uint8_t g = redstone_colors[lvl][1];
	    uint8_t b = redstone_colors[lvl][2];

	    enum tex_atlas_entry line  = (lvl == 0) ? TEXAT_REDSTONE_WIRE_OFF
	                                            : TEXAT_REDSTONE_WIRE_L1 + (lvl - 1);
	    enum tex_atlas_entry cross = (lvl == 0) ? TEXAT_REDSTONE_WIRE_CROSS_OFF
	                                            : TEXAT_REDSTONE_WIRE_CROSS_L1 + (lvl - 1);

	    tex_atlas_reg_col(atlas, line,  5, 10, r, g, b); // straight-run line
	    tex_atlas_reg_col(atlas, cross, 4, 10, r, g, b); // junction / dot cross
	}


	tex_atlas_reg(atlas, TEXAT_SANDSTONE_TOP, 0, 11);
	tex_atlas_reg(atlas, TEXAT_RAIL_POWERED_ON, 3, 11);

	tex_atlas_reg(atlas, TEXAT_SANDSTONE_SIDE, 0, 12);
	tex_atlas_reg(atlas, TEXAT_RAIL_DETECTOR, 3, 12);

	tex_atlas_reg(atlas, TEXAT_SANDSTONE_BOTTOM, 0, 13);

	tex_atlas_reg(atlas, TEXAT_WOOL_0, 0, 4);   // White
	tex_atlas_reg(atlas, TEXAT_WOOL_1, 2, 13);  // Orange
	tex_atlas_reg(atlas, TEXAT_WOOL_2, 2, 12);  // Magenta
	tex_atlas_reg(atlas, TEXAT_WOOL_3, 2, 11);  // Light Blue
	tex_atlas_reg(atlas, TEXAT_WOOL_4, 2, 10);  // Yellow
	tex_atlas_reg(atlas, TEXAT_WOOL_5, 2, 9);   // Lime
	tex_atlas_reg(atlas, TEXAT_WOOL_6, 2, 8);   // Pink
	tex_atlas_reg(atlas, TEXAT_WOOL_7, 2, 7);   // Gray
	tex_atlas_reg(atlas, TEXAT_WOOL_8, 1, 14);  // Light Gray
	tex_atlas_reg(atlas, TEXAT_WOOL_9, 1, 13);  // Cyan
	tex_atlas_reg(atlas, TEXAT_WOOL_10, 1, 12); // Purple
	tex_atlas_reg(atlas, TEXAT_WOOL_11, 1, 11); // Blue
	tex_atlas_reg(atlas, TEXAT_WOOL_12, 1, 10); // Brown
	tex_atlas_reg(atlas, TEXAT_WOOL_13, 1, 9);  // Green
	tex_atlas_reg(atlas, TEXAT_WOOL_14, 1, 8);  // Red
	tex_atlas_reg(atlas, TEXAT_WOOL_15, 1, 7);  // Black

	// P5(c): Beta lava-still is terrain index 237 = (13,14) (BlockFluid.java:7, lava arm).
	// (15,15) = index 255 is merely the bottom-right quadrant of Beta's 32x32 lava-FLOW
	// tile (238 with tileSize 2 spans 238/239/254/255).
	tex_atlas_reg(atlas, TEXAT_LAVA_STATIC, 13, 14);

	// DC-GRASS G2: the grass cells are RAW, untinted tiles. The biome colour is
	// applied exactly once, per vertex, by the chunk mesher (Beta BlockGrass
	// .java:23-28 colorMultiplier; RenderBlocks.java:2236-2281): top = tile 0
	// x biome; side = untinted tile 3 (BlockGrass.java:12-21), then the tile-38
	// overlay (TEXAT_GRASS_SIDE_OVERLAY, registered last below) x biome. These
	// used to bake a fixed (110,198,63) into the top -- which the mesher then
	// multiplied by the biome colour again (double tint) -- and a fixed-colour
	// tile-38-over-tile-3 composite into the side (fringe never biome-tinted).
	// Same registration position as before, so both keep their atlas slot.
	tex_atlas_reg(atlas, TEXAT_GRASS_TOP, 0, 0);
	tex_atlas_reg(atlas, TEXAT_GRASS_SIDE, 3, 0);
	tex_atlas_reg_col(atlas, TEXAT_TALLGRASS, 7, 2, 110, 198, 63);
	tex_atlas_reg_col(atlas, TEXAT_LEAVES_OAK, 4, 3, 72, 181, 24); // 0x48B518 ColorizerFoliage.func_31073_c
	tex_atlas_reg_col(atlas, TEXAT_FERN, 8, 3, 110, 198, 63);

	for(int k = 0; k < 10; k++)
		tex_atlas_reg(atlas, TEXAT_BREAK_0 + k, k, 15);

	// DC-GRASS G2: new cells go LAST so every pre-existing cell keeps its slot
	// (atlas capacity 14x14 = 196 cells; 188 registered after these three).
	tex_atlas_reg(atlas, TEXAT_GRASS_SIDE_OVERLAY, 6, 2); // Beta tile 38
	// Baked cells for the untinted consumers -- held/inventory/dropped grass
	// item (render_item.c, 3D and flat) and far-ring LOD skirts
	// (chunk_mesher.c lod_emit_skirts) -- colour baked ONCE with the CaveWii
	// unavailable-data grass colour (biome_color.h BIOME_GRASS_FALLBACK);
	// texel-identical to the pre-G2 TEXAT_GRASS_TOP / TEXAT_GRASS_SIDE cells.
	tex_atlas_reg_col(atlas, TEXAT_GRASS_TOP_ITEM, 0, 0, 110, 198, 63);
	tex_atlas_reg_grass(atlas, TEXAT_GRASS_SIDE_ITEM, 6, 2, 110, 198, 63, 3, 0);
	// Beta fast leaves, appended LAST (capacity 196) so every
	// earlier cell keeps its slot. Colours as the fancy cells above
	// (ColorizerFoliage pine 0x619961 / birch 0x80A755 / oak item 0x48B518).
	tex_atlas_reg_opaque_col(atlas, TEXAT_LEAVES_OAK_FAST, 5, 3, 4, 3, 72, 181,
							 24);
	tex_atlas_reg_opaque_col(atlas, TEXAT_LEAVES_SPRUCE_FAST, 5, 8, 4, 8, 97,
							 153, 97);
	tex_atlas_reg_opaque_col(atlas, TEXAT_LEAVES_BIRCH_FAST, 5, 3, 4, 3, 128,
							 167, 85);
	// Raw oak cells for biome-tinted oak faces (196/196: the atlas is full).
	tex_atlas_reg(atlas, TEXAT_LEAVES_OAK_RAW, 4, 3);
	tex_atlas_reg_opaque(atlas, TEXAT_LEAVES_OAK_FAST_RAW, 5, 3, 4, 3);

	memset(global_block_atlas, 0, sizeof(global_block_atlas));

	uint8_t* image = tex_read_atlas(filename, width, height);
	void* output = tex_atlas_compute(atlas, global_block_atlas, image,
									 *width, *height);
	dict_atlas_src_clear(atlas);
	free(image);

	return output;
}


void* tex_atlas_particles(const char* filename, size_t* width, size_t* height) {
	assert(filename && width && height);
	if(!filename || !width || !height)
		return NULL;
	*width = 0;
	*height = 0;
    dict_atlas_src_t atlas;
    dict_atlas_src_init(atlas);

  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_0,     0, 0);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_1,     1, 0);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_2,     2, 0);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_3,     3, 0);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_4,     4, 0);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_5,     5, 0);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_6,     6, 0);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SMOKE_7,     7, 0);

  tex_atlas_reg(atlas, TEXAT_PARTICLE_SPLASH_0,    0, 1);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_SPLASH_1,    1, 1);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_EMPTY,       2, 1);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_DROP_TINY_0, 3, 1);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_DROP_TINY_1, 4, 1);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_DROP_TINY_2, 5, 1);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_DROP_TINY_3, 6, 1);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_DROP_TINY_4, 7, 1);

  tex_atlas_reg(atlas, TEXAT_PARTICLE_BUBBLE,      0, 2);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_BOBBER,      1, 2);

  tex_atlas_reg(atlas, TEXAT_PARTICLE_FLAME,       0, 3);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_EMBER,       1, 3);

  tex_atlas_reg(atlas, TEXAT_PARTICLE_NOTE,        0, 4);

  tex_atlas_reg(atlas, TEXAT_PARTICLE_HEART,       0, 5);

  tex_atlas_reg(atlas, TEXAT_PARTICLE_DROP_FALL,   0, 6);
  tex_atlas_reg(atlas, TEXAT_PARTICLE_DROP_ROUND,  1, 6);
// you can register other rows here…

  memset(global_particle_atlas, 0, sizeof(global_particle_atlas));
	uint8_t* image = tex_read_atlas(filename, width, height);
	void* output = tex_atlas_compute(atlas,
									 global_particle_atlas,
									 image,
									 *width, *height);
	dict_atlas_src_clear(atlas);
	free(image);
	return output;

}
