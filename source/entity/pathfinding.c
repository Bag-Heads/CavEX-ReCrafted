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

#include <assert.h>
#include <math.h>
#include <stddef.h>

#include "pathfinding.h"

// Forward decl: the perf-counter accumulator lives with the rest of the perf
// plumbing near the bottom of this file, but pathfinder_create_path_to_entity
// (defined earlier) needs to call it on every search.
static void pathfinding_perf_search_done(int32_t nodes_opened);

// ================================================================================
// Path: binary min-heap over distance_to_target [740c583: Path.java]
// ================================================================================

void path_heap_clear(struct path_heap* h) {
	h->count = 0;
}

bool path_heap_is_empty(const struct path_heap* h) {
	return h->count == 0;
}

// Path.sortBack [740c583]: bubble slot `i` UP while its parent has a LARGER (or
// equal, per the `>=` break) distance_to_target. Verbatim structure; only the
// System-array-style indexing becomes plain C pointer-array indexing.
static void path_heap_sort_back(struct path_heap* h, int32_t i) {
	struct path_point* moving = h->items[i];
	float d = moving->distance_to_target;
	while(i > 0) {
		int32_t parent = (i - 1) >> 1;
		struct path_point* p = h->items[parent];
		if(d >= p->distance_to_target)
			break;
		h->items[i] = p;
		p->heap_index = i;
		i = parent;
	}
	h->items[i] = moving;
	moving->heap_index = i;
}

// Path.sortForward [740c583]: sift slot `i` DOWN toward whichever child has the
// smaller key, stopping once neither child beats the current key. Verbatim
// structure (the POSITIVE_INFINITY sentinel for a missing right child becomes a
// plain "right child doesn't exist -> only compare the left" branch in C).
static void path_heap_sort_forward(struct path_heap* h, int32_t i) {
	struct path_point* moving = h->items[i];
	float d = moving->distance_to_target;
	for(;;) {
		int32_t left = 1 + (i << 1);
		int32_t right = left + 1;
		if(left >= h->count)
			break;
		struct path_point* lp = h->items[left];
		float ld = lp->distance_to_target;
		bool has_right = right < h->count;
		struct path_point* rp = has_right ? h->items[right] : NULL;
		float rd = has_right ? rp->distance_to_target : INFINITY;
		if(ld < rd) {
			if(ld >= d)
				break;
			h->items[i] = lp;
			lp->heap_index = i;
			i = left;
		} else {
			if(rd >= d)
				break;
			h->items[i] = rp;
			rp->heap_index = i;
			i = right;
		}
	}
	h->items[i] = moving;
	moving->heap_index = i;
}

bool path_heap_add_point(struct path_heap* h, struct path_point* p) {
	assert(p->heap_index < 0); // Path.addPoint's IllegalStateException guard
	if(h->count >= PATHFINDER_POOL_CAPACITY)
		return false; // overflow policy: see pathfinding.h note 1
	h->items[h->count] = p;
	p->heap_index = h->count;
	path_heap_sort_back(h, h->count);
	h->count++;
	return true;
}

struct path_point* path_heap_dequeue(struct path_heap* h) {
	assert(h->count > 0);
	struct path_point* root = h->items[0];
	h->count--;
	h->items[0] = h->items[h->count];
	h->items[h->count] = NULL;
	if(h->count > 0)
		path_heap_sort_forward(h, 0);
	root->heap_index = -1;
	return root;
}

void path_heap_change_distance(struct path_heap* h, struct path_point* p,
							   float new_distance_to_target) {
	float old = p->distance_to_target;
	p->distance_to_target = new_distance_to_target;
	if(new_distance_to_target < old)
		path_heap_sort_back(h, p->heap_index);
	else
		path_heap_sort_forward(h, p->heap_index);
}

// ================================================================================
// Pathfinder: point map (Beta's MCHash) + the A* search core [740c583:
// Pathfinder.java]
// ================================================================================

void pathfinder_reset(struct pathfinder* pf) {
	pf->pool_count = 0;
	path_heap_clear(&pf->heap);
	for(int32_t i = 0; i < PATHFINDER_HASH_CAPACITY; i++)
		pf->hash_table[i] = 0; // 0 == empty slot (pool indices are stored +1)
}

// Open-addressing linear probe over path_point_hash(x,y,z). Returns the SLOT index
// (0-based into hash_table) holding either this exact point (already present) or
// the first empty slot to insert into.
static int32_t pathfinder_hash_slot(const struct pathfinder* pf, int32_t x,
									int32_t y, int32_t z, int32_t hash) {
	uint32_t slot = (uint32_t)hash % (uint32_t)PATHFINDER_HASH_CAPACITY;
	for(int32_t probes = 0; probes < PATHFINDER_HASH_CAPACITY; probes++) {
		int32_t entry = pf->hash_table[slot];
		if(entry == 0)
			return (int32_t)slot; // empty: insertion point
		const struct path_point* cand = &pf->pool[entry - 1];
		if(cand->hash == hash && cand->x == x && cand->y == y && cand->z == z)
			return (int32_t)slot; // found existing
		slot = (slot + 1) % (uint32_t)PATHFINDER_HASH_CAPACITY;
	}
	return -1; // table somehow full (can't happen: capacity 4x the pool cap, and
	           // the pool caps insertions first) -- defensive only.
}

struct path_point* pathfinder_open_point(struct pathfinder* pf, int32_t x,
										 int32_t y, int32_t z) {
	int32_t hash = path_point_hash(x, y, z);
	int32_t slot = pathfinder_hash_slot(pf, x, y, z, hash);
	if(slot < 0)
		return NULL;
	int32_t entry = pf->hash_table[slot];
	if(entry != 0)
		return &pf->pool[entry - 1]; // Pathfinder.openPoint: lookup hit
	if(pf->pool_count >= PATHFINDER_POOL_CAPACITY)
		return NULL; // overflow policy: see pathfinding.h note 1
	struct path_point* p = &pf->pool[pf->pool_count];
	path_point_init(p, x, y, z);
	pf->hash_table[slot] = pf->pool_count + 1;
	pf->pool_count++;
	return p;
}

// Pathfinder.getVerticalOffset [740c583]: scan the dim_x*dim_y*dim_z box rooted at
// (x,y,z) in Beta's exact nested order (x outer, y middle, z inner) and return on
// the FIRST solid/water/lava cell found -- a water or lava cell anywhere in the box
// short-circuits the WHOLE scan, even if a later cell in the box would have been
// solid. This early-return-on-first-hit shape is load-bearing (it is what makes
// getSafePoint's fall-through-water-but-not-lava distinction possible) and is
// transcribed verbatim, not "cleaned up" into an all-cells scan.
static int32_t pathfinder_get_vertical_offset(pathfinder_query_fn query, void* ctx,
											  int32_t x, int32_t y, int32_t z,
											  int32_t dim_x, int32_t dim_y,
											  int32_t dim_z) {
	for(int32_t cx = x; cx < x + dim_x; cx++) {
		for(int32_t cy = y; cy < y + dim_y; cy++) {
			for(int32_t cz = z; cz < z + dim_z; cz++) {
				switch(query(cx, cy, cz, ctx)) {
				case PATH_CELL_SOLID:
					return 0;
				case PATH_CELL_WATER:
					return -1;
				case PATH_CELL_LAVA:
					return -2;
				case PATH_CELL_PASSABLE:
				default:
					break;
				}
			}
		}
	}
	return 1;
}

// Pathfinder.getSafePoint [740c583]: open (x,y,z) if it's clear, else try stepping
// up by `can_step_up` (0 or 1, from findPathOptions' one-block-step probe); if
// either succeeds, fall THROUGH open space below (up to 3 cells -- the 4th aborts
// the whole candidate) until landing on ground, water, or the box edge. A lava cell
// hit during the fall-through aborts the candidate; a water cell just stops the
// fall (the mob can stand/swim there).
static struct path_point*
pathfinder_get_safe_point(struct pathfinder* pf, pathfinder_query_fn query,
						  void* ctx, int32_t x, int32_t y, int32_t z,
						  int32_t dim_x, int32_t dim_y, int32_t dim_z,
						  int32_t can_step_up) {
	struct path_point* result = NULL;
	if(pathfinder_get_vertical_offset(query, ctx, x, y, z, dim_x, dim_y, dim_z)
	   == 1)
		result = pathfinder_open_point(pf, x, y, z);
	if(!result && can_step_up > 0
	   && pathfinder_get_vertical_offset(query, ctx, x, y + can_step_up, z, dim_x,
										 dim_y, dim_z)
		   == 1) {
		result = pathfinder_open_point(pf, x, y + can_step_up, z);
		y += can_step_up;
	}
	if(result) {
		int32_t fall_count = 0;
		int32_t last_offset = 0;
		while(y > 0
			  && (last_offset = pathfinder_get_vertical_offset(
					  query, ctx, x, y - 1, z, dim_x, dim_y, dim_z))
					 == 1) {
			fall_count++;
			if(fall_count >= 4)
				return NULL;
			y--;
			if(y > 0)
				result = pathfinder_open_point(pf, x, y, z);
		}
		if(last_offset == -2)
			return NULL; // fell into lava at the boundary: reject the candidate
	}
	return result;
}

// Pathfinder.findPathOptions [740c583]: the four cardinal neighbours of `current`
// (south/+Z, west/-X, east/+X, north/-Z -- THIS EXACT ORDER, load-bearing for tie-
// break parity in the heap), each accepted only if reachable (getSafePoint didn't
// reject it), not already closed (`!is_first`), and within `search_range` of the
// TARGET (not the current point -- this is the range-pruning gate that bounds the
// whole search to a ball around the target). Returns the accepted count (0..4);
// see pathfinding.h note 2 for why the buffer is 4, not Beta's oversized 32.
static int32_t pathfinder_find_path_options(struct pathfinder* pf,
											pathfinder_query_fn query, void* ctx,
											struct path_point* current,
											int32_t dim_x, int32_t dim_y,
											int32_t dim_z, struct path_point* target,
											float search_range,
											struct path_point* out_options[4]) {
	int32_t can_step_up = 0;
	if(pathfinder_get_vertical_offset(query, ctx, current->x, current->y + 1,
									  current->z, dim_x, dim_y, dim_z)
	   == 1)
		can_step_up = 1;

	struct path_point* south = pathfinder_get_safe_point(
		pf, query, ctx, current->x, current->y, current->z + 1, dim_x, dim_y,
		dim_z, can_step_up);
	struct path_point* west = pathfinder_get_safe_point(
		pf, query, ctx, current->x - 1, current->y, current->z, dim_x, dim_y,
		dim_z, can_step_up);
	struct path_point* east = pathfinder_get_safe_point(
		pf, query, ctx, current->x + 1, current->y, current->z, dim_x, dim_y,
		dim_z, can_step_up);
	struct path_point* north = pathfinder_get_safe_point(
		pf, query, ctx, current->x, current->y, current->z - 1, dim_x, dim_y,
		dim_z, can_step_up);

	int32_t n = 0;
	if(south && !south->is_first
	   && path_point_distance_to(south, target) < search_range)
		out_options[n++] = south;
	if(west && !west->is_first
	   && path_point_distance_to(west, target) < search_range)
		out_options[n++] = west;
	if(east && !east->is_first
	   && path_point_distance_to(east, target) < search_range)
		out_options[n++] = east;
	if(north && !north->is_first
	   && path_point_distance_to(north, target) < search_range)
		out_options[n++] = north;
	return n;
}

// Pathfinder.createEntityPath [740c583]: reconstruct the waypoint chain by walking
// `previous` back from `end` to `start` (whose previous is always NULL -- it is
// never assigned a predecessor, see pathfinding.h). Fails (returns false, `out`
// untouched-but-cleared) iff the chain is longer than PATH_ENTITY_MAX_WAYPOINTS
// (overflow policy, pathfinding.h note 3).
static bool pathfinder_reconstruct_path(struct path_point* end,
										struct path_entity* out) {
	int32_t chain_len = 1;
	for(struct path_point* p = end; p->previous != NULL; p = p->previous)
		chain_len++;
	if(chain_len > PATH_ENTITY_MAX_WAYPOINTS) {
		path_entity_clear(out);
		return false;
	}
	int32_t idx = chain_len - 1;
	struct path_point* p = end;
	out->points[idx] = (struct path_waypoint) {p->x, p->y, p->z};
	while(p->previous != NULL) {
		p = p->previous;
		idx--;
		out->points[idx] = (struct path_waypoint) {p->x, p->y, p->z};
	}
	out->length = chain_len;
	out->index = 0;
	return true;
}

// Pathfinder.addToPath [740c583]: the A* main loop. `start`/`target` are already-
// opened PathPoints (pool-resident); dim_x/y/z is the entity's footprint box.
// Returns false only when the search never improves on the start node
// (Beta returns no path); otherwise reconstructs a path to the TARGET or
// the closest point discovered (search exhausted the open set without reaching the
// target -- Beta's own "best effort" fallback, not a v2 addition).
static bool pathfinder_add_to_path(struct pathfinder* pf, pathfinder_query_fn query,
								   void* ctx, struct path_point* start,
								   struct path_point* target, int32_t dim_x,
								   int32_t dim_y, int32_t dim_z, float search_range,
								   struct path_entity* out_path,
								   struct pathfinder_stats* out_stats) {
	start->total_path_distance = 0.0f;
	start->distance_to_next = path_point_distance_to(start, target);
	start->distance_to_target = start->distance_to_next;

	path_heap_clear(&pf->heap);
	path_heap_add_point(&pf->heap, start);

	struct path_point* best = start;

	while(!path_heap_is_empty(&pf->heap)) {
		struct path_point* current = path_heap_dequeue(&pf->heap);
		if(path_point_equals(current, target)) {
			if(out_stats)
				out_stats->nodes_opened = pf->pool_count;
			return pathfinder_reconstruct_path(current, out_path);
		}
		if(path_point_distance_to(current, target)
		   < path_point_distance_to(best, target))
			best = current;

		current->is_first = true;
		struct path_point* options[4];
		int32_t n = pathfinder_find_path_options(pf, query, ctx, current, dim_x,
												 dim_y, dim_z, target, search_range,
												 options);

		for(int32_t i = 0; i < n; i++) {
			struct path_point* cand = options[i];
			float new_dist
				= current->total_path_distance + path_point_distance_to(current, cand);
			if(!path_point_is_assigned(cand)
			   || new_dist < cand->total_path_distance) {
				cand->previous = current;
				cand->total_path_distance = new_dist;
				cand->distance_to_next = path_point_distance_to(cand, target);
				if(path_point_is_assigned(cand)) {
					path_heap_change_distance(
						&pf->heap, cand,
						cand->total_path_distance + cand->distance_to_next);
				} else {
					cand->distance_to_target
						= cand->total_path_distance + cand->distance_to_next;
					path_heap_add_point(&pf->heap, cand);
				}
			}
		}
	}

	if(out_stats)
		out_stats->nodes_opened = pf->pool_count;
	if(best == start) {
		path_entity_clear(out_path);
		return false;
	}
	return pathfinder_reconstruct_path(best, out_path);
}

static int32_t pf_floor(float v) {
	return (int32_t)floorf(v); // MathHelper.floor_double/float: a TRUE floor, never
	                          // truncation -- CODESTYLE.md 2.3.
}

bool pathfinder_create_path_to_entity(struct pathfinder* pf,
									  pathfinder_query_fn query, void* ctx,
									  float mob_x, float mob_y, float mob_z,
									  float mob_width, float mob_height,
									  float target_x, float target_y,
									  float target_z, float search_range,
									  struct path_entity* out_path,
									  struct pathfinder_stats* out_stats) {
	pathfinder_reset(pf);
	path_entity_clear(out_path);

	// Entity.boundingBox.minX/Z = pos -+ width/2 (centred footprint); minY = pos
	// (feet-at-origin) -- matches this engine's own e->pos/box_w convention
	// (monsters_object.h: "feet at the entity origin, centered on X/Z"), which is
	// also exactly Beta's model here, so no adjustment beyond the width/2 offset.
	struct path_point* start = pathfinder_open_point(
		pf, pf_floor(mob_x - mob_width / 2.0f), pf_floor(mob_y),
		pf_floor(mob_z - mob_width / 2.0f));
	struct path_point* target = pathfinder_open_point(
		pf, pf_floor(target_x - mob_width / 2.0f), pf_floor(target_y),
		pf_floor(target_z - mob_width / 2.0f));
	if(!start || !target) {
		if(out_stats)
			out_stats->nodes_opened = pf->pool_count;
		pathfinding_perf_search_done(pf->pool_count);
		return false; // pool exhausted opening even the endpoints: fail clean
	}

	int32_t dim_x = pf_floor(mob_width + 1.0f);
	int32_t dim_y = pf_floor(mob_height + 1.0f);
	int32_t dim_z = dim_x;

	bool ok = pathfinder_add_to_path(pf, query, ctx, start, target, dim_x, dim_y,
									 dim_z, search_range, out_path, out_stats);
	// Every call through this entry point runs a real search (no earlier
	// short-circuit below this line) -- count it unconditionally, success or not,
	// for the "searches/tick" instrument (a failed/partial search still spent the
	// CPU the hardware round wants to measure).
	pathfinding_perf_search_done(pf->pool_count);
	return ok;
}

bool pathfinder_create_path_to_xyz(struct pathfinder* pf, pathfinder_query_fn query,
								   void* ctx, float mob_x, float mob_y, float mob_z,
								   float mob_width, float mob_height,
								   int32_t target_x, int32_t target_y,
								   int32_t target_z, float search_range,
								   struct path_entity* out_path,
								   struct pathfinder_stats* out_stats) {
	// Pathfinder.createEntityPathTo(Entity,int,int,int,float) [740c583]: centre the
	// integer cell (+0.5F each axis) and forward to the float/double core.
	return pathfinder_create_path_to_entity(
		pf, query, ctx, mob_x, mob_y, mob_z, mob_width, mob_height,
		(float)target_x + 0.5f, (float)target_y + 0.5f, (float)target_z + 0.5f,
		search_range, out_path, out_stats);
}

// ================================================================================
// Perf counters -- mirrors server_world_perf_counters/perf_tick_reset
// (server_world.c): accumulate during the tick, latch+zero at the tick boundary.
// ================================================================================

static int32_t s_pf_searches_this_tick = 0;
static int32_t s_pf_searches_last_tick = 0;
static int32_t s_pf_nodes_opened_last = 0;
static int32_t s_pf_nodes_opened_session_max = 0;

static void pathfinding_perf_search_done(int32_t nodes_opened) {
	s_pf_searches_this_tick++;
	s_pf_nodes_opened_last = nodes_opened;
	if(nodes_opened > s_pf_nodes_opened_session_max)
		s_pf_nodes_opened_session_max = nodes_opened;
}

void pathfinding_perf_tick_reset(void) {
	s_pf_searches_last_tick = s_pf_searches_this_tick;
	s_pf_searches_this_tick = 0;
}

void pathfinding_perf_counters(int32_t* searches_last_tick,
							   int32_t* nodes_opened_last,
							   int32_t* nodes_opened_max) {
	if(searches_last_tick)
		*searches_last_tick = s_pf_searches_last_tick;
	if(nodes_opened_last)
		*nodes_opened_last = s_pf_nodes_opened_last;
	if(nodes_opened_max)
		*nodes_opened_max = s_pf_nodes_opened_session_max;
}
