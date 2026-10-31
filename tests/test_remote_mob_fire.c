/* Beta 1.7.3 remote Packet40 fire flag through the production codec,
 * remote dispatch, and client entity apply path. The S0 harness supplies
 * the normal client/world initialization without running its full suite.
 * [Packet40EntityMetadata.readPacketData:12-15;
 * DataWatcher.readWatchableObjects:111-120; Entity:227,1018-1039;
 * NetClientHandler.func_21148_a:172-178] */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

enum { MOB_ID = 61001, PLAYER_ID = 61002, UNKNOWN_ID = 61003,
	   BURNING_MOB_ID = 61004 };

static struct entity* client_entity(uint32_t id) {
	struct entity** pp = dict_entity_get(gstate.entities, id);
	assert(pp && *pp);
	return *pp;
}

static void send_metadata(struct server_remote* owner, int32_t id,
						  uint8_t flags) {
	/* Packet40 id 0x28, big-endian entity id, type-0/index-0 watcher
	 * header 0x00, one flag byte, then DataWatcher terminator 0x7f. */
	uint8_t bytes[] = {0x28, 0, 0, 0, 0, 0x00, flags, 0x7f};
	put_i32(bytes + 1, id);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_ENTITY_METADATA);
	assert(process_event(owner, &event));
}

static void spawn_pig(struct server_remote* owner, int32_t id,
					  uint8_t flags, uint8_t saddle) {
	/* Packet24: id, pig type, fixed-point x/y/z, yaw/pitch, then full
	 * DataWatcher with flags at index 0 and saddle at index 16. */
	uint8_t bytes[25] = {0x18};
	put_i32(bytes + 1, id);
	bytes[5] = 90;
	put_i32(bytes + 10, 320);
	bytes[20] = 0x00;
	bytes[21] = flags;
	bytes[22] = 0x10;
	bytes[23] = saddle;
	bytes[24] = 0x7f;
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_MOB_SPAWN);
	assert(process_event(owner, &event));
}

static void send_mixed_metadata(struct server_remote* owner, int32_t id,
						uint8_t flags, uint8_t saddle) {
	uint8_t bytes[] = {0x28, 0, 0, 0, 0, 0x00, flags,
		0x10, saddle, 0x7f};
	put_i32(bytes + 1, id);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_ENTITY_METADATA);
	assert(process_event(owner, &event));
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
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);
	gstate.remote_session = true;

	spawn_pig(&owner, BURNING_MOB_ID, 0x01, 1);
	struct entity* burning_pig = client_entity(BURNING_MOB_ID);
	if(!burning_pig->on_fire)
		fputs("RED remote mob fire: Packet24 full watcher fire flag lost at spawn\n", stderr);
	assert(burning_pig->on_fire && burning_pig->data.monster.saddled);
	spawn_pig(&owner, MOB_ID, 0x00, 1);
	struct entity* pig = client_entity(MOB_ID);
	assert(pig->type == ENTITY_MONSTER && !pig->on_fire
		&& pig->data.monster.saddled);
	send_mixed_metadata(&owner, MOB_ID, 0x01, 0);
	assert(pig->on_fire && !pig->data.monster.saddled);

	send_metadata(&owner, MOB_ID, 0x01);
	if(!pig->on_fire)
		fputs("RED remote mob fire: Packet40 flag 0x01 did not reach client mob\n", stderr);
	assert(pig->on_fire);
	struct remote_event move = {.kind = REMOTE_EVENT_PACKET};
	move.packet.type = MP_EVENT_REL_ENTITY_MOVE;
	move.packet.payload.rel_entity_move.entity_id = MOB_ID;
	move.packet.payload.rel_entity_move.dx = 1;
	assert(process_event(&owner, &move));
	assert(pig->on_fire); /* a MOVE carries no DataWatcher flag */
	send_metadata(&owner, MOB_ID, 0x00);
	assert(!pig->on_fire);

	struct remote_event player = {.kind = REMOTE_EVENT_PACKET};
	player.packet.type = MP_EVENT_NAMED_ENTITY_SPAWN;
	player.packet.payload.named_entity_spawn.entity_id = PLAYER_ID;
	player.packet.payload.named_entity_spawn.y = 320;
	assert(process_event(&owner, &player));
	struct entity* peer = client_entity(PLAYER_ID);
	assert(peer->type == ENTITY_REMOTE_PLAYER && !peer->on_fire);
	send_metadata(&owner, PLAYER_ID, 0x03);
	assert(peer->on_fire && peer->data.remote_player.sneaking);
	send_metadata(&owner, PLAYER_ID, 0x00);
	assert(!peer->on_fire && !peer->data.remote_player.sneaking);

	send_metadata(&owner, UNKNOWN_ID, 0x01);
	assert(!dict_entity_get(gstate.entities, UNKNOWN_ID));
	assert(!pig->on_fire && !peer->on_fire);
	server_remote_destroy(&owner);
	puts("test_remote_mob_fire: PASS");
	return 0;
}
