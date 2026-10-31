/* Beta 1.7.3 placement: BlockFence.canPlaceBlockAt (8-9),
 * BlockTorch.func_31032_h/canPlaceBlockAt/onNeighborBlockChange (27-38, 104),
 * BlockTrapDoor.canPlaceBlockOnSide (154-176). */
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

#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char fixture[128];
static struct server_local s;
static volatile int drain_run = 1;
static int failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(drain_run) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}
static void set(int x, int y, int z, int type) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data){.type = (uint8_t)type}));
}
static struct block_data read_block(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b;
}
static bool place(const struct block* b, int id, int x, int y, int z,
		int ax, int ay, int az, enum side side) {
	struct block_data anchor = read_block(ax, ay, az);
	struct block_info on = {.block = &anchor, .x = ax, .y = ay, .z = az};
	struct block_info where = {.x = x, .y = y, .z = z};
	struct item_data item = {.id = id, .count = 1};
	return b->block_item.onItemPlace(&s, &s.player, &item, &where, &on, side);
}
static void check(const char* name, bool actual, bool expected) {
	if(actual != expected) { fprintf(stderr, "RED %s: got %d, want %d\n", name, actual, expected); failures++; }
	else printf("GREEN %s\n", name);
}
static void check_int(const char* name, int actual, int expected) {
	if(actual != expected) { fprintf(stderr, "RED %s: got %d, want %d\n", name, actual, expected); failures++; }
	else printf("GREEN %s\n", name);
}

int main(void) {
	test_fixture_dir("test_place_fence_torch_trapdoor_tmp", fixture, sizeof fixture);
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
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &chunk) && chunk);
	s.player.x = s.player.y = s.player.z = 1000.0;

	/* Fence below: air refuses; sand, glass, stone and fence are solid/allowed. */
	const int supports[] = {BLOCK_AIR, BLOCK_SAND, BLOCK_GLASS, BLOCK_STONE, BLOCK_FENCE};
	const char* names[] = {"fence/air", "fence/sand", "fence/glass", "fence/stone", "fence/fence"};
	for(int i = 0; i < 5; i++) {
		int x = 2 + i;
		if(i != 0) set(x, 100, 2, supports[i]);
		bool ok = place(&block_fence, BLOCK_FENCE, x, 101, 2, x, 100, 2, SIDE_TOP);
		check(names[i], ok, i != 0);
		check("fence/cell", read_block(x, 101, 2).type == BLOCK_FENCE, i != 0);
	}

	/* A floor torch on a fence stays through an unrelated neighbour update,
	 * then pops when its own fence is removed. Wall attachment stays cube-only. */
	set(3, 100, 5, BLOCK_FENCE);
	bool torch = place(&block_torch, BLOCK_TORCH, 3, 101, 5, 3, 100, 5, SIDE_TOP);
	check("torch/fence-top", torch, true);
	check_int("torch/meta-5", read_block(3, 101, 5).metadata, 5);
	set(4, 101, 5, BLOCK_STONE);
	check("torch/stays-on-fence", read_block(3, 101, 5).type == BLOCK_TORCH, true);
	set(3, 100, 5, BLOCK_AIR);
	check("torch/pops-with-fence", read_block(3, 101, 5).type == BLOCK_AIR, true);
	set(8, 101, 5, BLOCK_FENCE);
	check("torch/fence-wall-refused", place(&block_torch, BLOCK_TORCH, 7, 101, 5,
		8, 101, 5, SIDE_LEFT), false);

	set(10, 100, 8, BLOCK_STONE);
	check("trapdoor/top-refused", place(&block_trapdoor, BLOCK_TRAP_DOOR,
		10, 101, 8, 10, 100, 8, SIDE_TOP), false);
	set(11, 102, 8, BLOCK_STONE);
	check("trapdoor/bottom-refused", place(&block_trapdoor, BLOCK_TRAP_DOOR,
		11, 101, 8, 11, 102, 8, SIDE_BOTTOM), false);
	set(12, 101, 8, BLOCK_GLASS);
	check("trapdoor/glass-side-refused", place(&block_trapdoor, BLOCK_TRAP_DOOR,
		12, 101, 7, 12, 101, 8, SIDE_FRONT), false);
	set(14, 101, 8, BLOCK_STONE);
	check("trapdoor/stone-side", place(&block_trapdoor, BLOCK_TRAP_DOOR,
		14, 101, 7, 14, 101, 8, SIDE_FRONT), true);
	check("trapdoor/cell", read_block(14, 101, 7).type == BLOCK_TRAP_DOOR, true);

	drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	printf("test_place_fence_torch_trapdoor: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
