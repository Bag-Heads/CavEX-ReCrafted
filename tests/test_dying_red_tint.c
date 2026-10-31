/* next-gaps-0924/G8b: a dying mob / remote player keeps Beta's red overlay.
 *
 * Beta RenderLiving.doRenderLiving (RenderLiving.java:67-75):
 *   if((colorMultiplier alpha) > 0 || hurtTime > 0 || deathTime > 0) { ...
 *       if(hurtTime > 0 || deathTime > 0) glColor4f(brightness, 0, 0, 0.4F);
 * so the red wash lasts the whole deathTime flop, not just the 10-tick
 * hurtTime window. Both client feeds are driven through production code:
 *   SP -- the CRPC_ENTITY_MOVE entity_monster.c's dying branch sends
 *         (hurt = false, death_time = n) into clin_process_direct;
 *   MP -- Packet38 status 2 then 3 through the real codec + process_event,
 *         then flush_entity_status after the 500 ms hurt window has passed
 *         (hurt cleared, deathTime still running).
 * The render gate read is entity_living_red_tint, the predicate both
 * entity_monster.c's and entity_remote_player.c's render bracket call.
 * Non-aborting: every case prints ok/FAIL so a RED run shows all of them. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

#ifdef _WIN32
#include <windows.h>
#define wait_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define wait_ms(ms) usleep((ms) * 1000)
#endif

enum { MOB_ID = 62001, PLAYER_ID = 62002, SP_MOB_ID = 62003 };

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

static void spawn_pig(struct server_remote* owner, int32_t id) {
	/* Packet24 (test_remote_mob_fire's vector): id, pig type 90, fixed-point
	 * x/y/z, yaw/pitch, DataWatcher flags@0, saddle@16, terminator. */
	uint8_t bytes[25] = {0x18};
	put_i32(bytes + 1, id);
	bytes[5] = 90;
	put_i32(bytes + 10, 320);
	bytes[20] = 0x00;
	bytes[21] = 0x00;
	bytes[22] = 0x10;
	bytes[23] = 0x00;
	bytes[24] = 0x7f;
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_MOB_SPAWN);
	assert(process_event(owner, &event));
}

static void send_status(struct server_remote* owner, int32_t id, uint8_t status) {
	/* Packet38EntityStatus: id 0x26, int entity id, byte status. */
	uint8_t bytes[6] = {0x26, 0, 0, 0, 0, status};
	put_i32(bytes + 1, id);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_ENTITY_STATUS);
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

	/* ---- MP: a remote mob and a remote player, hurt then killed. ---- */
	spawn_pig(&owner, MOB_ID);
	struct remote_event player = {.kind = REMOTE_EVENT_PACKET};
	player.packet.type = MP_EVENT_NAMED_ENTITY_SPAWN;
	player.packet.payload.named_entity_spawn.entity_id = PLAYER_ID;
	player.packet.payload.named_entity_spawn.y = 320;
	assert(process_event(&owner, &player));
	struct entity* pig = client_entity(MOB_ID);
	struct entity* peer = client_entity(PLAYER_ID);
	check(!entity_living_red_tint(pig) && !entity_living_red_tint(peer),
		  "MP alive, unhurt: no red");

	send_status(&owner, MOB_ID, 2);
	send_status(&owner, PLAYER_ID, 2);
	check(entity_living_red_tint(pig) && entity_living_red_tint(peer),
		  "MP status 2 (hurtTime window): red");
	send_status(&owner, MOB_ID, 3);
	send_status(&owner, PLAYER_ID, 3);
	wait_ms(650); /* past the 500 ms hurtTime stand-in, inside the flop */
	flush_entity_status((struct server_remote_impl*)owner.impl);
	printf("     MP after 650 ms: mob hurt=%d death_time=%d, player hurt=%d death_time=%d\n",
		   pig->hurt, pig->data.monster.death_time, peer->hurt,
		   peer->data.remote_player.death_time);
	check(!pig->hurt && pig->data.monster.death_time > 0,
		  "MP mob: hurt window over, deathTime running (fixture sanity)");
	check(!peer->hurt && peer->data.remote_player.death_time > 0,
		  "MP player: hurt window over, deathTime running (fixture sanity)");
	check(entity_living_red_tint(pig), "MP dying mob keeps red (deathTime > 0)");
	check(entity_living_red_tint(peer), "MP dying remote player keeps red (deathTime > 0)");

	/* ---- SP: the dying-branch CRPC_ENTITY_MOVE (hurt=false). ---- */
	spawn_pig(&owner, SP_MOB_ID);
	struct entity* sp = client_entity(SP_MOB_ID);
	struct client_rpc move = {.type = CRPC_ENTITY_MOVE};
	move.payload.entity_move.entity_id = SP_MOB_ID;
	move.payload.entity_move.pos[0] = sp->pos[0];
	move.payload.entity_move.pos[1] = sp->pos[1];
	move.payload.entity_move.pos[2] = sp->pos[2];
	move.payload.entity_move.hurt = false;
	move.payload.entity_move.fuse = -1;
	move.payload.entity_move.death_time = 5;
	clin_process_direct(&move);
	check(!sp->hurt && sp->data.monster.death_time == 5,
		  "SP dying move applied (hurt=false, death_time=5)");
	check(entity_living_red_tint(sp), "SP dying mob keeps red (deathTime > 0)");
	move.payload.entity_move.death_time = 0;
	clin_process_direct(&move);
	check(!entity_living_red_tint(sp), "SP alive again (death_time 0, unhurt): no red");

	server_remote_destroy(&owner);
	if(failures) {
		printf("test_dying_red_tint: %u FAILED\n", failures);
		return 1;
	}
	puts("test_dying_red_tint: PASS");
	return 0;
}
