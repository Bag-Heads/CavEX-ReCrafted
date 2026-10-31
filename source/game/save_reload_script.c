/* Copyright (c) 2026 CaveWii_v2 contributors
 * save_reload_3m phase driver -- see save_reload_script.h. */
#include "save_reload_script.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "../network/save_reload_probe.h"
#include "../platform/input.h"

const char* srs_phase_name(enum srs_phase p) {
	switch(p) {
		case SRS_TITLE: return "title";
		case SRS_LOAD1: return "load1";
		case SRS_FIXTURE: return "fixture";
		case SRS_PAUSE: return "pause";
		case SRS_FOCUS: return "focus";
		case SRS_CLICK: return "click";
		case SRS_SAVING: return "saving";
		case SRS_RELOAD: return "reload";
		case SRS_DWELL: return "dwell";
		case SRS_DONE: return "done";
		default: return "fail";
	}
}

void srs_init(struct srs_state* st) {
	memset(st, 0, sizeof *st);
	st->phase = SRS_TITLE;
}

static void enter(struct srs_state* st, const struct srs_obs* o,
				  enum srs_phase p) {
	st->phase = p;
	st->phase_ms = o->ms;
	st->presses = 0;
}
static void fail(struct srs_state* st, const struct srs_obs* o,
				 const char* reason, struct srs_out* out) {
	st->reason = reason;
	enter(st, o, SRS_FAIL);
	out->press = SRS_BTN_NONE;
	out->marker = "fail";
}
/* One press, then at least `gap` ms of neutral frames before the next. */
static bool press(struct srs_state* st, const struct srs_obs* o,
				  struct srs_out* out, enum srs_button b, uint32_t gap) {
	if(st->presses && o->ms - st->last_press_ms < gap)
		return false;
	out->press = b;
	st->last_press_ms = o->ms;
	st->presses++;
	return true;
}

void srs_tick(struct srs_state* st, const struct srs_obs* o,
			  struct srs_out* out) {
	memset(out, 0, sizeof *out);
	if(o->world_loaded && !st->prev_loaded)
		st->loaded_edges++;
	st->prev_loaded = o->world_loaded;
	if(st->phase == SRS_DONE || st->phase == SRS_FAIL)
		return;
	if(o->ms >= SRS_TOTAL_CAP_MS) {
		fail(st, o, "total_cap", out);
		return;
	}
	uint32_t in_phase = o->ms - st->phase_ms;
	switch(st->phase) {
		case SRS_TITLE:
			if(o->title_ready && !o->world_loaded) {
				out->create_world = true;
				st->created = true;
				out->marker = "create_world";
				enter(st, o, SRS_LOAD1);
			} else if(in_phase > 60000u) {
				fail(st, o, "title_never_reached", out);
			}
			return;
		case SRS_LOAD1:
			if(o->world_loaded && o->ingame && st->loaded_edges == 1) {
				out->marker = "world_loaded_1";
				enter(st, o, SRS_FIXTURE);
			} else if(st->loaded_edges > 1) {
				fail(st, o, "unexpected_second_load", out);
			} else if(in_phase > 90000u) {
				fail(st, o, "world_never_loaded", out);
			}
			return;
		case SRS_FIXTURE:
			if(!o->world_loaded) {
				fail(st, o, "world_lost_before_save", out);
			} else if(!st->fixture_ms) {
				if(o->probe_applied) {
					st->fixture_ms = o->ms ? o->ms : 1u;
					out->marker = "fixture_applied";
				} else if(in_phase > 30000u) {
					fail(st, o, "fixture_never_applied", out);
				}
			} else if(o->ms - st->fixture_ms >= SRS_SETTLE_AFTER_FIXTURE_MS
					  && o->ingame) {
				enter(st, o, SRS_PAUSE);
			}
			return;
		case SRS_PAUSE:
			if(o->pause) {
				out->marker = "pause_open";
				enter(st, o, SRS_FOCUS);
			} else if(in_phase > 15000u) {
				fail(st, o, "pause_never_opened", out);
			} else if(o->ingame) {
				(void)press(st, o, out, SRS_BTN_HOME, 1500u);
			}
			return;
		case SRS_FOCUS:
			if(!o->pause) {
				fail(st, o, "pause_closed_before_exit", out);
			} else if(o->focus_exit) {
				out->marker = "exit_focused";
				enter(st, o, SRS_CLICK);
			} else if(st->presses >= 12u) {
				fail(st, o, "exit_row_not_focused", out);
			} else {
				(void)press(st, o, out, SRS_BTN_DOWN, 300u);
			}
			return;
		case SRS_CLICK:
			/* Neutral frame gap after the last DOWN, then ONE click; retried
			 * (bounded) only while the pause still shows Save and quit
			 * focused with no exit pending -- i.e. the click never landed. */
			if(o->exit_pending || !o->pause) {
				out->marker = "save_quit_clicked";
				enter(st, o, SRS_SAVING);
			} else if(st->presses >= 3u) {
				fail(st, o, "save_quit_click_not_taken", out);
			} else if(o->focus_exit && in_phase >= 300u) {
				(void)press(st, o, out, SRS_BTN_CLICK, 1500u);
			}
			return;
		case SRS_SAVING:
			if(!o->world_loaded && o->title_ready && o->probe_saves >= 1u) {
				out->marker = "saved_at_title";
				enter(st, o, SRS_RELOAD);
			} else if(in_phase > 60000u) {
				fail(st, o,
					 o->stats_modal				   ? "stats_save_modal_blocked"
						 : o->probe_saves == 0u ? "save_never_published"
												: "title_never_reached_after_save",
					 out);
			}
			return;
		case SRS_RELOAD:
			if(!st->reload_armed) {
				out->arm_reload = true;
				st->reload_armed = true;
				out->marker = "reload_armed";
			} else if(o->world_loaded && o->ingame && st->loaded_edges == 2u
					  && o->probe_loads >= 2u && o->probe_after_loads >= 1u) {
				out->marker = "world_loaded_2";
				enter(st, o, SRS_DWELL);
			} else if(in_phase > 90000u) {
				fail(st, o, "reload_never_completed", out);
			}
			return;
		case SRS_DWELL:
			/* The reopened world idles in game; losing it, or a third load,
			 * fails the stability window. */
			if(!o->world_loaded || st->loaded_edges != 2u) {
				fail(st, o, "world_lost_after_reload", out);
			} else if(in_phase >= SRS_POST_RELOAD_DWELL_MS && o->ingame) {
				out->marker = "dwell_done";
				enter(st, o, SRS_DONE);
			}
			return;
		default:
			return;
	}
}

/* ---- device wiring ----------------------------------------------------- */
static struct {
	bool armed, verdict_sent, done;
	struct srs_state st;
	struct srs_out out;
	struct save_reload_hooks hooks;
	char run_id[64];
	unsigned seq;
	char pending[2048]; /* the observer cap is 2303 bytes */
	bool has_pending;
	char status[64];
} D;

void save_reload_arm(const struct save_reload_hooks* hooks,
					 const char* run_id) {
	memset(&D, 0, sizeof D);
	if(!hooks)
		return;
	D.hooks = *hooks;
	snprintf(D.run_id, sizeof D.run_id, "%s", run_id ? run_id : "");
	srs_init(&D.st);
	D.armed = true;
	save_reload_probe_arm(true);
}

bool save_reload_armed(void) { return D.armed; }

const char* save_reload_status(void) {
	return D.armed && D.done ? D.status : NULL;
}

bool save_reload_force_button(int button, bool* out) {
	if(!D.armed || D.out.press == SRS_BTN_NONE)
		return false;
	int want = D.out.press == SRS_BTN_HOME ? IB_HOME
		: D.out.press == SRS_BTN_DOWN	   ? IB_GUI_DOWN
										   : IB_GUI_CLICK;
	if(button != want)
		return false;
	*out = true;
	return true;
}

#define H16(v) (unsigned long)((v) >> 32), (unsigned long)((v) & 0xffffffffu)
static void snap_json(char* dst, size_t cap, const struct save_reload_snap* s) {
	snprintf(dst, cap,
			 "{\"inv\":\"%08lx%08lx\",\"chest\":\"%08lx%08lx\","
			 "\"furnace\":\"%08lx%08lx\",\"blocks\":\"%08lx%08lx\","
			 "\"pos\":\"%08lx%08lx\",\"burn\":%d,\"cook\":%d,"
			 "\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"fixture_ok\":%s,"
			 "\"resident\":%s}",
			 H16(s->inv), H16(s->chest), H16(s->furnace), H16(s->blocks),
			 H16(s->pos), (int)s->burn, (int)s->cook, s->x, s->y, s->z,
			 s->fixture_ok ? "true" : "false", s->resident ? "true" : "false");
}

static bool emit(const char* line) {
	if(D.hooks.emit && D.hooks.emit(line))
		return true;
	snprintf(D.pending, sizeof D.pending, "%s", line);
	D.has_pending = true;
	return false;
}

void save_reload_frame(const struct srs_obs* obs, const char* world_folder) {
	if(!D.armed || !obs)
		return;
	D.out = (struct srs_out) {0};
	if(D.has_pending) { /* a refused line is resent first, in order */
		if(!D.hooks.emit || !D.hooks.emit(D.pending))
			return;
		D.has_pending = false;
	}
	if(D.done)
		return;
	if(D.st.phase == SRS_DONE || D.st.phase == SRS_FAIL) {
		if(D.verdict_sent) {
			D.done = true;
			return;
		}
		struct save_reload_probe_view v;
		if(!save_reload_probe_read(&v))
			return;
		bool ok = D.st.phase == SRS_DONE;
		bool same = v.before.inv == v.after.inv
			&& v.before.chest == v.after.chest
			&& v.before.furnace == v.after.furnace
			&& v.before.blocks == v.after.blocks
			&& v.before.pos == v.after.pos;
		const char* status = !ok ? D.st.reason
			: !v.before.fixture_ok || !v.after.fixture_ok ? "fixture_not_intact"
			: !v.before.resident || !v.after.resident	  ? "fixture_not_resident"
			: !same										  ? "hash_mismatch"
														  : "ok";
		snprintf(D.status, sizeof D.status, "%s", status ? status : "fail");
		char b[400], a[400], line[2048]; /* fits: observer cap 2303 */
		snap_json(b, sizeof b, &v.before);
		snap_json(a, sizeof a, &v.after);
		snprintf(line, sizeof line,
				 "{\"event\":\"save_reload\",\"schema\":1,\"run_id\":\"%s\","
				 "\"status\":\"%s\",\"phase\":\"%s\",\"ms\":%u,"
				 "\"world\":\"%s\",\"client_loads\":%u,\"server_loads\":%u,"
				 "\"saves\":%u,\"after_loads\":%u,\"applied\":%u,"
				 "\"before\":%s,\"after\":%s}",
				 D.run_id, D.status, srs_phase_name(D.st.phase),
				 (unsigned)obs->ms, v.folder, D.st.loaded_edges, v.loads,
				 v.saves, v.after_loads, v.applied, b, a);
		D.verdict_sent = true;
		if(emit(line))
			D.done = true;
		return;
	}
	struct srs_obs o = *obs;
	struct save_reload_probe_view v;
	if(save_reload_probe_read(&v)) {
		o.probe_applied = v.applied;
		o.probe_loads = v.loads;
		o.probe_saves = v.saves;
		o.probe_after_loads = v.after_loads;
	}
	srs_tick(&D.st, &o, &D.out);
	if(D.out.create_world && (!D.hooks.create_world || !D.hooks.create_world())) {
		D.st.reason = "world_create_refused";
		D.st.phase = SRS_FAIL;
		D.out.marker = "fail";
	}
	if(D.out.arm_reload) {
		const char* folder = world_folder && world_folder[0] ? world_folder
															 : v.folder;
		if(!folder[0] || !D.hooks.arm_reload || !D.hooks.arm_reload(folder)) {
			D.st.reason = "reload_arm_refused";
			D.st.phase = SRS_FAIL;
			D.out.marker = "fail";
		}
	}
	if(D.out.marker) {
		char line[256];
		snprintf(line, sizeof line,
				 "{\"event\":\"save_reload_phase\",\"run_id\":\"%s\","
				 "\"seq\":%u,\"marker\":\"%s\",\"phase\":\"%s\",\"ms\":%u,"
				 "\"client_loads\":%u,\"reason\":\"%s\"}",
				 D.run_id, ++D.seq, D.out.marker, srs_phase_name(D.st.phase),
				 (unsigned)obs->ms, D.st.loaded_edges,
				 D.st.reason ? D.st.reason : "");
		(void)emit(line);
	}
}
