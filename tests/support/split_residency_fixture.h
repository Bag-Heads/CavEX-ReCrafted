/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef TESTS_SPLIT_RESIDENCY_FIXTURE_H
#define TESTS_SPLIT_RESIDENCY_FIXTURE_H
#include "block/blocks.h"
#include "entity/combat.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "test_fixture_dir.h"
#include "b1_bind_client.h"
#include <assert.h>
#include <m-lib/m-string.h>
#include <stdlib.h>
#include <string.h>

static char split_path[128];
static struct server_local* split_fixture(void) {
	test_fixture_dir("test_split_residency", split_path, sizeof(split_path));
#ifdef _WIN32
	_mkdir(split_path);
#else
	mkdir(split_path, 0777);
#endif
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true, false);
	string_t path;
	string_init_set_str(path, split_path);
	assert(level_archive_create_new(&s->level, path, 12345, (vec3) {0, 4, 0}));
	assert(server_world_create_with_populate_scratch(
		&s->world, path, WORLD_DIM_OVERWORLD, 12345, true,
		s->world_populate_scratch));
	string_clear(path);
	s->world.local = s;
	s->worlds[0].session = s; /* A1a: services through the owning session */
	s->player.has_pos = true;
	s->player.finished_loading = false;
	s->player.x = s->player.z = 0.5;
	s->player.y = 4.0 + PLAYER_EYE_HEIGHT;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.oxygen = MAX_OXYGEN;
	s->autosave_ticks = 0;
	s->far_view_distance = MAX_VIEW_DISTANCE + 1;
	b1_bind_client_world(&s->worlds[0]);
	return s;
}
static void split_destroy(struct server_local* s) {
	server_world_discard(&s->world);
	level_archive_discard(&s->level);
	server_world_populate_scratch_destroy(s->world_populate_scratch);
	inventory_destroy(&s->player.inventory, &s->player);
	dict_entity_clear(s->entities);
	dict_autosave_saved_clear(s->autosave_saved_this_epoch);
	for(unsigned k = 0; k < SERVER_LOCAL_WORLD_SLOTS; k++)
		dict_autosave_saved_clear(s->autosave_saved_this_epoch_w[k]);
	string_clear(s->level_name);
#ifndef CW_CAPTURE_PINNED_BASELINE
	if(s->residency)
		mem2_free(s->residency);
#endif
	free(s);
	test_fixture_dir_cleanup(split_path);
}
static struct server_chunk* split_populate(struct server_local* s, int x,
										   int z) {
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 1000; n++) {
		server_world_begin_update(&s->world);
		if(server_world_ensure_populated(&s->world, s, x, z, &c))
			return c;
	}
	assert(!"populate failed");
	return NULL;
}
#endif
