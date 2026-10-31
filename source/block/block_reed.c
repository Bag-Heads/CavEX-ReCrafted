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
	if(x)
		aabb_setsize(x, 0.75F, 1.0F, 0.75F);
	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_REED);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_REED;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

// Beta BlockReed.canPlaceBlockAt/canBlockStay: below is reed -> ok; else below must be dirt/grass AND
// have water on one of the 4 horizontal sides of that base block. (Material.water = flowing or still.)
bool reed_can_block_stay(struct server_local* s, int x, int y, int z) {
	struct block_data below;
	if(!server_world_get_block(block_world(s), x, y - 1, z, &below))
		return false;
	if(below.type == BLOCK_REED)
		return true;
	if(below.type != BLOCK_DIRT && below.type != BLOCK_GRASS)
		return false;
	static const int dx[4] = {-1, 1, 0, 0};
	static const int dz[4] = {0, 0, -1, 1};
	struct block_data n;
	for(int i = 0; i < 4; i++)
		if(server_world_get_block(block_world(s), x + dx[i], y - 1, z + dz[i], &n)
		   && (n.type == BLOCK_WATER_FLOW || n.type == BLOCK_WATER_STILL))
			return true;
	return false;
}

// Beta BlockReed.onNeighborBlockChange -> checkBlockCoordValid: drop + clear when it can no longer stay
// (the base block or its adjacent water was removed).
static void reed_remove(struct server_local* s, struct block_info* this) {
	server_local_spawn_block_drops(s, this);
	server_world_set_block(block_world(s), this->x, this->y, this->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}
static void onNeighbourBlockChange(struct server_local* s, struct block_info* info) {
	if(!reed_can_block_stay(s, info->x, info->y, info->z))
		reed_remove(s, info);
}

// Beta BlockReed.canPlaceBlockAt IS its canBlockStay: the old bare block_place_default let a
// reed be placed on stone, where it popped on the first neighbour change.
static bool reed_place(struct server_local* s, struct server_player* actor, struct item_data* it,
					   struct block_info* where, struct block_info* on,
					   enum side on_side) {
	if(!reed_can_block_stay(s, where->x, where->y, where->z))
		return false;
	return block_place_default(s, actor, it, where, on, on_side);
}

static void onRandomTick(struct server_local* s, struct block_info* this) {
	// Beta BlockReed.updateTick: the ENTIRE growth body -- including the metadata counter --
	// is gated on air above. A blocked or full-height reed leaves its counter UNTOUCHED (the
	// old code incremented meta unconditionally, so a meta-15 reed under a ceiling silently
	// wrapped its ready-to-grow state back to 0).
	struct block_data above;
	if(server_world_get_block(block_world(s), this->x, this->y + 1, this->z, &above)
	   && above.type != BLOCK_AIR)
		return;

	// height = this reed + the reeds stacked directly below (Beta `for(var6=1; below==reed; ++var6)`).
	int height = 1;
	for(int k = 1; k <= 2; k++) {
		struct block_data below;
		if(server_world_get_block(block_world(s), this->x, this->y - k, this->z, &below)
		   && below.type == BLOCK_REED)
			height++;
		else
			break;
	}

	if(height < 3) {
		if(this->block->metadata == 0xF) {
			// Beta: place the new segment, then reset THIS segment's counter to 0.
			server_world_set_block(block_world(s), this->x, this->y + 1, this->z,
								   (struct block_data) {
									   .type = BLOCK_REED,
									   .metadata = 0,
								   });
			this->block->metadata = 0;
		} else {
			this->block->metadata++;
		}
		server_world_set_block(block_world(s), this->x, this->y, this->z, *this->block);
	}
}

struct block block_reed = {
	.name = "Reed",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = onRandomTick,
	.onRightClick = NULL,
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta checkBlockCoordValid (drop if unsupported)
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
		.onItemPlace = reed_place, // Beta canPlaceBlockAt (base + adjacent water)
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
