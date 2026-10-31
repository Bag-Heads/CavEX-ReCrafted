/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "creative_picker_list.h"

#include "../../block/blocks.h"
#include "../../item/items.h" // direct include (was transitive via blocks.h pre-split)

/* The one include-predicate (see the header): registered AND renderable.
 * Every path below -- featured emit, sweep emit, count -- goes through it. */
static bool picker_includes(int id) {
	if(id <= 0 || id >= ITEMS_MAX)
		return false;
	return items[id] && items[id]->renderItem;
}

/* Featured placeables-first section (amahpour UX borrow, re-implemented):
 * the blocks/tools a build session actually reaches for, hand-ordered.
 * Unregistered entries are skipped by the predicate at enumeration time, so
 * this table can never offer a dead cell. Durability rides the item-damage
 * field exactly like the debug.give kit (dye colors). */
static const struct item_data featured[] = {
	/* build blocks */
	{.id = BLOCK_STONE}, {.id = BLOCK_COBBLESTONE}, {.id = BLOCK_PLANKS},
	{.id = BLOCK_DIRT}, {.id = BLOCK_SAND}, {.id = BLOCK_GRAVEL},
	{.id = BLOCK_LOG}, {.id = BLOCK_LEAVES}, {.id = BLOCK_GLASS},
	{.id = BLOCK_WOOL}, {.id = BLOCK_BRICKS}, {.id = BLOCK_MOSSY_COBBLE},
	{.id = BLOCK_SANDSTONE}, {.id = BLOCK_OBSIDIAN}, {.id = BLOCK_BOOKSHELF},
	{.id = BLOCK_SLAB}, {.id = BLOCK_WOODEN_STAIRS}, {.id = BLOCK_STONE_STAIRS},
	/* light + utility */
	{.id = BLOCK_TORCH}, {.id = BLOCK_LADDER}, {.id = BLOCK_CHEST},
	{.id = BLOCK_WORKBENCH}, {.id = BLOCK_FURNACE}, {.id = BLOCK_TNT},
	/* redstone kit */
	{.id = ITEM_REDSTONE}, {.id = BLOCK_REDSTONE_TORCH_LIT},
	{.id = BLOCK_LEVER}, {.id = ITEM_REDSTONE_REPEATER},
	{.id = BLOCK_STONE_BUTTON}, {.id = BLOCK_STONE_PRESSURE_PLATE},
	{.id = BLOCK_PISTON_BASE}, {.id = BLOCK_PISTON_STICKY},
	{.id = BLOCK_RAIL}, {.id = BLOCK_POWERED_RAIL},
	{.id = BLOCK_DETECTOR_RAIL}, {.id = ITEM_MINECART},
	/* hands */
	{.id = ITEM_FLINT_STEEL}, {.id = ITEM_BUCKET_WATER},
	{.id = ITEM_BUCKET_LAVA}, {.id = ITEM_DIAMOND_PICKAXE},
	{.id = ITEM_IRON_SWORD}, {.id = ITEM_BOW}, {.id = ITEM_ARROW},
	{.id = ITEM_DYE, .durability = 15}, /* bonemeal */
	{.id = ITEM_DYE, .durability = 4},  /* lapis */
};
#define FEATURED_COUNT (sizeof(featured) / sizeof(featured[0]))

static bool picker_is_featured_id(int id) {
	for(size_t k = 0; k < FEATURED_COUNT; k++)
		if(featured[k].id == id)
			return true;
	return false;
}

/* Enumerate every entry in display order, invoking `emit` until it returns
 * false. Featured section (predicate-filtered) first, then the ascending-id
 * sweep of everything not already featured; ITEM_DYE expands into its 16
 * damage colors (two of them live in the featured section, so the sweep
 * skips the dye id like any other featured id -- the 14 remaining colors
 * follow the sweep position of id 351 via the explicit expansion below). */
static bool picker_visit(bool (*emit)(void* user, const struct item_data* it),
						 void* user) {
	for(size_t k = 0; k < FEATURED_COUNT; k++) {
		if(!picker_includes(featured[k].id))
			continue;
		struct item_data it = featured[k];
		it.count = items[it.id]->max_stack;
		if(!emit(user, &it))
			return false;
	}
	for(int id = 1; id < ITEMS_MAX; id++) {
		if(!picker_includes(id))
			continue;
		if(id == ITEM_DYE) {
			/* all 16 colors, featured ones (4, 15) excluded */
			for(int dmg = 0; dmg < 16; dmg++) {
				if(dmg == 4 || dmg == 15)
					continue;
				struct item_data it = {.id = ITEM_DYE,
									   .durability = (uint16_t)dmg,
									   .count = items[id]->max_stack};
				if(!emit(user, &it))
					return false;
			}
			continue;
		}
		if(picker_is_featured_id(id))
			continue;
		struct item_data it = {.id = (uint16_t)id,
							   .count = items[id]->max_stack};
		if(!emit(user, &it))
			return false;
	}
	return true;
}

struct picker_counter {
	size_t n;
};

static bool picker_count_emit(void* user, const struct item_data* it) {
	(void)it;
	((struct picker_counter*)user)->n++;
	return true;
}

size_t creative_picker_count(void) {
	struct picker_counter c = {0};
	picker_visit(picker_count_emit, &c);
	return c.n;
}

struct picker_finder {
	size_t want;
	size_t seen;
	struct item_data* out;
	bool found;
};

static bool picker_find_emit(void* user, const struct item_data* it) {
	struct picker_finder* f = user;
	if(f->seen++ == f->want) {
		*f->out = *it;
		f->found = true;
		return false;
	}
	return true;
}

bool creative_picker_entry(size_t index, struct item_data* out) {
	struct picker_finder f = {.want = index, .out = out};
	picker_visit(picker_find_emit, &f);
	return f.found;
}
