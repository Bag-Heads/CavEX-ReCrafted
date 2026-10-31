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
#include "../network/server_world.h"
#include "block_sand.h"
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

// BlockGravel extends BlockSand in Beta and inherits onBlockAdded/
// onNeighborBlockChange/updateTick UNCHANGED -- only idDropped (below)
// differs. Byte-identical wrappers to block_sand.c's own (kept per-file,
// like this port's other duplicated-per-block-file hook wrappers -- e.g. the
// three torch blocks each carry their own thin onBlockAdded/
// onNeighbourBlockChange in block_torch.c) rather than cross-including
// block_sand.c; the shared logic itself lives once in block_sand_try_to_fall.
// [decompile 740c583 BlockGravel extends BlockSand]
static void gravel_added(struct server_local* s, struct block_info* info) {
	server_world_schedule_block_tick(block_world(s), info->x, info->y, info->z,
									 info->block->type);
}

static void gravel_neighbour_change(struct server_local* s,
									struct block_info* info) {
	server_world_schedule_block_tick(block_world(s), info->x, info->y, info->z,
									 info->block->type);
}

static void gravel_world_tick(struct server_local* s, struct block_info* info) {
	block_sand_try_to_fall(s, info->x, info->y, info->z, info->block->type);
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
	return tex_atlas_lookup(TEXAT_GRAVEL);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		// Beta [740c583 BlockGravel.idDropped]: return var2.nextInt(10) == 0 ?
		// Item.flint.shiftedIndex : this.blockID. This project's nextInt(n) port
		// is rand_gen_int (see block_ore.c's drop_redstone/drop_lapis comments,
		// and block_leaves.c's FH-02 fix note) -- raw `rand_gen(g) % 10` is the
		// same ad hoc formula FH-02 already replaced there, kept as an outlier
		// here.
		it->id = jrand_next_int_bound(g, 10) == 0 ? ITEM_FLINT : this->block->type;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

struct block block_gravel = {
	.name = "Gravel",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBlockAdded = gravel_added,
	.onNeighbourBlockChange = gravel_neighbour_change,
	.onWorldTick = gravel_world_tick,
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
	.digging.hardness = 900,
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
