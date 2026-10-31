/* mp-velocity-0926 (astra-knockback-0926 item 3): remote multiplayer must
 * apply Packet28EntityVelocity to the LOCAL player.
 *
 * Beta: NetClientHandler.handleEntityVelocity (NetClientHandler.java:165-170)
 * looks the entity up by id -- the client's own EntityPlayerSP included -- and
 * calls Entity.setVelocity(motionX/8000, motionY/8000, motionZ/8000), a plain
 * assignment of all three components (Entity.java:1000-1004). The server
 * sends Packet28 to the tracked player itself after knockBack
 * (EntityTrackerEntry.updatePlayerList:124-127).
 *
 * Driven through production code: the real Packet28 codec bytes ->
 * process_event (server_remote_protocol.c) -> clin_process_direct -> the
 * client apply path. Non-aborting: every case prints ok/FAIL. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

#include <math.h>

enum { LOCAL_ID = 42, MOB_ID = 63001 };

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static struct entity* client_entity(uint32_t id) {
	struct entity** pp = dict_entity_get(gstate.entities, id);
	assert(pp && *pp);
	return *pp;
}

static void velocity_packet(struct server_remote* owner, int32_t id,
							int16_t x, int16_t y, int16_t z) {
	/* Packet28: 0x1C, i32 entity id, i16 motionX/Y/Z (big endian). */
	uint8_t bytes[11] = {0x1C};
	put_i32(bytes + 1, id);
	bytes[5] = (uint8_t)((uint16_t)x >> 8); bytes[6] = (uint8_t)x;
	bytes[7] = (uint8_t)((uint16_t)y >> 8); bytes[8] = (uint8_t)y;
	bytes[9] = (uint8_t)((uint16_t)z >> 8); bytes[10] = (uint8_t)z;
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_ENTITY_VELOCITY);
	assert(process_event(owner, &event));
}

static bool approx(float a, float b) {
	return fabsf(a - b) < 1e-6F;
}

int main(void) {
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct server_remote owner = {0};
	assert(server_remote_init(&owner));
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, LOCAL_ID);
	((struct server_remote_impl*)owner.impl)->entity_id = LOCAL_ID;
	gstate.remote_session = true;

	struct entity* self = gstate.players[0].local_player;
	check(self && self->id == LOCAL_ID, "fixture: local player is entity 42");
	/* A remote mob (Packet24), to prove other entities are untouched. */
	uint8_t pig[25] = {0x18};
	put_i32(pig + 1, MOB_ID);
	pig[5] = 90;
	put_i32(pig + 10, 320);
	pig[24] = 0x7f;
	struct remote_event spawn = {.kind = REMOTE_EVENT_PACKET};
	spawn.packet = decode_exact(pig, sizeof(pig), MP_EVENT_MOB_SPAWN);
	assert(process_event(&owner, &spawn));
	struct entity* mob = client_entity(MOB_ID);
	glm_vec3_zero(mob->vel);
	glm_vec3_zero(self->vel);

	/* Knockback-shaped Packet28 for the own entity id. */
	velocity_packet(&owner, LOCAL_ID, 3200, 3200, -1600);
	printf("     local vel after own Packet28: %f %f %f\n", self->vel[0],
		   self->vel[1], self->vel[2]);
	check(approx(self->vel[0], 3200 / 8000.0F) && approx(self->vel[1], 3200 / 8000.0F)
			  && approx(self->vel[2], -1600 / 8000.0F),
		  "own Packet28 sets local motionX/Y/Z = raw/8000 (setVelocity)");
	check(mob->vel[0] == 0 && mob->vel[1] == 0 && mob->vel[2] == 0,
		  "other entities unchanged by the own-id Packet28");

	/* A plain assignment, not an addition: a second packet replaces it. */
	velocity_packet(&owner, LOCAL_ID, -800, 0, 400);
	check(approx(self->vel[0], -0.1F) && approx(self->vel[1], 0.0F)
			  && approx(self->vel[2], 0.05F),
		  "a second own Packet28 replaces (assigns) the velocity");

	/* Another entity's Packet28 never moves the local player. */
	velocity_packet(&owner, MOB_ID, 8000, 8000, 8000);
	check(approx(self->vel[0], -0.1F) && approx(self->vel[2], 0.05F),
		  "another entity's Packet28 leaves the local player alone");


	server_remote_destroy(&owner);
	printf("%s test_mp_local_velocity (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
