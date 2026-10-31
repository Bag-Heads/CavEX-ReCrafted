/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/C2a: owner-swapped movement policy for the local
 * split-screen guest body -- P2 inventory rows #4, #25, #26 and the respawn
 * ticks_existed reset (Entity.java:51).
 *
 * "Owner-swapped" means every case drives the SAME sample / the SAME hit at the
 * host body (owner 0, the reference the rows cite) and at the guest body
 * (owner 1) on one live fixture, and pins where the two agree and where they
 * differ on purpose:
 *
 *   barrier   (row #4, test_owner_swap_pose_dead_sleep_barrier)
 *     Beta EntityPlayer.isMovementBlocked (EntityPlayer.java:127-129) is
 *     `health <= 0 || isPlayerSleeping()`; EntityLiving.onLivingUpdate
 *     (EntityLiving.java:618-622) zeroes the input while it holds. The host
 *     enforces it with three classifiers ahead of its SRPC switch
 *     (server_local.c server_rpc_blocked_while_sleeping / _while_dead /
 *     _while_host_respawning): sleeping DROPS SRPC_PLAYER_POS, dead lets the
 *     corpse's physics sample FLOW, an in-flight respawn plan DROPS it. The
 *     guest's only movement route (server_local_guest_position) checked load
 *     and correction state but neither sleep nor the respawn plan. RED on the
 *     base for the sleeping leg and the PREPARED-plan leg; the dead leg pins
 *     the host-identical "corpse physics flow" so nobody "fixes" it later.
 *     Then a REAL guest respawn (SRPC_RESPAWN -> PREPARED -> LOADING -> READY
 *     -> poll -> commit) must hand back a body with ticks_existed == 0
 *     (Entity.java:51 `public int ticksExisted = 0;` on the NEW EntityPlayer
 *     Beta constructs at respawn; the host's W1F resets). RED on the base:
 *     the reused guest body inherited the corpse's count.
 *
 *   knockback (row #25, test_owner_swap_knockback_fall_momentum)
 *     G8-BLAST-VELOCITY v3: the hit raises beenAttacked on each body and
 *     sends nothing; both clients keep their own motion until the tracker
 *     phase delivers Q(server motion) -- equal for matched server states --
 *     on each body's own route (Packet28 -> setVelocity). The server motion
 *     step (server_local_tick_actor_knockback = one accepted sample) moves
 *     no body, bumps no token, sends no correction, and without a new fresh
 *     hit nothing is re-sent, so the clients steer through the push.
 *
 *   delta     (row #26, test_owner_swap_large_pose_delta_policy)
 *     A teleport-scale sample (horizontal delta^2 >= PLAYER_MOVE_TELEPORT_
 *     DELTA_SQ) is ACCEPTED by the host (only its footstep odometer is
 *     suppressed) and REFUSED + corrected for the guest, because the guest
 *     body owns residency anchor 1 and a jump would drag a streaming ring
 *     with it (the Cave transport rule). Edges pinned: 1.99 accepted, 2.00
 *     refused; the stale token is refused, the fresh one accepted. Passes on
 *     the base.
 *
 * One executable, three ctest names: argv[1] selects the case. Each case owns
 * a fresh fixture, so exactly one case runs per process. The harness (real
 * server, REAL guest admission Join -> ATTACHED -> gameplay -> body installed,
 * transport-only paused server) is test_split_partner_fire's shape.
 *
 * Non-aborting: every check reports ok/FAIL so a RED run shows all of them.
 */
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif
#include "support/split_residency_fixture.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/server_local_guest.h"
#include "block/dispenser_logic.h" /* DISPENSER_PROJECTILE_OWNER_SENTINEL */
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

/* ---- transport harness (test_split_partner_fire / test_split_live_toggle) - */
static unsigned markers;
static uint32_t update_done;

/* Every envelope the client consumes, tallied per route so a case can say
 * "exactly one correction reached this owner, carrying these flags". */
static unsigned host_pos_msgs;      /* CRPC_PLAYER_POS without input_epoch  */
static uint8_t host_pos_flags;      /* correction_flags of the last one     */
static uint32_t host_pos_seq;       /* correction_seq of the last one       */
static unsigned guest_corrections;  /* CRPC_LOCAL_GUEST LOCAL_GUEST_CORRECTION */
static uint8_t guest_corr_flags;
static uint32_t guest_corr_seq;
static double guest_corr_pos[3];

/* G8-BLAST-VELOCITY v3 lifetime evidence: every consumed Packet28-equivalent
 * velocity message, per owner (0 host, 1 guest), with its last payload. */
static unsigned vel_msgs[2];
static float vel_last[2][3];
static bool vel_last_add[2];
static void vel_reset(void) {
	memset(vel_msgs, 0, sizeof vel_msgs);
	memset(vel_last, 0, sizeof vel_last);
	memset(vel_last_add, 0, sizeof vel_last_add);
}
static float q28(double v) {
	if(v < -3.9) v = -3.9;
	if(v > 3.9) v = 3.9;
	return (float)(int32_t)(v * 8000.0) / 8000.0f;
}

static void consumed(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_LOCAL_PLAYER_VELOCITY
	   && rpc->payload.local_player_velocity.owner < 2) {
		const unsigned o = rpc->payload.local_player_velocity.owner;
		vel_msgs[o]++;
		memcpy(vel_last[o], rpc->payload.local_player_velocity.vel,
			   sizeof vel_last[o]);
		vel_last_add[o] = rpc->payload.local_player_velocity.add;
	}
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
}
static void counters_reset(void) {
	host_pos_msgs = 0; host_pos_flags = 0; host_pos_seq = 0;
	guest_corrections = 0; guest_corr_flags = 0; guest_corr_seq = 0;
	guest_corr_pos[0] = guest_corr_pos[1] = guest_corr_pos[2] = 0.0;
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
/* One real server update. The simulation touches the region archive, which is
 * owned by the thread that created it (this one), so the update runs HERE and
 * a temporary consumer thread supplies production's other half of the bounded
 * client inbox -- test_split_partner_fire's tick(). Client state is read only
 * after the consumer is joined and a final drain on this thread. */
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
/* G8-BLAST-VELOCITY v4: a pause-HELD update replays and sends nothing
 * (the local pause contract), so a tick that must run the tracker phase is
 * an UNPAUSED one; the fixture's transport-only pause is restored after.
 * Output the update queued behind a full client inbox is delivered too. */
static void live_tick(struct server_local* s) {
	const bool was = s->paused;
	s->paused = false;
	tick(s);
	for(int k = 0; k < 256 && !clin_rpc_flush_pending(); k++)
		clin_update_drain();
	clin_update_drain();
	s->paused = was;
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
 * Leaves the server paused (transport only). */
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
	assert(guest_client->local_player && gstate.players[0].local_player);
	assert(s->guest_actor && s->guest_actor->finished_loading);
	assert(s->player.has_pos && s->player.finished_loading);
	return s;
}
static void teardown(struct server_local* s) {
	guest_release(s);
	s->paused = true;
	split_destroy(s);
}

/* ---- the owner-swapped sample --------------------------------------------- */
static struct server_rpc_source guest_source(const struct server_local* s) {
	return (struct server_rpc_source) {.slot = 1, .generation = s->guest_generation};
}
static struct server_rpc pose_sample(double x, double y, double z,
	bool on_ground, uint32_t seq) {
	return (struct server_rpc) {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x, .y = y, .z = z, .rx = 0.0F, .ry = 0.0F,
			.vel_x = 0.0F, .vel_y = 0.0F, .vel_z = 0.0F,
			.on_ground = on_ground, .sneaking = false, .correction_seq = seq}};
}
/* Owner 0: the host's own inbox class. */
static void host_sample(struct server_local* s, double x, double y, double z,
	bool on_ground, uint32_t seq) {
	struct server_rpc call = pose_sample(x, y, z, on_ground, seq);
	assert(svin_rpc_send(&call));
	drain_server(s);
}
/* Owner 1: the guest's tagged route. A refusal at the QUEUE boundary (switch
 * off / unadmitted) is a different test's business; here it must be queued. */
static void guest_sample(struct server_local* s, double x, double y, double z,
	bool on_ground, uint32_t seq) {
	struct server_rpc call = pose_sample(x, y, z, on_ground, seq);
	assert(svin_rpc_send_from(guest_source(s), &call));
	drain_server(s);
}
static bool near3(const float* pos, double x, double y, double z) {
	return fabsf(pos[0] - (float)x) < 1e-4f && fabsf(pos[1] - (float)y) < 1e-4f
		&& fabsf(pos[2] - (float)z) < 1e-4f;
}

/* ======================================================================== */
/* Row #4 + the respawn ticks_existed reset.                                 */
static void case_barrier(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const uint8_t zero_all = PLAYER_CORRECTION_POSITION_ONLY
		| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
		| PLAYER_CORRECTION_ZERO_VEL_Z;
	(void)zero_all;

	/* Baseline: both awake, live bodies accept a one-tick step, so every
	 * refusal below is the barrier and not a dead route. */
	double hx = h->x, gx = g->x;
	host_sample(s, hx + 0.25, h->y, h->z, true, h->correction_seq);
	check(h->x == hx + 0.25, "baseline: awake live HOST sample accepted");
	guest_sample(s, gx + 0.25, g->y, g->z, true, g->correction_seq);
	check(g->x == gx + 0.25, "baseline: awake live GUEST sample accepted");

	/* --- sleeping: server_rpc_blocked_while_sleeping(SRPC_PLAYER_POS) ----- */
	counters_reset();
	h->sleeping = true;
	hx = h->x;
	uint32_t hseq = h->correction_seq;
	host_sample(s, hx + 0.25, h->y, h->z, true, hseq);
	check(h->x == hx && h->correction_seq == hseq && host_pos_msgs == 0,
		"host: SLEEPING drops SRPC_PLAYER_POS -- body unmoved, no correction, "
		"token untouched (EntityPlayer.isMovementBlocked, EntityPlayer.java:128)");
	h->sleeping = false;

	g->sleeping = true;
	gx = g->x;
	uint32_t gseq = g->correction_seq;
	guest_sample(s, gx + 0.25, g->y, g->z, true, gseq);
	check(g->x == gx && g->correction_seq == gseq && guest_corrections == 0,
		"guest: SLEEPING drops SRPC_PLAYER_POS the same way -- body unmoved, "
		"no correction, token untouched  [row #4: RED on base]");
	g->sleeping = false;
	guest_sample(s, gx + 0.25, g->y, g->z, true, g->correction_seq);
	check(g->x == gx + 0.25, "guest: the sample after waking moves the body again");

	/* --- dead: server_rpc_blocked_while_dead(SRPC_PLAYER_POS) is FALSE ---- */
	/* Beta's dead body still runs moveEntity with zeroed input -- it falls
	 * and slides. Both owners let the corpse's physics sample through. */
	counters_reset();
	short hh = h->health;
	float hfall = h->fall_distance;
	h->health = 0;
	hx = h->x;
	double hy = h->y;
	host_sample(s, hx + 0.1, hy - 0.5, h->z, false, h->correction_seq);
	check(h->x == hx + 0.1 && h->y == hy - 0.5,
		"host: DEAD body's fall/slide sample still flows (corpse physics, "
		"deviation-#18 dwell)");
	h->health = hh; h->fall_distance = hfall; h->on_ground = true;

	short gh = g->health;
	float gfall = g->fall_distance;
	g->health = 0;
	gx = g->x;
	double gy = g->y;
	guest_sample(s, gx + 0.1, gy - 0.5, g->z, false, g->correction_seq);
	check(g->x == gx + 0.1 && g->y == gy - 0.5,
		"guest: DEAD body's fall/slide sample flows identically -- the dead "
		"leg is host parity, NOT a drop");
	check(guest_corrections == 0, "guest: no correction authored for the corpse sample");
	g->health = gh; g->fall_distance = gfall; g->on_ground = true;

	/* --- respawn plan in flight: the host blocks SRPC_PLAYER_POS for every
	 * non-IDLE host_respawn_phase (server_local_host_respawn_active). Reach
	 * the guest's PREPARED phase through the production path: a REAL lethal
	 * hit, then SRPC_RESPAWN whose inventory close is made to fail once (the
	 * crafting-input drop needs an entity allocation; test_local_guest's own
	 * PREPARED hold), so the plan is recorded but LOADING has not begun. --- */
	/* G8-BLAST-VELOCITY v3 lifetime L1 (ASTRA-V2-REVIEW "TEST lifetime"):
	 * one real update drains every queued sample event, then BOTH owners
	 * take a fresh surviving hit (flags pending, nothing sent at the hit).
	 * The guest then dies for real below and respawns through the real
	 * plan; the host's pending send must stay correct across it. */
	tick(s);
	check(h->motion_q_n == 0 && g->motion_q_n == 0,
		"L1 setup: one real update replayed every queued movement event");
	vel_reset();
	h->hurt_resist_time = g->hurt_resist_time = 0;
	h->last_damage = g->last_damage = 0;
	h->vel_x = 0.1; h->motion_y = 0.0; h->vel_z = 0.0;
	g->vel_x = 0.2; g->motion_y = -0.1; g->vel_z = 0.06;
	{
		bool hacc = false, gacc = false;
		(void)server_local_damage_actor_result(s, h, 16, 0.0f, 1.0f, &hacc);
		(void)server_local_damage_actor_result(s, g, 16, 1.0f, 0.0f, &gacc);
		clin_update_drain();
		check(hacc && gacc && h->been_attacked && g->been_attacked
			&& vel_msgs[0] == 0 && vel_msgs[1] == 0,
			"L1 setup: both owners took a fresh hit; flags pending, no send at the hit");
	}
	/* knockBack (EntityLiving:392-403): kz 1 on the host, kx 1 on the guest. */
	const double l1_hx = 0.1 / 2.0, l1_hz = 0.0 / 2.0 + (double)KNOCKBACK_H;
	const double l1_hy = fmin(0.0 / 2.0 + (double)KNOCKBACK_V, (double)KNOCKBACK_V);
	check(h->vel_x == l1_hx && h->motion_y == l1_hy && h->vel_z == l1_hz
		&& g->vel_x == 0.2 / 2.0 + (double)KNOCKBACK_H
		&& g->vel_z == 0.06 / 2.0,
		"L1 setup: both server motions carry their own knockBack");
	g->ticks_existed = 77; /* the corpse's count, to be dropped at respawn */
	g->hurt_resist_time = 0;
	bool accepted = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH, 0.0f, 0.0f,
		&accepted);
	clin_update_drain();
	check(accepted && g->health == 0 && g->has_death_pos,
		"setup: guest killed through the production damage path (health 0, "
		"death position recorded)");
	check(s->guest_respawn_phase == GUEST_RESPAWN_IDLE,
		"setup: no plan yet -- the corpse's samples still flow (IDLE + dead)");
	check(!g->been_attacked && g->motion_q_n == 0 && g->vel_x != 0.0
		&& h->been_attacked,
		"L1: the lethal commit cleared the guest flag/events (the corpse keeps "
		"its motion); the host flag is still pending");
	counters_reset();
	gx = g->x;
	guest_sample(s, gx + 0.1, g->y, g->z, true, g->correction_seq);
	check(g->x == gx + 0.1 && guest_corrections == 0,
		"guest: IDLE-phase corpse sample flows (host parity for a dead body "
		"with no respawn plan)");

	g->inventory.items[INVENTORY_SLOT_CRAFTING]
		= (struct item_data) {.id = ITEM_COAL, .count = 1};
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation,
			.respawn_epoch = 0}};
	server_local_test_fail_next_entity_alloc();
	assert(svin_rpc_send_from(guest_source(s), &respawn));
	drain_server(s);
	check(s->guest_respawn_phase == GUEST_RESPAWN_PREPARED && g->health == 0
		&& g->finished_loading && !g->reject_pos_this_update,
		"setup: plan PREPARED (close failed once), body still dead, still "
		"finished_loading, no per-update barrier raised");
	uint64_t epoch = s->guest_respawn_epoch;
	double death_x = s->guest_respawn_death_x;
	counters_reset();
	gx = g->x;
	gseq = g->correction_seq;
	guest_sample(s, gx + 0.1, g->y, g->z, true, gseq);
	check(g->x == gx && g->correction_seq == gseq && guest_corrections == 0,
		"guest: PREPARED respawn plan DROPS the old-body sample -- the corpse "
		"cannot walk away from its recorded death position "
		"(host: server_local_host_respawn_active blocks PLAYER_POS)  "
		"[row #4: RED on base]");
	check(s->guest_respawn_death_x == death_x,
		"guest: the recorded death position is unchanged by the refused sample");

	/* Retry -> LOADING (the coal drops for real this time). */
	respawn.payload.respawn.respawn_epoch = 0;
	assert(svin_rpc_send_from(guest_source(s), &respawn));
	drain_server(s);
	check(s->guest_respawn_phase == GUEST_RESPAWN_LOADING
		&& s->guest_respawn_epoch == epoch && !g->finished_loading,
		"setup: retry reached LOADING on the same epoch");
	gx = g->x;
	guest_sample(s, gx + 0.1, g->y, g->z, true, g->correction_seq);
	check(g->x == gx, "guest: LOADING phase still drops the old-body sample");

	/* Exact READY ack from the guest route. An exact READY commits INLINE when
	 * the destination column is already published (respawn_ready -> poll), so
	 * unpublish it first to observe the latch, then publish + poll to commit
	 * (test_local_guest's own early-ACK recipe). */
	w_coord_t rcx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x));
	w_coord_t rcz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z));
	if(!server_world_is_chunk_loaded(&s->worlds[0], rcx, rcz)) {
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(&s->worlds[0], rcx, rcz, &loaded));
	}
	struct server_chunk* destination
		= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(rcx, rcz));
	assert(destination);
	destination->sent = false;
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket,
			.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
			.generation = s->guest_generation, .respawn_epoch = epoch}};
	assert(svin_rpc_send_from(guest_source(s), &ready));
	drain_server(s);
	check(s->guest_respawn_ready && g->health == 0
		&& s->guest_respawn_phase == GUEST_RESPAWN_LOADING,
		"setup: exact READY latched without premature revival "
		"(destination column not yet published)");
	destination->sent = true;
	server_local_guest_respawn_poll(s);
	clin_update_drain();
	check(g->health == MAX_PLAYER_HEALTH && g->finished_loading
		&& s->guest_respawn_phase == GUEST_RESPAWN_IDLE,
		"setup: guest respawn COMMITTED (fresh body, plan retired)");
	check(g->ticks_existed == 0,
		"guest respawn: the fresh body starts at ticks_existed == 0 "
		"(Entity.java:51; host W1F parity)  [RED on base: inherits the corpse's 77]");
	check(g->vel_x == 0.0 && g->motion_y == 0.0 && g->vel_z == 0.0
		&& !g->been_attacked && g->motion_q_n == 0 && !g->in_web,
		"L1: the respawned guest body carries NO old motion, flag or event "
		"(fresh EntityPlayer)");
	check(vel_msgs[0] == 0 && vel_msgs[1] == 0,
		"L1: nothing was sent through the whole death/respawn transaction");

	/* The LOADING transition raised the per-update input latch; production
	 * retires it at the next update boundary for EVERY body
	 * (server_local.c, the reject_pos_this_update clears ahead of the pause
	 * gate). Until then the fresh body's samples are refused; after ONE real
	 * update the new token moves it again. */
	check(g->reject_pos_this_update,
		"guest: the fresh body's per-update latch is still raised at commit");
	gx = g->x;
	guest_sample(s, gx + 0.25, g->y, g->z, true, g->correction_seq);
	check(g->x == gx, "guest: a sample inside that update is refused");
	tick(s);
	check(!g->reject_pos_this_update && g->health == MAX_PLAYER_HEALTH,
		"guest: one real update retires the latch and leaves the body alive");
	/* v4: that update was pause-HELD (the fixture is paused): the host's
	 * pending flag survives it and nothing is sent. */
	check(s->paused && vel_msgs[0] == 0 && vel_msgs[1] == 0 && h->been_attacked,
		"L1: a pause-held update keeps the host's pending flag, sends nothing");
	live_tick(s);
	/* L1 delivery: the first SIMULATING update is the tracker phase for the
	 * host's pending flag. Zero host events were queued, so the payload is
	 * exactly Q(knockBack motion); the guest got nothing. */
	check(vel_msgs[0] == 1 && !vel_last_add[0]
		&& vel_last[0][0] == q28(l1_hx) && vel_last[0][1] == q28(l1_hy)
		&& vel_last[0][2] == q28(l1_hz) && !h->been_attacked,
		"L1: the host's pending send stayed correct across the guest's "
		"death + respawn (one absolute Q(knockBack) message)");
	check(vel_msgs[1] == 0 && g->vel_x == 0.0 && g->motion_y == 0.0,
		"L1: no velocity ever reached the respawned guest");
	/* The fresh body's first fresh hit is computed from ZERO motion (an
	 * old 0.5 X motion would give 0.65 here). */
	{
		bool gacc = false;
		g->hurt_resist_time = 0;
		g->last_damage = 0;
		(void)server_local_damage_actor_result(s, g, 16, 1.0f, 0.0f, &gacc);
		live_tick(s);
		check(gacc && vel_msgs[1] == 1 && !vel_last_add[1]
			&& vel_last[1][0] == q28((double)KNOCKBACK_H)
			&& vel_last[1][1] == q28((double)KNOCKBACK_V)
			&& vel_last[1][2] == 0.0F && vel_msgs[0] == 1,
			"L1: the respawned guest's first hit sends Q(knockBack from zero), "
			"owner 1 only");
	}
	gx = g->x;
	guest_sample(s, gx + 0.25, g->y, g->z, true, g->correction_seq);
	check(g->x == gx + 0.25, "guest: the respawned body accepts its next sample");

	/* L2 (admission lifetime): the guest LEAVES with a pending flag, server
	 * motion and a queued movement event; a NEW guest is admitted through
	 * the real Join/gameplay handshake. Nothing of the old body may reach
	 * the new one; the host's pending send is delivered exactly once. */
	tick(s); /* replay the sample just accepted */
	vel_reset();
	h->hurt_resist_time = g->hurt_resist_time = 0;
	h->last_damage = g->last_damage = 0;
	h->vel_x = -0.2; h->motion_y = 0.1; h->vel_z = 0.0;
	{
		bool hacc = false, gacc = false;
		(void)server_local_damage_actor_result(s, h, 16, 1.0f, 0.0f, &hacc);
		(void)server_local_damage_actor_result(s, g, 16, 0.0f, -1.0f, &gacc);
		check(hacc && gacc && h->been_attacked && g->been_attacked,
			"L2 setup: both owners hit fresh, flags pending");
	}
	gx = g->x;
	s->paused = false; /* v4: a paused sample is no physics event */
	guest_sample(s, gx + 0.1, g->y, g->z, true, g->correction_seq);
	s->paused = true;
	check(g->motion_q_n == 1, "L2 setup: the old guest has one queued event");
	const double l2_hx = -0.2 / 2.0 + (double)KNOCKBACK_H;
	const double l2_hy = fmin(0.1 / 2.0 + (double)KNOCKBACK_V, (double)KNOCKBACK_V);
	const double l2_hz = 0.0;
	const uint64_t old_generation = s->guest_generation;
	guest_release(s);
	guest_attach(s);
	guest_body_home(s);
	assert(client_session_request_local_gameplay(&gstate.players[1]));
	pump_handshake(s);
	struct server_player* g2 = s->guest_actor;
	check(g2 && s->guest_gameplay && g2->finished_loading
		&& s->guest_generation != old_generation,
		"L2 setup: a NEW guest admitted through the real handshake");
	check(g2 && g2->vel_x == 0.0 && g2->motion_y == 0.0 && g2->vel_z == 0.0
		&& !g2->been_attacked && g2->motion_q_n == 0,
		"L2: the admitted body starts with no motion, flag or queued event");
	live_tick(s);
	check(vel_msgs[1] == 0,
		"L2: the departed guest's pending velocity never reaches the new guest");
	check(vel_msgs[0] == 1 && !vel_last_add[0]
		&& vel_last[0][0] == q28(l2_hx) && vel_last[0][1] == q28(l2_hy)
		&& vel_last[0][2] == q28(l2_hz) && !h->been_attacked,
		"L2: the host's pending send is delivered once, exactly Q(knockBack), "
		"across the guest replacement");
	live_tick(s);
	check(vel_msgs[0] == 1 && vel_msgs[1] == 0,
		"L2: no repeat on either route");

	/* L3 REAL host respawn (the admitted same-world transaction:
	 * SRPC_RESPAWN owner 0 -> LOADING -> exact epoch READY -> commit). The
	 * host dies with motion on its corpse while the NEW guest has a pending
	 * flag; the guest's send must arrive once and exactly, the fresh host
	 * body must carry nothing of the corpse. */
	g = g2;
	vel_reset();
	h->hurt_resist_time = g->hurt_resist_time = 0;
	h->last_damage = g->last_damage = 0;
	g->vel_x = 0.0; g->motion_y = 0.0; g->vel_z = 0.12;
	{
		bool hacc = false, gacc = false;
		(void)server_local_damage_actor_result(s, h, 16, 1.0f, 0.0f, &hacc);
		(void)server_local_damage_actor_result(s, g, 16, 0.0f, 1.0f, &gacc);
		check(hacc && gacc && h->been_attacked && g->been_attacked,
			"L3 setup: both owners hit fresh, flags pending");
		bool lethal = false;
		h->hurt_resist_time = 0;
		(void)server_local_damage_actor_result(s, h, MAX_PLAYER_HEALTH, 0.0f,
			0.0f, &lethal);
		clin_update_drain();
		printf("     L3 after lethal: accepted %d health %d flag %d q %d vel_x %g "
			   "guest flag %d phase %d\n", (int)lethal, (int)h->health,
			   (int)h->been_attacked, (int)h->motion_q_n, h->vel_x,
			   (int)g->been_attacked, (int)s->host_respawn_phase);
		check(lethal && h->health <= 0 && !h->been_attacked
			&& h->motion_q_n == 0 && h->vel_x != 0.0 && g->been_attacked,
			"L3 setup: host killed for real (flag cleared, corpse keeps its "
			"motion); the guest flag is still pending");
	}
	const double l3_gx = 0.0, l3_gy = (double)KNOCKBACK_V;
	const double l3_gz = 0.12 / 2.0 + (double)KNOCKBACK_H;
	h->spawn_x = (int)floor(h->x);
	h->spawn_y = (int)floor(h->y - PLAYER_EYE_HEIGHT);
	h->spawn_z = (int)floor(h->z);
	struct server_rpc host_respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 0, .generation = 0, .respawn_epoch = 0}};
	assert(svin_rpc_send(&host_respawn));
	drain_server(s);
	printf("     L3 after request: phase %d health %d epoch %llu\n",
		(int)s->host_respawn_phase, (int)h->health,
		(unsigned long long)s->host_respawn_epoch);
	check(s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING && h->health <= 0,
		"L3 setup: host respawn plan reached LOADING (no world switch)");
	host_respawn.payload.respawn.respawn_epoch = s->host_respawn_epoch;
	assert(svin_rpc_send(&host_respawn));
	drain_server(s);
	server_local_host_respawn_poll(s);
	clin_update_drain();
	check(h->health == MAX_PLAYER_HEALTH
		&& s->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE,
		"L3 setup: host respawn COMMITTED through the real transaction");
	check(h->vel_x == 0.0 && h->motion_y == 0.0 && h->vel_z == 0.0
		&& !h->been_attacked && h->motion_q_n == 0,
		"L3: the respawned host body carries NO old motion, flag or event");
	check(vel_msgs[0] == 0 && vel_msgs[1] == 0,
		"L3: nothing sent during the host death/respawn transaction");
	tick(s);
	check(vel_msgs[1] == 1 && !vel_last_add[1]
		&& vel_last[1][0] == q28(l3_gx) && vel_last[1][1] == q28(l3_gy)
		&& vel_last[1][2] == q28(l3_gz) && !g->been_attacked,
		"L3: the guest's pending send stayed correct across the host respawn");
	check(vel_msgs[0] == 0, "L3: nothing reached the respawned host");
	{
		bool hacc = false;
		h->hurt_resist_time = 0;
		h->last_damage = 0;
		(void)server_local_damage_actor_result(s, h, 16, 1.0f, 0.0f, &hacc);
		tick(s);
		check(hacc && vel_msgs[0] == 1 && !vel_last_add[0]
			&& vel_last[0][0] == q28((double)KNOCKBACK_H)
			&& vel_last[0][1] == q28((double)KNOCKBACK_V)
			&& vel_last[0][2] == 0.0F && vel_msgs[1] == 1,
			"L3: the respawned host's first hit sends Q(knockBack from zero)");
	}

	teardown(s);
}

/* ======================================================================== */
/* Row #25: knockback -- shared server integrator, documented client envelope. */
static void case_v4_routes(struct server_local* s); /* G8-BLAST-VELOCITY v4 */
static void case_knockback(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct entity* hbody = gstate.players[0].local_player;
	struct entity* gbody = gstate.players[1].local_player;
	assert(hbody && gbody);
	/* Isolate the matched knockBack steps from ordinary body collision.
	 * Bind now registers the guest physically (Astra option A); overlapping
	 * this fixture's bodies lets EntityLiving.onLivingUpdate's push loop
	 * alter the motion after friction. The v4 cases below test that push. */
	g->x = h->x + 4.0;
	server_local_refresh_guest_membership(s);

	/* Both clients are mid-fall with some horizontal momentum of their own,
	 * so the envelope's effect on client velocity is observable. */
	hbody->vel[0] = 0.3F; hbody->vel[1] = -0.5F; hbody->vel[2] = 0.05F;
	gbody->vel[0] = 0.3F; gbody->vel[1] = -0.5F; gbody->vel[2] = 0.05F;

	/* The SAME fresh flat hit on both bodies, knockback direction +x. */
	h->hurt_resist_time = 0; g->hurt_resist_time = 0;
	h->vel_x = h->vel_z = 0.0; g->vel_x = g->vel_z = 0.0;
	h->motion_y = g->motion_y = 0.0; /* matched server states */
	bool hacc = false, gacc = false;
	(void)server_local_damage_actor_result(s, h, 1, 1.0f, 0.0f, &hacc);
	(void)server_local_damage_actor_result(s, g, 1, 1.0f, 0.0f, &gacc);
	clin_update_drain();
	check(hacc && gacc && h->health == MAX_PLAYER_HEALTH - 1
		&& g->health == MAX_PLAYER_HEALTH - 1,
		"setup: both owners took the same fresh hit");
	check(h->vel_x == (double)KNOCKBACK_H && h->vel_z == 0.0
		&& g->vel_x == (double)KNOCKBACK_H && g->vel_z == 0.0
		&& h->motion_y == (double)KNOCKBACK_V && g->motion_y == (double)KNOCKBACK_V,
		"setup: both server bodies carry the same Beta knockBack motion "
		"(EntityLiving.knockBack)");

	double hx0 = h->x, gx0 = g->x;
	uint32_t hseq0 = h->correction_seq, gseq0 = g->correction_seq;
	/* v3: the hit itself delivers nothing -- both clients keep their own
	 * pre-hit motion until the tracker phase. */
	check(hbody->vel[0] == 0.3F && hbody->vel[1] == -0.5F
		&& hbody->vel[2] == 0.05F && gbody->vel[0] == 0.3F
		&& gbody->vel[1] == -0.5F && gbody->vel[2] == 0.05F
		&& h->been_attacked && g->been_attacked,
		"client: both bodies keep their motion until the tracker delivery");
	counters_reset();
	const bool ground0 = h->on_ground;
	server_local_tick_actor_knockback(s); /* one accepted sample each */
	server_local_flush_actor_velocity(s); /* the tracker phase */
	clin_update_drain();
	{
		/* Independent Beta step (EntityLiving.moveEntityWithHeading), upward
		 * Y collision-free, then Packet28 Q(v). */
		const double k = (double)(ground0 ? 0.6F * 0.91F : 0.91F);
		const double ex = (double)KNOCKBACK_H * k;
		const double ey = ((double)KNOCKBACK_V - 0.08) * (double)0.98F;
		const float qx = (float)(int32_t)(ex * 8000.0) / 8000.0f;
		const float qy = (float)(int32_t)(ey * 8000.0) / 8000.0f;
		check(hbody->vel[0] == qx && hbody->vel[1] == qy
			&& hbody->vel[2] == 0.0F && gbody->vel[0] == qx
			&& gbody->vel[1] == qy && gbody->vel[2] == 0.0F,
			"client: both bodies installed the SAME Q(post-step XYZ) on their own routes");
		check(!h->been_attacked && !g->been_attacked,
			"server: both flags cleared by the tracker phase");
	}

	/* Server side: owner-swap EQUIVALENCE (G8: Beta's EntityPlayerMP keeps
	 * the client-sent position, NetServerHandler.java:172/257; only its
	 * motion decays). */
	check(h->x == hx0 && g->x == gx0,
		"server: the integrator moves neither body (client-integrated push)");
	const double k = (double)(h->on_ground ? 0.6F * 0.91F : 0.91F);
	check(h->on_ground == g->on_ground && h->vel_x == g->vel_x
		&& h->vel_x == (double)KNOCKBACK_H * k && h->motion_y == g->motion_y,
		"server: both motions follow the same Beta step");
	check(h->correction_seq == hseq0 && g->correction_seq == gseq0,
		"server: neither correction token advances");
	check(host_pos_msgs == 0 && guest_corrections == 0,
		"client: no CRPC_PLAYER_POS / LOCAL_GUEST_CORRECTION snap for a push");
	check(gbody->vel[0] == hbody->vel[0] && hbody->vel[0] > 0.0F,
		"client: the delivered horizontal motion survives the server tick");

	/* FALLING guest: a later tick reaches a client whose own gravity turned
	 * motionY negative and whose stick changed its horizontal motion. With
	 * no correction both survive (the player steers through the push). */
	gbody->vel[0] = 0.12F; gbody->vel[1] = -0.35F; gbody->vel[2] = -0.02F;
	hbody->vel[1] = -0.35F;
	counters_reset();
	server_local_tick_actor_knockback(s);
	server_local_flush_actor_velocity(s); /* no new fresh hit: no send */
	clin_update_drain();
	check(g->x == gx0 && guest_corrections == 0 && host_pos_msgs == 0,
		"guest: the second push tick sends no correction either");
	check(gbody->vel[0] == 0.12F && gbody->vel[1] == -0.35F
		&& gbody->vel[2] == -0.02F,
		"guest client: its own fall AND steering survive (no server override)");
	check(hbody->vel[1] == -0.35F, "host client: the fall is kept");

	/* Token discipline, both owners: the token never moved, so the client's
	 * own samples during the push are accepted (client-authoritative, as
	 * Beta's handleFlying). */
	double hx1 = h->x, gx1 = g->x;
	host_sample(s, hx1 + 0.1, h->y, h->z, true, hseq0);
	check(h->x == hx1 + 0.1, "host: its sample during the push is accepted");
	counters_reset();
	guest_sample(s, gx1 + 0.1, g->y, g->z, true, gseq0);
	check(g->x == gx1 + 0.1 && guest_corrections == 0,
		"guest: its sample during the push is accepted");

	/* G8-BLAST-VELOCITY v4: the per-route witnesses on this fixture. */
	case_v4_routes(s);

	teardown(s);
}

/* ======================================================================== */
/* G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW) on BOTH actor routes, through real
 * (unpaused) updates of the admitted fixture: the guest's own push loop per
 * accepted packet (0/1/2 samples, mixed hit), arrow knockBack from the
 * shooter's CURRENT pose (player shooters both ways, a moved skeleton, a
 * dispenser's null owner; each with an ABSORBED blast), the landing +
 * same-update arrow ordering, and the guest's pause/resume replay. Expected
 * payloads are independent Beta transcriptions (below), consumed from the
 * production route (owner 0 = host, 1 = guest). */
static double v4_wit_y(double v) { return (v - 0.08) * (double)0.98F; }
/* S/Entity.java:630-655 (this = pushed, mover = the loop's body): the
 * mover's (+) half. */
static bool v4_collision(double this_x, double this_z, double mover_x,
						 double mover_z, double* hx, double* hz) {
	double var2 = mover_x - this_x, var4 = mover_z - this_z;
	double a2 = var2 < 0.0 ? -var2 : var2, a4 = var4 < 0.0 ? -var4 : var4;
	double var6 = a2 > a4 ? a2 : a4;
	if(!(var6 >= (double)0.01F))
		return false;
	var6 = (double)(float)sqrt(var6);
	var2 /= var6;
	var4 /= var6;
	double var8 = 1.0 / var6;
	if(var8 > 1.0)
		var8 = 1.0;
	*hx = var2 * var8 * (double)0.05F;
	*hz = var4 * var8 * (double)0.05F;
	return true;
}
/* S/EntityLiving.java:321-332 + knockBack:385-405, attacker at (ax, az). */
static void v4_knockback(double m[3], double ax, double az, double vx,
						 double vz) {
	const double dx = ax - vx, dz = az - vz;
	const float len = (float)sqrt(dx * dx + dz * dz);
	m[0] = m[0] / 2.0 - dx / (double)len * (double)0.4F;
	m[1] = fmin(m[1] / 2.0 + (double)0.4F, (double)0.4F);
	m[2] = m[2] / 2.0 - dz / (double)len * (double)0.4F;
}
/* Open-air doExplosionA:87-103 impulse/damage for a player eye. */
static int v4_blast(const vec3 c, double ex, double ey, double ez, double imp[3]) {
	double dx = ex - (double)c[0], dy = ey - (double)c[1], dz = ez - (double)c[2];
	double dist = (double)(float)sqrt(dx * dx + dy * dy + dz * dz);
	double var41 = 1.0 - dist / 8.0;
	imp[0] = dx / dist * var41;
	imp[1] = dy / dist * var41;
	imp[2] = dz / dist * var41;
	return (int)((var41 * var41 + var41) / 2.0 * 8.0 * 8.0 + 1.0);
}
/* One packet's moveEntityWithHeading on the superflat floor (feet on the
 * floor top, open air otherwise): a downward move is clipped to 0 (motionY
 * zeroed), then (motionY - 0.08) * 0.98F; X/Z friction from the PREVIOUS
 * packet's onGround (0.6F * 0.91F on grass, else 0.91F). Returns the box's
 * X/Z travel through *tx / *tz. */
static void v4_floor_step(double m[3], bool prev_ground, double* tx, double* tz) {
	*tx = m[0];
	*tz = m[2];
	if(m[1] < 0.0)
		m[1] = 0.0;
	m[1] = v4_wit_y(m[1]);
	const double k = (double)(prev_ground ? 0.6F * 0.91F : 0.91F);
	m[0] *= k;
	m[2] *= k;
}
static struct entity* v4_frozen(struct server_local* s, float x, float y,
								float z, int id) {
	struct entity* m = server_local_spawn_monster_at_w(&s->worlds[0],
		(vec3) {x, y, z}, id);
	assert(m);
	m->tick_server = NULL; /* no AI, no shot, no own push loop */
	m->pos[0] = x; m->pos[1] = y; m->pos[2] = z;
	m->data.monster.motion_d.pos[0] = (double)x;
	m->data.monster.motion_d.pos[1] = (double)y;
	m->data.monster.motion_d.pos[2] = (double)z;
	server_local_refresh_entity_membership(s, m);
	return m;
}
static struct entity* v4_arrow;
static struct server_player* v4_victim;
static bool v4_blast_on, v4_fired;
static vec3 v4_center;
static void v4_hook(struct server_local* sv, struct server_world* w, int phase) {
	if(v4_fired || phase != 1 || w != &sv->worlds[0])
		return;
	v4_fired = true;
	if(v4_arrow)
		check(v4_arrow->tick_server(v4_arrow, sv), "v4: the arrow hit and was removed");
	else if(v4_victim) /* a fresh null-attacker hit: the flag, no knockBack */
		(void)server_local_damage_actor(sv, v4_victim, 8, 0.0F, 0.0F);
	if(v4_blast_on)
		server_world_explode(w, v4_center, 4.0F); /* absorbed by the window */
}
/* One UNPAUSED real update with the entity-phase hook armed. */
static void v4_live_tick(struct server_local* s) {
	const bool was = s->paused;
	s->paused = false;
	v4_fired = false;
	server_local_test_phase_hook = v4_hook;
	tick(s);
	server_local_test_phase_hook = NULL;
	/* A blast floods the client inbox; the update's own output (the one
	 * velocity message among it) may wait in the server's pending queue --
	 * transport, not schedule. Deliver it all before observing. */
	for(int k = 0; k < 256 && !clin_rpc_flush_pending(); k++)
		clin_update_drain();
	clin_update_drain();
	s->paused = was;
}
static void v4_reset_body(struct server_player* b, double vx, double my,
						  double vz) {
	b->hurt_resist_time = 0;
	b->last_damage = 0;
	b->damage_remainder = 0;
	b->health = MAX_PLAYER_HEALTH;
	b->vel_x = vx;
	b->motion_y = my;
	b->vel_z = vz;
	b->been_attacked = false;
	b->in_web = false;
}
/* Place both bodies with one accepted sample each (unpaused, so they are
 * physics events) and replay them in one live update. */
static void v4_place(struct server_local* s, double hx, double hz, double gx,
					 double gz) {
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double y = 4.0 + (double)PLAYER_EYE_HEIGHT;
	const bool was = s->paused;
	s->paused = false;
	/* small hops (the guest delta clamp), one body at a time, never through
	 * the other one: the body standing on the other's target moves first. */
	const bool host_first = fabs(gx - h->x) < 1.5 && fabs(gz - h->z) < 1.5;
	for(int pass = 0; pass < 2; pass++) {
		const bool move_host = (pass == 0) == host_first;
		const double x0 = move_host ? h->x : g->x, z0 = move_host ? h->z : g->z;
		const double x1 = move_host ? hx : gx, z1 = move_host ? hz : gz;
		for(int k = 0; k < 8; k++) {
			const double t = (k + 1) / 8.0;
			if(move_host)
				host_sample(s, x0 + (x1 - x0) * t, y, z0 + (z1 - z0) * t, true,
							h->correction_seq);
			else
				guest_sample(s, x0 + (x1 - x0) * t, y, z0 + (z1 - z0) * t, true,
							 g->correction_seq);
		}
	}
	s->paused = was;
	v4_arrow = NULL;
	v4_victim = NULL;
	v4_blast_on = false;
	v4_live_tick(s);
	printf("     v4 place: host (%.4f, %.4f) q %d guest (%.4f, %.4f) q %d\n",
		   h->x, h->z, (int)h->motion_q_n, g->x, g->z, (int)g->motion_q_n);
	check(fabs(h->x - hx) < 1e-9 && fabs(h->z - hz) < 1e-9
		&& fabs(g->x - gx) < 1e-9 && fabs(g->z - gz) < 1e-9
		&& h->motion_q_n == 0 && g->motion_q_n == 0,
		"v4 setup: both bodies placed, their events replayed");
}
static void case_v4_routes(struct server_local* s) {
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double Y = 4.0 + (double)PLAYER_EYE_HEIGHT;
	struct block_data floor_b;
	assert(server_world_get_block(&s->worlds[0], 4, 3, 4, &floor_b));
	check(floor_b.type == BLOCK_GRASS || floor_b.type == BLOCK_DIRT,
		"v4 setup: the floor is grass/dirt (slipperiness 0.6F)");

	/* ---- G1: the GUEST's own push loop per accepted packet ---------------
	 * A never-ticking zombie 0.55 +x of the guest. N stationary guest
	 * samples, a fresh null hit (no knockBack) in the entity phase. */
	for(int n = 0; n <= 2; n++) {
		v4_place(s, 12.5, 12.5, 4.5, 4.5);
		struct entity* zom = v4_frozen(s, 5.05F, 4.0F, 4.5F, MONSTER_ZOMBIE);
		v4_reset_body(g, 0.1, 0.0, 0.0);
		v4_reset_body(h, 0.0, 0.0, 0.0);
		g->on_ground = true;
		const bool was = s->paused;
		s->paused = false;
		for(int k = 0; k < n; k++)
			guest_sample(s, 4.5, Y, 4.5, true, g->correction_seq);
		s->paused = was;
		check(g->motion_q_n == n, "G1 setup: one physics event per guest sample");
		vel_reset();
		v4_victim = g;
		v4_live_tick(s);
		v4_victim = NULL;
		double m[3] = {0.1, 0.0, 0.0};
		int pushes = 0;
		for(int k = 0; k < n; k++) {
			double tx, tz;
			v4_floor_step(m, true, &tx, &tz);
			const double cx = 4.5 + tx, cz = 4.5 + tz;
			double hx, hz;
			if(fabs(cx - (double)zom->pos[0]) < 0.3 + (double)0.2F
				   + (double)(zom->width / 2.0F)
			   && v4_collision((double)zom->pos[0], (double)zom->pos[2], cx, cz,
							   &hx, &hz)) {
				m[0] += hx;
				m[2] += hz;
				pushes++;
			}
		}
		printf("     G1 guest push n=%d: pushes %d msgs %u/%u v (%.6f %.6f %.6f) want (%.6f %.6f %.6f)\n",
			   n, pushes, vel_msgs[0], vel_msgs[1], (double)vel_last[1][0],
			   (double)vel_last[1][1], (double)vel_last[1][2], (double)q28(m[0]),
			   (double)q28(m[1]), (double)q28(m[2]));
		check(pushes == n, "G1: the witness geometry pushes on every guest packet");
		check(vel_msgs[1] == 1 && vel_msgs[0] == 0 && !vel_last_add[1]
			&& vel_last[1][0] == q28(m[0]) && vel_last[1][1] == q28(m[1])
			&& vel_last[1][2] == q28(m[2]) && g->vel_x == m[0]
			&& g->motion_y == m[1] && g->vel_z == m[2],
			"G1: guest payload = its packets' steps, each own push added after "
			"that packet's friction; zero packets = no own push");
		dict_entity_erase(s->entities, zom->id);
		free(zom);
	}

	/* ---- G2: arrow knockBack from the shooter's CURRENT pose, an ABSORBED
	 * blast, one guest packet after the hit (both routes) ------------------ */
	enum { SH_HOST, SH_GUEST, SH_SKELETON, SH_DISPENSER };
	static const char* const sh_name[] = {"host shoots guest",
		"guest shoots host", "moved skeleton -> guest", "dispenser -> guest"};
	for(int kind = SH_HOST; kind <= SH_DISPENSER; kind++) {
		/* shooter off the +z flight line; the victim at (4.5, 0.5) */
		const bool victim_is_host = kind == SH_GUEST;
		if(victim_is_host)
			v4_place(s, 4.5, 4.5, 1.5, 9.0);
		else
			v4_place(s, 1.5, 12.5, 4.5, 4.5);
		struct server_player* v = victim_is_host ? h : g;
		struct server_player* other = victim_is_host ? g : h;
		v4_reset_body(h, 0.0, 0.0, 0.0);
		v4_reset_body(g, 0.0, 0.0, 0.0);
		v->vel_x = 0.04;
		v->on_ground = true;
		struct entity* skel = NULL;
		double sx = other->x, sz = other->z;
		if(kind == SH_SKELETON) {
			skel = v4_frozen(s, 8.5F, 4.0F, 12.5F, MONSTER_SKELETON);
			/* moved since the shot: its CURRENT pose counts */
			skel->pos[0] = 1.25F;
			skel->pos[2] = 9.75F;
			skel->data.monster.motion_d.pos[0] = 1.25;
			skel->data.monster.motion_d.pos[2] = 9.75;
			server_local_refresh_entity_membership(s, skel);
			sx = 1.25;
			sz = 9.75;
		}
		struct entity* arrow = calloc(1, sizeof *arrow);
		assert(arrow);
		entity_arrow(910001u + (uint32_t)kind, arrow, true, &s->worlds[0],
			kind == SH_SKELETON ? skel->id
				: kind == SH_DISPENSER ? DISPENSER_PROJECTILE_OWNER_SENTINEL : 0u,
			ITEM_ARROW, ARROW_DAMAGE, kind != SH_SKELETON);
		arrow->data.arrow.from_skeleton = kind == SH_SKELETON;
		if(kind == SH_HOST || kind == SH_GUEST)
			arrow->data.arrow.shooter_actor = server_local_actor_ident(s, other);
		arrow->teleport(arrow, (vec3) {4.5F, (float)Y - 0.85F, 4.5F - 1.5F});
		glm_vec3_copy((vec3) {0.0F, 0.0F, 2.0F}, arrow->vel);
		/* absorbed blast 7.6 off the victim's eye (damage 16 <= 32) */
		v4_center[0] = 4.5F + 7.6F;
		v4_center[1] = 5.0F;
		v4_center[2] = 4.5F;
		double imp[3];
		const int bdmg = v4_blast(v4_center, 4.5, Y, 4.5, imp);
		check(bdmg * 8 <= ARROW_DAMAGE, "G2 setup: the blast is inside the arrow's window");
		const bool was = s->paused;
		s->paused = false;
		if(victim_is_host)
			host_sample(s, 4.5, Y, 4.5, true, h->correction_seq);
		else
			guest_sample(s, 4.5, Y, 4.5, true, g->correction_seq);
		s->paused = was;
		vel_reset();
		v4_arrow = arrow;
		v4_blast_on = true;
		v4_live_tick(s);
		v4_arrow = NULL;
		v4_blast_on = false;
		free(arrow);
		double m[3] = {0.04, 0.0, 0.0};
		if(kind != SH_DISPENSER)
			v4_knockback(m, sx, sz, 4.5, 4.5); /* NOT the +z heading */
		m[0] += imp[0];
		m[1] += imp[1];
		m[2] += imp[2];
		double tx, tz;
		v4_floor_step(m, true, &tx, &tz);
		const unsigned o = victim_is_host ? 0u : 1u;
		printf("     G2 %s: health %d msgs %u/%u v (%.6f %.6f %.6f) want (%.6f %.6f %.6f)\n",
			   sh_name[kind], (int)v->health, vel_msgs[0], vel_msgs[1],
			   (double)vel_last[o][0], (double)vel_last[o][1],
			   (double)vel_last[o][2], (double)q28(m[0]), (double)q28(m[1]),
			   (double)q28(m[2]));
		check(v->health == MAX_PLAYER_HEALTH - ARROW_DAMAGE,
			"G2: the arrow is the fresh hit, the blast absorbed");
		check(vel_msgs[o] == 1 && vel_msgs[1 - o] == 0 && !vel_last_add[o]
			&& vel_last[o][0] == q28(m[0]) && vel_last[o][1] == q28(m[1])
			&& vel_last[o][2] == q28(m[2]),
			"G2: ONE combined message = Q(knockBack from the shooter's current "
			"pose (none for a null owner) + absorbed impulse, one packet step), "
			"on the victim's own route");
		if(skel) {
			dict_entity_erase(s->entities, skel->id);
			free(skel);
		}
	}

	/* ---- G3: guest landing + same-update skeleton arrow (24 / 40) -------- */
	for(int c = 0; c < 2; c++) {
		v4_place(s, 12.5, 12.5, 4.5, 4.5);
		v4_reset_body(g, 0.0, 0.0, 0.0);
		v4_reset_body(h, 0.0, 0.0, 0.0);
		struct entity* skel = v4_frozen(s, 9.75F, 4.0F, 9.0F, MONSTER_SKELETON);
		g->on_ground = false;
		g->fall_distance = c ? 8.0F : 6.0F; /* falling; this sample lands */
		const int fall = c ? 40 : 24;
		const bool was = s->paused;
		s->paused = false;
		guest_sample(s, 4.5, Y, 4.5, true, g->correction_seq);
		s->paused = was;
		check(g->health == MAX_PLAYER_HEALTH && g->motion_q_n == 1
			&& g->motion_q[0].fall_damage == fall,
			"G3 setup: the guest landing is deferred into its packet");
		struct entity* arrow = calloc(1, sizeof *arrow);
		assert(arrow);
		entity_arrow(920001u + (uint32_t)c, arrow, true, &s->worlds[0], skel->id,
					 ITEM_ARROW, ARROW_DAMAGE, false);
		arrow->data.arrow.from_skeleton = true;
		arrow->teleport(arrow, (vec3) {4.5F, (float)Y - 0.85F, 4.5F - 1.5F});
		glm_vec3_copy((vec3) {0.0F, 0.0F, 2.0F}, arrow->vel);
		vel_reset();
		v4_arrow = arrow;
		v4_live_tick(s);
		v4_arrow = NULL;
		free(arrow);
		double m[3] = {0.0, 0.0, 0.0};
		v4_knockback(m, 9.75, 9.0, 4.5, 4.5);
		double tx, tz;
		v4_floor_step(m, false, &tx, &tz); /* previous packet airborne */
		const int want = fall > ARROW_DAMAGE ? fall : ARROW_DAMAGE;
		printf("     G3 fall %d: health %d last %d msgs %u v (%.6f %.6f %.6f) want (%.6f %.6f %.6f)\n",
			   fall, (int)g->health, (int)g->last_damage, vel_msgs[1],
			   (double)vel_last[1][0], (double)vel_last[1][1],
			   (double)vel_last[1][2], (double)q28(m[0]), (double)q28(m[1]),
			   (double)q28(m[2]));
		check(g->health == MAX_PLAYER_HEALTH - want && g->last_damage == want,
			"G3: arrow first (fresh), the landing inside its window "
			"(absorbed / excess only)");
		check(vel_msgs[1] == 1 && vel_last[1][0] == q28(m[0])
			&& vel_last[1][1] == q28(m[1]) && vel_last[1][2] == q28(m[2]),
			"G3: the arrow's knockBack survives (one guest packet step after it)");
		dict_entity_erase(s->entities, skel->id);
		free(skel);
	}

	/* ---- G4: guest pause/resume -------------------------------------- */
	{
		v4_place(s, 12.5, 12.5, 4.5, 4.5);
		v4_reset_body(g, 0.1, 0.0, 0.0);
		g->on_ground = true;
		s->paused = false;
		guest_sample(s, 4.5, Y, 4.5, true, g->correction_seq); /* physics */
		s->paused = true;
		guest_sample(s, 4.5, Y, 4.5, true, g->correction_seq); /* paused: none */
		check(g->motion_q_n == 1, "G4: a paused guest sample adds no event");
		vel_reset();
		g->been_attacked = true; /* a flag pending across the pause */
		g->been_attacked_life = server_local_actor_ident(s, g).generation;
		for(int k = 0; k < 2; k++) {
			tick(s); /* paused: held */
			check(g->motion_q_n == 1 && g->vel_x == 0.1 && g->motion_y == 0.0
				&& vel_msgs[1] == 0 && g->been_attacked,
				"G4: a held update evolves no guest motion and sends nothing");
		}
		g->been_attacked = false;
		v4_victim = g;
		v4_live_tick(s); /* resumes: hit, the held event once, one flush */
		v4_victim = NULL;
		double m[3] = {0.1, 0.0, 0.0};
		double tx, tz;
		v4_floor_step(m, true, &tx, &tz);
		check(g->motion_q_n == 0 && vel_msgs[1] == 1
			&& vel_last[1][0] == q28(m[0]) && vel_last[1][1] == q28(m[1])
			&& vel_last[1][2] == q28(m[2]),
			"G4: the resumed update replays the held guest event exactly once");
	}

	/* ---- G5 (v5, ASTRA-V4-REVIEW guest query membership): the guest body
	 * is a Chunk.entities[] member like the host -- Beta's second
	 * EntityPlayerMP. (a) its own section entry, in Beta order (the local
	 * section query == the exhaustive ordered reference); (b) a mob's own
	 * loop pushes the guest (S/EntityLiving.java:623-629 -> the guest's
	 * applyEntityCollision half on its server motion); (c) the HOST's packet
	 * loop pushes the guest, whose next tracker send carries it. */
	{
		v4_place(s, 4.5, 4.5, 5.05, 4.5);
		v4_reset_body(g, 0.0, 0.0, 0.0);
		v4_reset_body(h, 0.0, 0.0, 0.0);
		h->on_ground = g->on_ground = true;
		/* (a) */
		check(g->added_to_chunk && g->chunk_world_slot == 0,
			"G5a: the guest body has its own section membership");
		check(server_local_entity_query_local_w(s, &s->worlds[0], -1, -1, 1, 1,
				0, 7), "G5a: the local section query ran");
		size_t n_local = 0;
		const struct server_entity_query_ref* lr
			= server_local_entity_query_snapshot(s, &n_local);
		struct server_entity_query_ref local[64];
		check(n_local <= 64, "G5a: few refs");
		memcpy(local, lr, n_local * sizeof *lr);
		int host_refs = 0, guest_refs = 0;
		for(size_t i = 0; i < n_local; i++)
			if(local[i].player) {
				if(local[i].actor)
					guest_refs++;
				else
					host_refs++;
			}
		check(host_refs == 1 && guest_refs == 1,
			"G5a: one host and one guest entry in the section lists");
		check(server_local_prepare_entity_query_snapshot_bounded_w(s,
				&s->worlds[0], -1, -1, 1, 1, 0, 7),
			"G5a: the exhaustive reference ran");
		size_t n_ref = 0;
		const struct server_entity_query_ref* rr
			= server_local_entity_query_snapshot(s, &n_ref);
		bool same = n_ref == n_local;
		for(size_t i = 0; same && i < n_ref; i++)
			same = rr[i].player == local[i].player
				&& rr[i].actor == local[i].actor && rr[i].id == local[i].id
				&& rr[i].list_order == local[i].list_order
				&& rr[i].chunk_x == local[i].chunk_x
				&& rr[i].chunk_y == local[i].chunk_y
				&& rr[i].chunk_z == local[i].chunk_z;
		check(same, "G5a: section order == the exhaustive Beta-ordered reference");
		/* (b) */
		struct entity* zom = v4_frozen(s, 5.6F, 4.0F, 4.5F, MONSTER_ZOMBIE);
		entity_living_collide_with_nearby(zom, s);
		double hx, hz;
		const bool touch = v4_collision(5.05, 4.5, (double)zom->pos[0], 4.5,
										&hx, &hz);
		printf("     G5b mob loop -> guest: guest vx %.17g want %.17g host vx %.17g\n",
			   g->vel_x, -hx, h->vel_x);
		check(touch && g->vel_x == -hx && g->vel_z == -hz && h->vel_x == 0.0,
			"G5b: the mob's own loop pushes the guest (its half on the guest's "
			"server motion), not the out-of-reach host");
		dict_entity_erase(s->entities, zom->id);
		free(zom);
		/* (c) */
		v4_reset_body(g, 0.0, 0.0, 0.0);
		v4_reset_body(h, 0.0, 0.0, 0.0);
		const bool was = s->paused;
		s->paused = false;
		host_sample(s, 4.5, Y, 4.5, true, h->correction_seq);
		s->paused = was;
		check(h->motion_q_n == 1 && g->motion_q_n == 0,
			"G5c setup: one host packet, no guest packet");
		vel_reset();
		v4_victim = g; /* a fresh null hit: the guest flag, no knockBack */
		v4_live_tick(s);
		v4_victim = NULL;
		double px, pz;
		const bool push = v4_collision(5.05, 4.5, 4.5, 4.5, &px, &pz);
		printf("     G5c host packet -> guest: guest vx %.17g want %.17g msgs %u/%u sent %.6f\n",
			   g->vel_x, -px, vel_msgs[0], vel_msgs[1], (double)vel_last[1][0]);
		check(push && g->vel_x == -px && g->vel_z == -pz && g->motion_y == 0.0,
			"G5c: the host's packet loop pushed the guest (its -half)");
		check(vel_msgs[1] == 1 && vel_msgs[0] == 0 && !vel_last_add[1]
			&& vel_last[1][0] == q28(-px) && vel_last[1][1] == 0.0F
			&& vel_last[1][2] == q28(-pz),
			"G5c: the guest's tracker send carries the host's push");
	}
}

/* ======================================================================== */
/* Row #26: teleport-scale sample -- host accepts, guest refuses + corrects.  */
static void case_delta(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct entity* gbody = gstate.players[1].local_player;
	assert(gbody);
	const uint8_t zero_all = PLAYER_CORRECTION_POSITION_ONLY
		| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
		| PLAYER_CORRECTION_ZERO_VEL_Z;
	assert(PLAYER_MOVE_TELEPORT_DELTA_SQ == 4.0);

	/* Host: the jump is ACCEPTED; only the odometer is suppressed. */
	counters_reset();
	double hx = h->x;
	uint32_t hseq = h->correction_seq;
	float hwalk = h->distance_walked;
	host_sample(s, hx + 3.0, h->y, h->z, true, hseq);
	check(h->x == hx + 3.0 && h->correction_seq == hseq && host_pos_msgs == 0,
		"host: a teleport-scale sample (dx=3) is ACCEPTED, no correction, "
		"token untouched");
	check(h->distance_walked == hwalk,
		"host: the footstep odometer is suppressed across the jump");

	/* Guest: the same jump is REFUSED and corrected (residency anchor 1). */
	counters_reset();
	double gx = g->x, gy = g->y, gz = g->z;
	uint32_t gseq = g->correction_seq;
	gbody->vel[0] = 0.2F; gbody->vel[1] = -0.3F; gbody->vel[2] = 0.1F;
	guest_sample(s, gx + 3.0, gy, gz, true, gseq);
	check(g->x == gx && g->y == gy && g->z == gz,
		"guest: the same teleport-scale sample is REFUSED -- body unmoved "
		"(residency anchor 1 protection, documented deviation from the host)");
	check(g->correction_seq == gseq + 1 && guest_corrections == 1
		&& guest_corr_seq == g->correction_seq && guest_corr_flags == zero_all,
		"guest: ONE correction authored with the bumped token and "
		"POSITION_ONLY | ZERO_VEL_X/Y/Z");
	check(guest_corr_pos[0] == gx && guest_corr_pos[1] == gy && guest_corr_pos[2] == gz
		&& near3(gbody->pos, gx, gy, gz),
		"guest client: snapped back to the last ACCEPTED position");
	check(gbody->vel[0] == 0.0F && gbody->vel[1] == 0.0F && gbody->vel[2] == 0.0F,
		"guest client: velocity zeroed by the correction envelope");

	/* Token discipline: the pre-correction token is dead, the new one lives. */
	counters_reset();
	guest_sample(s, gx + 0.1, gy, gz, true, gseq);
	check(g->x == gx && guest_corrections == 0,
		"guest: a one-tick step echoing the SUPERSEDED token is refused silently");
	guest_sample(s, gx + 0.1, gy, gz, true, g->correction_seq);
	check(g->x == gx + 0.1, "guest: the same step on the new token is accepted");

	/* The bound is `delta^2 >= PLAYER_MOVE_TELEPORT_DELTA_SQ` (4.0): just
	 * under 2.0 blocks passes, exactly 2.0 is refused. */
	counters_reset();
	gx = g->x;
	guest_sample(s, gx + 1.99, g->y, g->z, true, g->correction_seq);
	check(g->x == gx + 1.99 && guest_corrections == 0,
		"guest: dx=1.99 (delta^2 < 4) is accepted");
	gx = g->x;
	gseq = g->correction_seq;
	guest_sample(s, gx + 2.0, g->y, g->z, true, gseq);
	check(g->x == gx && g->correction_seq == gseq + 1 && guest_corrections == 1,
		"guest: dx=2.00 (delta^2 == 4) is refused and corrected");
	/* Diagonal: 1.5^2 + 1.5^2 = 4.5 >= 4 refused; 1.4^2 + 1.4^2 = 3.92 accepted. */
	gx = g->x; double gz2 = g->z;
	counters_reset();
	guest_sample(s, gx + 1.4, g->y, gz2 + 1.4, true, g->correction_seq);
	check(g->x == gx + 1.4 && g->z == gz2 + 1.4 && guest_corrections == 0,
		"guest: diagonal 1.4/1.4 (delta^2 3.92) is accepted");
	gx = g->x; gz2 = g->z;
	guest_sample(s, gx + 1.5, g->y, gz2 + 1.5, true, g->correction_seq);
	check(g->x == gx && g->z == gz2 && guest_corrections == 1,
		"guest: diagonal 1.5/1.5 (delta^2 4.5) is refused and corrected");

	/* Host at the same diagonal: still accepted -- the asymmetry is the
	 * whole documented policy, not an accident of one axis. */
	counters_reset();
	hx = h->x; double hz = h->z; hseq = h->correction_seq;
	host_sample(s, hx + 1.5, h->y, hz + 1.5, true, hseq);
	check(h->x == hx + 1.5 && h->z == hz + 1.5 && h->correction_seq == hseq
		&& host_pos_msgs == 0,
		"host: diagonal 1.5/1.5 is accepted, no correction");

	teardown(s);
}

/* ======================================================================== */
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* which = argc > 1 ? argv[1] : NULL;
	if(!which
	   || (strcmp(which, "barrier") && strcmp(which, "knockback")
		   && strcmp(which, "delta"))) {
		fprintf(stderr,
			"usage: %s barrier|knockback|delta  (one case per process)\n",
			argv[0]);
		return 2;
	}
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);

	if(!strcmp(which, "barrier"))
		case_barrier();
	else if(!strcmp(which, "knockback"))
		case_knockback();
	else
		case_delta();

	if(failures) {
		printf("test_owner_swap_%s: %u FAILED\n", which, failures);
		return 1;
	}
	printf("test_owner_swap_%s: PASS\n", which);
	return 0;
}
