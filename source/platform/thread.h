/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#ifndef THREAD_H
#define THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// CR-08 (2026-08-20) compile-time proof: every __atomic_* use added across the
// cross-thread-sync sweep (server_local.h's thread_stop/thread_done, this
// file's tchannel_full_events/thread_create_failures, chunk_mesher.c's
// counters/gauges and their split-uint64 lo/hi halves) assumes uint32_t- and
// bool-sized objects are ALWAYS lock-free atomic on every build target. If
// that were ever false, GCC would silently lower one of those calls to a
// libatomic call instead -- a link-time dependency the Wii build cannot
// satisfy (no libatomic in the devkitPPC default link set) and which
// CLAUDE.md's "no 64-bit atomics on PPC32, don't grow a libatomic dependency"
// rule explicitly forbids reaching for. __atomic_always_lock_free is a GCC
// builtin usable in a constant expression on both toolchains this project
// builds with (ucrt64 GCC for PC, devkitPPC GCC for Wii) in C99 mode -- this
// is a real proof of the assumption, not a comment asserting it, and it
// re-checks on every TU that includes this header.
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
			   "uint32_t must be lock-free atomic on every CaveWii build "
			   "target (CR-08 cross-thread counters)");
_Static_assert(__atomic_always_lock_free(sizeof(bool), 0),
			   "bool must be lock-free atomic on every CaveWii build target "
			   "(CR-08 server_local thread_stop/thread_done)");

#ifdef PLATFORM_PC
#include <pthread.h>

struct thread {
	pthread_t native;
	bool created;
};

struct thread_channel {
	pthread_mutex_t lock;
	pthread_cond_t signal;
	void** data;
	size_t count;
	size_t length;
	bool initialized;
};

struct thread_mutex {
	pthread_mutex_t native;
	bool initialized;
	/* ci-1101/37 diagnostic: the stripe_thread_id() of the thread that holds
	 * this mutex (0 when free). Set on every acquisition, cleared on release;
	 * a plain store, no syscall. Read once by the drain owner just before it
	 * blocks (and never after, when it would read itself), so a long drain
	 * wait can name the holder. */
	uint32_t owner;
};

struct thread_cond {
	pthread_cond_t native;
	bool initialized;
};

#endif

#ifdef PLATFORM_WII
#include <gccore.h>
// gccore.h stops at mutex; cond_t lives in its own header.
#include <ogc/cond.h>

struct thread {
	lwp_t native;
	// Owned by us — memalign'd in thread_create, freed in thread_join.
	// libogc's default 8 KB stack is too small for our worker threads
	// (server world tick, chunk mesher recursion); we pre-allocate
	// 64 KB instead.
	void* stack;
	bool created;
};

struct thread_channel {
	mqbox_t native;
	bool initialized;
};

struct thread_mutex {
	mutex_t native;
	bool initialized;
	/* ci-1101/37 diagnostic; see the PC arm above. */
	uint32_t owner;
};

struct thread_cond {
	cond_t native;
	bool initialized;
};
#endif

bool thread_create(struct thread* t, void* (*entry)(void* arg), void* arg,
				   uint8_t priority);
void thread_join(struct thread* t);
// Change a LIVE thread's scheduler priority (P2-a, 2026-09-03 load-ramp fix;
// see server_local.c's server_local_thread). Wii: LWP_SetThreadPriority --
// libogc convention, higher number = higher priority (LWP_PRIO_IDLE=0,
// LWP_PRIO_HIGHEST=127); callable on any live thread, including from within
// itself, and never fails (void return). PC: a no-op -- PC threads are
// pthreads on a preemptive multi-core OS scheduler, not libogc's single-core
// cooperative-at-equal-priority one, so there is no starvation for a
// priority drop to fix and no portable priority knob worth reaching for
// (would need CAP_SYS_NICE / a realtime scheduling policy on Linux, an
// Administrator token on Windows). Callers that need to test the PRIORITY
// DECISION itself (not the OS effect) test the pure decision function
// instead -- see server_tick_pacing.h's server_thread_priority_for and
// tests/test_server_tick_pacing.c.
void thread_set_priority(struct thread* t, uint8_t priority);
// Disposable thread for abandonable work (the server_remote connector
// pattern: resolve+connect has no portable cancel, so the spawner must be
// able to walk away). PC: created detached — the OS reaps it on exit and
// thread_dispose is a no-op either way. Wii: the handle + 64 KB stack are
// kept; thread_dispose(t, true) joins (the thread has finished — the join
// returns promptly) and frees the stack, thread_dispose(t, false) abandons
// them ON PURPOSE (one LWP bookkeeping entry + 64 KB MEM2 per abandoned
// in-flight connect — the same accepted leak shape as PC's detached thread
// stuck in a resolver, bounded by user connect attempts).
bool thread_spawn_disposable(struct thread* t, void* (*entry)(void* arg),
							 void* arg, uint8_t priority);
void thread_dispose(struct thread* t, bool completed);

// Portable mutex/condvar (S5: the server_remote family synchronizes with
// these on both platforms — PC pthread underneath, Wii libogc LWP).
// Lock/unlock/wait/signal/broadcast assert on an uninitialized object in
// test builds and are no-ops on one in NDEBUG builds; init returns false
// on failure and the object stays safe to destroy.
bool thread_mutex_init(struct thread_mutex* m);
void thread_mutex_destroy(struct thread_mutex* m);
void thread_mutex_lock(struct thread_mutex* m);
void thread_mutex_unlock(struct thread_mutex* m);

/* R18 audio snapshots: on Wii exclude waiter enqueue between try and
 * release, and reject an existing waiter list on this thread. Thus release
 * visits ZERO queue entries and cannot reschedule. Keep the guarded work
 * finite and small; never retain this guard across a continuation. */
struct thread_mutex_guard { uint32_t irq; bool held; };
bool thread_mutex_try_guard(struct thread_mutex* m, struct thread_mutex_guard* guard);
void thread_mutex_release_guard(struct thread_mutex* m, struct thread_mutex_guard* guard);

bool thread_cond_init(struct thread_cond* c);
void thread_cond_destroy(struct thread_cond* c);
// Atomically releases m and waits; m is re-held on return (pthread/LWP
// condvar contract on both platforms).
void thread_cond_wait(struct thread_cond* c, struct thread_mutex* m);
// Bounded variant of thread_cond_wait: atomically releases m and waits for at
// most timeout_ms, re-holding m on return. Returns true when the wait ended in
// a SIGNAL, false when it ended in the timeout (or the object was unusable).
//
// SPURIOUS WAKEUPS ARE POSSIBLE ON BOTH PLATFORMS, so the return value is
// diagnostic ONLY -- every caller must re-check its real predicate under m
// either way. It exists so a poller can park instead of spinning on a timer
// (the telemetry sender's idle wait, hw_tel_wake.h) without giving up a
// deadline.
//
// PLATFORM NOTE, and the reason this wrapper exists at all: the two
// underlying primitives disagree about what the timespec MEANS.
// pthread_cond_timedwait takes an ABSOLUTE deadline (CLOCK_REALTIME for a
// default-attr condvar); libogc's LWP_CondTimedWait takes a RELATIVE duration
// (ogc/cond.h:92-100, "reltime ... holding the relative time for the
// timeout"). Passing one where the other is expected does not fail loudly --
// it silently waits ~56 years or returns instantly -- so the conversion is
// done once, here, and never at a call site.
bool thread_cond_timedwait(struct thread_cond* c, struct thread_mutex* m,
						   uint32_t timeout_ms);
void thread_cond_signal(struct thread_cond* c);
void thread_cond_broadcast(struct thread_cond* c);
uint32_t thread_create_failure_count(void);
/* Temporary Wii stall probe: first overwritten byte above the stack base.
 * Zero means no live/observable stack; 65536 means the paint is intact. */
size_t thread_stack_lowwater_bytes(const struct thread* t);
/* Wii-only saved-context census. The caller runs on another thread (normally
 * main), and receives only fixed-size scalars/return addresses. PC returns
 * valid=false because pthreads do not expose this libogc context ABI. */
#define THREAD_SAVED_CONTEXT_CALLERS 24
struct thread_saved_context {
	uint32_t valid;
	uint32_t pc, lr, sp;
	uint32_t state, suspend, priority;
	uint32_t wait_queue, wait_token;
	uint32_t caller_count;
	uint32_t callers[THREAD_SAVED_CONTEXT_CALLERS];
};
bool thread_saved_context_snapshot(const struct thread* t,
							   struct thread_saved_context* out);
/* Call once from main before publishing the diagnostic sampler. Registration
 * verifies the current SP against the linked SDK's default main stack. The
 * snapshot must run on another thread. Both return false on PC. */
bool thread_main_context_register(void);
bool thread_main_context_snapshot(struct thread_saved_context* out);
#ifdef PLATFORM_WII
/* For non-wrapper threads with caller-owned stacks (e.g. telemetry). The
 * caller must keep the handle and this exact stack allocation alive until
 * sampling stops. No allocation, formatting, waiting or lock acquisition. */
bool thread_saved_context_snapshot_raw(lwp_t native, const void* stack,
									  size_t stack_bytes,
									  struct thread_saved_context* out);
#endif
void thread_msleep(size_t ms);

/* G9 R8/R10 + REL-10 T2: main-only checkpoints that briefly sleep MAIN while
 * the local gameplay server can use the CPU: inside the registered tick
 * owner's gameplay tick (thread_g9_tick_boost_local/begin/end publish owner
 * and active state whatever the boost/lease/diagnostic toggles), or inside a
 * foreign normal light drain. Refused for no/remote/stale owner, between
 * ticks, an owner that is waiting/sleeping/blocked/in a lease dependency (Wii
 * also: not KTHR_STATE_RUNNING), MAIN holding a thread_mutex/stripe, or a
 * lease waiting for MAIN service; rechecked immediately before the sleep.
 * Startup configuration (experimental.g9_main_pacing_int, ON = preset 1 by
 * default since REL-10). Frame cap bounds requested sleep, not scheduler wake
 * latency. No priority changes or simulation deferral. G9 sleep records use
 * the existing bounded IRQ-excluded recorder on Wii; no allocation,
 * formatting or simulation lock in the checkpoint. */
void thread_main_pacing_configure(bool enabled, int slice_us,
	int sleep_ms, int frame_sleep_ms);
bool thread_main_pacing_enabled(void);
void thread_main_pacing_frame(void);
enum thread_pacing_phase { THREAD_PACING_OTHER, THREAD_PACING_RPC,
	THREAD_PACING_LIGHTING, THREAD_PACING_BUILD, THREAD_PACING_SCREEN,
	THREAD_PACING_RENDER, THREAD_PACING_PHASE_COUNT };
bool thread_main_pacing_checkpoint(void);
bool thread_main_pacing_checkpoint_at(enum thread_pacing_phase phase);
/* Main-owned cumulative saturating counters since startup configure:
 * enabled,slice_us,sleep_ms,cap_ms,checks,foreign (a donee: foreign drain or
 * eligible tick owner),no_drain (no eligible donee, incl. REL-10 refusals and
 * pre-sleep recheck failures),self_drain,
 * slice_skips,cap_skips,cap_frames,sleeps,requested_ms,wake_us,wake_max_us,
 * saturated, then six per-phase sleep counts (enum order). OFF observations are collected only with the G9 probe armed. */
#define THREAD_PACING_WORDS 22u
bool thread_main_pacing_snapshot(uint32_t out[THREAD_PACING_WORDS]);
/* REL-10-overdue (hw1030f: a 129 ms server tick at ~20 ms of CPU while MAIN
 * ran saturated at 64 and the per-frame cap limited donation to 8 ms/frame).
 * While the registered tick owner's gameplay tick is older than overdue_ms,
 * a checkpoint bypasses the frame cap and halves the slice, for at most
 * budget_ms of escalated sleep per tick (then the normal rules resume, so a
 * Nether-sized drain costs MAIN at most budget_ms extra). Foreign drains and
 * idle/non-owner states are untouched. overdue_ms 0 = OFF (pre-change
 * behaviour). Scheduling only: no simulation value changes. MAIN-only calls;
 * configure() applies the defaults first. docs/g9_tick_boost.md. */
#define THREAD_PACING_OVERDUE_MS_DEFAULT 12
#define THREAD_PACING_OVERDUE_BUDGET_MS_DEFAULT 40
void thread_main_pacing_overdue_configure(int overdue_ms, int budget_ms);
/* overdue_us, budget_ms, ticks escalated, escalated sleeps, sleeps the frame
 * cap had refused, budget exhaustions, last escalated tick serial, reserved. */
#define THREAD_PACING_OVERDUE_WORDS 8u
bool thread_main_pacing_overdue_snapshot(uint32_t out[THREAD_PACING_OVERDUE_WORDS]);

/* Eight process-lifetime column stripes. Diagnostics only: these calls retain
 * the underlying mutex ordering and never acquire another lock. */
enum thread_stripe_tag { THREAD_STRIPE_OTHER = 0, THREAD_STRIPE_MAIN = 1,
	THREAD_STRIPE_SERVER = 2, THREAD_STRIPE_MESHER = 3 };
enum thread_stripe_state { THREAD_STRIPE_IDLE = 0, THREAD_STRIPE_ACQUIRING = 1,
	THREAD_STRIPE_HELD = 2, THREAD_STRIPE_RELEASING = 3 };
#define THREAD_STRIPE_WAIT_THRESHOLD_US 250000u
struct thread_stripe_breadcrumb {
	uint32_t stripe, holder_tag, holder_id, holder_phase, wait_us, active;
	uint32_t holder_state, uncertain;
};
void thread_stripe_register(struct thread_mutex* mutex, unsigned stripe);
/* Stable operator-facing name for a stripe-registered mutex's long-wait
 * attribution (ci-1101/37); borrowed, must outlive the mutex. Call once after
 * thread_stripe_register. */
void thread_stripe_set_name(struct thread_mutex* mutex, const char* name);
void thread_stripe_phase(enum thread_stripe_tag tag, uint32_t phase);
void thread_stripe_lock(struct thread_mutex* mutex);
void thread_stripe_unlock(struct thread_mutex* mutex);
bool thread_stripe_breadcrumb_get(struct thread_stripe_breadcrumb* out);
#ifdef PLATFORM_PC
/* PC-only interleaving hooks. READ_BEGIN / READ_VALIDATE bracket the payload
 * loads of every seqlock read (stripe role records and the completed record);
 * UNLOCKED is the post-unlock/pre-idle window; ACTIVE_SLOT is inside the
 * active-wait record read; ACTIVE_HOLDER is after the holder scan and before
 * the active-wait record is re-validated. */
enum thread_stripe_test_event { THREAD_STRIPE_TEST_ODD = 1,
	THREAD_STRIPE_TEST_ACQUIRED = 2, THREAD_STRIPE_TEST_RELEASING = 3,
	THREAD_STRIPE_TEST_READ_BEGIN = 4, THREAD_STRIPE_TEST_READ_VALIDATE = 5,
	THREAD_STRIPE_TEST_UNLOCKED = 6, THREAD_STRIPE_TEST_ACTIVE_SLOT = 7,
	THREAD_STRIPE_TEST_ACTIVE_HOLDER = 8 };
void thread_stripe_test_hook_set(void (*hook)(unsigned event, void*), void* user);
void thread_stripe_test_snapshot(unsigned stripe, struct thread_stripe_breadcrumb* out);
#endif
// Relinquish the core to other READY threads of EQUAL priority, then resume (LWP_YieldThread:
// a strictly higher-priority thread already preempts on its own; a lower one never runs). On the
// Wii the server thread runs at SERVER_THREAD_PRIORITY 63 (server_tick_pacing.h), BELOW the
// main/render thread (64), so render is protected by priority, not by these yields; a long
// server op is a server-tick stall. The yields are pure PACING toward equal-priority peers --
// NO change to what is generated or simulated. PC: effectively a no-op.
void thread_yield(void);

/* Temporary G9 elapsed attribution. One drain owner; other threads never
 * write its counters. These are elapsed intervals, NOT scheduler CPU time.
 * Recovery sleeps exclude stripe reacquisition (counted as mutex wait).
 *
 * ci-1101/37 (hw1101c: one 71,206 us lock_us inside the server tick's light
 * drain, with no way to name the contended lock): while the drain owner waits
 * >= THREAD_DRAIN_WAIT_ATTRIB_US in thread_mutex_lock, the LONGEST such wait of
 * the current heartbeat window is attributed here -- the owner's exact elapsed
 * us, the contended mutex (address low 32 bits + its registered stripe index
 * and name when one exists) and the HOLDING thread's id/tag, sampled from the
 * mutex's owner field BEFORE the wait (0 = free or unknown). Both lock
 * implementations record it with ONE extra compare on the wait path and
 * nothing when the wait is short: no allocation, no syscall, no lock-path
 * printf. A long wait on an UNREGISTERED mutex has no stripe, so index and
 * name stay 0/"" with wait_us > 0 -- the miss is explicit. */
#define THREAD_DRAIN_WAIT_ATTRIB_US 10000u
#define THREAD_DRAIN_WAIT_MUTEX_NAME_MAX 24u
struct thread_drain_waits {
	uint64_t lock_us, recovery_us;
	uint32_t wait_us;         /* longest >= 10 ms wait this window, 0 if none */
	uint32_t wait_mutex;      /* contended mutex address low 32 bits */
	uint32_t wait_holder_id;  /* holder stripe_thread_id(), 0 free/unknown */
	uint32_t wait_holder_tag; /* enum thread_stripe_tag of the holder, 0 other */
	uint32_t wait_stripe;     /* thread_stripe_register index, 0 unregistered */
	char wait_mutex_name[THREAD_DRAIN_WAIT_MUTEX_NAME_MAX];
};
bool thread_drain_wait_active(void);
void thread_drain_wait_begin(void);
void thread_drain_wait_end(struct thread_drain_waits* out);
void thread_drain_recovery_sleep(void);

/* R19: experimental local-server drain lease. OFF is dormant. MAIN calls
 * service only at the completed draw-batch/input boundary, with no held
 * world/stripe lease. The server owns begin/checkpoint/end on one call stack. */
#include "g9_priority_lease.h"
void thread_g9_lease_configure(bool enabled, int max_us, int service_us);
void thread_g9_lease_sender(bool (*set_priority)(uint8_t));
bool thread_g9_lease_enabled(void);
bool thread_g9_lease_local_present(void);
bool thread_g9_lease_active_owner(void);
bool thread_g9_lease_active(void);
void thread_g9_lease_pops(uint32_t pops);
void thread_g9_lease_local(bool present, uint8_t base_priority);
void thread_g9_lease_begin(void);
void thread_g9_lease_end(void);
void thread_g9_lease_cancel(void);
void thread_g9_lease_checkpoint(void);
void thread_g9_lease_dependency(void);
void thread_g9_lease_dependency_done(void);
bool thread_g9_lease_defer(void);
void thread_g9_lease_audio(uint32_t reserve_us, bool worker_pending);
struct g9_lease_token thread_g9_lease_service_begin(void);
bool thread_g9_lease_service_end(struct g9_lease_token token,
	uint32_t duration_us, bool complete);
void thread_g9_lease_snapshot(struct g9_lease_stats* out);
void thread_g9_lease_state(struct g9_priority_lease* out, uint32_t* lock_depth);
/* Dormant deterministic clock seam; arm only with no local owner. */
void thread_g9_lease_test_clock(uint32_t (*read_us)(void));

/* G9 tick-wide boost (T2 opt-in, experimental.g9_tick_boost_int). The local
 * server raises ITSELF above MAIN for one whole gameplay tick (update +
 * epilogue) and restores at the tick's end or at the per-tick cap, whichever
 * comes first. Composes with the drain lease: effective priority = max. No
 * owner registered (remote Beta MP, no local server) = begin is a no-op. */
void thread_g9_tick_boost_configure(bool enabled, int cap_ms);
bool thread_g9_tick_boost_enabled(void);
/* Enabled AND a local server thread currently owns the boost (false for remote
 * Beta MP / no local server). Takes g9_lock; never call it under another lock
 * the boost path could need. Used to gate audio_music's decode promotion. */
bool thread_g9_tick_boost_local_present(void);
void thread_g9_tick_boost_local(bool present, uint8_t base_priority);
void thread_g9_tick_boost_begin(void);
void thread_g9_tick_boost_end(void);
void thread_g9_tick_boost_snapshot(struct g9_tick_boost_stats* out, bool* boosted);

/* MAIN-owned service cursor. Capabilities describe the current safe seam.
 * INPUT/AUDIO credit comes from the real once-per-frame operations, never
 * from a synthetic poll or a cached pre-request sample. R22 native input/audio
 * admission rejects until bounded entry points are available (failed candidate).
 * dispatch returns whether ACK was accepted; snapshot also checks return faults. */
enum g9_handoff_operation { G9_HANDOFF_RPC = 1, G9_HANDOFF_MESH = 2,
	G9_HANDOFF_INPUT = 4, G9_HANDOFF_AUDIO = 8, G9_HANDOFF_ALL = 15 };
enum g9_handoff_result { G9_HANDOFF_DONE, G9_HANDOFF_MORE, G9_HANDOFF_UNSAFE };
typedef enum g9_handoff_result (*g9_handoff_service)(unsigned operation,
	uint32_t remaining_us);
void thread_g9_handoff_configure(bool enabled);
bool thread_g9_handoff_enabled(void);
bool thread_g9_handoff_dispatch(unsigned capabilities, g9_handoff_service service);
struct g9_lease_token thread_g9_handoff_work_begin(unsigned operation);
uint32_t thread_g9_handoff_audio_remaining(void);
void thread_g9_handoff_audio_abort(void);
/* MAIN-only, finite/try-only native reserve validator. Called before ACK
 * and again at actual wait entry. NULL keeps the original OFF/control path. */
void thread_g9_handoff_audio_guard(bool (*validate)(uint32_t proposed_wait_us,
	uint32_t remaining_service_us));
void thread_g9_handoff_work_end(struct g9_lease_token token,
	unsigned operation, bool complete);
/* Rejection control for host tests; cannot override a failing native check. */
void thread_g9_handoff_test_priority(bool valid);
bool thread_g9_handoff_test_sequence(uint32_t generation, uint32_t turn);
bool thread_g9_handoff_test_timer(void);

bool tchannel_init(struct thread_channel* c, size_t count);
void tchannel_close(struct thread_channel* c);
bool tchannel_receive(struct thread_channel* c, void** msg, bool block);
// CONTRACT: block=true never drops the message -- it waits until the channel
// has room and returns true. Every block=true send site in the tree relies on
// this AND is individually proven never to wait at all (each channel is a
// token-conserved pool: capacity == total tokens in circulation, and a sender
// always holds >=1 token out of the channel at send time -- see the one-line
// proof at each call site). The wait path is therefore pure insurance; entering
// it means token conservation was broken by a future change. That event is
// counted (tchannel_full_count, "tokfull=" in the Wii watchdog hang report) so
// the regression names itself instead of degrading silently.
// On Wii this contract has to be EMULATED: libogc v3.0.4's tuxedo compat shim
// (lwpcompat/lwpc_mq.c) forces every MQ_Send through KMailboxTrySend, which on
// a full queue returns false and silently drops the message -- the 2026-06-11
// audit that root-caused the framebuffer-flip hang (see platform/wii/gfx.c).
bool tchannel_send(struct thread_channel* c, void* msg, bool block);
// Times a block=true send EVER found its channel full (should read 0 forever;
// see the tchannel_send contract above). Same diagnostic pattern as
// gfx_fb_ring_over_count / svin_dropped_count.
uint32_t tchannel_full_count(void);

#endif
