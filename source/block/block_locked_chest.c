/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	Beta 1.7.3 BlockLockedChest (block id 95), FH-07
	(docs/fork_hack_audit_2026-08-21.md). This id used to be squatted by the
	fork's iron chest (now BLOCK_IRON_CHEST/200, see block_iron_chest.c) --
	this file is the real Beta block that belongs at 95.

	[740c583 Block.java:687]
	  Registers block 95 with hardness 0.0F, light value 1.0F, wood footsteps,
	  internal name "lockedchest" and tick-on-load enabled; metadata changes
	  do not trigger neighbor notification.

	[740c583 BlockLockedChest.java], transcribed whole, no omissions:
	  - ctor: Material.wood, blockIndexInTexture = 26
	  - getBlockTexture/getBlockTextureFromSide: top/bottom -> index-1 (25); the
	    horizontal "front" face (chosen by the neighbour-opacity heuristic below)
	    -> index+1 (27); every other horizontal face -> index (26)
	  - canPlaceBlockAt: always true (no restriction beyond the engine's shared
	    entity-obstruction gate, which is exactly what block_place_default already
	    provides for every other unrestricted block, e.g. block_bedrock.c)
	  - updateTick: unconditionally sets the block to air -- it self-despawns on
	    its next random tick
	  - NO onBlockActivated override: right-click does nothing (not openable);
	    it is NEVER a container -- no window_registry.h row, no tile entity
	  - NO idDropped/quantityDropped override: Block's default applies (drops its
	    own id, quantity 1) -- block_drop_default below

	Beta's soundWoodFootstep/Material.wood have no dedicated fields in this
	port's struct block (no step-sound field exists at all -- see block_chest.c,
	also Material.wood, for the precedent); Material.wood is captured by
	getMaterial() alone.

	setTickOnLoad(true): in Beta this is a SEPARATE per-block flag consumed by
	World's own ambient/weather tick loop (the same 80-samples-per-loaded-chunk
	pass that also seeds cave sounds and thunder strikes), distinct from the
	ordinary per-block random-tick mechanism blocks like grass/leaves use
	elsewhere in Chunk.java. lockedChest is the ONLY Beta 1.7.3 block that sets
	it. This port has no separate tick-on-load array/loop -- only the single
	shared onRandomTick dispatch (server_world_random_tick,
	FCAVEX_RANDOM_TICKS_PER_CHUNK samples/chunk/tick). Per the FH-07 port note,
	updateTick is wired to THAT shared random-tick path instead: the practical
	effect (an unsupported/leftover locked chest eventually vanishes) is
	preserved, but the exact sampling distribution is this port's general
	random-tick pool, not Beta's dedicated ambient-loop array. Documented
	deviation, timing/feel layer only -- the eventual outcome (converts to air)
	is unconditional and byte-exact either way.
*/

#include "blocks.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // A1a: block_world(s) needs the full server state (as block_chest.c)
#include "../network/server_world.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_WOOD;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// [740c583] BlockLockedChest overrides neither getCollisionBoundingBoxFromPool
	// nor setBlockBoundsBasedOnState -- Block's own default full-cube bounds apply
	// to both the collision and selection/render box.
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	if(side == SIDE_TOP || side == SIDE_BOTTOM)
		return tex_atlas_lookup(TEXAT_CHEST_TOP); // blockIndexInTexture - 1 (25)

	// [740c583 BlockLockedChest.getBlockTexture] auto-facing from neighbour
	// opacity (Block.opaqueCubeLookup here mapped to this port's own analog,
	// block_is_normal_cube -- the established single source of truth for that
	// exact Beta concept, see its "Beta opaqueCubeLookup" citations in
	// server_world.c/worldgen_hell.c). This port's SIDE_FRONT/BACK/LEFT/RIGHT are
	// fixed world axes (blocks_side_offset: FRONT=-Z, BACK=+Z, LEFT=-X, RIGHT=+X),
	// exactly matching the four Beta probes at z-1/z+1 and x-1/x+1.
	// The four assignments below transcribe the facing selection,
	// including the redundant self-assignment in the first branch (kept for
	// byte-parity with the decompile's control flow, not simplified away).
	//
	// `this->neighbours` is NULL for item/inventory-icon rendering (see
	// block_chest.c's identical guard) -- use Beta's default front
	// facing +Z.
	enum side front = SIDE_BACK;
	if(this->neighbours) {
		bool opaque_front = block_is_normal_cube(this->neighbours[SIDE_FRONT].type); // z-1
		bool opaque_back = block_is_normal_cube(this->neighbours[SIDE_BACK].type);	  // z+1
		bool opaque_left = block_is_normal_cube(this->neighbours[SIDE_LEFT].type);   // x-1
		bool opaque_right = block_is_normal_cube(this->neighbours[SIDE_RIGHT].type); // x+1

		if(opaque_front && !opaque_back)
			front = SIDE_BACK; // face +Z -- Beta's redundant no-op branch
		if(opaque_back && !opaque_front)
			front = SIDE_FRONT; // face -Z
		if(opaque_left && !opaque_right)
			front = SIDE_RIGHT; // face +X
		if(opaque_right && !opaque_left)
			front = SIDE_LEFT; // face -X
	}

	return tex_atlas_lookup(side == front ? TEXAT_CHEST_FRONT_SINGLE // index + 1 (27)
										  : TEXAT_CHEST_SIDE);		  // index (26)
}

// [740c583 BlockLockedChest.updateTick] Replace the chest with air and notify
// neighbors unconditionally on every random tick -- see the file header for
// how this port reaches updateTick without a dedicated tick-on-load array.
static void onRandomTick(struct server_local* s, struct block_info* this) {
	server_world_set_block(block_world(s), this->x, this->y, this->z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

struct block block_locked_chest = {
	.name = "Locked Chest", // Beta internal name "lockedchest"
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = block_drop_default, // [740c583] no idDropped/quantityDropped override
	.onRandomTick = onRandomTick,
	.onRightClick = NULL, // [740c583] no onBlockActivated override -- never a container
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 15, // setLightValue(1.0F) -> 15 (matches BLOCK_GLOWSTONE's own encoding)
	.double_sided = false,
	.can_see_through = false, // [740c583] no isOpaqueCube override -> a normal opaque cube
	.ignore_lighting = false,
	// Absent from BlockFire's setBurnRate table [740c583 BlockFire] -- matches
	// BLOCK_CHEST (also Material.wood, also not in that table): not flammable.
	.flammable = false,
	.place_ignore = false,
	// Beta setHardness(0.0F). This port's OWN convention for a Beta hardness-0
	// block is inconsistent (block_repeater.c stores the literal 0; the majority
	// -- torch/tallgrass/sapling/flower/mushroom/tnt/fire/crops/reed -- store 50,
	// a nominal one-tick delay). Following the majority precedent here; see
	// REPORT.md for the open judgement call.
	.digging.hardness = 50,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		// [740c583] canPlaceBlockAt always true == no restriction beyond the
		// engine's shared entity-obstruction gate -- block_place_default IS that
		// baseline placer (see block_bedrock.c for the identical precedent: a
		// block with no special placement rule just uses it).
		.onItemPlace = block_place_default,
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
