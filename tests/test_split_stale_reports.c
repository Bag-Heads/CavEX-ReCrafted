/* 09-16 console reports, checked against the production input, pause, session,
 * layout, and local-server quit paths. Physical WPAD reports remain a Wii gate. */
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "support/test_fixture_dir.h"
#include "platform/input_owner.h"
#include "game/client_session.h"
#include "game/gui/local_menu_host.h"
#include "game/split_layout.h"
#include "game/game_state.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_local_guest.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/inventory_logic.h"
#include "block/blocks.h"
#include "item/items.h"
#include "item/recipe.h"
#include "platform/menu_sound.h"

#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MAKE_DIR(path) mkdir(path, 0700)
#endif

void __wrap_audio_menu_event(enum menu_sound_event event, ptime_t now) {
	(void)event; (void)now;
}

static void test_classic_unplug(void) {
	struct input_owner input = {0};
	struct input_device remote = {INPUT_DEVICE_WPAD, 1};
	assert(input_owner_bind(&input, 1, 0, remote));
	struct client_session guest = {.local_ticket=7, .local_generation=11,
		.local_bound=true, .local_phase=CLIENT_GUEST_ATTACHED};
	assert(local_pause_open(&guest.pause, false));
	struct input_player_frame sample = {.connected=true,
		.category=INPUT_CAT_CLASSIC_CONTROLLER};
	input_owner_latch(&input, &sample);
	input_owner_latch(&input, &sample); /* neutral rearm */
	/* An unplug report may carry a stale click and direction together. */
	sample.category=INPUT_CAT_WIIMOTE;
	sample.down[IB_GUI_DOWN]=sample.down[IB_GUI_CLICK]=true;
	input_owner_latch(&input, &sample);
	assert(!input.frame.connected && !input.frame.pressed[IB_GUI_CLICK]
		&& !input.frame.pressed[IB_GUI_DOWN]);
	assert(local_menu_host_recovery_frame(&guest, 1, &input.frame,
		0, 240, 640, 240));
	assert(local_pause_update(&guest.pause, &guest, &input.frame,
		0, 240, 640, 240)==LOCAL_PAUSE_NONE);
	assert(guest.local_phase==CLIENT_GUEST_ATTACHED && !guest.local_pending
		&& !guest.pause.exit_pending && guest.menu_host.recovery.phase==CONTROLLER_RECOVERY_LOST);
	struct split_layout layout; split_layout_reset(&layout);
	const bool present[2]={false,false}, lost[2]={false,true};
	split_layout_step(&layout,true,present,.35F);
	split_layout_step(&layout,true,present,.35F);
	assert(layout.boundary==.5F);
	split_layout_step(&layout,true,lost,0);
	split_layout_step(&layout,true,lost,4.99F);
	assert(layout.target==.5F);
	split_layout_step(&layout,true,lost,.01F);
	assert(layout.target==1.0F);
	split_layout_step(&layout,true,lost,.35F);
	assert(layout.boundary==1.0F && guest.local_bound);
	puts("PASS R-A: same-report Classic unplug suppresses stale Leave; recovery starts and layout expands after five seconds");
}

static void test_quit_with_unacked_leave(void) {
	assert(clin_init());
	assert(svin_init());
	char path[256];
	test_fixture_dir("test_split_stale_reports",path,sizeof path);
	assert(MAKE_DIR(path)==0);
	static struct server_local server;
	server_local_init(&server,false,false,false,false,false,false,false);
	string_set_str(server.level_name,path);
	assert(level_archive_create_new(&server.level,server.level_name,12345,
		(vec3){4,72,4}));
	assert(level_archive_save(&server.level));
	server_world_create(&server.world,server.level_name,WORLD_DIM_OVERWORLD,12345,true);
	server.worlds[0].session = &server; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&server))
		abort();
	server.player.active_inventory=&server.player.inventory;
	server.player.has_pos=server.player.finished_loading=true;
	server.player.x=4;server.player.y=72;server.player.z=4;
	server.player.health=160;
	server.player.dimension=WORLD_DIM_OVERWORLD;
	struct server_rpc_source source;
	assert(server_local_guest_create(&server,&source));
	assert(server.guest_actor && server.guest_owned);
	struct client_session guest={.local_ticket=7, .local_generation=source.generation,
		.local_bound=true, .local_phase=CLIENT_GUEST_ATTACHED};
	assert(client_session_request_local_leave(&guest));
	assert(guest.local_phase==CLIENT_GUEST_LEAVING && guest.local_pending
		&& server.guest_actor); /* leave has not reached the server */
	struct client_session host={0};
	assert(local_pause_open(&host.pause,true));
	struct input_player_frame press={.connected=true};
	press.pressed[IB_GUI_DOWN]=true;
	press.pressed[IB_GUI_RIGHT]=true;
	assert(local_pause_update(&host.pause,&host,&press,0,0,640,240)==LOCAL_PAUSE_NONE);
	assert(host.pause.page==LOCAL_PAUSE_MENU && host.pause.selected==3);
	press.pressed[IB_GUI_RIGHT]=false;
	press.pressed[IB_GUI_DOWN]=false; press.pressed[IB_GUI_CLICK]=true;
	assert(local_pause_update(&host.pause,&host,&press,0,0,640,240)==LOCAL_PAUSE_NONE);
	assert(host.pause.page==LOCAL_PAUSE_HOST_CONFIRM && host.pause.selected==0);
	press.pressed[IB_GUI_CLICK]=false; press.pressed[IB_GUI_DOWN]=true;
	assert(local_pause_update(&host.pause,&host,&press,0,0,640,240)==LOCAL_PAUSE_NONE);
	press.pressed[IB_GUI_DOWN]=false; press.pressed[IB_GUI_CLICK]=true;
	assert(local_pause_update(&host.pause,&host,&press,0,0,640,240)==LOCAL_PAUSE_HOST_EXIT
		&& host.pause.exit_pending);
	assert(svin_rpc_send(&(struct server_rpc){.type=SRPC_UNLOAD_WORLD}));
	server_local_test_process_messages(&server);
	assert(!server.player.has_pos && !server.level.data && !server.guest_actor);
	assert(guest.local_phase==CLIENT_GUEST_LEAVING); /* client reply still unconsumed */
	test_fixture_dir_cleanup(path);
	puts("PASS R-B: host quit commits with a server-owned guest and an unacked client Leave");
}

int main(void) {
	blocks_init(); items_init(); recipe_init();
	test_classic_unplug();
	test_quit_with_unacked_leave();
	return 0;
}
