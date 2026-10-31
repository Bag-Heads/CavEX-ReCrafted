/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RECIPE_H
#define RECIPE_H

#include <m-lib/m-array.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "item_types.h"

struct recipe {
	struct item_data result;
	size_t width, height;
	struct recipe_ingredients {
		struct item_data item;
		bool match_durability;
	} shape[9];
};

ARRAY_DEF(array_recipe, struct recipe, M_POD_OPLIST)

extern array_recipe_t recipes_crafting;

// FH-09: Beta's ShapelessRecipes engine (CraftingManager.addShapelessRecipe /
// ShapelessRecipes.matches). Unlike `struct recipe` above, a shapeless recipe
// carries no width/height/position -- it is a bag of up to 9 required
// ingredients that must each match exactly one non-empty grid slot (any
// position, list-removal semantics), with no unmatched slots and no leftover
// ingredients. `match_durability` mirrors the shaped engine's field: false is
// Beta's ItemStack damage == -1 wildcard, true enforces the exact damage/meta
// (dye colour, wool colour, ...). [740c583 ShapelessRecipes.java]
struct recipe_shapeless {
	struct item_data result;
	size_t count;
	struct recipe_shapeless_ingredient {
		struct item_data item;
		bool match_durability;
	} ingredients[9];
};

ARRAY_DEF(array_recipe_shapeless, struct recipe_shapeless, M_POD_OPLIST)

extern array_recipe_shapeless_t recipes_crafting_shapeless;

void recipe_init(void);
void recipe_add(array_recipe_t recipes, struct item_data result, size_t width,
				size_t height, uint8_t* shape, ...);
bool recipe_match(array_recipe_t recipes, struct item_data slots[9],
				  bool slot_empty[9], struct item_data* result);

void recipe_shapeless_add(array_recipe_shapeless_t recipes,
						  struct item_data result, size_t count, ...);
bool recipe_match_shapeless(array_recipe_shapeless_t recipes,
							struct item_data slots[9], bool slot_empty[9],
							struct item_data* result);

#endif