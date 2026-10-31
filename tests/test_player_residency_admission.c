/* Copyright (c) 2026 CaveWii_v2 contributors
 * residency-1005: player residency ADMISSION (Astra rulings
 * tmp/jobs/ASTRA-1005-RULING.md:1-3 and tmp/jobs/astra-1005b/
 * ASTRA-1005B-RULING.md:1-5).
 *
 * The mobs/items already freeze unless their +/-32 box is resident
 * (server_local.c entity pass, Beta World.updateEntityWithOptionalForce);
 * only the two PLAYER bodies can reach a hazard footprint whose chunk is not
 * resident, through their client-authored SRPC_PLAYER_POS samples. These
 * cases drive the REAL host handler (server_local_process) and the REAL guest
 * route (server_local_guest_position) on a live split fixture:
 *
 *   host / guest  a sample whose 0.6-wide body footprint touches a
 *       non-resident column is refused at once (no held sample, no replay),
 *       the authoritative pose kept, ONE correction authored on the owner's
 *       own route (host CRPC_PLAYER_POS, guest LOCAL_GUEST_CORRECTION) with a
 *       fresh token; the superseded token is still refused silently; no
 *       footstep/odometer side effect; the missing chunk comes back through
 *       the EXISTING near loader (no synchronous load in the handler); while
 *       an injected load failure keeps it missing the resident pose stays
 *       playable; once resident, fresh movement is admitted and the lava
 *       control applies Beta's 4-point hit. Negative X (chunk -1), a
 *       cross-chunk AABB, and the second world slot (per-world residency).
 *   unknown  an ALREADY-UNKNOWN body (a server-authored nudge across the
 *       resident edge) is restored to its last accepted pose with a
 *       correction and without replaying movement side effects; a body with
 *       no restorable pose (a teleport) gets no active hazard tick until its
 *       footprint is resident, then hazards resume.
 *
 * Beta: World.getBlockId answers through ChunkProviderServer.provideChunk,
 * which returns the EMPTY dummy chunk for a missing column unless a load
 * override is set (CFR World.java:206-214, ChunkProviderServer.java:98-103);
 * Beta never meets that case for a player because PlayerManager loads the
 * player's chunks as it moves (CFR PlayerInstance.java:59). Cave streams
 * asynchronously instead, so the ruling refuses the move until resident.
 *
 * One executable, argv[1] selects the case; each case owns a fresh fixture.
 * Non-aborting checks: a RED run lists every failure.
 */
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif

#define CAVEWII_F1_DISPATCH_ONLY
#include "../source/main.c"
#undef CAVEWII_F1_DISPATCH_ONLY
#include "support/split_residency_fixture.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/server_local_guest.h"
#include "network/server_local.h"
#include "network/server_transfer.h"
#include "network/client_world_payload.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define RES_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define RES_MKDIR(p) mkdir(p, 0777)
#endif

#define RZ_TEST_DEADLINE 200u /* RZ_RECOVERY_DEADLINE_TICKS */
#define RZ_TEST_FAIL_TEXT "Terrain failed to load - move to retry"
static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

/* ---- transport harness (test_owner_swap_pose_dead_sleep_barrier) ------- */
static unsigned markers;
static uint32_t update_done;
static unsigned host_pos_msgs;
static uint32_t host_pos_seq;
static double host_pos[3];
static unsigned guest_corrections;
static uint32_t guest_corr_seq;
static double guest_corr_pos[3];

static unsigned g_test_tick;
static unsigned hud_tick[64];
static char hud_text[64][48];
static unsigned hud_n;
static void hud_reset(void) { hud_n = 0; }
/* CRPC_MINECART_OCCUPANCY: the correlated ride-intent answer */
static unsigned occ_msgs;
static uint32_t occ_last_epoch, occ_last_seq;
static bool occ_last_occupied;
static unsigned hud_count(const char* text) {
	unsigned n = 0;
	for(unsigned i = 0; i < hud_n; i++)
		n += !strcmp(hud_text[i], text);
	return n;
}
/* LOCAL_GUEST_RESPAWN_RETRY / _LOADING_FAILED replies the server authored */
static unsigned guest_respawn_retry_msgs;
static unsigned load_failed_msgs; /* CRPC_LOAD_FAILED */
static uint64_t guest_respawn_retry_epoch;
/* residency-1005/6 R2: LOCAL_GUEST_JOIN_LOADING_FAILED replies consumed */
static unsigned join_failed_msgs;
static void consumed(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_LOAD_FAILED)
		load_failed_msgs++;
	if(rpc->type == CRPC_LOCAL_GUEST
	   && rpc->payload.local_guest.result == LOCAL_GUEST_JOIN_LOADING_FAILED)
		join_failed_msgs++;
	if(rpc->type == CRPC_LOCAL_GUEST
	   && (rpc->payload.local_guest.result == LOCAL_GUEST_RESPAWN_RETRY
		|| rpc->payload.local_guest.result
			== LOCAL_GUEST_RESPAWN_LOADING_FAILED)) {
		guest_respawn_retry_msgs++;
		guest_respawn_retry_epoch = rpc->payload.local_guest.respawn_epoch;
	}
	if(rpc->type == CRPC_PLAYER_POS) {
		if(rpc->payload.player_pos.input_epoch) {
			markers++;
		} else {
			host_pos_msgs++;
			host_pos_seq = rpc->payload.player_pos.correction_seq;
			host_pos[0] = rpc->payload.player_pos.position[0];
			host_pos[1] = rpc->payload.player_pos.position[1];
			host_pos[2] = rpc->payload.player_pos.position[2];
		}
	}
	if(rpc->type == CRPC_MINECART_OCCUPANCY) {
		occ_msgs++;
		occ_last_epoch = rpc->payload.minecart_occupancy.input_epoch;
		occ_last_seq = rpc->payload.minecart_occupancy.request_seq;
		occ_last_occupied = rpc->payload.minecart_occupancy.occupied;
	}
	if(rpc->type == CRPC_HUD_MESSAGE && hud_n < 64)
		hud_tick[hud_n] = g_test_tick,
		snprintf(hud_text[hud_n++], sizeof hud_text[0], "%s",
			rpc->payload.hud_message);
	if(rpc->type == CRPC_LOCAL_GUEST
	   && rpc->payload.local_guest.result == LOCAL_GUEST_CORRECTION) {
		guest_corrections++;
		guest_corr_seq = rpc->payload.local_guest.correction_seq;
		guest_corr_pos[0] = rpc->payload.local_guest.x;
		guest_corr_pos[1] = rpc->payload.local_guest.y;
		guest_corr_pos[2] = rpc->payload.local_guest.z;
	}
}
static void counters_reset(void) {
	host_pos_msgs = 0; host_pos_seq = 0;
	guest_corrections = 0; guest_corr_seq = 0;
	for(int i = 0; i < 3; i++)
		host_pos[i] = guest_corr_pos[i] = 0.0;
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
/* One real, UNPAUSED server update on this thread (it owns the archive). */
static void tick(struct server_local* s) {
	g_test_tick++;
	struct thread consumer;
	bool was_paused = s->paused;
	s->paused = false;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_local_test_update_once(s);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	s->paused = was_paused;
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
}
static void drain_server(struct server_local* s) {
	server_local_test_process_messages(s);
	ptime_t start = time_get();
	do {
		clin_update_drain();
		assert(time_diff_s(start, time_get()) < 20);
	} while(!clin_rpc_flush_pending());
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
	struct server_chunk* home
		= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert(home);
	home->sent = true;
}
static void guest_release(struct server_local* s) {
	s->paused = true;
	server_local_guest_release_checkpointed(s);
	client_session_clear_windows(&gstate.players[1]);
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[1].visible_window = -1;
	clin_update_drain();
}
static struct server_local* admitted_fixture(void) {
	struct server_local* s = split_fixture();
	s->paused = true;
	s->split_view_radius = 4;
	stream_to_marker(s);
	acknowledge(s);
	s->thread_started = true;
	enable_split(s);
	guest_attach(s);
	guest_body_home(s);
	struct client_session* guest_client = &gstate.players[1];
	assert(client_session_request_local_gameplay(guest_client));
	pump_handshake(s);
	assert(s->guest_gameplay && guest_client->local_gameplay_ready);
	assert(s->guest_actor && s->guest_actor->finished_loading);
	assert(s->player.has_pos && s->player.finished_loading);
	return s;
}

/* A second live world in slot 1, bound to this session, holding only the
 * columns a case loads into it explicitly. */
static char w1_path[128];
static void second_world_create(struct server_local* s) {
	test_fixture_dir("test_player_residency_admission_w1", w1_path,
		sizeof w1_path);
	RES_MKDIR(w1_path);
	RES_MKDIR(test_fixture_join(w1_path, "/region"));
	string_t level;
	string_init_set_str(level, w1_path);
	server_world_create(&s->worlds[1], level, WORLD_DIM_OVERWORLD, 12345,
		true);
	assert(s->worlds[1].generation != 0);
	string_clear(level);
	s->worlds[1].world_index = 1;
	s->worlds[1].session = s;
	s->worlds[1].local = s;
}
static void second_world_destroy(struct server_local* s) {
	server_world_discard(&s->worlds[1]);
	test_fixture_dir_cleanup(w1_path);
}

static void teardown(struct server_local* s) {
	guest_release(s);
	s->paused = true;
	split_destroy(s);
}

/* ---- the owner-parameterized sample --------------------------------------- */
struct owner {
	const char* name;
	bool guest;
	struct server_player* body;
};
static struct server_rpc pose_sample(double x, double y, double z, uint32_t seq) {
	return (struct server_rpc) {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x, .y = y, .z = z, .on_ground = true,
			.correction_seq = seq}};
}
static void sample(struct server_local* s, const struct owner* o, double x,
	double y, double z, uint32_t seq) {
	struct server_rpc call = pose_sample(x, y, z, seq);
	if(o->guest)
		assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
			.generation = s->guest_generation}, &call));
	else
		assert(svin_rpc_send(&call));
	drain_server(s);
}
static unsigned corrections(const struct owner* o) {
	return o->guest ? guest_corrections : host_pos_msgs;
}
static uint32_t correction_seq_seen(const struct owner* o) {
	return o->guest ? guest_corr_seq : host_pos_seq;
}
static const double* correction_pos(const struct owner* o) {
	return o->guest ? guest_corr_pos : host_pos;
}

/* Save with the retiring ring's designed backpressure handled
 * (server_world.c:7576, stage retiring_reserve, w->retiring_refused++): up to
 * 168 chunks saved back to back can outrun the 128-slot ring on slow disk IO,
 * and the save is then refused BY DESIGN until a write turns durable. Settle
 * the batch (end reconciles pending saves, freeing slots), retry once, and
 * reopen the batch if it was open. A refusal that did NOT bump
 * retiring_refused is a real write failure and still asserts. */
static bool save_chunk_bp(struct server_world* w, int cx, int cz) {
	uint32_t refused_before = w->retiring_refused;
	if(server_world_save_chunk(w, true, cx, cz))
		return true;
	assert(w->retiring_refused > refused_before);
	/* A retiring_reserve refusal implies the batch is open (server_world.c:7546);
	 * ending it must publish cleanly and makes the retry skip the ring. */
	bool end_ok = server_world_save_batch_end(w);
	assert(end_ok);
	bool ok = server_world_save_chunk(w, true, cx, cz);
	server_world_save_batch_begin(w);
	return ok;
}
static void evict(struct server_world* w, int cx, int cz) {
	assert(save_chunk_bp(w, cx, cz));
	assert(!server_world_chunk_resident(w, cx, cz));
}
static bool tick_until_resident(struct server_local* s, struct server_world* w,
	int cx, int cz, unsigned max_ticks) {
	for(unsigned i = 0; i < max_ticks; i++) {
		if(server_world_chunk_resident(w, cx, cz))
			return true;
		tick(s);
	}
	return server_world_chunk_resident(w, cx, cz);
}
static void put(struct server_world* w, int x, int y, int z, uint8_t type) {
	struct block_data b = {.type = type};
	if(!server_world_set_block(w, x, y, z, b)) {
		struct block_data live;
		assert(server_world_get_block(w, x, y, z, &live) && live.type == type);
	}
}
static void reset_body(struct server_player* p, double x, double y, double z) {
	p->x = x; p->y = y; p->z = z;
	p->health = MAX_PLAYER_HEALTH;
	p->hurt_resist_time = 0;
	p->last_damage = 0;
	p->fire = -PLAYER_FIRE_RESISTANCE;
	p->fall_distance = 0.0F;
	p->vel_x = p->vel_y = p->vel_z = 0.0F;
	p->on_ground = true;
}

#define LAVA_X 0
#define LAVA_Z 0
static double feet_y_of(const struct server_local* s) {
	return floor(s->player.y - PLAYER_EYE_HEIGHT);
}

/* ======================================================================== */
static void run_owner(struct server_local* s, const struct owner* o,
	struct server_player* other) {
	char label[256];
	struct server_world* w0 = &s->worlds[0];
	struct server_player* p = o->body;
	const double eye_y = s->player.y;
	const int lava_y = (int)feet_y_of(s);
	/* The other body stays clear of the lava cell and of chunk -1. */
	reset_body(other, 4.5, eye_y, 4.5);
	reset_body(p, 1.5, eye_y, 0.5);
	evict(w0, -1, 0);

	/* --- dry: refused at once, pose kept, one fresh correction --------- */
	counters_reset();
	uint32_t seq0 = p->correction_seq;
	float walked0 = p->distance_walked;
	sample(s, o, 0.1, eye_y, 0.5, seq0);
	snprintf(label, sizeof label, "%s dry: a sample whose body straddles "
		"non-resident chunk (-1,0) is REFUSED, pose kept", o->name);
	check(p->x == 1.5 && p->z == 0.5, label);
	snprintf(label, sizeof label, "%s dry: ONE correction on the owner's own "
		"route with a fresh token, carrying the kept pose", o->name);
	check(p->correction_seq != seq0 && corrections(o) == 1
		&& correction_seq_seen(o) == p->correction_seq
		&& correction_pos(o)[0] == 1.5 && correction_pos(o)[2] == 0.5, label);
	snprintf(label, sizeof label, "%s dry: no movement side effect "
		"(odometer unchanged)", o->name);
	check(p->distance_walked == walked0, label);
	snprintf(label, sizeof label, "%s dry: the handler loads nothing "
		"synchronously (chunk -1 still missing)", o->name);
	check(!server_world_chunk_resident(w0, -1, 0), label);

	/* --- the superseded token stays dead; the new one moves in residence */
	counters_reset();
	sample(s, o, 1.4, eye_y, 0.5, seq0);
	snprintf(label, sizeof label, "%s: a sample echoing the SUPERSEDED token "
		"is refused silently", o->name);
	check(p->x == 1.5 && corrections(o) == 0, label);
	/* x=1.6: the whole body box [1.3,1.9] stays in cell column 1, clear of
	 * the lava cell 0 placed below (Beta's burning box would reach it from
	 * x <= 1.3). */
	sample(s, o, 1.6, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s: a resident move on the fresh token is "
		"admitted (pose stays playable)", o->name);
	check(p->x == 1.6 && corrections(o) == 0, label);

	/* --- lava next to the missing column: refused, no hazard ---------- */
	put(w0, LAVA_X, lava_y, LAVA_Z, BLOCK_LAVA_STILL);
	counters_reset();
	uint32_t seq1 = p->correction_seq;
	sample(s, o, 0.1, eye_y, 0.5, seq1);
	snprintf(label, sizeof label, "%s lava: a footprint touching resident "
		"lava AND the missing column is refused + corrected", o->name);
	check(p->x == 1.6 && p->correction_seq != seq1 && corrections(o) == 1,
		label);

	/* --- load failure: the loader cannot bring (-1,0) back ------------- */
	server_world_test_fail_gen_attempts(100000);
	for(int i = 0; i < 6; i++)
		tick(s);
	snprintf(label, sizeof label, "%s load-failure: chunk -1 is still "
		"missing while loads fail", o->name);
	check(!server_world_chunk_resident(w0, -1, 0), label);
	printf("     [%s after failing loads: x=%.3f y=%.3f health=%d fire=%d "
		"fall=%.3f cause=%d]\n", o->name, p->x, p->y, p->health, p->fire,
		(double)p->fall_distance, (int)s->player_damage_cause);
	snprintf(label, sizeof label, "%s load-failure: the resident pose is "
		"kept and unharmed (no lava, no invented damage)", o->name);
	check(p->x == 1.6 && p->health == MAX_PLAYER_HEALTH
		&& p->fire <= 0, label);
	counters_reset();
	uint32_t seq2 = p->correction_seq;
	sample(s, o, 0.1, eye_y, 0.5, seq2);
	snprintf(label, sizeof label, "%s load-failure: the straddling move is "
		"still refused", o->name);
	check(p->x == 1.6 && corrections(o) == 1, label);
	sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s load-failure: resident movement still "
		"works", o->name);
	check(p->x == 1.5, label);

	/* --- retry: the existing near loader restores (-1,0) --------------- */
	server_world_test_fail_gen_attempts(0);
	bool back = tick_until_resident(s, w0, -1, 0, 600);
	snprintf(label, sizeof label, "%s retry: the existing bounded near "
		"loader makes chunk -1 resident again", o->name);
	check(back, label);
	snprintf(label, sizeof label, "%s retry: still unharmed at the resident "
		"pose", o->name);
	check(p->health == MAX_PLAYER_HEALTH && p->x == 1.5, label);
	counters_reset();
	sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s retry: once resident, fresh movement "
		"onto the cross-chunk footprint is admitted", o->name);
	check(p->x == 0.1 && corrections(o) == 0, label);
	p->hurt_resist_time = 0;
	tick(s);
	snprintf(label, sizeof label, "%s lava control: resident lava applies "
		"Beta's 4-point hit (Entity.onEntityUpdate:218)", o->name);
	check(p->health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2), label);
	put(w0, LAVA_X, lava_y, LAVA_Z, BLOCK_AIR);
	reset_body(p, 1.5, eye_y, 0.5);

	/* --- second world slot: residency is per world --------------------- */
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s->worlds[1], 0, 0, &sc) && sc);
	p->world_slot = 1;
	reset_body(p, 0.5, eye_y, 0.5);
	counters_reset();
	sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s world 1: refused while world 1 lacks "
		"(-1,0), although world 0 has it", o->name);
	check(p->x == 0.5 && corrections(o) == 1
		&& server_world_chunk_resident(w0, -1, 0), label);
	assert(server_world_load_chunk(&s->worlds[1], -1, 0, &sc) && sc);
	counters_reset();
	sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s world 1: admitted once world 1 holds "
		"(-1,0)", o->name);
	check(p->x == 0.1 && corrections(o) == 0, label);
	/* Leave world 1 first: its columns hold this body footprint, which the
	 * residency eviction veto (S3) protects while the body inhabits it. */
	p->world_slot = 0;
	reset_body(p, 1.5, eye_y, 0.5);
	evict(&s->worlds[1], -1, 0);
	evict(&s->worlds[1], 0, 0);
	reset_body(other, 1.5, eye_y, 0.5);
}

static void case_owner(bool guest) {
	struct server_local* s = admitted_fixture();
	second_world_create(s);
	struct owner o = {.name = guest ? "guest" : "host", .guest = guest,
		.body = guest ? s->guest_actor : &s->player};
	run_owner(s, &o, guest ? &s->player : s->guest_actor);
	second_world_destroy(s);
	teardown(s);
}

/* ======================================================================== */
/* A4: an already-UNKNOWN body.                                              */
static void case_unknown(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	const double eye_y = s->player.y;
	struct owner owners[2] = {
		{.name = "host", .guest = false, .body = &s->player},
		{.name = "guest", .guest = true, .body = s->guest_actor},
	};
	char label[256];
	for(int i = 0; i < 2; i++) {
		const struct owner* o = &owners[i];
		struct server_player* p = o->body;
		struct server_player* other = i == 0 ? s->guest_actor : &s->player;
		reset_body(other, 4.5, eye_y, 4.5);
		reset_body(p, 1.5, eye_y, 0.5);
		/* The last ACCEPTED pose: a real admitted sample, then one tick. */
		sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
		tick(s);
		check(p->x == 1.5, "setup: resident pose accepted");

		/* A server-authored nudge across the resident edge into (-1,0),
		 * whose loads fail: the body is now UNKNOWN. */
		evict(w0, -1, 0);
		server_world_test_fail_gen_attempts(100000);
		p->x = 0.1;
		p->fire = 100; /* a burning body: its cadence would tick */
		float walked = p->distance_walked;
		float fall = p->fall_distance;
		counters_reset();
		uint32_t seq = p->correction_seq;
		tick(s);
		snprintf(label, sizeof label, "%s unknown: restored to its last "
			"accepted pose (1.5)", o->name);
		check(p->x == 1.5 && p->z == 0.5, label);
		snprintf(label, sizeof label, "%s unknown: the restore is corrected "
			"on the owner's route with a fresh token", o->name);
		check(p->correction_seq != seq && corrections(o) == 1
			&& correction_pos(o)[0] == 1.5, label);
		snprintf(label, sizeof label, "%s unknown: no movement side effect "
			"replayed (odometer, fall)", o->name);
		check(p->distance_walked == walked && p->fall_distance == fall, label);

		/* A far server-authored pose (40.5, chunk 2) onto a missing column
		 * while the accepted pin (1.5) is resident: ELIGIBLE -- restored at
		 * any distance (ASTRA-RESIDENCY-REVIEW item 4 drops the <2-block
		 * rule). */
		evict(w0, 2, 0);
		p->x = 40.5;
		counters_reset();
		unsigned restores0 = p->rz.restores;
		tick(s);
		snprintf(label, sizeof label, "%s far: an eligible pin 39 blocks away "
			"is restored and corrected (no distance rule)", o->name);
		check(p->x == 1.5 && p->rz.restores == restores0 + 1
			&& corrections(o) == 1, label);

		/* No eligible pose (a fresh body with no pin): BOUNDED RECOVERY. */
		p->rz.pin.valid = false;
		p->x = 40.5;
		p->fire = 100;
		p->in_portal = true; /* stale portal contact */
		float dwell = p->time_in_portal;
		short health = p->health;
		unsigned rec0 = p->rz.recoveries, fail0 = p->rz.recovery_failures;
		unsigned notices0 = p->rz.notices;
		counters_reset();
		hud_reset();
		tick(s);
		snprintf(label, sizeof label, "%s recovery: entered, pose kept, no "
			"snap-back", o->name);
		check(p->rz.recovering && p->rz.recoveries == rec0 + 1
			&& p->x == 40.5 && corrections(o) == 0, label);
		bool need = false;
		for(unsigned k = 0; k < RZ_REQ_MAX; k++)
			need |= p->rz.req[k].used && p->rz.req[k].cx == 2
				&& p->rz.req[k].cz == 0 && p->rz.req[k].prio == RZ_PRIO_NEED;
		snprintf(label, sizeof label, "%s recovery: its footprint column is "
			"a NEED destination request", o->name);
		check(need, label);
		snprintf(label, sizeof label, "%s recovery: NO hazard tick on unknown "
			"data (fire cadence and health frozen)", o->name);
		check(p->fire == 100 && p->health == health, label);
		snprintf(label, sizeof label, "%s recovery: NO portal dwell / transfer "
			"from stale contact", o->name);
		check(p->time_in_portal == dwell && !p->portal_travel_requested,
			label);
		/* The loads keep failing: past the deadline the failure is explicit
		 * (counted + shown on the pane), then retried. */
		for(unsigned i = 0; i < 205; i++)
			tick(s);
		snprintf(label, sizeof label, "%s recovery: the pane is told "
			"(Loading terrain..., then the failure)", o->name);
		check(hud_count("Loading terrain...") >= 1
			&& hud_count(RZ_TEST_FAIL_TEXT) == 1, label);
		snprintf(label, sizeof label, "%s recovery: deadline hit -> one "
			"explicit retryable failure, still recovering", o->name);
		check(p->rz.recovery_failures == fail0 + 1 && p->rz.recovering
			&& p->fire == 100, label);
		snprintf(label, sizeof label, "%s recovery: toasts are rate-limited "
			"(<= 1 per 60 ticks over 206 ticks)", o->name);
		check(p->rz.notices - notices0 <= 5, label);
		p->in_portal = false;
		struct rz_session_slot* ss = &s->rz_sess[o->guest ? 1 : 0];
		const uint32_t attempt = ss->attempt;
		const unsigned ok0 = p->rz.recovery_ok;
		const unsigned max0 = p->rz.recovery_ticks_max;
		server_world_test_fail_gen_attempts(0);
		bool back = tick_until_resident(s, w0, 2, 0, 600);
		tick(s);
		/* R7 contract: late residency releases the physical hold, but
		 * cannot award success to the already failed attempt. */
		snprintf(label, sizeof label, "%s recovery: the loader makes the "
			"footprint resident; FAILED/200 stays immutable, no passive success",
			o->name);
		check(back && !p->rz.recovering && p->rz.failed
			&& p->rz.recovery_ok == ok0 && p->rz.recovery_ticks_max == max0
			&& p->rz.recovery_failures == fail0 + 1 && ss->attempt == attempt
			&& ss->last_attempt_result == 2
			&& ss->last_attempt_ticks == RZ_TEST_DEADLINE, label);
		tick(s);
		snprintf(label, sizeof label, "%s recovery: hazards resume once "
			"resident (fire cadence advances)", o->name);
		check(p->fire < 100, label);
		printf("     [%s recovery: %u ticks, notices %u]\n", o->name,
			p->rz.recovery_ticks_max, p->rz.notices - notices0);
		(void)tick_until_resident(s, w0, -1, 0, 600);
		reset_body(p, 1.5, eye_y, 0.5);
		tick(s);
	}
	teardown(s);
}


/* ======================================================================== */
/* Resident-world equivalence: with every footprint resident the gate must
 * be invisible. The SAME scripted run (both bodies walking samples across
 * the x=0 / z=0 chunk seams into resident negative chunks, the host through
 * a lava edge and the guest through water, a jump and a landing, with real
 * unpaused ticks) is traced per step -- both poses, health, fire, air, fall,
 * odometer, correction tokens, hurt window, and the server RNG streams
 * (rand_src, entity_rng) -- with the gate ON, OFF (the pre-residency-1005
 * path through server_local_test_set_residency_admission(false)) and ON
 * again (fixture determinism). All three traces must be byte-identical. */
#define EQUIV_STEPS 48
static uint64_t fnv(uint64_t h, const void* data, size_t n) {
	const unsigned char* b = data;
	for(size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 0x100000001b3ull;
	}
	return h;
}
static uint64_t body_hash(uint64_t h, const struct server_player* p) {
	h = fnv(h, &p->x, sizeof p->x);
	h = fnv(h, &p->y, sizeof p->y);
	h = fnv(h, &p->z, sizeof p->z);
	h = fnv(h, &p->health, sizeof p->health);
	h = fnv(h, &p->fire, sizeof p->fire);
	h = fnv(h, &p->oxygen, sizeof p->oxygen);
	h = fnv(h, &p->fall_distance, sizeof p->fall_distance);
	h = fnv(h, &p->distance_walked, sizeof p->distance_walked);
	h = fnv(h, &p->correction_seq, sizeof p->correction_seq);
	h = fnv(h, &p->hurt_resist_time, sizeof p->hurt_resist_time);
	h = fnv(h, &p->on_ground, sizeof p->on_ground);
	return h;
}
static void equiv_run(bool gate, uint64_t* trace, unsigned* rejects) {
	server_local_test_set_residency_admission(gate);
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double eye_y = s->player.y;
	const int ly = (int)feet_y_of(s);
	put(w0, -1, ly, 0, BLOCK_LAVA_STILL);
	put(w0, 6, ly, -1, BLOCK_WATER_STILL);
	reset_body(h, 1.5, eye_y, 0.5);
	reset_body(g, 6.5, eye_y, 1.5);
	tick(s);
	for(unsigned i = 0; i < EQUIV_STEPS; i++) {
		/* host: west along z=0.5 across the x=0 seam, through the lava
		 * edge at cell -1; guest: north across the z=0 seam through water,
		 * hopping on steps 20-23. */
		double hx = 1.5 - 0.06 * (double)(i + 1);
		double gz = 1.5 - 0.06 * (double)(i + 1);
		double gy = (i >= 20 && i < 24) ? eye_y + 0.25 * (double)(i - 19)
			: eye_y;
		struct owner ho = {.name = "host", .guest = false, .body = h};
		struct owner go = {.name = "guest", .guest = true, .body = g};
		sample(s, &ho, hx, eye_y, 0.5, h->correction_seq);
		sample(s, &go, 6.5, gy, gz, g->correction_seq);
		tick(s);
		uint64_t x = 0xcbf29ce484222325ull;
		x = body_hash(x, h);
		x = body_hash(x, g);
		x = fnv(x, &s->rand_src, sizeof s->rand_src);
		x = fnv(x, &s->entity_rng, sizeof s->entity_rng);
		trace[i] = x;
	}
	printf("EQSTATE host x=%.3f hp=%d fire=%d | guest z=%.3f y=%.3f hp=%d "
		"fire=%d walked=%.3f\n", h->x, h->health, h->fire, g->z, g->y,
		g->health, g->fire, (double)g->distance_walked);
	*rejects = h->rz.rejects + g->rz.rejects
		+ h->rz.restores + g->rz.restores;
	put(w0, -1, ly, 0, BLOCK_AIR);
	put(w0, 6, ly, -1, BLOCK_AIR);
	teardown(s);
	server_local_test_set_residency_admission(true);
}
/* One fixture per process (the client half is process-global), so each run
 * is a child process: "trace1" / "trace0" print the per-step trace. */
static void case_trace(bool gate) {
	static uint64_t tr[EQUIV_STEPS];
	unsigned rejects = 0;
	equiv_run(gate, tr, &rejects);
	for(unsigned i = 0; i < EQUIV_STEPS; i++)
		printf("TRACE %u %016llx\n", i, (unsigned long long)tr[i]);
	printf("REJECTS %u\n", rejects);
}
#ifdef _WIN32
#define RES_POPEN _popen
#define RES_PCLOSE _pclose
#else
#define RES_POPEN popen
#define RES_PCLOSE pclose
#endif
static bool read_trace(const char* exe, const char* mode, uint64_t* tr,
	unsigned* rejects) {
	char cmd[1024];
	snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, mode);
	FILE* f = RES_POPEN(cmd, "r");
	if(!f)
		return false;
	char line[512];
	unsigned seen = 0;
	bool have_rejects = false;
	while(fgets(line, sizeof line, f)) {
		unsigned i;
		unsigned long long v;
		if(sscanf(line, "TRACE %u %llx", &i, &v) == 2 && i < EQUIV_STEPS) {
			tr[i] = v;
			seen++;
		} else if(sscanf(line, "REJECTS %u", rejects) == 1)
			have_rejects = true;
	}
	int rc = RES_PCLOSE(f);
	return rc == 0 && seen == EQUIV_STEPS && have_rejects;
}
static void case_equiv(const char* exe) {
	static uint64_t on[EQUIV_STEPS], off[EQUIV_STEPS], on2[EQUIV_STEPS];
	unsigned r_on = 0, r_off = 0, r_on2 = 0;
	bool ok = read_trace(exe, "trace1", on, &r_on)
		&& read_trace(exe, "trace0", off, &r_off)
		&& read_trace(exe, "trace1", on2, &r_on2);
	check(ok, "equiv: three child runs (gate ON, OFF, ON) completed");
	if(!ok)
		return;
	unsigned first_off = EQUIV_STEPS, first_det = EQUIV_STEPS;
	for(unsigned i = EQUIV_STEPS; i-- > 0;) {
		if(on[i] != off[i]) first_off = i;
		if(on[i] != on2[i]) first_det = i;
	}
	printf("     [equiv: final ON %016llx OFF %016llx ON2 %016llx; first "
		"ON/OFF divergence step %u]\n",
		(unsigned long long)on[EQUIV_STEPS - 1],
		(unsigned long long)off[EQUIV_STEPS - 1],
		(unsigned long long)on2[EQUIV_STEPS - 1], first_off);
	check(first_det == EQUIV_STEPS,
		"equiv: the fixture is deterministic (gate ON twice, identical)");
	check(first_off == EQUIV_STEPS,
		"equiv: fully resident world -- gate ON and OFF traces byte-identical "
		"at every step (poses, hazards, tokens, RNG)");
	check(r_on == 0 && r_on2 == 0,
		"equiv: no refusal or restore ever fired in the resident run");
}

/* ======================================================================== */
/* S1 measurement (ASTRA-RESIDENCY-REVIEW item 2): PC loader latency under a
 * backlog. Every resident column of the ring outside the bodies' 3x3 is
 * evicted (a sprint/teleport-scale backlog), then the server ticks until a
 * target column two chunks ahead is resident again. Reported, not gated:
 * ticks, wall time, columns made resident, wall time per column. */
static unsigned evict_backlog(struct server_world* w) {
	unsigned n = 0;
	for(int cz = -6; cz <= 6; cz++)
		for(int cx = -6; cx <= 6; cx++) {
			if(cx >= -1 && cx <= 1 && cz >= -1 && cz <= 1)
				continue;
			if(server_world_chunk_resident(w, cx, cz)) {
				assert(save_chunk_bp(w, cx, cz));
				n++;
			}
		}
	return n;
}
static unsigned resident_count(struct server_world* w) {
	unsigned n = 0;
	for(int cz = -6; cz <= 6; cz++)
		for(int cx = -6; cx <= 6; cx++)
			n += server_world_chunk_resident(w, cx, cz);
	return n;
}
static void measure_latency(struct server_local* s, const char* label,
	int tcx, int tcz, void (*prime)(struct server_local*)) {
	struct server_world* w0 = &s->worlds[0];
	unsigned backlog = evict_backlog(w0);
	unsigned before = resident_count(w0);
	if(prime)
		prime(s);
	ptime_t t0 = time_get();
	unsigned ticks = 0;
	while(!server_world_chunk_resident(w0, tcx, tcz) && ticks < 2000) {
		tick(s);
		ticks++;
	}
	float ms = time_diff_s(t0, time_get()) * 1000.0f;
	unsigned loaded = resident_count(w0) - before;
	printf("LATENCY %s target=(%d,%d) backlog=%u ticks=%u wall_ms=%.1f "
		"columns_loaded=%u ms_per_column=%.2f\n", label, tcx, tcz, backlog,
		ticks, (double)ms, loaded,
		loaded ? (double)ms / (double)loaded : 0.0);
	char l[160];
	snprintf(l, sizeof l, "latency %s: the target column became resident",
		label);
	check(server_world_chunk_resident(w0, tcx, tcz), l);
}
static void case_latency(void) {
	struct server_local* s = admitted_fixture();
	measure_latency(s, "nearest-first", 3, 0, NULL);
	measure_latency(s, "nearest-first-diag", 3, -3, NULL);
	/* Fresh terrain (real generation + population): both bodies moved 4096
	 * blocks east, where nothing was ever generated. */
	{
		struct server_world* w0 = &s->worlds[0];
		const double eye_y = s->player.y;
		reset_body(&s->player, 4096.5, eye_y, 0.5);
		reset_body(s->guest_actor, 4097.5, eye_y, 0.5);
		ptime_t t0 = time_get();
		unsigned ticks = 0;
		unsigned before = 0;
		for(int cz = -6; cz <= 6; cz++)
			for(int cx = 250; cx <= 262; cx++)
				before += server_world_chunk_resident(w0, cx, cz);
		while(!server_world_chunk_resident(w0, 259, 0) && ticks < 4000) {
			tick(s);
			ticks++;
		}
		unsigned after = 0;
		for(int cz = -6; cz <= 6; cz++)
			for(int cx = 250; cx <= 262; cx++)
				after += server_world_chunk_resident(w0, cx, cz);
		float ms = time_diff_s(t0, time_get()) * 1000.0f;
		printf("LATENCY fresh-generation target=(259,0) ticks=%u wall_ms=%.1f "
			"columns_loaded=%u ms_per_column=%.2f body_column_resident=%d\n",
			ticks, (double)ms, after - before,
			after > before ? (double)ms / (double)(after - before) : 0.0,
			(int)server_world_chunk_resident(w0, 256, 0));
		check(server_world_chunk_resident(w0, 259, 0),
			"latency fresh-generation: the target column became resident");
	}
	teardown(s);
}

/* ======================================================================== */
/* S2: directional load-ahead + fairness (ASTRA-RESIDENCY-REVIEW item 1).
 * Real admitted host samples establish a cart-speed velocity; with the ring
 * evicted around the body (only its own 3x3 kept) the column 32 blocks
 * ahead is requested and served FIRST, ahead of nearest-first order, and
 * the same backlog served without the load-ahead (velocity 0) reaches it
 * later. The guest 256 blocks away keeps its share (round-robin). Queued is
 * never resident: a refused move stays refused until the column is. */
static unsigned evict_ring_around(struct server_world* w, int ccx, int ccz,
	int keep) {
	unsigned n = 0;
	for(int cz = ccz - 6; cz <= ccz + 6; cz++)
		for(int cx = ccx - 6; cx <= ccx + 6; cx++) {
			if(abs(cx - ccx) <= keep && abs(cz - ccz) <= keep)
				continue;
			if(server_world_chunk_resident(w, cx, cz)) {
				assert(save_chunk_bp(w, cx, cz));
				n++;
			}
		}
	return n;
}
static unsigned ticks_until_resident(struct server_local* s,
	struct server_world* w, int cx, int cz, unsigned max) {
	unsigned t = 0;
	while(!server_world_chunk_resident(w, cx, cz) && t < max) {
		tick(s);
		t++;
	}
	return t;
}
static void case_loadahead(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double eye_y = s->player.y;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	char label[200];
	reset_body(g, 4.5, eye_y, 4.5);

	/* --- control: the same backlog with NO motion (nearest-first) ----- */
	int32_t px[32], pz[32];
	bool prz[32];
	reset_body(h, 12.5, eye_y, 8.5);
	sample(s, &ho, 12.5, eye_y, 8.5, h->correction_seq);
	tick(s);
	sample(s, &ho, 12.5, eye_y, 8.5, h->correction_seq);
	unsigned backlog0 = evict_ring_around(w0, 0, 0, 1);
	server_local_test_near_picks_reset();
	tick(s);
	unsigned n0 = server_local_test_near_picks(px, pz, prz, 32);
	int idx0 = -1;
	for(unsigned i = 0; i < n0 && i < 32; i++)
		if(px[i] == 2 && pz[i] == 0 && idx0 < 0)
			idx0 = (int)i;
	printf("LOADAHEAD control (v=0): backlog=%u picks=%u first=(%d,%d)%s "
		"(2,0) at pick %d\n", backlog0, n0, n0 ? px[0] : 0, n0 ? pz[0] : 0,
		n0 && prz[0] ? " rz" : "", idx0);
	check(n0 && !prz[0] && !(px[0] == 2 && pz[0] == 0),
		"loadahead control: with no motion the loader starts nearest-first "
		"(not at the east column)");

	/* --- east at 0.8 block/tick: lead = min(0.8*40, 32) = 32 blocks ---- */
	(void)ticks_until_resident(s, w0, 4, 4, 400);
	sample(s, &ho, 12.5, eye_y, 8.5, h->correction_seq);
	tick(s);
	sample(s, &ho, 13.3, eye_y, 8.5, h->correction_seq);
	check(h->x == 13.3 && fabsf(h->rz.vx - 0.8F) < 1e-4F,
		"loadahead: admitted samples give the measured velocity 0.8 b/t");
	unsigned backlog1 = evict_ring_around(w0, 0, 0, 1);
	unsigned req1 = h->rz.requests, served1 = h->rz.picked;
	server_local_test_near_picks_reset();
	tick(s);
	unsigned n1 = server_local_test_near_picks(px, pz, prz, 32);
	printf("LOADAHEAD east (v=0.8): backlog=%u picks=%u first=(%d,%d)%s "
		"requests=%u served=%u\n", backlog1, n1, n1 ? px[0] : 0,
		n1 ? pz[0] : 0, n1 && prz[0] ? " rz" : "", h->rz.requests - req1,
		h->rz.picked - served1);
	check(h->rz.requests > req1 && h->rz.picked > served1,
		"loadahead: the column 32 blocks ahead was requested and served");
	check(n1 && prz[0] && px[0] == 2 && pz[0] == 0,
		"loadahead: the path column (2,0) is the loader's FIRST pick, ahead "
		"of the nearest-first order");
	check(server_world_chunk_resident(w0, 2, 0),
		"loadahead: the path column is resident after the first pass");
	unsigned loaded1 = h->rz.loaded;
	tick(s);
	check(h->rz.loaded > loaded1,
		"loadahead: SERVED is counted once the column is resident (loaded), "
		"not at the pick");

	/* --- queued is never resident ------------------------------------- */
	evict_ring_around(w0, 0, 0, 0);
	server_world_test_fail_gen_attempts(100000);
	reset_body(h, 14.5, eye_y, 8.5);
	sample(s, &ho, 14.5, eye_y, 8.5, h->correction_seq);
	counters_reset();
	sample(s, &ho, 16.2, eye_y, 8.5, h->correction_seq);
	tick(s);
	bool queued = false;
	for(unsigned k = 0; k < RZ_REQ_MAX; k++)
		queued |= h->rz.req[k].used && h->rz.req[k].cx == 1
			&& h->rz.req[k].cz == 0 && h->rz.req[k].prio == RZ_PRIO_NEED;
	check(queued, "queued: the refused footprint column (1,0) is a NEED "
		"request while loads fail");
	sample(s, &ho, 16.2, eye_y, 8.5, h->correction_seq);
	check(h->x == 14.5, "queued: a queued (non-resident) column is still "
		"refused -- queued is never resident");
	server_world_test_fail_gen_attempts(0);
	(void)ticks_until_resident(s, w0, 1, 0, 200);
	sample(s, &ho, 16.2, eye_y, 8.5, h->correction_seq);
	check(h->x == 16.2, "queued: once resident the fresh retry is admitted");
	printf("     [latency first refusal -> admission: %u ticks]\n",
		h->rz.latency_last);

	/* --- fairness: guest 256 blocks east, both bodies moving ---------- */
	/* Fairness is a count/order property of one loader pass; the 14 ms
	 * wall-clock gen window (server_world_gen_budget_spent) could end the
	 * pass after the host's column on a loaded PC (full -j8 suite), so the
	 * guest went unserved. Count-bounded windows only for this check. */
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	reset_body(g, 256.5 + 8.0, eye_y, 8.5);
	(void)ticks_until_resident(s, w0, 16, 0, 400);
	tick(s);
	struct owner go = {.name = "guest", .guest = true, .body = g};
	reset_body(h, 12.5, eye_y, 8.5);
	sample(s, &ho, 12.5, eye_y, 8.5, h->correction_seq);
	sample(s, &go, 264.5, eye_y, 8.5, g->correction_seq);
	tick(s);
	sample(s, &ho, 13.3, eye_y, 8.5, h->correction_seq);
	sample(s, &go, 265.3, eye_y, 8.5, g->correction_seq);
	evict_ring_around(w0, 0, 0, 1);
	evict_ring_around(w0, 16, 0, 1);
	unsigned hs = h->rz.picked, gs = g->rz.picked;
	tick(s);
	printf("FAIRNESS one tick: host served %u, guest served %u\n",
		h->rz.picked - hs, g->rz.picked - gs);
	check(h->rz.picked > hs && g->rz.picked > gs,
		"fairness: both bodies (256 blocks apart) are served in the same "
		"loader pass");
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	teardown(s);
}

/* ======================================================================== */
/* Round 3 item 5: BOTH streaming evictors, through their real tick paths.
 *  near: the host corpse dwells while the stream is anchored at a far
 *        respawn point (host_respawn_anchor) -- the slot-0 near evictor
 *        drains the old ring around it but never the host's pinned
 *        footprint, and does not stall on it.
 *  A2-W: the guest in world 1 is moved far by a direct server pose write
 *        before any settle -- the A2-W evictor drains world 1's old ring but
 *        keeps the still-pinned footprint column. */
static unsigned resident_in(struct server_world* w, int x0, int x1, int z0,
	int z1) {
	unsigned n = 0;
	for(int z = z0; z <= z1; z++)
		for(int x = x0; x <= x1; x++)
			n += server_world_chunk_resident(w, x, z);
	return n;
}
static void case_evictors(void) {
	struct server_local* s = admitted_fixture();
	second_world_create(s);
	struct server_world* w0 = &s->worlds[0];
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double eye_y = s->player.y;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	reset_body(g, 4.5, eye_y, 4.5);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	check(h->rz.pin.valid, "evictors: the host holds a pin at (0,0)");
	/* --- near evictor: stream anchored far away ----------------------- */
	reset_body(g, 320.5, eye_y, 0.5);
	s->host_respawn_x = 320.5;
	s->host_respawn_z = 0.5;
	s->host_respawn_anchor = true;
	s->host_respawn_phase = HOST_RESPAWN_PHASE_PREPARED;
	unsigned before = resident_in(w0, -4, 4, -4, 4);
	for(int i = 0; i < 60; i++)
		tick(s);
	unsigned after = resident_in(w0, -4, 4, -4, 4);
	printf("     [near evictor: old ring %u -> %u resident; (0,0) %s]\n",
		before, after, server_world_chunk_resident(w0, 0, 0) ? "kept"
			: "EVICTED");
	check(after < before && server_world_chunk_resident(w0, 0, 0),
		"evictors: the slot-0 near evictor drains the old ring around the "
		"host's pinned footprint and keeps (0,0)");
	check(after <= 9, "evictors: it does not stall on the protected column "
		"(only the pinned 3x3 may remain)");
	s->host_respawn_phase = HOST_RESPAWN_PHASE_IDLE;
	s->host_respawn_anchor = false;
	reset_body(g, 4.5, eye_y, 4.5);
	(void)ticks_until_resident(s, w0, 4, 4, 600);
	/* --- A2-W evictor: world 1 ----------------------------------------- */
	struct server_world* w1 = &s->worlds[1];
	struct server_chunk* sc = NULL;
	for(int z = -2; z <= 2; z++)
		for(int x = -2; x <= 2; x++)
			assert(server_world_load_chunk(w1, x, z, &sc) && sc);
	s->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;
	g->world_slot = 1;
	reset_body(g, 1.5, eye_y, 0.5);
	tick(s);
	check(g->rz.pin.valid && g->rz.pin.world_index == 1,
		"evictors: the guest holds a pin in world 1");
	server_transfer_stream_enable(s, 1);
	g->x += 16.0 * (server_local_view_radius(s) + 10);
	unsigned w1_before = resident_in(w1, -2, 2, -2, 2);
	for(int i = 0; i < 4; i++)
		server_transfer_stream_step(s);
	unsigned w1_after = resident_in(w1, -2, 2, -2, 2);
	printf("     [A2-W evictor: world 1 old ring %u -> %u; (0,0) %s]\n",
		w1_before, w1_after, server_world_chunk_resident(w1, 0, 0) ? "kept"
			: "EVICTED");
	check(w1_after < w1_before && server_world_chunk_resident(w1, 0, 0),
		"evictors: the A2-W evictor drains world 1 but keeps the guest's "
		"pinned footprint column");
	g->x = 1.5;
	g->world_slot = 0;
	reset_body(g, 4.5, eye_y, 4.5);
	tick(s);
	for(int z = -2; z <= 2; z++)
		for(int x = -2; x <= 2; x++)
			if(server_world_chunk_resident(w1, x, z))
				evict(w1, x, z);
	second_world_destroy(s);
	teardown(s);
}

/* ======================================================================== */
/* Round 3 item 5: correction OUTPUT queue saturation + rider consistency.
 * The client inbox is filled to capacity BEFORE the refused samples are
 * processed: the server retains its correction in the pending output ring
 * until the client drains; the correction is delivered with
 * the live token, the stale in-flight samples are retired, and nothing is
 * replayed. A refused rider sample drops its rider input. */
static void saturation_one(struct server_local* s, const struct owner* o) {
	struct server_player* p = o->body;
	const double eye_y = s->player.y;
	char label[220];
	reset_body(p, 1.5, eye_y, 0.5);
	sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
	tick(s);
	server_world_test_fail_gen_attempts(100000);
	evict(&s->worlds[0], -1, 0);
	counters_reset();
	uint32_t seq0 = p->correction_seq;
	struct server_rpc_source source = {.slot = o->guest ? 1u : 0u,
		.generation = o->guest ? s->guest_generation : 0};
	/* the refused fresh sample + 6 stale-in-flight ones on the same token */
	for(int i = 0; i < 7; i++) {
		struct server_rpc call = pose_sample(0.1 + 0.01 * i, eye_y, 0.5, seq0);
		assert(svin_credit_reserve(source));
		svin_credit_publish(source, &call);
	}
	struct client_rpc filler = {.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = 2}};
	unsigned queued = 0;
	while(queued < 1024 && clin_try_rpc_send(&filler))
		queued++;
	bool was_paused = s->paused;
	s->paused = false;
	server_local_test_update_once(s); /* archive owner; real network phase */
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	snprintf(label, sizeof label, "%s saturation: with the client inbox full "
		"(%u fillers) the correction is retained without early delivery",
		o->name, queued);
	check(queued > 0 && pending > 0 && corrections(o) == 0, label);
	s->paused = was_paused;
	while(!clin_rpc_flush_pending())
		clin_update_drain();
	clin_update_drain();
	snprintf(label, sizeof label, "%s saturation: after the drain exactly ONE "
		"correction is delivered, carrying the live token; the 6 stale "
		"samples were retired; pose kept (no replay)", o->name);
	check(corrections(o) == 1 && correction_seq_seen(o) == p->correction_seq
		&& p->correction_seq != seq0 && p->x == 1.5, label);
	/* retry on the delivered token is processed normally */
	counters_reset();
	sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s saturation: the client retry on the "
		"delivered token is answered again (still non-resident)", o->name);
	check(corrections(o) == 1 && p->x == 1.5, label);
	/* rider consistency */
	p->rider_vel_x = 0.3F;
	p->rider_vel_z = -0.2F;
	sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s rider: a refused rider sample drops the "
		"rider input that would drive the vehicle at the missing column",
		o->name);
	check(p->rider_vel_x == 0.0F && p->rider_vel_z == 0.0F, label);
	server_world_test_fail_gen_attempts(0);
	(void)ticks_until_resident(s, &s->worlds[0], -1, 0, 400);
}
static void case_saturation(void) {
	struct server_local* s = admitted_fixture();
	const double eye_y = s->player.y;
	struct owner ho = {.name = "host", .guest = false, .body = &s->player};
	struct owner go = {.name = "guest", .guest = true, .body = s->guest_actor};
	reset_body(s->guest_actor, 4.5, eye_y, 4.5);
	saturation_one(s, &ho);
	reset_body(&s->player, 4.5, eye_y, 4.5);
	saturation_one(s, &go);
	teardown(s);
}

/* ======================================================================== */
/* Round 3 item 6: an AUTHENTIC residency tape for the collector gate. Both
 * bodies walk 48 blocks east (real admitted samples, real loader, real
 * requests/loads); every tick the REAL formatter (hw_telemetry_format_
 * heartbeat) writes one heartbeat line carrying the per-actor "rza" rows
 * the server published. tools/wii_telemetry_server.py --residency-gate-tape
 * must PASS it and FAIL its negative mutations (collector selftest). */
#include "platform/hw_telemetry.h"
static void rz_fill_heartbeat(struct hw_telemetry_heartbeat* hb) {
	struct server_local_rz_wire w;
	server_local_rz_actor_telemetry(&w);
	memcpy(hb->rza, w.rza, sizeof hb->rza);
	memcpy(hb->rze, w.rze, sizeof hb->rze);
	hb->rzf = w.rzf;
	memcpy(hb->rzi, w.rzi, sizeof hb->rzi);
	hb->rz_tick = w.tick;
	hb->rzi_count = w.rzi_count;
}
static void case_tape(const char* out_path) {
	struct server_local* s = admitted_fixture();
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	reset_body(h, 1.5, eye_y, 0.5);
	reset_body(g, 1.5, eye_y, 6.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	sample(s, &go, 1.5, eye_y, 6.5, g->correction_seq);
	tick(s);
	FILE* f = fopen(out_path, "wb");
	check(f != NULL, "tape: output file opened");
	if(!f) {
		teardown(s);
		return;
	}
	static char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	unsigned lines = 0;
	for(int i = 0; i < 160; i++) {
		/* a sprint-scale backlog ahead of both bodies, twice */
		if(i == 2 || i == 60) {
			int cx = (int)floor(h->x) >> 4;
			(void)evict_ring_around(&s->worlds[0], cx, 0, 0);
		}
		if(i < 120) {
			double hx = 1.5 + 0.4 * (double)(i + 1);
			if(h->x != hx)
				sample(s, &ho, hx, eye_y, 0.5, h->correction_seq);
			if(g->x != hx)
				sample(s, &go, hx, eye_y, 6.5, g->correction_seq);
		}
		tick(s);
		struct hw_telemetry_heartbeat hb;
		memset(&hb, 0, sizeof hb);
		hb.tick = (uint32_t)i;
		hb.world_loaded = true;
		hb.x = (float)h->x;
		hb.y = (float)h->y;
		hb.z = (float)h->z;
		hb.storage_enabled = true; /* the block that carries gx/gz */
		hb.guest = true;
		hb.gx = (float)g->x;
		hb.gz = (float)g->z;
		rz_fill_heartbeat(&hb);
		size_t n = hw_telemetry_format_heartbeat(line, sizeof line,
			"residency_tape_pc", "split_play_8m", &hb);
		if(n > 0 && n < sizeof line) {
			fputs(line, f);
			fputc('\n', f);
			lines++;
		}
	}
	fclose(f);
	printf("     [tape: %u heartbeat lines; host x=%.1f guest x=%.1f; host "
		"requests=%u loaded=%u, guest requests=%u loaded=%u]\n", lines, h->x,
		g->x, h->rz.requests, h->rz.loaded, g->rz.requests, g->rz.loaded);
	check(lines == 160 && h->x >= 49.0 && g->x >= 49.0,
		"tape: 160 real heartbeat lines, both bodies moved >= 48 blocks");
	teardown(s);
}

/* ======================================================================== */
/* Round 3 item 4 + Round 4 item 7: feedback. Measured from the RECORDED
 * recovery-entry tick E (the update that armed the attempt): an unsuccessful
 * attempt's terminal failure reaches the affected pane by E+200 (no grace
 * tick), every pair of residency toasts on that pane is >= 60 ticks apart,
 * and each toast reaches only its own pane (gstate.players[0] host, [1]
 * guest). Legs: a recent PRE-ENTRY toast (a refusal streak's "Loading
 * terrain..." just before the body goes unknown), a SUCCESS just before the
 * deadline (no failure toast), a deliberate RETRY episode (second failure,
 * spacing across both episodes), and a SATURATED client output queue at the
 * deadline (the lossless route blocks, then delivers exactly one notice to
 * the right pane once the transport drains -- not a <= 200 witness). */
static unsigned toast_min_gap(unsigned from) {
	unsigned min_gap = 100000;
	for(unsigned i = from + 1; i < hud_n; i++)
		if(hud_tick[i] - hud_tick[i - 1] < min_gap)
			min_gap = hud_tick[i] - hud_tick[i - 1];
	return min_gap;
}
static unsigned toast_fail_tick(unsigned from, unsigned* count) {
	unsigned at = 0, n = 0;
	for(unsigned i = from; i < hud_n; i++)
		if(!strcmp(hud_text[i], RZ_TEST_FAIL_TEXT)) {
			n++;
			if(!at)
				at = hud_tick[i];
		}
	*count = n;
	return at;
}
/* Tick until the body ENTERS recovery; returns the entry tick E. */
static unsigned toast_enter(struct server_local* s, struct server_player* p) {
	for(int i = 0; i < 5; i++) {
		tick(s);
		if(p->rz.recovering)
			return g_test_tick;
	}
	return 0;
}
static void toast_unknown(struct server_world* w, struct server_player* p) {
	server_world_test_fail_gen_attempts(100000);
	if(server_world_chunk_resident(w, 3, 0))
		evict(w, 3, 0);
	p->rz.pin.valid = false;
	p->x = 56.5;
	p->z = 0.5;
}
static void toast_recover(struct server_local* s, const struct owner* o) {
	struct server_player* p = o->body;
	char label[220];
	const unsigned ok0 = p->rz.recovery_ok, retries0 = p->rz.retries;
	server_world_test_fail_gen_attempts(0);
	(void)ticks_until_resident(s, &s->worlds[0], 3, 0, 400);
	for(int i = 0; i < 5 && p->rz.recovering; i++)
		tick(s);
	snprintf(label, sizeof label, "%s toast setup: late terrain releases "
		"the hold without clearing failure or awarding success", o->name);
	check(server_world_chunk_resident(&s->worlds[0], 3, 0)
		&& !p->rz.recovering && p->rz.failed && p->rz.recovery_ok == ok0,
		label);
	/* R7 requires a deliberate fresh owner input between failed episodes.
	 * Direct fixture placement in the next leg is not such an input. */
	sample(s, o, p->x + 0.25, p->y, p->z, p->correction_seq);
	for(int i = 0; i < 5 && p->rz.recovering; i++)
		tick(s);
	snprintf(label, sizeof label, "%s toast setup: real owner retry "
		"completes once before the next independent leg", o->name);
	check(!p->rz.recovering && !p->rz.failed
		&& p->rz.retries == retries0 + 1 && p->rz.recovery_ok == ok0 + 1,
		label);
}
static void toasts_one(struct server_local* s, const struct owner* o) {
	struct server_player* p = o->body;
	struct server_world* w0 = &s->worlds[0];
	const double eye_y = s->player.y;
	char label[220];
	const struct client_session* mine = &gstate.players[o->guest ? 1 : 0];
	const struct client_session* theirs = &gstate.players[o->guest ? 0 : 1];
	memset(gstate.players[0].hud_message, 0, sizeof gstate.players[0].hud_message);
	memset(gstate.players[1].hud_message, 0, sizeof gstate.players[1].hud_message);
	hud_reset();

	/* --- leg A: a recent pre-entry toast, then an unsuccessful attempt -- */
	reset_body(p, 1.5, eye_y, 0.5);
	sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
	tick(s);
	server_world_test_fail_gen_attempts(100000);
	evict(w0, -1, 0);
	for(int i = 0; i < 24; i++) { /* a refusal streak toasts at age 20 */
		sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
		tick(s);
	}
	unsigned pre = hud_n;
	toast_unknown(w0, p);
	unsigned entry = toast_enter(s, p);
	for(unsigned i = 0; i < RZ_TEST_DEADLINE + 20; i++)
		tick(s);
	unsigned fails = 0;
	unsigned fail_at = toast_fail_tick(0, &fails);
	printf("     [%s leg A: pre-entry toasts %u, entry E=%u, failure at E+%u, "
		"%u toasts, min gap %u]\n", o->name, pre, entry, fail_at - entry,
		hud_n, toast_min_gap(0));
	snprintf(label, sizeof label, "%s toasts A: a recent pre-entry toast "
		"exists (the streak's loading notice)", o->name);
	check(pre >= 1 && entry != 0, label);
	snprintf(label, sizeof label, "%s toasts A: exactly ONE terminal failure, "
		"delivered by E+200 measured from the recorded entry tick", o->name);
	check(fails == 1 && fail_at >= entry && fail_at - entry <= RZ_TEST_DEADLINE,
		label);
	snprintf(label, sizeof label, "%s toasts A: spacing >= 60 ticks between "
		"ALL residency toasts of the pane (pre-entry one included)", o->name);
	check(hud_n >= 2 && toast_min_gap(0) >= 60, label);
	snprintf(label, sizeof label, "%s toasts A: routed to its OWN pane only",
		o->name);
	check(strstr(mine->hud_message, "Terrain") != NULL
		&& strstr(theirs->hud_message, "Terrain") == NULL, label);

	/* --- leg C: a deliberate retry episode, failing again ---------------- */
	unsigned before_c = hud_n;
	sample(s, o, 57.5, eye_y, 0.5, p->correction_seq); /* the owner moves */
	unsigned entry2 = p->rz.recovering && !p->rz.failed ? g_test_tick : 0;
	/* armed in the dispatcher with this update as its entry (recover_start) */
	for(unsigned i = 0; i < RZ_TEST_DEADLINE + 20; i++)
		tick(s);
	unsigned fails2 = 0;
	unsigned fail2_at = toast_fail_tick(before_c, &fails2);
	printf("     [%s leg C: retry entry %u, failure at E+%u, gap to previous "
		"failure %u, min gap %u]\n", o->name, entry2, fail2_at - entry2,
		fail2_at - fail_at, toast_min_gap(0));
	snprintf(label, sizeof label, "%s toasts C: the retried attempt fails "
		"ONCE by its own E+200; spacing holds across both episodes", o->name);
	check(entry2 && fails2 == 1 && fail2_at - entry2 <= RZ_TEST_DEADLINE
		&& toast_min_gap(0) >= 60, label);
	toast_recover(s, o);

	/* --- leg B: success just before the deadline (no failure toast) ----- */
	reset_body(p, 1.5, eye_y, 0.5);
	sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
	tick(s);
	for(int i = 0; i < 70; i++) /* well clear of earlier toasts */
		tick(s);
	unsigned before_b = hud_n;
	toast_unknown(w0, p);
	unsigned entry3 = toast_enter(s, p);
	while(g_test_tick < entry3 + RZ_TEST_DEADLINE - 60)
		tick(s);
	server_world_test_fail_gen_attempts(0);
	unsigned ok0 = p->rz.recovery_ok;
	while(p->rz.recovering && g_test_tick < entry3 + RZ_TEST_DEADLINE + 50)
		tick(s);
	unsigned fails3 = 0;
	(void)toast_fail_tick(before_b, &fails3);
	printf("     [%s leg B: entry %u, resident at E+%u, toasts %u]\n", o->name,
		entry3, g_test_tick - entry3, hud_n - before_b);
	snprintf(label, sizeof label, "%s toasts B: success just before the "
		"deadline -> recovery_ok, NO failure toast, spacing holds", o->name);
	check(!p->rz.recovering && p->rz.recovery_ok == ok0 + 1 && fails3 == 0
		&& g_test_tick - entry3 < RZ_TEST_DEADLINE && toast_min_gap(0) >= 60,
		label);
	(void)ticks_until_resident(s, w0, -1, 0, 400);
}

/* --- leg D: saturated output at the deadline ---------------------------- */
static void toasts_saturated(struct server_local* s, const struct owner* o) {
	struct server_player* p = o->body;
	char label[220];
	const struct client_session* mine = &gstate.players[o->guest ? 1 : 0];
	const struct client_session* theirs = &gstate.players[o->guest ? 0 : 1];
	for(int i = 0; i < 70; i++)
		tick(s);
	toast_unknown(&s->worlds[0], p);
	unsigned entry = toast_enter(s, p);
	while(g_test_tick < entry + RZ_TEST_DEADLINE - 1)
		tick(s);
	memset(gstate.players[0].hud_message, 0, sizeof gstate.players[0].hud_message);
	memset(gstate.players[1].hud_message, 0, sizeof gstate.players[1].hud_message);
	unsigned before = hud_n;
	/* the deadline update runs with the client inbox FULL */
	struct client_rpc filler = {.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = 2}};
	unsigned queued = 0;
	while(queued < 1024 && clin_try_rpc_send(&filler))
		queued++;
	g_test_tick++;
	bool was_paused = s->paused;
	s->paused = false;
	server_local_test_update_once(s); /* archive owner; pending output is lossless */
	uint32_t pending;
	clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
	unsigned during = hud_n - before;
	while(!clin_rpc_flush_pending())
		clin_update_drain();
	clin_update_drain();
	s->paused = was_paused;
	for(int i = 0; i < 70; i++)
		tick(s);
	unsigned fails = 0;
	(void)toast_fail_tick(before, &fails);
	printf("     [%s leg D: %u fillers, pending=%u, toasts before "
		"drain %u, failures after drain %u]\n", o->name, queued,
		pending, during, fails);
	snprintf(label, sizeof label, "%s toasts D: with the client queue full the "
		"owed failure is RETAINED in pending output and delivered "
		"exactly once to its own pane after the drain", o->name);
	check(queued > 0 && pending > 0 && during == 0 && fails == 1 && p->rz.failed
		&& strstr(mine->hud_message, "Terrain") != NULL
		&& strstr(theirs->hud_message, "Terrain") == NULL, label);
	toast_recover(s, o);
}
static void case_toasts(void) {
	struct server_local* s = admitted_fixture();
	const double eye_y = s->player.y;
	struct owner ho = {.name = "host", .guest = false, .body = &s->player};
	struct owner go = {.name = "guest", .guest = true, .body = s->guest_actor};
	reset_body(s->guest_actor, 4.5, eye_y, 6.5);
	reset_body(&s->player, 1.5, eye_y, 0.5);
	tick(s);
	toasts_one(s, &ho);
	toasts_saturated(s, &ho);
	reset_body(&s->player, 4.5, eye_y, 6.5);
	sample(s, &ho, 4.5, eye_y, 6.5, s->player.correction_seq);
	tick(s);
	toasts_one(s, &go);
	toasts_saturated(s, &go);
	teardown(s);
}

/* ======================================================================== */
/* Round 3 item 3: lifecycle. server_local_init must establish the residency
 * record, the loader cursor and the PUBLISHED counters on POISONED storage
 * (main.c's server is an uninitialized automatic), including after an
 * earlier session in the same process published nonzero counters; a body
 * crossing a real lifecycle edge (guest leave + rejoin) starts clean. */
static struct server_local* poisoned_fixture(void) {
	test_fixture_dir("test_split_residency", split_path, sizeof(split_path));
	RES_MKDIR(split_path);
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	memset(&s->player.rz, 0xA5, sizeof s->player.rz);
	s->rz_rr = 0xA5;
	server_local_test_create_without_thread(s, false, false, false, false,
		false, true, false);
	string_t path;
	string_init_set_str(path, split_path);
	assert(level_archive_create_new(&s->level, path, 12345, (vec3) {0, 4, 0}));
	assert(server_world_create_with_populate_scratch(&s->worlds[0], path,
		WORLD_DIM_OVERWORLD, 12345, true, s->world_populate_scratch));
	string_clear(path);
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	s->player.has_pos = true;
	s->player.finished_loading = false;
	s->player.x = s->player.z = 0.5;
	s->player.y = 4.0 + PLAYER_EYE_HEIGHT;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.oxygen = MAX_OXYGEN;
	s->autosave_ticks = 0;
	s->far_view_distance = MAX_VIEW_DISTANCE + 1;
	return s;
}
static bool rz_all_zero(const struct rz_actor* r) {
	const unsigned char* b = (const unsigned char*)r;
	for(size_t i = 0; i < sizeof *r; i++)
		if(b[i])
			return false;
	return true;
}
static bool tel_all_zero(void) {
	uint32_t t[SERVER_LOCAL_RZ_TEL_COUNT];
	server_local_rz_telemetry(t);
	for(unsigned i = 0; i < SERVER_LOCAL_RZ_TEL_COUNT; i++)
		if(t[i])
			return false;
	return true;
}
static void case_lifecycle(void) {
	/* session A on poisoned storage publishes nonzero counters */
	struct server_local* a = poisoned_fixture();
	check(rz_all_zero(&a->player.rz) && a->rz_rr == 0,
		"lifecycle: server_local_init zeroes a POISONED host residency "
		"record and loader cursor");
	a->player.rz.rejects = 7;
	server_local_test_update_once(a);
	uint32_t t[SERVER_LOCAL_RZ_TEL_COUNT];
	server_local_rz_telemetry(t);
	check(t[RZ_TEL_REJECTS] == 7, "lifecycle: session A published its "
		"counters (rejects 7)");
	split_destroy(a);
	/* session B, same process, poisoned again */
	struct server_local* b = poisoned_fixture();
	check(rz_all_zero(&b->player.rz) && b->rz_rr == 0 && tel_all_zero(),
		"lifecycle: a REUSED process starts session B with an empty record "
		"AND empty published counters (no leak from session A)");
	split_destroy(b);
}
static void case_rejoin(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	const double eye_y = s->player.y;
	struct owner go = {.name = "guest", .guest = true, .body = g};
	reset_body(g, 1.5, eye_y, 0.5);
	sample(s, &go, 1.5, eye_y, 0.5, g->correction_seq);
	tick(s);
	check(g->rz.pin.valid, "rejoin: the guest has a live pin");
	/* poison the transient state, then a REAL leave + rejoin */
	g->rz.failed = g->rz.recovering = true;
	g->rz.req[0] = (struct rz_request) {.cx = 9, .cz = 9, .used = true,
		.prio = RZ_PRIO_NEED};
	g->rz.streak = 9;
	guest_release(s);
	s->paused = false;
	guest_attach(s);
	struct client_session* gc = &gstate.players[1];
	assert(client_session_request_local_gameplay(gc));
	pump_handshake(s);
	struct server_player* g2 = s->guest_actor;
	check(g2 && !g2->rz.pin.valid && !g2->rz.recovering && !g2->rz.failed
		&& !g2->rz.req[0].used && g2->rz.streak == 0,
		"rejoin: a rejoined guest body starts with no pin, request, "
		"recovery or streak (production join path)");
	/* the production loading barrier retires a pin too (host) */
	s->player.rz.pin.valid = true;
	s->player.rz.req[1] = (struct rz_request) {.cx = 7, .used = true};
	s->player.finished_loading = false;
	server_local_test_update_once(s);
	check(!s->player.rz.pin.valid && !s->player.rz.req[1].used,
		"rejoin: a host behind its loading barrier has its pin and requests "
		"retired by server_local_update (production witness)");
	s->player.finished_loading = true;
	teardown(s);
}

/* ======================================================================== */
/* Round 3 item 2: recovery is BOUNDED. A permanent failure terminates into a
 * visible retryable failure at the deadline, never re-arms by itself, keeps
 * the body held (no damage-taking hazard, no crosshair action), and only the
 * OWNER's fresh sample starts one new attempt -- which, once the terrain is
 * loadable, recovers without replaying any refused movement. Host in world 0
 * (the real near loader), guest in world 1 (its column loaded explicitly:
 * the fixture runs no A2-W stream for the hand-made slot). */
static void bounded_one(struct server_local* s, const struct owner* o,
	struct server_player* other, struct server_world* w, bool real_loader) {
	struct server_player* p = o->body;
	const double eye_y = s->player.y;
	char label[220];
	/* the body is pushed onto a never-loaded column with no eligible pin */
	server_world_test_fail_gen_attempts(100000);
	if(server_world_chunk_resident(w, 3, 0))
		evict(w, 3, 0);
	p->rz.pin.valid = false;
	p->x = 56.5;
	p->z = 0.5;
	short health = p->health;
	unsigned rec0 = p->rz.recoveries, fail0 = p->rz.recovery_failures;
	unsigned retry0 = p->rz.retries;
	hud_reset();
	tick(s);
	check(p->rz.recovering && !p->rz.failed, "bounded: recovery entered");
	for(unsigned i = 0; i < RZ_TEST_DEADLINE - 2; i++)
		tick(s);
	snprintf(label, sizeof label, "%s/w%u bounded: still recovering just "
		"before the deadline", o->name, (unsigned)w->world_index);
	check(p->rz.recovering && !p->rz.failed, label);
	tick(s);
	tick(s);
	tick(s); /* the owed failure toast goes out on the next feedback pass */
	snprintf(label, sizeof label, "%s/w%u bounded: the deadline TERMINATES "
		"into ONE visible failure (counted, toast, requests dropped)",
		o->name, (unsigned)w->world_index);
	bool any_req = false;
	for(unsigned k = 0; k < RZ_REQ_MAX; k++)
		any_req |= p->rz.req[k].used;
	check(p->rz.failed && p->rz.recovery_failures == fail0 + 1
		&& hud_count(RZ_TEST_FAIL_TEXT) == 1 && !any_req,
		label);
	/* no automatic re-arm, however long it waits */
	for(int i = 0; i < 400; i++)
		tick(s);
	snprintf(label, sizeof label, "%s/w%u bounded: NO automatic re-arm in "
		"400 more ticks (same episode, no new failure, no new toast)",
		o->name, (unsigned)w->world_index);
	check(p->rz.failed && p->rz.recoveries == rec0 + 1
		&& p->rz.recovery_failures == fail0 + 1
		&& hud_count(RZ_TEST_FAIL_TEXT) == 1, label);
	p->hurt_resist_time = 0;
	bool accepted = true;
	(void)server_local_damage_actor_result(s, p, 4, 0.0F, 0.0F,
		&accepted);
	snprintf(label, sizeof label, "%s/w%u bounded: the failed body is held, "
		"not active (a real mob hit is absorbed, pose unchanged)", o->name,
		(unsigned)w->world_index);
	check(p->health == health && !accepted && p->x == 56.5, label);
	/* retry ownership: the OTHER actor's sample does not retry this body */
	struct owner oo = {.name = "other", .guest = !o->guest, .body = other};
	sample(s, &oo, other->x, eye_y, other->z, other->correction_seq);
	snprintf(label, sizeof label, "%s/w%u bounded: the other actor cannot "
		"retry this body", o->name, (unsigned)w->world_index);
	check(p->rz.failed && p->rz.retries == retry0, label);
	/* the owner's fresh sample starts ONE new attempt; it is not applied */
	server_world_test_fail_gen_attempts(0);
	sample(s, o, 57.5, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s/w%u bounded: the owner's fresh sample "
		"starts one new bounded attempt and is NOT applied", o->name,
		(unsigned)w->world_index);
	check(!p->rz.failed && p->rz.recovering && p->rz.retries == retry0 + 1
		&& p->x == 56.5, label);
	if(!real_loader) {
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(w, 3, 0, &sc) && sc);
	}
	for(int i = 0; i < 100 && p->rz.recovering; i++)
		tick(s);
	snprintf(label, sizeof label, "%s/w%u bounded: the retry recovers "
		"(resident, recovery_ok), no late replay of the refused samples",
		o->name, (unsigned)w->world_index);
	check(!p->rz.recovering && p->x == 56.5 && p->z == 0.5, label);
	sample(s, o, 57.5, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s/w%u bounded: a FRESH sample moves the "
		"recovered body", o->name, (unsigned)w->world_index);
	check(p->x == 57.5, label);
}
static void late_settlement(struct server_local* s, bool guest, unsigned age) {
	struct server_player* p = guest ? s->guest_actor : &s->player;
	struct owner o = {.name = guest ? "guest" : "host", .guest = guest, .body = p};
	struct server_world* w = &s->worlds[0];
	struct rz_session_slot* ss = &s->rz_sess[guest ? 1 : 0];
	memset(&p->rz, 0, sizeof p->rz);
	server_world_test_fail_gen_attempts(100000);
	if(server_world_chunk_resident(w, 3, 0)) evict(w, 3, 0);
	p->rz.pin.valid = false;
	p->x = 56.5; p->z = 0.5;
	tick(s);
	const uint32_t attempt = ss->attempt;
	const unsigned ok = p->rz.recovery_ok;
	while(s->near_send_seq - p->rz.recover_start < age - 1) tick(s);
	server_world_test_fail_gen_attempts(0);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(w, 3, 0, &sc) && sc);
	tick(s);
	if(age <= RZ_TEST_DEADLINE) {
		check(ss->last_attempt_result == 1 && ss->last_attempt_ticks <= 200,
			"boundary: resident at/before deadline succeeds within 200 ticks");
	} else {
		check(ss->last_attempt_result == 2 && ss->last_attempt_ticks == 200
			&& p->rz.recovery_ok == ok && ss->attempt == attempt,
			"late settlement: FAILED terminal is immutable, no passive success");
		uint32_t failures0 = ss->sum[3], retries0 = ss->sum[4];
		for(unsigned i = 0; i < 3; i++) {
			sample(s, &o, p->x, p->y, p->z, p->correction_seq);
			tick(s);
		}
		check(ss->last_attempt_result == 2 && ss->last_attempt_ticks == 200
			&& ss->attempt == attempt && ss->sum[4] == retries0
			&& ss->sum[3] == failures0 && p->rz.recovery_ok == ok,
			"late settlement: idle and correction echoes cannot retry or rewrite failure");
		struct server_local_rz_wire before_wire;
		server_local_rz_actor_telemetry(&before_wire);
		const unsigned actor = guest ? 1 : 0;
		check(before_wire.rze[actor][3] == attempt
			&& before_wire.rze[actor][4] == 200 && before_wire.rze[actor][5] == 0,
			"terminal receipt: failed identity/duration survive passive settlement");
		double x = p->x;
		sample(s, &o, x + 0.25, p->y, p->z, p->correction_seq);
		check(ss->attempt == attempt + 1 && ss->sum[4] == retries0 + 1
			&& p->x == x, "late settlement: deliberate owner retry is fresh and refused");
		tick(s);
		check(ss->last_attempt_result == 1 && ss->attempt == attempt + 1
			&& ss->sum[3] == failures0 && p->rz.recovery_ok == ok + 1,
			"late settlement: only fresh attempt earns success");
		tick(s); /* publication precedes settlement in the real update */
		struct server_local_rz_wire after_wire;
		server_local_rz_actor_telemetry(&after_wire);
		printf("R7 receipt actor=%u epoch=%u before=%u/%u/%u/%u/%u after=%u/%u/%u/%u/%u/%u/%u/%u\n",
			actor, ss->epoch, before_wire.rze[actor][0], before_wire.rze[actor][1],
			before_wire.rze[actor][2], before_wire.rze[actor][3], before_wire.rze[actor][4],
			after_wire.rze[actor][0], after_wire.rze[actor][1], after_wire.rze[actor][2],
			after_wire.rze[actor][3], after_wire.rze[actor][4], after_wire.rze[actor][5],
			after_wire.rze[actor][6], after_wire.rze[actor][7]);
		check(!memcmp(before_wire.rze[actor], after_wire.rze[actor], 5 * sizeof(uint32_t))
			&& after_wire.rze[actor][5] == attempt + 1
			&& after_wire.rze[actor][6] == attempt + 1
			&& after_wire.rze[actor][7] <= 200,
			"terminal receipt: immutable failure binds fresh retry and completion");
	}
	reset_body(p, 4.5, p->y, 4.5);
	memset(&p->rz, 0, sizeof p->rz);
	tick(s);
}
static void case_bounded(void) {
	struct server_local* s = admitted_fixture();
	for(unsigned a = 0; a < 2; a++)
		for(unsigned age = 199; age <= 201; age++)
			late_settlement(s, a != 0, age);
	second_world_create(s);
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	reset_body(g, 4.5, eye_y, 4.5);
	reset_body(h, 1.5, eye_y, 0.5);
	tick(s);
	bounded_one(s, &ho, g, &s->worlds[0], true);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	/* guest in world 1 */
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s->worlds[1], 0, 0, &sc) && sc);
	g->world_slot = 1;
	reset_body(g, 4.5, eye_y, 4.5);
	tick(s);
	bounded_one(s, &go, h, &s->worlds[1], false);
	g->world_slot = 0;
	reset_body(g, 4.5, eye_y, 4.5);
	tick(s);
	evict(&s->worlds[1], 3, 0);
	evict(&s->worlds[1], 0, 0);
	second_world_destroy(s);
	teardown(s);
}

/* ======================================================================== */
/* Round 3 item 1: stream_lean. A NEED request whose column still needs
 * generation is skipped in a lean pass (not a scan break): the lean pass
 * still sends the send-ready ring work, and the next non-lean pass (the pacer
 * alternates, at most 1 of 2 backlogged iterations is lean) generates it. */
static void case_lean(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	struct server_player* h = &s->player;
	const double eye_y = s->player.y;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	reset_body(s->guest_actor, 4.5, eye_y, 4.5);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	evict(w0, -1, 0);
	/* send-ready ring work: resident, populated, not yet sent */
	unsigned ready = 0;
	for(int cz = -2; cz <= 2; cz++)
		for(int cx = 1; cx <= 2; cx++) {
			struct server_chunk* c = dict_server_chunks_get(w0->chunks,
				S_CHUNK_ID(cx, cz));
			if(c && c->populated && c->sent) {
				c->sent = false;
				ready++;
			}
		}
	sample(s, &ho, 0.1, eye_y, 0.5, h->correction_seq); /* NEED (-1,0) */
	unsigned skips0 = server_local_test_rz_lean_skips();
	int32_t px[32], pz[32];
	bool prz[32];
	s->stream_lean = true;
	server_local_test_near_picks_reset();
	tick(s);
	unsigned n = server_local_test_near_picks(px, pz, prz, 32);
	unsigned later = 0;
	for(unsigned i = 1; i < n && i < 32; i++)
		later += !prz[i];
	printf("LEAN: ready=%u picks=%u first=(%d,%d)%s skips=%u later_non_rz=%u\n",
		ready, n, n ? px[0] : 0, n ? pz[0] : 0, n && prz[0] ? " rz" : "",
		server_local_test_rz_lean_skips() - skips0, later);
	check(n >= 2 && prz[0] && server_local_test_rz_lean_skips() > skips0
		&& later >= 1,
		"lean: the not-yet-generated NEED pick is skipped and the lean pass "
		"goes on to send-ready ring work (no scan break)");
	check(!server_world_chunk_resident(w0, -1, 0),
		"lean: the lean pass starts no generation for it");
	s->stream_lean = false;
	unsigned loaded0 = h->rz.loaded;
	tick(s);
	tick(s);
	check(server_world_chunk_resident(w0, -1, 0) && h->rz.loaded > loaded0,
		"lean: the next non-lean pass generates it -- bounded NEED progress "
		"(<= 1 extra iteration) -- and it counts as loaded");
	sample(s, &ho, 0.1, eye_y, 0.5, h->correction_seq);
	check(h->x == 0.1, "lean: the retried move is admitted");
	teardown(s);
}

/* ======================================================================== */
/* S4 item 6: the guest portal tick (server_local_guest_portal_tick) is gated
 * like the host's. It only runs with world-bound client payloads, so this
 * case enables them for its own ticks; the control proves the tick really
 * runs there (dwell advances on resident ground). */
static void case_portal(void) {
	check(clin_world_payloads_enabled(),
		"portal: world-bound client payloads enabled for this session");
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	struct server_player* g = s->guest_actor;
	const double eye_y = s->player.y;
	reset_body(&s->player, 4.5, eye_y, 4.5);
	reset_body(g, 1.5, eye_y, 0.5);
	tick(s);
	/* control: resident guest with portal contact -> the dwell ramps */
	g->time_until_portal = 0;
	g->time_in_portal = 0.0F;
	g->in_portal = true;
	tick(s);
	float ramped = g->time_in_portal;
	check(ramped > 0.0F, "portal control: a resident guest's portal dwell "
		"advances (the guest portal tick runs here)");
	/* recovering guest with stale contact -> no dwell, no transfer */
	server_world_test_fail_gen_attempts(100000);
	evict(w0, 2, 0);
	g->rz.pin.valid = false;
	g->x = 40.5;
	g->in_portal = true;
	float dwell = g->time_in_portal;
	for(int i = 0; i < 100; i++) {
		g->in_portal = true;
		tick(s);
	}
	check(g->rz.recovering && g->time_in_portal == dwell
		&& !g->portal_travel_requested,
		"portal: a guest in residency recovery advances NO dwell and opens no "
		"transfer from stale contact (gated like the host)");
	g->in_portal = false;
	server_world_test_fail_gen_attempts(0);
	(void)ticks_until_resident(s, w0, 2, 0, 600);
	reset_body(g, 1.5, eye_y, 0.5);
	tick(s);
	teardown(s);
}

/* ======================================================================== */
/* S5 (item 7): correction delivery under queue pressure + feedback rules. */
static void queue_samples(struct server_local* s, const struct owner* o,
	double x, double y, double z, uint32_t seq, unsigned n) {
	for(unsigned i = 0; i < n; i++) {
		struct server_rpc call = pose_sample(x + 0.01 * (double)i, y, z, seq);
		if(o->guest)
			assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
				.generation = s->guest_generation}, &call));
		else
			assert(svin_rpc_send(&call));
	}
	drain_server(s);
}
static void case_feedback(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	const double eye_y = s->player.y;
	struct owner owners[2] = {
		{.name = "host", .guest = false, .body = &s->player},
		{.name = "guest", .guest = true, .body = s->guest_actor},
	};
	char label[200];
	for(int i = 0; i < 2; i++) {
		const struct owner* o = &owners[i];
		struct server_player* p = o->body;
		struct server_player* other = i == 0 ? s->guest_actor : &s->player;
		reset_body(other, 4.5, eye_y, 4.5);
		reset_body(p, 1.5, eye_y, 0.5);
		sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
		tick(s);
		server_world_test_fail_gen_attempts(100000);
		evict(w0, -1, 0);

		/* --- 8 refused samples already queued on ONE token ----------- */
		counters_reset();
		uint32_t seq0 = p->correction_seq;
		unsigned rej0 = p->rz.rejects;
		queue_samples(s, o, 0.1, eye_y, 0.5, seq0, 8);
		snprintf(label, sizeof label, "%s pressure: 8 queued samples -> ONE "
			"refusal, ONE delivered correction carrying the live token",
			o->name);
		check(p->rz.rejects == rej0 + 1 && corrections(o) == 1
			&& correction_seq_seen(o) == p->correction_seq
			&& p->correction_seq != seq0 && p->x == 1.5, label);
		/* the client answers the correction with a fresh-token retry */
		counters_reset();
		queue_samples(s, o, 0.1, eye_y, 0.5, p->correction_seq, 4);
		snprintf(label, sizeof label, "%s pressure: a fresh-token burst is "
			"again ONE refusal + ONE correction (stale ones retired)",
			o->name);
		check(p->rz.rejects == rej0 + 2 && corrections(o) == 1, label);

		/* --- transient: admitted retry inside 1 s -> no toast -------- */
		server_world_test_fail_gen_attempts(0);
		(void)ticks_until_resident(s, w0, -1, 0, 200);
		hud_reset();
		sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
		tick(s);
		snprintf(label, sizeof label, "%s transient: a refusal admitted on "
			"retry within 1 s shows NO toast", o->name);
		check(p->x == 0.1 && hud_n == 0 && p->rz.streak == 0, label);

		/* --- persistent: refused every tick for 70 ticks -> 2 toasts -- */
		reset_body(p, 1.5, eye_y, 0.5);
		sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
		tick(s);
		server_world_test_fail_gen_attempts(100000);
		evict(w0, -1, 0);
		hud_reset();
		unsigned n0 = p->rz.notices;
		for(int t = 0; t < 70; t++) {
			sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
			tick(s);
		}
		snprintf(label, sizeof label, "%s persistent: \"Loading terrain...\" "
			"after 1 s, rate-limited to one per 3 s (70 ticks -> 1-2)",
			o->name);
		check(hud_count("Loading terrain...") >= 1
			&& hud_count("Loading terrain...") <= 2
			&& p->rz.notices - n0 == hud_n, label);
		printf("     [%s: %u toasts in 70 refused ticks, streak %u]\n",
			o->name, hud_n, p->rz.streak);
		server_world_test_fail_gen_attempts(0);
		(void)ticks_until_resident(s, w0, -1, 0, 200);
		sample(s, o, 0.1, eye_y, 0.5, p->correction_seq);
		snprintf(label, sizeof label, "%s persistent: admitted once resident; "
			"latency recorded", o->name);
		check(p->x == 0.1 && p->rz.latency_last >= 70, label);
		reset_body(p, 1.5, eye_y, 0.5);
		sample(s, o, 1.5, eye_y, 0.5, p->correction_seq);
		tick(s);
	}
	teardown(s);
}

/* ======================================================================== */
/* S3: the other producers (ASTRA-RESIDENCY-REVIEW items 3 and 5).          */
static void case_sources(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double eye_y = s->player.y;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	reset_body(g, 40.5, eye_y, 40.5);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);

	/* --- an arbitrary far PLAYER_POS is not teleport authority --------- */
	counters_reset();
	unsigned req0 = h->rz.requests, far0 = h->rz.far_refusals;
	sample(s, &ho, 2000.5, eye_y, 0.5, h->correction_seq);
	for(int i = 0; i < 10; i++)
		tick(s);
	check(h->x == 1.5 && corrections(&ho) == 1,
		"far: a 2000-block PLAYER_POS is refused and corrected");
	check(h->rz.far_refusals == far0 + 1 && h->rz.requests == req0,
		"far: counted as a far refusal, NO destination request");
	check(!server_world_chunk_resident(w0, 125, 0),
		"far: the caller-selected far column is never loaded");

	/* --- swept footprint: a diagonal seam with a missing corner -------- */
	reset_body(h, 15.5, eye_y, 15.5);
	sample(s, &ho, 15.5, eye_y, 15.5, h->correction_seq);
	tick(s);
	server_world_test_fail_gen_attempts(100000);
	evict(w0, 1, 0);
	counters_reset();
	sample(s, &ho, 16.5, eye_y, 16.5, h->correction_seq);
	check(server_world_chunk_resident(w0, 0, 0)
		&& server_world_chunk_resident(w0, 1, 1) && h->x == 15.5
		&& corrections(&ho) == 1,
		"swept: (0,0)->(1,1) with the corner (1,0) missing is refused although "
		"both endpoints are resident");
	server_world_test_fail_gen_attempts(0);
	(void)ticks_until_resident(s, w0, 1, 0, 200);
	sample(s, &ho, 16.5, eye_y, 16.5, h->correction_seq);
	check(h->x == 16.5, "swept: admitted once the corner column is resident");

	/* --- knockback/pushes move no server pose -------------------------- */
	double kx = h->x;
	server_local_push_actor_velocity(s, h, 0.4F, 0.0F);
	check(h->x == kx && h->vel_x != 0.0F, "knockback: a server push is "
		"velocity only -- the resulting pose arrives as an admitted sample");
	h->vel_x = h->vel_z = 0.0F;

	/* --- eviction protection ------------------------------------------ */
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	/* The streaming evictors (slot-0 near evictor, A2-W evictor) consult
	 * server_local_rz_evict_veto before they pick a column. */
	unsigned v0 = h->rz.evict_vetoes;
	check(server_local_rz_evict_veto(s, w0, 0, 0)
		&& h->rz.evict_vetoes == v0 + 1,
		"evict: the host footprint column (0,0) is vetoed for the evictors");
	unsigned gv0 = g->rz.evict_vetoes;
	check(server_local_rz_evict_veto(s, w0, 2, 2)
		&& g->rz.evict_vetoes == gv0 + 1,
		"evict: the guest footprint column (2,2) is protected too");
	check(!server_local_rz_evict_veto(s, w0, 3, 0)
		&& !server_local_rz_evict_veto(s, &s->worlds[1], 0, 0),
		"evict: other columns, and the same column of another world "
		"instance, are not protected");
	sample(s, &ho, 17.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	check(h->x == 17.5 && h->rz.pin.x == 17.5,
		"evict: the host moves on and its pin follows");
	check(!server_local_rz_evict_veto(s, w0, 0, 0),
		"evict: the column it left is evictable again");
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);

	/* --- the transfer landing barrier ---------------------------------- */
	server_world_test_fail_gen_attempts(100000);
	evict(w0, 3, 3);
	check(!server_local_rz_landing_ready(s, w0, 56.5, 56.5),
		"landing: a non-resident arrival footprint is not ready (no commit)");
	server_world_test_fail_gen_attempts(0);
	bool ready = false;
	for(int i = 0; i < 20 && !ready; i++)
		ready = server_local_rz_landing_ready(s, w0, 56.5, 56.5);
	check(ready && server_world_chunk_resident(w0, 3, 3),
		"landing: the barrier loads the arrival column itself (bounded "
		"steps) and then admits the commit");
	teardown(s);
}

/* ======================================================================== */
/* Round 4 item 1: the recovery barrier holds gameplay WITHOUT breaking the
 * protocol acknowledgements. A host riding a REAL cart enters bounded
 * recovery (RECOVERING, then the terminal FAILED hold); cumulative
 * SRPC_MINECART_CONTROL tuples through the real mailbox/dispatcher advance
 * the cart's acknowledgement counters but add NO pending_throttle; ride
 * intents return UNCHANGED, correlated authority (the client retires its
 * request) without touching occupancy or the ride epoch. After release the
 * held steps never replay: a fresh tuple adds exactly its own step. The
 * guest has no cart verbs (svin slot-1 class): they stay unsupported. */
static uint32_t hc_ride_seq;
static void host_rpc(struct server_local* s, struct server_rpc* call) {
	bool was = s->paused;
	s->paused = false; /* control is gameplay-gated on pause */
	assert(svin_rpc_send(call));
	drain_server(s);
	s->paused = was;
}
static struct server_rpc hc_intent(const struct server_local* s, uint32_t id,
	bool desired) {
	return (struct server_rpc) {.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {.entity_id = id,
			.input_epoch = s->input_epoch_counter,
			.request_seq = ++hc_ride_seq, .desired_occupied = desired}};
}
static struct server_rpc hc_control(const struct entity* cart, uint32_t seq,
	uint32_t pos, uint32_t neg) {
	return (struct server_rpc) {.type = SRPC_MINECART_CONTROL,
		.payload.minecart_control = {.entity_id = cart->id,
			.ride_epoch = cart->data.minecart.ride_epoch, .input_seq = seq,
			.positive_steps = pos, .negative_steps = neg}};
}
static bool hc_host_rides(const struct entity* cart) {
	return cart->data.minecart.occupied
		&& cart->data.minecart.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID;
}
/* One held phase: controls + stale tuple + ride intents. */
static void hc_held_phase(struct server_local* s, struct entity* cart,
	const char* phase, uint32_t seq, uint32_t pos) {
	char label[240];
	struct server_player* h = &s->player;
	const uint32_t epoch = cart->data.minecart.ride_epoch;
	struct server_rpc c = hc_control(cart, seq, pos, 0);
	host_rpc(s, &c);
	snprintf(label, sizeof label, "held controls %s: a valid same-epoch tuple "
		"is ACKNOWLEDGED (counters -> seq %u) but adds NO throttle", phase,
		seq);
	check(cart->data.minecart.control_seq == seq
		&& cart->data.minecart.control_positive_steps == pos
		&& cart->data.minecart.pending_throttle == 0, label);
	struct server_rpc stale = hc_control(cart, seq - 1, pos - 1, 0);
	host_rpc(s, &stale);
	snprintf(label, sizeof label, "held controls %s: a stale tuple neither "
		"rewinds the counters nor adds throttle", phase);
	check(cart->data.minecart.control_seq == seq
		&& cart->data.minecart.pending_throttle == 0, label);
	unsigned occ0 = occ_msgs;
	struct server_rpc off = hc_intent(s, cart->id, false);
	host_rpc(s, &off);
	snprintf(label, sizeof label, "held controls %s: a DISMOUNT intent returns "
		"unchanged correlated authority (occupied, same ride epoch, epoch/seq "
		"echoed)", phase);
	check(hc_host_rides(cart) && h->riding_minecart
		&& cart->data.minecart.ride_epoch == epoch && occ_msgs == occ0 + 1
		&& occ_last_epoch == off.payload.minecart_ride_intent.input_epoch
		&& occ_last_seq == off.payload.minecart_ride_intent.request_seq
		&& occ_last_occupied, label);
	occ0 = occ_msgs;
	struct server_rpc on = hc_intent(s, cart->id, true);
	host_rpc(s, &on);
	snprintf(label, sizeof label, "held controls %s: a MOUNT intent is answered "
		"the same way (no epoch rotation)", phase);
	check(hc_host_rides(cart) && cart->data.minecart.ride_epoch == epoch
		&& occ_msgs == occ0 + 1
		&& occ_last_seq == on.payload.minecart_ride_intent.request_seq, label);
}
static void case_held_controls(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double eye_y = s->player.y;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	reset_body(g, 4.5, eye_y, 4.5);
	reset_body(h, 14.5, eye_y, 0.5);
	sample(s, &ho, 14.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	struct entity* cart = server_local_spawn_minecart(
		(vec3) {15.5F, 4.0F, 0.5F}, s);
	assert(cart && cart->data.minecart.minecart_type == 0);
	clin_update_drain();
	const uint32_t cart_id = cart->id;
	struct server_rpc mount = hc_intent(s, cart_id, true);
	host_rpc(s, &mount);
	check(hc_host_rides(cart) && h->riding_minecart
		&& cart->data.minecart.ride_epoch != 0,
		"held controls setup: the host rides a real cart (RIDE_INTENT)");
	/* resident control: the dispatcher applies a step */
	cart->data.minecart.pending_throttle = 0;
	struct server_rpc c1 = hc_control(cart, 1, 1, 0);
	host_rpc(s, &c1);
	check(cart->data.minecart.pending_throttle == 1,
		"held controls control: resident, one step = +1 throttle");
	cart->data.minecart.pending_throttle = 0;
	/* the guest has no cart verbs: unsupported stays unsupported */
	struct server_rpc gc = hc_control(cart, 2, 2, 0);
	bool queued = svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
		.generation = s->guest_generation}, &gc);
	drain_server(s);
	check(!queued && cart->data.minecart.control_seq == 1,
		"held controls guest: a guest MINECART_CONTROL is not admitted "
		"(unsupported route)");

	/* RECOVERING: the host's footprint lands on a missing column (no pin) */
	server_world_test_fail_gen_attempts(100000);
	evict(w0, 1, 0);
	h->rz.pin.valid = false;
	h->x = 16.2;
	struct server_rpc c2 = hc_control(cart, 2, 2, 0);
	host_rpc(s, &c2); /* accepted pre-hold, not yet consumed by the cart */
	check(cart->data.minecart.pending_throttle == 1,
		"held controls: a pre-hold step is pending");
	tick(s);
	check(h->rz.recovering && !h->rz.failed,
		"held controls: the riding host is in bounded recovery");
	check(cart->data.minecart.pending_throttle == 0,
		"held controls: entering the hold discards the stale pre-hold "
		"impulse (it cannot fire after a 10 s hold)");
	hc_held_phase(s, cart, "RECOVERING", 3, 3);
	/* FAILED: past the deadline */
	for(unsigned i = 0; i < RZ_TEST_DEADLINE + 5 && !h->rz.failed; i++)
		tick(s);
	check(h->rz.failed && h->rz.recovering,
		"held controls: the attempt terminated (FAILED hold)");
	hc_held_phase(s, cart, "FAILED", 5, 5);

	/* release: the column becomes resident, recovery ends */
	server_world_test_fail_gen_attempts(0);
	(void)tick_until_resident(s, w0, 1, 0, 600);
	for(int i = 0; i < 5 && h->rz.recovering; i++)
		tick(s);
	check(!h->rz.recovering, "held controls: released once resident");
	for(int i = 0; i < 5; i++)
		tick(s);
	check(cart->data.minecart.pending_throttle == 0
		&& cart->data.minecart.control_seq == 5,
		"held controls release: ZERO replay -- no held step survives");
	struct server_rpc c6 = hc_control(cart, 6, 6, 0);
	host_rpc(s, &c6);
	check(cart->data.minecart.pending_throttle == 1,
		"held controls release: a fresh tuple adds exactly its own step");
	struct server_rpc dis = hc_intent(s, cart_id, false);
	host_rpc(s, &dis);
	check(!cart->data.minecart.occupied && !h->riding_minecart,
		"held controls release: a fresh dismount intent is applied");
	teardown(s);
}

/* ======================================================================== */
/* Round 4 item 3: retry ownership at the client/server boundary. The REAL
 * client producer (main.c: completed owner-tick PLAYER_POS of both panes,
 * fed by the corrections the client itself consumed) runs every tick while a
 * body sits in the terminal FAILED hold. Idle production and correction
 * echoes must not restart the attempt; only the owner's deliberate retry --
 * a fresh sample that actually MOVES the body (the stick), on the live token,
 * through its own route -- starts ONE bounded attempt, and that sample is
 * discarded. Wrong owner, stale token and duplicates cannot. */
static void client_produce(struct server_local* s) {
	clin_update();
	const unsigned slots[] = {0};
	main_client_host_tick_dispatch(slots, 1, true, false, -1);
	if(client_session_local_gameplay_active(&gstate.players[1])) {
		const struct input_player_frame neutral = {.connected = true};
		main_client_guest_tick_dispatch(&neutral);
	}
	drain_server(s);
}
static void client_camera_at(int pane, double x, double y, double z) {
	struct entity* body = gstate.players[pane].local_player;
	assert(body && body->world);
	body->pos[0] = (float)x;
	body->pos[1] = (float)y;
	body->pos[2] = (float)z;
	glm_vec3_copy(body->pos, body->pos_old);
	glm_vec3_zero(body->vel);
	body->on_ground = true;
	/* The client has already streamed this column; only the server's
	 * residency is missing in this boundary fixture. Keep idle ticks
	 * grounded so a gravity delta cannot impersonate deliberate input. */
	for(int dx = -1; dx <= 1; dx++)
		for(int dz = -1; dz <= 1; dz++)
			world_set_block(body->world, (w_coord_t)floor(x) + dx,
				(w_coord_t)floor(y - PLAYER_EYE_HEIGHT) - 1,
				(w_coord_t)floor(z) + dz,
				(struct block_data){.type = BLOCK_STONE}, false);
	gstate.players[pane].camera.x = (float)x;
	gstate.players[pane].camera.y = (float)y;
	gstate.players[pane].camera.z = (float)z;
}
static void client_retry_one(struct server_local* s, const struct owner* o,
	struct server_player* other) {
	struct server_player* p = o->body;
	const int pane = o->guest ? 1 : 0;
	char label[240];
	const double eye_y = s->player.y;
	/* the body is placed on a never-loadable column, no eligible pin */
	server_world_test_fail_gen_attempts(100000);
	if(server_world_chunk_resident(&s->worlds[0], 3, 0))
		evict(&s->worlds[0], 3, 0);
	p->rz.pin.valid = false;
	p->x = 56.5;
	p->z = 0.5;
	client_camera_at(pane, 56.5, eye_y, 0.5);
	unsigned rec0 = p->rz.recoveries, fail0 = p->rz.recovery_failures;
	unsigned retry0 = p->rz.retries, rej0 = p->rz.rejects;
	hud_reset();
	unsigned entry = 0;
	for(unsigned i = 0; i < RZ_TEST_DEADLINE + 400 + 5; i++) {
		client_produce(s);
		tick(s);
		if(!entry && p->rz.recovering)
			entry = g_test_tick;
	}
	printf("     [%s client idle: rejects +%u, recoveries +%u, failures +%u, "
		"retries +%u, failed=%d]\n", o->name, p->rz.rejects - rej0,
		p->rz.recoveries - rec0, p->rz.recovery_failures - fail0,
		p->rz.retries - retry0, (int)p->rz.failed);
	snprintf(label, sizeof label, "%s client retry: the real client producer "
		"ran during the hold (its samples reached the server)", o->name);
	check(p->rz.rejects > rej0, label);
	snprintf(label, sizeof label, "%s client retry: deadline + 400 ticks of "
		"IDLE client production -> exactly ONE failure, NO re-arm", o->name);
	check(p->rz.failed && p->rz.recoveries == rec0 + 1
		&& p->rz.recovery_failures == fail0 + 1 && p->rz.retries == retry0,
		label);
	/* the failure toast names the available retry action */
	snprintf(label, sizeof label, "%s client retry: the pane names the retry "
		"action (\"%s\")", o->name, RZ_TEST_FAIL_TEXT);
	check(hud_count(RZ_TEST_FAIL_TEXT) == 1, label);
	/* wrong owner: the other pane moves -- no retry of this body */
	struct owner oo = {.name = "other", .guest = !o->guest, .body = other};
	sample(s, &oo, other->x + 0.3, eye_y, other->z, other->correction_seq);
	snprintf(label, sizeof label, "%s client retry: the OTHER pane's movement "
		"cannot retry this body", o->name);
	check(p->rz.failed && p->rz.retries == retry0, label);
	/* stale token: a moved sample on a superseded token is dropped */
	sample(s, o, 57.5, eye_y, 0.5, p->correction_seq - 1);
	snprintf(label, sizeof label, "%s client retry: a moved sample on a STALE "
		"token cannot retry", o->name);
	check(p->rz.failed && p->rz.retries == retry0, label);
	/* the deliberate retry: the client moves its own body (stick input) */
	server_world_test_fail_gen_attempts(0);
	client_camera_at(pane, 57.0, eye_y, 0.5);
	client_produce(s);
	snprintf(label, sizeof label, "%s client retry: the owner's deliberate "
		"move through the real producer starts ONE attempt; the sample is "
		"discarded (pose kept)", o->name);
	check(!p->rz.failed && p->rz.recovering && p->rz.retries == retry0 + 1
		&& p->x == 56.5, label);
	/* duplicates: more moved samples before residency start nothing new */
	client_camera_at(pane, 57.5, eye_y, 0.5);
	client_produce(s);
	sample(s, o, 57.6, eye_y, 0.5, p->correction_seq);
	snprintf(label, sizeof label, "%s client retry: a DUPLICATE retry starts "
		"no second attempt", o->name);
	check(p->rz.retries == retry0 + 1 && p->x == 56.5, label);
	/* recovery through the production loader, then a fresh move */
	for(int i = 0; i < 100 && p->rz.recovering; i++) {
		client_produce(s);
		tick(s);
	}
	snprintf(label, sizeof label, "%s client retry: the attempt recovers with "
		"NO movement replay (pose still the kept one)", o->name);
	check(!p->rz.recovering && p->x == 56.5 && p->z == 0.5, label);
	client_camera_at(pane, 57.5, eye_y, 0.5);
	client_produce(s);
	snprintf(label, sizeof label, "%s client retry: a FRESH client sample after "
		"residency moves the body", o->name);
	check(fabs(p->x - 57.5) < 1e-3, label);
}
static void case_client_retry(void) {
	struct server_local* s = admitted_fixture();
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	printf("     [client: world_loaded=%d host lp=%p guest lp=%p]\n",
		(int)gstate.world_loaded, (void*)gstate.players[0].local_player,
		(void*)gstate.players[1].local_player);
	reset_body(g, 4.5, eye_y, 4.5);
	client_camera_at(1, 4.5, eye_y, 4.5);
	reset_body(h, 1.5, eye_y, 0.5);
	client_camera_at(0, 1.5, eye_y, 0.5);
	tick(s);
	client_retry_one(s, &ho, g);
	reset_body(h, 1.5, eye_y, 6.5);
	client_camera_at(0, 1.5, eye_y, 6.5);
	sample(s, &ho, 1.5, eye_y, 6.5, h->correction_seq);
	tick(s);
	client_retry_one(s, &go, h);
	teardown(s);
}

/* ======================================================================== */
/* Round 4 item 5: the missing-terrain WAKE producer. A sleeper's step-out
 * (BlockBed.getNearestEmptyChunkCoordinates: head 3x3 then foot 3x3, x outer)
 * must follow Beta's order over REAL terrain. The bed head is at (0, fy, 8):
 * its scan starts in column x = -1 (chunk -1), where a standable cell
 * (-1, fy, 7) is Beta's first hit; (0, fy, 7) is the first hit in the
 * resident chunk. With chunk -1 missing, the wake must not choose (0, fy, 7)
 * out of order (nor any unknown cell): the wake's state change happens once,
 * the step-out waits in a bounded hold for the scan columns, then lands on
 * Beta's cell. A permanent failure ends at the deadline with a visible
 * notice and the best PROVEN-resident outcome. Normal (daytime) wake and
 * damage wake through the production handlers, host and guest; a resident
 * control and a destroyed bed (no step-out, Beta) too. */
static double wk_eye(double feet) { return feet + 0.1 + PLAYER_EYE_HEIGHT; }
static void wake_terrain(struct server_local* s, int fy) {
	struct server_world* w0 = &s->worlds[0];
	if(!server_world_chunk_resident(w0, -1, 0)) {
		server_world_test_fail_gen_attempts(0);
		(void)ticks_until_resident(s, w0, -1, 0, 600);
	}
	for(int x = -2; x <= 2; x++)
		for(int z = 6; z <= 10; z++) {
			put(w0, x, fy - 1, z, BLOCK_STONE);
			put(w0, x, fy, z, BLOCK_GLASS); /* not standable: occupied */
			put(w0, x, fy + 1, z, BLOCK_AIR);
			clin_update_drain(); /* block-change sends are lossless */
		}
	put(w0, -1, fy, 7, BLOCK_AIR); /* Beta's FIRST hit, in chunk -1 */
	put(w0, 0, fy, 7, BLOCK_AIR);  /* first hit inside chunk 0 */
	put(w0, 0, fy, 8, BLOCK_BED);  /* the bed head */
}
static void wake_one(struct server_local* s, const struct owner* o,
	struct server_player* other, bool damage, bool permanent) {
	struct server_player* p = o->body;
	struct server_world* w0 = &s->worlds[0];
	const double eye_y = s->player.y;
	const int fy = (int)floor(eye_y - PLAYER_EYE_HEIGHT);
	char label[240];
	const char* kind = damage ? "damage" : "daytime";
	reset_body(other, 4.5, eye_y, 2.5);
	reset_body(p, 0.5, eye_y, 4.5);
	wake_terrain(s, fy);
	sample(s, o, 0.5, eye_y, 4.5, p->correction_seq);
	tick(s);
	server_world_test_fail_gen_attempts(100000);
	evict(w0, -1, 0);
	s->world_time = 13000; /* night */
	server_local_begin_sleep_for(s, p, 0, fy, 8);
	check(p->sleeping, "wake setup: the body sleeps in its bed");
	const double bed_x = p->x, bed_z = p->z;
	bool spawn0 = p->has_bed_spawn;
	p->has_bed_spawn = false;
	hud_reset();
	if(damage) {
		p->hurt_resist_time = 0;
		bool accepted = false;
		(void)server_local_damage_actor_result(s, p, 1, 0.0F, 0.0F, &accepted);
	} else {
		s->world_time = 1000; /* daytime: the production sleep tick wakes */
		unsigned n = 0;
		while(p->sleeping && n++ < 40)
			tick(s);
	}
	printf("     [%s %s wake: sleeping=%d spawn=%d pose (%.2f, %.2f) t=%llu]\n",
		o->name, kind, (int)p->sleeping, (int)p->has_bed_spawn, p->x, p->z,
		(unsigned long long)s->world_time);
	snprintf(label, sizeof label, "%s %s wake: the sleep state ends ONCE "
		"(spawn %s)", o->name, kind, damage ? "untouched" : "set");
	check(!p->sleeping && p->has_bed_spawn == !damage, label);
	snprintf(label, sizeof label, "%s %s wake: with scan column -1 missing the "
		"body is NOT stepped out out of Beta order to (0, %d, 7), nor onto "
		"unknown terrain (it stays in its resident bed pose)", o->name, kind,
		fy);
	check(!(fabs(p->x - 0.5) < 1e-6 && fabs(p->z - 7.5) < 1e-6)
		&& server_world_columns_resident(w0, p->x - 0.3, p->z - 0.3,
			p->x + 0.3, p->z + 0.3), label);
	/* while held, a client move is refused (no motion ahead of the step-out) */
	double hx = p->x, hz = p->z;
	sample(s, o, hx, eye_y, hz + 0.4, p->correction_seq);
	tick(s);
	snprintf(label, sizeof label, "%s %s wake: held -- a move before the "
		"step-out is refused", o->name, kind);
	check(fabs(p->z - hz) < 1e-9, label);
	if(!permanent) {
		server_world_test_fail_gen_attempts(0);
		for(int i = 0; i < 200 && !(fabs(p->x + 0.5) < 1e-6); i++)
			tick(s);
		snprintf(label, sizeof label, "%s %s wake: once chunk -1 is resident "
			"the step-out lands on Beta's FIRST cell (-1, %d, 7)", o->name,
			kind, fy);
		check(fabs(p->x + 0.5) < 1e-6 && fabs(p->z - 7.5) < 1e-6
			&& fabs(p->y - wk_eye(fy)) < 1e-6, label);
	} else {
		for(unsigned i = 0; i < RZ_TEST_DEADLINE + 10; i++)
			tick(s);
		snprintf(label, sizeof label, "%s %s wake: permanent failure -> at the "
			"deadline a visible notice and a PROVEN-resident step-out", o->name,
			kind);
		check(hud_n >= 1 && server_world_columns_resident(w0, p->x - 0.3,
			p->z - 0.3, p->x + 0.3, p->z + 0.3)
			&& !(fabs(p->x - bed_x) < 1e-6 && fabs(p->z - bed_z) < 1e-6), label);
		server_world_test_fail_gen_attempts(0);
		(void)ticks_until_resident(s, w0, -1, 0, 600);
	}
	snprintf(label, sizeof label, "%s %s wake: no duplicate wake/spawn effect "
		"after the step-out", o->name, kind);
	check(!p->sleeping && p->has_bed_spawn == !damage, label);
	double sx = p->x, sz = p->z;
	sample(s, o, sx, eye_y, sz + 0.2, p->correction_seq);
	snprintf(label, sizeof label, "%s %s wake: a fresh move after the step-out "
		"is admitted", o->name, kind);
	check(fabs(p->z - (sz + 0.2)) < 1e-9, label);
	p->has_bed_spawn = spawn0;
	put(w0, 0, fy, 8, BLOCK_AIR);
}
static void wake_controls(struct server_local* s, const struct owner* o,
	struct server_player* other) {
	struct server_player* p = o->body;
	const double eye_y = s->player.y;
	const int fy = (int)floor(eye_y - PLAYER_EYE_HEIGHT);
	char label[200];
	/* resident control: the step-out is immediate, Beta's first cell */
	reset_body(other, 4.5, eye_y, 2.5);
	reset_body(p, 0.5, eye_y, 4.5);
	printf("     [wake %s: fy=%d, chunk -1 resident=%d]\n", o->name, fy,
		(int)server_world_chunk_resident(&s->worlds[0], -1, 0));
	wake_terrain(s, fy);
	printf("     [wake %s: terrain set]\n", o->name);
	sample(s, o, 0.5, eye_y, 4.5, p->correction_seq);
	tick(s);
	s->world_time = 13000;
	server_local_begin_sleep_for(s, p, 0, fy, 8);
	printf("     [wake %s: asleep=%d at (%.2f, %.2f)]\n", o->name,
		(int)p->sleeping, p->x, p->z);
	s->world_time = 1000;
	tick(s);
	snprintf(label, sizeof label, "%s resident control: daytime wake steps out "
		"at once onto Beta's first cell (-1, %d, 7)", o->name, fy);
	check(!p->sleeping && fabs(p->x + 0.5) < 1e-6 && fabs(p->z - 7.5) < 1e-6,
		label);
	/* destroyed bed: Beta leaves the sleeper where it lay (no step-out) */
	reset_body(p, 0.5, eye_y, 4.5);
	wake_terrain(s, fy);
	sample(s, o, 0.5, eye_y, 4.5, p->correction_seq);
	tick(s);
	s->world_time = 13000;
	server_local_begin_sleep_for(s, p, 0, fy, 8);
	const double bx = p->x, bz = p->z;
	put(&s->worlds[0], 0, fy, 8, BLOCK_AIR);
	tick(s);
	snprintf(label, sizeof label, "%s destroyed bed: wakes in place, no "
		"step-out, nothing held", o->name);
	check(!p->sleeping && fabs(p->x - bx) < 1e-9 && fabs(p->z - bz) < 1e-9,
		label);
	sample(s, o, bx, eye_y, bz + 0.2, p->correction_seq);
	check(fabs(p->z - (bz + 0.2)) < 1e-9, "destroyed bed: moves freely after");
}
static void case_wake(void) {
	struct server_local* s = admitted_fixture();
	struct owner ho = {.name = "host", .guest = false, .body = &s->player};
	struct owner go = {.name = "guest", .guest = true, .body = s->guest_actor};
	const struct owner* owners[2] = {&ho, &go};
	for(int i = 0; i < 2; i++) {
		struct server_player* other = i ? &s->player : s->guest_actor;
		wake_controls(s, owners[i], other);
		wake_one(s, owners[i], other, false, false);
		wake_one(s, owners[i], other, true, false);
		wake_one(s, owners[i], other, false, true);
	}
	server_world_test_fail_gen_attempts(0);
	teardown(s);
}

/* ======================================================================== */
/* Round 4 item 8: residency evidence with lifecycle identity, through the
 * REAL formatter (hw_telemetry_format_heartbeat) and parsed back from the
 * wire. A guest attempt fails (duration frozen at termination, then a
 * growing failed DWELL); between two heartbeat samples the guest Leaves and
 * rejoins and the secondary world is retired and recreated; the history
 * survives with new identities; a second guest failure is followed IN THE
 * SAME sample interval by another Leave/rejoin and is still counted; then a
 * failure -> dwell -> deliberate retry -> success. No movement or service
 * credit is inherited across a body or world change. */
struct rz_row { uint32_t v[HW_TELEMETRY_RZA_FIELDS]; };
static unsigned rz_samples;
static bool rz_sample(struct server_local* s, struct rz_row out[2],
	uint32_t* tick) {
	(void)s;
	struct hw_telemetry_heartbeat hb;
	memset(&hb, 0, sizeof hb);
	hb.tick = rz_samples++;
	hb.world_loaded = true;
	rz_fill_heartbeat(&hb);
	static char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(line, sizeof line,
		"residency_lifecycle_pc", "split_play_8m", &hb);
	if(!n || n >= sizeof line)
		return false;
	const char* t = strstr(line, "\"rzt\":");
	const char* p = strstr(line, "\"rza\":[[");
	if(!strstr(line, "\"rzv\":2,") || !t || !p)
		return false;
	*tick = (uint32_t)strtoul(t + 6, NULL, 10);
	p += 8;
	for(unsigned a = 0; a < 2; a++)
		for(unsigned i = 0; i < HW_TELEMETRY_RZA_FIELDS; i++) {
			char* end = NULL;
			out[a].v[i] = (uint32_t)strtoul(p, &end, 10);
			if(end == p)
				return false;
			p = end;
			while(*p == ',' || *p == '[' || *p == ']')
				p++;
		}
	return true;
}
static void tel_fail_guest(struct server_local* s, struct server_player* g) {
	server_world_test_fail_gen_attempts(100000);
	if(server_world_chunk_resident(&s->worlds[0], 3, 0))
		evict(&s->worlds[0], 3, 0);
	g->rz.pin.valid = false;
	g->x = 56.5;
	g->z = 0.5;
	for(unsigned i = 0; i < RZ_TEST_DEADLINE + 5 && !g->rz.failed; i++)
		tick(s);
}
static struct server_player* tel_rejoin(struct server_local* s) {
	guest_release(s);
	s->paused = false;
	guest_attach(s);
	struct client_session* gc = &gstate.players[1];
	assert(client_session_request_local_gameplay(gc));
	pump_handshake(s);
	assert(s->guest_actor && s->guest_actor->finished_loading);
	return s->guest_actor;
}
static void case_telemetry_lifecycle(void) {
	struct server_local* s = admitted_fixture();
	second_world_create(s);
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner go = {.name = "guest", .guest = true, .body = g};
	struct rz_row r1[2], r2[2], r3[2], r4[2], r5[2];
	uint32_t t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0;
	reset_body(h, 1.5, eye_y, 0.5);
	reset_body(g, 4.5, eye_y, 6.5);
	sample(s, &go, 4.5, eye_y, 6.5, g->correction_seq);
	for(int i = 0; i < 8; i++) { /* some ordinary admitted movement */
		sample(s, &go, 5.5 + i, eye_y, 6.5, g->correction_seq);
		tick(s);
	}
	/* --- A: a guest attempt fails; duration frozen, dwell grows --------- */
	tel_fail_guest(s, g);
	tick(s);
	check(rz_sample(s, r1, &t1), "telemetry: sample 1 formatted + parsed");
	for(int i = 0; i < 30; i++)
		tick(s);
	struct rz_row r1b[2];
	uint32_t t1b = 0;
	check(rz_sample(s, r1b, &t1b), "telemetry: sample 1b formatted + parsed");
	printf("     [A: guest state %u attempt %u last %u/%u failures %u dwell %u "
		"-> %u, epoch %u, moved %u dm]\n", r1[1].v[RZA_STATE],
		r1[1].v[RZA_ATTEMPT], r1[1].v[RZA_LAST_ATTEMPT_TICKS],
		r1[1].v[RZA_LAST_ATTEMPT_RESULT], r1[1].v[RZA_RECOVERY_FAILURES],
		r1[1].v[RZA_STATE_AGE], r1b[1].v[RZA_STATE_AGE],
		r1[1].v[RZA_BODY_EPOCH], r1[1].v[RZA_MOVED_DM]);
	check(r1[1].v[RZA_STATE] == RZA_STATE_FAILED
		&& r1[1].v[RZA_RECOVERY_FAILURES] == 1
		&& r1[1].v[RZA_LAST_ATTEMPT_RESULT] == 2
		&& r1[1].v[RZA_LAST_ATTEMPT_TICKS] <= RZ_TEST_DEADLINE
		&& r1[1].v[RZA_MOVED_DM] >= 70,
		"telemetry A: FAILED with the duration FROZEN (<= 200) at termination");
	check(r1b[1].v[RZA_LAST_ATTEMPT_TICKS] == r1[1].v[RZA_LAST_ATTEMPT_TICKS]
		&& r1b[1].v[RZA_STATE_AGE] >= r1[1].v[RZA_STATE_AGE] + 30
		&& t1b == t1 + 30,
		"telemetry A: the failed DWELL grows separately; the frozen duration "
		"does not; rzt is the server tick");
	/* --- B: two lifecycle transitions between two samples --------------- */
	server_world_test_fail_gen_attempts(0);
	g = tel_rejoin(s);                 /* guest Leave + rejoin */
	second_world_destroy(s);           /* world retirement ... */
	second_world_create(s);            /* ... and reuse of slot 1 */
	tick(s);
	check(rz_sample(s, r2, &t2), "telemetry: sample 2 formatted + parsed");
	check(r2[1].v[RZA_RECOVERY_FAILURES] == 1
		&& r2[1].v[RZA_BODY_EPOCH] > r1[1].v[RZA_BODY_EPOCH]
		&& r2[1].v[RZA_STATE] != RZA_STATE_FAILED,
		"telemetry B: Leave/rejoin + world retire/reuse between samples keep "
		"the failure history, under a NEW body epoch");
	check(r2[1].v[RZA_MOVED_DM] == r1b[1].v[RZA_MOVED_DM]
		&& r2[1].v[RZA_LOADED] == r1b[1].v[RZA_LOADED],
		"telemetry B: no movement / service credit is created by the body "
		"change (the new body inherits nothing; the session sum is unchanged)");
	/* the guest visits the recreated slot 1: a new world identity */
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s->worlds[1], 0, 0, &sc) && sc);
	struct owner go2 = {.name = "guest", .guest = true, .body = g};
	g->world_slot = 1;
	reset_body(g, 4.5, eye_y, 4.5);
	tick(s);
	check(rz_sample(s, r3, &t3), "telemetry: sample 3 formatted + parsed");
	check(r3[1].v[RZA_WORLD_SLOT] == 1
		&& r3[1].v[RZA_WORLD_GEN] == s->worlds[1].generation
		&& r3[1].v[RZA_BODY_EPOCH] > r2[1].v[RZA_BODY_EPOCH]
		&& r3[1].v[RZA_MOVED_DM] == r2[1].v[RZA_MOVED_DM],
		"telemetry: a world change is a new identity (slot 1 + its "
		"generation, new epoch); the teleport adds NO movement credit");
	g->world_slot = 0;
	reset_body(g, 4.5, eye_y, 6.5);
	tick(s);
	evict(&s->worlds[1], 0, 0);
	/* --- C: failure + Leave/rejoin inside ONE sample interval ----------- */
	tel_fail_guest(s, g);
	server_world_test_fail_gen_attempts(0);
	g = tel_rejoin(s);
	tick(s);
	check(rz_sample(s, r4, &t4), "telemetry: sample 4 formatted + parsed");
	check(r4[1].v[RZA_RECOVERY_FAILURES] == 2
		&& r4[1].v[RZA_ATTEMPT] > r1[1].v[RZA_ATTEMPT]
		&& r4[1].v[RZA_BODY_EPOCH] > r3[1].v[RZA_BODY_EPOCH],
		"telemetry C: a failure followed by a body reset BETWEEN samples is "
		"still counted (2), with a distinct attempt id and epoch");
	/* --- D: failure -> dwell -> deliberate retry -> success ------------- */
	go2.body = g;
	reset_body(g, 4.5, eye_y, 6.5);
	sample(s, &go2, 4.5, eye_y, 6.5, g->correction_seq);
	tick(s);
	tel_fail_guest(s, g);
	for(int i = 0; i < 20; i++)
		tick(s);
	unsigned retry_attempt = 0;
	server_world_test_fail_gen_attempts(0);
	sample(s, &go2, 57.5, eye_y, 0.5, g->correction_seq); /* the owner moves */
	for(int i = 0; i < 200 && g->rz.recovering; i++)
		tick(s);
	sample(s, &go2, 57.5, eye_y, 0.5, g->correction_seq); /* fresh move */
	tick(s);
	check(rz_sample(s, r5, &t5), "telemetry: sample 5 formatted + parsed");
	retry_attempt = r5[1].v[RZA_ATTEMPT];
	printf("     [D: failures %u retries %u exits_ok %u last %u/%u attempt %u "
		"epoch %u state %u]\n", r5[1].v[RZA_RECOVERY_FAILURES],
		r5[1].v[RZA_RETRIES], r5[1].v[RZA_RECOVERY_EXITS_OK],
		r5[1].v[RZA_LAST_ATTEMPT_TICKS], r5[1].v[RZA_LAST_ATTEMPT_RESULT],
		retry_attempt, r5[1].v[RZA_BODY_EPOCH], r5[1].v[RZA_STATE]);
	check(r5[1].v[RZA_RECOVERY_FAILURES] == 3 && r5[1].v[RZA_RETRIES] >= 1
		&& r5[1].v[RZA_RECOVERY_EXITS_OK] > r4[1].v[RZA_RECOVERY_EXITS_OK]
		&& r5[1].v[RZA_LAST_ATTEMPT_RESULT] == 1
		&& r5[1].v[RZA_LAST_ATTEMPT_TICKS] <= RZ_TEST_DEADLINE
		&& retry_attempt > r4[1].v[RZA_ATTEMPT]
		&& r5[1].v[RZA_STATE] == RZA_STATE_OK,
		"telemetry D: failure -> dwell -> retry -> success is visible as "
		"failures 3, a retry, a new successful exit with a fresh attempt id");
	check(t1 < t2 && t2 < t3 && t3 < t4 && t4 < t5,
		"telemetry: rzt strictly increases across the samples");
	check(r5[0].v[RZA_RECOVERY_FAILURES] == 0 && r5[0].v[RZA_WORLD_SLOT] == 0,
		"telemetry: the host row is untouched by the guest's history");
	server_world_test_fail_gen_attempts(0);
	second_world_destroy(s);
	teardown(s);
}

/* ======================================================================== */
/* Round 4 item 12: the runtime residency fault (route E) through its actual
 * consumer (server_local_rz_fault_request -> the server update). Dormant: no
 * effect. Refused for an absent / wrong owner and when the partner shares a
 * column. Armed for the guest: its footprint columns fail for exactly ONE
 * bounded attempt (terminal + notice by entry + 200, marked in rzi, phases
 * in rzf), the host keeps loading and moving, the fault disarms at the
 * terminal, the guest's deliberate retry succeeds, and a new session starts
 * with no mark and no fault. */
static void case_runtime_fault(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	reset_body(h, 1.5, eye_y, 0.5);
	reset_body(g, 40.5, eye_y, 40.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	sample(s, &go, 40.5, eye_y, 40.5, g->correction_seq);
	tick(s);
	/* dormant */
	for(int i = 0; i < 20; i++)
		tick(s);
	check(server_local_rz_fault_phases() == 0 && s->rzi_count == 0
		&& !g->rz.recovering && server_world_rz_fault_blocked() == 0,
		"runtime fault: dormant -- no phase, no mark, no blocked load");
	/* refused: the partner shares the target column */
	reset_body(h, 41.5, eye_y, 40.5);
	sample(s, &ho, 41.5, eye_y, 40.5, h->correction_seq);
	tick(s);
	server_local_rz_fault_request(1);
	tick(s);
	check((server_local_rz_fault_phases() & RZF_REFUSED)
		&& !(server_local_rz_fault_phases() & RZF_ARMED) && !g->rz.recovering,
		"runtime fault: REFUSED while the partner's footprint shares a column");
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	/* armed for the guest */
	hud_reset();
	server_local_rz_fault_request(1);
	tick(s); /* arm */
	unsigned entry = 0;
	for(int i = 0; i < 5 && !entry; i++) {
		tick(s);
		if(s->rz_fault.entered)
			entry = g_test_tick;
	}
	check((server_local_rz_fault_phases() & (RZF_ARMED | RZF_ENTERED))
		== (RZF_ARMED | RZF_ENTERED) && g->rz.recovering && s->rzi_count == 1
		&& s->rzi[0][0] == 1 && s->rzi[0][1] == w0->world_index
		&& s->rzi[0][2] == w0->generation && s->rzi[0][4] == s->rz_fault.attempt,
		"runtime fault: armed for the guest's CURRENT body/world/attempt; the "
		"attempt is marked (actor, slot, generation, epoch, attempt)");
	/* the partner keeps progressing while the guest is held */
	double hx0 = h->x;
	unsigned fails0 = g->rz.recovery_failures;
	evict(w0, 1, 0);
	for(unsigned i = 0; i < RZ_TEST_DEADLINE + 5 && !g->rz.failed; i++) {
		if(i % 5 == 0 && h->x < 20.0)
			sample(s, &ho, h->x + 1.0, eye_y, 0.5, h->correction_seq);
		tick(s);
	}
	unsigned fail_at = 0, nfail = 0;
	for(unsigned i = 0; i < hud_n; i++)
		if(!strcmp(hud_text[i], RZ_TEST_FAIL_TEXT) && !nfail++)
			fail_at = hud_tick[i];
	printf("     [runtime fault: entry %u, failure toast at E+%u, blocked loads "
		"%u, host %.1f -> %.1f, phases 0x%x]\n", entry, fail_at - entry,
		server_world_rz_fault_blocked(), hx0, h->x,
		server_local_rz_fault_phases());
	check(g->rz.failed && g->rz.recovery_failures == fails0 + 1
		&& nfail == 1 && fail_at - entry <= RZ_TEST_DEADLINE
		&& server_world_rz_fault_blocked() > 0,
		"runtime fault: the targeted attempt fails permanently (its loads "
		"blocked) and its pane is told by entry + 200");
	check(h->x > hx0 + 8.0 && server_world_chunk_resident(w0, 1, 0)
		&& !h->rz.recovering,
		"runtime fault: the partner keeps loading and moving (column (1,0) "
		"reloaded, host walked)");
	tick(s);
	uint32_t ph = server_local_rz_fault_phases();
	check((ph & (RZF_TERMINAL | RZF_NOTICE | RZF_DISARMED))
		== (RZF_TERMINAL | RZF_NOTICE | RZF_DISARMED) && !s->rz_fault.armed,
		"runtime fault: terminal + notice evidence, then DISARMED");
	/* R7 guest90 lines 437/440: terminal request was dropped; the ordinary
	 * streamer loaded between disarm and owner retry, so loaded stayed zero.
	 * Delay owner input to force that exact ordering. No synthetic retry or
	 * fabricated service: credit only the real resident-column observation. */
	for(unsigned i = 0; i < 20; i++) tick(s);
	check(server_world_chunk_resident(w0, 2, 2) && g->rz.loaded == 1
		&& g->rz.failed && g->rz.recovery_failures == fails0 + 1,
		"R7 guest fault: late real service attributed, terminal failure unchanged");
	/* the guest's deliberate retry succeeds normally */
	sample(s, &go, 41.5, eye_y, 40.5, g->correction_seq);
	for(int i = 0; i < 200 && g->rz.recovering; i++)
		tick(s);
	sample(s, &go, 41.5, eye_y, 40.5, g->correction_seq);
	check(!g->rz.recovering && fabs(g->x - 41.5) < 1e-9
		&& (server_local_rz_fault_phases() & RZF_RETRY)
		&& (server_local_rz_fault_phases() & RZF_DISARMED) && !s->rz_fault.armed,
		"runtime fault: the deliberate fresh retry recovers and the next "
		"fresh move is admitted (retry evidence recorded)");
	tick(s); /* service retirement is published on the next server update */
	check(g->rz.requests >= 1 && g->rz.loaded == 1,
		"R7 guest fault: one late service counted exactly once across owner retry");
	/* Receipts have no authority after their world/age/lifecycle expires. */
	struct rz_request receipt = {.cx = 2, .cz = 2, .cancelled = true,
		.world_index = w0->world_index, .world_generation = w0->generation + 1,
		.born = s->near_send_seq};
	g->rz.req[5] = receipt;
	tick(s);
	check(!g->rz.req[5].cancelled && g->rz.loaded == 1,
		"R7 late-service receipt: another world generation cannot receive credit");
	receipt.world_generation = w0->generation;
	receipt.born = s->near_send_seq - 401;
	g->rz.req[5] = receipt;
	tick(s);
	check(!g->rz.req[5].cancelled && g->rz.loaded == 1,
		"R7 late-service receipt: expired receipt cannot receive credit");
	receipt.born = s->near_send_seq;
	g->rz.req[5] = receipt;
	server_local_rz_retire_body(g);
	check(!g->rz.req[5].cancelled,
		"R7 late-service receipt: loading/transfer retirement clears accounting");
	teardown(s);
	/* clean session reuse: a request left pending at session end and a
	 * fresh server_local_init in the same process (poisoned storage) */
	server_local_rz_fault_request(0);
	struct server_local* s2 = poisoned_fixture();
	server_local_test_update_once(s2);
	check(server_local_rz_fault_phases() == 0 && s2->rzi_count == 0
		&& !s2->rz_fault.armed && s2->rzf == 0,
		"runtime fault: a new session in the same process starts with no "
		"mark, phase, pending request or armed fault");
	split_destroy(s2);
}

/* ======================================================================== */
/* residency-1005/4 closure item 2: ACTUAL vehicles at a missing-column seam.
 * A real cart (host: SRPC_MINECART_RIDE_INTENT) and real boats (host:
 * SRPC_ENTITY_USE desired_occupied) are mounted through production, steered
 * through production (boat: the rider's PLAYER_POS vel_x/vel_z, the
 * boat_tick_server steering source; cart: SRPC_MINECART_CONTROL), ticked by
 * the real server update (an entity ticks only while its +/-32 box is
 * resident) and dismounted. The vehicle sits in resident chunk 0 next to
 * chunk 1, which is evicted: first a DELAYED load (every attempt fails for a
 * while, then the fault clears) and then a PERMANENTLY failed one (through
 * the deadline, then a deliberate fresh retry). A fully resident control
 * runs first.
 * Guest boats: since ride R2 (maintainer ruling 2026-10-08) the guest boat
 * MOUNT/DISMOUNT goes through the production SRPC_ENTITY_USE admission like
 * the host's, and the guest's steering drives its own PLAYER_POS route. Guest carts have no verbs
 * (case held_controls). World ownership: both bodies are in world 0 for
 * every leg; the roles are swapped (host rides with the guest as the idle
 * partner, then the guest rides with the host as partner). A slot-1 vehicle
 * world is not used: the hand-made slot-1 world is not served by the A2-W
 * streamer in this fixture, and the item-4 transfer path below binds a
 * slot-1 world only for a body, without a vehicle stream. */
enum { VH_CART, VH_BOAT };
static void vh_sample(struct server_local* s, const struct owner* o,
	double x, double z, uint32_t seq, float vx, float vz) {
	struct server_rpc call = pose_sample(x, s->player.y, z, seq);
	call.payload.player_pos.vel_x = vx;
	call.payload.player_pos.vel_z = vz;
	bool was = s->paused;
	s->paused = false;
	if(o->guest)
		assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
			.generation = s->guest_generation}, &call));
	else
		assert(svin_rpc_send(&call));
	drain_server(s);
	s->paused = was;
}
static void vh_use(struct server_local* s, const struct owner* o,
	uint32_t id, bool desired) {
	struct server_rpc call = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = id, .desired_occupied = desired}};
	bool was = s->paused;
	s->paused = false;
	if(o->guest)
		(void)svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
			.generation = s->guest_generation}, &call);
	else
		assert(svin_rpc_send(&call));
	drain_server(s);
	s->paused = was;
}
static bool vh_rides(const struct entity* e, const struct server_player* p) {
	if(!e)
		return false;
	if(e->type == ENTITY_BOAT)
		return e->data.boat.occupied && p->riding_boat;
	return e->data.minecart.occupied && p->riding_minecart;
}
static void vh_mount(struct server_local* s, const struct owner* o,
	struct entity* e, int kind, const char* tag) {
	char label[240];
	struct server_player* p = o->body;
	if(kind == VH_CART) {
		struct server_rpc mount = hc_intent(s, e->id, true);
		host_rpc(s, &mount);
	} else {
		/* ride R2 (maintainer ruling 2026-10-08: split P2 may ride boats):
		 * the guest mounts through the same production SRPC_ENTITY_USE
		 * admission as the host (Beta EntityBoat.interact:336-345). */
		vh_use(s, o, e->id, true);
	}
	snprintf(label, sizeof label, "%s: mounted (occupancy + rider flag)",
		tag);
	check(vh_rides(e, p), label);
}
/* the vehicle's own steering memory: boat rider_vel / cart pending_throttle */
static float vh_impulse(const struct entity* e) {
	if(e->type == ENTITY_BOAT)
		return fabsf(e->data.boat.rider_vel_x)
			+ fabsf(e->data.boat.rider_vel_z);
	return (float)e->data.minecart.pending_throttle;
}
static uint32_t vh_cseq;
/* One steering input of this rider: boat = a PLAYER_POS sample carrying the
 * stick velocity; cart = a cumulative control tuple + the seat sample. */
static void vh_steer_v(struct server_local* s, const struct owner* o,
	struct entity* e, int kind, double x, double z, float vx) {
	struct server_player* p = o->body;
	if(kind == VH_CART) {
		++vh_cseq;
		struct server_rpc c = hc_control(e, vh_cseq, vh_cseq, 0);
		host_rpc(s, &c);
	}
	vh_sample(s, o, x, z, p->correction_seq, vx, 0.0F);
}
static void vh_steer(struct server_local* s, const struct owner* o,
	struct entity* e, int kind, double x, double z) {
	vh_steer_v(s, o, e, kind, x, z, 0.25F);
}
static bool vh_same_pos(const struct entity* e, const vec3 pos) {
	return e->pos[0] == pos[0] && e->pos[1] == pos[1] && e->pos[2] == pos[2];
}

static void vehicle_leg(struct server_local* s, const struct owner* o,
	const struct owner* partner, int kind, double lz) {
	char tag[64], label[260];
	struct server_world* w0 = &s->worlds[0];
	struct server_player* p = o->body;
	struct server_player* q = partner->body;
	const double eye_y = s->player.y;
	snprintf(tag, sizeof tag, "vehicles %s/%s", o->name,
		kind == VH_CART ? "cart" : "boat");
	/* resident start: everything loaded, the rider beside the seam; a
	 * rail line (cart) or a water channel (boat) crosses the seam so the
	 * vehicle's resident physics actually moves it */
	server_world_test_fail_gen_attempts(0);
	(void)tick_until_resident(s, w0, 1, 0, 600);
	for(int x = 8; x <= 24; x++) {
		if(kind == VH_CART)
			put(w0, x, 4, (int)floor(lz), BLOCK_RAIL);
		else
			for(int dz = -1; dz <= 1; dz++)
				put(w0, x, 3, (int)floor(lz) + dz, BLOCK_WATER_STILL);
		clin_update_drain();
	}
	reset_body(p, 14.5, eye_y, lz);
	vh_sample(s, o, 14.5, lz, p->correction_seq, 0.0F, 0.0F);
	tick(s);
	struct entity* e = kind == VH_CART
		? server_local_spawn_minecart((vec3) {15.0F, 4.0F, (float)lz}, s)
		: server_local_spawn_boat((vec3) {15.0F, 4.0F, (float)lz}, s);
	assert(e);
	clin_update_drain();
	const uint32_t id = e->id;
	vh_mount(s, o, e, kind, tag);

	/* ---- fully RESIDENT control --------------------------------------- */
	float odo0 = p->distance_walked;
	vec3 v0;
	glm_vec3_copy(e->pos, v0);
	vh_steer(s, o, e, kind, 15.0, lz);
	snprintf(label, sizeof label, "%s control: a resident steering sample is "
		"admitted (pose + steering source; riding = no odometer)", tag);
	check(p->x == 15.0 && p->rider_vel_x == 0.25F
		&& p->distance_walked == odo0, label);
	if(kind == VH_CART) {
		snprintf(label, sizeof label, "%s control: the resident control tuple "
			"adds its step", tag);
		check(e->data.minecart.pending_throttle >= 1, label);
	}
	for(int i = 0; i < 4; i++)
		tick(s);
	printf("     [%s control: vehicle (%.3f,%.3f) -> (%.3f,%.3f) impulse "
		"%.3f]\n", tag, v0[0], v0[2], e->pos[0], e->pos[2], vh_impulse(e));
	snprintf(label, sizeof label, "%s control: the vehicle TICKS while its "
		"box is resident (it moved / consumed the impulse)", tag);
	check(!vh_same_pos(e, v0)
		|| (kind == VH_CART && e->data.minecart.pending_throttle == 0),
		label);
	/* the rider moves to the seam: its footprint will touch chunk 1 */
	vh_sample(s, o, 15.8, lz, p->correction_seq, 0.0F, 0.0F);
	check(p->x == 15.8, "vehicles: rider at the seam (x 15.8, footprint "
		"spans chunks 0 and 1)");
	tick(s);

	/* ---- DELAYED loading at the missing seam -------------------------- */
	server_world_test_fail_gen_attempts(100000);
	evict(w0, 1, 0);
	unsigned rej0 = p->rz.rejects;
	tick(s);
	snprintf(label, sizeof label, "%s delayed: the rider whose footprint "
		"lost chunk 1 is HELD (recovery), still seated", tag);
	check(p->rz.recovering && vh_rides(e, p), label);
	snprintf(label, sizeof label, "%s delayed: entry discarded the rider's "
		"steering source and the cart throttle", tag);
	check(p->rider_vel_x == 0.0F && p->rider_vel_z == 0.0F
		&& (kind != VH_CART || e->data.minecart.pending_throttle == 0),
		label);
	glm_vec3_copy(e->pos, v0);
	odo0 = p->distance_walked;
	const double px0 = p->x, pz0 = p->z;
	unsigned c0 = corrections(o);
	vh_steer(s, o, e, kind, 16.2, lz);
	vh_steer(s, o, e, kind, 16.4, lz);
	snprintf(label, sizeof label, "%s delayed: steering samples into the "
		"missing column are REFUSED and corrected (pose, odometer and "
		"steering unchanged; no throttle)", tag);
	check(p->x == px0 && p->z == pz0 && p->distance_walked == odo0
		&& p->rider_vel_x == 0.0F && p->rz.rejects >= rej0 + 2
		&& corrections(o) > c0
		&& (kind != VH_CART || e->data.minecart.pending_throttle == 0),
		label);
	bool need = false;
	for(unsigned k = 0; k < RZ_REQ_MAX; k++)
		need |= p->rz.req[k].used && p->rz.req[k].cx == 1
			&& p->rz.req[k].cz == 0 && p->rz.req[k].prio == RZ_PRIO_NEED;
	snprintf(label, sizeof label, "%s delayed: the rider owns a NEED request "
		"for the missing column", tag);
	check(need, label);
	/* the partner keeps progressing */
	const double qx0 = q->x;
	vh_sample(s, partner, q->x + 0.3, q->z, q->correction_seq, 0.0F, 0.0F);
	snprintf(label, sizeof label, "%s delayed: the partner %s keeps moving",
		tag, partner->name);
	check(fabs(q->x - (qx0 + 0.3)) < 1e-9, label);
	for(int i = 0; i < 10; i++)
		tick(s);
	snprintf(label, sizeof label, "%s delayed: the vehicle does not tick "
		"into the missing box (pose frozen, still seated)", tag);
	check(vh_same_pos(e, v0) && vh_rides(e, p) && p->x == px0, label);
	/* the fault clears: the production loader serves the NEED column */
	server_world_test_fail_gen_attempts(0);
	unsigned waited = 0;
	while(p->rz.recovering && waited < 200) {
		tick(s);
		waited++;
	}
	printf("     [%s delayed: recovery %u ticks after the fault cleared]\n",
		tag, waited);
	snprintf(label, sizeof label, "%s delayed: recovered through the "
		"production loader within 40 ticks of the fault clearing", tag);
	check(!p->rz.recovering && !p->rz.failed && waited <= 40, label);
	tick(s);
	snprintf(label, sizeof label, "%s delayed: NO stale impulse after "
		"recovery (rider steering 0, vehicle impulse 0, pose not replayed)",
		tag);
	check(p->rider_vel_x == 0.0F && vh_impulse(e) == 0.0F && p->x == px0
		&& vh_rides(e, p), label);
	/* a FRESH steering move after loading */
	/* steered back AWAY from the seam: the vehicle must stay in chunk 0,
	 * since the permanent leg evicts chunk 1 again (an evicted chunk saves
	 * and unloads the entities it holds) */
	vh_steer_v(s, o, e, kind, 15.6, lz, -0.25F);
	snprintf(label, sizeof label, "%s delayed: a FRESH steering sample after "
		"loading moves the rider (and steers / throttles again)", tag);
	check(p->x == 15.6 && p->rider_vel_x == -0.25F
		&& (kind != VH_CART || e->data.minecart.pending_throttle >= 1),
		label);
	glm_vec3_copy(e->pos, v0);
	for(int i = 0; i < 4; i++)
		tick(s);
	printf("     [%s delayed: after fresh steer vehicle (%.3f,%.3f)->(%.3f,"
		"%.3f) vel %.3f impulse %.3f res -2..3: %d%d%d%d%d%d]\n", tag, v0[0],
		v0[2], e->pos[0], e->pos[2], e->vel[0], vh_impulse(e),
		server_world_chunk_resident(w0, -2, 0),
		server_world_chunk_resident(w0, -1, 0),
		server_world_chunk_resident(w0, 0, 0),
		server_world_chunk_resident(w0, 1, 0),
		server_world_chunk_resident(w0, 2, 0),
		server_world_chunk_resident(w0, 3, 0));
	snprintf(label, sizeof label, "%s delayed: the vehicle ticks again once "
		"resident", tag);
	check(!vh_same_pos(e, v0)
		|| (kind == VH_CART && e->data.minecart.pending_throttle == 0),
		label);
	/* a following client: the rider stays at its seat and steers the boat
	 * back to x ~15 (chunk 0), then brakes it to a stop */
	if(kind == VH_BOAT) {
		for(int i = 0; i < 200; i++) {
			const float want = e->pos[0] > 15.0F ? -0.25F
				: (e->vel[0] < -0.004F ? 0.25F : 0.0F);
			if(want == 0.0F && fabsf(e->vel[0]) < 0.004F)
				break;
			vh_sample(s, o, e->pos[0], lz, p->correction_seq, want, 0.0F);
			tick(s);
		}
	}
	/* back to the seam for the permanent leg; steering released */
	vh_sample(s, o, 15.8, lz, p->correction_seq, 0.0F, 0.0F);
	for(int i = 0; i < 10; i++)
		tick(s);
	check(p->x == 15.8, "vehicles: the rider is back at the seam");
	snprintf(label, sizeof label, "%s permanent: the vehicle sits in resident "
		"chunk 0 beside the seam (x %.3f)", tag, e->pos[0]);
	check(e->pos[0] < 16.0F - 0.75F && e->pos[0] > 8.0F, label);

	/* ---- PERMANENTLY failed loading ----------------------------------- */
	server_world_test_fail_gen_attempts(100000);
	evict(w0, 1, 0);
	const unsigned fail0 = p->rz.recovery_failures, retry0 = p->rz.retries;
	hud_reset();
	tick(s);
	const unsigned entry = g_test_tick;
	check(p->rz.recovering, "vehicles permanent: hold entered");
	while(!p->rz.failed && g_test_tick - entry < RZ_TEST_DEADLINE + 10)
		tick(s);
	printf("     [%s permanent: FAILED at +%u ticks]\n", tag,
		g_test_tick - entry);
	snprintf(label, sizeof label, "%s permanent: the attempt TERMINATES by "
		"the deadline, one failure, rider still seated", tag);
	check(p->rz.failed && p->rz.recovery_failures == fail0 + 1
		&& g_test_tick - entry <= RZ_TEST_DEADLINE && vh_rides(e, p), label);
	glm_vec3_copy(e->pos, v0);
	odo0 = p->distance_walked;
	const double fx0 = p->x;
	vh_steer(s, o, e, kind, fx0, lz); /* idle steering: not a retry */
	snprintf(label, sizeof label, "%s permanent: an IDLE steering sample "
		"(kept pose) is refused, adds no impulse and does not retry", tag);
	check(p->rz.failed && p->rz.retries == retry0 && p->rider_vel_x == 0.0F
		&& (kind != VH_CART || e->data.minecart.pending_throttle == 0),
		label);
	/* dismount is a held entity action: unchanged authority */
	if(kind == VH_CART) {
		struct server_rpc off = hc_intent(s, id, false);
		host_rpc(s, &off);
	} else if(!o->guest) {
		vh_use(s, o, id, false);
	} else {
		vh_use(s, o, id, false); /* fail-closed guest route */
	}
	snprintf(label, sizeof label, "%s permanent: a dismount request from the "
		"FAILED hold leaves occupancy/rider flag unchanged", tag);
	check(vh_rides(e, p), label);
	for(int i = 0; i < 10; i++)
		tick(s);
	snprintf(label, sizeof label, "%s permanent: the vehicle stays frozen, "
		"no partial movement (pose, odometer)", tag);
	check(vh_same_pos(e, v0) && p->x == fx0 && p->distance_walked == odo0,
		label);
	/* the deliberate fresh retry, then recovery through the loader */
	server_world_test_fail_gen_attempts(0);
	vh_steer(s, o, e, kind, 16.3, lz);
	snprintf(label, sizeof label, "%s permanent: the rider's deliberate move "
		"starts ONE retry; the sample is discarded (no impulse kept)", tag);
	check(!p->rz.failed && p->rz.recovering && p->rz.retries == retry0 + 1
		&& p->x == fx0 && p->rider_vel_x == 0.0F
		&& (kind != VH_CART || e->data.minecart.pending_throttle == 0),
		label);
	waited = 0;
	while(p->rz.recovering && waited < 200) {
		tick(s);
		waited++;
	}
	printf("     [%s permanent: retry recovered in %u ticks]\n", tag, waited);
	tick(s);
	printf("     [%s permanent after retry: rec=%d x=%.3f fx0=%.3f rv=%.3f "
		"imp=%.3f rides=%d boat=(%.3f,%.3f) occ=%d]%c", tag,
		(int)p->rz.recovering, p->x, fx0, p->rider_vel_x, vh_impulse(e),
		(int)vh_rides(e, p), e->pos[0], e->pos[2],
		e->type == ENTITY_BOAT ? (int)e->data.boat.occupied : -1, 10);
	snprintf(label, sizeof label, "%s permanent: the retry recovers (<=200), "
		"no replay, no stale impulse, still seated", tag);
	check(!p->rz.recovering && waited <= 200 && p->x == fx0
		&& p->rider_vel_x == 0.0F && vh_impulse(e) == 0.0F
		&& vh_rides(e, p), label);
	vh_steer(s, o, e, kind, 16.2, lz);
	snprintf(label, sizeof label, "%s permanent: a FRESH move after loading "
		"is admitted", tag);
	check(p->x == 16.2 && p->rider_vel_x == 0.25F, label);
	/* production dismount once released */
	if(kind == VH_CART) {
		struct server_rpc off = hc_intent(s, id, false);
		host_rpc(s, &off);
	} else {
		/* ride R2: the guest dismounts through production use too */
		vh_use(s, o, id, false);
	}
	snprintf(label, sizeof label, "%s: dismounted once resident (occupancy "
		"and rider flag cleared together)", tag);
	check(!vh_rides(e, p) && !p->riding_boat && !p->riding_minecart
		&& (e->type == ENTITY_BOAT ? !e->data.boat.occupied
								   : !e->data.minecart.occupied),
		label);
	tick(s);
}
static void case_vehicles(void) {
	struct server_local* s = admitted_fixture();
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	/* roles A: the host rides, the guest is the partner */
	reset_body(g, 4.5, eye_y, 4.5);
	sample(s, &go, 4.5, eye_y, 4.5, g->correction_seq);
	tick(s);
	vehicle_leg(s, &ho, &go, VH_CART, 0.5);
	reset_body(g, 4.5, eye_y, 4.5);
	sample(s, &go, 4.5, eye_y, 4.5, g->correction_seq);
	vehicle_leg(s, &ho, &go, VH_BOAT, 3.5);
	/* roles B (swapped): the guest rides, the host is the partner */
	reset_body(h, 4.5, eye_y, 6.5);
	sample(s, &ho, 4.5, eye_y, 6.5, h->correction_seq);
	tick(s);
	vehicle_leg(s, &go, &ho, VH_BOAT, 9.5);
	teardown(s);
}

/* ======================================================================== */
/* residency-1005/4 closure item 2 (knockback): ACTUAL damage -> the client
 * velocity it produces -> the client's own movement production -> server
 * admission at the missing seam. The hit is server_local_damage_actor_result
 * (Beta knockBack on the server copy, delivered as CRPC_LOCAL_PLAYER_VELOCITY
 * and installed by the client); the client body integrates it with the real
 * main.c host tick dispatch (no stick input), which integrates the body,
 * attaches its camera and publishes the completed PLAYER_POS. Three runs
 * from the SAME start
 * pose: a fully resident control, the run with chunk 1 missing (loads
 * failing), and a run after the column is resident again, which must
 * reproduce the control trajectory exactly. */
#include "platform/input_owner.h"
#define KB_STEPS 10
static float kb_y0;
static bool kb_og0, kb_have0;
static void kb_place(struct server_local* s, const struct owner* o,
	double x, double z) {
	struct server_player* p = o->body;
	const double eye_y = s->player.y;
	reset_body(p, x, eye_y, z);
	sample(s, o, x, eye_y, z, p->correction_seq);
	struct entity* e = gstate.players[0].local_player;
	if(!kb_have0) {
		kb_y0 = e->pos[1];
		kb_og0 = e->on_ground;
		kb_have0 = true;
	}
	e->pos[0] = (float)x;
	e->pos[1] = kb_y0;
	e->pos[2] = (float)z;
	e->on_ground = kb_og0;
	glm_vec3_copy(e->pos, e->pos_old);
	glm_vec3_zero(e->vel);
	tick(s);
}
static void kb_hit(struct server_local* s, struct server_player* p) {
	p->hurt_resist_time = 0;
	p->health = MAX_PLAYER_HEALTH;
	bool accepted = false;
	(void)server_local_damage_actor_result(s, p, 1, 1.0F, 0.0F, &accepted);
	clin_update_drain();
	check(accepted, "knockback: the hit is accepted");
}
/* one client frame: integrate the body, attach the camera, produce, tick */
static void kb_step(struct server_local* s) {
	client_produce(s);
	tick(s);
}
static float kb_odo[KB_STEPS];
static void kb_run(struct server_local* s, const struct owner* o,
	double x0, double out[KB_STEPS], float* client_vx) {
	struct entity* e = gstate.players[0].local_player;
	kb_place(s, o, x0, 0.5);
	kb_hit(s, o->body);
	/* G8-BLAST-VELOCITY (ASTRA-V-REVIEW (b), v2+): the hit only raises
	 * beenAttacked; the client velocity is the tracker phase's ONE absolute
	 * Packet28 at the end of the next real update (EntityTrackerEntry:
	 * 125-127). One update delivers it before the client integrates. */
	tick(s);
	*client_vx = e->vel[0];
	for(int i = 0; i < KB_STEPS; i++) {
		kb_step(s);
		out[i] = o->body->x;
		kb_odo[i] = o->body->distance_walked;
	}
}
static void case_knockback(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* w0 = &s->worlds[0];
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	char label[200];
	assert(gstate.players[0].local_player);
	reset_body(g, 4.5, eye_y, 4.5);
	const double x0 = 15.3;
	double ctl[KB_STEPS], seam[KB_STEPS], post[KB_STEPS];
	float ctl_odo[KB_STEPS];
	float vc = 0, vs = 0, vp = 0;
	/* control: everything resident */
	kb_run(s, &ho, x0, ctl, &vc);
	memcpy(ctl_odo, kb_odo, sizeof ctl_odo);
	printf("     [knockback control: client vel_x %.4f, server x:", vc);
	for(int i = 0; i < KB_STEPS; i++)
		printf(" %.3f", ctl[i]);
	printf("]\n");
	check(vc > 0.0F && ctl[KB_STEPS - 1] > x0 + 0.5,
		"knockback control: the hit produced client velocity and the "
		"client's samples carried the body across the seam (resident)");
	/* the seam: chunk 1 missing, every load failing */
	kb_place(s, &ho, x0, 0.5);
	server_world_test_fail_gen_attempts(100000);
	evict(w0, 1, 0);
	const float odo0 = h->distance_walked;
	const unsigned rej0 = h->rz.rejects, c0 = corrections(&ho);
	kb_run(s, &ho, x0, seam, &vs);
	printf("     [knockback seam: client vel_x %.4f, server x:", vs);
	for(int i = 0; i < KB_STEPS; i++)
		printf(" %.3f", seam[i]);
	printf(", rejects +%u, corrections +%u]\n", h->rz.rejects - rej0,
		corrections(&ho) - c0);
	bool inside = true;
	for(int i = 0; i < KB_STEPS; i++)
		inside &= seam[i] + 0.3 < 16.0;
	check(vs == vc, "knockback seam: the same hit produced the same client "
		"velocity");
	check(inside && h->rz.rejects > rej0 && corrections(&ho) > c0,
		"knockback seam: every displacement into the missing column was "
		"REFUSED and corrected (the committed footprint never touched it)");
	check(h->fall_distance == 0.0F && h->health == MAX_PLAYER_HEALTH - 1,
		"knockback seam: no partial movement side effect (no fall, no "
		"extra damage)");
	snprintf(label, sizeof label, "knockback seam: the odometer advanced only "
		"by the admitted first step (seam step 1 %.4f, seam total %.4f; "
		"control total %.4f)", (double)(kb_odo[0] - odo0),
		(double)(kb_odo[KB_STEPS - 1] - odo0),
		(double)(ctl_odo[KB_STEPS - 1] - ctl_odo[0]));
	check(kb_odo[KB_STEPS - 1] == kb_odo[0], label);
	/* the column becomes resident through the production loader */
	server_world_test_fail_gen_attempts(0);
	check(tick_until_resident(s, w0, 1, 0, 200),
		"knockback: chunk 1 resident again");
	for(int i = 0; i < 5 && h->rz.recovering; i++)
		tick(s);
	kb_run(s, &ho, x0, post, &vp);
	printf("     [knockback post: client vel_x %.4f, server x:", vp);
	for(int i = 0; i < KB_STEPS; i++)
		printf(" %.3f", post[i]);
	printf("]\n");
	bool same = vp == vc;
	for(int i = 0; i < KB_STEPS; i++)
		same &= post[i] == ctl[i];
	check(same, "knockback post: a fresh hit once resident reproduces the "
		"resident control trajectory exactly (existing knockback behaviour)");
	teardown(s);
}

/* ======================================================================== */
/* residency-1005/4 closure item 4: bounded recovery for host/slot 0,
 * guest/slot 0, host/slot 1 and guest/slot 1 with ACTUAL bindings. Slot 1 is
 * reached by a REAL A2 transfer (server_transfer_request -> COMMITTED through
 * real updates), so its columns are served by the A2-W stream / the owning
 * production loader; nothing here calls server_world_load_chunk. Per body:
 *   delayed leg  -- a moved sample into an evicted column while every load
 *                   fails: refused, NEED request for that column; the fault
 *                   clears and the production loader serves it; the next
 *                   sample is admitted (refusal/service <= 40 ticks after the
 *                   fault clears);
 *   permanent    -- the body on an evicted column (no eligible pin) with loads
 *                   failing: NEED ownership of its footprint column, terminal
 *                   failure by +200, then a deliberate moved sample on the
 *                   live token starts one retry that recovers through the
 *                   production loader (<= 200) and a fresh sample moves it.
 * Throughout: each body's used requests <= 6, total <= 12; the other actor
 * keeps moving while the first is held.
 * Bindings: the case runs with world-bound payloads ON (main(): the A2
 * two-world protocol and its A2-W stream are opt-in; without them a slot-1
 * world has no stream and its NEED requests are never picked). Order: host
 * and guest in slot 0; the host transfers to the Nether (slot 1, the guest
 * stays in slot 0); then the guest transfers to the Nether too and joins the
 * host's live slot-1 world (the host is its partner there). A host return
 * transfer (Nether -> Overworld from a portal-less landing) rolled back in
 * this fixture, so the guest/slot 1 leg is a shared slot 1, not an exclusive
 * one. */
static unsigned bs_req_max_each, bs_req_max_total;
static unsigned bs_used(const struct server_player* p) {
	unsigned n = 0;
	for(unsigned k = 0; k < RZ_REQ_MAX; k++)
		n += p->rz.req[k].used;
	return n;
}
static void bs_tick(struct server_local* s) {
	tick(s);
	const unsigned a = bs_used(&s->player);
	const unsigned b = s->guest_actor ? bs_used(s->guest_actor) : 0;
	if(a > bs_req_max_each)
		bs_req_max_each = a;
	if(b > bs_req_max_each)
		bs_req_max_each = b;
	if(a + b > bs_req_max_total)
		bs_req_max_total = a + b;
}
static int bs_nb(struct server_world* w, int cx, int cz) {
	int n = 0;
	for(int dz = -1; dz <= 1; dz++)
		for(int dx = -1; dx <= 1; dx++)
			n += server_world_chunk_resident(w, cx + dx, cz + dz);
	return n;
}
static bool bs_need(const struct server_player* p, int cx, int cz,
	const struct server_world* w) {
	for(unsigned k = 0; k < RZ_REQ_MAX; k++)
		if(p->rz.req[k].used && p->rz.req[k].cx == cx
		   && p->rz.req[k].cz == cz && p->rz.req[k].prio == RZ_PRIO_NEED
		   && p->rz.req[k].world_index == w->world_index
		   && p->rz.req[k].world_generation == w->generation)
			return true;
	return false;
}
static enum server_transfer_state bs_drive(struct server_local* s,
	uint32_t id) {
	enum server_transfer_state st = server_transfer_state_of(s, id);
	for(unsigned i = 0; i < 4000; i++) {
		st = server_transfer_state_of(s, id);
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK)
			break;
		tick(s);
	}
	return st;
}
static bool bs_other_moves(struct server_local* s, const struct owner* oo) {
	struct server_player* q = oo->body;
	const double x0 = q->x;
	sample(s, oo, q->x + 0.25, q->y, q->z, q->correction_seq);
	return fabs(q->x - (x0 + 0.25)) < 1e-9;
}
static void bs_leg(struct server_local* s, const struct owner* o,
	const struct owner* oo) {
	char label[260];
	struct server_player* p = o->body;
	struct server_world* w = server_local_world_of(s, p);
	assert(w);
	const unsigned slot = w->world_index;
	char tag[48];
	snprintf(tag, sizeof tag, "bounded_slots %s/slot%u", o->name, slot);
	const double y = p->y;
	const int kc = (int)WCOORD_CHUNK_OFFSET((w_coord_t)floor(p->x));
	const int kz = (int)WCOORD_CHUNK_OFFSET((w_coord_t)floor(p->z));
	const double zc = kz * 16 + 8.5;
	const double edge = kc * 16 + 15.5;
	printf("     [%s: world dim %d gen %u, body chunk (%d,%d); a2w stream_on "
		"%d reset_pending %d slot_state %d frozen %d]\n", tag,
		(int)w->dimension, (unsigned)w->generation, kc, kz,
		(int)s->a2w_stream_on[slot], (int)s->a2w_reset_pending[slot],
		(int)s->world_slot_state[slot],
		(int)server_transfer_actor_frozen(s, p));
	check(server_world_chunk_resident(w, kc, kz)
		&& server_world_chunk_resident(w, kc + 1, kz),
		"bounded_slots: the body's chunk and its +x neighbour are resident "
		"(served by the world's own loader)");
	/* walk to the chunk edge on admitted samples */
	reset_body(p, edge - 1.0, y, zc);
	sample(s, o, edge - 1.0, y, zc, p->correction_seq);
	sample(s, o, edge, y, zc, p->correction_seq);
	snprintf(label, sizeof label, "%s: the body walked to the chunk edge on "
		"admitted samples", tag);
	check(p->x == edge, label);
	bs_tick(s);

	/* ---- delayed ------------------------------------------------------- */
	server_world_test_fail_gen_attempts(100000);
	evict(w, kc + 1, kz);
	const unsigned load0 = p->rz.loaded, pick0 = p->rz.picked;
	sample(s, o, edge + 0.6, y, zc, p->correction_seq);
	snprintf(label, sizeof label, "%s delayed: a move into the missing column "
		"is refused and its column is a NEED request of this body", tag);
	check(p->x == edge && bs_need(p, kc + 1, kz, w), label);
	bool other_ok = true;
	for(int i = 0; i < 10; i++) {
		bs_tick(s);
		sample(s, o, edge + 0.6, y, zc, p->correction_seq);
		if(i == 4)
			other_ok &= bs_other_moves(s, oo);
	}
	snprintf(label, sizeof label, "%s delayed: still refused while loads "
		"fail (picked +%u, loaded +%u); the %s keeps moving", tag,
		p->rz.picked - pick0, p->rz.loaded - load0, oo->name);
	check(p->x == edge && p->rz.loaded == load0 && other_ok, label);
	server_world_test_fail_gen_attempts(0);
	unsigned served = 0, drain_last = 0;
	/* the column's remaining backoff from its GENUINE failures in the fault
	 * window (those still back off, by design) */
	const int backoff0 = server_local_test_near_backoff_left(s, kc + 1, kz);
	const bool probe = getenv("RZ_PROBE") != NULL;
	while(p->x == edge && served < 200) {
		if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_DRAINING)
			drain_last = served + 1;
		const unsigned pk = p->rz.picked;
		server_local_test_near_picks_reset();
		bs_tick(s);
		served++;
		if(probe) {
			int32_t px[32], pz[32];
			bool prz[32];
			unsigned n = server_local_test_near_picks(px, pz, prz, 32);
			unsigned nrz = 0;
			for(unsigned i = 0; i < n && i < 32; i++)
				nrz += prz[i];
			printf("     [PROBE %s t+%u need=%d backoff=%d picked+%u near_picks=%u "
				"rz_picks=%u lean=%d resident=%d defer=%u att=%u gen=%u fails=%u capw=%u phase=%d nb=%d]\n", tag, served,
				(int)bs_need(p, kc + 1, kz, w),
				server_local_test_near_backoff_left(s, kc + 1, kz),
				p->rz.picked - pk, n, nrz, (int)s->stream_lean,
				(int)server_world_chunk_resident(w, kc + 1, kz), (unsigned)w->gen_deferred_window, (unsigned)w->gen_attempts_window, (unsigned)w->gen_this_window, server_world_test_gen_failures_remaining(), (unsigned)s->transfer_column_cap_waits, (int)s->transfer_budget_phase, bs_nb(w, kc + 1, kz));
		}
		sample(s, o, edge + 0.6, y, zc, p->correction_seq);
	}
	bs_tick(s); /* the request is settled (SERVED) on the next update */
	printf("     [%s delayed: admitted %u ticks after the fault cleared; "
		"picked +%u loaded +%u latency_last %u service_delay_max %u]\n", tag,
		served, p->rz.picked - pick0, p->rz.loaded - load0,
		p->rz.latency_last, p->rz.service_delay_max);
	snprintf(label, sizeof label, "%s delayed: served by the production loader "
		"(an ACTUAL load) and the move admitted <= 40 ticks after the fault "
		"cleared", tag);
	check(fabs(p->x - (edge + 0.6)) < 1e-9 && served <= 40
		&& p->rz.loaded > load0
		&& server_world_chunk_resident(w, kc + 1, kz), label);
	/* 4b: a column-budget wait (the split budget DRAINING) must not put the
	 * NEED column into a failure backoff: it is served within 2 updates of
	 * the later of (the last DRAINING update, the backoff its genuine
	 * fault-window failures left). */
	const unsigned bound = (drain_last > (unsigned)(backoff0 > 0 ? backoff0 : 0)
		? drain_last : (unsigned)(backoff0 > 0 ? backoff0 : 0)) + 2;
	snprintf(label, sizeof label, "%s delayed: served t+%u <= 2 updates after "
		"max(last DRAINING update t+%u, genuine-failure backoff %d)", tag,
		served, drain_last, backoff0);
	check(served <= bound, label);
	/* back into chunk kc for the permanent leg */
	sample(s, o, edge, y, zc, p->correction_seq);
	bs_tick(s);

	/* ---- permanent ------------------------------------------------------ */
	server_world_test_fail_gen_attempts(100000);
	evict(w, kc + 1, kz);
	const unsigned fail0 = p->rz.recovery_failures, retry0 = p->rz.retries;
	const unsigned load1 = p->rz.loaded;
	p->rz.pin.valid = false;
	p->x = (kc + 1) * 16 + 8.5; /* the body is on the missing column */
	p->z = zc;
	const double hx = p->x;
	hud_reset();
	bs_tick(s);
	const unsigned entry = g_test_tick;
	snprintf(label, sizeof label, "%s permanent: recovery entered, the body "
		"owns a NEED request for its footprint column", tag);
	check(p->rz.recovering && !p->rz.failed && bs_need(p, kc + 1, kz, w),
		label);
	other_ok = true;
	while(!p->rz.failed && g_test_tick - entry < RZ_TEST_DEADLINE + 10) {
		bs_tick(s);
		if(g_test_tick - entry == 50)
			other_ok &= bs_other_moves(s, oo);
	}
	const unsigned term = g_test_tick - entry;
	bs_tick(s);
	bs_tick(s);
	printf("     [%s permanent: FAILED at +%u ticks (deadline %u)]\n", tag,
		term, RZ_TEST_DEADLINE);
	snprintf(label, sizeof label, "%s permanent: the deadline terminates the "
		"attempt (failed at +%u <= 200, one failure, toast, requests "
		"dropped); the %s kept moving", tag, term, oo->name);
	check(p->rz.failed && term <= RZ_TEST_DEADLINE
		&& p->rz.recovery_failures == fail0 + 1 && bs_used(p) == 0
		&& hud_count(RZ_TEST_FAIL_TEXT) == 1 && other_ok, label);
	/* the deliberate fresh retry on the live token */
	server_world_test_fail_gen_attempts(0);
	sample(s, o, hx + 1.0, y, zc, p->correction_seq);
	snprintf(label, sizeof label, "%s permanent: the owner's moved sample on "
		"the live token starts ONE retry (not applied)", tag);
	check(!p->rz.failed && p->rz.recovering && p->rz.retries == retry0 + 1
		&& p->x == hx, label);
	unsigned rec = 0;
	while(p->rz.recovering && rec < 220) {
		bs_tick(s);
		rec++;
	}
	bs_tick(s); /* the request is settled (SERVED) on the next update */
	printf("     [%s permanent: retry recovered in %u ticks; loaded +%u]\n",
		tag, rec, p->rz.loaded - load1);
	snprintf(label, sizeof label, "%s permanent: the retry recovers through "
		"the production loader (<= 200 ticks, an ACTUAL load, no replay)",
		tag);
	check(!p->rz.recovering && rec <= 200 && p->rz.loaded > load1
		&& p->x == hx, label);
	sample(s, o, hx + 0.5, y, zc, p->correction_seq);
	snprintf(label, sizeof label, "%s permanent: a FRESH sample moves the "
		"recovered body", tag);
	check(fabs(p->x - (hx + 0.5)) < 1e-9, label);
}
static void bs_bounds_check(const char* tag) {
	char label[200];
	snprintf(label, sizeof label, "%s: request bounds held (max each %u <= 6, "
		"max total %u <= 12)", tag, bs_req_max_each, bs_req_max_total);
	check(bs_req_max_each <= RZ_REQ_MAX && bs_req_max_total <= 12, label);
}
static bool bs_transfer(struct server_local* s, struct server_player* mover,
	enum world_dim dim, const char* what) {
	uint32_t id = 0;
	bool was = s->paused;
	s->paused = false;
	enum server_transfer_rc rc = server_transfer_request(s, mover, dim, &id);
	enum server_transfer_state st = rc == SERVER_TRANSFER_OK
		? bs_drive(s, id) : SERVER_TRANSFER_ROLLED_BACK;
	s->paused = was;
	struct server_world* w = server_local_world_of(s, mover);
	char label[200];
	snprintf(label, sizeof label, "bounded_slots: %s by a REAL A2 transfer "
		"(rc %d, state %d, now slot %d dim %d)", what, (int)rc, (int)st,
		w ? (int)w->world_index : -1, w ? (int)w->dimension : -1);
	const bool ok = rc == SERVER_TRANSFER_OK && st == SERVER_TRANSFER_COMMITTED
		&& w && w->dimension == dim;
	check(ok, label);
	/* the arriving pane loads and acknowledges its input handoff (the
	 * production loading screen + readiness report), which ends the
	 * post-commit presentation hold */
	unsigned acked = 0;
	while(ok && server_transfer_actor_frozen(s, mover) && acked < 600) {
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		pump_handshake(s);
		tick(s);
		acked++;
	}
	snprintf(label, sizeof label, "bounded_slots: %s -- the arriving pane "
		"acknowledged its load (%u ticks), the body is live", what, acked);
	check(!ok || !server_transfer_actor_frozen(s, mover), label);
	/* settle: landing residency + the A2-W stream around the arrival */
	for(int i = 0; ok && i < 60; i++)
		tick(s);
	return ok && !server_transfer_actor_frozen(s, mover);
}
/* Ordinary owner RPCs, including reversals like the pane patrol. Direct
 * fixture placement establishes the start only and earns no movement. */
static void ordinary_walk_proof(struct server_local* s, const struct owner* o) {
	struct server_player* p = o->body;
	const unsigned a = o->guest ? 1 : 0;
	const double x = floor(p->x / 16.0) * 16.0 + 4.5;
	const double z = floor(p->z / 16.0) * 16.0 + 4.5;
	reset_body(p, x, p->y, z);
	tick(s);
	uint64_t before = s->rz_sess[a].moved_dm;
	for(unsigned i = 0; i < 200; i++) {
		double nx = x + ((i / 4) % 2 ? 3 - (i % 4) : 1 + (i % 4)) * 0.25;
		sample(s, o, nx, p->y, z, p->correction_seq);
		check(p->x == nx, "ordinary walk: real owner handler admitted input");
		tick(s);
	}
	check(s->rz_sess[a].moved_dm - before >= 320,
		"ordinary walk: same actor/world leg exceeds 320 admitted dm");
}
static void case_bounded_slots(void) {
	struct server_local* s = admitted_fixture();
	/* bind the session save root so slot 1 writes as a sibling of slot 0
	 * (as test_a2_transfer_transaction two_world_fixture does) */
	string_set_str(s->level_name, split_path);
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	reset_body(g, 4.5, eye_y, 4.5);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &go, 4.5, eye_y, 4.5, g->correction_seq);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	/* No settle: the legs start 1 tick after admission, inside the split
	 * join burst. Round 4 needed a 200-tick settle because the slot-0 near
	 * scan backed a partially-generated NEED column off for 20 ticks after
	 * every partial cascade step (51 ticks to serve; 4b fix in
	 * server_local.c). RZ_SETTLE=<n> re-adds a settle for comparison. */
	const int settle = getenv("RZ_SETTLE") ? atoi(getenv("RZ_SETTLE")) : 0;
	for(int i = 0; i < settle; i++)
		tick(s);
	ordinary_walk_proof(s, &ho);
	ordinary_walk_proof(s, &go);
	reset_body(h, 1.5, eye_y, 0.5);
	reset_body(g, 4.5, eye_y, 4.5);
	/* host/slot 0, guest/slot 0 */
	bs_req_max_each = bs_req_max_total = 0;
	bs_leg(s, &ho, &go);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	reset_body(g, 4.5, eye_y, -12.5);
	sample(s, &go, 4.5, eye_y, -12.5, g->correction_seq);
	tick(s);
	bs_leg(s, &go, &ho);
	bs_bounds_check("bounded_slots slot0");
	server_world_test_fail_gen_attempts(0);
	/* host/slot 1: the host travels to the Nether, the guest stays */
	reset_body(g, 4.5, eye_y, 4.5);
	sample(s, &go, 4.5, eye_y, 4.5, g->correction_seq);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &ho, 1.5, eye_y, 0.5, h->correction_seq);
	tick(s);
	if(bs_transfer(s, h, WORLD_DIM_NETHER, "host -> Nether")) {
		check(server_local_world_of(s, h)->world_index == 1,
			"bounded_slots: the host is bound to slot 1");
		ordinary_walk_proof(s, &ho);
		bs_req_max_each = bs_req_max_total = 0;
		bs_leg(s, &ho, &go);
		bs_bounds_check("bounded_slots host/slot1");
		server_world_test_fail_gen_attempts(0);
		/* the guest follows into slot 1 (it joins the host's live Nether
		 * through its own real A2 transfer); the host is its partner there */
		const double gz = g->z;
		if(bs_transfer(s, g, WORLD_DIM_NETHER, "guest -> Nether")) {
			check(server_local_world_of(s, g)->world_index == 1,
				"bounded_slots: the guest is bound to slot 1");
			/* the partner stands well clear of the guest leg columns */
			(void)gz;
			reset_body(h, h->x, h->y, h->z - 40.0);
			tick(s);
			bs_req_max_each = bs_req_max_total = 0;
			bs_leg(s, &go, &ho);
			bs_bounds_check("bounded_slots guest/slot1");
		}
	}
	server_world_test_fail_gen_attempts(0);
	teardown(s);
}

/* ======================================================================== */
/* Round 4b item 6: missing-terrain LIFECYCLE commits. The ACTUAL respawn
 * lifecycles (death -> SRPC_RESPAWN -> the plan's loading barrier -> commit)
 * of the guest and the host, and a saved-guest re-Join, each with the arrival
 * column missing: first a DELAYED load (gameplay waits; the commit lands on a
 * resident footprint), then a PERMANENT failure (no gameplay on unknown
 * terrain: the body is held/uncommitted and the failure is visible, never
 * published as a live pose on unknown data). Retirement is asserted after
 * the real transition (old pin/requests/recovery gone), never by clearing
 * finished_loading by hand. */
static void lc_poison(struct server_local* s, struct server_player* p) {
	struct server_world* w = server_local_world_of(s, p);
	p->rz.req[0] = (struct rz_request) {.cx = 9, .cz = 9, .used = true,
		.prio = RZ_PRIO_NEED, .world_index = w ? w->world_index : 0,
		.world_generation = w ? w->generation : 0};
	p->rz.pin.valid = true;
	p->rz.pin.x = p->x;
	p->rz.pin.z = p->z;
}
static bool lc_retired(const struct server_player* p) {
	for(unsigned k = 0; k < RZ_REQ_MAX; k++)
		if(p->rz.req[k].used && p->rz.req[k].cx == 9 && p->rz.req[k].cz == 9)
			return false;
	return !p->rz.recovering && !p->rz.failed;
}
static bool lc_on_resident(struct server_local* s,
	const struct server_player* p) {
	struct server_world* w = server_local_world_of(s, p);
	return w && server_world_columns_resident(w, p->x - 0.3, p->z - 0.3,
		p->x + 0.3, p->z + 0.3);
}
static unsigned lc_load_failed_msgs;
static void lc_guest_respawn(struct server_local* s, bool permanent) {
	struct server_player* g = s->guest_actor;
	struct server_rpc_source src = {.slot = 1, .generation = s->guest_generation};
	char label[220];
	const char* leg = permanent ? "permanent" : "delayed";
	lc_poison(s, g);
	g->hurt_resist_time = 0;
	bool hit = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH, 0.0f,
		0.0f, &hit);
	check(g->health == 0, "lifecycle guest: the guest body died");
	struct server_rpc rq = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation}};
	assert(svin_rpc_send_from(src, &rq));
	drain_server(s);
	const int tcx = (int)floor(s->guest_respawn_x) >> 4;
	const int tcz = (int)floor(s->guest_respawn_z) >> 4;
	printf("     [lifecycle guest %s: phase %d target (%.1f,%.1f) column "
		"(%d,%d) resident %d]\n", leg, (int)s->guest_respawn_phase,
		s->guest_respawn_x, s->guest_respawn_z, tcx, tcz,
		(int)server_world_chunk_resident(&s->worlds[0], tcx, tcz));
	server_world_test_fail_gen_attempts(100000);
	if(server_world_chunk_resident(&s->worlds[0], tcx, tcz))
		evict(&s->worlds[0], tcx, tcz);
	/* the guest client reports its respawn readiness */
	for(int i = 0; i < 30; i++) {
		struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = s->guest_ticket,
				.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
				.generation = s->guest_generation,
				.respawn_epoch = s->guest_respawn_epoch}};
		if(s->guest_respawn_phase != GUEST_RESPAWN_IDLE)
			(void)svin_rpc_send_from(src, &ready);
		drain_server(s);
		tick(s);
	}
	printf("     [lifecycle guest %s: after 30 failing ticks phase %d health %d "
		"fin %d pose (%.1f,%.1f) on_resident %d recovering %d]\n", leg,
		(int)s->guest_respawn_phase, g->health, (int)g->finished_loading,
		g->x, g->z, (int)lc_on_resident(s, g), (int)g->rz.recovering);
	snprintf(label, sizeof label, "lifecycle guest %s: while the arrival "
		"column is missing the guest has no LIVE gameplay on unknown terrain "
		"(uncommitted, or held by its residency barrier)", leg);
	check(g->health <= 0 || !g->finished_loading || lc_on_resident(s, g)
		|| g->rz.recovering, label);
	if(!permanent) {
		server_world_test_fail_gen_attempts(0);
		for(int i = 0; i < 300 && !(g->health > 0 && g->finished_loading
			&& lc_on_resident(s, g) && !g->rz.recovering); i++) {
			struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
				.payload.local_guest = {.ticket = s->guest_ticket,
					.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
					.generation = s->guest_generation,
					.respawn_epoch = s->guest_respawn_epoch}};
			if(s->guest_respawn_phase != GUEST_RESPAWN_IDLE)
				(void)svin_rpc_send_from(src, &ready);
			drain_server(s);
			tick(s);
		}
		snprintf(label, sizeof label, "lifecycle guest delayed: the respawn "
			"completes onto a RESIDENT arrival footprint and the old body's "
			"residency state is retired by the real commit");
		check(g->health > 0 && g->finished_loading && lc_on_resident(s, g)
			&& lc_retired(g), label);
	} else {
		/* ASTRA-RESIDENCY-FINAL item 1: the LOADING phase is BOUNDED. Count
		 * from the update the plan entered LOADING (the drained request
		 * above); the client keeps reporting readiness as the real one would
		 * not -- the bound must hold even against an eager client. */
		const uint64_t epoch1 = s->guest_respawn_epoch;
		const unsigned retry0 = guest_respawn_retry_msgs;
		struct client_session* gc = &gstate.players[1];
		const uint32_t serial0 = gc->respawn_failure_serial;
		unsigned waited = 30, terminated_at = 0;
		bool live_on_unknown = false;
		for(; waited < SERVER_TRANSFER_CLIENT_WAIT_TICKS + 40; waited++) {
			if(s->guest_respawn_phase == GUEST_RESPAWN_IDLE) {
				terminated_at = waited;
				break;
			}
			live_on_unknown |= g->health > 0 && g->finished_loading
				&& !lc_on_resident(s, g);
			tick(s);
		}
		printf("     [lifecycle guest permanent: terminated after %u ticks "
			"(bound %u) phase %d health %d fin %d anchor %d target gen %u "
			"retry msgs %u epoch %llu client loading %d failure %d]\n",
			terminated_at, (unsigned)SERVER_TRANSFER_CLIENT_WAIT_TICKS,
			(int)s->guest_respawn_phase, g->health, (int)g->finished_loading,
			(int)s->guest_respawn_anchor,
			(unsigned)s->guest_respawn_target.generation,
			guest_respawn_retry_msgs - retry0,
			(unsigned long long)guest_respawn_retry_epoch,
			(int)gc->local_respawn_loading, (int)gc->respawn_failure);
		check(!live_on_unknown, "lifecycle guest permanent: the body is never "
			"live on unknown terrain while the plan waits");
		check(terminated_at > 0
			&& terminated_at <= SERVER_TRANSFER_CLIENT_WAIT_TICKS + 2,
			"lifecycle guest permanent: GUEST_RESPAWN_LOADING TERMINATES at its "
			"bound (SERVER_TRANSFER_CLIENT_WAIT_TICKS), not indefinitely");
		check(g->health <= 0 && g->finished_loading && g->has_death_pos,
			"lifecycle guest permanent: nothing was committed -- the corpse is "
			"restored to its pre-request state (dead, loaded, death position)");
		check(!s->guest_respawn_anchor && !s->guest_respawn_epoch
			&& !s->guest_respawn_generation && !s->guest_respawn_ready
			&& !s->guest_respawn_cap.ticket && !s->guest_respawn_cap.reserved
			&& !s->guest_respawn_cap.landing_armed
			&& !s->guest_respawn_target.generation
			&& !server_local_guest_respawn_target(s),
			"lifecycle guest permanent: ownership cleaned up -- no respawn "
			"anchor, epoch, target, capacity ticket or landing arm survives");
		check(guest_respawn_retry_msgs == retry0 + 1
			&& guest_respawn_retry_epoch == epoch1,
			"lifecycle guest permanent: exactly ONE retryable failure, tagged "
			"with the terminated plan's epoch");
		check(!gc->local_respawn_loading && gc->local_player
			&& !gc->local_reply_failed && gc->local_player->health <= 0
			&& gc->respawn_failure == RESPAWN_FAILURE_RETRY
			&& gc->respawn_failure_serial == serial0 + 1
			&& gc->respawn_failure_epoch == epoch1,
			"lifecycle guest permanent: the failure is VISIBLE -- the guest "
			"client leaves its loading screen and the death screen offers "
			"Retry Respawn");
		/* a stale READY for the terminated epoch cannot revive the plan */
		struct server_rpc stale = {.type = SRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = s->guest_ticket,
				.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
				.generation = s->guest_generation, .respawn_epoch = epoch1}};
		(void)svin_rpc_send_from(src, &stale);
		drain_server(s);
		for(int i = 0; i < 60; i++)
			tick(s);
		check(s->guest_respawn_phase == GUEST_RESPAWN_IDLE && g->health <= 0
			&& guest_respawn_retry_msgs == retry0 + 1,
			"lifecycle guest permanent: a stale READY of the terminated epoch "
			"revives nothing and no second failure is authored (no re-arm)");
		/* the terrain returns; the owner presses Retry Respawn (the death
		 * screen's own SRPC_RESPAWN, epoch 0) -- a FRESH plan */
		server_world_test_fail_gen_attempts(0);
		(void)tick_until_resident(s, &s->worlds[0], tcx, tcz, 400);
		assert(svin_rpc_send_from(src, &rq));
		drain_server(s);
		tick(s); /* the client consumes the LOADING reply */
		const uint64_t epoch2 = s->guest_respawn_epoch;
		printf("     [lifecycle guest permanent retry: phase %d epoch %llu "
			"client loading %d client epoch %llu retry msgs %u]\n",
			(int)s->guest_respawn_phase, (unsigned long long)epoch2,
			(int)gc->local_respawn_loading,
			(unsigned long long)gc->local_respawn_epoch,
			guest_respawn_retry_msgs - retry0);
		check(s->guest_respawn_phase == GUEST_RESPAWN_LOADING
			&& epoch2 > epoch1 && gc->local_respawn_loading
			&& gc->local_respawn_epoch == epoch2,
			"lifecycle guest permanent: the deliberate retry starts ONE fresh "
			"plan (new epoch) and the client re-enters its loading screen");
		for(int i = 0; i < 400 && !(g->health > 0 && g->finished_loading
			&& s->guest_respawn_phase == GUEST_RESPAWN_IDLE); i++) {
			struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
				.payload.local_guest = {.ticket = s->guest_ticket,
					.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
					.generation = s->guest_generation,
					.respawn_epoch = s->guest_respawn_epoch}};
			if(s->guest_respawn_phase == GUEST_RESPAWN_LOADING)
				(void)svin_rpc_send_from(src, &ready);
			drain_server(s);
			tick(s);
		}
		check(g->health > 0 && g->finished_loading && lc_on_resident(s, g)
			&& lc_retired(g) && s->guest_respawn_phase == GUEST_RESPAWN_IDLE
			&& guest_respawn_retry_msgs == retry0 + 1,
			"lifecycle guest permanent: the deliberate retry SUCCEEDS onto a "
			"resident footprint, old residency state retired by the commit");
	}
}
/* the host client's respawn confirmation (the plan's epoch), as the death
 * screen sends it once its loading screen is up */
static void lc_host_ready(struct server_local* s) {
	if(s->host_respawn_phase != HOST_RESPAWN_PHASE_LOADING
	   || s->host_respawn_ready)
		return;
	struct server_rpc ok = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 0,
			.respawn_epoch = s->host_respawn_epoch}};
	(void)svin_rpc_send(&ok);
}
/* the host's own respawn: death -> SRPC_RESPAWN -> HOST_RESPAWN loading ->
 * the client's input handoff (production loading screen + readiness) */
static void lc_host_respawn(struct server_local* s, bool permanent) {
	struct server_player* h = &s->player;
	char label[220];
	const char* leg = permanent ? "permanent" : "delayed";
	const int sx = 6, sz = 0; /* respawn column (6,0) */
	s->player.spawn_x = sx * 16 + 8;
	s->player.spawn_y = 4;
	s->player.spawn_z = sz * 16 + 8;
	reset_body(h, 1.5, s->player.y, 0.5);
	sample(s, &(struct owner) {.name = "host", .guest = false, .body = h},
		1.5, h->y, 0.5, h->correction_seq);
	tick(s);
	lc_poison(s, h);
	/* Beta Minecraft.respawn constructs a new player; the split host's
	 * commit must retire the dead body's terminal latch too (R7 keeps it
	 * latched during passive terrain arrival). Seed it explicitly so this
	 * case does not depend on the preceding guest-respawn fault window. */
	h->rz.failed = true;
	server_world_test_fail_gen_attempts(100000);
	if(server_world_chunk_resident(&s->worlds[0], sx, sz))
		evict(&s->worlds[0], sx, sz);
	if(permanent) {
		/* The respawn plan loads its target column directly (outside the
		 * budgeted cascade the generation-failure injection covers), so the
		 * permanent leg makes the column unavailable to EVERY loader with
		 * the production residency fault hook (item 12). */
		const int32_t fx[1] = {sx}, fz[1] = {sz};
		server_world_rz_fault_set(s->worlds[0].world_index,
			s->worlds[0].generation, fx, fz, 1);
	}
	assert(server_local_set_player_health(s, 0));
	struct server_rpc rq = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 0}};
	assert(svin_rpc_send(&rq));
	drain_server(s);
	for(int i = 0; i < 40; i++) {
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		lc_host_ready(s);
		drain_server(s);
		tick(s);
	}
	printf("     [lifecycle host %s: after 40 failing ticks phase %d health %d "
		"fin %d pose (%.1f,%.1f) on_resident %d recovering %d]\n", leg,
		(int)s->host_respawn_phase, h->health, (int)h->finished_loading,
		h->x, h->z, (int)lc_on_resident(s, h), (int)h->rz.recovering);
	snprintf(label, sizeof label, "lifecycle host %s: while the respawn "
		"column is missing the host has no LIVE gameplay on unknown terrain",
		leg);
	check(h->health <= 0 || !h->finished_loading || lc_on_resident(s, h)
		|| h->rz.recovering, label);
	if(permanent)
		for(int i = 0; i < 250; i++) {
			screen_load_world.update(&screen_load_world, 30.0f);
			clin_update_input_readiness();
			drain_server(s);
			tick(s);
		}
	snprintf(label, sizeof label, "lifecycle host %s: still nothing live on "
		"unknown terrain after %s", leg, permanent ? "290 failing ticks"
		: "the failing window");
	check(h->health <= 0 || !h->finished_loading || lc_on_resident(s, h)
		|| h->rz.recovering, label);
	if(permanent) {
		printf("     [lifecycle host permanent: after 290 ticks phase %d health "
			"%d fin %d recovering %d failed %d blocked loads %u]\n",
			(int)s->host_respawn_phase, h->health, (int)h->finished_loading,
			(int)h->rz.recovering, (int)h->rz.failed,
			server_world_rz_fault_blocked());
		check(s->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE
			&& h->health <= 0 && server_world_rz_fault_blocked() > 0,
			"lifecycle host permanent: the plan could not load its target and "
			"returned the existing retryable failure (plan dropped, the body "
			"stays dead on its death screen -- nothing committed)");
		server_world_rz_fault_clear();
	}
	server_world_test_fail_gen_attempts(0);
	for(int i = 0; i < 400 && !(h->health > 0 && h->finished_loading
		&& lc_on_resident(s, h) && !h->rz.recovering
		&& s->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE); i++) {
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		lc_host_ready(s);
		if(s->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE && h->health <= 0) {
			/* the player presses Respawn again (the retry) */
			struct server_rpc again = {.type = SRPC_RESPAWN,
				.payload.respawn = {.owner = 0}};
			(void)svin_rpc_send(&again);
		}
		if(h->rz.failed) /* the owner's deliberate retry */
			sample(s, &(struct owner) {.name = "host", .guest = false,
				.body = h}, h->x + 0.5, h->y, h->z, h->correction_seq);
		drain_server(s);
		tick(s);
	}
	snprintf(label, sizeof label, "lifecycle host %s: once the column loads "
		"the respawn completes onto a RESIDENT footprint, old residency "
		"state retired by the real transition", leg);
	check(h->health > 0 && h->finished_loading && lc_on_resident(s, h)
		&& lc_retired(h), label);
}
/* a guest Leave + re-Join with failing loads. In this fixture the Join's
 * destination is the host's (resident) pose -- guest_create places the body
 * there and no saved record is restored -- so this leg witnesses the
 * retirement of the departed body's residency state across the real
 * Leave/Join and no live gameplay on unknown terrain; the missing SAVED-pose
 * column on Join is lifecycle_saved_join (Round 5, real Leave checkpoint). */
static void lc_guest_rejoin(struct server_local* s, bool permanent) {
	char label[220];
	const char* leg = permanent ? "permanent" : "delayed";
	struct server_player* g = s->guest_actor;
	const struct owner go = {.name = "guest", .guest = true, .body = g};
	sample(s, &go, 4.5, g->y, 4.5, g->correction_seq);
	tick(s);
	lc_poison(s, g);
	server_world_test_fail_gen_attempts(100000);
	guest_release(s);
	evict(&s->worlds[0], 0, 0); /* the saved guest pose's column */
	s->paused = false;
	guest_attach(s);
	struct client_session* gc = &gstate.players[1];
	(void)client_session_request_local_gameplay(gc);
	for(int i = 0; i < 40; i++) {
		pump_handshake(s);
		tick(s);
	}
	g = s->guest_actor;
	printf("     [lifecycle rejoin %s: guest %p gameplay %d fin %d pose "
		"(%.1f,%.1f) on_resident %d recovering %d]\n", leg, (void*)g,
		(int)s->guest_gameplay, g ? (int)g->finished_loading : -1,
		g ? g->x : 0.0, g ? g->z : 0.0, g ? (int)lc_on_resident(s, g) : -1,
		g ? (int)g->rz.recovering : -1);
	snprintf(label, sizeof label, "lifecycle rejoin %s: the re-joined body "
		"has no live gameplay on unknown terrain and carries no residency "
		"state of the departed body", leg);
	check(g && (!s->guest_gameplay || !g->finished_loading
		|| lc_on_resident(s, g) || g->rz.recovering) && lc_retired(g), label);
	if(permanent)
		for(int i = 0; i < 250; i++) {
			pump_handshake(s);
			tick(s);
		}
	server_world_test_fail_gen_attempts(0);
	for(int i = 0; i < 400 && g && !(s->guest_gameplay && g->finished_loading
		&& lc_on_resident(s, g) && !g->rz.recovering); i++) {
		pump_handshake(s);
		if(g->rz.failed)
			sample(s, &(struct owner) {.name = "guest", .guest = true,
				.body = g}, g->x + 0.5, g->y, g->z, g->correction_seq);
		tick(s);
	}
	snprintf(label, sizeof label, "lifecycle rejoin %s: once the column loads "
		"the guest is live on a RESIDENT footprint", leg);
	check(g && s->guest_gameplay && g->finished_loading
		&& lc_on_resident(s, g) && !g->rz.recovering, label);
}
static void case_lifecycle_missing(void) {
	struct server_local* s = admitted_fixture();
	const double eye_y = s->player.y;
	reset_body(&s->player, 1.5, eye_y, 0.5);
	reset_body(s->guest_actor, 4.5, eye_y, 4.5);
	tick(s);
	hud_reset();
	lc_guest_respawn(s, false);
	hud_reset();
	lc_guest_respawn(s, true);
	reset_body(s->guest_actor, 4.5, eye_y, 4.5);
	tick(s);
	lc_host_respawn(s, false);
	lc_host_respawn(s, true);
	lc_guest_rejoin(s, false);
	lc_guest_rejoin(s, true);
	(void)lc_load_failed_msgs;
	server_world_test_fail_gen_attempts(0);
	teardown(s);
}

/* ======================================================================== */
/* Round 5 (ASTRA-RESIDENCY-FINAL item 3): the vehicle witness with the two
 * actors in DIFFERENT worlds, the ownership swapped between the cases.
 *
 *   vehicles_guest_away -- the guest is in the Nether (slot 1, a REAL A2
 *       transfer); the HOST rides a real cart and a real boat in the
 *       Overworld (slot 0) at the chunk 0/1 seam of ITS world.
 *   vehicles_host_away  -- the host is in the Nether (slot 1); the GUEST rides
 *       a real boat in the Overworld (slot 0), whose simulation centre then
 *       follows the guest (the host is not in slot 0).
 *
 * Vehicles live only in slot 0 in production (server_local_spawn_minecart /
 * _boat and the item placements bind &s->worlds[0] == worlds[0]), so the rider is
 * always in slot 0 and the partner owns the other world; every world below is
 * the RIDER's own (server_local_world_of), asserted, never a hard-coded slot.
 * The cart PHYSICALLY moves: E-W rails x 10..14 between two stone bumpers
 * (x 9 and 15), driven by real cumulative control tuples -- the resident
 * control and the fresh post-recovery moves assert x changes. Bypass
 * sensitivity: every held tick asserts the rider's committed footprint and
 * the vehicle box stay off the missing column, and that every refused seam
 * sample is counted as a refusal (a gate bypass fails these). */
static uint32_t vw_seq_of(const struct entity* e) {
	return e->data.minecart.control_seq;
}
/* one real control tuple adding `pos` forward / `neg` backward steps */
static void vw_throttle(struct server_local* s, struct entity* e, uint32_t pos,
	uint32_t neg) {
	struct server_rpc c = {.type = SRPC_MINECART_CONTROL,
		.payload.minecart_control = {.entity_id = e->id,
			.ride_epoch = e->data.minecart.ride_epoch,
			.input_seq = vw_seq_of(e) + pos + neg,
			.positive_steps = e->data.minecart.control_positive_steps + pos,
			.negative_steps = e->data.minecart.control_negative_steps + neg}};
	/* queued, NOT drained here: production drains rider input inside the
	 * update that runs the entity pass (an input drained between updates is
	 * discarded at the next update boundary, server_local.c "Rider input is
	 * valid only for the server update that drained it") */
	bool was = s->paused;
	s->paused = false;
	assert(svin_rpc_send(&c));
	tick(s);
	s->paused = was;
}
/* the VEHICLE box stays off the missing column (the held rider's own pose
 * is checked frozen: its pre-eviction footprint is what put it on hold) */
static bool vw_off_missing(const struct entity* e, float half) {
	return e->pos[0] + half <= 16.0F;
}
/* let the vehicle come to rest, so a later displacement is attributable */
static void vw_rest(struct server_local* s, const struct entity* e, int kind) {
	for(int i = 0; i < 400; i++) {
		const bool moving = kind == VH_CART ? e->data.minecart.speed != 0.0f
			: fabsf(e->vel[0]) > 0.004F || fabsf(e->vel[2]) > 0.004F;
		if(!moving)
			break;
		tick(s);
	}
}
static void vw_leg(struct server_local* s, const struct owner* o,
	const struct owner* partner, int kind, double lz) {
	char tag[72], label[280];
	struct server_player* p = o->body;
	struct server_player* q = partner->body;
	struct server_world* w = server_local_world_of(s, p);
	struct server_world* wq = server_local_world_of(s, q);
	const double eye_y = s->player.y;
	snprintf(tag, sizeof tag, "vehicles_worlds %s/%s", o->name,
		kind == VH_CART ? "cart" : "boat");
	snprintf(label, sizeof label, "%s: the rider owns slot %d (dim %d), the "
		"partner %s owns slot %d (dim %d) -- separate worlds", tag,
		w ? (int)w->world_index : -1, w ? (int)w->dimension : -99,
		partner->name, wq ? (int)wq->world_index : -1,
		wq ? (int)wq->dimension : -99);
	check(w && wq && w != wq && w == &s->worlds[0], label);
	if(!w || !wq || w == wq)
		return;
	const float half = kind == VH_CART ? 0.49F : 0.75F;
	server_world_test_fail_gen_attempts(0);
	(void)tick_until_resident(s, w, 1, 0, 600);
	for(int x = 8; x <= 24; x++) {
		if(kind == VH_CART) {
			put(w, x, 3, (int)floor(lz), BLOCK_STONE);
			const bool rail = x >= 10 && x <= 14;
			const bool bump = x == 9 || x == 15;
			if(rail) {
				put(w, x, 4, (int)floor(lz), BLOCK_RAIL);
				struct block_data b = {.type = BLOCK_RAIL, .metadata = 1};
				(void)server_world_set_block(w, x, 4, (int)floor(lz), b);
			} else
				put(w, x, 4, (int)floor(lz), bump ? BLOCK_STONE : BLOCK_AIR);
		} else
			for(int dz = -1; dz <= 1; dz++)
				put(w, x, 3, (int)floor(lz) + dz, BLOCK_WATER_STILL);
		clin_update_drain();
	}
	const float vx0 = kind == VH_CART ? 12.5F : 15.0F;
	reset_body(p, vx0, eye_y, lz);
	vh_sample(s, o, vx0, lz, p->correction_seq, 0.0F, 0.0F);
	tick(s);
	struct entity* e = kind == VH_CART
		? server_local_spawn_minecart((vec3) {vx0, 4.0F, (float)lz}, s)
		: server_local_spawn_boat((vec3) {vx0, 4.0F, (float)lz}, s);
	assert(e);
	clin_update_drain();
	const uint32_t id = e->id;
	vh_mount(s, o, e, kind, tag);

	/* ---- fully RESIDENT control: the vehicle physically moves ----------- */
	vec3 v0;
	glm_vec3_copy(e->pos, v0);
	if(kind == VH_CART) {
		vw_throttle(s, e, 4, 0);
		for(int i = 0; i < 60 && fabsf(e->pos[0] - v0[0]) < 1.5F; i++)
			tick(s);
		for(int i = 0; i < 60 && e->data.minecart.speed != 0.0f; i++)
			tick(s);
	} else {
		vh_steer(s, o, e, kind, 15.0, lz);
		for(int i = 0; i < 4; i++)
			tick(s);
	}
	printf("     [%s control: vehicle x %.3f -> %.3f (speed %.4f)]\n", tag,
		v0[0], e->pos[0], kind == VH_CART ? e->data.minecart.speed : 0.0F);
	snprintf(label, sizeof label, "%s control: the vehicle PHYSICALLY moves "
		"while resident (x %.3f -> %.3f)", tag, v0[0], e->pos[0]);
	check(fabsf(e->pos[0] - v0[0]) > 0.2F, label);
	if(kind == VH_BOAT) {
		/* steer the boat back to x ~15 and brake it (a following client) */
		for(int i = 0; i < 200; i++) {
			const float want = e->pos[0] > 15.0F ? -0.25F
				: (e->vel[0] < -0.004F ? 0.25F : 0.0F);
			if(want == 0.0F && fabsf(e->vel[0]) < 0.004F)
				break;
			vh_sample(s, o, e->pos[0], lz, p->correction_seq, want, 0.0F);
			tick(s);
		}
	}
	/* the rider sits at the seam: its footprint spans chunks 0 and 1 */
	vh_sample(s, o, 15.8, lz, p->correction_seq, 0.0F, 0.0F);
	for(int i = 0; i < 4; i++)
		tick(s);
	snprintf(label, sizeof label, "%s: rider at the seam (x 15.8), vehicle in "
		"chunk 0 (x %.3f)", tag, e->pos[0]);
	check(p->x == 15.8 && e->pos[0] + half <= 16.0F && e->pos[0] > 8.0F,
		label);

	for(int leg = 0; leg < 2; leg++) {
		const bool permanent = leg == 1;
		const char* lname = permanent ? "permanent" : "delayed";
		vw_rest(s, e, kind);
		server_world_test_fail_gen_attempts(100000);
		evict(w, 1, 0);
		check(!server_world_chunk_resident(w, 1, 0) && wq != w,
			"vehicles_worlds: chunk 1 of the RIDER's world is missing");
		const unsigned rej0 = p->rz.rejects, fail0 = p->rz.recovery_failures;
		const unsigned retry0 = p->rz.retries;
		const unsigned c0 = corrections(o);
		hud_reset();
		tick(s);
		const unsigned entry = g_test_tick;
		snprintf(label, sizeof label, "%s %s: the rider whose footprint lost "
			"chunk 1 is HELD, still seated, steering and throttle discarded",
			tag, lname);
		check(p->rz.recovering && vh_rides(e, p) && p->rider_vel_x == 0.0F
			&& (kind != VH_CART || e->data.minecart.pending_throttle == 0),
			label);
		glm_vec3_copy(e->pos, v0);
		const double px0 = p->x;
		const float odo0 = p->distance_walked;
		/* refusal at the missing SEAT/SWEPT footprint: two moves into it */
		vh_steer(s, o, e, kind, 16.2, lz);
		vh_steer(s, o, e, kind, 16.4, lz);
		if(kind == VH_CART)
			vw_throttle(s, e, 3, 0);
		snprintf(label, sizeof label, "%s %s: both seam samples REFUSED and "
			"counted (rejects +%u, corrections +%u), pose/odometer/steering/"
			"throttle unchanged", tag, lname, p->rz.rejects - rej0,
			corrections(o) - c0);
		check(p->x == px0 && p->distance_walked == odo0
			&& p->rz.rejects >= rej0 + 2 && corrections(o) > c0
			&& p->rider_vel_x == 0.0F
			&& (kind != VH_CART || e->data.minecart.pending_throttle == 0),
			label);
		/* the partner, in ITS world, keeps progressing */
		const double qx0 = q->x;
		vh_sample(s, partner, q->x + 0.3, q->z, q->correction_seq, 0.0F,
			0.0F);
		snprintf(label, sizeof label, "%s %s: the partner %s keeps moving in "
			"its own world", tag, lname, partner->name);
		check(fabs(q->x - (qx0 + 0.3)) < 1e-9, label);
		/* bypass sentinel: every held tick keeps rider + vehicle off the
		 * missing column and the vehicle frozen */
		bool off = true, frozen = true;
		const unsigned hold = permanent ? RZ_TEST_DEADLINE + 10 : 10;
		for(unsigned i = 0; i < hold && !(permanent && p->rz.failed); i++) {
			if(kind == VH_CART && (i % 20) == 0)
				vw_throttle(s, e, 1, 0);
			vh_steer(s, o, e, kind, 16.3, lz);
			tick(s);
			off &= vw_off_missing(e, half);
			frozen &= vh_same_pos(e, v0) && p->x == px0;
		}
		snprintf(label, sizeof label, "%s %s: through the hold the committed "
			"rider footprint and the vehicle box never touch the missing "
			"column and nothing moves (rejects +%u)", tag, lname,
			p->rz.rejects - rej0);
		if(!(off && frozen))
			printf("     [%s %s dbg: off %d frozen %d p.x %.3f px0 %.3f e "
				"(%.4f,%.4f,%.4f) v0 (%.4f,%.4f,%.4f)]\n", tag, lname,
				(int)off, (int)frozen, p->x, px0, e->pos[0], e->pos[1],
				e->pos[2], v0[0], v0[1], v0[2]);
		check(off && frozen && vh_rides(e, p), label);
		if(permanent) {
			snprintf(label, sizeof label, "%s permanent: the attempt TERMINATES "
				"by +200 (at +%u), one failure, rider seated", tag,
				g_test_tick - entry);
			check(p->rz.failed && p->rz.recovery_failures == fail0 + 1
				&& g_test_tick - entry <= RZ_TEST_DEADLINE && vh_rides(e, p),
				label);
			if(kind == VH_CART) {
				struct server_rpc off_rq = hc_intent(s, id, false);
				host_rpc(s, &off_rq);
			} else
				vh_use(s, o, id, false);
			snprintf(label, sizeof label, "%s permanent: a dismount request "
				"from the FAILED hold changes nothing", tag);
			check(vh_rides(e, p), label);
			server_world_test_fail_gen_attempts(0);
			vh_steer(s, o, e, kind, 16.3, lz); /* the deliberate retry */
			snprintf(label, sizeof label, "%s permanent: the rider's deliberate "
				"move starts ONE retry, discarded", tag);
			check(p->rz.recovering && p->rz.retries == retry0 + 1
				&& p->x == px0 && p->rider_vel_x == 0.0F, label);
		} else
			server_world_test_fail_gen_attempts(0);
		unsigned waited = 0;
		while(p->rz.recovering && waited < 200) {
			tick(s);
			waited++;
		}
		tick(s);
		printf("     [%s %s: after recovery vehicle x %.4f (hold start %.4f) "
			"speed %.4f impulse %.3f rider %.3f]\n", tag, lname, e->pos[0],
			v0[0], kind == VH_CART ? e->data.minecart.speed : 0.0F,
			vh_impulse(e), p->x);
		snprintf(label, sizeof label, "%s %s: recovered through the production "
			"loader (%u ticks), no stale impulse, pose not replayed", tag,
			lname, waited);
		check(!p->rz.recovering && !p->rz.failed && waited <= 200
			&& p->x == px0 && p->rider_vel_x == 0.0F && vh_impulse(e) == 0.0F
			&& (kind == VH_CART ? vh_same_pos(e, v0)
				: fabsf(e->pos[0] - v0[0]) < 0.05F)
			&& vh_rides(e, p), label);
		/* FRESH movement after recovery: the vehicle moves again (away from
		 * the seam, so the next leg can evict chunk 1 with it in chunk 0) */
		glm_vec3_copy(e->pos, v0);
		if(kind == VH_CART) {
			vw_throttle(s, e, 0, 4);
			for(int i = 0; i < 60 && fabsf(e->pos[0] - v0[0]) < 1.5F; i++)
				tick(s);
			for(int i = 0; i < 60 && e->data.minecart.speed != 0.0f; i++)
				tick(s);
		} else {
			vh_steer_v(s, o, e, kind, 15.6, lz, -0.25F);
			for(int i = 0; i < 6; i++)
				tick(s);
		}
		printf("     [%s %s: fresh move vehicle x %.3f -> %.3f]\n", tag, lname,
			v0[0], e->pos[0]);
		snprintf(label, sizeof label, "%s %s: a FRESH move after loading "
			"PHYSICALLY moves the vehicle (x %.3f -> %.3f)", tag, lname, v0[0],
			e->pos[0]);
		check(fabsf(e->pos[0] - v0[0]) > 0.3F, label);
		if(kind == VH_BOAT)
			for(int i = 0; i < 200; i++) {
				const float want = e->pos[0] > 15.0F ? -0.25F
					: (e->vel[0] < -0.004F ? 0.25F : 0.0F);
				if(want == 0.0F && fabsf(e->vel[0]) < 0.004F)
					break;
				vh_sample(s, o, e->pos[0], lz, p->correction_seq, want, 0.0F);
				tick(s);
			}
		else if(!permanent) {
			/* drive the cart back to the forward bumper for the next leg */
			vw_throttle(s, e, 10, 0); /* into the bumper: rest at x 14.51 */
			for(int i = 0; i < 600 && e->data.minecart.speed != 0.0f; i++)
				tick(s);
		}
		vh_sample(s, o, 15.8, lz, p->correction_seq, 0.0F, 0.0F);
		for(int i = 0; i < 6; i++)
			tick(s);
		snprintf(label, sizeof label, "%s %s: rider back at the seam, vehicle "
			"in chunk 0 (x %.3f)", tag, lname, e->pos[0]);
		check(p->x == 15.8 && e->pos[0] + half <= 16.0F, label);
	}
	/* production dismount once released */
	if(kind == VH_CART) {
		struct server_rpc off_rq = hc_intent(s, id, false);
		host_rpc(s, &off_rq);
	} else if(!o->guest) {
		vh_use(s, o, id, false);
	} else {
		vh_use(s, o, id, false);
		server_local_set_boat_occupancy_for(s, e, p, false);
	}
	snprintf(label, sizeof label, "%s: dismounted once resident (occupancy and "
		"rider flag cleared together)", tag);
	check(!vh_rides(e, p) && !p->riding_boat && !p->riding_minecart, label);
	tick(s);
}
static struct server_local* vw_fixture(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	const double eye_y = s->player.y;
	reset_body(s->guest_actor, 4.5, eye_y, 4.5);
	reset_body(&s->player, 1.5, eye_y, 0.5);
	sample(s, &(struct owner) {.name = "guest", .guest = true,
		.body = s->guest_actor}, 4.5, eye_y, 4.5,
		s->guest_actor->correction_seq);
	sample(s, &(struct owner) {.name = "host", .guest = false,
		.body = &s->player}, 1.5, eye_y, 0.5, s->player.correction_seq);
	tick(s);
	return s;
}
/* the arrived body's first echo ends its post-commit presentation */
static void vw_settle(struct server_local* s, const struct owner* o) {
	struct server_player* p = o->body;
	for(int i = 0; i < 40 && (server_transfer_actor_inert(s, p)
		|| p->rz.recovering || !p->finished_loading
		|| (o->guest && s->a2w_guest_presentation)); i++) {
		sample(s, o, p->x, p->y, p->z, p->correction_seq);
		pump_handshake(s);
		tick(s);
	}
}
static void case_vehicles_guest_away(void) {
	struct server_local* s = vw_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	if(bs_transfer(s, g, WORLD_DIM_NETHER, "guest -> Nether")) {
		vw_settle(s, &go);
		vw_leg(s, &ho, &go, VH_CART, 0.5);
		vw_leg(s, &ho, &go, VH_BOAT, 3.5);
	}
	server_world_test_fail_gen_attempts(0);
	teardown(s);
}
static void case_vehicles_host_away(void) {
	struct server_local* s = vw_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct owner ho = {.name = "host", .guest = false, .body = h};
	struct owner go = {.name = "guest", .guest = true, .body = g};
	if(bs_transfer(s, h, WORLD_DIM_NETHER, "host -> Nether")) {
		vw_settle(s, &ho);
		/* the guest is the only slot-0 actor: walk it to the boat lane */
		reset_body(g, 14.5, s->player.y, 9.5);
		sample(s, &go, 14.5, g->y, 9.5, g->correction_seq);
		tick(s);
		vw_leg(s, &go, &ho, VH_BOAT, 9.5);
	}
	server_world_test_fail_gen_attempts(0);
	teardown(s);
}

/* ======================================================================== */
/* Round 5 (ASTRA-RESIDENCY-FINAL item 2): the missing-arrival witnesses the
 * lifecycle case could not construct.
 *
 * lifecycle_initial_load -- the ACTUAL initial host load (WORLD_RESET ->
 * near stream -> center gate -> CRPC_PLAYER_POS marker -> the client's input
 * handoff) with the spawn column unavailable to every loader (the production
 * residency fault hook, item 12). Delayed: no marker, no finished_loading,
 * the poisoned residency state retired by the loading barrier; the fault
 * clears and the same load completes onto a resident footprint and the body
 * moves. Permanent: the existing load watchdog (LOAD_WATCHDOG_TICKS, 6000)
 * publishes CRPC_LOAD_FAILED ONCE and keeps every barrier (the terminal,
 * visible failure of this session -- a retry is a fresh load, which is the
 * delayed leg). */
#define IL_LOAD_WATCHDOG_TICKS 6000u /* server_local.c LOAD_WATCHDOG_TICKS */
static void il_leg(bool permanent) {
	char label[220];
	const char* leg = permanent ? "permanent" : "delayed";
	struct server_local* s = split_fixture();
	s->paused = true;
	struct server_player* h = &s->player;
	lc_poison(s, h);
	const int32_t fx[1] = {0}, fz[1] = {0};
	server_world_rz_fault_set(s->worlds[0].world_index,
		s->worlds[0].generation, fx, fz, 1);
	const unsigned m0 = markers, lf0 = load_failed_msgs;
	const unsigned hold = permanent ? IL_LOAD_WATCHDOG_TICKS + 50 : 150;
	bool early = false, live_unknown = false;
	unsigned failed_at = 0;
	for(unsigned i = 0; i < hold; i++) {
		step(s);
		early |= markers != m0;
		live_unknown |= h->finished_loading && !lc_on_resident(s, h);
		if(!failed_at && load_failed_msgs != lf0)
			failed_at = i + 1;
	}
	printf("     [initial load %s: %u failing updates, blocked loads %u, "
		"marker %d, finished_loading %d, load_failed msgs %u at %u, "
		"terminal %d, center resident %d]\n", leg, hold,
		server_world_rz_fault_blocked(), (int)early, (int)h->finished_loading,
		load_failed_msgs - lf0, failed_at, (int)s->load_failure_terminal,
		(int)server_world_chunk_resident(&s->worlds[0], 0, 0));
	snprintf(label, sizeof label, "initial load %s: while the spawn column "
		"is unavailable (%u blocked loads) the load publishes no marker and "
		"the host never goes live on unknown terrain", leg,
		server_world_rz_fault_blocked());
	check(server_world_rz_fault_blocked() > 0 && !early && !live_unknown
		&& !h->finished_loading
		&& !server_world_chunk_resident(&s->worlds[0], 0, 0), label);
	snprintf(label, sizeof label, "initial load %s: the poisoned residency "
		"state was retired by the REAL loading barrier", leg);
	check(lc_retired(h) && !h->rz.pin.valid, label);
	if(!permanent) {
		check(load_failed_msgs == lf0 && !s->load_failure_terminal,
			"initial load delayed: no terminal failure inside the window");
		server_world_rz_fault_clear();
		stream_to_marker(s);
		acknowledge(s);
		check(h->finished_loading && lc_on_resident(s, h) && lc_retired(h)
			&& markers == m0 + 1,
			"initial load delayed: once the column loads, the SAME load "
			"completes (one marker) onto a RESIDENT footprint");
		s->thread_started = true;
		const struct owner ho = {.name = "host", .guest = false, .body = h};
		const double x0 = h->x;
		sample(s, &ho, x0 + 0.25, h->y, h->z, h->correction_seq);
		check(fabs(h->x - (x0 + 0.25)) < 1e-9,
			"initial load delayed: a fresh sample moves the loaded body");
	} else {
		check(load_failed_msgs == lf0 + 1 && s->load_failure_terminal
			&& failed_at >= IL_LOAD_WATCHDOG_TICKS - 5
			&& failed_at <= IL_LOAD_WATCHDOG_TICKS + 5,
			"initial load permanent: the load watchdog terminates at its "
			"bound with ONE visible CRPC_LOAD_FAILED");
		server_world_rz_fault_clear();
		for(int i = 0; i < 200; i++)
			step(s);
		check(markers == m0 && !h->finished_loading
			&& load_failed_msgs == lf0 + 1,
			"initial load permanent: the terminal failure keeps its barriers "
			"(no late marker, no second failure, nothing live)");
	}
	server_world_rz_fault_clear();
	split_destroy(s);
}
static void case_lifecycle_initial_load(void) {
	il_leg(false);
	il_leg(true);
}

/* lifecycle_saved_join -- a guest Join that RESTORES a saved pose whose
 * column is missing. Fixture support: the guest leaves through the REAL
 * client Leave (client_session_request_local_leave -> server_local_guest_leave
 * -> server_local_guest_checkpoint), which writes its record into level.data,
 * so the next Join's guest_create reads that record back (the old fixture
 * released without a checkpoint and every Join landed on the host's pose).
 * The saved column sits inside the host's near ring, so the production slot-0
 * near loader owns it. Delayed: gameplay is DENIED while the column is
 * unpublished (guest_terrain_ready), then granted at the saved pose on a
 * resident footprint. Permanent: never granted, the pane stays on its join
 * card, the guest can Leave (ownership released, record intact), and a
 * deliberate re-Join after the terrain returns succeeds. Retirement: the
 * departing body's residency state is poisoned; the re-joined body is a new
 * body that carries none of it. */
#define SJ_X 36.5 /* chunk (2,0) */
#define SJ_Z 4.5
static void sj_real_leave(struct server_local* s) {
	struct client_session* gc = &gstate.players[1];
	assert(client_session_request_local_leave(gc));
	for(int i = 0; i < 20 && (s->guest_actor
		|| gc->local_phase != CLIENT_GUEST_IDLE); i++) {
		pump_handshake(s);
		tick(s);
	}
}
static void sj_join(struct server_local* s) {
	s->paused = false;
	guest_attach(s);
}
static bool sj_request_gameplay(struct server_local* s, unsigned ticks) {
	struct client_session* gc = &gstate.players[1];
	for(unsigned i = 0; i < ticks && !gc->local_gameplay_ready; i++) {
		(void)client_session_request_local_gameplay(gc);
		pump_handshake(s);
		tick(s);
	}
	pump_handshake(s);
	return gc->local_gameplay_ready;
}
static void sj_leg(struct server_local* s, bool permanent) {
	char label[220];
	const char* leg = permanent ? "permanent" : "delayed";
	struct client_session* gc = &gstate.players[1];
	struct server_player* g = s->guest_actor;
	assert(g && s->guest_gameplay);
	reset_body(g, SJ_X, s->player.y, SJ_Z);
	sample(s, &(struct owner) {.name = "guest", .guest = true, .body = g},
		SJ_X, g->y, SJ_Z, g->correction_seq);
	tick(s);
	lc_poison(s, g);
	sj_real_leave(s);
	check(!s->guest_actor && !s->guest_owned
		&& gc->local_phase == CLIENT_GUEST_IDLE,
		"saved join: the guest left through the real client Leave");
	nbt_node* rec = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest");
	check(rec != NULL, "saved join: the Leave checkpointed a guest record");
	/* the saved column goes missing for every loader */
	if(server_world_chunk_resident(&s->worlds[0], 2, 0))
		evict(&s->worlds[0], 2, 0);
	const int32_t fx[1] = {2}, fz[1] = {0};
	server_world_rz_fault_set(s->worlds[0].world_index,
		s->worlds[0].generation, fx, fz, 1);
	sj_join(s);
	g = s->guest_actor;
	snprintf(label, sizeof label, "saved join %s: Join RESTORED the saved pose "
		"(%.1f,%.1f) into a NEW body with no residency state of the departed "
		"one", leg, g ? g->x : 0.0, g ? g->z : 0.0);
	check(g && g->x == SJ_X && g->z == SJ_Z && lc_retired(g)
		&& !g->rz.pin.valid && !g->finished_loading, label);
	const unsigned window = permanent ? 650 : 120;
	bool granted = false, live_unknown = false;
	for(unsigned i = 0; i < window && !granted; i++) {
		(void)client_session_request_local_gameplay(gc);
		pump_handshake(s);
		tick(s);
		granted = s->guest_gameplay || gc->local_gameplay_ready;
		live_unknown |= g->finished_loading && !lc_on_resident(s, g);
	}
	printf("     [saved join %s: %u failing ticks, blocked loads %u, "
		"gameplay %d client ready %d phase %d fin %d resident %d]\n", leg,
		window, server_world_rz_fault_blocked(), (int)s->guest_gameplay,
		(int)gc->local_gameplay_ready, (int)gc->local_phase,
		(int)g->finished_loading,
		(int)server_world_chunk_resident(&s->worlds[0], 2, 0));
	snprintf(label, sizeof label, "saved join %s: gameplay is DENIED while "
		"the saved column is unavailable (%u blocked loads) -- the body is "
		"never live on unknown terrain and the pane stays on its join card",
		leg, server_world_rz_fault_blocked());
	check(!granted && !live_unknown && server_world_rz_fault_blocked() > 0
		&& gc->local_phase == CLIENT_GUEST_ATTACHED
		&& !gc->local_gameplay_ready, label);
	if(permanent) {
		lc_poison(s, g);
		sj_real_leave(s);
		check(!s->guest_actor && !s->guest_owned
			&& gc->local_phase == CLIENT_GUEST_IDLE,
			"saved join permanent: the waiting guest can Leave (ownership "
			"released)");
		server_world_rz_fault_clear();
		sj_join(s);
		g = s->guest_actor;
		check(g && g->x == SJ_X && g->z == SJ_Z && lc_retired(g),
			"saved join permanent: the record survived the cancelled Join; "
			"the deliberate re-Join restores the saved pose again");
	}
	server_world_rz_fault_clear();
	const bool ok = sj_request_gameplay(s, 300);
	g = s->guest_actor;
	snprintf(label, sizeof label, "saved join %s: once the column loads the "
		"guest is admitted AT ITS SAVED POSE on a resident footprint", leg);
	check(ok && g && s->guest_gameplay && g->finished_loading
		&& g->x == SJ_X && g->z == SJ_Z && lc_on_resident(s, g)
		&& lc_retired(g), label);
	if(g) {
		const struct owner go = {.name = "guest", .guest = true, .body = g};
		sample(s, &go, SJ_X + 0.25, g->y, SJ_Z, g->correction_seq);
		check(fabs(g->x - (SJ_X + 0.25)) < 1e-9,
			"saved join: a fresh sample moves the admitted body");
	}
}
/* lifecycle_nether_respawn -- a SEPARATE-WORLD guest: the guest reaches the
 * Nether (slot 1) by a REAL A2 transfer while the host stays in the
 * Overworld (slot 0), dies there, and respawns at its legal Beta destination
 * -- the Overworld spawn (ServerConfigurationManager.recreatePlayerEntity ->
 * dimension 0) -- through the cross-world preflight (client capacity RESERVE
 * + landing arm, server_local_guest_respawn). The spawn column (3,0) lies
 * inside the host's near ring (radius 4): with the corpse's Nether ring (81
 * columns) and the host's Overworld ring (81) resident, a spawn OUTSIDE the
 * host ring cannot fit its landing under SERVER_TRANSFER_RESIDENT_COLUMN_CAP
 * (176) -- measured 175/176 resident, the landing never reports and the
 * PREPARED preflight times out into RETRY on every attempt (reported to the
 * lead as a capacity finding, not pinned here). Inside the ring the slot-0
 * near loader owns the column, so the missing-arrival fault is on the column
 * the host's own loader would serve.
 * Once the plan is LOADING the column goes missing for every loader:
 * delayed -> no commit while missing, then the body commits into the
 * OVERWORLD world on a resident footprint with its Nether-era residency
 * state retired; permanent -> the bounded LOADING terminates (item 1): the
 * corpse stays in the Nether, the capacity ticket / landing arm are
 * released, the pane gets its failure, and a deliberate retry succeeds. */
#define NR_CX 3
static void nr_ready(struct server_local* s) {
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket,
			.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
			.generation = s->guest_generation,
			.respawn_epoch = s->guest_respawn_epoch}};
	if(s->guest_respawn_phase == GUEST_RESPAWN_LOADING)
		(void)svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
			.generation = s->guest_generation}, &ready);
}
/* death-screen request, then the real preflight until the plan is LOADING */
static bool nr_request(struct server_local* s, unsigned* took) {
	struct server_rpc rq = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation}};
	assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
		.generation = s->guest_generation}, &rq));
	drain_server(s);
	unsigned i = 0;
	for(; i < 400 && s->guest_respawn_phase != GUEST_RESPAWN_LOADING; i++) {
		pump_handshake(s);
		tick(s);
	}
	*took = i;
	if(getenv("NR_DEBUG")) {
		struct client_session* gc = &gstate.players[1];
		printf("     [nr client: phase %d failed %d player %p loading %d "
			"epoch %llu completed %llu server epoch %llu]\n",
			(int)gc->local_phase, (int)gc->local_reply_failed,
			(void*)gc->local_player, (int)gc->local_respawn_loading,
			(unsigned long long)gc->local_respawn_epoch,
			(unsigned long long)gc->local_respawn_completed_epoch,
			(unsigned long long)s->guest_respawn_epoch);
		printf("     [nr dbg: landing state %d two_anchor %d anchor1 (%d,%d) "
			"dim %d; sent:", (int)clin_landing_state(),
			(int)server_local_two_anchor(s), (int)s->residency_anchors[1].x,
			(int)s->residency_anchors[1].z,
			(int)s->residency_anchors[1].dimension);
		for(int dz = -1; dz <= 1; dz++)
			for(int dx = -1; dx <= 1; dx++)
				printf(" %d", (int)server_world_is_chunk_sent(&s->worlds[0],
					NR_CX + dx, dz));
		printf("]\n");
		printf("       budget phase %d resident %u cap %u reservations %u "
			"cap_waits %u\n", (int)s->transfer_budget_phase,
			(unsigned)server_transfer_resident_columns(s),
			(unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
			(unsigned)s->transfer_column_reservation_n,
			(unsigned)s->transfer_column_cap_waits);
		printf("       resident slot0 %u slot1 %u\n",
			(unsigned)dict_server_chunks_size(s->worlds[0].chunks),
			(unsigned)dict_server_chunks_size(s->worlds[1].chunks));
		for(int dz = -4; dz <= 4; dz++) {
			printf("       z%+d:", dz);
			for(int x = -4; x <= 11; x++)
				printf("%c", server_world_is_chunk_sent(&s->worlds[0], x, dz)
					? 'S' : server_world_chunk_resident(&s->worlds[0], x, dz)
					? 'r' : '.');
			printf("\n");
		}
	}
	return s->guest_respawn_phase == GUEST_RESPAWN_LOADING;
}
static void nr_leg(struct server_local* s, bool permanent) {
	char label[240];
	const char* leg = permanent ? "permanent" : "delayed";
	struct server_player* g = s->guest_actor;
	struct client_session* gc = &gstate.players[1];
	struct server_world* nether = server_local_world_of(s, g);
	assert(nether && nether->dimension == WORLD_DIM_NETHER);
	const uint8_t nslot = nether->world_index;
	/* the arriving pane's first pose echo ends the post-commit presentation
	 * (what the real client does); the body is then live in the Nether */
	const struct owner go0 = {.name = "guest", .guest = true, .body = g};
	for(int i = 0; i < 40 && (server_transfer_actor_inert(s, g)
		|| g->rz.recovering || !g->finished_loading
		|| s->a2w_guest_presentation); i++) {
		sample(s, &go0, g->x, g->y, g->z, g->correction_seq);
		pump_handshake(s);
		tick(s);
	}
	printf("     [nether respawn %s: settled -- inert %d recovering %d fin %d "
		"presenting %d pose (%.2f,%.2f,%.2f)]\n", leg,
		(int)server_transfer_actor_inert(s, g), (int)g->rz.recovering,
		(int)g->finished_loading, (int)s->a2w_guest_presentation,
		g->x, g->y, g->z);
	lc_poison(s, g);
	g->hurt_resist_time = 0;
	bool hit = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH, 0.0f,
		0.0f, &hit);
	check(g->health == 0, "nether respawn: the guest died in the Nether");
	unsigned took = 0;
	const bool loading = nr_request(s, &took);
	struct server_world* target = server_local_guest_respawn_target(s);
	printf("     [nether respawn %s: LOADING %d after %u ticks; target slot %d "
		"dim %d (%.1f,%.1f); reserved %d ticket %u landing %d/%d]\n", leg,
		(int)loading, took, target ? (int)target->world_index : -1,
		target ? (int)target->dimension : -99, s->guest_respawn_x,
		s->guest_respawn_z, (int)s->guest_respawn_cap.reserved,
		(unsigned)s->guest_respawn_cap.ticket,
		(int)s->guest_respawn_cap.landing_armed,
		(int)s->guest_respawn_cap.landing_ready);
	check(loading && target == &s->worlds[0]
		&& target->dimension == WORLD_DIM_OVERWORLD
		&& s->guest_respawn_cap.reserved && s->guest_respawn_cap.ticket
		&& (int)floor(s->guest_respawn_x) >> 4 == NR_CX,
		"nether respawn: the plan targets the LEGAL destination -- the "
		"Overworld spawn in slot 0 -- through the cross-world preflight "
		"(capacity reserved, landing armed)");
	/* the arrival column goes missing for every loader */
	if(server_world_chunk_resident(&s->worlds[0], NR_CX, 0))
		evict(&s->worlds[0], NR_CX, 0);
	const int32_t fx[1] = {NR_CX}, fz[1] = {0};
	server_world_rz_fault_set(s->worlds[0].world_index,
		s->worlds[0].generation, fx, fz, 1);
	const unsigned window = permanent ? SERVER_TRANSFER_CLIENT_WAIT_TICKS + 40
		: 80;
	const unsigned retry0 = guest_respawn_retry_msgs;
	const uint64_t epoch1 = s->guest_respawn_epoch;
	bool committed_early = false;
	unsigned ended_at = 0;
	for(unsigned i = 0; i < window; i++) {
		if(permanent && s->guest_respawn_phase == GUEST_RESPAWN_IDLE) {
			ended_at = i;
			break;
		}
		nr_ready(s);
		drain_server(s);
		tick(s);
		committed_early |= g->health > 0 || g->world_slot != nslot;
	}
	printf("     [nether respawn %s: blocked loads %u, phase %d health %d "
		"slot %u fin %d ended at %u; ticket %u cleanup %u/%d client "
		"loading %d failure %d]\n", leg, server_world_rz_fault_blocked(),
		(int)s->guest_respawn_phase, g->health, (unsigned)g->world_slot,
		(int)g->finished_loading, ended_at,
		(unsigned)s->guest_respawn_cap.ticket,
		(unsigned)s->guest_respawn_cap.cleanup_ticket,
		(int)s->guest_respawn_cap.cleanup_cancel,
		(int)gc->local_respawn_loading, (int)gc->respawn_failure);
	snprintf(label, sizeof label, "nether respawn %s: nothing commits while "
		"the Overworld arrival column is unavailable (%u blocked loads) -- "
		"the corpse stays dead in the Nether slot", leg,
		server_world_rz_fault_blocked());
	check(!committed_early && server_world_rz_fault_blocked() > 0
		&& g->health <= 0 && g->world_slot == nslot, label);
	if(permanent) {
		check(ended_at > 0 && s->guest_respawn_phase == GUEST_RESPAWN_IDLE
			&& g->finished_loading && g->has_death_pos
			&& !s->guest_respawn_cap.ticket && !s->guest_respawn_cap.reserved
			&& !s->guest_respawn_cap.landing_armed
			&& !s->guest_respawn_cap.cleanup_ticket
			&& !s->guest_respawn_cap.cleanup_cancel
			&& !s->guest_respawn_anchor
			&& guest_respawn_retry_msgs == retry0 + 1
			&& guest_respawn_retry_epoch == epoch1,
			"nether respawn permanent: the bounded LOADING terminated -- "
			"capacity ticket RELEASED, landing arm cancelled, anchor dropped, "
			"corpse restored in the Nether, ONE failure for the plan's epoch");
		/* (the pane side of LOADING_FAILED is witnessed on the shared-world
		 * leg and in test_local_join_ready; this payloads-ON fixture has no
		 * installed client guest body: local_reply_failed from admission) */
		(void)gc;
		server_world_rz_fault_clear();
		const bool again = nr_request(s, &took);
		check(again && server_local_guest_respawn_target(s) == &s->worlds[0],
			"nether respawn permanent: the deliberate retry re-runs the "
			"cross-world preflight into a fresh LOADING plan");
	}
	server_world_rz_fault_clear();
	for(int i = 0; i < 400 && !(g->health > 0 && g->finished_loading
		&& s->guest_respawn_phase == GUEST_RESPAWN_IDLE); i++) {
		nr_ready(s);
		pump_handshake(s);
		tick(s);
	}
	struct server_world* now = server_local_world_of(s, g);
	snprintf(label, sizeof label, "nether respawn %s: once the column loads "
		"the body commits into the OVERWORLD (slot %d dim %d) at its spawn on "
		"a RESIDENT footprint, the Nether-era residency state retired by the "
		"real commit", leg, now ? (int)now->world_index : -1,
		now ? (int)now->dimension : -99);
	check(g->health > 0 && g->finished_loading && now == &s->worlds[0]
		&& g->dimension == WORLD_DIM_OVERWORLD
		&& (int)floor(g->x) >> 4 == NR_CX && lc_on_resident(s, g)
		&& lc_retired(g) && !s->guest_respawn_cap.ticket
		&& !s->guest_respawn_cap.reserved, label);
	const struct owner go = {.name = "guest", .guest = true, .body = g};
	const double x0 = g->x;
	for(int i = 0; i < 3; i++)
		tick(s);
	sample(s, &go, x0 + 0.25, g->y, g->z, g->correction_seq);
	check(fabs(g->x - (x0 + 0.25)) < 1e-9,
		"nether respawn: a fresh sample moves the respawned body");
}
static void case_lifecycle_nether_respawn(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	reset_body(g, 4.5, eye_y, 4.5);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &(struct owner) {.name = "guest", .guest = true, .body = g},
		4.5, eye_y, 4.5, g->correction_seq);
	tick(s);
	for(int pass = 0; pass < 2; pass++) {
		if(!bs_transfer(s, g, WORLD_DIM_NETHER, "guest -> Nether"))
			break;
		check(server_local_world_of(s, h) == &s->worlds[0]
			&& server_local_world_of(s, g)->dimension == WORLD_DIM_NETHER,
			"nether respawn: SEPARATE worlds -- host in the Overworld, guest "
			"in the Nether");
		g->spawn_x = NR_CX * 16 + 8;
		g->spawn_y = 4;
		g->spawn_z = 8;
		g->has_bed_spawn = false;
		nr_leg(s, pass == 1);
		server_world_rz_fault_clear();
	}
	teardown(s);
}
/* lifecycle_nether_respawn_far (residency-1005/6 R1, ASTRA-RESIDENCY-FINAL-2
 * lines 12-13) -- the REPORTED capacity deadlock. Same separate-world setup as
 * lifecycle_nether_respawn (guest in the Nether by a real A2 transfer, host
 * in the Overworld), but the guest's Overworld spawn is FAR from the host:
 * chunk (NRF_CX,0), whose landing core is disjoint from the host's radius-4
 * ring. The host is NOT moved and the spawn is NOT inside its ring. Measured
 * before the fix: resident 175/176 (slot0 94 + slot1 81), budget ENFORCED,
 * the landing never reports and every PREPARED preflight times out into
 * RETRY. Contract: the legal Overworld respawn lands through the production
 * loaders (no direct load, no fault hook), the resident set never exceeds
 * SERVER_TRANSFER_RESIDENT_COLUMN_CAP, the corpse's Nether collision core
 * stays resident while it is dead, and the committed body moves. */
#define NRF_CX 12
_Static_assert(SERVER_TRANSFER_RESIDENT_COLUMN_CAP == 256u,
	"the R1 witness pins the transfer cap it was measured against");
static void case_lifecycle_nether_respawn_far(void) {
	char label[260];
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	const double eye_y = s->player.y;
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	reset_body(g, 4.5, eye_y, 4.5);
	reset_body(h, 1.5, eye_y, 0.5);
	sample(s, &(struct owner) {.name = "guest", .guest = true, .body = g},
		4.5, eye_y, 4.5, g->correction_seq);
	tick(s);
	if(!bs_transfer(s, g, WORLD_DIM_NETHER, "guest -> Nether")) {
		check(false, "nether respawn far: the guest reached the Nether");
		teardown(s);
		return;
	}
	struct server_world* nether = server_local_world_of(s, g);
	check(server_local_world_of(s, h) == &s->worlds[0]
		&& nether && nether->dimension == WORLD_DIM_NETHER,
		"nether respawn far: SEPARATE worlds -- host in the Overworld, guest "
		"in the Nether");
	const uint8_t nslot = nether->world_index;
	const struct owner go0 = {.name = "guest", .guest = true, .body = g};
	for(int i = 0; i < 40 && (server_transfer_actor_inert(s, g)
		|| g->rz.recovering || !g->finished_loading
		|| s->a2w_guest_presentation); i++) {
		sample(s, &go0, g->x, g->y, g->z, g->correction_seq);
		pump_handshake(s);
		tick(s);
	}
	/* settle both rings: the host's Overworld ring and the guest's Nether
	 * ring fill to their steady state before the death */
	for(int i = 0; i < 200; i++)
		tick(s);
	const int r = server_local_view_radius(s);
	const w_coord_t hcx = WCOORD_CHUNK_OFFSET((int)floor(h->x));
	const w_coord_t hcz = WCOORD_CHUNK_OFFSET((int)floor(h->z));
	const w_coord_t ncx = WCOORD_CHUNK_OFFSET((int)floor(g->x));
	const w_coord_t ncz = WCOORD_CHUNK_OFFSET((int)floor(g->z));
	const double hx = h->x, hy = h->y, hz = h->z;
	const size_t settled0 = dict_server_chunks_size(s->worlds[0].chunks);
	const size_t settled1 = dict_server_chunks_size(nether->chunks);
	printf("     [far: settled resident slot0 %zu slot%u %zu = %zu / cap %u, "
		"radius %d, host chunk (%d,%d), corpse chunk (%d,%d)]\n", settled0,
		(unsigned)nslot, settled1,
		(size_t)server_transfer_resident_columns(s),
		(unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP, r, (int)hcx, (int)hcz,
		(int)ncx, (int)ncz);
	/* the spawn sits OUTSIDE the host ring with a disjoint landing core */
	g->spawn_x = NRF_CX * 16 + 8;
	g->spawn_y = 4;
	g->spawn_z = 8;
	g->has_bed_spawn = false;
	check(NRF_CX - SERVER_TRANSFER_LANDING_CORE_RADIUS - hcx > r,
		"nether respawn far: the spawn's landing core lies OUTSIDE the "
		"host's near ring (no host-loader help)");
	g->hurt_resist_time = 0;
	bool hit = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH, 0.0f,
		0.0f, &hit);
	check(g->health == 0, "nether respawn far: the guest died in the Nether");
	struct server_rpc rq = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation}};
	assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
		.generation = s->guest_generation}, &rq));
	drain_server(s);
	const unsigned retry0 = guest_respawn_retry_msgs;
	const unsigned window = 2 * SERVER_TRANSFER_CLIENT_WAIT_TICKS + 100;
	size_t peak = 0;
	unsigned loading_at = 0, committed_at = 0, i = 0;
	bool host_moved = false, core_lost = false;
	for(; i < window; i++) {
		nr_ready(s);
		pump_handshake(s);
		tick(s);
		const size_t res = server_transfer_resident_columns(s);
		if(res > peak)
			peak = res;
		host_moved |= h->x != hx || h->y != hy || h->z != hz
			|| server_local_world_of(s, h) != &s->worlds[0];
		if(g->health <= 0 && server_local_world_live(nether))
			for(int dz = -1; dz <= 1; dz++)
				for(int dx = -1; dx <= 1; dx++)
					core_lost |= !server_world_chunk_resident(nether,
						ncx + dx, ncz + dz);
		if(!loading_at && s->guest_respawn_phase == GUEST_RESPAWN_LOADING)
			loading_at = i + 1;
		if(g->health > 0 && g->finished_loading
		   && s->guest_respawn_phase == GUEST_RESPAWN_IDLE) {
			committed_at = i + 1;
			break;
		}
	}
	struct server_world* now = server_local_world_of(s, g);
	printf("     [far: LOADING at %u, committed at %u (window %u); peak "
		"resident %zu / cap %u; now slot0 %zu slot%u %zu; budget phase %d "
		"cap_waits %u; RETRY msgs %u; corpse core lost %d; host moved %d]\n",
		loading_at, committed_at, window, peak,
		(unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		dict_server_chunks_size(s->worlds[0].chunks), (unsigned)nslot,
		server_local_world_live(nether)
			? dict_server_chunks_size(nether->chunks) : 0,
		(int)s->transfer_budget_phase, (unsigned)s->transfer_column_cap_waits,
		guest_respawn_retry_msgs - retry0, (int)core_lost, (int)host_moved);
	snprintf(label, sizeof label, "nether respawn far: the legal Overworld "
		"respawn LANDS (LOADING at %u, commit at %u, no RETRY) through the "
		"production loaders", loading_at, committed_at);
	check(committed_at > 0 && loading_at > 0
		&& guest_respawn_retry_msgs == retry0, label);
	snprintf(label, sizeof label, "nether respawn far: the resident set "
		"stays WITHIN the cap the whole time (peak %zu <= %u, budget "
		"enforced)", peak, (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP);
	check(peak <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP
		&& s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED, label);
	/* Witness re-measured at cap 256 (colcap-1006): printed above as settled
	 * slot0 81 + slot1 81 = 162 and peak resident 172 / 256 (was 175/176 at
	 * the old cap, where the landing never reported). Pinned so a change in
	 * the far-respawn footprint is re-measured, not absorbed. */
	snprintf(label, sizeof label, "nether respawn far: witness re-measured at "
		"cap 256 (colcap-1006): settled %zu + %zu == 162, peak %zu == 172",
		settled0, settled1, peak);
	check(settled0 + settled1 == 162 && peak == 172, label);
	check(!host_moved, "nether respawn far: the host was never moved (pose "
		"and Overworld slot unchanged)");
	check(!core_lost, "nether respawn far: the corpse 3x3 Nether collision "
		"core stayed resident while it was dead");
	snprintf(label, sizeof label, "nether respawn far: the body committed "
		"into the OVERWORLD (slot %d dim %d) at its far spawn chunk %d on a "
		"resident footprint, Nether-era residency state retired",
		now ? (int)now->world_index : -1, now ? (int)now->dimension : -99,
		(int)floor(g->x) >> 4);
	check(g->health > 0 && g->finished_loading && now == &s->worlds[0]
		&& g->dimension == WORLD_DIM_OVERWORLD
		&& (int)floor(g->x) >> 4 == NRF_CX && lc_on_resident(s, g)
		&& lc_retired(g) && !s->guest_respawn_cap.ticket
		&& !s->guest_respawn_cap.reserved, label);
	if(committed_at) {
		const struct owner go = {.name = "guest", .guest = true, .body = g};
		const double x0 = g->x;
		for(int k = 0; k < 3; k++)
			tick(s);
		sample(s, &go, x0 + 0.25, g->y, g->z, g->correction_seq);
		check(fabs(g->x - (x0 + 0.25)) < 1e-9,
			"nether respawn far: a fresh sample moves the respawned body");
		check(server_transfer_resident_columns(s)
			<= SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
			"nether respawn far: after the respawn the session is back "
			"within the cap");
	}
	server_world_rz_fault_clear();
	teardown(s);
}
/* lifecycle_saved_join_bounded (residency-1005/6 R2, ASTRA-RESIDENCY-FINAL-2
 * lines 14-15): the saved-pose Join terrain wait is BOUNDED. Same fixture as
 * lifecycle_saved_join (the real Leave checkpoints CaveWiiGuest; the next
 * Join restores the saved pose (36.5, 4.5) in chunk (2,0), whose column is
 * held missing for every loader by the item-12 fault hook).
 *   delayed   -- 400 failing ticks (inside the bound), then the column
 *                returns: admitted AT the saved pose, no failure, wait
 *                disarmed;
 *   permanent -- the wait TERMINATES exactly SERVER_TRANSFER_CLIENT_WAIT_TICKS
 *                + 1 updates after the first terrain denial: ONE
 *                LOCAL_GUEST_JOIN_LOADING_FAILED, the body never admitted
 *                and its residency state retired, the client stops asking and
 *                raises the visible notice (host HUD reason + join card);
 *   stale     -- with the column back, a late GAMEPLAY of the failed
 *                generation is DENIED; a duplicate failure is inert; after
 *                the re-Join, a failure of the OLD generation is inert;
 *   Leave     -- the waiting guest leaves through the real client Leave
 *                (what main.c does on the failure), ownership released, the
 *                saved record byte-equal (nbt_eq) to the one before the Join;
 *   re-Join   -- a deliberate re-Join restores the saved pose and, terrain
 *                back, is admitted on a resident footprint and moves. */
static void sjb_fault_column(struct server_local* s) {
	if(server_world_chunk_resident(&s->worlds[0], 2, 0))
		evict(&s->worlds[0], 2, 0);
	const int32_t fx[1] = {2}, fz[1] = {0};
	server_world_rz_fault_set(s->worlds[0].world_index,
		s->worlds[0].generation, fx, fz, 1);
}
static nbt_node* sjb_record(struct server_local* s) {
	nbt_node* rec = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest");
	return rec ? nbt_clone(rec) : NULL;
}
static void case_lifecycle_saved_join_bounded(void) {
	char label[260];
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct client_session* gc = &gstate.players[1];
	const double eye_y = s->player.y;
	reset_body(&s->player, 1.5, eye_y, 0.5);
	tick(s);
	struct server_player* g = s->guest_actor;
	reset_body(g, SJ_X, s->player.y, SJ_Z);
	sample(s, &(struct owner) {.name = "guest", .guest = true, .body = g},
		SJ_X, g->y, SJ_Z, g->correction_seq);
	tick(s);
	sj_real_leave(s);
	check(!s->guest_actor && gc->local_phase == CLIENT_GUEST_IDLE,
		"bounded join: the guest left through the real client Leave "
		"(record checkpointed)");

	/* ---- delayed arrival inside the bound ---- */
	sjb_fault_column(s);
	sj_join(s);
	g = s->guest_actor;
	check(g && g->x == SJ_X && g->z == SJ_Z && lc_retired(g),
		"bounded join delayed: Join restored the saved pose");
	const unsigned f0 = join_failed_msgs;
	bool granted = false;
	for(unsigned i = 0; i < 400 && !granted; i++) {
		(void)client_session_request_local_gameplay(gc);
		pump_handshake(s);
		tick(s);
		granted = s->guest_gameplay || gc->local_gameplay_ready;
	}
	snprintf(label, sizeof label, "bounded join delayed: 400 ticks inside "
		"the bound -- denied, still waiting (armed %d, %u ticks), no failure",
		(int)s->guest_join_wait.armed, (unsigned)s->guest_join_wait.wait_ticks);
	check(!granted && s->guest_join_wait.armed && !s->guest_join_wait.failed
		&& s->guest_join_wait.wait_ticks >= 390
		&& s->guest_join_wait.wait_ticks <= SERVER_TRANSFER_CLIENT_WAIT_TICKS
		&& join_failed_msgs == f0 && !gc->local_join_failed
		&& gc->local_phase == CLIENT_GUEST_ATTACHED, label);
	server_world_rz_fault_clear();
	bool ok = sj_request_gameplay(s, 150);
	g = s->guest_actor;
	check(ok && g && s->guest_gameplay && g->finished_loading
		&& g->x == SJ_X && g->z == SJ_Z && lc_on_resident(s, g)
		&& join_failed_msgs == f0 && !s->guest_join_wait.armed
		&& !s->guest_join_wait.failed,
		"bounded join delayed: the column arrived inside the bound -- "
		"admitted AT the saved pose on a resident footprint, wait disarmed, "
		"no failure");
	sj_real_leave(s);
	nbt_node* rec0 = sjb_record(s);
	check(rec0 != NULL && !s->guest_actor,
		"bounded join: the delayed-leg body left; saved record captured");

	/* ---- permanent failure ---- */
	sjb_fault_column(s);
	sj_join(s);
	g = s->guest_actor;
	const uint64_t gen1 = s->guest_generation;
	const uint64_t ticket1 = gc->local_ticket;
	unsigned armed_at = 0, failed_at = 0, extra = 0;
	bool live_unknown = false;
	granted = false;
	const unsigned f1 = join_failed_msgs;
	for(unsigned i = 1; i <= 2 * SERVER_TRANSFER_CLIENT_WAIT_TICKS; i++) {
		(void)client_session_request_local_gameplay(gc);
		pump_handshake(s);
		tick(s);
		if(!armed_at && s->guest_join_wait.armed)
			armed_at = i;
		if(!failed_at && join_failed_msgs != f1)
			failed_at = i;
		granted |= s->guest_gameplay || gc->local_gameplay_ready;
		live_unknown |= g->finished_loading;
		if(failed_at && ++extra > 60)
			break;
	}
	printf("     [bounded join permanent: armed at %u, failed at %u "
		"(+%d), FAILED msgs %u, blocked loads %u, client failed %d phase %d "
		"notice '%s']\n", armed_at, failed_at,
		(int)failed_at - (int)armed_at, join_failed_msgs - f1,
		server_world_rz_fault_blocked(), (int)gc->local_join_failed,
		(int)gc->local_phase,
		client_session_local_admit_failure_reason(gc)
			? client_session_local_admit_failure_reason(gc) : "");
	/* the denial that arms the wait is processed inside update armed_at,
	 * which already counts; the failure lands in update failed_at */
	snprintf(label, sizeof label, "bounded join permanent: the wait "
		"TERMINATED on update +%d of the first terrain denial (bound %u, "
		"counted %u) with exactly ONE failure for this generation",
		(int)failed_at - (int)armed_at + 1,
		(unsigned)SERVER_TRANSFER_CLIENT_WAIT_TICKS,
		(unsigned)s->guest_join_wait.wait_ticks);
	check(armed_at > 0 && failed_at > 0
		&& failed_at - armed_at + 1 == SERVER_TRANSFER_CLIENT_WAIT_TICKS + 1
		&& s->guest_join_wait.wait_ticks
			== SERVER_TRANSFER_CLIENT_WAIT_TICKS + 1
		&& join_failed_msgs == f1 + 1 && s->guest_join_wait.failed
		&& !s->guest_join_wait.armed, label);
	check(!granted && !live_unknown && s->guest_actor == g
		&& !s->guest_gameplay && !g->finished_loading && lc_retired(g)
		&& !server_local_guest_join_wait_failed(NULL)
		&& server_local_guest_join_wait_failed(s),
		"bounded join permanent: the body was never admitted, its residency "
		"state is retired and it no longer owns stream demand");
	const char* why = client_session_local_admit_failure_reason(gc);
	check(gc->local_join_failed && gc->local_phase == CLIENT_GUEST_ATTACHED
		&& !gc->local_gameplay_ready && why
		&& strstr(why, "could not join")
		&& client_session_local_admit_failure_present(gc, &gstate.players[0])
		&& !client_session_request_local_gameplay(gc),
		"bounded join permanent: VISIBLE retryable failure -- the reason is "
		"on the host HUD slot, the join card settles as refused, and the "
		"client stops asking for the body");
	/* stale: the terrain comes back; a late GAMEPLAY of the failed
	 * generation (in flight at the bound) cannot revive the Join */
	server_world_rz_fault_clear();
	for(int i = 0; i < 60
		&& !server_world_is_chunk_sent(&s->worlds[0], 2, 0); i++)
		tick(s);
	struct server_rpc late = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket1,
			.action = LOCAL_GUEST_GAMEPLAY}};
	assert(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
		.generation = gen1}, &late));
	drain_server(s);
	tick(s);
	check(server_world_is_chunk_sent(&s->worlds[0], 2, 0)
		&& !s->guest_gameplay && !g->finished_loading
		&& !gc->local_gameplay_ready,
		"bounded join stale: with the column back, a late GAMEPLAY of the "
		"failed generation is DENIED (no revival)");
	const unsigned f2 = join_failed_msgs;
	clin_rpc_send(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket1, .generation = gen1,
			.owner = 1, .result = LOCAL_GUEST_JOIN_LOADING_FAILED}});
	clin_update_drain();
	check(join_failed_msgs == f2 + 1 && gc->local_join_failed
		&& !client_session_local_admit_failure_present(gc,
			&gstate.players[0]),
		"bounded join stale: a duplicate failure is inert (no second notice)");
	/* Leave: what main.c does on the failure */
	lc_poison(s, g);
	sj_real_leave(s);
	nbt_node* rec1 = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest");
	check(!s->guest_actor && !s->guest_owned
		&& gc->local_phase == CLIENT_GUEST_IDLE
		&& !s->guest_join_wait.failed && !s->guest_join_wait.armed,
		"bounded join Leave: the failed Join leaves through the real client "
		"Leave -- ownership released, wait state cleared");
	check(rec0 && rec1 && nbt_eq(rec0, rec1),
		"bounded join Leave: the saved record is INTACT (byte-equal to the "
		"record before the failed Join)");
	/* re-Join: deliberate, fresh generation; the old failure is stale */
	sj_join(s);
	g = s->guest_actor;
	check(g && s->guest_generation != gen1 && g->x == SJ_X && g->z == SJ_Z
		&& lc_retired(g) && !gc->local_join_failed,
		"bounded join re-Join: a NEW generation restores the saved pose from "
		"the intact record");
	clin_rpc_send(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket1, .generation = gen1,
			.owner = 1, .result = LOCAL_GUEST_JOIN_LOADING_FAILED}});
	clin_update_drain();
	check(!gc->local_join_failed && gc->local_phase == CLIENT_GUEST_ATTACHED,
		"bounded join stale: a failure of the OLD generation is inert after "
		"the re-Join");
	ok = sj_request_gameplay(s, 150);
	g = s->guest_actor;
	check(ok && g && s->guest_gameplay && g->finished_loading
		&& g->x == SJ_X && g->z == SJ_Z && lc_on_resident(s, g)
		&& lc_retired(g) && !s->guest_join_wait.failed,
		"bounded join re-Join: admitted AT the saved pose on a resident "
		"footprint");
	if(g) {
		const struct owner go = {.name = "guest", .guest = true, .body = g};
		sample(s, &go, SJ_X + 0.25, g->y, SJ_Z, g->correction_seq);
		check(fabs(g->x - (SJ_X + 0.25)) < 1e-9,
			"bounded join re-Join: a fresh sample moves the admitted body");
	}
	nbt_free(rec0);
	server_world_rz_fault_clear();
	teardown(s);
}
/* R7 recovery tape lines 249-250: both owners walked, then dug and stood
 * 83 blocks apart. P2 line 287 reaches host-only layout; line 480 times out.
 * Reproduce the real saved Join beyond the host ring, with its column evicted. */
static void r7_separated_rejoin(struct server_local* s) {
	string_set_str(s->level_name, split_path);
	assert(tick_until_resident(s, &s->worlds[0], 0, -3, 200));
	assert(tick_until_resident(s, &s->worlds[0], 0, 2, 200));
	reset_body(&s->player, 0.5, s->player.y, -40.51);
	reset_body(s->guest_actor, 0.5, s->player.y, 42.53);
	tick(s);
	sj_real_leave(s);
	evict(&s->worlds[0], 0, 2);
	sj_join(s);

	check(sj_request_gameplay(s, 200),
		"R7 separated saved Join: real admission outside host ring before P2 deadline");

	check(s->guest_actor && s->guest_actor->finished_loading
		&& fabs(s->guest_actor->z - 42.53) < 0.001,
		"R7 separated saved Join: saved position preserved");
}
static void case_lifecycle_saved_join(void) {
	struct server_local* s = admitted_fixture();
	/* the session save root, so the real Leave's checkpoint can write */
	string_set_str(s->level_name, split_path);
	const double eye_y = s->player.y;
	reset_body(&s->player, 1.5, eye_y, 0.5);
	tick(s);
	sj_leg(s, false);
	sj_leg(s, true);
	r7_separated_rejoin(s);
	server_world_rz_fault_clear();
	teardown(s);
}

/* ======================================================================== */
static void case_host(void) { case_owner(false); }
static void case_guest(void) { case_owner(true); }
static const struct { const char* name; void (*run)(void); } k_cases[] = {
	{"host", case_host}, {"guest", case_guest}, {"unknown", case_unknown},
	{"latency", case_latency}, {"loadahead", case_loadahead},
	{"sources", case_sources}, {"portal", case_portal},
	{"feedback", case_feedback}, {"lean", case_lean},
	{"bounded", case_bounded}, {"lifecycle", case_lifecycle},
	{"rejoin", case_rejoin}, {"toasts", case_toasts},
	{"evictors", case_evictors}, {"saturation", case_saturation},
	{"held_controls", case_held_controls},
	{"client_retry", case_client_retry},
	{"wake", case_wake},
	{"telemetry_lifecycle", case_telemetry_lifecycle},
	{"runtime_fault", case_runtime_fault},
	{"lifecycle_missing", case_lifecycle_missing},
	{"lifecycle_initial_load", case_lifecycle_initial_load},
	{"lifecycle_saved_join", case_lifecycle_saved_join},
	{"lifecycle_saved_join_bounded", case_lifecycle_saved_join_bounded},
	{"lifecycle_nether_respawn", case_lifecycle_nether_respawn},
	{"lifecycle_nether_respawn_far", case_lifecycle_nether_respawn_far},
	{"vehicles_guest_away", case_vehicles_guest_away},
	{"vehicles_host_away", case_vehicles_host_away},
	{"vehicles", case_vehicles},
	{"knockback", case_knockback},
	{"bounded_slots", case_bounded_slots},
};
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* which = argc > 1 ? argv[1] : NULL;
	void (*run)(void) = NULL;
	for(size_t i = 0; which && i < sizeof k_cases / sizeof k_cases[0]; i++)
		if(!strcmp(which, k_cases[i].name))
			run = k_cases[i].run;
	if(!which
	   || (!run && strcmp(which, "equiv") && strcmp(which, "tape")
		   && strcmp(which, "trace0") && strcmp(which, "trace1"))) {
		fprintf(stderr, "usage: %s <case>\n", argv[0]);
		return 2;
	}
	if(!strcmp(which, "equiv")) {
		case_equiv(argv[0]);
		if(failures) {
			printf("test_player_residency_admission equiv: %u FAILED\n",
				failures);
			return 1;
		}
		printf("test_player_residency_admission equiv: PASS\n");
		return 0;
	}
	/* The guest portal tick needs world-bound payloads; the switch is only
	 * accepted between sessions, i.e. before the first WORLD_RESET. */
	if(!strcmp(which, "lifecycle_saved_join")
	   || !strcmp(which, "portal") || !strcmp(which, "bounded_slots")
	   || !strcmp(which, "lifecycle_nether_respawn")
	   || !strcmp(which, "lifecycle_nether_respawn_far")
	   || !strcmp(which, "vehicles_guest_away")
	   || !strcmp(which, "vehicles_host_away"))
		(void)clin_world_payloads_set_enabled(true);
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);

	if(run)
		run();
	else if(!strcmp(which, "tape"))
		case_tape(argc > 2 ? argv[2] : "residency_tape_pc.ndjson");
	else
		case_trace(!strcmp(which, "trace1"));

	if(failures) {
		printf("test_player_residency_admission %s: %u FAILED\n", which,
			failures);
		return 1;
	}
	printf("test_player_residency_admission %s: PASS\n", which);
	return 0;
}
