#ifndef TEST_SOUND_ACTION_ENV_H
#define TEST_SOUND_ACTION_ENV_H

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "test_fixture_dir.h"
#include "block/blocks.h"
#include "block/piston_moving.h"
#include "entity/combat.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/audio.h"
#include "platform/thread.h"

static struct server_local s;
static char fixture[128];
static struct thread drain;
static bool draining = true;
static int sound_count;
static struct client_rpc last_sound;
static int gy;

static void* sound_drain(void* arg) {
	(void)arg;
	while(__atomic_load_n(&draining, __ATOMIC_RELAXED)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void sound_capture(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SOUND) {
		sound_count++;
		last_sound = *call;
	}
}

static void sound_env_init(const char* name) {
	test_fixture_dir(name, fixture, sizeof fixture);
	assert(clin_init());
	assert(svin_init());
	assert(thread_create(&drain, sound_drain, NULL, 64));
	blocks_init();
	items_init();
#ifdef _WIN32
	_mkdir(fixture);
	_mkdir(test_fixture_join(fixture, "/region"));
#else
	mkdir(fixture, 0777);
	mkdir(test_fixture_join(fixture, "/region"), 0777);
#endif
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.rand_src.seed = UINT32_C(0xA1B2C3D4);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	gy = -1;
	for(int y = 127; y >= 0; y--) {
		struct block_data b;
		assert(server_world_get_block(&s.worlds[0], 8, y, 8, &b));
		if(b.type != BLOCK_AIR) {
			gy = y;
			break;
		}
	}
	assert(gy > 0);
	s.player.x = 8.5;
	s.player.y = gy + 1 + PLAYER_EYE_HEIGHT;
	s.player.z = 8.5;
	clin_test_set_rpc_observer(sound_capture, NULL);
}

static void sound_env_finish(void) {
	clin_test_set_rpc_observer(NULL, NULL);
	__atomic_store_n(&draining, false, __ATOMIC_RELAXED);
	thread_join(&drain);
	test_fixture_dir_cleanup(fixture);
}

static void sound_set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta}));
}

static struct block_data sound_block(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b;
}

static inline void sound_hold(uint16_t id) {
	inventory_set_hotbar(&s.player.inventory, 0);
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = id, .count = 1});
}

static inline void sound_place(int x, int y, int z, enum side side) {
	struct server_rpc r = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side}};
	server_local_test_process_rpc(&s, &r);
}

static void sound_assert(uint16_t id, float vol, float pitch,
						 int x, int y, int z) {
	assert(sound_count == 1);
	assert(last_sound.payload.sound.sound_id == id);
	assert(last_sound.payload.sound.vol == vol);
	assert(last_sound.payload.sound.pitch == pitch);
	assert(!last_sound.payload.sound.ui);
	assert(last_sound.payload.sound.pos[0] == x + 0.5F);
	assert(last_sound.payload.sound.pos[1] == y + 0.5F);
	assert(last_sound.payload.sound.pos[2] == z + 0.5F);
}

#endif
