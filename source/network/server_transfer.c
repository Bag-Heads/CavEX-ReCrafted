/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-T / A2-W: the actor transfer transaction and the two-world lifecycle.
 * See server_transfer.h for the contract. The guest's commit record is lane
 * C's server_local_guest_commit_record (its record at the staged destination,
 * then level.dat -- level-last, no durable-marker guarantee), and its
 * COMMIT/ROLLBACK is published to lane C's adapter
 * (server_local_guest_on_travel_transaction), which APPLIES it to the guest
 * body and its client route (lane C a2t-guest-0927). */
#include "server_transfer.h"
#include "vehicle_smoke_probe.h" /* host_vehicle_smoke_8m producer sites */

#include "../block/blocks.h"
#include "../item/window_container.h" /* WINDOWC_INVENTORY */
#include "../util.h" /* CLOCK_SEED_TIME */
#include "../entity/combat.h" /* MAX_PLAYER_HEALTH */
#include "../platform/mem2.h"
#include "../platform/time.h"
#include "client_admission.h" /* enum clin_capacity_op / clin_capacity_result */
#include "client_interface.h" /* CRPC_INVENTORY_SLOT, the admission CRPCs */
#include "client_world_payload.h" /* clin_world_payloads_enabled */
#include "level_archive.h"
#include "piston_moving_transport.h" /* piston_moving_server_snapshot_w */
#include "portal_teleporter.h"
#include "server_local.h"
#include "server_local_guest.h"
#include "server_local_state.h"
#include "server_world.h"
#include "../worldgen/worldgen.h" /* worldgen_climate */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Astra A2 design sec. 5: "Preserve >= 1 MiB carve headroom". The measured
 * per-world budget is OPEN there; this gate is the documented floor plus the
 * creation's own success -- a refusal is visible (stderr + HUD), never a
 * silent fallback to slot 0. */
#define SERVER_WORLD_ADMIT_CARVE_HEADROOM (1024u * 1024u)

static uint32_t s_budget_seq;
static struct server_transfer_budget_snapshot s_budget_snapshot;
static uint32_t s_guest_commits;
static void budget_publish(struct server_local* s);

bool server_transfer_budget_snapshot_get(
	struct server_transfer_budget_snapshot* out) {
	if(!out)
		return false;
	memset(out, 0, sizeof(*out));
	out->actor_slot = UINT8_MAX;
	for(unsigned tries = 0; tries < 4; tries++) {
		uint32_t before = __atomic_load_n(&s_budget_seq, __ATOMIC_ACQUIRE);
		if(before & 1u)
			continue;
#define SNAP_LOAD(field) out->field = __atomic_load_n(&s_budget_snapshot.field, __ATOMIC_RELAXED)
		SNAP_LOAD(resident); SNAP_LOAD(live_backings); SNAP_LOAD(reserved);
		SNAP_LOAD(prebudget_peak); SNAP_LOAD(enforced_peak); SNAP_LOAD(cap_waits);
		SNAP_LOAD(request_id); SNAP_LOAD(guest_commits);
		SNAP_LOAD(rollback_id[0]); SNAP_LOAD(rollback_id[1]);
		SNAP_LOAD(budget_phase); SNAP_LOAD(actor_slot); SNAP_LOAD(transfer_state);
		SNAP_LOAD(rollback_reason[0]); SNAP_LOAD(rollback_reason[1]);
		SNAP_LOAD(held); SNAP_LOAD(source_draining); SNAP_LOAD(restoring);
#undef SNAP_LOAD
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if(before == __atomic_load_n(&s_budget_seq, __ATOMIC_ACQUIRE))
			return true;
	}
	memset(out, 0, sizeof(*out));
	out->actor_slot = UINT8_MAX;
	return false;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static enum server_transfer_fault s_fault = SERVER_TRANSFER_FAULT_NONE;
static size_t s_test_carve_free = SIZE_MAX;
static bool s_test_carve_free_set = false;
void server_transfer_test_fault(enum server_transfer_fault f) {
	s_fault = f;
}
static unsigned s_test_refuse_cap_sends = 0;
void server_transfer_test_refuse_capacity_sends(unsigned n) {
	s_test_refuse_cap_sends = n;
}
void server_transfer_test_set_carve_free(size_t bytes) {
	s_test_carve_free = bytes;
	s_test_carve_free_set = bytes != SIZE_MAX;
}
static bool take_fault(enum server_transfer_fault f) {
	if(s_fault != f)
		return false;
	s_fault = SERVER_TRANSFER_FAULT_NONE;
	return true;
}
#else
static bool take_fault(int f) {
	(void)f;
	return false;
}
#define SERVER_TRANSFER_FAULT_ADMISSION 1
#define SERVER_TRANSFER_FAULT_TELEPORTER 2
#define SERVER_TRANSFER_FAULT_COMMIT_MARKER 3
#endif

/* ===== session init ===================================================== */
void server_transfer_init(struct server_local* s) {
	memset(s->transfer, 0, sizeof(s->transfer));
	memset(s->transfer_rollback_id, 0, sizeof(s->transfer_rollback_id));
	memset(s->transfer_rollback_reason, 0, sizeof(s->transfer_rollback_reason));
	s->transfer_next_id = 1;
	s->transfer_epoch = 0;
	for(uint8_t i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++) {
		s->world_slot_state[i] = SERVER_WORLD_SLOT_EMPTY;
		s->world_slot_admitted_by_transfer[i] = false;
		memset(&s->world_service_stats[i], 0, sizeof(s->world_service_stats[i]));
	}
	s->session_sleep_ticks = 0;
	memset(s->transfer_cap_release_q, 0, sizeof(s->transfer_cap_release_q));
	s->transfer_cap_release_n = 0;
	memset(s->transfer_slot_ticket, 0, sizeof(s->transfer_slot_ticket));
	memset(s->a2w_stream_on, 0, sizeof(s->a2w_stream_on));
	memset(s->a2w_batch_owned, 0, sizeof(s->a2w_batch_owned));
	memset(s->a2w_reset_pending, 0, sizeof(s->a2w_reset_pending));
	memset(s->a2w_reset_generation, 0, sizeof(s->a2w_reset_generation));
	s->a2w_host_marker_pending = false;
	s->a2w_host_marker_wait = 0;
	memset(s->a2w_sent, 0, sizeof(s->a2w_sent));
	memset(s->a2w_evicted, 0, sizeof(s->a2w_evicted));
	s->transfer_budget_phase = SERVER_TRANSFER_BUDGET_OFF;
	s->transfer_column_reservation_n = 0;
	s->transfer_column_cap_waits = 0;
	s->transfer_column_budget_wait = false;
	s->transfer_column_peak = 0;
	s->transfer_prebudget_peak = 0;
	s_guest_commits = 0;
	budget_publish(s);
}

/* ===== M2: capacity ticket lifecycle ===================================== *
 * Every ticket the client granted is released exactly once, whatever
 * happens: a rollback, a refused send (retried every step until the
 * transport admits it), an OK that arrives after its transaction closed, a
 * committed reservation whose world is vacated or reserved again. RELEASE
 * is a ticket operation (client_admission.c cap_find): it goes out UNSTAMPED
 * with request id 0, so it is never tied to a dead world instance and its
 * reply is never mistaken for a transaction's. */
static bool send_release_now(uint32_t ticket) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_test_refuse_cap_sends) {
		s_test_refuse_cap_sends--;
		return false;
	}
#endif
	struct client_rpc rpc = {.type = CRPC_CAPACITY_REQUEST};
	rpc.payload.capacity_request.request_id = 0;
	rpc.payload.capacity_request.op = CLIN_CAP_OP_RELEASE;
	rpc.payload.capacity_request.ticket = ticket;
	return clin_try_rpc_send(&rpc);
}

static void drain_releases(struct server_local* s) {
	uint8_t kept = 0;
	for(uint8_t i = 0; i < s->transfer_cap_release_n; i++) {
		uint32_t ticket = s->transfer_cap_release_q[i];
		/* FIFO: once one is refused the rest wait behind it */
		if(kept == 0 && send_release_now(ticket))
			continue;
		s->transfer_cap_release_q[kept++] = ticket;
	}
	s->transfer_cap_release_n = kept;
}

bool server_transfer_release_capacity(struct server_local* s, uint32_t ticket) {
	if(!ticket)
		return true;
	if(!s)
		return false;
	for(uint8_t i = 0; i < s->transfer_cap_release_n; i++)
		if(s->transfer_cap_release_q[i] == ticket)
			return true;
	if(s->transfer_cap_release_n >= SERVER_TRANSFER_CAP_RELEASE_QUEUE)
		return false;
	s->transfer_cap_release_q[s->transfer_cap_release_n++] = ticket;
	drain_releases(s);
	return true;
}

static void queue_release(struct server_local* s, uint32_t ticket) {
	if(!server_transfer_release_capacity(s, ticket))
		fprintf(stderr, "[transfer] capacity release queue FULL: ticket %u "
						"stranded\n", (unsigned)ticket);
}

void server_transfer_on_world_vacated(struct server_local* s, uint8_t slot) {
	if(!s || slot >= SERVER_LOCAL_WORLD_SLOTS || !s->transfer_slot_ticket[slot])
		return;
	queue_release(s, s->transfer_slot_ticket[slot]);
	s->transfer_slot_ticket[slot] = 0;
}

bool server_transfer_flush_owned_batch(struct server_local* s,
									   struct server_world* w) {
	if(!s || !w || w->world_index == 0
	   || w->world_index >= SERVER_LOCAL_WORLD_SLOTS
	   || w != &s->worlds[w->world_index]
	   || !s->a2w_batch_owned[w->world_index])
		return true;
	s->a2w_batch_owned[w->world_index] = false;
	if(server_world_save_batch_end(w))
		return true;
	__atomic_fetch_add(&g_evict_stuck, 1, __ATOMIC_RELAXED);
	if(s->chunk_save_retry_delay == 0)
		s->chunk_save_retry_delay = 20;
	return false;
}

static void rollback(struct server_local* s, struct server_transfer_txn* t,
					 enum server_transfer_rollback_reason reason, const char* why);
static size_t free_slot_entities(struct server_local* s, uint8_t slot,
								 uint32_t generation);
static bool step_client_wait(struct server_local* s, struct server_transfer_txn* t);
static bool step_commit(struct server_local* s, struct server_transfer_txn* t,
						struct server_player* actor);

/* ===== A2-W lifecycle =================================================== */
enum server_world_slot_state server_local_world_slot_state(
	const struct server_local* s, uint8_t slot) {
	if(!s || slot >= SERVER_LOCAL_WORLD_SLOTS)
		return SERVER_WORLD_SLOT_EMPTY;
	if(slot == 0) /* the routed world: its lifecycle is the legacy load path */
		return server_local_world_live(&s->worlds[0]) ? SERVER_WORLD_SLOT_LIVE
													  : SERVER_WORLD_SLOT_EMPTY;
	return s->world_slot_state[slot];
}

static bool admission_budget_ok(const char** why) {
	size_t carve = mem2_carve_free_bytes();
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_test_carve_free_set)
		carve = s_test_carve_free;
#endif
	/* SIZE_MAX = unknown (PC, or the carve was never initialised): the
	 * creation's own allocation checks answer, as before the gate existed. */
	if(carve != SIZE_MAX && carve < SERVER_WORLD_ADMIT_CARVE_HEADROOM) {
		*why = "carve headroom below 1 MiB";
		return false;
	}
	return true;
}

struct server_world* server_local_world_admit(struct server_local* s,
											  enum world_dim dimension) {
	if(!s)
		return NULL;
	struct server_world* live = server_local_world_for_dimension(s, dimension);
	if(live && server_local_world_live(live))
		return live;
	/* slot 0 is the routed world: if it is not live nothing is admitted; and a
	 * second world needs the session save root (its files are siblings of slot
	 * 0 files -- an empty root would write to the filesystem root). */
	if(!server_local_world_live(&s->worlds[0]) || string_empty_p(s->level_name)) {
		fprintf(stderr, "[world_admit] dim %d REFUSED: %s\n", (int)dimension,
				string_empty_p(s->level_name) ? "session has no save root"
											   : "the routed world is not live");
		return NULL;
	}
	uint8_t slot = 0;
	for(uint8_t i = 1; i < SERVER_LOCAL_WORLD_SLOTS && !slot; i++)
		if(s->world_slot_state[i] == SERVER_WORLD_SLOT_EMPTY
		   && !server_world_chunks_ready(&s->worlds[i]))
			slot = i;
	const char* why = NULL;
	if(!slot)
		why = "no free world slot";
	else if(take_fault(SERVER_TRANSFER_FAULT_ADMISSION))
		why = "admission fault injected";
	else if(!admission_budget_ok(&why)) {
		/* admission_budget_ok supplied the refusal reason */
	}
	if(why) {
		fprintf(stderr, "[world_admit] dim %d REFUSED: %s\n", (int)dimension, why);
		server_local_send_hud_message(s, "Cannot open the other dimension now");
		return NULL;
	}
	struct server_world* w = &s->worlds[slot];
	s->a2w_batch_owned[slot] = false; /* a reused slot cannot inherit a window */
	s->world_slot_state[slot] = SERVER_WORLD_SLOT_LOADING;
	ptime_t start = time_get();
	/* The second world shares the session's save root, seed and terrain mode
	 * (the same level.dat rules both dimensions, as in Beta). */
	server_world_create_owned(w, s->level_name, dimension, s->worlds[0].seed,
						s->worlds[0].superflat, s, slot);
	if(!w->generator) {
		fprintf(stderr, "[world_admit] dim %d REFUSED: terrain generator "
						"allocation failed\n", (int)dimension);
		server_world_discard(w);
		s->world_slot_state[slot] = SERVER_WORLD_SLOT_EMPTY;
		server_local_send_hud_message(s, "Cannot open the other dimension now");
		return NULL;
	}
	w->local = s;
	w->session = s;
	w->world_index = slot;
	server_local_entity_sections_reset_slot(s, slot);
	s->world_slot_state[slot] = SERVER_WORLD_SLOT_LIVE;
	fprintf(stderr, "[world_admit] dim %d LIVE in slot %u (generation %u) ms=%d\n",
			(int)dimension, slot, w->generation,
			(int)time_diff_ms(start, time_get()));
	return w;
}

static bool slot_has_member(const struct server_local* s, uint8_t slot) {
	if(s->player.has_pos && s->player.world_slot == slot)
		return true;
	if(s->guest_actor && s->guest_actor->world_slot == slot)
		return true;
	return false;
}

static bool slot_named_by_transaction(const struct server_local* s, uint8_t slot) {
	if((s->guest_respawn_phase != GUEST_RESPAWN_IDLE
			|| s->guest_respawn_cap.cleanup_cancel)
	   && slot < SERVER_LOCAL_WORLD_SLOTS
	   && s->guest_respawn_target.world_index == slot
	   && s->guest_respawn_target.generation
	   && s->worlds[slot].generation == s->guest_respawn_target.generation)
		return true;
	for(size_t i = 0; i < 2; i++) {
		const struct server_transfer_txn* t = &s->transfer[i];
		if((t->open || t->landing_cancel_pending || t->hold_end_pending)
		   && (t->src_slot == slot || t->dst_slot == slot))
			return true;
	}
	return false;
}

/* A2-E F1: free every entity bound to (slot, generation); drop its sections. */
static size_t free_slot_entities(struct server_local* s, uint8_t slot,
								 uint32_t generation) {
	size_t freed = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		uint32_t id = dict_entity_ref(it)->key;
		dict_entity_next(it);
		if(!e || e->world_slot != slot || e->world_generation != generation)
			continue;
		free(e);
		dict_entity_erase(s->entities, id);
		freed++;
	}
	server_local_entity_sections_reset_slot(s, slot);
	return freed;
}

bool server_local_world_retire(struct server_local* s, struct server_world* w) {
	if(!s || !w)
		return false;
	uint8_t slot = w->world_index;
	if(slot == 0 || slot >= SERVER_LOCAL_WORLD_SLOTS || w != &s->worlds[slot])
		return false; /* slot 0 retires through the legacy release path */
	if(s->world_slot_state[slot] != SERVER_WORLD_SLOT_LIVE)
		return false;
	if(slot_has_member(s, slot) || slot_named_by_transaction(s, slot)) {
		fprintf(stderr, "[world_retire] slot %u REFUSED: an actor or a "
						"transaction still holds it\n", slot);
		return false;
	}
	if(!server_transfer_flush_owned_batch(s, w))
		return false;
	s->world_slot_state[slot] = SERVER_WORLD_SLOT_RETIRING;
	uint32_t generation = w->generation;
	/* Fable A2T-SERVER-REVIEW H1/H2: checkpoint FIRST, with the entities still
	 * resident (the chunk record carries them), and only a successful, synced
	 * checkpoint may discard anything. A failed checkpoint RETAINS the world
	 * exactly as it was -- LIVE, resident, every entity -- and reports; the
	 * next retire (vacant-world drain, session release) retries. */
	if(!server_world_checkpoint_chunks(w) || !server_world_sync_regions(w)) {
		s->world_slot_state[slot] = SERVER_WORLD_SLOT_LIVE;
		fprintf(stderr, "[world_retire] slot %u generation %u RETAINED: checkpoint "
						"failed, the world stays LIVE with its entities\n",
				slot, generation);
		return false;
	}
	size_t freed = free_slot_entities(s, slot, generation);
	/* the client releases this instance for good (stamped; a refused send is
	 * harmless: the vacant-world drain already released its columns) */
	(void)server_local_try_send_w(w, (struct client_rpc) {
		.type = CRPC_WORLD_RELEASE, .payload.world_release.retiring = true});
	server_world_discard(w);
	s->world_slot_state[slot] = SERVER_WORLD_SLOT_EMPTY;
	s->world_slot_admitted_by_transfer[slot] = false;
	fprintf(stderr, "[world_retire] slot %u generation %u EMPTY: checkpointed, "
					"%zu entities freed\n",
			slot, generation, freed);
	return true;
}

/* A secondary slot that holds anything to release. */
static bool secondary_slot_held(const struct server_local* s, uint8_t slot) {
	return s->world_slot_state[slot] != SERVER_WORLD_SLOT_EMPTY
		|| server_world_chunks_ready(&s->worlds[slot]);
}

static void discard_secondary_slot(struct server_local* s, uint8_t slot,
								   const char* why) {
	struct server_world* w = &s->worlds[slot];
	uint32_t generation = w->generation;
	if(server_world_chunks_ready(w)) {
		free_slot_entities(s, slot, generation);
		server_world_discard(w);
	}
	s->a2w_batch_owned[slot] = false;
	s->world_slot_state[slot] = SERVER_WORLD_SLOT_EMPTY;
	s->world_slot_admitted_by_transfer[slot] = false;
	server_transfer_on_world_vacated(s, slot); /* M2(d) */
	/* the bodies keep their saved dimension in the records; in memory
	 * they fall back to the routed slot the next load fills */
	if(s->player.world_slot == slot)
		s->player.world_slot = 0;
	if(s->guest_actor && s->guest_actor->world_slot == slot)
		s->guest_actor->world_slot = 0;
	fprintf(stderr, "[world_release] slot %u generation %u EMPTY (%s)\n", slot,
			generation, why);
}

bool server_local_worlds_checkpoint_secondary(struct server_local* s) {
	if(!s)
		return false;
	/* H1: all-or-nothing. Every held world checkpoints and syncs BEFORE any
	 * is discarded; one failure retains them all (LIVE, resident, entities
	 * intact) and the caller must not proceed as if saved. */
	for(uint8_t slot = 1; slot < SERVER_LOCAL_WORLD_SLOTS; slot++) {
		struct server_world* w = &s->worlds[slot];
		if(!secondary_slot_held(s, slot) || !server_world_chunks_ready(w))
			continue;
		if(!server_transfer_flush_owned_batch(s, w))
			return false;
		enum server_world_slot_state was = s->world_slot_state[slot];
		s->world_slot_state[slot] = SERVER_WORLD_SLOT_RETIRING;
		bool saved = server_world_checkpoint_chunks(w) && server_world_sync_regions(w);
		s->world_slot_state[slot] = was;
		if(!saved) {
			fprintf(stderr, "[world_release] slot %u generation %u RETAINED: "
							"checkpoint failed; nothing released\n",
					slot, w->generation);
			return false;
		}
	}
	return true;
}

bool server_local_worlds_release_secondary(struct server_local* s) {
	if(!s)
		return false;
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* t = &s->transfer[i];
		if(t->open)
			rollback(s, t, SERVER_TRANSFER_ROLLBACK_SESSION, "session release");
	}
	if(!server_local_worlds_checkpoint_secondary(s))
		return false;
	for(uint8_t slot = 1; slot < SERVER_LOCAL_WORLD_SLOTS; slot++)
		if(secondary_slot_held(s, slot))
			discard_secondary_slot(s, slot, "session release, checkpointed");
	return true;
}

void server_local_worlds_drop_checkpointed(struct server_local* s) {
	if(!s)
		return;
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* t = &s->transfer[i];
		if(t->open)
			rollback(s, t, SERVER_TRANSFER_ROLLBACK_SESSION, "session release");
	}
	for(uint8_t slot = 1; slot < SERVER_LOCAL_WORLD_SLOTS; slot++)
		if(secondary_slot_held(s, slot))
			discard_secondary_slot(s, slot, "session release, checkpointed");
	server_transfer_on_world_vacated(s, 0); /* M2(d): the session ends */
}

void server_local_worlds_discard_secondary(struct server_local* s) {
	if(!s)
		return;
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* t = &s->transfer[i];
		if(t->open)
			rollback(s, t, SERVER_TRANSFER_ROLLBACK_SESSION, "session discard");
	}
	for(uint8_t slot = 1; slot < SERVER_LOCAL_WORLD_SLOTS; slot++)
		if(secondary_slot_held(s, slot))
			discard_secondary_slot(s, slot, "DISCARDED without checkpoint");
	server_transfer_on_world_vacated(s, 0); /* M2(d): the session ends */
}

void server_local_world_service_stats(const struct server_local* s, uint8_t slot,
									  struct server_world_service_stats* out) {
	if(!out)
		return;
	if(!s || slot >= SERVER_LOCAL_WORLD_SLOTS) {
		memset(out, 0, sizeof(*out));
		return;
	}
	*out = s->world_service_stats[slot];
}

uint64_t server_local_session_sleep_ticks(const struct server_local* s) {
	return s ? s->session_sleep_ticks : 0;
}

/* ===== the transaction ================================================== */
static struct server_transfer_txn* txn_of_actor(struct server_local* s,
												const struct server_player* actor) {
	if(actor == &s->player)
		return &s->transfer[ACTOR_SLOT_HOST];
	if(s->guest_actor && actor == s->guest_actor)
		return &s->transfer[ACTOR_SLOT_GUEST];
	return NULL;
}

static struct server_player* txn_actor(struct server_local* s,
									   const struct server_transfer_txn* t) {
	if(t->actor_slot == ACTOR_SLOT_HOST)
		return &s->player;
	if(s->guest_actor && s->guest_generation == t->actor_generation)
		return s->guest_actor;
	return NULL;
}

bool server_transfer_active(const struct server_local* s) {
	if(!s)
		return false;
	return s->transfer[0].open || s->transfer[1].open;
}

bool server_transfer_actor_frozen(const struct server_local* s,
									 const struct server_player* actor) {
	if(!s || !actor)
		return false;
	if(actor == &s->player && s->a2w_host_presentation)
		return true;
	for(size_t i = 0; i < 2; i++) {
		const struct server_transfer_txn* t = &s->transfer[i];
		if(((t->open && t->hold_sent) || t->hold_end_pending)
		   && ((t->actor_slot == ACTOR_SLOT_HOST && actor == &s->player)
			   || (t->actor_slot == ACTOR_SLOT_GUEST
				   && actor == s->guest_actor
				   && t->actor_generation == s->guest_generation)))
			return true;
	}
	return false;
}

bool server_transfer_actor_inert(const struct server_local* s,
	const struct server_player* actor) {
	return server_transfer_actor_frozen(s, actor)
		|| (s && actor && actor == s->guest_actor && s->guest_gameplay
			&& s->guest_owned && s->guest_confirmed
			&& s->a2w_guest_presentation);
}

bool server_transfer_guest_continuity(const struct server_local* s) {
	return s && s->a2w_host_presentation && s->guest_gameplay
		&& s->guest_actor && s->guest_owned && s->guest_confirmed;
}

static bool send_transfer_hold(const struct server_transfer_txn* t, bool begin) {
	struct client_rpc rpc = {.type = CRPC_TRANSFER_HOLD};
	rpc.payload.transfer_hold.request_id = t->id;
	rpc.payload.transfer_hold.actor_generation = t->actor_generation;
	rpc.payload.transfer_hold.actor_slot = t->actor_slot;
	rpc.payload.transfer_hold.begin = begin;
	rpc.payload.transfer_hold.restored_source = !begin && t->hold_end_restored;
	rpc.payload.transfer_hold.restore_radius
		= rpc.payload.transfer_hold.restored_source ? t->restore_radius : 0;
	return clin_try_rpc_send(&rpc);
}

bool server_transfer_world_in_flight(const struct server_local* s,
									 const struct server_world* w) {
	if(!s || !w || w->world_index >= SERVER_LOCAL_WORLD_SLOTS
	   || w != &s->worlds[w->world_index])
		return false;
	return slot_named_by_transaction(s, w->world_index);
}

enum server_transfer_state server_transfer_state_of(const struct server_local* s,
													uint32_t request_id) {
	if(!s || !request_id)
		return SERVER_TRANSFER_IDLE;
	for(size_t i = 0; i < 2; i++) {
		const struct server_transfer_txn* t = &s->transfer[i];
		if(t->open && t->id == request_id)
			return t->state;
		if(!t->open && t->last_id == request_id)
			return t->last_state;
	}
	return SERVER_TRANSFER_IDLE;
}

const struct server_actor_transition*
server_transfer_outcome(const struct server_local* s, uint32_t request_id) {
	if(!s || !request_id)
		return NULL;
	for(size_t i = 0; i < 2; i++) {
		const struct server_transfer_txn* t = &s->transfer[i];
		if(!t->open && t->outcome_valid && t->outcome.request_id == request_id)
			return &t->outcome;
	}
	return NULL;
}

enum server_transfer_rc server_transfer_request(struct server_local* s,
												struct server_player* actor,
												enum world_dim destination,
												uint32_t* request_id) {
	if(!s || !actor || !request_id)
		return SERVER_TRANSFER_REFUSED_MEMBERSHIP;
	*request_id = 0;
	struct server_transfer_txn* t = txn_of_actor(s, actor);
	struct server_world* src = server_local_world_of(s, actor);
	if(!t || !src)
		return SERVER_TRANSFER_REFUSED_MEMBERSHIP;
	if(t->open) {
		*request_id = t->id; /* duplicate requests return the existing outcome */
		return SERVER_TRANSFER_DUPLICATE;
	}
	if((actor == &s->player && s->a2w_host_presentation)
	   || (actor == s->guest_actor && s->a2w_guest_presentation))
		return SERVER_TRANSFER_REFUSED_BUSY;
	if(t->hold_end_pending || t->landing_cancel_pending)
		return SERVER_TRANSFER_REFUSED_BUSY;
	if(src->dimension == destination)
		return SERVER_TRANSFER_REFUSED_SAME_WORLD;
	if(s->guest_respawn_phase != GUEST_RESPAWN_IDLE
	   || s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE || s->portal_job
	   || s->portal_origin.valid || s->death_world_evict)
		return SERVER_TRANSFER_REFUSED_BUSY;
	bool was_live = server_local_world_for_dimension(s, destination) != NULL
		&& server_local_world_live(server_local_world_for_dimension(s, destination));
	struct server_world* dst = server_local_world_admit(s, destination);
	if(!dst)
		return SERVER_TRANSFER_REFUSED_ADMISSION;
	memset(t, 0, sizeof(*t));
	t->open = true;
	t->id = s->transfer_next_id++;
	/* Upper bits belong to guest Join and respawn capacity protocols. */
	if(!s->transfer_next_id || s->transfer_next_id >= GUEST_RESPAWN_CAP_ID_BIT)
		s->transfer_next_id = 1;
	t->actor_slot = actor == &s->player ? ACTOR_SLOT_HOST : ACTOR_SLOT_GUEST;
	t->actor_generation = actor == &s->player ? 0 : s->guest_generation;
	t->src_slot = src->world_index;
	t->dst_slot = dst->world_index;
	t->dst_dim = destination;
	t->admitted_dst = !was_live;
	if(t->admitted_dst)
		s->world_slot_admitted_by_transfer[t->dst_slot] = true;
	t->state = SERVER_TRANSFER_REQUESTED;
	/* the staged destination pose: Beta's 8:1 coordinate scale */
	t->pose.x = actor->x;
	t->pose.y = actor->y;
	t->pose.z = actor->z;
	t->pose.rx = actor->rx;
	t->pose.ry = actor->ry;
	if(destination == WORLD_DIM_NETHER) {
		t->pose.x /= 8.0;
		t->pose.z /= 8.0;
	} else {
		t->pose.x *= 8.0;
		t->pose.z *= 8.0;
	}
	/* M2(d): a slot reserved again first gives back its previous committed
	 * reservation (the client refuses a second one for the slot as BUSY);
	 * the RESERVE for this request waits behind that RELEASE. */
	server_transfer_on_world_vacated(s, t->dst_slot);
	*request_id = t->id;
	server_local_portal_diag_publish(s);
	fprintf(stderr, "[transfer] request %u: actor %u slot %u -> slot %u (dim %d)\n",
			t->id, t->actor_slot, t->src_slot, t->dst_slot, (int)destination);
	/* host_vehicle_smoke_8m transfer_begin: after SERVER_TRANSFER_OK. */
	if(vsp_armed())
		vsp_srv_transfer_requested(s, actor, t->id,
								   vsp_srv_transfer_fixture_active());
	return SERVER_TRANSFER_OK;
}

static void publish_outcome(struct server_local* s, struct server_transfer_txn* t,
							struct server_player* actor,
							enum server_transition_phase phase) {
	struct server_actor_transition* o = &t->outcome;
	memset(o, 0, sizeof(*o));
	o->request_id = t->id;
	o->actor.slot = t->actor_slot;
	o->actor.generation = t->actor_generation;
	o->source = server_world_handle_of(&s->worlds[t->src_slot]);
	o->destination = server_world_handle_of(&s->worlds[t->dst_slot]);
	o->destination_dimension = t->dst_dim;
	o->phase = phase;
	if(phase == SERVER_TRANSITION_COMMIT) {
		o->x = t->pose.x;
		o->y = t->pose.y;
		o->z = t->pose.z;
		o->rx = t->pose.rx;
		o->ry = t->pose.ry;
		o->transition_epoch = ++s->transfer_epoch;
	} else if(actor) {
		o->x = actor->x;
		o->y = actor->y;
		o->z = actor->z;
		o->rx = actor->rx;
		o->ry = actor->ry;
	}
	t->outcome_valid = true;
	if(t->actor_slot == ACTOR_SLOT_GUEST)
		(void)server_local_guest_on_travel_transaction(s, o);
}

/* CRPC_CAPACITY_REQUEST for this transaction, stamped with the destination.
 * False when the transport refused (the step retries next tick). */
static bool send_capacity(struct server_local* s, struct server_transfer_txn* t,
						  uint8_t op) {
	struct server_world* dst = &s->worlds[t->dst_slot];
	/* In bounded world-payload mode the client reserves the effective ring
	 * that the server will actually stream/restore. Legacy single-payload
	 * admission keeps its historical raw-radius request byte-for-byte. */
	int r = clin_world_payloads_enabled() ? server_local_view_radius(s)
		: (s->split_view_radius > 0 ? s->split_view_radius : 1);
	uint32_t columns = (uint32_t)(2 * r + 1) * (uint32_t)(2 * r + 1);
	uint32_t mem1 = columns * SERVER_TRANSFER_MEM1_PER_COLUMN;
	if(clin_world_payloads_enabled()
	   && !world_local_shared_capacity_mem1_need(columns, &mem1))
		return false;
	struct client_rpc rpc = {.type = CRPC_CAPACITY_REQUEST};
	rpc.payload.capacity_request.request_id = t->id;
	rpc.payload.capacity_request.op = op;
	rpc.payload.capacity_request.slot = t->dst_slot;
	rpc.payload.capacity_request.mem1_bytes = mem1;
	rpc.payload.capacity_request.mem2_bytes = columns * SERVER_TRANSFER_MEM2_PER_COLUMN;
	rpc.payload.capacity_request.ticket = t->capacity_ticket;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_test_refuse_cap_sends) {
		s_test_refuse_cap_sends--;
		return false; /* the transport refused (a full client inbox) */
	}
#endif
	return server_local_try_send_w(dst, rpc);
}

static bool send_landing(struct server_local* s, struct server_transfer_txn* t,
						 bool cancel) {
	struct server_world* dst = &s->worlds[t->dst_slot];
	struct client_rpc rpc = {.type = CRPC_LANDING_ARM};
	rpc.payload.landing_arm.token = t->id;
	rpc.payload.landing_arm.x = (int32_t)floor(t->pose.x);
	rpc.payload.landing_arm.z = (int32_t)floor(t->pose.z);
	/* blocker 3: the landing is the arrival core this transaction streams */
	rpc.payload.landing_arm.radius = SERVER_TRANSFER_LANDING_CORE_RADIUS;
	rpc.payload.landing_arm.cancel = cancel;
	rpc.payload.landing_arm.dimension = (int8_t)dst->dimension;
	return server_local_try_send_w(dst, rpc);
}

size_t server_transfer_resident_columns(const struct server_local* s) {
	size_t n = 0;
	for(size_t i = 0; s && i < SERVER_LOCAL_WORLD_SLOTS; i++)
		if(server_local_world_live(&s->worlds[i]))
			n += dict_server_chunks_size(s->worlds[i].chunks);
	return n;
}

static bool column_budget_active(const struct server_local* s) {
	return s && s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED;
}

static void budget_publish(struct server_local* s) {
	if(!s)
		return;
	struct server_transfer_budget_snapshot v = {0};
	v.resident = (uint32_t)server_transfer_resident_columns(s);
	uint32_t shared = 0;
	column_backing_get_stats(&v.live_backings, &shared);
	v.reserved = s->transfer_column_reservation_n;
	v.prebudget_peak = s->transfer_prebudget_peak;
	v.enforced_peak = s->transfer_column_peak;
	v.cap_waits = s->transfer_column_cap_waits;
	v.guest_commits = s_guest_commits;
	for(unsigned i = 0; i < 2; i++) {
		v.rollback_id[i] = s->transfer_rollback_id[i];
		v.rollback_reason[i] = s->transfer_rollback_reason[i];
	}
	v.budget_phase = (uint8_t)s->transfer_budget_phase;
	v.actor_slot = UINT8_MAX;
	const struct server_transfer_txn* active = NULL;
	for(size_t i = 0; i < 2; i++) {
		const struct server_transfer_txn* t = &s->transfer[i];
		if((t->open || t->hold_end_pending)
		   && (!active || (t->hold_sent && !active->hold_sent)
					   || (t->state != SERVER_TRANSFER_REQUESTED
						   && active->state == SERVER_TRANSFER_REQUESTED)))
			active = t;
	}
	if(active) {
		const struct server_transfer_txn* t = active;
		v.request_id = t->id;
		v.actor_slot = t->actor_slot;
		v.transfer_state = (uint8_t)t->state;
		v.held = t->hold_sent || t->hold_end_pending;
		v.source_draining = t->state == SERVER_TRANSFER_SOURCE_DRAINING;
		v.restoring = t->state == SERVER_TRANSFER_RESTORING;
	}
	__atomic_add_fetch(&s_budget_seq, 1, __ATOMIC_ACQ_REL);
#define SNAP_STORE(field) __atomic_store_n(&s_budget_snapshot.field, v.field, __ATOMIC_RELAXED)
	SNAP_STORE(resident); SNAP_STORE(live_backings); SNAP_STORE(reserved);
	SNAP_STORE(prebudget_peak); SNAP_STORE(enforced_peak); SNAP_STORE(cap_waits);
	SNAP_STORE(request_id); SNAP_STORE(guest_commits);
	SNAP_STORE(rollback_id[0]); SNAP_STORE(rollback_id[1]);
	SNAP_STORE(budget_phase); SNAP_STORE(actor_slot); SNAP_STORE(transfer_state);
	SNAP_STORE(rollback_reason[0]); SNAP_STORE(rollback_reason[1]);
	SNAP_STORE(held); SNAP_STORE(source_draining); SNAP_STORE(restoring);
#undef SNAP_STORE
	__atomic_add_fetch(&s_budget_seq, 1, __ATOMIC_RELEASE);
}

void server_transfer_budget_tick(struct server_local* s) {
	if(!s)
		return;
	bool wanted = clin_world_payloads_enabled()
		&& (s->split_enabled || s->guest_gameplay
			|| s->player.world_slot != 0 || server_transfer_active(s));
	if(!wanted) {
		s->transfer_budget_phase = SERVER_TRANSFER_BUDGET_OFF;
		budget_publish(s);
		return;
	}
	if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_OFF)
		s->transfer_budget_phase = SERVER_TRANSFER_BUDGET_DRAINING;
	uint32_t live = 0, shared = 0;
	column_backing_get_stats(&live, &shared);
	if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_DRAINING) {
		size_t occupied = (size_t)live + s->transfer_column_reservation_n;
		if(occupied > s->transfer_prebudget_peak)
			s->transfer_prebudget_peak = (uint32_t)occupied;
		if((size_t)live + s->transfer_column_reservation_n
		   <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP) {
			s->transfer_budget_phase = SERVER_TRANSFER_BUDGET_ENFORCED;
			if(occupied > s->transfer_column_peak)
				s->transfer_column_peak = (uint32_t)occupied;
		}
	}
	budget_publish(s);
}

static int column_reservation_find(const struct server_local* s,
										const struct server_world* w,
										w_coord_t x, w_coord_t z) {
	for(unsigned i = 0; i < s->transfer_column_reservation_n; i++) {
		const struct server_transfer_column_reservation* r
			= &s->transfer_column_reservations[i];
		if(r->slot == w->world_index && r->generation == w->generation
		   && r->x == x && r->z == z)
			return (int)i;
	}
	return -1;
}

static void column_reservation_remove(struct server_local* s, unsigned i) {
	assert(i < s->transfer_column_reservation_n);
	s->transfer_column_reservations[i]
		= s->transfer_column_reservations[--s->transfer_column_reservation_n];
	budget_publish(s);
}

static unsigned s_prepare_max_ticks = SERVER_TRANSFER_PREPARE_MAX_TICKS;
static uint32_t s_prepare_timeouts;
static uint32_t s_cap_excursions, s_cap_excursion_peak;
uint32_t server_transfer_prepare_timeouts(void) { return s_prepare_timeouts; }
void server_transfer_cap_excursions(uint32_t* count, uint32_t* peak) {
	if(count)
		*count = s_cap_excursions;
	if(peak)
		*peak = s_cap_excursion_peak;
}
#ifdef CAVEWII_SERVER_LOCAL_TEST
void server_transfer_test_set_prepare_max_ticks(unsigned ticks) {
	s_prepare_max_ticks = ticks ? ticks : SERVER_TRANSFER_PREPARE_MAX_TICKS;
}
#endif

static size_t column_budget_occupied(const struct server_local* s) {
	uint32_t live = 0, shared = 0;
	column_backing_get_stats(&live, &shared);
	(void)shared;
	return (size_t)live + s->transfer_column_reservation_n;
}

static bool column_reserve(struct server_world* w, w_coord_t x, w_coord_t z) {
	struct server_local* s = w->session;
	if(!column_budget_active(s) || server_world_chunk_resident(w, x, z)
	   || column_reservation_find(s, w, x, z) >= 0)
		return true;
	if(s->transfer_column_reservation_n >= SERVER_TRANSFER_COLUMN_RESERVATIONS
	   || column_budget_occupied(s) >= SERVER_TRANSFER_RESIDENT_COLUMN_CAP) {
		s->transfer_column_cap_waits++;
		s->transfer_column_budget_wait = true;
		budget_publish(s);
		return false;
	}
	s->transfer_column_reservations[s->transfer_column_reservation_n++]
		= (struct server_transfer_column_reservation) {
			.slot = w->world_index, .generation = w->generation, .x = x, .z = z};
	size_t occupied = column_budget_occupied(s);
	if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED) {
		if(occupied > s->transfer_column_peak)
			s->transfer_column_peak = (uint32_t)occupied;
	} else if(occupied > s->transfer_prebudget_peak)
		s->transfer_prebudget_peak = (uint32_t)occupied;
	budget_publish(s);
	return true;
}

bool server_transfer_column_preflight(struct server_world* w, w_coord_t x,
									w_coord_t z) {
	if(!w || !w->session)
		return true;
	server_transfer_budget_tick(w->session);
	if(w->session->transfer_budget_phase == SERVER_TRANSFER_BUDGET_DRAINING) {
		w->session->transfer_column_cap_waits++;
		w->session->transfer_column_budget_wait = true;
		budget_publish(w->session);
		return false;
	}
	if(!column_budget_active(w->session))
		return true;
	/* Every covering populate quadrant uses one of the target's 3x3 columns.
	 * Reserve all missing keys before the target or Nether's world-global RNG
	 * can move. The common load gate below still checks every other caller. */
	for(int dz = w->generator ? -1 : 0; dz <= (w->generator ? 1 : 0); dz++)
		for(int dx = w->generator ? -1 : 0; dx <= (w->generator ? 1 : 0); dx++)
			if(!column_reserve(w, x + dx, z + dz)) {
				server_transfer_column_preflight_finish(w);
				return false;
			}
	return true;
}

static void cap_sample(struct server_local* s);
void server_transfer_column_preflight_finish(struct server_world* w) {
	if(!w || !w->session)
		return;
	struct server_local* s = w->session;
	cap_sample(s); /* the population's inserts are live before release */
	for(unsigned i = 0; i < s->transfer_column_reservation_n;)
		if(s->transfer_column_reservations[i].slot == w->world_index
		   && s->transfer_column_reservations[i].generation == w->generation)
			column_reservation_remove(s, i);
		else
			i++;
}

bool server_transfer_column_load_begin(struct server_world* w, w_coord_t x,
									w_coord_t z, bool* ephemeral) {
	assert(w && ephemeral);
	*ephemeral = false;
	if(w->session)
		server_transfer_budget_tick(w->session);
	if(w->session
	   && w->session->transfer_budget_phase == SERVER_TRANSFER_BUDGET_DRAINING
	   && !server_world_chunk_resident(w, x, z)) {
		w->session->transfer_column_cap_waits++;
		w->session->transfer_column_budget_wait = true;
		budget_publish(w->session);
		return false;
	}
	if(!column_budget_active(w->session) || server_world_chunk_resident(w, x, z))
		return true;
	if(column_reservation_find(w->session, w, x, z) >= 0)
		return true;
	if(!column_reserve(w, x, z))
		return false;
	*ephemeral = true;
	return true;
}

/* Sol review 2 (C): occupancy sampled at the allocation points (every column
 * load and every population preflight), not only between ticks, so an
 * intra-tick excursion over the ceiling is counted where it happens. */
static void cap_sample(struct server_local* s) {
	if(!s || s->transfer_budget_phase != SERVER_TRANSFER_BUDGET_ENFORCED)
		return;
	uint32_t live = 0, shared = 0;
	column_backing_get_stats(&live, &shared);
	const uint32_t occ = live + s->transfer_column_reservation_n;
	if(occ > SERVER_TRANSFER_RESIDENT_COLUMN_CAP) {
		s_cap_excursions++;
		if(occ > s_cap_excursion_peak)
			s_cap_excursion_peak = occ;
	}
}

void server_transfer_column_load_end(struct server_world* w, w_coord_t x,
									  w_coord_t z, bool ephemeral) {
	if(w)
		cap_sample(w->session);
	if(!ephemeral || !w || !w->session)
		return;
	int i = column_reservation_find(w->session, w, x, z);
	if(i >= 0)
		column_reservation_remove(w->session, (unsigned)i);
}

void server_transfer_column_inserted(struct server_world* w, w_coord_t x,
									w_coord_t z) {
	if(!w || !w->session)
		return;
	struct server_local* s = w->session;
	int i = column_reservation_find(s, w, x, z);
	if(i >= 0)
		column_reservation_remove(s, (unsigned)i);
	size_t occupied = column_budget_occupied(s);
	if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED) {
		if(occupied > s->transfer_column_peak)
			s->transfer_column_peak = (uint32_t)occupied;
	} else if(occupied > s->transfer_prebudget_peak)
		s->transfer_prebudget_peak = (uint32_t)occupied;
	budget_publish(s);
}

void server_transfer_column_wait_reset(struct server_world* w) {
	if(w && w->session)
		w->session->transfer_column_budget_wait = false;
}

bool server_transfer_column_waiting(const struct server_world* w) {
	return w && w->session && w->session->transfer_column_budget_wait;
}

static w_coord_t landing_floor_div16(w_coord_t v) {
	return v >= 0 ? v / CHUNK_SIZE : -((-v + CHUNK_SIZE - 1) / CHUNK_SIZE);
}

/* Blocker 3: one bounded slice of the arrival-core stream. Each core column
 * is made resident (populated), snapshotted into a pooled chunk slot and sent
 * as a CRPC_CHUNK stamped with the destination world -- the near stream's
 * send, addressed to that world, minus everything that could block: the pool
 * acquire and the send are the non-blocking forms, and a refusal simply
 * retries next step. The world's own `sent` bookkeeping is not touched: this
 * is an extra, idempotent delivery (a column the destination's own stream
 * sends again replaces the same data on the client). Sign text is not sent
 * (the session sign store is slot 0's; a secondary world's signs arrive with
 * its own stream). */
static void landing_stream_step(struct server_local* s,
								struct server_transfer_txn* t) {
	struct server_world* dst = &s->worlds[t->dst_slot];
	const w_coord_t lcx = landing_floor_div16((w_coord_t)floor(t->pose.x));
	const w_coord_t lcz = landing_floor_div16((w_coord_t)floor(t->pose.z));
	const int r = SERVER_TRANSFER_LANDING_CORE_RADIUS;
	unsigned sent_now = 0;
	for(int dz = -r; dz <= r && sent_now < SERVER_TRANSFER_LANDING_SENDS_PER_TICK;
		dz++)
		for(int dx = -r;
			dx <= r && sent_now < SERVER_TRANSFER_LANDING_SENDS_PER_TICK; dx++) {
			const unsigned bit = (unsigned)((dz + r) * (2 * r + 1) + (dx + r));
			if(t->landing_sent_mask & (1u << bit))
				continue;
			const w_coord_t cx = lcx + dx, cz = lcz + dz;
			if(!server_world_chunk_resident(dst, cx, cz)
			   && server_transfer_resident_columns(s)
				   >= SERVER_TRANSFER_RESIDENT_COLUMN_CAP)
				continue; /* over the transfer cap: wait for the drain */
			struct server_chunk* sc = NULL;
			/* G9: a column with pending light is never snapshotted. */
			if(server_world_light_pinned(dst, cx, cz)) {
				server_world_light_note_send_deferred();
				continue; /* next step, after the drain */
			}
			if(!server_world_ensure_populated(dst, s, cx, cz, &sc) || !sc)
				continue; /* generation deferred: next step */
			struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
			if(!slot)
				return; /* the client is behind: next step */
			clin_chunk_slot_copy_local(slot, sc);
			if(!piston_moving_server_snapshot_w(s, dst, cx, cz,
												&slot->piston_moving)) {
				clin_chunk_slot_release(slot);
				return;
			}
			if(dst->generator)
				worldgen_climate(dst->generator, cx, cz, slot->grass_i,
								 slot->grass_j);
			else {
				memset(slot->grass_i, 0, CHUNK_SIZE * CHUNK_SIZE);
				memset(slot->grass_j, 0, CHUNK_SIZE * CHUNK_SIZE);
			}
			if(!server_local_try_send_w(dst, (struct client_rpc) {
				   .type = CRPC_CHUNK,
				   .payload.chunk.x = cx * CHUNK_SIZE,
				   .payload.chunk.y = 0,
				   .payload.chunk.z = cz * CHUNK_SIZE,
				   .payload.chunk.sx = CHUNK_SIZE,
				   .payload.chunk.sy = WORLD_HEIGHT,
				   .payload.chunk.sz = CHUNK_SIZE,
				   .payload.chunk.slot = slot,
			   })) {
				clin_chunk_slot_release(slot);
				return; /* the inbox is full: next step */
			}
			t->landing_sent_mask |= (uint16_t)(1u << bit);
			t->landing_columns_sent++;
			sent_now++;
		}
}

/* ===== A2-W: per-world streaming (slots >= 1) ============================ */
/* Lane B's gate 1: an in-place guest WORLD_RESET binding the guest's view to
 * world `w` (guest_view 1); player 0's world, body and windows untouched. */
static void a2w_guest_gate1(struct server_world* w) {
	clin_rpc_send(&(struct client_rpc) {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset.dimension = w->dimension,
		.payload.world_reset.world_index = w->world_index,
		.payload.world_reset.world_generation = w->generation,
		.payload.world_reset.guest_world = true,
		.payload.world_reset.guest_view = 1,
		/* O07 / R04 D2-1: a guest move is a new binding */
		.payload.world_reset.guest_bind_seq = server_local_guest_bind_seq_bump(),
	});
}

void server_transfer_stream_enable(struct server_local* s, uint8_t slot) {
	if(!s || slot == 0 || slot >= SERVER_LOCAL_WORLD_SLOTS)
		return;
	if(!s->a2w_stream_on[slot]) {
		struct server_world* w = &s->worlds[slot];
		/* A new gate-1 view has no old client claims, even if this same live
		 * world retained its server columns while a prior guest was absent. */
		dict_server_chunks_it_t chunks;
		for(dict_server_chunks_it(chunks, w->chunks);
			!dict_server_chunks_end_p(chunks); dict_server_chunks_next(chunks)) {
			struct server_chunk* sc = &dict_server_chunks_ref(chunks)->value;
			sc->sent = false;
			sc->a2w_pub_stage = 0;
			sc->a2w_sign_cursor = 0;
			sc->a2w_entity_after_id = 0;
		}
		dict_entity_it_t entities;
		for(dict_entity_it(entities, s->entities);
			!dict_entity_end_p(entities); dict_entity_next(entities)) {
			struct entity* e = dict_entity_ref(entities)->value;
			if(e && server_local_entity_world(s, e) == w)
				e->client_pub_generation = 0;
		}
	}
	s->a2w_stream_on[slot] = true;
}

static int a2w_cheb(w_coord_t ax, w_coord_t az, w_coord_t bx, w_coord_t bz) {
	int dx = ax > bx ? ax - bx : bx - ax;
	int dz = az > bz ? az - bz : bz - az;
	return dx > dz ? dx : dz;
}

static bool a2w_entity_published(const struct entity* e,
	const struct server_world* w) {
	return e->client_pub_slot == w->world_index
		&& e->client_pub_generation == w->generation;
}

bool server_transfer_source_stream_allowed(const struct server_world* w,
										w_coord_t cx, w_coord_t cz) {
	if(!w || !w->session)
		return true;
	struct server_local* s = w->session;
	/* RESTORING owns a partially published column. The ordinary slot-0 near
	 * pass must not enqueue a second CHUNK ahead of its sign/entity cursor. */
	struct server_chunk* publishing = dict_server_chunks_get(
		w->chunks, S_CHUNK_ID(cx, cz));
	if(publishing && publishing->a2w_pub_stage != 0 && !publishing->sent)
		return false;
	for(size_t i = 0; i < 2; i++) {
		const struct server_transfer_txn* t = &s->transfer[i];
		if(!t->open || !t->hold_sent || t->src_slot != w->world_index
		   || t->state < SERVER_TRANSFER_SOURCE_DRAINING
		   || t->state == SERVER_TRANSFER_RESTORING)
			continue;
		if(a2w_cheb(cx, cz, t->source_cx, t->source_cz) <= 1)
			return true;
		struct server_player* partner = t->actor_slot == ACTOR_SLOT_HOST
			? s->guest_actor : &s->player;
		if(partner && server_local_world_of(s, partner) == w
		   && a2w_cheb(cx, cz,
				landing_floor_div16((w_coord_t)floor(partner->x)),
				landing_floor_div16((w_coord_t)floor(partner->z)))
				<= server_local_view_radius(s))
			return true;
		return false;
	}
	return true;
}

/* residency-1005/6 R1 (ASTRA-RESIDENCY-FINAL-2): the wanted radius of one
 * stream actor. A guest CORPSE whose respawn plan is live
 * (server_local_guest_respawn_target: PREPARED with capacity reserved and
 * landing armed, or LOADING) no longer needs its view ring -- its
 * destination is represented by the respawn anchor (slot 0: residency anchor
 * 1; slot >= 1: the respawn_anchor stand-in in server_transfer_stream_step).
 * It keeps only its 3x3 collision core, exactly what an A2-T mover keeps
 * while HELD (source_drain_step). Root cause of the reported deadlock: the
 * dead body kept its full radius-4 Nether ring (81 columns) for the whole
 * plan, so host ring (81) + corpse ring (81) + the far landing could not fit
 * SERVER_TRANSFER_RESIDENT_COLUMN_CAP (then 176, sized 2 x 81 + margin on
 * the transfer's premise that the mover's source ring drains): measured 175/176,
 * the landing never reported and every PREPARED preflight timed out. When the
 * plan ends (commit, LOADING_FAILED, RETRY) the corpse is an ordinary actor
 * again and its ring re-streams. Single player never has a guest. */
static int a2w_actor_radius(struct server_local* s,
							const struct server_player* actor, int r) {
	if(actor && actor == s->guest_actor && actor->health <= 0
	   && server_local_guest_respawn_target(s))
		return SERVER_TRANSFER_LANDING_CORE_RADIUS;
	return r;
}

/* Some actor's wanted square (a2w_actor_radius) covers (cx, cz). */
static bool a2w_wanted(struct server_local* s,
					   struct server_player* const* actors, size_t n,
					   w_coord_t cx, w_coord_t cz, int r) {
	for(size_t i = 0; i < n; i++)
		if(a2w_cheb(cx, cz, landing_floor_div16((w_coord_t)floor(actors[i]->x)),
					landing_floor_div16((w_coord_t)floor(actors[i]->z)))
		   <= a2w_actor_radius(s, actors[i], r))
			return true;
	return false;
}

/* IMP-TE-CAPS: the resumable sign cursor must address every sign row the
 * growable table can hold, or back-pressure replays a wrapped prefix. */
_Static_assert(sizeof(((struct server_chunk*)0)->a2w_sign_cursor) >= 4
	&& TE_STORE_ROWS_LIMIT <= UINT32_MAX - 1,
	"a2w_sign_cursor must hold any sign row index");

static bool a2w_send_column(struct server_local* s, struct server_world* w,
							struct server_chunk* sc, w_coord_t cx, w_coord_t cz) {
	/* G9: a column with pending light is never snapshotted (next step). */
	if(sc->a2w_pub_stage == 0 && server_world_light_pinned(w, cx, cz)) {
		server_world_light_note_send_deferred();
		return false;
	}
	if(sc->a2w_pub_stage == 0) {
		struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
		if(!slot)
			return false;
		clin_chunk_slot_copy_local(slot, sc);
		if(!piston_moving_server_snapshot_w(s, w, cx, cz,
										&slot->piston_moving)) {
			clin_chunk_slot_release(slot);
			return false;
		}
		if(w->generator)
			worldgen_climate(w->generator, cx, cz, slot->grass_i, slot->grass_j);
		else {
			memset(slot->grass_i, 0, CHUNK_SIZE * CHUNK_SIZE);
			memset(slot->grass_j, 0, CHUNK_SIZE * CHUNK_SIZE);
		}
		if(!server_local_try_send_w(w, (struct client_rpc) {
			   .type = CRPC_CHUNK,
			   .payload.chunk.x = cx * CHUNK_SIZE,
			   .payload.chunk.y = 0,
			   .payload.chunk.z = cz * CHUNK_SIZE,
			   .payload.chunk.sx = CHUNK_SIZE,
			   .payload.chunk.sy = WORLD_HEIGHT,
			   .payload.chunk.sz = CHUNK_SIZE,
			   .payload.chunk.slot = slot,
		   })) {
			clin_chunk_slot_release(slot);
			return false;
		}
		sc->a2w_pub_stage = 1;
	}
	if(sc->a2w_pub_stage == 1) {
		struct te_store signs = server_world_tile_store(w);
		for(unsigned k = sc->a2w_sign_cursor;
			 k < (unsigned)signs.max_sign; k++) { /* IMP-TE-CAPS: growable */
			if(te_store_row_owned(&signs, TE_KIND_SIGN, k)
			   && WCOORD_CHUNK_OFFSET(s->sign_pos[k].x) == cx
			   && WCOORD_CHUNK_OFFSET(s->sign_pos[k].z) == cz) {
				struct client_rpc rpc = {
					.type = CRPC_SIGN_TEXT,
					.payload.sign_text.x = s->sign_pos[k].x,
					.payload.sign_text.y = s->sign_pos[k].y,
					.payload.sign_text.z = s->sign_pos[k].z,
				};
				memcpy(rpc.payload.sign_text.grid, s->sign_texts[k], SIGN_SIZE);
				if(!server_local_try_send_w(w, rpc))
					return false;
			}
			sc->a2w_sign_cursor = k + 1;
		}
		sc->a2w_pub_stage = 2;
	}
	if(sc->a2w_pub_stage == 2) {
		for(;;) {
			struct entity* next = NULL;
			dict_entity_it_t it;
			for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
				dict_entity_next(it)) {
				struct entity* e = dict_entity_ref(it)->value;
				if(!e || e->id <= sc->a2w_entity_after_id
				   || a2w_entity_published(e, w)
				   || server_local_entity_world(s, e) != w
				   || landing_floor_div16((w_coord_t)floor(e->pos[0])) != cx
				   || landing_floor_div16((w_coord_t)floor(e->pos[2])) != cz)
					continue;
				if(!next || e->id < next->id)
					next = e;
			}
			if(!next)
				break;
			if(!server_local_try_publish_entity_w(s, w, next))
				return false;
			sc->a2w_entity_after_id = next->id;
		}
		sc->a2w_pub_stage = 3;
	}
	sc->sent = true;
	return true;
}

/* colcap-1006: `sent` keeps ONE meaning everywhere -- the column's TERRAIN is
 * published (server_world_is_chunk_sent readers: guest admission/respawn
 * gates, residency debt, the A2-W marker). An entity republish on an already
 * published column is pending work tracked by a2w_pub_stage alone (stage 1/2
 * with sent still true; server_transfer_secondary_entity_joined below). The
 * stream picker asks THIS, not `sent`, so that pending work is still served. */
static bool a2w_publication_done(struct server_world* w, w_coord_t cx,
								 w_coord_t cz) {
	const struct server_chunk* sc
		= dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	return sc && sc->sent && sc->a2w_pub_stage != 1 && sc->a2w_pub_stage != 2;
}

/* The pinned rectangle of ONE open transaction into `w` (Sol review 2, A):
 * the SAME rectangle the slot-0 furthest-column selection skips and the
 * per-column veto refuses, so a single transfer can never leave the
 * selection a victim the veto then refuses (the stall). While the job loads
 * an EXISTING portal's winner chunks (TP_JOB_LOAD_WINNER) that is Chebyshev
 * 1 around the first winner chunk -- the winner's chunk plus its seam
 * neighbours (tp_job_prepare_winner_chunks adds only +/-1 in x and z) --
 * because no frame is created on that path; otherwise Chebyshev
 * SERVER_TRANSFER_TELEPORTER_PIN_RADIUS around the staged destination
 * pose (create/load range, frame build, and after DONE the landing core). */
static bool teleporter_rect_of(const struct server_local* s,
							   const struct server_transfer_txn* t,
							   const struct server_world* w, w_coord_t* cx,
							   w_coord_t* cz, w_coord_t* r) {
	if(!t->open || t->dst_slot >= SERVER_LOCAL_WORLD_SLOTS
	   || &s->worlds[t->dst_slot] != w)
		return false;
	w_coord_t wx[4], wz[4];
	unsigned n = portal_teleporter_job_winner_chunks(t->job, wx, wz, 4);
	if(n) {
		*cx = wx[0];
		*cz = wz[0];
		*r = 1;
		for(unsigned k = 1; k < n; k++)
			assert(a2w_cheb(wx[k], wz[k], wx[0], wz[0]) <= 1);
		return true;
	}
	*cx = landing_floor_div16((w_coord_t)floor(t->pose.x));
	*cz = landing_floor_div16((w_coord_t)floor(t->pose.z));
	*r = SERVER_TRANSFER_TELEPORTER_PIN_RADIUS;
	return true;
}

bool server_transfer_teleporter_pin(const struct server_local* s,
									const struct server_world* w,
									w_coord_t* pin_x, w_coord_t* pin_z,
									w_coord_t* pin_r) {
	if(!s || !w)
		return false;
	for(size_t i = 0; i < 2; i++)
		if(teleporter_rect_of(s, &s->transfer[i], w, pin_x, pin_z, pin_r))
			return true;
	return false;
}

bool server_transfer_teleporter_pinned(const struct server_local* s,
									   const struct server_world* w,
									   w_coord_t cx, w_coord_t cz) {
	if(!s || !w)
		return false;
	for(size_t i = 0; i < 2; i++) {
		w_coord_t px, pz, pr;
		if(teleporter_rect_of(s, &s->transfer[i], w, &px, &pz, &pr)
		   && a2w_cheb(cx, cz, px, pz) <= pr)
			return true;
	}
	return false;
}

static bool a2w_client_claimed(const struct server_chunk* sc) {
	return sc && (sc->sent || sc->a2w_pub_stage != 0);
}

static void a2w_publication_clear(struct server_chunk* sc) {
	if(!sc)
		return;
	sc->sent = false;
	sc->a2w_pub_stage = 0;
	sc->a2w_sign_cursor = 0;
	sc->a2w_entity_after_id = 0;
}

void server_transfer_secondary_entity_joined(struct server_local* s,
										 const struct entity* e) {
	if(!s || !e || !clin_world_payloads_enabled())
		return;
	struct server_world* w = server_local_entity_world(s, e);
	if(!w || w->world_index == 0 || !s->a2w_stream_on[w->world_index])
		return;
	w_coord_t cx = landing_floor_div16((w_coord_t)floor(e->pos[0]));
	w_coord_t cz = landing_floor_div16((w_coord_t)floor(e->pos[2]));
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	if(!sc || a2w_entity_published(e, w))
		return;
	/* An already published column only owes its ENTITIES again: re-enter the
	 * entity stage, keeping `sent` (its terrain is still published -- the
	 * client holds it; colcap-1006: clearing it made every terrain-readiness
	 * reader of a secondary world blip for a tick per entity crossing). */
	if(sc->sent && sc->a2w_pub_stage != 1)
		sc->a2w_pub_stage = 2;
	/* An older ID may cross from an unsent support column after this column
	 * published larger IDs. Accepted spawns carry their own marker, so rescanning
	 * from zero cannot duplicate them and catches the incoming old ID. */
	if(sc->a2w_pub_stage >= 2)
		sc->a2w_entity_after_id = 0;
}

/* Keep both an open transaction's arrival core and the support of an
 * unfinished population cascade, as the primary-world evictors do. */
static bool a2w_pinned(const struct server_local* s, const struct server_world* w,
					   w_coord_t cx, w_coord_t cz) {
	for(unsigned stream = 0; stream < 2; stream++)
		if(w->gen_pending[stream]
		   && llabs((int64_t)w->gen_pending_x[stream] - cx) <= 1
		   && llabs((int64_t)w->gen_pending_z[stream] - cz) <= 1)
			return true;
	for(size_t i = 0; i < 2; i++) {
		const struct server_transfer_txn* t = &s->transfer[i];
		if(!t->open || t->dst_slot != w->world_index)
			continue;
		if(a2w_cheb(cx, cz, landing_floor_div16((w_coord_t)floor(t->pose.x)),
					landing_floor_div16((w_coord_t)floor(t->pose.z)))
		   <= SERVER_TRANSFER_LANDING_CORE_RADIUS + 1)
			return true;
	}
	/* Sol review 2 (B): the Teleporter work area -- including an EXISTING
	 * portal's winner chunks up to 128 blocks from the pose -- is pinned in
	 * a secondary world too (Beta chunkLoadOverride parity). */
	if(server_transfer_teleporter_pinned(s, w, cx, cz))
		return true;
	if(w->world_index == s->guest_respawn_target.world_index
	   && s->guest_respawn_cap.reserved
	   && s->guest_respawn_cap.landing_armed
	   && a2w_cheb(cx, cz,
		landing_floor_div16((w_coord_t)floor(s->guest_respawn_x)),
		landing_floor_div16((w_coord_t)floor(s->guest_respawn_z)))
		<= SERVER_TRANSFER_LANDING_CORE_RADIUS + 1)
		return true;
	return false;
}

static void a2w_evict(struct server_local* s, struct server_world* w,
					  struct server_player* const* actors, size_t n, int r) {
	/* The primary saver decrements this shared retry clock once per tick.
	 * Honor it here too after a failed secondary batch publication. */
	if(s->chunk_save_retry_delay > 0)
		return;
	int64_t victims[A2W_EVICTS_PER_TICK];
	int nv = 0;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks);
		!dict_server_chunks_end_p(it) && nv < A2W_EVICTS_PER_TICK;
		dict_server_chunks_next(it)) {
		int64_t id = dict_server_chunks_ref(it)->key;
		w_coord_t cx = S_CHUNK_X(id), cz = S_CHUNK_Z(id);
		/* residency-1005/2 S3: never a body's accepted footprint column */
		if(!a2w_wanted(s, actors, n, cx, cz, r) && !a2w_pinned(s, w, cx, cz)
		   && !server_local_rz_evict_veto(s, w, cx, cz))
			victims[nv++] = id;
	}
	if(!nv)
		return;
	/* This secondary-world pass saves up to eight columns. Give each touched
	 * region one batch publication, allowing its helper to do the fsyncs.
	 * A caller-owned batch is left open for its own seam. */
	if(!w->save_batch_open) {
		server_world_save_batch_begin(w);
		s->a2w_batch_owned[w->world_index] = true;
	}
	uint32_t refused_before = w->retiring_refused;
	for(int i = 0; i < nv; i++) {
		w_coord_t cx = S_CHUNK_X(victims[i]), cz = S_CHUNK_Z(victims[i]);
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, victims[i]);
		if(!sc)
			continue;
		/* G9 (ASTRA-G9-REVIEW Required 5): a column pinned by pending light
		 * is not coherent; its eviction waits for the drain. Skip it BEFORE
		 * the client UNLOAD so the publication is not churned (it stays
		 * resident, published, and is retried next update). */
		if(server_world_light_pinned(w, cx, cz)) {
			server_world_light_note_evict_deferred();
			continue;
		}
		/* the client drops the column first (a full inbox retries next
		 * update with the column still resident), then the server saves and
		 * evicts it; a failed save keeps it resident -- unsent, so a return
		 * streams it again */
		if(a2w_client_claimed(sc)
		   && !server_local_try_send_w(w, (struct client_rpc) {
				  .type = CRPC_UNLOAD_CHUNK,
				  .payload.unload_chunk.x = cx,
				  .payload.unload_chunk.z = cz,
			  }))
			break;
		a2w_publication_clear(sc);
		if(server_world_save_chunk(w, true, cx, cz))
			s->a2w_evicted[w->world_index]++;
	}
	if(w->retiring_refused != refused_before)
		(void)server_transfer_flush_owned_batch(s, w);
}

/* The near ring around `p` in `w` is resident and sent. */
static bool a2w_ring_sent(struct server_world* w, const struct server_player* p,
						  int r) {
	w_coord_t pcx = landing_floor_div16((w_coord_t)floor(p->x));
	w_coord_t pcz = landing_floor_div16((w_coord_t)floor(p->z));
	for(int dz = -r; dz <= r; dz++)
		for(int dx = -r; dx <= r; dx++)
			if(!server_world_is_chunk_sent(w, pcx + dx, pcz + dz))
				return false;
	return true;
}

void server_transfer_stream_step(struct server_local* s) {
	if(!s)
		return;
	const int r = server_local_view_radius(s);
	const bool two_actors = s->guest_actor != NULL || s->split_enabled;
	/* One ceiling whether or not a transfer is open (ASTRA-CC-R2 item 2). */
	const size_t cap = A2W_STEADY_COLUMN_CAP;
	/* A guest body committed to a slot >= 1 world outside player 0's by a
	 * path other than an A2-T COMMIT -- the saved-dimension Join, a respawn
	 * into an Overworld that is not slot 0 (lane C) -- has no client context
	 * there yet: give it one (gate 1, FIFO-ahead of every column) and start
	 * that world's stream, so the body's column can be published and its
	 * gameplay admission (guest_terrain_ready) completes. */
	struct server_player* pending_guest = NULL;
	if(clin_world_payloads_enabled() && s->guest_actor && s->guest_actor->has_pos) {
		struct server_world* gw = server_local_world_of(s, s->guest_actor);
		if(gw && gw->world_index != 0
		   && gw != server_local_world_of(s, &s->player)
		   && s->world_slot_state[gw->world_index] == SERVER_WORLD_SLOT_LIVE
		   && !server_transfer_world_in_flight(s, gw)) {
			if(!s->a2w_stream_on[gw->world_index]) {
				a2w_guest_gate1(gw);
				s->a2w_stream_on[gw->world_index] = true;
				fprintf(stderr, "[a2w] guest body in slot %u: gate-1 bind + stream\n",
						(unsigned)gw->world_index);
			}
			/* residency-1005/6 R2: a Join whose bounded terrain wait timed out
			 * no longer owns stream demand for its body */
			if(!s->guest_actor->finished_loading
			   && !server_local_guest_join_wait_failed(s))
				pending_guest = s->guest_actor;
		}
	}
	for(uint8_t slot = 1; slot < SERVER_LOCAL_WORLD_SLOTS; slot++) {
		server_diag_set(SERVER_DIAG_A2W_PICK);
		struct server_world* w = &s->worlds[slot];
		if(s->world_slot_state[slot] != SERVER_WORLD_SLOT_LIVE
		   || !server_local_world_live(w)) {
			if(server_local_world_live(w)) {
				server_diag_set(SERVER_DIAG_A2W_FLUSH);
				(void)server_transfer_flush_owned_batch(s, w);
			} else
				s->a2w_batch_owned[slot] = false;
			s->a2w_stream_on[slot] = false;
			continue;
		}
		if(!s->a2w_stream_on[slot]) {
			server_diag_set(SERVER_DIAG_A2W_FLUSH);
			(void)server_transfer_flush_owned_batch(s, w);
			continue;
		}
		if(s->a2w_reset_pending[slot]) {
			server_diag_set(SERVER_DIAG_A2W_FLUSH);
			(void)server_transfer_flush_owned_batch(s, w);
			continue;
		}
		struct server_player* actors[SERVER_LOCAL_MAX_PRESENT_ACTORS];
		size_t n = server_local_eligible_actors_w(s, w, actors,
												  SERVER_LOCAL_MAX_PRESENT_ACTORS);
		/* a guest body still waiting for its gameplay admission is streamed
		 * for too (that admission needs its column published) */
		if(pending_guest && server_local_world_of(s, pending_guest) == w
		   && n < SERVER_LOCAL_MAX_PRESENT_ACTORS) {
			bool listed = false;
			for(size_t a = 0; a < n; a++)
				listed |= actors[a] == pending_guest;
			if(!listed)
				actors[n++] = pending_guest;
		}
		struct server_player respawn_anchor = {0};
		if(n < SERVER_LOCAL_MAX_PRESENT_ACTORS
		   && server_local_guest_respawn_target(s) == w) {
			respawn_anchor.x = s->guest_respawn_x;
			respawn_anchor.z = s->guest_respawn_z;
			actors[n++] = &respawn_anchor;
		}
		if(n == 0) {
			/* vacated: the retire-vacant drain owns it from here, and a
			 * later arrival re-enables the stream after its own reset */
			server_diag_set(SERVER_DIAG_A2W_FLUSH);
			(void)server_transfer_flush_owned_batch(s, w);
			s->a2w_stream_on[slot] = false;
			continue;
		}
		server_diag_set(SERVER_DIAG_A2W_EVICT);
		a2w_evict(s, w, actors, n, r);
		for(int k = 0; k < A2W_SENDS_PER_TICK; k++) {
			server_diag_set(SERVER_DIAG_A2W_PICK);
			/* nearest unsent column of the wanted set */
			int best_d = INT32_MAX;
			w_coord_t bx = 0, bz = 0;
			/* residency-1005/2 S2: a body's destination request in this
			 * world goes first (same per-tick send budget). */
			int32_t rqx = 0, rqz = 0;
			if(server_local_rz_pick(s, w, &rqx, &rqz)) {
				best_d = 0;
				bx = rqx;
				bz = rqz;
			}
			for(size_t a = 0; a < n && best_d != 0; a++) {
				w_coord_t acx = landing_floor_div16((w_coord_t)floor(actors[a]->x));
				w_coord_t acz = landing_floor_div16((w_coord_t)floor(actors[a]->z));
				/* Transfer HOLD narrows a still-uncommitted landing; after COMMIT the
				 * host remains held for presentation but needs its full own-world
				 * stream (and at least radius 2 before the marker). */
				int ar = server_transfer_actor_frozen(s, actors[a])
					&& !(actors[a] == &s->player && s->a2w_host_presentation)
					? 1 : a2w_actor_radius(s, actors[a], r);
				for(int dz = -ar; dz <= ar; dz++)
					for(int dx = -ar; dx <= ar; dx++) {
						int d = dx < 0 ? -dx : dx;
						int e = dz < 0 ? -dz : dz;
						d = d > e ? d : e;
						if(d >= best_d
						   || a2w_publication_done(w, acx + dx, acz + dz))
							continue;
						best_d = d;
						bx = acx + dx;
						bz = acz + dz;
					}
			}
			if(best_d == INT32_MAX)
				break; /* the whole wanted set is streamed */
			if(two_actors && !server_world_chunk_resident(w, bx, bz)
			   && server_transfer_resident_columns(s) >= cap)
				break; /* over the column cap: wait for the old area to drain */
			struct server_chunk* sc = NULL;
			server_diag_set(SERVER_DIAG_A2W_ENSURE);
			if(!server_world_ensure_populated(w, s, bx, bz, &sc) || !sc)
				break; /* generation deferred: next update */
			server_diag_set(SERVER_DIAG_A2W_SEND);
			if(!a2w_send_column(s, w, sc, bx, bz))
				break; /* the client is behind: next update */
			s->a2w_sent[slot]++;
		}
		/* Match the primary's measured 40-tick publication window. This
		 * stream step runs before the primary increments save_batch_ticks. */
		if(s->save_batch_ticks == SAVE_BATCH_MAX_TICKS - 1) {
			server_diag_set(SERVER_DIAG_A2W_FLUSH);
			(void)server_transfer_flush_owned_batch(s, w);
		}
	}
	/* the host's load-screen release after a COMMIT: the near ring of the
	 * host's OWN world (A2W-CONTRACT 9), bounded */
	if(s->a2w_host_marker_pending && s->input_ack_pending == 0) {
		server_diag_set(SERVER_DIAG_A2W_MARKER);
		struct server_world* hw = server_local_world_of(s, &s->player);
		bool ready = hw && a2w_ring_sent(hw, &s->player, r < 2 ? r : 2);
		if(ready || ++s->a2w_host_marker_wait >= A2W_MARKER_MAX_WAIT) {
			if(server_local_host_world_marker(s)) {
				if(!ready)
					fprintf(stderr, "[a2w] host marker released after %u updates "
									"without the full near ring\n",
							(unsigned)s->a2w_host_marker_wait);
				s->a2w_host_marker_pending = false;
				s->a2w_host_marker_wait = 0;
			}
		}
	}
}

static void close_txn(struct server_local* s, struct server_transfer_txn* t,
					  enum server_transfer_state final) {
	if(t->job) {
		portal_teleporter_destroy(t->job);
		t->job = NULL;
	}
	/* release what the client holds for a transaction that did not commit */
	if(final == SERVER_TRANSFER_ROLLED_BACK) {
		/* M3: cancel whenever a landing was ever armed -- also when the
		 * rollback comes after READY (COMMIT_READY: record failure, actor
		 * left, destination gone), so the client never keeps a landing
		 * armed for a transaction that did not commit */
		if(t->landing_armed)
			t->landing_cancel_pending = !send_landing(s, t, true);
		if(t->capacity_reserved)
			queue_release(s, t->capacity_ticket); /* M2(a)/(c) */
	}
	t->open = false;
	t->state = final;
	t->last_id = t->id;
	t->last_state = final;
	/* a destination this transaction admitted and nobody joined is released */
	if(final == SERVER_TRANSFER_ROLLED_BACK && t->admitted_dst
	   && !t->landing_cancel_pending) {
		struct server_world* dst = &s->worlds[t->dst_slot];
		if(!slot_has_member(s, t->dst_slot))
			(void)server_local_world_retire(s, dst);
	}
	server_local_portal_diag_publish(s);
}

static bool finish_rollback(struct server_local* s, struct server_transfer_txn* t,
					 const char* why) {
	struct server_player* actor = txn_actor(s, t);
	if(t->hold_sent && actor) {
		const uint8_t flags = PLAYER_CORRECTION_POSITION_ONLY
			| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
			| PLAYER_CORRECTION_ZERO_VEL_Z;
		if(actor == &s->player) {
			server_local_next_player_correction(s);
			struct client_rpc rpc = {
				.type = CRPC_PLAYER_POS,
				.payload.player_pos = {
					.position = {actor->x, actor->y, actor->z},
					.rotation = {actor->rx, actor->ry},
					.correction_seq = actor->correction_seq,
					.correction_flags = flags,
				},
			};
			if(!clin_try_rpc_send(&rpc))
				return false;
		} else
			if(!server_local_guest_correct_position_for(s,
					t->actor_generation, flags))
				return false;
	}
	fprintf(stderr, "[transfer] request %u ROLLBACK: %s\n", t->id, why);
	/* M4: a player who was told it is travelling is told it is not */
	if(t->hud_shown && actor)
		server_local_actor_hud_message(s, actor, SERVER_TRANSFER_HUD_CANCELLED);
	publish_outcome(s, t, actor, SERVER_TRANSITION_ROLLBACK);
	if(t->hold_sent) {
		t->hold_end_restored = actor != NULL;
		t->hold_end_pending = true;
	}
	close_txn(s, t, SERVER_TRANSFER_ROLLED_BACK);
	/* Cancel a prepared landing before releasing the owner. Both are FIFO
	 * publications; a full inbox keeps the terminal END pending for next tick. */
	if(t->hold_end_pending && !t->landing_cancel_pending)
		t->hold_end_pending = !send_transfer_hold(t, false);
	budget_publish(s);
	return true;
}

static void rollback(struct server_local* s, struct server_transfer_txn* t,
					 enum server_transfer_rollback_reason reason, const char* why) {
	if(t->state != SERVER_TRANSFER_RESTORING && t->actor_slot < 2) {
		s->transfer_rollback_id[t->actor_slot] = t->id;
		s->transfer_rollback_reason[t->actor_slot] = (uint8_t)reason;
	}
	if(t->hold_sent && t->state != SERVER_TRANSFER_RESTORING) {
		fprintf(stderr, "[transfer] request %u restoring source after: %s\n",
			(unsigned)t->id, why);
		t->state = SERVER_TRANSFER_RESTORING;
		t->restore_radius = (uint8_t)server_local_view_radius(s);
		if(t->job) {
			portal_teleporter_destroy(t->job);
			t->job = NULL;
		}
		budget_publish(s);
		return;
	}
	if(!t->hold_sent)
		(void)finish_rollback(s, t, why);
}

/* The one allocation-free step after the durable marker. */
static void commit(struct server_local* s, struct server_transfer_txn* t,
				   struct server_player* actor) {
	struct server_world* dst = &s->worlds[t->dst_slot];
	/* G8-VEHICLE-SEAMS-01: the vehicle stays in the source world; release
	 * this actor's relations at COMMIT, before its membership changes. A
	 * rolled-back transaction never reaches here and keeps the mount. */
	server_local_release_actor_mounts(s, actor, NULL);
	/* host_vehicle_smoke_8m transfer_release: AFTER the shared release,
	 * BEFORE the actor's world_slot/dimension mutation. */
	if(vsp_armed())
		vsp_srv_transfer_released(s, actor, t->id, t->src_slot, t->dst_slot);
	actor->world_slot = t->dst_slot;
	actor->dimension = dst->dimension;
	/* residency-1005/2: the body enters a new world instance */
	server_local_rz_retire_body(actor);
	/* The heartbeat can race the next client publication in this same update.
	 * Mirror the committed body before outcome or WORLD_RESET enters its FIFO. */
	if(actor == &s->player) {
		extern int32_t g_srv_host_dim;
		__atomic_store_n(&g_srv_host_dim, (int32_t)actor->dimension,
						 __ATOMIC_RELAXED);
	} else {
		extern int32_t g_srv_guest_dim;
		__atomic_store_n(&g_srv_guest_dim, (int32_t)actor->dimension,
						 __ATOMIC_RELAXED);
	}
	actor->x = t->pose.x;
	actor->y = t->pose.y;
	actor->z = t->pose.z;
	actor->rx = t->pose.rx;
	actor->ry = t->pose.ry;
	actor->vel_x = t->pose.vel_x;
	actor->vel_y = t->pose.vel_y;
	actor->vel_z = t->pose.vel_z;
	actor->old_vel_y = t->pose.old_vel_y;
	/* Beta arrival: setInPortal pins the cooldown at 10, dwell consumed */
	actor->time_until_portal = 10;
	actor->time_in_portal = 1.0f;
	actor->in_portal = false;
	actor->portal_travel_requested = false;
	actor->added_to_chunk = false;
	if(actor == &s->player) {
		server_local_refresh_player_membership(s);
		/* split_dims_6m telemetry (server_local.c): host COMMITs */
		extern uint32_t g_srv_host_commits;
		__atomic_add_fetch(&g_srv_host_commits, 1u, __ATOMIC_RELAXED);
	}
	publish_outcome(s, t, actor, SERVER_TRANSITION_COMMIT);
	/* host_vehicle_smoke_8m transfer_commit: after membership mutation and
	 * publish_outcome, before close_txn. */
	if(vsp_armed())
		vsp_srv_transfer_committed(s, actor, t->id, t->src_slot, t->dst_slot,
								   t->outcome.transition_epoch);
	fprintf(stderr, "[transfer] request %u COMMIT: actor %u now in slot %u (dim %d) at "
					"%.1f %.1f %.1f epoch %u\n",
			t->id, t->actor_slot, t->dst_slot, (int)dst->dimension, actor->x,
			actor->y, actor->z, t->outcome.transition_epoch);
	close_txn(s, t, SERVER_TRANSFER_COMMITTED);
	if(t->actor_slot == ACTOR_SLOT_GUEST)
		s_guest_commits++;
	budget_publish(s);
}

/* Serialization: the request whose SOURCE is the Overworld first, then by
 * actor slot (host before guest). One transaction is driven at a time. */
static struct server_transfer_txn* pick_active(struct server_local* s) {
	struct server_transfer_txn* best = NULL;
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* t = &s->transfer[i];
		if(!t->open)
			continue;
		if(t->state != SERVER_TRANSFER_REQUESTED)
			return t; /* never interleave one that is past REQUEST */
		if(!best)
			best = t;
		else {
			bool best_ow = s->worlds[best->src_slot].dimension == WORLD_DIM_OVERWORLD;
			bool t_ow = s->worlds[t->src_slot].dimension == WORLD_DIM_OVERWORLD;
			if(t_ow && !best_ow)
				best = t;
			else if(t_ow == best_ow && t->actor_slot < best->actor_slot)
				best = t;
		}
	}
	return best;
}

/* The mover is held in its 3x3 source collision core. Its partner keeps the
 * full radius-four ring; only the mover's outer source columns may be spent
 * on the destination. One bounded save/unload batch runs per server tick. */
static bool source_drain_step(struct server_local* s,
								struct server_transfer_txn* t) {
	struct server_world* src = &s->worlds[t->src_slot];
	struct server_player* partner = t->actor_slot == ACTOR_SLOT_HOST
		? s->guest_actor : &s->player;
	bool partner_here = partner && server_local_world_of(s, partner) == src;
	w_coord_t pcx = 0, pcz = 0;
	if(partner_here) {
		pcx = landing_floor_div16((w_coord_t)floor(partner->x));
		pcz = landing_floor_div16((w_coord_t)floor(partner->z));
	}
	int64_t victims[A2W_EVICTS_PER_TICK];
	unsigned n = 0;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, src->chunks);
		!dict_server_chunks_end_p(it) && n < A2W_EVICTS_PER_TICK;
		dict_server_chunks_next(it)) {
		int64_t id = dict_server_chunks_ref(it)->key;
		w_coord_t cx = S_CHUNK_X(id), cz = S_CHUNK_Z(id);
		if(a2w_cheb(cx, cz, t->source_cx, t->source_cz) <= 1
		   || (partner_here && a2w_cheb(cx, cz, pcx, pcz)
									<= server_local_view_radius(s)))
			continue;
		victims[n++] = id;
	}
	if(!n)
		return true;
	for(unsigned i = 0; i < n; i++) {
		w_coord_t cx = S_CHUNK_X(victims[i]), cz = S_CHUNK_Z(victims[i]);
		struct server_chunk* sc = dict_server_chunks_get(src->chunks, victims[i]);
		if(!sc)
			continue;
		if(a2w_client_claimed(sc)
		   && !server_local_try_send_w(src, (struct client_rpc) {
				.type = CRPC_UNLOAD_CHUNK,
				.payload.unload_chunk.x = cx,
				.payload.unload_chunk.z = cz,
			}))
			return false;
		a2w_publication_clear(sc);
		if(!server_world_save_chunk(src, true, cx, cz))
			return false;
	}
	budget_publish(s);
	return false;
}

static bool source_restore_step(struct server_local* s,
								  struct server_transfer_txn* t) {
	struct server_world* dst = &s->worlds[t->dst_slot];
	struct server_world* src = &s->worlds[t->src_slot];
	struct server_player* actor = txn_actor(s, t);
	struct server_player* partner = t->actor_slot == ACTOR_SLOT_HOST
		? s->guest_actor : &s->player;
	bool partner_in_dst = partner && server_local_world_of(s, partner) == dst;
	w_coord_t pcx = 0, pcz = 0;
	if(partner_in_dst) {
		pcx = landing_floor_div16((w_coord_t)floor(partner->x));
		pcz = landing_floor_div16((w_coord_t)floor(partner->z));
	}
	/* Reclaim staged destination columns first. Client-held backings still
	 * count against admission until their UNLOAD lease actually retires. */
	unsigned trimmed = 0;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, dst->chunks);
		!dict_server_chunks_end_p(it) && trimmed < A2W_EVICTS_PER_TICK;) {
		int64_t id = dict_server_chunks_ref(it)->key;
		w_coord_t cx = S_CHUNK_X(id), cz = S_CHUNK_Z(id);
		if(partner_in_dst && a2w_cheb(cx, cz, pcx, pcz)
									<= server_local_view_radius(s)) {
			dict_server_chunks_next(it);
			continue;
		}
		struct server_chunk* sc = dict_server_chunks_get(dst->chunks, id);
		if(a2w_client_claimed(sc)
		   && !server_local_try_send_w(dst, (struct client_rpc) {
				.type = CRPC_UNLOAD_CHUNK,
				.payload.unload_chunk.x = cx,
				.payload.unload_chunk.z = cz,
			}))
			return false;
		a2w_publication_clear(sc);
		if(!server_world_save_chunk(dst, true, cx, cz))
			return false;
		trimmed++;
		/* Erase invalidates the iterator; restart from the dictionary head. */
		dict_server_chunks_it(it, dst->chunks);
	}
	if(trimmed) {
		budget_publish(s);
		return false;
	}
	if(!server_local_world_live(src))
		return true; /* retired source has no dictionary or client route */
	if(!actor) {
		/* A departed owner has no collision ring to restore, but a CHUNK
		 * already accepted before LEFT still has one publisher: its staged
		 * sign/entity cursor. Finish it if a partner still needs that column.
		 * Otherwise cancel its client claim before closing RESTORING, so the
		 * ordinary near pass cannot inherit an orphaned partial stage. */
		bool partner_in_src = partner && partner->has_pos
			&& server_local_world_of(s, partner) == src;
		bool routed = slot_has_member(s, src->world_index)
			&& (src->world_index == 0
				? server_local_world_live(src)
				: s->world_slot_state[src->world_index] == SERVER_WORLD_SLOT_LIVE)
			&& ((s->player.has_pos
					&& src == server_local_world_of(s, &s->player))
				|| (src->world_index != 0
					&& s->a2w_stream_on[src->world_index]));
		w_coord_t partner_cx = 0, partner_cz = 0;
		if(partner_in_src) {
			partner_cx = landing_floor_div16((w_coord_t)floor(partner->x));
			partner_cz = landing_floor_div16((w_coord_t)floor(partner->z));
		}
		dict_server_chunks_it_t source_it;
		for(dict_server_chunks_it(source_it, src->chunks);
			!dict_server_chunks_end_p(source_it);
			dict_server_chunks_next(source_it)) {
			struct server_chunk* sc = &dict_server_chunks_ref(source_it)->value;
			if(sc->a2w_pub_stage == 0 || sc->sent)
				continue;
			int64_t id = dict_server_chunks_ref(source_it)->key;
			w_coord_t cx = S_CHUNK_X(id), cz = S_CHUNK_Z(id);
			if(routed && partner_in_src
			   && a2w_cheb(cx, cz, partner_cx, partner_cz)
				  <= server_local_view_radius(s)) {
				if(!a2w_send_column(s, src, sc, cx, cz))
					return false;
			} else {
				if(routed && !server_local_try_send_w(src, (struct client_rpc) {
						.type = CRPC_UNLOAD_CHUNK,
						.payload.unload_chunk.x = cx,
						.payload.unload_chunk.z = cz,
					}))
					return false;
				a2w_publication_clear(sc);
			}
			return false; /* one accepted completion/cancellation per step */
		}
		return true;
	}
	int r = t->restore_radius;
	for(int dz = -r; dz <= r; dz++)
		for(int dx = -r; dx <= r; dx++) {
			w_coord_t cx = t->source_cx + dx, cz = t->source_cz + dz;
			if(server_world_is_chunk_sent(src, cx, cz))
				continue;
			struct server_chunk* sc = NULL;
			if(!server_world_ensure_populated(src, s, cx, cz, &sc) || !sc)
				return false;
			if(!a2w_send_column(s, src, sc, cx, cz))
				return false;
			budget_publish(s);
			return false; /* one restored client column per tick */
		}
	return true;
}

bool server_transfer_step(struct server_local* s) {
	if(!s)
		return true;
	server_diag_set(SERVER_DIAG_TRANSFER_PENDING);
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* prior = &s->transfer[i];
		if(prior->landing_cancel_pending && send_landing(s, prior, true)) {
			prior->landing_cancel_pending = false;
			if(!prior->open && prior->admitted_dst
			   && !prior->hold_end_pending
			   && !slot_has_member(s, prior->dst_slot))
				(void)server_local_world_retire(s,
					&s->worlds[prior->dst_slot]);
		}
		if(prior->hold_end_pending && !prior->landing_cancel_pending
		   && send_transfer_hold(prior, false)) {
			prior->hold_end_pending = false;
			if(!prior->open && prior->admitted_dst
			   && !slot_has_member(s, prior->dst_slot))
				(void)server_local_world_retire(s,
					&s->worlds[prior->dst_slot]);
			budget_publish(s);
		}
	}
	server_diag_set(SERVER_DIAG_TRANSFER_BUDGET);
	server_transfer_budget_tick(s);
	server_diag_set(SERVER_DIAG_TRANSFER_RELEASES);
	drain_releases(s); /* M2(a): a refused RELEASE retries every step */
	/* M4: every open transaction (the stepped one and a queued opposite one)
	 * keeps its actor informed, and a body that died while travelling does
	 * not travel -- the transaction rolls back so the death screen's Respawn
	 * takes the ordinary path (respawn is refused while a transfer is open). */
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* o = &s->transfer[i];
		if(!o->open)
			continue;
		if(o->state == SERVER_TRANSFER_RESTORING)
			continue;
		struct server_player* oa = txn_actor(s, o);
		if(!oa)
			continue; /* rolled back below by the stepped path */
		if(oa->health <= 0) {
			o->hud_shown = false; /* the death screen is the notice */
			rollback(s, o, SERVER_TRANSFER_ROLLBACK_ACTOR_DIED, "the acting body died");
			continue;
		}
		if(o->hud_ticks++ % SERVER_TRANSFER_HUD_REFRESH_TICKS == 0) {
			server_local_actor_hud_message(s, oa, SERVER_TRANSFER_HUD_TRAVELLING);
			o->hud_shown = true;
		}
	}
	server_diag_set(SERVER_DIAG_TRANSFER_PICK);
	struct server_transfer_txn* t = pick_active(s);
	server_local_portal_diag_publish(s);
	if(!t)
		return true;
	struct server_player* actor = txn_actor(s, t);
	struct server_world* dst = &s->worlds[t->dst_slot];
	if(t->state == SERVER_TRANSFER_RESTORING) {
		server_diag_set(SERVER_DIAG_TRANSFER_RESTORE);
		if(source_restore_step(s, t))
			finish_rollback(s, t, "source ring restored");
		return !server_transfer_active(s);
	}
	if(!actor) {
		rollback(s, t, SERVER_TRANSFER_ROLLBACK_ACTOR_LEFT, "the acting body left the session");
		return !server_transfer_active(s);
	}
	if(!server_local_world_live(dst)) {
		rollback(s, t, SERVER_TRANSFER_ROLLBACK_DESTINATION_GONE,
			"the destination world is no longer live");
		return !server_transfer_active(s);
	}
	if(t->state == SERVER_TRANSFER_REQUESTED) {
		server_diag_set(SERVER_DIAG_TRANSFER_REQUEST);
		/* The bounded two-world protocol is opt-in. Legacy single-payload
		 * transfers retain their original direct PREPARE path and may use the
		 * full configured source radius (which has no HOLD/restore contract). */
		if(!clin_world_payloads_enabled()) {
			t->state = SERVER_TRANSFER_SOURCE_DRAINING;
			return false;
		}
		if(!t->hold_sent) {
			if(!send_transfer_hold(t, true))
				return false;
			t->hold_sent = true;
			t->source_cx = landing_floor_div16((w_coord_t)floor(actor->x));
			t->source_cz = landing_floor_div16((w_coord_t)floor(actor->z));
			budget_publish(s);
		}
		if(s->transfer_budget_phase != SERVER_TRANSFER_BUDGET_ENFORCED)
			return false; /* source ring drains before destination work */
		if(!a2w_ring_sent(&s->worlds[t->src_slot], actor, 1))
			return false; /* the actor's collision core stays loaded during drain */
		t->state = SERVER_TRANSFER_SOURCE_DRAINING;
		budget_publish(s);
		return false;
	}
	if(t->state == SERVER_TRANSFER_SOURCE_DRAINING) {
		server_diag_set(SERVER_DIAG_TRANSFER_DRAIN);
		server_local_portal_diag_publish(s);
		if(clin_world_payloads_enabled() && !source_drain_step(s, t))
			return false;
		/* PREPARE: stage the reversible state. The source stays authoritative;
		 * the actor's own window closes (Beta closes any GUI on portal contact). */
		if(!server_local_close_active_inventory_for(s, actor)) {
			rollback(s, t, SERVER_TRANSFER_ROLLBACK_WINDOW,
				"the actor's window could not be closed");
			return !server_transfer_active(s);
		}
		struct java_random rng;
		uint64_t rng_seed
			= ((uint64_t)CLOCK_SEED_TIME() * UINT64_C(25214903917)
			   + (uint64_t)dst->current_tick)
			& ((UINT64_C(1) << 48) - 1);
		jrand_set_seed(&rng, (int64_t)rng_seed);
		t->job = portal_teleporter_begin_w(s, dst, &t->pose, &rng);
		if(!t->job) {
			rollback(s, t, SERVER_TRANSFER_ROLLBACK_TELEPORTER_ALLOC,
				"teleporter allocation failed");
			return !server_transfer_active(s);
		}
		t->state = SERVER_TRANSFER_PREPARING;
		t->prepare_ticks = 0;
		server_local_portal_diag_publish(s);
		return false;
	}
	if(t->state == SERVER_TRANSFER_CAPACITY || t->state == SERVER_TRANSFER_LANDING) {
		server_diag_set(SERVER_DIAG_TRANSFER_WAIT);
		return step_client_wait(s, t);
	}
	if(t->state == SERVER_TRANSFER_COMMIT_READY) {
		server_diag_set(SERVER_DIAG_COMMIT_MARKER);
		return step_commit(s, t, actor);
	}
	/* PREPARING: one bounded teleporter slice per tick on the DESTINATION */
	server_diag_set(SERVER_DIAG_TRANSFER_PREPARE);
	server_local_portal_diag_publish(s);
	if(take_fault(SERVER_TRANSFER_FAULT_TELEPORTER)) {
		rollback(s, t, SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAULT,
			"portal search/build fault injected");
		return !server_transfer_active(s);
	}
	enum portal_teleporter_result r = portal_teleporter_step(s, t->job);
	/* Sol review 2 (A): PREPARING is bounded. A Teleporter that cannot make
	 * progress (every column it needs refused by the shared cap and nothing
	 * evictable) rolls the transfer back cleanly -- counted, visible, the
	 * source restored -- instead of holding the actor forever. */
	if(r == PORTAL_TELEPORTER_WORKING
	   && ++t->prepare_ticks > s_prepare_max_ticks) {
		s_prepare_timeouts++;
		rollback(s, t, SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAILED,
			"portal preparation timed out");
		return !server_transfer_active(s);
	}
	if(r == PORTAL_TELEPORTER_WORKING)
		return false;
	if(r == PORTAL_TELEPORTER_FAILED) {
		rollback(s, t, SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAILED,
			"portal search/build failed");
		return !server_transfer_active(s);
	}
	t->pose = *portal_teleporter_job_pose(t->job);
	portal_teleporter_destroy(t->job);
	t->job = NULL;
	/* Chunks first: the destination (a built portal) is durable before any
	 * record can point at it -- the legacy portal path's contract. */
	server_local_portal_diag_publish(s);
	if(!server_transfer_flush_owned_batch(s, dst)
	   || !server_world_checkpoint_chunks(dst) || !server_world_sync_regions(dst)) {
		rollback(s, t, SERVER_TRANSFER_ROLLBACK_CHECKPOINT,
			"destination chunk checkpoint failed");
		return !server_transfer_active(s);
	}
	/* Astra a2t-0927: reserve the client/GPU capacity for the landing BEFORE
	 * membership; publish membership only once the client's landing is
	 * ready. Both are bounded round trips (lane B's client_admission.h). */
	if(t->actor_slot == ACTOR_SLOT_GUEST && clin_world_payloads_enabled()
	   && server_local_world_of(s, &s->player) == dst) {
		/* the guest joins player 0's world: the client holds and shows it
		 * already (a RESERVE for the primary slot is BAD_SLOT, a landing into
		 * a viewed world never prepares). The gate-1 bind at COMMIT is the
		 * whole client side. */
		t->joins_primary = true;
		t->state = SERVER_TRANSFER_COMMIT_READY;
		return false;
	}
	t->state = SERVER_TRANSFER_CAPACITY;
	t->wait_ticks = 0;
	t->request_sent = s->transfer_cap_release_n == 0
		&& send_capacity(s, t, CLIN_CAP_OP_RESERVE);
	return false;
}

/* The guest's client side of an A2-T COMMIT (world payloads ON): lane B's
 * gate 1 -- an in-place guest WORLD_RESET for the destination slot with
 * guest_view 1, which binds the guest's view to the prepared slot (or, for
 * player 0's world at its current generation, binds it back into player 0's
 * world). Player 0's world, body and windows are untouched.
 */
static void guest_transfer_publish(struct server_local* s,
								   const struct server_transfer_txn* t) {
	struct server_world* dst = &s->worlds[t->dst_slot];
	/* The first post-install position must echo a token that no source-body
	 * sample could have carried. Do not change the transfer HOLD/stream radius. */
	if(++s->guest_actor->correction_seq == 0)
		s->guest_actor->correction_seq = 1;
	s->a2w_guest_presentation = true;
	a2w_guest_gate1(dst);
	server_transfer_stream_enable(s, dst->world_index); /* A2-W */
	fprintf(stderr, "[transfer] guest publish: slot %u gen %u (%s)\n",
			(unsigned)dst->world_index, (unsigned)dst->generation,
			t->joins_primary ? "join player 0's world" : "bind prepared slot");
}

static bool step_client_wait(struct server_local* s, struct server_transfer_txn* t) {
	if(!t->request_sent) {
		t->request_sent = t->state == SERVER_TRANSFER_CAPACITY
			? s->transfer_cap_release_n == 0
				&& send_capacity(s, t, CLIN_CAP_OP_RESERVE)
			: send_landing(s, t, false);
		if(t->state == SERVER_TRANSFER_LANDING && t->request_sent)
			t->landing_armed = true;
	}
	/* blocker 3: the ARM is ahead of the columns in the client FIFO (the
	 * client prepares the slot's context on it) */
	if(t->state == SERVER_TRANSFER_LANDING && t->landing_armed)
		landing_stream_step(s, t);
	if(++t->wait_ticks > SERVER_TRANSFER_CLIENT_WAIT_TICKS) {
		rollback(s, t, t->state == SERVER_TRANSFER_CAPACITY
					   ? SERVER_TRANSFER_ROLLBACK_CAPACITY_TIMEOUT
					   : SERVER_TRANSFER_ROLLBACK_LANDING_TIMEOUT,
			t->state == SERVER_TRANSFER_CAPACITY
					   ? "client capacity reply timed out"
						   : "client landing never became ready");
		return !server_transfer_active(s);
	}
	return false;
}

static bool step_commit(struct server_local* s, struct server_transfer_txn* t,
					 struct server_player* actor) {
	server_diag_set(SERVER_DIAG_COMMIT_MARKER);
	/* residency-1005/2 S3: the arrival commits only onto a RESIDENT body
	 * footprint in the destination (the teleporter search/build loaded the
	 * area; this proves it, and loads any column evicted since, one bounded
	 * ensure_populated step per tick). Past the client-wait bound the
	 * transaction rolls back -- a visible, retryable failure -- instead of
	 * committing a body onto unknown terrain. */
	if(!server_local_rz_landing_ready(s, &s->worlds[t->dst_slot], t->pose.x,
		   t->pose.z)) {
		if(++t->commit_wait_ticks > SERVER_TRANSFER_CLIENT_WAIT_TICKS) {
			rollback(s, t, SERVER_TRANSFER_ROLLBACK_LANDING_TIMEOUT,
				"the landing footprint never became resident");
			return !server_transfer_active(s);
		}
		return false;
	}
	/* The record. Host: level.dat's Player record names the
	 * destination (Beta EntityPlayer.writeEntityToNBT:329 Dimension). Guest:
	 * its own record (.Data.CaveWiiGuest) at the staged destination, through
	 * lane C's server_local_guest_commit_record, then level.dat (level-last;
	 * no journal, so no durable-marker guarantee -- Astra a2t-0927). */
	bool marker;
	if(take_fault(SERVER_TRANSFER_FAULT_COMMIT_MARKER))
		marker = false;
	else if(t->actor_slot == ACTOR_SLOT_HOST) {
		/* M1: the write mutates the in-memory record before the save can
		 * fail; a refused save puts Pos/Rotation/Dimension and the modified
		 * flag back verbatim, so no later bare level_archive_save persists
		 * the destination while the host is still in the source. */
		struct level_archive_player_record before;
		server_diag_set(SERVER_DIAG_SAVE_SNAPSHOT);
		bool snap = level_archive_player_record_snapshot(&s->level, &before);
		marker = snap;
		if(marker) {
			server_diag_set(SERVER_DIAG_SAVE_WRITE_PLAYER);
			marker = level_archive_write_player(
				&s->level, (vec3) {t->pose.x, t->pose.y, t->pose.z},
				(vec2) {t->pose.rx, t->pose.ry}, NULL, t->dst_dim);
		}
		if(marker) {
			server_diag_set(SERVER_DIAG_SAVE_ENTRY);
			marker = level_archive_save(&s->level);
		}
		server_diag_set(SERVER_DIAG_SAVE_RETURN);
		if(!marker && snap && !level_archive_player_record_restore(&s->level, &before))
			fprintf(stderr, "[transfer] request %u: Player record restore "
							"FAILED after a refused save\n", (unsigned)t->id);
	} else
		marker = server_local_guest_commit_record(
			s, t->dst_dim, t->pose.x, t->pose.y, t->pose.z, t->pose.rx,
			t->pose.ry);
	if(!marker) {
		rollback(s, t, t->actor_slot == ACTOR_SLOT_HOST
					   ? SERVER_TRANSFER_ROLLBACK_HOST_RECORD
					   : SERVER_TRANSFER_ROLLBACK_GUEST_RECORD,
			t->actor_slot == ACTOR_SLOT_HOST
					   ? "the level.dat record failed"
						   : "the guest's record (level.dat) failed");
		return !server_transfer_active(s);
	}
	server_diag_set(SERVER_DIAG_COMMIT_BODY);
	commit(s, t, actor);
	server_diag_set(SERVER_DIAG_COMMIT_CAPACITY);
	if(t->capacity_reserved) {
		/* M2(d): the slot's committed reservation lives until the slot is
		 * vacated or reserved again. A refused COMMIT leaves the client entry
		 * RESERVED, which the same ticket still releases. */
		(void)send_capacity(s, t, CLIN_CAP_OP_COMMIT);
		server_transfer_on_world_vacated(s, t->dst_slot);
		s->transfer_slot_ticket[t->dst_slot] = t->capacity_ticket;
	}
	/* ITEM 4: a world-bound client (it answered the reservation) is told
	 * about the host's new world; a single-world client (DISABLED) keeps the
	 * server-side-only transfer it always had. */
	server_diag_set(SERVER_DIAG_COMMIT_PUBLISH);
	if(t->actor_slot == ACTOR_SLOT_HOST && t->capacity_reserved)
		server_local_host_transfer_publish(s, t->src_slot);
	else if(t->actor_slot == ACTOR_SLOT_GUEST
			&& (t->capacity_reserved || t->joins_primary))
		guest_transfer_publish(s, t);
	server_diag_set(SERVER_DIAG_COMMIT_HOLD_END);
	if(t->hold_sent)
		t->hold_end_pending = !send_transfer_hold(t, false);
	return !server_transfer_active(s);
}

void server_transfer_on_capacity_reply(struct server_local* s, uint32_t request_id,
									   uint8_t result, uint32_t ticket) {
	if(!s)
		return;
	/* RELEASE replies carry request id 0 (never a transaction's). */
	if(!request_id)
		return;
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* t = &s->transfer[i];
		if(t->id != request_id)
			continue;
		if(!t->open || t->state != SERVER_TRANSFER_CAPACITY) {
			/* M2(b): a reply for a transaction that is no longer waiting
			 * (timed out, rolled back, committed). An OK for a ticket this
			 * transaction never held is a reservation nobody will use:
			 * release it now. Its own ticket's COMMIT/RELEASE echo is not. */
			if(result == CLIN_CAP_OK && ticket && ticket != t->capacity_ticket) {
				fprintf(stderr, "[transfer] request %u: stale capacity OK "
								"(ticket %u) released\n",
						(unsigned)request_id, (unsigned)ticket);
				queue_release(s, ticket);
			}
			return;
		}
		if(result == CLIN_CAP_OK) {
			t->capacity_reserved = true;
			t->capacity_ticket = ticket;
			/* Blocker 3: host and guest alike wait for the client's landing.
			 * The client prepares an unviewed context for the destination on
			 * the ARM, this transaction streams the arrival core into it, and
			 * the client reports READY by itself once the core is installed
			 * (no view needs to show the world before COMMIT). The host's
			 * stamped WORLD_RESET then JOINS that prepared context instead of
			 * rebuilding it, so the host arrives with its core terrain. */
			t->state = SERVER_TRANSFER_LANDING;
			t->wait_ticks = 0;
			t->landing_sent_mask = 0;
			t->request_sent = send_landing(s, t, false);
			t->landing_armed = t->request_sent;
		} else if(result == CLIN_CAP_DISABLED) {
			/* a single-world client: no payload to reserve, no landing report
			 * will ever come -- the server-side transfer proceeds */
			t->capacity_disabled = true;
			t->state = SERVER_TRANSFER_COMMIT_READY;
		} else {
			static const char* const names[] = {
				"ok", "disabled", "bad slot", "busy", "no client world context",
				"no MEM1", "no MEM2"};
			char why[96];
			snprintf(why, sizeof(why), "client refused the landing capacity (%s)",
					 result < 7 ? names[result] : "unknown");
			fprintf(stderr, "[transfer] request %u: %s\n", t->id, why);
			struct server_player* ra = txn_actor(s, t);
			if(ra)
				server_local_actor_hud_message(s, ra, "Cannot open the other dimension now");
			t->hud_shown = false; /* the refusal above is the notice */
			enum server_transfer_rollback_reason reason =
				result == CLIN_CAP_BAD_SLOT ? SERVER_TRANSFER_ROLLBACK_CLIENT_BAD_SLOT :
				result == CLIN_CAP_BUSY ? SERVER_TRANSFER_ROLLBACK_CLIENT_BUSY :
				result == CLIN_CAP_NO_CONTEXT ? SERVER_TRANSFER_ROLLBACK_CLIENT_NO_CONTEXT :
				result == CLIN_CAP_NO_MEM1 ? SERVER_TRANSFER_ROLLBACK_CLIENT_NO_MEM1 :
				result == CLIN_CAP_NO_MEM2 ? SERVER_TRANSFER_ROLLBACK_CLIENT_NO_MEM2 :
				SERVER_TRANSFER_ROLLBACK_CLIENT_REFUSED;
			rollback(s, t, reason, why);
		}
		return;
	}
	/* M2(b): no transaction carries this id any more (its actor slot has
	 * opened a newer one): an OK is a reservation nobody will use. */
	if(result == CLIN_CAP_OK && ticket) {
		fprintf(stderr, "[transfer] request %u: capacity OK for a closed request "
						"(ticket %u) released\n",
				(unsigned)request_id, (unsigned)ticket);
		queue_release(s, ticket);
	}
}

void server_transfer_on_landing_ready(struct server_local* s, uint32_t token,
									  uint8_t slot, uint32_t generation) {
	if(!s)
		return;
	for(size_t i = 0; i < 2; i++) {
		struct server_transfer_txn* t = &s->transfer[i];
		if(!t->open || t->id != token || t->state != SERVER_TRANSFER_LANDING)
			continue;
		struct server_world* dst = &s->worlds[t->dst_slot];
		if(slot != t->dst_slot || generation != dst->generation)
			return; /* a stale landing: the client will re-report or time out */
		t->state = SERVER_TRANSFER_COMMIT_READY;
		return;
	}
}

/* ===== death / respawn from a non-routed world (host) ==================== */
bool server_transfer_respawn_host(struct server_local* s) {
	struct server_world* aw = server_local_world_of(s, &s->player);
	struct server_world* ow = &s->worlds[0];
	if(!aw || aw == ow || !server_local_world_live(ow))
		return false;
	if(server_transfer_active(s))
		return false;
	if(s->player.has_death_pos) {
		s->player.x = s->player.death_x;
		s->player.y = s->player.death_y;
		s->player.z = s->player.death_z;
	}
	struct server_respawn_plan plan;
	if(!server_local_prepare_respawn_plan(s, &plan))
		return false;
	if(!server_local_close_active_inventory_for(s, &s->player))
		return false;
	server_local_set_paused(s, false, -1); /* V6 F1: the shared pause seam */
	/* Beta EntityPlayer.onDeath -> dropAllItems: the drops fall where the
	 * lethal hit landed, IN THE DEATH WORLD (Astra A2 sec. 2: preserve
	 * death-world drops). Allocate before mutation (the Overworld policy). */
	vec3 at = {s->player.x, s->player.y, s->player.z};
	/* G6: InventoryPlayer.dropAllItems order, Beta spawn + death scatter. */
	for(int k = 0; k < INVENTORY_SIZE; k++) {
		const int i = server_local_death_drop_order[k];
		struct item_data item;
		inventory_get_slot(&s->player.inventory, i, &item);
		if(item.id == 0)
			continue;
		struct entity* drop = server_local_spawn_death_item_w(aw,
			&s->player, s->player.x, s->player.y, s->player.z, &item);
		if(!drop)
			continue;
		inventory_clear_slot(&s->player.inventory, i);
		clin_rpc_send(&(struct client_rpc) {
			.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot.window = WINDOWC_INVENTORY,
			.payload.inventory_slot.slot = i,
			.payload.inventory_slot.item = s->player.inventory.items[i]});
	}
	struct item_data picked;
	if(inventory_get_picked_item(&s->player.inventory, &picked)) {
		struct entity* drop = server_local_spawn_item_w(aw, at, &picked, false);
		if(drop) {
			drop->data.item.pickup_delay = 40;
			inventory_clear_picked_item(&s->player.inventory);
			clin_rpc_send(&(struct client_rpc) {
				.type = CRPC_INVENTORY_SLOT,
				.payload.inventory_slot.window = WINDOWC_INVENTORY,
				.payload.inventory_slot.slot = SPECIAL_SLOT_PICKED_ITEM,
				.payload.inventory_slot.item = s->player.inventory.picked_item});
		}
	}
	s->player.has_death_pos = false;
	/* the fresh body (Beta builds a new EntityPlayer) at the Overworld plan */
	server_local_respawn_fresh_body(s, &plan);
	/* COMMIT of the respawn transaction: membership switches to the Overworld
	 * in the same step; no world is torn down, both stay live. */
	s->player.world_slot = 0;
	s->player.dimension = WORLD_DIM_OVERWORLD;
	s->player.added_to_chunk = false;
	server_local_refresh_player_membership(s);
	s->player.finished_loading = true; /* no client world switch in this build */
	++s->transfer_epoch; /* the respawn is a transition of this session too */
	fprintf(stderr, "[transfer] host respawn: died in slot %u (dim %d), fresh body in "
					"the Overworld at %.1f %.1f %.1f\n",
			aw->world_index, (int)aw->dimension, s->player.x, s->player.y,
			s->player.z);
	/* the death world stays LIVE with its drops (design sec. 1: an unoccupied
	 * dimension keeps its clocks and contents; retirement is an explicit,
	 * separate decision -- server_local_world_retire) */
	return true;
}
