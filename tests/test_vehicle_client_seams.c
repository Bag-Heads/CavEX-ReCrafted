/* G8-VEHICLE-SEAMS-01 / ride-r1-1008: actual owned body/input/pose,
 * correction consumers and world publishers. Ordinary positive client cases
 * are separate from the deliberate guest ENTITY_USE refusals in the two
 * owner-swap tests. One case/process; injected authority is labelled below. */
#include "support/owner_swap_harness.h"
#include "entity/entity_boat.h"
#include "entity/monsters_object.h"
#include "game/camera.h"
#include "game/client_world_context.h"
#include "network/client_world_payload.h"
#include "network/server_transfer.h"

#define CAVEWII_F1_DISPATCH_ONLY
#include "../source/main.c"
#undef CAVEWII_F1_DISPATCH_ONLY

/* Script only the physical host input boundary; dispatch, physics and pose
 * publication remain production code, matching the guest frame fixture. */
static const struct input_player_frame* host_tick_frame;
static bool host_input_tick(struct entity* body) {
	return entity_local_player_walk_owned(body, &gstate.players[0], host_tick_frame);
}

#define SAME_FIELD(a, b, member) \
	(!memcmp(&(a)->member, &(b)->member, sizeof((a)->member)))

static void semantic_field(const char* name, const void* value, size_t size) {
	const unsigned char* bytes = value;
	printf(" %s=", name);
	for(size_t i = 0; i < size; i++) printf("%02x", (unsigned)bytes[i]);
}
#define LOG_FIELD(object, member) \
	semantic_field(#member, &(object)->member, sizeof((object)->member))
static void host_semantics(unsigned kind, const struct entity* e,
	const struct client_session* owner) {
	/* Complete compared-field packet for matched preimage/candidate runs.
	 * Every value is a scalar or packed scalar array; no pointer/padding bytes. */
	printf("HOST_SEMANTIC kind=%u", kind);
	LOG_FIELD(e, pos); LOG_FIELD(e, pos_old); LOG_FIELD(e, vel);
	LOG_FIELD(e, orient); LOG_FIELD(e, orient_old);
	LOG_FIELD(e, id); LOG_FIELD(e, type); LOG_FIELD(e, health);
	LOG_FIELD(e, on_ground); LOG_FIELD(e, collided_horizontally);
	LOG_FIELD(e, collided_vertically); LOG_FIELD(e, collided);
	LOG_FIELD(e, in_web); LOG_FIELD(e, on_fire);
	LOG_FIELD(e, y_offset); LOG_FIELD(e, y_size); LOG_FIELD(e, step_height);
	LOG_FIELD(e, fall_distance); LOG_FIELD(e, data.local_player.jump_ticks);
	LOG_FIELD(e, data.local_player.walk_dist); LOG_FIELD(e, data.local_player.walk_next_step);
	LOG_FIELD(e, data.local_player.boat_rider_vel_x); LOG_FIELD(e, data.local_player.boat_rider_vel_z);
	LOG_FIELD(owner, camera.x); LOG_FIELD(owner, camera.y); LOG_FIELD(owner, camera.z);
	LOG_FIELD(owner, camera.rx); LOG_FIELD(owner, camera.ry);
	LOG_FIELD(owner, camera.ysize.y_size); LOG_FIELD(owner, camera.ysize.y_size_prev);
	LOG_FIELD(owner, camera.ysize.applied); LOG_FIELD(owner, camera.ysize.pre_dip_y);
	LOG_FIELD(owner, camera.ysize.post_dip_y);
	LOG_FIELD(owner, oxygen); LOG_FIELD(owner, in_water); LOG_FIELD(owner, sleeping);
	LOG_FIELD(owner, ride.target_id); LOG_FIELD(owner, ride.pending);
	LOG_FIELD(owner, ride.pending_request_seq); LOG_FIELD(owner, ride.camera_correction);
	LOG_FIELD(owner, player_sync.correction_seq);
	putchar('\n');
}

static bool same_body(const struct entity* a, const struct entity* b) {
	/* Compare only defined semantic fields, never union/struct padding. */
	for(unsigned k = 0; k < 3; k++)
		if(memcmp(&a->pos[k], &b->pos[k], sizeof(float))
			|| memcmp(&a->pos_old[k], &b->pos_old[k], sizeof(float))
			|| memcmp(&a->vel[k], &b->vel[k], sizeof(float))) return false;
	for(unsigned k = 0; k < 2; k++)
		if(memcmp(&a->orient[k], &b->orient[k], sizeof(float))
			|| memcmp(&a->orient_old[k], &b->orient_old[k], sizeof(float))) return false;
	return a->id == b->id && a->type == b->type && a->health == b->health
		&& a->on_ground == b->on_ground
		&& a->collided_horizontally == b->collided_horizontally
		&& a->collided_vertically == b->collided_vertically
		&& a->collided == b->collided && a->in_web == b->in_web && a->on_fire == b->on_fire
		&& SAME_FIELD(a, b, y_offset) && SAME_FIELD(a, b, y_size)
		&& SAME_FIELD(a, b, step_height) && SAME_FIELD(a, b, fall_distance)
		&& a->data.local_player.jump_ticks == b->data.local_player.jump_ticks
		&& SAME_FIELD(a, b, data.local_player.walk_dist)
		&& a->data.local_player.walk_next_step == b->data.local_player.walk_next_step
		&& SAME_FIELD(a, b, data.local_player.boat_rider_vel_x)
		&& SAME_FIELD(a, b, data.local_player.boat_rider_vel_z);
}

static bool same_owner(const struct client_session* a, const struct client_session* b) {
	return SAME_FIELD(a, b, camera.x) && SAME_FIELD(a, b, camera.y)
		&& SAME_FIELD(a, b, camera.z) && SAME_FIELD(a, b, camera.rx)
		&& SAME_FIELD(a, b, camera.ry) && a->oxygen == b->oxygen
		&& SAME_FIELD(a, b, camera.ysize.y_size)
		&& SAME_FIELD(a, b, camera.ysize.y_size_prev)
		&& SAME_FIELD(a, b, camera.ysize.applied)
		&& SAME_FIELD(a, b, camera.ysize.pre_dip_y)
		&& SAME_FIELD(a, b, camera.ysize.post_dip_y)
		&& a->in_water == b->in_water && a->sleeping == b->sleeping
		&& a->ride.target_id == b->ride.target_id
		&& a->ride.pending == b->ride.pending
		&& a->ride.pending_request_seq == b->ride.pending_request_seq
		&& SAME_FIELD(a, b, ride.camera_correction[0])
		&& SAME_FIELD(a, b, ride.camera_correction[1])
		&& SAME_FIELD(a, b, ride.camera_correction[2])
		&& a->player_sync.correction_seq == b->player_sync.correction_seq;
}

static struct entity* mirrored(struct entity* server) {
	struct entity** e = dict_entity_get(world_view_entities(), server->id);
	assert(e && *e);
	return *e;
}

static struct entity* seeded_vehicle(struct server_local* s, unsigned kind) {
	struct server_player* g = s->guest_actor;
	vec3 pos = {2.5F, 4.0F, .5F};
	struct entity* e = kind == 0 ? server_local_spawn_boat(pos, s)
		: kind == 1 ? server_local_spawn_monster_at(pos, MONSTER_PIG, s)
		: server_local_spawn_minecart_type(pos, s, 0);
	assert(e); clin_update_drain();
	struct entity* c = mirrored(e);
	/* Fixture injection: valid authoritative guest relation behind the closed
	 * ENTITY_USE ingress. The client relation is delivered by the actual boat
	 * occupancy publisher or pig tick, never installed by a mirror write. */
	if(kind == 0) {
		server_local_set_boat_occupancy_for(s, e, g, true);
		clin_update_drain();
		assert(c->data.boat.occupied && c->data.boat.occupant_id == s->guest_entity_id);
		c->orient[0] = e->orient[0] = .25F;
	} else if(kind == 1) {
		e->data.monster.saddled = true;
		e->data.monster.pig_ridden = true;
		e->data.monster.pig_rider_actor = guest_ident(s);
		e->data.monster.pig_rider_id = s->guest_entity_id;
		assert(!e->tick_server(e, s));
		clin_update_drain();
		assert(c->data.monster.saddled && c->data.monster.pig_ridden
			&& c->data.monster.pig_rider_id == s->guest_entity_id);
	} else {
		e->data.minecart.occupied = c->data.minecart.occupied = true;
		e->data.minecart.occupant_id = c->data.minecart.occupant_id = s->guest_entity_id;
		g->riding_minecart = true;
		/* No cart control transaction/admission in step 1. */
		c->data.minecart.control_ready = false;
	}
	return e;
}

static void case_follow(unsigned kind) {
	struct server_local* s = admitted_fixture();
	struct client_session* g = &gstate.players[1];
	struct entity* body = g->local_player;
	struct entity* vehicle = seeded_vehicle(s, kind);
	struct entity* c = mirrored(vehicle);
	struct entity vehicle_before = *c;
	struct entity host_before = *gstate.players[0].local_player;
	struct client_session owner_before = gstate.players[0];
	float walked = body->data.local_player.walk_dist;
	/* Nonzero vertical motion and overdue footstep prove the riding branch
	 * suppresses gravity/walking rather than merely receiving an idle body. */
	body->vel[0] = .125F; body->vel[1] = -.375F; body->vel[2] = -.25F;
	body->data.local_player.walk_next_step = 0;
	float vx = body->vel[0], vy = body->vel[1], vz = body->vel[2];
	float server_walked = s->guest_actor->distance_walked;
	struct input_player_frame frame = {.connected = true, .category = INPUT_CAT_GAMECUBE};
	frame.down[IB_FORWARD] = frame.down[IB_RIGHT] = true;
	body->data.local_player.capture_input = true;
	counters_reset();
	/* EXACT preimage main.c P2 entry, with that session's actual input frame. */
	check(!entity_local_player_walk_owned(body, g, &frame), "client: owned tick keeps the body alive");
	float x = kind == 0 ? (float)((double)c->pos[0]
		+ cos((double)c->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET) : c->pos[0];
	float y = kind == 0 ? c->pos[1] + BOAT_MOUNTED_Y_OFFSET + body->y_offset + 1.62F
		: kind == 1 ? c->pos[1] + PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT : c->pos[1] + 1.0F;
	float z = kind == 0 ? (float)((double)c->pos[2]
		+ sin((double)c->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET) : c->pos[2];
	check(near3(body->pos, x, y, z) && near3(body->pos_old, x, y, z),
		"client: P2 current/previous body samples follow the real vehicle seat");
	check(body->data.local_player.walk_dist == walked && !sound_msgs,
		"client: riding suppresses walking and positional footsteps");
	if(kind == 0)
		check(body->vel[0] == vx && body->vel[1] == vy && body->vel[2] == vz,
			"client: boat tether preserves persistent body velocity");
	else check(body->vel[0] == 0 && body->vel[1] == 0 && body->vel[2] == 0,
		"client: pig/cart ridden update zeros body motion");
	struct camera_player_view view = {.world = body->world, .ride = &g->ride,
		.sleeping = g->sleeping, .yaw = body->orient[0], .pitch = body->orient[1]};
	camera_attach_owned(&g->camera, body, 1.0F, 0.0F, &view);
	check(fabs(g->camera.x - x) < 1e-4 && fabs(g->camera.y - y) < 1e-4
		&& fabs(g->camera.z - z) < 1e-4, "client: P2 camera follows its owned seat");
	if(kind == 0) {
		check(body->data.local_player.boat_rider_vel_x != 0
			&& body->data.local_player.boat_rider_vel_z != 0,
			"client: P2 forward/strafe frame produces nonzero X and Z boat steering");
		/* Only the guest executes a publication tick, preserving P0 bytes. */
		gstate.world_loaded = true; gstate.world_switch_pending = false; gstate.paused = false;
		clin_update(); main_client_guest_tick_dispatch(&frame); drain_server(s);
		check(s->guest_actor->rider_vel_x == body->data.local_player.boat_rider_vel_x
			&& s->guest_actor->rider_vel_z == body->data.local_player.boat_rider_vel_z
			&& s->guest_actor->rider_vel_x != 0 && s->guest_actor->rider_vel_z != 0,
			"client: actual P2 pose producer transports both steering components to its actor");
		check(s->guest_actor->distance_walked == server_walked,
			"client: owned mounted pose suppresses authoritative footstep odometer");
	} else if(kind == 1) {
		check(body->data.local_player.boat_rider_vel_x == 0
			&& body->data.local_player.boat_rider_vel_z == 0
			&& SAME_FIELD(c, &vehicle_before, pos)
			&& SAME_FIELD(c, &vehicle_before, vel)
			&& SAME_FIELD(c, &vehicle_before, data.monster.motion_d.motion),
			"client: P2 forward/strafe cannot steer the pig or author boat input");
		/* The seed's real pig tick raised the pose barrier. Clear it by an
		 * actual paused server update, then poison the authority's steering
		 * copy so a dropped zero-input pose cannot look like successful delivery. */
		step(s);
		s->guest_actor->rider_vel_x = .375F; s->guest_actor->rider_vel_z = -.5F;
		gstate.world_loaded = true; gstate.world_switch_pending = false; gstate.paused = false;
		clin_update(); main_client_guest_tick_dispatch(&frame); drain_server(s);
		check(s->guest_actor->rider_vel_x == 0 && s->guest_actor->rider_vel_z == 0
			&& s->guest_actor->distance_walked == server_walked,
			"client: actual pig-mounted P2 pose clears steering and suppresses authoritative footsteps");
	}
	check(same_body(gstate.players[0].local_player, &host_before)
		&& same_owner(&gstate.players[0], &owner_before),
		"client: guest input/follow/pose/camera leave P0 semantic state byte-identical");
	teardown(s);
}

static void case_steering_clear(unsigned kind) {
	assert(kind == 1 || kind == 2);
	struct server_local* s = admitted_fixture();
	struct client_session* guest = &gstate.players[1];
	struct entity* body = guest->local_player;
	struct entity* boat = seeded_vehicle(s, 0);
	struct input_player_frame frame = {.connected = true, .category = INPUT_CAT_GAMECUBE};
	frame.down[IB_FORWARD] = frame.down[IB_RIGHT] = true;
	body->data.local_player.capture_input = true;
	assert(!entity_local_player_walk_owned(body, guest, &frame));
	assert(body->data.local_player.boat_rider_vel_x != 0 && body->data.local_player.boat_rider_vel_z != 0);
	/* Closed guest ingress: inject the replicated cross-kind relation after
	 * the real server boat-exit writer. Input is produced by the owned tick. */
	server_local_set_boat_occupancy_for(s, boat, s->guest_actor, false);
	clin_update_drain();
	struct entity* next = seeded_vehicle(s, kind);
	if(kind == 2) next->data.minecart.rider_actor = guest_ident(s);
	struct entity host_before = *gstate.players[0].local_player;
	struct client_session owner_before = gstate.players[0];
	check(!entity_local_player_walk_owned(body, guest, &frame), "W1: real owned pig/cart follow survives boat exit");
	check(body->data.local_player.boat_rider_vel_x == 0 && body->data.local_player.boat_rider_vel_z == 0,
		"W1: pig/cart follow clears the previous boat's rider steering");
	struct camera_player_view view = {.world = body->world, .ride = &guest->ride,
		.sleeping = guest->sleeping, .yaw = body->orient[0], .pitch = body->orient[1]};
	camera_attach_owned(&guest->camera, body, 1.0F, 0.0F, &view);
	/* Valid-seat fixture behind closed guest ingress, then poison the server
	 * copy: a dropped pose would retain poison and cannot pass as delivery. */
	step(s);
	s->guest_actor->x = body->pos[0]; s->guest_actor->y = body->pos[1];
	s->guest_actor->z = body->pos[2];
	s->guest_actor->rider_vel_x = .625F; s->guest_actor->rider_vel_z = -.5F;
	assert(!s->guest_actor->reject_pos_this_update
		&& guest->player_sync.correction_seq == s->guest_actor->correction_seq);
	gstate.world_loaded = true; gstate.world_switch_pending = false; gstate.paused = false;
	clin_update(); main_client_guest_tick_dispatch(&frame); drain_server(s);
	check(s->guest_actor->rider_vel_x == 0 && s->guest_actor->rider_vel_z == 0,
		"W1: actual P2 pose producer cannot restore stale boat input to the server actor");
	check(same_body(gstate.players[0].local_player, &host_before)
		&& same_owner(&gstate.players[0], &owner_before), "W1: guest cross-kind tick preserves host semantic bytes");
	teardown(s);
}

static void case_unmounted_equiv(void) {
	struct server_local* s = admitted_fixture();
	struct entity* host = gstate.players[0].local_player;
	struct entity start = *host;
	struct client_session owner = gstate.players[0];
	/* Legacy host entry and existing owned entry receive identical no-input
	 * state in the same terrain. Only declared semantic output is compared. */
	host->data.local_player.capture_input = false;
	check(!host->tick_client(host), "equiv: legacy host walking tick survives");
	struct entity legacy = *host;
	struct client_session legacy_owner = gstate.players[0];
	*host = start; gstate.players[0] = owner;
	struct input_player_frame none = {.connected = true};
	check(!entity_local_player_walk_owned(host, &gstate.players[0], &none),
		"equiv: unmounted owned walking tick survives");
	check(same_body(host, &legacy) && same_owner(&gstate.players[0], &legacy_owner),
		"equiv: unmounted host output bytes match legacy wrapper, no padding compared");
	teardown(s);
}

static void case_host_equiv(unsigned kind) {
	struct server_local* s = admitted_fixture();
	struct entity* host = gstate.players[0].local_player;
	vec3 pos = {2.5F, 4, .5F};
	struct entity* server = kind == 0 ? server_local_spawn_boat(pos, s)
		: kind == 1 ? server_local_spawn_monster_at(pos, MONSTER_PIG, s)
		: server_local_spawn_minecart_type(pos, s, 0);
	assert(server); clin_update_drain();
	struct entity* c = mirrored(server);
	/* Replicated-host relation fixture. No input, no guest mount, no cart
	 * transaction; this selects the existing legacy host follow branch. */
	if(kind == 0) {
		c->data.boat.occupied = true; c->data.boat.occupant_id = host->id;
		c->orient[0] = .25F;
	} else if(kind == 1) {
		c->data.monster.saddled = c->data.monster.pig_ridden = true;
		c->data.monster.pig_rider_id = host->id;
	} else {
		c->data.minecart.occupied = true; c->data.minecart.occupant_id = host->id;
		c->data.minecart.control_ready = false;
	}
	host->data.local_player.capture_input = false;
	struct entity start = *host;
	struct client_session owner = gstate.players[0];
	check(!host->tick_client(host), "host equiv: legacy mounted entry survives");
	struct entity legacy = *host;
	struct client_session legacy_owner = gstate.players[0];
	/* Log actual legacy semantic bytes for the lead's matched old/new run.
	 * These are scalar float representations, not struct/pointer padding. */
	host_semantics(kind, &legacy, &legacy_owner);
	*host = start; gstate.players[0] = owner;
	struct input_player_frame none = {.connected = true};
	check(!entity_local_player_walk_owned(host, &gstate.players[0], &none),
		"host equiv: extracted owned entry survives");
	check(same_body(host, &legacy) && same_owner(&gstate.players[0], &legacy_owner),
		"host equiv: owned follow matches actual host wrapper output bytes for this kind");
	teardown(s);
}

static unsigned shared_ticks;
static bool (*real_shared_tick)(struct entity*, struct server_local*);
static bool counted_tick(struct entity* e, struct server_local* s) {
	shared_ticks++;
	return real_shared_tick(e, s); /* delegate to the real physics exactly once */
}
static void case_once(unsigned kind) {
	struct server_local* s = admitted_fixture();
	struct entity* boat = seeded_vehicle(s, kind);
	real_shared_tick = boat->tick_server; boat->tick_server = counted_tick;
	/* Callback decoration is counting instrumentation only; both client
	 * views exist from real admission and the full update drives physics. */
	s->paused = false;
	for(unsigned i = 0; i < 3; i++) {
		shared_ticks = 0; tick(s);
		check(shared_ticks == 1, "shared: two views produce exactly one physical hull tick per server update");
	}
	boat->tick_server = real_shared_tick;
	teardown(s);
}

static void payload_init(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
}
static struct entity* at_slot(unsigned slot, uint32_t id) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	struct entity** e = dict_entity_get(p.entities, id);
	return e ? *e : NULL;
}

static void attach_camera(struct client_session* owner) {
	struct entity* body = owner->local_player;
	struct camera_player_view view = {.world = body->world, .ride = &owner->ride,
		.sleeping = owner->sleeping, .yaw = body->orient[0], .pitch = body->orient[1]};
	camera_attach_owned(&owner->camera, body, 1.0F, 0.0F, &view);
}

static void case_boat_input_owner(void) {
	struct server_local* s = admitted_fixture();
	struct client_session* h = &gstate.players[0];
	struct client_session* g = &gstate.players[1];
	struct entity* guest_boat = seeded_vehicle(s, 0);
	struct entity* host_boat = server_local_spawn_boat((vec3){6.5F, 4, .5F}, s);
	assert(host_boat); clin_update_drain();
	server_local_set_boat_occupancy_for(s, host_boat, &s->player, true);
	clin_update_drain();
	h->local_player->orient[0] = g->local_player->orient[0] = 0;
	h->local_player->data.local_player.capture_input = true;
	g->local_player->data.local_player.capture_input = true;
	struct input_player_frame host_frame = {.connected = true}, guest_frame = {.connected = true};
	host_frame.down[IB_BACKWARD] = guest_frame.down[IB_FORWARD] = true;
	assert(!entity_local_player_walk_owned(h->local_player, h, &host_frame));
	attach_camera(h);
	struct entity host_before = *h->local_player;
	struct client_session owner_before = *h;
	struct entity host_hull_before = *mirrored(host_boat);
	assert(!entity_local_player_walk_owned(g->local_player, g, &guest_frame));
	attach_camera(g);
	/* Independent, single-axis expectation: at yaw zero forward produces
	 * +Z and backward -Z at Beta's airborne/water moveFlying speed 0.02F
	 * (EntityLiving.java:458,469,489); no helper computes the expectation. */
	check(g->local_player->data.local_player.boat_rider_vel_x == 0
		&& g->local_player->data.local_player.boat_rider_vel_z == .02F
		&& h->local_player->data.local_player.boat_rider_vel_z == -.02F,
		"input owner: opposing P1/P2 frames produce independent boat steering");
	check(same_body(h->local_player, &host_before) && same_owner(h, &owner_before)
		&& SAME_FIELD(mirrored(host_boat), &host_hull_before, data.boat.rider_vel_x)
		&& SAME_FIELD(mirrored(host_boat), &host_hull_before, data.boat.rider_vel_z),
		"input owner: P2 tick and camera cannot write P1 body, camera or hull input");
	gstate.world_loaded = true; gstate.world_switch_pending = false; gstate.paused = false;
	clin_update();
	bool (*saved_tick)(struct entity*) = h->local_player->tick_client;
	host_tick_frame = &host_frame;
	h->local_player->tick_client = host_input_tick;
	const unsigned slots[] = {0};
	main_client_host_tick_dispatch(slots, 1, true, false, -1);
	h->local_player->tick_client = saved_tick;
	main_client_guest_tick_dispatch(&guest_frame);
	drain_server(s);
	check(s->guest_actor->rider_vel_x == 0 && s->guest_actor->rider_vel_z == .02F
		&& s->player.rider_vel_x == 0 && s->player.rider_vel_z == -.02F,
		"input owner: both real tick pose producers retain source-slot ownership");
	assert(!guest_boat->tick_server(guest_boat, s));
	assert(!host_boat->tick_server(host_boat, s));
	clin_update_drain();
	check(guest_boat->data.boat.rider_vel_z == .02F
		&& host_boat->data.boat.rider_vel_z == -.02F,
		"input owner: actual hull ticks consume only their recorded actor's steering");
	/* Held P1 input remains present while the P2 frame is disconnected or
	 * capture is disabled. The follow branch must author fresh zeroes. */
	guest_frame.connected = false;
	assert(!entity_local_player_walk_owned(g->local_player, g, &guest_frame));
	check(g->local_player->data.local_player.boat_rider_vel_z == 0
		&& mirrored(guest_boat)->data.boat.rider_vel_z == 0,
		"input owner: disconnected P2 cannot inherit held P1 steering");
	guest_frame.connected = true; g->local_player->data.local_player.capture_input = false;
	assert(!entity_local_player_walk_owned(g->local_player, g, &guest_frame));
	check(g->local_player->data.local_player.boat_rider_vel_x == 0
		&& g->local_player->data.local_player.boat_rider_vel_z == 0,
		"input owner: disabled P2 capture clears steering while remaining seated");
	teardown(s);
}

static void case_pig_seat_correction(void) {
	struct server_local* s = admitted_fixture();
	struct client_session* g = &gstate.players[1];
	struct entity* pig = seeded_vehicle(s, 1);
	struct entity* body = g->local_player;
	struct entity host_before = *gstate.players[0].local_player;
	struct client_session owner_before = gstate.players[0];
	uint32_t before_seq = g->player_sync.correction_seq;
	body->vel[0] = .25F; body->vel[1] = -.5F; body->vel[2] = .75F;
	body->orient[0] = .375F; body->orient[1] = .625F;
	g->camera.rx = .375F; g->camera.ry = .625F;
	float history_dx = body->pos_old[0] - body->pos[0];
	pig->teleport(pig, (vec3){4.5F, 4.25F, 1.5F});
	counters_reset();
	assert(!pig->tick_server(pig, s)); clin_update_drain();
	struct server_player* actor = s->guest_actor;
	check(guest_corrections == 1 && !host_pos_msgs
		&& guest_corr_seq == actor->correction_seq && guest_corr_seq != before_seq
		&& g->player_sync.correction_seq == actor->correction_seq,
		"pig correction: actual tick delivers one new token to the admitted P2 consumer");
	check(near3(body->pos, actor->x, actor->y, actor->z)
		&& fabs(g->camera.x - actor->x) < 1e-4 && fabs(g->camera.y - actor->y) < 1e-4
		&& fabs(g->camera.z - actor->z) < 1e-4
		&& fabsf(body->pos_old[0] - body->pos[0] - history_dx) < 1e-4F,
		"pig correction: tagged position moves P2 body, interpolation and camera together");
	check(body->vel[0] == 0 && body->vel[1] == 0 && body->vel[2] == 0
		&& body->orient[0] == .375F && body->orient[1] == .625F
		&& g->camera.rx == .375F && g->camera.ry == .625F,
		"pig correction: zero-motion flags apply while preserving P2 aim");
	struct entity body_before = *body;
	struct client_session guest_before = *g;
	uint64_t stale_generation = s->guest_generation ^ (UINT64_C(1) << 32);
	check(!server_local_guest_correct_position_for(s, stale_generation, PLAYER_CORRECTION_POSITION_ONLY),
		"pig correction: producer refuses an old full guest lifetime");
	struct client_rpc stale = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .ticket = s->guest_ticket,
			.generation = stale_generation, .result = LOCAL_GUEST_CORRECTION,
			.x = 99, .y = 99, .z = 99, .correction_seq = guest_corr_seq + 1,
			.correction_flags = PLAYER_CORRECTION_POSITION_ONLY}};
	/* Delayed-envelope fixture: real transport and consumer, no replacement
	 * session or admission success fabricated. Equal low 32 bits is deliberate. */
	assert(clin_try_rpc_send_local(&stale, 1, stale_generation)); clin_update_drain();
	check(same_body(body, &body_before) && same_owner(g, &guest_before),
		"pig correction: stale tagged envelope cannot move the current same-ID P2 body");
	check(same_body(gstate.players[0].local_player, &host_before)
		&& same_owner(&gstate.players[0], &owner_before),
		"pig correction: production tick and stale delivery preserve P1 semantic bytes");
	teardown(s);
}

static void case_world_follow(unsigned kind) {
	assert(kind < 2);
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct entity* primary = seeded_vehicle(s, kind);
	struct server_world* other = server_local_world_admit(s, WORLD_DIM_NETHER);
	assert(other && clin_world_payload_prepare_landing(other->world_index, other->generation, other->dimension));
	assert(clin_world_payload_bind_view(other->world_index, 1));
	struct clin_world_payload payload; assert(clin_world_payload_of_view(1, &payload));
	struct client_session* g = &gstate.players[1];
	g->local_player->world = payload.terrain;
	/* Publication fixture ONLY: duplicate the same numeric entity/rider IDs
	 * and exact coordinates in another world. The server's global dictionary
	 * cannot hold two entities under one key, so the secondary is a standalone
	 * publisher, not an admitted/ticked vehicle or a portal-travel claim. */
	struct entity secondary = *primary;
	secondary.world = other; secondary.world_slot = other->world_index;
	secondary.world_generation = other->generation;
	secondary.client_pub_generation = 0;
	assert(server_local_try_publish_entity_w(s, other, &secondary)); clin_update_drain();
	if(kind == 1) {
		/* Spawn has no pig relation fields. Use the real world-stamped MOVE
		 * publisher/consumer with an explicitly authored replication fixture. */
		server_local_send_entity(s, &secondary, (struct client_rpc){.type = CRPC_ENTITY_MOVE,
			.payload.entity_move = {.entity_id = secondary.id,
				.pos = {secondary.pos[0], secondary.pos[1], secondary.pos[2]},
				.saddled = true, .pig_ridden = true, .pig_rider_id = s->guest_entity_id}});
		clin_update_drain();
	}
	struct entity* own = at_slot(other->world_index, secondary.id);
	struct entity* foreign = at_slot(0, secondary.id);
	assert(own && foreign && own != foreign);
	check(SAME_FIELD(own, foreign, pos),
		"world follow: two payloads contain distinct same-ID vehicles at identical coordinates");
	struct entity foreign_before = *foreign;
	struct entity host_before = *gstate.players[0].local_player;
	struct client_session owner_before = gstate.players[0];
	secondary.pos[0] += 3; secondary.pos[1] += 1; secondary.pos[2] += 2;
	if(kind == 0) assert(boat_send_correction(&secondary));
	else server_local_send_entity(s, &secondary, (struct client_rpc){.type = CRPC_ENTITY_MOVE,
		.payload.entity_move = {.entity_id = secondary.id,
			.pos = {secondary.pos[0], secondary.pos[1], secondary.pos[2]},
			.saddled = true, .pig_ridden = true, .pig_rider_id = s->guest_entity_id}});
	clin_update_drain();
	/* Monster MOVE installs the interpolation destination. Its actual client
	 * tick reaches that destination; boat correction snaps immediately. */
	if(kind == 1) for(unsigned i = 0; i < 3; i++) assert(!own->tick_client(own));
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_FORWARD] = frame.down[IB_RIGHT] = true;
	g->local_player->data.local_player.capture_input = true;
	/* Deliberately retain the primary world selection. The owned follow entry
	 * must still select view 1's payload rather than finding the foreign ID. */
	assert(world_view_entities() != payload.entities);
	assert(!entity_local_player_walk_owned(g->local_player, g, &frame));
	attach_camera(g);
	float x = kind == 0 ? (float)((double)own->pos[0]
		+ cos((double)own->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET) : own->pos[0];
	float y = kind == 0 ? own->pos[1] + BOAT_MOUNTED_Y_OFFSET + g->local_player->y_offset + PLAYER_EYE_HEIGHT
		: own->pos[1] + PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT;
	float z = kind == 0 ? (float)((double)own->pos[2]
		+ sin((double)own->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET) : own->pos[2];
	check(near3(g->local_player->pos, x, y, z) && near3(g->local_player->pos_old, x, y, z)
		&& fabs(g->camera.x - x) < 1e-4 && fabs(g->camera.y - y) < 1e-4 && fabs(g->camera.z - z) < 1e-4,
		"world follow: P2 body/history/camera choose only its viewed world's same-ID seat");
	check(SAME_FIELD(foreign, &foreign_before, pos) && SAME_FIELD(foreign, &foreign_before, vel)
		&& (kind != 0 || (SAME_FIELD(foreign, &foreign_before, data.boat.rider_vel_x)
			&& SAME_FIELD(foreign, &foreign_before, data.boat.rider_vel_z)))
		&& same_body(gstate.players[0].local_player, &host_before)
		&& same_owner(&gstate.players[0], &owner_before),
		"world follow: secondary correction/input/follow preserve primary hull and P1 semantic bytes");
	/* Restore the body's real admission world before lifetime teardown. */
	g->local_player->world = &gstate.world;
	assert(clin_world_payload_bind_view(0, 1));
	server_local_worlds_discard_secondary(s); teardown(s);
}

static void case_publication(const char* which) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_world* w = &s->worlds[0];
	struct entity* boat = server_local_spawn_boat((vec3){2.5F, 4, .5F}, s);
	struct entity* cart = server_local_spawn_minecart_type((vec3){3.5F, 4, .5F}, s, 0);
	assert(boat && cart); clin_update_drain();
	assert(at_slot(0, boat->id) && at_slot(0, cart->id));
	if(!strcmp(which, "same-world")) {
		check(clin_world_view_slot(0) == clin_world_view_slot(1),
			"publication: two same-world views share one live payload");
		boat->pos[0] += .25F;
		check(boat_send_correction(boat), "publication: same-world boat correction accepted");
		clin_update_drain();
		check(at_slot(0, boat->id)->pos[0] == boat->pos[0],
			"publication: same-world correction reaches the shared payload");
		teardown(s); return;
	}
	struct server_world* other = server_local_world_admit(s, WORLD_DIM_NETHER);
	assert(other);
	/* Existing landing seam installs the SECOND context without moving P0.
	 * This is a publication fixture, not a fabricated transfer success. */
	assert(clin_world_payload_prepare_landing(other->world_index, other->generation, other->dimension));
	if(strcmp(which, "late-bind")) assert(clin_world_payload_bind_view(other->world_index, 1));
	check(at_slot(other->world_index, boat->id) == NULL,
		"publication: matching entity IDs never leak into the other live world");
	/* Ordinary spawn and snapshot must choose their real world even with
	 * two live contexts. They are deliberately not manually stamped here. */
	struct entity* fresh_boat = server_local_spawn_boat((vec3){5.5F, 4, .5F}, s);
	struct entity* fresh_cart = server_local_spawn_minecart_type((vec3){6.5F, 4, .5F}, s, 0);
	assert(fresh_boat && fresh_cart); clin_update_drain();
	check(at_slot(0, fresh_boat->id) && at_slot(0, fresh_cart->id),
		"publication: ordinary vehicle spawns reach their world with two contexts live");
	float before = at_slot(0, boat->id)->pos[0];
	boat->pos[0] = before + .25F;
	if(!strcmp(which, "stale-stamp")) {
		/* Reuse the client ID, but make the producer's entity world stamp stale.
		 * A real publisher must resolve it before touching the live client ID. */
		uint32_t generation = boat->world_generation;
		boat->world_generation ^= UINT32_C(0x10000);
		(void)boat_send_correction(boat); clin_update_drain();
		check(at_slot(0, boat->id)->pos[0] == before,
			"publication: stale producer generation cannot correct a reused live entity ID");
		boat->world_generation = generation;
	} else {
		if(!strcmp(which, "pressure")) {
			struct client_rpc harmless = {.type = CRPC_TIME_SET};
			unsigned n = 0; while(clin_try_rpc_send(&harmless)) n++;
			assert(n > 0 && !clin_rpc_headroom(1));
			boat->data.boat.correction_ticks = UINT8_MAX;
			check(!boat_send_correction(boat), "publication: real token pressure refuses correction");
			check(boat->data.boat.correction_ticks == UINT8_MAX,
				"publication: refusal leaves correction retryable");
			cart->data.minecart.snapshot_dirty = true;
			cart->data.minecart.snapshot_urgent = true;
			server_local_test_publish_minecart_snapshots(s);
			check(cart->data.minecart.snapshot_dirty,
				"publication: refused real cart snapshot remains dirty for retry");
			clin_update_drain();
		}
		check(boat_send_correction(boat), "publication: real world correction send/retry accepted");
		cart->data.minecart.snapshot_dirty = true;
		cart->data.minecart.snapshot_urgent = true;
		cart->pos[0] += .25F;
		server_local_test_publish_minecart_snapshots(s); clin_update_drain();
		check(at_slot(0, boat->id)->pos[0] == boat->pos[0],
			"publication: boat correction reaches only the producing world");
		check(at_slot(0, cart->id)->pos[0] == cart->pos[0],
			"publication: cart snapshot reaches only the producing world");
	}
	if(!strcmp(which, "late-bind")) {
		/* Construct a secondary-world hull via EXISTING entity/world seams;
		 * no ordinary secondary boat spawn API exists in the preimage. */
		uint32_t id = server_local_gen_id(s);
		struct entity* secondary = calloc(1, sizeof(*secondary)); assert(secondary);
		entity_boat(id, secondary, true, other);
		secondary->teleport(secondary, (vec3){.5F, 70, .5F});
		*dict_entity_safe_get(s->entities, id) = secondary;
		/* Publication-only fixture: constructor binds the actual world stamp;
		 * the snapshot API requires that stamp, not physics section membership.
		 * The paused two-view rig can still be draining its column budget, so
		 * do not demand a new terrain column merely to exercise publication.
		 * This entity is never ticked or claimed as a chunk-lifecycle witness. */
		assert(server_local_entity_world(s, secondary) == other);
		bool first = server_local_try_publish_entity_w(s, other, secondary);
		clin_update_drain();
		check(!at_slot(0, id), "publication: unbound secondary snapshot never appears in primary");
		assert(clin_world_payload_bind_view(other->world_index, 1));
		check(first || server_local_try_publish_entity_w(s, other, secondary),
			"publication: secondary bind permits eventual production snapshot retry");
		/* Always request once after bind: a pre-bind accepted snapshot may
		 * intentionally be held or dropped by the consumer's routing gate. */
		check(server_local_try_publish_entity_w(s, other, secondary),
			"publication: bound secondary snapshot is accepted");
		clin_update_drain();
		check(at_slot(other->world_index, id) && !at_slot(0, id),
			"publication: late bind exposes the secondary hull only in its own payload");
	}
	check(!at_slot(other->world_index, boat->id)
		&& !at_slot(other->world_index, cart->id),
		"publication: primary vehicles remain absent from the secondary world");
	(void)w;
	server_local_worlds_discard_secondary(s);
	teardown(s);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "follow";
	bool publication = !strcmp(which, "same-world") || !strcmp(which, "separate-world")
		|| !strcmp(which, "late-bind") || !strcmp(which, "pressure") || !strcmp(which, "stale-stamp");
	if(publication || !strcmp(which, "world-follow")) payload_init(); else harness_init();
	if(!strcmp(which, "follow")) case_follow(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	else if(!strcmp(which, "steering-clear")) case_steering_clear(argc > 2 ? (unsigned)atoi(argv[2]) : 1);
	else if(!strcmp(which, "host-equiv")) case_host_equiv(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	else if(!strcmp(which, "unmounted-equiv")) case_unmounted_equiv();
	else if(!strcmp(which, "once")) case_once(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	else if(!strcmp(which, "input-owner")) case_boat_input_owner();
	else if(!strcmp(which, "pig-seat-correction")) case_pig_seat_correction();
	else if(!strcmp(which, "world-follow")) case_world_follow(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	else if(publication) case_publication(which);
	else { fprintf(stderr, "unknown vehicle client case: %s\n", which); return 2; }
	return harness_result("test_vehicle_client_seams", which);
}
