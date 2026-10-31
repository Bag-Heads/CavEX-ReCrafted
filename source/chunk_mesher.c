#include "block/block_leaves.h"
#include "settle_observer.h"
settle_callback mesher_settle_observe;
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

#include "tps_observer.h"
void (*mesher_tps_boundary)(bool);


#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "chunk_lod_geomip.h"
#include "chunk_lod_policy.h"
#include "chunk_mesher.h"
#include "column_observer.h"
#include "column_view.h"
#include "network/server_world.h"
#include "platform/displaylist.h"
#include "platform/thread.h"
#include "platform/g9_probe.h"
#include "platform/g9_capture.h"
#include "platform/time.h"
#include "platform/watchdog.h"
#include "stack.h"
#include "world.h"
#include "graphics/biome_color.h"
#include "graphics/render_block.h"
#include "graphics/texture_atlas.h"
#include "graphics/gfx_settings.h"

/*
	mesher_scratch_alloc / mesher_scratch_free: arena-first scratch
	allocator with a logged malloc fallback. Replaces the previous
	`assert(buf)` panic when the per-job arena is undersized.

	Worst-case scratch we ever ask the arena for in one build is
	  CHUNK_SIZE^3                (4 KB,  visited bitmap)
	+ (CHUNK_SIZE+2)^3 * 3        (~17 KB, light_data cache)
	+ CHUNK_SIZE^3 * 3            (12 KB, reachability BFS queue)
	= ~33 KB, well under MESHER_ARENA_SIZE = 64 KB. So the fallback
	path should never trigger in practice. But MESHER_ARENA_SIZE could
	drift on future refactors, and the previous `assert` failed
	gracefully only in debug builds -- release (-DNDEBUG) silently
	dereferenced NULL. The fallback keeps the game running, logs once
	per occurrence so the regression is visible, and bills the
	allocation to malloc/free (slow but correct).
*/

#ifdef CAVEWII_DISPLAYLIST_TEST
// One-shot scratch total-OOM seam for the real asynchronous mesher test
// (test_piston_chunk_mesh_env is the only target that defines this macro).
// The occurrence'th mesher_scratch_alloc from now reports TOTAL failure --
// arena exhausted AND the malloc fallback dead -- which is otherwise
// undrivable on PC. Same countdown shape and arming rules as the
// displaylist_test_fail_* seams: set only while the worker is idle; the
// request-channel send/receive orders the write before the build reads it.
static unsigned s_mesher_scratch_fail_countdown;

void chunk_mesher_test_fail_scratch_after(unsigned occurrence) {
	s_mesher_scratch_fail_countdown = occurrence;
}
#endif

static void* mesher_scratch_alloc(struct arena* arena, size_t size,
								  size_t align, bool* fallback_out) {
	assert(arena && fallback_out);

#ifdef CAVEWII_DISPLAYLIST_TEST
	if(s_mesher_scratch_fail_countdown > 0
	   && --s_mesher_scratch_fail_countdown == 0) {
		*fallback_out = false;
		return NULL;
	}
#endif

	void* p = arena_alloc(arena, size, align);
	if(p) {
		*fallback_out = false;
		return p;
	}

	fprintf(stderr,
			"[chunk_mesher] WARNING: arena exhausted (need %zu bytes "
			"align %zu, arena=%s, used=%zu/%zu); falling back to malloc. "
			"Bump MESHER_ARENA_SIZE if this fires repeatedly.\n",
			size, align, arena->name ? arena->name : "(unnamed)",
			arena->used, arena->size);

	void* m = malloc(size);
	if(!m) {
		fprintf(stderr,
				"[chunk_mesher] ERROR: malloc(%zu) also failed; "
				"this chunk cannot be meshed.\n",
				size);
		*fallback_out = false;
		return NULL;
	}

	*fallback_out = true;
	return m;
}

static void mesher_scratch_free(void* ptr, bool fallback) {
	if(fallback)
		free(ptr);
}

#define BLK_INDEX(x, y, z)                                                     \
	((x) + ((z) + (y) * (CHUNK_SIZE + 2)) * (CHUNK_SIZE + 2))
#define BLK_INDEX2(x, y, z) ((x) + ((z) + (y) * CHUNK_SIZE) * CHUNK_SIZE)
#define BLK_DATA(b, x, y, z) ((b)[BLK_INDEX((x) + 1, (y) + 1, (z) + 1)])

#define BLK_PADDED_COUNT ((CHUNK_SIZE + 2) * (CHUNK_SIZE + 2) * (CHUNK_SIZE + 2))

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

struct chunk_mesher_rpc {
	struct chunk* chunk;
	uint32_t dirty_generation;
	/* Split-dims B1: the mesher lease this job holds against its world's
	 * client context (lease_owner NULL = the world has no owner: no lease).
	 * Taken in send, given back on the owner thread in receive/shutdown. */
	const struct world_lease_owner* lease_owner;
	struct world_lease lease;
	// ingoing — block_data scratch grid lives inside the RPC slot now.
	// Was a malloc-per-send + free-per-build cycle; pooling it via the
	// rpc_msg[] array eliminates ~23 KB of heap churn per chunk mesh.
	struct {
		struct block_data blocks[BLK_PADDED_COUNT];
		// Per-column Beta biome grass/foliage colourmap indices for this chunk's
		// 16x16 columns (layout [localX*16 + localZ]); resolved to RGB and
		// applied as the per-vertex tint during the build. `has_tint` is false
		// when no section tint is loaded for this column, in which case the
		// whole chunk meshes untinted (white).
		uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE];
		uint8_t grass_j[CHUNK_SIZE * CHUNK_SIZE];
		bool has_tint;
		// LOD tier snapshotted at send time (chunk->lod). 0 = full mesh,
		// 1 = surface skin (worker emits SIDE_TOP faces only -> the far ring).
		// The worker only reads this; the main thread owns the camera/tier.
		uint8_t lod;
		// Foliage distance band (chunk->foliage_lod), set by world_update_lod from
		// camera distance. 0 = full foliage; 1 = small plants culled + leaves collapse
		// to a solid silhouette (internal leaf-leaf faces dropped); 2 = also tall grass
		// culled. Render-only -- sheds distant foliage detail to save vertices/memory.
		uint8_t foliage_lod;
		// T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md):
		// snapshot of world_lod_geomip_enabled()/chunk->lod_dist_blocks, same pattern as
		// lod/foliage_lod above -- the worker never reads world_lod_geomip_enabled()
		// itself so a config change mid-flight can't change an in-flight build's answer.
		bool geomip_enabled;
		uint16_t geomip_dist_blocks;
		// T1-c (2026-09-03 far-ring frame-drop root cause): the OLD mesh's final
		// per-list l->index, snapshotted at send time from c->mesh[k].index
		// (0 when c->has_displist[k] is false -- no history, e.g. a
		// never-before-built chunk). Lets the worker pre-size each new list's
		// buffer from the shape of the list it is about to replace instead of
		// always starting at the fixed small default and growing into it.
		// Read-only on the worker; see displaylist_init_presized.
		size_t prev_index[CHUNK_MESH_LISTS];
	} request;
	// outgoing
	struct {
		struct displaylist mesh[CHUNK_MESH_LISTS];
		bool has_displist[CHUNK_MESH_LISTS];
		// Any list that truncated on allocation failure makes the replacement
		// transactional: never install it; receive owns old-mesh retention/release.
		bool retry_required;
		/* M2 mesh blocks: the packed generation block (NULL outside block mode). */
		void* mesh_block;
		size_t mesh_block_bytes;
		uint8_t reachable[6];
	} result;
};

static struct chunk_mesher_rpc rpc_msg[CHUNK_MESHER_QLENGTH];
static struct thread_channel mesher_requests;
static struct thread_channel mesher_results;
static struct thread_channel mesher_empty_msg;
// C1 (candidate, hardware-unconfirmed): the worker handle, kept (was a local
// `struct thread t` discarded right after thread_create) so chunk_mesher_shutdown
// can join it. `.created` (thread.h) doubles as the "was this ever started" /
// "already shut down" flag -- thread_create sets it true, thread_join clears it.
static struct thread mesher_thread;
// CR-08 style: plain (not volatile) bool, single writer (chunk_mesher_shutdown),
// single reader (the worker, chunk_mesher_local_thread) -- same treatment as
// server_local.h's thread_stop. RELEASE on the write pairs with ACQUIRE on the
// read: the worker must not observe "stop" before shutdown finishes deciding to
// request it.
static bool s_mesher_stop = false;
#ifdef CAVEWII_DISPLAYLIST_TEST
static bool s_test_hold_worker, s_test_worker_held;
void chunk_mesher_test_hold_worker(bool hold) {
	__atomic_store_n(&s_test_hold_worker, hold, __ATOMIC_RELEASE);
}
bool chunk_mesher_test_worker_held(void) {
	return __atomic_load_n(&s_test_worker_held, __ATOMIC_ACQUIRE);
}
#endif
// THROUGHPUT dial, not a fault dial -- non-zero is EXPECTED, and the hang report prints it
// (stale=) right beside counters where non-zero means damage, which is how the 2026-07-29
// report's stale=245 got read as a symptom. A mesh finished building while its chunk had
// already been re-dirtied, so the result is discarded at the newer dirty_generation.
// NOTHING IS LOST: the finished display lists are destroyed on the discard branch, the chunk
// reference and the RPC token are returned on the shared tail, and the chunk re-sends next
// frame because every chunk_mark_dirty site also sets rebuild_displist.
// Judge it as a RATIO against the per-tier build counts, never as an absolute -- a long
// session of piston/TNT/redstone churn produces it by design. A sustained high ratio, or a
// climb at the per-frame build ceiling with the mesher queue pegged and terrain that never
// converges, is a re-dirty LIVELOCK; that is the failure worth naming. Archived reports read
// stale=40..1091 across seven dumps, none ever attributed to the mesher.
// CR-08 (2026-08-20): plain (not volatile) uint32_t (declaration + rationale
// in chunk_mesher.h). Sole writer is the main thread (chunk_mesher_receive,
// below); the increment there uses __atomic so the cross-thread readers --
// the main thread's own telemetry (main.c/screen_ingame.c) and the Wii
// watchdog thread's hang-report dump (wii/watchdog.c) -- observe it via
// __atomic_load_n at their own call sites instead of an unsynchronized read.
uint32_t mesh_stale_discards = 0;

/* colcap-1006 (R4 point 7): see chunk_mesher.h. Counters only. */
static uint32_t s_completed_bytes, s_completed_lists;
static void mesher_result_dl_sum(const struct chunk_mesher_rpc* r,
								 uint32_t* bytes, uint32_t* lists) {
	uint32_t b = 0, n = 0;
	for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++)
		if(r->result.has_displist[k] && r->result.mesh[k].accounted) {
			b += (uint32_t)r->result.mesh[k].length;
			n++;
		}
	*bytes = b;
	*lists = n;
}
void chunk_mesher_completed_stats(uint32_t* bytes, uint32_t* lists) {
	*bytes = __atomic_load_n(&s_completed_bytes, __ATOMIC_RELAXED);
	*lists = __atomic_load_n(&s_completed_lists, __ATOMIC_RELAXED);
}
uint32_t chunk_mesher_clock_ms(void) {
	static bool started;
	static ptime_t epoch;
	ptime_t now = time_get();
	if(!started) {
		epoch = now;
		started = true;
	}
	int32_t ms = time_diff_ms(epoch, now);
	return ms > 0 ? (uint32_t)ms : 0u;
}
// Count of chunk mesh display-lists DROPPED because an OOM truncated them mid-emit (see
// the drop in chunk_mesher_build). A non-zero/climbing value on the F3 is the live
// confirmation of the MEM2-carve-OOM -> partial-mesh -> GP-wedge freeze hypothesis.
// CR-08: same treatment; sole writer is the mesher WORKER thread (below), same
// cross-thread readers (main + watchdog) as mesh_stale_discards above.
uint32_t g_mesh_truncations = 0;
// H-1: cumulative count of chunk_mesher_build completions (successful or not --
// this counts BUILDS, i.e. worker throughput, not successful integrations).
// The measurement instrument for the H-1 fix: "chunks meshed/s" is this
// counter's delta over a wall-clock window (see docs/perf_frame_drops_
// rootcause_2026-09-03.md and the responsiveness-lane commit report for the
// before/after numbers). Same CR-08 treatment as the counters above: sole
// writer is the mesher WORKER thread, __atomic increment there, readers
// (main-thread telemetry + the F3 overlay) use __atomic_load_n.
uint32_t g_chunks_meshed = 0;

// Count of mesh jobs submitted but not yet consumed back (in the request
// queue + being built + in the results queue). Incremented in
// chunk_mesher_send, decremented in chunk_mesher_receive — both run only
// on the main thread (the worker thread merely shuttles items between
// queues and never touches this). CR-08: that single-writer claim only
// covered the WRITE side -- chunk_mesher_pending() below is also read from
// the Wii watchdog thread's hang-report dump, which the old plain `int`
// never accounted for, so the read (and the two updates) now go through
// __atomic instead. Exposed for the debug overlay as a gauge of mesher
// backlog under load.
static int mesher_pending = 0;

int chunk_mesher_pending(void) {
	return __atomic_load_n(&mesher_pending, __ATOMIC_RELAXED);
}

// H-1: cumulative chunk_mesher_build completions since process start. See
// g_chunks_meshed's declaration for what this measures and why.
uint32_t chunk_mesher_meshed_count(void) {
	return __atomic_load_n(&g_chunks_meshed, __ATOMIC_RELAXED);
}

// Wall-clock of the most recent chunk_mesher_build and the running max, in
// ms. Written on the mesher worker thread, read on the main thread by the
// debug overlay (chunk_mesher_build_ms() below; not read by the watchdog
// thread). CR-08: plain (not volatile) float, accessed with __atomic. The
// prior "aligned PPC loads/stores don't tear" reasoning for skipping
// synchronization was true for tearing but not for the other half of the
// audit's complaint -- `volatile` gives no defined cross-thread ordering, so
// a reader was never actually guaranteed to observe a write promptly rather
// than a stale cached copy. __atomic with RELAXED keeps the "approximate
// value is fine" cost model (no barrier needed, these are independent
// display-only gauges) while making the read/write itself well-defined. This
// is the single-core's heaviest CPU consumer under chunk load, so it's the
// prime suspect when frame time blows up while the GPU sits idle.
static float mesher_build_ms_last = 0.0F;
static float mesher_build_ms_max = 0.0F;

/* CR-08 integration fix: __atomic_load_n/_store_n reject floating types
 * (integral/pointer only); the GENERIC __atomic_load/__atomic_store forms
 * handle any 4-byte object with the same single load/store on PPC32 --
 * still lock-free, no libatomic (the thread.h _Static_asserts' class). */
static inline float mesher_atomic_load_f(const float* p) {
	float v;
	__atomic_load((float*)p, &v, __ATOMIC_RELAXED);
	return v;
}

static inline void mesher_atomic_store_f(float* p, float v) {
	__atomic_store(p, &v, __ATOMIC_RELAXED);
}

void chunk_mesher_build_ms(float* last, float* max) {
	if(last)
		*last = mesher_atomic_load_f(&mesher_build_ms_last);
	if(max)
		*max = mesher_atomic_load_f(&mesher_build_ms_max);
}

// M1 LOD verification instrument: total emitted vertices + chunk count bucketed
// by render tier (index 0 = full mesh, 1 = far-ring skin), accumulated across
// every build. Lets us MEASURE the skin's vertex reduction (CLAUDE.md: don't
// assume a speedup) -- compare verts/chunk between the two buckets.
//
// CR-08: g_lod_vertices is uint64_t, which has NO lock-free atomic load/store
// on 32-bit PowerPC -- GCC would lower a whole-value __atomic op on it to a
// libatomic call (a real link-time dependency this project avoids; see
// CLAUDE.md's "no 64-bit atomics on PPC32" constraint), and a plain 64-bit
// store is really two 32-bit stores, so a torn read is a real possibility,
// not just a theoretical ISO C gap. The sole writer is the mesher worker
// thread (chunk_mesher_build, below); the sole reader is the main thread's
// debug overlay (chunk_mesher_lod_stats -> screen_ingame.c; NOT read by the
// watchdog thread). Given that reader, each 64-bit counter is split into two
// independently __atomic 32-bit halves (lo/hi) instead of reaching for a
// lock: this getter runs every overlay frame on the main/render thread, and
// a lock here would stall render on the mesher worker (priority 32, can be
// starved for long stretches under chunk-load pressure) -- exactly the kind
// of stall this instrument exists to help diagnose, not cause. The only
// residual risk is a reader observing a torn hi:lo pair mid-carry (the low
// word wraps once every ~4 billion accumulated vertices -- not reachable in
// any real session); the result is a momentarily wrong debug ratio that
// self-corrects next frame, the same "approximate value is all the overlay
// needs" tolerance already used for the float gauges above. g_lod_chunks is
// a plain uint32_t (no tearing possible on PPC32) and just needs the
// visibility/atomicity __atomic gives for free.
static uint32_t g_lod_vertices_lo[2] = {0, 0};
static uint32_t g_lod_vertices_hi[2] = {0, 0};
static uint32_t g_lod_chunks[2] = {0, 0};

void chunk_mesher_lod_stats(uint64_t verts[2], uint32_t chunks[2]) {
	for(int i = 0; i < 2; i++) {
		if(verts) {
			uint32_t lo = __atomic_load_n(&g_lod_vertices_lo[i], __ATOMIC_RELAXED);
			uint32_t hi = __atomic_load_n(&g_lod_vertices_hi[i], __ATOMIC_RELAXED);
			verts[i] = ((uint64_t)hi << 32) | lo;
		}
		if(chunks)
			chunks[i] = __atomic_load_n(&g_lod_chunks[i], __ATOMIC_RELAXED);
	}
}

// T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md):
// cumulative geomip patches considered vs merged. Sole writer is the mesher worker
// thread (lod_geomip_compute_plan, below); sole reader is the main thread (debug
// overlay / bench_far_geomip). Same CR-08 __atomic-plain-uint32_t treatment as
// g_lod_chunks above -- no tearing possible on PPC32, just needs the
// visibility/atomicity __atomic gives for free.
static uint32_t g_geomip_considered = 0;
static uint32_t g_geomip_merged = 0;

void chunk_mesher_geomip_stats(uint32_t* considered, uint32_t* merged) {
	if(considered)
		*considered = __atomic_load_n(&g_geomip_considered, __ATOMIC_RELAXED);
	if(merged)
		*merged = __atomic_load_n(&g_geomip_merged, __ATOMIC_RELAXED);
}

// Sub-phase breakdown (ms) of whichever build had the max total — captured
// alongside mesher_build_ms_max so we can see what dominates the worst-case
// (cold) build: vertex-light precompute, vertex emit, reachability BFS, or
// display-list finalize. Worker-thread writes, main-thread (overlay) reads.
// CR-08: same plain-float + __atomic treatment as mesher_build_ms_last/max
// above (not read by the watchdog thread either).
static float mesher_max_vl = 0.0F;  // vertex_light precompute
static float mesher_max_em = 0.0F;  // vertex emit (rebuild - vl)
static float mesher_max_bfs = 0.0F; // chunk_test_init reachability
static float mesher_max_fin = 0.0F; // displaylist finalize/destroy
// Set by chunk_mesher_rebuild (worker thread), consumed by chunk_mesher_build
// on the same thread — not cross-thread, so no atomics needed (CR-08 audit
// confirms this one: single-thread-owned, verified by reading every call
// site of chunk_mesher_rebuild and g_vlight_ms -- both worker-thread only).
static float g_vlight_ms = 0.0F;

void chunk_mesher_build_breakdown(float* vl, float* em, float* bfs,
								  float* fin) {
	if(vl)
		*vl = mesher_atomic_load_f(&mesher_max_vl);
	if(em)
		*em = mesher_atomic_load_f(&mesher_max_em);
	if(bfs)
		*bfs = mesher_atomic_load_f(&mesher_max_bfs);
	if(fin)
		*fin = mesher_atomic_load_f(&mesher_max_fin);
}

// Scratch arena for the mesher thread. Holds the per-build `visited`
// bitmap (~4 KB) and `light_data` cache (~17.5 KB) so they don't go
// through malloc/free on every chunk. Reset at the start of each
// chunk_mesher_build. Sized to comfortably cover worst-case
// simultaneous usage with headroom for future per-job scratches.
//
// Static BSS buffer with explicit 32-byte alignment rather than a
// runtime memalign call: portable across devkitPPC and the MSYS2 PC
// toolchain (which doesn't expose memalign in <malloc.h>), and the
// 32-byte alignment is required so that GX-bound buffers carved out
// of the arena land on cache-line boundaries.
#define MESHER_ARENA_SIZE (64 * 1024)
static uint8_t mesher_arena_buffer[MESHER_ARENA_SIZE]
	__attribute__((aligned(32)));
static struct arena mesher_arena;

// Neighbour-admission predicate for the reachability BFS, precomputed per
// block type: see_through[t] == (!blocks[t] || blocks[t]->can_see_through).
// Same idea as ao_light_passes — one table load per neighbour instead of a
// blocks[type] pointer deref + flag read.
static uint8_t bfs_see_through[256];
static bool bfs_see_through_ready = false;

static void bfs_build_see_through(void) {
	for(int t = 0; t < 256; t++) {
		struct block* b = blocks[t];
		bfs_see_through[t] = (!b || b->can_see_through) ? 1 : 0;
	}
	bfs_see_through_ready = true;
}

/*
	Occlusion-culling reachability flood — optimized, output-identical to the
	original. A cell (x,y,z) packs to its BLK_INDEX2 visited-index
	i = x | (z<<4) | (y<<8) (valid because CHUNK_SIZE == 16); decode with
	masks. Speedups vs the original, all preserving the reachable[] output:
	  * a plain int index stack (no 3-byte memcpy per push/pop — an air chunk
	    does ~4096 of each),
	  * bfs_see_through[256] (no blocks[type] deref per neighbour),
	  * inlined +-neighbour index arithmetic (no blocks_side_offset call, no
	    BLK_INDEX2/BLK_DATA recompute): the bd base index is computed once per
	    popped cell and offset by constant strides.
	reachable[] is order-independent (it OR-accumulates the boundary sides a
	connected region touches), so the changed visit order does not affect the
	result. Covered by tests/test_chunk_reachable_equiv.c.
*/
static void chunk_test(struct block_data* bd, int* stk, bool* visited,
					   uint8_t* reachable, c_coord_t x, c_coord_t y,
					   c_coord_t z) {
	assert(bd && stk && visited && reachable);

	const int VZ = CHUNK_SIZE;                          // visited z stride
	const int VY = CHUNK_SIZE * CHUNK_SIZE;             // visited y stride
	const int BZ = CHUNK_SIZE + 2;                      // bd z stride
	const int BY = (CHUNK_SIZE + 2) * (CHUNK_SIZE + 2); // bd y stride

	int seed = (int)(x | (z << 4) | (y << 8));
	int seed_bi = (int)((x + 1) + (z + 1) * BZ + (y + 1) * BY);
	if(visited[seed] || !bfs_see_through[bd[seed_bi].type])
		return;

	int sp = 0;
	stk[sp++] = seed;
	visited[seed] = true;

	uint8_t reached_sides = 0;

	while(sp > 0) {
		int i = stk[--sp];
		int cx = i & 15, cz = (i >> 4) & 15, cy = (i >> 8) & 15;
		int bi = (cx + 1) + (cz + 1) * BZ + (cy + 1) * BY;

		if(cx == 0)
			reached_sides |= (1 << SIDE_LEFT);
		if(cx == CHUNK_SIZE - 1)
			reached_sides |= (1 << SIDE_RIGHT);
		if(cy == 0)
			reached_sides |= (1 << SIDE_BOTTOM);
		if(cy == CHUNK_SIZE - 1)
			reached_sides |= (1 << SIDE_TOP);
		if(cz == 0)
			reached_sides |= (1 << SIDE_FRONT);
		if(cz == CHUNK_SIZE - 1)
			reached_sides |= (1 << SIDE_BACK);

#define BFS_TRY(ok, ni, nbi)                                                   \
	do {                                                                       \
		if((ok) && !visited[ni] && bfs_see_through[bd[nbi].type]) {             \
			visited[ni] = true;                                                \
			stk[sp++] = (ni);                                                  \
		}                                                                      \
	} while(0)

		BFS_TRY(cx > 0, i - 1, bi - 1);
		BFS_TRY(cx < CHUNK_SIZE - 1, i + 1, bi + 1);
		BFS_TRY(cz > 0, i - VZ, bi - BZ);
		BFS_TRY(cz < CHUNK_SIZE - 1, i + VZ, bi + BZ);
		BFS_TRY(cy > 0, i - VY, bi - BY);
		BFS_TRY(cy < CHUNK_SIZE - 1, i + VY, bi + BY);

#undef BFS_TRY
	}

	for(int s = 0; s < 6; s++) {
		if(reached_sides & (1 << s))
			reachable[s] |= reached_sides;
	}
}

// Returns true when reachability was fully computed; false when a scratch
// total-OOM forced the conservative fully-connected fill, so the caller can
// schedule a retry instead of committing the degraded result as a success.
static bool chunk_test_init(struct arena* arena, struct block_data* bd,
							uint8_t* reachable) {
	assert(arena && bd && reachable);

	memset(reachable, 0, 6 * sizeof(uint8_t));

	// `visited` was a 4 KB malloc per chunk mesh; now lives in the
	// per-job arena and is reset (along with everything else) at the
	// start of the next chunk_mesher_build. On arena exhaustion the
	// helper logs a warning and malloc's instead; we free at function
	// exit. On total malloc failure, treat the chunk as fully
	// connected from every side (conservative: over-renders rather
	// than under-rendering) and report the degradation to the caller.
	bool visited_fallback = false;
	bool* visited = mesher_scratch_alloc(
		arena, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE, 4, &visited_fallback);
	if(!visited) {
		fprintf(stderr,
				"[chunk_mesher] chunk_test_init: skipping reachability "
				"computation; chunk will be treated as fully connected.\n");
		memset(reachable, 0x3F, 6 * sizeof(uint8_t));
		return false;
	}

	memset(visited, false, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE);

	// BFS stack. A plain int index stack (one packed BLK_INDEX2 per cell) —
	// no per-push/pop memcpy. Each cell is pushed at most once (the visited
	// guard precedes every push), so CHUNK_SIZE^3 entries is the exact
	// worst-case depth; it never overflows. Same arena + malloc-fallback
	// discipline as `visited`.
	bool stk_fallback = false;
	int* stk = mesher_scratch_alloc(
		arena, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * sizeof(int), 4,
		&stk_fallback);
	if(!stk) {
		fprintf(stderr,
				"[chunk_mesher] chunk_test_init: stack alloc failed; chunk "
				"will be treated as fully connected.\n");
		memset(reachable, 0x3F, 6 * sizeof(uint8_t));
		mesher_scratch_free(visited, visited_fallback);
		return false;
	}

	if(!bfs_see_through_ready)
		bfs_build_see_through();

	for(int y = 0; y < CHUNK_SIZE; y++) {
		for(int x = 0; x < CHUNK_SIZE; x++) {
			chunk_test(bd, stk, visited, reachable, x, y, 0);
			chunk_test(bd, stk, visited, reachable, x, 0, y);
			chunk_test(bd, stk, visited, reachable, 0, x, y);
			chunk_test(bd, stk, visited, reachable, x, y, CHUNK_SIZE - 1);
			chunk_test(bd, stk, visited, reachable, x, CHUNK_SIZE - 1, y);
			chunk_test(bd, stk, visited, reachable, CHUNK_SIZE - 1, x, y);
		}
	}

	// Both scratch buffers came from the arena (reset next job) or a malloc
	// fallback; the helper frees only the fallback case.
	mesher_scratch_free(stk, stk_fallback);
	mesher_scratch_free(visited, visited_fallback);
	return true;
}

#ifndef SETTINGS_FANCY_AO
/*
	Fast per-face lighting path. When SETTINGS_FANCY_AO is undefined in
	source/graphics/gfx_settings.h, replaces the 4-block-averaged AO
	routine below with a single-sample-per-axis lookup. Each
	light_data[idx*3 + axis] entry stores the light of one canonical
	neighbour (the block below for axis 0, left for axis 1, in front for
	axis 2). All four corner vertices of a given face end up reading the
	same value, so no AO darkening — but the lookup count drops from
	~24 per cell to 3, which is ~4x faster on the Wii.

	This path was added as cleanup task #9 after the perf milestone. The
	inherited "TODO: clean up horrible code" comment (formerly at line
	272) referred to the AO routine's triple-axis triple-nest with four
	inlined block_data fetches and four averages per axis. Rather than
	re-do the AO logic in place, the cleanup splits it into two paths
	and defaults to fancy.
*/
static void chunk_mesher_vertex_light(struct block_data* bd,
									  uint8_t* light_data) {
	assert(bd && light_data);

	for(c_coord_t y = 0; y < CHUNK_SIZE + 2; y++) {
		for(c_coord_t z = 0; z < CHUNK_SIZE + 2; z++) {
			for(c_coord_t x = 0; x < CHUNK_SIZE + 2; x++) {
				size_t idx = BLK_INDEX(x, y, z);

				// axis 0 = top/bottom face — sample block below.
				if(x != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct block_data b = BLK_DATA(bd, x, y - 1, z);
					light_data[idx * 3 + 0]
						= (b.torch_light << 4) | b.sky_light;
				}

				// axis 1 = left/right face — sample block to the left.
				if(y != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct block_data b = BLK_DATA(bd, x - 1, y, z);
					light_data[idx * 3 + 1]
						= (b.torch_light << 4) | b.sky_light;
				}

				// axis 2 = front/back face — sample block in front.
				if(x != CHUNK_SIZE + 1 && y != CHUNK_SIZE + 1) {
					struct block_data b = BLK_DATA(bd, x, y, z - 1);
					light_data[idx * 3 + 2]
						= (b.torch_light << 4) | b.sky_light;
				}
			}
		}
	}
}
#else
/*
	Smooth-lighting (AO) path — optimized, bit-exact rewrite of the original
	4-sample-average routine. The math is unchanged: same four samples per
	axis, same round-up divide by count, same shade-table subtraction and
	clamp, so the output `light_data` is byte-for-byte identical and Beta
	1.7.3's signature smooth lighting is preserved exactly. The speedups are
	pure redundancy elimination on the single-core Wii's hottest mesh loop:

	  * ao_light_passes[256] precomputes the per-block-type light predicate
	    once (it's constant after blocks_init), replacing a blocks[type]
	    pointer deref plus two flag derefs *per sample* with a single table
	    load.
	  * Each axis's four samples are reached by constant pointer offsets from
	    one base pointer (strides ±1 / ±(N+2) / ±(N+2)^2), instead of the
	    original four BLK_DATA macro expansions that each recomputed a
	    multiply-heavy 3D->1D index.

	Equivalence is covered by tests/test_vertex_light_equiv.c, which runs this
	sampling/averaging against a reference implementation over randomized
	grids (including the boundary shell) and asserts identical output.
*/

static uint8_t ao_light_passes[256];
static bool ao_light_passes_ready = false;

static void ao_build_passes(void) {
	for(int t = 0; t < 256; t++) {
		struct block* b = blocks[t];
		ao_light_passes[t]
			= (!b || (b->can_see_through && !b->ignore_lighting)) ? 1 : 0;
	}
	ao_light_passes_ready = true;
}

// Average sky/torch over the (up to) four samples that pass the light test,
// round up, subtract the AO shade term for that count, clamp at 0. Identical
// arithmetic to the original inline block.
static inline uint8_t ao_average(const struct block_data* s0,
								 const struct block_data* s1,
								 const struct block_data* s2,
								 const struct block_data* s3,
								 const uint8_t* passes) {
	static const int shade_table[5] = {0, 5, 3, 1, 0};
	const struct block_data* s[4] = {s0, s1, s2, s3};
	int sum_sky = 0, sum_torch = 0, count = 0;
	for(int k = 0; k < 4; k++) {
		if(passes[s[k]->type]) {
			sum_sky += s[k]->sky_light;
			sum_torch += s[k]->torch_light;
			count++;
		}
	}
	sum_torch = count > 0 ? (sum_torch + count - 1) / count : 0;
	sum_sky = count > 0 ? (sum_sky + count - 1) / count : 0;
	sum_torch = MAX(sum_torch - shade_table[count], 0);
	sum_sky = MAX(sum_sky - shade_table[count], 0);
	return (uint8_t)((sum_torch << 4) | sum_sky);
}

static void chunk_mesher_vertex_light(struct block_data* bd,
									  uint8_t* light_data) {
	assert(bd && light_data);

	if(!ao_light_passes_ready)
		ao_build_passes();
	const uint8_t* passes = ao_light_passes;

	const int SX = 1;                                   // x neighbour stride
	const int SZ = CHUNK_SIZE + 2;                      // z neighbour stride
	const int SY = (CHUNK_SIZE + 2) * (CHUNK_SIZE + 2); // y neighbour stride

	for(c_coord_t y = 0; y < CHUNK_SIZE + 2; y++) {
		for(c_coord_t z = 0; z < CHUNK_SIZE + 2; z++) {
			for(c_coord_t x = 0; x < CHUNK_SIZE + 2; x++) {
				size_t out = BLK_INDEX(x, y, z) * 3;

				// axis 0 (top/bottom): the y-1 plane —
				// (x,y-1,z) (x-1,y-1,z) (x,y-1,z-1) (x-1,y-1,z-1)
				if(x != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct block_data* b = &BLK_DATA(bd, x, y - 1, z);
					light_data[out + 0]
						= ao_average(b, b - SX, b - SZ, b - SZ - SX, passes);
				}

				// axis 1 (left/right): the x-1 plane —
				// (x-1,y,z) (x-1,y-1,z) (x-1,y,z-1) (x-1,y-1,z-1)
				if(y != CHUNK_SIZE + 1 && z != CHUNK_SIZE + 1) {
					struct block_data* b = &BLK_DATA(bd, x - 1, y, z);
					light_data[out + 1]
						= ao_average(b, b - SY, b - SZ, b - SY - SZ, passes);
				}

				// axis 2 (front/back): the z-1 plane —
				// (x,y,z-1) (x-1,y,z-1) (x,y-1,z-1) (x-1,y-1,z-1)
				if(x != CHUNK_SIZE + 1 && y != CHUNK_SIZE + 1) {
					struct block_data* b = &BLK_DATA(bd, x, y, z - 1);
					light_data[out + 2]
						= ao_average(b, b - SX, b - SY, b - SY - SX, passes);
				}
			}
		}
	}
}
#endif

// --- M1 far-ring LOD silhouette skirts ---------------------------------------
// The terrain portion of the surface skin drops side faces, so a hill viewed
// side-on shows gaps where a column steps down to a lower neighbour. This
// dedicated per-column pass closes them: one merged vertical "skirt" quad per
// stepped terrain-column side. Tree logs/leaves keep their real block hull and
// never rely on this stretched terrain approximation.
//
// It keys off COLUMN SURFACE HEIGHTS (not per-block air-visibility), so it
// structurally cannot emit interior cave-wall faces -- the trap that would re-inflate
// the vertex count. It runs only for lod_skin meshes; the full (lod=0) mesh never
// enters here, so that path stays byte-identical.
//
// Coords are w_coord_t (SIGNED) on purpose: edge columns read the +-1 ring at local
// index -1/16, and an unsigned c_coord_t would wrap -1 to a huge index and read out
// of bounds for the whole chunk perimeter (corrupting every boundary column's
// neighbour height -> spurious + missing skirts exactly at chunk seams).

enum { LOD_BLK_LEN = 256 }; // 1 block in 1/256-block emit units (== render BLK_LEN)

// This column's section-local TERRAIN surface = the highest local y in [0..15]
// whose block occludes the silhouette (not see-through), excluding tree logs.
// Logs keep their real block hull in the surface LOD; treating a treetop as the
// terrain surface stretches its skirt down to the ground as a bare log tower.
// Returns -1 if the section contains no skirt surface.
static int lod_surface_height(struct block_data* bd, w_coord_t x, w_coord_t z) {
	for(w_coord_t y = CHUNK_SIZE - 1; y >= 0; y--) {
		uint8_t type = BLK_DATA(bd, x, y, z).type;
		struct block* b = blocks[type];
		if(b && chunk_lod_skirt_surface_allowed(type, b->can_see_through))
			return (int)y;
	}
	return -1;
}

static void lod_emit_skirts(struct block_data* bd, w_coord_t cx, w_coord_t cy,
							w_coord_t cz, struct displaylist* d, size_t* vertices) {
	// Section-local surface height per column, scanned once.
	int surf[CHUNK_SIZE * CHUNK_SIZE];
	for(w_coord_t z = 0; z < CHUNK_SIZE; z++)
		for(w_coord_t x = 0; x < CHUNK_SIZE; x++)
			surf[x + z * CHUNK_SIZE] = lod_surface_height(bd, x, z);

	static const struct {
		int dx, dz;
		enum side side;
	} dirs[4] = {
		{-1, 0, SIDE_LEFT},
		{1, 0, SIDE_RIGHT},
		{0, -1, SIDE_FRONT},
		{0, 1, SIDE_BACK},
	};

	for(w_coord_t z = 0; z < CHUNK_SIZE; z++) {
		for(w_coord_t x = 0; x < CHUNK_SIZE; x++) {
			int H = surf[x + z * CHUNK_SIZE];
			if(H < 0)
				continue; // empty (all-see-through) column

			struct block_data sbd = BLK_DATA(bd, x, H, z);
			struct block* sb = blocks[sbd.type];
			if(!sb || !sb->getTextureIndex)
				continue;

			// Build the surface block's 6 neighbours so getTextureIndex is safe
			// (grass/snow read neighbours[SIDE_TOP]); mirrors the main emit loop.
			struct block_data nbd[6];
			for(int k = 0; k < SIDE_MAX; k++) {
				int ox, oy, oz;
				blocks_side_offset((enum side)k, &ox, &oy, &oz);
				nbd[k] = BLK_DATA(bd, x + ox, H + oy, z + oz);
			}
			struct block_info sinfo = (struct block_info) {
				.block = &sbd,
				.neighbours = nbd,
				.x = cx + x,
				.y = cy + H,
				.z = cz + z,
			};

			// Skirt light comes from the AIR CELL the skirt face abuts -- NOT from
			// the opaque surface block's own stored light. Several real paths
			// (demoted full->skin chunks, underwater shelves, slopes crossing a
			// section plane, relit edits) leave the surface block's stored sky at
			// 0, which painted the 16x-stretched skirt walls near-BLACK: the
			// "black lighting errors" / dark-pillars-in-the-water artifacts of the
			// on-device playtests. The cell just above the NEIGHBOUR's surface is
			// sky-lit by definition; emissive surface blocks still lift the torch
			// term via luminance. Computed per-direction inside the loop below.
			uint8_t lum = sb->luminance;

			for(int dch = 0; dch < 4; dch++) {
				w_coord_t nx = x + dirs[dch].dx;
				w_coord_t nz = z + dirs[dch].dz;
				int Hn;
				if(nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE)
					Hn = surf[nx + nz * CHUNK_SIZE];
				else
					Hn = lod_surface_height(bd, nx, nz); // ring (signed -1/16 ok)

				if(Hn >= H)
					continue; // neighbour not lower -> no silhouette step

				// Air cell adjacent to this skirt face: just above the neighbour
				// column's surface (in-bounds: Hn < H <= 15). When the neighbour
				// column is all-see-through in this section (Hn < 0), fall back
				// to the air above OUR surface (y = H+1 <= 16; the mesher buffer
				// carries the +1 ring -- the main loop reads it for SIDE_TOP).
				struct block_data abd = (Hn >= 0) ? BLK_DATA(bd, nx, Hn + 1, nz)
												  : BLK_DATA(bd, x, H + 1, z);
				uint8_t tl = abd.torch_light > lum ? abd.torch_light : lum;
				uint8_t light = (tl << 4) | abd.sky_light;
				uint8_t vl[24];
				for(int i = 0; i < 24; i++)
					vl[i] = light;

				// Half-open band [ylo .. H+1): from one above the neighbour surface
				// (or the section floor if it drops below this section) up to this
				// column's surface top. The section below continues the silhouette
				// from its own top, so the two tile edge-to-edge -- no gap, no
				// double-draw at the shared section-boundary plane.
				int ylo = (Hn < 0) ? 0 : Hn + 1;
				int ytop = H + 1;
				uint16_t height = (uint16_t)((ytop - ylo) * LOD_BLK_LEN);

				enum side side = dirs[dch].side;
				uint8_t tex = sb->getTextureIndex(&sinfo, side);
				// DC-GRASS G2: the grass side cell is the raw tile 3 now (the full
				// mesh adds a tinted tile-38 overlay quad). A far-ring skirt is one
				// stretched untinted quad, so it keeps its pre-G2 look by sampling
				// the baked composite cell (tile 38 x fallback over tile 3) --
				// byte-identical texels to the old TEXAT_GRASS_SIDE cell, pinned
				// by test_mesher_nongrass_equiv's lod-grass scene.
				if(sbd.type == BLOCK_GRASS
				   && tex == tex_atlas_lookup(TEXAT_GRASS_SIDE))
					tex = tex_atlas_lookup(TEXAT_GRASS_SIDE_ITEM);

				int16_t py = (int16_t)(ylo * LOD_BLK_LEN);
				int16_t px, pz;
				switch(side) {
				case SIDE_RIGHT:
					px = (int16_t)((x + 1) * LOD_BLK_LEN);
					pz = (int16_t)(z * LOD_BLK_LEN);
					break;
				case SIDE_BACK:
					px = (int16_t)(x * LOD_BLK_LEN);
					pz = (int16_t)((z + 1) * LOD_BLK_LEN);
					break;
				case SIDE_LEFT:
				case SIDE_FRONT:
				default:
					px = (int16_t)(x * LOD_BLK_LEN);
					pz = (int16_t)(z * LOD_BLK_LEN);
					break;
				}

				vertices[side] += render_block_lod_skirt(d + side, px, py, pz,
														 LOD_BLK_LEN, height, tex,
														 side, vl, lum)
					* 4;
			}
		}
	}
}

// --- T2 SPIKE: far-ring geomip error-metric merge ---------------------------
// (opt-in, PC-measured only; hardware-unconfirmed -- see
// docs/far_ring_geomip_design_2026-08-26.md for the full design, the
// measurement plan, and every simplification called out below.)
//
// Scans this chunk's 16x16 skin columns as an 8x8 grid of DISJOINT 2x2 patches
// (a single fixed-size partition, not the recursive quadtree a full land would
// want -- see the design doc's "spike, not the full land" scope note) and
// decides which are safe to collapse into one quad. Only ever called for
// lod_skin builds; when `enabled` is false this returns 0 immediately and
// leaves `suppress`/`anchor_size` all zero, touching NEITHER the geomip
// counters below NOR world_lod_geomip_projection_k/error_budget_px -- this is
// the same gate chunk_lod_geomip.h's lod_geomip_patch_mergeable itself
// enforces, and tests/test_lod_geomip_policy.c proves it exhaustively at the
// header level the same way test_lod_tree_policy.c proves
// chunk_lod_face_allowed's LOD0 gate.
//
// `suppress[i]` (i = x*CHUNK_SIZE+z) marks a column whose own SIDE_TOP face
// must NOT be emitted (it is covered by its patch anchor's merged quad);
// `anchor_size[i]` > 0 marks the anchor column (the patch's lowest-x,lowest-z
// corner) and gives the merged footprint's edge length IN COLUMNS (always 2 in
// this spike).
//
// A patch merges only when: every corner has a real skin surface (the -1
// sentinel from lod_surface_height is never mergeable); none of the four is a
// tree log (chunk_lod_face_allowed already keeps logs off the terrain skin
// path, but a demoted full->skin chunk could still report one here --
// defensive, matches lod_surface_height's own tree exclusion); all four
// corners are the SAME top block id (a material boundary -- e.g. grass meeting
// stone -- always keeps its own quads); IF that shared id is biome-tinted
// (BIOME_TINT_GRASS -- the only tint that can reach the terrain skin surface;
// BIOME_TINT_FOLIAGE blocks are can_see_through and lod_surface_height already
// skips them) AND a section tint is actually loaded (`has_tint`), all four
// corners must additionally share the SAME biome tint index (grass_i/grass_j
// -- invisible to chunk_lod_geomip.h, so checked here per that header's
// documented caller contract). When has_tint is false, or the block isn't
// tinted, the rendered tint is uniformly white regardless of grass_i/grass_j
// (see the has_tint ternary in the grass_rgb/foliage_rgb precompute above) --
// checking raw index equality in that case would be meaningless (or, worse,
// could read this chunk's stale previous-build RPC-slot bytes when has_tint is
// false, since world_get_section_tint then leaves the buffer untouched), so
// it is skipped entirely. Finally, the screen-space error of collapsing to the
// anchor's own height must stay within the configured budget
// (chunk_lod_geomip.h's header comment explains why the anchor, not the patch
// average, is the reference: it is what is actually drawn, and keeps the
// anchor corner watertight against the UNCHANGED per-column skirt pass above,
// which this spike does not touch).
static uint32_t lod_geomip_compute_plan(struct block_data* bd, bool enabled,
										uint16_t dist_blocks, const uint8_t* grass_i,
										const uint8_t* grass_j, bool has_tint,
										bool suppress[CHUNK_SIZE * CHUNK_SIZE],
										uint8_t anchor_size[CHUNK_SIZE * CHUNK_SIZE]) {
	memset(suppress, 0, CHUNK_SIZE * CHUNK_SIZE * sizeof(bool));
	memset(anchor_size, 0, CHUNK_SIZE * CHUNK_SIZE * sizeof(uint8_t));
	if(!enabled)
		return 0;

	float k = world_lod_geomip_projection_k();
	float budget = world_lod_geomip_error_budget_px();
	uint32_t considered = 0, merged = 0;

	for(int pz = 0; pz < CHUNK_SIZE; pz += 2) {
		for(int px = 0; px < CHUNK_SIZE; px += 2) {
			int h0 = lod_surface_height(bd, px, pz); // anchor corner
			int h1 = lod_surface_height(bd, px + 1, pz);
			int h2 = lod_surface_height(bd, px, pz + 1);
			int h3 = lod_surface_height(bd, px + 1, pz + 1);
			considered++;

			if(h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
				continue;

			uint8_t id0 = BLK_DATA(bd, px, h0, pz).type;
			uint8_t id1 = BLK_DATA(bd, px + 1, h1, pz).type;
			uint8_t id2 = BLK_DATA(bd, px, h2, pz + 1).type;
			uint8_t id3 = BLK_DATA(bd, px + 1, h3, pz + 1).type;

			if(id0 == BLOCK_LOG || id1 == BLOCK_LOG || id2 == BLOCK_LOG
			   || id3 == BLOCK_LOG)
				continue;

			if(id0 != id1 || id0 != id2 || id0 != id3)
				continue; // material boundary -- lod_geomip_patch_mergeable
							// would also catch this, but checking it here first
							// means the tint-index check below never runs for
							// a mismatched patch, and blocks[id0] below is a
							// single, already-validated id.

			int i0 = px * CHUNK_SIZE + pz, i1 = (px + 1) * CHUNK_SIZE + pz,
				i2 = px * CHUNK_SIZE + (pz + 1),
				i3 = (px + 1) * CHUNK_SIZE + (pz + 1);

			bool tint_matters = has_tint && blocks[id0]
				&& blocks[id0]->biome_tint != BIOME_TINT_NONE;
			if(tint_matters
			   && (grass_i[i0] != grass_i[i1] || grass_i[i0] != grass_i[i2]
				   || grass_i[i0] != grass_i[i3] || grass_j[i0] != grass_j[i1]
				   || grass_j[i0] != grass_j[i2] || grass_j[i0] != grass_j[i3]))
				continue;

			if(!lod_geomip_patch_mergeable(true, h0, h1, h2, h3, id0, id1, id2,
										   id3, (float)dist_blocks, budget, k))
				continue;

			anchor_size[i0] = 2;
			suppress[i1] = suppress[i2] = suppress[i3] = true;
			merged++;
		}
	}

	__atomic_fetch_add(&g_geomip_considered, considered, __ATOMIC_RELAXED);
	__atomic_fetch_add(&g_geomip_merged, merged, __ATOMIC_RELAXED);
	return merged;
}

// Foliage distance LOD (render-only). The per-chunk band (request.foliage_lod) is set by
// world_update_lod from camera distance (thresholds there). band>=1: small plants culled +
// leaves collapse to a solid silhouette (internal leaf-leaf faces dropped, canopy hull
// kept); band>=2: tall grass also culled. The blocks stay in the world -- walking closer
// re-meshes them at full detail. Sheds sub-pixel distant foliage to save vertices/memory.

// Small cross/billboard plants dropped at band>=1 (sub-pixel + a quad each).
static inline bool foliage_lod_is_small_plant(uint8_t type) {
	switch(type) {
		case BLOCK_SAPLING:		   // 6
		case 32:				   // dead bush (no BLOCK_ constant in the fork)
		case BLOCK_FLOWER:		   // 37 dandelion
		case BLOCK_ROSE:		   // 38
		case BLOCK_BROWM_MUSHROOM: // 39 (sic: fork spelling)
		case BLOCK_RED_MUSHROOM:   // 40
			return true;
		default:
			return false;
	}
}

// block_info.get_block backing for the mesher: read the padded `bd` SNAPSHOT by world coord.
// Runs on the mesher WORKER thread, so it must NEVER touch the live world (single-threaded dict);
// the snapshot already holds the chunk + its 1-block border, which is exactly the 3x3x3 reach the
// redstone-wire renderer needs. Anything outside that border reports "absent" (treated as air).
struct mesher_neighbour_ctx {
	const struct block_data* bd;
	w_coord_t cx, cy, cz;
};
static bool mesher_neighbour_get(void* vctx, w_coord_t x, w_coord_t y, w_coord_t z,
								 struct block_data* out) {
	const struct mesher_neighbour_ctx* c = vctx;
	int lx = (int)(x - c->cx), ly = (int)(y - c->cy), lz = (int)(z - c->cz);
	if(lx < -1 || lx > CHUNK_SIZE || ly < -1 || ly > CHUNK_SIZE || lz < -1
	   || lz > CHUNK_SIZE)
		return false;
	*out = BLK_DATA(c->bd, lx, ly, lz);
	return true;
}

#ifdef CAVEWII_DISPLAYLIST_TEST
// P2-b test-only seam (test_chunk_mesher_slice_env.c is the only target that
// calls this): CHUNK_MESHER_SLICE_MS is a compile constant because production
// never needs to change it, but a real chunk on PC meshes in well under 2 ms,
// so the slice would never fire under test with the real threshold -- making
// an "on vs off" comparison vacuous. -1 (default) means "use
// CHUNK_MESHER_SLICE_MS unchanged"; any value >= 0 overrides it, letting the
// test force the slice to fire on every emit checkpoint (0) or never
// (INT_MAX) against the SAME chunk data, then compare the finalized mesh
// bytes.
static int s_mesher_slice_ms_override = -1;

void chunk_mesher_test_set_slice_ms(int ms) {
	s_mesher_slice_ms_override = ms;
}
#endif

static inline int mesher_slice_threshold_ms(void) {
#ifdef CAVEWII_DISPLAYLIST_TEST
	if(s_mesher_slice_ms_override >= 0)
		return s_mesher_slice_ms_override;
#endif
	return CHUNK_MESHER_SLICE_MS;
}

// H-1 HOTFIX (2026-09-03 responsiveness lane): moved from the per-list FINALIZE
// loop (post-commit) into the EMIT loop below, and changed from "elapsed since
// build start > threshold -> sleep every remaining iteration" (which could
// accumulate up to ~(CHUNK_MESH_LISTS-1) x 1ms = ~25ms of dead wall-clock
// AFTER every byte was already committed, because slice_elapsed_ms only ever
// grows, so once it crossed the threshold every later list slept too) to a
// proper wall-clock BUDGET: sleep 1ms at most once per >= CHUNK_MESHER_SLICE_MS
// of emit work since the last yield, then reset the mark. This still yields
// the CPU cooperatively while a build runs (letting main/server back onto the
// single core promptly instead of only once the whole build happens to end),
// but never sleeps more than the elapsed-work/SLICE_MS ratio calls for, and it
// yields WHILE the mesh is still being produced instead of only after it has
// already been committed and is doing nothing useful with the delay. Pure
// pacing: nothing here is read by, or races with, anything else -- the worker
// is single-threaded end to end, so pausing mid-emit and resuming later
// produces the exact same bytes as never pausing at all
// (tests/test_chunk_mesher_slice_env.c).
static inline void mesher_emit_slice_tick(ptime_t* slice_mark) {
	float elapsed_ms = time_diff_s(*slice_mark, time_get()) * 1000.0F;
	if(elapsed_ms > (float)mesher_slice_threshold_ms()) {
		SETTLE(mesher_settle_observe, SET_SLEEP_BEGIN, NULL, NULL, 0, 0, 0, 0);
		thread_msleep(1);
		SETTLE(mesher_settle_observe, SET_SLEEP_END, NULL, NULL, 0, 0, 0, 0);
		*slice_mark = time_get();
	}
}

// Returns true when the emitted vertex light is faithful; false when a scratch
// total-OOM (arena exhausted AND the malloc fallback dead) forced the
// full-bright default light onto at least one face. The mesh is then complete
// and NULL-safe but wrongly lit, and the caller must schedule a retry -- until
// 2026-08-13 this path committed as a normal SUCCESS and the wrong lighting
// persisted until an unrelated edit re-dirtied the chunk (audit residual #1).
static bool chunk_mesher_rebuild(struct arena* arena, struct block_data* bd,
								 w_coord_t cx, w_coord_t cy, w_coord_t cz,
								 struct displaylist* d, bool count_only,
								 size_t* vertices, const uint8_t* grass_i,
								 const uint8_t* grass_j, bool has_tint,
								 bool lod_skin, uint8_t foliage_lod,
								 bool geomip_enabled,
								 uint16_t geomip_dist_blocks) {
	assert(arena && bd && d && vertices);

	// H-1: wall-clock mark for the cooperative emit-loop slice below (see
	// mesher_emit_slice_tick's declaration comment). Started here, at the top
	// of the actual emit work, not at chunk_mesher_build's build_t0 -- the
	// arena reset + per-list displaylist_init_presized calls that precede this
	// call are a fixed, sub-microsecond cost that never needs to yield.
	ptime_t slice_mark = time_get();

	// Snapshot-backed neighbour accessor for connection-aware renderers (redstone wire). Points
	// at this build's `bd`; valid for the whole rebuild (the render calls are synchronous below).
	struct mesher_neighbour_ctx nctx = {.bd = bd, .cx = cx, .cy = cy, .cz = cz};

	uint8_t* light_data = NULL;
	bool light_data_fallback = false;
	bool light_ok = true;

	g_vlight_ms = 0.0F; // accumulated when the lazy vertex_light fires below

	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		vertices[k] = 0;

	// Resolve each column's Beta grass/foliage tint once (RGB), so the per-block
	// loop is a cheap table read. Untinted columns (no stored tint) stay white,
	// which makes the per-vertex tint multiply a no-op.
	uint32_t grass_rgb[CHUNK_SIZE * CHUNK_SIZE];
	uint32_t foliage_rgb[CHUNK_SIZE * CHUNK_SIZE];
	for(int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; i++) {
		grass_rgb[i] = has_tint ? biome_grass_color(grass_i[i], grass_j[i])
								: 0xFFFFFFu;
		foliage_rgb[i] = has_tint ? biome_foliage_color(grass_i[i], grass_j[i])
								  : 0xFFFFFFu;
	}
	// DC-GRASS G2: the grass BLOCK's colour (top + side overlay) is the biome
	// colour (BlockGrass.java:23-28) when both the column's section tint and
	// the pack's grass colormap are present; otherwise the deterministic
	// CaveWii unavailable-data colour BIOME_GRASS_FALLBACK (biome_color.h --
	// not Beta, which always has a colour). Applied exactly ONCE: the grass
	// atlas cells are raw/untinted now. Other BIOME_TINT_GRASS users (tall
	// grass) keep grass_rgb unchanged.
	bool grass_known = has_tint && biome_grass_colormap_loaded();
	/* Oak leaves: with a real foliage colour the face samples the RAW oak
	 * cell and is tinted once (Beta: raw texture x colorMultiplier,
	 * BlockLeaves.java:19-29); without one it keeps the baked cell, white. */
	bool foliage_known = has_tint && biome_foliage_colormap_loaded();

	// T2 SPIKE: geomip merge plan (see lod_geomip_compute_plan above). Computed
	// unconditionally -- the function itself early-returns 0 with both arrays
	// left all-zero/false whenever geomip_enabled is false (the default), so
	// this costs one cheap function call and two memsets on the flag-off path,
	// touches no other state, and every check against these arrays in the main
	// loop below is then trivially false.
	bool geomip_suppress[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t geomip_anchor_size[CHUNK_SIZE * CHUNK_SIZE];
	if(lod_skin)
		lod_geomip_compute_plan(bd, geomip_enabled, geomip_dist_blocks, grass_i,
								grass_j, has_tint, geomip_suppress,
								geomip_anchor_size);
	else {
		memset(geomip_suppress, 0, sizeof(geomip_suppress));
		memset(geomip_anchor_size, 0, sizeof(geomip_anchor_size));
	}

	for(c_coord_t y = 0; y < CHUNK_SIZE; y++) {
		for(c_coord_t z = 0; z < CHUNK_SIZE; z++) {
			for(c_coord_t x = 0; x < CHUNK_SIZE; x++) {
				struct block_data local = BLK_DATA(bd, x, y, z);

				// Foliage LOD: distant chunks shed small plants (band>=1) and tall
				// grass (band>=2) -- they're a quad each and sub-pixel at range.
				if(foliage_lod >= 1
				   && (foliage_lod_is_small_plant(local.type)
					   || (foliage_lod >= 2 && local.type == BLOCK_TALL_GRASS)))
					continue;

				if(blocks[local.type]) {
					// Hoist the block def: it is invariant across all 6 sides,
					// but the compiler must re-load blocks[local.type] each time
					// because the opaque getSideMask/renderBlock indirect calls
					// could alias blocks[]. ~60 redundant indexed loads/block.
					struct block* lb = blocks[local.type];
					struct block_data neighbours[6];
					struct block_info neighbours_info[6];

					for(int k = 0; k < SIDE_MAX; k++) {
						enum side s = (enum side)k;

						int ox, oy, oz;
						blocks_side_offset(s, &ox, &oy, &oz);

						neighbours[k] = BLK_DATA(bd, x + ox, y + oy, z + oz);

						neighbours_info[k] = (struct block_info) {
							.block = neighbours + k,
							.neighbours = NULL,
							.x = cx + x + ox,
							.y = cy + y + oy,
							.z = cz + z + oz,
						};
					}

					struct block_info local_info = (struct block_info) {
						.block = &local,
						.neighbours = neighbours,
						.x = cx + x,
						.y = cy + y,
						.z = cz + z,
						.get_block = mesher_neighbour_get,
						.get_block_ctx = &nctx,
					};

					uint8_t vertex_light[24];
					bool light_loaded = count_only;

					for(int k = 0; k < SIDE_MAX; k++) {
						enum side s = (enum side)k;

						// Surface skin: terrain emits only its upward (SIDE_TOP)
						// face; logs/leaves keep their real outer hull so distant
						// trees do not collapse into bare stretched log skirts. The
						// per-side visibility test
						// below already culls every top face that has an opaque
						// block above it, so what survives is exactly the exposed
						// surface -- the heightmap skin -- at ~1 quad per column
						// instead of the full 6-sided mesh. Terrain sides/bottom
						// are dropped (the far ring is viewed from above + distance;
						// silhouette skirts EXIST -- lod_emit_skirts runs for
						// lod==1 tiers; this comment once called them a later
						// refinement). This only
						// changes which faces are DRAWN; it reads the same faithful
						// light/tint and touches no simulation state.
						if(!chunk_lod_face_allowed(lod_skin, local.type, s))
							continue;

						// T2 SPIKE: this column's top face is fully covered by its
						// patch anchor's merged quad (lod_geomip_compute_plan
						// above); only ever true when lod_skin && geomip_enabled --
						// geomip_suppress[] stays all-false otherwise (see that
						// function + test_lod_geomip_policy.c's exhaustive
						// disabled-gate proof), so this is inert on the default path.
						if(lod_skin && s == SIDE_TOP
						   && geomip_suppress[x * CHUNK_SIZE + z])
							continue;

						// Foliage LOD: at distance (band>=1) collapse leaves to a SOLID
						// SILHOUETTE -- drop the internal leaf-to-leaf faces so a dense
						// canopy renders as its outer hull (the tree's outline) at a
						// fraction of the vertices. Faces toward air/other blocks still
						// draw, so the shape is preserved. Render-only.
						if(foliage_lod >= 1 && local.type == BLOCK_LEAVES
						   && neighbours[k].type == BLOCK_LEAVES)
							continue;

						bool face_visible = true;

						// Hoist the neighbour block def (indexed 3x below) and
						// simplify the visibility gate: (!lb->t && !nb->t) || lb->t
						// is (De Morgan) lb->t || !nb->t.
						struct block* nb = blocks[neighbours[k].type];
						if(nb && (lb->transparent || !nb->transparent)) {
							struct face_occlusion* a = lb->getSideMask(
								&local_info, s, neighbours_info + k);
							struct face_occlusion* b = nb->getSideMask(
								neighbours_info + k, blocks_side_opposite(s),
								&local_info);

							face_visible = face_occlusion_test(a, b);
						}

						// DC-GRASS G2: Beta draws the grass top under a snow
						// LAYER -- Block.shouldSideBeRendered (client
						// Block.java:236) only culls against an opaque cube and
						// BlockSnow.isOpaqueCube() is false (BlockSnow.java:17)
						// -- while CaveWii's snow-layer bottom mask is full.
						// Grass under a snow layer only; a snow BLOCK above
						// still occludes it. Full-detail mesh only (the far-ring
						// LOD skin keeps its old culling; the snow top covers it).
						if(!face_visible && !lod_skin && s == SIDE_TOP
						   && local.type == BLOCK_GRASS
						   && neighbours[k].type == BLOCK_SNOW)
							face_visible = true;

						int dp_index = k;

						if(lb->transparent)
							dp_index += 6;

						if(lb->double_sided)
							dp_index = 12;

						if(face_visible || lb->renderBlockAlways) {
							if(!light_data) {
								// Per-build vertex light cache. Was ~17.5
								// KB malloc; now lives in the mesher
								// arena and is reset between builds. On
								// arena exhaustion fall back to malloc;
								// the helper frees only the fallback path
								// at the end of this function. On total
								// malloc failure the `!light_data` guard
								// below memsets a full-bright default light,
								// keeping the chunk drawable instead of
								// dereferencing NULL.
								light_data = mesher_scratch_alloc(
									arena,
									(CHUNK_SIZE + 2) * (CHUNK_SIZE + 2)
										* (CHUNK_SIZE + 2) * 3,
									4, &light_data_fallback);
								if(light_data) {
									SETTLE(mesher_settle_observe, SET_VL_BEGIN,
										   NULL, NULL, 0, 0, 0, 0);
									ptime_t vlt0 = time_get();
									chunk_mesher_vertex_light(bd, light_data);
									g_vlight_ms += time_diff_s(vlt0, time_get())
										* 1000.0F;
									SETTLE(mesher_settle_observe,
										   SET_EMIT_BEGIN, NULL, NULL, 0, 0, 0,
										   0);
								}
							}

							if(!light_loaded && !light_data) {
								// Total OOM: the arena was exhausted AND the
								// malloc fallback failed (the helper logged
								// "this chunk cannot be meshed"). Fill a
								// full-bright default light so this build
								// stays NULL-safe and its vertices[]/list
								// bookkeeping stays consistent, and report
								// the degradation: the caller marks the
								// result retry_required, so receive discards
								// it through the transactional OOM machinery
								// (old mesh retained one cycle, 8/16/32/64
								// backoff) instead of committing full-bright
								// lighting as a success.
								light_loaded = true;
								light_ok = false;
								memset(vertex_light, 0xFF, sizeof vertex_light);
							}

							if(!light_loaded) {
								light_loaded = true;
								vertex_light[0]
									= light_data[BLK_INDEX(x + 0, y + 0, z + 0)
													 * 3
												 + 0];
								vertex_light[1]
									= light_data[BLK_INDEX(x + 1, y + 0, z + 0)
													 * 3
												 + 0];
								vertex_light[2]
									= light_data[BLK_INDEX(x + 1, y + 0, z + 1)
													 * 3
												 + 0];
								vertex_light[3]
									= light_data[BLK_INDEX(x + 0, y + 0, z + 1)
													 * 3
												 + 0];
								vertex_light[4]
									= light_data[BLK_INDEX(x + 0, y + 2, z + 0)
													 * 3
												 + 0];
								vertex_light[5]
									= light_data[BLK_INDEX(x + 1, y + 2, z + 0)
													 * 3
												 + 0];
								vertex_light[6]
									= light_data[BLK_INDEX(x + 1, y + 2, z + 1)
													 * 3
												 + 0];
								vertex_light[7]
									= light_data[BLK_INDEX(x + 0, y + 2, z + 1)
													 * 3
												 + 0];

								vertex_light[8]
									= light_data[BLK_INDEX(x + 0, y + 0, z + 0)
													 * 3
												 + 1];
								vertex_light[9]
									= light_data[BLK_INDEX(x + 0, y + 1, z + 0)
													 * 3
												 + 1];
								vertex_light[10]
									= light_data[BLK_INDEX(x + 0, y + 1, z + 1)
													 * 3
												 + 1];
								vertex_light[11]
									= light_data[BLK_INDEX(x + 0, y + 0, z + 1)
													 * 3
												 + 1];
								vertex_light[12]
									= light_data[BLK_INDEX(x + 2, y + 0, z + 0)
													 * 3
												 + 1];
								vertex_light[13]
									= light_data[BLK_INDEX(x + 2, y + 1, z + 0)
													 * 3
												 + 1];
								vertex_light[14]
									= light_data[BLK_INDEX(x + 2, y + 1, z + 1)
													 * 3
												 + 1];
								vertex_light[15]
									= light_data[BLK_INDEX(x + 2, y + 0, z + 1)
													 * 3
												 + 1];

								vertex_light[16]
									= light_data[BLK_INDEX(x + 0, y + 0, z + 0)
													 * 3
												 + 2];
								vertex_light[17]
									= light_data[BLK_INDEX(x + 1, y + 0, z + 0)
													 * 3
												 + 2];
								vertex_light[18]
									= light_data[BLK_INDEX(x + 1, y + 1, z + 0)
													 * 3
												 + 2];
								vertex_light[19]
									= light_data[BLK_INDEX(x + 0, y + 1, z + 0)
													 * 3
												 + 2];
								vertex_light[20]
									= light_data[BLK_INDEX(x + 0, y + 0, z + 2)
													 * 3
												 + 2];
								vertex_light[21]
									= light_data[BLK_INDEX(x + 1, y + 0, z + 2)
													 * 3
												 + 2];
								vertex_light[22]
									= light_data[BLK_INDEX(x + 1, y + 1, z + 2)
													 * 3
												 + 2];
								vertex_light[23]
									= light_data[BLK_INDEX(x + 0, y + 1, z + 2)
													 * 3
												 + 2];
							}
						}

						// Beta biome tint (white = none). Grass tints only its
						// top face here (its side overlay is emitted separately
						// below), leaves all faces. Only TINTED faces touch the
						// display list's tint: set the colour, emit, reset to white
						// -- so a list stays white between tinted faces and the
						// ~99% untinted faces skip both set_tint calls entirely.
						// The emitted vertices carry the exact same tint either
						// way, so output is byte-identical.
						uint32_t tint = 0xFFFFFFu;
						switch(lb->biome_tint) {
							case BIOME_TINT_GRASS:
								if(s == SIDE_TOP)
									tint = local.type != BLOCK_GRASS ?
										grass_rgb[x * CHUNK_SIZE + z] :
										grass_known ?
										grass_rgb[x * CHUNK_SIZE + z] :
										BIOME_GRASS_FALLBACK;
								break;
							case BIOME_TINT_FOLIAGE:
								/* Leaves: block_leaves_face_tint, the
								 * BlockLeaves.colorMultiplier rule
								 * [BlockLeaves.java:19-29] shared with the
								 * piston-moving renderer. */
								if(local.type != BLOCK_LEAVES)
									tint = foliage_rgb[x * CHUNK_SIZE + z];
								else {
									bool raw_oak;
									tint = block_leaves_face_tint(
										local.metadata, foliage_known,
										foliage_rgb[x * CHUNK_SIZE + z],
										&raw_oak);
									if(raw_oak)
										local_info.texture_override
											= block_leaves_raw_oak_texture;
								}
								break;
							default: break;
						}
						bool tinted = tint != 0xFFFFFFu;
						// Untinted faces (~99%) emit into the 9-byte VTXFMT0 list
						// `dp_index`; the few biome-tinted faces emit into the
						// parallel 12-byte VTXFMT4 list dp_index+CHUNK_MESH_PARTS.
						// Same face -> identical pos/light/texcoord bytes either
						// way; only untinted faces stop carrying a (redundant
						// white) tint. chunk_render draws each list under the
						// matching gfx_tint() state, and white == identity tint,
						// so the rendered pixels are unchanged.
						int emit_index
							= tinted ? dp_index + CHUNK_MESH_PARTS : dp_index;
						if(tinted)
							displaylist_set_tint(d + emit_index,
												 (tint >> 16) & 0xFF,
												 (tint >> 8) & 0xFF, tint & 0xFF);

						// T2 SPIKE: this column anchors an accepted geomip merge --
						// emit ONE patch-sized quad instead of the native 1x1 quad.
						// Only reachable when lod_skin && geomip_enabled (see
						// lod_geomip_compute_plan); geomip_anchor_size[] stays all
						// zero otherwise, so this branch is dead on the default path
						// -- the original two-line call below is untouched in the
						// else. tex/tint/emit_index bookkeeping above already holds
						// for the whole patch (mergeability required identical
						// id+biome-tint-index across all four corners); light is
						// FLAT (this corner's own top-AO byte, vertex_light[4],
						// replicated to all 4 quad corners) rather than true
						// per-corner AO across the merged footprint -- the design
						// doc's disclosed spike-scope simplification, matching the
						// silhouette skirt pass's own flat-light convention above.
						// KNOWN CAVEAT (see design doc): the merge decision runs
						// before this per-block face_visible test, so it assumes
						// face_visible agrees with the surface scan's
						// can_see_through predicate for every block type eligible
						// for the terrain skin -- true for every block currently
						// reachable here (stone/dirt/grass/sand/gravel/sandstone/
						// clay all share the plain occluding face mask); a future
						// terrain-surface block with a custom getSideMask would
						// need re-auditing against this assumption.
						bool geomip_anchor = lod_skin && s == SIDE_TOP
							&& geomip_anchor_size[x * CHUNK_SIZE + z] > 0;
						if(geomip_anchor) {
							if(face_visible) {
								uint8_t flat_light[24];
								memset(flat_light, vertex_light[4],
									  sizeof flat_light);
								uint8_t patch_tex
									= lb->getTextureIndex(&local_info, SIDE_TOP);
								uint16_t patch_sz = (uint16_t)(
									geomip_anchor_size[x * CHUNK_SIZE + z]
									* LOD_BLK_LEN);
								vertices[emit_index]
									+= render_block_lod_patch_top(
										   d + emit_index, (int16_t)(x * LOD_BLK_LEN),
										   (int16_t)((y + 1) * LOD_BLK_LEN),
										   (int16_t)(z * LOD_BLK_LEN), patch_sz,
										   patch_tex, flat_light, lb->luminance)
									* 4;
							}
						} else if(face_visible)
							vertices[emit_index]
								+= lb->renderBlock(d + emit_index, &local_info, s,
												   neighbours_info + k,
												   vertex_light, count_only)
								* 4;

						if(lb->renderBlockAlways)
							vertices[emit_index]
								+= lb->renderBlockAlways(
									   d + emit_index, &local_info, s,
									   neighbours_info + k, vertex_light,
									   count_only)
								* 4;

						if(tinted) // reset to white for the next tinted face here
							displaylist_set_tint(d + emit_index, 255, 255, 255);
						local_info.texture_override = NULL;

						// DC-GRASS G2: Beta fancy-grass side overlay
						// (RenderBlocks.java:2276-2281 flat, 1954-1970 AO;
						// fancyGrass defaults true): a grass side whose texture
						// is the plain side tile (Beta tile 3 -- not snowy tile
						// 68, BlockGrass.java:12-21) is drawn untinted above,
						// then drawn AGAIN with tile 38 multiplied by the biome
						// colour: same face, same UVs/light/culling, equal
						// depth, no offset. The base went to the untinted list
						// dp_index (grass sides carry no tint); the overlay goes
						// to the TINTED double-sided list PARTS + 12, which no
						// other block uses (nothing else is both double_sided
						// and tinted), so chunk.c can give exactly this draw
						// Beta's alpha cut without touching leaves; it is drawn
						// last in the same opaque pass (base before overlay).
						// Always that list, even for a white colour, so routing
						// never depends on the value. Count-only and emission
						// add the same 1 quad.
						// Crack/override rendering never comes through here
						// (render_block_cracks uses renderBlock only), so no
						// overlay there -- Beta's overrideBlockTexture < 0 test.
						if(face_visible && local.type == BLOCK_GRASS
						   && s != SIDE_TOP && s != SIDE_BOTTOM
						   && lb->getTextureIndex(&local_info, s)
							   == tex_atlas_lookup(TEXAT_GRASS_SIDE)) {
							uint32_t gc = grass_known ?
								grass_rgb[x * CHUNK_SIZE + z] :
								BIOME_GRASS_FALLBACK;
							int ov_index = CHUNK_MESH_PARTS + 12;
							displaylist_set_tint(d + ov_index, (gc >> 16) & 0xFF,
												 (gc >> 8) & 0xFF, gc & 0xFF);
							vertices[ov_index]
								+= render_block_full_tex(
									   d + ov_index, &local_info, s, vertex_light,
									   tex_atlas_lookup(TEXAT_GRASS_SIDE_OVERLAY),
									   count_only)
								* 4;
							displaylist_set_tint(d + ov_index, 255, 255, 255);
						}
					}
				}
			}
		}

		// H-1: cooperative yield point, once per y-layer (16 checkpoints across a
		// full chunk). Every list touched this layer is only PARTIALLY emitted
		// (finalize hasn't run yet), but that's fine -- this thread is the only
		// writer, so pausing here and resuming later changes nothing about the
		// bytes eventually written, only when they land relative to the wall
		// clock. See mesher_emit_slice_tick's declaration comment for why this
		// replaced the old post-finalize-loop sleep.
		mesher_emit_slice_tick(&slice_mark);
	}

	// M1 far-ring LOD: close the silhouette gaps the top-only skin leaves above by
	// emitting per-column skirts. lod=0 never enters here -> full mesh byte-identical.
	if(lod_skin)
		lod_emit_skirts(bd, cx, cy, cz, d, vertices);

	// `light_data` was either from the arena (resets next job) or
	// from a malloc fallback; the helper checks the flag and frees
	// only the fallback case.
	mesher_scratch_free(light_data, light_data_fallback);
	return light_ok;
}

static void chunk_mesher_build(struct chunk_mesher_rpc* req) {
	ptime_t build_t0 = time_get();
	// One arena reset per job. All scratch allocations below (visited
	// in chunk_test_init, light_data in chunk_mesher_rebuild) come
	// from this arena and are valid only until the next build starts.
	arena_reset(&mesher_arena);

	req->result.retry_required = false;
	req->result.mesh_block = NULL;
	req->result.mesh_block_bytes = 0;
	displaylist_scratch_begin();
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		req->result.has_displist[k] = false;
		// Lists [0..PARTS-1] are untinted: pos(6) + light(1) + texcoord(2) = 9
		// bytes/vertex (GX_VTXFMT0). Lists [PARTS..2*PARTS-1] add the 3-byte
		// per-vertex biome tint = 12 bytes/vertex (GX_VTXFMT4). The mesher routes
		// each face to the matching list (see the emit loop), so untinted
		// geometry -- nearly everything -- pays only 9 bytes.
		bool tinted = k >= CHUNK_MESH_PARTS;
		// T1-c: pre-size from the list this build is about to replace (0 = no
		// history -> identical to plain displaylist_init, today's initial size).
		displaylist_init_presized(
			req->result.mesh + k, 64,
			tinted ? (3 * 2 + 3 + 2 * 1 + 1) : (3 * 2 + 2 * 1 + 1), tinted,
			req->request.prev_index[k]);
		displaylist_set_tinted(req->result.mesh + k, tinted);
		displaylist_allow_scratch(req->result.mesh + k);
	}

	size_t vertices[CHUNK_MESH_LISTS];
	SETTLE(mesher_settle_observe, SET_EMIT_BEGIN, NULL, req, 0, 0, 0, 0);
	ptime_t t_rb = time_get();
	// false = a scratch total-OOM degraded the emitted vertex light to the
	// full-bright fallback. Route it through the same transactional retry as
	// a truncation so the wrong lighting never commits as a success.
	if(!chunk_mesher_rebuild(&mesher_arena, req->request.blocks, req->chunk->x,
							 req->chunk->y, req->chunk->z, req->result.mesh,
							 false, vertices, req->request.grass_i,
							 req->request.grass_j, req->request.has_tint,
							 req->request.lod != 0, req->request.foliage_lod,
							 req->request.geomip_enabled,
							 req->request.geomip_dist_blocks))
		req->result.retry_required = true;
	float rb_ms = time_diff_s(t_rb, time_get()) * 1000.0F;

	SETTLE(mesher_settle_observe, SET_FIN_BEGIN, NULL, req, 0, 0, 0, 0);
	ptime_t t_fin = time_get();
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		// DROP a TRUNCATED list: an OOM during vertex emit (mem2 alloc/realloc NULL under
		// MEM2-carve fragmentation) made an emit call early-return without advancing the
		// write index, yet vertices[k] still counts the FULL intended vertices. Finalizing
		// would stamp a GX_QUADS header claiming more quads than were written -> the GP
		// parses the GX_NOP tail as POS/CLR/TEX (garbled geometry) and a desynced stream
		// can WEDGE the GP command processor (GX_WaitDrawDone never returns) = the real-Wii
		// frozen-no-dump hang. The incomplete replacement is never installed; receive
		// applies bounded retry/backoff. Counted for the F3 'mesh-trunc' diagnostic.
		if(req->result.mesh[k].truncated) {
			__atomic_fetch_add(&g_mesh_truncations, 1, __ATOMIC_RELAXED);
			req->result.retry_required = true;
		}
		// <= 0xFFFC, NOT 0xFFFF*4: displaylist_finalize stamps vtxcnt into a
		// uint16_t header, so the old bound let a 65536..262140-vertex list
		// PASS and silently wrap the header mod 65536 -- the GP then parses
		// the unclaimed vertex bytes as opcodes (the cmdIdle=0 wedge class).
		// 0xFFFC keeps the count a GX_QUADS multiple of 4. Unreachable for
		// 16^3 chunks today; the guard is for the emitter we haven't written.
		if(vertices[k] > 0 && vertices[k] <= 0xFFFC
		   && !req->result.mesh[k].truncated) {
			displaylist_finalize(req->result.mesh + k, vertices[k]);
			req->result.has_displist[k] = true;
		} else {
			// Normal reasons to land here: vertices[k] == 0 (nothing emitted)
			// and truncation (already counted + retried above). The third,
			// vertices[k] > 0xFFFC, is UNREACHABLE today -- the worst per-list
			// emitter is the double-sided bucket at CHUNK_SIZE^3 = 4096 blocks
			// x 2 quads x 4 vertices = 32768 < 0xFFFC (2026-08-13 audit) --
			// but if a future emitter raises the per-block quad count past the
			// u16 finalize header, silently dropping the list would leave a
			// PERMANENT hole in the world (the build still committed as a
			// success). Schedule the OOM-style retry and say so loudly; a
			// deterministic overflow then cycles at the 64-check backoff
			// ceiling instead of converging, and the log is what makes that
			// state debuggable -- still strictly better than the silent hole.
			if(vertices[k] > 0xFFFC) {
				fprintf(stderr,
						"[chunk_mesher] list %d emitted %zu vertices, over the "
						"0xFFFC u16 finalize cap; list dropped, retry "
						"scheduled. An emitter broke the 2-quad/block "
						"ceiling.\n",
						k, vertices[k]);
				req->result.retry_required = true;
			}
			displaylist_destroy(req->result.mesh + k);
		}
		/* M2: pack this generation's finished lists into one exact-size
		 * carve block. A pack failure is a carve OOM: drop the lists (their
		 * bytes are scratch, reclaimed by the next build) and retry like any
		 * other OOM -- never send scratch pointers to the main thread. */
		if(k == CHUNK_MESH_LISTS - 1 && displaylist_block_mode()) {
			/* A build already marked for retry is discarded on receive: do
			 * not spend a carve allocation packing it. */
			if(req->result.retry_required
			   || !displaylist_pack_block(req->result.mesh, req->result.has_displist,
					CHUNK_MESH_LISTS, &req->result.mesh_block,
					&req->result.mesh_block_bytes)) {
				for(int j = 0; j < CHUNK_MESH_LISTS; j++) {
					if(req->result.has_displist[j])
						displaylist_destroy(req->result.mesh + j);
					req->result.has_displist[j] = false;
				}
				req->result.retry_required = true;
			}
			displaylist_scratch_end();
		}
		// H-1 HOTFIX (2026-09-03): the cooperative slice used to live HERE --
		// after list k was already finalized (its bytes committed, never to
		// change again) -- checking elapsed-since-BUILD-START and sleeping
		// once that ever crossed the threshold. Because that elapsed value
		// only ever grows, the FIRST list to cross 2ms made every remaining
		// list in this same loop sleep too (up to CHUNK_MESH_LISTS-1 = 25
		// more 1ms sleeps), so a build that took just over 2ms to emit could
		// burn up to ~25ms of dead wall-clock AFTER its bytes were already
		// done -- entirely wasted, since giving up the CPU post-commit cannot
		// speed up anything still waiting on this chunk. The slice now runs
		// DURING emit instead (mesher_emit_slice_tick, called once per
		// y-layer in chunk_mesher_rebuild's loop, with the mark reset on
		// every sleep so it fires at most once per >= CHUNK_MESHER_SLICE_MS
		// of actual work) -- see that function's declaration comment.
	}
	float fin_ms = time_diff_s(t_fin, time_get()) * 1000.0F;

	// Per-tier vertex tally for the LOD instrument.
	size_t total_verts = 0;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		total_verts += vertices[k];
	int tier = req->request.lod ? 1 : 0;
	// CR-08: sole writer -- chunk_mesher_build only ever runs on the mesher
	// worker thread. Publish through the split lo/hi 32-bit atomics the
	// reader (chunk_mesher_lod_stats) reconstructs; see that declaration's
	// comment for why a whole-value uint64_t atomic isn't available on
	// PPC32. total_verts safely fits uint32_t for a single build (the
	// largest possible per-list emitter is far below the 0xFFFC finalize
	// cap, summed over CHUNK_MESH_LISTS lists), so only the CUMULATIVE
	// total can ever need the high word.
	uint32_t old_lo = __atomic_load_n(&g_lod_vertices_lo[tier], __ATOMIC_RELAXED);
	uint32_t new_lo = old_lo + (uint32_t)total_verts;
	__atomic_store_n(&g_lod_vertices_lo[tier], new_lo, __ATOMIC_RELAXED);
	if(new_lo < old_lo) // carry: the low word wrapped past 2^32
		__atomic_fetch_add(&g_lod_vertices_hi[tier], 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&g_lod_chunks[tier], 1, __ATOMIC_RELAXED);

	SETTLE(mesher_settle_observe, SET_BFS_BEGIN, NULL, req, 0, 0, 0, 0);
	ptime_t t_bfs = time_get();
	// Same treatment for the reachability scratch (audit residual #3): the
	// conservative fully-connected fill only over-renders, but it should
	// still self-heal through a retry instead of persisting until an
	// unrelated edit re-dirties the chunk.
	if(!chunk_test_init(&mesher_arena, req->request.blocks,
						req->result.reachable))
		req->result.retry_required = true;
	float bfs_ms = time_diff_s(t_bfs, time_get()) * 1000.0F;

	// req->request.blocks is now part of the rpc_msg pool — its lifetime
	// is the slot's lifetime. Don't free it; the slot returns to
	// mesher_empty_msg and gets reused.

	float build_ms = time_diff_s(build_t0, time_get()) * 1000.0F;
	if(mesher_settle_observe) {
		struct settle_phases sp
			= {build_ms, g_vlight_ms, rb_ms - g_vlight_ms, fin_ms, bfs_ms};
		SETTLE_DETAIL(mesher_settle_observe, SET_PHASES, req, &sp);
	}
	// H-1: worker throughput counter (see g_chunks_meshed's declaration).
	__atomic_fetch_add(&g_chunks_meshed, 1, __ATOMIC_RELAXED);
	// CR-08: sole writer (mesher worker thread); RELAXED store, read via
	// chunk_mesher_build_ms/chunk_mesher_build_breakdown's matching
	// __atomic_load_n on the main thread.
	mesher_atomic_store_f(&mesher_build_ms_last, build_ms);
	if(build_ms > mesher_atomic_load_f(&mesher_build_ms_max)) {
		mesher_atomic_store_f(&mesher_build_ms_max, build_ms);
		// Snapshot the sub-phase split of this new worst build. emit =
		// rebuild total minus the vertex_light precompute it contains.
		mesher_atomic_store_f(&mesher_max_vl, g_vlight_ms);
		mesher_atomic_store_f(&mesher_max_em, rb_ms - g_vlight_ms);
		mesher_atomic_store_f(&mesher_max_bfs, bfs_ms);
		mesher_atomic_store_f(&mesher_max_fin, fin_ms);
	}
}

// The ONLY writer of the watchdog worker phase. Without these three stamps the
// hang report's worker tag (and its cell-bar row) reads WDW_IDLE forever, which
// is not "the mesher was idle" but "no call site exists" -- the 2026-07-27 real-Wii
// list-walk hang was diagnosed against a worker=0 that carried no information.
static void* chunk_mesher_local_thread(void* user) {
	g9_probe_role(3);
	if(mesher_tps_boundary) mesher_tps_boundary(true);
	while(1) {
		struct chunk_mesher_rpc* request;
		watchdog_phase_worker(WDW_RECV);
		// A blocking receive on an initialized, token-conserving channel
		// cannot fail; if it does, the pool invariant is broken. Abort
		// instead of building from an uninitialized pointer.
		if(!tchannel_receive(&mesher_requests, (void**)&request, true)) {
			fprintf(stderr, "[chunk_mesher] mesher_requests receive failed: "
							"channel invariant broken\n");
			abort();
		}
		if(request != (void*)&s_mesher_stop)
			SETTLE(mesher_settle_observe, SET_JOB_DEQUEUE, NULL, request, 0, 0,
				   0, 0);
#ifdef CAVEWII_DISPLAYLIST_TEST
		while(__atomic_load_n(&s_test_hold_worker, __ATOMIC_ACQUIRE)
			  && !__atomic_load_n(&s_mesher_stop, __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&s_test_worker_held, true, __ATOMIC_RELEASE);
			thread_msleep(1);
		}
		__atomic_store_n(&s_test_worker_held, false, __ATOMIC_RELEASE);
#endif
		/* The dequeued job remains owned by rpc_msg until main retires it
		 * after join. The wake marker is never dereferenced. */
		if(__atomic_load_n(&s_mesher_stop, __ATOMIC_ACQUIRE))
			break;
		SETTLE(mesher_settle_observe, SET_WORKER, NULL, request, 0, 0, 0, 0);
		watchdog_phase_worker(WDW_BUILD);
		g9_probe_mesh((unsigned)(request - rpc_msg), 2, false);
		chunk_mesher_build(request);
		// Never full: results only ever holds rpc_msg pool tokens (QLENGTH total
		// == its capacity) and this thread holds one of them right now.
		watchdog_phase_worker(WDW_SEND);
		SETTLE(mesher_settle_observe, SET_COMPLETE, NULL, request, 0, 0, 0, 0);
		{ /* R4 point 7: this result's finalized lists are now "completed" */
			uint32_t b, n;
			mesher_result_dl_sum(request, &b, &n);
			__atomic_fetch_add(&s_completed_bytes, b, __ATOMIC_RELAXED);
			__atomic_fetch_add(&s_completed_lists, n, __ATOMIC_RELAXED);
		}
		g9_probe_mesh((unsigned)(request - rpc_msg), 3, false);
		tchannel_send(&mesher_results, request, true);
	}

	return NULL;
}

bool chunk_mesher_init() {
	if(mesher_thread.created) return true;
	if(!tchannel_init(&mesher_requests, CHUNK_MESHER_QLENGTH))
		goto fail;
	if(!tchannel_init(&mesher_results, CHUNK_MESHER_QLENGTH))
		goto fail;
	if(!tchannel_init(&mesher_empty_msg, CHUNK_MESHER_QLENGTH))
		goto fail;

	// Seeds exactly QLENGTH tokens into a fresh QLENGTH-capacity channel, on the
	// main thread before the worker exists -- the k-th send sees count k < cap.
	for(int k = 0; k < CHUNK_MESHER_QLENGTH; k++)
		if(!tchannel_send(&mesher_empty_msg, rpc_msg + k, true))
			goto fail;

	// Wrap the static, 32-aligned BSS buffer in the arena. On the Wii
	// the BSS lives in MEM2 by default (libogc's link map), which is
	// what we want — MEM1 is reserved for things the GX/PPU touch
	// every frame (FIFO, framebuffers).
	arena_init(&mesher_arena, mesher_arena_buffer, MESHER_ARENA_SIZE,
			   "mesher");

	// Defensive: a hypothetical re-init after a prior chunk_mesher_shutdown()
	// must not start the new worker pre-armed to exit immediately.
	__atomic_store_n(&s_mesher_stop, false, __ATOMIC_RELAXED);

	// Priority 32 puts the mesher comfortably below LWP_PRIO_NORMAL (64,
	// where the main render thread runs) and below the server thread
	// (64), but well above LWP_PRIO_IDLE (0). The previous value of 4
	// was near-idle, leaving the mesher vulnerable to starvation when
	// the main thread was running. See source/platform/thread.c for
	// the libogc priority convention.
	if(thread_create(&mesher_thread, chunk_mesher_local_thread, NULL, 32))
		return true;
fail:
	tchannel_close(&mesher_requests);
	tchannel_close(&mesher_results);
	tchannel_close(&mesher_empty_msg);
	return false;
}

/* Main-thread teardown, after the GPU safe point. Every acquired job keeps
 * rpc_msg[i].chunk non-NULL until receive or this retirement pass drops its
 * shell reference. This covers requests, results AND the worker's dequeued job. */
void chunk_mesher_shutdown(void) {
	if(!mesher_thread.created)
		return;
	__atomic_store_n(&s_mesher_stop, true, __ATOMIC_RELEASE);
	/* A separate non-NULL marker wakes an idle worker. If the nonblocking
	 * send finds the request queue full, an existing job already guarantees
	 * a wake. Never steal a pool token or wait on a stopped consumer. Only
	 * real jobs enter results, whose capacity remains exactly the pool size. */
	(void)tchannel_send(&mesher_requests, &s_mesher_stop, false);
	thread_join(&mesher_thread);
	for(unsigned i = 0; i < CHUNK_MESHER_QLENGTH; i++) {
		struct chunk_mesher_rpc* job = &rpc_msg[i];
		g9_probe_mesh(i, 0, false);
		if(!job->chunk) continue;
		SETTLE(mesher_settle_observe, SET_CANCEL, NULL, job, 0, 0, 0, 0);
		for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++)
			if(job->result.has_displist[k])
				displaylist_destroy(&job->result.mesh[k]);
		displaylist_block_free(job->result.mesh_block, job->result.mesh_block_bytes);
		job->result.mesh_block = NULL;
		if(job->lease_owner)
			(void)job->lease_owner->release(job->lease_owner->user,
											WORLD_LEASE_MESHER, &job->lease);
		job->lease_owner = NULL;
		job->chunk->mesh_in_flight = false;
		job->chunk->rebuild_displist = true;
		chunk_unref(job->chunk);
		job->chunk = NULL;
	}
	__atomic_store_n(&mesher_pending, 0, __ATOMIC_RELAXED);
	/* Sol review R4: every job was retired above, so nothing is "completed
	 * but unreceived" any more; a re-init starts from zero. */
	__atomic_store_n(&s_completed_bytes, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s_completed_lists, 0, __ATOMIC_RELAXED);
	tchannel_close(&mesher_requests);
	tchannel_close(&mesher_results);
	tchannel_close(&mesher_empty_msg);
}

static uint8_t chunk_mesher_oom_backoff(uint8_t streak) {
	// The first failure retains the complete old mesh for one retry cycle. The
	// second releases it in receive; increasingly sparse later retries let other
	// chunks make progress and give the allocator time to recover.
	if(streak <= 1)
		return 8;
	if(streak == 2)
		return 16;
	if(streak == 3)
		return 32;
	return 64;
}

void (*mesher_column_observe_complete)(const struct chunk*, uint32_t,
	const struct displaylist*, const bool*, const uint8_t*, bool);

static bool chunk_mesher_receive_bounded(uint32_t budget_us, unsigned max_results) {
	struct chunk_mesher_rpc* result;
	ptime_t start = budget_us ? time_get() : (ptime_t){0};
	unsigned work = 0;
	while(!budget_us || (work < max_results && time_diff_us(start, time_get()) < budget_us)) {
		if(!tchannel_receive(&mesher_results, (void**)&result, false)) return true;
		work++;
		SETTLE(mesher_settle_observe, SET_CONSUME, NULL, result, 0, 0, 0, 0);
		{ /* R4 point 7: no longer "completed" -- installed or destroyed below */
			uint32_t b, n;
			mesher_result_dl_sum(result, &b, &n);
			__atomic_fetch_sub(&s_completed_bytes, b, __ATOMIC_RELAXED);
			__atomic_fetch_sub(&s_completed_lists, n, __ATOMIC_RELAXED);
		}
		if(mesher_column_observe_complete)
			mesher_column_observe_complete(result->chunk, result->dirty_generation,
				result->result.mesh, result->result.has_displist,
				result->result.reachable, result->result.retry_required);
		/* Split-dims B1: give the job's lease back to the exact context it
		 * was taken from. A job started under an epoch that has since been
		 * reset or replaced may not publish (its chunk belongs to torn-down
		 * payloads), so it is treated exactly like a stale generation. */
		bool epoch_current = true;
		if(result->lease_owner)
			epoch_current = result->lease_owner->release(
				result->lease_owner->user, WORLD_LEASE_MESHER, &result->lease);
		result->lease_owner = NULL;
		bool current = epoch_current
			&& chunk_dirty_generation_matches(result->chunk,
											  result->dirty_generation);
		if(current && !result->result.retry_required) {
			for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
				if(result->chunk->has_displist[k])
					displaylist_destroy(result->chunk->mesh + k);

				result->chunk->mesh[k] = result->result.mesh[k];
				result->chunk->has_displist[k] = result->result.has_displist[k];
			}
			/* M2: the old generation's block goes with its lists; the new
			 * block is now owned by the chunk. */
			chunk_mesh_block_release(result->chunk);
			result->chunk->mesh_block = result->result.mesh_block;
			result->chunk->mesh_block_bytes = result->result.mesh_block_bytes;
			result->result.mesh_block = NULL;
			result->result.mesh_block_bytes = 0;

			for(int k = 0; k < 6; k++)
				result->chunk->reachable[k] = result->result.reachable[k];
			result->chunk->mesh_installed_ms = chunk_mesher_clock_ms();

			// Only a complete current-generation replacement proves allocator
			// recovery. Stale successes leave the existing OOM state untouched.
			result->chunk->mesh_oom_streak = 0;
			result->chunk->mesh_retry_skip_checks = 0;
		} else {
			// A stale result is obsolete; an allocation-truncated result is an
			// incomplete replacement. In both cases release every successful new
			// list. Generation and allocation status are independent: a result can
			// be BOTH stale and OOM, and must then count as stale while still
			// advancing the allocator backoff/recovery state.
			for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
				if(result->result.has_displist[k])
					displaylist_destroy(result->result.mesh + k);
			}
			displaylist_block_free(result->result.mesh_block,
								result->result.mesh_block_bytes);
			result->result.mesh_block = NULL;
			result->result.mesh_block_bytes = 0;

			if(!current) {
				// CR-08: sole writer (this function only runs on the main
				// thread); RELAXED, read cross-thread via __atomic_load_n at
				// each reader's call site (main telemetry + the Wii watchdog
				// thread's hang-report dump).
				__atomic_fetch_add(&mesh_stale_discards, 1, __ATOMIC_RELAXED);
				g9_probe_mesh((unsigned)(result - rpc_msg), 3, true);
			}

			if(result->result.retry_required) {
				if(result->chunk->mesh_oom_streak < UINT8_MAX)
					result->chunk->mesh_oom_streak++;
				result->chunk->mesh_retry_skip_checks = chunk_mesher_oom_backoff(
					result->chunk->mesh_oom_streak);

				// Keep one known-good mesh through the first failed replacement. If
				// that retry also OOMs, its retained lists are part of the pressure:
				// release them HERE, while receive runs in the post-flip safe phase.
				// Never free render-owned lists from chunk_check_built/enqueue.
				if(result->chunk->mesh_oom_streak >= 2) {
					for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
						if(result->chunk->has_displist[k]) {
							displaylist_destroy(result->chunk->mesh + k);
							result->chunk->has_displist[k] = false;
						}
					}
					chunk_mesh_block_release(result->chunk);
					// With no mesh, stale restrictive connectivity must not hide
					// otherwise visible chunks behind this one during the cooldown.
					memset(result->chunk->reachable, 0x3F,
						   sizeof(result->chunk->reachable));
				}

				// A stale OOM still has a newer dirty generation waiting. Explicitly
				// re-arm it so allocation failure can never consume that newer work.
				result->chunk->rebuild_displist = true;
				SETTLE(mesher_settle_observe, SET_DIRTY_RETRY, result->chunk,
					   NULL, 0, 0, 0, 0);
			}
		}

		// This chunk's build is done (integrated or discarded); allow the next send.
		// If it was re-dirtied while in flight, rebuild_displist is still set and
		// chunk_check_built re-sends next frame at the latest generation.
		result->chunk->mesh_in_flight = false;
		SETTLE_DETAIL(
			mesher_settle_observe,
			current ? (result->result.retry_required ? SET_RETRY : SET_ACCEPT) :
					  SET_STALE,
			result, result->chunk);

		chunk_unref(result->chunk);
		result->chunk = NULL;

		// Never full: empty_msg only ever holds rpc_msg pool tokens (QLENGTH
		// total == its capacity) and this thread holds one of them right now.
		tchannel_send(&mesher_empty_msg, result, true);
		// CR-08: writer is still main-thread-only (this function), so this
		// remains a plain decrement -- only the getter's cross-thread read
		// (chunk_mesher_pending, also polled by the Wii watchdog thread)
		// needed the atomic treatment.
		int pending = __atomic_load_n(&mesher_pending, __ATOMIC_RELAXED);
		g9_probe_mesh((unsigned)(result - rpc_msg), 0, false);
		if(pending > 0)
			__atomic_store_n(&mesher_pending, pending - 1, __ATOMIC_RELAXED);
	}
	return false;
}
void chunk_mesher_receive(void) {
	g9_capture_main(13, true, 0);
	(void)chunk_mesher_receive_bounded(0, CHUNK_MESHER_QLENGTH);
	g9_capture_main(13, false, 1);
}
bool chunk_mesher_receive_budget(uint32_t budget_us) {
	g9_capture_main(13, true, 0);
	bool complete = budget_us && chunk_mesher_receive_bounded(budget_us, CHUNK_MESHER_QLENGTH);
	g9_capture_main(13, false, complete);
	return complete;
}
bool chunk_mesher_receive_step(uint32_t budget_us) {
	g9_capture_main(13, true, 0);
	bool complete = budget_us && chunk_mesher_receive_bounded(budget_us, 1);
	g9_capture_main(13, false, complete);
	return complete;
}

#ifdef CAVEWII_DISPLAYLIST_TEST
// H-1 test-only seam (test_chunk_mesher_enqueue_cap_env.c): CHUNK_MESHER_
// ENQUEUE_CAP is production CHUNK_MESHER_QLENGTH (a no-op cap -- see its
// declaration's H-1 comment for why), so nothing in a default build ever
// exercises real deferral. -1 (default) means "use the compile constant
// unchanged"; a real value forces chunk_mesher_send's cap check to that
// value instead, letting a test prove the DEFERRAL MECHANISM itself (accept
// exactly N, refuse the rest, byte-identical once drained) is still sound,
// independent of what value production currently ships.
static int s_mesher_enqueue_cap_override = -1;

void chunk_mesher_test_set_enqueue_cap(int cap) {
	s_mesher_enqueue_cap_override = cap;
}
#endif

static inline int mesher_enqueue_cap(void) {
#ifdef CAVEWII_DISPLAYLIST_TEST
	if(s_mesher_enqueue_cap_override >= 0)
		return s_mesher_enqueue_cap_override;
#endif
	return CHUNK_MESHER_ENQUEUE_CAP;
}

/* Real padded snapshot readers, also measured by bench_chunk_gather. The
 * owned path dispatches once for the interior; every halo cell still resolves
 * its actual neighbour provider. No allocation, callbacks or mesh work here. */
void chunk_mesher_gather_generic(struct chunk* c, struct block_data* bd) {
	for(w_coord_t y = -1; y < CHUNK_SIZE + 1; y++)
		for(w_coord_t z = -1; z < CHUNK_SIZE + 1; z++)
			for(w_coord_t x = -1; x < CHUNK_SIZE + 1; x++)
				BLK_DATA(bd, x, y, z) = chunk_lookup_block(c, x, y, z);
}

/* T1: a server write cannot alter any presentation value, so each rectangular
 * piece of this gather may hold its own slice's stripe once. Never nest locks
 * across columns (even neighbouring columns can share one stripe). The main
 * thread exclusively owns client edits and dictionary lookup during gather. */
static void chunk_mesher_gather_shared(struct chunk* c, struct block_data* bd) {
	const int lo[] = {-1, 0, CHUNK_SIZE}, hi[] = {0, CHUNK_SIZE, CHUNK_SIZE + 1};
	for(unsigned gy = 0; gy < 3; gy++)
		for(unsigned gz = 0; gz < 3; gz++)
			for(unsigned gx = 0; gx < 3; gx++) {
				struct chunk* other = world_find_chunk(c->world,
					c->x + lo[gx], c->y + lo[gy], c->z + lo[gz]);
				bool shared = other && other->ownership == LOCAL_SHARED_VIEW;
				if(shared) {
					SETTLE(mesher_settle_observe, SET_STRIPE_BEGIN, NULL, NULL,
						   0, 0, 0, 0);
					thread_stripe_lock(other->backing->mutex);
					SETTLE(mesher_settle_observe, SET_STRIPE_END, NULL, NULL, 0,
						   0, 0, 0);
				}
				for(int y = lo[gy]; y < hi[gy]; y++)
					for(int z = lo[gz]; z < hi[gz]; z++)
						for(int x = lo[gx]; x < hi[gx]; x++) {
							if(shared)
								BLK_DATA(bd, x, y, z) = column_view_get_locked(other->view,
									(W2C_COORD(x) * 16 + W2C_COORD(z)) * 128 + c->y + y);
							else if(other)
								BLK_DATA(bd, x, y, z) = chunk_get_block(other,
									W2C_COORD(x), W2C_COORD(y), W2C_COORD(z));
							else
								BLK_DATA(bd, x, y, z) = chunk_lookup_block(c, x, y, z);
						}
				if(shared) thread_stripe_unlock(other->backing->mutex);
			}
}

void chunk_mesher_gather(struct chunk* c, struct block_data* bd) {
	if(c->ownership == OWNED_VOXEL) {
		for(w_coord_t y = 0; y < CHUNK_SIZE; y++)
			for(w_coord_t z = 0; z < CHUNK_SIZE; z++)
				for(w_coord_t x = 0; x < CHUNK_SIZE; x++)
					BLK_DATA(bd, x, y, z) = chunk_get_block_voxel(
						c, (c_coord_t)x, (c_coord_t)y, (c_coord_t)z);

		for(w_coord_t y = -1; y < CHUNK_SIZE + 1; y++)
			for(w_coord_t z = -1; z < CHUNK_SIZE + 1; z++)
				for(w_coord_t x = -1; x < CHUNK_SIZE + 1; x++)
					if(x < 0 || y < 0 || z < 0 || x >= CHUNK_SIZE
					   || y >= CHUNK_SIZE || z >= CHUNK_SIZE)
						BLK_DATA(bd, x, y, z) = chunk_lookup_block(c, x, y, z);
	} else if(c->ownership == LOCAL_SHARED_VIEW) {
		chunk_mesher_gather_shared(c, bd);
	} else {
		chunk_mesher_gather_generic(c, bd);
	}
}

bool chunk_mesher_send(struct chunk* c) {
	assert(c);

	// T1-d (2026-09-03 far-ring frame-drop root-cause, mechanism A): pure
	// PACING cap, checked BEFORE the empty-token acquire so a capped call
	// leaves a token in the pool for whichever chunk sends successfully this
	// frame. Refusing here is lossless: the caller (chunk.c's
	// chunk_check_built) leaves rebuild_displist set on a false return and
	// retries next frame -- see CHUNK_MESHER_ENQUEUE_CAP's declaration for the
	// telemetry this cap is sized from. H-1: production value is now
	// CHUNK_MESHER_QLENGTH (a no-op), so this only ever refuses under the
	// test-only override above.
	if(chunk_mesher_pending() >= mesher_enqueue_cap()) {
		SETTLE(mesher_settle_observe, SET_GATE_TOKEN, c, NULL, 0, 0, 0, 0);
		return false;
	}

	/* Split-dims B1: one mesher lease against the world's current context.
	 * Refused while the context is parked after a scoped reset: the same
	 * lossless deferral as the pacing cap above (rebuild_displist stays set,
	 * the chunk retries next frame). */
	const struct world_lease_owner* lease_owner
		= c->world ? c->world->lease_owner : NULL;
	struct world_lease lease = {0};
	if(lease_owner
	   && !lease_owner->acquire(lease_owner->user, WORLD_LEASE_MESHER, &lease)) {
		SETTLE(mesher_settle_observe, SET_GATE_TOKEN, c, NULL, 0, 0, 0, 0);
		return false;
	}

	struct chunk_mesher_rpc* request;
	if(!tchannel_receive(&mesher_empty_msg, (void**)&request, false)) {
		if(lease_owner)
			(void)lease_owner->release(lease_owner->user, WORLD_LEASE_MESHER,
									   &lease);
		SETTLE(mesher_settle_observe, SET_GATE_TOKEN, c, NULL, 0, 0, 0, 0);
		return false;
	}
	SETTLE(mesher_settle_observe, SET_GATHER, c, request, 0, 0, 0, 0);
	g9_probe_mesh((unsigned)(request - rpc_msg), 4, false);

	chunk_ref(c);

	request->chunk = c;
	request->lease_owner = lease_owner;
	request->lease = lease;
	memset(request->result.has_displist, 0, sizeof(request->result.has_displist));
	request->dirty_generation = c->dirty_generation;
	// Snapshot the desired LOD tier (set by world_update_lod from the camera
	// distance). The worker meshes against this and never reads the camera.
	request->request.lod = c->lod;
	request->request.foliage_lod = c->foliage_lod;
	// T2 SPIKE (opt-in, PC-measured only): snapshot the geomip flag + this
	// chunk's approx camera distance, same pattern as lod/foliage_lod above --
	// the worker never reads world_lod_geomip_enabled() or gstate.players[0].camera itself.
	request->request.geomip_enabled = world_lod_geomip_enabled();
	request->request.geomip_dist_blocks = c->lod_dist_blocks;
	// T1-c: snapshot the OLD mesh's final per-list index (0 = no history) so
	// the worker can pre-size the replacement buffers instead of always
	// starting small and growing. Read here on the main thread, where c->mesh
	// is safe to touch; the worker never reads c->mesh itself.
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		request->request.prev_index[k]
			= c->has_displist[k] ? c->mesh[k].index : 0;

	if(c->ownership == FAR_SKIN)
		SETTLE(mesher_settle_observe, SET_SKIN_BEGIN, NULL, NULL, 0, 0, 0, 0);
	g9_probe_main_mark(5, true, 0, 0, 0);
	chunk_mesher_gather(c, request->request.blocks);
	g9_probe_main_mark(5, false, 1, 0, 0);
	if(c->ownership == FAR_SKIN)
		SETTLE(mesher_settle_observe, SET_SKIN_END, NULL, NULL, 0, 0, 0, 0);

	// Snapshot this column's Beta biome colour indices for the worker to resolve
	// into per-vertex grass/foliage tints. Fetched here on the main thread (the
	// world dict is single-threaded); the worker only reads the snapshot.
	request->request.has_tint
		= world_get_section_tint(c->world, c->x, c->z, request->request.grass_i,
								 request->request.grass_j);

	// Never full: requests only ever holds rpc_msg pool tokens (QLENGTH total
	// == its capacity) and this thread holds the one acquired above.
	SETTLE(mesher_settle_observe, SET_SUBMIT, NULL, request, 0, 0, 0, 0);
	g9_probe_mesh((unsigned)(request - rpc_msg), 1, false);
	g9_probe_main_mark(6, true, 0, 0, 0);
	tchannel_send(&mesher_requests, request, true);
	g9_probe_main_mark(6, false, 1, 0, 0);
	// CR-08: main-thread-only writer, plain increment (see the matching note
	// at the decrement site in chunk_mesher_receive).
	__atomic_store_n(&mesher_pending,
					 __atomic_load_n(&mesher_pending, __ATOMIC_RELAXED) + 1,
					 __ATOMIC_RELAXED);
	return true;
}

void chunk_mesher_census_static(size_t* scratch, size_t* transport) {
	*scratch = sizeof(mesher_arena_buffer);
	*transport = sizeof(rpc_msg);
}
