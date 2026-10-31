/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef ITEM_INVENTORY_CLICK_H
#define ITEM_INVENTORY_CLICK_H

#include "window_container.h"

/* Shared SlotCrafting/SlotArmor/SlotFurnace rules. NULL server predicts with the client-world
 * map counter, without disk IO. Return true to run the ordinary slot rule. */
bool inventory_click_pre(struct inventory*, enum window_type, size_t slot,
	bool right, set_inv_slot_t changes, struct server_local* server);
void inventory_click_post(struct inventory*, enum window_type, size_t slot,
	set_inv_slot_t changes);
struct item_data inventory_click_predict(struct inventory*, enum window_type,
	size_t slot, bool right, bool quick);
/* Crafting grid size for a window type: the player's own 2x2 and the 3x3
 * workbench; every other window has no crafting grid. */
static inline size_t inventory_click_craft_size(enum window_type type) {
	return type == WINDOW_TYPE_INVENTORY ? 4
		: type == WINDOW_TYPE_WORKBENCH ? 9 : 0;
}

/* True for the crafting RESULT slot -- slot 0 of either grid -- the single
 * transfer that COMPLETES a craft (Beta's SlotCrafting). This is the one
 * authority for the test inventory_click_pre and inventory_transfer make
 * before they consume the grid, so a caller outside this module (the menu's
 * completed-craft cue) cannot drift from the rule that does the crafting.
 * Header-inline on purpose: the cue site is a GUI file that must not drag the
 * recipe/item machinery into targets that only compile the menu. */
static inline bool inventory_click_is_craft_output(enum window_type type,
												   size_t slot) {
	return inventory_click_craft_size(type) && slot == 0;
}
/* Container.getStackInSlot + slotClick shift recursion. Returns the actual
 * vanilla comparison stack, NOT a success boolean. Server-only side effects
 * are optional; all slot/count mutations are identical for client prediction. */
struct item_data inventory_transfer(struct inventory*, enum window_type,
	size_t slot, set_inv_slot_t changes, struct server_local* server);

#endif
