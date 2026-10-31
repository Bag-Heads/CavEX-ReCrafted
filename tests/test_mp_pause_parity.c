/* Beta 1.7.3 Minecraft.java:598: a GUI pauses only a local world. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "chunk.h"
#include "entity/entity.h"
#include "game/agent_input.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/input.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

#define CAVEWII_F1_DISPATCH_ONLY
#include "../source/main.c"
#undef CAVEWII_F1_DISPATCH_ONLY

void screen_pause_test_activate_row(size_t row);
bool clin_test_stream_restore_pause_screen(void);
static unsigned toggles, positions, unloads;
static float last_wire_y;
static volatile int drain_running = 1;
static void* drain_client(void* unused) {
	(void)unused;
	while(drain_running) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
static void observe(struct server_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == SRPC_TOGGLE_PAUSE) toggles++;
	if(rpc->type == SRPC_UNLOAD_WORLD) unloads++;
	if(rpc->type == SRPC_PLAYER_POS) {
		positions++;
		last_wire_y = rpc->payload.player_pos.y;
	}
}
static void drain(void) {
	svin_process_messages(observe, NULL, false);
	for(int i = 0; i < 8 && svin_pending_count(); i++) {
		svin_rpc_flush_pending();
		svin_process_messages(observe, NULL, false);
	}
	assert(svin_pending_count() == 0);
}
static void fill_action_queue(void) {
	for(int i = 0; i < 16; i++) {
		struct server_rpc pos = {.type = SRPC_PLAYER_POS};
		assert(svin_rpc_send(&pos));
	}
	for(int i = 0; i < 64; i++) {
		struct server_rpc dig = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.x = i, .y = 64, .z = 0}};
		assert(svin_rpc_send(&dig));
	}
}
static struct entity* body(void) {
	return gstate.players[0].local_player;
}
static void place_airborne(void) {
	struct entity* p = body();
	assert(p && p->tick_client);
	for(int x = 6; x <= 10; x++)
		for(int z = 6; z <= 10; z++)
			world_set_block(&gstate.world, x, 64, z,
				(struct block_data){.type = BLOCK_AIR}, false);
	p->pos[0] = 8.5F; p->pos[1] = 82.0F; p->pos[2] = 8.5F;
	memcpy(p->pos_old, p->pos, sizeof p->pos);
	p->vel[0] = p->vel[1] = p->vel[2] = 0.0F;
	p->on_ground = false;
	p->health = 20;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.players[0].current_screen = &screen_ingame;
	gstate.players[0].camera.x = p->pos[0];
	gstate.players[0].camera.y = p->pos[1];
	gstate.players[0].camera.z = p->pos[2];
	drain(); toggles = positions = 0;
}
static void game_tick(void) {
	/* Both providers publish from the real completed owner tick. */
	if(!gstate.paused && game_state_client_entities_active(&gstate)) {
		const unsigned slots[] = {0};
		main_client_host_tick_dispatch(slots, 1, true, false, -1);
	}
	clin_update();
	drain();
}
static void test_sp_home_pause_freezes_ticks(void) {
	gstate.remote_session = false; gstate.paused = false;
	place_airborne();
	game_tick();
	assert(positions == 1 && last_wire_y == body()->pos[1]);
	assert(screen_ingame_test_try_pause());
	drain();
	assert(toggles == 1 && gstate.paused);
	assert(gstate.players[0].current_screen == &screen_pause);
	float y = body()->pos[1];
	for(int i = 0; i < 4; i++) game_tick();
	assert(body()->pos[1] == y && last_wire_y == y && positions == 1);
	screen_pause_test_process_edges(true, false); drain();
	assert(toggles == 2 && !gstate.paused);
	game_tick(); game_tick();
	assert(body()->pos[1] < y);
	puts("PASS test_sp_home_pause_freezes_ticks");
}
static void test_remote_home_menu_keeps_body_ticking(void) {
	gstate.remote_session = true; gstate.paused = false;
	place_airborne();
	agent_input_feed_line("FORWARD=1"); agent_input_poll();
	assert(input_held(IB_FORWARD));
	assert(screen_ingame_test_try_pause()); drain();
	assert(gstate.players[0].current_screen == &screen_pause);
	assert(toggles == 0 && !gstate.paused);
	assert(!body()->data.local_player.capture_input);
	float x = body()->pos[0], y = body()->pos[1], z = body()->pos[2];
	for(int i = 0; i < 5; i++) game_tick();
	assert(body()->pos[1] < y && body()->pos[0] == x && body()->pos[2] == z);
	assert(positions > 0 && last_wire_y == body()->pos[1] && toggles == 0);
	agent_input_feed_line("FORWARD=0"); agent_input_poll();
	puts("PASS test_remote_home_menu_keeps_body_ticking");
}
static void test_remote_menu_dwell_outlasts_server_timeout(void) {
	/* REL-15. A vanilla server drops a client after 1200 ticks with no
	 * packet read (CFR NetworkManager.java:214-215). Beta's client keeps
	 * ticking a multiplayer world behind the menu (Minecraft.java:598,
	 * :1150) and sends a movement packet every tick, Packet10Flying even
	 * when idle (EntityClientPlayerMP.java:71-85). So a grounded body behind
	 * the open remote menu must publish one position per tick for longer
	 * than that timeout, with an honest onGround. */
	gstate.remote_session = true; gstate.paused = false;
	place_airborne();
	world_set_block(&gstate.world, 8, 70, 8,
		(struct block_data){.type = BLOCK_STONE}, false);
	assert(screen_ingame_test_try_pause()); drain();
	for(int i = 0; i < 40 && !body()->on_ground; i++) game_tick();
	assert(body()->on_ground && fabsf(body()->pos[1] - 72.62F) < 0.01F); /* eye y */
	positions = 0;
	const unsigned ticks = 1260; /* 63 s > the server's 60 s read timeout */
	for(unsigned i = 0; i < ticks; i++) game_tick();
	assert(positions == ticks && last_wire_y == body()->pos[1]);
	assert(body()->on_ground && toggles == 0 && !gstate.paused);
	assert(gstate.players[0].current_screen == &screen_pause);
	world_set_block(&gstate.world, 8, 70, 8,
		(struct block_data){.type = BLOCK_AIR}, false);
	puts("PASS test_remote_menu_dwell_outlasts_server_timeout");
}
static void test_pause_screen_round_trip(void) {
	/* Fill the ordinary-action reserve as in test_portal_travel. HOME must
	 * leave the local game on screen when its toggle cannot enter the queue. */
	gstate.remote_session = false; gstate.paused = false;
	place_airborne();
	fill_action_queue();
	assert(!screen_ingame_test_try_pause());
	assert(!gstate.paused && gstate.players[0].current_screen == &screen_ingame);
	drain();
	assert(toggles == 0);
	toggles = positions = 0;
	assert(screen_ingame_test_try_pause()); drain();
	assert(toggles == 1 && gstate.paused);
	fill_action_queue();
	screen_pause_test_process_edges(true, false);
	assert(gstate.paused && gstate.players[0].current_screen == &screen_pause);
	drain();
	assert(toggles == 1);
	screen_pause_test_process_edges(true, false); drain();
	assert(toggles == 2 && !gstate.paused);
	gstate.remote_session = true;
	place_airborne(); toggles = 0;
	assert(screen_ingame_test_try_pause()); drain();
	screen_pause_test_process_edges(true, false); drain();
	assert(gstate.players[0].current_screen == &screen_ingame);
	assert(toggles == 0 && !gstate.paused);
	assert(screen_ingame_test_try_pause()); drain();
	screen_pause_test_activate_row(2); drain(); /* Remote Options. */
	assert(gstate.players[0].current_screen == &screen_options);
	assert(toggles == 0 && !gstate.paused);
	screen_set(&screen_pause);
	unsigned before_unloads = unloads;
	screen_pause_test_activate_row(screen_pause_test_button_count(true) - 1);
	drain(); /* Remote Leave: reset, reliable unload, then title. */
	assert(gstate.players[0].current_screen == &screen_title);
	assert(!gstate.world_loaded);
	assert(unloads == before_unloads + 1 && toggles == 0 && !gstate.paused);
	puts("PASS test_pause_screen_round_trip");
}
static void test_remote_controller_loss_recovery_does_not_pause(void) {
	gstate.remote_session = true; gstate.paused = false;
	place_airborne();
	assert(screen_ingame_test_try_pause()); drain();
	assert(input_player_bind(0, (struct input_device){INPUT_DEVICE_KEYBOARD, 0}));
	input_poll(); /* No PC window: the bound device is disconnected. */
	assert(screen_controller_recovery()); drain();
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_LOST);
	assert(gstate.players[0].current_screen == &screen_pause);
	assert(toggles == 0 && !gstate.paused);
	float y = body()->pos[1];
	for(int i = 0; i < 3; i++) game_tick();
	assert(body()->pos[1] < y && positions > 0 && toggles == 0);
	struct input_player_frame* frame = (struct input_player_frame*)input_player_frame(0);
	assert(frame);
	frame->connected = true;
	assert(screen_controller_recovery());
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_READY);
	assert(toggles == 0 && !gstate.paused);
	assert(screen_controller_recovery()); /* Reconnect alone cannot dismiss. */
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_READY);
	frame->pressed[IB_GUI_CLICK] = true;
	assert(screen_controller_recovery());
	frame->pressed[IB_GUI_CLICK] = false;
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_NONE);
	assert(toggles == 0 && !gstate.paused);
	input_player_unbind(0);
	puts("PASS test_remote_controller_loss_recovery_does_not_pause");
}
static void test_remote_exit_settled_does_not_pause(void) {
	/* Arm the local unload path, then model a remote RETURN_INGAME settle. */
	gstate.remote_session = false; gstate.paused = true;
	place_airborne();
	screen_set(&screen_pause);
	screen_pause_test_activate_row(screen_pause_test_button_count(true) - 1);
	drain();
	assert(screen_pause_exit_pending() && !gstate.paused);
	gstate.remote_session = true;
	unsigned before = toggles;
	screen_pause_exit_settled(false); drain();
	assert(!screen_pause_exit_pending());
	assert(gstate.players[0].current_screen == &screen_pause);
	assert(toggles == before && !gstate.paused);
	puts("PASS test_remote_exit_settled_does_not_pause");
}
static struct client_rpc full_chunk(w_coord_t x, w_coord_t z) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	memset(slot, 0, sizeof(*slot));
	return (struct client_rpc){.type = CRPC_CHUNK, .payload.chunk = {
		.x = x, .y = 0, .z = z, .sx = CHUNK_SIZE,
		.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}};
}
static void test_remote_stream_recovery_restores_pause_screen(void) {
	/* Beta never pauses a multiplayer world (Minecraft.java:598), but the
	 * SCREEN must come back exactly as it went in: a recovery begun from the
	 * remote pause menu ends on screen_pause again at handoff -- with no
	 * toggle and gstate.paused still false. */
	gstate.remote_session = true; gstate.paused = false;
	clin_test_reset_input_epoch_guard();
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	struct client_rpc center = full_chunk(0, 0);
	clin_process(&center);
	clin_process(&(struct client_rpc){.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {0.5, 65.62, 0.5}, .input_epoch = 1}});
	assert(gstate.players[0].local_player && gstate.world_loaded);
	screen_set(&screen_pause);
	assert(!gstate.paused);
	drain(); toggles = 0;
	chunk_test_fail_alloc_after(1);
	struct client_rpc failed = full_chunk(112, 112);
	clin_process(&failed);
	assert(gstate.players[0].current_screen == &screen_load_world);
	/* The pause SCREEN is recorded in a remote session exactly as in a local
	 * one; only the pause FLAG / toggle is local business. */
	assert(clin_test_stream_restore_pause_screen());
	assert(!gstate.paused);
	drain();
	clin_process(&(struct client_rpc){.type = CRPC_STREAM_RESET});
	center = full_chunk(0, 0); clin_process(&center);
	clin_process(&(struct client_rpc){.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {0.5, 65.62, 0.5}, .input_epoch = 2}});
	/* The accepted epoch ACK releases the input handoff, which concludes the
	 * recovery: the screen it recorded is the one that comes back. (A LATER
	 * unrelated FINISH_REMOTE_LOAD correction re-exposes ingame in either
	 * session kind -- that is the login/terrain-hold release, not this one.) */
	assert(gstate.world_loaded && !gstate.world_switch_pending);
	drain();
	assert(gstate.players[0].current_screen == &screen_pause);
	assert(!gstate.paused && toggles == 0);
	puts("PASS test_remote_stream_recovery_restores_pause_screen");
}
static void test_recovery_ownership_resets_on_session_switch(void) {
	/* A REMOTE recovery modal holds primary_recovery.owns_pause without ever
	 * pausing the world (screen.c). The provider teardown (main.c:3500) then
	 * clears gstate.remote_session, so the production recovery entry must
	 * drop a record the new session kind cannot own -- otherwise the local
	 * machine claims a pause the player opened themselves, which
	 * controller_events.c's ACT_RESUME would then take away from them. */
	gstate.remote_session = true; gstate.paused = false;
	place_airborne();
	assert(screen_ingame_test_try_pause()); drain();
	assert(gstate.players[0].current_screen == &screen_pause);
	assert(input_player_bind(0, (struct input_device){INPUT_DEVICE_KEYBOARD, 0}));
	input_poll(); /* No PC window: the bound device is disconnected. */
	assert(screen_controller_recovery()); drain();
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_LOST);
	assert(screen_controller_recovery_state()->owns_pause);
	assert(toggles == 0 && !gstate.paused);
	struct input_player_frame* frame = (struct input_player_frame*)input_player_frame(0);
	assert(frame);
	frame->connected = true;
	assert(screen_controller_recovery());
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_READY);
	assert(screen_controller_recovery_state()->owns_pause);
	assert(toggles == 0 && !gstate.paused);

	/* The remote provider is torn down: this is a LOCAL session again. */
	gstate.remote_session = false;
	screen_set(&screen_ingame);
	toggles = 0;
	assert(screen_ingame_test_try_pause()); drain();
	assert(toggles == 1 && gstate.paused);
	/* No modal may survive the session change, and nothing may claim the
	 * pause the player just opened. */
	assert(!screen_controller_recovery());
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_NONE);
	assert(!screen_controller_recovery_state()->owns_pause);
	frame->pressed[IB_GUI_CLICK] = true;
	assert(!screen_controller_recovery()); drain();
	frame->pressed[IB_GUI_CLICK] = false;
	assert(toggles == 1 && gstate.paused);
	assert(gstate.players[0].current_screen == &screen_pause);

	/* A fresh LOCAL controller loss now pauses exactly as single player. */
	screen_pause_test_process_edges(true, false); drain();
	assert(toggles == 2 && !gstate.paused);
	frame->connected = false;
	assert(screen_controller_recovery()); drain();
	assert(toggles == 3 && gstate.paused);
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_LOST);
	assert(screen_controller_recovery_state()->owns_pause);
	frame->connected = true;
	assert(screen_controller_recovery());
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_READY);
	frame->pressed[IB_GUI_CLICK] = true;
	assert(screen_controller_recovery()); drain();
	frame->pressed[IB_GUI_CLICK] = false;
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_NONE);
	assert(toggles == 4 && !gstate.paused);
	input_player_unbind(0);
	puts("PASS test_recovery_ownership_resets_on_session_switch");
}
int main(void) {
	static char agent_env[] = "CAVEWII_AGENT=1";
	putenv(agent_env);
	assert(clin_init());
	assert(svin_init());
	struct thread drain_thread;
	thread_create(&drain_thread, drain_client, NULL, 64);
	blocks_init(); items_init();
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	struct entity* player = calloc(1, sizeof *player);
	assert(player);
	entity_local_player(1, player, &gstate.world);
	gstate.players[0].local_player = player;
	dict_entity_set_at(gstate.entities, player->id, player);
	assert(body());
	test_sp_home_pause_freezes_ticks();
	test_remote_home_menu_keeps_body_ticking();
	test_remote_menu_dwell_outlasts_server_timeout();
	test_pause_screen_round_trip();
	test_remote_stream_recovery_restores_pause_screen();
	test_remote_exit_settled_does_not_pause();
	test_remote_controller_loss_recovery_does_not_pause();
	test_recovery_ownership_resets_on_session_switch();
	drain_running = 0;
	thread_join(&drain_thread);
	return 0;
}
