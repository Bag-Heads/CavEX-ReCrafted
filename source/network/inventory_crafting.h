/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#ifndef INVENTORY_CRAFTING_H
#define INVENTORY_CRAFTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../item/window_container.h"
#include "recipe_craft.h"

struct server_local;

/* Craft EXACTLY one batch of the registered recipe named by `recipe_index` (the
 * flat numbering documented in recipe_craft.h) into the player section of an
 * OPEN, EMPTY player (2x2) or workbench (3x3) crafting window, as if the player
 * had laid the pattern out by hand and shift-clicked the result once.
 *
 * The whole operation is a transaction: it is simulated on a private copy
 * first, and either every slot change lands or none does. On any non-OK return
 * `inv` (and `changes`, which may be NULL) is byte-for-byte untouched.
 *
 * `server` is the authoritative side and is REQUIRED in production: it is what
 * lets Item.onCreated allocate a durable map id (source/item/items/item_map.c),
 * so a persistent map is never minted client-side. NULL is a test/prediction
 * mode that falls back to the inventory's client map counter, exactly as the
 * manual click path's inventory_click.c craft_created already does. */
enum recipe_craft_reason inventory_crafting_one_batch(struct inventory* inv,
	enum window_type type, uint16_t recipe_index, struct server_local* server,
	set_inv_slot_t changes);

/* Read-only verdict from the same detached scratch transaction used before
 * commit. Inventory, callbacks, revisions and map counters are untouched. */
/* Display-only intrinsic recipe eligibility; never authorizes a craft. */
enum recipe_craft_reason inventory_crafting_recipe_eligibility(const struct inventory*,enum window_type,uint16_t);
enum recipe_craft_reason inventory_crafting_one_batch_preflight(
 const struct inventory*,enum window_type,uint16_t recipe_index);

/* Number of registered recipes, i.e. the exclusive upper bound of a valid
 * `recipe_index`. Exposed for the guide and for tests that sweep every recipe. */
size_t inventory_crafting_recipe_count(void);

#endif
