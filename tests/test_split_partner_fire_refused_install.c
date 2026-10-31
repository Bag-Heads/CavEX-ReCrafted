/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/C2b: no lost partner-fire update after a REFUSED
 * first guest body install (KANBAN "Reported": Fable R5 of 520acfe2).
 *
 * The sequence that lost both flames (pre-fix):
 *   1. the server admits the guest body and answers LOCAL_GUEST_GAMEPLAY_READY;
 *   2. the client's clin_guest_body_install is refused (entity alloc failure,
 *      client_interface.c clin_entity_spawn_alloc) -- the session stays
 *      ATTACHED with no body and simply re-asks;
 *   3. the server's next tick publishes BOTH fire states
 *      (server_local_publish_split_fire): the host's untagged
 *      CRPC_LOCAL_PLAYER_FIRE is applied to players[0].local_player; the
 *      guest's generation-tagged one reaches client_session_local_reply with
 *      NO body, is skipped ("never a reply failure") and yet is RECORDED as
 *      sent by the publisher's on-change key;
 *   4. the retried READY takes the server's idempotent branch
 *      (server_local_guest_gameplay: `if(s->guest_gameplay) return ...`) --
 *      no resend key moves -- and the client NOW installs the body: the
 *      guest copy starts on_fire == false (the skipped message is gone) and
 *      the install's N2 clear wipes the host copy's already-applied true.
 *   Both partner flames stay hidden until the next real state change.
 *
 * This test drives exactly that: a real admission on the split residency
 * fixture whose FIRST install is refused through the deterministic entity-slot
 * seam (clin_test_fail_entity_slot_after(1)), a real server tick that publishes
 * both burning bodies while the guest has no body, then the idempotent retry
 * and N steady ticks. It asserts what main.c reads: both partner copies are
 * on_fire once the body exists, and the two copies follow later changes as
 * usual. Non-aborting checks so a RED run shows every failing clause.
 */
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif
#include "support/split_residency_fixture.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/server_local_guest.h"
#include <stdio.h>

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

/* ---- transport harness (test_split_partner_fire's shape) ----------------- */
static unsigned markers;
static uint32_t update_done;
static unsigned fire_msgs[2];

static void consumed(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_PLAYER_POS && rpc->payload.player_pos.input_epoch)
		markers++;
	if(rpc->type == CRPC_LOCAL_PLAYER_FIRE
	   && rpc->payload.local_player_fire.owner < 2)
		fire_msgs[rpc->payload.local_player_fire.owner]++;
}
static void fire_count_reset(void) {
	fire_msgs[0] = fire_msgs[1] = 0;
}
static void* update_server(void* ptr) {
	server_local_test_update_once(ptr);
	__atomic_store_n(&update_done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void step(struct server_local* s) {
	struct thread worker;
	update_done = 0;
	assert(thread_create(&worker, update_server, s, 64));
	ptime_t start = time_get();
	while(!__atomic_load_n(&update_done, __ATOMIC_ACQUIRE)) {
		clin_update_drain();
		assert(time_diff_s(start, time_get()) < 20);
		thread_msleep(1);
	}
	thread_join(&worker);
	clin_update_drain();
}
static int consumer_running;
static void* consumer_entry(void* unused) {
	(void)unused;
	while(__atomic_load_n(&consumer_running, __ATOMIC_ACQUIRE)) {
		clin_update_drain();
		thread_msleep(1);
	}
	return NULL;
}
/* One LIVE tick on this thread (region archive owner) with a temporary
 * consumer supplying the other half of the bounded client inbox. */
static void tick(struct server_local* s) {
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_local_test_update_once(s);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
}
static void ticks(struct server_local* s, unsigned n) {
	for(unsigned i = 0; i < n; i++)
		tick(s);
}
static void stream_to_marker(struct server_local* s) {
	unsigned before = markers;
	for(unsigned i = 0; i < 2000 && markers == before; i++)
		step(s);
	assert(markers == before + 1 && s->input_ack_pending);
}
static void acknowledge(struct server_local* s) {
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	step(s);
	assert(!s->input_ack_pending && !s->stream_recovery_epoch);
	assert(s->input_epoch_counter != 0);
}
static void drain_server(struct server_local* s) {
	server_local_test_process_messages(s);
	clin_update_drain();
}
static void enable_split(struct server_local* s) {
	struct server_rpc call = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = true,
			.world_generation = s->input_epoch_counter, .request_seq = 1}};
	assert(svin_rpc_send(&call));
	drain_server(s);
	assert(s->split_enabled);
}
static void pump_handshake(struct server_local* s) {
	client_session_flush_local(&gstate.players[1]);
	server_local_test_process_messages(s);
	clin_update_drain();
}
static void guest_attach(struct server_local* s) {
	struct client_session* client = &gstate.players[1];
	assert(client_session_request_local_join(client));
	pump_handshake(s);
	pump_handshake(s);
	pump_handshake(s);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED && s->guest_confirmed);
}
static void guest_body_home(struct server_local* s) {
	struct server_player* body = s->guest_actor;
	assert(body);
	body->x = s->player.x;
	body->y = s->player.y;
	body->z = s->player.z;
	body->rx = body->ry = 0;
	if(!server_world_is_chunk_loaded(&s->worlds[0], 0, 0)) {
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(&s->worlds[0], 0, 0, &loaded));
	}
	struct server_chunk* home
		= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert(home);
	home->sent = true;
}
/* Sealed dry air pocket around both bodies: nothing re-ignites or quenches. */
static void seal_pocket(struct server_local* s) {
	const struct block_data air = {.type = BLOCK_AIR};
	const struct block_data stone = {.type = BLOCK_STONE};
	for(int y = 4; y <= 5; y++) {
		(void)server_world_set_block(&s->worlds[0], 0, y, 0, air);
		(void)server_world_set_block(&s->worlds[0], 1, y, 0, stone);
		(void)server_world_set_block(&s->worlds[0], -1, y, 0, stone);
		(void)server_world_set_block(&s->worlds[0], 0, y, 1, stone);
		(void)server_world_set_block(&s->worlds[0], 0, y, -1, stone);
	}
	(void)server_world_set_block(&s->worlds[0], 0, 3, 0, stone);
	(void)server_world_set_block(&s->worlds[0], 0, 6, 0, stone);
	struct block_data b;
	for(int y = 4; y <= 5; y++)
		assert(server_world_get_block(&s->worlds[0], 0, y, 0, &b)
			&& b.type == BLOCK_AIR);
	assert(server_world_get_block(&s->worlds[0], 0, 3, 0, &b)
		&& b.type == BLOCK_STONE);
	assert(server_world_get_block(&s->worlds[0], 0, 6, 0, &b)
		&& b.type == BLOCK_STONE);
}
#define STEADY_TICKS 10u

/* ======================================================================== */
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);

	struct server_local* s = split_fixture();
	s->paused = true; /* transport only until the body is admitted */
	s->split_view_radius = 4;
	stream_to_marker(s);
	acknowledge(s);
	s->thread_started = true;

	enable_split(s);
	guest_attach(s);
	guest_body_home(s);
	struct client_session* host_client = &gstate.players[0];
	struct client_session* guest_client = &gstate.players[1];
	struct server_player* host = &s->player;
	assert(host_client->local_player && host->finished_loading);

	/* ---- 1+2: the server admits; the client's FIRST install is refused. --- */
	assert(client_session_request_local_gameplay(guest_client));
	client_session_flush_local(guest_client);
	server_local_test_process_messages(s); /* READY queued, body admitted */
	struct server_player* guest = s->guest_actor;
	assert(guest && s->guest_gameplay && guest->finished_loading
		&& s->guest_entity_id);
	const uint32_t admitted_id = s->guest_entity_id;
	/* The server is paused: the only entity-slot allocation in this drain is
	 * the guest body's, so the first occurrence is exactly the install. */
	clin_test_fail_entity_slot_after(1);
	clin_update_drain();
	check(!guest_client->local_player && !guest_client->local_gameplay_ready
		&& guest_client->local_phase == CLIENT_GUEST_ATTACHED,
		"fixture: first LOCAL_GUEST_GAMEPLAY_READY install REFUSED -- no body, "
		"not gameplay-ready, session still ATTACHED");
	check(!dict_entity_get(gstate.entities, admitted_id),
		"fixture: the refused install left no entity under the admitted id");
	check(s->guest_gameplay && s->guest_entity_id == admitted_id,
		"fixture: the server still holds the admitted body (it cannot see the "
		"refusal)");

	/* ---- 3: both bodies burn; the server publishes while the guest has no
	 * client body. The host copy gets its true now; the guest's tagged
	 * message has nowhere to land. ---------------------------------------- */
	seal_pocket(s);
	/* 599, not 600: the burn cadence (Entity.java:203-215, `fire % 20 == 0`)
	 * would damage the guest on a 600 tick, and that tagged
	 * CRPC_PLAYER_SET_HEALTH with no body is a reply FAILURE that retires the
	 * guest (client_session_local_reply) -- a different outcome from the one
	 * under test. The lost update needs the window between the refusal and
	 * the retry to carry only the fire publish, which is also why the bug is
	 * rare in play. */
	host->fire = guest->fire = 599;
	s->paused = false;
	fire_count_reset();
	tick(s);
	check(host->fire == 598 && guest->fire == 598,
		"fixture: both server bodies burning (599 -> 598)");
	check(!guest_client->local_reply_failed,
		"fixture: no reply failure in the refusal window (the guest is not "
		"being retired; the READY retry is what comes next)");
	printf("     fire messages on the publish tick: host %u guest %u\n",
		fire_msgs[0], fire_msgs[1]);
	check(fire_msgs[0] == 1 && fire_msgs[1] == 1,
		"fixture: the publisher sent ONE message per body on that tick "
		"(the guest's is the one with no body to land on)");
	check(host_client->local_player->on_fire,
		"host copy on_fire right after the publish (before the retry)");
	check(!guest_client->local_player,
		"fixture: still no guest body when the guest FIRE arrived");

	/* ---- 4: the idempotent retry; the install succeeds this time. -------- */
	s->paused = true;
	assert(client_session_request_local_gameplay(guest_client));
	pump_handshake(s);
	check(guest_client->local_player && guest_client->local_gameplay_ready,
		"retry: idempotent READY -> guest body installed, gameplay-ready");
	check(s->guest_gameplay && s->guest_entity_id == admitted_id
		&& s->guest_actor == guest,
		"retry: same server body, same entity id (no re-admission)");
	if(!guest_client->local_player) {
		puts("test_split_partner_fire_refused_install: fixture could not "
			 "install the retried body -- aborting");
		s->paused = true;
		split_destroy(s);
		return 1;
	}

	/* ---- the claim: no lost update. Both partner copies show the burn the
	 * server believes it already published, at install or within N ticks. -- */
	s->paused = false;
	fire_count_reset();
	ticks(s, STEADY_TICKS);
	check(host->fire > 0 && guest->fire > 0,
		"fixture: both bodies still burning through the steady window");
	printf("     fire messages over %u ticks after the retry: host %u guest %u\n",
		STEADY_TICKS, fire_msgs[0], fire_msgs[1]);
	check(guest_client->local_player->on_fire,
		"GUEST copy on_fire after a refused first install + idempotent retry "
		"(the host viewport draws the guest's flames)");
	check(clin_local_body_fire_visible(guest_client),
		"host viewport draw predicate true for the burning guest");
	check(host_client->local_player->on_fire,
		"HOST copy on_fire after the retried install (the guest viewport draws "
		"the host's flames; the N2 clear must not wipe an applied publish)");
	check(clin_local_body_fire_visible(host_client),
		"guest viewport draw predicate true for the burning host");

	/* ---- and the copies still follow later changes as usual. ------------- */
	guest->fire = 1; /* out on the next tick; the host keeps burning */
	tick(s);
	check(guest->fire <= 0 && host->fire > 0,
		"fixture: guest burned out, host still burning");
	check(!guest_client->local_player->on_fire && host_client->local_player->on_fire,
		"guest copy clears on its change, host copy stays on_fire");
	host->fire = 1;
	tick(s);
	check(host->fire <= 0, "fixture: host burned out");
	check(!host_client->local_player->on_fire,
		"host copy clears on its change");
	guest->fire = 600;
	tick(s);
	check(guest_client->local_player->on_fire,
		"guest copy re-lights on a fresh burn");

	/* Teardown: release the guest so no fixture directory is left behind. */
	s->paused = true;
	server_local_guest_release_checkpointed(s);
	assert(!s->guest_actor && !s->guest_gameplay);
	client_session_clear_windows(&gstate.players[1]);
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[1].visible_window = -1;
	clin_update_drain();
	split_destroy(s);

	if(failures) {
		printf("test_split_partner_fire_refused_install: %u FAILED\n", failures);
		return 1;
	}
	puts("test_split_partner_fire_refused_install: PASS");
	return 0;
}
