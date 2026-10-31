/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#ifndef NETWORK_RECIPE_CRAFT_H
#define NETWORK_RECIPE_CRAFT_H

/* Leaf value header for the server-authoritative selected-recipe transaction.
 * Deliberately dependency-free (no window/inventory/world headers) so both RPC
 * interface headers can name the wire types without dragging the item layer in.
 * The transaction itself lives in inventory_crafting.h. */

#include <stdint.h>

/* Wire identity of one registered recipe: a FLAT index over `recipes_crafting`
 * in registration order, immediately followed by `recipes_crafting_shapeless`
 * in registration order (so index `array_recipe_size(recipes_crafting)` is
 * shapeless #0). This is byte-for-byte the numbering the client-side recipe
 * guide's own enumerator already produces (source/game/gui/recipe_browser.c
 * `raw_entry`), so a selection made in the UI names the same registration on
 * the server with no translation table and no second source of truth.
 * Registration order is itself the order Beta's CraftingManager walks when it
 * resolves a grid [740c583 CraftingManager.findMatchingRecipe; mirrored by
 * source/item/recipe.c recipe_match / recipe_match_shapeless], so the index is
 * stable for a given build. It is NEVER serialized to a Beta remote server --
 * Beta has no selected-recipe packet; this request exists only between the
 * client and this engine's own local/authoritative server. */
#define RECIPE_CRAFT_INDEX_NONE ((uint16_t)0xFFFF)

/* Rejection reason carried by the reply. RECIPE_CRAFT_OK is the ONLY value for
 * which any inventory slot changed; every other value guarantees the request
 * was fully rejected with no mutation at all (spec: "no hidden overflow, lost
 * ingredients or false success"). */
enum recipe_craft_reason {
	RECIPE_CRAFT_OK = 0,
	RECIPE_CRAFT_INVALID_REQUEST, /* malformed: zero ticket, unusable window type */
	RECIPE_CRAFT_STALE_TICKET,	  /* ticket older than the one already retired */
	RECIPE_CRAFT_WRONG_PLAYER,	  /* request's player does not own this actor */
	RECIPE_CRAFT_WRONG_WINDOW,	  /* window id is not this actor's open window */
	RECIPE_CRAFT_BUSY,			  /* pending transaction, held cursor, non-empty grid */
	RECIPE_CRAFT_UNKNOWN_RECIPE,  /* index unregistered, or the grid it builds is
								   * claimed by an earlier registration */
	RECIPE_CRAFT_RECIPE_TOO_LARGE, /* 3x3 recipe requested in the 2x2 player grid */
	RECIPE_CRAFT_MISSING_INGREDIENTS,
	RECIPE_CRAFT_NO_CAPACITY,	 /* result + returned containers do not all fit */
	RECIPE_CRAFT_CREATE_FAILED,	 /* Item.onCreated refused (durable map id) */
	/* The one-batch postcondition did not hold: the single output shift-click
	 * produced more (or less) than exactly one result stack, or left the grid
	 * holding something other than the returned container items. Fail-closed;
	 * see inventory_crafting.c's "one batch means ONE batch" gate. */
	RECIPE_CRAFT_NOT_ONE_BATCH,
	/* The request's queue envelope resolved to no actor at all -- a stale guest
	 * generation, or a guest slot that is not bound/confirmed. Nothing was even
	 * looked at, let alone mutated; the reply exists purely so the client's one
	 * in-flight request terminates instead of hanging forever. */
	RECIPE_CRAFT_STALE_SESSION,
	/* CLIENT-side only, never sent on the wire: the pending request outlived
	 * its window (closed, replaced, or a new provider generation) or its
	 * deadline, so the guide retires it locally. */
	RECIPE_CRAFT_TIMED_OUT,
};

#endif
