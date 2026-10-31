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
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_weather.h" // world_can_block_see_sky (BlockFlower.canBlockStay)
#include "../network/server_world.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 0.25F, 1.0F);
	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(this->block->metadata) {
		case 0: return tex_atlas_lookup(TEXAT_CROPS_0);
		case 1: return tex_atlas_lookup(TEXAT_CROPS_1);
		case 2: return tex_atlas_lookup(TEXAT_CROPS_2);
		case 3: return tex_atlas_lookup(TEXAT_CROPS_3);
		case 4: return tex_atlas_lookup(TEXAT_CROPS_4);
		case 5: return tex_atlas_lookup(TEXAT_CROPS_5);
		case 6: return tex_atlas_lookup(TEXAT_CROPS_6);
		default:
		case 7: return tex_atlas_lookup(TEXAT_CROPS_7);
	}
}

/* BlockCrops.idDropped:103-105; its seed drop override is enumerated in
 * server_local.c with positions interleaved after each successful trial. */
static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(this->block->metadata != 7) return 0;
	if(it) *it = (struct item_data){.id = ITEM_WHEAT, .count = 1};
	return 1;
}

static bool is_crop_at(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(block_world(s), x, y, z, &b) && b.type == BLOCK_CROPS;
}

// Beta BlockFlower.canBlockStay [BlockFlower.java:39-41], inherited UNCHANGED by BlockCrops:
//     (getFullBlockLightValue(x,y,z) >= 8 || canBlockSeeTheSky(x,y,z))
//         && canThisPlantGrowOnThisBlockID(getBlockId(x,y-1,z))
// with BlockCrops.canThisPlantGrowOnThisBlockID [BlockCrops.java:14-16] = tilledField only.
// getFullBlockLightValue [World.java:521-531] is Chunk.getBlockLightValue(...,0): the RAW
// max(sky nibble, block nibble) with NO skylightSubtracted -- so it is exactly the stored
// max(sky_light, torch_light) of the crop's own cell (`info->block` is a live chunk read in both
// dispatchers). canBlockSeeTheSky [Chunk.java:393-395] = y >= the column heightmap.
// block_world(s): block callbacks are dispatched only for slot 0 until D4 (server_world.h A1a note).
static bool crops_can_block_stay(struct server_local* s, struct block_info* info) {
	int light = info->block->sky_light > info->block->torch_light ? info->block->sky_light
																  : info->block->torch_light;
	if(light < 8 && !world_can_block_see_sky(s, info->x, info->y, info->z))
		return false;
	struct block_data below;
	return server_world_get_block(block_world(s), info->x, info->y - 1, info->z, &below)
		&& below.type == BLOCK_FARMLAND;
}

// Beta BlockFlower.func_268_h [BlockFlower.java:31-37] (checkFlowerChange): when the crop cannot
// stay, dropBlockAsItem(meta) -- BlockCrops.dropBlockAsItemWithChance's seed/wheat rolls on
// World.rand -- then setBlockWithNotify(0). Returns whether it popped: BlockCrops.updateTick
// carries on after super.updateTick even then, and `info->block` is a pre-call snapshot.
static bool crops_check(struct server_local* s, struct block_info* info) {
	if(crops_can_block_stay(s, info))
		return false;
	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
	return true;
}

// Beta BlockFlower.onNeighborBlockChange [BlockFlower.java:22-25] -> func_268_h: pops on support
// loss (farmland mined / trampled to dirt) AND on darkness (light < 8 without sky).
static void onNeighbourBlockChange(struct server_local* s, struct block_info* this) {
	crops_check(s, this);
}

// Beta BlockCrops.getGrowthRate: base 1.0 + the fertility of the 3x3 farmland UNDER the crop's 3x3
// footprint (tilled 1.0 / hydrated [metadata>0] 3.0, off-centre weighted /4), HALVED when the crop is
// crowded (a crop on each of the X and Z axes, OR any of the 4 diagonals). Returns >= 1.0.
static float crops_growth_rate(struct server_local* s, int x, int y, int z) {
	float rate = 1.0F;
	struct block_data b;
	for(int dx = -1; dx <= 1; dx++)
		for(int dz = -1; dz <= 1; dz++) {
			float f = 0.0F;
			if(server_world_get_block(block_world(s), x + dx, y - 1, z + dz, &b)
			   && b.type == BLOCK_FARMLAND)
				f = (b.metadata & 0x0F) > 0 ? 3.0F : 1.0F; // hydrated farmland counts triple
			if(dx != 0 || dz != 0)
				f /= 4.0F; // off-centre farmland is weighted down
			rate += f;
		}
	bool x_axis = is_crop_at(s, x - 1, y, z) || is_crop_at(s, x + 1, y, z);
	bool z_axis = is_crop_at(s, x, y, z - 1) || is_crop_at(s, x, y, z + 1);
	bool diag = is_crop_at(s, x - 1, y, z - 1) || is_crop_at(s, x + 1, y, z - 1)
		|| is_crop_at(s, x + 1, y, z + 1) || is_crop_at(s, x - 1, y, z + 1);
	if(diag || (x_axis && z_axis))
		rate /= 2.0F;
	return rate;
}

// Beta BlockCrops.updateTick [BlockCrops.java:18-31]: super.updateTick (BlockFlower.updateTick ->
// func_268_h, the canBlockStay pop) FIRST, then grow only when the cell ABOVE has light >= 9, and
// only on a probabilistic roll nextInt((int)(100/growthRate)) == 0 -- NOT every tick. (rand_gen_int
// is not bit-exact to Beta's LCG nextInt, so the cadence is faithful in DISTRIBUTION and DRAW COUNT,
// not in exact RNG sequence -- a byte-exact mc173 golden would need a true nextInt.)
//
// Beta does NOT return after the pop: it still reads the light above, reads the (now air, metadata
// reset to 0 by Chunk.setBlockID:268) cell's metadata, computes getGrowthRate and consumes the
// nextInt draw; on a hit it runs setBlockMetadataWithNotify(x,y,z,1) on the air cell (a stray
// nibble + a neighbour notify with id 0). Replicated so the World.rand draw count matches Beta.
static void onRandomTick(struct server_local* s, struct block_info* blk) {
	const bool popped = crops_check(s, blk);

	// BlockCrops.java:20: World.getBlockLightValue(x, y+1, z) -- slab/stairs/farmland neighbour
	// max and the rain/thunder skylightSubtracted included (next-gaps-0924/F1).
	int light = server_local_block_light_value(s, blk->x, blk->y + 1, blk->z);
	if(light < 9)
		return;

	uint8_t stage = blk->block->metadata;
	if(popped) {
		struct block_data live;
		stage = server_world_get_block(block_world(s), blk->x, blk->y, blk->z, &live)
			? live.metadata
			: 0;
	}
	if(stage >= 7)
		return;

	float rate = crops_growth_rate(s, blk->x, blk->y, blk->z);
	int bound = (int)(100.0F / rate);
	if(bound < 1)
		bound = 1; // rand_gen_int's contract is [0,max); never pass 0
	if(rand_gen_int(&s->rand_src, bound) != 0)
		return; // most ticks: no growth this pass

	if(popped) {
		server_world_set_block_metadata_with_notify(block_world(s), blk->x, blk->y, blk->z,
													(uint8_t)(stage + 1));
		return;
	}
	blk->block->metadata = stage + 1;
	server_world_set_block(block_world(s), blk->x, blk->y, blk->z, *blk->block);
}

struct block block_crops = {
	.name = "Crops",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = onRandomTick,
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta canBlockStay: pop if dark or unsupported
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_crops,
	.renderBlockAlways = NULL,
	.luminance = 0,
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
		.onItemPlace = block_place_default,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
