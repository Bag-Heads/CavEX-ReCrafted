/* Beta BlockChest.onBlockRemoval (mcb173-retromcp src_original lines
 * 129-157): for every occupied slot, in slot order, THREE nextFloat()*0.8F
 * +0.1F offsets (x, y, z) drawn once and shared by every piece of that slot;
 * then while(stackSize > 0): nextInt(21)+10 capped by the remainder, one
 * EntityItem per piece at (blockX+ox, blockY+oy, blockZ+oz) with motion
 * (nextGaussian*0.05F, nextGaussian*0.05F+0.2F, nextGaussian*0.05F), all on
 * BlockChest's own `private Random random` (never the world rand). The
 * pickup delay stays at the EntityItem default 0. BlockFurnace and
 * BlockDispenser use the identical shape (their own Random each).
 *
 * This golden derives the expected entities from that recipe on the same
 * seeded java.util.Random-exact stream (CaveWii's BlockChest.random) and
 * compares every CRPC_SPAWN_ITEM the break publishes: count, size, position,
 * motion. Closed-window break (block_chest onBreak -> destroyed-container
 * path) and open-window break (spill_active_container) must both match. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "worldgen/java_random.h"

/* CAVEWII_SERVER_LOCAL_TEST seam (server_local.c, next to
 * server_local_test_seed_math_random): pins BlockChest.random. Declared here
 * rather than in server_local.h to keep this task inside its owned files. */
void server_local_test_seed_chest_break_rand(int64_t seed);

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#define CX 8
#define CY 70
#define CZ 8
/* 27 slots x ceil(64/10) = 7 pieces max each -> 189; headroom for safety. */
#define MAX_DROPS 256

static struct server_local s;
static volatile int drain_run = 1;
static struct client_rpc drops[MAX_DROPS];
static int drop_count;
static struct item_data slots[CHEST_SIZE_STORAGE];

static void* drain(void* arg) {
	(void)arg;
	while(drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void capture(const struct client_rpc* rpc, void* user) {
	(void)user;
	if(rpc->type == CRPC_SPAWN_ITEM) {
		assert(drop_count < MAX_DROPS);
		drops[drop_count++] = *rpc;
	}
}

/* A full chest with a few counts that exercise the cap and the <10 case. */
static void fill_slots(void) {
	for(int k = 0; k < CHEST_SIZE_STORAGE; k++)
		slots[k] = (struct item_data) {.id = BLOCK_COBBLESTONE, .count = 64};
	slots[3] = (struct item_data) {.id = ITEM_COAL, .count = 7};	 // one piece, < 10
	slots[9] = (struct item_data) {.id = BLOCK_DIRT, .count = 10}; // exactly the floor
	slots[12] = (struct item_data) {.id = BLOCK_SAND, .count = 31}; // 10-30 then remainder
	slots[20] = (struct item_data) {
		.id = ITEM_IRON_PICKAXE, .count = 1, .durability = 17}; // damage carried
}

static void verify(const char* label) {
	struct java_random expected;
	jrand_set_seed(&expected, 4242);
	int i = 0;
	long total_expected = 0, total_got = 0;
	for(int slot = 0; slot < CHEST_SIZE_STORAGE; slot++) {
		int remain = slots[slot].count;
		if(slots[slot].id == 0 || !remain)
			continue;
		total_expected += remain;
		float ox = jrand_next_float(&expected) * 0.8F + 0.1F;
		float oy = jrand_next_float(&expected) * 0.8F + 0.1F;
		float oz = jrand_next_float(&expected) * 0.8F + 0.1F;
		float x = (float)CX + ox, y = (float)CY + oy, z = (float)CZ + oz;
		while(remain) {
			int take = (int)jrand_next_int_bound(&expected, 21) + 10;
			if(take > remain)
				take = remain;
			remain -= take;
			/* Beta: (float)random.nextGaussian() * 0.05F (cast first). */
			float vx = (float)jrand_next_gaussian(&expected) * 0.05F;
			float vy = (float)jrand_next_gaussian(&expected) * 0.05F + 0.2F;
			float vz = (float)jrand_next_gaussian(&expected) * 0.05F;
			if(i >= drop_count) {
				fprintf(stderr,
						"%s: slot %d piece expected but only %d entities spawned\n",
						label, slot, drop_count);
				assert(i < drop_count);
			}
			const struct client_rpc* rpc = &drops[i++];
			const struct item_data* got = &rpc->payload.spawn_item.item;
			if(got->id != slots[slot].id || got->count != take
			   || got->durability != slots[slot].durability)
				fprintf(stderr,
						"%s: slot %d piece %d got id %u x%u dmg %u, expected id %u "
						"x%d dmg %u\n",
						label, slot, i - 1, got->id, got->count, got->durability,
						slots[slot].id, take, slots[slot].durability);
			assert(got->id == slots[slot].id);
			assert(got->count == take);
			assert(got->durability == slots[slot].durability);
			assert(take >= 1 && take <= 30);
			const float* p = rpc->payload.spawn_item.pos;
			const float* v = rpc->payload.spawn_item.vel;
			if(p[0] != x || p[1] != y || p[2] != z)
				fprintf(stderr,
						"%s: piece %d pos got %.9g %.9g %.9g expected %.9g %.9g %.9g\n",
						label, i - 1, p[0], p[1], p[2], x, y, z);
			assert(p[0] == x && p[1] == y && p[2] == z);
			if(v[0] != vx || v[1] != vy || v[2] != vz)
				fprintf(stderr,
						"%s: piece %d vel got %.9g %.9g %.9g expected %.9g %.9g %.9g\n",
						label, i - 1, v[0], v[1], v[2], vx, vy, vz);
			assert(v[0] == vx && v[1] == vy && v[2] == vz);
			total_got += take;
		}
	}
	if(i != drop_count)
		fprintf(stderr, "%s: %d entities spawned, %d expected\n", label,
				drop_count, i);
	assert(i == drop_count);
	assert(total_got == total_expected);
	/* The point of the parity fix: a full chest never leaves as 27 whole
	 * stacks. 23 x 64 alone needs >= 69 pieces. */
	assert(drop_count > CHEST_SIZE_STORAGE);
}

static void place_chest(void) {
	struct block_data air = {.type = BLOCK_AIR};
	struct block_info where = {.block = &air, .x = CX, .y = CY, .z = CZ};
	struct block_info on = where;
	struct item_data it = {.id = BLOCK_CHEST, .count = 1};
	assert(blocks[BLOCK_CHEST]->block_item.onItemPlace(
		&s, &s.player, &it, &where, &on, SIDE_TOP));
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], CX, CY, CZ, &b));
	assert(b.type == BLOCK_CHEST);
}

static void run(bool open) {
	place_chest();
	int row = server_local_chest_row_w(&s.worlds[0], CX, CY, CZ);
	assert(row >= 0);
	fill_slots();
	memcpy(s.chest_items[row], slots, sizeof(slots));
	s.player.active_inventory = &s.player.inventory;
	if(open)
		assert(server_local_open_inventory(&s, &inventory_logic_chest,
										   CHEST_SIZE, CX, CY, CZ, WINDOWC_CHEST,
										   WINDOW_TYPE_CHEST));
	/* The furnace's furnaceRand must be untouched by a chest break (R2-1). */
	s.container_drop_rng = (struct random_gen) {.seed = 777};
	server_local_test_seed_chest_break_rand(4242);
	drop_count = 0;
	struct block_data block = {.type = BLOCK_CHEST, .metadata = 2};
	struct block_info info = {.block = &block, .x = CX, .y = CY, .z = CZ};
	assert(server_world_set_block(&s.worlds[0], CX, CY, CZ,
								  (struct block_data) {.type = BLOCK_AIR}));
	block_chest.onBreak(&s, &info);
	assert(server_local_chest_row_w(&s.worlds[0], CX, CY, CZ) < 0);
	verify(open ? "open window" : "closed window");
	assert(s.container_drop_rng.seed == 777);
	if(open) {
		struct inventory* inv = s.player.active_inventory;
		assert(inv != &s.player.inventory);
		assert(inv->invalidated);
		for(int k = 0; k < CHEST_SIZE_STORAGE; k++)
			assert(inv->items[CHEST_SLOT_STORAGE + k].count == 0);
		inventory_destroy_owned(inv, &s.player);
		s.player.active_inventory = &s.player.inventory;
	}
}

int main(void) {
	char dir[128];
	test_fixture_dir("test_chest_break_scatter_tmp", dir, sizeof dir);
	assert(clin_init());
	blocks_init();
	items_init();
	MKDIR(dir);
	MKDIR(test_fixture_join(dir, "/region"));
	string_t level;
	string_init_set_str(level, dir);
	memset(&s, 0, sizeof s);
	if(!server_local_te_tables_init(&s))
		abort();
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	dict_entity_init(s.entities);
	s.player.has_pos = true;
	s.player.x = CX + 0.5F;
	s.player.y = CY + 1.62F;
	s.player.z = CZ - 2.0F;
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	struct thread thread;
	assert(thread_create(&thread, drain, NULL, 64));
	clin_test_set_rpc_observer(capture, NULL);
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &chunk));
	run(false);
	run(true);
	clin_test_set_rpc_observer(NULL, NULL);
	drain_run = 0;
	thread_join(&thread);
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(s.entities);
	server_world_discard(&s.worlds[0]);
	inventory_destroy(&s.player.inventory, &s.player);
	string_clear(level);
	test_fixture_dir_cleanup(dir);
	puts("PASS chest break scatter: closed and open windows, exact seeded "
		 "10-30 pieces (BlockChest.onBlockRemoval)");
	return 0;
}
