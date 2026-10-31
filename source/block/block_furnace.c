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
#include "../network/server_furnace.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
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

// Idle furnace: the UNLIT front on the facing side (metadata 2/3/5/4 = front/back/
// right/left), FURNACE_SIDE elsewhere, FURNACE_TOP on top/bottom. (Same facing layout
// as the lit block's getTextureIndex2; metadata now holds ONLY facing, never burn
// state -- burn lives in the tile entity.)
static uint8_t getTextureIndex1(struct block_info* this, enum side side) {
	switch(side) {
		case SIDE_TOP:
		case SIDE_BOTTOM:
			return tex_atlas_lookup(TEXAT_FURNACE_TOP);
		case SIDE_FRONT:
			return tex_atlas_lookup(this->block->metadata == 2
										? TEXAT_FURNACE_FRONT
										: TEXAT_FURNACE_SIDE);
		case SIDE_BACK:
			return tex_atlas_lookup(this->block->metadata == 3
										? TEXAT_FURNACE_FRONT
										: TEXAT_FURNACE_SIDE);
		case SIDE_RIGHT:
			return tex_atlas_lookup(this->block->metadata == 5
										? TEXAT_FURNACE_FRONT
										: TEXAT_FURNACE_SIDE);
		case SIDE_LEFT:
			return tex_atlas_lookup(this->block->metadata == 4
										? TEXAT_FURNACE_FRONT
										: TEXAT_FURNACE_SIDE);
		default:
			return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
	}
}

static uint8_t getTextureIndex2(struct block_info* this, enum side side) {
	switch(side) {
		case SIDE_FRONT:
			if(this->block->metadata == 2)
				return tex_atlas_lookup(TEXAT_FURNACE_FRONT_LIT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		case SIDE_BACK:
			if(this->block->metadata == 3)
				return tex_atlas_lookup(TEXAT_FURNACE_FRONT_LIT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		case SIDE_RIGHT:
			if(this->block->metadata == 5)
				return tex_atlas_lookup(TEXAT_FURNACE_FRONT_LIT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		case SIDE_LEFT:
			if(this->block->metadata == 4)
				return tex_atlas_lookup(TEXAT_FURNACE_FRONT_LIT);
			else
				return tex_atlas_lookup(TEXAT_FURNACE_SIDE);
		default: return tex_atlas_lookup(TEXAT_FURNACE_TOP);
	}
}

// Beta: right-click ALWAYS opens the furnace GUI (fuel now goes in the fuel SLOT, no
// longer into block metadata). Register/find the tile entity for this position so the
// per-tick smelter has somewhere to keep burn/cook + the 3 ItemStacks; a furnace with
// no TE record yet (a pre-persistence loaded world) registers here lazily.
static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	/* C2 tiles (P2 row #9): Beta BlockFurnace.blockActivated opens the GUI
	 * for ANY player, on the TileEntityFurnace of the world it clicked in
	 * (opening world = server_local_world_of(actor)). Several windows on one
	 * furnace alias ONE backing inventory (inventory_furnace.c: refresh
	 * before a click, write-through + peer publish after, no close
	 * write-back), as every Beta ContainerFurnace aliases one TileEntity. */
	if(actor->active_inventory != &actor->inventory)
		return; // a window is already open
	struct server_world* w = server_local_world_of(s, actor);
	if(!w)
		return; // the actor has no live world this tick

	if(furnace_te_register_w(w, on->x, on->y, on->z) < 0)
		return;
	server_local_open_inventory_in(s, w, actor, &inventory_logic_furnace,
									FURNACE_SIZE, on->x, on->y, on->z,
									WINDOWC_FURNACE, WINDOW_TYPE_FURNACE);
}

// Beta BlockFurnace.onBlockPlacedBy: facing from the placer's yaw (front faces the
// placer). player.rx is ALREADY in degrees (== Beta rotationYaw), so feed it straight
// to furnace_facing_meta -- do NOT convert radians->degrees (that ~57x-inflated the
// angle and gave a near-random front face). Then register the TE.
static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk = (struct block_data) {.type = BLOCK_FURNACE,
												 .metadata
												 = furnace_facing_meta(actor->rx),
												 .sky_light = 0,
												 .torch_light = 0};
	struct block_info bi = *where;
	bi.block = &blk;
	if(block_place_obstructed_for(s, actor, &bi)) // Beta canBlockBePlacedAt entity gate
		return false;

	// A missing-block TE can be a retained no-loss spill after EntityItem OOM.
	// Never recycle it into a newly placed furnace before the retry succeeds.
	struct server_world* w = server_local_world_of(s, actor);
	if(!w)
		return false;
	bool had_te = furnace_te_lookup_w(w, where->x, where->y, where->z) != NULL;
	if(had_te)
		return false;
	if(furnace_te_register_w(w, where->x, where->y, where->z) < 0)
		return false;
	if(!server_world_set_block(w, where->x, where->y, where->z, blk)) {
		if(!had_te)
			furnace_te_free_w(w, where->x, where->y, where->z);
		return false;
	}
	return true;
}

/* The live window (any present body's) viewing THIS world's furnace at
 * (x,y,z), or NULL. Beta: only a ContainerFurnace whose TileEntityFurnace is
 * the removed tile stops being usable (TileEntityFurnace.canInteractWith
 * compares worldObj.getBlockTileEntity(x,y,z) to itself); a window bound to
 * another world's furnace at the same coordinates views a different tile and
 * must not be spilled by this removal (C2 tiles, row #13). */
static struct inventory* live_furnace_window(struct server_local* s,
											 struct server_world* w,
											 w_coord_t x, w_coord_t y,
											 w_coord_t z) {
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		if(!body)
			continue;
		struct inventory* inv = body->active_inventory;
		if(inv && inv != &body->inventory
		   && inv->logic == &inventory_logic_furnace && inv->x == x
		   && inv->y == y && inv->z == z
		   && server_local_window_bound_to(inv, w))
			return inv;
	}
	return NULL;
}

// Both the idle and lit furnace drop the idle furnace item. Tile-entity
// ownership is handled independently by onBreak, even when TNT drops no block.
static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	(void)g;
	if(it) {
		it->id = BLOCK_FURNACE;
		it->durability = 0;
		it->count = 1;
	}
	return 1;
}

/* After the furnace's contents left the tile (scattered, or spilled from the
 * authoritative window), every remaining window on it -- any present body's,
 * bound to this world -- loses its now-duplicate copy of the storage and is
 * invalidated, so neither a click nor its close can bring those stacks back
 * (TileEntityFurnace.canInteractWith fails once the tile is gone). */
static void furnace_detach_windows(struct server_local* s,
								   struct server_world* w, w_coord_t x,
								   w_coord_t y, w_coord_t z) {
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		struct inventory* inv = body ? body->active_inventory : NULL;
		if(!inv || inv == &body->inventory
		   || inv->logic != &inventory_logic_furnace || inv->x != x
		   || inv->y != y || inv->z != z
		   || !server_local_window_bound_to(inv, w))
			continue;
		inv->invalidated = true;
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		static const size_t wslot[3]
			= {FURNACE_SLOT_INPUT, FURNACE_SLOT_FUEL, FURNACE_SLOT_OUTPUT};
		for(int k = 0; k < 3; k++)
			if(inv->items[wslot[k]].id != 0) {
				inventory_clear_slot(inv, wslot[k]);
				set_inv_slot_push(changes, wslot[k]);
			}
		server_local_send_inv_changes(changes, inv, WINDOWC_FURNACE);
		set_inv_slot_clear(changes);
	}
}

static void onBreak(struct server_local* s, struct block_info* this) {
	/* Block callbacks carry no world; removals run in world slot 0 today
	 * (the handoff note asks for the removal world on the break path). */
	struct server_world* w = block_world(s);
	struct furnace_te* f = furnace_te_lookup_w(w, this->x, this->y, this->z);
	if(!f)
		return;
	if(!s->death_drop_active) {
		if(server_local_furnace_scatter_contents(
			   s, this->x, this->y, this->z, f->items)) {
			furnace_te_free_w(w, this->x, this->y, this->z);
			furnace_detach_windows(s, w, this->x, this->y, this->z);
		}
		return; // failed scatter retains the TE for the orphan retry pass
	}
	struct inventory* viewer = live_furnace_window(s, w, this->x, this->y, this->z);
	/* The host's window is the authoritative owner in a death-drop journal
	 * (its spill is journaled). Any other viewer holds an alias of the TE,
	 * so the TE's own stacks are the ones that drop. Either way every
	 * remaining window is detached afterwards. */
	if(viewer && viewer->owner == &s->player) {
		(void)server_local_spill_active_container(s, this->x, this->y, this->z);
		furnace_te_free_w(w, this->x, this->y, this->z);
		furnace_detach_windows(s, w, this->x, this->y, this->z);
		return;
	}

	if(server_local_spawn_destroyed_container_items(
		   s, this->x, this->y, this->z, f->items, 3)) {
		furnace_te_free_w(w, this->x, this->y, this->z);
		furnace_detach_windows(s, w, this->x, this->y, this->z);
	}
	// Failure deliberately retains the TE for the server retry pass.
}

struct block block_furnaceoff = {
	.name = "Furnace",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex1,
	.getDroppedItem = getDroppedItem,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.transparent = false,
	.renderBlock = render_block_furnace,
	.renderBlockAlways = NULL,
	.luminance = 0, //depends on metadata
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta hardness x1500: Block.java:653 stoneOvenIdle `.setHardness(3.5F)`
	// -> 5250 (already correct). `.best = TOOL_TIER_ANY` (not WOOD):
	// ItemPickaxe.blocksEffectiveAgainst omits Block.stoneOvenIdle entirely,
	// so getStrVsBlock is flat 1.0F for any pickaxe tier -- no per-tier bonus
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
		.render_data.block.default_metadata = 0,
		.render_data.block.default_rotation = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_furnaceon = {
	.name = "Furnace",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex2,
	.getDroppedItem = getDroppedItem,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 13,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta hardness x1500: Block.java:654 stoneOvenActive `.setHardness(3.5F)`
	// -> 5250 (already correct). `.best = TOOL_TIER_ANY` (not WOOD): same
	// omission from ItemPickaxe.blocksEffectiveAgainst as the idle furnace
	// above -- flat 1.0F strVsBlock for any pickaxe tier.
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
