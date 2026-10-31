/* Menu feel only: production adapters and poll clock, with scripted owner frames. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/game_state.h"
#include "game/gui/container_menu.h"
#include "game/gui/local_menu_host.h"
#include "platform/input_owner.h"
#include "platform/time.h"

static unsigned now_ms, sends;
static struct input_player_frame frames[2];
static struct container_input in[2];

ptime_t __wrap_time_get(void) {
	ptime_t origin = {0};
	return time_add_ms(origin, now_ms);
}
const struct input_player_frame* __wrap_input_player_frame(unsigned player) {
	return player < 2 ? &frames[player] : NULL;
}
static bool capture(void* user, struct server_rpc* rpc) {
	(void)user; (void)rpc; sends++; return true;
}
static void level(unsigned player, bool connected, bool down) {
	struct input_player_frame* f = &frames[player];
	bool previous = f->down[IB_GUI_PAGE_SWAP];
	f->connected = connected;
	f->down[IB_GUI_PAGE_SWAP] = down;
	f->pressed[IB_GUI_PAGE_SWAP] = down && !previous;
	f->released[IB_GUI_PAGE_SWAP] = !down && previous;
}
static void poll(unsigned ms, bool p1, bool p2) {
	now_ms += ms;
	level(0, true, p1); level(1, true, p2);
	input_poll();
}
static void read(void) {
	container_menu_read_input(&in[0], 0, 0, 640, 480);
	container_menu_read_player_owned(&in[1], 1, &frames[1], 0, 240, 640, 240);
}
static void step(unsigned ms, bool p1, bool p2) { poll(ms, p1, p2); read(); }
static void reset(enum input_category family) {
	for(unsigned p = 0; p < 2; p++) {
		input_menu_page_cancel(p);
		frames[p] = (struct input_player_frame){.connected = true, .category = family};
	}
	step(1, false, false); /* A screen must observe neutral before arming. */
}
static void quiet(void) {
	for(unsigned p = 0; p < 2; p++) assert(!in[p].page_swap && !in[p].page_hold);
}
static void boundaries(enum input_category family) {
	reset(family);
	for(unsigned tap = 0; tap < 10; tap++) {
		step(1, true, true); quiet();
		assert(in[0].page_pressed && in[1].page_held);
		step(tap == 9 ? 799 : 20, false, false);
		assert(in[0].page_swap && in[1].page_swap);
		assert(in[0].page_released && !in[0].page_held && !in[0].page_hold);
		step(1, false, false); quiet();
	}
	/* Both edges can arrive between polls, without an observed held frame. */
	poll(1, false, false);
	for(unsigned p = 0; p < 2; p++) {
		frames[p].pressed[IB_GUI_PAGE_SWAP] = true;
		frames[p].released[IB_GUI_PAGE_SWAP] = true;
	}
	read();
	for(unsigned p = 0; p < 2; p++) {
		assert(in[p].page_swap && !in[p].page_hold && !in[p].page_held);
		assert(in[p].page_pressed && in[p].page_released);
	}
	step(800, false, false); quiet();
	step(1, true, true);
	step(799, true, true); quiet(); assert(in[0].page_held_ms == 799);
	step(1, true, true);
	assert(in[0].page_hold && in[1].page_hold && !in[0].page_swap);
	assert(in[0].page_held_ms == 800);
	struct container_input repeated;
	container_menu_read_player(&repeated, &frames[1], 0, 240, 640, 240);
	assert(repeated.page_hold && repeated.page_held_ms == 800);
	step(800, true, true); quiet();
	step(1, false, false); quiet();
	step(1, true, true);
	step(800, false, false); quiet(); /* No late tap or release-fired HOLD. */
	step(1, true, true);
	step(800, true, true); assert(in[0].page_hold && in[1].page_hold);
	step(1, false, false); quiet();
}
static void independent(void) {
	reset(INPUT_CAT_NUNCHUK);
	step(1, true, false);
	step(400, true, true); quiet();
	step(100, true, false); assert(in[1].page_swap && !in[0].page_swap);
	step(300, true, false); assert(in[0].page_hold && !in[1].page_hold);
	step(1, false, false); quiet();
}
static void cancellation(void) {
	reset(INPUT_CAT_NUNCHUK);
	step(1, true, true);
	frames[0].connected = frames[1].connected = false;
	read(); assert(in[0].suspended && in[1].suspended); quiet();
	step(900, true, true); quiet();
	step(1, false, false); quiet();
	step(1, true, true);
	frames[0].category = frames[1].category = INPUT_CAT_CLASSIC_CONTROLLER;
	step(900, true, true); quiet();
	step(1, false, false); quiet();
	step(1, true, true);
	poll(900, true, true); /* Another screen consumed this poll. */
	step(1, false, false); quiet();
	step(1, true, true);
	gstate.players[0].current_screen = &screen_crafting;
	gstate.players[1].current_screen = &screen_crafting;
	step(900, true, true); quiet();
	step(1, false, false); quiet();
	step(1, true, true);
	frames[0].pressed[IB_INVENTORY] = frames[1].pressed[IB_INVENTORY] = true;
	step(800, true, true); quiet();
	frames[0].pressed[IB_INVENTORY] = frames[1].pressed[IB_INVENTORY] = false;
	step(1, false, false); quiet();
	step(1, true, true);
	input_player_unbind(0); input_player_unbind(1);
	step(900, true, true); quiet();
	step(1, false, false); quiet();
}
static void compact_lifecycle(void) {
	struct client_session* s = &gstate.players[1];
	struct window_container inventory;
	assert(windowc_create(&inventory, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	s->windows[WINDOWC_INVENTORY] = &inventory;
	s->visible_window = -1;
	assert(local_menu_host_open(s, 1, WINDOWC_INVENTORY, 640, 240));
	reset(INPUT_CAT_GAMECUBE);
	step(1, true, true);
	local_menu_host_reset(s); /* P2 closing must not cancel P1's hold. */
	step(800, true, true);
	assert(in[0].page_hold && !in[1].page_hold);
	step(1, false, false); quiet();
	assert(local_menu_host_open(s, 1, WINDOWC_INVENTORY, 640, 240));
	struct inventory* items = windowc_get_latest(&inventory);
	struct item_data before[INVENTORY_SIZE];
	memcpy(before, items->items, sizeof(before));
	struct item_data picked = items->picked_item;
	for(unsigned k = 0; k < 5; k++) {
		poll(k == 2 ? 800 : 1, false, k == 1 || k == 2);
		assert(local_menu_host_frame(s, 1, &frames[1], 0, 240, 640, 240,
			.016F, capture, NULL));
		struct input_menu_page event;
		input_menu_page_read(1, &frames[1], &event);
		assert(event.hold == (k == 2));
	}
	assert(!sends && !memcmp(before, items->items, sizeof(before))
		&& !memcmp(&picked, &items->picked_item, sizeof(picked)));
	assert(!container_menu_sort_supported(&inventory));
	local_menu_host_reset(s);
	s->windows[WINDOWC_INVENTORY] = NULL;
	windowc_destroy(&inventory);
}
int main(void) {
	gstate.config_user.root = json_parse_string("{}");
	assert(gstate.config_user.root);
	input_init();
	gstate.players[0].current_screen = gstate.players[1].current_screen = &screen_inventory;
	boundaries(INPUT_CAT_NUNCHUK);
	boundaries(INPUT_CAT_CLASSIC_CONTROLLER);
	boundaries(INPUT_CAT_GAMECUBE);
	independent(); cancellation(); compact_lifecycle();
	config_destroy(&gstate.config_user);
	puts("test_inventory_page_hold: PASS (full + compact, P1/P2, 800 ms, cancellation, no Sort)");
	return 0;
}
