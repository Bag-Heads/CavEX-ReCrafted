/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Storage minecart's (minecart_type 1) entity-backed chest window
 * [740c583: EntityMinecart.interact minecartType==1 -> displayGUIChest(this);
 * getSizeInventory() returns 27, same as TileEntityChest -- both cited before
 * this file was written]. Deliberately mirrors inventory_chest.c's shape --
 * same CHEST_SIZE/CHEST_SLOT_* layout (storage 0..26, player main 27..53,
 * hotbar 54..62), same on_collect priorities -- differing ONLY in how the
 * storage half is located: by entity id (inv->entity_id) instead of a block
 * position (inv->x/y/z), since a minecart's storage moves with the cart. The
 * player-side hotbar/main writeback on close is IDENTICAL either way and is
 * therefore the SHARED inventory_chest_player_side_on_destroy
 * (inventory_chest.c) rather than a second copy.
 */

#include "../entity/entity.h"
#include "../entity/entity_minecart.h"
#include "../item/window_container.h"
#include "../network/inventory_logic.h"
#include "inventory_achievements.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)

static bool inv_pre_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
						   set_inv_slot_t changes) {
	return true;
}

static void inv_post_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
							bool accepted, set_inv_slot_t changes) {
}

static bool inv_on_collect(struct inventory* inv, struct server_player* player, struct item_data* item) {
	uint8_t priorities[INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN];

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		priorities[k] = k + CHEST_SLOT_HOTBAR;

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		priorities[k + INVENTORY_SIZE_HOTBAR] = k + CHEST_SLOT_MAIN;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	/* Beta RetroMCP EntityItem.java:110-119: save id/count before drain. */
	struct item_data before = *item;
	bool success
		= inventory_collect(inv, item, priorities,
							sizeof(priorities) / sizeof(*priorities), changes);
	server_local_send_inv_changes(changes, inv, WINDOWC_MINECART_CHEST);
	set_inv_slot_clear(changes);

	if(success) inventory_achievement_collected(inv, player, before);
	return success;
}

/* Find the live entity backing this window. Requires BOTH that it is still a
 * minecart AND still minecart_type 1: the type is immutable after spawn (no
 * live path can actually change it under an open window today), so the
 * second half is defence-in-depth matching this codebase's precondition-assert
 * density elsewhere -- a soft reject here, not an assert, to match
 * inv_on_create/inv_on_close's own existing soft-fail convention for a
 * missing backing store (inventory_chest.c's find_chest returning -1). */
static struct entity* find_cart(struct server_local* s, uint32_t entity_id) {
	struct entity** pp = dict_entity_get(s->entities, entity_id);
	struct entity* e = pp ? *pp : NULL;
	if(!e || e->type != ENTITY_MINECART || e->data.minecart.minecart_type != 1)
		return NULL;
	return e;
}

static bool inv_on_create(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	struct entity* cart = find_cart(s, inv->entity_id);
	if(!cart)
		return false;

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		inv->items[k + CHEST_SLOT_HOTBAR]
			= player->inventory.items[k + INVENTORY_SLOT_HOTBAR];

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		inv->items[k + CHEST_SLOT_MAIN]
			= player->inventory.items[k + INVENTORY_SLOT_MAIN];

	for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++)
		inv->items[k + CHEST_SLOT_STORAGE] = cart->data.minecart.inventory[k];
	return true;
}

static bool inv_on_close(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	struct entity* cart = find_cart(s, inv->entity_id);
	struct item_data picked_item;
	bool has_picked_item = inventory_get_picked_item(inv, &picked_item);

	if(!cart) {
		/* The backing entity vanished while the GUI was open. Currently
		 * UNREACHABLE in this engine (MINECART_SLICE_NOTES.md): nothing can
		 * destroy a minecart while ANY window is open -- SRPC_ENTITY_ATTACK
		 * itself refuses whenever active_inventory != the player's own
		 * inventory, and void-kill (VOID_KILL_Y) only ever applies to the
		 * player, never to arbitrary entities. Kept as a real, tested
		 * fallback rather than an unreachable assert, so a FUTURE destroy
		 * path (TNT, lava, ...) fails safe: spill storage + cursor near the
		 * PLAYER instead of silently discarding them, the same shape as
		 * block_chest.c's "block disappeared under an open GUI" handling
		 * (server_local_spill_active_container). Uses the plain (non-
		 * death-drop-journal) spawn helpers -- see on_close's own note below
		 * on why that scope boundary is deliberate. */
		struct item_data drops[CHEST_SIZE_STORAGE + 1];
		size_t drop_count = 0;
		for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++)
			if(inv->items[k + CHEST_SLOT_STORAGE].id != 0)
				drops[drop_count++] = inv->items[k + CHEST_SLOT_STORAGE];
		if(has_picked_item)
			drops[drop_count++] = picked_item;
		if(drop_count > 0
		   && !server_local_spawn_container_item_batch(
			   (vec3) {player->x, player->y, player->z}, drops,
			   drop_count, false, s))
			return false;

		set_inv_slot_t orphan_changes;
		set_inv_slot_init(orphan_changes);
		for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++)
			if(inv->items[k + CHEST_SLOT_STORAGE].id != 0) {
				inventory_clear_slot(inv, k + CHEST_SLOT_STORAGE);
				set_inv_slot_push(orphan_changes, k + CHEST_SLOT_STORAGE);
			}
		if(has_picked_item) {
			inventory_clear_picked_item(inv);
			set_inv_slot_push(orphan_changes, SPECIAL_SLOT_PICKED_ITEM);
		}
		server_local_send_inv_changes(orphan_changes, inv,
									  WINDOWC_MINECART_CHEST);
		set_inv_slot_clear(orphan_changes);
		inventory_destroy_owned(inv, player);
		return true;
	}

	/* Journal-aware cursor transfer, mirroring inventory_chest.c's inv_on_close
	 * exactly: server_local_spawn_sourced_item_batch degrades to the plain
	 * spawn on an ordinary (non-death) close (s->death_drop_active false) and
	 * validates+transfers from the journal on a Nether death (true). The
	 * provenance MUST match what server_local_plan_death_drops planned for
	 * this same cursor -- window WINDOWC_MINECART_CHEST, owner_x/y/z left at
	 * inv->x/y/z (0/0/0, unused for an entity-keyed inventory either side) --
	 * or a real journaled death would fail this transfer instead of
	 * completing it. Without this, EVERY on_close call during an active
	 * journal (i.e. every ordinary Nether death with a picked-up item and
	 * this GUI open) hit server_local_spawn_item_batch's own
	 * "s->death_drop_active -> reject" guard and returned false here,
	 * forcing the death down the disruptive world-reload recovery fallback
	 * instead of a clean close -- the dormant twin M1's softlock fix would
	 * otherwise have exposed. Storage itself (below) needs no such handling:
	 * it is a plain memory copy into the entity, never a spawn call. */
	if(has_picked_item) {
		struct death_drop_provenance source = {
			.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
			.window = WINDOWC_MINECART_CHEST,
			.slot = SPECIAL_SLOT_PICKED_ITEM,
			.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
		};
		if(!server_local_spawn_sourced_item_batch_for(
			   (vec3) {player->x, player->y, player->z}, &picked_item,
			   &source, 1, true, false, s, player))
			return false;
	}

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++) {
		cart->data.minecart.inventory[k] = inv->items[k + CHEST_SLOT_STORAGE];

		if(inv->items[k + CHEST_SLOT_STORAGE].id != 0) {
			inventory_clear_slot(inv, k + CHEST_SLOT_STORAGE);
			set_inv_slot_push(changes, k + CHEST_SLOT_STORAGE);
		}
	}

	if(has_picked_item) {
		inventory_clear_picked_item(inv);
		set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
	}

	server_local_send_inv_changes(changes, inv, WINDOWC_MINECART_CHEST);
	set_inv_slot_clear(changes);

	inventory_destroy_owned(inv, player);
	return true;
}

struct inventory_logic inventory_logic_minecart_chest = {
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = inv_on_create,
	.on_destroy = inventory_chest_player_side_on_destroy,
	.on_close = inv_on_close,
};
