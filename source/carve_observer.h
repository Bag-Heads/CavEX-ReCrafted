/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef CARVE_OBSERVER_H
#define CARVE_OBSERVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NULL hooks are owned by the producer TUs. Arm before allocation/threads;
 * only disarm after all producers have joined. Never changes engine storage. */
enum carve_action {
	CARVE_BEGIN, CARVE_END, CARVE_ALLOC, CARVE_FREE, CARVE_RESIZE,
	CARVE_TAG, CARVE_TRANSFER, CARVE_FINAL, CARVE_DL_INIT, CARVE_DL_HINT,
	CARVE_OWNER
};
enum carve_tenant {
	CARVE_OTHER, CARVE_COLUMN, CARVE_VIEW, CARVE_OVERLAY, CARVE_PRIVATE,
	CARVE_VOXEL, CARVE_FAR, CARVE_LIST, CARVE_POPULATE, CARVE_RESIDENCY,
	CARVE_TEXTURE,
	/* The fixed-tenant slot pools' one-shot reservations (mem2.h). They are
	 * carved before any thread exists and never freed, so without their own
	 * tenant several MB of carve capacity would simply be missing from the
	 * per-tenant totals -- the pool hands out SLOTS, which are tagged by their
	 * own tenant (CARVE_FAR / CARVE_VIEW), so nothing else ever accounts the
	 * reservation itself. Noted to the observer at carve_observer_start,
	 * because the reservation predates it (mem2_pool_note_reservations). */
	CARVE_SLOTS,
	/* The MP compressed-body backing reservation (platform/mem2.h,
	 * MEM2_MP_BACKING_BYTES; design section 2.4). Its own tenant for the same
	 * reason CARVE_SLOTS exists: the reservation is carved ONCE, sub-allocated
	 * internally by mp_backing_alloc and never handed to the shared heap, so
	 * without a tenant of its own several MB of carve capacity would simply
	 * be missing from the per-tenant census. Bodies inside it are NOT
	 * carve_note'd -- the reservation already accounts every byte they can
	 * ever occupy, and noting them would double-count (the same rule
	 * CARVE_POOLED encodes for pool slots). */
	CARVE_MP_BACKING,
	CARVE_TENANTS
};
/* CARVE_POOLED: this allocation is a SLOT inside a fixed-tenant pool
 * (platform/mem2.h), not a block of the shared __lwp_heap. It is still in the
 * carve and still belongs to its tenant, but the PHYSICAL bytes were already
 * accounted once by the pool one-shot reservation (tenant CARVE_SLOTS), so
 * snapshot_locked must not count it a second time -- and above all must not
 * count it as a heap BLOCK, or the used-block reconciliation against
 * __lwp_heap_getinfo would report an error on every snapshot. */
enum carve_flags {
	CARVE_IN_HEAP = 1, CARVE_SPILLED = 2, CARVE_MEM2 = 4, CARVE_POOLED = 8
};
typedef void (*carve_callback)(enum carve_action, uintptr_t, uintptr_t,
	size_t, size_t);
extern carve_callback mem2_carve_observe, displaylist_carve_observe;
#define carve_note(hook, action, p, q, n, aux) do { \
	if(hook) (hook)((action), (uintptr_t)(p), (uintptr_t)(q), (n), (aux)); \
} while(0)

struct world;
struct chunk;
struct server_local;
extern void (*server_carve_boundary)(struct server_local*);
extern void (*server_carve_retirement)(struct server_local*, unsigned, uint32_t);
/* Disjoint payload classes. Placement: 0 carve, 1 MEM1 heap, 2 MEM2 heap.
 * PC uses placement 2 for its ordinary heap; records carry pc=1. */
enum carve_class {
	CARVE_A_OTHER, CARVE_A_COLUMN_UNKNOWN, CARVE_A_PLAYER,
	CARVE_A_DEPENDENCY, CARVE_A_SUPPORT_READY, CARVE_A_LEASED,
	CARVE_A_FAR, CARVE_A_LIST_PENDING, CARVE_A_LIST_LIVE,
	CARVE_A_LIST_RETAINED, CARVE_A_OVERLAY, CARVE_A_GUARD,
	CARVE_A_OVERHEAD, CARVE_CLASSES
};
struct carve_attrib {
	uint64_t bytes[CARVE_CLASSES][3], count[CARVE_CLASSES][3];
	uint64_t used, free, largest, errors;
	uint32_t columns[5]; /* unknown/player/dependency/ready-support/leased */
	uint32_t frame, tick, phase, owner_tick;
	uint32_t eligible, retired, save_failed;
	/* Frame-observations for this point that were absorbed (latest values
	 * win) before this record reached the wire -- see the emission-cadence
	 * contract on carve_observer_frame/carve_observer_emit_next below. Zero
	 * for a genuine point transition (never coalesced away). */
	uint32_t coalesced;
};
/* Frame-observed extrema, plus the exact anchor_ready caller boundary.
 * Three retained records; no per-frame queue or second pointer ledger.
 *
 * Emission cadence (2026-09-07, the carve-observer sender-saturation fix):
 * carve_observer_frame keeps every point's true extremum exact every single
 * frame (carve_observer_attrib always reads the real value), and folds each
 * new observation into that point's queued record (latest values win,
 * counted in the record's `coalesced` field) rather than minting a second
 * one while a send is still outstanding. The actual THROTTLE is in
 * carve_observer_emit_next: a point's very first successful send -- its
 * true transition into loading-high-water / readiness / low-water -- is
 * NEVER delayed or coalesced away, but every later send FOR THAT SAME POINT
 * is capped to at most one per ~1 s (60 frames at the Wii's 60 Hz main
 * loop), independently per point -- not one clock shared by all three: a
 * shared clock lets one continuously-churning point (e.g. session
 * low-water, active the whole run) perpetually win every reopening and
 * starve another (e.g. loading high-water, only active pre-ready), freezing
 * its reported value at its very first sample forever, which defeats the
 * whole point of tracking an extremum. What was missing before this fix:
 * repeatedly re-sending a still-dirty pending record on every round-robin
 * turn, unthrottled, is exactly the ~16 records/s the sender-saturation
 * hardware evidence measured. The window is frame-counted (not wall-clock),
 * so this is deterministic under a test harness that drives frames without
 * sleeping. */
void carve_observer_frame(struct world* w, uint32_t frame, uint32_t tick,
	uint32_t phase, bool ready);
/* LOADING-PHASE CADENCE (2026-09-08). Main-thread-owned, set every frame:
 * true while the world is still loading OR an anchor is not yet ready -- i.e.
 * the loading high-water point has not been passed. While it is set the
 * observer offers at most ONE record per second (a carve_census) plus the
 * three one-shot snapshot-point transitions: the eleven carve_tenant records
 * of each set are suppressed (counted, and reported on the census line as
 * loading_suppressed), per-point repeat sends are held, and retirement bursts
 * fold into one pending record. Nothing that carries information is
 * discarded: suppressed tenant values are re-derived from the next set, held
 * point records keep folding new observations in, and folded bursts keep
 * their summed totals. Readiness restores the full cadence.
 *
 * Why: three real-Wii split_anchor_6m runs (2026-09-07) lost the telemetry
 * stream between 53 and 68 heartbeats, always inside the two-anchor reload,
 * where the observer's ~12 records/s meet a sender whose net_send shares the
 * IOS IPC path with the server thread's libfat fsync storm. The reload is
 * also the phase in which per-tenant samples say least. Default (never
 * called) is false, so tests and the offline gate tooling keep the full
 * cadence unless they ask for the loading phase explicitly. */
void carve_observer_loading(bool loading);
/* PER-FRAME RECONCILIATION THROTTLE (2026-09-08, the far-extract crawl fix).
 * carve_observer_frame used to run the observer's WHOLE reconciliation --
 * carve_observer_world() + carve_observer_snapshot(), the identical pair the
 * once-per-second census (stage_set) already runs -- on EVERY main-loop frame:
 * two O(ledger) mark sweeps, a hash probe per plane of every resident chunk, a
 * 4096-cell all-air scan of every far-LOD chunk, the whole-carve boundary-tag
 * walk and a third O(ledger) attribution walk, the last two while holding
 * mem2_lock. On the single-core Wii that is the main thread doing an O(resident
 * population) sweep sixty times a second and taking the allocator lock for it,
 * and the server thread -- which needs mem2_lock for every mem2_alloc/mem2_free
 * its far-skin extraction makes -- is starved behind it. Real-Wii build
 * 3cdbc5a8 (split_anchor_6m, seed 42, sep 256, far 6, voxel, --cw-carve-census)
 * measured srv_far_us_max climbing 52 ms -> 1.15 s -> 6.07 s -> 15.0 s for ONE
 * far column while wd_srv sat in WDS_FAR_EXTRACT on 708 of the last 863
 * heartbeats and srv_loop_it fell to 1-2 per second; the same scene without the
 * observer peaked at 578 ms per column and reached anchor_ready in 48 s. The
 * climb is the population growing, not any per-allocation bookkeeping: one far
 * column extraction produces only ten carve events (five plane allocations and
 * five frees -- tests/test_carve_frame_walk_cost.c counts them).
 *
 * So the walk now runs at the CENSUS cadence, once per CARVE_FRAME_WALK_FRAMES
 * frames, instead of once per frame: one reconciliation per second in total
 * rather than sixty-one. A record's CONTENT is unchanged -- a walk that does
 * happen is the same walk, and produces the same bytes -- what changes is how
 * often the two frame-observed extrema (loading high-water, session low-water)
 * are SAMPLED. That resolution loss is bounded and deliberate: during loading
 * carve usage climbs close to monotonically, so the high-water is essentially
 * the last sample before readiness, and readiness itself is never throttled --
 * point 1's one-shot transition always forces a walk on the frame it happens,
 * as does the observer's very first frame. Emission was already capped at one
 * record per point per second (see the cadence contract above), so nothing that
 * reached the wire is lost.
 *
 * carve_observer_walk_interval overrides the interval; 1 restores the exact
 * pre-2026-09-08 per-frame cadence and is what the byte-identity gate in
 * tests/test_carve_frame_walk_cost.c runs the observer at.
 * carve_observer_walk_count reports how many reconciliations have run since
 * carve_observer_start, which is what the cost gate asserts on. */
enum { CARVE_FRAME_WALK_FRAMES = 60 };
void carve_observer_walk_interval(uint32_t frames);
unsigned carve_observer_walk_count(void);
bool carve_observer_attrib(unsigned point, struct carve_attrib* out);
/* Considers one record per call; failed publication retries the same
 * record; the emission-cadence throttle above governs whether it sends at
 * all this call. */
/* Ceiling of the staged-census retry back-off, in main-loop frames: one
 * offer per second at the Wii's 60 Hz loop. See obs.staged_retry_frame in
 * carve_observer.c for the measured drain rate this is derived from. */
enum { CARVE_STAGED_BACKOFF_MAX_FRAMES = 60 };
bool carve_observer_emit_next(bool (*emit)(const char*));
extern void (*world_carve_depart)(const struct chunk*);
struct carve_totals {
	uint64_t bytes[CARVE_TENANTS], carve[CARVE_TENANTS], count[CARVE_TENANTS];
	uint64_t allocs[CARVE_TENANTS], spills[CARVE_TENANTS], spill_bytes[CARVE_TENANTS];
	uint64_t dl_live, dl_retired, departed, departed_peak, departed_gx, departed_gx_peak;
	uint64_t grows, copy_bytes, moved, old_peak;
	uint64_t dl_used, dl_slack, gx_used, gx_capacity, gx_slack;
	uint64_t scaled[CARVE_TENANTS], scaled_slack, scaled_lists, full5, far6_slices;
	uint64_t guards, overhead, used, free, largest, capacity, errors, observer_bytes;
	struct carve_attrib attribution;
};
/* Records in the ledger's hash table, reserved ONCE in carve_observer_start and
 * never reallocated (carve_observer.c, LEDGER TABLE). Power of two: the probe
 * index is masked, not divided. Half of it is the live-record limit, which the
 * measured real-Wii peak (4,888 on 703aef00 split_anchor_6m) fits 3.35x inside.
 * 32,768 * 44 B = 1,441,792 B on the Wii, from the general malloc heap (arena2
 * above the carve under mem2.c's MALLOC_MEM2 pin), claimed at boot. */
enum { CARVE_LEDGER_SLOTS = 32768 };
bool carve_observer_start(void);
bool carve_observer_stop(void);
bool carve_observer_active(void);
/* Ledger state, for the census line and the PC pressure test: `enabled` is
 * false only when the one arm-time reservation failed, `capacity` is fixed for
 * the life of the run, `live` is tracked blocks right now, `peak` its
 * high-water, and `full` counts inserts refused because the fixed table was
 * full (which must stay 0 for every number in the census to mean anything). */
struct carve_ledger_stat {
	bool enabled;
	uint64_t capacity, live, peak, full;
};
void carve_observer_ledger(struct carve_ledger_stat* out);
/* Owner-thread fixture: make the NEXT carve_observer_start fail its one ledger
 * reservation, so the disabled-ledger reporting path can be exercised without
 * exhausting the test host's heap. Cleared by the start it affects. */
void carve_observer_test_ledger_arm_fail(void);
void carve_observer_snapshot(struct carve_totals* out);
/* Owner-thread fixture: exercise real reconciliation with a synthetic heap. */
void carve_observer_test_snapshot(struct carve_totals* out, size_t capacity,
	size_t available, size_t largest, size_t blocks);
/* One paired packet group per heartbeat, emitted SYNCHRONOUSLY (whole set in
 * one call). Kept for tests and the offline gate-attrib tooling. */
bool carve_observer_emit(struct world* w, uint32_t tick, bool measuring,
	bool (*emit)(const char*));
/* Production path: snapshot the per-heartbeat census set (CARVE_TENANTS
 * carve_tenant records + one carve_census) without touching the transport.
 * carve_observer_emit_next then drains it ONE record per frame, so a
 * twelve-record burst never meets the four-slot telemetry FIFO at once. A set
 * that is still draining is kept; the new snapshot is skipped. */
bool carve_observer_stage(struct world* w, uint32_t tick, bool measuring);
/* Main-thread-owned world labels. Retained/GPU totals are overlapping subsets. */
void carve_observer_world(struct world* w);
void carve_observer_anchor(double x, double z, int separation);
#endif
