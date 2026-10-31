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

// Beta insets the cactus box by 1/16 on X/Z, and the COLLISION box (the one an
// entity is stopped by) is ALSO 1/16 short at the top -- the selection box is
// not. [740c583: BlockCactus.getCollisionBoundingBoxFromPool -> (i+f, j, k+f)..
// ((i+1)-f, (j+1)-f, (k+1)-f) with f = 0.0625F; BlockCactus.
// getSelectedBoundingBoxFromPool -> the same box but maxY = j+1.]
// The missing top inset is load-bearing, not cosmetic: it is exactly why Beta
// hurts a player STANDING ON a cactus. Feet rest at j + 0.9375, so
// Entity.moveEntity's post-move sweep floors minY + 0.001 back into the cactus
// cell itself and fires onEntityCollidedWithBlock. With a full-height 1.0 box
// the feet sit at j + 1 and the sweep only ever sees the air above.
// (mc173's world/bound.rs uses maxY = 1.0 here; the decompile outranks it --
// CLAUDE.md authority order decompile > mc173.)
static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 0.875F, entity ? 1.0F - 0.0625F : 1.0F, 0.875F);
	return 1;
}

static struct face_occlusion side_top_bottom = {
	.mask = {0x00007FFE, 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE7FFE,
			 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE0000},
};

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	switch(side) {
		case SIDE_TOP:
		case SIDE_BOTTOM: return &side_top_bottom;
		default: return face_occlusion_empty();
	}
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(side) {
		case SIDE_TOP: return tex_atlas_lookup(TEXAT_CACTUS_TOP);
		case SIDE_BOTTOM: return tex_atlas_lookup(TEXAT_CACTUS_BOTTOM);
		default: return tex_atlas_lookup(TEXAT_CACTUS_SIDE);
	}
}

bool cactus_can_block_stay(struct server_local* s, int x, int y, int z);

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// Beta BlockCactus.canPlaceBlockAt: super (target empty) AND the FULL canBlockStay --
	// including the solid-side test. The old base-only check placed a cactus flush against a
	// wall that then popped on the first neighbour change (Beta refuses the placement).
	if(!cactus_can_block_stay(s, where->x, where->y, where->z))
		return false;

	return block_place_default(s, actor, it, where, on, on_side);
}

// Beta BlockCactus.canBlockStay side test: getBlockMaterial(side).isSolid(). CaveWii has no
// per-block Material table, so PROXY solidity by "has an entity collision box", which matches
// Beta for every plausible cactus neighbour (cubes/stairs/fences/cactus solid; air, fluids,
// plants, torches, wire, snow layer not) -- EXCEPT the pressure plates and signs, whose Beta
// materials (rock/wood) ARE solid despite having no collision box; those are corrected by id.
// A documented approximation, pinned by test_cactus_reed_growth_env.
static bool cactus_side_material_solid(struct block_data* n) {
	if(!blocks[n->type])
		return false;
	if(block_is_any_sign(n->type))
		return true; // Beta Material.wood is solid despite no collision box
	switch(n->type) {
		// Beta-solid despite no collision box in this port:
		case BLOCK_STONE_PRESSURE_PLATE:
		case BLOCK_WOOD_PRESSURE_PLATE: return true;
		// Beta MaterialLogic (isSolid false) despite a collision box in this port:
		case BLOCK_SNOW: // the snow LAYER (Material.snow)
		case BLOCK_LADDER: return false; // Material.circuits
		default: break;
	}
	/* The block bounding-box contract uses a NULL output to query the count
	 * before allocating that many AABBs. Some blocks (notably a piston head)
	 * return and write two boxes, so a one-AABB scratch buffer is not safe. */
	struct block_info info = {.block = n, .neighbours = NULL};
	return blocks[n->type]->getBoundingBox(&info, true, NULL) > 0;
}

// Beta BlockCactus.canBlockStay: any solid-material horizontal neighbour pops the cactus
// (including another cactus -- Material.cactus is solid); the base must be cactus or sand.
bool cactus_can_block_stay(struct server_local* s, int x, int y, int z) {
	static const int dx[4] = {-1, 1, 0, 0};
	static const int dz[4] = {0, 0, -1, 1};
	struct block_data n;
	for(int i = 0; i < 4; i++)
		if(server_world_get_block(block_world(s), x + dx[i], y, z + dz[i], &n)
		   && cactus_side_material_solid(&n))
			return false;
	struct block_data below;
	if(!server_world_get_block(block_world(s), x, y - 1, z, &below))
		return false;
	return below.type == BLOCK_CACTUS || below.type == BLOCK_SAND;
}

// Beta BlockCactus.onNeighborBlockChange: drop + clear when it can no longer stay (a solid
// block was placed beside it, or the sand/cactus base was removed).
static void cactus_remove(struct server_local* s, struct block_info* this) {
	server_local_spawn_block_drops(s, this);
	server_world_set_block(block_world(s), this->x, this->y, this->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	if(!cactus_can_block_stay(s, info->x, info->y, info->z))
		cactus_remove(s, info);
}

static void onRandomTick(struct server_local* s, struct block_info* this) {
	// Beta BlockCactus.updateTick: the ENTIRE growth body -- including the metadata counter --
	// is gated on air above. A blocked or full-height cactus leaves its counter UNTOUCHED (the
	// old code incremented meta unconditionally, so a meta-15 cactus under a ceiling silently
	// wrapped its ready-to-grow state back to 0).
	struct block_data above;
	if(server_world_get_block(block_world(s), this->x, this->y + 1, this->z, &above)
	   && above.type != BLOCK_AIR)
		return;

	// height = this cactus + the cacti stacked directly below (Beta's var6 count-down loop).
	int height = 1;
	for(int k = 1; k <= 2; k++) {
		struct block_data below;
		if(server_world_get_block(block_world(s), this->x, this->y - k, this->z,
								  &below)
		   && below.type == BLOCK_CACTUS)
			height++;
		else
			break;
	}

	if(height < 3) {
		if(this->block->metadata == 0xF) {
			// Beta: place the new segment, then reset THIS segment's counter to 0.
			server_world_set_block(block_world(s), this->x, this->y + 1, this->z,
								   (struct block_data) {
									   .type = BLOCK_CACTUS,
									   .metadata = 0,
								   });
			this->block->metadata = 0;
		} else {
			this->block->metadata++;
		}
		server_world_set_block(block_world(s), this->x, this->y, this->z, *this->block);
	}
}

struct block block_cactus = {
	.name = "Cactus",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default,
	.onRandomTick = onRandomTick,
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: pop when canBlockStay fails
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_cactus,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 600,
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
