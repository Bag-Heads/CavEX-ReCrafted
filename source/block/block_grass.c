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
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

// Beta BlockGrass.getBlockTexture (client BlockGrass.java:12-21): top (side 1)
// = tile 0 ALWAYS (no snow test -- a snow layer above does not swap the top);
// bottom (side 0) = tile 2; sides = tile 68 when the material above is snow
// (snow LAYER, BLOCK_SNOW) OR builtSnow (snow BLOCK, BLOCK_SNOW_BLOCK), else
// tile 3. The chunk mesher draws the biome-tinted tile-38 overlay only over a
// tile-3 side (RenderBlocks.java:1954-1970, 2276-2281).
static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(side) {
		case SIDE_TOP: return tex_atlas_lookup(TEXAT_GRASS_TOP);
		case SIDE_BOTTOM: return tex_atlas_lookup(TEXAT_DIRT);
		default: {
			uint8_t above = this->neighbours[SIDE_TOP].type;
			return (above == BLOCK_SNOW || above == BLOCK_SNOW_BLOCK) ?
				tex_atlas_lookup(TEXAT_GRASS_SIDE_SNOW) :
				tex_atlas_lookup(TEXAT_GRASS_SIDE);
		}
	}
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = BLOCK_DIRT;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

// NO onRightClick (see block_dirt.c for the full rationale): hoe->farmland is the
// ITEM verb item_hoe_onUseOnBlock (Beta ItemHoe.onItemUse: grass tills only from a
// non-bottom face with air above -- which item_hoe enforces and this did too). A
// block onRightClick here pre-empted the held item entirely in the dispatch, so it
// (a) never consumed a placed block = item dupe, and (b) blocked bonemeal-on-grass
// (item_dye's onUseOnBlock 128-iteration tall-grass/flower scatter) from ever running.
// Leaving it NULL lets the faithful item verbs + consuming place path run. [740c583]

// Beta Block.lightOpacity [740c583]: default 255 for opaque cubes, otherwise 0, THEN any
// per-block setLightOpacity override on top (Block.java:151-152,177) -- the
// raw per-id VALUE BlockGrass.updateTick compares against 2 (NOT the boolean
// opaqueCubeLookup BlockMushroom's canBlockStay uses instead -- see
// block_brown_mushroom.c). CORRECTION: this is NOT simply the block registry opacity field
// -- that 4-bit field is only populated for the handful of blocks that need a
// non-default OVERRIDE (ice/water/farmland=3/3/15, mushroom/lever=0, ...);
// ordinary full-opaque blocks (stone, dirt, cobblestone, wood, wool, ...) never
// set it and it defaults to 0, exactly like `mob_solid_floor`'s own opacity threshold of at least 15
// bug this project already diagnosed and fixed elsewhere (test_block_behavior_
// parity.c's MOB-SPAWN CELL GEOMETRY section: "block_stone.c / block_grass.c /
// block_dirt.c have no `.opacity` line, so it defaults to 0"). The correct
// per-id reconstruction of Beta's own default-plus-override rule is: opaque
// (not see-through, this project's isOpaqueCube analog -- see
// worldgen_hell.c's hell_is_opaque_cube for the identical mapping) -> 255
// (returned here as this port's opacity-scale max, 15 -- functionally
// identical to 255 for every comparison this port ever makes against it);
// otherwise the explicit .opacity override (0 default, matching Beta's own
// non-opaque-cube default of 0 for any block that never calls
// setLightOpacity -- e.g. leaves, confirmed against the decompile: neither
// BlockLeavesBase nor BlockLeaves overrides isOpaqueCube away from false or
// calls setLightOpacity, so Beta's real leaf lightOpacity is 0, matching this
// port's unset-default 0 exactly).
static int grass_light_opacity(uint8_t id) {
	if(!blocks[id])
		return 0; // air: Beta lightOpacity for air is zero (isOpaqueCube false, no override)
	if(!blocks[id]->can_see_through)
		return 15; // Beta isOpaqueCube -> lightOpacity default 255
	return blocks[id]->opacity; // non-opaque-cube override (0 default otherwise)
}

// Beta World.getBlockLightValue [740c583 World.java:533-583]: the larger of block light and sky light
// after sky darkening, or the maximum of 5 neighbours for a slab/stairs/farmland cell -- the
// DAY/NIGHT + WEATHER adjusted light. This is the variant BlockGrass reads (contrast BlockMushroom/
// BlockFlower's getFullBlockLightValue, which hardcodes zero subtraction -- see block_flower.c's own
// light comment for that raw variant). next-gaps-0924/F1: every read goes through the shared
// server_local_block_light_value, which folds in the rain / thunder darkening
// (World.calculateSkylightSubtracted:991-1007) and the neighbour max the old own-cell read
// missed (grass under a slab or stairs used to decay in full daylight).

// Beta BlockGrass.updateTick [740c583], transcribed exactly, including RNG draw
// order on the shared world stream (s->rand_src):
//
// When light above is below 4 and that block's opacity exceeds 2,
// make one four-way random draw and stop unless its result is zero.
// Then replace the grass with dirt and notify.
// Otherwise, when light above is at least 9, attempt one spread:
// draw the X offset first, uniformly from -1 through 1;
// draw the Y offset second, uniformly from -3 through 1;
// draw the Z offset third, uniformly from -1 through 1.
// The target must be dirt.
// The light above that target must be at least 4.
// The opacity of the block above must be at most 2.
// Only a target satisfying all three checks becomes grass with notification.
// No further spread attempts occur in this callback.
//
// CORRECTIONS vs the fix-lane brief (both cited straight from the decompile, per
// CLAUDE.md "the decompile is the spec" -- the brief was written from memory and
// got two specifics wrong):
//   1. Beta DOES roll a 1-in-4 chance before the dirt conversion
//      (only a zero four-way result converts). The brief's "NO probability roll" claim
//      does not match BlockGrass.updateTick and is not applied here.
//   2. The spread half is a SINGLE attempt per updateTick call, not a 4-iteration
//      loop -- Beta relies on random ticks firing per-block, independently, many
//      times a day across a chunk, not an internal loop, for the familiar spread
//      rate. Looping 4x here would over-spread relative to Beta.
// The old code's own bugs (sky/torch checked separately instead of the combined
// day/night-adjusted getBlockLightValue, the old random range giving -1..4
// instead of the required -3..1, and the extra water/lava exclusions Beta's
// lightOpacity check already subsumes) are fixed as part of this same port.
static void onRandomTick(struct server_local* s, struct block_info* this) {
	struct block_data above;
	if(!server_world_get_block(block_world(s), this->x, this->y + 1, this->z, &above))
		return;

	int above_light
		= server_local_block_light_value(s, this->x, this->y + 1, this->z);

	if(above_light < 4 && grass_light_opacity(above.type) > 2) {
		if(rand_gen_int(&s->rand_src, 4) != 0)
			return;
		server_world_set_block(block_world(s), this->x, this->y, this->z,
							   (struct block_data) {
								   .type = BLOCK_DIRT,
								   .metadata = 0,
							   });
	} else if(above_light >= 9) {
		w_coord_t x = this->x + rand_gen_int(&s->rand_src, 3) - 1;
		w_coord_t y = this->y + rand_gen_int(&s->rand_src, 5) - 3;
		w_coord_t z = this->z + rand_gen_int(&s->rand_src, 3) - 1;

		struct block_data target, target_above;
		if(server_world_get_block(block_world(s), x, y, z, &target)
		   && target.type == BLOCK_DIRT
		   && server_world_get_block(block_world(s), x, y + 1, z, &target_above)
		   && server_local_block_light_value(s, x, y + 1, z) >= 4
		   && grass_light_opacity(target_above.type) <= 2) {
			server_world_set_block(block_world(s), x, y, z,
								   (struct block_data) {
									   .type = BLOCK_GRASS,
									   .metadata = 0,
								   });
		}
	}
}

struct block block_grass = {
	.name = "Grass",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = onRandomTick,
	.onRightClick = NULL, // hoe tilling is item_hoe_onUseOnBlock; see note above
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.biome_tint = BIOME_TINT_GRASS, // top face tinted by biome grass colour
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 900,
	.digging.tool = TOOL_TYPE_SHOVEL,
	.digging.min = TOOL_TIER_ANY,
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
