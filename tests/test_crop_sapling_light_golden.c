/* Beta 1.7.3 BlockCrops.java:19-24 and BlockSapling.java:14-15:
 * getBlockLightValue(x,y+1,z) >= 9 gates the sole growth RNG draw.
 * World.java:577 / Chunk.java:337-349 subtract time-of-day sky light.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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
#include "block/blocks_data.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

#define Y 100
static char fixture[128];
static struct server_local s;
static volatile int draining = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}
static void set(int x, int y, int z, int type, int meta) {
	if(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta})) return;
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	assert(b.type == type && b.metadata == meta);
}
static struct block_data at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b;
}
static uint32_t hit_seed(int bound) {
	for(uint32_t seed = 1; seed < 1000000; seed++) {
		struct random_gen trial = {.seed = seed};
		if(rand_gen_int(&trial, bound) == 0) return seed;
	}
	assert(0 && "no deterministic hit seed");
	return 0;
}
static void check(int type, uint64_t time, int torch, int eligible) {
	const int x = type == BLOCK_CROPS ? 2 : 14, z = 4;
	set(x, Y - 1, z, type == BLOCK_CROPS ? BLOCK_FARMLAND : BLOCK_DIRT, 0);
	set(x, Y, z, type, 0);
	set(x + 1, Y + 1, z, torch ? BLOCK_TORCH : BLOCK_AIR, 0);
	for(int i = 0; i < 3; i++) server_world_tick(&s.worlds[0], &s);
	server_world_light_drain(&s.worlds[0]); /* G9: World.tick's light drain */
	struct block_data above = at(x, Y + 1, z);
	assert(above.sky_light == 15);
	if(torch) assert(above.torch_light >= 9);
	s.world_time = time;
	int sky = (int)above.sky_light - server_local_sky_subtract(time);
	int light = above.torch_light > sky ? above.torch_light : sky;
	if((light >= 9) != eligible)
		fprintf(stderr, "light fixture %s time=%llu torch=%d sky=%d block=%d effective=%d\n",
			type == BLOCK_CROPS ? "crop" : "sapling", (unsigned long long)time,
			torch, above.sky_light, above.torch_light, light);
	assert((light >= 9) == eligible);
	/* Dry isolated crop: growthRate=2, bound=50. Sapling bound=30. */
	int bound = type == BLOCK_CROPS ? 50 : 30;
	s.rand_src.seed = hit_seed(bound);
	s.rand_src.has_next_gaussian = false;
	struct random_gen expected = s.rand_src;
	if(eligible) assert(rand_gen_int(&expected, bound) == 0);
	struct block_data b = at(x, Y, z);
	struct block_info info = {.block = &b, .neighbours = NULL, .x = x, .y = Y, .z = z};
	blocks[type]->onRandomTick(&s, &info);
	if(s.rand_src.seed != expected.seed)
		fprintf(stderr, "RED %s %s: expected %d growth draw, RNG advanced unexpectedly\n",
			type == BLOCK_CROPS ? "crop" : "sapling",
			torch ? "midnight torch" : time == 18000 ? "midnight" : "noon", eligible);
	assert(s.rand_src.seed == expected.seed && "Beta growth gate draw count");
	assert(at(x, Y, z).metadata == (eligible ? (type == BLOCK_CROPS ? 1 : 8) : 0));
	printf("%s %s: light=%d, growth=%d, draws=%d PASS\n",
		type == BLOCK_CROPS ? "crop" : "sapling",
		torch ? "midnight torch" : time == 18000 ? "midnight" : "noon",
		light, eligible, eligible);
}
int main(void) {
	test_fixture_dir("test_crop_sapling_light_golden_tmp", fixture, sizeof fixture);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	remove(test_fixture_join(fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	const int types[] = {BLOCK_CROPS, BLOCK_SAPLING};
	for(size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
		int type = types[i];
		check(type, 6000, 0, 1);
		check(type, 18000, 0, 0);
		check(type, 18000, 1, 1);
	}
	draining = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	return 0;
}
