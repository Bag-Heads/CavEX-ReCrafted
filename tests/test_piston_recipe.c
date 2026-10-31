/*
	Recipe-match golden for the Beta 1.7.3 piston crafting recipes (M4 pistons, phase 4a). Pins the
	exact shapes/outputs from CraftingManager (decompile 740c583):
	  pistonBase(33)      = "TTT"/"#X#"/"#R#"  (T planks, # cobble, X iron ingot, R redstone), x1
	  pistonStickyBase(29)= "S"/"P"            (S slimeball, P piston),                          x1
	recipe_match is pure shape-matching (no item registry), so this links only recipe.c.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "block/blocks_data.h"
#include "item/items.h"
#include "item/recipe.h"

// Lay a 3x3 grid: slots[i].id = ids[i]; slot_empty[i] = (ids[i] == 0).
static void grid(struct item_data slots[9], bool empty[9], const int ids[9]) {
	for(int i = 0; i < 9; i++) {
		slots[i] = (struct item_data) {.id = (uint16_t)ids[i]};
		empty[i] = (ids[i] == 0);
	}
}

int main(void) {
	recipe_init();

	struct item_data slots[9];
	bool empty[9];
	struct item_data out = {0};

	// piston: the full 3x3 grid.
	const int piston[9] = {BLOCK_PLANKS,	  BLOCK_PLANKS, BLOCK_PLANKS,
						   BLOCK_COBBLESTONE, ITEM_IRON,	BLOCK_COBBLESTONE,
						   BLOCK_COBBLESTONE, ITEM_REDSTONE, BLOCK_COBBLESTONE};
	grid(slots, empty, piston);
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == BLOCK_PISTON_BASE && out.count == 1);

	// a WRONG grid (iron swapped to gold) must NOT yield a piston (negative control).
	int bad[9];
	for(int i = 0; i < 9; i++)
		bad[i] = piston[i];
	bad[4] = BLOCK_GOLD_ORE; // not iron ingot
	grid(slots, empty, bad);
	out = (struct item_data) {0};
	bool m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != BLOCK_PISTON_BASE);

	// sticky: slimeball over piston, placed top-left (recipe_match scans every offset), rest empty.
	const int sticky[9] = {ITEM_SLIME_BALL, 0, 0, BLOCK_PISTON_BASE, 0, 0, 0, 0, 0};
	grid(slots, empty, sticky);
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == BLOCK_PISTON_STICKY && out.count == 1);

	printf("test_piston_recipe: OK (Beta pistonBase TTT/#X#/#R# + pistonStickyBase S/P)\n");
	return 0;
}
