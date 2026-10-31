/*
	CAVEWII_AGENT bridge ACT-half regression test.

	Drives agent_input's parser + frame-latch directly (no thread, no stdin) and asserts
	the exact edge semantics the engine relies on:
	  - HOLD buttons (FORWARD/MINE) -> input_held level + a single rising edge on press;
	  - PULSE buttons (PLACE) -> exactly one input_pressed rising edge per command, with
	    a release frame between queued pulses so N commands == N distinct presses (the
	    must-fix: forcing a hold never re-fires an input_pressed-gated place);
	  - non-owned buttons fall through (hooks return false);
	  - absolute LOOK maps degrees -> radians on the authored axes only.

	Arms the bridge via the CAVEWII_AGENT env (set before the first armed check) so the
	real arming path is exercised, not a test back door.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "game/agent_input.h"
#include "game/gui/chat_composer_model.h"
#include "platform/input.h"

static bool held(enum input_button b) {
	bool v = false;
	bool owned = agent_input_held(b, &v);
	assert(owned); // every button queried here is agent-owned
	return v;
}

static bool pressed(enum input_button b) {
	bool v = false;
	bool owned = agent_input_pressed(b, &v);
	assert(owned);
	return v;
}

static bool released(enum input_button b) {
	bool v = false;
	bool owned = agent_input_released(b, &v);
	assert(owned);
	return v;
}

int main(void) {
	// Arm BEFORE the first armed check (cached on first read).
	static char env[] = "CAVEWII_AGENT=1";
	putenv(env);
	assert(agent_input_armed());

	// ---- HOLD button: FORWARD ----
	agent_input_feed_line("FORWARD=1");
	agent_input_poll();
	assert(held(IB_FORWARD));
	assert(pressed(IB_FORWARD));   // rising edge on the first latched frame
	assert(!released(IB_FORWARD));

	agent_input_poll();            // still held, no new edge
	assert(held(IB_FORWARD));
	assert(!pressed(IB_FORWARD));
	assert(!released(IB_FORWARD));

	agent_input_feed_line("FORWARD=0");
	agent_input_poll();
	assert(!held(IB_FORWARD));
	assert(released(IB_FORWARD));   // falling edge exactly once
	agent_input_poll();
	assert(!released(IB_FORWARD));

	// ---- HOLD button: MINE (== left click held, IB_ACTION1) ----
	agent_input_feed_line("MINE=1");
	agent_input_poll();
	assert(held(IB_ACTION1));
	assert(pressed(IB_ACTION1));    // one rising edge (the left-click entity hit)
	agent_input_poll();
	assert(held(IB_ACTION1));       // stays held -> the dig loop keeps breaking
	assert(!pressed(IB_ACTION1));
	agent_input_feed_line("MINE=0");
	agent_input_poll();
	assert(!held(IB_ACTION1));
	assert(released(IB_ACTION1));

	// ---- PULSE button: PLACE (IB_ACTION2) -> one press, idempotent within the frame ----
	agent_input_feed_line("PLACE=1");
	agent_input_poll();
	assert(pressed(IB_ACTION2));
	assert(pressed(IB_ACTION2));    // idempotent: same value no matter how many reads
	assert(held(IB_ACTION2));       // a pulse is momentarily down for its one press frame
	agent_input_poll();
	assert(!pressed(IB_ACTION2));   // exactly one frame of press
	assert(!held(IB_ACTION2));      // back up the very next frame
	assert(released(IB_ACTION2));

	// ---- two queued PLACE pulses -> two DISTINCT presses (release frame between) ----
	agent_input_feed_line("PLACE=1 PLACE=1"); // multiple commands on one line
	agent_input_poll();
	assert(pressed(IB_ACTION2));     // press #1
	agent_input_poll();
	assert(!pressed(IB_ACTION2));    // forced release so the next can re-edge
	agent_input_poll();
	assert(pressed(IB_ACTION2));     // press #2
	agent_input_poll();
	assert(!pressed(IB_ACTION2));

	// PLACE=0 is a no-op for a pulse button (no phantom press queued).
	agent_input_feed_line("PLACE=0");
	agent_input_poll();
	assert(!pressed(IB_ACTION2));

	// ---- non-owned button falls through (hook returns false) ----
	{
		bool v = false;
		assert(!agent_input_held(IB_HOME, &v));
		assert(!agent_input_pressed(IB_HOME, &v));
		assert(!agent_input_pressed(IB_DEBUG_OVERLAY, &v));
	}

	// ---- absolute LOOK: degrees in -> radians out, only authored axes touched ----
	{
		float rx = 1.0f, ry = 2.0f;
		assert(!agent_input_apply_look(&rx, &ry)); // nothing authored yet
		assert(rx == 1.0f && ry == 2.0f);

		agent_input_feed_line("YAW=90");
		assert(agent_input_apply_look(&rx, &ry));   // yaw authored
		assert(fabsf(rx - (float)(M_PI / 2.0)) < 1e-4f);
		assert(ry == 2.0f);                          // pitch axis untouched

		agent_input_feed_line("PITCH=45");
		rx = ry = 0.0f;
		assert(agent_input_apply_look(&rx, &ry));
		assert(fabsf(rx - (float)(M_PI / 2.0)) < 1e-4f);
		assert(fabsf(ry - (float)(M_PI / 4.0)) < 1e-4f);
	}

	// ---- RESET clears held levels and pending pulses ----
	agent_input_feed_line("FORWARD=1 PLACE=1");
	agent_input_feed_line("RESET");
	agent_input_poll();
	assert(!held(IB_FORWARD));
	assert(!pressed(IB_ACTION2));

	// ---- MENU buttons (2026-09-17): owned ONLY once a command has named them ----
	agent_input_feed_line("RESET");
	agent_input_poll();
	{
		bool v = true;
		// Never commanded: NOT owned, so native menu input stays in charge.
		assert(!agent_input_pressed(IB_CHAT, &v));
		assert(!agent_input_held(IB_GUI_UP, &v));
		assert(!agent_input_pressed(IB_GUI_CLICK, &v));
	}
	/* B1b: an armed rig that has only walked the player has NOT claimed the
	 * menu surface, so the chat composer keeps ordinary PC text entry. */
	assert(!agent_input_menu_claimed());
	/* The composer cannot call into this app-only file (it compiles into the
	 * core pool without CAVEWII_AGENT_BRIDGE), so the bridge WRITES a
	 * core-owned flag instead. Unclaimed, nothing has written it. */
	assert(!chat_controller_mode_requested());
	agent_input_feed_line("CHAT=1");  // PULSE: exactly one pressed frame
	agent_input_poll();
	assert(pressed(IB_CHAT));
	/* ... and now it has, so the composer may enter controller mode. */
	assert(agent_input_menu_claimed());
	assert(chat_controller_mode_requested()
		   && "claiming a menu button must request the controller mode");
	agent_input_poll();
	assert(!held(IB_CHAT) && !pressed(IB_CHAT));
	agent_input_feed_line("GUI_UP=1"); // LEVEL: held until released (key-repeat)
	agent_input_poll();
	assert(pressed(IB_GUI_UP) && held(IB_GUI_UP));
	agent_input_poll();
	assert(held(IB_GUI_UP) && !pressed(IB_GUI_UP));
	agent_input_feed_line("GUI_UP=0");
	agent_input_poll();
	assert(released(IB_GUI_UP) && !held(IB_GUI_UP));
	agent_input_feed_line("GUI_CLICK=1 GUI_SEND=1 GUI_BACK=1 GUI_PAGE=1 GUI_PREVIOUS=1 HOME=1");
	agent_input_poll();
	assert(pressed(IB_GUI_CLICK) && pressed(IB_GUI_QUICK_MOVE)
		   && pressed(IB_GUI_CLICK_ALT) && pressed(IB_GUI_PAGE)
		   && pressed(IB_GUI_PREVIOUS) && pressed(IB_HOME));
	{
		bool v = true;
		// A menu button nobody named is still native, even now.
		assert(!agent_input_held(IB_GUI_DOWN, &v));
		// RESET hands every menu button back to native input.
		agent_input_feed_line("RESET");
		agent_input_poll();
		assert(!agent_input_pressed(IB_CHAT, &v));
		assert(agent_input_held(IB_FORWARD, &v) && !v); // gameplay stays owned
		/* B1b mode EXIT: with the menu surface released the composer goes back
		 * to ordinary PC text entry on its next open. */
		assert(!agent_input_menu_claimed());
		assert(!chat_controller_mode_requested()
			   && "RESET must withdraw the controller-mode request");
	}

	return 0;
}
