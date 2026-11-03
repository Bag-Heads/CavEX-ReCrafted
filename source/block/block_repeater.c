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

// Block-table entries for the Beta 1.7.3 redstone repeater (ids 93 idle / 94 active). The
// faithful SIMULATION lives in redstone_repeater.c (the pure scheduled-toggle logic) +
// redstone_power.c (the directional power output) + redstone_server.c (the production hooks
// redstone_repeater_server_tick / _neighbour_change). This file binds those onto the two block
// ids and supplies the render-facing fields (a 0.125 slab with the Beta face textures). The
// placeable inventory form is ITEM_REDSTONE_REPEATER (item_repeater.c); the block drops it.

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../item/items.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"
#include "blocks_data.h"
#include "redstone_server.h"

static enum block_material getMaterial(struct block_info* this) {
	(void)this;
	// Beta Material.circuits; the repo has no circuits material (redstone wire also uses STONE).
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity, struct AABB* x) {
	(void)this;
	if(x)
		aabb_setsize(x, 1.0F, 0.125F, 1.0F); // Beta setBlockBounds(0,0,0,1,0.125,1)
	// Beta BlockRedstoneRepeater.java:13 inherits Block.java:267-268 collision.
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	(void)this;
	(void)side;
	(void)it;
	return face_occlusion_empty(); // isOpaqueCube()==false -> occludes no neighbour face
}

// Beta getBlockTextureFromSideAndMetadata: side0 bottom (idle 115/active 99 == the redstone-torch
// tiles), side1 top (idle 131/active 147 == the dedicated repeater tiles), every other side stone.
static uint8_t getTextureIndexIdle(struct block_info* this, enum side side) {
	(void)this;
	if(side == SIDE_TOP)
		return tex_atlas_lookup(TEXAT_REPEATER_TOP_OFF);
	if(side == SIDE_BOTTOM)
		return tex_atlas_lookup(TEXAT_REDSTONE_TORCH); // Beta idx 115
	return tex_atlas_lookup(TEXAT_SLAB_STONE_SIDE);	  // Beta side idx 5 (stone-slab side)
}
static uint8_t getTextureIndexActive(struct block_info* this, enum side side) {
	(void)this;
	if(side == SIDE_TOP)
		return tex_atlas_lookup(TEXAT_REPEATER_TOP_ON);
	if(side == SIDE_BOTTOM)
		return tex_atlas_lookup(TEXAT_REDSTONE_TORCH_LIT); // Beta idx 99
	return tex_atlas_lookup(TEXAT_SLAB_STONE_SIDE);		   // Beta side idx 5
}

// Beta BlockRedstoneRepeater.idDropped -> Item.redstoneRepeater (the crafted item, not the block).
static size_t drop_repeater(struct block_info* this, struct item_data* it,
							struct java_random* g, struct server_local* s) {
	(void)this;
	(void)g;
	(void)s;
	if(it) {
		it->id = ITEM_REDSTONE_REPEATER;
		it->durability = 0;
		it->count = 1;
	}
	return 1;
}

// Beta BlockRedstoneRepeater.blockActivated: right-click cycles the delay index (bits 2-3) through
// {0,1,2,3} -> {2,4,6,8}-tick delays, KEEPING the facing (bits 0-1). The notify re-arms scheduling.
static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	(void)it;
	(void)where;
	(void)on_side;
	struct block_data b;
	if(!server_world_get_block(block_world(s), on->x, on->y, on->z, &b))
		return;
	int meta = b.metadata & 0x0F;
	int delay = (meta & 12) >> 2;
	delay = ((delay + 1) << 2) & 12; // (idx+1)<<2 & 12: advance the delay index, wrap 3 -> 0
	// Beta World.setBlockMetadataWithNotify [740c583 World.java:414-423]: a metadata-only write
	// plus the six-neighbour notify -- Chunk.setBlockMetadata, NOT setBlockIDWithMetadata, so
	// onBlockAdded (the 2-hop notify below) does not run on a delay cycle.
	server_world_set_block_metadata_with_notify(block_world(s), on->x, on->y, on->z,
												(uint8_t)((delay | (meta & 3)) & 0x0F));
}

// Beta BlockRedstoneRepeater.onBlockAdded [740c583 BlockRedstoneRepeater.java:127-134]:
// notifyBlocksOfNeighborChange(x+1 / x-1 / z+1 / z-1 / y-1 / y+1, this.blockID) -- the
// neighbours of each of the six neighbours learn of the change with var5 = the repeater id.
// Chunk.setBlockIDWithMetadata [Chunk.java:236-267] runs this on EVERY id write, so it fires on
// placement and on each idle<->active swap (updateTick's setBlockAndMetadataWithNotify). It is
// what carries the repeater's output through a solid block to the wire/torch/piston behind it
// (HUNT-14 item 1); World.notifyBlockChange alone only reaches the solid block itself. The
// redstone torch does the same in block_torch.c (redstone_torch_active_notify).
static void repeater_on_added(struct server_local* s, struct block_info* info) {
	static const int dx[6] = {1, -1, 0, 0, 0, 0};
	static const int dy[6] = {0, 0, 0, 0, -1, 1};
	static const int dz[6] = {0, 0, 1, -1, 0, 0};
	const uint8_t self = info->block->type; // 93 idle or 94 active: the var5 the consumers see
	for(int i = 0; i < 6; i++)
		notifyNeighbours_from(s, info->x + dx[i], info->y + dy[i], info->z + dz[i], self);
}

struct block block_redstone_repeater = {
	.name = "Redstone Repeater",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndexIdle,
	.getDroppedItem = drop_repeater,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.onBlockAdded = repeater_on_added, // BlockRedstoneRepeater.onBlockAdded (2-hop notify)
	// Metadata-delayed {2,4,6,8} scheduled toggle. The block self-schedules in
	// onNeighbourBlockChange, so the engine's constant-tick_delay auto-schedule
	// is suppressed for it (server_world_notify_block GAP #1).
	.onWorldTick = redstone_repeater_server_tick,
	.onNeighbourBlockChange = redstone_repeater_server_neighbour_change,
	.transparent = false,
	.renderBlock = render_block_repeater,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta setHardness(0.0F) [740c583 Block.java:685] = instant break. The
	// literal 0 this used to store is WRONG in this engine: tool_dig_delay_ms
	// collapses hardness <= 0 to the -1 "unbreakable" sentinel (bedrock's),
	// so the repeater could never be dug at all (task_6ea50e06, found by the
	// FH-14 wire fix). 50 = the port's proven one-tick instant-break encoding
	// (torch/sapling/wire precedent; see test_redstone_wire_dig).
	.digging.hardness = 50,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		// The placeable inventory form is ITEM_REDSTONE_REPEATER (faithful yaw->facing place);
		// this vestigial block-item path just satisfies the non-NULL registry assert.
		.onItemPlace = block_place_default,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_redstone_repeater_lit = {
	.name = "Redstone Repeater",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndexActive,
	.getDroppedItem = drop_repeater,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.onBlockAdded = repeater_on_added, // same hook: the active id's swap notifies 2 hops too
	.onWorldTick = redstone_repeater_server_tick,
	.onNeighbourBlockChange = redstone_repeater_server_neighbour_change,
	.transparent = false,
	.renderBlock = render_block_repeater,
	.renderBlockAlways = NULL,
	.luminance = 9, // Beta redstoneRepeaterActive.setLightValue(0.625F) -> (int)(15*0.625) = 9
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 50, // same fix as the idle variant above (lit repeater)
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
