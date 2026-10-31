/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-T (split-dimensions-plan-0924, Astra A2 design sec. 2 "Player transaction";
 * astra-next-0926 row A6): the ACTOR TRANSFER TRANSACTION. One actor (the host
 * or the admitted guest body) moves between the two live worlds of a session
 * -- Overworld <-> Nether -- as a server-owned transaction:
 *
 *     REQUEST -> PREPARE -> COMMIT      (the actor is a member of the destination)
 *     REQUEST -> PREPARE -> ROLLBACK    (the actor is exactly where it was)
 *
 * REQUEST validates the actor's committed membership (server_local_world_of)
 * and admits the destination world (A2-W lifecycle: EMPTY -> LOADING -> LIVE,
 * or an already-LIVE world), binding a request id, the actor identity
 * (generation), source/destination handles and a transition epoch. A second
 * request for the same actor while one is open answers DUPLICATE with the
 * existing id. Opposite requests in the same tick (host -> Nether while the
 * guest -> Overworld) are serialized Overworld-source first, then by actor id;
 * the partner keeps simulating in its own world throughout.
 *
 * PREPARE stages everything reversible -- dismount, window closure, the
 * destination pose (scaled 8:1), the portal search/build (portal_teleporter on
 * the DESTINATION world) -- while the SOURCE stays authoritative. Only the
 * transferring actor's actions are gated. Any failure before the commit marker
 * rolls back transaction-owned state: the actor, its pose, inventory and
 * membership are untouched in the source, and the partner never notices.
 *
 * COMMIT is one allocation-free step after the durable marker: membership
 * switches atomically (server_local_world_of answers the destination), the
 * pose is installed, the client is told through the generation-tagged
 * transition (lane C applies it to the guest body, lane B binds the client's
 * concurrent world). Death/respawn across dimensions is the same lifecycle
 * (respawn transaction to the Overworld; death-world drops stay where they
 * fell). No journal dependency: the checkpoint journal is deferred; durability
 * here is the existing chunks-first / level.dat-last contract.
 *
 * Lifecycle (A2-W, server_local.[ch]): a world slot is EMPTY, LOADING, LIVE or
 * RETIRING; the server thread alone moves it. An unoccupied LIVE world stays
 * LIVE with zero resident columns; retirement (server_local_world_retire)
 * happens only when no actor is committed to it, drains its leases and FREES
 * its entities (A2-E forward finding F1: a retired generation's entities used
 * to stay in s->entities until the next slot-0 reset). Services run ONCE per
 * world per tick (world tick, weather, spawning) and session services once
 * per tick (sleep quorum), Overworld then Nether.
 *
 * API announcement: the implementation defines SERVER_TRANSFER_API. Until then
 * the transaction tests (tests/test_a2_transfer_transaction.c) are RED by
 * construction. */
#ifndef SERVER_TRANSFER_H
#define SERVER_TRANSFER_H

#include "../entity/actor_ident.h"
#include "../world.h"
#include "portal_teleporter.h" /* struct portal_teleporter_pose */
#include "server_world.h" /* struct server_world_handle */

#define SERVER_TRANSFER_API 1

#include <stdbool.h>
#include <stdint.h>

struct server_local;
struct server_player;

/* ---- the frozen transition type lane C consumes (C3-27-needs-from-A #4) --- */
enum server_transition_phase {
	SERVER_TRANSITION_PREPARE = 0, /* staged; source still authoritative */
	SERVER_TRANSITION_COMMIT,	   /* membership switched; apply the pose */
	SERVER_TRANSITION_ROLLBACK,	   /* nothing changed; release staged state */
};

struct server_actor_transition {
	uint32_t request_id;
	struct actor_ident actor; /* slot + generation of the acting body */
	struct server_world_handle source;
	struct server_world_handle destination;
	enum world_dim destination_dimension;
	double x, y, z; /* committed pose (COMMIT), or the source pose (ROLLBACK) */
	float rx, ry;
	enum server_transition_phase phase;
	uint32_t transition_epoch; /* increments per transaction of this session */
};

/* ---- request / drive ---------------------------------------------------- */
enum server_transfer_state {
	SERVER_TRANSFER_IDLE = 0, /* unknown / never requested id */
	SERVER_TRANSFER_REQUESTED,
	SERVER_TRANSFER_SOURCE_DRAINING, /* held actor, source outer-ring eviction */
	SERVER_TRANSFER_RESTORING, /* rollback restores source ring before release */
	SERVER_TRANSFER_PREPARING,	 /* the Teleporter works on the destination */
	SERVER_TRANSFER_CAPACITY,	 /* CRPC_CAPACITY_REQUEST(RESERVE) sent; waiting
								  * for SRPC_CAPACITY_REPLY (lane B, Astra a2t-0927:
								  * client/GPU capacity is reserved BEFORE membership) */
	SERVER_TRANSFER_LANDING,	 /* CRPC_LANDING_ARM sent; waiting for
								  * SRPC_LANDING_READY (membership only when the
								  * landing is ready on the client) */
	SERVER_TRANSFER_COMMIT_READY, /* the client is ready: the next step commits */
	SERVER_TRANSFER_COMMITTED,	 /* terminal: the actor is in the destination */
	SERVER_TRANSFER_ROLLED_BACK, /* terminal: the actor is where it was */
};

/* Sticky, per-actor diagnostic for a rollback that may finish between heartbeats. */
enum server_transfer_rollback_reason {
	SERVER_TRANSFER_ROLLBACK_NONE = 0,
	SERVER_TRANSFER_ROLLBACK_SESSION,
	SERVER_TRANSFER_ROLLBACK_ACTOR_DIED,
	SERVER_TRANSFER_ROLLBACK_ACTOR_LEFT,
	SERVER_TRANSFER_ROLLBACK_DESTINATION_GONE,
	SERVER_TRANSFER_ROLLBACK_WINDOW,
	SERVER_TRANSFER_ROLLBACK_TELEPORTER_ALLOC,
	SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAULT,
	SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAILED,
	SERVER_TRANSFER_ROLLBACK_CHECKPOINT,
	SERVER_TRANSFER_ROLLBACK_CAPACITY_TIMEOUT,
	SERVER_TRANSFER_ROLLBACK_LANDING_TIMEOUT,
	SERVER_TRANSFER_ROLLBACK_HOST_RECORD,
	SERVER_TRANSFER_ROLLBACK_GUEST_RECORD,
	SERVER_TRANSFER_ROLLBACK_CLIENT_REFUSED,
	SERVER_TRANSFER_ROLLBACK_CLIENT_BAD_SLOT,
	SERVER_TRANSFER_ROLLBACK_CLIENT_BUSY,
	SERVER_TRANSFER_ROLLBACK_CLIENT_NO_CONTEXT,
	SERVER_TRANSFER_ROLLBACK_CLIENT_NO_MEM1,
	SERVER_TRANSFER_ROLLBACK_CLIENT_NO_MEM2,
};

/* The client round trips are bounded: a reply that never comes rolls the
 * transaction back (visible), it never hangs the tick. */
#define SERVER_TRANSFER_CLIENT_WAIT_TICKS 600
/* M2: pending capacity RELEASEs (two actors x {rollback, stale OK} plus one
 * committed ticket per world slot fits with room to spare). */
#define SERVER_TRANSFER_CAP_RELEASE_QUEUE 8
/* Legacy single-payload request stays byte-for-byte unchanged. In local
 * world-payload mode the MEM1 request comes from the shared-shell bound in
 * world_local_shared_capacity_mem1_need; server-owned terrain is not copied. */
#define SERVER_TRANSFER_MEM1_PER_COLUMN (48u * 1024u)
#define SERVER_TRANSFER_MEM2_PER_COLUMN (32u * 1024u)

/* Gap-doc blocker 3: the ARRIVAL CORE of the destination world is streamed to
 * the transferring player's client during LANDING, stamped with the
 * destination, and the landing is armed over exactly that core (Chebyshev
 * radius 1 around the landing column: 9 columns -- the area PREPARE's
 * Teleporter already made resident). The client prepares an unviewed context
 * for the slot on the ARM and reports READY once the core is installed.
 * The stream is bounded: at most SERVER_TRANSFER_LANDING_SENDS_PER_TICK
 * columns per step, never a blocking pool acquire or send, and a core column
 * that is NOT yet resident is generated only while the session holds fewer
 * than SERVER_TRANSFER_RESIDENT_COLUMN_CAP resident columns over all worlds. */
#define SERVER_TRANSFER_LANDING_CORE_RADIUS 1
#define SERVER_TRANSFER_LANDING_CORE_COLUMNS 9u
#define SERVER_TRANSFER_LANDING_SENDS_PER_TICK 2u
/* The ONE authoritative resident-column ceiling over all worlds (colcap-1006,
 * ASTRA-COLUMN-CAP-R1 + ASTRA-CC-R2 items 1-2): 242 demonstrated initial-load
 * demand + 14 transient slots, bounded by eight 32-slot slabs. 242 = two
 * disjoint radius-4 views under the 3x3 population-support contract
 * (2 x 11^2; coincident coordinates in DIFFERENT worlds still count twice).
 * Enforced on live backings (incl. client-retained) + outstanding
 * reservations (column_reserve), never on dictionary size alone; source
 * draining and reservation rollback stay the progress mechanism. Was 176
 * (2 x 81 + margin), which pinned the split 256-block case (I07 i06/i06b). */
#define SERVER_TRANSFER_RESIDENT_COLUMN_CAP 256u
_Static_assert(SERVER_TRANSFER_RESIDENT_COLUMN_CAP <= 8u * 32u,
			   "the resident ceiling must fit eight 32-slot column slabs");
enum server_transfer_budget_phase {
	SERVER_TRANSFER_BUDGET_OFF = 0,
	SERVER_TRANSFER_BUDGET_DRAINING,
	SERVER_TRANSFER_BUDGET_ENFORCED,
};
struct server_transfer_budget_snapshot {
	uint32_t resident, live_backings, reserved;
	uint32_t prebudget_peak, enforced_peak, cap_waits;
	uint32_t request_id, guest_commits;
	uint32_t rollback_id[2];
	uint8_t budget_phase, actor_slot, transfer_state;
	uint8_t rollback_reason[2];
	bool held, source_draining, restoring;
};
/* False after bounded contention; caller must report invalid, not zero. */
bool server_transfer_budget_snapshot_get(
	struct server_transfer_budget_snapshot* out);
/* At most one synchronous ensure_populated cascade runs on the server thread.
 * Its target and four covering quadrants can load the target's 3x3 footprint. */
#define SERVER_TRANSFER_COLUMN_RESERVATIONS 9u
struct server_transfer_column_reservation {
	uint8_t slot;
	uint32_t generation;
	w_coord_t x, z;
};

/* The preflight is all-or-nothing and happens before a population RNG draw.
 * The common load gate also covers direct Teleporter winner loads. A successful
 * insertion consumes its keyed reservation; finish releases unconsumed keys. */
bool server_transfer_column_preflight(struct server_world* w, w_coord_t x,
										w_coord_t z);
void server_transfer_column_preflight_finish(struct server_world* w);
/* colcap-1006 (pre-existing bug, Beta chunkLoadOverride parity): an OPEN
 * transaction whose destination is `w` pins its Teleporter work area --
 * Chebyshev SERVER_TRANSFER_TELEPORTER_PIN_RADIUS chunks around the staged
 * destination pose's chunk: the create/load range (pose +/-20 blocks), the
 * frame build (pose +/-16) and, once the pose is final, the landing core
 * (radius 1). Beta wraps the whole Teleporter call in
 * chunkProviderServer.chunkLoadOverride (ServerConfigurationManager.java:
 * 209-211; ChunkProviderServer.java:86): every chunk it touches is
 * force-loaded and nothing unloads mid-call. Our Teleporter runs across
 * updates, so the evictor must not retire those columns in between (the
 * slot-0 near evictor did, every tick, failing tp_job_build). Pinned columns
 * are ordinary live backings: they still count toward
 * SERVER_TRANSFER_RESIDENT_COLUMN_CAP. Returns the first pin rectangle for
 * `w` (for the furthest-column selection) and whether any applies. */
#define SERVER_TRANSFER_TELEPORTER_PIN_RADIUS 2
/* PREPARING (the Teleporter's sliced search/load/build) is bounded: past
 * this many slices the transfer rolls back (counted) instead of holding the
 * actor indefinitely. 2400 server ticks = 2 minutes. */
#define SERVER_TRANSFER_PREPARE_MAX_TICKS 2400u
uint32_t server_transfer_prepare_timeouts(void);
/* Column-ceiling excursions caught at the allocation points (column loads and
 * finished population preflights) while the budget is ENFORCED: count and
 * peak live_backings + reserved. Zero in a correct run. */
void server_transfer_cap_excursions(uint32_t* count, uint32_t* peak);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* 0 restores SERVER_TRANSFER_PREPARE_MAX_TICKS */
void server_transfer_test_set_prepare_max_ticks(unsigned ticks);
#endif
bool server_transfer_teleporter_pin(const struct server_local* s,
									const struct server_world* w,
									w_coord_t* pin_x, w_coord_t* pin_z,
									w_coord_t* pin_r);
/* Whether (cx, cz) of `w` lies in ANY open transaction's pinned area. */
bool server_transfer_teleporter_pinned(const struct server_local* s,
									   const struct server_world* w,
									   w_coord_t cx, w_coord_t cz);
bool server_transfer_column_load_begin(struct server_world* w, w_coord_t x,
										w_coord_t z, bool* ephemeral);
void server_transfer_column_load_end(struct server_world* w, w_coord_t x,
									  w_coord_t z, bool ephemeral);
void server_transfer_column_inserted(struct server_world* w, w_coord_t x,
									w_coord_t z);
void server_transfer_column_wait_reset(struct server_world* w);
bool server_transfer_column_waiting(const struct server_world* w);

/* M4: the client shows a HUD notice for CLIENT_HUD_NOTICE_MS (3 s); the
 * travelling notice is re-sent every 2.5 s so it stays up for the whole wait
 * (PREPARE + the two client round trips, up to ~60 s) instead of silence. */
#define SERVER_TRANSFER_HUD_REFRESH_TICKS 50u
#define SERVER_TRANSFER_HUD_TRAVELLING "Travelling..."
#define SERVER_TRANSFER_HUD_CANCELLED "Travel cancelled"

/* One actor's transaction (session state; one per actor slot). */
struct server_transfer_txn {
	bool open;
	bool hold_sent; /* named owner stopped before bounded source drain */
	bool hold_end_pending; /* terminal END retries when the inbox is full */
	bool hold_end_restored; /* rollback END waits for source client terrain */
	w_coord_t source_cx, source_cz;
	uint8_t restore_radius; /* fixed at rollback; sent with restored-source END */
	uint32_t id;
	uint8_t actor_slot; /* ACTOR_SLOT_HOST / ACTOR_SLOT_GUEST */
	uint64_t actor_generation;
	uint8_t src_slot, dst_slot;
	enum world_dim dst_dim;
	enum server_transfer_state state;
	bool admitted_dst; /* this transaction admitted the destination world */
	/* the client admission round trips (lane B's client_admission.h) */
	bool request_sent;		/* the current CRPC was admitted by the transport */
	unsigned wait_ticks;	/* ticks spent waiting for the current reply */
	unsigned commit_wait_ticks; /* residency-1005/2: landing footprint wait */
	bool capacity_reserved; /* RESERVE answered OK: `capacity_ticket` is held */
	bool capacity_disabled; /* CLIN_CAP_DISABLED: single-world client, no landing */
	uint32_t capacity_ticket;
	/* M4: the "Travelling..." notice is refreshed every
	 * SERVER_TRANSFER_HUD_REFRESH_TICKS while the transaction is open */
	unsigned hud_ticks;
	bool hud_shown;
	/* M3: a LANDING_ARM was admitted by the transport: any rollback from here
	 * on (LANDING or COMMIT_READY) sends the cancel */
	bool landing_armed;
	bool landing_cancel_pending; /* nonblocking rollback cancel retry */
	/* the guest moves into the world player 0's view already shows (world
	 * payloads ON): the client holds it -- no capacity reservation (the
	 * client refuses one for its primary slot) and no landing */
	bool joins_primary;
	/* blocker 3: core columns (bit dz*3+dx over the landing core) already
	 * handed to the transport for this transaction, and the count of columns
	 * sent in total (diagnostics / tests) */
	uint16_t landing_sent_mask;
	unsigned landing_columns_sent;
	struct portal_teleporter_pose pose; /* staged destination pose */
	struct portal_teleporter_job* job;
	struct server_actor_transition outcome;
	bool outcome_valid;
	/* Sol review 2 (A): PREPARING slices so far; bounded by
	 * SERVER_TRANSFER_PREPARE_MAX_TICKS */
	unsigned prepare_ticks;
	uint32_t last_id; /* the closed transaction's id and terminal state */
	enum server_transfer_state last_state;
};

enum server_transfer_rc {
	SERVER_TRANSFER_OK = 0,			   /* accepted; PREPARE runs on the next steps */
	SERVER_TRANSFER_DUPLICATE,		   /* this actor already has an open request:
										* *request_id is the existing one */
	SERVER_TRANSFER_REFUSED_MEMBERSHIP, /* the actor has no committed live world */
	SERVER_TRANSFER_REFUSED_SAME_WORLD, /* destination == the actor's world */
	SERVER_TRANSFER_REFUSED_BUSY,	   /* a session checkpoint transaction or a
										* world switch owns the tick */
	SERVER_TRANSFER_REFUSED_ADMISSION,  /* the destination world could not be
										* admitted (memory / load refusal) */
};

/* REQUEST. `actor` is &s->player or s->guest_actor. On OK/DUPLICATE
 * *request_id names the transaction. Server thread only. */
enum server_transfer_rc server_transfer_request(struct server_local* s,
												struct server_player* actor,
												enum world_dim destination,
													uint32_t* request_id);
/* Server input gates use this as well as the client's local physics hold. */
bool server_transfer_actor_frozen(const struct server_local* s,
									 const struct server_player* actor);
/* Body mutation gate also spans a guest's COMMIT until its own READY body
 * installs and a valid pose echoes the fresh correction token. Streaming uses
 * frozen so a committed guest presentation can refill its full landing ring. */
bool server_transfer_actor_inert(const struct server_local* s,
	const struct server_player* actor);
/* Only a committed host A2 presentation with an admitted guest may keep the
 * world clock and guest gameplay live across the host's own input barrier. */
bool server_transfer_guest_continuity(const struct server_local* s);
bool server_transfer_source_stream_allowed(const struct server_world* w,
										w_coord_t cx, w_coord_t cz);
/* Called on the server thread before transfer work and again after eviction. */
void server_transfer_budget_tick(struct server_local* s);
/* One bounded step of every open transaction (the server update calls this
 * once per tick; tests may call it directly). True when no request is open. */
bool server_transfer_step(struct server_local* s);
enum server_transfer_state server_transfer_state_of(const struct server_local* s,
													uint32_t request_id);
/* The last transition published for `request_id` (COMMIT or ROLLBACK), or
 * NULL while it is still open / unknown. Valid until the next step. */
const struct server_actor_transition*
server_transfer_outcome(const struct server_local* s, uint32_t request_id);
/* Any transaction open in this session (C3 need #5: join / respawn / checkpoint
 * refuse while true, exactly like world_switch_phase != IDLE). */
bool server_transfer_active(const struct server_local* s);
/* The client's answers (SRPC_CAPACITY_REPLY / SRPC_LANDING_READY, routed from
 * the server RPC switch). A reply that names no open transaction, or a
 * landing for another world instance, is ignored (stale). */
void server_transfer_on_capacity_reply(struct server_local* s, uint32_t request_id,
									   uint8_t result, uint32_t ticket);
void server_transfer_on_landing_ready(struct server_local* s, uint32_t token,
									  uint8_t slot, uint32_t generation);
/* Resident columns over every live world (the transfer cap's measure). */
size_t server_transfer_resident_columns(const struct server_local* s);
/* True while an OPEN transaction names `w` as its source or destination
 * (the vacant-world drain must not evict a destination under PREPARE). */
bool server_transfer_world_in_flight(const struct server_local* s,
									 const struct server_world* w);
/* Session init (server_local_init) and the host's death in a non-routed world
 * (server_local_respawn_request delegates here: drops stay in the death
 * world, the fresh body is committed to the Overworld, no world teardown). */
void server_transfer_init(struct server_local* s);
bool server_transfer_respawn_host(struct server_local* s);
/* Session release of every world slot >= 1 (save-and-quit / menu / reload):
 * open transactions roll back, then EVERY held world checkpoints and syncs
 * before ANY is discarded (Fable A2T-SERVER-REVIEW H1: all-or-nothing). True:
 * all were checkpointed and discarded, whatever their membership (the actors
 * keep their saved dimension in the records and fall back to the routed slot
 * for the next load). False: a checkpoint failed and NOTHING was released --
 * the worlds stay LIVE, resident, with their entities; the caller must not
 * proceed as if saved (the production unload refuses like a slot-0 checkpoint
 * failure). */
bool server_local_worlds_release_secondary(struct server_local* s);
/* The checkpoint half alone (all-or-nothing, nothing discarded, no rollback
 * of open transactions): the production unload runs it BEFORE the routed
 * world's own checkpoint so a refusal leaves every world -- and the host's
 * committed slot -- exactly as they were. */
bool server_local_worlds_checkpoint_secondary(struct server_local* s);
/* server_local.c: the host's client side of an A2-T COMMIT (BEGIN for the
 * source world, stamped WORLD_RESET for the destination, the destination's
 * sent claims dropped unless the host joins the guest's live world). */
void server_local_host_transfer_publish(struct server_local* s, uint8_t src_slot);
/* M2(d): a world slot the client was told to release (the vacated-world
 * drain's CRPC_WORLD_RELEASE) no longer needs its committed capacity
 * reservation: its ticket is released. */
void server_transfer_on_world_vacated(struct server_local* s, uint8_t slot);
/* Server-thread handoff: true means transport or the existing retry queue
 * owns this ticket (including an already queued duplicate). False leaves
 * ownership with the caller. RELEASE is ticket-only, never world-stamped. */
bool server_transfer_release_capacity(struct server_local* s, uint32_t ticket);

/* ===== A2-W: per-world streaming for world slots >= 1 (A2W-CONTRACT.md) ====
 * One bounded slice per server update, for every LIVE world in slot >= 1
 * whose client context exists (server_transfer_stream_enable): its resident
 * set is the union of the radius squares around its own eligible actors
 * (radius = server_local_view_radius, A2W-CONTRACT 3); columns outside it are
 * saved, evicted and unloaded on the client (stamped); missing columns are
 * generated nearest-first and sent stamped with that world, at most
 * A2W_SENDS_PER_TICK, never blocking (try_* pool acquire and send). With two
 * actors in play (split or an attached guest) generation respects the
 * resident-column cap over all worlds: A2W_STEADY_COLUMN_CAP, an alias of
 * the single SERVER_TRANSFER_RESIDENT_COLUMN_CAP (ASTRA-CC-R2 item 2: no
 * separate steady ceiling that would starve secondary-world population
 * support). That dictionary-count check is only a preliminary gate; the
 * shared backing/reservation preflight (column_reserve) stays authoritative.
 * Slot 0 keeps its own near/far loop (single player byte-identical).
 * Also releases the host's input-epoch marker once the near ring of the
 * host's own world is streamed (or A2W_MARKER_MAX_WAIT ticks passed). */
#define A2W_SENDS_PER_TICK 2
#define A2W_EVICTS_PER_TICK 8
#define A2W_STEADY_COLUMN_CAP SERVER_TRANSFER_RESIDENT_COLUMN_CAP
#define A2W_MARKER_MAX_WAIT 600u
void server_transfer_stream_step(struct server_local* s);
/* Close only an A2-W-owned secondary save window, reporting publication failure. */
bool server_transfer_flush_owned_batch(struct server_local* s,
									   struct server_world* w);
/* A newly joined secondary entity reopens an already published column at its
 * entity cursor; the bounded stream then sends it after the existing CHUNK. */
void server_transfer_secondary_entity_joined(struct server_local* s,
										 const struct entity* e);
void server_transfer_stream_enable(struct server_local* s, uint8_t slot);
/* server_local.c: arm the host's input handoff and send its marker (the
 * load-screen release) for the host's current world. False when a handoff is
 * already pending. */
bool server_local_host_world_marker(struct server_local* s);
/* The release half that follows a SUCCESSFUL checkpoint_secondary (same
 * session teardown, nothing re-written): open transactions roll back and every
 * secondary slot is dropped and marked EMPTY. */
void server_local_worlds_drop_checkpointed(struct server_local* s);
/* The no-persist counterpart for the abandon / discard release paths: every
 * secondary world is dropped WITHOUT a checkpoint (open transactions roll
 * back first). Stated on purpose: the caller has decided the save is not to
 * be written (the legacy slot-0 discard path makes the same decision). */
void server_local_worlds_discard_secondary(struct server_local* s);

/* ---- A2-W world lifecycle (server_local.c) ------------------------------ */
enum server_world_slot_state {
	SERVER_WORLD_SLOT_EMPTY = 0,
	SERVER_WORLD_SLOT_LOADING,
	SERVER_WORLD_SLOT_LIVE,
	SERVER_WORLD_SLOT_RETIRING,
};
/* The LIVE world for `dimension`, admitting it into a free slot if needed
 * (EMPTY -> LOADING -> LIVE from the session's save root). NULL on refusal
 * (no free slot, memory, load failure) with nothing partially exposed. */
struct server_world* server_local_world_admit(struct server_local* s,
											  enum world_dim dimension);
/* LIVE -> RETIRING -> EMPTY. Refused (false) while an actor is committed to
 * `w` or a transaction names it. Order (Fable A2T-SERVER-REVIEW H1/H2):
 * checkpoint + sync FIRST with every entity still resident (the chunk records
 * carry them), THEN free every entity bound to its generation, reset its
 * entity sections (A2-E F1) and drop its region handles. A failed checkpoint
 * answers false and RETAINS the world exactly as it was (LIVE, resident,
 * entities intact); the next retire retries. */
bool server_local_world_retire(struct server_local* s, struct server_world* w);
enum server_world_slot_state server_local_world_slot_state(
	const struct server_local* s, uint8_t slot);

/* Per-world once-only service counters (test instruments; count per update). */
struct server_world_service_stats {
	uint64_t world_ticks;	/* server_world_tick ran for this world */
	uint64_t weather_ticks; /* the weather pass ran for this world */
	uint64_t spawn_passes;	/* the mob spawn pass ran for this world */
	/* M5: random-tick passes and the chunk the last one centred on (each
	 * world ticks around ITS OWN players only) */
	uint64_t random_tick_passes;
	int32_t random_tick_cx, random_tick_cz;
};
void server_local_world_service_stats(const struct server_local* s, uint8_t slot,
									  struct server_world_service_stats* out);
/* Session-level once-only service counter (the sleep quorum tick). */
uint64_t server_local_session_sleep_ticks(const struct server_local* s);

#ifdef CAVEWII_SERVER_LOCAL_TEST
enum server_transfer_fault {
	SERVER_TRANSFER_FAULT_NONE = 0,
	SERVER_TRANSFER_FAULT_ADMISSION,   /* destination admission fails */
	SERVER_TRANSFER_FAULT_TELEPORTER,  /* portal search/build fails mid-PREPARE */
	SERVER_TRANSFER_FAULT_COMMIT_MARKER, /* the durable marker write fails */
};
void server_transfer_test_fault(enum server_transfer_fault f);
/* Override the carve-free reading the admission budget judges (SIZE_MAX =
 * back to the platform gauge). Models a Wii whose MEM2 carve is nearly full. */
/* Test seam: the next `n` CRPC_CAPACITY_REQUEST sends are refused as if the
 * client inbox were full. */
void server_transfer_test_refuse_capacity_sends(unsigned n);
void server_transfer_test_set_carve_free(size_t bytes);
#endif

#endif
