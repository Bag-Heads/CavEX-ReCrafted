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

#include <assert.h>

#include "../../network/server_local.h"
#include "../../network/client_interface.h"
#include "../../network/server_interface.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

void item_minecart_spawn_position(const struct block_info* rail, vec3 out) {
	assert(rail && out);
	/* ItemMinecart passes railY+.5 to EntityMinecart's coordinate
	 * constructor; that constructor adds yOffset (.7/2). This engine's spawn
	 * path accepts the final logical Entity.pos directly. */
	out[0] = rail->x + 0.5F;
	out[1] = rail->y + 0.5F + 0.35F;
	out[2] = rail->z + 0.5F;
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
                        struct block_info* where, struct block_info* on,
                        enum side on_side)
{
    // Only place on rails. Beta accepts ANY rail block -- BlockRail.isRailBlock
    // is rail OR powered rail OR detector rail, and CaveWii registers all three
    // (blocks.c id 28, craftable via recipe.c). Cart TRAVERSAL of the detector
    // rail is still the engine TODO; placement parity is not.
    // [740c583: ItemMinecart.onItemUse; BlockRail.isRailBlock]
    if (!on
        || (on->block->type != BLOCK_RAIL
            && on->block->type != BLOCK_POWERED_RAIL
            && on->block->type != BLOCK_DETECTOR_RAIL))
        return false;

    // compute spawn position: center of rail
	vec3 pos;
	item_minecart_spawn_position(on, pos);

    // server side: spawn a minecart entity
    // Beta ItemMinecart.onItemUse queries and constructs in the supplied
    // World. Consume only after the owning-world spawn succeeds.
    return server_local_spawn_minecart_type_w(pos,
        server_local_world_of(s, actor), 0) != NULL;
}

struct item item_minecart = {
    .name         = "Minecart",
    .has_damage   = false,
    .max_stack    = 1, // vehicles don't stack in Beta 1.7.3
    .fuel         = 0,
    .renderItem   = render_item_flat,
    .onItemPlace  = onItemPlace,
    .armor.is_armor = false,
    .tool.type      = TOOL_TYPE_ANY,
    .render_data = {
        .item = {
            .texture_x = 7,
            .texture_y = 8,
        },
    },
};
