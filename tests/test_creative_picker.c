/*
	Creative phase-2 item picker (docs/creative_mode.md phase 2) -- NET-NEW
	addition under the 2026-08-25 maintainer exception, so these are DESIGN
	pins, not Beta goldens: the list's include-predicate/ordering contract
	and the SRPC_CREATIVE_GIVE handler's gates (creative-off no-op, registry
	validation, hotbar-first placement, full-inventory no-op, server-chosen
	stack size).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/gui/creative_picker_list.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_creative.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)

static void test_list_contract(void) {
	size_t n = creative_picker_count();
	assert(n > 0);

	/* Every entry is registered + renderable (the include predicate), and
	 * count is the item's max_stack. No (id, durability) pair repeats. */
	static bool seen[ITEMS_MAX][16];
	memset(seen, 0, sizeof(seen));
	size_t dye_variants = 0;
	for(size_t k = 0; k < n; k++) {
		struct item_data it;
		assert(creative_picker_entry(k, &it));
		assert(it.id > 0 && it.id < ITEMS_MAX);
		assert(items[it.id] && items[it.id]->renderItem);
		assert(it.count == items[it.id]->max_stack);
		size_t dmg = it.id == ITEM_DYE ? it.durability : 0;
		assert(dmg < 16);
		assert(!seen[it.id][dmg]);
		seen[it.id][dmg] = true;
		if(it.id == ITEM_DYE)
			dye_variants++;
	}
	/* All 16 dye colors are offered (2 featured + 14 in the sweep). */
	assert(dye_variants == 16);

	/* Featured-first: the first entries are the hand-curated build blocks in
	 * their table order (amahpour UX borrow, re-implemented). */
	struct item_data first;
	assert(creative_picker_entry(0, &first));
	assert(first.id == BLOCK_STONE);
	struct item_data second;
	assert(creative_picker_entry(1, &second));
	assert(second.id == BLOCK_COBBLESTONE);

	/* One past the end fails. */
	struct item_data out;
	assert(!creative_picker_entry(n, &out));
	puts("  picker: list contract 1/1");
}

static void test_give_handler(void) {
	assert(clin_init());
	assert(svin_init());

	struct server_local server = {0};
	assert(inventory_create(&server.player.inventory, &inventory_logic_player,
							&server, &server.player, INVENTORY_SIZE, 0, 0, 0));
	server.player.active_inventory = &server.player.inventory;

	struct server_rpc give = {
		.type = SRPC_CREATIVE_GIVE,
		.payload.creative_give.id = BLOCK_STONE,
		.payload.creative_give.durability = 0,
	};

	/* Gate 1: creative OFF -> nothing lands anywhere. */
	server.creative_mode = false;
	server_creative_handle_give(&server, &give);
	for(size_t i = 0; i < INVENTORY_SIZE; i++) {
		struct item_data cur;
		assert(!inventory_get_slot(&server.player.inventory, i, &cur)
			   || !item_get(&cur));
	}

	/* Gate 2: creative ON -> a full stone stack in the FIRST hotbar slot. */
	server.creative_mode = true;
	server_creative_handle_give(&server, &give);
	struct item_data got;
	assert(inventory_get_slot(&server.player.inventory, INVENTORY_SLOT_HOTBAR,
							  &got));
	assert(got.id == BLOCK_STONE);
	assert(got.count == items[BLOCK_STONE]->max_stack);

	/* Gate 3: unregistered id -> no-op (next hotbar slot stays empty). */
	struct server_rpc bogus = give;
	bogus.payload.creative_give.id = 255; /* no Beta block/item at 255 */
	if(!items[255]) {
		server_creative_handle_give(&server, &bogus);
		struct item_data cur;
		assert(!inventory_get_slot(&server.player.inventory,
								   INVENTORY_SLOT_HOTBAR + 1, &cur)
			   || !item_get(&cur));
	}

	/* Gate 4: hotbar-first order -- the second grant lands in hotbar+1. */
	server_creative_handle_give(&server, &give);
	assert(inventory_get_slot(&server.player.inventory,
							  INVENTORY_SLOT_HOTBAR + 1, &got));
	assert(got.id == BLOCK_STONE);

	/* Gate 5: full inventory -> silent no-op (no crash, nothing replaced). */
	struct item_data fill = {.id = BLOCK_DIRT, .count = 1};
	for(size_t i = 0; i < INVENTORY_SIZE_HOTBAR; i++)
		inventory_set_slot(&server.player.inventory,
						   INVENTORY_SLOT_HOTBAR + i, fill);
	for(size_t i = 0; i < INVENTORY_SIZE_MAIN; i++)
		inventory_set_slot(&server.player.inventory, INVENTORY_SLOT_MAIN + i,
						   fill);
	server_creative_handle_give(&server, &give);
	assert(inventory_get_slot(&server.player.inventory, INVENTORY_SLOT_HOTBAR,
							  &got));
	assert(got.id == BLOCK_DIRT && got.count == 1);

	inventory_destroy(&server.player.inventory, &server.player);
	puts("  picker: give handler 1/1");
}

int main(void) {
	blocks_init();
	items_init();
	test_list_contract();
	test_give_handler();
	puts("test_creative_picker: PASS 2/2");
	return 0;
}
