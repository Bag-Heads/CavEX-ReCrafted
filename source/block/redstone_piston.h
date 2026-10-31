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

#ifndef REDSTONE_PISTON_H
#define REDSTONE_PISTON_H

#include <stdbool.h>

// Pure Beta 1.7.3 BlockPistonBase DECISION logic (decompile 740c583), isolated behind an injected
// world view so the SAME code runs over the live server world (production) and a flat stub cell
// grid (golden tests) -- the redstone_repeater.c precedent. DECISION-ONLY: returns booleans/ints and
// NEVER mutates the world (no set_block, no BFS materialization, no tile entity). So it is fully
// golden-testable AND carries ZERO new Wii stack/cascade risk (the actual moving-block writes are a
// LATER phase that reuses the wire-flood safety rails). The power query is delegated to the existing
// faithful boolean model (redstone_power.c) via the injected accessor.

#define RS_PISTON_STICKY_ID 29	// Block.pistonStickyBase
#define RS_PISTON_BASE_ID 33	// Block.pistonBase
#define RS_PISTON_HEAD_ID 34	// Block.pistonExtension (the head)
#define RS_PISTON_MOVING_ID 36	// Block.pistonMoving (the moving tile-entity block)
#define RS_PISTON_OBSIDIAN_ID 49

// Beta Material.mobilityFlag: 0 = movable (pushed); 1 = destroyed-on-push (not pushed, ENDS a chain);
// 2 = immovable (blocks a push).
#define RS_MOBILITY_NORMAL 0
#define RS_MOBILITY_DESTROY 1
#define RS_MOBILITY_IMMOVABLE 2

// Side numbering (Beta): 0=-Y, 1=+Y, 2=-Z, 3=+Z, 4=-X, 5=+X (matches redstone_power.h).
struct redstone_piston_world {
	void* ctx;
	int (*get_type)(void* ctx, int x, int y, int z);
	int (*get_meta)(void* ctx, int x, int y, int z);
	float (*get_hardness)(void* ctx, int id); // Block.getHardness(); -1.0 = unbreakable (bedrock)
	int (*get_mobility_flag)(void* ctx, int id); // Material.mobilityFlag (0/1/2)
	bool (*has_tile_entity)(void* ctx, int x, int y, int z); // a tile-entity block can't be pushed
	// Beta World.isBlockIndirectlyProvidingPowerTo(x,y,z,side) (wiresProvidePower TRUE) -- the SAME
	// boolean strong-power query the torch mount + repeater input already use (redstone_power.c).
	bool (*is_indirectly_providing_power)(void* ctx, int x, int y, int z, int side);

	// --- MATERIALIZATION accessors (Phase 2; NULL-safe to leave unset for the decision-only goldens).
	// These are the ONLY world-mutating hooks; isolating them here keeps the decision API pure and lets
	// the move sequence be golden-tested against a recording stub before it touches the live server.
	// Beta World.setBlockWithNotify / setBlockAndMetadataWithNotify (notify=true) vs setBlockAndMetadata
	// (notify=false): the move writes the transient id-36 cells SILENTLY (Beta setBlockAndMetadata) and
	// only the air-clears / drops / final settles NOTIFY -- matching Beta's neighbour-update ordering.
	void (*set_block)(void* ctx, int x, int y, int z, int id, int meta, bool notify);
	// Convert (x,y,z) into a Beta pistonMoving (id 36) cell + record a moving tile-entity that will
	// settle into `stored_id`:`stored_meta`. facing 0-5; extending true on a push / false on a pull;
	// is_source is true ONLY for the retracting piston base (TileEntityPiston.field_31023_j). An
	// extending piston head is not a source tile.
	// Mirrors Beta setBlockAndMetadata(pistonMoving)+setBlockTileEntity(getNewTileEntity(...)).
	void (*add_moving_te)(void* ctx, int x, int y, int z, int stored_id, int stored_meta, int facing,
						  bool extending, bool is_source);
	// Beta TileEntityPiston.clearPistonTileEntity: force-settle a still-moving id-36 cell to its stored
	// block NOW (used by retract on the front head, and by the sticky pull on the 2-ahead cell). A
	// no-op if (x,y,z) is not a live moving-TE.
	void (*settle_moving_te)(void* ctx, int x, int y, int z);
	// Beta Block.dropBlockAsItem: spill the block at (x,y,z) (id,meta) as an item (the destroy-on-push
	// terminal in tryExtend). The caller clears the cell separately via set_block(...AIR...).
	void (*spawn_drops)(void* ctx, int x, int y, int z, int id, int meta);
	// Beta BlockPistonBase.ignoreUpdates toggling (NULL-safe; the decision-only goldens leave it unset).
	// playBlock(retract) REOPENS notifications (ignoreUpdates=false) around EACH air-clear so a neighbour
	// piston on the vacated cell re-triggers THIS tick, then re-closes. The extend path keeps the guard
	// closed throughout (its terminal drop-clear is NOT reopened in Beta). piston_retract brackets exactly
	// the three Beta air-clears with this; the glue toggles the moving piston's per-id ignore flag.
	void (*set_ignore_updates)(void* ctx, bool ignore);
	// Beta TileEntityPiston.getPistonOrientation()/isExtending() for the sticky-retract var14 gate: report
	// a moving-TE's stored facing + extending flag at (x,y,z). Returns false (no out written) if (x,y,z) is
	// not a tracked moving-TE. NULL-safe (a NULL accessor means "treat any id-36 cell as not-consumable").
	bool (*moving_te_state)(void* ctx, int x, int y, int z, int* facing, bool* extending);
};

// Metadata helpers (Beta BlockPistonBase.getOrientation / isPowered).
static inline int piston_orientation(int meta) {
	return meta & 7;
}
static inline bool piston_is_powered(int meta) {
	return (meta & 8) != 0;
}

// Beta BlockPistonBase.isIndirectlyPowered(x,y,z,front): the QUASI-CONNECTIVITY check -- the 6 faces
// EXCEPT the front (each guarded `front != side`), then the piston's OWN cell, then the FIVE y+1
// cells. The (x,y+2,z) + four y+1 horizontals are the famous "block above the piston powers it" quirk.
bool piston_is_indirectly_powered(const struct redstone_piston_world* w, int x, int y, int z,
								  int front);

// Beta BlockPistonBase.canPushBlock(id,x,y,z,destroy): can the block at (x,y,z) (id) be moved? destroy
// = the destroy-on-push allowance (a mobilityFlag-1 block blocks a push only when !destroy).
bool piston_can_push_block(const struct redstone_piston_world* w, int id, int x, int y, int z,
						   bool destroy);

// Beta BlockPistonBase.canExtend(x,y,z,side): is a push of <=12 blocks in `side` from the piston legal?
bool piston_can_extend(const struct redstone_piston_world* w, int x, int y, int z, int side);

// Beta BlockPistonBase.updatePistonState two edges (the piston at (x,y,z) with its current metadata).
// should_extend already folds in the canExtend legality gate; both honour the `meta == 7` guard.
bool piston_should_extend(const struct redstone_piston_world* w, int x, int y, int z);
bool piston_should_retract(const struct redstone_piston_world* w, int x, int y, int z);

// Beta BlockPistonBase.determineOrientation: the facing (0-5) a piston placed at (bx,by,bz) by a player
// at (px,py,pz) looking at `yaw` degrees takes. yoff = the placer's eye offset (1.62 for a player).
int piston_determine_orientation(float px, float py, float pz, float yoff, float yaw, int bx, int by,
								 int bz);

// Beta PistonBlockTextures.offsetsForSide[facing] (0-5): the +1 step in the head's facing direction. The
// piston base is at (head - this); used by the head block's onBlockRemoval/onNeighborBlockChange to find
// the base cell behind it. A corrupt facing (>5) yields a zero step.
void piston_facing_offset(int facing, int* dx, int* dy, int* dz);

// Beta BlockPistonBase.tryExtend (the playBlock eventId==0 materialization): push the <=12-block column
// in `front` one cell forward -- each pushed block becomes a moving-TE (extending), the cell in front of
// the piston becomes the HEAD (id 34) moving-TE, and a mobilityFlag-1 terminal block is dropped+cleared.
// Returns true if the push materialized (false only if a re-scan finds it illegal -- canExtend gates it).
// Mutates the world ONLY through w->set_block / w->add_moving_te / w->spawn_drops. `sticky` selects the
// piston id (29 vs 33) the reverse walk recognises and the head's sticky bit.
bool piston_extend(const struct redstone_piston_world* w, int x, int y, int z, int front, bool sticky);

// Beta BlockPistonBase.playBlock eventId==1 (the retract materialization): the base cell becomes a
// moving-TE settling back to the piston, the front head is removed (non-sticky) or the block 2 ahead is
// PULLED back into the head cell (sticky). Honours the still-extending-head force-settle + the sticky
// pull gate. Mutates only through the injected accessors.
void piston_retract(const struct redstone_piston_world* w, int x, int y, int z, int front, bool sticky);

#endif
