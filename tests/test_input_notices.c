/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/input_inventory.h"

static ptime_t clock_ms(unsigned ms) {
	return time_add_ms((ptime_t) {0}, ms);
}

static unsigned visible(const struct input_inventory* inventory,
						unsigned player, unsigned ms) {
	unsigned count = 0;
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++)
		count += input_notice_visible(&inventory->notices[k], player,
									  clock_ms(ms));
	return count;
}

int main(void) {
	struct input_inventory inventory = {0};
	struct input_owner owners[2] = {0};
	struct input_device remote = {INPUT_DEVICE_WPAD, 3};
	struct input_device gc = {INPUT_DEVICE_GAMECUBE, 1};
	assert(input_owner_bind(owners, 2, 1, remote));
	struct input_identity classic = {true, INPUT_CAT_CLASSIC_CONTROLLER};
	input_inventory_poll(&inventory, remote, classic, true, true, true,
						 clock_ms(0), owners, 2);
	assert(!visible(&inventory, 1, 249));
	input_inventory_poll(&inventory, remote, classic, true, true, true,
						 clock_ms(250), owners, 2);
	assert(visible(&inventory, 1, 250) == 1
		   && visible(&inventory, 0, 250) == 0);
	char text[96];
	/* Approved wording (menus.html notice(): "Player N Connected"). Hardware
	 * identity and player assignment are different states, and this edge is
	 * the assignment. */
	input_notice_text(&inventory.notices[0], text, sizeof(text));
	assert(!strcmp(text, "Player 2 Connected"));
	assert(inventory.notices[0].event == CONTROLLER_EVENT_CONNECTED);
	int x = -1, y = -1;
	assert(input_notice_origin(&inventory.notices[0], 1, clock_ms(250), 0, 240,
							   640, 240, &x, &y));
	assert(x == 8 && y == 248);
	assert(!input_notice_origin(&inventory.notices[0], 0, clock_ms(250), 0, 0,
								640, 240, &x, &y));
	for(unsigned ms = 251; ms <= 3249; ms++)
		input_inventory_poll(&inventory, remote, classic, true, true, true,
							 clock_ms(ms), owners, 2);
	assert(visible(&inventory, 1, 3249) == 1);
	assert(visible(&inventory, 1, 3250) == 0);
	assert(!input_notice_origin(&inventory.notices[0], 1, clock_ms(3250), 0,
								240, 640, 240, &x, &y));
	puts("PASS notices: one transition, no repeated-poll restart, exact 3000ms "
		 "expiry, P2 viewport");

	struct input_identity pad = {true, INPUT_CAT_GAMECUBE};
	input_inventory_poll(&inventory, gc, pad, true, true, true, clock_ms(4000),
						 owners, 2);
	input_inventory_poll(&inventory, gc, pad, true, true, true, clock_ms(4250),
						 owners, 2);
	assert(visible(&inventory, 0, 4250) == 1);
	/* Unassigned hardware is the DETECTED state, whose approved card offers a
	 * join rather than claiming a player. */
	input_notice_text(&inventory.notices[0], text, sizeof(text));
	assert(!strcmp(text, "New controller"));
	assert(inventory.notices[0].event == CONTROLLER_EVENT_DETECTED);
	assert(input_notice_origin(&inventory.notices[0], 0, clock_ms(4250), 0, 0,
							   640, 480, &x, &y)
		   && x == 8 && y == 8);
	struct input_connection_notice notice = inventory.notices[0];
	notice.kind = remote.kind;
	notice.channel = remote.channel;
	notice.category = INPUT_CAT_NUNCHUK;
	/* The family reaches the card as its PICTURE, not as a second name. */
	struct controller_card card;
	assert(controller_card_build(&notice, 0, false, clock_ms(4250), 0, 0, 640,
								 480, &card));
	assert(card.picture == CONTROLLER_PICTURE_NUNCHUK
		   && !strcmp(card.text, "New controller")
		   && !strcmp(card.action_confirm, "Join P2"));
	notice.category = INPUT_CAT_WIIMOTE;
	notice.event = CONTROLLER_EVENT_ATTACHMENT_MISSING;
	input_notice_text(&notice, text, sizeof(text));
	assert(!strcmp(text, "Attach Nunchuk or Classic"));
	notice.connected = false;
	notice.previous_category = INPUT_CAT_CLASSIC_CONTROLLER;
	notice.assigned = true;
	notice.event = CONTROLLER_EVENT_DISCONNECTED;
	input_notice_text(&notice, text, sizeof(text));
	assert(!strcmp(text, "Player 1 disconnected"));
	/* The lost device's family still identifies the card. */
	assert(controller_card_build(&notice, 0, false, clock_ms(4250), 0, 0, 640,
								 480, &card));
	assert(card.picture == CONTROLLER_PICTURE_CLASSIC);
	puts("PASS notices: approved event wording and pictures, whole-screen "
		 "origin, no Classic Pro inference");

	/* All eight devices flapping at the minimum stable cadence cannot overflow
	 * the live set or replace an earlier event before its full three seconds.
	 */
	inventory = (struct input_inventory) {0};
	for(unsigned step = 0; step < 40; step++) {
		unsigned start = step * 251;
		for(unsigned k = 0; k < INPUT_DEVICE_COUNT; k++) {
			struct input_device d
				= {k < 4 ? INPUT_DEVICE_WPAD : INPUT_DEVICE_GAMECUBE, k % 4};
			struct input_identity id
				= {!(step % 2), k < 4 ? INPUT_CAT_NUNCHUK : INPUT_CAT_GAMECUBE};
			input_inventory_poll(&inventory, d, id, true, true, true,
								 clock_ms(start), NULL, 0);
			input_inventory_poll(&inventory, d, id, true, true, true,
								 clock_ms(start + 250), NULL, 0);
		}
		unsigned expected = (step < 12 ? step + 1 : 12) * 8;
		assert(visible(&inventory, 0, start + 250) == expected);
	}
	puts(
		"PASS notices: eight-device transition burst retains every live event");
	/* Assignment can churn faster than debounce. Never abort under asserts. */
	inventory = (struct input_inventory) {0};
	input_inventory_poll(&inventory, remote, classic, true, true, true,
		clock_ms(0), NULL, 0);
	input_inventory_poll(&inventory, remote, classic, true, true, true,
		clock_ms(250), NULL, 0);
	for(unsigned k = 0; k < INPUT_NOTICE_COUNT * 3; k++) {
		input_inventory_poll(&inventory, remote, classic, true, true, true,
			clock_ms(251 + k), NULL, 0);
		input_inventory_poll(&inventory, remote, classic, true, true, true,
			clock_ms(251 + k), owners, 2);
	}
	assert(inventory.notices_dropped == INPUT_NOTICE_COUNT * 2 + 1);
	assert(visible(&inventory, 1, 251 + INPUT_NOTICE_COUNT * 3) == INPUT_NOTICE_COUNT);
	puts("PASS notice churn: bounded storage, counted oldest eviction, no abort");
	return 0;
}
