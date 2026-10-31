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

#include "../cglm/types.h"
#include "../graphics/render_block.h"
#include "../graphics/render_item.h"
#include "../graphics/texture_atlas.h"
#include "../item/tool.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // A1a: block_world(s) needs the full server state (as block_chest.c)
#include "../network/server_world.h"
#include "redstone_server.h" // redstone_server_indirect_power (Beta isBlockIndirectlyGettingPowered)
#include "../entity/entity_primed_tnt.h"
#include "aabb.h"
#include "blocks.h"
#include "blocks_data.h"
#include "face_occlusion.h"
#include "redstone_power.h"
#include "tnt_texture.h"
#define TNT_FUSE_TICKS TNT_FUSE_DEFAULT

// Beta World.isBlockIndirectlyGettingPowered side order. This is the same
// canonical power view used by pistons, including strong power through solids.
static bool tnt_is_powered(struct server_local* s, int x, int y, int z) {
	return redstone_server_indirect_power(s, x, y - 1, z, 0, true)
		|| redstone_server_indirect_power(s, x, y + 1, z, 1, true)
		|| redstone_server_indirect_power(s, x, y, z - 1, 2, true)
		|| redstone_server_indirect_power(s, x, y, z + 1, 3, true)
		|| redstone_server_indirect_power(s, x - 1, y, z, 4, true)
		|| redstone_server_indirect_power(s, x + 1, y, z, 5, true);
}

static bool tnt_prime_if_powered(struct server_local* s, int x, int y, int z) {
	if(!tnt_is_powered(s, x, y, z))
		return false;
	return server_local_prime_tnt_block(s, x, y, z, TNT_FUSE_TICKS);
}

static void onBlockAdded(struct server_local* s, struct block_info* info) {
	/* Beta BlockTNT.onBlockAdded delegates to the same powered-neighbour check.
	 * The generic world hook also covers piston/world-edit placement, not only
	 * this block item's placement callback. */
	(void)tnt_prime_if_powered(s, info->x, info->y, info->z);
}

// Beta BlockTNT.onNeighborBlockChange is synchronous and only reacts when the
// changed block can provide power. A primed entity never competes for admission
// to World's scheduled block tick set.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	uint8_t origin = server_world_notify_origin();
	/* R1d: the old "metadata != 0 primes on ANY neighbour update" legacy branch
	 * is DELETED. Its stated window ("before its legacy archive record is
	 * restored") cannot occur: server_world_load_chunk installs the chunk and
	 * runs server_local_restore_chunk_entities synchronously on the one server
	 * thread, before any tick or notify can reach its blocks, and that restore
	 * migrates block-backed TNT by record (server_local.c legacy_tnt). What the
	 * branch DID do was detonate Beta's flint-stamped TNT (meta 1, persisted)
	 * on a torch/water/sand change beside it; Beta primes only on power here
	 * or on the player break. A record-less legacy meta!=0 block now behaves
	 * exactly like Beta's stamped TNT. [740c583: BlockTNT.java:22-28,44-60] */
	if(origin > 0 && redstone_id_can_provide_power(origin))
		(void)tnt_prime_if_powered(s, info->x, info->y, info->z);
}



// R1: Beta BlockTNT.onBlockClicked -- a LEFT-click with flint & steel held stamps metadata 1
// through World.setBlockMetadata (chunk write only: no neighbour notify, no client packet --
// server_world_set_block_metadata_quiet is that exact port), then falls to the
// base no-op. The stamp is consumed by the player break (hardness 0 -> the same click breaks
// it): onBlockDestroyedByPlayer sees (meta & 1) and primes instead of dropping -- see
// server_local_dig_primes_tnt. The item is NOT damaged on this path (Beta never calls
// damageItem here; the right-click prime's wear is carve-out entry #3 in docs/deviations.md).
// [740c583: BlockTNT.java:54-60, World.java:426-438]
static void onBlockClicked(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this) {
	(void)actor;
	if(!held || held->id != ITEM_FLINT_STEEL)
		return;
	(void)server_world_set_block_metadata_quiet(block_world(s), this->x, this->y, this->z, 1);
}

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

static size_t getBoundingBox(struct block_info* this, bool entity, struct AABB* x) {
	if (x) aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion* getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	// Primed TNT is an entity now, so every world/item face is always the base
	// Beta TNT tile. EntityTNTPrimed's white pulse is a render overlay, never snow.
	return tex_atlas_lookup(tnt_face_texat(this->block->metadata, side));
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
							struct block_info* where, struct block_info* on,
							enum side on_side) {
	struct block_data blk = (struct block_data) {
		.type = it->id,
		.metadata = 0,
		.sky_light = 0,
		.torch_light = 0,
	};

	struct block_info blk_info = *where;
	blk_info.block = &blk;

	if(block_place_obstructed_for(s, actor, &blk_info)) // player + all entities (Beta gate)
		return false;

	return server_world_set_block(block_world(s), where->x, where->y, where->z, blk);
}

struct block block_tnt = {
	.name = "TNT",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onBlockAdded = onBlockAdded,
	.onRandomTick = NULL,
	.onWorldTick = NULL,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	// Beta BlockTNT.blockActivated delegates to Block.blockActivated and returns
	// false. Leave this unset so the held item's use/placement path still runs.
	.onRightClick = NULL,
	.onBlockClicked = onBlockClicked,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = true,
	.place_ignore = false,
	.digging.hardness = 50,
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
