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

#include "blocks.h"
#include "fluid_block.h"
#include "../graphics/gfx_settings.h"
#include "../game/game_state.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	return 0;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	#ifdef GFX_FANCY_LIQUIDS
	int block_height = (this->block->metadata & 0x8) ?
		16 :
		(8 - this->block->metadata) * 2 * 7 / 8;
	switch(side) {
		case SIDE_TOP:
			return (it->block->type == BLOCK_WATER_FLOW
					|| it->block->type == BLOCK_WATER_STILL) ?
				face_occlusion_full() :
				face_occlusion_empty(); 
		case SIDE_BOTTOM: return face_occlusion_full();
		default: return face_occlusion_rect(block_height);
	}
	#else
	if (gstate.players[0].in_water) {
		return (it->block->type == BLOCK_WATER_FLOW
				|| it->block->type == BLOCK_WATER_STILL) ?
			face_occlusion_full() :
			face_occlusion_empty(); 
	} else return face_occlusion_full();
	#endif
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	#ifdef GFX_FANCY_LIQUIDS
	/* Beta BlockFluid.getBlockTextureFromSide: top/bottom use the still
	   texture; horizontal faces use the flow texture. A moving TOP selects
	   the latter dynamically in render_block_fluid from getFlowDirection. */
	return (side == SIDE_TOP || side == SIDE_BOTTOM) ?
		TEXTURE_INDEX(1, 0) : TEXTURE_INDEX(5, 0);
	#else
	return tex_atlas_lookup(TEXAT_WATER_STATIC);
	#endif
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	return 0;
}

// Flowing water (id 8) runs the Beta spread; it settles itself to still (id 9)
// when stable, which stops it being re-ticked. Still water (id 9) re-wakes to
// flowing when a neighbour opens a path. Both delegate to fluid_block.c.
static void onWorldTick(struct server_local* s, struct block_info* this) {
	fluid_block_tick(s, this, false);
}

static void onBlockAdded_flow(struct server_local* s,
							  struct block_info* this) {
	fluid_block_on_added(s, this, false, BLOCK_WATER_FLOW);
}

static void onNeighbourChange_flow(struct server_local* s,
								   struct block_info* this) {
	/* BlockFluid.onNeighborBlockChange has only the lava harden check; water's
	   handler is intentionally a no-op. Registering it still matters: unlike
	   CaveWii's old blanket heuristic, Beta does not schedule a neighbouring
	   moving fluid merely because some other block changed. */
	fluid_block_on_neighbour(s, this, false, 0);
}

static void onNeighbourChange_still(struct server_local* s,
									struct block_info* this) {
	fluid_block_on_neighbour(s, this, false, BLOCK_WATER_FLOW);
}

struct block block_water_still = {
	.name = "Water",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBlockAdded = NULL, /* BlockFluid.onBlockAdded is a no-op for water */
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.onNeighbourBlockChange = onNeighbourChange_still,
	#ifdef GFX_FANCY_LIQUIDS
	.transparent = true,
	.renderBlock = render_block_fluid,
	#else
	.transparent = false,
	.renderBlock = render_block_full,
	#endif
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 3,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = true,
	.digging.hardness = 150000,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_water_flowing = {
	.name = "Water",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBlockAdded = onBlockAdded_flow,
	.onRandomTick = onWorldTick, /* BlockFluid setTickOnLoad(true) */
	.onRightClick = NULL,
	.onWorldTick = onWorldTick,
	.tick_delay = FLUID_WATER_TICK_RATE, // Beta BlockFluid.tickRate() water = 5
	.onNeighbourBlockChange = onNeighbourChange_flow,
	#ifdef GFX_FANCY_LIQUIDS
	.transparent = true,
	.renderBlock = render_block_fluid,
	#else
	.transparent = false,
	.renderBlock = render_block_full,
	#endif
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 3,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = true,
	.digging.hardness = 150000,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
