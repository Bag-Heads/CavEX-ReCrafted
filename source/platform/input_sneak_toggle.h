/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The Wii Remote's SNEAK TOGGLE: pure, header-only, no platform headers, so
 * the rule can be unit-tested on PC (tests/test_sneak_toggle.c) while input.c
 * owns the wiring and the scoping.
 *
 * WHY IT EXISTS (maintainer decision 2026-09-19 "I agree, cavex layout";
 * Astra ruling 3 D3, tmp/jobs/b1-chat-opener-0919/astra-ruling-3.raw). In the
 * CavEX layout the right thumb lives on the D-pad, and only B, Nunchuk Z and
 * Nunchuk C can be pressed WHILE WALKING -- jump, break and place take all
 * three. Sneak has to be held while moving and there is no finger left for it,
 * so on the Wii Remote it becomes a toggle. That is a carve-out #2
 * playability deviation from Beta's held Shift
 * [740c583 MovementInputFromOptions], documented in
 * docs/wii_controller_playability.md, opt-out through
 * controls.wiimote_sneak_toggle = 0 (which restores the faithful HOLD), and
 * scoped to the Wii Remote + Nunchuk family only -- the Classic Controller,
 * the GameCube pad and PC all keep the hold, where a shoulder button or a key
 * is free while moving.
 *
 * WHAT THIS MACHINE OWNS, and nothing else:
 *   - one toggle per FRESH press (a hold never re-toggles);
 *   - the two clear edges it can see for itself: death/respawn (any change of
 *     `dead`) and a successful vehicle or bed ENTRY (the rising edge of
 *     `vehicle`);
 *   - PRESERVE through a frame a screen owns, with that frame's input
 *     SUPPRESSED and a NEUTRAL REARM required on the way back. Remote 1 is
 *     both the menu Close and the sneak button, so the press that closes a
 *     menu is still down when gameplay resumes and must not toggle.
 *
 * WHAT input.c OWNS (the rest of Astra D3's clear list, because none of it is
 * visible from here): world/session leave or replacement, controller
 * disconnect, and an owner or family change. Each of those zeroes the whole
 * struct, which lands back in the unarmed state above.
 *
 * D3 also lists a SCHEME-MODE change. There is no run-time mechanism for one
 * and none is needed: `controls.wiimote_sneak_toggle` is read exactly once, at
 * BOOT, by input_init(), which main.c calls from its start-up path and never
 * again, and nothing rewrites the config while the game runs. The clear is
 * satisfied because the change cannot happen, not by machinery (review 09
 * finding 5; reported to Astra).
 */

#ifndef INPUT_SNEAK_TOGGLE_H
#define INPUT_SNEAK_TOGGLE_H

#include <stdbool.h>

/* Absent `controls.wiimote_sneak_toggle` means TOGGLE on the Remote family:
 * the shipped scheme's behaviour is also the fallback, so a config that simply
 * does not mention the key plays the layout the maintainer chose. An explicit
 * 0 is the faithful hold. */
#define INPUT_SNEAK_TOGGLE_DEFAULT 1

struct input_sneak_toggle {
	bool engaged;	  /* the latched sneak level this machine publishes */
	bool button_down; /* the previous poll's RAW sneak level */
	/* False until a neutral frame has been seen. A zeroed machine starts
	 * here, which is what makes "held across a screen change, a reconnect or
	 * a world load" safe by construction. */
	bool armed;
	bool dead, vehicle; /* the previous poll's clear-edge levels */
};

/* One poll. `raw` is the sneak action's level BEFORE this machine replaces it;
 * `gameplay` is "the in-game screen owned the frame this level came from";
 * `dead` and `vehicle` are levels, not edges -- the machine takes their edges
 * itself so a caller cannot forget to. Returns the sneak level to publish. */
static inline bool input_sneak_toggle_step(struct input_sneak_toggle* t,
	bool raw, bool gameplay, bool dead, bool vehicle) {
	/* Clears first, so a clear that lands on the same poll as a press wins:
	 * dying with the inventory open must not leave a crouch behind. */
	if(dead != t->dead || (vehicle && !t->vehicle))
		t->engaged = false;
	t->dead = dead;
	t->vehicle = vehicle;
	if(!gameplay) {
		/* A screen owns the frame: PRESERVE the state, suppress the input,
		 * and make the way back cost a neutral frame. */
		t->armed = false;
	} else if(!t->armed) {
		if(!raw)
			t->armed = true;
	} else if(raw && !t->button_down) {
		t->engaged = !t->engaged;
	}
	t->button_down = raw;
	return t->engaged;
}

#endif
