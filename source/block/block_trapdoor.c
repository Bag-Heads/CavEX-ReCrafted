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

// FIXED: the redstone open/close path (BlockTrapDoor.onNeighborBlockChange) was dead --
// every dispatch site calls onNeighbourBlockChange with block_info.neighbours == NULL
// (redstone_server.c:121, blocks.c:392, server_world.c:964), so the old `if
// (!info->neighbours) return;` guard always fired. Rewritten below to read live world
// state directly (the block_tnt.c pattern), like Beta's own method does.
//todo; fix rendering of the door in various metadata positions

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/audio.h" // S0: trapdoor aux-1003 one-shots
#include "../util.h"		   // rand_gen_flt for the aux-1003 rolls
#include "blocks.h"
#include "redstone_power.h"	   // redstone_id_can_provide_power
#include "redstone_server.h"   // redstone_server_indirectly_getting_powered

// S0 aux-1003 (shared by the hand toggle + the redstone flip): BlockTrapDoor
// fires the SAME 1003 event as doors -- open-vs-close FILE by 50/50 roll,
// NOT by the new state; vol 1.0F, pitch rand*0.1F + 0.9F, at the cell.
// Server-side rolls (thin-client doctrine). [740c583: BlockTrapDoor.java:
// 80,90 -> RenderGlobal.playAuxSFX case 1003]
static void trapdoor_emit_toggle_sound(w_coord_t x, w_coord_t y, w_coord_t z) {
	struct random_gen* rng = server_local_sound_rng();
	uint16_t sid = rand_gen_flt(rng) < 0.5F ? SOUND_RANDOM_DOOR_OPEN
											: SOUND_RANDOM_DOOR_CLOSE;
	server_local_emit_sound(sid, (vec3) {x + 0.5F, y + 0.5F, z + 0.5F}, 1.0F,
							rand_gen_flt(rng) * 0.1F + 0.9F);
}

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_WOOD;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x) {
		if(this->block->metadata & 0x04) {
			switch(this->block->metadata & 0x03) {
				case 0:
					aabb_setsize(x, 1.0F, 1.0F, 0.1875F);
					aabb_translate(x, 0, 0, 0.40625F);
					break;
				case 1:
					aabb_setsize(x, 1.0F, 1.0F, 0.1875F);
					aabb_translate(x, 0, 0, -0.40625F);
					break;
				case 2:
					aabb_setsize(x, 0.1875F, 1.0F, 1.0F);
					aabb_translate(x, 0.40625F, 0, 0);
					break;
				case 3:
					aabb_setsize(x, 0.1875F, 1.0F, 1.0F);
					aabb_translate(x, -0.40625F, 0, 0);
					break;
			}
		} else {
			aabb_setsize(x, 1.0F, 0.1875F, 1.0F);
		}
	}

	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	if(this->block->metadata & 0x04) {
		// could be improved
		return face_occlusion_empty();
	} else {
		switch(side) {
			case SIDE_TOP: return face_occlusion_empty();
			case SIDE_BOTTOM:
				return (it->block->type == this->block->type) ?
					face_occlusion_full() :
					face_occlusion_empty();
			default: return face_occlusion_rect(3);
		}
	}
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_TRAPDOOR);
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// Beta BlockTrapDoor.canPlaceBlockOnSide: no vertical attachment, and
	// the side attachment must be World.isBlockNormalCube.
	if(on_side == SIDE_TOP || on_side == SIDE_BOTTOM
	   || !block_is_normal_cube(on->block->type))
		return false;

	int metadata = 0;
	switch(on_side) {
		case SIDE_LEFT: metadata = 2; break;
		case SIDE_RIGHT: metadata = 3; break;
		case SIDE_BACK: metadata = 1; break;
		default: break;
	}

	struct block_data blk = (struct block_data) {
		.type = it->id,
		.metadata = metadata,
		.sky_light = 0,
		.torch_light = 0,
	};

	struct block_info blk_info = *where;
	blk_info.block = &blk;

	if(block_place_obstructed_for(s, actor, &blk_info)) // player + all entities (Beta gate)
		return false;

	server_world_set_block(block_world(s), where->x, where->y, where->z, blk);
	return true;
}


// [740c583 BlockTrapDoor.onNeighborBlockChange]:
//   the hinge side is the low two metadata bits;
//   the support cell starts at (x, z) and steps by side:
//   side 0 -> z+1,
//   side 1 -> z-1,
//   side 2 -> x+1,
//   side 3 -> x-1.
//   No normal cube at that support cell: the trapdoor becomes air and drops.
//   A non-air neighbour id that canProvidePower then re-reads the power:
//       onPoweredBlockChange(x,y,z, isBlockIndirectlyGettingPowered(x,y,z));
// The 4 facing offsets above match this file's own getBoundingBox open-panel
// translations (meta&3: 0=+Z, 1=-Z, 2=+X, 3=-X), so both agree on which wall a
// given facing hangs from. isBlockNormalCube's C proxy is the same
// "blocks[]-registered and not can_see_through" predicate block_ladder.c's own
// onNeighbourBlockChange already uses for its identical support-drop.
//
// DEVIATION (documented, not a shortcut): Beta re-reads getBlockMetadata/
// setBlockMetadataWithNotify on (x,y,z) UNCONDITIONALLY after the drop check,
// even when the cell was JUST set to air -- at that point it is silently poking
// a meaningless metadata nibble on an air block (air has no rendered state) and,
// as a side effect of AIR not being flagged in setNeighborNotifyOnMetadataChange,
// fires an extra notifyBlocksOfNeighborChange sourced from block id 0. This
// engine's block_info carries a captured `struct block_data` snapshot, not a
// live re-read, so replicating that literally would mean writing the STALE
// trapdoor type back over the air cell we just cleared -- a real dupe/undo bug,
// not a faithful port of an inert Beta quirk. We `return` after the drop
// instead: no other block here or in mc173 (block_entity/redstone modules)
// distinguishes an air-sourced neighbour notify from none at all, so this has
// no observable simulation effect.
static void onNeighbourBlockChange(struct server_local* s,
									struct block_info* info) {
	uint8_t meta = info->block->metadata;
	uint8_t facing = meta & 0x03;

	w_coord_t nx = info->x, nz = info->z;
	if(facing == 0) nz += 1;
	if(facing == 1) nz -= 1;
	if(facing == 2) nx += 1;
	if(facing == 3) nx -= 1;

	struct block_data nb;
	bool supported = server_world_get_block(block_world(s), nx, info->y, nz, &nb)
		&& blocks[nb.type] && !blocks[nb.type]->can_see_through;

	if(!supported) {
		server_local_spawn_block_drops(s, info);
		server_world_set_block(block_world(s), info->x, info->y, info->z,
							   (struct block_data) {.type = BLOCK_AIR,
													 .metadata = 0});
		return;
	}

	// Beta gates the power recheck on the notifying neighbour actually being
	// able to provide power (Block.blocksList[var5].canProvidePower()).
	uint8_t origin = server_world_notify_origin();
	if(origin > 0 && redstone_id_can_provide_power(origin)) {
		// World.isBlockIndirectlyGettingPowered -- the SAME wrapper
		// block_door.c/block_dispenser.c/block_rail.c already use for their
		// own onNeighborBlockChange power reads.
		bool powered = redstone_server_indirectly_getting_powered(
			s, info->x, info->y, info->z);
		bool was_open = (meta & 0x04) != 0;
		// onPoweredBlockChange's own gate: only act on a REAL state change.
		if(was_open != powered) {
			struct block_data cur = *info->block;
			cur.metadata = meta ^ 0x04;
			server_world_set_block(block_world(s), info->x, info->y, info->z, cur);
			trapdoor_emit_toggle_sound(info->x, info->y, info->z); // S0 aux-1003
			// No notifyNeighbours here: Beta's onPoweredBlockChange only calls
			// setBlockMetadataWithNotify, and BlockTrapDoor is registered with
			// disableNeighborNotifyOnMetadataChange() (Block.java:688), which
			// routes that call to the light render-only notifyBlockChange, NOT
			// notifyBlocksOfNeighborChange -- unlike lever/button, whose
			// onBlockActivated makes an explicit extra notify call that
			// BlockTrapDoor's onPoweredBlockChange does not.
		}
	}
}


static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
                         struct block_info* where, struct block_info* on,
                         enum side on_side) {
    struct block_data cur = *on->block;
    cur.metadata ^= 0x04;
    server_world_set_block(block_world(s),
                           on->x, on->y, on->z,
                           cur);
    // The hand toggle always flips (meta ^ 4), so Beta fires 1003
    // unconditionally here [740c583: BlockTrapDoor.blockActivated:80].
    trapdoor_emit_toggle_sound(on->x, on->y, on->z); // S0 aux-1003
}

// R1: Beta BlockTrapDoor.onBlockClicked is `this.blockActivated(...)` -- a LEFT-click flips the
// wooden trapdoor exactly like a right-click (meta ^ 4, aux-1003 sound). The held item is
// irrelevant. [740c583: BlockTrapDoor.java:70-72]
static void onBlockClicked(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this) {
	(void)held;
	onRightClick(s, actor, NULL, this, this, SIDE_TOP);
}

struct block block_trapdoor = {
	.name = "Trapdoor",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.onBlockClicked = onBlockClicked,
	.transparent = false,
	.renderBlock = render_block_trapdoor,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 1,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 4500, // TODO: might not be correct
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.fuel = 1,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,

	},
};
