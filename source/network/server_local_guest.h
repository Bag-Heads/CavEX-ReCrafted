/* Local provider ownership; server-thread-only entry points. GPL-3.0-or-later. */
#ifndef SERVER_LOCAL_GUEST_H
#define SERVER_LOCAL_GUEST_H
#include <stdbool.h>
#include "client_admission.h" /* shared respawn capacity request-ID namespace */
#include "server_interface.h"
struct server_local;
struct server_world;
/* Create/load the world's fixed local guest slot. Returns an inactive actor;
 * caller binds the returned generation to a client session before publish.
 * No entity, terrain admission, or controller assignment is implied. */
bool server_local_guest_create(struct server_local*, struct server_rpc_source*);
bool server_local_guest_publish(struct server_local*, struct server_rpc_source);
/* Block-window private state can be checkpointed while open. Entity windows,
 * riding, fishing, sleep/death and cross-dimension gameplay remain gated. */
bool server_local_guest_settled(const struct server_local*);
bool server_local_guest_checkpoint(struct server_local*);
/* On success send a matching-generation detach acknowledgement to the main
 * thread, which calls client_session_unbind_local. No cross-thread client edit. */
bool server_local_guest_leave(struct server_local*, struct server_rpc_source);
void server_local_guest_control(struct server_local*, struct server_rpc_source,
	const struct server_rpc*);
/* Experimental local split-screen gameplay admission. A NEW admission requires
 * the switch on (boot: server_local_set_split_enabled; live: SRPC_SPLIT_SET),
 * a CONFIRMED guest, the host's dimension and LOADED terrain under the body. On
 * success the actor is finished_loading, owns residency anchor 1 and may stream
 * position. Idempotent; retryable -- and the retry of a body ALREADY admitted
 * is granted whatever the switch says, so a live OFF blocks new joins without
 * ejecting the guest that is already playing. */
bool server_local_guest_gameplay(struct server_local*, struct server_rpc_source);
/* residency-1005/6 R2: this Join generation's bounded terrain wait timed
 * out (the body is never admitted; it holds no stream demand). */
bool server_local_guest_join_wait_failed(const struct server_local*);
bool server_local_guest_respawn(struct server_local*, struct server_rpc_source,
	const struct server_rpc*);
bool server_local_guest_respawn_ready(struct server_local*,
	struct server_rpc_source, const struct server_rpc*);
void server_local_guest_respawn_poll(struct server_local*);
/* Precommit stream anchor after the exact client target is admitted; NULL
 * outside LOADING or if that world generation has retired. Server thread. */
struct server_world* server_local_guest_respawn_target(struct server_local*);
void server_local_guest_respawn_preflight_tick(struct server_local*);
bool server_local_guest_on_landing_ready(struct server_local*, uint32_t token,
	uint8_t slot, uint32_t generation);
/* Position/rotation/movement state plus the shared footstep leg (odometer,
 * step sound, trample -- A1d row #16) and the rider steering source (row #7);
 * portal dwell stays host-only (A2). Ignored unless a body is admitted. */
void server_local_guest_position(struct server_local*, const struct server_rpc*);
/* World-unload implementation only: call immediately after the complete-world
 * checkpoint succeeds, before any further actor/world mutation. */
void server_local_guest_release_checkpointed(struct server_local*);
/* Existing complete-world checkpoint, retaining all owners on failure. */
bool server_local_checkpoint_guests(struct server_local*);
/* R-S1a3 F2: author a fresh correction token and send the guest body back to
 * its server-authored position -- the guest twin of
 * server_local_next_player_correction + CRPC_PLAYER_POS. Called by the
 * per-tick knockback integrator (server_local_tick_actor_knockback) so a shove
 * reaches the guest client and the samples the client authored before it knew
 * about the hit are refused until it echoes the new token. No-op with no body. */
void server_local_guest_correct_position(struct server_local*);
/* Piston sibling: target only the generation captured by its moving-TE pass
 * and preserve the caller's selective PLAYER_CORRECTION_* state. */
bool server_local_guest_correct_position_for(struct server_local*,
	uint64_t expected_generation, uint8_t correction_flags);

/* ===== residency-1005: player residency ADMISSION (ASTRA-1005-RULING.md:1-3,
 * ASTRA-1005B-RULING.md:1-5). Shared by the host SRPC_PLAYER_POS handler and
 * server_local_guest_position; defined in server_local.c.
 * Admit a client-authored pose only when the body's whole 0.6-wide
 * collision/hazard footprint lies in resident columns of the body's own
 * world. A refusal is immediate (no held sample, no replay): the pose is
 * kept, one correction goes out on the owner's route (fresh token; the
 * existing sequence echo then drops the stale in-flight samples), and the
 * missing columns are left to the existing bounded near loader, whose ring
 * is centred on the KEPT pose: a nearby refused footprint is inside it and
 * gets loaded nearest-first, a far one (a host teleport-scale sample) stays
 * corrected back. Returns true when admitted. */
struct server_player;
bool server_local_actor_admit_pose(struct server_local*, struct server_player*,
	double x, double z);
/* Pin an admitted pose as the body's last accepted resident pose. */
void server_local_actor_pin_pose(struct server_local*, struct server_player*);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Equivalence seam: false bypasses admission AND the per-tick settle (the
 * pre-residency-1005 behaviour), so a fully resident run can be compared
 * byte-for-byte with the gate on and off. Default true. */
void server_local_test_set_residency_admission(bool enabled);
/* S2 tests: the slot-0 near-send pick order since the last reset (count of
 * picks; up to `max` of the first 32 copied, rz = served from a body's
 * destination request). */
unsigned server_local_test_near_picks(int32_t* xs, int32_t* zs, bool* rz,
	unsigned max);
void server_local_test_near_picks_reset(void);
/* Lean iterations that skipped a not-yet-generated destination pick. */
unsigned server_local_test_rz_lean_skips(void);
int server_local_test_near_backoff_left(const struct server_local* s,
	int32_t cx, int32_t cz);
#endif

/* ===== A1d (split-dimensions-plan-0924, lane C): per-actor barriers ======
 * Declared here, beside the guest lifecycle, because they are the lane-C
 * surface both bodies share; the definitions live in lane C's reserved
 * server_local.c ranges (sleep: send_sleep_state..sleep_tick; movement: the
 * dispatch range). Every call names the ACTING body -- &s->player or the
 * current s->guest_actor -- and resolves its world with server_local_world_of
 * (D8). */
struct server_player;
/* Beta EntityPlayer.sleepInBedAt's OK branch for the clicking player
 * (host: server_local_begin_sleep, unchanged). */
void server_local_begin_sleep_for(struct server_local*, struct server_player*,
	w_coord_t head_x, w_coord_t head_y, w_coord_t head_z);
/* BlockBed.blockActivated's occupied scan: another present body is asleep
 * with its bed head at this cell. */
bool server_local_bed_occupied_by_other(struct server_local*,
	const struct server_player*, w_coord_t head_x, w_coord_t head_y,
	w_coord_t head_z);
/* EntityPlayer.attackEntityFrom's wake, for the body that was hit. */
void server_local_wake_actor_on_damage(struct server_local*,
	struct server_player*);
/* The position a sleeping body reloads at (Beta readEntityFromNBT wakes a
 * player saved asleep and steps it beside the bed); the live body is not
 * touched. For an awake body: its current position. */
bool server_local_sleep_wake_position(struct server_local*,
	const struct server_player*, double* x, double* y, double* z);
/* Beta Entity.moveEntity's footstep leg for one accepted pose sample of the
 * given body, run with the pre-sample state before the position is stored. */
void server_local_actor_movement_step(struct server_local*,
	struct server_player*, double nx, double ny, double nz);
/* A HUD toast on the given body's own route (host: the untagged
 * CRPC_HUD_MESSAGE; guest: recipient 1 under its generation). */
void server_local_actor_hud_message(struct server_local*,
	const struct server_player*, const char* msg);

/* A2-T: the guest's travel / cross-dimension respawn callbacks. They CONSUME
 * an A2 transaction (REQUEST -> PREPARE -> COMMIT/ROLLBACK is A2's) and never
 * create one: COMMIT is APPLIED to the guest body and its client route,
 * ROLLBACK is acknowledged APPLIED with no change, a stale actor is REFUSED. */
struct server_actor_transition; /* defined by A2 (A2-T / A2-W) */
enum server_actor_transition_result {
	SERVER_ACTOR_TRANSITION_PENDING_A2 = 0, /* retired by A2-T (never returned) */
	SERVER_ACTOR_TRANSITION_APPLIED,
	SERVER_ACTOR_TRANSITION_REFUSED,
};
enum server_actor_transition_result server_local_guest_on_travel_transaction(
	struct server_local*, const struct server_actor_transition*);
enum server_actor_transition_result server_local_guest_on_respawn_transaction(
	struct server_local*, const struct server_actor_transition*);
/* The guest's COMMIT record (A2-T): its record rebuilt AT the staged
 * destination (dimension + pose), installed, and level.dat saved -- the
 * level-last half of chunks-first/level-last, not a durable marker (no
 * journal). false = not written; the prior in-memory record is restored and A2
 * rolls the transaction back. */
bool server_local_guest_commit_record(struct server_local*, enum world_dim dimension,
	double x, double y, double z, float rx, float ry);
/* A2-T: the guest's portal dwell (the host's Beta dwell state machine on the
 * guest body, in its own world); the latch opens the guest's A2-T transaction.
 * No-op while world-bound client payloads are disabled. Called once per server
 * tick from the environment pass, after the host's own portal step. */
void server_local_guest_portal_tick(struct server_local*);
/* A2-T guest JOIN client-capacity round trip. A Join whose admission world is
 * not the host's own asks the client for capacity first (CRPC_CAPACITY_REQUEST
 * RESERVE); these consume the answer and time the request out.
 * on_capacity_reply returns true when the reply was a Join's (its id carries
 * GUEST_JOIN_CAP_ID_BIT) -- the caller then does not offer it to the
 * transfer. */
#define GUEST_JOIN_CAP_ID_BIT 0x80000000u
#define GUEST_RESPAWN_CAP_ID_BIT CLIN_RESPAWN_CAP_ID_BIT

/* Server-thread producer identity; separate from transport/world generations. */
uint64_t server_local_achievement_epoch(struct server_local*,
	const struct server_player*);
bool server_local_guest_on_capacity_reply(struct server_local*, uint32_t request_id,
	uint8_t result, uint32_t ticket);
void server_local_guest_join_capacity_tick(struct server_local*);
#endif
