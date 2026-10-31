/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* host_vehicle_smoke_8m -- the owner-thread observation probe.
 *
 * CONTRACT: tmp/jobs/vehicle-smoke-1004/CONTRACT-v1.md (FROZEN 2026-10-04).
 * That file, not this header, is the authority on event names, payload field
 * sets, types and ordering; tools/wii_telemetry_server.py grades against it.
 *
 * WHAT THIS IS. Every vehicle_smoke.* observer line is built from a POD record
 * captured on the thread that OWNS the state it describes:
 *   - S (server worker): server_local.c / server_local_guest.c /
 *     server_transfer.c sites push into a bounded 32-record SPSC FIFO;
 *   - M/C (main thread, which also runs the client): main.c, the script,
 *     server_interface.c's calling-thread ingress, client_interface.c's
 *     clin_process_observed witness and entity_local_player.c's follow branch
 *     push into a bounded 8-record FIFO.
 * No record holds a pointer into server/client/entity state. The main thread
 * is the SOLE serializer and seq allocator (vsp_format_next / vsp_commit_line,
 * driven by source/game/vehicle_smoke_script.c). A full FIFO is NEVER an
 * overwrite and NEVER a block: the push is refused, an overflow counter
 * latches, and the script turns that into a terminal FAIL.
 *
 * DORMANCY. Every producer site in production code is written
 *     if(vsp_armed()) vsp_xxx(...);
 * vsp_armed() is one relaxed atomic load of a process-global bool that only
 * vsp_arm() (reached only from the armed host_vehicle_smoke_8m profile) ever
 * sets. Disarmed: no record, no allocation, no RNG draw, no clock read, no
 * state write -- the dormant-equivalence cases in
 * tests/test_vehicle_smoke_script.c pin that byte-for-byte.
 *
 * MUTATING HOOKS. Four profile-scoped hooks can change simulation state, each
 * gated on its own armed bit in addition to vsp_armed() (the "hook mask"):
 *   VSP_HOOK_FIXTURE  one server-thread fixture build (demo_set + real spawns)
 *   VSP_HOOK_LETHAL   one lethal call into server_local_set_player_health
 *   VSP_HOOK_REFUSAL  the seeded ACCEPT-batch admission refusal (exact Q)
 *   VSP_HOOK_TRANSFER one server_transfer_request(host, Nether) (fixture_request)
 * Each is consumed once, reset on world teardown (vsp_srv_world_teardown) and
 * on completion/failure (vsp_hooks_reset). */

#ifndef NETWORK_VEHICLE_SMOKE_PROBE_H
#define NETWORK_VEHICLE_SMOKE_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct server_local;
struct server_player;
struct server_world;
struct entity;
struct client_rpc;
struct server_rpc;
struct client_session;

#define VSP_SCHEMA 1u
/* O09 (CONTRACT-v1.3 cart fixture): the smoke's cart track is a two-ended
 * stopped east-west track at az+2. The v1.2 OPEN-END north-south track (rails
 * az+1..az+3, stone az+4, open -Z end) is kept only as a named reproducer of
 * the Beta ejection-off-the-end case (G1/G4): build with
 * -DVSP_CART_OPEN_END=1. The smoke never uses it. */
#ifndef VSP_CART_OPEN_END
#define VSP_CART_OPEN_END 0
#endif
#define VSP_PROFILE "host_vehicle_smoke_8m"
#define VSP_RIG_WORLD "cw_vehicle_rig"
#define VSP_SRV_FIFO 32u
#define VSP_MAIN_FIFO 8u
/* CONTRACT-v1 wire format: final line incl. run_id and LF <= 2303 bytes. */
#define VSP_LINE_MAX 2303u
#define VSP_LINE_CAP 2304u

enum vsp_kind { VSP_BOAT = 0, VSP_PIG, VSP_CART, VSP_KIND_COUNT };

enum vsp_phase {
	VSP_P0 = 0,
	VSP_P1,
	VSP_P2,
	VSP_P3,
	VSP_P4,
	VSP_P5,
	VSP_PH_DONE,
	VSP_PH_FAILED,
};

enum vsp_origin { VSP_PRODUCTION = 0, VSP_FIXTURE, VSP_SYNTHETIC };

enum vsp_event {
	VSP_EV_RUN_START = 0,
	VSP_EV_PHASE_BEGIN,
	VSP_EV_PHASE_END,
	VSP_EV_RIG_READY,
	VSP_EV_ACTION_ATTEMPT,
	VSP_EV_UI_ACTION,
	VSP_EV_ACTION_RESULT,
	VSP_EV_RIDE_SAMPLE,
	VSP_EV_DISMOUNT,
	VSP_EV_SWITCH,
	VSP_EV_ISOLATION,
	VSP_EV_CHECKPOINT,
	VSP_EV_WORLD_RESET,
	VSP_EV_TITLE,
	VSP_EV_RELOAD,
	VSP_EV_DEATH,
	VSP_EV_RESPAWN,
	VSP_EV_TRANSFER_BEGIN,
	VSP_EV_TRANSFER_RELEASE,
	VSP_EV_TRANSFER_COMMIT,
	VSP_EV_TRANSFER_ACK,
	VSP_EV_PENDING,
	VSP_EV_PAUSE,
	VSP_EV_RESUME,
	VSP_EV_ADMISSION_CLEAR,
	VSP_EV_BARRIER_RELEASE,
	VSP_EV_INSTALLED,
	VSP_EV_GAMEPLAY_READY,
	VSP_EV_RUN_OK,
	VSP_EV_FAIL,
	VSP_EV_COUNT,
};

/* action / channel / button / screen / site / status vocabularies (closed). */
enum vsp_action {
	VSP_ACT_SADDLE = 0,
	VSP_ACT_MOUNT,
	VSP_ACT_DISMOUNT,
	VSP_ACT_SWITCH,
	VSP_ACT_GUEST_UI,
	VSP_ACT_GUEST_USE,
	VSP_ACT_GUEST_CART_INTENT,
	VSP_ACT_GUEST_CART_CONTROL,
};
enum vsp_channel { VSP_CH_HOST_BUTTONS = 0, VSP_CH_GUEST_FRAME, VSP_CH_OWNED_RPC };
enum vsp_button {
	VSP_BTN_NONE = 0,
	VSP_BTN_ACTION1,
	VSP_BTN_ACTION2,
	VSP_BTN_JUMP,
	VSP_BTN_HOME,
	VSP_BTN_GUI_UP,
	VSP_BTN_GUI_DOWN,
	VSP_BTN_GUI_CLICK,
};
enum vsp_screen {
	VSP_SCR_NONE = 0,
	VSP_SCR_INGAME,
	VSP_SCR_PAUSE,
	VSP_SCR_DEATH,
	VSP_SCR_TITLE,
	VSP_SCR_LOADING,
};
enum vsp_ui_act {
	VSP_UI_SAVE_EXIT = 0,
	VSP_UI_RESPAWN,
	VSP_UI_GUEST_LEAVE,
	VSP_UI_GUEST_JOIN,
	VSP_UI_ACCEPT,
	VSP_UI_CONFIRM,
	VSP_UI_GAMEPLAY,
};
enum vsp_site { VSP_SITE_INGRESS = 0, VSP_SITE_AUTHORITY, VSP_SITE_CLIENT };
enum vsp_status {
	VSP_ST_SENT = 0,
	VSP_ST_APPLIED,
	VSP_ST_REFUSED,
	VSP_ST_CLIENT_NOOP,
};
enum vsp_rpc {
	VSP_RPC_ENTITY_ATTACK = 0,
	VSP_RPC_ENTITY_USE,
	VSP_RPC_MINECART_RIDE_INTENT,
	VSP_RPC_MINECART_CONTROL,
};
enum vsp_cause {
	VSP_CAUSE_SYNTHETIC_ADMISSION = 0,
	VSP_CAUSE_GLOBAL_PAUSE,
	VSP_CAUSE_TRANSFER,
	VSP_CAUSE_LOADING,
	VSP_CAUSE_STATUS_BACKPRESSURE,
};

/* Stage vocabularies (one per staged event; index into a name table). */
enum vsp_stage_sc { VSP_STAGE_SERVER = 0, VSP_STAGE_CLIENT };
enum vsp_stage_ba { VSP_STAGE_BEFORE = 0, VSP_STAGE_AFTER };
enum vsp_stage_ck { VSP_CK_BEGIN = 0, VSP_CK_DURABLE };
enum vsp_stage_reload {
	VSP_RL_DISK = 0,
	VSP_RL_INVENTORY,
	VSP_RL_ENTITIES,
	VSP_RL_CLIENT_READY,
};
enum vsp_stage_death { VSP_DEATH_SERVER = 0, VSP_DEATH_UI };
enum vsp_stage_respawn {
	VSP_RS_REQUESTED = 0,
	VSP_RS_RELEASED,
	VSP_RS_FRESH,
	VSP_RS_ACK,
	VSP_RS_CLIENT_READY,
};
enum vsp_stage_pending {
	VSP_PD_ARMED = 0,
	VSP_PD_BLOCKED,
	VSP_PD_RETAINED,
};

/* ---- the frozen reusable objects (CONTRACT-v1 "Wire format") ----------- */
struct vsp_a { /* A */
	uint64_t generation;
	uint32_t wire_id, input_epoch;
	uint8_t slot;
};
struct vsp_w { /* W; generation 0 = not live (never serialized: FAIL) */
	uint32_t generation;
	int32_t dimension;
	uint8_t slot;
};
struct vsp_r { /* R */
	uint32_t id;
	struct vsp_w world;
	uint8_t kind; /* enum vsp_kind */
};
struct vsp_s { /* S: authoritative actor */
	struct vsp_a actor;
	struct vsp_w world;
	float pos[3];
	float steer[2];
	int32_t health;
	uint32_t mount_count, inv_hash, inv_used;
	bool ready, confirmed, riding_boat, riding_cart;
};
struct vsp_v { /* V: server vehicle */
	struct vsp_r target;
	struct vsp_a rider;
	float pos[3];
	float steer[2];
	uint32_t ride_epoch, control_seq, positive_steps, negative_steps;
	int32_t pending_throttle;
	bool alive, occupied, saddled;
};
struct vsp_c { /* C (present) or C0 (present == false) */
	struct vsp_a actor;
	struct vsp_w bound;
	float pos[3];
	float steer[2];
	uint32_t mount_count, ride_target, ride_input_epoch, ride_request_seq;
	uint32_t inv_hash, inv_used;
	uint64_t c0_generation, c0_ticket;
	bool present, ready, ride_pending, ride_desired_occupied, ride_send_dirty;
	bool ride_exit_lift_pending, ride_exit_repin_eligible;
};
struct vsp_vc { /* VC: client vehicle; kind-specific field set at serialize */
	struct vsp_r target;
	float pos[3];
	float steer[2];
	uint32_t occupant_id, pig_rider_id, cart_type, ride_epoch, control_seq;
	uint32_t positive_steps, negative_steps;
	int32_t pending_throttle, replay_throttle;
	bool alive, occupied, pig_ridden, saddled, control_ready, control_dirty;
};
struct vsp_q { /* Q: the real pending ACCEPT tuple */
	uint64_t ticket, generation;
	struct vsp_w world;
};

/* ---- per-event payloads ------------------------------------------------- */
struct vsp_p_run_start { uint8_t transfer_mode; /* 0 contact, 1 fixture */ };
struct vsp_p_phase {
	uint32_t phase_index, pending_markers, open_ops;
};
struct vsp_p_rig_ready {
	uint32_t fixture_hash, pig_ai_updates;
	struct vsp_r targets[3];
	struct vsp_s host, guest;
	bool water_ok, support_ok, rails_ok, ray_ok, saddle_consumed;
};
struct vsp_p_action_attempt {
	struct vsp_a source;
	struct vsp_r target;
	uint32_t edge_seq, held_ms, request_seq, camera_hit;
	float reach_m;
	uint8_t action, channel, button;
	bool desired_occupied, los;
};
struct vsp_p_ui_action {
	uint32_t player, held_ms, edge_seq, input_epoch;
	uint8_t action, screen, button;
	bool delivered;
};
struct vsp_p_action_result {
	struct vsp_a source;
	struct vsp_r target;
	uint64_t serial;
	uint32_t request_seq, ride_epoch;
	uint8_t action, site, status, rpc;
	union {
		struct { struct vsp_s host; struct vsp_v vehicle; } authority;
		struct { struct vsp_c host; struct vsp_vc vehicle; } client;
	} u;
};
struct vsp_p_ride_sample {
	struct vsp_r target;
	uint32_t window_id, samples, eligible_updates, tick_calls, missing_calls;
	uint32_t duplicate_calls, follow_count, request_seq;
	float displacement_m, seat_error_max_m;
	uint8_t stage; /* vsp_stage_sc */
	union {
		struct { struct vsp_s host; struct vsp_v vehicle; } server;
		struct { struct vsp_c host; struct vsp_vc vehicle; } client;
	} u;
};
struct vsp_p_dismount {
	struct vsp_s host;
	struct vsp_v old;
	uint64_t serial;
};
struct vsp_p_switch {
	struct vsp_s host;
	struct vsp_v old, nu;
	uint64_t release_serial, mount_serial;
	uint8_t from, to;
};
struct vsp_p_isolation {
	uint32_t attempt_op, guest_follow_count, unexpected_route_count;
	uint32_t unexpected_attach_count;
	float guest_pose_delta_max_m;
	uint8_t stage; /* vsp_stage_ba */
	uint8_t owner; /* vsp_stage_sc: SERVER or CLIENT */
	union {
		struct { struct vsp_s host, guest; struct vsp_v vehicle; } server;
		struct { struct vsp_c host, guest; struct vsp_vc vehicle; } client;
	} u;
};
struct vsp_p_checkpoint {
	struct vsp_s host, guest;
	struct vsp_v vehicle;
	uint32_t checkpoint_id, fixture_hash, pose_bits[3];
	uint8_t stage;
	bool save_ok, level_ok, chunks_ok, regions_synced;
};
/* CONTRACT-v1.1 Q6: reset/title are RECEIPTS read on the main thread from
 * the live client state when observed (never constants). */
struct vsp_p_world_reset {
	struct vsp_w source_world;
	uint32_t world_reset_count, checkpoint_id;
	uint32_t local_entity; /* host body id still bound (0 = none) */
	bool world_loaded;
};
struct vsp_p_title {
	uint32_t world_reset_count, checkpoint_id;
	bool world_loaded, autoload_consumed;
};
struct vsp_p_reload {
	uint32_t checkpoint_id;
	uint8_t stage;
	union {
		struct {
			struct vsp_w world;
			uint32_t pose_bits[3];
			bool disk_reopen, player_read_ok, fallback_used, override_used;
		} disk;
		struct {
			struct vsp_w world;
			uint32_t inv_hash, inv_used;
			bool read_ok;
		} inventory;
		struct {
			struct vsp_s host, guest;
			struct vsp_r targets[3];
			uint32_t fixture_hash, ambiguous_matches;
			bool saddle;
		} entities;
		struct {
			struct vsp_c host, guest;
			uint32_t input_ack_epoch;
		} client_ready;
	} u;
};
struct vsp_p_death {
	uint8_t stage;
	union {
		struct {
			struct vsp_s before_host, host;
			struct vsp_v vehicle;
			uint32_t deaths_before, deaths_after;
			char cause[24];
		} server;
		struct { struct vsp_c host; } ui;
	} u;
};
struct vsp_p_respawn {
	uint32_t old_input_epoch, new_input_epoch;
	/* CONTRACT-v1.1 Q4: the split host-respawn transaction epoch
	 * (server_local.host_respawn_epoch / client host_respawn_*_epoch); 0 on
	 * RELEASED, which precedes the transaction's allocation. Separate from
	 * the input epochs, which the split route does not change. */
	uint64_t respawn_epoch;
	uint8_t stage;
	union {
		struct {
			struct vsp_s host;
			struct vsp_v old;
			uint64_t serial;
			int32_t health;
			bool ack;
		} server;
		struct { struct vsp_c host; bool ready; } client;
	} u;
};
struct vsp_p_transfer {
	/* begin/release/commit share one shape; unused members stay zero and
	 * are simply not serialized for that event. */
	uint32_t transfer_id, destination_mount_count, transition_epoch;
	struct vsp_a actor;
	struct vsp_w src, dst;
	struct vsp_s host;
	struct vsp_v vehicle;
	uint64_t serial;
	uint8_t mode; /* begin: 0 contact, 1 fixture_request */
};
struct vsp_p_transfer_ack {
	uint32_t transfer_id, input_ack_epoch, destination_mount_count;
	struct vsp_a actor;
	struct vsp_w dst;
	uint8_t stage;
	bool ready;
	union {
		struct vsp_v source_vehicle; /* server */
		struct vsp_c host;           /* client */
	} u;
};
struct vsp_p_progress {
	/* pending / pause / resume / admission_clear / barrier_release /
	 * installed share this shape; the serializer emits each event's exact
	 * field set from it. */
	struct vsp_q tuple;
	uint64_t serial;
	uint32_t transfer_id, installed_count;
	uint8_t stage, cause, result; /* result: 0 none, 1 installed */
	bool paused, loading, transfer_open, refusal_armed, pending;
	bool host_ack_pending;
};
struct vsp_p_gameplay_ready {
	struct vsp_q tuple;
	uint32_t ready_count, installed_count;
	uint8_t stage;
	bool confirmed;
	union {
		struct vsp_s server_guest;
		struct vsp_c client_guest;
	} u;
};
struct vsp_p_fail {
	char code[49];
	uint32_t overflow_count;
	uint8_t failed_phase;
};

struct vsp_record {
	uint8_t event;  /* enum vsp_event */
	uint8_t origin; /* enum vsp_origin */
	uint8_t phase;  /* enum vsp_phase at capture */
	bool ok;
	char reason[49];
	uint32_t op_id, ms, server_update, active_update;
	/* O04: global push order (server and main FIFOs share one counter);
	 * vsp_pop merges the two FIFOs by it. Not serialized. */
	uint32_t push_order;
	union {
		struct vsp_p_run_start run_start;
		struct vsp_p_phase phase;
		struct vsp_p_rig_ready rig_ready;
		struct vsp_p_action_attempt attempt;
		struct vsp_p_ui_action ui;
		struct vsp_p_action_result result;
		struct vsp_p_ride_sample ride;
		struct vsp_p_dismount dismount;
		struct vsp_p_switch sw;
		struct vsp_p_isolation iso;
		struct vsp_p_checkpoint ck;
		struct vsp_p_world_reset reset;
		struct vsp_p_title title;
		struct vsp_p_reload reload;
		struct vsp_p_death death;
		struct vsp_p_respawn respawn;
		struct vsp_p_transfer transfer;
		struct vsp_p_transfer_ack tack;
		struct vsp_p_progress progress;
		struct vsp_p_gameplay_ready ready;
		struct vsp_p_fail fail;
	} u;
};

/* ---- arming / identity -------------------------------------------------- */
extern bool vsp_g_armed; /* written only by vsp_arm/vsp_disarm */
static inline bool vsp_armed(void) {
	return __atomic_load_n(&vsp_g_armed, __ATOMIC_RELAXED);
}

#define VSP_HOOK_FIXTURE 0x1u
#define VSP_HOOK_LETHAL 0x2u
#define VSP_HOOK_REFUSAL 0x4u
#define VSP_HOOK_TRANSFER 0x8u
#define VSP_HOOK_BUTTONS 0x10u /* autopilot's script-button override */

struct vsp_arm_cfg {
	enum vsp_kind kind;
	bool transfer_fixture_request; /* predeclared p4 mode */
	const char* build_id;
};
void vsp_arm(const struct vsp_arm_cfg* cfg);
/* Observation off. Records already queued stay until vsp_reset. */
void vsp_disarm(void);
/* Full teardown (tests between cases; production never resets a run). */
void vsp_reset(void);
enum vsp_kind vsp_kind_now(void);
const char* vsp_kind_name(enum vsp_kind kind);
uint32_t vsp_now_ms(void); /* ms since arm, any thread */

/* The run's current phase and operation, written by the script, read by the
 * producers to stamp their records. */
void vsp_set_phase(enum vsp_phase phase);
enum vsp_phase vsp_phase_now(void);
void vsp_set_op(uint32_t op_id);
/* The action the CURRENT operation performs (enum vsp_action), set by the
 * driver with the op. A server result for that op whose handler cannot tell
 * the delivering channel (guest ENTITY_USE from a frame press or an owned
 * RPC) reports this action; any other op is unaffected. */
void vsp_set_op_action(uint32_t op_id, uint8_t action);
uint32_t vsp_op_now(void);

uint32_t vsp_server_update(void);
uint32_t vsp_active_update(void);
uint32_t vsp_overflow_count(void);
uint32_t vsp_hook_mask(void);
/* Disarm every mutating hook (completion, failure, teardown). Captured
 * records and run counters survive. */
void vsp_hooks_reset(void);

/* ---- main-thread record queue / serializer ------------------------------ */
/* Push a main/client-owned record (stamps ms/phase/counters if the caller
 * left ms == 0). False = FIFO full: overflow latched. */
bool vsp_main_push(struct vsp_record* rec);

/* O11 / S03 #2: request receipts recorded at the REAL accepted-send seam.
 * The driver queues the receipts it expects (in production order); the send
 * site reserves a push order BEFORE handing the request to the server inbox
 * and, only if the send was accepted, pushes the queued receipt with that
 * order -- so the receipt precedes every server row its request can cause,
 * whenever the server thread runs. A refused send records nothing (its
 * reserved order is simply unused); the retry reserves afresh. Main thread
 * only. Receipts are never inferred from a later readiness state. */
void vsp_await_dispatch(uint8_t action, uint32_t player, uint8_t screen,
						uint8_t button, uint32_t edge_seq, uint32_t op);
uint32_t vsp_dispatch_reserve(void);            /* 0: nothing awaited */
/* O11 / S03 #3: a collectible item lies within the guest's reach (checked at
 * every isolation window open; server thread). */
struct server_local;
bool vsp_srv_collectible_near_guest(struct server_local* s);
void vsp_dispatch_sent(uint8_t action, uint32_t order);
bool vsp_dispatch_done(uint8_t action);
/* The guest's LOCAL_GUEST action -> its VSP_UI receipt (VSP_UI_NONE_SENT =
 * none; 0 is VSP_UI_SAVE_EXIT, a real receipt). */
#define VSP_UI_NONE_SENT 0xFFu
uint8_t vsp_ui_of_guest_action(int local_guest_action);
size_t vsp_pending_records(void); /* both FIFOs */
/* Pop the next queued record (server FIFO first). False when empty. */
bool vsp_pop(struct vsp_record* out);
/* Serialize one record with the given seq into a contract line WITHOUT the
 * run_id key (hw_telemetry appends it). Returns the length, or 0 with
 * *err set ("oversize", "nonfinite", "invalid_world", ...) -- the caller
 * fails the run on 0. reserve = 12 + strlen(run_id) + 1 (LF). */
size_t vsp_serialize(const struct vsp_record* rec, uint32_t seq,
					 size_t reserve, char* out, size_t cap,
					 const char** err);

/* ---- script -> server command mailbox ----------------------------------- */
enum vsp_cmd { VSP_CMD_FIXTURE = 0, VSP_CMD_LETHAL, VSP_CMD_TRANSFER, VSP_CMD_COUNT };
void vsp_cmd_fixture(uint32_t op_id);  /* build/rebuild the fixture once */
void vsp_cmd_lethal(uint32_t op_id);   /* the one lethal fixture call */
void vsp_cmd_transfer(uint32_t op_id); /* the predeclared fixture_request */
void vsp_cmd_refusal(uint32_t op_id);  /* arm at the next real ACCEPT */
/* Server thread: consume one command (its hook bit drops with it). 0 = none
 * pending or the hook was reset. */
uint32_t vsp_take_cmd(enum vsp_cmd cmd);
/* The script declares the p2 reload; only then do the load-path sites emit
 * reload records (the boot-time load of the fresh rig emits none). */
void vsp_expect_reload(uint32_t checkpoint_id);
uint32_t vsp_checkpoint_id(void); /* last checkpoint begin id (server) */
/* The fixture origin (block anchor) and the three targets the server built,
 * readable on main once rig_ready was pushed. */
bool vsp_fixture_targets(struct vsp_r out[3], int32_t anchor[3]);
uint32_t vsp_fixture_hash_now(void);

/* Ride windows and isolation brackets: opened/closed by the script; each
 * owner accumulates and emits its own record. */
void vsp_ride_window(uint32_t window_id, uint32_t target_id, bool open);
void vsp_iso_bracket(uint32_t attempt_op, bool open);
/* True once the SERVER has seen the host and its mount at zero steering for
 * >= min_updates consecutive server updates (any thread). */
bool vsp_server_host_at_rest(uint32_t min_updates);

/* O14 (CONTRACT-v1.3 "two-home P2 fixture"): the death-phase remote home.
 * N = Beta death-drop flight (<= 8.47 m, fall <= 3.5 m, no ice) + the pickup
 * box's Euclidean corner reach (1.425 * sqrt 2 = 2.016 m) + 0.5 m margin =
 * 10.98 m, used as 11.0 m. D is the horizontal region holding every MOUNTED
 * death point, relative to the fixture anchor (ax, az). */
#define VSP_DROP_FLIGHT_M 8.47
#define VSP_PICKUP_REACH_EUCLID_M 2.016
#define VSP_DROP_N_M 11.0
#define VSP_DROP_FALL_MAX_M 3.5
#define VSP_DEATH_D_X0 (-2.0)
#define VSP_DEATH_D_X1 4.0
#define VSP_DEATH_D_Z0 0.0
#define VSP_DEATH_D_Z1 2.5
/* H2 = (ax + VSP_HOME2_DX, az + VSP_HOME2_DZ): 12.5 m from D's nearest point. */
#define VSP_HOME2_DX (-0.5F)
#define VSP_HOME2_DZ (-12.5F)
/* Horizontal distance from (x, z) to D for the fixture anchored at (ax, az). */
double vsp_death_region_distance(int32_t ax, int32_t az, double x, double z);
/* True once the SERVER's authoritative guest body has moved < 0.0001 m for
 * >= min_updates consecutive server updates (any thread). */
bool vsp_server_guest_at_rest(uint32_t min_updates);
/* Script -> server: from now until disarm, a present Overworld guest closer
 * than VSP_DROP_N_M to D fails BY NAME (guest_inside_drop_envelope). */
void vsp_far_home_enforce(void);
/* Smallest guest-to-D distance the server saw while enforcing (metres; a
 * negative value = never sampled). Diagnostic, any thread. */
double vsp_far_home_min_m(void);
/* One stderr diagnostic line at disarm (guest-to-D minimum, nearest item). */
void vsp_far_home_report(void);
/* p5 FINAL detached census window (contract p5 "stable detached census and
 * surviving released source"): the same ride_sample census, but an update or
 * client dispatch pass is ELIGIBLE when the released target is alive in its
 * world and NOT ridden by the host -- tick_calls are still the real vehicle
 * tick dispatches, missing/duplicate still per update/pass. Set before the
 * window opens; cleared by the next ordinary vsp_ride_window. */
void vsp_ride_window_detached(uint32_t window_id, uint32_t target_id,
							  int world_slot, bool open);
/* The vehicle a bracket is ABOUT (a P2 probe's target), set before opening
 * it; 0 = the host's current mount, else the run kind's fixture target. */
void vsp_iso_target(uint32_t target_id);
bool vsp_iso_server_closed(uint32_t attempt_op); /* after record pushed */
bool vsp_ride_server_closed(uint32_t window_id);

/* ---- server-thread producers (S) ---------------------------------------- */
void vsp_srv_update_begin(struct server_local* s);
/* After the paused/loading gates: commands, censuses. */
void vsp_srv_update_stable(struct server_local* s, bool loading,
						   bool entity_tick_completed);
/* The pause/loading early returns of server_local_update. */
void vsp_srv_update_held(struct server_local* s, bool paused_hold);
/* Immediately before server_local_guest_join_capacity_tick. */
void vsp_srv_retry_pass(struct server_local* s);
void vsp_srv_vehicle_ticked(struct server_local* s, struct entity* e);

/* entity use / attack / cart ride intent: pre/post around the real handler. */
struct vsp_use_pre {
	uint32_t target_id;
	uint32_t old_id; /* the vehicle the actor validly rode before, 0 none */
	uint8_t old_kind;
	struct vsp_v old_before;
	bool target_saddled_before;
	bool target_mounted_before;
	bool actor_is_host;
	uint32_t request_seq;
};
bool vsp_srv_use_pre(struct server_local* s, struct server_player* actor,
					 uint32_t target_id, uint32_t request_seq,
					 struct vsp_use_pre* pre);
void vsp_srv_use_post(struct server_local* s, struct server_player* actor,
					  const struct vsp_use_pre* pre, enum vsp_rpc rpc);
/* CONTRACT-v1.1 Q9: the REAL pre-mount release witness. Called by
 * server_local_release_actor_mounts after it dropped `released_id`'s relation
 * to `actor` (and zeroed the actor's steering) and BEFORE the caller writes
 * the new relation. Inside a host use bracket (vsp_srv_use_pre..post) it
 * allocates the release serial at the release itself and emits the
 * vehicle_smoke.dismount observation under the bracket's op; the switch
 * result only references that serial. Outside a bracket (transfer commit,
 * guest leave, death) it does nothing: those seams own their own witnesses. */
void vsp_srv_release_witness(struct server_local* s,
							 struct server_player* actor, uint32_t released_id);

void vsp_srv_pause_toggled(struct server_local* s);
void vsp_srv_input_ack(struct server_local* s, uint32_t epoch,
					   bool host_presentation_cleared);
void vsp_srv_health_committed(struct server_local* s, short old_health,
							  uint32_t cause);
void vsp_srv_respawn_stage(struct server_local* s, enum vsp_stage_respawn st);
void vsp_srv_checkpoint_stage(int stage_index, bool ok); /* 0 level,1 chunks,2 sync */
void vsp_srv_checkpoint(struct server_local* s, enum vsp_stage_ck stage,
						bool ok);
void vsp_srv_world_teardown(struct server_local* s);
void vsp_srv_reload_disk(struct server_local* s, const float pos[3],
						 bool player_read_ok, bool fallback_used,
						 bool override_used);
void vsp_srv_reload_inventory(struct server_local* s, bool read_ok);
void vsp_srv_guest_accept(struct server_local* s);
bool vsp_srv_refuse_publish(struct server_local* s); /* SG predicate */
void vsp_srv_publish_blocked(struct server_local* s, enum vsp_cause cause);
void vsp_srv_installed(struct server_local* s);
void vsp_srv_guest_ready(struct server_local* s);
void vsp_srv_transfer_requested(struct server_local* s,
								struct server_player* actor, uint32_t id,
								bool fixture);
void vsp_srv_transfer_released(struct server_local* s,
							   struct server_player* actor, uint32_t id,
							   uint8_t src_slot, uint8_t dst_slot);
void vsp_srv_transfer_committed(struct server_local* s,
								struct server_player* actor, uint32_t id,
								uint8_t src_slot, uint8_t dst_slot,
								uint32_t transition_epoch);
/* Guest native relation edge census around the mount writers. */
void vsp_srv_guest_edge_census(struct server_local* s);
/* server_local.c's fixture helper reports what it built / verified. */
/* `live_hash` re-reads the fixture cells (checkpoint and reload receipts
 * hash what is ACTUALLY there, never the build-time value). */
void vsp_srv_fixture_built(struct server_local* s, uint32_t op,
						   const int32_t anchor[3], const uint32_t ids[3],
						   uint32_t fixture_hash,
						   uint32_t (*live_hash)(struct server_local* s));
void vsp_srv_rig_ready(struct server_local* s, uint32_t op, bool water_ok,
					   bool support_ok, bool rails_ok, bool ray_ok,
					   bool saddle_consumed);
uint32_t vsp_pig_ai_updates(void);
/* A fixture/hook command that could not be carried out honestly (allocation
 * failure, host not mounted, request refused). The script fails the run with
 * this code; the server never retries a consumed command. */
void vsp_srv_fixture_failed(const char* code);
/* Main thread: the first such failure code, or NULL. */
const char* vsp_fixture_failure(void);
/* Bracket the fixture_request call so server_transfer_request's begin
 * record carries mode=fixture_request / origin=fixture. */
void vsp_srv_transfer_fixture(bool on);
bool vsp_srv_transfer_fixture_active(void);
/* Bracket the disclosed lethal call so its death record carries
 * origin=fixture and cause=fixture_lethal. */
void vsp_srv_lethal_begin(bool on);
/* Split-owned host respawn: the readiness ACK is SRPC_RESPAWN(epoch). */
void vsp_srv_respawn_ack(struct server_local* s, uint64_t epoch);
uint32_t vsp_srv_installed_count(void);

/* ---- main/client-thread producers (M/C) --------------------------------- */
/* server_interface.c svin_rpc_send_from (the contract's ingress site): a
 * slot-1 vehicle RPC refused at the closed mailbox gate (admitted=false ->
 * status refused) or admitted through it (admitted=true -> status sent). */
void vsp_ingress_refused(const struct server_rpc* call, uint8_t slot,
						 uint64_t generation, bool admitted);
/* client_interface.c clin_process_observed witness, around the handler. */
struct vsp_cw_pre {
	uint32_t guest_rel;  /* bitmask of native guest relations before */
	float guest_pos[3];
	bool guest_pred;     /* guest cart prediction before */
	bool valid;
};
void vsp_client_witness_begin(const struct client_rpc* call,
							  struct vsp_cw_pre* pre);
void vsp_client_witness_end(const struct client_rpc* call,
							const struct vsp_cw_pre* pre);
/* entity_local_player.c: one ACTUAL mounted follow/pin application. */
void vsp_client_follow(const struct client_session* session,
					   const struct entity* body, const struct entity* vehicle,
					   float seat_error);
/* Client counters for the open bracket / ride window, read by the script. */
struct vsp_client_counters {
	uint32_t guest_follow_count, unexpected_route_count;
	uint32_t unexpected_attach_count;
	uint32_t host_follow_count;   /* ride window: follows on the target */
	uint32_t host_follow_target;  /* last target the host followed */
	float host_seat_error_max;
	bool route_violation_sticky;
	/* CONTRACT-v1.1 Q3: the ACTUAL client vehicle dispatch, counted apart
	 * from follow/seat. ride_eligible is the independent census (one per
	 * entities_client_tick pass over the host's bound view while the host
	 * body is natively the target's rider); ride_tick_calls counts the real
	 * dispatcher's calls of the target's tick_client inside those passes;
	 * zero calls in an eligible pass is missing, two or more duplicate.
	 * Accumulated per pass across the whole window (Q2), never a final
	 * snapshot. */
	uint32_t ride_eligible, ride_tick_calls, ride_missing, ride_duplicate;
	uint32_t ride_calls_max;
	float ride_disp_max;
	float ride_start[3];
	bool ride_started;
	bool ride_detached; /* p5 detached-census mode for this window */
};
void vsp_client_counters(struct vsp_client_counters* out);
void vsp_client_counters_window_reset(bool iso, bool ride, uint32_t ride_target);
/* CONTRACT-v1.1 Q5: the named view's own active input epoch (host = the
 * acknowledged clin_input_epoch(); guest = 0, it has no input-epoch
 * handoff). main thread. */
uint32_t vsp_client_actor_epoch(unsigned view);

/* Q3 dispatch tap. main.c brackets its real entities_client_tick(ts.entities)
 * call with begin/end. While a client ride window is open and `ents` is the
 * host's bound view, begin decides eligibility (the target exists there and
 * natively names the host body as rider) BEFORE the pass and, if eligible,
 * routes the target's own tick_client through a counting trampoline that
 * calls the original unchanged; end restores the original pointer (unless
 * the dispatcher removed the entity) and books the pass. Outside an armed
 * window both are no-ops. main thread only. */
struct dict_entity_s;
struct vsp_dispatch_tap {
	struct entity* e;
	bool (*orig)(struct entity*);
	uint32_t id, calls;
	bool eligible, tapped;
};
void vsp_client_dispatch_begin(struct vsp_dispatch_tap* t,
							   struct dict_entity_s* ents, int slot);
void vsp_client_dispatch_end(struct vsp_dispatch_tap* t,
							 struct dict_entity_s* ents);
/* Test seam: the same tap against an explicit host-view dict and body id. */
void vsp_client_dispatch_begin_for(struct vsp_dispatch_tap* t,
								   struct dict_entity_s* ents,
								   struct dict_entity_s* host_view,
								   uint32_t host_body_id);
void vsp_client_dispatch_begin_slot(struct vsp_dispatch_tap* t,
									struct dict_entity_s* ents,
									struct dict_entity_s* host_view,
									uint32_t host_body_id, int slot);

/* Snapshot helpers usable by the main thread (client state only). */
bool vsp_snap_client(const struct client_session* session, unsigned view,
					 struct vsp_c* out);
void vsp_snap_client_absent(const struct client_session* session,
							struct vsp_c* out);
/* O15: the client entity's real world AABB in a view's payload (the box
 * main.c's camera entity ray tests): x1,y1,z1,x2,y2,z2. Main thread. */
bool vsp_client_entity_box(unsigned view, uint32_t id, float out[6]);
bool vsp_snap_client_vehicle(unsigned view, uint32_t id, uint8_t kind,
							 struct vsp_vc* out);
bool vsp_snap_client_vehicle_slot(unsigned slot, uint32_t id, uint8_t kind,
								  struct vsp_vc* out);
uint32_t vsp_client_inventory_hash(const struct client_session* session,
								   uint32_t* used);

/* FNV-1a helpers (exported for the tests). */
uint32_t vsp_fnv_begin(void);
uint32_t vsp_fnv_bytes(uint32_t h, const void* data, size_t n);

#endif
