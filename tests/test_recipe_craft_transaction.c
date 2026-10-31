/*
	Requirement 1 (lane/craft-rpc): the server-authoritative "craft one batch of
	recipe R" transaction must be INDISTINGUISHABLE from the manual grid.

	For EVERY registered recipe -- shaped and shapeless, in the 3x3 workbench and
	again in the 2x2 player grid whenever it fits there -- this drives two
	inventories seeded identically:

	  expected: the ordinary UI sequence driven through the REAL click dispatch a
	            window click takes on the wire -- inventory_click_pre +
	            inventory_action + inventory_click_post per click (pick the stack
	            up, drop ONE in the cell, put the remainder back, for every
	            occupied cell), then the output shift-click
	            (inventory_transfer), then a shift-click per returned container
	            (bucket/bowl, [740c583 SlotCrafting.java:36-43]);
	  actual:   one inventory_crafting_one_batch() call.

	The expected side deliberately shares NO sequencing with the module under
	test (hostile-review finding 2): it goes through the cursor and the ordinary
	slot rules, and the recipe matcher re-runs after every single click, so a
	sequencing bug in inventory_crafting.c cannot cancel out on both sides.

	and asserts the two resulting inventories are byte-for-byte identical,
	including leftovers, damage/metadata, and the map counter. The exact-vs-
	wildcard split (CraftingManager's ItemStack damage == -1 wildcard vs an
	exact meta such as the cookie's cocoa dye) and mirrored shaped matching
	([740c583 CraftingManager.findMatchingRecipe / ShapedRecipes.matches], the
	`flip` loop in source/item/recipe.c) get their own explicit sections, since
	sweeping the registry alone would not prove they were EXERCISED.

	The ingredient PICK order this shares with the manual spelling is pinned
	separately and adversarially by tests/test_recipe_craft_pick_order.c.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/block/blocks.h"
#include "../source/item/inventory_click.h"
#include "../source/item/items.h"
#include "../source/item/recipe.h"
#include "../source/network/inventory_crafting.h"

/* The window geometry under test, both spellings in one place. */
struct grid_env {
	enum window_type type;
	size_t capacity, main, hotbar, side;
	const char* name;
};
static const struct grid_env workbench = {WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE,
	CRAFTING_SLOT_MAIN, CRAFTING_SLOT_HOTBAR, 3, "3x3 workbench"};
static const struct grid_env player = {WINDOW_TYPE_INVENTORY, INVENTORY_SIZE,
	INVENTORY_SLOT_MAIN, INVENTORY_SLOT_HOTBAR, 2, "2x2 player grid"};

/* Mirrors the module's own logical-cell -> slot mapping, written out here
 * rather than shared, so a regression in one is not hidden by the other.
 * [740c583 ContainerPlayer ctor: a 2x2 InventoryCrafting at container slots
 * 1..4; the matcher indexes a 3x3 space as x + y*3.] */
static int grid_slot(const struct grid_env* env, size_t logical) {
	static const int small[9] = {1, 2, -1, 3, 4, -1, -1, -1, -1};
	if(logical >= 9) return -1;
	return env->side == 3 ? (int)(1 + logical) : small[logical];
}

/* One recipe expanded into the canonical arrangement the guided path uses. */
struct pattern {
	struct item_data cells[9];
	bool exact[9];
	struct item_data result;
	size_t side_needed; /* smallest square grid that can hold it */
	bool shapeless;
};

static bool pattern_of(uint16_t index, size_t side, struct pattern* p) {
	memset(p, 0, sizeof(*p));
	size_t shaped = array_recipe_size(recipes_crafting);
	if((size_t)index < shaped) {
		const struct recipe* r = array_recipe_cget(recipes_crafting, index);
		p->result = r->result;
		p->side_needed = r->width > r->height ? r->width : r->height;
		if(p->side_needed > side) return false;
		for(size_t y = 0; y < r->height; y++)
			for(size_t x = 0; x < r->width; x++) {
				p->cells[x + y * 3] = r->shape[x + y * r->width].item;
				p->exact[x + y * 3]
					= r->shape[x + y * r->width].match_durability;
			}
		return true;
	}
	size_t n = (size_t)index - shaped;
	if(n >= array_recipe_shapeless_size(recipes_crafting_shapeless)) return false;
	const struct recipe_shapeless* r
		= array_recipe_shapeless_cget(recipes_crafting_shapeless, n);
	p->result = r->result;
	p->shapeless = true;
	p->side_needed = r->count <= 4 ? 2 : 3;
	if(r->count > side * side) return false;
	size_t placed = 0;
	for(size_t logical = 0; logical < 9 && placed < r->count; logical++) {
		static const int small[9] = {1, 2, -1, 3, 4, -1, -1, -1, -1};
		if(side == 2 && small[logical] < 0) continue;
		p->cells[logical] = r->ingredients[placed].item;
		p->exact[logical] = r->ingredients[placed].match_durability;
		placed++;
	}
	return placed == r->count;
}

/* Put the ingredients a single batch needs into hotbar stacks, respecting each
 * item's real max stack size -- so a 3-milk-bucket recipe genuinely occupies
 * three unstackable slots (Beta ItemBucket setMaxStackSize(1)) rather than an
 * impossible stack of three, and the pick order is exercised over real stacks. */
static void seed(struct inventory* inv, const struct grid_env* env,
	const struct pattern* p) {
	size_t used = 0;
	for(size_t k = 0; k < 9; k++) {
		if(!p->cells[k].id) continue;
		struct item_data probe = p->cells[k];
		struct item* def = item_get(&probe);
		unsigned cap = def && def->max_stack ? def->max_stack : 1;
		size_t slot;
		for(slot = 0; slot < used; slot++) {
			struct item_data* it = &inv->items[env->hotbar + slot];
			if(it->id == p->cells[k].id
			   && it->durability == p->cells[k].durability && it->count < cap)
				break;
		}
		if(slot == used) {
			assert(used < INVENTORY_SIZE_HOTBAR);
			inv->items[env->hotbar + used] = p->cells[k];
			inv->items[env->hotbar + used].count = 1;
			used++;
		} else {
			inv->items[env->hotbar + slot].count++;
		}
	}
}

/* ---- the REAL wire click path -------------------------------------------
 *
 * Hostile-review finding 2: an "expected" side that re-types the production
 * sequence (consume a stack, assign the cell, one inventory_click_post) proves
 * only self-consistency -- a shared sequencing bug cancels on both sides. So the
 * expected side below drives the ACTUAL click dispatch a player's window clicks
 * take: server_local.c's SRPC_WINDOW_CLICK calls inventory_action, whose
 * pre_action/post_action hooks are inventory_crafting.c's
 * inventory_click_pre / inventory_click_post; a shift-click is
 * inventory_quick_move, i.e. inventory_transfer. A test window carries
 * logic == NULL and dispatches no callback, so the two halves are spelled out
 * here exactly as that logic row would call them.
 *
 * Nothing about the ingredient LAYOUT is shared with the module any more: an
 * ingredient reaches a grid cell only by being picked up onto the cursor
 * (left-click, Beta Container.slotClick pickup), dropped in one at a time
 * (right-click, inventory_place_item's `!dest->id && right` branch =
 * [740c583 Container.java:90-130]) and having the remainder put back
 * (left-click the source slot again) -- three real clicks per cell, with the
 * matcher re-running after every single one. */
static void wire_click(struct inventory* inv, const struct grid_env* env,
	size_t slot, bool right) {
	if(inventory_click_pre(inv, env->type, slot, right, NULL, NULL))
		(void)inventory_action(inv, NULL, slot, right, NULL);
	inventory_click_post(inv, env->type, slot, NULL);
}

/* Which stack a player would reach for: hotbar then main, both ascending --
 * Beta's own InventoryPlayer mainInventory order (see
 * test_recipe_craft_pick_order.c for the citation and the adversarial proof). */
static int wire_source_slot(const struct inventory* inv,
	const struct grid_env* env, struct item_data want, bool exact) {
	const size_t begin[2] = {env->hotbar, env->main};
	const size_t count[2] = {INVENTORY_SIZE_HOTBAR, INVENTORY_SIZE_MAIN};
	for(size_t range = 0; range < 2; range++)
		for(size_t k = 0; k < count[range]; k++) {
			const struct item_data* it = &inv->items[begin[range] + k];
			if(!it->id || it->id != want.id || !it->count
			   || (exact && it->durability != want.durability)
			   || item_data_pinned(it))
				continue;
			return (int)(begin[range] + k);
		}
	return -1;
}

/* The ordinary UI sequence, as clicks on the wire. */
static bool manual_one_batch(struct inventory* inv, const struct grid_env* env,
	const struct pattern* p) {
	for(unsigned pass = 0; pass < 2; pass++)
		for(size_t logical = 0; logical < 9; logical++) {
			int slot = grid_slot(env, logical);
			if(!p->cells[logical].id || slot < 0
			   || p->exact[logical] != (pass == 0))
				continue;
			int from = wire_source_slot(inv, env, p->cells[logical],
				p->exact[logical]);
			if(from < 0) return false;
			/* Pick the whole stack up. */
			wire_click(inv, env, (size_t)from, false);
			if(!inv->picked_item.id) return false;
			/* Drop exactly one into the (empty) grid cell. */
			wire_click(inv, env, (size_t)slot, true);
			if(inv->items[slot].count != 1) return false;
			/* Put the remainder back where it came from. */
			if(inv->picked_item.id) wire_click(inv, env, (size_t)from, false);
			if(inv->picked_item.id) return false;
		}
	/* The matcher has already run after every click above; the output slot must
	 * be showing the registration's result before the player ever shift-clicks. */
	if(memcmp(&inv->items[0], &p->result, sizeof(p->result)) != 0) return false;
	/* Shift-click the output, then shift each returned container out. */
	(void)inventory_transfer(inv, env->type, 0, NULL, NULL);
	if(inv->items[0].id) return false;
	for(size_t logical = 0; logical < 9; logical++) {
		int slot = grid_slot(env, logical);
		if(slot < 0 || !inv->items[slot].id) continue;
		(void)inventory_transfer(inv, env->type, (size_t)slot, NULL, NULL);
		if(inv->items[slot].id) return false;
	}
	/* The window is handed back idle: empty grid, empty output, empty cursor. */
	return !inv->picked_item.id;
}

/* Returns true when the recipe was actually exercised in this grid. */
static bool compare_one(uint16_t index, const struct grid_env* env,
	unsigned* leftovers_seen, unsigned* exact_seen) {
	struct pattern p;
	if(!pattern_of(index, env->side, &p)) return false;

	struct inventory expected, actual;
	uint16_t expected_maps = 4242, actual_maps = 4242;
	assert(inventory_create(&expected, NULL, NULL, NULL, env->capacity, 0, 0, 0));
	assert(inventory_create(&actual, NULL, NULL, NULL, env->capacity, 0, 0, 0));
	expected.client_map_counter = &expected_maps;
	actual.client_map_counter = &actual_maps;
	seed(&expected, env, &p);
	memcpy(actual.items, expected.items,
		sizeof(*actual.items) * env->capacity);

	bool manual_ok = manual_one_batch(&expected, env, &p);
	enum recipe_craft_reason reason = inventory_crafting_one_batch(&actual,
		env->type, index, NULL, NULL);

	/* Both sides must AGREE on whether this batch is craftable at all -- a
	 * guided path that succeeded where the grid does not would be exactly the
	 * failure this requirement exists to forbid. */
	if(!manual_ok) {
		assert(reason != RECIPE_CRAFT_OK);
		inventory_destroy(&expected, NULL);
		inventory_destroy(&actual, NULL);
		return false;
	}
	assert(reason == RECIPE_CRAFT_OK);
	assert(!memcmp(expected.items, actual.items,
		sizeof(*actual.items) * env->capacity));
	assert(expected_maps == actual_maps);

	for(size_t logical = 0; logical < 9; logical++) {
		struct item_data cell = p.cells[logical];
		struct item* def = item_get(&cell);
		if(cell.id && def && def->container_item) (*leftovers_seen)++;
		if(p.exact[logical]) (*exact_seen)++;
	}
	inventory_destroy(&expected, NULL);
	inventory_destroy(&actual, NULL);
	return true;
}

static void sweep_every_recipe(void) {
	size_t total = inventory_crafting_recipe_count();
	assert(total > 0 && total < RECIPE_CRAFT_INDEX_NONE);
	unsigned big = 0, small = 0, leftovers = 0, exact = 0;
	for(uint16_t index = 0; index < (uint16_t)total; index++) {
		/* Every registration must be craftable in the 3x3 grid: it is the
		 * grid Beta's own CraftingManager registrations are written for. */
		bool ok = compare_one(index, &workbench, &leftovers, &exact);
		assert(ok);
		big++;
		if(compare_one(index, &player, &leftovers, &exact)) small++;
	}
	/* The sweep is only meaningful if it actually reached the interesting
	 * cases; assert the coverage instead of trusting it. */
	assert(big == total);
	assert(small > 0 && small < big);
	assert(leftovers > 0); /* bucket/bowl container returns exercised */
	assert(exact > 0);	   /* exact-metadata ingredients exercised */
	printf("  swept %u recipes in the 3x3 grid, %u of them also in the 2x2 "
		   "(%u container-returning cells, %u exact-meta cells)\n",
		big, small, leftovers, exact);
}

/* Mirrored shaped matching is Beta behaviour the guided path inherits through
 * the real matcher: a horizontally flipped arrangement of a registered shape
 * still resolves. Pin it directly so the sweep's canonical (unflipped) layout
 * cannot silently be the only thing that works. */
static void mirrored_shape_still_matches(void) {
	unsigned checked = 0;
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++) {
		const struct recipe* r = array_recipe_cget(recipes_crafting, n);
		if(r->width < 2) continue;
		/* Asymmetric rows only -- a symmetric shape proves nothing. */
		bool asymmetric = false;
		for(size_t y = 0; y < r->height && !asymmetric; y++)
			for(size_t x = 0; x < r->width / 2; x++)
				if(r->shape[x + y * r->width].item.id
					!= r->shape[(r->width - 1 - x) + y * r->width].item.id) {
					asymmetric = true;
					break;
				}
		if(!asymmetric) continue;

		struct item_data grid[9] = {{0}}, out = {0};
		bool empty[9];
		for(size_t k = 0; k < 9; k++) empty[k] = true;
		for(size_t y = 0; y < r->height; y++)
			for(size_t x = 0; x < r->width; x++) {
				size_t dst = (r->width - 1 - x) + y * 3;
				grid[dst] = r->shape[x + y * r->width].item;
				grid[dst].count = 1;
				empty[dst] = !grid[dst].id;
			}
		assert(recipe_match(recipes_crafting, grid, empty, &out));
		checked++;
		if(checked >= 8) break;
	}
	assert(checked > 0);
	printf("  mirrored matching confirmed on %u asymmetric shaped recipes\n",
		checked);
}

/* An unregistered index must be refused, and must not touch the inventory. */
static void unknown_index_is_refused(void) {
	struct inventory inv;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	inv.items[CRAFTING_SLOT_HOTBAR]
		= (struct item_data) {.id = BLOCK_LOG, .count = 9};
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, inv.items, sizeof(before));
	uint16_t past_end = (uint16_t)inventory_crafting_recipe_count();
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, past_end,
			   NULL, NULL)
		== RECIPE_CRAFT_UNKNOWN_RECIPE);
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH,
			   RECIPE_CRAFT_INDEX_NONE, NULL, NULL)
		== RECIPE_CRAFT_UNKNOWN_RECIPE);
	/* A window whose grid the player already laid out by hand is refused. */
	inv.items[CRAFTING_SLOT_INPUT]
		= (struct item_data) {.id = BLOCK_LOG, .count = 1};
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, 0, NULL,
			   NULL)
		== RECIPE_CRAFT_BUSY);
	inv.items[CRAFTING_SLOT_INPUT] = (struct item_data) {0};
	/* So is one holding a picked-up cursor stack. */
	inventory_set_picked_item(&inv, (struct item_data) {.id = BLOCK_LOG, .count = 1});
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, 0, NULL,
			   NULL)
		== RECIPE_CRAFT_BUSY);
	inventory_clear_picked_item(&inv);
	assert(!memcmp(before, inv.items, sizeof(before)));
	inventory_destroy(&inv, NULL);
	puts("  unknown index / occupied grid / held cursor all refused, no mutation");
}

/* A 3x3-only recipe must be refused in the 2x2 player grid, not silently
 * squeezed -- Beta's ContainerPlayer simply has no cells for it. */
static void oversized_recipe_refused_in_small_grid(void) {
	unsigned refused = 0;
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++) {
		const struct recipe* r = array_recipe_cget(recipes_crafting, n);
		if(r->width <= 2 && r->height <= 2) continue;
		struct inventory inv;
		assert(inventory_create(&inv, NULL, NULL, NULL, INVENTORY_SIZE, 0, 0, 0));
		for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
			inv.items[INVENTORY_SLOT_HOTBAR + k]
				= (struct item_data) {.id = r->shape[0].item.id, .count = 64};
		struct item_data before[INVENTORY_SIZE];
		memcpy(before, inv.items, sizeof(before));
		assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_INVENTORY,
				   (uint16_t)n, NULL, NULL)
			== RECIPE_CRAFT_RECIPE_TOO_LARGE);
		assert(!memcmp(before, inv.items, sizeof(before)));
		inventory_destroy(&inv, NULL);
		if(++refused >= 4) break;
	}
	assert(refused > 0);
	printf("  %u oversized recipes refused in the 2x2 grid with no mutation\n",
		refused);
}

int main(void) {
	blocks_init();
	items_init();
	recipe_init();
	sweep_every_recipe();
	mirrored_shape_still_matches();
	unknown_index_is_refused();
	oversized_recipe_refused_in_small_grid();
	puts("PASS test_recipe_craft_transaction: one-batch request == manual grid "
		 "for every registered recipe");
	return 0;
}
