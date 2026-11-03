#include "../settle_observer.h"
/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#include "cw_canary.h" // ROUND 10 cycle 2: CW_SD_TRACE_OFF() kill switch for
						// cw_tel_trace/cw_tel_trace_hb (PLATFORM_WII-gated
						// no-op include on PC).
#include "hw_telemetry.h"
#include "thread.h"
#include "g9_probe.h"
#include "g9_capture.h"
#include "service_observer.h"
#include "time.h"
#include "hw_tel_trace.h"
#include "hw_tel_ring.h"
#include "hw_tel_stop.h"
#include "hw_tel_wake.h" // signalled idle park for the sender LWP
#include "time.h" // same device clock used by hw_run completion in main.c
#include "watchdog.h" // wd_report_path_for_seq + WD_REPORT_SEQ_MAX: the seq-suffixed
					  // hang reports the mailbox has to replay, not just report 1
// Narrow declaration keeps this standalone telemetry test free of the server
// interface's world/registry headers. The signature is in server_interface.h.
extern uint32_t svin_dropped_count(void);

#include <errno.h> // EAGAIN/EWOULDBLOCK for hw_telemetry_send_fail_classify -- portable,
				   // used unconditionally so the classifier compiles (and is testable)
				   // on PC too, even though the real failing net_send only exists on Wii.
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h> // getenv: the PC-only CAVEWII_HB_STDOUT switch
#include <string.h>

/* The async Wii sender uses this exact slot type after lane integration.
 * Observer formatting must fit before it can truthfully report success. */
_Static_assert(CW_TEL_LINE > 3072u,
	"telemetry sender slot must hold the largest observer packet plus NUL");
/* colcap-1006 (R4 point 9; Sol review R3 Major 1): the async sender's slot,
 * the heartbeat formatter's buffer and the collector's recvfrom cap are ONE
 * number. A formatter that accepts a line the ring then refuses loses the
 * heartbeat on the console only. */
_Static_assert(CW_TEL_LINE == HW_TELEMETRY_HEARTBEAT_LINE_SIZE,
	"ring slot must hold the widest heartbeat the formatter accepts");

#ifdef PLATFORM_WII
#include <arpa/inet.h>
#include <fcntl.h>
#include <network.h>
#include <ogc/if_config.h>
#include <ogc/mutex.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <poll.h> // POLLOUT for the bounded-send net_poll gate (libogc's own
				  // header, the same bits net_select passes to IOS).
#include <sys/socket.h>
#include <unistd.h>
// newlib hides this obsolete POSIX declaration in strict C11 mode.
int usleep(useconds_t useconds);
#endif

#define TEL_LOAD(v) __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define TEL_STORE(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELEASE)
static bool s_enabled = false;
/* The offered-piston fixture generates region-stat bursts faster than IOS
 * drains the three observer slots (98 offers before HB1 on f939bd2_10.11).
 * Keep exact cumulative count/sum/max per marker instead of per-call rows.
 * All publication words are PPC32 atomics; the main consumer never waits. */
static const char* const s_path_markers[] = {"load_begin", "level_load_end",
	"level_save_end", "level_save_fail", "save_region", "load_region", "region_stat"};
#define PATH_MARKERS (sizeof(s_path_markers) / sizeof(s_path_markers[0]))
#define PATH_SUMMARY_INTERVAL_MS 250u
static struct {
	uint32_t writer, version, count, sum, max, last, stats[3], cache[3];
} s_path_samples[PATH_MARKERS];
static struct {
	uint32_t sent[PATH_MARKERS], version, mark;
	unsigned cursor, slot;
	bool pending, paced;
	char line[512];
} s_path_send;
static uint32_t s_path_closed;
/* Main-thread sample for split_play_8m; kept beside the formatter so the
 * protocol can add eye evidence without growing the shared heartbeat ABI. */
static float s_split_guest_y;
static int s_split_guest_head_block, s_split_guest_air;
void hw_telemetry_split_guest_eye(float y, int head_block, int air) {
	s_split_guest_y = y;
	s_split_guest_head_block = head_block;
	s_split_guest_air = air;
}
static bool s_suppress_sd_probes = false;

bool hw_telemetry_sd_probes_enabled(void) {
	return TEL_LOAD(s_enabled) && !s_suppress_sd_probes;
}
// Sender-owned transport quiescence. Main separately closes admission and
// requests drain; profile_done must reach the wire before orderly quiescence.
static bool s_quiesced = false;
static char s_run_id[64] = "";
static char s_profile[32] = "";
static ptime_t s_portal_quit_started;
static uint32_t s_portal_quit_server_seq;
static uint32_t s_portal_quit_switch_begin_ms;
static char s_local_ip[16] = "";
static bool s_mailbox_sent = false;
static bool s_mailbox_sending = false;
// Set when the client died mid-mailbox (the already-sent HEAD chunks -- the
// cause/phase lines -- are lost); leaves the mailbox eligible for a whole
// resend on the next accept instead of retiring a truncated copy.
static bool s_mailbox_interrupted = false;

// Bound each SDK allocation; blocking IOS sends supply backpressure on the LWP.
#define HW_TELEMETRY_SEND_CHUNK 1024u
#define HW_TELEMETRY_GUARD_LINE_SIZE 1152u

// The portable classifier remains available for legacy tests/callers.
// Pull transport is stricter: any negative SDK result may hide a prefix,
// so record evidence and quiesce without retry. Keep the descriptor for
// diagnostic alive probes; the stop worker cancels, then the sender closes.
//
// PACING BOUND (2026-09-07 fix round): a fixed 2ms post-failure pause with a
// 3-strike quiesce tolerated only ~4ms of sustained back-pressure (2 sleeps
// of 2ms before the 3rd consecutive -ENOBUFS fired quiesce) -- shorter than
// a real lwIP send-buffer drain under a per-frame carve+heartbeat+settle
// burst. The strike count now advances only AFTER the escalating 2/8/32ms
// ladder below has been walked once per consecutive failure (see
// transient_pace_us and its use in sender_entry): strike 1 -> sleep 2ms,
// strike 2 -> sleep 8ms, strike 3 -> sleep 32ms, strike 4 -> quiesce. That is
// a documented BOUND of 2+8+32 = 42ms of continuous transient back-pressure
// tolerated before the stream gives up -- roughly 10x the old ~4ms, while a
// hard (non-transient) errno still quiesces on the very first occurrence and
// the collector's own stale-heartbeat rule is untouched (see tick() in
// wii_telemetry_server.py).
#define HW_TELEMETRY_SEND_FAIL_QUIESCE_STRIKES 4u

// SENDER LWP PRIORITY (2026-09-07 starvation fix). libogc: higher number =
// higher priority, and KThreadReschedule only preempts a STRICTLY
// lower-priority current thread; equal-priority threads are cooperative (no
// timeslicing), so a thread at main's own priority never interrupts main --
// it only runs while main is blocked.
//
// At the old 40 the sender ran ONLY when both main (64) and the server
// (SERVER_THREAD_PRIORITY 63) were blocked. During the two-anchor reload main
// blocks on the frame wait ~60x/s but the server consumes every one of those
// gaps generating and lighting columns, so the sender was scheduled a handful
// of times per second: measured on build 0077cd09 (split_anchor_6m, seed 42,
// sep 256, far 6, voxel) the ring refused 571 of ~583 observer offers over the
// 53 s the stream lived -- an admitted-record rate of ~2/s -- with tel_fail 0
// (no socket ever failed) and the game itself at 61 fps.
//
// 64 == main is the smallest priority that fixes that: the sender now
// preempts the SERVER when a record is pending, and can still never preempt
// MAIN (equal priority, cooperative). The main loop's exposure is therefore
// bounded by whatever the sender holds the core for between yields, and every
// path in sender_entry ends in a blocking net_send (which leaves the ready
// queue) or a usleep -- it never spins.
//
// 2026-09-08 (real Wii, split scene, stage fields on d6cc037c): after the SD
// flush storm the sender sat in its IDLE usleep for 1-2 s per iteration
// (tel_stg=0, tel_stg_us 1.0-2.0 s, tel_it 0) while main ran 60 fps and the
// server tick was 100-150 ms. At priority 64 the sender can NEVER preempt
// main or the server (libogc only preempts a strictly lower priority), so a
// timer-woken sender waits until both of them block. 65 lets it run the
// moment its sleep ends. The earlier "a stalled IOS send would delay the
// frame loop" objection was wrong: a thread blocked inside IPC holds no CPU;
// the sender's only CPU cost is formatting one line between blocks.
// Diagnostic value first: if the IDLE dwell stays in seconds at 65, the
// decrementer timer itself is being starved and the wait must become a
// frame-signalled condvar instead.
//
// 2026-09-08 FOLLOW-UP: the priority stays 65, but the 1 ms IDLE POLL is
// gone -- a thread one priority above main that wakes on a 1 ms timer
// preempts the render loop ~1,000 times a second to look at an empty ring.
// The idle wait is now a signalled condvar park with a 10 ms ceiling (see
// s_sender_wake / HW_TELEMETRY_SENDER_IDLE_WAIT_MS below): producers wake it
// the moment a record is admitted, so 65 now buys prompt service on a real
// event instead of 1,000 empty preemptions, and the idle wakeup rate drops
// 10x. The priority and the poll rate were always two separate knobs; only
// the second one was ever paying for the starvation fix.
#define HW_TELEMETRY_SENDER_PRIORITY 65

// Escalating transient back-pressure pacing, indexed by consec_fail-1 and
// clamped to the last entry beyond the ladder's length. Used only by the
// async Wii sender thread (sender_entry) between drain attempts; the
// synchronous classifier above never sleeps itself. Exposed to the test
// build via hw_telemetry_test_transient_pace_us so the documented bound is
// asserted, not just claimed in a comment. Neither caller exists on a plain
// PC (non-test) build, so keep this out of that TU entirely.
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
static const uint32_t s_transient_pace_us[] = {2000u, 8000u, 32000u};

static uint32_t transient_pace_us(unsigned consec_fail) {
	if(consec_fail == 0)
		return 0;
	unsigned idx = consec_fail - 1u;
	unsigned last =
		(unsigned)(sizeof(s_transient_pace_us) / sizeof(s_transient_pace_us[0])) - 1u;
	if(idx > last)
		idx = last;
	return s_transient_pace_us[idx];
}
#endif

#ifdef CAVEWII_HW_TELEMETRY_TEST
uint32_t hw_telemetry_test_transient_pace_us(unsigned consec_fail) {
	return transient_pace_us(consec_fail);
}
#endif

void hw_telemetry_send_fail_state_reset(struct hw_telemetry_send_fail_state* st) {
	if(!st)
		return;
	st->fail_count = 0;
	st->last_errno = 0;
	st->consec_fail = 0;
	st->quiesced = false;
	st->resync = false;
}

void hw_telemetry_send_fail_state_note_success(
	struct hw_telemetry_send_fail_state* st) {
	if(st)
		st->consec_fail = 0;
}

void hw_telemetry_send_fail_classify(struct hw_telemetry_send_fail_state* st,
									 int neg_errno, bool* out_close_socket) {
	bool close_socket = false;
	if(st) {
		st->fail_count++;
		st->last_errno = neg_errno;
		// -ENOBUFS is lwIP/IOS BACK-PRESSURE, not a dead peer: the send
		// buffer is momentarily full (2026-09-07 hardware finding -- with
		// the carve attribution observer armed, main published a
		// heartbeat + settle + 11 carve_tenant + carve_census burst inside
		// one frame and the sender's blocking net_send hits the IOS
		// sndbuf; classifying that as a hard error closed the socket and
		// quiesced the whole stream at ~60 s while the game ran on).
		// Treat it exactly like -EAGAIN: transient, three-strike.
		bool transient = neg_errno == -EAGAIN || neg_errno == -EWOULDBLOCK
			|| neg_errno == -ENOBUFS;
		if(transient) {
			st->consec_fail++;
			if(st->consec_fail >= HW_TELEMETRY_SEND_FAIL_QUIESCE_STRIKES)
				st->quiesced = true; // still never closes -- see the doc above
		} else {
			st->consec_fail = 0;
			st->quiesced = true;
			close_socket = true;
		}
	}
	if(out_close_socket)
		*out_close_socket = close_socket;
}

#ifdef PLATFORM_WII
// The module's own live send-fail state (the real Wii socket path below
// drives this); PC has no socket and therefore no failures, so the
// accessors just below return the fixed zero/none defaults for it.
static struct hw_telemetry_send_fail_state s_send_fail;
#endif

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
static struct cw_tel_trace s_failure_trace;
static int s_last_results[3];
static struct cw_tel_ring s_sender_ring;
/* Guard context follows the ownership of its heartbeat slot. Publish BOTH
 * copies before the slot's release store; replacement/fallback/pop then apply
 * to the pair. No extra queue, lock, allocation or producer IO. 5 x 1152 B
 * also holds 47 UINT32_MAX words plus a maximally escaped 63-byte run ID. */
static char s_heartbeat_guard[CW_TEL_SLOTS + 1u][HW_TELEMETRY_GUARD_LINE_SIZE];
_Static_assert(HW_TELEMETRY_GUARD_LINE_SIZE > 47u * 11u
	+ 6u * (sizeof(s_run_id) - 1u) + 160u, "guard context and escaped run ID fit");

static bool publish_heartbeat_pair(const char* line, const char* guard) {
	/* Keep the claim/replacement/fallback transitions identical to
	 * cw_tel_publish_hb; the OFF equivalence test pins every ring byte. */
	struct cw_tel_ring* q = &s_sender_ring;
	uint32_t expected = 0;
	if(!__atomic_compare_exchange_n(&q->producer, &expected, 1, false,
		__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		goto drop;
	size_t n = cw_tel_line_len(line);
	bool ok = line && n < CW_TEL_LINE
		&& !__atomic_load_n(&q->closed, __ATOMIC_ACQUIRE);
	bool replaced = false;
	if(ok) {
		uint32_t state = CW_TEL_HB_EMPTY;
		bool claimed = __atomic_compare_exchange_n(&q->hb_state, &state,
			CW_TEL_HB_WRITING, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
		if(!claimed) {
			state = CW_TEL_HB_FULL;
			claimed = replaced = __atomic_compare_exchange_n(&q->hb_state,
				&state, CW_TEL_HB_WRITING, false, __ATOMIC_ACQUIRE,
				__ATOMIC_RELAXED);
		}
		if(claimed) {
			snprintf(s_heartbeat_guard[CW_TEL_SLOTS], HW_TELEMETRY_GUARD_LINE_SIZE,
				"%s", guard ? guard : "");
			memcpy(q->hb_line, line, n + 1);
			__atomic_store_n(&q->hb_state, CW_TEL_HB_FULL, __ATOMIC_RELEASE);
			if(replaced)
				__atomic_fetch_add(&q->hb_replaced, 1, __ATOMIC_RELAXED);
		} else {
			uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
			uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
			ok = head - tail < CW_TEL_SLOTS;
			if(ok) {
				snprintf(s_heartbeat_guard[head % CW_TEL_SLOTS],
					HW_TELEMETRY_GUARD_LINE_SIZE, "%s", guard ? guard : "");
				ok = cw_tel_fifo_store(q, line, n, 0u);
			}
		}
	}
	if(ok)
		cw_tel_note_occupancy(q);
	__atomic_store_n(&q->producer, 0, __ATOMIC_RELEASE);
	if(ok)
		return true;
drop:
	__atomic_fetch_add(&q->drops, 1, __ATOMIC_RELAXED);
	return false;
}
/* IDLE PARK (2026-09-08). The sender used to poll its ring with usleep(1000):
 * ~1,000 timer expirations per second, each of which PREEMPTS main (the
 * sender sits at priority 65, one above main's 64 -- see
 * HW_TELEMETRY_SENDER_PRIORITY), to find an empty ring almost every time.
 * It now parks on this object and is woken by whoever admitted a record.
 *
 * The 10 ms ceiling is what makes the change safe rather than clever: every
 * periodic duty in the loop (the once-a-second alive probe, the quiesce
 * check, the stop check) is re-evaluated at least 100x/s whether or not a
 * signal ever arrives, so the heartbeat cadence, the collector's 5 s stale
 * rule and shutdown all behave exactly as before even if a signal were lost.
 * Worst-case idle wakeups: 1000/10 = 100 per second, a 10x cut. Worst-case
 * enqueue->wake latency on the signal path: one context switch, because the
 * signal makes a STRICTLY higher-priority thread runnable and libogc
 * reschedules on that immediately; only a lost signal (impossible, see
 * hw_tel_wake.h's CORRECTNESS note) would fall back to the 10 ms bound. */
#define HW_TELEMETRY_SENDER_IDLE_WAIT_MS 10u
static struct cw_tel_wake s_sender_wake;
/* Init-once, never destroyed: producers on several threads may signal at any
 * time, and a teardown racing them buys nothing (the object is .bss and lives
 * as long as the process). Re-init across a telemetry reset is likewise
 * skipped -- a mutex/condvar carries no per-session state. */
static bool s_sender_wake_ready;
static void sender_wake_init(void) {
	if(!s_sender_wake_ready)
		s_sender_wake_ready = cw_tel_wake_init(&s_sender_wake);
}
/* Every admission path calls this AFTER the ring publish returns, so the
 * lock-free publish contract in hw_tel_ring.h is untouched. */
static void sender_wake(void) {
	if(s_sender_wake_ready)
		cw_tel_wake_signal(&s_sender_wake);
}
// Sender self-telemetry (2026-09-07). The Wii cannot be profiled from here,
// so the sender reports its own service rate in the heartbeat: how many loop
// passes it got, how many records it actually handed to the transport, and
// the longest single transport call. Windowed counters are reset by
// hw_telemetry_sender_stats_take() once per heartbeat; the ring's own
// drops/hb_replaced/occupancy_max are monotonic. All RELAXED: they are
// diagnostics, never control flow.
static uint32_t s_sender_iters, s_sender_sends, s_sender_send_max_us;
/* Monotonic count of lines abandoned by the bounded-send deadline (see
 * struct send_gate below and the tel_wto heartbeat field). */
static uint32_t s_sender_write_timeouts;
/* STAGE INSTRUMENTATION (2026-09-08). The sender publishes WHICH call it is
 * in, so a heartbeat composed mid-wedge names the stuck call instead of only
 * proving that something is stuck (tel_it 0 with tel_wto climbing was the
 * 2026-09-07 signature, and it identified nothing).
 *
 * Stage and its entry timestamp live in ONE 32-bit word because they must be
 * read together: two separate atomics could hand the reader a stage from one
 * transition and a timestamp from the next, which would report a fresh POLL
 * as a multi-second one (or worse, hide a real multi-second SEND). 3 high
 * bits of stage (8 values fit, all now used -- HW_TEL_STAGE_LOCK added
 * 2026-09-08) + 29 low bits of a millisecond stamp, which
 * wraps every ~6.2 days -- far beyond any proof run, and the reader's
 * subtraction is masked so even a wrap across a heartbeat reads correctly.
 * A 32-bit atomic is lock-free on Broadway; a 64-bit one would be a libatomic
 * libcall on 32-bit PPC, i.e. a LOCK taken by the very thread whose stalls we
 * are trying to measure. Written only by the sender thread, read only by
 * main: RELAXED is sufficient and is never control flow. */
#define CW_TEL_STAGE_SHIFT 29u
#define CW_TEL_STAGE_MS_MASK 0x1FFFFFFFu
static uint32_t s_sender_stage; /* packed: stage<<29 | entry_ms */
/* Windowed per-syscall maxima (reset by the take, like s_sender_send_max_us).
 * These isolate the ONE net_poll and the ONE net_send from the record-level
 * number, which lumps poll + every chunk + framing together. */
static uint32_t s_sender_poll_max_us, s_sender_syscall_max_us;
/* Live gauges: the last raw transport results (see the header's field docs). */
static int32_t s_sender_last_poll_ret, s_sender_last_send_ret;
static uint32_t s_sender_last_poll_revents;
/* Monotonic count of lines abandoned by the non-blocking experiment, PLUS
 * (2026-09-08 fix round) a whole CONNECTION the experiment gave up on before
 * any line: cw_net_set_nonblock's F_GETFL read-back proved the IOS SETFL did
 * not actually latch, so the accept path fell back to a genuinely blocking
 * socket (with its poll gate kept, see s_client_is_nonblock) rather than
 * publish an unverified non-blocking one. Same family ("the experiment could
 * not do its job as configured"), folded here rather than adding a new wire
 * field -- the 9,216 B heartbeat slot has 22 B of headroom left (see
 * test_heartbeat_production_buffer_fits_full_payload's HEADROOM WARNING). */
static uint32_t s_sender_abandoned;
/* The experiment's arming flag (--cw-tel-nonblock). Set once before the
 * sender thread exists, read-only afterwards. */
static bool s_nonblock_mode;
#ifdef PLATFORM_WII
static int s_syscall_errno;
#endif
static uint32_t s_live_frame, s_live_tick;
#endif
#ifdef PLATFORM_WII
static lwp_t s_sender_thread = LWP_THREAD_NULL;
static unsigned char s_sender_stack[32768] __attribute__((aligned(32)));
static bool s_sender_started, s_quiesce_requested;
static struct cw_tel_stop s_stop;
static lwp_t s_stop_thread = LWP_THREAD_NULL;
static unsigned char s_stop_stack[8192] __attribute__((aligned(32)));
static uint32_t s_public_fail;
static int s_public_errno;
bool hw_telemetry_sender_context_snapshot(struct thread_saved_context* out) {
	/* Watchdog priority exceeds sender/stop worker; after this admission
	 * check neither can free the native thread before the bounded snapshot. */
	if(!__atomic_load_n(&s_sender_started, __ATOMIC_ACQUIRE)
	   || __atomic_load_n(&s_stop.requested, __ATOMIC_ACQUIRE)) {
		if(out) *out = (struct thread_saved_context) {0};
		return false;
	}
	return thread_saved_context_snapshot_raw(s_sender_thread, s_sender_stack,
		sizeof(s_sender_stack), out);
}
#endif
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
static void trace_stop(unsigned reason, const char* line, size_t queued,
	unsigned stalls, int error, int result) {
	if(s_failure_trace.reason) return;
	cw_tel_trace_capture(&s_failure_trace, reason, line, queued, stalls,
		error, result, s_last_results);
	s_failure_trace.frame = __atomic_load_n(&s_live_frame, __ATOMIC_RELAXED);
	s_failure_trace.tick = __atomic_load_n(&s_live_tick, __ATOMIC_RELAXED);
#ifdef PLATFORM_WII
	s_failure_trace.syscall_errno = s_syscall_errno;
	s_failure_trace.os_time = gettime(); /* libogc's OSGetTime/timebase API */
	s_failure_trace.mem2_free_kb = (int)(SYS_GetArena2Size()/1024);
	s_failure_trace.pending = __atomic_load_n(&s_sender_ring.head, __ATOMIC_ACQUIRE)
		- __atomic_load_n(&s_sender_ring.tail, __ATOMIC_RELAXED);
	/* Normal sender LWP (or init before it exists), never exception context.
  * libfat may sleep: no transport/producer lock is held by main. One attempt,
  * one bounded fwrite; retain the struct even when the card write fails. */
	char text[512];
	size_t n = cw_tel_trace_format(text, sizeof(text), &s_failure_trace, s_run_id);
	FILE* f = fopen("sd:/cw_tel_trace.txt", "wb");
	if(f) {
		(void)fwrite(text, 1, n, f);
		(void)fclose(f);
	}
#else
	s_failure_trace.mem2_free_kb = -1; /* PC has no Arena2/timebase. */
#endif
}
#endif

bool hw_telemetry_lease_priority(uint8_t priority) {
#ifdef PLATFORM_WII
	/* Called under IRQ exclusion by the drain owner. No socket/mutex work.
	 * An absent sender is harmless; a stopping sender rejects acquisition. */
	if(__atomic_load_n(&s_sender_started, __ATOMIC_ACQUIRE)) {
		if(__atomic_load_n(&s_stop.requested, __ATOMIC_ACQUIRE) && priority == 66)
			return false;
		LWP_SetThreadPriority(s_sender_thread, priority);
	}
#else
	(void)priority;
#endif
	return true;
}

void hw_telemetry_note_frame(uint32_t frame) {
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	__atomic_store_n(&s_live_frame, frame, __ATOMIC_RELAXED);
#else
	(void)frame;
#endif
}

// tel_fail counts TRANSPORT failures only. A full/contended admission FIFO
// drops an observer record without any socket being involved; folding those
// into tel_fail (and synthesizing -ENOBUFS for tel_last_errno whenever the
// drop counter was non-zero) made every hardware run with the carve observer
// armed look like a socket storm -- 2026-09-07 split_anchor_6m reported
// tel_fail 69 / errno -105 with, as far as this counter could ever prove,
// zero real send errors. Drops now have their own counter and tel_last_errno
// reports the real errno or nothing.
uint32_t hw_telemetry_tel_fail_count(void) {
#ifdef PLATFORM_WII
	return __atomic_load_n(&s_public_fail, __ATOMIC_RELAXED);
#else
	return 0;
#endif
}

uint32_t hw_telemetry_observer_drop_count(void) {
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	return __atomic_load_n(&s_sender_ring.drops, __ATOMIC_RELAXED);
#else
	return 0;
#endif
}

#ifdef CAVEWII_HW_TELEMETRY_TEST
/* Stage-clock seam (2026-09-08): lets a PC test model a 3 s park inside the
 * transport without a test that actually sleeps 3 s. NULL = default clock.
 * Declared HERE, ahead of hw_telemetry_sender_stats_take, because the stage
 * clock it feeds is read by that function. */
static uint64_t (*s_test_stage_now_us)(void*) = NULL;
static void* s_test_stage_user = NULL;
#endif

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
/* Microsecond clock for the per-stage durations. On Wii this is the timebase;
 * on a PC build with no test clock installed it is a constant 0, so every
 * derived duration honestly reads 0 rather than reporting noise from a clock
 * the platform does not have (the same convention tel_smax_us already uses). */
static uint64_t sender_now_us(void) {
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(s_test_stage_now_us)
		return s_test_stage_now_us(s_test_stage_user);
#endif
#ifdef PLATFORM_WII
	return (uint64_t)ticks_to_microsecs(gettime());
#else
	return 0;
#endif
}

static uint32_t sender_now_ms(void) {
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(s_test_stage_now_us)
		return (uint32_t)(s_test_stage_now_us(s_test_stage_user) / 1000u);
#endif
#ifdef PLATFORM_WII
	/* libogc's own timebase->ms macro, the same one tcp_send_now_ms uses. */
	return (uint32_t)ticks_to_millisecs(gettime());
#else
	return 0;
#endif
}

/* Publish `stage` with a fresh entry stamp and return the PREVIOUS packed
 * word, so the caller can restore it on the way out (a net_send inside a
 * record must fall back to OTHER, not stay in SEND). */
static uint32_t sender_stage_enter(unsigned stage) {
	uint32_t prev = __atomic_load_n(&s_sender_stage, __ATOMIC_RELAXED);
	__atomic_store_n(&s_sender_stage,
					 (stage << CW_TEL_STAGE_SHIFT)
						 | (sender_now_ms() & CW_TEL_STAGE_MS_MASK),
					 __ATOMIC_RELAXED);
	return prev;
}

/* Restore `prev`'s STAGE and, when `max_us` is non-NULL, fold the time just
 * spent in the stage into that windowed maximum. `entered_us` comes from the
 * caller's own sender_now_us() so the duration is exact microseconds,
 * independent of the millisecond stamp packed into the published word.
 *
 * 2026-09-08 fix round: this used to restore `prev` VERBATIM, timestamp
 * included. `prev` was captured by the matching sender_stage_enter() call,
 * so its timestamp is however long ago THAT was -- stale the instant this
 * function returns. A park that happens outside the five instrumented calls
 * (e.g. the blocking LWP_MutexLock in send_lock_enter, before any stage is
 * entered for this record) then reads the LAST restored stage with an
 * ever-growing dwell, since nothing re-stamps it until the next enter/leave
 * pair. Re-stamp with NOW instead: the restored stage's dwell then correctly
 * starts counting from the moment it actually became current again. */
static void sender_stage_leave(uint32_t prev, uint64_t entered_us,
							   uint32_t* max_us) {
	if(max_us) {
		uint64_t spent = sender_now_us() - entered_us;
		uint32_t us = spent > UINT32_MAX ? UINT32_MAX : (uint32_t)spent;
		/* NOTE (hostile review, item 3): this load-compare-store races main's
		 * __atomic_exchange_n(max_us, 0, ...) window reset in
		 * hw_telemetry_sender_stats_take -- if the exchange lands between
		 * this load and store, `us` gets re-stored into the just-zeroed
		 * counter and the max this window actually saw is reported in the
		 * NEXT window instead. Not fixed here (note only, no code change). */
		if(us > __atomic_load_n(max_us, __ATOMIC_RELAXED))
			__atomic_store_n(max_us, us, __ATOMIC_RELAXED);
	}
	uint32_t prev_stage = prev >> CW_TEL_STAGE_SHIFT;
	__atomic_store_n(&s_sender_stage,
					 (prev_stage << CW_TEL_STAGE_SHIFT)
						 | (sender_now_ms() & CW_TEL_STAGE_MS_MASK),
					 __ATOMIC_RELAXED);
}

#ifdef PLATFORM_WII
/* Publish a stage with no matching leave (the loop's own resting states).
 * Wii-only: its callers are sender_entry's own resting points, and that
 * thread exists only on hardware. */
static void sender_stage_set(unsigned stage) {
	(void)sender_stage_enter(stage);
}
#endif
#endif

void hw_telemetry_sender_stats_take(struct hw_telemetry_sender_stats* out) {
	if(!out)
		return;
	memset(out, 0, sizeof(*out));
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	out->iterations = __atomic_exchange_n(&s_sender_iters, 0, __ATOMIC_RELAXED);
	out->sends = __atomic_exchange_n(&s_sender_sends, 0, __ATOMIC_RELAXED);
	out->max_send_us =
		__atomic_exchange_n(&s_sender_send_max_us, 0, __ATOMIC_RELAXED);
	out->occupancy_max =
		__atomic_load_n(&s_sender_ring.occupancy_max, __ATOMIC_RELAXED);
	out->drops = __atomic_load_n(&s_sender_ring.drops, __ATOMIC_RELAXED);
	out->hb_replaced =
		__atomic_load_n(&s_sender_ring.hb_replaced, __ATOMIC_RELAXED);
	out->write_timeouts =
		__atomic_load_n(&s_sender_write_timeouts, __ATOMIC_RELAXED);
	/* Windowed like iterations/sends: exchanged to 0 so each heartbeat
	 * reports the worst syscall IN ITS OWN window, not since boot. */
	out->poll_max_us =
		__atomic_exchange_n(&s_sender_poll_max_us, 0, __ATOMIC_RELAXED);
	out->send_max_us =
		__atomic_exchange_n(&s_sender_syscall_max_us, 0, __ATOMIC_RELAXED);
	out->abandoned = __atomic_load_n(&s_sender_abandoned, __ATOMIC_RELAXED);
	out->last_poll_ret =
		__atomic_load_n(&s_sender_last_poll_ret, __ATOMIC_RELAXED);
	out->last_poll_revents =
		__atomic_load_n(&s_sender_last_poll_revents, __ATOMIC_RELAXED);
	out->last_send_ret =
		__atomic_load_n(&s_sender_last_send_ret, __ATOMIC_RELAXED);
	/* LIVE gauge, deliberately not reset: "where is the sender RIGHT NOW,
	 * and for how long". One packed load, so stage and its entry stamp can
	 * never come from two different transitions. The subtraction is masked
	 * to the stamp's 29 bits so a wrap between the sender's write and this
	 * read still yields the true elapsed time. */
	uint32_t packed = __atomic_load_n(&s_sender_stage, __ATOMIC_RELAXED);
	out->stage = packed >> CW_TEL_STAGE_SHIFT;
	uint32_t delta_ms = (sender_now_ms() - (packed & CW_TEL_STAGE_MS_MASK))
		& CW_TEL_STAGE_MS_MASK;
	/* us would overflow uint32 past ~4,294 s in a stage; saturate honestly. */
	out->stage_us = delta_ms > (UINT32_MAX / 1000u) ? UINT32_MAX
												   : delta_ms * 1000u;
#endif
}

void hw_telemetry_set_nonblock_experiment(bool on) {
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	/* Arming is a startup decision (main.c, from the run-arg) taken before
	 * the sender thread exists; nothing reads it concurrently at that point,
	 * so a plain store is correct and no barrier is owed. */
	s_nonblock_mode = on;
#else
	(void)on;
#endif
}

bool hw_telemetry_nonblock_experiment(void) {
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	return s_nonblock_mode;
#else
	return false;
#endif
}

int hw_telemetry_tel_last_errno(void) {
#ifdef PLATFORM_WII
	return __atomic_load_n(&s_public_errno, __ATOMIC_RELAXED);
#else
	return 0;
#endif
}

#ifdef CAVEWII_HW_TELEMETRY_TEST
static hw_telemetry_test_sink_fn s_test_sink = NULL;
static void* s_test_sink_user = NULL;
static bool s_test_send_lock_busy = false;
static unsigned s_test_send_lock_blocks = 0;
/* PC stand-in for the blocking LWP_MutexLock() park in send_lock_enter (see
 * that function and hw_telemetry_test_set_lock_wait). NULL disarms it. */
static void (*s_test_lock_wait)(void*) = NULL;
static void* s_test_lock_wait_user = NULL;
static bool s_test_async_sender = false;
/* Bounded-send gate seam (2026-09-08): NULL disarms, so every existing test
 * keeps the pre-deadline behaviour byte for byte. */
static int (*s_test_gate_ready)(void*, uint32_t) = NULL;
static uint64_t (*s_test_gate_now)(void*) = NULL;
static void* s_test_gate_user = NULL;
#endif

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
// IOS's non-blocking flag bit. libogc's net_fcntl passes flags RAW to the IOS
// SO_FCNTL ioctl (network_wii.c net_fcntl), and IOS's bit is IOS_O_NONBLOCK
// 0x04 (private to network_wii.c; libogc's own FIONBIO path sets it via
// F_GETFL | 0x04). Newlib's O_NONBLOCK is 0x4000 and even libogc network.h's
// fallback is 04000 octal -- both leave the socket BLOCKING on real hardware
// while Dolphin never exercises this path. Mirror libogc's own read-modify-
// write idiom. Defined outside the PLATFORM_WII block below (unlike the
// net_fcntl callers that use it) so a PC test can exercise the decision
// function right after it without a real IOS socket.
#define CW_IOS_O_NONBLOCK 0x04

// Pure decision, no I/O: given a F_SETFL attempt's return and the F_GETFL
// read-back taken right after it, was the non-blocking bit ACTUALLY
// confirmed latched? Split out (2026-09-08 fix round) so the exact defect
// being fixed -- cw_net_set_nonblock used to trust net_fcntl(F_SETFL)'s own
// success return alone, with no read-back, so an IOS SETFL that reports
// success without the bit actually taking would sail through unnoticed --
// is provable from a PC test with synthetic setfl/getfl results, independent
// of the real accept()-path integration below it, which stays Wii-only.
// int32_t, not s32 (libogc's gctypes.h typedef): this function compiles on
// PC too, where s32 is not declared.
static bool cw_net_flags_confirm_nonblock(int32_t setfl_ret, int32_t getfl_ret) {
	return setfl_ret >= 0 && getfl_ret >= 0
		&& (getfl_ret & CW_IOS_O_NONBLOCK) != 0;
}
#endif

#ifdef PLATFORM_WII
static s32 s_sock = -1;
static s32 s_listen_sock = -1;
static s32 s_client_sock = -1;
// 2026-09-08 fix round: whether s_client_sock actually ended up non-blocking,
// PER CONNECTION -- distinct from s_nonblock_mode, which is only what the
// experiment REQUESTED. A verify-mismatch fallback (see cw_net_set_nonblock)
// accepts the client as a genuinely blocking socket even with the experiment
// armed, so the per-line send gate must key off this, not s_nonblock_mode.
static bool s_client_is_nonblock = false;
static bool s_pull = false;
// Suppress ALL diagnostic-probe SD writes (cw_tel_trace + the S5.3 hunt
// probes below) for WATCHDOG-mode sends: those can run from the fault path
// (exception context), where a fopen would re-enter libfat/heap locks the
// interrupted thread may hold -- wedging the panic handler BEFORE the
// sd:/hang_report write (the historical "banner without hang_report.txt"
// symptom). Declared here (moved up from its previous spot just above
// cw_tel_trace) so tcp_send_bytes's probe below can honor the same gate.
static bool s_tel_trace_suppress = false;
// Deferred-accept guard, kept as belt-and-braces. The original theory ("the
// IOS socket layer isn't settled at early boot and net_accept hard-faults") is
// superseded: the cw_boot_trace pinpoint (stuck between send_before_accept and
// send_after_accept) is fully explained by the accept BLOCKING -- the old
// net_fcntl(F_SETFL, O_NONBLOCK) call passed newlib's O_NONBLOCK (0x4000) raw
// to IOS, whose non-blocking bit is 0x04 (see CW_IOS_O_NONBLOCK below), so the
// listen socket was silently blocking and a boot-time accept with no pending
// client parked the main thread forever -- pre-video, indistinguishable from a
// hard crash. (The stage server's "identical accept works fine" because a
// blocking accept is harmless in its dedicated wait loop.) With the flag fixed,
// accept returns -EAGAIN immediately; the arm still keeps boot free of
// accept syscalls, and the boot line has no client yet anyway.
static bool s_accept_armed = false;
static struct sockaddr_in s_dest;
static mutex_t s_send_mutex = LWP_MUTEX_NULL;
static bool s_send_mutex_ready = false;

// Returns false when the socket could NOT be VERIFIED non-blocking -- callers
// must then refuse the experiment for this socket: a silently-blocking
// socket recreates the exact main-thread-park class this flag fix exists to
// kill, only in reverse (an unverified NON-blocking flag would leave a caller
// that assumes sends cannot wait -- the abandon-on-EAGAIN path -- parked in a
// blocking net_send forever instead). 2026-09-08 fix round: this used to
// trust net_fcntl(F_SETFL)'s own return alone, with no F_GETFL read-back --
// see cw_net_flags_confirm_nonblock above, which owns the decision so it can
// be tested on PC. Read back exactly like cw_net_set_blocking below.
static bool cw_net_set_nonblock(s32 sock) {
	s32 flags = net_fcntl(sock, F_GETFL, 0);
	if(flags < 0)
		flags = 0;
	s32 setfl_ret = net_fcntl(sock, F_SETFL, (u32)flags | CW_IOS_O_NONBLOCK);
	if(setfl_ret < 0)
		return false;
	s32 getfl_ret = net_fcntl(sock, F_GETFL, 0);
	return cw_net_flags_confirm_nonblock(setfl_ret, getfl_ret);
}

// Accepted sockets may inherit listener flags: explicitly clear IOS's bit.
static bool cw_net_set_blocking(s32 sock) {
	s32 flags = net_fcntl(sock, F_GETFL, 0);
	if(flags < 0) return false;
	if(net_fcntl(sock, F_SETFL, (u32)flags & ~CW_IOS_O_NONBLOCK) < 0)
		return false;
	flags = net_fcntl(sock, F_GETFL, 0);
	return flags >= 0 && !(flags & CW_IOS_O_NONBLOCK);
}

struct tcp_send_ctx {
	s32 sock;
};

static int tcp_send_bytes(const char* data, size_t len, void* user) {
	struct tcp_send_ctx* ctx = user;
	if(!ctx || ctx->sock < 0) return -EBADF;
	// SC pairs with client publication and the stop worker's snapshot: either
	// cancellation sees this descriptor, or this check sees the stop request.
	if(__atomic_load_n(&s_stop.requested, __ATOMIC_SEQ_CST)) return -ECANCELED;
	// logc256kr/libogc/network_wii.c:858-892 loops after short writes and
	// :888 discards earlier progress on error. Never retry a negative result.
	errno = 0;
	s32 n = net_send(ctx->sock, data, (s32)len, 0);
	s_syscall_errno = errno;
	// PROBE (S5.3 hunt, temporary): the raw one-shot socket result, before
	// send_line_with classifies transient backpressure versus a hard error --
	// names whether a kill is -EAGAIN (transient backpressure, consistent
	// with the MP connect's own concurrent socket traffic colliding with
	// this send on IOS's network IPC) vs. a hard error (peer gone). Own
	// budget, gated the same as cw_tel_trace (see s_tel_trace_suppress).
	{
		static unsigned budget = 40;
		if(hw_telemetry_sd_probes_enabled() && !s_tel_trace_suppress
		   && budget > 0 && !CW_SD_TRACE_OFF()) {
			budget--;
			FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
			if(f) {
				fprintf(f, "tcp_send_ret n=%ld len=%lu sock=%ld\n", (long)n,
						(unsigned long)len, (long)ctx->sock);
				fflush(f);
				fclose(f);
			}
		}
	}
	return (int)n;
}

// The bounded-send writability gate (2026-09-08). libogc's net_poll is a
// synchronous IOCTL_SO_POLL against IOS with an explicit millisecond timeout
// (logc256kr/libogc/network_wii.c:1143), and net_select is implemented ON TOP
// of it (:991) -- so poll is the primitive, and using it directly avoids
// net_select's fd_set copying for a single descriptor. `struct pollsd` and the
// POLLOUT bit are libogc's own (network.h / poll.h), the same pair net_select
// passes straight through to IOS.
//
// Contract (see struct send_gate): >0 writable, 0 not writable within
// timeout_ms, <0 the gate could not answer -- the caller then just sends,
// which is exactly the pre-2026-09-08 behaviour, so a poll that fails (IOS
// error, exhausted net heap: net_poll net_mallocs its request buffer) can
// never silence a healthy stream.
static int tcp_send_ready(void* user, uint32_t timeout_ms) {
	struct tcp_send_ctx* ctx = user;
	if(!ctx || ctx->sock < 0) return -EBADF;
	if(__atomic_load_n(&s_stop.requested, __ATOMIC_SEQ_CST)) return -ECANCELED;
	struct pollsd sd = {.socket = ctx->sock, .events = POLLOUT, .revents = 0};
	s32 r = net_poll(&sd, 1, (s32)timeout_ms);
	/* Wii-only: `revents` is the one piece of transport state IOS actually
	 * hands back here (POLLOUT/POLLERR/POLLHUP/POLLNVAL, libogc poll.h). It
	 * is published even on a negative return, where it stays whatever IOS
	 * left it -- a reader must weigh it against tel_pret, not alone. */
	__atomic_store_n(&s_sender_last_poll_revents, (uint32_t)sd.revents,
					 __ATOMIC_RELAXED);
	if(r < 0) return (int)r;
	if(r == 0) return 0; /* the deadline, honoured by IOS itself */
	/* A hung-up/errored socket reports itself here; let the real net_send
	 * produce the real errno so the existing classifier still owns the
	 * live/dead decision -- this gate only ever answers "wait or go". */
	return 1;
}

static uint64_t tcp_send_now_ms(void* user) {
	(void)user;
	return (uint64_t)ticks_to_millisecs(gettime());
}
#endif

enum telemetry_send_mode {
	TELEMETRY_SEND_NORMAL,
	TELEMETRY_SEND_WATCHDOG,
};

static bool send_lock_enter(enum telemetry_send_mode mode) {
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(mode == TELEMETRY_SEND_WATCHDOG && s_test_send_lock_busy) {
		s_test_send_lock_blocks++;
		return false;
	}
#endif
#ifdef PLATFORM_WII
	if(!s_send_mutex_ready)
		return false;
	if(mode == TELEMETRY_SEND_WATCHDOG)
		return LWP_MutexTryLock(s_send_mutex) == 0;
	/* 2026-09-08 fix round: this blocking lock sits BEFORE send_line_transport
	 * enters any of its own five instrumented stages, so a contended mutex
	 * used to report whatever stage the PREVIOUS record left behind (see
	 * sender_stage_leave's fix above) -- an ever-growing dwell on a stage the
	 * sender was not actually in. Name it. */
	{
		uint64_t lock_at = sender_now_us();
		uint32_t lock_prev = sender_stage_enter(HW_TEL_STAGE_LOCK);
		bool ok = LWP_MutexLock(s_send_mutex) == 0;
		sender_stage_leave(lock_prev, lock_at, NULL);
		return ok;
	}
#else
	(void)mode;
#ifdef CAVEWII_HW_TELEMETRY_TEST
	/* PC stand-in for the real LWP_MutexLock() park above: off Wii there is
	 * no mutex to block on, so an armed test hook stands in for the wait,
	 * wrapped in the same LOCK stage enter/leave the real call gets (see
	 * hw_telemetry_test_set_lock_wait). Disarmed (the default), this is a
	 * no-op and the PC path behaves exactly as it did before this fix. */
	if(mode == TELEMETRY_SEND_NORMAL && s_test_lock_wait) {
		uint64_t lock_at = sender_now_us();
		uint32_t lock_prev = sender_stage_enter(HW_TEL_STAGE_LOCK);
		s_test_lock_wait(s_test_lock_wait_user);
		sender_stage_leave(lock_prev, lock_at, NULL);
	}
#endif
	return true;
#endif
}

static void send_lock_leave(void) {
#ifdef PLATFORM_WII
	if(s_send_mutex_ready)
		LWP_MutexUnlock(s_send_mutex);
#endif
}

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
typedef int (*send_bytes_fn)(const char* data, size_t len, void* user);

struct send_progress {
	size_t queued;
	unsigned stalls;
	int error;
	/* Set only by the non-blocking experiment (see
	 * hw_telemetry_set_nonblock_experiment): this line was given up on
	 * because the socket had no room RIGHT NOW, not because anything
	 * failed. Deliberately a separate flag rather than another magic errno,
	 * so it can never collide with a real socket error, and so tel_abd and
	 * tel_wto stay independently countable. */
	bool abandoned;
};

/* BOUNDED SENDS (2026-09-08). The 2026-09-06 root fix deliberately made the
 * telemetry socket BLOCKING on its own sender thread: the non-blocking paced
 * path hit libogc's partial-then-EAGAIN trap (network_wii.c queues the partial
 * AND reports the error, so the queued length is unknowable) and quiesced the
 * stream. That is still the right call and is NOT undone here. What a blocking
 * send cannot do is bound how long the sender waits: on the 2026-09-07
 * split_anchor_6m runs tel_smax_us climbed 17 -> 48 -> 166 ms and tel_it fell
 * 142 -> 11 -> 0 -- the sender sat inside one net_send for longer than a whole
 * heartbeat window -- exactly while srv_rt_us jumped 1 -> 100 ms (the server
 * thread in a libfat fsync). net_send and libfat both go through IOS IPC.
 *
 * So the wait, not the send, is what gets bounded:
 *   - `ready` is net_poll(POLLOUT) with a HW_TELEMETRY_SEND_DEADLINE_MS
 *     timeout, called ONCE per line before its first byte. IOS returns at the
 *     timeout whether or not the socket ever becomes writable, so the sender
 *     can no longer commit to a line the link has no room for.
 *   - `now_ms`/`deadline_ms` re-check the same per-line budget between 1 KiB
 *     chunks, costing no IPC at all: a line whose first chunks were cheap and
 *     whose next one would start past the deadline is abandoned instead.
 * A line abandoned either way is NEVER retried (the 4096 duplicate trap) and
 * the next line resyncs framing with a leading LF only when bytes actually
 * reached the wire. It is counted in tel_wto and deliberately does NOT feed
 * hw_telemetry_send_fail_classify: a slow link must not spend the four
 * transient strikes that exist to detect a DEAD one.
 *
 * WORST CASE, stated honestly. Per line: one poll of at most 250 ms, plus the
 * IOS service time of the single <=1 KiB net_send that may already be in
 * flight when the deadline passes -- that one call cannot be cancelled by
 * anything in this process, and pretending otherwise would be a lie. No
 * further chunk is ever STARTED past the deadline. Per second: at most four
 * fully-timed-out lines, and because the heartbeat has its own one-deep lane
 * that is drained first, a published heartbeat waits at most one such line
 * (<=250 ms + one chunk) before its own turn -- an order of magnitude inside
 * the collector's 5 s stale limit. Cost: exactly one extra IOS ioctl per line
 * (net_poll also net_mallocs 12 B from libogc's own net heap), i.e. ~+1 IPC
 * per record, not per KiB -- chosen over a per-chunk poll precisely because
 * the failure being fixed is IPC contention.
 *
 * EXCEPTION (2026-09-26): evidence lines (hw_telemetry_line_is_critical)
 * skip the between-chunk deadline once committed and retry a clean poll
 * timeout up to HW_TELEMETRY_CRITICAL_POLL_TRIES times -- abandoning a
 * heartbeat after its first KiB cost the collector the whole record for a
 * 255 ms IOS stall. See the header comment for the evidence. A heartbeat
 * that is itself slow therefore delays the lines behind it (observers,
 * which the ring drops and counts) instead of being lost. */
struct send_gate {
	int (*ready)(void* user, uint32_t timeout_ms);
	uint64_t (*now_ms)(void* user);
	void* user;
	uint32_t budget_ms;
	uint64_t deadline_ms; /* per line, set by send_line_with */
	bool armed;           /* deadline_ms is valid for the line in flight */
};

/* ESCAPE HATCH. The gate's whole value rests on IOS reporting POLLOUT the way
 * lwIP/BSD does for this socket, and no PC test can prove that -- if IOS never
 * reported a telemetry socket writable, a gate without a fallback would
 * abandon EVERY line and kill the very stream this lane exists to keep alive
 * (strictly worse than the failure being fixed). So after
 * HW_TELEMETRY_SEND_GATE_STRIKES consecutive abandonments with not one
 * successful line in between -- 5 s at the 250 ms deadline, by which point the
 * collector already calls the heartbeat stale -- the gate disarms itself for
 * the rest of the session and the sender reverts to exactly its pre-2026-09-08
 * blocking behaviour. The signature on the wire is unmistakable: tel_wto
 * stops at the strike count while records resume. Any successful line resets
 * the streak. */
enum { HW_TELEMETRY_SEND_GATE_STRIKES = 20 };
static unsigned s_gate_consec_timeouts;
static bool s_gate_off;

static void send_gate_note_timeout(void) {
	__atomic_fetch_add(&s_sender_write_timeouts, 1, __ATOMIC_RELAXED);
	if(++s_gate_consec_timeouts >= HW_TELEMETRY_SEND_GATE_STRIKES)
		s_gate_off = true;
}

/* True when this chunk may not be STARTED any more: the per-line deadline has
 * passed. Costs no IPC (gettime on Wii, the test clock on PC). */
static bool send_gate_expired(const struct send_gate* gate) {
	if(!gate || !gate->armed || !gate->now_ms)
		return false;
	return (int64_t)(gate->now_ms(gate->user) - gate->deadline_ms) >= 0;
}

static bool send_bytes_all(const char* data, size_t len, send_bytes_fn fn,
						   void* user, struct send_progress* progress,
						   const struct send_gate* gate) {
	size_t off = 0;
	while(off < len) {
		if(send_gate_expired(gate)) {
			/* -ETIMEDOUT is this module's own sentinel for "abandoned by the
			 * deadline", never a socket errno: send_line_with keeps it away
			 * from the fail classifier. */
			progress->error = -ETIMEDOUT;
			return false;
		}
		size_t want = len - off;
		if(want > HW_TELEMETRY_SEND_CHUNK)
			want = HW_TELEMETRY_SEND_CHUNK;
		/* STAGE: this is the call the 2026-09-07 evidence suspects. Timed
		 * per SYSCALL (tel_xmax_us), not per record, and instrumented HERE
		 * rather than inside tcp_send_bytes so the PC test build -- which
		 * drives this exact function through a stub transport -- exercises
		 * the same instrumentation the Wii does. */
		uint64_t sent_at = sender_now_us();
		uint32_t stage_prev = sender_stage_enter(HW_TEL_STAGE_SEND);
		int n = fn(data + off, want, user);
		sender_stage_leave(stage_prev, sent_at, &s_sender_syscall_max_us);
		__atomic_store_n(&s_sender_last_send_ret, (int32_t)n, __ATOMIC_RELAXED);
		s_last_results[0] = s_last_results[1];
		s_last_results[1] = s_last_results[2];
		s_last_results[2] = n;
		if(n <= 0 || (size_t)n > want) {
			progress->error = n < 0 ? n : -EIO;
			/* NON-BLOCKING EXPERIMENT: -EAGAIN/-EWOULDBLOCK from a socket
			 * that cannot wait is not a failure, it is "no room right now".
			 * Abandon the line instead of spending one of the four transient
			 * strikes that exist to detect a DEAD link. Every other negative
			 * errno deliberately falls through to the normal classifier, so
			 * arming the experiment can never mask a genuinely dead socket.
			 * libogc may have queued a partial BEFORE returning the error
			 * (network_wii.c's send loop overwrites its running total with
			 * the error), so the queued length is unknowable and the caller
			 * owes a framing resync -- never a retry. */
			if(s_nonblock_mode
			   && (n == -EAGAIN || n == -EWOULDBLOCK))
				progress->abandoned = true;
			return false;
		}
		off += (size_t)n;
		progress->queued += (size_t)n;
		/* A SHORT write on a non-blocking socket means the send buffer just
		 * filled: the remainder would block. Abandon (bytes definitely
		 * reached the wire, so a resync is owed) rather than spinning. */
		if(s_nonblock_mode && (size_t)n < want) {
			progress->error = -EAGAIN;
			progress->abandoned = true;
			return false;
		}
	}
	return true;
}

/* See HW_TELEMETRY_CRITICAL_POLL_TRIES (hw_telemetry.h). Matched on the
 * exact record prefix every producer writes first, so the classification
 * costs a few byte compares and needs no ring tag. */
bool hw_telemetry_line_is_critical(const char* line) {
	static const char* const prefixes[] = {
		"{\"event\":\"heartbeat\",",
		"{\"event\":\"perf_survival_guard\",",
		"{\"event\":\"anchor_frames\",",
		"{\"event\":\"profile_done\",",
	};
	if(!line)
		return false;
	for(size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
		if(!strncmp(line, prefixes[i], strlen(prefixes[i])))
			return true;
	return false;
}

static bool send_line_with(const char* line, send_bytes_fn fn, void* user,
	struct hw_telemetry_send_fail_state* state, bool* close_socket,
	struct send_gate* gate) {
	if(close_socket)
		*close_socket = false;
	if(state && state->quiesced)
		return false;
	struct send_progress progress = {.error = -EINVAL};
	bool sent = false;
	if(s_gate_off)
		gate = NULL; /* see HW_TELEMETRY_SEND_GATE_STRIKES */
	if(gate)
		gate->armed = false;
	/* Evidence lines: poll retried while the wire is clean, and no
	 * between-chunk deadline once committed (see
	 * HW_TELEMETRY_CRITICAL_POLL_TRIES). `chunk_gate` is what the byte loops
	 * see; NULL means "finish the line", exactly the pre-2026-09-08
	 * blocking behaviour, for these few records only. */
	bool critical = hw_telemetry_line_is_critical(line);
	const struct send_gate* chunk_gate = critical ? NULL : gate;
	if(line && fn) {
		size_t len = strlen(line);
		progress.error = -EMSGSIZE;
		if(len <= HW_TELEMETRY_HEARTBEAT_LINE_SIZE) {
			/* Bounded writability gate, once per line, before any byte (see
			 * struct send_gate). Only a clean timeout abandons: a gate that
			 * cannot answer (<0 -- IOS error, no net heap) falls back to the
			 * pre-2026-09-08 behaviour of just sending, so a broken poll can
			 * never silence a healthy stream. */
			if(gate && gate->now_ms) {
				gate->deadline_ms = gate->now_ms(gate->user) + gate->budget_ms;
				gate->armed = true;
			}
			int poll_ret = 1; /* no gate installed == "just send", as before */
			for(unsigned attempt = 1; gate && gate->ready; attempt++) {
				/* STAGE: net_poll is IPC too -- on the 2026-09-07 runs its
				 * 250 ms timeouts kept firing (tel_wto climbed) while the
				 * sender still made no progress, so it must be timed and
				 * named separately from the send. */
				uint64_t polled_at = sender_now_us();
				uint32_t stage_prev = sender_stage_enter(HW_TEL_STAGE_POLL);
				poll_ret = gate->ready(gate->user, gate->budget_ms);
				sender_stage_leave(stage_prev, polled_at,
								   &s_sender_poll_max_us);
				__atomic_store_n(&s_sender_last_poll_ret, (int32_t)poll_ret,
								 __ATOMIC_RELAXED);
				if(poll_ret != 0)
					break;
				/* Nothing was handed to fn, so the wire framing is intact and
				 * no resync is owed for THIS line. Every timed-out poll is a
				 * real 250 ms wait and is counted (tel_wto, and the
				 * escape-hatch streak, so 20 strikes still mean 5 s of no
				 * progress). An evidence line retries while it is still
				 * clean to retry; the hatch tripping ends the retries so the
				 * NEXT line takes the reverted blocking path, as before. */
				send_gate_note_timeout();
				if(!critical || s_gate_off
				   || attempt >= HW_TELEMETRY_CRITICAL_POLL_TRIES)
					return false;
			}
			// FRAMING RESYNC (2026-09-07): a previously abandoned line may
			// have left an ambiguous prefix on the wire (libogc queues a
			// partial AND reports the error, so its length is unknowable and
			// the bytes must NEVER be retried -- the 2026-09-02 stage-fetch
			// corruption). A bare LF ahead of the next line closes that
			// prefix, so the damage is exactly one malformed NDJSON line,
			// which the collector already skips (note_bad_line: three
			// CONSECUTIVE bad lines to fail). Same trick the sender's alive
			// probe already uses.
			bool resynced = !state || !state->resync
				|| send_bytes_all("\n", 1, fn, user, &progress, chunk_gate);
			/* Y:a:3093 was delivered as a complete JSON body but charged
			 * dropped when the deadline refused its LF. The next resync LF
			 * made that body visible, hence dropped=3 with no seq gap.
			 * Once an observer body is fully accepted, finish exactly its
			 * one delimiter. Body chunks retain the ordinary deadline; no
			 * failed/ambiguous body is replayed. One extra 1-byte SDK call
			 * can still incur IOS service time, which remains unsubtracted. */
			const struct send_gate* delimiter_gate = line
				&& !strncmp(line, "{\"event\":\"service_observer\",", 28)
				? NULL : chunk_gate;
			sent = resynced
				&& send_bytes_all(line, len, fn, user, &progress, chunk_gate)
				&& send_bytes_all("\n", 1, fn, user, &progress, delimiter_gate);
		}
	}
	if(sent) {
		s_gate_consec_timeouts = 0;
		if(state)
			state->resync = false;
		hw_telemetry_send_fail_state_note_success(state);
	} else {
		// A line that never reached the transport at all (NULL or oversized:
		// nothing was handed to fn, nothing queued) is a PRODUCER reject, not
		// a socket failure. Drop it silently -- counting it would inflate
		// tel_fail and, worse, quiesce a perfectly healthy stream.
		if(progress.queued == 0
		   && (progress.error == -EMSGSIZE || progress.error == -EINVAL))
			return false;
		// DEADLINE ABANDONMENT (2026-09-08), not a transport failure: the
		// per-line budget ran out between two chunks. The link is not proven
		// unhealthy, so this must NOT spend one of the four transient strikes
		// that exist to detect a dead one, and must NOT enter trace_stop
		// (whose "first stop" evidence would then name a slow line as the
		// cause of a later, real death). Bytes DID reach the wire, so the
		// next line owes the LF resync; they are never resent.
		if(progress.error == -ETIMEDOUT) {
			send_gate_note_timeout();
			if(state && progress.queued)
				state->resync = true;
			return false;
		}
		// NON-BLOCKING ABANDONMENT (2026-09-08 experiment), also not a
		// transport failure: the socket had no room at this instant. Counted
		// on its own (tel_abd), kept out of the classifier and out of
		// trace_stop for the same reason the deadline is -- naming a
		// momentarily-full send buffer as the "first stop" would poison the
		// evidence for a later, real death. The resync is UNCONDITIONAL
		// here, unlike the deadline case: libogc can queue a partial and
		// still report -EAGAIN, so `queued == 0` does not prove the wire is
		// clean (the 2026-09-02 duplicate-bytes trap).
		if(progress.abandoned) {
			__atomic_fetch_add(&s_sender_abandoned, 1, __ATOMIC_RELAXED);
			if(state)
				state->resync = true;
			return false;
		}
		// Admission cancellation is not a transport failure. The sender loop
		// records orderly shutdown; actual socket errors still retain first stop.
		if(progress.error != -ECANCELED)
			trace_stop(CW_TEL_SEND, line, progress.queued, progress.stalls,
				progress.error, s_last_results[2]);
		hw_telemetry_send_fail_classify(state, progress.error, close_socket);
		// A prefix without its newline poisons NDJSON, so this line is
		// abandoned (never retried) and the NEXT one resyncs. Whether the
		// STREAM dies is now the classifier's decision alone: transient
		// back-pressure (-EAGAIN/-EWOULDBLOCK/-ENOBUFS) needs three
		// consecutive strikes, a hard errno still quiesces at once. Before
		// this, one momentary -ENOBUFS killed telemetry for the whole run.
		if(state && (progress.queued || progress.error < 0))
			state->resync = true;
	}
	return sent;
}

// PRE-CONNECT BUFFER (pull mode): every line emitted before the collector's
// first TCP connect used to be silently dropped -- including the boot event
// (the build-stamp anchor) and any pre-connect fault dump, exactly the
// evidence a boot-crash hunt needs. Buffer them and flush once on the FIRST
// accept, boot line first. Slot 0 pins the FIRST buffered line (the boot
// event, emitted before any client can exist); the remaining slots ring over
// the newest lines. One-shot: after the first flush attempt the buffer is
// retired -- post-connect drops (client died mid-run) stay the crash-mailbox's
// job, not this buffer's.
/* O20 (real-Wii HW1/HW2 vehicle smokes): the buffer used to be eight
 * 12 KB slots -- one line each, sized for the largest heartbeat -- so the
 * host_vehicle_smoke_8m boot burst (12+ mostly sub-1 KB lines before the pull
 * collector connects) evicted its oldest lines, the eviction counted as
 * tel_drop, and that profile's collector fails on any tel_drop at the next
 * heartbeat: every hardware vehicle run died there. The SAME 96 KB is now a
 * packed arena of up to PRECONNECT_MAX_LINES lines, so the burst fits
 * without eviction. Policy is unchanged when the arena is genuinely full:
 * the first line (boot) stays pinned, the OLDEST other lines are evicted,
 * and each eviction is reported to the caller as a drop. Static size is
 * unchanged, so the Wii .bss layout (boot-death gate) does not move. */
#define PRECONNECT_SLOTS 8u /* the arena keeps the old 8 x line-size bytes */
/* residency-1005/4: fixed at the old 8 x 12,288 B so the .bss layout is
 * unchanged by the larger heartbeat line (still >= 7 full lines). */
#define PRECONNECT_ARENA_BYTES (PRECONNECT_SLOTS * 12288u)
_Static_assert(PRECONNECT_ARENA_BYTES >= 7u * HW_TELEMETRY_HEARTBEAT_LINE_SIZE,
	"the preconnect arena must still hold several full heartbeat lines");
#define PRECONNECT_MAX_LINES 64u
static char s_pre_arena[PRECONNECT_ARENA_BYTES];
static uint32_t s_pre_off[PRECONNECT_MAX_LINES];  /* line start in the arena */
static uint32_t s_pre_len[PRECONNECT_MAX_LINES];  /* bytes incl. the NUL */
static unsigned s_pre_count = 0;  /* lines held */
static uint32_t s_pre_used = 0;   /* arena bytes held */
static unsigned s_pre_stored = 0; // total store calls (diagnostic)
static bool s_pre_flushed = false;

static void preconnect_reset(void) {
	s_pre_count = 0;
	s_pre_used = 0;
	s_pre_stored = 0;
	s_pre_flushed = false;
}

/* Drop held line 1 (the oldest after the pinned boot line). */
static void preconnect_evict_oldest(void) {
	if(s_pre_count < 2)
		return;
	const uint32_t off = s_pre_off[1], len = s_pre_len[1];
	memmove(s_pre_arena + off, s_pre_arena + off + len,
			s_pre_used - (off + len));
	s_pre_used -= len;
	for(unsigned k = 1; k + 1 < s_pre_count; k++) {
		s_pre_off[k] = s_pre_off[k + 1] - len;
		s_pre_len[k] = s_pre_len[k + 1];
	}
	s_pre_count--;
}

static unsigned preconnect_store(const char* line) {
	if(!line || s_pre_flushed)
		return 0;
	s_pre_stored++;
	size_t n = strlen(line) + 1u;
	/* A line longer than a heartbeat can only be a dump chunk
	 * (HW_TELEMETRY_DUMP_LINE_SIZE < HW_TELEMETRY_HEARTBEAT_LINE_SIZE); clamp
	 * it like the old per-slot snprintf did. */
	if(n > HW_TELEMETRY_HEARTBEAT_LINE_SIZE)
		n = HW_TELEMETRY_HEARTBEAT_LINE_SIZE;
	unsigned dropped = 0; /* R08 F4: each evicted record, not one per call */
	while(s_pre_count > 1
		  && (s_pre_count >= PRECONNECT_MAX_LINES
			  || s_pre_used + n > PRECONNECT_ARENA_BYTES)) {
		preconnect_evict_oldest();
		dropped++;
	}
	if(s_pre_count >= PRECONNECT_MAX_LINES
	   || s_pre_used + n > PRECONNECT_ARENA_BYTES)
		return dropped + 1u; /* cannot happen: pinned line < 1/8 of the arena */
	memcpy(s_pre_arena + s_pre_used, line, n - 1u);
	s_pre_arena[s_pre_used + n - 1u] = '\0';
	s_pre_off[s_pre_count] = s_pre_used;
	s_pre_len[s_pre_count] = (uint32_t)n;
	s_pre_used += (uint32_t)n;
	s_pre_count++;
	return dropped;
}

// Returns true only when EVERY buffered line was fully sent; *sent_out (may
// be NULL) receives the fully-sent count either way. Retires the buffer
// regardless of outcome. send_line_with quiesces a partially-sent stream;
// the caller closes only on hard errors, never transient backpressure.
static bool preconnect_flush(send_bytes_fn fn, void* user, unsigned* sent_out,
	struct hw_telemetry_send_fail_state* state, bool* close_socket,
	struct send_gate* gate) {
	if(sent_out)
		*sent_out = 0;
	if(s_pre_flushed)
		return true;
	s_pre_flushed = true;
	if(s_pre_count == 0)
		return true;
	unsigned sent = 0;
	bool ok = true;
	for(unsigned k = 0; k < s_pre_count; k++) {
		if(!send_line_with(s_pre_arena + s_pre_off[k], fn, user, state,
						   close_socket, gate)) {
			ok = false;
			break;
		}
		sent++;
	}
	if(sent_out)
		*sent_out = sent;
	return ok;
}
#endif

static bool send_line_mode(const char* line, enum telemetry_send_mode mode);
static bool send_line(const char* line);

static size_t append_raw(char* dst, size_t dst_size, size_t off,
						 const char* src) {
	if(!src)
		src = "";
	size_t len = strlen(src);
	if(dst && dst_size && off < dst_size) {
		size_t room = dst_size - off - 1;
		size_t n = len < room ? len : room;
		memcpy(dst + off, src, n);
		dst[off + n] = '\0';
	}
	return off + len;
}

static size_t append_format(char* dst, size_t dst_size, size_t off,
							const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	if(dst && dst_size && off < dst_size) {
		size_t room = dst_size - off;
		int n = vsnprintf(dst + off, room, fmt, ap);
		va_end(ap);
		if(n < 0)
			return off;
		return off + (size_t)n;
	}
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if(n < 0)
		return off;
	return off + (size_t)n;
}

/* colcap-1006 (ASTRA-CC-R3 item 2): the shared physical-pool + transfer-
 * budget run, used by the split_dims block (at its historical position, so
 * that line is byte-identical) and by the anchor pool block. */
static size_t append_pool_budget(char* dst, size_t dst_size, size_t off,
								 const struct hw_pool_budget_evidence* pb) {
	return append_format(dst, dst_size, off,
		",\"column_pool_used\":%u,\"column_pool_peak\":%u,"
		"\"column_pool_slabs\":%u,\"column_pool_overflow\":%u,"
		"\"column_pool_carve_fail\":%u,\"budget_valid\":%s,\"budget_wii\":%s,"
		"\"budget_resident\":%u,\"budget_live_backings\":%u,\"budget_reserved\":%u,"
		"\"budget_prepeak\":%u,\"budget_peak\":%u,\"budget_waits\":%u,"
		"\"budget_phase\":%u",
		pb->column_pool_used, pb->column_pool_peak, pb->column_pool_slabs,
		pb->column_pool_overflow, pb->column_pool_carve_fail,
		pb->budget_valid ? "true" : "false", pb->budget_wii ? "true" : "false",
		pb->budget_resident, pb->budget_live_backings, pb->budget_reserved,
		pb->budget_prepeak, pb->budget_peak, pb->budget_waits,
		(unsigned)pb->budget_phase);
}

static size_t append_json_string(char* dst, size_t dst_size, size_t off,
								 const char* src, size_t src_len) {
	off = append_raw(dst, dst_size, off, "\"");
	for(size_t i = 0; i < src_len; i++) {
		unsigned char c = (unsigned char)src[i];
		switch(c) {
			case '"': off = append_raw(dst, dst_size, off, "\\\""); break;
			case '\\': off = append_raw(dst, dst_size, off, "\\\\"); break;
			case '\b': off = append_raw(dst, dst_size, off, "\\b"); break;
			case '\f': off = append_raw(dst, dst_size, off, "\\f"); break;
			case '\n': off = append_raw(dst, dst_size, off, "\\n"); break;
			case '\r': off = append_raw(dst, dst_size, off, "\\r"); break;
			case '\t': off = append_raw(dst, dst_size, off, "\\t"); break;
			default:
				if(c < 0x20) {
					char esc[7];
					snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
					off = append_raw(dst, dst_size, off, esc);
				} else {
					char one[2] = {(char)c, '\0'};
					off = append_raw(dst, dst_size, off, one);
				}
				break;
		}
	}
	return append_raw(dst, dst_size, off, "\"");
}

static size_t append_json_cstr(char* dst, size_t dst_size, size_t off,
							   const char* src) {
	if(!src)
		src = "";
	return append_json_string(dst, dst_size, off, src, strlen(src));
}

size_t hw_telemetry_format_boot(char* dst, size_t dst_size, const char* run_id,
								const char* profile, const char* build_id,
								const char* local_ip) {
	if(dst && dst_size)
		dst[0] = '\0';
	size_t off = 0;
	off = append_raw(dst, dst_size, off, "{\"event\":\"boot\",\"run_id\":");
	off = append_json_cstr(dst, dst_size, off, run_id);
	off = append_raw(dst, dst_size, off, ",\"profile\":");
	off = append_json_cstr(dst, dst_size, off, profile);
	off = append_raw(dst, dst_size, off, ",\"build_id\":");
	off = append_json_cstr(dst, dst_size, off, build_id);
	off = append_raw(dst, dst_size, off, ",\"local_ip\":");
	off = append_json_cstr(dst, dst_size, off, local_ip);
	return append_raw(dst, dst_size, off, "}");
}

static uint64_t heartbeat_uptime_ms(void) {
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(s_test_stage_now_us)
		return s_test_stage_now_us(s_test_stage_user) / 1000u;
#endif
#ifdef PLATFORM_WII
	return (uint64_t)time_get() / TB_TIMER_CLOCK;
#else
	return 0;
#endif
#else
	ptime_t now = time_get();
	return (uint32_t)((uint64_t)now.tv_sec * 1000u +
					   (uint64_t)now.tv_nsec / 1000000u);
#endif
}

/* colcap-1006: registered by mem2_init (Wii); see hw_telemetry_set_carve_largest_probe. */
static bool (*s_carve_largest_probe)(uint32_t* bytes) = NULL;
size_t hw_telemetry_format_perf_guard(char* dst, size_t cap, const uint32_t words[47], uint32_t heartbeat_tick) {
	if(!dst || !cap || !words) return 0;
#ifdef PLATFORM_WII
	const unsigned hz = 60750000u;
#else
	const unsigned hz = 1000000u;
#endif
	size_t off = append_format(dst, cap, 0,
		"{\"event\":\"perf_survival_guard\",\"clock_hz\":%u,\"heartbeat_tick\":%u,\"record\":[", hz, heartbeat_tick);
	for(unsigned i = 0; i < 47; i++)
		off = append_format(dst, cap, off, "%s%u", i ? "," : "", words[i]);
	return append_raw(dst, cap, off, "]}");
}

struct hw_telemetry_fps_sample hw_telemetry_sample_fps(
	struct hw_telemetry_fps_window* window, uint32_t frame_counter, uint64_t now_us) {
	struct hw_telemetry_fps_sample sample = {0};
	uint32_t frames = frame_counter - window->frame_counter;
	if(!window->initialized || now_us < window->sampled_us
	   || frames > UINT32_MAX / 2u) {
		window->initialized = true;
		window->frame_counter = frame_counter;
		window->sampled_us = now_us;
		return sample;
	}
	if(now_us == window->sampled_us)
		return sample;
	sample.frames = frames;
	sample.elapsed_us = now_us - window->sampled_us;
	sample.fps = (float)((double)frames * 1000000.0 / (double)sample.elapsed_us);
	window->frame_counter = frame_counter;
	window->sampled_us = now_us;
	return sample;
}
size_t hw_telemetry_format_heartbeat(
	char* dst, size_t dst_size, const char* run_id, const char* profile,
	const struct hw_telemetry_heartbeat* hb) {
	if(dst && dst_size)
		dst[0] = '\0';
	if(!hb)
		return 0;
	/* G9 R6: the 26-entry light-queue array (at most 26 x 5 digits + 25
	 * commas = 155 B + NUL). */
	char lq_text[HW_TELEMETRY_LQ_N * 6 + 1];
	{
		size_t o = 0;
		lq_text[0] = '\0';
		for(int k = 0; k < HW_TELEMETRY_LQ_N; k++) {
			int w = snprintf(lq_text + o, sizeof lq_text - o, k ? ",%u" : "%u",
							 (unsigned)hb->lq[k]);
			if(w < 0 || (size_t)w >= sizeof lq_text - o)
				break;
			o += (size_t)w;
		}
	}
	/* I07 / R5: the 22-entry unsaturated per-window array (22 x 10 digits
	 * + 21 commas = 241 B + NUL). */
	char lqd_text[HW_TELEMETRY_LQD_N * 11 + 1];
	{
		size_t o = 0;
		lqd_text[0] = '\0';
		for(int k = 0; k < HW_TELEMETRY_LQD_N; k++) {
			int w = snprintf(lqd_text + o, sizeof lqd_text - o,
							 k ? ",%u" : "%u", (unsigned)hb->lqd[k]);
			if(w < 0 || (size_t)w >= sizeof lqd_text - o)
				break;
			o += (size_t)w;
		}
	}
	/* mp-split-0912/25: the terminal reason TEXT is the only STRING inside
	 * the one big append_format below, and a peer-supplied string must go
	 * through append_json_string's escaper (the same one build_id/run_id
	 * use) rather than a bare %s. Escape it into this scratch buffer FIRST
	 * so the field can ride that call at its schema position next to
	 * mp_end/_phase/_rx instead of being appended after the line's closing
	 * brace the way death_cause and the optional blocks are. The escaper
	 * emits its own surrounding quotes, so the format string carries a bare
	 * %s for this field.
	 *
	 * Bound: 47 source characters, each at most 6 emitted bytes (\u00xx for
	 * a control byte), plus the two quotes and the NUL. A UTF-8 section sign
	 * (0xC2 0xA7 -- what a Beta kick reason actually carries) is two bytes
	 * both >= 0x20, so it passes through unexpanded; only control bytes hit
	 * the 6x case. If that arithmetic were ever wrong append_json_string
	 * truncates rather than overruns (it takes the destination size), so the
	 * failure mode is a clipped reason, never a stack smash -- and
	 * test_heartbeat_mp_end_reason_escapes saturates all 47 characters with
	 * the 6x escape and pins the exact emitted bytes, so the bound is proved
	 * in the asserts-on test build rather than trusted here (this file is
	 * compiled with NDEBUG in production, where an assert would be a
	 * no-op). */
	char reason_json[(HW_TELEMETRY_MP_END_REASON_SIZE - 1) * 6 + 3];
	{
		/* sizeof - 1, not sizeof: the last byte is the NUL slot, and a
		 * caller that filled all 48 bytes without terminating must still
		 * cost at most the 47 characters this buffer is sized for rather
		 * than tripping the assert below. */
		size_t reason_len = 0;
		while(reason_len < sizeof(hb->mp_end_reason) - 1
			  && hb->mp_end_reason[reason_len] != '\0')
			reason_len++;
		append_json_string(reason_json, sizeof(reason_json), 0,
						   hb->mp_end_reason, reason_len);
	}
	size_t off = 0;
	off = append_raw(dst, dst_size, off,
					 "{\"event\":\"heartbeat\",\"run_id\":");
	off = append_json_cstr(dst, dst_size, off, run_id);
	off = append_raw(dst, dst_size, off, ",\"profile\":");
	off = append_json_cstr(dst, dst_size, off, profile);
	off = append_raw(dst, dst_size, off, ",\"build_id\":");
	off = append_json_cstr(dst, dst_size, off, hb->build_id);
	/* The run-completion clock's time_get() reads this same Wii timebase. */
	off = append_format(dst, dst_size, off, ",\"up_ms\":%llu",
					 (unsigned long long)heartbeat_uptime_ms());
	/* CPU observation extends the legacy schema; never replace its fields. */
	off = append_format(
		dst, dst_size, off,
		",\"tick\":%u,\"%s\":%.2f,\"gpu_ms\":%.2f,\"vsync_ms\":%.2f,"
		"\"ms_bfs\":%.2f,\"ms_light\":%.2f,\"ms_lod\":%.2f,"
		"\"ms_build\":%.2f,\"ms_recv\":%.2f,\"ms_render\":%.2f,"
		"\"ms_tick\":%.2f,\"ms_bfs_max\":%.2f,\"ms_light_max\":%.2f,"
		"\"ms_lod_max\":%.2f,\"ms_build_max\":%.2f,\"chunks\":%d,"
		"\"mesh_q\":%d,\"mesh_stale\":%u,\"mesh_trunc\":%u,"
		"\"estk\":%u,\"near_evict_defer\":%u,\"srv_rt_us\":%d,\"srv_wt_us\":%d,"
		"\"heap_used_kb\":%d,\"heap_free_kb\":%d,\"hblk\":%d,\"htop_kb\":%d,"
		"\"mem1_free_kb\":%d,"
		"\"mem2_free_kb\":%d,\"spill\":%u,\"oom\":%u,\"dlbad\":%u,"
		"\"dlcall\":%u,\"wpad\":%d,\"wksp\":%d,\"wkspT\":%d,\"wkspU\":%d,"
		"\"wkspB\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
		"\"rx_deg\":%.2f,\"ry_deg\":%.2f,\"near_radius\":%d,"
		"\"far_view_distance\":%d,\"fring\":%d,"
		"\"far_missing\":%d,\"far_missing_max\":%d,\"far_settle_ms\":%u,"
		"\"srv_gen_us\":%d,\"srv_gen_cols\":%d,\"srv_gen_backlog\":%d,"
		"\"srv_loop_it\":%u,\"srv_elapsed_ms\":%d,\"srv_ticks_pend\":%u,"
		"\"srv_partial_m\":%d,\"srv_sleep_ms\":%d,\"srv_far_us_max\":%d,"
		"\"cw_set_n\":%u,"
		/* push-0921/B4c: the loading screen's CUMULATIVE backstop-admission
		 * counter. Unconditional -- a missing key is a collector FAIL for the
		 * two 10-minute MP profiles, so it may never be profile-gated (see
		 * hw_heartbeat's lw_to_admits comment). */
		"\"lw_to_admits\":%u,"
		"\"cflood\":%u,\"ndisp\":%u,"
		/* G9 R6 (ASTRA-G9-REVIEW-2 item 5): the pending-light queue as ONE
		 * positional array of 26 saturating uint16 values (the hw_heartbeat
		 * field comment fixes the order): 59 B at zero, 163 B at its widest
		 * -- 13 B over the 13-entry uint32 v1 array, which the widest
		 * split_dims line can spare above its 200 B floor in the 12,288 B
		 * slot (uint32 fields would cost 293 B and could not). */
		"\"lq\":[%s],"
		/* I07: the unsaturated per-heartbeat-window drain record (hw_heartbeat
		 * lqd comment fixes the order): 52 B at zero, 241 B at its widest. */
		"\"lqd_v\":3,\"lqd\":[%s],"
		"\"wd_main\":%u,"
		"\"wd_srv\":%u,\"wd_worker\":%u,\"wd_hb_gap_max_ms\":%u,"
		"\"world_loaded\":%s,\"map_step\":%d,\"map_max\":%d,"
		"\"np_entered\":%u,\"np_skipped\":%u,\"np_focus\":%u,"
		"\"np_flags\":%u,\"np_rows\":%d,"
		"\"map_sign\":%s,\"smig_state\":%u,\"smig_ms\":%u,"
		"\"smig_regions\":%u,\"smig_chunks\":%u,\"smig_signs\":%u,"
		"\"smig_expected\":%u,\"smig_verified\":%u,"
		"\"smig_chests\":%u,\"smig_chests_expected\":%u,"
		"\"smig_chests_verified\":%u,\"smig_spawners\":%u,"
		"\"smig_spawners_expected\":%u,\"smig_spawners_verified\":%u,"
		/* lava-aabb-0924/H1: chunk-save failure visibility (hw_heartbeat's
		 * field comment). Additive keys, always emitted. */
		"\"save_fail_n\":%u,\"save_fail_stage\":%u,\"save_fail_sub\":%u,"
		"\"save_skip_n\":%u,\"save_idx_fb\":%u,"
		"\"mp_state\":%u,\"mp_gen\":%u,\"mp_login\":%u,\"mp_ret_kb\":%u,"
		"\"mp_ret_n\":%u,\"mp_ready\":%u,\"mp_backlog\":%u,\"mp_evt_q\":%u,"
		"\"mp_evict\":%u,\"mp_alloc_fail\":%u,\"mp_reconn\":%u,"
		"\"wl_pending\":%u,\"wl_failed\":%u,"
		"\"mp_rx_kb\":%u,"
		/* Terminal-event latch (mp-split-0912/5): how this connect
		 * generation ended, the phase it ended in, and the exact byte
		 * count at that moment -- see hw_heartbeat's field comments. */
		"\"mp_end\":%u,\"mp_end_phase\":%u,\"mp_end_rx\":%u,"
		/* mp-split-0912/25: the reason TEXT the three fields above cannot
		 * carry -- pre-escaped into reason_json at the top of this function,
		 * quotes included, hence the bare %s. Empty string whenever
		 * mp_end == 0. */
		"\"mp_end_reason\":%s,"
		"\"mp_dec_phase\":%u,\"mp_dec_pkt\":%u,"
		"\"mp_dec_have\":%u,\"mp_dec_need\":%u,"
		"\"mp_move_ticks\":%u,\"mp_move_samples\":%u,\"rpcdrop\":%u,"
		/* MP join-burst admission budget (W4, design section 2.7). Purely
		 * ADDITIVE JSON keys: the Python collector parses the object by name
		 * and is untouched by this slice, so an older collector reading a
		 * newer heartbeat simply does not see them. */
		"\"mp_adm_radius\":%u,\"mp_resident_cols\":%u,"
		"\"mp_backed_cols\":%u,\"mp_backing_kb\":%u,"
		"\"mp_backing_peak_kb\":%u,\"mp_demotions\":%u,"
		"\"mp_compactions\":%u,\"mp_backing_refusals\":%u,"
		"\"mp_ready_pending\":%u,"
		"\"music_key\":%u,\"music_ms\":%u,\"music_underruns\":%u,"
		"\"music_starts\":%u,\"music_rejects\":%u,\"music_dur\":%u,"
		"\"mrs_cycles\":%u,\"mrs_target\":%u,\"mrs_wedge\":%s,"
		"\"creative\":%s,\"dlcalls\":%u,\"tris\":%u,"
		"\"ms_mesh_worker\":%.2f,\"ms_mesh_worker_max\":%.2f,"
		"\"mem2_lock_wait\":%u,"
		"\"tel_fail\":%u,\"tel_last_errno\":%d,\"tel_quiesced\":%s,"
		"\"frame_counter\":%u,\"mem2_carve_free_kb\":%d,"
		"\"chunks_meshed\":%u,\"near_missing\":%d,"
		"\"near_mesh_pending\":%d,\"near_missing_max\":%d,"
		"\"near_settle_ms\":%u,\"edit_to_visible_frames\":%u,"
		"\"load_screen_ms\":%u,\"player_health\":%d,\"loading_screen\":%s,\"perf_frames\":%u,\"perf_max_us\":%u,"
		"\"perf_over_30ms\":%u,\"perf_gpu_over_16600\":%u,"
		"\"perf_gpu_over_8000\":%u,\"perf_invalid\":%u,"
		"\"perf_duration_us\":%llu,\"far_storage\":%d,\"vi_ready_max_us\":%u,\"vi_ready_over_8000\":%u,\"gpu_poll_late_max_us\":%u,\"gpu_poll_late_over_8000\":%u,\"server_time\":%u,\"sd_trace_max_us\":%u,\"sd_trace_over_30ms\":%u,"
		/* Sender self-telemetry (2026-09-07): tel_drop = ring admission
		 * refusals (monotonic), tel_it = sender loop passes in the last
		 * heartbeat window, tel_sends = records handed to the transport in
		 * that window, tel_smax_us = longest single transport call in it
		 * (Wii only), tel_qmax = high-water ring occupancy 0..5 including
		 * the heartbeat lane, tel_hb_repl = heartbeats superseded in that
		 * lane. Worst case 137 bytes. tel_wto (2026-09-08, +21 bytes at its
		 * widest) = lines abandoned by the bounded-send deadline, monotonic:
		 * evidence of a SLOW link, never a dead one -- deliberately not part
		 * of tel_fail nor of the transient quiesce strikes.
		 *
		 * tel_it SCALE CHANGED 2026-09-08 (signalled idle park). The sender
		 * used to poll with usleep(1000), so a healthy idle window read
		 * ~1000. It now parks on a condvar with a 10 ms ceiling, so a healthy
		 * IDLE window reads ~100 plus one pass per record handled. The
		 * DIAGNOSTIC meaning is unchanged and is the only thing any consumer
		 * asserts on: 0 still means the sender was never scheduled in that
		 * window (the 2026-09-07 death signature). Do NOT compare a tel_it
		 * from a run before this change against one after it. */
		"\"tel_drop\":%u,\"tel_it\":%u,\"tel_sends\":%u,"
		"\"tel_smax_us\":%u,\"tel_qmax\":%u,\"tel_hb_repl\":%u,"
		"\"tel_wto\":%u,"
		/* SENDER STAGE (2026-09-08 wedge lane, +170 bytes at their widest).
		 * tel_stg = which call the sender was in when this heartbeat was
		 * composed (enum hw_telemetry_sender_stage), tel_stg_us = how long
		 * it had been there. These two ride the heartbeat precisely because
		 * the heartbeat is the ONE record with a priority lane: on the
		 * 2026-09-07 deaths it still got through every few seconds while
		 * tel_it read 0, so it is the only carrier that can name the stuck
		 * call from the host side while the wedge is happening.
		 * tel_pmax_us/tel_xmax_us are the windowed longest SINGLE net_poll
		 * and net_send -- tel_smax_us times a whole record and therefore
		 * cannot separate the two IOS calls. tel_abd counts lines given up
		 * by the opt-in non-blocking experiment (0 unless armed), kept
		 * distinct from tel_wto's deadline abandonments. tel_pret/tel_prev/
		 * tel_sret are the last raw net_poll return, its revents, and the
		 * last net_send return. */
		"\"tel_stg\":%u,\"tel_stg_us\":%u,\"tel_pmax_us\":%u,"
		"\"tel_xmax_us\":%u,\"tel_abd\":%u,\"tel_pret\":%d,"
		"\"tel_prev\":%u,\"tel_sret\":%d,"
		"\"srv_us\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],"
		"\"guard_scan_us\":[%u,%u],\"world_us\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"worst_parts_us\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}",
		hb->tick, hb->fps_window_present ? "wf" : "fps", hb->fps,
		hb->gpu_ms, hb->vsync_ms, hb->ms_bfs,
		hb->ms_light, hb->ms_lod, hb->ms_build, hb->ms_recv,
		hb->ms_render, hb->ms_tick, hb->ms_bfs_max, hb->ms_light_max,
		hb->ms_lod_max, hb->ms_build_max, hb->chunks_rendered,
		hb->mesh_pending, hb->mesh_stale,
		hb->mesh_trunc, hb->evict_stuck, hb->near_evict_defer,
		hb->srv_rt_us, hb->srv_wt_us,
		hb->heap_used_kb, hb->heap_free_kb, hb->heap_blocks, hb->heap_top_kb,
		hb->mem1_free_kb,
		hb->mem2_free_kb, hb->mem2_spill, hb->mem2_oom, hb->dlbad,
		hb->dlcall, hb->wpad_probe0, hb->wkspace_free, hb->wkspace_total,
		hb->wkspace_used, hb->wkspace_free_blocks, hb->x, hb->y,
		hb->z, hb->rx_deg,
		hb->ry_deg,
		hb->near_radius, hb->far_view_distance, hb->far_ring_width,
		hb->far_missing, hb->far_missing_max, hb->far_settle_ms,
		hb->srv_gen_us, hb->srv_gen_cols, hb->srv_gen_backlog,
		hb->srv_loop_it, hb->srv_elapsed_ms, hb->srv_ticks_pend,
		hb->srv_partial_m, hb->srv_sleep_ms, hb->srv_far_us_max,
		hb->cw_set_n,
		hb->lw_to_admits,
		hb->cflood, hb->notify_dispatch_total,
		lq_text, lqd_text,
		hb->wd_main, hb->wd_srv, hb->wd_worker,
		hb->wd_hb_gap_max_ms,
		hb->world_loaded ? "true" : "false", hb->menu_ap_step,
		hb->menu_ap_max_step,
		(unsigned)hb->np_entered, (unsigned)hb->np_skipped,
		(unsigned)hb->np_focus, (unsigned)hb->np_flags, hb->np_rows,
		hb->menu_ap_sign_reached ? "true" : "false",
		hb->save_migrate_state, hb->save_migrate_ms,
		hb->save_migrate_regions, hb->save_migrate_chunks,
		hb->save_migrate_signs, hb->save_migrate_expected,
		hb->save_migrate_verified, hb->save_migrate_chests_rewritten,
		hb->save_migrate_chests_expected, hb->save_migrate_chests_verified,
		hb->save_migrate_spawners_synthesized,
		hb->save_migrate_spawners_expected,
		hb->save_migrate_spawners_verified,
		hb->save_fail_n, hb->save_fail_stage, hb->save_fail_sub,
		hb->save_skip_n, hb->save_idx_fb,
		hb->mp_state, hb->mp_gen, hb->mp_logged_in, hb->mp_ret_kb,
		hb->mp_ret_entries, hb->mp_ready, hb->mp_backlog, hb->mp_evt_q,
		hb->mp_evict, hb->mp_alloc_fail, hb->mp_reconn,
		hb->wl_pending, hb->wl_failed,
		hb->mp_rx_kb, hb->mp_end, hb->mp_end_phase, hb->mp_end_rx,
		reason_json,
		hb->mp_dec_phase, hb->mp_dec_pkt,
		hb->mp_dec_have, hb->mp_dec_need,
		hb->mp_move_ticks, hb->mp_move_samples, svin_dropped_count(),
		hb->mp_adm_radius, hb->mp_resident_cols, hb->mp_backed_cols,
		hb->mp_backing_kb, hb->mp_backing_peak_kb, hb->mp_demotions,
		hb->mp_compactions, hb->mp_backing_refusals, hb->mp_ready_pending,
		hb->music_key, hb->music_ms, hb->music_underruns,
		hb->music_starts, hb->music_rejects, hb->music_dur,
		hb->mp_reconnect_cycles, hb->mp_reconnect_target,
		hb->mp_reconnect_wedge ? "true" : "false",
		hb->creative ? "true" : "false", hb->dlcalls, hb->tris,
		hb->ms_mesh_worker, hb->ms_mesh_worker_max, hb->mem2_lock_wait,
		hb->tel_fail, hb->tel_last_errno, hb->tel_quiesced ? "true" : "false",
		hb->frame_counter, hb->mem2_carve_free_kb, hb->chunks_meshed,
		hb->near_missing, hb->near_mesh_pending, hb->near_missing_max,
		hb->near_settle_ms,
		hb->edit_to_visible_frames, hb->load_screen_ms,
		hb->player_health, hb->loading_screen ? "true" : "false",
		hb->frame_evidence.frames, hb->frame_evidence.max_us,
		hb->frame_evidence.over_30ms, hb->frame_evidence.gpu_over_16600,
		hb->frame_evidence.gpu_over_8000, hb->frame_evidence.invalid,
		(unsigned long long)hb->frame_evidence.duration_us, hb->far_storage,
		hb->frame_evidence.vi_ready_max_us, hb->frame_evidence.vi_ready_over_8000,
		hb->frame_evidence.gpu_poll_late_max_us, hb->frame_evidence.gpu_poll_late_over_8000,
		hb->server_time, hb->frame_evidence.sd_trace_max_us,
		hb->frame_evidence.sd_trace_over_30ms,
		hb->tel.drops, hb->tel.iterations, hb->tel.sends,
		hb->tel.max_send_us, hb->tel.occupancy_max, hb->tel.hb_replaced,
		hb->tel.write_timeouts,
		/* MASKED to the same 3 bits the sender packs the stage into, so the
		 * field's width is a STRUCTURAL one digit rather than the ten a bare
		 * uint32 would force this formatter to reserve. For every value the
		 * sender can actually produce (stats_take derives it as packed >> 29
		 * on a uint32) the mask is a no-op; it exists so the worst-case line
		 * budget below reflects the field's real bound instead of its type's.
		 * Worth 9 bytes of a slot with very little left -- see the worst-case
		 * assert in test_heartbeat_production_buffer_fits_full_payload. */
		hb->tel.stage & 7u, hb->tel.stage_us, hb->tel.poll_max_us,
		hb->tel.send_max_us, hb->tel.abandoned, (int)hb->tel.last_poll_ret,
		hb->tel.last_poll_revents, (int)hb->tel.last_send_ret,
		hb->server_work_us[0], hb->server_work_us[1], hb->server_work_us[2],
		hb->server_work_us[3], hb->server_work_us[4], hb->server_work_us[5],
		hb->server_work_us[6], hb->server_work_us[7], hb->server_work_us[8],
		hb->server_work_us[9], hb->server_work_us[10],
		hb->guard_scan_us[0], hb->guard_scan_us[1],
		hb->world_work_us[0], hb->world_work_us[1], hb->world_work_us[2], hb->world_work_us[3], hb->world_work_us[4], hb->world_work_us[5], hb->world_work_us[6], hb->world_work_us[7], hb->world_work_us[8], hb->world_work_us[9], hb->world_work_us[10], hb->world_work_us[11], hb->world_work_us[12], hb->world_work_us[13],
		hb->frame_evidence.worst_parts_us[0], hb->frame_evidence.worst_parts_us[1], hb->frame_evidence.worst_parts_us[2],
		hb->frame_evidence.worst_parts_us[3], hb->frame_evidence.worst_parts_us[4], hb->frame_evidence.worst_parts_us[5],
		hb->frame_evidence.worst_parts_us[6], hb->frame_evidence.worst_parts_us[7], hb->frame_evidence.worst_parts_us[8],
		hb->frame_evidence.worst_parts_us[9], hb->frame_evidence.worst_parts_us[10],
		hb->frame_evidence.worst_parts_us[11], hb->frame_evidence.worst_parts_us[12],
		hb->frame_evidence.worst_parts_us[13],
		hb->frame_evidence.worst_parts_us[14], hb->frame_evidence.worst_parts_us[15],
		hb->frame_evidence.worst_parts_us[16], hb->frame_evidence.worst_parts_us[17], hb->frame_evidence.worst_parts_us[18], hb->frame_evidence.worst_parts_us[19]);
	/* S-A3 (Astra H3): the live split toggle, ALWAYS emitted -- a run that
	 * never toggled still has to SAY 0/0/0/-1, or "no key" and "no toggle"
	 * are the same line. split_last_ok is CLAMPED to its -1/0/1 domain (the
	 * getter can produce nothing else), so the worst-case line budget below
	 * reserves 2 characters for it rather than the 11 a bare int would force
	 * -- same reasoning as the tel.stage mask above, and worth 9 bytes of a
	 * slot with little left. */
	off = append_format(dst, dst_size, off - 1,
		",\"split_live\":%u,\"split_pending\":%u,\"split_toggle_ms\":%u,"
		"\"split_last_ok\":%d}",
		(unsigned)hb->split_live, (unsigned)hb->split_pending,
		hb->split_toggle_ms,
		hb->split_last_ok < 0 ? -1 : (hb->split_last_ok > 0 ? 1 : 0));
	const char* death_cause = !hb->anchor.deaths ? "none"
		: hb->anchor.death_cause ? hb->anchor.death_cause : "unknown";
	if(hb->death_present) {
		off = append_format(dst, dst_size, off - 1,
			",\"d1\":[%u,%u,%u,%u,%u]}", hb->death_record[0],
			hb->death_record[1], hb->death_record[2], hb->death_record[3],
			hb->death_record[4]);
	} else {
		off = append_format(dst, dst_size, off - 1,
			",\"deaths\":%u,\"death_cause\":", hb->anchor.deaths);
		off = append_json_cstr(dst, dst_size, off, death_cause);
		off = append_raw(dst, dst_size, off, "}");
	}
	/* Only a session-valid ZERO-death d1 may carry the marker. Its widest
	 * encoding saves 18 bytes against the former 44-byte legacy pair, and
	 * g1 costs 7: even with the G9 lease row the established reserve grows.
	 * Any death/unavailable snapshot fails the separate authoritative gate. */
	if(hb->perf_guard_present && hb->perf_guard_words[2]
	   && hb->death_present && hb->death_record[0]
	   && !hb->death_record[1] && !hb->death_record[2]
	   && !hb->death_record[3] && !hb->death_record[4])
		off = append_raw(dst, dst_size, off - 1, ",\"g1\":1}");
	off = append_format(dst, dst_size, off - 1,
		",\"tnt_valid\":%u,\"tnt_st\":%u,\"tnt_bits\":%u,"
		"\"tnt_primed_left\":%u,\"tnt_carved\":%u,\"tnt_pad\":%u,"
		"\"tnt_gen\":%u,\"tnt_tick\":%u}",
		hb->tnt_valid, hb->tnt_st, hb->tnt_bits, hb->tnt_primed_left,
		hb->tnt_carved, hb->tnt_pad, hb->tnt_gen, hb->tnt_tick);
	if(hb->rskip) /* REL-MP-FIX3b; see hw_heartbeat.rskip */
		off = append_format(dst, dst_size, off - 1, ",\"rskip\":%u}", hb->rskip);
	if(hb->rns) /* REL-MP-FIX3d; see hw_heartbeat.rns */
		off = append_format(dst, dst_size, off - 1, ",\"rns\":%u}", hb->rns);
	if(hb->clq[0] || hb->clq[1] || hb->clq[2] || hb->clq[3]) /* hw_heartbeat.clq */
		off = append_format(dst, dst_size, off - 1,
			",\"clq\":[%u,%u,%u,%u]}", hb->clq[0], hb->clq[1], hb->clq[2],
			hb->clq[3]);
	if(hb->lwc_present) /* REL-MP-COREFIRST; see hw_heartbeat.lwc */
		off = append_format(dst, dst_size, off - 1,
			",\"lwc\":[%u,%u,%u,%u]}", hb->lwc[0], hb->lwc[1], hb->lwc[2],
			hb->lwc[3]);
	/* nether-dark/1; see hw_heartbeat.blc. Never beside the dims block (its
	 * line keeps its 200 B spare floor), the dlo/pac placement rule. */
	if(hb->blc_present && !hb->dims.enabled)
		off = append_format(dst, dst_size, off - 1,
			",\"blc\":[%d,%u,%u,%u,%u,%u,%u]}", (int)hb->blc_dim, hb->blc[0],
			hb->blc[1], hb->blc[2], hb->blc[3], hb->blc[4], hb->blc[5]);
	if(hb->ssw_present) /* hw1030n; see hw_heartbeat.ssw */
		off = append_format(dst, dst_size, off - 1, ",\"ssw\":[%u,%u]}",
			hb->ssw[0], hb->ssw[1]);
	/* ci-1101/37: the longest >= 10 ms drain mutex wait this window, with the
	 * contended lock named and its holder -- only when one happened, so a
	 * quiet heartbeat keeps its bytes. The name is a build-time literal
	 * (thread_stripe_set_name), never console input: it carries the same
	 * escaping guard as mp_end_reason all the same, and an empty name emits
	 * "" rather than dropping the key. */
	if(hb->lqd_wait_present) {
		char name_text[THREAD_DRAIN_WAIT_MUTEX_NAME_MAX * 6 + 1];
		const char* nm = hb->lqd_wait_name;
		append_json_string(name_text, sizeof name_text, 0, nm, strlen(nm));
		off = append_format(dst, dst_size, off - 1,
			",\"lqd_w\":[%u,%u,%u,%u,%u,%s]}", hb->lqd_wait_us,
			hb->lqd_wait_mutex, hb->lqd_wait_holder_id,
			hb->lqd_wait_holder_tag, hb->lqd_wait_stripe, name_text);
	}
	if(hb->mm_valid)
		off = append_format(dst, dst_size, off - 1,
			",\"mm_valid\":%u,\"mm_st\":%u,\"mm_bits\":%u,"
			"\"mm_recur_ok\":%u,\"mm_recur_bad\":%u,\"mm_rise_milli\":%u,"
			"\"mm_step_milli\":%u,\"mm_gen\":%u,\"mm_tick\":%u}",
			hb->mm_valid, hb->mm_st, hb->mm_bits, hb->mm_recur_ok,
			hb->mm_recur_bad, hb->mm_rise_milli, hb->mm_step_milli,
			hb->mm_gen, hb->mm_tick);
	if(hb->mm_moves)
		off = append_format(dst, dst_size, off - 1,
			",\"mm_moves\":%u,\"mm_move_us\":%u,\"mm_env_walks\":%u,"
			"\"mm_env_ents\":%u}",
			hb->mm_moves, hb->mm_move_us, hb->mm_env_walks, hb->mm_env_ents);
	{
		/* residency-1005/2 + /4 closure item 8: the per-actor residency
		 * block, schema v2 -- "rzv":2, "rzt" (server tick), "rza" (two rows
		 * of full-width values, server_local.h enum server_local_rza_field)
		 * and "rzi" (induced marks) -- only when either actor has any
		 * residency evidence (column 0, the world slot, alone does not
		 * count) or a mark exists: legacy wire bytes otherwise. */
		bool rz_any = hb->rzi_count != 0 || hb->rzf != 0;
		for(unsigned a = 0; a < 2; a++)
			for(unsigned i = 1; i < HW_TELEMETRY_RZA_FIELDS; i++)
				rz_any |= hb->rza[a][i] != 0;
		if(rz_any) {
			off = append_format(dst, dst_size, off - 1,
				",\"rzv\":2,\"rzt\":%u,\"rzf\":%u,\"rza\":[", hb->rz_tick,
				hb->rzf);
			for(unsigned a = 0; a < 2; a++) {
				for(unsigned i = 0; i < HW_TELEMETRY_RZA_FIELDS; i++)
					off = append_format(dst, dst_size, off,
						i ? ",%u" : (a ? ",[%u" : "[%u"), hb->rza[a][i]);
				off = append_raw(dst, dst_size, off, "]");
			}
			off = append_raw(dst, dst_size, off, "]");
			off = append_raw(dst, dst_size, off, ",\"rze\":[");
			for(unsigned a = 0; a < 2; a++) {
				for(unsigned i = 0; i < 8; i++)
					off = append_format(dst, dst_size, off,
						i ? ",%u" : (a ? ",[%u" : "[%u"), hb->rze[a][i]);
				off = append_raw(dst, dst_size, off, "]");
			}
			off = append_raw(dst, dst_size, off, "]");
			const unsigned nm = hb->rzi_count < HW_TELEMETRY_RZI_MAX
				? hb->rzi_count : HW_TELEMETRY_RZI_MAX;
			if(nm) {
				off = append_raw(dst, dst_size, off, ",\"rzi\":[");
				for(unsigned m = 0; m < nm; m++) {
					for(unsigned i = 0; i < HW_TELEMETRY_RZI_FIELDS; i++)
						off = append_format(dst, dst_size, off,
							i ? ",%u" : (m ? ",[%u" : "[%u"), hb->rzi[m][i]);
					off = append_raw(dst, dst_size, off, "]");
				}
				off = append_raw(dst, dst_size, off, "]");
			}
			off = append_raw(dst, dst_size, off, "}");
		}
	}
	if(hb->dims.enabled) {
		off = append_format(dst, dst_size, off - 1,
			",\"dim_v0\":%d,\"dim_v1\":%d,\"views_live\":%u,"
			"\"dims_active\":%s,\"dims_phase\":\"%s\","
			"\"srv_dim_host\":%d,\"srv_dim_guest\":%d,"
			"\"host_phase\":\"%s\",\"host_commits\":%u,\"slot1_cols\":%d,"
			"\"rel_sent\":%u,\"rel_rx\":%u,\"rel_slot\":%d,"
			"\"wslot_v0\":%d,\"wslot_v1\":%d",
			hb->dims.dim_v0, hb->dims.dim_v1, hb->dims.views_live,
			hb->dims.dims_active ? "true" : "false",
			hb->dims.phase ? hb->dims.phase : "pane_wait",
			hb->dims.srv_dim_host, hb->dims.srv_dim_guest,
			hb->dims.host_phase ? hb->dims.host_phase : "wait",
			hb->dims.host_commits, hb->dims.slot1_cols, hb->dims.rel_sent,
			hb->dims.rel_rx, hb->dims.rel_slot, hb->dims.wslot_v0,
			hb->dims.wslot_v1);
		/* colcap-1006 (ASTRA-CC-R3 item 2): the shared pool/budget run sits
		 * at exactly the position it always had -- byte-identical. */
		off = append_pool_budget(dst, dst_size, off, &hb->dims.pb);
		off = append_format(dst, dst_size, off,
			",\"transfer_request\":%u,\"guest_commits\":%u,"
			"\"transfer_actor\":%u,\"transfer_state\":%u,\"transfer_held\":%s,"
			"\"transfer_source_draining\":%s,\"transfer_restoring\":%s,"
			"\"host_rb_id\":%u,\"host_rb_reason\":%u,"
			"\"guest_rb_id\":%u,\"guest_rb_reason\":%u}",
			hb->dims.transfer_request,
			hb->dims.guest_commits, (unsigned)hb->dims.transfer_actor,
			(unsigned)hb->dims.transfer_state,
			hb->dims.transfer_held ? "true" : "false",
			hb->dims.transfer_source_draining ? "true" : "false",
			hb->dims.transfer_restoring ? "true" : "false",
			hb->dims.host_rb_id, (unsigned)hb->dims.host_rb_reason,
			hb->dims.guest_rb_id, (unsigned)hb->dims.guest_rb_reason);
	}
	if(hb->anchor.enabled) {
		const struct hw_anchor_evidence* a = &hb->anchor;
		const bool death_unavailable = hb->death_present && !hb->death_record[0];
		char death_count[11];
		if(!death_unavailable) snprintf(death_count, sizeof(death_count), "%u", a->deaths);
		off--; /* Replace only the closing brace; old fields keep their bytes.
				*/
		off = append_format(
			dst, dst_size, off,
			",\"anchor_sep\":%u,\"anchor_ready\":%s,\"anchor_measuring\":%s,"
			"\"anchor_ms\":%u,\"anchor_collision\":%s,"
			"\"anchor_near_owed\":[%u,%u],\"anchor_oldest\":[%u,%u],"
			"\"anchor_missing\":[%u,%u],\"anchor_mesh_pending\":[%u,%u],"
			"\"anchor_far_missing\":[%u,%u],\"anchor_far_max\":[%u,%u],"
			"\"anchor_far_mesh_pending\":[%u,%u],\"anchor_ready_streak\":%u,"
			"\"anchor_load_faults\":[%u,%u,%u],\"anchor_run_faults\":[%u,%u,%u],"
			"\"anchor_deaths\":%s,\"anchor_walk\":[%u,%u,%u,%u,%u],"
			"\"actual_tps\":%.3f,\"tick_backlog\":%u,\"anchor_load_carve_min\":"
			"%d,\"anchor_run_carve_min\":%d,"
			"\"anchor_frame_total\":%u,\"anchor_frame_dropped\":%u,\"anchor_"
			"frame_us\":[",
			a->separation, a->ready ? "true" : "false",
			a->measuring ? "true" : "false", a->elapsed_ms,
			a->collision ? "true" : "false", a->near_owed[0], a->near_owed[1],
			a->oldest[0], a->oldest[1], a->missing[0], a->missing[1],
			a->mesh_pending[0], a->mesh_pending[1], a->far_missing[0],
			a->far_missing[1], a->far_max[0], a->far_max[1],
			a->far_mesh_pending[0], a->far_mesh_pending[1], a->ready_streak,
			a->faults_load[0], a->faults_load[1], a->faults_load[2],
			a->faults_run[0], a->faults_run[1], a->faults_run[2],
			death_unavailable ? "null" : death_count,
			a->walk_tick, a->walk_stage, (unsigned)a->walk_brake,
			(unsigned)a->walk_water, (unsigned)a->walk_capture, a->actual_tps,
			a->tick_backlog, a->carve_load_min, a->carve_run_min,
			a->frame_total, a->frame_dropped);
		for(uint32_t i = 0; i < a->frame_count && i < HW_ANCHOR_FRAME_CAP; i++)
			off = append_format(dst, dst_size, off, "%s%u", i ? "," : "",
								a->frame_us[i]);
		off = append_raw(dst, dst_size, off, "],\"anchor_death_cause\":");
		off = death_unavailable ? append_raw(dst, dst_size, off, "null")
			: append_json_cstr(dst, dst_size, off, death_cause);
		off = append_raw(dst, dst_size, off, ",\"anchor_damage_cause\":");
		off = append_json_cstr(dst, dst_size, off, a->damage_cause);
		/* S1-d (mp-split-0912/68): the two owners' ACCEPTED-action tallies and
		 * their live health, appended at the END of the anchor block so a
		 * parallel field lane merges without touching the format string above.
		 * Fixed-order compact arrays -- [dig, place, attack, use, hotbar,
		 * drop] -- because split_actions_6m reads them positionally and a
		 * named object would cost ~60 B per owner for no added meaning.
		 * ALWAYS emitted when the anchor block is: a run where the guest did
		 * nothing has to be a visible [0,0,0,0,0,0], not a missing key (the
		 * collector treats a missing array as invalid evidence, which is the
		 * right verdict but the wrong REASON). Worst-case cost is 150 B for
		 * the two arrays plus 39 B for act_health; see the measured total in
		 * test_hw_telemetry.c. */
		for(unsigned p = 0; p < 2; p++) {
			off = append_format(dst, dst_size, off, ",\"act%u\":[", p);
			for(unsigned k = 0; k < HW_ANCHOR_ACTION_COUNT; k++)
				off = append_format(dst, dst_size, off, "%s%u", k ? "," : "",
									a->actions[p][k]);
			off = append_raw(dst, dst_size, off, "]");
		}
		off = append_format(dst, dst_size, off,
							",\"act_health\":[%d,%d]", a->health[0],
							a->health[1]);
		off = append_raw(dst, dst_size, off, "}");
	}
	if(hb->pool.enabled) {
		/* colcap-1006 (ASTRA-CC-R3 item 2): anchor-run memory evidence.
		 * "cpv" is the schema marker the collector keys validation on; the
		 * shared pool/budget run is skipped when the dims block above
		 * already carried it (no duplicate keys). */
		off = append_raw(dst, dst_size, off - 1, ",\"cpv\":3");
		if(!hb->dims.enabled)
			off = append_pool_budget(dst, dst_size, off, &hb->pool.pb);
		/* item 3: per-slab occupancy + display-list bytes. */
		const uint32_t ns = hb->pool.slab_count < HW_POOL_SLAB_REPORT_MAX
			? hb->pool.slab_count : HW_POOL_SLAB_REPORT_MAX;
		off = append_raw(dst, dst_size, off, ",\"cps\":[");
		for(uint32_t i = 0; i < ns; i++)
			off = append_format(dst, dst_size, off, i ? ",%u" : "%u",
								(unsigned)hb->pool.slab_used[i]);
		off = append_format(dst, dst_size, off,
			"],\"dll\":%u,\"dlb\":%u,\"cpe\":[%u,%u,%u,%u]",
			hb->pool.dl_lists, hb->pool.dl_bytes, hb->pool.event_seq,
			hb->pool.shrinks, hb->pool.shrink_fail, hb->pool.events_lost);
		/* R4 point 7: display-list owner census (never beside the dims
		 * block, whose line keeps its 200 B spare floor). */
		if(hb->pool.dlo_valid && !hb->dims.enabled) {
			off = append_raw(dst, dst_size, off, ",\"dlo\":[");
			for(unsigned i = 0; i < HW_DLO_FIELDS; i++)
				off = append_format(dst, dst_size, off, i ? ",%u" : "%u",
									hb->pool.dlo[i]);
			off = append_raw(dst, dst_size, off, "]");
		}
		/* R5 item 3: allocator CPU/lock cost (same placement rule). */
		if(hb->pool.pac_valid && !hb->dims.enabled) {
			off = append_raw(dst, dst_size, off, ",\"pac\":[");
			for(unsigned i = 0; i < HW_PAC_FIELDS; i++)
				off = append_format(dst, dst_size, off, i ? ",%u" : "%u",
									hb->pool.pac[i]);
			off = append_raw(dst, dst_size, off, "]");
		}
		off = append_raw(dst, dst_size, off, "}");
	}
	if(hb->g9p_present) {
		off = append_raw(dst, dst_size, off - 1, ",\"g9p_v\":1,\"g9p\":[");
		for(unsigned i = 0; i < 22; i++)
			off = append_format(dst, dst_size, off, i ? ",%u" : "%u", hb->g9p[i]);
		off = append_raw(dst, dst_size, off, "]}");
	}
	if(hb->g9_lease.enabled) {
		const struct g9_priority_lease* s = &hb->g9_lease;
		const struct g9_lease_stats* ls = &s->stats;
		unsigned flags = s->cancelled | (s->active << 1) | (s->boosted << 2) | (s->parked << 3);
		off = append_format(dst, dst_size, off - 1,
			",\"g9l_v\":2,\"g9l\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}",
			ls->acquired, ls->restored, ls->cancelled, ls->faults,
			ls->lease_pops, ls->pops_max, ls->pops_last, ls->lease_max_us,
			ls->service_max_us, ls->priority_sets, ls->sender_sets, flags);
	}
	if(hb->cpu.enabled) {
		const struct tps_cpu_evidence* c = &hb->cpu;
		off = append_format(dst, dst_size, off - 1,
			",\"cpu_clock_step_ns\":%u,\"cpu_valid\":%u,\"cpu_ticks\":%u,\"cpu_srv_us\":[",
			c->clock_step_ns, c->valid, c->ticks);
		for(unsigned i = 0; i < TPS_PHASE_COUNT; i++)
			off = append_format(dst, dst_size, off, "%s%u", i ? "," : "", c->phase_us[i]);
		off = append_raw(dst, dst_size, off, "],\"cpu_nested_us\":[");
		for(unsigned i = 0; i < TPS_PART_COUNT; i++)
			off = append_format(dst, dst_size, off, "%s%u", i ? "," : "", c->nested_us[i]);
		off = append_raw(dst, dst_size, off, "],\"cpu_waits\":[");
		for(unsigned i = 0; i < 3; i++)
			off = append_format(dst, dst_size, off, "%s%u,%u", i ? "," : "",
				c->wait_count[i], c->wait_us[i]);
		off = append_format(dst, dst_size, off,
			"],\"cpu_main_us\":%u,\"cpu_mesher_us\":%u,\"cpu_frames\":%u,"
			"\"cpu_preemptions_available\":false,\"cpu_packed_us\":%u,\"cpu_clock_mode\":%u}",
			c->main_us, c->mesher_us, c->frames, c->packed_us, c->clock_mode);
	}
	if(hb->storage_enabled) {
		off = append_format(dst, dst_size, off - 1,
			",\"item_nbt_exhaustions\":%u,\"op\":%u,\"oph\":%u,\"opc\":%u,\"opf\":%u,\"oer\":%u,\"orc\":%u,\"ocy\":%u,\"oex\":%u,\"owu\":%u,\"db\":%u,\"ds\":%u,\"dln\":%u,\"dls\":%u,\"dlp\":%u}",
			hb->item_nbt_exhaustions, hb->op, hb->oph, hb->opc, hb->opf,
			hb->oer, hb->orc, hb->ocy, hb->oex, hb->owu, hb->db, hb->ds,
			hb->dln, hb->dls, hb->dlp);
		if(hb->dlc)
			off = append_format(dst, dst_size, off - 1, ",\"dlc\":%u}", hb->dlc);
		if(hb->dlbp || hb->dlbo || hb->dlbf)
			off = append_format(dst, dst_size, off - 1,
				",\"dlbp\":%u,\"dlbo\":%u,\"dlbf\":%u}", hb->dlbp, hb->dlbo,
				hb->dlbf);
		if(hb->dlbm)
			off = append_format(dst, dst_size, off - 1,
				",\"dlbm\":%u,\"dlbs\":%u,\"dlbc\":%u,\"dlbx\":%u}", hb->dlbm,
				hb->dlbs, hb->dlbc, hb->dlbx);
		{
			uint32_t sum = 0;
			unsigned n = sizeof(hb->srv_ph) / sizeof(hb->srv_ph[0]);
			for(unsigned k = 0; k < n; k++)
				sum += hb->srv_ph[k];
			if(sum) {
				off = append_format(dst, dst_size, off - 1, ",\"srv_ph\":[");
				for(unsigned k = 0; k < n; k++)
					off = append_format(dst, dst_size, off, k ? ",%u" : "%u",
										hb->srv_ph[k]);
				off = append_format(dst, dst_size, off, "]}");
			}
		}
		if(hb->srv_lean)
			off = append_format(dst, dst_size, off - 1, ",\"srv_lean\":%u}",
								hb->srv_lean);
		{
			uint32_t sum = 0;
			unsigned n = sizeof(hb->srv_tclk) / sizeof(hb->srv_tclk[0]);
			for(unsigned k = 0; k < n; k++)
				sum += hb->srv_tclk[k];
			if(sum) {
				off = append_format(dst, dst_size, off - 1, ",\"tclk\":[");
				for(unsigned k = 0; k < n; k++)
					off = append_format(dst, dst_size, off, k ? ",%u" : "%u",
										hb->srv_tclk[k]);
				off = append_format(dst, dst_size, off, "]}");
			}
		}
		if(hb->guest)
			off = append_format(dst, dst_size, off - 1,
				",\"gx\":%.1f,\"gy\":%.2f,\"gz\":%.1f,"
				"\"guest_head_block\":%d,\"guest_air\":%d}",
				(double)hb->gx, (double)s_split_guest_y, (double)hb->gz,
				s_split_guest_head_block, s_split_guest_air);
		if(hb->split_reach)
			off = append_format(dst, dst_size, off - 1, ",\"split_reach\":%u}",
								hb->split_reach);
		if(hb->server_radius)
			off = append_format(dst, dst_size, off - 1, ",\"server_radius\":%u}",
								hb->server_radius);
	}
	/* colcap-1006 (ASTRA-CC-R2 item 5): carve contiguity with its validity
	 * flag. Emitted on every heartbeat once a probe is registered (the Wii
	 * after mem2_init), so an invalid reading there is a visible false/0;
	 * with no carve to sample (PC, tests) only a caller-set valid reading
	 * appears, and the legacy wire bytes are otherwise unchanged. */
	if(off > 0 && (hb->mem2_carve_largest_valid || s_carve_largest_probe))
		off = append_format(dst, dst_size, off - 1,
			",\"mem2_carve_largest_valid\":%s,"
			"\"mem2_carve_largest_allocatable_bytes\":%u}",
			hb->mem2_carve_largest_valid ? "true" : "false",
			hb->mem2_carve_largest_valid
				? hb->mem2_carve_largest_allocatable_bytes : 0u);
	/* O20: the vehicle driver's verdict, last, only when set. */
	if(hb && hb->vs_status && hb->vs_status[0] && off > 0) {
		off = append_raw(dst, dst_size, off - 1, ",\"vs_status\":");
		off = append_json_cstr(dst, dst_size, off, hb->vs_status);
		off = append_raw(dst, dst_size, off, "}");
	}
	return off;
}

/* Called before note() updates max_us. Keep the exact worst admitted
 * frame's component timings; these wall intervals may overlap. */
void hw_frame_evidence_note_parts(struct hw_frame_evidence* s, bool ready,
	float frame_seconds, const float parts_ms[20]) {
	if(!ready || !isfinite(frame_seconds) || frame_seconds <= 0.0f
	   || frame_seconds >= 4000.0f)
		return;
	uint32_t frame_us = (uint32_t)(frame_seconds * 1.0e6f);
	if(frame_us <= s->max_us)
		return;
	for(int i = 0; i < 20; i++) {
		if(!isfinite(parts_ms[i]) || parts_ms[i] < 0.0f || parts_ms[i] >= 4000000.0f) {
			s->invalid = 1;
			s->worst_parts_us[i] = 0;
		} else
			s->worst_parts_us[i] = (uint32_t)(parts_ms[i] * 1000.0f);
	}
}

void hw_frame_evidence_note_trace(struct hw_frame_evidence* s, bool ready, float seconds) {
	if(!ready) return;
	if(!isfinite(seconds) || seconds < 0.0F || seconds >= 4000.0F) {
		s->invalid = 1;
		return;
	}
	uint32_t us = (uint32_t)ceil((double)seconds * 1000000.0);
	if(us > s->sd_trace_max_us) s->sd_trace_max_us = us;
	s->sd_trace_over_30ms += us > 30000;
}

void hw_frame_evidence_note_wait(struct hw_frame_evidence* s, bool ready,
	uint32_t vi_ready_us, uint32_t gpu_poll_late_us) {
	if(!ready) return;
	if(vi_ready_us > s->vi_ready_max_us) s->vi_ready_max_us = vi_ready_us;
	if(gpu_poll_late_us > s->gpu_poll_late_max_us)
		s->gpu_poll_late_max_us = gpu_poll_late_us;
	s->vi_ready_over_8000 += vi_ready_us > 8000;
	s->gpu_poll_late_over_8000 += gpu_poll_late_us > 8000;
}

bool hw_frame_settle_admit(struct hw_frame_settle* s, bool playable, float dt_s) {
	if(!playable) {
		s->playable_s = 0.0f;
		return false;
	}
	/* A frame is admitted only if the window had already elapsed when it
	 * started; the frame that crosses the boundary still belongs to the
	 * settle. Once settled, stay admitted whatever this frame's dt is. */
	bool settled = s->playable_s >= HW_FRAME_EVIDENCE_SETTLE_S;
	if(!settled && isfinite(dt_s) && dt_s > 0.0f)
		s->playable_s += dt_s;
	return settled;
}

void hw_frame_evidence_note(struct hw_frame_evidence* s, bool ready,
	float frame_seconds, float gpu_seconds) {
	if(!ready)
		return;
	/* Invalid evidence fails closed at the collector. A long frame must
	 * never disappear through a narrowing cast or gameplay's dt clamp. */
	if(!isfinite(frame_seconds) || frame_seconds <= 0.0F
	   || frame_seconds >= 4000.0F || !isfinite(gpu_seconds)
	   || gpu_seconds < 0.0F || s->frames == UINT32_MAX) {
		s->invalid = 1;
		return;
	}
	uint32_t us = (uint32_t)ceil((double)frame_seconds * 1000000.0);
	s->frames++;
	s->duration_us += us;
	if(us > s->max_us)
		s->max_us = us;
	s->over_30ms += us > 30000;
	s->gpu_over_16600 += gpu_seconds > 0.0166F;
	s->gpu_over_8000 += gpu_seconds > 0.008F;
}

size_t hw_telemetry_format_overlay_spill(char* dst, size_t dst_size,
	const struct hw_overlay_spill* s) {
	if(!dst || !dst_size || !s) return 0;
	int n = snprintf(dst, dst_size,
		"{\"event\":\"overlay_spill\",\"v\":1,\"tick\":%u,\"fallback_live\":%u,"
		"\"fallback_total\":%u,\"spill\":%u,\"mem2_last_spill_bytes\":%u}",
		s->tick, s->fallback_live, s->fallback_total, s->spill,
		s->last_spill_bytes);
	if(n <= 0 || (size_t)n >= dst_size) {
		dst[0] = '\0';
		return 0;
	}
	return (size_t)n;
}

bool hw_telemetry_overlay_spill_due(const struct hw_overlay_spill* cur,
	const struct hw_overlay_spill* sent) {
	if(!cur || !sent) return false;
	return cur->fallback_live != sent->fallback_live
		|| cur->fallback_total != sent->fallback_total
		|| cur->spill != sent->spill
		|| cur->last_spill_bytes != sent->last_spill_bytes;
}

size_t hw_telemetry_format_profile_done(char* dst, size_t dst_size,
										const char* run_id,
										const char* profile,
										const char* status) {
	if(dst && dst_size)
		dst[0] = '\0';
	size_t off = 0;
	off = append_raw(dst, dst_size, off,
					 "{\"event\":\"profile_done\",\"run_id\":");
	off = append_json_cstr(dst, dst_size, off, run_id);
	off = append_raw(dst, dst_size, off, ",\"profile\":");
	off = append_json_cstr(dst, dst_size, off, profile);
	off = append_raw(dst, dst_size, off, ",\"status\":");
	off = append_json_cstr(dst, dst_size, off, status ? status : "ok");
	return append_raw(dst, dst_size, off, "}");
}

size_t hw_telemetry_format_dump_chunk(char* dst, size_t dst_size,
									  const char* run_id, const char* profile,
									  const char* event, const char* cause,
									  uint32_t dump_id, uint32_t seq, bool last,
									  const char* text, size_t text_len) {
	if(dst && dst_size)
		dst[0] = '\0';
	size_t off = 0;
	char buf[160];
	off = append_raw(dst, dst_size, off, "{\"event\":");
	off = append_json_cstr(dst, dst_size, off, event ? event : "watchdog_dump");
	off = append_raw(dst, dst_size, off, ",\"run_id\":");
	off = append_json_cstr(dst, dst_size, off, run_id);
	off = append_raw(dst, dst_size, off, ",\"profile\":");
	off = append_json_cstr(dst, dst_size, off, profile);
	off = append_raw(dst, dst_size, off, ",\"cause\":");
	off = append_json_cstr(dst, dst_size, off, cause);
	snprintf(buf, sizeof(buf), ",\"dump_id\":%u,\"seq\":%u,\"last\":%s,\"text\":",
			 dump_id, seq, last ? "true" : "false");
	off = append_raw(dst, dst_size, off, buf);
	off = append_json_string(dst, dst_size, off, text ? text : "", text_len);
	return append_raw(dst, dst_size, off, "}");
}

#ifdef PLATFORM_WII
/* DIAGNOSTIC PROBE (one-run, revert before fix commit): durable per-line tracer
 * to localize the emit_boot first-send hardware crash. Gated to Wii + TEL_LOAD(s_enabled)
 * (telemetry only inits on a hw-run), so normal play and the PC build/unit test
 * never reach it. Same fopen+fflush+fclose discipline as main.c hw_boot_trace_write. */
static void cw_tel_trace(const char* label) {
	/* Bound total probe lines: a NON-crashing run must not spam the SD ~360x/min
	 * or perturb later-send timing; the first send's crash window is ~8 lines, so
	 * 24 captures the first ~3 sends and then goes silent. */
	static unsigned budget = 24;
	if(!hw_telemetry_sd_probes_enabled() || s_tel_trace_suppress
	   || budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(!f)
		return;
	fprintf(f, "phase=%s\n", label ? label : "");
	fflush(f);
	fclose(f);
}

/* PROBE (S5.3 hunt continuation, temporary): cw_tel_trace's bare "phase=X"
 * labels don't carry the VALUES this hunt needs (heartbeat attempt number,
 * client fd, accept/send outcome, quiesce timing) to pin down WHY telemetry
 * goes dark after heartbeat 1 while the main loop itself keeps running (no
 * watchdog HANG has ever fired across 4 mp_connect_smoke runs -- that
 * thread is independent of main and fires unconditionally past a 3s stall,
 * so its silence already argues the loop is not truly stuck). Own budget so
 * it never competes with cw_tel_trace's boot-line budget. */
static void cw_tel_trace_hb(const char* label, long a, long b, long c) {
	static unsigned budget = 80;
	if(!hw_telemetry_sd_probes_enabled() || s_tel_trace_suppress
	   || budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(!f)
		return;
	fprintf(f, "hb_phase=%s a=%ld b=%ld c=%ld\n", label ? label : "", a, b, c);
	fflush(f);
	fclose(f);
}
#else
#define cw_tel_trace(label) ((void)0)
#define cw_tel_trace_hb(label, a, b, c) ((void)0)
#endif

static bool send_line_transport(const char* line, enum telemetry_send_mode mode) {
	if(!TEL_LOAD(s_enabled) || TEL_LOAD(s_quiesced))
		return false;
	bool emit_mailbox_after_unlock = false;
	bool sent = false;
#ifdef PLATFORM_WII
	s_tel_trace_suppress = (mode == TELEMETRY_SEND_WATCHDOG);
#endif
	cw_tel_trace("send_before_lock");
	if(!send_lock_enter(mode)) {
		cw_tel_trace("send_lock_denied");
		return false;
	}
	cw_tel_trace("send_after_lock");
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(s_test_sink) {
		sent = s_test_sink(line, s_test_sink_user);
		send_lock_leave();
		return sent;
	}
#endif
#ifdef PLATFORM_WII
	cw_tel_trace("send_before_lock_check");
	// PROBE (S5.3 hunt continuation): attempt counter for NORMAL-mode sends
	// specifically -- this is THE number that discriminates "the main loop
	// stopped calling emit_hw_heartbeat again" (count stays 1 forever, a
	// logic-gate bug) from "it keeps calling, but delivery dies" (count
	// climbs past 1 while the collector still sees only heartbeat #1).
	static unsigned s_hb_attempts;
	if(mode == TELEMETRY_SEND_NORMAL) {
		s_hb_attempts++;
		cw_tel_trace_hb("hb_attempt", (long)s_hb_attempts, (long)s_client_sock,
					   (long)TEL_LOAD(s_accept_armed));
	}
	if(s_pull) {
		bool accepted_new_client = false;
		if(TEL_LOAD(s_accept_armed) && mode == TELEMETRY_SEND_NORMAL
		   && s_client_sock < 0 && s_listen_sock >= 0) {
			// ROUND 13 CYCLE 4 (S5.3 hunt, Codex's third review; this specific
			// fix is a real bug independent of the hunt, kept as production
			// code): net_accept hands its
			// output sockaddr straight to a synchronous IOS_Ioctl as an
			// OUTPUT buffer. IOS invalidates the dcache range it DMAs into,
			// and documents that ioctl buffers should be 32-byte aligned
			// with lengths a multiple of 32 -- an ordinary stack
			// `struct sockaddr_in` only guarantees ITS OWN alignment/size,
			// not that nothing else shares its 32-byte cache line. A
			// neighboring stack local sharing that line would get silently
			// clobbered by the invalidate (a genuine shared-cache-line
			// hazard, not a hunt probe). Give the ioctl its OWN dedicated,
			// 32-byte-aligned, 32-byte-sized buffer -- nothing else fits on
			// that line -- and copy out into normal use only after
			// net_accept returns.
			uint8_t accept_buf[32] __attribute__((aligned(32)));
			memset(accept_buf, 0, sizeof(accept_buf));
			socklen_t len = sizeof(struct sockaddr_in);
			cw_tel_trace("send_before_accept");
			/* STAGE: net_accept is a synchronous IOS ioctl like the rest --
			 * a boot-time park in here was the original emit_boot death, so
			 * it stays separately nameable. */
			uint64_t accept_at = sender_now_us();
			uint32_t accept_stage = sender_stage_enter(HW_TEL_STAGE_ACCEPT);
			s32 accepted = net_accept(
				s_listen_sock, (struct sockaddr*)accept_buf, &len);
			sender_stage_leave(accept_stage, accept_at, NULL);
			struct sockaddr_in client;
			memcpy(&client, accept_buf, sizeof(client));
			(void)client; // not read further downstream; copied for hygiene
						  // per the fix's own prescription, not because
						  // anything currently consumes it
			cw_tel_trace("send_after_accept");
			if(mode == TELEMETRY_SEND_NORMAL)
				cw_tel_trace_hb("accept_ret", (long)accepted, 0, 0);
			if(accepted >= 0) {
				/* The accepted socket's blocking mode IS the experiment
				 * (--cw-tel-nonblock). Either way the flag is VERIFIED (an
				 * F_GETFL read-back, 2026-09-08 fix round -- see
				 * cw_net_set_nonblock) before the socket is published: an
				 * unverified flag is how the original silently-blocking
				 * listener parked the main thread forever, and an unverified
				 * NON-blocking flag would be just as bad in reverse -- the
				 * abandon-on-EAGAIN path below assumes sends cannot wait.
				 *
				 * When the experiment is armed but the read-back proves the
				 * bit did NOT latch, fall back to a genuinely blocking
				 * socket WITH its poll gate kept (s_client_is_nonblock
				 * drives the gate below, not s_nonblock_mode) rather than
				 * publish an unverified non-blocking one -- and count the
				 * fallback in tel_abd (s_sender_abandoned): the experiment
				 * gave up on this connection before a single line. */
				bool client_nonblock = false;
				bool flagged;
				if(s_nonblock_mode) {
					client_nonblock = cw_net_set_nonblock(accepted);
					if(client_nonblock) {
						flagged = true;
					} else {
						__atomic_fetch_add(&s_sender_abandoned, 1,
							__ATOMIC_RELAXED);
						flagged = cw_net_set_blocking(accepted);
					}
				} else {
					flagged = cw_net_set_blocking(accepted);
				}
				if(flagged) {
					__atomic_store_n(&s_client_sock, accepted, __ATOMIC_SEQ_CST);
					s_client_is_nonblock = client_nonblock;
					accepted_new_client = true;
				} else {
					net_close(accepted);
				}
			}
		}
		if(mode == TELEMETRY_SEND_NORMAL && accepted_new_client
		   && !s_mailbox_sent)
			emit_mailbox_after_unlock = true;
		if(s_client_sock >= 0) {
			struct tcp_send_ctx ctx = {.sock = s_client_sock};
			// First client: replay the pre-connect buffer (boot event first)
			// before any live line, so the collector's stamp gate sees the
			// boot line. On failure, skip the live line and honor the failed
			// buffered line's classification (including prefix quiescence).
			bool stream_ok = true;
			bool close_socket = false;
			/* Bounded per-line send budget: the sender may wait at most
			 * HW_TELEMETRY_SEND_DEADLINE_MS for this socket, then abandons
			 * the line (never the stream). See struct send_gate. */
			/* Under the non-blocking experiment the writability POLL is
			 * dropped (`ready` NULL) while the deadline CLOCK is kept: a
			 * non-blocking send cannot wait, so polling first would buy no
			 * bound at all and would cost one extra IOS ioctl per line --
			 * and IOS IPC contention is the very thing the experiment is
			 * trying to isolate. The between-chunk deadline still applies,
			 * so a line whose individual chunks are slow is still abandoned
			 * rather than run to completion.
			 *
			 * Keyed off s_client_is_nonblock, NOT s_nonblock_mode (2026-09-08
			 * fix round): a verify-mismatch fallback accepts THIS client as
			 * blocking even with the experiment armed, and a blocking socket
			 * with no poll gate is exactly the unbounded park the gate exists
			 * to prevent. */
			struct send_gate gate = {
				.ready = s_client_is_nonblock ? NULL : tcp_send_ready,
				.now_ms = tcp_send_now_ms, .user = &ctx,
				.budget_ms = HW_TELEMETRY_SEND_DEADLINE_MS};
			if(accepted_new_client)
				stream_ok = preconnect_flush(tcp_send_bytes, &ctx, NULL,
					&s_send_fail, &close_socket, &gate);
			sent = stream_ok && (!line || send_line_with(line, tcp_send_bytes, &ctx,
				&s_send_fail, &close_socket, &gate));
			if(mode == TELEMETRY_SEND_NORMAL)
				cw_tel_trace_hb("send_outcome", (long)stream_ok, (long)sent,
							   (long)s_client_sock);
			if(!sent) {
				// send_line_with already counted the failed line (including a
				// pre-connect failure) exactly once and classified its error.
				if(mode == TELEMETRY_SEND_NORMAL)
					cw_tel_trace_hb("send_fail_classified", (long)s_send_fail.last_errno,
								   (long)close_socket,
								   (long)s_send_fail.consec_fail);
				if(s_send_fail.quiesced && !TEL_LOAD(s_quiesced))
					fprintf(stderr, "telemetry transport stopped: rc=%d; "
						"game continues; first failure in sd:/cw_tel_trace.txt\n",
						s_send_fail.last_errno);
				/* Preserve the descriptor for alive probes. On stop, the worker
				 * shuts it down before this sender closes it. */
				// Transient failure with an incomplete prefix quiesces without
				// closing. A negative result cannot prove that zero bytes queued.
				emit_mailbox_after_unlock = false;
				if(s_mailbox_sending && s_send_fail.quiesced)
					s_mailbox_interrupted = true;
				if(s_send_fail.quiesced)
					TEL_STORE(s_quiesced, true);
			}
		} else {
			if(mode == TELEMETRY_SEND_NORMAL)
				cw_tel_trace_hb("no_client_buffered", (long)s_hb_attempts, 0,
							   0);
			// No client yet: buffer instead of dropping (boot event, early
			// heartbeats, pre-connect fault dumps).
			//
			// NOTE (2026-09-07 fix round, brief finding #3): a preconnect-ring
			// EVICTION here increments the SAME s_sender_ring.drops counter
			// that hw_telemetry_observer_drop_count() reports for admission-
			// FIFO drops (a full/contended observer ring). Documented, not
			// separated: both are "a diagnostic record never reached the
			// wire, no socket was involved" -- the one property carve_census's
			// new max_tel_drop and the collector actually need -- so folding
			// them costs nothing a caller currently reads. Splitting them
			// would need a second atomic counter threaded through
			// carve_observer.c's tel_drop field and every collector
			// consumer; revisit only if a future run needs to tell "no
			// client yet" apart from "client connected but its ring is full".
			unsigned evicted = preconnect_store(line);
			if(evicted)
				__atomic_fetch_add(&s_sender_ring.drops, evicted, __ATOMIC_RELAXED);
		}
	} else if(s_sock >= 0 && line) {
		size_t len = strlen(line);
		s32 sendto_r = net_sendto(s_sock, line, (s32)len, 0,
								  (struct sockaddr*)&s_dest, sizeof(s_dest));
		sent = sendto_r == (s32)len;
        if(!sent) {
            trace_stop(CW_TEL_UDP, line, sendto_r > 0 ? sendto_r : 0,
                0, sendto_r < 0 ? sendto_r : -EIO, sendto_r);
            hw_telemetry_send_fail_classify(&s_send_fail, -EIO, NULL);
            TEL_STORE(s_quiesced, true);
        }
	}
#else
	(void)line;
#endif
	cw_tel_trace("send_before_unlock");
	send_lock_leave();
	cw_tel_trace("send_after_unlock");
	if(emit_mailbox_after_unlock)
		hw_telemetry_emit_crash_mailbox();
#ifdef PLATFORM_WII
	// Accept polling can flush/fail preconnect records without a FIFO item.
	__atomic_store_n(&s_public_fail, s_send_fail.fail_count, __ATOMIC_RELAXED);
	__atomic_store_n(&s_public_errno, s_send_fail.last_errno, __ATOMIC_RELAXED);
#endif
	return sent;
}

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
typedef bool (*sender_line_transport_fn)(const char* line, void* user);

/* The one-slot body of sender_entry: a dequeued slot is consumed exactly once,
 * irrespective of transport success. Tests inject bytes below this boundary. */
static bool sender_drain_one(sender_line_transport_fn transport, void* user) {
	const char* line = cw_tel_peek(&s_sender_ring);
	if(!line)
		return false;
#ifdef PLATFORM_WII
	uint64_t started = gettime();
#endif
	const char* guard = NULL;
	if(!strncmp(line, "{\"event\":\"heartbeat\",", 21)) {
		unsigned slot = line == s_sender_ring.hb_line ? CW_TEL_SLOTS
			: __atomic_load_n(&s_sender_ring.tail, __ATOMIC_RELAXED) % CW_TEL_SLOTS;
		guard = s_heartbeat_guard[slot];
	}
	/* Never deliver a marked heartbeat if its required context failed.
	 * Both records use the same blocking sender; no negative rc is retried. */
	unsigned records = 1;
	if(guard && guard[0]) {
		if(transport(guard, user)) {
			(void)transport(line, user);
			records++;
		}
	} else {
		(void)transport(line, user);
	}
#ifdef PLATFORM_WII
	/* Wii-only: the PC test build has no IOS transport worth timing, so
  * tel_smax_us stays 0 there rather than reporting a meaningless number. */
	uint32_t us = (uint32_t)ticks_to_microsecs(gettime() - started);
	if(us > __atomic_load_n(&s_sender_send_max_us, __ATOMIC_RELAXED))
		__atomic_store_n(&s_sender_send_max_us, us, __ATOMIC_RELAXED);
#endif
	__atomic_fetch_add(&s_sender_sends, records, __ATOMIC_RELAXED);
	cw_tel_pop(&s_sender_ring);
	return true;
}
#endif

static bool send_line_lane(const char* line, enum telemetry_send_mode mode,
						   bool priority, const char* guard) {
#ifdef PLATFORM_WII
	/* Fault/watchdog output stays in the existing SD crash mailbox. Never
  * enter IOS or a producer critical section from an exception handler. */
	if(mode == TELEMETRY_SEND_WATCHDOG) return false;
	// Join frees the sender's LWP control block. Reject stopped calls before
	// comparing its retained handle, which a newly created thread may reuse.
	if(TEL_LOAD(s_stop.requested)) return false;
	if(TEL_LOAD(s_sender_started) && LWP_GetSelf() == s_sender_thread)
		return (!guard || send_line_transport(guard, mode))
			&& send_line_transport(line, mode); /* sender-owned replay/pair */
	if(!TEL_LOAD(s_enabled) || TEL_LOAD(s_quiesced) || TEL_LOAD(s_stop.requested) || TEL_LOAD(s_quiesce_requested))
		return false;
	bool published = priority ? publish_heartbeat_pair(line, guard)
		: cw_tel_publish(&s_sender_ring, line);
	/* Wake on a DROP too, not only on a successful admission: a refused
	 * publish means the ring was full, which is precisely the state in which
	 * the sender most needs to be running. */
	sender_wake();
	return published;
#else
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(s_test_async_sender) {
		bool published = priority ? publish_heartbeat_pair(line, guard)
			: cw_tel_publish(&s_sender_ring, line);
		sender_wake();
		return published;
	}
#endif
	(void)priority;
	if(guard && !send_line_transport(guard, mode))
		return false;
	return send_line_transport(line, mode);
#endif
}

static bool send_line_mode(const char* line, enum telemetry_send_mode mode) {
	return send_line_lane(line, mode, false, NULL);
}

/* The heartbeat -- and ONLY the heartbeat -- takes the ring's priority lane.
 * See cw_tel_publish_hb: the collector's liveness rule is "a heartbeat within
 * 5 s", and FIFO order alone could not honour that once the sender's drain
 * rate fell below the observer offer rate. */
static bool send_heartbeat_line(const char* line, const char* guard) {
	return send_line_lane(line, TELEMETRY_SEND_NORMAL, true, guard);
}

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
/* Drop attribution (diagnostic): which observer events a full/contended ring
 * refused, by event name, so a tel_drop can be traced to its producer. Main
 * thread and other producers may race; counts are best-effort, names exact. */
#define TEL_DROP_NAMES 8
static struct { char name[28]; uint32_t n; } s_drop_names[TEL_DROP_NAMES];
static uint32_t s_drop_names_seq;
/* Producers and the formatter share the table: a try-lock guards it (a busy
 * table skips attribution for that one refusal; the drop itself is still
 * counted by the ring). Never blocks a producer. */
static uint32_t s_drop_names_lock;
static bool drop_names_try_lock(void) {
	uint32_t expected = 0;
	return __atomic_compare_exchange_n(&s_drop_names_lock, &expected, 1, false,
		__ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void drop_names_unlock(void) {
	__atomic_store_n(&s_drop_names_lock, 0, __ATOMIC_RELEASE);
}
static void tel_drop_names_reset(void) {
	if(!drop_names_try_lock()) return;
	memset(s_drop_names, 0, sizeof s_drop_names);
	__atomic_store_n(&s_drop_names_seq, 0, __ATOMIC_RELAXED);
	drop_names_unlock();
}
static void tel_drop_name_note(const char* line) {
	const char* e = line ? strstr(line, "\"event\":\"") : NULL;
	char name[28] = "?";
	if(e) {
		e += 9;
		size_t k = 0;
		while(e[k] && e[k] != '"' && k + 1 < sizeof name) { name[k] = e[k]; k++; }
		name[k] = 0;
	}
	if(!drop_names_try_lock()) return;
	for(unsigned i = 0; i < TEL_DROP_NAMES; i++) {
		if(!s_drop_names[i].n) {
			snprintf(s_drop_names[i].name, sizeof s_drop_names[i].name, "%s", name);
			s_drop_names[i].n = 1;
			__atomic_fetch_add(&s_drop_names_seq, 1, __ATOMIC_RELAXED);
			drop_names_unlock();
			return;
		}
		if(!strcmp(s_drop_names[i].name, name)) {
			s_drop_names[i].n++;
			__atomic_fetch_add(&s_drop_names_seq, 1, __ATOMIC_RELAXED);
			drop_names_unlock();
			return;
		}
	}
	drop_names_unlock();
}
bool hw_telemetry_drop_names_line(char* out, size_t cap, uint32_t* seen_seq) {
	uint32_t seq = __atomic_load_n(&s_drop_names_seq, __ATOMIC_RELAXED);
	if(!out || !cap || !seen_seq || seq == *seen_seq) return false;
	if(!drop_names_try_lock()) return false; /* retried next heartbeat */
	size_t off = (size_t)snprintf(out, cap, "{\"event\":\"tel_drop_names\",\"names\":{");
	for(unsigned i = 0; i < TEL_DROP_NAMES && s_drop_names[i].n && off < cap; i++)
		off += (size_t)snprintf(out + off, cap - off, "%s\"%s\":%u", i ? "," : "",
			s_drop_names[i].name, (unsigned)s_drop_names[i].n);
	drop_names_unlock();
	if(off + 3 >= cap) return false;
	snprintf(out + off, cap - off, "}}");
	*seen_seq = seq;
	return true;
}
#else
bool hw_telemetry_drop_names_line(char* out, size_t cap, uint32_t* seen_seq) {
	(void)out; (void)cap; (void)seen_seq;
	return false;
}
#endif

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
static bool send_observer_line(const char* line, bool low_priority) {
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(!s_test_async_sender)
		return send_line(line);
#endif
	if(!TEL_LOAD(s_enabled) || hw_telemetry_quiesced())
		return false;
	/* EVERY observer record is low priority (2026-09-07 starvation fix). The
  * previous rule marked only carve_attrib low-priority, so carve_tenant,
  * carve_census, settle and tps records still went through the
  * full-priority publish and could occupy all four slots -- the heartbeat's
  * "reserved" slot did not exist whenever the carve census set was in
  * flight. An observer record is a diagnostic sample by definition; losing
  * one costs a sample, losing the heartbeat costs the run. */
	(void)low_priority;
	bool published = cw_tel_publish_observer(&s_sender_ring, line);
	if(!published)
		tel_drop_name_note(line);
	sender_wake();
	return published;
}
#endif

#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
/* O22: see cw_tel_try_publish_observer. */
static bool send_observer_line_retry(const char* line) {
#ifdef CAVEWII_HW_TELEMETRY_TEST
	if(!s_test_async_sender)
		return send_line(line);
#endif
	if(!TEL_LOAD(s_enabled) || hw_telemetry_quiesced())
		return false;
	bool published = cw_tel_try_publish_observer(&s_sender_ring, line);
	sender_wake();
	return published;
}
#endif

#ifdef PLATFORM_WII
static bool sender_wii_transport(const char* line, void* unused) {
	(void)unused;
	return send_line_transport(line, TELEMETRY_SEND_NORMAL);
}

static void stop_cancel(void* unused) {
	(void)unused;
	// net_shutdown does not release/recycle the descriptor (unlike close).
	// network_wii.c:728-740 uses a separate aligned ioctl buffer and no
	// send mutex. Keep its synchronous IOS wait off main as well.
	s32 sock = __atomic_load_n(&s_client_sock, __ATOMIC_SEQ_CST);
	if(sock >= 0) (void)net_shutdown(sock, SHUT_RDWR);
	if(s_sock >= 0) (void)net_shutdown(s_sock, SHUT_RDWR);
}

static void stop_join(void* unused) {
	(void)unused;
	/* Invalidate the lease before Join can free the native sender handle. */
	thread_g9_lease_cancel();
	(void)LWP_JoinThread(s_sender_thread, NULL);
}

static void* stop_entry(void* unused) {
	(void)unused;
	while(!TEL_LOAD(s_sender_started) || !TEL_LOAD(s_stop.requested))
		usleep(10000);
	cw_tel_stop_run(&s_stop, stop_cancel, stop_join, NULL);
	return NULL; /* own handle retained until reset */
}

static uint64_t stop_now_ms(void* unused) {
	(void)unused;
	return ticks_to_millisecs(gettime());
}

static void stop_sleep_ms(void* unused) {
	(void)unused;
	/* This is the poll callback cw_tel_stop_orderly uses while it waits for
	 * the drain and then for the join -- including the pass right after it
	 * sets s_stop.requested. Waking the sender here is what keeps shutdown
	 * as prompt as it was with the 1 ms poll: the sender re-checks
	 * s_stop.requested within ~1 ms of it being set instead of waiting out
	 * its 10 ms park. */
	sender_wake();
	usleep(1000);
}

static bool stop_drained(void* unused) {
	(void)unused;
	/* Sender publishes only after the FIFO and preconnect records finish,
	 * or after a terminal transport failure makes delivery impossible. */
	return TEL_LOAD(s_quiesced);
}

static void* sender_entry(void* unused) {
	(void)unused;
	bool g9_labeled = false;
	uint64_t alive_at = 0;
	while(!TEL_LOAD(s_sender_started)) usleep(1000);
	for(;;) {
		if(!g9_labeled && g9_probe_enabled()) {
			g9_probe_role(4);
			g9_labeled = true;
		}
		bool g9_pumped = g9_probe_pump(hw_telemetry_emit_observer_retry);
		/* Retained rows defer clean refusal, including no pull client. The
		 * observer's final deadline releases this fence on terminal failure
		 * without inventing final coverage or replaying ambiguous TCP. */
		(void)service_observer_pump(hw_telemetry_emit_service_observer);
		__atomic_fetch_add(&s_sender_iters, 1, __ATOMIC_RELAXED);
		const char* line = cw_tel_peek(&s_sender_ring);
		if(line && !TEL_LOAD(s_stop.requested) && !TEL_LOAD(s_quiesced)
		   && (g9_pumped || g9_capture_pending() || service_observer_hold_transport())
		   && !strncmp(line, "{\"event\":\"profile_done\",", 24)) {
			if(!g9_pumped) usleep(1000); /* let the active tick seal; retry failed TCP */
			continue;
		} /* seal only after retained probe records reach the wire */
		/* The collector seals on profile_done. Drain the frozen observer
		 * request first, even when main has already requested quiescence. */
		if(line && settle_observer_active()
		   && !strncmp(line, "{\"event\":\"profile_done\",", 24)
		   && !TEL_LOAD(s_quiesced) && !TEL_LOAD(s_stop.requested))
			(void)settle_observer_pump(hw_telemetry_emit_observer);
		if(line && !TEL_LOAD(s_quiesced) && !TEL_LOAD(s_stop.requested)) {
			(void)sender_drain_one(sender_wii_transport, NULL);
			/* BACK-PRESSURE: the drain loop otherwise spins with zero pacing,
			 * so a producer burst (heartbeat + settle + carve census in one
			 * frame) is handed to IOS as fast as net_send returns and fills
			 * the lwIP send buffer. After a transient failure, give it time
			 * to drain instead of immediately re-offering the next line --
			 * escalating 2/8/32ms per consecutive strike (see
			 * transient_pace_us's doc comment for the resulting 42ms bound)
			 * rather than a flat 2ms that only bought ~4ms of tolerance. */
			if(s_send_fail.consec_fail) {
				/* STAGE: a deliberate pacing sleep is NOT a wedge, and must
				 * not be misread as one -- name it so a heartbeat composed
				 * during the 42 ms ladder says RING_WAIT rather than the
				 * SEND it just left. */
				sender_stage_set(HW_TEL_STAGE_RING_WAIT);
				usleep(transient_pace_us(s_send_fail.consec_fail));
			}
			sender_stage_set(HW_TEL_STAGE_OTHER);
			continue;
		}
		if(!line && !TEL_LOAD(s_quiesced) && !TEL_LOAD(s_stop.requested)
		   && settle_observer_pump(hw_telemetry_emit_observer))
			continue;
		if(!line && !TEL_LOAD(s_quiesced) && s_pull && s_client_sock < 0 && TEL_LOAD(s_accept_armed)
					&& !TEL_LOAD(s_stop.requested))
			(void)send_line_transport(NULL, TELEMETRY_SEND_NORMAL);
		if(TEL_LOAD(s_quiesce_requested) && !TEL_LOAD(s_quiesced)
					&& cw_tel_drained(&s_sender_ring) && (!s_pull || s_client_sock >= 0)
					&& !service_observer_hold_transport()) {
			trace_stop(CW_TEL_QUIESCE, NULL, 0, 0, 0, 0);
			TEL_STORE(s_quiesced, true); /* all admitted records, including done, drained */
		}
		if(TEL_LOAD(s_stop.requested)) {
			service_observer_transport_abort();
			trace_stop(CW_TEL_SHUTDOWN, NULL, 0, 0, 0, 0);
			TEL_STORE(s_quiesced, true);
			// Stop worker must finish shutdown before close can recycle the fd.
			while(!TEL_LOAD(s_stop.cancelled)) usleep(1000);
			/* STAGE: net_close is IOS IPC too, and a shutdown that parks in
			 * it is the documented 2026-08-18 trailing-heartbeat wedge. */
			sender_stage_set(HW_TEL_STAGE_CLOSE);
			if(s_client_sock >= 0) net_close(s_client_sock);
			if(s_listen_sock >= 0) net_close(s_listen_sock);
			if(s_sock >= 0) net_close(s_sock);
			return NULL; /* stop worker joins; main only waits to its deadline */
		}
		uint64_t now = gettime();
		if(TEL_LOAD(s_quiesced) && s_client_sock >= 0 && now-alive_at >= (uint64_t)TB_TIMER_CLOCK*1000) {
			/* Exactly 64 bytes, leading LF abandons any ambiguous old prefix.
    * Each probe is new evidence, never a retry of the failed record. */
			char alive[64];
			cw_tel_alive_format(alive,
				__atomic_load_n(&s_live_frame, __ATOMIC_RELAXED),
				__atomic_load_n(&s_live_tick, __ATOMIC_RELAXED));
			uint64_t alive_started = sender_now_us();
			uint32_t alive_stage = sender_stage_enter(HW_TEL_STAGE_SEND);
			(void)net_send(s_client_sock, alive, sizeof(alive), 0);
			sender_stage_leave(alive_stage, alive_started,
							   &s_sender_syscall_max_us);
			alive_at = now;
		}
		/* STAGE: the loop has nothing to do. A heartbeat composed here
		 * SHOULD read IDLE -- an IDLE stage with a large tel_stg_us is the
		 * signature of a sender that is alive but starved of records, which
		 * is a completely different fault from one parked in a syscall.
		 *
		 * IDLE remains the stage across the whole park: the wait IS the idle
		 * state, so an IDLE stage with a multi-second tel_stg_us still means
		 * exactly what it meant with the 1 ms sleep -- alive but not being
		 * scheduled. What changes is tel_it, which now counts REAL work
		 * passes plus at most 100 timeout passes per second instead of ~1000
		 * timer passes; see the tel_it comment on the heartbeat format. */
		sender_stage_set(HW_TEL_STAGE_IDLE);
		(void)cw_tel_wake_wait(&s_sender_wake,
							   HW_TELEMETRY_SENDER_IDLE_WAIT_MS);
	}
}
#endif

static bool send_line(const char* line) {
	return send_line_mode(line, TELEMETRY_SEND_NORMAL);
}

void hw_telemetry_shutdown(void) {
	thread_g9_lease_cancel();
	/* Main-thread lifecycle hook: the final summary/seal must be admitted
	 * before closing the FIFO, exactly like the final heartbeat. */
	tps_observer_close(false);
#ifdef PLATFORM_WII
	if(TEL_LOAD(s_sender_started)) {
		cw_tel_close(&s_sender_ring);
		TEL_STORE(s_quiesce_requested, true);
		sender_wake(); /* evaluate quiescence now, not up to 10 ms from now */
		(void)cw_tel_stop_orderly(&s_stop, stop_drained,
			stop_now_ms, stop_sleep_ms, NULL);
		/* Deadline expiry retains live state. Initialization stays one-shot. */
		return;
	}
#endif
	TEL_STORE(s_enabled, false);
#ifdef PLATFORM_WII
	if(s_client_sock >= 0) {
		net_close(s_client_sock);
		s_client_sock = -1;
	}
	if(s_listen_sock >= 0) {
		net_close(s_listen_sock);
		s_listen_sock = -1;
	}
	if(s_sock >= 0) {
		net_close(s_sock);
		s_sock = -1;
	}
	if(s_send_mutex_ready) {
		LWP_MutexDestroy(s_send_mutex);
		s_send_mutex = LWP_MUTEX_NULL;
		s_send_mutex_ready = false;
	}
	TEL_STORE(s_accept_armed, false);
	s_pull = false;
	hw_telemetry_send_fail_state_reset(&s_send_fail);
#endif
	s_run_id[0] = '\0';
	s_profile[0] = '\0';
	s_local_ip[0] = '\0';
	s_mailbox_sent = false;
	s_mailbox_sending = false;
	s_mailbox_interrupted = false;
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	preconnect_reset();
#endif
}

bool hw_telemetry_init(const struct hw_telemetry_config* cfg) {
#ifdef PLATFORM_WII
	if(TEL_LOAD(s_sender_started)) return false;
#endif
	hw_telemetry_shutdown();
	memset(s_path_samples, 0, sizeof(s_path_samples));
	memset(&s_path_send, 0, sizeof(s_path_send));
	__atomic_store_n(&s_path_closed, 0, __ATOMIC_RELAXED);
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	tel_drop_names_reset();
	memset(&s_failure_trace, 0, sizeof(s_failure_trace));
	memset(s_last_results, 0, sizeof(s_last_results));
#endif
	if(!cfg || !cfg->enabled || !cfg->host || !cfg->host[0] || cfg->port <= 0
	   || cfg->port > 65535)
		return false;
	s_suppress_sd_probes = cfg->suppress_sd_probes;
	// A fresh init is a fresh telemetry session: un-quiesce.
	TEL_STORE(s_quiesced, false);

	snprintf(s_run_id, sizeof(s_run_id), "%s",
			 cfg->run_id ? cfg->run_id : "manual");
	snprintf(s_profile, sizeof(s_profile), "%s",
			 cfg->profile ? cfg->profile : "far_idle_2m");
	s_portal_quit_started = time_get();
	s_portal_quit_server_seq = 0;
	s_portal_quit_switch_begin_ms = 0;

#ifdef PLATFORM_WII
	// NON-recursive on purpose: the watchdog/fault path enters via TryLock and
	// must FAIL when it interrupted the very thread that owns the mutex (a CPU
	// fault mid-send runs the panic handler ON the owner thread; a recursive
	// mutex would let it re-enter the half-completed send from exception
	// context and wedge inside IOS before the SD hang_report is written).
	if(!s_send_mutex_ready
	   && LWP_MutexInit(&s_send_mutex, false) != 0)
		goto fail;
	s_send_mutex_ready = true;

	s_sock = -1;
	s_listen_sock = -1;
	s_client_sock = -1;
	TEL_STORE(s_accept_armed, false);
	preconnect_reset();
	s_pull = cfg->pull || strcmp(cfg->host, "pull") == 0
			 || strcmp(cfg->host, "tcp-listen") == 0;

	char netmask[16] = {0};
	char gateway[16] = {0};
	if(if_config(s_local_ip, netmask, gateway, true,
				 HW_TELEMETRY_DHCP_MAX_RETRIES)
	   < 0)
		goto fail;
	if(cfg->netmask && cfg->netmask[0]
	   && strcmp(netmask, cfg->netmask) != 0) {
		// The old static re-config here could NEVER work: libogc's if_configex
		// hard-returns -EINVAL for use_dhcp=false (and ignores max_retries), so
		// this path unconditionally failed -- and failing init here silently
		// disabled telemetry for the whole run over a netmask mismatch the
		// transport doesn't even depend on (pull mode: the collector connects
		// IN to us; udp: routing falls back to the gateway). Keep the DHCP
		// config, note the mismatch, and continue.
		fprintf(stderr,
				"[hw-telemetry] --cw-netmask %s ignored (DHCP netmask %s; "
				"libogc if_config has no static path)\n",
				cfg->netmask, netmask);
	}

	if(s_pull) {
		s_listen_sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
		if(s_listen_sock < 0)
			goto fail;
		int yes = 1;
		net_setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes,
					   sizeof(yes));
		memset(&s_dest, 0, sizeof(s_dest));
		s_dest.sin_family = AF_INET;
		s_dest.sin_port = htons((uint16_t)cfg->port);
		s_dest.sin_addr.s_addr = INADDR_ANY;
		s32 bind_r = net_bind(s_listen_sock, (struct sockaddr*)&s_dest, sizeof(s_dest));
		if(bind_r < 0)
			goto fail;
		s32 listen_r = net_listen(s_listen_sock, 1);
		if(listen_r < 0)
			goto fail;
		if(!cw_net_set_nonblock(s_listen_sock))
			goto fail;
	} else {
		s_sock = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
		if(s_sock < 0)
			goto fail;
		memset(&s_dest, 0, sizeof(s_dest));
		s_dest.sin_family = AF_INET;
		s_dest.sin_port = htons((uint16_t)cfg->port);
		s_dest.sin_addr.s_addr = inet_addr(cfg->host);
		if(s_dest.sin_addr.s_addr == INADDR_ANY
		   || s_dest.sin_addr.s_addr == 0xFFFFFFFFu)
			goto fail;
	}
#else
	snprintf(s_local_ip, sizeof(s_local_ip), "pc");
#endif
	TEL_STORE(s_enabled, true);
#ifdef PLATFORM_WII
	memset(&s_sender_ring, 0, sizeof(s_sender_ring));
	memset(s_heartbeat_guard, 0, sizeof(s_heartbeat_guard));
	__atomic_store_n(&s_sender_write_timeouts, 0, __ATOMIC_RELAXED);
	s_gate_consec_timeouts = 0;
	s_gate_off = false;
	TEL_STORE(s_stop.requested, false); TEL_STORE(s_quiesce_requested, false);
	/* Before the sender exists, so its very first park is already signalled.
	 * A failure here is not fatal: cw_tel_wake_wait degrades to a plain
	 * 10 ms sleep, i.e. the old behaviour minus 90% of the wakeups. */
	sender_wake_init();
	if(LWP_CreateThread(&s_sender_thread, sender_entry, NULL, s_sender_stack,
				sizeof(s_sender_stack), HW_TELEMETRY_SENDER_PRIORITY) != 0) goto fail;
	s32 stop_start = LWP_CreateThread(&s_stop_thread, stop_entry, NULL,
		s_stop_stack, sizeof(s_stop_stack), 40);
	if(stop_start != 0) {
		trace_stop(CW_TEL_INIT, NULL, 0, 0, stop_start, stop_start);
		// Sender is still behind its start gate: no socket call can be active.
		// Let it close resources and exit; retain its handle until reset.
		TEL_STORE(s_enabled, false);
		__atomic_store_n(&s_stop.requested, true, __ATOMIC_SEQ_CST);
		TEL_STORE(s_stop.cancelled, true);
		TEL_STORE(s_sender_started, true);
		return false;
	}
	TEL_STORE(s_sender_started, true);
#endif
	return true;

#ifdef PLATFORM_WII
fail:
	trace_stop(CW_TEL_INIT, NULL, 0, 0, errno, 0);
	hw_telemetry_shutdown();
	return false;
#endif
}

bool hw_telemetry_active(void) {
#ifdef PLATFORM_WII
	return TEL_LOAD(s_enabled) && !TEL_LOAD(s_stop.requested);
#else
	return TEL_LOAD(s_enabled);
#endif
}

const char* hw_telemetry_local_ip(void) { return s_local_ip; }

// See the declaration comment (hw_telemetry.h) for the design note this
// implements (Batch E item 5, 2026-08-23). Deliberately dependency-free:
// no reference to s_enabled/s_local_ip/sockets/etc -- the entire DHCP/socket
// retry policy lives in the caller-supplied `attempt`, so this function is
// exercised by tests/test_hw_telemetry.c with plain fakes.
bool hw_telemetry_retry_bringup(hw_telemetry_retry_attempt_fn attempt,
								hw_telemetry_retry_sleep_fn sleep_fn, void* user,
								uint32_t budget_ms, uint32_t sleep_ms,
								int* attempts_out) {
	int attempts = 0;
	bool ok = false;
	if(attempt) {
		uint32_t elapsed_ms = 0;
		for(;;) {
			attempts++;
			ok = attempt(user);
			if(ok)
				break;
			if(!sleep_fn || sleep_ms == 0 || elapsed_ms >= budget_ms)
				break;
			sleep_fn(sleep_ms, user);
			elapsed_ms += sleep_ms;
		}
	}
	if(attempts_out)
		*attempts_out = attempts;
	return ok;
}

// Allow the deferred first net_accept once the settled game main loop is running.
// Before this, send_line_mode (reached by emit_boot at boot) skips the accept to
// avoid the early-boot net_accept hard-fault on real hardware.
void hw_telemetry_arm_accept(void) {
#ifdef PLATFORM_WII
	TEL_STORE(s_accept_armed, true);
#endif
}

int hw_telemetry_debug_listen_flags(void) {
#ifdef PLATFORM_WII
	if(s_listen_sock < 0)
		return -1;
	int r = (int)net_fcntl(s_listen_sock, F_GETFL, 0);
	return r;
#else
	return -1;
#endif
}

#ifdef CAVEWII_HW_TELEMETRY_TEST
void hw_telemetry_test_set_sink(hw_telemetry_test_sink_fn sink, void* user) {
	s_test_sink = sink;
	s_test_sink_user = user;
}

void hw_telemetry_test_async_sender_begin(void) {
	memset(&s_sender_ring, 0, sizeof(s_sender_ring));
	memset(s_heartbeat_guard, 0, sizeof(s_heartbeat_guard));
	/* The sender self-telemetry counters live beside the ring and are just as
  * per-session: leaving them behind would let one test's drains show up in
  * the next test's window. */
	__atomic_store_n(&s_sender_iters, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s_sender_sends, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s_sender_send_max_us, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s_sender_write_timeouts, 0, __ATOMIC_RELAXED);
	s_gate_consec_timeouts = 0;
	s_gate_off = false;
	s_test_gate_ready = NULL;
	s_test_gate_now = NULL;
	s_test_gate_user = NULL;
	s_test_async_sender = true;
	/* Same one-shot construction the Wii init does before it creates the
	 * sender LWP, so the test build exercises the real signal path. */
	sender_wake_init();
}

void* hw_telemetry_test_sender_wake(void) {
	return s_sender_wake_ready ? &s_sender_wake : NULL;
}

void hw_telemetry_test_set_send_gate(int (*ready)(void*, uint32_t),
									 uint64_t (*now_ms)(void*), void* user) {
	s_test_gate_ready = ready;
	s_test_gate_now = now_ms;
	s_test_gate_user = user;
}
bool hw_telemetry_test_send_line_with_gate(const char* line,
	int (*send_fn)(const char*, size_t, void*), void* user,
	struct hw_telemetry_send_fail_state* state) {
	struct send_gate gate = {.ready = s_test_gate_ready, .now_ms = s_test_gate_now,
		.user = s_test_gate_user, .budget_ms = HW_TELEMETRY_SEND_DEADLINE_MS};
	return send_line_with(line, send_fn, user, state, NULL,
		s_test_gate_ready || s_test_gate_now ? &gate : NULL);
}

void hw_telemetry_test_set_stage_clock(uint64_t (*now_us)(void*), void* user) {
	s_test_stage_now_us = now_us;
	s_test_stage_user = user;
}

uint32_t hw_telemetry_test_sender_stage(void) {
	return __atomic_load_n(&s_sender_stage, __ATOMIC_RELAXED)
		>> CW_TEL_STAGE_SHIFT;
}

struct test_sender_transport {
	hw_telemetry_test_send_fn send_fn;
	void* user;
	bool sent;
};

static bool sender_test_transport(const char* line, void* user) {
	struct test_sender_transport* transport = user;
	/* The PC proof seam drives the SAME bounded-send path the Wii sender
	 * does; with no gate installed it behaves exactly as before. */
	struct send_gate gate = {.ready = s_test_gate_ready,
		.now_ms = s_test_gate_now, .user = s_test_gate_user,
		.budget_ms = HW_TELEMETRY_SEND_DEADLINE_MS};
	transport->sent = send_line_with(line, transport->send_fn,
		transport->user, NULL, NULL,
		s_test_gate_ready || s_test_gate_now ? &gate : NULL);
	return transport->sent;
}

unsigned hw_telemetry_test_async_sender_drain_n(
	hw_telemetry_test_send_fn send_fn, void* user, unsigned max_records) {
	unsigned sent = 0;
	while(sent < max_records) {
		struct test_sender_transport transport = {
			.send_fn = send_fn, .user = user,
		};
		if(!sender_drain_one(sender_test_transport, &transport))
			break;
		if(!transport.sent)
			break;
		sent++;
	}
	return sent;
}

unsigned hw_telemetry_test_async_sender_drain(
	hw_telemetry_test_send_fn send_fn, void* user) {
	unsigned sent = 0;
	for(;;) {
		struct test_sender_transport transport = {
			.send_fn = send_fn, .user = user,
		};
		if(!sender_drain_one(sender_test_transport, &transport))
			break;
		if(!transport.sent)
			break;
		sent++;
	}
	return sent;
}

const char* hw_telemetry_test_async_sender_peek(void) {
	return cw_tel_peek(&s_sender_ring);
}

bool hw_telemetry_test_heartbeat_off_equiv(void) {
	static struct cw_tel_ring reference;
	memset(&reference, 0, sizeof(reference));
	hw_telemetry_test_async_sender_begin();
	uint32_t seed = 0x101023u;
	for(unsigned i = 0; i < 10000; i++) {
		seed = seed * 1664525u + 1013904223u;
		char line[80];
		snprintf(line, sizeof(line), "{\"event\":\"heartbeat\",\"tick\":%u}", i);
		unsigned action = seed >> 28;
		bool a, b;
		if(action < 7) {
			a = cw_tel_publish_hb(&reference, line);
			b = publish_heartbeat_pair(line, NULL);
		} else if(action < 10) {
			a = cw_tel_publish_observer(&reference, "observer");
			b = cw_tel_publish_observer(&s_sender_ring, "observer");
		} else if(action < 12) {
			a = cw_tel_publish(&reference, "normal");
			b = cw_tel_publish(&s_sender_ring, "normal");
		} else {
			const char* old_line = cw_tel_peek(&reference);
			const char* new_line = cw_tel_peek(&s_sender_ring);
			if((old_line == NULL) != (new_line == NULL)
			   || (old_line && strcmp(old_line, new_line))) return false;
			if(old_line && action >= 14) {
				cw_tel_pop(&reference);
				cw_tel_pop(&s_sender_ring);
			}
			a = b = true;
		}
		if(a != b || memcmp(&reference, &s_sender_ring, sizeof(reference)))
			return false;
	}
	hw_telemetry_test_async_sender_end();
	return true;
}

uint32_t hw_telemetry_test_async_sender_drops(void) {
	return __atomic_load_n(&s_sender_ring.drops, __ATOMIC_RELAXED);
}

void hw_telemetry_test_async_sender_end(void) {
	s_test_async_sender = false;
}

void hw_telemetry_test_set_send_lock_busy(bool busy) {
	s_test_send_lock_busy = busy;
	s_test_send_lock_blocks = 0;
}

unsigned hw_telemetry_test_send_lock_blocks(void) {
	return s_test_send_lock_blocks;
}

void hw_telemetry_test_set_lock_wait(void (*wait_fn)(void* user), void* user) {
	s_test_lock_wait = wait_fn;
	s_test_lock_wait_user = user;
}

bool hw_telemetry_test_nonblock_flag_confirm(int32_t setfl_ret,
											 int32_t getfl_ret) {
	return cw_net_flags_confirm_nonblock(setfl_ret, getfl_ret);
}

bool hw_telemetry_test_send_line_with(const char* line,
									  hw_telemetry_test_send_fn send_fn,
									  void* user) {
	return send_line_with(line, send_fn, user, NULL, NULL, NULL);
}

bool hw_telemetry_test_send_line_with_state(const char* line,
	hw_telemetry_test_send_fn send_fn, void* user,
	struct hw_telemetry_send_fail_state* state, bool* close_socket) {
	return send_line_with(line, send_fn, user, state, close_socket, NULL);
}

const struct cw_tel_trace* hw_telemetry_test_trace(void) { return &s_failure_trace; }
void hw_telemetry_test_trace_reset(void) {
	memset(&s_failure_trace, 0, sizeof(s_failure_trace));
	memset(s_last_results, 0, sizeof(s_last_results));
}
void hw_telemetry_test_preconnect_reset(void) { preconnect_reset(); }

unsigned hw_telemetry_test_preconnect_store_n(const char* line) {
	return preconnect_store(line);
}
bool hw_telemetry_test_preconnect_store(const char* line) {
	return preconnect_store(line) != 0;
}

unsigned hw_telemetry_test_preconnect_count(void) { return s_pre_stored; }

bool hw_telemetry_test_preconnect_flush(hw_telemetry_test_send_fn send_fn,
										void* user, unsigned* sent_out) {
	return preconnect_flush(send_fn, user, sent_out, NULL, NULL, NULL);
}

// task_08716cec: see the declaration comment (hw_telemetry.h) and the
// send_line_mode PLATFORM_WII root-cause comment (this file) for the full
// story. This hook exists purely so the trigger's EFFECT (immediate,
// budget-of-1 quiesce) is provable from a PC test -- the real trigger site
// (a failed net_send against an established s_client_sock) only compiles
// under PLATFORM_WII.
void hw_telemetry_test_note_established_client_send_failure(void) {
	hw_telemetry_quiesce();
}
#endif

void hw_telemetry_emit_boot(const char* build_id) {
	char line[384];
	hw_telemetry_format_boot(line, sizeof(line), s_run_id, s_profile, build_id,
							 s_local_ip);

	send_line(line);
}

bool hw_anchor_evidence_begin(struct hw_anchor_evidence* a, bool playable,
	const uint32_t faults[3]) {
	if(!a->enabled)
		return false;
	/* Called once per heartbeat, never per render frame. All installed
	 * columns AND meshes must settle before the fixed measurement window. */
	for(unsigned p = 0; p < ((a->separation || a->guest) ? 2u : 1u); p++)
		if(a->missing[p] || a->mesh_pending[p] || a->far_missing[p]
		   || a->far_mesh_pending[p])
			playable = false;
	if(!playable)
		a->ready_streak = 0;
	else if(a->ready_streak < HW_ANCHOR_READY_HEARTBEATS)
		a->ready_streak++;
	a->ready = a->ready_streak == HW_ANCHOR_READY_HEARTBEATS;
	for(unsigned i = 0; i < 3; i++) {
		if(!a->measuring)
			a->faults_load[i] = faults[i];
		else
			a->faults_run[i] = faults[i] - a->faults_load[i];
	}
	if(a->measuring || !a->ready)
		return false;
	a->measuring = true;
	return true;
}

void hw_anchor_evidence_note(struct hw_anchor_evidence* a, uint32_t elapsed_ms,
	uint32_t frame_us, int carve_kb) {
	if(!a->enabled)
		return;
	int* minimum = a->measuring ? &a->carve_run_min : &a->carve_load_min;
	if(carve_kb >= 0 && (*minimum < 0 || carve_kb < *minimum))
		*minimum = carve_kb;
	if(!a->measuring)
		return;
	a->elapsed_ms = elapsed_ms;
	for(unsigned p = 0; p < 2; p++)
		if(a->far_missing[p] > a->far_max[p])
			a->far_max[p] = a->far_missing[p];
	/* Preserve every frame after the start boundary, even during later stalls.
	 * The collector owns the full history and percentiles, never the console. */
	if(elapsed_ms > 0) {
		if(a->frame_count == HW_ANCHOR_FRAME_CAP) {
			if(!hw_telemetry_emit_anchor_frames(a))
				a->frame_dropped += a->frame_count;
			a->frame_count = 0;
		}
		a->frame_total++;
		a->frame_us[a->frame_count++] = frame_us;
	}
}

/* Declared here so the observer helpers start at the same boundary as before
 * O22 (tests/test_anchor_measurement_clock.py slices up to this line). */
static bool emit_observer_packet(const char* line, bool watchdog_start);

static bool emit_observer_packet_mode(const char* line, bool watchdog_start,
									  bool retry) {
	if(!line) return false;
#ifdef PLATFORM_WII
	/* Only the existing sender may finish an admitted observer drain after
	 * quiesce was requested. Producers retain the original rejection gate. */
	if(TEL_LOAD(s_quiesced) || TEL_LOAD(s_stop.requested)) return false;
	if(TEL_LOAD(s_quiesce_requested)
	   && (!TEL_LOAD(s_sender_started) || LWP_GetSelf() != s_sender_thread))
		return false;
#else
	if(hw_telemetry_quiesced()) return false;
#endif
	char packet[3072];
	size_t len = strlen(line);
	if(!len || len > 2303 || line[len - 1] != '}') return false;
	size_t off;
	if(watchdog_start) {
		/* This diagnostic path must not enter newlib formatting from the
		 * high-priority observer. Reject exceptional IDs rather than escaping
		 * them through snprintf; run IDs for these captures are plain ASCII. */
		for(size_t i = 0; s_run_id[i]; i++)
			if((unsigned char)s_run_id[i] < 0x20 || s_run_id[i] == '"'
			   || s_run_id[i] == '\\') return false;
		memcpy(packet, line, len - 1);
		packet[len - 1] = '\0';
		off = append_raw(packet, sizeof(packet), len - 1, ",\"run_id\":\"");
		off = append_raw(packet, sizeof(packet), off, s_run_id);
		off = append_raw(packet, sizeof(packet), off, "\"");
	} else {
		off = append_format(packet, sizeof(packet), 0, "%.*s,\"run_id\":",
			(int)len - 1, line);
		off = append_json_cstr(packet, sizeof(packet), off, s_run_id);
	}
	off = append_raw(packet, sizeof(packet), off, "}");
	if(off >= sizeof(packet)) return false;
#ifdef PLATFORM_WII
	/* Producer hooks publish a bounded request. The existing sender alone
	 * serializes/sends through its normal blocking transport, including the
	 * final tail behind the profile_done fence. This is not exception IO. */
	if(!strncmp(line, "{\"event\":\"service_observer\",", 28)
	   && LWP_GetSelf() == s_sender_thread) {
		if(s_pull && s_client_sock < 0) return false;
		bool sent = send_line_transport(packet, TELEMETRY_SEND_NORMAL);
		/* A false result after entering IOS can include an unknowable
		 * prefix. Retrying would duplicate a row already framed by resync.
		 * This observer ends fail-closed; other telemetry keeps its policy. */
		if(!sent) service_observer_transport_abort();
		return sent;
	}
	/* Probe pump has exactly one consumer: the existing sender. Send copied
	 * batches directly, preserving ring order and the profile_done barrier. */
	if((!strncmp(line, "{\"event\":\"g9_cpu\",", sizeof("{\"event\":\"g9_cpu\",") - 1)
		|| !strncmp(line, "{\"event\":\"g9_capture", sizeof("{\"event\":\"g9_capture") - 1))
	   && LWP_GetSelf() == s_sender_thread) {
		/* Retain samples in their own ring until TCP accepts. send_line's
		 * preconnect path buffers but returns false: retrying it here would
		 * duplicate batches and evict the boot/stamp record before MAIN arms
		 * accept (the isolated diagnostic deliberately runs before that). */
		if(s_pull && s_client_sock < 0) return false;
		return send_line(packet);
	}
#endif
#if defined(PLATFORM_PC) && !defined(CAVEWII_HW_TELEMETRY_TEST)
	(void)retry;
	if(watchdog_start) return false;
	return fprintf(stdout, "%s\n", packet) >= 0;
#else
	if(watchdog_start) {
		/* The watchdog must never wait on sender_wake's mutex or IOS. The
		 * sender polls the ring within 10 ms even without a signal. */
		if(!TEL_LOAD(s_enabled) || hw_telemetry_quiesced()) return false;
		return cw_tel_publish_observer(&s_sender_ring, packet);
	}
	if(retry)
		return send_observer_line_retry(packet);
	const char carve_attrib_prefix[] = "{\"event\":\"carve_attrib\"";
	return send_observer_line(packet,
		!strncmp(packet, carve_attrib_prefix, sizeof(carve_attrib_prefix) - 1));
#endif
}

static bool emit_observer_packet(const char* line, bool watchdog_start) {
	return emit_observer_packet_mode(line, watchdog_start, false);
}

bool hw_telemetry_emit_observer(const char* line) {
	return emit_observer_packet(line, false);
}

void hw_telemetry_save_path_note(const char* marker, uint32_t elapsed_us,
	const uint32_t stats[3], const unsigned cache[3]) {
	if(!marker || !stats || !cache || !hw_telemetry_active()) return;
	/* Only this profile uses cumulative summaries; existing timing captures
	 * retain their per-operation wire contract. Unknown future markers use
	 * the original admission/drop accounting rather than disappearing. */
	if(!strcmp(s_profile, "split_offer_piston_2m")) {
		if(__atomic_load_n(&s_path_closed, __ATOMIC_SEQ_CST)) return;
		for(unsigned i = 0; i < PATH_MARKERS; i++) {
			if(strcmp(marker, s_path_markers[i])) continue;
			uint32_t empty = 0;
			if(!__atomic_compare_exchange_n(&s_path_samples[i].writer, &empty,
				1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) break;
			if(__atomic_load_n(&s_path_closed, __ATOMIC_SEQ_CST)) {
				__atomic_store_n(&s_path_samples[i].writer, 0, __ATOMIC_RELEASE);
				return;
			}
			__atomic_fetch_add(&s_path_samples[i].version, 1, __ATOMIC_SEQ_CST);
			__atomic_fetch_add(&s_path_samples[i].count, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&s_path_samples[i].sum, elapsed_us, __ATOMIC_RELAXED);
			if(elapsed_us > __atomic_load_n(&s_path_samples[i].max, __ATOMIC_RELAXED))
				__atomic_store_n(&s_path_samples[i].max, elapsed_us, __ATOMIC_RELAXED);
			__atomic_store_n(&s_path_samples[i].last, elapsed_us, __ATOMIC_RELAXED);
			for(unsigned k = 0; k < 3; k++) {
				__atomic_store_n(&s_path_samples[i].stats[k], stats[k], __ATOMIC_RELAXED);
				__atomic_store_n(&s_path_samples[i].cache[k], cache[k], __ATOMIC_RELAXED);
			}
			__atomic_fetch_add(&s_path_samples[i].version, 1, __ATOMIC_SEQ_CST);
			__atomic_store_n(&s_path_samples[i].writer, 0, __ATOMIC_RELEASE);
			return;
		}
	}
	char line[384];
	int n = snprintf(line, sizeof line,
		"{\"event\":\"save_path_io\",\"marker\":\"%s\","
		"\"open_n\":%u,\"open_us\":%u,\"total_us\":%u,\"span_us\":%u,"
		"\"cache_selects\":%u,\"cache_hits\":%u,\"cache_fallbacks\":%u}",
		marker, stats[0], stats[1], stats[2], elapsed_us,
		cache[0], cache[1], cache[2]);
	if(n > 0 && (size_t)n < sizeof line)
		(void)hw_telemetry_emit_observer(line);
}

/* Main only. A refused snapshot stays byte-identical while later operations
 * accumulate independently. Versions fence coherent snapshots; sums/counts
 * use the same modulo-2^32 contract as the existing path timing counters.
 * Returns true only after all current summaries have entered the sender FIFO,
 * so MAIN can enqueue profile_done behind their final tail. */
bool hw_telemetry_save_path_pump(uint32_t now_ms) {
	if(strcmp(s_profile, "split_offer_piston_2m") || hw_telemetry_quiesced())
		return true;
	if(!s_path_send.pending && (!s_path_send.paced
	   || now_ms - s_path_send.mark >= PATH_SUMMARY_INTERVAL_MS)) {
		for(unsigned n = 0; n < PATH_MARKERS; n++) {
			unsigned i = (s_path_send.cursor + n) % PATH_MARKERS;
			uint32_t version = __atomic_load_n(&s_path_samples[i].version, __ATOMIC_SEQ_CST);
			if((version & 1u) || version == s_path_send.sent[i]) continue;
			uint32_t words[10];
			words[0] = __atomic_load_n(&s_path_samples[i].count, __ATOMIC_RELAXED);
			words[1] = __atomic_load_n(&s_path_samples[i].sum, __ATOMIC_RELAXED);
			words[2] = __atomic_load_n(&s_path_samples[i].max, __ATOMIC_RELAXED);
			words[3] = __atomic_load_n(&s_path_samples[i].last, __ATOMIC_RELAXED);
			for(unsigned k = 0; k < 3; k++) {
				words[4+k] = __atomic_load_n(&s_path_samples[i].stats[k], __ATOMIC_RELAXED);
				words[7+k] = __atomic_load_n(&s_path_samples[i].cache[k], __ATOMIC_RELAXED);
			}
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if(version != __atomic_load_n(&s_path_samples[i].version, __ATOMIC_SEQ_CST))
				continue;
			int len = snprintf(s_path_send.line, sizeof(s_path_send.line),
				"{\"event\":\"save_path_io\",\"marker\":\"%s\",\"aggregation\":\"cumulative\","
				"\"operations\":%u,\"span_total_us\":%u,\"span_max_us\":%u,\"span_us\":%u,"
				"\"open_n\":%u,\"open_us\":%u,\"total_us\":%u,"
				"\"cache_selects\":%u,\"cache_hits\":%u,\"cache_fallbacks\":%u}",
				s_path_markers[i], words[0], words[1], words[2], words[3],
				words[4], words[5], words[6], words[7], words[8], words[9]);
			if(len <= 0 || (size_t)len >= sizeof(s_path_send.line)) return false;
			s_path_send.slot = i; s_path_send.version = version;
			s_path_send.pending = true;
			break;
		}
	}
	if(s_path_send.pending) {
		if(!hw_telemetry_emit_observer_retry(s_path_send.line)) return false;
		s_path_send.sent[s_path_send.slot] = s_path_send.version;
		s_path_send.cursor = (s_path_send.slot + 1) % PATH_MARKERS;
		s_path_send.pending = false;
		s_path_send.paced = true; s_path_send.mark = now_ms;
	}
	for(unsigned i = 0; i < PATH_MARKERS; i++)
		if(__atomic_load_n(&s_path_samples[i].writer, __ATOMIC_SEQ_CST)
		   || s_path_send.sent[i] != __atomic_load_n(&s_path_samples[i].version, __ATOMIC_ACQUIRE))
			return false;
	return true;
}

void hw_telemetry_save_path_finish(void) {
	/* End diagnostic measurement with the driver's verdict. A writer that
	 * already entered completes its version before pump reports drained. */
	__atomic_store_n(&s_path_closed, 1, __ATOMIC_SEQ_CST);
}

bool hw_telemetry_emit_service_observer(const char* line) {
	/* Serialization is sender-only. The producer publishes one bounded
 * atomic request; only clean refusal may defer, never ambiguous TCP. */
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	return emit_observer_packet(line, false);
#else
	/* PC has no asynchronous transport: do not turn an observer flush into
 * potentially blocking stdout. Producer fixtures inject a bounded sink. */
	(void)line;
	return false;
#endif
}

bool hw_telemetry_emit_watchdog_start(const char* line) {
	return emit_observer_packet(line, true);
}

bool hw_telemetry_emit_observer_retry(const char* line) {
	return emit_observer_packet_mode(line, false, true);
}

uint32_t hw_telemetry_observer_deferred_count(void) {
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	return __atomic_load_n(&s_sender_ring.deferred, __ATOMIC_RELAXED);
#else
	return 0;
#endif
}

uint32_t hw_telemetry_portal_quit_up_ms(void) {
	return (uint32_t)time_diff_ms(s_portal_quit_started, time_get());
}

void hw_telemetry_portal_quit_switch_begun(void) {
	if(strcmp(s_profile, "split_portal_quit_4m") == 0
	   || strcmp(s_profile, "split_dims_6m") == 0)
		s_portal_quit_switch_begin_ms = hw_telemetry_portal_quit_up_ms();
}

bool hw_telemetry_emit_portal_quit_server(const char* marker,
					 uint32_t tick, const char* switch_state) {
	if(strcmp(s_profile, "split_portal_quit_4m") != 0
	   && strcmp(s_profile, "split_dims_6m") != 0)
		return false;
	char line[256];
	int n = snprintf(line, sizeof line,
		"{\"event\":\"split_portal_quit_server\",\"marker\":\"%s\","
		"\"seq\":%u,\"up_ms\":%u,\"tick\":%u,"
		"\"switch\":\"%s\",\"switch_begin_up_ms\":%u,"
		"\"request_id\":null}",
		marker, ++s_portal_quit_server_seq,
		hw_telemetry_portal_quit_up_ms(), tick,
		switch_state, s_portal_quit_switch_begin_ms);
	return n > 0 && (size_t)n < sizeof line
		&& hw_telemetry_emit_observer(line);
}

bool hw_telemetry_emit_anchor_frames(const struct hw_anchor_evidence* a) {
	if(!a || !a->enabled || !a->frame_count || hw_telemetry_quiesced())
		return true;
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t off = append_raw(line, sizeof(line), 0,
							"{\"event\":\"anchor_frames\",\"run_id\":");
	off = append_json_cstr(line, sizeof(line), off, s_run_id);
	off = append_format(line, sizeof(line), off,
						",\"anchor_frame_total\":%u,\"anchor_frame_dropped\":%"
						"u,\"anchor_frame_us\":[",
						a->frame_total, a->frame_dropped);
	for(uint32_t i = 0; i < a->frame_count && i < HW_ANCHOR_FRAME_CAP; i++)
		off = append_format(line, sizeof(line), off, "%s%u", i ? "," : "",
							a->frame_us[i]);
	off = append_format(line, sizeof(line), off, "]}");
	if(off >= sizeof(line))
		return false;
#ifdef CAVEWII_HW_TELEMETRY_TEST
	return send_line(line);
#elif defined(PLATFORM_PC)
	bool ok = fprintf(stdout, "%s\n", line) >= 0;
	return fflush(stdout) == 0 && ok;
#else
	return send_line(line);
#endif
}

void hw_telemetry_set_carve_largest_probe(bool (*probe)(uint32_t* bytes)) {
	s_carve_largest_probe = probe;
}

size_t hw_telemetry_format_column_pool_grow(char* dst, size_t dst_size,
											const struct hw_column_pool_grow* ev) {
	if(dst && dst_size)
		dst[0] = '\0';
	if(!ev)
		return 0;
	return append_format(dst, dst_size, 0,
		"{\"event\":\"column_pool_grow\",\"seq\":%u,\"requested_bytes\":%u,"
		"\"before_valid\":%s,\"free_bytes_before\":%u,"
		"\"largest_allocatable_bytes_before\":%u,\"slabs_before\":%u,"
		"\"slabs_after\":%u,\"success\":%s}",
		ev->seq, ev->requested_bytes, ev->before_valid ? "true" : "false",
		ev->before_valid ? ev->free_bytes_before : 0u,
		ev->before_valid ? ev->largest_allocatable_bytes_before : 0u,
		ev->slabs_before, ev->slabs_after, ev->success ? "true" : "false");
}

/* colcap-1006 (R4 point 9): the column-pool events are offered through the
 * LOSSLESS retrying observer admission -- the caller (mem2_pool_evq.h) keeps
 * a refused event at the head of its queue and retries it, so a refusal is
 * deferred, never a drop, and order is kept. */
bool hw_telemetry_emit_column_pool_grow(const struct hw_column_pool_grow* ev) {
	char line[384];
	size_t n = hw_telemetry_format_column_pool_grow(line, sizeof line, ev);
	return n > 0 && n < sizeof line && hw_telemetry_emit_observer_retry(line);
}

size_t hw_telemetry_format_column_pool_shrink(char* dst, size_t dst_size,
	const struct hw_column_pool_shrink* ev) {
	if(dst && dst_size)
		dst[0] = '\0';
	if(!ev)
		return 0;
	return append_format(dst, dst_size, 0,
		"{\"event\":\"column_pool_shrink\",\"seq\":%u,\"slab\":%u,"
		"\"released_bytes\":%u,\"slabs_before\":%u,\"slabs_after\":%u}",
		ev->seq, ev->slab, ev->released_bytes, ev->slabs_before,
		ev->slabs_after);
}

bool hw_telemetry_emit_column_pool_shrink(const struct hw_column_pool_shrink* ev) {
	char line[256];
	size_t n = hw_telemetry_format_column_pool_shrink(line, sizeof line, ev);
	return n > 0 && n < sizeof line && hw_telemetry_emit_observer_retry(line);
}

void hw_telemetry_emit_heartbeat(const struct hw_telemetry_heartbeat* hb) {
	/* colcap-1006: the caller (main.c) does not sample carve contiguity;
	 * the registered probe does, once per heartbeat, on this thread. Only a
	 * copy is modified -- the caller's record stays const. */
	static struct hw_telemetry_heartbeat s_hb_with_carve;
	if(hb && !hb->mem2_carve_largest_valid && s_carve_largest_probe) {
		s_hb_with_carve = *hb;
		uint32_t bytes = 0;
#ifdef PLATFORM_WII
		watchdog_main_census_phase(3);
#endif
		s_hb_with_carve.mem2_carve_largest_valid = s_carve_largest_probe(&bytes);
#ifdef PLATFORM_WII
		watchdog_main_census_phase(0);
#endif
		s_hb_with_carve.mem2_carve_largest_allocatable_bytes
			= s_hb_with_carve.mem2_carve_largest_valid ? bytes : 0u;
		hb = &s_hb_with_carve;
	}
#if defined(PLATFORM_WII) || defined(CAVEWII_HW_TELEMETRY_TEST)
	if(hb) __atomic_store_n(&s_live_tick, hb->tick, __ATOMIC_RELAXED);
#endif
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n =
		hw_telemetry_format_heartbeat(line, sizeof(line), s_run_id, s_profile, hb);
	if(n >= sizeof(line)) {
		// Truncated payload = an unterminated JSON line the collector would
		// silently drop -- and overflow happens exactly when float fields carry
		// corruption-scale garbage, i.e. when evidence matters most. Send a
		// minimal well-formed substitute instead.
		snprintf(line, sizeof(line),
				 "{\"event\":\"heartbeat\",\"run_id\":\"%s\",\"profile\":\"%s\","
				 "\"build_id\":\"%s\",\"tick\":%u,\"overflow\":true,"
				 "\"world_loaded\":%s,\"up_ms\":%llu}",
				 s_run_id, s_profile, hb && hb->build_id ? hb->build_id : "",
				 hb ? hb->tick : 0,
				 (hb && hb->world_loaded) ? "true" : "false",
				 (unsigned long long)heartbeat_uptime_ms());
	}
	char guard[HW_TELEMETRY_GUARD_LINE_SIZE];
	const char* paired_guard = NULL;
	if(hb && hb->perf_guard_present) {
		size_t off = hw_telemetry_format_perf_guard(guard, sizeof(guard),
			hb->perf_guard_words, hb->tick);
		if(!off || off >= sizeof(guard)) return;
		off = append_raw(guard, sizeof(guard), off - 1, ",\"run_id\":");
		off = append_json_cstr(guard, sizeof(guard), off, s_run_id);
		off = append_raw(guard, sizeof(guard), off, "}");
		if(off >= sizeof(guard)) return;
		paired_guard = guard;
	}
#if defined(PLATFORM_PC) && !defined(CAVEWII_HW_TELEMETRY_TEST)
	/* Preserve the PC diagnostic context previously emitted by main. */
	if(paired_guard && !hw_telemetry_quiesced())
		fprintf(stdout, "%s\n", paired_guard);
#endif
#ifdef PLATFORM_PC
	/* PC anchor replay uses stdout; keep the same context-before-heartbeat
	 * ordering as the dedicated Wii sender. OFF bytes remain unchanged. */
	/* nether-dark/1: CAVEWII_HB_STDOUT=1 also prints every PC heartbeat (a
	 * PC-only diagnostic switch; unset = the bytes above, unchanged). */
	static int s_pc_hb_stdout = -1;
	if(s_pc_hb_stdout < 0) {
		const char* e = getenv("CAVEWII_HB_STDOUT");
		s_pc_hb_stdout = e && e[0] == '1';
	}
	if(hb && (hb->anchor.enabled || s_pc_hb_stdout) && !hw_telemetry_quiesced()
	   && n < sizeof(line)) {
		fprintf(stdout, "%s\n", line);
		fflush(stdout);
	}
#endif
	send_heartbeat_line(line, paired_guard);
}

void hw_telemetry_emit_profile_done(const char* status) {
	/* Freeze before the completion fence: the sender drains this tail before
	 * sealing the collector. No dependency on MAIN exiting/audio teardown. */
	SERVICE_OBSERVER(service_observer_state(SO_SHUTDOWN); service_observer_request(true));
	g9_capture_finish();
#if defined(PLATFORM_WII) || !defined(CAVEWII_HW_TELEMETRY_TEST)
	settle_observer_finish();
	settle_observer_publish(hw_telemetry_emit_observer);
#endif
#if defined(PLATFORM_PC) && !defined(CAVEWII_HW_TELEMETRY_TEST)
	while(g9_capture_pump(hw_telemetry_emit_observer_retry)) {}
#endif
	char line[384];
	hw_telemetry_format_profile_done(line, sizeof(line), s_run_id, s_profile,
									 status);
	send_line(line);
#if defined(PLATFORM_PC) && !defined(CAVEWII_HW_TELEMETRY_TEST)
	/* PC has no line transport (send_line_transport is a no-op there), so
	   the completion line goes to stdout exactly like observer lines do --
	   an offline collector replay of a PC run can then see profile_done
	   (B1c contract, 2026-09-17). Wii behaviour unchanged. */
	fprintf(stdout, "%s\n", line);
#endif
}

void hw_telemetry_quiesce(void) {
	tps_observer_close(false);
#ifdef PLATFORM_WII
	cw_tel_close(&s_sender_ring);
	TEL_STORE(s_quiesce_requested, true); /* sender records trace after draining FIFO */
	sender_wake();
#else
	TEL_STORE(s_quiesced, true);
#endif
}

bool hw_telemetry_quiesced(void) {
#ifdef PLATFORM_WII
	return TEL_LOAD(s_quiesced) || TEL_LOAD(s_quiesce_requested) || TEL_LOAD(s_stop.requested);
#else
	return TEL_LOAD(s_quiesced);
#endif
}

void hw_telemetry_emit_dump(const char* cause, const char* text, bool fault) {
	if(!TEL_LOAD(s_enabled) || !text)
		return;
	static uint32_t dump_id = 0;
	char line[HW_TELEMETRY_DUMP_LINE_SIZE];
	const size_t chunk = HW_TELEMETRY_DUMP_TEXT_CHUNK;
	size_t len = strlen(text);
	uint32_t id = __atomic_add_fetch(&dump_id, 1, __ATOMIC_RELAXED);
	uint32_t seq = 0;
	for(size_t off = 0; off < len || (len == 0 && off == 0); off += chunk) {
		size_t n = len - off;
		if(n > chunk)
			n = chunk;
		bool last = off + n >= len;
		hw_telemetry_format_dump_chunk(line, sizeof(line), s_run_id, s_profile,
									   fault ? "fault" : "watchdog_dump", cause,
									   id, seq++, last, text + off, n);
		send_line_mode(line, fault ? TELEMETRY_SEND_WATCHDOG : TELEMETRY_SEND_NORMAL);
		if(len == 0)
			break;
	}
}

static bool emit_mailbox_file(const char* path, uint32_t dump_id) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return true;
	char text[HW_TELEMETRY_DUMP_TEXT_CHUNK];
	char next[HW_TELEMETRY_DUMP_TEXT_CHUNK];
	char line[HW_TELEMETRY_DUMP_LINE_SIZE];
	uint32_t seq = 0;
	// One-chunk lookahead so the FINAL chunk carries last=true even when the
	// file length is an exact multiple of the chunk size -- the old
	// "last = n < sizeof" test never terminated a 620/1240/...-byte file, and
	// the collector completes a dump only on a last=true chunk.
	size_t n = fread(text, 1, sizeof(text), f);
	for(;;) {
		size_t n2 = 0;
		if(n == sizeof(text))
			n2 = fread(next, 1, sizeof(next), f);
		bool last = n < sizeof(text) || n2 == 0;
		hw_telemetry_format_dump_chunk(line, sizeof(line), s_run_id, s_profile,
									   "crash_mailbox", path, dump_id, seq++,
									   last, text, n);
		if(!send_line(line)) {
			fclose(f);
			return false;
		}
		if(last)
			break;
		memcpy(text, next, n2);
		n = n2;
	}
	if(ferror(f) || fclose(f) != 0)
		return false;
	if(remove(path) != 0) {
		fprintf(stderr, "[hw-telemetry] could not retire mailbox %s\n", path);
		return false;
	}
	return true;
}

void hw_telemetry_emit_crash_mailbox(void) {
#ifdef PLATFORM_WII
	if(TEL_LOAD(s_stop.requested) || !TEL_LOAD(s_sender_started)
	   || LWP_GetSelf() != s_sender_thread) return;
#endif
	if(!TEL_LOAD(s_enabled) || s_mailbox_sending || s_mailbox_sent)
		return;
	s_mailbox_sending = true;
	s_mailbox_interrupted = false;
	bool sent = emit_mailbox_file(HW_TELEMETRY_HANG_REPORT_PATH, 1);
	if(sent)
		sent = emit_mailbox_file(HW_TELEMETRY_FBGUARD_REPORT_PATH, 2);
	// The APP-DIR FALLBACKS the watchdog writes when the root open fails (see
	// hw_telemetry.h). Replaying only the primaries meant that on exactly the boot where
	// the root write did NOT work -- the case the fallback exists for -- the mailbox came
	// back empty and the surviving report sat on the card unread. emit_mailbox_file
	// returns true for a path that does not exist, so on an ordinary boot these cost one
	// fopen each. DISTINCT dump_ids: the collector keys its assembly on (dump_id, seq), so
	// a fallback sharing its primary's id would overwrite that report part for part.
	if(sent)
		sent = emit_mailbox_file(HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH, 3);
	if(sent)
		sent = emit_mailbox_file(HW_TELEMETRY_FBGUARD_REPORT_FALLBACK_PATH, 4);
	// The SEQ-SUFFIXED hang reports (2026-07-29, watchdog.h wd_report_path_for_seq).
	// Reports 2..WD_REPORT_SEQ_MAX of a boot land at hang_report_<seq>.txt; replaying only
	// report 1 would leave every LATER stall on the card unread -- which is the same
	// "a sink nobody fetches" defect the fallback paths above were added to fix. The
	// 2026-07-29 session produced SIX. emit_mailbox_file returns true for a path that does
	// not exist, so an ordinary boot pays one fopen per slot and nothing else.
	// DISTINCT dump_ids throughout: the collector keys assembly on (dump_id, seq), so two
	// files sharing an id would overwrite each other part for part.
	char seq_path[WD_REPORT_PATH_MAX];
	for(uint32_t seq = 2; sent && seq <= WD_REPORT_SEQ_MAX; seq++)
		sent = emit_mailbox_file(wd_report_path_for_seq(seq_path, sizeof(seq_path),
														HW_TELEMETRY_HANG_REPORT_PATH,
														seq),
								 4 + (seq - 1)); // 5..11
	for(uint32_t seq = 2; sent && seq <= WD_REPORT_SEQ_MAX; seq++)
		sent = emit_mailbox_file(
			wd_report_path_for_seq(seq_path, sizeof(seq_path),
								   HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH, seq),
			4 + WD_REPORT_SEQ_MAX - 1 + (seq - 1)); // 12..18
	// If the client died mid-stream, the collector holds a HEAD-truncated copy
	// (cause/phase lines lost) -- keep the mailbox eligible so the next accept
	// resends it whole (same dump_id/seq keys: the collector's assembly just
	// overwrites the parts).
	s_mailbox_sent = sent && !s_mailbox_interrupted;
	s_mailbox_interrupted = false;
	s_mailbox_sending = false;
}
