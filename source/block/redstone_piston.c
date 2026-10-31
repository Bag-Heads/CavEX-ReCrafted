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

#include "redstone_piston.h"

#include <math.h>

// Beta PistonBlockTextures offsetsX/Y/ZForSide -- the unit step into a face, indexed by side.
// Sized 8, not 6: piston_orientation()/the `side` args are `meta & 7` (0..7), so a corrupt
// orientation of 6 or 7 would index past a [6] array -> out-of-bounds read (UB). Indices 6/7
// give a zero step (the piston's own cell) -- a safe no-op that matches piston_facing_offset's
// "corrupt 6/7 yields a zero step" guard. Valid orientations 0..5 are unchanged.
static const int OFF_X[8] = {0, 0, 0, 0, -1, 1, 0, 0};
static const int OFF_Y[8] = {-1, 1, 0, 0, 0, 0, 0, 0};
static const int OFF_Z[8] = {0, 0, -1, 1, 0, 0, 0, 0};

bool piston_is_indirectly_powered(const struct redstone_piston_world* w, int x, int y, int z,
								  int front) {
	// Beta BlockPistonBase.isIndirectlyPowered, transcribed branch-for-branch (12 probes; the front
	// face is skipped by its `front != side` guard). The trailing 5 -- the own cell, (x,y+2,z) and the
	// four (y+1) horizontals -- are the quasi-connectivity ("block above powers the piston") quirk.
#define IPP(px, py, pz, side) w->is_indirectly_providing_power(w->ctx, (px), (py), (pz), (side))
	if(front != 0 && IPP(x, y - 1, z, 0))
		return true;
	if(front != 1 && IPP(x, y + 1, z, 1))
		return true;
	if(front != 2 && IPP(x, y, z - 1, 2))
		return true;
	if(front != 3 && IPP(x, y, z + 1, 3))
		return true;
	if(front != 5 && IPP(x + 1, y, z, 5))
		return true;
	if(front != 4 && IPP(x - 1, y, z, 4))
		return true;
	if(IPP(x, y, z, 0)) // the piston's own cell
		return true;
	if(IPP(x, y + 2, z, 1)) // <-- the block-above BUD quirk
		return true;
	if(IPP(x, y + 1, z - 1, 2))
		return true;
	if(IPP(x, y + 1, z + 1, 3))
		return true;
	if(IPP(x - 1, y + 1, z, 4))
		return true;
	return IPP(x + 1, y + 1, z, 5);
#undef IPP
}

bool piston_can_push_block(const struct redstone_piston_world* w, int id, int x, int y, int z,
						   bool destroy) {
	// Beta BlockPistonBase.canPushBlock, exact order.
	if(id == RS_PISTON_OBSIDIAN_ID)
		return false;
	if(id != RS_PISTON_BASE_ID && id != RS_PISTON_STICKY_ID) {
		if(w->get_hardness(w->ctx, id) == -1.0F) // bedrock / any unbreakable block
			return false;
		int m = w->get_mobility_flag(w->ctx, id);
		if(m == RS_MOBILITY_IMMOVABLE)
			return false;
		if(!destroy && m == RS_MOBILITY_DESTROY)
			return false;
	} else if(piston_is_powered(w->get_meta(w->ctx, x, y, z))) {
		return false; // a powered (extended) piston base cannot itself be pushed
	}
	return !w->has_tile_entity(w->ctx, x, y, z); // a tile-entity block cannot move
}

bool piston_can_extend(const struct redstone_piston_world* w, int x, int y, int z, int side) {
	// Beta BlockPistonBase.canExtend: scan the push column from the front cell; at most 12 movable
	// blocks (the twelve-block limit); a mobilityFlag-1 (destroy-on-push) block or air ENDS the chain.
	int cx = x + OFF_X[side], cy = y + OFF_Y[side], cz = z + OFF_Z[side];
	int n = 0;
	for(;;) {
		if(n < 13) {
			if(cy <= 0 || cy >= 127) // Beta world-height guard
				return false;
			int id = w->get_type(w->ctx, cx, cy, cz);
			if(id != 0) {
				if(!piston_can_push_block(w, id, cx, cy, cz, true))
					return false;
				if(w->get_mobility_flag(w->ctx, id) != RS_MOBILITY_DESTROY) {
					if(n == 12)
						return false; // the 13th movable block: over the 12-block limit
					cx += OFF_X[side];
					cy += OFF_Y[side];
					cz += OFF_Z[side];
					n++;
					continue;
				}
			}
		}
		return true;
	}
}

bool piston_should_extend(const struct redstone_piston_world* w, int x, int y, int z) {
	int meta = w->get_meta(w->ctx, x, y, z);
	if(meta == 7) // Beta updatePistonState ignores metadata 7 (invalid/uninitialised state)
		return false;
	int front = piston_orientation(meta);
	// EXTEND when powered AND not already powered/extended AND the push is legal.
	return piston_is_indirectly_powered(w, x, y, z, front) && !piston_is_powered(meta)
		&& piston_can_extend(w, x, y, z, front);
}

bool piston_should_retract(const struct redstone_piston_world* w, int x, int y, int z) {
	int meta = w->get_meta(w->ctx, x, y, z);
	if(meta == 7)
		return false;
	int front = piston_orientation(meta);
	// RETRACT when NOT powered AND currently powered/extended.
	return !piston_is_indirectly_powered(w, x, y, z, front) && piston_is_powered(meta);
}

bool piston_extend(const struct redstone_piston_world* w, int x, int y, int z, int front, bool sticky) {
	// Beta BlockPistonBase.tryExtend, transcribed. Phase 1's piston_can_extend already proved the push
	// legal; tryExtend re-walks to (a) drop a mobilityFlag-1 terminal and (b) shift the column.
	int base_id = sticky ? RS_PISTON_STICKY_ID : RS_PISTON_BASE_ID;
	int head_meta = front | (sticky ? 8 : 0); // Beta side|(isSticky?8:0): the head carries the sticky bit
	int cx = x + OFF_X[front], cy = y + OFF_Y[front], cz = z + OFF_Z[front];
	int n = 0;
	// FORWARD scan: advance to the column's end (air or a destroy-on-push terminal).
	for(;;) {
		if(n < 13) {
			if(cy <= 0 || cy >= 127) // Beta excludes the bottom and top world layers from the push scan
				return false;
			int id = w->get_type(w->ctx, cx, cy, cz);
			if(id != 0) {
				if(!piston_can_push_block(w, id, cx, cy, cz, true)) // destroy=true
					return false;
				if(w->get_mobility_flag(w->ctx, id) != RS_MOBILITY_DESTROY) {
					if(n == 12)
						return false; // 13th movable -> over the limit
					cx += OFF_X[front];
					cy += OFF_Y[front];
					cz += OFF_Z[front];
					n++;
					continue;
				}
				// mobilityFlag-1 terminal: drop it + clear it (Beta dropBlockAsItem + setBlockWithNotify 0).
				w->spawn_drops(w->ctx, cx, cy, cz, id, w->get_meta(w->ctx, cx, cy, cz));
				w->set_block(w->ctx, cx, cy, cz, 0, 0, true);
			}
			break; // air OR a just-cleared terminal: the column end -- start the reverse shift
		}
		break;
	}
	// REVERSE walk: from the end cell back to the piston, each block shifts one cell forward into the
	// (now vacated) cell ahead of it. The cell directly in front of the piston becomes the HEAD.
	while(cx != x || cy != y || cz != z) {
		int px = cx - OFF_X[front], py = cy - OFF_Y[front], pz = cz - OFF_Z[front];
		int pid = w->get_type(w->ctx, px, py, pz);
		int pmeta = w->get_meta(w->ctx, px, py, pz);
		if((pid == base_id) && px == x && py == y && pz == z) {
			// the source cell is the piston itself -> place the extension HEAD (id 34) as a moving-TE.
			w->add_moving_te(w->ctx, cx, cy, cz, RS_PISTON_HEAD_ID, head_meta, front, /*extending*/ true,
							 /*is_source*/ false);
		} else {
			// a pushed block -> it moves forward into (cx,cy,cz) as a moving-TE storing its id/meta.
			w->add_moving_te(w->ctx, cx, cy, cz, pid, pmeta, front, /*extending*/ true, /*is_source*/ false);
		}
		cx = px;
		cy = py;
		cz = pz;
	}
	return true;
}

void piston_retract(const struct redstone_piston_world* w, int x, int y, int z, int front, bool sticky) {
	// Beta BlockPistonBase.playBlock eventId==1, transcribed. The base cell becomes a retracting moving-
	// TE (settles back to the piston); the front head is removed (non-sticky) or the block 2 ahead is
	// pulled back (sticky).
	int base_id = sticky ? RS_PISTON_STICKY_ID : RS_PISTON_BASE_ID;
	int fx = x + OFF_X[front], fy = y + OFF_Y[front], fz = z + OFF_Z[front]; // the front (head) cell
	// Beta brackets each air-clear with ignoreUpdates=false/true so a neighbour piston on the vacated cell
	// re-triggers THIS tick (the force-settle + the base->moving-TE write are NOT reopened in Beta).
#define RS_CLEAR_NOTIFY(W, CX, CY, CZ)                                                                  \
	do {                                                                                               \
		if((W)->set_ignore_updates)                                                                    \
			(W)->set_ignore_updates((W)->ctx, false);                                                  \
		(W)->set_block((W)->ctx, (CX), (CY), (CZ), 0, 0, true);                                         \
		if((W)->set_ignore_updates)                                                                    \
			(W)->set_ignore_updates((W)->ctx, true);                                                   \
	} while(0)
	// Force-settle a still-extending head moving-TE at the front cell FIRST (Beta clearPistonTileEntity).
	w->settle_moving_te(w->ctx, fx, fy, fz);
	// The base cell is the sole source moving-TE (extending=false, field_31023_j/source=true).
	w->add_moving_te(w->ctx, x, y, z, base_id, front, front, /*extending*/ false, /*is_source*/ true);
	if(sticky) {
		// the block 2 ahead of the piston -- the candidate to be pulled into the head cell.
		int ax = x + OFF_X[front] * 2, ay = y + OFF_Y[front] * 2, az = z + OFF_Z[front] * 2;
		int aid = w->get_type(w->ctx, ax, ay, az);
		int ameta = w->get_meta(w->ctx, ax, ay, az);
		bool consumed = false; // Beta var14: a still-extending moving-TE 2-ahead is force-settled, not pulled
		if(aid == RS_PISTON_MOVING_ID) {
			// Beta gates this on the moving-TE being extending AND in the SAME orientation as this piston
			// a matching orientation and an extending state; only then settle it, treat its
			// STORED block as the candidate, and mark consumed. An unrelated moving-TE is left untouched
			// (aid stays 36 -> !canPushBlock -> the head cell is cleared below).
			int mface;
			bool mext;
			if(w->moving_te_state && w->moving_te_state(w->ctx, ax, ay, az, &mface, &mext) && mface == front
			   && mext) {
				w->settle_moving_te(w->ctx, ax, ay, az);
				aid = w->get_type(w->ctx, ax, ay, az);
				ameta = w->get_meta(w->ctx, ax, ay, az);
				consumed = true;
			}
		}
		// Beta gate: pull NOTHING if consumed, empty, unpushable, or an immovable/destroy block (a piston
		// base/sticky-base is the lone non-zero-mobility exception that may still be pulled).
		if(consumed || aid <= 0 || !piston_can_push_block(w, aid, ax, ay, az, false)
		   || (w->get_mobility_flag(w->ctx, aid) != RS_MOBILITY_NORMAL && aid != RS_PISTON_BASE_ID
			   && aid != RS_PISTON_STICKY_ID)) {
			if(!consumed)
				RS_CLEAR_NOTIFY(w, fx, fy, fz); // clear the head cell -> air
		} else {
			RS_CLEAR_NOTIFY(w, ax, ay, az); // vacate the 2-ahead cell
			// the pulled block moves into the head cell as a retracting moving-TE.
			w->add_moving_te(w->ctx, fx, fy, fz, aid, ameta, front, /*extending*/ false, /*is_source*/ false);
		}
	} else {
		RS_CLEAR_NOTIFY(w, fx, fy, fz); // non-sticky: the head cell -> air
	}
#undef RS_CLEAR_NOTIFY
}

int piston_determine_orientation(float px, float py, float pz, float yoff, float yaw, int bx, int by,
								 int bz) {
	// Beta BlockPistonBase.determineOrientation. The up/down special-case fires only when the placer
	// is within 2 blocks horizontally; otherwise the facing comes from the yaw octant. Computed in
	// double where Beta does (player posY is double in Beta; CaveWii passes a float, the only drift).
	if(fabsf((float)bx - px) < 2.0F && fabsf((float)bz - pz) < 2.0F) {
		double v = (double)py + 1.82 - (double)yoff;
		if(v - (double)by > 2.0)
			return 1; // UP (+Y)
		if((double)by - v > 0.0)
			return 0; // DOWN (-Y)
	}
	int i = (int)floor((double)(yaw * 4.0F / 360.0F) + 0.5) & 3;
	return i == 0 ? 2 : (i == 1 ? 5 : (i == 2 ? 3 : (i == 3 ? 4 : 0)));
}

// Beta PistonBlockTextures.offsets{X,Y,Z}ForSide[facing] (0-5): the +1 unit step in the head's facing
// direction. The piston BASE sits one cell BEHIND the head -- at (head - this) -- which is the cell
// BlockPistonExtension.onBlockRemoval/onNeighborBlockChange step to (Beta uses offsets[faceToSide[facing]];
// faceToSide is the opposite face, and offsets[opposite] == -offsets[facing], so the cell is identical).
// Out-of-range facing (a corrupt 6/7) yields a zero step (the caller then sees its own cell, not a base).
void piston_facing_offset(int facing, int* dx, int* dy, int* dz) {
	if(facing < 0 || facing > 5) {
		*dx = *dy = *dz = 0;
		return;
	}
	*dx = OFF_X[facing];
	*dy = OFF_Y[facing];
	*dz = OFF_Z[facing];
}
