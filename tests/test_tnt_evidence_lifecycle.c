/* TNT telemetry lifetime across actual world release/refusal/load boundaries. */
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

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static int s_drain = 1;

static void* drain_entry(void* unused) {
	(void)unused;
	while(__atomic_load_n(&s_drain, __ATOMIC_SEQ_CST)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static struct server_local_tnt_evidence take(void) {
	struct server_local_tnt_evidence d = {0};
	assert(server_local_tnt_evidence_take(&d));
	return d;
}

int main(void) {
	char path[160], region[180];
	test_fixture_dir("test_tnt_evidence_lifecycle", path, sizeof path);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	static struct server_local s;
	server_local_test_create_without_thread(&s, false, false, false, false,
		false, true, false);
	struct server_local_tnt_evidence initial = take();
	assert(initial.st == 0 && initial.bits == 0 && initial.tick == 0);
	MKDIR(path);
	snprintf(region, sizeof region, "%s/region", path);
	MKDIR(region);
	string_t name;
	string_init_set_str(name, path);
	struct level_archive level;
	assert(level_archive_create_new(&level, name, 4242,
		(vec3) {8.5f, 6.0f, 8.5f}));
	level_archive_destroy(&level);
	assert(server_local_load_world_impl(&s, name));
	struct server_local_tnt_evidence loaded = take();
	assert(loaded.gen != initial.gen && loaded.st == 0);
	assert(s.player.has_pos);
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	struct server_chunk* chunk = NULL;
	if(!server_world_is_chunk_loaded(&s.world, 0, 1))
		assert(server_world_load_chunk(&s.world, 0, 1, &chunk) && chunk);
	/* Exercise the existing verdict publisher with an already settled pad. */
	const int px = 8, py = 6, tz = 18;
	for(int x = px - 7; x <= px + 7; x++)
		for(int z = tz - 2; z <= tz + 2; z++)
			assert(server_world_set_block(&s.worlds[0], x, py - 1, z,
				(struct block_data) {.type = BLOCK_STONE}));
	for(int x = px - 7; x < px + 1; x++)
		assert(server_world_set_block(&s.worlds[0], x, py - 1, tz,
			(struct block_data) {.type = BLOCK_AIR}));
	s.tnt_chain_bx = px;
	s.tnt_chain_by = py;
	s.tnt_chain_bz = tz - 10;
	s.tnt_chain_tick = 700;
	server_local_test_update_once(&s);
	struct server_local_tnt_evidence pass = take();
	assert(pass.st == 2 && pass.bits == 7 && pass.primed_left == 0);
	assert(pass.carved >= 8 && pass.pad == 75 && pass.tick == 700);
	/* A failed checkpoint returns before the common committed release. */
	string_t saved_path;
	string_init_set(saved_path, s.level.file_name);
	string_cat_str(s.level.file_name, "/missing");
	server_local_unload_world_impl(&s, true);
	struct server_local_tnt_evidence refused = take();
	assert(refused.gen == pass.gen && refused.st == pass.st);
	string_set(s.level.file_name, saved_path);
	string_clear(saved_path);
	server_local_unload_world_impl(&s, true);
	struct server_local_tnt_evidence discarded = take();
	assert(discarded.gen != pass.gen && discarded.st == 0 && discarded.bits == 0);
	server_local_test_fail_next_world_load();
	assert(!server_local_load_world_impl(&s, name));
	struct server_local_tnt_evidence failed_load = take();
	assert(failed_load.gen != discarded.gen && failed_load.st == 0);
	assert(server_local_load_world_impl(&s, name));
	struct server_local_tnt_evidence next = take();
	assert(next.gen != failed_load.gen && next.st == 0 && next.tick == 0);
	s.player.finished_loading = true;
	s.tnt_chain_demo = true;
	s.tnt_chain_tick = 0; /* the existing builder's explicit fresh-fixture precondition */
	s.piston_demo_defer = 1;
	server_local_test_update_once(&s);
	struct server_local_tnt_evidence pending = take();
	assert(pending.gen != next.gen && pending.st == 1 && pending.tick == 1);
	server_local_unload_world_impl(&s, true);
	string_clear(name);
	__atomic_store_n(&s_drain, 0, __ATOMIC_SEQ_CST);
	thread_join(&drain);
	puts("PASS TNT verdict retention, release/load reset and fixture rebuild");
	return 0;
}
