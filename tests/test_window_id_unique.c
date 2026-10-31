/*
	test_window_id_unique -- pins the C1 data-loss fix: the gstate.players[0].windows[]
	routing ids MUST be pairwise distinct, and opening a (wooden) chest MUST NOT
	destroy the permanent player-inventory window.

	The bug: WINDOWC_CHEST was 0, the same id as WINDOWC_INVENTORY. The client
	CRPC_OPEN_WINDOW handler does `if(windows[w]){windowc_destroy;free;}
	windows[w]=malloc; windowc_create(...)`, so opening a single wooden chest
	(window=WINDOWC_CHEST=0) freed and overwrote the live inventory container --
	armor/items gone (Beta reserves windowId 0 for the player inventory and never
	hands it to a container). Fix: WINDOWC_CHEST -> 3 (a free routing slot).

	Two guards:
	  (A) static: every WINDOWC_ routing id is pairwise distinct.
	  (B) behavioral: seed an item into the inventory window, then replay the
	      exact CRPC_OPEN_WINDOW handler for a chest and assert the inventory
	      window object + its seeded slot survive. Fails against WINDOWC_CHEST==0.

	Links window_container.c + inventory.c; item_get is stubbed (test_item_pickup
	pattern).
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../source/item/inventory.h"
#include "../source/item/items.h"
#include "../source/item/window_container.h"

// --- stub item registry: everything stacks to 64 (mirror test_item_pickup) ---
static struct item stack64 = {.max_stack = 64};
struct item* item_get(struct item_data* item) {
	(void)item;
	return &stack64;
}

int main(void) {
	// (A) Static uniqueness of the routing ids. Index 0 is reserved for the
	// player inventory; the bug aliased the chest onto it.
	const int ids[] = {WINDOWC_INVENTORY, WINDOWC_CHEST, WINDOWC_CRAFTING,
					   WINDOWC_FURNACE,   WINDOWC_IRON_CHEST, WINDOWC_SIGN};
	const int n = (int)(sizeof(ids) / sizeof(ids[0]));
	for(int i = 0; i < n; i++)
		for(int j = i + 1; j < n; j++)
			assert(ids[i] != ids[j]); // FAILS today: CHEST==INVENTORY==0
	assert(WINDOWC_INVENTORY != WINDOWC_CHEST);
	assert(WINDOWC_INVENTORY == 0); // id 0 stays reserved for the inventory

	// (B) Behavioral no-clobber, mirroring the client gstate.players[0].windows[] semantics.
	struct window_container* windows[256] = {0};

	windows[WINDOWC_INVENTORY] = malloc(sizeof(struct window_container));
	assert(windows[WINDOWC_INVENTORY]);
	assert(windowc_create(windows[WINDOWC_INVENTORY], WINDOW_TYPE_INVENTORY,
						  INVENTORY_SIZE));

	// Seed a sentinel into the armor slot.
	const struct item_data armor = {.id = 306, .count = 1, .durability = 0};
	windowc_slot_change(windows[WINDOWC_INVENTORY], INVENTORY_SLOT_ARMOR, armor);

	struct item_data got;
	assert(inventory_get_slot(windowc_get_latest(windows[WINDOWC_INVENTORY]),
							  INVENTORY_SLOT_ARMOR, &got));
	assert(got.id == 306 && got.count == 1);

	// Replay the CRPC_OPEN_WINDOW handler for a single wooden chest verbatim.
	const uint8_t w = WINDOWC_CHEST;
	if(windows[w]) {
		windowc_destroy(windows[w]);
		free(windows[w]);
	}
	windows[w] = malloc(sizeof(struct window_container));
	assert(windows[w]);
	assert(windowc_create(windows[w], WINDOW_TYPE_CHEST, CHEST_SIZE));

	// The inventory window + its armor slot MUST still be intact. Against the
	// buggy define (CHEST==0) windows[0] was just freed/overwritten and this
	// read hits the chest window -> armor gone -> these assertions fail.
	assert(windows[WINDOWC_INVENTORY] != NULL);
	assert(windows[WINDOWC_INVENTORY] != windows[WINDOWC_CHEST]);
	assert(inventory_get_slot(windowc_get_latest(windows[WINDOWC_INVENTORY]),
							  INVENTORY_SLOT_ARMOR, &got));
	assert(got.id == 306 && got.count == 1); // armor survived the chest open

	// The two windows are genuinely separate objects with their own sizes.
	assert(windows[WINDOWC_INVENTORY]->slot_count == INVENTORY_SIZE);
	assert(windows[WINDOWC_CHEST]->slot_count == CHEST_SIZE);

	windowc_destroy(windows[WINDOWC_INVENTORY]);
	free(windows[WINDOWC_INVENTORY]);
	windowc_destroy(windows[WINDOWC_CHEST]);
	free(windows[WINDOWC_CHEST]);

	// (C) Batch H (2026-08-25) hostile-vector: a mismatched-action-id accept
	// must NOT empty the snapshot list. source/item/window_container.c's
	// windowc_action_apply_result accept branch used to walk from the front
	// removing every node hunting for a match; on a total miss (a hostile
	// or buggy MP peer's Packet106Transaction(accepted=true) naming an
	// action_id this window isn't tracking -- see server_remote.c
	// MP_EVENT_TRANSACTION, forwarded with zero validation) it removed
	// EVERY node, including the always-present base snapshot windowc_create
	// seeds, leaving wc->invs empty for the next windowc_get_latest/
	// windowc_slot_change call to dereference a garbage pointer off the
	// intrusive list's own sentinel. The only guard was
	// `assert(!ilist_inventory_empty_p(wc->invs))`, compiled to nothing
	// under the shipping .dol's -DNDEBUG.
	{
		struct window_container wc;
		assert(windowc_create(&wc, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
		assert(windowc_revision_count(&wc) == 1); // just the base snapshot

		uint16_t id1;
		assert(windowc_new_action(&wc, &id1, false, 5));
		assert(windowc_revision_count(&wc) == 2);

		// Hostile/mismatched ack: 999 was never issued by this window.
		windowc_action_apply_result(&wc, 999, /*accept=*/true);
		assert(windowc_revision_count(&wc) >= 1); // NEVER empty
		assert(windowc_revision_count(&wc) == 2); // no match -> untouched no-op
		assert(windowc_get_latest(&wc) != NULL);  // still safe to dereference

		// A genuine, matching ack still collapses the queue correctly --
		// the fix must not break the real accept path, only the mismatched
		// one.
		windowc_action_apply_result(&wc, id1, /*accept=*/true);
		assert(windowc_revision_count(&wc) == 1);
		assert(windowc_get_latest(&wc) != NULL);

		windowc_destroy(&wc);
		puts("  windowc_action_apply_result: mismatched accept id is a "
			 "no-op (never empties the list); genuine ack still collapses it");
	}

	puts("test_window_id_unique: OK");
	return 0;
}
