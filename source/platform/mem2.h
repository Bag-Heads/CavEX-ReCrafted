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

#ifndef PLATFORM_MEM2_H
#define PLATFORM_MEM2_H

#include "../carve_observer.h"
#include "mem2_pool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
	A dedicated allocator carved from the Wii's MEM2 (the slower 64 MB pool,
	which sits ~75% idle while the tight 24 MB MEM1 fast pool fills first because
	libogc's default malloc heap grows MEM1-before-MEM2). It is for large,
	long-lived, GX-read buffers -- today the texture texels -- so they stop
	consuming the scarce MEM1 the engine actually needs for framebuffers, the GX
	FIFO and per-chunk display lists.

	On PC (no MEM1/MEM2 split) mem2_init is a no-op and mem2_alloc is a plain
	32-byte-aligned malloc.
*/

// Reserve the MEM2 buffer heap AND the audio PCM region. Call ONCE, as early as
// possible in startup, before the malloc heap has spilled into MEM2.
// Idempotent-safe and harmless to call on PC. If the reservation fails for any
// reason, mem2_alloc transparently falls back to the normal heap, so callers
// never have to check.
void mem2_init(void);

/*
	AUDIO PCM REGION -- its OWN MEM2 region, NOT part of the lwp carve.

	The arena is one never-freed, never-realloced, session-lifetime block; the
	carve's other tenants (display lists above all) churn through mem2_realloc
	tens of times per mesh. Taking the arena out of the carve with mem2_alloc
	parked a 12 MB immovable brick inside the very heap whose realloc churn had
	already been measured wedging the GP (mem2.c MEM2_HEAP_SIZE 16->40 MB note):
	it consumed 30% of the carve that grow was measured to need, and its
	lifetime shares nothing with its neighbours. So it gets its own region and
	the carve is sized to leave room for it.

	Priority is CARVE FIRST -- see the ordering argument in mem2.c. When arena2
	cannot satisfy both, the AUDIO region is what clips.
*/
#define MEM2_AUDIO_REGION_BYTES (12u * 1024u * 1024u)

/*
	Bytes wanted for the lwp display-list/GX carve. THE TWO CONSTANTS ARE A
	COUPLED PAIR: arena2 cannot hold both wants plus the spill margin, so every
	byte of carve want is a byte the audio region does not get (mem2.c's
	CONSEQUENCE note carries the sizing argument and the hang history -- read it
	before touching this). It lives in the header, next to the constant it trades
	against, so tests/test_mem2_audio_region.c can pin the SHIPPING split against
	the measured sound-pack size instead of a hand-copied number: on the Wii the
	pair decides whether the real Beta pack is fully resident or partly silent.
	Wii-only in effect -- the PC branch carves nothing -- but defined on both so
	the golden compiles.
*/
/* 2026-09-10 (Astra M2 A/B ruling, repartition experiment): 36 -> 38.5 MiB,
 * paid for by the spill margin (mem2.c MEM2_RESERVE_MARGIN 4 -> 1.5 MiB), so
 * carve + margin stay 40 MiB and the audio remainder is byte-identical
 * (tests/test_mem2_audio_region.c pins that). With mesh blocks on, the
 * split_anchor_6m 42/256 run left 3,931 KiB of arena2 unclaimed all run
 * while the carve floor sat at 40 KiB: the headroom is in the wrong pool. */
#define MEM2_HEAP_SIZE (38u * 1024u * 1024u + 512u * 1024u)

/*
	FIXED-TENANT POOLS (2026-09-08, carve-arena interleave lane; see mem2_pool.h
	for the mechanism and column_view.c:123-158 for the bitmap precedent).

	Two carve tenants have a FIXED object size and a lifetime unrelated to the
	display lists they sit between, so each live one is a wedge in the arena the
	lists need contiguous. They are carved ONCE, at the bottom of a virgin heap,
	and served from bitmap slots instead of the shared first-fit free list.

	KEYED BY TENANT, NOT BY SIZE. A slot is reachable only through
	mem2_alloc_pooled(size, MEM2_POOL_*) from the tenant that owns the pool:

	  * MEM2_POOL_FAR -- the far-ring column store's chunk block buffers
	    (chunk_alloc_far_voxel, called only from world.c's far install);
	  * MEM2_POOL_VIEW -- the `struct column_view` index blocks
	    (column_view_create).

	Every other request of the same size -- and 10,240 B is EVERY chunk's block
	buffer, near ones included (chunk.c:183; world.c only RETAGS the far ones
	CARVE_FAR after the fact) -- goes to the shared heap as before. A size-keyed
	pool was tried first and is a trap: the ~968 near-chunk buffers of the
	initial load drain every slot before the far ring asks for its first, so the
	far ring interleaves exactly as before while the census reads 100% used.

	SLOT COUNTS. Both are derived, not guessed, and both are pinned against
	their defines by tests/test_mem2_pool.c (which may include
	network/server_local_state.h; this header may not -- that header states it
	is only for TUs embedding struct server_local).

	 * FAR = 224 slots (7 full bitmap words). The far annulus at the shipped +1
	   ring, MAX_VIEW_DISTANCE + 1 = 6 on Wii, is (2*6+1)^2 - (2*5+1)^2 = 48
	   columns, and each column's voxel storage spans its surface band
	   (cy_lo..cy_hi from chunk_min..chunk_max in world.c's far install). The
	   GEOMETRIC worst case is a column spanning all COLUMN_HEIGHT = 8 Y-slices
	   (chunk.h:61-68), i.e. 48 * 8 = 384 slots = 4,030,464 B -- deliberately
	   NOT carved. Real Wii 6a2c5673 split_anchor_6m at fv6 measured 178 live
	   (a 3.7-chunk mean band); 224 covers that with 26% headroom and is a whole
	   number of bitmap words. Buying the last 160 slots would cost another
	   1.68 MB of exactly the long runs this pool exists to protect, to cover a
	   band Beta terrain does not produce inside one 16x16 section -- and the
	   cost of being wrong is bounded and visible, because a request past the
	   bound is COUNTED (pool_far_over) and served by the shared heap, never
	   dropped.
	 * VIEW = 288 slots (9 full bitmap words) = 2 * MAX_CHUNKS
	   (server_local_state.h:211; Wii (5*2+2)^2 = 144), the factor two because a
	   view outlives its column while a retirement is pending. The same run
	   measured 242 live.

	DEMAND-CARVED, IN SLABS (round 3, 2026-09-08). Those two numbers are now
	CAPS, not reservations. Both pools start EMPTY and carve one slab of
	MEM2_POOL_SLAB_SLOTS = 32 slots the first time a tagged request finds no
	free slot, up to MEM2_POOL_FAR_SLABS = 7 and MEM2_POOL_VIEW_SLABS = 9 slabs
	-- the same 224 / 288 ceiling, reached only if the scene asks for it. A far
	or view slab is never returned (see mem2_pool.h, WHY SLABS): those pools
	are the scene's high-water mark, not a cache. A wholly empty COLUMN slab
	above MEM2_POOL_COLUMN_BOOT_SLABS is (mem2_pool.h, SHRINK; colcap-1006).

	WHY: the boot carve was measured costing exactly what it was meant to save.
	Real Wii 703aef00 (split_anchor_6m 42/256 fv6 voxel, compaction off) at t30
	had the FAR pool holding 0 of 224 slots -- 2.35 MB idle -- while the shared
	heap was at 94 KB free with a 928 B largest block (load floor 92 KB, against
	368 KB on 6a2c5673 with no pools at all), and single-player floors fell with
	it: far_walk 9.8 -> 7.1 MB, perf_stress 14.7 -> 12.0 MB. The readiness-side
	win was real (heap free 1,062 KB vs 396, largest 51 KB vs 936 B, far peak
	178/224, view 242/288, no overflow) -- the cost was paying for all of it
	during the load, minutes before the far ring asked for its first slot.
	Slabs keep the readiness win and give the load its bytes back.

	SLAB SIZES. far: 32 * (10240 + 256) = 335,872 B payload + 256 B lead guard =
	336,128 B per carve, 7 slabs = 2,351,104 B at the cap. view:
	32 * (2112 + 256) = 75,776 B payload + 256 B lead = 76,032 B per carve, 9
	slabs = 681,984 B at the cap. The CAP total is the same 3,033,088 B round 2
	took at boot -- 8.0% of the 36 MB MEM2_HEAP_SIZE carve, inside the 3 MiB
	ceiling this lane was given -- but it is now reached only by a scene that
	fills it. This is a MOVE, not new memory: a carved slab and its red-zone
	guard leave the shared free list, which loses that capacity and loses the
	two block sizes that were cutting its long runs. MEM1 is untouched. The slot
	stride keeps the guard, so mem2_canary_fill / _verify behave
	byte-identically on a pooled pointer. Each SLAB asks the heap for its
	payload + MEM2_GUARD, so it carries a red zone like every other block and
	the observer's used == payload + blocks * 256 + overhead reconciliation
	stays exact.

	ACCOUNTING. Each slab is carve_note'd and tagged CARVE_SLOTS, so it is
	visible in the per-tenant census instead of hiding inside "free bytes that
	went missing"; a SLOT handed out of it is flagged CARVE_POOLED, which keeps
	it in its own tenant's b/n columns but out of every PHYSICAL total -- above
	all out of the heap BLOCK count, since a slot is not a block and counting it
	as one would make carve_observer.c's __lwp_heap_getinfo reconciliation
	report an error on every snapshot. The census reports pool_bytes as the
	CARVED total (not the cap) plus pool_far_slabs / pool_view_slabs, so a run
	shows how much of the ceiling the scene actually bought.
*/
#define MEM2_POOL_FAR_OBJECT 10240u	 /* CHUNK_SIZE^3 * 5/2, chunk.c:183 */
#define MEM2_POOL_FAR_SLOTS 224u
#define MEM2_POOL_VIEW_OBJECT 2112u	 /* COLUMN_VIEW_INDEX_BYTES */
#define MEM2_POOL_VIEW_SLOTS 288u
/* COLUMN (2026-09-11, Astra debate E): the five resident column arrays of a
   server chunk (ids 32768 + metadata 16384 + sky 16384 + torch 16384 +
   heightmap 256 = 82,176 B) as ONE slot, so long-lived column storage stops
   interleaving with variable-size mesh blocks in the shared free list -- the
   measured spill class at 42/256 (R2/R3: 37/67 column-array spills with
   4-6 MB free and a 37 KB largest hole). 288 slots = 9 slabs = two anchors'
   MAX_CHUNKS (144) each; MEM2_POOL_COLUMN_BOOT_SLABS are carved EAGERLY at
   mem2_init (a virgin heap: the only time 2.6 MB contiguous is guaranteed),
   the rest on demand. Single player needs ~121-144 columns = 4-5 slabs. */
#define MEM2_POOL_COLUMN_OBJECT 82176u
#define MEM2_POOL_COLUMN_SLOTS 288u
/* 4 since stg25 (2026-09-11): 5 slabs left the shared heap at 1.6 MB in the
   42/256 window with ZERO spills -- the reserve gate reads the shared heap,
   and the fifth slab's slots sat idle there. The pool still grows on demand. */
#define MEM2_POOL_COLUMN_BOOT_SLABS 4u
/* colcap-1006 (ASTRA-CC-R3 item 4): a wholly empty column slab above the
 * boot slabs goes back to the carve only while the pool keeps at least this
 * many free slots afterwards (half a slab of hysteresis, so a pool hovering
 * at a slab boundary does not carve and return 2.6 MB per column). */
#define MEM2_POOL_COLUMN_SHRINK_HEADROOM 16u
/* The same bounds expressed as slab counts -- the form the demand carve caps
 * on. Both slot counts are whole multiples of MEM2_POOL_SLAB_SLOTS by
 * construction (7 * 32 = 224, 9 * 32 = 288), which tests/test_mem2_pool.c
 * pins. */
#define MEM2_POOL_FAR_SLABS (MEM2_POOL_FAR_SLOTS / MEM2_POOL_SLAB_SLOTS)
#define MEM2_POOL_VIEW_SLABS (MEM2_POOL_VIEW_SLOTS / MEM2_POOL_SLAB_SLOTS)
#define MEM2_POOL_COLUMN_SLABS (MEM2_POOL_COLUMN_SLOTS / MEM2_POOL_SLAB_SLOTS)

/* Which pool a tagged request belongs to. MEM2_POOL_NONE is the ordinary
 * shared-heap path, i.e. exactly what mem2_alloc does. */
enum mem2_pool_id {
	MEM2_POOL_NONE = 0,
	MEM2_POOL_FAR,
	MEM2_POOL_VIEW,
	MEM2_POOL_COLUMN,
};

/* Allocate for a named pool tenant: a bitmap slot when `pool` is present, has
 * this exact object size and has a free slot, otherwise the ordinary carve/
 * fallback allocation with the miss counted against that pool. Release is
 * UNCHANGED -- mem2_free / mem2_free_sized recognise a pooled pointer by range,
 * which is what lets every existing free site stay as it is. On PC this is
 * plain mem2_alloc (no carve, no pools). */
void* mem2_alloc_pooled(size_t size, enum mem2_pool_id pool);
/* colcap-1006 (R4 point 3): PC test seam -- true when CW_TEST_COLUMN_POOL
 * selected a host emulation of the Wii column pool (mem2.c). Always false on
 * the Wii (the real pool) and on a normal PC run. */
bool mem2_column_pool_emulated(void);

/* Pool occupancy for the carve census (carve_observer.c's carve_census line).
 * Zeroed on PC, which has no carve and therefore no pools. */
struct mem2_pool_stat {
	/* `slots` is the CAP (slabs_max * MEM2_POOL_SLAB_SLOTS), unchanged from
	 * round 2 so an existing reader of pool_far_slots still sees the bound;
	 * `slabs` / `slabs_max` and `bytes` say how much of it the scene has
	 * actually carved. `carve_fail` is the subset of `overflow` caused by a
	 * heap that had no room for another slab rather than by the cap -- a
	 * different finding, so it is counted separately. */
	uint32_t slots, used, peak, overflow;
	uint32_t slabs, slabs_max, carve_fail;
	uint64_t bytes;
	/* colcap-1006 R3 item 4: slabs handed back to the carve (column pool
	 * only) and hand-backs the heap refused (the slab is then kept). */
	uint32_t shrinks, shrink_fail;
};
void mem2_pool_column_stats(struct mem2_pool_stat* out);
/* colcap-1006 (ASTRA-CC-R3 item 3): the column pool's stats AND its per-slab
 * occupancy (live slots per carved slab, slab order = carve order, boot slabs
 * first) from ONE lock acquisition, so sum(per_slab) == out->used exactly.
 * Writes min(slabs, cap) entries; returns that count. Telemetry only: reads
 * the bitmap, never allocates or moves anything. Zero on PC (no pool). */
unsigned mem2_pool_column_occupancy(struct mem2_pool_stat* out,
									uint8_t* per_slab, unsigned cap,
									uint32_t* event_seq, uint32_t* events_lost);
/* colcap-1006 (R4 point 9): deliver queued column-pool grow/shrink events,
 * oldest first (mem2_pool_evq.h), at most a small fixed budget per call.
 * Called once per heartbeat (never on the allocation path), which also
 * retries an event the telemetry ring refused. `event_seq` above is the seq of the last QUEUED event at the
 * snapshot; `events_lost` counts events the queue could not hold. No-op on
 * PC. */
void mem2_pool_column_events_flush(void);

/* colcap-1006 (ASTRA-CC-R5 item 2): COLUMN-POOL SELF-TEST HOOKS, driven only
 * by source/game/pool_selftest.c under the pool_selftest_3m hw_run profile.
 * INERT until mem2_pool_selftest_arm(): every other run -- and every play
 * session of a release build -- keeps the production placement
 * (lowest-index-first) and the real heap-return path; the only cost there is
 * one predictable branch on the column take and on a column hand-back.
 *   arm(placement)       latch the self-test and select the column take
 *                        policy (ROTATING = the pre-R4 take, the comparison
 *                        leg of the ordered-GX equivalence). Returns false
 *                        where no column pool exists (PC without the
 *                        CW_TEST_COLUMN_POOL emulation).
 *   refuse_handbacks(n)  the next n empty-slab hand-backs are refused as if
 *                        the heap had rejected the free: counted in
 *                        shrink_fail, the slab KEPT and still usable -- the
 *                        failure path of mem2_pool_column_try_shrink_locked.
 *   locate(ptr)          slab index and slot of a live column slot (false
 *                        for a pointer the pool does not own).
 *   placement_digest     FNV-1a over (slab, slot) of every column take since
 *                        arm, and the take count: proves two runs really
 *                        placed columns differently. */
enum mem2_pool_placement {
	MEM2_POOL_PLACE_LOWEST = 0,
	MEM2_POOL_PLACE_ROTATING = 1,
};
bool mem2_pool_selftest_arm(enum mem2_pool_placement placement);
bool mem2_pool_selftest_armed(void);
void mem2_pool_selftest_refuse_handbacks(unsigned count);
unsigned mem2_pool_selftest_refusals_pending(void);
bool mem2_pool_column_locate(const void* ptr, int* slab, int* slot);
uint32_t mem2_pool_selftest_placement_digest(uint32_t* takes);
/* The column pool the self-test drives (Wii: the real pool, PC: the
 * emulation) with per-slab occupancy, one lock acquisition. */
unsigned mem2_pool_selftest_stats(struct mem2_pool_stat* out,
								  uint8_t* per_slab, unsigned cap);
/* colcap-1006 (ASTRA-CC-R5 item 3): allocator CPU / lock cost on Broadway,
 * taken (and reset) once per anchor heartbeat:
 *   [0] column-pool allocations   [1] their total us   [2] the longest us
 *   [3] column-slot frees         [4] their total us   [5] the longest us
 *   [6] CONTENDED mem2-lock waits [7] total us waited  [8] the longest us
 * An allocation/free is timed from entry to just before its unlock, so its
 * time INCLUDES any lock wait, the demand carve / hand-back heap walk and
 * the canary work; [6..8] isolate the wait itself (every mem2 caller, every
 * thread). Cost of the instrument: two timebase reads per mem2_free and per
 * column allocation, updates under the lock already held. PC: zeros. */
#define MEM2_COST_FIELDS 9u
void mem2_cost_take(uint32_t out[MEM2_COST_FIELDS]);
void mem2_pool_stats(struct mem2_pool_stat* far_out,
					 struct mem2_pool_stat* view_out);

/* Attribute every slab carved SO FAR to CARVE_SLOTS. Called by
 * carve_observer_start: slabs carved before an observer existed (or under a
 * previous one) have to be replayed into the fresh ledger -- every slab the
 * pool still HOLDS (a column slab handed back by the R3 item 4 shrink was
 * already noted CARVE_FREE and is no longer listed). Slabs carved while an observer is armed are
 * noted at carve time by mem2_allocate_pooled instead. No-op on PC. */
void mem2_pool_note_reservations(void);

/* ======================================================================
 * MP COMPRESSED-BODY BACKING RESERVATION (CARVE_MP_BACKING)
 * ======================================================================
 * docs/mp_admission_budget_design_2026-09-13.md section 2.4, W2 of
 * mp-split-0912. Every zlib-compressed Packet51 body the MP client holds --
 * the decoder's (mp_codec.c), the retained head/chain
 * (server_remote_chunks.c) and the demotion/compaction re-encodes -- lives
 * in ONE fixed reservation instead of the MEM1 malloc heap. Two things that
 * buys, both measured on the real Wii run betamc_0913: the 441-column join
 * burst stops collapsing MEM1 (tick 15, mem1_free_kb 16), and the budget
 * becomes a number that can be CHECKED BEFORE allocating (Astra gate-2 item
 * 4) instead of a cap that triggers destructive eviction after the fact.
 *
 * WHY A FIRST-FIT FREE LIST and not mem2_alloc_pooled: a body is a
 * variable-length object (measured 3-9 KB for ordinary terrain, up to
 * MP_CHUNK_COMPRESSED_CAP for a pathological one). The slot pools above are
 * FIXED-object-size bitmaps by construction, so a pooled slot would have to
 * be sized for the worst case and would waste most of the reservation.
 *
 * WHY LAZY, and why the reservation is RETURNED at session end (F5a, review
 * finding M9): single player never allocates an MP body, so carving 6 MiB at
 * mem2_init would take it away from meshes and columns in the session that
 * needs them most. The region is therefore carved on the FIRST
 * mp_backing_alloc -- which is the MP join burst, when the client's world is
 * still empty and the carve is at its least fragmented -- and handed BACK to
 * the carve when the MP session ends (mp_backing_release_if_idle, called from
 * server_remote_cancel after remote_chunks_clear_locked has returned every
 * body). W2 shipped it as a one-shot slab like a pool slab; that made
 * MP -> title -> load a local world run permanently 6 MiB short of carve on
 * the platform where the mesh/far-ring budget is already the binding
 * constraint. It is NOT a pool slab: nothing hands out addresses inside it
 * across sessions, so releasing it is sound where releasing a pool slab is
 * not. If the heap cannot give the full want, the carve
 * retries at 3/4, 1/2 and 1/4 down to MEM2_MP_BACKING_MIB_MIN and reports
 * whatever it actually got as the capacity; only a total refusal makes
 * mp_backing_alloc fail, which the MP layer turns into a clean, visible
 * capacity failure (mp_end=5), never an OOM crash.
 *
 * THREAD SAFETY: bodies are allocated on the READ thread (the decoder,
 * outside the remote lock) and freed on either the read or the main/pump
 * thread, so the allocator carries its own lock -- the same serialising
 * mem2 mutex on the Wii, a private mutex on PC.
 *
 * PC has no carve; the region is one malloc of the same size and the SAME
 * allocator runs on top of it, so a PC test exercises the real capacity
 * arithmetic rather than an emulation of it. */
/* F4 (mp-split-0912): the Wii default is 6 MiB, DOWN from W2's provisional
 * 8 MiB, and it is sized from the measurement W2 asked for rather than from
 * a guess. BetaMC's whole 441-column view is 1.93 MB compressed on the wire
 * (run betamc_0913), i.e. ~4.5 KB/column, so 6 MiB holds it with a 3x
 * margin; the acceptance fixture's poor-compression bound is 441 x 15 KB =
 * 6.6 MB, which 6 MiB does NOT hold. That is a deliberate, documented trade
 * and not a silent cap: a 15 KB/column server hits the CLEAN capacity
 * failure (mp_end=5, "does not fit this console"), never an OOM, and the
 * player raises `mp.backing_mib` to 8 -- the number W2 shipped and case (h)
 * still proves -- to play it. The 2 MiB bought back is spent where the same
 * run showed the wall actually was: mem2_carve_free_kb went 25570 -> 39 and
 * ndisp stayed 0 for 60 s, i.e. the console had the world resident and no
 * carve left to MESH it. Backing bytes and mesh bytes come out of the same
 * carve, so the reservation is exactly the dial that pays for
 * REMOTE_MESH_RESERVE_BYTES (server_remote_chunks.c). */
#ifdef PLATFORM_WII
#define MEM2_MP_BACKING_BYTES (6u * 1024u * 1024u)
#else
#define MEM2_MP_BACKING_BYTES (12u * 1024u * 1024u)
#endif
/* config `mp.backing_mib` clamp (design section 2.4). The floor is the
 * smallest reservation that still holds a Beta 21x21 view of ordinary
 * terrain; the ceiling is what the 38.5 MiB carve can lend without starving
 * the resident ring it exists to serve. */
#define MEM2_MP_BACKING_MIB_MIN 2u
#define MEM2_MP_BACKING_MIB_MAX 16u

/* Set the reservation size, clamped to [MEM2_MP_BACKING_MIB_MIN,
 * MEM2_MP_BACKING_MIB_MAX] MiB. Takes effect on the next carve: if the region
 * is not reserved yet it simply sets the want; if it IS reserved and
 * completely EMPTY it is released and re-reserved at the new size (this is
 * what lets a test run the 8 MiB Wii budget on PC, and what a per-connection
 * config change does between sessions); if the region is reserved and holds
 * live bodies the call is IGNORED -- moving bodies under their owners is not
 * something this allocator can do, and silently shrinking the capacity out
 * from under live accounting would be worse than not applying the setting.
 * Returns the capacity in effect after the call. */
size_t mp_backing_configure(size_t bytes);
/* Allocate `length` bytes for ONE compressed body. Returns NULL when the
 * reservation cannot hold it -- which is a CAPACITY answer, never a fallback
 * to another heap: the caller must turn it into the clean session end the
 * design requires. */
void* mp_backing_alloc(size_t length);
/* Release a body. NULL is a no-op. Passing a pointer this allocator did not
 * hand out is a programming error, so it asserts rather than guessing. */
void mp_backing_free(void* body);
/* True when `body` lies inside the reservation. The MP codec's free sites
 * serve both reservation-backed map bodies and plain-malloc'd multi-block /
 * explosion bodies through one field, and this range test is what keeps a
 * reservation alloc from ever meeting a plain free() (or the reverse). */
bool mp_backing_owns(const void* body);
/* Bytes requested by live bodies, the high-water mark of that figure, and the
 * reservation capacity (0 until the first carve; the WANT is reported so a
 * budget can be read before any body exists). Any pointer may be NULL. */
void mp_backing_stats(size_t* in_use, size_t* peak, size_t* capacity);
/* Tests only: forget the whole reservation (asserts it is empty) so the next
 * case starts from a virgin region and a cleared peak. */
void mp_backing_reset(void);
/* F5a (review finding M9): hand the reservation AND the scratch side budget
 * back to the carve, but only when nothing is live in either. Called at MP
 * session end (server_remote_cancel, after remote_chunks_clear_locked has
 * freed every body and released every scratch slot). Returns the bytes given
 * back -- 0 when something was still live, which is a legitimate answer and
 * not an error: a body outliving the clear would be a leak for the leak check
 * to report, and a second session sharing the process (the test suite's
 * fixtures) keeps the region for as long as it needs it. Deliberately NOT
 * called from mp_backing_free: a mid-burst dip to zero live bodies must not
 * churn the carve. */
size_t mp_backing_release_if_idle(void);

/* ----------------------------------------------------------------------
 * SCRATCH SIDE BUDGET (F5a, review finding L10)
 * ----------------------------------------------------------------------
 * The two working buffers on the same body path used to be bare MEM1
 * `malloc`s outside every budget: the demotion/compaction re-encode scratch
 * (one per connection, 163,904 B) and the promotion inflate scratch (one per
 * promotion, up to MP_CHUNK_INFLATED_CAP = 81,920 B). Net MEM1 improved when
 * the bodies left it, but those two stayed exactly where the betamc_0913 run
 * showed MEM1 collapsing, and no telemetry field reported them.
 *
 * They now live in a small side region acquired through the SAME carve shim
 * as the reservation (so they are MEM2/CARVE_MP_BACKING bytes on the Wii) and
 * released by the same mp_backing_release_if_idle at session end. It is a
 * FIXED-SLOT arena, not a free list, and that is the point: each slot's
 * offset is fixed on its first acquire and every later acquire of that slot
 * reuses it, so a slot can never fail for want of a contiguous run -- the
 * reclaim ladder (server_remote_chunks.c) NEEDS its re-encode scratch exactly
 * when the body reservation is full, and an allocation that competes with the
 * bodies it is trying to reclaim would deadlock the ladder.
 *
 * STILL OUTSIDE: zlib's own inflate window (~44 KB, allocated by zlib's
 * default zalloc). Routing zlib through this arena needs a general allocator,
 * not fixed slots; it is named here rather than quietly ignored. */
enum mp_backing_scratch_slot {
	/* remote_demote_scratch_locked: raw column + deflate staging. */
	MP_BACKING_SCRATCH_RECODE = 0,
	/* remote_start_promotion_locked: the inflated column. */
	MP_BACKING_SCRATCH_INFLATE,
	/* REL-MP-JOINSPEED: the deferred-edit pool, REMOTE_DEFERRED_EDIT_CAPACITY
	 * x 16 B = 262,144 B (server_remote_internal.h). */
	MP_BACKING_SCRATCH_EDITS,
	MP_BACKING_SCRATCH_SLOTS
};
/* 288 KB covered the first two slots above (163,904 + 81,920 = 245,824 B)
 * with room for the alignment padding; REL-MP-JOINSPEED adds the 256 KiB
 * edit pool (262,144 B), so 544 KiB. It is a compile-time ceiling, and the
 * region is only as large as this constant, once. */
#define MEM2_MP_SCRATCH_BYTES (544u * 1024u)
/* Claim `bytes` of slot `slot`. The FIRST claim of a slot fixes its size (and
 * its offset inside the side region); a later claim must not ask for more
 * than that first one did. Returns NULL only when the side region itself
 * cannot be acquired or the slot does not fit -- never because another slot
 * is busy. Claiming a slot that is already claimed returns the same pointer
 * (the caller owns it for the whole session), which is what makes
 * remote_demote_scratch_locked's lazy-once idiom work unchanged. */
void* mp_backing_scratch_acquire(enum mp_backing_scratch_slot slot,
								 size_t bytes);
/* Release a slot (idempotent). The side region itself goes back to the carve
 * only through mp_backing_release_if_idle. */
void mp_backing_scratch_release(enum mp_backing_scratch_slot slot);
/* Bytes claimed by live slots, the high-water of that figure, and the side
 * region's capacity (the constant above until it is acquired, then its actual
 * size). Any pointer may be NULL. Reported separately from
 * mp_backing_stats on purpose: that function's three numbers are the BODY
 * budget the capacity arithmetic and the mp_adm_backing_kb heartbeat field
 * are defined against, and folding a fixed side budget into them would move
 * every one of those readings without making any of them more true. */
void mp_backing_scratch_stats(size_t* in_use, size_t* peak, size_t* capacity);

// Hand out the audio PCM region. FIRST CALLER WINS: the granted base and size
// are latched on the first call and every later call returns exactly the same
// pointer and size, whatever `want` says -- that is what makes
// audio_registry_reload safe (it drains the voices and re-fills the SAME
// arena in place, so the region must be reusable, never re-reserved).
//
// FAIL-SOFT, never a silent success: `*out_size` is the bytes actually granted,
// which may be LESS than `want` when arena2 could not hold the full region
// after the carve took its share. The caller must honour the granted size (the
// registry then simply resolves fewer variants and says so). Returns NULL with
// *out_size = 0 when no region exists at all; audio is then off for the
// session. NEVER falls back to the general heap: a 12 MB memalign would land in
// the 24 MB MEM1 pool, which is the exact pathology this move exists to remove.
//
// MAIN THREAD ONLY, same contract as every audio_* call.
void* mem2_audio_region(size_t want, size_t* out_size);

// Every region base is 32-byte aligned: GX texture data and the AESND/DSP DMA
// both require it, and the audio arena's per-sample alignment is only as good as
// its base (audio_arena_alloc bumps by 32-aligned SIZES, which preserves an
// aligned base but cannot create one).
#define MEM2_REGION_ALIGN 32u

// The MEM2 region split, as pure arithmetic so it is testable off-console (the
// Wii branch below is the only caller; the PC branch has no regions at all).
// Offsets are relative to arena2 lo. See mem2.c for the clipping order.
struct mem2_region_plan {
	size_t carve_off;	   // lwp display-list/GX carve
	size_t carve_size;
	size_t audio_off;	   // audio PCM region
	size_t audio_size;	   // 0 when arena2 had nothing left for it
	size_t reserved_total; // carve + audio == the SYS_SetArena2Lo bump
	size_t spill_size;	   // what is left of arena2 for the malloc spill
};

// Split [0, avail) into the two regions. Returns false (and zeroes *out) when
// there is no usable carve at all, which is the caller's "leave arena2 alone"
// signal. Compiled on BOTH platforms so the PC suite can pin the goldens.
bool mem2_plan_regions(size_t avail, size_t carve_want, size_t audio_want,
					   size_t margin, struct mem2_region_plan* out);

// Allocate `size` bytes, 32-byte aligned (a GX texture-data requirement), from
// the MEM2 heap; falls back to the normal aligned heap (MEM1 on Wii) if MEM2 is
// unavailable or exhausted -- so worst case is exactly the pre-MEM2 behaviour,
// never a failure that the caller must handle.
void* mem2_alloc(size_t size);
// Capture storage: MEM2 carve only on Wii; NULL on exhaustion, never MEM1.
void* mem2_alloc_strict(size_t size);

// Free a buffer from mem2_alloc. Pointer-range aware: a buffer that landed in
// the MEM2 heap is returned there, a fallback buffer goes back to the normal
// heap, and a non-mem2 pointer (e.g. a plain malloc) is just free()d -- so it is
// safe to use uniformly for any of those. MOST texture texels are persistent
// and never freed; this exists for the transient per-LOD mipmap buffers, which
// ARE freed during a mipmapped texture build.
void mem2_free(void* ptr);

// realloc for mem2 buffers: returns a fresh `new_size` allocation holding the
// first min(old_size, new_size) bytes of `ptr`, and frees `ptr`. mem2 has no
// in-place grow, so this always copies -- fine for the few-times-per-mesh
// display-list growth. `old_size` is the current allocation size; `ptr` may be
// NULL (then it behaves as mem2_alloc).
void* mem2_realloc(void* ptr, size_t old_size, size_t new_size);

/* Finalization only: shrink an owned buffer, retaining new_size bytes and
 * the Wii 256-byte canary. Carve allocations release their tail in place
 * (no copy); general-heap fallback may move, still 32-byte aligned on Wii.
 * NULL leaves the original allocation/size intact. Never grows. */
void* mem2_shrink(void* ptr, size_t old_size, size_t new_size);

// Session diagnostics: cumulative alloc/free/realloc call counts and total bytes
// requested via mem2_alloc. Cheap counters (no per-pointer bookkeeping). Drive the
// test_mem1_budget regression guard (proving the ~80KB/chunk server chunk arrays route
// through MEM2, off the capped MEM1 heap, and are all freed) and can feed an F3 dial.
size_t mem2_stat_alloc_calls(void);
size_t mem2_stat_free_calls(void);
size_t mem2_stat_realloc_calls(void);
size_t mem2_stat_alloc_bytes(void);
// Wii MEM2-carve health: count of allocs that spilled from the carve to the MEM1
// fallback (carve fragmented/full), and of true NULL allocs (even the fallback failed ->
// the partial-mesh-truncation freeze path). 0 on PC. THE numbers that say whether the
// audio arena's move out of the carve worked. Surfaced in all three shipping sinks:
// the F3 "spill %zu oom %zu" row (screen_ingame.c), the watchdog hang report + its
// cell-bar rows 7/8 (wii/watchdog.c), and the telemetry heartbeat (main.c). Healthy
// after a full session is 0 and 0; anything climbing means the carve is still short.
size_t mem2_stat_carve_spill(void);
size_t mem2_stat_oom(void);
// SPLIT-SPILL (2026-10-14): guarded bytes (request + MEM2_GUARD) of the most
// recent request that spilled out of the carve, recorded where
// mem2_stat_carve_spill counts it. 0 before any spill and always 0 on PC.
// A size class for attribution, not a tenant identity.
uint32_t mem2_stat_last_spill_bytes(void);
// P2-c telemetry (2026-09-03 round-2 frame-drop root-cause doc, mechanism A):
// cumulative count of a thread finding the Wii mem2 serialising mutex ALREADY
// HELD and having to actually block for it, vs an uncontended try-lock. 0 on
// PC (no MEM1/MEM2 split, no lock, no contention possible). A climbing count
// under chunk-load pressure is the direct evidence for "the mesher's MEM2
// churn contends with the GP/main thread's own MEM2 traffic", independent of
// the mesher's own wall-clock build time (ms_mesh_worker below).
uint32_t mem2_stat_lock_wait(void);
// The sizes mem2_init actually landed on (0 on PC, which has no regions). Read
// them together with the two counters above: carve_size < MEM2_HEAP_SIZE means
// arena2 was too small and the carve itself was clipped, which is the one way a
// spill storm can return. The audio region's granted size is ALSO the F3 "snd:
// arena used/total" denominator (screen_ingame.c row 20), so a clipped audio
// reservation is photographable without a new overlay row.
size_t mem2_stat_carve_size(void);
size_t mem2_stat_audio_region_size(void);
// T-c (2026-09-03, Round 3 (2)(a) MEM2 gauge fix): free bytes (KB) remaining
// IN THE CARVE HEAP ITSELF right now (Wii: __lwp_heap_getinfo on mem2.c's
// mem2_heap; PC: -1, no carve). Distinct from mem2_free_kb (main.c's
// SYS_GetArena2Size() reading), which is the arena2 remainder ABOVE the
// carve and is blind to carve occupancy -- Round 3 (2)(a) measured
// mem2_free_kb staying flat while mem2_stat_carve_spill climbed into the
// tens of thousands as reach 8->16 exhausted the carve. This is the number
// that predicts a spill BEFORE it happens, rather than confirming one after.
// -1 if the carve heap was never initialised (mem2_ready false) or getinfo
// itself fails.
int mem2_stat_carve_free_kb(void);

/* F4 (mp-split-0912, design 2.3/2.4): the SAME reading in exact bytes, for a
 * caller that must compare it against an allocation it is about to make
 * rather than display it. mem2_stat_carve_free_kb is a gauge (int KB, -1 for
 * "no carve"); this is the arithmetic primitive the MP promotion gate tests
 * `free >= column + mesh reserve` with, and the _kb gauge is now derived from
 * it so there is one getinfo call site and one lock discipline.
 *
 * SIZE_MAX means "there is no carve to measure, do not gate": always on PC
 * (mem2_alloc is plain malloc there, so no carve ceiling exists), and on the
 * Wii only when the carve heap was never initialised or __lwp_heap_getinfo
 * itself fails. A caller must treat SIZE_MAX as "unknown, behave as before
 * the gate existed" and let the allocation's own NULL check answer -- never
 * as "no room", which would refuse every column on a healthy console.
 *
 * What the reading already accounts for: every carve tenant that has taken
 * its bytes, including the fixed far/column-view pool slabs and the lazily
 * carved CARVE_MP_BACKING reservation (mem2.h above) -- once reserved, those
 * bytes are out of the heap's free list, so a gate built on this number does
 * not have to model them. Display lists (CARVE_LIST) live in the same free
 * list, which is exactly why a mesh reserve has to be subtracted by hand:
 * the mesher allocates AFTER promotion, so its bytes are not yet spent at
 * the moment the gate runs. */
size_t mem2_carve_free_bytes(void);

/* colcap-1006 (ASTRA-CC-R2 item 5): CONTIGUITY, not just total free bytes.
 * A column slab is ONE first-fit block of lead + 32 x stride bytes
 * (mem2_pool_grow), so a total-free reading cannot say whether slab seven or
 * eight will fit; the largest single hole decides it.
 *
 * The libogc rule that turns a free block's PHYSICAL size into the largest
 * REQUEST that block can satisfy (devkitPro libogc v3.1.0,
 * libogc/lwp_heap.c __lwp_heap_allocate, and gc/ogc/lwp_heap.h):
 *   dsize = size + pg_size + HEAP_BLOCK_USED_OVERHEAD, plus
 *           (pg_size - size % pg_size) when size is not a page multiple;
 *   a free block is taken iff its front_flag (physical size, header
 *   included) >= dsize.
 * HEAP_BLOCK_USED_OVERHEAD is sizeof(void*) * 2 = 8 on PPC32 and mem2_heap's
 * pg_size is 32 (mem2_init's __lwp_heap_init(..., 32)). The extra pg_size is
 * the alignment slack the allocator spends to page-align the user pointer.
 * So the largest satisfiable request is the page multiple
 *   floor((block - pg - overhead) / pg) * pg, or 0 when block < 2 pg + overhead.
 * The physical block size mem2_carve_snapshot reports is NOT this number and
 * must not be relabelled as it. Portable so the arithmetic is tested on PC. */
static inline size_t mem2_lwp_allocatable_from_block(size_t block_bytes,
													 size_t pg_size,
													 size_t used_overhead) {
	if(!pg_size || block_bytes < 2 * pg_size + used_overhead)
		return 0;
	return (block_bytes - pg_size - used_overhead) / pg_size * pg_size;
}
/* Largest single request the carve heap can satisfy RIGHT NOW (rule above,
 * applied to the largest free block of one bounded boundary-tag walk under
 * mem2_lock). Returns the VALIDITY: false (and *out_bytes = 0) on PC (no
 * carve), before mem2_init, after a heap-integrity failure, or when the walk
 * did not end exactly on the heap's final block. Costs one O(blocks) walk:
 * call it at heartbeat cadence, never per allocation. */
bool mem2_carve_largest_allocatable_bytes(size_t* out_bytes);
/* colcap-1006 review (Sol #1): the free-extent walk over libogc boundary
 * tags, hardened so a corrupted tag can never steer a read off alignment or
 * out of the arena. Layout (gc/ogc/lwp_heap.h, PPC32; mem2.c static-asserts
 * sizeof(heap_block) == 16): word 0 back_flag, word 1 front_flag = block size
 * | HEAP_BLOCK_USED (bit 0). Every block starts 4-byte aligned, is at least
 * one heap_block (16 B) and a multiple of 4, ends at or before `final`, and
 * its successor's back_flag repeats its front_flag. Any violation returns
 * false (INVALID) BEFORE the next header is read; nothing is written. The
 * caller (mem2.c) runs the full structural validator (mem2_heap_validate via
 * mem2_diag_validate_locked) first; these checks are the walker's own guard,
 * not a substitute. Bounded by (final - base) / 16 + 1 steps. Portable so a
 * corrupted image is tested on PC (test_split_column_cap heap_walk_corrupt). */
static inline bool mem2_heap_free_extent_raw(uintptr_t base, uintptr_t final,
											 size_t* free_total,
											 size_t* largest) {
	*free_total = 0;
	*largest = 0;
	if(!base || (base & 3u) || final < base || ((final - base) & 3u))
		return false;
	uintptr_t p = base;
	for(size_t steps = (final - base) / 16u + 1u; p < final; steps--) {
		if(!steps || final - p < 8u)
			return false;
		const uint32_t front = ((const uint32_t*)p)[1];
		const size_t size = (size_t)(front & ~(uint32_t)1u);
		if(size < 16u || (size & 3u) || size > final - p)
			return false;
		const uintptr_t next = p + size;
		if(((const uint32_t*)next)[0] != front)
			return false;
		if(!(front & 1u)) {
			*free_total += size;
			if(size > *largest)
				*largest = size;
		}
		p = next;
	}
	if(p != final) {
		*free_total = *largest = 0;
		return false;
	}
	return true;
}
/* Temporary cold-stall diagnostics: relaxed, lock-free snapshots only. */
uint32_t mem2_diag_op(void);
uint32_t mem2_diag_thread(void);
uint32_t mem2_diag_bad(void);
uint32_t mem2_diag_scan_count(void);
uint32_t mem2_diag_scan_max_us(void);
uint32_t mem2_diag_scan_total_us(void);

// Size-aware free: on Wii, VERIFIES the trailing red-zone canary of this block
// before releasing it (an overrun into the guard names the smashed block at
// release time, instead of a frozen console minutes later when the lwp_heap
// walks a corrupted free-list). Use at every release site that knows the
// original mem2_alloc size (display lists, chunk block buffers, server chunk
// arrays -- the heavy churn surfaces). On PC it is identical to mem2_free (no
// guard bytes exist there; ASan covers that platform instead).
void mem2_free_sized(void* ptr, size_t size);

// Canary diagnostics: total guard violations seen, and the first offender
// (block base, alloc size, first corrupt byte offset into the guard). 0/NULL on
// PC. Surfaced on the F3 far row ("cnry") and in the watchdog hang report.
uint32_t mem2_canary_hits(void);
void mem2_canary_first(void** ptr, size_t* size, size_t* guard_off);

/* Tag AFTER the original allocator call: preserves existing --wrap=mem2_alloc
 * fault injection and every allocation size/order. NULL tags attribute failures. */
static inline void* mem2_tag(void* ptr, size_t size, enum carve_tenant tenant) {
	if(mem2_carve_observe) {
		carve_note(mem2_carve_observe, CARVE_BEGIN, 0, 0, 0, 0);
		carve_note(mem2_carve_observe, CARVE_TAG, ptr, 0, size, tenant);
		carve_note(mem2_carve_observe, CARVE_END, 0, 0, 0, 0);
	}
	return ptr;
}
/* Exact allocator + ledger sample under the existing Wii heap lock. */
void mem2_carve_snapshot(void (*sample)(size_t, size_t, size_t, size_t));

#endif
