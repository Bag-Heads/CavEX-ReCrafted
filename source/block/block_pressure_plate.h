/*
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

#ifndef BLOCK_PRESSURE_PLATE_H
#define BLOCK_PRESSURE_PLATE_H

#include "../network/server_local.h" // struct server_local; w_coord_t (via world.h)

// Audit #24 (ratified). Beta BlockPressurePlate.onEntityCollidedWithBlock
// (740c583) is reached from Entity.moveEntity's per-tick "which blocks does my
// post-move AABB overlap" scan -- every entity whose box overlaps a cell
// holding a pressure plate dispatches here, every tick, regardless of WHICH
// entity triggered the call (setStateIfMobInteractsWithPlate, the function
// this eventually calls, runs its OWN independent getEntitiesWithinAABB query
// over the plate's narrower sensitivity box -- the caller only needs to
// resolve "which cell", never "which entity is on it").
//
// This engine has no generic entity/block collision dispatch table (the
// soulsand-slowdown precedent in entity.c and the detector-rail precedent in
// block_rail.h/.c are likewise bespoke per-effect calls from each entity
// kind's own tick, not a vtable slot), so this is exposed here for every
// caller that resolves "which cell is this entity's post-move box touching"
// on its own:
//   - server_world.c's server_world_tick: the PLAYER, via the same
//     center-column feet-cell approximation server_local.c already uses for
//     fire/cactus/ladder contact (s->player.x/y/z is not in s->entities).
//   - entity_monster.c's post-move block scan: mobs (ENTITY_MONSTER covers
//     every hostile AND passive mob -- Beta's EntityLiving minus the player).
//   - entity_item.c's post-move block scan: dropped items (ENTITY_ITEM).
//
// EntityArrow.onUpdate (740c583) is a bespoke ray-trace path that NEVER calls
// Entity.moveEntity -- real Beta 1.7.3 arrows therefore never trigger
// onEntityCollidedWithBlock for ANY block, pressure plates included. This is
// verified from the decompile (EntityArrow.onUpdate has no moveEntity call at
// all, in-flight or stuck), not an oversight: entity_arrow.c correctly does
// NOT call this entry point.
//
// No-op unless (x,y,z) is CURRENTLY an unpressed plate (metadata != 1) --
// mirrors Beta's own outer gate: `if (getBlockMetadata(...) != 1)
// setStateIfMobInteractsWithPlate(...)`. Safe and idempotent to call every
// tick an entity's box merely resolves onto a plate cell.
// [740c583: BlockPressurePlate.onEntityCollidedWithBlock]
void block_pressure_plate_on_entity_tick(struct server_local* s, w_coord_t x,
										 w_coord_t y, w_coord_t z);

#endif
