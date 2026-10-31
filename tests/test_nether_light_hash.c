/*
	nether-drain-1030 (T1 identity proof): the Beta region-queue light drain
	(server_beta_light.inc) must settle a streamed Nether column ring to the
	SAME light arrays whether it runs the frozen pre-T1 reference relax
	(server_world_test_beta_light_reference(true): server_beta_light_near +
	per-cell get_block reads) or the shipping relax (chunk-neighbourhood memo,
	resident-known schedule, paged merge window). Seed 12345, radius 5, then
	12 one-row steps along +x, draining after every row -- the stream the
	test_nether_stream_cost repro measures. Both dimensions; the Nether is
	the one whose glowstone/fire population makes the drain 10-15x heavier.
	Proof: a 64-bit FNV over every cell's id, metadata, sky and torch nibble
	of the whole ring, plus the queue's logical peak and the relax census
	(columns entered, cells relaxed), equal between the two relaxes.
	Both runs share one process: each pins its World.rand (`new Random()`,
	whose JDK seedUniquifier advances per World and moves population lava
	flow) to the same seed -- see test_world_reload_light_determinism.
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

struct result {
	uint64_t hash;
	uint64_t columns, cells;
	size_t queue_peak;
	size_t lit_cells; /* cells with a non-zero torch nibble: the hash is live */
};

static uint64_t fnv(uint64_t h, uint8_t b) {
	return (h ^ b) * 1099511628211ULL;
}

static void run_dim(const char* sub, enum world_dim dim, int64_t seed,
					bool reference, struct result* out) {
	memset(out, 0, sizeof *out);
	/* its own fresh folder per run: the second run must GENERATE the ring
	 * like the first, never reload the first run's saved chunks */
	static unsigned run;
	char name[64];
	snprintf(name, sizeof name, "%s-%u-%s", sub, run++, reference ? "ref" : "new");
	char path[256];
	snprintf(path, sizeof path, "%s", test_fixture_join(TMP_WORLD, name));
	MKDIR(path);
	char region[300];
	snprintf(region, sizeof region, "%s/region", path);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, path);
	memset(&s, 0, sizeof(s));
	/* World.rand = new Random(): pin it so both relaxes see the same stream */
	server_world_test_seed_runtime_jrand(&s.worlds[0].rand, INT64_C(0x5EED1030));
	/* Single-world harness: slot 0 is the only world (no s.world alias). */
	server_world_create(&s.worlds[0], level, dim, seed, false);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);
	server_world_test_beta_light_reference(reference);
	server_world_test_beta_light_work(NULL, true);

	for(int cx = -RADIUS; cx <= RADIUS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	assert(server_world_light_drain(&s.worlds[0]));
	for(int step = 1; step <= STEPS; step++) {
		const int cx = step + RADIUS;
		for(int cz = -RADIUS; cz <= RADIUS; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
		assert(server_world_light_drain(&s.worlds[0]));
	}
	assert(s.worlds[0].generation_light_n == 0 && "the drain settled the queue");

	uint64_t wk[4];
	server_world_test_beta_light_work(wk, true);
	out->columns = wk[0];
	out->cells = wk[1];
	out->queue_peak = s.worlds[0].generation_light_stat[2];
	uint64_t h = 1469598103934665603ULL;
	for(int cx = -RADIUS; cx <= RADIUS + STEPS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++)
			for(int lx = 0; lx < CHUNK_SIZE; lx++)
				for(int lz = 0; lz < CHUNK_SIZE; lz++)
					for(int y = 0; y < WORLD_HEIGHT; y++) {
						struct block_data b = {0};
						assert(server_world_get_block(&s.worlds[0], cx * CHUNK_SIZE + lx,
							y, cz * CHUNK_SIZE + lz, &b));
						h = fnv(h, b.type);
						h = fnv(h, b.metadata);
						h = fnv(h, (uint8_t)(b.sky_light | b.torch_light << 4));
						if(b.torch_light) out->lit_cells++;
					}
	out->hash = h;
	printf("  %s %-9s: hash %016llx columns %llu cells %llu queue-peak %zu "
		   "lit-cells %zu\n", sub, reference ? "reference" : "shipping",
		   (unsigned long long)h, (unsigned long long)out->columns,
		   (unsigned long long)out->cells, out->queue_peak, out->lit_cells);
	fflush(stdout);
	server_world_test_beta_light_reference(false);
	dict_entity_clear(s.entities);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
}

static void check_dim(const char* sub, enum world_dim dim) {
	struct result ref, now;
	run_dim(sub, dim, 12345, true, &ref);
	run_dim(sub, dim, 12345, false, &now);
	assert(ref.lit_cells > 0 && "the ring holds block light (the hash is live)");
	assert(ref.cells > 0 && "the drain relaxed cells");
	assert(ref.columns == now.columns && "same columns entered");
	assert(ref.cells == now.cells && "same cells relaxed");
	assert(ref.queue_peak == now.queue_peak && "same queue depth");
	assert(ref.hash == now.hash && "byte-identical light arrays");
	printf("  %s: reference == shipping (%016llx)\n", sub,
		   (unsigned long long)ref.hash);
}

int main(void) {
	test_fixture_dir("test_nether_light_hash_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	check_dim("nether", WORLD_DIM_NETHER);
	check_dim("ow", WORLD_DIM_OVERWORLD);
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	puts("test_nether_light_hash: OK");
	return 0;
}
