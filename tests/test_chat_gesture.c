/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The chat OPENER gesture, as a pure state machine (source/platform/
 * input_tap_gesture.h -- no libogc, no config, no clock of its own), so the
 * rules the maintainer's 2026-09-19 console report and Astra's gate-2 D2
 * ruling fixed are checkable on PC:
 *
 *   - TAP flavour (Wii Remote button 2, code 7): armed by a FRESH press,
 *     opens on the RELEASE, and only while the hold stayed inside the tap
 *     window -- a longer hold is the pointer-look recenter and opens nothing.
 *   - RELEASE flavour (Classic Plus, code 212): armed by a fresh press, opens
 *     on the release, CANCELLED if the chord partner (Minus) is held at any
 *     moment between the two, in either press order and either release order.
 *   - after a cancel, an expiry or a fired gesture, both buttons must be seen
 *     neutral before it can arm again.
 *   - a poll GAP (screen change), an owner change and a disconnect all clear
 *     it, and a button still held across one of those cannot fire on release.
 *
 * The wiring of this machine to the real Wii poll, the shipped bindings and
 * the D-pad legacy path lives in tests/test_input_wii_adapter.c, which runs
 * the production PLATFORM_WII input.c.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/input_tap_gesture.h"

/* ---- the bare machine --------------------------------------------------- */

#define TAP INPUT_CHAT_TAP_MS

static void test_tap_flavour(void) {
	struct input_tap_gesture g = {0};
	uint32_t t = 1000;
	/* A zeroed gesture is BLOCKED: whatever is held when it appears, it takes
	 * a neutral frame before anything can arm. */
	assert(!input_tap_gesture_step(&g, true, false, t, TAP));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, TAP));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, TAP)); /* arms */
	assert(!input_tap_gesture_step(&g, true, false, t += 10, TAP)); /* held */
	assert(input_tap_gesture_step(&g, false, false, t += 10, TAP)); /* opens */
	/* Exactly once: the same released level does not keep firing. */
	assert(!input_tap_gesture_step(&g, false, false, t += 10, TAP));

	/* THE BOUNDARY, pinned at the three milliseconds Astra ruling 2 D1 names:
	 * 499 opens, 500 opens (the rule is <= 500), 501 does not. 500 ms is a
	 * provisional usability choice, so a drift has to fail here by name. */
	assert(TAP == 500u && "the ruled tap window is 500 ms");
	{
		static const uint32_t held[] = {499u, 500u, 501u};
		static const bool opens[] = {true, true, false};
		for(unsigned k = 0; k < 3; k++) {
			assert(!input_tap_gesture_step(&g, true, false, t, TAP));
			assert(input_tap_gesture_step(&g, false, false, t + held[k], TAP)
				   == opens[k]);
			t += held[k] + 10;
			/* An expired hold left the machine blocked: give it the neutral
			 * frame the next arm needs. */
			assert(!input_tap_gesture_step(&g, false, false, t, TAP));
		}
	}

	/* An expired hold stays dead for the whole rest of the hold, and the
	 * release that ends it re-arms the machine for the next tap. */
	assert(!input_tap_gesture_step(&g, true, false, t, TAP));
	for(unsigned k = 0; k < 5; k++)
		assert(!input_tap_gesture_step(&g, true, false, t += TAP, TAP));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, TAP));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, TAP));
	assert(input_tap_gesture_step(&g, false, false, t += 10, TAP));

	/* No cap (0) means no expiry at all -- the release flavour's rule. */
	assert(!input_tap_gesture_step(&g, true, false, t += 10, 0));
	assert(!input_tap_gesture_step(&g, true, false, t += 100000, 0));
	assert(input_tap_gesture_step(&g, false, false, t += 10, 0));
	puts("PASS chat gesture: a tap opens on release, a hold past the window "
		 "opens nothing and re-arms, an uncapped gesture never expires");
}

static void test_partner_cancel(void) {
	const uint32_t cap = 0; /* the Classic Plus flavour */
	/* Every ordering of one chord, checked against the same machine. */
	struct input_tap_gesture g = {0};
	uint32_t t = 0;
	assert(!input_tap_gesture_step(&g, false, false, t, cap)); /* neutral */

	/* opener first, partner joins, partner lifts first */
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, cap));
	/* ... and now, both seen neutral, it arms again */
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(input_tap_gesture_step(&g, false, false, t += 10, cap));

	/* opener first, opener lifts first */
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, cap));

	/* partner first (staggered): the opener press lands into a held partner */
	assert(!input_tap_gesture_step(&g, false, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, cap));

	/* partner first, opener lifts first */
	assert(!input_tap_gesture_step(&g, false, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, cap));

	/* the partner tapped in the MIDDLE of a long opener hold still cancels */
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, cap));

	/* a partner held ALONE never arms anything, and never blocks a later
	 * clean gesture once it is released */
	assert(!input_tap_gesture_step(&g, false, true, t += 10, cap));
	assert(!input_tap_gesture_step(&g, false, false, t += 10, cap));
	assert(!input_tap_gesture_step(&g, true, false, t += 10, cap));
	assert(input_tap_gesture_step(&g, false, false, t += 10, cap));
	puts("PASS chat gesture: a partner overlap cancels the opener in both "
		 "press orders and both release orders, and neutral re-arms");
}

/* ---- the scoped wrapper: poll serial, owner identity, connection --------- */

#define WPAD_KIND 2
#define OPENER 7

static bool tick(struct input_tap_scope* s, unsigned long serial, bool opener,
				 bool partner, uint32_t now_ms) {
	return input_tap_gesture_tick(s, serial, WPAD_KIND, 0, OPENER, TAP, 0u, true,
								   opener, partner, now_ms);
}

static void test_scope_frame_latch(void) {
	struct input_tap_scope s = {0};
	uint32_t t = 500;
	assert(!tick(&s, 1, false, false, t));
	assert(!tick(&s, 2, true, false, t += 10));
	/* Idempotent WITHIN one poll: several consumers, one answer, and the
	 * repeated call may not advance the machine. */
	assert(!tick(&s, 2, false, false, t + 5));
	assert(!tick(&s, 2, false, false, t + 5));
	assert(tick(&s, 3, false, false, t += 10));
	assert(tick(&s, 3, false, false, t));
	assert(!tick(&s, 4, false, false, t += 10));
	puts("PASS chat gesture: the scoped answer is latched per poll and a "
		 "repeat read never advances the machine");
}

static void test_scope_gap_owner_disconnect(void) {
	struct input_tap_scope s = {0};
	uint32_t t = 0;
	assert(!tick(&s, 1, false, false, t));
	assert(!tick(&s, 2, true, false, t += 10)); /* armed */
	/* A skipped poll is a screen change: the gesture is gone, and the button
	 * still held across it has to be released and pressed again. */
	assert(!tick(&s, 4, true, false, t += 10)); /* still held, now blocked */
	assert(!tick(&s, 5, false, false, t += 10)
		   && "a gesture survived a poll gap");
	assert(!tick(&s, 6, true, false, t += 10));
	assert(tick(&s, 7, false, false, t += 10));

	/* An owner change clears it the same way. */
	assert(!tick(&s, 8, true, false, t += 10)); /* armed */
	assert(!input_tap_gesture_tick(&s, 9, WPAD_KIND, 1, OPENER, TAP, 0u, true,
									true, false, t += 10));
	assert(!input_tap_gesture_tick(&s, 10, WPAD_KIND, 1, OPENER, TAP, 0u, true,
									true, false, t += 10));
	assert(!input_tap_gesture_tick(&s, 11, WPAD_KIND, 1, OPENER, TAP, 0u, true,
									false, false, t += 10));

	/* So does a disconnect -- and a disconnected owner never opens anything. */
	assert(!tick(&s, 12, false, false, t += 10)); /* the first pad, neutral */
	assert(!tick(&s, 13, true, false, t += 10));  /* armed */
	assert(!input_tap_gesture_tick(&s, 14, WPAD_KIND, 0, OPENER, TAP, 0u, false,
									true, false, t += 10));
	assert(!input_tap_gesture_tick(&s, 15, WPAD_KIND, 0, OPENER, TAP, 0u, false,
									false, false, t += 10));
	assert(!tick(&s, 16, true, false, t += 10)); /* reconnected, still held */
	assert(!tick(&s, 17, false, false, t += 10)
		   && "a gesture survived a disconnect");
	assert(!tick(&s, 18, true, false, t += 10));
	assert(tick(&s, 19, false, false, t += 10));

	/* A binding change (a different opener code) is a different gesture. */
	assert(!tick(&s, 20, true, false, t += 10));
	assert(!input_tap_gesture_tick(&s, 21, WPAD_KIND, 0, 212, 0, 0u, true, false,
									false, t += 10));
	puts("PASS chat gesture: a poll gap, an owner change, a disconnect and a "
		 "re-binding each clear the gesture instead of firing it");
}

int main(void) {
	test_tap_flavour();
	test_partner_cancel();
	test_scope_frame_latch();
	test_scope_gap_owner_disconnect();
	return 0;
}
