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

// Audit #24 (ratified) fix. Beta 1.7.3 BlockPressurePlate (decompile 740c583),
// verified verbatim this session:
//
// tickRate returns 20 ticks.
// onNeighborBlockChange checks for a normal cube below the plate.
// If support is missing, drop the item and replace the plate with air, notifying.
// A supported plate needs no further action in that callback.
// updateTick runs the sensing helper on the authoritative world only,
// and only when the metadata is nonzero.
// The helper is setStateIfMobInteractsWithPlate.
//
// onEntityCollidedWithBlock also calls that helper on the authoritative world,
// but only when the metadata is not already the pressed value of 1.
// This supplies arrival detection without unconditional polling.
//
// setStateIfMobInteractsWithPlate remembers whether the metadata was 1.
// The horizontal inset of the sensing box is 0.125.
// The everything mode queries all entities in that box, excluding none.
// The mobs mode instead queries EntityLiving.
// The box starts at cell-floor height and is 0.25 tall, with the inset on X/Z.
// Any returned entity means present.
// Arrival writes pressed metadata, notifies this cell then the cell below,
// and plays random.click at the cell center, 0.1 above the floor, volume 0.3, pitch 0.6.
// Departure writes unpressed metadata and makes the same two notifications,
// then plays the same sound at volume 0.3, pitch 0.5.
// The two transition tests retain their original order.
// Only presence schedules another update after the tickRate delay.
// Absence ends the recheck chain.
//
// Registration (Block.java:662/664): pressurePlateStone = EnumMobType.mobs
// (EntityLiving.class ONLY -- players + every mob, NOT items/arrows/minecarts);
// pressurePlatePlanks = EnumMobType.everything (getEntitiesWithinAABBExcludingEntity
// with a null exclusion -- literally every entity type present in the box, no
// filter at all).
//
// THREE divergences fixed here:
//   (a) onWorldTick sensed ONLY the local player via an eye-height heuristic.
//       Replaced with the real setStateIfMobInteractsWithPlate entity-AABB sweep
//       (plate_someone_present), using the SAME sensitivity box Beta computes,
//       gated by triggerMobType exactly as registered above.
//   (b) neither struct set .tick_delay, so the engine re-armed the poll every
// tick instead of Beta's 20-tick interval. Both structs now set
// the tick delay to 20.
// (c) the re-arm was unconditional. plate_update_state now reschedules ONLY
// while an entity is present, matching Beta's final presence gate (the
//       block_rail.c detector-rail precedent has the identical shape).
//
// Real Beta arrival detection is via onEntityCollidedWithBlock, called from
// Entity.moveEntity every tick an entity's post-move AABB overlaps the plate's
// CELL (a broader, block-granularity scan than the narrow sensitivity box the
// sense query itself uses) -- see block_pressure_plate.h for the full citation
// of where this engine wires that per entity kind (player: server_world.c;
// mobs: entity_monster.c; items: entity_item.c) and why arrows are correctly
// NOT wired (EntityArrow.onUpdate never calls Entity.moveEntity in Beta 1.7.3).
// Placement (onBlockAdded) and a still-supported onNeighborBlockChange are both
// genuine Beta no-ops -- neither schedules anything -- so the old "arm the poll
// at placement" / "re-arm on every supported neighbour change" GAP #1
// workarounds are removed: the real collision hook now supplies the initial
// press detection Beta gets for free from onEntityCollidedWithBlock, and the
// self-sustaining while-pressed recheck chain is armed by plate_update_state's
// own presence-gated scheduling exactly like Beta.

#include "../entity/combat.h" // PLAYER_EYE_HEIGHT / PLAYER_BODY_HEIGHT (player sensitivity-box probe)
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/audio.h" // SOUND_RANDOM_CLICK
#include "block_pressure_plate.h" // block_pressure_plate_on_entity_tick (audit #24 public entry point)
#include "blocks.h"

static enum block_material getMaterial1(struct block_info* this) {
	return MATERIAL_STONE;
}

static enum block_material getMaterial2(struct block_info* this) {
	return MATERIAL_WOOD;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockPressurePlate.setBlockBoundsBasedOnState (java:122-128): meta == 1.
	if(x)
		aabb_setsize(x, 0.875F,
					 this->block->metadata == 1 ? 0.03125F : 0.0625F, 0.875F);
	return entity ? 0 : 1;
}

struct face_occlusion side_bottom = {
	.mask = {0x00007FFE, 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE7FFE,
			 0x7FFE7FFE, 0x7FFE7FFE, 0x7FFE0000},
};

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return (side == SIDE_BOTTOM) ? &side_bottom : face_occlusion_empty();
}

static uint8_t getTextureIndex1(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_STONE);
}

static uint8_t getTextureIndex2(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_PLANKS);
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	struct block_data blk;
	if(!server_world_get_block(block_world(s), where->x, where->y - 1, where->z,
							   &blk))
		return false;

	// Beta canPlaceBlockAt: isBlockNormalCube -- the SAME test the support-drop uses, so
	// a placeable plate can never immediately pop (the old can_see_through proxy accepted
	// blocks like TNT that the drop test rejects).
	if(!block_is_normal_cube(blk.type))
		return false;

	if(!block_place_default(s, actor, it, where, on, on_side))
		return false;

	// Beta onBlockAdded is a no-op for BlockPressurePlate -- placement never arms
	// anything. The old "GAP #1: arm the press-poll here" workaround is gone now
	// that block_pressure_plate_on_entity_tick gives this engine a real,
	// immediate arrival trigger (wired from the player/mob/item per-tick move
	// code); the very next tick an entity's box resolves onto this cell is
	// exactly when Beta would first call onEntityCollidedWithBlock too.
	// [740c583: BlockPressurePlate.onBlockAdded]
	return true;
}

static void onBlockRemoval(struct server_local* s, struct block_info* info) {
	if((info->block->metadata & 0x01) == 0)
		return;
	notifyNeighbours_from(
		s, info->x, info->y, info->z, info->block->type);
	notifyNeighbours_from(
		s, info->x, info->y - 1, info->z, info->block->type);
}


// Beta BlockPressurePlate.onNeighborBlockChange (740c583): the plate drops when the block
// below is no longer a normal cube (World.isBlockNormalCube -- same test as canPlaceBlockAt).
// Without this a plate floats when its support is mined, and a PRESSED floating plate keeps
// powering forever (the phantom-power symptom). Mirrors block_lever.c's support-drop: drop the
// item, clear to air, notify so adjacent wire/components re-evaluate the lost power source.
// A still-supported plate is a pure Beta no-op -- unlike the pre-fix code, this does NOT
// re-arm any poll (see the file header: the real entity-collision hook owns arrival now).
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	struct block_data below;
	if(server_world_get_block(block_world(s), info->x, info->y - 1, info->z, &below)
	   && block_is_normal_cube(below.type))
		return; // still supported -- Beta does nothing further here

	server_local_spawn_block_drops(s, info);
	server_world_set_block(block_world(s), info->x, info->y, info->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

// Beta setStateIfMobInteractsWithPlate uses a 0.125 horizontal inset: a
// column narrower than the full cell in X/Z, 0.25 tall starting at the
// plate's own Y. Distinct from getBoundingBox's SELECTION box above (out of
// scope for audit #24 -- unchanged).
// [740c583: BlockPressurePlate.setStateIfMobInteractsWithPlate]
static void plate_sense_box(w_coord_t x, w_coord_t y, w_coord_t z,
							struct AABB* out) {
	aabb_setsize(out, 0.75F, 0.25F, 0.75F);
	aabb_translate(out, (float)x + 0.125F, (float)y, (float)z + 0.125F);
}

// A local player's world AABB. Local players are not in s->entities (the same
// precedent as server_local.c's mob_spawn_entity_clear and
// redstone_piston_server.c's pmove_player_box): 0.6 x PLAYER_BODY_HEIGHT x
// 0.6, feet at player.y - PLAYER_EYE_HEIGHT (player.y is the EYE position).
static void plate_player_box(const struct server_player* player,
							 struct AABB* out) {
	assert(player && out);
	aabb_setsize_centered(out, 0.6F, PLAYER_BODY_HEIGHT, 0.6F);
	aabb_translate(out, 0.0F, PLAYER_BODY_HEIGHT * 0.5F - PLAYER_EYE_HEIGHT,
				   0.0F);
	aabb_translate(out, (float)player->x, (float)player->y,
				   (float)player->z);
}

// Beta setStateIfMobInteractsWithPlate's entity query. `living_only` selects
// between the two EnumMobType values Beta actually registers (Block.java:
// 662/664): true (stone, EnumMobType.mobs) = EntityLiving.class ONLY -- the
// player plus every mob (this engine's ENTITY_MONSTER already covers hostile
// AND passive mobs, i.e. Beta's EntityLiving minus the player); false (wood,
// EnumMobType.everything) queries all entities with none excluded --
// no type filter AT ALL, so every live entity in the box counts, dropped
// items included. AxisAlignedBB.intersectsWith is the STRICT test
// (aabb_intersection_strict is its verbatim transcription, cited at
// tests/test_detector_rail_golden.c).
static bool plate_someone_present(struct server_local* s, w_coord_t x,
								  w_coord_t y, w_coord_t z, bool living_only) {
	/* Beta BlockPressurePlate.setStateIfMobInteractsWithPlate queries the
	 * plate's World (RetroMCP b1.7.3 BlockPressurePlate.java:69-83). */
	struct server_world* w = block_world(s);
	struct AABB box;
	plate_sense_box(x, y, z, &box);

	struct server_player* players[SERVER_LOCAL_MAX_PRESENT_ACTORS];
	size_t player_count = server_local_present_actors(
		s, players, SERVER_LOCAL_MAX_PRESENT_ACTORS);
	for(size_t i = 0; i < player_count; i++) {
		if(server_local_world_of(s, players[i]) != w)
			continue;
		struct AABB pb;
		plate_player_box(players[i], &pb);
		if(aabb_intersection_strict(&box, &pb))
			return true;
	}

	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(!e || e->delay_destroy == 0)
			continue; // same live-entity filter as every other AABB scan in this engine
		if(server_local_entity_world(s, e) != w)
			continue;
		if(living_only && e->type != ENTITY_MONSTER)
			continue;
		struct AABB eb;
		entity_make_world_bbox(e, &eb);
		if(aabb_intersection_strict(&box, &eb))
			return true;
	}
	return false;
}

// Beta setStateIfMobInteractsWithPlate, the ONE private method both public
// hooks (onEntityCollidedWithBlock, updateTick) call -- transcribed verbatim.
// Mirrors block_rail.c's detector_rail_update_state (the SAME 20-tick interval,
// reschedule-only-while-present shape).
// [740c583: BlockPressurePlate.setStateIfMobInteractsWithPlate]
static void plate_update_state(struct server_local* s, w_coord_t x, w_coord_t y,
							   w_coord_t z) {
	struct block_data cur;
	if(!server_world_get_block(block_world(s), x, y, z, &cur)
	   || (cur.type != BLOCK_STONE_PRESSURE_PLATE
		   && cur.type != BLOCK_WOOD_PRESSURE_PLATE))
		return;

	bool was_pressed = cur.metadata == 1;
	bool living_only = cur.type == BLOCK_STONE_PRESSURE_PLATE;
	bool present = plate_someone_present(s, x, y, z, living_only);

	if(present && !was_pressed) {
		cur.metadata = 1;
		server_world_set_block_silent(block_world(s), x, y, z, cur);
		// Beta notifies BOTH the plate's own cell and the support cell below,
		// using this.blockID as the source -- a door/wire attached to the SUPPORT cube
		// (not the plate itself) must re-evaluate the strong power driven
		// through it.
		notifyNeighbours_from(s, x, y, z, cur.type);
		notifyNeighbours_from(s, x, y - 1, z, cur.type);
		server_local_emit_sound(SOUND_RANDOM_CLICK,
			(vec3) {(float)x + 0.5F, (float)y + 0.1F, (float)z + 0.5F}, 0.3F,
			0.6F);
	}

	if(!present && was_pressed) {
		cur.metadata = 0;
		server_world_set_block_silent(block_world(s), x, y, z, cur);
		notifyNeighbours_from(s, x, y, z, cur.type);
		notifyNeighbours_from(s, x, y - 1, z, cur.type);
		server_local_emit_sound(SOUND_RANDOM_CLICK,
			(vec3) {(float)x + 0.5F, (float)y + 0.1F, (float)z + 0.5F}, 0.3F,
			0.5F);
	}

	// Beta's final presence gate schedules another update -- reschedule ONLY
	// while something is present. Audit #24 fix (c): the pre-fix code
	// rescheduled unconditionally every tick.
	if(present)
		server_world_schedule_block_tick(block_world(s), x, y, z, cur.type);
}

// Public entry point (block_pressure_plate.h): Beta's onEntityCollidedWithBlock
// outer gate, re-verifying the cell itself rather than trusting the caller
// (the block_detector_rail_on_minecart_tick precedent).
// [740c583: BlockPressurePlate.onEntityCollidedWithBlock]
void block_pressure_plate_on_entity_tick(struct server_local* s, w_coord_t x,
										 w_coord_t y, w_coord_t z) {
	struct block_data cur;
	if(!server_world_get_block(block_world(s), x, y, z, &cur)
	   || (cur.type != BLOCK_STONE_PRESSURE_PLATE
		   && cur.type != BLOCK_WOOD_PRESSURE_PLATE)
	   || cur.metadata == 1)
		return; // Beta calls the sensing helper on collision only when metadata is not 1.

	plate_update_state(s, x, y, z);
}

// The scheduled 20-tick recheck (Beta updateTick). Audit #24 fix (a)+(b):
// info->neighbours is populated (server_world_block_needs_neighbours' allowlist)
// but UNUSED -- same precedent as block_rail.c's detector_rail_world_tick.
// [740c583: BlockPressurePlate.updateTick]
static void onWorldTick(struct server_local* s, struct block_info* info) {
	if(info->block->metadata != 0)
		plate_update_state(s, info->x, info->y, info->z);
}

struct block block_stone_pressure_plate = {
	.name = "Pressure Plate",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial1,
	.getTextureIndex = getTextureIndex1,
	.getDroppedItem = block_drop_default,
	.onBlockRemoval = onBlockRemoval,
	.onRandomTick = NULL,
	.onWorldTick = onWorldTick,
	.tick_delay = 20, // Beta BlockPressurePlate.tickRate -- audit #24 fix (b)
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: drop when support removed
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_pressure_plate,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta hardness x1500: Block.java:662 pressurePlateStone
	// Hardness 0.5 becomes 750 (already correct). The best tier is ANY
	// (not WOOD): ItemPickaxe.blocksEffectiveAgainst omits
	// Block.pressurePlateStone entirely, so getStrVsBlock is flat 1.0F for
	// any pickaxe tier -- no per-tier bonus (same mechanism as
	// block_ore.c's redstone-ore comment). `.min`/`.tool` (harvest-tier
	// gate) are out of scope for this fix and left unchanged.
	.digging.hardness = 750,
	.digging.tool = TOOL_TYPE_PICKAXE,
	.digging.min = TOOL_TIER_WOOD,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_wooden_pressure_plate = {
	.name = "Pressure Plate",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial2,
	.getTextureIndex = getTextureIndex2,
	.getDroppedItem = block_drop_default,
	.onBlockRemoval = onBlockRemoval,
	.onRandomTick = NULL,
	.onWorldTick = onWorldTick,
	.tick_delay = 20, // Beta BlockPressurePlate.tickRate -- audit #24 fix (b)
	.onNeighbourBlockChange = onNeighbourBlockChange, // Beta: drop when support removed
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_pressure_plate,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// Beta hardness x1500: Block.java:664 pressurePlatePlanks
	// Hardness 0.5 becomes 750 (already correct). Registered with
	// Material.wood, NOT Material.rock like the stone plate above --
	// Material.java's static init never calls setNoHarvest on
	// Material.wood (only rock/iron/web/snow/builtSnow get it), so
	// InventoryPlayer.canHarvestBlock (InventoryPlayer.java:271-278) returns
	// true immediately from blockMaterial.getIsHarvestable, before any
	// tool check -- harvestable (and drops) with ANY tool or bare hands, no
	// pickaxe requirement, and no per-tier speed bonus either (not in any
	// tool's blocksEffectiveAgainst array). Selecting ANY for the tool,
	// minimum tier, and best tier is this codebase's
	// existing idiom for exactly that "flat, tool-free" case (see
	// block_wool.c, block_leaves.c) -- flat hardness/30 every time, matching
	// Block.blockStrength's fast branch (Block.java:327-333) unconditionally.
	.digging.hardness = 750,
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
