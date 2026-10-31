/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/*
	mp_reconnect_soak hardware profile: the engine-facing driver around the
	pure mp_reconnect_soak_core state machine (see that header for the
	design). This is the impure half -- it reads gstate.remote_session /
	game_state_client_entities_active(&gstate) / gstate.stats.dt and calls
	clin_send_unload_world() / screen_set(screen_multiplayer_connect), the
	same shape autopilot.c is for autopilot_unstick.h and menu_autopilot.c is
	for menu_autopilot_script.h.

	Gated entirely by mp_reconnect_soak_arm(): a normal run (any other
	profile, or no hw_run at all) never calls it, so this file's frame_tick
	is a guaranteed no-op and every other profile's behavior is byte-
	identical to before this file existed. See main.c (hw_run wiring).
*/

#ifndef GAME_MP_RECONNECT_SOAK_H
#define GAME_MP_RECONNECT_SOAK_H

#include <stdbool.h>

// Arm the cycle driver (call once at boot, only when hw_run.enabled &&
// hw_run.mp_reconnect_soak). A no-op state machine until frame_tick sees the
// first successful connect (cycle 1 rides the pre-existing boot-time
// mp_autoconnect entry -- see mp_reconnect_soak_core.h's top comment).
void mp_reconnect_soak_arm(void);

// Advance one frame (call exactly once per frame, from main.c's main loop,
// after clin_update() so this frame's world_loaded/remote_session state is
// current). No-op unless armed.
void mp_reconnect_soak_frame_tick(void);

// Telemetry getters (source/main.c wires these into the hw_telemetry
// heartbeat). All return 0/false when not armed.
int mp_reconnect_soak_cycles(void);
int mp_reconnect_soak_target(void);
bool mp_reconnect_soak_wedged(void);

#endif
