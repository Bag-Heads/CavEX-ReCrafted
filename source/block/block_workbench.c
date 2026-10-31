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
#include "../network/client_interface.h"
#include "../network/inventory_logic.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_WOOD;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(side) {
		case SIDE_FRONT:
		case SIDE_LEFT: return tex_atlas_lookup(TEXAT_WORKBENCH_SIDE_2);
		case SIDE_RIGHT:
		case SIDE_BACK: return tex_atlas_lookup(TEXAT_WORKBENCH_SIDE_1);
		case SIDE_TOP: return tex_atlas_lookup(TEXAT_WORKBENCH_TOP);
		default:
		case SIDE_BOTTOM: return tex_atlas_lookup(TEXAT_PLANKS);
	}
}

static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	if(actor->active_inventory == &actor->inventory) {
		server_local_open_inventory_for(
			s, actor, &inventory_logic_crafting, CRAFTING_SIZE, 0, 0, 0,
			WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH);
	}
}

struct block block_workbench = {
	.name = "Workbench",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta hardness x1500: Block.java:650 workbench `.setHardness(2.5F)` ->
	// 3750 (already correct). `.best = TOOL_TIER_ANY` (not MAX):
	// ItemAxe.blocksEffectiveAgainst = {planks, bookShelf, wood, chest}
	// omits Block.workbench entirely, so getStrVsBlock is flat 1.0F for any
	// axe tier -- no per-tier bonus (same mechanism as block_ore.c's
	// redstone-ore comment). Without this, TOOL_TIER_MAX let the equipped
	// axe's own tier divide the delay, mining the workbench up to 8x
	// (diamond axe, tool_tier_divider(DIAMOND)=8) too fast.
	.digging.hardness = 3750,
	.digging.tool = TOOL_TYPE_AXE,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default,
		.fuel = 1,
		.render_data.block.has_default = true,
		.render_data.block.default_metadata = 0,
		.render_data.block.default_rotation = 2,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
