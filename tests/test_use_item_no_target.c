/*
	critique-1028 eat (maintainer report): food and other held items used with
	NO block under the crosshair did nothing. Beta 1.7.3:
	  Minecraft.clickMouse :834-838 -- after the entity/tile branches, a
	    right-click whose place did not consume it runs
	    playerController.sendUseItem(player, world, currentItem) for any held
	    stack;
	  PlayerController.sendUseItem :47-60 -> ItemStack.useItemRightClick;
	  ItemFood.onItemRightClick :14-17 -- `--stackSize; heal(healAmount)`
	    (instant, no use duration);
	  PlayerControllerMP.sendUseItem :148-153 -- Packet15Place(-1, -1, -1,
	    255, currentItem) first; NetServerHandler.handlePlace :291-296 routes
	    direction 255 to item use; Packet15Place.writePacketData :41-54.

	Drives the REAL server_local dispatch (server_local_test_process_rpc):
	  U1 bread with no target: heals Beta's 5 half-hearts, one bread consumed,
	     slot synced; golden apple caps at full; stew returns the bowl;
	  U2 full health: refused (playability carve-out #2, unchanged), kept;
	  U3 snowball / bow with no target still fire (count / arrow consumed);
	  U4 a block target is unchanged: SRPC_BLOCK_PLACE places stone, and bread
	     against a block still eats through the same hook;
	  U5 the online bytes for bread with no target are exactly Beta's
	     Packet15 air-use form (the encoder call server_remote_protocol.c makes
	     for SRPC_USE_ITEM).
	The client half (no target -> SRPC_USE_ITEM for any held stack) is
	test_split_actions_client's test_no_target_use_item.
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
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/mp_codec.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_world[128];
/* Overworld only: every world access below names &s.worlds[0] explicitly
 * (test_world_alias_gate forbids the legacy single-world alias). */
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

static void hold(uint16_t id, uint8_t count) {
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = id, .count = count});
}
static struct item_data held(void) {
	struct item_data it = {0};
	inventory_get_hotbar_item(&s.player.inventory, &it);
	return it;
}
static void use_no_target(void) {
	struct item_data h = held();
	struct server_rpc use = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 1.0F, .held_item = h,
							 .has_held_item = h.count > 0}};
	server_local_test_process_rpc(&s, &use);
}
static size_t entity_count(void) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		n++;
		dict_entity_next(it);
	}
	return n;
}
static int surface_y(int x, int z) {
	for(int y = 126; y > 0; y--) {
		struct block_data b;
		if(server_world_get_block(&s.worlds[0], x, y, z, &b) && b.type != BLOCK_AIR)
			return y;
	}
	assert(!"no surface");
	return 0;
}

static void test_eat_no_target(void) {
	s.player.health = 80;
	hold(ITEM_BREAD, 1);
	use_no_target();
	/* Beta Item.bread = new ItemFood(41, 5, ...): 5 half-hearts. */
	assert(s.player.health == 80 + 2 * HEALTH_PER_HEART + HEALTH_PER_HEART / 2);
	assert(held().count == 0 && "one bread consumed");

	s.player.health = 8;
	hold(ITEM_APPLE_GOLDEN, 1);
	use_no_target();
	assert(s.player.health == MAX_PLAYER_HEALTH && "golden apple heals to full (capped)");
	assert(held().count == 0);

	s.player.health = 80;
	hold(ITEM_MUSHROOM_STEW, 1);
	use_no_target();
	assert(s.player.health > 80);
	assert(held().id == ITEM_BOWL && held().count == 1 && "stew returns the bowl");
	puts("PASS U1 no-target eat: bread +5 half-hearts and consumed; golden "
		 "apple capped at full; stew -> bowl");
}

static void test_full_health_refused(void) {
	s.player.health = MAX_PLAYER_HEALTH;
	hold(ITEM_PORKCHOP_COOKED, 1);
	use_no_target();
	assert(held().id == ITEM_PORKCHOP_COOKED && held().count == 1);
	assert(s.player.health == MAX_PLAYER_HEALTH);
	puts("PASS U2 full health: food kept (playability carve-out #2 unchanged)");
}

static void test_throw_and_bow_no_target(void) {
	s.player.health = MAX_PLAYER_HEALTH;
	size_t before = entity_count();
	hold(ITEM_SNOW_BALL, 4);
	use_no_target();
	assert(held().count == 3 && entity_count() == before + 1);

	uint32_t pending = s.pending_arrow_count;
	hold(ITEM_BOW, 1);
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 1,
					   (struct item_data) {.id = ITEM_ARROW, .count = 5});
	use_no_target();
	struct item_data arrows = {0};
	inventory_get_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 1, &arrows);
	/* The bow queues its arrow for the next tick's spawn (pending_arrows). */
	assert(arrows.count == 4 && s.pending_arrow_count == pending + 1);
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 1,
					   (struct item_data) {0});
	puts("PASS U3 no-target snowball and bow still fire (item / arrow consumed)");
}

static void test_block_target_unchanged(void) {
	int x = 2, z = 2;
	int y = surface_y(x, z);
	s.player.x = x + 0.5;
	s.player.z = z - 1.5;
	s.player.y = y + 1 + 1.62;
	hold(BLOCK_STONE, 2);
	struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_TOP,
								.held_item = held(), .has_held_item = true}};
	server_local_test_process_rpc(&s, &place);
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y + 1, z, &b)
		   && b.type == BLOCK_STONE && "block target still places");
	assert(held().count == 1);

	s.player.health = 80;
	hold(ITEM_BREAD, 1);
	place.payload.block_place.y = y + 1;
	place.payload.block_place.held_item = held();
	server_local_test_process_rpc(&s, &place);
	assert(s.player.health == 80 + 2 * HEALTH_PER_HEART + HEALTH_PER_HEART / 2
		   && held().count == 0 && "bread against a block still eats");
	puts("PASS U4 block target unchanged: stone placed; bread at a block eats");
}

static void test_remote_bytes(void) {
	/* server_remote_protocol.c SRPC_USE_ITEM: mp_encode_place(-1, 255, -1,
	 * 255, item). Beta: 0x0F; int x=-1; byte y=-1; int z=-1; byte dir=255;
	 * short id=297 (bread); byte count=1; short damage=0. */
	static const uint8_t golden[] = {0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x29, 0x01, 0x00, 0x00};
	struct mp_item_stack bread
		= {.present = true, .id = ITEM_BREAD, .count = 1, .damage = 0};
	struct mp_frame frame;
	assert(mp_encode_place(&frame, -1, 255, -1, 255, &bread));
	assert(frame.length == sizeof golden
		   && !memcmp(frame.data, golden, sizeof golden));
	puts("PASS U5 online bread with no target = Beta Packet15 (-1,-1,-1,255) bytes");
}

int main(void) {
	test_fixture_dir("test_use_item_no_target", s_world, sizeof s_world);
	test_remote_bytes();
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(s_world);
	MKDIR(test_fixture_join(s_world, "/region"));
	string_t level;
	string_init_set_str(level, s_world);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 0.5;
	s.player.y = 80.0;
	s.player.z = 0.5;
	inventory_set_hotbar(&s.player.inventory, 0);

	test_eat_no_target();
	test_full_health_refused();
	test_throw_and_bow_no_target();
	test_block_target_unchanged();

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(s_world);
	puts("test_use_item_no_target: ALL OK");
	return 0;
}
