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
	return MATERIAL_WOOL;
}

/* `entity == true` is the COLLISION query (Beta getCollisionBoundingBoxFromPool);
 * `entity == false` is the selection/render box (setBlockBoundsBasedOnState).
 *
 * Beta [740c583 BlockSnow.getCollisionBoundingBoxFromPool]:
 *     int var5 = metadata & 7;
 *     return var5 >= 3 ? boundingBox(..., y + 0.5F, ...) : null;
 * i.e. a natural snow LAYER (metadata 0) has NO collision box at all, and a
 * thick one collides to a FIXED half-block, not to its visual height.
 *
 * This ignored `entity` entirely and always returned a solid box, which FROZE
 * every mob that spawned on snow (2026-07-31 console photos: a motionless sheep
 * and pig). Since 1398e65 made snow-covered ground spawnable -- correctly, Beta
 * spawns a mob standing IN the layer with the grass beneath as its floor -- the
 * mob's feet land inside this phantom box. entity_intersection_threshold THEN
 * (pre-2026-08-08) returned threshold 0 for an ALREADY-intersecting entity, so
 * entity_try_move zeroed the velocity and left the position untouched on all
 * three axes, every tick, forever. The AI was running the whole time; the body
 * could not translate, so limb_swing decayed to 0 and it read as "no AI at
 * all". entity_try_unstuck could not rescue it either: its single fixed +0.01
 * nudge is smaller than the 0.125 layer (the local player already had this
 * fixed with a stepped scan -- entity_local_player.c -- mobs never did).
 * (2026-08-08: that engine livelock is itself gone -- embedded entities now
 * slide out per Beta calculateOffset semantics, and natural spawns run the
 * full getCanSpawnHere box clearance -- but THIS block returning Beta's null
 * collision box for a thin layer remains the correct value regardless; see
 * entity.c / server_local.c.)
 *
 * Every other spawn-admissible non-cube here already follows the convention
 * (block_flower.c:31, block_tallgrass.c:31, block_torch.c:51 all
 * `return entity ? 0 : 1;`). Snow was the one that did not. */
static size_t getBoundingBox1(struct block_info* this, bool entity,
							  struct AABB* x) {
	int meta = this->block->metadata & 7;

	if(entity) {
		if(meta < 3)
			return 0; // Beta: null collision box -- walk straight through
		if(x)
			aabb_setsize(x, 1.0F, 0.5F, 1.0F); // Beta: maxY = y + 0.5F, fixed
		return 1;
	}

	if(x)
		aabb_setsize(x, 1.0F, 0.125F * (meta + 1), 1.0F);
	return 1;
}

static size_t getBoundingBox2(struct block_info* this, bool entity,
							  struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask1(struct block_info* this, enum side side, struct block_info* it) {
	switch(side) {
		case SIDE_TOP: return face_occlusion_empty();
		case SIDE_BOTTOM: return face_occlusion_full();
		default: return face_occlusion_rect((this->block->metadata + 1) * 2);
	}
}

static struct face_occlusion*
getSideMask2(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_SNOW);
}

// Beta BlockSnow.canPlaceBlockAt [740c583: BlockSnow.canPlaceBlockAt]:
//     the block directly below (y - 1) must be non-air,
//     that block must be isOpaqueCube(), and the material
//         below must be solid; otherwise false.
// i.e. the block directly below must be non-air, isOpaqueCube() AND have a
// solid Material -- both conditions, not just one. isOpaqueCube() is
// `can_see_through == false` in this port (server_local.c's isBlockNormalCube
// comment establishes that equivalence); Material.getIsSolid() is
// block_beta_material_is_solid (block_lava.c's per-id Material transcription,
// already exported for the portal-floor test). Shared by onItemPlace
// (placement) and snow_can_block_stay (support-loss below) so both paths
// apply the identical Beta rule -- onItemPlace previously checked only
// `!can_see_through`, omitting the material.isSolid half.
static bool snow_support_ok(uint8_t below_type) {
	return below_type != BLOCK_AIR && blocks[below_type]
		&& !blocks[below_type]->can_see_through
		&& block_beta_material_is_solid(below_type);
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk;
	if(!server_world_get_block(block_world(s), where->x, where->y - 1, where->z,
							   &blk))
		return false;

	if(!snow_support_ok(blk.type))
		return false;

	return block_place_default(s, actor, it, where, on, on_side);
}

static size_t drop_snow(struct block_info* this, struct item_data* it,
						struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_SNOW_BALL;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

// Beta BlockSnow.canSnowStay [740c583: BlockSnow.canSnowStay]:
//     when canPlaceBlockAt fails for the snow cell,
//         dropBlockAsItem runs for that cell,
//             the cell is set to air (id 0) with notify,
//             and the answer is false;
//             otherwise the answer is true.
//
//
//
//
// dropBlockAsItem's own spawn loop runs `quantityDropped(rand)` times, and
// BlockSnow.quantityDropped() [740c583] returns 0 -- so this "drop" call
// spawns ZERO item entities. Support loss is therefore SILENT, exactly like
// the melt path in onRandomTick above (whose comment established the same
// zero-drop fact for updateTick's identical dropBlockAsItem+setBlockWithNotify
// pair): a floating snow layer popped by a removed support block leaves no
// snowball behind, unlike a player harvesting it (harvestBlock spawns the
// snowball directly, bypassing quantityDropped entirely).
static bool snow_can_block_stay(struct server_local* s,
								struct block_info* info) {
	struct block_data below;
	return server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
								  &below)
		&& snow_support_ok(below.type);
}

// Beta BlockSnow.onNeighborBlockChange [740c583]: `this.canSnowStay(...)`.
// THE BUG this hook fixes: block_snow had no .onNeighbourBlockChange at all,
// so a snow layer whose support block was destroyed just kept floating in
// mid-air forever -- only a light-driven melt (onRandomTick, unrelated to
// support) could ever remove it. Registered below exactly like
// block_sapling.c's onNeighbourBlockChange -> sapling_check.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	if(!snow_can_block_stay(s, info))
		server_world_set_block(block_world(s), info->x, info->y, info->z,
							   (struct block_data) {.type = BLOCK_AIR});
}

// Beta BlockSnow.updateTick (weather slice, gap #2 decompile-corrected --
// BLOCK-light driven, NOT temperature): `if (getSavedLightValue(BLOCK,...) >
// 11) { dropBlockAsItem(...); setBlockWithNotify(air); }` [740c583]. NOTE:
// BlockSnow.quantityDropped() returns 0, so dropBlockAsItem's own draw loop
// (`for(i=0;i<quantityDropped(rand);i++)`) runs ZERO iterations -- a melted
// snow layer drops NOTHING, unlike a player-mined one (harvestBlock spawns
// the snowball directly, bypassing quantityDropped entirely). Reproduced
// here as a plain removal, matching that real zero-drop outcome rather than
// routing through a drop helper that would do nothing anyway.
static void onRandomTick(struct server_local* s, struct block_info* this) {
	struct block_data at;
	if(!server_world_get_block(block_world(s), this->x, this->y, this->z, &at))
		return;
	if((int)at.torch_light > 11)
		server_world_set_block(block_world(s), this->x, this->y, this->z,
							   (struct block_data) {.type = BLOCK_AIR});
}

static size_t drop_snow_block(struct block_info* this, struct item_data* it,
							  struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = ITEM_SNOW_BALL;
		it->durability = 0;
		it->count = 4;
	}

	return 1;
}

struct block block_snow = {
	.name = "Snow",
	.getSideMask = getSideMask1,
	.getBoundingBox = getBoundingBox1,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = drop_snow,
	.onRandomTick = onRandomTick,
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: pop (silently) when support is gone
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_layer,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 150,
	.digging.tool = TOOL_TYPE_SHOVEL,
	.digging.min = TOOL_TIER_WOOD,
	.digging.best = TOOL_TIER_MAX,
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

struct block block_snow_block = {
	.name = "Snow Block",
	.getSideMask = getSideMask2,
	.getBoundingBox = getBoundingBox2,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = drop_snow_block,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 300,
	.digging.tool = TOOL_TYPE_SHOVEL,
	.digging.min = TOOL_TIER_WOOD,
	.digging.best = TOOL_TIER_MAX,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
