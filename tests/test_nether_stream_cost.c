/*
	P1 NETHER-LAG diagnostic + guard (critique 10-28): walk a single player's
	server column ring through REAL Nether terrain and REAL overworld terrain on
	the production server_world_load_chunk (generate + 3x3 populate) and
	server_world_light_drain paths, timing every call, plus the Beta
	MetadataChunkBlock relax census (server_world_test_beta_light_work).
	MEASUREMENT ONLY for now: it prints per-step cost so the stall is named and
	asserts nothing about time (the fix is not landed; see critique-1028 card
	NETHER-LAG). Seed 12345, radius 5, 12 one-row steps along +x.
*/

#include <assert.h>
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
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

enum { RADIUS = 5, STEPS = 12 };

struct cost {
	uint64_t load_max_us, load_total_us, drain_max_us, drain_total_us;
	uint64_t step_max_us;
};

static void run_dim(const char* sub, enum world_dim dim, int64_t seed,
					struct cost* out) {
	memset(out, 0, sizeof *out);
	char path[256];
	snprintf(path, sizeof path, "%s", test_fixture_join(TMP_WORLD, sub));
	MKDIR(path);
	char region[300];
	snprintf(region, sizeof region, "%s/region", path);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, path);
	memset(&s, 0, sizeof(s));
	/* Single-world harness: slot 0 is the only world (no s.world alias). */
	server_world_create(&s.worlds[0], level, dim, seed, false);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);

	for(int cx = -RADIUS; cx <= RADIUS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	server_world_light_drain(&s.worlds[0]);

	for(int step = 1; step <= STEPS; step++) {
		int cx = step + RADIUS;
		uint64_t step_us = 0;
		for(int cz = -RADIUS; cz <= RADIUS; cz++) {
			struct server_chunk* sc = NULL;
			ptime_t t0 = time_get();
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
			uint64_t us = time_diff_us(t0, time_get());
			step_us += us;
			out->load_total_us += us;
			if(us > out->load_max_us) out->load_max_us = us;
		}
		server_world_test_beta_light_work(NULL, true);
		ptime_t t0 = time_get();
		server_world_light_drain(&s.worlds[0]);
		uint64_t dus = time_diff_us(t0, time_get());
		step_us += dus;
		out->drain_total_us += dus;
		if(dus > out->drain_max_us) out->drain_max_us = dus;
		if(step_us > out->step_max_us) out->step_max_us = step_us;
		uint64_t wk[4];
		server_world_test_beta_light_work(wk, true);
		printf("  %s step %2d: loads %6llu us, drain %6llu us (beta-light "
			   "columns %llu cells %llu probes %llu resolves %llu)\n", sub,
			   step, (unsigned long long)(step_us - dus),
			   (unsigned long long)dus, (unsigned long long)wk[0],
			   (unsigned long long)wk[1], (unsigned long long)wk[2],
			   (unsigned long long)wk[3]);
	}
	printf("  %s: load max %llu us total %llu us | drain max %llu us total "
		   "%llu us | step max %llu us\n",
		   sub, (unsigned long long)out->load_max_us,
		   (unsigned long long)out->load_total_us,
		   (unsigned long long)out->drain_max_us,
		   (unsigned long long)out->drain_total_us,
		   (unsigned long long)out->step_max_us);
	fflush(stdout);
	dict_entity_clear(s.entities);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
}

int main(void) {
	test_fixture_dir("test_nether_stream_cost_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);

	struct cost ow, ne;
	run_dim("ow", WORLD_DIM_OVERWORLD, 12345, &ow);
	run_dim("nether", WORLD_DIM_NETHER, 12345, &ne);

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	puts("test_nether_stream_cost: OK");
	return 0;
}
