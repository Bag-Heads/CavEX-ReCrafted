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

#include <math.h>

#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// [decompile 740c583] ItemDoor.onItemUse refuses any side but 1:
	// -- that is the clicked side; Beta only places a door from a click on
	// the TOP face of the block below (this port's SIDE_TOP is the side
	// that yields `where` == on+(0,1,0), the same cell Beta's Y increment
	// produces). Independently corroborated by mc173's use_door_stack:
	// `if face != Face::PosY { return false; }`.
	if(on_side != SIDE_TOP)
		return false;

	// [decompile 740c583] ItemDoor.onItemUse -> BlockDoor.canPlaceBlockAt
	// (BlockDoor.java:185): `var3 >= 127 ? false : isBlockNormalCube(x, y-1, z)
	// && super.canPlaceBlockAt(x, y, z) && super.canPlaceBlockAt(x, y+1, z)`.
	// The pre-fix code checked only the two cells exist and the UPPER one is
	// air; it never refused the roof, never rejected a non-replaceable upper
	// cell, and never required the support below to be a normal cube.
	if(where->y >= 127)
		return false;
	struct block_data blk;
	// isBlockNormalCube(x, y-1, z): the support must be a full opaque solid.
	if(!server_world_get_block(block_world(s), where->x, where->y - 1, where->z, &blk))
		return false;
	if(!block_is_normal_cube(blk.type))
		return false;
	// super.canPlaceBlockAt(y) and (y+1): BOTH door cells must be air or a
	// replaceable-material block (Block.canPlaceBlockAt:444-447,
	// `id == 0 || blocksList[id].blockMaterial.func_27090_g()`), which this
	// engine spells `place_ignore` (the same predicate the generic place
	// dispatch applies, and the idiom item_sign_post.c already re-applies so
	// the function stays correct when called directly). A missing block entry
	// (id 0) is air.
	if(where->block && where->block->type != 0 && blocks[where->block->type]
	   && !blocks[where->block->type]->place_ignore)
		return false;
	if(!server_world_get_block(block_world(s), where->x, where->y + 1, where->z, &blk))
		return false;
	if(blk.type != 0 && blocks[blk.type] && !blocks[blk.type]->place_ignore)
		return false;
	if(!blocks[on->block->type]) return false;

	// [decompile 740c583] ItemDoor.onItemUse: `var9 = floor((rotationYaw +
	// 180.0F) * 4.0F / 360.0F - 0.5D) & 3` -- the door's default facing is
	// AWAY from the player (yaw+180), independently corroborated by
	// mc173's `door_face = Face::from_yaw(look.x).opposite()`
	// (world/use.rs). player.rx maps 1:1 onto Beta's rotationYaw (see
	// item_repeater.c's sibling port of a neighbouring onBlockPlacedBy
	// formula for the same idiom).
	int direction
		= ((int)floorf((actor->rx + 180.0F) * 4.0F / 360.0F - 0.5F)) & 3;

	// [decompile 740c583] var10/var11: the unit step for `direction`, used
	// to probe the two horizontal neighbours the door could hinge toward.
	int dx = 0, dz = 0;
	switch(direction) {
		case 0: dz = 1; break;
		case 1: dx = -1; break;
		case 2: dz = -1; break;
		case 3: dx = 1; break;
	}

	// [decompile 740c583] normal-cube count on each side
	// (both door cells); then whether there is already a same-type door on
	// that side. Flip: mirror an adjacent door first, else swing
	// toward the side with more solid neighbours (so the door opens into
	// the wall, not out into open air).
	struct block_data back_lo, back_hi, front_lo, front_hi;
	bool has_back_lo = server_world_get_block(block_world(s), where->x - dx,
											  where->y, where->z - dz,
											  &back_lo);
	bool has_back_hi = server_world_get_block(block_world(s), where->x - dx,
											  where->y + 1, where->z - dz,
											  &back_hi);
	bool has_front_lo = server_world_get_block(block_world(s), where->x + dx,
											   where->y, where->z + dz,
											   &front_lo);
	bool has_front_hi = server_world_get_block(block_world(s), where->x + dx,
											   where->y + 1, where->z + dz,
											   &front_hi);

	int back_solid = (has_back_lo && block_is_normal_cube(back_lo.type))
		+ (has_back_hi && block_is_normal_cube(back_hi.type));
	int front_solid = (has_front_lo && block_is_normal_cube(front_lo.type))
		+ (has_front_hi && block_is_normal_cube(front_hi.type));
	bool back_is_door = (has_back_lo && back_lo.type == BLOCK_DOOR_IRON)
		|| (has_back_hi && back_hi.type == BLOCK_DOOR_IRON);
	bool front_is_door = (has_front_lo && front_lo.type == BLOCK_DOOR_IRON)
		|| (has_front_hi && front_hi.type == BLOCK_DOOR_IRON);

	bool flip = false;
	if(back_is_door && !front_is_door)
		flip = true;
	else if(front_solid > back_solid)
		flip = true;

	int metadata = direction;
	if(flip) {
		// [decompile 740c583] `var9 = var9 - 1 & 3; var9 += 4;` -- rotate
		// the facing one step and mark the flip via bit 0x04. Beta's own
		// getState()/isOpen() overload this exact bit -- a hinge-flipped
		// door IS placed with 0x04 set; that is Beta's real placement
		// behaviour, independently confirmed by mc173's
		// `block::door::set_open(&mut metadata, true)` in the very same
		// flip branch (world/use.rs) -- NOT an "open on placement" bug.
		metadata = ((direction - 1) & 3) + 4;
	}

	struct block_data blk2 = (struct block_data) {
		.type = BLOCK_DOOR_IRON,
		.metadata = metadata,
		.sky_light = 0,
		.torch_light = 0,
	};

	struct block_info blk2_info = *where;
	blk2_info.block = &blk2;

	// BOTH door cells vs player + all entities (Beta gate). The old second
	// check moved the PLAYER up a block against the same lower cell -- by
	// translation-invariance that tested the cell BELOW the door, not the
	// upper half. Check the actual upper cell instead.
	if(block_place_obstructed_for(s, actor, &blk2_info))
		return false;
	struct block_info upper_info = blk2_info;
	upper_info.y = where->y + 1;
	if(block_place_obstructed_for(s, actor, &upper_info))
		return false;

	server_world_set_block(block_world(s), where->x, where->y, where->z, blk2);

	blk2.metadata |= 8;
	server_world_set_block(block_world(s), where->x, where->y + 1, where->z, blk2);
	return true;
}

struct item item_door_iron = {
	.name = "Iron Door",
	.has_damage = false,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 12,
			.texture_y = 2,
		},
	},
};
