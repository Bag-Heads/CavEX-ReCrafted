/*
	test_item_pickup -- pins the pickup-collection logic (inventory_collect +
	the player priority order) against Beta 1.7.3 semantics, and the M2 field
	report from the 2026-06-10 validation session ("items are not being picked
	up" with a FULL hotbar and an EMPTY main inventory).

	Pinned:
	  1. Hotbar full of other items + main empty -> the pickup OVERFLOWS into
	     the first main slot (Beta addItemStackToInventory searches hotbar
	     first, then main -- a full hotbar must NOT block pickup).
	  2. A partial same-id hotbar stack is topped up FIRST (priority order),
	     remainder flows to the next slot.
	  3. Completely full inventory -> collect returns false and the stack is
	     untouched (the ENTITY must stay on the ground -- see entity_item.c,
	     which used to destroy it unconditionally: silent item loss).
	  4. Full inventory with one partial matching stack -> PARTIAL pickup:
	     absorbs what fits, returns false, remainder stays in the stack.

	Links only source/network/inventory_logic.c; item_get is stubbed (every id
	stacks to 64). The priority table mirrors inventory_player.c's
	inv_on_collect construction (hotbar slots INVENTORY_SLOT_HOTBAR..+8, then
	main INVENTORY_SLOT_MAIN..+26 -- the constants are the shared ones).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../source/item/inventory.h"
#include "../source/item/items.h"
#include "../source/network/inventory_logic.h"

// --- stub item registry: everything stacks to 64 ---------------------------
static struct item stack64 = {.max_stack = 64};
struct item* item_get(struct item_data* item) {
	(void)item;
	return &stack64;
}

#define SAPLING 6 // any id distinct from the fillers below

static void player_priorities(uint8_t* out) {
	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		out[k] = k + INVENTORY_SLOT_HOTBAR;
	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		out[k + INVENTORY_SIZE_HOTBAR] = k + INVENTORY_SLOT_MAIN;
}

static void fill_hotbar_distinct(struct inventory* inv) {
	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		inv->items[INVENTORY_SLOT_HOTBAR + k] = (struct item_data) {
			.id = 100 + k, .count = 64, .durability = 0};
}

int main(void) {
	uint8_t prio[INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN];
	player_priorities(prio);
	size_t prio_len = sizeof(prio) / sizeof(*prio);

	// --- 1. full hotbar + empty main: overflow into main slot 0 ------------
	{
		struct item_data slots[INVENTORY_SIZE];
		struct inventory inv = {.items = slots, .capacity = INVENTORY_SIZE};
		memset(slots, 0, sizeof(slots));
		fill_hotbar_distinct(&inv);

		struct item_data pick = {.id = SAPLING, .count = 1, .durability = 0};
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		bool ok = inventory_collect(&inv, &pick, prio, prio_len, changes);
		set_inv_slot_clear(changes);

		assert(ok);
		assert(pick.count == 0);
		assert(inv.items[INVENTORY_SLOT_MAIN].id == SAPLING);
		assert(inv.items[INVENTORY_SLOT_MAIN].count == 1);
	}

	// --- 2. partial same-id hotbar stack tops up first, remainder overflows -
	{
		struct item_data slots[INVENTORY_SIZE];
		struct inventory inv = {.items = slots, .capacity = INVENTORY_SIZE};
		memset(slots, 0, sizeof(slots));
		fill_hotbar_distinct(&inv);
		inv.items[INVENTORY_SLOT_HOTBAR + 3]
			= (struct item_data) {.id = SAPLING, .count = 60, .durability = 0};

		struct item_data pick = {.id = SAPLING, .count = 10, .durability = 0};
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		bool ok = inventory_collect(&inv, &pick, prio, prio_len, changes);
		set_inv_slot_clear(changes);

		assert(ok);
		assert(pick.count == 0);
		assert(inv.items[INVENTORY_SLOT_HOTBAR + 3].count == 64); // topped up
		assert(inv.items[INVENTORY_SLOT_MAIN].id == SAPLING);    // remainder
		assert(inv.items[INVENTORY_SLOT_MAIN].count == 6);
	}

	// --- 3. completely full inventory: collect fails, stack untouched ------
	{
		struct item_data slots[INVENTORY_SIZE];
		struct inventory inv = {.items = slots, .capacity = INVENTORY_SIZE};
		memset(slots, 0, sizeof(slots));
		for(size_t k = 0; k < prio_len; k++)
			inv.items[prio[k]]
				= (struct item_data) {.id = 200, .count = 64, .durability = 0};

		struct item_data pick = {.id = SAPLING, .count = 1, .durability = 0};
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		bool ok = inventory_collect(&inv, &pick, prio, prio_len, changes);
		set_inv_slot_clear(changes);

		assert(!ok);
		assert(pick.count == 1); // untouched -> the ground entity must SURVIVE
	}

	// --- 4. full except one partial matching stack: partial pickup ---------
	{
		struct item_data slots[INVENTORY_SIZE];
		struct inventory inv = {.items = slots, .capacity = INVENTORY_SIZE};
		memset(slots, 0, sizeof(slots));
		for(size_t k = 0; k < prio_len; k++)
			inv.items[prio[k]]
				= (struct item_data) {.id = 200, .count = 64, .durability = 0};
		inv.items[INVENTORY_SLOT_MAIN + 5]
			= (struct item_data) {.id = SAPLING, .count = 62, .durability = 0};

		struct item_data pick = {.id = SAPLING, .count = 5, .durability = 0};
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		bool ok = inventory_collect(&inv, &pick, prio, prio_len, changes);
		set_inv_slot_clear(changes);

		assert(!ok);                                             // not fully
		assert(inv.items[INVENTORY_SLOT_MAIN + 5].count == 64);  // absorbed 2
		assert(pick.count == 3); // remainder stays on the ground (Beta)
	}

	printf("test_item_pickup: OK (overflow-to-main, top-up order, full-stay, "
		   "partial)\n");
	return 0;
}
