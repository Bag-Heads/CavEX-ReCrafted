/* Included by thread.c: shares its IRQ-safe g9_lock, thread identity and
 * monotonic clock. No diagnostic enable flag controls the real lease. The
 * known base priorities are supplied by their owners, never inferred from
 * effective/inherited KThread.prio. */
static struct g9_priority_lease s_lease;
static uint32_t s_lease_owner, s_lease_main, s_lease_depth;
static uint8_t s_lease_base;
static bool (*s_lease_sender)(uint8_t);
static bool s_lease_sender_raised, s_lease_audio_valid, s_lease_audio_worker;
static uint32_t s_lease_audio_at, s_lease_audio_reserve;
static bool s_lease_main_priority_ok = true;
static uint32_t (*s_lease_test_clock)(void);
/* G9 tick-wide boost. Serialized by g9_lock (IRQ exclusion on Wii); the cap
 * timer runs under the SDK IRQ lock exactly like the lease timer below. */
static struct {
	bool enabled, boosted, sender_raised;
	uint32_t owner, cap_us, start;
	/* debug.g9_worst_tick_int reads these lock-free (thread.c): cap_at is
	 * the cap instant (lease_now), published before cap_seq increments. */
	uint32_t cap_at, cap_seq;
	uint8_t base;
	struct g9_tick_boost_stats stats;
} s_tb;
static bool s_handoff_enabled, s_handoff_priority_ok = true;
static uint32_t s_handoff_test_turn;
static bool (*s_handoff_audio_guard)(uint32_t proposed_wait_us, uint32_t remaining_service_us);
static struct {
	struct g9_lease_token token;
	uint32_t used_us, done, operation_start, entered;
	unsigned operation;
} s_handoff; /* MAIN only; no pool, ring, stack or worker growth. */
static uint32_t lease_now(void) {
	return s_lease_test_clock ? s_lease_test_clock() : stripe_now_us();
}
void thread_g9_lease_test_clock(uint32_t (*read_us)(void)) {
	assert(!s_lease_owner && !s_lease.active);
	s_lease_test_clock = read_us;
}
#ifdef PLATFORM_WII
static KTickTask s_lease_timer;
static KTickTask s_tb_timer;
static KThrQueue s_lease_main_queue;
static KTickTask s_lease_main_timer;
#else
static pthread_cond_t s_lease_main_wake = PTHREAD_COND_INITIALIZER;
#endif
static bool s_lease_main_timeout;
static void lease_main_wake(void) {
	if(!s_handoff_enabled) return;
#ifdef PLATFORM_WII
	KThrQueueUnblockAllByMask(&s_lease_main_queue, UINT32_MAX);
#else
	pthread_cond_broadcast(&s_lease_main_wake);
#endif
}
static bool lease_owner(void) {
	return s_lease_owner && s_lease_owner == stripe_thread_id();
}
/* Caller holds g9_lock. Tuxedo's explicit priority setter can context-switch
 * EVEN WITH IRQs MASKED: commit every visible transition BEFORE calling it.
 * No libc allocation, mutex or telemetry emission in this section. */
static void lease_priority(uint8_t priority) {
	s_lease.stats.priority_sets++;
	/* Compose with the tick-wide boost: effective = max(lease, tick). A lease
	 * restore (65 -> base) can therefore never end a running tick boost early;
	 * the tick boost's own end/cap applies the lease's state symmetrically. */
	if(s_tb.boosted && s_tb.owner == s_lease_owner
	   && priority < G9_TICK_BOOST_PRIORITY) priority = G9_TICK_BOOST_PRIORITY;
#ifdef PLATFORM_WII
	LWP_SetThreadPriority((lwp_t)s_lease_owner, priority);
#else
	(void)priority; /* PC's preemptive scheduler has no equivalent knob. */
#endif
}
static uint32_t lease_flags(void) {
	return s_lease.active | (s_lease.boosted << 1) | (s_lease.waiting << 2)
		| (s_lease.failed << 3) | (s_lease.parked << 4);
}
static void lease_event(uint32_t event, uint32_t reason, uint32_t duration,
	bool accepted, bool complete) {
	if(!g9_capture_enabled()) return;
	capture_lease(event, reason, s_lease.generation, s_lease.turn,
		duration, accepted, complete, lease_flags(), s_lease.pops);
}
static void lease_drop(uint32_t now, uint32_t reason) {
	bool boosted = s_lease.boosted;
	g9_lease_restore(&s_lease, now);
#ifdef PLATFORM_WII
	KTickTaskStop(&s_lease_timer);
#endif
	if(boosted) { lease_event(3, reason, now-s_lease.start, true, true); lease_priority(s_lease_base); }
	lease_main_wake();
}
static uint32_t lease_reserve(uint32_t now) {
	if(!s_lease_audio_valid) return 0;
	if(s_lease_audio_reserve == UINT32_MAX) return UINT32_MAX;
	uint32_t age = now-s_lease_audio_at;
	return age < s_lease_audio_reserve ? s_lease_audio_reserve-age : 0;
}
static bool lease_request(uint32_t now) {
	/* Wrapped pairs cannot alias an earlier service cursor. R19 OFF keeps
	 * its original path; the treatment fails closed until a fresh session. */
	if(s_handoff_enabled && s_lease.turn == UINT32_MAX) {
		s_lease.failed = s_lease.cancelled = true;
		s_lease.stats.faults++;
		s_lease.stats.cancelled++;
		lease_drop(now, 6);
		return false;
	}
	g9_lease_request(&s_lease, now);
	return true;
}
static void lease_timer_restore(void) {
	/* Independent recovery even if the server never reaches its next pop
 * hook. This changes only its BASE priority; no simulated heartbeat. The
 * timer runs under the SDK IRQ lock (tuxedo/tick.c:KTickTaskRun). */
	uint32_t now = lease_now();
	if(s_lease.boosted) {
		if(!lease_request(now)) return;
		s_lease.stats.timer_restores++;
		lease_event(3, 4, now-s_lease.start, true, true);
		lease_event(4, 4, 0, true, true);
		/* request restores the model; retain the native transition below. */
#ifdef PLATFORM_WII
		KTickTaskStop(&s_lease_timer);
#endif
		lease_priority(s_lease_base);
		lease_main_wake();
	}
}
#ifdef PLATFORM_WII
static void lease_timer_expire(KTickTask* task) {
	(void)task;
	lease_timer_restore();
}
#endif
bool thread_g9_handoff_test_timer(void) {
	if(!s_lease_test_clock || !s_handoff_enabled) return false;
	g9_lock_t lock = g9_lock();
	bool expired = s_lease.boosted && lease_now()-s_lease.start >= s_lease.max_us;
	if(expired) lease_timer_restore();
	g9_unlock(lock);
	return expired;
}
static void lease_raise(uint32_t now) {
	bool priority_ok = s_handoff_priority_ok;
#ifdef PLATFORM_WII
	KThread* self = KThreadGetSelf();
	priority_ok = priority_ok && self->prio == self->baseprio;
#endif
	if(s_handoff_enabled && !priority_ok) {
		s_lease.failed = true; s_lease.stats.faults++;
		lease_event(9, 7, 0, false, false);
		lease_drop(now, 7);
		return;
	}
	if(s_lease_depth || !g9_lease_audio_ok(&s_lease, lease_reserve(now),
		!s_lease_audio_valid || s_lease_audio_worker)) {
		s_lease.failed = true;
		lease_drop(now, 5);
		return;
	}
	if(s_lease.boosted) return; /* Keep the audio check; one setter/timer per interval. */
	uint32_t turn = s_lease.turn;
	if(s_handoff_enabled && now-s_lease.start >= s_lease.max_us) {
		if(lease_request(now) && turn != s_lease.turn) lease_event(4, 3, 0, true, true);
		return;
	}
	if(!g9_lease_boost(&s_lease, now)) {
		if(turn != s_lease.turn) lease_event(4, 3, 0, true, true);
		return;
	}
#ifdef PLATFORM_WII
	uint32_t left = s_lease.max_us - (now-s_lease.start);
	KTickTaskStart(&s_lease_timer, lease_timer_expire,
		(uint64_t)left * G9_TB_HZ / 1000000u, 0);
#endif
	lease_event(2, 0, 0, true, true);
	lease_priority(65);
}
void thread_g9_lease_configure(bool enabled, int max_us, int service_us) {
	assert(!s_lease_owner && !s_lease.active);
	s_lease = (struct g9_priority_lease){.enabled = enabled,
		.max_us = max_us < 500 ? 500u : max_us > 8000 ? 8000u : (uint32_t)max_us,
		.service_us = service_us < 100 ? 100u : service_us > 2000 ? 2000u : (uint32_t)service_us,
		.wait_us = 4000u, .stats.reserve_min_us = UINT32_MAX};
	s_lease_main = stripe_thread_id();
	s_lease_audio_valid = false;
#ifdef PLATFORM_WII
	KThread* main_thread = KThreadGetSelf();
	s_lease_main_priority_ok = main_thread->baseprio == KTHR_MIN_PRIO-64;
#endif
}
void thread_g9_lease_sender(bool (*set_priority)(uint8_t)) {
	assert(!s_lease.active);
	s_lease_sender = set_priority;
}
bool thread_g9_lease_enabled(void) { return s_lease.enabled; }
bool thread_g9_lease_local_present(void) {
	if(!s_lease.enabled) return false;
	g9_lock_t lock = g9_lock(); bool present = s_lease_owner != 0; g9_unlock(lock);
	return present;
}
bool thread_g9_lease_active_owner(void) {
	if(!s_lease.enabled) return false;
	g9_lock_t lock = g9_lock();
	bool active = lease_owner() && s_lease.active;
	g9_unlock(lock);
	return active;
}
bool thread_g9_lease_active(void) {
	if(!s_lease.enabled) return false;
	g9_lock_t lock = g9_lock(); bool active = s_lease.active; g9_unlock(lock);
	return active;
}
void thread_g9_lease_pops(uint32_t pops) {
	if(!s_lease.enabled) return;
	g9_lock_t lock = g9_lock();
	if(lease_owner() && s_lease.boosted) s_lease.pops += pops;
	g9_unlock(lock);
}
void thread_g9_lease_local(bool present, uint8_t base_priority) {
	if(!s_lease.enabled) return;
	if(!present) thread_g9_lease_end();
	g9_lock_t lock = g9_lock();
	if(present) {
		assert(!s_lease_owner && stripe_thread_id() != s_lease_main);
		s_lease_base = base_priority;
		s_lease_owner = stripe_thread_id();
		s_lease_depth = 0;
		s_lease.cancelled = false;
	} else { assert(lease_owner()); s_lease_owner = 0; }
	g9_unlock(lock);
}
void thread_g9_lease_audio(uint32_t reserve_us, bool worker_pending) {
	if(!s_lease.enabled) return;
	assert(stripe_thread_id() == s_lease_main);
	g9_lock_t lock = g9_lock();
	s_lease_audio_at = lease_now();
	s_lease_audio_reserve = reserve_us;
	s_lease_audio_worker = worker_pending;
	s_lease_audio_valid = true;
	g9_unlock(lock);
}
void thread_g9_lease_begin(void) {
	if(!s_lease.enabled) return;
	g9_lock_t lock = g9_lock();
	if(s_handoff_enabled && lease_owner() && s_lease.generation == UINT32_MAX) {
		s_lease.failed = s_lease.cancelled = true;
		s_lease.stats.faults++;
		s_lease.stats.cancelled++;
		g9_unlock(lock);
		return;
	}
	uint32_t now = lease_now();
	uint32_t generation = s_lease.generation;
	bool begun = g9_lease_begin(&s_lease, lease_owner() && !s_lease.cancelled, true, now, lease_reserve(now),
		!s_lease_audio_valid || s_lease_audio_worker);
	if(generation != s_lease.generation) lease_event(1, begun ? 0 : 5, 0, begun, true);
	if(begun) {
		if(s_handoff_test_turn) { s_lease.turn = s_handoff_test_turn; s_handoff_test_turn = 0; }
		/* Missing/unprepared sender fails closed. The callback is IRQ-safe
 * and restores its OWN known base 65 at the common drain cleanup. */
		s_lease_sender_raised = s_lease_main_priority_ok && s_lease_base == 63
			&& s_lease_depth == 0 && s_lease_sender;
		/* Publish ownership before raising another runnable thread: it may
		 * stop/cancel this drain and clear the flag before the call returns. */
		if(s_lease_sender_raised) s_lease.stats.sender_sets++;
		bool prepared = s_lease_sender_raised && s_lease_sender(66);
		if(!prepared) {
			s_lease_sender_raised = false;
			s_lease.failed = true; s_lease.stats.faults++;
			lease_event(9, 7, 0, false, false);
		} else if(s_lease.active && !s_lease.cancelled && s_lease_sender_raised)
			lease_raise(lease_now());
	}
	g9_unlock(lock);
}
static void lease_cleanup(uint32_t now, uint32_t reason) {
	bool active = s_lease.active;
	bool boosted = s_lease.boosted, sender_raised = s_lease_sender_raised;
	g9_lease_end(&s_lease, now);
	s_lease_sender_raised = false;
#ifdef PLATFORM_WII
	KTickTaskStop(&s_lease_timer);
#endif
	if(boosted) lease_event(3, reason, now-s_lease.start, true, true);
	if(active) lease_event(8, reason, 0, true, true);
	/* The generation is terminal before either setter can switch threads. */
	/* Restore the sender first. Lowering the owner first could let MAIN join
	 * and free that native handle before this thread resumes its cleanup. */
	/* A running tick boost keeps its own sender raise (effective = max). */
	if(sender_raised && !s_tb.sender_raised) { s_lease.stats.sender_sets++; (void)s_lease_sender(65); }
	if(boosted) lease_priority(s_lease_base);
	lease_main_wake();
}
void thread_g9_lease_end(void) {
	if(!s_lease.enabled) return;
	g9_lock_t lock = g9_lock();
	if(lease_owner()) lease_cleanup(lease_now(), 1);
	g9_unlock(lock);
}
void thread_g9_lease_cancel(void) {
	if(!s_lease.enabled) return;
	g9_lock_t lock = g9_lock();
	if(!s_lease.cancelled) s_lease.stats.cancelled++;
	s_lease.cancelled = true;
	if(s_lease.active) lease_cleanup(lease_now(), 6);
	g9_unlock(lock);
}
void thread_g9_lease_dependency(void) {
	if(!s_lease.enabled) return;
	g9_lock_t lock = g9_lock();
	if(lease_owner()) {
		s_lease_depth++;
		s_lease.parked = true; /* Publish before the setter can run MAIN. */
		lease_drop(lease_now(), 2);
	}
	g9_unlock(lock);
}
static void lease_lock_held(void) {
	if(!s_lease.enabled) return;
	g9_lock_t lock = g9_lock();
	if(lease_owner()) s_lease_depth++;
	g9_unlock(lock);
}
void thread_g9_lease_dependency_done(void) {
	if(!s_lease.enabled) return;
	g9_lock_t lock = g9_lock();
	bool released = lease_owner() && s_lease_depth && --s_lease_depth == 0;
	if(released) s_lease.parked = false;
	bool resume = released && !s_lease.boosted && s_lease.active && !s_lease.failed;
	g9_unlock(lock);
	if(resume) thread_g9_lease_checkpoint();
}
static void lease_expire_ack(uint32_t now) {
	uint32_t stalls = s_lease.stats.stalls;
	g9_lease_expire_ack(&s_lease, now);
	if(stalls != s_lease.stats.stalls) lease_event(6, 0, now-s_lease.requested, false, false);
}
void thread_g9_lease_checkpoint(void) {
	if(!s_lease.enabled) return;
	for(;;) {
		g9_lock_t lock = g9_lock();
		if(!lease_owner() || !s_lease.active || s_lease.failed || s_lease_depth) {
			g9_unlock(lock);
			return;
		}
		uint32_t now = lease_now();
		if(now-s_lease.start >= s_lease.max_us) {
			bool boosted = s_lease.boosted;
			uint32_t turn = s_lease.turn;
			if(!lease_request(now)) { g9_unlock(lock); return; }
#ifdef PLATFORM_WII
			KTickTaskStop(&s_lease_timer);
#endif
			if(boosted) lease_event(3, 3, now-s_lease.start, true, true);
			if(turn != s_lease.turn) lease_event(4, 3, 0, true, true);
			if(boosted) lease_priority(s_lease_base);
			lease_main_wake();
		}
		/* The native drop may have run MAIN, including a successful ACK. */
		now = lease_now();
		lease_expire_ack(now);
		bool wait = s_lease.waiting && !s_lease.failed;
		if(!wait && !s_lease.failed) lease_raise(now);
		if(wait) s_lease.stats.sleep_requested_us += 1000;
		g9_unlock(lock);
		if(!wait) return;
		/* Actual blocking; MAIN cannot be donated to by priority-65 yield.
		 * Deadline includes time spent preempted, not just these sleeps. */
		thread_msleep_native(1); /* ACK wait is eligible, not a dependency park. */
	}
}
bool thread_g9_lease_defer(void) {
	if(!s_lease.enabled) return false;
	g9_lock_t lock = g9_lock();
	bool defer = stripe_thread_id() == s_lease_main && s_lease.active && !s_lease.failed
		&& !s_lease.parked && (!s_handoff_enabled || !s_lease_depth)
		&& (s_lease.boosted || s_lease.waiting);
#ifdef PLATFORM_WII
	if(s_handoff_enabled) {
		KThread* self = KThreadGetSelf();
		defer = defer && self->baseprio == KTHR_MIN_PRIO-64 && self->prio == self->baseprio;
	}
#endif
	g9_unlock(lock);
	return defer;
}
struct g9_lease_token thread_g9_lease_service_begin(void) {
	struct g9_lease_token token = {0};
	if(!s_lease.enabled) return token;
	assert(stripe_thread_id() == s_lease_main);
	g9_lock_t lock = g9_lock();
	lease_expire_ack(lease_now());
	if(s_lease.active && !s_lease.failed && s_lease.waiting && !s_lease.parked)
		token = (struct g9_lease_token){s_lease.generation, s_lease.turn};
	if(token.generation) lease_event(7, 0, 0, true, true);
	g9_unlock(lock);
	return token;
}
bool thread_g9_lease_service_end(struct g9_lease_token token,
	uint32_t duration_us, bool complete) {
	if(!s_lease.enabled) return false;
	assert(stripe_thread_id() == s_lease_main);
	g9_lock_t lock = g9_lock();
	uint32_t now = lease_now();
	bool valid = s_lease.active && s_lease.waiting && !s_lease.failed
		&& token.generation == s_lease.generation && token.turn == s_lease.turn;
	uint32_t audio_before = s_lease.stats.audio_reject;
	if(valid && complete && s_handoff_enabled && s_handoff_audio_guard) {
		uint32_t left = duration_us < s_lease.service_us ? s_lease.service_us-duration_us : 0;
		bool audio_safe = s_handoff_audio_guard(4000, left);
		uint32_t after = lease_now(), validation_us = after-now;
		duration_us = validation_us > UINT32_MAX-duration_us ? UINT32_MAX : duration_us+validation_us;
		now = after;
		if(!audio_safe) { complete = false; s_lease_audio_valid = false; }
	}
	bool late = now-s_lease.requested > s_lease.wait_us || duration_us > s_lease.service_us;
	bool ack = g9_lease_ack(&s_lease, token, now, duration_us,
		lease_reserve(now), !s_lease_audio_valid || s_lease_audio_worker, complete);
	uint32_t reason = !valid ? 8 : (late ? 1u : 0) | (!complete ? 2u : 0)
		| (s_lease.stats.audio_reject != audio_before ? 4u : 0);
	/* Preserve the ACK token even when stale; it may name the prior drain. */
	capture_lease(5, reason, token.generation, token.turn, duration_us, ack, complete, lease_flags(), 0);
	g9_unlock(lock);
	return ack;
}
void thread_g9_lease_snapshot(struct g9_lease_stats* out) {
	g9_lock_t lock = g9_lock(); *out = s_lease.stats; g9_unlock(lock);
}
void thread_g9_lease_state(struct g9_priority_lease* out, uint32_t* lock_depth) {
	g9_lock_t lock = g9_lock();
	*out = s_lease;
	if(lock_depth) *lock_depth = s_lease_depth;
	g9_unlock(lock);
}

void thread_g9_handoff_configure(bool enabled) {
	assert(!s_lease_owner && !s_lease.active);
	s_handoff_enabled = enabled;
	s_handoff_test_turn = 0;
	s_handoff_audio_guard = NULL;
	memset(&s_handoff, 0, sizeof(s_handoff));
	/* The R22 candidate never inherits R19's optional larger service cap. */
	if(enabled && s_lease.service_us > 500) s_lease.service_us = 500;
}
void thread_g9_handoff_test_priority(bool valid) {
	g9_lock_t lock = g9_lock(); s_handoff_priority_ok = valid; g9_unlock(lock);
}
bool thread_g9_handoff_enabled(void) {
	return s_handoff_enabled && thread_g9_lease_local_present();
}
bool thread_g9_handoff_test_sequence(uint32_t generation, uint32_t turn) {
	if(!s_lease_test_clock || s_lease_owner || s_lease.active) return false;
	s_lease.generation = generation;
	s_handoff_test_turn = turn;
	return true;
}
static bool handoff_token_equal(struct g9_lease_token a, struct g9_lease_token b) {
	return a.generation == b.generation && a.turn == b.turn;
}
static bool handoff_pending(void) {
	if(!thread_g9_handoff_enabled()) return false;
	assert(stripe_thread_id() == s_lease_main);
	g9_lock_t lock = g9_lock();
	lease_expire_ack(lease_now());
	bool safe = s_lease.active && s_lease.waiting && !s_lease.failed
		&& !s_lease.parked && !s_lease_depth && !s_lease.cancelled;
#ifdef PLATFORM_WII
	KThread* self = KThreadGetSelf();
	safe = safe && self->baseprio == KTHR_MIN_PRIO-64 && self->prio == self->baseprio;
#endif
	struct g9_lease_token token = {s_lease.generation, s_lease.turn};
	g9_unlock(lock);
	if(!safe) { s_handoff.token = (struct g9_lease_token){0}; return false; }
	if(!handoff_token_equal(s_handoff.token, token)) {
		memset(&s_handoff, 0, sizeof(s_handoff));
		s_handoff.entered = lease_now();
		s_handoff.token = thread_g9_lease_service_begin();
	}
	return s_handoff.token.generation != 0;
}
static bool lease_cpu_pending(struct g9_lease_token token) {
	return s_lease.active && !s_lease.failed && !s_lease.cancelled
		&& !s_lease.parked && !s_lease_depth && !s_lease.waiting
		&& s_lease.generation == token.generation && s_lease.turn == token.turn
		&& s_lease.ack == token.turn;
}
#ifdef PLATFORM_WII
static void lease_main_timeout(KTickTask* task) {
	(void)task;
	s_lease_main_timeout = true;
	lease_main_wake();
}
#endif
static void handoff_cpu_return(struct g9_lease_token token) {
	g9_lock_t lock = g9_lock();
	uint32_t start = s_lease.generation == token.generation && s_lease.ack == token.turn
		? s_lease.start : lease_now();
	g9_unlock(lock);
	g9_capture_main(1, true, G9_HANDOFF_ALL);
	lock = g9_lock();
	s_lease_main_timeout = false;
	if(lease_cpu_pending(token) && s_handoff_audio_guard) {
		uint32_t elapsed = lease_now()-start;
		uint32_t wait = elapsed < s_lease.max_us ? s_lease.max_us-elapsed : 0;
		uint32_t used = lease_now()-s_handoff.entered;
		if(s_handoff.used_us > used) used = s_handoff.used_us;
		uint32_t left = used < s_lease.service_us ? s_lease.service_us-used : 0;
		if(!s_handoff_audio_guard(wait, left)) {
			s_lease.failed = true;
			s_lease.stats.faults++;
			lease_drop(lease_now(), 4);
		}
	}
	if(lease_cpu_pending(token)) {
		s_lease.stats.main_sleep_requested_us += s_lease.max_us;
#ifdef PLATFORM_WII
		/* Same atomic predicate/enqueue sequence as Tuxedo SleepTicks. IRQ
		 * exclusion is released by the context switch; no mutex is held and
		 * no priority inheritance is possible on this private event queue. */
		uint32_t elapsed = lease_now()-start;
		s_lease_main_timeout = elapsed >= s_lease.max_us;
		if(!s_lease_main_timeout) KTickTaskStart(&s_lease_main_timer, lease_main_timeout,
			(uint64_t)(s_lease.max_us-elapsed) * G9_TB_HZ / 1000000u, 0);
		while(lease_cpu_pending(token) && !s_lease_main_timeout)
			(void)KThrQueueBlock(&s_lease_main_queue, token.generation);
		KTickTaskStop(&s_lease_main_timer);
#else
		while(lease_cpu_pending(token) && !s_lease_main_timeout) {
			uint32_t elapsed = lease_now()-start;
			if(elapsed >= s_lease.max_us) { s_lease_main_timeout = true; break; }
			struct timespec deadline;
			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_nsec += (long)(s_lease.max_us-elapsed) * 1000;
			if(deadline.tv_nsec >= 1000000000) { deadline.tv_nsec -= 1000000000; deadline.tv_sec++; }
			(void)pthread_cond_timedwait(&s_lease_main_wake, &s_g9_lock, &deadline);
			/* Lease deadlines use the same injected/native monotonic clock as
			 * ACKs. A host OS wake delay is not elapsed injected fixture time. */
			s_lease_main_timeout = lease_now()-start >= s_lease.max_us;
		}
#endif
		if(s_lease_main_timeout && lease_cpu_pending(token)) {
			s_lease.failed = true; s_lease.stats.faults++;
			lease_event(9, 1, lease_now()-start, false, false);
			lease_drop(lease_now(), 4);
		}
	}
	uint32_t elapsed = lease_now()-start;
	if(elapsed > s_lease.max_us && elapsed-s_lease.max_us > s_lease.stats.overshoot_us)
		s_lease.stats.overshoot_us = elapsed-s_lease.max_us;
	g9_unlock(lock);
	g9_capture_main(1, false, elapsed);
}
static bool handoff_finish(bool complete) {
	struct g9_lease_token token = s_handoff.token;
	/* Charge elapsed cursor lifetime as well as essential operations. This
	 * conservative candidate includes dispatch/capture/entry overhead and
	 * intervening frame work, rather than hiding it outside the 500-us cap. */
	uint32_t elapsed = lease_now()-s_handoff.entered;
	uint32_t duration = elapsed > s_handoff.used_us ? elapsed : s_handoff.used_us;
	bool accepted = thread_g9_lease_service_end(token, duration, complete);
	s_handoff.token = (struct g9_lease_token){0};
	if(accepted) handoff_cpu_return(token);
	return accepted;
}
struct g9_lease_token thread_g9_handoff_work_begin(unsigned operation) {
	if(!operation || operation > G9_HANDOFF_AUDIO || (operation & (operation-1)))
		return (struct g9_lease_token){0};
	if(!handoff_pending() || s_handoff.operation || (s_handoff.done & operation))
		return (struct g9_lease_token){0};
	if(lease_now()-s_handoff.entered >= s_lease.service_us) {
		(void)handoff_finish(false);
		return (struct g9_lease_token){0};
	}
#ifdef PLATFORM_WII
	/* Input remains S2-owned/unadmitted. R18 audio has a finite cursor,
	 * but its unknown native E/margins still deny completion/wait credit. */
	if(operation == G9_HANDOFF_INPUT) {
		g9_capture_main(1, true, operation);
		g9_capture_main(1, false, G9_HANDOFF_UNSAFE);
		(void)handoff_finish(false);
		return (struct g9_lease_token){0};
	}
#endif
	s_handoff.operation = operation;
	s_handoff.operation_start = lease_now();
	g9_capture_main(1, true, operation);
	return s_handoff.token;
}
uint32_t thread_g9_handoff_audio_remaining(void) {
	if(!s_handoff.token.generation || s_handoff.operation != G9_HANDOFF_AUDIO) return 0;
	uint32_t elapsed = lease_now()-s_handoff.entered;
	uint32_t used = elapsed > s_handoff.used_us ? elapsed : s_handoff.used_us;
	uint32_t cap = s_lease.service_us < 500u ? s_lease.service_us : 500u;
	return used < cap ? cap-used : 0;
}
void thread_g9_handoff_audio_abort(void) {
	if(s_handoff.token.generation && !s_handoff.operation) (void)handoff_finish(false);
}
void thread_g9_handoff_audio_guard(bool (*validate)(uint32_t proposed_wait_us, uint32_t remaining_service_us)) {
	g9_lock_t lock = g9_lock();
	s_handoff_audio_guard = validate;
	g9_unlock(lock);
}
void thread_g9_handoff_work_end(struct g9_lease_token token,
	unsigned operation, bool complete) {
	if(!token.generation || !handoff_token_equal(token, s_handoff.token)
	   || operation != s_handoff.operation) return;
	uint32_t duration = lease_now()-s_handoff.operation_start;
	s_handoff.operation = 0;
	g9_capture_main(1, false, complete ? G9_HANDOFF_DONE : G9_HANDOFF_MORE);
	s_handoff.used_us = duration > UINT32_MAX-s_handoff.used_us
		? UINT32_MAX : s_handoff.used_us+duration;
	if(complete) s_handoff.done |= operation;
	if(s_handoff.used_us > s_lease.service_us) (void)handoff_finish(false);
}
bool thread_g9_handoff_dispatch(unsigned capabilities, g9_handoff_service service) {
	if(!handoff_pending()) return false;
	/* An earlier empty queue is not a witness for messages/results that
	 * arrived while other essential work ran. Recheck at the final safe seam. */
	s_handoff.done &= ~(capabilities & (G9_HANDOFF_RPC|G9_HANDOFF_MESH));
	/* At most one retained quantum of each essential consumer per seam. The
	 * production callbacks reject indivisible/unproven RPC and recovery work. */
	for(unsigned op = G9_HANDOFF_RPC; op <= G9_HANDOFF_MESH; op <<= 1) {
		if(!(capabilities & op) || (s_handoff.done & op) || !service) continue;
		struct g9_lease_token token = thread_g9_handoff_work_begin(op);
		if(!token.generation) return false;
		uint32_t elapsed = lease_now()-s_handoff.entered;
		uint32_t used = elapsed > s_handoff.used_us ? elapsed : s_handoff.used_us;
		uint32_t left = used < s_lease.service_us ? s_lease.service_us-used : 0;
		enum g9_handoff_result result = service(op, left);
		thread_g9_handoff_work_end(token, op, result == G9_HANDOFF_DONE);
		if(!s_handoff.token.generation) return false;
		if(result == G9_HANDOFF_UNSAFE) {
			g9_capture_main(1, true, op);
			g9_capture_main(1, false, G9_HANDOFF_UNSAFE);
			(void)handoff_finish(false);
			return false;
		}
		if(!handoff_pending()) return false;
	}
	if(s_handoff.done == G9_HANDOFF_ALL
	   && (capabilities & (G9_HANDOFF_RPC|G9_HANDOFF_MESH))
		  == (G9_HANDOFF_RPC|G9_HANDOFF_MESH)) return handoff_finish(true);
	if(s_handoff.used_us >= s_lease.service_us) (void)handoff_finish(false);
	return false;
}

/* ===== G9 tick-wide boost (T2, opt-in; docs/g9_tick_boost.md) =============
 * Beta 1.7.3 singleplayer ran the world tick and rendering on ONE thread: a
 * heavy tick delayed frames, never the tick. This is the scheduling
 * equivalent on Broadway's strict-priority single core: the local server runs
 * above MAIN (64) for one gameplay tick and returns to its base (63) at the
 * tick's real end or at the hard per-tick cap, whichever comes first. Only
 * WHEN the server runs changes; the ordered work and every value are the
 * same. Remote/no-owner: nothing is registered, begin is a counted no-op. */
#include "service_observer.h"
static void tb_priority(uint8_t priority) {
	s_tb.stats.priority_sets++;
#ifdef PLATFORM_WII
	LWP_SetThreadPriority((lwp_t)s_tb.owner, priority);
#else
	(void)priority; /* PC's preemptive multi-core scheduler: no knob. */
#endif
}
static bool tb_owner(void) { return s_tb.owner && s_tb.owner == stripe_thread_id(); }
/* Caller holds g9_lock (or runs under the SDK IRQ lock). Commit every visible
 * transition BEFORE a setter can switch threads. */
static void tb_drop(uint32_t now, bool capped) {
	if(!s_tb.boosted) return;
	uint32_t us = now-s_tb.start;
	SERVICE_OBSERVER(service_observer_boost(capped ? 2u : 1u, now));
	s_tb.boosted = false;
	s_tb.stats.last_us = us;
	if(us > s_tb.stats.max_us) s_tb.stats.max_us = us;
	if(capped) {
		s_tb.stats.capped++;
		__atomic_store_n(&s_tb.cap_at, now, __ATOMIC_RELAXED);
		__atomic_fetch_add(&s_tb.cap_seq, 1u, __ATOMIC_RELEASE);
	} else if(us > s_tb.cap_us) s_tb.stats.over_cap++; /* host: no cap timer */
	bool sender = s_tb.sender_raised;
	s_tb.sender_raised = false;
#ifdef PLATFORM_WII
	KTickTaskStop(&s_tb_timer);
#endif
	/* Sender first (lease_cleanup's ordering), then this thread's own
	 * priority: max(the lease's current state, base). */
	if(sender && !s_lease_sender_raised && s_lease_sender) {
		s_tb.stats.sender_sets++;
		(void)s_lease_sender(65);
	}
	bool lease_holds = s_lease.boosted && s_lease_owner == s_tb.owner;
	tb_priority(lease_holds ? 65u : s_tb.base);
}
#ifdef PLATFORM_WII
static void tb_timer_expire(KTickTask* task) {
	(void)task;
	tb_drop(lease_now(), true); /* the hard cap: frames/input/audio resume */
}
#endif
void thread_g9_tick_boost_configure(bool enabled, int cap_ms) {
	assert(!s_tb.owner && !s_tb.boosted);
	int ms = cap_ms < 1 ? 1 : cap_ms > 250 ? 250 : cap_ms;
	s_tb.enabled = enabled;
	s_tb.cap_us = (uint32_t)ms * 1000u;
	s_tb.sender_raised = false;
	memset(&s_tb.stats, 0, sizeof s_tb.stats);
}
bool thread_g9_tick_boost_enabled(void) { return s_tb.enabled; }
bool thread_g9_tick_boost_local_present(void) {
	if(!s_tb.enabled) return false;
	g9_lock_t lock = g9_lock();
	bool present = s_tb.owner != 0;
	g9_unlock(lock);
	return present;
}
void thread_g9_tick_boost_local(bool present, uint8_t base_priority) {
	if(!s_tb.enabled) return;
	g9_lock_t lock = g9_lock();
	if(present) {
		assert(!s_tb.owner && stripe_thread_id() != s_lease_main);
		s_tb.owner = stripe_thread_id();
		s_tb.base = base_priority;
	} else if(tb_owner()) {
		tb_drop(lease_now(), false);
		s_tb.owner = 0;
	}
	g9_unlock(lock);
}
void thread_g9_tick_boost_begin(void) {
	if(!s_tb.enabled) return;
	g9_lock_t lock = g9_lock();
	if(!tb_owner() || s_tb.boosted) {
		s_tb.stats.rejected++;
		g9_unlock(lock);
		return;
	}
	s_tb.boosted = true;
	s_tb.start = lease_now();
	SERVICE_OBSERVER(service_observer_boost(0, s_tb.start));
	s_tb.stats.ticks++;
#ifdef PLATFORM_WII
	KTickTaskStart(&s_tb_timer, tb_timer_expire,
		(uint64_t)s_tb.cap_us * G9_TB_HZ / 1000000u, 0);
#endif
	/* Keep the telemetry sender (65) able to preempt a boosted server, as the
	 * lease does. A refusing/stopping sender does not block the tick boost. */
	if(s_lease_sender_raised) s_tb.sender_raised = true;
	else if(s_lease_sender) {
		s_tb.stats.sender_sets++;
		s_tb.sender_raised = s_lease_sender(G9_TICK_BOOST_SENDER_PRIORITY);
		if(!s_tb.sender_raised) s_tb.stats.sender_fail++;
	}
	if(s_tb.boosted) tb_priority(G9_TICK_BOOST_PRIORITY);
	g9_unlock(lock);
}
void thread_g9_tick_boost_end(void) {
	if(!s_tb.enabled) return;
	g9_lock_t lock = g9_lock();
	if(tb_owner()) tb_drop(lease_now(), false);
	g9_unlock(lock);
}
void thread_g9_tick_boost_snapshot(struct g9_tick_boost_stats* out, bool* boosted) {
	g9_lock_t lock = g9_lock();
	*out = s_tb.stats;
	if(boosted) *boosted = s_tb.boosted;
	g9_unlock(lock);
}
