#include "../settle_observer.h"
settle_callback mem2_settle_observe;
uint32_t settle_observer_control;
/* Defined here, next to the control word, for the same reason: mem2.c uses the
 * SETTLE macro and is linked into pools that do not carry settle_observer.o. */
uint32_t settle_observer_inflight;
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

#include "../tps_observer.h"
tps_callback mem2_tps_observe;


#include <stdint.h>
#include <string.h>

#include "mem2.h"
carve_callback mem2_carve_observe;

// Session diagnostics (both platforms): cumulative alloc/free/realloc call counts +
// total bytes requested via mem2_alloc. Cheap counters (no per-pointer bookkeeping). Used
// by the test_mem1_budget regression guard to prove the ~80KB/chunk server arrays route
// through MEM2 (not the capped MEM1 heap) and are all freed (no leak). The explicit
// realloc counter stays platform-consistent even though Wii realloc also increments
// alloc/free internally. Not thread-safe by design (diagnostic snapshots only).
static size_t s_mem2_alloc_calls = 0;
static size_t s_mem2_free_calls = 0;
static size_t s_mem2_realloc_calls = 0;
static size_t s_mem2_alloc_bytes = 0;
// DIAGNOSTIC (Wii): how many mem2_allocs the MEM2 carve could NOT satisfy and so
// spilled to the memalign fallback (s_mem2_carve_spill) -- the general malloc
// heap, which is Arena2 itself under the MALLOC_MEM2=1 pin below (it was MEM1
// on the June toolchain, when the weak default was 0) -- and how many failed
// even the fallback = a TRUE NULL alloc (s_mem2_oom). The F3 "MEM2 free" reads SYS_GetArena2Size,
// which is the spill region ABOVE the carve, NOT the carve where display lists live -- so
// it can show tens of MB free while the carve is fragmented/full. A climbing carve-spill
// (and any oom) is the real fragmentation/OOM tell behind the partial-mesh -> GP-wedge
// freeze hypothesis. PC leaves both 0.
static size_t s_mem2_carve_spill = 0;
static size_t s_mem2_oom = 0;
// SPLIT-SPILL (2026-10-14): the GUARDED byte count (size + MEM2_GUARD) of the
// most recent request the carve could not satisfy, stored at the exact site
// s_mem2_carve_spill increments. Lets a heartbeat-cadence observer line name
// the size class of the spilling tenant (80.25 KiB overlay private image vs a
// packed display-list block) instead of inferring it; it does NOT identify
// the tenant uniquely. 0 until the first spill (PC never spills). Written
// under the mem2 lock with a relaxed atomic store (no allocation, no logging
// inside the allocator); read lock-free by main with a relaxed atomic load,
// so a reader sees either the previous or the new 32-bit value, never a tear.
static uint32_t s_mem2_last_spill_bytes = 0;
// P2-c telemetry (2026-09-03 round-2 frame-drop root-cause doc, mechanism A's
// "the saturated mesher's MEM2 grow/flush churn ... contends with the GP's
// own MEM2 streaming reads" theory): how many times a thread found the
// serialising mem2 mutex (s_mem2_mutex, below, Wii-only) ALREADY HELD by
// another thread and had to actually block for it, vs sailing through
// uncontended. Written from mem2_lock() with __atomic (genuinely cross-
// thread: the increment happens on the LOSING thread, BEFORE it acquires the
// lock, so it cannot be serialised by that same lock the way s_mem2_carve_
// spill/s_mem2_oom above are). PC has no mem2 lock at all (no MEM1/MEM2
// split, no contention to count) and always reads 0.
static uint32_t s_mem2_lock_wait = 0;

size_t mem2_stat_alloc_calls(void) { return s_mem2_alloc_calls; }
size_t mem2_stat_free_calls(void) { return s_mem2_free_calls; }
size_t mem2_stat_realloc_calls(void) { return s_mem2_realloc_calls; }
size_t mem2_stat_alloc_bytes(void) { return s_mem2_alloc_bytes; }
size_t mem2_stat_carve_spill(void) { return s_mem2_carve_spill; }
size_t mem2_stat_oom(void) { return s_mem2_oom; }
uint32_t mem2_stat_last_spill_bytes(void) {
	return __atomic_load_n(&s_mem2_last_spill_bytes, __ATOMIC_RELAXED);
}
uint32_t mem2_stat_lock_wait(void) {
	return __atomic_load_n(&s_mem2_lock_wait, __ATOMIC_RELAXED);
}

// REGION SPLIT (shared TU so the PC suite pins it; only the Wii branch calls it).
//
// ORDERING, and therefore WHO GETS CLIPPED. arena2 measures ~51.8 MB in practice
// (the MEM2-migration table in docs/platform_perf_notes.md read 36627 KB free
// above the then-16 MB carve, so arena2 = 36627 + 16384 = 53011 KB), while the
// three wants sum to 36 + 12 + 4 = 52 MB. They still do not all fit, so the
// split is a PRIORITY, not a partition:
//
//   1. MEM2_RESERVE_MARGIN is taken off the top first and is never spent. It is
//      the second-level fallback: when the carve is exhausted mem2_alloc falls
//      back to memalign -- the general malloc heap, i.e. under the
//      MALLOC_MEM2=1 pin this margin plus whatever else of arena2 is left.
//      (The original "fills MEM1 and then spills here" wording described the
//      June toolchain's MEM1 heap; there is no cross-arena spill either way.)
//   2. The CARVE takes up to MEM2_HEAP_SIZE out of what is left.
//   3. The AUDIO region takes whatever remains, up to MEM2_AUDIO_REGION_BYTES.
//
// The carve wins because the two failure modes are not comparable. A short audio
// region degrades: the registry resolves fewer variants, says so, and the game
// plays with some sounds missing (audio_registry.c fail-softs by design, and the
// granted size is on the F3 "snd: arena used/total" row). A short CARVE spills
// display-list reallocs into the general malloc heap (MEM1 on the June
// toolchain, arena2 under today's pin) -- the exact storm that drove
// mem2_stat_carve_spill into the tens of thousands and wedged the GP, i.e. the
// freeze this whole region split exists to keep away. A cosmetic loss is always
// the right side of that trade.
//
// CONSEQUENCE, stated on purpose rather than discovered on hardware: at 53011 KB
// arena2 the audio region is granted 12051 KB, a little under the 12288 KB want,
// so the region IS clipped. The full real Beta pack is 15,078,752 B resident
// (tmp/jobs/c2-audio-coverage-0917/report.md) against the 12,443,648 B the
// console actually grants, so resident loading alone would drop 15 files. The
// shipped default, audio.deferred_cave ON (docs/deviations.md, maintainer ruling
// 2026-10-21), keeps the 13 cave variants off the arena: the resident set is
// ~11.3 MB and fits with ~0.7 MB spare. No sound key is dropped at the shipped
// constants with the default on. MEM2_HEAP_SIZE is the only dial
// that changes this: every megabyte taken off the carve goes straight to audio,
// and every megabyte added to it comes straight out of audio (the ~0.7 MB spare over
// the ~11.3 MB deferred set is gone at about 37 MB, after which mood/portal/fire
// keys start being cut). Do NOT "fix" a future overflow by shrinking AUDIO_ARENA_BYTES; that hides
// the shortfall instead of reporting it. PCM8 (2:1) is the sanctioned next step.
bool mem2_plan_regions(size_t avail, size_t carve_want, size_t audio_want,
					   size_t margin, struct mem2_region_plan* out) {
	if(!out)
		return false;
	memset(out, 0, sizeof(*out));
	if(avail <= margin)
		return false; // genuinely no room
	size_t budget = avail - margin;
	size_t carve = carve_want < budget ? carve_want : budget;
	if(!carve)
		return false; // no usable carve: leave arena2 untouched
	size_t left = budget - carve;
	// The audio base must be 32-byte aligned for the DSP DMA, and a carve CLAMPED to
	// arena2 is not necessarily a multiple of 32. Pad the offset up and pay for the
	// pad out of the audio region, never out of the spill margin.
	size_t audio_off = (carve + (MEM2_REGION_ALIGN - 1))
		& ~(size_t)(MEM2_REGION_ALIGN - 1);
	size_t pad = audio_off - carve;
	size_t audio = 0;
	if(left > pad) {
		size_t room = left - pad;
		audio = audio_want < room ? audio_want : room;
	}
	if(!audio)
		audio_off = carve; // nothing reserved past the carve, so no pad either
	out->carve_off = 0;
	out->carve_size = carve;
	out->audio_off = audio_off;
	out->audio_size = audio;
	out->reserved_total = audio_off + audio;
	out->spill_size = avail - out->reserved_total;
	return true;
}

#ifdef PLATFORM_WII

#include <limits.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ogc/irq.h>
#include <ogc/lwp_heap.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <ogc/system.h>
#include <stdio.h>

#include "hw_telemetry.h"
#include "wii/mem2_heap_validate.h"
#include "mem2_pool_evq.h"

// Verified against the 2026-09-28 Wii ELF: this libogc _sbrk_r starts malloc
// in Arena1, then permits it to spill into Arena2 when MALLOC_MEM2 is 1.
// The flag is an initialized-data symbol read by _sbrk_r; libogc ships a
// weak default that has differed across devkitPro images. Define it strong
// so spill permission is our decision. It must stay initialized data, never
// a runtime assignment: libc can allocate before static init runs. The 1
// value preserves the shipped MEM1-first, MEM2-spill behavior; changing it
// would be a separate real-Wii performance and capacity decision.
u32 MALLOC_MEM2 = 1;

// Bytes reserved in MEM2 for GX buffers. Tenants: texture texels (stock ~1.5 MB,
// more for HD packs), client chunk block buffers (10240 B/chunk, chunk.c), server
// chunk id/meta/light/heightmap arrays (~82 KB/column, region_archive.c) and
// per-chunk display lists (variable, tens of KB/chunk, and they churn via realloc
// as meshes grow).
//
// THE "TENANTS PEAK AT 6-8 MB" FIGURE -- which this comment used to carry, and which
// docs/deviations.md and docs/render_distance_root_cause.md still quote -- IS STALE. It
// was an ESTIMATE from the 16 MB era, never re-measured, and the client chunk buffers
// alone now blow through it: 10240 B x 968 chunks ((2*5+1)^2 sections x COLUMN_HEIGHT
// at the shipped Wii MAX_VIEW_DISTANCE 5) = 9.45 MiB, and 11.25 MiB at the MAX_CHUNKS
// (12x12 columns) ceiling. Add the server-chunk arrays -- the same estimate budgeted
// ~12 MB for them and assumed they lived in the malloc spill, but region_archive.c
// routes them through mem2_alloc, so they are carve tenants too -- plus the display
// lists, and real demand is well north of 20 MB. That is WHY the 16 MB carve spilled
// tens of thousands of times. Treat 6-8 MB as folklore until someone photographs a
// real high-water reading.
//
// GROWN 16MB -> 40MB. Real-Wii F3 showed mem2_stat_carve_spill climbing into the tens of
// thousands: the 16MB carve was far too small for the display-list realloc churn, so nearly
// every list spilled to MEM1 (memalign), pinning MEM1 free at ~1.5MB and conditioning the
// freeze. ~36MB of MEM2 sat idle above the carve. The one hardware datum
// (docs/release_1_0_feasibility.md) is spill 0 / oom 0 / canary 0 at a 40MB carve, which
// proves 40MB SUFFICIENT -- it never proved it NECESSARY.
//
// TRIMMED 40MB -> 36MB (2026-07-29), to pay for the audio region beside it. THIS IS NOT A
// REGRESSION FROM A MEASURED-GOOD STATE, and the comparison is the whole argument:
//   - BEFORE: a 40MB carve with the 12MB audio arena allocated INSIDE it (mem2_alloc), so
//     the display lists and chunk buffers effectively had ~28MB -- and that is the exact
//     configuration the console was running when it hung on 2026-07-27.
//   - AFTER: a dedicated 36MB with no audio competing for it. That is 8MB MORE room for the
//     churn tenants than the state that failed, plus the fragmentation win of removing a
//     permanent, never-freed, never-realloced 12MB block from a high-churn lwp heap.
// The 12MB audio PCM arena is NOT a tenant here and must never become one again. It now has
// its own region (MEM2_AUDIO_REGION_BYTES). This constant is also the dial that decides how
// much arena2 is left for that region: at 36MB the region gets 12051 KB, enough to hold the
// ~11.3MB deferred-cave resident set of the real Beta pack (the full 15.08MB pack
// never fit resident), so no sound content is dropped with the default on. Raising it
// back toward 40MB silently starts cutting sound keys -- read the CONSEQUENCE note on
// mem2_plan_regions before changing it, and re-check it against docs/deviations.md's
// measured pack size.
//
// Still CLAMPED to what arena2 actually has (minus the spill margin) so it can never
// silently fail to MEM1.
//
// The VALUE lives in mem2.h beside MEM2_AUDIO_REGION_BYTES, because the two are a coupled
// pair and tests/test_mem2_audio_region.c has to see both to pin the shipping split
// against the measured pack size. This comment is the argument for it.

// Leave at least this much of arena2 for the normal malloc spill.
/* 4 MiB until 2026-09-10; 1.5 MiB since the repartition experiment (the other
 * 2.5 MiB moved into MEM2_HEAP_SIZE -- the two are a coupled pair, see mem2.h).
 * The margin is arena2's landing zone for malloc spill; the mesh-blocks run
 * never claimed any of it, the list-ladder run claimed 2.35 MiB of it while
 * spilling 9,134 lists -- which is the shape mesh blocks (now default ON)
 * removes. Revert both constants together if a hardware run shows arena2
 * exhaustion (mem2_free_kb at 0 in the heartbeat). */
#define MEM2_RESERVE_MARGIN (1536 * 1024)

static heap_cntrl mem2_heap;
static uint8_t* mem2_base = NULL;	  // start of our reserved MEM2 region
static size_t mem2_carve_size = 0;	  // actual carve size (may be < MEM2_HEAP_SIZE if clamped)
static bool mem2_ready = false;

/* Temporary cold-stall probe. The existing MEM2 mutex serializes heap calls;
 * the active operation is visible to the watchdog without taking that lock. */
enum { MEM2_DIAG_IDLE, MEM2_DIAG_ALLOC, MEM2_DIAG_FREE,
	MEM2_DIAG_SHRINK, MEM2_DIAG_GETINFO, MEM2_DIAG_POOL, MEM2_DIAG_MP,
	MEM2_DIAG_VALIDATE };
static uint32_t s_diag_op, s_diag_thread, s_diag_bad, s_diag_bad_addr;
static uint32_t s_diag_scan_count, s_diag_scan_max_us, s_diag_scan_total_us;
static uint32_t s_diag_reported;

uint32_t mem2_diag_op(void) {
	return __atomic_load_n(&s_diag_op, __ATOMIC_RELAXED);
}
uint32_t mem2_diag_thread(void) {
	return __atomic_load_n(&s_diag_thread, __ATOMIC_RELAXED);
}
uint32_t mem2_diag_bad(void) {
	return __atomic_load_n(&s_diag_bad, __ATOMIC_ACQUIRE);
}
uint32_t mem2_diag_scan_count(void) {
	return __atomic_load_n(&s_diag_scan_count, __ATOMIC_RELAXED);
}
uint32_t mem2_diag_scan_max_us(void) {
	return __atomic_load_n(&s_diag_scan_max_us, __ATOMIC_RELAXED);
}
uint32_t mem2_diag_scan_total_us(void) {
	return __atomic_load_n(&s_diag_scan_total_us, __ATOMIC_RELAXED);
}

static void mem2_diag_begin(uint32_t op) {
	__atomic_store_n(&s_diag_thread, (uint32_t)LWP_GetSelf(), __ATOMIC_RELAXED);
	__atomic_store_n(&s_diag_op, op, __ATOMIC_RELEASE);
}
static void mem2_diag_end(void) {
	__atomic_store_n(&s_diag_op, MEM2_DIAG_IDLE, __ATOMIC_RELEASE);
}
static void mem2_diag_emit_bad(void) {
	if(!mem2_diag_bad() || __atomic_exchange_n(&s_diag_reported, 1,
			__ATOMIC_RELAXED)) return;
	char line[128];
	snprintf(line, sizeof(line),
		"{\"event\":\"mem2_integrity\",\"bad\":%u,\"addr\":%u}",
		mem2_diag_bad(), __atomic_load_n(&s_diag_bad_addr, __ATOMIC_RELAXED));
	(void)hw_telemetry_emit_watchdog_start(line); /* ring only, no wake/IOS */
}
static bool mem2_diag_validate_locked(void) {
	if(mem2_diag_bad()) return false;
	mem2_diag_begin(MEM2_DIAG_VALIDATE);
	uint64_t start = gettime();
	struct mem2_heap_check check;
	bool ok = mem2_heap_validate(&mem2_heap,
		(heap_block*)&mem2_heap.start, (heap_block*)&mem2_heap.final,
		(uintptr_t)mem2_base, mem2_carve_size, &check);
	uint32_t us = (uint32_t)ticks_to_microsecs(gettime() - start);
	__atomic_fetch_add(&s_diag_scan_count, 1, __ATOMIC_RELAXED);
	uint32_t total = __atomic_load_n(&s_diag_scan_total_us, __ATOMIC_RELAXED);
	__atomic_store_n(&s_diag_scan_total_us,
		UINT32_MAX - total < us ? UINT32_MAX : total + us, __ATOMIC_RELAXED);
	uint32_t max = __atomic_load_n(&s_diag_scan_max_us, __ATOMIC_RELAXED);
	if(us > max) __atomic_store_n(&s_diag_scan_max_us, us, __ATOMIC_RELAXED);
	if(!ok) {
		__atomic_store_n(&s_diag_bad_addr, (uint32_t)check.address,
			__ATOMIC_RELAXED);
		__atomic_store_n(&s_diag_bad, (uint32_t)check.bad, __ATOMIC_RELEASE);
	}
	mem2_diag_end();
	return ok;
}
static void* mem2_diag_allocate_locked(size_t bytes, uint32_t op) {
	if(mem2_diag_bad()) return NULL;
	mem2_diag_begin(op);
	void* p = __lwp_heap_allocate(&mem2_heap, bytes);
	mem2_diag_end();
	return p;
}
static void mem2_diag_free_locked(void* p, uint32_t op) {
	if(mem2_diag_bad()) return;
	mem2_diag_begin(op);
	(void)__lwp_heap_free(&mem2_heap, p);
	mem2_diag_end();
}
/* colcap-1006 R3 item 4: the same free, reporting whether libogc accepted
 * the block (a refused free changes nothing in the heap). */
static bool mem2_diag_free_checked_locked(void* p, uint32_t op) {
	if(mem2_diag_bad()) return false;
	mem2_diag_begin(op);
	bool ok = __lwp_heap_free(&mem2_heap, p);
	mem2_diag_end();
	return ok;
}

// The audio PCM region: reserved beside the carve at init, OUTSIDE the lwp heap,
// handed out once by mem2_audio_region. Never freed and never part of the heap,
// so mem2_free's [mem2_base, mem2_base+mem2_carve_size) range test correctly
// refuses it -- passing an audio pointer to mem2_free would reach free() on a
// non-malloc block, so nothing may ever do that.
static uint8_t* mem2_audio_base = NULL;
static size_t mem2_audio_reserved = 0; // reserved at init (0 = none available)
static size_t mem2_audio_granted = 0;  // handed out (<= reserved)
static bool mem2_audio_taken = false;

// HEAP RED-ZONE. The real-Wii freeze is a CPU wedge that lands in a DIFFERENT subsystem each
// run (main=GFX_MQRECV, then GFX_DRAWDONE, then LIGHTING), with the GP idle, the VI/decrementer
// IRQs still firing (the watchdog fires + the VI callback completes), and every memory counter
// clean -- the textbook signature of a heap block-header SMASH: a fixed-distance buffer OVERRUN
// writing past one lwp_heap block into the NEXT block's boundary tag, corrupting the free-list /
// a libogc waiter node so the next blocking wait never wakes (a lost wakeup, not an allocator
// spin -- that would run IRQs-off and the watchdog could never report). A 26-agent adversarial
// audit + a hand audit could NOT pin the exact write statically: every concrete emit/copy path
// is bounds-correct in isolation. So this red-zone over-allocates EVERY mem2 block by MEM2_GUARD
// bytes of trailing slack: a bounded overrun lands in dead space instead of the next block's
// header. It is output-preserving (callers get their requested bytes unchanged; the user pointer
// is still the block base, so free() is unchanged -- no header, no alignment risk) and doubles as
// the hypothesis TEST: freeze stops => it was a bounded MEM2-heap overrun (next: a canary to name
// the exact site); freeze persists => corruption is elsewhere (the newlib MEM1 heap, or a non-
// overrun cause). Cost: 256B per live alloc, well under 1MB vs the 40MB carve (spill=0 headroom).
// Per CLAUDE.md this is hardening PAIRED with diagnosis, not a silent band-aid -- the root-cause
// hunt continues; this both possibly-fixes and narrows it in one hardware run.
#define MEM2_GUARD 256

// ACTIVE CANARY (the red-zone's promised "next step"): the guard slack is now
// PATTERNED at alloc and VERIFIED at every size-aware release (mem2_free_sized:
// display lists, chunk block buffers, server chunk arrays) and at realloc.
// A bounded overrun that lands in the guard no longer just vanishes into dead
// space -- it is COUNTED and the first offender (block, size, offset) is
// recorded, surfaced on the F3 far row and in the watchdog hang report. That
// converts the "stray heap writer" hypothesis from un-attributable freeze to a
// named site on the very run it happens. Wii-only: the PC mem2_alloc is plain
// malloc with no guard bytes (ASan owns that platform).
#define MEM2_CANARY_BYTE 0xA5

static uint32_t s_canary_hits = 0;
static void* s_canary_first_ptr = NULL;
static size_t s_canary_first_size = 0;
static size_t s_canary_first_off = 0;

static void mem2_canary_fill(void* p, size_t size) {
	if(p)
		memset((uint8_t*)p + size, MEM2_CANARY_BYTE, MEM2_GUARD);
}

static void mem2_canary_verify(void* p, size_t size) {
	if(!p)
		return;
	const uint8_t* g = (const uint8_t*)p + size;
	for(size_t i = 0; i < MEM2_GUARD; i++) {
		if(g[i] != MEM2_CANARY_BYTE) {
			if(s_canary_hits++ == 0) {
				s_canary_first_ptr = p;
				s_canary_first_size = size;
				s_canary_first_off = i;
			}
			return;
		}
	}
}

uint32_t mem2_canary_hits(void) {
	return s_canary_hits;
}

void mem2_canary_first(void** ptr, size_t* size, size_t* guard_off) {
	if(ptr)
		*ptr = s_canary_first_ptr;
	if(size)
		*size = s_canary_first_size;
	if(guard_off)
		*guard_off = s_canary_first_off;
}

// All three mem2 entry points run CONCURRENTLY on the single Broadway core: the server
// thread (worldgen_alloc_chunk + chunk destroy, ~10 ops/streamed-or-evicted chunk), the
// mesher worker (display-list grow), and the main/render thread (chunk buffers + display
// list frees). libogc's __lwp_heap_* serialise each individual splice with interrupts
// off, but mem2_realloc spans alloc+memcpy+free as THREE separate ops, and any
// cross-thread corruption (or a single-threaded double-free) seeds a CYCLIC free-list
// that the next allocate/free then walks FOREVER with interrupts disabled -> a frozen,
// no-exception, screen-stays-lit hang (the real-Wii crash observed after ~5 min). Serialise
// the whole alloc/free/realloc. Recursive because realloc re-enters alloc+free. The ready
// flag keeps the pre-thread / pre-mem2_init single-threaded allocs lock-free and safe.
/* colcap-1006 R5 item 3 (mem2.h, mem2_cost_take): cumulative since the last
 * take, in timebase ticks; written only with mem2_lock held. */
static uint32_t s_cost_n[3];
static uint64_t s_cost_ticks[3], s_cost_max[3];
static inline void mem2_cost_note_locked(unsigned k, uint64_t ticks) {
	s_cost_n[k]++;
	s_cost_ticks[k] += ticks;
	if(ticks > s_cost_max[k])
		s_cost_max[k] = ticks;
}
static mutex_t s_mem2_mutex;
static bool s_mem2_lock_ready = false;
static inline void mem2_lock(void) {
	if(!s_mem2_lock_ready)
		return;
	// P2-c telemetry: try the uncontended path first so contention can be
	// COUNTED rather than only felt as latency. LWP_MutexTryLock returns 0 on
	// success (lock taken, no further call needed) or nonzero if another
	// thread already holds it, in which case we count the miss and fall back
	// to the normal blocking acquire. One extra libogc call in the
	// uncontended common case; negligible next to the heap-splice work every
	// mem2_alloc/free/realloc already does under this same lock.
	if(LWP_MutexTryLock(s_mem2_mutex) == 0)
		return;
	__atomic_fetch_add(&s_mem2_lock_wait, 1, __ATOMIC_RELAXED);
	tps_note(mem2_tps_observe, TPS_ENTER, TPS_MEM2_WAIT);
	SETTLE(mem2_settle_observe, SET_ALLOC_BEGIN, NULL, NULL, 0, 0, 0, 0);
	const uint64_t wait0 = gettime();
	LWP_MutexLock(s_mem2_mutex);
	mem2_cost_note_locked(2, gettime() - wait0); /* R5 item 3: lock held */
	SETTLE(mem2_settle_observe, SET_ALLOC_END, NULL, NULL, 0, 0, 0, 0);
	tps_note(mem2_tps_observe, TPS_LEAVE, TPS_MEM2_WAIT);
}
static inline void mem2_unlock(void) {
	if(s_mem2_lock_ready)
		LWP_MutexUnlock(s_mem2_mutex);
}

/* Slot stride keeps the red-zone guard so mem2_canary_fill/_verify are
 * byte-identical on a pooled pointer; both strides are already 32 B aligned
 * (10,496 and 2,368), which the asserts below pin. */
#define MEM2_POOL_FAR_STRIDE (MEM2_POOL_FAR_OBJECT + MEM2_GUARD)
#define MEM2_POOL_VIEW_STRIDE (MEM2_POOL_VIEW_OBJECT + MEM2_GUARD)
#define MEM2_POOL_COLUMN_STRIDE (MEM2_POOL_COLUMN_OBJECT + MEM2_GUARD)
_Static_assert(MEM2_POOL_FAR_STRIDE % 32u == 0
	&& MEM2_POOL_VIEW_STRIDE % 32u == 0, "pool slot stride must be GX aligned");

static struct mem2_pool s_pool_far, s_pool_view, s_pool_column;
/* colcap-1006 R3 item 4: column-slab hand-backs the heap refused. */
static uint32_t s_column_shrink_fail;
/* colcap-1006 (R4 point 9): ordered grow/shrink events (mem2_pool_evq.h). */
static struct mem2_pool_evq s_column_evq;
/* colcap-1006 R5 item 2 (mem2.h, COLUMN-POOL SELF-TEST HOOKS). Touched only
 * under mem2_lock; all zero = production behaviour. */
static bool s_st_armed, s_st_rotating;
static unsigned s_st_refuse;
static uint32_t s_st_digest = 2166136261u, s_st_takes;

/* The pool a TAGGED request names, or NULL. Keyed by TENANT, never by size:
 * 10,240 B is every chunk's block buffer, so a size-keyed lookup would let the
 * near field drain the far pool (see mem2.h). The object size is still checked,
 * so a tenant whose object grows silently falls back to the shared heap instead
 * of overrunning a slot. */
static struct mem2_pool* mem2_pool_by_id(enum mem2_pool_id id, size_t size) {
	if(id == MEM2_POOL_FAR && size == MEM2_POOL_FAR_OBJECT)
		return &s_pool_far;
	if(id == MEM2_POOL_VIEW && size == MEM2_POOL_VIEW_OBJECT)
		return &s_pool_view;
	if(id == MEM2_POOL_COLUMN && size == MEM2_POOL_COLUMN_OBJECT)
		return &s_pool_column;
	return NULL;
}

static struct mem2_pool* mem2_pool_holding(const void* ptr) {
	if(mem2_pool_owns(&s_pool_far, ptr))
		return &s_pool_far;
	if(mem2_pool_owns(&s_pool_view, ptr))
		return &s_pool_view;
	if(mem2_pool_owns(&s_pool_column, ptr))
		return &s_pool_column;
	return NULL;
}

static void mem2_pool_stat_fill(struct mem2_pool_stat* out,
								const struct mem2_pool* p) {
	/* slots = the CAP, so an existing reader of pool_far_slots still sees the
	 * bound the pool will never exceed; slabs / bytes say how much of it this
	 * scene has actually carved (mem2.h, DEMAND-CARVED, IN SLABS). */
	out->slots = mem2_pool_slots_max(p);
	out->used = p->used;
	out->peak = p->peak;
	out->overflow = p->overflow;
	out->slabs = mem2_pool_slabs(p);
	out->slabs_max = mem2_pool_slabs_max(p);
	out->carve_fail = p->carve_fail;
	out->bytes = mem2_pool_bytes(p);
	out->shrinks = p->shrinks;
	out->shrink_fail = p == &s_pool_column ? s_column_shrink_fail : 0;
}

void mem2_pool_stats(struct mem2_pool_stat* far_out,
					 struct mem2_pool_stat* view_out) {
	mem2_lock();
	if(far_out)
		mem2_pool_stat_fill(far_out, &s_pool_far);
	if(view_out)
		mem2_pool_stat_fill(view_out, &s_pool_view);
	mem2_unlock();
}

static void mem2_pools_publish(void);

static bool mem2_carve_largest_probe(uint32_t* bytes); /* colcap-1006 */
void mem2_init(void) {
	// Init the serialising mutex once, BEFORE the mem2_ready early-return, so it exists
	// even if the MEM2 carve below bails (the malloc-fallback path is shared too).
	if(!s_mem2_lock_ready && LWP_MutexInit(&s_mem2_mutex, true) == 0)
		s_mem2_lock_ready = true;

	if(mem2_ready)
		return;

	// arena2 IS the malloc heap's sbrk region under the MALLOC_MEM2=1 pin above
	// (not a "spill target once MEM1 is full" -- there is no cross-arena spill).
	// Called early enough that the heap's first sbrk has not claimed from it
	// yet, so [lo,hi) is the whole untouched arena.
	uint8_t* lo = (uint8_t*)SYS_GetArena2Lo();
	uint8_t* hi = (uint8_t*)SYS_GetArena2Hi();
	if(!lo || !hi || hi <= lo)
		return;
	// Align the base ourselves rather than assume libogc handed us an aligned arena:
	// __lwp_heap_init re-aligns its own start internally, but the audio region is raw
	// bytes at lo + audio_off and its alignment is only as good as lo's.
	lo = (uint8_t*)(((uintptr_t)lo + (MEM2_REGION_ALIGN - 1))
					& ~(uintptr_t)(MEM2_REGION_ALIGN - 1));
	if(hi <= lo)
		return;

	// CLAMP both regions to what arena2 actually has instead of bailing if the wants
	// do not fit. A silent bail would set mem2_ready=false and route EVERY alloc to
	// the general malloc heap (MEM1 on the June toolchain -- the exact disaster the
	// spill counter exposed; arena2 under today's pin, which still abandons the
	// carve's GX-alignment and churn discipline) -- so always take the largest
	// carve that fits, then give audio the remainder (mem2_plan_regions carries
	// the ordering argument).
	struct mem2_region_plan plan;
	if(!mem2_plan_regions((size_t)(hi - lo), MEM2_HEAP_SIZE,
						  MEM2_AUDIO_REGION_BYTES, MEM2_RESERVE_MARGIN, &plan))
		return;

	// Build a libogc heap over the carve (32-byte page = GX alignment), then move
	// arena2's start above BOTH regions so the malloc heap's sbrk can never claim
	// either.
	// Order matters: init the heap on the bytes we are about to fence off, THEN
	// fence them off. (The old, reverted attempt bumped arena2 lo and wrote into
	// the region without a real heap, which faulted.) The audio region needs no
	// init -- it is raw bytes for a bump allocator, and it is fenced by the same
	// single SYS_SetArena2Lo call, so it can never be half-published.
	if(__lwp_heap_init(&mem2_heap, lo + plan.carve_off, plan.carve_size, 32) == 0)
		return; // heap too small to hold any block

	SYS_SetArena2Lo((void*)(lo + plan.reserved_total));
	mem2_base = lo + plan.carve_off;
	mem2_carve_size = plan.carve_size;
	mem2_audio_base = plan.audio_size ? lo + plan.audio_off : NULL;
	mem2_audio_reserved = plan.audio_size;
	mem2_ready = true;
	mem2_pools_publish();
	hw_telemetry_set_carve_largest_probe(mem2_carve_largest_probe);
}

/* Publish the two fixed-tenant pools (mem2.h) EMPTY. Nothing is allocated here:
 * round 2 reserved both whole out of a virgin heap and real Wii 703aef00
 * measured the cost -- 2.35 MB of far pool sitting at 0/224 slots at t30 while
 * the shared heap was down to 94 KB free and a 928 B largest block. A slab is
 * now carved on the first take that finds no free slot (mem2_allocate_pooled),
 * so the heap keeps every byte the scene has not asked for yet. Called once
 * from mem2_init, before any thread exists, so it needs no lock; the carve
 * hook it installs IS called under mem2_lock afterwards.
 *
 * Each slab asks the heap for its payload + MEM2_GUARD. That guard is NOT
 * slack: every other block in this heap carries a red zone, and the observer
 * reconciles `used` against `payload + blocks * 256` (carve_observer.c
 * snapshot_locked), so a slab without one would skew the overhead figure by
 * 256 B per slab for no reason.
 *
 * The guard is placed at the FRONT (mem2_pool_init's `lead`, MEM2_POOL_LEAD
 * below), not the tail: the carve ledger is keyed by address alone, so a slab
 * whose slot 0 started at the slab's own address would make the first take
 * collide with the slab's reservation record and the first release ERASE it --
 * see LEDGER IDENTITY in mem2_pool.h for the real-Wii measurement. Same bytes,
 * same accounting, disjoint addresses. */
#define MEM2_POOL_LEAD MEM2_GUARD
_Static_assert(MEM2_POOL_LEAD % 32u == 0 && MEM2_POOL_LEAD > 0,
	"pool lead must be non-zero and keep slot 0 GX aligned");
_Static_assert(MEM2_POOL_FAR_SLABS * MEM2_POOL_SLAB_SLOTS == MEM2_POOL_FAR_SLOTS
	&& MEM2_POOL_VIEW_SLABS * MEM2_POOL_SLAB_SLOTS == MEM2_POOL_VIEW_SLOTS,
	"slot caps must be a whole number of slabs");

/* The pool's demand carve. Called from inside mem2_allocate_pooled with
 * mem2_lock already held, which is the same lock every __lwp_heap_allocate in
 * this file runs under -- so this is an ordinary heap allocation made at the
 * moment the scene proves it needs one, not a re-entrant path. */
static void* mem2_pool_heap_carve(size_t bytes, void* ctx) {
	(void)ctx;
	return mem2_ready ? mem2_diag_allocate_locked(bytes, MEM2_DIAG_POOL) : NULL;
}

static void mem2_pool_note_slab(const struct mem2_pool* p, const void* block);

static void mem2_pools_publish(void) {
	mem2_pool_init(&s_pool_far, MEM2_POOL_LEAD, MEM2_POOL_FAR_OBJECT,
		MEM2_POOL_FAR_STRIDE, MEM2_POOL_FAR_SLABS, mem2_pool_heap_carve, NULL);
	mem2_pool_init(&s_pool_view, MEM2_POOL_LEAD, MEM2_POOL_VIEW_OBJECT,
		MEM2_POOL_VIEW_STRIDE, MEM2_POOL_VIEW_SLABS, mem2_pool_heap_carve,
		NULL);
	mem2_pool_init(&s_pool_column, MEM2_POOL_LEAD, MEM2_POOL_COLUMN_OBJECT,
		MEM2_POOL_COLUMN_STRIDE, MEM2_POOL_COLUMN_SLABS, mem2_pool_heap_carve,
		NULL);
	/* Eager boot slabs (see mem2.h MEM2_POOL_COLUMN_BOOT_SLABS): the heap is
	 * virgin here, so the contiguous 2.6 MB per slab is guaranteed; a slab
	 * requested late from a fragmented heap is exactly what a demand-grown
	 * pool cannot promise (Astra debate 2026-09-11, E). */
	for(unsigned k = 0; k < MEM2_POOL_COLUMN_BOOT_SLABS; k++) {
		const void* slab = mem2_pool_grow(&s_pool_column);
		if(!slab)
			break;
		mem2_pool_note_slab(&s_pool_column, slab);
	}
}

void mem2_pool_column_stats(struct mem2_pool_stat* out) {
	mem2_lock();
	mem2_pool_stat_fill(out, &s_pool_column);
	mem2_unlock();
}

/* colcap-1006 (ASTRA-CC-R3 item 3): <= MEM2_POOL_MAX_SLABS popcounts under the
 * lock the heartbeat already takes for the stats -- one acquisition, a few
 * dozen instructions, once per heartbeat. */
unsigned mem2_pool_column_occupancy(struct mem2_pool_stat* out,
									uint8_t* per_slab, unsigned cap,
									uint32_t* event_seq, uint32_t* events_lost) {
	mem2_lock();
	mem2_pool_stat_fill(out, &s_pool_column);
	if(event_seq)
		*event_seq = s_column_evq.seq;
	if(events_lost)
		*events_lost = s_column_evq.lost;
	unsigned n = mem2_pool_slabs(&s_pool_column);
	if(n > cap)
		n = cap;
	for(unsigned i = 0; i < n; i++)
		per_slab[i] = (uint8_t)mem2_pool_slab_used(&s_pool_column, i);
	mem2_unlock();
	return n;
}

/* Attribute ONE slab to CARVE_SLOTS. Emitted inside a caller-owned
 * CARVE_BEGIN/CARVE_END window (the observer's lock is not recursive), which is
 * why this helper does not open one of its own.
 *
 * Key the record on the SLAB's own address, not on its slot 0 (mem2_pool.h,
 * LEDGER IDENTITY): the ledger is address-keyed, so noting it at base -- which
 * is what slot 0 would be handed out as -- made the first take collide and the
 * first release erase this record along with its megabytes. */
static void mem2_pool_note_slab(const struct mem2_pool* p, const void* block) {
	if(!block)
		return;
	size_t bytes = mem2_pool_slab_bytes(p);
	carve_note(mem2_carve_observe, CARVE_ALLOC, (uintptr_t)block, 0, bytes,
		CARVE_MEM2 | CARVE_IN_HEAP);
	carve_note(mem2_carve_observe, CARVE_TAG, (uintptr_t)block, 0, bytes,
		CARVE_SLOTS);
}

/* Replay every slab carved so far into a freshly armed observer. Called from
 * carve_observer_start: a slab carved before an observer existed would
 * otherwise be invisible -- its bytes gone from the heap's free bytes with no
 * tenant owning them, because the per-tenant totals only ever see the SLOTS
 * (tagged CARVE_FAR/CARVE_VIEW by their own call sites), never the slab.
 * Re-arming replays the slabs the pool still holds, which is correct: a
 * column slab handed back (R3 item 4) was noted CARVE_FREE and is gone. */
static void mem2_pool_note_all(const struct mem2_pool* p) {
	unsigned n = mem2_pool_slabs(p);
	if(!n)
		return; /* empty pool: nothing carved yet, nothing to attribute */
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	for(unsigned i = 0; i < n; i++)
		mem2_pool_note_slab(p, mem2_pool_slab_block(p, i));
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}

void mem2_pool_note_reservations(void) {
	mem2_lock();
	mem2_pool_note_all(&s_pool_far);
	mem2_pool_note_all(&s_pool_view);
	mem2_pool_note_all(&s_pool_column); /* eager boot slabs (Astra P2) */
	mem2_unlock();
}

void* mem2_audio_region(size_t want, size_t* out_size) {
	// FIRST CALLER WINS (mem2.h): latch the grant so audio_registry_reload gets
	// the identical block back and re-fills it in place. No lock -- main thread
	// only, once, at init, before the worker threads exist.
	if(!mem2_audio_taken) {
		mem2_audio_taken = true;
		mem2_audio_granted
			= mem2_audio_reserved < want ? mem2_audio_reserved : want;
		if(!mem2_audio_base)
			mem2_audio_granted = 0;
	}
	if(out_size)
		*out_size = mem2_audio_granted;
	return mem2_audio_granted ? mem2_audio_base : NULL;
}

size_t mem2_stat_carve_size(void) { return mem2_carve_size; }
size_t mem2_stat_audio_region_size(void) { return mem2_audio_reserved; }

// T-c (2026-09-03, Round 3 (2)(a) MEM2 gauge fix): live free-bytes reading of
// the carve heap itself, via the SAME __lwp_heap_getinfo call input.c already
// uses for the lwp kernel workspace gauge (input_lwp_wkspace_free) -- same
// "== 0 means success" return convention, same codebase precedent. Locked
// like every other mem2_heap access: the mesher/server threads can be mid-
// alloc/free/realloc (which splice the heap's internal free-list) at the
// exact moment the heartbeat builder on the main thread calls this.
// F4 (mp-split-0912): the exact-bytes primitive. SIZE_MAX == "no carve to
// measure" (contract in mem2.h) -- here that is an uninitialised carve or a
// getinfo that failed, both of which must read as UNKNOWN rather than as
// zero free bytes: a caller gating an allocation on this would otherwise
// refuse everything on a console whose heap it simply could not sample.
size_t mem2_carve_free_bytes(void) {
	if(!mem2_ready)
		return SIZE_MAX;
	heap_iblock info;
	mem2_lock();
	bool valid = mem2_diag_validate_locked();
	bool ok = false;
	if(valid) {
		mem2_diag_begin(MEM2_DIAG_GETINFO);
		ok = __lwp_heap_getinfo(&mem2_heap, &info) == 0;
		mem2_diag_end();
	}
	mem2_unlock();
	if(!valid) mem2_diag_emit_bad();
	return ok ? (size_t)info.free_size : SIZE_MAX;
}

// colcap-1006 (ASTRA-CC-R2 item 5): ONE bounded physical boundary-tag walk
// (the same front_flag size/used encoding mem2_carve_snapshot walks; no
// free-list mutation) returning the summed free bytes and the largest free
// block's PHYSICAL size. Caller holds mem2_lock. Valid only when the walk
// lands exactly on the heap's final block -- a size of 0 or one running past
// it means the tags cannot be trusted, and the reading says so instead of
// reporting a partial maximum.
static bool mem2_carve_walk_free_locked(size_t* free_total, size_t* largest) {
	*free_total = 0;
	*largest = 0;
	if(!mem2_ready || mem2_diag_bad())
		return false;
	/* Sol review #1: reuse the structural validator mem2_carve_snapshot uses
	 * (bounds, minimum size, alignment, back tags, free list); a corrupt heap
	 * is INVALID here and is never walked further. */
	if(!mem2_diag_validate_locked())
		return false;
	mem2_diag_begin(MEM2_DIAG_GETINFO); /* a stall here names the heap walk */
	bool ok = mem2_heap_free_extent_raw((uintptr_t)mem2_heap.start,
		(uintptr_t)mem2_heap.final, free_total, largest);
	mem2_diag_end();
	return ok;
}
// The libogc request rule (mem2.h mem2_lwp_allocatable_from_block) applied
// with mem2_heap's OWN page size and libogc's HEAP_BLOCK_USED_OVERHEAD.
static size_t mem2_carve_allocatable_of(size_t block) {
	return mem2_lwp_allocatable_from_block(block, mem2_heap.pg_size,
										   HEAP_BLOCK_USED_OVERHEAD);
}
bool mem2_carve_largest_allocatable_bytes(size_t* out_bytes) {
	size_t free_total = 0, largest = 0;
	mem2_lock();
	bool ok = mem2_carve_walk_free_locked(&free_total, &largest);
	mem2_unlock();
	if(!ok)
		mem2_diag_emit_bad(); /* no-op unless the validator flagged the heap */
	if(out_bytes)
		*out_bytes = ok ? mem2_carve_allocatable_of(largest) : 0;
	return ok;
}
// hw_telemetry's heartbeat probe signature (hw_telemetry_set_carve_largest_probe).
static bool mem2_carve_largest_probe(uint32_t* bytes) {
	size_t v = 0;
	bool ok = mem2_carve_largest_allocatable_bytes(&v);
	*bytes = ok ? (v > UINT32_MAX ? UINT32_MAX : (uint32_t)v) : 0;
	return ok;
}

// Derived from the primitive above so there is ONE getinfo call site and one
// lock discipline. Contract unchanged: KB, and -1 for "no reading".
int mem2_stat_carve_free_kb(void) {
	size_t free_bytes = mem2_carve_free_bytes();
	return free_bytes == SIZE_MAX ? -1 : (int)(free_bytes / 1024u);
}

// colcap-1006 growth-event state, touched only under mem2_lock. Grow and
// shrink events share ONE ordered queue (mem2_pool_evq.h, R4 point 9): the
// sequence number is assigned when the event is queued under this lock and
// the queue is drained oldest-first outside it, so the wire order is the
// sequence order whichever thread changed the pool.
static unsigned s_column_grow_fail_slabs = UINT_MAX;

static void mem2_lock_cb(void) { mem2_lock(); }
static void mem2_unlock_cb(void) { mem2_unlock(); }
static bool mem2_column_event_emit(const struct mem2_pool_ev* ev, void* user) {
	(void)user;
	return ev->kind == MEM2_POOL_EV_GROW
		? hw_telemetry_emit_column_pool_grow(&ev->grow)
		: hw_telemetry_emit_column_pool_shrink(&ev->shrink);
}
/* Sol review R4: delivery is NOT on the allocation path -- the Wii pays
 * nothing per alloc/free. One bounded flush per heartbeat (main.c) delivers
 * up to MEM2_POOL_EVQ_FLUSH_BUDGET events; pool changes are a handful per
 * session, the 16-slot queue absorbs a burst, and an overflow is counted. */
#define MEM2_POOL_EVQ_FLUSH_BUDGET 8u
void mem2_pool_column_events_flush(void) {
	/* No live telemetry: leave the queue alone (it fills, `lost` counts the
	 * overflow) instead of formatting a line nobody can receive. */
	if(mem2_pool_evq_pending(&s_column_evq) && hw_telemetry_active())
		(void)mem2_pool_evq_publish(&s_column_evq, mem2_lock_cb,
			mem2_unlock_cb, mem2_column_event_emit, NULL,
			MEM2_POOL_EVQ_FLUSH_BUDGET);
}

/* R5 item 2 hooks (mem2.h). The digest folds (slab, slot) of each column
 * take made while armed, under the allocator lock. */
static void mem2_pool_selftest_note_take_locked(const struct mem2_pool* p,
												const void* ptr) {
	int slab = mem2_pool_slab_of(p, ptr);
	uint32_t slot = slab < 0 ? 0xffu : (uint32_t)(((const uint8_t*)ptr
		- p->base[slab]) / p->stride);
	uint32_t key = ((uint32_t)(slab & 0xff) << 8) | (slot & 0xffu);
	for(unsigned k = 0; k < 2; k++) {
		s_st_digest ^= (key >> (8 * k)) & 0xffu;
		s_st_digest *= 16777619u;
	}
	s_st_takes++;
}
bool mem2_pool_selftest_arm(enum mem2_pool_placement placement) {
	if(!mem2_ready)
		return false;
	mem2_lock();
	s_st_armed = true;
	s_st_rotating = placement == MEM2_POOL_PLACE_ROTATING;
	s_st_digest = 2166136261u;
	s_st_takes = 0;
	mem2_unlock();
	return true;
}
bool mem2_pool_selftest_armed(void) { return s_st_armed; }
void mem2_pool_selftest_refuse_handbacks(unsigned count) {
	mem2_lock();
	if(s_st_armed)
		s_st_refuse = count;
	mem2_unlock();
}
unsigned mem2_pool_selftest_refusals_pending(void) {
	mem2_lock();
	unsigned n = s_st_refuse;
	mem2_unlock();
	return n;
}
bool mem2_pool_column_locate(const void* ptr, int* slab, int* slot) {
	mem2_lock();
	int s = mem2_pool_slab_of(&s_pool_column, ptr);
	bool live = false;
	if(s >= 0) {
		unsigned bit = (unsigned)(((const uint8_t*)ptr - s_pool_column.base[s])
								  / s_pool_column.stride);
		live = (s_pool_column.bits[s] >> bit) & 1u;
		if(slab) *slab = s;
		if(slot) *slot = (int)bit;
	}
	mem2_unlock();
	return live;
}
uint32_t mem2_pool_selftest_placement_digest(uint32_t* takes) {
	mem2_lock();
	uint32_t d = s_st_digest;
	if(takes) *takes = s_st_takes;
	mem2_unlock();
	return d;
}
unsigned mem2_pool_selftest_stats(struct mem2_pool_stat* out,
								  uint8_t* per_slab, unsigned cap) {
	return mem2_pool_column_occupancy(out, per_slab, cap, NULL, NULL);
}

static void* mem2_allocate_pooled(size_t size, bool fallback,
								  enum mem2_pool_id pool_id) {
	const uint64_t cost0 = pool_id == MEM2_POOL_COLUMN ? gettime() : 0;
	mem2_lock();
	if(mem2_diag_bad()) { mem2_unlock(); return NULL; }
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	s_mem2_alloc_calls++;
	s_mem2_alloc_bytes += size;
	void* p = NULL;
	size_t guarded = size + MEM2_GUARD; // red-zone slack absorbs a bounded overrun (see above)
	// FIXED-TENANT POOL (mem2.h) first: a request from a pool's OWN TENANT gets
	// a bitmap slot instead of a first-fit block, so it never lands between two
	// display lists. Untagged callers (pool_id == MEM2_POOL_NONE) never see a
	// slot even at the same size. A full pool is NOT an error -- it falls
	// through to the shared heap below and is counted, so the census shows a
	// pool that is undersized for the scene rather than silently reverting.
	// DEMAND CARVE (round 3): an empty or full pool below its slab cap carves
	// ONE more slab here, at the moment the scene proves it needs it, and the
	// new slab is attributed to CARVE_SLOTS immediately -- inside the
	// CARVE_BEGIN window this call already opened, because the observer's lock
	// is not recursive. Past the cap, or when the heap cannot spare a slab, the
	// request falls through to the shared heap and is counted (carve_fail
	// separates the two, which are different findings).
	struct mem2_pool* pool = mem2_pool_by_id(pool_id, size);
	// colcap-1006 (ASTRA-CC-R2 item 5): a COLUMN-pool demand carve is captured
	// HERE, under this lock, at the allocation instant (a periodic largest-hole
	// sample can miss it), and published after mem2_unlock below -- no
	// formatting or telemetry I/O inside the lock. The heap walk runs only on
	// a carve attempt the event will publish: every success (at most one per
	// slab, a handful per session) and the FIRST failure at a given slab count
	// (repeat failures at that count are already in carve_fail). An ordinary
	// pooled take or a full pool never walks.
	struct mem2_pool_ev grow_ev = {.kind = MEM2_POOL_EV_GROW};
	/* R4 points 2-3: the COLUMN pool places lowest-index-first; far/view
	 * keep the rotating take. */
	void* (*take)(struct mem2_pool*) = pool == &s_pool_column && !s_st_rotating
		? mem2_pool_take_lowest : mem2_pool_take;
	if(pool) {
		p = take(pool);
		if(!p) {
			const unsigned slabs_before = mem2_pool_slabs(pool);
			/* Sol review #2: every carve attempt below the slab cap is
			 * captured, so a SUCCESS is always published (also a retry after
			 * an earlier failure at the same slab count); only a repeated
			 * FAILURE at one count is not re-published (carve_fail counts
			 * it). Cost: one validate + walk on this pool-exhausted path,
			 * which already pays a failed first-fit walk (mem2_pool_grow);
			 * an ordinary pooled take never walks. */
			const bool grow_watch = pool == &s_pool_column && mem2_ready
				&& slabs_before < mem2_pool_slabs_max(pool);
			struct hw_column_pool_grow* g = &grow_ev.grow;
			if(grow_watch) {
				size_t free_before = 0, largest_before = 0;
				g->before_valid = mem2_carve_walk_free_locked(&free_before,
					&largest_before);
				g->free_bytes_before = g->before_valid
					? (uint32_t)free_before : 0u;
				g->largest_allocatable_bytes_before = g->before_valid
					? (uint32_t)mem2_carve_allocatable_of(largest_before) : 0u;
				g->requested_bytes
					= (uint32_t)(pool->lead + mem2_pool_slab_bytes(pool));
				g->slabs_before = slabs_before;
			}
			const void* slab = mem2_pool_grow(pool);
			if(grow_watch) {
				g->success = slab != NULL;
				g->slabs_after = mem2_pool_slabs(pool);
				/* Queued (and numbered) only when it is news: every success,
				 * and the first failure at a slab count. A repeat failure
				 * takes no seq, so a delivered-seq gap always means loss. */
				if(slab != NULL || slabs_before != s_column_grow_fail_slabs)
					(void)mem2_pool_evq_push_locked(&s_column_evq, &grow_ev);
				s_column_grow_fail_slabs = slab ? UINT_MAX : slabs_before;
			}
			if(slab) {
				mem2_pool_note_slab(pool, slab);
				p = take(pool);
			}
			if(!p)
				mem2_pool_note_overflow(pool,
					mem2_pool_slabs(pool) < mem2_pool_slabs_max(pool));
		}
		if(p && s_st_armed && pool == &s_pool_column)
			mem2_pool_selftest_note_take_locked(pool, p);
	}
	bool pooled = p != NULL;
	if(!p && mem2_ready)
		p = mem2_diag_allocate_locked(guarded, MEM2_DIAG_ALLOC);
	bool in_carve = p != NULL;
	// Fallback: the normal 32-byte-aligned heap (MEM1 first on Wii) when the MEM2 heap
	// is exhausted or not yet carved. Worst case the buffer lives where it did before.
	if(!p && fallback) {
		if(mem2_ready) {
			s_mem2_carve_spill++; // carve couldn't satisfy -> spilling to MEM1
			__atomic_store_n(&s_mem2_last_spill_bytes,
				guarded > UINT32_MAX ? UINT32_MAX : (uint32_t)guarded,
				__ATOMIC_RELAXED);
		}
		p = memalign(32, guarded);
		if(!p)
			s_mem2_oom++; // even the MEM1 fallback failed: a true NULL (mesh truncates)
	} else if(!p)
		s_mem2_oom++; // strict carve-only caller failed; it will remain dormant
	mem2_canary_fill(p, size); // pattern the guard (both carve + fallback paths)
	carve_note(mem2_carve_observe, CARVE_ALLOC, p, 0, size,
		CARVE_MEM2 | (in_carve ? CARVE_IN_HEAP : 0)
		| (pooled ? CARVE_POOLED : 0)
		| (fallback && mem2_ready && !in_carve ? CARVE_SPILLED : 0));
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	if(pool_id == MEM2_POOL_COLUMN)
		mem2_cost_note_locked(0, gettime() - cost0);
	mem2_unlock();
	return p;
}

void mem2_cost_take(uint32_t out[MEM2_COST_FIELDS]) {
	mem2_lock();
	for(unsigned k = 0; k < 3; k++) {
		uint64_t total = ticks_to_microsecs(s_cost_ticks[k]);
		uint64_t worst = ticks_to_microsecs(s_cost_max[k]);
		out[3 * k] = s_cost_n[k];
		out[3 * k + 1] = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
		out[3 * k + 2] = worst > UINT32_MAX ? UINT32_MAX : (uint32_t)worst;
		s_cost_n[k] = 0;
		s_cost_ticks[k] = s_cost_max[k] = 0;
	}
	mem2_unlock();
}

static void* mem2_allocate(size_t size, bool fallback) {
	return mem2_allocate_pooled(size, fallback, MEM2_POOL_NONE);
}
void* mem2_alloc(size_t size) { return mem2_allocate(size, true); }
void* mem2_alloc_strict(size_t size) { return mem2_allocate(size, false); }
void* mem2_alloc_pooled(size_t size, enum mem2_pool_id pool) {
	return mem2_allocate_pooled(size, true, pool);
}
bool mem2_column_pool_emulated(void) { return false; }

void mem2_free_sized(void* ptr, size_t size) {
	if(!ptr)
		return;
	mem2_lock();
	if(mem2_diag_bad()) { mem2_unlock(); return; }
	mem2_canary_verify(ptr, size);
	/* No pool bookkeeping here on purpose: a pooled pointer is recognised by
	 * RANGE in mem2_free below (which is what lets every existing free site
	 * stay unchanged), and a released heap block carries nothing that names the
	 * tenant that asked for it -- keying off `size` would reintroduce exactly
	 * the near/far confusion the tenant-keyed take path exists to avoid.
	 * See mem2_pool.h, NO LIVE-OVERFLOW COUNTER. */
	mem2_unlock();
	mem2_free(ptr);
}

/* colcap-1006 (ASTRA-CC-R3 item 4, option A, T1): after a column slot is
 * released, hand a WHOLLY EMPTY non-boot column slab back to the carve (see
 * mem2_pool.h SHRINK). Called with mem2_lock held, inside the caller's
 * CARVE_BEGIN window. The heap free comes FIRST and the pool detaches the
 * slab only once libogc accepted it -- a refused free (corrupt tags; the
 * validator would flag it) leaves the slab in the pool, still usable, and is
 * counted, so the fallback is "keep the slab", never a half-returned one.
 * Checked on EVERY column release, not only the one that empties a slab: a
 * slab that emptied while the pool lacked headroom goes back as soon as
 * demand falls far enough. At most ONE slab per call (bounded work under the
 * lock); the next column release returns the next. Cost: a scan of <= 9
 * bitmap words per column release, plus one __lwp_heap_free per hand-back. */
static void mem2_pool_column_try_shrink_locked(void) {
	if(!mem2_ready)
		return;
	const int s = mem2_pool_shrink_candidate(&s_pool_column,
		MEM2_POOL_COLUMN_BOOT_SLABS, MEM2_POOL_COLUMN_SHRINK_HEADROOM);
	if(s < 0)
		return;
	void* block = (void*)mem2_pool_slab_block(&s_pool_column, (unsigned)s);
	/* R5 item 2: a self-test-forced refusal takes EXACTLY the branch a heap
	 * that rejected the free takes (counted, slab kept, nothing detached). */
	if(s_st_refuse) {
		s_st_refuse--;
		s_column_shrink_fail++;
		return;
	}
	if(!block || !mem2_diag_free_checked_locked(block, MEM2_DIAG_POOL)) {
		s_column_shrink_fail++;
		return;
	}
	const unsigned before = mem2_pool_slabs(&s_pool_column);
	/* Cannot refuse: mem2_pool_shrink's only refusals (below `keep`, not
	 * carved, a live slot) are exactly what the candidate scan just ruled
	 * out under this same lock, and nothing ran in between. */
	(void)mem2_pool_shrink(&s_pool_column, (unsigned)s,
		MEM2_POOL_COLUMN_BOOT_SLABS);
	carve_note(mem2_carve_observe, CARVE_FREE, block, 0, 0, 0);
	struct mem2_pool_ev ev = {.kind = MEM2_POOL_EV_SHRINK};
	ev.shrink.slab = (uint32_t)s;
	ev.shrink.released_bytes
		= (uint32_t)(s_pool_column.lead + mem2_pool_slab_bytes(&s_pool_column));
	ev.shrink.slabs_before = before;
	ev.shrink.slabs_after = mem2_pool_slabs(&s_pool_column);
	(void)mem2_pool_evq_push_locked(&s_column_evq, &ev);
	/* The slab count changed: a later carve failure at any count is news. */
	s_column_grow_fail_slabs = UINT_MAX;
}

void mem2_free(void* ptr) {
	if(!ptr)
		return;
	const uint64_t cost0 = gettime(); /* R5 item 3 */
	mem2_lock();
	if(mem2_diag_bad()) { mem2_unlock(); return; }
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_FREE, ptr, 0, 0, 0);
	s_mem2_free_calls++;
	// Pointers that actually landed in our reserved MEM2 region go back to the
	// MEM2 heap; fallback allocations (and any plain malloc/memalign passed in)
	// go to the normal heap. The range test is what makes this safe to use for
	// the mixed buffers a mipmap build frees.
	// A pooled pointer is INSIDE the carve range, so the pool test must come
	// first: handing a slot to __lwp_heap_free would splice boundary tags
	// through a block the heap does not own.
	struct mem2_pool* pool = mem2_pool_holding(ptr);
	if(pool) {
		if(mem2_pool_release(pool, ptr) && pool == &s_pool_column)
			mem2_pool_column_try_shrink_locked();
	} else if(mem2_ready && (uint8_t*)ptr >= mem2_base
	   && (uint8_t*)ptr < mem2_base + mem2_carve_size)
		mem2_diag_free_locked(ptr, MEM2_DIAG_FREE);
	else
		free(ptr);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	if(pool == &s_pool_column)
		mem2_cost_note_locked(1, gettime() - cost0);
	mem2_unlock();
}

_Static_assert(sizeof(heap_block) == 16 && offsetof(heap_block, next) == 8
	&& HEAP_OVERHEAD == 8, "mem2 tail shrink requires the libogc heap ABI");
#include "wii/mem2_shrink.inc"

void* mem2_shrink(void* ptr, size_t old_size, size_t new_size) {
	if(!ptr || !new_size || new_size > old_size) return NULL;
	if(mem2_diag_bad()) return NULL;
	if(new_size == old_size) return ptr;
	/* A pool slot is a FIXED-size object the lwp heap does not own; splicing a
	 * tail out of one would corrupt the neighbouring slot's bytes and the heap's
	 * boundary tags alike. Refuse -- the caller keeps its full allocation, which
	 * is already the exact rung it wanted. */
	if(mem2_pool_holding(ptr)) return NULL;
	if(!(mem2_ready && (uint8_t*)ptr >= mem2_base
		 && (uint8_t*)ptr < mem2_base + mem2_carve_size))
		/* Rare general-heap spill: existing aligned alloc/copy/free keeps
		 * GX alignment. Failure leaves the old buffer and canary intact. */
		return mem2_realloc(ptr, old_size, new_size);
	mem2_lock();
	if(mem2_diag_bad()) { mem2_unlock(); return NULL; }
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	mem2_canary_verify(ptr, old_size);
	mem2_diag_begin(MEM2_DIAG_SHRINK);
	u32 level = IRQ_Disable();
	bool ok = mem2_heap_shrink(&mem2_heap, ptr, new_size + MEM2_GUARD);
	IRQ_Restore(level);
	mem2_diag_end();
	if(ok) {
		mem2_canary_fill(ptr, new_size);
		carve_note(mem2_carve_observe, CARVE_RESIZE, ptr, ptr, new_size, CARVE_MEM2);
	}
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	mem2_unlock();
	return ok ? ptr : NULL;
}

void* mem2_realloc(void* ptr, size_t old_size, size_t new_size) {
	if(mem2_diag_bad()) return NULL;
	s_mem2_realloc_calls++;
	if(!ptr)
		return mem2_alloc(new_size); // locks itself
	// The lwp_heap has no in-place grow: allocate fresh, copy the kept bytes, free the old
	// buffer. The inner mem2_alloc/mem2_free EACH take the lock, so every HEAP splice is still
	// atomic. We deliberately do NOT hold the lock across the memcpy: `np` is freshly allocated
	// and not yet published to any other thread, and `ptr` is caller-owned (the caller still
	// holds it and cannot have freed it), so the copy touches only thread-private memory -- no
	// other thread can observe or splice either buffer mid-copy. The old code held the recursive
	// lock across alloc+memcpy(tens of KB)+free, which let the prio-32 mesher worker stall the
	// prio-64 main/server threads for the whole copy: a priority inversion (libogc LWP mutexes
	// do not inherit priority) that regressed frame pacing (~5min -> ~3min on hardware). Dropping
	// the outer hold removes the inversion with zero loss of heap safety.
	void* np = mem2_alloc(new_size); // locks itself
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_TRANSFER, ptr, np, new_size, old_size);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	if(np) {
		// Verify the OLD block's canary before retiring it -- the realloc path is
		// the display-list grow churn, the hottest alloc surface in the engine.
		mem2_lock();
		mem2_canary_verify(ptr, old_size);
		mem2_unlock();
		memcpy(np, ptr, old_size < new_size ? old_size : new_size); // np private, ptr owned
		mem2_free(ptr); // locks itself
	}
	// np == NULL: caller asserts; matches realloc's NULL-on-failure contract (old kept).
	return np;
}

/* --- MP backing reservation: the Wii region shim (mem2.h) ---------------
 * ONE carve block out of the shared __lwp_heap, tagged CARVE_MP_BACKING and
 * carrying the same 256 B lead guard every pool slab does (see
 * mem2_pools_publish: a red zone like every other block, and a ledger key
 * that is not also the first payload address). Nothing inside it is ever
 * carve_note'd -- the reservation accounts every byte a body can occupy, so
 * noting bodies would double-count exactly the way CARVE_POOLED exists to
 * prevent for pool slots.
 *
 * The lock is the SAME serialising mem2 mutex the rest of this file uses:
 * these critical sections are a free-list walk, they never nest (the region
 * carve below calls __lwp_heap_allocate directly, not mem2_alloc), and a
 * second mutex would add a second lock order to reason about for nothing. */
#define MP_BACKING_LEAD MEM2_GUARD
static void mp_backing_lock(void) { mem2_lock(); }
static void mp_backing_unlock(void) { mem2_unlock(); }
static void* mp_backing_region_acquire(size_t bytes) {
	if(!mem2_ready || mem2_diag_bad())
		return NULL;
	uint8_t* block = mem2_diag_allocate_locked(bytes + MP_BACKING_LEAD,
		MEM2_DIAG_MP);
	if(!block)
		return NULL;
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_ALLOC, block, 0,
			   bytes + MP_BACKING_LEAD, CARVE_MEM2 | CARVE_IN_HEAP);
	carve_note(mem2_carve_observe, CARVE_TAG, block, 0,
			   bytes + MP_BACKING_LEAD, CARVE_MP_BACKING);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	return block + MP_BACKING_LEAD;
}
static void mp_backing_region_release(void* region, size_t bytes) {
	(void)bytes;
	if(mem2_diag_bad()) return;
	uint8_t* block = (uint8_t*)region - MP_BACKING_LEAD;
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_FREE, block, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	if(mem2_ready)
		mem2_diag_free_locked(block, MEM2_DIAG_MP);
}

#else // PLATFORM_PC and everything else: no MEM1/MEM2 split.

#include <stdlib.h>

// PC has no MEM1/MEM2 split and no GX alignment requirement, so these are plain
// malloc/free. (The Wii texel path needs 32-byte alignment, but on PC textures
// go through pc/texture.c's own malloc, not mem2_alloc.)
void mem2_init(void) {}

// PC has no regions to carve, so the audio region stays a LAZY plain malloc --
// exactly where and when the arena was claimed before this seam existed. Keeping
// it lazy also keeps mem2_init a no-op, so a test binary that never loads the
// registry never pays 12 MB. Deliberately NOT routed through mem2_alloc: the
// whole point of the seam is that the arena is accounted separately from the
// churn allocations, and mem2_stat_alloc_bytes is what proves it (the counters
// feed test_mem1_budget's MEM2-residency guard).
//
// The base honours MEM2_REGION_ALIGN like the Wii regions do (mem2.h): the audio
// arena bumps by 32-aligned sizes and the deferred cave loader's attach refuses a
// scratch that is not 32-aligned. Plain malloc only promises 16 -- glibc's mmap-
// backed 12 MB block lands at page+16 -- so on Linux the opt-in deferred path
// silently fell back to "no loader" (test_audio_deferred_registry). Over-allocate
// by the alignment and round the base up; the raw block is kept so nothing is
// lost, and the region is still never freed (first caller wins, as before).
static void* s_audio_region_raw = NULL;
static void* s_audio_region = NULL;
static size_t s_audio_region_size = 0;
static bool s_audio_region_taken = false;

void* mem2_audio_region(size_t want, size_t* out_size) {
	if(!s_audio_region_taken) { // first caller wins; reload reuses the block
		s_audio_region_taken = true;
		if(want && want <= SIZE_MAX - (MEM2_REGION_ALIGN - 1u))
			s_audio_region_raw = malloc(want + (MEM2_REGION_ALIGN - 1u));
		s_audio_region = s_audio_region_raw ?
			(void*)(((uintptr_t)s_audio_region_raw + (MEM2_REGION_ALIGN - 1u))
					& ~(uintptr_t)(MEM2_REGION_ALIGN - 1u)) :
			NULL;
		s_audio_region_size = s_audio_region ? want : 0;
	}
	if(out_size)
		*out_size = s_audio_region_size;
	return s_audio_region;
}

// No carve on PC. The audio region size is the one real number here, and it is
// what the F3 "snd: arena used/total" denominator reads on both platforms.
size_t mem2_stat_carve_size(void) { return 0; }
/* No carve on PC, so no fixed-tenant pools: the census reports them absent
 * (all zero) rather than synthesizing numbers a PC run cannot have, and there
 * is no reservation for the observer to attribute. */
void mem2_pool_stats(struct mem2_pool_stat* far_out,
					 struct mem2_pool_stat* view_out) {
	if(far_out) memset(far_out, 0, sizeof(*far_out));
	if(view_out) memset(view_out, 0, sizeof(*view_out));
}
void mem2_pool_column_stats(struct mem2_pool_stat* out) {
	if(out) memset(out, 0, sizeof(*out));
}
unsigned mem2_pool_column_occupancy(struct mem2_pool_stat* out,
									uint8_t* per_slab, unsigned cap,
									uint32_t* event_seq, uint32_t* events_lost) {
	(void)per_slab;
	(void)cap;
	if(out) memset(out, 0, sizeof(*out));
	if(event_seq) *event_seq = 0;
	if(events_lost) *events_lost = 0;
	return 0;
}
void mem2_pool_column_events_flush(void) {}
void mem2_pool_note_reservations(void) {}
size_t mem2_stat_audio_region_size(void) { return s_audio_region_size; }
int mem2_stat_carve_free_kb(void) { return -1; } // no carve on PC
uint32_t mem2_diag_op(void) { return 0; }
uint32_t mem2_diag_thread(void) { return 0; }
uint32_t mem2_diag_bad(void) { return 0; }
uint32_t mem2_diag_scan_count(void) { return 0; }
uint32_t mem2_diag_scan_max_us(void) { return 0; }
uint32_t mem2_diag_scan_total_us(void) { return 0; }
// F4: no carve, so no ceiling to measure -- SIZE_MAX is the contract's
// "unknown, do not gate" (mem2.h), which is what keeps the MP promotion gate
// inert on PC until a test overrides the reading explicitly.
size_t mem2_carve_free_bytes(void) { return SIZE_MAX; }
bool mem2_carve_largest_allocatable_bytes(size_t* out_bytes) {
	if(out_bytes)
		*out_bytes = 0;
	return false; // no carve on PC
}

void* mem2_alloc(size_t size) {
	s_mem2_alloc_calls++;
	s_mem2_alloc_bytes += size;
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	void* p = malloc(size);
	carve_note(mem2_carve_observe, CARVE_ALLOC, p, 0, size, CARVE_MEM2);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	return p;
}

void* mem2_alloc_strict(size_t size) { return mem2_alloc(size); }
/* COLUMN-POOL EMULATION (colcap-1006, R4 point 3) -- PC TEST SEAM ONLY.
 * The PC build has no carve and no pools, so no PC world/light/mesh test can
 * see where a column lands. With CW_TEST_COLUMN_POOL=rotating|lowest in the
 * environment, MEM2_POOL_COLUMN requests are served from a host-side copy of
 * the Wii column pool (the same mem2_pool.h code: 4 eager boot slabs, cap,
 * demand growth, the SAME take policy the Wii uses, and the SAME empty-slab
 * hand-back rule), so the existing golden/equivalence tests can be run under
 * both placements and must produce identical bytes. Unset (every normal PC
 * run): one getenv at the first pooled request, then the plain malloc path
 * exactly as before. */
#include <stdatomic.h>
#include <stdio.h>
static int s_emul_policy = -1; /* -1 unread, 0 off, 1 rotating, 2 lowest */
static struct mem2_pool s_emul_pool;
static atomic_flag s_emul_lock = ATOMIC_FLAG_INIT;
static uint32_t s_emul_takes, s_emul_grows, s_emul_shrinks, s_emul_overflow;
static uint32_t s_emul_shrink_hw_low; /* fewest slabs after a hand-back */
static void emul_lock(void) { while(atomic_flag_test_and_set(&s_emul_lock)) {} }
static void emul_unlock(void) { atomic_flag_clear(&s_emul_lock); }
static void* emul_carve(size_t bytes, void* ctx) {
	(void)ctx;
	/* Match the Wii carve's 32-byte alignment. Host malloc guarantees only
	 * fundamental alignment (often 16 bytes), including for large slabs. Keep
	 * the raw allocation immediately before the aligned block for hand-back. */
	if(bytes > SIZE_MAX - 31u - sizeof(void*)) return NULL;
	void* raw = malloc(bytes + 31u + sizeof(void*));
	if(!raw) return NULL;
	void* block = (void*)(((uintptr_t)raw + sizeof(void*) + 31u) & ~(uintptr_t)31u);
	((void**)block)[-1] = raw;
	return block;
}
static void emul_report(void) {
	fprintf(stderr, "[mem2-column-emul] policy=%s takes=%u grows=%u "
		"shrinks=%u overflow=%u slabs_end=%u peak=%u fewest_after_shrink=%u\n",
		s_emul_policy == 2 ? "lowest" : "rotating", s_emul_takes, s_emul_grows,
		s_emul_shrinks, s_emul_overflow, mem2_pool_slabs(&s_emul_pool),
		s_emul_pool.peak, s_emul_shrink_hw_low);
	/* A placement-equivalence test that never reached the pool proves
	 * nothing: with CW_TEST_COLUMN_POOL_REQUIRE set, fail the process. */
	if(getenv("CW_TEST_COLUMN_POOL_REQUIRE") && !s_emul_takes)
		_Exit(3);
}
static int emul_policy(void) {
	int policy = __atomic_load_n(&s_emul_policy, __ATOMIC_ACQUIRE);
	if(policy >= 0)
		return policy;
	emul_lock();
	if(s_emul_policy < 0) {
		const char* e = getenv("CW_TEST_COLUMN_POOL");
		int want = !e ? 0 : !strcmp(e, "lowest") ? 2 : !strcmp(e, "rotating") ? 1 : 0;
		if(want) {
			/* 256 = the Wii MEM2_GUARD lead/stride guard */
			mem2_pool_init(&s_emul_pool, 256u, MEM2_POOL_COLUMN_OBJECT,
				MEM2_POOL_COLUMN_OBJECT + 256u, MEM2_POOL_COLUMN_SLABS,
				emul_carve, NULL);
			for(unsigned k = 0; k < MEM2_POOL_COLUMN_BOOT_SLABS; k++)
				(void)mem2_pool_grow(&s_emul_pool);
			s_emul_shrink_hw_low = UINT32_MAX;
			atexit(emul_report);
		}
		__atomic_store_n(&s_emul_policy, want, __ATOMIC_RELEASE);
	}
	emul_unlock();
	return s_emul_policy;
}

bool mem2_column_pool_emulated(void) { return emul_policy() > 0; }
/* PC: no Broadway allocator to time (mem2.h). */
void mem2_cost_take(uint32_t out[MEM2_COST_FIELDS]) {
	memset(out, 0, MEM2_COST_FIELDS * sizeof *out);
}

/* R5 item 2 hooks (mem2.h) on the PC emulation: same semantics, so the
 * self-test driver runs under ctest exactly as it runs on the Wii. */
static bool s_emul_st_armed;
static int s_emul_st_rotating = -1; /* -1 = follow CW_TEST_COLUMN_POOL */
static unsigned s_emul_st_refuse;
static uint32_t s_emul_st_digest = 2166136261u, s_emul_st_takes;
static uint32_t s_emul_shrink_fail;
bool mem2_pool_selftest_arm(enum mem2_pool_placement placement) {
	if(emul_policy() <= 0)
		return false;
	emul_lock();
	s_emul_st_armed = true;
	s_emul_st_rotating = placement == MEM2_POOL_PLACE_ROTATING;
	s_emul_st_digest = 2166136261u;
	s_emul_st_takes = 0;
	emul_unlock();
	return true;
}
bool mem2_pool_selftest_armed(void) { return s_emul_st_armed; }
void mem2_pool_selftest_refuse_handbacks(unsigned count) {
	emul_lock();
	if(s_emul_st_armed)
		s_emul_st_refuse = count;
	emul_unlock();
}
unsigned mem2_pool_selftest_refusals_pending(void) {
	emul_lock();
	unsigned n = s_emul_st_refuse;
	emul_unlock();
	return n;
}
bool mem2_pool_column_locate(const void* ptr, int* slab, int* slot) {
	if(emul_policy() <= 0)
		return false;
	emul_lock();
	int s = mem2_pool_slab_of(&s_emul_pool, ptr);
	bool live = false;
	if(s >= 0) {
		unsigned bit = (unsigned)(((const uint8_t*)ptr - s_emul_pool.base[s])
								  / s_emul_pool.stride);
		live = (s_emul_pool.bits[s] >> bit) & 1u;
		if(slab) *slab = s;
		if(slot) *slot = (int)bit;
	}
	emul_unlock();
	return live;
}
uint32_t mem2_pool_selftest_placement_digest(uint32_t* takes) {
	emul_lock();
	uint32_t d = s_emul_st_digest;
	if(takes) *takes = s_emul_st_takes;
	emul_unlock();
	return d;
}
unsigned mem2_pool_selftest_stats(struct mem2_pool_stat* out,
								  uint8_t* per_slab, unsigned cap) {
	memset(out, 0, sizeof(*out));
	if(emul_policy() <= 0)
		return 0;
	emul_lock();
	out->slots = mem2_pool_slots_max(&s_emul_pool);
	out->used = s_emul_pool.used;
	out->peak = s_emul_pool.peak;
	out->overflow = s_emul_pool.overflow;
	out->slabs = mem2_pool_slabs(&s_emul_pool);
	out->slabs_max = mem2_pool_slabs_max(&s_emul_pool);
	out->carve_fail = s_emul_pool.carve_fail;
	out->bytes = mem2_pool_bytes(&s_emul_pool);
	out->shrinks = s_emul_pool.shrinks;
	out->shrink_fail = s_emul_shrink_fail;
	unsigned n = mem2_pool_slabs(&s_emul_pool);
	if(n > cap)
		n = cap;
	for(unsigned i = 0; i < n; i++)
		per_slab[i] = (uint8_t)mem2_pool_slab_used(&s_emul_pool, i);
	emul_unlock();
	return n;
}

void* mem2_alloc_pooled(size_t size, enum mem2_pool_id pool) {
	const int policy = pool == MEM2_POOL_COLUMN && size == MEM2_POOL_COLUMN_OBJECT
		? emul_policy() : 0;
	if(!policy)
		return mem2_alloc(size);
	emul_lock();
	const bool lowest = s_emul_st_rotating >= 0 ? !s_emul_st_rotating
											   : policy == 2;
	void* (*take)(struct mem2_pool*) = lowest ? mem2_pool_take_lowest
											  : mem2_pool_take;
	void* p = take(&s_emul_pool);
	if(!p && mem2_pool_grow(&s_emul_pool)) {
		s_emul_grows++;
		p = take(&s_emul_pool);
	}
	if(p)
		s_emul_takes++;
	if(p && s_emul_st_armed) {
		int slab = mem2_pool_slab_of(&s_emul_pool, p);
		uint32_t slot = (uint32_t)(((uint8_t*)p - s_emul_pool.base[slab])
								   / s_emul_pool.stride);
		uint32_t key = ((uint32_t)(slab & 0xff) << 8) | (slot & 0xffu);
		for(unsigned k = 0; k < 2; k++) {
			s_emul_st_digest ^= (key >> (8 * k)) & 0xffu;
			s_emul_st_digest *= 16777619u;
		}
		s_emul_st_takes++;
	}
	else
		s_emul_overflow++;
	emul_unlock();
	if(!p)
		return mem2_alloc(size); /* over the cap: the shared heap, as on Wii */
	s_mem2_alloc_calls++;
	s_mem2_alloc_bytes += size;
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_ALLOC, p, 0, size, CARVE_MEM2);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	return p;
}

void mem2_free(void* ptr) {
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_FREE, ptr, 0, 0, 0);
	if(ptr)
		s_mem2_free_calls++;
	bool pooled = false;
	if(ptr && __atomic_load_n(&s_emul_policy, __ATOMIC_ACQUIRE) > 0) {
		emul_lock();
		if(mem2_pool_owns(&s_emul_pool, ptr)) {
			pooled = mem2_pool_release(&s_emul_pool, ptr);
			int s = mem2_pool_shrink_candidate(&s_emul_pool,
				MEM2_POOL_COLUMN_BOOT_SLABS, MEM2_POOL_COLUMN_SHRINK_HEADROOM);
			if(s >= 0 && s_emul_st_refuse) {
				s_emul_st_refuse--; /* forced refusal: slab kept (mem2.h) */
				s_emul_shrink_fail++;
			} else if(s >= 0) {
				void* block = mem2_pool_shrink(&s_emul_pool, (unsigned)s,
					MEM2_POOL_COLUMN_BOOT_SLABS);
				if(block) free(((void**)block)[-1]);
				s_emul_shrinks++;
				if(mem2_pool_slabs(&s_emul_pool) < s_emul_shrink_hw_low)
					s_emul_shrink_hw_low = mem2_pool_slabs(&s_emul_pool);
			}
			pooled = true;
		}
		emul_unlock();
	}
	if(!pooled)
		free(ptr);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
}

void* mem2_realloc(void* ptr, size_t old_size, size_t new_size) {
	if(ptr && __atomic_load_n(&s_emul_policy, __ATOMIC_ACQUIRE) > 0) {
		emul_lock();
		const bool owned = mem2_pool_owns(&s_emul_pool, ptr);
		emul_unlock();
		if(owned) { /* an emulated slot is not a malloc block */
			void* np = malloc(new_size);
			if(np) {
				memcpy(np, ptr, old_size < new_size ? old_size : new_size);
				mem2_free(ptr);
			}
			return np;
		}
	}
	s_mem2_realloc_calls++;
	(void)old_size; // plain realloc knows its own block size on PC
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	uintptr_t old = (uintptr_t)ptr;
	void* p = realloc(ptr, new_size);
	carve_note(mem2_carve_observe, CARVE_RESIZE, old, p, new_size, CARVE_MEM2);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	return p;
}

void* mem2_shrink(void* ptr, size_t old_size, size_t new_size) {
	if(!ptr || !new_size || new_size > old_size) return NULL;
	return new_size == old_size ? ptr : mem2_realloc(ptr, old_size, new_size);
}

// PC: no guard bytes exist (mem2_alloc is plain malloc), so the canary is a
// no-op here -- reading past the block would itself be the bug under ASan,
// which is the sanitizer that owns this platform.
void mem2_free_sized(void* ptr, size_t size) {
	(void)size;
	mem2_free(ptr);
}

uint32_t mem2_canary_hits(void) {
	return 0;
}

void mem2_canary_first(void** ptr, size_t* size, size_t* guard_off) {
	if(ptr)
		*ptr = NULL;
	if(size)
		*size = 0;
	if(guard_off)
		*guard_off = 0;
}

/* --- MP backing reservation: the PC region shim (mem2.h) ----------------
 * No carve, so the region is one malloc of the same size and the identical
 * allocator runs on top of it -- a PC test therefore exercises the REAL
 * capacity arithmetic (the 8 MiB Wii budget included, via
 * mp_backing_configure) instead of a look-alike.
 *
 * The lock is an atomic test-and-set spin rather than a pthread mutex on
 * purpose: mem2.c is linked into bench, soak and test targets whose link
 * sets are not guaranteed to carry a threads library, and the project
 * already proves (platform/thread.h, CR-08) that uint32_t is lock-free
 * atomic on every build target. The critical section is a bounded free-list
 * walk with at most two threads (the MP read thread and the pump) ever
 * contending, on a multi-core host -- the one case where spinning is
 * cheaper than a syscall. The Wii, which is single-core and where a
 * preempted holder would spin the waiter for a whole slice, uses a real
 * mutex instead (the branch above). */
static uint32_t s_mp_backing_spin = 0;
static void mp_backing_lock(void) {
	while(__atomic_exchange_n(&s_mp_backing_spin, 1u, __ATOMIC_ACQUIRE))
		;
}
static void mp_backing_unlock(void) {
	__atomic_store_n(&s_mp_backing_spin, 0u, __ATOMIC_RELEASE);
}
static void* mp_backing_region_acquire(size_t bytes) {
	void* region = malloc(bytes);
	if(!region)
		return NULL;
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_ALLOC, region, 0, bytes, CARVE_MEM2);
	carve_note(mem2_carve_observe, CARVE_TAG, region, 0, bytes,
			   CARVE_MP_BACKING);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	return region;
}
static void mp_backing_region_release(void* region, size_t bytes) {
	(void)bytes;
	carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_FREE, region, 0, 0, 0);
	carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	free(region);
}

#endif

/* ======================================================================
 * MP compressed-body backing reservation -- the allocator
 * ======================================================================
 * Platform-independent on purpose: only the region acquire/release and the
 * lock differ per platform (the two shims above), so the arithmetic a PC
 * test proves is the arithmetic the Wii runs. See mem2.h for the design
 * contract and docs/mp_admission_budget_design_2026-09-13.md section 2.4.
 *
 * Structure: ONE contiguous region, address-ordered doubly-linked blocks,
 * first fit, split on take, coalesce both neighbours on release. A body is
 * 3-9 KB of ordinary terrain (measured; see tests/test_mp_admission.c case
 * (g)) against a 32 B header, so the header overhead is ~0.4% -- which is
 * why a boundary-tag list is the right shape here and a fixed-object pool
 * (mem2_pool.h) is not. */
#include <assert.h>
#include <stdlib.h>

/* 8 is what malloc guarantees on the 32-bit target and what a struct with a
 * pointer member needs; the bytes themselves are a zlib stream read a byte at
 * a time, so nothing here wants GX's 32. */
#define MP_BACKING_ALIGN 8u
#define MP_BACKING_MAGIC 0x4D50424Bu /* "MPBK" */
struct mp_backing_block {
	uint32_t magic;
	uint32_t size; /* payload capacity, an MP_BACKING_ALIGN multiple */
	uint32_t req;  /* bytes the caller asked for; 0 means FREE */
	uint32_t reserved;
	struct mp_backing_block* prev;
	struct mp_backing_block* next;
};
#define MP_BACKING_HDR                                                        \
	((sizeof(struct mp_backing_block) + (MP_BACKING_ALIGN - 1))               \
	 & ~(size_t)(MP_BACKING_ALIGN - 1))

static uint8_t* s_mp_backing_region = NULL;
static size_t s_mp_backing_capacity = 0;
static size_t s_mp_backing_want = MEM2_MP_BACKING_BYTES;
static size_t s_mp_backing_in_use = 0;
static size_t s_mp_backing_peak = 0;
static size_t s_mp_backing_live = 0;
static struct mp_backing_block* s_mp_backing_first = NULL;

/* Carve the region if it is not carved yet. Called with the lock held.
 * The ladder (full want, 3/4, 1/2, 1/4, floor) exists because a lazy carve
 * can meet a heap that has the bytes but not one contiguous run of them: a
 * smaller reservation that the session can actually use beats refusing every
 * MP join outright, and whatever lands becomes THE capacity every later
 * check and every telemetry field reads -- never a claimed size the region
 * does not have. */
static bool mp_backing_reserve_locked(void) {
	if(s_mp_backing_region)
		return true;
	const unsigned quarters[4] = {4u, 3u, 2u, 1u};
	size_t floor_bytes = (size_t)MEM2_MP_BACKING_MIB_MIN * 1024u * 1024u;
	size_t tried = 0;
	for(unsigned k = 0; k < 4; k++) {
		size_t want = s_mp_backing_want / 4u * quarters[k];
		if(want < floor_bytes)
			want = floor_bytes;
		want &= ~(size_t)(MP_BACKING_ALIGN - 1);
		if(want <= MP_BACKING_HDR || want == tried)
			continue; /* the clamp collapsed this rung onto the last one */
		tried = want;
		void* region = mp_backing_region_acquire(want);
		if(!region)
			continue;
		s_mp_backing_region = (uint8_t*)region;
		s_mp_backing_capacity = want;
		struct mp_backing_block* head = (struct mp_backing_block*)region;
		head->magic = MP_BACKING_MAGIC;
		head->size = (uint32_t)(want - MP_BACKING_HDR);
		head->req = 0;
		head->reserved = 0;
		head->prev = NULL;
		head->next = NULL;
		s_mp_backing_first = head;
		return true;
	}
	return false;
}

/* Absorb `b->next` into `b`. Both must be free and adjacent by construction
 * (the list is address-ordered and covers the region with no gaps). */
static void mp_backing_merge_locked(struct mp_backing_block* b) {
	struct mp_backing_block* n = b->next;
	assert(n && n->magic == MP_BACKING_MAGIC && n->req == 0 && b->req == 0);
	assert((uint8_t*)n == (uint8_t*)b + MP_BACKING_HDR + b->size);
	b->size = (uint32_t)((size_t)b->size + MP_BACKING_HDR + n->size);
	b->next = n->next;
	if(b->next)
		b->next->prev = b;
	n->magic = 0; /* poison: a stale pointer into an absorbed header asserts */
}

void* mp_backing_alloc(size_t length) {
	if(length == 0 || length > UINT32_MAX)
		return NULL;
	mp_backing_lock();
	if(mem2_diag_bad()) { mp_backing_unlock(); return NULL; }
	if(!mp_backing_reserve_locked()) {
		mp_backing_unlock();
		return NULL;
	}
	size_t need
		= (length + (MP_BACKING_ALIGN - 1)) & ~(size_t)(MP_BACKING_ALIGN - 1);
	struct mp_backing_block* found = NULL;
	for(struct mp_backing_block* b = s_mp_backing_first; b; b = b->next) {
		assert(b->magic == MP_BACKING_MAGIC);
		if(b->req == 0 && (size_t)b->size >= need) {
			found = b;
			break;
		}
	}
	if(!found) {
		/* THE capacity answer (design section 2.4): no fallback heap, no
		 * eviction. The caller turns this into the visible, clean session
		 * end -- which is the whole point of owning a reservation. */
		mp_backing_unlock();
		return NULL;
	}
	/* Split only when the tail can hold a header plus a body worth having;
	 * a smaller remainder stays with this block as internal slack rather
	 * than becoming an unusable free node. */
	if((size_t)found->size >= need + MP_BACKING_HDR + MP_BACKING_ALIGN) {
		struct mp_backing_block* tail = (struct mp_backing_block*)((uint8_t*)
			found + MP_BACKING_HDR + need);
		tail->magic = MP_BACKING_MAGIC;
		tail->size = (uint32_t)((size_t)found->size - need - MP_BACKING_HDR);
		tail->req = 0;
		tail->reserved = 0;
		tail->prev = found;
		tail->next = found->next;
		if(tail->next)
			tail->next->prev = tail;
		found->next = tail;
		found->size = (uint32_t)need;
	}
	found->req = (uint32_t)length;
	s_mp_backing_in_use += length;
	if(s_mp_backing_in_use > s_mp_backing_peak)
		s_mp_backing_peak = s_mp_backing_in_use;
	s_mp_backing_live++;
	mp_backing_unlock();
	return (uint8_t*)found + MP_BACKING_HDR;
}

void mp_backing_free(void* body) {
	if(!body)
		return;
	mp_backing_lock();
	if(mem2_diag_bad()) { mp_backing_unlock(); return; }
	assert(s_mp_backing_region
		   && (uint8_t*)body >= s_mp_backing_region + MP_BACKING_HDR
		   && (uint8_t*)body < s_mp_backing_region + s_mp_backing_capacity
		   && "mp_backing_free on a pointer this allocator never handed out");
	struct mp_backing_block* b
		= (struct mp_backing_block*)((uint8_t*)body - MP_BACKING_HDR);
	assert(b->magic == MP_BACKING_MAGIC && b->req != 0
		   && "double free or corrupted MP backing header");
	if(s_mp_backing_in_use >= b->req)
		s_mp_backing_in_use -= b->req;
	else
		s_mp_backing_in_use = 0;
	if(s_mp_backing_live)
		s_mp_backing_live--;
	b->req = 0;
	if(b->next && b->next->req == 0)
		mp_backing_merge_locked(b);
	if(b->prev && b->prev->req == 0)
		mp_backing_merge_locked(b->prev);
	mp_backing_unlock();
}

bool mp_backing_owns(const void* body) {
	if(!body)
		return false;
	mp_backing_lock();
	bool inside = s_mp_backing_region
		&& (const uint8_t*)body >= s_mp_backing_region + MP_BACKING_HDR
		&& (const uint8_t*)body < s_mp_backing_region + s_mp_backing_capacity;
	mp_backing_unlock();
	return inside;
}

void mp_backing_stats(size_t* in_use, size_t* peak, size_t* capacity) {
	mp_backing_lock();
	if(in_use)
		*in_use = s_mp_backing_in_use;
	if(peak)
		*peak = s_mp_backing_peak;
	/* Before the first carve there is no region, so report the WANT -- a
	 * budget has to be readable before a body exists, and the ladder in
	 * mp_backing_reserve_locked can only ever land at or below it. */
	if(capacity)
		*capacity = s_mp_backing_capacity ? s_mp_backing_capacity
										  : s_mp_backing_want;
	mp_backing_unlock();
}

size_t mp_backing_configure(size_t bytes) {
	size_t lo = (size_t)MEM2_MP_BACKING_MIB_MIN * 1024u * 1024u;
	size_t hi = (size_t)MEM2_MP_BACKING_MIB_MAX * 1024u * 1024u;
	if(bytes < lo)
		bytes = lo;
	if(bytes > hi)
		bytes = hi;
	bytes &= ~(size_t)(MP_BACKING_ALIGN - 1);
	mp_backing_lock();
	if(mem2_diag_bad()) {
		mp_backing_unlock();
		return 0;
	}
	s_mp_backing_want = bytes;
	/* Re-reserve only while the region is EMPTY. Live bodies cannot be moved,
	 * and shrinking the capacity under live accounting would make every later
	 * check lie -- so the new size simply waits for the next reservation. */
	if(s_mp_backing_region && s_mp_backing_live == 0
	   && s_mp_backing_capacity != bytes) {
		mp_backing_region_release(s_mp_backing_region, s_mp_backing_capacity);
		s_mp_backing_region = NULL;
		s_mp_backing_capacity = 0;
		s_mp_backing_first = NULL;
		(void)mp_backing_reserve_locked();
	}
	size_t effective
		= s_mp_backing_capacity ? s_mp_backing_capacity : s_mp_backing_want;
	mp_backing_unlock();
	return effective;
}

/* --- the scratch side budget (mem2.h, F5a / review finding L10) -------- */
static uint8_t* s_mp_scratch_region = NULL;
static size_t s_mp_scratch_capacity = 0;
static size_t s_mp_scratch_cursor = 0;
static size_t s_mp_scratch_in_use = 0;
static size_t s_mp_scratch_peak = 0;
static size_t s_mp_scratch_off[MP_BACKING_SCRATCH_SLOTS];
static size_t s_mp_scratch_size[MP_BACKING_SCRATCH_SLOTS];
static bool s_mp_scratch_held[MP_BACKING_SCRATCH_SLOTS];
static size_t s_mp_scratch_live = 0;

/* Release BOTH regions. Called with the lock held. */
static size_t mp_backing_release_all_locked(void) {
	size_t given_back = 0;
	if(s_mp_backing_region) {
		given_back += s_mp_backing_capacity;
		mp_backing_region_release(s_mp_backing_region, s_mp_backing_capacity);
	}
	s_mp_backing_region = NULL;
	s_mp_backing_capacity = 0;
	s_mp_backing_first = NULL;
	s_mp_backing_peak = 0;
	if(s_mp_scratch_region) {
		given_back += s_mp_scratch_capacity;
		mp_backing_region_release(s_mp_scratch_region, s_mp_scratch_capacity);
	}
	s_mp_scratch_region = NULL;
	s_mp_scratch_capacity = 0;
	s_mp_scratch_cursor = 0;
	s_mp_scratch_peak = 0;
	for(unsigned k = 0; k < MP_BACKING_SCRATCH_SLOTS; k++) {
		s_mp_scratch_off[k] = 0;
		s_mp_scratch_size[k] = 0;
		s_mp_scratch_held[k] = false;
	}
	return given_back;
}

void mp_backing_reset(void) {
	mp_backing_lock();
	if(mem2_diag_bad()) { mp_backing_unlock(); return; }
	assert(s_mp_backing_live == 0 && s_mp_backing_in_use == 0
		   && "mp_backing_reset with bodies still live is a leak, not a reset");
	assert(s_mp_scratch_live == 0 && s_mp_scratch_in_use == 0
		   && "mp_backing_reset with a scratch slot still held is a leak");
	(void)mp_backing_release_all_locked();
	mp_backing_unlock();
}

size_t mp_backing_release_if_idle(void) {
	mp_backing_lock();
	if(mem2_diag_bad()) { mp_backing_unlock(); return 0; }
	/* Both budgets must be quiet. A live body or a held slot means this is
	 * not the end of the session the reservation belongs to (or something
	 * leaked, which is the leak check's business, not this one's) -- so keep
	 * the regions and report 0 rather than pulling memory out from under a
	 * live pointer. */
	if(s_mp_backing_live || s_mp_backing_in_use || s_mp_scratch_live
	   || s_mp_scratch_in_use) {
		mp_backing_unlock();
		return 0;
	}
	size_t given_back = mp_backing_release_all_locked();
	mp_backing_unlock();
	return given_back;
}

/* Acquire the side region if it is not acquired yet. Lock held. Unlike the
 * body reservation there is no size ladder: the whole point of a fixed side
 * budget is that it is small (544 KB) and either present or not. */
static bool mp_scratch_reserve_locked(void) {
	if(s_mp_scratch_region)
		return true;
	void* region = mp_backing_region_acquire((size_t)MEM2_MP_SCRATCH_BYTES);
	if(!region)
		return false;
	s_mp_scratch_region = (uint8_t*)region;
	s_mp_scratch_capacity = (size_t)MEM2_MP_SCRATCH_BYTES;
	s_mp_scratch_cursor = 0;
	return true;
}

void* mp_backing_scratch_acquire(enum mp_backing_scratch_slot slot,
								 size_t bytes) {
	if(slot >= MP_BACKING_SCRATCH_SLOTS || bytes == 0)
		return NULL;
	mp_backing_lock();
	if(mem2_diag_bad()) { mp_backing_unlock(); return NULL; }
	if(!mp_scratch_reserve_locked()) {
		mp_backing_unlock();
		return NULL;
	}
	if(s_mp_scratch_size[slot] == 0) {
		/* First claim: fix this slot's offset and size for the life of the
		 * region, so it can never fail again while the region stands. */
		size_t need
			= (bytes + (MP_BACKING_ALIGN - 1)) & ~(size_t)(MP_BACKING_ALIGN - 1);
		if(need > s_mp_scratch_capacity - s_mp_scratch_cursor) {
			mp_backing_unlock();
			return NULL;
		}
		s_mp_scratch_off[slot] = s_mp_scratch_cursor;
		s_mp_scratch_size[slot] = need;
		s_mp_scratch_cursor += need;
	} else if(bytes > s_mp_scratch_size[slot]) {
		/* A slot's size is fixed by its first claim; a later, larger claim is
		 * a caller bug (both slots are compile-time bounded), so it asserts
		 * in a test build and refuses rather than overrunning in release. */
		assert(false && "MP scratch slot re-claimed larger than its first claim");
		mp_backing_unlock();
		return NULL;
	}
	if(!s_mp_scratch_held[slot]) {
		s_mp_scratch_held[slot] = true;
		s_mp_scratch_live++;
		s_mp_scratch_in_use += s_mp_scratch_size[slot];
		if(s_mp_scratch_in_use > s_mp_scratch_peak)
			s_mp_scratch_peak = s_mp_scratch_in_use;
	}
	uint8_t* p = s_mp_scratch_region + s_mp_scratch_off[slot];
	mp_backing_unlock();
	return p;
}

void mp_backing_scratch_release(enum mp_backing_scratch_slot slot) {
	if(slot >= MP_BACKING_SCRATCH_SLOTS)
		return;
	mp_backing_lock();
	if(mem2_diag_bad()) { mp_backing_unlock(); return; }
	if(s_mp_scratch_held[slot]) {
		s_mp_scratch_held[slot] = false;
		if(s_mp_scratch_live)
			s_mp_scratch_live--;
		if(s_mp_scratch_in_use >= s_mp_scratch_size[slot])
			s_mp_scratch_in_use -= s_mp_scratch_size[slot];
		else
			s_mp_scratch_in_use = 0;
	}
	mp_backing_unlock();
}

void mp_backing_scratch_stats(size_t* in_use, size_t* peak, size_t* capacity) {
	mp_backing_lock();
	if(in_use)
		*in_use = s_mp_scratch_in_use;
	if(peak)
		*peak = s_mp_scratch_peak;
	if(capacity)
		*capacity = s_mp_scratch_capacity ? s_mp_scratch_capacity
										  : (size_t)MEM2_MP_SCRATCH_BYTES;
	mp_backing_unlock();
}

void mem2_carve_snapshot(void (*sample)(size_t, size_t, size_t, size_t)) {
#ifdef PLATFORM_WII
	mem2_lock();
	heap_iblock info;
	size_t largest = 0, capacity = 0, available = 0, used_blocks = 0;
	bool valid = mem2_ready && mem2_diag_validate_locked();
	bool info_ok = false;
	if(valid) {
		mem2_diag_begin(MEM2_DIAG_GETINFO);
		info_ok = __lwp_heap_getinfo(&mem2_heap, &info) == 0;
		mem2_diag_end();
	}
	if(info_ok) {
		capacity = mem2_carve_size;
		available = info.free_size;
		used_blocks = info.used_blocks;
		/* Physical boundary-tag walk, same front_flag size/used encoding as
		 * lwp_heap. No free-list mutation; bounded by the reserved region. */
		for(heap_block* b = mem2_heap.start; b < mem2_heap.final;) {
			size_t size = b->front_flag & ~HEAP_BLOCK_USED;
			if(!size || size > (size_t)((uint8_t*)mem2_heap.final - (uint8_t*)b)) break;
			if(!(b->front_flag & HEAP_BLOCK_USED) && size > largest) largest = size;
			b = (heap_block*)((uint8_t*)b + size);
		}
	}
	sample(capacity, available, largest, used_blocks);
	mem2_unlock();
	if(mem2_ready && !valid) mem2_diag_emit_bad();
#else
	sample(0, 0, 0, 0);
#endif
}
