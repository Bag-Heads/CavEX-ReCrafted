/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#include "../../graphics/render_item.h"
#include "../items.h"

// Beta ItemBoat has NO onItemPlace/onUseOnBlock -- placement is entirely
// onItemRightClick's own independent raycast, which CaveWii routes through
// SRPC_USE_ITEM (item_has_air_use in items.h) to server_local.c's
// server_local_place_boat, mirroring the bow/egg/snowball air-use items.
// [740c583: ItemBoat]
struct item item_boat = {
	.name = "Boat",
	.has_damage = false,
	.max_stack = 1, // Beta ItemBoat ctor: maxStackSize = 1
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			// [740c583: Item.java:351] boat = (new
			// ItemBoat(77)).setIconCoord(8, 8)...
			.texture_x = 8,
			.texture_y = 8,
		},
	},
};
