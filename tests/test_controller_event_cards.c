/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The approved card per event: exact text, picture family, upper-left
 * position inside the AFFECTED view, and the 3,000 ms duration -- raised
 * exactly once per edge. Wording authority: the approved menus.html event
 * selector (notice() and its per-event strings) plus specs/menus.md. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/input_inventory.h"

static ptime_t at(unsigned ms) {
	return time_add_ms((ptime_t) {0}, ms);
}

static struct input_inventory inv;
static struct input_owner owners[INPUT_LOCAL_PLAYERS];

static void hold(struct input_device d, struct input_identity id, unsigned from,
				 unsigned to) {
	for(unsigned ms = from; ms <= to; ms++)
		input_inventory_poll(&inv, d, id, true, true, true, at(ms), owners,
							 INPUT_LOCAL_PLAYERS);
}

/* Every live card for one viewport, in ring order. */
static unsigned cards(unsigned player, bool split, unsigned ms,
					  struct controller_card* out, unsigned capacity) {
	unsigned count = 0;
	for(size_t k = 0; k < INPUT_NOTICE_COUNT && count < capacity; k++)
		if(controller_card_build(&inv.notices[k], player, split, at(ms), 0, 0,
								 640, 480, &out[count]))
			count++;
	return count;
}

static const struct input_device remote = {INPUT_DEVICE_WPAD, 1};
static const struct input_device gc = {INPUT_DEVICE_GAMECUBE, 0};
static const struct input_identity none = {false, INPUT_CAT_NONE};
static const struct input_identity bare = {true, INPUT_CAT_WIIMOTE};
static const struct input_identity nunchuk = {true, INPUT_CAT_NUNCHUK};
static const struct input_identity classic
	= {true, INPUT_CAT_CLASSIC_CONTROLLER};

static void test_detected_and_connected(void) {
	struct controller_card card[4];
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));

	hold(remote, nunchuk, 0, 300);
	assert(cards(0, false, 300, card, 4) == 1);
	assert(!strcmp(card[0].text, "New controller"));
	assert(!strcmp(card[0].action_confirm, "Join P2"));
	assert(!card[0].action_cancel[0]);
	assert(card[0].picture == CONTROLLER_PICTURE_NUNCHUK);
	/* Upper-left of the supplied view, and the approved 3s life. */
	assert(card[0].x == 8 && card[0].y == 8);
	assert(card[0].duration_ms == INPUT_NOTICE_MS);
	assert(card[0].event == CONTROLLER_EVENT_DETECTED);
	/* Exactly once per edge: the same notice, polled for three more seconds,
	 * never becomes a second card, and it goes away on its own deadline
	 * (published at the end of the 250 ms identity debounce). */
	assert(cards(0, false, INPUT_IDENTITY_STABLE_MS + INPUT_NOTICE_MS - 1, card,
				 4)
		   == 1);
	assert(cards(0, false, INPUT_IDENTITY_STABLE_MS + INPUT_NOTICE_MS, card, 4)
		   == 0);

	/* Split-screen has no free slot to offer, so the same edge says so and
	 * offers no action. */
	memset(&inv, 0, sizeof(inv));
	hold(remote, nunchuk, 0, 300);
	assert(cards(0, true, 300, card, 4) == 1);
	assert(!strcmp(card[0].text, "No free player slot"));
	assert(!card[0].action_confirm[0]);

	/* Assignment is the other card, in the affected player's own view. */
	memset(&inv, 0, sizeof(inv));
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 1, remote));
	hold(remote, classic, 0, 300);
	assert(cards(0, false, 300, card, 4) == 0); /* not player 1's viewport */
	assert(cards(1, false, 300, card, 4) == 1);
	assert(!strcmp(card[0].text, "Player 2 Connected"));
	assert(card[0].picture == CONTROLLER_PICTURE_CLASSIC);
	/* And it lands in the physical rectangle its viewport owner supplies. */
	struct controller_card lower;
	assert(controller_card_build(&inv.notices[0], 1, false, at(300), 0, 240,
								 640, 240, &lower));
	assert(lower.x == 8 && lower.y == 248);
	puts("PASS controller cards: approved detected/split/connected wording, "
		 "family picture, viewport-relative origin, 3,000 ms, once per edge");
}

static void test_loss_family_cards(void) {
	struct controller_card card[8];
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));

	hold(remote, nunchuk, 0, 300);
	hold(remote, none, 400, 700);
	assert(cards(0, false, 700, card, 8) == 2);
	assert(!strcmp(card[1].text, "Player 1 disconnected"));
	assert(card[1].event == CONTROLLER_EVENT_DISCONNECTED);
	/* The family survives the loss: the card still shows what went away. */
	assert(card[1].picture == CONTROLLER_PICTURE_NUNCHUK);

	memset(&inv, 0, sizeof(inv));
	hold(remote, nunchuk, 0, 300);
	hold(remote, none, 400, 700);
	hold(remote, nunchuk, 800, 1100);
	assert(cards(0, false, 1100, card, 8) == 3);
	assert(!strcmp(card[2].text, "Player 1 reconnected"));

	memset(&inv, 0, sizeof(inv));
	hold(remote, nunchuk, 0, 300);
	hold(remote, classic, 400, 700);
	assert(cards(0, false, 700, card, 8) == 2);
	assert(!strcmp(card[1].text, "Controller changed"));

	memset(&inv, 0, sizeof(inv));
	hold(remote, bare, 0, 300);
	assert(cards(0, false, 300, card, 8) == 1);
	assert(!strcmp(card[0].text, "Attach Nunchuk or Classic"));
	/* A bare Remote is drawn as a Remote, never as a family it does not have. */
	assert(card[0].picture == CONTROLLER_PICTURE_REMOTE);
	puts("PASS controller cards: approved disconnected/reconnected/changed/"
		 "attachment-missing wording and pictures");
}

static void test_battery_pointer_join_cards(void) {
	struct controller_card card[8];
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	hold(remote, nunchuk, 0, 300);

	input_inventory_battery(&inv, remote, true, 5, at(400), owners,
							INPUT_LOCAL_PLAYERS);
	input_inventory_battery(&inv, remote, true, 5,
							at(400 + INPUT_BATTERY_STABLE_MS), owners,
							INPUT_LOCAL_PLAYERS);
	unsigned now = 400 + INPUT_BATTERY_STABLE_MS;
	unsigned count = cards(0, false, now, card, 8);
	assert(count >= 1);
	assert(!strcmp(card[count - 1].text, "Player 1 - Battery low"));
	/* The battery glyph replaces the controller picture on this card. */
	assert(card[count - 1].picture == CONTROLLER_PICTURE_BATTERY);

	memset(&inv, 0, sizeof(inv));
	hold(remote, nunchuk, 0, 300);
	input_inventory_pointer(&inv, remote, true, true, at(400), owners,
							INPUT_LOCAL_PLAYERS);
	input_inventory_pointer(&inv, remote, true, false, at(500), owners,
							INPUT_LOCAL_PLAYERS);
	input_inventory_pointer(&inv, remote, true, false,
							at(500 + INPUT_POINTER_LOST_STABLE_MS), owners,
							INPUT_LOCAL_PLAYERS);
	now = 500 + INPUT_POINTER_LOST_STABLE_MS;
	count = cards(0, false, now, card, 8);
	assert(count >= 1);
	/* Stick navigation stays available, and the card says so. No picture:
	 * the approved pointer notice carries none. */
	assert(!strcmp(card[count - 1].text, "Pointer out of view - Use stick"));
	assert(card[count - 1].picture == CONTROLLER_PICTURE_NONE);

	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	hold(gc, (struct input_identity) {true, INPUT_CAT_GAMECUBE}, 0, 300);
	assert(input_inventory_join_request(&inv, gc, at(400)));
	count = cards(0, false, 400, card, 8);
	assert(!strcmp(card[count - 1].text, "Joining Player 2..."));
	assert(!strcmp(card[count - 1].action_cancel, "Cancel"));
	assert(!card[count - 1].action_confirm[0]);
	assert(card[count - 1].picture == CONTROLLER_PICTURE_GAMECUBE);
	input_inventory_join_settle(&inv, false, at(500));
	count = cards(0, false, 500, card, 8);
	assert(!strcmp(card[count - 1].text, "Could not join"));
	assert(!strcmp(card[count - 1].action_confirm, "Retry"));
	assert(!strcmp(card[count - 1].action_cancel, "Back"));
	input_inventory_join_cancel(&inv, at(600));
	count = cards(0, false, 600, card, 8);
	assert(!strcmp(card[count - 1].text, "New controller"));
	assert(!strcmp(card[count - 1].action_confirm, "Join P2"));
	puts("PASS controller cards: battery glyph card, silent-audio pointer "
		 "card, join/failure/cancel actions");
}

static void test_notice_text_matches_the_card(void) {
	struct controller_card card;
	char text[96];
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 1, remote));
	hold(remote, classic, 0, 300);
	assert(controller_card_build(&inv.notices[0], 1, false, at(300), 0, 0, 640,
								 480, &card));
	input_notice_text(&inv.notices[0], text, sizeof(text));
	/* One wording authority: the renderer's short text IS the card's text. */
	assert(!strcmp(text, card.text));
	assert(!strcmp(text, "Player 2 Connected"));
	/* Families keep the approved hardware names for the recovery detail line. */
	assert(!strcmp(controller_family_name(INPUT_CAT_NUNCHUK),
				   "Wii Remote + Nunchuk"));
	assert(!strcmp(controller_family_name(INPUT_CAT_CLASSIC_CONTROLLER),
				   "Classic Controller"));
	assert(!strcmp(controller_family_name(INPUT_CAT_GAMECUBE),
				   "GameCube controller"));
	assert(!strcmp(controller_family_name(INPUT_CAT_WIIMOTE), "Wii Remote"));
	/* A GameCube pad is never drawn as a Wii family, whatever it reports. */
	assert(controller_picture_of(gc, INPUT_CAT_NUNCHUK)
		   == CONTROLLER_PICTURE_GAMECUBE);
	puts("PASS controller cards: one wording authority shared with the notice "
		 "renderer; approved family names and pictures");
}

/* p2-toast: P1 = GameCube pad, pad unplugged, a Wii Remote + Nunchuk appears.
 * Its A takes over P1 (input_p1_takeover), so the card must not say P2. A real
 * second player (split join armed) still sees P2. */
static void test_takeover_card_names_p1(void) {
	struct controller_card card;
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, gc));
	owners[0].frame.connected = true;
	hold(gc, (struct input_identity) {true, INPUT_CAT_GAMECUBE}, 0, 300);
	hold(gc, none, 301, 600); /* unplugged; P1 stays bound to it */
	hold(remote, nunchuk, 601, 900);
	struct input_connection_notice* n = NULL;
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++)
		if(inv.notices[k].active
		   && inv.notices[k].event == CONTROLLER_EVENT_DETECTED)
			n = &inv.notices[k];
	assert(n);
	assert(controller_card_build(n, 0, false, at(900), 0, 0, 640, 480, &card));
	controller_card_name_join_target(&card, &inv);
	assert(!strstr(card.action_confirm, "P2"));
	assert(!strcmp(card.action_confirm, "Play as P1"));
	inv.split_join_armed = true;
	assert(controller_card_build(n, 0, false, at(900), 0, 0, 640, 480, &card));
	controller_card_name_join_target(&card, &inv);
	assert(!strcmp(card.action_confirm, "Join P2"));
	/* ui1031/p1-takeover: in game the split join IS armed, but P1's pad is
	 * published gone, so A takes P1 -- the card must say so. */
	assert(input_inventory_vacant_seat(&inv, owners, INPUT_LOCAL_PLAYERS) == 1);
	inv.vacant_seat = 1;
	assert(controller_card_build(n, 0, false, at(900), 0, 0, 640, 480, &card));
	controller_card_name_join_target(&card, &inv);
	assert(!strcmp(card.action_confirm, "Play as P1"));
	/* In split, P2's device gone: the split pane's "No free player slot" is
	 * false while that seat stands empty; A takes P2. */
	inv.vacant_seat = 2;
	assert(controller_card_build(n, 0, true, at(900), 0, 0, 640, 480, &card));
	controller_card_name_join_target(&card, &inv);
	assert(!strcmp(card.text, "New controller"));
	assert(!strcmp(card.action_confirm, "Play as P2"));
	inv.vacant_seat = 0;
	assert(controller_card_build(n, 0, true, at(900), 0, 0, 640, 480, &card));
	controller_card_name_join_target(&card, &inv);
	assert(!strcmp(card.text, "No free player slot") && !card.action_confirm[0]);
	puts("PASS ui1031: a vacant seat names the seat A takes (Play as P1/P2), "
		 "split armed or not");
}

int main(void) {
	test_takeover_card_names_p1();
	test_detected_and_connected();
	test_loss_family_cards();
	test_battery_pointer_join_cards();
	test_notice_text_matches_the_card();
	return 0;
}
