/*
	Fixed-tenant bitmap-slot pools, demand-carved in slabs
	(2026-09-08, carve-arena interleave lane; round 3).

	Covers the module's accounting contract -- demand carve, per-slab growth,
	the slab cap, exhaustion, overflow (cap-driven and carve-failure-driven),
	take/release across slabs, the double-free guard, the interior-pointer
	refusal, the census totals -- and PINS the two shipped bounds against the
	defines they were derived from, so a later change to MAX_VIEW_DISTANCE /
	MAX_CHUNKS / COLUMN_VIEW_INDEX_BYTES cannot silently leave the pools sized
	for the old world. That pin lives here rather than in platform/mem2.h
	because network/server_local_state.h states it is only for TUs that embed
	struct server_local; a test may include it.
*/

#include "../source/platform/mem2_pool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The two shipped pools' numbers, without dragging in the Wii allocator. */
#define MEM2_GUARD 256u
#define MEM2_POOL_FAR_OBJECT 10240u
#define MEM2_POOL_FAR_SLOTS 224u
#define MEM2_POOL_VIEW_OBJECT 2112u
#define MEM2_POOL_VIEW_SLOTS 288u
#define FAR_STRIDE (MEM2_POOL_FAR_OBJECT + MEM2_GUARD)
#define VIEW_STRIDE (MEM2_POOL_VIEW_OBJECT + MEM2_GUARD)
/* mem2.c's MEM2_POOL_LEAD: each slab's red-zone guard, carried at the FRONT so
 * the slab's own address is never also a slot address (mem2_pool.h, LEDGER
 * IDENTITY). Every carve below is sized lead + SLAB_SLOTS * stride. */
#define POOL_LEAD MEM2_GUARD
#define FAR_SLABS (MEM2_POOL_FAR_SLOTS / MEM2_POOL_SLAB_SLOTS)
#define VIEW_SLABS (MEM2_POOL_VIEW_SLOTS / MEM2_POOL_SLAB_SLOTS)

/* ------------------------------------------------------------------ */
/* The derivations, pinned                                            */
/* ------------------------------------------------------------------ */

#include "../source/column_view.h"
#include "../source/network/server_local_state.h"

/* Object sizes are the tenants' real sizes, not chosen numbers. */
_Static_assert(MEM2_POOL_VIEW_OBJECT == COLUMN_VIEW_INDEX_BYTES,
			   "view pool object must be struct column_view");
_Static_assert(MEM2_POOL_FAR_OBJECT == 16u * 16u * 16u * 5u / 2u,
			   "far pool object must be CHUNK_SIZE^3 * 5/2 (chunk.c:183)");

/* The FORMULAS come from server_local_state.h and are pinned against it here;
 * the Wii NUMBERS they are evaluated at cannot be, because a test TU compiles
 * with PLATFORM_PC and therefore sees the PC arm of that header's
 * #ifdef PLATFORM_PC split (MAX_VIEW_DISTANCE 10, not 5). So: pin the shape
 * against the real header, and carry the Wii view distance as a named literal
 * with its citation. A change to either arm trips one of these. */
_Static_assert(MAX_CHUNKS
				   == (MAX_VIEW_DISTANCE * 2 + 2) * (MAX_VIEW_DISTANCE * 2 + 2),
			   "MAX_CHUNKS formula moved (server_local_state.h:211)");
_Static_assert(MAX_VIEW_DISTANCE == 10,
			   "PC view distance moved; re-check the Wii arm too");
#define CW_WII_VIEW_DISTANCE 5 /* server_local_state.h:96, the #else arm */
#define CW_WII_MAX_CHUNKS ((CW_WII_VIEW_DISTANCE * 2 + 2) * (CW_WII_VIEW_DISTANCE * 2 + 2))

/* FAR bound. The far annulus at the shipped +1 ring (MAX_VIEW_DISTANCE + 1 = 6
 * on Wii) is 48 columns; each column's voxel storage spans its surface band
 * (cy_lo..cy_hi in world.c's far install), whose GEOMETRIC worst case is all
 * COLUMN_HEIGHT = 8 Y-slices -- 384 slots, 4,030,464 B, deliberately not
 * carved. The bound is measurement-driven instead: 178 live on real Wii
 * (6a2c5673 split_anchor_6m, fv6, voxel), +25% headroom, rounded up to a whole
 * slab. Overflow past it is counted and served by the shared heap, so being
 * wrong here is bounded and visible. */
#define FAR_RING (CW_WII_VIEW_DISTANCE + 1)
#define FAR_ANNULUS_COLUMNS ((FAR_RING * 2 + 1) * (FAR_RING * 2 + 1) - (CW_WII_VIEW_DISTANCE * 2 + 1) * (CW_WII_VIEW_DISTANCE * 2 + 1))
#define FAR_MEASURED_LIVE 178u
#define VIEW_MEASURED_LIVE 242u
#define FAR_WORST_CASE_SLOTS (FAR_ANNULUS_COLUMNS * 8) /* chunk.h:61-68 */
_Static_assert(FAR_ANNULUS_COLUMNS == 48, "far annulus at the +1 ring");
_Static_assert(FAR_WORST_CASE_SLOTS == 384, "far geometric worst case");
_Static_assert(MEM2_POOL_FAR_SLOTS >= FAR_MEASURED_LIVE * 5u / 4u,
			   "far pool must keep >=25% headroom over the measured live count");
_Static_assert(MEM2_POOL_FAR_SLOTS < FAR_WORST_CASE_SLOTS,
			   "carving the geometric worst case is the trade this bound "
			   "deliberately refuses -- if that changes, restate the budget");
/* VIEW bound: 2 * MAX_CHUNKS -- a view outlives its column while a retirement
 * is pending, so one live view per resident column is not enough. */
_Static_assert(CW_WII_MAX_CHUNKS == 144, "Wii resident column bound");
_Static_assert(MEM2_POOL_VIEW_SLOTS == 2 * CW_WII_MAX_CHUNKS,
			   "view pool slots must follow MAX_CHUNKS");

/* SLAB SHAPE (round 3). Both caps must be a whole number of slabs, because the
 * carve unit IS a slab: one bitmap word, 32 slots, so slab index == word index
 * and no slot straddles two slabs. 224 = 7 * 32 and 288 = 9 * 32. */
_Static_assert(MEM2_POOL_SLAB_SLOTS == 32u, "a slab is one 32-bit bitmap word");
_Static_assert(MEM2_POOL_FAR_SLOTS % MEM2_POOL_SLAB_SLOTS == 0
				   && MEM2_POOL_VIEW_SLOTS % MEM2_POOL_SLAB_SLOTS == 0,
			   "slot caps must be a whole number of slabs");
_Static_assert(FAR_SLABS == 7 && VIEW_SLABS == 9, "shipped slab caps");
_Static_assert(FAR_SLABS <= MEM2_POOL_MAX_SLABS
				   && VIEW_SLABS <= MEM2_POOL_MAX_SLABS,
			   "shipped caps must fit the pool's slab arrays");

/* The budget the design note states, recomputed from the defines, and the
 * 3 MiB pool ceiling this lane was given. It is now a CEILING reached only by a
 * scene that fills it, not a boot reservation. */
#define FAR_SLAB_BYTES (MEM2_POOL_SLAB_SLOTS * FAR_STRIDE)
#define VIEW_SLAB_BYTES (MEM2_POOL_SLAB_SLOTS * VIEW_STRIDE)
_Static_assert(FAR_SLAB_BYTES == 335872u, "far slab payload");
_Static_assert(VIEW_SLAB_BYTES == 75776u, "view slab payload");
_Static_assert((size_t)FAR_SLABS * FAR_SLAB_BYTES == 2351104u, "far cap bytes");
_Static_assert((size_t)VIEW_SLABS * VIEW_SLAB_BYTES == 681984u, "view cap bytes");
_Static_assert((size_t)FAR_SLABS * FAR_SLAB_BYTES
				   + (size_t)VIEW_SLABS * VIEW_SLAB_BYTES
			   <= 3u * 1024u * 1024u,
			   "total pool ceiling must stay inside the 3 MiB budget");
/* Both strides stay GX aligned so the red-zone canary works on a pooled block. */
_Static_assert(FAR_STRIDE % 32u == 0 && VIEW_STRIDE % 32u == 0, "stride align");

/* ------------------------------------------------------------------ */
/* Heap fixture: a bump allocator standing in for __lwp_heap_allocate  */
/* ------------------------------------------------------------------ */

/* mem2.c's carve hook is a bare __lwp_heap_allocate under mem2_lock. Here it is
 * a bump allocator over a static arena with a settable BUDGET, so a slab carve
 * can be made to fail the way a nearly full heap makes it fail -- which is a
 * counted, served outcome, not an error (mem2_pool.h, OVERFLOW is not an
 * error). `carves` counts the calls, which is how the tests below prove a pool
 * carves ON DEMAND and not one byte sooner. */
struct fixture_heap {
	uint8_t* arena;
	size_t size, next, budget;
	unsigned carves, refusals;
};
static void* fixture_carve(size_t bytes, void* ctx) {
	struct fixture_heap* h = (struct fixture_heap*)ctx;
	h->carves++;
	size_t base = (h->next + 31u) & ~(size_t)31u;
	if(base + bytes > h->size || base + bytes > h->budget) {
		h->refusals++;
		return NULL;
	}
	h->next = base + bytes;
	return h->arena + base;
}
static void fixture_reset(struct fixture_heap* h, uint8_t* arena, size_t size) {
	h->arena = arena;
	h->size = size;
	h->next = 0;
	h->budget = size;
	h->carves = h->refusals = 0;
}

/* Enough for the whole far ceiling plus its per-slab leads and 32 B rounding. */
static uint8_t far_arena[FAR_SLABS * (POOL_LEAD + FAR_SLAB_BYTES) + 256];
static uint8_t view_arena[VIEW_SLABS * (POOL_LEAD + VIEW_SLAB_BYTES) + 256];
static uint8_t small_arena[8 * (POOL_LEAD + MEM2_POOL_SLAB_SLOTS * 96) + 256];

/* ------------------------------------------------------------------ */

static void test_absent_pool(void) {
	struct fixture_heap h;
	struct mem2_pool p = {0};
	fixture_reset(&h, small_arena, sizeof(small_arena));

	/* No carve hook: permanently absent, and every entry point is a safe no-op
	 * so the caller just uses the shared heap (today's behaviour). */
	mem2_pool_init(&p, POOL_LEAD, 64, 96, 4, NULL, &h);
	assert(p.slabs_max == 0);
	assert(mem2_pool_bytes(&p) == 0 && mem2_pool_slots_max(&p) == 0);
	assert(mem2_pool_grow(&p) == NULL);
	assert(mem2_pool_take(&p) == NULL);
	assert(!mem2_pool_owns(&p, small_arena));
	assert(!mem2_pool_release(&p, small_arena));
	assert(h.carves == 0);

	/* A stride that cannot hold the object is refused rather than handing out
	 * slots that overlap their neighbour. */
	mem2_pool_init(&p, POOL_LEAD, 128, 64, 4, fixture_carve, &h);
	assert(p.slabs_max == 0 && mem2_pool_grow(&p) == NULL);
	/* Zero slabs, and more slabs than the struct has room for, likewise. */
	mem2_pool_init(&p, POOL_LEAD, 64, 96, 0, fixture_carve, &h);
	assert(p.slabs_max == 0);
	mem2_pool_init(&p, POOL_LEAD, 64, 96, MEM2_POOL_MAX_SLABS + 1,
				   fixture_carve, &h);
	assert(p.slabs_max == 0);
	assert(h.carves == 0);
}

/* THE ROUND-3 GATE: nothing is carved until a slot is actually wanted, and then
 * exactly one slab is. Round 2 took all 3,033,088 B at boot; real Wii 703aef00
 * measured the far pool still at 0/224 slots at t30 while the shared heap was
 * down to 94 KB free with a 928 B largest block. */
static void test_demand_carve(void) {
	enum { SLABS = 4 };
	struct fixture_heap h;
	struct mem2_pool p = {0};
	fixture_reset(&h, small_arena, sizeof(small_arena));
	mem2_pool_init(&p, POOL_LEAD, 64, 96, SLABS, fixture_carve, &h);

	/* Empty at init: no carve, no bytes, no slots -- and take() alone never
	 * grows, because the caller has to attribute a new slab to the observer
	 * between the two calls (mem2.c's mem2_allocate_pooled). */
	assert(mem2_pool_slabs(&p) == 0 && mem2_pool_slots(&p) == 0);
	assert(mem2_pool_bytes(&p) == 0);
	assert(mem2_pool_slots_max(&p) == SLABS * MEM2_POOL_SLAB_SLOTS);
	assert(mem2_pool_take(&p) == NULL);
	assert(h.carves == 0);

	/* One slab at a time, each exactly lead + 32 * stride from the heap. */
	size_t before = h.next;
	const void* slab0 = mem2_pool_grow(&p);
	assert(slab0 && h.carves == 1);
	assert(h.next - before == POOL_LEAD + MEM2_POOL_SLAB_SLOTS * 96);
	assert(mem2_pool_slabs(&p) == 1);
	assert(mem2_pool_bytes(&p) == MEM2_POOL_SLAB_SLOTS * 96);
	assert(mem2_pool_slab_block(&p, 0) == slab0);
	assert(mem2_pool_slab_block(&p, 1) == NULL); /* not carved yet */

	/* That slab serves exactly 32 slots, then take() refuses again. */
	void* got[SLABS * MEM2_POOL_SLAB_SLOTS];
	for(unsigned i = 0; i < MEM2_POOL_SLAB_SLOTS; i++) {
		got[i] = mem2_pool_take(&p);
		assert(got[i] && mem2_pool_owns(&p, got[i]));
	}
	assert(p.used == MEM2_POOL_SLAB_SLOTS && mem2_pool_take(&p) == NULL);
	assert(h.carves == 1); /* still one: growth is the caller's decision */

	/* Fill to the cap, one slab per exhaustion. */
	for(unsigned s = 1; s < SLABS; s++) {
		assert(mem2_pool_grow(&p) != NULL);
		assert(mem2_pool_slabs(&p) == s + 1);
		for(unsigned i = 0; i < MEM2_POOL_SLAB_SLOTS; i++) {
			unsigned n = s * MEM2_POOL_SLAB_SLOTS + i;
			got[n] = mem2_pool_take(&p);
			assert(got[n] && mem2_pool_owns(&p, got[n]));
		}
	}
	assert(h.carves == SLABS);
	assert(p.used == SLABS * MEM2_POOL_SLAB_SLOTS);
	assert(p.peak == p.used);
	assert(mem2_pool_bytes(&p) == (size_t)SLABS * MEM2_POOL_SLAB_SLOTS * 96);

	/* THE CAP. Past it the pool refuses to grow and never touches the heap
	 * again, however many times it is asked -- the request is served by the
	 * shared heap and counted as an overflow by the caller. */
	for(unsigned i = 0; i < 5; i++) {
		assert(mem2_pool_take(&p) == NULL);
		assert(mem2_pool_grow(&p) == NULL);
		mem2_pool_note_overflow(&p, mem2_pool_slabs(&p) < mem2_pool_slabs_max(&p));
	}
	assert(h.carves == SLABS); /* the cap is checked before the heap is asked */
	assert(p.overflow == 5 && p.carve_fail == 0);
	assert(mem2_pool_slabs(&p) == SLABS);

	/* Every slot is distinct, slot-base aligned inside its own slab, and
	 * releasing gives the capacity back without ever returning a slab. */
	for(unsigned i = 0; i < SLABS * MEM2_POOL_SLAB_SLOTS; i++)
		for(unsigned j = i + 1; j < SLABS * MEM2_POOL_SLAB_SLOTS; j++)
			assert(got[i] != got[j]);
	for(unsigned i = 0; i < SLABS * MEM2_POOL_SLAB_SLOTS; i++)
		assert(mem2_pool_release(&p, got[i]));
	assert(p.used == 0);
	assert(p.peak == SLABS * MEM2_POOL_SLAB_SLOTS); /* high-water never falls */
	assert(mem2_pool_slabs(&p) == SLABS); /* a slab is never returned */
	assert(mem2_pool_bytes(&p) == (size_t)SLABS * MEM2_POOL_SLAB_SLOTS * 96);
	/* And the drained slabs are reusable: no new carve is needed. */
	for(unsigned i = 0; i < SLABS * MEM2_POOL_SLAB_SLOTS; i++)
		assert(mem2_pool_take(&p) != NULL);
	assert(h.carves == SLABS && p.used == SLABS * MEM2_POOL_SLAB_SLOTS);
	printf("[pool] demand carve: %u slabs of %u slots bought on exhaustion, "
		   "cap holds, %u heap carves total\n",
		   mem2_pool_slabs(&p), MEM2_POOL_SLAB_SLOTS, h.carves);
}

/* A heap with no room for another slab is NOT the same finding as a pool at its
 * cap, so it is counted separately (carve_fail) -- and it is still served, by
 * the shared heap, exactly as an over-cap request is. */
static void test_carve_failure(void) {
	struct fixture_heap h;
	struct mem2_pool p = {0};
	fixture_reset(&h, small_arena, sizeof(small_arena));
	mem2_pool_init(&p, POOL_LEAD, 64, 96, 4, fixture_carve, &h);
	assert(mem2_pool_grow(&p) != NULL);
	/* Squeeze the heap: the next slab does not fit. */
	h.budget = h.next + 16;
	assert(mem2_pool_grow(&p) == NULL);
	assert(h.refusals == 1);
	assert(mem2_pool_slabs(&p) == 1); /* the pool is unchanged by a refusal */
	for(unsigned i = 0; i < MEM2_POOL_SLAB_SLOTS; i++)
		assert(mem2_pool_take(&p));
	assert(mem2_pool_take(&p) == NULL);
	mem2_pool_note_overflow(&p, mem2_pool_slabs(&p) < mem2_pool_slabs_max(&p));
	assert(p.overflow == 1 && p.carve_fail == 1);
	/* When the heap frees up the pool grows again -- the refusal was not
	 * sticky, which matters because the pressure that caused it is transient. */
	h.budget = h.size;
	assert(mem2_pool_grow(&p) != NULL);
	assert(mem2_pool_take(&p) != NULL);
	printf("[pool] slab carve refused under heap pressure: counted as "
		   "overflow+cfail, served, recovers when the heap does\n");
}

static void test_bad_release(void) {
	struct fixture_heap h;
	struct mem2_pool p = {0};
	fixture_reset(&h, small_arena, sizeof(small_arena));
	mem2_pool_init(&p, POOL_LEAD, 64, 96, 2, fixture_carve, &h);
	assert(mem2_pool_grow(&p));
	void* s = mem2_pool_take(&p);
	assert(s && p.used == 1);

	/* Double release is idempotent and changes nothing -- the shared heap's
	 * answer to a double free is a corrupted free list; the pool must not
	 * reproduce that (mem2.c's own idempotent-teardown discipline). */
	assert(mem2_pool_release(&p, s));
	assert(p.used == 0);
	assert(!mem2_pool_release(&p, s));
	assert(p.used == 0);

	/* Foreign and interior pointers are refused, not misattributed. */
	int stack_object = 0;
	assert(!mem2_pool_owns(&p, &stack_object));
	assert(!mem2_pool_release(&p, &stack_object));
	assert(!mem2_pool_owns(&p, NULL));
	assert(!mem2_pool_release(&p, NULL));
	uint8_t* base0 = p.base[0];
	assert(!mem2_pool_owns(&p, base0 + 1));	 /* interior of slot 0 */
	assert(!mem2_pool_owns(&p, base0 + 64)); /* inside slot 0's guard */
	assert(!mem2_pool_release(&p, base0 + 8));
	/* One past this slab's end is not this slab's -- and with a second slab
	 * carved it must not be misread as that slab's slot 0 either. */
	assert(!mem2_pool_owns(&p, base0 + MEM2_POOL_SLAB_SLOTS * 96));
	assert(mem2_pool_grow(&p));
	assert(!mem2_pool_owns(&p, base0 + MEM2_POOL_SLAB_SLOTS * 96));
	assert(mem2_pool_owns(&p, p.base[1]));

	/* LEDGER IDENTITY (mem2_pool.h): a slab's own address is NOT a slot. If it
	 * were, the carve observer -- which is keyed by address alone -- would see
	 * the first take collide with that slab's reservation record and the first
	 * release ERASE it, losing the slab from the census (measured on real Wii
	 * 703aef00 with the boot carve; see the header). */
	for(unsigned i = 0; i < mem2_pool_slabs(&p); i++) {
		const void* block = mem2_pool_slab_block(&p, i);
		assert(block && block != (const void*)p.base[i]);
		assert(!mem2_pool_owns(&p, block));
		assert(!mem2_pool_release(&p, (void*)block));
	}
	assert(p.used == 0);
}

static void test_census_totals(void) {
	/* The two shipped pools, at their shipped bounds. pool_bytes is what the
	 * census reports -- the CARVED total, which for a scene at the measured
	 * real-Wii live counts is strictly less than the ceiling. */
	struct fixture_heap fh, vh;
	struct mem2_pool f, v;
	fixture_reset(&fh, far_arena, sizeof(far_arena));
	fixture_reset(&vh, view_arena, sizeof(view_arena));
	mem2_pool_init(&f, POOL_LEAD, MEM2_POOL_FAR_OBJECT, FAR_STRIDE, FAR_SLABS,
				   fixture_carve, &fh);
	mem2_pool_init(&v, POOL_LEAD, MEM2_POOL_VIEW_OBJECT, VIEW_STRIDE,
				   VIEW_SLABS, fixture_carve, &vh);
	assert(mem2_pool_bytes(&f) == 0 && mem2_pool_bytes(&v) == 0);

	/* Drive them the way mem2_allocate_pooled does: take, and grow only when a
	 * take found nothing. */
	unsigned far_over = 0, view_over = 0;
	for(unsigned i = 0; i < FAR_MEASURED_LIVE; i++) {
		if(!mem2_pool_take(&f)) {
			if(mem2_pool_grow(&f) == NULL || !mem2_pool_take(&f))
				far_over++;
		}
	}
	for(unsigned i = 0; i < VIEW_MEASURED_LIVE; i++) {
		if(!mem2_pool_take(&v)) {
			if(mem2_pool_grow(&v) == NULL || !mem2_pool_take(&v))
				view_over++;
		}
	}
	assert(far_over == 0 && view_over == 0);
	assert(f.used == FAR_MEASURED_LIVE && v.used == VIEW_MEASURED_LIVE);
	/* 178 slots is 6 slabs of 32 (the 6th only 18 full); 242 is 8. */
	assert(mem2_pool_slabs(&f) == 6 && mem2_pool_slabs(&v) == 8);
	assert(mem2_pool_bytes(&f) == 6u * FAR_SLAB_BYTES);
	assert(mem2_pool_bytes(&v) == 8u * VIEW_SLAB_BYTES);
	printf("[pool] measured real-Wii live counts (far %u, view %u) carve "
		   "%u+%u slabs = %zu B, %zu B under the %u B ceiling\n",
		   f.used, v.used, mem2_pool_slabs(&f), mem2_pool_slabs(&v),
		   mem2_pool_bytes(&f) + mem2_pool_bytes(&v),
		   (size_t)FAR_SLABS * FAR_SLAB_BYTES
			   + (size_t)VIEW_SLABS * VIEW_SLAB_BYTES
			   - (mem2_pool_bytes(&f) + mem2_pool_bytes(&v)),
		   (unsigned)((size_t)FAR_SLABS * FAR_SLAB_BYTES
					  + (size_t)VIEW_SLABS * VIEW_SLAB_BYTES));

	/* Fill to the ceiling: the bound admits every slot it advertises -- each
	 * distinct and inside its slab -- then refuses cleanly, and the carved
	 * total is exactly the round-2 boot reservation. */
	while(f.used < MEM2_POOL_FAR_SLOTS) {
		void* q = mem2_pool_take(&f);
		if(!q) {
			assert(mem2_pool_grow(&f));
			q = mem2_pool_take(&f);
		}
		assert(q && mem2_pool_owns(&f, q));
	}
	assert(mem2_pool_take(&f) == NULL && mem2_pool_grow(&f) == NULL);
	while(v.used < MEM2_POOL_VIEW_SLOTS) {
		void* q = mem2_pool_take(&v);
		if(!q) {
			assert(mem2_pool_grow(&v));
			q = mem2_pool_take(&v);
		}
		assert(q && mem2_pool_owns(&v, q));
	}
	assert(mem2_pool_take(&v) == NULL && mem2_pool_grow(&v) == NULL);
	assert(mem2_pool_slabs(&f) == FAR_SLABS && mem2_pool_slabs(&v) == VIEW_SLABS);
	assert(mem2_pool_bytes(&f) == 2351104u);
	assert(mem2_pool_bytes(&v) == 681984u);
	assert(mem2_pool_bytes(&f) + mem2_pool_bytes(&v) == 3033088u);
	assert(f.overflow == 0 && v.overflow == 0);
	printf("[pool] both pools fill to their ceiling and refuse cleanly: far "
		   "%u/%u in %u slabs, view %u/%u in %u slabs, %zu B carved\n",
		   f.used, mem2_pool_slots_max(&f), mem2_pool_slabs(&f), v.used,
		   mem2_pool_slots_max(&v), mem2_pool_slabs(&v),
		   mem2_pool_bytes(&f) + mem2_pool_bytes(&v));
}

/* colcap-1006 (ASTRA-CC-R3 item 3): per-slab occupancy is the popcount of
 * each slab's word, sums to `used`, and reads 0 for an uncarved index. */
static void test_slab_occupancy(void) {
	enum { SLABS = 4 };
	struct fixture_heap h;
	struct mem2_pool p = {0};
	fixture_reset(&h, small_arena, sizeof(small_arena));
	mem2_pool_init(&p, POOL_LEAD, 64, 96, SLABS, fixture_carve, &h);
	assert(mem2_pool_slab_used(&p, 0) == 0);
	void* got[3 * MEM2_POOL_SLAB_SLOTS];
	unsigned n = 0;
	for(unsigned s = 0; s < 3; s++) {
		assert(mem2_pool_grow(&p));
		for(unsigned i = 0; i < MEM2_POOL_SLAB_SLOTS; i++)
			got[n++] = mem2_pool_take(&p);
	}
	for(unsigned s = 0; s < 3; s++)
		assert(mem2_pool_slab_used(&p, s) == MEM2_POOL_SLAB_SLOTS);
	/* Drain slab 1 completely and half of slab 2. */
	for(unsigned i = 0; i < MEM2_POOL_SLAB_SLOTS; i++)
		assert(mem2_pool_release(&p, got[MEM2_POOL_SLAB_SLOTS + i]));
	for(unsigned i = 0; i < MEM2_POOL_SLAB_SLOTS; i += 2)
		assert(mem2_pool_release(&p, got[2 * MEM2_POOL_SLAB_SLOTS + i]));
	assert(mem2_pool_slab_used(&p, 0) == 32 && mem2_pool_slab_used(&p, 1) == 0
		   && mem2_pool_slab_used(&p, 2) == 16 && mem2_pool_slab_used(&p, 3) == 0
		   && mem2_pool_slab_used(&p, MEM2_POOL_MAX_SLABS - 1) == 0);
	unsigned sum = 0;
	for(unsigned s = 0; s < mem2_pool_slabs(&p); s++)
		sum += mem2_pool_slab_used(&p, s);
	assert(sum == p.used && p.used == 48);
	printf("[pool] slab occupancy: [32,0,16] sums to used=%u\n", p.used);
}

/* ------------------------------------------------------------------ */
/* SHRINK (colcap-1006, ASTRA-CC-R3 item 4, option A)                 */
/* ------------------------------------------------------------------ */

/* A heap that can take a slab BACK: SH_CHUNKS fixed chunks, first-free
 * carve, a free that can be told to refuse (libogc's __lwp_heap_free
 * returning false on a block it does not recognise). `live` counts the
 * chunks currently carved -- the bytes the pool holds out of the heap. */
#define SH_OBJ 64u
#define SH_STRIDE 96u
#define SH_CHUNK (POOL_LEAD + MEM2_POOL_SLAB_SLOTS * SH_STRIDE)
#define SH_CHUNKS 12u
#define SH_KEEP 4u	   /* MEM2_POOL_COLUMN_BOOT_SLABS */
#define SH_CAP 9u	   /* MEM2_POOL_COLUMN_SLABS */
#define SH_HEADROOM 16u /* MEM2_POOL_COLUMN_SHRINK_HEADROOM */
struct shrink_heap {
	uint8_t* arena;
	bool used[SH_CHUNKS];
	unsigned carves, frees, refused, live;
	bool refuse_free;
};
static void* shrink_carve(size_t bytes, void* ctx) {
	struct shrink_heap* h = (struct shrink_heap*)ctx;
	assert(bytes == SH_CHUNK);
	h->carves++;
	for(unsigned i = 0; i < SH_CHUNKS; i++)
		if(!h->used[i]) {
			h->used[i] = true;
			h->live++;
			return h->arena + (size_t)i * SH_CHUNK;
		}
	return NULL;
}
static bool shrink_heap_free(struct shrink_heap* h, void* block) {
	size_t off = (size_t)((uint8_t*)block - h->arena);
	assert(off % SH_CHUNK == 0 && off / SH_CHUNK < SH_CHUNKS);
	if(h->refuse_free) {
		h->refused++;
		return false;
	}
	assert(h->used[off / SH_CHUNK]); /* never a double hand-back */
	h->used[off / SH_CHUNK] = false;
	h->live--;
	h->frees++;
	return true;
}
static uint8_t shrink_arena[SH_CHUNKS * SH_CHUNK + 64];
static uint8_t shrink_arena_b[SH_CHUNKS * SH_CHUNK + 64];

static void shrink_heap_init(struct shrink_heap* h, uint8_t* arena) {
	memset(h, 0, sizeof *h);
	h->arena = (uint8_t*)(((uintptr_t)arena + 31u) & ~(uintptr_t)31u);
}

/* mem2.c's two pool paths, transcribed: allocate = take, else grow + take;
 * free = release, then (EVERY column release) at most one hysteretic
 * candidate is handed back -- heap free FIRST, detach only once the heap
 * accepted it. Returns the index handed back, or -1. */
static void* sh_alloc(struct mem2_pool* p) {
	void* s = mem2_pool_take(p);
	if(!s && mem2_pool_grow(p))
		s = mem2_pool_take(p);
	return s;
}
static int sh_free(struct mem2_pool* p, struct shrink_heap* h, void* ptr,
				   bool shrink) {
	assert(mem2_pool_release(p, ptr));
	if(!shrink)
		return -1;
	int c = mem2_pool_shrink_candidate(p, SH_KEEP, SH_HEADROOM);
	if(c < 0)
		return -1;
	void* block = (void*)mem2_pool_slab_block(p, (unsigned)c);
	if(!shrink_heap_free(h, block))
		return -1; /* fallback: the slab stays, still usable */
	assert(mem2_pool_shrink(p, (unsigned)c, SH_KEEP) == block);
	return c;
}
/* Release every live slot whose slab currently has base `base` (slab indices
 * shift on a hand-back; the base address does not). */
static unsigned sh_drain(struct mem2_pool* p, struct shrink_heap* h,
						 void** slot, unsigned n, const uint8_t* base,
						 unsigned limit, int* returned) {
	unsigned done = 0;
	for(unsigned i = 0; i < n && done < limit; i++) {
		if(!slot[i])
			continue;
		int sl = mem2_pool_slab_of(p, slot[i]);
		assert(sl >= 0);
		if(p->base[sl] != base)
			continue;
		int r = sh_free(p, h, slot[i], true);
		if(r >= 0 && returned)
			*returned = r;
		slot[i] = NULL;
		done++;
	}
	return done;
}

/* The tenant: every live object carries a payload derived from its id alone,
 * the way a column slot carries that column's arrays. */
static void sh_fill(void* slot, uint32_t id) {
	uint32_t x = id * 2654435761u + 1u;
	for(unsigned i = 0; i < SH_OBJ; i++) {
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		((uint8_t*)slot)[i] = (uint8_t)x;
	}
}
static bool sh_check(const void* slot, uint32_t id) {
	uint8_t want[SH_OBJ];
	sh_fill(want, id);
	return memcmp(slot, want, SH_OBJ) == 0;
}
static void sh_check_invariants(const struct mem2_pool* p) {
	unsigned sum = 0;
	for(unsigned i = 0; i < p->slabs; i++) {
		sum += mem2_pool_slab_used(p, i);
		assert(p->block[i] && p->base[i] == p->block[i] + POOL_LEAD);
		assert(p->lo <= p->base[i]
			   && p->base[i] + SH_STRIDE * MEM2_POOL_SLAB_SLOTS <= p->hi);
		for(unsigned j = i + 1; j < p->slabs; j++)
			assert(p->block[i] != p->block[j]);
	}
	for(unsigned i = p->slabs; i < MEM2_POOL_MAX_SLABS; i++)
		assert(!p->block[i] && !p->base[i] && !p->bits[i]);
	assert(sum == p->used && p->hint < (p->slabs ? p->slabs : 1u));
}

/* grow -> drain -> shrink -> regrow, with the boot slabs kept, hysteresis,
 * the heap bytes actually handed back, a retired slab's slots no longer
 * owned, and every live slot's address and bytes unchanged throughout. */
static void test_shrink_grow_drain_regrow(void) {
	struct shrink_heap h;
	struct mem2_pool p = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&p, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	for(unsigned k = 0; k < SH_KEEP; k++)
		assert(mem2_pool_grow(&p)); /* the eager boot slabs */
	enum { N = 8 * MEM2_POOL_SLAB_SLOTS };
	void* slot[N];
	void* addr[N];
	for(unsigned i = 0; i < N; i++) {
		slot[i] = addr[i] = sh_alloc(&p);
		assert(slot[i]);
		sh_fill(slot[i], i);
	}
	assert(mem2_pool_slabs(&p) == 8 && h.live == 8 && p.peak == N);
	const uint8_t* base0 = p.base[0];
	const uint8_t* base6 = p.base[6];
	const uint8_t* base7 = p.base[7];
	void* block7 = p.block[7];
	/* The load transient drains: slab 7 first. Empty, but used 224 leaves no
	 * headroom after a hand-back (224 - 224 < 16): kept. */
	int ret = -1;
	assert(sh_drain(&p, &h, slot, N, base7, 32, &ret) == 32);
	assert(ret < 0 && mem2_pool_slabs(&p) == 8 && p.bits[7] == 0);
	/* Draining slab 6: once used <= 208 the empty slab 7 goes back (the
	 * 16th release); slab 6 itself empties at used 192 and is kept. */
	assert(sh_drain(&p, &h, slot, N, base6, 32, &ret) == 32);
	assert(ret == 7 && mem2_pool_slabs(&p) == 7 && h.frees == 1 && h.live == 7);
	assert(!h.used[((uint8_t*)block7 - h.arena) / SH_CHUNK]); /* bytes back */
	assert(p.base[6] == base6 && p.bits[6] == 0 && p.used == 192);
	assert(mem2_pool_shrink_candidate(&p, SH_KEEP, SH_HEADROOM) < 0);
	/* 16 more from boot slab 0: at used 176 the second empty slab goes.
	 * 8 -> 6, measured here, not projected. */
	ret = -1;
	assert(sh_drain(&p, &h, slot, N, base0, 16, &ret) == 16);
	assert(ret == 6 && mem2_pool_slabs(&p) == 6 && h.live == 6 && p.shrinks == 2);
	assert(mem2_pool_bytes(&p) == 6u * MEM2_POOL_SLAB_SLOTS * SH_STRIDE);
	sh_check_invariants(&p);
	/* Retired slots are no longer the pool's. */
	assert(!mem2_pool_owns(&p, base6) && !mem2_pool_owns(&p, base7));
	/* The boot slabs are never candidates, even when one is wholly empty. */
	assert(sh_drain(&p, &h, slot, N, base0, 32, &ret) == 16);
	assert(p.bits[0] == 0 && mem2_pool_slabs(&p) == 6 && p.used == 160);
	assert(mem2_pool_shrink(&p, 0, SH_KEEP) == NULL);
	/* Live storage never moved, never changed. */
	for(unsigned i = 0; i < N; i++)
		if(slot[i])
			assert(slot[i] == addr[i] && mem2_pool_owns(&p, slot[i])
				   && sh_check(slot[i], i));
	/* Regrow after churn: 96 more -> 32 fill the free slots, 64 need two
	 * fresh slabs, carved from the chunks the heap got back. */
	unsigned carves_before = h.carves;
	void* extra[3 * MEM2_POOL_SLAB_SLOTS];
	for(unsigned i = 0; i < 3 * MEM2_POOL_SLAB_SLOTS; i++) {
		extra[i] = sh_alloc(&p);
		assert(extra[i]);
		sh_fill(extra[i], 100000u + i);
	}
	assert(mem2_pool_slabs(&p) == 8 && h.carves == carves_before + 2);
	assert(h.live == 8 && p.used == 256);
	sh_check_invariants(&p);
	for(unsigned i = 0; i < N; i++)
		if(slot[i]) assert(slot[i] == addr[i] && sh_check(slot[i], i));
	for(unsigned i = 0; i < 3 * MEM2_POOL_SLAB_SLOTS; i++)
		assert(sh_check(extra[i], 100000u + i));
	printf("[pool] shrink: 4 boot + 4 grown -> drain -> 8 to 6 slabs "
		   "(hysteresis held each until headroom), empty boot slab kept, "
		   "regrow to 8 from the returned chunks; live slots unmoved\n");
}

/* Interleaved occupancy: one live slot pins its slab; index shifts keep every
 * live slot owned, releasable and mapped to the right (shifted) slab. */
static void test_shrink_interleaved_occupancy(void) {
	struct shrink_heap h;
	struct mem2_pool p = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&p, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	enum { N = 9 * MEM2_POOL_SLAB_SLOTS };
	void* slot[N];
	for(unsigned i = 0; i < N; i++) {
		slot[i] = sh_alloc(&p);
		assert(slot[i]);
		sh_fill(slot[i], i);
	}
	assert(mem2_pool_slabs(&p) == 9);
	/* Release everything except ONE slot in slabs 5 and 7 and 8 (and the
	 * boot slabs untouched), interleaving across slabs. */
	for(unsigned j = 0; j < MEM2_POOL_SLAB_SLOTS; j++)
		for(unsigned s = 4; s < 9; s++) {
			unsigned i = s * 32 + j;
			bool pin = (s == 5 && j == 31) || (s == 7 && j == 0) || (s == 8 && j == 17);
			if(!pin) {
				(void)sh_free(&p, &h, slot[i], true);
				slot[i] = NULL;
			}
		}
	/* Slabs 4 and 6 emptied (wholly) and went back; 5, 7, 8 are pinned. */
	assert(mem2_pool_slabs(&p) == 7 && p.shrinks == 2 && h.live == 7);
	assert(mem2_pool_slab_used(&p, 4) == 1 && mem2_pool_slab_used(&p, 5) == 1
		   && mem2_pool_slab_used(&p, 6) == 1);
	sh_check_invariants(&p);
	const unsigned pins[3] = {5 * 32 + 31, 7 * 32 + 0, 8 * 32 + 17};
	for(unsigned k = 0; k < 3; k++) {
		assert(mem2_pool_slab_of(&p, slot[pins[k]]) == (int)(4 + k));
		assert(sh_check(slot[pins[k]], pins[k]));
	}
	/* Releasing the pins empties each slab in turn; hysteresis decides:
	 * the first two go back (62 and 31 free slots left), the last would
	 * leave 128 - 128 = 0 and is kept. */
	for(unsigned k = 0; k < 3; k++) {
		(void)sh_free(&p, &h, slot[pins[k]], true);
		slot[pins[k]] = NULL;
		sh_check_invariants(&p);
	}
	assert(mem2_pool_slabs(&p) == SH_KEEP + 1 && p.used == 4 * 32
		   && h.live == SH_KEEP + 1 && p.bits[SH_KEEP] == 0);
	for(unsigned i = 0; i < 4 * 32; i++)
		assert(slot[i] && sh_check(slot[i], i) && mem2_pool_owns(&p, slot[i]));
	printf("[pool] shrink: interleaved occupancy -- pinned slabs kept, emptied "
		   "ones returned, shifted indices still own their slots\n");
}

/* Guards: boot slabs, uncarved index, live slot, absent pool, failed heap
 * free (fallback keeps the slab), retired slots unowned, hint repaired. */
static void test_shrink_guards(void) {
	struct mem2_pool absent = {0};
	mem2_pool_init(&absent, POOL_LEAD, SH_OBJ, SH_STRIDE, 0, shrink_carve, NULL);
	assert(mem2_pool_shrink_candidate(&absent, SH_KEEP, SH_HEADROOM) < 0);
	assert(mem2_pool_shrink(&absent, 0, 0) == NULL);

	struct shrink_heap h;
	struct mem2_pool p = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&p, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	for(unsigned k = 0; k < 6; k++)
		assert(mem2_pool_grow(&p));
	/* Every slab empty: the boot slabs are never candidates. */
	assert(mem2_pool_shrink(&p, 3, SH_KEEP) == NULL);
	assert(mem2_pool_shrink(&p, 6, SH_KEEP) == NULL);	/* not carved */
	assert(mem2_pool_shrink(&p, 99, SH_KEEP) == NULL);
	/* A fresh slab is where takes look first (mem2_pool_grow sets the
	 * hint), so one take lands in slab 5: a live slot pins it. */
	void* live = mem2_pool_take(&p);
	assert(live && mem2_pool_slab_of(&p, live) == 5);
	assert(mem2_pool_shrink(&p, 5, SH_KEEP) == NULL); /* live slot */
	assert(mem2_pool_slabs(&p) == 6 && p.shrinks == 0);

	/* Failed heap free: nothing is detached, the slab still serves. */
	struct mem2_pool q = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&q, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	void* s[6 * 32];
	for(unsigned i = 0; i < 6 * 32; i++)
		s[i] = sh_alloc(&q);
	h.refuse_free = true;
	for(unsigned i = 4 * 32; i < 6 * 32; i++)
		(void)sh_free(&q, &h, s[i], true);
	assert(h.refused >= 1 && mem2_pool_slabs(&q) == 6 && q.shrinks == 0);
	assert(mem2_pool_take(&q) != NULL); /* the kept slab is still usable */
	h.refuse_free = false;

	/* Retired slab: its slot addresses are no longer owned or releasable,
	 * and the hint that pointed at it is repaired. */
	struct mem2_pool r = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&r, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	for(unsigned k = 0; k < 6; k++)
		assert(mem2_pool_grow(&r));
	r.hint = 5;
	uint8_t* retired_slot = r.base[5];
	uint8_t* retired_block = (uint8_t*)r.block[5];
	assert(mem2_pool_shrink(&r, 5, SH_KEEP) == retired_block);
	assert(r.hint < r.slabs && !mem2_pool_owns(&r, retired_slot));
	assert(!mem2_pool_release(&r, retired_slot));
	r.hint = 4;
	uint8_t* moved = r.base[4];
	assert(mem2_pool_grow(&r) && mem2_pool_slabs(&r) == 6);
	r.hint = 5;
	assert(mem2_pool_shrink(&r, 4, SH_KEEP)); /* hint above the hole shifts */
	assert(r.hint == 4 && r.base[3] && r.base[4] != moved);
	sh_check_invariants(&r);
	printf("[pool] shrink guards: boot/uncarved/live refused, refused heap "
		   "free keeps the slab, retired slots unowned, hint repaired\n");
}

/* Storage-level byte-exact equivalence: the same tenant script against a
 * pool that shrinks and one that never does. Every live object's bytes are
 * identical between the two at every checkpoint, and in the shrinking pool
 * no live object ever moves (its address is checked at each one). */
struct sh_pair {
	struct shrink_heap ha, hb;
	struct mem2_pool a, b; /* a shrinks, b never does */
	void* pa[300];
	void* pb[300];
	void* addr_a[300];
	unsigned live, shrinks, gen;
};
static void sh_pair_alloc(struct sh_pair* t, unsigned id) {
	if(t->pa[id])
		return;
	t->pa[id] = sh_alloc(&t->a);
	t->pb[id] = sh_alloc(&t->b);
	assert(t->pa[id] && t->pb[id]);
	t->gen++;
	sh_fill(t->pa[id], id * 7919u + t->gen);
	sh_fill(t->pb[id], id * 7919u + t->gen);
	t->addr_a[id] = t->pa[id];
	t->live++;
}
static void sh_pair_free(struct sh_pair* t, unsigned id) {
	if(!t->pa[id])
		return;
	assert(t->pa[id] == t->addr_a[id]); /* never moved */
	assert(memcmp(t->pa[id], t->pb[id], SH_OBJ) == 0);
	unsigned before = t->a.shrinks;
	(void)sh_free(&t->a, &t->ha, t->pa[id], true);
	(void)sh_free(&t->b, &t->hb, t->pb[id], false);
	t->shrinks += t->a.shrinks - before;
	t->pa[id] = t->pb[id] = NULL;
	t->live--;
}
static void sh_pair_verify(const struct sh_pair* t) {
	sh_check_invariants(&t->a);
	for(unsigned i = 0; i < 300; i++)
		if(t->pa[i]) {
			assert(t->pa[i] == t->addr_a[i]);
			assert(mem2_pool_owns(&t->a, t->pa[i]));
			assert(memcmp(t->pa[i], t->pb[i], SH_OBJ) == 0);
		}
	assert(t->a.used == t->b.used && t->a.used == t->live);
}
static struct sh_pair s_sh_pair;

/* Storage-level byte-exact equivalence, on the cc_c1 shape: a steady set of
 * 160 columns that churns, plus a 90-column transient (the load ring) that
 * arrives and is released in random order, six times. The same tenant script
 * runs against a pool that hands empty slabs back and one that never does:
 * every live object's bytes are identical between the two at every
 * checkpoint, and in the shrinking pool no live object ever moves. */
static void test_shrink_equivalence(void) {
	struct sh_pair* t = &s_sh_pair;
	memset(t, 0, sizeof *t);
	shrink_heap_init(&t->ha, shrink_arena);
	shrink_heap_init(&t->hb, shrink_arena_b);
	mem2_pool_init(&t->a, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &t->ha);
	mem2_pool_init(&t->b, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &t->hb);
	for(unsigned k = 0; k < SH_KEEP; k++) {
		assert(mem2_pool_grow(&t->a));
		assert(mem2_pool_grow(&t->b));
	}
	uint32_t rng = 12345u;
#define SH_RNG() (rng = rng * 1103515245u + 12345u, rng >> 8)
	for(unsigned id = 0; id < 160; id++)
		sh_pair_alloc(t, id);
	for(unsigned cycle = 0; cycle < 6; cycle++) {
		for(unsigned id = 160; id < 250; id++)
			sh_pair_alloc(t, id);
		sh_pair_verify(t);
		unsigned order[90];
		for(unsigned i = 0; i < 90; i++)
			order[i] = 160 + i;
		for(unsigned i = 89; i > 0; i--) {
			unsigned j = SH_RNG() % (i + 1);
			unsigned x = order[i]; order[i] = order[j]; order[j] = x;
		}
		for(unsigned i = 0; i < 90; i++) {
			sh_pair_free(t, order[i]);
			if(i % 4 == 0) { /* steady churn interleaved with the drain */
				unsigned id = SH_RNG() % 160;
				sh_pair_free(t, id);
				sh_pair_alloc(t, id);
			}
		}
		for(unsigned i = 0; i < 200; i++) {
			unsigned id = SH_RNG() % 160;
			sh_pair_free(t, id);
			sh_pair_alloc(t, id);
		}
		sh_pair_verify(t);
	}
#undef SH_RNG
	assert(t->shrinks > 0 && t->ha.frees == t->shrinks);
	assert(mem2_pool_slabs(&t->b) >= mem2_pool_slabs(&t->a));
	printf("[pool] shrink equivalence: 6 transient cycles over a churning "
		   "steady set, %u hand-backs, every live object byte-identical to "
		   "the never-shrinking pool and never moved (slabs %u vs %u)\n",
		   t->shrinks, mem2_pool_slabs(&t->a), mem2_pool_slabs(&t->b));
}

/* Concurrent access under the CONTRACT (the owning allocator's lock): four
 * threads take, write, verify and release slots while the free path hands
 * empty slabs back and the alloc path regrows. A slot handed to two threads
 * at once, a slot moved, or a counter out of step fails the run. */
#include <pthread.h>
static pthread_mutex_t s_sh_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mem2_pool s_sh_pool;
static struct shrink_heap s_sh_heap;
#define SH_THREADS 4
#define SH_PER_THREAD 60
static void* sh_worker(void* arg) {
	const uint32_t tid = (uint32_t)(uintptr_t)arg;
	void* mine[SH_PER_THREAD] = {0};
	uint32_t rng = 7u + tid * 7919u;
	for(unsigned step = 0; step < 40000; step++) {
		rng = rng * 1664525u + 1013904223u;
		unsigned k = (rng >> 10) % SH_PER_THREAD;
		if(!mine[k]) {
			pthread_mutex_lock(&s_sh_lock);
			void* s = sh_alloc(&s_sh_pool);
			pthread_mutex_unlock(&s_sh_lock);
			if(!s)
				continue; /* pool at its cap: the shared heap would serve */
			sh_fill(s, tid * 1000u + k); /* outside the lock: tenant-owned */
			mine[k] = s;
		} else if(((rng >> 3) & 1) || step > 38000) {
			assert(sh_check(mine[k], tid * 1000u + k)); /* nobody else wrote */
			pthread_mutex_lock(&s_sh_lock);
			(void)sh_free(&s_sh_pool, &s_sh_heap, mine[k], true);
			sh_check_invariants(&s_sh_pool);
			pthread_mutex_unlock(&s_sh_lock);
			mine[k] = NULL;
		}
	}
	for(unsigned k = 0; k < SH_PER_THREAD; k++)
		if(mine[k]) {
			assert(sh_check(mine[k], tid * 1000u + k));
			pthread_mutex_lock(&s_sh_lock);
			(void)sh_free(&s_sh_pool, &s_sh_heap, mine[k], true);
			pthread_mutex_unlock(&s_sh_lock);
		}
	return NULL;
}
static void test_shrink_concurrent(void) {
	shrink_heap_init(&s_sh_heap, shrink_arena);
	mem2_pool_init(&s_sh_pool, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP,
				   shrink_carve, &s_sh_heap);
	for(unsigned k = 0; k < SH_KEEP; k++)
		assert(mem2_pool_grow(&s_sh_pool));
	pthread_t t[SH_THREADS];
	for(unsigned i = 0; i < SH_THREADS; i++)
		assert(pthread_create(&t[i], NULL, sh_worker, (void*)(uintptr_t)i) == 0);
	for(unsigned i = 0; i < SH_THREADS; i++)
		assert(pthread_join(t[i], NULL) == 0);
	assert(s_sh_pool.used == 0);
	sh_check_invariants(&s_sh_pool);
	/* The heap holds exactly the chunks the pool still lists. */
	assert(s_sh_heap.live == mem2_pool_slabs(&s_sh_pool));
	assert(s_sh_heap.carves - s_sh_heap.frees == s_sh_heap.live);
	assert(s_sh_pool.shrinks == s_sh_heap.frees && s_sh_pool.shrinks > 0);
	printf("[pool] shrink concurrent: %d threads x 40000 ops under the lock, "
		   "%u grows / %u hand-backs, no double handout, drained to %u boot "
		   "slabs\n", SH_THREADS, s_sh_heap.carves - SH_KEEP, s_sh_pool.shrinks,
		   mem2_pool_slabs(&s_sh_pool));
}

/* ------------------------------------------------------------------ */
/* EVENT QUEUE ORDER (colcap-1006, R4 point 9; Sol review R3 Major 2) */
/* ------------------------------------------------------------------ */
#include "../source/platform/mem2_pool_evq.h"

static pthread_mutex_t s_evq_lock = PTHREAD_MUTEX_INITIALIZER;
static void evq_lock(void) { pthread_mutex_lock(&s_evq_lock); }
static void evq_unlock(void) { pthread_mutex_unlock(&s_evq_lock); }
static struct mem2_pool_evq s_evq;
/* The "wire": every delivered seq in delivery order. The emit callback can
 * be told to refuse (a full telemetry ring), which must defer, not drop. */
#define EVQ_WIRE_MAX 200000
static uint32_t s_wire[EVQ_WIRE_MAX];
static uint32_t s_wire_n;
static uint32_t s_refuse_every; /* refuse every Nth offer (0 = never) */
static uint32_t s_offers;
static pthread_mutex_t s_wire_lock = PTHREAD_MUTEX_INITIALIZER;
static bool evq_emit(const struct mem2_pool_ev* ev, void* user) {
	(void)user;
	pthread_mutex_lock(&s_wire_lock);
	bool refuse = s_refuse_every && (++s_offers % s_refuse_every) == 0;
	if(!refuse) {
		assert(s_wire_n < EVQ_WIRE_MAX);
		s_wire[s_wire_n++] = ev->kind == MEM2_POOL_EV_GROW ? ev->grow.seq
														   : ev->shrink.seq;
	}
	pthread_mutex_unlock(&s_wire_lock);
	return !refuse;
}
#define EVQ_THREADS 4
#define EVQ_PER_THREAD 20000
static void* evq_worker(void* arg) {
	uint32_t rng = 99u + (uint32_t)(uintptr_t)arg * 31u;
	for(unsigned i = 0; i < EVQ_PER_THREAD; i++) {
		rng = rng * 1664525u + 1013904223u;
		struct mem2_pool_ev ev = {.kind = (rng >> 9) & 1
			? MEM2_POOL_EV_GROW : MEM2_POOL_EV_SHRINK};
		evq_lock();
		/* A full queue is legal here (refusals back it up); retry until it
		 * takes the event so the test can demand a gap-free wire. */
		while(!mem2_pool_evq_push_locked(&s_evq, &ev)) {
			s_evq.lost--; /* the test re-offers; not a loss */
			evq_unlock();
			(void)mem2_pool_evq_publish(&s_evq, evq_lock, evq_unlock, evq_emit, NULL, 4u);
			evq_lock();
		}
		evq_unlock();
		/* Publish OUTSIDE the lock, from every thread, racing, budgeted. */
		(void)mem2_pool_evq_publish(&s_evq, evq_lock, evq_unlock, evq_emit, NULL, 4u);
	}
	return NULL;
}
static void test_event_queue_order(void) {
	memset(&s_evq, 0, sizeof s_evq);
	s_wire_n = 0;
	s_offers = 0;
	s_refuse_every = 7; /* a ring that refuses every 7th offer */
	pthread_t t[EVQ_THREADS];
	for(unsigned i = 0; i < EVQ_THREADS; i++)
		assert(pthread_create(&t[i], NULL, evq_worker, (void*)(uintptr_t)i) == 0);
	for(unsigned i = 0; i < EVQ_THREADS; i++)
		assert(pthread_join(t[i], NULL) == 0);
	s_refuse_every = 0;
	while(mem2_pool_evq_pending(&s_evq))
		assert(mem2_pool_evq_publish(&s_evq, evq_lock, evq_unlock, evq_emit,
									 NULL, 4u) <= 4u);
	/* Every event delivered exactly once, strictly in seq order. */
	assert(s_wire_n == EVQ_THREADS * EVQ_PER_THREAD && s_evq.lost == 0);
	for(uint32_t i = 0; i < s_wire_n; i++)
		assert(s_wire[i] == i + 1);
	/* Sol review R4: one call does at most `budget` deliveries. */
	{
		uint32_t before = s_wire_n;
		for(unsigned i = 0; i < 5; i++) {
			struct mem2_pool_ev ev = {.kind = MEM2_POOL_EV_GROW};
			evq_lock();
			assert(mem2_pool_evq_push_locked(&s_evq, &ev));
			evq_unlock();
		}
		assert(mem2_pool_evq_publish(&s_evq, evq_lock, evq_unlock, evq_emit, NULL, 2u) == 2u);
		assert(s_wire_n == before + 2 && mem2_pool_evq_pending(&s_evq));
		assert(mem2_pool_evq_publish(&s_evq, evq_lock, evq_unlock, evq_emit, NULL, 8u) == 3u);
		assert(!mem2_pool_evq_pending(&s_evq));
		for(uint32_t i = 0; i < s_wire_n; i++)
			assert(s_wire[i] == i + 1);
	}
	/* Overflow is counted, never silent, and numbers nothing. */
	struct mem2_pool_evq q;
	memset(&q, 0, sizeof q);
	for(unsigned i = 0; i < MEM2_POOL_EVQ_SLOTS + 3; i++) {
		struct mem2_pool_ev ev = {.kind = MEM2_POOL_EV_GROW};
		(void)mem2_pool_evq_push_locked(&q, &ev);
	}
	assert(q.lost == 3 && q.seq == MEM2_POOL_EVQ_SLOTS
		   && q.head - q.tail == MEM2_POOL_EVQ_SLOTS);
	printf("[pool] event queue: %d threads x %d events published outside the "
		   "lock with every 7th offer refused -> %u delivered, gap-free, in "
		   "seq order; overflow counted (lost=3)\n", EVQ_THREADS,
		   EVQ_PER_THREAD, s_wire_n);
}

/* ------------------------------------------------------------------ */
/* LOWEST-INDEX-FIRST PLACEMENT (colcap-1006, R4 points 2-3)          */
/* ------------------------------------------------------------------ */

/* The cc_m1 shape (real Wii: 242-column load transient, then ~162 steady):
 * a steady set of 160 columns that keeps churning and a 90-column transient
 * that arrives and leaves in random order, six times -- run against the
 * ROTATING placement (the unchanged allocator) and the LOWEST placement, with
 * the same hand-back rule, plus a third pool that never hands back. Every
 * live object's bytes are compared across all three at every checkpoint, and
 * no live object ever moves. */
struct sh_tri {
	struct shrink_heap h[3];
	struct mem2_pool p[3]; /* 0 rotating+shrink, 1 lowest+shrink, 2 rotating, never */
	void* slot[3][300];
	void* addr[3][300];
	unsigned live, shrinks[2], slabs_min_after_drain[2], low_reuse;
	uint32_t gen;
};
static struct sh_tri s_tri;
static uint8_t tri_arena[3][SH_CHUNKS * SH_CHUNK + 64];
static void* tri_take(unsigned k, struct mem2_pool* p) {
	void* (*take)(struct mem2_pool*) = k == 1 ? mem2_pool_take_lowest : mem2_pool_take;
	void* s = take(p);
	if(!s && mem2_pool_grow(p))
		s = take(p);
	return s;
}
static void tri_alloc(struct sh_tri* t, unsigned id) {
	if(t->slot[0][id])
		return;
	t->gen++;
	for(unsigned k = 0; k < 3; k++) {
		void* s = tri_take(k, &t->p[k]);
		assert(s);
		sh_fill(s, id * 7919u + t->gen);
		t->slot[k][id] = t->addr[k][id] = s;
	}
	t->live++;
}
static void tri_free(struct sh_tri* t, unsigned id) {
	if(!t->slot[0][id])
		return;
	for(unsigned k = 0; k < 3; k++) {
		assert(t->slot[k][id] == t->addr[k][id]); /* never moved */
		assert(memcmp(t->slot[k][id], t->slot[0][id], SH_OBJ) == 0);
	}
	for(unsigned k = 0; k < 3; k++) {
		if(k < 2) {
			unsigned before = t->p[k].shrinks;
			(void)sh_free(&t->p[k], &t->h[k], t->slot[k][id], true);
			t->shrinks[k] += t->p[k].shrinks - before;
		} else {
			(void)sh_free(&t->p[k], &t->h[k], t->slot[k][id], false);
		}
		t->slot[k][id] = NULL;
	}
	t->live--;
}
static void tri_verify(const struct sh_tri* t) {
	for(unsigned k = 0; k < 3; k++) {
		sh_check_invariants(&t->p[k]);
		assert(t->p[k].used == t->live);
	}
	for(unsigned i = 0; i < 300; i++)
		if(t->slot[0][i])
			for(unsigned k = 0; k < 3; k++) {
				assert(t->slot[k][i] == t->addr[k][i]);
				assert(mem2_pool_owns(&t->p[k], t->slot[k][i]));
				assert(memcmp(t->slot[k][i], t->slot[0][i], SH_OBJ) == 0);
			}
}
static void test_lowest_placement_reclaims(void) {
	struct sh_tri* t = &s_tri;
	memset(t, 0, sizeof *t);
	for(unsigned k = 0; k < 3; k++) {
		shrink_heap_init(&t->h[k], tri_arena[k]);
		mem2_pool_init(&t->p[k], POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP,
					   shrink_carve, &t->h[k]);
		for(unsigned b = 0; b < SH_KEEP; b++)
			assert(mem2_pool_grow(&t->p[k]));
	}
	t->slabs_min_after_drain[0] = t->slabs_min_after_drain[1] = UINT32_MAX;
	uint32_t rng = 12345u;
#define TRI_RNG() (rng = rng * 1103515245u + 12345u, rng >> 8)
	for(unsigned id = 0; id < 160; id++)
		tri_alloc(t, id);
	for(unsigned cycle = 0; cycle < 6; cycle++) {
		for(unsigned id = 160; id < 250; id++)
			tri_alloc(t, id);
		tri_verify(t);
		unsigned order[90];
		for(unsigned i = 0; i < 90; i++)
			order[i] = 160 + i;
		for(unsigned i = 89; i > 0; i--) {
			unsigned j = TRI_RNG() % (i + 1);
			unsigned x = order[i]; order[i] = order[j]; order[j] = x;
		}
		for(unsigned i = 0; i < 90; i++) {
			tri_free(t, order[i]);
			if(i % 4 == 0) {
				unsigned id = TRI_RNG() % 160;
				tri_free(t, id);
				tri_alloc(t, id);
				/* LOW-SLAB REUSE: the re-allocated steady column landed in
				 * the oldest slab with room. */
				int sl = mem2_pool_slab_of(&t->p[1], t->slot[1][id]);
				for(int w = 0; w < sl; w++)
					assert(t->p[1].bits[w] == UINT32_MAX);
				t->low_reuse++;
			}
		}
		for(unsigned i = 0; i < 200; i++) {
			unsigned id = TRI_RNG() % 160;
			tri_free(t, id);
			tri_alloc(t, id);
		}
		tri_verify(t);
		for(unsigned k = 0; k < 2; k++)
			if(mem2_pool_slabs(&t->p[k]) < t->slabs_min_after_drain[k])
				t->slabs_min_after_drain[k] = mem2_pool_slabs(&t->p[k]);
	}
#undef TRI_RNG
	/* Measured, not projected: after each drain the lowest placement is
	 * back at 6 slabs (160 steady + 16 headroom fit in 6 * 32 = 192), and
	 * it hands back and regrows two slabs EVERY cycle. */
	assert(t->slabs_min_after_drain[1] == 6);
	assert(t->shrinks[1] == 6u * 2u);
	assert(t->shrinks[1] > t->shrinks[0]);
	assert(t->h[1].frees == t->shrinks[1] && t->h[1].live == mem2_pool_slabs(&t->p[1]));
	printf("[pool] lowest-first on the cc_m1 shape: %u hand-backs (rotating "
		   "%u), back to %u slabs after every drain (rotating min %u), "
		   "%u low-slab reuses checked; every live object byte-identical "
		   "across lowest / rotating / never-shrinking and never moved\n",
		   t->shrinks[1], t->shrinks[0], t->slabs_min_after_drain[1],
		   t->slabs_min_after_drain[0], t->low_reuse);
}

/* drain -> shrink -> regrow with pinned and interleaved occupants under the
 * lowest placement: a pinned column holds its slab; everything else drains. */
static void test_lowest_pinned_drain_regrow(void) {
	struct shrink_heap h;
	struct mem2_pool p = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&p, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	for(unsigned k = 0; k < SH_KEEP; k++)
		assert(mem2_pool_grow(&p));
	enum { N = 8 * MEM2_POOL_SLAB_SLOTS };
	void* s[N];
	for(unsigned i = 0; i < N; i++) {
		s[i] = tri_take(1, &p);
		assert(s[i]);
		sh_fill(s[i], i);
	}
	/* Lowest-first fills in index order: column i is in slab i / 32. */
	for(unsigned i = 0; i < N; i++)
		assert(mem2_pool_slab_of(&p, s[i]) == (int)(i / 32));
	const uint8_t* pinned_base = p.base[6];
	void* pin = s[6 * 32 + 5];
	/* Drain the transient (slabs 4-7) except the pin, interleaved. */
	for(unsigned j = 0; j < 32; j++)
		for(unsigned sl = 4; sl < 8; sl++) {
			unsigned i = sl * 32 + j;
			if(s[i] == pin)
				continue;
			(void)sh_free(&p, &h, s[i], true);
			s[i] = NULL;
		}
	/* 129 live: slabs 4, 5, 7 emptied and went back as headroom allowed;
	 * the pinned slab stays. */
	assert(mem2_pool_slabs(&p) == 5 && p.used == 129);
	assert(mem2_pool_slab_of(&p, pin) == 4 && p.base[4] == pinned_base);
	assert(sh_check(pin, 6 * 32 + 5));
	/* Regrow: new columns fill the pinned slab's 31 holes FIRST, then carve. */
	void* more[96];
	for(unsigned i = 0; i < 96; i++) {
		more[i] = tri_take(1, &p);
		assert(more[i]);
		if(i < 31)
			assert(mem2_pool_slab_of(&p, more[i]) == 4);
		sh_fill(more[i], 5000 + i);
	}
	assert(mem2_pool_slabs(&p) == 8 && p.used == 129 + 96);
	sh_check_invariants(&p);
	for(unsigned i = 0; i < N; i++)
		if(s[i]) assert(sh_check(s[i], i));
	for(unsigned i = 0; i < 96; i++)
		assert(sh_check(more[i], 5000 + i));
	assert(sh_check(pin, 6 * 32 + 5));
	printf("[pool] lowest-first: pinned slab kept through the drain (8 -> 5), "
		   "its holes refilled first on regrow (5 -> 8); live bytes unchanged\n");
}

/* Concurrent alloc/free under the lock with the lowest placement. */
static bool s_sh_lowest;
static void* sh_worker_lowest(void* arg) {
	const uint32_t tid = (uint32_t)(uintptr_t)arg;
	void* mine[SH_PER_THREAD] = {0};
	uint32_t rng = 11u + tid * 104729u;
	for(unsigned step = 0; step < 40000; step++) {
		rng = rng * 1664525u + 1013904223u;
		unsigned k = (rng >> 10) % SH_PER_THREAD;
		if(!mine[k]) {
			pthread_mutex_lock(&s_sh_lock);
			void* s = tri_take(1, &s_sh_pool);
			pthread_mutex_unlock(&s_sh_lock);
			if(!s)
				continue;
			sh_fill(s, tid * 1000u + k);
			mine[k] = s;
		} else if(((rng >> 3) & 1) || step > 38000) {
			assert(sh_check(mine[k], tid * 1000u + k));
			pthread_mutex_lock(&s_sh_lock);
			(void)sh_free(&s_sh_pool, &s_sh_heap, mine[k], true);
			sh_check_invariants(&s_sh_pool);
			pthread_mutex_unlock(&s_sh_lock);
			mine[k] = NULL;
		}
	}
	for(unsigned k = 0; k < SH_PER_THREAD; k++)
		if(mine[k]) {
			assert(sh_check(mine[k], tid * 1000u + k));
			pthread_mutex_lock(&s_sh_lock);
			(void)sh_free(&s_sh_pool, &s_sh_heap, mine[k], true);
			pthread_mutex_unlock(&s_sh_lock);
		}
	return NULL;
}
static void test_lowest_concurrent(void) {
	shrink_heap_init(&s_sh_heap, shrink_arena);
	memset(&s_sh_pool, 0, sizeof s_sh_pool);
	mem2_pool_init(&s_sh_pool, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP,
				   shrink_carve, &s_sh_heap);
	for(unsigned k = 0; k < SH_KEEP; k++)
		assert(mem2_pool_grow(&s_sh_pool));
	pthread_t t[SH_THREADS];
	for(unsigned i = 0; i < SH_THREADS; i++)
		assert(pthread_create(&t[i], NULL, sh_worker_lowest, (void*)(uintptr_t)i) == 0);
	for(unsigned i = 0; i < SH_THREADS; i++)
		assert(pthread_join(t[i], NULL) == 0);
	assert(s_sh_pool.used == 0);
	sh_check_invariants(&s_sh_pool);
	assert(s_sh_heap.live == mem2_pool_slabs(&s_sh_pool));
	assert(s_sh_heap.carves - s_sh_heap.frees == s_sh_heap.live);
	printf("[pool] lowest-first concurrent: %d threads x 40000 ops, %u grows / "
		   "%u hand-backs, ends at %u slabs, no double handout\n", SH_THREADS,
		   s_sh_heap.carves - SH_KEEP, s_sh_pool.shrinks,
		   mem2_pool_slabs(&s_sh_pool));
	(void)s_sh_lowest;
}

/* Failed heap return under the lowest placement: the slab is kept and the
 * next column goes into it (it is the lowest slab with room). */
static void test_lowest_failed_return(void) {
	struct shrink_heap h;
	struct mem2_pool p = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&p, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	void* s[6 * 32];
	for(unsigned i = 0; i < 6 * 32; i++)
		s[i] = tri_take(1, &p);
	h.refuse_free = true;
	for(unsigned i = 4 * 32; i < 6 * 32; i++)
		(void)sh_free(&p, &h, s[i], true);
	assert(h.refused >= 1 && mem2_pool_slabs(&p) == 6 && p.shrinks == 0);
	void* again = tri_take(1, &p);
	assert(again && mem2_pool_slab_of(&p, again) == 4);
	h.refuse_free = false;
	printf("[pool] lowest-first: refused heap return keeps the slab, which the "
		   "next column reuses first\n");
}

/* Allocator CPU cost on the host: the two takes scan the same <= 9 bitmap
 * words. Reported, not gated (host timing says nothing about Broadway; the
 * Wii lock-wait counter mem2_lock_wait is the on-console instrument). */
#include <time.h>
static void test_lowest_take_cost(void) {
	struct shrink_heap h;
	struct mem2_pool p = {0};
	shrink_heap_init(&h, shrink_arena);
	mem2_pool_init(&p, POOL_LEAD, SH_OBJ, SH_STRIDE, SH_CAP, shrink_carve, &h);
	for(unsigned k = 0; k < 8; k++)
		assert(mem2_pool_grow(&p));
	void* s[8 * 32];
	for(unsigned i = 0; i < 8 * 32; i++)
		s[i] = mem2_pool_take(&p);
	for(unsigned i = 0; i < 8 * 32; i += 2) /* half full, holes everywhere */
		assert(mem2_pool_release(&p, s[i]));
	double ns[2];
	for(unsigned k = 0; k < 2; k++) {
		void* (*take)(struct mem2_pool*) = k ? mem2_pool_take_lowest : mem2_pool_take;
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		enum { ITER = 2000000 };
		for(unsigned i = 0; i < ITER; i++) {
			void* x = take(&p);
			assert(x);
			mem2_pool_release(&p, x);
		}
		clock_gettime(CLOCK_MONOTONIC, &b);
		ns[k] = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / ITER;
	}
	printf("[pool] take+release cost (host, 8 slabs half full): rotating "
		   "%.1f ns, lowest %.1f ns per pair\n", ns[0], ns[1]);
}

int main(void) {
	test_absent_pool();
	test_demand_carve();
	test_carve_failure();
	test_bad_release();
	test_census_totals();
	test_slab_occupancy();
	test_shrink_grow_drain_regrow();
	test_shrink_interleaved_occupancy();
	test_shrink_guards();
	test_shrink_equivalence();
	test_shrink_concurrent();
	test_event_queue_order();
	test_lowest_placement_reclaims();
	test_lowest_pinned_drain_regrow();
	test_lowest_concurrent();
	test_lowest_failed_return();
	test_lowest_take_cost();
	printf("test_mem2_pool: PASS\n");
	return 0;
}
