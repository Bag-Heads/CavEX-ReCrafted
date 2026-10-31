/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef SETTLE_OBSERVER_H
#define SETTLE_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct chunk;
struct world;

/* Wire IDs: append only. Bit 6 of event.stage is far; bit 7 is gameplay. */
enum settle_stage {
	SET_DIRTY_INSTALL,
	SET_DIRTY_NEIGHBOUR,
	SET_DIRTY_LIGHT,
	SET_DIRTY_LOD,
	SET_DIRTY_RETRY,
	SET_DIRTY_EDIT,
	SET_VISIT,
	SET_GATE_NEIGHBOUR,
	SET_GATE_INFLIGHT,
	SET_GATE_RETRY,
	SET_GATE_FAR,
	SET_GATE_TOKEN,
	SET_GATE_NOTVISITED,
	SET_GATHER,
	SET_SUBMIT,
	SET_WORKER,
	SET_VL_BEGIN,
	SET_EMIT_BEGIN,
	SET_FIN_BEGIN,
	SET_BFS_BEGIN,
	SET_COMPLETE,
	SET_CONSUME,
	SET_ACCEPT,
	SET_STALE,
	SET_RETRY,
	SET_CANCEL,
	SET_DRAW,
	SET_CULLED,
	SET_EVICT,
	SET_READY,
	SET_SLOT_BEGIN,
	SET_SLOT_END,
	SET_SEND_BEGIN,
	SET_ENQUEUE,
	SET_SEND_END,
	SET_DEQUEUE,
	SET_INSTALL_BEGIN,
	SET_INSTALL_OK,
	SET_INSTALL_FAIL,
	SET_STOP_UNLOAD,
	SET_STOP_BUDGET,
	SET_STOP_POOL,
	SET_STOP_NOTREADY,
	SET_STOP_BACKOFF,
	SET_STRIPE_BEGIN,
	SET_STRIPE_END,
	SET_SKIN_BEGIN,
	SET_SKIN_END,
	SET_SLEEP_BEGIN,
	SET_SLEEP_END,
	SET_ALLOC_BEGIN,
	SET_ALLOC_END,
	SET_LIGHT_BEGIN,
	SET_LIGHT_END,
	SET_PHASES,
	SET_RESET,
	SET_EP_BEGIN,
	SET_EP_END,
	SET_EP_CANCEL,
	SET_BLOCKER,
	SET_DIRTY_WAIT,
	SET_RETIRE,
	SET_JOB_DEQUEUE,
	SET_STAGE_COUNT
};

/* Full identity in each event; no engine pointers on the wire. 16-bit epoch
 * overflow and 32-bit ID/counter overflow invalidate the entire capture. */
struct settle_event {
	uint64_t us;
	uint32_t frame, job_id;
	int32_t cx, cz;
	uint32_t dirty_generation;
	uint16_t world_epoch;
	uint8_t cy, stage;
};
typedef char
	settle_event_must_be_32_bytes[sizeof(struct settle_event) == 32 ? 1 : -1];
#define SETTLE_RING_CAP 1024u
#define SETTLE_CHUNK_CAP 1024u
/* BOUNDED PROBE WINDOW for the chunk sidecar -- the same discipline column()
 * has always used (PROBES=16 over COLS=32), which the chunk table was missing.
 *
 * WHY IT IS LOAD-BEARING, MEASURED not assumed (PC split_anchor_6m, seed 42,
 * separation 256, far view 6, --cw-settle, 147 s): the observer took
 * 105,973,129 callbacks (~720k/s) and LOST 81,392,199 of them -- 77% -- every
 * one of which was a chunk() call that walked all SETTLE_CHUNK_CAP slots and
 * found neither the pointer nor a free slot. The table holds 1,024 sidecars;
 * the engine's working set at this profile is the near square plus the far
 * ring (Wii 11x11x8 = 968 near + ~384 far; PC 21x21x8 = 3,528 near), so it
 * runs at load factor >= 1 and EVERY miss was a full-table scan -- on main,
 * under the observer mutex, inside per-block engine callbacks. That is
 * 8.3e10 probe iterations across the run, ~5.7e8/s.
 *
 * BEFORE/AFTER on that same PC profile, same build otherwise, observer armed
 * both times -- this is the whole case for the change:
 *     window 1024 (pre-fix):  133 frames/heartbeat, 21,537 frames,
 *                             worst anchor_frame_us 69,567 (69.6 ms),
 *                             106.0M observer callbacks (726k/s)
 *     window   16 (this fix): 838 frames/heartbeat, 108,873 frames,
 *                             worst anchor_frame_us 3,547 (3.5 ms),
 *                             708.3M observer callbacks (5.58M/s)
 * i.e. the full-table scan was eating ~6.3x the entire main loop and 20x the
 * worst frame. A PC absorbs 70 ms frames; Broadway, ~20-40x slower per core,
 * does not -- which is the shape of the hardware stall (main alive but
 * effectively not advancing, ~1 s after a world reset re-dirties every chunk).
 * The stall itself has NOT been reproduced on PC; what is reproduced and
 * measured here is the cost that produces it. Hardware-unconfirmed.
 *
 * The window makes the observer's per-callback cost O(1) again. It costs no
 * lookup fidelity: inserts also probe only this window, so a live entry is
 * never more than SETTLE_CHUNK_PROBES-1 from its home slot and is always
 * found. What it does change is that an insert into a saturated window fails
 * sooner -- and a failed insert is counted through lost(), which already
 * invalidates the capture, so nothing becomes silently wrong. The 77% loss
 * above is a pre-existing CAPACITY mismatch (1,024 slots vs a ~1.4-3.5k
 * working set), not something this bound introduces; see the settle notes in
 * PROJECT_MEMORY.md.
 *
 * Overridable at compile time ONLY so a measurement build can restore the
 * pre-fix full-table scan (-DSETTLE_CHUNK_PROBES=1024u) and reproduce the
 * before/after probe counts. Production always takes the default. */
#ifndef SETTLE_CHUNK_PROBES
#define SETTLE_CHUNK_PROBES 16u
#endif
#define SETTLE_PULL_CAP 512u

struct settle_input {
	enum settle_stage stage;
	uint32_t observer_generation;
	const struct chunk*
		chunk; /* read on main only; worker uses token sidecar */
	const void* token;
	const void* detail;
	uint32_t value;
	int32_t cx, cz;
	unsigned far_band;
};
typedef void (*settle_callback)(const struct settle_input*);
extern settle_callback chunk_settle_observe, world_settle_observe;
extern settle_callback mesher_settle_observe, client_settle_observe;
extern settle_callback server_settle_observe, mem2_settle_observe;
/* Bit zero is the pending-publish flag; the remaining bits are the lifecycle
 * generation. Loading it before the hook makes a pre-stop callback stale. */
extern uint32_t settle_observer_control;
/* In-flight callers. A teardown drains this to zero instead of blocking on the
 * observer mutex; see the IN-FLIGHT GATE note in settle_observer.c. */
extern uint32_t settle_observer_inflight;

/* Enter the gate BEFORE re-validating the hook, so no caller can be invisible
 * to a concurrent drain: settle_observer_stop() nulls the hooks first and only
 * then drains, so a reload that still sees a hook happened-before that null,
 * which happened-before the drain -- the drain therefore observes this
 * increment. A reload that sees NULL drops the call without touching any
 * observer state. Everything here sits inside the `if(scb_)` arm, so a dormant
 * site still pays exactly one NULL test and no atomic RMW.
 *
 * MEASURED, not assumed (test_settle_observer --census, ucrt64, thread cycles;
 * baseline = the same two RMWs inside observe() instead of here):
 *   dormant  census 81 -> 121 cpu/job, scan 2,990 -> 2,520 cpu/frame -- the two
 *            move in OPPOSITE directions, i.e. code layout, not a real cost;
 *            the dormant instruction path is unchanged by construction.
 *   armed    10,016 -> 10,234 cpu/job (+2.2%, ~14 cycles per armed site) for
 *            the extra hook reload. That is the price of an EXACT drain instead
 *            of a best-effort one, and the observer is armed only under
 *            --cw-tps-cpu. */
#define SETTLE_ENTER(cb, scb_)                                                 \
	(__atomic_fetch_add(&settle_observer_inflight, 1u, __ATOMIC_ACQ_REL),      \
	 ((scb_) = __atomic_load_n(&(cb), __ATOMIC_ACQUIRE)) != NULL)
#define SETTLE_LEAVE()                                                         \
	__atomic_fetch_sub(&settle_observer_inflight, 1u, __ATOMIC_RELEASE)

/* Arm before threads, disarm after joins. Arguments (including chunk reads)
 * are not evaluated on the dormant path: one NULL test, no stamps. */
#define SETTLE(cb, st, c, tok, val, x, z, band)                                \
	do {                                                                       \
		uint32_t sgen_                                                          \
			= __atomic_load_n(&settle_observer_control, __ATOMIC_ACQUIRE) & ~1u; \
		settle_callback scb_ = __atomic_load_n(&(cb), __ATOMIC_ACQUIRE);       \
		if(scb_) {                                                             \
			if(SETTLE_ENTER(cb, scb_)) {                                       \
				const struct settle_input si_ = {.stage = (st),                \
											 .observer_generation = sgen_,     \
												 .chunk = (c),                 \
												 .token = (tok),               \
												 .value = (val),               \
												 .cx = (x),                    \
												 .cz = (z),                    \
												 .far_band = (band)};          \
				scb_(&si_);                                                    \
			}                                                                  \
			SETTLE_LEAVE();                                                    \
		}                                                                      \
	} while(0)
#define SETTLE_DETAIL(cb, st, tok, data)                                       \
	do {                                                                       \
		uint32_t sgen_                                                          \
			= __atomic_load_n(&settle_observer_control, __ATOMIC_ACQUIRE) & ~1u; \
		settle_callback scb_ = __atomic_load_n(&(cb), __ATOMIC_ACQUIRE);       \
		if(scb_) {                                                             \
			if(SETTLE_ENTER(cb, scb_)) {                                       \
				const struct settle_input si_                                  \
					= {.stage = (st), .observer_generation = sgen_,            \
					   .token = (tok), .detail = (data)};                      \
				scb_(&si_);                                                    \
			}                                                                  \
			SETTLE_LEAVE();                                                    \
		}                                                                      \
	} while(0)

struct settle_phases {
	float total, vl, emit, fin, bfs;
};
struct settle_light {
	uint32_t pops, stale, sources, frontier, budget_hit;
};
struct settle_counts {
	uint64_t enqueue, dequeue, rpc_outstanding;
	uint64_t submitted, completed, cancelled, outstanding;
	uint64_t worker_dequeued, queue_cancelled, queue_outstanding;
	uint64_t accepted, stale, retry, result_outstanding, result_cancelled;
	uint64_t calls, clock_reads, overflow, lost;
	/* Chunk-sidecar probe iterations (in-process diagnostic ONLY -- it has no
	 * wire byte; drain_now's counter list is an explicit 19 and is unchanged).
	 * The sidecar is an open-addressed table and its scan is the observer's
	 * only per-callback cost that is not O(1); this counter is what makes the
	 * bound provable instead of assumed (test_chunk_probe_bounded). */
	uint64_t probes;
	uint32_t ring_used;
	bool valid, finished;
};
bool settle_observer_start(void);
void settle_observer_finish(
	void); /* freeze at profile boundary, before quiesce */
void settle_observer_stop(bool (*emit)(const char*));
bool settle_observer_active(void);
void settle_observer_frame(struct world*, bool gameplay, int32_t cx, int32_t cz,
						   int radius, bool allow_far, size_t tokens);
void settle_observer_publish(bool (*emit)(const char*));
/* Wii telemetry sender only: drains a main-thread 1 Hz request directly through
 * the existing transport, avoiding its four-record producer mailbox. */
bool settle_observer_pump(bool (*emit)(const char*));
void settle_observer_counts(struct settle_counts*);
size_t settle_observer_static_bytes(void);
/* Non-zero only if a stop()/start() had to carry its buffer because a callback
 * was still inside the observer when the bounded drain expired. Diagnostic.
 * SHUTDOWN/ARM PATHS ONLY -- a world reset retires in place and never drains,
 * so this reads 0 for the whole of a normal session (see below). */
uint32_t settle_observer_deferred_retirements(void);
/* Wall-clock microseconds the last stop()/start() drain actually took. Reported
 * in the 1 Hz record so a hardware rerun can tell a slow drain (starvation)
 * from a stall that never reached the drain at all. Same scope caveat: in
 * production start()/stop() run only at the --cw-tps-cpu arm and at shutdown,
 * so an in-session record reads 0 here by construction. */
uint32_t settle_observer_last_drain_us(void);
/* RESET path, which the two above cannot see. A world reset recycles the armed
 * buffer in place: one memset, no free, no allocation, no drain and no I/O.
 * These are its count and its last measured cost. In-process only -- the wire
 * record sits exactly on its 1,023 B envelope ceiling and has no spare byte --
 * so client_interface.c reads them and writes one deferred trace line AFTER
 * the reset completes rather than on the path. */
uint32_t settle_observer_reset_retirements(void);
uint32_t settle_observer_last_reset_us(void);
size_t settle_observer_armed_bytes(void);
/* Explicit caller pull only. Returns false if worst episode exceeded retained
 * history or capacity is insufficient; never returns a partial episode. <=16 KiB. */
bool settle_observer_pull(struct settle_event* out, size_t cap, size_t* used);
/* 46 B JSON envelope + base64(438 B payload) = 46 + 584 + 2 = 632 B. The two
 * teardown-diagnostic bytes fit inside base64's 3-byte rounding, so the record
 * is byte-for-byte the same size it was before them. */
#define SETTLE_RECORD_BODY_MAX 632u

/* Stopped-only deterministic clock seam; tests drive real production hooks. */
bool settle_observer_test_clock(uint64_t (*clock_us)(void));
/* Test-only seam: restores the PRE-FIX teardown (stop() waits on the observer
 * mutex instead of draining the in-flight gate), so one run can demonstrate the
 * old behaviour against the new one. Compiled ONLY under CAVEWII_TESTING -- a
 * release binary carries neither the entry point nor the static it flips, so
 * "never set in production" is enforced by the build, not by convention. */
#ifdef CAVEWII_TESTING
bool settle_observer_test_blocking_stop(bool blocking);
#endif

#endif
