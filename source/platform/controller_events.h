/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef PLATFORM_CONTROLLER_EVENTS_H
#define PLATFORM_CONTROLLER_EVENTS_H

#include "input_inventory.h"
#include "menu_sound.h"

/* Reaction layer for the debounced controller events input_inventory
 * publishes: the approved audio cue map, and the approved loss/recovery gate.
 * Detection stays in input_inventory; this file never polls hardware. */

/* The cue an event is worth, per docs/ui_design/approved/specs/sound-events.md.
 * Returns false for the events the approved map keeps SILENT: pointer
 * loss/regain ("Visual notice only; no audio because crossing the sensor
 * boundary is frequent"), and the join offer/cancel, which are ordinary menu
 * transitions owned by the menu layer's own category/Back cues -- never a
 * device cue, and never the successful-action confirmation. */
bool controller_event_cue(enum controller_event event,
						  enum menu_sound_event* out);

/* Emit at most one cue per EDGE for every notice raised since the last call,
 * and re-arm the per-device low-battery latch for every device whose episode
 * ended. Edge-triggered by construction: a notice record exists only for an
 * edge, and each is announced exactly once. Safe to call every frame; the
 * dispatcher's own per-device collapse window folds a discovery and its player
 * assignment into a single connection cue. */
void controller_events_announce(struct input_inventory* inventory,
								ptime_t now);

/* ---- Loss / pause recovery ----------------------------------------------
 * Approved rule (menus.md): "Lost controller / required attachment missing
 * retain the player's menu and focus. Reconnection requires neutral input
 * then a fresh Resume action. Never auto-activate a held button or close/
 * reset the menu on reconnect." The base tree auto-resumed as soon as the
 * connection came back; this machine never does. */
enum controller_recovery_phase {
	CONTROLLER_RECOVERY_NONE,
	CONTROLLER_RECOVERY_LOST,	 /* paused, controller gone */
	CONTROLLER_RECOVERY_NEUTRAL, /* usable again, still holding something */
	CONTROLLER_RECOVERY_READY	 /* neutral seen: Resume is offered */
};
struct controller_recovery {
	enum controller_recovery_phase phase;
	/* True only while THIS machine is the reason the game is paused, so a
	 * pause the player opened themselves is never taken away from them. */
	bool owns_pause;
	enum controller_event reason;
};
struct controller_recovery_input {
	bool present; /* the player's own device is usable again (debounced) */
	bool neutral; /* a fresh neutral report seen since it came back */
	bool resume;  /* a fresh deliberate Resume press from THIS player */
	bool paused;  /* the game is paused right now */
	enum controller_event reason; /* why it was lost, while !present */
};
enum controller_recovery_action {
	CONTROLLER_RECOVERY_ACT_NONE,
	CONTROLLER_RECOVERY_ACT_PAUSE,
	CONTROLLER_RECOVERY_ACT_RESUME,
	CONTROLLER_RECOVERY_ACT_DISMISS /* restore a menu that was already paused */
};
/* Advance the machine and return the transition the CALLER must perform (the
 * pause RPC is the caller's business). The state only moves on
 * controller_recovery_commit, so a rejected RPC leaves the machine consistent
 * with the world instead of silently desynchronising. */
enum controller_recovery_action
controller_recovery_step(const struct controller_recovery* recovery,
						 struct controller_recovery_input in,
						 struct controller_recovery* next);
void controller_recovery_commit(struct controller_recovery* recovery,
								const struct controller_recovery* next,
								enum controller_recovery_action action,
								bool succeeded);

/* The persistent recovery dialog, rendered by screen.c over the retained menu. */
struct controller_recovery_card {
	char title[48];	 /* "Controller disconnected" */
	char detail[48]; /* "Reconnect your controller." */
	char status[24]; /* "Game paused" / "World continues" */
	char action[16]; /* "Resume", only once the neutral gate has passed */
	enum controller_picture picture;
};
bool controller_recovery_card(const struct controller_recovery* recovery,
							  enum input_category family, bool split,
							  struct controller_recovery_card* out);

#endif
