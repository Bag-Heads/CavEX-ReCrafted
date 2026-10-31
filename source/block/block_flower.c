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

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockFlower.<init> (java:10-11): half-width .2F, height .2F * 3.
	if(x)
		aabb_setsize(x, 0.4F, 0.2F * 3.0F, 0.4F);
	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_DANDELION);
}

// Beta BlockFlower.canThisPlantGrowOnThisBlockID: grass, dirt or tilledField (farmland).
static bool flower_soil_ok(uint8_t type) {
	return type == BLOCK_DIRT || type == BLOCK_GRASS || type == BLOCK_FARMLAND;
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk;
	if(!server_world_get_block(block_world(s), where->x, where->y - 1, where->z,
							   &blk))
		return false;

	if(!flower_soil_ok(blk.type))
		return false;

	return block_place_default(s, actor, it, where, on, on_side);
}

// Beta BlockFlower.canBlockStay uses (getFullBlockLightValue >= 8 ||
// canBlockSeeTheSky) && soil ok (BlockFlower.java:39-40). World.java:521-529
// passes 0 to Chunk.getBlockLightValue, so this check uses undimmed stored
// sky/block light even at night; max(sky, torch) >= 8 follows that rule.
static bool flower_can_block_stay(struct server_local* s,
								  struct block_info* info) {
	int light = info->block->sky_light > info->block->torch_light
		? info->block->sky_light
		: info->block->torch_light;
	if(light < 8)
		return false;
	struct block_data below;
	return server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
								  &below)
		&& flower_soil_ok(below.type);
}

// Beta BlockFlower.onNeighborBlockChange + updateTick -> checkFlowerChange: drop as item and
// clear when the flower can no longer stay (soil removed, or it fell dark). The random tick is
// what pops an already-placed flower whose LIGHT decayed without any adjacent block changing.
static void flower_check(struct server_local* s, struct block_info* info) {
	if(flower_can_block_stay(s, info))
		return;
	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	flower_check(s, info);
}

static void onRandomTick(struct server_local* s, struct block_info* this) {
	flower_check(s, this);
}

struct block block_flower = {
	.name = "Flower",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onRandomTick = onRandomTick, // Beta updateTick -> checkFlowerChange
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: pop when canBlockStay fails
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_cross,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.render_block_data.cross_random_displacement = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 50,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
