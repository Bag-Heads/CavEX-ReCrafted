/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* REL-03b (R3, maintainer ruling 2026-10-17): a FAILED new-world spawn search
 * must stop world creation with a visible error instead of saving the
 * (0.5, 64, 0.5) placeholder. Beta sets the spawn only once the sand
 * predicate succeeds (World.java:215-227); there is no "no spawn found"
 * world in Beta.
 *
 * Failure is forced through the existing --wrap=mem2_alloc fault-injection
 * seam (mem2.h, mem2_tag): while armed, every allocation of a server chunk's
 * block-id array (SERVER_CHUNK_IDS_BYTES, server_chunk_arrays_alloc on the PC
 * five-array shape) returns NULL, so worldgen_generate_chunk fails inside the
 * real spawn search. Everything else is production code: the real worldgen,
 * the real screen_select_world.c create tail, the real level.dat writer.
 *
 * Cases:
 *   (1) the status query reports the failure (and the seam was reached);
 *       a later superflat / successful search reports success again.
 *   (2) the zero-input "+new" create path: no world directory, no level.dat,
 *       and the select-world status line shows the spawn error.
 *   (3) the create-world dialog entry returns false (the dialog then shows
 *       its failure line and stays retryable) and persists nothing.
 *   (4) control: the same dialog create unarmed persists a level.dat, so (3)
 *       is not vacuous.
 * Prints "REL-03b RED" and exits 1 on any failure. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "network/server_interface.h"
#include "network/server_world.h"
#include "worldgen/worldgen.h"

/* screen_select_world.c test hook (declared locally, as in
 * tests/test_frontend_no_pointer.c): the status/hint line the render draws. */
bool screen_sworld_test_hint(char* text, size_t cap, int* y, int* h,
							 int* list_bottom, int* strip_top);

#define SPAWN_ERROR_TEXT "Couldn't find a spawn point"

/* ---- the fault-injection seam (-Wl,--wrap=mem2_alloc) ------------------ */
void* __real_mem2_alloc(size_t size);
static bool s_fail_chunk_ids;
static unsigned s_refused;
void* __wrap_mem2_alloc(size_t size) {
	if(s_fail_chunk_ids && size == SERVER_CHUNK_IDS_BYTES) {
		s_refused++;
		return NULL;
	}
	return __real_mem2_alloc(size);
}

static char s_saves[160];
static int s_failures;

static void check(bool ok, const char* what) {
	if(!ok) {
		printf("REL-03b RED: %s\n", what);
		s_failures++;
	}
}

static bool path_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static void case_status_query(void) {
	float world_spawn[3], player_spawn[3];
	s_refused = 0;
	s_fail_chunk_ids = true;
	worldgen_find_initial_spawns(12345, false, false, world_spawn,
								 player_spawn);
	s_fail_chunk_ids = false;
	check(s_refused > 0, "(1) the chunk-generation fault seam was never reached");
	check(!worldgen_last_spawn_search_ok(),
		  "(1) worldgen_last_spawn_search_ok() reports success after a failed "
		  "spawn search");

	worldgen_find_initial_spawns(12345, true, false, world_spawn, player_spawn);
	check(worldgen_last_spawn_search_ok(),
		  "(1) superflat (no search) must report success");

	s_fail_chunk_ids = true;
	worldgen_find_initial_spawns(12345, false, false, world_spawn,
								 player_spawn);
	s_fail_chunk_ids = false;
	/* Seed 12345: column (0, 0) is already sand (tests/oracle/spawn fixture
	 * Spawn=(0,64,0)), so the successful search is one chunk. */
	worldgen_find_initial_spawns(12345, false, false, world_spawn, player_spawn);
	check(worldgen_last_spawn_search_ok(),
		  "(1) a successful search after a failure must report success");
	check(world_spawn[0] == 0.5F && world_spawn[1] == 64.0F
			  && world_spawn[2] == 0.5F,
		  "(1) seed 12345 successful search must give SpawnXYZ (0.5,64,0.5)");
	fprintf(stderr, "(1) status query: failure reported, success restored\n");
}

static void case_autoload_new(void) {
	config_set_string(&gstate.config_user, "autoload", "+new");
	screen_sworld_test_reset_autoload_latch();
	screen_sworld_test_reset(640, 480);
	const size_t rows_before = screen_sworld_test_row_count();

	s_refused = 0;
	s_fail_chunk_ids = true;
	const bool fired = screen_sworld_test_autoload_new_tick("+new", false);
	s_fail_chunk_ids = false;
	check(fired, "(2) the +new sentinel did not fire");
	check(s_refused > 0, "(2) the fault seam was never reached");

	char hint[160];
	(void)screen_sworld_test_hint(hint, sizeof(hint), NULL, NULL, NULL, NULL);
	check(strstr(hint, SPAWN_ERROR_TEXT) != NULL,
		  "(2) no visible spawn-search error on the select-world screen");
	if(!strstr(hint, SPAWN_ERROR_TEXT))
		printf("    status line was: \"%s\"\n", hint);

	check(!path_exists(test_fixture_join(s_saves, "/New World/level.dat")),
		  "(2) a level.dat (with the placeholder spawn) was persisted after "
		  "a failed spawn search");
	check(!path_exists(test_fixture_join(s_saves, "/New World")),
		  "(2) the incomplete world directory was left behind");
	screen_sworld_test_reset(640, 480);
	check(screen_sworld_test_row_count() == rows_before,
		  "(2) a world row appeared after a failed spawn search");
	config_set_string(&gstate.config_user, "autoload", "");
	fprintf(stderr, "(2) +new path checked\n");
}

static void case_dialog(void) {
	s_refused = 0;
	s_fail_chunk_ids = true;
	const bool ok = screen_sworld_create_from_dialog("Failed World",
		"Failed World", true, 12345, false);
	s_fail_chunk_ids = false;
	check(s_refused > 0, "(3) the fault seam was never reached");
	check(!ok, "(3) the create-world dialog reported success after a failed "
			   "spawn search (it would not show its failure line)");
	check(!path_exists(test_fixture_join(s_saves, "/Failed World/level.dat")),
		  "(3) the dialog path persisted a level.dat after a failed search");
	check(!path_exists(test_fixture_join(s_saves, "/Failed World")),
		  "(3) the dialog path left the incomplete directory behind");
	fprintf(stderr, "(3) dialog path checked\n");
}

static void case_control(void) {
	const bool ok = screen_sworld_create_from_dialog("Good World",
		"Good World", true, 12345, false);
	check(ok, "(4) control: an unarmed dialog create failed");
	check(path_exists(test_fixture_join(s_saves, "/Good World/level.dat")),
		  "(4) control: an unarmed dialog create persisted no level.dat");
	fprintf(stderr, "(4) control create checked\n");
}

int main(void) {
	test_fixture_dir("test_spawn_search_failure_tmp", s_saves, sizeof s_saves);
	screen_sworld_test_delete_tree(s_saves);
	MKDIR(s_saves);
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	config_set_string(&gstate.config_user, "paths.worlds", s_saves);
	config_set_string(&gstate.config_user, "debug.superflat", "off");
	config_set_string(&gstate.config_user, "debug.start_nether", "off");
	/* sworld_finish_create's success tail sends SRPC_LOAD_WORLD (control
	 * case); the channel must exist even though nothing drains it. */
	if(!svin_init()) {
		printf("test_spawn_search_failure: setup failed (svin_init)\n");
		return 2;
	}

	case_status_query();
	case_autoload_new();
	case_dialog();
	case_control();

	screen_sworld_test_delete_tree(s_saves);
	test_fixture_dir_cleanup(s_saves);
	if(s_failures) {
		printf("REL-03b RED: %d check(s) failed\n", s_failures);
		return 1;
	}
	printf("REL-03b GREEN: failed spawn search stops world creation\n");
	return 0;
}
