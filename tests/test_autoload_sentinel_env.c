/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* Regression coverage for the 2026-08-17 autoload "+new" sentinel leak fix
 * (docs/betapp_oracle_2026-08-12.md's capture recipe + the 2026-07-30
 * junk-worlds 121/122/123 incident). Drives screen_select_world.c's
 * sworld_autoload_new_tick() in isolation -- the same carve-out reasoning as
 * screen_sworld_test_reset uses (no GL context, no input, no audio) -- so the
 * "does the sentinel fire, and does the gate stay closed when it shouldn't"
 * behaviour is testable without a windowing/audio context. NOT a
 * reimplementation: the test hook is a thin passthrough to the exact
 * function screen_sworld_update calls, so it cannot drift from what ships.
 *
 * Three shapes, matching the fix's own regression requirements:
 *   (a) empty saves + no sentinel -> one reset+update tick -> count stays 0,
 *       no directory is created.
 *   (b) sentinel set (via config, the real Wii path -- the Wii has no
 *       environment) -> a world IS created. This is the capture-rig
 *       convenience the fix must NOT remove: "+new" still has no
 *       existing-save gate, by design, after this fix -- only the SILENT
 *       leak is closed, not the unconditional-create behaviour itself.
 *   (b2) bonus: the "source" the announce fprintf reports (env vs config) is
 *       derived from a REAL getenv, not just a test-supplied bool -- this is
 *       the exact wiring layer one's fprintf depends on.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
static void set_autoload_env(const char* value) {
	// Modern CRT (this project builds with MSYS2 ucrt64 GCC) copies the
	// string into the environment block, so `buf` need not outlive the call
	// -- static anyway, cheap defense against an older/non-copying CRT.
	static char buf[64];
	snprintf(buf, sizeof(buf), "CAVEX_AUTOLOAD=%s", value ? value : "");
	_putenv(buf);
}
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
static void set_autoload_env(const char* value) {
	if(value)
		setenv("CAVEX_AUTOLOAD", value, 1);
	else
		unsetenv("CAVEX_AUTOLOAD");
}
#endif

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "network/server_interface.h"

static char s_fixture_SAVES_ROOT[128]; /* was #define SAVES_ROOT='test_autoload_sentinel_tmp' (a literal) */
#define SAVES_ROOT s_fixture_SAVES_ROOT

static bool path_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static void point_config_at_fixture(void) {
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	config_set_string(&gstate.config_user, "paths.worlds", SAVES_ROOT);
	// Fast, deterministic, allocation-light spawn resolution
	// [worldgen_find_initial_spawns returns immediately for superflat,
	// skipping worldgen_find_spawn's real chunk search] -- exactly the
	// reason every hw_run profile that creates a world uses it too.
	config_set_string(&gstate.config_user, "debug.superflat", "on");
}

/* ---- (a): no sentinel -> nothing happens ------------------------------- */

static void test_no_sentinel_is_a_no_op(void) {
	set_autoload_env(NULL);
	// No "autoload" key at all -- config_read_string's own fallback ("") is
	// what a truly untouched config.json looks like.

	screen_sworld_test_reset_autoload_latch();
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 0
		   && "fixture must start with zero saves");

	// Mirrors screen_sworld_update's own resolution exactly (env, else
	// config), so this is not testing a synthetic stand-in scenario.
	const char* env = getenv("CAVEX_AUTOLOAD");
	bool from_env = env && env[0];
	const char* name
		= from_env ? env : config_read_string(&gstate.config_user, "autoload", "");
	assert(!screen_sworld_test_autoload_new_tick(name, from_env)
		   && "the gate must not fire with no sentinel armed");

	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 0
		   && "the world count must stay 0 -- no autoload means no create");
	assert(!path_exists(test_fixture_join(SAVES_ROOT, "/New World"))
		   && "no directory may appear under the saves path with no sentinel armed");
	fprintf(stderr, "(a) no sentinel: count==0, no directory created -- OK\n");
}

/* ---- (b): sentinel set via config (the real Wii path) -> world created - */

static void test_sentinel_creates_world(void) {
	assert(config_set_string(&gstate.config_user, "autoload", "+new"));
	set_autoload_env(NULL); // env must NOT be the source for this shape

	screen_sworld_test_reset_autoload_latch();
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 0);

	const char* env = getenv("CAVEX_AUTOLOAD");
	bool from_env = env && env[0];
	assert(!from_env && "this shape exercises the CONFIG source, not env");
	const char* name = config_read_string(&gstate.config_user, "autoload", "");
	assert(strcmp(name, "+new") == 0);
	assert(screen_sworld_test_autoload_new_tick(name, from_env)
		   && "the sentinel must still fire unconditionally -- capture "
			  "convenience is preserved, only the silent leak is fixed");

	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 1
		   && "a world must have been created");
	assert(path_exists(test_fixture_join(SAVES_ROOT, "/New World/level.dat"))
		   && "the created world must have persisted a real level.dat, not "
			  "just an in-memory row");

	// Latched: a second tick in the same "boot" must NOT create a second
	// world -- unchanged pre-existing behaviour, just re-confirmed here since
	// the refactor moved autoload_done to file scope.
	const char* name2 = config_read_string(&gstate.config_user, "autoload", "");
	assert(!screen_sworld_test_autoload_new_tick(name2, false)
		   && "autoload_done must latch after the first fire");
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 1
		   && "the latch must prevent a second world from a second tick");

	fprintf(stderr,
			"(b) sentinel set (config source): world created, latched -- OK\n");
}

/* ---- (b2) bonus: the source getenv reports is real, not test-supplied -- */

static void test_source_detection_is_real_getenv(void) {
	// Config still carries "autoload":"+new" from the previous check; giving
	// ENV a value too proves the real precedence (env wins over config)
	// still resolves the way screen_sworld_update's own resolution does --
	// exactly the `from_env` bool this fix's announce fprintf reports on.
	set_autoload_env("+new");
	screen_sworld_test_reset_autoload_latch();

	const char* env = getenv("CAVEX_AUTOLOAD");
	assert(env && strcmp(env, "+new") == 0);
	bool from_env = env && env[0];
	assert(from_env);

	assert(screen_sworld_test_autoload_new_tick(env, from_env)
		   && "env-sourced sentinel must fire exactly like config-sourced");

	set_autoload_env(NULL);
	fprintf(stderr, "(b2) env-sourced sentinel reports from_env=true -- OK\n");
}

int main(void) {
	test_fixture_dir("test_autoload_sentinel_tmp", s_fixture_SAVES_ROOT, sizeof s_fixture_SAVES_ROOT);
	// Defensive pre-clean: sworld_delete_tree (production's own recursive
	// delete, exercised by test_world_manage_env.c) is a safe no-op on a
	// missing path, so this also covers a debris-free first run.
	screen_sworld_test_delete_tree(SAVES_ROOT);
	MKDIR(SAVES_ROOT);
	point_config_at_fixture();
	// svin_init() mirrors main.c's boot sequence: sworld_finish_create
	// (inside screen_sworld_create_new) sends an SRPC_LOAD_WORLD over
	// svin_rpc_send, which needs the channel initialised even though no
	// server thread drains it here -- tchannel_init on PLATFORM_PC is just
	// malloc + mutex/cond init, no thread spawned, so this is safe and cheap
	// in a test process (the queued message is simply abandoned at exit).
	assert(svin_init());

	test_no_sentinel_is_a_no_op();
	test_sentinel_creates_world();
	test_source_detection_is_real_getenv();

	screen_sworld_test_delete_tree(SAVES_ROOT);
	fprintf(stderr, "test_autoload_sentinel_env: all OK\n");
	test_fixture_dir_cleanup(SAVES_ROOT);
	return 0;
}
