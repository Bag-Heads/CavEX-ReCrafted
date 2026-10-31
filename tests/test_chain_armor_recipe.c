/*
	Faithfulness PIN for the Beta 1.7.3 chain (chainmail) armor recipes (source/item/recipe.c). The
	2026-06-19 audit (and several stale docs) claimed "chain armor should not be craftable" and proposed
	removing these recipes -- that premise is FACTUALLY WRONG. Beta 1.7.3 RecipesArmor.recipeItems[0] =
	{leather, Block.fire, ingotIron, diamond, ingotGold}: index 1 IS Block.fire (id 51), and
	RecipesArmor.addRecipes() loops EVERY material column including the fire column, registering chainmail
	helmet/chestplate/leggings/boots crafted from fire ('X') in the standard armor patterns. CaveWii is
	byte-faithful (BLOCK_FIRE == Beta id 51; the four shapes match recipePatterns exactly). REMOVING these
	recipes would INTRODUCE a divergence, so this POSITIVE test pins that each chain piece IS craftable
	from the fire grid -- and must never become a negative test.
	(Pure shape-match, like test_piston_recipe.c; links only recipe.c.)
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "block/blocks_data.h" // BLOCK_FIRE
#include "item/items.h"		   // ITEM_CHAIN_*
#include "item/recipe.h"

#define F BLOCK_FIRE

static void grid(struct item_data slots[9], bool empty[9], const int ids[9]) {
	for(int i = 0; i < 9; i++) {
		slots[i] = (struct item_data) {.id = (uint16_t)ids[i]};
		empty[i] = (ids[i] == 0);
	}
}
static void expect(const int ids[9], int want_id) {
	struct item_data slots[9];
	bool empty[9];
	struct item_data out = {0};
	grid(slots, empty, ids);
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == (uint16_t)want_id && out.count == 1);
}

int main(void) {
	recipe_init();

	// Beta armor patterns crafted from fire: helmet XXX/X X, chestplate X X/XXX/XXX,
	// leggings XXX/X X/X X, boots X X/X X (placed top-left; recipe_match scans offsets).
	const int helmet[9] = {F, F, F, F, 0, F, 0, 0, 0};
	const int chest[9] = {F, 0, F, F, F, F, F, F, F};
	const int legs[9] = {F, F, F, F, 0, F, F, 0, F};
	const int boots[9] = {F, 0, F, F, 0, F, 0, 0, 0};

	expect(helmet, ITEM_CHAIN_HELMET);
	expect(chest, ITEM_CHAIN_CHESTPLATE);
	expect(legs, ITEM_CHAIN_LEGGINGS);
	expect(boots, ITEM_CHAIN_BOOTS);

	printf("test_chain_armor_recipe: OK (Beta 'fire armor' quirk -- chainmail IS craftable from fire; "
		   "faithful, do NOT remove)\n");
	return 0;
}
