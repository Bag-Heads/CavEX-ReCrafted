/* Beta 1.7.3: BlockMushroom.java:13-23, BlockSapling.java:12-49,
 * BlockCactus.java:11-27, BlockReed.java:14-29. Pin draw order and the
 * distinction between random ticks and neighbour changes. */
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

static struct server_local s;
static char fixture[128];
static volatile int draining = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}
static void set(int x, int y, int z, int type, int meta) {
	server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta});
}
static struct block_data at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b;
}
static void tick(int type, int x, int y, int z) {
	struct block_data b = at(x, y, z);
	assert(b.type == type);
	struct block_info info = {.block = &b, .x = x, .y = y, .z = z};
	blocks[type]->onRandomTick(&s, &info);
}
static void neighbour(int type, int x, int y, int z) {
	struct block_data b = at(x, y, z);
	assert(b.type == type);
	struct block_info info = {.block = &b, .x = x, .y = y, .z = z};
	blocks[type]->onNeighbourBlockChange(&s, &info);
}
static uint32_t mushroom_seed(int* dx, int* dz) {
	for(uint32_t seed = 1; seed < 1000000; seed++) {
		struct random_gen r = {.seed = seed};
		if(rand_gen_int(&r, 100)) continue;
		int x = rand_gen_int(&r, 3) - 1;
		int y = rand_gen_int(&r, 2) - rand_gen_int(&r, 2);
		int z = rand_gen_int(&r, 3) - 1;
		if(y == 0 && (x || z)) { *dx = x; *dz = z; return seed; }
	}
	assert(0);
	return 0;
}
static void mushroom(int type, int x, int z) {
	int dx, dz;
	uint32_t seed = mushroom_seed(&dx, &dz);
	set(x, 99, z, BLOCK_STONE, 0);
	set(x, 100, z, type, 0);
	assert(at(x + dx, 100, z + dz).type == BLOCK_AIR);
	assert(at(x + dx, 99, z + dz).type == BLOCK_STONE);
	struct block_data dest = at(x + dx, 100, z + dz);
	assert(dest.sky_light < 13 && dest.torch_light < 13);
	s.rand_src.seed = seed;
	s.rand_src.has_next_gaussian = false;
	struct random_gen expected = s.rand_src;
	assert(rand_gen_int(&expected, 100) == 0);
	(void)rand_gen_int(&expected, 3);
	(void)rand_gen_int(&expected, 2);
	(void)rand_gen_int(&expected, 2);
	(void)rand_gen_int(&expected, 3);
	(void)rand_gen_int(&expected, 3); /* two dead stores in Beta source */
	(void)rand_gen_int(&expected, 3);
	tick(type, x, 100, z);
	assert(at(x + dx, 100, z + dz).type == type);
	if(s.rand_src.seed != expected.seed)
		fprintf(stderr, "RED mushroom %d: missing two post-check nextInt(3) draws\n", type);
	assert(s.rand_src.seed == expected.seed);
	printf("GREEN mushroom %d: seven draws including two post-check draws\n", type);
}
static void sapling(void) {
	const int x = 18, y = 100, z = 18;
	set(x, y - 1, z, BLOCK_STONE, 0); /* invalid soil, bright sky */
	set(x, y, z, BLOCK_SAPLING, 0);
	set(x + 3, y - 1, z, BLOCK_STONE, 0);
	set(x + 3, y, z, BLOCK_SAPLING, 0);
	assert(at(x, y, z).type == BLOCK_SAPLING);
	assert(server_local_block_light_value(&s, x, y + 1, z) >= 9);
	s.rand_src.seed = 1234567;
	tick(BLOCK_SAPLING, x, y, z);
	assert(at(x, y, z).type == BLOCK_AIR);
	uint32_t actual = s.rand_src.seed;
	s.rand_src.seed = 1234567;
	neighbour(BLOCK_SAPLING, x + 3, y, z); /* same removal draws */
	struct random_gen expected = s.rand_src;
	(void)rand_gen_int(&expected, 30);
	if(actual != expected.seed)
		fprintf(stderr, "RED sapling: missing post-removal nextInt(30) draw\n");
	assert(actual == expected.seed);
	puts("GREEN sapling: removal followed by one nextInt(30) draw");
}
static void invalid_growth(int type, int x) {
	const int y = 100, z = 18;
	set(x, y - 1, z, BLOCK_STONE, 0); /* invalid for either plant */
	set(x, y, z, type, 0);
	assert(at(x, y, z).type == type);
	tick(type, x, y, z);
	if(at(x, y, z).type != type)
		fprintf(stderr, "RED %s: random tick removed invalid block\n",
			type == BLOCK_CACTUS ? "cactus" : "reed");
	assert(at(x, y, z).type == type);
	neighbour(type, x, y, z);
	assert(at(x, y, z).type == BLOCK_AIR);
	printf("GREEN %s: random tick retains; neighbour change removes\n",
		type == BLOCK_CACTUS ? "cactus" : "reed");
}
int main(int argc, char** argv) {
	test_fixture_dir("test_growth_misc_golden_tmp", fixture, sizeof fixture);
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
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);
	struct server_chunk* chunk = NULL;
	for(int cz = 0; cz <= 1; cz++) for(int cx = 0; cx <= 1; cx++)
		assert(server_world_load_chunk(&s.worlds[0], cx, cz, &chunk) && chunk);
	for(int z = 3; z <= 13; z++) for(int x = 3; x <= 13; x++) {
		set(x, 99, z, BLOCK_STONE, 0);
		set(x, 102, z, BLOCK_STONE, 0);
	}
	for(int i = 0; i < 4; i++) server_world_tick(&s.worlds[0], &s);
	if(argc == 1 || strcmp(argv[1], "brown") == 0)
		mushroom(BLOCK_BROWM_MUSHROOM, 7, 8);
	if(argc == 1 || strcmp(argv[1], "red") == 0)
		mushroom(BLOCK_RED_MUSHROOM, 10, 8);
	if(argc == 1 || strcmp(argv[1], "sapling") == 0)
		sapling();
	if(argc == 1 || strcmp(argv[1], "cactus") == 0)
		invalid_growth(BLOCK_CACTUS, 21);
	if(argc == 1 || strcmp(argv[1], "reed") == 0)
		invalid_growth(BLOCK_REED, 25);
	draining = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	return 0;
}
