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
#include "../network/server_world.h"
#include "../network/server_weather.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

// Beta BlockFarmland.isWaterNearby: any water in the 9x9x2 box (x-4..x+4, y..y+1, z-4..z+4).
static bool farmland_water_nearby(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	for(int dx = -4; dx <= 4; dx++)
		for(int dy = 0; dy <= 1; dy++)
			for(int dz = -4; dz <= 4; dz++)
				if(server_world_get_block(block_world(s), x + dx, y + dy, z + dz, &b)
				   && (b.type == BLOCK_WATER_FLOW || b.type == BLOCK_WATER_STILL))
					return true;
	return false;
}

// Beta BlockFarmland.onEntityWalking: a 1-in-4 roll per footstep reverts the farmland to plain dirt
// (the "trampling" mechanic). UNCONDITIONAL -- unlike updateTick's dry-out below it does not spare a
// crop directly above; the crop is dropped by the neighbour-notify (block_crops onNeighbourBlockChange
// -> Beta canBlockStay), exactly as Beta's setBlockWithNotify pops it. The entity-movement half (Beta
// Entity.moveEntity's distanceWalked/nextStepDistance footstep accumulator) lives in server_local.c's
// SRPC_PLAYER_POS handler (player) and the mob tick (entity_monster.c), which reach this through
// onEntityWalking below.
// (rand_gen_int is not bit-exact to Beta's world.rand.nextInt(4), so the 1/4 is faithful in
// DISTRIBUTION, not exact RNG sequence -- the engine-wide caveat.)
void block_farmland_on_entity_walking(struct server_local* s, int x, int y, int z) {
	if(rand_gen_int(&s->rand_src, 4) == 0)
		server_world_set_block(block_world(s), x, y, z,
							   (struct block_data) {.type = BLOCK_DIRT, .metadata = 0});
}

// R1: the struct-block onEntityWalking hook, dispatched by server_local_entity_walked_on_block on
// every footstep of the player AND of every canTriggerWalking mob. Beta applies NO walker
// condition here (the walker argument is unused, as in Beta); sneaking, riding and the
// non-walking entities are all filtered earlier by Entity.moveEntity's gate at the call sites.
// W2-R1c closed the former player-only deviation (it was an engine limitation: there was no
// actor-generic walk hook). [740c583: BlockFarmland.java:46-51, Entity.java:437-458]
static void onEntityWalking(struct server_local* s, struct block_info* this,
							enum block_walker walker) {
	(void)walker;
	block_farmland_on_entity_walking(s, this->x, this->y, this->z);
}

// Beta BlockFarmland.updateTick: on 1/5 of random ticks, hydrate to moisture 7 when water is nearby
// OR the cell directly above is being rained on [740c583: canBlockBeRainedOn(x, y+1, z) -- weather
// slice fix, previously omitted when there was no weather sim to check]; otherwise dry out one
// moisture level, and at level 0 with no crop directly above, revert to dirt.
static void onRandomTick(struct server_local* s, struct block_info* this) {
	if(rand_gen_int(&s->rand_src, 5) != 0)
		return;
	int x = this->x, y = this->y, z = this->z;
	if(farmland_water_nearby(s, x, y, z)
	   || world_can_block_be_rained_on(s, x, y + 1, z)) {
		if((this->block->metadata & 0x0F) != 7) {
			this->block->metadata = 7;
			server_world_set_block(block_world(s), x, y, z, *this->block);
		}
		return;
	}
	int m = this->block->metadata & 0x0F;
	if(m > 0) {
		this->block->metadata = (uint8_t)(m - 1);
		server_world_set_block(block_world(s), x, y, z, *this->block);
	} else {
		struct block_data above;
		bool crop_above = server_world_get_block(block_world(s), x, y + 1, z, &above)
			&& above.type == BLOCK_CROPS;
		if(!crop_above)
			server_world_set_block(block_world(s), x, y, z,
								   (struct block_data) {.type = BLOCK_DIRT, .metadata = 0});
	}
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockFarmland.java:10,14-15: selection 15/16, FULL-cube collision.
	if(x)
		aabb_setsize(x, 1.0F, entity ? 1.0F : 0.9375F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	switch(side) {
		case SIDE_TOP: return face_occlusion_empty();
		case SIDE_BOTTOM: return face_occlusion_full();
		default: return face_occlusion_rect(15);
	}
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(side) {
		case SIDE_TOP:
			// Beta: the top face is WET when moisture > 0, else DRY.
			return ((this->block->metadata & 0x0F) > 0) ?
				tex_atlas_lookup(TEXAT_FARMLAND_WET) :
				tex_atlas_lookup(TEXAT_FARMLAND_DRY);
		default: return tex_atlas_lookup(TEXAT_DIRT);
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

static void onNeighbourBlockChange(struct server_local* s, struct block_info* this) {
	// Beta BlockFarmland.onNeighborBlockChange: if the MATERIAL directly above isSolid(), revert
	// the farmland to plain dirt. The set-to-dirt notify also pops any crop above (block_crops
	// canBlockStay). W2-B2: this is Material.isSolid (block_beta_material_is_solid -- true for
	// glass, leaves, TNT, slabs, chests, fences, cake, cobweb...; false only for air/fire/
	// liquids/plants/circuits/snow layer/portal), not the old normal-cube stand-in, which missed
	// every solid non-cube. An unloaded cell reads as air (World.getBlockMaterial -> air), so it
	// never reverts. [740c583: BlockFarmland.java:81-88, Material.java:47, MaterialLogic.java:8]
	struct block_data above;
	if(server_world_get_block(block_world(s), this->x, this->y + 1, this->z, &above)
	   && block_beta_material_is_solid(above.type)) {
		server_world_set_block(block_world(s), this->x, this->y, this->z,
							   (struct block_data) {.type = BLOCK_DIRT, .metadata = 0});
	}
}

struct block block_farmland = {
	.name = "Farmland",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = onRandomTick, // Beta updateTick: hydrate near water / dry out to dirt
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: revert to dirt when covered by a solid
	.onRightClick = NULL,
	.onEntityWalking = onEntityWalking,
	.transparent = false,
	.renderBlock = render_block_farmland,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 15,
	.ignore_lighting = true,
	.flammable = false,
	.place_ignore = false,
	/* Digging [decompile 740c583]: Block.java:652 tilledField
	 * setHardness(0.6F) -> 900ms. Block.tilledField IS in
	 * ItemSpade.blocksEffectiveAgainst (alongside grass/dirt/sand/gravel/
	 * snow/clay), so a shovel gets its tier's efficiencyOnProperMaterial --
	 * same SHOVEL/ANY/MAX registration as block_dirt.c. `.min = ANY` keeps
	 * bare hands (and every other tool) on the unpenalized flat hardness/30
	 * branch: Material.ground is never setRequiresTool'd, so
	 * canHarvestBlock is unconditionally true. (Was TOOL_TYPE_ANY across
	 * the board: the only shovel-favoured Beta block missing its shovel
	 * class -- parity_audit_2026-08-25 lower-tier finding.) */
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
