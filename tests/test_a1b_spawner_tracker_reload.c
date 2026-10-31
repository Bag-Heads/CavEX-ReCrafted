/* A1b/fix-1 (Fable review F1): the near-player spawner tracker
 * (active_spawners[], MAX_ACTIVE_SPAWNERS = 8) must survive world reloads.
 * Each reload (the ordinary load path; portal travel reuses it) creates a new
 * slot-0 generation. A row tracked in an earlier generation is never seen or
 * evicted by the new instance's pass, so without a load-boundary release it
 * stays `used` forever; after 8 reloads the tracker is full and every spawner
 * in the session silently stops firing. Drives the real
 * server_local_load_world_impl / server_local_unload_world_impl 9 times, each
 * with a tracked spawner, and requires the spawner to be tracked every time. */
#include "support/split_residency_fixture.h"

#include "item/items.h"
#include "network/server_spawner.h"

#include <stdio.h>

int server_local_test_tick_spawners(struct server_local* s);

static struct server_local s_load;

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	char dir[128];
	test_fixture_dir("test_a1b_spawner_tracker_reload", dir, sizeof(dir));
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0777);
#endif
	server_local_test_create_without_thread(&s_load, false, false, false,
											false, false, true /*superflat*/,
											false);
	string_t lvl;
	string_init_set_str(lvl, dir);
	{
		struct level_archive la;
		assert(level_archive_create_new(&la, lvl, 4242,
			(vec3) {8.5F, 4.0F + PLAYER_EYE_HEIGHT, 8.5F}));
		level_archive_destroy(&la);
	}
	enum { RELOADS = MAX_ACTIVE_SPAWNERS + 1 };
	int failures = 0;
	uint32_t last_generation = 0;
	for(int i = 0; i < RELOADS; i++) {
		assert(server_local_load_world_impl(&s_load, lvl));
		struct server_world* w = &s_load.worlds[0];
		assert(w->generation != 0 && w->generation != last_generation);
		last_generation = w->generation;
		struct server_chunk* c = NULL;
		if(!dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0)))
			assert(server_world_load_chunk(w, 0, 0, &c) && c);
		/* One dungeon spawner beside the player (distinct cell per load). */
		int sx = 4 + i, sy = 6, sz = 4;
		assert(server_world_set_block_silent(w, sx, sy, sz,
			(struct block_data) {.type = BLOCK_MOB_SPAWNER}));
		assert(spawner_te_upsert_w(w, sx, sy, sz, "Zombie"));
		s_load.player.x = 8.5;
		s_load.player.y = 4.0 + PLAYER_EYE_HEIGHT;
		s_load.player.z = 8.5;
		int tracked = server_local_test_tick_spawners(&s_load);
		printf("reload %d (generation %u): tracked spawners = %d\n", i + 1,
			   (unsigned)w->generation, tracked);
		if(tracked < 1) {
			fprintf(stderr, "FAIL reload %d: spawner not tracked "
					"(tracker rows stranded by earlier generations)\n", i + 1);
			failures++;
		}
		/* Remove it again so each load holds exactly one spawner. */
		spawner_te_free_w(w, sx, sy, sz);
		assert(server_world_set_block_silent(w, sx, sy, sz,
			(struct block_data) {.type = BLOCK_AIR}));
		server_local_unload_world_impl(&s_load, false);
		clin_drain_discard();
	}
	string_clear(lvl);
	test_fixture_dir_cleanup(dir);
	if(failures)
		return 1;
	puts("PASS spawner tracker survives 9 world reloads");
	return 0;
}
