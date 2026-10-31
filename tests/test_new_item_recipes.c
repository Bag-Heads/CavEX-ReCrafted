/*
	Recipe-match goldens for five Beta 1.7.3 crafting recipes that were entirely
	absent from recipe.c (CraftingManager, decompile 740c583):

	  minecartCrate (chest minecart, id 342)   = "A"/"B", A = chest,  B = minecart
	  minecartPowered (furnace minecart, 343)  = "A"/"B", A = furnace, B = minecart
	  boat (id 333)                            = "# #"/"###", # = planks (5, U-shape)
	  painting (id 321)                        = "###"/"#X#"/"###", # = stick (8), X = cloth (any wool)
	  lever (block 69)                         = "X"/"#", X = stick, # = cobblestone

	recipe_match is pure shape-matching (no item registry), so this links only
	recipe.c -- the test_piston_recipe.c / test_sign_recipe.c convention.
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
	struct item_data out;

	// Chest minecart: chest over minecart, placed top-left (recipe_match
	// scans every offset), rest of the 3x3 grid empty.
	const int chest_cart[9] = {BLOCK_CHEST, 0, 0, ITEM_MINECART, 0, 0, 0, 0, 0};
	grid(slots, empty, chest_cart);
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_MINECART_CHEST && out.count == 1);

	// Negative control: swapping chest for furnace must NOT yield a chest cart
	// (it must yield the furnace cart instead -- checked next).
	const int furnace_cart[9] = {BLOCK_FURNACE, 0, 0, ITEM_MINECART, 0, 0, 0, 0, 0};
	grid(slots, empty, furnace_cart);
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_MINECART_FURNACE && out.count == 1);

	// A bare minecart with nothing above it must NOT craft either cart variant.
	const int plain_cart[9] = {ITEM_MINECART, 0, 0, 0, 0, 0, 0, 0, 0};
	grid(slots, empty, plain_cart);
	out = (struct item_data) {0};
	bool m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || (out.id != ITEM_MINECART_CHEST && out.id != ITEM_MINECART_FURNACE));

	// Boat: "# #"/"###" -- 5 planks, full 3x3 grid (width 3, height 2, top row).
	const int boat[9] = {BLOCK_PLANKS, 0,			  BLOCK_PLANKS,
						 BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS,
						 0,			  0,			  0};
	grid(slots, empty, boat);
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_BOAT && out.count == 1);

	// Negative control: filling the boat's middle gap (a solid 2x3 block of
	// planks) must NOT still match the boat (the gap is load-bearing).
	const int not_boat[9] = {BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS,
							 BLOCK_PLANKS, BLOCK_PLANKS, BLOCK_PLANKS,
							 0,			  0,			  0};
	grid(slots, empty, not_boat);
	out = (struct item_data) {0};
	m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != ITEM_BOAT);

	// Painting: "###"/"#X#"/"###" -- 8 sticks around one wool block (any colour).
	const int painting[9] = {ITEM_STICK, ITEM_STICK, ITEM_STICK,
							 ITEM_STICK, BLOCK_WOOL,	ITEM_STICK,
							 ITEM_STICK, ITEM_STICK, ITEM_STICK};
	grid(slots, empty, painting);
	slots[4].durability = 11; // any wool colour must match (match_durability=false)
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_PAINTING && out.count == 1);

	// Negative control: a missing corner stick must NOT still match.
	const int not_painting[9] = {0,			ITEM_STICK, ITEM_STICK,
								 ITEM_STICK, BLOCK_WOOL,	ITEM_STICK,
								 ITEM_STICK, ITEM_STICK, ITEM_STICK};
	grid(slots, empty, not_painting);
	out = (struct item_data) {0};
	m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != ITEM_PAINTING);

	// Lever: stick over cobblestone, placed top-left, rest empty.
	const int lever[9] = {ITEM_STICK, 0, 0, BLOCK_COBBLESTONE, 0, 0, 0, 0, 0};
	grid(slots, empty, lever);
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == BLOCK_LEVER && out.count == 1);

	// Negative control: the pieces swapped (cobblestone over stick) must NOT
	// craft a lever -- the shape is orientation-sensitive.
	const int not_lever[9] = {BLOCK_COBBLESTONE, 0, 0, ITEM_STICK, 0, 0, 0, 0, 0};
	grid(slots, empty, not_lever);
	out = (struct item_data) {0};
	m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != BLOCK_LEVER);

	printf("test_new_item_recipes: OK (chest/furnace minecart, boat, painting, "
		   "lever -- 5 previously-missing Beta CraftingManager recipes)\n");
	return 0;
}
