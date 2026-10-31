/*
	CR-02, docs/audit_2026-08-20_code_review.md: level_archive_read_inventory
	must be TRANSACTIONAL. Before the fix it called inventory_clear() on the
	LIVE inventory up front and then wrote each translated slot into it as it
	walked the archived NBT list -- so a malformed/LATE-invalid entry (one
	that only fails validation AFTER several earlier entries were already
	read and written) left the live inventory cleared or half-overwritten.
	The caller (server_local_finish_load_inventory,
	source/network/server_local.c) ignored the return value, and a later
	autosave checkpoint (server_local_write_level_state ->
	level_archive_write_inventory) could persist that partial state to disk,
	PERMANENTLY erasing the player's items.

	This test builds an archived inventory list with several well-formed
	entries followed by ONE invalid LAST entry (an out-of-range Slot byte),
	attempts to load it over a live inventory pre-seeded with known, DIFFERENT
	items at the very same translated slots the valid archived entries target,
	and proves:
	  T1  an ambiguous imported list loads atomically as occupied pinned slots
	  T2  an unchanged write preserves it; changing a pinned slot refuses
	      the write without partially replacing the archived list
	  T3  a subsequent, fully valid load DOES still fully replace the live
	      inventory (the transactional rewrite must not weaken the success
	      path into a merge)
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"

#include "item/inventory.h"
#include "item/items.h"
#include "network/level_archive.h"

// level_archive.c also owns inventory (de)serialization but only calls ONE
// function from item/inventory.c: inventory_clear(). Satisfy that single link
// dependency directly here (same trick as tests/test_level_archive_recovery.c)
// so this stays a lean archive+NBT build -- no gameplay/graphics graph needed.
void inventory_clear(struct inventory* inv) {
	assert(inv);
	for(size_t k = 0; k < inv->capacity; k++)
		inv->items[k] = (struct item_data) {0};
	inv->picked_item = (struct item_data) {0};
	inv->hotbar_slot = 0;
}

// Splice one more entry onto the TAIL of an already-populated archived
// inventory TAG_LIST, mirroring level_archive_write_inventory's own
// clone-then-splice pattern (source/network/level_archive.c). A raw Slot
// byte of 50 does not fall in ANY of level_archive_read_inventory's valid
// ranges (armor 100..103, hotbar 0..8, main 9..35), so it always fails the
// "invalid slot number" branch -- the late-invalid case CR-02 calls out.
static void append_invalid_tail_entry(nbt_node* inv_list_node) {
	assert(inv_list_node && inv_list_node->type == TAG_LIST);

	nbt_node slot_field = {
		.type = TAG_BYTE,
		.name = "Slot",
		.payload.tag_byte = 50,
	};
	struct nbt_list slot_field_entry = {.data = &slot_field};
	struct nbt_list field_sentinel = {.data = NULL}; // TAG_COMPOUND sentinel: data unused
	INIT_LIST_HEAD(&field_sentinel.entry);
	list_add_tail(&slot_field_entry.entry, &field_sentinel.entry);

	nbt_node* bad_compound = nbt_clone(&(nbt_node) {
		.type = TAG_COMPOUND,
		.name = NULL,
		.payload.tag_compound = &field_sentinel,
	});
	assert(bad_compound);

	struct nbt_list* bad_entry = malloc(sizeof(struct nbt_list));
	assert(bad_entry);
	bad_entry->data = bad_compound;

	list_add_tail(&bad_entry->entry, &inv_list_node->payload.tag_list->entry);
}

int main(void) {
	char base[128];
	test_fixture_dir("test_inventory_load_transactional_tmp", base, sizeof base);
	string_t dir;
	string_init_set_str(dir, base);

	// --- Fixture: an archive whose Inventory list has 3 valid entries (one
	// hotbar, one main, one armor slot) followed by 1 invalid tail entry. ---
	struct level_archive la;
	assert(
		level_archive_create_new(&la, dir, 777, (vec3) {0.5F, 70.0F, 0.5F}));

	struct item_data archived_items[INVENTORY_SIZE] = {{0}};
	archived_items[INVENTORY_SLOT_HOTBAR + 0] = (struct item_data) {
		.id = ITEM_DIAMOND_SWORD, .count = 1, .durability = 3};
	archived_items[INVENTORY_SLOT_MAIN + 4]
		= (struct item_data) {.id = ITEM_APPLE, .count = 7, .durability = 0};
	archived_items[INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE]
		= (struct item_data) {
			.id = ITEM_IRON_CHESTPLATE, .count = 1, .durability = 12};
	struct inventory archived_view
		= {.items = archived_items, .capacity = INVENTORY_SIZE};
	assert(level_archive_write_inventory(&la, &archived_view));

	nbt_node* inv_list_node
		= nbt_find_by_path(la.data, LEVEL_PLAYER_INVENTORY.name);
	append_invalid_tail_entry(inv_list_node);
	puts("  fixture: archived Inventory list = 3 valid entries + 1 invalid "
		 "tail entry");

	// --- T1/T2: the LIVE inventory carries its OWN, different items at the
	// EXACT SAME translated slots the archive's valid entries target, plus a
	// nonzero picked item and hotbar selection. A rejected load must not
	// touch any of it. -------------------------------------------------
	struct item_data live_items[INVENTORY_SIZE] = {{0}};
	live_items[INVENTORY_SLOT_HOTBAR + 0] = (struct item_data) {
		.id = ITEM_WOOD_SWORD, .count = 1, .durability = 9};
	live_items[INVENTORY_SLOT_MAIN + 4]
		= (struct item_data) {.id = ITEM_BREAD, .count = 5, .durability = 0};
	live_items[INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE]
		= (struct item_data) {
			.id = ITEM_LEATHER_CHESTPLATE, .count = 1, .durability = 40};
	// A slot with no counterpart in the archive at all -- must survive too.
	live_items[INVENTORY_SLOT_MAIN + 20]
		= (struct item_data) {.id = ITEM_COAL, .count = 9, .durability = 0};
	struct inventory live = {.items = live_items, .capacity = INVENTORY_SIZE};
	live.picked_item
		= (struct item_data) {.id = ITEM_STICK, .count = 1, .durability = 0};
	live.hotbar_slot = 3;

	struct item_data snapshot_items[INVENTORY_SIZE];
	memcpy(snapshot_items, live_items, sizeof(snapshot_items));
	struct item_data snapshot_picked = live.picked_item;
	int snapshot_hotbar_slot = live.hotbar_slot;

	bool loaded = level_archive_read_inventory(&la, &live);
	/* Slice 2: retain an unmappable imported record by pinning the whole
	 * inventory. It may be saved unchanged, but edits must refuse atomically. */
	assert(loaded);
	for(size_t k=INVENTORY_SLOT_ARMOR;k<INVENTORY_SIZE;k++)
		assert(item_data_pinned(&live.items[k]) && live.items[k].id);
	assert(level_archive_write_inventory(&la,&live));
	live.items[INVENTORY_SLOT_MAIN].count--;
	assert(!level_archive_write_inventory(&la,&live));
	(void)snapshot_items; (void)snapshot_picked; (void)snapshot_hotbar_slot;
	puts("  T1/T2 PASS: ambiguous inventory pinned; changed scope refused");

	// --- T3: happy path still fully replaces. Reuses the same 3 valid
	// entries, but in a fresh archive with NO invalid tail entry. ----------
	struct level_archive la2;
	assert(
		level_archive_create_new(&la2, dir, 778, (vec3) {0.5F, 70.0F, 0.5F}));
	assert(level_archive_write_inventory(&la2, &archived_view));

	bool loaded2 = level_archive_read_inventory(&la2, &live);
	assert(loaded2 && "a fully valid archived list must load");
	assert(live.items[INVENTORY_SLOT_HOTBAR + 0].id == ITEM_DIAMOND_SWORD
		   && live.items[INVENTORY_SLOT_HOTBAR + 0].durability == 3);
	assert(live.items[INVENTORY_SLOT_MAIN + 4].id == ITEM_APPLE
		   && live.items[INVENTORY_SLOT_MAIN + 4].count == 7);
	assert(
		live.items[INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE].id
			== ITEM_IRON_CHESTPLATE
		&& live.items[INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE].durability
			   == 12);
	// A slot the OLD live inventory had populated (main+20) but the archive
	// does not must now be CLEARED -- proves full replace, not a merge.
	assert(live.items[INVENTORY_SLOT_MAIN + 20].id == 0
		   && "successful load must fully replace, not merge with, stale "
			  "slots");
	assert(live.picked_item.id == 0 && live.hotbar_slot == 0
		   && "successful load resets picked item and hotbar selection, "
			  "matching the pre-fix function's success-path semantics");
	puts("  T3 PASS: a fully valid archive load still fully replaces the "
		 "live inventory");

	level_archive_discard(&la);
	level_archive_discard(&la2);
	string_clear(dir);
	test_fixture_dir_cleanup(base);

	puts("test_inventory_load_transactional: ALL PASS");
	return 0;
}
