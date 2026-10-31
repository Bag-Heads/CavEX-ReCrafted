/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

/*
	Inventory logic for a Beta 1.7.3 double chest: two horizontally-adjacent
	wooden chests presented as a single 54-slot window. It reuses the existing
	54-slot iron-chest window/GUI on the client, but on the server the storage
	spans TWO chest tile entities instead of one:
	  - window storage slots  0..26  <-> primary half   (inv->x, inv->y, inv->z)
	  - window storage slots 27..53  <-> secondary half (inv->x2,inv->y2,inv->z2)
	The primary/secondary roles are assigned canonically by the caller
	(block_chest.c via chest_is_primary), so both chests map the same way no
	matter which half the player opened.

	Each chest keeps its own 27-slot backing storage in s->chest_items[], so
	breaking one half drops only that half's items and leaves the other a
	normal single chest — exactly Beta behaviour, handled entirely by the
	per-chest getDroppedItem path with no special-casing here.
*/

#include "../item/recipe.h"
#include "../item/window_container.h"
#include "inventory_logic.h"
#include "inventory_achievements.h"
#include "inventory_chest_shared.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

// Find the chest tile-entity index backing the block at (x,y,z), or -1.
/* A1b: the backing row is resolved in the window's OPENING world (its
 * binding), never by coordinates alone; a stale binding has no backing. */
static int find_chest(const struct inventory* inv, w_coord_t x, w_coord_t y,
					  w_coord_t z) {
	struct server_world* w = server_local_window_world(inv);
	struct te_store st = server_local_window_tile_store(inv);
	int i = w ? te_store_find(&st, TE_KIND_CHEST, x, y, z) : -1;
	if(i < 0)
		return -1;
	struct block_data block;
	return server_world_get_block(w, x, y, z, &block)
		&& block.type == BLOCK_CHEST ? i : -1;
}

static bool inv_pre_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
						   set_inv_slot_t changes) {
	/* C2 tiles: a window whose opening world instance is gone (bound, but
	 * the slot now holds another generation or none) has no backing tile --
	 * Beta's TileEntity.canInteractWith fails (getBlockTileEntity != this),
	 * so the Container is unusable: refuse the click, mutate nothing. */
	if(inv->user && inv->owner && inv->owner->window_world.generation != 0
	   && !server_local_window_world(inv))
		return false;
	return true;
}

static void inv_post_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
							bool accepted, set_inv_slot_t changes) {
	if(accepted) {
		/* Ordinary clicks previously relied on client prediction. Shared viewers
		 * need the actual source/cursor changes as well as Quick Move changes. */
		set_inv_slot_push(changes, slot);
		set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
		inventory_chest_publish(inv, changes);
	}
}

static bool inv_on_close(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	int a = (inv->orphaned_chest_halves & 1) ? -1 : find_chest(inv, inv->x, inv->y, inv->z);
	int b = (inv->orphaned_chest_halves & 2) ? -1 : find_chest(inv, inv->x2, inv->y2, inv->z2);
	struct item_data picked_item;
	bool has_picked_item = inventory_get_picked_item(inv, &picked_item);
	struct item_data drops[IRON_CHEST_SIZE_STORAGE + 1];
	struct death_drop_provenance provenance[IRON_CHEST_SIZE_STORAGE + 1];
	size_t drop_count = 0;
	for(size_t k = 0; k < IRON_CHEST_SIZE_STORAGE; k++) {
		bool owner_missing = (k < CHEST_SIZE_STORAGE) ? (a < 0) : (b < 0);
		if(owner_missing && inv->items[k + IRON_CHEST_SLOT_STORAGE].id != 0) {
			drops[drop_count] = inv->items[k + IRON_CHEST_SLOT_STORAGE];
			provenance[drop_count++] = (struct death_drop_provenance) {
				.kind = DEATH_DROP_OWNER_CONTAINER_STORAGE,
				.window = WINDOWC_IRON_CHEST,
				.slot = (int16_t)(k + IRON_CHEST_SLOT_STORAGE),
				.owner_slot = (int16_t)(k % CHEST_SIZE_STORAGE),
				.owner_x = k < CHEST_SIZE_STORAGE ? inv->x : inv->x2,
				.owner_y = k < CHEST_SIZE_STORAGE ? inv->y : inv->y2,
				.owner_z = k < CHEST_SIZE_STORAGE ? inv->z : inv->z2,
			};
		}
	}
	size_t storage_count = drop_count;
	if(has_picked_item) {
		drops[drop_count] = picked_item;
		provenance[drop_count++] = (struct death_drop_provenance) {
			.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
			.window = WINDOWC_IRON_CHEST,
			.slot = SPECIAL_SLOT_PICKED_ITEM,
			.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
		};
	}
	if(drop_count > 0) {
		bool spawned = server_local_spawn_sourced_item_batch_for(
			(vec3) {player->x, player->y, player->z}, drops,
			provenance, drop_count, true, storage_count > 0, s, player);
		if(!spawned)
			return false;
	}

	/* Only the view that actually spilled an orphan may retire its peers'
	 * copies. An empty non-owner can close first after a failed break batch. */
	for(size_t half = 0; half < 2; half++) {
		if((half == 0 ? a : b) >= 0) continue;
		for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++) {
			if(!inv->items[half * CHEST_SIZE_STORAGE + k].id) continue;
			inventory_chest_invalidate_peers(inv,
				half ? inv->x2 : inv->x, half ? inv->y2 : inv->y,
				half ? inv->z2 : inv->z);
			break;
		}
	}
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	// A surviving half receives its current 27 slots. A half removed while the
	// GUI was open has just had only its orphaned slots spawned above; the break
	// path skipped its stale backing row, so this cannot duplicate old contents.
	for(size_t k = 0; k < IRON_CHEST_SIZE_STORAGE; k++) {
		// route storage slot k: 0..26 -> primary, 27..53 -> secondary
		int which = (k < CHEST_SIZE_STORAGE) ? a : b;
		size_t local = (k < CHEST_SIZE_STORAGE) ? k : (k - CHEST_SIZE_STORAGE);

		if(which >= 0)
			s->chest_items[which][local]
				= inv->items[k + IRON_CHEST_SLOT_STORAGE];

		struct item_data item;
		inventory_get_slot(inv, k + IRON_CHEST_SLOT_STORAGE, &item);
		if(item.id != 0) {
			inventory_clear_slot(inv, k + IRON_CHEST_SLOT_STORAGE);
			set_inv_slot_push(changes, k + IRON_CHEST_SLOT_STORAGE);
		}
	}

	if(has_picked_item) {
		inventory_clear_picked_item(inv);
		set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
	}

	server_local_send_inv_changes(changes, inv, WINDOWC_IRON_CHEST);
	set_inv_slot_clear(changes);

	inventory_destroy_owned(inv, player);
	return true;
}

static bool inv_on_collect(struct inventory* inv, struct server_player* player, struct item_data* item) {
	uint8_t priorities[INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN];

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		priorities[k] = k + IRON_CHEST_SLOT_HOTBAR;

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		priorities[k + INVENTORY_SIZE_HOTBAR] = k + IRON_CHEST_SLOT_MAIN;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	/* Beta RetroMCP EntityItem.java:110-119: save id/count before drain. */
	struct item_data before = *item;
	bool success
		= inventory_collect(inv, item, priorities,
							sizeof(priorities) / sizeof(*priorities), changes);
	server_local_send_inv_changes(changes, inv, WINDOWC_IRON_CHEST);
	set_inv_slot_clear(changes);

	if(success) inventory_achievement_collected(inv, player, before);
	return success;
}

static bool inv_on_create(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	int a = find_chest(inv, inv->x, inv->y, inv->z);
	int b = find_chest(inv, inv->x2, inv->y2, inv->z2);
	if(a < 0 || b < 0)
		return false;

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		inv->items[k + IRON_CHEST_SLOT_HOTBAR]
			= player->inventory.items[k + INVENTORY_SLOT_HOTBAR];
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		inv->items[k + IRON_CHEST_SLOT_MAIN]
			= player->inventory.items[k + INVENTORY_SLOT_MAIN];
	}

	for(size_t k = 0; k < IRON_CHEST_SIZE_STORAGE; k++) {
		int which = (k < CHEST_SIZE_STORAGE) ? a : b;
		size_t local = (k < CHEST_SIZE_STORAGE) ? k : (k - CHEST_SIZE_STORAGE);

		inv->items[k + IRON_CHEST_SLOT_STORAGE]
			= s->chest_items[which][local];
	}
	return inventory_chest_register(inv);
}

static bool inv_on_destroy(struct inventory* inv, struct server_player* player) {
	inventory_chest_unregister(inv);
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		player->inventory.items[k + INVENTORY_SLOT_HOTBAR]
			= inv->items[k + IRON_CHEST_SLOT_HOTBAR];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_HOTBAR);
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		player->inventory.items[k + INVENTORY_SLOT_MAIN]
			= inv->items[k + IRON_CHEST_SLOT_MAIN];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_MAIN);
	}

	server_local_send_inv_changes(changes, &player->inventory,
								  WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);
	return true;
}

struct inventory_logic inventory_logic_double_chest = {
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = inv_on_create,
	.on_destroy = inv_on_destroy,
	.on_close = inv_on_close,
};
