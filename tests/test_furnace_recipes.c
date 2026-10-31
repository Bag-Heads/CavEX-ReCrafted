/*
	Pins the Beta 1.7.3 furnace smelt-recipe map (furnace_recipes.c) against the
	decompile (740c583 FurnaceRecipes). PURE (no item registry), so it links only
	furnace_recipes.c. This is the single source of truth shared by the instant
	furnace (inventory_furnace.c) and the timed-furnace tick env (furnace_logic.h),
	so a recipe regression here fails the build for both.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "block/blocks_data.h"
#include "block/furnace_recipes.h"
#include "item/items.h"

static void expect(int in_id, int out_id, int out_dur) {
	struct item_data out = {.count = 0};
	struct item_data in = {.id = (uint16_t)in_id, .durability = 0, .count = 1};
	bool ok = furnace_smelt_result(in, &out);
	assert(ok);
	assert(out.id == (uint16_t)out_id);
	assert(out.durability == (uint16_t)out_dur);
	assert(out.count == 1); // Beta results are always a single item
}

static void expect_none(int in_id) {
	struct item_data out = {.count = 0};
	struct item_data in = {.id = (uint16_t)in_id, .durability = 0, .count = 1};
	assert(!furnace_smelt_result(in, &out));
}

int main(void) {
	// The 10 Beta 1.7.3 smelts (FurnaceRecipes.smelting()).
	expect(ITEM_CLAY_BALL, ITEM_BRICK, 0);
	expect(BLOCK_CACTUS, ITEM_DYE, 2);		   // cactus green dye
	expect(BLOCK_LOG, ITEM_COAL, 1);		   // wood -> charcoal (coal damage 1)
	expect(BLOCK_COBBLESTONE, BLOCK_STONE, 0);
	expect(BLOCK_DIAMOND_ORE, ITEM_DIAMOND, 0);
	expect(BLOCK_SAND, BLOCK_GLASS, 0);
	expect(ITEM_FISH, ITEM_FISH_COOKED, 0);
	expect(BLOCK_GOLD_ORE, ITEM_GOLD, 0);
	expect(BLOCK_IRON_ORE, ITEM_IRON, 0);
	expect(ITEM_PORKCHOP, ITEM_PORKCHOP_COOKED, 0);

	// Non-smeltables (a few that are deliberately NOT furnace recipes in b1.7.3).
	expect_none(BLOCK_STONE);	  // already smelted
	expect_none(ITEM_COAL);		  // fuel, not input
	expect_none(BLOCK_DIRT);
	expect_none(ITEM_BRICK);
	expect_none(0);				  // empty / air

	printf("test_furnace_recipes: OK (10 Beta smelts pinned, non-recipes rejected)\n");
	return 0;
}
