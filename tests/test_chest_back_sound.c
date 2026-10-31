/* Requirement 2 of the chest sound corrections: the dedicated Back cue on a
 * SUCCESSFUL chest/inventory return, and silence on every failed one.
 *
 * docs/ui_design/approved/specs/sound-events.md: "Back on successful
 * return/resume"; "Empty slots, blocked moves, failed safe-close attempts and
 * capacity failures are silent"; "A cancelled discard prompt uses Back;
 * opening it uses category."
 * docs/ui_design/approved/specs/crafting-chests.md:20: "Safe Back preserves a
 * held stack if it cannot be returned."
 *
 * MENU_SOUND_BACK has had its own sample since the eight-cue land; the sort
 * panel's own Back control was still falling through to the section (category)
 * cue, which is the alias this test pins shut. Only audio_menu_event is
 * linker-wrapped: the safe-close preflight, the return click and the close
 * packet are the shipping container_menu paths. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/container_menu.h"
#include "graphics/gui_util.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/menu_sound.h"

static enum menu_sound_event sounds[64];
static size_t sound_count;
void __wrap_audio_menu_event(enum menu_sound_event e, ptime_t now) {
	(void)now;
	assert(sound_count < sizeof(sounds) / sizeof(*sounds));
	sounds[sound_count++] = e;
}

static struct server_rpc sent[32];
static size_t sent_count;
static bool send_fails;
static bool send_click(void* user, struct server_rpc* call) {
	(void)user;
	if(send_fails) return false;
	assert(sent_count < sizeof(sent) / sizeof(*sent));
	sent[sent_count++] = *call;
	return true;
}

static struct window_container wc;
static struct window_container player;
static struct client_session session;
static struct entity body;
static struct container_menu menu;
static const struct container_slot slots[] = {{.x=0, .y=0, .slot=0}, {.x=36, .y=0, .slot=1}, {.x=72, .y=0, .slot=27}};

static bool step(struct container_input input) {
	return container_menu_step(&menu, slots, 3, 0, 0, &wc, WINDOWC_CHEST,
							   &input, .016F, send_click, NULL);
}

static void transaction(uint16_t action, bool accepted) {
	struct client_rpc call = {.type = CRPC_WINDOW_TRANSACTION,
		.payload.window_transaction = {.window = WINDOWC_CHEST,
			.action_id = action, .accepted = accepted}};
	assert(client_session_window_reply(&session, &call));
}

static bool closed_sent(void) {
	for(size_t k = 0; k < sent_count; k++)
		if(sent[k].type == SRPC_WINDOW_CLOSE) return true;
	return false;
}

static void reset_window(void) {
	windowc_discard_pending_actions(&wc);
	inventory_clear(windowc_get_latest(&wc));
	menu = (struct container_menu){0};
	container_menu_sort_layout(&menu, 640, 480, &wc);
	sound_count = sent_count = 0;
	send_fails = false;
}

/* Empty hand: the close packet is the successful return. */
static void test_plain_close(void) {
	reset_window();
	assert(step((struct container_input){.close = true}));
	assert(sent_count == 1 && sent[0].type == SRPC_WINDOW_CLOSE);
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_BACK);
	puts("PASS back cue: a plain close plays the Back cue once");
}

/* Held stack that fits: the safe return runs first and is itself silent; the
 * cue lands only when the close is actually sent, with the stack preserved. */
static void test_safe_close_with_held_stack(void) {
	reset_window();
	windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM,
						(struct item_data){.id = 1, .count = 7});
	assert(!step((struct container_input){.close = true}));
	assert(menu.close_requested && sent_count == 1);
	assert(sent[0].type == SRPC_WINDOW_CLICK); /* the return click, not a close */
	assert(!sound_count); /* a return step is not a completed close */
	assert(!step((struct container_input){0}));
	assert(!sound_count && sent_count == 1); /* fenced on the pending action */
	transaction(sent[0].payload.window_click.action_id, true);
	assert(step((struct container_input){0}));
	assert(closed_sent() && sound_count == 1 && sounds[0] == MENU_SOUND_BACK);
	struct inventory* inv = windowc_get_latest(&wc);
	assert(!inv->picked_item.id);
	unsigned held = 0;
	for(size_t k = wc.slot_count - 36; k < wc.slot_count; k++)
		if(inv->items[k].id == 1) held += inv->items[k].count;
	assert(held == 7); /* crafting-chests.md:20 -- the stack is preserved */
	puts("PASS back cue: safe close returns the held stack, then plays Back once");
}

/* No room to return the stack: a failed safe-close attempt is silent. */
static void test_blocked_close_silent(void) {
	reset_window();
	struct inventory* inv = windowc_get_latest(&wc);
	for(size_t k = wc.slot_count - 36; k < wc.slot_count; k++)
		windowc_slot_change(&wc, k, (struct item_data){.id = 4, .count = 64});
	windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM,
						(struct item_data){.id = 1, .count = 7});
	assert(!step((struct container_input){.close = true}));
	assert(menu.return_blocked && !menu.close_requested);
	assert(!sent_count && !sound_count);
	inv = windowc_get_latest(&wc);
	assert(inv->picked_item.id == 1 && inv->picked_item.count == 7);
	puts("PASS back cue: a blocked safe close is silent and keeps the stack");
}

/* A close the transport refused never happened: silent. */
static void test_failed_send_silent(void) {
	reset_window();
	send_fails = true;
	assert(!step((struct container_input){.close = true}));
	assert(menu.send_failed && !sent_count && !sound_count);
	puts("PASS back cue: a refused close send plays nothing");
}

/* Sort parked 2026-10-25 (maintainer: a CavEX addition, not Beta 1.7.3). No
 * input path may focus or open it, and stale state cannot resurrect it: the
 * chest closes like Beta's, D-pad UP off the top row, A, and the pointer on the
 * former header entry all leave Sort shut, silent, and request nothing. */
static void test_sort_parked(void) {
	assert(!container_menu_sort_supported(&wc) && !container_menu_sort_supported(&player));

	reset_window();
	menu.sort_open = true; /* stale: Back is the window's own close */
	assert(step((struct container_input){.close = true}));
	assert(!menu.sort_open && closed_sent());
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_BACK);

	reset_window();
	menu.sort_focus = true; /* stale: A must not open a Sort panel */
	assert(!step((struct container_input){.pick = true}));
	assert(!menu.sort_open && !menu.sort_focus);
	assert(wc.sort.status == WINDOW_SORT_IDLE);
	for(size_t k = 0; k < sent_count; k++) assert(sent[k].type == SRPC_WINDOW_CLICK); /* an ordinary slot click, as in Beta */
	for(size_t k = 0; k < sound_count; k++) assert(sounds[k] != MENU_SOUND_SECTION); /* no panel opened */

	reset_window();
	assert(!step((struct container_input){.up = true})); /* top row: no header target */
	assert(!menu.sort_focus && !menu.sort_open && !sent_count);

	reset_window();
	struct gutil_button_bounds entry;
	container_menu_sort_entry_bounds(&menu, &wc, 0, 0, &entry);
	struct container_input input = {.pointer_valid = true,
		.pointer_x = (float)(entry.x + entry.width / 2), .pointer_y = (float)(entry.y + 12)};
	assert(!step(input));
	assert(!menu.sort_focus);
	input.pick = true;
	assert(!step(input));
	assert(!menu.sort_open && !menu.sort_focus && !sent_count);
	assert(wc.sort.status == WINDOW_SORT_IDLE);
	puts("PASS Sort parked: no input path focuses or opens Sort; stale state cannot");
}

int main(void) {
	blocks_init();
	items_init();
	assert(windowc_create(&player, WINDOW_TYPE_INVENTORY, 45));
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
	gstate.players[0].windows[WINDOWC_INVENTORY] = &player;
	gstate.players[0].windows[WINDOWC_CHEST] = &wc;
	session.local_player = &body;
	session.visible_window = WINDOWC_CHEST;
	session.windows[WINDOWC_INVENTORY] = &player;
	session.windows[WINDOWC_CHEST] = &wc;

	test_plain_close();
	test_safe_close_with_held_stack();
	test_blocked_close_silent();
	test_failed_send_silent();
	test_sort_parked();

	session.windows[WINDOWC_INVENTORY] = session.windows[WINDOWC_CHEST] = NULL;
	windowc_destroy(&wc);
	windowc_destroy(&player);
	puts("PASS chest Back cue fires only on a successful return");
	return 0;
}
