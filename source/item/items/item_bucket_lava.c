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
#include "../../block/blocks.h"
#include "../../block/fluid_block.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static bool onUseOnBlock(struct server_local* s, struct server_player* actor, struct item_data* it,
                         struct block_info* on, enum side on_side)
{
    if (!s || !it || !on || !on->block) return false;

    // Cell adjacent to the clicked face, via the canonical blocks_side_offset (the old
    // hand-rolled switch had SIDE_FRONT/SIDE_BACK swapped vs it -- z-axis clicks placed the
    // fluid on the wrong face; same bug fixed in flint & steel + item_bucket.c).
    int dx, dy, dz;
    blocks_side_offset(on_side, &dx, &dy, &dz);
    int x = on->x + dx, y = on->y + dy, z = on->z + dz;

    // ItemBucket accepts air or any non-solid Beta material. Replacement is a
    // normal setBlockAndMetadataWithNotify and does not drop the displaced block.
    struct block_data bd;
    if (!server_world_get_block(block_world(s), x, y, z, &bd)) return false;
    if (bd.type != BLOCK_AIR && fluid_block_material_is_solid_id(bd.type))
        return false;

    // Place flowing lava (id 10) so it spreads via onWorldTick; it settles to
    // still (id 11) once stable. Reuse the AIR cell's light values.
    if(!server_world_set_block(block_world(s), x, y, z, (struct block_data){
        .type        = BLOCK_LAVA_FLOW,
        .metadata    = 0,
        .sky_light   = bd.sky_light,
        .torch_light = bd.torch_light,
    }))
		return false;

    const struct item_data new_it = { .id = ITEM_BUCKET, .count = 1, .durability = 0 };
    *it = new_it;
	return true;
}

struct item item_bucket_lava = {
	.name = "Lava Bucket",
	.has_damage = false,  // Disable damage (it won't be used up anymore)
	.max_damage = 0,      // Set max_damage to 0, so it doesn't get used up
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onUseOnBlock = onUseOnBlock,
	.container_item = ITEM_BUCKET, // Beta Item.java:345 setContainerItem(bucketEmpty)
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 12,
			.texture_y = 4,
		},
	},
};
