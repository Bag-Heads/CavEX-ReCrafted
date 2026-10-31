/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The shared TAP gesture: pure, header-only, no platform headers, so the rule
 * can be unit-tested on PC (tests/test_chat_gesture.c) while input.c owns the
 * wiring. TWO consumers use the same machine, each with its own state:
 *
 *   - the chat OPENER (input_chat_open_edge), described below;
 *   - the Wii Remote HOTBAR step (input_hotbar_step_edge, b1-chat-opener-0919
 *     slice 1 / Astra ruling 3 D2). In the maintainer's CavEX layout Minus and
 *     Plus carry BOTH the hotbar and the F3 chord, so a press-edge hotbar
 *     would move a slot every time the overlay was toggled. Each half runs one
 *     gesture with the OTHER half as its partner, so any overlap cancels both
 *     -- exactly the rule already ratified for Classic Plus.
 *
 * WHY IT EXISTS (maintainer console report 2026-09-19 + Astra gate 2 D2,
 * tmp/jobs/b1-chat-opener-0919/astra-ruling-1.raw). The opener used to be
 * D-pad Up, which collides with movement on the Wii Remote. It moves to
 * button 2 on the Remote, PLUS on the Classic and X on the GameCube pad --
 * and neither replacement can be a plain press edge:
 *
 *   - Wii Remote 2 is ALSO the pointer-look recenter while it is HELD
 *     (input.c's input_native_joystick / input_player_look). So the opener
 *     is a TAP: armed by a fresh press, fired on the RELEASE, and only while
 *     the hold stayed inside INPUT_CHAT_TAP_MS. A longer hold is a recenter
 *     and opens nothing.
 *   - Classic PLUS is half of the debug-overlay chord (Plus+Minus, code
 *     212213). The chord detector CONSUMES both components the moment they
 *     are both down (input_owner.h input_binding_levels), and there is no
 *     chord window, so an immediate press could not be taken back once Minus
 *     arrived later. Astra's answer: fire on the RELEASE, and cancel the
 *     gesture if the partner is held at ANY moment in between.
 *   - GameCube X has no partner and no conflicting hold, so it stays a plain
 *     press edge and never reaches this machine.
 *
 * After a cancel, an expiry or a fired gesture, BOTH buttons must be seen
 * neutral before it can arm again. The state belongs to ONE owning device and
 * one binding, and a poll gap (the in-game gate was not called that frame --
 * a screen change), an owner change or a disconnect clears it.
 */

#ifndef INPUT_TAP_GESTURE_H
#define INPUT_TAP_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

/* The Wii Remote's tap window, in milliseconds. Deliberately generous: this
 * is a Wii-controller playability value (CLAUDE.md carve-out #2), not a Beta
 * constant -- it only has to separate "a tap" from "a deliberate recentring
 * hold", and a nervous player must not lose the opener to a slow thumb. */
#define INPUT_CHAT_TAP_MS 500u

/* The Wii Remote's HOTBAR tap window (Astra ruling 3 D2: "pin the hotbar
 * tap-duration boundary in the brief and test it"). Same number, different
 * reason, so it gets its own name: here the window separates "a tap that
 * cycles a slot" from "the start of a hold Minus + press Plus chord", and a
 * hold longer than this must produce NO slot change at all -- not even a late
 * one. Also a carve-out #2 playability value, provisional until the maintainer
 * has tapped it on real hardware. */
#define INPUT_HOTBAR_TAP_MS 500u

/* The HITCH TOLERANCE, in observed DOWN polls (Astra ruling 4 D2, review 09
 * finding 3). A chunk-load stall can swallow every poll between a press and
 * its release, so a genuine tap arrives as two samples 800 ms -- or three
 * seconds -- apart, and the wall-clock window above would throw it away. So a
 * release ALSO counts as a tap when the button was observed DOWN on at most
 * this many ACTUAL controller polls of that player, however long the hitch.
 *
 * The consequence is stated rather than hidden, because sparse sampling
 * genuinely cannot tell the two apart: across a stall, EITHER a tap OR a real
 * hold may produce ONE release step. What never steps is an OBSERVED long hold
 * -- more than this many down polls AND longer than the window -- and that is
 * the case the rule exists to refuse, because it is the one a player can
 * actually perform at 60 Hz.
 *
 * Scoped to the hotbar: the CHAT opener passes 0 (no tolerance) and keeps
 * today's pure wall-clock rule, since tapping 2 again is a free retry there
 * while a lost hotbar step is a wrong slot in your hand. */
#define INPUT_HOTBAR_HITCH_POLLS 2u

enum {
	/* Nothing may arm until both buttons have been seen neutral. A zeroed
	 * gesture starts here, which is what makes "held across a screen change /
	 * a reconnect" safe by construction. */
	INPUT_TAP_GESTURE_BLOCKED = 0,
	INPUT_TAP_GESTURE_IDLE,
	INPUT_TAP_GESTURE_ARMED,
};

struct input_tap_gesture {
	unsigned char state;
	bool opener_down;  /* the previous tick's physical opener level */
	uint32_t armed_ms; /* when the current arm started */
	/* How many polls of THIS gesture's own player have seen the opener down
	 * since it armed, saturated one past the caller's tolerance -- that is all
	 * the rule ever asks. The arming poll counts as the first. */
	unsigned char down_polls;
};

/* One tick of the bare machine, with the hitch tolerance spelled out.
 *
 * `opener`/`partner` are PHYSICAL levels (never action levels: the chord
 * consumes the action, which would read as a release and fire). `now_ms` is
 * any monotonic millisecond reading -- only unsigned differences are taken, so
 * its wrap is harmless. `hold_ms` is the tap window; 0 means "no expiry" (the
 * release flavour). `hitch_polls` is INPUT_HOTBAR_HITCH_POLLS for the hotbar
 * and 0 for every other consumer, where 0 restores the pure wall-clock rule
 * exactly (the arming poll alone already exceeds a tolerance of 0). Returns
 * true on the single tick the consumer must act on. */
static inline bool input_tap_gesture_step_hitch(struct input_tap_gesture* g,
												 bool opener, bool partner,
												 uint32_t now_ms,
												 uint32_t hold_ms,
												 unsigned hitch_polls) {
	bool fresh = opener && !g->opener_down;
	bool open = false;
	if(g->state == INPUT_TAP_GESTURE_ARMED) {
		/* Count THIS poll's observation first, so the test below asks "how
		 * many down polls has the game actually seen", including this one. */
		if(opener && g->down_polls <= hitch_polls
		   && g->down_polls < (unsigned char)255)
			g->down_polls++;
		/* An OBSERVED long hold: past the window AND sampled often enough that
		 * the length is a real hold rather than an unlucky stall. */
		bool observed_hold = hold_ms
			&& (uint32_t)(now_ms - g->armed_ms) > hold_ms
			&& g->down_polls > hitch_polls;
		if(partner || observed_hold) {
			/* A chord half, or a hold that is no longer a tap. Either way the
			 * gesture is spent, and neutral is the only way back. */
			g->state = INPUT_TAP_GESTURE_BLOCKED;
		} else if(!opener) {
			open = true;
			g->state = INPUT_TAP_GESTURE_IDLE;
		}
	}
	if(g->state == INPUT_TAP_GESTURE_BLOCKED && !opener && !partner)
		g->state = INPUT_TAP_GESTURE_IDLE;
	if(g->state == INPUT_TAP_GESTURE_IDLE && fresh) {
		/* A press that lands INTO a held partner is a chord half, never an
		 * arm -- the staggered order of the same cancel. */
		g->state = partner ? INPUT_TAP_GESTURE_BLOCKED
						   : INPUT_TAP_GESTURE_ARMED;
		g->armed_ms = now_ms;
		g->down_polls = 1; /* this poll saw it down */
	}
	g->opener_down = opener;
	return open;
}

/* The tolerance-free flavour: the chat opener's rule, unchanged, and the one
 * tests/test_chat_gesture.c drives. */
static inline bool input_tap_gesture_step(struct input_tap_gesture* g,
										   bool opener, bool partner,
										   uint32_t now_ms, uint32_t hold_ms) {
	return input_tap_gesture_step_hitch(g, opener, partner, now_ms, hold_ms, 0u);
}

/* The machine plus everything it is SCOPED to: the poll it last advanced on,
 * the owning device, and the opener code the state belongs to. */
struct input_tap_scope {
	struct input_tap_gesture gesture;
	unsigned long serial; /* the hardware poll this state was advanced on */
	bool open;            /* that poll's answer, latched */
	int kind;             /* enum input_device_kind of the owner */
	unsigned channel;
	int opener; /* the bound opener code, 0 when this gate does not apply */
};

/* Advance the scoped gesture for one hardware poll and answer "open chat now".
 *
 * `serial` is input.c's per-poll counter: a repeat call inside the SAME poll
 * returns the latched answer without advancing anything (several consumers,
 * one answer), and any gap -- the in-game gate was not called last poll,
 * i.e. another screen owned the frame -- clears the gesture. So do a change of
 * owning device, a change of binding, and `connected == false`. */
static inline bool input_tap_gesture_tick(struct input_tap_scope* s,
										   unsigned long serial, int kind,
										   unsigned channel, int opener,
										   uint32_t hold_ms,
										   unsigned hitch_polls, bool connected,
										   bool opener_down, bool partner_down,
										   uint32_t now_ms) {
	if(s->serial == serial)
		return s->open;
	if(serial != s->serial + 1 || !connected || kind != s->kind
	   || channel != s->channel || opener != s->opener) {
		s->gesture = (struct input_tap_gesture) {0};
		s->kind = kind;
		s->channel = channel;
		s->opener = opener;
	}
	s->serial = serial;
	/* Exactly one advance per ACTUAL poll of this scope's own device, which is
	 * what makes the hitch tolerance's "down polls of THAT player" countable
	 * here: a repeat call inside the same poll returned above. */
	s->open = connected && opener
		&& input_tap_gesture_step_hitch(&s->gesture, opener_down, partner_down,
										 now_ms, hold_ms, hitch_polls);
	return s->open;
}

#endif
