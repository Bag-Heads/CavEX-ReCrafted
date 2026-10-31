/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d, P2 inventory row #8: saddled-pig riding,
 * owner-swapped.
 *
 * Beta citations (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - EntityPig.interact, EntityPig.java:36-45: it returns false when unsaddled,
 *     in a multiplayer world, or ridden by another entity
 *     (:37) -- unsaddled, or ridden by SOMEONE ELSE, refuses;
 *     else the interacting player mounts it (mountEntity, :40).
 *   - Entity.mountEntity, Entity.java:941-966: mounting what you already ride
 *     dismounts (:950-953); any previous mount is released first (:961);
 *     ridingEntity becomes the new mount (:964).
 *   - EntityPig.writeEntityToNBT, EntityPig.java:16: "Saddle" is the pig's
 *     own state, not the rider's.
 *
 * R2 (ride-r2-1008): production ENTITY_USE admits P2 with its own rider
 * identity. Both cases pass directly; the historical `red` argv/name is
 * retained for the guest regression. Client follow and tagged pig seat
 * correction are also exercised in test_vehicle_client_seams.
 *
 * Two ctest names, one executable (argv[1]):
 *   base  -- host mounts, re-use dismounts (mountEntity's toggle idiom),
 *            an unsaddled pig never mounts.
 *   red   -- the same at the guest with its own identity recorded, plus the
 *            "ridden by the other player" refusal both ways. Plain pass. */
#include "support/owner_swap_harness.h"
#include "entity/monsters_object.h"
#include "game/camera.h"

#define TEST_NAME "test_owner_swap_pig_mount"

static struct server_rpc use_pig(uint32_t id) {
	return (struct server_rpc) {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = id, .desired_occupied = true}};
}
/* A pig beside a body: one block +x, standing at the feet (y 4), inside the
 * 6.0 reach and LOS gate (test_local_guest's own placement). */
static struct entity* pig_beside(struct server_local* s, struct server_player* p,
	bool saddled) {
	struct entity* pig = server_local_spawn_monster_at(
		(vec3) {(float)p->x + 1.0F, 4.0F, (float)p->z}, MONSTER_PIG, s);
	assert(pig && pig->type == ENTITY_MONSTER && pig->data.monster.id == MONSTER_PIG);
	pig->data.monster.saddled = saddled;
	clin_update_drain();
	return pig;
}
static bool ridden_by(const struct entity* pig, struct actor_ident who) {
	return pig->data.monster.pig_ridden
		&& actor_ident_equal(pig->data.monster.pig_rider_actor, who);
}
static bool unridden(const struct entity* pig) {
	return !pig->data.monster.pig_ridden && pig->data.monster.pig_rider_id == 0;
}
static void presentation(struct server_local* s, struct entity* pig, unsigned slot) {
	/* Production pig tick publishes pig_ridden/pig_rider_id and, for an
	 * admitted rider, its seat correction. No client relation injection. */
	assert(!pig->tick_server(pig, s));
	clin_update_drain();
	struct client_session* owner = &gstate.players[slot];
	struct entity* body = owner->local_player;
	struct entity** pp = dict_entity_get(world_view_entities(), pig->id);
	assert(pp && *pp && body);
	struct entity* mirror = *pp;
	check(mirror->data.monster.pig_ridden && mirror->data.monster.pig_rider_id == body->id,
		"admission/presentation: real pig tick replicates the owning client rider");
	float walked = body->data.local_player.walk_dist;
	body->vel[0] = .125F; body->vel[1] = -.375F; body->vel[2] = -.25F;
	body->data.local_player.walk_next_step = 0;
	body->data.local_player.capture_input = true;
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_FORWARD] = frame.down[IB_RIGHT] = true;
	counters_reset();
	check(!entity_local_player_walk_owned(body, owner, &frame),
		"admission/presentation: real owned client tick survives");
	float y = mirror->pos[1] + PIG_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT;
	check(near3(body->pos, mirror->pos[0], y, mirror->pos[2])
		&& near3(body->pos_old, mirror->pos[0], y, mirror->pos[2]),
		"admission/presentation: body and history occupy the pig seat");
	check(body->data.local_player.walk_dist == walked && !sound_msgs
		&& body->vel[0] == 0 && body->vel[1] == 0 && body->vel[2] == 0,
		"admission/presentation: mounted tick suppresses walking, gravity and footsteps");
	struct camera_player_view view = {.world = body->world, .ride = &owner->ride,
		.yaw = body->orient[0], .pitch = body->orient[1]};
	camera_attach_owned(&owner->camera, body, 1.0F, 0.0F, &view);
	check(fabs(owner->camera.x - mirror->pos[0]) < 1e-4
		&& fabs(owner->camera.y - y) < 1e-4 && fabs(owner->camera.z - mirror->pos[2]) < 1e-4,
		"admission/presentation: the owning camera occupies the pig seat");
}

static struct server_local* sty(void) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, 0.5);
	return s;
}

/* ======================================================================== */
static void case_base(void) {
	struct server_local* s = sty();
	struct server_player* h = &s->player;
	struct entity* pig = pig_beside(s, h, true);

	struct server_rpc use = use_pig(pig->id);
	host_send(s, &use);
	check(ridden_by(pig, ACTOR_IDENT_HOST)
		&& pig->data.monster.pig_rider_id == SERVER_LOCAL_PLAYER_ENTITY_ID,
		"host: using a saddled pig mounts it with the HOST recorded as rider "
		"(EntityPig.interact:40 -> Entity.mountEntity:964)");
	presentation(s, pig, 0);
	host_send(s, &use);
	check(unridden(pig),
		"host: using the pig you already ride dismounts (Entity.mountEntity:950-953)");

	struct entity* bare = pig_beside(s, h, false);
	struct server_rpc use_bare = use_pig(bare->id);
	host_send(s, &use_bare);
	check(unridden(bare), "host: an unsaddled pig never mounts (EntityPig.interact:37)");
	teardown(s);
}

/* ======================================================================== */
static void case_red(void) {
	struct server_local* s = sty();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct entity* host_pig = pig_beside(s, h, true);
	struct entity* guest_pig = pig_beside(s, g, true);
	struct actor_ident me = guest_ident(s);

	struct server_rpc huse = use_pig(host_pig->id);
	host_send(s, &huse);
	check(ridden_by(host_pig, ACTOR_IDENT_HOST), "setup: host riding its own pig");

	struct server_rpc guse = use_pig(guest_pig->id);
	check(guest_send(s, &guse), "queue: the guest's ENTITY_USE is admitted");
	check(ridden_by(guest_pig, me),
		"guest: using ITS saddled pig mounts it with the GUEST identity as "
		"rider (EntityPig.interact:40, mountEntity for var1 -- the interacting "
		"player)  [row #8: guest admission regression]");
	presentation(s, guest_pig, 1);
	check(ridden_by(host_pig, ACTOR_IDENT_HOST),
		"host: its own ride is untouched by the guest's mount");

	/* Ridden by the OTHER player: refused both ways (interact:37). */
	struct server_rpc steal = use_pig(host_pig->id);
	check(guest_send(s, &steal), "queue: the guest's use of the host's pig is admitted");
	check(ridden_by(host_pig, ACTOR_IDENT_HOST) && !ridden_by(host_pig, me),
		"guest: the pig the HOST rides refuses the guest "
		"(EntityPig.interact:37, riddenByEntity != var1)");
	if(ridden_by(guest_pig, me)) {
		struct server_rpc hsteal = use_pig(guest_pig->id);
		host_send(s, &hsteal);
		check(ridden_by(guest_pig, me) && !ridden_by(guest_pig, ACTOR_IDENT_HOST),
			"host: the pig the GUEST rides refuses the host the same way");
		/* Toggle off. */
		check(guest_send(s, &guse), "queue: the guest's second use is admitted");
		check(unridden(guest_pig),
			"guest: using the pig it already rides dismounts (mountEntity:950-953)");
	}

	struct entity* bare = pig_beside(s, g, false);
	struct server_rpc use_bare = use_pig(bare->id);
	check(guest_send(s, &use_bare), "queue: the guest's use of a bare pig is admitted");
	check(unridden(bare), "guest: an unsaddled pig never mounts (EntityPig.interact:37)");

	host_send(s, &huse);
	check(unridden(host_pig), "host: dismounted for teardown");
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
