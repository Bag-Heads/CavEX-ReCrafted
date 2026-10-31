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

#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/client_interface.h"
#include "../../network/server_interface.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	/* S1-a3 OPENED (Astra call #8 Q2): the actor-aware health transaction now
	 * exists, so the ACTING actor eats and heals through it. Beta 1.7.3 has no
	 * hunger -- ItemFood.onItemRightClick is `--stackSize; player.heal(
	 * healAmount);` with NO health gate [ItemFood.java:14-18], so Beta eats
	 * (and wastes) food at full health. The full-health refusal below is NOT
	 * Beta: it is playability carve-out #2 (Astra ruling B, 2026-09-24;
	 * docs/wii_controller_playability.md #30) -- eating fires instantly on the
	 * shared use/place/activate button, so a stray press would throw food
	 * away. The heal itself is Beta's: the same amount, capped at
	 * MAX_PLAYER_HEALTH inside the writer.
	 * A zero-health body never reaches here (the dead action barrier drops the
	 * verb) and would be refused by the writer anyway [EntityLiving.heal gates
	 * on health > 0]. The health reply rides the actor owner+generation
	 * envelope, so a guest bite repaints the GUEST hearts, never P1 hearts. */
	if (actor->health >= MAX_PLAYER_HEALTH) return false;

	(void)server_local_heal_actor(s, actor, actor->health+2*HEALTH_PER_HEART+(HEALTH_PER_HEART/2));
	return true;
}

struct item item_bread = {
	.name = "Bread",
	.has_damage = false,
	.max_stack = 1, // Beta ItemFood.maxStackSize == 1
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 9,
			.texture_y = 2,
		},
	},
};


