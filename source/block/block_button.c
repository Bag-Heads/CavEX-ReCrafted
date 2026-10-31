/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

// Beta 1.7.3 BlockButton (decompile 740c583). The stone button (id 77) is the MOMENTARY power
// source: a right-click presses it (bit 8 set), a self-scheduled tick 20 game-ticks later
// (tickRate() == 20) releases it. Wall-mount only (orientations 1-4; Beta has no floor button).
// Metadata: bits 0-2 (&7) = orientation -- 1: mounted on the block at x-1, 2: x+1, 3: z-1,
// 4: z+1 (the SAME mount encoding as the lever's wall cases); bit 8 (&8) = PRESSED.
// Power: redstone_power.c RSP_BUTTON_ID -- isPoweringTo = pressed (all sides, like the lever),
// isIndirectlyPoweringTo = pressed && the mount face only. Modeled on block_lever.c.

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/audio.h" // SOUND_RANDOM_CLICK -- BlockButton.blockActivated / updateTick aux click
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	// Beta BlockButton is Material.circuits; MATERIAL_STONE keeps it non-fuel (the lever/torch
	// precedent -- server_furnace.c reads only this).
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta getCollisionBoundingBoxFromPool returns null -> NO entity collision. The selection
	// box is Beta setBlockBoundsBasedOnState: a 0.375..0.625 (y) x 0.3125..0.6875 (wall axis)
	// face, 0.125 deep (0.0625 when pressed), hugging the mount wall.
	if(entity)
		return 0;

	if(x) {
		bool pressed = (this->block->metadata & 8) > 0;
		float depth = pressed ? 0.0625F : 0.125F;
		float wall = 0.5F - depth * 0.5F; // slide the centered box flush to the wall
		switch(this->block->metadata & 7) {
			case 1: // mounted at x-1: box on the -X wall of this cell
				aabb_setsize(x, depth, 0.25F, 0.375F);
				aabb_translate(x, -wall, 0.375F, 0);
				break;
			case 2: // x+1: +X wall
				aabb_setsize(x, depth, 0.25F, 0.375F);
				aabb_translate(x, wall, 0.375F, 0);
				break;
			case 3: // z-1: -Z wall
				aabb_setsize(x, 0.375F, 0.25F, depth);
				aabb_translate(x, 0, 0.375F, -wall);
				break;
			case 4: // z+1: +Z wall
				aabb_setsize(x, 0.375F, 0.25F, depth);
				aabb_translate(x, 0, 0.375F, wall);
				break;
			default: // unreachable for the wall-only button; a small centered box
				aabb_setsize(x, 0.375F, 0.25F, 0.375F);
				break;
		}
	}

	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	// Beta: Block.button uses stone.blockIndexInTexture (the plain stone tile).
	return tex_atlas_lookup(TEXAT_STONE);
}

// The mount cell for an orientation (Beta onBlockPlaced/onNeighborBlockChange mapping; the
// default y-1 mirrors Beta's else-branch, unreachable for the wall-only button).
static void button_mount_cell(int meta, int x, int y, int z, int* mx, int* my,
							  int* mz) {
	*mx = x;
	*my = y;
	*mz = z;
	switch(meta & 7) {
		case 1: *mx = x - 1; break;
		case 2: *mx = x + 1; break;
		case 3: *mz = z - 1; break;
		case 4: *mz = z + 1; break;
		default: *my = y - 1; break;
	}
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// Beta BlockButton.canPlaceBlockOnSide: WALL sides only (2..5) and the clicked block must
	// be a normal cube. No floor/ceiling mount (unlike the lever's orientation 5/6).
	if(on_side == SIDE_BOTTOM || on_side == SIDE_TOP
	   || !block_is_normal_cube(on->block->type))
		return false;

	// Beta onBlockPlaced side->orientation, the lever/torch SIDE->meta mapping:
	// SIDE_RIGHT (clicked the mount's +X face) -> 1 (mount at x-1), SIDE_LEFT -> 2,
	// SIDE_BACK -> 3, SIDE_FRONT -> 4.
	int metadata;
	switch(on_side) {
		case SIDE_RIGHT: metadata = 1; break;
		case SIDE_LEFT: metadata = 2; break;
		case SIDE_BACK: metadata = 3; break;
		case SIDE_FRONT: metadata = 4; break;
		default: return false;
	}

	// Starts UNPRESSED (bit 8 clear).
	server_world_set_block(block_world(s), where->x, where->y, where->z,
						   (struct block_data) {
							   .type = it->id,
							   .metadata = metadata,
							   .sky_light = 0,
							   .torch_light = 0,
						   });
	notifyNeighbours(s, where->x, where->y, where->z);

	return true;
}

// Shared by press, release and removal: Beta notifies the button's own neighbours AND the
// mount cell's neighbours, BOTH with source = this.blockID (notifyBlocksOfNeighborChange's
// var5) -- so an origin-gated consumer (the door) accepts the mount-cell notify even though
// the block AT that cell is plain stone.
static void button_notify(struct server_local* s, int x, int y, int z,
						  int meta) {
	int mx, my, mz;
	button_mount_cell(meta, x, y, z, &mx, &my, &mz);
	notifyNeighbours_from(s, x, y, z, BLOCK_STONE_BUTTON);
	notifyNeighbours_from(s, mx, my, mz, BLOCK_STONE_BUTTON);
}

static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	// Beta BlockButton.blockActivated: already pressed -> no-op (the press does NOT extend the
	// timer); else set bit 8, notify, and self-schedule the release tickRate()==20 game-ticks
	// out (the block's tick_delay -- GAP #1 suppresses any auto-schedule since this block also
	// defines onNeighbourBlockChange, so this explicit arm is the ONLY timer).
	struct block_data cur = *on->block;
	if(cur.metadata & 8)
		return;
	int newmeta = (cur.metadata & 7) | 8;

	server_world_set_block_silent(block_world(s), on->x, on->y, on->z,
						   (struct block_data) {
							   .type = BLOCK_STONE_BUTTON,
							   .metadata = newmeta,
							   .sky_light = on->block->sky_light,
							   .torch_light = 0,
						   });
	// Beta blockActivated: playSoundEffect(x+0.5,y+0.5,z+0.5, "random.click",
	// 0.3F, 0.6F) BEFORE notifyBlocksOfNeighborChange -- unlike the lever, the
	// press pitch is a FIXED 0.6F (this branch only runs when var8==8, i.e.
	// the button was OFF, so there is no ON/OFF pitch split here).
	// [740c583: BlockButton.blockActivated] Field regression 2026-09-03: this
	// call was missing entirely, the same gap as the lever (block_lever.c,
	// fixed 3d761da) -- docs/sound_parity_inventory_2026-08-26.md line 68
	// already flagged lever/button clicks as "deferred, out of THIS slice".
	server_local_emit_sound(SOUND_RANDOM_CLICK,
						   (vec3) {(float)on->x + 0.5F, (float)on->y + 0.5F,
								   (float)on->z + 0.5F},
						   0.3F, 0.6F);
	button_notify(s, on->x, on->y, on->z, newmeta);
	server_world_schedule_block_tick(block_world(s), on->x, on->y, on->z,
									 BLOCK_STONE_BUTTON);
}

// The self-scheduled release (Beta updateTick): clear bit 8 and notify. A stale
// tick on an already-released button is a one-shot no-op.
static void onWorldTick(struct server_local* s, struct block_info* info) {
	struct block_data cur = *info->block;
	if((cur.metadata & 8) == 0)
		return;
	int newmeta = cur.metadata & 7;
	server_world_set_block_silent(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {
							   .type = BLOCK_STONE_BUTTON,
							   .metadata = newmeta,
							   .sky_light = cur.sky_light,
							   .torch_light = 0,
						   });
	button_notify(s, info->x, info->y, info->z, newmeta);
	// Beta updateTick: notifyBlocksOfNeighborChange happens FIRST, THEN
	// playSoundEffect(x+0.5,y+0.5,z+0.5, "random.click", 0.3F, 0.5F) -- the
	// release pitch is a FIXED 0.5F (this branch only runs when bit 8 was
	// set). [740c583: BlockButton.updateTick] Field regression 2026-09-03
	// (see onRightClick above for the same gap's press-side citation).
	server_local_emit_sound(SOUND_RANDOM_CLICK,
						   (vec3) {(float)info->x + 0.5F,
								   (float)info->y + 0.5F,
								   (float)info->z + 0.5F},
						   0.3F, 0.5F);
}

static void onBlockRemoval(struct server_local* s, struct block_info* info) {
	if(info->block->metadata & 8)
		button_notify(s, info->x, info->y, info->z,
			info->block->metadata);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	// Beta drops itself (one stone button, id 77).
	if(it) {
		it->id = BLOCK_STONE_BUTTON;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

// Beta BlockButton.onNeighborBlockChange: drop when the SPECIFIC mount wall (meta&7) is no
// longer a normal cube. (Beta first runs checkForDrop's any-wall canPlaceBlockAt test, then
// the orientation test -- the orientation test subsumes it for every reachable state, since a
// placed button's mount IS one of the four walls.) The lever support-drop pattern.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	int meta = info->block->metadata;
	int mx, my, mz;
	button_mount_cell(meta, info->x, info->y, info->z, &mx, &my, &mz);

	struct block_data mount;
	if(server_world_get_block(block_world(s), mx, my, mz, &mount)
	   && block_is_normal_cube(mount.type))
		return; // still attached -- nothing to do

	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

// R1: Beta BlockButton.onBlockClicked is `this.blockActivated(...)` -- a LEFT-click presses the
// button exactly like a right-click (same already-pressed no-op, click sound, notify and
// 20-tick release). The held item is irrelevant; blockActivated reads only the clicked cell.
// [740c583: BlockButton.java:120-122]
static void onBlockClicked(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this) {
	(void)held;
	onRightClick(s, actor, NULL, this, this, SIDE_TOP);
}

struct block block_stone_button = {
	.name = "Button",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBlockRemoval = onBlockRemoval,
	.onRandomTick = NULL,
	.onWorldTick = onWorldTick, // the self-scheduled release (Beta updateTick)
	.tick_delay = 20,			// Beta BlockButton.tickRate()
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: drop if the mount is removed
	.onRightClick = onRightClick,
	.onBlockClicked = onBlockClicked,
	.transparent = false,
	.renderBlock = render_block_button,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750, // Beta BlockButton hardness 0.5F at the port's 1500/unit scale
							 // (dirt 0.5F->750, stone 1.5F->2250; the lever's 25 is a
							 // pre-existing outlier, not the scale)
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
