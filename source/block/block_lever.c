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

// Beta 1.7.3 BlockLever (decompile 740c583). The lever is the canonical TOGGLEABLE redstone power
// source: a player right-click flips bit 8 (ON/OFF); the boolean power model (redstone_power.c,
// case RSP_LEVER_ID) reads bit 8 for isPoweringTo (all sides) and (meta&7)->mount-face for
// isIndirectlyPoweringTo. Metadata: bits 0-2 (&7) = orientation 1-6 (1-4 wall, 5/6 floor); bit 8
// (&8) = ON. Modeled on block_torch.c (the face-attached-block template).

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/audio.h" // SOUND_RANDOM_CLICK -- BlockLever.blockActivated aux click
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	// Beta BlockLever is Material.circuits (like the torch). This enum has no "circuits" value;
	// MATERIAL_STONE keeps the lever NON-fuel for the furnace fuel check (server_furnace.c reads
	// only this), exactly as block_torch.c does -- a lever item must not burn.
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockLever.getCollisionBoundingBoxFromPool returns null -> NO entity collision. The
	// render/selection box (entity==false) is a small box on the mounting face, like the torch.
	if(entity)
		return 0;

	if(x) {
		aabb_setsize(x, 0.375F, 0.6F, 0.375F);

		// Beta BlockLever.setBlockBoundsBasedOnState (java:111-124): wall y 0.2..0.8.
		switch(this->block->metadata & 7) {
			case 1: aabb_translate(x, -0.3125F, 0.2F, 0); break;	  // -X wall
			case 2: aabb_translate(x, 0.3125F, 0.2F, 0); break;		  // +X wall
			case 3: aabb_translate(x, 0, 0.2F, -0.3125F); break;	  // -Z wall
			case 4: aabb_translate(x, 0, 0.2F, 0.3125F); break;		  // +Z wall
			default: aabb_setsize(x, 0.5F, 0.6F, 0.5F); break;		  // 5/6 floor
		}
	}

	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_LEVER);
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// Beta BlockLever.canPlaceBlockOnSide: the mounting block (the clicked block) must be a NORMAL
	// CUBE (World.isBlockNormalCube). The old can_see_through proxy wrongly allowed non-cube solids
	// (slabs/stairs/etc.). SIDE_BOTTOM is still rejected (Beta has no side-0 mount case).
	if(on_side == SIDE_BOTTOM || !block_is_normal_cube(on->block->type))
		return false;

	// Beta BlockLever.onBlockPlaced maps the clicked side to orientation. We use the SAME SIDE->meta
	// mapping as block_torch.c so the power orientation stays consistent with redstone_power.c:
	// SIDE_TOP (clicked top -> floor lever) -> 5, SIDE_RIGHT -> 1, SIDE_LEFT -> 2, SIDE_BACK -> 3,
	// SIDE_FRONT -> 4. The floor lever is Beta's 5 + World.rand.nextInt(2)
	// [BlockLever.onBlockPlaced:37-38]: meta 5 or 6, the two handle rotations (both power
	// side 1), picked by ONE draw on the shared world stream (s->rand_src == World.rand).
	// This runs server-side, so the draw is the world's, exactly as in Beta; wall levers draw
	// nothing. The old fixed 5 skipped that draw and shifted every later World.rand consumer.
	// tests/test_lever_place_golden.c.
	int metadata = 0;
	switch(on_side) {
		case SIDE_LEFT: metadata = 2; break;
		case SIDE_RIGHT: metadata = 1; break;
		case SIDE_FRONT: metadata = 4; break;
		case SIDE_BACK: metadata = 3; break;
		default: metadata = 5 + rand_gen_int(&s->rand_src, 2); break; // SIDE_TOP -> floor
	}

	// Starts OFF (bit 8 clear).
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

static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	// Beta BlockLever.blockActivated: toggle bit 8 (newmeta = (meta&7) + (8 - (meta&8))), then
	// notify neighbours so adjacent wire re-floods / components re-evaluate. `on` is the lever just
	// clicked (the right-click target). server-only (no RNG, no client mutation).
	struct block_data cur = *on->block;
	int newmeta = (cur.metadata & 7) + (8 - (cur.metadata & 8));

	server_world_set_block_silent(block_world(s), on->x, on->y, on->z,
						   (struct block_data) {
							   .type = BLOCK_LEVER,
							   .metadata = newmeta,
							   .sky_light = on->block->sky_light,
							   .torch_light = 0,
						   });
	// Beta blockActivated: world.playSoundEffect(x+0.5,y+0.5,z+0.5, "random.click",
	// 0.3F, var8 > 0 ? 0.6F : 0.5F) where var8 = 8 - (oldmeta & 8) is the SAME
	// value folded into newmeta above -- var8 > 0 iff the lever is now ON, so
	// (newmeta & 8) is the exact faithful gate: turning ON plays pitch 0.6F,
	// turning OFF plays pitch 0.5F, always at volume 0.3F. [740c583:
	// BlockLever.blockActivated] Regression 2026-09-03: this call was missing
	// entirely (never wired since the lever's S0/M4 redstone-slice sound work
	// was deferred, docs/sound_parity_inventory_2026-08-26.md line 68) --
	// restored here, not caused by today's audio_sound_table.h minecart edit.
	server_local_emit_sound(SOUND_RANDOM_CLICK,
						   (vec3) {(float)on->x + 0.5F, (float)on->y + 0.5F,
								   (float)on->z + 0.5F},
						   0.3F, (newmeta & 8) ? 0.6F : 0.5F);
	// Beta blockActivated notifies the lever's neighbours AND the mount cell's neighbours,
	// both with source = this.blockID (var5) -- the mount-cell notify is what lets a door or
	// wire behind the mounting block react to the strong power driven through it (and the
	// explicit source id is what passes the door's canProvidePower origin gate).
	notifyNeighbours_from(s, on->x, on->y, on->z, BLOCK_LEVER);
	int mx = on->x, my = on->y, mz = on->z;
	switch(newmeta & 7) {
		case 1: mx--; break;
		case 2: mx++; break;
		case 3: mz--; break;
		case 4: mz++; break;
		default: my--; break; // 5/6 floor lever
	}
	notifyNeighbours_from(s, mx, my, mz, BLOCK_LEVER);
}

static void onBlockRemoval(struct server_local* s, struct block_info* info) {
	if((info->block->metadata & 8) == 0)
		return;
	int mx = info->x, my = info->y, mz = info->z;
	switch(info->block->metadata & 7) {
		case 1: mx--; break;
		case 2: mx++; break;
		case 3: mz--; break;
		case 4: mz++; break;
		default: my--; break;
	}
	notifyNeighbours_from(s, info->x, info->y, info->z, BLOCK_LEVER);
	notifyNeighbours_from(s, mx, my, mz, BLOCK_LEVER);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	// Beta BlockLever drops itself (one lever, id 69).
	if(it) {
		it->id = BLOCK_LEVER;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

// Beta BlockLever.onNeighborBlockChange: drop the lever when its mounting block is no longer a normal
// cube (the wall/floor it hangs on was removed). Without this a floating lever lingers -- and an ON
// lever lingers as a phantom redstone power source. (block_torch.c now has the equivalent torch
// support-drop -- incl. the redstone torches' self-scheduling variant that preserves the inverter.)
static void onNeighbourBlockChange(struct server_local* s, struct block_info* info) {
	// Mount cell from meta&7 -- matches Beta BlockLever.onBlockPlaced AND this file's aabb:
	// 1 -> -X (x-1), 2 -> +X (x+1), 3 -> -Z (z-1), 4 -> +Z (z+1), 5/6 (floor) -> below (y-1).
	int x = info->x, y = info->y, z = info->z;
	int mx = x, my = y, mz = z;
	switch(info->block->metadata & 7) {
		case 1: mx = x - 1; break;
		case 2: mx = x + 1; break;
		case 3: mz = z - 1; break;
		case 4: mz = z + 1; break;
		default: my = y - 1; break; // 5/6 floor lever -> the block below
	}

	struct block_data mount;
	if(server_world_get_block(block_world(s), mx, my, mz, &mount)
	   && block_is_normal_cube(mount.type))
		return; // still attached -- nothing to do

	server_local_spawn_block_drops(s, info); // Beta dropBlockAsItem (drops the lever item)
	server_world_set_block(block_world(s), x, y, z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

// R1: Beta BlockLever.onBlockClicked is `this.blockActivated(...)` -- a LEFT-click throws the
// lever exactly like a right-click (toggle, click sound, both notifies). The held item is
// irrelevant; blockActivated reads only the clicked cell. [740c583: BlockLever.java:129-131]
static void onBlockClicked(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this) {
	(void)held;
	onRightClick(s, actor, NULL, this, this, SIDE_TOP);
}

struct block block_lever = {
	.name = "Lever",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBlockRemoval = onBlockRemoval,
	.onRandomTick = NULL,
	.onWorldTick = NULL, // the lever has no scheduled tick
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: drop if the mount is removed
	.onRightClick = onRightClick,
	.onBlockClicked = onBlockClicked,
	.transparent = false,
	.renderBlock = render_block_lever,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750, // Beta BlockLever hardness 0.5F at the port's 1500/unit scale
							 // (dirt 0.5F->750; the old 25 was ~30x too fast to mine)
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
