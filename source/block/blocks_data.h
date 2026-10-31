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

#ifndef BLOCKS_DATA_H
#define BLOCKS_DATA_H

#include <stdbool.h>
#include <stdint.h>

enum side {
	SIDE_TOP = 0,
	SIDE_BOTTOM = 1,
	SIDE_LEFT = 2,
	SIDE_RIGHT = 3,
	SIDE_FRONT = 4,
	SIDE_BACK = 5,
	SIDE_MAX
};

enum block_material {
	MATERIAL_WOOD,
	MATERIAL_STONE,
	MATERIAL_WOOL,
	MATERIAL_ORGANIC,
	MATERIAL_SAND,
	MATERIAL_GLASS,
};

// Per-block Beta biome colour-tint policy, consulted by the mesher. Beta tints
// only grass (top) and foliage (leaves, all faces) by the per-column
// temperature/rainfall colour; everything else is untinted. Default 0 = none.
enum biome_tint {
	BIOME_TINT_NONE = 0,
	BIOME_TINT_GRASS,	 // grass colour, top face only (grass block, tall grass)
	BIOME_TINT_FOLIAGE,	 // foliage colour, all faces (leaves)
};

enum block_type {
	BLOCK_AIR = 0,
	BLOCK_STONE = 1,
	BLOCK_GRASS = 2,
	BLOCK_DIRT = 3,
	BLOCK_COBBLESTONE = 4,
	BLOCK_PLANKS = 5,
	BLOCK_SAPLING = 6,
	BLOCK_BEDROCK = 7,
	BLOCK_WATER_FLOW = 8,
	BLOCK_WATER_STILL = 9,
	BLOCK_LAVA_FLOW = 10,
	BLOCK_LAVA_STILL = 11,
	BLOCK_SAND = 12,
	BLOCK_GRAVEL = 13,
	BLOCK_GOLD_ORE = 14,
	BLOCK_IRON_ORE = 15,
	BLOCK_COAL_ORE = 16,
	BLOCK_LOG = 17,
	BLOCK_LEAVES = 18,
	BLOCK_SPONGE = 19,
	BLOCK_GLASS = 20,
	BLOCK_LAPIS_ORE = 21,
	BLOCK_LAPIS_CAST = 22,
	BLOCK_DISPENSER = 23,
	BLOCK_SANDSTONE = 24,
	BLOCK_NOTEBLOCK = 25,
	BLOCK_BED = 26,
	BLOCK_POWERED_RAIL = 27,
	BLOCK_DETECTOR_RAIL = 28,
	BLOCK_PISTON_STICKY = 29, // Beta Block.pistonStickyBase
	BLOCK_COBWEB = 30,		  // Beta Block.web (Material.web)
	BLOCK_TALL_GRASS = 31,
	BLOCK_DEADBUSH = 32,	  // Beta Block.deadBush (BlockDeadBush : BlockFlower, Material.plants)
	BLOCK_PISTON_BASE = 33,	  // Beta Block.pistonBase
	BLOCK_PISTON_HEAD = 34,	  // Beta Block.pistonExtension (the head)
	BLOCK_WOOL = 35,
	BLOCK_PISTON_MOVING = 36, // Beta Block.pistonMoving (the moving tile-entity block)
	BLOCK_FLOWER = 37,
	BLOCK_ROSE = 38,
	BLOCK_BROWM_MUSHROOM = 39,
	BLOCK_RED_MUSHROOM = 40,
	BLOCK_GOLD_CAST = 41,
	BLOCK_IRON_CAST = 42,
	BLOCK_DOUBLE_SLAB = 43,
	BLOCK_SLAB = 44,
	BLOCK_BRICKS = 45,
	BLOCK_TNT = 46,
	BLOCK_BOOKSHELF = 47,
	BLOCK_MOSSY_COBBLE = 48,
	BLOCK_OBSIDIAN = 49,
	BLOCK_TORCH = 50,
	BLOCK_FIRE = 51,
	BLOCK_MOB_SPAWNER = 52,
	BLOCK_WOODEN_STAIRS = 53,
	BLOCK_CHEST = 54,
	BLOCK_REDSTONE_WIRE = 55,
	BLOCK_DIAMOND_ORE = 56,
	BLOCK_DIAMOND_CAST = 57,
	BLOCK_WORKBENCH = 58,
	BLOCK_CROPS = 59,
	BLOCK_FARMLAND = 60,
	BLOCK_FURNACE = 61,
	BLOCK_FURNACE_LIT = 62, // active/burning furnace (Beta stoneOvenActive); blocks[62]
	BLOCK_SIGN_POST = 63, // Beta Block.signPost (freestanding, metadata = 0..15 yaw)
	BLOCK_DOOR_WOOD = 64,
	BLOCK_LADDER = 65,
	BLOCK_RAIL = 66,
	BLOCK_STONE_STAIRS = 67,
	BLOCK_SIGN = 68,
	BLOCK_LEVER = 69, // Beta Block.lever (BlockLever, Material.circuits)
	BLOCK_STONE_PRESSURE_PLATE = 70,
	BLOCK_DOOR_IRON = 71,
	BLOCK_WOOD_PRESSURE_PLATE = 72,
	BLOCK_REDSTONE_TORCH = 75,
	BLOCK_REDSTONE_TORCH_LIT = 76,
	BLOCK_STONE_BUTTON = 77, // Beta Block.button (BlockButton, Material.circuits)
	BLOCK_SNOW = 78,
	BLOCK_ICE = 79,
	BLOCK_SNOW_BLOCK = 80,
	BLOCK_CACTUS = 81,
	BLOCK_REED = 83,
	BLOCK_CLAY = 82,
	BLOCK_JUKEBOX = 84,
	BLOCK_FENCE = 85,
	BLOCK_PUMPKIN = 86,
	BLOCK_NETHERRACK = 87, // Beta Block.netherrack (Material.rock, Hell filler)
	BLOCK_SOULSAND = 88,   // Beta Block.slowSand (Material.sand, Hell [60..65] layer)
	BLOCK_GLOWSTONE = 89,
	BLOCK_PORTAL = 90,
	BLOCK_PUMPKIN_LIT = 91,
	BLOCK_CAKE = 92,				  // Beta Block.cake (Material.cakeMaterial)
	BLOCK_REDSTONE_REPEATER = 93,	  // Block.redstoneRepeaterIdle   (output OFF)
	BLOCK_REDSTONE_REPEATER_LIT = 94, // Block.redstoneRepeaterActive (output ON)
	// Beta Block.lockedChest (BlockLockedChest) [740c583 Block.java:687]: Material.wood,
	// hardness 0, luminance 15, self-despawns via updateTick, NEVER a container (no
	// onBlockActivated override). FH-07 (docs/fork_hack_audit_2026-08-21.md): this id
	// used to be squatted by the fork's iron chest -- see BLOCK_IRON_CHEST below, now
	// relocated off it.
	BLOCK_LOCKED_CHEST = 95,
	BLOCK_TRAP_DOOR = 96,
//	BLOCK_MINECART = 98,
	// NET-NEW (non-Beta, FH-07 relocation): the fork's iron chest, moved off Beta's own
	// id 95 (BlockLockedChest) so a real Beta 1.7.3 world/wire value of 95 decodes as
	// the faithful locked chest instead. 200 sits far above Beta's id ceiling (0..96,
	// plus the 98 minecart gap) so it can never collide with a future faithful Beta id.
	// Feature set is byte-identical to before the relocation -- only the id changed.
	BLOCK_IRON_CHEST = 200,
};

// Beta has two BlockSign ids: the freestanding signPost (63) and signWall
// (68). Block id, not metadata, distinguishes post yaw 0..15 from wall-facing
// metadata 2..5.
static inline bool block_is_any_sign(uint8_t type) {
	return type == BLOCK_SIGN_POST || type == BLOCK_SIGN;
}

typedef int32_t w_coord_t;

struct block_data {
	uint8_t type;
	uint8_t metadata : 4;
	uint8_t sky_light : 4;
	uint8_t torch_light : 4;
};

struct piston_moving_state;

// R1: who stepped on a block (struct block.onEntityWalking). Beta passes the Entity;
// no 1.7.3 override reads it (W2-R1c: farmland no longer does either), so the
// kind is kept only as caller-side provenance for the dispatch.
enum block_walker {
	BLOCK_WALKER_PLAYER,
	BLOCK_WALKER_MOB,
};

struct block_info {
	struct block_data* block;
	struct block_data* neighbours;
	w_coord_t x, y, z;
	// Optional thread-safe read of an arbitrary nearby block (the DIAGONAL
	// neighbours the 6-entry `neighbours` cache lacks), needed by
	// connection-aware renderers -- redstone wire's cross/line/trim shape and
	// its vertical "climbing" faces inspect a 3x3x3 neighbourhood (Beta
	// RenderBlocks.renderBlockRedstoneWire). Returns true and fills *out when the
	// block is available. The chunk mesher backs this with its `bd` snapshot
	// (worker-thread safe -- it NEVER touches the single-threaded live world);
	// the main-thread block-cracks overlay backs it with world_get_block. NULL
	// when the constructing path supplies no accessor (callers must tolerate it).
	bool (*get_block)(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* out);
	void* get_block_ctx;
	// Optional lookup for BlockPistonMoving's TileEntityPiston-backed selection/collision bounds.
	// NULL models an orphan id36 and therefore yields no box, exactly like Beta.
	bool (*get_piston_moving)(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
							  struct piston_moving_state* out);
	void* get_piston_moving_ctx;
	// Optional per-call texture override, consulted ONLY by render_block.c's
	// block_texture_index() helper instead of the block's registered
	// getTextureIndex. NULL (the default for every existing designated-init
	// call site, C zero-fills omitted members) means "use the real one".
	// The block-cracks overlay (render_block_cracks) is the sole user: it used
	// to swap blocks[type]->getTextureIndex itself, a GLOBAL vtable slot the
	// mesher thread also reads (chunk_mesher.c) -- a cross-thread data race on
	// live gameplay state. Stack-local to whichever block_info the caller
	// builds, so it carries the override through the SAME thread's call stack
	// with no shared mutable state at all.
	uint8_t (*texture_override)(struct block_info*, enum side);
};

#endif
