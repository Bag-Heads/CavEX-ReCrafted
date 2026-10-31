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
#include "../../block/redstone_server.h" // redstone_wire_neighbour_change (Beta onBlockAdded self-flood)
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
                        struct block_info* where, struct block_info* on,
                        enum side on_side) {
    if (!on || !on->block) return false;

    // [decompile 740c583] ItemRedstone.onItemUse: unless the clicked block IS
    // a snow layer (Block.snow, id 78 -- replaced in place, like tall
    // grass), the target is the clicked block offset by the clicked SIDE
    // (var7's per-side var4/var5/var6 adjustment) -- exactly `where`, which
    // the generic placement dispatch already computes the same way for
    // every side, not just top -- and that cell must be air.
    int tx, ty, tz;
    if (on->block->type == BLOCK_SNOW) {
        tx = on->x;
        ty = on->y;
        tz = on->z;
    } else {
        tx = where->x;
        ty = where->y;
        tz = where->z;
        struct block_data target;
        if (!server_world_get_block(block_world(s), tx, ty, tz, &target))
            return false;
        if (target.type != BLOCK_AIR)
            return false;
    }

    // [decompile 740c583] BlockRedstoneWire.canPlaceBlockAt:
    // `isBlockNormalCube(x, y - 1, z)` -- a normal solid cube must sit
    // directly below wherever the wire actually lands (re-enabled: this was
    // commented out, and paired with a wrong `can_see_through` substitute).
    struct block_data below;
    if (!server_world_get_block(block_world(s), tx, ty - 1, tz, &below))
        return false;
    if (!block_is_normal_cube(below.type))
        return false;

    server_world_set_block(block_world(s), tx, ty, tz, (struct block_data) {
        .type = BLOCK_REDSTONE_WIRE,
        .metadata = 0,
        .sky_light = 0,
        .torch_light = 0,
    });

    // Beta ItemRedstone.onItemUse -> World.setBlockWithNotify -> BlockRedstoneWire.onBlockAdded ->
    // updateAndPropagateCurrentStrength (decompile 740c583): the freshly placed dust floods at once,
    // reading isBlockIndirectlyGettingPowered so any PRE-EXISTING adjacent provider (lit torch / lever
    // / pressure plate) latches its 15 immediately. server_world_set_block above only notifies the 6
    // NEIGHBOURS, never the placed wire's own flood -- so without this a lone dust dropped beside an
    // already-lit source stayed dark (metadata 0) until an unrelated later neighbour change re-flooded
    // it. This is the same self-flood redstone_wire_place runs for the block's own placer; the dust
    // item path (ITEM_REDSTONE) bypasses that placer, so it must run the flood itself. Locked by
    // tests/test_redstone_wire_place_flood_env.c. (redstone_wire_neighbour_change reads only x/y/z.)
    struct block_info self = {.block = NULL, .neighbours = NULL, .x = tx, .y = ty, .z = tz};
    redstone_wire_neighbour_change(s, &self);

    return true;
}

struct item item_redstone = {
	.name = "Redstone",
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
			.texture_y = 3,
		},
	},
};
