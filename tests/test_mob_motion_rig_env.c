/* test_mob_motion_rig_env -- the mob_motion_2m fixture end to end on PC (lane C,
 * mob motion units S2 proof). The production rig (server_local.c
 * server_local_mob_motion_rig_build/_drive) is armed exactly as the hw_run
 * profile arms it ("debug.mob_motion_rig" -> s->mob_motion_rig, built through
 * the deferred demo path) and driven by complete production server updates;
 * the heartbeat's evidence take must then report a terminal PASS with all three
 * checks (TNT lift, Beta dry recurrence, path step jump). This is the PC twin
 * of the real-Wii run: same code, same verdict, read the same way. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/mob_motion_probe.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static int s_drain = 1;

static void* drain_entry(void* unused) {
	(void)unused;
	while(__atomic_load_n(&s_drain, __ATOMIC_SEQ_CST)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	char path[160], region[180];
	test_fixture_dir("test_mob_motion_rig_env", path, sizeof path);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	static struct server_local s;
	server_local_test_create_without_thread(&s, false, false, false, false,
		false, true, false);
	MKDIR(path);
	snprintf(region, sizeof region, "%s/region", path);
	MKDIR(region);
	string_t name;
	string_init_set_str(name, path);
	struct level_archive level;
	assert(level_archive_create_new(&level, name, 12345,
		(vec3) {8.5f, 6.0f, 8.5f}));
	level_archive_destroy(&level);
	assert(server_local_load_world_impl(&s, name));
	assert(s.player.has_pos);
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;

	/* Nothing published before the rig is armed: the heartbeat omits mm_*. */
	struct server_local_mob_motion_evidence e = {0};
	assert(!server_local_mob_motion_evidence_take(&e) && e.valid == 0);

	/* Let the near chunks stream in, then arm the rig the way main.c does. */
	for(int i = 0; i < 60; i++)
		server_local_test_update_once(&s);
	s.mob_motion_rig = true;
	s.piston_demo_defer = 1;
	server_local_test_update_once(&s);
	assert(server_local_test_mob_motion_rig_tick() > 0);
	assert(server_local_mob_motion_evidence_take(&e));
	assert(e.st == MM_PROBE_PENDING && e.bits == 0 && e.recur_ok == 0);

	int guard = 0;
	while(server_local_test_mob_motion_rig_tick() > 0 && guard++ < 2000)
		server_local_test_update_once(&s);
	assert(server_local_test_mob_motion_rig_tick() == -1);
	assert(server_local_mob_motion_evidence_take(&e));
	printf("mob_motion rig verdict: st=%u bits=%u recur ok=%u bad=%u "
		   "rise=%u mblocks step=%u mblocks tick=%u\n",
		   (unsigned)e.st, (unsigned)e.bits, (unsigned)e.recur_ok,
		   (unsigned)e.recur_bad, (unsigned)e.rise_milli,
		   (unsigned)e.step_milli, (unsigned)e.tick);
	assert(e.st == MM_PROBE_PASS);
	assert(e.bits == MM_PROBE_BITS_ALL);
	assert(e.recur_bad == 0 && e.recur_ok >= MM_RECUR_MIN);
	assert(e.rise_milli >= 500);
	assert(e.step_milli == 1000);

	/* World release resets the fixture and withdraws the evidence. */
	server_local_unload_world_impl(&s, true);
	assert(!server_local_mob_motion_evidence_take(&e) && e.valid == 0);
	assert(server_local_test_mob_motion_rig_tick() == 0);
	string_clear(name);
	__atomic_store_n(&s_drain, 0, __ATOMIC_SEQ_CST);
	thread_join(&drain);
	puts("PASS mob_motion_2m fixture: TNT lift, Beta dry recurrence and path "
		 "step jump through production server updates");
	return 0;
}
