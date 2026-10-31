/*
	Recipe-match golden for the Beta 1.7.3 sign crafting recipe (CR-03 part C).
	Pins the exact shape/output from CraftingManager (decompile 740c583):
	  this.addRecipe(new ItemStack(Item.sign, 1), "###", "###", " X ",
	                 '#', Block.planks, 'X', Item.stick);
	i.e. ONE sign ITEM (id 323), not three of the signPost BLOCK (id 68) --
	the pre-fix output this audit finding replaced.
	recipe_match is pure shape-matching (no item registry), so this links
	only recipe.c, the same convention as test_piston_recipe.c.
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

	// sign: "###"/"###"/" X " -- planks fill the top two rows, a stick sits
	// centered in the bottom row.
	const int sign[9] = {BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS,
						 BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS,
						 0,			  ITEM_STICK,	 0};
	grid(slots, empty, sign);
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_SIGN && out.count == 1);

	// a WRONG grid (stick moved off-center) must NOT yield a sign (negative control).
	int bad[9];
	for(int i = 0; i < 9; i++)
		bad[i] = sign[i];
	bad[7] = 0;
	bad[6] = ITEM_STICK; // stick in the wrong column
	grid(slots, empty, bad);
	out = (struct item_data) {0};
	bool m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != ITEM_SIGN);

	printf("test_sign_recipe: OK (Beta sign ###/###/ X -> 1x item 323, "
		   "not 3x BLOCK_SIGN)\n");
	return 0;
}
