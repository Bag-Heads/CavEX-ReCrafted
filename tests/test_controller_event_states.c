/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The DISTINCT debounced controller states the approved package requires
 * (docs/ui_design/approved/specs/menus.md, "Controller events", and the
 * menus.html event selector): detected, connected, disconnected, reconnected,
 * attachment changed, attachment missing, low battery, pointer lost/regained,
 * plus a join lifecycle that is modelled but never admits anybody. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/input_inventory.h"

static ptime_t at(unsigned ms) {
	return time_add_ms((ptime_t) {0}, ms);
}

static struct input_inventory inv;
static struct input_owner owners[INPUT_LOCAL_PLAYERS];

static const struct input_device_status* stat(struct input_device d) {
	return input_inventory_device(&inv, d);
}

/* Hold one identity across the full debounce window, exactly as the real poll
 * does frame after frame. */
static void hold(struct input_device d, struct input_identity id, unsigned from,
				 unsigned to) {
	for(unsigned ms = from; ms <= to; ms++)
		input_inventory_poll(&inv, d, id, true, true, true, at(ms), owners,
							 INPUT_LOCAL_PLAYERS);
}

static unsigned live(unsigned player, unsigned ms) {
	unsigned count = 0;
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++)
		count += input_notice_visible(&inv.notices[k], player, at(ms));
	return count;
}

static unsigned events_of(enum controller_event event, unsigned ms) {
	unsigned count = 0;
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++)
		if(inv.notices[k].active && inv.notices[k].event == event
		   && !time_reached(at(ms), inv.notices[k].expires))
			count++;
	return count;
}

static const struct input_device remote = {INPUT_DEVICE_WPAD, 0};
static const struct input_device gc = {INPUT_DEVICE_GAMECUBE, 2};
static const struct input_identity none = {false, INPUT_CAT_NONE};
static const struct input_identity bare = {true, INPUT_CAT_WIIMOTE};
static const struct input_identity nunchuk = {true, INPUT_CAT_NUNCHUK};
static const struct input_identity classic
	= {true, INPUT_CAT_CLASSIC_CONTROLLER};

static void test_identity_states(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));

	/* A bare Remote is hardware, not a playable family: the approved state is
	 * "required attachment missing", never "connected". */
	hold(remote, bare, 0, 300);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_ATTACHMENT_MISSING);
	assert(!stat(remote)->published_usable && !stat(remote)->ever_usable);

	/* Attaching the Nunchuk makes it usable, and it belongs to nobody yet:
	 * hardware identity, not player assignment. */
	hold(remote, nunchuk, 301, 600);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_DETECTED);
	assert(stat(remote)->published_usable && stat(remote)->ever_usable);
	assert(!stat(remote)->published_assigned);
	/* Detection offers a join; it does not perform one. */
	assert(inv.join.state == INPUT_JOIN_OFFERED);

	/* Now the player claims it. The hardware did not change, so this edge is
	 * the ASSIGNMENT -- a separate state with its own card. */
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	hold(remote, nunchuk, 601, 620);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_CONNECTED);
	assert(stat(remote)->published_assigned && stat(remote)->published_player == 0);

	/* Swapping the Nunchuk for a Classic on the SAME Remote is an attachment
	 * change, never a new player. */
	hold(remote, classic, 700, 1000);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_ATTACHMENT_CHANGED);
	assert(stat(remote)->published_assigned);
	assert(input_device_equal(owners[0].device, remote));

	/* Losing it, then getting it back, is disconnected -> reconnected: the
	 * return is deliberately NOT a fresh "connected". */
	hold(remote, none, 1100, 1400);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_DISCONNECTED);
	hold(remote, classic, 1500, 1800);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_RECONNECTED);
	/* Pulling the attachment while assigned is the missing-attachment state,
	 * and coming back from it is a reconnect, not a first connection. */
	hold(remote, bare, 1900, 2200);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_ATTACHMENT_MISSING);
	hold(remote, nunchuk, 2300, 2600);
	assert(stat(remote)->last_event == CONTROLLER_EVENT_RECONNECTED);
	/* Exactly ONE "connected" was ever raised for this device -- the player
	 * assignment at 601. Neither return re-classified as a first connection. */
	assert(events_of(CONTROLLER_EVENT_CONNECTED, 2600) == 1);
	assert(events_of(CONTROLLER_EVENT_RECONNECTED, 2600) == 2);
	puts("PASS controller states: detected/connected/attachment-changed/"
		 "disconnected/reconnected/attachment-missing are distinct edges");
}

/* Drive the pointer on the fake clock; count distinct POINTER_LOST publishes. */
static unsigned ptr_published;
static ptime_t ptr_max_exp;
static void ptr_step(bool valid, unsigned ms) {
	input_inventory_pointer(&inv, remote, true, valid, at(ms), owners,
							INPUT_LOCAL_PLAYERS);
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++) {
		const struct input_connection_notice* n = &inv.notices[k];
		if(n->active && n->event == CONTROLLER_EVENT_POINTER_LOST
		   && (ptr_published == 0
			   || (time_reached(n->expires, ptr_max_exp)
				   && !time_reached(ptr_max_exp, n->expires)))) {
			ptr_max_exp = n->expires;
			ptr_published++;
		}
	}
}
static void ptr_reset(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	ptr_published = 0;
	ptr_max_exp = (ptime_t) {0};
	for(unsigned ms = 0; ms <= 2 * INPUT_POINTER_STABLE_MS; ms += 25)
		ptr_step(true, 100 + ms);
}

static void test_pointer_notice_throttle(void) {
	/* Toggling every 900 ms for 15 s: at most 2 notices. */
	ptr_reset();
	unsigned t0 = 5000;
	for(unsigned ms = 0; ms < 15000; ms += 25)
		ptr_step(((ms / 900) & 1) == 0, t0 + ms);
	assert(ptr_published <= 2);
	/* One 5 s loss: exactly one notice. */
	ptr_reset();
	for(unsigned ms = 0; ms < 5000; ms += 25)
		ptr_step(false, t0 + ms);
	assert(ptr_published == 1);
	/* A loss shorter than the debounce: none. */
	ptr_reset();
	for(unsigned ms = 0; ms < INPUT_POINTER_LOST_STABLE_MS - 100; ms += 25)
		ptr_step(false, t0 + ms);
	for(unsigned ms = 0; ms < 2000; ms += 25)
		ptr_step(true, t0 + 5000 + ms);
	assert(ptr_published == 0);
	/* A regain retires the live card. */
	ptr_reset();
	for(unsigned ms = 0; ms < 2000; ms += 25)
		ptr_step(false, t0 + ms);
	assert(ptr_published == 1 && events_of(CONTROLLER_EVENT_POINTER_LOST, t0 + 2000) == 1);
	for(unsigned ms = 0; ms <= INPUT_POINTER_STABLE_MS + 50; ms += 25)
		ptr_step(true, t0 + 2000 + ms);
	assert(events_of(CONTROLLER_EVENT_POINTER_LOST, t0 + 2000 + INPUT_POINTER_STABLE_MS + 50) == 0);
	puts("PASS controller states: pointer-lost notice throttled, retired on regain");
}

static void test_assignment_is_its_own_state(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	/* A GameCube pad in port 3, seen before anyone owns it. */
	struct input_identity pad = {true, INPUT_CAT_GAMECUBE};
	hold(gc, pad, 0, 300);
	assert(stat(gc)->last_event == CONTROLLER_EVENT_DETECTED);
	assert(events_of(CONTROLLER_EVENT_CONNECTED, 300) == 0);
	/* Player 2 claims it. */
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 1, gc));
	hold(gc, pad, 301, 320);
	assert(events_of(CONTROLLER_EVENT_CONNECTED, 320) == 1);
	assert(stat(gc)->published_player == 1);
	/* Holding the same assignment forever raises nothing more. */
	hold(gc, pad, 321, 800);
	assert(events_of(CONTROLLER_EVENT_CONNECTED, 800) == 1);
	/* Unbinding is bookkeeping, not a controller event: no new card of any
	 * kind, and in particular the device is not re-announced as "detected". */
	unsigned before = live(0, 900) + live(1, 900);
	unsigned detected = events_of(CONTROLLER_EVENT_DETECTED, 900);
	owners[1] = (struct input_owner) {0};
	hold(gc, pad, 900, 1300);
	assert(live(0, 1300) + live(1, 1300) <= before);
	assert(events_of(CONTROLLER_EVENT_DETECTED, 1300) == detected);
	assert(events_of(CONTROLLER_EVENT_CONNECTED, 1300) == 1);
	puts("PASS controller states: assignment is its own edge, an unchanged "
		 "assignment repeats nothing, unbinding raises nothing");
}

static void test_battery_and_pointer_states(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	hold(remote, nunchuk, 0, 300);

	/* A wired GameCube pad can never produce a battery event. */
	for(unsigned ms = 0; ms < 6000; ms += 100)
		input_inventory_battery(&inv, gc, false, 0, at(ms), owners,
								INPUT_LOCAL_PLAYERS);
	assert(events_of(CONTROLLER_EVENT_LOW_BATTERY, 6000) == 0);

	/* A healthy pack adopts silently; dropping below the threshold has to
	 * hold for the stable window before it becomes an episode. */
	for(unsigned ms = 300; ms <= 1000; ms += 50)
		input_inventory_battery(&inv, remote, true, 80, at(ms), owners,
								INPUT_LOCAL_PLAYERS);
	assert(!stat(remote)->battery_low);
	for(unsigned ms = 1050; ms < 1050 + INPUT_BATTERY_STABLE_MS; ms += 50)
		input_inventory_battery(&inv, remote, true, INPUT_BATTERY_LOW_PERCENT,
								at(ms), owners, INPUT_LOCAL_PLAYERS);
	assert(!stat(remote)->battery_low);
	input_inventory_battery(&inv, remote, true, INPUT_BATTERY_LOW_PERCENT,
							at(1050 + INPUT_BATTERY_STABLE_MS), owners,
							INPUT_LOCAL_PLAYERS);
	assert(stat(remote)->battery_low
		   && stat(remote)->last_event == CONTROLLER_EVENT_LOW_BATTERY);

	/* The pointer is adopted from its FIRST observation without an event: a
	 * pointer that was never aimed at the sensor bar has not been lost. */
	memset(&inv, 0, sizeof(inv));
	hold(remote, nunchuk, 0, 300);
	for(unsigned ms = 300; ms <= 300 + 4 * INPUT_POINTER_STABLE_MS; ms += 25)
		input_inventory_pointer(&inv, remote, true, false, at(ms), owners,
								INPUT_LOCAL_PLAYERS);
	assert(events_of(CONTROLLER_EVENT_POINTER_LOST,
					 300 + 4 * INPUT_POINTER_STABLE_MS)
		   == 0);
	/* Aim it, then lose it: THAT is the approved pointer-loss state. */
	unsigned base = 300 + 4 * INPUT_POINTER_STABLE_MS;
	for(unsigned ms = base; ms <= base + INPUT_POINTER_STABLE_MS; ms += 25)
		input_inventory_pointer(&inv, remote, true, true, at(ms), owners,
								INPUT_LOCAL_PLAYERS);
	assert(stat(remote)->pointer_valid);
	base += INPUT_POINTER_STABLE_MS + 25;
	for(unsigned ms = base; ms <= base + INPUT_POINTER_LOST_STABLE_MS; ms += 25)
		input_inventory_pointer(&inv, remote, true, false, at(ms), owners,
								INPUT_LOCAL_PLAYERS);
	unsigned lost_at = base + INPUT_POINTER_LOST_STABLE_MS;
	assert(!stat(remote)->pointer_valid
		   && stat(remote)->last_event == CONTROLLER_EVENT_POINTER_LOST);
	assert(events_of(CONTROLLER_EVENT_POINTER_LOST, lost_at) == 1);
	/* Regaining it RETIRES the card rather than raising a second one. */
	for(unsigned ms = lost_at; ms <= lost_at + INPUT_POINTER_STABLE_MS; ms += 25)
		input_inventory_pointer(&inv, remote, true, true, at(ms), owners,
								INPUT_LOCAL_PLAYERS);
	unsigned back_at = lost_at + INPUT_POINTER_STABLE_MS;
	assert(stat(remote)->last_event == CONTROLLER_EVENT_POINTER_REGAINED);
	assert(events_of(CONTROLLER_EVENT_POINTER_LOST, back_at) == 0);
	/* A Classic or an unassigned Remote has no pointer state at all. */
	input_inventory_pointer(&inv, remote, false, false, at(back_at + 1), owners,
							INPUT_LOCAL_PLAYERS);
	assert(!stat(remote)->pointer_known);
	puts("PASS controller states: battery hysteresis needs a stable window, "
		 "wired hardware never reports one, pointer loss needs a prior aim");
}

static void test_join_is_modelled_but_gated(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	hold(remote, nunchuk, 0, 300);
	assert(inv.join.state == INPUT_JOIN_OFFERED);
	/* A request for a device that was never offered is refused outright. */
	assert(!input_inventory_join_request(&inv, gc, at(310)));
	assert(input_inventory_join_request(&inv, remote, at(320)));
	assert(inv.join.state == INPUT_JOIN_JOINING);
	assert(events_of(CONTROLLER_EVENT_JOIN_OFFERED, 320) == 1);
	/* The split admission path is still gated, so the settle is a failure --
	 * and a failure admits NOTHING: no owner, no second player. */
	input_inventory_join_settle(&inv, false, at(400));
	assert(inv.join.state == INPUT_JOIN_FAILED);
	assert(events_of(CONTROLLER_EVENT_JOIN_FAILED, 400) == 1);
	assert(events_of(CONTROLLER_EVENT_JOIN_OFFERED, 400) == 0);
	for(unsigned p = 0; p < INPUT_LOCAL_PLAYERS; p++)
		assert(owners[p].device.kind == INPUT_DEVICE_NONE);
	/* Back from the failure returns to the standing offer, not to nothing. */
	input_inventory_join_cancel(&inv, at(500));
	assert(inv.join.state == INPUT_JOIN_OFFERED);
	assert(events_of(CONTROLLER_EVENT_JOIN_CANCELLED, 500) == 1);
	assert(events_of(CONTROLLER_EVENT_JOIN_FAILED, 500) == 0);
	/* Unplugging the offered hardware takes the offer with it. */
	hold(remote, none, 600, 900);
	assert(inv.join.state == INPUT_JOIN_IDLE);
	for(unsigned p = 0; p < INPUT_LOCAL_PLAYERS; p++)
		assert(owners[p].device.kind == INPUT_DEVICE_NONE);
	puts("PASS controller states: join offer/failure/cancel are modelled and "
		 "bind nobody; the offer dies with its device");
}

/* critique-1028 P1 takeover (AUDIT-GC-GLYPHS.md section 2). The input layer
 * has no online/offline switch: input_p1_takeover (input.c, Wii) runs the
 * same rule in single player and in a remote session, so one model covers
 * both. Player 1 holds a Remote+Nunchuk; a GameCube pad that appeared (and
 * was offered "Join") presses A. */
static unsigned events_any(unsigned ms) {
	unsigned n = 0;
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++)
		if(inv.notices[k].active && !time_reached(at(ms), inv.notices[k].expires)
		   && (inv.notices[k].event == CONTROLLER_EVENT_CONNECTED
			   || inv.notices[k].event == CONTROLLER_EVENT_RECONNECTED
			   || inv.notices[k].event == CONTROLLER_EVENT_DETECTED
			   || inv.notices[k].event == CONTROLLER_EVENT_JOIN_OFFERED))
			n++;
	return n;
}

static void test_p1_takeover(void) {
	memset(&inv, 0, sizeof inv);
	memset(owners, 0, sizeof owners);
	const struct input_device wii = {INPUT_DEVICE_WPAD, 0};
	const struct input_device gc = {INPUT_DEVICE_GAMECUBE, 0};
	const struct input_identity nunchuk = {true, INPUT_CAT_NUNCHUK};
	const struct input_identity pad = {true, INPUT_CAT_GAMECUBE};
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, wii));
	owners[0].frame.connected = true;
	owners[0].frame.category = INPUT_CAT_NUNCHUK;
	hold(wii, nunchuk, 0, 400);
	hold(gc, pad, 0, 400); /* unassigned: DETECTED + Join offer */
	assert(stat(gc)->armed && inv.join.state == INPUT_JOIN_OFFERED);

	/* The gate. */
	assert(input_p1_takeover_eligible(owners, INPUT_LOCAL_PLAYERS, false, gc,
		true, INPUT_CAT_GAMECUBE, true));
	assert(!input_p1_takeover_eligible(owners, INPUT_LOCAL_PLAYERS, true, gc,
		true, INPUT_CAT_GAMECUBE, true) && "split offered: A means Join P2");
	assert(!input_p1_takeover_eligible(owners, INPUT_LOCAL_PLAYERS, false, gc,
		false, INPUT_CAT_GAMECUBE, true) && "never armed: a held A");
	assert(!input_p1_takeover_eligible(owners, INPUT_LOCAL_PLAYERS, false, gc,
		true, INPUT_CAT_GAMECUBE, false) && "no Confirm press");
	assert(!input_p1_takeover_eligible(owners, INPUT_LOCAL_PLAYERS, false, wii,
		true, INPUT_CAT_NUNCHUK, true) && "P1's own pad");
	assert(!input_p1_takeover_eligible(owners, INPUT_LOCAL_PLAYERS, false,
		(struct input_device) {INPUT_DEVICE_WPAD, 2}, true, INPUT_CAT_WIIMOTE,
		true) && "a bare Remote is never a family");
	{
		struct input_owner split[INPUT_LOCAL_PLAYERS];
		memcpy(split, owners, sizeof split);
		assert(input_owner_bind(split, INPUT_LOCAL_PLAYERS, 1,
			(struct input_device) {INPUT_DEVICE_WPAD, 1}));
		assert(!input_p1_takeover_eligible(split, INPUT_LOCAL_PLAYERS, false,
			gc, true, INPUT_CAT_GAMECUBE, true) && "P2 bound: never steal P1");
	}
	{
		struct input_owner none[INPUT_LOCAL_PLAYERS] = {0};
		assert(!input_p1_takeover_eligible(none, INPUT_LOCAL_PLAYERS, false,
			gc, true, INPUT_CAT_GAMECUBE, true) && "unbound P1 = the claim");
	}

	/* The takeover itself, as input_p1_takeover performs it. */
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, gc));
	owners[0].frame.connected = true;
	owners[0].frame.category = INPUT_CAT_GAMECUBE;
	input_inventory_takeover(&inv, wii, gc, 0, at(401));
	assert(!stat(gc)->armed && !stat(wii)->armed);
	assert(inv.join.state == INPUT_JOIN_IDLE);

	/* No card for either device afterwards: not "Player 1 connected", not
	 * "reconnected", and the Join offer for the taking pad is gone. */
	hold(wii, nunchuk, 402, 1200);
	hold(gc, pad, 402, 1200);
	/* (Cards from before the takeover expire by 3250 ms; anything the
	 * takeover raised would still be live at 3300.) */
	assert(events_any(3300) == 0);

	/* Prompts follow, nothing sticks: the confirming A (still down) is
	 * blocked until released; then a fresh press is a press. The old
	 * Remote's held buttons belong to no owner any more. */
	struct input_player_frame s = {.connected = true,
		.category = INPUT_CAT_GAMECUBE};
	for(int b = 0; b < IB_COUNT; b++) s.glyphs[b] = -1;
	s.down[IB_GUI_CLICK] = s.down[IB_ACTION1] = true;
	s.look_x = 0.9F; /* a deflected stick at the takeover frame */
	input_owner_latch(&owners[0], &s);
	assert(owners[0].frame.look_x == 0 && "look blocked until neutral");
	assert(owners[0].frame.category == INPUT_CAT_GAMECUBE);
	assert(!owners[0].frame.down[IB_GUI_CLICK]
		   && !owners[0].frame.pressed[IB_GUI_CLICK]
		   && !owners[0].frame.down[IB_ACTION1] && "takeover A never leaks");
	input_owner_latch(&owners[0], &s);
	assert(owners[0].frame.look_x == 0 && "still blocked while deflected");
	s.down[IB_GUI_CLICK] = s.down[IB_ACTION1] = false;
	s.look_x = 0;
	input_owner_latch(&owners[0], &s);
	s.down[IB_GUI_CLICK] = true;
	input_owner_latch(&owners[0], &s);
	assert(owners[0].frame.pressed[IB_GUI_CLICK] && "a fresh press works");

	/* The old Remote may take P1 back the same way -- it was disarmed by the
	 * takeover and is eligible again only once seen neutral (hold above). */
	assert(stat(wii)->armed);
	assert(input_p1_takeover_eligible(owners, INPUT_LOCAL_PLAYERS, false,
		wii, stat(wii)->armed, INPUT_CAT_NUNCHUK, true));
	puts("PASS P1 takeover: gated (split/P2/held/bare Remote), silent, "
		 "prompts follow, no stuck input");
}

int main(void) {
	test_identity_states();
	test_assignment_is_its_own_state();
	test_battery_and_pointer_states();
	test_pointer_notice_throttle();
	test_join_is_modelled_but_gated();
	test_p1_takeover();
	return 0;
}
