/*
	Batch C / FH-09, FH-10, FH-11: the shapeless recipe engine (Beta
	ShapelessRecipes.matches) + RecipesDyes' 31 recipes (16-iteration wool
	recolour loop + 15 dye-mix recipes), plus the two shaped recipes that
	round out the batch: cookie (RecipesFood) and the fishing rod
	(CraftingManager). All shapes/outputs transcribed from the recovered
	decompile (740c583); see the citations inline.

	recipe_match/recipe_match_shapeless are pure id+durability matching (no
	item registry, no rendering), so this links only recipe.c -- the same
	minimal convention as test_piston_recipe.c / test_sign_recipe.c /
	test_compass_clock_recipe.c's sibling minimal tests.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "block/blocks_data.h"
#include "item/items.h"
#include "item/recipe.h"

static void clear_grid(struct item_data slots[9], bool empty[9]) {
	for(int i = 0; i < 9; i++) {
		slots[i] = (struct item_data) {0};
		empty[i] = true;
	}
}

static void place(struct item_data slots[9], bool empty[9], int idx,
				  uint16_t id, uint16_t durability) {
	slots[idx]
		= (struct item_data) {.id = id, .durability = durability, .count = 1};
	empty[idx] = false;
}

int main(void) {
	recipe_init();

	struct item_data slots[9];
	bool empty[9];
	struct item_data out;

	// ==========================================================================
	// Engine semantics (ShapelessRecipes.matches, [740c583 ShapelessRecipes.java])
	// ==========================================================================

	// 1) Any-position match: "3 bone -> 3 bonemeal" needs exactly one bone,
	// ANYWHERE in the grid -- place it in the bottom-right corner (slot 8),
	// nowhere near the top-left the shaped engine would require.
	clear_grid(slots, empty);
	place(slots, empty, 8, ITEM_BONE, 0);
	out = (struct item_data) {0};
	assert(recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								  &out));
	assert(out.id == ITEM_DYE && out.durability == 15 && out.count == 3);
	puts("  engine: any-position match (bone in slot 8 -> 3 bonemeal)");

	// 2) No-extras rejection: the same bone, plus an unrelated stick in
	// another slot. Beta's matches() fails the WHOLE recipe the moment one
	// non-empty grid slot has no unmatched required ingredient left --
	// no shapeless recipe in this port pairs bone with a stick, so the
	// match must fail outright (not fall through to a different recipe).
	clear_grid(slots, empty);
	place(slots, empty, 8, ITEM_BONE, 0);
	place(slots, empty, 0, ITEM_STICK, 0);
	out = (struct item_data) {0};
	assert(!recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								   &out));
	puts("  engine: no-extras rejection (bone + stray stick -> no match)");

	// 3) Leftover-ingredient rejection: "dye5+dye9 -> 2x dye13" (magenta)
	// with only ONE of its two required dyes present. Every grid slot that
	// IS filled matches something, but the recipe's required list is not
	// fully consumed -- matches() returns false while required items remain [ShapelessRecipes.java:46].
	clear_grid(slots, empty);
	place(slots, empty, 0, ITEM_DYE, 5);
	out = (struct item_data) {0};
	assert(!recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								   &out));
	puts("  engine: leftover-ingredient rejection (half a 2-dye recipe)");

	// 4) Meta-sensitive matching: dye(meta 6) + white wool must yield wool
	// meta ~6&15==9, the DYE-6 recolour recipe -- never wool meta 10 (which
	// is what dye meta 5 would produce). Proves durability is part of the
	// match, not just item id.
	clear_grid(slots, empty);
	place(slots, empty, 0, ITEM_DYE, 6);
	place(slots, empty, 1, BLOCK_WOOL, 0);
	out = (struct item_data) {0};
	assert(recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								  &out));
	assert(out.id == BLOCK_WOOL && out.durability == 9 && out.count == 1);
	puts("  engine: meta-sensitive matching (dye6 -> wool9, not wool10)");

	// A genuinely wrong meta pairing (dye1 + dye12, no such recipe exists --
	// the real recipe is dye1+dye11 -> dye14) must not match anything.
	clear_grid(slots, empty);
	place(slots, empty, 0, ITEM_DYE, 1);
	place(slots, empty, 1, ITEM_DYE, 12);
	out = (struct item_data) {0};
	assert(!recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								   &out));
	puts("  engine: meta-sensitive matching (dye1+dye12 -> no recipe)");

	// 5) 2x2 vs 3x3: the same wool-recolour recipe placed entirely inside
	// the player's 2x2 crafting corner (slots 0,1 -- the mapping
	// inventory_player.c uses), and again using slots reachable ONLY in a
	// 3x3 workbench grid (2 and 6). Shapeless recipes ignore position
	// entirely, so both must match identically.
	clear_grid(slots, empty);
	place(slots, empty, 0, ITEM_DYE, 2);
	place(slots, empty, 1, BLOCK_WOOL, 0);
	out = (struct item_data) {0};
	assert(recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								  &out));
	assert(out.id == BLOCK_WOOL && out.durability == ((~2) & 15));

	clear_grid(slots, empty);
	place(slots, empty, 2, ITEM_DYE, 2);
	place(slots, empty, 6, BLOCK_WOOL, 0);
	out = (struct item_data) {0};
	assert(recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								  &out));
	assert(out.id == BLOCK_WOOL && out.durability == ((~2) & 15));
	puts("  engine: 2x2-corner and 3x3-only-slot placements both match");

	// ==========================================================================
	// FH-09 golden: the 16-iteration wool-recolour loop.
	// [740c583 RecipesDyes.java loop; BlockCloth.getDyeFromBlock/getBlockFromDye]
	// ==========================================================================
	for(int n = 0; n < 16; n++) {
		clear_grid(slots, empty);
		place(slots, empty, 0, ITEM_DYE, (uint16_t)n);
		place(slots, empty, 1, BLOCK_WOOL, 0);
		out = (struct item_data) {0};
		assert(recipe_match_shapeless(recipes_crafting_shapeless, slots,
									  empty, &out));
		assert(out.id == BLOCK_WOOL && out.count == 1);
		assert(out.durability == (uint16_t)((~n) & 15));
	}
	puts("  golden: all 16 wool recolours (dye N + white wool -> wool ~N&15)");

	// ==========================================================================
	// FH-09 golden: the 15 dye-mix / dye-source recipes, verbatim decompile
	// order. [740c583 RecipesDyes.java]
	// ==========================================================================

	// dyePowder 2 meta 11 <- plantYellow (BLOCK_FLOWER)
	clear_grid(slots, empty);
	place(slots, empty, 0, BLOCK_FLOWER, 0);
	out = (struct item_data) {0};
	assert(recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								  &out));
	assert(out.id == ITEM_DYE && out.durability == 11 && out.count == 2);

	// dyePowder 2 meta 1 <- plantRed (BLOCK_ROSE)
	clear_grid(slots, empty);
	place(slots, empty, 0, BLOCK_ROSE, 0);
	out = (struct item_data) {0};
	assert(recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								  &out));
	assert(out.id == ITEM_DYE && out.durability == 1 && out.count == 2);

	// dyePowder 3 meta 15 <- bone (already exercised above for engine
	// semantics; re-check the golden values explicitly here too)
	clear_grid(slots, empty);
	place(slots, empty, 0, ITEM_BONE, 0);
	out = (struct item_data) {0};
	assert(recipe_match_shapeless(recipes_crafting_shapeless, slots, empty,
								  &out));
	assert(out.id == ITEM_DYE && out.durability == 15 && out.count == 3);
	puts("  golden: plantYellow->2x dye11, plantRed->2x dye1, bone->3x dye15");

	// The 12 remaining dye+dye recipes, table-driven.
	struct dye_mix_case {
		int n_in;
		int in_dur[4];
		int out_dur;
		int out_count;
	};

	const struct dye_mix_case cases[] = {
		{2, {1, 15}, 9, 2},		  // dye1+dye15 -> 2x dye9  (pink)
		{2, {1, 11}, 14, 2},	  // dye1+dye11 -> 2x dye14 (orange)
		{2, {2, 15}, 10, 2},	  // dye2+dye15 -> 2x dye10 (lime)
		{2, {0, 15}, 8, 2},		  // dye0+dye15 -> 2x dye8  (light blue... per decompile order)
		{2, {8, 15}, 7, 2},		  // dye8+dye15 -> 2x dye7  (light gray)
		{3, {0, 15, 15}, 7, 3},	  // dye0+dye15+dye15 -> 3x dye7
		{2, {4, 15}, 12, 2},	  // dye4+dye15 -> 2x dye12
		{2, {4, 2}, 6, 2},		  // dye4+dye2  -> 2x dye6
		{2, {4, 1}, 5, 2},		  // dye4+dye1  -> 2x dye5
		{2, {5, 9}, 13, 2},		  // dye5+dye9  -> 2x dye13
		{3, {4, 1, 9}, 13, 3},	  // dye4+dye1+dye9 -> 3x dye13
		{4, {4, 1, 1, 15}, 13, 4}, // dye4+dye1+dye1+dye15 -> 4x dye13
	};

	for(size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		clear_grid(slots, empty);
		for(int k = 0; k < cases[c].n_in; k++)
			place(slots, empty, k, ITEM_DYE, (uint16_t)cases[c].in_dur[k]);
		out = (struct item_data) {0};
		assert(recipe_match_shapeless(recipes_crafting_shapeless, slots,
									  empty, &out));
		assert(out.id == ITEM_DYE);
		assert(out.durability == (uint16_t)cases[c].out_dur);
		assert(out.count == (uint8_t)cases[c].out_count);
	}
	puts("  golden: all 12 remaining dye-mix recipes (count + meta exact)");

	// ==========================================================================
	// FH-10: cookie (SHAPED -- "#X#", # = wheat, X = dyePowder meta 3 cocoa).
	// The existing shaped engine's match_durability field already handles a
	// meta-sensitive ingredient (see the lapis-cast recipe in recipe.c), so
	// no engine change was needed for this one. [740c583 RecipesFood.java]
	// ==========================================================================
	clear_grid(slots, empty);
	place(slots, empty, 0, ITEM_WHEAT, 0);
	place(slots, empty, 1, ITEM_DYE, 3);
	place(slots, empty, 2, ITEM_WHEAT, 0);
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_COOKIE && out.count == 8);

	// Negative: wrong dye meta (4 == lapis lazuli, not cocoa) must not match.
	clear_grid(slots, empty);
	place(slots, empty, 0, ITEM_WHEAT, 0);
	place(slots, empty, 1, ITEM_DYE, 4);
	place(slots, empty, 2, ITEM_WHEAT, 0);
	out = (struct item_data) {0};
	bool m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != ITEM_COOKIE);
	puts("  FH-10: cookie #X# (wheat/cocoa dye3/wheat) -> 8x cookie; "
		 "wrong dye meta rejected");

	// ==========================================================================
	// FH-11: fishing rod (SHAPED -- "  #", " #X", "# X"; # = stick, X = silk
	// == ITEM_STRING). [740c583 CraftingManager.java: fishingRod recipe]
	// ==========================================================================
	clear_grid(slots, empty);
	place(slots, empty, 2, ITEM_STICK, 0);	 // row0 col2
	place(slots, empty, 4, ITEM_STICK, 0);	 // row1 col1
	place(slots, empty, 5, ITEM_STRING, 0); // row1 col2
	place(slots, empty, 6, ITEM_STICK, 0);	 // row2 col0
	place(slots, empty, 8, ITEM_STRING, 0); // row2 col2
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_FISHING_ROD && out.count == 1);

	// Negative: drop one of the two required strings -- must not match.
	clear_grid(slots, empty);
	place(slots, empty, 2, ITEM_STICK, 0);
	place(slots, empty, 4, ITEM_STICK, 0);
	place(slots, empty, 6, ITEM_STICK, 0);
	place(slots, empty, 8, ITEM_STRING, 0);
	out = (struct item_data) {0};
	m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != ITEM_FISHING_ROD);
	puts("  FH-11: fishing rod (3 stick diagonal + 2 string) -> 1x rod; "
		 "missing string rejected");

	printf("test_shapeless_recipes: OK (FH-09 engine + 31 dye recipes, "
		   "FH-10 cookie, FH-11 fishing rod)\n");
	return 0;
}
