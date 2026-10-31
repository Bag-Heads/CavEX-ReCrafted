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
#include "../../entity/entity_primed_tnt.h"
#include "../../platform/audio.h" /* S2: fire.ignite */
#include "../../util.h"			  /* rand_gen_flt for the ignite pitch roll */
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

// Beta 1.7.3 ItemFlintAndSteel.onItemUse: light fire in the cell adjacent to the clicked
// face if that cell is air. Beta does NOT gate on the clicked block being flammable -- you
// can light a (transient) fire on any face; the fire block's tick (block_fire.c) extinguishes
// it if there's no fuel and no solid support. Every valid click damages the real
// held stack once even when the adjacent cell is occupied; maxDamage=64 breaks
// on use 65 because ItemStack.damageItem compares strictly greater-than.
// [decompile 740c583 ItemFlintAndSteel.onItemUse]
static bool onUseOnBlock(struct server_local* s, struct server_player* actor, struct item_data* it,
						   struct block_info* on, enum side on_side) {
	if (!on || !on->block) return false;

	/* Controller playability adaptation: Beta primes TNT when flint & steel is
	 * held during the left-click break path (BlockTNT.onBlockClicked stamps
	 * metadata 1 before onBlockDestroyedByPlayer). Wii use is right-click, so
	 * preserve the established direct ignition here in the ITEM layer. TNT's
	 * blockActivated itself remains false, allowing every non-flint item and
	 * adjacent block placement to follow PlayerController.sendPlaceBlock. */
	if(on->block->type == BLOCK_TNT) {
		if(server_local_prime_tnt_block(
			   s, on->x, on->y, on->z, TNT_FUSE_DEFAULT))
			item_stack_damage(it, 1);
		return true;
	}

	// Cell adjacent to the clicked face. Use the canonical blocks_side_offset (the SAME mapping
	// the dispatch used to build `where` and that every block placer uses) -- the old hand-rolled
	// switch had SIDE_FRONT/SIDE_BACK swapped vs it, so z-axis clicks lit fire on the wrong face.
	int dx, dy, dz;
	blocks_side_offset(on_side, &dx, &dy, &dz);
	int tx = on->x + dx;
	int ty = on->y + dy;
	int tz = on->z + dz;

	struct block_data blk;
	if (server_world_get_block(block_world(s), tx, ty, tz, &blk)
		&& blk.type == BLOCK_AIR) {
		// S2: Beta plays "fire.ignite" 1.0F, rand*0.4F+0.8F at the target
		// cell center, just BEFORE setBlockWithNotify(fire) -- and only on
		// the air-target branch (a blocked click stays silent). This covers
		// portal LIGHTING too: the portal fill itself (BlockFire.onBlockAdded
		// -> tryToCreatePortal, dispatched from this very set_block) is
		// silent in b1.7.3. [740c583: ItemFlintAndSteel.onItemUse:35-40]
		server_local_emit_sound(
			SOUND_FIRE_IGNITE, (vec3) {tx + 0.5F, ty + 0.5F, tz + 0.5F}, 1.0F,
			rand_gen_flt(server_local_sound_rng()) * 0.4F + 0.8F);
		server_world_set_block(block_world(s), tx, ty, tz, (struct block_data) {
			.type = BLOCK_FIRE,
			.metadata = 0,
			.sky_light = 0,
			.torch_light = 15,
		});
	}

	item_stack_damage(it, 1);
	return true;
}

struct item item_flint_steel = {
	.name = "Flint and Steel",
	.has_damage = true,
	.max_damage = 64,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onUseOnBlock = onUseOnBlock,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 5,
			.texture_y = 0,
		},
	},
};
