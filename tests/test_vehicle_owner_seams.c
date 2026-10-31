/* G8-VEHICLE-SEAMS-01: positive prerequisite tests, never WILL_FAIL.
 * One case per process. R2 boat/pig admission and switches use real handlers;
 * labelled relation fixtures retain malformed/lifetime and existing cart
 * regressions. No copied simulation is linked. */
#include "support/owner_swap_harness.h"
#include "entity/entity_boat.h"
#include "entity/entity_minecart.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "network/server_transfer.h"
#include "game/camera.h"

enum vehicle_kind { V_BOAT, V_PIG, V_CART };

static void guest_mount(struct server_local* s, struct entity* e) {
	struct server_rpc rpc = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = e->id, .desired_occupied = true}};
	assert(guest_send(s, &rpc));
}

static struct entity* prop(struct server_local* s, enum vehicle_kind kind,
	struct server_player* p) {
	vec3 pos = {(float)p->x + 1.0F, 4.0F, (float)p->z};
	struct entity* e = kind == V_BOAT ? server_local_spawn_boat(pos, s)
		: kind == V_PIG ? server_local_spawn_monster_at(pos, MONSTER_PIG, s)
		: server_local_spawn_minecart_type(pos, s, 0);
	assert(e);
	if(kind == V_PIG) e->data.monster.saddled = true;
	clin_update_drain();
	return e;
}

static void seed_guest(struct server_local* s, struct entity* e,
	enum vehicle_kind kind) {
	/* Relation fixture for isolated ownership/lifetime negatives. Positive
	 * boat/pig admission uses guest_mount instead. Guest carts remain closed. */
	if(kind == V_BOAT) {
		server_local_set_boat_occupancy_for(s, e, s->guest_actor, true);
	} else if(kind == V_PIG) {
		e->data.monster.pig_ridden = true;
		e->data.monster.pig_rider_id = s->guest_entity_id;
		e->data.monster.pig_rider_actor = guest_ident(s);
	} else {
		e->data.minecart.occupied = true;
		e->data.minecart.occupant_id = s->guest_entity_id;
		e->data.minecart.rider_actor = guest_ident(s);
		e->data.minecart.ride_epoch = 91;
		s->guest_actor->riding_minecart = true;
	}
	clin_update_drain();
}

static bool attached(const struct entity* e, enum vehicle_kind kind,
	uint32_t id, struct actor_ident actor) {
	if(kind == V_BOAT)
		return e->data.boat.occupied && e->data.boat.occupant_id == id
			&& actor_ident_equal(e->data.boat.rider_actor, actor);
	if(kind == V_PIG)
		return e->data.monster.pig_ridden && e->data.monster.pig_rider_id == id
			&& actor_ident_equal(e->data.monster.pig_rider_actor, actor);
	return e->data.minecart.occupied && e->data.minecart.occupant_id == id;
}

static bool detached(const struct entity* e, enum vehicle_kind kind) {
	if(kind == V_BOAT)
		return !e->data.boat.occupied && e->data.boat.occupant_id == 0
			&& actor_ident_is_host(e->data.boat.rider_actor)
			&& e->data.boat.rider_vel_x == 0 && e->data.boat.rider_vel_z == 0;
	if(kind == V_PIG)
		return !e->data.monster.pig_ridden && e->data.monster.pig_rider_id == 0
			&& actor_ident_is_host(e->data.monster.pig_rider_actor);
	return !e->data.minecart.occupied && e->data.minecart.occupant_id == 0
		&& e->data.minecart.ride_epoch == 0
		&& e->data.minecart.pending_throttle == 0;
}

static void host_mount(struct server_local* s, struct entity* e,
	enum vehicle_kind kind) {
	static uint32_t request_seq;
	struct server_rpc rpc = kind == V_CART
		? (struct server_rpc){.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {.entity_id = e->id,
				.input_epoch = s->input_epoch_counter, .request_seq = ++request_seq,
				.desired_occupied = true}}
		: (struct server_rpc){.type = SRPC_ENTITY_USE,
			.payload.entity_use = {.entity_id = e->id, .desired_occupied = true}};
	host_send(s, &rpc);
}

static void case_wire_ids(void) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, 0.5);
	struct entity* h = prop(s, V_BOAT, &s->player);
	struct entity* g = prop(s, V_BOAT, s->guest_actor);
	host_mount(s, h, V_BOAT);
	seed_guest(s, g, V_BOAT);
	check(s->guest_entity_id != SERVER_LOCAL_PLAYER_ENTITY_ID,
		"owner: admitted guest has a distinct real wire entity ID");
	check(attached(h, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST),
		"owner: host wire ID and zero actor identity stay unchanged");
	check(attached(g, V_BOAT, s->guest_entity_id, guest_ident(s)),
		"owner: occupancy writer publishes guest wire ID AND full lifetime");
	s->player.rider_vel_x = .25F; s->player.rider_vel_z = -.5F;
	s->guest_actor->rider_vel_x = .125F; s->guest_actor->rider_vel_z = .375F;
	g->data.boat.rider_vel_x = .125F; g->data.boat.rider_vel_z = .375F;
	server_local_set_boat_occupancy(s, g, false); /* real force-release wrapper */
	clin_update_drain();
	check(detached(g, V_BOAT) && !s->guest_actor->riding_boat
		&& s->guest_actor->rider_vel_x == 0 && s->guest_actor->rider_vel_z == 0,
		"owner: release clears both reciprocal relation and steering copies");
	check(attached(h, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST)
		&& s->player.riding_boat && s->player.rider_vel_x == .25F
		&& s->player.rider_vel_z == -.5F,
		"owner: guest release preserves host mount and steering");
	teardown(s);
}

static void case_other_rider(unsigned kind_value) {
	enum vehicle_kind kind = (enum vehicle_kind)kind_value;
	assert(kind_value < 3);
	struct server_local* s = admitted_fixture();
	/* Both targets are within reach: refusal must be occupancy ownership,
	 * not a distant target or absent guest mount. */
	body_stand_at(s->guest_actor, 2.5, .5);
	struct entity* h = prop(s, kind, &s->player);
	struct entity* g = prop(s, kind, s->guest_actor);
	host_mount(s, h, kind); seed_guest(s, g, kind);
	if(kind == V_BOAT) g->data.boat.occupant_id = s->guest_entity_id; /* valid-wire injection */
	assert(attached(h, kind, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST));
	uint32_t ht = s->player.correction_seq, gt = s->guest_actor->correction_seq;
	host_mount(s, g, kind); /* actual reciprocal theft request, no conditional skip */
	check(attached(h, kind, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST)
		&& attached(g, kind, s->guest_entity_id, guest_ident(s)),
		"negative owner: host cannot steal seeded guest vehicle or release its own ride");
	struct server_rpc request = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = h->id, .desired_occupied = true}};
	if(kind == V_CART) request = (struct server_rpc){.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {.entity_id = h->id,
			.input_epoch = s->input_epoch_counter, .request_seq = 1, .desired_occupied = true}};
	bool queued = guest_send(s, &request);
	check(queued == (kind != V_CART), "closed ingress: guest cart verbs remain refused, boat/pig use class unchanged");
	check(attached(h, kind, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST)
		&& attached(g, kind, s->guest_entity_id, guest_ident(s))
		&& s->player.correction_seq == ht && s->guest_actor->correction_seq == gt,
		"negative owner: guest request cannot mutate host or own live relation/tokens");
	if(kind == V_CART) check(g->data.minecart.ride_epoch == 91
		&& g->data.minecart.control_seq == 0
		&& g->data.minecart.control_positive_steps == 0
		&& g->data.minecart.control_negative_steps == 0,
		"negative owner: refused request does not rotate another rider's epoch or cumulative tuple");
	teardown(s);
}

static void case_switch(unsigned pair, bool separate, bool guest_switch) {
	enum vehicle_kind from = (enum vehicle_kind)(pair / 3);
	enum vehicle_kind to = (enum vehicle_kind)(pair % 3);
	assert(from != to);
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, .5);
	struct server_player* actor = guest_switch ? s->guest_actor : &s->player;
	struct entity* previous = prop(s, from, actor);
	struct entity* partner;
	if(separate) {
		/* Owner-only fixture injection of a committed membership. No
		 * transfer/capacity success is claimed here. Its
		 * mount uses a real constructor and the existing world-membership seam. */
		string_set_str(s->level_name, split_path);
		struct server_world* w = server_local_world_admit(s, WORLD_DIM_NETHER); assert(w);
		s->guest_actor->world_slot = w->world_index;
		s->guest_actor->dimension = w->dimension;
		uint32_t id = server_local_gen_id(s);
		partner = calloc(1, sizeof(*partner)); assert(partner);
		if(from == V_BOAT) entity_boat(id, partner, true, w);
		else if(from == V_PIG) entity_monster(id, partner, true, w, MONSTER_PIG);
		else entity_minecart(id, partner, true, w);
		partner->teleport(partner, (vec3){7.5F, 4, .5F});
		struct server_chunk* loaded = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
		if(!loaded) assert(server_world_load_chunk(w, 0, 0, &loaded));
		assert(loaded);
		*dict_entity_safe_get(s->entities, id) = partner;
		assert(server_local_test_entity_join(s, id));
		if(from == V_PIG) partner->data.monster.saddled = true;
	} else partner = prop(s, from, guest_switch ? &s->player : s->guest_actor);
	struct entity* next = prop(s, to, actor);
	if(guest_switch) {
		if(from == V_CART) seed_guest(s, previous, from); /* existing cart seam */
		else guest_mount(s, previous);
		host_mount(s, partner, from);
	} else {
		host_mount(s, previous, from);
		seed_guest(s, partner, from);
	}
	assert(attached(previous, from, server_local_actor_wire_id(s, actor),
		server_local_actor_ident(s, actor)));
	s->player.rider_vel_x = .25F; s->player.rider_vel_z = -.25F;
	s->guest_actor->rider_vel_x = .125F; s->guest_actor->rider_vel_z = .375F;
	struct server_player* other = guest_switch ? &s->player : s->guest_actor;
	uint32_t other_token = other->correction_seq;
	if(guest_switch && to == V_CART) {
		/* Guest cart admission is out of scope. A refused cart request must
		 * retain the admitted boat/pig, rather than faking a successful switch. */
		struct server_rpc rpc = {.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {.entity_id = next->id,
				.input_epoch = s->input_epoch_counter, .request_seq = 1,
				.desired_occupied = true}};
		check(!guest_send(s, &rpc) && attached(previous, from,
			s->guest_entity_id, guest_ident(s)) && detached(next, to),
			"switch: closed guest cart ingress retains the current admitted ride");
		check(attached(partner, from, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST)
			&& other->rider_vel_x == .25F && other->rider_vel_z == -.25F
			&& other->correction_seq == other_token,
			"switch refusal: host ride, steering and correction token are preserved");
		teardown(s); return;
	}
	if(guest_switch) guest_mount(s, next);
	else host_mount(s, next, to);
	check(attached(next, to, server_local_actor_wire_id(s, actor),
		server_local_actor_ident(s, actor)),
		"switch: production use/ride handler establishes the requested owner/kind");
	check(detached(previous, from), "switch: previous kind's reciprocal relation retires");
	check(actor->riding_boat == (to == V_BOAT)
		&& actor->riding_minecart == (to == V_CART),
		"switch: body flags agree with the sole live relation");
	check(actor->rider_vel_x == 0 && actor->rider_vel_z == 0,
		"switch: steering from the previous mount cannot leak into the new kind");
	check(attached(partner, from, server_local_actor_wire_id(s, other),
		server_local_actor_ident(s, other))
		&& other->rider_vel_x == (guest_switch ? .25F : .125F)
		&& other->rider_vel_z == (guest_switch ? -.25F : .375F)
		&& other->correction_seq == other_token,
		"switch: other actor's mount, steering and correction token are preserved");
	/* Release fixture guest before retiring its possible secondary world. */
	guest_release(s); server_local_worlds_discard_secondary(s); teardown(s);
}

/* RetroMCP EntityBoat.interact:336-345 / EntityPig.interact:36-43:
 * only the acting player may release an occupied mount; pig use toggles,
 * whereas the existing local boat desired-state protocol is retry-safe. */
static void trace_field(const void* value, size_t size) {
	const unsigned char* bytes = value;
	putchar(' ');
	for(size_t i = 0; i < size; i++) printf("%02x", (unsigned)bytes[i]);
}
#define TRACE_FIELD(object, member) trace_field(&(object)->member, sizeof((object)->member))
static void trace_rpc(const struct client_rpc* rpc, void* unused) {
	consumed(rpc, unused);
	if(rpc->type == CRPC_BOAT_CORRECTION) {
		const struct boat_state_rpc* b = &rpc->payload.boat_state;
		printf("TRACE boat-rpc");
		TRACE_FIELD(b, entity_id); TRACE_FIELD(b, pos); TRACE_FIELD(b, vel);
		TRACE_FIELD(b, yaw); TRACE_FIELD(b, damage_taken); TRACE_FIELD(b, shake_ticks);
		TRACE_FIELD(b, shake_dir); TRACE_FIELD(b, occupied); TRACE_FIELD(b, occupant_id);
		TRACE_FIELD(b, on_ground); putchar('\n');
	} else if(rpc->type == CRPC_ENTITY_MOVE) {
		printf("TRACE move-rpc");
		TRACE_FIELD(rpc, payload.entity_move.entity_id);
		TRACE_FIELD(rpc, payload.entity_move.pos);
		TRACE_FIELD(rpc, payload.entity_move.pig_ridden);
		TRACE_FIELD(rpc, payload.entity_move.pig_rider_id); putchar('\n');
	} else if(rpc->type == CRPC_PLAYER_POS) {
		printf("TRACE player-rpc");
		TRACE_FIELD(rpc, payload.player_pos.position); TRACE_FIELD(rpc, payload.player_pos.correction_seq);
		TRACE_FIELD(rpc, payload.player_pos.correction_flags); putchar('\n');
	}
}
static void case_host_trace(unsigned kind_value) {
	assert(kind_value < 2);
	enum vehicle_kind kind = (enum vehicle_kind)kind_value;
	struct server_local* s = split_fixture();
	s->paused = true;
	stream_to_marker(s); acknowledge(s);
	assert(!s->guest_actor && !s->split_enabled);
	struct entity* e = prop(s, kind, &s->player);
	if(kind == V_BOAT) e->teleport(e, (vec3){1.5F, 4.7F, .5F});
	clin_test_set_consume_observer(trace_rpc, NULL);
	host_mount(s, e, kind);
	struct entity* body = gstate.players[0].local_player;
	assert(body); body->data.local_player.capture_input = false;
	for(unsigned n = 0; n < 12; n++) {
		s->player.rider_vel_x = .125F; s->player.rider_vel_z = -.0625F;
		assert(!e->tick_server(e, s)); clin_update_drain();
		assert(!body->tick_client(body));
		struct camera_player_view view = {.world = body->world,
			.ride = &gstate.players[0].ride, .yaw = body->orient[0], .pitch = body->orient[1]};
		camera_attach_owned(&gstate.players[0].camera, body, 1, 0, &view);
		printf("TRACE state %u", n);
		TRACE_FIELD(e, pos); TRACE_FIELD(e, vel); TRACE_FIELD(e, orient);
		TRACE_FIELD(e, gameplay_rng.seed); TRACE_FIELD(e, gameplay_rng.have_next_gaussian);
		TRACE_FIELD(e, gameplay_rng.next_gaussian_cached); TRACE_FIELD(e, health);
		TRACE_FIELD(body, pos); TRACE_FIELD(body, pos_old); TRACE_FIELD(body, vel);
		TRACE_FIELD(body, data.local_player.walk_dist);
		TRACE_FIELD((&s->player), x); TRACE_FIELD((&s->player), y); TRACE_FIELD((&s->player), z);
		TRACE_FIELD((&s->player), correction_seq);
		TRACE_FIELD((&gstate.players[0]), camera.x); TRACE_FIELD((&gstate.players[0]), camera.y);
		TRACE_FIELD((&gstate.players[0]), camera.z); putchar('\n');
	}
	struct server_rpc use = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = e->id, .desired_occupied = false}};
	host_send(s, &use);
	check(detached(e, kind), "SP trace: production mount, ticks, publication, follow/camera and dismount survive");
	clin_test_set_consume_observer(consumed, NULL); teardown(s);
}

static void case_admission(unsigned kind_value, bool disconnect) {
	assert(kind_value < 2);
	enum vehicle_kind kind = (enum vehicle_kind)kind_value;
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	body_stand_at(g, 2.5, .5);
	struct entity* host = prop(s, V_BOAT, &s->player);
	struct entity* target = prop(s, kind, g);
	host_mount(s, host, V_BOAT);
	struct server_rpc use = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = target->id, .desired_occupied = true}};
	uint32_t ht = s->player.correction_seq, gt = g->correction_seq;
	vec3 home; glm_vec3_copy(target->pos, home);
	if(kind == V_PIG) {
		target->data.monster.saddled = false;
		assert(guest_send(s, &use));
		check(detached(target, kind), "admission: unsaddled pig cannot be ridden");
		target->data.monster.saddled = true;
	}
	target->teleport(target, (vec3){home[0] + 20, home[1], home[2]});
	assert(guest_send(s, &use));
	check(detached(target, kind), "admission: out-of-reach use is refused");
	target->teleport(target, (vec3){home[0] + 1, home[1], home[2]});
	for(int y = 4; y < 8; y++) { put_block(s, 3, y, 0, BLOCK_STONE, 0); clin_update_drain(); }
	assert(guest_send(s, &use));
	check(detached(target, kind), "admission: own-world wall blocks use");
	for(int y = 4; y < 8; y++) { put_block(s, 3, y, 0, BLOCK_AIR, 0); clin_update_drain(); }
	target->teleport(target, home);
	string_set_str(s->level_name, split_path);
	struct server_world* other = server_local_world_admit(s, WORLD_DIM_NETHER); assert(other);
	uint8_t slot = g->world_slot; enum world_dim dim = g->dimension;
	g->world_slot = other->world_index; g->dimension = other->dimension;
	assert(guest_send(s, &use));
	check(detached(target, kind), "admission: live target in another world is refused");
	g->world_slot = slot; g->dimension = dim;
	struct server_rpc_source stale = guest_source(s);
	stale.generation ^= UINT64_C(1) << 32;
	/* The transport accepts a tagged gameplay class; the production owned
	 * dispatch, rather than the queue, validates its full lifetime. */
	assert(svin_rpc_send_from(stale, &use));
	drain_server(s);
	check(detached(target, kind) && g->correction_seq == gt,
		"admission: stale full 64-bit source/invalid requests leave relation and token unchanged");
	guest_mount(s, target);
	check(attached(target, kind, s->guest_entity_id, guest_ident(s)),
		"admission: production guest ENTITY_USE establishes full owner identity");
	if(kind == V_BOAT) {
		guest_mount(s, target);
		check(attached(target, kind, s->guest_entity_id, guest_ident(s)),
			"admission: duplicate boat desired=true preserves the admitted rider");
		uint32_t wire = target->data.boat.occupant_id;
		target->data.boat.occupant_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		use.payload.entity_use.desired_occupied = false;
		assert(guest_send(s, &use));
		check(target->data.boat.occupied && g->riding_boat,
			"admission: same actor with a mismatched wire ID cannot release the boat");
		target->data.boat.occupant_id = wire;
	} else {
		guest_mount(s, target);
		check(detached(target, kind), "admission: repeated pig interaction toggles off per Beta");
		guest_mount(s, target);
	}
	host_mount(s, target, kind);
	check(attached(target, kind, s->guest_entity_id, guest_ident(s))
		&& attached(host, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST),
		"admission: host cannot steal the guest ride or release its own mount");
	struct server_rpc steal = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = host->id, .desired_occupied = true}};
	assert(guest_send(s, &steal)); steal.payload.entity_use.desired_occupied = false;
	assert(guest_send(s, &steal));
	check(attached(target, kind, s->guest_entity_id, guest_ident(s))
		&& attached(host, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST),
		"admission: guest mount/release retries cannot steal the host ride");
	if(disconnect) {
		guest_release(s);
		check(detached(target, kind), "disconnect: admitted ride retires before guest invalidation");
	} else {
		use.payload.entity_use.desired_occupied = false;
		assert(guest_send(s, &use));
		check(detached(target, kind) && !g->riding_boat && !g->riding_minecart,
			"admission: production dismount clears only the acting guest");
		if(kind == V_BOAT) {
			assert(guest_send(s, &use));
			check(detached(target, kind), "admission: duplicate boat desired=false remains detached");
		}
	}
	check(s->player.riding_boat && s->player.correction_seq == ht
		&& attached(host, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST),
		"admission: host mount/body correction is unchanged throughout guest requests");
	server_local_worlds_discard_secondary(s); teardown(s);
}

static void case_negative(const char* which) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, .5);
	struct entity* host = prop(s, V_BOAT, &s->player);
	struct entity* guest = prop(s, V_BOAT, s->guest_actor);
	host_mount(s, host, V_BOAT);
	seed_guest(s, guest, V_BOAT);
	guest->data.boat.occupant_id = s->guest_entity_id; /* valid-wire fixture injection */
	s->player.rider_vel_x = .25F;
	s->guest_actor->rider_vel_x = .125F;
	uint32_t ht = s->player.correction_seq, gt = s->guest_actor->correction_seq;
	double hx = s->player.x, gx = s->guest_actor->x;
	uint8_t original_slot = s->guest_actor->world_slot;
	enum world_dim original_dimension = s->guest_actor->dimension;
	if(!strcmp(which, "wrong-world")) {
		/* Inject only a committed membership boundary; tick must re-resolve
		 * the relation's world instead of accepting a live actor in any world. */
		string_set_str(s->level_name, split_path);
		struct server_world* other = server_local_world_admit(s, WORLD_DIM_NETHER); assert(other);
		s->guest_actor->world_slot = other->world_index;
		s->guest_actor->dimension = WORLD_DIM_NETHER;
	} else {
		/* Same wire entity ID, another full lifetime (including high bits).
		 * No freed actor pointer is dereferenced and no actual rejoin is faked. */
		guest->data.boat.rider_actor.generation ^= UINT64_C(1) << 32;
	}
	assert(guest->tick_server);
	check(!guest->tick_server(guest, s), "negative: hull survives relation invalidation");
	clin_update_drain();
	check(detached(guest, V_BOAT),
		"negative: wrong world / stale lifetime with reused wire ID cannot remain attached");
	check(attached(host, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST)
		&& s->player.riding_boat && s->player.rider_vel_x == .25F
		&& s->player.correction_seq == ht && s->player.x == hx,
		"negative: invalid relation never mutates the other actor");
	check(s->guest_actor->correction_seq == gt && s->guest_actor->x == gx,
		"negative: invalid relation cannot issue a seat correction to current guest");
	s->guest_actor->world_slot = original_slot;
	s->guest_actor->dimension = original_dimension;
	server_local_worlds_discard_secondary(s);
	teardown(s);
}

static void case_destroy(void) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, .5);
	struct entity* host = prop(s, V_BOAT, &s->player);
	struct entity* guest = prop(s, V_BOAT, s->guest_actor);
	host_mount(s, host, V_BOAT);
	seed_guest(s, guest, V_BOAT);
	guest->data.boat.occupant_id = s->guest_entity_id; /* valid-wire fixture injection */
	s->guest_actor->rider_vel_x = .125F;
	/* Damage threshold injection selects the real next-tick destruction path;
	 * destruction/drop/release behavior is entirely production code. */
	guest->data.boat.damage_taken = BOAT_DESTROY_DAMAGE_THRESHOLD + 1;
	check(guest->tick_server(guest, s), "destroy: real damaged-hull tick requests destruction");
	clin_update_drain();
	check(detached(guest, V_BOAT) && !s->guest_actor->riding_boat
		&& s->guest_actor->rider_vel_x == 0,
		"destroy: matching rider and reciprocal hull relation settle before reap");
	check(s->player.riding_boat
		&& attached(host, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST),
		"destroy: guest hull destruction preserves the host hull");
	teardown(s);
}

static void stationary_pig_tick(struct server_local* s, struct entity* pig) {
	/* Fixture injection to hold an unchanged seat across refusal/retry: select
	 * neutral AI with the REAL RNG API, clear initial motion, and teleport to
	 * the same grounded cell. No correction/body fields are reset here. */
	struct java_random probe;
	int64_t seed = 1;
	for(;; seed++) {
		jrand_set_seed(&probe, seed);
		if(jrand_next_int_bound(&probe, 80) && jrand_next_int_bound(&probe, 80)
			&& jrand_next_int_bound(&probe, 100)) break;
	}
	jrand_set_seed(&pig->gameplay_rng, seed);
	vec3 here = {7.5F, 4, .5F};
	pig->teleport(pig, here);
	glm_vec3_zero(pig->vel);
	pig->data.monster.motion_d.motion[0] = 0;
	pig->data.monster.motion_d.motion[1] = 0;
	pig->data.monster.motion_d.motion[2] = 0;
	pig->data.monster.direction[0] = pig->data.monster.direction[1] = 0;
	pig->data.monster.living_sound_time = -10000;
	pig->on_ground = true;
	check(!pig->tick_server(pig, s), "pig: actual ridden server tick survives");
}

static uint64_t pig_route_generation, pig_route_ticket;
static uint8_t pig_route_owner;
static void pig_consumed(const struct client_rpc* call, void* user) {
	consumed(call, user);
	if(call->type == CRPC_LOCAL_GUEST
		&& call->payload.local_guest.result == LOCAL_GUEST_CORRECTION) {
		pig_route_generation = call->payload.local_guest.generation;
		pig_route_ticket = call->payload.local_guest.ticket;
		pig_route_owner = call->payload.local_guest.owner;
	}
}
static void case_pig_negative(bool wrong_world) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, .5);
	struct entity* host = prop(s, V_BOAT, &s->player);
	struct entity* pig = prop(s, V_PIG, s->guest_actor);
	host_mount(s, host, V_BOAT); seed_guest(s, pig, V_PIG);
	struct server_player* g = s->guest_actor;
	uint8_t original_slot = g->world_slot;
	enum world_dim original_dimension = g->dimension;
	uint32_t gt = g->correction_seq, ht = s->player.correction_seq;
	double gx = g->x, gy = g->y, gz = g->z;
	if(wrong_world) {
		/* Inject a valid committed guest membership in another live world;
		 * the pig stays in its original world, so the relation is invalid. */
		string_set_str(s->level_name, split_path);
		struct server_world* other = server_local_world_admit(s, WORLD_DIM_NETHER); assert(other);
		g->world_slot = other->world_index; g->dimension = other->dimension;
	} else pig->data.monster.pig_rider_actor.generation ^= UINT64_C(1) << 32;
	counters_reset(); stationary_pig_tick(s, pig); clin_update_drain();
	check(!pig->data.monster.pig_ridden && !pig->data.monster.pig_rider_id
		&& actor_ident_is_host(pig->data.monster.pig_rider_actor),
		"pig negative: real tick retires wrong-world/stale-lifetime relation despite reused wire ID");
	check(g->x == gx && g->y == gy && g->z == gz && g->correction_seq == gt
		&& !guest_corrections && !host_pos_msgs && s->player.correction_seq == ht,
		"pig negative: invalid rider cannot mutate guest pose/token or issue host correction");
	check(s->player.riding_boat && attached(host, V_BOAT, SERVER_LOCAL_PLAYER_ENTITY_ID, ACTOR_IDENT_HOST),
		"pig negative: live host ride survives invalid guest pig relation");
	g->world_slot = original_slot; g->dimension = original_dimension;
	server_local_worlds_discard_secondary(s); teardown(s);
}
static void case_pig_correction(bool pressure) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, .5);
	struct entity* host = prop(s, V_BOAT, &s->player);
	struct entity* pig = prop(s, V_PIG, s->guest_actor);
	host_mount(s, host, V_BOAT); seed_guest(s, pig, V_PIG);
	struct server_player* g = s->guest_actor;
	clin_test_set_consume_observer(pig_consumed, NULL);
	if(pressure) {
		/* Warm the unrelated ordinary mob-move publisher by an ACTUAL tick
		 * before starving tokens. Its first snapshot is blocking in preimage;
		 * that is not the private seat-correction retry seam. Then a real
		 * current-token guest pose creates the seat discrepancy under test. */
		stationary_pig_tick(s, pig); clin_update_drain(); step(s);
		guest_sample(s, 7.25, g->y, .5, true, g->correction_seq);
		assert(g->x == 7.25 && g->z == .5);
	}
	uint32_t ht = s->player.correction_seq, gt = g->correction_seq;
	double hx = s->player.x, hy = s->player.y, hz = s->player.z;
	struct camera hc = gstate.players[0].camera;
	/* A stale pose is actually queued before correction, to test owner-only
	 * invalidation. It is not a hand-authored successful steering witness. */
	struct server_rpc stale = pose_sample(g->x + .25, g->y, g->z, true, gt);
	assert(svin_rpc_send_from(guest_source(s), &stale));
	if(pressure) {
		/* try_send_local accepts only LOCAL_GUEST envelopes. Use an accepted
		 * shape with a stale ticket, which the real client ignores on drain. */
		struct client_rpc harmless = {.type = CRPC_LOCAL_GUEST,
			.payload.local_guest = {.owner = 1, .generation = s->guest_generation,
				.ticket = s->guest_ticket ^ UINT64_C(1), .result = LOCAL_GUEST_ATTACHED}};
		unsigned n = 0;
		while(clin_try_rpc_send_local(&harmless, 1, s->guest_generation)) n++;
		assert(n > 0 && !clin_rpc_headroom(1));
	}
	counters_reset();
	stationary_pig_tick(s, pig);
	check(g->x == (double)pig->pos[0]
		&& g->y == (double)pig->pos[1] + (double)PIG_MOUNTED_Y_OFFSET + (double)PLAYER_EYE_HEIGHT
		&& g->z == (double)pig->pos[2], "pig: actual seat correction moves only the guest body");
	if(pressure) {
		check(g->correction_seq == gt, "pig retry: refused private send preserves the unacknowledged token");
		clin_update_drain(); counters_reset();
		double x = g->x, y = g->y, z = g->z;
		stationary_pig_tick(s, pig);
		check(g->x == x && g->y == y && g->z == z,
			"pig retry: next actual tick has unchanged seat coordinates");
	}
	clin_update_drain();
	check(guest_corrections == 1 && g->correction_seq != gt
		&& guest_corr_seq == g->correction_seq
		&& pig_route_owner == 1 && pig_route_ticket == s->guest_ticket
		&& pig_route_generation == s->guest_generation
		&& guest_corr_pos[0] == g->x && guest_corr_pos[1] == g->y && guest_corr_pos[2] == g->z,
		"pig: one correction delivers on guest lifetime/token route, including retry at equal coordinates");
	check(gstate.players[1].player_sync.correction_seq == g->correction_seq
		&& near3(gstate.players[1].local_player->pos, g->x, g->y, g->z),
		"pig: real client applies correction to this guest body, not merely consumes an envelope");
	check(host_pos_msgs == 0 && s->player.correction_seq == ht
		&& s->player.x == hx && s->player.y == hy && s->player.z == hz
		&& gstate.players[0].camera.x == hc.x && gstate.players[0].camera.y == hc.y
		&& gstate.players[0].camera.z == hc.z && s->player.riding_boat,
		"pig: no host correction/body/token/camera/mount mutation");
	double corrected_x = g->x, corrected_y = g->y, corrected_z = g->z;
	drain_server(s);
	check(g->x == corrected_x && g->y == corrected_y && g->z == corrected_z,
		"pig: pre-correction queued guest pose cannot undo the seat correction");
	/* Delayed same-wire-ID traffic with an old full lifetime is refused. */
	struct server_rpc_source departed = guest_source(s);
	departed.generation ^= UINT64_C(1) << 32;
	(void)svin_rpc_send_from(departed, &stale); drain_server(s);
	check(g->x == corrected_x && g->correction_seq == guest_corr_seq
		&& s->player.correction_seq == ht,
		"pig: stale-generation queued pose cannot mutate current guest or host token");
	teardown(s);
}

/* R02 F1: malformed relation names a live rider of another vehicle. The
 * actor identity alone is real; its wire identity is deliberately wrong. */
static void case_invalid_release(unsigned kind_value, bool host_rider) {
	assert(kind_value == V_BOAT || (kind_value == V_CART && !host_rider));
	enum vehicle_kind kind = (enum vehicle_kind)kind_value;
	struct server_local* s = admitted_fixture();
	struct server_player* rider = host_rider ? &s->player : s->guest_actor;
	struct actor_ident actor = server_local_actor_ident(s, rider);
	uint32_t wire = server_local_actor_wire_id(s, rider);
	struct entity* live = prop(s, kind, rider);
	struct entity* invalid = prop(s, kind, rider);
	if(host_rider) host_mount(s, live, kind);
	else {
		seed_guest(s, live, kind);
		if(kind == V_CART) live->data.minecart.rider_actor = actor;
	}
	assert(server_local_vehicle_rider(s, live, actor, wire) == rider);
	rider->rider_vel_x = .125F; rider->rider_vel_z = -.25F;
	if(kind == V_BOAT) {
		live->data.boat.rider_vel_x = .125F;
		live->data.boat.rider_vel_z = -.25F;
		invalid->data.boat.occupied = true;
		invalid->data.boat.rider_actor = actor;
		invalid->data.boat.occupant_id = host_rider ? s->guest_entity_id : 0;
		assert(!server_local_vehicle_rider(s, invalid, actor, invalid->data.boat.occupant_id));
		check(!invalid->tick_server(invalid, s), "F1: invalid boat relation reaches the real tick cleanup");
		clin_update_drain();
	} else {
		invalid->data.minecart.occupied = true;
		invalid->data.minecart.rider_actor = actor;
		invalid->data.minecart.occupant_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		assert(!server_local_vehicle_rider(s, invalid, actor, invalid->data.minecart.occupant_id));
		struct server_rpc release = {.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {.entity_id = invalid->id,
				.input_epoch = s->input_epoch_counter, .request_seq = 1,
				.desired_occupied = false}};
		host_send(s, &release);
	}
	if(kind == V_BOAT) check(detached(invalid, kind), "F1: boat tick retires the malformed relation");
	check(attached(live, kind, wire, actor)
		&& (kind == V_BOAT ? rider->riding_boat : rider->riding_minecart)
		&& rider->rider_vel_x == .125F && rider->rider_vel_z == -.25F,
		"F1: invalid release preserves another vehicle's valid rider flag and steering");
	if(kind == V_BOAT) check(live->data.boat.rider_vel_x == .125F
		&& live->data.boat.rider_vel_z == -.25F,
		"F1: valid hull steering remains unchanged");
	teardown(s);
}

static void case_stale_vehicle(unsigned kind_value) {
	assert(kind_value == V_BOAT || kind_value == V_CART);
	enum vehicle_kind kind = (enum vehicle_kind)kind_value;
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_world* old = server_local_world_admit(s, WORLD_DIM_NETHER); assert(old);
	uint32_t id = server_local_gen_id(s);
	struct entity* stale = calloc(1, sizeof(*stale)); assert(stale);
	/* Retain an entity outside the dictionary across actual slot retirement.
	 * Its raw world pointer will name the successor, but its stamp stays old. */
	if(kind == V_BOAT) entity_boat(id, stale, true, old);
	else entity_minecart(id, stale, true, old);
	uint32_t generation = stale->world_generation;
	uint8_t slot = stale->world_slot;
	assert(generation);
	server_local_worlds_discard_secondary(s);
	struct server_world* next = server_local_world_admit(s, WORLD_DIM_NETHER); assert(next);
	assert(next == old && next->world_index == slot && next->generation != generation);
	struct server_player* g = s->guest_actor;
	g->world_slot = slot; g->dimension = next->dimension;
	stale->teleport(stale, (vec3){(float)g->x, 4, (float)g->z});
	*dict_entity_safe_get(s->entities, id) = stale;
	struct actor_ident actor = guest_ident(s);
	if(kind == V_BOAT) {
		stale->data.boat.occupied = true; stale->data.boat.occupant_id = s->guest_entity_id;
		stale->data.boat.rider_actor = actor; g->riding_boat = true;
	} else {
		stale->data.minecart.occupied = true; stale->data.minecart.occupant_id = s->guest_entity_id;
		stale->data.minecart.rider_actor = actor; g->riding_minecart = true;
	}
	assert(!server_local_entity_world(s, stale));
	check(!server_local_vehicle_rider(s, stale, actor, s->guest_entity_id),
		"F2: retired vehicle generation cannot resolve a successor-world rider");
	check(!server_local_actor_vehicle(s, g), "F2: stale generation cannot authorize a seat reference");
	check(!server_local_actor_mounts_valid(s, g), "F2: stale generation cannot validate a mounted snapshot");
	check(!server_local_guest_checkpoint(s), "F2: actual guest snapshot rejects the stale-only mount");
	/* Retire injected obsolete storage explicitly; no successful release is
	 * inferred from cleanup after these negative observations. */
	g->riding_boat = g->riding_minecart = false;
	dict_entity_erase(s->entities, id); free(stale);
	g->world_slot = 0; g->dimension = WORLD_DIM_OVERWORLD;
	guest_release(s); server_local_worlds_discard_secondary(s); teardown(s);
}

static unsigned frozen_pig_ticks;
static bool (*frozen_pig_real_tick)(struct entity*, struct server_local*);
static bool frozen_pig_count_tick(struct entity* e, struct server_local* s) {
	frozen_pig_ticks++;
	return frozen_pig_real_tick(e, s);
}
static void case_frozen_pig_anchor(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	struct entity* pig = prop(s, V_PIG, g);
	/* Legitimate frozen-tick seam: the far home column is loaded, but the
	 * pig's +/-32 block neighborhood is missing. It awaits chunk attachment. */
	pig->teleport(pig, (vec3){513.5F, 4, .5F});
	struct server_chunk* column = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(32, 0));
	if(!column) assert(server_world_load_chunk(&s->worlds[0], 32, 0, &column));
	assert(column);
	pig->added_to_chunk = false;
	seed_guest(s, pig, V_PIG);
	g->x = pig->pos[0]; g->z = pig->pos[2];
	g->y = (double)pig->pos[1] + PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT;
	assert(server_local_vehicle_rider(s, pig, guest_ident(s), s->guest_entity_id) == g);
	frozen_pig_real_tick = pig->tick_server; pig->tick_server = frozen_pig_count_tick;
	s->paused = false; tick(s);
	assert(!frozen_pig_ticks && !g->reject_pos_this_update);
	assert(s->residency_anchors[1].x == 32);
	for(unsigned i = 0; i < 20; i++)
		guest_sample(s, g->x + 1.0, g->y, g->z, true, g->correction_seq);
	tick(s);
	check(frozen_pig_ticks == 0, "F3: real updates skip the pig's authoritative tick at the missing resident boundary");
	check(fabs(g->x - (double)pig->pos[0]) < 2.0,
		"F3: repeated small owned samples cannot walk the frozen pig's seat reference away");
	check(s->residency_anchors[1].x == 32,
		"F3: actual update keeps anchor 1 in the fixed pig seat's column");
	pig->tick_server = frozen_pig_real_tick;
	teardown(s);
}

static void case_pig_walking(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	struct entity* pig = prop(s, V_PIG, g);
	seed_guest(s, pig, V_PIG);
	step(s); /* retire the gameplay admission's reject-pos latch */
	g->x = pig->pos[0]; g->z = pig->pos[2];
	g->y = (double)pig->pos[1] + PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT;
	assert(server_local_vehicle_rider(s, pig, guest_ident(s), s->guest_entity_id) == g);
	int x = (int)floor(g->x + .25), y = (int)floor(g->y - (1.62 + .2)), z = (int)floor(g->z);
	put_block(s, x, y, z, 73, 0); clin_update_drain(); /* unlit redstone ore */
	g->distance_walked = 1.05F; g->next_step_distance = 1;
	struct server_player host = s->player;
	counters_reset();
	guest_sample(s, g->x + .25, g->y, g->z, true, g->correction_seq);
	check(g->distance_walked == 1.05F && g->next_step_distance == 1,
		"F6: valid guest pig rider skips the authoritative walking odometer leg");
	check(!sound_msgs, "F6: valid guest pig rider emits no footstep sound");
	check(block_at(s, x, y, z) == 73,
		"F6: valid guest pig rider cannot invoke onEntityWalking's ore glow callback");
	check(s->player.x == host.x && s->player.y == host.y && s->player.z == host.z
		&& s->player.health == host.health && s->player.distance_walked == host.distance_walked,
		"F6: guest sample preserves host body and odometer");
	teardown(s);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "wire-ids";
	harness_init();
	if(!strcmp(which, "wire-ids")) case_wire_ids();
	else if(!strcmp(which, "host-trace")) case_host_trace(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	else if(!strcmp(which, "admission") || !strcmp(which, "disconnect"))
		case_admission(argc > 2 ? (unsigned)atoi(argv[2]) : 0, !strcmp(which, "disconnect"));
	else if(!strcmp(which, "other-rider")) case_other_rider(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	else if(!strcmp(which, "switch") && argc >= 3)
		case_switch((unsigned)atoi(argv[2]), argc > 3 && atoi(argv[3]) != 0, false);
	else if(!strcmp(which, "guest-switch") && argc >= 3)
		case_switch((unsigned)atoi(argv[2]), false, true);
	else if(!strcmp(which, "wrong-world") || !strcmp(which, "stale-lifetime")) case_negative(which);
	else if(!strcmp(which, "destroy")) case_destroy();
	else if(!strcmp(which, "pig-correction")) case_pig_correction(false);
	else if(!strcmp(which, "pig-retry")) case_pig_correction(true);
	else if(!strcmp(which, "pig-wrong-world")) case_pig_negative(true);
	else if(!strcmp(which, "pig-stale-lifetime")) case_pig_negative(false);
	else if(!strcmp(which, "invalid-release")) case_invalid_release(argc > 2 ? (unsigned)atoi(argv[2]) : 0,
		argc > 3 && atoi(argv[3]) != 0);
	else if(!strcmp(which, "stale-vehicle")) case_stale_vehicle(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	else if(!strcmp(which, "frozen-pig-anchor")) case_frozen_pig_anchor();
	else if(!strcmp(which, "pig-walking")) case_pig_walking();
	else { fprintf(stderr, "unknown vehicle owner case: %s\n", which); return 2; }
	return harness_result("test_vehicle_owner_seams", which);
}
