/* First guest-body admission recovery: real local server and client handlers.
 * Each argv case runs in a fresh process. Expected RED checks use return 1,
 * never an abort, so CTest WILL_FAIL records the missing contract precisely. */
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

static unsigned failures, markers;
static void check(bool yes, const char* why) {
	printf("%s %s\n", yes ? "ok  " : "FAIL", why);
	if(!yes) failures++;
}
static void consumed(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_PLAYER_POS && rpc->payload.player_pos.input_epoch)
		markers++;
}
static uint32_t update_done;
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
static void pump(struct server_local* s) {
	client_session_flush_local(&gstate.players[1]);
	server_local_test_process_messages(s);
	clin_update_drain();
}
static void send_guest(const struct client_rpc* rpc, uint64_t generation) {
	assert(clin_rpc_send_local(rpc, 1, generation));
}
static bool visible_reason(const struct client_session* guest) {
	/* A failure must remain readable on the guest HUD well past a toast's
	 * three-second lifetime. The reason text itself may be implementation
	 * chosen; an empty or expired message cannot tell a controller user why. */
	return guest->hud_message[0] != '\0'
		&& !time_reached(time_add_ms(time_get(), 60000), guest->hud_message_until);
}
static struct server_local* setup(void) {
	struct server_local* s = split_fixture();
	/* admit-retry-0925 step 2: give the real guest Leave a world directory so
	 * its complete-world checkpoint (chest/sign snapshots under level_name)
	 * can succeed; with the empty default it failed at chest_snapshot and the
	 * server correctly retained the guest (LEAVE_FAILED). */
	string_set_str(s->level_name, split_path);
	s->paused = true;
	s->split_view_radius = 4;
	unsigned before = markers;
	for(unsigned i = 0; i < 2000 && markers == before; i++) step(s);
	assert(markers == before + 1 && s->input_ack_pending);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	step(s);
	assert(!s->input_ack_pending && s->input_epoch_counter);
	struct server_rpc split = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = true,
			.world_generation = s->input_epoch_counter, .request_seq = 1}};
	assert(svin_rpc_send(&split));
	server_local_test_process_messages(s);
	clin_update_drain();
	assert(s->split_enabled);
	assert(client_session_request_local_join(&gstate.players[1]));
	pump(s); pump(s); pump(s);
	assert(gstate.players[1].local_phase == CLIENT_GUEST_ATTACHED
		&& s->guest_confirmed && s->guest_actor);
	struct server_player* body = s->guest_actor;
	body->x = s->player.x; body->y = s->player.y; body->z = s->player.z;
	body->rx = body->ry = 0;
	if(!server_world_is_chunk_loaded(&s->worlds[0], 0, 0)) {
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(&s->worlds[0], 0, 0, &loaded));
	}
	struct server_chunk* home = dict_server_chunks_get(s->worlds[0].chunks,
		S_CHUNK_ID(0, 0));
	assert(home); home->sent = true;
	return s;
}
static void request_ready(struct server_local* s) {
	assert(client_session_request_local_gameplay(&gstate.players[1]));
	client_session_flush_local(&gstate.players[1]);
	server_local_test_process_messages(s);
	assert(s->guest_gameplay && s->guest_entity_id);
}
static void refused_first(struct server_local* s) {
	request_ready(s);
	clin_test_fail_entity_slot_after(1);
	clin_update_drain();
	check(!gstate.players[1].local_player
		&& !gstate.players[1].local_gameplay_ready
		&& gstate.players[1].local_phase == CLIENT_GUEST_ATTACHED,
		"setup: real first READY refused; attached without guest body or gameplay");
}
/* Astra reviews A item 2 / 3 item 2: the visible surface is the reason's OWN
 * slot, drawn by the host HUD on a dedicated line (the guest has no viewport
 * without a body), plus the join card main.c settles on the edge. The edge
 * fires once; the reason survives ordinary host notices. */
static void host_surface(const char* what) {
	struct client_session* g = &gstate.players[1];
	struct client_session* h = &gstate.players[0];
	char line[128];
	bool first = client_session_local_admit_failure_present(g, h);
	bool again = client_session_local_admit_failure_present(g, h);
	snprintf(line, sizeof(line), "%s: join-card edge reported once", what);
	check(first && !again, line);
	const char* reason = client_session_local_admit_failure_reason(g);
	snprintf(line, sizeof(line), "%s: host HUD line has the guest's reason", what);
	check(reason && reason[0] && strcmp(reason, g->local_admit_notice) == 0, line);
	struct client_rpc ordinary = {.type = CRPC_HUD_MESSAGE};
	strcpy(ordinary.payload.hud_message, "ordinary host notice");
	for(unsigned n = 0; n < 3; n++)
		(void)client_session_player_reply(h, &ordinary, time_get());
	snprintf(line, sizeof(line), "%s: ordinary host notices never replace the reason", what);
	check(client_session_local_admit_failure_reason(g) == reason
		&& strcmp(h->hud_message, "ordinary host notice") == 0, line);
}
static void settle_leave(struct server_local* s, const char* what) {
	struct client_session* g = &gstate.players[1];
	char line[128];
	for(unsigned n = 0; n < 4 && g->local_bound; n++) pump(s);
	snprintf(line, sizeof(line), "%s: authoritative Leave released server guest and unbound", what);
	check(!s->guest_actor && !s->guest_gameplay && !g->local_bound
		&& g->local_phase == CLIENT_GUEST_IDLE, line);
}
/* A real rejoin through the same server after a retirement: the leak Astra
 * found would refuse client_session_request_local_join outright. */
static void rejoin_and_play(struct server_local* s, const char* what) {
	struct client_session* g = &gstate.players[1];
	char line[128];
	bool asked = client_session_request_local_join(g);
	pump(s); pump(s); pump(s);
	snprintf(line, sizeof(line), "%s: rejoin attaches a new generation", what);
	check(asked && g->local_phase == CLIENT_GUEST_ATTACHED && s->guest_confirmed
		&& s->guest_actor, line);
	if(g->local_phase != CLIENT_GUEST_ATTACHED || !s->guest_actor) return;
	struct server_player* body = s->guest_actor;
	body->x = s->player.x; body->y = s->player.y; body->z = s->player.z;
	body->rx = body->ry = 0;
	request_ready(s);
	clin_update_drain();
	snprintf(line, sizeof(line), "%s: rejoined guest installs a body and plays", what);
	check(g->local_player && g->local_gameplay_ready, line);
	snprintf(line, sizeof(line), "%s: the new Join cleared the failure reason", what);
	check(!client_session_local_admit_failure_reason(g), line);
}
static void refuse_again(struct server_local* s) {
	struct client_session* g = &gstate.players[1];
	assert(client_session_request_local_gameplay(g));
	client_session_flush_local(g);
	server_local_test_process_messages(s);
	clin_test_fail_entity_slot_after(1);
	clin_update_drain();
	client_session_flush_local(g);
}
static void retry_boundary(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	for(unsigned n = 0; n + 1 < CLIENT_GUEST_ADMIT_RETRY_MAX; n++) refuse_again(s);
	check(g->local_phase == CLIENT_GUEST_ATTACHED && g->local_admit.active
		&& g->local_admit.refusals == CLIENT_GUEST_ADMIT_RETRY_MAX,
		"retry_boundary: first + RETRY_MAX-1 refused retries still recovering");
	refuse_again(s);
	check(g->local_phase == CLIENT_GUEST_LEAVING && !g->local_admit.active,
		"retry_boundary: the RETRY_MAX-th refused retry retires");
	check(visible_reason(g), "retry_boundary: guest keeps a readable reason");
	host_surface("retry_boundary");
	settle_leave(s, "retry_boundary");
	rejoin_and_play(s, "retry_boundary");
}
static void retry_last(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	for(unsigned n = 0; n + 1 < CLIENT_GUEST_ADMIT_RETRY_MAX; n++) refuse_again(s);
	struct client_rpc health = {.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = 11};
	send_guest(&health, g->local_generation);
	request_ready(s);
	clin_update_drain();
	check(g->local_player && g->local_gameplay_ready
		&& g->local_player->health == 11 && !g->local_admit.active,
		"retry_last: the last allowed retry installs, replays and opens gameplay");
	check(!client_session_local_admit_failure_present(g, &gstate.players[0]),
		"retry_last: a recovered admission reports no failure");
}
static void overflow_boundary(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	for(unsigned n = 0; n < CLIENT_GUEST_ADMIT_HUD_RETAIN_MAX; n++) {
		struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
		snprintf(hud.payload.hud_message, sizeof(hud.payload.hud_message),
			"kept %u", n);
		send_guest(&hud, g->local_generation);
	}
	clin_update_drain();
	client_session_flush_local(g);
	check(g->local_phase == CLIENT_GUEST_ATTACHED && !g->local_reply_failed
		&& g->local_admit.hud_count == CLIENT_GUEST_ADMIT_HUD_RETAIN_MAX,
		"overflow_boundary: exactly HUD_RETAIN_MAX notices are retained");
	struct client_rpc extra = {.type = CRPC_HUD_MESSAGE};
	strcpy(extra.payload.hud_message, "one too many");
	send_guest(&extra, g->local_generation);
	clin_update_drain();
	client_session_flush_local(g);
	check(g->local_phase == CLIENT_GUEST_LEAVING,
		"overflow_boundary: notice HUD_RETAIN_MAX+1 retires");
	check(client_session_local_admit_failure_reason(g)
		&& strstr(client_session_local_admit_failure_reason(g),
			"too many pending messages"),
		"overflow_boundary: loss is explicit -- too many pending messages");
	check(visible_reason(g), "overflow_boundary: guest keeps a readable reason");
	host_surface("overflow_boundary");
	settle_leave(s, "overflow_boundary");
}
static void timeout_case(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	check(g->local_admit.active && !time_reached(time_get(), g->local_admit.deadline)
		&& time_reached(time_add_ms(time_get(), CLIENT_GUEST_ADMIT_TIMEOUT_MS + 1000),
			g->local_admit.deadline),
		"timeout: recovery deadline armed at CLIENT_GUEST_ADMIT_TIMEOUT_MS");
	client_session_flush_local(g);
	check(g->local_phase == CLIENT_GUEST_ATTACHED,
		"timeout: no retirement before the deadline");
	g->local_admit.deadline = time_get(); /* the deadline has now passed */
	client_session_flush_local(g);
	check(g->local_phase == CLIENT_GUEST_LEAVING && !g->local_admit.active,
		"timeout: an expired recovery retires");
	check(visible_reason(g), "timeout: guest keeps a readable reason");
	host_surface("timeout");
	settle_leave(s, "timeout");
}
static void live_hud(const char* text) {
	struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
	strcpy(hud.payload.hud_message, text);
	send_guest(&hud, gstate.players[1].local_generation);
	clin_update_drain();
}
static void next_turn(void) {
	gstate.players[1].hud_message_until = time_get(); /* showing turn ended */
	client_session_flush_local(&gstate.players[1]);
}
static void notices(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	const char* text[3] = {"first notice", "second notice", "third notice"};
	for(unsigned n = 0; n < 3; n++) {
		struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
		strcpy(hud.payload.hud_message, text[n]);
		send_guest(&hud, g->local_generation);
	}
	clin_update_drain();
	request_ready(s);
	clin_update_drain();
	check(g->local_gameplay_ready && strcmp(g->hud_message, text[0]) == 0
		&& g->hud_queue_count == 2,
		"notices: first retained notice showing, two queued");
	live_hud("live one");
	check(strcmp(g->hud_message, text[0]) == 0 && g->hud_queue_count == 3,
		"notices: a live notice waits behind the retained ones");
	client_session_flush_local(g);
	check(strcmp(g->hud_message, text[0]) == 0,
		"notices: a showing notice keeps its full toast time");
	next_turn();
	bool ok = strcmp(g->hud_message, text[1]) == 0;
	next_turn();
	ok = ok && strcmp(g->hud_message, text[2]) == 0;
	next_turn();
	ok = ok && strcmp(g->hud_message, "live one") == 0 && g->hud_queue_count == 0;
	check(ok, "notices: each notice gets its own toast turn in arrival order");
	/* Review 3 item 3: the FINAL dequeued notice keeps its turn too. */
	live_hud("live two");
	check(strcmp(g->hud_message, "live one") == 0 && g->hud_queue_count == 1,
		"notices: a live arrival during the final dequeued notice queues");
	next_turn();
	check(strcmp(g->hud_message, "live two") == 0,
		"notices: it then gets its own turn");
	next_turn(); /* queue empty: protection ends */
	live_hud("live three");
	live_hud("live four");
	check(strcmp(g->hud_message, "live four") == 0 && g->hud_queue_count == 0,
		"notices: with no backlog, ordinary notices behave as before");
}
static void notices_single(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
	strcpy(hud.payload.hud_message, "only retained");
	send_guest(&hud, g->local_generation);
	clin_update_drain();
	request_ready(s);
	clin_update_drain();
	check(g->local_gameplay_ready && strcmp(g->hud_message, "only retained") == 0
		&& g->hud_queue_count == 0,
		"notices_single: the one retained notice shows with nothing queued");
	live_hud("arrives live");
	check(strcmp(g->hud_message, "only retained") == 0 && g->hud_queue_count == 1,
		"notices_single: a live arrival queues behind the single retained notice");
	next_turn();
	check(strcmp(g->hud_message, "arrives live") == 0,
		"notices_single: the live notice gets the next turn");
}
static void hud_overflow(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
	strcpy(hud.payload.hud_message, "retained");
	send_guest(&hud, g->local_generation);
	clin_update_drain();
	request_ready(s);
	clin_update_drain();
	for(unsigned n = 0; n < CLIENT_HUD_QUEUE_MAX + 3; n++) {
		char text[48];
		snprintf(text, sizeof(text), "live %u", n);
		live_hud(text);
	}
	check(g->hud_queue_count == CLIENT_HUD_QUEUE_MAX && g->hud_queue_dropped == 3,
		"hud_overflow: a full queue drops and COUNTS arrivals (no replace)");
	bool ordered = true;
	for(unsigned n = 0; n < CLIENT_HUD_QUEUE_MAX; n++) {
		char text[48];
		snprintf(text, sizeof(text), "live %u", n);
		next_turn();
		ordered = ordered && strcmp(g->hud_message, text) == 0;
	}
	check(ordered, "hud_overflow: every queued notice kept its order and text");
	next_turn();
	check(strcmp(g->hud_message, "(+3 more)") == 0 && g->hud_queue_dropped == 0,
		"hud_overflow: the loss is shown as its own (+3 more) turn");
}
static void cleanup_retry(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	/* Make the server's Leave checkpoint fail (chest snapshot under a world
	 * directory that does not exist), exactly the ownership-preserving refusal
	 * Astra named. IN-011: this used to be an empty level_name, which made the
	 * snapshot path "/chests.dat_new" -- the root of the cwd's DRIVE. On a
	 * read-only root (C:\) the refusal happened by accident; on a writable one
	 * (D:\) the checkpoint succeeded and wrote chests.dat/signs.dat there. A
	 * never-created child of this process's own fixture directory fails the
	 * fopen identically from any cwd and never writes outside the fixture. */
	char refused_path[sizeof(split_path) + 32];
	snprintf(refused_path, sizeof(refused_path), "%s/no_such_world", split_path);
	string_set_str(s->level_name, refused_path);
	g->local_admit.deadline = time_get();
	client_session_flush_local(g);
	for(unsigned n = 0; n < 3 && g->local_phase == CLIENT_GUEST_LEAVING; n++) pump(s);
	check(g->local_phase == CLIENT_GUEST_FAILED && g->local_bound && s->guest_actor,
		"cleanup_retry: refused checkpoint keeps ownership (FAILED, bound, server guest)");
	host_surface("cleanup_retry");
	string_set_str(s->level_name, split_path);
	check(client_session_request_local_join(g) && g->local_phase == CLIENT_GUEST_LEAVING,
		"cleanup_retry: the join card Retry re-sends the authoritative Leave");
	settle_leave(s, "cleanup_retry");
	rejoin_and_play(s, "cleanup_retry");
}
static void mixed(struct server_local* s) {
	struct client_session* g = &gstate.players[1];
	struct client_session* h = &gstate.players[0];
	refused_first(s);
	const uint64_t gen = g->local_generation;
	const int host_health = h->local_player->health;
	struct client_rpc health1 = {.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = 31};
	struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
	strcpy(hud.payload.hud_message, "guest admission notice");
	struct client_rpc sleep = {.type = CRPC_SLEEP_STATE,
		.payload.sleep_state = {.timer = 42, .sleeping = true}};
	struct client_rpc fire = {.type = CRPC_LOCAL_PLAYER_FIRE,
		.payload.local_player_fire = {.owner = 1, .on_fire = true}};
	struct client_rpc health2 = {.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = 27};
	send_guest(&health1, gen); send_guest(&hud, gen);
	send_guest(&sleep, gen); send_guest(&fire, gen);
	send_guest(&health2, gen);
	clin_update_drain();
	client_session_flush_local(g);
	check(g->local_phase == CLIENT_GUEST_ATTACHED && !g->local_reply_failed,
		"mixed: body-bound replies during refusal do not retire the guest");
	check(!g->local_gameplay_ready && !g->local_player,
		"mixed: gameplay stays gated until retained replies can replay");
	check(h->local_player->health == host_health,
		"mixed: tagged guest health never falls back to host");
	if(g->local_phase != CLIENT_GUEST_ATTACHED || g->local_reply_failed) return;
	request_ready(s);
	clin_update_drain();
	check(g->local_player && g->local_gameplay_ready,
		"mixed: idempotent READY enables gameplay after successful replay");
	if(g->local_player) {
		check(g->local_player->health == 27,
			"mixed: ordered health 31 then 27 replayed to guest body");
		check(g->local_player->on_fire,
			"mixed: retained fire applied to installed guest body");
	}
	check(strcmp(g->hud_message, "guest admission notice") == 0
		&& g->sleeping && g->sleep_timer == 42,
		"mixed: HUD and sleep replayed before gameplay opens");
}
static void collision(struct server_local* s) {
	request_ready(s);
	uint32_t id = s->guest_entity_id;
	struct entity* occupant = calloc(1, sizeof(*occupant));
	assert(occupant);
	occupant->id = id;
	dict_entity_set_at(gstate.entities, id, occupant);
	clin_update_drain();
	client_session_flush_local(&gstate.players[1]);
	struct client_session* g = &gstate.players[1];
	/* Astra review A item 1: retirement is the authoritative Leave. */
	check(g->local_phase == CLIENT_GUEST_LEAVING && !g->local_gameplay_ready
		&& !g->local_player,
		"collision: occupied authoritative id immediately retires guest via Leave");
	check(visible_reason(g),
		"collision: persistent controller-readable refusal reason");
	check(dict_entity_get(gstate.entities, id)
		&& *dict_entity_get(gstate.entities, id) == occupant,
		"collision: existing entity remains untouched");
	host_surface("collision");
	/* The fixture owns the injected occupant, not the guest-body release. */
	dict_entity_erase(gstate.entities, id);
	free(occupant);
	settle_leave(s, "collision");
	rejoin_and_play(s, "collision");
}
static void exhaustion(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	for(unsigned n = 0; n < 32 && g->local_phase == CLIENT_GUEST_ATTACHED; n++) {
		if(!client_session_request_local_gameplay(g)) break;
		client_session_flush_local(g);
		server_local_test_process_messages(s);
		clin_test_fail_entity_slot_after(1);
		clin_update_drain();
	}
	client_session_flush_local(g);
	check(g->local_phase == CLIENT_GUEST_LEAVING
		|| g->local_phase == CLIENT_GUEST_FAILED || g->local_phase == CLIENT_GUEST_IDLE,
		"exhaustion: bounded retries terminate instead of looping forever");
	check(!g->local_gameplay_ready && !g->local_player,
		"exhaustion: no gameplay or body after retirement");
	check(visible_reason(g),
		"exhaustion: persistent controller-readable retry reason");
	/* Drive any queued authoritative leave through the real server. */
	for(unsigned n = 0; n < 3 && g->local_bound; n++) pump(s);
	check(!s->guest_actor && !s->guest_gameplay,
		"exhaustion: authoritative server guest body cleaned up");
}
static void overflow(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	bool survived_first = false;
	for(unsigned n = 0; n < 256 && g->local_phase == CLIENT_GUEST_ATTACHED; n++) {
		struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
		snprintf(hud.payload.hud_message, sizeof(hud.payload.hud_message),
			"distinct event %u", n);
		send_guest(&hud, g->local_generation);
		clin_update_drain();
		if(n == 0) survived_first = !g->local_reply_failed;
	}
	client_session_flush_local(g);
	check(survived_first,
		"overflow: a first retained HUD reply does not retire admission");
	check(g->local_phase != CLIENT_GUEST_ATTACHED && !g->local_gameplay_ready,
		"overflow: finite retained storage retires admission");
	check(visible_reason(g),
		"overflow: persistent controller-readable buffer reason");
	for(unsigned n = 0; n < 3 && g->local_bound; n++) pump(s);
	check(!s->guest_actor && !s->guest_gameplay,
		"overflow: authoritative server guest body cleaned up");
}
static void stale(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	struct client_rpc old = {.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = 3};
	struct client_rpc current = {.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = 29};
	/* A nonzero mismatched token reaches the tagged inbox, then the current
	 * generation fence must discard it. The first generation is 1. */
	send_guest(&old, g->local_generation + 7);
	send_guest(&current, g->local_generation);
	clin_update_drain();
	check(!g->local_reply_failed,
		"stale: mismatched generation dropped while current reply enters recovery");
	if(g->local_reply_failed) return;
	request_ready(s); clin_update_drain();
	check(g->local_player && g->local_player->health == 29,
		"stale: mismatched generation health dropped; current health replayed");
}
static void leave_reset(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	struct client_rpc health = {.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = 7};
	send_guest(&health, g->local_generation);
	clin_update_drain();
	check(!g->local_reply_failed,
		"leave/reset: current reply retained during first-body recovery");
	const int host_health = gstate.players[0].local_player->health;
	assert(client_session_request_local_leave(g));
	const uint64_t ticket = g->local_ticket, generation = g->local_generation;
	/* Deliver the matching authoritative status to the real client handler.
	 * This fixture has no chest snapshot, so server checkpointing itself can
	 * fail before it emits LEFT; the recovery teardown contract is client-side. */
	clin_rpc_send(&(struct client_rpc){.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket, .generation = generation,
			.result = LOCAL_GUEST_LEFT}});
	clin_update_drain();
	check(g->local_phase == CLIENT_GUEST_IDLE && !g->local_bound,
		"leave/reset: matching LEFT clears recovery generation");
	check(gstate.players[0].local_player->health == host_health,
		"leave/reset: retained guest health never falls back to host");
	/* WORLD_RESET uses the same real client entry point that discards a join. */
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	check(!g->local_player && !g->local_gameplay_ready,
		"leave/reset: reset cannot replay a retired guest body");
}
static void reset(struct server_local* s) {
	refused_first(s);
	struct client_session* g = &gstate.players[1];
	struct client_rpc health = {.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = 7};
	struct client_rpc hud = {.type = CRPC_HUD_MESSAGE};
	strcpy(hud.payload.hud_message, "retire this generation");
	send_guest(&health, g->local_generation);
	send_guest(&hud, g->local_generation);
	clin_update_drain();
	check(!g->local_reply_failed && !g->local_player,
		"reset: body-bound replies retained before world reset");
	const int host_health = gstate.players[0].local_player->health;
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	check(!g->local_bound && !g->local_player && !g->local_gameplay_ready
		&& g->hud_message[0] == '\0',
		"reset: world reset drops retained replies and gameplay gate");
	check(gstate.players[0].local_player == NULL
		|| gstate.players[0].local_player->health == host_health,
		"reset: no retained guest health falls back to host");
	(void)s;
}
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc != 2) return 2;
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
	struct server_local* s = setup();
	if(strcmp(argv[1], "mixed") == 0) mixed(s);
	else if(strcmp(argv[1], "collision") == 0) collision(s);
	else if(strcmp(argv[1], "exhaustion") == 0) exhaustion(s);
	else if(strcmp(argv[1], "overflow") == 0) overflow(s);
	else if(strcmp(argv[1], "stale") == 0) stale(s);
	else if(strcmp(argv[1], "leave_reset") == 0) leave_reset(s);
	else if(strcmp(argv[1], "reset") == 0) reset(s);
	else if(strcmp(argv[1], "retry_boundary") == 0) retry_boundary(s);
	else if(strcmp(argv[1], "retry_last") == 0) retry_last(s);
	else if(strcmp(argv[1], "overflow_boundary") == 0) overflow_boundary(s);
	else if(strcmp(argv[1], "timeout") == 0) timeout_case(s);
	else if(strcmp(argv[1], "notices") == 0) notices(s);
	else if(strcmp(argv[1], "cleanup_retry") == 0) cleanup_retry(s);
	else if(strcmp(argv[1], "notices_single") == 0) notices_single(s);
	else if(strcmp(argv[1], "hud_overflow") == 0) hud_overflow(s);
	else return 2;
	s->paused = true;
	/* A case whose authoritative Leave already released the guest leaves
	 * nothing to release (the release asserts a held guest). */
	if(s->guest_actor) server_local_guest_release_checkpointed(s);
	client_session_clear_windows(&gstate.players[1]);
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[1].visible_window = -1;
	clin_update_drain();
	split_destroy(s);
	printf("test_split_admission_recovery %s: %u FAILED\n", argv[1], failures);
	return failures ? 1 : 0;
}
