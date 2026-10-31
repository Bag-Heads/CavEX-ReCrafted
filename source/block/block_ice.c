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
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
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
	return (it->block->type == this->block->type) ? face_occlusion_full() :
													face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return TEXTURE_INDEX(4, 0);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	return 0;
}

// Beta BlockIce.updateTick (weather slice, gap #2 decompile-corrected -- BLOCK-
// light driven, NOT temperature): `if (getSavedLightValue(BLOCK,...) >
// 11 - Block.lightOpacity[ice]) { dropBlockAsItem(...);
// setBlockWithNotify(waterStill); }` [740c583]. lightOpacity[ice] == 3 (this
// block's own `.opacity` below), so the threshold is 11-3 == 8. Like snow,
// BlockIce.quantityDropped() returns 0 -- dropBlockAsItem's draw loop runs
// zero iterations, so melted ice drops NOTHING (getDroppedItem above, the
// player-mined path, is a separate call site and unaffected).
static void onRandomTick(struct server_local* s, struct block_info* this) {
	struct block_data at;
	if(!server_world_get_block(block_world(s), this->x, this->y, this->z, &at))
		return;
	if((int)at.torch_light > 11 - 3)
		server_world_set_block(block_world(s), this->x, this->y, this->z,
							   (struct block_data) {.type = BLOCK_WATER_STILL});
}

// Beta BlockIce.harvestBlock [740c583: BlockIce.java]:
//   the base harvestBlock runs first; then, when the material directly
//    below the broken cell (y - 1) is solid or liquid,
//        the broken cell becomes Block.waterMoving via
//        setBlockWithNotify; otherwise it stays air.
//
//
//
//
// Note: waterMOVING (flowing, id 8), NOT waterStill -- unlike updateTick's melt
// path above, which sets waterStill directly. harvestBlock() is only reached from
// ItemInWorldManager's removed-and-harvestable gate on harvestBlock, where harvestable =
// EntityPlayer.canHarvestBlock(this) -- so this conversion is PLAYER-HARVEST ONLY.
// This port's SRPC_BLOCK_DIG handler (server_local.c) is the one call site that
// mirrors that caller (its own comment there documents the
// PlayerControllerSP.sendBlockRemoved mapping); it invokes this function directly,
// gated by the SAME harvest-gate condition (harvestable) it already computes for the
// drop-spawn call. Deliberately NOT wired through the generic block->onBreak hook:
// onBreak also fires from the TNT explosion path (server_world.c) and from
// server_local_spawn_block_drops (piston-push / support-loss pop callers in
// block_lever.c, block_torch.c, etc.) -- Beta's harvestBlock is never invoked from
// Explosion.doExplosionB (which calls dropBlockAsItemWithChance directly) or from
// any non-player removal, so wiring the water-conversion through onBreak would
// incorrectly melt ice that a TNT blast or a piston push destroys, which Beta does
// not do. onRandomTick's melt path above is likewise untouched (it never calls
// this function -- it sets waterStill inline, matching updateTick's own separate
// setBlockWithNotify call).
void block_ice_harvest(struct server_local* s, struct block_info* this) {
	struct block_data below;
	if(!server_world_get_block(block_world(s), this->x, this->y - 1, this->z, &below))
		return;

	// Material.getIsSolid(): block_beta_material_is_solid (block_lava.c) is the
	// existing per-id transcription of the SAME Beta Material static-init table
	// (false only for air/liquids/fire/plants/circuits/snow-layer/portal).
	// Material.getIsLiquid(): true only for MaterialLiquid, i.e. the four
	// water/lava block ids (block_beta_material_is_solid already reports these as
	// NOT solid, so they need the explicit OR below, mirroring Beta's own
	// `getIsSolid() || getIsLiquid()`).
	bool below_solid_or_liquid = block_beta_material_is_solid(below.type)
		|| below.type == BLOCK_WATER_FLOW || below.type == BLOCK_WATER_STILL
		|| below.type == BLOCK_LAVA_FLOW || below.type == BLOCK_LAVA_STILL;

	if(below_solid_or_liquid)
		server_world_set_block(block_world(s), this->x, this->y, this->z,
							   (struct block_data) {.type = BLOCK_WATER_FLOW});
}

struct block block_ice = {
	.name = "Ice",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = onRandomTick,
	.onRightClick = NULL,
	.transparent = true,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 3, // Beta Block.ice setLightOpacity(3) -- a skylight VALUE, not render-only
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750, // Beta setHardness(0.5F); scale is 1500 per hardness unit
	// FIX (field report: "breaking ice by hand takes far too long"). Beta
	// Block.blockStrength(EntityPlayer) [740c583: Block.java:327-333]:
	//   !canHarvestBlock(this) ? 1.0F/hardness/100.0F
	//                          : getCurrentPlayerStrVsBlock(this)/hardness/30.0F
	// InventoryPlayer.canHarvestBlock: a harvestable material always passes;
	// otherwise the held item's canHarvestBlock decides, and an empty hand fails.
	// Material.ice [740c583: Material.java static init] is
	// a Material built on MapColor.iceColor, then setIsTranslucent() -- it NEVER calls
	// setNoHarvest(), so getIsHarvestable() is the default TRUE. canHarvestBlock
	// therefore returns true UNCONDITIONALLY for ice, even bare hand -- ice
	// ALWAYS takes the fast /30 branch, never the /100 "wrong tool" penalty.
	// The previous `.min = TOOL_TIER_WOOD` wrongly routed a bare hand through
	// tool.c's `delay*10/3` penalty branch (the /100-branch proxy, correct only
	// for materials that DO require a tool, e.g. Material.rock/setNoHarvest --
	// see block_stone.c/block_ore.c), giving 750*10/3 = 2500ms (50 ticks) instead
	// of Beta's true hand time. `.min = TOOL_TIER_ANY` is this codebase's
	// existing "hand-harvestable, but `.tool` is still the EFFECTIVE tool for the
	// speed bonus" convention (block_dirt.c/block_log.c/block_grass.c/
	// block_sand.c all pair a specific `.tool` with `.min = TOOL_TIER_ANY` the
	// same way) -- with it, tool.c takes neither the pickaxe-match branch (hand's
	// item_type != PICKAXE) nor the `min != ANY` penalty branch, leaving
	// delay = hardness undivided = 750ms (15 ticks = 0.75s), matching
	// hardness*30 ticks * 50ms with getCurrentPlayerStrVsBlock's hand default of
	// 1.0F exactly.
	// Ice IS in ItemPickaxe.blocksEffectiveAgainst [740c583: ItemPickaxe.java
	// static init], so a pickaxe still gets ItemTool.getStrVsBlock's flat
	// per-tier efficiencyOnProperMaterial bonus (wood=2/stone=4/iron=6/
	// diamond=8/gold=12, EnumToolMaterial -- tool.c's tool_tier_divider mirrors
	// this table exactly) via the SAME /30 branch (canHarvestBlock is already
	// true, so no separate "harvest gate" changes): wood 750/2->400ms(8 ticks),
	// stone 750/4->200ms(4 ticks), iron 750/6->150ms(3 ticks),
	// diamond 750/8->100ms(2 ticks), gold 750/12->100ms(2 ticks) (tests/
	// test_ice_dig.c pins all six values via tool_dig_delay_ms).
	.digging.tool = TOOL_TYPE_PICKAXE,
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
