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
#include "../network/server_local.h" // server_world_set_block (redstone ore glow/revert)
#include "../network/server_local_state.h" // full server state (s->world)
#include "../network/server_world.h" // server_world_set_block
#include "blocks.h"

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
	return face_occlusion_full();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(this->block->type) {
		case 14: // gold
			return tex_atlas_lookup(TEXAT_ORE_GOLD);
		case 15: // iron
			return tex_atlas_lookup(TEXAT_ORE_IRON);
		case 16: // coal
			return tex_atlas_lookup(TEXAT_ORE_COAL);
		case 21: // lapis
			return tex_atlas_lookup(TEXAT_ORE_LAPIS);
		case 56: // diamond
			return tex_atlas_lookup(TEXAT_ORE_DIAMOND);
		case 73: // redstone
		case 74: return tex_atlas_lookup(TEXAT_ORE_REDSTONE);
		default: return tex_atlas_lookup(TEXAT_STONE);
	}
}

static size_t drop_coal(struct block_info* this, struct item_data* it,
						struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_COAL;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

static size_t drop_diamond(struct block_info* this, struct item_data* it,
						   struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_DIAMOND;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

static size_t drop_redstone(struct block_info* this, struct item_data* it,
							struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_REDSTONE;
		it->durability = 0;
		it->count = 1; // idDropped only; quantity is evaluated before chance
	}

	return 1;
}

static size_t drop_lapis(struct block_info* this, struct item_data* it,
						 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_DYE;
		it->durability = 4;
		it->count = 1; // idDropped only; quantity is evaluated before chance
	}

	return 1;
}

// Beta BlockRedstoneOre.func_320_h -- the private helper shared by
// onBlockClicked (left-click), onEntityWalking (any entity standing on it),
// and blockActivated (right-click): swap 73 (unlit) -> 74 (lit), keeping
// metadata/light untouched. Called on EITHER id (both are the SAME
// BlockRedstoneOre class, gated by `this.blockID == oreRedstone.blockID`), so
// touching the already-lit ore is correctly a no-op here too. sparkle() is a
// client-only particle spawn with no server-visible effect -- skipped, not
// ported. [decompile 740c583 BlockRedstoneOre.func_320_h]
//
// R1 (docs/intent/r1-block-click-walk-callbacks.md): all three Beta triggers are wired --
// onBlockClicked (the server dig-start dispatch, server_local_block_clicked),
// onEntityWalking (the footstep dispatch, server_local_entity_walked_on_block, for the
// player AND every canTriggerWalking mob), and blockActivated (onRightClick, below).
static void redstone_ore_touch(struct server_local* s, struct block_info* info) {
	if(info->block->type != 73) // already lit: func_320_h's swap condition is false
		return;
	struct block_data b = *info->block;
	b.type = 74;
	server_world_set_block(block_world(s), info->x, info->y, info->z, b);
}

static void redstone_ore_right_click(struct server_local* s, struct server_player* actor, struct item_data* it,
									 struct block_info* where, struct block_info* on,
									 enum side on_side) {
	redstone_ore_touch(s, on);
}

// Beta BlockRedstoneOre.onBlockClicked: func_320_h, then super (base no-op). The held item is
// irrelevant. [740c583: BlockRedstoneOre.java:21-24]
static void redstone_ore_clicked(struct server_local* s, struct server_player* actor,
								 const struct item_data* held, struct block_info* this) {
	(void)actor;
	(void)held;
	redstone_ore_touch(s, this);
}

// Beta BlockRedstoneOre.onEntityWalking: func_320_h, then super (base no-op), for ANY walker
// (player or mob -- no player-only gate exists here). [740c583: BlockRedstoneOre.java:26-29]
static void redstone_ore_walked(struct server_local* s, struct block_info* this,
								enum block_walker walker) {
	(void)walker;
	redstone_ore_touch(s, this);
}

// Beta BlockRedstoneOre.updateTick: `if(blockID == oreRedstoneGlowing.blockID)
// setBlockWithNotify(x, y, z, oreRedstone)` -- revert to 73, same metadata/light,
// drawing NOTHING from its Random. The ONLY caller is the RANDOM tick: the lit
// ore is `new BlockRedstoneOre(74, 51, true)` -> setTickOnLoad(true), and
// World.updateBlocksAndPlayCaveSounds calls updateTick for tickOnLoad ids on its
// 80 samples/chunk/tick. Nothing ever schedules a tick for the ore (tickRate()
// 30 is dead code for it), so the glow lasts a random time (mean 32768/80 =
// 409.6 ticks, ~20 s), not 30 ticks. The unlit id 73 is not tickOnLoad and
// never random-ticks. [740c583: BlockRedstoneOre.java:8-14,41-46;
// Block.java:218; World.java:1945-1956] (next-gaps-0924/W2-R1b)
static void redstone_ore_revert(struct server_local* s, struct block_info* info) {
	struct block_data b = *info->block;
	b.type = 73;
	server_world_set_block(block_world(s), info->x, info->y, info->z, b);
}

struct block block_coalore = {
	.name = "Coal ore",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = drop_coal,
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
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_WOOD,
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

struct block block_ironore = {
	.name = "Iron ore",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
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
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_STONE,
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

/* Gold ore digging [decompile 740c583]: Block.java:606 setHardness(3.0F) ->
 * 4500ms (FH-01 field scale beta*1500). ItemPickaxe.canHarvestBlock: oreGold/
 * blockGold need harvestLevel >= 2 -- IRON or DIAMOND only (stone pickaxe is
 * level 1, gold pickaxe level 0), hence `.min = TOOL_TIER_IRON`. oreGold IS a
 * blocksEffectiveAgainst member, so a harvest-capable pickaxe gets its own
 * tier's efficiencyOnProperMaterial: `.best = TOOL_TIER_MAX`. A below-min tool
 * takes Block.blockStrength's cannot-harvest 1.0F/hardness/100 branch
 * [Block.java:331] -- the literal 1.0F means NO efficiency bonus applies
 * there, which tool.c's flat *10/3 penalty models exactly. (Was
 * TOOL_TIER_STONE: a stone pickaxe both harvested and tier-sped gold ore --
 * parity_audit_2026-08-25 lower-tier finding.) */
struct block block_goldore = {
	.name = "Gold ore",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
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
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_IRON,
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

struct block block_diamondore = {
	.name = "Diamond ore",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = drop_diamond,
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
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_IRON,
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

struct block block_redstoneore = {
	.name = "Redstone ore",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = drop_redstone,
	.onRandomTick = NULL,
	.onRightClick = redstone_ore_right_click,
	.onBlockClicked = redstone_ore_clicked,
	.onEntityWalking = redstone_ore_walked,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	// Beta digging speed is TWO mechanisms, not one -- Block.blockStrength
	// [740c583:327-333]:
	//   !canHarvestBlock ? 1/hardness/100 : getCurrentPlayerStrVsBlock/hardness/30
	// canHarvestBlock [InventoryPlayer.canHarvestBlock]: Material.rock has
	// setNoHarvest(), so it falls to the held item's own canHarvestBlock ->
	// ItemPickaxe: harvestLevel >= 2 for oreRedstone/oreRedstoneGlowing (iron
	// and diamond only; hand/wood/stone/gold fail -> the /100 branch, `.min`
	// below). getCurrentPlayerStrVsBlock is the held item's getStrVsBlock,
	// which is flat 1.0F for THIS block regardless of tier -- oreRedstone is
	// OMITTED from ItemPickaxe.blocksEffectiveAgainst entirely, so even a
	// qualifying (iron/diamond) pickaxe gets no per-tier speed multiplier on
	// top of the /30 branch. `.best = TOOL_TIER_ANY` encodes exactly that: it
	// caps BOTH iron and diamond down to tool_tier_divider's no-bonus default
	// (1), so they dig at the same flat hardness/30-ticks speed as each other
	// -- never their own tier's efficiencyOnProperMaterial (6/8), which would
	// only apply on a block this ore is NOT a member of. `.min = TOOL_TIER_IRON`
	// still drives the separate harvest/drop gate in server_local.c, unchanged.
	.digging.min = TOOL_TIER_IRON,
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

struct block block_redstoneore_lit = {
	.name = "Redstone ore",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = drop_redstone,
	.onRandomTick = redstone_ore_revert, // Block.tickOnLoad -> random-tick updateTick
	.onRightClick = redstone_ore_right_click,
	.onBlockClicked = redstone_ore_clicked,
	.onEntityWalking = redstone_ore_walked,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 9,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	// Same Beta mechanism as the unlit variant above (Block.blockStrength
	// [740c583:327-333] + InventoryPlayer/ItemPickaxe.canHarvestBlock):
	// oreRedstoneGlowing is the same block, just lit. `.min = TOOL_TIER_IRON`
	// is the harvest gate (canHarvestBlock needs level >= 2); `.best =
	// TOOL_TIER_ANY` caps iron AND diamond to the same no-bonus divider,
	// matching blocksEffectiveAgainst's omission (flat 1.0F for every tier).
	.digging.min = TOOL_TIER_IRON,
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

struct block block_lapisore = {
	.name = "Lapis lazuli ore",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = drop_lapis,
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
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_STONE,
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
