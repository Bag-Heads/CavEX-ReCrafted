/*
	Creative phase-2 item picker (docs/creative_mode.md phase 2) -- NET-NEW
	addition under the 2026-08-25 maintainer exception, so these are DESIGN
	pins, not Beta goldens: the list's include-predicate/ordering contract
	and the SRPC_CREATIVE_GIVE handler's gates (creative-off no-op, registry
	validation, hotbar-first placement, full-inventory no-op, server-chosen
	stack size).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/gui/creative_picker_list.h"
#include "game/game_state.h"
#include "game/gui/local_menu_host.h"
#include "platform/input_owner.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_creative.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)

static void test_list_contract(void) {
	size_t n = creative_picker_count();
	assert(n > 0);

	/* Every entry is registered + renderable (the include predicate), and
	 * count is the item's max_stack. No (id, durability) pair repeats. */
	static bool seen[ITEMS_MAX][16];
	memset(seen, 0, sizeof(seen));
	size_t dye_variants = 0;
	for(size_t k = 0; k < n; k++) {
		struct item_data it;
		assert(creative_picker_entry(k, &it));
		assert(it.id > 0 && it.id < ITEMS_MAX);
		assert(items[it.id] && items[it.id]->renderItem);
		assert(it.count == items[it.id]->max_stack);
		size_t dmg = it.id == ITEM_DYE ? it.durability : 0;
		assert(dmg < 16);
		assert(!seen[it.id][dmg]);
		seen[it.id][dmg] = true;
		if(it.id == ITEM_DYE)
			dye_variants++;
	}
	/* All 16 dye colors are offered (2 featured + 14 in the sweep). */
	assert(dye_variants == 16);

	/* Featured-first: the first entries are the hand-curated build blocks in
	 * their table order (amahpour UX borrow, re-implemented). */
	struct item_data first;
	assert(creative_picker_entry(0, &first));
	assert(first.id == BLOCK_STONE);
	struct item_data second;
	assert(creative_picker_entry(1, &second));
	assert(second.id == BLOCK_COBBLESTONE);

	/* One past the end fails. */
	struct item_data out;
	assert(!creative_picker_entry(n, &out));
	puts("  picker: list contract 1/1");
}

static void test_give_handler(void) {
	assert(clin_init());
	assert(svin_init());

	struct server_local server = {0};
	assert(inventory_create(&server.player.inventory, &inventory_logic_player,
							&server, &server.player, INVENTORY_SIZE, 0, 0, 0));
	server.player.active_inventory = &server.player.inventory;

	struct server_rpc give = {
		.type = SRPC_CREATIVE_GIVE,
		.payload.creative_give.id = BLOCK_STONE,
		.payload.creative_give.durability = 0,
	};

	/* Gate 1: creative OFF -> nothing lands anywhere. */
	server.creative_mode = false;
	server_creative_handle_give(&server, &give);
	for(size_t i = 0; i < INVENTORY_SIZE; i++) {
		struct item_data cur;
		assert(!inventory_get_slot(&server.player.inventory, i, &cur)
			   || !item_get(&cur));
	}

	/* Gate 2: creative ON -> a full stone stack in the FIRST hotbar slot. */
	server.creative_mode = true;
	server_creative_handle_give(&server, &give);
	struct item_data got;
	assert(inventory_get_slot(&server.player.inventory, INVENTORY_SLOT_HOTBAR,
							  &got));
	assert(got.id == BLOCK_STONE);
	assert(got.count == items[BLOCK_STONE]->max_stack);

	/* Gate 3: unregistered id -> no-op (next hotbar slot stays empty). */
	struct server_rpc bogus = give;
	bogus.payload.creative_give.id = 255; /* no Beta block/item at 255 */
	if(!items[255]) {
		server_creative_handle_give(&server, &bogus);
		struct item_data cur;
		assert(!inventory_get_slot(&server.player.inventory,
								   INVENTORY_SLOT_HOTBAR + 1, &cur)
			   || !item_get(&cur));
	}

	/* Gate 4: hotbar-first order -- the second grant lands in hotbar+1. */
	server_creative_handle_give(&server, &give);
	assert(inventory_get_slot(&server.player.inventory,
							  INVENTORY_SLOT_HOTBAR + 1, &got));
	assert(got.id == BLOCK_STONE);

	/* Gate 5: full inventory -> silent no-op (no crash, nothing replaced). */
	struct item_data fill = {.id = BLOCK_DIRT, .count = 1};
	for(size_t i = 0; i < INVENTORY_SIZE_HOTBAR; i++)
		inventory_set_slot(&server.player.inventory,
						   INVENTORY_SLOT_HOTBAR + i, fill);
	for(size_t i = 0; i < INVENTORY_SIZE_MAIN; i++)
		inventory_set_slot(&server.player.inventory, INVENTORY_SLOT_MAIN + i,
						   fill);
	server_creative_handle_give(&server, &give);
	assert(inventory_get_slot(&server.player.inventory, INVENTORY_SLOT_HOTBAR,
							  &got));
	assert(got.id == BLOCK_DIRT && got.count == 1);

	inventory_destroy(&server.player.inventory, &server.player);
	puts("  picker: give handler 1/1");
}

/* Drive the same owner-local host that main uses, without a global screen. */
static unsigned split_grants;
static bool split_give(void* user, struct server_rpc* rpc) {
	assert(rpc->type == SRPC_CREATIVE_GIVE);
	server_creative_handle_give(user, rpc);
	split_grants++;
	return true;
}
static unsigned latch_sends;
static bool latch_send(void* user, struct server_rpc* rpc) {
	(void)user; (void)rpc;
	latch_sends++;
	return true;
}
static unsigned owner_replies;
static void guest_queue_reply(const struct client_rpc* rpc, void* user) {
	struct server_local* s = user;
	assert(rpc->type == CRPC_INVENTORY_SLOT);
	assert(clin_rpc_send_local(rpc, 1, s->guest_generation));
	owner_replies++;
}
static void rejected_grant(struct server_local* s, struct server_rpc_source source,
	bool ingress) {
	struct item_data host_before[INVENTORY_SIZE], guest_before[INVENTORY_SIZE];
	memcpy(host_before, s->player.inventory.items, sizeof host_before);
	memcpy(guest_before, s->guest_actor->inventory.items, sizeof guest_before);
	unsigned replies_before = owner_replies;
	struct server_rpc grant = {.type = SRPC_CREATIVE_GIVE,
		.payload.creative_give = {.id = BLOCK_STONE}};
	assert(svin_rpc_send_from(source, &grant) == ingress);
	server_local_test_process_messages(s);
	clin_update_drain();
	assert(owner_replies == replies_before);
	assert(!memcmp(host_before, s->player.inventory.items, sizeof host_before));
	assert(!memcmp(guest_before, s->guest_actor->inventory.items, sizeof guest_before));
}
static void test_split_grant_transport(void) {
	/* The real mailbox/owned dispatch and client delivery, with only the
	 * already-admitted actor and its normal reply-hook wiring fixtured. */
	while(clin_test_inbox_pop(NULL, NULL)) {}
	struct server_local s = {.creative_mode = true, .guest_generation = 73,
		.guest_gameplay = true, .guest_confirmed = true};
	struct server_player guest = {.health = MAX_PLAYER_HEALTH,
		.has_pos = true, .finished_loading = true};
	s.guest_actor = &guest;
	assert(inventory_create(&s.player.inventory, &inventory_logic_player,
		&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
	assert(inventory_create(&guest.inventory, &inventory_logic_player,
		&s, &guest, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	guest.active_inventory = &guest.inventory;
	guest.inventory.reply = guest_queue_reply;
	guest.inventory.reply_user = &s;
	struct window_container host_window;
	assert(windowc_create(&host_window, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	assert(!gstate.players[0].windows[WINDOWC_INVENTORY]);
	gstate.players[0].windows[WINDOWC_INVENTORY] = &host_window;
	assert(client_session_bind_local(&gstate.players[1], s.guest_generation));
	struct inventory* host_mirror = windowc_get_latest(&host_window);
	struct inventory* guest_mirror = windowc_get_latest(
		gstate.players[1].windows[WINDOWC_INVENTORY]);
	struct item_data host_before[INVENTORY_SIZE];
	memcpy(host_before, s.player.inventory.items, sizeof host_before);
	struct item_data mirror_before[INVENTORY_SIZE];
	memcpy(mirror_before, host_mirror->items, sizeof mirror_before);
	struct server_rpc_source p2 = {.slot = 1, .generation = s.guest_generation};
	svin_set_split_accept(false);
	rejected_grant(&s, p2, false);
	svin_set_split_accept(true);
	struct server_rpc grant = {.type = SRPC_CREATIVE_GIVE,
		.payload.creative_give = {.id = BLOCK_STONE}};
	assert(svin_rpc_send_from(p2, &grant)
		&& "P2 creative selection must reach the owner-routed server mailbox");
	server_local_test_process_messages(&s);
	assert(owner_replies == 1);
	clin_update_drain();
	struct item_data got;
	assert(inventory_get_slot(&guest.inventory, INVENTORY_SLOT_HOTBAR, &got));
	assert(got.id == BLOCK_STONE && got.count == items[BLOCK_STONE]->max_stack);
	assert(inventory_get_slot(guest_mirror, INVENTORY_SLOT_HOTBAR, &got));
	assert(got.id == BLOCK_STONE && got.count == items[BLOCK_STONE]->max_stack);
	assert(!memcmp(host_before, s.player.inventory.items, sizeof host_before));
	assert(!memcmp(mirror_before, host_mirror->items, sizeof mirror_before));
	rejected_grant(&s, (struct server_rpc_source){1, p2.generation - 1}, true);
	s.guest_ticket = 1; s.guest_confirmed = false;
	rejected_grant(&s, p2, true);
	s.guest_ticket = 0; s.guest_confirmed = true; s.guest_gameplay = false;
	rejected_grant(&s, p2, true);
	s.guest_gameplay = true; guest.health = 0;
	rejected_grant(&s, p2, true);
	guest.health = MAX_PLAYER_HEALTH; guest.sleeping = true;
	rejected_grant(&s, p2, true);
	guest.sleeping = false; guest.rz.recovering = true;
	rejected_grant(&s, p2, true);
	guest.rz.recovering = false; guest.finished_loading = false;
	rejected_grant(&s, p2, true); /* admitted actor still loading */
	guest.finished_loading = true; s.input_ack_pending = 1;
	rejected_grant(&s, p2, true);
	s.input_ack_pending = 0; s.creative_mode = false;
	rejected_grant(&s, p2, true);
	s.creative_mode = true;
	for(size_t i = INVENTORY_SLOT_MAIN; i < INVENTORY_SIZE; i++)
		inventory_set_slot(&guest.inventory, i, (struct item_data){.id = BLOCK_DIRT, .count = 1});
	rejected_grant(&s, p2, true);
	/* The original host wrapper still delivers its ordinary untagged reply. */
	server_creative_handle_give(&s, &grant);
	clin_update_drain();
	assert(owner_replies == 1);
	assert(inventory_get_slot(host_mirror, INVENTORY_SLOT_HOTBAR, &got));
	assert(got.id == BLOCK_STONE && got.count == items[BLOCK_STONE]->max_stack);
	assert(client_session_unbind_local(&gstate.players[1], s.guest_generation));
	gstate.players[0].windows[WINDOWC_INVENTORY] = NULL;
	windowc_destroy(&host_window);
	inventory_destroy(&guest.inventory, &guest);
	inventory_destroy(&s.player.inventory, &s.player);
	svin_set_split_accept(false);
	puts("  picker: owned mailbox grants, client delivery and rejection gates 1/1");
}

static void test_split_picker(void) {
	struct client_session host = {.visible_window = -1};
	struct client_session guest = {.visible_window = -1, .local_generation = 73};
	struct window_container window;
	assert(windowc_create(&window, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	guest.windows[WINDOWC_INVENTORY] = &window;
	host.current_screen = guest.current_screen = &screen_ingame;
	struct client_session untouched = host;
	struct server_local p2 = {.creative_mode = true};
	assert(inventory_create(&p2.player.inventory, &inventory_logic_player,
		&p2, &p2.player, INVENTORY_SIZE, 0, 0, 0));
	p2.player.active_inventory = &p2.player.inventory;
	gstate.config_creative = true;
	struct input_player_frame frame = {.connected = true,
		.category = INPUT_CAT_GAMECUBE};
	frame.pressed[IB_OPEN_INVENTORY] = true;
	assert(local_menu_host_frame(&guest, 1, &frame, 0, 240, 640, 240,
		.016F, split_give, &p2));
	assert(local_menu_host_active(&guest));
	assert(!guest.container.active && "creative open must select the picker, not survival inventory");
	assert(guest.current_screen == &screen_ingame);
	assert(!local_menu_host_active(&host));
	assert(!memcmp(&host, &untouched, sizeof host));
	frame.pressed[IB_OPEN_INVENTORY] = false;
	frame.pressed[IB_GUI_CLICK] = true;
	assert(local_menu_host_frame(&guest, 1, &frame, 0, 240, 640, 240,
		.016F, split_give, &p2));
	struct item_data got;
	assert(split_grants == 1);
	assert(inventory_get_slot(&p2.player.inventory, INVENTORY_SLOT_HOTBAR, &got));
	assert(got.id == BLOCK_STONE && got.count == items[BLOCK_STONE]->max_stack);
	assert(!memcmp(&host, &untouched, sizeof host));
	/* Inventory is an owner-local escape to the real survival container. */
	const enum input_button route[] = {IB_GUI_DOWN, IB_GUI_DOWN,
		IB_GUI_RIGHT, IB_GUI_RIGHT, IB_GUI_CLICK};
	for(size_t k = 0; k < sizeof route / sizeof *route; k++) {
		memset(frame.pressed, 0, sizeof frame.pressed);
		frame.pressed[route[k]] = true;
		assert(local_menu_host_frame(&guest, 1, &frame, 0, 240, 640, 240,
			.016F, split_give, &p2));
	}
	assert(guest.menu_host.mode == LOCAL_MENU_CONTAINER && guest.container.active);
	assert(guest.menu_host.window_identity == &window && guest.menu_host.player == 1);
	assert(!memcmp(&host, &untouched, sizeof host));
	local_menu_host_reset(&guest);
	memset(frame.pressed, 0, sizeof frame.pressed);
	frame.pressed[IB_OPEN_INVENTORY] = true;
	assert(local_menu_host_frame(&guest, 1, &frame, 0, 240, 640, 240,
		.016F, split_give, &p2));
	frame.pressed[IB_GUI_CLICK] = false;
	frame.pressed[IB_INVENTORY] = true;
	assert(local_menu_host_frame(&guest, 1, &frame, 0, 240, 640, 240,
		.016F, split_give, &p2));
	assert(!local_menu_host_active(&guest));
	assert(!memcmp(&host, &untouched, sizeof host));
	gstate.config_creative = false;
	inventory_destroy(&p2.player.inventory, &p2.player);
	windowc_destroy(&window);
	puts("  picker: P2 open/grant/close isolated from P1 1/1");
}

static void test_split_picker_lifecycle(void) {
	struct client_session p1 = {.visible_window = -1}, p2 = {.visible_window = -1};
	struct input_player_frame f = {.connected = true, .category = INPUT_CAT_GAMECUBE};
	gstate.config_creative = true;
	f.pressed[IB_OPEN_INVENTORY] = true;
	assert(local_menu_host_frame(&p1, 0, &f, 0, 0, 640, 240, .016F, split_give, NULL));
	assert(local_menu_host_frame(&p2, 1, &f, 0, 240, 640, 240, .016F, split_give, NULL));
	struct creative_picker saved = p1.menu_host.picker;
	f.pressed[IB_OPEN_INVENTORY] = false; f.pressed[IB_GUI_RIGHT] = true;
	assert(local_menu_host_frame(&p2, 1, &f, 0, 240, 640, 240, .016F, split_give, NULL));
	assert(p2.menu_host.picker.cursor == 1);
	assert(!memcmp(&saved, &p1.menu_host.picker, sizeof saved));
	f.pressed[IB_GUI_RIGHT] = false; f.pressed[IB_HOME] = true;
	assert(local_menu_host_frame(&p2, 1, &f, 0, 240, 640, 240, .016F, split_give, NULL));
	assert(!local_menu_host_active(&p2) && local_menu_host_active(&p1));
	f.pressed[IB_HOME] = false; f.connected = false; f.pressed[IB_GUI_RIGHT] = true;
	assert(local_menu_host_frame(&p1, 0, &f, 0, 0, 640, 240, .016F, split_give, NULL));
	assert(p1.menu_host.recovery.phase == CONTROLLER_RECOVERY_LOST);
	assert(!memcmp(&saved, &p1.menu_host.picker, sizeof saved));
	local_menu_host_reset(&p1);
	gstate.config_creative = false;

	/* Empty tail cells and buttons remain valid through pane transitions. */
	struct creative_picker compact, roundtrip;
	creative_picker_open(&compact);
	compact.page = (creative_picker_count() - 1) / 18; compact.cursor = 17;
	screen_creative_picker_from_compact(&compact);
	screen_creative_picker_to_compact(&roundtrip);
	assert(roundtrip.page * 18 + roundtrip.cursor == creative_picker_count() - 1);
	/* A stationary IR sprite after a layout change cannot steal selection. */
	int selected = roundtrip.cursor;
	f = (struct input_player_frame){.connected = true, .pointer_valid = true,
		.pointer_x = 160, .pointer_y = 274, .category = INPUT_CAT_NUNCHUK};
	assert(creative_picker_update(&roundtrip, &f, 0, 240, 640, 240,
		split_give, NULL) == CREATIVE_PICKER_KEEP);
	assert(roundtrip.cursor == selected);
	assert(roundtrip.pointer_available && roundtrip.pointer_y == 34);
	f.pointer_y = 100; /* P1's pane is outside P2's mouse viewport. */
	creative_picker_update(&roundtrip, &f, 0, 240, 640, 240, split_give, NULL);
	assert(!roundtrip.pointer_available && roundtrip.cursor == selected);

	compact.cursor = 47; /* Inventory button, stable after the original 45 cells. */
	screen_creative_picker_from_compact(&compact);
	screen_creative_picker_to_compact(&roundtrip);
	assert(roundtrip.cursor == 47 && roundtrip.page * 18 < creative_picker_count());
	/* Reconnect latch: a button held across a Wiimote drop must not act. */
	{
		struct creative_picker rc;
		creative_picker_open(&rc);
		struct input_player_frame g = {.connected = true, .category = INPUT_CAT_NUNCHUK};
		struct input_player_frame lost = {.connected = false};
		latch_sends = 0;
		creative_picker_update(&rc, &lost, 0, 0, 640, 480, latch_send, NULL);
		g.down[IB_GUI_CLICK] = g.pressed[IB_GUI_CLICK] = true;
		creative_picker_update(&rc, &g, 0, 0, 640, 480, latch_send, NULL);
		assert(latch_sends == 0);
		memset(g.down, 0, sizeof g.down);
		memset(g.pressed, 0, sizeof g.pressed);
		creative_picker_update(&rc, &g, 0, 0, 640, 480, latch_send, NULL);
		g.down[IB_GUI_CLICK] = g.pressed[IB_GUI_CLICK] = true;
		creative_picker_update(&rc, &g, 0, 0, 640, 480, latch_send, NULL);
		assert(latch_sends == 1);
	}
	puts("  picker: independent cursors, Home, recovery, layout transitions 1/1");
}

int main(void) {
	blocks_init();
	items_init();
	test_list_contract();
	test_give_handler();
	test_split_picker();
	test_split_picker_lifecycle();
	test_split_grant_transport();
	puts("test_creative_picker: PASS 5/5");
	return 0;
}
