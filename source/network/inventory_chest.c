/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include "../item/recipe.h"
#include "../item/window_container.h"
#include "inventory_logic.h"
#include "inventory_achievements.h"
#include "inventory_chest_shared.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

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
	int backing = (inv->orphaned_chest_halves & 1) ? -1 : find_chest(inv, inv->x, inv->y, inv->z);
	struct item_data picked_item;
	bool has_picked_item = inventory_get_picked_item(inv, &picked_item);
	if(backing < 0) {
		// The block can disappear under an open GUI (TNT/support updates). Its
		// break path deliberately leaves the live window as owner; atomically
		// spill those orphaned storage slots now instead of pinning the window or
		// silently destroying them.
		struct item_data drops[CHEST_SIZE_STORAGE + 1];
		struct death_drop_provenance provenance[CHEST_SIZE_STORAGE + 1];
		size_t drop_count = 0;
		for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++)
			if(inv->items[k + CHEST_SLOT_STORAGE].id != 0) {
				drops[drop_count] = inv->items[k + CHEST_SLOT_STORAGE];
				provenance[drop_count++] = (struct death_drop_provenance) {
					.kind = DEATH_DROP_OWNER_CONTAINER_STORAGE,
					.window = WINDOWC_CHEST,
					.slot = (int16_t)(k + CHEST_SLOT_STORAGE),
					.owner_slot = (int16_t)k,
					.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
				};
			}
		size_t storage_count = drop_count;
		if(has_picked_item) {
			drops[drop_count] = picked_item;
			provenance[drop_count++] = (struct death_drop_provenance) {
				.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
				.window = WINDOWC_CHEST,
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

		if(storage_count > 0)
			inventory_chest_invalidate_peers(inv, inv->x, inv->y, inv->z);
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
		server_local_send_inv_changes(orphan_changes, inv, WINDOWC_CHEST);
		set_inv_slot_clear(orphan_changes);
		inventory_destroy_owned(inv, player);
		return true;
	}
	if(has_picked_item) {
		struct death_drop_provenance source = {
			.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
			.window = WINDOWC_CHEST,
			.slot = SPECIAL_SLOT_PICKED_ITEM,
			.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
		};
		if(!server_local_spawn_sourced_item_batch_for(
			   (vec3) {player->x, player->y, player->z},
			   &picked_item, &source, 1, true, false, s, player))
			return false;
	}

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++) {
		s->chest_items[backing][k] = inv->items[k + CHEST_SLOT_STORAGE];

		struct item_data item;
		inventory_get_slot(inv, k + CHEST_SLOT_STORAGE, &item);
		if(item.id != 0) {
			inventory_clear_slot(inv, k + CHEST_SLOT_STORAGE);
			set_inv_slot_push(changes, k + CHEST_SLOT_STORAGE);
		}
	}

	if(has_picked_item) {
		inventory_clear_picked_item(inv);
		set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
	}

	server_local_send_inv_changes(changes, inv, WINDOWC_CHEST);
	set_inv_slot_clear(changes);

	inventory_destroy_owned(inv, player);
	return true;
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
	server_local_send_inv_changes(changes, inv, WINDOWC_CHEST);
	set_inv_slot_clear(changes);

	if(success) inventory_achievement_collected(inv, player, before);
	return success;
}

static bool inv_on_create(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	int backing = find_chest(inv, inv->x, inv->y, inv->z);
	if(backing < 0)
		return false;

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		inv->items[k + CHEST_SLOT_HOTBAR]
			= player->inventory.items[k + INVENTORY_SLOT_HOTBAR];
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		inv->items[k + CHEST_SLOT_MAIN]
			= player->inventory.items[k + INVENTORY_SLOT_MAIN];
	}

	for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++)
		inv->items[k + CHEST_SLOT_STORAGE] = s->chest_items[backing][k];
	return inventory_chest_register(inv);
}

/* SHARED with any other CHEST_SLOT_HOTBAR/_MAIN-shaped window (minecart-slice:
 * inventory_minecart_chest.c) -- purely player-side, no block/entity backing
 * store touched here, so it needed zero changes to serve a second owner
 * kind. Declared in inventory_logic.h. Extract-on-second-transcription
 * (CODESTYLE.md S3.2): this IS the second transcription. */
bool inventory_chest_player_side_on_destroy(struct inventory* inv,
											struct server_player* player) {
	inventory_chest_unregister(inv);
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		player->inventory.items[k + INVENTORY_SLOT_HOTBAR]
			= inv->items[k + CHEST_SLOT_HOTBAR];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_HOTBAR);
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		player->inventory.items[k + INVENTORY_SLOT_MAIN]
			= inv->items[k + CHEST_SLOT_MAIN];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_MAIN);
	}



	server_local_send_inv_changes(changes, &player->inventory,
								  WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);
	return true;
}

struct inventory_logic inventory_logic_chest = {
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = inv_on_create,
	.on_destroy = inventory_chest_player_side_on_destroy,
	.on_close = inv_on_close,
};
