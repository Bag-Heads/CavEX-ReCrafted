/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Goldens hand-derived from 740c583, never recorded from CaveWii output.
 * All citations are 1.7.3-LTS/src/minecraft/net/minecraft/src unless marked
 * server (the sibling minecraft_server source tree at the same commit). */
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>
#include "graphics/render_item.h"
#include "platform/texture.h"
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "support/test_fixture_dir.h"
#include "item/items/item_map.h"
#include "network/map_data_archive.h"
#include "network/map_idcounts.h"
#include "network/level_archive.h"
#include "item/items.h"
#include "item/recipe.h"
#include "game/game_state.h"
#include "game/gui/container_compact.h"
#include "network/server_remote_internal.h"
#include "item/window_container.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_local.h"
#include "network/server_interface.h"
#include "network/server_local_state.h"

#define STACK(id_, count_, damage_) { .id = id_, .count = count_, .durability = damage_ }
struct click_case {
	const char* name;
	bool right;
	struct item_data slot, cursor, slot_after, cursor_after;
};

static const struct click_case clicks[] = {
	/* Container.java:84-88,102-110: return PRE-click slot; ceil-half pickup. */
	{"left pickup", false, STACK(1,9,0), {0}, {0}, STACK(1,9,0)},
	{"right odd pickup", true, STACK(1,9,0), {0}, STACK(1,4,0), STACK(1,5,0)},
	{"right even pickup", true, STACK(1,8,0), {0}, STACK(1,4,0), STACK(1,4,0)},
	{"right last item", true, STACK(1,1,0), {0}, {0}, STACK(1,1,0)},
	{"empty no-op", false, {0}, {0}, {0}, {0}},
	{"empty right no-op", true, {0}, {0}, {0}, {0}},
	/* Container.java:90-100: empty destination; right places exactly one. */
	{"left place", false, {0}, STACK(1,9,0), STACK(1,9,0), {0}},
	{"right place", true, {0}, STACK(1,9,0), STACK(1,1,0), STACK(1,8,0)},
	/* Container.java:112-130: incompatible stacks swap for BOTH buttons. */
	{"left swap", false, STACK(3,7,0), STACK(1,9,0), STACK(1,9,0), STACK(3,7,0)},
	{"right swap", true, STACK(3,7,0), STACK(1,9,0), STACK(1,9,0), STACK(3,7,0)},
	{"subtype swap", true, STACK(35,7,1), STACK(35,9,2), STACK(35,9,2), STACK(35,7,1)},
	{"subtype merge", false, STACK(35,7,1), STACK(35,9,1), STACK(35,16,1), {0}},
	/* ItemCoal.java:6, ItemDye.java:9; damage must match for subtypes. */
	{"coal variants", false, STACK(263,3,0), STACK(263,4,1), STACK(263,4,1), STACK(263,3,0)},
	{"dye variants", true, STACK(351,3,1), STACK(351,4,2), STACK(351,4,2), STACK(351,3,1)},
	/* Container.java:112: damage is IGNORED for non-subtypes. Destination
	 * metadata survives a merge; limit-one worn tools do NOT swap. */
	{"plain metadata merge", false, STACK(1,7,2), STACK(1,9,3), STACK(1,16,2), {0}},
	{"worn tool left", false, STACK(257,1,42), STACK(257,1,91), STACK(257,1,42), STACK(257,1,91)},
	{"worn tool right", true, STACK(257,1,42), STACK(257,1,91), STACK(257,1,42), STACK(257,1,91)},
	/* Container.java:113-127; Item.java:375 (cookie=8), ItemSnowball.java:6
	 * and ItemEgg.java:6 (=16), ItemSign.java:6 and ItemBucket.java:8 (=1). */
	{"cookie limit", false, STACK(357,7,0), STACK(357,3,0), STACK(357,8,0), STACK(357,2,0)},
	{"egg limit", false, STACK(344,15,0), STACK(344,3,0), STACK(344,16,0), STACK(344,2,0)},
	{"snowball limit", true, STACK(332,15,0), STACK(332,3,0), STACK(332,16,0), STACK(332,2,0)},
	{"sign limit", true, STACK(323,1,0), STACK(323,1,0), STACK(323,1,0), STACK(323,1,0)},
	{"bucket limit", false, STACK(325,1,0), STACK(325,1,0), STACK(325,1,0), STACK(325,1,0)},
	{"full left", false, STACK(1,64,0), STACK(1,9,0), STACK(1,64,0), STACK(1,9,0)},
	{"full right", true, STACK(1,64,0), STACK(1,9,0), STACK(1,64,0), STACK(1,9,0)},
};

static bool equal(struct item_data a, struct item_data b) {
	return a.id == b.id && (!a.id || (a.count == b.count && a.durability == b.durability));
}

static struct client_rpc replies[INVENTORY_SIZE + 1];
static size_t reply_count;
static struct server_remote remote;

static struct mp_frame outgoing(struct server_rpc* call) {
	struct server_remote_impl* impl = remote.impl;
	assert(!impl->frame_count);
	remote_process_srpc(call, &remote);
	assert(impl->frame_count == 1);
	struct mp_frame frame = impl->frames[impl->frame_head].frame;
	impl->frame_head = (impl->frame_head + 1) % REMOTE_QUEUE_CAPACITY;
	impl->frame_count--;
	return frame;
}

static void put16(uint8_t* bytes, unsigned value) {
	bytes[0] = value >> 8; bytes[1] = value;
}

static unsigned get16(const uint8_t* bytes) {
	return ((unsigned)bytes[0] << 8) | bytes[1];
}

static size_t stack_bytes(uint8_t* bytes, struct item_data item) {
	put16(bytes, item.id ? item.id : 0xffff);
	if(!item.id) return 2;
	bytes[2] = item.count;
	put16(bytes + 3, item.durability);
	return 5;
}

/* Test peer's Packet102 parser: the shipping client correctly has no inbound
 * C->S decoder. Field order/width is Packet102WindowClick.java:36-59. */
static struct server_rpc click_wire(struct server_rpc call) {
	struct mp_frame frame = outgoing(&call);
	bool drop = call.type == SRPC_WINDOW_DROP;
	uint8_t window = drop ? call.payload.window_drop.window : call.payload.window_click.window;
	unsigned slot = drop ? (uint16_t)-999 : call.payload.window_click.slot;
	bool right = drop ? call.payload.window_drop.right : call.payload.window_click.right_click;
	bool shift = !drop && call.payload.window_click.quick_move;
	unsigned action = drop ? call.payload.window_drop.action_id : call.payload.window_click.action_id;
	struct item_data expected = drop ? (struct item_data){0} : call.payload.window_click.expected;
	uint8_t want[13] = {0x66, window, slot >> 8, slot, right, action >> 8, action, shift};
	size_t length = 8 + stack_bytes(want + 8, expected);
	assert(frame.length == length && !memcmp(frame.data, want, length));
	const uint8_t* p = frame.data;
	struct server_rpc decoded = {.type = get16(p + 2) == (uint16_t)-999 ? SRPC_WINDOW_DROP : SRPC_WINDOW_CLICK};
	struct item_data stack = {0};
	if(get16(p + 8) != 0xffff)
		stack = (struct item_data){.id = get16(p + 8), .count = p[10], .durability = get16(p + 11)};
	if(decoded.type == SRPC_WINDOW_DROP) {
		decoded.payload.window_drop.window = p[1];
		decoded.payload.window_drop.right = p[4] != 0;
		decoded.payload.window_drop.action_id = get16(p + 5);
		decoded.payload.window_drop.expected = stack;
	} else {
		decoded.payload.window_click.window = p[1];
		decoded.payload.window_click.slot = get16(p + 2);
		decoded.payload.window_click.right_click = p[4] != 0;
		decoded.payload.window_click.action_id = get16(p + 5);
		decoded.payload.window_click.quick_move = p[7] != 0;
		decoded.payload.window_click.expected = stack;
	}
	return decoded;
}

static void incoming(const uint8_t* bytes, size_t length) {
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	for(size_t i = 0; i < length; i++) {
		enum mp_decode_result result = mp_decoder_feed(&decoder, bytes[i], &event.packet);
		assert(result == (i + 1 == length ? MP_DECODE_EVENT : MP_DECODE_MORE));
	}
	assert(process_event(&remote, &event));
	mp_decoder_destroy(&decoder);
}

/* Encode the simulated peer's reply, then use the REAL remote decoder and
 * dispatcher. Goldens below independently pin each state and reply order. */
static void reply_wire(const struct client_rpc* call, uint8_t window, struct window_container* wc) {
	uint8_t bytes[4 + 128 * 5];
	size_t length;
	gstate.players[0].windows[window] = wc;
	((struct server_remote_impl*)remote.impl)->active_window = window;
	if(call->type == CRPC_WINDOW_TRANSACTION) {
		bytes[0] = 0x6a; bytes[1] = window;
		put16(bytes + 2, call->payload.window_transaction.action_id);
		bytes[4] = call->payload.window_transaction.accepted;
		length = 5;
	} else if(call->type == CRPC_WINDOW_ITEMS) {
		bytes[0] = 0x68; bytes[1] = window;
		put16(bytes + 2, call->payload.window_items.count);
		length = 4;
		for(size_t i = 0; i < call->payload.window_items.count; i++)
			length += stack_bytes(bytes + length, call->payload.window_items.items[i]);
	} else {
		assert(call->type == CRPC_INVENTORY_SLOT);
		bytes[0] = 0x67;
		bytes[1] = call->payload.inventory_slot.slot == SPECIAL_SLOT_PICKED_ITEM ? 0xff : window;
		put16(bytes + 2, call->payload.inventory_slot.slot == SPECIAL_SLOT_PICKED_ITEM
			? 0xffff : call->payload.inventory_slot.slot);
		length = 4 + stack_bytes(bytes + 4, call->payload.inventory_slot.item);
	}
	incoming(bytes, length);
	gstate.players[0].windows[window] = NULL;
}

static void capture(const struct client_rpc* call, void* user) {
	(void)user;
	assert(reply_count < sizeof(replies) / sizeof(*replies));
	replies[reply_count++] = *call;
}

static void ordinary_goldens(void) {
	static struct server_local server;
	const size_t capacities[] = {63, 90, 45};
	struct inventory_logic* logics[] = {&inventory_logic_chest,
		&inventory_logic_double_chest, &inventory_logic_player};
	const enum window_type types[] = {WINDOW_TYPE_CHEST, WINDOW_TYPE_CHEST, WINDOW_TYPE_INVENTORY};
	const uint8_t windows[] = {WINDOWC_CHEST, WINDOWC_IRON_CHEST, WINDOWC_INVENTORY};
	for(size_t fixture = 0; fixture < 3; fixture++) {
		for(size_t k = 0; k < sizeof(clicks) / sizeof(*clicks); k++) {
			const struct click_case* c = &clicks[k];
			struct inventory storage;
			struct inventory* inv = fixture == 2 ? &server.player.inventory : &storage;
			struct window_container wc;
			assert(inventory_create(inv, NULL, NULL, NULL, capacities[fixture], 0,0,0));
			assert(windowc_create(&wc, types[fixture], capacities[fixture]));
			inv->logic = logics[fixture];
			inv->reply = capture;
			server.player.active_inventory = inv;
			server.player.health = 160;
			server.player.has_pos = server.player.finished_loading = true;
			const size_t slot = fixture == 2 ? 9 : 0;
			inv->items[slot] = c->slot;
			inv->picked_item = c->cursor;
			windowc_slot_change(&wc, slot, c->slot);
			windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, c->cursor);
			uint16_t action;
			assert(windowc_new_action(&wc, &action, c->right, slot));
			struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
				.payload.window_click = {.window = windows[fixture], .slot = slot,
					.right_click = c->right, .action_id = action, .expected = c->slot}};
			reply_count = 0;
			/* Actual Packet102 round-trip followed by authoritative dispatch;
			 * provider_equivalence separately compares the direct local route. */
			struct server_rpc decoded = click_wire(call);
			server_local_process_window_for(&server, &server.player, &decoded);
			/* server NetServerHandler.java:498-504; EntityPlayerMP.java:310-314,
			 * 339-342: true ACK only, including no-ops. No successful echoes. */
			assert(reply_count == 1 && replies[0].type == CRPC_WINDOW_TRANSACTION);
			assert(replies[0].payload.window_transaction.accepted);
			reply_wire(&replies[0], windows[fixture], &wc);
			for(size_t s = 0; s < inv->capacity; s++) {
				struct item_data want = s == slot ? c->slot_after : (struct item_data){0};
				assert(equal(inv->items[s], want));
				assert(equal(windowc_get_latest(&wc)->items[s], want));
			}
			assert(equal(inv->picked_item, c->cursor_after));
			assert(equal(windowc_get_latest(&wc)->picked_item, c->cursor_after));
			inv->logic = NULL;
			inventory_destroy(inv, NULL);
			windowc_destroy(&wc);
			printf("PASS fixture=%zu %s\n", fixture, c->name);
		}
	}
}

struct cell { size_t slot; struct item_data item; };
struct transfer_case {
	const char* name;
	enum window_type type;
	uint8_t window;
	size_t capacity, slot, fill_begin, fill_end;
	struct cell before[5], after[5];
	size_t before_count, after_count;
	struct item_data cursor, returned;
};

static const struct transfer_case transfers[] = {
	/* ContainerChest.java:36-55, Container.java:192-248: reverse merge then
	 * first empty; forward storage insertion; full chest still returns source. */
	{"single reverse", 0, 3, 63, 0, 0,0,
		{{0,STACK(1,20,0)},{62,STACK(1,60,0)}},
		{{0,{0}},{62,STACK(1,64,0)},{61,STACK(1,16,0)}}, 2,3, {0}, STACK(1,20,0)},
	{"double reverse", 0, 4, 90, 27, 0,0,
		{{27,STACK(1,20,0)},{89,STACK(1,60,0)}},
		{{27,{0}},{89,STACK(1,64,0)},{88,STACK(1,16,0)}}, 2,3, {0}, STACK(1,20,0)},
	{"double storage forward", 0, 4, 90, 88, 0,0,
		{{88,STACK(1,20,0)},{0,STACK(1,60,0)}},
		{{88,{0}},{0,STACK(1,64,0)},{1,STACK(1,16,0)}}, 2,3, {0}, STACK(1,20,0)},
	{"full chest accepted", 0, 3, 63, 0, 27,63,
		{{0,STACK(1,12,0)}}, {{0,STACK(1,12,0)}}, 1,1, {0}, STACK(1,12,0)},
	{"partial chest", 0, 3, 63, 0, 27,63,
		{{0,STACK(1,12,0)},{62,STACK(1,60,0)}},
		{{0,STACK(1,8,0)},{62,STACK(1,64,0)}}, 2,2, {0}, STACK(1,12,0)},
	{"quick cookie limit", 0, 3, 63, 0, 0,0,
		{{0,STACK(357,8,0)},{62,STACK(357,7,0)}},
		{{0,{0}},{62,STACK(357,8,0)},{61,STACK(357,7,0)}}, 2,3, {0}, STACK(357,8,0)},
	{"quick subtype separates", 0, 3, 63, 0, 0,0,
		{{0,STACK(263,8,1)},{62,STACK(263,7,0)}},
		{{0,{0}},{61,STACK(263,8,1)}}, 2,2, {0}, STACK(263,8,1)},
	{"quick non-subtype preserves destination damage", 0, 3, 63, 0, 0,0,
		{{0,STACK(1,8,1)},{62,STACK(1,7,2)}},
		{{0,{0}},{62,STACK(1,15,2)}}, 2,2, {0}, STACK(1,8,1)},
	{"quick worn tools empty slot", 0, 3, 63, 0, 0,0,
		{{0,STACK(257,1,91)},{62,STACK(257,1,42)}},
		{{0,{0}},{61,STACK(257,1,91)}}, 2,2, {0}, STACK(257,1,91)},
	/* Container.java:67-79 does not require an empty cursor. */
	{"quick held cursor unchanged", 0, 3, 63, 0, 0,0,
		{{0,STACK(1,12,0)}}, {{0,{0}},{62,STACK(1,12,0)}}, 1,2, STACK(3,7,0), STACK(1,12,0)},
	/* ContainerPlayer.java:65-96: main -> hotbar, no auto-equip; unchanged
	 * transfer returns NULL (different from ContainerChest). */
	{"player main to hotbar", 255, 0, 45, 9, 0,0,
		{{9,STACK(306,1,42)}}, {{9,{0}},{36,STACK(306,1,42)}}, 1,2, {0}, STACK(306,1,42)},
	{"player full accepted null", 255, 0, 45, 9, 36,45,
		{{9,STACK(1,12,0)}}, {{9,STACK(1,12,0)}}, 1,1, {0}, {0}},
	{"player hotbar to main", 255, 0, 45, 36, 0,0,
		{{36,STACK(1,12,0)}}, {{36,{0}},{9,STACK(1,12,0)}}, 1,2, {0}, STACK(1,12,0)},
	/* SlotCrafting.java:36-43, ContainerPlayer.java:71-94: partial result
	 * insertion still consumes one ingredient; next result count prevents retry. */
	{"partial crafting output", 255, 0, 45, 0, 9,45,
		{{0,STACK(5,4,0)},{1,STACK(17,2,0)},{44,STACK(5,62,0)}},
		{{0,STACK(5,4,0)},{1,STACK(17,1,0)},{44,STACK(5,64,0)}}, 3,3, {0}, STACK(5,4,0)},
	{"workbench output", 1, 1, 46, 0, 0,0,
		{{0,STACK(5,4,0)},{1,STACK(17,1,0)}},
		{{0,{0}},{1,{0}},{45,STACK(5,4,0)}}, 2,3, {0}, STACK(5,4,0)},
	/* ContainerFurnace.java:78-112: output reverse; main/hotbar forward. */
	{"furnace output", 2, 2, 39, 2, 0,0,
		{{2,STACK(265,7,0)}}, {{2,{0}},{38,STACK(265,7,0)}}, 1,2, {0}, STACK(265,7,0)},
	{"furnace main", 2, 2, 39, 3, 0,0,
		{{3,STACK(263,7,0)}}, {{3,{0}},{30,STACK(263,7,0)}}, 1,2, {0}, STACK(263,7,0)},
	/* ContainerDispenser.java has no getStackInSlot override (Container:42). */
	{"dispenser shift no-op", 3, 6, 45, 0, 0,0,
		{{0,STACK(1,12,0)}}, {{0,STACK(1,12,0)}}, 1,1, {0}, STACK(1,12,0)},
};

static struct inventory_logic* logic_for(enum window_type type, size_t capacity) {
	if(type == WINDOW_TYPE_INVENTORY) return &inventory_logic_player;
	if(type == WINDOW_TYPE_WORKBENCH) return &inventory_logic_crafting;
	if(type == WINDOW_TYPE_FURNACE) return &inventory_logic_furnace;
	if(type == WINDOW_TYPE_DISPENSER) return &inventory_logic_dispenser;
	return capacity == 90 ? &inventory_logic_double_chest : &inventory_logic_chest;
}

static void transfer_goldens(void) {
	static struct server_local server;
	for(size_t k = 0; k < sizeof(transfers) / sizeof(*transfers); k++) {
		const struct transfer_case* c = &transfers[k];
		struct inventory storage;
		struct inventory* inv = c->window ? &storage : &server.player.inventory;
		struct window_container wc;
		assert(inventory_create(inv, NULL, NULL, NULL, c->capacity, 0,0,0));
		assert(windowc_create(&wc, c->type, c->capacity));
		inv->logic = logic_for(c->type, c->capacity);
		if(c->type == WINDOW_TYPE_FURNACE || c->type == WINDOW_TYPE_DISPENSER) {
			inv->user = &server; inv->y = -1; // absent backing, legal click-only fixture
		}
		inv->reply = capture;
		server.player.active_inventory = inv;
		server.player.health = 160;
		server.player.has_pos = server.player.finished_loading = true;
		for(size_t s = c->fill_begin; s < c->fill_end; s++)
			inv->items[s] = (struct item_data)STACK(3,64,0);
		for(size_t s = 0; s < c->before_count; s++) inv->items[c->before[s].slot] = c->before[s].item;
		struct item_data want[90];
		memcpy(want, inv->items, c->capacity * sizeof(*want));
		for(size_t s = 0; s < c->after_count; s++) want[c->after[s].slot] = c->after[s].item;
		inv->picked_item = c->cursor;
		for(size_t s = 0; s < c->capacity; s++) windowc_slot_change(&wc, s, inv->items[s]);
		windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, c->cursor);
		uint16_t action;
		assert(windowc_new_quick_action(&wc, &action, c->slot));
		assert(equal(windowc_get_latest(&wc)->revision.expected, c->returned));
		struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = c->window, .slot = c->slot, .quick_move = true,
				.action_id = action, .expected = c->returned}};
		call = click_wire(call);
		reply_count = 0;
		server_local_process_window_for(&server, &server.player, &call);
		assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
		reply_wire(&replies[0], c->window, &wc);
		assert(!wc.quick_pending && !windowc_actions_pending(&wc));
		for(size_t s = 0; s < inv->capacity; s++) {
			assert(equal(inv->items[s], want[s]));
			assert(equal(windowc_get_latest(&wc)->items[s], want[s]));
		}
		assert(equal(inv->picked_item, c->cursor));
		assert(equal(windowc_get_latest(&wc)->picked_item, c->cursor));
		inv->logic = NULL;
		inventory_destroy(inv, NULL);
		windowc_destroy(&wc);
		printf("PASS %s\n", c->name);
	}
}

static struct server_rpc response;
static size_t response_count;
static void capture_response(struct server_rpc* call, void* user) {
	(void)user;
	assert(call->type == SRPC_WINDOW_TRANSACTION);
	response = *call;
	response_count++;
}

static void rejection_goldens(void) {
	/* server NetServerHandler.java:498-526. A stale comparison rejects AFTER
	 * mutation, with ACK, full Packet104, then cursor Packet103(-1,-1).
	 * Cases deliberately mismatch id/count/damage and Quick Move's source. */
	const struct { bool quick; struct item_data expected; } cases[] = {
		{false, STACK(3,12,0)}, {false, STACK(1,11,0)},
		{false, STACK(1,12,1)}, {false,{0}}, {true,STACK(1,11,0)}
	};
	static struct server_local server;
	for(size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
		struct inventory inv;
		struct window_container wc;
		assert(inventory_create(&inv, NULL, NULL, NULL, 63, 0,0,0));
		assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
		inv.logic = &inventory_logic_chest;
		inv.reply = capture;
		inv.items[0] = (struct item_data)STACK(1,12,0);
		/* An unrelated slot MUST also appear in the full correction. */
		inv.items[11] = (struct item_data)STACK(257,1,123);
		server.player.active_inventory = &inv;
		server.player.health = 160;
		server.player.has_pos = server.player.finished_loading = true;
		windowc_slot_change(&wc, 0, cases[c].expected);
		uint16_t id, queued;
		assert(cases[c].quick ? windowc_new_quick_action(&wc, &id, 0)
			: windowc_new_action(&wc, &id, false, 0));
		/* A later prediction must be discarded on the first rejection. */
		assert(windowc_new_action(&wc, &queued, false, 1));
		struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = 3, .slot = 0, .quick_move = cases[c].quick,
				.action_id = id, .expected = cases[c].expected}};
		call = click_wire(call);
		reply_count = 0;
		server_local_process_window_for(&server, &server.player, &call);
		assert(reply_count == 3);
		assert(replies[0].type == CRPC_WINDOW_TRANSACTION && !replies[0].payload.window_transaction.accepted);
		assert(replies[1].type == CRPC_WINDOW_ITEMS && replies[1].payload.window_items.count == 63);
		assert(replies[2].type == CRPC_INVENTORY_SLOT && replies[2].payload.inventory_slot.slot == 255);
		assert(!inv.items[0].id);
		assert(cases[c].quick ? inv.items[62].count == 12 : inv.picked_item.count == 12);
		for(size_t slot = 0; slot < 63; slot++) assert(equal(replies[1].payload.window_items.items[slot], inv.items[slot]));
		assert(equal(replies[2].payload.inventory_slot.item, inv.picked_item));
		assert(inv.transaction_pending);
		struct client_rpc saved[3]; memcpy(saved, replies, sizeof(saved));
		call.payload.window_click.slot = 11;
		call.payload.window_click.action_id = queued;
		call.payload.window_click.expected = inv.items[11];
		reply_count = 0;
		server_local_process_window_for(&server, &server.player, &call);
		assert(!reply_count && inv.items[11].durability == 123);
		struct server_rpc wrong = {.type = SRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = 3, .action_id = id + 1, .accepted = true}};
		server_local_process_window_for(&server, &server.player, &wrong);
		assert(inv.transaction_pending);
		wrong.payload.window_transaction.action_id = id;
		wrong.payload.window_transaction.window = 4;
		server_local_process_window_for(&server, &server.player, &wrong);
		assert(inv.transaction_pending);
		response_count = 0;
		reply_wire(&saved[0], 3, &wc);
		assert(wc.correction_pending && !windowc_actions_pending(&wc));
		assert(!windowc_new_action(&wc, &queued, false, 2));
		svin_process_messages(capture_response, NULL, false);
		assert(response_count == 1 && response.payload.window_transaction.action_id == id);
		struct mp_frame echo = outgoing(&response);
		uint8_t expected_echo[] = {0x6a, 3, id >> 8, id, 1};
		assert(echo.length == 5 && !memcmp(echo.data, expected_echo, 5));
		reply_wire(&saved[1], 3, &wc);
		assert(wc.correction_pending);
		reply_wire(&saved[2], 3, &wc);
		assert(!wc.correction_pending);
		for(size_t slot = 0; slot < 63; slot++) assert(equal(windowc_get_latest(&wc)->items[slot], inv.items[slot]));
		assert(equal(windowc_get_latest(&wc)->picked_item, inv.picked_item));
		server_local_process_window_for(&server, &server.player, &response);
		assert(!inv.transaction_pending);
		server_local_process_window_for(&server, &server.player, &call);
		assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
		inv.logic = NULL; inventory_destroy(&inv, NULL); windowc_destroy(&wc);
	}
	puts("PASS mismatch after mutation: id/count/damage/null/Quick Move, full correction, gate and matching echo");
}

static void drop_goldens(void) {
	/* Container.java:48-61 and Packet102WindowClick:48-59: empty return bytes
	 * ff ff for EVERY drop, even with a nonempty cursor. */
	const struct { bool right; unsigned before, after, dropped; } cases[] = {
		{false, 12, 0, 12}, {true, 12, 11, 1}, {true, 1, 0, 1},
		{false, 0, 0, 0}, {true, 0, 0, 0}
	};
	static struct server_local server;
	for(size_t k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
		memset(&server, 0, sizeof(server));
		dict_entity_init(server.entities);
		dict_server_chunks_init(server.world.chunks);
		rand_gen_seed(&server.rand_src);
		struct inventory* inv = &server.player.inventory;
		assert(inventory_create(inv, NULL, NULL, NULL, 45, 0,0,0));
		inv->logic = &inventory_logic_player; inv->reply = capture;
		server.player.active_inventory = inv;
		server.player.health = 160;
		server.player.has_pos = server.player.finished_loading = true;
		if(cases[k].before) inv->picked_item = (struct item_data)STACK(1,cases[k].before,19);
		struct window_container wc;
		assert(windowc_create(&wc, WINDOW_TYPE_INVENTORY, 45));
		windowc_slot_change(&wc, 255, inv->picked_item);
		uint16_t ordinary, id;
		assert(windowc_new_action(&wc, &ordinary, false, 9));
		/* A preceding ordinary click shares the action ID counter. */
		windowc_action_apply_result(&wc, ordinary, true);
		windowc_slot_change(&wc, 255, inv->picked_item);
		assert(windowc_new_drop_action(&wc, &id, cases[k].right) && id == (uint16_t)(ordinary + 1));
		assert(windowc_get_latest(&wc)->picked_item.count == cases[k].after);
		struct server_rpc call = {.type = SRPC_WINDOW_DROP,
			.payload.window_drop = {.window = 0, .right = cases[k].right, .action_id = id}};
		call = click_wire(call);
		reply_count = 0;
		server_local_process_window_for(&server, &server.player, &call);
		assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
		reply_wire(&replies[0], 0, &wc);
		assert(inv->picked_item.count == cases[k].after);
		assert(equal(windowc_get_latest(&wc)->picked_item, inv->picked_item));
		unsigned dropped = 0;
		dict_entity_it_t it;
		for(dict_entity_it(it, server.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
			struct entity* entity = dict_entity_ref(it)->value;
			assert(entity->type == ENTITY_ITEM && entity->data.item.pickup_delay == 40);
			assert(entity->data.item.item.durability == 19);
			dropped += entity->data.item.item.count;
			free(entity);
		}
		assert(dropped == cases[k].dropped);
		dict_entity_clear(server.entities);
		dict_server_chunks_clear(server.world.chunks);
		clin_drain_discard();
		inv->logic = NULL; inventory_destroy(inv, NULL); windowc_destroy(&wc);
	}
	puts("PASS outside drops: whole/one/last/empty, prediction, action IDs, empty Packet102 stack and accepted ACK only");
}

static void provider_equivalence(void) {
	/* Same multi-click trace through local RPC replies vs Packet102 serialization,
	 * independent peer parsing, then Packet106/104/103 decoding and dispatch.
	 * This is transport equivalence; the decompile-led tables above are the
	 * semantic oracle. An external Java-server proof belongs to slice 5. */
	static struct server_local servers[2];
	const size_t capacities[] = {63,90,45};
	const uint8_t windows[] = {3,4,0};
	for(size_t fixture = 0; fixture < 3; fixture++) {
		struct inventory storage[2];
		struct window_container wc[2];
		struct inventory* inv[2];
		size_t base = fixture == 2 ? 9 : 0;
		for(size_t lane = 0; lane < 2; lane++) {
			struct server_local* s = &servers[lane];
			memset(s, 0, sizeof(*s));
			dict_entity_init(s->entities); dict_server_chunks_init(s->world.chunks);
			rand_gen_seed(&s->rand_src);
			inv[lane] = fixture == 2 ? &s->player.inventory : &storage[lane];
			assert(inventory_create(inv[lane], NULL, NULL, NULL, capacities[fixture], 0,0,0));
			enum window_type type = fixture == 2 ? WINDOW_TYPE_INVENTORY : WINDOW_TYPE_CHEST;
			inv[lane]->logic = logic_for(type, capacities[fixture]);
			inv[lane]->reply = capture;
			s->player.active_inventory = inv[lane];
			s->player.health = 160;
			s->player.has_pos = s->player.finished_loading = true;
			assert(windowc_create(&wc[lane], type, capacities[fixture]));
			inv[lane]->items[base] = (struct item_data)STACK(1,12,0);
			inv[lane]->items[base + 1] = (struct item_data)STACK(3,7,0);
			windowc_slot_change(&wc[lane], base, inv[lane]->items[base]);
			windowc_slot_change(&wc[lane], base + 1, inv[lane]->items[base + 1]);
		}
		const struct { size_t offset; bool right, quick, drop; } trace[] = {
			{0,0,0,0}, {2,1,0,0}, {1,1,0,0}, {3,0,0,0},
			{1,0,1,0}, {0,0,0,1}, {2,0,0,0}, {0,1,0,1}
		};
		for(size_t step = 0; step < sizeof(trace) / sizeof(*trace); step++) {
			for(size_t lane = 0; lane < 2; lane++) {
				uint16_t id;
				size_t slot = base + trace[step].offset;
				bool drop = trace[step].drop, quick = trace[step].quick;
				assert(drop ? windowc_new_drop_action(&wc[lane], &id, trace[step].right)
					: quick ? windowc_new_quick_action(&wc[lane], &id, slot)
					: windowc_new_action(&wc[lane], &id, trace[step].right, slot));
				struct server_rpc call;
				if(drop) call = (struct server_rpc){.type = SRPC_WINDOW_DROP,
					.payload.window_drop = {.window = windows[fixture], .action_id = id,
						.right = trace[step].right}};
				else call = (struct server_rpc){.type = SRPC_WINDOW_CLICK,
					.payload.window_click = {.window = windows[fixture], .slot = slot,
						.action_id = id, .right_click = trace[step].right, .quick_move = quick,
						.expected = windowc_get_latest(&wc[lane])->revision.expected}};
				if(lane) call = click_wire(call);
				reply_count = 0;
				server_local_process_window_for(&servers[lane], &servers[lane].player, &call);
				assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
				if(lane) reply_wire(&replies[0], windows[fixture], &wc[lane]);
				else {
					struct client_session session = {0};
					session.windows[windows[fixture]] = &wc[lane];
					assert(client_session_window_reply(&session, &replies[0]));
				}
				clin_drain_discard();
			}
			for(size_t slot = 0; slot < capacities[fixture]; slot++) {
				assert(equal(inv[0]->items[slot], inv[1]->items[slot]));
				for(size_t lane = 0; lane < 2; lane++)
					assert(equal(windowc_get_latest(&wc[lane])->items[slot], inv[lane]->items[slot]));
			}
			assert(equal(inv[0]->picked_item, inv[1]->picked_item));
			assert(equal(windowc_get_latest(&wc[0])->picked_item, windowc_get_latest(&wc[1])->picked_item));
		}
		size_t moved_to = fixture == 2 ? 36 : capacities[fixture] - 1;
		assert(equal(inv[0]->items[moved_to], (struct item_data)STACK(1,11,0)));
		assert(equal(inv[0]->items[base + 3], (struct item_data)STACK(3,7,0)));
		assert(!inv[0]->picked_item.id);
		for(size_t lane = 0; lane < 2; lane++) {
			dict_entity_it_t it;
			for(dict_entity_it(it, servers[lane].entities); !dict_entity_end_p(it); dict_entity_next(it))
				free(dict_entity_ref(it)->value);
			dict_entity_clear(servers[lane].entities);
			dict_server_chunks_clear(servers[lane].world.chunks);
			inv[lane]->logic = NULL; inventory_destroy(inv[lane], NULL); windowc_destroy(&wc[lane]);
		}
	}
	puts("PASS local-vs-remote equivalence: three full-window traces, same slots/cursor after every action");
}

static bool created_fail;
static uint16_t created_damage;
static unsigned created_calls;
static bool created_hook(struct server_local* server,
	struct server_player* actor, struct item_data* item) {
	assert(server);
	/* S1: onCreated now takes the acting crafter. These fixture inventories
	 * are built with a NULL owner (inventory_create(..., NULL, ...)), so the
	 * hook legitimately receives NULL here -- the injection this test needs is
	 * unaffected either way. */
	(void)actor;
	created_calls++;
	if(created_fail) return false;
	item->durability = created_damage;
	return true;
}

static void persistent_craft_replies(void) {
	/* SlotCrafting:17-18 / ItemMap:228-237. The disk-backed onCreated hook
	 * is replaced ONLY here to deterministically inject ID allocation/failure;
	 * the real item definition, recipe, click, transaction and prediction run.
	 * Successful clicks have only the vanilla ACK, including unequal IDs. */
	const struct { bool quick, fail; uint16_t damage; size_t replies; bool accept; } cases[] = {
		{false,false,1,1,true}, {false,false,123,1,true},
		{false,true,0,1,true}, {true,false,123,1,true}, {true,true,0,3,false}
	};
	struct item_data map = STACK(358,1,0);
	struct item* def = item_get(&map);
	bool (*saved_hook)(struct server_local*, struct server_player*,
		struct item_data*) = def->onCreated;
	def->onCreated = created_hook;
	static struct server_local server;
	for(size_t k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
		struct inventory inv;
		struct window_container wc;
		assert(inventory_create(&inv, NULL, NULL, NULL, 46, 0,0,0));
		assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, 46));
		inv.logic = &inventory_logic_crafting;
		inv.user = &server; inv.reply = capture;
		server.player.active_inventory = &inv;
		server.player.health = 160;
		server.player.has_pos = server.player.finished_loading = true;
		inv.items[0] = map;
		for(size_t i = 1; i <= 9; i++) inv.items[i] = (struct item_data)STACK(i == 5 ? 345 : 339,1,0);
		for(size_t i = 0; i < 46; i++) windowc_slot_change(&wc, i, inv.items[i]);
		wc.map_counter = cases[k].damage ? cases[k].damage - 1 : 0;
		created_fail = cases[k].fail; created_damage = cases[k].damage; created_calls = 0;
		uint16_t id;
		assert(cases[k].quick ? windowc_new_quick_action(&wc, &id, 0)
			: windowc_new_action(&wc, &id, false, 0));
		assert(!created_calls); // prediction never does disk IO
		struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = 1, .slot = 0, .quick_move = cases[k].quick,
				.action_id = id, .expected = map}};
		call = click_wire(call);
		reply_count = 0;
		server_local_process_window_for(&server, &server.player, &call);
		assert(created_calls == 1 && reply_count == cases[k].replies);
		assert(replies[0].payload.window_transaction.accepted == cases[k].accept);
		for(size_t i = 0; i < reply_count; i++) reply_wire(&replies[i], 1, &wc);
		if(!cases[k].fail || !cases[k].accept) {
			for(size_t i = 0; i < 46; i++) assert(equal(windowc_get_latest(&wc)->items[i], inv.items[i]));
			assert(equal(windowc_get_latest(&wc)->picked_item, inv.picked_item));
		}
		if(cases[k].fail) assert(inv.items[0].id == 358 && inv.items[5].id == 345 && !inv.picked_item.id);
		else if(cases[k].quick) assert(inv.items[45].id == 358 && inv.items[45].durability == cases[k].damage);
		else assert(inv.picked_item.id == 358 && inv.picked_item.durability == cases[k].damage);
		if(!cases[k].accept) {
			svin_process_messages(capture_response, NULL, false);
			server_local_process_window_for(&server, &server.player, &response);
			assert(!inv.transaction_pending);
		}
		inv.logic = NULL; inventory_destroy(&inv, NULL); windowc_destroy(&wc);
	}
	def->onCreated = saved_hook;
	puts("PASS crafting: vanilla ACK only on acceptance; allocation failure preserves server ownership");
}

/* Exercises the exact production held-map upload, reading the GL texture back
 * as well as checking the CPU mirror. Blank expectations are hand-derived from
 * [740c583 MapData.colors=new byte[16384], MapItemRenderer.renderMap]. */
static void assert_map_upload(int32_t id, const uint8_t* colors) {
	uint8_t expected[128 * 128 * 4], actual[128 * 128 * 4];
	if(colors) item_map_colors_to_rgba(colors, expected);
	else for(int i = 0; i < 128 * 128; i++) {
		expected[4*i] = expected[4*i+1] = expected[4*i+2] = 0;
		expected[4*i+3] = ((i + i / 128) & 1) * 8 + 16;
	}
	assert(render_item_map_upload(id));
	assert(memcmp(texture_map_live.data, expected, sizeof(expected)) == 0);
	glBindTexture(GL_TEXTURE_2D, texture_map_live.id);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
	assert(glGetError() == GL_NO_ERROR);
	assert(memcmp(actual, expected, sizeof(expected)) == 0);
}

static void install_map_plane(int32_t id, const uint8_t* colors, bool wire) {
	if(wire) {
		/* [740c583 NetClientHandler.handleMapData,
		 * MapData.updateMPMapData]: Packet131 type-0 column strips. */
		for(unsigned x = 0; x < 128; x++) {
			uint8_t packet[6 + 3 + 128] = {0x83};
			put16(packet + 1, 358);
			put16(packet + 3, (uint16_t)id);
			packet[5] = 131;
			packet[7] = x;
			for(unsigned y = 0; y < 128; y++) packet[9 + y] = colors[y * 128 + x];
			incoming(packet, sizeof(packet));
		}
		flush_map_mirrors(remote.impl);
		return;
	}
	struct map_rpc_slot* slot = clin_map_slot_try_acquire();
	assert(slot);
	slot->map_id = id;
	memcpy(slot->colors, colors, MAP_DATA_COLORS_SIZE);
	clin_process(&(struct client_rpc){.type = CRPC_MAP_DATA, .payload.map_data.slot = slot});
}

static bool compact_capture(void* user, struct server_rpc* call) {
	struct server_rpc* captured = user;
	assert(!captured->type);
	*captured = *call;
	return true;
}

/* Activate the actual visible compact slot, without bypassing its adapter or
 * menu prediction. The returned RPC subsequently crosses click_wire(). */
static struct server_rpc lifecycle_click(struct container_compact* view,
	struct window_container* wc, size_t slot, bool quick) {
	struct server_rpc call = {0};
	if(view) {
		size_t k = 0;
		while(k < view->count && view->slots[k].slot != slot) k++;
		assert(k < view->count);
		view->menu.selected = k;
		struct container_input input = {.pick=true,.quick_move=quick};
		assert(!container_compact_step(view, wc, 1, &input, .016F, compact_capture, &call));
		assert(call.type == SRPC_WINDOW_CLICK && call.payload.window_click.slot == slot);
		assert(call.payload.window_click.quick_move == quick);
	} else {
		uint16_t id;
		assert(quick ? windowc_new_quick_action(wc, &id, slot)
			: windowc_new_action(wc, &id, false, slot));
		call = (struct server_rpc){.type=SRPC_WINDOW_CLICK,
			.payload.window_click={.window=1,.slot=slot,.quick_move=quick,
				.action_id=id,.expected=windowc_get_latest(wc)->revision.expected}};
	}
	return call;
}

static void* map_fail_alloc(size_t size) { (void)size; return NULL; }

static void map_residency_golden(void) {
	/* 740c583 MapData.colors is zero until data; MapStorage.loadData retains
	 * identity. Our eight-plane RAM cap must preserve all evicted bytes. */
	clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, 46));
	const uint8_t* blank = clin_held_map_colors(-123, NULL);
	for(unsigned craft = 1; craft <= 128; craft++) {
		windowc_slot_change(&wc, 0, (struct item_data)STACK(358,1,0));
		for(size_t k = 1; k <= 9; k++)
			windowc_slot_change(&wc, k, (struct item_data)STACK(k == 5 ? 345 : 339,1,0));
		uint16_t action;
		assert(windowc_new_action(&wc, &action, false, 0));
		assert(windowc_get_latest(&wc)->picked_item.durability == craft);
		assert(clin_held_map_colors(craft, NULL) == blank);
		assert(clin_map_test_plane_count() == 0);
		windowc_action_apply_result(&wc, action, true);
		windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, (struct item_data){0});
	}
	windowc_destroy(&wc);
	static uint8_t colors[MAP_DATA_COLORS_SIZE];
	memset(colors, 4, sizeof(colors));
	uint32_t failures = clin_map_allocation_failures();
	clin_set_alloc_hook(map_fail_alloc);
	install_map_plane(21, colors, false);
	clin_set_alloc_hook(NULL);
	assert(clin_map_allocation_failures() == failures + 1);
	assert(clin_map_test_plane_count() == 0);
	assert_map_upload(21, NULL);
	uint32_t io_failures = clin_map_io_failures();
	for(unsigned wire = 0; wire < 2; wire++) {
		uint32_t hot_version = 0;
		for(int id = 21; id < 21 + CLIN_MAP_PLANES + 5; id++) {
			for(unsigned k = 0; k < sizeof(colors); k++) colors[k] = 4 + (k + id) % 48;
			install_map_plane(id, colors, wire);
			assert(clin_map_test_plane_count() == (size_t)(id - 20 < CLIN_MAP_PLANES ? id - 20 : CLIN_MAP_PLANES));
			assert_map_upload(id, colors);
			if(id == 21 + CLIN_MAP_PLANES - 1)
				clin_held_map_colors(21, &hot_version); // refresh oldest: LRU, not FIFO
			if(id == 21 + CLIN_MAP_PLANES) {
				uint32_t version;
				clin_held_map_colors(21, &version);
				assert(version == hot_version); // hot 21 was not evicted/reloaded
			}
		}
		/* Touch oldest-to-newest: every lookup forces a spill/reload. */
		for(int id = 21; id < 21 + CLIN_MAP_PLANES + 5; id++) {
			for(unsigned k = 0; k < sizeof(colors); k++) colors[k] = 4 + (k + id) % 48;
			assert_map_upload(id, colors);
			assert(clin_map_test_plane_count() == CLIN_MAP_PLANES);
		}
		if(wire) {
			/* Both LRUs evicted 21. A one-column Packet131 must preserve
			 * every untouched byte restored from its session map_21.dat. */
			for(unsigned k = 0; k < sizeof(colors); k++) colors[k] = 4 + (k + 21) % 48;
			uint8_t packet[6 + 3 + 128] = {0x83};
			put16(packet + 1, 358); put16(packet + 3, 21);
			packet[5] = 131; packet[7] = 7;
			for(unsigned y = 0; y < 128; y++) {
				packet[9 + y] = 12;
				colors[y * 128 + 7] = 12;
			}
			incoming(packet, sizeof(packet));
			flush_map_mirrors(remote.impl);
			assert_map_upload(21, colors);
			assert(clin_map_test_plane_count() == CLIN_MAP_PLANES);
		}
		assert(clin_map_io_failures() == io_failures);
		clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
		assert(clin_map_test_plane_count() == 0);
		assert(clin_held_map_colors(21, NULL) == blank);
		assert_map_upload(21, NULL);
	}
	puts("PASS map residency: 128 crafts = zero planes; OOM counted/blank; 13 data IDs = eight planes; local and Packet131 exact eviction/reload; teardown zero");
}

/* Queue real decoded Packet131 strips, then exercise the production pump.
 * 740c583 MapData.updateMPMapData: only the addressed column changes.
 * NetClientHandler.handleMapData / ItemMap.onUpdate: no client re-request. */
static void map_column(int16_t id, uint8_t x, uint8_t color, bool queue) {
	uint8_t packet[137] = {0x83};
	put16(packet + 1, 358); put16(packet + 3, (uint16_t)id);
	packet[5] = 131; packet[7] = x;
	memset(packet + 9, color, 128);
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	struct server_remote_impl* impl = remote.impl;
	struct remote_event event = {.kind=REMOTE_EVENT_PACKET, .generation=impl->generation};
	for(size_t i = 0; i < sizeof(packet); i++)
		assert(mp_decoder_feed(&decoder, packet[i], &event.packet)
			== (i + 1 == sizeof(packet) ? MP_DECODE_EVENT : MP_DECODE_MORE));
	if(queue) {
		assert(impl->event_count < REMOTE_QUEUE_CAPACITY);
		impl->events[(impl->event_head + impl->event_count++) % REMOTE_QUEUE_CAPACITY] = event;
	} else assert(process_event(&remote, &event));
	mp_decoder_destroy(&decoder);
}

static void map_retry_golden(void) {
	struct server_remote_impl* impl = remote.impl;
	clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
	memset(impl->map_mirrors, 0, sizeof(impl->map_mirrors));
	assert(!impl->event_count);
	static uint8_t expected[MAP_DATA_COLORS_SIZE];
	/* Interleave five identities, no intermediate flush; all columns exact. */
	for(unsigned col = 0; col < 128; col++)
		for(int id = 1; id <= 5; id++) map_column(id, col, 4 + id + col % 40, false);
	flush_map_mirrors(impl);
	for(int id = 1; id <= 5; id++) {
		for(unsigned k = 0; k < sizeof(expected); k++) expected[k] = 4 + id + (k % 128) % 40;
		assert_map_upload(id, expected);
	}
	/* Nine distinct dirty identities: ninth remains queued, next pump applies. */
	uint32_t deferred = impl->map_update_deferred;
	for(int id = 1; id <= 9; id++) map_column(id, 7, 42, true);
	server_remote_pump(&remote);
	assert(!impl->event_count && impl->map_retry_count == 1 && impl->map_update_deferred == deferred + 1);
	assert(impl->map_data_dropped == 0);
	server_remote_pump(&remote);
	assert(!impl->event_count);
	for(int id = 1; id <= 9; id++) {
		for(unsigned k = 0; k < sizeof(expected); k++)
			expected[k] = k % 128 == 7 ? 42 : id <= 5 ? 4 + id + (k % 128) % 40 : 0;
		assert_map_upload(id, expected);
	}
	/* Force ID 1 out of both caches, preserving its disk copy. */
	for(int id = 10; id < 19; id++) { map_column(id, 2, 17, true); server_remote_pump(&remote); }
	clin_map_test_fail_reload(true);
	assert(clin_map_recover(1, NULL) == NULL);
	assert_map_upload(1, NULL); // shared blank while unknown
	map_column(1, 8, 32, true);
	map_column(1, 8, 33, true);
	deferred = impl->map_update_deferred;
	server_remote_pump(&remote);
	assert(!impl->event_count && impl->map_retry_count == 2 && impl->map_update_deferred == deferred + 1);
	for(unsigned i = 0; i < REMOTE_MAP_MIRRORS; i++)
		assert(!impl->map_mirrors[i].used || impl->map_mirrors[i].map_id != 1);
	/* Retry is bounded per pump even after its diagnostic counter saturates. */
	impl->map_update_deferred = UINT32_MAX;
	server_remote_pump(&remote);
	assert(!impl->event_count && impl->map_retry_count == 2 && impl->map_update_deferred == UINT32_MAX);
	clin_map_test_fail_reload(false);
	server_remote_pump(&remote);
	assert(!impl->event_count);
	for(unsigned k = 0; k < sizeof(expected); k++)
		expected[k] = k % 128 == 8 ? 33 : k % 128 == 7 ? 42 : 5 + (k % 128) % 40;
	assert_map_upload(1, expected);
	/* Full retry queue invalidates the affected identity, never publishes its
	 * stale disk plane. A later Packet131 re-covers only the addressed column. */
	for(int id = 40; id < 49; id++) { map_column(id, 2, 17, true); server_remote_pump(&remote); }
	clin_map_test_fail_reload(true);
	assert(clin_map_recover(1, NULL) == NULL);
	for(unsigned i = 0; i < REMOTE_MAP_RETRIES; i++) map_column(1, 8, 9, true);
	server_remote_pump(&remote);
	assert(impl->map_retry_count == REMOTE_MAP_RETRIES && !impl->event_count);
	/* ID 2 is also spilled/unavailable, forcing the 65th retry. */
	map_column(2, 8, 10, true);
	server_remote_pump(&remote);
	assert(impl->map_retry_overflows == 1 && impl->map_retry_count == 1);
	assert_map_upload(1, NULL);
	clin_map_test_fail_reload(false);
	map_column(1, 9, 31, true);
	server_remote_pump(&remote);
	memset(expected, 0, sizeof(expected));
	for(unsigned y = 0; y < 128; y++) expected[y * 128 + 9] = 31;
	assert_map_upload(1, expected);
	/* Failed delivery retains dirty bytes, even with no further packets. */
	clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
	memset(impl->map_mirrors, 0, sizeof(impl->map_mirrors));
	map_column(31, 9, 29, true);
	clin_set_alloc_hook(map_fail_alloc);
	deferred = impl->map_push_deferred;
	server_remote_pump(&remote);
	struct remote_map_mirror* m = remote_map_mirror_for(impl, 31);
	assert(m && m->dirty && !impl->event_count && impl->map_push_deferred == deferred + 1);
	assert_map_upload(31, NULL);
	clin_set_alloc_hook(NULL);
	server_remote_pump(&remote);
	assert(!m->dirty);
	memset(expected, 0, sizeof(expected));
	for(unsigned y = 0; y < 128; y++) expected[y * 128 + 9] = 29;
	assert_map_upload(31, expected);
	clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
	memset(impl->map_mirrors, 0, sizeof(impl->map_mirrors));
	puts("PASS Packet131: five interleaved maps byte-exact; ninth dirty ID waits; failed recovery stays unknown/queued; untouched bytes survive; failed delivery stays dirty and retries next pump");
}

/* Real transport enqueue/backpressure, with an independent producer. */
struct map_pressure { uint16_t action; atomic_bool done; };
static void* map_pressure_producer(void* arg) {
	struct map_pressure* p = arg;
	struct server_remote_impl* impl = remote.impl;
	for(unsigned i = 0; i < REMOTE_QUEUE_CAPACITY + 1; i++) {
		struct remote_event e = {.kind = REMOTE_EVENT_PACKET, .generation = impl->generation};
		if(i == REMOTE_QUEUE_CAPACITY - 1) {
			e.packet.type = MP_EVENT_TRANSACTION;
			e.packet.payload.transaction.window = 3;
			e.packet.payload.transaction.action = p->action;
			e.packet.payload.transaction.accepted = true;
		} else {
			e.packet.type = MP_EVENT_MAP_DATA;
			e.packet.payload.map_data.item_id = 358;
			e.packet.payload.map_data.map_id = 100 + i % 16;
			e.packet.payload.map_data.length = 4;
			e.packet.payload.map_data.bytes[1] = i;
			e.packet.payload.map_data.bytes[3] = 42;
		}
		assert(server_remote_test_push_event(&remote, &e));
	}
	atomic_store(&p->done, true);
	return NULL;
}

static void map_pressure_golden(void) {
	struct server_remote_impl* impl = remote.impl;
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
	gstate.players[0].windows[3] = &wc;
	clin_set_alloc_hook(map_fail_alloc);
	ptime_t start = time_get();
	for(unsigned batch = 0; batch < 20; batch++) {
		struct map_pressure p = {0};
		assert(windowc_new_action(&wc, &p.action, false, 0));
		struct thread producer;
		assert(thread_create(&producer, map_pressure_producer, &p, 64));
		/* Wait for actual production queue saturation, then hold for 35ms. */
		struct server_remote_status status;
		ptime_t deadline = time_add_ms(time_get(), 2000);
		do {
			server_remote_get_status(&remote, &status);
			assert(!time_reached(time_get(), deadline));
			thread_msleep(1);
		} while(status.queued_events != REMOTE_QUEUE_CAPACITY);
		thread_msleep(35);
		do {
			server_remote_pump(&remote);
			thread_msleep(1);
		} while(!atomic_load(&p.done));
		thread_join(&producer);
		server_remote_pump(&remote);
		assert(!windowc_actions_pending(&wc));
		assert(!impl->stop_now && impl->state == REMOTE_PLAY);
		assert(!impl->event_count && impl->map_retry_count <= REMOTE_MAP_RETRIES);
	}
	assert(time_diff_ms(start, time_get()) > 500);
	assert(impl->map_retry_overflows && impl->event_backpressure_waits);
	assert(impl->map_push_deferred && impl->map_retry_count);
	clin_set_alloc_hook(NULL);
	gstate.players[0].windows[3] = NULL;
	windowc_destroy(&wc);
	impl->map_retry_count = 0;
	memset(impl->map_mirrors, 0, sizeof(impl->map_mirrors));
	clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
	puts("PASS prolonged map failure >500ms: production FIFO saturated 20 times, 20 inventory ACKs retired, retry overflow unknown, no disconnect");
}

/* [740c583 SlotCrafting.onPickupFromSlot, ItemMap.onCreated,
 * MapStorage.getUniqueDataId]. Explicit no-zero maintainer ruling starts at 1.
 * The peer executes its disk allocator, independently of the client counter;
 * every request and response crosses the real remote serializer/decoder. */
static void consecutive_map_crafts(void) {

	for(unsigned mode = 0; mode < 8; mode++) {
		bool quick = mode & 1, wire = mode & 2, compact = mode & 4;
		char path[128];
		test_fixture_dir(quick ? "sortfix_quick" : "sortfix_normal", path, sizeof(path));
#ifdef _WIN32
		assert(_mkdir(path) == 0);
#else
		assert(mkdir(path, 0777) == 0);
#endif
		static struct server_local server;
		memset(&server, 0, sizeof(server));
		string_init_set_str(server.level_name, path);
		int32_t allocated;
		for(int n = 1; n <= 20; n++)
			assert(map_idcounts_get_unique_id(server.level_name, "map", &allocated) && allocated == n);
		for(size_t i = 0; i < MAX_MAP_CACHE; i++) server.map_cache_id[i] = INT32_MIN;
		struct inventory* inv = calloc(1, sizeof(*inv));
		assert(inv);
		struct window_container wc;
		assert(inventory_create(inv, NULL, NULL, NULL, 46, 0,0,0));
		assert(inventory_create(&server.player.inventory, &inventory_logic_player,
			&server, &server.player, 45, 0,0,0));
		server.player.inventory.reply = capture;
		clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
		assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, 46));
		struct container_compact view;
		assert(container_compact_open(&view, wc.type, wc.slot_count, 0, 640, 240));
		inv->logic = &inventory_logic_crafting; inv->user = &server; inv->reply = capture;
		server.player.active_inventory = inv;
		server.player.health = 160;
		server.player.has_pos = server.player.finished_loading = true;
		inv->items[0] = (struct item_data)STACK(358,1,0);
		for(size_t i = 1; i <= 9; i++) inv->items[i] = (struct item_data)STACK(i == 5 ? 345 : 339,2,0);
		for(size_t i = 0; i < 46; i++) windowc_slot_change(&wc, i, inv->items[i]);
		if(quick) {
			for(size_t i = 10; i < 46; i++) {
				inv->items[i] = (struct item_data)STACK(1,64,0);
				windowc_slot_change(&wc, i, inv->items[i]);
			}
			struct server_rpc call = lifecycle_click(compact ? &view : NULL, &wc, 0, true);
			call = click_wire(call); reply_count = 0;
			server_local_process_window_for(&server, &server.player, &call);
			assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
			if(wire) reply_wire(&replies[0], 1, &wc);
			else windowc_action_apply_result(&wc, replies[0].payload.window_transaction.action_id, true);
			assert(wc.map_counter == 0 && inv->items[5].count == 2);
			for(size_t i = 10; i < 46; i++) {
				inv->items[i] = (struct item_data){0};
				windowc_slot_change(&wc, i, inv->items[i]);
			}
		}
		for(unsigned craft = 1; craft <= 2; craft++) {
			struct server_rpc call = lifecycle_click(compact ? &view : NULL, &wc, 0, quick);
			/* Unrelated authoritative update replays the pending craft once;
			 * neither replay nor the server can spend the client counter. */
			windowc_slot_change(&wc, 10, (struct item_data){0});
			assert(wc.map_counter == craft);
			call = click_wire(call); reply_count = 0;
			server_local_process_window_for(&server, &server.player, &call);
			assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
			if(wire) reply_wire(&replies[0], 1, &wc);
			else windowc_action_apply_result(&wc, replies[0].payload.window_transaction.action_id, true);
			if(!quick) {
				assert(inv->picked_item.durability == 20 + craft);
				assert(windowc_get_latest(&wc)->picked_item.durability == craft);
				call = lifecycle_click(compact ? &view : NULL, &wc, 46 - craft, false);
				call = click_wire(call); reply_count = 0;
				server_local_process_window_for(&server, &server.player, &call);
				assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
				if(wire) reply_wire(&replies[0], 1, &wc);
			else windowc_action_apply_result(&wc, replies[0].payload.window_transaction.action_id, true);
			}
			for(size_t i = 0; i < 46; i++) {
				struct item_data predicted = windowc_get_latest(&wc)->items[i];
				if(predicted.id == 358 && i >= 10) predicted.durability += 20;
				assert(equal(inv->items[i], predicted));
			}
			assert(inv->items[46 - craft].durability == 20 + craft);
		}
		assert(inv->items[45].durability == 21 && inv->items[44].durability == 22);
		/* Distinct terrain is fixture input, persisted through the map archive.
		 * [740c583 MapData.colors / MapItemRenderer.renderMap]. */
		for(int id = 21; id <= 22; id++) {
			struct map_data* md = item_map_cache_resident(&server, id);
			assert(md);
			memset(md->colors, id == 21 ? 4 : 8, MAP_DATA_COLORS_SIZE);
			item_map_cache_mark_dirty(&server, id);
			install_map_plane(id, md->colors, wire);
			assert_map_upload(id, md->colors);
			/* Previous-texture leak: predicted 1/2 must upload blank even
			 * immediately after rendering authoritative 21/22. */
			struct item_data predicted = windowc_get_latest(&wc)->items[id == 21 ? 45 : 44];
			assert(predicted.durability == id - 20);
			assert_map_upload((int16_t)predicted.durability, NULL);
			assert_map_upload(id, md->colors);
		}
		assert_map_upload(21, item_map_cache_resident(&server, 21)->colors);
		assert(clin_map_test_plane_count() == 2); // predictions 1/2 never owned nodes
		item_map_cache_flush_all(&server);
		/* Production close destroys the heap workbench, copies its authoritative
		 * slots to the player and publishes them. No fixture stack hand-copy.
		 * [740c583 ContainerWorkbench.onCraftGuiClosed, EntityPlayerMP]. */
		reply_count = 0;
		struct server_rpc close = {.type = SRPC_WINDOW_CLOSE,
			.payload.window_close.window = WINDOWC_CRAFTING};
		if(compact) {
			close = (struct server_rpc){0};
			struct container_input back = {.close=true};
			assert(container_compact_step(&view, &wc, WINDOWC_CRAFTING,
				&back, .016F, compact_capture, &close));
			assert(close.type == SRPC_WINDOW_CLOSE && close.payload.window_close.window == WINDOWC_CRAFTING);
			struct mp_frame frame = outgoing(&close);
			assert(frame.length == 2 && frame.data[0] == 0x65 && frame.data[1] == WINDOWC_CRAFTING);
		}
		server_local_process_window_for(&server, &server.player, &close);
		assert(server.player.active_inventory == &server.player.inventory);
		assert(reply_count == 37); // output clear + all 36 player slots
		for(size_t r = 0; r < reply_count; r++) clin_process(&replies[r]);
		struct inventory* client = windowc_get_latest(gstate.players[0].windows[0]);
		assert(client->items[44].durability == 21 && client->items[43].durability == 22);
		assert(clin_map_test_plane_count() == 2); // authoritative IDs replace implicit blanks
		assert_map_upload((int16_t)client->items[44].durability, item_map_cache_resident(&server, 21)->colors);
		assert_map_upload((int16_t)client->items[43].durability, item_map_cache_resident(&server, 22)->colors);
		/* Real pickup callback and its slot update: a collected map learns its
		 * authoritative Damage through Packet103, never a guessed counter. */
		struct item_data pickup = STACK(358,1,21);
		reply_count = 0;
		assert(server.player.inventory.logic->on_collect(&server.player.inventory,
			&server.player, &pickup));
		assert(reply_count == 1);
		clin_process(&replies[0]);
		assert(client->items[36].id == 358 && client->items[36].durability == 21);
		assert(level_archive_create_new(&server.level, server.level_name, 123, (vec3){0,64,0}));
		assert(server_local_test_save_player_state(&server));
		level_archive_destroy(&server.level);
		assert(level_archive_create(&server.level, server.level_name));
		inventory_clear(&server.player.inventory);
		/* Production reset + load + publish, not a test-only slot assignment. */
		clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
		assert_map_upload(21, NULL); // same ID/version must not retain old terrain
		server.inventory_finish_pending = true;
		server_local_test_finish_load_inventory(&server);
		clin_update_drain();
		client = windowc_get_latest(gstate.players[0].windows[0]);
		assert(client->items[44].durability == 21 && client->items[43].durability == 22);
		assert(client->items[36].durability == 21);
		for(int id = 21; id <= 22; id++) {
			struct map_data md;
			assert(map_data_archive_load(server.level_name, id, &md));
			assert(md.colors[0] == (id == 21 ? 4 : 8));
			install_map_plane(id, md.colors, wire);
			assert_map_upload((int16_t)client->items[id == 21 ? 44 : 43].durability, md.colors);
		}
		level_archive_destroy(&server.level);
		int32_t next;
		assert(map_idcounts_get_unique_id(server.level_name, "map", &next) && next == 23);
		inventory_destroy(&server.player.inventory, &server.player);
		windowc_destroy(&wc); string_clear(server.level_name);
		test_fixture_dir_cleanup(path);
	}

	puts("PASS full/compact lifecycle ordinary/Quick Move maps: client IDs 1,2 / server IDs 21,22; local and remote ACK-only; close/pickup/save/relog publish authoritative IDs; GL readback blank predictions and exact terrain; generic inventory icon");
}

static void negative_map_import(void) {
	/* 740c583 ItemStack:83-85, ItemMap:24-27, MapStorage:111-154,
	 * World:2470-2471: signed short survives stack, allocator and filename. */
	char path[128]; test_fixture_dir("sortfix_negative", path, sizeof(path));
#ifdef _WIN32
	assert(_mkdir(path) == 0);
#else
	assert(mkdir(path, 0777) == 0);
#endif
	static struct server_local server;
	memset(&server, 0, sizeof(server)); string_init_set_str(server.level_name, path);
	struct level_archive archive;
	assert(level_archive_create_new(&archive, server.level_name, 123, (vec3){0,64,0}));
	struct inventory saved;
	assert(inventory_create(&saved, NULL, NULL, NULL, 45, 0,0,0));
	const int ids[] = {-32768, -1};
	for(size_t k = 0; k < 2; k++) {
		struct map_data md; map_data_set_defaults(&md, 123 + k, -456, 0);
		md.colors[17] = 42 + k;
		assert(map_data_archive_save(server.level_name, ids[k], &md));
		saved.items[36 + k] = (struct item_data)STACK(358,1,(uint16_t)ids[k]);
	}
	assert(level_archive_write_inventory(&archive, &saved)); level_archive_destroy(&archive);
	/* Uncompressed root compound: TAG_SHORT map=-32768. */
	const unsigned char counts[] = {10,0,0,2,0,3,'m','a','p',0x80,0,0};
	char file[256]; snprintf(file, sizeof(file), "%s/data/idcounts.dat", path);
	FILE* f = fopen(file, "wb"); assert(f);
	assert(fwrite(counts, 1, sizeof(counts), f) == sizeof(counts)); assert(!fclose(f));
	for(unsigned reload = 0; reload < 2; reload++) {
		for(size_t i = 0; i < MAX_MAP_CACHE; i++) server.map_cache_id[i] = INT32_MIN;
		assert(level_archive_create(&archive, server.level_name));
		inventory_clear(&saved); assert(level_archive_read_inventory(&archive, &saved));
		for(size_t k = 0; k < 2; k++) {
			struct item_data* stack = &saved.items[36 + k];
			assert((int16_t)stack->durability == ids[k]);
			item_map.onUpdate(&server, stack, 36 + k, false);
			assert((int16_t)stack->durability == ids[k]);
			struct map_data* md = item_map_cache_resident(&server, ids[k]);
			assert(md && md->x_center == 123 + (int)k && md->colors[17] == 42 + k);
			item_map_cache_mark_dirty(&server, ids[k]);
		}
		item_map_cache_flush_all(&server);
		assert(level_archive_write_inventory(&archive, &saved)); level_archive_destroy(&archive);
	}
	int32_t next; assert(map_idcounts_get_unique_id(server.level_name, "map", &next) && next == -32767);
	struct map_data md; assert(!map_data_archive_load(server.level_name, 1, &md));
	inventory_destroy(&saved, NULL); string_clear(server.level_name); test_fixture_dir_cleanup(path);
	puts("PASS signed map import/reload: Damage -32768/-1, signed filenames/cache/flush, idcounts next -32767; no replacement allocation");
}

static void map_counter_lifetime(void) {
	/* ItemMap.onCreated uses World MapStorage, not the container lifetime. */
	struct client_session session = {0};
	assert(client_session_bind_local(&session, 1));
	struct client_rpc open = {.type = CRPC_OPEN_WINDOW,
		.payload.window_open = {.window = 1, .type = WINDOW_TYPE_WORKBENCH, .slot_count = 46}};
	for(unsigned craft = 1; craft <= 2; craft++) {
		assert(client_session_local_reply(&session, 1, &open));
		struct window_container* wc = session.windows[1];
		windowc_slot_change(wc, 0, (struct item_data)STACK(358,1,0));
		for(size_t slot = 1; slot <= 9; slot++)
			windowc_slot_change(wc, slot, (struct item_data)STACK(slot == 5 ? 345 : 339,1,0));
		uint16_t id;
		assert(craft == 1 ? windowc_new_action(wc, &id, false, 0) : windowc_new_quick_action(wc, &id, 0));
		windowc_action_apply_result(wc, id, true);
		assert(session.map_counter == craft);
		struct inventory* latest = windowc_get_latest(wc);
		assert((craft == 1 ? latest->picked_item : latest->items[45]).durability == craft);
	}
	client_session_clear_windows(&session);
	assert(session.map_counter == 0);
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, 46));
	windowc_slot_change(&wc, 10, (struct item_data)STACK(358,1,999));
	assert(wc.map_counter == 0); // receiving a map does not run onCreated
	uint16_t id;
	assert(windowc_new_action(&wc, &id, false, 0)); // no output at author time
	windowc_slot_change(&wc, 0, (struct item_data)STACK(358,1,0));
	assert(wc.map_counter == 0 && !windowc_get_latest(&wc)->picked_item.id);
	windowc_destroy(&wc);
	puts("PASS client map counter survives window replacement, resets on session teardown, and replay cannot invent an allocation");
}

static void stale_rejection_boundary(void) {
	/* [740c583 NetClientHandler.handleTransaction/handleWindowItems]: full
	 * server replacement wins even if our richer prediction history has no ID. */
	for(unsigned drop = 0; drop < 2; drop++) for(unsigned stale = 0; stale < 2; stale++) {
		struct window_container wc;
		struct client_session session = {0};
		assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63)); session.windows[3] = &wc;
		windowc_slot_change(&wc, 0, (struct item_data)STACK(1,12,0));
		windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, (struct item_data)STACK(3,5,0));
		uint16_t id;
		assert(drop ? windowc_new_drop_action(&wc, &id, false) : windowc_new_quick_action(&wc, &id, 0));
		struct client_rpc rejected = {.type = CRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = 3, .action_id = stale ? 0 : 999, .accepted = false}};
		assert(client_session_window_reply(&session, &rejected));
		assert(!windowc_actions_pending(&wc) && !wc.quick_pending && wc.correction_pending);
		assert(!windowc_new_quick_action(&wc, &id, 0));
		struct client_rpc full = {.type = CRPC_WINDOW_ITEMS,
			.payload.window_items = {.window = 3, .count = 63}};
		full.payload.window_items.items[0] = (struct item_data)STACK(1,12,0);
		assert(client_session_window_reply(&session, &full));
		struct client_rpc cursor = {.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot = {.window = 3, .slot = SPECIAL_SLOT_PICKED_ITEM,
				.item = STACK(3,5,0)}};
		assert(client_session_window_reply(&session, &cursor));
		windowc_action_apply_result(&wc, id, true); // delayed ACK of retired request
		assert(!windowc_actions_pending(&wc) && !wc.correction_pending);
		assert(windowc_get_latest(&wc)->items[0].count == 12 && !windowc_get_latest(&wc)->items[62].id);
		assert(windowc_get_latest(&wc)->picked_item.count == 5);
		response_count = 0; svin_process_messages(capture_response, NULL, false);
		assert(response_count == 1 && response.payload.window_transaction.action_id == (stale ? 0 : 999));
		windowc_destroy(&wc);
	}
	/* Generation replacement and disconnect retire snapshots AND an unsent
	 * transaction reply. A late old-generation reply cannot touch the new UI. */
	for(unsigned replacement = 0; replacement < 2; replacement++) for(unsigned drop = 0; drop < 2; drop++) {
		struct client_session session = {0};
		assert(client_session_bind_local(&session, 10));
		struct window_container* wc = session.windows[0];
		windowc_slot_change(wc, 9, (struct item_data)STACK(1,12,0));
		windowc_slot_change(wc, SPECIAL_SLOT_PICKED_ITEM, (struct item_data)STACK(3,5,0));
		uint16_t id;
		assert(drop ? windowc_new_drop_action(wc, &id, false) : windowc_new_quick_action(wc, &id, 9));
		wc->transaction_reply_pending = wc->correction_pending = true;
		wc->transaction_reply_action = id;
		if(replacement) assert(client_session_bind_local(&session, 11));
		else { assert(client_session_unbind_local(&session, 10)); assert(client_session_bind_local(&session, 11)); }
		struct client_rpc old = {.type = CRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = 0, .action_id = id, .accepted = false}};
		assert(!client_session_local_reply(&session, 10, &old));
		assert(!windowc_actions_pending(session.windows[0]));
		assert(!session.windows[0]->transaction_reply_pending && !session.windows[0]->correction_pending);
		client_session_flush_window_transactions(&session);
		response_count = 0; svin_process_messages(capture_response, NULL, false); assert(!response_count);
		client_session_clear_windows(&session);
	}
	puts("PASS unknown/stale rejection with Quick Move/drop, authoritative replacement, retired ACKs, disconnect and generation replacement");
}

static void special_slot_goldens(void) {
	/* SlotArmor.java:15-26: slot limit one, matching armor or pumpkin helmet.
	 * Container.java:102-110 / InventoryCraftResult.java:20-25: right pickup
	 * is ceil-half for furnace output, but the WHOLE crafting result.
	 * Container.java:132-141: output-to-cursor takes the whole result or none. */
	const struct {
		enum window_type type;
		size_t slot;
		struct click_case click;
	} cases[] = {
		{WINDOW_TYPE_INVENTORY, 5, {"pumpkin helmet limit", false,
			{0}, STACK(86,7,0), STACK(86,1,0), STACK(86,6,0)}},
		{WINDOW_TYPE_INVENTORY, 6, {"pumpkin wrong armor slot", false,
			{0}, STACK(86,7,0), {0}, STACK(86,7,0)}},
		{WINDOW_TYPE_INVENTORY, 5, {"armor right swap", true,
			STACK(298,1,20), STACK(306,1,30), STACK(306,1,30), STACK(298,1,20)}},
		{WINDOW_TYPE_INVENTORY, 0, {"player whole right craft", true,
			STACK(5,4,0), {0}, {0}, STACK(5,4,0)}},
		{WINDOW_TYPE_WORKBENCH, 0, {"workbench whole right craft", true,
			STACK(5,4,0), {0}, {0}, STACK(5,4,0)}},
		{WINDOW_TYPE_WORKBENCH, 0, {"craft onto nonsubtype cursor", true,
			STACK(5,4,0), STACK(5,60,3), {0}, STACK(5,64,3)}},
		{WINDOW_TYPE_WORKBENCH, 0, {"craft cursor insufficient room", true,
			STACK(5,4,0), STACK(5,61,0), STACK(5,4,0), STACK(5,61,0)}},
		{WINDOW_TYPE_FURNACE, 2, {"furnace right pickup", true,
			STACK(265,7,0), {0}, STACK(265,3,0), STACK(265,4,0)}},
		{WINDOW_TYPE_FURNACE, 2, {"furnace whole onto cursor", true,
			STACK(265,7,0), STACK(265,57,3), {0}, STACK(265,64,3)}},
		{WINDOW_TYPE_FURNACE, 2, {"furnace blocks placement", false,
			{0}, STACK(265,7,0), {0}, STACK(265,7,0)}}
	};
	static struct server_local server;
	for(size_t k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
		enum window_type type = cases[k].type;
		size_t capacity = type == WINDOW_TYPE_INVENTORY ? 45
			: type == WINDOW_TYPE_WORKBENCH ? 46 : 39;
		uint8_t window = type == WINDOW_TYPE_INVENTORY ? 0
			: type == WINDOW_TYPE_WORKBENCH ? 1 : 2;
		const struct click_case* c = &cases[k].click;
		struct inventory storage;
		struct inventory* inv = window ? &storage : &server.player.inventory;
		struct window_container wc;
		assert(inventory_create(inv, NULL, NULL, NULL, capacity, 0,0,0));
		assert(windowc_create(&wc, type, capacity));
		inv->logic = logic_for(type, capacity);
		inv->reply = capture; inv->user = &server; inv->y = -1;
		server.player.active_inventory = inv;
		server.player.health = 160;
		server.player.has_pos = server.player.finished_loading = true;
		inv->items[cases[k].slot] = c->slot;
		inv->picked_item = c->cursor;
		bool craft = cases[k].slot == 0;
		if(craft) inv->items[1] = (struct item_data)STACK(17,1,0);
		struct item_data want[46];
		memcpy(want, inv->items, capacity * sizeof(*want));
		want[cases[k].slot] = c->slot_after;
		if(craft && !c->slot_after.id) want[1] = (struct item_data){0};
		for(size_t slot = 0; slot < capacity; slot++) windowc_slot_change(&wc, slot, inv->items[slot]);
		windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, c->cursor);
		uint16_t id;
		assert(windowc_new_action(&wc, &id, c->right, cases[k].slot));
		assert(equal(windowc_get_latest(&wc)->revision.expected, c->slot));
		struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = window, .slot = cases[k].slot,
				.right_click = c->right, .action_id = id, .expected = c->slot}};
		call = click_wire(call);
		reply_count = 0;
		server_local_process_window_for(&server, &server.player, &call);
		assert(reply_count == 1 && replies[0].payload.window_transaction.accepted);
		reply_wire(&replies[0], window, &wc);
		for(size_t slot = 0; slot < capacity; slot++) {
			assert(equal(inv->items[slot], want[slot]));
			assert(equal(windowc_get_latest(&wc)->items[slot], want[slot]));
		}
		assert(equal(inv->picked_item, c->cursor_after));
		assert(equal(windowc_get_latest(&wc)->picked_item, c->cursor_after));
		inv->logic = NULL; inventory_destroy(inv, NULL); windowc_destroy(&wc);
		printf("PASS %s\n", c->name);
	}
}

static size_t pressure_drained, pressure_echoes;
static struct server_rpc_source pressure_source;
static void drain_pressure(struct server_rpc_source source, struct server_rpc* call, void* user) {
	(void)user;
	pressure_drained++;
	if(call->type == SRPC_WINDOW_TRANSACTION) {
		pressure_echoes++;
		assert(svin_source_equal(source, pressure_source));
		assert(call->payload.window_transaction.window == 3);
		assert(call->payload.window_transaction.action_id == 91);
		assert(call->payload.window_transaction.accepted);
	} else assert(call->type == SRPC_WINDOW_CLICK);
}

static void transaction_backpressure(void) {
	/* NetClientHandler.handleTransaction's positive response must survive the
	 * port's bounded transport, including the local guest ownership token. */
	for(unsigned guest = 0; guest < 2; guest++) {
		struct client_session session = {0};
		struct window_container wc;
		assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
		session.windows[3] = &wc;
		windowc_slot_change(&wc, 0, (struct item_data)STACK(1,12,0));
		windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, (struct item_data)STACK(3,5,0));
		uint16_t action;
		assert(guest ? windowc_new_drop_action(&wc, &action, false)
			: windowc_new_quick_action(&wc, &action, 0));
		session.local_bound = guest != 0;
		session.local_generation = guest ? 123 : 0;
		pressure_source = (struct server_rpc_source){.slot = guest,
			.generation = session.local_generation};
		struct server_rpc filler = {.type = SRPC_WINDOW_CLICK};
		size_t queued = 0;
		while(svin_rpc_send(&filler)) { queued++; assert(queued < 1024); }
		assert(queued > 0);
		struct client_rpc rejected = {.type = CRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = 3, .action_id = 91, .accepted = false}};
		assert(client_session_window_reply(&session, &rejected));
		assert(wc.transaction_reply_pending && wc.correction_pending);
		assert(!windowc_actions_pending(&wc) && !wc.quick_pending);
		pressure_drained = pressure_echoes = 0;
		while(pressure_drained < queued) {
			svin_process_owned_messages(drain_pressure, NULL, false);
			svin_rpc_flush_pending();
		}
		assert(!pressure_echoes);
		client_session_flush_window_transactions(&session);
		assert(!wc.transaction_reply_pending && wc.correction_pending);
		svin_process_owned_messages(drain_pressure, NULL, false);
		client_session_flush_window_transactions(&session);
		svin_process_owned_messages(drain_pressure, NULL, false);
		assert(pressure_echoes == 1);
		windowc_destroy(&wc);
	}
	puts("PASS transaction response survives full transport exactly once, primary and guest ownership");
}

static void synthetic_sign_contract(void) {
	/* This port-only character window predates item click parity. It is not
	 * Packet102/Container.slotClick and must not inherit the item recovery gate. */
	static struct server_local server;
	struct inventory inv;
	struct window_container wc;
	struct client_session session = {0};
	assert(inventory_create(&inv, NULL, NULL, NULL, SIGN_SIZE, 0,0,0));
	assert(windowc_create(&wc, WINDOW_TYPE_SIGN, SIGN_SIZE));
	inv.logic = &inventory_logic_sign; inv.reply = capture;
	server.player.active_inventory = &inv;
	server.player.health = 160;
	server.player.has_pos = server.player.finished_loading = true;
	session.windows[WINDOWC_SIGN] = &wc;
	for(size_t slot = 0; slot < SIGN_SIZE; slot++) {
		inv.items[slot] = (struct item_data)STACK(1,' ',0);
		windowc_slot_change(&wc, slot, inv.items[slot]);
	}
	for(unsigned k = 0; k < 2; k++) {
		uint16_t id;
		assert(windowc_new_action(&wc, &id, true, 0));
		struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = WINDOWC_SIGN, .slot = 0,
				.right_click = true, .action_id = id}};
		reply_count = 0;
		server_local_process_window_for(&server, &server.player, &call);
		assert(reply_count == 2 && !replies[0].payload.window_transaction.accepted);
		assert(replies[1].type == CRPC_INVENTORY_SLOT);
		for(size_t r = 0; r < reply_count; r++) assert(client_session_window_reply(&session, &replies[r]));
		assert(inv.items[0].count == '!' + k);
		assert(equal(windowc_get_latest(&wc)->items[0], inv.items[0]));
		assert(!inv.transaction_pending && !wc.correction_pending && !wc.transaction_reply_pending);
	}
	uint16_t id;
	assert(windowc_new_sign_set_action(&wc, &id, 15, 'A'));
	struct server_rpc denied = {.type = SRPC_SIGN_SET,
		.payload.sign_set = {.window = WINDOWC_SIGN, .slot = 15, .ch = 'A', .action_id = id}};
	reply_count = 0;
	server_local_process_window_for(&server, &server.player, &denied);
	assert(reply_count == 1 && !replies[0].payload.window_transaction.accepted);
	assert(client_session_window_reply(&session, &replies[0]));
	assert(!wc.correction_pending && !wc.transaction_reply_pending);
	assert(windowc_get_latest(&wc)->items[15].count == ' ');
	response_count = 0;
	svin_process_messages(capture_response, NULL, false);
	assert(!response_count);
	inv.logic = NULL; inventory_destroy(&inv, NULL); windowc_destroy(&wc);
	puts("PASS synthetic sign compatibility: character echoes and rejected SET do not install item recovery gates");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	recipe_init();
	assert(svin_init());
	assert(clin_init());
	assert(server_remote_init(&remote));
	((struct server_remote_impl*)remote.impl)->state = REMOTE_PLAY;
	ordinary_goldens();
	transfer_goldens();
	rejection_goldens();
	drop_goldens();
	provider_equivalence();
	persistent_craft_replies();
	assert(glfwInit());
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* gl = glfwCreateWindow(32, 32, "map upload golden", NULL, NULL);
	assert(gl);
	glfwMakeContextCurrent(gl);
	assert(glewInit() == GLEW_OK);
	assert(tex_gfx_load(&texture_map_live, calloc(128 * 128, 4), 128, 128, TEX_FMT_RGBA32, false));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	/* [740c583 Item.java map setIconCoord(12,3); ItemRenderer's map branch
	 * is first person only]. Compact/full inventory use the generic icon. */
	assert(items[358]->render_data.item.texture_x == 12);
	assert(items[358]->render_data.item.texture_y == 3);
	map_residency_golden();
	map_retry_golden();
	map_pressure_golden();
	consecutive_map_crafts();
	tex_gfx_clear(&texture_map_live);
	glfwDestroyWindow(gl);
	glfwTerminate();
	map_counter_lifetime();
	negative_map_import();
	stale_rejection_boundary();
	special_slot_goldens();
	transaction_backpressure();
	synthetic_sign_contract();
	server_remote_destroy(&remote);
	puts("PASS Beta window click goldens");
	return 0;
}
