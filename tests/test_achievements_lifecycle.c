/* B1 module contract; B2 extends this with production lifecycle handlers. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "support/test_fixture_dir.h"
#include "entity/actor_ident.h"
#include "config.h"
#include "game/achievements/achievements.h"

static char s_dir[128];
static struct stats_profile s_reload;

static void make_dir(const char* dir) {
#ifdef _WIN32
	assert(mkdir(dir) == 0);
#else
	assert(mkdir(dir, 0755) == 0);
#endif
}

static void inventory(unsigned slot, uint64_t generation) {
	struct game_event ev = {0};
	ev.kind = GE_INVENTORY_OPENED;
	ev.authority = GE_AUTH_CLIENT;
	ev.who = (struct actor_ident){.slot = slot, .generation = generation};
	game_event_emit(&ev);
}

static struct ach_status status_of(unsigned slot) {
	struct ach_status st;
	assert(achievements_debug_status(slot, &st));
	return st;
}

static void jump(unsigned slot, uint64_t generation) {
	struct game_event ev = {0};
	ev.kind = GE_JUMPED;
	ev.authority = GE_AUTH_CLIENT;
	ev.who = (struct actor_ident){.slot = slot, .generation = generation};
	game_event_emit(&ev);
}

static unsigned jumps(unsigned slot) {
	return stats_counter_total(&achievements_debug_profile(slot)->general[
		stat_general_index(2010)]);
}

static void test_failed_release(void) {
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 10, "player1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(achievements_bind(1, 20, "player2", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	jump(0, 10);
	jump(1, 20);
	jump(1, 20);
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	assert(!achievements_unbind(0, 10));
	assert(status_of(0).dirty);
	assert(!status_of(0).bound && !status_of(0).closing);
	assert(strcmp(status_of(0).profile_id, "player1") == 0);
	assert(status_of(0).last_save_error == STATS_SAVE_ERR_WRITE);
	assert(achievements_bind(0, 11, "player1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_SAVE_PENDING);
	assert(achievements_bind(0, 11, "replacement", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_SAVE_PENDING);
	assert(achievements_unbind(1, 20));
	assert(achievements_bind(1, 21, "player1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_DUPLICATE); /* retained owners still reserve their file */
	assert(jumps(0) == 1);
	jump(0, 10);
	jump(0, 11);
	assert(achievements_flush_all());
	assert(!status_of(0).dirty);
	assert(achievements_bind(0, 11, "player1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(jumps(0) == 1 && jumps(1) == 2);
	jump(0, 10);
	achievements_tick();
	assert(jumps(0) == 1);
	assert(status_of(0).rejected[ACH_REJECT_STALE_GENERATION] == 1);
	puts("PASS failed flush -> unbind -> rebind retains both owners");
}

static void test_every_save_failure(void) {
	for(int slot = 0; slot < 2; slot++) {
		for(int stage = STATS_IO_STAGE_TMP_OPEN;
			stage <= STATS_IO_STAGE_RENAME_MAIN; stage++) {
			char id[17];
			snprintf(id, sizeof(id), "failure%d%d", slot, stage);
			assert(achievements_init(s_dir));
			assert(achievements_bind(slot, 1, id, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
			jump(slot, 1);
			inventory(slot, 1);
			assert(achievements_flush_all()); /* a real previous committed file */
			jump(slot, 1);
			stats_io_fail_at(stage);
			assert(!achievements_unbind(slot, 1));
			assert(stats_io_fail_armed() == STATS_IO_STAGE_NONE);
			assert(jumps(slot) == 2 && status_of(slot).dirty);
			assert(status_of(slot).save_failures == 1);
			assert(achievements_bind(slot, 2, id, ACH_MODE_REMOTE_SERVER)
				== ACH_BIND_SAVE_PENDING);
			assert(achievements_flush_all());
			assert(achievements_bind(slot, 2, id, ACH_MODE_REMOTE_SERVER)
				== ACH_BIND_OK);
			assert(jumps(slot) == 2);
			struct ach_notice notice;
			assert(!achievements_notice_front(slot, &notice));
			assert(!notice.hint_active); /* reload is silent, identity preserved */
			jump(slot, 1); /* old local session cannot credit the remote one */
			jump(slot, 2);
			assert(achievements_unbind(slot, 2));
			assert(achievements_bind(slot, 3, id, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
			const struct stats_pair* pair
				= &achievements_debug_profile(slot)->general[
					stat_general_index(2010)];
			assert(pair->local == 2 && pair->remote == 1);
		}
	}
	puts("PASS all five save failure stages for both profiles; "
		 "columns and silent reload");
}

static void test_failed_reinit_shutdown(void) {
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 100, "teardown1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(achievements_bind(1, 200, "teardown2", ACH_MODE_REMOTE_SERVER)
		== ACH_BIND_OK);
	jump(0, 100);
	jump(1, 200);
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	assert(!achievements_init("a_different_root"));
	assert(achievements_init_status() == ACH_INIT_SAVE_PENDING);
	assert(jumps(0) == 1 && jumps(1) == 1);
	assert(status_of(0).dirty && status_of(0).closing);
	assert(!status_of(1).dirty && status_of(1).closing);
	jump(0, 100);
	achievements_tick();
	assert(jumps(0) == 1);
	assert(status_of(0).rejected[ACH_REJECT_CLOSING] == 1);
	assert(achievements_flush_all());
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 101, "teardown1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(jumps(0) == 1);
	jump(0, 101);
	stats_io_fail_at(STATS_IO_STAGE_FSYNC);
	assert(!achievements_shutdown());
	assert(jumps(0) == 2 && status_of(0).dirty && status_of(0).closing);
	assert(achievements_flush_all());
	assert(achievements_shutdown());
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 102, "teardown1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(achievements_bind(1, 202, "teardown2", ACH_MODE_REMOTE_SERVER)
		== ACH_BIND_OK);
	assert(jumps(0) == 2 && jumps(1) == 1);
	puts("PASS re-init/shutdown refuse dirty-state loss "
		 "and retry against the old root");
}

static void test_two_pending_profiles(void) {
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 1, "pending1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(achievements_bind(1, 2, "pending2", ACH_MODE_REMOTE_SERVER)
		== ACH_BIND_OK);
	jump(0, 1);
	jump(1, 2);
	jump(1, 2);
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	assert(!achievements_flush_all());
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	assert(!achievements_unbind(0, 1));
	assert(status_of(0).save_failures == 2);
	/* Make P2 dirty again; its prior successful flush is independent of P1. */
	jump(1, 2);
	stats_io_fail_at(STATS_IO_STAGE_FSYNC);
	assert(!achievements_unbind(1, 2));
	assert(status_of(0).dirty && status_of(1).dirty);
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	assert(!achievements_flush_all());
	assert(status_of(0).dirty && !status_of(1).dirty);
	assert(jumps(0) == 1 && jumps(1) == 3);
	assert(achievements_bind(1, 3, "pending2", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(jumps(1) == 3);
	assert(achievements_bind(0, 3, "pending1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_SAVE_PENDING);
	assert(achievements_flush_all());
	assert(achievements_bind(0, 3, "pending1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(jumps(0) == 1 && jumps(1) == 3);
	puts("PASS two pending profiles recover independently "
		 "through partial flush failure");
}

static void test_world_cwd(void) {
	char cwd[CW_STATS_PATH_MAX], root[CW_STATS_PATH_MAX], world[CW_STATS_PATH_MAX];
	assert(getcwd(cwd, sizeof(cwd)));
	snprintf(root, sizeof(root), "%s/%s", cwd, s_dir);
	snprintf(world, sizeof(world), "%s/world", s_dir);
	make_dir(world);
	assert(achievements_init(s_dir)); /* intentionally relative startup root */
	assert(achievements_bind(0, 1, "cwd1", ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 2, "cwd2", ACH_MODE_REMOTE_SERVER) == ACH_BIND_OK);
	assert(chdir(world) == 0);
	jump(0, 1);
	jump(1, 2);
	assert(achievements_flush_all());
	make_dir("DIM-1");
	assert(chdir("DIM-1") == 0);
	jump(0, 1);
	assert(achievements_unbind(0, 1));
	assert(achievements_unbind(1, 2));
	assert(achievements_bind(0, 3, "cwd1", ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 4, "cwd2", ACH_MODE_REMOTE_SERVER) == ACH_BIND_OK);
	assert(jumps(0) == 2 && jumps(1) == 1);
	char stats[CW_STATS_PATH_MAX];
	snprintf(stats, sizeof(stats), "%s/stats", root);
	enum stats_load_status load;
	stats_profile_load(stats, "cwd1", &s_reload, &load);
	assert(load == STATS_LOAD_OK
		&& stats_counter_total(&s_reload.general[stat_general_index(2010)]) == 2);
	assert(!test_fixture_is_dir(".", "stats"));
	assert(chdir(cwd) == 0);
	puts("PASS global stats root survives world and DIM-1 "
		 "cwd changes for both owners");
}

static void test_config_keys(void) {
	_Static_assert(CONFIG_PROFILE_ID_CAP == CW_PROFILE_ID_MAX + 1,
		"profile key bounds agree");
	struct config c = {0};
	c.root = json_parse_string("{}");
	assert(c.root);
	char ids[2][CONFIG_PROFILE_ID_CAP];
	assert(config_read_profiles(&c, ids) == CONFIG_PROFILES_OK);
	assert(strcmp(ids[0], "player1") == 0 && strcmp(ids[1], "player2") == 0);
	assert(!json_object_get_value(json_value_get_object(c.root), "profile"));
	assert(config_set_string(&c, "profile.player1", "player2"));
	assert(config_read_profiles(&c, ids) == CONFIG_PROFILES_DUPLICATE);
	assert(config_set_string(&c, "profile.player1", "second"));
	assert(config_read_profiles(&c, ids) == CONFIG_PROFILES_OK);
	assert(strcmp(ids[0], "second") == 0 && strcmp(ids[1], "player2") == 0);
	assert(config_set_string(&c, "profile.player2", "second"));
	assert(config_read_profiles(&c, ids) == CONFIG_PROFILES_DUPLICATE);
	assert(!ids[0][0] && !ids[1][0]);
	assert(config_set_string(&c, "profile.player2", "other"));
	const char* values[] = {"", "Player1", "bad-key", "../player1", "space key",
		"12345678901234567", "valid_123", "1234567890123456"};
	for(unsigned slot = 0; slot < 2; slot++) {
		for(unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
			assert(config_set_string(&c,
				slot ? "profile.player2" : "profile.player1", values[i]));
			assert(config_read_profiles(&c, ids)
				== (stats_profile_id_valid(values[i])
					? CONFIG_PROFILES_OK : CONFIG_PROFILES_BAD_KEY));
		}
		assert(config_set_string(&c, slot ? "profile.player2" : "profile.player1",
			slot ? "second" : "first"));
	}
	char path[200];
	snprintf(path, sizeof(path), "%s/profiles.json", s_dir);
	assert(config_save(&c, path));
	struct config reloaded;
	assert(config_create(&reloaded, path));
	assert(config_read_profiles(&reloaded, ids) == CONFIG_PROFILES_OK);
	assert(strcmp(ids[0], "first") == 0 && strcmp(ids[1], "second") == 0);
	config_destroy(&reloaded);
	assert(config_set_int(&c, "profile.player1", 123));
	assert(config_read_profiles(&c, ids) == CONFIG_PROFILES_BAD_KEY);
	config_destroy(&c);
	c.root = json_parse_string("{\"profile\":false}");
	assert(config_read_profiles(&c, ids) == CONFIG_PROFILES_BAD_KEY);
	config_destroy(&c);
	c.root = json_parse_string("{\"profile\":{\"player1\":null}}");
	assert(config_read_profiles(&c, ids) == CONFIG_PROFILES_BAD_KEY);
	config_destroy(&c);
	assert(config_read_profiles(NULL, ids) == CONFIG_PROFILES_BAD_KEY);
	puts("PASS distinct profile defaults; "
		 "duplicate, invalid and mistyped keys rejected");
}

static void* fail_alloc(size_t bytes) {
	(void)bytes;
	return NULL;
}

static void unused_free(void* block) {
	(void)block;
	assert(0); /* a failed allocation must never reach the destructor */
}

static void test_allocation_failure(void) {
	assert(achievements_shutdown());
	achievements_set_allocator(fail_alloc, unused_free);
	assert(!achievements_init(s_dir));
	assert(achievements_init_status() == ACH_INIT_NO_MEMORY);
	assert(achievements_bind(0, 1, "player1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_BAD_KEY);
	assert(achievements_bind(1, 2, "player2", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_BAD_KEY);
	jump(0, 1);
	jump(1, 2);
	assert(achievements_shutdown());
	achievements_set_allocator(NULL, NULL);
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 1, "player1", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(jumps(0) == 1);
	assert(achievements_bind(1, 2, "player2", (enum ach_bind_mode)42)
		== ACH_BIND_BAD_KEY);
	assert(achievements_bind(1, 2, "player2", ACH_MODE_LOCAL_WORLD)
		== ACH_BIND_OK);
	assert(jumps(1) == 2);
	printf("PASS allocation failure/recovery; PC state block = %zu bytes\n",
		achievements_block_bytes());
}

static void boundary(unsigned kind, unsigned slot, uint64_t epoch, unsigned mode) {
	game_event_emit(&(struct game_event){.kind = kind, .sub = mode,
		.authority = GE_AUTH_CLIENT, .who = {.slot = slot, .generation = epoch}});
}

static void test_queued_boundaries(void) {
	assert(achievements_init(s_dir));
	assert(!achievements_set_profiles("duplicate", "duplicate"));
	boundary(GE_SESSION_BEGIN, 0, 500, ACH_MODE_LOCAL_WORLD);
	achievements_tick(); assert(!status_of(0).bound);
	assert(achievements_set_profiles("queued1", "queued2"));
	boundary(GE_SESSION_BEGIN, 0, 501, ACH_MODE_LOCAL_WORLD);
	boundary(GE_SESSION_BEGIN, 1, 502, ACH_MODE_LOCAL_WORLD);
	/* Admitted facts can arrive before main has loaded the profiles. A
	 * stalled main drain must retain pure counters through both BEGINs. */
	for(unsigned i = 0; i < 26000; i++) {
		jump(0, 501); jump(1, 502);
	}
	achievements_tick();
	assert(jumps(0) == 26000 && jumps(1) == 26000);
	assert(!status_of(0).progress_incomplete && !status_of(1).progress_incomplete);
	boundary(GE_SESSION_END, 0, 501, 0);
	boundary(GE_SESSION_BEGIN, 0, 503, ACH_MODE_REMOTE_SERVER);
	for(unsigned i = 0; i < 26000; i++) { jump(0, 503); jump(1, 502); }
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	/* Make the outgoing epoch dirty before END: already queued witness. */
	achievements_tick();
	/* The initial burst was dirty and END attempted to save it. */
	assert(achievements_lifecycle_pending());
	assert(status_of(0).bound && status_of(0).generation == 503
  && status_of(0).dirty && jumps(0) == 52000);
	assert(achievements_lifecycle_retry());
	assert(status_of(0).bound && status_of(0).generation == 503);
	const struct stats_pair* pair = &achievements_debug_profile(0)->general[
		stat_general_index(2010)];
	assert(pair->local == 26000 && pair->remote == 26000);
	assert(jumps(1) == 52000);
	jump(0, 501); jump(1, 501); achievements_tick();
	assert(jumps(0) == 52000 && jumps(1) == 52000);
	assert(!status_of(0).progress_incomplete && !status_of(1).progress_incomplete);
	boundary(GE_SESSION_END, 0, 503, 0);
	boundary(GE_SESSION_END, 1, 502, 0);
	achievements_tick(); assert(!status_of(0).bound && !status_of(1).bound);
	puts("PASS queued admission/close/rebind and failed-save Retry; 104000 facts exact");
}

static void test_control_recovery(void) {
	assert(achievements_init(s_dir));
	assert(achievements_set_profiles("control1", "control2"));
	boundary(GE_SESSION_BEGIN, 0, 1000, ACH_MODE_LOCAL_WORLD);
	achievements_tick(); jump(0, 1000);
	/* A stalled save cannot fill the control reserve with duplicate saves. */
	for(unsigned i = 0; i < 8192; i++)
		boundary(GE_SESSION_CHECKPOINT, 0, 1000, 0);
	boundary(GE_SESSION_END, 0, 1000, 0);
	achievements_tick();
	assert(!achievements_lifecycle_pending() && !status_of(0).dirty && jumps(0) == 1);
	/* Deliberately OUTSIDE the lifecycle envelope: more than 3072 ownership
	 * boundaries without a drain. The error is durable and Retry retires any
	 * still-current route, rather than leaving shutdown permanently wedged. */
	for(unsigned i = 0; i < 1600; i++) {
		boundary(GE_SESSION_BEGIN, 0, 2000 + i, ACH_MODE_LOCAL_WORLD);
		boundary(GE_SESSION_END, 0, 2000 + i, 0);
	}
	achievements_tick(); assert(!achievements_lifecycle_pending());
	assert(!game_event_owner_epoch(0) && !status_of(0).bound);
	boundary(GE_SESSION_BEGIN, 0, 5000, ACH_MODE_LOCAL_WORLD);
	achievements_tick();
	assert(status_of(0).bound && status_of(0).progress_incomplete && jumps(0) == 1);
	boundary(GE_SESSION_END, 0, 5000, 0); achievements_tick();
	puts("PASS checkpoint deduplication; out-of-envelope control loss flagged on disk and recovery runs during play");
}

static void test_pending_bind_overflow(void) {
	assert(achievements_init(s_dir));
	assert(achievements_set_profiles("burst1", "burst2"));
	boundary(GE_SESSION_BEGIN, 0, 6000, ACH_MODE_LOCAL_WORLD);
	/* Deliberate ordered overflow BEFORE the first profile load. The emit
	 * error must not dirty an empty profile and deadlock BEGIN on bad-key IO. */
	for(unsigned i = 0; i < 3500; i++) inventory(0, 6000);
	achievements_tick();
	assert(status_of(0).bound && status_of(0).progress_incomplete);
	assert(achievements_debug_profile(0)->progress_incomplete);
	boundary(GE_SESSION_END, 0, 6000, 0); achievements_tick();
	assert(!status_of(0).dirty);
	boundary(GE_SESSION_BEGIN, 0, 6001, ACH_MODE_LOCAL_WORLD); achievements_tick();
	assert(status_of(0).bound && status_of(0).progress_incomplete);
	assert(!status_of(0).dirty); /* loaded durable flag, no invented progress */
	boundary(GE_SESSION_END, 0, 6001, 0); achievements_tick();
	puts("PASS out-of-envelope ordered overflow before BEGIN loads; incomplete flag survives reload");
}

int main(void) {
	test_fixture_dir("test_achievements_lifecycle_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);
	test_failed_release();
	test_every_save_failure();
	test_failed_reinit_shutdown();
	test_two_pending_profiles();
	test_world_cwd();
	test_config_keys();
	test_allocation_failure();
	test_queued_boundaries();
	test_control_recovery();
	test_pending_bind_overflow();
	assert(achievements_shutdown());
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_achievements_lifecycle (B2 FIFO + module; handlers in test_achievements_handlers)");
	return 0;
}
