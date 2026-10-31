/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_world_tick_order_golden -- G10 (KANBAN "random ticks run before
 * scheduled ticks; Beta runs TickUpdates first"). Golden-first.
 *
 * Spec (CFR server decompile, /c/tmp/mcb173-server-decompile/out/net/
 * minecraft/src/World.java):
 *   World.tick:1372-1374   setWorldTime(time + 1); TickUpdates(false);
 *                          doRandomUpdateTicks();
 * Both passes draw from the one world Random (BlockFire.updateTick's
 * `var5.nextInt(3)` age bump [BlockFire.java updateTick, var5 = world.rand];
 * the random pass's own picks), and a random pick hits whatever the
 * scheduled pass left in the world. So in one tick: the scheduled callbacks
 * consume their draws FIRST, and the random picks come from the stream after
 * them.
 *
 * Fixture: 3x3 resident chunks, every random-tickable block cleared, one
 * fire on a stone pillar (its only scheduled tick is due this tick; with no
 * fuel around it draws exactly one nextInt(3) and re-arms), and one GLOWING
 * redstone ore (74) placed at the cell Beta's order makes the random pass hit
 * first. BlockRedstoneOre.updateTick reverts 74 -> 73 drawing nothing
 * [BlockRedstoneOre.java:41-46]. Expected (Beta order): the ore is 73 after
 * the tick and the world stream equals S0 advanced by nextInt(3) then every
 * pick. The random-first order picks from S0 and misses the ore.
 *
 * The expected values come from the Beta order modelled on a COPY of the
 * stream (the pick arithmetic is server_world_random_tick's
 * rand_gen_range x, z, y -- this port's sampler, a recorded deviation from
 * Beta's distHashCounter that this golden does not touch). */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
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
#include "block/blocks_data.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h"
#include "util.h"

#define SEED_S0 0x7E10A0D1u
#define FIRE_X 8
#define FIRE_Y 100
#define FIRE_Z 8

static struct server_local s;
static char s_dir[160];

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static void set(int x, int y, int z, int type) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {.type = (uint8_t)type}));
}

static int get_type(int x, int y, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b.type;
}

static void fixture_open(void) {
	test_fixture_dir("test_world_tick_order_golden", s_dir, sizeof s_dir);
	MKDIR(s_dir);
	MKDIR(test_fixture_join(s_dir, "/region"));
	remove(test_fixture_join(s_dir, "/region/r.0.0.mcr"));
	remove(test_fixture_join(s_dir, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(s_dir, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(s_dir, "/region/r.-1.-1.mcr"));

	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	dict_entity_init(s.entities);
	string_t level;
	string_init_set_str(level, s_dir);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	string_clear(level);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = 1;

	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	/* No random-tickable block anywhere in range: the only random-pass
	 * effect is the one this golden plants. */
	int cleared = 0;
	for(int x = -16; x < 32; x++)
		for(int z = -16; z < 32; z++)
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				int t = get_type(x, y, z);
				if(t && blocks[t] && blocks[t]->onRandomTick) {
					set(x, y, z, BLOCK_STONE);
					cleared++;
				}
			}
	printf("  fixture: cleared %d random-tickable cells\n", cleared);
}

static void fixture_close(void) {
	dict_entity_clear(s.entities);
	server_world_discard(&s.worlds[0]);
	test_fixture_dir_cleanup(s_dir);
}

struct pick {
	int x, y, z;
};

static int s_fail;
static void check(const char* id, bool ok, const char* what) {
	printf("  [%s] %s: %s\n", id, ok ? "PASS" : "FAIL", what);
	if(!ok)
		s_fail++;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	printf("G10: World.tick TickUpdates before doRandomUpdateTicks\n");
	fixture_open();
	struct server_world* w = &s.worlds[0];

	set(FIRE_X, FIRE_Y - 1, FIRE_Z, BLOCK_STONE);
	set(FIRE_X, FIRE_Y, FIRE_Z, BLOCK_FIRE);
	assert(get_type(FIRE_X, FIRE_Y, FIRE_Z) == BLOCK_FIRE);
	/* BlockFire.onBlockAdded scheduled it tickRate 40 after placement. */
	printf("  scheduled entries before the tick: %zu (current_tick %llu)\n",
		   w->scheduled_tick_count, (unsigned long long)w->current_tick);
	assert(w->scheduled_tick_count == 1);
	w->current_tick += 39; /* the next server_world_tick reaches 40 */

	/* The chunk the random pass visits first: the production pass walks
	 * w->chunks in dict order (server_world_random_tick). */
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, w->chunks);
	assert(!dict_server_chunks_end_p(it));
	const int64_t first_id = dict_server_chunks_ref(it)->key;
	size_t chunks_in_range = 0;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it))
		chunks_in_range++;
	assert(chunks_in_range == 9);

	/* Beta-order model on a copy of S0. */
	struct random_gen model = {.seed = SEED_S0};
	(void)rand_gen_int(&model, 3); /* BlockFire.updateTick age bump */
	struct pick beta_pick;
	beta_pick.x = (int)S_CHUNK_X(first_id) * CHUNK_SIZE
		+ rand_gen_range(&model, 0, CHUNK_SIZE);
	beta_pick.z = (int)S_CHUNK_Z(first_id) * CHUNK_SIZE
		+ rand_gen_range(&model, 0, CHUNK_SIZE);
	beta_pick.y = rand_gen_range(&model, 0, WORLD_HEIGHT);
	for(size_t k = 1; k < chunks_in_range * FCAVEX_RANDOM_TICKS_PER_CHUNK; k++) {
		(void)rand_gen_range(&model, 0, CHUNK_SIZE);
		(void)rand_gen_range(&model, 0, CHUNK_SIZE);
		(void)rand_gen_range(&model, 0, WORLD_HEIGHT);
	}
	const uint32_t beta_seed_after = model.seed;

	/* The random-first control pick (from S0 itself): must differ, or the
	 * golden could not tell the orders apart. */
	struct random_gen ctrl = {.seed = SEED_S0};
	struct pick rf_pick;
	rf_pick.x = (int)S_CHUNK_X(first_id) * CHUNK_SIZE
		+ rand_gen_range(&ctrl, 0, CHUNK_SIZE);
	rf_pick.z = (int)S_CHUNK_Z(first_id) * CHUNK_SIZE
		+ rand_gen_range(&ctrl, 0, CHUNK_SIZE);
	rf_pick.y = rand_gen_range(&ctrl, 0, WORLD_HEIGHT);
	printf("  Beta-order first pick (%d,%d,%d); random-first pick (%d,%d,%d)\n",
		   beta_pick.x, beta_pick.y, beta_pick.z, rf_pick.x, rf_pick.y,
		   rf_pick.z);
	assert(!(beta_pick.x == rf_pick.x && beta_pick.y == rf_pick.y
			 && beta_pick.z == rf_pick.z));
	assert(!(beta_pick.x == FIRE_X && beta_pick.z == FIRE_Z
			 && (beta_pick.y == FIRE_Y || beta_pick.y == FIRE_Y - 1)));

	set(beta_pick.x, beta_pick.y, beta_pick.z, 74); /* glowing redstone ore */
	assert(get_type(beta_pick.x, beta_pick.y, beta_pick.z) == 74);
	assert(w->scheduled_tick_count == 1);

	s.rand_src.seed = SEED_S0;
	s.rand_src.has_next_gaussian = false;
	const uint64_t tick_before = w->current_tick;
	(void)server_local_world_block_ticks_w(&s, w, true, 0, 0);

	const int ore_after = get_type(beta_pick.x, beta_pick.y, beta_pick.z);
	printf("  ore after the tick: %d; world stream %08x (Beta model %08x)\n",
		   ore_after, (unsigned)s.rand_src.seed, (unsigned)beta_seed_after);
	check("g10-clock", w->current_tick == tick_before + 1,
		  "the clock advanced once (setWorldTime before TickUpdates)");
	check("g10-fire", get_type(FIRE_X, FIRE_Y, FIRE_Z) == BLOCK_FIRE
			  && w->scheduled_tick_count == 1,
		  "the due fire callback ran and re-armed (tickRate 40)");
	check("g10-ore", ore_after == 73,
		  "the random pass hit the cell Beta's order picks after the "
		  "scheduled draw: glowing ore 74 reverted to 73");
	check("g10-stream", s.rand_src.seed == beta_seed_after,
		  "world stream = S0 -> fire nextInt(3) -> every random pick");

	/* R6 fixture freeze: retain due fluids from the FIRST tick, without
	 * stopping the clock or ordinary scheduled fire. The unfrozen golden
	 * above pins the production order and RNG byte-for-byte. */
	fixture_close();
	fixture_open();
	w = &s.worlds[0];
	s.pool_selftest_freeze = true;
	set(FIRE_X, FIRE_Y - 1, FIRE_Z, BLOCK_STONE);
	set(FIRE_X, FIRE_Y, FIRE_Z, BLOCK_WATER_FLOW);
	const size_t queued = w->scheduled_tick_count;
	assert(queued > 0);
	struct scheduled_tick original = w->scheduled_ticks[w->scheduled_tick_head];
	s.rand_src.seed = SEED_S0;
	for(unsigned i = 0; i < 80; i++)
		(void)server_local_world_block_ticks_w(&s, w, true, 0, 0);
	/* Field-by-field: the struct's two trailing padding bytes are never
	 * written by the queue, so a memcmp compared unowned garbage. */
	const struct scheduled_tick* kept = &w->scheduled_ticks[w->scheduled_tick_head];
	check("r6-retained", w->scheduled_tick_count == queued
		  && kept->x == original.x && kept->y == original.y && kept->z == original.z
		  && kept->fire_at_tick == original.fire_at_tick
		  && kept->block_type == original.block_type
		  && kept->queue_flags == original.queue_flags,
		  "due fluid retained field-for-field, not executed");
	check("r6-random", s.rand_src.seed == SEED_S0,
		  "no random-block draws from first simulation tick");
	check("r6-clock", w->current_tick == 80, "world clock continues");
	set(FIRE_X + 4, FIRE_Y - 1, FIRE_Z, BLOCK_STONE);
	set(FIRE_X + 4, FIRE_Y, FIRE_Z, BLOCK_FIRE);
	for(unsigned i = 0; i < 40; i++)
		(void)server_local_world_block_ticks_w(&s, w, true, 0, 0);
	check("r6-nonfluid", s.rand_src.seed != SEED_S0,
		  "ordinary scheduled fire still executes while fluids remain queued");
	s.pool_selftest_freeze = false;
	(void)server_local_world_block_ticks_w(&s, w, true, 0, 0);
	check("r6-resume", get_type(FIRE_X + 1, FIRE_Y, FIRE_Z) == BLOCK_WATER_FLOW,
		  "normal path resumes retained fluid execution");

	fixture_close();
	if(s_fail) {
		printf("test_world_tick_order_golden: %d FAIL\n", s_fail);
		return 1;
	}
	printf("test_world_tick_order_golden: PASS\n");
	return 0;
}
