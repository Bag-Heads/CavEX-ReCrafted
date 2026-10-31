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
#include "../../platform/audio.h" /* S2: the nether-evap random.fizz */
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

// ItemBucket is a use-on-clicked-block action, not ordinary block placement:
// it must run even when the adjacent cell contains a replaceable non-solid.
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

    // Beta accepts air OR any block whose Material.isSolid() is false (plants,
    // circuits, snow layer, liquids, fire, portal, ...). The replacement goes
    // through the ordinary block lifecycle; it does not drop the displaced block.
    struct block_data bd;
    if (!server_world_get_block(block_world(s), x, y, z, &bd)) return false;
    if (bd.type != BLOCK_AIR && fluid_block_material_is_solid_id(bd.type))
        return false;

    if (block_world(s)->dimension == WORLD_DIM_NETHER) {
        // Beta ItemBucket.onItemRightClick: in the nether (worldProvider.isHellWorld) a WATER
        // bucket VAPORIZES -- no block is placed, but the bucket still empties (the shared
        // swap-to-empty below runs either way). Only water: lava places normally (the check is
        // `isFull == Block.waterMoving.blockID`). S2: the "random.fizz" 0.5F at
        // 2.6F+(r1-r2)*0.8F now plays from EXACTLY the two rand_src draws that always
        // preserved Beta's stream alignment here (the ONLY bucket-related sound in b1.7.3 --
        // fill/place are silent). The 8 largesmoke particles stay deferred (no ad-hoc smoke
        // wired here; sim contract unchanged: no water in the nether, bucket consumed).
        // [decompile 740c583 ItemBucket.onItemRightClick isHellWorld branch]
		float fr1 = rand_gen_flt(&s->rand_src);
		float fr2 = rand_gen_flt(&s->rand_src);
		/* Beta plays the fizz at the PLAYER's ray origin + 0.5 (var7, var9 =
		 * eye, var11) [ItemBucket.java:81], not at the target cell. */
		vec3 fizz_pos = {x + 0.5F, y + 0.5F, z + 0.5F};
		if(actor && actor->has_pos) {
			fizz_pos[0] = (float)(actor->x + 0.5);
			fizz_pos[1] = (float)(actor->y + 0.5);
			fizz_pos[2] = (float)(actor->z + 0.5);
		}
		server_local_emit_sound(SOUND_RANDOM_FIZZ, fizz_pos, 0.5F,
								2.6F + (fr1 - fr2) * 0.8F);
		/* Eight particles each draw Math.random for X, Y and Z. The server does
		 * not render them, but later entity launch physics shares this stream. */
		server_local_math_random_discard(24);
    } else {
        // Place flowing water (id 8) so it spreads via onWorldTick; once stable it
        // settles itself back to still (id 9). Reuse the AIR cell's light values.
        if(!server_world_set_block(block_world(s), x, y, z, (struct block_data){
            .type        = BLOCK_WATER_FLOW,
            .metadata    = 0,
            .sky_light   = bd.sky_light,
            .torch_light = bd.torch_light,
        }))
			return false;
    }

    const struct item_data new_it = { .id = ITEM_BUCKET, .count = 1, .durability = 0 };
    *it = new_it;
	return true;
}

struct item item_bucket_water = {
	.name = "Water Bucket",
	.has_damage = false,  // Disable damage (it won't be used up anymore)
	.max_damage = 0,      // Set max_damage to 0, so it doesn't get used up
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onUseOnBlock = onUseOnBlock,
	.container_item = ITEM_BUCKET, // Beta Item.java:344 setContainerItem(bucketEmpty)
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 11,
			.texture_y = 4,
		},
	},
};
