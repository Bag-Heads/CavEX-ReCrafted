/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef NETWORK_RESIDENCY_H
#define NETWORK_RESIDENCY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define RESIDENCY_ANCHORS 2
/* Two complete radius-16 squares, without truncation. Each square has at
 * most one coordinate in each modulo-33 bucket; two ways hold their union. */
#define RESIDENCY_EDGE 33
#define RESIDENCY_BUCKETS (RESIDENCY_EDGE * RESIDENCY_EDGE)
#define RESIDENCY_AGE_TICKS 20u
/* A1c (split-dimensions-plan-0924, PLAN-v3 s.2): a residency anchor is
 * (dimension, x, z). `dimension` is an `enum world_dim` value (world.h:
 * Overworld 0, Nether -1) stored as int32_t so this header stays free of the
 * world headers. It is the LAST member on purpose: every existing `{x, z}`
 * initializer keeps meaning "Overworld", which is what every anchor was
 * before the field existed. */
struct residency_anchor {
	int32_t x, z;
	int32_t dimension;
};
struct residency_debt {
	int32_t x, z;
	/* A1c: the dimension this column debt belongs to. Rows are keyed
	 * (dimension, x, z): the same column coordinate in two dimensions is two
	 * rows (each served through its own world's sent-state). */
	int32_t dimension;
	uint32_t first_wait, attempted, retry;
	uint8_t near_owners, far_owners;
	bool used, attempted_once;
};
struct residency_scheduler {
	struct residency_debt debt[RESIDENCY_BUCKETS][RESIDENCY_ANCHORS];
	uint32_t epoch;
	unsigned next_near, next_far;
};
/* ===== residency-1005/2: per-actor player residency (ASTRA-RESIDENCY-REVIEW
 * items 1-7). One record per player body (host, guest).
 *
 * pin      the last accepted pose whose collision/hazard footprint was
 *          resident in the body's own world instance (world_index +
 *          generation). Restored, without replaying movement side effects,
 *          when the body is found on an UNKNOWN footprint; protected from
 *          eviction while it names the body's live world.
 * req[]    bounded, coalesced DESTINATION REQUESTS: columns this actor needs
 *          next, by priority -- a refused footprint or a recovery/
 *          destination column (RZ_PRIO_NEED) before the directional
 *          load-ahead along the measured velocity (RZ_PRIO_LEAD). A request
 *          is a loader PRIORITY, never residency: admission still reads
 *          only resident columns. Requests are served round-robin between
 *          the two actors (a guest 256 blocks away keeps its share), only
 *          inside the actor's own streaming ring (the existing memory
 *          budget), and are dropped once resident, stale or out of reach.
 * recovery the bounded per-actor barrier for a body with no restorable pose:
 *          no hazard/portal tick on unknown data, its footprint requested at
 *          RZ_PRIO_NEED, a deadline, and an explicit retryable failure.
 * counters diagnostics only (telemetry + tests). */
#define RZ_REQ_MAX 6u
enum rz_prio {
	RZ_PRIO_NONE = 0,
	RZ_PRIO_LEAD = 1, /* directional load-ahead */
	RZ_PRIO_NEED = 2, /* refused footprint / recovery / owned destination */
};
struct rz_request {
	int32_t cx, cz;
	uint32_t world_generation;
	uint32_t born;		/* near_send_seq when first requested */
	uint32_t attempted; /* near_send_seq of the last loader attempt */
	uint8_t world_index;
	uint8_t prio;
	bool used, attempted_once;
	bool cancelled; /* terminal request receipt only; never schedules work */
};
struct rz_actor {
	struct {
		double x, y, z;
		uint32_t world_generation;
		uint8_t world_index;
		bool valid;
	} pin;
	struct rz_request req[RZ_REQ_MAX];
	/* velocity estimate from admitted samples (blocks per server tick) */
	double last_x, last_z;
	uint32_t last_seq;
	float vx, vz;
	bool have_last;
	/* recovery barrier: `recovering` = the body is HELD (RECOVERING or
	 * FAILED); `failed` = the bounded attempt hit its deadline and ended in a
	 * visible retryable failure (no automatic re-arm; only the owner's next
	 * fresh movement sample starts one new attempt). */
	bool recovering;
	bool failed;
	uint32_t recover_start, recover_deadline, failed_at;
	/* rate-limited HUD feedback */
	uint32_t notice_seq;
	bool notice_armed;
	bool notice_fail_pending; /* the terminal failure toast is owed */
	/* closure item 5: a woken body whose step-out scan columns are not all
	 * resident waits (held, bounded) for them before stepping out */
	bool wake_pending;
	bool wake_notice_pending;
	uint32_t wake_deadline;
	bool streak_fail_noticed; /* a too-long refusal streak was reported */
	/* counters */
	uint32_t rejects;		 /* refused fresh samples (all) */
	uint32_t far_refusals;	 /* refused, footprint beyond request reach */
	uint32_t restores;		 /* already-UNKNOWN body restored to its pin */
	uint32_t streak;		 /* consecutive refused fresh samples */
	uint32_t streak_max;
	uint32_t streak_start;	 /* near_send_seq of the streak's first refusal */
	uint32_t latency_last;	 /* ticks, first refusal -> next admission */
	uint32_t latency_max;
	uint32_t recoveries;	 /* recovery barriers entered */
	uint32_t recovery_ok;	 /* ...left with a resident footprint */
	uint32_t recovery_failures; /* ...that hit their deadline */
	uint32_t recovery_ticks_max;
	uint32_t retries;		 /* owner-started fresh attempts after a failure */
	uint32_t requests;		 /* destination requests created */
	uint32_t picked;		 /* requests picked by a loader (attempts) */
	uint32_t loaded;		 /* requests satisfied: their column became resident */
	uint32_t service_delay_max; /* ticks, request born -> column resident */
	uint32_t evict_vetoes;	 /* evictions refused for this pin */
	uint32_t notices;		 /* HUD notices sent */
};

/* Dimension-BLIND containment (the pre-A1c predicate, kept for the callers
 * that operate on one explicit world already -- server_world.c's
 * furthest-chunk scan, carve_observer -- and for fixtures). Inside
 * server_local.c every world-qualified path uses residency_contains_in. */
static inline bool residency_contains(const struct residency_anchor* a,
									  size_t count, int32_t radius, int32_t x,
									  int32_t z) {
	for(size_t p = 0; p < count; p++)
		if(llabs((int64_t)a[p].x - x) <= radius
		   && llabs((int64_t)a[p].z - z) <= radius)
			return true;
	return false;
}
/* A1c: containment in ONE dimension -- an anchor in another dimension never
 * covers (x, z), whatever its coordinates. */
static inline bool residency_contains_in(const struct residency_anchor* a,
										 size_t count, int32_t radius,
										 int32_t dimension, int32_t x,
										 int32_t z) {
	for(size_t p = 0; p < count; p++)
		if(a[p].dimension == dimension && llabs((int64_t)a[p].x - x) <= radius
		   && llabs((int64_t)a[p].z - z) <= radius)
			return true;
	return false;
}
#endif
