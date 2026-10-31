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

#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// Beta BlockReed.canPlaceBlockAt: reed-on-reed is fine; dirt/grass needs adjacent water. (Was missing
	// the water requirement -- sugarcane could be placed on any dry dirt/grass.)
	if(!reed_can_block_stay(s, where->x, where->y, where->z))
		return false;

	return block_place_default(s, actor,
							   &(struct item_data) {
								   .id = BLOCK_REED,
								   .durability = 0,
							   },
							   where, on, on_side);
}

struct item item_sugarcane = {
	.name = "Sugarcane",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 11,
			.texture_y = 1,
		},
	},
};
