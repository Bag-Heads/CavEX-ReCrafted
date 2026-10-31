/*
	test_lever_place_golden -- FAITHFULNESS golden for the floor lever's
	placement orientation (lever-piston-0925).

	Spec (Beta 1.7.3, ../mcb173-retromcp/minecraft/src_original/net/minecraft/src,
	BlockLever.onBlockPlaced:32-58):
	    on the TOP face (1) with a normal cube below,
	        meta = 5 + World.rand.nextInt(2);      // :37-38, World.rand
	    face 2 ... -> meta 4;  ... face 5 -> meta 1;
	A floor lever (clicked the TOP face) is meta 5 or 6 -- the two handle
	rotations, both powering side 1 -- chosen by ONE World.rand.nextInt(2)
	draw. Wall levers take no draw. World.rand is this port's s->rand_src
	(the shared world stream; its generator is a documented deviation, the
	DRAW COUNT and position are what parity requires).

	Pins:
	  1. floor placement draws EXACTLY one rand_gen_int(&rand_src, 2) and the
	     meta is 5 + that draw (state compared against a copy advanced by that
	     one draw);
	  2. both orientations are reachable (5 and 6 each seen across seeds);
	  3. wall placements draw nothing and keep their fixed metas.
	RED on the pre-port lever (always 5, no draw).
*/

#include <assert.h>
#include <stdbool.h>
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
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture[128];
static struct server_local s;
static int s_checks, s_failures;

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

static void check(const char* what, bool ok) {
	s_checks++;
	if(!ok) {
		s_failures++;
		fprintf(stderr, "FAIL %s\n", what);
	}
}

/* An identical write is a Beta no-op that returns false, so verify the cell
 * by reading it back instead of trusting the return value. */
static void set(int x, int y, int z, int type) {
	server_world_set_block(&s.worlds[0], x, y, z,
						   (struct block_data) {.type = (uint8_t)type});
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b)
		   && b.type == (uint8_t)type);
}

static struct block_data g_on;
static int meta_at(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.metadata : -1;
}

/* Place a lever at (x,y,z) by clicking `side` of the block at (ox,oy,oz). */
static bool place(int x, int y, int z, int ox, int oy, int oz,
				  enum side side) {
	assert(server_world_get_block(&s.worlds[0], ox, oy, oz, &g_on));
	struct block_info on = {
		.block = &g_on, .neighbours = NULL, .x = ox, .y = oy, .z = oz};
	struct block_info where = {
		.block = NULL, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct item_data it = {.id = BLOCK_LEVER, .durability = 0, .count = 1};
	return blocks[BLOCK_LEVER]->block_item.onItemPlace(&s, &s.player, &it, &where, &on,
											side);
}

static bool same_rng(const struct random_gen* a, const struct random_gen* b) {
	return memcmp(a, b, sizeof *a) == 0;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_lever_place_golden_tmp", s_fixture,
					 sizeof s_fixture);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	MKDIR(s_fixture);
	MKDIR(test_fixture_join(s_fixture, "/region"));
	remove(test_fixture_join(s_fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_fixture);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	s.player.x = 1000.0;
	s.player.y = 1000.0;
	s.player.z = 1000.0;

	const int yf = 100;
	for(int x = 0; x <= 15; x++)
		for(int z = 0; z <= 15; z++) {
			set(x, yf, z, BLOCK_STONE);
			set(x, yf + 1, z, BLOCK_AIR);
		}

	/* 1 + 2: floor levers, one per cell, across many world-stream states. */
	bool seen5 = false, seen6 = false;
	int placed = 0;
	for(int x = 1; x <= 14 && !(seen5 && seen6 && placed >= 16); x++) {
		for(int z = 1; z <= 14; z++) {
			struct random_gen expect_rng = s.rand_src;
			const int draw = rand_gen_int(&expect_rng, 2);
			const bool ok = place(x, yf + 1, z, x, yf, z, SIDE_TOP);
			check("floor lever placed", ok);
			const int meta = meta_at(x, yf + 1, z);
			check("floor lever meta = 5 + World.rand.nextInt(2)",
				  meta == 5 + draw);
			check("floor placement consumed exactly one nextInt(2) draw",
				  same_rng(&s.rand_src, &expect_rng));
			if(meta == 5)
				seen5 = true;
			if(meta == 6)
				seen6 = true;
			placed++;
		}
	}
	check("orientation 5 reachable", seen5);
	check("orientation 6 reachable", seen6);
	printf("%s floor lever: %d placements, 5 seen=%d, 6 seen=%d, one draw each\n",
		   s_failures ? "RED " : "PASS", placed, seen5, seen6);

	/* 3: wall levers draw nothing. Mount on the stone column at (8,101,8)
	 * after clearing a ring of cells around it. */
	const int fail_wall = s_failures;
	for(int x = 6; x <= 10; x++)
		for(int z = 6; z <= 10; z++)
			set(x, yf + 1, z, BLOCK_AIR);
	set(8, yf + 1, 8, BLOCK_STONE);
	struct {
		int x, z;
		enum side side;
	} walls[4] = {
		{9, 8, SIDE_RIGHT},
		{7, 8, SIDE_LEFT},
		{8, 9, SIDE_FRONT},
		{8, 7, SIDE_BACK},
	};
	for(int i = 0; i < 4; i++) {
		struct random_gen before = s.rand_src;
		const bool ok = place(walls[i].x, yf + 1, walls[i].z, 8, yf + 1, 8,
							  walls[i].side);
		check("wall lever placed", ok);
		const int meta = meta_at(walls[i].x, yf + 1, walls[i].z);
		check("wall lever meta in 1..4", meta >= 1 && meta <= 4);
		check("wall placement draws nothing", same_rng(&s.rand_src, &before));
	}
	printf("%s wall levers: fixed metas, no World.rand draw\n",
		   s_failures == fail_wall ? "PASS" : "RED ");

	g_drain_run = 0;
	thread_join(&drain);
	if(s_failures) {
		fprintf(stderr, "test_lever_place_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_lever_place_golden (%d checks)\n", s_checks);
	return 0;
}
