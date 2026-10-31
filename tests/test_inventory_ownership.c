/* inventory_destroy must not free an inventory embedded in caller storage. */

#include <assert.h>
#include <stdio.h>

#include "item/inventory.h"
#include "item/items.h"

struct embedded_inventory {
	unsigned before;
	struct inventory inventory;
	unsigned after;
};

static unsigned destroy_calls;

static bool request_free(struct inventory* inv, struct server_player* player) {
	assert(inv);
	assert(player);
	destroy_calls++;
	return true;
}

static struct inventory_logic free_requesting_logic = {
	.on_destroy = request_free,
};

struct item* item_get(struct item_data* item) {
	(void)item;
	return NULL;
}

int main(void) {
	struct embedded_inventory owner = {
		.before = 0x12345678,
		.after = 0x9abcdef0,
	};
	struct server_player* actor = (struct server_player*)&owner;

	assert(!inventory_owned_may_free(false, false));
	assert(!inventory_owned_may_free(false, true));
	assert(!inventory_owned_may_free(true, false));
	assert(inventory_owned_may_free(true, true));

	assert(inventory_init(&owner.inventory, &free_requesting_logic, NULL, actor,
						  1, 0, 0, 0));
	assert(!owner.inventory.owned_by_heap);
	inventory_destroy(&owner.inventory, actor);

	assert(destroy_calls == 1);
	assert(!owner.inventory.items);
	assert(owner.before == 0x12345678);
	assert(owner.after == 0x9abcdef0);
	puts("test_inventory_ownership: OK");
	return 0;
}
