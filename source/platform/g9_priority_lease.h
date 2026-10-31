#ifndef G9_PRIORITY_LEASE_H
#define G9_PRIORITY_LEASE_H

#include <stdbool.h>
#include <stdint.h>

/* T2 scheduling only. All times are monotonic microseconds modulo 2^32;
 * intervals are below INT32_MAX. The caller serializes this state. A token
 * identifies BOTH the logical drain and its service turn, including wrap. */
struct g9_lease_token { uint32_t generation, turn; };
struct g9_lease_stats {
	uint32_t acquired, restored, rejected, services, late_ack, stalls;
	uint32_t audio_reject, cancelled, timer_restores, lease_max_us;
	uint32_t service_max_us, overshoot_us, reserve_min_us, faults;
	uint32_t priority_sets, sender_sets, lease_pops, pops_max, pops_last;
	uint32_t sleep_requested_us, main_sleep_requested_us;
};
/* G9 tick-wide boost (T2, opt-in, local/split server only; docs/g9_tick_boost.md).
 * Counters only; no simulation state. ticks = boosts begun, capped = boosts
 * ended by the per-tick cap timer, rejected = begin calls refused (no
 * registered local owner / wrong thread / already boosted). */
#define G9_TICK_BOOST_PRIORITY 65u
#define G9_TICK_BOOST_SENDER_PRIORITY 66u
struct g9_tick_boost_stats {
	uint32_t ticks, capped, rejected, max_us, last_us, over_cap;
	uint32_t sender_sets, sender_fail, priority_sets;
};
struct g9_priority_lease {
	bool enabled, active, boosted, failed, waiting, parked, cancelled;
	uint32_t pops;
	uint32_t generation, turn, ack, start, requested;
	uint32_t max_us, service_us, wait_us;
	struct g9_lease_stats stats;
};
static inline uint32_t g9_lease_elapsed(uint32_t a, uint32_t b) { return b-a; }
static inline bool g9_lease_audio_ok(struct g9_priority_lease* s,
	uint32_t reserve_us, bool worker_pending) {
	if(reserve_us < s->stats.reserve_min_us) s->stats.reserve_min_us = reserve_us;
	/* Two conservative milliseconds of reserve margin, plus the ACK deadline.
	 * Reserve is already the device's conservative queued duration, not a
	 * count of decoded chunks waiting for MAIN to hand them to the device. */
	if(worker_pending || reserve_us <= s->max_us + s->wait_us + 2000u) {
		s->stats.audio_reject++;
		return false;
	}
	return true;
}
static inline void g9_lease_restore(struct g9_priority_lease* s, uint32_t now) {
	if(!s->boosted) return;
	uint32_t us = g9_lease_elapsed(s->start, now);
	if(us > s->stats.lease_max_us) s->stats.lease_max_us = us;
	if(us > s->max_us && us-s->max_us > s->stats.overshoot_us)
		s->stats.overshoot_us = us-s->max_us;
	s->boosted = false;
	s->stats.restored++;
	s->stats.lease_pops += s->pops;
	s->stats.pops_last = s->pops;
	if(s->pops > s->stats.pops_max) s->stats.pops_max = s->pops;
}
static inline void g9_lease_request(struct g9_priority_lease* s, uint32_t now) {
	g9_lease_restore(s, now);
	if(!s->active || s->failed || s->waiting) return;
	if(++s->turn == 0) ++s->turn;
	s->waiting = true;
	s->requested = now;
}
static inline bool g9_lease_begin(struct g9_priority_lease* s, bool local,
	bool work, uint32_t now, uint32_t reserve_us, bool worker_pending) {
	if(!s->enabled || !local || !work || s->active) {
		if(s->enabled) s->stats.rejected++;
		return false;
	}
	if(++s->generation == 0) ++s->generation;
	s->active = true;
	s->failed = !g9_lease_audio_ok(s, reserve_us, worker_pending);
	s->waiting = false;
	s->turn = s->ack = 0;
	s->start = now;
	return !s->failed;
}
static inline bool g9_lease_boost(struct g9_priority_lease* s, uint32_t now) {
	if(!s->active || s->failed || s->waiting) return false;
	if(g9_lease_elapsed(s->start, now) >= s->max_us) {
		g9_lease_request(s, now);
		return false;
	}
	if(!s->boosted) { s->boosted = true; s->pops = 0; s->stats.acquired++; }
	return true;
}
static inline bool g9_lease_ack(struct g9_priority_lease* s,
	struct g9_lease_token token, uint32_t now, uint32_t service_us,
	uint32_t reserve_us, bool worker_pending, bool complete) {
	if(!s->active || !s->waiting || s->failed
	   || token.generation != s->generation || token.turn != s->turn) return false;
	if(service_us > s->stats.service_max_us) s->stats.service_max_us = service_us;
	bool timely = g9_lease_elapsed(s->requested, now) <= s->wait_us
		&& service_us <= s->service_us;
	bool audio = g9_lease_audio_ok(s, reserve_us, worker_pending);
	if(!timely || !complete || !audio) {
		s->failed = true;
		if(!timely) s->stats.late_ack++;
		if(!complete) s->stats.faults++;
		return false;
	}
	s->ack = token.turn;
	s->waiting = false;
	s->start = now;
	s->stats.services++;
	return true;
}
static inline void g9_lease_expire_ack(struct g9_priority_lease* s, uint32_t now) {
	if(s->active && s->waiting && !s->failed
	   && g9_lease_elapsed(s->requested, now) > s->wait_us) {
		s->failed = true;
		s->stats.stalls++;
	}
}
static inline void g9_lease_end(struct g9_priority_lease* s, uint32_t now) {
	g9_lease_restore(s, now);
	s->active = s->waiting = false;
}
#endif
