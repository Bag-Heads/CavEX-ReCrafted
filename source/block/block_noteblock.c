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

// Beta 1.7.3 BlockNote (noteblock-slice, decompile 740c583). Right-click
// cycles the pitch and plays it (changePitch + triggerNote, ALWAYS consumed --
// no window, so the plain void onRightClick form is correct); redstone
// rising-edge ALSO triggers (onNeighborBlockChange, gated on the notifying
// block's canProvidePower, the door/dispenser precedent). Left-click (Beta
// onBlockClicked, trigger-only, no pitch change) rides the server dig-START
// dispatch added by R1 (struct block.onBlockClicked, 2026-09-24), which closed
// the old "no punch hook" deviation.

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_noteblock.h"
#include "blocks.h"
#include "noteblock_logic.h"
#include "redstone_power.h"
#include "redstone_server.h"

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
	return tex_atlas_lookup(TEXAT_JUKBEBOX_SIDE);
}

// Beta BlockNote.blockActivated: changePitch() then triggerNote(), ALWAYS
// returns true. Register/find the TE first (mirrors block_dispenser.c/
// block_furnace.c's onRightClick) so a pre-persistence-loaded world's note
// block still has somewhere to keep its value. [decompile 740c583]
static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	int8_t* note = noteblock_te_lookup_w(block_world(s), on->x, on->y, on->z);
	if(!note)
		return; // no registered TE at this position (shouldn't normally happen)
	*note = noteblock_change_pitch((uint8_t)*note);
	server_local_noteblock_trigger(s, on->x, on->y, on->z);
}

// Beta BlockNote.onNeighborBlockChange [decompile 740c583]: gated on the
// NOTIFYING block's canProvidePower() (the var5 origin gate, read via
// server_world_notify_origin -- the door/dispenser precedent). Recompute
// powered = isBlockGettingPowered(x,y,z) -- the WEAK/direct query (NO "through
// a solid cube" hop, and a single cell -- unlike the dispenser's
// isBlockIndirectlyGettingPowered + y+1 second check, both dispenser-specific
// and NOT part of BlockNote). RISING EDGE only: trigger fires ONLY when the
// state actually CHANGES to powered==true; the stored state updates on EVERY
// change (including the falling edge), matching Beta's
// `if(previousRedstoneState != var6) { if(var6) trigger(); previousRedstoneState = var6; }`
// -- a no-op state resync never happens because the outer gate already
// requires a real change to reach here.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	uint8_t origin = server_world_notify_origin();
	if(origin == 0 || !redstone_id_can_provide_power(origin))
		return;

	bool* prev = noteblock_prev_powered_lookup_w(block_world(s), info->x, info->y, info->z);
	if(!prev)
		return; // no registered TE at this position

	bool powered
		= redstone_server_getting_powered(s, info->x, info->y, info->z);
	if(*prev != powered) {
		if(powered)
			server_local_noteblock_trigger(s, info->x, info->y, info->z);
		*prev = powered;
	}
}

// Beta canBlockBePlacedAt entity gate + TE registration, mirroring
// block_dispenser.c's onItemPlace with no facing metadata: BlockNote's block
// metadata is unused (the note VALUE lives entirely in the TE, unlike the
// dispenser's direction nibble).
static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk = (struct block_data) {
		.type = it->id,
		.metadata = 0,
		.sky_light = 0,
		.torch_light = 0,
	};
	struct block_info bi = *where;
	bi.block = &blk;
	if(block_place_obstructed_for(s, actor, &bi)) // Beta canBlockBePlacedAt entity gate
		return false;

	// A missing-block TE can be a retained no-loss spill after an EntityItem
	// OOM elsewhere (the furnace/dispenser onItemPlace precedent). Never
	// recycle it into a newly placed note block before the retry succeeds.
	bool had_te
		= noteblock_te_lookup_w(block_world(s), where->x, where->y, where->z) != NULL;
	if(had_te)
		return false;
	if(noteblock_te_register_w(block_world(s), where->x, where->y, where->z) < 0)
		return false;
	if(!server_world_set_block(block_world(s), where->x, where->y, where->z, blk)) {
		if(!had_te)
			noteblock_te_free_w(block_world(s), where->x, where->y, where->z);
		return false;
	}
	return true;
}

// Ownership teardown: the note value itself is not dropped as an item (only
// the block, via the default getDroppedItem below).
static void onBreak(struct server_local* s, struct block_info* this) {
	noteblock_te_free_w(block_world(s), this->x, this->y, this->z);
}

// R1: Beta BlockNote.onBlockClicked -- a LEFT-click plays the CURRENT pitch once via
// triggerNote, with NO changePitch (the server world is never multiplayerWorld). A missing TE
// plays nothing, like the right-click path. [740c583: BlockNote.java:38-43]
static void onBlockClicked(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this) {
	(void)actor;
	(void)held;
	if(!noteblock_te_lookup_w(block_world(s), this->x, this->y, this->z))
		return;
	server_local_noteblock_trigger(s, this->x, this->y, this->z);
}

struct block block_noteblock = {
	.name = "Note Block",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRightClick = onRightClick,
	.onBlockClicked = onBlockClicked,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 1200,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
