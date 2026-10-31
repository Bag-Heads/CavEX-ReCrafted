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

#include "../../block/blocks.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../../network/client_interface.h"
#include "../../network/server_interface.h"
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/server_world.h"
#include "../../util.h"
#include "../../worldgen/tree_runtime.h"

// Beta ItemDye --stackSize. `it` is the REAL held stack (the dispatch does not
// auto-consume the onUseOnBlock path), so consume one here, clearing the slot at 0.
static void consume_one(struct item_data* it) {
	if(it->count > 0 && --it->count == 0) {
		it->id = 0;
		it->durability = 0;
	}
}

// Beta 1.7.3 ItemDye.onItemUse grass-block scatter: 128 attempts, each a short
// random walk up from the clicked block, planting tall grass (9/10) / dandelion /
// rose on air that sits on a grass block. Beta draws from the static Item.itemRand
// (a time-seeded Java LCG -> never reproducible across sessions); CaveWii draws
// from the runtime xorshift RNG (s->rand_src) -- the SAME distribution + draw
// structure, not the same VALUES (which Beta's itemRand can't pin either). The
// algorithm/guards/distribution are pinned deterministically in test_bonemeal.
static void scatter_tall_grass(struct server_local* s, int ox, int oy, int oz) {
	struct random_gen* g = &s->rand_src;
	for(int n = 0; n < 128; n++) {
		int x = ox, y = oy + 1, z = oz;
		bool blocked = false;
		// Random walk of n/16 steps; longer walks (later n) reach further out.
		for(int step = 0; step < n / 16; step++) {
			// Draw order is load-bearing (x, then the two y draws, then z); each
			// is its own statement so C cannot reorder the side effects.
			x += rand_gen_int(g, 3) - 1;
			int yd1 = rand_gen_int(g, 3) - 1;
			int yd2 = rand_gen_int(g, 3);
			y += yd1 * yd2 / 2; // (nextInt(3)-1)*nextInt(3)/2, trunc toward 0 == Java
			z += rand_gen_int(g, 3) - 1;

			struct block_data below, here;
			uint8_t below_t = server_world_get_block(block_world(s), x, y - 1, z, &below)
				? below.type
				: BLOCK_AIR;
			uint8_t here_t = server_world_get_block(block_world(s), x, y, z, &here)
				? here.type
				: BLOCK_AIR;
			if(below_t != BLOCK_GRASS || block_is_normal_cube(here_t)) {
				blocked = true; // Beta: `continue label53` -> skip this whole attempt
				break;
			}
		}
		if(blocked)
			continue;

		struct block_data target;
		uint8_t target_t = server_world_get_block(block_world(s), x, y, z, &target)
			? target.type
			: BLOCK_AIR;
		if(target_t != BLOCK_AIR)
			continue;

		if(rand_gen_int(g, 10) != 0) {
			server_world_set_block(
				block_world(s), x, y, z,
				(struct block_data) {.type = BLOCK_TALL_GRASS, .metadata = 1});
		} else if(rand_gen_int(g, 3) != 0) {
			server_world_set_block(
				block_world(s), x, y, z,
				(struct block_data) {.type = BLOCK_FLOWER, .metadata = 0});
		} else {
			server_world_set_block(
				block_world(s), x, y, z,
				(struct block_data) {.type = BLOCK_ROSE, .metadata = 0});
		}
	}
}

// Beta 1.7.3 ItemDye.onItemUse: white dye (damage value 15) is BONE MEAL. The
// clicked block dispatches by id, then one is consumed (Beta --stackSize, even on
// a no-grow result like a full crop):
//   - CROPS    -> BlockCrops.fertilize == setBlockMetadataWithNotify(x,y,z,7);
//   - SAPLING  -> BlockSapling.growTree (server_grow_tree, faithful WorldGen* trees);
//   - GRASS    -> the 128-attempt tall-grass / flower scatter above.
// Any other block (or a non-white dye) is a no-op, consuming nothing.
// [decompile 740c583 ItemDye.onItemUse, BlockCrops.fertilize, BlockSapling.growTree]
static bool onUseOnBlock(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* on, enum side on_side) {
	(void)on_side;
	if(!on || !on->block)
		return false;
	if(it->durability != 15) // dye damage 15 == white == bone meal (other colours: no-op)
		return false;

	switch(on->block->type) {
		case BLOCK_CROPS:
			server_world_set_block(
				block_world(s), on->x, on->y, on->z,
				(struct block_data) {.type = BLOCK_CROPS, .metadata = 7});
			consume_one(it);
			return true;

		case BLOCK_SAPLING:
			// Beta growTree(world, x, y, z, world.rand); --stackSize. growTree is
			// void (consumes even when the tree fails to fit), so consume always.
			server_grow_tree(s, on->x, on->y, on->z, on->block->metadata & 3);
			consume_one(it);
			return true;

		case BLOCK_GRASS:
			consume_one(it); // Beta --stackSize happens before the scatter loop
			scatter_tall_grass(s, on->x, on->y, on->z);
			return true;

		default:
			return false;
	}
}

struct item item_dye = {
	.name = "Ink Sac",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onUseOnBlock = onUseOnBlock,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 14,
			.texture_y = 4,
		},
	},
};
