/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <string.h>

#include "inventory_click.h"
#include "items.h"
#include "recipe.h"

/* [740c583 ItemMap.onCreated, MapStorage.getUniqueDataId]: onCreated also
 * runs on the CLIENT. Cache the allocation per action so correction replay
 * cannot spend another ID. Maintainer ruling: reserve zero, also on wrap. */
static bool craft_created(struct inventory* inv, struct item_data* output,
	struct server_local* server) {
	struct item* def = item_get(output);
	if(!def || !def->onCreated) return true;
	/* S1 (R-S1a1b F3): the hook takes the ACTING crafter. inv->owner is the
	 * server_player this inventory was created for (inventory.c's
	 * inventory_init_common), i.e. the host for the host's own inventory /
	 * windows and the guest for the guest's -- so a guest-crafted map centres
	 * on the guest. It is NULL only for the logic-less client mirrors and for
	 * harness inventories created with a NULL player; the hook decides what a
	 * missing body means (item_map falls back to the host). */
	if(server) return def->onCreated(server, inv->owner, output);
	/* No server: this is the CLIENT prediction / dry-run side. Beta's
	 * Item.onCreated returns VOID -- it cannot fail, and the craft always
	 * completes [740c583 Item.onCreated, dispatched from
	 * ItemStack.onCrafted -> SlotCrafting.onPickupFromSlot; cross-checked on
	 * this machine against ../McBetaCpp, whose decompile-grounded port declares
	 * `virtual void Item::onCreated(ItemStack*, World*, EntityPlayer*)`
	 * (src/net/minecraft/src/Item.h:51) and calls it for its return-less side
	 * effect from ItemStack.cpp:239]. The ONLY hook in this engine that can
	 * refuse is the map's, because its id comes from a durable on-disk counter
	 * (item_map.c) that a client has no business spending. So the map is the one
	 * id that needs the client-side counter dance below; every other onCreated
	 * has no client-visible effect at all and must predict SUCCESS. Returning
	 * false for them (as this did before) would make the client predict a failed
	 * craft the server happily performs -- a guaranteed desync the moment a
	 * second onCreated item is registered. */
	if(output->id != 358) return true;
	if(!inv->revision.created_map_id) {
		if(!inv->client_map_counter) return false;
		inv->revision.map_counter_before = *inv->client_map_counter;
		if(++*inv->client_map_counter == 0) ++*inv->client_map_counter;
		inv->revision.created_map_id = *inv->client_map_counter;
	}
	output->durability = inv->revision.created_map_id;
	return true;
}

static void changed(set_inv_slot_t changes, size_t slot) {
	if(changes) set_inv_slot_push(changes, slot);
}


void inventory_click_post(struct inventory* inv, enum window_type type,
	size_t slot, set_inv_slot_t changes) {
	size_t count = inventory_click_craft_size(type);
	if(!count || slot > count || inv->capacity <= count) return;
	struct item_data grid[9] = {{0}}, result;
	bool empty[9];
	memset(empty, true, sizeof(empty));
	for(size_t k = 0; k < count; k++) {
		size_t index = count == 4 ? k + k / 2 : k;
		empty[index] = !inventory_get_slot(inv, k + 1, &grid[index]);
	}
	/* Existing shaped/shapeless recipe sets and order, unchanged. */
	if(recipe_match(recipes_crafting, grid, empty, &result)
	   || recipe_match_shapeless(recipes_crafting_shapeless, grid, empty, &result))
		inv->items[0] = result;
	else inventory_clear_slot(inv, 0);
	changed(changes, 0);
}

static void craft_consume(struct inventory* inv, enum window_type type,
	set_inv_slot_t changes) {
	/* [740c583 SlotCrafting.java:36-43]: one ingredient per occupied cell;
	 * consumed container items replace that same crafting-grid cell. */
	for(size_t k = 1; k <= inventory_click_craft_size(type); k++) {
		struct item* def = item_get(&inv->items[k]);
		if(!inv->items[k].id) continue;
		inventory_consume(inv, k);
		if(def && def->container_item)
			inv->items[k] = (struct item_data){.id = def->container_item, .count = 1};
		changed(changes, k);
	}
	inventory_click_post(inv, type, 0, changes);
}

bool inventory_click_pre(struct inventory* inv, enum window_type type,
	size_t slot, bool right, set_inv_slot_t changes, struct server_local* server) {
	(void)right;
	struct item_data output = inv->items[slot];
	struct item_data cursor = inv->picked_item;
	struct item* def = item_get(&cursor);
	if(type == WINDOW_TYPE_INVENTORY && slot >= 5 && slot < 9 && cursor.id) {
		/* [740c583 SlotArmor.java:15-26, Container.java:90-130]: only
		 * matching armor or a pumpkin helmet, and slot limit one. */
		if(!((def && def->armor.is_armor
			&& def->armor.type == ARMOR_TYPE_HELMET + slot - 5)
			|| (slot == 5 && cursor.id == 86))) return false;
		if(!output.id) {
			inv->items[slot] = cursor;
			inv->items[slot].count = 1;
			if(!--inv->picked_item.count) inventory_clear_picked_item(inv);
			changed(changes, slot);
			changed(changes, SPECIAL_SLOT_PICKED_ITEM);
			return false;
		}
		if((output.id == cursor.id && def
			&& (!def->has_subtypes || output.durability == cursor.durability))
			|| cursor.count > 1) return false;
	}
	bool craft = inventory_click_is_craft_output(type, slot);
	bool furnace = type == WINDOW_TYPE_FURNACE && slot == FURNACE_SLOT_OUTPUT;
	if(!craft && !furnace) return true;
	if(!cursor.id && !craft) return true;
	/* Container.java:132-141: invalid destination can still be collected
	 * onto a compatible cursor, but only as a whole stack that fits. */
	if(!output.id || (cursor.id && (!def || def->max_stack <= 1
		|| cursor.id != output.id
		|| (def->has_subtypes && cursor.durability != output.durability)
		|| cursor.count + output.count > def->max_stack))) return false;
	if(craft && !craft_created(inv, &output, server)) return false;
	if(cursor.id) inv->picked_item.count += output.count;
	else inv->picked_item = output;
	inventory_clear_slot(inv, slot);
	changed(changes, slot);
	changed(changes, SPECIAL_SLOT_PICKED_ITEM);
	/* Beta RetroMCP SlotCrafting.java:17-35, Container.java:110,143.
	 * NULL server is prediction, even if a caller retained an owner. Furnace
	 * passes NULL here; its authoritative pre_action observes this take. */
	if(craft && server && inv->user == server && inv->logic
	   && inv->logic->on_result_taken)
		inv->logic->on_result_taken(inv, slot, output);
	if(craft) craft_consume(inv, type, changes);
	return false;
}

/* [740c583 Container.java:192-248]. Merge first, then place the entire
 * remainder into the first empty slot. Preserve the destination's metadata. */
static void mergeItemStack(struct inventory* inv, struct item_data* source,
	size_t begin, size_t end, bool reverse, set_inv_slot_t changes) {
	struct item* def = item_get(source);
	if(!def) return;
	bool stackable = def->max_stack > 1 && !(def->max_damage && source->durability);
	for(int pass = 0; pass < 2 && source->count; pass++) {
		if(pass == 0 && !stackable) continue;
		for(size_t n = 0; n < end - begin && source->count; n++) {
			size_t k = reverse ? end - 1 - n : begin + n;
			struct item_data* dest = &inv->items[k];
			if(pass == 0 && dest->id == source->id
			   && dest->nbt_record == source->nbt_record
			   && !item_data_pinned(dest)
			   && (!def->has_subtypes || dest->durability == source->durability)
			   && dest->count < def->max_stack) {
				unsigned take = def->max_stack - dest->count;
				if(take > source->count) take = source->count;
				dest->count += take;
				source->count -= take;
				changed(changes, k);
			} else if(pass == 1 && !dest->id) {
				*dest = *source;
				source->count = 0;
				changed(changes, k);
				break;
			}
		}
	}
}

struct item_data inventory_transfer(struct inventory* inv, enum window_type type,
	size_t slot, set_inv_slot_t changes, struct server_local* server) {
	struct item_data none = {0};
	if(!inventory_slot_valid(inv, slot) || inv->invalidated) return none;
	struct item_data original = inv->items[slot];
	if(!original.id || item_data_pinned(&original)) return none;
	/* ContainerDispenser does not override getStackInSlot (Container:42-44).
	 * Beta shift-click returns the stack unchanged and moves NOTHING. */
	if(type == WINDOW_TYPE_DISPENSER) return original;
	bool storage = type == WINDOW_TYPE_CHEST || type == WINDOW_TYPE_IRON_CHEST;
	size_t main = storage && inv->capacity >= 36 ? inv->capacity - 36
		: type == WINDOW_TYPE_INVENTORY ? INVENTORY_SLOT_MAIN
		: type == WINDOW_TYPE_WORKBENCH ? CRAFTING_SLOT_MAIN
		: type == WINDOW_TYPE_FURNACE ? FURNACE_SLOT_MAIN : 0;
	if(!main || inv->capacity != main + 36) return none;
	size_t begin, end;
	bool craft = inventory_click_is_craft_output(type, slot);
	bool reverse = false;
	if(storage) {
		begin = slot < main ? main : 0;
		end = slot < main ? inv->capacity : main;
		reverse = slot < main;
	} else if(slot < main) {
		begin = main; end = inv->capacity;
		reverse = craft || (type == WINDOW_TYPE_FURNACE && slot == 2);
	} else if(slot < main + 27) {
		begin = main + 27; end = inv->capacity;
	} else {
		begin = main; end = main + 27;
	}
	/* Container.java:67-77 recursively retries only when the resulting source
	 * stack count is smaller than the returned original. Iterate that exact
	 * condition (including repeated crafting), avoiding recursion on Wii. */
	struct item_data source = original;
	bool moved = false;
	for(;;) {
		struct item_data before = source;
		struct item* def = item_get(&source);
		if(craft && def && def->onCreated) {
			/* [740c583 SlotCrafting.onPickupFromSlot -> ItemMap.onCreated].
			 * Transfer the created identity (maintainer client+server ruling),
			 * reserving only when a destination exists. */
			bool room = false;
			for(size_t k = begin; k < end; k++)
				if(!inv->items[k].id) { room = true; break; }
			if(room) {
				if(!craft_created(inv, &source, server)) return moved ? original : none;
			}
		}
		mergeItemStack(inv, &source, begin, end, reverse, changes);
		if(source.count == before.count) return (storage || moved) ? original : none;
		moved = true;
		/* Beta RetroMCP ContainerWorkbench.java:86 / ContainerPlayer.java:91
		 * / ContainerFurnace.java:96: one callback per actual transfer, never
		 * on zero room or a prediction/scratch copy. Capture the moved count
		 * before craft_consume overwrites the result with the next recipe. */
		if(server && inv->user == server && inv->logic
		   && inv->logic->on_result_taken) {
			struct item_data taken = before;
			taken.count -= source.count;
			inv->logic->on_result_taken(inv, slot, taken);
		}
		inv->items[slot] = source.count ? source : none;
		changed(changes, slot);
		if(craft) {
			craft_consume(inv, type, changes);
		} else inventory_click_post(inv, type, slot, changes);
		source = inv->items[slot];
		if(!source.id || source.count >= before.count) break;
	}
	return original;
}

struct item_data inventory_click_predict(struct inventory* inv, enum window_type type,
	size_t slot, bool right, bool quick) {
	if(quick) return inventory_transfer(inv, type, slot, NULL, NULL);
	struct item_data expected = inv->items[slot];
	if(inventory_click_pre(inv, type, slot, right, NULL, NULL))
		inventory_action(inv, NULL, slot, right, NULL);
	inventory_click_post(inv, type, slot, NULL);
	return expected;
}
