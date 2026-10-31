/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2. See mp_reconnect_soak.h.
*/

#include "mp_reconnect_soak.h"

#include <stdio.h>

#include "game_state.h"
#include "gui/screen.h"
#include "mp_reconnect_soak_core.h"
#include "../network/client_interface.h" // clin_send_unload_world

static struct mp_reconnect_soak_state s_state;

void mp_reconnect_soak_arm(void) {
	mp_reconnect_soak_state_reset(&s_state, true,
								   MP_RECONNECT_SOAK_CYCLES_TARGET);
}

void mp_reconnect_soak_frame_tick(void) {
	if(!s_state.armed)
		return;

	bool connected = game_state_client_entities_active(&gstate);
	bool remote_active = gstate.remote_session;
	enum mp_reconnect_soak_action action = mp_reconnect_soak_state_tick(
		&s_state, connected, remote_active, gstate.stats.dt);

	if(s_state.wedge_pending_report) {
		s_state.wedge_pending_report = false;
		fprintf(stderr,
				"[mp_reconnect_soak] WEDGE at cycle %d/%d (phase=%d) -- soak "
				"aborted, no further cycles will be attempted\n",
				s_state.cycles_completed, s_state.cycles_target,
				(int)s_state.phase);
	}

	switch(action) {
	case MRS_ACTION_DISCONNECT:
		// The SAME SRPC_UNLOAD_WORLD -> server_remote_disconnect() path
		// screen_pause.c's "Save and quit to title" and screen_multiplayer_
		// connect.c's mid-connect Cancel already use (server_remote_
		// protocol.c's remote_process_srpc handles SRPC_UNLOAD_WORLD FIRST,
		// unconditionally -- no pause-toggle or in-screen state required).
		// clin_send_unload_world() latches its own per-frame retry on a
		// saturated queue (client_interface.c), so a single call here is
		// sufficient even under backpressure.
		clin_send_unload_world();
		break;
	case MRS_ACTION_RECONNECT:
		// The EXACT call main.c's boot-time mp_autoconnect entry makes.
		// mpc_reset() documents repeated visits as safe ("Re-request is
		// idempotent"), and net_bringup_request() (Wii) is a no-op once
		// NET_BRINGUP_READY has latched, so this reconnects through the
		// real user-facing flow every cycle, not a shortcut around it.
		if(screen_multiplayer_connect)
			screen_set(screen_multiplayer_connect);
		break;
	default:
		break;
	}
}

int mp_reconnect_soak_cycles(void) {
	return s_state.armed ? s_state.cycles_completed : 0;
}

int mp_reconnect_soak_target(void) {
	return s_state.armed ? s_state.cycles_target : 0;
}

bool mp_reconnect_soak_wedged(void) {
	return s_state.armed && s_state.wedged;
}
