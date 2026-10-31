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

#include "../network/server_spawner.h" // spawner_te_free (FH-17)
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_SPAWNER);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	return 0;
}

// FH-17: ownership teardown -- the spawner's mob type/delay is never dropped
// as an item (getDroppedItem above is Beta-faithful 0: digging a mob spawner
// drops nothing), but the spawner_te_pos/spawner_tes row must not survive the
// block, or a later save would persist a "MobSpawner" tile entity at a
// position that is no longer one. Mirrors block_noteblock.c's onBreak/
// noteblock_te_free precedent exactly.
static void onBreak(struct server_local* s, struct block_info* this) {
	spawner_te_free(s, this->x, this->y, this->z); /* A1b: slot-0 adapter (D4 bridge) */
}

struct block block_spawner = {
	.name = "Monster Spawner",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	// Beta lightOpacity[52] == 0: Block.<init> (Block.java:151-152) sets
	// `lightOpacity = isOpaqueCube() ? 255 : 0`, BlockMobSpawner.isOpaqueCube
	// (BlockMobSpawner.java:22-24) is false, and the registration
	// (Block.java:75) never calls setLightOpacity. So a spawner neither stops
	// the heightmap nor dims skylight passing through it (the glass/cactus
	// encoding: can_see_through + opacity 0). It was 1, which darkened
	// every sky-exposed spawner column by one level per cell and raised the
	// heightmap onto it. Block-light/sky horizontal spread is unchanged
	// (lighting.c uses MAX(opacity, 1)); LOS is unchanged (opacity != 15).
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta hardness x1500: Block.java:644 mobSpawner `.setHardness(5.0F)` ->
	// 7500 (already correct). `.best = TOOL_TIER_ANY` (not WOOD):
	// ItemPickaxe.blocksEffectiveAgainst omits Block.mobSpawner entirely, so
	// getStrVsBlock is flat 1.0F for any pickaxe tier -- no per-tier bonus
	// (same mechanism as block_ore.c's redstone-ore comment).
	.digging.hardness = 7500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_WOOD,
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
