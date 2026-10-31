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

#include "blocks.h"
#include "blocks_data.h"
#include "fluid_block.h"
#include "../graphics/gfx_settings.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	return 0;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	#ifdef GFX_FANCY_LIQUIDS
	int block_height = (this->block->metadata & 0x8) ?
		16 :
		(8 - this->block->metadata) * 2 * 7 / 8;
	switch(side) {
		case SIDE_TOP:
			// Water's flow/still equivalence, mirrored (bug-hunt D10): Beta
			// BlockFluid.shouldSideBeRendered suppresses by MATERIAL, so still
			// lava over flowing lava (and vice versa) occludes the seam face.
			// Exact-type equality left the cross-id seam drawn -- render-only
			// overdraw, no simulation effect.
			return (it->block->type == BLOCK_LAVA_FLOW
					|| it->block->type == BLOCK_LAVA_STILL) ?
				face_occlusion_full() :
				face_occlusion_empty();
		case SIDE_BOTTOM: return face_occlusion_full();
		default: return face_occlusion_rect(block_height);
	}
	#else
	return face_occlusion_full();
	#endif
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	#ifdef GFX_FANCY_LIQUIDS
	return TEXTURE_INDEX(2, 0);
	#else
	return tex_atlas_lookup(TEXAT_LAVA_STATIC);
	#endif
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	return 0;
}

// Flowing lava (id 10) runs the Beta spread (2-level step, no infinite source);
// it settles itself to still (id 11) when stable. Still lava (id 11) re-wakes to
// flowing when a neighbour opens a path. Both delegate to fluid_block.c.
static void onWorldTick(struct server_local* s, struct block_info* this) {
	fluid_block_tick(s, this, true); // Nether step handled inside (dimension read there)
}

static void onBlockAdded_flow(struct server_local* s,
							  struct block_info* this) {
	fluid_block_on_added(s, this, true, BLOCK_LAVA_FLOW);
}

static void onBlockAdded_still(struct server_local* s,
							   struct block_info* this) {
	fluid_block_on_added(s, this, true, 0);
}

static void onNeighbourChange_flow(struct server_local* s,
								   struct block_info* this) {
	fluid_block_on_neighbour(s, this, true, 0);
}

static void onNeighbourChange_still(struct server_local* s,
									struct block_info* this) {
	fluid_block_on_neighbour(s, this, true, BLOCK_LAVA_FLOW);
}

// ---- Beta BlockStationary.updateTick: STILL lava randomly starts fires ----
//
// Two per-id tables transcribed from the Beta 1.7.3 material assignments
// (Material.java static init + the Block.java constructors). They are Beta
// MATERIAL properties, deliberately NOT our fire_encourage/fire_catch tables:
// isFlammable here is Material.getBurning -- true ONLY for the wood /
// leaves / cloth / tnt materials (a chest or sign counts; TALL GRASS and other
// Material.plants blocks do NOT, even though fire's own spread consumes them).
// The walk-abort test is Material.getIsSolid -- false only for air, liquids,
// fire (MaterialTransparent), plants + circuits + snow-layer (MaterialLogic),
// and portal, so a torch or rail above lava lets the walk continue where a
// slab or glass stops it (leaves are a SOLID material: burnable, but they
// also abort the walk). [decompile 740c583 Material static init;
// BlockStationary.updateTick; BlockStationary.isFlammable]
static const uint8_t lava_burnable[256] = {
	// Material.wood: planks, log, note block, bookshelf, wooden stairs, chest,
	// crafting table, signs (standing 63 / wall 68), wooden door, jukebox,
	// fence, wooden pressure plate, trapdoor
	[5] = 1,  [17] = 1, [25] = 1, [47] = 1, [53] = 1, [54] = 1, [58] = 1,
	[63] = 1, [64] = 1, [68] = 1, [84] = 1, [85] = 1, [72] = 1, [96] = 1,
	[18] = 1,			// Material.leaves
	[26] = 1, [35] = 1, // Material.cloth: bed, wool
	[46] = 1,			// Material.tnt
};
static const uint8_t lava_walk_not_solid[256] = {
	[0] = 1,  [8] = 1,	[9] = 1,  [10] = 1, [11] = 1, [51] = 1, // air/liquids/fire
	// Material.plants: sapling, tall grass, dead bush, dandelion, rose, brown +
	// red mushroom, crops, reed
	[6] = 1,  [31] = 1, [32] = 1, [37] = 1, [38] = 1, [39] = 1, [40] = 1,
	[59] = 1, [83] = 1,
	// Material.circuits: rails (27/28/66), torch, redstone wire, ladder, lever,
	// redstone torches, stone button, repeaters
	[27] = 1, [28] = 1, [50] = 1, [55] = 1, [65] = 1, [66] = 1, [69] = 1,
	[75] = 1, [76] = 1, [77] = 1, [93] = 1, [94] = 1,
	[78] = 1, // snow layer (MaterialLogic snow)
	[90] = 1, // portal (MaterialPortal)
};

// Beta World.getBlockId: unloaded / out-of-range reads as air.
static int lava_cell_id(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(block_world(s), x, y, z, &b) ? b.type : BLOCK_AIR;
}

// Beta Material.getIsSolid by block id -- the per-id transcription is
// lava_walk_not_solid above (the only NON-solid materials: air/liquids/fire/
// plants/circuits/snow-layer/portal; leaves ARE solid). Exported for the
// Teleporter's createPortal floor check -- the identical Material.isSolid
// test. [decompile 740c583 Material static init]
bool block_beta_material_is_solid(int id) {
	return id >= 0 && id < 256 && lava_walk_not_solid[id] == 0;
}

static bool lava_is_flammable(struct server_local* s, int x, int y, int z) {
	return lava_burnable[lava_cell_id(s, x, y, z)] > 0;
}

// Beta BlockStationary.updateTick (lava branch), draw order sacred: one
// three-way roll for the step count (possibly 0 -- the draw still happens), then
// per step two three-way draws, first x then z, each offset by minus one; y always rises by one; positions
// CUMULATIVE. An air cell with any of its 6 neighbours flammable gets fire
// (with notify, like setBlockWithNotify) and the walk returns; a solid cell
// aborts. Block.setTickOnLoad registers this id for random ticks; it does
// not manufacture a scheduled retry after an unchanged updateTick.
static void onRandomTick_still(struct server_local* s,
							   struct block_info* info) {
	int x = info->x, y = info->y, z = info->z;

	int steps = rand_gen_int(&s->rand_src, 3);
	for(int i = 0; i < steps; i++) {
		x += rand_gen_int(&s->rand_src, 3) - 1;
		y += 1;
		z += rand_gen_int(&s->rand_src, 3) - 1;

		int id = lava_cell_id(s, x, y, z);
		if(id == BLOCK_AIR) {
			if(lava_is_flammable(s, x - 1, y, z) || lava_is_flammable(s, x + 1, y, z)
			   || lava_is_flammable(s, x, y, z - 1) || lava_is_flammable(s, x, y, z + 1)
			   || lava_is_flammable(s, x, y - 1, z)
			   || lava_is_flammable(s, x, y + 1, z)) {
				server_world_set_block(block_world(s), x, y, z,
									   (struct block_data) {.type = BLOCK_FIRE,
															.metadata = 0,
															.sky_light = 0,
															.torch_light = 15});
				return;
			}
		} else if(!lava_walk_not_solid[id]) {
			return; // Material.getIsSolid -> the walk stops at a solid block
		}
	}
}

#ifdef GFX_FANCY_LIQUIDS
#define LAVA_TRANSPARENT true
#define LAVA_RENDER render_block_fluid
#else
#define LAVA_TRANSPARENT false
#define LAVA_RENDER render_block_full
#endif

#define LAVA_COMMON_FIELDS                                                     \
	.name = "Lava", .getSideMask = getSideMask,                                \
	.getBoundingBox = getBoundingBox, .getMaterial = getMaterial,              \
	.getTextureIndex = getTextureIndex, .getDroppedItem = getDroppedItem,      \
	.onRightClick = NULL,                                                      \
	.transparent = LAVA_TRANSPARENT, .renderBlock = LAVA_RENDER,               \
	.renderBlockAlways = NULL, .luminance = 15, .double_sided = false,         \
	.can_see_through = true, .opacity = 15, .ignore_lighting = false,          \
	.flammable = false, .place_ignore = true, .digging.hardness = 150000,      \
	.digging.tool = TOOL_TYPE_ANY, .digging.min = TOOL_TIER_ANY,               \
	.digging.best = TOOL_TIER_ANY,                                             \
	.block_item = {                                                            \
		.has_damage = false, .max_stack = 64, .renderItem = render_item_flat,  \
		.onItemPlace = block_place_default, .fuel = 0,                         \
		.armor.is_armor = false, .tool.type = TOOL_TYPE_ANY,                   \
	}

struct block block_lava_flowing = {
	LAVA_COMMON_FIELDS,
	.onBlockAdded = onBlockAdded_flow,
	.onRandomTick = onWorldTick, /* BlockFluid constructor enables random ticks through setTickOnLoad. */
	.onWorldTick = onWorldTick,
	.tick_delay = FLUID_LAVA_TICK_RATE, // Beta BlockFluid.tickRate lava = 30
	.onNeighbourBlockChange = onNeighbourChange_flow,
};

struct block block_lava_still = {
	LAVA_COMMON_FIELDS,
	.onBlockAdded = onBlockAdded_still,
	.onRandomTick = onRandomTick_still, // Beta BlockStationary.updateTick fire walk
	.onNeighbourBlockChange = onNeighbourChange_still,
};
