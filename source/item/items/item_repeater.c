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

// The placeable redstone-repeater ITEM (Beta Item.redstoneRepeater, id 356). Placing it builds the
// idle repeater block (93) facing away from the placer, per Beta onBlockPlacedBy; the schedule-if-
// powered + obstruction check live in redstone_repeater_place (redstone_server.c).

#include <math.h>

#include "../../block/redstone_server.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	(void)it;
	(void)on;
	(void)on_side;
	// Beta onBlockPlacedBy: facing = ((floor(rotationYaw*4/360 + 0.5) & 3) + 2) % 4. player.rx is
	// already in DEGREES (== Beta rotationYaw), and rotationYaw*4/360 == rotationYaw/90.
	int facing = ((((int)floorf(actor->rx / 90.0F + 0.5F)) & 3) + 2) % 4;
	return redstone_repeater_place(s, actor, where, facing);
}

struct item item_repeater = {
	.name = "Redstone Repeater",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 6, // Beta Item.redstoneRepeater.setIconCoord(6, 5)
			.texture_y = 5,
		},
	},
};
