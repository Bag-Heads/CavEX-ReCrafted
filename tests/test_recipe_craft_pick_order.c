/*
	Requirement 4 (lane/craft-rpc): the deterministic ingredient PICK order.

	When the player owns several candidate stacks, the guided craft must choose
	between them the same way every time, or a replay of the same request is not
	byte-stable. The manual grid gives no order to copy -- the player drags
	whichever stack they like -- so the guided path adopts Beta's OWN player-
	inventory scan, the one every automatic InventoryPlayer consumer already
	uses: getInventorySlotContainItem / consumeInventoryItem / storeItemStack
	walk `mainInventory[]` ASCENDING, and mainInventory[0..8] IS the hotbar
	(9..35 being the upper inventory).

	Verified against the runnable oracle (the b1.7.3 decompile pack is not
	present on this host; CLAUDE.md's fallback order is decompile > mc173):
	  * ../mc173  mc173/src/item/inv.rs  `InventoryHandle::consume` -- a plain
	    ascending scan of the inventory slice, first match wins;
	  * ../mc173  mc173-server/src/player.rs -- one 36-slot `main_inv` whose
	    `0..9` half is the hotbar, as its own shift-click shows by sending a
	    stack to `if main_index < 9 { 9..36 } else { 0..9 }`.

	In this engine's window layout that ascending Beta order is: the window's
	nine hotbar slots first, then its twenty-seven main slots, both ascending.

	On top of the order, ONE reservation rule: an exact-metadata requirement is
	satisfied before a wildcard one, so a wildcard cannot spend the only
	metadata-specific stack a later cell needs. Beta's own registered recipe set
	never puts an exact and a wildcard requirement on the same item id, so this
	rule is defensive -- and the only honest way to PROVE it is to register a
	synthetic contention recipe here (appended to the registry, so it shadows
	nothing) and show the craft succeeds where a naive one-pass scan fails.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/block/blocks.h"
#include "../source/item/items.h"
#include "../source/item/recipe.h"
#include "../source/network/inventory_crafting.h"

static uint16_t log_to_planks(void) {
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++) {
		const struct recipe* r = array_recipe_cget(recipes_crafting, n);
		if(r->width == 1 && r->height == 1 && r->shape[0].item.id == BLOCK_LOG
		   && !r->shape[0].match_durability && r->result.id == BLOCK_PLANKS)
			return (uint16_t)n;
	}
	assert(!"1x1 log -> planks recipe missing");
	return RECIPE_CRAFT_INDEX_NONE;
}

/* Each call is an independent, zeroed workbench window. */
static void fresh(struct inventory* inv) {
	assert(inventory_create(inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
}

/* The hotbar is scanned before the main section, even though its slots have the
 * HIGHER window indices -- because in Beta's array it is mainInventory[0..8]. */
static void hotbar_outranks_main(uint16_t planks) {
	struct inventory inv_storage, *inv = &inv_storage;
	fresh(inv);
	inv->items[CRAFTING_SLOT_MAIN + 5]
		= (struct item_data) {.id = BLOCK_LOG, .durability = 1, .count = 4};
	inv->items[CRAFTING_SLOT_HOTBAR + 6]
		= (struct item_data) {.id = BLOCK_LOG, .durability = 2, .count = 4};
	assert(inventory_crafting_one_batch(inv, WINDOW_TYPE_WORKBENCH, planks, NULL,
			   NULL)
		== RECIPE_CRAFT_OK);
	assert(inv->items[CRAFTING_SLOT_HOTBAR + 6].count == 3);
	assert(inv->items[CRAFTING_SLOT_MAIN + 5].count == 4);
	inventory_destroy(inv, NULL);
	puts("  hotbar section is consumed before the main section");
}

/* Within a section, the lowest index wins. */
static void ascending_within_each_section(uint16_t planks) {
	struct inventory inv_storage, *inv = &inv_storage;
	fresh(inv);
	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		inv->items[CRAFTING_SLOT_HOTBAR + k]
			= (struct item_data) {.id = BLOCK_LOG, .durability = 3, .count = 1};
	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		inv->items[CRAFTING_SLOT_MAIN + k]
			= (struct item_data) {.id = BLOCK_LOG, .durability = 3, .count = 1};
	/* Nine crafts drain the hotbar low-to-high and touch no main slot. */
	for(unsigned n = 0; n < INVENTORY_SIZE_HOTBAR; n++) {
		assert(inventory_crafting_one_batch(inv, WINDOW_TYPE_WORKBENCH, planks,
				   NULL, NULL)
			== RECIPE_CRAFT_OK);
		assert(!inv->items[CRAFTING_SLOT_HOTBAR + n].id
			|| inv->items[CRAFTING_SLOT_HOTBAR + n].id == BLOCK_PLANKS);
		for(size_t k = n + 1; k < INVENTORY_SIZE_HOTBAR; k++)
			assert(inv->items[CRAFTING_SLOT_HOTBAR + k].id == BLOCK_LOG);
	}
	/* The tenth reaches the main section, again at its lowest index. */
	assert(inventory_crafting_one_batch(inv, WINDOW_TYPE_WORKBENCH, planks, NULL,
			   NULL)
		== RECIPE_CRAFT_OK);
	assert(inv->items[CRAFTING_SLOT_MAIN + 0].id != BLOCK_LOG);
	assert(inv->items[CRAFTING_SLOT_MAIN + 1].id == BLOCK_LOG);
	inventory_destroy(inv, NULL);
	puts("  within a section the lowest slot index is consumed first");
}

/* A pinned stack (nbt_record 255, the saved-scope pin inventory_consume itself
 * refuses to decrement) is never a candidate, whatever its index. */
static void pinned_stacks_are_skipped(uint16_t planks) {
	struct inventory inv_storage, *inv = &inv_storage;
	fresh(inv);
	inv->items[CRAFTING_SLOT_HOTBAR + 0] = (struct item_data) {
		.id = BLOCK_LOG, .durability = 1, .count = 5, .nbt_record = 255};
	inv->items[CRAFTING_SLOT_HOTBAR + 1]
		= (struct item_data) {.id = BLOCK_LOG, .durability = 2, .count = 5};
	assert(inventory_crafting_one_batch(inv, WINDOW_TYPE_WORKBENCH, planks, NULL,
			   NULL)
		== RECIPE_CRAFT_OK);
	assert(inv->items[CRAFTING_SLOT_HOTBAR + 0].count == 5);
	assert(inv->items[CRAFTING_SLOT_HOTBAR + 1].count == 4);

	/* With ONLY the pinned stack there is nothing to craft with at all. */
	struct inventory only_storage, *only = &only_storage;
	fresh(only);
	only->items[CRAFTING_SLOT_HOTBAR + 0] = (struct item_data) {
		.id = BLOCK_LOG, .count = 5, .nbt_record = 255};
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, only->items, sizeof(before));
	assert(inventory_crafting_one_batch(only, WINDOW_TYPE_WORKBENCH, planks,
			   NULL, NULL)
		== RECIPE_CRAFT_MISSING_INGREDIENTS);
	assert(!memcmp(before, only->items, sizeof(before)));
	inventory_destroy(only, NULL);
	inventory_destroy(inv, NULL);
	puts("  pinned stacks are never spent, and are not counted as available");
}

/* An exact-metadata cell reserves its stack before a wildcard cell of the same
 * item id gets to spend it. Registered here because Beta's own recipe set has
 * no such contention -- see this file's header. */
static void exact_requirements_reserve_before_wildcards(void) {
	uint16_t synthetic = (uint16_t)array_recipe_size(recipes_crafting);
	recipe_add(recipes_crafting,
		(struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1}, 2, 1,
		(uint8_t[]) {1, 2},
		/* cell 0: any dye */ (struct item_data) {.id = ITEM_DYE}, false,
		/* cell 1: lapis   */ (struct item_data) {.id = ITEM_DYE, .durability = 4},
		true);
	assert(array_recipe_size(recipes_crafting) == (size_t)synthetic + 1);

	struct inventory inv_storage, *inv = &inv_storage;
	fresh(inv);
	/* The ONLY lapis sits at the LOWEST index, exactly where a naive one-pass
	 * scan would let the wildcard cell take it and then starve the exact cell. */
	inv->items[CRAFTING_SLOT_HOTBAR + 0]
		= (struct item_data) {.id = ITEM_DYE, .durability = 4, .count = 1};
	inv->items[CRAFTING_SLOT_HOTBAR + 1]
		= (struct item_data) {.id = ITEM_DYE, .durability = 0, .count = 1};
	assert(inventory_crafting_one_batch(inv, WINDOW_TYPE_WORKBENCH, synthetic,
			   NULL, NULL)
		== RECIPE_CRAFT_OK);
	/* Both dyes spent, one stick made: the exact cell got the lapis and the
	 * wildcard cell fell through to the other stack. */
	assert(!inv->items[CRAFTING_SLOT_HOTBAR + 0].id);
	assert(!inv->items[CRAFTING_SLOT_HOTBAR + 1].id);
	unsigned sticks = 0;
	for(size_t k = 0; k < inv->capacity; k++)
		if(inv->items[k].id == ITEM_STICK) sticks += inv->items[k].count;
	assert(sticks == 1);
	inventory_destroy(inv, NULL);

	/* And with no lapis at all the exact cell correctly starves, rather than
	 * being quietly satisfied by some other dye. */
	struct inventory nolapis_storage, *nolapis = &nolapis_storage;
	fresh(nolapis);
	nolapis->items[CRAFTING_SLOT_HOTBAR + 0]
		= (struct item_data) {.id = ITEM_DYE, .durability = 0, .count = 8};
	assert(inventory_crafting_one_batch(nolapis, WINDOW_TYPE_WORKBENCH,
			   synthetic, NULL, NULL)
		== RECIPE_CRAFT_MISSING_INGREDIENTS);
	assert(nolapis->items[CRAFTING_SLOT_HOTBAR + 0].count == 8);
	inventory_destroy(nolapis, NULL);
	puts("  exact-metadata cells reserve their stack ahead of wildcard cells");
}

/* The whole point of a defined order: the same request over the same bytes
 * produces the same bytes, run after run. */
static void replay_is_byte_stable(uint16_t planks) {
	struct item_data seeded[CRAFTING_SIZE];
	struct item_data first[CRAFTING_SIZE];
	memset(seeded, 0, sizeof(seeded));
	/* Many interchangeable candidates, deliberately scattered. */
	const size_t spots[] = {CRAFTING_SLOT_HOTBAR + 8, CRAFTING_SLOT_MAIN + 2,
		CRAFTING_SLOT_HOTBAR + 3, CRAFTING_SLOT_MAIN + 20,
		CRAFTING_SLOT_HOTBAR + 0, CRAFTING_SLOT_MAIN + 11};
	for(size_t k = 0; k < sizeof(spots) / sizeof(*spots); k++)
		seeded[spots[k]] = (struct item_data) {
			.id = BLOCK_LOG, .durability = (uint16_t)k, .count = 3};

	for(unsigned run = 0; run < 3; run++) {
		struct inventory inv_storage, *inv = &inv_storage;
	fresh(inv);
		memcpy(inv->items, seeded, sizeof(seeded));
		for(unsigned n = 0; n < 5; n++)
			assert(inventory_crafting_one_batch(inv, WINDOW_TYPE_WORKBENCH,
					   planks, NULL, NULL)
				== RECIPE_CRAFT_OK);
		if(run == 0) memcpy(first, inv->items, sizeof(first));
		else assert(!memcmp(first, inv->items, sizeof(first)));
		inventory_destroy(inv, NULL);
	}
	puts("  five batches over six candidate stacks replay byte-for-byte");
}

int main(void) {
	blocks_init();
	items_init();
	recipe_init();
	uint16_t planks = log_to_planks();
	hotbar_outranks_main(planks);
	ascending_within_each_section(planks);
	pinned_stacks_are_skipped(planks);
	replay_is_byte_stable(planks);
	/* Last: it appends to the registry. */
	exact_requirements_reserve_before_wildcards();
	puts("PASS test_recipe_craft_pick_order: hotbar-then-main ascending, exact "
		 "before wildcard, pinned skipped, replay byte-stable");
	return 0;
}
