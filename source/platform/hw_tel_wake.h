/* SENDER WAKE (2026-09-08). A bounded, SIGNALLED park for the telemetry
 * sender LWP, replacing its 1 kHz idle `usleep(1000)`.
 *
 * WHY. The sender thread runs at priority 65 (HW_TELEMETRY_SENDER_PRIORITY,
 * 7b412e4a) because at 64 it could never preempt main or the server and its
 * 1 ms idle sleep returned 1-2 s late on real hardware. 65 keeps the stream
 * alive, but a 1 ms timer sleep at a priority ABOVE main means ~1,000 timer
 * expirations and ~1,000 preemptions of the render loop per second on the
 * single Broadway core -- for an empty ring almost every time. This turns
 * that poll into a wait: the sender sleeps until a producer signals it, with
 * a 10 ms ceiling so nothing (the heartbeat cadence, the collector's 5 s
 * stale rule, the quiesce/stop checks) depends on the signal arriving.
 *
 * CORRECTNESS. `pending` is written only under `lock`, and the waiter tests
 * it under the same `lock` before it parks. A producer that sets pending
 * BEFORE the waiter parks is therefore seen (the waiter returns without
 * waiting); one that sets it AFTER wakes the waiter with the signal. There is
 * no window in which a wake is lost -- the failure mode the naive
 * "signal a condvar with no state behind it" version has. The condvar signal
 * itself is deliberately sent AFTER unlocking: it is legal on both platforms
 * and avoids waking a thread straight into a mutex it cannot take yet.
 *
 * COST TO THE PRODUCER. One uncontended mutex lock/unlock, a bool store and a
 * cond signal per admitted record -- no allocation, no syscall, no wait. The
 * lock is never held across a send, a copy or an IOS call, so the longest a
 * producer can be held up is the waiter's own predicate test. The lock-free
 * ring in hw_tel_ring.h is untouched: publish() itself still takes no mutex;
 * the signal happens at the lane boundary in hw_telemetry.c, after publish
 * has already returned.
 *
 * LIFETIME. Init once, never destroy in the production module -- see
 * hw_telemetry.c. A destroy racing a producer's signal would be a
 * use-after-free with no benefit (the object is .bss and outlives the
 * process). cw_tel_wake_destroy exists for tests, which own both ends. */
#ifndef CW_HW_TEL_WAKE_H
#define CW_HW_TEL_WAKE_H

#include "thread.h"

#include <stdbool.h>
#include <stdint.h>

struct cw_tel_wake {
	struct thread_mutex lock;
	struct thread_cond cond;
	/* Guarded by `lock`. "A producer has something for you" -- NOT the work
	 * itself: the ring stays the only source of truth, and a woken consumer
	 * always re-peeks it. */
	bool pending;
	/* Both primitives constructed. Read without the lock (there is no lock to
	 * take yet when it is false), so it is an atomic store/load pair. */
	bool ready;
};

static inline bool cw_tel_wake_init(struct cw_tel_wake* w) {
	if(!w)
		return false;
	__atomic_store_n(&w->ready, false, __ATOMIC_RELEASE);
	w->pending = false;
	if(!thread_mutex_init(&w->lock))
		return false;
	if(!thread_cond_init(&w->cond)) {
		thread_mutex_destroy(&w->lock);
		return false;
	}
	__atomic_store_n(&w->ready, true, __ATOMIC_RELEASE);
	return true;
}

/* Tests only (see LIFETIME above). Must not run while a producer may signal. */
static inline void cw_tel_wake_destroy(struct cw_tel_wake* w) {
	if(!w || !__atomic_load_n(&w->ready, __ATOMIC_ACQUIRE))
		return;
	__atomic_store_n(&w->ready, false, __ATOMIC_RELEASE);
	thread_cond_destroy(&w->cond);
	thread_mutex_destroy(&w->lock);
}

/* Producer side. Safe to call with nothing waiting (the flag is remembered)
 * and safe to call from any thread. A wake that fires before init simply does
 * nothing: the consumer's bounded timeout still bounds the latency. */
static inline void cw_tel_wake_signal(struct cw_tel_wake* w) {
	if(!w || !__atomic_load_n(&w->ready, __ATOMIC_ACQUIRE))
		return;
	thread_mutex_lock(&w->lock);
	w->pending = true;
	thread_mutex_unlock(&w->lock);
	thread_cond_signal(&w->cond);
}

/* Consumer side. Returns true when a wake was pending or arrived, false on
 * the timeout. Consumes the pending flag either way; a spurious return costs
 * one extra ring peek and nothing else. */
static inline bool cw_tel_wake_wait(struct cw_tel_wake* w,
									uint32_t timeout_ms) {
	if(!w || !__atomic_load_n(&w->ready, __ATOMIC_ACQUIRE)) {
		/* Degraded (init failed): keep the loop's pacing rather than spin. */
		thread_msleep(timeout_ms);
		return false;
	}
	thread_mutex_lock(&w->lock);
	bool woken = w->pending;
	if(!woken)
		woken = thread_cond_timedwait(&w->cond, &w->lock, timeout_ms);
	w->pending = false;
	thread_mutex_unlock(&w->lock);
	return woken;
}

#endif
