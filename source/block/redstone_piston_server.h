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

#ifndef REDSTONE_PISTON_SERVER_H
#define REDSTONE_PISTON_SERVER_H

#include <stdbool.h>

#include "blocks.h" // struct block_info, struct item_data
#include "piston_moving.h"

struct server_local;

// Beta: a moving-TE materializes at T+2 (updateEntity progress 0->0.5->1.0; the THIRD updateEntity,
// the same server frame the move fired + 2). Exposed so the NBT persistence bridge can convert between
// the runtime integer settle COUNTDOWN and Beta's float "progress" save field (progress = (SETTLE-
// ticks)/SETTLE). A single named constant so an oracle vector can retune it; the decompile + mc173
// evidence is 2. Used "check-then-decrement" so the 3rd te_tick visit settles.
#define PISTON_SETTLE_TICKS 2

// Production glue binding the pure redstone_piston.c (Beta BlockPistonBase decision + materialization)
// onto the live server world: the rs_ctx-style accessors, the SYNCHRONOUS move dispatch (Beta 1.7.3 has
// no block-event queue -- playNoteAt->playBlock is inline), the in-RAM moving-TE store, and the T+2
// settle driver. The Wii re-entrancy rails (the move-depth cap, the ignoreUpdates guard, the cascade
// yield) live here, mirroring the wire-flood hardening in redstone_server.c.

// Beta BlockPistonBase.onNeighborBlockChange/onBlockAdded -> updatePistonState (the synchronous extend/
// retract). Bound onto blocks 29/33 (.onNeighbourBlockChange); the base self-reacts to power changes.
void piston_server_neighbour_change(struct server_local* s, struct block_info* info);

// Beta BlockPistonExtension.onBlockRemoval: mining an extended head (id 34) drops + clears the still-
// powered base behind it. Called from the dig path (SRPC_BLOCK_DIG) with the head's pre-removal metadata.
void piston_head_on_removal(struct server_local* s, int x, int y, int z, int head_meta);

// Beta BlockPistonExtension.onNeighborBlockChange: a baseless head self-removes (orphan cleanup); else it
// forwards to the base. Bound onto block 34 (.onNeighbourBlockChange). No-op while a move is in flight.
void piston_head_neighbour_change(struct server_local* s, struct block_info* info);

// Beta BlockPistonMoving.onBlockRemoval -> TileEntityPiston.clearPistonTileEntity: force-finish the
// live moving TE when a lifecycle write replaces its id-36 cell (dig, the explosion carve, any
// overwrite). Beta's cell re-check sees the already-published replacement id, so the stored block is
// NOT re-materialized -- the in-flight move dies with its cell instead of ghost-settling later.
// Bound onto block 36 (.onBlockRemoval). [740c583: BlockPistonMoving.onBlockRemoval,
// TileEntityPiston.clearPistonTileEntity]
void piston_moving_on_removal(struct server_local* s, int x, int y, int z);

// Beta BlockPistonBase.onBlockPlacedBy: place the piston (base 33 / sticky 29) at `where` facing
// `facing` (a determineOrientation result, 0-5), then run updatePistonState. Returns false if the cell
// is entity-obstructed (the item is not consumed). Used by the piston block_item.onItemPlace.
/* S1-a1b: `actor` is the placing player whose body the canBlockBePlacedAt
 * sweep must test (never the host on a split-screen server). */
bool piston_server_place(struct server_local* s, struct server_player* actor,
		struct block_info* where, int facing, bool sticky);

// The moving-TE settle driver (Beta TileEntityPiston.updateEntity): advance every in-flight moving block
// one tick and materialize it into its stored block at PISTON_SETTLE_TICKS. Call ONCE per server tick
// from server_local_update, next to furnace_server_tick -- NOT the scheduled-tick queue, NOT onWorldTick.
void piston_moving_te_tick(struct server_local* s);

// Beta BlockPistonMoving.dropBlockAsItemWithChance: the stored block of the moving-TE at (x,y,z), for the
// block-break drop path. Returns false (and leaves *out untouched) if no moving-TE is tracked there.
bool piston_moving_stored(struct server_local* s, int x, int y, int z, struct item_data* out);

// Exact TileEntityPiston state lookup used by server collision, chunk snapshots, and tests.
bool piston_moving_state_at(struct server_local* s, int x, int y, int z,
							struct piston_moving_state* out);
bool piston_moving_state_lookup(void* ctx, int x, int y, int z,
								struct piston_moving_state* out);

#endif
