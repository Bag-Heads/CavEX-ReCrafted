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
#include "../network/server_interface.h" // MAX_PLAYER_HEALTH
#include "../network/server_local.h" // s->player.health, server_local_set_player_health
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_WOOL;
}

// Beta BlockCake.eatCakeSlice: eating a cake slice (right-click) heals the player and advances the bite
// metadata 0..5; the 6th bite removes the cake. Only when the player is NOT at full health, and the held
// item is never consumed. Beta heals 3 (out of 20 max); CaveWii's health scale is 8x (MAX_PLAYER_HEALTH
// = 160), so a faithful slice heals 3*8 = 24. `on` is the clicked cake block.
#define CAKE_BITES_TOTAL 6
#define CAKE_HEAL_PER_BITE 24 // Beta heal(3) at CaveWii's 160/20 = 8x health scale
static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it, struct block_info* where,
						 struct block_info* on, enum side on_side) {
	(void)it;
	(void)where;
	(void)on_side;
	/* S1-a3 OPENED (Astra call #8 Q2): the actor-aware health transaction now
	 * exists, so the ACTING actor takes the slice. Beta BlockCake.eatCakeSlice
	 * is `if (player.health < 20) { player.heal(3); ... }` [740c583] -- health
	 * only, no hunger -- which is exactly the gate + heal below, per actor.
	 * The bite metadata and the cake removal are shared-world state and stay
	 * untagged broadcasts; only the heal and its health reply are
	 * actor-private. Cake PLACEMENT was already per-actor and is unchanged. */
	if(actor->health >= MAX_PLAYER_HEALTH)
		return; // Beta `if (player.health < 20)` gate -- no eating at full health
	struct block_data cake;
	if(!server_world_get_block(block_world(s), on->x, on->y, on->z, &cake) || cake.type != BLOCK_CAKE)
		return;
	(void)server_local_heal_actor(s, actor, actor->health + CAKE_HEAL_PER_BITE); // clamps to MAX
	int bites = (cake.metadata & 0x0F) + 1;
	if(bites >= CAKE_BITES_TOTAL) {
		server_world_set_block(block_world(s), on->x, on->y, on->z,
							   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
	} else {
		cake.metadata = (uint8_t)bites;
		server_world_set_block(block_world(s), on->x, on->y, on->z, cake);
	}
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x) {
		// Beta BlockCake.java:11-16,25-30: collision top is 0.5F - 1/16.
		aabb_setsize(x, 0.875F - 0.125F * this->block->metadata,
					 entity ? 0.4375F : 0.5F, 0.875F);
		aabb_translate(x, 0.0625F * this->block->metadata, 0, 0);
	}

	return 1;
}

// TODO: mask not correct for all states
struct face_occlusion bottom_mask = {
	.mask = {0x00007FFE, 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE7FFE,
			 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE0000},
};

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	switch(side) {
		case SIDE_BOTTOM: return &bottom_mask;
		default: return face_occlusion_empty();
	}
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(side) {
		case SIDE_TOP: return tex_atlas_lookup(TEXAT_CAKE_TOP);
		case SIDE_BOTTOM: return tex_atlas_lookup(TEXAT_CAKE_BOTTOM);
		case SIDE_LEFT:
			return (this->block->metadata > 0) ?
				tex_atlas_lookup(TEXAT_CAKE_SIDE_CUT) :
				tex_atlas_lookup(TEXAT_CAKE_SIDE);
		default: return tex_atlas_lookup(TEXAT_CAKE_SIDE);
	}
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	return 0;
}

// Beta BlockCake.canBlockStay [740c583]: `getBlockMaterial(x,y-1,z).isSolid()`.
// block_beta_material_is_solid (block_lava.c) is this project's existing 1:1
// port of Beta's per-id Material.isSolid() table -- the same helper
// block_sign.c's/block_ice.c's own support checks already use.
static bool cake_can_block_stay(struct server_local* s, struct block_info* info) {
	struct block_data below;
	return server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
								  &below)
		&& block_beta_material_is_solid(below.type);
}

// Beta BlockCake.onNeighborBlockChange [740c583]: if `!canBlockStay`,
// dropBlockAsItem(meta) then setBlockWithNotify(air). quantityDropped() == 0
// (BlockCake overrides it to return 0, above) means the drop loop never runs
// any iteration -- a cake with its support pulled pops to air and drops
// NOTHING, unlike almost every other support-loss pop in this project. The
// server_local_spawn_block_drops call below is still made (matching Beta's own
// unconditional dropBlockAsItem call) for parity/consistency with every other
// onNeighbourBlockChange in this codebase; it is a guaranteed no-op via
// getDroppedItem returning 0 above.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	if(cake_can_block_stay(s, info))
		return;
	server_local_spawn_block_drops(s, info); // no-op: quantityDropped == 0
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

// R1: Beta BlockCake.onBlockClicked calls eatCakeSlice directly -- a LEFT-click eats a slice
// exactly like a right-click (same health < 20 gate, heal 3, bite / removal). The held item is
// irrelevant. [740c583: BlockCake.java:62-64]
static void onBlockClicked(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this) {
	(void)held;
	onRightClick(s, actor, NULL, this, this, SIDE_TOP);
}

struct block block_cake = {
	.name = "Cake",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = NULL,
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: pop when canBlockStay fails
	.onRightClick = onRightClick, // Beta blockActivated -> eatCakeSlice
	.onBlockClicked = onBlockClicked,
	.transparent = false,
	.renderBlock = render_block_cake,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
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
