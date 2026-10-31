/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* host_vehicle_smoke_8m -- the scripted host-vehicle phase driver.
 *
 * CONTRACT: tmp/jobs/vehicle-smoke-1004/CONTRACT-v1.md (FROZEN 2026-10-04),
 * ASTRA-PLAN.md rulings 1-14, ASTRA-CONTRACT-RULING.md. The contract, not this
 * header, is the authority on the phases, events, fields and predicates.
 *
 * WHAT THIS IS. One state-driven, per-frame driver (p0..p5) for ONE vehicle
 * kind per process (--cw-vehicle-kind boat|pig|cart). It drives the REAL host
 * input path (autopilot_script_look + autopilot_script_buttons: button LEVELS
 * read by input_held/input_pressed, consumed by screen_ingame and the death
 * screen exactly as a controller's would be), the real owned GUI frames
 * (local_pause_update for Save and quit, the guest's own pause Leave), real
 * guest frames (screen_ingame_update_owned / entity_local_player_walk_owned)
 * and a small set of DISCLOSED server fixture commands (vehicle_smoke_probe.h:
 * fixture build, one lethal call, one predeclared transfer request, the
 * seeded ACCEPT refusal). It never writes a relation, a death flag, a
 * transaction state, an ACK or a pose.
 *
 * WHAT IT EMITS. Every vehicle_smoke.* line: the script is the main-thread
 * serializer (vehicle_smoke_emit): it pops owner-thread POD records from the
 * probe FIFOs, assigns the contiguous seq, serializes and hands the immutable
 * line to hw_telemetry_emit_observer through the emit hook, retrying the SAME
 * bytes on refusal. It also OBSERVES the records it serializes -- that is how
 * a phase knows the authority applied an action, the checkpoint went durable,
 * the transfer committed -- so a phase can only advance on admitted evidence.
 *
 * INERTNESS. Nothing here runs unless vehicle_smoke_arm() was handed an ARMED
 * hw_run config whose profile is host_vehicle_smoke_8m with a valid kind.
 * Unarmed -- every other profile and normal play -- vehicle_smoke_frame()
 * zeroes its actions and returns, the probe stays disarmed and
 * vehicle_smoke_rig_create_allowed() refuses, so the world-create path in
 * screen_select_world.c cannot be reached. */

#ifndef GAME_VEHICLE_SMOKE_SCRIPT_H
#define GAME_VEHICLE_SMOKE_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../network/vehicle_smoke_probe.h"
#include "../platform/input_owner.h" /* struct input_player_frame */

struct client_session;
struct server_rpc;
struct hw_run_config;

#define VEHICLE_SMOKE_RIG_WORLD VSP_RIG_WORLD
#define VEHICLE_SMOKE_RIG_DISPLAY "Vehicle Rig"

/* CONTRACT-v1 phase ceilings and the global deadline (owner monotonic ms). */
#define VEHICLE_SMOKE_P0_MS 100000u
#define VEHICLE_SMOKE_P1_MS 60000u
#define VEHICLE_SMOKE_P2_MS 100000u
#define VEHICLE_SMOKE_P3_MS 60000u
/* O15: the host hop's aim -- the point of a target box (x1,y1,z1,x2,y2,z2)
 * nearest the eye, inset 5 cm into the box. Pure; exported for its test. */
void vehicle_smoke_hull_aim(const float box[6], const float eye[3], float out[3]);
/* O25: the boat window's facing -- toward the FARTHER wall of the 2-wide
 * basin (x ax-2..ax), fixed at window start. +pi/2 = +X, -pi/2 = -X. */
float vehicle_smoke_boat_yaw(float boat_x, int32_t anchor_x);
#define VEHICLE_SMOKE_P4_MS 130000u
#define VEHICLE_SMOKE_P5_MS 20000u
#define VEHICLE_SMOKE_TOTAL_MS 480000u

/* RIDE window floors (CONTRACT-v1 RIDE predicate). */
#define VEHICLE_SMOKE_RIDE_UPDATES 100u
#define VEHICLE_SMOKE_RIDE_DISPLACEMENT 0.25F
/* P2 negative probes observe >= 10 active updates after delivery. */
#define VEHICLE_SMOKE_PROBE_UPDATES 10u
/* PROGRESS: the global pause is held >= 2000 ms. */
#define VEHICLE_SMOKE_PAUSE_MS 2000u

struct vehicle_smoke_hooks {
	/* hw_telemetry_emit_observer(line). May refuse; the SAME bytes are
	 * retried next frame. */
	bool (*emit)(void* user, const char* line);
	/* hw_telemetry_emit_profile_done(status) through main's completion
	 * tail. Called exactly once per run, after run_ok/fail drained. */
	void (*profile_done)(void* user, const char* status);
	/* screen_sworld_create_vehicle_rig(VEHICLE_SMOKE_RIG_DISPLAY, dir, ...) */
	bool (*create_world)(void* user, const char* directory);
	/* ephemeral config "autoload" = directory, then screen_set(select_world) */
	bool (*arm_autoload)(void* user, const char* directory);
	/* NULL while the named-autoload latch is unconsumed, else its value. */
	const char* (*autoload_conflict)(void* user);
	/* svin_rpc_send_from({0,0}, call): the host's own mailbox. */
	bool (*rpc_host)(void* user, struct server_rpc* call);
	/* svin_rpc_send_from({1, guest generation}, call). */
	bool (*rpc_guest)(void* user, struct server_rpc* call);
	/* O22 (nullable): true once the telemetry stream can never accept
	 * another line this session (hw_telemetry_quiesced -- the collector is
	 * gone). The driver observes evidence only as it is emitted, so on a dead
	 * stream it would otherwise wait out an unrelated stage timeout (HW3:
	 * fail:saddle_timeout_st3) instead of naming the cause. */
	bool (*telemetry_lost)(void* user);
	void* user;
};

/* Everything the driver observes about the client, sampled by main. */
struct vehicle_smoke_ctx {
	struct client_session* host;  /* gstate.players[0] */
	struct client_session* guest; /* gstate.players[1] */
	bool world_loaded, world_switch_pending;
	bool at_title, host_death_screen, host_loading_screen;
	unsigned view_count;
	uint32_t epoch;  /* clin_input_epoch() */
	uint32_t now_ms; /* any monotonic ms; the driver rebases it */
	/* Main's count of WORLD_RESET(local_entity 0) consumed by clin_update. */
	uint32_t world_reset_count;
	/* LOS sampling: the block at a view's terrain (false = not resident). */
	bool (*block_at)(unsigned view, int x, int y, int z, uint8_t* type);
};

/* What main must apply this frame. Zeroed when unarmed. */
struct vehicle_smoke_actions {
	/* host: autopilot_script_look + autopilot_script_buttons */
	bool host_look;
	float host_yaw, host_pitch;
	bool host_buttons_armed;
	bool host_buttons[IB_COUNT];
	/* O17 (R07 F3): the host's POINTER is owned by the script this frame:
	 * reported unavailable (a Wiimote pointing off-screen), so the real
	 * death-screen consumer acts on its SELECTED button instead of whatever
	 * the live desktop cursor happens to hover. */
	bool host_pointer_owned;
	/* the host's OWNED GUI frame (pause menu) */
	bool host_frame_valid;
	struct input_player_frame host_frame;
	/* the guest's owned frame: walking, ACTION2, its own pause */
	bool guest_frame_valid;
	struct input_player_frame guest_frame;
	bool guest_look;
	float guest_yaw, guest_pitch;
	bool drive_guest_body; /* run the guest body's physics/actions */
	bool suppress_autojoin;
};

struct vehicle_smoke_config {
	enum vsp_kind kind;
	bool transfer_fixture_request;
	const char* build_id;
	size_t run_id_len; /* hw_telemetry appends ,"run_id":"..." */
	bool seed_set;
	int64_t seed;
};

/* Arm from an ARMED hw_run config (host_vehicle_smoke_8m + kind). Also arms
 * the probe. Refuses any other profile. */
void vehicle_smoke_arm(const struct hw_run_config* cfg,
					   const struct vehicle_smoke_hooks* hooks,
					   const char* build_id, size_t run_id_len);
/* Test entry: arm from an explicit config (no hw_run). */
void vehicle_smoke_arm_config(const struct vehicle_smoke_config* cfg,
							  const struct vehicle_smoke_hooks* hooks);
bool vehicle_smoke_armed(void);
void vehicle_smoke_frame(const struct vehicle_smoke_ctx* ctx,
						 struct vehicle_smoke_actions* out);
/* Serialize and emit queued records; returns lines emitted this call. Also
 * called by vehicle_smoke_frame; exposed for tests. */
unsigned vehicle_smoke_emit(unsigned max_lines);
/* Fail the run now with a contract fail code (ASCII [A-Za-z0-9_.:-]). */
void vehicle_smoke_fail(const char* code);

/* The gate screen_sworld_create_vehicle_rig() consults: true only while the
 * driver is armed AND `directory` is the fixed rig world. */
bool vehicle_smoke_rig_create_allowed(const char* directory);
/* screen_select_world.c: the authorization the armed driver grants (and its
 * reset revokes), and the create itself (refuses an existing directory and
 * any directory but VEHICLE_SMOKE_RIG_WORLD). */
void screen_sworld_vehicle_rig_authorize(bool on);
/* O19: the rig slot before a create. `rig_path` is the full saves path of
 * VEHICLE_SMOKE_RIG_WORLD. FREE: nothing there. ARCHIVED: an earlier run's
 * consumed rig DIRECTORY was moved aside (never deleted) to
 * "<rig_path>_consumed_NNN"; its path is copied to `archived`. REFUSED:
 * something that is not a directory holds the name, or no archive slot is
 * free, or the move failed -- the create must not proceed. */
enum vehicle_rig_slot {
	VEHICLE_RIG_SLOT_FREE,
	VEHICLE_RIG_SLOT_ARCHIVED,
	VEHICLE_RIG_SLOT_REFUSED,
};
#define VEHICLE_RIG_ARCHIVE_MAX 1000u
enum vehicle_rig_slot vehicle_smoke_rig_prepare_slot(const char* rig_path,
	char* archived, size_t archived_cap);
bool screen_sworld_create_vehicle_rig(const char* display_name,
									  const char* directory, bool seed_set,
									  int64_t seed);

/* Observation for tests and main. */
enum vsp_phase vehicle_smoke_phase_now(void);
unsigned vehicle_smoke_step_now(void);
uint32_t vehicle_smoke_seq_now(void);
const char* vehicle_smoke_fail_code(void);
bool vehicle_smoke_finished(void);
/* Full teardown (tests between cases). */
void vehicle_smoke_reset(void);

#endif
