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

// Beta [decompile 740c583] Item.java:375: `cookie = (new ItemCookie(101, 1,
// false, 8))...`. ItemCookie(int id, int healAmount, boolean isWolfFav, int
// stackSize) [ItemCookie.java]: `super(id, healAmount, isWolfFav); this.
// maxStackSize = stackSize;` -- so healAmount=1 (the ItemFood ctor arg,
// UNCHANGED by ItemCookie), isWolfsFavoriteMeat=false, maxStackSize=8
// (overriding ItemFood's own maxStackSize=1 default). ItemFood.
// onItemRightClick: `--stackSize; player.heal(this.healAmount); return
// this;` -- healAmount is in Beta's half-heart units (20-point scale, same
// units as ZOMBIE_ATTACK_DAMAGE/entity/combat.h), so healAmount=1 is exactly
// half a heart: engine_heal = 1 * (HEALTH_PER_HEART/2) = HEALTH_PER_HEART/2,
// the SAME per-half-heart conversion item_apple.c (4 half-hearts ->
// 2*HEALTH_PER_HEART) and item_bread.c (5 half-hearts ->
// 2*HEALTH_PER_HEART+HEALTH_PER_HEART/2) already use.
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

	(void)server_local_heal_actor(s, actor, actor->health + (HEALTH_PER_HEART / 2));
	return true;
}

struct item item_cookie = {
	.name = "Cookie",
	.has_damage = false,
	.max_stack = 8, // Beta ItemCookie ctor 4th arg: maxStackSize=8
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 12,
			.texture_y = 5,
		},
	},
};
