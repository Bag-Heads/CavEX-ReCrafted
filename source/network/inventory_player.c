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

#include "../item/inventory_click.h"
#include "../item/items.h" // direct include (was transitive via inventory.h pre-split)
#include "../item/recipe.h"
#include "../item/window_container.h"
#include "inventory_logic.h"
#include "inventory_achievements.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

static bool inv_pre_action(struct inventory* inv, struct server_player* player,
	size_t slot, bool right, set_inv_slot_t changes) {
	return inventory_click_pre(inv, WINDOW_TYPE_INVENTORY, slot, right, changes, inv->user);
}

static void inv_post_action(struct inventory* inv, struct server_player* player,
	size_t slot, bool right, bool accepted, set_inv_slot_t changes) {
	inventory_click_post(inv, WINDOW_TYPE_INVENTORY, slot, changes);
}

static bool inv_on_close(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	struct item_data drops[INVENTORY_SIZE_CRAFTING + 1];
	struct death_drop_provenance provenance[INVENTORY_SIZE_CRAFTING + 1];
	size_t drop_count = 0;

	for(size_t k = INVENTORY_SLOT_CRAFTING;
		k < INVENTORY_SLOT_CRAFTING + INVENTORY_SIZE_CRAFTING; k++) {
		struct item_data item;
		if(inventory_get_slot(inv, k, &item)) {
			drops[drop_count] = item;
			provenance[drop_count++] = (struct death_drop_provenance) {
				.kind = DEATH_DROP_OWNER_PLAYER_SLOT,
				.window = WINDOWC_INVENTORY,
				.slot = (int16_t)k,
			};
		}
	}

	struct item_data picked_item;
	bool has_picked_item = inventory_get_picked_item(inv, &picked_item);
	if(has_picked_item) {
		drops[drop_count] = picked_item;
		provenance[drop_count++] = (struct death_drop_provenance) {
			.kind = DEATH_DROP_OWNER_PLAYER_CURSOR,
			.window = WINDOWC_INVENTORY,
			.slot = SPECIAL_SLOT_PICKED_ITEM,
		};
	}

	if(!server_local_spawn_sourced_item_batch_for(
		   (vec3) {player->x, player->y, player->z}, drops,
		   provenance, drop_count, true, false, s, player))
		return false;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	inventory_clear_slot(inv, INVENTORY_SLOT_OUTPUT);
	set_inv_slot_push(changes, INVENTORY_SLOT_OUTPUT);

	for(size_t k = INVENTORY_SLOT_CRAFTING;
		k < INVENTORY_SLOT_CRAFTING + INVENTORY_SIZE_CRAFTING; k++) {
		struct item_data item;
		inventory_get_slot(inv, k, &item);

		if(item.id != 0) {
			inventory_clear_slot(inv, k);
			set_inv_slot_push(changes, k);
		}
	}

	if(has_picked_item) {
		inventory_clear_picked_item(inv);
		set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
	}

	server_local_send_inv_changes(changes, inv, WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);
	inv->transaction_pending = false;
	return true;
}

static bool inv_on_collect(struct inventory* inv, struct server_player* player, struct item_data* item) {
	uint8_t priorities[INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN];

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		priorities[k] = k + INVENTORY_SLOT_HOTBAR;

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		priorities[k + INVENTORY_SIZE_HOTBAR] = k + INVENTORY_SLOT_MAIN;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	/* Beta RetroMCP EntityItem.java:110-119: save id/count before drain. */
	struct item_data before = *item;
	bool success
		= inventory_collect(inv, item, priorities,
							sizeof(priorities) / sizeof(*priorities), changes);
	server_local_send_inv_changes(changes, inv, WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);

	if(success) inventory_achievement_collected(inv, player, before);
	return success;
}

struct inventory_logic inventory_logic_player = {
	.on_result_taken = inventory_achievement_craft_taken,
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = NULL,
	.on_destroy = NULL,
	.on_close = inv_on_close,
};
