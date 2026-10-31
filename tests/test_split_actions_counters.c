/* S1-d (mp-split-0912/68): per-ACTOR accepted-action counters.
 * GPL-3.0-or-later.
 *
 * The split_actions_6m console profile stands or falls on one server-side
 * claim: struct server_player.actions (and its main-thread mirror,
 * server_local.actor_actions) counts an action for the actor that PERFORMED
 * it, on the accepted path ONLY. If that were wrong in either direction the
 * hardware profile would be worthless -- a counter that ticks on refused
 * requests turns a broken build into a PASS, and one that misses accepted
 * actions turns an honest run into a FAIL.
 *
 * So this drives the REAL slot-1 mailbox (svin_rpc_send_from +
 * server_local_test_process_messages) against an admitted guest body and
 * reads the SERVER's own counters back, per verb and in both directions:
 *   - an accepted host action moves ONLY act0, an accepted guest action moves
 *     ONLY act1;
 *   - every refusal the handlers already implement (a dig that removed
 *     nothing, an out-of-range cell, an invalid hotbar slot, a place with an
 *     empty hand, a drop with an empty hand, a verb while the acting body owns
 *     a different window, a stale generation, and a withdrawn body admission)
 *     moves NOTHING;
 *   - the published mirror always equals the body's own tally;
 *   - a NEW guest body starts the slot-1 mirror at zero, so a rejoin cannot
 *     inherit the previous occupant's counts.
 */
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "block/blocks.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_guest.h"
#include "network/server_local_state.h"
#include "network/server_world.h"

#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MAKE_DIR(p) mkdir(p, 0700)
#endif

/* The work column, well clear of both bodies so no placement is ever refused
 * by the two-body obstruction sweep (6bde02c7) for a reason this test did not
 * intend. Inside chunk (0,0), which is the only one loaded. */
#define WX 8
#define WY 70
#define WZ 8

static struct server_local s_server;
static struct server_rpc_source s_guest_src;

/* A window that is NOT the acting body's base inventory. Never dereferenced:
 * every handler under test compares active_inventory by POINTER before it
 * would read anything out of it, which is exactly the refusal being pinned. */
static struct inventory s_other_window;
static unsigned s_guest_slot_replies;
static unsigned s_pickup_events;
static uint32_t s_pickup_collector;
static uint32_t s_rpc_order_hash = 2166136261u;

static void observe_reply(const struct client_rpc* call, void* user) {
	(void)user;
	s_rpc_order_hash = (s_rpc_order_hash ^ (uint32_t)call->type) * 16777619u;
	if(call->type == CRPC_INVENTORY_SLOT)
		s_guest_slot_replies++;
	if(call->type == CRPC_PICKUP_ITEM) {
		s_pickup_events++;
		s_pickup_collector = call->payload.pickup_item.collector_id;
	}
}

static void clear_inventory(struct server_player* player) {
	for(size_t k = 0; k < INVENTORY_SIZE; k++)
		inventory_clear_slot(&player->inventory, k);
}

static void fill_collectible_slots(struct server_player* player,
								   uint16_t id) {
	clear_inventory(player);
	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		player->inventory.items[INVENTORY_SLOT_HOTBAR + k]
			= (struct item_data) {.id = id, .count = 64};
	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		player->inventory.items[INVENTORY_SLOT_MAIN + k]
			= (struct item_data) {.id = id, .count = 64};
}

static void make_item(struct entity* item, uint16_t id, uint8_t count,
					  float x, float y, float z) {
	memset(item, 0, sizeof(*item));
	entity_item(server_local_gen_id(&s_server), item, true, &s_server.world,
				(struct item_data) {.id = id, .count = count});
	item->teleport(item, (vec3) {x, y, z});
}

static uint32_t mix_u32(uint32_t hash, uint32_t value) {
	for(unsigned k = 0; k < 4; k++)
		hash = (hash ^ ((value >> (k * 8)) & 0xffu)) * 16777619u;
	return hash;
}

static void ignore_reply(const struct client_rpc* call, void* user) {
	(void)call;
	(void)user;
}

static void pump(void) {
	server_local_test_process_messages(&s_server);
	clin_drain_discard();
}

static void snapshot(uint32_t out[2][SERVER_ACTION_COUNT]) {
	memcpy(out, s_server.actor_actions, sizeof(s_server.actor_actions));
}

/* Every counter of both owners is exactly where it was. */
static void assert_unchanged(const uint32_t before[2][SERVER_ACTION_COUNT]) {
	assert(!memcmp(before, s_server.actor_actions,
				   sizeof(s_server.actor_actions)));
}

/* The published mirror is the body's own tally, for both owners. */
static void assert_mirrors_bodies(void) {
	for(unsigned k = 0; k < SERVER_ACTION_COUNT; k++) {
		assert(s_server.actor_actions[0][k] == s_server.player.actions[k]);
		assert(s_server.guest_actor == NULL
			   || s_server.actor_actions[1][k]
				   == s_server.guest_actor->actions[k]);
	}
}

static uint8_t block_at(int x, int y, int z) {
	struct block_data blk;
	bool ok = server_world_get_block(&s_server.world, x, y, z, &blk);
	assert(ok);
	return blk.type;
}

/* server_world_set_block returns FALSE for a no-op write -- Beta
 * Chunk.setBlockIDWithMetadata is a no-op for an identical id+metadata, and
 * World.setBlockAndMetadataWithNotify reports that as "nothing changed". The
 * postcondition this fixture needs is the RESULTING block, so assert that. */
static void set_block(int x, int y, int z, uint8_t type) {
	(void)server_world_set_block(&s_server.worlds[0], x, y, z,
								 (struct block_data) {.type = type,
													  .metadata = 0});
	clin_drain_discard();
	assert(block_at(x, y, z) == type);
}

static bool send_guest(struct server_rpc* call) {
	return svin_rpc_send_from(s_guest_src, call);
}

static struct entity* find_new_item(const uint32_t* before, size_t count) {
	dict_entity_it_t it;
	dict_entity_it(it, s_server.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		bool old = false;
		for(size_t k = 0; k < count; k++)
			old = old || before[k] == e->id;
		if(e->type == ENTITY_ITEM && !old)
			return e;
		dict_entity_next(it);
	}
	assert(0 && "block drop did not create an item entity");
	return NULL;
}

static void setup(void) {
	char path[256];
	blocks_init();
	items_init();
	recipe_init();
	dict_entity_init(gstate.entities);
	/* Both mailboxes, exactly as main.c boots them: svin carries the SRPCs
	 * this test posts, clin the CRPC replies they generate. */
	assert(svin_init());
	assert(clin_init());
	clin_test_set_rpc_observer(observe_reply, NULL);
	test_fixture_dir("test_split_actions_counters", path, sizeof(path));
	assert(MAKE_DIR(path) == 0);
	server_local_init(&s_server, false, false, false, false, false, false, false);
	string_set_str(s_server.level_name, path);
	assert(level_archive_create_new(&s_server.level, s_server.level_name, 12345,
									(vec3) {4, 72, 4}));
	assert(level_archive_save(&s_server.level));
	server_world_create(&s_server.world, s_server.level_name,
						WORLD_DIM_OVERWORLD, 12345, true);
	s_server.worlds[0].session = &s_server; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s_server))
		abort();

	struct server_player* host = &s_server.player;
	assert(host->inventory.items);
	host->active_inventory = &host->inventory;
	host->has_pos = host->finished_loading = true;
	host->x = 4;
	host->y = 72;
	host->z = 4;
	host->health = MAX_PLAYER_HEALTH;
	host->dimension = WORLD_DIM_OVERWORLD;

	/* A fresh server publishes a zeroed pair -- server_local is an
	 * uninitialized automatic, so this is the explicit-init guard from
	 * server_local_init, not a tautology. */
	for(unsigned p = 0; p < 2; p++)
		for(unsigned k = 0; k < SERVER_ACTION_COUNT; k++)
			assert(s_server.actor_actions[p][k] == 0);
	for(unsigned k = 0; k < SERVER_ACTION_COUNT; k++)
		assert(host->actions[k] == 0);

	assert(server_local_guest_create(&s_server, &s_guest_src));
	assert(s_guest_src.slot == 1 && s_guest_src.generation);
	struct server_player* guest = s_server.guest_actor;
	assert(guest && guest != host);
	guest->has_pos = guest->finished_loading = true;
	/* Inside chunk (0,0) -- the only column this fixture loads -- and four
	 * blocks clear of the work column, so neither body ever obstructs a
	 * placement and the guest's own item drop lands somewhere resident (a
	 * drop stranded in an unloaded chunk is refused by guest_leave's
	 * entity-owner preflight, which is correct and not what is under test). */
	guest->x = 12;
	guest->y = 72;
	guest->z = 12;
	guest->health = MAX_PLAYER_HEALTH;
	guest->dimension = WORLD_DIM_OVERWORLD;
	for(unsigned k = 0; k < SERVER_ACTION_COUNT; k++)
		assert(guest->actions[k] == 0);

	/* The queue boundary itself is gated on the experimental switch, and the
	 * body admission on guest_gameplay -- both are prerequisites for any of
	 * the verbs below to be routed at all (test_local_guest pins that gate;
	 * here it is setup). */
	assert(server_local_set_split_enabled(&s_server, true));
	s_server.guest_entity_id = server_local_gen_id(&s_server);
	s_server.guest_gameplay = true;

	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s_server.world, 0, 0, &chunk) && chunk);
	set_block(WX, WY - 1, WZ, BLOCK_STONE); /* the support the place clicks */
	set_block(WX, WY + 1, WZ, BLOCK_AIR);
	clin_drain_discard();
}

static void test_pickup_owner_edges(void) {
	struct server_player* host = &s_server.player;
	struct server_player* guest = s_server.guest_actor;
	const size_t slot = INVENTORY_SLOT_HOTBAR;
	struct entity item;
	set_block(12, WY, 12, BLOCK_STONE);

	/* Host behavior is serialized before guest routing is exercised. These
	 * four cases pin the unchanged state and RPC order for full, partial,
	 * delay, and already-destroying items. */
	struct server_player* saved_guest = s_server.guest_actor;
	s_server.guest_actor = NULL;
	s_server.guest_gameplay = false;
	host->x = 12; host->y = 72; host->z = 12;
	uint32_t golden = 2166136261u;
	s_rpc_order_hash = 2166136261u;
	fill_collectible_slots(host, BLOCK_STONE);
	make_item(&item, BLOCK_DIRT, 3, 12, 71.7F, 12);
	assert(!item.tick_server(&item, &s_server));
	golden = mix_u32(golden, item.data.item.item.count);
	golden = mix_u32(golden, (uint32_t)item.delay_destroy);
	fill_collectible_slots(host, BLOCK_STONE);
	host->inventory.items[slot] = (struct item_data) {.id = BLOCK_DIRT, .count = 63};
	make_item(&item, BLOCK_DIRT, 3, 12, 71.7F, 12);
	assert(!item.tick_server(&item, &s_server));
	golden = mix_u32(golden, host->inventory.items[slot].count);
	golden = mix_u32(golden, item.data.item.item.count);
	clear_inventory(host);
	make_item(&item, BLOCK_DIRT, 1, 12, 71.7F, 12);
	item.data.item.pickup_delay = 2;
	assert(!item.tick_server(&item, &s_server));
	golden = mix_u32(golden, (uint32_t)item.data.item.pickup_delay);
	golden = mix_u32(golden, host->inventory.items[slot].count);
	make_item(&item, BLOCK_DIRT, 1, 12, 71.7F, 12);
	item.delay_destroy = 1;
	assert(!item.tick_server(&item, &s_server));
	golden = mix_u32(golden, (uint32_t)item.delay_destroy);
	golden = mix_u32(golden, host->inventory.items[slot].count);
	golden = mix_u32(golden, s_rpc_order_hash);
	/* itement-fix-0925 re-pin: 0xc1dbc53f -> 0x4a5b3efe. Single cause: the
	 * partial-fill case no longer emits the random.pop sound RPC -- Beta's
	 * addItemStackToInventory returns false when a remainder is left, so
	 * onCollideWithPlayer plays no pop [InventoryPlayer.java:143-149;
	 * EntityItem.java:113,122]. Proof: restoring ONLY that host partial pop
	 * in entity_item.c restores 0xc1dbc53f. */
	assert(golden == UINT32_C(0x4a5b3efe));
	printf("PASS host pickup state/RPC golden full+partial+delay+dead = %08x\n",
		   golden);
	fflush(stdout);
	clin_drain_discard();
	if(getenv("CAVEWII_HOST_PICKUP_GOLDEN_ONLY"))
		return;

	/* Restore the admitted guest. Host is first when both overlap; a full or
	 * partial host leaves the live remainder for the guest in the same tick. */
	s_server.guest_actor = saved_guest;
	s_server.guest_gameplay = true;
	guest->x = host->x = 12; guest->y = host->y = 72; guest->z = host->z = 12;
	clear_inventory(host); clear_inventory(guest);
	make_item(&item, BLOCK_DIRT, 1, 12, 71.7F, 12);
	assert(!item.tick_server(&item, &s_server));
	assert(host->inventory.items[slot].count == 1
		   && guest->inventory.items[slot].id == 0 && item.delay_destroy == 1);
	clin_drain_discard();

	fill_collectible_slots(host, BLOCK_STONE); clear_inventory(guest);
	make_item(&item, BLOCK_DIRT, 1, 12, 71.7F, 12);
	assert(!item.tick_server(&item, &s_server));
	assert(guest->inventory.items[slot].count == 1 && item.delay_destroy == 1);
	clin_drain_discard();

	fill_collectible_slots(host, BLOCK_STONE); clear_inventory(guest);
	host->inventory.items[slot] = (struct item_data) {.id = BLOCK_DIRT, .count = 63};
	make_item(&item, BLOCK_DIRT, 3, 12, 71.7F, 12);
	assert(!item.tick_server(&item, &s_server));
	assert(host->inventory.items[slot].count == 64
		   && guest->inventory.items[slot].count == 2
		   && item.data.item.item.count == 0 && item.delay_destroy == 1);
	clin_drain_discard();

	fill_collectible_slots(host, BLOCK_STONE);
	fill_collectible_slots(guest, BLOCK_STONE);
	make_item(&item, BLOCK_DIRT, 1, 12, 71.7F, 12);
	assert(!item.tick_server(&item, &s_server));
	assert(item.data.item.item.count == 1 && item.delay_destroy <= 0);
	clin_drain_discard();

	/* Strict Beta boundary: face contact is outside; one float step inward is
	 * inside. Delay reaches zero once, and withdrawn/loading/dead guests never
	 * receive the stack. */
	host->x = 4; host->y = 72; host->z = 4;
	clear_inventory(host); clear_inventory(guest);
	make_item(&item, BLOCK_DIRT, 1, 13.425F, 71.7F, 12);
	assert(!item.tick_server(&item, &s_server));
	assert(item.data.item.item.count == 1);
	make_item(&item, BLOCK_DIRT, 1, nextafterf(13.425F, 12.0F), 71.7F, 12);
	item.data.item.pickup_delay = 2;
	assert(!item.tick_server(&item, &s_server));
	assert(item.data.item.pickup_delay == 1 && guest->inventory.items[slot].id == 0);
	item.teleport(&item, (vec3) {guest->x, 71.7F, guest->z});
	assert(!item.tick_server(&item, &s_server));
	assert(guest->inventory.items[slot].count == 1);
	clin_drain_discard();

	for(unsigned state = 0; state < 4; state++) {
		clear_inventory(guest);
		make_item(&item, BLOCK_DIRT, 1, 12, 71.7F, 12);
		if(state == 0) s_server.guest_gameplay = false;
		if(state == 1) guest->finished_loading = false;
		if(state == 2) guest->health = 0;
		if(state == 3) s_server.guest_actor = NULL;
		assert(!item.tick_server(&item, &s_server));
		assert(item.data.item.item.count == 1
			   && guest->inventory.items[slot].id == 0);
		s_server.guest_actor = guest;
		s_server.guest_gameplay = true;
		guest->finished_loading = true;
		guest->health = MAX_PLAYER_HEALTH;
	}
	clear_inventory(host); clear_inventory(guest);
	puts("PASS host-first, remainder routing, strict overlap/delay boundary, "
		 "full and ineligible guest pickup edges");
}

static void test_dig(void) {
	struct server_player* host = &s_server.player;
	struct server_player* guest = s_server.guest_actor;
	struct server_rpc dig = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.finished = true, .x = WX, .y = WY, .z = WZ}};

	/* (a) an accepted HOST dig moves only act0. */
	set_block(WX, WY, WZ, BLOCK_STONE);
	assert(svin_rpc_send(&dig));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_AIR);
	assert(host->actions[SERVER_ACTION_DIG] == 1);
	assert(guest->actions[SERVER_ACTION_DIG] == 0);

	/* (b) an accepted GUEST dig moves only act1. */
	set_block(WX, WY, WZ, BLOCK_STONE);
	assert(send_guest(&dig));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_AIR);
	assert(guest->actions[SERVER_ACTION_DIG] == 1);
	assert(host->actions[SERVER_ACTION_DIG] == 1);
	assert_mirrors_bodies();

	/* (c) a dig START is a progress ping, not an action. */
	uint32_t before[2][SERVER_ACTION_COUNT];
	snapshot(before);
	set_block(WX, WY, WZ, BLOCK_STONE);
	dig.payload.block_dig.finished = false;
	assert(send_guest(&dig));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_STONE); /* nothing was removed */
	assert_unchanged(before);
	dig.payload.block_dig.finished = true;

	/* (d) an out-of-range cell is refused before any world write. */
	dig.payload.block_dig.y = WORLD_HEIGHT;
	assert(send_guest(&dig));
	pump();
	assert_unchanged(before);
	dig.payload.block_dig.y = -1;
	assert(send_guest(&dig));
	pump();
	assert_unchanged(before);
	dig.payload.block_dig.y = WY;

	/* (e) a body that is still loading cannot act. */
	guest->finished_loading = false;
	assert(send_guest(&dig));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_STONE);
	assert_unchanged(before);
	guest->finished_loading = true;
	puts("PASS dig counts the block-gone edge, per actor; dig-start, an "
		 "out-of-range cell and an unloaded body count nothing");
}

static void test_hotbar_and_use(void) {
	struct server_player* host = &s_server.player;
	struct server_player* guest = s_server.guest_actor;
	uint32_t before[2][SERVER_ACTION_COUNT];

	struct server_rpc hot = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot.slot = 3};
	assert(send_guest(&hot));
	pump();
	assert(inventory_get_hotbar(&guest->inventory) == 3);
	assert(guest->actions[SERVER_ACTION_HOTBAR] == 1);
	assert(host->actions[SERVER_ACTION_HOTBAR] == 0);
	assert(svin_rpc_send(&hot));
	pump();
	assert(host->actions[SERVER_ACTION_HOTBAR] == 1);

	/* An out-of-range slot changes no selection and counts nothing. */
	snapshot(before);
	hot.payload.hotbar_slot.slot = INVENTORY_SIZE_HOTBAR;
	assert(send_guest(&hot));
	pump();
	assert(inventory_get_hotbar(&guest->inventory) == 3);
	assert_unchanged(before);
	hot.payload.hotbar_slot.slot = 0;
	assert(send_guest(&hot));
	pump();
	assert(inventory_get_hotbar(&guest->inventory) == 0);
	assert(guest->actions[SERVER_ACTION_HOTBAR] == 2);

	/* USE is the one slot that counts an accepted VERB rather than a world
	 * effect: a block item (or an empty hand) used into open air is
	 * legitimately inert in Beta, and the routing is what the profile proves. */
	struct server_rpc use = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 0.0F}};
	assert(send_guest(&use));
	pump();
	assert(guest->actions[SERVER_ACTION_USE] == 1);
	assert(host->actions[SERVER_ACTION_USE] == 0);

	/* ...but a body whose live window is NOT its base inventory is refused,
	 * so the accepted-path-only rule still holds for it. */
	snapshot(before);
	guest->active_inventory = &s_other_window;
	assert(send_guest(&use));
	pump();
	assert_unchanged(before);
	guest->active_inventory = &guest->inventory;
	assert_mirrors_bodies();
	puts("PASS hotbar counts a real selection change and use counts the "
		 "accepted air-use dispatch, per actor; an invalid slot and a "
		 "non-base window count nothing");
}

static void test_place_and_drop(void) {
	struct server_player* host = &s_server.player;
	struct server_player* guest = s_server.guest_actor;
	uint32_t before[2][SERVER_ACTION_COUNT];
	size_t slot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&guest->inventory);

	/* (a) an empty hand places nothing and counts nothing. */
	set_block(WX, WY, WZ, BLOCK_AIR);
	snapshot(before);
	guest->inventory.items[slot] = (struct item_data) {0};
	struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = WX, .y = WY - 1, .z = WZ,
								.side = SIDE_TOP}};
	assert(send_guest(&place));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_AIR);
	assert_unchanged(before);

	/* (b) Collect a REAL block drop, then place that same stack through the
	 * real slot-1 mailbox. No fixture stock may make this path pass. */
	uint32_t before_ids[256];
	size_t before_count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s_server.entities);
	while(!dict_entity_end_p(it) && before_count < 256) {
		before_ids[before_count++] = dict_entity_ref(it)->key;
		dict_entity_next(it);
	}
	struct block_data dropped_block = {.type = BLOCK_DIRT};
	struct block_info dropped_info
		= {.x = 12, .y = WY, .z = 12, .block = &dropped_block};
	set_block(12, WY, 12, BLOCK_STONE);
	server_local_spawn_block_drops_after_break(&s_server, &dropped_info);
	struct entity* dropped = find_new_item(before_ids, before_count);
	assert(dropped->data.item.item.id == BLOCK_DIRT
		   && dropped->data.item.pickup_delay == 10);
	dropped->teleport(dropped,
		(vec3) {guest->x, guest->y - 0.3F, guest->z});
	unsigned replies_before = s_guest_slot_replies;
	unsigned pickups_before = s_pickup_events;
	for(int tick = 0; tick < 10; tick++)
		assert(!dropped->tick_server(dropped, &s_server));
	assert(guest->inventory.items[slot].id == BLOCK_DIRT
		   && guest->inventory.items[slot].count == 1);
	assert(host->inventory.items[INVENTORY_SLOT_HOTBAR].id == 0);
	assert(s_guest_slot_replies == replies_before + 1);
	assert(s_pickup_events == pickups_before + 1);
	assert(s_pickup_collector == s_server.guest_entity_id);
	assert(send_guest(&place));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_DIRT);
	assert(guest->inventory.items[slot].id == 0); /* collected item consumed */
	assert(guest->actions[SERVER_ACTION_PLACE] == 1);
	assert(host->actions[SERVER_ACTION_PLACE] == 0);

	/* (c) the same click into an occupied cell places nothing. */
	snapshot(before);
	assert(send_guest(&place));
	pump();
	assert(guest->inventory.items[slot].id == 0); /* nothing consumed */
	assert_unchanged(before);

	/* (d) DROP counts only when an item entity really left the hand. */
	set_block(WX, WY, WZ, BLOCK_AIR);
	guest->inventory.items[slot]
		= (struct item_data) {.id = BLOCK_DIRT, .count = 6};
	struct server_rpc drop = {.type = SRPC_DROP_ITEM,
		.payload.drop_item.all = false};
	size_t entities_before = dict_entity_size(s_server.entities);
	assert(send_guest(&drop));
	pump();
	assert(guest->actions[SERVER_ACTION_DROP] == 1);
	assert(host->actions[SERVER_ACTION_DROP] == 0);
	assert(dict_entity_size(s_server.entities) == entities_before + 1);
	assert(guest->inventory.items[slot].count == 5);

	snapshot(before);
	guest->inventory.items[slot] = (struct item_data) {0};
	assert(send_guest(&drop));
	pump();
	assert(dict_entity_size(s_server.entities) == entities_before + 1);
	assert_unchanged(before);
	assert_mirrors_bodies();
	puts("PASS guest collects a real delayed block drop with slot-1 reply and "
		 "collector identity, then places and consumes it through the owned RPC");
}

static void test_routing_refusals(void) {
	struct server_player* guest = s_server.guest_actor;
	uint32_t before[2][SERVER_ACTION_COUNT];
	snapshot(before);
	set_block(WX, WY, WZ, BLOCK_STONE);

	/* A structurally valid request from a STALE generation is queued and then
	 * dropped at the dispatcher: no body acts, so no counter moves. This is
	 * the case that would silently turn a routing bug into a PASS. */
	struct server_rpc_source stale
		= {.slot = 1, .generation = s_guest_src.generation + 7};
	struct server_rpc dig = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.finished = true, .x = WX, .y = WY, .z = WZ}};
	assert(svin_rpc_send_from(stale, &dig));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_STONE);
	assert_unchanged(before);

	/* A withdrawn BODY admission (a window-only attached guest) is refused by
	 * server_local_guest_action_allowed for the same reason. */
	s_server.guest_gameplay = false;
	assert(send_guest(&dig));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_STONE);
	assert_unchanged(before);
	s_server.guest_gameplay = true;

	/* ...and with admission restored the identical request is accepted, so
	 * the refusals above were the gate, not a broken fixture. */
	assert(send_guest(&dig));
	pump();
	assert(block_at(WX, WY, WZ) == BLOCK_AIR);
	assert(guest->actions[SERVER_ACTION_DIG]
		   == before[1][SERVER_ACTION_DIG] + 1);
	assert(s_server.actor_actions[0][SERVER_ACTION_DIG]
		   == before[0][SERVER_ACTION_DIG]);
	assert_mirrors_bodies();
	puts("PASS a stale generation and a withdrawn body admission move no "
		 "counter; the same request lands once admission is restored");
}

static void test_rejoin_clears_the_mirror(void) {
	struct server_player* guest = s_server.guest_actor;
	/* The tally under test is real, not incidentally zero. */
	assert(guest->actions[SERVER_ACTION_DIG] > 0
		   && guest->actions[SERVER_ACTION_HOTBAR] > 0
		   && guest->actions[SERVER_ACTION_PLACE] > 0
		   && guest->actions[SERVER_ACTION_USE] > 0
		   && guest->actions[SERVER_ACTION_DROP] > 0);
	uint32_t host_before[SERVER_ACTION_COUNT];
	memcpy(host_before, s_server.player.actions, sizeof(host_before));

	guest->inventory.picked_item = (struct item_data) {0};
	assert(server_local_guest_settled(&s_server));
	assert(server_local_guest_leave(&s_server, s_guest_src));
	assert(!s_server.guest_actor);

	assert(server_local_guest_create(&s_server, &s_guest_src));
	struct server_player* fresh = s_server.guest_actor;
	assert(fresh);
	fresh->inventory.reply = ignore_reply;
	fresh->inventory.reply_user = &s_server;
	/* A new body carries no tally, and -- the part a pointer-free mirror could
	 * get wrong -- the PUBLISHED slot-1 row was cleared with it. */
	for(unsigned k = 0; k < SERVER_ACTION_COUNT; k++) {
		assert(fresh->actions[k] == 0);
		assert(s_server.actor_actions[1][k] == 0);
	}
	/* The HOST's own tally is untouched by the guest lifetime boundary. */
	assert(!memcmp(host_before, s_server.player.actions, sizeof(host_before)));
	assert(!memcmp(host_before, s_server.actor_actions[0], sizeof(host_before)));
	puts("PASS a rejoining guest starts at zero and clears the published "
		 "slot-1 mirror; the host tally survives the boundary");
}

int main(void) {
	setup();
	test_pickup_owner_edges();
	if(getenv("CAVEWII_HOST_PICKUP_GOLDEN_ONLY"))
		return 0;
	test_dig();
	test_hotbar_and_use();
	test_place_and_drop();
	test_routing_refusals();
	test_rejoin_clears_the_mirror();
	puts("test_split_actions_counters: PASS");
	return 0;
}
