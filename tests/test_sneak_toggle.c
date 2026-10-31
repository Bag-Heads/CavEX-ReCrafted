/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The Wii Remote's SNEAK TOGGLE as a pure state machine
 * (source/platform/input_sneak_toggle.h -- no libogc, no config, no clock), so
 * the rules Astra ruling 3 D3 fixed are checkable on PC.
 *
 * WHY IT EXISTS. In the maintainer's CavEX layout (2026-09-19) the right thumb
 * lives on the D-pad and only B / Nunchuk Z / Nunchuk C can be pressed while
 * walking -- jump, break and place take all three. Sneak has to be HELD while
 * moving and there is no finger left for it, so on the Wii Remote it becomes a
 * toggle: a documented carve-out #2 deviation from Beta's held Shift.
 *
 * THE RULES (Astra ruling 3 D3, verbatim in intent):
 *   - toggle once per FRESH press, per player;
 *   - CLEARED on death/respawn and on a successful vehicle or bed entry (this
 *     machine's two edges), and -- wired by input.c, not by this header -- on
 *     world/session leave or replacement, controller disconnect, owner or
 *     family change, and a scheme-mode change;
 *   - PRESERVED through inventory, chat and pause: opening a screen must not
 *     silently remove ledge protection in a continuing world;
 *   - gameplay toggle input is SUPPRESSED inside screens, and coming back needs
 *     a NEUTRAL REARM -- the very press that closes a menu (Remote 1 is both
 *     the menu Close and the sneak button) must not also toggle sneak.
 */

#include <assert.h>
#include <stdio.h>

#include "platform/input_sneak_toggle.h"

/* raw, gameplay, dead, vehicle -- in the machine's argument order. */
static bool step(struct input_sneak_toggle* t, bool raw, bool gameplay,
				 bool dead, bool vehicle) {
	return input_sneak_toggle_step(t, raw, gameplay, dead, vehicle);
}

static void test_fresh_press_toggles(void) {
	struct input_sneak_toggle t = {0};
	/* A zeroed machine is UNARMED: whatever is held when it appears has to be
	 * seen released first. That is what makes "held across a screen change or
	 * a reconnect" safe by construction. */
	assert(!step(&t, true, true, false, false));
	assert(!step(&t, true, true, false, false));
	assert(!step(&t, false, true, false, false)); /* arms */
	assert(step(&t, true, true, false, false) && "a fresh press did not engage");
	/* ONE toggle per press: the hold does not flicker it back off. */
	for(unsigned k = 0; k < 5; k++)
		assert(step(&t, true, true, false, false) && "a held press re-toggled");
	assert(step(&t, false, true, false, false) && "the release cleared sneak");
	assert(step(&t, true, true, false, false) == false
		   && "the second press did not disengage");
	assert(!step(&t, false, true, false, false));
	puts("PASS sneak toggle: one toggle per fresh press, a hold never "
		 "re-toggles, and a zeroed machine needs a neutral frame first");
}

static void test_clear_events(void) {
	/* DEATH and RESPAWN both clear -- a corpse cannot be sneaking, and the
	 * new body must not inherit a crouch nobody asked for. */
	struct input_sneak_toggle t = {0};
	assert(!step(&t, false, true, false, false));
	assert(step(&t, true, true, false, false));
	assert(!step(&t, true, true, true, false) && "death did not clear sneak");
	assert(!step(&t, false, true, true, false));
	/* engage again while dead is still true, then RESPAWN clears it too. */
	assert(step(&t, true, true, true, false));
	assert(!step(&t, true, true, false, false) && "respawn did not clear sneak");

	/* VEHICLE / BED ENTRY clears on the rising edge only: the state is gone
	 * the moment the cart, boat or bed takes the player. */
	struct input_sneak_toggle v = {0};
	assert(!step(&v, false, true, false, false));
	assert(step(&v, true, true, false, false));
	assert(!step(&v, true, true, false, true) && "vehicle entry did not clear");
	/* while STILL riding, a fresh press may engage again (the clear is the
	 * entry, not the whole ride) ... */
	assert(!step(&v, false, true, false, true));
	assert(step(&v, true, true, false, true));
	/* ... and merely staying aboard does not keep clearing it. */
	assert(step(&v, true, true, false, true));
	assert(step(&v, false, true, false, true));
	/* Leaving the vehicle is not an entry, so it clears nothing; the NEXT
	 * entry does. */
	assert(step(&v, false, true, false, false));
	assert(!step(&v, false, true, false, true) && "the next entry did not clear");
	puts("PASS sneak toggle: death, respawn and each vehicle/bed ENTRY clear "
		 "it; staying aboard and dismounting do not");
}

static void test_screens_preserve_and_rearm(void) {
	struct input_sneak_toggle t = {0};
	assert(!step(&t, false, true, false, false));
	assert(step(&t, true, true, false, false));
	assert(step(&t, false, true, false, false));

	/* A screen owns the frame: the toggle input is SUPPRESSED but the state
	 * is PRESERVED -- opening the inventory, chat or pause must not silently
	 * remove ledge protection in a continuing world. */
	for(unsigned k = 0; k < 4; k++)
		assert(step(&t, false, false, false, false)
			   && "a screen cleared the sneak toggle");
	/* ... and a press INSIDE the screen toggles nothing. */
	for(unsigned k = 0; k < 3; k++)
		assert(step(&t, true, false, false, false)
			   && "a press inside a screen reached the sneak toggle");

	/* THE CLOSE-THEN-NO-TOGGLE CASE. Remote 1 is both the menu Close and the
	 * sneak button: the press that closes the menu is still DOWN on the frame
	 * gameplay resumes, and it must not toggle. Only a fresh press after a
	 * neutral frame may. */
	assert(step(&t, true, true, false, false)
		   && "the press that closed a menu toggled sneak");
	assert(step(&t, true, true, false, false));
	assert(step(&t, false, true, false, false)); /* neutral: re-arms */
	assert(!step(&t, true, true, false, false)
		   && "the gesture never re-armed after a screen");

	/* THE HARDER HALF of the same rule, and the reason the rearm is a state
	 * and not just an edge memory: a screen may CONSUME the button, so the
	 * machine sees the level go FALSE while the menu is up (input_player_block
	 * and the container screens' own rearm latch both do exactly that) and
	 * TRUE again on the first gameplay frame, with no physical release in
	 * between. That must not read as a fresh press either. */
	struct input_sneak_toggle c = {0};
	assert(!step(&c, false, true, false, false));
	assert(step(&c, true, true, false, false));
	assert(step(&c, false, false, false, false)); /* screen: consumed */
	assert(step(&c, false, false, false, false));
	assert(step(&c, true, true, false, false)
		   && "a consumed menu press toggled sneak on the way back");
	assert(step(&c, true, true, false, false));
	assert(step(&c, false, true, false, false));
	assert(!step(&c, true, true, false, false)
		   && "the machine never re-armed after a consumed press");
	puts("PASS sneak toggle: a screen preserves the state, suppresses its "
		 "input, and neither the press that closed the menu nor one the menu "
		 "consumed can toggle on the way back");
}

static void test_clear_inside_a_screen(void) {
	/* A clear event that lands while a screen owns the frame still clears:
	 * dying with the inventory open must not leave a crouch behind. */
	struct input_sneak_toggle t = {0};
	assert(!step(&t, false, true, false, false));
	assert(step(&t, true, true, false, false));
	assert(step(&t, false, false, false, false));
	assert(!step(&t, false, false, true, false)
		   && "death inside a screen did not clear sneak");
	puts("PASS sneak toggle: a clear event still clears while a screen owns "
		 "the frame");
}

int main(void) {
	test_fresh_press_toggles();
	test_clear_events();
	test_screens_preserve_and_rearm();
	test_clear_inside_a_screen();
	return 0;
}
