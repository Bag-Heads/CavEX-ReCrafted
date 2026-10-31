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

#ifndef INVENTORY_H
#define INVENTORY_H

#include <m-lib/m-dict.h>
#include <m-lib/m-i-list.h>
#include <stdbool.h>
#include <stddef.h>

#include "item_types.h"

// player inventory
#define INVENTORY_SIZE 45
#define INVENTORY_SIZE_HOTBAR 9
#define INVENTORY_SIZE_ARMOR 4
#define INVENTORY_SIZE_MAIN 27
#define INVENTORY_SIZE_CRAFTING 4

#define INVENTORY_SLOT_OUTPUT 0
#define INVENTORY_SLOT_CRAFTING 1
#define INVENTORY_SLOT_ARMOR 5
#define INVENTORY_SLOT_MAIN 9
#define INVENTORY_SLOT_HOTBAR 36

// crafting
#define CRAFTING_SIZE 46
#define CRAFTING_SIZE_INPUT 9

#define CRAFTING_SLOT_OUTPUT 0
#define CRAFTING_SLOT_INPUT 1
#define CRAFTING_SLOT_MAIN 10
#define CRAFTING_SLOT_HOTBAR 37

// furnace -- Beta TileEntityFurnace slots: 0=input, 1=fuel, 2=output (slot 1 was a
// 2nd input before the M4 timed-furnace rewrite; FURNACE_SIZE_INPUT kept for any
// legacy callers but the model is now input/fuel/output).
#define FURNACE_SIZE 39
#define FURNACE_SIZE_INPUT 2

#define FURNACE_SLOT_INPUT 0
#define FURNACE_SLOT_FUEL 1
#define FURNACE_SLOT_OUTPUT 2
#define FURNACE_SLOT_MAIN 3
#define FURNACE_SLOT_HOTBAR 30

// chest
#define CHEST_SIZE 63
#define CHEST_SIZE_STORAGE 27

#define CHEST_SLOT_STORAGE 0
#define CHEST_SLOT_MAIN 27
#define CHEST_SLOT_HOTBAR 54

// iron chest
#define IRON_CHEST_SIZE 90
#define IRON_CHEST_SIZE_STORAGE 54
#define IRON_CHEST_SIZE_MAIN_STORAGE 27
#define IRON_CHEST_SIZE_SIDE_STORAGE 27

#define IRON_CHEST_SLOT_STORAGE 0
#define IRON_CHEST_SLOT_MAIN_STORAGE 0
#define IRON_CHEST_SLOT_SIDE_STORAGE 27
#define IRON_CHEST_SLOT_MAIN 54
#define IRON_CHEST_SLOT_HOTBAR 81

// dispenser -- Beta TileEntityDispenser: 9-slot 3x3 grid (ContainerDispenser.java:9-16).
#define DISPENSER_SIZE 45
#define DISPENSER_SIZE_STORAGE 9

#define DISPENSER_SLOT_STORAGE 0
#define DISPENSER_SLOT_MAIN 9
#define DISPENSER_SLOT_HOTBAR 36

// sign (characters as inventory)
#define SIGN_SIZE 64
// Per-line grid width (4 lines x SIGN_LINE_COLS == SIGN_SIZE); mirrors the
// gui layer's SIGN_OSK_COLS (source/game/gui/sign_osk_model.h) without a
// #include -- that header is deliberately kept a pure/dependency-free module
// (aca219c), so the two layers each carry their own copy of the same fact,
// same as SIGN_SIZE/SIGN_OSK_SIZE already do above it.
//
// Beta's own per-line ceiling is narrower than the grid: [740c583
// GuiEditSign.keyTyped] `signText[editLine].length() < 15`, character-count
// only -- verified against the decompile, no pixel-width term (maintainer
// ruling 2026-08-17). The 16th column (index SIGN_BETA_MAX_CHARS, i.e.
// slot % SIGN_LINE_COLS == SIGN_BETA_MAX_CHARS) may only ever hold ' '.
#define SIGN_LINE_COLS 16
#define SIGN_BETA_MAX_CHARS 15

// picked item slot
#define SPECIAL_SLOT_PICKED_ITEM 255

DICT_SET_DEF(set_inv_slot, size_t)

struct inventory_logic;
struct client_rpc;

struct inventory {
	/* Server replies belong to this window's actor. NULL preserves the legacy
	 * primary transport; client mirrors never emit. Initialized by create. */
	void (*reply)(const struct client_rpc* call, void* user);
	void* reply_user;
	struct inventory* shared_next;
	struct server_player* owner;
	uint8_t orphaned_chest_halves; // removed backing stays removed if the block is replaced
	uint16_t* client_map_counter; // borrowed client-world counter; NULL on server
	struct item_data picked_item;
	struct item_data* items;
	size_t capacity;
	int hotbar_slot;
	w_coord_t x;
	w_coord_t y;
	w_coord_t z;
	// Secondary block position for a double chest (two linked chest tile
	// entities). When has_partner is true the storage spans both halves:
	// (x,y,z) is the primary half (window storage slots 0..26) and
	// (x2,y2,z2) the secondary (27..53). For every other inventory
	// has_partner is false and these are unused.
	w_coord_t x2;
	w_coord_t y2;
	w_coord_t z2;
	bool has_partner;
	/* The item array is always owned. The inventory struct itself is owned
	 * only for inventory_create*(), never for inventory_init(). Deliberately
	 * placed here (not down by logic/user, where it lived before): on Wii
	 * (PPC) the compiler already pads has_partner out to entity_id's 4-byte
	 * alignment, so this bool fills that existing hole instead of forcing a
	 * new one before ILIST_INTERFACE. Keeps sizeof(struct inventory) at 120
	 * on Wii -- see the 15264-byte inventory_sort_plan budget guard in
	 * inventory_sort.h, which embeds this struct and has no growth room. */
	bool owned_by_heap;
	// Entity-keyed backing owner (storage minecart, minecart-slice), the
	// alternative to the block-position key above for a logic whose storage
	// moves. 0 = position-keyed (x,y,z above); nonzero identifies the owning
	// entity by its server id. Entity id 0 is never allocated
	// (server_local_gen_id skips it), so zero is an unambiguous sentinel.
	// Set by inventory_create_entity BEFORE on_create runs, exactly like x/y/z
	// are set before on_create in the position-keyed constructors.
	uint32_t entity_id;
	// Server-side block owner disappeared (TNT/support removal). Storage is
	// already spilled or retained transactionally; reject further clicks and
	// close this GUI at the stable end-of-tick boundary.
	bool invalidated;
	bool transaction_pending; // server Container.setCanCraft(false) until matching 106
	uint16_t transaction_action;
	struct {
		uint16_t action_id;
		bool action_type;
		bool sent; // accepted by transport; cancellation may never reclaim this ID
		size_t action_slot;
		bool sign_set;
		bool no_action; // initial base snapshot
		bool quick_move;
		bool drop;
		struct item_data expected; // slotClick return, including null Quick Move no-op
		uint16_t map_counter_before; // undo only an unsent allocation
		uint16_t created_map_id; // replay reuses this allocation
		uint8_t sign_ch;
	} revision; // for window container
	struct inventory_logic* logic;
	void* user;
	ILIST_INTERFACE(ilist_inventory, struct inventory);
};

/* The ACTING player, threaded explicitly through every window callback (the
 * third sanctioned MP seam, docs/multiplayer_plan_2026-08-14.md "Seam
 * phase"): when the server player pluralizes, the callbacks already know WHO
 * acted -- no ambient active-player global, ever. Single-player passes
 * &server_local.player; the client-side window mirrors (window_container.c)
 * carry logic == NULL, never dispatch a callback, and pass NULL. */
struct server_player;

struct inventory_logic {
	// Populate a newly allocated inventory before it is published to the client.
	// Returning false leaves ownership with inventory_create(), which releases
	// the item array and reports failure. Container implementations use this to
	// reject a window whose backing tile entity is missing instead of opening an
	// unbacked copy that would delete its contents on close.
	bool (*on_create)(struct inventory* inv, struct server_player* player);
	bool (*on_destroy)(struct inventory* inv, struct server_player* player);
	bool (*pre_action)(struct inventory* inv, struct server_player* player,
					   size_t slot, bool right, set_inv_slot_t changes);
	void (*post_action)(struct inventory* inv, struct server_player* player,
						size_t slot, bool right, bool accepted,
						set_inv_slot_t changes);
	bool (*on_collect)(struct inventory* inv, struct server_player* player,
					   struct item_data* item);
	/* Committed result take only. NULL on client mirrors; the callback owns
	 * the result-slot/id filter. Snapshot precedes consume/recompute. */
	void (*on_result_taken)(struct inventory* inv, size_t slot,
		struct item_data result);
	// Transactional ownership contract: false MUST leave this inventory alive
	// and its player-owned contents unchanged so the caller can retry. true may
	// commit backing state and destroy/free the inventory; callers must not touch
	// the old pointer afterward.
	bool (*on_close)(struct inventory* inv, struct server_player* player);
};

ILIST_DEF(ilist_inventory, struct inventory, M_POD_OPLIST)

bool inventory_create(struct inventory* inv, struct inventory_logic* logic,
					  void* user, struct server_player* player,
					  size_t capacity, w_coord_t x, w_coord_t y, w_coord_t z);
// Initialize caller-owned storage (an embedded player inventory). A destroy
// callback may request disposal, but inventory_destroy never frees inv itself.
bool inventory_init(struct inventory* inv, struct inventory_logic* logic,
					void* user, struct server_player* player,
					size_t capacity, w_coord_t x, w_coord_t y, w_coord_t z);
// Like inventory_create, but binds a second block position (a double chest's
// other half). Both positions are set before on_create runs so the logic can
// populate storage from both tile entities. (x,y,z) is the primary half.
bool inventory_create_linked(struct inventory* inv, struct inventory_logic* logic,
							 void* user, struct server_player* player,
							 size_t capacity, w_coord_t x, w_coord_t y,
							 w_coord_t z, w_coord_t x2, w_coord_t y2, w_coord_t z2);
// Like inventory_create, but keys the backing owner by ENTITY id instead of a
// block position (a storage minecart's inventory moves with the cart).
// entity_id is set before on_create runs, same ordering guarantee as x/y/z
// above -- required since on_create needs it to find the live entity.
bool inventory_create_entity(struct inventory* inv, struct inventory_logic* logic,
							 void* user, struct server_player* player,
							 size_t capacity, uint32_t entity_id);
#ifdef CAVEWII_SERVER_LOCAL_TEST
// Inner item-array allocation fault seam for transactional server-window tests.
void inventory_test_set_alloc_hook(void* (*alloc_fn)(size_t));
#endif
void inventory_copy(struct inventory* inv, struct inventory* from);
// player: the actor whose on_destroy writeback runs. NULL is legal only for
// logic-less inventories (client window mirrors), which dispatch nothing. This
// never frees inv itself, even when on_destroy requests it.
void inventory_destroy(struct inventory* inv, struct server_player* player);
bool inventory_owned_may_free(bool owned_by_heap, bool free_mem);
// Heap-storage counterpart: honors on_destroy's disposal request, guarded by
// the ownership set by inventory_create*().
void inventory_destroy_owned(struct inventory* inv,
							 struct server_player* player);
void inventory_clear(struct inventory* inv);
void inventory_consume(struct inventory* inv, size_t slot);
size_t inventory_get_hotbar(struct inventory* inv);
void inventory_set_hotbar(struct inventory* inv, size_t slot);
bool inventory_get_hotbar_item(struct inventory* inv, struct item_data* item);
void inventory_set_slot(struct inventory* inv, size_t slot,
						struct item_data item);
void inventory_clear_slot(struct inventory* inv, size_t slot);
bool inventory_get_slot(struct inventory* inv, size_t slot,
						struct item_data* item);
void inventory_clear_picked_item(struct inventory* inv);
void inventory_set_picked_item(struct inventory* inv, struct item_data item);
bool inventory_get_picked_item(struct inventory* inv, struct item_data* item);
bool inventory_drop_picked(struct inventory* inv, bool right,
						   struct item_data* out);
bool inventory_action(struct inventory* inv, struct server_player* player,
					  size_t slot, bool right, set_inv_slot_t changes);

// True iff `slot` is a valid index into inv->items (and inv is non-NULL) -- i.e.
// the precondition inventory_action only asserts. The window-click dispatch
// (server_local.c SRPC_WINDOW_CLICK) MUST consult this on the UNTRUSTED, network-
// delayed client slot BEFORE inventory_action, because inventory_action's own
// `slot < capacity` check is an assert() that compiles out under the shipping
// .dol's -DNDEBUG. Window capacities differ (player 45, chest 63, iron chest up
// to 90) and a stale large-window click can arrive after active_inventory shrank
// back to the player inventory, so without this guard inventory_action would
// read+write inv->items[slot] out of bounds. static inline so both the dispatch
// and tests/test_window_click_slot_bound.c exercise the very same predicate.
static inline bool inventory_slot_valid(const struct inventory* inv,
										size_t slot) {
	return inv && slot < inv->capacity;
}

static inline bool inventory_slot_pinned(const struct inventory* inv,
									 size_t slot) {
	return inventory_slot_valid(inv, slot) && item_data_pinned(&inv->items[slot]);
}

#endif
