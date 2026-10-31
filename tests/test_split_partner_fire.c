/* Copyright (c) 2026 CaveWii_v2 contributors
 * lava-aabb-0924/B3: the split-screen partner body's fire flag.
 *
 * main.c draws the fire billboard on the OTHER viewport's local body when
 * that body's client entity has on_fire set. Before this slice the only
 * writer of a local body's on_fire was the input handoff (login / respawn /
 * dimension switch, players[0] only), so a guest who walked into lava never
 * burned in the host's viewport, and a host who portalled while burning
 * burned in the guest's viewport forever.
 *
 * Every case below runs the REAL server tick (server_local_test_update_once
 * on a worker thread, exactly like test_split_live_toggle's step) against the
 * REAL guest admission (Join -> ATTACHED -> gameplay request -> body installed
 * in the shared client entity store), and reads the client copies main.c
 * reads: gstate.players[1].local_player (drawn in the host's viewport) and
 * gstate.players[0].local_player (drawn in the guest's viewport).
 *
 *   (a) server fire > 0 -> the partner copy is on_fire after the next tick;
 *   (b) server fire runs out -> the partner copy clears on the next tick;
 *   (c) a latched client true (the handoff's write after a burning portal or
 *       death) with a non-burning server body clears on the next tick;
 *   (d) the draw predicate is false once the partner corpse is retired
 *       (health <= 0 && death_time > 20, entity_local_player_render_other's
 *       own retire condition), true before it.
 *
 * lava-aabb-0924/B4 (Fable R4 N1/N2/N4): the publisher is ON CHANGE, so
 * steady play adds ~0 messages to the 16-token clin backpressure pool.
 * Counted from the client's consume observer (every envelope, tagged or not):
 *   (i)   N ticks of steady burning -> exactly ONE CRPC_LOCAL_PLAYER_FIRE per
 *         body, not N;
 *   (ii)  a state change -> exactly one more;
 *   (iii) a new guest route generation (release + re-admission) -> a forced
 *         resend for BOTH bodies even though neither state changed, and
 *         (N2) the host copy's stale on_fire from before is cleared the
 *         moment the new guest body is admitted;
 *   (iv)  after guest_release -> ZERO messages over N ticks;
 *   (v)   (b) is independent of (a): it establishes the burning copies
 *         itself (and checks them) before extinguishing;
 *   (c)   the latch clear now rides a real resend edge (the host input
 *         epoch), since an on-change publisher cannot see a client write.
 *
 * Non-aborting: every case reports ok/FAIL so a RED run shows all of them.
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

/* ---- transport harness (shape shared with test_split_live_toggle) -------- */
static unsigned markers;
static uint32_t update_done;
/* CRPC_LOCAL_PLAYER_FIRE envelopes consumed by the client, per owner body. */
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
/* One LIVE (unpaused) tick. The simulation touches the region archive, which
 * is owned by the thread that created it (this one), so the tick runs HERE and
 * a temporary consumer thread supplies production's other half of the bounded
 * client inbox -- test_local_guest's run_hazard_update shape. Client state is
 * read only after the consumer is joined and a final drain on this thread. */
static int consumer_running;
static void* consumer_entry(void* unused) {
	(void)unused;
	while(__atomic_load_n(&consumer_running, __ATOMIC_ACQUIRE)) {
		clin_update_drain();
		thread_msleep(1);
	}
	return NULL;
}
static void tick(struct server_local* s) {
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_local_test_update_once(s);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
}
static void stream_to_marker(struct server_local* s) {
	unsigned before = markers;
	for(unsigned i = 0; i < 2000 && markers == before; i++)
		step(s);
	assert(markers == before + 1 && s->input_ack_pending);
}
static void acknowledge(struct server_local* s) {
	/* Real 30 s readiness fallback: no mesher workers exist in this rig. */
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
/* Place the guest body in the host's own published column so the production
 * readiness predicate can pass (same setup as test_split_live_toggle). */
static void guest_body_home(struct server_local* s) {
	struct server_player* body = s->guest_actor;
	assert(body);
	body->x = s->player.x;
	body->y = s->player.y;
	body->z = s->player.z;
	body->rx = body->ry = 0;
	if(!server_world_is_chunk_loaded(&s->world, 0, 0)) {
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(&s->world, 0, 0, &loaded));
	}
	struct server_chunk* home
		= dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(0, 0));
	assert(home);
	home->sent = true;
}
/* A sealed dry air pocket around both bodies (feet y=4, head y=5, column
 * 0,0): no lava/fire contact can re-ignite, no water can quench, no sky. */
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
	/* set_block reports false for an identical write; verify the result. */
	struct block_data b;
	for(int y = 4; y <= 5; y++)
		assert(server_world_get_block(&s->world, 0, y, 0, &b)
			&& b.type == BLOCK_AIR);
	assert(server_world_get_block(&s->world, 0, 3, 0, &b)
		&& b.type == BLOCK_STONE);
	assert(server_world_get_block(&s->world, 0, 6, 0, &b)
		&& b.type == BLOCK_STONE);
}

/* test_split_live_toggle's guest_release: the checkpointed release queues no
 * LEFT ack, so the client half is retired by hand. */
static void guest_release(struct server_local* s) {
	s->paused = true;
	server_local_guest_release_checkpointed(s);
	assert(!s->guest_actor && !s->guest_gameplay && !s->guest_generation);
	client_session_clear_windows(&gstate.players[1]);
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[1].visible_window = -1;
	clin_update_drain();
}
/* The REAL admission: Join -> ATTACHED -> gameplay request -> body installed.
 * Leaves the server paused (transport only); the caller unpauses. */
static void guest_admit(struct server_local* s) {
	s->paused = true;
	guest_attach(s);
	guest_body_home(s);
	struct client_session* guest_client = &gstate.players[1];
	assert(client_session_request_local_gameplay(guest_client));
	pump_handshake(s);
	assert(s->guest_gameplay && guest_client->local_gameplay_ready);
	assert(guest_client->local_player && gstate.players[0].local_player);
	assert(s->guest_actor && s->guest_actor->finished_loading);
}
static void ticks(struct server_local* s, unsigned n) {
	for(unsigned i = 0; i < n; i++)
		tick(s);
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
	s->paused = true; /* transport only until the guest body is admitted */
	s->split_view_radius = 4;
	stream_to_marker(s);
	acknowledge(s);
	s->thread_started = true;

	enable_split(s);
	guest_attach(s);
	guest_body_home(s);
	struct client_session* host_client = &gstate.players[0];
	struct client_session* guest_client = &gstate.players[1];
	assert(client_session_request_local_gameplay(guest_client));
	pump_handshake(s);
	assert(s->guest_gameplay && guest_client->local_gameplay_ready);
	assert(guest_client->local_player && host_client->local_player);
	assert(guest_client->local_player != host_client->local_player);
	struct server_player* host = &s->player;
	struct server_player* guest = s->guest_actor;
	assert(guest && guest->finished_loading && host->finished_loading);

	seal_pocket(s);
	s->paused = false;
	host->fire = guest->fire = -PLAYER_FIRE_RESISTANCE;
	tick(s);
	check(!host_client->local_player->on_fire
		&& !guest_client->local_player->on_fire,
		"baseline: neither body burning -> neither partner copy on_fire");

	/* (a) guest's server fire goes positive. */
	guest->fire = 600;
	tick(s);
	check(guest->fire == 599, "(a) fixture: guest server fire ticked 600 -> 599");
	check(guest_client->local_player->on_fire,
		"(a) guest burning -> the host viewport's partner copy (players[1]) "
		"is on_fire after the next tick");
	check(clin_local_body_fire_visible(guest_client),
		"(a) host viewport draw predicate true for the burning guest");
	check(!host_client->local_player->on_fire,
		"(a) the host body is not painted by the guest's fire");
	/* ...and the mirror: the host burning shows in the guest viewport. */
	host->fire = 600;
	tick(s);
	check(host->fire == 599, "(a) fixture: host server fire ticked 600 -> 599");
	check(host_client->local_player->on_fire,
		"(a) host burning -> the guest viewport's partner copy (players[0]) "
		"is on_fire after the next tick");

	/* (i) steady burning: N ticks, both bodies burning throughout (fire far
	 * above N), so neither state changes after the window's first tick.
	 * Start from a published false so the window holds exactly one edge. */
	host->fire = guest->fire = -PLAYER_FIRE_RESISTANCE;
	tick(s);
	check(!host_client->local_player->on_fire
		&& !guest_client->local_player->on_fire,
		"(i) setup: both partner copies cleared");
	host->fire = guest->fire = 600;
	fire_count_reset();
	ticks(s, STEADY_TICKS);
	check(host->fire > 0 && guest->fire > 0,
		"(i) fixture: both bodies burned through the whole window");
	check(host_client->local_player->on_fire
		&& guest_client->local_player->on_fire,
		"(i) both partner copies on_fire");
	printf("     (i) fire messages over %u steady ticks: host %u guest %u\n",
		STEADY_TICKS, fire_msgs[0], fire_msgs[1]);
	check(fire_msgs[0] == 1,
		"(i) steady burning host -> exactly 1 CRPC_LOCAL_PLAYER_FIRE, not N");
	check(fire_msgs[1] == 1,
		"(i) steady burning guest -> exactly 1 CRPC_LOCAL_PLAYER_FIRE, not N");

	/* (ii) one state change -> exactly one more message for THAT body. */
	guest->fire = 1; /* burns out on the next tick; the host keeps burning */
	fire_count_reset();
	ticks(s, STEADY_TICKS);
	check(guest->fire <= 0 && host->fire > 0,
		"(ii) fixture: guest burned out, host still burning");
	check(!guest_client->local_player->on_fire
		&& host_client->local_player->on_fire,
		"(ii) guest copy cleared, host copy still on_fire");
	printf("     (ii) fire messages over %u ticks: host %u guest %u\n",
		STEADY_TICKS, fire_msgs[0], fire_msgs[1]);
	check(fire_msgs[1] == 1, "(ii) guest change -> exactly 1 more message");
	check(fire_msgs[0] == 0, "(ii) unchanged host -> 0 messages");

	/* (b) fire extinguished (burns out on the next tick). (v) Independent of
	 * (a): the burning copies are established HERE and checked before the
	 * extinguish, so (b) can never pass vacuously. */
	host->fire = guest->fire = 600;
	tick(s);
	check(host_client->local_player->on_fire
		&& guest_client->local_player->on_fire,
		"(b) precondition: both partner copies on_fire before the extinguish");
	guest->fire = 1;
	host->fire = 1;
	tick(s);
	check(guest->fire <= 0 && host->fire <= 0,
		"(b) fixture: both server fires burned out");
	check(!guest_client->local_player->on_fire,
		"(b) guest fire out -> partner copy clears on the next tick");
	check(!host_client->local_player->on_fire,
		"(b) host fire out -> partner copy clears on the next tick");

	/* (d) corpse retire gate (client-side presentation only). */
	struct entity* body = guest_client->local_player;
	short health_before = body->health;
	int death_before = guest_client->camera.hurt.death_time;
	body->on_fire = true;
	body->health = 0;
	guest_client->camera.hurt.death_time = 20;
	check(clin_local_body_fire_visible(guest_client),
		"(d) burning corpse still drawn at death_time 20 -> fire drawn");
	guest_client->camera.hurt.death_time = 21;
	check(!clin_local_body_fire_visible(guest_client),
		"(d) corpse retired (health<=0, death_time>20) -> fire NOT drawn");
	body->health = health_before;
	check(clin_local_body_fire_visible(guest_client),
		"(d) living burning body at death_time 21 -> fire drawn");
	guest_client->camera.hurt.death_time = death_before;
	body->on_fire = false;

	/* (iii) + N2: guest route generation change. The host burns, the guest
	 * leaves, and a NEW guest is admitted: a new route generation. Nothing is
	 * published while no guest is admitted, so the host copy's true is stale
	 * by the time the new guest joins (N2) and must be cleared at admission;
	 * the first tick then force-resends BOTH bodies even though the host's
	 * burning state is the same value that was last published. */
	host->fire = 600;
	tick(s);
	check(host_client->local_player->on_fire,
		"(iii) setup: host copy on_fire before the guest leaves");
	uint64_t old_generation = s->guest_generation;
	guest_release(s);
	host->fire = -PLAYER_FIRE_RESISTANCE; /* host stops burning while alone */
	s->paused = false;
	tick(s);
	host->fire = 600; /* ...and burns again by the time the next guest joins */
	guest_admit(s);
	guest = s->guest_actor;
	check(s->guest_generation != old_generation,
		"(iii) fixture: a new guest route generation");
	check(!host_client->local_player->on_fire,
		"(iii/N2) guest admission clears the host copy's stale on_fire");
	s->paused = false;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	fire_count_reset();
	ticks(s, STEADY_TICKS);
	printf("     (iii) fire messages over %u ticks after re-admission: "
		"host %u guest %u\n", STEADY_TICKS, fire_msgs[0], fire_msgs[1]);
	check(host_client->local_player->on_fire,
		"(iii) host copy on_fire after the first tick of the new generation");
	check(!guest_client->local_player->on_fire,
		"(iii) new guest copy not on_fire");
	check(fire_msgs[0] == 1,
		"(iii) new generation -> exactly 1 forced host resend over N ticks");
	check(fire_msgs[1] == 1,
		"(iii) new generation -> exactly 1 forced guest resend over N ticks");

	/* (c) no latch across the input handoff: clin_reset_input_handoff wrote
	 * on_fire=true for a burning portal / respawn (a new input epoch) while
	 * the server body is no longer burning. The epoch edge must force a
	 * resend although the published value (false) has not changed. The epoch
	 * advances exactly as server_local_arm_input_handoff advances it, with
	 * the ACK already consumed (input_ack_pending == 0). */
	host->fire = -PLAYER_FIRE_RESISTANCE;
	tick(s);
	check(!host_client->local_player->on_fire, "(c) setup: host copy cleared");
	host_client->local_player->on_fire = true;
	s->input_epoch_counter++;
	fire_count_reset();
	tick(s);
	check(!host_client->local_player->on_fire,
		"(c) latched handoff on_fire + non-burning server host -> the guest "
		"viewport's partner copy clears on the next tick");
	check(fire_msgs[0] == 1 && fire_msgs[1] == 0,
		"(c) the epoch edge forces exactly one host resend");

	/* (iv) N4: after guest_release no split partner exists -- nothing may be
	 * published, burning or not, changing or not. */
	guest_release(s);
	s->paused = false;
	host->fire = 600;
	fire_count_reset();
	ticks(s, STEADY_TICKS);
	host->fire = 1;
	ticks(s, 2);
	printf("     (iv) fire messages over %u ticks after release: host %u "
		"guest %u\n", STEADY_TICKS + 2, fire_msgs[0], fire_msgs[1]);
	check(fire_msgs[0] == 0 && fire_msgs[1] == 0,
		"(iv) guest released -> ZERO CRPC_LOCAL_PLAYER_FIRE over N ticks, "
		"including across a host fire change");

	/* Teardown (the guest was already released in (iv)), so no fixture
	 * directory is left behind. */
	s->paused = true;
	split_destroy(s);

	if(failures) {
		printf("test_split_partner_fire: %u FAILED\n", failures);
		return 1;
	}
	puts("test_split_partner_fire: PASS");
	return 0;
}
