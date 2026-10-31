/*
	IMP-QUIT-UNSAVABLE (maintainer 2026-10-21): an unsavable chunk at Save and
	quit must never leave the player stuck on the pause screen.

	Drives the REAL server unload path (server_local_test_process_rpc ->
	SRPC_UNLOAD_WORLD -> release -> checkpoint) with chunk (0,0) refusing to
	save exactly like a fail-closed entity_preserve
	(server_world_test_unsavable_chunk), and observes the real client RPCs:

	  Q1  the refusal returns (no hang), keeps the world fully live and answers
	      CRPC_RETURN_INGAME carrying the dialog state: unsaved_chunk, the
	      chunk coordinates and the count;
	  Q2  "Try again" while the cause persists is asked again, still live;
	  Q3  "Quit anyway" naming a DIFFERENT chunk/count excuses nothing;
	  Q4  "Try again" once the cause clears quits normally and BOTH chunks'
	      changes are on disk;
	  Q5  (fresh world) "Quit anyway" naming the chunk quits: every other
	      chunk's change is on disk, the unsavable chunk was never written,
	      and level.dat is saved.

	The client half (the approved confirm dialog, its strings, safe answer
	and the re-sent unload payloads) is covered by
	tests/test_pause_approved_actions.c test_exit_unsavable_dialog.
*/

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
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"

static char s_fixture_WORLD_ROOT[128];
#define WORLD_ROOT s_fixture_WORLD_ROOT

static volatile int drain_run = 1;
static void* drain_client(void* unused) {
	(void)unused;
	while(drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void remove_world_dirs(const char* root) {
	test_fixture_rm_rf(root);
}

static void make_world(const char* root) {
	char path[256];
	remove_world_dirs(root);
	MKDIR(root);
	snprintf(path, sizeof(path), "%s/region", root);
	MKDIR(path);
	string_t name;
	string_init_set_str(name, root);
	struct level_archive level;
	assert(level_archive_create_new(
		&level, name, 424242, (vec3) {8.5F, 70.0F, 8.5F}));
	level_archive_destroy(&level);
	string_clear(name);
}

/* Same hand-init convention as test_death_screen_env.c's init_server_ex. */
static void init_server(struct server_local* server, const char* root) {
	memset(server, 0, sizeof(*server));
	rand_gen_seed(&server->rand_src);
	server->container_drop_rng.seed = 0x12345678u;
	string_init_set_str(server->level_name, root);
	assert(level_archive_create(&server->level, server->level_name));
	if(!level_archive_read_death_tx_counter(&server->level,
											&server->death_tx_counter))
		server->death_tx_counter = CAVEWII_DEATH_TX_LEGACY_FLOOR;
	assert(inventory_create(&server->player.inventory, &inventory_logic_player,
							server, &server->player, INVENTORY_SIZE, 0, 0, 0));
	server->player.active_inventory = &server->player.inventory;
	server->player.x = 8.5;
	server->player.y = 70.0;
	server->player.z = 8.5;
	server->player.spawn_x = 8;
	server->player.spawn_y = 70;
	server->player.spawn_z = 8;
	server->player.health = MAX_PLAYER_HEALTH;
	server->player.dimension = WORLD_DIM_OVERWORLD;
	server->player.has_pos = true;
	server->player.finished_loading = true;
	server->player.fire = -PLAYER_FIRE_RESISTANCE;
	server->player.oxygen = MAX_OXYGEN;
	server->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	if(!server_local_te_tables_init(server))
		abort();
	piston_moving_store_init(&server->piston_moving);
	dict_entity_init(server->entities);
	server->world_populate_scratch = server_world_populate_scratch_create();
	assert(server->world_populate_scratch);
	assert(server_world_create_with_populate_scratch(
		&server->worlds[0], server->level_name, WORLD_DIM_OVERWORLD, 424242, true,
		server->world_populate_scratch));
	server->worlds[0].local = server;
	server->worlds[0].session = server;
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&server->worlds[0], 0, 0, &chunk) && chunk);
	chunk = NULL;
	assert(server_world_load_chunk(&server->worlds[0], 1, 0, &chunk) && chunk);
}

static void discard_server(struct server_local* server) {
	if(server->player.has_pos)
		server_world_discard(&server->worlds[0]);
	server_world_populate_scratch_destroy(server->world_populate_scratch);
	server->world_populate_scratch = NULL;
	piston_moving_store_destroy(&server->piston_moving);
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(server->entities);
	if(server->level.data)
		level_archive_discard(&server->level);
	inventory_destroy(&server->player.inventory, &server->player);
	mem2_free_sized(
		server->entity_tick_keys,
		server->entity_tick_capacity * sizeof(*server->entity_tick_keys));
	server->entity_tick_keys = NULL;
	death_drop_journal_free(server->death_drop_journal);
	server->death_drop_journal = NULL;
	string_clear(server->level_name);
}

/* A marker block high in the air of each chunk (generated terrain never
 * reaches y=120, so a gold block there exists only if it was saved). */
#define MARK_Y 120
#define MARK_BLOCK 41 /* gold block */
static void mark_chunks(struct server_local* server) {
	(void)server_world_set_block_silent(
		&server->worlds[0], 3, MARK_Y, 3,
		(struct block_data) {.type = MARK_BLOCK, .metadata = 0});
	(void)server_world_set_block_silent(
		&server->worlds[0], 16 + 3, MARK_Y, 3,
		(struct block_data) {.type = MARK_BLOCK, .metadata = 0});
}

/* Reopen the world from disk: does chunk (cx,0) exist on disk, and does it
 * carry the marker? */
static void disk_probe(int cx, bool* on_disk, bool* marked) {
	string_t name;
	string_init_set_str(name, WORLD_ROOT);
	struct server_world_populate_scratch* scratch
		= server_world_populate_scratch_create();
	struct server_world* w = calloc(1, sizeof(*w));
	assert(scratch && w);
	assert(server_world_create_with_populate_scratch(
		w, name, WORLD_DIM_OVERWORLD, 424242, true, scratch));
	*on_disk = server_world_disk_has_chunk(w, cx, 0);
	*marked = false;
	if(*on_disk) {
		struct server_chunk* chunk = NULL;
		assert(server_world_load_chunk(w, cx, 0, &chunk) && chunk);
		struct block_data blk;
		assert(server_world_get_block(w, cx * 16 + 3, MARK_Y, 3, &blk));
		*marked = blk.type == MARK_BLOCK;
	}
	server_world_discard(w);
	free(w);
	server_world_populate_scratch_destroy(scratch);
	string_clear(name);
}

static unsigned plain_returns, unsaved_returns, resets;
static struct client_rpc last_return;
static void observer(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_RETURN_INGAME) {
		last_return = *rpc;
		if(rpc->payload.return_ingame.unsaved_chunk)
			unsaved_returns++;
		else
			plain_returns++;
	} else if(rpc->type == CRPC_WORLD_RESET) {
		resets++;
	}
}

static void unload(struct server_local* s, bool quit_anyway, int32_t x,
				   int32_t z, uint32_t count) {
	struct server_rpc call = {
		.type = SRPC_UNLOAD_WORLD,
		.payload.unload_world = {
			.accept_unsaved = quit_anyway,
			.chunk_x = x,
			.chunk_z = z,
			.count = count,
		},
	};
	server_local_test_process_rpc(s, &call);
}

static void expect_dialog(struct server_local* s, unsigned n) {
	assert(unsaved_returns == n && plain_returns == 0 && resets == 0);
	assert(last_return.payload.return_ingame.chunk_x == 0
		   && last_return.payload.return_ingame.chunk_z == 0
		   && last_return.payload.return_ingame.count == 1);
	/* Nothing torn down: the world, its chunks and the player stay live. */
	assert(s->player.has_pos && s->level.data);
	assert(server_world_is_chunk_loaded(&s->worlds[0], 0, 0)
		   && server_world_is_chunk_loaded(&s->worlds[0], 1, 0));
}

int main(void) {
	test_fixture_dir("test_quit_unsavable_env_world", s_fixture_WORLD_ROOT,
					 sizeof s_fixture_WORLD_ROOT);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	struct thread drain;
	thread_create(&drain, drain_client, NULL, 64);
	clin_test_set_rpc_observer(observer, NULL);

	/* ---- Q1..Q4: Try again ------------------------------------------- */
	make_world(WORLD_ROOT);
	struct server_local server;
	init_server(&server, WORLD_ROOT);
	mark_chunks(&server);
	server_world_test_unsavable_chunk(true, 0, 0);

	unload(&server, false, 0, 0, 0);
	expect_dialog(&server, 1);
	printf("Q1 unsavable chunk at quit -> dialog state (chunk 0,0 x1), no hang, world live OK\n");

	unload(&server, false, 0, 0, 0); /* Try again, cause persists */
	expect_dialog(&server, 2);
	printf("Q2 Try again while unsavable -> asked again, still live OK\n");

	unload(&server, true, 1, 0, 1); /* Quit anyway for the WRONG chunk */
	expect_dialog(&server, 3);
	unload(&server, true, 0, 0, 2); /* ... or the wrong count */
	expect_dialog(&server, 4);
	printf("Q3 Quit anyway naming another chunk/count excuses nothing OK\n");

	server_world_test_unsavable_chunk(false, 0, 0); /* the cause clears */
	unload(&server, false, 0, 0, 0); /* Try again */
	assert(resets == 1 && unsaved_returns == 4 && plain_returns == 0);
	assert(!server.player.has_pos);
	bool on_disk, marked;
	disk_probe(0, &on_disk, &marked);
	assert(on_disk && marked);
	disk_probe(1, &on_disk, &marked);
	assert(on_disk && marked);
	discard_server(&server);
	printf("Q4 Try again after the cause clears -> quits, both chunks saved OK\n");

	/* ---- Q5: Quit anyway --------------------------------------------- */
	make_world(WORLD_ROOT);
	unsaved_returns = plain_returns = resets = 0;
	init_server(&server, WORLD_ROOT);
	mark_chunks(&server);
	server.player.x = 20.5; /* a level.dat change only a real save keeps */
	server_world_test_unsavable_chunk(true, 0, 0);
	unload(&server, false, 0, 0, 0);
	expect_dialog(&server, 1);
	unload(&server, true, 0, 0, 1); /* Quit anyway, exactly as named */
	assert(resets == 1 && unsaved_returns == 1 && plain_returns == 0);
	assert(!server.player.has_pos);
	server_world_test_unsavable_chunk(false, 0, 0);
	disk_probe(1, &on_disk, &marked);
	assert(on_disk && marked && "every other chunk is saved");
	disk_probe(0, &on_disk, &marked);
	assert(!on_disk && "only the unsavable chunk's changes are lost");
	{
		string_t name;
		string_init_set_str(name, WORLD_ROOT);
		struct level_archive level;
		assert(level_archive_create(&level, name));
		vec3 pos;
		vec2 rot;
		enum world_dim dim;
		assert(level_archive_read_player(&level, pos, rot, NULL, &dim));
		assert(fabsf(pos[0] - 20.5F) < 0.01F && "level.dat saved");
		level_archive_destroy(&level);
		string_clear(name);
	}
	discard_server(&server);
	printf("Q5 Quit anyway -> quits, other chunk + level.dat saved, only the unsavable chunk lost OK\n");

	clin_test_set_rpc_observer(NULL, NULL);
	test_fixture_dir_cleanup(WORLD_ROOT);
	drain_run = 0;
	thread_join(&drain);
	printf("test_quit_unsavable_env: ALL OK\n");
	return 0;
}
