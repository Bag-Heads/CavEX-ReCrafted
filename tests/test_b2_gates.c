/* Split-dims B2 activation gates (lane B, b2-gates-0926), client consumer,
 * through the real clin_process / clin_update_drain:
 *
 *  G1 in-place guest-world WORLD_RESET (payload.world_reset.guest_world):
 *     with world-bound payloads ON it replaces only the guest world's slot --
 *     new context/generation, that payload's terrain/entities/signs cleared,
 *     the guest view re-bound -- while player 0's world, body, primary slot
 *     and load state are untouched. Flag OFF (or ON with no guest view): the
 *     message is dropped, nothing changes.
 *  G2 world-addressed terrain resend: a failed install into a non-primary
 *     world raises its marker; clin_update offers ONE request naming the world
 *     to the registered sender (retrying a refused send, never duplicating an
 *     accepted one); the server's STREAM_RESET stamped for that world
 *     re-streams that payload in place (same identity, fresh epoch), clears
 *     the marker, leaves player 0's world alone; a stale-stamped STREAM_RESET
 *     is ignored.
 *
 *  F1-F7 (Fable B2-GATES review): the guest's first move into an empty slot
 *     (guest_view), re-READY body into the guest's store, lease owner, leased
 *     predecessors, install-failure retry on both gates, unsolicited and
 *     post-switch stamped STREAM_RESETs.
 *
 * Producers are the lane-A/C handoff (SRPC_WORLD_RESEND + the guest reset);
 * the messages here are the producer contract. Non-aborting: ok/FAIL lines. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "chunk.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "game/sign_text_store.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/piston_moving_transport.h"
#include "network/server_interface.h"
#include "world.h"

#define LOCAL_EID 77

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static struct client_ctx_registry* reg;

static void world_reset(uint8_t index, uint32_t gen, bool guest) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID, .world_index = index,
			.world_generation = gen, .guest_world = guest}});
}

static struct clin_world_payload payload(unsigned slot) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	return p;
}

static void column_at(w_coord_t x, w_coord_t z, uint8_t id,
	uint8_t index, uint32_t gen) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(size_t lx = 0; lx < CHUNK_SIZE; lx++)
		for(size_t lz = 0; lz < CHUNK_SIZE; lz++)
			slot->ids[(lx * CHUNK_SIZE + lz) * WORLD_HEIGHT] = id;
	clin_process(&(struct client_rpc) {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = gen,
		.payload.chunk = {.x = x, .y = 0, .z = z, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}});
}

static void column(w_coord_t x, uint8_t id, uint8_t index, uint32_t gen) {
	column_at(x, 0, id, index, gen);
}

static bool has_chunk(unsigned slot, w_coord_t x) {
	return world_find_chunk(payload(slot).terrain, x, 0, 0) != NULL;
}

static void spawn_item(uint32_t id, uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_SPAWN_ITEM,
		.world_index = index, .world_generation = gen,
		.payload.spawn_item = {.entity_id = id,
			.item = {.id = 1, .durability = 0, .count = 1},
			.pos = {0.5F, 70.0F, 0.5F}}});
}

static bool has_entity(unsigned slot, uint32_t id) {
	struct entity** pp = dict_entity_get(payload(slot).entities, id);
	return pp && *pp;
}

static void flag(bool on) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(clin_world_payloads_set_enabled(false));
	assert(clin_world_payloads_set_enabled(on));
}

/* Guest view (1) on the overworld (slot 0, gen 100), player 0 (view 0) in
 * the Nether (slot 1, gen 200, primary). */
static void two_worlds(void) {
	flag(true);
	world_reset(0, 100, false);
	world_reset(1, 200, false);
	assert(clin_world_payload_bind_view(0, 1));
}

static void test_guest_reset(void) {
	/* Flag OFF: a guest-world reset is dropped -- nothing changes. */
	flag(false);
	world_reset(0, 50, false);
	column(0, BLOCK_STONE, 0, 50);
	struct client_world_context* before = client_ctx_registry_current(reg, 0);
	world_reset(0, 51, true);
	check(client_ctx_registry_current(reg, 0) == before
			  && before->server_generation == 50 && has_chunk(0, 0),
		  "G1 flag OFF: a guest-world reset is dropped");

	two_worlds();
	column(0, BLOCK_STONE, 0, 100);
	column(0, BLOCK_NETHERRACK, 1, 200);
	spawn_item(901, 0, 100);
	spawn_item(902, 1, 200);
	struct entity* p0_body = gstate.players[0].local_player;
	bool loaded = gstate.world_loaded, pending = gstate.world_switch_pending;
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);

	world_reset(0, 101, true); /* the guest respawns into a new overworld */
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	check(ow && ow->server_generation == 101 && (ow->bound_views & 2u),
		  "G1 the guest slot gets a new context, still bound to view 1");
	check(!has_chunk(0, 0) && !has_entity(0, 901),
		  "G1 the guest world's terrain and entities are cleared");
	check(client_ctx_registry_current(reg, 1) == nether
			  && (nether->bound_views & 1u) && has_chunk(1, 0)
			  && has_entity(1, 902),
		  "G1 player 0's world (context, view, terrain, entities) untouched");
	check(gstate.players[0].local_player == p0_body
			  && clin_world_payload_primary_slot() == 1
			  && gstate.world_loaded == loaded
			  && gstate.world_switch_pending == pending,
		  "G1 player 0's body, primary slot and load state untouched");
	column(0, BLOCK_DIRT, 0, 101);
	column(0, BLOCK_STONE, 0, 100);
	check(has_chunk(0, 0)
			  && world_get_block(payload(0).terrain, 0, 0, 0).type == BLOCK_DIRT,
		  "G1 the new generation streams, the old one is STALE");

	/* ON but no view shows that slot as the guest's: dropped. */
	assert(client_ctx_unbind_view(client_ctx_registry_current(reg, 0), 1));
	ow = client_ctx_registry_current(reg, 0);
	world_reset(0, 102, true);
	check(client_ctx_registry_current(reg, 0) == ow,
		  "G1 a guest-world reset for a slot no guest view shows is dropped");
	/* Never for player 0's own slot. */
	world_reset(1, 201, true);
	check(client_ctx_registry_current(reg, 1) == nether,
		  "G1 a guest-world reset naming player 0's slot is dropped");
}

static unsigned sends, refuse;
static unsigned last_index;
static uint32_t last_gen;
static bool sender(unsigned index, uint32_t gen) {
	sends++;
	last_index = index;
	last_gen = gen;
	if(refuse) {
		refuse--;
		return false;
	}
	return true;
}

static void test_world_resend(void) {
	two_worlds();
	clin_world_payload_set_resend_sender(sender);
	column(0, BLOCK_STONE, 0, 100);
	column(0, BLOCK_NETHERRACK, 1, 200);
	/* A sidecar the guest world cannot store: an install failure. */
	struct piston_moving_state st = {.stored_id = BLOCK_STONE, .facing = 1};
	struct client_rpc m = {.type = CRPC_PISTON_MOVING, .world_index = 0,
		.world_generation = 100,
		.payload.piston_moving = {.x = 0, .y = -1, .z = 0}};
	assert(piston_moving_wire_encode(&m.payload.piston_moving.state, &st));
	clin_process(&m);
	check(clin_world_payload_resend_pending(0)
			  && !clin_world_payload_resend_requested(0),
		  "G2 the guest world's hole raises its marker");

	sends = 0;
	refuse = 1;
	clin_update_drain();
	check(sends == 1 && !clin_world_payload_resend_requested(0),
		  "G2 a refused request is not recorded");
	clin_update_drain();
	check(sends == 2 && last_index == 0 && last_gen == 100
			  && clin_world_payload_resend_requested(0),
		  "G2 the request names the world {0, gen 100}; accepted");
	clin_update_drain();
	check(sends == 2, "G2 an accepted request is never duplicated");

	/* A stale-stamped STREAM_RESET is ignored. */
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 99});
	check(has_chunk(0, 0) && clin_world_payload_resend_pending(0),
		  "G2 a STREAM_RESET for another generation is ignored");

	/* The server's per-world confirmation. */
	struct client_world_context* before = client_ctx_registry_current(reg, 0);
	uint32_t epoch = before->generation;
	bool loaded = gstate.world_loaded, switching = gstate.world_switch_pending;
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 100});
	struct client_world_context* after = client_ctx_registry_current(reg, 0);
	check(after && after->server_generation == 100 && after->generation != epoch
			  && (after->bound_views & 2u),
		  "G2 the re-stream keeps the identity, fresh epoch, view kept");
	check(!has_chunk(0, 0) && !clin_world_payload_resend_pending(0)
			  && !clin_world_payload_resend_requested(0),
		  "G2 the payload re-streams from empty and the marker clears");
	check(has_chunk(1, 0) && clin_world_payload_primary_slot() == 1
			  && gstate.world_loaded == loaded
			  && gstate.world_switch_pending == switching,
		  "G2 player 0's world and load state untouched");
	column(0, BLOCK_STONE, 0, 100);
	check(has_chunk(0, 0), "G2 the re-streamed column installs");
	sends = 0;
	clin_update_drain();
	check(sends == 0, "G2 no request once confirmed");
	clin_world_payload_set_resend_sender(NULL);
}

/* ---- Fable B2-GATES review fixes (F1-F7) -------------------------------- */

static void guest_reset_view(uint8_t index, uint32_t gen, uint8_t view) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID, .world_index = index,
			.world_generation = gen, .guest_world = true,
			.guest_view = view}});
}

/* Fill the context pool with leased (retiring) entries of the slot's world
 * so the next install fails; the slot's current context is leased too. */
static struct client_world_context* pinned[CLIENT_CTX_POOL];
static unsigned n_pinned;
static void fill_pool(unsigned slot, int dimension, uint32_t gen) {
	for(;;) {
		struct client_world_context* c = client_ctx_registry_current(reg, slot);
		assert(c && n_pinned < CLIENT_CTX_POOL);
		assert(client_ctx_lease_acquire(c, CLIENT_CTX_LEASE_MESHER));
		pinned[n_pinned++] = c;
		if(client_ctx_registry_in_use(reg) >= CLIENT_CTX_POOL)
			break;
		assert(client_ctx_registry_install(reg, dimension, slot, gen));
	}
}
static void unpin_all(void) {
	for(unsigned i = 0; i < n_pinned; i++)
		assert(client_ctx_registry_release(reg, pinned[i],
										   CLIENT_CTX_LEASE_MESHER));
	n_pinned = 0;
}

/* An attached guest session the READY handler will accept. */
static void attach_guest(void) {
	struct client_session* g = &gstate.players[1];
	g->local_bound = true;
	g->local_ticket = 5;
	g->local_generation = 9;
	g->local_phase = CLIENT_GUEST_ATTACHED;
	g->local_gameplay_ready = true;
}
static void guest_ready(uint32_t eid) {
	clin_process(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 5, .generation = 9, .owner = 1,
			.result = LOCAL_GUEST_GAMEPLAY_READY, .entity_id = eid,
			.health = MAX_PLAYER_HEALTH,
			.x = 0.5, .y = 70.0, .z = 0.5}});
}
static void detach_guest(void) {
	flag(false); /* frees the extra store, drops the alias */
	gstate.players[1] = (struct client_session) {0};
}

static void public_guest_status(int result, uint64_t ticket, uint64_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket, .generation = gen,
			.owner = 1, .result = result,
			.entity_id = result == LOCAL_GUEST_GAMEPLAY_READY ? 570 : 0,
			.health = result == LOCAL_GUEST_GAMEPLAY_READY ? MAX_PLAYER_HEALTH : 0,
			.x = 0.5, .y = 70.0, .z = 0.5}});
}

static uint64_t public_guest_join(uint64_t gen) {
	struct client_session* g = &gstate.players[1];
	check(client_session_request_local_join(g), "public guest Join accepted");
	uint64_t ticket = g->local_ticket;
	public_guest_status(LOCAL_GUEST_OFFER, ticket, gen);
	public_guest_status(LOCAL_GUEST_INSTALLED, ticket, gen);
	public_guest_status(LOCAL_GUEST_ATTACHED, ticket, gen);
	public_guest_status(LOCAL_GUEST_GAMEPLAY_READY, ticket, gen);
	return ticket;
}

/* Production first admission sends READY while both players share the first
 * world. No guest-world reset has occurred yet to bind view 1. */
static void test_initial_guest_view_survives_host_switch(void) {
	flag(true);
	world_reset(0, 100, false);
	gstate.players[1] = (struct client_session) {0};
	uint64_t first = public_guest_join(9);
	check(clin_world_view_slot(1) == 0 && has_entity(0, 570),
		  "initial guest READY binds view 1 to its body world");
	world_reset(1, 200, false);
	check(clin_world_view_slot(0) == 1 && clin_world_view_slot(1) == 0
			  && gstate.players[1].local_player && has_entity(0, 570)
			  && gstate.players[1].local_gameplay_ready,
		  "host switch preserves the guest view, body and gameplay in slot 0");
	check(client_session_request_local_leave(&gstate.players[1]),
		  "public guest Leave accepted");
	public_guest_status(LOCAL_GUEST_LEFT, first, 9);
	check(clin_world_view_slot(1) == -1 && !gstate.players[1].local_player,
		  "settled Leave unbinds view 1 and retires its body");
	uint64_t second = public_guest_join(10);
	check(second != first && clin_world_view_slot(1) == 1
			  && gstate.players[1].local_player
			  && gstate.players[1].local_player->world == payload(1).terrain,
		  "later Join binds the new guest to the host's current world");
	detach_guest();
}

static void test_initial_guest_bind_failure_retries(void) {
	flag(true); /* no primary context: simulate failed WORLD_RESET install */
	gstate.players[1] = (struct client_session) {0};
	uint64_t ticket = public_guest_join(11);
	check(!gstate.players[1].local_player
			  && !gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(1) == -1,
		  "READY cannot admit a body when the primary context cannot bind");
	world_reset(0, 102, false);
	check(!gstate.players[1].local_bound && !gstate.players[1].local_player,
		  "fresh primary reset retires the failed guest admission");
	(void)ticket;
	public_guest_join(12);
	check(gstate.players[1].local_gameplay_ready
			  && gstate.players[1].local_player
			  && clin_world_view_slot(1) == 0,
		  "new Join binds the body after the primary context recovers");
	detach_guest();
}

static void transfer_hold(unsigned slot, uint64_t generation,
	uint32_t request_id, bool begin) {
	clin_process(&(struct client_rpc){.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = slot,
			.actor_generation = generation, .request_id = request_id,
			.begin = begin}});
}

static void transfer_hold_restored(unsigned slot, uint64_t generation,
	uint32_t request_id, uint8_t radius) {
	clin_process(&(struct client_rpc){.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = slot,
			.actor_generation = generation, .request_id = request_id,
			.restored_source = true, .restore_radius = radius}});
}

static void test_rollback_hold_waits_for_resend(void) {
	two_worlds();
	attach_guest();
	guest_reset_view(0, 101, 1);
	guest_ready(556);
	check(gstate.players[1].local_player
			  && gstate.players[1].local_player->world == payload(0).terrain,
		  "rollback source body is in the guest's own world");
	clin_world_payload_set_resend_sender(sender);
	sends = refuse = 0;
	transfer_hold(1, 9, 27, true);
	transfer_hold(1, 9, 27, false); /* ordinary COMMIT END is immediate */
	check(!clin_transfer_actor_held(1), "ordinary END keeps the COMMIT path immediate");
	transfer_hold(1, 9, 28, true);
	chunk_test_fail_alloc_after(1);
	column(0, BLOCK_STONE, 0, 101);
	chunk_test_fail_alloc_after(0);
	check(clin_world_payload_resend_pending(0) && !has_chunk(0, 0),
		  "failed source CHUNK leaves missing collision and raises scoped resend");
	clin_process(&(struct client_rpc){.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 5, .generation = 9, .owner = 1,
			.result = LOCAL_GUEST_CORRECTION, .correction_seq = 1,
			.correction_flags = PLAYER_CORRECTION_POSITION_ONLY,
			.x = 0.5, .y = 70.0, .z = 0.5}});
	transfer_hold_restored(1, 9, 28, 3);
	check(clin_transfer_actor_held(1),
		  "rollback END cannot release into failed source CHUNK");
	clin_update_drain();
	check(sends == 1 && clin_world_payload_resend_requested(0)
			  && clin_transfer_actor_held(1),
		  "authentic scoped resend request retains the owner hold");
	clin_process(&(struct client_rpc){.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 101});
	clin_update_drain();
	check(!clin_world_payload_resend_pending(0) && clin_transfer_actor_held(1),
		  "resend reset alone cannot release before restored columns install");
	for(w_coord_t dz = -3; dz <= 3; dz++)
		for(w_coord_t dx = -3; dx <= 3; dx++) {
			if(dx == 3 && dz == 3) continue;
			column_at(dx * CHUNK_SIZE, dz * CHUNK_SIZE, BLOCK_STONE, 0, 101);
		}
	clin_update_drain();
	check(clin_transfer_actor_held(1),
		  "one missing source-ring column still retains rollback hold");
	column_at(3 * CHUNK_SIZE, 3 * CHUNK_SIZE, BLOCK_STONE, 0, 101);
	clin_update_drain();
	check(!clin_transfer_actor_held(1),
		  "restored radius-three terrain releases without waiting for radius four");
	clin_world_payload_set_resend_sender(NULL);
	detach_guest();
}

static void test_guest_correction_try_route(void) {
	flag(true);
	world_reset(0, 100, false);
	attach_guest();
	guest_ready(557);
	assert(gstate.players[1].local_player);
	struct client_rpc filler = {.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = 2}}; /* rejected on drain */
	unsigned queued = 0;
	while(queued < 1024 && clin_try_rpc_send(&filler)) queued++;
	check(queued > 0 && queued < 1024,
		  "guest correction test fills the real client inbox token pool");
	struct client_rpc correction = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 5, .generation = 9, .owner = 1,
			.result = LOCAL_GUEST_CORRECTION, .correction_seq = 1,
			.correction_flags = PLAYER_CORRECTION_POSITION_ONLY,
			.x = 0.5, .y = 70.0, .z = 0.5}};
	check(!clin_try_rpc_send_local(&correction, 1, 9),
		  "guest correction refuses full inbox promptly for server retry");
	clin_update_drain();
	check(clin_try_rpc_send_local(&correction, 1, 9),
		  "guest correction enqueues with generation once tokens return");
	clin_update_drain();
	check(gstate.players[1].player_sync.correction_seq == 1,
		  "retried tagged correction reaches the owning guest");
	detach_guest();
}

struct counted_send_probe {
	uint32_t* counter;
	unsigned consumed;
	bool published_before_consume;
};

static void counted_send_consume(const struct client_rpc* call, void* user) {
	struct counted_send_probe* probe = user;
	if(call->type != CRPC_TIME_SET) return;
	probe->consumed++;
	probe->published_before_consume =
		__atomic_load_n(probe->counter, __ATOMIC_ACQUIRE) == 1;
}

static void test_counted_try_send(void) {
	uint32_t accepted = 0;
	struct client_rpc filler = {.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = 2}};
	unsigned queued = 0;
	while(queued < 1024 && clin_try_rpc_send(&filler)) queued++;
	check(queued > 0 && queued < 1024,
		  "counted-send test fills the real client inbox token pool");
	struct client_rpc message = {.type = CRPC_TIME_SET};
	check(!clin_try_rpc_send_counted(&message, &accepted) && accepted == 0,
		  "refused counted send leaves accepted counter unchanged");
	clin_update_drain();
	struct counted_send_probe probe = {.counter = &accepted};
	clin_test_set_consume_observer(counted_send_consume, &probe);
	check(clin_try_rpc_send_counted(&message, &accepted) && accepted == 1,
		  "accepted counted send publishes exactly once");
	clin_update_drain();
	check(probe.consumed == 1 && probe.published_before_consume,
		  "accepted counter is visible before RPC consumption");
	clin_test_set_consume_observer(NULL, NULL);
}

static void test_host_rollback_hold_radius(void) {
	flag(true);
	world_reset(0, 100, false);
	assert(gstate.players[0].local_player);
	transfer_hold(0, 0, 51, true);
	clin_process(&(struct client_rpc){.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {0.5F, 70.0F, 0.5F},
			.correction_seq = 1,
			.correction_flags = PLAYER_CORRECTION_POSITION_ONLY}});
	transfer_hold_restored(0, 0, 51, 0);
	check(clin_transfer_actor_held(0),
		  "malformed rollback radius fails closed for host");
	transfer_hold_restored(0, 0, 51, 1);
	for(w_coord_t dz = -1; dz <= 1; dz++)
		for(w_coord_t dx = -1; dx <= 1; dx++) {
			if(dx == 1 && dz == 1) continue;
			column_at(dx * CHUNK_SIZE, dz * CHUNK_SIZE, BLOCK_STONE, 0, 100);
		}
	clin_update_drain();
	check(clin_transfer_actor_held(0),
		  "host stays held until its corrected source ring is complete");
	column_at(CHUNK_SIZE, CHUNK_SIZE, BLOCK_STONE, 0, 100);
	clin_update_drain();
	check(!clin_transfer_actor_held(0),
		  "host restored END releases on exact radius-one source ring");
}

static void test_transfer_hold_lifecycle(void) {
	flag(true);
	world_reset(0, 100, false);
	transfer_hold(0, 1, 20, true);
	check(!clin_transfer_actor_held(0), "host hold rejects a guest generation");
	transfer_hold(0, 0, 20, true);
	check(clin_transfer_actor_held(0), "host BEGIN freezes only its owner");
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_NETHER,
			.local_entity = 0, .world_index = 1,
			.world_generation = 200}});
	check(clin_transfer_actor_held(0), "host hold survives transfer WORLD_RESET");
	transfer_hold(0, 0, 19, false);
	check(clin_transfer_actor_held(0), "older END cannot release host hold");
	transfer_hold(0, 0, 20, false);
	transfer_hold(0, 0, 20, true);
	check(!clin_transfer_actor_held(0), "completed host token rejects late BEGIN");
	transfer_hold(0, 0, 21, true);
	transfer_hold(0, 0, 20, false);
	check(clin_transfer_actor_held(0), "old END cannot clear newer host hold");
	transfer_hold(0, 0, 21, false);
	check(!clin_transfer_actor_held(0), "matching host END releases hold");
	gstate.players[1] = (struct client_session){0};
	uint64_t ticket = public_guest_join(30);
	transfer_hold(1, 29, 25, true);
	check(!clin_transfer_actor_held(1), "guest BEGIN rejects stale generation");
	transfer_hold(1, 30, 25, true);
	check(clin_transfer_actor_held(1) && !clin_transfer_actor_held(0),
		  "guest BEGIN freezes only guest owner");
	guest_reset_view(0, 101, 1);
	check(clin_transfer_actor_held(1),
		  "guest hold survives its stamped guest-world reset");
	transfer_hold_restored(1, 30, 25, 1);
	check(clin_transfer_actor_held(1),
		  "pending restored END still holds guest before matching Leave");
	check(client_session_request_local_leave(&gstate.players[1]),
		  "held guest Leave accepted");
	public_guest_status(LOCAL_GUEST_LEFT, ticket, 30);
	check(!clin_transfer_actor_held(1), "settled Leave clears guest hold");
	transfer_hold(1, 30, 25, true);
	check(!clin_transfer_actor_held(1), "retired guest ignores stale BEGIN");
	uint64_t ticket2 = public_guest_join(31);
	transfer_hold(1, 31, UINT32_MAX, true);
	check(clin_transfer_actor_held(1), "new guest generation starts fresh token history");
	transfer_hold(1, 31, UINT32_MAX, false);
	transfer_hold(1, 31, 1, true);
	check(clin_transfer_actor_held(1), "guest request tokens advance across wrap");
	check(client_session_request_local_leave(&gstate.players[1]),
		  "second guest Leave accepted");
	public_guest_status(LOCAL_GUEST_LEFT, ticket2, 31);
	transfer_hold(0, 0, 22, true);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.local_entity = 0}});
	check(!clin_transfer_actor_held(0), "session teardown clears host hold");
	detach_guest();
}

/* F1 + F2 + F3: the guest's FIRST move into an empty slot. */
static void test_guest_first_move(void) {
	flag(true);
	world_reset(0, 100, false); /* player 0 on the overworld, slot 0 */
	attach_guest();
	guest_reset_view(1, 300, 0);
	check(!client_ctx_registry_current(reg, 1),
		  "F1 an empty slot with no guest_view is still dropped");
	guest_reset_view(1, 300, 1);
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	check(nether && nether->server_generation == 300
			  && nether->bound_views == 2u && clin_world_payload_ready(1),
		  "F1 guest_view=1 installs the empty slot and binds view 1");
	check((client_ctx_registry_current(reg, 0)->bound_views & 3u) == 1u
			  && clin_world_payload_primary_slot() == 0,
		  "F1 view 0 and player 0's primary slot untouched");
	check(payload(1).terrain->lease_owner != NULL
			  && payload(1).terrain->dimension == WORLD_DIM_NETHER,
		  "F3 the new slot's terrain leases against its owner");
	check(!gstate.players[1].local_gameplay_ready,
		  "F2 the guest's gameplay_ready is cleared (waits for re-READY)");
	guest_ready(555);
	check(gstate.players[1].local_player && has_entity(1, 555)
			  && !has_entity(0, 555),
		  "F2 the re-READY body lands in the guest slot's store");
	check(gstate.players[1].local_player->world == payload(1).terrain,
		  "F2 the guest body is bound to the guest slot's terrain");
	check(gstate.players[1].local_gameplay_ready,
		  "F2 the re-READY re-opens guest gameplay");
	guest_reset_view(0, 101, 1);
	check(client_ctx_registry_current(reg, 0)->server_generation == 100,
		  "F1 guest_view never re-generates player 0's slot");
	detach_guest();
}

/* F7: gate 1 with leases outstanding on the predecessor. */
static void test_guest_reset_leased(void) {
	two_worlds();
	struct client_world_context* pred = client_ctx_registry_current(reg, 0);
	assert(client_ctx_lease_acquire(pred, CLIENT_CTX_LEASE_GPU));
	unsigned in_use = client_ctx_registry_in_use(reg);
	world_reset(0, 101, true);
	struct client_world_context* succ = client_ctx_registry_current(reg, 0);
	check(succ && succ != pred && succ->server_generation == 101
			  && succ->bound_views == 2u,
		  "F7 leased predecessor: the successor installs, view 1 moves");
	check(pred->retiring && pred->bound_views == 0
			  && client_ctx_registry_owns(reg, pred)
			  && client_ctx_registry_in_use(reg) == in_use + 1,
		  "F7 the leased predecessor stays owned, unbound");
	assert(client_ctx_registry_release(reg, pred, CLIENT_CTX_LEASE_GPU));
	check(!client_ctx_registry_owns(reg, pred)
			  && client_ctx_registry_in_use(reg) == in_use,
		  "F7 its lease release reclaims it");
}

/* F7 + F2: gate 1 install failure -> retried; the body waits. */
static void test_guest_reset_install_failure(void) {
	two_worlds();
	attach_guest();
	fill_pool(0, WORLD_DIM_OVERWORLD, 100);
	assert(clin_world_payload_bind_view(0, 1));
	uint32_t failures = clin_world_payload_install_failures(0);
	world_reset(0, 101, true);
	check(!client_ctx_registry_current(reg, 0) && !clin_world_payload_ready(0)
			  && clin_world_payload_install_failures(0) == failures + 1,
		  "F7 gate-1 install failure: no context, not ready, counted");
	guest_ready(556);
	check(!gstate.players[1].local_player && !has_entity(1, 556),
		  "F2 no body in player 0's store while the guest slot is retried");
	clin_update_drain();
	check(!client_ctx_registry_current(reg, 0),
		  "F4 still no room: the install waits");
	unpin_all();
	clin_update_drain();
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	check(ow && ow->server_generation == 101 && ow->bound_views == 2u
			  && clin_world_payload_ready(0)
			  && payload(0).terrain->lease_owner != NULL,
		  "F4 the retry installs gen 101, binds view 1, slot ready");
	detach_guest();
}

static void test_leave_clears_pending_reinstall_view(void) {
	two_worlds();
	gstate.players[1] = (struct client_session){0};
	uint64_t first = public_guest_join(40);
	fill_pool(0, WORLD_DIM_OVERWORLD, 100);
	assert(clin_world_payload_bind_view(0, 1));
	guest_reset_view(0, 101, 0);
	check(!client_ctx_registry_current(reg, 0),
		  "guest gate-1 view claim waits for context capacity");
	check(client_session_request_local_leave(&gstate.players[1]),
		  "guest Leave during gate-1 retry accepted");
	public_guest_status(LOCAL_GUEST_LEFT, first, 40);
	unpin_all();
	clin_update_drain();
	struct client_world_context* old = client_ctx_registry_current(reg, 0);
	check(old && !(old->bound_views & 2u) && clin_world_view_slot(1) == -1,
		  "retired guest's pending reinstall cannot rebind view 1");
	(void)public_guest_join(41);
	check(clin_world_view_slot(1) == 1
			  && gstate.players[1].local_player
			  && gstate.players[1].local_player->world == payload(1).terrain,
		  "later guest joins current host world after retired retry");
	detach_guest();
}

static void raise_marker(void) {
	struct piston_moving_state st = {.stored_id = BLOCK_STONE, .facing = 1};
	struct client_rpc m = {.type = CRPC_PISTON_MOVING, .world_index = 0,
		.world_generation = 100,
		.payload.piston_moving = {.x = 0, .y = -1, .z = 0}};
	assert(piston_moving_wire_encode(&m.payload.piston_moving.state, &st));
	clin_process(&m);
	assert(clin_world_payload_resend_pending(0));
}

/* F5: a stamped STREAM_RESET with no request outstanding is ignored. */
static void test_unsolicited_stream_reset(void) {
	two_worlds();
	column(0, BLOCK_STONE, 0, 100);
	const uint32_t epoch = client_ctx_registry_current(reg, 0)->generation;
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 100});
	check(client_ctx_registry_current(reg, 0)->generation == epoch && has_chunk(0, 0),
		  "F5 unsolicited stamped STREAM_RESET (no marker) is ignored");
	raise_marker(); /* marker raised, but no sender: never requested */
	clin_update_drain();
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 100});
	check(client_ctx_registry_current(reg, 0)->generation == epoch && has_chunk(0, 0)
			  && clin_world_payload_resend_pending(0),
		  "F5 a raised but never-requested marker ignores it too");
}

/* F4 + F7: gate 2 successor install failure keeps the marker and retries. */
static void test_stream_reset_install_failure(void) {
	two_worlds();
	clin_world_payload_set_resend_sender(sender);
	column(0, BLOCK_STONE, 0, 100);
	raise_marker();
	sends = refuse = 0;
	clin_update_drain();
	assert(sends == 1 && clin_world_payload_resend_requested(0));
	fill_pool(0, WORLD_DIM_OVERWORLD, 100);
	assert(clin_world_payload_bind_view(0, 1));
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 100});
	check(!client_ctx_registry_current(reg, 0)
			  && clin_world_payload_resend_pending(0)
			  && !clin_world_payload_resend_requested(0),
		  "F4 install failure: marker KEPT, request re-armed");
	clin_update_drain();
	check(sends == 1, "F4 no request while the slot has no context");
	unpin_all();
	clin_update_drain();
	struct client_world_context* ctx = client_ctx_registry_current(reg, 0);
	check(ctx && ctx->server_generation == 100 && ctx->bound_views == 2u,
		  "F4 the retry installs the same identity and re-binds view 1");
	check(sends == 2 && last_index == 0 && last_gen == 100
			  && clin_world_payload_resend_requested(0),
		  "F4 a fresh request follows the retried install");
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 100});
	check(!clin_world_payload_resend_pending(0),
		  "F4 its confirmation clears the marker");
	clin_world_payload_set_resend_sender(NULL);
}

/* F6: request in flight, then player 0 switches INTO that slot. */
static void test_reply_after_primary_switch(void) {
	two_worlds();
	clin_world_payload_set_resend_sender(sender);
	raise_marker();
	sends = refuse = 0;
	clin_update_drain();
	assert(sends == 1 && clin_world_payload_resend_requested(0));
	world_reset(0, 100, false); /* player 0 moves into slot 0 */
	assert(clin_world_payload_primary_slot() == 0);
	clin_test_arm_stream_reset(); /* the primary awaits ITS recovery reset */
	/* The pool may reuse an entry's address: compare epochs. */
	const uint32_t epoch = client_ctx_registry_current(reg, 0)->generation;
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET,
		.world_index = 0, .world_generation = 100});
	check(client_ctx_registry_current(reg, 0)->generation == epoch,
		  "F6 the in-flight world reply is not taken as primary recovery");
	clin_process(&(struct client_rpc) {.type = CRPC_STREAM_RESET});
	struct client_world_context* succ = client_ctx_registry_current(reg, 0);
	check(succ && succ->generation != epoch && succ->server_generation == 100,
		  "F6 the genuine recovery reset still re-streams the primary");
	clin_world_payload_set_resend_sender(NULL);
}

/* The guest moves back INTO player 0's world (join variant of gate 1). */
static void test_guest_join_primary(void) {
	flag(false);
	world_reset(1, 200, false);
	guest_reset_view(1, 200, 1);
	check(!(client_ctx_registry_current(reg, 1)->bound_views & 2u),
		  "J flag OFF: a guest join is dropped");

	two_worlds(); /* guest on slot 0 (gen 100), player 0 on slot 1 (200) */
	attach_guest();
	guest_ready(557);
	assert(gstate.players[1].local_player && has_entity(0, 557));
	column(0, BLOCK_NETHERRACK, 1, 200);
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	const uint32_t epoch = nether->generation;
	guest_reset_view(1, 201, 1);
	check(client_ctx_registry_current(reg, 0) && gstate.players[1].local_player
			  && !(nether->bound_views & 2u),
		  "J a join naming another generation is dropped");
	guest_reset_view(1, 200, 1);
	check(client_ctx_registry_current(reg, 1) == nether
			  && nether->generation == epoch && nether->bound_views == 3u
			  && has_chunk(1, 0),
		  "J view 1 joins player 0's context; nothing re-generated");
	check(!client_ctx_registry_current(reg, 0) && !clin_world_payload_ready(0)
			  && !gstate.players[1].local_player,
		  "J the guest's previous world is released with its body");
	check(!gstate.players[1].local_gameplay_ready
			  && clin_world_payload_primary_slot() == 1
			  && gstate.players[0].local_player,
		  "J guest waits for re-READY; player 0 untouched");
	guest_ready(558);
	check(gstate.players[1].local_player && has_entity(1, 558)
			  && gstate.players[1].local_gameplay_ready,
		  "J the re-READY body lands in player 0's store");
	guest_reset_view(1, 200, 1);
	check(gstate.players[1].local_player && gstate.players[1].local_gameplay_ready,
		  "J a repeat join is a no-op");
	detach_guest();
}

/* Fable R2-1: the real first move -- the guest already has a body in player
 * 0's store. R2-2: the same move with the pool full, retried. */
static void test_first_move_with_body(void) {
	flag(true);
	world_reset(0, 100, false); /* player 0 on slot 0 */
	assert(clin_world_payload_bind_view(0, 1)); /* guest shares its world */
	attach_guest();
	guest_ready(560);
	assert(gstate.players[1].local_player && has_entity(0, 560));
	guest_reset_view(1, 300, 1);
	check(!gstate.players[1].local_player && !has_entity(0, 560)
			  && gstate.players[0].local_player,
		  "R2-1 the body held in player 0's store is released on the move");
	guest_ready(561);
	check(gstate.players[1].local_player && has_entity(1, 561)
			  && !has_entity(0, 561)
			  && gstate.players[1].local_player->world == payload(1).terrain,
		  "R2-1 the re-READY installs a fresh body into the guest slot");
	detach_guest();

	flag(true);
	world_reset(0, 100, false);
	assert(clin_world_payload_bind_view(0, 1));
	attach_guest();
	guest_ready(562);
	assert(client_ctx_registry_install(reg, WORLD_DIM_NETHER, 1, 299));
	fill_pool(1, WORLD_DIM_NETHER, 299);
	guest_reset_view(1, 300, 1);
	check(!client_ctx_registry_current(reg, 1) && !gstate.players[1].local_player,
		  "R2-2 pool full: the move waits, the old body is gone");
	guest_ready(563);
	check(!gstate.players[1].local_player && !has_entity(0, 563),
		  "R2-2 no body lands in player 0's store while retried");
	unpin_all();
	clin_update_drain();
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	check(nether && nether->server_generation == 300
			  && nether->bound_views == 2u && ow->bound_views == 1u
			  && clin_world_payload_ready(1),
		  "R2-2 the retry takes view 1 from player 0's context");
	guest_ready(564);
	check(gstate.players[1].local_player && has_entity(1, 564),
		  "R2-2 the re-READY body then lands in the guest slot");
	detach_guest();
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	assert(svin_init());
	assert(clin_init());
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	reg = clin_world_contexts();
	test_guest_reset();
	test_initial_guest_view_survives_host_switch();
	test_initial_guest_bind_failure_retries();
	test_transfer_hold_lifecycle();
	test_rollback_hold_waits_for_resend();
	test_guest_correction_try_route();
	test_counted_try_send();
	test_host_rollback_hold_radius();
	test_world_resend();
	test_guest_first_move();
	test_guest_reset_leased();
	test_guest_reset_install_failure();
	test_leave_clears_pending_reinstall_view();
	test_unsolicited_stream_reset();
	test_stream_reset_install_failure();
	test_reply_after_primary_switch();
	test_guest_join_primary();
	test_first_move_with_body();
	flag(false);
	printf("%s test_b2_gates (%u failure%s)\n", failures ? "FAIL" : "PASS",
		   failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
