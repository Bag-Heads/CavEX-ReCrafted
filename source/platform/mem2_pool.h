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

#ifndef PLATFORM_MEM2_POOL_H
#define PLATFORM_MEM2_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
	FIXED-TENANT BITMAP-SLOT POOL, DEMAND-CARVED IN SLABS
	(2026-09-08, carve-arena interleave lane; round 3).

	WHY. Two tenants of the Wii carve have a FIXED object size and a lifetime
	that shares nothing with the display lists around them:

	  * far-column chunk block buffers -- 10,240 B (CHUNK_SIZE^3 * 5/2,
	    chunk.c:183, tagged CARVE_VOXEL and retagged CARVE_FAR by world.c:988);
	  * `struct column_view` index blocks -- 2,112 B (COLUMN_VIEW_INDEX_BYTES,
	    column_view.c:357).

	They are allocated from the SAME first-fit __lwp_heap as every display list,
	so each one lands in the middle of a run of list blocks and cuts it in half.
	Real Wii 6a2c5673 (split_anchor_6m, fv6, voxel) measured 178 far + 242 view
	blocks live at readiness -- 480 wedges driven into the arena the lists need
	contiguous. Serving them from pool slots takes those 480 block sizes out of
	the shared free list entirely.

	WHY SLABS, AND NOT ONE BOOT CARVE (the round-3 change). Round 2 reserved
	both pools WHOLE at boot: 2,351,104 + 681,984 = 3,033,088 B taken out of a
	virgin heap before the first list existed. Real Wii 703aef00
	(split_anchor_6m 42/256 fv6 voxel, compaction off) measured what that costs
	when the scene does not need it yet: at t30 the FAR pool held 0 of 224 slots
	-- 2.35 MB sitting idle -- while the shared heap was down to 94 KB free with
	a 928 B largest block, and the single-player floors fell with it (far_walk
	9.8 -> 7.1 MB, perf_stress 14.7 -> 12.0 MB). The reservation was paid in
	full at the moment of peak pressure and drawn on minutes later.

	So a pool now starts EMPTY and carves a SLAB of MEM2_POOL_SLAB_SLOTS slots
	the first time a tagged request finds no free slot, up to a fixed cap (the
	round-2 bound, expressed as slabs). The heap keeps every byte the scene has
	not asked for yet, the slabs land where the traffic actually is, and a
	tenant that never shows up costs nothing. What does NOT change: a slab is
	never returned to the heap. Handing a drained slab back would put a
	slab-sized hole into the middle of the list arena at exactly the moment the
	churn that emptied it is still running -- reintroducing the interleaving
	this module exists to remove -- so a pool is a high-water mark of the scene,
	not a cache. (One exception since colcap-1006 R3 item 4: a WHOLLY EMPTY
	column slab above the boot slabs, with hysteresis -- see SHRINK below.
	The far and view pools never shrink.)

	WHAT. The bitmap-slot shape column_view.c:123-158 already uses for its page
	pool: a uint32_t word bitmap, a rotating hint, __builtin_ctz to find a free
	bit, and index -> address arithmetic. A slab is exactly ONE bitmap word --
	32 slots -- so slab index and word index are the same number and no slot can
	straddle two slabs. The ONE deliberate difference from column_view is
	locking: its bitmap is lock-free (atomics + CAS) because it runs under a
	per-column stripe with no allocator lock to inherit, whereas this pool is
	driven from inside mem2_allocate / mem2_free, which ALREADY hold mem2_lock
	across every heap splice. Taking the same lock is what makes plain loads and
	stores correct here, and adding atomics on top would be cargo cult -- so
	this module documents its caller contract instead.

	CONTRACT: every entry point must be called with the owning allocator's lock
	held (mem2.c's mem2_lock). mem2_pool_grow additionally calls back into the
	allocator through the `carve` hook installed at init, so that hook must be
	safe to call with the lock already held (mem2.c's is: it is a bare
	__lwp_heap_allocate, the same call every mem2_allocate makes under the same
	lock). The PC suite drives it single-threaded, which satisfies the contract
	trivially.

	TENANT-KEYED, NOT SIZE-KEYED. A pool serves ONE named tenant, reached
	through its own allocator entry point (mem2.c's mem2_alloc_pooled). Keying
	on the request SIZE instead was tried and is wrong: 10,240 B is EVERY
	chunk's block buffer (chunk.c:183 -- world.c only RETAGS the far ones
	CARVE_FAR afterwards), so the ~968 near-chunk buffers of the initial load
	would drain every slot before the far ring asked for its first one, and the
	far ring would interleave into the shared heap exactly as before while the
	census showed the pool 100% used.

	OVERFLOW is not an error. A pool at its slab CAP that cannot serve a request
	falls through to the ordinary carve allocation; the caller reports that
	through mem2_pool_note_overflow so the census can show a pool that is
	undersized for the scene rather than silently reverting to the interleaved
	behaviour this exists to remove. A FAILED CARVE counts the same way -- a
	heap with no room for a slab is exactly the case where the request must
	still be served, from whatever the heap does have. Only a TAGGED request can
	overflow -- an untagged allocation that merely happens to be the pool's
	object size was never a candidate for a slot.

	NO LIVE-OVERFLOW COUNTER. An earlier draft carried `overflow_live` ("how
	many overflowed blocks are still outstanding"). It cannot be maintained
	honestly: the release path recognises a pooled pointer by RANGE
	(mem2_free/mem2_free_sized), which is what lets every existing free site
	stay unchanged, and a heap block has nothing on it that names the tenant
	that asked for it. Retiring the counter would have meant either a size-keyed
	free (the same bug in the mirror) or a tenant flag threaded through every
	owner. `overflow` (cumulative) and `peak` answer the question the census
	actually asks -- is this bound too small for the scene, and by how much --
	so the unmaintainable counter is gone rather than reported wrong.
*/

/* One slab = one bitmap word. Slab index IS word index, so a slot never
 * straddles two slabs and the take path needs no division. */
#define MEM2_POOL_SLAB_SLOTS 32u
/* Cap on the cap: the shipped pools need 7 (far, 224 slots) and 9 (view, 288).
 * 16 keeps both plus headroom for a future bound at 64 B of bitmap and 128 B of
 * pointers per pool. */
#define MEM2_POOL_MAX_SLABS 16u

/* Carve one slab's raw bytes out of the owning allocator's heap. Called with
 * that allocator's lock already held (see CONTRACT above). NULL means the heap
 * could not spare the bytes, which is a normal, counted outcome. */
typedef void* (*mem2_pool_carve_fn)(size_t bytes, void* ctx);

struct mem2_pool {
	/*
		LEDGER IDENTITY (2026-09-08 fix round 2, per slab in round 3).
		`block[i]` is the raw allocation slab i took out of the carve -- the
		address the carve observer keys that slab's reservation record on
		(mem2.c's mem2_pool_note_slab). `base[i]` is the slab's FIRST SLOT,
		deliberately `lead` bytes above it.

		They MUST differ. The carve ledger is keyed by ADDRESS ONLY
		(carve_observer.c's find/insert/erase), so if slot 0 started at
		`block[i]` the very first take out of that slab would emit a
		CARVE_ALLOC on an address the ledger still holds live -- insert()
		counts that as an error and drops the slot record -- and the first
		RELEASE of that slot would emit a CARVE_FREE that erases the
		RESERVATION, silently deleting the slab's bytes from the accounted
		carve. Real Wii 703aef00 (split_anchor_6m, seed 42, sep 256, fv6,
		voxel) measured exactly that with the boot carve: err 1 at t10 (view
		pool's first take), err 2 at t40 (far pool's first take), then `over`
		+2,351,360 B at t70 and +682,240 B at t98 as each pool's slot 0 was
		first released, leaving a permanent unattributed floor of 3.03 MB --
		the pools' whole size.

		The lead costs NOTHING: it is the red-zone guard every block in this
		heap carries, moved from the tail to the front. Same bytes reserved,
		same payload noted, same one guard per slab that the observer's
		`used == payload + blocks*256 + overhead` reconciliation expects --
		and now each slab and its slots have disjoint addresses.
	*/
	uint8_t* block[MEM2_POOL_MAX_SLABS]; /* raw carve alloc; observer's key */
	uint8_t* base[MEM2_POOL_MAX_SLABS];  /* block + lead; slot 0 of the slab */
	uint32_t bits[MEM2_POOL_MAX_SLABS];  /* one word per slab, 32 slots each */
	mem2_pool_carve_fn carve;
	void* carve_ctx;
	/* Fast reject for mem2_free's ownership test: the address span of every
	 * carved slab. Slabs come from the shared heap and are therefore NOT
	 * contiguous, so a hit inside [lo, hi) still scans -- but the common case
	 * (an ordinary heap block below or above every slab) costs two compares. */
	const uint8_t* lo;
	const uint8_t* hi;
	size_t lead;   /* gap between a slab's own address and its slot 0 */
	size_t object; /* usable bytes handed to the caller */
	size_t stride; /* object + the allocator's red-zone guard, 32 B aligned */
	unsigned slabs_max; /* cap; 0 = pool permanently ABSENT */
	unsigned slabs;		/* carved and still held (mem2_pool_shrink lowers it) */
	unsigned used;
	unsigned peak;
	unsigned hint;	   /* rotating start word, as column_view.c's s_hint */
	uint32_t overflow; /* cumulative TAGGED requests the pool could not serve */
	uint32_t carve_fail; /* of those, the ones a FAILED slab carve caused */
	uint32_t shrinks;	 /* wholly empty slabs handed back (mem2_pool_shrink) */
};

/*
	The implementation is header-only (static inline) on purpose: mem2.c is
	listed individually in a dozen CMake test targets, so a new .c would have to
	be added to every one of them and a missed entry is a link error at the end
	of a long build. These are branch-free bookkeeping routines -- the same call
	mem2.h already makes for mem2_tag.

	mem2_pool_init  publish an EMPTY pool. Nothing is allocated here; the first
	                take that finds no free slot carves the first slab. `lead`
	                is the gap between a slab's own address and its slot 0 --
	                see LEDGER IDENTITY above; it must be a multiple of the slot
	                alignment (32 B here) and non-zero for any pool whose slabs
	                are themselves accounted by address. A NULL `carve`, a zero
	                `slabs_max` (or one above MEM2_POOL_MAX_SLABS), a zero
	                `object` or a `stride` below it leaves the pool permanently
	                ABSENT: every take returns NULL and the caller uses the
	                heap, which is the right behaviour when the carve cannot
	                spare the bytes.
	mem2_pool_grow  carve ONE more slab, or NULL at the cap / on a failed carve.
	                Returns the slab's own address, which is what the caller
	                notes to the carve observer -- never a slot address.
	mem2_pool_slab_bytes  the payload a single slab reserves (slots * stride).
	                The carve REQUEST is this plus `lead`; the lead is the
	                slab's red-zone guard, which the observer accounts as the
	                one guard every heap block carries.
	mem2_pool_bytes carved total (slabs * slab bytes), i.e. what the census
	                reports as pool_bytes. Zero for an empty or absent pool.
	mem2_pool_slabs / mem2_pool_slabs_max / mem2_pool_slots / mem2_pool_slots_max
	                slab and slot accounting for the census and the tests.
	mem2_pool_slab_block  slab i's own address, for replaying the reservations
	                into a freshly armed observer.
	mem2_pool_owns  true only for a slot BASE. An interior pointer is never
	                handed out, so one arriving here means a caller derived it --
	                freeing "its" slot would release a live neighbour. Refuse.
	mem2_pool_take  claim a slot from an ALREADY CARVED slab, or NULL when the
	                pool is absent or every carved slab is full. Growing is the
	                caller's decision (mem2_pool_grow) so that the new slab can
	                be attributed to the observer between the two calls.
	mem2_pool_release  release a slot; false (and no change) when `ptr` is not a
	                slot base of this pool, or when that slot is ALREADY FREE --
	                an idempotent double-free guard, because the shared heap's
	                answer to a double free is a corrupted free list and the pool
	                must not copy that.
	mem2_pool_note_overflow  record a TAGGED request this pool could not serve,
	                which the shared heap served instead. Cumulative; see NO
	                LIVE-OVERFLOW COUNTER above. `carve_failed` separates "the
	                cap is too small for this scene" from "the heap had no room
	                for another slab", which are different findings.
*/

#include <string.h>

static inline void mem2_pool_init(struct mem2_pool* p, size_t lead,
					size_t object, size_t stride, unsigned slabs_max,
					mem2_pool_carve_fn carve, void* carve_ctx) {
	memset(p, 0, sizeof(*p));
	if(!carve || !slabs_max || slabs_max > MEM2_POOL_MAX_SLABS || !object
	   || stride < object)
		return; /* absent pool: every take returns NULL, caller uses the heap */
	p->carve = carve;
	p->carve_ctx = carve_ctx;
	p->lead = lead;
	p->object = object;
	p->stride = stride;
	p->slabs_max = slabs_max;
}

static inline size_t mem2_pool_slab_bytes(const struct mem2_pool* p) {
	return p->stride * (size_t)MEM2_POOL_SLAB_SLOTS;
}

static inline size_t mem2_pool_bytes(const struct mem2_pool* p) {
	return mem2_pool_slab_bytes(p) * (size_t)p->slabs;
}

static inline unsigned mem2_pool_slabs(const struct mem2_pool* p) {
	return p->slabs;
}

static inline unsigned mem2_pool_slabs_max(const struct mem2_pool* p) {
	return p->slabs_max;
}

static inline unsigned mem2_pool_slots(const struct mem2_pool* p) {
	return p->slabs * MEM2_POOL_SLAB_SLOTS;
}

static inline unsigned mem2_pool_slots_max(const struct mem2_pool* p) {
	return p->slabs_max * MEM2_POOL_SLAB_SLOTS;
}

/* Slab i's own address: what the carve observer keys its reservation record on.
 * Disjoint from every slot by construction (LEDGER IDENTITY above). */
static inline const void* mem2_pool_slab_block(const struct mem2_pool* p,
											   unsigned i) {
	return i < p->slabs ? p->block[i] : NULL;
}

/* Carve one more slab. The pool is unchanged when the cap is reached or the
 * heap cannot spare the bytes -- both are normal outcomes that route the
 * request to the shared heap (counted by the caller as an overflow).
 *
 * COST OF A REFUSAL, stated rather than optimised away. At the cap this returns
 * without touching the heap at all, which is the steady state. Below the cap on
 * a heap with no room, every overflowing request pays one FAILED first-fit walk
 * before the caller pays the ordinary one for the object itself -- at most
 * double a walk that already happens on that path, in a state (heap exhausted,
 * pool not yet full) that is transient by definition. A retry back-off was
 * considered and rejected: it would need its own notion of "has the heap freed
 * anything since", which this module cannot see, and it would delay the carve
 * past the moment the heap recovers -- trading a bounded, visible cost for an
 * unbounded, invisible one. */
static inline const void* mem2_pool_grow(struct mem2_pool* p) {
	if(!p->carve || p->slabs >= p->slabs_max)
		return NULL;
	uint8_t* block
		= (uint8_t*)p->carve(p->lead + mem2_pool_slab_bytes(p), p->carve_ctx);
	if(!block)
		return NULL;
	unsigned i = p->slabs;
	p->block[i] = block;
	p->base[i] = block + p->lead;
	p->bits[i] = 0;
	const uint8_t* end = p->base[i] + p->stride * (size_t)MEM2_POOL_SLAB_SLOTS;
	if(!p->slabs || p->base[i] < p->lo)
		p->lo = p->base[i];
	if(!p->slabs || end > p->hi)
		p->hi = end;
	p->slabs = i + 1;
	/* A fresh slab is where the next takes should look first; without this the
	 * rotating hint would keep re-probing the full words that forced the grow. */
	p->hint = i;
	return block;
}

/* The slab index holding `ptr`, or -1. Linear over the carved slabs (at most
 * MEM2_POOL_MAX_SLABS pointer compares), behind a two-compare span reject. */
static inline int mem2_pool_slab_of(const struct mem2_pool* p, const void* ptr) {
	const uint8_t* q = (const uint8_t*)ptr;
	if(!p->slabs || !q || q < p->lo || q >= p->hi)
		return -1;
	for(unsigned i = 0; i < p->slabs; i++) {
		if(q < p->base[i])
			continue;
		size_t off = (size_t)(q - p->base[i]);
		if(off >= p->stride * (size_t)MEM2_POOL_SLAB_SLOTS)
			continue;
		/* Slot BASES only. An interior pointer is never handed out, so one
		 * arriving here means a caller derived it -- freeing "its" slot would
		 * release a neighbour that is still live. Refuse. */
		if(off % p->stride)
			return -1;
		return (int)i;
	}
	return -1;
}

/* Live slots in slab i (0..MEM2_POOL_SLAB_SLOTS); 0 for a slab not carved.
 * colcap-1006 (ASTRA-CC-R3 item 3): per-slab occupancy, so a run measures
 * which slabs are actually empty instead of inferring it from `used`. */
static inline unsigned mem2_pool_slab_used(const struct mem2_pool* p,
										   unsigned i) {
	return i < p->slabs ? (unsigned)__builtin_popcount(p->bits[i]) : 0u;
}

static inline bool mem2_pool_owns(const struct mem2_pool* p, const void* ptr) {
	return mem2_pool_slab_of(p, ptr) >= 0;
}

static inline void* mem2_pool_take(struct mem2_pool* p) {
	if(!p->slabs || p->used >= p->slabs * MEM2_POOL_SLAB_SLOTS)
		return NULL;
	/* Rotating start word (column_view.c's s_hint): consecutive takes spread
	 * across the bitmap instead of all contending on word 0. One word IS one
	 * slab, so this also keeps a take inside the slab it started in. */
	for(unsigned n = 0; n < p->slabs; n++) {
		unsigned w = (p->hint + n) % p->slabs;
		uint32_t used = p->bits[w];
		if(used == UINT32_MAX)
			continue;
		unsigned bit = (unsigned)__builtin_ctz(~used);
		p->bits[w] = used | (UINT32_C(1) << bit);
		p->hint = w;
		if(++p->used > p->peak)
			p->peak = p->used;
		return p->base[w] + p->stride * (size_t)bit;
	}
	return NULL;
}

/* colcap-1006 (ASTRA-CC-R4 points 2-3, T1 candidate): LOWEST-INDEX-FIRST
 * placement, used by the COLUMN pool only. Slab index order IS carve order
 * (boot slabs first; a hand-back only closes the gap), so a new column goes
 * into the oldest slab with room and the newest slabs fill last and drain
 * first -- the condition the empty-slab hand-back needs. Nothing live moves
 * and no slot is ever re-assigned: this only chooses WHICH free slot a new
 * column gets, which no world/light/mesh byte depends on (the slot's address
 * is never part of any output). Cost: the same <= slabs word scan as the
 * rotating take, starting at word 0 instead of the hint. */
static inline void* mem2_pool_take_lowest(struct mem2_pool* p) {
	if(!p->slabs || p->used >= p->slabs * MEM2_POOL_SLAB_SLOTS)
		return NULL;
	for(unsigned w = 0; w < p->slabs && w < MEM2_POOL_MAX_SLABS; w++) {
		uint32_t used = p->bits[w];
		if(used == UINT32_MAX)
			continue;
		unsigned bit = (unsigned)__builtin_ctz(~used);
		p->bits[w] = used | (UINT32_C(1) << bit);
		p->hint = w;
		if(++p->used > p->peak)
			p->peak = p->used;
		return p->base[w] + p->stride * (size_t)bit;
	}
	return NULL;
}

static inline bool mem2_pool_release(struct mem2_pool* p, void* ptr) {
	int s = mem2_pool_slab_of(p, ptr);
	if(s < 0)
		return false;
	unsigned bit = (unsigned)(((uint8_t*)ptr - p->base[s])
							  / (ptrdiff_t)p->stride);
	uint32_t mask = UINT32_C(1) << bit;
	if(!(p->bits[s] & mask))
		return false; /* already free: idempotent, never double-releases a slot */
	p->bits[s] &= ~mask;
	p->used--;
	return true;
}

static inline void mem2_pool_note_overflow(struct mem2_pool* p,
										   bool carve_failed) {
	p->overflow++;
	if(carve_failed)
		p->carve_fail++;
}

/*
	SHRINK (colcap-1006, ASTRA-CC-R3 item 4, option A -- T1: memory layout
	only; no slot ever moves and no slot byte is read or written).

	WHY THE OLD "NEVER RETURNED" RULE IS RELAXED FOR ONE CASE. Real Wii cc_c1
	(split_anchor_6m, seed 12345, sep 256) carved slabs 5-8 for a 242-column
	LOAD transient and held all 8 (21 MB) for the whole session while steady
	demand was 162 columns; the mesh burst that followed had no carve left and
	spilled. The interleave argument above is about handing a slab back WHILE
	the churn that emptied it is still running; the hysteresis below keeps a
	half-slab of free slots in the pool after the return, so a slab goes back
	only once demand has fallen well below the remaining capacity, and the
	boot slabs (carved from the virgin heap, the only contiguous 2.6 MB that is
	guaranteed) are never returned.

	mem2_pool_shrink_candidate  the index of a WHOLLY EMPTY slab at index >=
	                `keep` (the boot slabs), chosen highest-index first (the
	                most recently carved), provided that after removing it the
	                pool still has at least `headroom` free slots -- else -1.
	mem2_pool_shrink detach slab `s` from the pool and return its raw block
	                (the address mem2_pool_grow returned, the observer's key)
	                for the caller to hand back to the heap; NULL, and no
	                change, when s < keep, s is not carved, or s has a live
	                slot. The remaining slabs keep their base/block/bits --
	                only their INDEX shifts down by one -- so every live slot
	                keeps its address, its bytes and its ownership. The span
	                (lo/hi) is recomputed from the remaining slabs, the hint
	                follows its slab, `used`/`peak`/`overflow`/`carve_fail`
	                are untouched (an empty slab holds no used slot), and the
	                next exhaustion carves a fresh slab exactly as before.
	CALLER CONTRACT, as for every entry point: the owning allocator's lock is
	held. A slot pointer into a returned slab is no longer owned by the pool,
	so a (buggy) late double free of such a slot now reaches the shared heap
	instead of being refused here -- the same as a double free of any other
	heap block.
*/
static inline int mem2_pool_shrink_candidate(const struct mem2_pool* p,
											 unsigned keep, unsigned headroom) {
	if(p->slabs <= keep)
		return -1;
	const unsigned cap_after = (p->slabs - 1u) * MEM2_POOL_SLAB_SLOTS;
	if(p->used > cap_after || cap_after - p->used < headroom)
		return -1;
	for(unsigned i = p->slabs; i-- > keep;)
		if(!p->bits[i])
			return (int)i;
	return -1;
}

static inline void* mem2_pool_shrink(struct mem2_pool* p, unsigned s,
									 unsigned keep) {
	if(s < keep || s >= p->slabs || s >= MEM2_POOL_MAX_SLABS || p->bits[s])
		return NULL;
	uint8_t* block = p->block[s];
	/* slabs <= MEM2_POOL_MAX_SLABS by construction; the second bound only
	 * makes that visible to the compiler's array-bounds analysis. */
	for(unsigned i = s; i + 1 < p->slabs && i + 1 < MEM2_POOL_MAX_SLABS; i++) {
		p->block[i] = p->block[i + 1];
		p->base[i] = p->base[i + 1];
		p->bits[i] = p->bits[i + 1];
	}
	p->slabs--;
	p->block[p->slabs] = NULL;
	p->base[p->slabs] = NULL;
	p->bits[p->slabs] = 0;
	p->lo = p->hi = NULL;
	const size_t span = p->stride * (size_t)MEM2_POOL_SLAB_SLOTS;
	for(unsigned i = 0; i < p->slabs; i++) {
		if(!p->lo || p->base[i] < p->lo)
			p->lo = p->base[i];
		if(!p->hi || p->base[i] + span > p->hi)
			p->hi = p->base[i] + span;
	}
	if(p->hint > s)
		p->hint--;
	else if(p->hint == s)
		p->hint = 0;
	if(p->hint >= p->slabs)
		p->hint = 0;
	p->shrinks++;
	return block;
}

#endif
