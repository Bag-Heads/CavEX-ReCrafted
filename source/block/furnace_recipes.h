/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef FURNACE_RECIPES_H
#define FURNACE_RECIPES_H

#include <stdbool.h>

#include "../item/item_types.h"

// Beta 1.7.3 FurnaceRecipes (decompile 740c583). Returns true and fills *out (always
// count 1) iff `in` has a smelting result, else false. PURE (no item registry) so it
// serves as BOTH the production `smelt_result` furnace_env callback (furnace_logic.h)
// AND a headless-testable recipe map (tests/test_furnace_recipes.c). The 10 recipes
// match Beta exactly; this is the single source of truth (inventory_furnace.c calls it).
bool furnace_smelt_result(struct item_data in, struct item_data* out);

#endif
