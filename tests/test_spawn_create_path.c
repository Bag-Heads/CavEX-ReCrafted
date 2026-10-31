/*
	SPEC R3: the REAL create-world path (screen_select_world.c's dialog create,
	as the GUI uses it) yields Beta's spawn for the seed whose search Beta can
	only reproduce by populating chunks mid-search: 3141592653589793238
	(tests/oracle/spawn/sweep_fixtures.json: spawn (-321,64,296), first-join
	feet (-320.5,65,296.5); a raw-terrain probe gives (-319,64,294)).

	Flow under test, single-threaded: screen_sworld_create_from_dialog ->
	sworld_finish_create -> server_local_create_spawns (the svin_spawn_*
	mailbox: the create flow's wait services the server half through the
	test wait hook, which is where the server thread would run
	server_local_spawn_search_service) -> level.dat -> then the server loads
	the world exactly like SRPC_LOAD_WORLD does and the player's feet are
	read. World.rand is pinned to the fixture's state through the worldgen
	test pin (production is clock-seeded, World.java:169).

	Exit 0 = SpawnXYZ and feet match Beta. 1 = mismatch. 2 = setup failure.
*/

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "config.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "parson/parson.h"
#include "worldgen/worldgen.h"

static struct server_local s_srv;
static char s_saves[160];
static unsigned s_serviced;

static void setup_fail(const char* what) {
	printf("R3-create SETUP-FAILURE: %s\n", what);
	exit(2);
}

/* The server thread's half, run from the create flow's wait. */
static void service(void) {
	s_serviced++;
	server_local_spawn_search_service(&s_srv);
}

int main(void) {
	puts("test_spawn_create_path: GUI create path vs Beta spawn, seed "
		 "3141592653589793238 (SPEC R3)");
	const int64_t seed = 3141592653589793238LL;
	const uint64_t state48 = 275016690400420ULL; /* fixture spawn_rng.state48 */
	const int beta_spawn[3] = {-321, 64, 296};
	const double beta_feet[3] = {-320.5, 65.0, 296.5};

	test_fixture_dir("test_spawn_create_path_tmp", s_saves, sizeof s_saves);
	screen_sworld_test_delete_tree(s_saves);
	MKDIR(s_saves);
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	config_set_string(&gstate.config_user, "paths.worlds", s_saves);
	config_set_string(&gstate.config_user, "debug.superflat", "off");
	config_set_string(&gstate.config_user, "debug.start_nether", "off");
	if(!clin_init() || !svin_init())
		setup_fail("clin_init/svin_init");
	blocks_init();
	items_init();
	/* main.c: a local session is up (the mailbox has a provider). */
	svin_set_local_motion(true);
	memset(&s_srv, 0, sizeof(s_srv));
	server_local_test_create_without_thread(&s_srv, false, false, false, false,
											false, false, false);
	server_local_test_spawn_search_wait_hook = service;
	worldgen_test_pin_spawn_rng_state(true, state48);

	const bool created = screen_sworld_create_from_dialog(
		"Pi World", "Pi World", true, seed, false);
	worldgen_test_pin_spawn_rng_state(false, 0);
	server_local_test_spawn_search_wait_hook = NULL;
	if(!created)
		setup_fail("screen_sworld_create_from_dialog returned false");
	if(!s_serviced) {
		printf("R3-create RED: the create flow never asked the server half "
			   "(server_local_create_spawns took the in-thread path)\n");
		return 1;
	}

	string_t lvl;
	string_init_printf(lvl, "%s/Pi World", s_saves);
	struct level_archive la;
	if(!level_archive_create(&la, lvl))
		setup_fail("level.dat not readable after create");
	int32_t sx = 0, sy = 0, sz = 0;
	if(!level_archive_read(&la, LEVEL_WORLD_SPAWNX, &sx, 0)
	   || !level_archive_read(&la, LEVEL_WORLD_SPAWNY, &sy, 0)
	   || !level_archive_read(&la, LEVEL_WORLD_SPAWNZ, &sz, 0))
		setup_fail("level.dat without SpawnXYZ");
	level_archive_discard(&la);
	int failures = 0;
	if(sx != beta_spawn[0] || sy != beta_spawn[1] || sz != beta_spawn[2]) {
		printf("R3-create RED: SpawnXYZ Beta (%d,%d,%d), CaveWii create path "
			   "(%d,%d,%d)\n",
			   beta_spawn[0], beta_spawn[1], beta_spawn[2], sx, sy, sz);
		failures++;
	}

	/* The SRPC_LOAD_WORLD the create tail queued is served here directly. */
	if(!server_local_load_world_impl(&s_srv, lvl))
		setup_fail("server_local_load_world_impl");
	for(unsigned n = 0; n < 4000 && !s_srv.player.finished_loading; n++) {
		server_local_test_update_once(&s_srv);
		clin_drain_discard();
	}
	if(!s_srv.player.finished_loading)
		setup_fail("world load did not complete in 4000 updates");
	const double feet[3] = {s_srv.player.x,
							s_srv.player.y - (double)PLAYER_EYE_HEIGHT,
							s_srv.player.z};
	for(int i = 0; i < 3; i++)
		if(feet[i] != beta_feet[i]) {
			printf("R3-create RED: player feet Beta (%.4f,%.4f,%.4f), CaveWii "
				   "(%.4f,%.4f,%.4f)\n",
				   beta_feet[0], beta_feet[1], beta_feet[2], feet[0], feet[1],
				   feet[2]);
			failures++;
			break;
		}
	server_local_unload_world_impl(&s_srv, false);
	clin_drain_discard();
	string_clear(lvl);
	screen_sworld_test_delete_tree(s_saves);
	test_fixture_dir_cleanup(s_saves);
	if(failures) {
		printf("test_spawn_create_path: RED\n");
		return 1;
	}
	printf("test_spawn_create_path: GREEN -- the GUI create path spawns at "
		   "(%d,%d,%d), feet (%.1f,%.1f,%.1f), served by the server half "
		   "(%u service calls)\n",
		   sx, sy, sz, feet[0], feet[1], feet[2], s_serviced);
	return 0;
}
