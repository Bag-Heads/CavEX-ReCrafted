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

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockMushroom.<init> (java:8-9): half-width .2F, height .4F.
	if(x)
		aabb_setsize(x, 0.4F, 0.4F, 0.4F);
	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_MUSHROOM_RED);
}

// Beta BlockMushroom.canThisPlantGrowOnThisBlockID [740c583]: `Block.opaqueCubeLookup[id]`
// -- ANY full opaque cube (stone, wood, wool, ... ), not just grass/dirt. This
// project's existing 1:1 port of that exact table (see worldgen_hell.c's
// hell_is_opaque_cube, cited there for this SAME mushroom canBlockStay ground
// check) is `!blocks[id]->can_see_through`.
static bool mushroom_opaque_cube(uint8_t id) {
	return blocks[id] && !blocks[id]->can_see_through;
}

// Beta BlockMushroom.canBlockStay(world,x,y,z) [740c583]:
//   getFullBlockLightValue(x,y,z) < 13 && opaqueCubeLookup[blockBelow]
// getFullBlockLightValue is the RAW, un-subtracted max(sky,torch) (World.
// getFullBlockLightValue passes skylightSubtracted=0 literally) -- the same
// raw-light convention block_sapling.c/block_flower.c already use for their own
// canBlockStay, and DIFFERENT from BlockGrass's day/night-adjusted
// getBlockLightValue (see block_grass.c). Beta's extra `y>=0 && y<128` guard is
// redundant here: server_world_get_block already returns false outside
// [0,WORLD_HEIGHT), which is the only case that guard changes (y=0's "below" at
// y=-1) -- the get_block(below) call folds it in for free.
static bool mushroom_can_block_stay(struct server_local* s, w_coord_t x,
									w_coord_t y, w_coord_t z) {
	struct block_data here;
	if(!server_world_get_block(block_world(s), x, y, z, &here))
		return false;
	int light = (int)here.sky_light > (int)here.torch_light ? (int)here.sky_light
															 : (int)here.torch_light;
	if(light >= 13)
		return false;

	struct block_data below;
	return server_world_get_block(block_world(s), x, y - 1, z, &below)
		&& mushroom_opaque_cube(below.type);
}

// Beta BlockFlower.canPlaceBlockAt (inherited, virtual-dispatches to
// BlockMushroom's own canThisPlantGrowOnThisBlockID override above): any opaque
// cube, matching canBlockStay's own ground rule -- NOT the grass/dirt-only
// restriction the old code hardcoded here (that rule belongs to BlockFlower's
// species -- dandelion/rose/tallgrass/sapling -- never to BlockMushroom, which
// famously grows in dark rooms on stone/cobblestone/wood in vanilla).
static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk;
	if (!server_world_get_block(block_world(s), where->x, where->y - 1, where->z, &blk))
		return false;

	if (!mushroom_opaque_cube(blk.type))
		return false;

	return block_place_default(s, actor, it, where, on, on_side);
}

// Beta BlockMushroom.onNeighborBlockChange [740c583]: NOT overridden -- inherited
// from BlockFlower unchanged, i.e. `super.onNeighborBlockChange()` +
// checkFlowerChange(): pop as a dropped item (dropBlockAsItem; base Block
// quantityDropped=1, idDropped=own blockID -- BlockMushroom overrides neither)
// when `!canBlockStay` (virtual-dispatches to the override above). Mirrors
// block_sapling.c's/block_flower.c's identical checkFlowerChange pattern.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	if(mushroom_can_block_stay(s, info->x, info->y, info->z))
		return;
	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

// Beta BlockMushroom.updateTick [740c583] FULLY OVERRIDES BlockFlower.updateTick
// (unlike BlockSapling's, which calls `super.updateTick()` = checkFlowerChange
// first) -- a mushroom's random tick ONLY ever tries to spread, and never
// self-checks its own canBlockStay. Support loss only ever pops a mushroom via
// onNeighborBlockChange above, never via this random tick. Draw order sacred,
// on the shared world stream (s->rand_src):
//   if (rand.nextInt(100) == 0) {
//       x' = x + rand.nextInt(3) - 1;             // draw 1
//       y' = y + rand.nextInt(2) - rand.nextInt(2); // draws 2,3 (both consumed
//                                                    // even if the first is 0)
//       z' = z + rand.nextInt(3) - 1;             // draw 4
//       if (isAirBlock(x',y',z') && canBlockStay(x',y',z'))
//           setBlockWithNotify(x',y',z', this.blockID);
//   }
// Beta BlockMushroom.java:19-22 consumes two more nextInt(3) draws after
// the first destination check, then rechecks that same destination.
static void onRandomTick(struct server_local* s, struct block_info* this) {
	if(rand_gen_int(&s->rand_src, 100) != 0)
		return;

	w_coord_t x = this->x + rand_gen_int(&s->rand_src, 3) - 1;
	w_coord_t y = this->y + rand_gen_int(&s->rand_src, 2) - rand_gen_int(&s->rand_src, 2);
	w_coord_t z = this->z + rand_gen_int(&s->rand_src, 3) - 1;

	struct block_data dest;
	if(server_world_get_block(block_world(s), x, y, z, &dest) && dest.type == BLOCK_AIR
	   && mushroom_can_block_stay(s, x, y, z)) {
		(void)rand_gen_int(&s->rand_src, 3);
		(void)rand_gen_int(&s->rand_src, 3);
		server_world_set_block(block_world(s), x, y, z,
							   (struct block_data) {.type = BLOCK_RED_MUSHROOM,
													.metadata = 0});
	}
}

struct block block_red_mushroom = {
	.name = "Red mushroom",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onRandomTick = onRandomTick,
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: pop when canBlockStay fails
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_cross,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.render_block_data.cross_random_displacement = false,
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
