#include "settle_observer.h"
settle_callback world_settle_observe;
/*
	Copyright (c) 2022 ByteBit/xtreme8000

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
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks_data.h"
#include "column_view.h"
#include "game/game_state.h"
#include "light_brightness.h"
#include "lighting.h"
#include "network/piston_moving_transport.h"
#include "network/server_world.h"
#include "platform/gfx.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/g9_probe.h"
#include "platform/g9_capture.h"
#include "world.h"
#include "world_visibility.h"

static void world_light_repair_reset(struct world* w);
static void world_light_repair_mark_cell(struct world* w, w_coord_t x,
	w_coord_t z);
static bool world_light_seam_missing(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z);

static bool world_client_piston_moving_lookup(
	void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
	struct piston_moving_state* out) {
	(void)ctx;
	return client_piston_moving_query(x, y, z, out);
}

// params depend on fog texture. W1-b (far-reach program Wave 1): this is now
// only the DEFAULT for the runtime s_fog_clip dial (world_set_fog_clip /
// world_get_fog_clip below) -- the BFS render-reach call site reads
// world_get_fog_clip() at world_bfs time, not this macro directly.
#define FOG_DIST_NO_RENDER 1.13F
// Fog now begins at 0.25*fog_distance (Beta GL_LINEAR start=0.25*farPlane; see the fog
// shader/texture). A chunk needs the fog path enabled once it's past that, so has_fog must
// switch on at 0.25 -- the old 0.72 left chunks in the 0.25..0.72 band rendering fog-less
// while farther chunks were fogged (a visible discontinuity). Within 0.25R the fog amount is
// clamped to 0 anyway, so this is purely the per-chunk fog-path on/off boundary.
#define FOG_DIST_NO_EFFECT 0.25F

#define FOG_DIST_LESS(c, cx, cz, dist)                                                 \
	(glm_vec2_distance2(                                                       \
		 (vec2) {(c)->x + CHUNK_SIZE / 2, (c)->z + CHUNK_SIZE / 2},            \
		 (vec2) {cx, cz})                            \
	 <= glm_pow2((dist) * gstate.config.fog_distance))

struct wsection_dict_realloc_record {
	void* old_ptr;
	void* new_ptr;
};

enum wsection_dict_transaction_state {
	WSECTION_DICT_TX_IDLE,
	WSECTION_DICT_TX_PUBLISHING,
	WSECTION_DICT_TX_ARMED,
};

/* The allocator journal is process-global rather than dictionary-local. The
 * production client world is currently main-thread-owned, but tests and future
 * independent worlds may overlap. Tie callbacks to the exact owner and serialize
 * foreign transactions so ordinary contention can never masquerade as OOM. */
static unsigned s_wsection_dict_realloc_transaction_state;
static uintptr_t s_wsection_dict_realloc_transaction_owner;
static jmp_buf s_wsection_dict_realloc_escape;
static struct wsection_dict_realloc_record s_wsection_dict_realloc_records[4];
static size_t s_wsection_dict_realloc_record_count;
static unsigned s_wsection_dict_realloc_fail_countdown;

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static void (*s_wsection_dict_transaction_test_hook)(void*);
static void* s_wsection_dict_transaction_test_hook_arg;
static void (*s_wsection_dict_contention_test_hook)(void*);
static void* s_wsection_dict_contention_test_hook_arg;
#endif

static uintptr_t wsection_dict_current_thread(void) {
	uintptr_t result = 0;
#ifdef PLATFORM_PC
	pthread_t native = pthread_self();
#elif defined(PLATFORM_WII)
	lwp_t native = LWP_GetSelf();
#else
	/* CaveWii has only PC and Wii threaded builds. Keep single-threaded tools
	 * buildable without claiming cross-thread ownership there. */
	uintptr_t native = 1;
#endif
	typedef char wsection_dict_thread_id_must_fit_uintptr[
		(sizeof(native) <= sizeof(result)) ? 1 : -1];
	(void)sizeof(wsection_dict_thread_id_must_fit_uintptr);
	memcpy(&result, &native, sizeof(native));
	return result;
}

static bool wsection_dict_transaction_owned(void) {
	if(__atomic_load_n(
		   &s_wsection_dict_realloc_transaction_state, __ATOMIC_ACQUIRE)
	   != WSECTION_DICT_TX_ARMED)
		return false;
	return __atomic_load_n(
			   &s_wsection_dict_realloc_transaction_owner, __ATOMIC_RELAXED)
		== wsection_dict_current_thread();
}

static bool wsection_dict_transaction_try_begin(void) {
	const uintptr_t current = wsection_dict_current_thread();
	for(;;) {
		unsigned expected = WSECTION_DICT_TX_IDLE;
		if(__atomic_compare_exchange_n(
			   &s_wsection_dict_realloc_transaction_state, &expected,
			   WSECTION_DICT_TX_PUBLISHING, false, __ATOMIC_ACQ_REL,
			   __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&s_wsection_dict_realloc_transaction_owner,
						 current, __ATOMIC_RELAXED);
			__atomic_store_n(&s_wsection_dict_realloc_transaction_state,
						 WSECTION_DICT_TX_ARMED, __ATOMIC_RELEASE);
			return true;
		}
		/* Contention is not OOM: an independent test or future world mutator must
		 * not drop a block merely because another dictionary is resizing. A
		 * same-owner nested reserve cannot wait for itself and fails closed. */
		if(expected == WSECTION_DICT_TX_ARMED
		   && __atomic_load_n(
				  &s_wsection_dict_realloc_transaction_owner,
				  __ATOMIC_RELAXED)
				== current)
			return false;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
		if(s_wsection_dict_contention_test_hook)
			s_wsection_dict_contention_test_hook(
				s_wsection_dict_contention_test_hook_arg);
#endif
		thread_msleep(1);
	}
}

static void wsection_dict_transaction_end(void) {
	assert(wsection_dict_transaction_owned());
	__atomic_store_n(&s_wsection_dict_realloc_transaction_state,
					 WSECTION_DICT_TX_IDLE, __ATOMIC_RELEASE);
}

void* world_wsection_dict_realloc(void* ptr, size_t old_size, size_t new_size) {
	if(s_wsection_dict_realloc_fail_countdown > 0
	   && --s_wsection_dict_realloc_fail_countdown == 0)
		return NULL;
	if(!wsection_dict_transaction_owned())
		return realloc(ptr, new_size);
	if(s_wsection_dict_realloc_record_count
	   >= sizeof(s_wsection_dict_realloc_records)
		  / sizeof(s_wsection_dict_realloc_records[0]))
		return NULL;
	void* fresh = malloc(new_size);
	if(!fresh)
		return NULL;
	if(ptr)
		memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
	s_wsection_dict_realloc_records[s_wsection_dict_realloc_record_count++]
		= (struct wsection_dict_realloc_record) {
			.old_ptr = ptr,
			.new_ptr = fresh,
		};
	return fresh;
}

void world_wsection_dict_free(void* ptr) {
	if(wsection_dict_transaction_owned()) {
		/* M-lib's resize scratch is a transaction-owned allocation with no old
		 * buffer. Retire its record now so commit/rollback cannot free it twice. */
		for(size_t i = 0; i < s_wsection_dict_realloc_record_count; i++) {
			struct wsection_dict_realloc_record* record
				= &s_wsection_dict_realloc_records[i];
			if(record->old_ptr == NULL && record->new_ptr == ptr) {
				free(ptr);
				s_wsection_dict_realloc_records[i]
					= s_wsection_dict_realloc_records[
						--s_wsection_dict_realloc_record_count];
				return;
			}
		}
		assert(!"dictionary transaction attempted to free a borrowed buffer");
		return;
	}
	free(ptr);
}

void world_wsection_dict_memory_full(void) {
	if(wsection_dict_transaction_owned())
		longjmp(s_wsection_dict_realloc_escape, 1);
	abort();
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static void dict_wsection_test_fail_realloc_after(unsigned occurrence) {
	s_wsection_dict_realloc_fail_countdown = occurrence;
}
#endif

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void world_test_set_section_dict_transaction_hook(
	void (*hook)(void*), void* arg) {
	s_wsection_dict_transaction_test_hook = hook;
	s_wsection_dict_transaction_test_hook_arg = arg;
}

void world_test_set_section_dict_contention_hook(
	void (*hook)(void*), void* arg) {
	s_wsection_dict_contention_test_hook = hook;
	s_wsection_dict_contention_test_hook_arg = arg;
}
#endif

static bool dict_wsection_try_resize(dict_wsection_t dict, size_t capacity,
									 bool rehash_same_size) {
	assert(dict);
	if(!wsection_dict_transaction_try_begin())
		return false;
	struct dict_wsection_s saved = *dict;
	s_wsection_dict_realloc_record_count = 0;
	if(setjmp(s_wsection_dict_realloc_escape) == 0) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
		if(s_wsection_dict_transaction_test_hook)
			s_wsection_dict_transaction_test_hook(
				s_wsection_dict_transaction_test_hook_arg);
#endif
		if(rehash_same_size)
			dict_wsection_i_resize_up(dict, dict->mask + 1, true);
		else
			dict_wsection_reserve(dict, capacity);
		for(size_t i = 0; i < s_wsection_dict_realloc_record_count; i++)
			free(s_wsection_dict_realloc_records[i].old_ptr);
		s_wsection_dict_realloc_record_count = 0;
		wsection_dict_transaction_end();
		return true;
	}
	for(size_t i = 0; i < s_wsection_dict_realloc_record_count; i++)
		free(s_wsection_dict_realloc_records[i].new_ptr);
	s_wsection_dict_realloc_record_count = 0;
	*dict = saved;
	wsection_dict_transaction_end();
	return false;
}

static bool dict_wsection_try_reserve(dict_wsection_t dict, size_t capacity) {
	/* m-lib reserve(0) means shrink-to-fit, which frees live buffers -- outside
	 * the journal's grow-only contract (bug-hunt D10 latent reserve-0 guard).
	 * The same-size rehash path uses try_resize(_, 0, true) directly and is
	 * unaffected by this guard. */
	if(capacity == 0)
		return true;
	return dict_wsection_try_resize(dict, capacity, false);
}

static bool dict_wsection_try_reserve_insertions(dict_wsection_t dict,
												 size_t count) {
	assert(dict);
	/* Zero future insertions need no guarantee (D10 reserve-0 guard). */
	if(count == 0)
		return true;
	size_t size = dict_wsection_size(dict);
	if(count > SIZE_MAX - size
	   || count > SIZE_MAX - (size_t)dict->count_delete)
		return false;
	size_t live_after = size + count;
	size_t occupied_after = (size_t)dict->count_delete + count;
	if(occupied_after >= (size_t)dict->upper_limit) {
		/* Tombstones alone exhausted the probe table. If the live entries still
		 * fit, reproduce M-lib's same-size rehash under the checked scratch
		 * allocator; doubling an 804-byte-value table wastes hundreds of KiB. */
		if(live_after < (size_t)dict->upper_limit) {
			if(dict->mask == (m_index_t)-1)
				return false;
			return dict_wsection_try_resize(dict, 0, true);
		}
		/* Live occupancy really needs a larger table. Asking reserve for the
		 * current bucket count necessarily rounds to the next power of two. */
		size_t growth_capacity = (size_t)dict->mask + 1;
		if(growth_capacity == 0)
			return false;
		if(live_after < growth_capacity)
			live_after = growth_capacity;
	}
	return dict_wsection_try_reserve(dict, live_after);
}

void (*world_carve_depart)(const struct chunk*);

static void world_release_section_chunks(struct world* w,
										 struct world_section* s) {
	assert(w && s);
	for(size_t k = 0; k < COLUMN_HEIGHT; k++) {
		struct chunk* c = s->column[k];
		if(c) {
			/* Urgency counts resident owners, not deferred worker/GPU refs.
			 * Eviction is not an edit becoming visible: do not record latency. */
			if(c->mesh_urgent) {
				assert(w->mesh_urgent_count > 0);
				w->mesh_urgent_count--;
				c->mesh_urgent = false;
			}
			if(w->world_chunk_cache == c)
				w->world_chunk_cache = NULL;
			SETTLE(world_settle_observe, SET_RETIRE, c, NULL, 0, 0, 0, 0);
			if(world_carve_depart && c->reference_count > 1) world_carve_depart(c);
			chunk_unref(c);
			s->column[k] = NULL;
		}
	}
}

/* Preflight safe_get through the checked allocator. Tombstone-only pressure is
 * compacted at the current table size; live-capacity pressure grows once. */
static struct world_section* world_try_insert_section(struct world* w,
											  int64_t id) {
	assert(w && !dict_wsection_get(w->sections, id));
	if(!dict_wsection_try_reserve_insertions(w->sections, 1))
		return NULL;
	return dict_wsection_safe_get(w->sections, id);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void world_test_fail_section_dict_realloc_after(unsigned occurrence) {
	/* Sets the world.c translation unit's allocator seam; the dictionary helpers
	 * are header-generated and therefore keep separate test state per TU. */
	dict_wsection_test_fail_realloc_after(occurrence);
}
#endif

void world_unload_section(struct world* w, w_coord_t x, w_coord_t z) {
	assert(w);
	if(w->settled_light) {
		column_light_note_fallback(COLUMN_LIGHT_RESIDENCY);
		column_light_publication_release(w->settled_light);
		w->settled_light = NULL;
	}

	struct world_section* s
		= dict_wsection_get(w->sections, SECTION_TO_ID(x, z));

	if(s) {
		world_release_section_chunks(w, s);
		dict_wsection_erase(w->sections, SECTION_TO_ID(x, z));
	}
	lighting_remote_relight_invalidate_column(w->remote_relight, x, z);
}

void world_unload_all(struct world* w) {
	assert(w);
	column_light_publication_release(w->settled_light);
	w->settled_light = NULL;

	dict_wsection_it_t it;
	dict_wsection_it(it, w->sections);

	while(!dict_wsection_end_p(it)) {
		struct world_section* s = &dict_wsection_ref(it)->value;
		world_release_section_chunks(w, s);

		dict_wsection_next(it);
	}

	g9_probe_light_clear(w);
	stack_clear(&w->lighting_updates);
	// Abandon any in-progress relight frontier AND zero its pop accumulator (clear, not a
	// raw stack_clear, so a reset mid-flood can't leave a stale cflood reading).
	lighting_flood_queue_clear(&w->lighting_flood_queue);
	lighting_remote_relight_reset(w->remote_relight); /* reset/dimension */
	world_light_repair_reset(w); /* the flagged sections go with the dict */
	dict_wsection_reset(w->sections);
	w->world_chunk_cache = NULL;
}

/* Between frames/sessions, with the server stopped. Wait for the GPU first.
 * Receive every old job before starting remote inflate into the released heap. */
bool world_finish_local_teardown(struct world* w) {
	gfx_wait_idle();
	world_pre_render_clear(w);
	world_render_completed(w, false);
	ptime_t deadline = time_add_ms(time_get(), 500);
	while(chunk_mesher_pending()) {
		if(time_reached(time_get(), deadline)) {
			fprintf(stderr, "[world] local worker drain timed out after 500 ms; retaining current provider\n");
			return false;
		}
		chunk_mesher_receive();
		if(chunk_mesher_pending()) thread_msleep(1);
	}
	return column_view_pool_shutdown();
}

// --- corrupt chunk-chain guard ----------------------------------------------
// M-lib's intrusive list terminates a walk by IDENTITY against the list's sentinel head,
// and its node contracts are M_ASSERT -- compiled out under the shipping -DNDEBUG. A
// render/gpu-busy chain whose next-pointers no longer reach that head therefore SPINS the
// walker forever instead of faulting: the real-Wii main-thread hang (watchdog main phase
// WDM_BUILD_CHUNKS, with the server then blocked in clin_chunk_slot_acquire because main
// never returned to release chunk slots). Every chunk-list walk in this file is bounded by
// world_ilist_walk_cap(); exceeding it abandons the pass and bumps s_ilist_overrun, so a
// corrupt chain costs a dropped frame of chunk building/drawing, not the console.
//
// What the cap does NOT buy: it bounds the WALK, so a bail must still hand the next frame a
// coherent list. m-lib mutates through the pointers stored in the node itself, so a walk
// that also writes can leave wreckage long before the cap is reached -- see the two bails in
// world_render_completed, and ilist_node_unremovable below.
//
// The cap must never bite a healthy list. world_bfs pushes each resident chunk at most
// once per frame (visited_epoch), and gpu_busy_chunks is a copy of that same set, so one
// entry per resident chunk is the true ceiling; dict_wsection_size is O(1) (m-dict keeps
// the count), so the bound costs nothing per frame. WORLD_ILIST_WALK_CAP_MIN (world.h) is
// the absolute floor, so a nearly-empty world still gets a walk long enough to be worth
// bounding at all.
//
// THE CURRENT SECTION COUNT ALONE IS NOT A SAFE BOUND (2026-07-29). render and
// gpu_busy_chunks hold the PREVIOUS frame's entries, and world_unload_all -- the
// dimension switch / stream-reset path (client_interface.c) -- drops sections to ZERO
// without draining either list. The cap would then collapse to the 4096 floor while a
// far-ring render list legitimately held more than that, and the very next
// world_build_chunks would take the DELIBERATE-LEAK bail on a perfectly healthy chain:
// the guard's own failure mode, on the frame the player changes worlds.
// Draining the lists there instead is not the cheaper fix -- it is not even a correct
// one: after the hand-off in world_render_completed the render list still holds the same
// nodes whose reference tokens now belong to gpu_busy_chunks, so unref-ing both lists
// would double-free every chunk in flight. So the bound is kept as a HIGH-WATER MARK
// instead. Every entry any list can hold was pushed under some earlier frame's cap, so a
// bound that never decreases can never sit below the live entry count; the cost of never
// decaying it is that a corrupt chain in a now-small world is walked up to the largest
// cap this world ever had before it bails -- a few thousand extra pointer chases in the
// frame it fires, which is nothing against the alternative of dropping healthy frames.
static size_t s_ilist_overrun = 0;

static size_t world_ilist_walk_cap(struct world* w) {
	size_t sections = dict_wsection_size(w->sections);
	size_t cap;

	// Guard the multiply: a wrapped product would UNDER-cap and start abandoning
	// healthy frames, the exact opposite of what this exists for.
	if(sections > SIZE_MAX / COLUMN_HEIGHT) {
		cap = SIZE_MAX;
	} else {
		cap = sections * COLUMN_HEIGHT;
		if(cap < WORLD_ILIST_WALK_CAP_MIN)
			cap = WORLD_ILIST_WALK_CAP_MIN;
	}

	if(cap > w->ilist_walk_cap_max)
		w->ilist_walk_cap_max = cap;

	return w->ilist_walk_cap_max;
}

size_t world_ilist_walk_cap_current(struct world* w) {
	assert(w);
	return world_ilist_walk_cap(w);
}

// m-lib's _unlink detaches a node by writing THROUGH the pointers stored in that node
// ("next->prev = prev; prev->next = next"), so a node that points at itself, or that has
// already been detached, turns a remove-as-you-go walk into a write through itself/NULL and
// a SECOND unref of the same chunk. A self-linked node is exactly what a duplicate
// _push_back leaves behind (re-pushing the node that is currently the list TAIL sets both of
// its pointers to itself), so the node is checked before it is removed instead of the chain
// that produced it being trusted. One compare against a cache line _ref just touched; it can
// never fire on a linked node of a healthy list, where next/prev are a neighbour or the head.
static bool ilist_node_unremovable(const struct m_il1st_head_s* n) {
	return n->next == NULL || n->prev == NULL || n->next == n || n->prev == n;
}

size_t world_ilist_overrun_count(void) {
	return s_ilist_overrun;
}

void world_ilist_overrun_reset(void) {
	s_ilist_overrun = 0;
}

static void world_bfs(struct world* w, ilist_chunks_t render, float x, float y,
					  float z, vec4* planes) {
	assert(w && render && planes);

	// New frame stamp for "visited" (replaces the old per-frame loop that zeroed
	// every loaded chunk's tmp_data -- O(loaded chunks), ~1152 on Wii / ~3872 on
	// PC, every frame). Skip 0 so a freshly-created chunk (visited_epoch == 0) is
	// never a false positive; on the ~4-billion-frame wrap no stale stamp can
	// alias the current epoch in practice (chunks evict in seconds, vastly sooner
	// than a full uint32 cycle).
	if(++w->bfs_epoch == 0)
		w->bfs_epoch = 1;

	enum side sides[6]
		= {SIDE_TOP, SIDE_LEFT, SIDE_BACK, SIDE_BOTTOM, SIDE_RIGHT, SIDE_FRONT};

	struct chunk* c_camera
		= world_find_chunk(w, x, fminf(y, WORLD_HEIGHT - 1), z);

	ilist_chunks_t queue;
	ilist_chunks_init(queue);

	if(!c_camera)
		return;

	ilist_chunks_push_back(queue, c_camera);
	c_camera->tmp_data = (struct chunk_step) {
		.from = SIDE_MAX,
		.used_exit_sides = 0,
		.steps = 0,
		.visited_epoch = w->bfs_epoch,
	};

	const size_t cap = world_ilist_walk_cap(w);
	size_t steps = 0;

	while(!ilist_chunks_empty_p(queue)) {
		// A chunk node relinked while still queued makes this drain cyclic; queued
		// chunks are not yet ref'd, so abandoning the flood only shrinks this frame's
		// visible set. Both lists survive the bail: `render` holds only nodes this loop
		// popped and re-pushed, one chunk_ref each, so it stays walkable and balanced;
		// `queue` is left non-empty pointing at a stack head that is about to die, which
		// is inert because nothing ever walks it again -- the next _push_back/_push_front
		// on one of those chunks overwrites BOTH of its pointers, and chunk_destroy does
		// not read them.
		if(++steps > cap) {
			s_ilist_overrun++;
			return;
		}

		struct chunk* current = ilist_chunks_pop_front(queue);
		ilist_chunks_push_back(render, current);
		chunk_ref(current);

		for(int s = 0; s < 6; s++) {
			struct chunk* neigh
				= world_find_chunk_neighbour(w, current, sides[s]);

			// `steps < 6` is intentionally dropped: with the old per-frame reset
			// every unvisited chunk had steps == 0, so the test was vacuously
			// true here (steps is only ever read from the just-popped `current`,
			// never from an unvisited `neigh`). The epoch check preserves that
			// exact visited-set without the reset.
			if(neigh && neigh->tmp_data.visited_epoch != w->bfs_epoch
			   && !(current->tmp_data.used_exit_sides & (1 << sides[s]))
			   && (current->tmp_data.from == SIDE_MAX
				   || current->reachable[current->tmp_data.from]
					   & (1 << sides[s]))
			   && FOG_DIST_LESS(neigh, x, z, world_get_fog_clip())
			   && glm_aabb_frustum(
				   (vec3[2]) {{neigh->x, neigh->y, neigh->z},
							  {neigh->x + CHUNK_SIZE, neigh->y + CHUNK_SIZE,
							   neigh->z + CHUNK_SIZE}},
				   planes)) {
				ilist_chunks_push_back(queue, neigh);
				neigh->tmp_data = (struct chunk_step) {
					.from = blocks_side_opposite(sides[s]),
					.used_exit_sides = current->tmp_data.used_exit_sides
						| (1 << blocks_side_opposite(sides[s])),
					.steps = world_visibility_bfs_next_steps(
						current->tmp_data.steps, neigh->y),
					.visited_epoch = w->bfs_epoch,
				};
			}
		}
	}
}

void world_create(struct world* w) {
	assert(w);
	w->settled_light = NULL;
	w->settled_light_generation = 0;
	w->settled_light_world_index = 0;

	dict_wsection_init(w->sections);
	ilist_chunks_init(w->render);
	ilist_chunks2_init(w->gpu_busy_chunks);
	stack_create(&w->lighting_updates, 16,
				 sizeof(struct world_modification_entry));
	lighting_flood_queue_create(&w->lighting_flood_queue);
	world_light_repair_reset(w);
	lighting_client_set_seam_hook(world_light_seam_missing);
	w->remote_relight = NULL;
	w->remote_relight_failed = false;
	memset(w->render_views, 0, sizeof(w->render_views));
	w->world_chunk_cache = NULL;
	w->anim_timer = time_get();
	w->dimension = WORLD_DIM_OVERWORLD;
	w->bfs_epoch = 0; // first world_bfs bumps it to 1
	// A fresh world holds no list entries, so this is the one point at which the walk
	// bound may safely start over (see world_ilist_walk_cap).
	w->ilist_walk_cap_max = 0;
	w->build_frame = 0;
	w->edit_to_visible_frames_max = 0;
	w->mesh_urgent_count = 0;
	w->lease_owner = NULL;
	w->gpu_lease_owner = NULL;
	w->gpu_lease = (struct world_lease) {0};
	w->gpu_lease_refused = 0;
	w->build_focus = false;
	w->build_focus_cx = 0;
	w->build_focus_cz = 0;
	w->build_focus_radius = 0;
}

/* Split-dims B1: give the GPU hand-off's lease back to the context it came
 * from, once nothing the GPU read last frame is pinned any more. */
static void world_gpu_lease_release(struct world* w) {
	if(!w->gpu_lease.held) return;
	if(w->gpu_lease_owner)
		(void)w->gpu_lease_owner->release(w->gpu_lease_owner->user,
										  WORLD_LEASE_GPU, &w->gpu_lease);
	w->gpu_lease = (struct world_lease) {0};
	w->gpu_lease_owner = NULL;
}

// H-3: see struct world's edit_to_visible_frames_max doc comment.
uint32_t world_edit_to_visible_frames_max(struct world* w) {
	assert(w);
	return w->edit_to_visible_frames_max;
}

void world_destroy(struct world* w) {
	assert(w);

	world_gpu_lease_release(w);
	world_unload_all(w);
	for(unsigned v = 0; v < WORLD_RENDER_VIEWS; v++) {
		free(w->render_views[v].chunks);
		w->render_views[v] = (struct world_render_view){0};
	}
	g9_probe_light_clear(w);
	stack_destroy(&w->lighting_updates);
	lighting_flood_queue_destroy(&w->lighting_flood_queue);
	w->remote_relight = NULL; /* caller-owned storage */
	dict_wsection_clear(w->sections);
}

size_t world_loaded_chunks(struct world* w) {
	assert(w);
	return dict_wsection_size(w->sections);
}

static void world_dl_census_chunk(const struct chunk* c, uint32_t* bytes,
								  uint32_t* lists, uint32_t* own) {
	for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++) {
		if(!c->has_displist[k] || !c->mesh[k].accounted)
			continue;
		*bytes += (uint32_t)c->mesh[k].length;
		(*lists)++;
		if(own && !c->mesh[k].block_owned)
			(*own)++;
	}
}

void world_dl_census(struct world* w, uint32_t now_ms,
					 struct world_dl_census* out) {
	assert(w && out);
	memset(out, 0, sizeof *out);
	dict_wsection_it_t it;
	for(dict_wsection_it(it, w->sections); !dict_wsection_end_p(it);
		dict_wsection_next(it)) {
		const struct world_section* s = &dict_wsection_ref(it)->value;
		for(unsigned cy = 0; cy < COLUMN_HEIGHT; cy++) {
			const struct chunk* c = s->column[cy];
			if(!c)
				continue;
			uint32_t b = 0, n = 0;
			world_dl_census_chunk(c, &b, &n, &out->own_alloc_lists);
			if(!n)
				continue;
			out->installed_bytes += b;
			out->installed_lists += n;
			uint32_t age = now_ms - c->mesh_installed_ms;
			out->age_bytes[age < 5000u ? 0 : age < 30000u ? 1 : 2] += b;
		}
	}
	ilist_chunks2_it_t gpu;
	size_t steps = 0;
	for(ilist_chunks2_it(gpu, w->gpu_busy_chunks); !ilist_chunks2_end_p(gpu);
		ilist_chunks2_next(gpu)) {
		if(++steps > w->ilist_walk_cap_max)
			break; /* never walk past the admitted bound */
		struct chunk* c = ilist_chunks2_ref(gpu);
		if(world_find_chunk(w, c->x, c->y, c->z) == c)
			continue; /* still installed: already counted above */
		world_dl_census_chunk(c, &out->pinned_bytes, &out->pinned_lists, NULL);
	}
}

/* Split-dims B2: the view world (see world.h). NULL = the default gstate
 * objects, which is the only state unless a view scope is entered with the
 * world-bound payload flag ON. */
static struct world* s_view_world;
static struct dict_entity_s* s_view_entities;

struct world* world_view_current(void) {
	return s_view_world ? s_view_world : &gstate.world;
}

struct dict_entity_s* world_view_entities(void) {
	return s_view_entities ? s_view_entities : gstate.entities;
}

void world_view_set(struct world* w, struct dict_entity_s* entities) {
	s_view_world = w;
	s_view_entities = entities;
}

void world_gpu_wait_idle(void) {
	gfx_wait_idle();
}

/* Split-dims B2: measured per-world storage (see world.h). Read-only walk of
 * the owner-thread structures; no allocation, no chunk state touched. */
void world_memory_usage(struct world* w, struct world_memory* out) {
	assert(w && out);
	*out = (struct world_memory) {0};
	/* Section dictionary: M-lib's open index + pair/freelist data buffers at
	 * their current CAPACITIES (dict slack included -- it is resident). */
	if(w->sections->index)
		out->dict_bytes += ((size_t)w->sections->mask + 1u)
			* sizeof(*w->sections->index);
	if(w->sections->data)
		out->dict_bytes += (size_t)w->sections->freelist_cap
			* sizeof(*w->sections->data);
	out->mem1_bytes += out->dict_bytes;
	/* chunk_alloc's exact malloc request (chunk.c: struct + alignment slack,
	 * clamped up to a pointer exactly as chunk_alloc does, + base stash + red
	 * zone). */
	size_t align = __alignof__(struct chunk);
	if(align < sizeof(void*))
		align = sizeof(void*);
	const size_t shell = sizeof(struct chunk) + align + sizeof(void*)
		+ CHUNK_GUARD;
	dict_wsection_it_t it;
	for(dict_wsection_it(it, w->sections); !dict_wsection_end_p(it);
		dict_wsection_next(it)) {
		struct world_section* s = &dict_wsection_ref(it)->value;
		out->sections++;
		for(int y = 0; y < COLUMN_HEIGHT; y++) {
			struct chunk* c = s->column[y];
			if(!c)
				continue;
			out->chunks++;
			out->mem1_bytes += shell;
			if(c->ownership == OWNED_VOXEL && c->blocks) {
				out->voxel_chunks++;
				out->voxel_bytes += CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2;
			} else if(c->ownership == LOCAL_SHARED_VIEW) {
				/* Backed by the shared column-view pool (local provider):
				 * that pool is accounted once, globally, not per world. */
				out->shared_view_chunks++;
			} else if(c->far_skin) {
				out->mem1_bytes += sizeof(struct far_column_skin);
			}
			if(c->mesh_block) {
				out->mesh_bytes += displaylist_block_allocation_bytes(c->mesh_block_bytes);
			} else {
				/* WII-EQUIVALENT bytes on every platform (Fable B2 F6): on the
				 * Wii a finished list's length is its GX allocation; on PC the
				 * list holds a 22/25-byte-per-vertex CPU copy uploaded to a
				 * VBO, so the Wii figure is derived from its vertex count
				 * (index after finalize) with the same GX-format estimate the
				 * carve observer uses (35-byte header + 9/12 B per vertex, rounded up to
				 * the 32-byte GX allocation granule). */
				for(size_t l = 0; l < CHUNK_MESH_LISTS; l++)
					if(c->has_displist[l]) {
#ifdef PLATFORM_WII
						out->mesh_bytes += c->mesh[l].length;
#else
						/* GX lists are 32-byte aligned allocations, as
						 * the Wii length and carve_observer.c's align32. */
						out->mesh_bytes += (35 + c->mesh[l].index
							* (c->mesh[l].tinted ? 12u : 9u) + 31u)
							& ~(size_t)31u;
#endif
					}
			}
		}
	}
	out->mem2_bytes = out->voxel_bytes + out->mesh_bytes;
	for(unsigned v = 0; v < WORLD_RENDER_VIEWS; v++)
		out->mem1_bytes += w->render_views[v].capacity * sizeof(struct chunk*);
	out->mem1_bytes += w->lighting_updates.length
		* w->lighting_updates.element_size;
	out->mem1_bytes += w->lighting_flood_queue.length
		* w->lighting_flood_queue.element_size;
}

struct block_data world_get_block(struct world* w, w_coord_t x, w_coord_t y,
								  w_coord_t z) {
	assert(w);
	struct chunk* c = world_find_chunk(w, x, y, z);

	return c ? chunk_get_block(c, W2C_COORD(x), W2C_COORD(y), W2C_COORD(z)) :
			   (struct block_data) {
				   .type = (y < WORLD_HEIGHT) ? 1 : 0,
				   .metadata = 0,
				   .sky_light
					   = (w->dimension == WORLD_DIM_OVERWORLD
						  && y >= WORLD_HEIGHT)
						 ? 15
						 : 0,
				   .torch_light = 0,
			   };
}

w_coord_t world_get_height(struct world* w, w_coord_t x, w_coord_t z) {
	assert(w);

	w_coord_t cx = WCOORD_CHUNK_OFFSET(x);
	w_coord_t cz = WCOORD_CHUNK_OFFSET(z);
	struct world_section* s
		= dict_wsection_get(w->sections, SECTION_TO_ID(cx, cz));

	return s ? s->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE] : 0;
}

void world_copy_heightmap(struct world* w, struct chunk* c,
						  uint8_t* heightmap) {
	assert(w && c && heightmap);
	struct world_section* s = dict_wsection_get(
		w->sections, SECTION_TO_ID(c->x / CHUNK_SIZE, c->z / CHUNK_SIZE));
	assert(s);

	memcpy(heightmap, s->heightmap, sizeof(s->heightmap));
}

void world_set_section_tint(struct world* w, w_coord_t x, w_coord_t z,
							const uint8_t* grass_i, const uint8_t* grass_j) {
	assert(w && grass_i && grass_j);
	struct world_section* s = dict_wsection_get(
		w->sections,
		SECTION_TO_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	if(!s)
		return;
	memcpy(s->grass_i, grass_i, sizeof(s->grass_i));
	memcpy(s->grass_j, grass_j, sizeof(s->grass_j));
}

bool world_get_section_tint(struct world* w, w_coord_t x, w_coord_t z,
							uint8_t* grass_i, uint8_t* grass_j) {
	assert(w && grass_i && grass_j);
	struct world_section* s = dict_wsection_get(
		w->sections,
		SECTION_TO_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	if(!s)
		return false; // no tint stored yet (e.g. first mesh before the column RPC)
	memcpy(grass_i, s->grass_i, sizeof(s->grass_i));
	memcpy(grass_j, s->grass_j, sizeof(s->grass_j));
	return true;
}

bool world_get_section_tint_at(struct world* w, w_coord_t x, w_coord_t z,
							   uint8_t* out_grass_i, uint8_t* out_grass_j) {
	assert(w && out_grass_i && out_grass_j);
	w_coord_t cx = WCOORD_CHUNK_OFFSET(x);
	w_coord_t cz = WCOORD_CHUNK_OFFSET(z);
	struct world_section* s
		= dict_wsection_get(w->sections, SECTION_TO_ID(cx, cz));
	if(!s)
		return false;
	size_t idx = (size_t)(W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE);
	*out_grass_i = s->grass_i[idx];
	*out_grass_j = s->grass_j[idx];
	return true;
}

static bool wsection_heightmap_get_block(void* user, c_coord_t x, w_coord_t y,
										 c_coord_t z, struct block_data* blk) {
	assert(user);
	struct world_section* s = user;
	struct chunk* c = s->column[y / CHUNK_SIZE];
	if(c)
		*blk = chunk_get_block(c, x, W2C_COORD(y), z);
	return c;
}

/* Beta MetadataChunkBlock.java:123 skips propagation when the source value
 * is unchanged. A flow/still publication (BlockFlowing.java:29-33) must still
 * install/dirty its ID, but forcing its already-settled client light can visit
 * unrelated stored cells that the server's no-op never touched. No existing
 * seed/frontier is removed: genuine edits before/after this optical no-op
 * retain their work and order. Match the client flood's actual source rule,
 * including its Nether Sky=0 behavior, rather than luminance alone. */
static bool world_fluid_light_noop(struct world* w, w_coord_t x, w_coord_t y,
	w_coord_t z, struct block_data old, struct block_data now,
	uint8_t old_height, uint8_t height) {
	const bool pair = (old.type == BLOCK_WATER_FLOW && now.type == BLOCK_WATER_STILL)
		|| (old.type == BLOCK_WATER_STILL && now.type == BLOCK_WATER_FLOW)
		|| (old.type == BLOCK_LAVA_FLOW && now.type == BLOCK_LAVA_STILL)
		|| (old.type == BLOCK_LAVA_STILL && now.type == BLOCK_LAVA_FLOW);
	if(!pair || old.metadata != now.metadata || old_height != height
	   || old.sky_light != now.sky_light || old.torch_light != now.torch_light)
		return false;
	const struct block* a=blocks[old.type];
	const struct block* b=blocks[now.type];
	if(!a || !b || a->opacity != b->opacity || a->luminance != b->luminance
	   || a->can_see_through != b->can_see_through)
		return false;
	if(x == INT32_MIN || x == INT32_MAX || z == INT32_MIN || z == INT32_MAX
	   || y <= 0 || y >= WORLD_HEIGHT-1)
		return false;
	const bool nether=w->dimension == WORLD_DIM_NETHER;
	int sky=!nether && y >= height ? 15 : 0;
	int torch=b->luminance;
	const int opacity=b->opacity ? b->opacity : 1;
	for(enum side side=0;side<SIDE_MAX;side++) {
		int dx,dy,dz; blocks_side_offset(side,&dx,&dy,&dz);
		struct world_section* section=dict_wsection_get(w->sections,
			SECTION_TO_ID(WCOORD_CHUNK_OFFSET(x+dx),WCOORD_CHUNK_OFFSET(z+dz)));
		if(!section || section->is_far_lod
		   || !world_chunk_from_section(w,section,y+dy)) return false;
		if(b->can_see_through) {
			struct block_data n=world_get_block(w,x+dx,y+dy,z+dz);
			if(!nether && (int)n.sky_light-opacity > sky) sky=(int)n.sky_light-opacity;
			if((int)n.torch_light-opacity > torch) torch=(int)n.torch_light-opacity;
		}
	}
	return now.sky_light == sky && now.torch_light == torch;
}

static uint32_t s_remote_unchanged_skips;
uint32_t world_remote_unchanged_skips(void) {
	return s_remote_unchanged_skips;
}

bool world_remote_block_unchanged(struct world* w, w_coord_t x, w_coord_t y,
								  w_coord_t z, struct block_data blk) {
	/* Beta Chunk.setBlockIDWithMetadata returns false with no write and no
	 * light work when id and metadata are unchanged (decompile Chunk.java:
	 * 240-241; CFR /c/tmp/mcb173-server-decompile/out/net/minecraft/src/
	 * Chunk.java:212-213). The vanilla server sends every light change as such
	 * a Packet53/52 (World.java:491-501, PlayerInstance.java:122-166); writing
	 * the payload's 0/0 light and forcing a relight per edit flooded the
	 * client (tmp/jobs/triage-1020/flood-diag.md). Only a resident, non-far
	 * cell qualifies (else world_set_block must still allocate/install), and
	 * id 36 keeps its sidecar-settling overwrite. */
	if(y < 0 || y >= WORLD_HEIGHT || blk.type == BLOCK_PISTON_MOVING)
		return false;
	struct world_section* s = dict_wsection_get(w->sections,
		SECTION_TO_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	struct chunk* c = world_chunk_from_section(w, s, y);
	if(!c || s->is_far_lod || c->far_skin)
		return false;
	struct block_data old
		= chunk_get_block(c, W2C_COORD(x), W2C_COORD(y), W2C_COORD(z));
	if(old.type != blk.type || old.metadata != blk.metadata)
		return false;
	s_remote_unchanged_skips++;
	return true;
}

static bool world_remote_relight_admit(struct world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z);

/* LIGHT-CLIENT-LOSSLESS: does cell (x, z) lie on its column's edge next to a
 * column that is absent or far-LOD (Beta doChunksNearChunkExist(x, 0, z, 1)
 * false)? Same answer as world_light_seam_ok, without a pop descriptor. */
static bool world_cell_touches_absent(struct world* w, w_coord_t x, w_coord_t z) {
	int lx = W2C_COORD(x), lz = W2C_COORD(z);
	int dx = lx == 0 ? -1 : (lx == CHUNK_SIZE - 1 ? 1 : 0);
	int dz = lz == 0 ? -1 : (lz == CHUNK_SIZE - 1 ? 1 : 0);
	w_coord_t cx = WCOORD_CHUNK_OFFSET(x), cz = WCOORD_CHUNK_OFFSET(z);
	for(int a = dx < 0 ? -1 : 0; a <= (dx > 0 ? 1 : 0); a++)
		for(int b = dz < 0 ? -1 : 0; b <= (dz > 0 ? 1 : 0); b++) {
			if(!a && !b)
				continue;
			struct world_section* n
				= dict_wsection_get(w->sections, SECTION_TO_ID(cx + a, cz + b));
			if(!n || n->is_far_lod)
				return true;
		}
	return false;
}

void world_set_block(struct world* w, w_coord_t x, w_coord_t y, w_coord_t z,
					 struct block_data blk, bool light_update) {
	assert(w);

	if(y < 0 || y >= WORLD_HEIGHT)
		return;

	// [D2] Write the block IMMEDIATELY -- Beta setBlockAndMetadata is synchronous,
	// so the LAST write to a cell wins in arrival order. The old light_update path
	// deferred the WRITE itself onto the relight stack, which drains one entry per
	// frame in LIFO order -> same-cell writes landed in REVERSE order, with up to
	// one-frame-per-backlog-entry latency. Now only the relight is deferred.
	w_coord_t cx = WCOORD_CHUNK_OFFSET(x);
	w_coord_t cz = WCOORD_CHUNK_OFFSET(z);
	struct world_section* s
		= dict_wsection_get(w->sections, SECTION_TO_ID(cx, cz));
	struct chunk* c = world_chunk_from_section(w, s, y);
	const bool resident = light_update && c && !s->is_far_lod && !c->far_skin;
	struct block_data old = {0};
	uint8_t old_height = 0;
	if(resident) {
		old = chunk_get_block(c, W2C_COORD(x), W2C_COORD(y), W2C_COORD(z));
		old_height = s->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE];
	}

	if(!c) {
		if(!s) {
			s = world_try_insert_section(w, SECTION_TO_ID(cx, cz));
			if(!s) {
				printf("[world] section dictionary alloc failed; block dropped\n");
				return;
			}
			memset(s, 0, sizeof(*s));
			s->is_far_lod = false; // a real block edit created this section
		}
		c = chunk_alloc(); // alignment-honouring (over-aligned struct; malloc under-aligns -> AVX fault)
		if(!c) {
			// MEM1 low: drop this block write rather than abort. The server
			// streaming evictor (server_local_update) now keeps the chunk set
			// bounded so this should never trigger -- it's a logged safety
			// net, not a silent stub.
			printf("[world] chunk alloc failed (MEM1 low); block dropped\n");
			return;
		}

		w_coord_t cy = y / CHUNK_SIZE;
		chunk_init(c, w, cx * CHUNK_SIZE, cy * CHUNK_SIZE, cz * CHUNK_SIZE);
		chunk_ref(c);

		w->world_chunk_cache = c;

		assert(s->column[cy] == NULL);
		s->column[cy] = c;
	}

	/* LIGHT-CLIENT-LOSSLESS: at a cell touching a non-resident column the
	 * client relight is raise-only (world_light_seam_ok), so the light the
	 * edit packet carries (often 0) would stick and its loss would cascade
	 * inward. Keep the cell's stored light there, as Beta's setBlock does
	 * (Chunk.setBlockIDWithMetadata leaves the light arrays to the relight). */
	const bool seam_keep = resident && !(gstate.remote_session && w->remote_relight)
		&& world_cell_touches_absent(w, x, z);
	if(seam_keep) {
		blk.sky_light = old.sky_light;
		blk.torch_light = old.torch_light;
	}
	chunk_set_block(c, W2C_COORD(x), W2C_COORD(y), W2C_COORD(z), blk);
	lighting_heightmap_update(s->heightmap, W2C_COORD(x), y, W2C_COORD(z),
							  blk.type, wsection_heightmap_get_block, s);
	/* LIGHT-CLIENT-LOSSLESS (fix-r2): an edit that RAISES the heightmap
	 * clears the skylight of the newly covered cells [old height, new height)
	 * of this column at once, exactly as Beta does with no relight check
	 * (../mcb173-retromcp/minecraft/src/net/minecraft/src/Chunk.java:251
	 * setBlockIDWithMetadata -> func_1003_g, lines 199-200 setNibble(.., 0)).
	 * Only a seam cell needs it: there the relight is raise-only, so the old
	 * skylight 15 would otherwise stay under the new roof for good; elsewhere
	 * the relight lowers it to the same fixed point. Every cleared cell is
	 * then seeded below, so the relight raises whatever its resident
	 * neighbours still light. Seams only = the success path is untouched. */
	w_coord_t seam_clear_lo = 0, seam_clear_hi = 0;
	if(seam_keep && w->dimension != WORLD_DIM_NETHER
	   && !(gstate.remote_session && w->remote_relight_failed)) {
		const uint8_t height
			= s->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE];
		if(height > old_height) {
			seam_clear_lo = old_height;
			seam_clear_hi = height;
			for(w_coord_t cy = seam_clear_lo; cy < seam_clear_hi; cy++) {
				struct chunk* cc = world_chunk_from_section(w, s, cy);
				if(!cc || cc->far_skin)
					continue;
				struct block_data b
					= chunk_get_block(cc, W2C_COORD(x), W2C_COORD(cy), W2C_COORD(z));
				chunk_set_light(cc, W2C_COORD(x), W2C_COORD(cy), W2C_COORD(z),
								(uint8_t)(b.torch_light << 4));
			}
		}
	}

	if(!light_update) /* partial Packet51: the server's light wins this cell */
		lighting_remote_relight_clear_cell(w->remote_relight, x, y, z);
	if(light_update && !(resident && world_fluid_light_noop(w, x, y, z,
		old, blk, old_height, s->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE]))
	   && !world_remote_relight_admit(w, x, y, z)) {
		// Defer ONLY the relight: one flood per frame paces the single Wii core.
		// lighting_flood re-reads the cell through world_light_get_block, so an
		// entry processed after later same-cell writes still converges to the
		// committed state (the deferred WRITE-reorder bug is gone).
		size_t g9_depth = g9_probe_main_enabled()
			? stack_size(&w->lighting_updates) : 0;
		/* LIGHT-CLIENT-LOSSLESS: stack_push DROPS an entry whose grow fails.
		 * Reserve first; on a failed grow the column AND its side neighbours'
		 * columns go to the repair instead, as the forced seed would have
		 * pushed them (its sweep covers this cell and its in-column
		 * neighbours, and a later edge change marks the next column), so
		 * nothing is lost. The cells a seam roof just uncovered of skylight
		 * (below) are seeded the same way. */
		const w_coord_t seed_lo = seam_clear_hi > seam_clear_lo ? seam_clear_lo : y;
		for(w_coord_t sy = seed_lo; sy <= y; sy++) { // the edit last: popped first, as before
			if(!lighting_client_reserve(&w->lighting_updates,
					stack_size(&w->lighting_updates) + 1)) {
				lighting_client_note_oom_park();
				world_light_repair_mark_cell(w, x, z);
			} else {
				stack_push(&w->lighting_updates,
						   &(struct world_modification_entry) {
							   .x = x,
							   .y = sy,
							   .z = z,
							   .blk = blk,
						   });
			}
		}
		if(g9_probe_main_enabled() && stack_size(&w->lighting_updates) > g9_depth)
			g9_probe_light_push(w, (unsigned)stack_size(&w->lighting_updates));
	}
}

// M1 deep reach: build a far-ring synthetic-voxel chunk directly from its compact
// per-column skin (skin_y + top_id) + biome tint, BYPASSING the per-cell
// world_set_block path. The old path called world_set_block ~5000x per far chunk,
// each re-doing a dict_wsection_get hash lookup, a per-cell heightmap maintenance
// scan, and a chunk_mark_dirty + 6-neighbour trigger -- a measured client hitch
// under fast far streaming (it dropped the client to ~2fps). Here we resolve the
// section + the 1-3 spanned chunks ONCE, write the packed buffer directly, set the
// synthetic heightmap directly (heightmap = skin_y+1), and mark dirty + dirty
// the 4 neighbours ONCE. The resulting blocks buffer + heightmap are pinned
// against a slow per-cell reference by bench_far_chunk; far-skin extraction is
// separately covered by the production extractor tests.
//
// x,z = the chunk-origin WORLD coords (chunk_x*CHUNK_SIZE), like world_set_block.
// skin_y/top_id are [lx*CHUNK_SIZE + lz]; a skin_y >= WORLD_HEIGHT (the network
// FAR_NO_SURFACE=0xFF sentinel, or any out-of-range value) means an all-air column.
//
// S2 far-storage compression ("lod.far_storage", world_get_far_storage; see
// world.h and chunk.h's struct far_column_skin design comment): the VOXEL
// default below is completely untouched -- same allocations, same per-cell
// chunk_set_block_raw fill. The SKIN mode branches at each of the three
// spots that would otherwise bake the column into a real block buffer
// (allocation, init, fill) and instead has each chunk shell synthesize its
// own cells from a 512 B private copy of this function's skin_y/top_id, via
// chunk_alloc_far_skin/chunk_init_far_skin. Everything else in this function
// -- the dictionary lookup/insert, is_far_lod, dirty-marking, neighbour
// re-dirty, grass/foliage tint -- is IDENTICAL for both modes; see
// tests/test_far_storage_equiv.c for the byte-identical display-list proof.
bool world_build_far_section(struct world* w, w_coord_t x, w_coord_t z,
							 const uint8_t* skin_y, const uint8_t* top_id,
							 const uint8_t* grass_i, const uint8_t* grass_j) {
	assert(w && skin_y && top_id && grass_i && grass_j);

	w_coord_t cx = WCOORD_CHUNK_OFFSET(x);
	w_coord_t cz = WCOORD_CHUNK_OFFSET(z);

	// Vertical span: floor = the lowest surface, top = the highest surface + 2
	// air cells. Fill only this range (not down to y=0): a NULL section below halts
	// the render BFS, so deeper fill is useless for occlusion.
	int chunk_min = WORLD_HEIGHT;
	int chunk_max = 0;
	for(int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; i++) {
		int sy = skin_y[i];
		if(sy >= WORLD_HEIGHT)
			continue; // no selected liquid or opaque surface
		if(sy < chunk_min)
			chunk_min = sy;
		if(sy + 2 > chunk_max)
			chunk_max = sy + 2;
	}
	if(chunk_min >= WORLD_HEIGHT) {
		/* An all-air replacement needs no allocation and therefore cannot fail.
		 * Commit its intentional absence only after the complete skin was checked. */
		world_unload_section(w, cx, cz);
		return true;
	}
	if(chunk_max >= WORLD_HEIGHT)
		chunk_max = WORLD_HEIGHT - 1;
	const uint8_t open_sky
		= (w->dimension == WORLD_DIM_OVERWORLD) ? 15 : 0;

	/* Build the complete replacement owner before touching the published section.
	 * A far refresh can target an already resident full/far owner; releasing that
	 * owner first turns a recoverable allocation failure into a permanent hole
	 * because the server has already marked the FAR RPC sent. */
	int cy_lo = chunk_min / CHUNK_SIZE, cy_hi = chunk_max / CHUNK_SIZE;
	enum world_far_storage storage = world_get_far_storage();

	// S2 far-storage compression ("lod.far_storage":"skin"): precompute the
	// per-column skin (heightmap = sy+1 or 0 for all-air; the surface block
	// id) ONCE here -- every Y-slice's chunk_init_far_skin below copies the
	// SAME 512 B into its own private far_column_skin (chunk.h). Identical
	// per-column decisions to the voxel fill loop further down; kept as a
	// separate small (256-iteration) pass so that loop's bytes stay untouched
	// for the default VOXEL path (T1: zero cost when the toggle is off).
	uint8_t skin_heightmap[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t skin_top_id[CHUNK_SIZE * CHUNK_SIZE];
	if(storage == WORLD_FAR_STORAGE_SKIN) {
		for(int lx = 0; lx < CHUNK_SIZE; lx++) {
			for(int lz = 0; lz < CHUNK_SIZE; lz++) {
				int i = lx * CHUNK_SIZE + lz;
				int sy = skin_y[i];
				if(sy >= WORLD_HEIGHT) {
					skin_heightmap[i] = 0;
					skin_top_id[i] = BLOCK_AIR; // never read: heightmap==0 always -> air
				} else {
					skin_heightmap[i] = (uint8_t)(sy + 1);
					skin_top_id[i] = top_id[i];
				}
			}
		}
	}

	struct chunk* replacement[COLUMN_HEIGHT] = {0};
	for(int cy = cy_lo; cy <= cy_hi; cy++) {
		replacement[cy] = (storage == WORLD_FAR_STORAGE_SKIN)
			? chunk_alloc_far_skin()
			// aligned: over-aligned struct, malloc under-aligns. The far-ring
			// variant takes its 10 KB block buffer from the far-column slot
			// pool (platform/mem2.h) instead of the shared carve heap -- this
			// call site IS the pool's tenant.
			: chunk_alloc_far_voxel();
		if(!replacement[cy]) {
			printf("[world] far chunk alloc failed (MEM1 low); section preserved\n");
			for(int k = cy_lo; k < cy; k++)
				chunk_unref(replacement[k]);
			return false;
		}
		if(storage == WORLD_FAR_STORAGE_SKIN)
			chunk_init_far_skin(replacement[cy], w, cx * CHUNK_SIZE,
								cy * CHUNK_SIZE, cz * CHUNK_SIZE, skin_heightmap,
								skin_top_id, (uint8_t)chunk_min);
		else
			chunk_init(replacement[cy], w, cx * CHUNK_SIZE, cy * CHUNK_SIZE,
					   cz * CHUNK_SIZE);
		chunk_ref(replacement[cy]);
	}

	/* Existing owners reuse their dictionary bucket. A new coordinate preflights
	 * M-lib's fatal safe_get allocation only after all chunks are owned locally;
	 * failure releases that private replacement and leaves the world unchanged. */
	int64_t id = SECTION_TO_ID(cx, cz);
	struct world_section* s = dict_wsection_get(w->sections, id);
	if(!s) {
		s = world_try_insert_section(w, id);
		if(!s) {
			printf("[world] far section dictionary alloc failed; world unchanged\n");
			for(int cy = cy_lo; cy <= cy_hi; cy++)
				chunk_unref(replacement[cy]);
			return false;
		}
	}
	world_release_section_chunks(w, s);
	memset(s, 0, sizeof(*s));
	s->is_far_lod = true; // synthetic LOD: the client relight must not flood into it
	for(int cy = cy_lo; cy <= cy_hi; cy++)
		s->column[cy] = replacement[cy];
	w->world_chunk_cache = replacement[cy_hi];

	if(storage == WORLD_FAR_STORAGE_SKIN) {
		/* The private skin is x-major; public section metadata and height
		 * queries are z-major, as in the voxel path below. */
		for(int lx = 0; lx < CHUNK_SIZE; lx++)
			for(int lz = 0; lz < CHUNK_SIZE; lz++)
				s->heightmap[lx + lz * CHUNK_SIZE]
					= skin_heightmap[lx * CHUNK_SIZE + lz];
	} else {
		// Top of the allocated (spanned) vertical range -- light all open air up to
		// here so the synthetic section is SELF-CONSISTENT with what a relight recomputes.
		// Beta's Hell provider hasNoSky, so its corresponding value is zero.
		int y_top = (cy_hi + 1) * CHUNK_SIZE - 1;
		if(y_top >= WORLD_HEIGHT)
			y_top = WORLD_HEIGHT - 1;

		// Fill each column directly + set its heightmap to the surface fixpoint.
		//
		// LIGHTING SELF-CONSISTENCY (the M1 far-ring crash fix): the client per-frame
		// relight (world_update_lighting), seeded by ordinary fluid/grass/fire ticks near
		// the player, crosses the near/far seam and floods over THIS synthetic section via
		// world_light_get_block. lighting_flood (no visited-set) re-pushes a neighbour on
		// every light CHANGE, so any cell whose stored light disagrees with the recomputed
		// value becomes a flood source. The old code stored sky=15 on the OPAQUE fillers
		// (recomputed to 0) and left the high open-sky air at the chunk_init sky=0
		// (recomputed to 15) -- two seam-spanning inconsistencies that cascaded skylight
		// (which never decays under open sky) across the WHOLE annulus, ballooning the BFS
		// queue until realloc OOM'd -> the 0x00800000 NULL-data DSI crash. Storing the
		// VALUES THE FLOOD RECOMPUTES (opaque=0, Overworld open-sky air=15;
		// Nether=0) makes old==new at the seam, so the flood stops. No render
		// regression: the drawn far skin TOP face + the
		// LOD skirts sample the ABUTTING AIR cell, never the filler's light. Nether lava
		// remains emissive through the block registry's Beta luminance=15 render minimum.
		for(int lx = 0; lx < CHUNK_SIZE; lx++) {
			for(int lz = 0; lz < CHUNK_SIZE; lz++) {
				int i = lx * CHUNK_SIZE + lz;
				int sy = skin_y[i];
				if(sy >= WORLD_HEIGHT) {
					// All-air column (no surface): every spanned cell uses the dimension's
					// open-air sky value. In the Overworld, heightmap 0 makes relight
					// recompute every y>=0 to 15; in the Nether hasNoSky keeps it at 0.
					for(int y = cy_lo * CHUNK_SIZE; y <= y_top; y++)
						chunk_set_block_raw(s->column[y / CHUNK_SIZE], lx, W2C_COORD(y), lz,
											(struct block_data) {.type = BLOCK_AIR,
																 .metadata = 0,
																 .sky_light = open_sky,
																 .torch_light = 0});
					s->heightmap[lx + lz * CHUNK_SIZE] = 0;
					continue;
				}
				uint8_t tid = top_id[i];
				// Surface-id filler up to the surface; the cell at sy
				// is the visible skin block, the fillers below are occluded (only there
				// so the column reads solid for lod_surface_height + the skirts). OPAQUE
				// sub-surface cells store sky=0 -- the value the relight recomputes (storing
				// 15 was a flood amplifier). Below chunk_min the memset air stays sky=0
				// (occluded, y < column_height), already consistent.
				for(int y = chunk_min; y <= sy; y++)
					chunk_set_block_raw(s->column[y / CHUNK_SIZE], lx, W2C_COORD(y), lz,
										(struct block_data) {.type = tid,
															 .metadata = 0,
															 .sky_light = 0,
															 .torch_light = 0});
				// ALL open air above the surface uses the dimension's sky value, not
				// just the two cells the skin samples. The skin top face needs sy+1;
				// the rest up to the spanned top must match too, or every memset-zero
				// cell seeds the runaway Overworld skylight cascade across the annulus.
				for(int y = sy + 1; y <= y_top; y++)
					chunk_set_block_raw(s->column[y / CHUNK_SIZE], lx, W2C_COORD(y), lz,
										(struct block_data) {.type = BLOCK_AIR,
															 .metadata = 0,
															 .sky_light = open_sky,
															 .torch_light = 0});
				// Synthetic surface height. This intentionally follows the compact
				// skin for transparent water/lava as well as opaque terrain.
				s->heightmap[lx + lz * CHUNK_SIZE] = (uint8_t)(sy + 1);
			}
		}
	}

	if(mem2_carve_observe) for(int cy = cy_lo; cy <= cy_hi; cy++) {
		struct chunk* c = s->column[cy];
		if(c->blocks) mem2_tag(c->blocks, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2, CARVE_FAR);
	}

	// Mark each spanned chunk dirty + needing a display-list rebuild ONCE.
	for(int cy = cy_lo; cy <= cy_hi; cy++) {
		struct chunk* c = s->column[cy];
		chunk_mark_dirty(c);
		SETTLE(world_settle_observe, SET_DIRTY_INSTALL, c, NULL, 0, 0, 0, 0);
		c->rebuild_displist = true;
	}

	// Dirty the 4 horizontal neighbour chunks once so their faces toward this new
	// surface re-mesh (the per-cell path triggered this per boundary cell).
	// H-2 band 0 (2026-09-03 maintainer ruling): also mark them urgent -- a
	// newly-installed section's neighbours must not wait behind an unrelated
	// near/far backlog before their border re-meshes.
	static const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
	for(int n = 0; n < 4; n++)
		for(int cy = cy_lo; cy <= cy_hi; cy++) {
			struct chunk* o
				= world_find_chunk(w, (cx + nb[n][0]) * CHUNK_SIZE,
								   cy * CHUNK_SIZE, (cz + nb[n][1]) * CHUNK_SIZE);
			if(o) {
				o->rebuild_displist = true;
				SETTLE(world_settle_observe, SET_DIRTY_NEIGHBOUR, o, NULL, 0, 0,
					   0, 0);
				chunk_mark_urgent(o);
			}
		}

	// Biome grass/foliage tint (same [lx*16+lz] layout as the full-chunk path).
	memcpy(s->grass_i, grass_i, sizeof(s->grass_i));
	memcpy(s->grass_j, grass_j, sizeof(s->grass_j));
	return true;
}

// BULK full-column install for a CRPC_CHUNK (the near path). Byte-for-byte equivalent
// to the old per-cell clin_chunk (32K world_set_block calls -> dict lookup + per-cell
// dirty + neighbour trigger + heightmap maintain EACH), but resolves the section + the
// 8 vertical chunks ONCE, writes cells via chunk_set_block_raw, copies the server's
// authoritative heightmap, and marks dirty + dirties the 4 neighbours ONCE. This is the
// same speedup world_build_far_section applies to the far ring; it cut the near install
// from the load-time bottleneck (~100ms/column on the render thread) to a few ms.
//
// ids is full-byte (one id/cell); metadata/lighting_sky/lighting_torch are nibble-packed
// (two cells/byte). Source order [lx][lz][y] is transposed by the chunk loader
// into its X-paired storage, byte-equivalent to clin_chunk. x,z = chunk-origin
// world coords.
static bool world_build_full_section_impl(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* lighting_sky,
	const uint8_t* lighting_torch, const uint8_t* heightmap, const uint8_t* grass_i,
	const uint8_t* grass_j, struct column_backing* backing) {
	assert(w && ids && metadata && lighting_sky && lighting_torch && heightmap);

	w_coord_t cx = WCOORD_CHUNK_OFFSET(x);
	w_coord_t cz = WCOORD_CHUNK_OFFSET(z);

	/* Allocate every replacement chunk before releasing an existing full/far
	 * owner. A failed promotion/rebuild therefore leaves the old complete section
	 * intact until the epoch marker deliberately requests a clean restream. */
	const int n_cy = WORLD_HEIGHT / CHUNK_SIZE;
	struct column_view* view = backing ? column_view_create(backing,
		ids, metadata, lighting_sky, lighting_torch) : NULL;
	if(backing && !view) return false;
	struct chunk* replacement[COLUMN_HEIGHT] = {0};
	for(int cy = 0; cy < n_cy; cy++) {
		replacement[cy] = view ? chunk_alloc_shared() : chunk_alloc();
		if(!replacement[cy]) {
			printf("[world] full chunk alloc failed (MEM1 low); section preserved\n");
			for(int k = 0; k < cy; k++)
				chunk_unref(replacement[k]);
			column_view_unref(view);
			return false;
		}
		if(view)
			chunk_init_shared(replacement[cy], w, cx * CHUNK_SIZE, cy * CHUNK_SIZE,
				cz * CHUNK_SIZE, view, backing);
		else
			chunk_init(replacement[cy], w, cx * CHUNK_SIZE, cy * CHUNK_SIZE, cz * CHUNK_SIZE);
		chunk_ref(replacement[cy]);
	}
	column_view_unref(view); // the shells now own the complete replacement

	/* Preflight a new dictionary owner before destroying anything at this
	 * coordinate. Existing far/full sections reuse their bucket, so an allocation
	 * failure cannot pass through M-lib's fatal safe_get path. */
	int64_t id = SECTION_TO_ID(cx, cz);
	struct world_section* s = dict_wsection_get(w->sections, id);
	if(!s) {
		s = world_try_insert_section(w, id);
		if(!s) {
			printf("[world] full section dictionary alloc failed; section dropped\n");
			for(int cy = 0; cy < n_cy; cy++)
				chunk_unref(replacement[cy]);
			return false;
		}
	}
	/* REL-FLK: a full-to-full refresh of a resident column keeps its eight
	 * shells (complete meshes, mesh blocks, first-mesh/LOD/visibility state and
	 * any worker/GPU refs) and exchanges only the voxel storage/ownership with
	 * the preflighted staging shells. The dirty generation advanced below makes
	 * an older in-flight job stale, so the last complete geometry stays drawable
	 * until chunk_mesher_receive's complete-current-result swap retires it.
	 * Detaching the shells here instead left the column with no lists until the
	 * replacement meshed (DIAG.md: the whole-column flicker).
	 * Local sessions only: the real-Beta MP client path keeps the fresh-shell
	 * replace below byte-identical (fresh dirty generation/urgency and stale
	 * drops of queued light work, pinned by test_lighting_remote_ingress_equiv). */
	bool refresh = !gstate.remote_session && !s->is_far_lod;
	for(int cy = 0; cy < n_cy; cy++)
		refresh = refresh && s->column[cy];
	if(refresh) {
		for(int cy = 0; cy < n_cy; cy++) {
			struct chunk* c = s->column[cy];
			struct chunk* r = replacement[cy];
			uint8_t* blocks = c->blocks;
			const struct far_column_skin* skin = c->far_skin;
			enum chunk_ownership ownership = c->ownership;
			struct column_backing* lease = c->backing;
			struct column_view* shared = c->view;
			c->blocks = r->blocks;
			c->far_skin = r->far_skin;
			c->ownership = r->ownership;
			c->backing = r->backing;
			c->view = r->view;
			r->blocks = blocks;
			r->far_skin = skin;
			r->ownership = ownership;
			r->backing = lease;
			r->view = shared;
			chunk_unref(r); // the staging shell now frees the old storage
		}
	} else {
		world_release_section_chunks(w, s);
		memset(s, 0, sizeof(*s));
		s->is_far_lod = false; // real full-res section (incl. the far->full promotion)
		for(int cy = 0; cy < n_cy; cy++)
			s->column[cy] = replacement[cy];
		w->world_chunk_cache = replacement[n_cy - 1];
	}

	if(!backing)
		for(int cy = 0; cy < n_cy; cy++)
			chunk_load_packed_slice(s->column[cy], cy * CHUNK_SIZE,
				ids, metadata, lighting_sky, lighting_torch);

	memcpy(s->heightmap, heightmap, sizeof(s->heightmap));

	for(int cy = 0; cy < n_cy; cy++) {
		chunk_mark_dirty(s->column[cy]);
		SETTLE(world_settle_observe, SET_DIRTY_INSTALL, s->column[cy], NULL, 0,
			   0, 0, 0);
		s->column[cy]->rebuild_displist = true;
	}

	// H-2 band 0 (2026-09-03 maintainer ruling): see the matching comment in
	// world_build_far_section above -- same rule, full-column install path.
	static const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
	for(int n = 0; n < 4; n++)
		for(int cy = 0; cy < n_cy; cy++) {
			struct chunk* o
				= world_find_chunk(w, (cx + nb[n][0]) * CHUNK_SIZE,
								   cy * CHUNK_SIZE, (cz + nb[n][1]) * CHUNK_SIZE);
			if(o) {
				o->rebuild_displist = true;
				SETTLE(world_settle_observe, SET_DIRTY_NEIGHBOUR, o, NULL, 0, 0,
					   0, 0);
				chunk_mark_urgent(o);
			}
		}

	memcpy(s->grass_i, grass_i, sizeof(s->grass_i));
	memcpy(s->grass_j, grass_j, sizeof(s->grass_j));
	/* REL-MP-FIX3d: the authoritative body supersedes this column's work. */
	lighting_remote_relight_invalidate_column(w->remote_relight, cx, cz);
	return true;
}

bool world_build_full_section(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky,
	const uint8_t* torch, const uint8_t* height, const uint8_t* grass_i,
	const uint8_t* grass_j) {
	return world_build_full_section_impl(w, x, z, ids, metadata, sky, torch,
		height, grass_i, grass_j, NULL);
}

bool world_build_full_section_leased(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky,
	const uint8_t* torch, const uint8_t* height, const uint8_t* grass_i,
	const uint8_t* grass_j, struct column_backing* backing) {
	return world_build_full_section_impl(w, x, z, ids, metadata, sky, torch,
		height, grass_i, grass_j, backing);
}

void world_local_revision(struct world* w, int32_t x, int32_t z,
	struct column_revision revision, uint32_t previous_content, bool install) {
	if(!column_light_enabled()) return;
	struct world_section* s = dict_wsection_get(w->sections,
		SECTION_TO_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	struct chunk* c = s ? s->column[0] : NULL;
	if(!c || c->ownership != LOCAL_SHARED_VIEW || !c->view) return;
	if(install) column_view_bind_revision(c->view, revision);
	else (void)column_view_apply_revision(c->view, revision, previous_content);
}
void world_local_publication(struct world* w, struct column_light_publication* p,
	uint8_t world_index, uint32_t world_generation) {
	column_light_publication_release(w->settled_light);
	w->settled_light = NULL;
	if(!p) return;
	if(!column_light_enabled() || gstate.remote_session || p->dimension != w->dimension) {
		column_light_note_fallback(COLUMN_LIGHT_OWNER);
		column_light_publication_release(p);
		return;
	}
	w->settled_light = p;
	w->settled_light_generation = world_generation;
	w->settled_light_world_index = world_index;
}

bool world_local_shared_capacity_mem1_need(uint32_t columns, uint32_t* out) {
	if(!out || !columns) return false;
	/* M-lib's open table grows to roundpow2(1 + n / 0.7). Using the
	 * deliberately larger roundpow2(2*n + 2) also bounds every intermediate
	 * table while n columns are inserted. A checked resize temporarily owns
	 * old AND new data/index buffers plus collision scratch (world.c above).
	 * The pair capacity is at most buckets+2: M-lib uses 3+upper_limit,
	 * upper_limit < buckets. Replacements and render reallocs may likewise
	 * overlap old and new storage. No light queue is grown by full install.
	 * Keep one replacement column live alongside the existing ring, and add
	 * 128 bytes per allocation as conservative allocator rounding/headroom
	 * beyond chunk_alloc_shell's exact request (which includes its red zone).
	 * The separate 4 MiB admission margin protects other runtime allocations. */
	uint64_t buckets = 16;
	uint64_t wanted = 2ull * columns + 2;
	while(buckets < wanted) buckets *= 2;
	size_t align = __alignof__(struct chunk);
	if(align < sizeof(void*)) align = sizeof(void*);
	uint64_t shell = sizeof(struct chunk) + align + sizeof(void*)
		+ CHUNK_GUARD + 128u;
	uint64_t chunks = ((uint64_t)columns + 1u) * COLUMN_HEIGHT * shell;
	uint64_t dict = 2ull * (buckets + 2) * sizeof(dict_wsection_freelist_ct)
		+ (2ull * buckets + columns) * sizeof(m_indexhash_t) + 3u * 128u;
	uint64_t render = 2ull * WORLD_RENDER_VIEWS * columns * COLUMN_HEIGHT
		* sizeof(struct chunk*) + 2u * WORLD_RENDER_VIEWS * 128u;
	uint64_t total = chunks + dict + render;
	if(total > UINT32_MAX) return false;
	*out = (uint32_t)total;
	return true;
}

/* Only main owns the section/chunk dictionary. This descriptor lasts one resume
 * call and is reset after the flood yield at every pop (height != NULL).
 * No descriptor survives a return to the frame/pacing driver. Server publication cannot replace client shells inside a pop.
 * Keep every shared-view stripe: backing publication/recovery remains concurrent. */
struct world_light_pop {
	struct world* w;
	struct world_section* section;
	struct chunk* center;
	w_coord_t cx, cz, x, y, z;
	unsigned packed_index, column_index;
	/* LIGHT-CLIENT-LOSSLESS root cause: Beta does not relight a cell unless the
	 * chunks within one block of it exist (MetadataChunkBlock.java:43
	 * doChunksNearChunkExist(x, 0, z, 1) -> World.java:323-327
	 * checkChunksExist). seam=true makes the client flood/repair treat such a
	 * cell as raise-only (lighting_client_set_seam_hook): it never LOWERS light
	 * that the missing column may support. It may still raise, because a client
	 * edit writes the packet's light into the cell before the relight (Beta's
	 * setBlock keeps the old light), so a plain skip could leave it dark. A
	 * cache of the 8 neighbour columns' residency (known/ok bit per direction)
	 * serves the pops of one column. Not reproduced: Beta caches the answer per
	 * chunk from the first cell of the box it visits (MetadataChunkBlock.java:
	 * 40-54); here each cell gets its own exact answer. */
	bool seam, seam_have;
	w_coord_t seam_cx, seam_cz;
	uint16_t seam_known, seam_ok;
};

static struct chunk* world_light_resolve(struct world_light_pop* p,
	w_coord_t x, w_coord_t y, w_coord_t z, bool begin) {
	w_coord_t cx = WCOORD_CHUNK_OFFSET(x), cz = WCOORD_CHUNK_OFFSET(z);
	if(!begin && cx == p->cx && cz == p->cz && y >= 0 && y < WORLD_HEIGHT
	   && y / CHUNK_SIZE == p->y / CHUNK_SIZE)
		return p->center;
	struct world_section* s;
	if(!begin && cx == p->cx && cz == p->cz)
		s = p->section;
	else
		s = dict_wsection_get(p->w->sections, SECTION_TO_ID(cx, cz));
	struct chunk* c = s && !s->is_far_lod
		? world_chunk_from_section(p->w, s, y) : NULL;
	if(begin) {
		p->section = s;
		p->center = c;
		p->cx = cx;
		p->cz = cz;
		p->x = x;
		p->y = y;
		p->z = z;
		p->packed_index = W2C_COORD(x)
			+ (W2C_COORD(z) + W2C_COORD(y) * CHUNK_SIZE) * CHUNK_SIZE;
		p->column_index = (W2C_COORD(x) * CHUNK_SIZE + W2C_COORD(z))
			* WORLD_HEIGHT + y;
	}
	return c;
}

/* Is the column at (cx+dx, cz+dz) resident real terrain (not absent, not a
 * far-LOD skin)? Cached per centre column for one resume call: nothing loads
 * or evicts a column inside a call (the flood runs on MAIN between installs). */
static bool world_light_seam_side(struct world_light_pop* p, w_coord_t cx,
	w_coord_t cz, int dx, int dz) {
	if(!p->seam_have || cx != p->seam_cx || cz != p->seam_cz) {
		p->seam_have = true, p->seam_cx = cx, p->seam_cz = cz;
		p->seam_known = p->seam_ok = 0;
	}
	uint16_t bit = (uint16_t)(1u << ((dx + 1) * 3 + (dz + 1)));
	if(!(p->seam_known & bit)) {
		struct world_section* s
			= dict_wsection_get(p->w->sections, SECTION_TO_ID(cx + dx, cz + dz));
		p->seam_known |= bit;
		if(s && !s->is_far_lod)
			p->seam_ok |= bit;
	}
	return (p->seam_ok & bit) != 0;
}

/* Beta's doChunksNearChunkExist(x, 0, z, 1) for one cell: only a cell on its
 * column's edge can reach another column. Without this the paced client flood
 * relaxed edge cells as if the missing neighbour were dark, so a flood that
 * touched a residency or near/far seam (far-LOD sections read as absent) could
 * lower a strip of light about 15 cells deep along the seam. That is the most
 * likely source of the hw1023a 500k-pop cascade during the Dream Journal
 * install: the saved Beta light is self-consistent, yet the flood rewrote so
 * many cells that the column-view override pool filled (op 1024/1024, opf 25
 * at hb 156-157). Unconfirmed on hardware until cfceil stays 0 there. */
static bool world_light_seam_ok(struct world_light_pop* p, w_coord_t x,
	w_coord_t z) {
	int lx = W2C_COORD(x), lz = W2C_COORD(z);
	int dx = lx == 0 ? -1 : (lx == CHUNK_SIZE - 1 ? 1 : 0);
	int dz = lz == 0 ? -1 : (lz == CHUNK_SIZE - 1 ? 1 : 0);
	if(!dx && !dz)
		return true;
	w_coord_t cx = WCOORD_CHUNK_OFFSET(x), cz = WCOORD_CHUNK_OFFSET(z);
	return (!dx || world_light_seam_side(p, cx, cz, dx, 0))
		&& (!dz || world_light_seam_side(p, cx, cz, 0, dz))
		&& (!dx || !dz || world_light_seam_side(p, cx, cz, dx, dz));
}

/* lighting_client_set_seam_hook: the popped cell touches a non-resident column. */
static bool world_light_seam_missing(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z) {
	(void)y;
	struct world_light_pop* p = user;
	return p->seam && !world_light_seam_ok(p, x, z);
}

static bool world_light_get_block(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z, struct block_data* blk, uint8_t* height) {
	struct world_light_pop* p = user;
	struct chunk* c = world_light_resolve(p, x, y, z, height != NULL);
	if(!c)
		return false;
	if(blk)
		*blk = chunk_get_block(c, W2C_COORD(x), W2C_COORD(y), W2C_COORD(z));
	if(height)
		*height = p->section->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE];
	return true;
}

static bool world_light_get_light(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z, uint8_t* sky, uint8_t* torch) {
	struct world_light_pop* p = user;
	struct chunk* c = world_light_resolve(p, x, y, z, false);
	if(!c)
		return false;
	if(c->ownership == LOCAL_SHARED_VIEW) {
		unsigned index = (W2C_COORD(x) * CHUNK_SIZE + W2C_COORD(z))
			* WORLD_HEIGHT + y;
		if(WCOORD_CHUNK_OFFSET(x) == p->cx && WCOORD_CHUNK_OFFSET(z) == p->cz)
			index = p->column_index + (x - p->x) * CHUNK_SIZE * WORLD_HEIGHT
				+ (z - p->z) * WORLD_HEIGHT + (y - p->y);
		column_view_get_light(c->view, index, sky, torch);
	} else {
		unsigned index = W2C_COORD(x)
			+ (W2C_COORD(z) + W2C_COORD(y) * CHUNK_SIZE) * CHUNK_SIZE;
		if(c == p->center)
			index = p->packed_index + (x - p->x) + (z - p->z) * CHUNK_SIZE
				+ (y - p->y) * CHUNK_SIZE * CHUNK_SIZE;
		/* Same X-paired five-byte layout as chunk_get_block_voxel. */
		uint8_t light = c->blocks[index / 2 * 5 + index % 2 + 2];
		*sky = light & 15;
		*torch = light >> 4;
	}
	return true;
}

static void world_light_set_light(void* user, w_coord_t x, w_coord_t y,
	w_coord_t z, uint8_t light) {
	struct world_light_pop* p = user;
	if(!p->center)
		return;
	/* Preserve the old setter's world_find_chunk cache side effect as well. */
	p->w->world_chunk_cache = p->center;
	chunk_set_light(p->center, W2C_COORD(x), W2C_COORD(y), W2C_COORD(z), light);
}

/* REL-MP-FIX3d, REMOTE SESSIONS ONLY: seeds go to the FIX3c2 bounded pending
 * bitmap (lighting.c) instead of the growing lighting_updates stack. Local mode
 * never attaches a reservation, so its stacks, order and goldens are untouched. */
static bool world_remote_column_loaded(void* user, w_coord_t cx, w_coord_t cz) {
	const struct world_light_pop* pop = user;
	struct world* w = pop->w;
	struct world_section* s = dict_wsection_get(w->sections, SECTION_TO_ID(cx, cz));
	return s && !s->is_far_lod;
}

static uint32_t s_remote_relight_no_slot; /* seeds kept on the legacy stack */
static bool world_remote_relight_admit(struct world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z) {
	if(!gstate.remote_session)
		return false;
	/* A failed reservation is a failed load (readiness closed, discarded
	 * by the next login's reset): its seeds never reach the legacy stack. */
	if(w->remote_relight_failed)
		return true;
	if(!w->remote_relight)
		return false;
	struct world_light_pop pop = {.w = w};
	if(lighting_remote_relight_admit(w->remote_relight, x, y, z,
			world_remote_column_loaded, &pop) != LIGHTING_REMOTE_NO_SLOT)
		return true;
	/* Slots cover every loaded column (fix3c2-ledger.md), so this should not
	 * happen; if it does the seed takes the legacy path: nothing is dropped. */
	if(!s_remote_relight_no_slot++)
		printf("[world] remote relight slots exhausted; seed kept on legacy stack\n");
	return false;
}

uint32_t world_remote_relight_no_slots(void) {
	return s_remote_relight_no_slot;
}

bool world_remote_relight_begin(struct world* w, void* storage, size_t bytes,
								size_t slots) {
	assert(w);
	struct lighting_remote_relight* r
		= lighting_remote_relight_init(storage, bytes, slots);
	if(r)
		w->remote_relight = r;
	w->remote_relight_failed = !r && !w->remote_relight;
	return r != NULL;
}

void world_remote_relight_end(struct world* w) {
	assert(w);
	w->remote_relight = NULL;
	w->remote_relight_failed = false;
}

bool world_remote_relight_pending(struct world* w) {
	assert(w);
	return lighting_remote_relight_active(w->remote_relight);
}

static enum column_light_fallback world_light_certificate_validate(struct world* w,
	const struct column_light_publication* p, size_t budget) {
	if(!column_light_enabled() || gstate.remote_session || !p->owner
	   || p->dimension != w->dimension) return COLUMN_LIGHT_OWNER;
	if(!p->world_generation || p->world_generation != w->settled_light_generation
	   || p->world_index != w->settled_light_world_index) return COLUMN_LIGHT_VERSION;
	if(!lighting_flood_queue_empty(&w->lighting_flood_queue)) return COLUMN_LIGHT_ACTIVE;
	if(stack_size(&w->lighting_updates) != 1) return COLUMN_LIGHT_SEEDS;
	if(!p->pops || p->pops > budget) return COLUMN_LIGHT_BUDGET;
	if(!p->mutation_epoch || p->mutation_epoch != column_light_mutation_epoch()
	   || p->presentation_epoch != column_view_presentation_epoch()
	   || column_view_recovery_pending()) return COLUMN_LIGHT_VERSION;
	struct world_modification_entry source;
	stack_at(&w->lighting_updates, &source, 0);
	if(source.x != p->x || source.y != p->y || source.z != p->z)
		return COLUMN_LIGHT_SEEDS;
	for(unsigned n = 0; n < p->dependencies; n++) {
		const struct column_light_dependency* d = &p->dependency[n];
		struct world_section* s = dict_wsection_get(w->sections, SECTION_TO_ID(d->x, d->z));
		if(!d->backing) {
			if(s) return COLUMN_LIGHT_RESIDENCY;
			continue;
		}
		if(!s || s->is_far_lod || d->revision.owner != p->owner
		   || d->backing->world_instance != p->owner
		   || d->backing->dimension != w->dimension) return COLUMN_LIGHT_RESIDENCY;
		struct column_view* view = s->column[0] ? s->column[0]->view : NULL;
		for(unsigned cy = 0; cy < COLUMN_HEIGHT; cy++) {
			struct chunk* c = s->column[cy];
			if(!c || c->ownership != LOCAL_SHARED_VIEW || c->view != view
			   || c->backing != d->backing) return COLUMN_LIGHT_RESIDENCY;
		}
		if(!view || !column_view_revision_matches(view, &d->revision))
			return COLUMN_LIGHT_VERSION;
		if(!column_view_content_matches(view, s->heightmap)) return COLUMN_LIGHT_BYTES;
	}
	for(unsigned n = 0; n < p->cells; n++) {
		const struct column_light_cell* cell = &p->read[n];
		struct world_section* s = cell->y >= 0 && cell->y < WORLD_HEIGHT
			? dict_wsection_get(w->sections, SECTION_TO_ID(WCOORD_CHUNK_OFFSET(cell->x),
				WCOORD_CHUNK_OFFSET(cell->z))) : NULL;
		struct chunk* c = s && !s->is_far_lod ? s->column[cell->y / CHUNK_SIZE] : NULL;
		if((c != NULL) != cell->exists) return COLUMN_LIGHT_RESIDENCY;
		if(c) {
			struct block_data b = chunk_get_block(c, W2C_COORD(cell->x),
				W2C_COORD(cell->y), W2C_COORD(cell->z));
			uint8_t height = s->heightmap[W2C_COORD(cell->x) + W2C_COORD(cell->z) * CHUNK_SIZE];
			if(b.type != cell->block.type || b.metadata != cell->block.metadata
			   || b.sky_light != cell->block.sky_light || b.torch_light != cell->block.torch_light
			   || height != cell->height) return COLUMN_LIGHT_BYTES;
			if(cell->x == p->x && cell->y == p->y && cell->z == p->z
			   && (source.blk.type != b.type || source.blk.metadata != b.metadata))
				return COLUMN_LIGHT_BYTES;
		}
	}
	/* Linearization point: MAIN's presentation is unchanged during validation;
	 * server COW preserves it after this acquire, including during the setter. */
	if(p->mutation_epoch != column_light_mutation_epoch()
	   || p->presentation_epoch != column_view_presentation_epoch()
	   || column_view_recovery_pending()) return COLUMN_LIGHT_VERSION;
	return COLUMN_LIGHT_FALLBACKS;
}
static size_t world_light_certificate_consume(struct world* w, size_t budget) {
	struct column_light_publication* p = w->settled_light;
	if(!p) return 0;
	w->settled_light = NULL;
	ptime_t start = time_get();
	enum column_light_fallback reason = world_light_certificate_validate(w, p, budget);
	size_t avoided = 0;
	if(reason == COLUMN_LIGHT_FALLBACKS) {
		struct world_modification_entry source;
		stack_pop(&w->lighting_updates, &source);
		g9_probe_light_pop(w, (unsigned)stack_size(&w->lighting_updates) + 1);
		struct world_light_pop pop = {.w = w};
		struct block_data b;
		uint8_t height;
		if(world_light_get_block(&pop, source.x, source.y, source.z, &b, &height)) {
			/* Atomically install the already-equal immutable light via the exact
			 * baseline forced setter: same dirty and padded-border invalidations. */
			world_light_set_light(&pop, source.x, source.y, source.z,
				(uint8_t)(b.sky_light | b.torch_light << 4));
			avoided = p->pops;
			column_light_note_hit(p->pops);
		}
	} else column_light_note_fallback(reason);
	column_light_note_validation((uint32_t)time_diff_us(start, time_get()));
	column_light_publication_release(p);
	return avoided;
}

/* --- LIGHT-CLIENT-LOSSLESS: client light column repair ---
 *
 * A client flood that parks (500k per-flood ceiling, or a frontier/seed grow
 * that failed on a low heap) hands its frontier to world_light_repair_mark
 * column by column (lighting_flood_queue_spill), which frees the frontier.
 * The repair then sweeps each marked column with lighting_relax_cell, one cell
 * at a time (no queue: O(1) memory), alternating forward/backward passes,
 * until a whole pass changes nothing. A change on a column edge marks the
 * column across that edge. Invariant (the flood's own): every cell whose light
 * may disagree with its neighbours is in the flood frontier OR in a marked
 * column OR ahead of / behind the cursor of a pass that will repeat. When the
 * frontier is empty and no column is marked, every relaxable cell holds the
 * unique fixed point of the light equation, i.e. the field a complete flood
 * reaches (tests/test_lighting_client_budget_equiv.c, lossless cases). Cost is
 * charged to the same per-frame pop budget: 1 per relaxed cell, 1 per empty
 * 16x16x16 chunk skipped, 1 per 16 sections probed when picking a column. */
#define LIGHT_REPAIR_CELLS (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
static uint32_t s_light_repairs_done;

static void world_light_repair_reset(struct world* w) {
	w->light_repair_hint = 0;
	w->light_repair_active = false;
	w->light_repair_cx = w->light_repair_cz = 0;
	w->light_repair_index = 0;
	w->light_repair_pass = 0;
	w->light_repair_changed = false;
	w->light_spill_active = false;
}

void world_light_repair_mark(struct world* w, w_coord_t cx, w_coord_t cz) {
	assert(w);
	struct world_section* s = dict_wsection_get(w->sections, SECTION_TO_ID(cx, cz));
	// Absent / far-LOD columns are never relit (the flood skips them too); a
	// later full install arrives with final server light.
	if(!s || s->is_far_lod)
		return;
	if(w->light_repair_active && cx == w->light_repair_cx
	   && cz == w->light_repair_cz) {
		w->light_repair_changed = true; // being swept: make this pass repeat
		return;
	}
	if(!s->light_repair) {
		s->light_repair = true;
		w->light_repair_hint++;
	}
}

static void world_light_repair_mark_cb(void* user, w_coord_t cx, w_coord_t cz) {
	world_light_repair_mark(user, cx, cz);
}

/* A FORCED relight of cell (x, z) that cannot be queued: mark its column AND
 * the columns of its side neighbours, as the forced seed would push them even
 * when the cell's own light does not change (stone sealing a lit cross-column
 * tunnel: the cell already holds 0, so no edge change would ever mark the
 * column across the seal). */
static void world_light_repair_mark_cell(struct world* w, w_coord_t x,
	w_coord_t z) {
	w_coord_t cx = WCOORD_CHUNK_OFFSET(x), cz = WCOORD_CHUNK_OFFSET(z);
	world_light_repair_mark(w, cx, cz);
	if(WCOORD_CHUNK_OFFSET(x - 1) != cx) world_light_repair_mark(w, cx - 1, cz);
	if(WCOORD_CHUNK_OFFSET(x + 1) != cx) world_light_repair_mark(w, cx + 1, cz);
	if(WCOORD_CHUNK_OFFSET(z - 1) != cz) world_light_repair_mark(w, cx, cz - 1);
	if(WCOORD_CHUNK_OFFSET(z + 1) != cz) world_light_repair_mark(w, cx, cz + 1);
}

uint32_t world_light_repair_pending(const struct world* w) {
	assert(w);
	return w->light_repair_hint + (w->light_repair_active ? 1u : 0u);
}

uint32_t world_light_repairs_done(void) {
	return s_light_repairs_done;
}

static bool world_light_repair_busy(const struct world* w) {
	return w->light_repair_active || w->light_repair_hint;
}

/* Upper bound of what world_light_repair_pick charges (1 + probes / 16, at
 * most one probe per section), checked against the budget BEFORE the scan.
 * Clamped to one frame's budget so a pathological dictionary still fits a
 * fresh frame instead of never fitting. */
static size_t world_light_repair_pick_cost(const struct world* w) {
	size_t cost = 1 + dict_wsection_size(w->sections) / 16;
	return cost < LIGHTING_CLIENT_POPS_PER_FRAME ? cost
		: LIGHTING_CLIENT_POPS_PER_FRAME;
}

/* Pick the next marked column (clearing its flag: the cursor owns it now).
 * Returns the budget it cost (<= world_light_repair_pick_cost); leaves
 * light_repair_active false when none. */
static size_t world_light_repair_pick(struct world* w) {
	const size_t cap = world_light_repair_pick_cost(w);
	size_t probes = 0;
	dict_wsection_it_t it;
	for(dict_wsection_it(it, w->sections); !dict_wsection_end_p(it);
		dict_wsection_next(it)) {
		probes++;
		struct world_section* s = &dict_wsection_ref(it)->value;
		if(!s->light_repair)
			continue;
		s->light_repair = false;
		if(w->light_repair_hint)
			w->light_repair_hint--;
		if(s->is_far_lod)
			continue; // demoted since it was marked: nothing to relight
		int64_t id = dict_wsection_ref(it)->key;
		w->light_repair_active = true;
		w->light_repair_cx = (w_coord_t)(int32_t)(uint32_t)(id & 0xFFFFFFFF);
		w->light_repair_cz = (w_coord_t)(int32_t)(uint32_t)((uint64_t)id >> 32);
		w->light_repair_index = 0;
		w->light_repair_pass = 0;
		w->light_repair_changed = false;
		return 1 + probes / 16 < cap ? 1 + probes / 16 : cap;
	}
	w->light_repair_hint = 0; // the remaining flags left with their sections
	return 1 + probes / 16 < cap ? 1 + probes / 16 : cap;
}

/* Spend at most `budget` on the repair; returns the budget used. Picking a
 * new column first checks its worst-case cost against `budget`: 0 = not
 * affordable now, nothing done (the reserved slice at the start of the next
 * frame takes it). Otherwise >= 1, so the caller's loop progresses. */
static size_t world_light_repair_step(struct world* w, bool nether, size_t budget) {
	size_t used = 0;
	if(!w->light_repair_active) {
		if(world_light_repair_pick_cost(w) > budget)
			return 0;
		used += world_light_repair_pick(w);
		if(!w->light_repair_active || used >= budget)
			return used;
	}
	w_coord_t cx = w->light_repair_cx, cz = w->light_repair_cz;
	struct world_section* s = dict_wsection_get(w->sections, SECTION_TO_ID(cx, cz));
	if(!s || s->is_far_lod) {
		// Unloaded or demoted mid-repair: nothing left to relight here.
		w->light_repair_active = false;
		return used + 1;
	}
	struct world_light_pop pop = {.w = w, .seam = true};
	while(used < budget) {
		if(w->light_repair_index >= LIGHT_REPAIR_CELLS) {
			if(w->light_repair_changed) { // not settled yet: another pass
				w->light_repair_pass++;
				w->light_repair_index = 0;
				w->light_repair_changed = false;
				continue;
			}
			w->light_repair_active = false; // a clean pass: fixed point
			s_light_repairs_done++;
			break;
		}
		uint32_t i = w->light_repair_index;
		uint32_t cell = (w->light_repair_pass & 1u) ? LIGHT_REPAIR_CELLS - 1 - i : i;
		unsigned cy = cell / (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE);
		used++;
		if(!s->column[cy]) { // empty chunk: no cell to relight
			uint32_t in_chunk = cell % (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE);
			uint32_t left = (w->light_repair_pass & 1u)
				? in_chunk + 1 : CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE - in_chunk;
			w->light_repair_index += left;
			continue;
		}
		w->light_repair_index++;
		int lx = cell % CHUNK_SIZE, lz = (cell / CHUNK_SIZE) % CHUNK_SIZE;
		w_coord_t x = cx * CHUNK_SIZE + lx, z = cz * CHUNK_SIZE + lz;
		w_coord_t y = (w_coord_t)(cell / (CHUNK_SIZE * CHUNK_SIZE));
		if(lighting_relax_cell(nether, x, y, z, world_light_get_block,
				world_light_get_light, world_light_set_light, &pop) != 1)
			continue;
		w->light_repair_changed = true;
		// A changed edge cell feeds the column across that edge.
		if(lx == 0) world_light_repair_mark(w, cx - 1, cz);
		if(lx == CHUNK_SIZE - 1) world_light_repair_mark(w, cx + 1, cz);
		if(lz == 0) world_light_repair_mark(w, cx, cz - 1);
		if(lz == CHUNK_SIZE - 1) world_light_repair_mark(w, cx, cz + 1);
		/* world_light_repair_mark of THIS column never happens here (only
		 * neighbours), and marking cannot insert/erase sections, so `s` stays
		 * valid for the whole step. */
	}
	return used;
}

/* A parked client flood: move up to `budget` entries of its intact frontier
 * into column repairs (the frontier storage is freed once empty). Returns the
 * entries moved, each charged as one budget unit (>= 1, so the caller's loop
 * progresses). Nothing is dropped. */
static size_t world_light_spill(struct world* w, size_t budget) {
	size_t n = lighting_flood_queue_spill(&w->lighting_flood_queue,
		world_light_repair_mark_cb, w, budget);
	if(lighting_flood_queue_empty(&w->lighting_flood_queue))
		w->light_spill_active = false;
	return n ? n : 1;
}

/* Reserved repair slice (fix-r2): while repairs wait, each frame gives them
 * up to this much budget FIRST, so sustained unrelated relights can never
 * starve them. They still take the whole budget once nothing else waits. */
#define LIGHT_REPAIR_RESERVE 256u

/* The slice to hand world_light_repair_step: `slice`, widened to the pick's
 * worst-case cost when a new column must be picked, never above `budget`. */
static size_t world_light_repair_slice(const struct world* w, size_t slice,
	size_t budget) {
	if(!w->light_repair_active) {
		size_t cost = world_light_repair_pick_cost(w);
		if(cost > slice)
			slice = cost;
	}
	return slice < budget ? slice : budget;
}

// Per-frame client relight pop budget. world_update_lighting runs on the RENDER thread,
// which cannot advance the main-loop watchdog heartbeat mid-flood -- so an unbudgeted
// multi-second surface skylight flood froze the core (the far-ring freeze, watchdog
// row1=WDM_LIGHTING; Broadway-speed-bound, so Dolphin couldn't reproduce it). This caps
// the relight work per frame: a giant flood is PACED across frames (its BFS frontier
// persists in w->lighting_flood_queue) instead of blocking one frame for seconds. Pure
// pacing -> byte-exact final light field (tests/test_lighting_client_budget_equiv.c);
// only the settle RATE changes. ~4096 keeps ms_light to a few ms on Broadway.
//
// ACCEPTED RENDER-ONLY DEVIATION (chunk replaced mid-flood): because a flood now spans
// frames, a section can be torn down + re-installed (far<->full promotion / chunk reload
// via world_build_full_section, which writes server-authoritative light directly) WHILE a
// flood frontier sits in it. EVICTION-to-gone is handled (the lighting_flood get_block
// guard skips it); a REPLACE leaves the section resident, so the paced flood may relight a
// few freshly-installed cells from its frontier. This is RENDER LIGHT ONLY -- client light
// is never saved (NBT) nor read back into simulation, the server region is untouched, so
// no faithfulness VALUE drifts -- and it SELF-HEALS: the client flood uses the same
// lighting_flood core over the same block data as the server, so those cells converge to
// the exact value the server sent once the flood completes. Worst case is a transient seam
// flicker near a just-streamed boundary. Documented, not code-fixed: dropping the queue on
// every section rebuild would stall convergence during normal far-ring streaming.
static size_t s_lighting_driver_last_work = 0;
static size_t s_lighting_driver_max_work = 0;
static size_t s_lighting_driver_last_stale_drops = 0;
static size_t s_lighting_driver_max_stale_drops = 0;

static void world_lighting_driver_record(size_t work, size_t stale_drops) {
	s_lighting_driver_last_work = work;
	s_lighting_driver_last_stale_drops = stale_drops;
	if(work > s_lighting_driver_max_work)
		s_lighting_driver_max_work = work;
	if(stale_drops > s_lighting_driver_max_stale_drops)
		s_lighting_driver_max_stale_drops = stale_drops;
}

void world_lighting_driver_diag_reset(void) {
	s_lighting_driver_last_work = 0;
	s_lighting_driver_max_work = 0;
	s_lighting_driver_last_stale_drops = 0;
	s_lighting_driver_max_stale_drops = 0;
}

size_t world_lighting_driver_last_work(void) {
	return s_lighting_driver_last_work;
}

size_t world_lighting_driver_max_work(void) {
	return s_lighting_driver_max_work;
}

size_t world_lighting_driver_last_stale_drops(void) {
	return s_lighting_driver_last_stale_drops;
}

size_t world_lighting_driver_max_stale_drops(void) {
	return s_lighting_driver_max_stale_drops;
}

static void g9_client_light_mark(struct world* w, unsigned region, bool begin, size_t work) {
	if(!g9_probe_main_enabled()) {
		g9_capture_main(region, begin, (uint32_t)work);
		return;
	}
	unsigned depth = (unsigned)stack_size(&w->lighting_updates);
	bool flood = !lighting_flood_queue_empty(&w->lighting_flood_queue);
	g9_probe_main_mark(region, begin, (uint32_t)work,
		depth + (uint32_t)stack_size(&w->lighting_flood_queue),
		g9_probe_light_age(w, depth, flood));
}

void world_update_lighting(struct world* w) {
	if(thread_g9_lease_defer()) return; /* queued presentation work retained */
	g9_client_light_mark(w, 3, true, 0);
	SETTLE(world_settle_observe, SET_LIGHT_BEGIN, NULL, NULL, 0, 0, 0, 0);
	assert(w);

	const bool nether = (w->dimension == WORLD_DIM_NETHER);
	size_t budget = LIGHTING_CLIENT_POPS_PER_FRAME;
	size_t work = 0;
	size_t stale_drops = 0;
	if(w->settled_light) {
		size_t certified = world_light_certificate_consume(w, budget);
		/* Charge the original pop budget even though the kernel was avoided:
		 * no later seed/mesh publication advances to an earlier frame. */
		work += certified;
		budget -= certified;
	}

	/* LIGHT-CLIENT-LOSSLESS (fix-r2 starvation bound): waiting repairs take a
	 * reserved slice FIRST every frame, so a sustained stream of unrelated
	 * relights can never starve them. Only parked/OOM work is ever marked, so
	 * the success path (no repair pending) is untouched. */
	if(budget > 0 && world_light_repair_busy(w)) {
		size_t did = world_light_repair_step(w, nether,
			world_light_repair_slice(w, LIGHT_REPAIR_RESERVE, budget));
		(void)thread_g9_handoff_dispatch(0, NULL);
		work += did;
		budget -= did;
		thread_main_pacing_checkpoint_at(THREAD_PACING_LIGHTING);
	}

	/* REL-MP-FIX3d: the remote bounded relight spends this frame's budget
	 * first; every call charges >= 1, so the loop is bounded by the budget. */
	while(budget > 0 && gstate.remote_session
		  && lighting_remote_relight_active(w->remote_relight)) {
		if(thread_g9_lease_defer()) break;
		size_t slice = (thread_main_pacing_enabled() || thread_g9_lease_active())
			&& budget > 256 ? 256 : budget, did = 0;
		struct world_light_pop pop = {.w = w};
		lighting_remote_relight_drain(w->remote_relight, nether, slice, &did,
			world_light_get_block, world_light_get_light, world_light_set_light,
			world_remote_column_loaded, &pop);
		did = did ? did : 1;
		(void)thread_g9_handoff_dispatch(0, NULL);
		work += did;
		budget -= did;
		thread_main_pacing_checkpoint_at(THREAD_PACING_LIGHTING);
	}

	// Spend this frame's pop budget: resume an in-flight flood first, then start as many
	// fresh deferred relights as fit. A single huge flood spans frames (paced, no
	// freeze); a burst of small relights drains many per frame (no backlog).
	while(budget > 0) {
		if(thread_g9_lease_defer()) break;
		if(w->light_spill_active) {
			/* LIGHT-CLIENT-LOSSLESS (fix-r2): a parked frontier moves into
			 * column repairs a budgeted slice at a time, every entry charged,
			 * under the same slice/pacing contract as the flood below. */
			size_t slice = (thread_main_pacing_enabled() || thread_g9_lease_active())
				&& budget > 256 ? 256 : budget;
			size_t did = world_light_spill(w, slice);
			(void)thread_g9_handoff_dispatch(0, NULL);
			work += did;
			budget = (did >= budget) ? 0 : (budget - did);
			thread_main_pacing_checkpoint_at(THREAD_PACING_LIGHTING);
			continue;
		}
		if(lighting_flood_queue_empty(&w->lighting_flood_queue)) {
			if(stack_empty(&w->lighting_updates) && world_light_repair_busy(w)) {
				/* LIGHT-CLIENT-LOSSLESS: parked work, once no edit waits. Same
				 * slice/pacing contract as the flood below. */
				size_t slice = (thread_main_pacing_enabled() || thread_g9_lease_active())
					&& budget > 256 ? 256 : budget;
				size_t did = world_light_repair_step(w, nether,
					world_light_repair_slice(w, slice, budget));
				if(!did)
					break; // the next column's pick does not fit this frame
				(void)thread_g9_handoff_dispatch(0, NULL);
				work += did;
				budget = (did >= budget) ? 0 : (budget - did);
				thread_main_pacing_checkpoint_at(THREAD_PACING_LIGHTING);
				continue;
			}
			if(stack_empty(&w->lighting_updates)) {
				world_lighting_driver_record(work, stale_drops);
				if(world_settle_observe) {
					struct settle_light sl
						= {(uint32_t)work, (uint32_t)stale_drops,
						   (uint32_t)stack_size(&w->lighting_updates),
						   (uint32_t)stack_size(&w->lighting_flood_queue),
						   budget == 0};
					SETTLE_DETAIL(world_settle_observe, SET_LIGHT_END, NULL,
								  &sl);
				}
				g9_client_light_mark(w, 3, false, work);
				return; // nothing in flight and nothing queued
			}

			struct world_modification_entry source;
			g9_probe_light_pop(w, (unsigned)stack_size(&w->lighting_updates));
			stack_pop(&w->lighting_updates, &source);

			// Drop entries whose COLUMN has since been UNLOADED. A burst (explosion: up
			// to ~512 CRPC_SET_BLOCKs) followed by eviction (death->respawn evicts the
			// whole death area in one tick) leaves stale entries referencing columns the
			// server already unloaded -- and world_set_block(...) CREATES the column if
			// absent, resurrecting it as a permanent orphan chunk. A deferred entry only
			// updates RESIDENT render state; a future re-install arrives with final
			// blocks+light, so dropping is correct, not lossy. NOTE: test the SECTION,
			// not the chunk -- a resident column with a not-yet-materialized y-chunk
			// (placing into empty air) must still go through world_set_block's creation.
			if(!dict_wsection_get(w->sections,
								  SECTION_TO_ID(WCOORD_CHUNK_OFFSET(source.x),
												WCOORD_CHUNK_OFFSET(source.z)))) {
				// Stale -> drop. CHARGE the budget for the dict probe so this branch is
				// paced too: a pathological backlog (mass eviction marking thousands of
				// queued entries stale in one tick) is then drained at most
				// LIGHTING_CLIENT_POPS_PER_FRAME/frame like real relights, instead of
				// O(backlog) uncapped hash lookups in a single frame. Makes EVERY loop
				// iteration consume >=1 budget -> the loop runs at most `budget` times.
				budget--;
				work++;
				stale_drops++;
				if((work & 255u) == 0) {
					(void)thread_g9_handoff_dispatch(0, NULL);
					thread_main_pacing_checkpoint_at(THREAD_PACING_LIGHTING);
				}
				continue;
			}

			// [D2] The block was already written synchronously in world_set_block; here
			// we only run the deferred relight. Dimension-aware skylight, mirroring the
			// server relight (server passes WORLD_DIM_NETHER): the Nether has NO skylight.
			if(!lighting_flood_seed(&w->lighting_flood_queue, source.x, source.y,
									source.z, true)) {
				/* LIGHT-CLIENT-LOSSLESS: no room for even the seed. The column
				 * repair re-relaxes it instead; charge the attempt. */
				lighting_client_note_oom_park();
				world_light_repair_mark_cell(w, source.x, source.z);
				budget--;
				work++;
				continue;
			}
		}

		/* T2 pacing preserves this frame's total pop budget and write order.
		 * The resume call has returned all column locks before sleeping. */
		size_t slice = (thread_main_pacing_enabled() || thread_g9_lease_active())
			&& budget > 256 ? 256 : budget;
		g9_client_light_mark(w, 4, true, 0);
		struct world_light_pop pop = {.w = w, .seam = true};
		size_t did = lighting_flood_resume_light(&w->lighting_flood_queue, nether, slice,
										   world_light_get_block, world_light_get_light,
										   world_light_set_light, &pop);
		g9_client_light_mark(w, 4, false, did);
		/* LIGHT-CLIENT-LOSSLESS: a parked flood (500k ceiling or a failed
		 * frontier grow) keeps its frontier; the iterations that follow spill
		 * it into column repairs, a charged slice at a time (fix-r2), and
		 * free the frontier memory once empty. Charge >= 1 so the loop
		 * progresses. */
		if(lighting_flood_last_status() != LIGHTING_DRAIN_DONE) {
			w->light_spill_active = true;
			if(!did)
				did = 1;
		}
		/* The real flood returned every column stripe; no RPC, input or GPU
		 * retirement may run here. Arm/validate the same production cursor. */
		(void)thread_g9_handoff_dispatch(0, NULL);
		work += did;
		budget = (did >= budget) ? 0 : (budget - did);
		thread_main_pacing_checkpoint_at(THREAD_PACING_LIGHTING);
	}
	g9_client_light_mark(w, 3, false, work);
	world_lighting_driver_record(work, stale_drops);
	if(world_settle_observe) {
		struct settle_light sl
			= {(uint32_t)work, (uint32_t)stale_drops,
			   (uint32_t)stack_size(&w->lighting_updates),
			   (uint32_t)stack_size(&w->lighting_flood_queue), budget == 0};
		SETTLE_DETAIL(world_settle_observe, SET_LIGHT_END, NULL, &sl);
	}
}

struct chunk* world_find_chunk_neighbour(struct world* w, struct chunk* c,
										 enum side s) {
	assert(w && c);

	int x, y, z;
	blocks_side_offset(s, &x, &y, &z);

	if(y + c->y / CHUNK_SIZE < 0
	   || y + c->y / CHUNK_SIZE >= WORLD_HEIGHT / CHUNK_SIZE)
		return NULL;

	struct world_section* res = dict_wsection_get(
		w->sections,
		SECTION_TO_ID(x + c->x / CHUNK_SIZE, z + c->z / CHUNK_SIZE));

	return res ? res->column[y + c->y / CHUNK_SIZE] : NULL;
}

struct chunk* world_chunk_from_section(struct world* w, struct world_section* s,
									   w_coord_t y) {
	assert(w);
	return (y >= 0 && y < WORLD_HEIGHT && s) ? s->column[y / CHUNK_SIZE] : NULL;
}

struct chunk* world_find_chunk(struct world* w, w_coord_t x, w_coord_t y,
							   w_coord_t z) {
	assert(w);

	if(y < 0 || y >= WORLD_HEIGHT)
		return NULL;

	int cx = WCOORD_CHUNK_OFFSET(x);
	int cy = y / CHUNK_SIZE;
	int cz = WCOORD_CHUNK_OFFSET(z);

	if(w->world_chunk_cache && cx == w->world_chunk_cache->x / CHUNK_SIZE
	   && cy == w->world_chunk_cache->y / CHUNK_SIZE
	   && cz == w->world_chunk_cache->z / CHUNK_SIZE)
		return w->world_chunk_cache;

	struct world_section* res
		= dict_wsection_get(w->sections, SECTION_TO_ID(cx, cz));

	if(res)
		w->world_chunk_cache = res->column[cy];

	return res ? res->column[cy] : NULL;
}

void world_preload(struct world* w,
				   void (*progress)(struct world* w, float percent)) {
	assert(w);

	dict_wsection_it_t it;
	dict_wsection_it(it, w->sections);

	size_t total = dict_wsection_size(w->sections);
	size_t count = 0;

	while(!dict_wsection_end_p(it)) {
		struct world_section* s = &dict_wsection_ref(it)->value;
		for(size_t k = 0; k < COLUMN_HEIGHT; k++) {
			if(s->column[k])
				chunk_check_built(s->column[k]);
		}

		if(progress)
			progress(w, (float)count / (float)total);
		count++;
		dict_wsection_next(it);
	}
}

bool world_block_intersection(struct world* w, struct ray* r, w_coord_t x,
							  w_coord_t y, w_coord_t z, enum side* s) {
	assert(w && r && s);

	struct block_data blk = world_get_block(w, x, y, z);

	if(blocks[blk.type]) {
		struct block_info blk_info = (struct block_info) {
			.block = &blk,
			.x = x,
			.y = y,
			.z = z,
			.get_piston_moving = world_client_piston_moving_lookup,
		};

		size_t count = blocks[blk.type]->getBoundingBox(&blk_info, false, NULL);
		if(count > 0) {
			struct AABB bbox[count];
			blocks[blk.type]->getBoundingBox(&blk_info, false, bbox);

			for(size_t k = 0; k < count; k++) {
				aabb_translate(bbox + k, x, y, z);

				if(aabb_intersection_ray(bbox + k, r, s))
					return true;
			}
		}
	}

	return false;
}

void world_pre_render(struct world* w, struct camera* camera, mat4 view) {
	assert(w && camera && view);

	ilist_chunks_init(w->render);
	world_bfs(w, w->render, camera->x, camera->y, camera->z, camera->frustum_planes);

	ilist_chunks_it_t it;
	ilist_chunks_it(it, w->render);

	const size_t cap = world_ilist_walk_cap(w);
	size_t steps = 0;

	while(!ilist_chunks_end_p(it)) {
		// Fogged chunks move BEHIND the cursor (push_front), so a healthy list is
		// visited exactly once per entry; only a broken chain can exceed the cap. The
		// bail itself is clean -- it mutates nothing and takes/drops no reference, and
		// the list it leaves is the one it was handed. It is NOT a full guarantee for
		// this walk though: _remove followed by push_front re-links the node m-lib has
		// already cached an it->next for, so on a corrupt chain the cursor can reach a
		// stale NULL and fault before ever counting to the cap. Closing that needs the
		// partition restructured (and world_pre_render re-inits w->render before it
		// walks, so the corruption cannot be injected from a test as things stand).
		if(++steps > cap) {
			s_ilist_overrun++;
			return;
		}

		struct chunk* c = ilist_chunks_ref(it);
		bool has_fog = !FOG_DIST_LESS(c, camera->x, camera->z, FOG_DIST_NO_EFFECT);

		chunk_pre_render(c, view, has_fog);

		if(has_fog) {
			ilist_chunks_remove(w->render, it);
			ilist_chunks_push_front(w->render, c);
		} else {
			ilist_chunks_next(it);
		}
	}
}

bool world_pre_render_views(struct world* w, struct camera* const* cameras,
							size_t count) {
	if(!w || !cameras || !count || count > WORLD_RENDER_VIEWS) return false;
	for(size_t v = 0; v < count; v++) if(!cameras[v]) return false;
	/* A resident section has at most COLUMN_HEIGHT chunks. Reserve before BFS:
	 * failure cannot strand a partially captured list's reference tokens. */
	size_t sections = dict_wsection_size(w->sections);
	if(sections > SIZE_MAX / COLUMN_HEIGHT / sizeof(struct chunk*)) return false;
	size_t capacity = sections * COLUMN_HEIGHT;
	for(size_t v = 0; v < count; v++) {
		struct world_render_view* result = &w->render_views[v];
		if(result->capacity < capacity) {
			struct chunk** grown = realloc(result->chunks, capacity * sizeof(*grown));
			if(!grown) return false;
			result->chunks = grown;
			result->capacity = capacity;
		}
	}
	for(unsigned v = 0; v < WORLD_RENDER_VIEWS; v++) w->render_views[v].count = 0;
	for(size_t v = 0; v < count; v++) {
		world_pre_render(w, cameras[v], cameras[v]->view);
		struct world_render_view* result = &w->render_views[v];
		ilist_chunks_it_t it;
		ilist_chunks_it(it, w->render);
		while(!ilist_chunks_end_p(it)) {
			/* BFS visits each resident chunk once; its existing guard reports
			 * corruption. Never truncate a healthy view to a fixed budget. */
			if(result->count >= result->capacity) {
				s_ilist_overrun++;
				/* A corrupt list cannot be safely unref'd by visit. Match the
				 * existing handoff guard: abandon tokens, report the fault. */
				world_pre_render_clear(w);
				return false;
			}
			result->chunks[result->count++] = ilist_chunks_ref(it);
			ilist_chunks_next(it);
		}
	}
	/* The scratch intrusive list was overwritten by the second BFS. Rebuild
	 * it as an ordered union, retaining the first view's exact fog ordering.
	 * BFS stamps are scratch now: clear, mark membership, then clear again so
	 * neither uint32 wrap nor a future camera can inherit our union marks. */
	for(size_t v = 0; v < count; v++)
		for(size_t i = 0; i < w->render_views[v].count; i++)
			w->render_views[v].chunks[i]->tmp_data.visited_epoch = 0;
	ilist_chunks_init(w->render);
	for(size_t v = 0; v < count; v++) {
		struct world_render_view* result = &w->render_views[v];
		for(size_t i = 0; i < result->count; i++) {
			struct chunk* c = result->chunks[i];
			if(c->tmp_data.visited_epoch) chunk_unref(c);
			else {
				c->tmp_data.visited_epoch = 1;
				ilist_chunks_push_back(w->render, c);
			}
		}
	}
	for(size_t v = 0; v < count; v++)
		for(size_t i = 0; i < w->render_views[v].count; i++)
			w->render_views[v].chunks[i]->tmp_data.visited_epoch = 0;
	return true;
}

void world_pre_render_clear(struct world* w) {
	assert(w);
	for(unsigned v = 0; v < WORLD_RENDER_VIEWS; v++) w->render_views[v].count = 0;
	ilist_chunks_init(w->render);
}

// --- M1 far-ring draw-distance LOD ------------------------------------------
// Each loaded chunk gets a render tier from its Chebyshev chunk-distance to the
// camera: near chunks render the full faithful 6-sided mesh; far chunks render
// the surface-only "skin" (top faces, ~1 quad/column) for a big GPU-vertex drop
// at distance. This is the sanctioned better-than-Beta render carve-out -- it
// changes only HOW far terrain is drawn, never the simulation state or the
// near-field look. (Phase 1: re-mesh already-resident chunks. Extending reach
// past the current MEM1-bound view distance is a later phase.)
//
// s_lod_near_radius is the seed of the runtime "reach dial": chunks within it
// stay full-detail. Default keeps a generous full-detail core; tune on-device
// against the real-Wii GPU/framerate (Dolphin can't see the GPU ceiling).
#define LOD_NEAR_RADIUS_DEFAULT 2 /* tests only; the game sets lod.high_detail_radius at boot (main.c) */
static int s_lod_near_radius = LOD_NEAR_RADIUS_DEFAULT;
/* Split RENDER REACH (2026-09-10, T2, docs/local_splitscreen.md): with two
 * cameras active, chunks farther than this many chunks (Chebyshev) from BOTH
 * cameras are demoted to the dormant tier (lod 2: resident, simulated, not
 * meshed, not drawn; fog pulled in by main.c). 0 = off (full rings). Server
 * residency, ticking and saves are untouched: this is a render budget only.
 * Measured need: two full 5-chunk rings are ~15 MiB of GX streams against a
 * ~12 MiB mesh budget (Astra HIGH verdict, docs/p2_astra_high_2026-09-10.md);
 * rings 5+6 are ~6.5 MiB of that. */
static int s_split_render_reach;
/* Client mirror of the EFFECTIVE server load radius (main.c refreshes it
 * every frame from server_local_view_radius): the readiness scans and the
 * loading-screen gate must scan the square the server actually streams, or
 * a radius-4 split waits forever on a shell that never arrives. 0 = not set
 * (callers fall back to MAX_VIEW_DISTANCE). */
static int s_server_view_radius;
void world_set_server_view_radius(int chunks) {
	s_server_view_radius = chunks < 0 ? 0 : chunks;
}
int world_server_view_radius(void) {
	return s_server_view_radius;
}
void world_set_split_render_reach(int chunks) {
	s_split_render_reach = chunks < 0 ? 0 : chunks;
}
int world_split_render_reach(void) {
	return s_split_render_reach;
}

void world_lod_set_near_radius(int chunks) {
	s_lod_near_radius = chunks < 0 ? 0 : chunks;
}

int world_lod_near_radius(void) {
	return s_lod_near_radius;
}

// S2 far-storage compression (see world.h). Default flipped to SKIN in the
// far-reach program Wave 1 (docs/far_reach_program_2026-09-03.md W1-a,
// 2026-09-03) -- proven byte-identical emitted display lists to VOXEL
// (tests/test_far_storage_equiv.c). main.c reads "lod.far_storage" and calls
// world_set_far_storage(WORLD_FAR_STORAGE_VOXEL) only when it explicitly
// reads "voxel"; any caller that skips that config read (tests, tools) now
// gets the cheaper, still-byte-exact SKIN default too.
// RESTORE (2026-09-03, maintainer ruling): back to the 2026-09-02 VOXEL default
// until the skin path re-lands behind a hardware throughput/eyeball gate.
static enum world_far_storage s_far_storage = WORLD_FAR_STORAGE_VOXEL;

void world_set_far_storage(enum world_far_storage mode) {
	s_far_storage = mode;
}

// W1-b fog-end clip (see world.h). Default matches the historical compile-
// time FOG_DIST_NO_RENDER constant below (still the value used if a caller
// skips main.c's config read, e.g. tests/tools).
static float s_fog_clip = FOG_DIST_NO_RENDER;

void world_set_fog_clip(float multiplier) {
	s_fog_clip = multiplier;
}

float world_get_fog_clip(void) {
	return s_fog_clip;
}

enum world_far_storage world_get_far_storage(void) {
	return s_far_storage;
}

// Far-ring keep-up probe (2026-09-03 reach-sweep gate 4). Bounded scan over
// the far annulus (inner_radius_excl < Chebyshev distance <= outer_radius_incl,
// in CHUNK-INDEX space, same units as SECTION_TO_ID) around (center_cx,
// center_cz), counting slots with no dict_wsection entry at all -- i.e. the
// client has neither a full near chunk nor a far-LOD section there yet. O(1)
// hash lookup per candidate slot; the caller (main.c's emit_hw_heartbeat) is
// responsible for throttling calls to its own heartbeat cadence (<=1 Hz) --
// see hw_telemetry.h's far_missing field doc for why this is a documented
// throttled scan rather than a receive/evict-only incremental counter (the
// annulus recenters continuously with the player's exact position, and this
// module has no visibility into the server's far_sent membership to diff
// against). A slot inside the annulus with ANY section present (near or far)
// counts as "not missing" -- the client has geometry there either way.
int world_far_missing_count(struct world* w, w_coord_t center_cx,
							w_coord_t center_cz, int inner_radius_excl,
							int outer_radius_incl) {
	assert(w);
	if(outer_radius_incl <= inner_radius_excl)
		return 0;
	int missing = 0;
	for(w_coord_t dz = -outer_radius_incl; dz <= outer_radius_incl; dz++) {
		w_coord_t adz = dz < 0 ? -dz : dz;
		for(w_coord_t dx = -outer_radius_incl; dx <= outer_radius_incl; dx++) {
			w_coord_t adx = dx < 0 ? -dx : dx;
			w_coord_t cheb = adx > adz ? adx : adz;
			if(cheb <= inner_radius_excl || cheb > outer_radius_incl)
				continue; // inside the near core, or past the far reach
			if(!dict_wsection_get(w->sections,
								  SECTION_TO_ID(center_cx + dx, center_cz + dz)))
				missing++;
		}
	}
	return missing;
}

// Near-ring readiness probe (2026-09-03 maintainer ruling, responsiveness
// lane H-2/H-3): a bounded disk scan (Chebyshev distance <= radius, in
// CHUNK-INDEX space -- same units world_far_missing_count takes) around
// (center_cx, center_cz), reporting TWO independent counts in one pass:
//
//   *out_missing  -- sections with NO dict_wsection entry at all (the server
//                    hasn't streamed this column yet). Same "absent slot"
//                    definition world_far_missing_count uses; callers
//                    typically pass MAX_VIEW_DISTANCE as `radius` (the
//                    server's own near-send radius, server_local_state.h),
//                    matching the boundary world_far_missing_count's callers
//                    already use as its inner_radius_excl.
//   *out_mesh_pending -- RESIDENT, FULL-DETAIL (lod==0) chunks inside the disk
//                    that still need a (re)build: rebuild_displist set
//                    (queued, not yet sent) or mesh_in_flight set (sent,
//                    result not yet integrated). A column that hasn't
//                    streamed in yet contributes 0 here (nothing to mesh) --
//                    it is counted by *out_missing instead, so the two never
//                    double-count the same gap.
//
// THE lod==0 FILTER IS LOAD-BEARING, NOT A REFINEMENT (2026-09-04 RCA, real
// Wii far_walk_6m run staged_far_walk_6m_20260904_083316). The runtime reach
// dial (world_lod_set_near_radius, F3/config "lod.near_radius") is INDEPENDENT
// of the server's near-send radius MAX_VIEW_DISTANCE, and the shipped Wii
// default is SMALLER than it (that run: near_radius=2, MAX_VIEW_DISTANCE=5).
// Every column at Chebyshev distance in (near_radius+1, radius] is therefore a
// BAND-2 (lod==1, far-skin) chunk that happens to sit INSIDE this scan disk.
// Counting those as "near mesh pending" made the band-2 starvation gate
// self-referential: main.c derived allow_far from this very count, and
// world_build_chunks refuses to offer a lod==1 chunk to the mesher while
// allow_far is false -- so the pending term could never fall, allow_far could
// never become true, and those chunks could never mesh. Measured symptom: the
// heartbeat's near_missing pinned at ~674-680 of 968 sub-chunks for a full 6
// minutes (11x11 columns x 8, of which the dist-4/5 shells alone are 72x8=576),
// while far_missing drained to 0 (streaming was fine) and tris sat at 13.4k vs
// 37.6k on the pre-gate baseline -- i.e. two thirds of the world's geometry
// was never meshed at all. A chunk the gate itself refuses to build must not be
// counted by the signal that drives the gate; lod==1 chunks are band 2's own
// backlog and are bounded by world_band2_allow_far below instead. When the reach
// dial is >= radius (the PC default and any reach >= MAX_VIEW_DISTANCE) every
// chunk in the disk is lod==0 and this filter changes nothing.
//
// This is the loading-screen gate's "is the near ring actually done" check
// (screen_load_world.c) AND the mesher band-priority gate's "may far tiles
// be submitted yet" check (world_build_chunks below) -- both need the exact
// same near-ring definition, so they share this one bounded scan instead of
// drifting into two slightly different notions of "near". Cost bound: O(disk
// area x COLUMN_HEIGHT), independent of total loaded chunks -- the same
// "bounded, not incremental" tradeoff world_far_missing_count's doc comment
// makes, and cheap enough to call every frame (world_build_chunks does) at
// the small radii this project ships (radius 5-10; disk area <= (2*10+1)^2 =
// 441 columns x COLUMN_HEIGHT).
void world_near_ring_status(struct world* w, w_coord_t center_cx,
							w_coord_t center_cz, int radius, int* out_missing,
							int* out_mesh_pending) {
	assert(w);
	int missing = 0, pending = 0;
	for(w_coord_t dz = -radius; dz <= radius; dz++) {
		w_coord_t adz = dz < 0 ? -dz : dz;
		for(w_coord_t dx = -radius; dx <= radius; dx++) {
			w_coord_t adx = dx < 0 ? -dx : dx;
			w_coord_t cheb = adx > adz ? adx : adz;
			if(cheb > radius)
				continue;
			struct world_section* s = dict_wsection_get(
				w->sections, SECTION_TO_ID(center_cx + dx, center_cz + dz));
			if(!s) {
				missing++;
				continue;
			}
			for(size_t k = 0; k < COLUMN_HEIGHT; k++) {
				struct chunk* c = s->column[k];
				// c->lod == 0 -- see the "load-bearing" paragraph above: a
				// lod==1 chunk inside this disk is BAND 2's backlog, and the
				// gate this count drives is exactly what stops it building.
				if(c && c->lod == 0
				   && (c->rebuild_displist || c->mesh_in_flight))
					pending++;
			}
		}
	}
	if(out_missing)
		*out_missing = missing;
	if(out_mesh_pending)
		*out_mesh_pending = pending;
}

// H-2 band-2 (far) admission rule (2026-09-04, replacing the original
// 2026-09-03 `near_mesh_pending == 0`). Pure RENDER SCHEDULING ORDER -- which
// chunk's bytes reach the GPU first -- never a simulation value, so no Beta
// citation applies (same standing as world_build_chunks' band gates).
//
// The intent is unchanged: NEAR FIRST. What changed is that "first" is no
// longer "all-or-nothing forever". The old rule was a hard edge -- one
// outstanding near sub-chunk anywhere in the disk starved the ENTIRE far band,
// for as long as it stayed outstanding -- which on real hardware became a
// permanent stall (see world_near_ring_status's RCA paragraph). Two bounds
// replace it, either of which admits band 2:
//
//   (a) SLACK. near_mesh_pending <= WORLD_BAND2_NEAR_PENDING_SLACK. The near
//       band cannot consume more than one mesher queue's worth of sends in a
//       frame anyway (chunk_mesher_send refuses past CHUNK_MESHER_QLENGTH), so
//       while the near backlog fits inside that queue the leftover tokens cost
//       the near band nothing -- they would otherwise be dropped on the floor.
//       That is why the default is exactly CHUNK_MESHER_QLENGTH, not a tuned
//       magic number.
//   (b) RELEASE VALVE. One frame in WORLD_BAND2_STARVE_RELEASE_FRAMES admits
//       band 2 unconditionally, so no near backlog -- however large, however
//       persistent -- can starve the far band indefinitely. Deliberately NOT
//       conditioned on out_missing: 29 of every 30 frames still belong to near
//       (plus band 0's urgent pass, which runs first every frame regardless),
//       and the guarantee is worthless if the pathological case can switch it
//       off. At 60fps that is a band-2 turn twice a second.
//
// `frame` is world's monotonic build_frame (the caller passes it BEFORE
// world_build_chunks increments it, so the phase is stable); the function is
// pure so tests can assert the rule directly.
bool world_band2_allow_far(int near_mesh_pending, uint32_t frame) {
	if(near_mesh_pending <= WORLD_BAND2_NEAR_PENDING_SLACK)
		return true;
	return WORLD_BAND2_STARVE_RELEASE_FRAMES > 0
		&& (frame % (uint32_t)WORLD_BAND2_STARVE_RELEASE_FRAMES) == 0;
}

// T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md):
// geomipmapping error-metric merge state. All default to the faithful "off"/no-op
// values; main.c only calls world_lod_set_geomip when "lod.geomip" reads "on".
#include "chunk_lod_geomip.h"

static bool s_lod_geomip_enabled = false;
static float s_lod_geomip_error_budget_px = 2.0F;
static float s_lod_geomip_k = 0.0F;

void world_lod_set_geomip(bool enabled, float error_budget_px, float fov_deg,
						  float screen_height_px) {
	s_lod_geomip_enabled = enabled;
	s_lod_geomip_error_budget_px = error_budget_px < 0.0F ? 0.0F : error_budget_px;
	s_lod_geomip_k = lod_geomip_projection_k(fov_deg, screen_height_px);
}

bool world_lod_geomip_enabled(void) {
	return s_lod_geomip_enabled;
}

float world_lod_geomip_error_budget_px(void) {
	return s_lod_geomip_error_budget_px;
}

float world_lod_geomip_projection_k(void) {
	return s_lod_geomip_k;
}

// Foliage LOD distance bands (gentle, tunable; Chebyshev chunks from the camera). A chunk
// at/beyond PLANT_DIST sheds small plants + collapses its leaves to a solid silhouette
// (band 1); at/beyond GRASS_DIST it also sheds tall grass (band 2). The per-band culling
// lives in chunk_mesher_rebuild; here we only set the band + re-dirty on a change.
#define FOLIAGE_LOD_PLANT_DIST 3
#define FOLIAGE_LOD_GRASS_DIST 4

static int s_anchor_sep;
/* colcap-1006 (R5 item 3): where the synthetic anchor stands NOW (0..sep);
 * s_anchor_sep keeps meaning two-anchor mode. Main thread only. */
static int s_anchor_offset;
/* Experimental local split-screen: when a REAL guest body is admitted,
 * the second anchor is that body, not s_anchor_sep's synthetic +X
 * offset. Diagnostics only (world_anchor_status); residency itself is
 * the server's, and the two-view LOD already runs off the real
 * cameras. Kept truthful so a split_play_2m receipt reports the
 * frontier the guest actually owns. */
static bool s_guest_anchor;
static double s_guest_anchor_x, s_guest_anchor_z;

void world_set_guest_anchor(bool active, double x, double z) {
	s_guest_anchor = active;
	s_guest_anchor_x = x;
	s_guest_anchor_z = z;
}
void world_set_anchor_sep(int blocks) {
	s_anchor_sep = blocks;
	s_anchor_offset = blocks;
}
void world_set_anchor_offset(int blocks) {
	s_anchor_offset = blocks;
}

void world_anchor_status(struct world* w, double x, double y, double z,
						 int server_radius, int far_radius, int missing[2],
						 int pending[2], int far[2], int far_pending[2],
						 bool* collision) {
	for(unsigned p = 0; p < 2; p++) {
		double ax = x + p * s_anchor_offset, az = z;
		if(p && s_guest_anchor) { ax = s_guest_anchor_x; az = s_guest_anchor_z; }
		w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(ax));
		w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(az));
		missing[p] = pending[p] = far[p] = far_pending[p] = 0;
		int radius = far_radius > server_radius ? far_radius : server_radius;
		for(int dz = -radius; dz <= radius; dz++)
			for(int dx = -radius; dx <= radius; dx++) {
				bool near = abs(dx) <= server_radius && abs(dz) <= server_radius;
				struct world_section* section = dict_wsection_get(
					w->sections, SECTION_TO_ID(cx + dx, cz + dz));
				/* Synthetic skins cannot satisfy full-column residency.
				 * Unlike the mesher admission gate, this observation must
				 * include band-2 meshes too; it never controls their service. */
				bool full = section && !section->is_far_lod;
				if(full && near)
					for(size_t k = 0; k < COLUMN_HEIGHT; k++)
						if(!section->column[k])
							full = false;
				/* A far column beyond the split render reach (two cameras)
				 * is never streamed nor drawn: a policy exclusion, not a
				 * missing column. Near (simulation) columns always count. */
				bool far_excluded = !near && s_split_render_reach
					&& (s_anchor_sep || s_guest_anchor)
					&& (abs(dx) > s_split_render_reach || abs(dz) > s_split_render_reach);
				if(far_excluded)
					continue;
				if(!section || (near && !full))
					(near ? missing : far)[p]++;
				if(!section)
					continue;
				for(size_t k = 0; k < COLUMN_HEIGHT; k++) {
					struct chunk* c = section->column[k];
					/* A dormant chunk (split render reach) is never built:
					 * it is a policy exclusion, not unserved work. */
					if(c && c->lod != 2
					   && (c->rebuild_displist || c->mesh_in_flight))
						(near ? pending : far_pending)[p]++;
				}
			}
	}
	double bx = s_guest_anchor ? s_guest_anchor_x : x + s_anchor_offset;
	double bz = s_guest_anchor ? s_guest_anchor_z : z;
	struct AABB box = {
		(float)(bx - 0.3), (float)(y - 1.62), (float)(bz - 0.3),
		(float)(bx + 0.3), (float)(y + 0.18), (float)(bz + 0.3)};
	*collision = world_aabb_intersection(w, &box);
}

void world_update_lod(struct world* w) {
	if(!s_anchor_sep) {
		struct camera* camera = &gstate.players[0].camera;
		world_update_lod_views(w, &camera, 1);
		return;
	}
	struct camera anchor = gstate.players[0].camera;
	anchor.x += s_anchor_offset;
	struct camera* cameras[] = {&gstate.players[0].camera, &anchor};
	world_update_lod_views(w, cameras, s_anchor_sep ? 2 : 1);
}

void world_update_lod_views(struct world* w, struct camera* const* cameras, size_t count) {
	assert(w && cameras && count && count <= WORLD_RENDER_VIEWS);
	w_coord_t cam_cx[WORLD_RENDER_VIEWS], cam_cz[WORLD_RENDER_VIEWS];
	for(size_t v = 0; v < count; v++) {
		cam_cx[v] = WCOORD_CHUNK_OFFSET((w_coord_t)floor(cameras[v]->x));
		cam_cz[v] = WCOORD_CHUNK_OFFSET((w_coord_t)floor(cameras[v]->z));
	}

	dict_wsection_it_t it;
	dict_wsection_it(it, w->sections);

	while(!dict_wsection_end_p(it)) {
		struct world_section* s = &dict_wsection_ref(it)->value;
		for(size_t k = 0; k < COLUMN_HEIGHT; k++) {
			struct chunk* c = s->column[k];
			if(!c)
				continue;

			w_coord_t dist = 0;
			for(size_t v = 0; v < count; v++) {
				w_coord_t dx = WCOORD_CHUNK_OFFSET(c->x) - cam_cx[v];
				w_coord_t dz = WCOORD_CHUNK_OFFSET(c->z) - cam_cz[v];
				if(dx < 0) dx = -dx;
				if(dz < 0) dz = -dz;
				w_coord_t d = dx > dz ? dx : dz;
				if(!v || d < dist) dist = d;
			}

			// 1-chunk hysteresis band: promote to full within the near radius,
			// demote to skin only beyond near+1; in between, keep the current
			// tier so a camera hovering on a boundary doesn't thrash re-meshes.
			uint8_t desired = c->lod;
			if(dist <= s_lod_near_radius)
				desired = 0;
			else if(dist > s_lod_near_radius + 1)
				desired = 1;
			// Split render reach (two cameras only): STRICTLY beyond the
			// reach from every camera the chunk goes dormant, within it the
			// ordinary tiers apply. No hysteresis band on purpose: the first
			// Wii run with a +1 band kept ring 5 resident (dln 5,859 vs
			// 6,450 at readiness, carve floor unchanged) -- the reach must be
			// the reach. A border crossing costs one rebuild, which is rare.
			if(s_split_render_reach && count >= 2) {
				if(dist > s_split_render_reach)
					desired = 2;
				else if(c->lod == 2)
					desired = dist <= s_lod_near_radius ? 0 : 1;
			} else if(c->lod == 2) {
				/* Reach switched off (guest left / single camera): restore. */
				desired = dist <= s_lod_near_radius ? 0 : 1;
			}

			// Re-dirty ONLY on an actual tier change (not while lod != meshed):
			// re-dirtying an in-flight mesh every frame would stale every result
			// and never converge. chunk_check_built re-meshes at the new tier.
			if(desired != c->lod) {
				c->lod = desired;
				if(desired == 2) {
					/* Drop the lists now (that is the whole point of the
					 * tier); bump the generation so an in-flight result for
					 * the old tier lands stale; nothing to rebuild. */
					chunk_mark_dirty(c);
					c->rebuild_displist = false;
					chunk_mesh_release(c);
				} else {
					c->rebuild_displist = true;
					chunk_mark_dirty(c);
				}
				SETTLE(world_settle_observe, SET_DIRTY_LOD, c, NULL, 0, 0, 0,
					   0);
			}

			// T2 geomip spike: snapshot this chunk's approx camera distance in
			// blocks for the mesher (see chunk.h's lod_dist_blocks). Gated behind
			// the flag so this loop touches NO new state when geomip is off --
			// dist here is Chebyshev CHUNK distance, already computed above for
			// the lod tier decision; no re-dirty needed, it's read fresh by
			// chunk_mesher_send on every (re)send regardless of dirty_generation.
			if(world_lod_geomip_enabled())
				c->lod_dist_blocks = (uint16_t)(dist * CHUNK_SIZE);

			// Foliage band from the same camera distance. (dist is chunk-granular, so
			// this only flips on a chunk-border crossing -- same cadence as the lod
			// tier above -- not continuously, so no per-frame re-mesh thrash.)
			uint8_t fdesired = 0;
			if(dist >= FOLIAGE_LOD_GRASS_DIST)
				fdesired = 2;
			else if(dist >= FOLIAGE_LOD_PLANT_DIST)
				fdesired = 1;
			if(fdesired != c->foliage_lod) {
				c->foliage_lod = fdesired;
				/* A dormant chunk (split render reach) owns no mesh to
				 * re-dirty; the band is applied when it is promoted. */
				if(c->lod != 2) {
					c->rebuild_displist = true;
					chunk_mark_dirty(c);
					SETTLE(world_settle_observe, SET_DIRTY_LOD, c, NULL, 0, 0,
						   0, 0);
				}
			}
		}
		dict_wsection_next(it);
	}
}

// H-2: called for every mesh_urgent chunk world_build_chunks's urgent passes
// visit, AFTER attempting chunk_check_built on it. Releases the priority
// slot and folds the wait into the H-3 edit-to-visible instrument once the
// chunk is fully settled; a no-op while still pending.
static void world_urgent_settle_check(struct world* w, struct chunk* c) {
	if(c->rebuild_displist || c->mesh_in_flight)
		return;
	uint32_t waited = w->build_frame >= c->mesh_urgent_since_frame
		? w->build_frame - c->mesh_urgent_since_frame
		: 0;
	if(waited > w->edit_to_visible_frames_max)
		w->edit_to_visible_frames_max = waited;
	c->mesh_urgent = false;
	// mesh_urgent_count is asserted-nonzero, not clamped: every increment
	// (chunk_mark_urgent) is paired with one decrement, either on settlement
	// here or when world_release_section_chunks retires the resident owner.
	assert(w->mesh_urgent_count > 0);
	w->mesh_urgent_count--;
}

void world_set_build_focus(struct world* w, bool enabled, w_coord_t cx,
						   w_coord_t cz, int radius) {
	assert(w);
	w->build_focus = enabled && radius >= 1;
	w->build_focus_cx = cx;
	w->build_focus_cz = cz;
	w->build_focus_radius = radius >= 1 ? radius : 0;
}

bool world_build_focus_wanted(bool remote_session, bool loading_screen) {
	return remote_session && loading_screen;
}

// REL-MP-COREFIRST (2026-10-22, tmp/jobs/join-regress-1022/REMAINING.md):
// on the remote loading screen w->render is empty (main.c clears it while the
// screen does not render the world), so before this band every token went to
// the urgent dict walk and then the w->sections walk -- both in HASH order,
// always from the first bucket. A server settle storm keeps re-dirtying (and
// urgent-marking, via chunk_set_block) chunks all over the disk, so the 3x3
// safe core the admission gate waits on (core_mesh_pending) could sit behind
// the whole backlog: fresh-vanilla Wii joins 18-36 s with the core resident
// at <=3 s. This band offers the focus disk ring by ring, nearest first:
// ring 0 and ring 1 ARE the safe core. Pure render scheduling order: the
// same chunk_check_built (in-flight guard, retry pacing, B2 neighbour wait),
// the same lod==1/allow_far gate, the same token accounting, so each chunk
// still meshes from its then-current data to the same bytes. It runs before
// band 0 on purpose: on the loading screen the urgent marks are the storm's,
// not a player's (no player input reaches the world there). Bounded:
// (2r+1)^2 direct lookups (121 at r=5), main thread only, no lock, so it can
// never stall the server writer (the 10-03 seqlock lesson).
static size_t world_build_focus_band(struct world* w, size_t tokens,
									 bool allow_far) {
	const int r = w->build_focus_radius;
	for(int ring = 0; ring <= r && tokens > 0; ring++) {
		for(int dz = -ring; dz <= ring && tokens > 0; dz++) {
			int adz = dz < 0 ? -dz : dz;
			// Only the ring's own cells: full rows at |dz| == ring, the two
			// side cells otherwise (ring 0 is the single centre cell).
			int step = (adz == ring || ring == 0) ? 1 : 2 * ring;
			for(int dx = -ring; dx <= ring && tokens > 0; dx += step) {
				struct world_section* s = dict_wsection_get(
					w->sections, SECTION_TO_ID(w->build_focus_cx + dx,
											   w->build_focus_cz + dz));
				if(!s)
					continue;
				for(size_t k = 0; k < COLUMN_HEIGHT && tokens > 0; k++) {
					struct chunk* c = s->column[k];
					if(!c)
						continue;
					SETTLE(world_settle_observe, SET_VISIT, c, NULL, 0, 0, 0,
						   0);
					if(c->lod == 1 && !allow_far) {
						SETTLE(world_settle_observe, SET_GATE_FAR, c, NULL, 0,
							   0, 0, 0);
						continue;
					}
					if(chunk_check_built(c))
						tokens--;
				}
			}
		}
	}
	return tokens;
}

size_t world_build_chunks(struct world* w, size_t tokens, bool allow_far) {
	assert(w);
	if(thread_g9_lease_defer()) return tokens;
	w->build_frame++;

	const size_t cap = world_ilist_walk_cap(w);
	size_t steps = 0;

	// REL-MP-COREFIRST: remote loading screen only (w->build_focus is false
	// everywhere else, and then this is a single bool test).
	if(w->build_focus)
		tokens = world_build_focus_band(w, tokens, allow_far);

	// H-2 band 0 (2026-09-03 maintainer ruling): urgent chunks (player edits +
	// lateral neighbours, a just-installed section's lateral neighbours) get
	// first crack at this frame's token budget, before band 1 (near) or band
	// 2 (far) get a turn -- see struct chunk's mesh_urgent doc comment. The
	// w->render scan below is cheap and unconditional (one bool check per
	// visible/near chunk, same list this function already walks for band 1);
	// the w->sections scan is the potentially-expensive one (up to every
	// resident chunk, not just the visible/near ones -- an edit can urgent-
	// mark a chunk that is not currently in the BFS render list, e.g. just
	// outside the frustum), so it is gated on mesh_urgent_count > 0 and
	// therefore costs nothing beyond one integer compare on the overwhelming
	// majority of frames (no edit pending).
	ilist_chunks_it_t it_urgent;
	ilist_chunks_it(it_urgent, w->render);
	while(tokens > 0 && !ilist_chunks_end_p(it_urgent)) {
		if(++steps > cap) {
			s_ilist_overrun++;
			return tokens;
		}
		struct chunk* c = ilist_chunks_ref(it_urgent);
		if(c->mesh_urgent) {
			// H-2 URGENT-GATE FIX (2026-09-03, responsiveness lane): band 0
			// is a PRIORITY WITHIN band 1 (near, lod==0) -- it does not let a
			// far (lod==1) urgent chunk (e.g. a just-installed far section's
			// lateral neighbour, world_build_far_section) jump band 2's own
			// starvation gate. Without this check, ordinary far-ring
			// streaming kept marking far neighbours urgent and this pass
			// meshed them regardless of allow_far, defeating "band 2 fully
			// starved while near is unmeshed" (test_mesher_band_priority
			// caught this). Pure render SCHEDULING ORDER -- which chunk's
			// bytes reach the GPU first -- never a simulation value, so no
			// Beta citation applies; mirrors the band-2 gate below verbatim.
			SETTLE(world_settle_observe, SET_VISIT, c, NULL, 0, 0, 0, 0);
			if(c->lod == 1 && !allow_far)
				SETTLE(world_settle_observe, SET_GATE_FAR, c, NULL, 0, 0, 0, 0);
			if(!(c->lod == 1 && !allow_far) && chunk_check_built(c))
				tokens--;
			world_urgent_settle_check(w, c);
		}
		ilist_chunks_next(it_urgent);
	}

	if(tokens > 0 && w->mesh_urgent_count > 0) {
		dict_wsection_it_t it_urgent2;
		dict_wsection_it(it_urgent2, w->sections);
		while(tokens > 0 && w->mesh_urgent_count > 0
			  && !dict_wsection_end_p(it_urgent2)) {
			struct world_section* s = &dict_wsection_ref(it_urgent2)->value;
			for(size_t k = 0; k < COLUMN_HEIGHT && tokens > 0
				&& w->mesh_urgent_count > 0;
				k++) {
				struct chunk* c = s->column[k];
				if(c && c->mesh_urgent) {
					// H-2 URGENT-GATE FIX (2026-09-03): same band-2
					// starvation gate as the w->render urgent pass above --
					// see that comment for why a far (lod==1) urgent chunk
					// must still wait for allow_far.
					SETTLE(world_settle_observe, SET_VISIT, c, NULL, 0, 0, 0,
						   0);
					if(c->lod == 1 && !allow_far)
						SETTLE(world_settle_observe, SET_GATE_FAR, c, NULL, 0,
							   0, 0, 0);
					if(!(c->lod == 1 && !allow_far) && chunk_check_built(c))
						tokens--;
					world_urgent_settle_check(w, c);
				}
			}
			dict_wsection_next(it_urgent2);
		}
	}

	steps = 0;
	ilist_chunks_it_t it;
	ilist_chunks_it(it, w->render);

	while(tokens > 0 && !ilist_chunks_end_p(it)) {
		// The hang site. Abandon this frame's build pass rather than spin: the render
		// list is rebuilt from scratch every world_pre_render, so a bad frame costs
		// deferred meshes, and main keeps running (and keeps releasing chunk slots).
		if(++steps > cap) {
			s_ilist_overrun++;
			return tokens;
		}

		struct chunk* c = ilist_chunks_ref(it);
		// H-2 band 2 starvation gate (2026-09-03 maintainer ruling): while
		// ANY near-ring chunk still needs a (re)build, a far (lod==1) chunk
		// is not even OFFERED to chunk_check_built -- caller (main.c) derives
		// allow_far from world_near_ring_status's out_mesh_pending, the same
		// near-ring definition the loading screen's gate uses.
		SETTLE(world_settle_observe, SET_VISIT, c, NULL, 0, 0, 0, 0);
		if(c->lod == 1 && !allow_far)
			SETTLE(world_settle_observe, SET_GATE_FAR, c, NULL, 0, 0, 0, 0);
		if(!(c->lod == 1 && !allow_far) && chunk_check_built(c))
			tokens--;
		ilist_chunks_next(it);
	}

	dict_wsection_it_t it2;
	dict_wsection_it(it2, w->sections);

	while(tokens > 0 && !dict_wsection_end_p(it2)) {
		struct world_section* s = &dict_wsection_ref(it2)->value;
		// `tokens > 0` belongs in the INNER condition too: this loop spends up to
		// COLUMN_HEIGHT tokens per outer step while only the outer while re-tested the
		// budget, so a column that built past the last token wrapped size_t 0 ->
		// SIZE_MAX and silently turned the budgeted scan into a full scan of every
		// resident section, every frame.
		for(size_t k = 0; k < COLUMN_HEIGHT && tokens > 0; k++) {
			struct chunk* c = s->column[k];
			if(c) {
				SETTLE(world_settle_observe, SET_VISIT, c, NULL, 0, 0, 0, 0);
				if(c->lod == 1 && !allow_far)
					SETTLE(world_settle_observe, SET_GATE_FAR, c, NULL, 0, 0, 0,
						   0);
			}
			if(c && !(c->lod == 1 && !allow_far) && chunk_check_built(c))
				tokens--;
		}

		dict_wsection_next(it2);
	}

	return tokens;
}

void world_render_completed(struct world* w, bool new_render) {
	assert(w);

	const size_t cap = world_ilist_walk_cap(w);
	size_t steps = 0;

	ilist_chunks2_it_t it;
	ilist_chunks2_it(it, w->gpu_busy_chunks);

	while(!ilist_chunks2_end_p(it)) {
		if(++steps > cap) {
			// Defensive rather than a proven hang site: this walk UNLINKS each node it
			// visits and m-lib's _unlink nulls it, so today a broken chain faults on the
			// revisit instead of spinning -- the node check below is what catches that.
			// m-lib calls the nulling "not really needed, but safer" (m-i-list.h) -- drop
			// it upstream and this becomes the same infinite walk as world_build_chunks.
			// Bail via init (an O(1) head reset) and NOT _reset, which walks the same
			// broken chain to the sentinel and would hang.
			s_ilist_overrun++;
			ilist_chunks2_init(w->gpu_busy_chunks);
			break;
		}

		struct chunk* c = ilist_chunks2_ref(it);

		// The reachable one. A hand-off abandoned mid-walk used to leave a self-linked
		// node here (see the bail below); _remove would then write through the NULLs
		// _unlink had just stored and unref this same chunk on the next turn of the loop
		// -- a double unref, i.e. free-then-dereference. Drop what is left of the list
		// instead: the references behind the bad node leak until world_destroy, which is
		// the same bounded trade the cap bail above makes.
		if(ilist_node_unremovable(&c->ilist_chunks2)) {
			s_ilist_overrun++;
			ilist_chunks2_init(w->gpu_busy_chunks);
			break;
		}

		ilist_chunks2_remove(w->gpu_busy_chunks, it);
		chunk_unref(c);
	}

	ilist_chunks2_reset(w->gpu_busy_chunks);
	/* Everything the GPU read last frame is unpinned (or, after a bail,
	 * leaked and never freed): its context may now free/re-admit. */
	world_gpu_lease_release(w);

	if(new_render) {
		steps = 0;
		/* The hand-off below pins this frame's drawn chunks for the GPU:
		 * lease their context until the next call drains them. A refusal
		 * (the context is parked) cannot un-draw the frame; it is counted.
		 * By construction it does not happen: a re-stream installs a
		 * successor context (never parked), and a scoped reset that parks a
		 * context unloads every chunk first, with nothing installing until
		 * it completes -- so the render list is empty while parked. */
		if(w->lease_owner && !ilist_chunks_empty_p(w->render)) {
			if(w->lease_owner->acquire(w->lease_owner->user, WORLD_LEASE_GPU,
									   &w->gpu_lease))
				w->gpu_lease_owner = w->lease_owner;
			else
				w->gpu_lease_refused++;
		}

		ilist_chunks_it_t it2;
		ilist_chunks_it(it2, w->render);

		while(!ilist_chunks_end_p(it2)) {
			if(++steps > cap) {
				// This bail has to leave gpu_busy_chunks WALKABLE, and by now it is not.
				// A cyclic render chain hands _push_back the SAME chunk over and over,
				// and re-pushing the node that is currently the list tail self-links it
				// (node->next == node->prev == node), so the partial hand-off below has
				// already built the next frame's fault: the drain above walks that node
				// forever, or -- once _remove has nulled it -- unrefs one chunk twice and
				// dereferences it after the free. Drop the whole partial hand-off with
				// the same O(1) head reset the drain bail uses.
				//
				// The tokens it was carrying LEAK (those chunks stay pinned until
				// world_destroy). They cannot be handed back: the only record of who held
				// them is the render chain just proved unwalkable, and unref-ing once per
				// VISIT would unref the revisited chunk many times -- the exact fault this
				// is avoiding. Bounded to one frame's visible set per overrun, on an
				// already corrupt list, and world_ilist_overrun_count() makes it visible
				// instead of silent.
				s_ilist_overrun++;
				ilist_chunks2_init(w->gpu_busy_chunks);
				return;
			}

			// move imaginary reference token from "render" to "gpu_busy_chunks"
			ilist_chunks2_push_back(w->gpu_busy_chunks, ilist_chunks_ref(it2));
			ilist_chunks_next(it2);
		}
	}
}

static size_t world_render_list(struct world* w, struct camera* c,
	const struct world_render_view* result, bool pass) {
	assert(w && c);

	size_t in_view = 0;
	ilist_chunks_it_t it;

	// Same bound as the build pass: a corrupt render chain must drop this frame's
	// terrain, not spin the main thread inside the draw loop. Every exit below falls
	// through to the epilogue so the GX/GL state is always restored.
	const size_t cap = world_ilist_walk_cap(w);
	size_t steps = 0;
	bool overrun = false;

	gfx_fog(true);
	gfx_lighting(true);
	// Default to untinted: the bulk (9-byte GX_VTXFMT0) chunk lists draw with the
	// per-vertex CLR1 biome-tint channel + its TEV stage OFF. chunk_render turns
	// gfx_tint() on only around each chunk's few tinted (12-byte GX_VTXFMT4) lists
	// (grass/leaves). White tint == identity, so untinted faces are unchanged.
	gfx_tint(false);

	if(!pass) {
		gfx_bind_texture(&texture_terrain);
		gfx_blending(MODE_OFF);
		gfx_alpha_test(true);

		ilist_chunks_it(it, w->render);

		while(result ? steps < result->count : !ilist_chunks_end_p(it)) {
			if(++steps > cap) {
				s_ilist_overrun++;
				break;
			}

			struct chunk* chunk = result ? result->chunks[steps - 1] : ilist_chunks_ref(it);
			if(result) chunk_pre_render(chunk, c->view,
				!FOG_DIST_LESS(chunk, c->x, c->z, FOG_DIST_NO_EFFECT));
			chunk_render(chunk, false, c->x, c->y, c->z);
			thread_main_pacing_checkpoint_at(THREAD_PACING_RENDER);
			in_view++;
			if(!result) ilist_chunks_next(it);
		}
	} else {
		gfx_alpha_test(false);
		gfx_blending(MODE_BLEND);
		gfx_write_buffers(false, true, true);

		mat4 matrix_anim;
		// CLAMP: time_diff_ms returns int32_t; multiplying by 7 in 32-bit
		// arithmetic overflows once the session/world has been open ~85h
		// (INT32_MAX/7 ms). Widen to int64_t for the multiply/divide/mod so
		// the exact same formula never overflows; every value that fit
		// before is unchanged (this only widens the intermediate).
		int anim = (int)((int64_t)time_diff_ms(w->anim_timer, time_get()) * 7
						  / 800 % 28);
		glm_translate_make(
			matrix_anim,
			(vec3) {(anim / 14) * 0.4921875F, (anim % 14) * 0.0703125F, 0.0F});

		gfx_matrix_texture(true, matrix_anim);
		gfx_bind_texture(&texture_anim);

		for(int t_pass = 0; t_pass < 2 && !overrun; t_pass++) {
			if(t_pass == 1)
				gfx_write_buffers(true, false, true);

			steps = 0;
			ilist_chunks_it(it, w->render);
			while(result ? steps < result->count : !ilist_chunks_end_p(it)) {
				if(++steps > cap) {
					s_ilist_overrun++;
					overrun = true;
					break;
				}

				struct chunk* chunk = result ? result->chunks[steps - 1] : ilist_chunks_ref(it);
				if(result) chunk_pre_render(chunk, c->view,
					!FOG_DIST_LESS(chunk, c->x, c->z, FOG_DIST_NO_EFFECT));
				chunk_render(chunk, true, c->x, c->y, c->z);
				thread_main_pacing_checkpoint_at(THREAD_PACING_RENDER);
				if(!result) ilist_chunks_next(it);
			}
		}

		gfx_matrix_texture(false, NULL);
		gfx_write_buffers(true, true, true);
	}

	gfx_tint(false); // restore single-channel state for entities/particles/GUI
	return in_view;
}

size_t world_render(struct world* w, struct camera* c, bool pass) {
	return world_render_list(w, c, NULL, pass);
}

size_t world_render_view(struct world* w, struct camera* c, unsigned view, bool pass) {
	assert(w && view < WORLD_RENDER_VIEWS);
	return world_render_list(w, c, &w->render_views[view], pass);
}

bool world_aabb_intersection(struct world* w, struct AABB* a) {
	assert(w && a);

	w_coord_t min_x = floorf(a->x1);
	// need to look one further, otherwise fence block breaks
	w_coord_t min_y = floorf(a->y1) - 1;
	w_coord_t min_z = floorf(a->z1);

	w_coord_t max_x = ceilf(a->x2) + 1;
	w_coord_t max_y = ceilf(a->y2) + 1;
	w_coord_t max_z = ceilf(a->z2) + 1;

	for(w_coord_t x = min_x; x < max_x; x++) {
		for(w_coord_t z = min_z; z < max_z; z++) {
			for(w_coord_t y = min_y; y < max_y; y++) {
				struct block_data blk = world_get_block(w, x, y, z);

				if(blocks[blk.type]) {
					struct block_info blk_info = (struct block_info) {
						.block = &blk,
						.neighbours = NULL,
						.x = x,
						.y = y,
						.z = z,
						.get_piston_moving = world_client_piston_moving_lookup,
					};

					size_t count = blocks[blk.type]->getBoundingBox(&blk_info,
																	true, NULL);
					if(count > 0) {
						struct AABB bbox[count];
						blocks[blk.type]->getBoundingBox(&blk_info, true, bbox);

						for(size_t k = 0; k < count; k++) {
							aabb_translate(bbox + k, x, y, z);

							if(aabb_intersection(a, bbox + k))
								return true;
						}
					}
				}
			}
		}
	}

	return false;
}

const float* world_dimension_light(struct world* w) {
	assert(w);

	return (w->dimension == WORLD_DIM_OVERWORLD) ? light_lookup_overworld :
												   light_lookup_nether;
}
