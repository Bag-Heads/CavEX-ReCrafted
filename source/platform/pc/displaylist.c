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

#include <GL/glew.h>
#include <assert.h>
#include <malloc.h>
#include <string.h>

#include "../displaylist.h"
#include "../displaylist_ladder.h"
#include "../displaylist_stats.h"
#include "../mem2.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Backend release is defined after this ownership implementation. */
static void dl_release_storage(struct displaylist* l);
/* GPU lifetime, shared by the real Wii backend and the delayed PC fake.
 * Detached structs are copied: chunk/mesher owners can disappear immediately.
 * The first batch is static; overflow batches use MEM2 and are cached for reuse.
 * Queue capacity alone never waits between split passes. Actual allocation
 * failure retains the bounded seal/wait fallback, never an early release.
 * A yielding atomic lock avoids lazy mutex initialization and never calls GX.
 * Critical sections only copy metadata; allocator and GPU waits stay outside. */
#define DISPLAYLIST_RETIRE_CAPACITY 256u
struct dl_retire_batch {
	struct dl_retire_batch* next;
	uint32_t count;
	struct displaylist lists[DISPLAYLIST_RETIRE_CAPACITY];
};
static struct dl_retire_batch s_retired;
static struct dl_retire_batch* s_retire_tail = &s_retired;
static struct dl_retire_batch* s_retire_free;
static uint32_t s_retired_count, s_retire_lock;
static struct displaylist_gpu_hooks s_gpu;
static uint32_t s_gpu_issued, s_gpu_completed;
static bool s_gpu_open, s_gpu_aborting;
struct displaylist_backing {
	void* data;
	size_t bytes, refs;
	uint32_t last_submit;
	uint32_t retired_lists, retired_slack, retired_pad, retired_bytes;
	uint16_t magic;
	bool submitted, released;
};
#define DL_BACKING_MAGIC UINT16_C(0xD1BA)
static uint32_t s_backing_count, s_backing_bad_free;
/* The chunk keeps the original block + stream-byte count. Its ownership
 * record is adjacent, beyond ALL GX-readable bytes, so release is O(1).
 * No MEM1 calloc, registry walk or change to the emitted streams. */
static size_t dl_backing_offset(size_t bytes) {
	return (bytes + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
}
static size_t dl_backing_allocation_bytes(size_t bytes) {
	return dl_backing_offset(bytes) + sizeof(struct displaylist_backing);
}
size_t displaylist_block_allocation_bytes(size_t stream_bytes) {
	return dl_backing_allocation_bytes(stream_bytes);
}
uint32_t displaylist_block_bad_free_count(void) {
	return __atomic_load_n(&s_backing_bad_free, __ATOMIC_RELAXED);
}

/* newlib hides usleep under strict C99, as in thread.c/gfx.c. */
int usleep(unsigned int);
static void dl_lock(void) {
	while(__atomic_exchange_n(&s_retire_lock, 1, __ATOMIC_ACQUIRE))
		usleep(200);
}
static void dl_unlock(void) {
	__atomic_store_n(&s_retire_lock, 0, __ATOMIC_RELEASE);
}
static bool dl_is_main(void) {
	return !s_gpu.is_main || s_gpu.is_main();
}
static bool dl_completed(uint32_t stamp) {
	return !s_gpu_aborting
		&& (uint32_t)(s_gpu_completed - stamp) < UINT32_C(0x80000000);
}
void displaylist_gpu_configure(const struct displaylist_gpu_hooks* hooks,
							  uint16_t initial_token) {
	assert(!s_retired_count && !s_backing_count && !s_retired.next);
	s_gpu = hooks ? *hooks : (struct displaylist_gpu_hooks){0};
	s_gpu_issued = s_gpu_completed = initial_token;
	s_gpu_open = s_gpu_aborting = false;
}
uint32_t displaylist_gpu_pending(void) {
	dl_lock();
	uint32_t count = s_retired_count;
	dl_unlock();
	return count;
}
void displaylist_gpu_reap(void) {
	assert(dl_is_main());
	if(!dl_is_main()) return;
	uint16_t raw = s_gpu.read_completed ? s_gpu.read_completed() : 0;
	dl_lock();
	/* Extend only a token in the actually issued interval. This rejects an
	 * old PE register after abort and keeps wrap comparison unambiguous. */
	uint16_t delta = (uint16_t)(raw - (uint16_t)s_gpu_completed);
	if(!s_gpu_aborting && s_gpu.read_completed
	   && delta <= s_gpu_issued - s_gpu_completed)
		s_gpu_completed += delta;
	struct dl_retire_batch* prev = NULL;
	struct dl_retire_batch* batch = &s_retired;
	while(batch) {
		for(uint32_t k = 0; k < batch->count;) {
			struct displaylist* l = &batch->lists[k];
			if(l->submitted && !dl_completed(l->last_submit)) { k++; continue; }
			struct displaylist retired = *l;
			*l = batch->lists[--batch->count];
			s_retired_count--;
			dl_unlock();
			dl_release_storage(&retired);
			dl_lock();
		}
		struct dl_retire_batch* next = batch->next;
		if(!batch->count && prev) {
			prev->next = next;
			if(s_retire_tail == batch) s_retire_tail = prev;
			batch->next = s_retire_free;
			s_retire_free = batch;
		} else prev = batch;
		batch = next;
	}
	dl_unlock();
}
bool displaylist_gpu_token_complete(uint32_t token) {
	dl_lock();
	bool done = dl_completed(token);
	dl_unlock();
	return done;
}
uint32_t displaylist_gpu_seal(void) {
	assert(dl_is_main());
	if(!dl_is_main()) return 0;
	/* Do not permit 16-bit token ambiguity, even if flips are skipped. */
	for(;;) {
		displaylist_gpu_reap();
		dl_lock();
		if(s_gpu_issued - s_gpu_completed < 0x7fffu) break;
		uint32_t oldest = s_gpu_completed + 1;
		dl_unlock();
		assert(s_gpu.wait);
		s_gpu.wait(oldest);
	}
	uint32_t token = ++s_gpu_issued;
	s_gpu_open = false;
	if(!s_gpu.read_completed) s_gpu_completed = s_gpu_issued;
	dl_unlock();
	if(s_gpu.seal) s_gpu.seal((uint16_t)token);
	return token;
}
static void dl_submit(struct displaylist* l) {
	assert(dl_is_main());
	/* GL owns its copied VBO data. No delayed CPU ownership by default. */
	if(!s_gpu.read_completed) return;
	assert(!s_gpu_aborting);
	/* MAIN owns submission, seal/abort state and every submitted list/slab.
	 * Wii workers destroy only their unpublished, unsubmitted generations;
	 * they never access these fields concurrently. Publication transfers them
	 * to MAIN. The retirement test draws while the metadata lock is held. */
	l->last_submit = s_gpu_issued + 1;
	l->submitted = true;
	s_gpu_open = true;
	if(l->backing) {
		l->backing->last_submit = l->last_submit;
		l->backing->submitted = true;
	}
}
static void dl_retire(struct displaylist* l) {
	/* Packed members transfer accounting to their slab. Only that slab's
	 * latest-use fence may release ANY part of its resident allocation. */
	if(l->block_owned && !l->accounted) {
#ifdef PLATFORM_PC
		if(!l->vbo) return; /* member CPU bytes belong to the slab, VBO does not */
#else
		return;
#endif
	}
#ifdef PLATFORM_WII
	assert(!l->submitted || dl_is_main()); /* workers never retire GPU work */
#endif
	for(;;) {
		dl_lock();
		bool release_here = true;
#ifdef PLATFORM_PC
		/* GL object deletion requires the context's MAIN thread, even when
		 * the fake has already completed this list's fence. */
		release_here = !l->vbo || dl_is_main();
#endif
		if((!l->submitted || dl_completed(l->last_submit)) && release_here) {
			dl_unlock();
			dl_release_storage(l);
			return;
		}
		struct dl_retire_batch* target = s_retire_tail;
		/* The static head cannot join the free chain; reuse its drained space
		 * too before taking a cached or newly allocated overflow batch. */
		if(target->count == DISPLAYLIST_RETIRE_CAPACITY
		   && s_retired.count < DISPLAYLIST_RETIRE_CAPACITY) target = &s_retired;
		if(target->count < DISPLAYLIST_RETIRE_CAPACITY) {
			target->lists[target->count++] = *l;
			s_retired_count++;
			dl_unlock();
			return;
		}
		struct dl_retire_batch* extra = s_retire_free;
		if(extra) s_retire_free = extra->next;
		dl_unlock();
		if(!extra) extra = mem2_alloc(sizeof(*extra));
		if(extra) {
			extra->count = 0; extra->next = NULL;
			dl_lock();
			if(s_retire_tail->count == DISPLAYLIST_RETIRE_CAPACITY) {
				s_retire_tail->next = extra;
				s_retire_tail = extra;
				extra = NULL;
			}
			if(extra) {
				extra->next = s_retire_free;
				s_retire_free = extra;
			}
			dl_unlock();
			continue;
		}
		dl_lock();
		uint32_t stamp = s_gpu_issued + 1;
		for(struct dl_retire_batch* b = &s_retired; b; b = b->next)
			for(uint32_t k = 0; k < b->count; k++)
				if((uint32_t)(stamp - b->lists[k].last_submit) < 0x80000000u)
					stamp = b->lists[k].last_submit;
		bool open = stamp == s_gpu_issued + 1;
		dl_unlock();
		if(dl_is_main()) {
			/* Can fill during either split pass, before gfx_finish. Seal the
			 * commands already submitted, never wait on an unissued token. */
			if(open) displaylist_gpu_seal();
			assert(s_gpu.wait);
			s_gpu.wait(stamp);
			displaylist_gpu_reap();
		} else {
			/* MAIN reaps once per frame; a worker must never issue GX. */
			usleep(200);
		}
	}
}
void displaylist_gpu_abort_begin(void) {
	assert(dl_is_main());
	dl_lock();
	s_gpu_aborting = true;
	dl_unlock();
}
void displaylist_gpu_abort_complete(void) {
	assert(dl_is_main());
	dl_lock();
	assert(s_gpu_aborting);
	/* Retire old/unsealed work ONLY after abort quiescence. PI reset may
	 * zero PE_TOKEN at wrap; rebase the low bits on the actual post-abort PE
	 * value while monotonically advancing the extended epoch. Thus reset 0
	 * cannot masquerade as completion of a post-abort token 65535 or 0. */
	uint32_t end = s_gpu_issued + (s_gpu_open ? 1u : 0u);
	uint16_t raw = s_gpu.read_completed ? s_gpu.read_completed() : (uint16_t)end;
	s_gpu_issued = end + (uint16_t)(raw - (uint16_t)end);
	s_gpu_completed = s_gpu_issued;
	s_gpu_open = s_gpu_aborting = false;
	dl_unlock();
	displaylist_gpu_reap();
}
static struct displaylist_backing* dl_backing_new(void* data, size_t bytes,
												 size_t refs) {
	struct displaylist_backing* b = (struct displaylist_backing*)
		((uint8_t*)data + dl_backing_offset(bytes));
	memset(b, 0, sizeof(*b));
	b->data = data; b->bytes = bytes; b->refs = refs;
	b->magic = DL_BACKING_MAGIC;
	__atomic_fetch_add(&s_backing_count, 1, __ATOMIC_RELAXED);
	return b;
}
/* Called with the metadata lock. The last list and chunk's block owner must
 * BOTH be released; the slab fence is the latest use of ANY member list. */
static struct displaylist_backing* dl_backing_detach(
	struct displaylist_backing* b) {
	if(!b->released || b->refs) return NULL;
	return b;
}
static void dl_backing_retire(struct displaylist_backing* b) {
	if(!b) return;
	struct displaylist slab = {0};
	slab.data = b->data; slab.length = dl_backing_allocation_bytes(b->bytes);
	slab.finished = true;
	slab.submitted = b->submitted; slab.last_submit = b->last_submit;
	slab.backing = b;
	dl_retire(&slab);
}
static void dl_backing_unref(struct displaylist* l) {
	if(!l->backing) return;
	dl_lock();
	struct displaylist_backing* b = l->backing;
	assert(b->refs);
	if(l->accounted) {
		b->retired_lists++;
		b->retired_slack += storage_slack(l);
		b->retired_pad += storage_pad(l);
		b->retired_bytes += (uint32_t)l->length;
		l->accounted = false;
	}
	--b->refs;
	b = dl_backing_detach(b);
	dl_unlock();
	dl_backing_retire(b);
	l->backing = NULL;
}
static void dl_backing_free(void* block, size_t bytes) {
	if(!block) return;
	dl_lock();
	struct displaylist_backing* b = (struct displaylist_backing*)
		((uint8_t*)block + dl_backing_offset(bytes));
	/* This guard is active under NDEBUG too. An invalid owner release must
	 * never mutate the candidate record (including an already retired slab). */
	if(b->magic != DL_BACKING_MAGIC || b->data != block
	   || b->bytes != bytes || b->released) {
		__atomic_fetch_add(&s_backing_bad_free, 1, __ATOMIC_RELAXED);
		dl_unlock();
		return;
	}
	b->magic = 0;
	b->released = true;
	b = dl_backing_detach(b);
	dl_unlock();
	dl_backing_retire(b);
}

static void dl_backing_release_accounting(struct displaylist* l) {
	struct displaylist_backing* b = l->backing;
	if(!b) return;
	__atomic_fetch_sub(&s_storage_lists, b->retired_lists, __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_storage_slack, b->retired_slack, __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_storage_pad, b->retired_pad, __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_storage_bytes, b->retired_bytes, __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_backing_count, 1, __ATOMIC_RELAXED);
	l->backing = NULL;
}
#include "../displaylist_scene_hash.h"
#include "../../carve_observer.h"
carve_callback displaylist_carve_observe;

// The vertex format is tightly byte-packed (22/25 bytes), so floats/halfwords land at
// offsets that are NOT naturally aligned (e.g. MEM_FLT at 14/17/18/21). A plain
// `*(float*)` store to such an address is misaligned-access UB (UBSan halts the CI ASan
// soak; x86 tolerates it at runtime). Access through aligned(1) types so gcc emits a
// safe unaligned load/store -- the bytes written are byte-identical to before.
typedef float cw_unaligned_f32 __attribute__((aligned(1)));
typedef uint16_t cw_unaligned_u16 __attribute__((aligned(1)));
typedef int16_t cw_unaligned_i16 __attribute__((aligned(1)));
#define MEM_U8(b, i) (*((uint8_t*)(b) + (i)))
#define MEM_U16(b, i) (*(cw_unaligned_u16*)((uint8_t*)(b) + (i)))
#define MEM_I16(b, i) (*(cw_unaligned_i16*)((uint8_t*)(b) + (i)))
#define MEM_FLT(b, i) (*(cw_unaligned_f32*)((uint8_t*)(b) + (i)))

#ifdef CAVEWII_DISPLAYLIST_TEST
static unsigned s_displaylist_malloc_fail_countdown;
static unsigned s_displaylist_realloc_fail_countdown;

void displaylist_test_fail_malloc_after(unsigned occurrence) {
	s_displaylist_malloc_fail_countdown = occurrence;
}

void displaylist_test_fail_realloc_after(unsigned occurrence) {
	s_displaylist_realloc_fail_countdown = occurrence;
}

static void* displaylist_test_malloc(size_t size) {
	if(s_displaylist_malloc_fail_countdown > 0
	   && --s_displaylist_malloc_fail_countdown == 0)
		return NULL;
	return malloc(size);
}
static void* displaylist_test_realloc(void* ptr, size_t size) {
	if(s_displaylist_realloc_fail_countdown > 0
	   && --s_displaylist_realloc_fail_countdown == 0)
		return NULL;
	return realloc(ptr, size);
}
#define DISPLAYLIST_MALLOC(size) displaylist_test_malloc(size)
#define DISPLAYLIST_REALLOC(ptr, size) displaylist_test_realloc(ptr, size)
#else
#define DISPLAYLIST_MALLOC(size) malloc(size)
#define DISPLAYLIST_REALLOC(ptr, size) realloc(ptr, size)
#endif

static void* census_list_malloc(struct displaylist* l) {
	carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	void* p = DISPLAYLIST_MALLOC(l->length);
	carve_note(displaylist_carve_observe, CARVE_ALLOC, p, l, l->length, 0);
	carve_note(displaylist_carve_observe, CARVE_TAG, p, 0, l->length, CARVE_LIST);
	carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
	return p;
}
static void* census_list_realloc(struct displaylist* l, size_t bytes) {
	carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	uintptr_t old = (uintptr_t)l->data;
	void* p = DISPLAYLIST_REALLOC(l->data, bytes);
	carve_note(displaylist_carve_observe, CARVE_RESIZE, old, p, bytes, 0);
	carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
	return p;
}

void displaylist_init(struct displaylist* l, size_t vertices,
					  size_t vertex_size) {
	assert(l && vertices > 0 && vertex_size > 0);

	l->length = 4096;
	l->data = NULL;
	l->index = 0;
	l->finished = false;
	l->accounted = false;
	l->block_owned = false;
	l->scratch_ok = false;
	l->submitted = false;
	l->last_submit = 0;
	l->backing = NULL;
	l->vbo = 0;
	l->truncated = false; // PC mem2_alloc=malloc rarely OOMs; stays false, never dropped
	l->tinted = false;
	l->tint[0] = l->tint[1] = l->tint[2] = 255; // white = no tint
	carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(displaylist_carve_observe, CARVE_DL_INIT, l, 0, vertices, vertex_size);
	carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
}

// T1-c (2026-09-03, Wii-targeted; mirrored here for API/build symmetry -- see
// the Wii backend for the full rationale). PC's `index` is a BYTE offset
// only until displaylist_finalize overwrites it with the vertex COUNT, so
// the "previous build's index" hint chunk_mesher.c snapshots (whatever units
// the platform's OWN `index` carries post-finalize) must be converted back
// to bytes with this platform's own stride before it can size `length` (a
// byte capacity); `tinted` (mirroring the flag the caller sets right after
// this call) selects that stride. On PC this rarely matters in practice
// (plain malloc realloc is comparatively cheap and PC isn't the hardware
// target of this fix) but keeping the two backends behaviourally consistent
// avoids a divergent no-op stub silently masking a future PC regression.
void displaylist_init_presized(struct displaylist* l, size_t vertices,
							   size_t vertex_size, bool tinted,
							   size_t prior_index) {
	displaylist_init(l, vertices, vertex_size);

	// LADDER (2026-09-08, displaylist_ladder.h): snap the hint to this
	// backend's own rung table -- the 4096 B doubling chain displaylist_init
	// and the two grow paths already walk -- instead of taking the raw prior
	// byte count. Sizing a rebuild to an EXACT prior byte count is the same
	// off-ladder capacity bug the Wii backend had, in the same code shape, and
	// it is what the PC split harness measures (dl_used/dl_slack, DL moves), so
	// the fix has to be visible on the instrument that measures it. Capacity
	// only; `index` still governs every emitted byte.
	//
	// MEASURED, and the trade is NOT free on this backend. run_split_anchor_pc
	// (seed 42, sep 256, fv6, voxel), three runs per arm vs 6a2c5673: DL moves
	// at readiness 12,131 -> 11,641 mean (-3.9%, spread <1% on both arms) and
	// dl_used unchanged to 0.02% (the T1 claim) -- but dl_slack at readiness
	// 32.7 MB -> 65.6 MB, because THIS table is a 4096 B DOUBLING chain, so a
	// snap costs up to 2x where the Wii table steps by 1.26. PC has no carve
	// and malloc bins by size class, so that 33 MB buys contiguity nothing here
	// needs. If a future round wants the PC arm cheap, densify this table the
	// way the Wii one is densified rather than dropping the snap -- dropping it
	// also drops the only PC-visible signal the change has.
	size_t prior_bytes = displaylist_rung_pc(
		prior_index * (size_t)(tinted ? 25 : 22));
	if(prior_bytes > l->length)
		l->length = prior_bytes;
	carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(displaylist_carve_observe, CARVE_DL_HINT, l, 0, prior_index, tinted);
	carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
}

// T1-c grow-churn gauge -- see the getter's declaration in
// platform/displaylist.h for the full rationale. Mirrored here for API/build
// symmetry with the Wii backend; PC's realloc-doubling grow is comparatively
// cheap, so this mainly keeps the two backends' instrumentation from
// diverging silently.
uint32_t g_displaylist_grow_count = 0;
uint32_t displaylist_grow_count(void) { return g_displaylist_grow_count; }

void displaylist_set_tinted(struct displaylist* l, bool tinted) {
	assert(l);
	l->tinted = tinted;
}

void displaylist_set_tint(struct displaylist* l, uint8_t r, uint8_t g,
						  uint8_t b) {
	assert(l);
	l->tint[0] = r;
	l->tint[1] = g;
	l->tint[2] = b;
}

static void dl_release_storage(struct displaylist* l) {
	storage_retire(l);
	dl_backing_release_accounting(l);

	if(!l->block_owned) {
		carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
		carve_note(displaylist_carve_observe, CARVE_FREE, l->data, l, 0, 0);
		if(l->data) free(l->data);
		carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
	}

	if(l->vbo)
		glDeleteBuffers(1, &l->vbo);
}

void displaylist_destroy(struct displaylist* l) {
	assert(l);
	dl_backing_unref(l);
	dl_retire(l);
	l->data = NULL;
	l->length = l->index = 0;
	l->finished = l->accounted = l->block_owned = false;
	l->scratch_ok = l->submitted = false;
	l->last_submit = 0;
	l->vbo = 0;
}

void displaylist_reset(struct displaylist* l) {
	assert(l && !l->finished);
	storage_retire(l);
	l->index = 0;
}

void displaylist_finalize(struct displaylist* l, uint16_t vtxcnt) {
	assert(l && !l->finished && l->data);
	storage_retire(l);

	/* No GX padding on PC. Keep the old allocation on shrink failure;
	 * neither the vertex stream nor the later GL upload range changes. */
	// LADDER, deliberately NOT applied here. The ladder exists to make a freed
	// block an exact refit in the Wii carve's single first-fit __lwp_heap; PC
	// finalizes into a general-purpose malloc that already bins by size class
	// and has no carve to shred, so snapping the compaction target to this
	// backend's 4096 B doubling chain would buy nothing and cost up to a
	// doubling of retained bytes on every small list. The PC ladder is applied
	// where the bug actually was -- displaylist_init_presized above, which sized
	// a rebuild to an exact prior byte count -- and that is also the path the
	// split harness measures (compaction is off by default there).
	size_t used = (size_t)vtxcnt * (l->tinted ? 25 : 22);
	if(displaylist_compact_enabled() && used && used < l->length) {
		void* compact = census_list_realloc(l, used);
		if(compact) { l->data = compact; l->length = used; }
	}

	carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(displaylist_carve_observe, CARVE_FINAL, l->data, l,
		(size_t)vtxcnt * (l->tinted ? 25 : 22),
		35 + (size_t)vtxcnt * (l->tinted ? 12 : 9));
	carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
	l->index = vtxcnt;
	storage_finalize(l);
}

void displaylist_pos(struct displaylist* l, int16_t x, int16_t y, int16_t z) {
	assert(l && !l->finished);
	if(l->truncated)
		return; // a prior alloc failed; the list is already being dropped

	if(!l->data) {
		l->data = census_list_malloc(l);
		if(!l->data) {
			l->truncated = true; // OOM: drop the list (mirrors the Wii path) vs the
			return;				 // NDEBUG-masked NULL store. PC/bench: ~never hit.
		}
	}

	if(l->index + (l->tinted ? 25 : 22) > l->length) {
		// realloc-on-failure keeps the old buffer + returns NULL -- use a temp so we
		// don't null (and leak) l->data, then drop the list. (Same bug class as stack.)
		void* nd = census_list_realloc(l, l->length * 2);
		if(!nd) {
			l->truncated = true;
			return;
		}
		l->data = nd;
		l->length *= 2;
		g_displaylist_grow_count++;
	}

	MEM_FLT(l->data, l->index) = (float)x / 256.0F;
	l->index += 4;
	MEM_FLT(l->data, l->index) = (float)y / 256.0F;
	l->index += 4;
	MEM_FLT(l->data, l->index) = (float)z / 256.0F;
	l->index += 4;
}

void displaylist_color(struct displaylist* l, uint8_t index) {
	assert(l && !l->finished);
	if(l->truncated || !l->data)
		return; // pos() OOM'd and dropped the list -- don't write through NULL

	MEM_U8(l->data, l->index++) = index % 16;
	MEM_U8(l->data, l->index++) = index / 16;
	// Per-vertex biome tint (attribute a_color) follows the light index on
	// tinted chunk-mesh lists; untinted lists keep the original 22-byte vertex.
	if(l->tinted) {
		MEM_U8(l->data, l->index++) = l->tint[0];
		MEM_U8(l->data, l->index++) = l->tint[1];
		MEM_U8(l->data, l->index++) = l->tint[2];
	}
}

void displaylist_texcoord(struct displaylist* l, uint8_t s, uint8_t t) {
	assert(l && !l->finished);
	if(l->truncated || !l->data)
		return; // pos() OOM'd and dropped the list -- don't write through NULL
	MEM_FLT(l->data, l->index) = (float)s / 256.0F;
	l->index += 4;
	MEM_FLT(l->data, l->index) = (float)t / 256.0F;
	l->index += 4;
}

void displaylist_vertex(struct displaylist* l, int16_t x, int16_t y, int16_t z,
						uint8_t light, uint8_t s, uint8_t t) {
	// Byte-identical to pos(x,y,z) + color(light) + texcoord(s,t); the null
	// check, the whole-vertex capacity reserve, the data/index/tinted loads and
	// the index advance are hoisted to once-per-vertex. See the Wii version.
	assert(l && !l->finished);
	if(l->truncated)
		return; // a prior alloc failed; the list is already being dropped

	if(!l->data) {
		l->data = census_list_malloc(l);
		if(!l->data) {
			l->truncated = true; // OOM: drop the list vs the NDEBUG-masked NULL store
			return;
		}
	}

	if(l->index + (l->tinted ? 25 : 22) > l->length) {
		void* nd = census_list_realloc(l, l->length * 2);
		if(!nd) {
			l->truncated = true; // keep the old buffer (realloc didn't free it); drop
			return;
		}
		l->data = nd;
		l->length *= 2;
		g_displaylist_grow_count++;
	}

	// Offsets mirror pos (3 floats @0,4,8) + color (2 bytes @12,13 [+ tint
	// @14,15,16]) + texcoord (2 floats). restrict: l->data never overlaps the
	// control struct.
	uint8_t* restrict p = (uint8_t*)l->data + l->index;
	MEM_FLT(p, 0) = (float)x / 256.0F;
	MEM_FLT(p, 4) = (float)y / 256.0F;
	MEM_FLT(p, 8) = (float)z / 256.0F;
	MEM_U8(p, 12) = light % 16;
	MEM_U8(p, 13) = light / 16;
	if(l->tinted) {
		MEM_U8(p, 14) = l->tint[0];
		MEM_U8(p, 15) = l->tint[1];
		MEM_U8(p, 16) = l->tint[2];
		MEM_FLT(p, 17) = (float)s / 256.0F;
		MEM_FLT(p, 21) = (float)t / 256.0F;
		l->index += 25;
	} else {
		MEM_FLT(p, 14) = (float)s / 256.0F;
		MEM_FLT(p, 18) = (float)t / 256.0F;
		l->index += 22;
	}
}

// Per-frame draw-call + triangle counters -- mirrors the Wii backend (see
// platform/wii/displaylist.c for the full rationale). Counted only in
// displaylist_render (the chunk-mesh draw path), not
// displaylist_render_immediate (piston-moving blocks -- not chunk traffic).
static uint32_t g_frame_dlcalls = 0;
static uint32_t g_frame_tris = 0;

void displaylist_frame_counters_reset(void) {
	g_frame_dlcalls = 0;
	g_frame_tris = 0;
}

void displaylist_frame_counters_get(uint32_t* dlcalls, uint32_t* tris) {
	if(dlcalls)
		*dlcalls = g_frame_dlcalls;
	if(tris)
		*tris = g_frame_tris;
}

void displaylist_render(struct displaylist* l) {
	assert(l);
	if(!l->data || l->truncated) return;

	// Tinted lists insert a 3-byte a_color (biome tint) between the light index
	// and the texcoord, growing the vertex 22 -> 25 bytes.
	int stride = l->tinted ? 25 : 22;

	if(!l->finished) {
		l->finished = true;

		glGenBuffers(1, &l->vbo);
		glBindBuffer(GL_ARRAY_BUFFER, l->vbo);
		glBufferData(GL_ARRAY_BUFFER, l->index * stride, l->data,
					 GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	/* R5 item 2b: the vertex bytes this list hands to GL (the PC analogue
	 * of the Wii GX_CallDispList range), when the scene hash is armed. */
	if(g_dl_scene_hash_armed)
		dl_scene_hash_list(l->data, l->index * stride);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(3);
	glEnableVertexAttribArray(2);

	glBindBuffer(GL_ARRAY_BUFFER, l->vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, NULL);
	glVertexAttribPointer(3, 2, GL_UNSIGNED_BYTE, GL_FALSE, stride,
						  (uint8_t*)(3 * sizeof(float)));
	if(l->tinted) {
		// a_color = the biome tint, normalised 0..1; texcoord shifts by 3 bytes.
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, stride,
							  (uint8_t*)(2 + 3 * sizeof(float)));
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
							  (uint8_t*)(2 + 3 + 3 * sizeof(float)));
	} else {
		// No per-vertex tint: feed a constant white a_color so the shader's
		// lighting*a_color multiply is a no-op for normal blocks.
		glDisableVertexAttribArray(1);
		glVertexAttrib4f(1, 1.0f, 1.0f, 1.0f, 1.0f);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
							  (uint8_t*)(2 + 3 * sizeof(float)));
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	dl_submit(l);
	glDrawArrays(GL_QUADS, 0, l->index);
	g_frame_dlcalls++;
	g_frame_tris += (uint32_t)(l->index / 4) * 2; // GL_QUADS: 2 tris/quad

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(3);
	glDisableVertexAttribArray(2);
	if(l->tinted)
		glDisableVertexAttribArray(1);
}

void displaylist_render_immediate(struct displaylist* l, uint16_t vtxcnt) {
	assert(l && l->data && !l->finished);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(3);
	glEnableVertexAttribArray(2);

	// The stride was hardcoded to the UNTINTED 22, so a tinted list (25 B/vertex,
	// tint at 14,15,16 and the texcoord shifted to 17,21) was walked at the wrong
	// pitch and its texcoords read out of the tint bytes. Mirror displaylist_render:
	// same layout rule, so the piston's moved grass/leaves block draws correctly
	// here too. (On Wii the same bug also desynced the GX FIFO -- see that backend.)
	int stride = l->tinted ? 25 : 22;
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, l->data);
	glVertexAttribPointer(3, 2, GL_UNSIGNED_BYTE, GL_FALSE, stride,
						  (uint8_t*)l->data + 3 * sizeof(float));
	if(l->tinted) {
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, stride,
							  (uint8_t*)l->data + 2 + 3 * sizeof(float));
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
							  (uint8_t*)l->data + 2 + 3 + 3 * sizeof(float));
	} else {
		glDisableVertexAttribArray(1);
		glVertexAttrib4f(1, 1.0f, 1.0f, 1.0f, 1.0f);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
							  (uint8_t*)l->data + 2 + 3 * sizeof(float));
	}

	glDrawArrays(GL_QUADS, 0, vtxcnt);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(3);
	glDisableVertexAttribArray(2);
	if(l->tinted)
		glDisableVertexAttribArray(1);
}


/* M2 mesh blocks are a Wii (GX display list) mechanism; the PC VBO path has no
 * carve to protect. Stubs keep chunk_mesher.c platform-agnostic. */
/* The normal PC path stays immediate and has no slabs. The fake GPU can
 * exercise real shared ownership/retirement through the same packing API. */
static bool s_pc_test_block_mode;
void displaylist_set_block_mode(bool enabled) {
	s_pc_test_block_mode = enabled && s_gpu.read_completed;
}
bool displaylist_block_mode(void) { return s_pc_test_block_mode; }
void displaylist_scratch_begin(void) {}
void displaylist_scratch_end(void) {}
void displaylist_allow_scratch(struct displaylist* l) { (void)l; }
bool displaylist_pack_block(struct displaylist* lists, const bool* has,
							size_t n, void** out_block, size_t* out_bytes) {
	*out_block = NULL;
	*out_bytes = 0;
	if(!s_pc_test_block_mode) return true;
	size_t bytes = 0, refs = 0;
	for(size_t k = 0; k < n; k++) {
		if(!has[k] || !lists[k].data) continue;
		assert(!lists[k].submitted && !lists[k].block_owned);
		bytes += lists[k].length;
		refs++;
	}
	if(!bytes) return true;
	uint8_t* block = DISPLAYLIST_MALLOC(dl_backing_allocation_bytes(bytes));
	if(!block) return false;
	struct displaylist_backing* backing = dl_backing_new(block, bytes, refs);
	size_t off = 0;
	for(size_t k = 0; k < n; k++) {
		struct displaylist* l = &lists[k];
		if(!has[k] || !l->data) continue;
		memcpy(block + off, l->data, l->length);
		free(l->data);
		l->data = block + off;
		l->block_owned = true;
		l->backing = backing;
		off += l->length;
	}
	*out_block = block;
	*out_bytes = bytes;
	return true;
}
void displaylist_block_free(void* block, size_t bytes) {
	dl_backing_free(block, bytes);
}
void displaylist_block_counters(uint32_t* packed, uint32_t* overflow,
								uint32_t* pack_fail) {
	if(packed) *packed = 0;
	if(overflow) *overflow = 0;
	if(pack_fail) *pack_fail = 0;
}
void displaylist_block_stats(struct displaylist_block_stats* out) {
	memset(out, 0, sizeof(*out));
}
