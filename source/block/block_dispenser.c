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

// Beta 1.7.3 BlockDispenser (dispenser-slice, decompile 740c583). Redstone
// (onNeighborBlockChange/updateTick): the SAME two-cell "isBlockIndirectly-
// GettingPowered(x,y,z) || (x,y+1,z)" pattern block_door.c already uses,
// origin-gated on the notifying block's canProvidePower() -- but unlike the
// door (which flips state SYNCHRONOUSLY), the dispenser SCHEDULES a
// tickRate()==4 delayed block tick and RE-CHECKS the same condition when it
// fires (power removed before the scheduled tick lands = no dispense). The
// actual ejection (dispenseItem) needs entity construction, which is private
// to server_local.c (server_entity_alloc/server_local_entity_join are
// `static` there) -- it lives in server_local_dispenser_dispense, mirroring
// how block_tnt.c delegates entity creation to server_local_prime_tnt_block
// rather than touching entities itself.

#include <string.h> // memcpy (onBreak's pre-teardown items[] snapshot)

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/client_interface.h"
#include "../network/inventory_logic.h"
#include "../network/server_dispenser.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"
#include "dispenser_logic.h"
#include "furnace_logic.h" // furnace_facing_meta: shared placer-yaw facing helper
#include "redstone_power.h"
#include "redstone_server.h"

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
	switch(side) {
		case SIDE_FRONT:
			if(this->block->metadata == 2)
				return tex_atlas_lookup(TEXAT_DISPENSER_FRONT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		case SIDE_BACK:
			if(this->block->metadata == 3)
				return tex_atlas_lookup(TEXAT_DISPENSER_FRONT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		case SIDE_RIGHT:
			if(this->block->metadata == 5)
				return tex_atlas_lookup(TEXAT_DISPENSER_FRONT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		case SIDE_LEFT:
			if(this->block->metadata == 4)
				return tex_atlas_lookup(TEXAT_DISPENSER_FRONT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		default: return tex_atlas_lookup(TEXAT_FURNACE_TOP);
	}
}

// Beta right-click ALWAYS opens the dispenser GUI (BlockDispenser.blockActivated
// -> EntityPlayer.displayGUIDispenser). Register/find the tile entity for this
// position first (mirrors block_furnace.c's onRightClick exactly) so a
// pre-persistence-loaded world's dispenser still has somewhere to keep its 9
// slots.
static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	/* C2 tiles (P2 row #10): Beta BlockDispenser.blockActivated opens the
	 * GUI for ANY player, on the TileEntityDispenser of the world it clicked
	 * in. Several windows on one dispenser alias ONE backing inventory
	 * (inventory_dispenser.c), as Beta's ContainerDispenser aliases one
	 * TileEntity. */
	if(actor->active_inventory != &actor->inventory)
		return; // a window is already open
	struct server_world* w = server_local_world_of(s, actor);
	if(!w)
		return; // the actor has no live world this tick

	if(dispenser_te_register_w(w, on->x, on->y, on->z) < 0)
		return;
	server_local_open_inventory_in(s, w, actor, &inventory_logic_dispenser,
									DISPENSER_SIZE, on->x, on->y, on->z,
									WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER);
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk = (struct block_data) {
		.type = it->id,
		.metadata = furnace_facing_meta(actor->rx),
		.sky_light = 0,
		.torch_light = 0,
	};
	struct block_info bi = *where;
	bi.block = &blk;
	if(block_place_obstructed_for(s, actor, &bi)) // Beta canBlockBePlacedAt entity gate
		return false;

	// A missing-block TE can be a retained no-loss spill after EntityItem OOM
	// (the furnace onItemPlace precedent). Never recycle it into a newly
	// placed dispenser before the retry succeeds.
	struct server_world* w = server_local_world_of(s, actor);
	if(!w)
		return false;
	bool had_te = dispenser_te_lookup_w(w, where->x, where->y, where->z) != NULL;
	if(had_te)
		return false;
	if(dispenser_te_register_w(w, where->x, where->y, where->z) < 0)
		return false;
	if(!server_world_set_block(w, where->x, where->y, where->z, blk)) {
		if(!had_te)
			dispenser_te_free_w(w, where->x, where->y, where->z);
		return false;
	}
	return true;
}

// Beta BlockDispenser.onNeighborBlockChange [decompile 740c583]: only when
// the NOTIFYING block canProvidePower (the var5 origin gate, read via
// server_world_notify_origin -- the door/TNT precedent), recompute powered =
// isBlockIndirectlyGettingPowered at (x,y,z) OR (x,y+1,z) -- note the y+1
// check, a dispenser also fires from power one block ABOVE it. If powered,
// schedule a tickRate()==4 delayed block tick (self-armed here because this
// block ALSO defines onNeighbourBlockChange -- GAP #1 in server_world.c
// suppresses the automatic constant-tick_delay auto-schedule for any block
// that does, exactly like the button/TNT precedent).
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	uint8_t origin = server_world_notify_origin();
	if(origin == 0 || !redstone_id_can_provide_power(origin))
		return;

	bool powered
		= redstone_server_indirectly_getting_powered(s, info->x, info->y, info->z)
		|| redstone_server_indirectly_getting_powered(s, info->x, info->y + 1,
													  info->z);
	if(powered)
		(void)server_world_schedule_block_tick(block_world(s), info->x, info->y,
											   info->z, BLOCK_DISPENSER);
}

// Beta BlockDispenser.updateTick [decompile 740c583]: RE-CHECK the identical
// two-cell powered condition (power removed before the scheduled tick lands
// = no dispense -- this re-check is the whole reason it is a SEPARATE method
// from the schedule call above, not a redundant restatement), then
// dispenseItem.
static void onWorldTick(struct server_local* s, struct block_info* info) {
	bool powered
		= redstone_server_indirectly_getting_powered(s, info->x, info->y, info->z)
		|| redstone_server_indirectly_getting_powered(s, info->x, info->y + 1,
													  info->z);
	if(powered)
		server_local_dispenser_dispense(s, info->x, info->y, info->z,
										info->block->metadata);
}

// Ownership teardown, independent of the block's OWN drop (getDroppedItem
// below always drops one plain dispenser item; this handles the 9-slot
// INVENTORY separately, exactly like furnace/chest's onBreak split).
//
// D6 (adversarial review, 2026-08-13): the Beta-faithful scatter
// (server_local_dispenser_scatter_contents) runs UNCONDITIONALLY, from a
// snapshot taken before any window teardown, so a break with the dispenser's
// own GUI open produces the SAME drop distribution as one with it closed --
// no more falling back to the generic chest-style spill for that case.
// server_local_dispenser_invalidate_open_window (a no-op if no matching
// window is open) only marks the window invalidated and clears its visible
// copy of the storage slots; it does not itself spawn anything.
/* Every remaining window on this dispenser (any present body's, bound to
 * this world) loses its duplicate storage copy and is invalidated once the
 * contents left the tile. */
static void dispenser_detach_windows(struct server_local* s,
									 struct server_world* w, w_coord_t x,
									 w_coord_t y, w_coord_t z) {
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		struct inventory* inv = body ? body->active_inventory : NULL;
		if(!inv || inv == &body->inventory
		   || inv->logic != &inventory_logic_dispenser || inv->x != x
		   || inv->y != y || inv->z != z
		   || !server_local_window_bound_to(inv, w))
			continue;
		inv->invalidated = true;
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		for(int k = 0; k < DISPENSER_TE_SLOTS; k++) {
			size_t slot = DISPENSER_SLOT_STORAGE + (size_t)k;
			if(inv->items[slot].id != 0) {
				inventory_clear_slot(inv, slot);
				set_inv_slot_push(changes, slot);
			}
		}
		server_local_send_inv_changes(changes, inv, WINDOWC_DISPENSER);
		set_inv_slot_clear(changes);
	}
}

static void onBreak(struct server_local* s, struct block_info* this) {
	/* Block callbacks carry no world; removals run in world slot 0 today.
	 * server_local_dispenser_invalidate_open_window clears every present
	 * body's window bound to that world's dispenser (C2 tiles, row #13). */
	struct server_world* w = block_world(s);
	struct dispenser_te* d = dispenser_te_lookup_w(w, this->x, this->y, this->z);
	if(!d)
		return;

	struct item_data items[DISPENSER_TE_SLOTS];
	memcpy(items, d->items, sizeof(items));

	server_local_dispenser_invalidate_open_window(s, this->x, this->y,
												  this->z);

	// Beta BlockDispenser.onBlockRemoval's item scatter -- see
	// server_local_dispenser_scatter_contents for the full citation. Reads
	// the snapshot taken above (mirrors Beta reading the live
	// TileEntityDispenser before super.onBlockRemoval tears it down).
	server_local_dispenser_scatter_contents(s, this->x, this->y, this->z,
											items);
	dispenser_te_free_w(w, this->x, this->y, this->z);
	dispenser_detach_windows(s, w, this->x, this->y, this->z);
}

struct block block_dispenser = {
	.name = "Dispenser",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onWorldTick = onWorldTick, // Beta updateTick (the scheduled dispense)
	.tick_delay = 4,			// Beta BlockDispenser.tickRate()
	.onNeighbourBlockChange = onNeighbourBlockChange,
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
	// Beta hardness x1500: Block.java:615 dispenser `.setHardness(3.5F)` ->
	// 5250 (already correct). `.best = TOOL_TIER_ANY` (not WOOD):
	// ItemPickaxe.blocksEffectiveAgainst omits Block.dispenser entirely, so
	// getStrVsBlock is flat 1.0F for any pickaxe tier -- no per-tier bonus
	// (same mechanism as block_ore.c's redstone-ore comment).
	.digging.hardness = 5250,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_WOOD,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.fuel = 0,
		.render_data.block.has_default = true,
		.render_data.block.default_metadata = 2,
		.render_data.block.default_rotation = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
