/*
	CaveWii_v2 CAVEWII_AGENT bridge -- ACT-half (synthetic input driver).

	The act side of the perceive->act bridge whose perceive half is state_export_emit.
	A background thread reads NAME=VALUE commands from stdin (env-gated CAVEWII_AGENT=1,
	PC dev rig only) and drives the REAL local player through the engine's own input
	layer -- input_held / input_pressed / input_released and the camera look -- so the
	byte-faithful physics walk/mine/place the character with no human at the controls.

	Two button kinds (see agent_input.c):
	  * HOLD buttons (FORWARD/BACK/LEFT/RIGHT/JUMP/SNEAK/MINE) -- a level the agent
	    holds until told otherwise; drives input_held (movement) and the dig hold.
	  * PULSE buttons (PLACE/USE, INVENTORY, SCROLL_LEFT/RIGHT) -- a one-shot RISING
	    EDGE on input_pressed, which is what block-place / use / hotbar actually gate
	    on. This is the must-fix: forcing input_held alone never fires an
	    input_pressed-gated action (that sank the earlier paste-package).

	Edges are frame-latched (advanced once per frame by agent_input_poll) so input_pressed
	is idempotent within a frame -- safe for the screen handler that reads it several times.
*/

#ifndef AGENT_INPUT_H
#define AGENT_INPUT_H

#include <stdbool.h>

#include "../platform/input.h"

// Start the stdin command reader thread IFF the bridge is armed (env CAVEWII_AGENT=1).
// A no-op otherwise, so a normal run spawns no thread and reads no stdin. Call once,
// after input_init().
void agent_input_init(void);

// Advance the per-frame button edge latch (rising/falling edges for
// input_pressed/released, and pulse-button one-shots). Call exactly once per frame,
// before any input consumer runs this frame.
void agent_input_poll(void);

// True iff the bridge is armed (CAVEWII_AGENT=1; cached). When false, every hook
// below returns false so the engine falls through to native input untouched.
bool agent_input_armed(void);

// True iff a MENU button (CHAT / HOME / the GUI directions / click / backspace /
// send / page / previous) has been named at least once, i.e. the bridge owns the
// menu surface right now. RESET gives every menu button back to native input and
// this returns false again.
//
// B1b (Astra 2026-09-17, "APPROVE B1b's agent-armed PC controller mode, scoped to
// chat and explicit bridge activation"): the chat composer reads THIS, not
// agent_input_armed() alone, to decide whether to stay out of PC keyboard text
// mode. An armed rig that only walks the player keeps ordinary PC text entry, so
// nothing changes for a human at the keyboard.
bool agent_input_menu_claimed(void);

// input_held / input_pressed / input_released hooks. Return true and write the agent's
// value to *out IFF the bridge is armed AND owns button b (movement + action buttons).
// Return false otherwise -> the caller falls through to native/autopilot input.
bool agent_input_held(enum input_button b, bool* out);
bool agent_input_pressed(enum input_button b, bool* out);
bool agent_input_released(enum input_button b, bool* out);

// Absolute look: if the agent has authored a yaw and/or pitch (degrees), overwrite the
// matching component of *rx / *ry (RADIANS). Convention matches the PERCEIVE schema:
// rx = yaw, ry = polar-from-up pitch where 90deg = horizon, larger = look down. An
// un-authored axis is left untouched. Returns true iff it overrode at least one axis.
bool agent_input_apply_look(float* rx, float* ry);

// Parse and apply a single command line (whitespace-separated NAME=VALUE tokens). The
// reader thread calls this per line; exposed so a unit test can feed commands without
// stdin or a thread. Does NOT require the bridge to be armed.
void agent_input_feed_line(const char* line);

#endif
