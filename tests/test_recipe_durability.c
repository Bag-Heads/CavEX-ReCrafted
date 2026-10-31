/*
	test_recipe_durability -- pins recipe_match()'s durability gate as PER-CELL,
	matching Beta 1.7.3 ShapedRecipes.func_21137_a where the damage check
	(var9.getItemDamage() != -1 && var9.getItemDamage() != var10.getItemDamage())
	is evaluated against the PER-INGREDIENT stack var9, not ingredient 0.

	Audit item C3: the C port's durability clause read the enforce flag from
	cell 0 ('current->shape->match_durability', which decays to shape[0]) for
	every cell, while the durability VALUE was already compared per-cell
	(shape[shape_idx]). For recipes with mixed per-cell match_durability flags
	this is a faithfulness bug: a cell that should enforce its damage was gated
	by cell 0's flag, and a wildcard cell was wrongly gated when cell 0 enforced.

	Scenarios (A2/A3 FAIL pre-fix; A1 is the baseline sanity match):
	  A1  mixed recipe, all durabilities correct                  -> match
	  A2  later cell ENFORCES (flag true), fed wrong durability    -> no match
	      (pre-fix: read shape[0].flag == false -> wrongly matched)
	  A3  cell 0 ENFORCES, a later WILDCARD cell fed any durability -> match
	      (pre-fix: read shape[0].flag == true -> wildcard cell wrongly gated)

	Built headless against the real array_recipe_t API (recipe_add varargs +
	recipe_match); recipe.c links in isolation (it only #includes recipe.h).
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/item/recipe.h"

// Raw ids, kept free of BLOCK_*/ITEM_* constants so the test does not couple to
// the item enum. recipe_add's varargs are (struct item_data, int match) pairs.
#define ID_A 100 // wildcard-durability ingredient
#define ID_B 200 // durability-enforced ingredient

// Build the 9-slot crafting grid for a single column (width 1, height 2):
// cell (0,0) and (0,1) are at grid indices 0 and 3 (px + py*3).
static void grid_1x2(struct item_data slots[9], bool slot_empty[9],
					 struct item_data top, struct item_data bottom) {
	memset(slots, 0, sizeof(struct item_data) * 9);
	for(int k = 0; k < 9; k++)
		slot_empty[k] = true;
	slots[0] = top;       // (x=0, y=0)
	slot_empty[0] = false;
	slots[3] = bottom;    // (x=0, y=1)
	slot_empty[3] = false;
}

int main(void) {
	struct item_data slots[9];
	bool slot_empty[9];
	struct item_data result;

	// ---- recipe R1: top = A (wildcard durability), bottom = B (enforce 4) ----
	// shape[0] = {A, dur 0, match false}, shape[1] = {B, dur 4, match true}
	array_recipe_t r1;
	array_recipe_init(r1);
	recipe_add(r1, (struct item_data) {.id = 1, .durability = 0, .count = 1}, 1,
			   2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = ID_A, .durability = 0}, false,
			   (struct item_data) {.id = ID_B, .durability = 4}, true);

	// A1: correct durabilities -> match. A's durability is wildcard so any value
	// is fine; B must equal 4.
	grid_1x2(slots, slot_empty,
			 (struct item_data) {.id = ID_A, .durability = 99, .count = 1},
			 (struct item_data) {.id = ID_B, .durability = 4, .count = 1});
	assert(recipe_match(r1, slots, slot_empty, &result));
	assert(result.id == 1);

	// A2: bottom cell ENFORCES durability (flag true) but is fed dur 7 != 4.
	// Per-cell gate -> NO match. Pre-fix the gate read shape[0].match_durability
	// (false) for cell 1, so the wrong durability slipped through and it MATCHED.
	grid_1x2(slots, slot_empty,
			 (struct item_data) {.id = ID_A, .durability = 99, .count = 1},
			 (struct item_data) {.id = ID_B, .durability = 7, .count = 1});
	assert(!recipe_match(r1, slots, slot_empty, &result));

	array_recipe_clear(r1);

	// ---- recipe R2: top = B (enforce 4), bottom = A (wildcard durability) ----
	// Mirror direction: shape[0] enforces, shape[1] is the wildcard. A later
	// wildcard cell must IGNORE durability even when cell 0 enforces.
	array_recipe_t r2;
	array_recipe_init(r2);
	recipe_add(r2, (struct item_data) {.id = 2, .durability = 0, .count = 1}, 1,
			   2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = ID_B, .durability = 4}, true,
			   (struct item_data) {.id = ID_A, .durability = 0}, false);

	// A3: top (cell 0) enforces dur 4 and is fed 4 (ok); bottom (cell 1) is a
	// wildcard fed an arbitrary durability 123 -> must still MATCH. Pre-fix the
	// cell-1 gate read shape[0].match_durability (true), so the wildcard cell's
	// mismatched durability wrongly REJECTED the match.
	grid_1x2(slots, slot_empty,
			 (struct item_data) {.id = ID_B, .durability = 4, .count = 1},
			 (struct item_data) {.id = ID_A, .durability = 123, .count = 1});
	assert(recipe_match(r2, slots, slot_empty, &result));
	assert(result.id == 2);

	// Control: top (cell 0) enforces dur 4 but is fed 5 -> NO match (proves the
	// enforce path is live and A3's match is not a false positive).
	grid_1x2(slots, slot_empty,
			 (struct item_data) {.id = ID_B, .durability = 5, .count = 1},
			 (struct item_data) {.id = ID_A, .durability = 123, .count = 1});
	assert(!recipe_match(r2, slots, slot_empty, &result));

	array_recipe_clear(r2);

	printf("test_recipe_durability: OK (per-cell enforce honored, wildcard "
		   "ignored, both directions)\n");
	return 0;
}
