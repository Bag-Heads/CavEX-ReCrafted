/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"
#include "redstone_server.h"
#include "../particle.h"
#include "../game/game_state.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)


static enum block_material getMaterial(struct block_info* this) {
	// Beta BlockTorch is Material.circuits (NOT wood), so TileEntityFurnace.
	// getItemBurnTime returns 0 -- a torch is NOT furnace fuel. getMaterial here is
	// read ONLY by the furnace fuel check (server_furnace.c); this enum has no
	// "circuits" value, so MATERIAL_STONE is used purely to keep the torch correctly
	// NON-fuel (it must not hit the material==wood -> 300-tick branch). A torch item
	// is id 50 (< 256), so without this it would wrongly burn for 15s.
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x) {
		// Beta BlockTorch.collisionRayTrace (java:126-139): half .15F,
		// wall depth .3F, y 0.2F..0.8F. Direct endpoints preserve Beta floats.
		static const struct AABB wall[] = {
			{0.0F, 0.2F, 0.35F, 0.3F, 0.8F, 0.65F},
			{0.7F, 0.2F, 0.35F, 1.0F, 0.8F, 0.65F},
			{0.35F, 0.2F, 0.0F, 0.65F, 0.8F, 0.3F},
			{0.35F, 0.2F, 0.7F, 0.65F, 0.8F, 1.0F},
		};
		int meta = this->block->metadata & 7;
		if(meta >= 1 && meta <= 4)
			*x = wall[meta - 1];
		else
			aabb_setsize(x, 0.2F, 0.6F, 0.2F);
	}

	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex1(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_TORCH);
}

static uint8_t getTextureIndex2(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_REDSTONE_TORCH);
}

static uint8_t getTextureIndex3(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_REDSTONE_TORCH_LIT);
}

// Beta BlockTorch.func_31032_h: only the floor mount accepts a fence.
static bool floor_support(uint8_t type) {
	return block_is_normal_cube(type) || type == BLOCK_FENCE;
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// Beta BlockTorch.canPlaceBlockOnSide: wall mounts need a normal cube;
	// the floor also accepts a fence. SIDE_BOTTOM stays rejected.
	if(on_side == SIDE_BOTTOM
	   || !(on_side == SIDE_TOP ? floor_support(on->block->type)
							 : block_is_normal_cube(on->block->type)))
		return false;

	int metadata = 0;
	switch(on_side) {
		case SIDE_LEFT: metadata = 2; break;
		case SIDE_RIGHT: metadata = 1; break;
		case SIDE_FRONT: metadata = 4; break;
		case SIDE_BACK: metadata = 3; break;
		default: metadata = 5; break;
	}

	server_world_set_block(block_world(s), where->x, where->y, where->z,
						   (struct block_data) {
							   .type = it->id,
							   .metadata = metadata,
							   .sky_light = 0,
							   .torch_light = 0,
						   });

	return true;
}

static void redstone_torch_active_notify(
	struct server_local* s, struct block_info* info) {
	static const int dx[6] = {0, 0, -1, 1, 0, 0};
	static const int dy[6] = {-1, 1, 0, 0, 0, 0};
	static const int dz[6] = {0, 0, 0, 0, -1, 1};
	for(int i = 0; i < 6; i++)
		notifyNeighbours_from(
			s, info->x + dx[i], info->y + dy[i], info->z + dz[i],
			BLOCK_REDSTONE_TORCH_LIT);
}

static size_t drop_redstone_torch(struct block_info* this, struct item_data* it,
								  struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = BLOCK_REDSTONE_TORCH_LIT;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

// Beta BlockTorch.onNeighborBlockChange (the regular torch AND both redstone torches): drop the
// torch when its mount is no longer a normal cube (or a fence for a floor torch).
// Otherwise it floats; a redstone torch also lingers as a phantom power source.
// Mount cell from metadata (this file's aabb + Beta):
// 1 -> -X, 2 -> +X, 3 -> -Z, 4 -> +Z, 5 (floor) -> below (y-1).
//
// Self-scheduling: defining onNeighbourBlockChange SUPPRESSES the engine's constant-tick_delay
// auto-schedule (server_world_notify_block GAP #1, the `!cb->onNeighbourBlockChange` guards) -- which
// is how the redstone torch's inverter (redstone_torch_tick, tickRate 2) normally re-arms on a
// neighbour change. So when the torch is still supported we re-arm it ourselves with the SAME call
// the engine would have made, preserving the inverter exactly. For the regular torch this is a
// no-op (server_world_schedule_block_tick returns false: no onWorldTick), so one handler serves all
// three torch blocks.
static void torch_neighbour_change(struct server_local* s, struct block_info* info) {
	int x = info->x, y = info->y, z = info->z;
	int mx = x, my = y, mz = z;
	switch(info->block->metadata) {
		case 1: mx = x - 1; break;
		case 2: mx = x + 1; break;
		case 3: mz = z - 1; break;
		case 4: mz = z + 1; break;
		default: my = y - 1; break; // 5 = floor torch -> the block below
	}

	struct block_data mount;
	if(!(server_world_get_block(block_world(s), mx, my, mz, &mount)
		 && (info->block->metadata == 5 ? floor_support(mount.type)
								  : block_is_normal_cube(mount.type)))) {
		server_local_spawn_block_drops(s, info); // Beta dropBlockAsItem (drops the torch item)
		server_world_set_block(block_world(s), x, y, z,
							   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
		return;
	}

	// Still supported: re-arm this torch's own scheduled tick -- identical to the auto-schedule
	// server_world_notify_block would make for a non-self-scheduling block (line ~491). A no-op for
	// the regular torch (no onWorldTick); for the redstone torches it preserves the inverter.
	server_world_schedule_block_tick(block_world(s), x, y, z, info->block->type);
}

// Torch flame/smoke particles moved to the CLIENT random display tick
// (source/game/client_display_tick.c) -- where Beta 1.7.3 puts them
// (Block.randomDisplayTick is client-only). The old onWorldTick here was a
// double boundary violation: the SERVER tick thread wrote the global client
// particle array (racing the render thread's particle_update/render) AND
// advanced the client RNG (gstate.rand_src) cross-thread. It also made every
// torch in the world re-schedule a server tick at ~20Hz forever (the
// fire-scheduled-tick requeue), polluting the scheduled-tick queue for zero
// sim effect. onWorldTick is now NULL on all three torch blocks.

struct block block_torch = {
	.name = "Torch",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex1,
	.getDroppedItem = block_drop_default,
	.onRandomTick = NULL,
	.onWorldTick = NULL, // flames are client display ticks (see above)
	.onNeighbourBlockChange = torch_neighbour_change, // Beta: drop if the mount is removed
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_torch,
	.renderBlockAlways = NULL,
	.luminance = 14,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 50,
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

struct block block_redstone_torch = {
	.name = "Redstone Torch",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex2,
	.getDroppedItem = drop_redstone_torch,
	// Beta BlockRedstoneTorch.updateTick (the inverter): scheduled at tickRate()==2 on a
	// neighbour change, one-shot (re-arms only on the next neighbour change). idle id 75.
	.onWorldTick = redstone_torch_tick,
	.tick_delay = REDSTONE_TORCH_TICK_RATE,
	// Beta BlockRedstoneTorch.onNeighborBlockChange: drop if the mount is gone, else re-arm the
	// inverter (self-scheduling -- adding this suppresses the engine's auto-schedule; see the handler).
	.onNeighbourBlockChange = torch_neighbour_change,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_torch,
	.renderBlockAlways = NULL,
	.luminance = 0, // an UNLIT redstone torch is dark (Beta torchRedstoneIdle emits no light)
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 50,
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

struct block block_redstone_torch_lit = {
	.name = "Redstone Torch",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex3,
	.getDroppedItem = drop_redstone_torch,
	.onBlockAdded = redstone_torch_active_notify,
	.onBlockRemoval = redstone_torch_active_notify,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	// lit id 76: same scheduled inverter tick; turns OFF when its mount becomes powered.
	.onWorldTick = redstone_torch_tick,
	.tick_delay = REDSTONE_TORCH_TICK_RATE,
	.onNeighbourBlockChange = torch_neighbour_change, // Beta: support-drop, else re-arm the inverter
	.transparent = false,
	.renderBlock = render_block_torch,
	.renderBlockAlways = NULL,
	.luminance = 7,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 50,
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
