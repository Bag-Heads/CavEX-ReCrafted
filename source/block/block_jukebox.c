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

// Beta 1.7.3 BlockJukeBox (jukebox-slice, decompile 740c583). Right-click:
// metadata==0 (no record) -> blockActivated returns FALSE, falling through to
// the held item's use (ItemRecord.onItemUse in item_record.c handles INSERT);
// metadata!=0 -> ALWAYS eject (func_28038_b_), regardless of the held item.
// This maps directly onto the engine's onRightClickHandled scoped-consume
// dispatch (block_dispenser.c's onRightClick precedent, but here the block
// itself can decline the click). onBreak ejects first (drops the record), then
// the block's own default drop runs as usual -- mirrors Beta's onBlockRemoval
// calling func_28038_b_ before super.onBlockRemoval. Records are Mojang IP:
// audio playback (2026-08-31) is wired through the S3 streaming machinery
// (see server_local_jukebox_eject/_insert's CRPC_RECORD_EVENT dispatch and
// client_interface.c's handler) -- no Mojang IP is ever bundled, a client
// with no imported <pack>/records/ pack plays nothing; the SIM (insert/
// eject/consume/persist) is complete either way.

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_jukebox.h"
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
		case SIDE_TOP: return tex_atlas_lookup(TEXAT_JUKBEBOX_TOP);
		default: return tex_atlas_lookup(TEXAT_JUKBEBOX_SIDE);
	}
}

// Beta BlockJukeBox.blockActivated [decompile 740c583]: metadata==0 (no
// record) -> return false (unhandled -- ItemRecord.onItemUse may insert);
// else -> func_28038_b_ (eject) and return true UNCONDITIONALLY (even the
// defensive record==0 case inside server_local_jukebox_eject, which cannot
// arise from normal play, still counts as "handled" here, matching Beta
// exactly).
static bool onRightClickHandled(struct server_local* s, struct server_player* actor, struct item_data* it,
								struct block_info* where,
								struct block_info* on, enum side on_side) {
	if(on->block->metadata == 0)
		return false;
	server_local_jukebox_eject(s, on->x, on->y, on->z);
	return true;
}

// TE registration mirrors block_dispenser.c/block_noteblock.c's onItemPlace:
// no facing metadata (BlockJukeBox's metadata is the has-record bit, always
// 0 at placement).
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
	if(block_place_obstructed_for(s, actor, &bi))
		return false;

	bool had_te = jukebox_te_lookup_w(block_world(s), where->x, where->y, where->z) != NULL;
	if(had_te)
		return false;
	if(jukebox_te_register_w(block_world(s), where->x, where->y, where->z) < 0)
		return false;
	if(!server_world_set_block(block_world(s), where->x, where->y, where->z, blk)) {
		if(!had_te)
			jukebox_te_free_w(block_world(s), where->x, where->y, where->z);
		return false;
	}
	return true;
}

// Beta BlockJukeBox.onBlockRemoval: func_28038_b_ (eject, drops the record if
// any) FIRST, then the default removal -- the block's own getDroppedItem
// (block_drop_default below) still runs separately as usual.
static void onBreak(struct server_local* s, struct block_info* this) {
	server_local_jukebox_eject(s, this->x, this->y, this->z);
	jukebox_te_free_w(block_world(s), this->x, this->y, this->z);
}

struct block block_jukebox = {
	.name = "Jukebox",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onRightClickHandled = onRightClickHandled,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 3000,
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
