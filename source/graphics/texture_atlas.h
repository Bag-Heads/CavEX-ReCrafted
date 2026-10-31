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

#ifndef TEXTURE_ATLAS_H
#define TEXTURE_ATLAS_H

#include <stdint.h>

#include <m-lib/m-array.h>

enum tex_atlas_entry {
	TEXAT_STONE,
	TEXAT_DIRT,
	TEXAT_PLANKS,
	TEXAT_SLAB_STONE_SIDE,
	TEXAT_SLAB_STONE_TOP,
	TEXAT_BRICKS,
	TEXAT_TNT_SIDE,
	TEXAT_TNT_TOP,
	TEXAT_TNT_BOTTOM,
	TEXAT_COBWEB,
	TEXAT_ROSE,
	TEXAT_DANDELION,
	TEXAT_WATER_STATIC,
	TEXAT_SAPLING_OAK,
	TEXAT_COBBLESTONE,
	TEXAT_BEDROCK,
	TEXAT_SAND,
	TEXAT_GRAVEL,
	TEXAT_LOG_OAK_SIDE,
	TEXAT_LOG_OAK_TOP,
	TEXAT_CAST_BLOCK_IRON,
	TEXAT_CAST_BLOCK_GOLD,
	TEXAT_CAST_BLOCK_DIAMOND,
	TEXAT_CHEST_TOP,
	TEXAT_CHEST_SIDE,
	TEXAT_CHEST_FRONT_SINGLE,
	TEXAT_MUSHROOM_RED,
	TEXAT_MUSHROOM_BROWN,
	TEXAT_ORE_GOLD,
	TEXAT_ORE_IRON,
	TEXAT_ORE_COAL,
	TEXAT_BOOKSHELF,
	TEXAT_COBBLESTONE_MOSSY,
	TEXAT_OBSIDIAN,
	TEXAT_CHEST_FRONT_1,
	TEXAT_CHEST_FRONT_2,
	TEXAT_WORKBENCH_TOP,
	TEXAT_FURNACE_FRONT,
	TEXAT_FURNACE_SIDE,
	TEXAT_DISPENSER_FRONT,
	TEXAT_SPONGE,
	TEXAT_GLASS,
	TEXAT_ORE_DIAMOND,
	TEXAT_ORE_REDSTONE,
	TEXAT_LEAVES_BIRCH,
	TEXAT_DEADBUSH,
	TEXAT_CHEST_BACK_1,
	TEXAT_CHEST_BACK_2,
	TEXAT_WORKBENCH_SIDE_1,
	TEXAT_WORKBENCH_SIDE_2,
	TEXAT_FURNACE_FRONT_LIT,
	TEXAT_FURNACE_TOP,
	TEXAT_SAPLING_SPRUCE,
	TEXAT_SPAWNER,
	TEXAT_SNOW,
	TEXAT_GRASS_SIDE_SNOW,
	TEXAT_CACTUS_TOP,
	TEXAT_CACTUS_SIDE,
	TEXAT_CACTUS_BOTTOM,
	TEXAT_CLAY,
	TEXAT_REED,
	TEXAT_JUKBEBOX_SIDE,
	TEXAT_JUKBEBOX_TOP,
	TEXAT_SAPLING_BIRCH,
	TEXAT_TORCH,
	TEXAT_LEVER, // Beta lever blockIndexInTexture=96 = atlas tile (0,6)
	TEXAT_DOOR_WOOD_TOP,
	TEXAT_DOOR_IRON_TOP,
	TEXAT_LADDER,
	TEXAT_TRAPDOOR,
	TEXAT_FARMLAND_WET,
	TEXAT_FARMLAND_DRY,
	TEXAT_CROPS_0,
	TEXAT_CROPS_1,
	TEXAT_CROPS_2,
	TEXAT_CROPS_3,
	TEXAT_CROPS_4,
	TEXAT_CROPS_5,
	TEXAT_CROPS_6,
	TEXAT_CROPS_7,
	TEXAT_DOOR_WOOD_BOTTOM,
	TEXAT_DOOR_IRON_BOTTOM,
	TEXAT_REDSTONE_TORCH_LIT,
	TEXAT_PUMPKIN_TOP,
	TEXAT_NETHERRACK,
	TEXAT_SOULSAND,
	TEXAT_GLOWSTONE,
	TEXAT_RAIL_CURVED,
	TEXAT_REDSTONE_TORCH,
	// Redstone repeater TOP faces (Beta BlockRedstoneRepeater.getBlockTextureFromSideAndMetadata:
	// top idle=131 active=147). Real CC BY 3.0 Isabella II art already in assets/terrain.png. The
	// bottom faces (idle 115 / active 99) ARE the redstone-torch tiles (3,7)/(3,6) -> reuse
	// TEXAT_REDSTONE_TORCH[_LIT]; the sides use TEXAT_STONE (Beta idx 5).
	TEXAT_REPEATER_TOP_OFF, // terrain idx 131, tile (3,8)
	TEXAT_REPEATER_TOP_ON,	// terrain idx 147, tile (3,9)
	// Piston faces (Beta BlockPistonBase blockIndexInTexture 106-110 / PistonBlockTextures). The
	// shipped terrain.png reserves these tiles; a Beta 1.7.3 pack supplies the real art.
	TEXAT_PISTON_TOP_STICKY, // terrain idx 106, tile (10,6) -- sticky head plate
	TEXAT_PISTON_TOP,		 // terrain idx 107, tile (11,6) -- normal head plate / base front (retracted)
	TEXAT_PISTON_SIDE,		 // terrain idx 108, tile (12,6)
	TEXAT_PISTON_BACK,		 // terrain idx 109, tile (13,6)
	TEXAT_PISTON_INNER,		 // terrain idx 110, tile (14,6) -- base front when extended/powered
	TEXAT_LOG_SPRUCE_SIDE,
	TEXAT_LOG_BIRCH_SIDE,
	TEXAT_PUMPKIN_SIDE,
	TEXAT_PUMPKIN_FRONT,
	TEXAT_PUMPKIN_FRONT_LIT,
	TEXAT_CAKE_TOP,
	TEXAT_CAKE_SIDE,
	TEXAT_CAKE_SIDE_CUT,
	TEXAT_CAKE_BOTTOM,
	TEXAT_RAIL,
	TEXAT_LEAVES_SPRUCE,
	TEXAT_BED_TOP_1,
	TEXAT_BED_TOP_2,
	TEXAT_CAST_BLOCK_LAPIS,
	TEXAT_BED_BACK,
	TEXAT_BED_SIDE_1,
	TEXAT_BED_SIDE_2,
	TEXAT_BED_FRONT,
	TEXAT_ORE_LAPIS,
	TEXAT_RAIL_POWERED_OFF,
	TEXAT_REDSTONE_WIRE_OFF,
    // power‐levels 1…15
    TEXAT_REDSTONE_WIRE_L1,
    TEXAT_REDSTONE_WIRE_L2,
    TEXAT_REDSTONE_WIRE_L3,
    TEXAT_REDSTONE_WIRE_L4,
    TEXAT_REDSTONE_WIRE_L5,
    TEXAT_REDSTONE_WIRE_L6,
    TEXAT_REDSTONE_WIRE_L7,
    TEXAT_REDSTONE_WIRE_L8,
    TEXAT_REDSTONE_WIRE_L9,
    TEXAT_REDSTONE_WIRE_L10,
    TEXAT_REDSTONE_WIRE_L11,
    TEXAT_REDSTONE_WIRE_L12,
    TEXAT_REDSTONE_WIRE_L13,
    TEXAT_REDSTONE_WIRE_L14,
    TEXAT_REDSTONE_WIRE_L15,
	// Beta BlockRedstoneWire renders a CROSS tile (terrain idx 164 = tile (4,10))
	// for junctions/dots and a LINE tile (idx 165 = tile (5,10), the L1..L15 set
	// above) for straight runs -- see render_block_redstone_wire. Power level
	// 0..15 is colour-baked into each, exactly like the LINE variants. (The old
	// dead TEXAT_REDSTONE_WIRE_INTERSECT_OFF/ON mapped to tile (5,11)=181, blank
	// in both the Isabella and real-Beta atlases, and were never consumed.)
	TEXAT_REDSTONE_WIRE_CROSS_OFF,
	TEXAT_REDSTONE_WIRE_CROSS_L1,
	TEXAT_REDSTONE_WIRE_CROSS_L2,
	TEXAT_REDSTONE_WIRE_CROSS_L3,
	TEXAT_REDSTONE_WIRE_CROSS_L4,
	TEXAT_REDSTONE_WIRE_CROSS_L5,
	TEXAT_REDSTONE_WIRE_CROSS_L6,
	TEXAT_REDSTONE_WIRE_CROSS_L7,
	TEXAT_REDSTONE_WIRE_CROSS_L8,
	TEXAT_REDSTONE_WIRE_CROSS_L9,
	TEXAT_REDSTONE_WIRE_CROSS_L10,
	TEXAT_REDSTONE_WIRE_CROSS_L11,
	TEXAT_REDSTONE_WIRE_CROSS_L12,
	TEXAT_REDSTONE_WIRE_CROSS_L13,
	TEXAT_REDSTONE_WIRE_CROSS_L14,
	TEXAT_REDSTONE_WIRE_CROSS_L15,
	TEXAT_SANDSTONE_TOP,
	TEXAT_RAIL_POWERED_ON,
	TEXAT_SANDSTONE_SIDE,
	TEXAT_RAIL_DETECTOR,
	TEXAT_SANDSTONE_BOTTOM,
	TEXAT_WOOL_0,
	TEXAT_WOOL_1,
	TEXAT_WOOL_2,
	TEXAT_WOOL_3,
	TEXAT_WOOL_4,
	TEXAT_WOOL_5,
	TEXAT_WOOL_6,
	TEXAT_WOOL_7,
	TEXAT_WOOL_8,
	TEXAT_WOOL_9,
	TEXAT_WOOL_10,
	TEXAT_WOOL_11,
	TEXAT_WOOL_12,
	TEXAT_WOOL_13,
	TEXAT_WOOL_14,
	TEXAT_WOOL_15,
	TEXAT_GRASS_TOP,
	TEXAT_GRASS_SIDE,
	TEXAT_TALLGRASS,
	TEXAT_LEAVES_OAK,
	TEXAT_FERN,
	TEXAT_LAVA_STATIC,

	TEXAT_BREAK_0,
	TEXAT_BREAK_1,
	TEXAT_BREAK_2,
	TEXAT_BREAK_3,
	TEXAT_BREAK_4,
	TEXAT_BREAK_5,
	TEXAT_BREAK_6,
	TEXAT_BREAK_7,
	TEXAT_BREAK_8,
	TEXAT_BREAK_9,

    TEXAT_PARTICLE_SMOKE_0,
    TEXAT_PARTICLE_SMOKE_1,
    TEXAT_PARTICLE_SMOKE_2,
    TEXAT_PARTICLE_SMOKE_3,
    TEXAT_PARTICLE_SMOKE_4,
    TEXAT_PARTICLE_SMOKE_5,
    TEXAT_PARTICLE_SMOKE_6,
    TEXAT_PARTICLE_SMOKE_7,

    TEXAT_PARTICLE_SPLASH_0,
    TEXAT_PARTICLE_SPLASH_1,
    TEXAT_PARTICLE_EMPTY,
    TEXAT_PARTICLE_DROP_TINY_0,
    TEXAT_PARTICLE_DROP_TINY_1,
    TEXAT_PARTICLE_DROP_TINY_2,
    TEXAT_PARTICLE_DROP_TINY_3,
    TEXAT_PARTICLE_DROP_TINY_4,

    TEXAT_PARTICLE_BUBBLE,
    TEXAT_PARTICLE_BOBBER,

    TEXAT_PARTICLE_FLAME,
    TEXAT_PARTICLE_EMBER,

    TEXAT_PARTICLE_NOTE,

    TEXAT_PARTICLE_HEART,

    TEXAT_PARTICLE_DROP_FALL,
    TEXAT_PARTICLE_DROP_ROUND,

	// DC-GRASS G2: appended AFTER every pre-existing entry (and registered
	// after every pre-existing atlas cell in tex_atlas_block) so no existing
	// enum value or atlas slot moves -- non-grass output stays byte-identical
	// (tests/test_mesher_nongrass_equiv.c).
	// Beta terrain tile 38 = (6,2), the greyscale grass side overlay that
	// RenderBlocks draws biome-tinted over the untinted side tile 3
	// (RenderBlocks.java:1954-1970, 2276-2281). Raw, untinted.
	TEXAT_GRASS_SIDE_OVERLAY,
	// Baked grass cells for consumers without a per-vertex tint: the
	// held/inventory/dropped grass ITEM (render_item.c) and, for the side,
	// far-ring LOD skirts (chunk_mesher.c). Colour applied once (tile 0 x
	// fallback; tile 38 x fallback over tile 3) = the pre-G2 grass cells.
	TEXAT_GRASS_TOP_ITEM,
	TEXAT_GRASS_SIDE_ITEM,
	// Beta FAST leaves (render.fancy_graphics off, the default): the opaque
	// tiles blockIndexInTexture + 1 -- 53 = (5,3) oak/birch, 133 = (5,8)
	// spruce [BlockLeaves.java:177-184]. A pack whose fast cell is empty
	// gets the fancy cell with its holes filled opaque (opaque_from below).
	TEXAT_LEAVES_OAK_FAST,
	TEXAT_LEAVES_SPRUCE_FAST,
	TEXAT_LEAVES_BIRCH_FAST,
	// Raw (uncoloured) oak leaf tiles 52 / 53, for oak faces the mesher tints
	// with the column's biome foliage colour: Beta multiplies the raw texture
	// by colorMultiplier ONCE [BlockLeaves.java:19-29]. The baked _OAK cells
	// above stay for untinted consumers (items, particles, no-colormap
	// columns), like the G2 grass _ITEM cells.
	TEXAT_LEAVES_OAK_RAW,
	TEXAT_LEAVES_OAK_FAST_RAW,

	TEXAT_MAX,
};

struct texture_entry {
	enum tex_atlas_entry name;
	uint8_t x : 4, y : 4;
	struct {
		bool enable;
		uint8_t r, g, b;
	} colorize;
	struct {
		bool enable;
		uint8_t x : 4, y : 4;
	} bg;
	/* If the source cell (x, y) has no opaque texel at all (a pack without
	 * that tile), build it from cell (opaque_from.x, .y) instead, with every
	 * cut-out texel filled by that cell's darkened mean colour, alpha 255. */
	struct {
		bool enable;
		uint8_t x : 4, y : 4;
	} opaque_from;
};

ARRAY_DEF(dict_atlas_src, struct texture_entry, M_POD_OPLIST)

#define TEXTURE_INDEX(x, y) (((y)*14) + (x))
#define TEXTURE_X(idx) ((idx) % 14)
#define TEXTURE_Y(idx) ((idx) / 14)

void tex_atlas_reg(dict_atlas_src_t atlas, enum tex_atlas_entry name, uint8_t x,
				   uint8_t y);
void tex_atlas_reg_col(dict_atlas_src_t atlas, enum tex_atlas_entry name,
					   uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);
void tex_atlas_reg_grass(dict_atlas_src_t atlas, enum tex_atlas_entry name,
						 uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b,
						 uint8_t bg_x, uint8_t bg_y);
void* tex_atlas_compute(dict_atlas_src_t atlas, uint8_t* atlas_dst,
						uint8_t* image, size_t width, size_t height);
void* tex_atlas_block(const char* filename, size_t* width, size_t* height);
void* tex_atlas_particles(const char* filename, size_t* width, size_t* height);

// Live-water atlas slot (pixel origin + tile size), captured by tex_atlas_compute
// when the block atlas is built, so water_fx can blit its animated tile in place.
// g_water_atlas_x/y are -1 until captured.
extern int g_water_atlas_x, g_water_atlas_y, g_water_atlas_tile;

/* pack-cache-0927: the lookup tables the two atlas builds fill (TEXAT_MAX
   bytes each), for the pack cache to save after a fresh load and restore on
   a hit together with the g_water_atlas_* capture. */
void tex_atlas_tables_export(uint8_t* block, uint8_t* particle);
void tex_atlas_tables_import(const uint8_t* block, const uint8_t* particle);

uint8_t tex_atlas_lookup(enum tex_atlas_entry name);
uint8_t tex_atlas_lookup_particle(enum tex_atlas_entry name);

/* Content UV rectangle in the packed atlas, excluding duplicated borders. */
bool tex_atlas_tile_uv(size_t width, size_t height, uint8_t tile, float uv[4]);


#endif
