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

#include <assert.h>

#include <string.h>

#include "inventory.h"
#include "items.h" // direct include (was transitive via inventory.h pre-split)

static void* (*s_inventory_alloc)(size_t) = malloc;

#ifdef CAVEWII_SERVER_LOCAL_TEST
void inventory_test_set_alloc_hook(void* (*alloc_fn)(size_t)) {
	s_inventory_alloc = alloc_fn ? alloc_fn : malloc;
}
#endif

static bool inventory_init_common(struct inventory* inv,
								  struct inventory_logic* logic, void* user,
								  struct server_player* player,
								  size_t capacity, w_coord_t x, w_coord_t y,
								  w_coord_t z, bool has_partner, w_coord_t x2,
								  w_coord_t y2, w_coord_t z2,
								  uint32_t entity_id, bool owned_by_heap) {
	assert(inv && capacity > 0);
	/* NULL player only for logic-less client mirrors (see inventory.h). */
	assert(player || !logic);

	inv->client_map_counter = NULL;
	memset(&inv->revision, 0, sizeof(inv->revision));
	inv->user = user;
	inv->reply = NULL;
	inv->reply_user = NULL;
	inv->shared_next = NULL;
	inv->owner = player;
	inv->orphaned_chest_halves = 0;
	inv->logic = logic;
	inv->owned_by_heap = owned_by_heap;
	inv->capacity = capacity;
	inv->items = s_inventory_alloc(sizeof(struct item_data) * capacity);

	if(!inv->items)
		return false;
	/* ZERO the slots: this is a plain malloc, and every reader treats the
	 * array as authoritative item state. Uninitialised slots are heap garbage
	 * that can read as REAL items -- caught 2026-08-04 when a fresh harness
	 * inventory's hotbar slot randomly decoded as a weapon (per-tool attack
	 * strength 5 instead of fist 1, heap-layout dependent). Any production
	 * path that fills fewer than all `capacity` slots has the same exposure.
	 * Same malloc-trap class as the minecart pending_throttle/roll_dist. */
	memset(inv->items, 0, sizeof(struct item_data) * capacity);

	inv->x = x;
	inv->y = y;
	inv->z = z;
	inv->x2 = x2;
	inv->y2 = y2;
	inv->z2 = z2;
	inv->has_partner = has_partner;
	inv->entity_id = entity_id;
	inv->invalidated = false;
	inv->transaction_pending = false;
	inv->transaction_action = 0;

	inventory_clear(inv);

	ilist_inventory_init_field(inv);

	// on_create runs here, so all positions (incl. the partner) AND entity_id
	// are already set -- a double-chest logic can populate both halves from
	// on_create, and an entity-keyed logic can look its owner up by id.
	if(inv->logic && inv->logic->on_create
	   && !inv->logic->on_create(inv, player)) {
		free(inv->items);
		inv->items = NULL;
		return false;
	}

	return true;
}

bool inventory_create(struct inventory* inv, struct inventory_logic* logic,
					  void* user, struct server_player* player,
					  size_t capacity, w_coord_t x, w_coord_t y, w_coord_t z) {
	return inventory_init_common(inv, logic, user, player, capacity, x, y, z,
								 false, 0, 0, 0, 0, true);
}

bool inventory_init(struct inventory* inv, struct inventory_logic* logic,
					void* user, struct server_player* player,
					size_t capacity, w_coord_t x, w_coord_t y, w_coord_t z) {
	return inventory_init_common(inv, logic, user, player, capacity, x, y, z,
								 false, 0, 0, 0, 0, false);
}

bool inventory_create_linked(struct inventory* inv, struct inventory_logic* logic,
							 void* user, struct server_player* player,
							 size_t capacity, w_coord_t x, w_coord_t y,
							 w_coord_t z, w_coord_t x2, w_coord_t y2, w_coord_t z2) {
	return inventory_init_common(inv, logic, user, player, capacity, x, y, z,
								 true, x2, y2, z2, 0, true);
}

bool inventory_create_entity(struct inventory* inv, struct inventory_logic* logic,
							 void* user, struct server_player* player,
							 size_t capacity, uint32_t entity_id) {
	assert(entity_id != 0);
	return inventory_init_common(inv, logic, user, player, capacity, 0, 0, 0,
								 false, 0, 0, 0, entity_id, true);
}

void inventory_copy(struct inventory* inv, struct inventory* from) {
	assert(inv && from);

	inv->client_map_counter = from->client_map_counter;
	inv->capacity = from->capacity;
	inv->picked_item = from->picked_item;
	inv->hotbar_slot = from->hotbar_slot;
	inv->invalidated = from->invalidated;

	for(size_t k = 0; k < inv->capacity; k++)
		inv->items[k] = from->items[k];
}

static bool inventory_destroy_common(struct inventory* inv,
								 struct server_player* player) {
	assert(inv && inv->items);
	/* NULL player is legal only when no callback can dispatch (the client
	 * window mirrors); a server-backed logic always names its actor. */
	assert(player || !inv->logic);

	bool free_mem = false;

	if(inv->logic && inv->logic->on_destroy)
		free_mem = inv->logic->on_destroy(inv, player);

	free(inv->items);
	inv->items = NULL;
	return free_mem;
}

void inventory_destroy(struct inventory* inv, struct server_player* player) {
	(void)inventory_destroy_common(inv, player);
}

bool inventory_owned_may_free(bool owned_by_heap, bool free_mem) {
	return owned_by_heap && free_mem;
}

void inventory_destroy_owned(struct inventory* inv,
							 struct server_player* player) {
	bool owned_by_heap = inv->owned_by_heap;
	bool free_mem = inventory_destroy_common(inv, player);
	assert(owned_by_heap);
	if(inventory_owned_may_free(owned_by_heap, free_mem))
		free(inv);
}

void inventory_clear(struct inventory* inv) {
	assert(inv);

	for(size_t k = 0; k < inv->capacity; k++) {
		inv->items[k].id = 0;
		inv->items[k].durability = 0;
		inv->items[k].count = 0;
		inv->items[k].nbt_record = 0;
	}

	inv->picked_item.id = 0;
	inv->picked_item.durability = 0;
	inv->picked_item.count = 0;
	inv->picked_item.nbt_record = 0;
	inv->hotbar_slot = 0;
}

void inventory_consume(struct inventory* inv, size_t slot) {
	assert(inv && slot < inv->capacity);

	if(!item_data_pinned(&inv->items[slot]) && inv->items[slot].count > 0) {
		inv->items[slot].count--;

		if(inv->items[slot].count == 0) {
			inv->items[slot].id = 0;
			inv->items[slot].durability = 0;
			inv->items[slot].count = 0;
			inv->items[slot].nbt_record = 0;
		}
	}
}

size_t inventory_get_hotbar(struct inventory* inv) {
	assert(inv);
	return inv->hotbar_slot;
}

void inventory_set_hotbar(struct inventory* inv, size_t slot) {
	assert(inv && slot < INVENTORY_SIZE_HOTBAR);
	inv->hotbar_slot = slot;
}

bool inventory_get_hotbar_item(struct inventory* inv, struct item_data* item) {
	return inventory_get_slot(
		inv, inventory_get_hotbar(inv) + INVENTORY_SLOT_HOTBAR, item);
}

void inventory_set_slot(struct inventory* inv, size_t slot,
						struct item_data item) {
	assert(inv && slot < inv->capacity);
	inv->items[slot] = item;
}

void inventory_clear_slot(struct inventory* inv, size_t slot) {
	assert(inv && slot < inv->capacity);
	inv->items[slot].id = 0;
	inv->items[slot].durability = 0;
	inv->items[slot].count = 0;
	inv->items[slot].nbt_record = 0;
}

bool inventory_get_slot(struct inventory* inv, size_t slot,
						struct item_data* item) {
	assert(inv && slot < inv->capacity);
	if(item)
		*item = inv->items[slot];
	return inv->items[slot].id > 0;
}

void inventory_clear_picked_item(struct inventory* inv) {
	assert(inv);
	inv->picked_item.id = 0;
	inv->picked_item.durability = 0;
	inv->picked_item.count = 0;
	inv->picked_item.nbt_record = 0;
}

void inventory_set_picked_item(struct inventory* inv, struct item_data item) {
	assert(inv);
	inv->picked_item = item;
}

bool inventory_get_picked_item(struct inventory* inv, struct item_data* item) {
	assert(inv);

	if(inv->picked_item.id != 0) {
		if(item)
			*item = inv->picked_item;
		return true;
	} else {
		return false;
	}
}

// Drop the held cursor item (Beta Container.slotClick slotId == -999). right=false
// drops the WHOLE held stack; right=true drops ONE (splitStack(1), clearing the
// cursor when it reaches 0). Returns true and writes the dropped stack to *out if
// something was held; false (caller spawns nothing) on an empty cursor.
bool inventory_drop_picked(struct inventory* inv, bool right,
						   struct item_data* out) {
	assert(inv && out);

	struct item_data picked;
	if(!inventory_get_picked_item(inv, &picked))
		return false;

	if(right && picked.count > 1) {
		*out = picked;
		out->count = 1;
		picked.count--;
		inventory_set_picked_item(inv, picked);
	} else {
		// left: the whole stack; right with count 1: the last one. Either clears.
		*out = picked;
		inventory_clear_picked_item(inv);
	}
	return true;
}

static bool inventory_pick_item(struct inventory* inv, size_t slot) {
	if(inv->picked_item.id == 0 && inv->items[slot].id != 0) {
		inv->picked_item = inv->items[slot];
		inv->items[slot].id = 0;
		inv->items[slot].durability = 0;
		inv->items[slot].count = 0;
		inv->items[slot].nbt_record = 0;
		/* Beta RetroMCP SlotFurnace.java:15-25 / Container.java:110:
		 * empty-cursor takes land here rather than inventory_click_pre. */
		if(inv->logic && inv->logic->on_result_taken)
			inv->logic->on_result_taken(inv, slot, inv->picked_item);
		return true;
	} else {
		return false;
	}
}

static bool inventory_pick_item_split(struct inventory* inv, size_t slot) {
	if(inv->picked_item.id == 0 && inv->items[slot].id != 0) {
		if(inv->items[slot].count == 1)
			return inventory_pick_item(inv, slot);

		int total = inv->items[slot].count;

		inv->picked_item = inv->items[slot];
		inv->picked_item.count = total - total / 2;
		inv->items[slot].count = total / 2;
		/* Container.java:99-110: right button takes ceil(stackSize / 2). */
		if(inv->logic && inv->logic->on_result_taken)
			inv->logic->on_result_taken(inv, slot, inv->picked_item);
		return true;
	} else {
		return false;
	}
}

/* [740c583 Container.java:90-130]. Both buttons share the same merge/swap
 * predicate. In particular a full same-ID worn tool is a zero-count merge. */
static bool inventory_place_item(struct inventory* inv, size_t slot, bool right) {
	struct item_data* dest = &inv->items[slot];
	struct item_data* cursor = &inv->picked_item;
	struct item* def = item_get(cursor);
	if(dest->id == cursor->id && dest->nbt_record == cursor->nbt_record && def
	   && (!def->has_subtypes || dest->durability == cursor->durability)) {
		int take = right ? 1 : cursor->count;
		int room = def->max_stack - dest->count;
		if(take > room) take = room;
		/* Invalid imported over-limit stacks must not wrap a negative split. */
		if(take <= 0) return false;
		dest->count += take;
		cursor->count -= take;
		if(!cursor->count) inventory_clear_picked_item(inv);
	} else if(!dest->id && right) {
		*dest = *cursor;
		dest->count = 1;
		if(!--cursor->count) inventory_clear_picked_item(inv);
	} else {
		struct item_data previous = *dest;
		*dest = *cursor;
		*cursor = previous;
	}
	return true;
}

bool inventory_action(struct inventory* inv, struct server_player* player,
					  size_t slot, bool right, set_inv_slot_t changes) {
	assert(inv && slot < inv->capacity);
	assert(player || !inv->logic);

	bool result = !item_data_pinned(&inv->items[slot])
		&& !item_data_pinned(&inv->picked_item);

	if(inv->logic && inv->logic->pre_action
	   && !inv->logic->pre_action(inv, player, slot, right, changes))
		result = false;

	if(result) {
		if(right) {
			result = (inv->picked_item.id == 0) ?
				inventory_pick_item_split(inv, slot) :
				inventory_place_item(inv, slot, true);
		} else {
			result = (inv->picked_item.id == 0) ?
				inventory_pick_item(inv, slot) :
				inventory_place_item(inv, slot, false);
		}
	}

	if(inv->logic && inv->logic->post_action)
		inv->logic->post_action(inv, player, slot, right, result, changes);

	return result;
}
