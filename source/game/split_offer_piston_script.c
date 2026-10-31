/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "split_offer_piston_script.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "hw_run.h"
#include "client_session.h"
#include "../entity/entity.h"

#define SOP_DEADLINE_MS 120000u
#define SOP_PLAY_MS 30000u
static struct {
	bool armed, finished, clock, created, pending, done, resume[2];
	unsigned phase, step;
	uint32_t seq, base, mark, play_mark;
	struct vehicle_smoke_hooks hooks;
	struct sop_record record, observed[SOP_ERROR + 1];
	bool seen[SOP_ERROR + 1];
	float host_rest[3];
	bool have_host_rest;
	char failure[48], line[SOP_LINE_CAP];
} D;

void split_offer_piston_reset(void) {
	memset(&D, 0, sizeof(D));
	sop_probe_reset();
	screen_sworld_split_offer_piston_authorize(false);
}
void split_offer_piston_arm(const struct hw_run_config* cfg,
	const struct vehicle_smoke_hooks* hooks) {
	split_offer_piston_reset();
	if(!cfg || !hooks || !cfg->enabled || !cfg->split_guest
	   || cfg->profile != HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M) return;
	D.armed = true;
	D.hooks = *hooks;
	sop_probe_arm();
	screen_sworld_split_offer_piston_authorize(true);
}
bool split_offer_piston_armed(void) { return D.armed; }
bool split_offer_piston_finished(void) { return D.finished; }
const char* split_offer_piston_failure(void) {
	return D.failure[0] ? D.failure : NULL;
}
static void fail(const char* code) {
	if(!D.failure[0]) snprintf(D.failure, sizeof(D.failure), "%s", code);
}
static bool marker(const char* event, const char* reason) {
	/* Refusal retries the same bytes: seq advances only on admission. */
	int n = snprintf(D.line, sizeof(D.line),
		"{\"schema\":1,\"profile\":\"%s\",\"event\":\"split_offer_piston.%s\","
		"\"seq\":%u,\"origin\":\"driver\",\"phase\":%u,\"reason\":\"%s\"}",
		SOP_PROFILE, event, D.seq + 1, D.phase, reason);
	if(n < 0 || (size_t)n >= sizeof(D.line) || !D.hooks.emit
	   || !D.hooks.emit(D.hooks.user, D.line)) return false;
	D.seq++;
	return true;
}
static bool drain(void) {
	for(unsigned i = 0; i < 3; i++) {
		if(!D.pending) {
			if(!sop_probe_pop(&D.record)) return true;
			D.pending = true;
			if(!sop_probe_format(&D.record, D.seq + 1, D.line, sizeof(D.line))) {
				fail("record_schema");
				D.pending = false;
				return true;
			}
		}
		if(!D.hooks.emit || !D.hooks.emit(D.hooks.user, D.line)) return false;
		D.seq++;
		D.observed[D.record.event] = D.record;
		D.seen[D.record.event] = true;
		if(!D.record.ok) fail(D.record.reason);
		D.pending = false;
	}
	return sop_probe_pending() == 0;
}
static void next(uint32_t now) {
	D.phase++;
	D.step = 0;
	D.mark = now;
}
static void connected_frame(struct input_player_frame* frame) {
	memset(frame, 0, sizeof(*frame));
	frame->connected = true;
	frame->category = INPUT_CAT_GAMECUBE;
	for(unsigned b = 0; b < IB_COUNT; b++) frame->glyphs[b] = -1;
}
static bool displaced(const double a[3], const double b[3], double floor) {
	double dx = a[0] - b[0], dz = a[2] - b[2];
	return dx*dx + dz*dz >= floor*floor;
}
static const char* extension_error(void) {
	const struct sop_record* b = &D.observed[SOP_BEFORE];
	const struct sop_record* a = &D.observed[SOP_EXTENDED];
	if(!D.seen[SOP_BEFORE]) return "before_missing";
	if(!a->bound || !a->generation || a->generation != b->generation)
		return "guest_generation";
	if(a->gameplay || a->confirmed || a->finished_loading)
		return "guest_admitted_early";
	if(!a->extended || a->tick - b->tick < 5) return "extension_incomplete";
	if(fabs(a->guest[0] - b->guest[0] - 0.5625) > 0.0001)
		return "guest_push_distance";
	if(a->guest[1] != b->guest[1] || a->guest[2] != b->guest[2])
		return "guest_push_distance";
	if(a->guest_corrections - b->guest_corrections != 1)
		return "guest_correction_delta";
	if(a->host_corrections != b->host_corrections
	   || a->host_token != b->host_token
	   || memcmp(a->host, b->host, sizeof(a->host))
	   || a->host_health != b->host_health) return "host_route_touched";
	if(a->guest_damage || a->health != b->health || a->health <= 0)
		return "guest_damage";
	return NULL;
}
void split_offer_piston_frame(const struct vehicle_smoke_ctx* ctx,
	struct vehicle_smoke_actions* out) {
	memset(out, 0, sizeof(*out));
	if(!D.armed || D.finished || !ctx) return;
	if(!D.clock) { D.clock = true; D.base = D.mark = ctx->now_ms; }
	out->suppress_autojoin = true;
	out->host_look = out->host_buttons_armed = true;
	out->host_pitch = 1.5707964F;
	out->host_frame_valid = out->guest_frame_valid = true;
	out->drive_guest_body = true;
	connected_frame(&out->host_frame);
	connected_frame(&out->guest_frame);
	/* The ordinary owned Resume edge handles a controllerless fixture's
 * recovery modal. No recovery state is written directly. */
	struct client_session* players[] = {ctx->host, ctx->guest};
	struct input_player_frame* frames[] = {&out->host_frame, &out->guest_frame};
	for(unsigned i = 0; i < 2; i++) {
		bool ready = players[i] && players[i]->menu_host.recovery.phase
			== CONTROLLER_RECOVERY_READY;
		if(ready && !D.resume[i]) {
			frames[i]->pressed[IB_GUI_CLICK] = true;
			frames[i]->down[IB_GUI_CLICK] = true;
		}
		D.resume[i] = ready && !D.resume[i];
	}
	if(ctx->now_ms - D.base > SOP_DEADLINE_MS) fail("deadline");
	if(sop_probe_overflows()) fail("probe_overflow");
	if(D.hooks.telemetry_lost && D.hooks.telemetry_lost(D.hooks.user)) {
		fail("telemetry_lost");
		D.done = true;
	}
	if(!D.done && !drain()) return;
	if(D.failure[0]) {
		if(!D.done && !marker("fail", D.failure)) return;
		D.done = true;
	}
	if(D.done) {
		/* Revoke fixture mutations without touching owner-thread POD/state. */
		sop_probe_disarm();
		screen_sworld_split_offer_piston_authorize(false);
		D.finished = true;
		char status[64];
		snprintf(status, sizeof(status), "%s%s", D.failure[0] ? "fail:" : "",
			D.failure[0] ? D.failure : "ok");
		if(D.hooks.profile_done) D.hooks.profile_done(D.hooks.user, status);
		return;
	}
	if(D.step == 0) {
		if(!marker("phase", "")) return;
		D.step = 1;
	}
	switch(D.phase) {
	case 0:
		if(!D.created) {
			if(D.hooks.autoload_conflict && D.hooks.autoload_conflict(D.hooks.user)) {
				fail("autoload_latch_consumed");
				return;
			}
			D.created = true;
			if(!D.hooks.create_world || !D.hooks.create_world(D.hooks.user, SOP_RIG_WORLD))
				fail("world_create");
			return;
		}
		if(!ctx->world_loaded || ctx->world_switch_pending || !ctx->host
		   || !ctx->host->local_player) return;
		/* Controller movement, never pose assignment: stop at x fraction .88.
 * At .82..1 the first half head cannot intersect the body's near X face;
 * the second half moves it .5625 once, beyond the full head and settle. */
		if(D.step == 1) {
			double x = ctx->host->camera.x, frac = x - floor(x);
			if(frac < 0.85 || frac > 0.90) {
				out->host_buttons[IB_LEFT] = out->host_buttons[IB_SNEAK] = true;
				return;
			}
			D.mark = ctx->now_ms; D.step = 2;
		}
		if(ctx->now_ms - D.mark >= 1000) {
			double x = ctx->host->camera.x, frac = x - floor(x);
			if(frac < 0.81 || frac > 0.99) { D.step = 1; return; }
			if(marker("world_ready", "peaceful_superflat_disposable")) next(ctx->now_ms);
		}
		break;
	case 1:
		if(D.step == 1) {
			if(ctx->guest && client_session_request_local_join(ctx->guest)) D.step = 2;
			return;
		}
		if(!D.seen[SOP_OFFERED]) return;
		if(D.step == 2) {
			double host[] = {ctx->host->camera.x, ctx->host->camera.y, ctx->host->camera.z};
			if(!displaced(host, D.observed[SOP_OFFERED].guest, 4.0)) {
				out->host_buttons[IB_FORWARD] = true;
				out->host_yaw = 3.1415927F;
				return;
			}
			D.step = 3; D.mark = ctx->now_ms;
		}
		/* An ordinary stop coasts under Beta friction. Wait for an observed
 * stable pose, rather than assuming a one-second neutral input erased the
 * last movement packet. The strict host-pose predicate stays byte-exact. */
		{
			float pos[] = {ctx->host->camera.x, ctx->host->camera.y, ctx->host->camera.z};
			if(!D.have_host_rest || memcmp(pos, D.host_rest, sizeof(pos))) {
				memcpy(D.host_rest, pos, sizeof(pos));
				D.have_host_rest = true; D.mark = ctx->now_ms;
			}
			if(ctx->now_ms - D.mark >= 500) next(ctx->now_ms);
		}
		break;
	case 2:
		if(D.step == 1 && sop_probe_command(SOP_CMD_BUILD)) D.step = 2;
		if(D.seen[SOP_FIXTURE]) next(ctx->now_ms);
		break;
	case 3:
		if(D.step == 1 && sop_probe_command(SOP_CMD_POWER)) D.step = 2;
		if(D.seen[SOP_EXTENDED]) {
			const char* error = extension_error();
			if(error) fail(error); else next(ctx->now_ms);
		}
		break;
	case 4:
		if(D.step == 1 && sop_probe_command(SOP_CMD_RELEASE)) D.step = 2;
		if(D.seen[SOP_ADMITTED] && ctx->guest && ctx->guest->local_gameplay_ready)
			next(ctx->now_ms);
		break;
	case 5:
		if(D.step == 1 && sop_probe_command(SOP_CMD_PLAY_BEGIN)) D.step = 2;
		if(!D.seen[SOP_PLAY_BEGIN]) return;
		if(D.step == 2) { D.play_mark = ctx->now_ms; D.step = 3; }
		/* Real owned walk, a 4 s forward / 4 s backward circuit away from
 * the fixture; the server record must prove ordinary movement continued. */
		out->guest_look = true;
		out->guest_yaw = 0; out->guest_pitch = 1.5707964F;
		out->guest_frame.down[((ctx->now_ms - D.play_mark) / 4000) % 2
			? IB_BACKWARD : IB_FORWARD] = true;
		if(ctx->now_ms - D.play_mark >= SOP_PLAY_MS) next(ctx->now_ms);
		break;
	case 6:
		if(D.step == 1 && sop_probe_command(SOP_CMD_PLAY_END)) D.step = 2;
		if(D.seen[SOP_PLAY_END]) {
			const struct sop_record* a = &D.observed[SOP_PLAY_END];
			const struct sop_record* b = &D.observed[SOP_PLAY_BEGIN];
			if(!a->gameplay || !a->finished_loading || a->generation != b->generation)
				fail("play_admission_lost");
			else if(a->guest_damage || a->health <= 0) fail("play_damage");
			else if(a->play_ms < SOP_PLAY_MS || a->tick - b->tick < 300)
				fail("play_window_short");
			else if(!displaced(a->guest, b->guest, 1.0)) fail("play_no_movement");
			else if(marker("run_ok", "")) D.done = true;
		}
		break;
	default: fail("phase_invalid"); break;
	}
}
