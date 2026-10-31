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
#include "../util.h"
#include "../worldgen/tree_runtime.h"
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockSapling.<init> (java:8-9): half-width .4F, height .8F.
	if(x)
		aabb_setsize(x, 0.8F, 0.8F, 0.8F);
	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(this->block->metadata & 0x3) {
		default:
		case 0: return tex_atlas_lookup(TEXAT_SAPLING_OAK);
		case 1: return tex_atlas_lookup(TEXAT_SAPLING_SPRUCE);
		case 2: return tex_atlas_lookup(TEXAT_SAPLING_BIRCH);
	}
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
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

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = this->block->type;
		it->durability = this->block->metadata & 3;
		it->count = 1;
	}

	return 1;
}

// Beta BlockFlower.canThisPlantGrowOnThisBlockID, inherited UNCHANGED by
// BlockSapling (it never overrides the method): grass, dirt or tilledField
// (farmland) -- the exact same predicate block_flower.c's flower_soil_ok /
// block_rose.c's rose_soil_ok already implement for their own species.
// [740c583: BlockFlower.canThisPlantGrowOnThisBlockID]
static bool sapling_soil_ok(uint8_t type) {
	return type == BLOCK_DIRT || type == BLOCK_GRASS || type == BLOCK_FARMLAND;
}

// Beta BlockSapling inherits BlockFlower.canBlockStay: undimmed full block
// light >= 8 or sky visibility, plus valid soil (BlockFlower.java:39-40;
// World.java:521-529 passes 0 rather than skylightSubtracted). The stored
// max(sky, torch) check therefore has no night-dimming deviation.
static bool sapling_can_block_stay(struct server_local* s,
								   struct block_info* info) {
	int light = info->block->sky_light > info->block->torch_light
		? info->block->sky_light
		: info->block->torch_light;
	if(light < 8)
		return false;
	struct block_data below;
	return server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
								  &below)
		&& sapling_soil_ok(below.type);
}

// Beta BlockFlower.checkFlowerChange, inherited UNCHANGED by BlockSapling:
// drop as an item and clear the block when it can no longer stay (soil
// removed, or it fell dark) -- called from BOTH onNeighborBlockChange and
// (via BlockSapling.updateTick's `super.updateTick(...)`, see onRandomTick
// below) the random tick. Same shape as block_flower.c's flower_check /
// block_rose.c's rose_check, but returns whether it actually popped the
// block: onRandomTick (unlike flower/rose's own updateTick, which IS just
// this check) has more to do afterward, and needs to know whether to
// continue -- `info->block` is a snapshot taken BEFORE this call
// (server_world.c's random-tick dispatcher hands onRandomTick a local
// `struct block_data` copy, not a live chunk pointer), so it will NOT
// reflect a write server_world_set_block below just made; the return value
// is the only reliable signal. [740c583: BlockFlower.checkFlowerChange]
static bool sapling_check(struct server_local* s, struct block_info* info) {
	if(sapling_can_block_stay(s, info))
		return false;
	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
	return true;
}

static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	sapling_check(s, info);
}

// Beta 1.7.3 BlockSapling.updateTick: `super.updateTick(...)` (BlockFlower's
// checkFlowerChange, sapling_check above) runs FIRST, popping the sapling as
// an item when it is dark (light<8 && no sky) or its grass/dirt/farmland
// support is gone -- BEFORE any grow-cadence roll. Beta then still reads the
// light above and consumes nextInt(30) when that light is >= 9, even after
// the sapling was removed. A surviving sapling grows only on a zero roll.
// The FIRST qualifying roll sets the age bit (metadata | 8) and keeps the
// sapling; the SECOND grows it into a tree via growTree (the faithful
// WorldGen* builders), species in metadata & 3.
//
// [740c583: BlockSapling.updateTick + growTree]
static void onRandomTick(struct server_local* s, struct block_info* this) {
	bool popped = sapling_check(s, this);

	// Light read at (x, y+1, z) -- Beta's getBlockLightValue of the block above,
	// not the sapling cell (BlockSapling.java:15): slab/stairs/farmland neighbour max
	// and the rain/thunder skylightSubtracted included (next-gaps-0924/F1). A
	// non-resident cell reads 0 (Beta EmptyChunk), which fails the gate like before.
	int light = server_local_block_light_value(s, this->x, this->y + 1, this->z);
	if(light < 9)
		return;

	if(rand_gen_int(&s->rand_src, 30) != 0)
		return;
	if(popped)
		return; // Beta re-reads air metadata here; no sapling remains to age.

	uint8_t meta = this->block->metadata;
	if((meta & 8) == 0) {
		// First qualifying tick: advance to the aged state, keep the sapling.
		struct block_data aged = *this->block;
		aged.metadata = meta | 8;
		server_world_set_block(block_world(s), this->x, this->y, this->z, aged);
	} else {
		// Second: grow into a tree (species = metadata & 3).
		server_grow_tree(s, this->x, this->y, this->z, meta & 3);
	}
}

struct block block_sapling = {
	.name = "Sapling",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = onRandomTick,
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
		.fuel = 1,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
