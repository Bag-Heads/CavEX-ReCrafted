/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#ifndef ENTITY_LOS_H
#define ENTITY_LOS_H

// Pure, world-free line-of-sight voxel traversal -- the gameplay-equivalent core of
// Beta 1.7.3's canEntityBeSeen / World.rayTraceBlocks (used by mob AI to decide
// whether a mob can see the player). This header deliberately knows NOTHING about
// the server world or the block registry: it walks the integer voxels a segment
// crosses and asks a caller-supplied predicate `is_opaque(x,y,z,ctx)` whether each
// cell blocks the ray. That split lets the exact traversal be unit-tested directly
// (tests/test_mob_los.c) with a tiny in-test grid, while entity_monster.c wires the
// predicate to server_world_get_block + the blocks[] LOS-blocker test.
//
// FAITHFULNESS (Beta 1.7.3, github.com/PeytonPlayz595/Beta-1.7.3
// net.minecraft.src.World.func_28105_a, L702-851): the original is an incremental
// boundary-crossing ray walk, NOT a fixed-step point sampler -- a fixed step leaks
// the ray through thin/diagonal walls (steps over a shared face) and is non-faithful.
// We use the equivalent incremental boundary-crossing DDA (Amanatides & Woo "A Fast
// Voxel Traversal Algorithm"):
//   * Floor both endpoints to integer cells.
//   * Each step advance to the nearest axis-plane the ray crosses (smallest t of the
//     three candidate planes), pick that cell on the far side of the crossed face,
//     and test it (cf. World.java L765-841). This visits every cell the segment
//     passes through, including corner-touched cells, so nothing is skipped and no
//     thin/diagonal wall leaks.
//   * When the walk reaches the STOP cell, return clear -- the stop (target-eye)
//     cell is NOT tested as a blocker (cf. L728-730 returning before the block test).
//   * Beta caps the walk at 200 steps; we cap at LOS_MAX_STEPS (sized to the 16-block
//     follow range: a segment spanning N cells per axis crosses at most ~3N faces).
//
// This is GAMEPLAY-EQUIVALENT, not byte-identical to Beta's exact stepping: the core
// invariants the gate cares about hold exactly -- BLOCKED iff a solid lies STRICTLY
// BETWEEN the eyes, an open line is CLEAR, and a thin/diagonal wall never leaks (all
// verified empirically + by tests/test_mob_los.c). At an exact corner/edge TIE where
// the ray grazes a shared plane, Amanatides-Woo and Beta's particular stepping scheme
// may select a different (but equally gap-free) cell to visit; that tie-break choice
// never changes the blocked/clear RESULT for a real wall or a real gap, only which of
// two coincident cells gets the predicate call first.
//
// ONE DELIBERATE DEVIATION FROM BETA: Beta tests the START cell as a blocker
// (L711-719) but EXCLUDES the stop cell. We exclude BOTH endpoints. Rationale: the
// start cell is the MOB'S OWN EYE cell and the end cell is the PLAYER'S EYE cell;
// neither should self-occlude. In Beta the start eye-cell is virtually always air
// (the mob's head), so excluding it is observably equivalent in the gameplay-
// relevant case -- AND it guarantees a mob standing in a non-solid-but-not-see-
// through plant (e.g. tall grass mapped opaque) or a player in a doorway block is
// never falsely blinded by the block they occupy. This matches the task's explicit
// "exclude start/end so a mob in grass or a player in a doorway isn't falsely
// blocked" requirement; only voxels STRICTLY BETWEEN the eyes can block.

#include <math.h>
#include <stdbool.h>

// Upper bound on cells visited. The mob LOS gate only fires within the 16-block
// follow range; an eye-to-eye segment of length L crosses at most ceil(|dx|)+
// ceil(|dy|)+ceil(|dz|) axis planes. With each axis bounded by the world height
// (128) and the follow range, 512 is a safe, generous cap that can never be hit by
// a legitimate in-range LOS query yet guarantees termination on any degenerate or
// out-of-range input (matches Beta's finite 200-step guard in spirit).
#define LOS_MAX_STEPS 512

// Caller-supplied opacity predicate: return true iff the voxel (x,y,z) BLOCKS the
// line of sight. `ctx` is an opaque pointer threaded through unchanged.
typedef bool (*los_is_opaque_fn)(int x, int y, int z, void* ctx);

// The Beta-faithful "does this block occlude an eye-to-eye LOS ray?" decision,
// factored out of the world predicate so it can be unit-tested directly against the
// exact rule the server uses (tests/test_mob_los.c), with no copy to drift. Takes the
// three registry flags that matter; the world wiring (entity_monster.c::
// mob_los_cell_opaque) looks them up from blocks[type] and forwards them here.
//
// Beta's rayTraceBlocks blocker test is `id>0 && canCollideCheck(meta,false) &&
// collisionRayTrace hits the AABB`. We reproduce that as: a block occludes iff
//   (a) it is NOT see-through (every full solid: stone/wool/double-slab/...), OR
//   (b) it is a full-footprint solid the MESHER flags see-through but whose solid
//       AABB still fully crosses an eye-to-eye line -- SLAB, STAIRS, FARMLAND, which
//       carry opacity==15. The !place_ignore guard drops the only other opacity-15
//       see-through blocks, the FLUIDS (lava is opacity 15 + place_ignore), which
//       Beta's canCollideCheck(meta,false) passes -- so LOS still crosses lava/water.
// Thin decorations (torch/flower/wire/fence/ladder/crops/grass/cactus/cobweb -- all
// opacity 0..1) pass, matching Beta's tiny-AABB miss. DOCUMENTED DEVIATION: full-cube
// GLASS (opacity 0) + LEAVES (opacity 1) block LOS in Beta but stay see-through here.
static inline bool los_block_occludes(bool can_see_through, int opacity,
									  bool place_ignore) {
	return !can_see_through || (opacity == 15 && !place_ignore);
}

// Walk the segment fx0,fy0,fz0 -> fx1,fy1,fz1 through the voxel grid and return
// true iff some cell STRICTLY BETWEEN the endpoints (BOTH the start and end cells
// excluded -- see the deviation note above) is opaque, i.e. the line of sight is
// BLOCKED. Returns false (clear/visible) for a degenerate near-zero-length segment
// (point-blank: trivially visible, no NaN, no infinite loop). Deterministic and
// allocation-free.
static inline bool los_segment_blocked(float fx0, float fy0, float fz0, float fx1,
									   float fy1, float fz1,
									   los_is_opaque_fn is_opaque, void* ctx) {
	// Current cell (floor of the start point) and the destination cell.
	int gx = (int)floorf(fx0);
	int gy = (int)floorf(fy0);
	int gz = (int)floorf(fz0);
	const int x1 = (int)floorf(fx1);
	const int y1 = (int)floorf(fy1);
	const int z1 = (int)floorf(fz1);

	// If start == end the segment is contained in one cell -- clear by definition
	// (point-blank / same-column degenerate case). The START cell is NOT tested as a
	// blocker (excluded; see the header note) -- the first cell tested is the next
	// one the ray steps into.
	if(gx == x1 && gy == y1 && gz == z1)
		return false;

	const float dx = fx1 - fx0;
	const float dy = fy1 - fy0;
	const float dz = fz1 - fz0;

	// Per-axis step direction.
	const int sx = (dx > 0.0f) ? 1 : -1;
	const int sy = (dy > 0.0f) ? 1 : -1;
	const int sz = (dz > 0.0f) ? 1 : -1;

	// tMax: parametric distance (along the segment, t in [0,1]) to the FIRST axis-
	// plane crossing on each axis. tDelta: t to cross one full cell on that axis.
	// An axis with (near-)zero delta never crosses a plane -> tMax/tDelta = +inf so
	// the min() below ignores it (handles the same-XZ-column / axis-aligned case).
	const float INF = 1.0e30f;
	float t_max_x, t_max_y, t_max_z;
	float t_delta_x, t_delta_y, t_delta_z;

	if(fabsf(dx) < 1.0e-8f) {
		t_max_x = INF;
		t_delta_x = INF;
	} else {
		// Next integer plane in the step direction.
		float next = (sx > 0) ? (float)(gx + 1) : (float)gx;
		t_max_x = (next - fx0) / dx;
		t_delta_x = (float)sx / dx; // == 1/|dx|
	}
	if(fabsf(dy) < 1.0e-8f) {
		t_max_y = INF;
		t_delta_y = INF;
	} else {
		float next = (sy > 0) ? (float)(gy + 1) : (float)gy;
		t_max_y = (next - fy0) / dy;
		t_delta_y = (float)sy / dy;
	}
	if(fabsf(dz) < 1.0e-8f) {
		t_max_z = INF;
		t_delta_z = INF;
	} else {
		float next = (sz > 0) ? (float)(gz + 1) : (float)gz;
		t_max_z = (next - fz0) / dz;
		t_delta_z = (float)sz / dz;
	}

	for(int step = 0; step < LOS_MAX_STEPS; step++) {
		// Beta World.rayTraceBlocks (CFR server World.java:562-594;
		// Jad World.java:841-875) disables an axis once its cell equals
		// the destination. Otherwise a negative t=1 crossing can step
		// PAST an integer endpoint before the other axes finish a tie.
		if(gx == x1)
			t_max_x = INF;
		if(gy == y1)
			t_max_y = INF;
		if(gz == z1)
			t_max_z = INF;

		// Advance to the nearest axis-plane crossing (smallest tMax). Ties resolve to
		// X then Y then Z, matching camera_ray_pick's stepping; the visited-cell set
		// is identical either way for a blocker test.
		if(t_max_x <= t_max_y && t_max_x <= t_max_z) {
			gx += sx;
			t_max_x += t_delta_x;
		} else if(t_max_y <= t_max_z) {
			gy += sy;
			t_max_y += t_delta_y;
		} else {
			gz += sz;
			t_max_z += t_delta_z;
		}

		// Reached the destination (target-eye) cell: clear. The END cell is NOT
		// tested as a blocker (Beta returns at L728-730 before the block test).
		if(gx == x1 && gy == y1 && gz == z1)
			return false;

		if(is_opaque(gx, gy, gz, ctx))
			return true;
	}

	// Step cap hit (only on a degenerate / out-of-range segment the caller should
	// have distance-gated away). Fail OPEN: treat as visible rather than blinding the
	// mob, consistent with the unloaded-cell fail-open in the world predicate.
	//
	// CALLER CONTRACT: this fail-open means a blocker sitting BEYOND the cap (~170
	// cells of crossed planes) is NOT detected -> a long over-length query would see
	// THROUGH a wall. That is safe ONLY because every production caller distance-gates
	// the segment to the 16-block mob follow range (<= ~48 crossings, far under the
	// cap), so the cap is unreachable for a legitimate in-range LOS query. A FUTURE
	// caller that runs LOS over a longer segment (e.g. a raised-LOD reach or a >16-
	// block sensor) MUST keep its segment well under LOS_MAX_STEPS, or change this
	// fall-through to fail-CLOSED (`return true`) for its over-length case, so it does
	// not silently inherit a see-through-wall bug.
	return false;
}

#endif
