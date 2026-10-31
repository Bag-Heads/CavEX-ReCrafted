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

#ifndef ENTITY_PATHFINDING_H
#define ENTITY_PATHFINDING_H

// FH-13: Beta 1.7.3 A* pathfinding -- a faithful, pure, world-free port of
// net.minecraft.src.Pathfinder / PathEntity / PathPoint / Path (decompile ref
// 740c583, tmp/decompile_Path{finder,Entity,Point}.java + tmp/decompile_Path.java),
// mirroring los.h's split: the traversal/search core here knows NOTHING about the
// server world or block registry -- it asks a caller-supplied predicate
// `pathfinder_query_fn` to classify a single voxel, so the exact algorithm can be
// unit-tested against a tiny synthetic grid (tests/test_pathfinding_golden.c) while
// entity_monster.c wires the predicate to server_world_get_block + blocks[].
//
// SHAPE DIFFERENCES FROM BETA (all deliberate, all documented -- none touch the
// SEARCH RESULT for any search that fits the bounds below):
//  1. Beta's `Path` heap starts at 1024 entries and DOUBLES (System.arraycopy) when
//     full -- unbounded. This engine never mallocs on a per-tick hot path
//     (CODESTYLE.md 1.7), so PATHFINDER_POOL_CAPACITY hard-caps the node pool at
//     1024 -- Beta's OWN initial capacity, cited directly from Path.java:4 (the point
//     heap array is created with 1024 slots), not an invented number. When
//     the pool is exhausted mid-search, pathfinder_open_point() simply refuses to
//     mint a new point (returns NULL); the caller-visible effect is a search that
//     stops discovering new cells and returns whatever partial/closest-effort path
//     it already has -- EXACTLY Beta's own "no path found -> return the closest
//     point reached, or null if the start never improved" fallback (addToPath's
//     closest-point best-effort tracking), never a crash or a truncated/wrong path. A
//     pathological open-volume geometry that would have made Beta grow the array
//     past 1024 instead degrades cleanly to "search stops early", which is the
//     honest, documented overflow policy the FH-13 brief asks for.
//  2. Beta's `pathOptions` scratch buffer is declared `PathPoint[32]` but
//     findPathOptions only EVER fills indices 0-3 (the four cardinal neighbours);
//     the other 28 slots are permanently dead space (no diagonal moves exist in
//     1.7.3 pathfinding). We size the real buffer to 4 -- behaviourally identical,
//     just without the dead space.
//  3. PathEntity's waypoint array is Beta's exact reconstructed chain length
//     (dynamically allocated). PATH_ENTITY_MAX_WAYPOINTS caps it at 64 -- generous
//     for the search ranges this port actually uses (16.0F chase / 10.0F wander;
//     ordinary terrain detours around a single wall/gap/structure stay well under
//     64 waypoints). Overflow (a reconstructed chain longer than the cap) fails the
//     search -- same "mob stands" policy as note 1, cited at the call site.
//  4. Beta's Pathfinder/Path/PathPoint/MCHash are all separately `new`-ed per call
//     (a fresh Pathfinder over a ChunkCache in World.getPathToEntity/getEntityPathToXYZ).
//     This engine reuses ONE static struct pathfinder scratch instance across every
//     search this tick (CODESTYLE.md 1.7's bounded-static-buffer pattern);
//     pathfinder_reset() is called at the top of every search, mirroring Beta's own
//     clearing of its path heap and point map (Pathfinder.java:22) -- so this is an allocation
//     strategy change only, not a behaviour change (a fresh Pathfinder and a reset
//     one are observationally identical: both start with an empty heap and an empty
//     point map).
//  5. Entity positions in this engine are float (CODESTYLE.md 2.4: "the
//     float-position delta is a recorded deviation -- don't upgrade entity math to
//     double ad hoc"), so createEntityPathTo's double parameters become float here.
//     This is the SAME deviation the rest of the entity/ tree already carries, not
//     a new one introduced by this port.

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- Node pool / search bounds (see note 1/2/3 above for the citations) --------
#ifndef PATHFINDER_POOL_CAPACITY // overridable only for the w3 measurement build (docs/intent/w3-pathfinder-node-cap.md)
#define PATHFINDER_POOL_CAPACITY 1024 // == Beta Path.java's initial PathPoint[1024]
#endif
// Open-addressing table over the pool; 4x load factor headroom keeps probe chains
// short without link-list bucket overhead (a bounded array, no allocation).
#ifndef PATHFINDER_HASH_CAPACITY
#define PATHFINDER_HASH_CAPACITY 4096
#endif
#define PATH_ENTITY_MAX_WAYPOINTS 64 // see note 3 above

// --- PathPoint [740c583: PathPoint.java] ---------------------------------------
// One block-coordinate node. `previous` is a pointer into the OWNING
// struct pathfinder's `pool` array (stable addresses: the pool is a plain
// embedded array, never reallocated) -- direct object references, exactly like
// Beta's own `PathPoint previous` field, so the reconstructed chain (createEntityPath)
// is a trivial pointer walk with no index translation.
struct path_point {
	int32_t x, y, z;
	int32_t hash; // PathPoint.func_22329_a(x,y,z), cached at construction like Beta
	// Beta's `index`: >=0 iff currently sitting in the Path heap at that slot
	// (doubles as isAssigned()); -1 once dequeued. NOT the same as `is_first` below
	// (a dequeued point is unassigned again but stays is_first forever).
	int32_t heap_index;
	float total_path_distance; // Beta totalPathDistance: accumulated path cost g()
	float distance_to_next;    // Beta distanceToNext: distanceTo(target), h()-ish
	float distance_to_target;  // Beta distanceToTarget: the heap's sort key (g+h)
	struct path_point* previous;
	// Beta isFirst: set true the instant this point is DEQUEUED (closed-set
	// membership), and NEVER cleared. findPathOptions filters `!candidate.isFirst`,
	// so a closed point can never be re-offered as a neighbour again -- this is a
	// standard A* closed-set guard wearing an odd Beta field name, not a "point can
	// be reopened" mechanic (verified against the decompile: a point can still be
	// re-assigned cheaper totalPathDistance WHILE still in the open heap via
	// changeDistance, but never after being dequeued).
	bool is_first;
};

// PathPoint.func_22329_a(x,y,z) [740c583]: packs (x,y,z) into one hash, biased so
// negative x/z set the sign-adjacent bits Beta's own literal (`Integer.MIN_VALUE`,
// `'耀'`==0x8000) sets. Transcribed verbatim in unsigned arithmetic (Java's
// `<<`/`|` on int are the same bit ops as C's on uint32_t; the final cast back to
// int32_t matches Java's signed int reinterpretation).
static inline int32_t path_point_hash(int32_t x, int32_t y, int32_t z) {
	uint32_t h = ((uint32_t)y & 255u) | (((uint32_t)x & 32767u) << 8)
		| (((uint32_t)z & 32767u) << 24) | (x < 0 ? 0x80000000u : 0u)
		| (z < 0 ? 0x00008000u : 0u);
	return (int32_t)h;
}

static inline void path_point_init(struct path_point* p, int32_t x, int32_t y,
									int32_t z) {
	p->x = x;
	p->y = y;
	p->z = z;
	p->hash = path_point_hash(x, y, z);
	p->heap_index = -1;
	p->total_path_distance = 0.0f;
	p->distance_to_next = 0.0f;
	p->distance_to_target = 0.0f;
	p->previous = NULL;
	p->is_first = false;
}

// PathPoint.distanceTo [740c583]: float Euclidean distance between two node
// coordinates. Beta computes this with MathHelper.sqrt_float, which (per
// CODESTYLE.md 2.3; MathHelper.java:15) is a double sqrt cast to float -- an ordinary sqrt, NOT the
// mh_sin/mh_cos lookup-table family (those replace Beta's angle tables only). A
// plain sqrtf is therefore the faithful, not a substituted, implementation.
static inline float path_point_distance_to(const struct path_point* a,
											const struct path_point* b) {
	float dx = (float)(b->x - a->x);
	float dy = (float)(b->y - a->y);
	float dz = (float)(b->z - a->z);
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// PathPoint.equals [740c583]: hash + all three coordinates.
static inline bool path_point_equals(const struct path_point* a,
									 const struct path_point* b) {
	return a->hash == b->hash && a->x == b->x && a->y == b->y && a->z == b->z;
}

// PathPoint.isAssigned [740c583].
static inline bool path_point_is_assigned(const struct path_point* p) {
	return p->heap_index >= 0;
}

// --- Path: the binary min-heap over distance_to_target [740c583: Path.java] ----
// sortBack/sortForward are transcribed verbatim (see pathfinding.c); this header
// only carries the struct + declares the entry points so tests can drive the heap
// directly (tests/test_pathfinding_golden.c "Path heap" cases) without running a
// full search.
struct path_heap {
	struct path_point* items[PATHFINDER_POOL_CAPACITY];
	int32_t count;
};

void path_heap_clear(struct path_heap* h);
bool path_heap_is_empty(const struct path_heap* h);
// Path.addPoint [740c583]. Precondition (Beta: throws IllegalStateException):
// p->heap_index < 0 (not already in a heap) -- a compound-precondition assert per
// CODESTYLE.md 1.6, never legitimately reachable (findPathOptions' `!is_first`
// filter guarantees a candidate offered here was never dequeued, so it is either
// brand new or still resident in the heap, and the `is_assigned` branch in
// pathfinder_add_to_path already routes the "still resident" case to
// path_heap_change_distance instead). Returns false (no-op) iff the heap is at
// PATHFINDER_POOL_CAPACITY -- the pool-exhaustion overflow policy (see header note
// 1); the pool itself is sized 1:1 with the heap so this can only trip alongside
// pathfinder_open_point() already having refused new points.
bool path_heap_add_point(struct path_heap* h, struct path_point* p);
// Path.dequeue [740c583]: pop the min (root), fix the root down (sortForward), and
// mark the popped point unassigned (heap_index = -1). Precondition: !is_empty().
struct path_point* path_heap_dequeue(struct path_heap* h);
// Path.changeDistance [740c583]: update a resident point's sort key and re-heapify
// from its current slot (sortBack if it got smaller, sortForward if larger).
void path_heap_change_distance(struct path_heap* h, struct path_point* p,
							   float new_distance_to_target);

// --- PathEntity: the waypoint cursor [740c583: PathEntity.java] ---------------
struct path_waypoint {
	int32_t x, y, z;
};

struct path_entity {
	struct path_waypoint points[PATH_ENTITY_MAX_WAYPOINTS];
	int32_t length; // Beta pathLength; 0 == "no path" (Beta would be a null
	                // PathEntity reference -- this engine uses length==0 as that
	                // sentinel instead of a pointer, so callers test length>0).
	int32_t index;  // Beta pathIndex, the waypoint cursor
};

static inline void path_entity_clear(struct path_entity* p) {
	p->length = 0;
	p->index = 0;
}

// PathEntity.isFinished [740c583].
static inline bool path_entity_is_finished(const struct path_entity* p) {
	return p->index >= p->length;
}

// PathEntity.incrementPathIndex [740c583].
static inline void path_entity_increment(struct path_entity* p) {
	p->index++;
}

// PathEntity.getPosition(Entity) [740c583]: the CURRENT waypoint's world position,
// offset on X/Z by half the truncated (width + 1.0F) [PathEntity.java:26,28] -- for a normal (<1-wide) mob
// this is the block-centre 0.5 offset; for a wide mob (width >= 1.0, e.g. the
// spider's 1.4) the truncating (int) cast makes this 1.0 instead of 0.5, a real
// Beta quirk (wide mobs aim at the FAR corner of the target cell, not its centre)
// that this port keeps verbatim -- do not "fix" it into a flat 0.5. Y is the RAW
// waypoint y (the cell's own coordinate -- entities are feet-at-origin, matching
// Beta's posY convention, so no further offset is added). Caller must have already
// checked !path_entity_is_finished(p).
static inline void path_entity_get_position(const struct path_entity* p,
											 float entity_width, float* out_x,
											 float* out_y, float* out_z) {
	float offset = (float)(int32_t)(entity_width + 1.0f) * 0.5f;
	const struct path_waypoint* wp = &p->points[p->index];
	*out_x = (float)wp->x + offset;
	*out_y = (float)wp->y;
	*out_z = (float)wp->z + offset;
}

// --- World query callback -------------------------------------------------------
// One voxel's classification for getVerticalOffset's per-cell test [740c583:
// Pathfinder.getVerticalOffset]. Folds Beta's two-stage test (door special-case,
// then Material.getIsSolid/water/lava) into one callback result so the caller
// (entity_monster.c, wired to server_world_get_block + blocks[]) does the id/door/
// material lookup once per cell; the search core only branches on the outcome.
//   PATH_CELL_PASSABLE -- air, OR any block Beta's `id>0` guard skips (id==0), OR
//                         an OPEN door (BlockDoor.isOpen(meta)).
//   PATH_CELL_SOLID    -- a solid-material block, OR a CLOSED door (Beta returns 0
//                         for both -- "blocks movement").
//   PATH_CELL_WATER    -- Material.water.
//   PATH_CELL_LAVA     -- Material.lava.
enum pathfinder_cell_kind {
	PATH_CELL_PASSABLE = 0,
	PATH_CELL_SOLID,
	PATH_CELL_WATER,
	PATH_CELL_LAVA,
};

typedef enum pathfinder_cell_kind (*pathfinder_query_fn)(int32_t x, int32_t y,
														 int32_t z, void* ctx);

// --- Pathfinder: the search itself [740c583: Pathfinder.java] -----------------
struct pathfinder {
	struct path_point pool[PATHFINDER_POOL_CAPACITY];
	int32_t pool_count; // next free pool slot == Beta pointMap's live entry count
	// Open-addressing hash table: slot holds (pool_index + 1), 0 == empty. Sized
	// PATHFINDER_HASH_CAPACITY (4x the pool) for a low collision load factor.
	int32_t hash_table[PATHFINDER_HASH_CAPACITY];
	struct path_heap heap;
};

// Path.clearPath() + MCHash.clearMap() [740c583]: called at the top of every
// search (see header note 4). Also the correct way to initialize a fresh
// struct pathfinder (a zero-initialized one is already equivalent, but calling
// this explicitly documents intent at the call site).
void pathfinder_reset(struct pathfinder* pf);

// Pathfinder.openPoint [740c583]: lookup-or-create by (x,y,z). Returns NULL iff
// the point is new AND the pool is exhausted (overflow policy, header note 1).
struct path_point* pathfinder_open_point(struct pathfinder* pf, int32_t x,
										 int32_t y, int32_t z);

// Per-search perf counters (FH-13: cheap instruments for the hardware round to
// MEASURE Broadway cost before any T2 stagger/budget is even considered -- see
// pathfinding.c for the accumulator wiring and the accessor pair, mirroring the
// server_world_perf_counters/perf_tick_reset pattern in server_world.c).
struct pathfinder_stats {
	int32_t nodes_opened; // distinct PathPoints minted this search (pool_count at
	                      // the moment the search returned) -- Beta's own point-map
	                      // size proxy, the direct "how much did this search cost"
	                      // number the hardware round wants.
};

// Pathfinder.createEntityPathTo(Entity, double, double, double, float) [740c583],
// via World.getPathToEntity's ChunkCache wiring replaced by the query callback (see
// header comment). mob_x/y/z + mob_width/height stand in for the pathing
// entity argument; target_x/y/z + search_range for the target/range arguments. Returns false (no
// path -- Beta's null) and leaves *out_path untouched-but-cleared on failure;
// true with *out_path populated (length >= 1) on success, INCLUDING Beta's
// "couldn't reach the target, here is the path to the closest point we found"
// partial-success case (addToPath's closest-point fallback) -- callers must not assume a
// successful path necessarily ends AT the target.
bool pathfinder_create_path_to_entity(struct pathfinder* pf,
									  pathfinder_query_fn query, void* ctx,
									  float mob_x, float mob_y, float mob_z,
									  float mob_width, float mob_height,
									  float target_x, float target_y,
									  float target_z, float search_range,
									  struct path_entity* out_path,
									  struct pathfinder_stats* out_stats);

// Pathfinder.createEntityPathTo(Entity, int, int, int, float) [740c583]: the
// integer-target overload (World.getEntityPathToXYZ / EntityCreature.
// updateWanderPath's target). Beta centres the integer cell (+0.5F each axis) and
// forwards to the double overload; this does the same then calls the shared core.
bool pathfinder_create_path_to_xyz(struct pathfinder* pf, pathfinder_query_fn query,
								   void* ctx, float mob_x, float mob_y, float mob_z,
								   float mob_width, float mob_height,
								   int32_t target_x, int32_t target_y,
								   int32_t target_z, float search_range,
								   struct path_entity* out_path,
								   struct pathfinder_stats* out_stats);

// --- Perf counters (F3/hardware-round instrument; see struct pathfinder_stats) -
// Mirrors server_world_perf_counters/perf_tick_reset (server_world.h): accumulate
// during the tick, latch+zero once at the tick boundary. Call
// pathfinding_perf_tick_reset() once per server tick (server_local_update, next to
// server_world_perf_tick_reset); the accumulator itself is updated internally by
// pathfinder_create_path_to_entity/xyz on every call (both entry points always run
// a real search -- there is no earlier short-circuit at this layer).
void pathfinding_perf_tick_reset(void);
// searches_last_tick: how many pathfinder_create_path_to_* calls resolved last
// tick (the "searches/tick" counter the FH-13 brief asks for). nodes_opened_last:
// the LAST search's node count this reset window (0 if none ran). nodes_opened_max:
// session-peak nodes_opened over any single search, for spotting the rare
// pathological search before it's a hardware freeze.
void pathfinding_perf_counters(int32_t* searches_last_tick,
							   int32_t* nodes_opened_last, int32_t* nodes_opened_max);

#endif
