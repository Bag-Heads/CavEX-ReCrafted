/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * test_carve_pool_ledger -- the fixed-tenant slot pools must not corrupt the
 * carve observer's ledger, and one missed FREE note must not blind it.
 *
 * REAL-WII EVIDENCE (build 703aef00, split_anchor_6m seed 42 sep 256 far 6
 * voxel, tmp/wii_runs/split_anchor_6m_703aef00_s42_d256_fv6_voxel_0908_210248):
 * carve_census err went 0 -> 1 at t10, the tick the VIEW pool handed out its
 * first slot, and 1 -> 2 at t40, the tick the FAR pool handed out its first.
 * Then `over` (used - payload - guards, i.e. carve bytes the ledger cannot
 * account for) jumped +2,351,360 B at t70 and +682,240 B at t98 -- the two
 * pools' reservations, to the byte, as each pool's slot 0 was released for the
 * first time -- and never came back: the run's `over` floor stayed at ~3.21 MB
 * against ~0.19 MB before the pools, exactly pool_bytes = 3,033,088.
 *
 * ROOT CAUSE. The ledger is keyed by ADDRESS ALONE (carve_observer.c's
 * find/insert/erase). mem2.c noted each pool's one-shot reservation at
 * `p->base` (mem2_pool_note_one) while mem2_pool_take handed slot 0 out at
 * `p->base + stride * 0` -- the SAME address. So the first take emitted a
 * CARVE_ALLOC on an address the ledger still held live (insert() counted an
 * error and dropped the slot), and the first release emitted a CARVE_FREE that
 * erased the RESERVATION record, deleting megabytes of accounted carve.
 *
 * THE FIX is in mem2_pool.h (LEDGER IDENTITY): a pool's slots start `lead`
 * bytes above the block, so the reservation's address is never a slot address.
 * The lead is free -- it is the red-zone guard the reservation already carried,
 * moved from the tail to the front.
 *
 * Part 2 pins the second half: insert() now SELF-HEALS a collision (drops the
 * stale record, tracks the block that is really live) while still counting the
 * error, so a future missed note cannot blind the ledger for a whole run the
 * way this one did for 34 s of the same hardware run.
 *
 * ROUND 3 (2026-09-08) adds two more:
 *
 * Part 3 -- THE LEDGER TABLE IS RESERVED ONCE, AT ARM TIME. The same hardware
 * run measured the OTHER half of the defect: the ledger table used to grow on
 * demand, and its 1.44 MB calloc for the 32,768-record step FAILED under load
 * pressure -- tracked blocks collapsed 4,888 -> 745 and errors ran 3 -> 11,468
 * over t104-137, recovering only when memory freed again. Every census number
 * in that window came from a ledger tracking a seventh of the heap. The table
 * is now allocated once in carve_observer_start (CARVE_LEDGER_SLOTS records, at
 * boot, before the world loads) and NEVER reallocated; tombstones are gone with
 * it (backward-shift deletion), because tombstone compaction was what the
 * mid-run allocation existed to do. Part 3 drives a scripted load well past the
 * old table's first growth step, with heavy churn to exercise the new deletion,
 * and gates that tracked blocks == live blocks with errors == 0 and a capacity
 * that never moved.
 *
 * Part 4 -- and if that ONE reservation fails, the observer says so. It arms
 * with the ledger DISABLED rather than counting an error per allocation
 * (11,468 of them was the noise that hid the real signal) or refusing to arm at
 * all: the heap-side census numbers come from __lwp_heap_getinfo, not the
 * ledger, and are exactly what a run short of memory most needs to report.
 *
 * Part 1 is also re-based on round 3's DEMAND CARVE: the pools start empty and
 * buy one slab (32 slots) per exhaustion, so the reservation the ledger must
 * not lose is now one record PER SLAB.
 */
#include "carve_observer.h"
#include "platform/mem2_pool.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The shipped far pool, at its shipped numbers (platform/mem2.h). */
#define GUARD 256u
#define FAR_OBJECT 10240u
#define FAR_SLOTS 224u
#define FAR_STRIDE (FAR_OBJECT + GUARD)
#define POOL_LEAD GUARD /* mem2.c's MEM2_POOL_LEAD */
#define FAR_SLABS (FAR_SLOTS / MEM2_POOL_SLAB_SLOTS)
#define FAR_SLAB_BYTES (MEM2_POOL_SLAB_SLOTS * FAR_STRIDE)

/* mem2.c's carve hook (a bare __lwp_heap_allocate under mem2_lock) stood in by
 * a bump allocator, so a slab lands at a real, distinct address the ledger can
 * key on. */
static uint8_t far_store[FAR_SLABS * (POOL_LEAD + FAR_SLAB_BYTES) + 32];
static size_t far_store_next;
static void* far_carve(size_t bytes, void* ctx) {
	(void)ctx;
	size_t base = (far_store_next + 31u) & ~(size_t)31u;
	if(base + bytes > sizeof(far_store)) return NULL;
	far_store_next = base + bytes;
	return far_store + base;
}

/* ------------------------------------------------------------------ */
/* The exact note sequences mem2.c emits, replayed verbatim            */
/* ------------------------------------------------------------------ */

/* mem2.c mem2_pool_note_slab, inside the caller's CARVE_BEGIN/END window. */
static void note_slab(const struct mem2_pool* p, const void* block) {
	size_t bytes = mem2_pool_slab_bytes(p);
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_ALLOC, (uintptr_t)block, 0, bytes,
			   CARVE_MEM2 | CARVE_IN_HEAP);
	carve_note(mem2_carve_observe, CARVE_TAG, (uintptr_t)block, 0, bytes,
			   CARVE_SLOTS);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}

/* mem2.c mem2_allocate_pooled's grow path: carve one slab, attribute it, retry.
 * Returns NULL at the cap / on a failed carve, which the caller counts. */
static void* pool_take_or_grow(struct mem2_pool* p) {
	void* s = mem2_pool_take(p);
	if(s) return s;
	const void* slab = mem2_pool_grow(p);
	if(!slab) return NULL;
	note_slab(p, slab);
	return mem2_pool_take(p);
}

/* mem2.c mem2_allocate_pooled's hit path, then mem2_tag (mem2.h). */
static void note_slot_alloc(const void* slot, size_t bytes, unsigned tenant) {
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_ALLOC, (uintptr_t)slot, 0, bytes,
			   CARVE_MEM2 | CARVE_IN_HEAP | CARVE_POOLED);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_TAG, (uintptr_t)slot, 0, bytes, tenant);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}

/* mem2.c mem2_free. */
static void note_free(const void* p) {
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_FREE, (uintptr_t)p, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}

/* An ordinary shared-heap block (mem2_allocate's miss path + mem2_tag). */
static void note_heap_alloc(const void* p, size_t bytes, unsigned tenant) {
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_ALLOC, (uintptr_t)p, 0, bytes,
			   CARVE_MEM2 | CARVE_IN_HEAP);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_TAG, (uintptr_t)p, 0, bytes, tenant);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}

/* PC has no carve, so the heap side of the reconciliation is supplied: a
 * capacity comfortably larger than any payload below (so the used/payload/guard
 * identity holds) and the number of HEAP BLOCKS the ledger is expected to hold,
 * +1 for the terminal dummy libogc's getinfo counts. Passing the expectation is
 * the point: snapshot_locked raises an error when its own block count disagrees,
 * so every call below also pins that a pool SLOT is not a heap block while the
 * reservation is. */
enum { SNAP_CAPACITY = 64u * 1024u * 1024u };
static void snap(struct carve_totals* t, unsigned blocks) {
	carve_observer_test_snapshot(t, SNAP_CAPACITY, 0, 0, blocks + 1u);
}

/* ------------------------------------------------------------------ */
/* Part 1: the two-anchor reload sequence that lost the reservations   */
/* ------------------------------------------------------------------ */

static void test_pool_slot_reuse_keeps_the_reservation(void) {
	struct mem2_pool p;
	far_store_next = 0;
	mem2_pool_init(&p, POOL_LEAD, FAR_OBJECT, FAR_STRIDE, FAR_SLABS, far_carve,
				   NULL);
	assert(mem2_pool_slabs(&p) == 0 && mem2_pool_bytes(&p) == 0);

	assert(carve_observer_start());
	struct carve_ledger_stat led;
	carve_observer_ledger(&led);
	assert(led.enabled && led.capacity == CARVE_LEDGER_SLOTS);

	struct carve_totals t;
	/* DEMAND CARVE: nothing is reserved until a slot is wanted, so the ledger
	 * starts with no CARVE_SLOTS record at all -- round 2 had two, worth
	 * 3,033,088 B, from boot. */
	snap(&t, 0);
	assert(t.errors == 0);
	assert(t.count[CARVE_SLOTS] == 0 && t.bytes[CARVE_SLOTS] == 0);

	/* The hardware sequence: the pool fills, drains and refills across the
	 * reload, so slot 0 of each slab -- the address that aliased that slab's
	 * reservation -- is taken, released and re-issued many times. Eight live at
	 * a time needs exactly one slab. */
	void* slot[8];
	for(unsigned round = 0; round < 4; round++) {
		for(unsigned i = 0; i < 8; i++) {
			slot[i] = pool_take_or_grow(&p);
			assert(slot[i]);
			note_slot_alloc(slot[i], FAR_OBJECT, CARVE_VOXEL);
		}
		/* Round 0 hands out slab 0's slot 0 first; the rotating hint re-issues
		 * it later too. Either way it must never collide with the slab. */
		assert(round != 0 || slot[0] == p.base[0]);
		assert(slot[0] != mem2_pool_slab_block(&p, 0));
		for(unsigned i = 0; i < 8; i++) {
			note_free(slot[i]);
			assert(mem2_pool_release(&p, slot[i]));
		}
	}
	assert(mem2_pool_slabs(&p) == 1); /* one slab covered all four rounds */

	snap(&t, 1); /* the one carved slab is one heap block */
	/* THE GATE. Pre-fix this reads errors == 1 (the very first take collided
	 * with the reservation) and CARVE_SLOTS == 0 bytes / 0 records (the first
	 * release erased the reservation record), which is the 2,351,360 B the
	 * hardware run's `over` gained at t70 and never gave back. */
	assert(t.errors == 0);
	assert(t.count[CARVE_SLOTS] == 1);
	assert(t.bytes[CARVE_SLOTS] == FAR_SLAB_BYTES);
	/* A pool SLOT stays out of the physical totals (its bytes were counted
	 * once by its slab) but keeps its tenant's b/n columns honest. */
	assert(t.count[CARVE_VOXEL] == 0); /* all released */
	assert(t.carve[CARVE_SLOTS] == FAR_SLAB_BYTES);

	/* Now grow past one slab: every carve adds exactly one more reservation
	 * record, and slot reuse inside the older slabs never disturbs them. */
	void* many[3 * MEM2_POOL_SLAB_SLOTS];
	for(unsigned i = 0; i < 3 * MEM2_POOL_SLAB_SLOTS; i++) {
		many[i] = pool_take_or_grow(&p);
		assert(many[i]);
		note_slot_alloc(many[i], FAR_OBJECT, CARVE_VOXEL);
	}
	assert(mem2_pool_slabs(&p) == 3);
	snap(&t, 3); /* three carved slabs, three heap blocks */
	assert(t.errors == 0);
	assert(t.count[CARVE_SLOTS] == 3);
	assert(t.bytes[CARVE_SLOTS] == 3u * FAR_SLAB_BYTES);
	assert(t.count[CARVE_VOXEL] == 3u * MEM2_POOL_SLAB_SLOTS);
	assert(t.bytes[CARVE_VOXEL] == 3u * MEM2_POOL_SLAB_SLOTS * FAR_OBJECT);
	assert(t.carve[CARVE_VOXEL] == 0); /* pooled: not a heap block */

	/* Re-arming replays the slabs carved so far -- correct, because a slab is
	 * never returned -- and must produce exactly the same three records. */
	for(unsigned i = 0; i < 3 * MEM2_POOL_SLAB_SLOTS; i++) {
		note_free(many[i]);
		assert(mem2_pool_release(&p, many[i]));
	}
	assert(carve_observer_stop());
	assert(carve_observer_start());
	for(unsigned i = 0; i < mem2_pool_slabs(&p); i++)
		note_slab(&p, mem2_pool_slab_block(&p, i));
	snap(&t, 3);
	assert(t.errors == 0);
	assert(t.count[CARVE_SLOTS] == 3);
	assert(t.bytes[CARVE_SLOTS] == 3u * FAR_SLAB_BYTES);

	assert(carve_observer_stop()); /* stop() is false if any error was counted */
	printf("[carve/pool] %u slabs carved on demand survive slot-0 reuse and a "
		   "re-arm, errors 0\n", mem2_pool_slabs(&p));
}

/* ------------------------------------------------------------------ */
/* Part 2: one missed FREE must cost one error, not the whole ledger   */
/* ------------------------------------------------------------------ */

static void test_insert_collision_self_heals(void) {
	static uint8_t a[64], b[64];
	assert(carve_observer_start());

	struct carve_totals t;
	note_heap_alloc(a, 4096, CARVE_TEXTURE);
	snap(&t, 1);
	assert(t.errors == 0 && t.count[CARVE_TEXTURE] == 1
		   && t.bytes[CARVE_TEXTURE] == 4096);

	/* The defect class: a free path that forgot its note. The heap then reuses
	 * the address for a different, larger block. */
	note_heap_alloc(a, 8192, CARVE_TEXTURE);
	snap(&t, 1);
	/* Counted -- the miss stays visible -- but the ledger now describes the
	 * block that is actually live. Pre-fix the insert was REFUSED, so this read
	 * 4096 and the real block stayed untracked for the rest of the run. */
	assert(t.errors == 1);
	assert(t.count[CARVE_TEXTURE] == 1);
	assert(t.bytes[CARVE_TEXTURE] == 8192);

	/* And it recovers: the next honest free/alloc pair is clean again. */
	note_free(a);
	note_heap_alloc(a, 1024, CARVE_TEXTURE);
	note_heap_alloc(b, 2048, CARVE_TEXTURE);
	snap(&t, 2);
	assert(t.errors == 1); /* no NEW error */
	assert(t.count[CARVE_TEXTURE] == 2);
	assert(t.bytes[CARVE_TEXTURE] == 1024 + 2048);

	note_free(a);
	note_free(b);
	snap(&t, 0);
	assert(t.count[CARVE_TEXTURE] == 0 && t.bytes[CARVE_TEXTURE] == 0);
	carve_observer_stop(); /* false: one error was counted, on purpose */
	printf("[carve/ledger] missed FREE costs 1 error and self-heals; "
		   "ledger tracks the live block\n");
}

/* ------------------------------------------------------------------ */
/* Part 3: the fixed ledger table under a scripted load                */
/* ------------------------------------------------------------------ */

/* Synthetic block addresses. The ledger is keyed by address and never
 * dereferences a key, so a scripted load does not have to own 6,000 real
 * allocations to reproduce the shape of one -- and a stride that is not a power
 * of two keeps the probe runs from degenerating into one contiguous block. */
static uintptr_t addr(unsigned i) {
	return (uintptr_t)0x90000000u + (uintptr_t)i * 4176u;
}
static void note_alloc_at(uintptr_t k, size_t bytes, unsigned tenant) {
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_ALLOC, k, 0, bytes,
			   CARVE_MEM2 | CARVE_IN_HEAP);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_TAG, k, 0, bytes, tenant);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}
static void note_free_at(uintptr_t k) {
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_FREE, k, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}

/*
	THE HARDWARE DEFECT, AS A PC GATE. On real Wii 703aef00 the ledger table
	grew mid-run and its 1.44 MB calloc for the 32,768-record step FAILED under
	load pressure: tracked blocks collapsed 4,888 -> 745 and errors ran 3 ->
	11,468 over t104-137, recovering only when memory freed again.

	A PC test cannot squeeze the host heap the way arena2 was squeezed. What it
	CAN gate -- and what actually makes the hardware failure impossible rather
	than merely unlikely -- is that the allocation which failed no longer
	exists:

	  * the table capacity NEVER MOVES after arming, and neither does the
	    observer own byte footprint, across a load far past the point the old
	    table would have doubled twice (it started at 4,096 records and doubled
	    whenever live * 4 reached capacity);
	  * tracked blocks == live blocks at every checkpoint, with errors == 0 and
	    ledger_full == 0 -- the counter that fires if the FIXED size were ever
	    the thing that under-tracks;
	  * the churn is heavy and out of order, because the backward-shift deletion
	    that replaced the tombstones is what let the table stop growing. A probe
	    chain broken by it shows up immediately: an unfindable key makes its
	    free a no-op, so the final release-everything cannot reach zero.

	Part 4 covers the one remaining way the reservation can fail at all.
*/
static void test_fixed_ledger_under_load(void) {
	enum { LIVE = 6000, CHURN_ROUNDS = 6 };
	/* Past the 4,888 the hardware run reached before the table gave up. */
	assert(LIVE > 4888);
	assert(carve_observer_start());

	struct carve_ledger_stat led0, led;
	carve_observer_ledger(&led0);
	assert(led0.enabled && led0.capacity == CARVE_LEDGER_SLOTS);
	assert(led0.live == 0 && led0.full == 0);

	struct carve_totals t0, t;
	snap(&t0, 0);
	assert(t0.errors == 0);

	for(unsigned i = 0; i < LIVE; i++)
		note_alloc_at(addr(i), 1024, CARVE_COLUMN);
	snap(&t, LIVE);
	assert(t.errors == 0);
	assert(t.count[CARVE_COLUMN] == LIVE);
	assert(t.bytes[CARVE_COLUMN] == (uint64_t)LIVE * 1024u);
	carve_observer_ledger(&led);
	assert(led.enabled && led.capacity == led0.capacity); /* never reallocated */
	assert(led.live == LIVE && led.peak == LIVE && led.full == 0);
	/* The observer footprint is the table plus its fixed state, so pinning it
	 * across the whole load pins "no allocation happened". */
	assert(t.observer_bytes == t0.observer_bytes);

	/* Heavy out-of-order churn: free a third of the set and re-allocate the
	 * same count at fresh addresses, six times over. Every erase closes its own
	 * gap by moving whatever records were displaced past it. */
	static unsigned live_key[LIVE];
	for(unsigned i = 0; i < LIVE; i++)
		live_key[i] = i;
	unsigned next = LIVE;
	for(unsigned r = 0; r < CHURN_ROUNDS; r++) {
		/* A stride coprime with LIVE walks the live set in a different order
		 * each round, so the deletions are never a contiguous sweep. */
		for(unsigned n = 0, s = r; n < LIVE; n++, s = (s + 2333u) % LIVE) {
			if(n % 3)
				continue;
			note_free_at(addr(live_key[s]));
			live_key[s] = next++;
			note_alloc_at(addr(live_key[s]), 1024, CARVE_COLUMN);
		}
		snap(&t, LIVE);
		assert(t.errors == 0);
		assert(t.count[CARVE_COLUMN] == LIVE); /* tracked == live, every round */
		carve_observer_ledger(&led);
		assert(led.live == LIVE && led.capacity == led0.capacity);
		assert(led.full == 0 && led.peak == LIVE);
		assert(t.observer_bytes == t0.observer_bytes);
	}

	/* Release everything ever allocated. A key the deletion made unfindable
	 * would make its free a no-op and leave the count above zero. */
	for(unsigned i = 0; i < next; i++)
		note_free_at(addr(i));
	snap(&t, 0);
	assert(t.errors == 0);
	assert(t.count[CARVE_COLUMN] == 0 && t.bytes[CARVE_COLUMN] == 0);
	carve_observer_ledger(&led);
	assert(led.live == 0 && led.full == 0);
	assert(led.capacity == led0.capacity);

	assert(carve_observer_stop());
	printf("[carve/ledger] %u live + %u churn rounds (%u keys): tracked == "
		   "live, errors 0, table fixed at %u records, 0 regrows\n",
		   (unsigned)LIVE, (unsigned)CHURN_ROUNDS, next,
		   (unsigned)led0.capacity);
}

/* ------------------------------------------------------------------ */
/* Part 4: a failed arm-time reservation is REPORTED, not degraded     */
/* ------------------------------------------------------------------ */

static void test_ledger_arm_failure_reports_disabled(void) {
	carve_observer_test_ledger_arm_fail();
	/* The observer still ARMS: the heap-side census numbers come from
	 * __lwp_heap_getinfo, not from the ledger, and a run that is short of
	 * memory is precisely the run that needs them. */
	assert(carve_observer_start());
	struct carve_ledger_stat led;
	carve_observer_ledger(&led);
	assert(!led.enabled && led.capacity == 0);

	/* Every note is a quiet no-op. The old behaviour -- one counted error per
	 * refused insert -- is what produced the 11,468 errors that buried the real
	 * signal on hardware; the single ledger:0 census field replaces it. */
	struct carve_totals t;
	for(unsigned i = 0; i < 500; i++)
		note_alloc_at(addr(i), 1024, CARVE_COLUMN);
	/* Zero heap blocks tracked, so the reconciliation is told zero: the ledger
	 * is honestly empty rather than wrong. */
	snap(&t, 0);
	assert(t.errors == 0);
	assert(t.count[CARVE_COLUMN] == 0);
	carve_observer_ledger(&led);
	assert(!led.enabled && led.live == 0 && led.full == 0);
	assert(carve_observer_stop());

	/* One-shot fixture: the next arm gets its table. */
	assert(carve_observer_start());
	carve_observer_ledger(&led);
	assert(led.enabled && led.capacity == CARVE_LEDGER_SLOTS);
	assert(carve_observer_stop());
	printf("[carve/ledger] failed arm-time reservation reports ledger:0 and "
		   "counts no per-allocation errors; next arm recovers\n");
}

int main(void) {
	test_pool_slot_reuse_keeps_the_reservation();
	test_insert_collision_self_heals();
	test_fixed_ledger_under_load();
	test_ledger_arm_failure_reports_disabled();
	printf("test_carve_pool_ledger: PASS\n");
	return 0;
}
