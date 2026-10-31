/* Pure decision seam for checklist 6b. The device supplies snapshots of the
 * production server's dwell/switch state; this code never advances a portal. */
#ifndef GAME_SPLIT_PORTAL_QUIT_CORE_H
#define GAME_SPLIT_PORTAL_QUIT_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum spq_winner { SPQ_UNDECIDED, SPQ_UNLOAD_FIRST, SPQ_SWITCH_FIRST };
struct spq_observation {
	int remaining;
	uint32_t tick, up_ms;
	bool switch_started;
	bool save_settled;
};
struct spq_core {
	int target;
	bool requested;
	enum spq_winner winner;
	uint32_t request_tick, request_up_ms;
};
struct spq_decision {
	bool request_quit;
	enum spq_winner winner;
};
static inline bool spq_after_confirm(uint32_t confirm_tick,
	uint32_t confirm_ms, uint32_t switch_tick, uint32_t switch_ms) {
	return switch_tick > confirm_tick && switch_ms > confirm_ms;
}
enum spq_unload_resolution {
	SPQ_UNLOAD_WAITING, SPQ_UNLOAD_REPLIED,
	SPQ_UNLOAD_REPLIED_LATE, SPQ_UNLOAD_DROPPED
};
static inline enum spq_unload_resolution spq_resolve_unload(
	bool deadline_elapsed, bool reply_seen, bool run_ended) {
	if(reply_seen) return deadline_elapsed
		? SPQ_UNLOAD_REPLIED_LATE : SPQ_UNLOAD_REPLIED;
	return run_ended
		? SPQ_UNLOAD_DROPPED : SPQ_UNLOAD_WAITING;
}

/* Reproduce the float addition chain, including the 0.99999994F at 80. */
static inline int spq_remaining_ticks(float progress) {
	float value = 0.0f;
	if(progress <= 0.0f) return 81;
	for(int additions = 1; additions <= 81; additions++) {
		value += 0.0125f;
		if(progress <= value) return 81 - additions;
	}
	return 0;
}
static inline void spq_core_init(struct spq_core* core, int target) {
	*core = (struct spq_core){.target = target};
}
static inline struct spq_decision spq_core_step(
	struct spq_core* core, struct spq_observation obs) {
	struct spq_decision out = {0};
	if(!core) return out;
	if(!core->requested && obs.remaining >= 0
	   && obs.remaining <= core->target && !obs.switch_started) {
		core->requested = true;
		core->request_tick = obs.tick;
		core->request_up_ms = obs.up_ms;
		out.request_quit = true;
	}
	if(core->requested && core->winner == SPQ_UNDECIDED) {
		if(obs.switch_started) core->winner = SPQ_SWITCH_FIRST;
		else if(obs.save_settled) core->winner = SPQ_UNLOAD_FIRST;
	}
	out.winner = core->winner;
	return out;
}

static inline size_t spq_format_marker(char* dst, size_t cap, uint32_t seq,
	const char* name, uint32_t up_ms, uint32_t tick, int target, bool ok,
	const char* extra) {
	if(!dst || cap == 0 || !name) return 0;
	int n = snprintf(dst, cap,
		"{\"event\":\"split_portal_quit\",\"seq\":%u,"
		"\"marker\":\"%s\",\"up_ms\":%u,\"tick\":%u,"
		"\"remaining_ticks\":%d,\"ok\":%s%s%s}",
		seq, name, up_ms, tick, target, ok ? "true" : "false",
		extra && extra[0] ? "," : "", extra ? extra : "");
	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

#endif
