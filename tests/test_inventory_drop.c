/*
	test_inventory_drop -- pins C7's Beta-faithful drop-count math
	(inventory_drop_picked), the core behind clicking OUTSIDE a window to drop the
	held cursor item (Beta Container.slotClick slotId == -999):
	  left  (right=false) -> drop the WHOLE held stack, clear the cursor.
	  right (right=true)  -> drop ONE (splitStack(1)); clear the cursor at 0.
	The thrown spawn + the IR "pointer outside the window" trigger are the
	Wii-adapted UX layer (server_local.c / screen_inventory.c); this pins the
	count math that must stay byte-faithful.

	Links inventory.c; item_get is stubbed (test_item_pickup pattern).
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/item/inventory.h"
#include "../source/item/items.h"

static struct item stack64 = {.max_stack = 64};
struct item* item_get(struct item_data* item) {
	(void)item;
	return &stack64;
}

int main(void) {
	struct inventory inv;
	memset(&inv, 0, sizeof(inv));
	struct item_data out;

	// empty cursor -> nothing to drop (no spawn).
	assert(!inventory_drop_picked(&inv, false, &out));
	assert(!inventory_drop_picked(&inv, true, &out));

	// LEFT: drop the whole held stack; cursor cleared.
	inventory_set_picked_item(&inv, (struct item_data) {.id = 4, .count = 10});
	assert(inventory_drop_picked(&inv, false, &out));
	assert(out.id == 4 && out.count == 10);
	assert(!inventory_get_picked_item(&inv, NULL));

	// RIGHT, count > 1: drop ONE, keep the rest on the cursor.
	inventory_set_picked_item(&inv, (struct item_data) {.id = 4, .count = 10});
	assert(inventory_drop_picked(&inv, true, &out));
	assert(out.id == 4 && out.count == 1);
	struct item_data left;
	assert(inventory_get_picked_item(&inv, &left));
	assert(left.id == 4 && left.count == 9);

	// RIGHT, count == 1: drop the last one; cursor cleared.
	inventory_set_picked_item(&inv, (struct item_data) {.id = 7, .count = 1});
	assert(inventory_drop_picked(&inv, true, &out));
	assert(out.id == 7 && out.count == 1);
	assert(!inventory_get_picked_item(&inv, NULL));

	// RIGHT repeatedly empties a stack one at a time, then no-ops.
	inventory_set_picked_item(&inv, (struct item_data) {.id = 4, .count = 3});
	assert(inventory_drop_picked(&inv, true, &out) && out.count == 1);
	assert(inventory_drop_picked(&inv, true, &out) && out.count == 1);
	assert(inventory_drop_picked(&inv, true, &out) && out.count == 1);
	assert(!inventory_get_picked_item(&inv, NULL));
	assert(!inventory_drop_picked(&inv, true, &out));

	// preserves durability on a left drop.
	inventory_set_picked_item(&inv,
							  (struct item_data) {.id = 256, .count = 1,
												  .durability = 42});
	assert(inventory_drop_picked(&inv, false, &out));
	assert(out.id == 256 && out.count == 1 && out.durability == 42);

	puts("test_inventory_drop: OK");
	return 0;
}
