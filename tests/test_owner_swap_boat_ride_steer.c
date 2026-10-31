/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d, P2 inventory row #7: boat mount/dismount
 * and rider steering, owner-swapped.
 *
 * Beta citations (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - EntityBoat.interact, EntityBoat.java:336-345: when riddenByEntity is
 *     a non-null EntityPlayer other than the interacting player, it
 *     returns true -- occupied by a DIFFERENT player: no-op; else
 *     the interacting player mounts the boat (mountEntity).
 *   - Entity.mountEntity, Entity.java:941-966: mounting the entity you already
 *     ride DISMOUNTS (`ridingEntity = null`, :950-953); otherwise the previous
 *     mount is released (:961) and ridingEntity becomes the new mount (:964) -- the
 *     rider is whoever called it, never a fixed player.
 *   - EntityBoat.onUpdate, EntityBoat.java:195-197: `motionX +=
 *     riddenByEntity.motionX * 0.2D` -- steering reads the RIDER's motion.
 *   - Entity.moveEntity, Entity.java:436-457: the footstep odometer runs only
 *     while ridingEntity == null -- a rider makes no footsteps.
 *
 * R2 (ride-r2-1008): production ENTITY_USE admits P2 with its own rider
 * identity. Both cases pass directly; the historical `red` argv/name is
 * retained for the guest regression. test_vehicle_client_seams additionally
 * exercises the occupancy publisher, owned tick, camera and pose producer.
 *
 * Two ctest names, one executable (argv[1]):
 *   base  -- the host reference: mount records the host identity, the pose
 *            stream feeds the steering source, the odometer is gated, a
 *            desired=false use dismounts and zeroes the steering.
 *   red   -- the same sequence at the guest, plus the "occupied by a different
 *            rider" refusal in both directions. Ordinary passing regression. */
#include "support/owner_swap_harness.h"
#include "entity/entity_boat.h"
#include "game/camera.h"

#define TEST_NAME "test_owner_swap_boat_ride_steer"

static struct server_rpc use_boat(uint32_t id, bool desired) {
	return (struct server_rpc) {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = id, .desired_occupied = desired}};
}
/* A boat beside a body: one block +x, hull at the feet (y 4), inside the 6.0
 * reach and the LOS gate of server_local_entity_interaction_allowed. */
static struct entity* boat_beside(struct server_local* s, struct server_player* p) {
	struct entity* boat = server_local_spawn_boat(
		(vec3) {(float)p->x + 1.0F, 4.0F, (float)p->z}, s);
	assert(boat && boat->type == ENTITY_BOAT);
	clin_update_drain();
	return boat;
}
static bool ridden_by(const struct entity* boat, struct actor_ident who) {
	return boat->data.boat.occupied
		&& actor_ident_equal(boat->data.boat.rider_actor, who);
}
static bool unridden(const struct entity* boat) {
	return !boat->data.boat.occupied && boat->data.boat.occupant_id == 0
		&& boat->data.boat.rider_vel_x == 0.0F && boat->data.boat.rider_vel_z == 0.0F;
}
static void presentation(struct entity* boat, unsigned slot) {
	struct client_session* owner = &gstate.players[slot];
	struct entity* body = owner->local_player;
	struct entity** pp = dict_entity_get(world_view_entities(), boat->id);
	assert(pp && *pp && body);
	struct entity* mirror = *pp;
	check(mirror->data.boat.occupied && mirror->data.boat.occupant_id == body->id,
		"admission/presentation: real boat correction records the owning client body");
	float walked = body->data.local_player.walk_dist;
	body->vel[0] = .125F; body->vel[1] = -.375F; body->vel[2] = -.25F;
	body->data.local_player.walk_next_step = 0;
	body->data.local_player.capture_input = true;
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_FORWARD] = frame.down[IB_RIGHT] = true;
	counters_reset();
	check(!entity_local_player_walk_owned(body, owner, &frame),
		"admission/presentation: real owned client tick survives");
	float x = (float)((double)mirror->pos[0]
		+ cos((double)mirror->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET);
	float y = mirror->pos[1] + BOAT_MOUNTED_Y_OFFSET + body->y_offset + PLAYER_EYE_HEIGHT;
	float z = (float)((double)mirror->pos[2]
		+ sin((double)mirror->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET);
	check(near3(body->pos, x, y, z) && near3(body->pos_old, x, y, z),
		"admission/presentation: body and history occupy the boat seat");
	check(body->data.local_player.walk_dist == walked && !sound_msgs
		&& body->vel[0] == .125F && body->vel[1] == -.375F && body->vel[2] == -.25F,
		"admission/presentation: mounted tick suppresses walking, gravity and footsteps");
	struct camera_player_view view = {.world = body->world, .ride = &owner->ride,
		.yaw = body->orient[0], .pitch = body->orient[1]};
	camera_attach_owned(&owner->camera, body, 1.0F, 0.0F, &view);
	check(fabs(owner->camera.x - x) < 1e-4 && fabs(owner->camera.y - y) < 1e-4
		&& fabs(owner->camera.z - z) < 1e-4,
		"admission/presentation: the owning camera occupies the boat seat");
}
/* One pose sample carrying the client's own input velocity (the steering
 * source the boat reads while the body rides). */
static struct server_rpc steer_sample(const struct server_player* p, float vx,
	float vz) {
	struct server_rpc call = pose_sample(p->x, p->y, p->z, true, p->correction_seq);
	call.payload.player_pos.vel_x = vx;
	call.payload.player_pos.vel_z = vz;
	return call;
}

static struct server_local* dock(void) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, 0.5);
	return s;
}

/* ======================================================================== */
static void case_base(void) {
	struct server_local* s = dock();
	struct server_player* h = &s->player;
	struct entity* boat = boat_beside(s, h);

	struct server_rpc mount = use_boat(boat->id, true);
	host_send(s, &mount);
	check(ridden_by(boat, ACTOR_IDENT_HOST) && h->riding_boat
		&& boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID,
		"host: ENTITY_USE desired=true mounts the boat and records the HOST as "
		"its rider (EntityBoat.interact:341 -> Entity.mountEntity:964)");
	presentation(boat, 0);

	struct server_rpc steer = steer_sample(h, 0.3F, -0.1F);
	host_send(s, &steer);
	check(h->rider_vel_x == 0.3F && h->rider_vel_z == -0.1F,
		"host: the pose stream feeds the rider's steering source "
		"(EntityBoat.onUpdate:195-197 reads riddenByEntity.motion)");

	float walked = h->distance_walked;
	walk_step(s, h, 0.5, false);
	check(h->distance_walked == walked,
		"host: no footstep odometer while riding (Entity.moveEntity:436, "
		"ridingEntity == null gate)");

	struct server_rpc dismount = use_boat(boat->id, false);
	host_send(s, &dismount);
	check(unridden(boat) && !h->riding_boat && h->rider_vel_x == 0.0F
		&& h->rider_vel_z == 0.0F,
		"host: desired=false dismounts and zeroes both steering copies "
		"(Entity.mountEntity:950-953)");
	teardown(s);
}

/* ======================================================================== */
static void case_red(void) {
	struct server_local* s = dock();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct entity* host_boat = boat_beside(s, h);
	struct entity* guest_boat = boat_beside(s, g);
	struct actor_ident me = guest_ident(s);

	/* Host reference first, so the guest's attempts can be checked against
	 * an occupied host boat as well. */
	struct server_rpc hmount = use_boat(host_boat->id, true);
	host_send(s, &hmount);
	check(ridden_by(host_boat, ACTOR_IDENT_HOST) && h->riding_boat,
		"setup: host riding its own boat");

	/* --- the guest mounts ITS boat -------------------------------------- */
	struct server_rpc gmount = use_boat(guest_boat->id, true);
	check(guest_send(s, &gmount), "queue: the guest's ENTITY_USE is admitted");
	check(ridden_by(guest_boat, me) && g->riding_boat,
		"guest: ENTITY_USE desired=true mounts ITS boat and records the GUEST "
		"identity as rider (EntityBoat.interact:341, mountEntity for var1 -- "
		"the interacting player)  [row #7: guest admission regression]");
	presentation(guest_boat, 1);
	check(ridden_by(host_boat, ACTOR_IDENT_HOST) && h->riding_boat,
		"host: its own ride is untouched by the guest's mount");

	/* --- steering source from the GUEST's pose stream -------------------- */
	struct server_rpc gsteer = steer_sample(g, 0.3F, -0.1F);
	assert(svin_rpc_send_from(guest_source(s), &gsteer));
	drain_server(s);
	check(g->rider_vel_x == 0.3F && g->rider_vel_z == -0.1F,
		"guest: its pose stream feeds ITS steering source "
		"(EntityBoat.onUpdate:195-197)  [already GREEN independently of admission]");
	check(h->rider_vel_x == 0.0F && h->rider_vel_z == 0.0F,
		"host: the guest's steering never lands on the host body");

	/* --- odometer gate while riding (only meaningful once mounted) -------- */
	if(g->riding_boat) {
		float walked = g->distance_walked;
		walk_step(s, g, 0.5, false);
		check(g->distance_walked == walked,
			"guest: no footstep odometer while riding (Entity.moveEntity:436)");
	}

	/* --- a boat occupied by the OTHER player refuses (interact:337-339) --- */
	struct server_rpc steal = use_boat(host_boat->id, true);
	check(guest_send(s, &steal), "queue: the guest's use of the host's boat is admitted");
	check(ridden_by(host_boat, ACTOR_IDENT_HOST) && h->riding_boat && !ridden_by(host_boat, me),
		"guest: using the boat the HOST rides is a no-op "
		"(EntityBoat.interact:337-339, riddenByEntity != var1)");
	if(ridden_by(guest_boat, me)) {
		struct server_rpc hsteal = use_boat(guest_boat->id, true);
		host_send(s, &hsteal);
		check(ridden_by(guest_boat, me) && !ridden_by(guest_boat, ACTOR_IDENT_HOST),
			"host: using the boat the GUEST rides is a no-op the same way");
		check(ridden_by(host_boat, ACTOR_IDENT_HOST) && h->riding_boat,
			"host: still riding its own boat after the refused steal");
	}

	/* --- dismount --------------------------------------------------------- */
	struct server_rpc gdismount = use_boat(guest_boat->id, false);
	check(guest_send(s, &gdismount), "queue: the guest's dismount is admitted");
	check(unridden(guest_boat) && !g->riding_boat && g->rider_vel_x == 0.0F
		&& g->rider_vel_z == 0.0F,
		"guest: desired=false leaves ITS boat unridden with the steering zeroed");
	struct server_rpc hdismount = use_boat(host_boat->id, false);
	host_send(s, &hdismount);
	check(unridden(host_boat) && !h->riding_boat, "host: dismounted for teardown");
	teardown(s);
}

/* ======================================================================== */
int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : NULL;
	if(!which || (strcmp(which, "base") && strcmp(which, "red"))) {
		fprintf(stderr, "usage: %s base|red  (one case per process)\n", argv[0]);
		return 2;
	}
	harness_init();
	if(!strcmp(which, "base"))
		case_base();
	else
		case_red();
	return harness_result(TEST_NAME, which);
}
