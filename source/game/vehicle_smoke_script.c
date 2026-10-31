/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* host_vehicle_smoke_8m phase driver. See vehicle_smoke_script.h for the
 * design note and tmp/jobs/vehicle-smoke-1004/CONTRACT-v1.md for the binding
 * contract. */

#include "vehicle_smoke_script.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../entity/entity.h"
#include "../network/client_interface.h"
#include "../network/client_world_payload.h"
#include "../network/server_interface.h"
#include "client_session.h"
#include "game_state.h"
#include "gui/local_pause.h"
#include "hw_run.h"

/* Pacing: never start a new operation while the observer path is behind --
 * a full probe FIFO is a terminal FAIL, so the driver waits instead. */
#define VS_PACE_RECORDS 4u
#define VS_STAGE_MS 12000u      /* one action stage */
#define VS_WALK_MS 20000u       /* guest positioning walk */
#define VS_RIDE_MS 25000u       /* one ride window */
#define VS_DRAIN_MS 15000u      /* final drain before profile_done */
#define VS_DEATH_GRACE_MS 1200u /* released click before the respawn hold */
#define VS_HOLD_MAX_MS 6000u
#define VS_LINES_PER_FRAME 3u
#define VEHICLE_SMOKE_FINAL_PASSES 20u /* p5 detached census passes */
#define VS_REACH_RAY 4.4F       /* inside the client's 4.5 entity ray */

enum vs_hop_expect { VS_EXPECT_MOUNTED, VS_EXPECT_DETACHED, VS_EXPECT_SADDLED };

struct vs_hop { /* one host interaction through the real input path */
	bool active;
	uint8_t action, kind, button, expect;
	uint32_t target, op, edge_seq, request_seq;
	unsigned st;
	uint32_t st_ms, press_ms;
	uint64_t serial;
	/* O15 diagnostic (stderr at a hop failure, never on the wire): why the
	 * aim stage did not see a settled hit on the target. */
	uint32_t d_frames, d_none, d_other, d_far, d_notgt, d_gate;
	uint32_t d_other_id;
	float d_eye[3], d_tgt[3], d_dist;
};

struct vs_probe { /* one P2 negative probe inside its isolation bracket */
	bool active, occupied;
	uint32_t rest_au; /* active update the host was first seen at rest */
	uint8_t kind, channel, rpc, action;
	uint32_t target, op, delivered_active;
	unsigned st;
	uint32_t st_ms;
	bool client_before_sent;
};

struct vs_ride { /* one ride window */
	bool active;
	uint8_t kind;
	uint32_t target, window;
	unsigned st;
	uint32_t st_ms;
	float start[3];
	float disp;
	bool need_motion;
	/* O06: cart ride regulation on the bounded fixture track */
	uint32_t cart_pos0, cart_neg0; /* client step counters at window open */
	uint32_t cart_brake_steps;     /* BACKWARD steps owed (= FORWARD steps) */
	bool cart_braking;
	float cart_yaw; /* O09: the window's rail-axis facing */
	float boat_yaw; /* O25: the window's basin-axis facing */
};

static struct {
	bool armed;
	struct vehicle_smoke_hooks hooks;
	struct vehicle_smoke_config cfg;
	size_t reserve;

	enum vsp_phase phase;
	unsigned step;
	bool clock_set, started, closing, finished, done_sent, run_ok_pushed;
	uint32_t base_ms, now_ms, phase_start_ms, step_start_ms, close_mark;
	uint32_t finish_start_ms;
	char fail_code[49];
	bool failed;

	/* serializer */
	uint32_t seq;
	char line[VSP_LINE_CAP];
	bool line_pending;

	/* operations */
	uint32_t next_op;
	uint32_t edge_seq;
	uint32_t window;
	uint32_t probe_request_seq;
	bool last_edge; /* a host button edge was delivered last frame */
	struct vs_hop hop;
	struct vs_probe probe;
	struct vs_ride ride;
	unsigned seq_index; /* switch sequence / probe list cursor */

	/* fixture */
	struct vsp_r targets[3];
	int32_t anchor[3];
	bool have_targets;
	float guest_goal[2];
	uint32_t base_inv_hash[2], base_inv_used[2];
	/* O14 two-home: the p3 pre-bracket walk H1 -> H2 and its settling */
	bool far_home_done;
	uint8_t far_sub;
	uint32_t far_start_ms, far_still_frames;
	float far_last[3];

	/* observed records (only what passed through the serializer) */
	struct {
		bool rig_ready, rig_ok;
		uint32_t auth_op;
		uint8_t auth_status, auth_action;
		char auth_reason[49];
		uint64_t auth_serial;
		uint32_t ingress_op;
		uint32_t iso_server_before, iso_server_after;
		uint32_t iso_server_attach;
		uint32_t ride_window;
		uint32_t ride_eligible, ride_ticks, ride_missing, ride_dup;
		float ride_disp;
		bool ck_begin, ck_durable, ck_ok;
		uint32_t checkpoint_id;
		uint32_t pose_bits[3];
		uint32_t ck_host_hash, ck_guest_hash;
		unsigned reload_stages; /* bit per vsp_stage_reload */
		struct vsp_r reload_targets[3];
		uint32_t reload_pose_bits[3];
		bool death_server;
		unsigned respawn_stages;
		bool transfer_begin, transfer_release, transfer_commit, transfer_ack;
		uint32_t transfer_id;
		unsigned pending_stages; /* bit per (stage*8+cause) */
		bool paused_seen, resumed_seen, clear_seen, barrier_seen;
		uint32_t pause_ms, resume_ms;
		uint32_t installed, ready_server;
	} obs;

	/* phase-local */
	uint32_t mark_ms, mark_active, mark_reset;
	uint32_t fixture_op, lethal_op, transfer_op, refusal_op, save_op, iso_op;
	uint32_t death_epoch; /* Q4: host client input epoch at death */
	bool p2_resume_edge;  /* controllerless P2: Resume edge this frame */
	uint32_t p2_resumes;  /* how many recovery Resumes P2 delivered */
	bool p1_resume_edge;  /* the same, for the controllerless P1 */
	uint32_t p1_resumes;
	bool accept_pushed, confirm_pushed, gameplay_pushed; /* p4 readmission rows */
	bool p4_bracket; /* p4 transfer bracket opened (before the Leave) */
	uint32_t final_op, final_window; /* p5 detached census */
	bool flag_a, flag_b;
	uint64_t leave_gen;
	uint32_t hold_start_ms;

	/* client isolation bracket: guest pose delta accumulated EVERY frame
	 * between the client's before and after records (never endpoint-only) */
	bool ciso_open, ciso_have_pose;
	uint64_t ciso_gen; /* guest generation of the client pose baseline */
	float ciso_pose0[3], ciso_max;
} V;

/* ---- plumbing ------------------------------------------------------------ */

void vehicle_smoke_reset(void) {
	memset(&V, 0, sizeof(V));
	vsp_reset();
	screen_sworld_vehicle_rig_authorize(false);
}

static void vs_arm_common(const struct vehicle_smoke_config* cfg,
						  const struct vehicle_smoke_hooks* hooks) {
	vehicle_smoke_reset();
	if(!cfg || !hooks || cfg->kind >= VSP_KIND_COUNT)
		return;
	V.cfg = *cfg;
	V.hooks = *hooks;
	/* hw_telemetry appends ,"run_id":"<id>" and the transport adds LF. */
	V.reserve = 12u + cfg->run_id_len + 1u;
	V.armed = true;
	V.phase = VSP_P0;
	screen_sworld_vehicle_rig_authorize(true);
	vsp_arm(&(struct vsp_arm_cfg) {.kind = cfg->kind,
		.transfer_fixture_request = cfg->transfer_fixture_request,
		.build_id = cfg->build_id});
}

void vehicle_smoke_arm_config(const struct vehicle_smoke_config* cfg,
							  const struct vehicle_smoke_hooks* hooks) {
	vs_arm_common(cfg, hooks);
}

void vehicle_smoke_arm(const struct hw_run_config* cfg,
					   const struct vehicle_smoke_hooks* hooks,
					   const char* build_id, size_t run_id_len) {
	vehicle_smoke_reset();
	if(!cfg || !hooks || !cfg->enabled || !cfg->vehicle_smoke
	   || cfg->profile != HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M
	   || !cfg->split_guest)
		return;
	enum vsp_kind kind;
	switch(cfg->vehicle_kind) {
		case HW_RUN_VEHICLE_BOAT: kind = VSP_BOAT; break;
		case HW_RUN_VEHICLE_PIG: kind = VSP_PIG; break;
		case HW_RUN_VEHICLE_CART: kind = VSP_CART; break;
		default: return;
	}
	vs_arm_common(&(struct vehicle_smoke_config) {.kind = kind,
		.transfer_fixture_request
			= cfg->vehicle_transfer == HW_RUN_VEHICLE_TRANSFER_FIXTURE_REQUEST,
		.build_id = build_id, .run_id_len = run_id_len,
		.seed_set = cfg->seed_set, .seed = cfg->seed}, hooks);
}

bool vehicle_smoke_armed(void) {
	return V.armed;
}

bool vehicle_smoke_rig_create_allowed(const char* directory) {
	return V.armed && directory
		&& strcmp(directory, VEHICLE_SMOKE_RIG_WORLD) == 0;
}

enum vsp_phase vehicle_smoke_phase_now(void) {
	return V.phase;
}
unsigned vehicle_smoke_step_now(void) {
	return V.step;
}
uint32_t vehicle_smoke_seq_now(void) {
	return V.seq;
}
const char* vehicle_smoke_fail_code(void) {
	return V.failed ? V.fail_code : NULL;
}
bool vehicle_smoke_finished(void) {
	return V.finished;
}

/* ---- records / serializer ------------------------------------------------ */

static void vs_rec(struct vsp_record* r, enum vsp_event ev,
				   enum vsp_origin origin, uint32_t op) {
	memset(r, 0, sizeof(*r));
	r->event = (uint8_t)ev;
	r->origin = (uint8_t)origin;
	r->ok = true;
	r->op_id = op;
}

static bool vs_push(struct vsp_record* r);

/* The ONE failure path: hooks off, a fail record, then a bounded drain and
 * exactly one profile_done (vs_finish). A second failure keeps the first. */
static void vs_fail(const char* code) {
	if(V.failed || V.finished || !V.armed)
		return;
	V.failed = true;
	snprintf(V.fail_code, sizeof(V.fail_code), "%s", code ? code : "fail");
	for(char* c = V.fail_code; *c; c++) {
		bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')
			|| (*c >= '0' && *c <= '9') || *c == '_' || *c == '.'
			|| *c == ':' || *c == '-';
		if(!ok)
			*c = '_';
	}
	vsp_hooks_reset();
	enum vsp_phase was = V.phase;
	V.phase = VSP_PH_FAILED;
	vsp_set_phase(VSP_PH_FAILED);
	struct vsp_record r;
	vs_rec(&r, VSP_EV_FAIL, VSP_PRODUCTION, 0);
	r.ok = false;
	snprintf(r.reason, sizeof(r.reason), "%s", V.fail_code);
	snprintf(r.u.fail.code, sizeof(r.u.fail.code), "%s", V.fail_code);
	r.u.fail.failed_phase = (uint8_t)was;
	r.u.fail.overflow_count = vsp_overflow_count();
	(void)vsp_main_push(&r); /* best effort: profile_done is unconditional */
	vsp_disarm();
	V.finish_start_ms = V.now_ms;
}

void vehicle_smoke_fail(const char* code) {
	vs_fail(code);
}

static bool vs_push(struct vsp_record* r) {
	if(V.failed)
		return false;
	if(!vsp_main_push(r)) {
		vs_fail("probe_overflow");
		return false;
	}
	return true;
}

/* Track what the serializer admits: the phase machine advances ONLY on
 * evidence that has been staged for the wire. */
static void vs_observe(const struct vsp_record* r) {
	switch((enum vsp_event)r->event) {
		case VSP_EV_RIG_READY: {
			const struct vsp_p_rig_ready* p = &r->u.rig_ready;
			V.obs.rig_ready = true;
			V.obs.rig_ok = p->water_ok && p->support_ok && p->rails_ok
				&& p->ray_ok && p->saddle_consumed && p->pig_ai_updates > 0;
			break;
		}
		case VSP_EV_ACTION_RESULT: {
			const struct vsp_p_action_result* p = &r->u.result;
			if(p->site == VSP_SITE_AUTHORITY) {
				V.obs.auth_op = r->op_id;
				V.obs.auth_status = p->status;
				V.obs.auth_action = p->action;
				V.obs.auth_serial = p->serial;
				snprintf(V.obs.auth_reason, sizeof(V.obs.auth_reason), "%s",
						 r->reason);
			} else if(p->site == VSP_SITE_INGRESS)
				V.obs.ingress_op = r->op_id;
			break;
		}
		case VSP_EV_ISOLATION:
			if(r->u.iso.owner == VSP_STAGE_SERVER) {
				if(r->u.iso.stage == VSP_STAGE_BEFORE)
					V.obs.iso_server_before = r->u.iso.attempt_op;
				else {
					V.obs.iso_server_after = r->u.iso.attempt_op;
					V.obs.iso_server_attach = r->u.iso.unexpected_attach_count
						+ r->u.iso.guest_follow_count;
				}
			}
			break;
		case VSP_EV_RIDE_SAMPLE:
			if(r->u.ride.stage == VSP_STAGE_SERVER) {
				V.obs.ride_window = r->u.ride.window_id;
				V.obs.ride_eligible = r->u.ride.eligible_updates;
				V.obs.ride_ticks = r->u.ride.tick_calls;
				V.obs.ride_missing = r->u.ride.missing_calls;
				V.obs.ride_dup = r->u.ride.duplicate_calls;
				V.obs.ride_disp = r->u.ride.displacement_m;
			}
			break;
		case VSP_EV_CHECKPOINT:
			if(r->u.ck.stage == VSP_CK_BEGIN)
				V.obs.ck_begin = true;
			else {
				V.obs.ck_durable = true;
				V.obs.ck_ok = r->u.ck.save_ok;
				memcpy(V.obs.pose_bits, r->u.ck.pose_bits, sizeof(V.obs.pose_bits));
				V.obs.ck_host_hash = r->u.ck.host.inv_hash;
				V.obs.ck_guest_hash = r->u.ck.guest.inv_hash;
			}
			V.obs.checkpoint_id = r->u.ck.checkpoint_id;
			break;
		case VSP_EV_RELOAD:
			V.obs.reload_stages |= 1u << r->u.reload.stage;
			if(r->u.reload.stage == VSP_RL_ENTITIES)
				memcpy(V.obs.reload_targets, r->u.reload.u.entities.targets,
					   sizeof(V.obs.reload_targets));
			if(r->u.reload.stage == VSP_RL_DISK)
				memcpy(V.obs.reload_pose_bits, r->u.reload.u.disk.pose_bits,
					   sizeof(V.obs.reload_pose_bits));
			break;
		case VSP_EV_DEATH:
			if(r->u.death.stage == VSP_DEATH_SERVER)
				V.obs.death_server = true;
			break;
		case VSP_EV_RESPAWN:
			V.obs.respawn_stages |= 1u << r->u.respawn.stage;
			break;
		case VSP_EV_TRANSFER_BEGIN:
			V.obs.transfer_begin = true;
			V.obs.transfer_id = r->u.transfer.transfer_id;
			break;
		case VSP_EV_TRANSFER_RELEASE:
			V.obs.transfer_release = true;
			break;
		case VSP_EV_TRANSFER_COMMIT:
			V.obs.transfer_commit = true;
			break;
		case VSP_EV_TRANSFER_ACK:
			if(r->u.tack.stage == VSP_STAGE_SERVER)
				V.obs.transfer_ack = true;
			break;
		case VSP_EV_PENDING:
			V.obs.pending_stages
				|= 1u << (r->u.progress.stage * 8u + r->u.progress.cause);
			break;
		case VSP_EV_PAUSE:
			V.obs.paused_seen = true;
			V.obs.pause_ms = r->ms;
			break;
		case VSP_EV_RESUME:
			V.obs.resumed_seen = true;
			V.obs.resume_ms = r->ms;
			break;
		case VSP_EV_ADMISSION_CLEAR:
			V.obs.clear_seen = true;
			break;
		case VSP_EV_BARRIER_RELEASE:
			V.obs.barrier_seen = true;
			break;
		case VSP_EV_INSTALLED:
			V.obs.installed = r->u.progress.installed_count;
			break;
		case VSP_EV_GAMEPLAY_READY:
			if(r->u.ready.stage == VSP_STAGE_SERVER)
				V.obs.ready_server = r->u.ready.ready_count;
			break;
		default:
			break;
	}
}

unsigned vehicle_smoke_emit(unsigned max_lines) {
	unsigned n = 0;
	if(!V.armed || V.finished)
		return 0;
	while(n < max_lines) {
		if(!V.line_pending) {
			struct vsp_record r;
			if(!vsp_pop(&r))
				break;
			vs_observe(&r);
			const char* err = NULL;
			size_t len = vsp_serialize(&r, V.seq + 1u, V.reserve, V.line,
									   sizeof(V.line), &err);
			if(!len) {
				char code[48];
				snprintf(code, sizeof(code), "serialize_%s_ev%u",
						 err ? err : "error", (unsigned)r.event);
				vs_fail(code);
				continue; /* the fail record itself is next in line */
			}
			V.line_pending = true;
		}
		if(!V.hooks.emit || !V.hooks.emit(V.hooks.user, V.line)) {
			/* O22: a refusal on a live stream is retried (the SAME bytes,
			 * same seq, next frame). A dead stream will never accept it, and
			 * evidence is observed only as it is emitted, so fail BY NAME now
			 * instead of timing out at whatever stage was waiting. */
			if(V.hooks.telemetry_lost && V.hooks.telemetry_lost(V.hooks.user))
				vs_fail("telemetry_quiesced");
			break;
		}
		V.line_pending = false;
		V.seq++;
		n++;
	}
	return n;
}

static bool vs_drained(void) {
	return !V.line_pending && vsp_pending_records() == 0;
}

static bool vs_paced(void) {
	return !V.line_pending && vsp_pending_records() <= VS_PACE_RECORDS;
}

/* ---- client observation helpers ----------------------------------------- */

static bool vs_c(const struct vehicle_smoke_ctx* ctx, unsigned view,
				 struct vsp_c* out) {
	struct client_session* s = view ? ctx->guest : ctx->host;
	if(vsp_snap_client(s, view, out))
		return true;
	vsp_snap_client_absent(s, out);
	return false;
}

static bool vs_vc(unsigned view, uint8_t kind, struct vsp_vc* out) {
	return vsp_snap_client_vehicle(view, V.targets[kind].id, kind, out);
}

static bool vs_no_prediction(const struct vsp_c* c) {
	return !c->ride_pending && !c->ride_send_dirty && !c->ride_exit_lift_pending
		&& !c->ride_exit_repin_eligible;
}

/* CONTRACT-v1 ClientMounted(C,VC). */
static bool vs_client_mounted(const struct vsp_c* c, const struct vsp_vc* vc) {
	if(!c->present || !c->ready || !vc->alive || c->mount_count != 1
	   || !vs_no_prediction(c) || vc->target.world.slot != c->bound.slot
	   || vc->target.world.generation != c->bound.generation)
		return false;
	switch(vc->target.kind) {
		case VSP_BOAT:
			return vc->occupied && vc->occupant_id == c->actor.wire_id;
		case VSP_PIG:
			return vc->pig_ridden && vc->pig_rider_id == c->actor.wire_id
				&& vc->saddled;
		default:
			return vc->cart_type == 0 && vc->occupied
				&& vc->occupant_id == c->actor.wire_id && vc->control_ready
				&& vc->ride_epoch > 0 && c->ride_target == vc->target.id
				&& c->ride_desired_occupied
				&& c->ride_input_epoch == c->actor.input_epoch
				&& c->ride_request_seq != 0;
	}
}

/* The driver's settle gate for a detach: zero native relations, no live
 * prediction. (The collector additionally grades steer == 0.) */
static bool vs_client_detached(const struct vsp_c* c) {
	return c->present && c->ready && c->mount_count == 0 && vs_no_prediction(c);
}

static void vs_aim(const float eye[3], const float tgt[3], float* yaw,
				   float* pitch) {
	float dx = tgt[0] - eye[0], dy = tgt[1] - eye[1], dz = tgt[2] - eye[2];
	*yaw = atan2f(dx, dz);
	*pitch = atan2f(sqrtf(dx * dx + dz * dz), dy); /* polar angle from +Y */
}

static float vs_dist(const float a[3], const float b[3]) {
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void vs_eye(const struct client_session* s, float out[3]) {
	out[0] = s->camera.x;
	out[1] = s->camera.y;
	out[2] = s->camera.z;
}

/* The aim point of a client vehicle: a little above its logical position so
 * the entity ray meets the hull, not its floor. */
static bool vs_target_point(unsigned view, uint8_t kind, float out[3]) {
	struct vsp_vc vc;
	if(!vs_vc(view, kind, &vc) || !vc.alive)
		return false;
	out[0] = vc.pos[0];
	out[1] = vc.pos[1] + (kind == VSP_PIG ? 0.45F : kind == VSP_BOAT ? 0.3F
																	  : 0.15F);
	out[2] = vc.pos[2];
	return true;
}

/* O15: where the HOST hop aims. The client's camera entity ray (main.c,
 * raycast_entity, 4.5 m) reports the distance to where it ENTERS the target's
 * box. Aiming at the centre point (vs_target_point) put that entry at the
 * box's far top face: from the pen's far corner the boat's centre is 4.78 m
 * away, so the ray never hit it although the boat's near hull was ~4.1 m away
 * (measured: O15 run vs_pc_pig_20261004_184503_o15d9, 36542/36543 aim frames
 * with no entity hit while the unsteered pig stood still; switch_timeout_st0).
 * A player aims at the near hull: the point of the target's real client box
 * nearest the eye, inset 5 cm into the box so the ray enters it. Records keep
 * vs_target_point (their reach/LOS semantics are unchanged). */
void vehicle_smoke_hull_aim(const float box[6], const float eye[3], float out[3]) {
	for(int a = 0; a < 3; a++) {
		float lo = box[a] + 0.05F, hi = box[a + 3] - 0.05F;
		if(lo > hi)
			lo = hi = (box[a] + box[a + 3]) * 0.5F;
		out[a] = eye[a] < lo ? lo : (eye[a] > hi ? hi : eye[a]);
	}
}

/* O25: a boat window steers toward the FARTHER basin wall, fixed at window
 * start. The basin interior is x [ax-2, ax] (vsp_sl_plan), so a floating boat
 * (Beta width 1.5) has centre range [ax-1.25, ax-0.75]; Beta boats have
 * stepHeight 0 (Entity.java:93), so whether a boat pushed west rides over
 * the rim (top = water level) or pins on its face depends on its bob phase
 * at contact. HW5b (real Wii) pinned at ax-1.25 in the first window; the
 * second window then drove west into the face for 25 s (ride_boat_st1),
 * while HW5 had crested the rim. Facing the farther wall gives every window
 * >= 0.35 m of open water whatever the phase was (RIDE needs 0.25). */
float vehicle_smoke_boat_yaw(float boat_x, int32_t anchor_x) {
	return boat_x < (float)anchor_x - 1.0F ? 1.5707964F : -1.5707964F;
}

static bool vs_hop_aim_point(uint8_t kind, const float eye[3], float out[3]) {
	float box[6];
	if(!V.targets[kind].id || !vsp_client_entity_box(0, V.targets[kind].id, box))
		return vs_target_point(0, kind, out);
	vehicle_smoke_hull_aim(box, eye, out);
	return true;
}

/* Client LOS for the attempt record: the server's own rule, opaque cells
 * occlude (fences, rails, water do not). */
static bool vs_los(const struct vehicle_smoke_ctx* ctx, unsigned view,
				   const float a[3], const float b[3]) {
	if(!ctx->block_at)
		return false;
	float d = vs_dist(a, b);
	unsigned steps = (unsigned)(d / 0.05F) + 1u;
	/* The ray ends where it reaches the TARGET's box, not at its origin
	 * point: a cart resting against its end stop has its origin inside
	 * the stop's cell (measured: (0.50,4.65,3.08) in stone (0,4,3)) while
	 * the camera ray demonstrably hits the cart. 0.49 m = the smallest
	 * vehicle half-width (EntityMinecart setSize 0.98). */
	float stop_t = d > 0.49F ? 1.0F - 0.49F / d : 0.0F;
	for(unsigned i = 1; i < steps; i++) {
		float t = (float)i / (float)steps;
		if(t >= stop_t)
			break;
		int x = (int)floorf(a[0] + (b[0] - a[0]) * t);
		int y = (int)floorf(a[1] + (b[1] - a[1]) * t);
		int z = (int)floorf(a[2] + (b[2] - a[2]) * t);
		uint8_t type = 0;
		if(!ctx->block_at(view, x, y, z, &type))
			return false;
		if(type != BLOCK_AIR && type != BLOCK_WATER_STILL
		   && type != BLOCK_WATER_FLOW && type != BLOCK_RAIL
		   && type != BLOCK_FENCE) {
			return false;
		}
	}
	return true;
}

static void vs_frame(struct input_player_frame* f) {
	memset(f, 0, sizeof(*f));
	f->connected = true;
	f->category = INPUT_CAT_NUNCHUK;
	for(int b = 0; b < IB_COUNT; b++)
		f->glyphs[b] = -1;
}

static uint32_t vs_new_op(void) {
	return ++V.next_op;
}

/* ---- main-owned records -------------------------------------------------- */

static void vs_push_phase(enum vsp_event ev, enum vsp_phase ph) {
	struct vsp_record r;
	vs_rec(&r, ev, VSP_PRODUCTION, 0);
	r.u.phase.phase_index = (uint32_t)ph;
	r.u.phase.pending_markers = 0;
	r.u.phase.open_ops = 0;
	(void)vs_push(&r);
}

static void vs_push_attempt(uint8_t action, uint8_t channel, unsigned view,
							uint8_t kind, uint8_t button, uint32_t op,
							const struct vehicle_smoke_ctx* ctx,
							uint32_t request_seq, bool desired) {
	struct client_session* who = view ? ctx->guest : ctx->host;
	struct vsp_record r;
	vs_rec(&r, VSP_EV_ACTION_ATTEMPT, VSP_SYNTHETIC, op);
	struct vsp_p_action_attempt* p = &r.u.attempt;
	p->action = action;
	p->channel = channel;
	p->source.slot = (uint8_t)view;
	p->source.generation = view ? who->local_generation : 0;
	p->source.wire_id = who->local_player ? who->local_player->id : 0;
	p->source.input_epoch = view ? vsp_client_actor_epoch(view) : ctx->epoch;
	struct vsp_vc vc;
	(void)vs_vc(view, kind, &vc);
	p->target = vc.target;
	p->button = button;
	p->edge_seq = button != VSP_BTN_NONE ? ++V.edge_seq : 0;
	p->held_ms = 0;
	p->request_seq = request_seq;
	p->desired_occupied = desired;
	float eye[3], tgt[3];
	vs_eye(who, eye);
	if(vs_target_point(view, kind, tgt)) {
		p->reach_m = vs_dist(eye, tgt);
		p->los = vs_los(ctx, view, eye, tgt);
	}
	p->camera_hit = who->camera_hit.entity_hit ? who->camera_hit.entity_id : 0;
	(void)vs_push(&r);
}

/* `view` names the acting owner (0 host, 1 guest): the result's source is
 * that owner's actor. `request_seq` is the operation's own request (the
 * attempt's), not whatever stale ride state the client still holds. */
static void vs_push_client_result(uint8_t action, uint8_t status, uint8_t kind,
								  uint32_t op,
								  const struct vehicle_smoke_ctx* ctx,
								  const char* reason, unsigned view,
								  uint32_t request_seq) {
	struct vsp_record r;
	vs_rec(&r, VSP_EV_ACTION_RESULT, VSP_PRODUCTION, op);
	snprintf(r.reason, sizeof(r.reason), "%s", reason ? reason : "");
	struct vsp_p_action_result* p = &r.u.result;
	p->action = action;
	p->site = VSP_SITE_CLIENT;
	p->status = status;
	(void)vs_c(ctx, 0, &p->u.client.host);
	(void)vs_vc(0, kind, &p->u.client.vehicle);
	if(view) {
		struct vsp_c g;
		(void)vs_c(ctx, 1, &g);
		p->source = g.actor;
		struct vsp_vc gv;
		(void)vs_vc(1, kind, &gv);
		p->target = gv.target;
	} else {
		p->source = p->u.client.host.actor;
		p->target = p->u.client.vehicle.target;
	}
	p->request_seq = request_seq;
	p->ride_epoch = p->u.client.vehicle.ride_epoch;
	(void)vs_push(&r);
}

static void vs_push_client_iso(uint8_t stage, uint32_t op, uint8_t kind,
							   const struct vehicle_smoke_ctx* ctx) {
	struct vsp_record r;
	vs_rec(&r, VSP_EV_ISOLATION, VSP_PRODUCTION, op);
	struct vsp_p_isolation* p = &r.u.iso;
	p->stage = stage;
	p->owner = VSP_STAGE_CLIENT;
	p->attempt_op = op;
	(void)vs_c(ctx, 0, &p->u.client.host);
	(void)vs_c(ctx, 1, &p->u.client.guest);
	(void)vs_vc(0, kind, &p->u.client.vehicle);
	struct vsp_client_counters cc;
	vsp_client_counters(&cc);
	p->guest_follow_count = cc.guest_follow_count;
	p->unexpected_route_count = cc.unexpected_route_count;
	p->unexpected_attach_count = cc.unexpected_attach_count;
	/* guest pose delta: the client guest body against the bracket start,
	 * accumulated per frame by vs_ciso_track; C0 supplies no sample. */
	if(stage == VSP_STAGE_BEFORE) {
		V.ciso_open = true;
		V.ciso_max = 0.0F;
		V.ciso_have_pose = ctx->guest->local_player != NULL;
		V.ciso_gen = ctx->guest->local_generation;
		if(V.ciso_have_pose)
			memcpy(V.ciso_pose0, ctx->guest->local_player->pos,
				   sizeof(V.ciso_pose0));
	} else
		V.ciso_open = false;
	p->guest_pose_delta_max_m = V.ciso_max;
	(void)vs_push(&r);
}

static void vs_ciso_track(const struct vehicle_smoke_ctx* ctx) {
	if(!V.ciso_open || !ctx->guest->local_player)
		return;
	/* A readmission is a new guest lifetime: reseed from its first
	 * installed pose (same rule as the server bracket). */
	if(V.ciso_have_pose && ctx->guest->local_generation != V.ciso_gen)
		V.ciso_have_pose = false;
	if(!V.ciso_have_pose) { /* first genuinely installed body in the bracket */
		memcpy(V.ciso_pose0, ctx->guest->local_player->pos, sizeof(V.ciso_pose0));
		V.ciso_have_pose = true;
		V.ciso_gen = ctx->guest->local_generation;
		return;
	}
	float d = vs_dist(ctx->guest->local_player->pos, V.ciso_pose0);
	if(d > V.ciso_max)
		V.ciso_max = d;
}

static void vs_push_ui(uint8_t action, uint32_t player, uint8_t screen,
					   uint8_t button, uint32_t held_ms, bool delivered,
					   uint32_t op, const struct vehicle_smoke_ctx* ctx) {
	struct vsp_record r;
	vs_rec(&r, VSP_EV_UI_ACTION, VSP_SYNTHETIC, op);
	struct vsp_p_ui_action* p = &r.u.ui;
	p->action = action;
	p->player = player;
	p->screen = screen;
	p->button = button;
	p->held_ms = held_ms;
	p->edge_seq = button != VSP_BTN_NONE ? ++V.edge_seq : 0;
	p->delivered = delivered;
	p->input_epoch = player ? vsp_client_actor_epoch(player) : ctx->epoch;
	(void)vs_push(&r);
}

/* ---- the host interaction sub-machine ----------------------------------- */

static void vs_hop_start(uint8_t action, uint8_t kind, uint8_t button,
						 uint8_t expect) {
	memset(&V.hop, 0, sizeof(V.hop));
	V.hop.active = true;
	V.hop.action = action;
	V.hop.kind = kind;
	V.hop.target = V.targets[kind].id;
	V.hop.button = button;
	V.hop.expect = expect;
	V.hop.op = vs_new_op();
	V.hop.st_ms = V.now_ms;
}

static void vs_hop_stage(unsigned st) {
	V.hop.st = st;
	V.hop.st_ms = V.now_ms;
}

static enum input_button vs_ib(uint8_t button) {
	switch(button) {
		case VSP_BTN_ACTION1: return IB_ACTION1;
		case VSP_BTN_ACTION2: return IB_ACTION2;
		case VSP_BTN_JUMP: return IB_JUMP;
		default: return IB_GUI_CLICK;
	}
}

static const char* const k_act_name[] = {
	"saddle", "mount", "dismount", "switch", "guest_ui", "guest_use",
	"guest_cart_intent", "guest_cart_control"};

static void vs_hop_fail(const char* what) {
	const struct vs_hop* h = &V.hop;
	fprintf(stderr, "[vehicle_smoke] O15 hop-fail %s_st%u kind=%u target=%u: "
			"aim frames=%u gate=%u none=%u other=%u(last id %u) far=%u notgt=%u; "
			"last eye=(%.3f,%.3f,%.3f) tgt=(%.3f,%.3f,%.3f) dist=%.3f\n",
			what, h->st, (unsigned)h->kind, (unsigned)h->target, (unsigned)h->d_frames,
			(unsigned)h->d_gate, (unsigned)h->d_none, (unsigned)h->d_other,
			(unsigned)h->d_other_id, (unsigned)h->d_far, (unsigned)h->d_notgt,
			h->d_eye[0], h->d_eye[1], h->d_eye[2], h->d_tgt[0], h->d_tgt[1],
			h->d_tgt[2], h->d_dist);
	char code[96];
	snprintf(code, sizeof(code), "%s_%s_st%u", k_act_name[V.hop.action], what,
			 V.hop.st);
	vs_fail(code);
}

/* 0 running, 1 done, -1 failed. The host keeps looking at the target and
 * its five script buttons stay NEUTRAL except on the single press frame. */
static int vs_hop_step(const struct vehicle_smoke_ctx* ctx,
					   struct vehicle_smoke_actions* out) {
	struct vs_hop* h = &V.hop;
	if(!h->active)
		return 1;
	out->host_buttons_armed = true;
	float eye[3], tgt[3];
	vs_eye(ctx->host, eye);
	bool have_tgt = vs_hop_aim_point(h->kind, eye, tgt);
	if(have_tgt) {
		out->host_look = true;
		vs_aim(eye, tgt, &out->host_yaw, &out->host_pitch);
		/* Look down at the mount being ridden. O06: the cart's JUMP dismount
		 * too -- every attempt (JUMP included) names a target the real
		 * camera hits (grader operation(): camera_hit == target); first
		 * reached once the cart's p1 could get this far. */
		if((h->kind == VSP_PIG || h->kind == VSP_CART)
		   && h->action == VSP_ACT_DISMOUNT)
			out->host_pitch = 3.0F;
	}
	if(V.now_ms - h->st_ms > VS_STAGE_MS) {
		vs_hop_fail("timeout");
		return -1;
	}
	switch(h->st) {
		case 0: { /* aim; wait for a settled real camera hit */
			h->d_frames++;
			if(!vs_paced() || V.last_edge
			   || !client_session_host_gameplay_active(ctx->host)) {
				h->d_gate++;
				return 0;
			}
			bool hit = ctx->host->camera_hit.entity_hit
				&& ctx->host->camera_hit.entity_id == h->target;
			memcpy(h->d_eye, eye, sizeof(h->d_eye));
			if(have_tgt)
				memcpy(h->d_tgt, tgt, sizeof(h->d_tgt));
			h->d_dist = have_tgt ? vs_dist(eye, tgt) : -1.0F;
			if(!have_tgt)
				h->d_notgt++;
			else if(!ctx->host->camera_hit.entity_hit)
				h->d_none++;
			else if(!hit) {
				h->d_other++;
				h->d_other_id = ctx->host->camera_hit.entity_id;
			} else if(vs_dist(eye, tgt) > VS_REACH_RAY + 1.5F)
				h->d_far++;
			if(!hit || !have_tgt || vs_dist(eye, tgt) > VS_REACH_RAY + 1.5F)
				return 0;
			vs_hop_stage(1);
			return 0;
		}
		case 1: { /* the press: the attempt is recorded on the SAME frame the
				   * edge is delivered, and only if the camera still hits the
				   * target THEN. A ridden pig wanders under the host (Beta
				   * pig AI), so a hit seen one frame earlier can be gone on
				   * the press frame -- the client then sends nothing and the
				   * attempt would name a press that never reached the wire
				   * (measured: pig->boat switch_timeout, hit=0 on the press
				   * frame). Lost hit: back to aiming, nothing recorded. */
			bool hit = ctx->host->camera_hit.entity_hit
				&& ctx->host->camera_hit.entity_id == h->target;
			if(!hit || !have_tgt || vs_dist(eye, tgt) > VS_REACH_RAY + 1.5F) {
				vs_hop_stage(0);
				return 0;
			}
			vsp_set_op(h->op);
			/* request_seq: only a CART interaction authors a ride request,
			 * and the press allocates exactly next_request_seq + 1 (0 is
			 * skipped) -- entity_minecart.c (mount) / entity_local_player.c
			 * (JUMP dismount). Boat/pig requests carry none. The grader
			 * checks it against the authority's value from the real RPC. */
			uint32_t req = 0;
			if(h->kind == VSP_CART) {
				req = ctx->host->ride.next_request_seq + 1u;
				if(req == 0)
					req = 1;
			}
			h->request_seq = req;
			vs_push_attempt(h->action, VSP_CH_HOST_BUTTONS, 0, h->kind,
							h->button, h->op, ctx, req,
							h->expect == VS_EXPECT_MOUNTED);
			h->edge_seq = V.edge_seq;
			out->host_buttons[vs_ib(h->button)] = true;
			h->press_ms = V.now_ms;
			V.last_edge = true;
			vs_hop_stage(2);
			return 0;
		}
		case 2: /* neutral again */
			vs_hop_stage(3);
			return 0;
		case 3: /* the AUTHORITY's result for exactly this op */
			if(V.obs.auth_op != h->op)
				return 0;
			if(V.obs.auth_status != VSP_ST_APPLIED) {
				char what[64];
				snprintf(what, sizeof(what), "refused_%.24s", V.obs.auth_reason);
				vs_hop_fail(what);
				return -1;
			}
			h->serial = V.obs.auth_serial;
			vs_hop_stage(4);
			return 0;
		case 4: { /* the client settles the same relation */
			struct vsp_c c;
			struct vsp_vc vc;
			if(!vs_c(ctx, 0, &c))
				return 0;
			(void)vs_vc(0, h->kind, &vc);
			bool settled = h->expect == VS_EXPECT_MOUNTED
				? vs_client_mounted(&c, &vc)
				: h->expect == VS_EXPECT_DETACHED ? vs_client_detached(&c)
				: (vc.alive && vc.saddled);
			if(!settled)
				return 0;
			if(!vs_paced())
				return 0;
			vs_push_client_result(h->action, VSP_ST_APPLIED, h->kind, h->op,
								  ctx, "", 0, h->request_seq);
			h->active = false;
			return 1;
		}
		default:
			vs_hop_fail("bad_stage");
			return -1;
	}
}

/* ---- the P2 negative probe sub-machine ----------------------------------- */

static void vs_probe_start(uint8_t kind, uint8_t channel, uint8_t rpc,
						   bool occupied) {
	memset(&V.probe, 0, sizeof(V.probe));
	V.probe.active = true;
	V.probe.kind = kind;
	V.probe.channel = channel;
	V.probe.rpc = rpc;
	V.probe.occupied = occupied;
	V.probe.target = V.targets[kind].id;
	V.probe.op = vs_new_op();
	V.probe.st_ms = V.now_ms;
	V.probe.action = channel == VSP_CH_GUEST_FRAME ? VSP_ACT_GUEST_UI
		: rpc == VSP_RPC_MINECART_RIDE_INTENT ? VSP_ACT_GUEST_CART_INTENT
		: rpc == VSP_RPC_MINECART_CONTROL ? VSP_ACT_GUEST_CART_CONTROL
		: VSP_ACT_GUEST_USE;
}

static void vs_probe_stage(unsigned st) {
	V.probe.st = st;
	V.probe.st_ms = V.now_ms;
}

static int vs_probe_step(const struct vehicle_smoke_ctx* ctx,
						 struct vehicle_smoke_actions* out) {
	struct vs_probe* p = &V.probe;
	if(!p->active)
		return 1;
	/* the guest stays a connected, neutral, driven device throughout */
	out->guest_frame_valid = true;
	out->drive_guest_body = true;
	vs_frame(&out->guest_frame);
	float eye[3], tgt[3];
	vs_eye(ctx->guest, eye);
	if(vs_target_point(1, p->kind, tgt)) {
		out->guest_look = true;
		vs_aim(eye, tgt, &out->guest_yaw, &out->guest_pitch);
	}
	if(V.now_ms - p->st_ms > VS_STAGE_MS) {
		char code[48];
		snprintf(code, sizeof(code), "probe_%s_%s_st%u",
				 vsp_kind_name((enum vsp_kind)p->kind),
				 k_act_name[p->action], p->st);
		vs_fail(code);
		return -1;
	}
	switch(p->st) {
		case 0: /* open BOTH owners' brackets before any delivery */
			if(!vs_paced())
				return 0;
			{
				/* The host must be at rest first: a boat's client steering
				 * decays for a few frames after the last forward input, and a
				 * bracket opened then would record a changing host. */
				struct vsp_c hc;
				if(!vs_c(ctx, 0, &hc) || hc.steer[0] != 0.0F || hc.steer[1] != 0.0F) {
					p->rest_au = 0;
					return 0;
				}
				/* the server's rider steering follows the client's input
				 * stream: require >= 10 active server updates of rest
				 * (measured: op 4 bracket opened with server boat steer
				 * still -0.02 while the client already read 0) */
				if(!p->rest_au)
					p->rest_au = vsp_active_update() | 1u;
				if(vsp_active_update() - p->rest_au < 10u
				   || !vsp_server_host_at_rest(10u))
					return 0;
			}
			vsp_iso_target(p->target);
			vsp_set_op(p->op);
			/* The authority cannot see WHICH channel delivered a guest
			 * ENTITY_USE (frame press vs owned RPC); the operation names
			 * its action so the result carries the attempt's action. */
			vsp_set_op_action(p->op, p->action);
			vsp_client_counters_window_reset(true, false, 0);
			vsp_iso_bracket(p->op, true);
			vs_push_client_iso(VSP_STAGE_BEFORE, p->op, p->kind, ctx);
			vs_probe_stage(1);
			return 0;
		case 1:
			if(V.obs.iso_server_before != p->op)
				return 0;
			vs_probe_stage(2);
			return 0;
		case 2: { /* deliver */
			if(p->channel == VSP_CH_GUEST_FRAME) {
				if(!(ctx->guest->camera_hit.entity_hit
					 && ctx->guest->camera_hit.entity_id == p->target))
					return 0;
				vs_push_attempt(p->action, VSP_CH_GUEST_FRAME, 1, p->kind,
								VSP_BTN_ACTION2, p->op, ctx, 0, true);
				vs_probe_stage(3);
				return 0;
			}
			struct server_rpc call;
			memset(&call, 0, sizeof(call));
			uint32_t seq = 0;
			if(p->rpc == VSP_RPC_ENTITY_USE) {
				call.type = SRPC_ENTITY_USE;
				call.payload.entity_use.entity_id = p->target;
				call.payload.entity_use.desired_occupied = true;
			} else if(p->rpc == VSP_RPC_MINECART_RIDE_INTENT) {
				seq = ++V.probe_request_seq;
				call.type = SRPC_MINECART_RIDE_INTENT;
				call.payload.minecart_ride_intent.entity_id = p->target;
				call.payload.minecart_ride_intent.input_epoch = ctx->epoch;
				call.payload.minecart_ride_intent.request_seq = seq;
				call.payload.minecart_ride_intent.desired_occupied = true;
			} else {
				struct vsp_vc vc;
				(void)vs_vc(1, p->kind, &vc);
				call.type = SRPC_MINECART_CONTROL;
				call.payload.minecart_control.entity_id = p->target;
				call.payload.minecart_control.ride_epoch = vc.ride_epoch;
				call.payload.minecart_control.input_seq = 1;
				call.payload.minecart_control.positive_steps = 1;
				call.payload.minecart_control.negative_steps = 0;
			}
			vs_push_attempt(p->action, VSP_CH_OWNED_RPC, 1, p->kind,
							VSP_BTN_NONE, p->op, ctx, seq, true);
			/* The owned RPC's admission answer is itself an observation: a
			 * slot-1 refusal is recorded by server_interface's ingress site,
			 * an admitted ENTITY_USE by the authority's result. */
			(void)(V.hooks.rpc_guest && V.hooks.rpc_guest(V.hooks.user, &call));
			p->delivered_active = vsp_active_update();
			vs_probe_stage(4);
			return 0;
		}
		case 3: /* the guest-frame press: one frame */
			out->guest_frame.pressed[IB_ACTION2] = true;
			out->guest_frame.down[IB_ACTION2] = true;
			p->delivered_active = vsp_active_update();
			vs_probe_stage(4);
			return 0;
		case 4: { /* settle: >= 10 active updates and the owner's answer */
			if(vsp_active_update() - p->delivered_active
			   < VEHICLE_SMOKE_PROBE_UPDATES)
				return 0;
			bool answered;
			if(p->channel == VSP_CH_GUEST_FRAME && p->kind == VSP_CART)
				answered = true; /* client_noop: nothing reaches the wire */
			else if(p->channel == VSP_CH_GUEST_FRAME || p->rpc == VSP_RPC_ENTITY_USE)
				answered = V.obs.auth_op == p->op;
			else
				answered = V.obs.ingress_op == p->op;
			if(!answered)
				return 0;
			if(answered && V.obs.auth_op == p->op
			   && V.obs.auth_status == VSP_ST_APPLIED) {
				vs_fail("guest_probe_applied");
				return -1;
			}
			if(p->channel == VSP_CH_GUEST_FRAME && p->kind == VSP_CART) {
				struct vsp_c g;
				bool present = vs_c(ctx, 1, &g);
				if(present && (g.ride_pending || g.mount_count)) {
					vs_fail("guest_cart_ui_predicted");
					return -1;
				}
				vs_push_client_result(VSP_ACT_GUEST_UI, VSP_ST_CLIENT_NOOP,
									  p->kind, p->op, ctx, "guest_cart_ui_noop",
									  1, 0);
			}
			vsp_iso_bracket(p->op, false);
			vs_probe_stage(5);
			return 0;
		}
		case 5:
			if(V.obs.iso_server_after != p->op || !vs_paced())
				return 0;
			vs_push_client_iso(VSP_STAGE_AFTER, p->op, p->kind, ctx);
			{
				struct vsp_client_counters cc;
				vsp_client_counters(&cc);
				if(cc.route_violation_sticky || cc.unexpected_attach_count
				   || cc.unexpected_route_count || V.obs.iso_server_attach) {
					vs_fail("guest_isolation_violation");
					return -1;
				}
			}
			p->active = false;
			return 1;
		default:
			vs_fail("probe_bad_stage");
			return -1;
	}
}

/* ---- the ride window sub-machine ----------------------------------------- */

static void vs_ride_start(uint8_t kind) {
	memset(&V.ride, 0, sizeof(V.ride));
	V.ride.active = true;
	V.ride.kind = kind;
	V.ride.target = V.targets[kind].id;
	V.ride.window = ++V.window;
	V.ride.st_ms = V.now_ms;
	/* Motion is a RIDE requirement only for the run's own kind (the pig
	 * cannot be steered: a switch window onto it only proves the follow). */
	V.ride.need_motion = kind == V.cfg.kind;
}

static int vs_ride_step(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	struct vs_ride* w = &V.ride;
	if(!w->active)
		return 1;
	out->host_buttons_armed = true;
	out->host_look = true;
	/* boat: steer toward the farther basin wall (O25); cart: face along the rails
	 * (+Z, away from the host) so the throttle resolves forward; pig: no
	 * steering exists (EntityPig) -- the look only frames the ride. */
	out->host_yaw = w->kind == VSP_BOAT ? w->boat_yaw : 0.0F;
#if !VSP_CART_OPEN_END
	/* O09 (CONTRACT-v1.3): the cart track runs east-west between two stops;
	 * face the farther stop, fixed at window start (a stopped cart's throttle
	 * follows the rider's facing, entity_minecart_resolve_throttle). */
	if(w->kind == VSP_CART)
		out->host_yaw = w->cart_yaw;
#endif
	out->host_pitch = 1.9F;
	if(V.now_ms - w->st_ms > VS_RIDE_MS) {
		char code[48];
		snprintf(code, sizeof(code), "ride_%s_st%u",
				 vsp_kind_name((enum vsp_kind)w->kind), w->st);
		vs_fail(code);
		return -1;
	}
	struct vsp_vc vc;
	(void)vs_vc(0, w->kind, &vc);
	switch(w->st) {
		case 0:
			if(!vs_paced())
				return 0;
			memcpy(w->start, vc.pos, sizeof(w->start));
			/* O09: +X (yaw +pi/2) from the west half, else -X. */
			w->boat_yaw = vehicle_smoke_boat_yaw(vc.pos[0], V.anchor[0]);
			w->cart_yaw = vc.pos[0] < (float)V.anchor[0] ? 1.5707964F
														  : -1.5707964F;
			/* O06: the fixture track is two rail cells (az+2, az+3) ending in
			 * a SOLID stop at az+4 that -- with Beta's moveEntity collision --
			 * really stops the cart at its face (az+3.51). Every cart window
			 * rides +Z (RIDE requires positive_steps advancement) and brakes
			 * to rest after 0.3 m, so the first ride leaves room for the
			 * second (ridden displacement is 0.75 x speed). */
			w->cart_pos0 = vc.positive_steps;
			w->cart_neg0 = vc.negative_steps;
			w->cart_brake_steps = 0;
			w->cart_braking = false;
			vsp_client_counters_window_reset(false, true, w->target);
			vsp_ride_window(w->window, w->target, true);
			w->st = 1;
			return 0;
		case 1: {
			float dx = vc.pos[0] - w->start[0], dz = vc.pos[2] - w->start[2];
			float d = sqrtf(dx * dx + dz * dz);
			if(d > w->disp)
				w->disp = d;
			struct vsp_client_counters cc;
			vsp_client_counters(&cc);
			if(w->kind == VSP_BOAT)
				out->host_buttons[IB_FORWARD] = true;
			else if(w->kind == VSP_CART) {
				/* O06: accelerate until the ride has proven its motion
				 * (>= 0.3 m, RIDE needs 0.25), then brake to rest so the cart
				 * never runs off the open -Z rail end. The rider throttle
				 * follows the CURRENT motion while moving
				 * (entity_minecart_resolve_throttle): FORWARD accelerates,
				 * BACKWARD brakes; a cart at rest takes the facing. */
				if(!w->cart_braking && cc.ride_disp_max >= 0.3F) {
					w->cart_braking = true;
					w->cart_brake_steps = vc.positive_steps - w->cart_pos0;
				}
				/* Brake with exactly as many 0.01 steps as were authored
				 * forward (counted on the client's own cumulative step
				 * counters, so a tick-less frame can neither skip nor double
				 * a step). Ridden friction during the ride leaves at most a
				 * tiny reverse residual: the cart rests on the rail instead of
				 * oscillating (a time-held brake reversed it, measured: 55/55
				 * steps, cart coasted off the -Z rail end to z 1.79). */
				if(!w->cart_braking)
					out->host_buttons[IB_FORWARD] = true;
				else if(vc.negative_steps - w->cart_neg0 < w->cart_brake_steps)
					out->host_buttons[IB_BACKWARD] = true;
			}
			if(cc.ride_eligible < VEHICLE_SMOKE_RIDE_UPDATES + 10u
			   || cc.host_follow_count < VEHICLE_SMOKE_RIDE_UPDATES + 10u
			   || (w->need_motion
				   && cc.ride_disp_max < VEHICLE_SMOKE_RIDE_DISPLACEMENT))
				return 0;
			vsp_ride_window(w->window, w->target, false);
			w->st = 2;
			return 0;
		}
		case 2: {
			if(V.obs.ride_window != w->window || !vs_paced())
				return 0;
			if(V.obs.ride_eligible < VEHICLE_SMOKE_RIDE_UPDATES
			   || V.obs.ride_missing || V.obs.ride_dup
			   || (w->need_motion
				   && V.obs.ride_disp < VEHICLE_SMOKE_RIDE_DISPLACEMENT)) {
				vs_fail("ride_server_window_short");
				return -1;
			}
			struct vsp_client_counters cc;
			vsp_client_counters(&cc);
			struct vsp_record r;
			vs_rec(&r, VSP_EV_RIDE_SAMPLE, VSP_PRODUCTION, vsp_op_now());
			struct vsp_p_ride_sample* p = &r.u.ride;
			p->stage = VSP_STAGE_CLIENT;
			(void)vs_c(ctx, 0, &p->u.client.host);
			p->u.client.vehicle = vc;
			p->target = vc.target;
			p->window_id = w->window;
			/* CONTRACT-v1.1 Q3: eligible/tick/missing/duplicate come from
			 * the real client dispatch tap (main.c around
			 * entities_client_tick), accumulated per pass over the whole
			 * window; follow/seat stay the separate follow observation. */
			p->samples = cc.ride_eligible;
			p->eligible_updates = cc.ride_eligible;
			p->tick_calls = cc.ride_tick_calls;
			p->missing_calls = cc.ride_missing;
			p->duplicate_calls = cc.ride_duplicate;
			p->follow_count = cc.host_follow_count;
			p->displacement_m = cc.ride_disp_max;
			p->seat_error_max_m = cc.host_seat_error_max;
			/* Only a cart ride has a ride request; a boat/pig window may
			 * coexist with a historical (inactive) cart request. */
			p->request_seq = w->kind == VSP_CART
				? p->u.client.host.ride_request_seq : 0u;
			(void)vs_push(&r);
			w->active = false;
			return 1;
		}
		default:
			return -1;
	}
}

/* ---- phases --------------------------------------------------------------- */

static void vs_step(unsigned next) {
	V.step = next;
	V.step_start_ms = V.now_ms;
	V.flag_a = V.flag_b = false;
}

static uint32_t vs_phase_budget(enum vsp_phase ph) {
	switch(ph) {
		case VSP_P0: return VEHICLE_SMOKE_P0_MS;
		case VSP_P1: return VEHICLE_SMOKE_P1_MS;
		case VSP_P2: return VEHICLE_SMOKE_P2_MS;
		case VSP_P3: return VEHICLE_SMOKE_P3_MS;
		case VSP_P4: return VEHICLE_SMOKE_P4_MS;
		case VSP_P5: return VEHICLE_SMOKE_P5_MS;
		default: return VEHICLE_SMOKE_TOTAL_MS;
	}
}

/* phase_end only after the old phase's observations are drained and the
 * server has run two full updates since the request (no in-flight update can
 * stamp an old-phase record after the next begin). */
static bool vs_end_phase(enum vsp_phase next) {
	if(V.hop.active || V.probe.active || V.ride.active || !vs_drained())
		return false;
	if(!V.closing) {
		V.closing = true;
		V.close_mark = vsp_server_update();
		return false;
	}
	if(vsp_server_update() < V.close_mark + 2u)
		return false;
	vs_push_phase(VSP_EV_PHASE_END, V.phase);
	V.closing = false;
	V.phase = next;
	vsp_set_phase(next);
	V.phase_start_ms = V.now_ms;
	vs_step(0);
	if(next <= VSP_P5)
		vs_push_phase(VSP_EV_PHASE_BEGIN, next);
	return true;
}

/* The directed switch sequence per run kind (CONTRACT-v1 coverage table). */
static const uint8_t k_switch_seq[VSP_KIND_COUNT][4] = {
	[VSP_BOAT] = {VSP_BOAT, VSP_PIG, VSP_CART, VSP_BOAT},
	[VSP_PIG] = {VSP_PIG, VSP_BOAT, VSP_CART, VSP_PIG},
	[VSP_CART] = {VSP_CART, VSP_BOAT, VSP_PIG, VSP_CART},
};

static uint8_t vs_mount_button(uint8_t kind) {
	(void)kind;
	return VSP_BTN_ACTION2;
}

static uint8_t vs_dismount_button(uint8_t kind) {
	return kind == VSP_CART ? VSP_BTN_JUMP : VSP_BTN_ACTION2;
}

/* The P2 probe list for one target in one state. */
struct vs_probe_spec {
	uint8_t channel, rpc;
};
static unsigned vs_probe_specs(uint8_t kind, struct vs_probe_spec out[4]) {
	unsigned n = 0;
	out[n++] = (struct vs_probe_spec) {VSP_CH_GUEST_FRAME, VSP_RPC_ENTITY_USE};
	out[n++] = (struct vs_probe_spec) {VSP_CH_OWNED_RPC, VSP_RPC_ENTITY_USE};
	if(kind == VSP_CART) {
		out[n++] = (struct vs_probe_spec) {VSP_CH_OWNED_RPC,
										   VSP_RPC_MINECART_RIDE_INTENT};
		out[n++] = (struct vs_probe_spec) {VSP_CH_OWNED_RPC,
										   VSP_RPC_MINECART_CONTROL};
	}
	return n;
}

/* Run all probes for `kind` in the current occupancy state; true when done. */
static bool vs_probes(const struct vehicle_smoke_ctx* ctx,
					  struct vehicle_smoke_actions* out, uint8_t kind,
					  bool occupied) {
	struct vs_probe_spec specs[4];
	unsigned n = vs_probe_specs(kind, specs);
	if(V.probe.active) {
		int r = vs_probe_step(ctx, out);
		if(r <= 0)
			return false;
		V.seq_index++;
	}
	if(V.seq_index >= n) {
		V.seq_index = 0;
		return true;
	}
	vs_probe_start(kind, specs[V.seq_index].channel, specs[V.seq_index].rpc,
				   occupied);
	(void)vs_probe_step(ctx, out);
	return false;
}

/* Walk the guest to its standing cell (scripted owned frames). */
static int vs_guest_walk(const struct vehicle_smoke_ctx* ctx,
						 struct vehicle_smoke_actions* out) {
	out->guest_frame_valid = true;
	out->drive_guest_body = true;
	vs_frame(&out->guest_frame);
	float dx = V.guest_goal[0] - ctx->guest->camera.x;
	float dz = V.guest_goal[1] - ctx->guest->camera.z;
	float d = sqrtf(dx * dx + dz * dz);
	out->guest_look = true;
	out->guest_yaw = atan2f(dx, dz);
	out->guest_pitch = 1.5707964F;
	if(d < 0.25F)
		return 1;
	if(V.now_ms - V.step_start_ms > VS_WALK_MS) {
		vs_fail("guest_walk_timeout");
		return -1;
	}
	out->guest_frame.down[IB_FORWARD] = true;
	if(d < 0.6F && ((V.now_ms / 100u) % 2u))
		out->guest_frame.down[IB_FORWARD] = false; /* creep the last stretch */
	return 0;
}

static bool vs_guest_ready(const struct vehicle_smoke_ctx* ctx) {
	return ctx->guest && ctx->guest->local_gameplay_ready
		&& ctx->guest->local_player;
}

static void vs_phase_p0(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	switch(V.step) {
		case 0: /* create the fresh disposable world, once */
			if(!V.flag_a) {
				const char* conflict = V.hooks.autoload_conflict
					? V.hooks.autoload_conflict(V.hooks.user) : NULL;
				if(conflict) {
					vs_fail("autoload_latch_consumed");
					return;
				}
				if(!V.hooks.create_world
				   || !V.hooks.create_world(V.hooks.user,
											VEHICLE_SMOKE_RIG_WORLD)) {
					vs_fail("rig_world_create_refused");
					return;
				}
				V.flag_a = true;
			}
			if(ctx->world_loaded && !ctx->world_switch_pending && ctx->host->local_player
			   && client_session_host_gameplay_active(ctx->host)
			   && vs_guest_ready(ctx))
				vs_step(1);
			return;
		case 1: { /* place the guest beside the host (not in its body box) */
			int ax = (int)floorf(ctx->host->camera.x);
			int az = (int)floorf(ctx->host->camera.z);
			V.guest_goal[0] = (float)ax - 0.5F;
			/* within the 4.5 m entity ray of all three targets, clear of
			 * the host's rays to each of them (PC-run geometry) */
			V.guest_goal[1] = (float)az - 1.0F;
			vs_step(2);
			return;
		}
		case 2: {
			int r = vs_guest_walk(ctx, out);
			if(r == 1)
				vs_step(3);
			return;
		}
		case 3: /* the server fixture: blocks, real spawns, inventories */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(!vs_paced())
				return;
			V.fixture_op = vs_new_op();
			vsp_set_op(V.fixture_op);
			vsp_cmd_fixture(V.fixture_op);
			vs_step(4);
			return;
		case 4:
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(vsp_fixture_failure()) {
				vs_fail(vsp_fixture_failure());
				return;
			}
			if(!vsp_fixture_targets(V.targets, V.anchor))
				return;
			V.have_targets = true;
			vs_step(5);
			return;
		case 5: { /* every target visible to BOTH clients */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			struct vsp_vc a, b;
			for(unsigned k = 0; k < 3; k++)
				if(!vs_vc(0, (uint8_t)k, &a) || !a.alive || !vs_vc(1, (uint8_t)k, &b)
				   || !b.alive)
					return;
			vs_hop_start(VSP_ACT_SADDLE, VSP_PIG, VSP_BTN_ACTION1,
						 VS_EXPECT_SADDLED);
			/* The saddle-by-hit is fixture preparation: it runs under the
			 * fixture's own operation, which rig_ready closes. A separate
			 * operation would appear before the fixture op's first record
			 * and break first-appearance operation order. */
			V.hop.op = V.fixture_op;
			vs_step(6);
			return;
		}
		case 6: /* the REAL saddle-by-hit */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(vs_hop_step(ctx, out) == 1)
				vs_step(7);
			return;
		case 7: /* rig_ready (server, after the observed saddle) */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(!V.obs.rig_ready)
				return;
			if(!V.obs.rig_ok) {
				vs_fail("rig_not_ready");
				return;
			}
			V.base_inv_hash[0] = vsp_client_inventory_hash(ctx->host,
														   &V.base_inv_used[0]);
			V.base_inv_hash[1] = vsp_client_inventory_hash(ctx->guest,
														   &V.base_inv_used[1]);
			if(!V.base_inv_used[0] || !V.base_inv_used[1]
			   || V.base_inv_hash[0] == V.base_inv_hash[1]) {
				vs_fail("baseline_inventories_invalid");
				return;
			}
			vs_step(8);
			return;
		case 8:
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			(void)vs_end_phase(VSP_P1);
			return;
		default:
			vs_fail("p0_step");
			return;
	}
}

/* p1 sub-steps (V.step):
 *   0 mount K          1 ride K           2 probes K occupied
 *   3 dismount K       4 probes K empty   5 probes S1 empty
 *   6 probes S2 empty  7 remount K        8 switch K->S1
 *   9 probes S1 occ.  10 switch S1->S2   11 probes S2 occ.
 *  12 switch S2->K    13 end
 * where [K,S1,S2,K] is the run kind's directed switch sequence. */
static void vs_phase_p1(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	const uint8_t* q = k_switch_seq[V.cfg.kind];
	uint8_t K = q[0], S1 = q[1], S2 = q[2];
	out->guest_frame_valid = true;
	vs_frame(&out->guest_frame);
	switch(V.step) {
		case 0:
			if(!V.flag_a) {
				vs_hop_start(VSP_ACT_MOUNT, K, vs_mount_button(K),
							 VS_EXPECT_MOUNTED);
				V.flag_a = true;
			}
			if(vs_hop_step(ctx, out) == 1)
				vs_step(1);
			return;
		case 1:
			if(!V.flag_a) {
				vs_ride_start(K);
				V.flag_a = true;
			}
			if(vs_ride_step(ctx, out) == 1)
				vs_step(2);
			return;
		case 2:
			if(vs_probes(ctx, out, K, true))
				vs_step(3);
			return;
		case 3:
			if(!V.flag_a) {
				vs_hop_start(VSP_ACT_DISMOUNT, K, vs_dismount_button(K),
							 VS_EXPECT_DETACHED);
				V.flag_a = true;
			}
			if(vs_hop_step(ctx, out) == 1)
				vs_step(4);
			return;
		case 4:
			if(vs_probes(ctx, out, K, false))
				vs_step(5);
			return;
		case 5:
			if(vs_probes(ctx, out, S1, false))
				vs_step(6);
			return;
		case 6:
			if(vs_probes(ctx, out, S2, false))
				vs_step(7);
			return;
		case 7: /* the empty-target host positive control + remount */
			if(!V.flag_a) {
				vs_hop_start(VSP_ACT_MOUNT, K, vs_mount_button(K),
							 VS_EXPECT_MOUNTED);
				V.flag_a = true;
			}
			if(vs_hop_step(ctx, out) == 1)
				vs_step(8);
			return;
		case 8:
		case 10:
		case 12: {
			uint8_t to = V.step == 8 ? S1 : V.step == 10 ? S2 : K;
			if(!V.flag_a) {
				vs_hop_start(VSP_ACT_SWITCH, to, vs_mount_button(to),
							 VS_EXPECT_MOUNTED);
				V.flag_a = true;
			}
			/* Each switch is followed by a settled ride on the new vehicle:
			 * the client follow census proves the new relation is the one
			 * the body actually rides (switch_follow). */
			if(!V.flag_b) {
				if(vs_hop_step(ctx, out) == 1) {
					vs_ride_start(to);
					V.flag_b = true;
				}
				return;
			}
			if(vs_ride_step(ctx, out) == 1)
				vs_step(V.step + 1);
			return;
		}
		case 9:
			if(vs_probes(ctx, out, S1, true))
				vs_step(10);
			return;
		case 11:
			if(vs_probes(ctx, out, S2, true))
				vs_step(12);
			return;
		case 13:
			(void)vs_end_phase(VSP_P2);
			return;
		default:
			vs_fail("p1_step");
			return;
	}
}

/* The host's OWNED pause frame (Save and quit), production local_pause. */
static bool vs_host_menu(const struct vehicle_smoke_ctx* ctx,
						 struct vehicle_smoke_actions* out, unsigned* sub) {
	struct local_pause* pause = &ctx->host->pause;
	out->host_frame_valid = true;
	vs_frame(&out->host_frame);
	switch(*sub) {
		case 0: out->host_frame.pressed[IB_HOME] = true; (*sub)++; return false;
		case 1: if(pause->open) (*sub)++; return false;
		case 2:
			out->host_frame.pressed[IB_GUI_DOWN] = true;
			out->host_frame.pressed[IB_GUI_RIGHT] = true; (*sub)++; return false;
		case 3:
			if(pause->selected == 3) (*sub)++;
			else {
				out->host_frame.pressed[IB_GUI_DOWN] = true;
				out->host_frame.pressed[IB_GUI_RIGHT] = true;
			}
			return false;
		case 4: out->host_frame.pressed[IB_GUI_CLICK] = true; (*sub)++; return false;
		case 5: if(pause->page == LOCAL_PAUSE_HOST_CONFIRM) (*sub)++; return false;
		case 6: out->host_frame.pressed[IB_GUI_DOWN] = true; (*sub)++; return false;
		case 7: if(pause->selected == 1) (*sub)++; return false;
		case 8: out->host_frame.pressed[IB_GUI_CLICK] = true; (*sub)++; return false;
		default: return pause->exit_pending || !pause->open;
	}
}

static unsigned s_menu_sub;

static void vs_phase_p2(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	uint8_t K = (uint8_t)V.cfg.kind;
	switch(V.step) {
		case 0: /* bracket the save boundary (server+client) */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(!vs_paced())
				return;
			V.save_op = vs_new_op();
			vsp_set_op(V.save_op);
			vsp_client_counters_window_reset(true, false, 0);
			vsp_iso_target(0);
			vsp_iso_bracket(V.save_op, true);
			vs_push_client_iso(VSP_STAGE_BEFORE, V.save_op, K, ctx);
			s_menu_sub = 0;
			vs_step(1);
			return;
		case 1:
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(V.obs.iso_server_before == V.save_op) {
				/* O11 / S03 #2: the Save receipt is recorded by main.c at the
				 * ACCEPTED SRPC_UNLOAD_WORLD send (before the server can
				 * checkpoint), never inferred from exit_pending a frame later.
				 * The reset mark is taken BEFORE the click for the same reason. */
				V.mark_reset = ctx->world_reset_count;
				vsp_await_dispatch(VSP_UI_SAVE_EXIT, 0, VSP_SCR_PAUSE,
								   VSP_BTN_GUI_CLICK, ++V.edge_seq, V.save_op);
				vs_step(2);
			}
			return;
		case 2: /* real pause-menu Save and quit, mounted */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(!vs_host_menu(ctx, out, &s_menu_sub))
				return;
			if(ctx->host->pause.exit_failed && !ctx->host->pause.exit_pending) {
				vs_fail("save_exit_refused");
				return;
			}
			if(!vsp_dispatch_done(VSP_UI_SAVE_EXIT))
				return; /* the UNLOAD request has not been accepted yet */
			vs_step(3);
			return;
		case 3: /* durable checkpoint, then the actual reset */
			if(!V.obs.ck_durable)
				return;
			if(!V.obs.ck_ok) {
				vs_fail("checkpoint_not_durable");
				return;
			}
			if(ctx->world_reset_count == V.mark_reset || ctx->world_loaded)
				return;
			{
				struct vsp_record r;
				vs_rec(&r, VSP_EV_WORLD_RESET, VSP_PRODUCTION, V.save_op);
				r.u.reset.world_reset_count = ctx->world_reset_count;
				r.u.reset.checkpoint_id = V.obs.checkpoint_id;
				/* Q6: live receipts at the observation, main thread. */
				r.u.reset.world_loaded = ctx->world_loaded;
				r.u.reset.local_entity = ctx->host->local_player
					? ctx->host->local_player->id : 0;
				r.u.reset.source_world = (struct vsp_w) {
					.slot = V.targets[K].world.slot,
					.generation = V.targets[K].world.generation,
					.dimension = V.targets[K].world.dimension};
				(void)vs_push(&r);
			}
			vs_step(4);
			return;
		case 4: /* the real title */
			if(!ctx->at_title)
				return;
			{
				struct vsp_record r;
				vs_rec(&r, VSP_EV_TITLE, VSP_PRODUCTION, V.save_op);
				r.u.title.world_reset_count = ctx->world_reset_count;
				r.u.title.checkpoint_id = V.obs.checkpoint_id;
				r.u.title.world_loaded = ctx->world_loaded;
				r.u.title.autoload_consumed = V.hooks.autoload_conflict
					&& V.hooks.autoload_conflict(V.hooks.user) != NULL;
				(void)vs_push(&r);
			}
			vsp_expect_reload(V.obs.checkpoint_id);
			vs_step(5);
			return;
		case 5: /* ONE named same-directory autoload */
			if(!V.flag_a) {
				if(!V.hooks.arm_autoload
				   || !V.hooks.arm_autoload(V.hooks.user, VEHICLE_SMOKE_RIG_WORLD)) {
					vs_fail("autoload_arm_refused");
					return;
				}
				V.flag_a = true;
			}
			if((V.obs.reload_stages & 7u) == 7u && ctx->world_loaded
			   && !ctx->world_switch_pending && vs_guest_ready(ctx)
			   && client_session_host_gameplay_active(ctx->host))
				vs_step(6);
			return;
		case 6: { /* reload: pose before physics, remapped targets */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(memcmp(V.obs.reload_pose_bits, V.obs.pose_bits,
					  sizeof(V.obs.pose_bits)) != 0) {
				vs_fail("reload_pose_mismatch");
				return;
			}
			memcpy(V.targets, V.obs.reload_targets, sizeof(V.targets));
			struct vsp_c h, g;
			if(!vs_c(ctx, 0, &h) || !vs_c(ctx, 1, &g) || !vs_client_detached(&h)
			   || !vs_client_detached(&g) || !vs_paced())
				return;
			struct vsp_record r;
			vs_rec(&r, VSP_EV_RELOAD, VSP_PRODUCTION, V.save_op);
			r.u.reload.stage = VSP_RL_CLIENT_READY;
			r.u.reload.checkpoint_id = V.obs.checkpoint_id;
			r.u.reload.u.client_ready.host = h;
			r.u.reload.u.client_ready.guest = g;
			r.u.reload.u.client_ready.input_ack_epoch = ctx->epoch;
			(void)vs_push(&r);
			vs_step(7);
			return;
		}
		case 7: /* normal remount of the selected kind */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(!V.flag_a) {
				vs_hop_start(VSP_ACT_MOUNT, K, vs_mount_button(K),
							 VS_EXPECT_MOUNTED);
				V.flag_a = true;
			}
			if(vs_hop_step(ctx, out) == 1)
				vs_step(8);
			return;
		case 8: /* close the save bracket */
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(!vs_paced())
				return;
			vsp_iso_bracket(V.save_op, false);
			vs_step(9);
			return;
		case 9:
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			if(V.obs.iso_server_after != V.save_op || !vs_paced())
				return;
			vs_push_client_iso(VSP_STAGE_AFTER, V.save_op, K, ctx);
			vs_step(10);
			return;
		case 10:
			out->guest_frame_valid = true;
			vs_frame(&out->guest_frame);
			(void)vs_end_phase(VSP_P3);
			return;
		default:
			vs_fail("p2_step");
			return;
	}
}

/* O14 (CONTRACT-v1.3 two-home): the TRANSITION POINT. After p3 phase_begin
 * -- the p2 save bracket's server and client after records are admitted and
 * closed -- and BEFORE the death bracket arms, P2 walks by ordinary owned
 * input from the interaction home H1 to the remote death-phase home H2, then
 * stays neutral until BOTH owners acknowledge it has settled. No item exists
 * yet (the death drops are made by the later Respawn inside the bracket).
 * From the settle ack to disarm the server enforces guest >= N from D. */
#define VS_SETTLE_MS 5000u
#define VS_SETTLE_FRAMES 10u
static bool vs_far_home(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	switch(V.far_sub) {
		case 0:
			if(!V.have_targets || !vs_guest_ready(ctx)) {
				vs_fail("far_home_no_fixture");
				return false;
			}
			V.guest_goal[0] = (float)V.anchor[0] + VSP_HOME2_DX;
			V.guest_goal[1] = (float)V.anchor[2] + VSP_HOME2_DZ;
			V.step_start_ms = V.now_ms; /* vs_guest_walk's deadline base */
			V.far_sub = 1;
			return false;
		case 1: {
			int r = vs_guest_walk(ctx, out);
			if(r != 1)
				return false;
			V.far_start_ms = V.now_ms;
			V.far_still_frames = 0;
			V.far_last[0] = ctx->guest->camera.x;
			V.far_last[1] = ctx->guest->camera.y;
			V.far_last[2] = ctx->guest->camera.z;
			V.far_sub = 2;
			return false;
		}
		default: { /* neutral frames only (set by the caller) */
			const float dx = ctx->guest->camera.x - V.far_last[0];
			const float dy = ctx->guest->camera.y - V.far_last[1];
			const float dz = ctx->guest->camera.z - V.far_last[2];
			V.far_last[0] = ctx->guest->camera.x;
			V.far_last[1] = ctx->guest->camera.y;
			V.far_last[2] = ctx->guest->camera.z;
			if(dx * dx + dy * dy + dz * dz < 1e-8F)
				V.far_still_frames++;
			else
				V.far_still_frames = 0;
			if(V.far_still_frames >= VS_SETTLE_FRAMES
			   && vsp_server_guest_at_rest(VS_SETTLE_FRAMES) && vs_paced()) {
				vsp_far_home_enforce();
				V.far_home_done = true;
				return true;
			}
			if(V.now_ms - V.far_start_ms > VS_SETTLE_MS)
				vs_fail("guest_settle_timeout");
			return false;
		}
	}
}

static void vs_phase_p3(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	uint8_t K = (uint8_t)V.cfg.kind;
	out->guest_frame_valid = true;
	vs_frame(&out->guest_frame);
	if(!V.far_home_done) {
		if(vs_far_home(ctx, out))
			vs_step(0);
		return;
	}
	switch(V.step) {
		case 0:
			if(!vs_paced())
				return;
			V.iso_op = vs_new_op();
			vsp_set_op(V.iso_op);
			vsp_client_counters_window_reset(true, false, 0);
			vsp_iso_target(0);
			vsp_iso_bracket(V.iso_op, true);
			vs_push_client_iso(VSP_STAGE_BEFORE, V.iso_op, K, ctx);
			vs_step(1);
			return;
		case 1: /* the ONE disclosed lethal fixture call, while mounted */
			if(V.obs.iso_server_before != V.iso_op)
				return;
			/* The death operation IS the bracketed operation: the grader
			 * keys the p3 isolation on the death's op (no second op). */
			V.lethal_op = V.iso_op;
			vsp_set_op(V.lethal_op);
			vsp_cmd_lethal(V.lethal_op);
			vs_step(2);
			return;
		case 2:
			if(vsp_fixture_failure()) {
				vs_fail(vsp_fixture_failure());
				return;
			}
			if(!V.obs.death_server || !ctx->host_death_screen || !vs_paced())
				return;
			{
				struct vsp_record r;
				vs_rec(&r, VSP_EV_DEATH, VSP_PRODUCTION, V.lethal_op);
				r.u.death.stage = VSP_DEATH_UI;
				(void)vs_c(ctx, 0, &r.u.death.u.ui.host);
				/* Q4: the client's own input epoch at death. */
				V.death_epoch = r.u.death.u.ui.host.actor.input_epoch;
				(void)vs_push(&r);
			}
			V.hold_start_ms = V.now_ms;
			vs_step(3);
			return;
		case 3: /* a released click through the entry grace, then the hold */
			out->host_buttons_armed = true;
			/* O17 (R07 F3): own the host pointer for the death screen (the
			 * real consumer reads the global pointer, not the owned frame) */
			out->host_pointer_owned = true;
			if(V.now_ms - V.hold_start_ms < VS_DEATH_GRACE_MS)
				return;
			V.hold_start_ms = V.now_ms;
			vs_step(4);
			return;
		case 4:
			out->host_buttons_armed = true;
			out->host_pointer_owned = true;
			out->host_buttons[IB_GUI_CLICK] = true;
			/* The delivery record is written on the frame the delivered hold
			 * reaches the configured Respawn hold -- this frame's screen
			 * update is where the death UI commits and the request leaves,
			 * so the row precedes every server stage it causes. (Recording it
			 * only after REQUESTED was observed put it after released and
			 * requested: vehicle_respawn_order, measured on the PC run.) */
			if(!V.flag_b
			   && V.now_ms - V.hold_start_ms
				  >= (uint32_t)gstate.config_death_hold_respawn_ms) {
				vs_push_ui(VSP_UI_RESPAWN, 0, VSP_SCR_DEATH, VSP_BTN_GUI_CLICK,
						   V.now_ms - V.hold_start_ms, true, V.lethal_op, ctx);
				V.flag_b = true;
			}
			if(!(V.obs.respawn_stages & (1u << VSP_RS_REQUESTED))) {
				if(V.now_ms - V.hold_start_ms > VS_HOLD_MAX_MS)
					vs_fail("respawn_hold_not_accepted");
				return;
			}
			if(!V.flag_b) {
				vs_fail("respawn_committed_before_hold");
				return;
			}
			vs_step(5);
			return;
		case 5: { /* released / fresh / ack, then the settled fresh body */
			out->host_buttons_armed = true;
			unsigned need = (1u << VSP_RS_RELEASED) | (1u << VSP_RS_FRESH)
				| (1u << VSP_RS_ACK);
			if((V.obs.respawn_stages & need) != need || ctx->host_death_screen)
				return;
			struct vsp_c h;
			if(!vs_c(ctx, 0, &h) || !vs_client_detached(&h)
			   || !ctx->host->local_player
			   || ctx->host->local_player->health <= 0 || !vs_paced())
				return;
			struct vsp_record r;
			vs_rec(&r, VSP_EV_RESPAWN, VSP_PRODUCTION, V.lethal_op);
			r.u.respawn.stage = VSP_RS_CLIENT_READY;
			/* Q4: the client's own view of the transaction -- the host
			 * respawn epoch it completed -- and its own input epochs (the
			 * one it held at death, and now). */
			r.u.respawn.respawn_epoch
				= ctx->host->host_respawn_completed_epoch
					? ctx->host->host_respawn_completed_epoch
					: ctx->host->host_respawn_epoch;
			r.u.respawn.old_input_epoch = V.death_epoch;
			r.u.respawn.new_input_epoch = ctx->epoch;
			r.u.respawn.u.client.host = h;
			r.u.respawn.u.client.ready = h.ready;
			(void)vs_push(&r);
			vs_step(6);
			return;
		}
		case 6: /* normal remount */
			if(!V.flag_a) {
				vs_hop_start(VSP_ACT_MOUNT, K, vs_mount_button(K),
							 VS_EXPECT_MOUNTED);
				V.flag_a = true;
			}
			if(vs_hop_step(ctx, out) == 1)
				vs_step(7);
			return;
		case 7:
			if(!vs_paced())
				return;
			vsp_iso_bracket(V.iso_op, false);
			vs_step(8);
			return;
		case 8:
			if(V.obs.iso_server_after != V.iso_op || !vs_paced())
				return;
			vs_push_client_iso(VSP_STAGE_AFTER, V.iso_op, K, ctx);
			vs_step(9);
			return;
		case 9:
			(void)vs_end_phase(VSP_P4);
			return;
		default:
			vs_fail("p3_step");
			return;
	}
}

/* The guest's own pause Leave (production local_pause + leave request). */
static bool vs_guest_leave_menu(const struct vehicle_smoke_ctx* ctx,
								struct vehicle_smoke_actions* out,
								unsigned* sub) {
	struct local_pause* pause = &ctx->guest->pause;
	switch(*sub) {
		case 0: out->guest_frame.pressed[IB_HOME] = true; (*sub)++; return false;
		case 1: if(pause->open) (*sub)++; return false;
		case 2:
			out->guest_frame.pressed[IB_GUI_DOWN] = true;
			out->guest_frame.pressed[IB_GUI_RIGHT] = true; (*sub)++; return false;
		case 3:
			if(pause->selected == 3) (*sub)++;
			else {
				out->guest_frame.pressed[IB_GUI_DOWN] = true;
				out->guest_frame.pressed[IB_GUI_RIGHT] = true;
			}
			return false;
		case 4:
			out->guest_frame.pressed[IB_GUI_CLICK] = true;
			out->suppress_autojoin = true;
			(*sub)++;
			return false;
		default:
			return pause->exit_pending
				|| ctx->guest->local_phase == CLIENT_GUEST_LEAVING
				|| ctx->guest->local_phase == CLIENT_GUEST_IDLE;
	}
}

static void vs_phase_p4(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	uint8_t K = (uint8_t)V.cfg.kind;
	out->guest_frame_valid = true;
	vs_frame(&out->guest_frame);
	/* Sticky from the Leave click until the refusal is armed (step 2). */
	out->suppress_autojoin = (V.step == 0 && V.flag_a && s_menu_sub >= 5)
		|| V.step == 1;
	/* O11 / S03 #2: the guest client's OWN readmission requests (JOIN,
	 * ACCEPT, CONFIRM, GAMEPLAY) are recorded by client_session_flush_local
	 * at each ACCEPTED send, in production order and before the server can
	 * consume them (vsp_dispatch_sent). Here the driver only reads that the
	 * receipt exists; it never infers a dispatch from a later state. */
	if(!V.accept_pushed && vsp_dispatch_done(VSP_UI_ACCEPT))
		V.accept_pushed = true;
	if(!V.confirm_pushed && vsp_dispatch_done(VSP_UI_CONFIRM))
		V.confirm_pushed = true;
	if(!V.gameplay_pushed && vsp_dispatch_done(VSP_UI_GAMEPLAY))
		V.gameplay_pushed = true;
	if(!V.p4_bracket) {
		if(!vs_paced())
			return;
		V.iso_op = vs_new_op();
		vsp_set_op(V.iso_op);
		vsp_client_counters_window_reset(true, false, 0);
		vsp_iso_target(0);
		vsp_iso_bracket(V.iso_op, true);
		vs_push_client_iso(VSP_STAGE_BEFORE, V.iso_op, K, ctx);
		V.p4_bracket = true;
		return;
	}
	if(V.obs.iso_server_before != V.iso_op)
		return;
	switch(V.step) {
		case 0: /* the declared guest Leave */
			if(!V.flag_a) {
				if(!vs_paced())
					return;
				s_menu_sub = 0;
				V.leave_gen = ctx->guest->local_generation;
				/* The readmission is ONE operation from the declared Leave
				 * through gameplay readiness: allocate it before the Leave so
				 * every row of it carries a real, first-appearing op. */
				V.refusal_op = vs_new_op();
				V.flag_a = true;
			}
			if(!vs_guest_leave_menu(ctx, out, &s_menu_sub))
				return;
			vs_push_ui(VSP_UI_GUEST_LEAVE, 1, VSP_SCR_PAUSE, VSP_BTN_GUI_CLICK,
					   0, true, V.refusal_op, ctx);
			vs_step(1);
			return;
		case 1:
			if(ctx->guest->local_phase != CLIENT_GUEST_IDLE
			   || ctx->guest->local_player)
				return;
			/* the seeded refusal waits for the NEXT real ACCEPT */
			vsp_set_op(V.refusal_op);
			vsp_cmd_refusal(V.refusal_op);
			/* O11: the readmission's four guest requests, in production
			 * order, recorded at their accepted sends. */
			vsp_await_dispatch(VSP_UI_GUEST_JOIN, 1, VSP_SCR_INGAME,
							   VSP_BTN_NONE, 0, V.refusal_op);
			vsp_await_dispatch(VSP_UI_ACCEPT, 1, VSP_SCR_INGAME, VSP_BTN_NONE,
							   0, V.refusal_op);
			vsp_await_dispatch(VSP_UI_CONFIRM, 1, VSP_SCR_INGAME, VSP_BTN_NONE,
							   0, V.refusal_op);
			vsp_await_dispatch(VSP_UI_GAMEPLAY, 1, VSP_SCR_INGAME, VSP_BTN_NONE,
							   0, V.refusal_op);
			vs_step(2);
			return;
		case 2: /* rejoin: the production Join -> OFFER -> ACCEPT */
			out->suppress_autojoin = false;
			/* JOIN and ACCEPT receipts: recorded at their accepted sends
			 * (O11; read above), BEFORE the server arms its tuple. */
			if(!V.accept_pushed
			   || !(V.obs.pending_stages
					& (1u << (VSP_PD_ARMED * 8u + VSP_CAUSE_SYNTHETIC_ADMISSION))))
				return;
			vs_step(3);
			return;
		case 3: /* the measured GLOBAL pause, held >= 2000 ms */
			out->suppress_autojoin = false;
			if(!V.flag_a) {
				if(!vs_paced())
					return;
				struct server_rpc call = {.type = SRPC_TOGGLE_PAUSE};
				if(!V.hooks.rpc_host || !V.hooks.rpc_host(V.hooks.user, &call))
					return;
				V.flag_a = true;
			}
			if(!V.obs.paused_seen)
				return;
			if(!V.flag_b) {
				if(V.now_ms - V.step_start_ms < VEHICLE_SMOKE_PAUSE_MS + 200u)
					return;
				struct server_rpc call = {.type = SRPC_TOGGLE_PAUSE};
				if(!V.hooks.rpc_host || !V.hooks.rpc_host(V.hooks.user, &call))
					return;
				V.flag_b = true;
			}
			if(!V.obs.resumed_seen)
				return;
			if(V.obs.resume_ms - V.obs.pause_ms < VEHICLE_SMOKE_PAUSE_MS) {
				vs_fail("pause_too_short");
				return;
			}
			vs_step(4);
			return;
		case 4: /* the transfer bracket is already open (phase start, before
				 * the Leave); host is mounted (p3 remount) */
			if(!vs_paced())
				return;
			vs_step(5);
			return;
		case 5:
			if(V.obs.iso_server_before != V.iso_op)
				return;
			if(!V.cfg.transfer_fixture_request) {
				/* Contact mode needs a PC-proven per-kind portal geometry
				 * (Astra ruling 7 / CONTRACT-v1 Q3) that does not exist yet;
				 * it is refused BY NAME rather than silently substituted. */
				vs_fail("transfer_contact_mode_unproven");
				return;
			}
			/* The transfer operation IS the bracketed operation (the p4
			 * isolation is keyed on transfer_begin's op). */
			V.transfer_op = V.iso_op;
			vsp_set_op(V.transfer_op);
			vsp_cmd_transfer(V.transfer_op);
			vs_step(6);
			return;
		case 6: /* clear-while-open, natural retention, commit, release */
			if(vsp_fixture_failure()) {
				vs_fail(vsp_fixture_failure());
				return;
			}
			if(!V.obs.transfer_begin || !V.obs.clear_seen
			   || !V.obs.transfer_release || !V.obs.transfer_commit)
				return;
			vs_step(7);
			return;
		case 7: /* barrier release, install, readiness, ACKs */
			if(!V.obs.barrier_seen || !V.obs.installed || !V.obs.transfer_ack
			   || !V.obs.ready_server)
				return;
			if(!vs_guest_ready(ctx) || ctx->world_switch_pending
			   || !client_session_host_gameplay_active(ctx->host) || !vs_paced())
				return;
			{
				/* Both client observations need a BOUND view: a readmitted
				 * guest is "ready" a frame before its view binds, and an
				 * unbound snapshot would serialize as generation 0 (fail
				 * closed). Wait for the actual binding. */
				struct vsp_c hb, gb;
				if(!vs_c(ctx, 0, &hb) || !vs_c(ctx, 1, &gb))
					return;
			}
			{
				struct vsp_record r;
				vs_rec(&r, VSP_EV_TRANSFER_ACK, VSP_PRODUCTION, V.transfer_op);
				struct vsp_p_transfer_ack* p = &r.u.tack;
				p->stage = VSP_STAGE_CLIENT;
				p->transfer_id = V.obs.transfer_id;
				(void)vs_c(ctx, 0, &p->u.host);
				p->actor = p->u.host.actor;
				p->dst = p->u.host.bound;
				p->input_ack_epoch = ctx->epoch;
				p->ready = p->u.host.ready;
				(void)vs_push(&r);
			}
			{
				struct vsp_record r;
				vs_rec(&r, VSP_EV_GAMEPLAY_READY, VSP_PRODUCTION, V.refusal_op);
				struct vsp_p_gameplay_ready* p = &r.u.ready;
				p->stage = VSP_STAGE_CLIENT;
				(void)vs_c(ctx, 1, &p->u.client_guest);
				/* The guest client's OWN admission tuple: the ticket and
				 * generation it was admitted under, in the world its view is
				 * bound to (never copied from the server record). */
				p->tuple.ticket = ctx->guest->local_ticket;
				p->tuple.generation = ctx->guest->local_generation;
				p->tuple.world = p->u.client_guest.bound;
				p->confirmed = ctx->guest->local_phase == CLIENT_GUEST_ATTACHED;
				p->ready_count = 1;
				p->installed_count = V.obs.installed;
				(void)vs_push(&r);
			}
			vs_step(8);
			return;
		case 8:
			if(!vs_paced())
				return;
			vsp_iso_bracket(V.iso_op, false);
			vs_step(9);
			return;
		case 9:
			if(V.obs.iso_server_after != V.iso_op || !vs_paced())
				return;
			vs_push_client_iso(VSP_STAGE_AFTER, V.iso_op, K, ctx);
			vs_step(10);
			return;
		case 10:
			(void)vs_end_phase(VSP_P5);
			return;
		default:
			vs_fail("p4_step");
			return;
	}
}

static void vs_phase_p5(const struct vehicle_smoke_ctx* ctx,
						struct vehicle_smoke_actions* out) {
	uint8_t K = (uint8_t)V.cfg.kind;
	out->guest_frame_valid = true;
	vs_frame(&out->guest_frame);
	switch(V.step) {
		case 0: /* the post-boundary detached census (both owners) */
			if(!vs_paced())
				return;
			V.iso_op = vs_new_op();
			vsp_set_op(V.iso_op);
			vsp_client_counters_window_reset(true, false, 0);
			vsp_iso_target(0);
			vsp_iso_bracket(V.iso_op, true);
			vs_push_client_iso(VSP_STAGE_BEFORE, V.iso_op, K, ctx);
			vs_step(1);
			return;
		case 1:
			if(V.obs.iso_server_before != V.iso_op || !vs_paced())
				return;
			vsp_iso_bracket(V.iso_op, false);
			vs_step(2);
			return;
		case 2:
			if(V.obs.iso_server_after != V.iso_op || !vs_paced())
				return;
			vs_push_client_iso(VSP_STAGE_AFTER, V.iso_op, K, ctx);
			vs_step(5);
			return;
		case 5: { /* FINAL detached census on the released SOURCE vehicle:
				   * the host stays detached in the destination while the
				   * run kind's vehicle keeps being ticked by its own world. */
			if(!vs_paced())
				return;
			V.final_op = vs_new_op();
			vsp_set_op(V.final_op);
			V.final_window = ++V.window;
			vsp_client_counters_window_reset(false, true, V.targets[K].id);
			vsp_ride_window_detached(V.final_window, V.targets[K].id,
									 (int)V.targets[K].world.slot, true);
			vs_step(6);
			return;
		}
		case 6: {
			struct vsp_client_counters cc;
			vsp_client_counters(&cc);
			if(cc.ride_eligible < VEHICLE_SMOKE_FINAL_PASSES)
				return;
			vsp_ride_window_detached(V.final_window, V.targets[K].id,
									 (int)V.targets[K].world.slot, false);
			vs_step(7);
			return;
		}
		case 7: {
			if(V.obs.ride_window != V.final_window || !vs_paced())
				return;
			struct vsp_client_counters cc;
			vsp_client_counters(&cc);
			struct vsp_record r;
			vs_rec(&r, VSP_EV_RIDE_SAMPLE, VSP_PRODUCTION, V.final_op);
			struct vsp_p_ride_sample* rp = &r.u.ride;
			rp->stage = VSP_STAGE_CLIENT;
			(void)vs_c(ctx, 0, &rp->u.client.host);
			/* the source vehicle as the client store that HOLDS it sees it
			 * (the host's view is the destination after the transfer) */
			/* ids are per world: read the vehicle from the client store of
			 * the SOURCE world's own slot (no view need present it) */
			(void)vsp_snap_client_vehicle_slot(V.targets[K].world.slot,
											   V.targets[K].id, K,
											   &rp->u.client.vehicle);
			rp->target = rp->u.client.vehicle.target;
			rp->window_id = V.final_window;
			rp->samples = cc.ride_eligible;
			rp->eligible_updates = cc.ride_eligible;
			rp->tick_calls = cc.ride_tick_calls;
			rp->missing_calls = cc.ride_missing;
			rp->duplicate_calls = cc.ride_duplicate;
			rp->follow_count = cc.host_follow_count;
			rp->displacement_m = cc.ride_disp_max;
			rp->seat_error_max_m = 0;
			rp->request_seq = 0; /* detached: no ride request */
			(void)vs_push(&r);
			vs_step(3);
			return;
		}
		case 3: /* all hooks off, everything admitted, then run_ok */
			vsp_hooks_reset();
			if(V.hop.active || V.probe.active || V.ride.active || !vs_drained())
				return;
			if(!V.closing) {
				V.closing = true;
				V.close_mark = vsp_server_update();
				return;
			}
			if(vsp_server_update() < V.close_mark + 2u || !vs_drained())
				return;
			if(vsp_hook_mask() != 0 || vsp_overflow_count() != 0) {
				vs_fail("hooks_or_overflow_at_end");
				return;
			}
			vs_push_phase(VSP_EV_PHASE_END, VSP_P5);
			V.phase = VSP_PH_DONE;
			vsp_set_phase(VSP_PH_DONE);
			{
				struct vsp_record r;
				vs_rec(&r, VSP_EV_RUN_OK, VSP_PRODUCTION, 0);
				(void)vs_push(&r);
			}
			V.run_ok_pushed = true;
			/* No producer may add a record after run_ok. */
			vsp_disarm();
			V.finish_start_ms = V.now_ms;
			vs_step(4);
			return;
		default:
			return;
	}
}

/* ---- the per-frame entry point ------------------------------------------- */

static void vs_finish(void) {
	/* Drain the tail (bounded) and send exactly one profile_done. */
	(void)vehicle_smoke_emit(VS_LINES_PER_FRAME);
	if(!vs_drained() && V.now_ms - V.finish_start_ms < VS_DRAIN_MS)
		return;
	if(V.done_sent)
		return;
	V.done_sent = true;
	V.finished = true;
	char status[96];
	if(V.failed)
		snprintf(status, sizeof(status), "fail:%s", V.fail_code);
	else
		snprintf(status, sizeof(status), "%s", vs_drained() ? "ok" : "fail:drain_timeout");
	if(V.hooks.profile_done)
		V.hooks.profile_done(V.hooks.user, status);
}

void vehicle_smoke_frame(const struct vehicle_smoke_ctx* ctx,
						 struct vehicle_smoke_actions* out) {
	if(!out)
		return;
	memset(out, 0, sizeof(*out));
	if(!V.armed || !ctx || !ctx->host || !ctx->guest)
		return;
	if(!V.clock_set) {
		V.clock_set = true;
		V.base_ms = ctx->now_ms;
	}
	V.now_ms = ctx->now_ms - V.base_ms;
	if(V.finished)
		return;
	if(V.failed || V.run_ok_pushed) {
		vs_finish();
		return;
	}
	if(!V.started) {
		V.started = true;
		struct vsp_record r;
		vs_rec(&r, VSP_EV_RUN_START, VSP_FIXTURE, 0);
		r.u.run_start.transfer_mode = V.cfg.transfer_fixture_request ? 1 : 0;
		(void)vs_push(&r);
		V.phase = VSP_P0;
		vsp_set_phase(VSP_P0);
		V.phase_start_ms = V.now_ms;
		vs_push_phase(VSP_EV_PHASE_BEGIN, VSP_P0);
	}
	(void)vehicle_smoke_emit(VS_LINES_PER_FRAME);
	if(V.failed) {
		vs_finish();
		return;
	}
	if(vsp_overflow_count()) {
		vs_fail("probe_overflow");
		vs_finish();
		return;
	}
	if(V.now_ms >= VEHICLE_SMOKE_TOTAL_MS) {
		vs_fail("run_deadline");
		vs_finish();
		return;
	}
	if(V.now_ms - V.phase_start_ms >= vs_phase_budget(V.phase)) {
		char code[48];
		snprintf(code, sizeof(code), "phase_timeout_step%u%s", V.step,
				 V.line_pending ? "_emit_blocked" : "");
		vs_fail(code);
		vs_finish();
		return;
	}
	/* O11: a fixture failure raised on the server thread (e.g. a collectible
	 * left within the guest's reach at an isolation window) ends the run by
	 * name in whatever phase it occurs. */
	if(vsp_fixture_failure()) {
		vs_fail(vsp_fixture_failure());
		vs_finish();
		return;
	}
	vs_ciso_track(ctx);
	bool edge = V.last_edge;
	switch(V.phase) {
		case VSP_P0: vs_phase_p0(ctx, out); break;
		case VSP_P1: vs_phase_p1(ctx, out); break;
		case VSP_P2: vs_phase_p2(ctx, out); break;
		case VSP_P3: vs_phase_p3(ctx, out); break;
		case VSP_P4: vs_phase_p4(ctx, out); break;
		case VSP_P5: vs_phase_p5(ctx, out); break;
		default: break;
	}
	/* The controllerless scripted P2 meets the PRODUCTION controller-
	 * recovery modal (no hardware device behind slot 1, then the scripted
	 * frame "reconnects"): it is cleared the way a player clears it -- one
	 * GUI_CLICK edge on the offered Resume, through P2's own owned frame,
	 * then a neutral frame. Never while the modal is not READY. Disclosed as
	 * a controllerless-P2 limitation (O02-result.md). */
	if(out->guest_frame_valid && ctx->guest
	   && ctx->guest->menu_host.recovery.phase == CONTROLLER_RECOVERY_READY
	   && !V.p2_resume_edge) {
		memset(out->guest_frame.pressed, 0, sizeof(out->guest_frame.pressed));
		memset(out->guest_frame.down, 0, sizeof(out->guest_frame.down));
		out->guest_frame.pressed[IB_GUI_CLICK] = true;
		out->guest_frame.down[IB_GUI_CLICK] = true;
		V.p2_resume_edge = true;
		V.p2_resumes++;
	} else if(V.p2_resume_edge) {
		V.p2_resume_edge = false;
	}
	/* The same modal for P1: the PC/autopilot host has no connected device
	 * frame, and the split death view hands the frame to P1's recovery
	 * consumer first (main.c local_death_arbitrate). While P1 is dead or in
	 * recovery, P1's own owned frame is a connected neutral device, and an
	 * offered Resume gets one GUI_CLICK edge. The death screen's own
	 * Respawn hold still arrives through the scripted host buttons. */
	if(ctx->host && !out->host_frame_valid
	   && (ctx->host->menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE
		   || (ctx->host->local_player && ctx->host->local_player->health <= 0))) {
		out->host_frame_valid = true;
		vs_frame(&out->host_frame);
		if(ctx->host->menu_host.recovery.phase == CONTROLLER_RECOVERY_READY
		   && !V.p1_resume_edge) {
			out->host_frame.pressed[IB_GUI_CLICK] = true;
			out->host_frame.down[IB_GUI_CLICK] = true;
			V.p1_resume_edge = true;
			V.p1_resumes++;
		} else
			V.p1_resume_edge = false;
	}
	/* One neutral frame between edges: an edge delivered last frame clears
	 * the flag only after a frame without one. */
	if(edge && V.last_edge && !(out->host_buttons[IB_ACTION1]
								|| out->host_buttons[IB_ACTION2]
								|| out->host_buttons[IB_JUMP]))
		V.last_edge = false;
	if(V.failed) {
		memset(out, 0, sizeof(*out));
		vs_finish();
	}
}
