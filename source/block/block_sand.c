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

#include "../entity/entity_falling_sand.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "block_sand.h"
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_SAND;
}

// Beta BlockSand.canFallBelow -- see block_sand.h for the full derivation of
// why direct id comparison (not this engine's getMaterial()) is the faithful
// translation. [decompile 740c583 BlockSand.canFallBelow]
bool block_sand_can_fall_below(struct server_local* s, w_coord_t x, w_coord_t y,
							   w_coord_t z) {
	struct block_data blk;
	// Beta's getBlockId(x,y,z) returns 0 (air) for an out-of-bounds Y (and
	// forces a chunk load for any X/Z in singleplayer, which this port's own
	// checkChunksExist-radius gates already ensure is unnecessary here); an
	// unloaded read is this predicate's only way to observe that, so treat it
	// as "nothing there" too.
	if(!server_world_get_block(block_world(s), x, y, z, &blk))
		return true;
	return blk.type == BLOCK_AIR || blk.type == BLOCK_FIRE
		|| blk.type == BLOCK_WATER_FLOW || blk.type == BLOCK_WATER_STILL
		|| blk.type == BLOCK_LAVA_FLOW || blk.type == BLOCK_LAVA_STILL;
}

// Beta World.canBlockBePlacedAt(...) as EntityFallingSand's landing check
// calls it -- see block_sand.h for the full derivation. Collapses to "air, or
// the thin snow layer". [decompile 740c583 World.canBlockBePlacedAt]
bool block_sand_can_place_at(struct server_local* s, w_coord_t x, w_coord_t y,
							 w_coord_t z) {
	struct block_data blk;
	if(!server_world_get_block(block_world(s), x, y, z, &blk))
		return false;
	return blk.type == BLOCK_AIR || blk.type == BLOCK_SNOW;
}

void block_sand_try_to_fall(struct server_local* s, w_coord_t x, w_coord_t y,
							w_coord_t z, uint8_t block_id) {
	if(!block_sand_can_fall_below(s, x, y - 1, z) || y < 0)
		return;

	// Beta: `byte var8 = 32; if (!fallInstantly && checkChunksExist(...32))`.
	// fallInstantly is Beta's own ChunkProviderGenerate/Hell/Sky populate()
	// guard, set true only for the duration of that call so worldgen-placed
	// sand/gravel settles synchronously instead of spawning an entity mid-
	// generation. It is moot in this port: worldgen (server_world.c's
	// server_world_populate_quadrant, including the gravel-vein pass via
	// worldgen_minable) writes every block through the raw quad_set/
	// quad_fluid_set function-pointer callbacks -- direct chunk-array writes
	// that NEVER call server_world_set_block -- so onBlockAdded/
	// onNeighbourBlockChange (and therefore this whole function) are
	// structurally unreachable during generation. The only reachable caller
	// of this function is the live per-tick scheduled-tick queue (a placed/
	// disturbed sand or gravel block during normal play), where fallInstantly
	// is always false. Modeled as a bare `true` (the "!fallInstantly" term)
	// rather than a dead static flag with no reachable setter.
	if(server_world_area_loaded_radius(block_world(s), x, y, z, 32)) {
		(void)server_local_spawn_falling_sand(
			s, (vec3) {(float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f},
			block_id);
		return;
	}

	// Direct-settle branch: clear the source cell, then walk straight down
	// while the cell below stays passable, re-placing at the first supported
	// cell (or nowhere, if the search reaches y==0 without support -- Beta's
	// own "block simply vanishes" outcome here). [decompile 740c583
	// BlockSand.tryToFall's else branch]
	server_world_set_block(
		block_world(s), x, y, z, (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
	while(block_sand_can_fall_below(s, x, y - 1, z) && y > 0)
		y--;
	if(y > 0)
		server_world_set_block(
			block_world(s), x, y, z,
			(struct block_data) {.type = block_id, .metadata = 0});
}

static void sand_added(struct server_local* s, struct block_info* info) {
	// Beta BlockSand.onBlockAdded: schedule at tickRate() (3).
	server_world_schedule_block_tick(block_world(s), info->x, info->y, info->z,
									 info->block->type);
}

// Beta BlockSand.onNeighborBlockChange: identical schedule call, unconditional
// (the support check happens inside tryToFall at fire time, against the BELOW
// cell -- not here). Defining onNeighbourBlockChange suppresses the engine's
// constant-tick_delay auto-schedule (blocks.c GAP #1 / server_world.c's own
// twin), so this handler is what actually arms the tick -- the torch
// self-scheduling precedent (block_torch.c).
static void sand_neighbour_change(struct server_local* s,
								  struct block_info* info) {
	server_world_schedule_block_tick(block_world(s), info->x, info->y, info->z,
									 info->block->type);
}

static void sand_world_tick(struct server_local* s, struct block_info* info) {
	// Beta BlockSand.updateTick -> tryToFall.
	block_sand_try_to_fall(s, info->x, info->y, info->z, info->block->type);
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

// Beta BlockSoulSand.getCollisionBoundingBoxFromPool: full x/z, height 1 - 0.125 = 0.875
// (an entity sinks 1/8 into the block). Only the COLLISION box (entity == true) is shrunk;
// Beta does not override getSelectedBoundingBoxFromPool, so the selection/render box stays a
// full cube (entity == false). [decompile 740c583 BlockSoulSand]
static size_t getBoundingBoxSoul(struct block_info* this, bool entity,
								 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, entity ? 0.875F : 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

static uint8_t getTextureIndex1(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_SAND);
}

static uint8_t getTextureIndex2(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_SOULSAND);
}

struct block block_sand = {
	.name = "Sand",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex1,
	.getDroppedItem = block_drop_default,
	.onBlockAdded = sand_added, // Beta BlockSand.onBlockAdded
	.onNeighbourBlockChange = sand_neighbour_change, // Beta BlockSand.onNeighborBlockChange
	.onWorldTick = sand_world_tick, // Beta BlockSand.updateTick -> tryToFall
	.tick_delay = BLOCK_SAND_TICK_RATE,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750,
	.digging.tool = TOOL_TYPE_SHOVEL,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_MAX,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_soulsand = {
	.name = "Soul Sand",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBoxSoul,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex2,
	.getDroppedItem = block_drop_default,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
