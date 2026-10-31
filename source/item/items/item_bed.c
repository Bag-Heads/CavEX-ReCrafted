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

#include "../../block/bed_meta.h"
#include "../../block/blocks_data.h"
#include "../../cglm/types.h"
#include "../../entity/entity.h"
#include "../../graphics/render_item.h"
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/server_world.h"
#include "../items.h"
#include "../tool.h"

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
                        struct block_info* where, struct block_info* on,
                        enum side on_side) {
    // [decompile 740c583] ItemBed.onItemUse: `if (var7 != 1) return false;`
    // -- a bed is only placeable via a TOP-face click (independently
    // confirmed by mc173's use_bed_stack: `if face != Face::PosY { return
    // false; }`).
    if(on_side != SIDE_TOP)
        return false;

    // [decompile 740c583] var9 = floor(rotationYaw*4.0F/360.0F + 0.5D) & 3
    // -- NO +180 here (unlike ItemDoor): the head lands in FRONT of the
    // player. player.rx maps 1:1 onto rotationYaw (see item_repeater.c's
    // sibling port of the identical inner formula).
    int beta_dir = ((int)floorf(actor->rx * 4.0F / 360.0F + 0.5F)) & 3;

    // [decompile 740c583] BlockBed.headBlockToFootBlockMap per beta_dir:
    // 0->(dx=0,dz=+1) +Z, 1->(dx=-1,dz=0) -X, 2->(dx=0,dz=-1) -Z,
    // 3->(dx=+1,dz=0) +X. PAR-BED-META: the metadata IS var9 (foot) and
    // var9 + 8 (head), Beta's own layout (block/bed_meta.h) -- the same
    // bytes a real Beta save or server chunk carries.
    int dx = bed_meta_dx((uint8_t)beta_dir), dz = bed_meta_dz((uint8_t)beta_dir);

    int fx = where->x, fy = where->y, fz = where->z; // foot (== on + TOP offset)
    int hx = fx + dx, hy = fy, hz = fz + dz;          // head

    // [decompile 740c583]: both cells air, both supported by a normal cube
    // below -- NO fallback search across directions like the old code;
    // Beta tries exactly the yaw-derived direction and refuses outright if
    // it doesn't fit (no drop, no partial placement).
    struct block_data foot_cell, head_cell, foot_below, head_below;
    if(!server_world_get_block(block_world(s), fx, fy, fz, &foot_cell)
       || foot_cell.type != BLOCK_AIR)
        return false;
    if(!server_world_get_block(block_world(s), hx, hy, hz, &head_cell)
       || head_cell.type != BLOCK_AIR)
        return false;
    if(!server_world_get_block(block_world(s), fx, fy - 1, fz, &foot_below)
       || !block_is_normal_cube(foot_below.type))
        return false;
    if(!server_world_get_block(block_world(s), hx, hy - 1, hz, &head_below)
       || !block_is_normal_cube(head_below.type))
        return false;

    struct block_data foot = {
        .type = BLOCK_BED,
        .metadata = bed_meta_from_yaw_index(beta_dir, false), // var9
        .sky_light = 0,
        .torch_light = 0,
    };
    struct block_data head = {
        .type = BLOCK_BED,
        .metadata = bed_meta_from_yaw_index(beta_dir, true), // var9 + 8
        .sky_light = 0,
        .torch_light = 0,
    };

    // Collision check, BOTH bed cells vs player + all entities (Beta gate --
    // the head cell was previously unchecked, so a bed head could embed a mob).
    struct block_info foot_info = *where;
    foot_info.block = &foot;
    if (block_place_obstructed_for(s, actor, &foot_info))
        return false;
    struct block_info head_info = *where;
    head_info.block = &head;
    head_info.x = hx;
    head_info.z = hz;
    if (block_place_obstructed_for(s, actor, &head_info))
        return false;

    // Place foot
    server_world_set_block(block_world(s), fx, fy, fz, foot);
    // Place head
    server_world_set_block(block_world(s), hx, hy, hz, head);

    return true;
}

struct item item_bed = {
    .name = "Bed",
    .has_damage = false,
    .max_stack = 1,
    .fuel = 0,
    .renderItem = render_item_flat,
    .onItemPlace = onItemPlace,
    .armor.is_armor = false,
    .tool.type = TOOL_TYPE_ANY,
    .render_data = {
        .item = {
            .texture_x = 13,
            .texture_y = 2,
        },
    },
};
