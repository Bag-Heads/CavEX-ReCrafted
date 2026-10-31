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
	// Beta BlockTallGrass.<init> (java:8-9): half-width .4F, height .8F.
	if(x)
		aabb_setsize(x, 0.8F, 0.8F, 0.8F);
	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex1(struct block_info* this, enum side side) {
	switch(this->block->metadata) {
		case 1: return tex_atlas_lookup(TEXAT_TALLGRASS); // tallgrass
		case 2: return tex_atlas_lookup(TEXAT_FERN);	  // fern
		default: return tex_atlas_lookup(TEXAT_DEADBUSH); // deadbush
	}
}

static uint8_t getTextureIndex2(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_DEADBUSH); // deadbush
}

static bool onItemPlace1(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	struct block_data blk;
	if(!server_world_get_block(block_world(s), where->x, where->y - 1, where->z,
							   &blk))
		return false;

	if(blk.type != BLOCK_DIRT && blk.type != BLOCK_GRASS)
		return false;

	return block_place_default(s, actor, it, where, on, on_side);
}

static bool onItemPlace2(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	struct block_data blk;
	if(!server_world_get_block(block_world(s), where->x, where->y - 1, where->z,
							   &blk))
		return false;

	if(!blocks[blk.type] || blocks[blk.type]->can_see_through)
		return false;

	return block_place_default(s, actor, it, where, on, on_side);
}

static size_t drop_seed(struct block_info* this, struct item_data* it,
						struct java_random* g, struct server_local* s) {
	bool drop_seed = jrand_next_int_bound(g, 8) == 0;

	if(it && drop_seed) {
		it->id = ITEM_SEED;
		it->durability = 0;
		it->count = 1;
	}

	return drop_seed ? 1 : 0;
}

static size_t drop_nothing(struct block_info* this, struct item_data* it,
						   struct java_random* g, struct server_local* s) {
	return 0;
}

// Beta BlockFlower.canThisPlantGrowOnThisBlockID [740c583], inherited UNCHANGED
// by BlockTallGrass (it overrides neither canThisPlantGrowOnThisBlockID nor
// canBlockStay): grass, dirt or tilledField (farmland). Identical predicate to
// block_flower.c's flower_soil_ok / block_sapling.c's sapling_soil_ok for their
// own species.
static bool tallgrass_soil_ok(uint8_t type) {
	return type == BLOCK_DIRT || type == BLOCK_GRASS || type == BLOCK_FARMLAND;
}

// Beta BlockFlower.canBlockStay, inherited UNCHANGED by BlockTallGrass:
// (getFullBlockLightValue >= 8 || canBlockSeeTheSky) && soil ok. Same
// server-side light-model DOCUMENTED DEVIATION as block_flower.c's
// flower_can_block_stay / block_sapling.c's sapling_can_block_stay (no
// time-of-day light field server-side -- max(sky,torch) >= 8 stands in for
// Beta's getFullBlockLightValue; see those functions' own comments).
static bool tallgrass_can_block_stay(struct server_local* s,
									 struct block_info* info) {
	int light = info->block->sky_light > info->block->torch_light
		? info->block->sky_light
		: info->block->torch_light;
	if(light < 8)
		return false;
	struct block_data below;
	return server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
								  &below)
		&& tallgrass_soil_ok(below.type);
}

// Beta BlockDeadBush.canThisPlantGrowOnThisBlockID [740c583]: sand ONLY
// (overrides BlockFlower's grass/dirt/farmland default).
static bool deadbush_soil_ok(uint8_t type) {
	return type == BLOCK_SAND;
}

// Beta BlockFlower.canBlockStay, inherited UNCHANGED by BlockDeadBush (only the
// soil predicate above is overridden): (getFullBlockLightValue >= 8 ||
// canBlockSeeTheSky) && sand below. Same light-model deviation as tallgrass.
static bool deadbush_can_block_stay(struct server_local* s,
									struct block_info* info) {
	int light = info->block->sky_light > info->block->torch_light
		? info->block->sky_light
		: info->block->torch_light;
	if(light < 8)
		return false;
	struct block_data below;
	return server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
								  &below)
		&& deadbush_soil_ok(below.type);
}

// Beta BlockFlower.checkFlowerChange, inherited UNCHANGED by both species: drop
// as an item (dropBlockAsItem -> the block's own getDroppedItem/idDropped --
// drop_seed's nextInt(8)==0 seed roll for tallgrass, drop_nothing's guaranteed
// nothing for dead bush, EXACTLY the same idDropped call a harvest would make,
// per BlockTallGrass.idDropped/BlockDeadBush.idDropped) and clear the block when
// it can no longer stay (soil removed, or it fell dark). Called from BOTH
// onNeighborBlockChange (support/soil lost) and updateTick (darkness decay with
// no neighbour ever changing) -- mirrors block_flower.c's flower_check /
// block_sapling.c's sapling_check.
static void tallgrass_check(struct server_local* s, struct block_info* info) {
	if(tallgrass_can_block_stay(s, info))
		return;
	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

static void deadbush_check(struct server_local* s, struct block_info* info) {
	if(deadbush_can_block_stay(s, info))
		return;
	server_local_spawn_block_drops(s, info); // drop_nothing -> always 0 items
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

static void tallgrass_onNeighbourBlockChange(struct server_local* s,
											 struct block_info* info) {
	tallgrass_check(s, info);
}

static void tallgrass_onRandomTick(struct server_local* s,
								   struct block_info* this) {
	tallgrass_check(s, this);
}

static void deadbush_onNeighbourBlockChange(struct server_local* s,
											struct block_info* info) {
	deadbush_check(s, info);
}

static void deadbush_onRandomTick(struct server_local* s,
								  struct block_info* this) {
	deadbush_check(s, this);
}

struct block block_tallgrass = {
	.name = "Tallgrass",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex1,
	.getDroppedItem = drop_seed,
	.onRandomTick = tallgrass_onRandomTick,
	.onNeighbourBlockChange = tallgrass_onNeighbourBlockChange,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_cross,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.render_block_data.cross_random_displacement = true,
	.ignore_lighting = false,
	.flammable = true,
	.place_ignore = false,
	.digging.hardness = 50,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace1,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_deadbush = {
	.name = "Deadbush",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex2,
	.getDroppedItem = drop_nothing,
	.onRandomTick = deadbush_onRandomTick,
	.onNeighbourBlockChange = deadbush_onNeighbourBlockChange,
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
	.flammable = true,
	.place_ignore = false,
	.digging.hardness = 50,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace2,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
