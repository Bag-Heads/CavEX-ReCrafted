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
#include "../../network/client_interface.h"
#include "../../network/server_interface.h"
#include "../../entity/entity_monster.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)


static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	server_local_spawn_monster((vec3) {where->x, where->y, where->z}, MONSTER_ZOMBIE, s);
	return false; // spawn eggs have infinite uses
}

// NET-NEW (non-Beta): b1.7.3 has no spawn-egg concept at all -- there is no
// analogue anywhere in Item.java's static initializer. This item (and its id,
// see items.h ITEM_EGG_ZOMBIE) is entirely this fork's/fCavEX's own addition,
// inherited verbatim and flagged undocumented by FH-08
// (docs/fork_hack_audit_2026-08-21.md), now relocated off Beta's own item-id
// space and recorded as a deviation (docs/deviations.md). Per the
// Beta-1.7.3-first rule (CLAUDE.md) this stays a registration-only addition
// with no in-game acquisition path (no recipe, no debug/creative kit entry,
// no mob drop) until Beta 1.7.3 parity work is complete enough to justify
// spending playability budget on net-new content.
//
// The zombie entity now exists (Phase 2 mob mechanics), so this egg spawns a
// real zombie matching its `item_egg_zombie` symbol. The icon at (8,4) is a
// generic spawn-egg placeholder shared until per-mob egg art lands; per-type
// eggs (skeleton/spider/creeper) are a creative-inventory follow-up.
struct item item_egg_zombie = {
	.name = "Zombie Egg",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 8,
			.texture_y = 4,
		},
	},
};

