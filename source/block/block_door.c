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

// Beta 1.7.3 BlockDoor (decompile 740c583). Metadata: bits 0-1 = facing, bit 2 (0x04) = OPEN,
// bit 3 (0x08) = top half. Beta has ONE open bit -- a right-click XORs it (blockActivated
// toggles bit 2) and redstone SETS it absolutely (onPoweredBlockChange when the desired state
// differs); the old fork split "manual bit 0x01 / redstone bit 0x04", which contradicted this
// file's own bounding-box math and made redstone and the hand fight each other.
//
// Redstone (onNeighborBlockChange): only when the NOTIFYING block canProvidePower (Beta's
// var5 gate, read via server_world_notify_origin) -- powered = isBlockIndirectlyGettingPowered
// at the bottom cell OR the top cell. Without the source gate, ANY neighbour change would slam
// a hand-opened door shut the moment no power is present.
//todo; fix rendering of the door in various metadata positions


#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/audio.h" // S0: door open/close one-shots
#include "../util.h"		   // rand_gen_flt for the aux-1003 rolls
#include "blocks.h"
#include "redstone_power.h"
#include "redstone_server.h"

static enum block_material getMaterial1(struct block_info* this) {
	return MATERIAL_WOOD;
}

static enum block_material getMaterial2(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x) {
		uint8_t state = ((this->block->metadata & 0x03)
						 + ((this->block->metadata & 0x04) ? 1 : 0))
			% 4;
		switch(state) {
			case 0:
				aabb_setsize(x, 0.1875F, 1.0F, 1.0F);
				aabb_translate(x, -0.40625F, 0, 0);
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
				aabb_setsize(x, 1.0F, 1.0F, 0.1875F);
				aabb_translate(x, 0, 0, 0.40625F);
				break;
		}
	}

	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	// could be improved
	return face_occlusion_empty();
}

static uint8_t getTextureIndex1(struct block_info* this, enum side side) {
	return (this->block->metadata & 0x08) ?
		tex_atlas_lookup(TEXAT_DOOR_WOOD_TOP) :
		tex_atlas_lookup(TEXAT_DOOR_WOOD_BOTTOM);
}

static uint8_t getTextureIndex2(struct block_info* this, enum side side) {
	return (this->block->metadata & 0x08) ?
		tex_atlas_lookup(TEXAT_DOOR_IRON_TOP) :
		tex_atlas_lookup(TEXAT_DOOR_IRON_BOTTOM);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_DOOR_WOOD;
		it->durability = 0;
		it->count = 1;
	}

	//only drop item from bottom half
	if(this->block->metadata < 8) return 1;
	return 0;
}

static size_t getDroppedItem2(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_DOOR_IRON;
		it->durability = 0;
		it->count = 1;
	}

	//only drop item from bottom half
	if(this->block->metadata < 8) return 1;
	return 0;
}

// Set the door's Beta OPEN bit (0x04) on the bottom half at (x,y,z) and sync the top half.
// Shared by the hand toggle (blockActivated toggles bit 2) and redstone (onPoweredBlockChange).
/* `nudge` is the body the swing may push clear (playability carve-out #2
 * below): the ACTING actor for a hand toggle, the host for the actor-less
 * redstone path -- which is what it has always pushed. */
static void door_set_open(struct server_local* s, struct server_player* nudge,
                          w_coord_t x, w_coord_t y, w_coord_t z,
                          uint8_t doorType, uint8_t bottomMeta, bool open)
{
    uint8_t newMeta = (bottomMeta & (uint8_t)~0x04) | (open ? 0x04 : 0x00);
    if (newMeta == bottomMeta)
        return;

    // ——— NUDGE PLAYER IF THE DOOR SWINGS INTO THEM ———
    // Playability carve-out #2 (kept from the original port, now keyed to the REAL open bit):
    // Beta lets the door clip the player; on the Wii that traps them, so push them clear.
    {
        struct block_data test = { .type = doorType, .metadata = newMeta };
        struct block_info di = {
            .x          = x,
            .y          = y,
            .z          = z,
            .block      = &test,
            .neighbours = NULL
        };
        if (nudge && entity_local_player_block_collide(
                (vec3){nudge->x, nudge->y, nudge->z}, &di))
        {
            double dx = nudge->x - (x + 0.5);
            double dz = nudge->z - (z + 0.5);
            if (fabs(dx) > fabs(dz)) {
                double push = (dx > 0.0) ? +0.6 : -0.6;
                nudge->x = x + 0.5 + push;
            } else {
                double push = (dz > 0.0) ? +0.6 : -0.6;
                nudge->z = z + 0.5 + push;
            }
        }
    }
    // ————————————————————————————————————————————

    server_world_set_block(block_world(s), x, y, z,
        (struct block_data){ .type = doorType, .metadata = newMeta });
    struct block_data above;
    if (server_world_get_block(block_world(s), x, y + 1, z, &above)
        && above.type == doorType)
        server_world_set_block(block_world(s), x, y + 1, z,
            (struct block_data){ .type = doorType,
                                 .metadata = (uint8_t)(newMeta | 0x08) });

    // S0 aux-1003 door sound, fired on every REAL state flip (this function
    // is the shared hand+redstone chokepoint; the unchanged-metadata early
    // return above supplies Beta's change gate). FAITHFUL TRAP: the
    // open-vs-close FILE is a 50/50 roll, NOT the door's new state; vol
    // 1.0F, pitch from a uniform roll scaled by 0.1 then offset by 0.9, at the bottom cell center. Both
    // rolls are server-side here (Beta rolled them client-side in the aux
    // handler -- thin-client doctrine moves the roll with the RPC).
    // [740c583: BlockDoor.blockActivated:107 / onPoweredBlockChange:129 ->
    //  RenderGlobal.playAuxSFX case 1003]
    {
        struct random_gen* rng = server_local_sound_rng();
        uint16_t sid = rand_gen_flt(rng) < 0.5F ? SOUND_RANDOM_DOOR_OPEN
                                                : SOUND_RANDOM_DOOR_CLOSE;
        server_local_emit_sound(sid,
                                (vec3){x + 0.5F, y + 0.5F, z + 0.5F}, 1.0F,
                                rand_gen_flt(rng) * 0.1F + 0.9F);
    }
}

static void toggleDoor(struct server_local* s, struct server_player* actor,
                       w_coord_t x, w_coord_t y, w_coord_t z,
                       uint8_t doorType)
{
    struct block_data bd;
    if (!server_world_get_block(block_world(s), x, y, z, &bd)) return;
    // Beta blockActivated verifies the matching door id below before recursing; without this
    // guard a transient orphan TOP half would transmute whatever block sits below it.
    if (bd.type != doorType) return;
    // Beta blockActivated: the hand toggles the ONE open bit, bit 2.
    door_set_open(s, actor, x, y, z, doorType, bd.metadata, (bd.metadata & 0x04) == 0);
}

// Beta BlockDoor.blockActivated returns true immediately for Material.iron: the click is
// CONSUMED with no action (a NULL handler would fall through to the held item's placement).
static void onRightClickIron(struct server_local* s, struct server_player* actor, struct item_data* it,
                             struct block_info* where, struct block_info* on,
                             enum side on_side)
{
    (void)s; (void)it; (void)where; (void)on; (void)on_side;
}

static void onRightClick(struct server_local* s, struct server_player* actor,
                              struct item_data* it,
                              struct block_info* where,
                              struct block_info* on,
                              enum side on_side)
{
    struct block_data cur = *on->block;
    bool topHalf = (cur.metadata & 0x08) != 0;
    w_coord_t bx = on->x;
    w_coord_t by = on->y - (topHalf ? 1 : 0);
    w_coord_t bz = on->z;

    toggleDoor(s, actor, bx, by, bz, cur.type);
}


// Beta BlockDoor.onNeighborBlockChange, bottom-half body. Order transcribed: (1) support
// drops -- top half missing clears the bottom (with the item drop); the block below no longer
// a normal cube clears BOTH halves (with the drop); (2) only when nothing dropped AND the
// notify origin canProvidePower (the Beta var5 gate), recompute powered = indirectly-getting-
// powered at the bottom OR top cell and drive onPoweredBlockChange (flip when state differs --
// this is the Beta quirk where cutting power closes a door that was opened BY HAND).
static void door_bottom_neighbour_change(struct server_local* s, w_coord_t x,
                                         w_coord_t y, w_coord_t z,
                                         struct block_data cur, uint8_t origin)
{
    bool dropped = false;
    struct block_data above;
    bool above_is_door
        = server_world_get_block(block_world(s), x, y + 1, z, &above)
        && above.type == cur.type;
    if (!above_is_door) {
        server_world_set_block(block_world(s), x, y, z,
            (struct block_data){ .type = BLOCK_AIR, .metadata = 0 });
        dropped = true;
    }

    struct block_data below;
    if (!(server_world_get_block(block_world(s), x, y - 1, z, &below)
          && block_is_normal_cube(below.type))) {
        if (!dropped)
            server_world_set_block(block_world(s), x, y, z,
                (struct block_data){ .type = BLOCK_AIR, .metadata = 0 });
        dropped = true;
        if (above_is_door)
            server_world_set_block(block_world(s), x, y + 1, z,
                (struct block_data){ .type = BLOCK_AIR, .metadata = 0 });
    }

    if (dropped) {
        // Beta dropBlockAsItem uses bottom-half metadata: the bottom half drops the door item
        // (this file's getDroppedItem returns 0 items for a top-half meta).
        struct block_data drop_src = cur;
        struct block_info binfo = {
            .block = &drop_src, .neighbours = NULL, .x = x, .y = y, .z = z};
        server_local_spawn_block_drops(s, &binfo);
        return;
    }

    if (origin > 0 && redstone_id_can_provide_power(origin)) {
        bool powered
            = redstone_server_indirectly_getting_powered(s, x, y, z)
            || redstone_server_indirectly_getting_powered(s, x, y + 1, z);
        if (((cur.metadata & 0x04) != 0) != powered)
            door_set_open(s, &s->player, x, y, z, cur.type, cur.metadata, powered);
    }
}

static void onNeighbourBlockChange(struct server_local* s,
                                   struct block_info* info)
{
    // Beta var5 -- MUST be read at entry (a nested set_block re-publishes it).
    uint8_t origin = server_world_notify_origin();
    struct block_data cur = *info->block;

    if (cur.metadata & 0x08) {
        // TOP half (Beta): an orphan top (no door below) is silently cleared; otherwise
        // power changes delegate to the bottom half's full handler.
        struct block_data below;
        bool below_is_door
            = server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
                                     &below)
            && below.type == cur.type;
        if (!below_is_door) {
            // DOCUMENTED DEVIATION: Beta clears the orphan top and STILL recurses into the
            // (non-door) cell below when the origin can provide power -- a destructive quirk
            // acting on a foreign block. We clear and stop.
            server_world_set_block(block_world(s), info->x, info->y, info->z,
                (struct block_data){ .type = BLOCK_AIR, .metadata = 0 });
            return;
        }
        if (origin > 0 && redstone_id_can_provide_power(origin))
            door_bottom_neighbour_change(s, info->x, info->y - 1, info->z,
                                         below, origin);
        return;
    }

    door_bottom_neighbour_change(s, info->x, info->y, info->z, cur, origin);
}



// R1: Beta BlockDoor.onBlockClicked delegates to blockActivated -- a LEFT-click swings the
// WOODEN door exactly like a right-click (top half forwards to the bottom, toggling bit 2, aux-1003).
// The iron door's blockActivated returns at once for Material.iron, so its onBlockClicked is
// the base no-op: block_iron_door registers no hook. [740c583: BlockDoor.java:85-91]
static void onBlockClicked(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this)
{
    (void)held;
    onRightClick(s, actor, NULL, this, this, SIDE_TOP);
}

struct block block_wooden_door = {
	.name = "Wooden Door",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial1,
	.getTextureIndex = getTextureIndex1,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = NULL,
	.onWorldTick = NULL,
    .onNeighbourBlockChange  = onNeighbourBlockChange,
	.onRightClick = onRightClick,
	.onBlockClicked = onBlockClicked,
	.transparent = false,
	.renderBlock = render_block_door,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 1,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 4500,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 1,
		.renderItem = render_item_flat,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_iron_door = {
	.name = "Iron Door",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial2,
	.getTextureIndex = getTextureIndex2,
	.getDroppedItem = getDroppedItem2,
	.onRandomTick = NULL,
    .onNeighbourBlockChange  = onNeighbourBlockChange,
	// Beta BlockDoor.blockActivated returns true for Material.iron: an iron door can NOT be
	// opened by hand -- redstone only. The no-op handler CONSUMES the click (the old fork let
	// the hand toggle it; a NULL here would fall through to item placement against the door).
	.onRightClick = onRightClickIron,
	.transparent = false,
	.renderBlock = render_block_door,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 1,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta hardness x1500: Block.java:663 doorSteel has hardness 5.0, giving
	// 7500 (already correct). The best tool tier is ANY rather than WOOD:
	// ItemPickaxe.blocksEffectiveAgainst omits Block.doorSteel entirely, so
	// getStrVsBlock is flat 1.0F for any pickaxe tier -- no per-tier bonus
	// (same mechanism as block_ore.c's redstone-ore comment).
	.digging.hardness = 7500,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_WOOD,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 1,
		.renderItem = render_item_flat,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
