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
#include "../network/client_interface.h"
#include "../network/inventory_logic.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"

static bool live_window_owns_chest(struct server_local* s, w_coord_t x,
								   w_coord_t y, w_coord_t z) {
	struct inventory* inv = s->player.active_inventory;
	/* A1b: a window bound to another world views a different chest. */
	return inv && inv != &s->player.inventory
		&& server_local_window_bound_to(inv, block_world(s))
		&& inv->logic == &inventory_logic_iron_chest && inv->x == x
		&& inv->y == y && inv->z == z;
}

static enum block_material getMaterial(struct block_info* this) {
	// Iron chest is a fork block (no Beta equivalent) crafted from 8 iron ingots and
	// is its own item (id 95 < 256), so getMaterial == MATERIAL_WOOD would make the
	// furnace fuel check (server_furnace.c, the ONLY getMaterial consumer) burn it for
	// 300 ticks like a plank -- absurd for an all-iron block, and inconsistent with its
	// iron siblings (block_iron id 42, iron_door id 71 = MATERIAL_STONE, both non-fuel).
	// STONE keeps it correctly NON-fuel.
	return MATERIAL_STONE;
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

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	uint8_t tex[SIDE_MAX] = {
		[SIDE_TOP] = tex_atlas_lookup(TEXAT_CAST_BLOCK_IRON),
		[SIDE_BOTTOM] = tex_atlas_lookup(TEXAT_CAST_BLOCK_IRON),
		[SIDE_BACK] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
		[SIDE_FRONT] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
		[SIDE_LEFT] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
		[SIDE_RIGHT] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
	};

	switch(this->block->metadata) {
		case 0: tex[SIDE_FRONT] = tex_atlas_lookup(TEXAT_CHEST_FRONT_SINGLE); break;
		case 1: tex[SIDE_RIGHT] = tex_atlas_lookup(TEXAT_CHEST_FRONT_SINGLE); break;
		case 2: tex[SIDE_BACK] = tex_atlas_lookup(TEXAT_CHEST_FRONT_SINGLE); break;
		case 3: tex[SIDE_LEFT] = tex_atlas_lookup(TEXAT_CHEST_FRONT_SINGLE); break;
	}

 	return tex[side];
}

static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	/* S1-a1b FAIL-CLOSED for a non-host actor: this container's live-window
	 * ownership test (live_window_owns_chest below / block_iron_chest.c:28)
	 * consults ONLY s->player.active_inventory -- there is no position-keyed
	 * leg and no scan of any other actor, unlike the chest's
	 * server_local_has_chest_view. Admitting a guest window here would let a
	 * guest break a container it has open, take the "window does not own the
	 * contents" branch, spawn the tile-entity snapshot as items AND keep the
	 * same stacks in its live window -- a duplication on close (R-S1a1
	 * finding 3, owner S1-a2). Opening stays host-only until that ownership
	 * test takes the acting actor; PLACEMENT (facing) is per-actor already. */
	if(actor != &s->player)
		return;
	if(actor->active_inventory == &actor->inventory) {
		server_local_open_inventory_in(
			s, block_world(s), actor, &inventory_logic_iron_chest, IRON_CHEST_SIZE, on->x,
			on->y, on->z, WINDOWC_IRON_CHEST, WINDOW_TYPE_IRON_CHEST);
	}
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk = (struct block_data) {
		.type = it->id,
		.metadata = it->durability,
		.sky_light = 0,
		.torch_light = 0,
	};

	struct block_info blk_info = *where;
	blk_info.block = &blk;

	if(block_place_obstructed_for(s, actor, &blk_info)) // player + all entities (Beta gate)
		return false;
	/* A1b: rows of the placing world only (D4 bridge: slot 0). */
	struct te_store st = server_world_tile_store(block_world(s));
	if(te_store_find(&st, TE_KIND_CHEST, where->x, where->y, where->z) >= 0)
		return false; // retained orphan row awaiting an OOM-safe spill

	for (int i = te_store_claim_free(&st, TE_KIND_CHEST, where->x, where->y,
									 where->z); i >= 0; i = -1) {
		{
			memset(&s->chest_items[i], 0, 54*sizeof(struct item_data));
			server_world_set_block(block_world(s), where->x, where->y, where->z, blk);
			return true;
		}
	}

	puts("Too many chests");
	return false;
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = this->block->type;
		it->durability = this->block->metadata;
		it->count = 1;
	}

	return 1;
}

static void onBreak(struct server_local* s, struct block_info* this) {
	bool window_owns_contents
		= live_window_owns_chest(s, this->x, this->y, this->z);
	struct te_store st = server_world_tile_store(block_world(s)); /* A1b */
	for(int i = te_store_find(&st, TE_KIND_CHEST, this->x, this->y, this->z);
		i >= 0; i = -1) {
		if(window_owns_contents) {
			(void)server_local_spill_active_container(
				s, this->x, this->y, this->z);
			te_store_free_row(&st, TE_KIND_CHEST, i);
			memset(s->chest_items[i], 0, sizeof(s->chest_items[i]));
		} else {
			if(!server_local_spawn_destroyed_container_items(
				   s, this->x, this->y, this->z, s->chest_items[i],
				   MAX_CHEST_SLOTS))
				return;
			te_store_free_row(&st, TE_KIND_CHEST, i);
			memset(s->chest_items[i], 0, sizeof(s->chest_items[i]));
		}
		break;
	}
}

struct block block_iron_chest = {
	.name = "Iron Chest",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 3750,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_WOOD,
	.digging.best = TOOL_TIER_MAX,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.fuel = 0,
		.render_data.block.has_default = true,
		.render_data.block.default_metadata = 0,
		.render_data.block.default_rotation = 2,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

