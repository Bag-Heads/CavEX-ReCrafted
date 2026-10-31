/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef GAME_SOAK_IDLE_PLAY_CORE_H
#define GAME_SOAK_IDLE_PLAY_CORE_H

#include <stdbool.h>
#include <stdint.h>

/*
	REL-16a (SPEC R16): pure timer/phase core of the soak_idle_play profile:
	one process and one world alternate idle and controller-driven play for
	at least three continuous hours (tmp/jobs/release-plan/REL-16a/CONTRACT.md).
	Test driver only, no simulation value; the wiring is REL-16-wire.
	Restated existing values: release-frame memory floors
	(tools/wii_telemetry_server.py:9097) and the 5 s heartbeat stale timeout.
	Measured thresholds arrive in struct soak_ip_limits; a zero field fails
	closed (UNAPPROVED_LIMITS). Failures are sticky. Phase changes never touch
	the leak baseline and a second soak_ip_begin is refused, so nothing can
	restart the clock or erase memory history. A trailing partial segment is
	not graded.
*/

#define SOAK_IP_MIN_DURATION_S 10800u /* contract: >= 3 continuous hours */
#define SOAK_IP_MEM1_FLOOR_KB 4096u
#define SOAK_IP_CARVE_FLOOR_KB 2048u
#define SOAK_IP_HB_MAX_GAP_S 5u

enum soak_ip_phase { SOAK_IP_IDLE = 0, SOAK_IP_PLAY };

/* PROCESS_RESET: process/world id, uptime, seq or a cumulative counter went
   back. WATCHDOG: any dump in the soak. MEMORY_FLOOR: below a floor or OOM.
   NO_PROGRESS: a play segment short of movement or chunk activity, counted
   only between in-play samples. LEAK: an idle-segment end more than
   max_drop_kb below the post-warmup idle baseline. NO_MEMORY_HISTORY: the run
   never set that baseline and compared a later idle end against it. */
enum soak_ip_fail {
	SOAK_IP_PASS = 0, SOAK_IP_FAIL_UNAPPROVED_LIMITS, SOAK_IP_FAIL_PROCESS_RESET,
	SOAK_IP_FAIL_TELEMETRY_GAP, SOAK_IP_FAIL_CRASH, SOAK_IP_FAIL_WATCHDOG,
	SOAK_IP_FAIL_MEMORY_FLOOR, SOAK_IP_FAIL_NO_PROGRESS, SOAK_IP_FAIL_LEAK,
	SOAK_IP_FAIL_MISSING_PHASE, SOAK_IP_FAIL_SHORT, SOAK_IP_FAIL_NO_MEMORY_HISTORY,
};

/* From the approved measured baseline (CONTRACT.md); never chosen after a run. */
struct soak_ip_limits {
	uint32_t idle_s, play_s, warmup_s;
	uint32_t min_play_blocks, min_play_chunks, max_drop_kb;
};

/* One heartbeat. moved_blocks and chunks_loaded are cumulative counters. */
struct soak_ip_sample {
	uint32_t uptime_s, process_id, world_id, hb_seq;
	uint32_t mem1_free_kb, carve_free_kb, moved_blocks, chunks_loaded;
	bool crashed, watchdog_fired, oom;
};

struct soak_ip_state {
	bool started, have_prev, have_base;
	enum soak_ip_fail fail;
	struct soak_ip_limits lim;
	struct soak_ip_sample prev, seg_start;
	uint32_t start_s, last_uptime_s, fail_uptime_s, idle_segments, play_segments;
	uint32_t mem1_base_kb, carve_base_kb, leak_checks;
};

static inline enum soak_ip_phase soak_ip_phase_at(const struct soak_ip_state* st,
		uint32_t uptime_s) {
	uint32_t cycle = st->lim.idle_s + st->lim.play_s;
	if(!cycle || uptime_s < st->start_s)
		return SOAK_IP_IDLE;
	return (uptime_s - st->start_s) % cycle < st->lim.idle_s ? SOAK_IP_IDLE
															 : SOAK_IP_PLAY;
}

/* Starts the one continuous run. A second call is refused and changes nothing. */
static inline bool soak_ip_begin(struct soak_ip_state* st,
		const struct soak_ip_limits* lim, uint32_t uptime_s) {
	const struct soak_ip_state zero = {0};
	if(!st || !lim || st->started)
		return false;
	*st = zero;
	st->started = true;
	st->lim = *lim;
	st->start_s = st->last_uptime_s = uptime_s;
	if(!lim->idle_s || !lim->play_s || !lim->warmup_s || !lim->min_play_blocks
	   || !lim->min_play_chunks || !lim->max_drop_kb)
		st->fail = SOAK_IP_FAIL_UNAPPROVED_LIMITS;
	return true;
}

static inline void soak_ip_set_fail(struct soak_ip_state* st,
		enum soak_ip_fail f, uint32_t uptime_s) {
	if(st->fail == SOAK_IP_PASS) {
		st->fail = f;
		st->fail_uptime_s = uptime_s;
	}
}

/* Grades the segment whose last sample is st->prev. A play segment counts only
   from its first in-play sample (seg_start) to its last (prev), so counter
   increases inside a boundary interval that is partly idle never count. */
static inline void soak_ip_close_segment(struct soak_ip_state* st) {
	const struct soak_ip_sample* p = &st->prev;
	if(soak_ip_phase_at(st, p->uptime_s) == SOAK_IP_PLAY) {
		st->play_segments++;
		if(p->moved_blocks - st->seg_start.moved_blocks < st->lim.min_play_blocks
		   || p->chunks_loaded - st->seg_start.chunks_loaded
			   < st->lim.min_play_chunks)
			soak_ip_set_fail(st, SOAK_IP_FAIL_NO_PROGRESS, p->uptime_s);
		return;
	}
	st->idle_segments++;
	if(p->uptime_s - st->start_s < st->lim.warmup_s)
		return;
	if(!st->have_base) {
		st->have_base = true;
		st->mem1_base_kb = p->mem1_free_kb;
		st->carve_base_kb = p->carve_free_kb;
		return;
	}
	st->leak_checks++;
	if(st->mem1_base_kb > p->mem1_free_kb + st->lim.max_drop_kb
	   || st->carve_base_kb > p->carve_free_kb + st->lim.max_drop_kb)
		soak_ip_set_fail(st, SOAK_IP_FAIL_LEAK, p->uptime_s);
}

static inline void soak_ip_observe(struct soak_ip_state* st,
		const struct soak_ip_sample* s) {
	const struct soak_ip_sample* p;
	if(!st || !st->started || !s || st->fail != SOAK_IP_PASS)
		return;
	p = &st->prev;
	if(st->have_prev
	   && (s->process_id != p->process_id || s->world_id != p->world_id
		   || s->uptime_s <= p->uptime_s || s->hb_seq <= p->hb_seq
		   || s->moved_blocks < p->moved_blocks
		   || s->chunks_loaded < p->chunks_loaded))
		soak_ip_set_fail(st, SOAK_IP_FAIL_PROCESS_RESET, s->uptime_s);
	else if(s->uptime_s < st->start_s
			|| s->uptime_s - st->last_uptime_s > SOAK_IP_HB_MAX_GAP_S)
		soak_ip_set_fail(st, SOAK_IP_FAIL_TELEMETRY_GAP, s->uptime_s);
	else if(s->crashed)
		soak_ip_set_fail(st, SOAK_IP_FAIL_CRASH, s->uptime_s);
	else if(s->watchdog_fired)
		soak_ip_set_fail(st, SOAK_IP_FAIL_WATCHDOG, s->uptime_s);
	else if(s->oom || s->mem1_free_kb < SOAK_IP_MEM1_FLOOR_KB
			|| s->carve_free_kb < SOAK_IP_CARVE_FLOOR_KB)
		soak_ip_set_fail(st, SOAK_IP_FAIL_MEMORY_FLOOR, s->uptime_s);
	if(st->fail != SOAK_IP_PASS)
		return;
	if(!st->have_prev)
		st->seg_start = *s;
	else if(soak_ip_phase_at(st, s->uptime_s)
			!= soak_ip_phase_at(st, p->uptime_s)) {
		soak_ip_close_segment(st);
		st->seg_start = *s; /* first sample inside the new phase */
	}
	st->prev = *s;
	st->have_prev = true;
	st->last_uptime_s = s->uptime_s;
}

static inline enum soak_ip_fail soak_ip_finish(const struct soak_ip_state* st) {
	if(!st || !st->started)
		return SOAK_IP_FAIL_MISSING_PHASE;
	if(st->fail != SOAK_IP_PASS)
		return st->fail;
	if(!st->idle_segments || !st->play_segments)
		return SOAK_IP_FAIL_MISSING_PHASE;
	if(st->last_uptime_s - st->start_s < SOAK_IP_MIN_DURATION_S)
		return SOAK_IP_FAIL_SHORT;
	if(!st->have_base || !st->leak_checks)
		return SOAK_IP_FAIL_NO_MEMORY_HISTORY;
	return SOAK_IP_PASS;
}

#endif
