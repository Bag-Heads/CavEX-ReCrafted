/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * RetroMCP b1.7.3 src_original MapData.func_28169_a:71-104 replaces the
 * visible-player list on every held-map update. MapItemRenderer:81-103
 * draws type 0 at (x/2+64,y/2+64), rotation*360/16, size 8 map pixels.
 * The server/client split must retain that list when the inbox is full. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/items/item_map.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/map_data.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "support/test_fixture_dir.h"

static struct server_local s;

static void drain(void) {
	for(unsigned pass = 0; pass < 8; pass++) {
		struct client_rpc call;
		while(clin_test_inbox_pop(&call, NULL)) {
			if(call.type == CRPC_MAP_MARKERS || call.type == CRPC_MAP_DATA)
				clin_process_direct(&call);
			else
				clin_test_rpc_release(&call);
		}
		if(clin_rpc_flush_pending()) {
			while(clin_test_inbox_pop(&call, NULL)) {
				if(call.type == CRPC_MAP_MARKERS || call.type == CRPC_MAP_DATA)
					clin_process_direct(&call);
				else
					clin_test_rpc_release(&call);
			}
			return;
		}
	}
	assert(!"marker publication did not drain");
}

static void update_full(struct item_data* stack) {
	struct client_rpc prior = {.type = CRPC_HUD_MESSAGE};
	unsigned filled = 0;
	while(clin_try_rpc_send(&prior)) filled++;
	assert(filled > 0);
	/* Like Wii MAIN during an expensive frame: no consumer until after
	 * the held-map update. Ordinary queued output must not discard icons. */
	server_local_test_tick_item_updates(&s);
	drain();
	uint32_t version;
	const uint8_t* pixels = clin_held_map_colors((int16_t)stack->durability, &version);
	assert(version > 0 && pixels); /* terrain delivery succeeds under the SAME pressure */
	assert(memcmp(pixels, item_map_cache_resident(&s, (int16_t)stack->durability)->colors,
		MAP_DATA_COLORS_SIZE) == 0);
}

int main(void) {
	char path[256];
	test_fixture_dir("map_marker_backpressure", path, sizeof(path));
#ifdef _WIN32
	assert(_mkdir(path) == 0);
#else
	assert(mkdir(path, 0777) == 0);
#endif
	assert(clin_init());
	blocks_init();
	items_init();
	string_init_set_str(s.level_name, path);
	server_world_create(&s.worlds[0], s.level_name, WORLD_DIM_OVERWORLD, 42, true);
	s.worlds[0].session = &s;
	for(unsigned i = 0; i < MAX_MAP_CACHE; i++) s.map_cache_id[i] = INT32_MIN;
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = 462; s.player.z = -1576; s.player.rx = 90;
	struct item_data stack = {.id = ITEM_MAP, .count = 1};
	assert(item_map.onCreated(&s, &s.player, &stack));
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR, stack);

	struct item_map_marker markers[ITEM_MAP_MAX_MARKERS];
	update_full(&stack);
	size_t count = clin_held_map_markers((int16_t)stack.durability, markers);
	printf("fresh crafted map, full inbox: markers=%zu (expected 1)\n", count);
	fflush(stdout);
	assert(count == 1);
	assert(markers[0].type == 0 && markers[0].x == 0 && markers[0].y == 0);
	assert(markers[0].rotation == 4);
	/* Repeated pressure must also move, rotate, and remove the old icon. */
	s.player.x += 8; s.player.z -= 16; s.player.rx = 180;
	update_full(&stack);
	assert(clin_held_map_markers((int16_t)stack.durability, markers) == 1);
	assert(markers[0].x == 2 && markers[0].y == -3 && markers[0].rotation == -8);
	s.player.x += 1024;
	update_full(&stack);
	assert(clin_held_map_markers((int16_t)stack.durability, markers) == 0);
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_discard(&s.worlds[0]);
	string_clear(s.level_name);
	test_fixture_dir_cleanup(path);
	puts("PASS map marker: fresh craft, movement, rotation and off-map removal survive full inbox");
	return 0;
}
