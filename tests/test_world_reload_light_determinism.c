/*
	world-reload-1030 (Bucket A determinism): a world built, destroyed and
	built AGAIN in the same process must settle to the byte-identical blocks
	and light it settled to the first time, also with another dimension built
	in between -- no light/worldgen/population/queue state may survive
	server_world_destroy and change gameplay light (mob spawn, crop growth).

	Root cause of the nether-drain-1030 "second world settles to a different
	fixpoint" finding: NOT light state. World.rand is `new Random()`
	(World.java:37), seeded from the JDK's process-global seedUniquifier ^
	nanoTime (server_world_runtime_jrand_seed). Population lava springs flow
	under scheduledUpdatesAreImmediate with World.rand (BlockFlowing lava
	`rand.nextInt(4)`), so a second World instance gets a different stream,
	different flowing-lava cells, and the light that follows from them. That
	is faithful Beta (one JVM: every new World draws the next uniquifier; and
	on the console the clock half differs every boot anyway), so it is kept.
	This test pins each build's World.rand to the same seed and then requires
	byte-identical blocks AND light: anything else surviving a world unload
	fails here.
	Proof: seed 12345, a radius-5 ring plus 6 streamed rows along +x drained
	after every row, built three times in one process (overworld, Nether,
	overworld); two 64-bit FNVs (block id+metadata, sky+torch nibbles) over
	the whole ring, equal between the two overworld builds; plus a control
	that an UNPINNED rebuild really does differ (the pin is what is load-
	bearing, so the test is live).
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

enum { RADIUS = 5, STEPS = 6 };

struct result {
	uint64_t blocks, light;
	size_t lit_cells;
};

static uint64_t fnv(uint64_t h, uint8_t b) {
	return (h ^ b) * 1099511628211ULL;
}

static void build(const char* sub, enum world_dim dim, bool pin_rand,
				  struct result* out) {
	memset(out, 0, sizeof *out);
	/* a fresh folder per build: every build GENERATES its ring */
	static unsigned run;
	char name[64];
	snprintf(name, sizeof name, "%s-%u", sub, run++);
	char path[256];
	snprintf(path, sizeof path, "%s", test_fixture_join(TMP_WORLD, name));
	MKDIR(path);
	char region[300];
	snprintf(region, sizeof region, "%s/region", path);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, path);
	memset(&s, 0, sizeof(s));
	/* World.rand = new Random(): pin it so every build draws the same stream */
	if(pin_rand) server_world_test_seed_runtime_jrand(&s.worlds[0].rand, INT64_C(0x5EED1030));
	/* Single-world harness: slot 0 is the only world (no s.world alias). */
	server_world_create(&s.worlds[0], level, dim, 12345, false);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);

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

	uint64_t hb = 1469598103934665603ULL, hl = hb;
	for(int cx = -RADIUS; cx <= RADIUS + STEPS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++)
			for(int lx = 0; lx < CHUNK_SIZE; lx++)
				for(int lz = 0; lz < CHUNK_SIZE; lz++)
					for(int y = 0; y < WORLD_HEIGHT; y++) {
						struct block_data b = {0};
						assert(server_world_get_block(&s.worlds[0], cx * CHUNK_SIZE + lx,
							y, cz * CHUNK_SIZE + lz, &b));
						hb = fnv(hb, b.type);
						hb = fnv(hb, b.metadata);
						hl = fnv(hl, (uint8_t)(b.sky_light | b.torch_light << 4));
						if(b.torch_light) out->lit_cells++;
					}
	out->blocks = hb;
	out->light = hl;
	printf("  %-8s blocks %016llx light %016llx lit-cells %zu\n", name,
		   (unsigned long long)hb, (unsigned long long)hl, out->lit_cells);
	fflush(stdout);
	dict_entity_clear(s.entities);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
}

int main(void) {
	test_fixture_dir("test_world_reload_light_determinism_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);

	struct result a, n, b, u;
	build("ow", WORLD_DIM_OVERWORLD, true, &a);
	build("nether", WORLD_DIM_NETHER, true, &n);
	build("ow", WORLD_DIM_OVERWORLD, true, &b);
	assert(a.lit_cells > 0 && "the ring holds block light (the hash is live)");
	assert(n.lit_cells > 0 && "the Nether ring holds block light");
	assert(a.blocks == b.blocks && "rebuilt world: byte-identical blocks");
	assert(a.light == b.light && "rebuilt world: byte-identical light");
	/* control: an unpinned World.rand (a fresh `new Random()`) moves the
	 * population lava flow, so the pin above is what makes a == b */
	build("ow-free", WORLD_DIM_OVERWORLD, false, &u);
	assert(u.blocks != a.blocks && "World.rand reaches the ring (control)");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	puts("test_world_reload_light_determinism: OK");
	return 0;
}
