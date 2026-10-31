/*
	Recipe-match + registration golden for the Beta 1.7.3 compass and clock
	(gap-audit parity tail, 2026-08-15). Pins the exact shapes/outputs from
	CraftingManager (decompile 740c583, lines 71-72, in Beta's own order):
	  pocketSundial/clock (91 -> 347) = " # "/"#X#"/" # "  (# gold,  X redstone), x1
	  compass             (89 -> 345) = " # "/"#X#"/" # "  (# iron,  X redstone), x1
	Registration side [Item.java:363,365]: plain items -- maxStackSize 64, no
	damage -- with icons (6,3) and (6,4). The needle/dial animation
	(TextureCompassFX/TextureWatchFX) is a DOCUMENTED static-icon deviation
	(no item-atlas anim infra yet, items_object.h); nothing here depends on it.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks_data.h"
#include "item/items.h"
#include "item/recipe.h"

static void grid(struct item_data slots[9], bool empty[9], const int ids[9]) {
	for(int i = 0; i < 9; i++) {
		slots[i] = (struct item_data) {.id = (uint16_t)ids[i]};
		empty[i] = (ids[i] == 0);
	}
}

int main(void) {
	items_init();
	recipe_init();

	/* Registration: both ids resolve to the cited plain items. */
	struct item_data d_compass = {.id = ITEM_COMPASS};
	struct item_data d_clock = {.id = ITEM_CLOCK};
	struct item* compass = item_get(&d_compass);
	struct item* clock = item_get(&d_clock);
	assert(compass && strcmp(compass->name, "Compass") == 0);
	assert(clock && strcmp(clock->name, "Clock") == 0);
	assert(compass->max_stack == 64 && !compass->has_damage);
	assert(clock->max_stack == 64 && !clock->has_damage);
	assert(compass->render_data.item.texture_x == 6
		   && compass->render_data.item.texture_y == 3); /* Item.java:363 */
	assert(clock->render_data.item.texture_x == 6
		   && clock->render_data.item.texture_y == 4); /* Item.java:365 */
	/* Field-caught 2026-08-31: cooked porkchop pointed at the RAW tile.
	 * [740c583 Item.java:337-338: porkRaw (7,5), porkCooked (8,5)] */
	assert(items[319] && items[319]->render_data.item.texture_x == 7
		   && items[319]->render_data.item.texture_y == 5);
	assert(items[320] && items[320]->render_data.item.texture_x == 8
		   && items[320]->render_data.item.texture_y == 5);
	puts("  registration: 345/347 plain items, icons (6,3)/(6,4)");

	struct item_data slots[9];
	bool empty[9];
	struct item_data out = {0};

	/* Compass: iron plus-ring, redstone center. */
	const int compass_grid[9] = {0,			ITEM_IRON,	   0,
								 ITEM_IRON, ITEM_REDSTONE, ITEM_IRON,
								 0,			ITEM_IRON,	   0};
	grid(slots, empty, compass_grid);
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_COMPASS && out.count == 1);

	/* Clock: gold plus-ring, redstone center -- must yield the CLOCK, never
	   the compass (the metal is the only discriminator). */
	const int clock_grid[9] = {0,		  ITEM_GOLD,	 0,
							   ITEM_GOLD, ITEM_REDSTONE, ITEM_GOLD,
							   0,		  ITEM_GOLD,	 0};
	grid(slots, empty, clock_grid);
	out = (struct item_data) {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_CLOCK && out.count == 1);
	puts("  recipes: iron-plus -> compass, gold-plus -> clock");

	/* Negative controls. Wrong center (iron plus, IRON center): no compass. */
	int bad[9];
	memcpy(bad, compass_grid, sizeof(bad));
	bad[4] = ITEM_IRON;
	grid(slots, empty, bad);
	out = (struct item_data) {0};
	bool m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || (out.id != ITEM_COMPASS && out.id != ITEM_CLOCK));

	/* Filled corners (full 3x3 of iron + redstone center): the plus recipe
	   must NOT match a superset grid. */
	const int corners[9] = {ITEM_IRON, ITEM_IRON,	  ITEM_IRON,
							ITEM_IRON, ITEM_REDSTONE, ITEM_IRON,
							ITEM_IRON, ITEM_IRON,	  ITEM_IRON};
	grid(slots, empty, corners);
	out = (struct item_data) {0};
	m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || (out.id != ITEM_COMPASS && out.id != ITEM_CLOCK));
	puts("  negatives: wrong center + superset grid rejected");

	printf("test_compass_clock_recipe: OK (Beta CraftingManager:71-72)\n");
	return 0;
}
