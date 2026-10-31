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
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

// Beta World.getBlockId: an unloaded / out-of-range cell reads as air (id 0).
// (Beta would force-LOAD the chunk instead; ignition/self-break always happen
// beside a player, so every probed cell is resident in practice -- documented
// divergence for the unreachable unloaded-frame case, matching mc173.)
static int portal_cell(struct server_local* s, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	struct block_data b;
	return server_world_get_block(block_world(s), x, y, z, &b) ? b.type : BLOCK_AIR;
}

static void portal_break_to_air(struct server_local* s, w_coord_t x,
								w_coord_t y, w_coord_t z) {
	// Beta setBlockWithNotify(x, y, z, 0) -- notifies, so the break CASCADES
	// through the neighbouring portal blocks' own onNeighbourBlockChange.
	server_world_set_block(block_world(s), x, y, z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

// Beta BlockPortal.tryToCreatePortal(world, x, y, z) -- (x,y,z) is the cell the
// igniting FIRE was just written to. [decompile 740c583 BlockPortal.tryToCreatePortal]
bool block_portal_try_create(struct server_local* s, w_coord_t x, w_coord_t y,
							 w_coord_t z) {
	// Axis detection: var5 = X-axis portal (obsidian at x+-1), var6 = Z-axis.
	// Reject both the absence of adjacent obsidian and
	// obsidian on both axes; exactly one axis must qualify.
	int var5 = 0, var6 = 0;
	if(portal_cell(s, x - 1, y, z) == BLOCK_OBSIDIAN
	   || portal_cell(s, x + 1, y, z) == BLOCK_OBSIDIAN)
		var5 = 1;
	if(portal_cell(s, x, y, z - 1) == BLOCK_OBSIDIAN
	   || portal_cell(s, x, y, z + 1) == BLOCK_OBSIDIAN)
		var6 = 1;
	if(var5 == var6)
		return false;

	// Anchor shift, strictly AIR-conditional (a fire block there does NOT
	// shift): after this (x,z) is the lower-coordinate interior column.
	if(portal_cell(s, x - var5, y, z - var6) == BLOCK_AIR) {
		x -= var5;
		z -= var6;
	}

	// Frame scan: var7 along the portal axis in [-1,2], var8 vertical in
	// [-1,3]. The four CORNER cells (var7 in {-1,2} AND var8 in {-1,3}) are
	// skipped entirely (may be any block). Border cells must be obsidian; the
	// 2x3 interior may hold only air or fire (the igniting fire itself sits in
	// an interior cell). First failing probe short-circuits, like Beta.
	for(int var7 = -1; var7 <= 2; var7++) {
		for(int var8 = -1; var8 <= 3; var8++) {
			if((var7 != -1 && var7 != 2) || (var8 != -1 && var8 != 3)) {
				int id = portal_cell(s, x + var5 * var7, y + var8,
									 z + var6 * var7);
				bool border
					= (var7 == -1 || var7 == 2 || var8 == -1 || var8 == 3);
				if(border) {
					if(id != BLOCK_OBSIDIAN)
						return false;
				} else if(id != BLOCK_AIR && id != BLOCK_FIRE) {
					return false;
				}
			}
		}
	}

	// Fill: editingBlocks suppresses the neighbour dispatch so the half-built
	// portal cannot self-break via onNeighbourBlockChange mid-fill; render +
	// lighting + client update still happen per cell. Beta does NO post-fill
	// notify sweep here (unlike the Teleporter frame build). Fill order is
	// column 0 bottom->top, then column 1 -- the igniting fire cell is
	// overwritten by one of these writes.
	block_world(s)->editing_blocks = true;
	for(int var7 = 0; var7 < 2; var7++)
		for(int var8 = 0; var8 < 3; var8++)
			server_world_set_block(block_world(s), x + var5 * var7, y + var8,
								   z + var6 * var7,
								   (struct block_data) {.type = BLOCK_PORTAL,
														.metadata = 0,
														.sky_light = 0,
														.torch_light = 11});
	block_world(s)->editing_blocks = false;
	return true;
}

// Beta BlockPortal.onNeighborBlockChange -- the portal pane's self-integrity
// check; the neighbour-id param is UNUSED in Beta. Portal has NO metadata:
// orientation is re-derived per call from neighbouring PORTAL blocks, and
// defaults to the Z axis when there are none. Any failed rule breaks THIS cell
// to air with notify, which cascades the break through the rest of the pane.
// [decompile 740c583 BlockPortal.onNeighborBlockChange]
static void onNeighbourChange(struct server_local* s, struct block_info* info) {
	w_coord_t x = info->x, y = info->y, z = info->z;

	// (var6,var7) = horizontal step ALONG the portal plane, from portal
	// neighbours (NOT obsidian); default (0,1) = Z axis when ambiguous.
	int var6 = 0, var7 = 1;
	if(portal_cell(s, x - 1, y, z) == BLOCK_PORTAL
	   || portal_cell(s, x + 1, y, z) == BLOCK_PORTAL) {
		var6 = 1;
		var7 = 0;
	}

	// Descend to this column's lowest portal block.
	w_coord_t var8 = y;
	while(portal_cell(s, x, var8 - 1, z) == BLOCK_PORTAL)
		var8--;

	if(portal_cell(s, x, var8 - 1, z) != BLOCK_OBSIDIAN) {
		// Floor under the column must be obsidian.
		portal_break_to_air(s, x, y, z);
	} else {
		// Column must be EXACTLY 3 portal tall, capped by obsidian.
		int var9;
		for(var9 = 1;
			var9 < 4 && portal_cell(s, x, var8 + var9, z) == BLOCK_PORTAL;
			var9++)
			;
		if(var9 == 3
		   && portal_cell(s, x, var8 + var9, z) == BLOCK_OBSIDIAN) {
			bool var10 = portal_cell(s, x - 1, y, z) == BLOCK_PORTAL
				|| portal_cell(s, x + 1, y, z) == BLOCK_PORTAL;
			bool var11 = portal_cell(s, x, y, z - 1) == BLOCK_PORTAL
				|| portal_cell(s, x, y, z + 1) == BLOCK_PORTAL;
			if(var10 && var11) {
				// Portal neighbours on BOTH horizontal axes: invalid.
				portal_break_to_air(s, x, y, z);
			} else if((portal_cell(s, x + var6, y, z + var7) != BLOCK_OBSIDIAN
					   || portal_cell(s, x - var6, y, z - var7)
						   != BLOCK_PORTAL)
					  && (portal_cell(s, x - var6, y, z - var7)
							  != BLOCK_OBSIDIAN
						  || portal_cell(s, x + var6, y, z + var7)
							  != BLOCK_PORTAL)) {
				// Along the axis: one horizontal neighbour must be obsidian
				// and the opposite one portal (either handedness) -- the
				// decompiled negation, transcribed verbatim.
				portal_break_to_air(s, x, y, z);
			}
		} else {
			portal_break_to_air(s, x, y, z);
		}
	}
}

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockPortal.java:10-24: null collision, selection is a 4/16
	// plane. Either X portal neighbour selects the X plane; otherwise Z.
	if(entity)
		return 0;
	if(x) {
		bool along_x = false;
		if(this->neighbours) {
			along_x = this->neighbours[SIDE_LEFT].type == BLOCK_PORTAL
				|| this->neighbours[SIDE_RIGHT].type == BLOCK_PORTAL;
		} else if(this->get_block) {
			struct block_data b;
			along_x = (this->get_block(this->get_block_ctx,
				this->x - 1, this->y, this->z, &b) && b.type == BLOCK_PORTAL)
				|| (this->get_block(this->get_block_ctx,
				this->x + 1, this->y, this->z, &b) && b.type == BLOCK_PORTAL);
		}
		// Callers must supply a neighbour provider to distinguish an X pane
		// from Beta's default Z pane; no global world is safe for split/MP.
		*x = along_x ? (struct AABB) {0, 0, 0.375F, 1, 1, 0.625F}
			: (struct AABB) {0.375F, 0, 0, 0.625F, 1, 1};
	}
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return TEXTURE_INDEX(0, 0);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	return 0;
}

struct block block_portal = {
	.name = "Portal",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = NULL,
	.onNeighbourBlockChange = onNeighbourChange, // Beta self-integrity break
	.onRightClick = NULL,
	.transparent = true,
	.renderBlock = render_block_portal,
	.renderBlockAlways = NULL,
	.luminance = 11,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 1,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = -1,
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
