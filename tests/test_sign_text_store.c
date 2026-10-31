/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	source/game/sign_text_store.h: the client-side per-position sign-text
	cache. Two parts:

	  (1) Pure store unit semantics: set/get/overwrite/remove/missing-lookup/
	      clear()/clear_chunk() driven directly, no server or client harness.

	  (2) An integration case through the REAL production dispatch: place a
	      sign (SRPC_BLOCK_PLACE), stream its chunk for the first time
	      (server_local_test_update_once -- FEED PATH 1, the per-chunk
	      CRPC_SIGN_TEXT wired into server_local.c's near-send loop), edit and
	      close it (SRPC_SIGN_SET + SRPC_WINDOW_CLOSE -- FEED PATH 2, the
	      inv_on_close edit echo), then break it (SRPC_BLOCK_DIG -- FEED PATH
	      3, block_sign.c onBreak's CRPC_SIGN_TEXT_REMOVED). Every CRPC is
	      applied through the real clin_process (client_interface.c), the
	      same function the actual render thread calls -- this is the same
	      direct-SRPC-dispatch harness tests/test_sign_placement.c already
	      uses for sign placement, extended with clin_test_set_rpc_observer
	      (the test_boat_env.c/test_minecart_authority.c precedent) to feed
	      the two new CRPCs into the real client dispatch.

	FEED PATH 4 (multiplayer inbound Packet130UpdateSign -> server_remote.c's
	MP_EVENT_UPDATE_SIGN -> CRPC_SIGN_TEXT) is implemented but NOT exercised
	here: server_remote.c/mp_codec.c are excluded from ${_cavex_srcs} (added
	to the `cavex` target separately, see CMakeLists.txt) and the only
	existing harness that drives process_event() end-to-end
	(tests/test_mp_s0_env.c) does so over a real loopback socket with a full
	login/world-join sequence -- disproportionate scope to stand up here for
	one forwarding case whose own wire DECODE already has its own golden
	coverage in that file. See the session report for the full reasoning.
*/

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
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
#include "entity/entity.h"
#include "game/sign_text_store.h"
#include "game/client_world_context.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/items_object.h"
#include "item/window_container.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_sign_text_store_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

// ===================== Part 1: pure store unit semantics ==================

static void fill_grid(char grid[64], char c) {
	memset(grid, c, 64);
}

static void test_store_basic_semantics(void) {
	sign_text_store_init();
	sign_text_store_clear();

	char out[64];
	// Missing lookup: nothing was ever set at this position.
	assert(!sign_text_store_get(1, 2, 3, out));

	// Set + get round-trip.
	char grid_a[64];
	fill_grid(grid_a, ' ');
	memcpy(grid_a, "HELLO", 5);
	sign_text_store_set(1, 2, 3, grid_a);
	assert(sign_text_store_get(1, 2, 3, out));
	assert(memcmp(out, grid_a, 64) == 0);

	// A different position is unaffected.
	assert(!sign_text_store_get(4, 5, 6, out));

	// Overwrite at the SAME position replaces, not merges.
	char grid_b[64];
	fill_grid(grid_b, ' ');
	memcpy(grid_b, "BYE", 3);
	sign_text_store_set(1, 2, 3, grid_b);
	assert(sign_text_store_get(1, 2, 3, out));
	assert(memcmp(out, grid_b, 64) == 0);

	// Remove drops it; removing an already-missing position (a repeat remove,
	// or a position that was never set) is a harmless no-op.
	sign_text_store_remove(1, 2, 3);
	assert(!sign_text_store_get(1, 2, 3, out));
	sign_text_store_remove(1, 2, 3);
	assert(!sign_text_store_get(1, 2, 3, out));
	sign_text_store_remove(99, 99, 99);

	printf("  store basic semantics: set/get/overwrite/remove/missing OK\n");
}

static void test_store_multi_position_and_clear(void) {
	sign_text_store_clear();
	char out[64];

	char grid1[64], grid2[64], grid3[64];
	fill_grid(grid1, ' ');
	memcpy(grid1, "ONE", 3);
	fill_grid(grid2, ' ');
	memcpy(grid2, "TWO", 3);
	fill_grid(grid3, ' '); // a negative-coordinate position
	memcpy(grid3, "SIX", 3);

	sign_text_store_set(10, 64, 10, grid1);
	sign_text_store_set(20, 64, 10, grid2);
	sign_text_store_set(-5, 64, -5, grid3);

	assert(sign_text_store_get(10, 64, 10, out) && memcmp(out, grid1, 64) == 0);
	assert(sign_text_store_get(20, 64, 10, out) && memcmp(out, grid2, 64) == 0);
	assert(sign_text_store_get(-5, 64, -5, out) && memcmp(out, grid3, 64) == 0);

	// clear() drops EVERY position, not just the last one set.
	sign_text_store_clear();
	assert(!sign_text_store_get(10, 64, 10, out));
	assert(!sign_text_store_get(20, 64, 10, out));
	assert(!sign_text_store_get(-5, 64, -5, out));

	printf("  store multi-position + clear() drops everything OK\n");
}

static void test_store_clear_chunk(void) {
	sign_text_store_clear();
	char out[64];

	char grid_a[64], grid_b[64], grid_c[64], grid_neg[64];
	fill_grid(grid_a, 'A');
	fill_grid(grid_b, 'B');
	fill_grid(grid_c, 'C');
	fill_grid(grid_neg, 'N');

	// Chunk (0,0) covers x,z in [0,15]; chunk (1,0) covers x in [16,31].
	sign_text_store_set(3, 64, 3, grid_a);  // chunk (0,0)
	sign_text_store_set(15, 70, 0, grid_b); // chunk (0,0), a different cell
	sign_text_store_set(16, 64, 3, grid_c); // chunk (1,0) -- must survive
	// Chunk (-1,0) covers x in [-16,-1]: proves floor division, not truncation
	// toward zero (a naive x/16 would put -1 in chunk 0, not chunk -1).
	sign_text_store_set(-1, 64, 3, grid_neg); // chunk (-1,0) -- must survive

	sign_text_store_clear_chunk(0, 0);

	assert(!sign_text_store_get(3, 64, 3, out));
	assert(!sign_text_store_get(15, 70, 0, out));
	assert(sign_text_store_get(16, 64, 3, out) && memcmp(out, grid_c, 64) == 0);
	assert(sign_text_store_get(-1, 64, 3, out)
		  && memcmp(out, grid_neg, 64) == 0);

	// A chunk with nothing in it is a harmless no-op.
	sign_text_store_clear_chunk(42, 42);

	sign_text_store_clear();
	printf("  store clear_chunk: removes only the matching chunk's signs, "
		  "floor-divides negatives correctly OK\n");
}

// ================ Part 2: integration through the real CRPC path ==========

static struct server_local s;
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

// Applies the store-relevant CRPCs synchronously through the REAL
// client_interface.c dispatch (clin_process) -- exactly what the render
// thread's clin_update would do -- proving this store's production wiring,
// not a hand-rolled substitute. Every other CRPC type is left for the drain
// thread above, matching the test_boat_env.c / test_minecart_authority.c
// rpc-observer precedent.
static void sign_rpc_observer(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SIGN_TEXT || call->type == CRPC_SIGN_TEXT_REMOVED) {
		struct client_rpc copy = *call;
		clin_process(&copy);
	}
}

static void set_block(int x, int y, int z, int type, int meta) {
	struct block_data expected
		= {.type = (uint8_t)type, .metadata = (uint8_t)meta};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static void test_full_sign_lifecycle(void) {
	sign_text_store_clear();

	const int x = 5, y = 70, z = 5;
	set_block(x, y, z, BLOCK_STONE, 0);
	set_block(x, y, z - 1, BLOCK_AIR, 0); // FRONT target, still chunk (0,0)

	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0,
					   (struct item_data) {.id = ITEM_SIGN, .count = 1});
	inventory_set_hotbar(&s.player.inventory, 0);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;
	s.player.x = x + 0.5;
	s.player.y = y + 1.62;
	s.player.z = z + 3.5; // clear of the target cell, same chunk

	// ---- place: Beta's displayGUIEditSign opens the sign edit GUI right
	// after a successful placement (test_sign_placement.c's own precedent).
	struct server_rpc place = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_FRONT},
	};
	server_local_test_process_rpc(&s, &place);
	assert(s.player.active_inventory != &s.player.inventory);
	assert(s.player.active_inventory->logic == &inventory_logic_sign);

	// ---- FEED PATH 1: the chunk carrying the sign was loaded directly
	// (server_world_load_chunk below), never streamed. Nothing has told the
	// client this sign exists yet -- the store must still be empty for it.
	char out[64], blank[64];
	fill_grid(blank, ' ');
	assert(!sign_text_store_get(x, y, z - 1, out));
	// One real tick streams the near radius (server_local_update's chunk-send
	// loop); a couple of extra calls are a harmless no-op once nothing is
	// left unsent, cheap insurance against a single-tick send-budget split.
	for(int i = 0; i < 3; i++)
		server_local_test_update_once(&s);
	assert(sign_text_store_get(x, y, z - 1, out));
	assert(memcmp(out, blank, 64) == 0); // freshly placed: an all-space grid

	// ---- FEED PATH 2: edit two cells, then close -- inv_on_close's durable
	// copy echoes back as the SAME CRPC_SIGN_TEXT.
	struct server_rpc set_h = {
		.type = SRPC_SIGN_SET,
		.payload.sign_set = {.window = WINDOWC_SIGN,
							 .slot = 0,
							 .ch = 'H',
							 .action_id = 1,
							 .x = x,
							 .y = y,
							 .z = z - 1},
	};
	server_local_test_process_rpc(&s, &set_h);
	struct server_rpc set_i = {
		.type = SRPC_SIGN_SET,
		.payload.sign_set = {.window = WINDOWC_SIGN,
							 .slot = 1,
							 .ch = 'i',
							 .action_id = 2,
							 .x = x,
							 .y = y,
							 .z = z - 1},
	};
	server_local_test_process_rpc(&s, &set_i);

	struct server_rpc close = {
		.type = SRPC_WINDOW_CLOSE,
		.payload.window_close = {.window = WINDOWC_SIGN},
	};
	server_local_test_process_rpc(&s, &close);
	assert(s.player.active_inventory == &s.player.inventory); // window closed

	assert(sign_text_store_get(x, y, z - 1, out));
	assert(out[0] == 'H' && out[1] == 'i');
	for(size_t k = 2; k < 64; k++)
		assert(out[k] == ' '); // every other cell stayed blank

	// ---- FEED PATH 3: breaking the sign removes it from the client store.
	struct server_rpc dig = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.x = x,
							  .y = y,
							  .z = z - 1,
							  .side = SIDE_TOP,
							  .finished = true},
	};
	server_local_test_process_rpc(&s, &dig);
	assert(!sign_text_store_get(x, y, z - 1, out));

	printf("  full lifecycle (place -> chunk-send -> edit+close -> break) "
		  "through the real CRPC path OK\n");
}


/* IMP-TE-CAPS: a chunk may now hold more signs than the old 256-row table
 * (Beta has no cap). Unloading it must erase EVERY cached text, or dense-sign
 * chunks leak client memory on each unload. 300 signs in chunk (2,2), plus
 * a neighbour that must survive. */
static void test_store_clear_dense_chunk(void) {
	sign_text_store_clear();
	char grid[64], out[64];
	fill_grid(grid, 'D');
	int placed = 0;
	for(int y = 0; y < 2 && placed < 300; y++)
		for(int x = 32; x < 48 && placed < 300; x++)
			for(int z = 32; z < 48 && placed < 300; z++, placed++)
				sign_text_store_set(x, 60 + y, z, grid);
	sign_text_store_set(48, 60, 32, grid); /* chunk (3,2): survives */
	assert(sign_text_store_count(sign_text_store_primary()) == 301);
	sign_text_store_clear_chunk(2, 2);
	assert(sign_text_store_count(sign_text_store_primary()) == 1
		   && "every one of the 300 signs is erased, not just 256");
	assert(sign_text_store_get(48, 60, 32, out));
	sign_text_store_clear();
	printf("  store clear_chunk: 300-sign chunk fully erased OK\n");
}

int main(void) {
	test_fixture_dir("test_sign_text_store_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	test_store_basic_semantics();
	test_store_multi_position_and_clear();
	test_store_clear_chunk();
	test_store_clear_dense_chunk();

	assert(clin_init());
	struct thread drain;

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	string_clear(level);
	/* A real client receives this identity from CRPC_WORLD_RESET. Install the
	 * fixture context only after creation assigns the world's generation. */
	assert(client_ctx_registry_install(clin_world_contexts(),
		s.worlds[0].dimension, s.worlds[0].world_index,
		s.worlds[0].generation));
	clin_test_set_rpc_observer(sign_rpc_observer, NULL);
	assert(thread_create(&drain, drain_entry, NULL, 64));
	s.world.local = &s; // server_local_test_update_once's tick needs this
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	// sign_pos[i].y < 0 is the free-slot convention (server_local_init's own
	// boot memset); a plain zero-fill from memset(&s,0,...) above would read
	// as slot 0 already OCCUPIED at (0,0,0).
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;

	printf("test_sign_text_store:\n");
	test_full_sign_lifecycle();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_sign_text_store\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
