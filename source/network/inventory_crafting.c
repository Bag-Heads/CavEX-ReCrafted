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

#include <string.h>

#include "../item/inventory_click.h"
#include "../item/items.h" // direct include (was transitive via inventory.h pre-split)
#include "../item/recipe.h"
#include "../item/window_container.h"
#include "inventory_crafting.h"
#include "inventory_logic.h"
#include "inventory_achievements.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

static bool inv_pre_action(struct inventory* inv, struct server_player* player,
	size_t slot, bool right, set_inv_slot_t changes) {
	return inventory_click_pre(inv, WINDOW_TYPE_WORKBENCH, slot, right, changes, inv->user);
}

static void inv_post_action(struct inventory* inv, struct server_player* player,
	size_t slot, bool right, bool accepted, set_inv_slot_t changes) {
	inventory_click_post(inv, WINDOW_TYPE_WORKBENCH, slot, changes);
}

static bool inv_on_close(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	struct item_data drops[CRAFTING_SIZE_INPUT + 1];
	struct death_drop_provenance provenance[CRAFTING_SIZE_INPUT + 1];
	size_t drop_count = 0;

	for(size_t k = CRAFTING_SLOT_INPUT;
		k < CRAFTING_SLOT_INPUT + CRAFTING_SIZE_INPUT; k++) {
		struct item_data item;
		if(inventory_get_slot(inv, k, &item)) {
			drops[drop_count] = item;
			provenance[drop_count++] = (struct death_drop_provenance) {
				.kind = DEATH_DROP_OWNER_ACTIVE_CRAFT_SLOT,
				.window = WINDOWC_CRAFTING,
				.slot = (int16_t)k,
				.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
			};
		}
	}

	struct item_data picked_item;
	bool has_picked_item = inventory_get_picked_item(inv, &picked_item);
	if(has_picked_item) {
		drops[drop_count] = picked_item;
		provenance[drop_count++] = (struct death_drop_provenance) {
			.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
			.window = WINDOWC_CRAFTING,
			.slot = SPECIAL_SLOT_PICKED_ITEM,
			.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
		};
	}

	if(!server_local_spawn_sourced_item_batch_for(
		   (vec3) {player->x, player->y, player->z}, drops,
		   provenance, drop_count, true, false, s, player))
		return false;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	inventory_clear_slot(inv, CRAFTING_SLOT_OUTPUT);
	set_inv_slot_push(changes, CRAFTING_SLOT_OUTPUT);

	for(size_t k = CRAFTING_SLOT_INPUT;
		k < CRAFTING_SLOT_INPUT + CRAFTING_SIZE_INPUT; k++) {
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

	server_local_send_inv_changes(changes, inv, WINDOWC_CRAFTING);
	set_inv_slot_clear(changes);

	inventory_destroy_owned(inv, player);
	return true;
}

static bool inv_on_collect(struct inventory* inv, struct server_player* player, struct item_data* item) {
	uint8_t priorities[INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN];

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		priorities[k] = k + CRAFTING_SLOT_HOTBAR;

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		priorities[k + INVENTORY_SIZE_HOTBAR] = k + CRAFTING_SLOT_MAIN;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	/* Beta RetroMCP EntityItem.java:110-119: save id/count before drain. */
	struct item_data before = *item;
	bool success
		= inventory_collect(inv, item, priorities,
							sizeof(priorities) / sizeof(*priorities), changes);
	server_local_send_inv_changes(changes, inv, WINDOWC_CRAFTING);
	set_inv_slot_clear(changes);

	if(success) inventory_achievement_collected(inv, player, before);
	return success;
}

static bool inv_on_create(struct inventory* inv, struct server_player* player) {
	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		inv->items[k + CRAFTING_SLOT_HOTBAR]
			= player->inventory.items[k + INVENTORY_SLOT_HOTBAR];
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		inv->items[k + CRAFTING_SLOT_MAIN]
			= player->inventory.items[k + INVENTORY_SLOT_MAIN];
	}
	return true;
}

static bool inv_on_destroy(struct inventory* inv, struct server_player* player) {
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		player->inventory.items[k + INVENTORY_SLOT_HOTBAR]
			= inv->items[k + CRAFTING_SLOT_HOTBAR];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_HOTBAR);
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		player->inventory.items[k + INVENTORY_SLOT_MAIN]
			= inv->items[k + CRAFTING_SLOT_MAIN];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_MAIN);
	}

	server_local_send_inv_changes(changes, &player->inventory,
								  WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);
	return true;
}

struct inventory_logic inventory_logic_crafting = {
	.on_result_taken = inventory_achievement_craft_taken,
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = inv_on_create,
	.on_destroy = inv_on_destroy,
	.on_close = inv_on_close,
};

/* ===========================================================================
 * Server-authoritative one-batch selected-recipe transaction (lane/craft-rpc).
 *
 * The guided recipe guide must never be able to produce a result the MANUAL
 * grid could not, so this does not re-implement crafting: it lays the selected
 * registration out in a real crafting window exactly as a player would, then
 * drives the SAME pinned manual code -- inventory_click_post (the recipe
 * matcher) and inventory_transfer (Container.transferStackInSlot +
 * SlotCrafting.onPickupFromSlot, including container items and Item.onCreated)
 * -- and finally quick-moves the returned containers out of the grid, which is
 * the last click of the manual sequence. Every Beta rule therefore lives in one
 * place (source/item/inventory_click.c, already cited to [740c583]) and this
 * module only sequences it and makes it atomic.
 * ======================================================================== */

/* 3x3 logical matcher cell -> window slot, for the 2x2 ContainerPlayer grid.
 * Beta's player grid is a 2x2 InventoryCrafting whose slots 0..3 are added as
 * container slots 1..4 [740c583 ContainerPlayer ctor], while recipe.c's matcher
 * (like Beta's ShapedRecipes.matches) always indexes a 3x3 space as x + y * 3 --
 * so only logical cells 0,1,3,4 exist in the small grid. This is literally the
 * mapping the runnable oracle carries for the same window: ../mc173
 * mc173-server/src/player.rs `mapping: Some(&[1, 2, -1, 3, 4, -1, -1, -1, -1])`. */
static const int8_t recipe_craft_slot_2x2[9]
	= {1, 2, -1, 3, 4, -1, -1, -1, -1};

/* -1 when the logical cell has no slot in this window's grid. */
static int recipe_craft_grid_slot(enum window_type type, size_t logical) {
	if(logical >= 9) return -1;
	if(type == WINDOW_TYPE_WORKBENCH)
		return (int)(CRAFTING_SLOT_INPUT + logical);
	/* INVENTORY_SLOT_CRAFTING == 1 == the table's first entry. */
	return recipe_craft_slot_2x2[logical];
}

static size_t recipe_craft_grid_side(enum window_type type) {
	return type == WINDOW_TYPE_WORKBENCH ? 3 : 2;
}

size_t inventory_crafting_recipe_count(void) {
	return array_recipe_size(recipes_crafting)
		+ array_recipe_shapeless_size(recipes_crafting_shapeless);
}

/* The canonical grid arrangement for one registered recipe, as the guided path
 * will lay it out. Shaped recipes go in unmirrored at the top-left corner (the
 * arrangement the registration itself describes); shapeless recipes -- which
 * Beta matches by bag, not by position [740c583 ShapelessRecipes.matches;
 * ../mc173 mc173/src/item/craft.rs ShapelessRecipe::check] -- fill the window's
 * real cells in order, skipping the 3x3 columns the 2x2 grid does not have. */
struct recipe_craft_plan {
	struct item_data cells[9];
	bool exact[9];
	struct item_data result;
};

static enum recipe_craft_reason recipe_craft_plan_build(uint16_t recipe_index,
	enum window_type type, struct recipe_craft_plan* plan) {
	memset(plan, 0, sizeof(*plan));
	size_t shaped = array_recipe_size(recipes_crafting);
	size_t side = recipe_craft_grid_side(type);

	if((size_t)recipe_index < shaped) {
		const struct recipe* r
			= array_recipe_cget(recipes_crafting, (size_t)recipe_index);
		if(r->width > side || r->height > side)
			return RECIPE_CRAFT_RECIPE_TOO_LARGE;
		plan->result = r->result;
		for(size_t y = 0; y < r->height; y++)
			for(size_t x = 0; x < r->width; x++) {
				size_t src = x + y * r->width, dst = x + y * 3;
				plan->cells[dst] = r->shape[src].item;
				plan->exact[dst] = r->shape[src].match_durability;
			}
		return RECIPE_CRAFT_OK;
	}

	size_t n = (size_t)recipe_index - shaped;
	if(n >= array_recipe_shapeless_size(recipes_crafting_shapeless))
		return RECIPE_CRAFT_UNKNOWN_RECIPE;
	const struct recipe_shapeless* r
		= array_recipe_shapeless_cget(recipes_crafting_shapeless, n);
	if(r->count > side * side) return RECIPE_CRAFT_RECIPE_TOO_LARGE;
	plan->result = r->result;
	size_t placed = 0;
	for(size_t logical = 0; logical < 9 && placed < r->count; logical++) {
		if(recipe_craft_grid_slot(type, logical) < 0) continue;
		plan->cells[logical] = r->ingredients[placed].item;
		plan->exact[logical] = r->ingredients[placed].match_durability;
		placed++;
	}
	return placed == r->count ? RECIPE_CRAFT_OK : RECIPE_CRAFT_RECIPE_TOO_LARGE;
}

/* Requirement 4 -- the deterministic ingredient pick order.
 *
 * The manual grid has no Beta-defined order at all: the player drags whichever
 * stack they like into the grid by hand, so there is nothing to copy verbatim.
 * The guided path therefore adopts Beta's OWN player-inventory scan, which is
 * the order every automatic InventoryPlayer consumer already uses --
 * getInventorySlotContainItem / consumeInventoryItem / storeItemStack walk
 * `mainInventory[]` ASCENDING, and mainInventory[0..8] IS the hotbar (9..35 is
 * the upper inventory). Confirmed against the runnable oracle: ../mc173
 * mc173/src/item/inv.rs `InventoryHandle::consume` is a plain ascending scan of
 * its slice, and mc173-server/src/player.rs holds exactly one 36-slot
 * `main_inv` whose `0..9` half is the hotbar (see its shift-click, which sends
 * a stack to `if main_index < 9 { 9..36 } else { 0..9 }`).
 *
 * In this engine's window layout that ascending Beta order is: the window's
 * nine hotbar slots first, then its twenty-seven main slots -- both ascending.
 * Exact-metadata requirements are satisfied BEFORE wildcard ones (the two-pass
 * caller below) so a wildcard cannot spend the only metadata-specific candidate
 * a later exact cell needs; that is the same reservation the client-side
 * availability readout performs (recipe_browser_stock), so what the guide shows
 * as craftable is what the server will actually find.
 *
 * Pinned stacks (nbt_record == 255, inventory.c's saved-scope pin) are never
 * eligible, matching inventory_consume's own refusal to decrement one. */
static bool recipe_craft_take(struct inventory* inv, size_t main, size_t hotbar,
	struct item_data ingredient, bool exact, struct item_data* selected) {
	const size_t begin[2] = {hotbar, main};
	const size_t count[2] = {INVENTORY_SIZE_HOTBAR, INVENTORY_SIZE_MAIN};
	for(size_t range = 0; range < 2; range++)
		for(size_t k = 0; k < count[range]; k++) {
			struct item_data* item = &inv->items[begin[range] + k];
			if(!item->id || item->id != ingredient.id || !item->count
			   || (exact && item->durability != ingredient.durability)
			   || item_data_pinned(item))
				continue;
			*selected = *item;
			selected->count = 1;
			inventory_consume(inv, begin[range] + k);
			return true;
		}
	return false;
}

/* Can this one stack be merged into [begin, end) IN FULL?
 *
 * This is Beta's own pre-craft gate, and the runnable oracle states it
 * literally: ../mc173 mc173/src/item/inv.rs `InventoryHandle::can_push`, which
 * mc173-server/src/player.rs wraps the entire craft-result branch in
 * (`if main_inv.can_push(result_stack)` ... only then `craft_tracker.consume`).
 * Beta reaches the same place from the other side -- Container.transferStackInSlot
 * returns null the moment mergeItemStack moved nothing, before
 * SlotCrafting.onPickupFromSlot consumes anything [740c583 Container.java:192-248].
 *
 * The rules mirror inventory_click.c's mergeItemStack exactly: a stack merges
 * onto a same-id/same-nbt slot (same metadata too when the item has subtypes,
 * and never when it carries damage) up to max_stack, and the FIRST empty slot
 * absorbs the entire remainder in one go. */
static bool recipe_craft_can_push(const struct inventory* inv, size_t begin,
	size_t end, struct item_data stack) {
	struct item* def = item_get(&stack);
	if(!def) return false;
	bool stackable = def->max_stack > 1 && !(def->max_damage && stack.durability);
	unsigned remaining = stack.count;
	for(size_t k = begin; k < end && remaining; k++) {
		const struct item_data* slot = &inv->items[k];
		if(!slot->id) return true;
		if(!stackable || item_data_pinned(slot) || slot->id != stack.id
		   || slot->nbt_record != stack.nbt_record
		   || (def->has_subtypes && slot->durability != stack.durability)
		   || slot->count >= def->max_stack)
			continue;
		unsigned room = (unsigned)def->max_stack - slot->count;
		remaining -= room < remaining ? room : remaining;
	}
	return remaining == 0;
}

/* Total count of `id` held in [begin, end). Counts by ID alone on purpose: the
 * one-batch gate below uses it on the player section, where the result stack is
 * the only thing that just arrived (returned containers are still sitting in the
 * grid at that point), and an Item.onCreated that rewrites the stack's DAMAGE (a
 * map's id) must not make the delta read as zero. */
static unsigned recipe_craft_count_id(const struct inventory* inv, size_t begin,
	size_t end, uint16_t id) {
	unsigned n = 0;
	for(size_t k = begin; k < end; k++)
		if(inv->items[k].id == id) n += inv->items[k].count;
	return n;
}

/* Run the whole manual sequence on `inv`, which the caller has already proven
 * is a private scratch copy. Returns OK only when the batch completed with the
 * output slot and the entire grid emptied back into the player section. */
static enum recipe_craft_reason recipe_craft_run(struct inventory* inv,
	enum window_type type, const struct recipe_craft_plan* plan,
	struct server_local* server) {
	size_t main = type == WINDOW_TYPE_WORKBENCH ? CRAFTING_SLOT_MAIN
												: INVENTORY_SLOT_MAIN;
	size_t hotbar = type == WINDOW_TYPE_WORKBENCH ? CRAFTING_SLOT_HOTBAR
												  : INVENTORY_SLOT_HOTBAR;

	/* Lay the pattern out, exact requirements before wildcards. */
	for(unsigned pass = 0; pass < 2; pass++)
		for(size_t logical = 0; logical < 9; logical++) {
			int slot = recipe_craft_grid_slot(type, logical);
			if(!plan->cells[logical].id || slot < 0
			   || plan->exact[logical] != (pass == 0))
				continue;
			struct item_data selected;
			if(!recipe_craft_take(inv, main, hotbar, plan->cells[logical],
				   plan->exact[logical], &selected))
				return RECIPE_CRAFT_MISSING_INGREDIENTS;
			inv->items[slot] = selected;
		}

	/* The real matcher. CraftingManager resolves a GRID, not an index, and
	 * returns the FIRST registration that matches [740c583
	 * CraftingManager.findMatchingRecipe; recipe.c recipe_match]. If some
	 * earlier registration claims this arrangement, the manual grid would give
	 * the player that other result -- so the guided request is refused rather
	 * than silently handing out something the grid does not actually make. */
	inventory_click_post(inv, type, 1, NULL);
	if(memcmp(&inv->items[0], &plan->result, sizeof(plan->result)) != 0)
		return RECIPE_CRAFT_UNKNOWN_RECIPE;

	/* One shift-click on the output: Container.transferStackInSlot's craft
	 * branch -- Item.onCreated first (so a map's id comes from the server's
	 * durable counter, never the client), then mergeItemStack into the player
	 * section, then SlotCrafting.onPickupFromSlot consumes one item per
	 * occupied cell and leaves the container item (bucket, bowl) behind in that
	 * same cell [740c583 SlotCrafting.java:36-43, Container.java:192-248 --
	 * both already pinned in inventory_click.c; note the ../mc173 oracle does
	 * NOT model container items at all (craft.rs CraftTracker::consume merely
	 * decrements), so the decompile is the authority here per CLAUDE.md's
	 * decompile > mc173 order]. */
	if(!recipe_craft_can_push(inv, main, inv->capacity, inv->items[0]))
		return RECIPE_CRAFT_NO_CAPACITY;
	unsigned result_before
		= recipe_craft_count_id(inv, main, inv->capacity, plan->result.id);
	(void)inventory_transfer(inv, type, 0, NULL, server);
	/* Past that gate the result WILL merge, so an output still sitting in slot
	 * 0 can only mean Item.onCreated refused -- item_map_on_created returning
	 * false because idcounts.dat could not be durably written. Beta has no such
	 * failure (its counter is in-memory), and this engine deliberately fails the
	 * craft rather than hand out a map id the on-disk counter never reserved
	 * (see item_map.c's own note). Reported distinctly so the guide can say so.
	 *
	 * An occupied output slot has exactly one OTHER cause, and the two are told
	 * apart by what is sitting there (hostile review, finding 1): when
	 * craft_created refuses, inventory_transfer returns before touching
	 * anything, so slot 0 still holds this recipe's own result byte-for-byte;
	 * when the transfer's loop instead re-armed the matcher off the returned
	 * containers and stopped (the new output was not SMALLER, so
	 * Container.java:67-77 breaks rather than recursing), slot 0 holds a
	 * DIFFERENT stack -- a second batch the guided path never promised. Both are
	 * refusals, but only the first is a map-id failure, and mislabelling the
	 * second would send the guide a message about idcounts.dat that has nothing
	 * to do with what happened. (A second batch producing a byte-identical stack
	 * is indistinguishable here and reports CREATE_FAILED; it is still refused,
	 * and the one-batch gate below catches every case that gets past this.) */
	if(inv->items[0].id)
		return memcmp(&inv->items[0], &plan->result, sizeof(plan->result)) == 0
			? RECIPE_CRAFT_CREATE_FAILED : RECIPE_CRAFT_NOT_ONE_BATCH;

	/* ONE BATCH MEANS ONE BATCH -- the invariant this whole module rests on,
	 * enforced instead of assumed (hostile review, finding 1).
	 *
	 * inventory_transfer's craft branch is Beta's own recursion
	 * [740c583 Container.java:67-77]: after SlotCrafting.onPickupFromSlot has
	 * consumed the grid it re-runs the matcher and loops again while the new
	 * output stack is SMALLER than the one just taken. That is correct for a
	 * player holding shift -- and it is exactly what this guided path must not
	 * do, because it promised the client one batch of one named recipe.
	 *
	 * Today the loop cannot fire from here: recipe_craft_take pins every grid
	 * cell to count 1, so the only thing craft_consume can leave behind is a
	 * container item, and ITEM_BUCKET -- the only container_item Beta 1.7.3
	 * defines [740c583 Item.java:344/345/353, setContainerItem(bucketEmpty)] --
	 * has no bucket-only recipe. That is a property of the CURRENT registry, not
	 * of this code: one future container_item whose returned containers form a
	 * registered pattern with a smaller result would silently craft twice, spend
	 * the containers the player was owed, and blow the capacity preflight that
	 * only ever priced ONE batch. So state the postcondition and fail closed on
	 * it rather than rely on the registry staying lucky:
	 *
	 *   - the player section gained EXACTLY plan->result.count of the result, and
	 *   - every grid cell holds exactly what SlotCrafting owed it -- one
	 *     container item, or nothing.
	 *
	 * A rejection here (never expected in a correct build) costs nothing: this
	 * runs on the dry copy first, so the caller returns before any mutation. It
	 * is a reason code and NOT an assert on purpose -- the shipping .dol builds
	 * with -DNDEBUG, where an assert is a no-op and the invariant would go
	 * unenforced exactly where it matters (CLAUDE.md's assert-under-NDEBUG
	 * trap); tests get the same code path with the same verdict. */
	unsigned result_after
		= recipe_craft_count_id(inv, main, inv->capacity, plan->result.id);
	if(result_after < result_before
	   || result_after - result_before != (unsigned)plan->result.count)
		return RECIPE_CRAFT_NOT_ONE_BATCH;
	for(size_t logical = 0; logical < 9; logical++) {
		int slot = recipe_craft_grid_slot(type, logical);
		if(slot < 0) continue;
		struct item_data cell = plan->cells[logical];
		struct item* def = cell.id ? item_get(&cell) : NULL;
		uint16_t owed = def ? def->container_item : 0;
		const struct item_data* left = &inv->items[slot];
		if(left->id != owed || (owed && left->count != 1))
			return RECIPE_CRAFT_NOT_ONE_BATCH;
	}

	/* The last manual click: shift the returned containers out of the grid.
	 * Beta sends a grid stack to mergeItemStack(main..end, false) -- forward,
	 * main section then hotbar -- which is what inventory_transfer does for a
	 * non-output slot [740c583 ContainerWorkbench/ContainerPlayer
	 * transferStackInSlot]. A container that cannot leave the grid would be
	 * lost when the window closes, so it counts against capacity. */
	for(size_t logical = 0; logical < 9; logical++) {
		int slot = recipe_craft_grid_slot(type, logical);
		if(slot < 0 || !inv->items[slot].id) continue;
		(void)inventory_transfer(inv, type, (size_t)slot, NULL, server);
		if(inv->items[slot].id) return RECIPE_CRAFT_NO_CAPACITY;
	}
	/* Postcondition the commit relies on: the window this batch borrowed is
	 * handed back exactly as it was found -- empty grid, empty output. Each
	 * grid removal above re-ran the matcher (inventory_transfer's non-output
	 * branch), and an empty grid clears slot 0; check it rather than assume. */
	if(inv->items[0].id) return RECIPE_CRAFT_NO_CAPACITY;
	return RECIPE_CRAFT_OK;
}

/* A private, list-detached copy of `inv` over caller-provided storage. The
 * scratch must never be reachable from the shared-inventory list or dispatch a
 * window logic callback: only the pure click helpers run against it. */
static void recipe_craft_scratch(struct inventory* scratch,
	struct item_data* storage, uint16_t* map_counter,
	const struct inventory* inv) {
	*scratch = *inv;
	memcpy(storage, inv->items, inv->capacity * sizeof(*storage));
	scratch->items = storage;
	scratch->logic = NULL;
	scratch->user = NULL;
	scratch->reply = NULL;
	scratch->reply_user = NULL;
	scratch->shared_next = NULL;
	*map_counter = inv->client_map_counter ? *inv->client_map_counter : 0;
	scratch->client_map_counter = map_counter;
	memset(&scratch->revision, 0, sizeof(scratch->revision));
}

static enum recipe_craft_reason recipe_craft_preflight_do(const struct inventory* inv,
 enum window_type type,uint16_t recipe_index,struct recipe_craft_plan* plan,bool action_guards) {
	if(!inv || !inv->items
	   || (type != WINDOW_TYPE_WORKBENCH && type != WINDOW_TYPE_INVENTORY)
	   || (type == WINDOW_TYPE_WORKBENCH && inv->capacity != CRAFTING_SIZE)
	   || (type == WINDOW_TYPE_INVENTORY && inv->capacity != INVENTORY_SIZE))
		return RECIPE_CRAFT_INVALID_REQUEST;
	if(action_guards) {
	if(inv->invalidated || inv->transaction_pending
	   || inv->picked_item.id)
		return RECIPE_CRAFT_BUSY;
	/* The guided action owns the whole grid for the duration of the batch, so
	 * it refuses to run over a hand-laid pattern the player left behind. */
	for(size_t logical = 0; logical < 9; logical++) {
		int slot = recipe_craft_grid_slot(type, logical);
		if(slot >= 0 && inv->items[slot].id) return RECIPE_CRAFT_BUSY;
	}

	}
	enum recipe_craft_reason reason
		= recipe_craft_plan_build(recipe_index, type, plan);
	if(reason != RECIPE_CRAFT_OK) return reason;

	/* Requirement 2 -- capacity preflight. Beta refuses the craft outright when
	 * the result cannot be merged: transferStackInSlot returns null BEFORE
	 * onPickupFromSlot ever runs, so nothing is consumed. The oracle spells the
	 * same gate explicitly: ../mc173 mc173-server/src/player.rs's craft-result
	 * branch is wrapped in `if main_inv.can_push(result_stack)`. We extend that
	 * gate to the returned containers too, because the guided action also owes
	 * the player the buckets/bowls the manual sequence would leave in the grid.
	 *
	 * The dry run deliberately passes server = NULL so it cannot spend a
	 * durable map id it is about to throw away; the committing run below is the
	 * only one that touches the server's counter. */
	struct item_data dry_items[CRAFTING_SIZE];
	struct inventory dry;
	uint16_t dry_maps;
	recipe_craft_scratch(&dry, dry_items, &dry_maps, inv);
	if(!action_guards) {
		/* Ribbon eligibility describes the carried ingredients and capacity,
		 * independently of a temporary cursor, hand-laid grid or pending action. */
		dry.invalidated=false;dry.transaction_pending=false;dry.picked_item=(struct item_data){0};
		dry.items[0]=(struct item_data){0};
		for(size_t logical=0;logical<9;logical++){int slot=recipe_craft_grid_slot(type,logical);if(slot>=0)dry.items[slot]=(struct item_data){0};}
	}
	reason = recipe_craft_run(&dry, type, plan, NULL);
	if(reason != RECIPE_CRAFT_OK) return reason;

 return RECIPE_CRAFT_OK;
}
enum recipe_craft_reason inventory_crafting_one_batch_preflight(const struct inventory* inv,
 enum window_type type,uint16_t recipe_index) {
 struct recipe_craft_plan plan;return recipe_craft_preflight_do(inv,type,recipe_index,&plan,true);
}

enum recipe_craft_reason inventory_crafting_recipe_eligibility(const struct inventory* inv,
 enum window_type type,uint16_t recipe_index) {
 struct recipe_craft_plan plan;return recipe_craft_preflight_do(inv,type,recipe_index,&plan,false);
}

enum recipe_craft_reason inventory_crafting_one_batch(struct inventory* inv,
	enum window_type type, uint16_t recipe_index, struct server_local* server,
	set_inv_slot_t changes) {
 struct recipe_craft_plan plan;
 enum recipe_craft_reason reason=recipe_craft_preflight_do(inv,type,recipe_index,&plan,true);
 if(reason!=RECIPE_CRAFT_OK)return reason;

	struct item_data wet_items[CRAFTING_SIZE];
	struct inventory wet;
	uint16_t wet_maps;
	recipe_craft_scratch(&wet, wet_items, &wet_maps, inv);
	reason = recipe_craft_run(&wet, type, &plan, server);
	if(reason != RECIPE_CRAFT_OK) return reason;

	for(size_t k = 0; k < inv->capacity; k++)
		if(memcmp(&inv->items[k], &wet.items[k], sizeof(inv->items[k])) != 0) {
			inv->items[k] = wet.items[k];
			if(changes) set_inv_slot_push(changes, k);
		}
	/* Client-prediction / test mode only: the authoritative side allocates map
	 * ids through server_local's idcounts, not through this counter. The
	 * allocation ITSELF is carried over too (revision.created_map_id /
	 * map_counter_before): that pair is what lets a correction replay reuse the
	 * id this batch minted instead of spending another one, and what lets an
	 * unsent action hand the counter back -- the same contract
	 * inventory_click.c's craft_created writes for a hand-clicked craft. Leaving
	 * it on the discarded scratch would make the header's "client prediction"
	 * mode quietly leak an id per replay. */
	if(!server && inv->client_map_counter) {
		*inv->client_map_counter = wet_maps;
		inv->revision.map_counter_before = wet.revision.map_counter_before;
		inv->revision.created_map_id = wet.revision.created_map_id;
	}
	/* The scratch has no logic/user, so manual transfer there is silent.
	 * Award the real SlotCrafting result exactly once after this atomic batch
	 * commits (Beta RetroMCP SlotCrafting.java:17-35). Dry runs never award. */
	if(server && inv->user == server)
		inventory_achievement_craft_taken(inv, 0, plan.result);
	return RECIPE_CRAFT_OK;
}
