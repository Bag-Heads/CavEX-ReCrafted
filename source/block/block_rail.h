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

#ifndef BLOCK_RAIL_H
#define BLOCK_RAIL_H

#include "../network/server_local.h" // struct server_local; w_coord_t (via world.h)

// CR-05 (detector rail occupancy). Beta drives BlockDetectorRail.setStateIfMinecart-
// InteractsWithRail from TWO triggers that share one query (getEntitiesWithinAABB over
// the rail's detection box): Entity's generic per-tick "which blocks does my bbox
// overlap" scan invokes onEntityCollidedWithBlock every tick a minecart intersects the
// cell, and a self-armed 20-tick scheduled updateTick re-checks while metadata bit 0x8
// is set. This engine has no generic entity/block collision dispatch table (the
// soulsand-slowdown precedent in entity.c is likewise a bespoke per-effect call from the
// entity's own tick, not a vtable slot), so the arrival half is exposed here for the ONE
// caller that already resolves "which rail cell is this cart on" every tick:
// entity_minecart.c's minecart_server_tick. The recheck half is wired internally as
// block_detector_rail's onWorldTick (tick_delay = BlockDetectorRail.tickRate() = 20).
//
// No-op unless (x,y,z) is CURRENTLY a detector rail with metadata bit 0x8 clear --
// mirrors Beta's own outer gate: `if ((var6 & 8) == 0) this.setStateIfMinecart...`.
// Re-verifies cart presence itself via the real Beta detection-box query (does not trust
// the caller's own rail-lookup fallback), so calling this every tick a cart merely
// resolves onto a detector-rail cell is safe and idempotent once the bit is set.
// [740c583: BlockDetectorRail.onEntityCollidedWithBlock]
void block_detector_rail_on_minecart_tick(struct server_local* s, w_coord_t x,
										  w_coord_t y, w_coord_t z);

#endif
