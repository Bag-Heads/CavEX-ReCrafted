/*
	Copyright (c) 2023 ByteBit/xtreme8000

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
#include "../network/client_interface.h"
#include "../network/inventory_logic.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_WOOD;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// [740c583 BlockSign.getCollisionBoundingBoxFromPool] both variants have
	// no entity collision, independently of their selection bounds.
	if(entity)
		return 0;

	if(x) {
		if(this->block->type == BLOCK_SIGN_POST) {
			// BlockSign ctor bounds. setBlockBoundsBasedOnState is a no-op for
			// the freestanding variant, including yaw metadata 4..7.
			aabb_setsize(x, 0.5F, 1.0F, 0.5F);
		} else {
			// Beta BlockSign.setBlockBoundsBasedOnState (java:28-50):
			// raw side 2..5, y 9/32..25/32 and depth 2/16; otherwise full.
			aabb_setsize(x, 1.0F, 1.0F, 1.0F);
			switch(this->block->metadata) {
				case 2:
					aabb_setsize(x, 1.0F, 0.5F, 0.125F);
					aabb_translate(x, 0, 0.28125F, 0.4375F);
					break;
				case 3:
					aabb_setsize(x, 1.0F, 0.5F, 0.125F);
					aabb_translate(x, 0, 0.28125F, -0.4375F);
					break;
				case 4:
					aabb_setsize(x, 0.125F, 0.5F, 1.0F);
					aabb_translate(x, 0.4375F, 0.28125F, 0);
					break;
				case 5:
					aabb_setsize(x, 0.125F, 0.5F, 1.0F);
					aabb_translate(x, -0.4375F, 0.28125F, 0);
					break;
			}
		}
	}

	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	(void)this;
	(void)side;
	(void)it;
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_PLANKS);
}

// [740c583 ItemSign.onItemUse] side-face metadata for signWall/BLOCK_SIGN is
// the clicked face value verbatim: FRONT/BACK/LEFT/RIGHT -> 2/3/4/5.
// SIDE_TOP/SIDE_BOTTOM fall to 0, but ItemSign refuses those wall-placement
// cases before reaching this helper.
uint8_t block_sign_wall_metadata_for_side(enum side on_side) {
	switch(on_side) {
		case SIDE_LEFT: return 4;
		case SIDE_RIGHT: return 5;
		case SIDE_BACK: return 3;
		case SIDE_FRONT: return 2;
		default: return 0;
	}
}

// Shared placement primitive for Beta signPost/63 and signWall/68. Both need
// the same obstruction check and sign_pos/sign_texts registration; only the
// block id and metadata interpretation differ.
bool block_sign_place(struct server_local* s, struct server_player* actor,
					  struct block_info* where,
					  uint8_t type, uint8_t metadata) {
	if(!block_is_any_sign(type))
		return false;

	struct block_data blk = (struct block_data) {
		.type = type,
		.metadata = metadata,
		.sky_light = 0,
		.torch_light = 0,
	};

	struct block_info blk_info = *where;
	blk_info.block = &blk;

	if(block_place_obstructed_for(s, actor, &blk_info)) // player + all entities (Beta gate;
		return false;                        // the sign's no-collision box never refuses)

	/* A1b: take the first free row of the shared table for the placing
	 * world (D4 bridge: slot 0), stamped with that world's owner -- the same
	 * first-free scan as before. */
	struct te_store st = server_world_tile_store(block_world(s));
	for (int i = te_store_claim_free(&st, TE_KIND_SIGN, where->x, where->y,
									 where->z); i >= 0; i = -1) {
		{
			memset(&s->sign_texts[i], ' ', SIGN_SIZE * sizeof(char));
			server_world_set_block(block_world(s), where->x, where->y, where->z, blk);
			return true;
		}
	}

	puts("Too many signs");
	return false;
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	(void)it; // block-as-item placer remains the existing wall-sign path.
	if(!blocks[on->block->type])
		return false;

	return block_sign_place(s, actor, where, BLOCK_SIGN,
							block_sign_wall_metadata_for_side(on_side));
}

static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	if(actor->active_inventory == &actor->inventory) {
		server_local_open_inventory_in(s, block_world(s), actor, &inventory_logic_sign,
										SIGN_SIZE, on->x, on->y, on->z,
										WINDOWC_SIGN, WINDOW_TYPE_SIGN);
	}
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	(void)this;
	(void)g;
	(void)s;
	if(it) {
		// [740c583 BlockSign.idDropped] both block ids drop Item.sign (323).
		it->id = ITEM_SIGN;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

// [740c583 BlockSign.onNeighborBlockChange] support is selected by the
// BlockSign instance (therefore block id), never by metadata. The post checks
// below; block 68 uses Beta's raw wall metadata 2..5.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	w_coord_t sx = info->x;
	w_coord_t sy = info->y;
	w_coord_t sz = info->z;
	if(info->block->type == BLOCK_SIGN_POST) {
		sy--;
	} else {
		switch((info->block->metadata - 2) & 0x03) {
			case 0: sz++; break; // Beta meta 2
			case 1: sz--; break; // Beta meta 3
			case 2: sx++; break; // Beta meta 4
			case 3: sx--; break; // Beta meta 5
		}
	}

	struct block_data support;
	if(server_world_get_block(block_world(s), sx, sy, sz, &support)
	   && block_beta_material_is_solid(support.type))
		return;

	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

static void onBreak(struct server_local* s, struct block_info* this) {
	struct server_world* w = block_world(s);
	struct te_store st = server_world_tile_store(w); /* A1b */
	for(int i = te_store_find(&st, TE_KIND_SIGN, this->x, this->y, this->z);
		i >= 0; i = -1)
		{
			te_store_free_row(&st, TE_KIND_SIGN, i);
			// Client-side sign-text store feed (source/game/sign_text_store.h):
			// the server has just dropped this sign's durable text; without
			// this the client's cached grid for (x,y,z) would survive its
			// sign, e.g. a later unrelated block placed at the same cell
			// could read stale sign text if that cell is ever queried again.
			struct client_rpc removed = {
				.type = CRPC_SIGN_TEXT_REMOVED,
				.payload.sign_text_removed
				= {.x = this->x, .y = this->y, .z = this->z},
			};
			clin_rpc_stamp(&removed, w->world_index, w->generation);
			clin_rpc_send(&removed);
			break;
		}
}

struct block block_sign = {
	.name = "Sign",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRightClick = onRightClick,
	.transparent = false,
	.renderBlock = render_block_sign,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 1,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// [740c583 Block.java:655,660] both sign variants: setHardness(1.0F).
	// Field scale = beta_hardness * 1500 (derivation in test_redstone_ore_dig.c).
	// TOOL_TYPE_ANY is deliberate: signs are absent from
	// ItemAxe.blocksEffectiveAgainst, so an axe digs no faster than a hand.
	.digging.hardness = 1500,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.fuel = 1,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
