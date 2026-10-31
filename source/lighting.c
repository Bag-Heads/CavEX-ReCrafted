/*
	Copyright (c) 2023 ByteBit/xtreme8000

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
#include <stdlib.h>
#include <string.h>

#include "lighting.h"

#ifdef CAVEWII_SERVER_LOCAL_TEST
bool (*lighting_test_history_hook)(const struct lighting_history_event*);
static bool s_history_failed;
void lighting_test_history_reset(void) { s_history_failed = false; }
bool lighting_test_history_failed(void) { return s_history_failed; }
void lighting_test_history_emit(struct lighting_history_event event) {
	if(lighting_test_history_hook && !lighting_test_history_hook(&event))
		s_history_failed = true;
}
#endif

void lighting_heightmap_update(uint8_t* heightmap, c_coord_t x, w_coord_t y,
							   c_coord_t z, uint8_t type,
							   bool (*get_block)(void* user, c_coord_t x,
												 w_coord_t y, c_coord_t z,
												 struct block_data* blk),
							   void* user) {
	assert(heightmap && get_block);

	uint8_t* height = heightmap + x + z * CHUNK_SIZE;

	if(blocks[type]
	   && (!blocks[type]->can_see_through || blocks[type]->opacity > 0)) {
		if(y >= *height)
			G9B_BYTE(heightmap, x + z * CHUNK_SIZE, y + 1);
	} else if(y < *height) {
		while(*height > 0) {
			struct block_data blk;

			if(get_block(user, x, *height - 1, z, &blk) && blocks[blk.type]
			   && (!blocks[blk.type]->can_see_through
				   || blocks[blk.type]->opacity > 0))
				break;

			G9B_BYTE(heightmap, x + z * CHUNK_SIZE, *height - 1);
		}
	}
}

struct lighting_update_entry {
	w_coord_t x, y, z;
	// A seed cell is re-set + re-propagated even when its recomputed light is
	// unchanged (matching the old "source == current" special case). Propagated
	// neighbours are not forced -- they only continue the flood when they change.
	bool forced;
};

#ifdef CAVEWII_SERVER_LOCAL_TEST
static void lighting_history_push(struct stack* q,
	const struct lighting_update_entry* e) {
	G9B_QUEUE("push", q, e->x, e->y, e->z, e->forced,
		stack_size(q), stack_size(q) + 1);
	stack_push(q, (void*)e);
}
static void lighting_history_pop(struct stack* q, struct lighting_update_entry* e) {
	stack_pop(q, e);
	G9B_QUEUE("pop", q, e->x, e->y, e->z, e->forced,
		stack_size(q) + 1, stack_size(q));
}
#define LIGHT_HISTORY_PUSH lighting_history_push
#define LIGHT_HISTORY_POP lighting_history_pop
#else
#define LIGHT_HISTORY_PUSH stack_push
#define LIGHT_HISTORY_POP stack_pop
#endif

static inline int8_t MAX_I8(int8_t a, int8_t b) {
	return a > b ? a : b;
}

// I07 lane 3 (T1): blocks_side_offset as a table. Designated by the enum
// value, so the mapping cannot drift from blocks.c's switch whatever the
// enum order; tests/bench_g9_drain.c checks the six entries against
// blocks_side_offset at start-up. Same offsets in the same side order.
static const int8_t LIGHT_SIDE_OFF[SIDE_MAX][3] = {
	[SIDE_TOP] = {0, 1, 0},	   [SIDE_BOTTOM] = {0, -1, 0},
	[SIDE_LEFT] = {-1, 0, 0},  [SIDE_RIGHT] = {1, 0, 0},
	[SIDE_FRONT] = {0, 0, -1}, [SIDE_BACK] = {0, 0, 1},
};

// A2 pacing hook: the server installs thread_yield() here (lighting_set_yield) so a
// big relight BFS (a worldgen lava lake = thousands of queue pops, hundreds of ms)
// periodically hands the single Broadway core back to the render thread instead of
// freezing it. NULL by default, so tests + the client thread flood with no yield --
// which keeps the flood byte-identical (a yield only changes WHEN cells are visited,
// never which or to what value: the light field is an order-independent fixed point,
// proven by tests/test_lighting_multi.c). On PC thread_yield is itself a no-op.
static void (*s_flood_yield)(void) = NULL;
void lighting_set_yield(void (*y)(void)) {
	s_flood_yield = y;
}
// Yield once per this many queue pops. 256 keeps the render-thread gap small on a
// lava-heavy relight while the yield overhead stays negligible.
#define LIGHTING_FLOOD_YIELD_EVERY 256

// Work ceiling (defense-in-depth, M1 far-ring crash). A legitimate relight is
// thousands of pops (a worldgen lava lake), up to ~100k for a full-chunk relight.
// A flood that blows past 500k is PATHOLOGICAL -- a stored-vs-recomputed light
// inconsistency seeding an annulus-spanning skylight cascade (the synthetic far
// sections used to do this; world_build_far_section now stores self-consistent light
// so it converges at the seam). Rather than churn the queue to a realloc-OOM (the old
// memcpy-through-NULL 0x00800000 DSI) or freeze the core for seconds, abort + count.
// Should never trip in healthy operation; non-zero flags a relight-seam regression.
#define LIGHTING_FLOOD_MAX_POPS 500000u
static uint32_t s_flood_ceiling_hits = 0;
uint32_t lighting_flood_ceiling_hits(void) {
	return s_flood_ceiling_hits;
}

/* LIGHT-CLIENT-LOSSLESS: the client (render-thread) relight's own counters.
 * A park is a STOP with the frontier intact that the caller (world.c) turns
 * into a column repair; nothing is discarded. Kept apart from the counters
 * above (server floods / process-wide stack drops) so a client fault is
 * never reported as a server-queue one. */
static uint32_t s_client_ceiling_parks = 0; // per-flood 500k pop ceiling reached
static uint32_t s_client_oom_parks = 0;	 // a client frontier grow failed
uint32_t lighting_client_ceiling_parks(void) { return s_client_ceiling_parks; }
uint32_t lighting_client_oom_parks(void) { return s_client_oom_parks; }
void lighting_client_note_oom_park(void) { s_client_oom_parks++; }

/* LIGHT-CLIENT-LOSSLESS root cause: the caller's "does this cell touch a
 * column that is not resident?" test (world.c). At such a cell the client
 * relight may RAISE light but never LOWER it: the missing column may be what
 * supports the stored value, so lowering it starts a darkening cascade along
 * the whole residency / near-far seam. NULL (pure units) = off. */
static bool (*s_client_seam_hook)(void*, w_coord_t, w_coord_t, w_coord_t);
void lighting_client_set_seam_hook(
	bool (*hook)(void*, w_coord_t, w_coord_t, w_coord_t)) {
	s_client_seam_hook = hook;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static long s_client_reserve_fail_after = -1;
void lighting_test_client_fail_reserve_after(long n) {
	s_client_reserve_fail_after = n;
}
#endif
/* Client frontier grow, reported instead of dropped. No allocation hook:
 * the client flood runs on MAIN, where stack_push used to realloc too. The
 * test hook fails the n-th reserve that has to grow (separate from the
 * server drain's hook so server fixtures keep their own count). */
bool lighting_client_reserve(struct stack* q, size_t need) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(need > q->length || q->data == NULL) {
		if(s_client_reserve_fail_after == 0)
			return false;
		if(s_client_reserve_fail_after > 0)
			s_client_reserve_fail_after--;
	}
#endif
	return stack_reserve(q, need);
}

// Shared block/sky-light propagation flood over a pre-seeded queue: pop a cell,
// recompute its light from its neighbours (+ its own luminance / skylight), and
// push its neighbours when it changes. Seeding with N source cells and running
// this ONCE reaches the same fixed point as N separate single-source floods
// (the light field is order-independent), but at O(cells) instead of
// O(sources x cells) -- see lighting_update_at_blocks.
#ifdef CAVEWII_SERVER_LOCAL_TEST
static long s_drain_reserve_fail_after = -1;
void lighting_test_drain_fail_reserve_after(long n) {
	s_drain_reserve_fail_after = n;
}
#endif

/* G9 lossless reserve: room for one pop's up-to-6 pushes (net +5) BEFORE the
 * pop, so a failed grow leaves the frontier intact and loses nothing. The test
 * hook fails the n-th reserve that would have to grow the queue. */
static void (*s_drain_allocation)(bool before);
void lighting_set_drain_allocation(void (*hook)(bool before)) { s_drain_allocation = hook; }
bool lighting_drain_reserve(struct stack* q, size_t need) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(need > q->length || q->data == NULL) {
		if(s_drain_reserve_fail_after == 0)
			return false;
		if(s_drain_reserve_fail_after > 0)
			s_drain_reserve_fail_after--;
	}
#endif
	/* realloc/libc may take a lock not routed through thread_mutex. */
	bool growing = need > q->length || q->data == NULL;
	if(growing && s_drain_allocation) s_drain_allocation(true);
	bool ok = stack_reserve(q, need);
	if(growing && s_drain_allocation) s_drain_allocation(false);
	return ok;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*lighting_test_drain_yield_hook)(void);
#endif
void lighting_drain_yield(void) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(lighting_test_drain_yield_hook) lighting_test_drain_yield_hook();
#endif
	if(s_flood_yield) s_flood_yield();
}
void lighting_drain_ceiling_hit(void) { s_flood_ceiling_hits++; }

static size_t lighting_flood(
	struct stack* queue, bool ignore_sky_light, size_t max_pops,
	size_t ceiling, enum lighting_drain_result* status,
	struct lighting_drain_stats* st,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	bool (*get_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t* sky, uint8_t* torch),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user, bool preserve_ignored_sky, bool client) {
	size_t pops = 0;
	if(status)
		*status = LIGHTING_DRAIN_DONE;
	while(!stack_empty(queue)) {
		// Per-call WORK BUDGET (max_pops > 0): suspend the flood with its BFS frontier
		// left INTACT in the queue, so the caller can resume it next frame. This is the
		// client/main-thread relight's freeze fix -- world_update_lighting runs the flood
		// on the render thread, which (unlike the server's yield) cannot advance the
		// main-loop watchdog heartbeat mid-flood, so an unbudgeted multi-second surface
		// skylight flood froze the core (watchdog row1=WDM_LIGHTING). Pacing it is
		// BYTE-EXACT: the queue carries the exact frontier across frames, so the same
		// cells are popped in the same order, reaching the same fixed point -- only the
		// settle RATE changes (proven in tests/test_lighting_client_budget_equiv.c).
		// max_pops == 0 (server + tests) = unbudgeted, byte-identical to before.
		if(max_pops && pops >= max_pops)
			break;
		// Work ceiling: bail on a pathological flood instead of freezing the core /
		// OOM-churning the queue. pops is counted UNCONDITIONALLY here (the old
		// `++pops` lived inside the yield `&&`, so it never counted on the client
		// thread where s_flood_yield is NULL -- exactly the thread that ran away).
		if(++pops > ceiling) {
			/* LIGHT-CLIENT-LOSSLESS: a client park is counted apart from the
			 * server/process ceiling, so telemetry never blames the server. */
			if(client)
				s_client_ceiling_parks++;
			else
				s_flood_ceiling_hits++;
			if(status) { /* lossless: frontier intact, caller stops */
				*status = LIGHTING_DRAIN_CEILING;
				pops--; /* nothing was popped */
			}
			break;
		}
		if(status && !(client ? lighting_client_reserve(queue, stack_size(queue) + 5)
						  : lighting_drain_reserve(queue, stack_size(queue) + 5))) {
			*status = LIGHTING_DRAIN_OOM; /* frontier intact, nothing dropped */
			pops--;
			break;
		}

		// I07 drain telemetry: the frontier peak, read before the pop (one
		// compare per pop; st is NULL on the client / legacy paths).
		if(st && stack_size(queue) > st->depth_peak)
			st->depth_peak = (uint32_t)stack_size(queue);

		struct lighting_update_entry current;
		LIGHT_HISTORY_POP(queue, &current);

		// A2: hand the core back to render every LIGHTING_FLOOD_YIELD_EVERY pops so a
		// long flood doesn't freeze the frame. Pure pacing -- the queue state is
		// untouched across the yield, so the fixed point is identical.
		if(s_flood_yield && (pops % LIGHTING_FLOOD_YIELD_EVERY) == 0)
			s_flood_yield();

		uint8_t column_height;
		struct block_data old;
		bool old_exists = get_block(user, current.x, current.y, current.z, &old,
									&column_height);
		// A queued cell can be UNLOADED before it is popped when the flood is paced
		// across frames (the client budget): death->respawn evicts the death area, and
		// walking evicts far chunks, between the push and the resume. get_block then
		// returns false WITHOUT writing `old`, so the old assert(old_exists) (a no-op
		// under NDEBUG) let the flood run on garbage -> world_light_set_light deref'd a
		// NULL chunk -> DSI crash. Skip the evicted cell: its column is gone, and a
		// future re-install arrives with final server light (same rule as the deferred-
		// entry stale-column drop in world_update_lighting). For the SYNCHRONOUS server +
		// test floods nothing evicts mid-flood, so old_exists is always true here and
		// this is byte-identical (the branch is never taken).
		if(!old_exists) {
			G9B_QUEUE("skip-absent", queue, current.x, current.y, current.z,
				current.forced, stack_size(queue), stack_size(queue));
			continue;
		}

		uint8_t old_light = (old.torch_light << 4) | old.sky_light;
		/* Beta World.java:1300-1301: no Sky queue in Nether. Preserve the
		 * saved nibble; plain Chunk.setBlockID may have written it. */
		uint8_t new_light_sky = ignore_sky_light && preserve_ignored_sky
			? old.sky_light : 0;
		uint8_t new_light_torch = 0;

		if(!ignore_sky_light && current.y >= column_height)
			new_light_sky = 0xF;

		if(blocks[old.type])
			new_light_torch = blocks[old.type]->luminance;

		// I07 lane 3 (T1): the neighbour-existence answers of the read loop
		// are kept for the push loop below. Nothing between the two loops
		// can change them (set_light writes light into a resident chunk; no
		// chunk is loaded or evicted inside a pop), so the push loop asks
		// get_block again only when the read loop did not run (an opaque
		// cell). Same pushes in the same order; six fewer resolves per
		// written see-through cell.
		bool neighbour_known = false;
		uint8_t neighbour_exists = 0;

		if(!blocks[old.type] || blocks[old.type]->can_see_through) {
			neighbour_known = true;
			const int8_t opacity = blocks[old.type] ?
				MAX_I8(blocks[old.type]->opacity, 1) :
				1;
			for(enum side s = 0; s < SIDE_MAX; s++) {
				const int x = LIGHT_SIDE_OFF[s][0], y = LIGHT_SIDE_OFF[s][1],
						  z = LIGHT_SIDE_OFF[s][2];

				uint8_t other_sky, other_torch;
				bool other_exists;
				if(get_light) {
					other_exists = get_light(user, current.x + x, current.y + y,
											 current.z + z, &other_sky,
											 &other_torch);
				} else {
					struct block_data other;
					other_exists = get_block(user, current.x + x, current.y + y,
											 current.z + z, &other, NULL);
					other_sky = other.sky_light;
					other_torch = other.torch_light;
				}

				if(other_exists) {
					neighbour_exists |= (uint8_t)(1u << s);

					if(!ignore_sky_light)
						new_light_sky = MAX_I8(
							MAX_I8((int8_t)other_sky - opacity, 0),
							new_light_sky);
					new_light_torch
						= MAX_I8(MAX_I8((int8_t)other_torch - opacity, 0),
								 new_light_torch);
				}
			}
		}

		if(client && s_client_seam_hook
		   && s_client_seam_hook(user, current.x, current.y, current.z)) {
			new_light_sky = MAX_I8(new_light_sky, old.sky_light);
			new_light_torch = MAX_I8(new_light_torch, old.torch_light);
		}
		uint8_t new_light = (new_light_torch << 4) | new_light_sky;
		if(old_light == new_light && !current.forced)
			G9B_QUEUE("noop", queue, current.x, current.y, current.z,
				false, stack_size(queue), stack_size(queue));

		if(old_light != new_light || current.forced) {
			set_light(user, current.x, current.y, current.z, new_light);
			if(st)
				st->writes++;

			for(enum side s = 0; s < SIDE_MAX; s++) {
				const int x = LIGHT_SIDE_OFF[s][0], y = LIGHT_SIDE_OFF[s][1],
						  z = LIGHT_SIDE_OFF[s][2];

				const bool exists = neighbour_known ?
					(neighbour_exists & (1u << s)) != 0 :
					get_block(user, current.x + x, current.y + y,
							  current.z + z, NULL, NULL);
				if(exists) {
					if(st)
						st->pushes++;
					LIGHT_HISTORY_PUSH(queue,
							   &(struct lighting_update_entry) {
								   .x = current.x + x,
								   .y = current.y + y,
								   .z = current.z + z,
								   .forced = false,
							   });
				}
			}
		}
	}
	return pops;
}

void lighting_update_at_block(
	struct world_modification_entry source, bool ignore_sky_light,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user) {
	assert(get_block && set_light);

	struct stack queue;
	stack_create(&queue, 128, sizeof(struct lighting_update_entry));
	G9B_QUEUE("queue-init", &queue, 0, 0, 0, false, 0, 0);
	LIGHT_HISTORY_PUSH(&queue, &(struct lighting_update_entry) {
						   .x = source.x,
						   .y = source.y,
						   .z = source.z,
						   .forced = true,
					   });
	lighting_flood(&queue, ignore_sky_light, 0 /*unbudgeted*/,
				   LIGHTING_FLOOD_MAX_POPS, NULL, NULL, get_block, NULL, set_light,
				   user, false, false);
	stack_destroy(&queue);
}

// Multi-source variant: recompute the light field from MANY source cells in a
// single flood. Equivalent to calling lighting_update_at_block once per source
// (proven byte-for-byte in tests/test_lighting_multi.c) but vastly cheaper when
// the sources cluster -- e.g. relighting a worldgen lava lake, where the
// per-block flood was O(lava x area) and froze gen on lava-heavy chunks.
static void lighting_update_at_blocks_impl(
	const struct world_modification_entry* sources, size_t n,
	bool ignore_sky_light,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user, bool preserve_ignored_sky) {
	assert(get_block && set_light);
	if(n == 0)
		return;

	struct stack queue;
	stack_create(&queue, n > 128 ? n : 128,
				 sizeof(struct lighting_update_entry));
	G9B_QUEUE("queue-init", &queue, 0, 0, 0, false, 0, 0);
	for(size_t i = 0; i < n; i++)
		LIGHT_HISTORY_PUSH(&queue, &(struct lighting_update_entry) {
							   .x = sources[i].x,
							   .y = sources[i].y,
							   .z = sources[i].z,
							   .forced = true,
						   });
	lighting_flood(&queue, ignore_sky_light, 0 /*unbudgeted*/,
				   LIGHTING_FLOOD_MAX_POPS, NULL, NULL, get_block, NULL, set_light,
				   user, preserve_ignored_sky, false);
	stack_destroy(&queue);
}

void lighting_update_at_blocks(
	const struct world_modification_entry* sources, size_t n,
	bool ignore_sky_light,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	void* user) {
	lighting_update_at_blocks_impl(sources, n, ignore_sky_light,
		get_block, set_light, user, false);
}

/* World.java:1300-1301: server Block work never rewrites Nether Sky.
 * Keep the legacy client/remote ingress behavior on the public path above. */
void lighting_update_at_blocks_server(
	const struct world_modification_entry* sources, size_t n,
	bool ignore_sky_light,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	void* user) {
	lighting_update_at_blocks_impl(sources, n, ignore_sky_light,
		get_block, set_light, user, true);
}

// --- Resumable, budgeted client/main-thread relight (the far-ring freeze fix) ---
//
// The render thread runs world_update_lighting every frame but, unlike the server's
// yield, it cannot advance the main-loop watchdog heartbeat mid-flood -- so a single
// multi-second surface skylight flood froze the core (watchdog row1 = WDM_LIGHTING).
// These let world.c carry ONE flood's BFS frontier across frames in a persistent queue
// and spend only a bounded pop budget per frame. Byte-exact: the frontier is preserved
// verbatim across the suspend, so the same cells pop in the same order to the same fixed
// point -- only the settle rate is paced (tests/test_lighting_client_budget_equiv.c).
static uint32_t s_client_flood_pops = 0; // accumulated pops for the in-progress flood
static uint32_t s_client_flood_max = 0;  // largest single flood seen (across resumes)

uint32_t lighting_client_flood_max_pops(void) {
	return s_client_flood_max;
}

void lighting_flood_queue_create(struct stack* q) {
	assert(q);
	stack_create(q, 128, sizeof(struct lighting_update_entry));
	G9B_QUEUE("queue-init", q, 0, 0, 0, false, 0, 0);
}

void lighting_flood_queue_destroy(struct stack* q) {
	assert(q);
	stack_destroy(q);
}

bool lighting_flood_queue_empty(const struct stack* q) {
	assert(q);
	return stack_empty((struct stack*)q);
}

// Abandon an in-flight flood (world reset / unload): empty the frontier AND zero the
// per-flood pop accumulator. Routing the reset through here (instead of a raw stack_clear
// in world.c) keeps s_client_flood_pops from carrying a stale value past a death/respawn
// or dimension change -- which would otherwise muddy the cflood F3 reading the freeze
// diagnosis is confirmed by, and (latently) could trip the 500k abandon on the next flood
// if a future caller ever resumed without seeding first.
void lighting_flood_queue_clear(struct stack* q) {
	assert(q);
	struct lighting_update_entry drop;
	while(!stack_empty(q))
		LIGHT_HISTORY_POP(q, &drop);
	s_client_flood_pops = 0;
}

bool lighting_flood_seed(struct stack* q, w_coord_t x, w_coord_t y, w_coord_t z,
						 bool forced) {
	assert(q);
	// A NEW flood starts here: reset the per-flood pop accumulator so the max counter
	// measures one flood, not the running total.
	s_client_flood_pops = 0;
	/* LIGHT-CLIENT-LOSSLESS: reserve before the push, so a failed grow is
	 * REPORTED (false, nothing pushed) and the caller repairs the column,
	 * instead of stack_push dropping the seed. */
	if(!lighting_client_reserve(q, stack_size(q) + 1))
		return false;
	LIGHT_HISTORY_PUSH(q, &(struct lighting_update_entry) {
					  .x = x, .y = y, .z = z, .forced = forced,
				  });
	return true;
}

// Process up to max_pops cells of the pre-seeded queue; returns the number of cells
// POPPED this call (so the caller can spend a per-frame budget across several floods).
// When the queue drains the flood is done; check lighting_flood_queue_empty().
//
// LIGHT-CLIENT-LOSSLESS (hw1023a Finding 2): this used to ABANDON the frontier when
// one flood passed 500k pops, and its pushes went through stack_push, which DROPS a
// push whose grow fails. Both lost light work silently. Now the flood uses the G9
// contract: room for a pop's pushes is reserved BEFORE the pop, and both the 500k
// per-flood ceiling and a failed reserve STOP the flood with the frontier intact
// (lighting_flood_last_status() says which). The caller decides what to do with the
// parked frontier (world.c spills it into column repairs); a caller that ignores a
// CEILING simply continues the same flood, because the accumulator restarts. On the
// success path the pops, order and values are unchanged (reserve never reorders;
// tests/test_lighting_client_budget_equiv.c).
static enum lighting_drain_result s_client_last_status = LIGHTING_DRAIN_DONE;
enum lighting_drain_result lighting_flood_last_status(void) {
	return s_client_last_status;
}

size_t lighting_flood_resume(
	struct stack* q, bool ignore_sky_light, size_t max_pops,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user) {
	return lighting_flood_resume_light(q, ignore_sky_light, max_pops,
		get_block, NULL, set_light, user);
}

size_t lighting_flood_resume_light(
	struct stack* q, bool ignore_sky_light, size_t max_pops,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user) {
	assert(q && get_block && set_light);
	// Pops left before this flood reaches the per-flood ceiling (0 = park now).
	size_t room = s_client_flood_pops < LIGHTING_FLOOD_MAX_POPS
		? LIGHTING_FLOOD_MAX_POPS - s_client_flood_pops : 0;
	enum lighting_drain_result st;
	size_t did = lighting_flood(q, ignore_sky_light, max_pops, room, &st, NULL,
								get_block, get_light, set_light, user, false, true);
	s_client_flood_pops += (uint32_t)did;
	s_client_last_status = st;
	if(st == LIGHTING_DRAIN_OOM)
		s_client_oom_parks++;

	// Flood complete, or parked at the ceiling: finalize the max counter. A parked
	// flood's frontier is still in q; the accumulator restarts for what follows.
	if(st == LIGHTING_DRAIN_CEILING || stack_empty(q)) {
		if(s_client_flood_pops > s_client_flood_max)
			s_client_flood_max = s_client_flood_pops;
		s_client_flood_pops = 0;
	}
	return did;
}

/* LIGHT-CLIENT-LOSSLESS spill: hand up to max_entries parked frontier cells'
 * columns to `mark` (consecutive duplicates folded: a flood frontier is
 * spatially clustered). A FORCED entry (an edit seed) also marks the columns
 * of its side neighbours: the forced pop pushes them whether or not the
 * seed's own light changes, so a seed whose cell already holds its new value
 * (stone sealing a lit cross-column tunnel) must still reach the column
 * across the edge. Incremental (hw1023a fix-r2): the caller charges every
 * spilled entry to its per-frame budget and resumes next frame, so a large
 * frontier is never drained synchronously. Once the frontier is empty its
 * storage is FREED so a low-memory heap gets it back. Lossless because the
 * flood invariant (every cell whose stored light may disagree with its
 * neighbours is in the frontier) becomes "is in the frontier or in a marked
 * column", which the caller's column repair re-relaxes to the fixed point.
 * Returns the entries spilled this call. */
size_t lighting_flood_queue_spill(struct stack* q,
	void (*mark)(void* user, w_coord_t cx, w_coord_t cz), void* user,
	size_t max_entries) {
	assert(q && mark);
	size_t n = 0;
	bool have = false;
	w_coord_t lcx = 0, lcz = 0;
	struct lighting_update_entry e;
	while(n < max_entries && !stack_empty(q)) {
		LIGHT_HISTORY_POP(q, &e);
		n++;
		w_coord_t cx = WCOORD_CHUNK_OFFSET(e.x), cz = WCOORD_CHUNK_OFFSET(e.z);
		if(e.forced) {
			for(enum side s = 0; s < SIDE_MAX; s++) {
				w_coord_t ncx = WCOORD_CHUNK_OFFSET(e.x + LIGHT_SIDE_OFF[s][0]),
						  ncz = WCOORD_CHUNK_OFFSET(e.z + LIGHT_SIDE_OFF[s][2]);
				if(ncx != cx || ncz != cz)
					mark(user, ncx, ncz);
			}
		}
		if(have && cx == lcx && cz == lcz)
			continue;
		have = true, lcx = cx, lcz = cz;
		mark(user, cx, cz);
	}
	if(stack_empty(q)) {
		s_client_flood_pops = 0;
		if(!q->fixed && q->data) {
			free(q->data);
			q->data = NULL;
			q->length = 0;
		}
	}
	return n;
}

/* One relaxation of the client flood's light equation at (x,y,z), with no
 * queue: the lighting_flood body for preserve_ignored_sky == false (the
 * client form), minus the pushes. Returns 1 when the light changed (and was
 * written), 0 when it already held the fixed-point value, -1 when the cell
 * does not exist (get_block false). Used by world.c's bounded column repair. */
int lighting_relax_cell(bool ignore_sky_light, w_coord_t x, w_coord_t y,
	w_coord_t z,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	void* user) {
	uint8_t column_height;
	struct block_data old;
	if(!get_block(user, x, y, z, &old, &column_height))
		return -1;
	uint8_t new_light_sky = 0, new_light_torch = 0;
	if(!ignore_sky_light && y >= column_height)
		new_light_sky = 0xF;
	if(blocks[old.type])
		new_light_torch = blocks[old.type]->luminance;
	if(!blocks[old.type] || blocks[old.type]->can_see_through) {
		const int8_t opacity = blocks[old.type] ?
			MAX_I8(blocks[old.type]->opacity, 1) : 1;
		for(enum side s = 0; s < SIDE_MAX; s++) {
			w_coord_t nx = x + LIGHT_SIDE_OFF[s][0], ny = y + LIGHT_SIDE_OFF[s][1],
					  nz = z + LIGHT_SIDE_OFF[s][2];
			uint8_t other_sky, other_torch;
			bool other_exists;
			if(get_light) {
				other_exists = get_light(user, nx, ny, nz, &other_sky, &other_torch);
			} else {
				struct block_data other;
				other_exists = get_block(user, nx, ny, nz, &other, NULL);
				other_sky = other.sky_light;
				other_torch = other.torch_light;
			}
			if(!other_exists)
				continue;
			if(!ignore_sky_light)
				new_light_sky = MAX_I8(MAX_I8((int8_t)other_sky - opacity, 0),
									   new_light_sky);
			new_light_torch = MAX_I8(MAX_I8((int8_t)other_torch - opacity, 0),
									 new_light_torch);
		}
	}
	if(s_client_seam_hook && s_client_seam_hook(user, x, y, z)) { // as the flood
		new_light_sky = MAX_I8(new_light_sky, old.sky_light);
		new_light_torch = MAX_I8(new_light_torch, old.torch_light);
	}
	uint8_t old_light = (old.torch_light << 4) | old.sky_light;
	uint8_t new_light = (new_light_torch << 4) | new_light_sky;
	if(old_light == new_light)
		return 0;
	set_light(user, x, y, z, new_light);
	return 1;
}

// --- G9 server light drain: lossless (ASTRA-G9-REVIEW Required 2) ---
//
// The server pending-light drain (server_world_light_drain) owns its frontier
// queue. Unlike lighting_update_at_blocks it never loses work: seeding reserves
// all n entries first (all or nothing), and every pop first reserves room for
// that pop's pushes, so an allocation failure stops the flood with the frontier
// intact (LIGHTING_DRAIN_OOM). The pop ceiling likewise stops with the frontier
// intact (LIGHTING_DRAIN_CEILING). Either way the caller must not let any
// consumer, save or publication see the field; it stops the world explicitly.
// On success the pops, order and values are those of lighting_update_at_blocks
// over the same sources (same LIFO stack, same forced seeds, same flood body).
void lighting_drain_queue_init(struct stack* q) {
	assert(q);
	q->length = 0;
	q->index = 0;
	q->element_size = sizeof(struct lighting_update_entry);
	q->data = NULL;
	q->fixed = false;
	G9B_QUEUE("queue-init", q, 0, 0, 0, false, 0, 0);
}

size_t lighting_drain_queue_bytes(const struct stack* q) {
	assert(q);
	return q->data ? q->length * q->element_size : 0;
}

bool lighting_drain_seed(struct stack* q,
						 const struct world_modification_entry* sources,
						 size_t n) {
	assert(q && (sources || n == 0));
	if(!lighting_drain_reserve(q, stack_size(q) + n)) {
		for(size_t i = 0; i < n; i++)
			G9B_QUEUE("seed-reject", q, sources[i].x, sources[i].y,
				sources[i].z, true, stack_size(q), stack_size(q));
		return false;
	}
	for(size_t i = 0; i < n; i++) {
		LIGHT_HISTORY_PUSH(q, &(struct lighting_update_entry) {
						  .x = sources[i].x,
						  .y = sources[i].y,
						  .z = sources[i].z,
						  .forced = true,
						  });
	}
	return true;
}

enum lighting_drain_result lighting_drain_flood(
	struct stack* q, bool ignore_sky_light, size_t ceiling, size_t* pops,
	struct lighting_drain_stats* st,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	bool (*get_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t* sky, uint8_t* torch),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user) {
	assert(q && get_block && set_light);
	enum lighting_drain_result r = LIGHTING_DRAIN_DONE;
	if(st)
		*st = (struct lighting_drain_stats) {0};
	size_t did = lighting_flood(q, ignore_sky_light, 0, ceiling, &r, st,
								get_block, get_light, set_light, user, true, false);
	if(pops)
		*pops = did;
	return r;
}

/* REL-MP-FIX3c, REMOTE SESSIONS ONLY (nothing above calls this, so local
 * floods, their order and goldens are untouched). One pending BIT per cell per
 * column slot: set or not, so no frontier overflows and nothing is dropped.
 * ONE caller block: header | rr_slot[n] | u16 active[n] | u64 bits[n][512];
 * no allocation after init. LEDGER: 4096 B bitmap + 16 B rr_slot + 2 B active
 * = 4114 B/slot, + 16 B header + <= 6 B pad. Slots = REMOTE_RESIDENT_COLUMN_CAP
 * (2R+3)^2: Wii R=5 169 slots = 695,288 B; R=4 121 = 497,816 B. Budget: 1 per
 * relaxed cell (expansion included) and 1 per empty 64-bit word scanned.
 * Cells in columns the caller reports unloaded are never marked: Beta lights
 * only where chunks exist (MetadataChunkBlock.java:43 doChunksNearChunkExist,
 * World.java:319-329 blockExists 319, doChunksNearChunkExist 323,
 * checkChunksExist 327; World.java:1689 blockExists before a lighting
 * region is queued). So pending columns <= loaded columns <= slots, provided
 * the caller invalidates a column when it unloads. */
#define RR_WORDS (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 64)
#define RR_MAGIC 0x52524C54u
#define RR_FREE 0xFFFFu
struct rr_slot {
	int32_t cx, cz;
	uint32_t pending; // set bits in this slot; 0 <=> slot free
	uint16_t cursor, active_pos; // retained scan word; index in active[]
};
struct lighting_remote_relight { uint32_t magic, slots, n_active, rr; };
static size_t rr_bits_off(size_t n) {
	return (sizeof(struct lighting_remote_relight) + n * 18 + 7) & ~(size_t)7;
}
#define RR_OK(r) ((r) && (r)->magic == RR_MAGIC)
#define RR_META(r) ((struct rr_slot*)((r) + 1))
#define RR_ACTIVE(r) ((uint16_t*)(RR_META(r) + (r)->slots))
#define RR_BITS(r, s) \
	((uint64_t*)((char*)(r) + rr_bits_off((r)->slots)) + (size_t)(s) * RR_WORDS)

size_t lighting_remote_relight_bytes(size_t slots) {
	_Static_assert(sizeof(struct rr_slot) + sizeof(uint16_t) == 18, "ledger");
	return slots && slots < RR_FREE ? rr_bits_off(slots) + slots * RR_WORDS * 8 : 0;
}

struct lighting_remote_relight* lighting_remote_relight_init(void* storage,
	size_t bytes, size_t slots) {
	size_t need = lighting_remote_relight_bytes(slots);
	if(!storage || !need || bytes < need || ((uintptr_t)storage & 7))
		return NULL;
	struct lighting_remote_relight* r = storage;
	*r = (struct lighting_remote_relight) {RR_MAGIC, (uint32_t)slots, 0, 0};
	for(size_t i = 0; i < slots; i++)
		RR_META(r)[i] = (struct rr_slot) {.active_pos = RR_FREE};
	memset(RR_BITS(r, 0), 0, slots * RR_WORDS * 8);
	return r;
}

static int rr_find(struct lighting_remote_relight* r, int32_t cx, int32_t cz,
				   bool create) {
	struct rr_slot* m = RR_META(r);
	for(uint32_t i = 0; i < r->n_active; i++)
		if(m[RR_ACTIVE(r)[i]].cx == cx && m[RR_ACTIVE(r)[i]].cz == cz)
			return RR_ACTIVE(r)[i];
	if(!create || r->n_active == r->slots)
		return -1;
	uint32_t s = 0;
	while(m[s].active_pos != RR_FREE) // one is free: n_active < slots
		s++;
	m[s] = (struct rr_slot) {cx, cz, 0, 0, (uint16_t)r->n_active};
	RR_ACTIVE(r)[r->n_active++] = (uint16_t)s;
	return (int)s;
}

static void rr_release_if_empty(struct lighting_remote_relight* r, int s) {
	struct rr_slot* m = RR_META(r);
	if(m[s].pending)
		return;
	uint16_t pos = m[s].active_pos, last = RR_ACTIVE(r)[--r->n_active];
	RR_ACTIVE(r)[pos] = last, m[last].active_pos = pos, m[s].active_pos = RR_FREE;
}

// commit=false: do the slotless columns of the n cells all fit? commit=true:
// take those slots and set the n pending bits (caller checked they fit).
static bool rr_mark(struct lighting_remote_relight* r, w_coord_t (*c)[3], int n,
					bool commit) {
	int32_t miss[7][2], lcx = 0, lcz = 0;
	int nmiss = 0, s = -1;
	for(int i = 0; i < n; i++) {
		int32_t cx = WCOORD_CHUNK_OFFSET(c[i][0]), cz = WCOORD_CHUNK_OFFSET(c[i][2]);
		bool same = i && cx == lcx && cz == lcz; // same column: skip the search
		if(!same)
			s = rr_find(r, cx, cz, commit), lcx = cx, lcz = cz;
		bool dup = same || s >= 0;
		if(commit) {
			uint32_t bit = (uint32_t)c[i][1] | (uint32_t)(c[i][2] & 15) << 7
				| (uint32_t)(c[i][0] & 15) << 11;
			uint64_t* w = &RR_BITS(r, s)[bit >> 6];
			if(!(*w & (1ull << (bit & 63))))
				*w |= 1ull << (bit & 63), RR_META(r)[s].pending++;
		}
		for(int k = 0; k < nmiss && !dup; k++)
			dup = miss[k][0] == cx && miss[k][1] == cz;
		if(!dup)
			miss[nmiss][0] = cx, miss[nmiss++][1] = cz;
	}
	return r->n_active + (commit ? 0 : nmiss) <= r->slots;
}

enum lighting_remote_status lighting_remote_relight_admit(
	struct lighting_remote_relight* r, w_coord_t x, w_coord_t y, w_coord_t z,
	lighting_column_loaded_fn loaded, void* user) {
	if(!RR_OK(r) || !loaded)
		return LIGHTING_REMOTE_UNINIT;
	w_coord_t c[7][3];
	int n = 0;
	for(int s = -1; s < SIDE_MAX; s++) { // -1 = the source cell itself
		w_coord_t cx = x + (s < 0 ? 0 : LIGHT_SIDE_OFF[s][0]),
				  cy = y + (s < 0 ? 0 : LIGHT_SIDE_OFF[s][1]),
				  cz = z + (s < 0 ? 0 : LIGHT_SIDE_OFF[s][2]);
		if(cy >= 0 && cy < WORLD_HEIGHT
		   && loaded(user, WCOORD_CHUNK_OFFSET(cx), WCOORD_CHUNK_OFFSET(cz)))
			c[n][0] = cx, c[n][1] = cy, c[n++][2] = cz;
	}
	if(!rr_mark(r, c, n, false)) // all or nothing
		return LIGHTING_REMOTE_NO_SLOT;
	rr_mark(r, c, n, true);
	return r->n_active ? LIGHTING_REMOTE_PENDING : LIGHTING_REMOTE_IDLE;
}

enum lighting_remote_status lighting_remote_relight_drain(
	struct lighting_remote_relight* r, bool ignore_sky_light, size_t budget,
	size_t* spent,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	lighting_column_loaded_fn loaded, void* user) {
	size_t used = 0, dummy;
	*(spent = spent ? spent : &dummy) = 0;
	if(!RR_OK(r) || !get_block || !set_light || !loaded)
		return LIGHTING_REMOTE_UNINIT;
	for(; r->n_active && used < budget; *spent = ++used, r->rr++) {
		if(r->rr >= r->n_active)
			r->rr = 0;
		int s = RR_ACTIVE(r)[r->rr];
		struct rr_slot* m = &RR_META(r)[s];
		uint64_t* w = &RR_BITS(r, s)[m->cursor];
		if(!*w) { // retained cursor: new arrivals never rewind it
			m->cursor = (uint16_t)((m->cursor + 1) % RR_WORDS);
			continue;
		}
		uint32_t bit = m->cursor * 64u + (uint32_t)__builtin_ctzll(*w);
		w_coord_t x = m->cx * CHUNK_SIZE + (w_coord_t)(bit >> 11),
				  y = (w_coord_t)(bit & 127),
				  z = m->cz * CHUNK_SIZE + (w_coord_t)((bit >> 7) & 15);
		uint8_t height;
		struct block_data old, o;
		if(get_block(user, x, y, z, &old, &height)) {
			// The lighting_flood relaxation, preserve_ignored_sky false.
			struct block* b = blocks[old.type];
			int8_t sky = !ignore_sky_light && y >= height ? 0xF : 0;
			int8_t torch = b ? b->luminance : 0, op = b ? MAX_I8(b->opacity, 1) : 1;
			w_coord_t nb[SIDE_MAX][3];
			int n = 0;
			for(enum side d = 0; d < SIDE_MAX; d++) {
				w_coord_t nx = x + LIGHT_SIDE_OFF[d][0],
						  ny = y + LIGHT_SIDE_OFF[d][1], nz = z + LIGHT_SIDE_OFF[d][2];
				uint8_t os = 0, ot = 0;
				if(get_light ? !get_light(user, nx, ny, nz, &os, &ot)
							 : !get_block(user, nx, ny, nz, &o, NULL))
					continue;
				if(!get_light)
					os = o.sky_light, ot = o.torch_light;
				if(loaded(user, WCOORD_CHUNK_OFFSET(nx), WCOORD_CHUNK_OFFSET(nz)))
					nb[n][0] = nx, nb[n][1] = ny, nb[n++][2] = nz;
				if(!b || b->can_see_through) {
					if(!ignore_sky_light)
						sky = MAX_I8(MAX_I8((int8_t)os - op, 0), sky);
					torch = MAX_I8(MAX_I8((int8_t)ot - op, 0), torch);
				}
			}
			uint8_t nl = (uint8_t)(torch << 4 | sky);
			if(nl != ((old.torch_light << 4) | old.sky_light)) {
				if(!rr_mark(r, nb, n, false)) { // slots < loaded columns: the
					r->rr++; // bit stays set; next call starts at another column
					return LIGHTING_REMOTE_NO_SLOT;
				}
				set_light(user, x, y, z, nl);
				rr_mark(r, nb, n, true);
			}
		}
		RR_BITS(r, s)[bit >> 6] &= ~(1ull << (bit & 63));
		m->pending--;
		rr_release_if_empty(r, s);
	}
	return r->n_active ? LIGHTING_REMOTE_PENDING : LIGHTING_REMOTE_IDLE;
}

void lighting_remote_relight_invalidate_column(
	struct lighting_remote_relight* r, w_coord_t cx, w_coord_t cz) {
	int s = RR_OK(r) ? rr_find(r, cx, cz, false) : -1;
	if(s >= 0) {
		memset(RR_BITS(r, s), 0, RR_WORDS * 8);
		RR_META(r)[s].pending = 0;
		rr_release_if_empty(r, s);
	}
}

void lighting_remote_relight_reset(struct lighting_remote_relight* r) {
	while(RR_OK(r) && r->n_active) {
		struct rr_slot* m = &RR_META(r)[RR_ACTIVE(r)[0]];
		lighting_remote_relight_invalidate_column(r, m->cx, m->cz);
	}
	if(RR_OK(r)) // REL-MP-FIX3d: a reset session restarts the round-robin
		r->rr = 0; // like a fresh init (same order for the same input)
}

void lighting_remote_relight_clear_cell(struct lighting_remote_relight* r,
	w_coord_t x, w_coord_t y, w_coord_t z) {
	int s = RR_OK(r) && y >= 0 && y < WORLD_HEIGHT
		? rr_find(r, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z), false) : -1;
	uint32_t bit = (uint32_t)y | (uint32_t)(z & 15) << 7 | (uint32_t)(x & 15) << 11;
	if(s >= 0 && (RR_BITS(r, s)[bit >> 6] & (1ull << (bit & 63)))) {
		RR_BITS(r, s)[bit >> 6] &= ~(1ull << (bit & 63));
		RR_META(r)[s].pending--;
		rr_release_if_empty(r, s);
	}
}

bool lighting_remote_relight_active(const struct lighting_remote_relight* r) {
	return RR_OK(r) && r->n_active;
}
