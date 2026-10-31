/* F1 round 4: reuse the real two-world server fixture, but exercise main's
 * verbatim dispatch with actual EntityClientPlayerMP owner physics. */
#define main explosion_fixture_main
#include "test_explosion_beta_golden.c"
#undef main
#define CAVEWII_F1_DISPATCH_ONLY
#include "../source/main.c"
#undef CAVEWII_F1_DISPATCH_ONLY

#include "network/server_local_guest.h"
#include "network/server_transfer.h"
#include "game/client_world_context.h"
#include "particle.h"
#include "network/server_remote_internal.h"

static unsigned replay_observations;
static void replay_credit_guard(struct server_player* body, unsigned remaining) {
	unsigned owner = body == W.guest_actor ? 1 : 0;
	printf("replay owner %u remaining %u outstanding %u\n", owner, remaining,
		svin_credit_outstanding(owner));
	assert(svin_credit_outstanding(owner) >= remaining);
	if(remaining == ACTOR_MOTION_QUEUE)
		assert(!clin_local_tick_begin(owner));
	replay_observations++;
}

static void queued_credit(unsigned owner) {
	assert(clin_local_tick_begin(owner));
	struct server_player* p = owner ? W.guest_actor : &W.player;
	struct camera c = {.x = p->x, .y = p->y, .z = p->z};
	clin_local_tick_end(owner, &c);
	server_local_test_process_messages(&W);
}

static void guest_create(void) {
	struct server_player* p = calloc(1, sizeof *p);
	assert(p);
	assert(inventory_create(&p->inventory, &inventory_logic_player, &W, p,
		INVENTORY_SIZE, 0, 0, 0));
	p->active_inventory = &p->inventory;
	p->has_pos = p->finished_loading = true;
	p->health = MAX_PLAYER_HEALTH;
	p->x = 10.5; p->y = 43.0; p->z = 10.5;
	W.guest_actor = p;
	W.guest_generation = 31;
	W.guest_owned = W.guest_gameplay = true;
	gstate.players[1].local_generation = 31;
	svin_set_split_accept(true);
}

static void replay_run(void) {
	w_session(0, false);
	svin_set_local_motion(true);
	for(unsigned k = 0; k < ACTOR_MOTION_QUEUE; k++) {
		/* The 101-message budget is reset for each simulated server pump. */
		W.rpc_taken[0] = 0;
		queued_credit(0);
	}
	assert(W.player.motion_q_n == ACTOR_MOTION_QUEUE);
	assert(svin_credit_outstanding(0) == ACTOR_MOTION_QUEUE);
	server_local_test_motion_replay_hook = replay_credit_guard;
	server_local_test_replay_actor_motion(&W);
	server_local_test_motion_replay_hook = NULL;
	assert(replay_observations == ACTOR_MOTION_QUEUE);
	assert(svin_credit_outstanding(0) == 0);
	assert(clin_local_tick_begin(0));
	clin_local_tick_cancel(0);
	puts("R4 finding 1: credits retained until replay terminal PASS");
	w_close();
}

static struct server_rpc reset_owned_sample;
static unsigned reset_owned_n;
static void reset_capture_owned(struct server_rpc* rpc, void* user) {
	(void)user;
	if(rpc->type == SRPC_PLAYER_POS) {
		assert(reset_owned_n++ == 0);
		reset_owned_sample = *rpc;
	}
}
static void reset_run(void) {
	assert(clin_world_payloads_set_enabled(true));
	struct client_rpc reset = {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = 420, .world_index = 0, .world_generation = 200}};
	clin_process(&reset);
	svin_set_local_motion(true);
	assert(clin_local_tick_begin(0));
	struct camera c = {.x = 8.5F, .y = 43.0F, .z = 8.5F};
	clin_local_tick_end(0, &c);
	assert(svin_credit_outstanding(0) == 1);
	struct client_world_context* ctx = client_ctx_registry_current(clin_world_contexts(), 0);
	reset.payload.world_reset.world_generation = 199;
	clin_process(&reset); /* actual superseded reset rejection */
	assert(client_ctx_registry_current(clin_world_contexts(), 0) == ctx);
	assert(clin_local_tick_begin(0));
	printf("rejected reset: outstanding %u (expected 2)\n", svin_credit_outstanding(0));
	assert(svin_credit_outstanding(0) == 2);
	clin_local_tick_cancel(0);
	/* Retain the old lifetime's in-flight completion across an ACCEPTED reset.
	 * Its terminal completion must not release the replacement reservation. */
	svin_process_messages(reset_capture_owned, NULL, false);
	assert(reset_owned_n == 1);
	reset.payload.world_reset.world_generation = 201;
	clin_process(&reset);
	assert(clin_local_tick_begin(0));
	assert(svin_credit_outstanding(0) == 1);
	uint32_t stale = svin_credit_stale_releases();
	svin_credit_release(&reset_owned_sample);
	assert(svin_credit_stale_releases() == stale + 1
		&& svin_credit_outstanding(0) == 1);
	clin_local_tick_cancel(0);
	assert(svin_credit_outstanding(0) == 0);
	puts("R4 finding 2: rejected reset retains live credit lifetime PASS");
}

static void leave_run(void) {
	w_session(0, false);
	svin_set_local_motion(true);
	guest_create();
	queued_credit(1);
	assert(W.guest_actor->motion_credit_n == 1);
	const uint32_t epoch = W.guest_actor->motion_credit_epoch;
	server_local_set_paused(&W, true, -1);
	server_local_guest_release_checkpointed(&W); /* actual committed leave */
	assert(!W.guest_actor && !W.guest_owned);
	printf("guest leave: outstanding %u (expected 0)\n", svin_credit_outstanding(1));
	assert(svin_credit_outstanding(1) == 0);
	server_local_set_paused(&W, false, -1);
	guest_create();
	W.guest_generation = gstate.players[1].local_generation = 32;
	queued_credit(1);
	assert(svin_credit_outstanding(1) == 1);
	uint32_t stale = svin_credit_stale_releases();
	svin_credit_release_tag(1, epoch, 31, 1);
	assert(svin_credit_stale_releases() == stale + 1
		&& svin_credit_outstanding(1) == 1);
	server_local_guest_release_checkpointed(&W);
	assert(svin_credit_outstanding(1) == 0);
	(void)svin_retire_local();
	svin_set_local_motion(true); /* provider restart: fresh epoch */
	assert(clin_local_tick_begin(0));
	clin_local_tick_cancel(0);
	assert(svin_credit_outstanding(0) == 0);
	puts("R4 finding 4: guest queued credits terminally cancelled at leave PASS");
	w_close();
}

static struct entity* client_body(unsigned owner) {
	struct entity* e = calloc(1, sizeof *e);
	assert(e);
	entity_local_player(420 + owner, e, &gstate.world);
	e->health = MAX_PLAYER_HEALTH;
	e->pos[0] = 8.5F + owner * 2;
	e->pos[1] = 43.0F;
	e->pos[2] = 8.5F + owner * 2;
	e->vel[0] = 0.125F;
	e->vel[1] = 0.2F;
	gstate.players[owner].local_player = e;
	gstate.players[owner].camera = (struct camera){0};
	gstate.players[owner].sleeping = false;
	gstate.players[owner].player_sync.state_known = true;
	dict_entity_set_at(gstate.entities, e->id, e);
	return e;
}

static void column_install_in(struct world* world) {
	struct chunk_rpc_slot* c = clin_chunk_slot_acquire();
	assert(c);
	memset(c, 0, sizeof *c);
	for(unsigned x = 0; x < CHUNK_SIZE; x++)
		for(unsigned z = 0; z < CHUNK_SIZE; z++)
			for(unsigned y = 0; y <= 40; y++)
				c->ids[(x * CHUNK_SIZE + z) * WORLD_HEIGHT + y] = BLOCK_STONE;
	assert(world_build_full_section(world, 0, 0, c->ids, c->metadata,
		c->lighting_sky, c->lighting_torch, c->heightmap, c->grass_i, c->grass_j));
	clin_chunk_slot_release(c);
}
static void column_install(void) {
	column_install_in(&gstate.world);
}

static void host_dispatch(void) {
	const unsigned slots[] = {0};
	main_client_host_tick_dispatch(slots, 1, true, false, -1);
}

/* Beta client EntityClientPlayerMP.onUpdate:31-35/func_4056_N:80-81 has
 * no sleep gate. Server NetServerHandler.handleFlying:112-116 still runs
 * onUpdateEntity, but skips the packet collision move while sleeping. */
static void sleeping_accept(struct server_rpc_source source,
	struct server_rpc* call, void* user) {
	struct server_local* s = user;
	assert(call->type == SRPC_PLAYER_POS && source.slot < 2);
	struct server_player* body = source.slot ? s->guest_actor : &s->player;
	assert(body->sleeping && server_local_actor_motion_reserve(s, body));
	/* Explicit accepting-consumer fixture: the pre-existing native ingress
	 * sleep barrier still drops these packets (F2). Feed the REAL published
	 * sample to the native queue, credit transfer and replay to witness the
	 * sleeping branch without changing that unrelated barrier. */
	server_local_actor_motion_enqueue(s, body,
		call->payload.player_pos.x, call->payload.player_pos.y,
		call->payload.player_pos.z, call->payload.player_pos.vel_x,
		call->payload.player_pos.vel_z, call->payload.player_pos.on_ground);
}

static enum svin_admit_result sleeping_admit(struct server_rpc_source source,
	const struct server_rpc* call, void* user) {
	(void)source; (void)call; (void)user;
	return SVIN_ADMIT;
}

static void sleeping_run(void) {
	w_session(0, false);
	svin_set_local_motion(true);
	guest_create();
	column_install();
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	const struct input_player_frame frame = {.connected = true};
	const struct svin_drain_ops sleep_ops = {.process = sleeping_accept,
		.admit = sleeping_admit};
	for(unsigned owner = 0; owner < 2; owner++) {
		struct entity* e = client_body(owner);
		if(owner) e->tick_client = NULL;
		gstate.players[owner].sleeping = true;
		struct server_player* body = owner ? W.guest_actor : &W.player;
		body->sleeping = true;
		struct clin_local_tick_stats before, after;
		clin_local_tick_stats(owner, &before);
		for(unsigned tick = 0; tick < 3; tick++) {
			if(owner) main_client_guest_tick_dispatch(&frame);
			else host_dispatch();
			svin_process_owned_messages_ops(&sleep_ops, &W);
		}
		clin_local_tick_stats(owner, &after);
		assert(after.executed == before.executed + 3);
		assert(after.cancelled == before.cancelled);
		assert(svin_credit_outstanding(owner) == 3 && body->motion_q_n == 3);
		for(unsigned tick = 0; tick < 3; tick++) {
			const struct actor_motion_event* ev = &body->motion_q[tick];
			assert(ev->sleeping && ev->physics && ev->credited);
			assert(ev->nx == e->pos[0] && ev->ny == e->pos[1] && ev->nz == e->pos[2]);
		}
		assert(!memcmp(e->pos, e->pos_old, sizeof e->pos));
		assert(e->vel[0] == 0 && e->vel[1] == 0 && e->vel[2] == 0);
		unsigned sleeping = server_local_test_sleeping_branch_count;
		server_local_test_replay_actor_motion(&W);
		assert(server_local_test_sleeping_branch_count == sleeping + 3);
		assert(body->motion_q_n == 0 && svin_credit_outstanding(owner) == 0);
		/* Native ingress remains unchanged: its sleeping barrier terminally
		 * settles the next actually published sample and returns its credit. */
		if(owner) main_client_guest_tick_dispatch(&frame);
		else host_dispatch();
		assert(svin_credit_outstanding(owner) == 1);
		server_local_test_process_messages(&W);
		assert(svin_credit_outstanding(owner) == 0 && body->motion_q_n == 0);
	}
	puts("R5 P1: both sleeping owners publish every tick; accepting fixture reaches native sleeping replay; native barrier settles PASS");
	server_local_guest_release_checkpointed(&W);
	w_close();
}

static void terrain_run(void) {
	svin_set_local_motion(true);
	svin_set_split_accept(true);
	gstate.players[1].local_generation = 31;
	struct entity* host = client_body(0);
	struct entity* guest = client_body(1);
	guest->tick_client = NULL;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	/* Distinct interpolation and velocity state at the same missing column. */
	guest->pos_old[0] = host->pos_old[0] = -7.0F;
	guest->orient[0] = host->orient[0] = 0.25F;
	guest->orient_old[0] = host->orient_old[0] = -0.5F;
	struct clin_local_tick_stats before, after;
	clin_local_tick_stats(1, &before);
	host_dispatch();
	const struct input_player_frame frame = {.connected = true};
	main_client_guest_tick_dispatch(&frame);
	clin_local_tick_stats(1, &after);
	assert(after.executed == before.executed && after.cancelled == before.cancelled + 1);
	assert(svin_credit_outstanding(0) == 0 && svin_credit_outstanding(1) == 0);
	assert(!memcmp(host->pos, host->pos_old, sizeof host->pos));
	assert(!memcmp(guest->pos, guest->pos_old, sizeof guest->pos));
	assert(!memcmp(guest->orient, guest->orient_old, sizeof guest->orient));
	assert(!memcmp(host->vel, guest->vel, sizeof host->vel));
	assert(guest->vel[0] == 0 && guest->vel[1] == 0 && guest->vel[2] == 0);
	column_install();
	main_client_guest_tick_dispatch(&frame);
	assert(guest->pos[0] == 10.5F); /* old horizontal velocity cannot return */
	clin_local_tick_stats(1, &after);
	assert(after.executed == before.executed + 1 && svin_credit_outstanding(1) == 1);
	puts("R5 P3: guest terrain hold matches host interpolation/velocity PASS");
}

static unsigned fatal_dead_observations;
static void fatal_credit_guard(struct server_player* body, unsigned remaining) {
	assert(body == &W.player);
	printf("fatal replay remaining %u health %d credits %u\n", remaining,
		body->health, svin_credit_outstanding(0));
	assert(svin_credit_outstanding(0) == ACTOR_MOTION_QUEUE);
	assert(!clin_local_tick_begin(0)); /* no replacement publication mid-pass */
	if(remaining < ACTOR_MOTION_QUEUE) {
		assert(body->health <= 0);
		fatal_dead_observations++;
	}
}

static void fatal_run(void) {
	w_session(0, false);
	svin_set_local_motion(true);
	/* Real ingress defers a lethal landing until replay, then accepts the
	 * remaining 100 samples while the body is still alive. */
	W.player.fall_distance = 30.0;
	W.player.health = 8;
	struct entity* client = client_body(0);
	client->on_ground = true;
	for(unsigned k = 0; k < ACTOR_MOTION_QUEUE; k++) {
		W.rpc_taken[0] = 0;
		assert(clin_local_tick_begin(0));
		struct camera c = {.x = 8.5F, .y = 43.0F, .z = 8.5F};
		clin_local_tick_end(0, &c);
		server_local_test_process_messages(&W);
	}
	assert(W.player.motion_q_n == ACTOR_MOTION_QUEUE);
	assert(W.player.motion_q[0].fall_damage >= W.player.health);
	assert(W.player.health == 8 && svin_credit_outstanding(0) == ACTOR_MOTION_QUEUE);
	server_local_test_motion_replay_hook = fatal_credit_guard;
	server_local_test_replay_actor_motion(&W);
	server_local_test_motion_replay_hook = NULL;
	assert(fatal_dead_observations == ACTOR_MOTION_QUEUE - 1);
	assert(W.player.health <= 0 && W.player.death_ticks == 22);
	assert(W.player.motion_q_n == 0 && W.player.motion_credit_n == 0);
	assert(svin_credit_outstanding(0) == 0);
	assert(clin_local_tick_begin(0));
	clin_local_tick_cancel(0);
	puts("R5 P2: lethal landing retains credits through all corpse replay PASS");
	w_close();
}

static void tag_run(bool invalid) {
	w_session(0, false);
	svin_set_local_motion(true);
	guest_create();
	queued_credit(0);
	if(invalid) {
		/* Impossible today: a guest body's old tag owns a LIVE host credit.
		 * Guard this explicitly so a future tag-source bug cannot free it. */
		W.guest_actor->motion_credit_n = W.player.motion_credit_n;
		W.guest_actor->motion_credit_slot = W.player.motion_credit_slot;
		W.guest_actor->motion_credit_epoch = W.player.motion_credit_epoch;
		W.guest_actor->motion_credit_gen = W.player.motion_credit_gen;
		queued_credit(1);
		assert(!"live old credit tag was accepted");
	} else {
		const uint32_t stale = svin_credit_stale_releases();
		svin_credit_host_lifetime_bump();
		queued_credit(0);
		assert(svin_credit_stale_releases() == stale + 1);
		assert(svin_credit_outstanding(0) == 1 && W.player.motion_credit_n == 1);
		server_local_test_replay_actor_motion(&W);
		assert(svin_credit_outstanding(0) == 0);
		puts("R5 P3: stale old enqueue tag cannot retire replacement credit PASS");
		server_local_guest_release_checkpointed(&W);
		w_close();
	}
}

static void dispatch_run(void) {
	svin_set_local_motion(true);
	struct entity* host = client_body(0);
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	struct clin_local_tick_stats before, after;
	clin_local_tick_stats(0, &before);
	host_dispatch(); /* real tick returns at the Beta blockExists gate */
	clin_local_tick_stats(0, &after);
	printf("terrain hold: executed delta %u outstanding %u\n",
		after.executed - before.executed, svin_credit_outstanding(0));
	assert(after.executed == before.executed);
	assert(after.cancelled == before.cancelled + 1);
	assert(svin_credit_outstanding(0) == 0);
	assert(host->pos[0] == 8.5F && host->pos[1] == 43.0F);
	column_install();
	host->vel[0] = 0.125F;
	host_dispatch();
	clin_local_tick_stats(0, &after);
	assert(after.executed == before.executed + 1);
	assert(host->pos[0] > 8.5F);
	assert(svin_credit_outstanding(0) == 1);
	/* Beta EntityClientPlayerMP.onUpdate:31-35 sends even while sleeping;
	 * func_4056_N:80-81 emits stationary Packet10, with no sleep gate. */
	gstate.players[0].sleeping = true;
	clin_local_tick_stats(0, &before);
	host_dispatch();
	clin_local_tick_stats(0, &after);
	assert(after.executed == before.executed + 1 && after.cancelled == before.cancelled);
	/* Guest direct-walk terrain hold and an independent host loading hold. */
	svin_set_split_accept(true);
	gstate.players[1].local_generation = 31;
	struct entity* guest = client_body(1);
	guest->tick_client = NULL;
	guest->pos[0] = 32.5F; /* that column is absent */
	const struct input_player_frame frame = {.connected = true};
	clin_local_tick_stats(1, &before);
	main_client_guest_tick_dispatch(&frame);
	clin_local_tick_stats(1, &after);
	assert(after.executed == before.executed && after.cancelled == before.cancelled + 1);
	assert(svin_credit_outstanding(1) == 0);
	guest->pos[0] = 10.5F;
	main_client_guest_tick_dispatch(&frame);
	clin_local_tick_stats(1, &after);
	assert(after.executed == before.executed + 1 && svin_credit_outstanding(1) == 1);
	clin_local_tick_stats(0, &before);
	const unsigned slots[] = {0};
	main_client_host_tick_dispatch(slots, 1, false, true, 0);
	clin_local_tick_stats(0, &after);
	assert(after.attempted == before.attempted && after.executed == before.executed);
	/* Real stamped client payloads put the owners in separate worlds. The
	 * host dispatch must execute only its own body, and guest riding must
	 * still reserve before following a mount (including when exhausted). */
	assert(clin_world_payloads_set_enabled(true));
	struct client_rpc reset = {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = 420, .world_index = 0, .world_generation = 300}};
	clin_process(&reset);
	reset.payload.world_reset.world_index = 1;
	reset.payload.world_reset.world_generation = 301;
	reset.payload.world_reset.dimension = WORLD_DIM_NETHER;
	reset.payload.world_reset.guest_world = true;
	reset.payload.world_reset.guest_view = 1;
	clin_process(&reset);
	struct clin_world_payload worlds[2];
	for(unsigned o = 0; o < 2; o++) {
		assert(clin_world_payload_of_view(o, &worlds[o]));
		column_install_in(worlds[o].terrain);
	}
	assert(worlds[0].terrain != worlds[1].terrain);
	host = gstate.players[0].local_player;
	assert(host && host->world == worlds[0].terrain);
	host->health = MAX_PLAYER_HEALTH;
	host->pos[0] = host->pos[2] = 8.5F; host->pos[1] = 43.0F;
	host->vel[0] = 0.125F;
	gstate.players[0].sleeping = false;
	gstate.world_loaded = true; gstate.world_switch_pending = false;
	guest = calloc(1, sizeof *guest); assert(guest);
	entity_local_player(421, guest, worlds[1].terrain);
	guest->health = MAX_PLAYER_HEALTH;
	guest->pos[0] = guest->pos[2] = 10.5F; guest->pos[1] = 43.0F;
	guest->tick_client = NULL;
	gstate.players[1].local_player = guest;
	gstate.players[1].local_generation = 31;
	gstate.players[1].sleeping = false;
	dict_entity_set_at(worlds[1].entities, guest->id, guest);
	const unsigned separate_slots[] = {0, 1};
	clin_local_tick_stats(0, &before);
	main_client_host_tick_dispatch(separate_slots, 2, true, false, -1);
	clin_local_tick_stats(0, &after);
	assert(after.executed == before.executed + 1);
	struct entity* pig = calloc(1, sizeof *pig); assert(pig);
	pig->id = 422; pig->type = ENTITY_MONSTER;
	pig->data.monster.id = MONSTER_PIG;
	pig->data.monster.pig_ridden = true;
	pig->data.monster.pig_rider_id = guest->id;
	pig->pos[0] = pig->pos[2] = 10.5F; pig->pos[1] = 41.0F;
	dict_entity_set_at(worlds[1].entities, pig->id, pig);
	clin_local_tick_stats(1, &before);
	main_client_guest_tick_dispatch(&frame);
	clin_local_tick_stats(1, &after);
	assert(after.executed == before.executed + 1);
	assert(guest->pos[1] == pig->pos[1] + PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT);
	for(unsigned k = 0; k < SVIN_MOTION_CREDITS; k++)
		main_client_guest_tick_dispatch(&frame);
	const struct entity held = *guest;
	pig->pos[0] += 1.0F;
	clin_local_tick_stats(1, &before);
	main_client_guest_tick_dispatch(&frame);
	clin_local_tick_stats(1, &after);
	assert(after.suspended == before.suspended + 1);
	assert(!memcmp(&held, guest, sizeof held));
	clin_local_tick_stats(0, &before);
	main_client_host_tick_dispatch(separate_slots, 2, true, false, -1);
	clin_local_tick_stats(0, &after);
	assert(after.executed == before.executed + 1); /* unsaturated owner proceeds */
	puts("R4 findings 3/5: main real dispatch + owner physics/terrain hold PASS");
}

static struct server_rpc authored[2][256];
static unsigned authored_n[2], delivered_n[2];
static void pressure_ingress(struct server_rpc_source source,
	const struct server_rpc* call) {
	uint8_t slot = source.slot;
	assert(slot < 2 && authored_n[slot] < 256);
	assert(call->payload.player_pos.credited);
	authored[slot][authored_n[slot]++] = *call;
}
static void pressure_delivered(struct server_local* s, const struct server_rpc* call) {
	(void)s;
	if(call->type != SRPC_PLAYER_POS) return;
	unsigned owner = call->payload.player_pos.credit_slot;
	assert(owner < 2 && delivered_n[owner] < authored_n[owner]);
	assert(!memcmp(&call->payload.player_pos,
		&authored[owner][delivered_n[owner]++].payload.player_pos,
		sizeof call->payload.player_pos));
}
static void pressure_step(unsigned owner) {
	struct entity* body = gstate.players[owner].local_player;
	const struct entity prior = *body;
	const ptime_t cadence = gstate.players[owner].player_sync.last_pos_update;
	struct clin_local_tick_stats a, b;
	clin_local_tick_stats(owner, &a);
	unsigned before = authored_n[owner];
	if(owner == 0) host_dispatch();
	else {
		const struct input_player_frame frame = {.connected = true};
		main_client_guest_tick_dispatch(&frame);
	}
	clin_local_tick_stats(owner, &b);
	if(b.executed == a.executed) {
		/* Exhaustion holds every actor/RNG byte and the movement cadence. */
		assert(!memcmp(&prior, body, sizeof prior));
		assert(!memcmp(&cadence, &gstate.players[owner].player_sync.last_pos_update,
			sizeof cadence));
		assert(authored_n[owner] == before);
		assert(b.suspended == a.suspended + 1);
	} else {
		assert(b.executed == a.executed + 1);
		assert(authored_n[owner] == before + 1);
		const struct server_rpc* call = &authored[owner][before];
		assert(call->payload.player_pos.x == (double)body->pos[0]);
		assert(call->payload.player_pos.y == (double)body->pos[1]);
		assert(call->payload.player_pos.z == (double)body->pos[2]);
		assert(call->payload.player_pos.vel_y == body->vel[1]);
		if(authored_n[owner] % 7 == 0) {
			struct server_rpc action = {.type = SRPC_HOTBAR_SLOT,
				.payload.hotbar_slot.slot = authored_n[owner] % 9};
			assert(svin_rpc_send_from((struct server_rpc_source){.slot = owner,
				.generation = owner ? W.guest_generation : 0}, &action));
		}
	}
}
static void pressure_run(unsigned total) {
	w_session(0, false);
	svin_set_local_motion(true);
	guest_create();
	column_install();
	client_body(0);
	struct entity* guest = client_body(1);
	guest->tick_client = NULL; /* production guest install's exact contract */
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.players[1].local_bound = true;
	gstate.players[1].local_phase = CLIENT_GUEST_ATTACHED;
	gstate.players[1].local_gameplay_ready = true;
	gstate.players[0].player_sync.correction_seq = W.player.correction_seq;
	gstate.players[1].player_sync.correction_seq = W.guest_actor->correction_seq;
	svin_test_credit_publish_hook = pressure_ingress;
	server_local_test_rpc_done_hook = pressure_delivered;
	memset(authored_n, 0, sizeof authored_n);
	memset(delivered_n, 0, sizeof delivered_n);
	struct clin_local_tick_stats before[2], after[2];
	for(unsigned o = 0; o < 2; o++) clin_local_tick_stats(o, &before[o]);
	/* The consumer stays stopped well beyond both physical transport bounds.
	 * Real frames have 0, 1, 2 and 3 tick opportunities; the frame pump cannot
	 * coalesce their already-published poses or synthesize a held tick. */
	unsigned frame = 0;
	for(; frame < 160; frame++) {
		for(unsigned tick = 0; tick < frame % 4; tick++) {
			for(unsigned k = 0; k < 2; k++) {
				unsigned o = (frame + k) % 2;
				if(authored_n[o] < total) pressure_step(o);
			}
		}
		clin_update();
	}
	if(total >= 101) {
		assert(authored_n[0] < total && authored_n[1] < total);
		for(unsigned o = 0; o < 2; o++) {
			clin_local_tick_stats(o, &after[o]);
			assert(after[o].suspended > before[o].suspended);
		}
	}
	unsigned updates = 0;
	while(authored_n[0] < total || authored_n[1] < total
		|| delivered_n[0] < total || delivered_n[1] < total) {
		assert(++updates <= (2 * total + 2 * total / 7 + 15) / 16 + 4);
		svin_rpc_flush_pending();
		server_local_test_process_messages(&W);
		server_local_test_replay_actor_motion(&W);
		for(unsigned tick = 0; tick < 24; tick++)
			for(unsigned o = 0; o < 2; o++)
				if(authored_n[o] < total) pressure_step(o);
		clin_update();
	}
	server_local_test_replay_actor_motion(&W);
	for(unsigned o = 0; o < 2; o++) {
		clin_local_tick_stats(o, &after[o]);
		assert(after[o].executed - before[o].executed == total);
		assert(authored_n[o] == total && delivered_n[o] == total);
		assert(svin_credit_outstanding(o) == 0);
	}
	svin_test_credit_publish_hook = NULL;
	server_local_test_rpc_done_hook = NULL;
	server_local_guest_release_checkpointed(&W);
	printf("R4 main dispatch both owners: %u exact samples each, %u recovery pumps PASS\n",
		total, updates);
	w_close();
}

static void frame_run(void) {
	svin_set_local_motion(true);
	gstate.world_loaded = true; gstate.world_switch_pending = false;
	column_install();
	struct entity* body = client_body(0);
	svin_test_credit_publish_hook = pressure_ingress;
	vec3 poses[3];
	for(unsigned tick = 0; tick < 3; tick++) {
		host_dispatch();
		glm_vec3_copy(body->pos, poses[tick]);
	}
	/* The old 50 ms producer is due, but the LOCAL frame must add nothing.
	 * This also makes the restored-timer mutation actually execute its timer. */
	gstate.players[0].player_sync.last_pos_update = time_get();
	gstate.players[0].player_sync.last_pos_update.tv_sec--;
	clin_update();
	printf("actual catch-up frame: 3 physics steps -> %u published samples\n", authored_n[0]);
	assert(authored_n[0] == 3);
	for(unsigned k = 0; k < 3; k++) {
		const struct server_rpc* call = &authored[0][k];
		assert(call->payload.player_pos.x == (double)poses[k][0]);
		assert(call->payload.player_pos.y == (double)poses[k][1]);
		assert(call->payload.player_pos.z == (double)poses[k][2]);
	}
	svin_test_credit_publish_hook = NULL;
	puts("R4 main catch-up frame: each real step has its own immutable final pose PASS");
}

static bool hold_admit(struct server_rpc_source src, const struct server_rpc* rpc, void* user) {
	(void)src; (void)rpc; (void)user;
	return false;
}
static void never_process(struct server_rpc_source src, struct server_rpc* rpc, void* user) {
	(void)src; (void)rpc; (void)user;
	assert(!"held guest must not dispatch");
}
static bool refuse_close(struct inventory* inv, struct server_player* player) {
	(void)inv; (void)player;
	return false;
}
static void transfer_run(void) {
	w_session(1, false);
	W.worlds[1].dimension = WORLD_DIM_NETHER;
	W.player.dimension = WORLD_DIM_NETHER;
	svin_set_local_motion(true);
	guest_create();
	queued_credit(0);
	W.player.health = 0;
	server_local_set_paused(&W, true, -1);
	assert(clin_local_tick_begin(1));
	const struct camera c = {.x = 10.5F, .y = 43.0F, .z = 10.5F};
	clin_local_tick_end(1, &c);
	svin_process_owned_messages_admitted(never_process, hold_admit, NULL);
	assert(svin_held_count() == 1 && svin_credit_outstanding(1) == 1);
	const uint64_t generation = W.guest_generation;
	const double mx = W.guest_actor->vel_x = 0.05;
	/* Fail the real destination preparation at its shared plan seam. */
	W.player.spawn_x = W.player.spawn_z = 8; W.player.spawn_y = 41;
	server_local_test_fail_next_host_respawn_plan();
	assert(!server_transfer_respawn_host(&W));
	assert(W.paused && W.player.world_slot == 1);
	assert(svin_held_count() == 1 && svin_credit_outstanding(0) == 1
		&& svin_credit_outstanding(1) == 1);
	/* Refuse the pre-commit inventory closure through its normal callback. */
	struct inventory_logic* prior_logic = W.player.inventory.logic;
	struct inventory_logic refusal = *prior_logic;
	refusal.on_close = refuse_close;
	W.player.inventory.logic = &refusal;
	assert(!server_transfer_respawn_host(&W));
	W.player.inventory.logic = prior_logic;
	assert(W.paused && W.player.world_slot == 1);
	assert(svin_held_count() == 1 && svin_credit_outstanding(0) == 1
		&& svin_credit_outstanding(1) == 1);
	/* Actual cross-world death/respawn clears the SHARED pause. The held
	 * guest sample retains its old paused meaning, never moves or consumes
	 * the replacement host's credit, and the guest continues after it. */
	assert(server_transfer_respawn_host(&W));
	assert(!W.paused && W.player.world_slot == 0);
	assert(svin_credit_outstanding(0) == 0);
	assert(W.guest_generation == generation && W.guest_actor->vel_x == mx);
	assert(svin_held_count() == 1 && svin_credit_outstanding(1) == 1);
	server_local_test_process_messages(&W);
	assert(svin_held_count() == 0 && svin_credit_outstanding(1) == 0);
	server_local_test_replay_actor_motion(&W);
	assert(W.guest_actor->vel_x == mx);
	queued_credit(1);
	server_local_test_replay_actor_motion(&W);
	assert(W.guest_actor->vel_x == mx * (double)0.91F);
	assert(svin_credit_outstanding(1) == 0);
	server_local_guest_release_checkpointed(&W);
	puts("R4 transfer: held guest + preparation/commit refusal + real cross-world respawn PASS");
	w_close();
}

static void remote_run(void) {
	/* Real producer -> real remote dispatch -> encoded outbound frame queue;
	 * no sockets/worker thread are needed to inspect cadence and exact bytes. */
	struct server_remote_impl* impl = calloc(1, sizeof *impl);
	assert(impl && thread_mutex_init(&impl->lock)
		&& thread_mutex_init(&impl->frame_lock)
		&& thread_cond_init(&impl->write_signal));
	struct server_remote remote = {.impl = impl};
	impl->state = REMOTE_PLAY;
	impl->generation = 1;
	impl->remote_load_complete_sent = true;
	impl->have_last_sent_sneaking = true;
	gstate.remote_session = true;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	column_install();
	struct entity* host = client_body(0);
	clin_remote_movement_reset();
	unsigned ticks = 0;
	for(unsigned frame = 0; frame < 12; frame++) {
		for(unsigned tick = 0; tick < frame % 4; tick++) {
			host_dispatch();
			svin_process_messages(remote_process_srpc, &remote, false);
			assert(impl->frame_count == ++ticks);
		}
		clin_update();
		svin_process_messages(remote_process_srpc, &remote, false);
		assert(impl->frame_count == ticks); /* no frame-timer duplicate */
	}
	uint32_t executed, samples;
	clin_remote_movement_stats(&executed, &samples);
	assert(executed == ticks && samples == ticks && ticks == 18);
	for(unsigned k = 0; k < ticks; k++) {
		const struct mp_frame* f = &impl->frames[k].frame;
		/* The neutral body still falls/moves during these real ticks, so
		 * first position+look is Packet13, later changed poses Packet11. */
		assert(f->data[0] == (k == 0 ? 0x0d : 0x0b));
		assert(f->length == (k == 0 ? 42u : 34u));
		printf("outbound %02u ", k);
		for(size_t b = 0; b < f->length; b++) printf("%02x", f->data[b]);
		putchar('\n');
	}
	/* Full-inbox remote refusal still advances actual owner physics; local
	 * movement credits cannot stall it or retain a remote movement sample. */
	struct server_rpc flood = {.type = SRPC_PLAYER_POS};
	for(unsigned k = 0; k < 16; k++) assert(svin_rpc_send(&flood));
	float x = host->pos[0];
	uint32_t drops = svin_dropped_count();
	host_dispatch();
	assert(host->pos[0] != x && svin_dropped_count() == drops + 1);
	assert(svin_pending_count() == 0 && svin_credit_outstanding(0) == 0);
	puts("R4 outbound remote: 0/1/2/3 actual ticks, 18 exact frames, remote refusal unchanged PASS");
	thread_cond_destroy(&impl->write_signal);
	thread_mutex_destroy(&impl->frame_lock); thread_mutex_destroy(&impl->lock);
	free(impl);
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(argc >= 2 && clin_init() && svin_init());
	blocks_init(); items_init();
	world_create(&gstate.world); dict_entity_init(gstate.entities); particle_init();
	gstate.remote_session = false;
	snprintf(g_tmp_world, sizeof g_tmp_world, "test_f1_r4_%s_%d", argv[1], GETPID());
	MKDIR(g_tmp_world);
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	if(!strcmp(argv[1], "replay")) replay_run();
	else if(!strcmp(argv[1], "reset")) reset_run();
	else if(!strcmp(argv[1], "leave")) leave_run();
	else if(!strcmp(argv[1], "dispatch")) dispatch_run();
	else if(!strcmp(argv[1], "pressure")) pressure_run(argc == 3 ? (unsigned)atoi(argv[2]) : 250);
	else if(!strcmp(argv[1], "transfer")) transfer_run();
	else if(!strcmp(argv[1], "remote")) remote_run();
	else if(!strcmp(argv[1], "frame")) frame_run();
	else if(!strcmp(argv[1], "sleeping")) sleeping_run();
	else if(!strcmp(argv[1], "terrain")) terrain_run();
	else if(!strcmp(argv[1], "fatal")) fatal_run();
	else if(!strcmp(argv[1], "tag")) tag_run(false);
	else if(!strcmp(argv[1], "tag-invalid")) tag_run(true);
	else assert(!"unknown R4 case");
	g_drain_run = 0; thread_join(&drain);
	return 0;
}
