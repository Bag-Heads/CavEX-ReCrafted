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

#include "furnace_recipes.h"

#include "blocks_data.h"

// Beta FurnaceRecipes.smelting() (decompile 740c583): the 10 b1.7.3 smelts.
bool furnace_smelt_result(struct item_data in, struct item_data* out) {
	struct item_data r = {.count = 0};
	switch(in.id) {
		case ITEM_CLAY_BALL:
			r = (struct item_data) {.id = ITEM_BRICK, .durability = 0, .count = 1};
			break;
		case BLOCK_CACTUS:
			r = (struct item_data) {.id = ITEM_DYE, .durability = 2, .count = 1};
			break;
		case BLOCK_LOG:
			r = (struct item_data) {.id = ITEM_COAL, .durability = 1, .count = 1};
			break;
		case BLOCK_COBBLESTONE:
			r = (struct item_data) {.id = BLOCK_STONE, .durability = 0, .count = 1};
			break;
		case BLOCK_DIAMOND_ORE:
			r = (struct item_data) {.id = ITEM_DIAMOND, .durability = 0, .count = 1};
			break;
		case BLOCK_SAND:
			r = (struct item_data) {.id = BLOCK_GLASS, .durability = 0, .count = 1};
			break;
		case ITEM_FISH:
			r = (struct item_data) {.id = ITEM_FISH_COOKED, .durability = 0, .count = 1};
			break;
		case BLOCK_GOLD_ORE:
			r = (struct item_data) {.id = ITEM_GOLD, .durability = 0, .count = 1};
			break;
		case BLOCK_IRON_ORE:
			r = (struct item_data) {.id = ITEM_IRON, .durability = 0, .count = 1};
			break;
		case ITEM_PORKCHOP:
			r = (struct item_data) {
				.id = ITEM_PORKCHOP_COOKED, .durability = 0, .count = 1};
			break;
		default: break;
	}
	if(r.count == 0)
		return false;
	*out = r;
	return true;
}
