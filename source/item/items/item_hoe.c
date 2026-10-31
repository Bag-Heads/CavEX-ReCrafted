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
#include "../../item/items.h" // direct include (was transitive via blocks.h pre-split)
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/server_world.h"
#include "../../platform/audio.h"

// Beta 1.7.3 ItemHoe.onItemUse: till grass/dirt into farmland (Block.tilledField). The
// decompile gate `(side==0 || above!=0 || block!=grass) && block!=dirt` returns false (no
// till) -- so it tills when block==DIRT (unconditionally; the `&& block!=dirt` short-circuits
// the rest) OR block==GRASS with side!=bottom AND the block above == air. Beta side 0 is the
// BOTTOM face; our SIDE_BOTTOM is the equivalent. The hoe is NOT consumed (the dispatch does
// not auto-consume the onUseOnBlock path).
//
// DURABILITY WIRED 2026-07-29. This comment used to say the damageItem(1) was "deferred to
// that future system" because no tool degraded anywhere; handoff P1(b) built that system, so
// the deferral is discharged here rather than left as a stale note. Beta ItemHoe.onItemUse:
//     var3.setBlockWithNotify(var4, var5, var6, var10.blockID);
//     var1.damageItem(1, var2);
// -- wear on the SUCCESS path only (inside the !multiplayerWorld, i.e. server-authoritative
// branch, after the till commits), so every `return false` above costs nothing. NOTE the hoe
// does NOT wear on block-break or melee: ItemHoe extends Item and overrides neither
// onBlockDestroyed nor hitEntity, which is why item_wear_on_block_break/-_hit_entity return 0
// for TOOL_TYPE_HOE. Tilling is the hoe's ONLY wear path.
// [decompile 740c583 ItemHoe.onItemUse]
bool item_hoe_onUseOnBlock(struct server_local* s, struct server_player* actor, struct item_data* it,
						   struct block_info* on, enum side on_side) {
	if(!on || !on->block)
		return false;

	uint8_t block = on->block->type;
	bool is_dirt = block == BLOCK_DIRT;
	bool is_grass = block == BLOCK_GRASS;
	if(!is_dirt && !is_grass)
		return false;

	if(is_grass) {
		// grass: not from the bottom face, and the cell above must be air.
		if(on_side == SIDE_BOTTOM)
			return false;
		struct block_data above;
		if(!server_world_get_block(block_world(s), on->x, on->y + 1, on->z, &above)
		   || above.type != BLOCK_AIR)
			return false;
	}

	// Beta server ItemHoe.onItemUse:38-44: sound BEFORE tilling and wear;
	// the farmland STEP name, (volume+1)/2, pitch*0.8, no RNG draws.
	server_local_emit_sound_w(block_world(s),
		audio_block_step_sound_id(BLOCK_FARMLAND),
		(vec3) {on->x + 0.5F, on->y + 0.5F, on->z + 0.5F},
		(audio_block_sound_volume(BLOCK_FARMLAND) + 1.0F) / 2.0F,
		audio_block_sound_pitch(BLOCK_FARMLAND) * 0.8F);
	server_world_set_block(
		block_world(s), on->x, on->y, on->z,
		(struct block_data) {.type = BLOCK_FARMLAND, .metadata = 0});
	// Beta order: setBlockWithNotify FIRST, then damageItem(1). The dispatch that called us
	// mirrors the mutated slot back with CRPC_INVENTORY_SLOT (the flint & steel precedent),
	// so the durability bar updates on the client.
	item_stack_damage(it, 1);
	return true;
}
