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
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x) {
		switch(this->block->metadata) {
			case 2:
				aabb_setsize(x, 1.0F, 1.0F, 0.125F);
				aabb_translate(x, 0, 0, 0.4375F);
				break;
			case 3:
				aabb_setsize(x, 1.0F, 1.0F, 0.125F);
				aabb_translate(x, 0, 0, -0.4375F);
				break;
			case 4:
				aabb_setsize(x, 0.125F, 1.0F, 1.0F);
				aabb_translate(x, 0.4375F, 0, 0);
				break;
			case 5:
				aabb_setsize(x, 0.125F, 1.0F, 1.0F);
				aabb_translate(x, -0.4375F, 0, 0);
				break;
			default: aabb_setsize(x, 1.0F, 1.0F, 1.0F); break;
		}
	}

	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_LADDER);
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	if(!blocks[on->block->type] || blocks[on->block->type]->can_see_through)
		return false;

	int metadata = 0;
	switch(on_side) {
		case SIDE_LEFT: metadata = 4; break;
		case SIDE_RIGHT: metadata = 5; break;
		case SIDE_FRONT: metadata = 2; break;
		case SIDE_BACK: metadata = 3; break;
		default: return false;
	}

	server_world_set_block(block_world(s), where->x, where->y, where->z,
						   (struct block_data) {
							   .type = it->id,
							   .metadata = metadata,
							   .sky_light = 0,
							   .torch_light = 0,
						   });
	return true;
}

// [740c583 BlockLadder.onNeighborBlockChange] FH-03: an unsupported ladder
// must pop -- transcribed as Beta's own four INDEPENDENT checks (not
// else-if), so any metadata outside 2..5 (corrupt/legacy data) leaves
// support absent and always drops, exactly like the real method.
// Metadata 2 checks the normal cube at +Z; metadata 3 checks -Z;
// metadata 4 checks +X; metadata 5 checks -X. Each check independently
// establishes support when its orientation and neighbor both match.
// Without support, the ladder drops its item using the current metadata,
// then replaces its cell with air and notifies neighbors.
//
//
//
//
//
// isBlockNormalCube's C proxy here is the SAME "solid mount face" predicate
// this file's own onItemPlace already gates placement on two screens up
// (!blocks[type] || can_see_through) -- reusing it means a ladder legally
// placed against a wall can never immediately pop itself off, and a ladder
// that survives can never be resting on something onItemPlace would refuse.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	uint8_t meta = info->block->metadata;
	bool has_support = false;
	struct block_data nb;

	if(meta == 2 && server_world_get_block(block_world(s), info->x, info->y,
										   info->z + 1, &nb)
	   && blocks[nb.type] && !blocks[nb.type]->can_see_through)
		has_support = true;
	if(meta == 3 && server_world_get_block(block_world(s), info->x, info->y,
										   info->z - 1, &nb)
	   && blocks[nb.type] && !blocks[nb.type]->can_see_through)
		has_support = true;
	if(meta == 4 && server_world_get_block(block_world(s), info->x + 1, info->y,
										   info->z, &nb)
	   && blocks[nb.type] && !blocks[nb.type]->can_see_through)
		has_support = true;
	if(meta == 5 && server_world_get_block(block_world(s), info->x - 1, info->y,
										   info->z, &nb)
	   && blocks[nb.type] && !blocks[nb.type]->can_see_through)
		has_support = true;

	if(!has_support) {
		server_local_spawn_block_drops(s, info);
		server_world_set_block(block_world(s), info->x, info->y, info->z,
							   (struct block_data) {.type = BLOCK_AIR,
													 .metadata = 0});
	}
}

struct block block_ladder = {
	.name = "Ladder",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onRandomTick = NULL,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_ladder,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 600,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace,
		.fuel = 1,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
