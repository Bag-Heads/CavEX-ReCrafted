/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d: the shared owner-swap harness.
 *
 * Lifted verbatim from tests/test_owner_swap_pose_dead_sleep_barrier.c (C2a)
 * so every test_owner_swap_*.c drives the SAME real admission -- Join ->
 * ATTACHED -> gameplay request -> body installed -- on the split residency
 * fixture, with the server paused (transport only) and each case owning a
 * fresh fixture (exactly one case per process; argv[1] selects it).
 *
 * "Owner-swapped" means every case drives the SAME request at the host body
 * (owner 0, the reference the P2 inventory rows cite) and at the guest body
 * (owner 1), and pins where the two must agree and where they differ on
 * purpose. RED checks report ok/FAIL and exit 1 (never abort) so a RED run
 * shows every check; setup asserts may abort.
 *
 * Every envelope the client consumes is tallied per route by `consumed`, so a
 * case can say "exactly one correction reached this owner", "one footstep
 * sound was published", or "one HUD toast named the refusal". */
#ifndef TESTS_OWNER_SWAP_HARNESS_H
#define TESTS_OWNER_SWAP_HARNESS_H
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif
#include "split_residency_fixture.h"
#include "entity/actor_ident.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/server_local_guest.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
static inline void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

/* ---- transport harness (test_split_partner_fire / test_split_live_toggle) - */
static unsigned markers;
static uint32_t update_done;

static unsigned host_pos_msgs;      /* CRPC_PLAYER_POS without input_epoch  */
static uint8_t host_pos_flags;      /* correction_flags of the last one     */
static uint32_t host_pos_seq;       /* correction_seq of the last one       */
static unsigned guest_corrections;  /* CRPC_LOCAL_GUEST LOCAL_GUEST_CORRECTION */
static uint8_t guest_corr_flags;
static uint32_t guest_corr_seq;
static double guest_corr_pos[3];
static unsigned sound_msgs;         /* CRPC_SOUND, positional (ui == false)  */
static uint16_t last_sound_id;
static float last_sound_pos[3];
static unsigned hud_msgs;           /* CRPC_HUD_MESSAGE                      */
static char last_hud[48];
static unsigned sleep_state_msgs;   /* CRPC_SLEEP_STATE                      */
static bool last_sleep_state;

static inline void consumed(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_PLAYER_POS) {
		if(rpc->payload.player_pos.input_epoch) {
			markers++;
		} else {
			host_pos_msgs++;
			host_pos_flags = rpc->payload.player_pos.correction_flags;
			host_pos_seq = rpc->payload.player_pos.correction_seq;
		}
	}
	if(rpc->type == CRPC_LOCAL_GUEST
	   && rpc->payload.local_guest.result == LOCAL_GUEST_CORRECTION) {
		guest_corrections++;
		guest_corr_flags = rpc->payload.local_guest.correction_flags;
		guest_corr_seq = rpc->payload.local_guest.correction_seq;
		guest_corr_pos[0] = rpc->payload.local_guest.x;
		guest_corr_pos[1] = rpc->payload.local_guest.y;
		guest_corr_pos[2] = rpc->payload.local_guest.z;
	}
	if(rpc->type == CRPC_SOUND && !rpc->payload.sound.ui) {
		sound_msgs++;
		last_sound_id = rpc->payload.sound.sound_id;
		last_sound_pos[0] = rpc->payload.sound.pos[0];
		last_sound_pos[1] = rpc->payload.sound.pos[1];
		last_sound_pos[2] = rpc->payload.sound.pos[2];
	}
	if(rpc->type == CRPC_HUD_MESSAGE) {
		hud_msgs++;
		memcpy(last_hud, rpc->payload.hud_message, sizeof(last_hud));
		last_hud[sizeof(last_hud) - 1] = 0;
	}
	if(rpc->type == CRPC_SLEEP_STATE) {
		sleep_state_msgs++;
		last_sleep_state = rpc->payload.sleep_state.sleeping;
	}
}
static inline void counters_reset(void) {
	host_pos_msgs = 0; host_pos_flags = 0; host_pos_seq = 0;
	guest_corrections = 0; guest_corr_flags = 0; guest_corr_seq = 0;
	guest_corr_pos[0] = guest_corr_pos[1] = guest_corr_pos[2] = 0.0;
	sound_msgs = 0; last_sound_id = 0;
	last_sound_pos[0] = last_sound_pos[1] = last_sound_pos[2] = 0.0F;
	hud_msgs = 0; last_hud[0] = 0;
	sleep_state_msgs = 0; last_sleep_state = false;
}
static void* update_server(void* ptr) {
	server_local_test_update_once(ptr);
	__atomic_store_n(&update_done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static inline void step(struct server_local* s) {
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
/* One real server update on THIS thread (the region archive is owned by the
 * thread that created it) with a temporary consumer thread supplying the
 * other half of the bounded client inbox -- test_split_partner_fire's tick(). */
static int consumer_running;
static void* consumer_entry(void* unused) {
	(void)unused;
	while(__atomic_load_n(&consumer_running, __ATOMIC_ACQUIRE)) {
		clin_update_drain();
		thread_msleep(1);
	}
	return NULL;
}
static inline void tick(struct server_local* s) {
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_local_test_update_once(s);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
}
static inline void stream_to_marker(struct server_local* s) {
	unsigned before = markers;
	for(unsigned i = 0; i < 2000 && markers == before; i++)
		step(s);
	assert(markers == before + 1 && s->input_ack_pending);
}
static inline void acknowledge(struct server_local* s) {
	/* Real 30 s readiness fallback: no mesher workers exist in this rig. */
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	step(s);
	assert(!s->input_ack_pending && !s->stream_recovery_epoch);
	assert(s->input_epoch_counter != 0);
}
/* The real client's load screen: a pending input-handoff marker (a load, or
 * the host's A2-T world switch) is acknowledged once it arrives -- with the
 * 30 s readiness fallback, since this rig has no mesher workers. */
static inline void client_acks_marker(struct server_local* s) {
	if(!s->input_ack_pending)
		return;
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
}
static inline void drain_server(struct server_local* s) {
	server_local_test_process_messages(s);
	clin_update_drain();
}
static inline void enable_split(struct server_local* s) {
	struct server_rpc call = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = true,
			.world_generation = s->input_epoch_counter, .request_seq = 1}};
	assert(svin_rpc_send(&call));
	drain_server(s);
	assert(s->split_enabled);
}
static inline void pump_handshake(struct server_local* s) {
	client_session_flush_local(&gstate.players[1]);
	server_local_test_process_messages(s);
	clin_update_drain();
}
static inline void guest_attach(struct server_local* s) {
	struct client_session* client = &gstate.players[1];
	assert(client_session_request_local_join(client));
	pump_handshake(s);
	pump_handshake(s);
	pump_handshake(s);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED && s->guest_confirmed);
}
/* Place the guest body in the host's own published column so the production
 * readiness predicate can pass (same setup as test_split_live_toggle). */
static inline void guest_body_home(struct server_local* s) {
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
/* test_split_live_toggle's guest_release: the checkpointed release queues no
 * LEFT ack, so the client half is retired by hand. */
static inline void guest_release(struct server_local* s) {
	s->paused = true;
	server_local_guest_release_checkpointed(s);
	assert(!s->guest_actor && !s->guest_gameplay && !s->guest_generation);
	client_session_clear_windows(&gstate.players[1]);
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[1].visible_window = -1;
	clin_update_drain();
}
/* The REAL admission: Join -> ATTACHED -> gameplay request -> body installed.
 * Leaves the server paused (transport only). */
static inline struct server_local* admitted_fixture(void) {
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
	assert(guest_client->local_player && gstate.players[0].local_player);
	assert(s->guest_actor && s->guest_actor->finished_loading);
	assert(s->player.has_pos && s->player.finished_loading);
	return s;
}
static inline void teardown(struct server_local* s) {
	if(s->guest_actor)
		guest_release(s);
	s->paused = true;
	split_destroy(s);
}

/* ---- the owner-swapped request ------------------------------------------- */
static inline struct server_rpc_source guest_source(const struct server_local* s) {
	return (struct server_rpc_source) {.slot = 1, .generation = s->guest_generation};
}
static inline struct actor_ident guest_ident(const struct server_local* s) {
	return (struct actor_ident) {.generation = s->guest_generation,
		.slot = ACTOR_SLOT_GUEST};
}
/* Owner 0: the host's own inbox class. Always queued. */
static inline void host_send(struct server_local* s, struct server_rpc* call) {
	assert(svin_rpc_send(call));
	drain_server(s);
}
/* Owner 1: the guest's tagged route. Returns whether the QUEUE admitted it --
 * a refusal at that boundary (server_interface.c's slot-1 verb list) is itself
 * an observation a case may pin, so this never asserts. */
static inline bool guest_send(struct server_local* s, struct server_rpc* call) {
	bool queued = svin_rpc_send_from(guest_source(s), call);
	drain_server(s);
	return queued;
}
static inline struct server_rpc pose_sample(double x, double y, double z,
	bool on_ground, uint32_t seq) {
	return (struct server_rpc) {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x, .y = y, .z = z, .rx = 0.0F, .ry = 0.0F,
			.vel_x = 0.0F, .vel_y = 0.0F, .vel_z = 0.0F,
			.on_ground = on_ground, .sneaking = false, .correction_seq = seq}};
}
static inline void host_sample(struct server_local* s, double x, double y, double z,
	bool on_ground, uint32_t seq) {
	struct server_rpc call = pose_sample(x, y, z, on_ground, seq);
	host_send(s, &call);
}
static inline void guest_sample(struct server_local* s, double x, double y, double z,
	bool on_ground, uint32_t seq) {
	struct server_rpc call = pose_sample(x, y, z, on_ground, seq);
	assert(svin_rpc_send_from(guest_source(s), &call));
	drain_server(s);
}
/* A one-tick walking step for either owner: +dx on x, on the ground, not
 * sneaking, echoing the body's current token. */
static inline void walk_step(struct server_local* s, struct server_player* p, double dx,
	bool sneaking) {
	struct server_rpc call = pose_sample(p->x + dx, p->y, p->z, true,
		p->correction_seq);
	call.payload.player_pos.sneaking = sneaking;
	if(p == &s->player) {
		host_send(s, &call);
	} else {
		assert(svin_rpc_send_from(guest_source(s), &call));
		drain_server(s);
	}
}
static inline bool near3(const float* pos, double x, double y, double z) {
	return fabsf(pos[0] - (float)x) < 1e-4f && fabsf(pos[1] - (float)y) < 1e-4f
		&& fabsf(pos[2] - (float)z) < 1e-4f;
}
/* Set one world block in the loaded home column (superflat: bedrock 0, dirt
 * 1-2, grass 3, air above; both bodies stand with their feet at y == 4). */
static inline void put_block(struct server_local* s, int x, int y, int z, uint8_t type,
	uint8_t meta) {
	assert(server_world_set_block(&s->worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta}));
}
static inline uint8_t block_at(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s->worlds[0], x, y, z, &b));
	return b.type;
}
/* Move a body sideways in the home column as SETUP (the same direct write
 * guest_body_home does), so the two owners act on their own props. */
static inline void body_stand_at(struct server_player* p, double x, double z) {
	p->x = x;
	p->z = z;
}

static inline void harness_init(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
}
static inline int harness_result(const char* name, const char* which) {
	if(failures) {
		printf("%s[%s]: %u FAILED\n", name, which, failures);
		return 1;
	}
	printf("%s[%s]: PASS\n", name, which);
	return 0;
}
#endif
