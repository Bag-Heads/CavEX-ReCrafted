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
#include <gccore.h>
#include <malloc.h>
#include <stdlib.h>
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
#include "../mem2.h"

#define GX_NOP 0
#define DISPLAYLIST_CLL 32

/* ---- M2 mesh blocks: per-build scratch arena + exact-size generation block ---- */
/* Sized for INTERLEAVED emission (the mesher fills all 26 lists in one block
 * loop, so a grow usually cannot extend in place and copies): the host
 * equivalence test measures ~3x the finished bytes for an unhinted build; the
 * measured max generation is 83 KiB (census 2026-09-09) -> 768 KiB covers it
 * with margin. Lives in the general MEM2 heap (MALLOC_MEM2), never the carve;
 * an overflow falls back per list to the carve and is counted (dlbo). */
#define DISPLAYLIST_SCRATCH_BYTES (768u * 1024u)
static bool s_block_mode;
static uint8_t* s_scratch_base;
static size_t s_scratch_cap, s_scratch_used;
static bool s_scratch_active;
static uint32_t s_block_packed, s_block_overflow, s_block_pack_fail;
static bool s_block_requested;
static uint32_t s_block_scratch_fail, s_block_grow_copies, s_block_copy_bytes,
	s_block_spill_bytes;

static bool dl_in_scratch(const void* p) {
	/* base is published with RELEASE after cap (scratch_begin, worker);
	 * this runs on the main thread too (destroy) -> ACQUIRE pairs with it. */
	const uint8_t* base = __atomic_load_n(&s_scratch_base, __ATOMIC_ACQUIRE);
	return base && (const uint8_t*)p >= base
		&& (const uint8_t*)p < base + s_scratch_cap;
}

static void* dl_scratch_take(size_t size) {
	size_t need = (size + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL * DISPLAYLIST_CLL;
	if(!s_scratch_active || s_scratch_used + need > s_scratch_cap)
		return NULL;
	void* p = s_scratch_base + s_scratch_used;
	s_scratch_used += need;
	return p;
}

/* A list's first allocation: scratch when a build window is open, else the
 * carve exactly as before. Overflowing the scratch falls back to the carve for
 * THIS list only (counted); the pack step leaves such lists carve-owned. */
static void* dl_alloc_list(struct displaylist* l, size_t size) {
	if(s_scratch_active && l->scratch_ok) {
		void* p = dl_scratch_take(size);
		if(p)
			return p;
		__atomic_fetch_add(&s_block_overflow, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&s_block_spill_bytes, (uint32_t)size, __ATOMIC_RELAXED);
	}
	return mem2_tag(mem2_alloc(size), size, CARVE_LIST);
}

/* Grow: scratch-resident lists bump a new region and copy (the old region is
 * reclaimed by the next scratch_begin); carve-resident lists keep the
 * mem2_realloc path unchanged. */
static void* dl_grow_list(struct displaylist* l, size_t old_total,
						  size_t new_total) {
	void* old = l->data;
	if(l->scratch_ok && dl_in_scratch(old)) {
		/* The growing list is the top of the bump arena in the common
		 * run-of-faces case: extend it in place, no copy, no dead region. */
		size_t old_need = (old_total + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL * DISPLAYLIST_CLL;
		size_t new_need = (new_total + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL * DISPLAYLIST_CLL;
		if((uint8_t*)old + old_need == s_scratch_base + s_scratch_used
		   && s_scratch_used - old_need + new_need <= s_scratch_cap) {
			s_scratch_used += new_need - old_need;
			return old;
		}
		void* p = dl_scratch_take(new_total);
		if(!p) {
			/* NEW-4 note: an overflowed list lands in the ledger as a plain
			 * carve alloc (no CARVE_TRANSFER); dlbo is the signal it happened. */
			__atomic_fetch_add(&s_block_overflow, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&s_block_spill_bytes, (uint32_t)new_total,
							   __ATOMIC_RELAXED);
			p = mem2_tag(mem2_alloc(new_total), new_total, CARVE_LIST);
			if(!p)
				return NULL;
		} else {
			__atomic_fetch_add(&s_block_grow_copies, 1, __ATOMIC_RELAXED);
		}
		memcpy(p, old, old_total < new_total ? old_total : new_total);
		return p;
	}
	return mem2_realloc(old, old_total, new_total);
}

void displaylist_set_block_mode(bool enabled) {
	s_block_requested = enabled;
	s_block_mode = enabled;
}

bool displaylist_block_mode(void) {
	return s_block_mode;
}

void displaylist_scratch_begin(void) {
	if(!s_block_mode)
		return;
	if(!s_scratch_base) {
		/* General heap, never the carve: the arena is the one allocation the
		 * mesher path keeps forever, and it must not fragment the pool it is
		 * there to protect. */
		/* scratch raw: over-allocate and align the base to a GX cache line so
		 * every scratch region (dl_scratch_take rounds to 32) is 32 B aligned
		 * like a carve block would be; the arena is process-lifetime. */
		uint8_t* raw = malloc(DISPLAYLIST_SCRATCH_BYTES + DISPLAYLIST_CLL);
		if(!raw) {
			/* Effective mode OFF: every later build takes the carve path.
			 * Counted, and reported through displaylist_block_stats. */
			__atomic_fetch_add(&s_block_scratch_fail, 1, __ATOMIC_RELAXED);
			s_block_mode = false;
			return;
		}
		uint8_t* base = (uint8_t*)(((uintptr_t)raw + DISPLAYLIST_CLL - 1)
								   & ~(uintptr_t)(DISPLAYLIST_CLL - 1));
		/* cap first, then base: displaylist_destroy on the main thread reads
		 * both (dl_in_scratch) and must never see base with a zero cap. */
		s_scratch_cap = DISPLAYLIST_SCRATCH_BYTES;
		__atomic_store_n(&s_scratch_base, base, __ATOMIC_RELEASE);
	}
	s_scratch_used = 0;
	s_scratch_active = true;
}

void displaylist_scratch_end(void) {
	s_scratch_active = false;
}

void displaylist_allow_scratch(struct displaylist* l) {
	assert(l);
	l->scratch_ok = s_block_mode;
}

void displaylist_block_free(void* block, size_t bytes) {
	dl_backing_free(block, bytes);
}

void displaylist_block_counters(uint32_t* packed, uint32_t* overflow,
								uint32_t* pack_fail) {
	/* Sole writer is the mesher worker; main reads for the heartbeat. */
	if(packed) *packed = __atomic_load_n(&s_block_packed, __ATOMIC_RELAXED);
	if(overflow) *overflow = __atomic_load_n(&s_block_overflow, __ATOMIC_RELAXED);
	if(pack_fail) *pack_fail = __atomic_load_n(&s_block_pack_fail, __ATOMIC_RELAXED);
}

void displaylist_block_stats(struct displaylist_block_stats* out) {
	assert(out);
	out->mode_requested = s_block_requested;
	out->mode_effective = s_block_mode;
	out->scratch_fail = __atomic_load_n(&s_block_scratch_fail, __ATOMIC_RELAXED);
	out->grow_copies = __atomic_load_n(&s_block_grow_copies, __ATOMIC_RELAXED);
	out->copy_bytes = __atomic_load_n(&s_block_copy_bytes, __ATOMIC_RELAXED);
	out->spill_bytes = __atomic_load_n(&s_block_spill_bytes, __ATOMIC_RELAXED);
}

bool displaylist_pack_block(struct displaylist* lists, const bool* has,
							size_t n, void** out_block, size_t* out_bytes) {
	assert(lists && has && out_block && out_bytes);
	*out_block = NULL;
	*out_bytes = 0;
	size_t total = 0, refs = 0;
	for(size_t k = 0; k < n; k++) {
		struct displaylist* l = &lists[k];
		if(!has[k] || !l->data || !l->finished || !dl_in_scratch(l->data))
			continue;
		assert(l->length % DISPLAYLIST_CLL == 0);
		total += l->length;
		refs++;
	}
	if(!total)
		return true;
	size_t allocated = dl_backing_allocation_bytes(total);
	uint8_t* block = mem2_tag(mem2_alloc(allocated), allocated, CARVE_LIST);
	if(!block) {
		__atomic_fetch_add(&s_block_pack_fail, 1, __ATOMIC_RELAXED);
		return false;
	}
	struct displaylist_backing* backing = dl_backing_new(block, total, refs);
	size_t off = 0, gx_used = 0;
	for(size_t k = 0; k < n; k++) {
		struct displaylist* l = &lists[k];
		if(!has[k] || !l->data || !l->finished || !dl_in_scratch(l->data))
			continue;
		memcpy(block + off, l->data, l->length);
		l->data = block + off;
		l->block_owned = true;
		l->backing = backing;
		off += l->length;
		gx_used += l->index;
	}
	assert(off == total);
	DCStoreRange(block, total);
	/* ONE finalize note for the block record (the ledger keys records by
	 * their base address; per-list notes at block+offset would no-op and
	 * leave the record reporting the whole block minus the first list as
	 * slack). Include the ownership tail in resident bytes; gx_used remains
	 * only the summed vertex-stream length, with no metadata sent to GX. */
	carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(displaylist_carve_observe, CARVE_FINAL, block, 0, allocated, gx_used);
	carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
	__atomic_fetch_add(&s_block_packed, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_block_copy_bytes, (uint32_t)total, __ATOMIC_RELAXED);
	*out_block = block;
	*out_bytes = total;
	return true;
}
#define DISPLAYLIST_CACHE_LINES(v, s)                                          \
	(((v) * (s) + 3 + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL * DISPLAYLIST_CLL)

#define MEM_U8(b, i) (*((uint8_t*)(b) + (i)))
#define MEM_U16(b, i) (*(uint16_t*)((uint8_t*)(b) + (i)))
#define MEM_I16(b, i) (*(int16_t*)((uint8_t*)(b) + (i)))

void displaylist_init(struct displaylist* l, size_t vertices,
					  size_t vertex_size) {
	assert(l && vertices > 0 && vertex_size > 0);

	// LADDER (displaylist_ladder.h): every capacity this backend ever sets is
	// the smallest rung >= the request, so a freed list block is an EXACT refit
	// for the next list of that rung instead of being split by first-fit.
	// Capacity only -- `index` still governs every emitted byte.
	l->length = displaylist_rung(DISPLAYLIST_CACHE_LINES(vertices, vertex_size));
	l->data = NULL;
	/* has 32 byte padding of GX_NOP in front for possible misalignment caused
	 * by realloc */
	l->index = DISPLAYLIST_CLL + 3;
	l->finished = false;
	l->accounted = false;
	l->truncated = false;
	l->tinted = false;
	l->block_owned = false;
	l->scratch_ok = false;
	l->submitted = false;
	l->last_submit = 0;
	l->backing = NULL;
	l->tint[0] = l->tint[1] = l->tint[2] = 255; // white = no tint
}

// T1-c (2026-09-03 far-ring frame-drop root cause, mechanism A): identical to
// displaylist_init, except `length` is additionally raised to cover
// `prior_index` (a caller-supplied hint -- in practice the FINAL l->index a
// previous build of the same chunk/side reached, snapshotted by
// chunk_mesher_send before the old list is replaced) rounded up to the cache
// line. `length` and `index` already share one coordinate frame on this
// backend (both counted from l->data's offset 0; see displaylist_pos/vertex's
// `index + vtx_bytes > length` grow check), so rounding the remembered index
// up to DISPLAYLIST_CLL is sufficient to guarantee `length >= prior_index`
// without re-deriving the header/pad arithmetic -- `tinted` is accepted for
// API symmetry with the PC backend (which needs it to convert its own
// vertex-count-shaped hint to bytes) but is unused here. prior_index == 0 (no
// history: a never-before-built chunk, or a shape the caller chooses not to
// track) reproduces plain displaylist_init exactly -- today's initial size
// stays reachable. This changes ONLY the capacity of the buffer
// displaylist_pos/displaylist_vertex lazily allocate on first touch; the
// bytes they go on to write are governed solely by `index`, so a rebuild of
// an unchanged-shape chunk emits byte-identical output while skipping the
// mem2_realloc allocate+copy+free grow(s) it would otherwise need (proof:
// tests/test_displaylist_vertex_equiv.c's T1-c section).
void displaylist_init_presized(struct displaylist* l, size_t vertices,
							   size_t vertex_size, bool tinted,
							   size_t prior_index) {
	(void)tinted;
	displaylist_init(l, vertices, vertex_size);

	// LADDER: the hint is snapped to a RUNG, not to the 32 B cache line. The
	// prior build's own capacity was itself the smallest rung >= the index it
	// reached, so rung(prior_index) is that same rung -- this rebuild asks for
	// exactly the size of the block it is about to reuse, which is what makes
	// the refit exact rather than a first-fit split leaving a 32 B crumb.
	if(prior_index > l->length) {
		size_t hinted = displaylist_rung(prior_index);
		if(hinted > l->length)
			l->length = hinted;
	}
}

// Count of mem2_realloc grow cycles (displaylist_pos/displaylist_vertex),
// across every list -- the allocate+copy+free churn T1-c targets. Sole writer
// is the mesher worker thread (both emit paths run there); read by the debug
// overlay / a future perf gauge. Plain (not atomic): same single-writer,
// approximate-reader treatment as chunk_mesher.c's mesher_build_ms_* (a
// display-only gauge, not a correctness-load-bearing count).
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

	if(l->data && !l->block_owned && !dl_in_scratch(l->data))
		// Size-aware free verifies the trailing red-zone canary: display lists
		// are the engine's hottest mem2 churn surface, so this is the primary
		// tripwire for the stray-writer corruption class.
		mem2_free_sized(l->data, l->length + (l->finished ? 0 : DISPLAYLIST_CLL));
}

void displaylist_destroy(struct displaylist* l) {
	assert(l);
	dl_backing_unref(l);
	dl_retire(l);
	// Idempotent teardown: NULL the pointer so a second destroy (a latent double-free in
	// the mesh receive/evict paths) is a no-op instead of freeing the same MEM2 block
	// twice -- a double mem2_free corrupts the lwp_heap free-list into a cycle that the
	// next allocation walks forever (interrupts off) = a frozen, no-dump hang.
	l->data = NULL;
	l->length = 0;
	l->block_owned = false;
	l->scratch_ok = false; /* the worker re-grants it per build */
	// Also clear index + finished (2026-06-13): displaylist_render early-outs on
	// !finished and the CALL-site guard rejects index < CLL+3, so a destroyed
	// list can never form a garbage GX_CallDispList even if has_displist[] lags
	// the destroy on some future path. Cheap belt-and-braces for the rank-2
	// (corrupt-struct) wedge vector.
	l->index = 0;
	l->finished = false;
	l->accounted = false;
	l->submitted = false;
	l->last_submit = 0;
}

void displaylist_reset(struct displaylist* l) {
	assert(l && !l->finished);
	storage_retire(l);
	l->index = DISPLAYLIST_CLL + 3;
	l->truncated = false;
}

// Count of display lists whose declared vertex count did not match the bytes
// written -- the 2026-06-13 GP-wedge investigation's CPU-side tripwire for the
// malformed-list producer (the far-ring LOD lists are the only producer with no
// byte-exact test). Surfaced in the watchdog hang report as "dlbad=". Should be 0.
uint32_t g_displaylist_malformed = 0;

void displaylist_finalize(struct displaylist* l, uint16_t vtxcnt) {
	assert(l && !l->finished && l->data);

	size_t stride = l->tinted ? 12 : 9;
	size_t written = (size_t)(l->index - (DISPLAYLIST_CLL + 3));

	// Stream invariant: the header's vertex count must match the bytes actually
	// written, or the GP parses the NOP tail (or buffer beyond) as opcodes --
	// the cmdIdle=0 wedge class. The assert fails the offending PRODUCER in the
	// asserts-on PC tests...
	assert(written == (size_t)vtxcnt * stride);

	// ...but the .dol ships NDEBUG, so the assert is GONE on exactly the
	// platform that wedges (the 2026-06-13 full-FIFO GP-wedge root cause). The
	// RUNTIME GUARD: emit only the count the bytes ACTUALLY support (floor), so
	// an over-declaring producer can NEVER make the GP read past the written
	// data into NOPs/garbage. A short list draws its valid prefix and drops the
	// rest -- a visual gap, never a wedge -- and the event is counted for the
	// hang report so the producer still gets named on hardware.
	uint16_t safe = (uint16_t)(written / stride);
	if(safe != vtxcnt) {
		g_displaylist_malformed++;
		vtxcnt = safe;
	}

	// Tinted chunk meshes carry a second (direct-RGB) colour channel, so they
	// are drawn with GX_VTXFMT4 (configured in gfx.c) instead of the plain
	// indexed-light GX_VTXFMT0 used by everything else.
	MEM_U8(l->data, DISPLAYLIST_CLL)
		= GX_QUADS | ((l->tinted ? GX_VTXFMT4 : GX_VTXFMT0) & 7);
	MEM_U16(l->data, DISPLAYLIST_CLL + 1) = vtxcnt;

	// T1-b (2026-09-03 far-ring frame-drop root cause, mechanism A): flush only
	// the bytes the GP will ever read, not the whole (possibly 1.5x-grown, or
	// T1-c pre-sized) buffer out to `length`. displaylist_render's own
	// GX_CallDispList call below derives its size argument from l->index,
	// rounded up to this same DISPLAYLIST_CLL cache line -- NEVER from
	// l->length -- so a DCStoreRange out to `length` flushed dead bytes the CP
	// never fetches. `length` can be far bigger than `index` in the common
	// case (grow slack, or a T1-c history hint sized for a larger prior
	// build), so this is a real reduction, not a rename; the emitted bytes in
	// [0, call_len) are byte-for-byte identical to before -- only the
	// flushed/padded BYTE COUNT shrinks. Pad the tail NOPs to the same
	// boundary (not out to `length`) for the same reason: nothing past
	// call_len is ever parsed by the GP, so zeroing it was wasted CPU on
	// every finalize.
	size_t call_len
		= (l->index + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL * DISPLAYLIST_CLL;

	/* libogc gx.h GX_CallDispList: aligned address, 32B-rounded length,
	 * unused command bytes = GX_NOP. GX_BeginDispList's extra recording
	 * line is not needed by this CPU emitter. Keep the leading NOP line,
	 * rounded tail and mem2's 256B canary; never change index or CALL/flush.
	 * Carve shrink releases the tail in place; no vertex-stream copy.
	 * Finished length is the actual requested allocation (no extra line). */
	if(dl_in_scratch(l->data)) {
		/* M2: the stream is exact-size by construction once packed; the
		 * flush and the finalize note happen in displaylist_pack_block at
		 * the address the GPU will read. */
		l->length = call_len;
		memset(l->data, GX_NOP, DISPLAYLIST_CLL);
		memset((uint8_t*)l->data + l->index, GX_NOP, call_len - l->index);
		l->finished = true;
		storage_finalize(l);
		return;
	}

	size_t allocated = l->length + DISPLAYLIST_CLL;
	/* Shrink tails can fragment later carve admissions. Opt-in only until
	 * a construction-safe allocator is proven; OFF never splices a tail. */
	// LADDER + COMPACTION (2026-09-08, lane/dl-ladder-pools): shrinking to the
	// RUNG instead of to the byte was tried here, on the theory that a compacted
	// block should be interchangeable with a fresh one of the same rung, and it
	// is NOT what shipped -- the project's own libogc-heap churn fixture
	// (tests/support/libogc_heap/churn.h, the REAL heap driven by the REAL
	// emitter) refuted it. At the 1.5x rung spacing the fixture could no longer
	// admit its 81,920 B column tenant at all; densifying the ladder to a 1.26
	// step fixed that but still regressed the largest free block by more than
	// the fixture's 1 MB gate (e.g. step 1641: baseline 2,542,032 B vs rung
	// 1,269,640 B) -- retaining a rung per compacted list simply costs more
	// contiguity than the byte-exact tail release buys. Measurement over plan
	// (CLAUDE.md): compaction keeps its byte-exact semantics.
	//
	// The +41 s readiness cost of memory.displaylist_compact=1 on real Wii
	// 6a2c5673 (65 s -> 106 s) therefore stands as a DIAGNOSIS, not something
	// this change fixes: ~15 k finalizes each splice an arbitrary tail into a
	// first-fit free list whose allocate() walk is linear in the number of free
	// blocks and runs under the lock the mesher and server both need. The
	// conclusion is that the switch should stay OFF (its default) -- the ladder
	// removes the interleave compaction was trying to buy back, without paying
	// the splice.
	void* compact = displaylist_compact_enabled()
		? mem2_shrink(l->data, allocated, call_len) : NULL;
	if(compact) {
		l->data = compact;
		l->length = call_len;
	} else {
		l->length = allocated; // shrink failure retains complete original list
	}

	memset(l->data, GX_NOP, DISPLAYLIST_CLL);
	memset((uint8_t*)l->data + l->index, GX_NOP, call_len - l->index);
	DCStoreRange(l->data, call_len);
	l->finished = true;
	storage_finalize(l);
	carve_note(displaylist_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(displaylist_carve_observe, CARVE_FINAL, l->data, 0,
		call_len, l->index);
	carve_note(displaylist_carve_observe, CARVE_END, 0, 0, 0, 0);
}

void displaylist_pos(struct displaylist* l, int16_t x, int16_t y, int16_t z) {
	assert(l && !l->finished);

	if(l->truncated) // mesh already dropped (failed grow): skip the re-grow churn
		return;

	if(!l->data) {
		l->data = dl_alloc_list(l, l->length + DISPLAYLIST_CLL);
		// OOM HARDENING: was assert(l->data) -- a no-op under -DNDEBUG, so an OOM here
		// (e.g. MEM2 exhausted) fell through to a NULL deref below and faulted the Wii.
		// Drop this mesh instead (l->data stays NULL; the emit calls for the rest of
		// this list all no-op via the same guard). The chunk simply isn't drawn this
		// pass and remeshes once memory frees -- the CLAUDE.md "no silent crash" rule.
		if(!l->data) {
			l->truncated = true; // header would over-declare vtxcnt -> drop the list
			return;
		}
	}

	// A vertex spans pos(6) + colour(1, +3 tint when tinted) + texcoord(2).
	int vtx_bytes = l->tinted ? 12 : 9;
	if(l->index + vtx_bytes > l->length) {
		size_t old_total = l->length + DISPLAYLIST_CLL; // before length grows
		// Grow 1.5x (was 1.25x). mem2_realloc always allocates+copies+frees (the
		// lwp_heap has no in-place grow), so each grow memcpys the whole buffer;
		// 1.5x roughly halves both the grow count and the total bytes copied per
		// chunk vs 1.25x (geometric series), for ~1.6% more transient peak. The
		// finalized list still reads l->index bytes, so output is unchanged.
		// LADDER: grow to the smallest BACKBONE rung >= the raw request
		// (displaylist_ladder.h). The backbone IS this recurrence, so a grow
		// off a backbone rung costs zero extra bytes; the densified midpoints
		// exist for init/presize snapping only, and a list that leaves one
		// climbs the backbone from there and never returns to them -- which is
		// what keeps a growing list's capacity one of 21 values instead of 42,
		// so a freed grow block exactly refits the next grow of that rung.
		size_t new_length = displaylist_rung_grow(
			(l->length * 3 / 2 + vtx_bytes + DISPLAYLIST_CLL - 1)
			/ DISPLAYLIST_CLL * DISPLAYLIST_CLL);
		// Commit length+data ONLY on success. mem2_realloc returns NULL WITHOUT freeing
		// the old buffer; the old `l->data = mem2_realloc(...)` LOST that pointer (leak)
		// then deref'd NULL. Keep the valid old buffer + old length and stop growing.
		void* grown = dl_grow_list(l, old_total, new_length + DISPLAYLIST_CLL);
		if(!grown) {
			l->truncated = true; // header would over-declare vtxcnt -> drop the list
			return;
		}
		l->length = new_length;
		l->data = grown;
		g_displaylist_grow_count++;
	}

	MEM_U16(l->data, l->index) = x;
	l->index += 2;
	MEM_U16(l->data, l->index) = y;
	l->index += 2;
	MEM_U16(l->data, l->index) = z;
	l->index += 2;
}

void displaylist_color(struct displaylist* l, uint8_t index) {
	assert(l && !l->finished);
	// No-op on a dropped mesh: !data (initial alloc failed) OR truncated (a
	// later GROW failed in displaylist_pos). The truncated half is load-bearing:
	// pos stops advancing the index on a failed grow, but this writer kept
	// advancing + writing -- marching past length AND the cache-line slack into
	// the NEIGHBOURING mem2 block = a stray heap writer under MEM2 pressure
	// (the lwp_heap corruption class).
	if(!l->data || l->truncated)
		return;
	MEM_U8(l->data, l->index++) = index;
	// GX attribute order is POS, CLR0 (this index), CLR1 (the tint), TEX0 — so
	// the tint bytes go here, right after the light index, when this list opted
	// into the tint channel.
	if(l->tinted) {
		MEM_U8(l->data, l->index++) = l->tint[0];
		MEM_U8(l->data, l->index++) = l->tint[1];
		MEM_U8(l->data, l->index++) = l->tint[2];
	}
}

void displaylist_texcoord(struct displaylist* l, uint8_t s, uint8_t t) {
	assert(l && !l->finished);
	if(!l->data || l->truncated) // dropped mesh (see displaylist_color): full no-op
		return;
	MEM_U8(l->data, l->index++) = s;
	MEM_U8(l->data, l->index++) = t;
}

void displaylist_vertex(struct displaylist* l, int16_t x, int16_t y, int16_t z,
						uint8_t light, uint8_t s, uint8_t t) {
	// Byte-identical to pos(x,y,z) + color(light) + texcoord(s,t), but the
	// null-check, the whole-vertex capacity reserve (identical to the one in
	// displaylist_pos), the data/index/tinted loads and the index advance are
	// done ONCE instead of being re-derived across three calls. This is the
	// mesher emit hot path; the per-vertex bookkeeping was the residual cost.
	assert(l && !l->finished);

	if(l->truncated) // mesh already dropped (failed grow): skip the re-grow churn
		return;

	if(!l->data) {
		l->data = dl_alloc_list(l, l->length + DISPLAYLIST_CLL);
		if(!l->data) { // OOM HARDENING (see displaylist_pos): drop this mesh, no NULL deref
			l->truncated = true; // header would over-declare vtxcnt -> drop the list
			return;
		}
	}

	int vtx_bytes = l->tinted ? 12 : 9;
	if(l->index + vtx_bytes > l->length) {
		size_t old_total = l->length + DISPLAYLIST_CLL; // before length grows
		// LADDER: grow to the smallest BACKBONE rung >= the raw request
		// (displaylist_ladder.h). The backbone IS this recurrence, so a grow
		// off a backbone rung costs zero extra bytes; the densified midpoints
		// exist for init/presize snapping only, and a list that leaves one
		// climbs the backbone from there and never returns to them -- which is
		// what keeps a growing list's capacity one of 21 values instead of 42,
		// so a freed grow block exactly refits the next grow of that rung.
		size_t new_length = displaylist_rung_grow(
			(l->length * 3 / 2 + vtx_bytes + DISPLAYLIST_CLL - 1)
			/ DISPLAYLIST_CLL * DISPLAYLIST_CLL);
		void* grown = dl_grow_list(l, old_total, new_length + DISPLAYLIST_CLL);
		if(!grown) { // OOM: keep the valid old buffer + old length, stop growing this list
			l->truncated = true; // header would over-declare vtxcnt -> drop the list
			return;
		}
		l->length = new_length;
		l->data = grown;
		g_displaylist_grow_count++;
	}

	// `restrict`: the GX byte buffer (l->data, a separate mem2 block) never
	// overlaps the displaylist control struct, so these stores cannot clobber
	// l->tint / l->index -- the compiler keeps them in registers and writes
	// l->index back exactly once. Offsets mirror pos(0,2,4) + color(6 [,7,8,9])
	// + texcoord(last two).
	uint8_t* restrict p = (uint8_t*)l->data + l->index;
	MEM_U16(p, 0) = x;
	MEM_U16(p, 2) = y;
	MEM_U16(p, 4) = z;
	MEM_U8(p, 6) = light;
	if(l->tinted) {
		MEM_U8(p, 7) = l->tint[0];
		MEM_U8(p, 8) = l->tint[1];
		MEM_U8(p, 9) = l->tint[2];
		MEM_U8(p, 10) = s;
		MEM_U8(p, 11) = t;
		l->index += 12;
	} else {
		MEM_U8(p, 7) = s;
		MEM_U8(p, 8) = t;
		l->index += 9;
	}
}

// A chunk side / LOD list cannot legitimately approach this size (worst-case
// dense side ~hundreds of KB); a length or index beyond it means the struct is
// corrupt. The captured wedge's CALL size was ~4.3MB, far past any real list.
#define DISPLAYLIST_RENDER_MAX_BYTES (2u * 1024u * 1024u)

// CALLs refused because the list struct was corrupt (the GP-wedge CALL-site
// guard below). Surfaced as "dlcall=" in the watchdog hang report.
uint32_t g_displaylist_badcall = 0;

// Per-frame draw-call + triangle counters (2026-09-03 far-ring frame-drop
// root-cause doc's "instrument caveats": the existing heartbeat `dlcall` is
// g_displaylist_badcall, a CORRUPTION counter that reads 0 on every healthy
// run -- there was no actual GP-load gauge to correlate against the vsync/GP
// dips). Counted here (the only place an ACTUAL GX_CallDispList is issued for
// a chunk mesh; displaylist_render_immediate's few callers -- piston-moving
// blocks -- are not chunk draw traffic and are intentionally excluded) so
// main.c can reset once per frame and the Wii heartbeat can report the real
// per-frame GP submission volume. `vtxcnt` is read back from the header this
// same finalize() wrote (GX_QUADS: 4 vertices/quad, 2 triangles/quad).
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

	if(!l->finished)
		return;

	// CALL-SITE GUARD (2026-06-13 GP-wedge root cause): a real-Wii hang captured
	// the GP stuck on a GX_CallDispList -- FIFO@rd opcode 0x40 with a ~4.3MB size
	// operand. The CALL address+size below are derived from l->data/l->index; a
	// corrupt list struct (a stale/torn/garbage index, or a freed data ptr) feeds
	// GX_CallDispList garbage, the GP runs off the list into unrelated MEM2,
	// parses it as opcodes and WEDGES (cmdIdle=0, GX_WaitDrawDone never returns).
	// This is the CALL-side analog of displaylist_render_immediate's guard, which
	// the earlier dlbad guard MISSED: that validated the vertex stream INSIDE the
	// list (the finalize vtxcnt invariant), but the wedge is in the INVOCATION.
	// A valid finished list always has data != NULL, !truncated, and
	// DISPLAYLIST_CLL+3 <= index <= length <= the sane cap; anything else is
	// corruption -> skip the draw (the side drops for this frame and re-meshes)
	// rather than wedge the GP. Counted (dlcall=) so the corruption still gets
	// NAMED on hardware even though it's now non-fatal. The deeper root-cause fix
	// (what corrupts the struct) is tracked separately.
	if(!l->data || l->truncated || dl_in_scratch(l->data)
	   || l->index < DISPLAYLIST_CLL + 3
	   || l->index > l->length || l->length > DISPLAYLIST_RENDER_MAX_BYTES) {
		g_displaylist_badcall++;
		return;
	}

	uint8_t* submit = (uint8_t*)l->data
		+ (DISPLAYLIST_CLL - (uintptr_t)l->data % DISPLAYLIST_CLL);
	uint32_t submit_bytes = (l->index + (uintptr_t)l->data % DISPLAYLIST_CLL - 1)
		/ DISPLAYLIST_CLL * DISPLAYLIST_CLL;
	/* R5 item 2b: exactly the bytes GX is handed, when the self-test armed
	 * the scene hash (displaylist_scene_hash.h); one branch otherwise. */
	if(g_dl_scene_hash_armed)
		dl_scene_hash_list(submit, submit_bytes);
	dl_submit(l);
	GX_CallDispList(submit, submit_bytes);

	g_frame_dlcalls++;
	uint16_t vtxcnt = MEM_U16(l->data, DISPLAYLIST_CLL + 1);
	g_frame_tris += (uint32_t)(vtxcnt / 4) * 2; // GX_QUADS: 2 tris/quad
}

void displaylist_render_immediate(struct displaylist* l, uint16_t vtxcnt) {
	assert(l && l->data);

	// NDEBUG hardening (2026-06-12 audit): the assert above compiles out of the
	// shipping .dol, so an OOM-dropped list (data==NULL after a failed alloc, or
	// truncated==true after a failed grow) would stream NULL/stale bytes here --
	// vtxcnt is the CALLER's intended count, so the GP would be promised vertices
	// the buffer never received = the under-fed-command wedge class. Skipping the
	// draw entirely is GX-safe (no GX_Begin emitted) and visually just drops the
	// one cube for the frames the OOM persists.
	if(!l || !l->data || l->truncated)
		return;

	uint8_t* base = (uint8_t*)l->data + DISPLAYLIST_CLL + 3;

	if(l->tinted) {
		// A TINTED list is written at 12 bytes/vertex with the biome tint at
		// 7,8,9 and the texcoord pushed to 10,11 (see displaylist_vertex), and
		// its caller has enabled the CLR1 vertex descriptor via gfx_tint().
		//
		// This branch did not exist: every immediate draw emitted VTXFMT0 with
		// a 9-byte stride and NO CLR1 attribute. With CLR1 set to GX_DIRECT the
		// GP is promised a colour per vertex that the CPU never writes, so it
		// keeps consuming FIFO bytes past the quad -- a GX FIFO DESYNC, which
		// is precisely the "3D world collapses to horizontal streaks while the
		// immediate-mode GUI and hand keep drawing" failure photographed on
		// 2026-07-31. It also read s,t from offsets 7,8, which in a tinted
		// vertex are two thirds of the tint. Reachable in ordinary play: a
		// piston pushing a GRASS BLOCK or LEAVES (the only biome-tinted blocks)
		// is the sole tinted immediate list in the build.
		GX_Begin(GX_QUADS, GX_VTXFMT4, vtxcnt);
		for(uint16_t k = 0; k < vtxcnt; k++) {
			GX_Position3s16(MEM_U16(base, 0), MEM_U16(base, 2),
							MEM_U16(base, 4));
			GX_Color1x8(MEM_U8(base, 6));
			GX_Color3u8(MEM_U8(base, 7), MEM_U8(base, 8), MEM_U8(base, 9));
			GX_TexCoord2u8(MEM_U8(base, 10), MEM_U8(base, 11));
			base += 12;
		}
		GX_End();
		return;
	}

	GX_Begin(GX_QUADS, GX_VTXFMT0, vtxcnt);
	for(uint16_t k = 0; k < vtxcnt; k++) {
		GX_Position3s16(MEM_U16(base, 0), MEM_U16(base, 2), MEM_U16(base, 4));
		GX_Color1x8(MEM_U8(base, 6));
		GX_TexCoord2u8(MEM_U8(base, 7), MEM_U8(base, 8));
		base += 9;
	}
	GX_End();
}
