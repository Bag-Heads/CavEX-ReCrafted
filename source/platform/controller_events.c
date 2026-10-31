/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>

#include "controller_events.h"

bool controller_event_cue(enum controller_event event,
						  enum menu_sound_event* out) {
	switch(event) {
		/* "Connection cue once for a genuine detected/usable device event",
		 * and "Connection cue once when a usable device returns". */
		case CONTROLLER_EVENT_DETECTED:
		case CONTROLLER_EVENT_CONNECTED:
		case CONTROLLER_EVENT_RECONNECTED:
		case CONTROLLER_EVENT_ATTACHMENT_CHANGED:
			*out = MENU_SOUND_CONNECTED;
			return true;
		/* "Controller lost / required attachment missing / join failed:
		 * attention cue once on a stable new event. Failure uses this event
		 * cue, never the successful-action confirmation." */
		case CONTROLLER_EVENT_DISCONNECTED:
		case CONTROLLER_EVENT_ATTACHMENT_MISSING:
		case CONTROLLER_EVENT_JOIN_FAILED:
			*out = MENU_SOUND_ATTENTION;
			return true;
		case CONTROLLER_EVENT_LOW_BATTERY:
			*out = MENU_SOUND_BATTERY_ATTENTION;
			return true;
		default: return false;
	}
}

void controller_events_announce(struct input_inventory* inventory,
								ptime_t now) {
	if(!inventory)
		return;
	/* Re-arm first: a device whose episode ended in this same frame must be
	 * able to raise a fresh episode's cue immediately afterwards. */
	for(unsigned index = 0; index < INPUT_DEVICE_COUNT; index++) {
		struct input_device_status* status = &inventory->devices[index];
		if(!status->battery_rearm)
			continue;
		status->battery_rearm = false;
		audio_menu_battery_rearm((struct input_device) {
			index < 4 ? INPUT_DEVICE_WPAD : INPUT_DEVICE_GAMECUBE, index % 4});
	}
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++) {
		struct input_connection_notice* notice = &inventory->notices[k];
		enum menu_sound_event cue;
		if(!notice->active || notice->announced)
			continue;
		/* One decision per edge, whether or not it turns out to be audible. */
		notice->announced = true;
		if(controller_event_cue(notice->event, &cue))
			audio_menu_device_event(cue, input_notice_device(notice), now);
	}
}

enum controller_recovery_action
controller_recovery_step(const struct controller_recovery* recovery,
						 struct controller_recovery_input in,
						 struct controller_recovery* next) {
	if(!recovery || !next)
		return CONTROLLER_RECOVERY_ACT_NONE;
	*next = *recovery;
	/* The player resumed by their own means (or the world was reloaded): the
	 * machine gives the pause back rather than holding a gate over a running
	 * game. */
	if(!in.paused && next->owns_pause) {
		next->owns_pause = false;
		next->phase = CONTROLLER_RECOVERY_NONE;
	}
	if(!in.present) {
		next->phase = CONTROLLER_RECOVERY_LOST;
		next->reason = in.reason == CONTROLLER_EVENT_NONE ?
			CONTROLLER_EVENT_DISCONNECTED :
			in.reason;
		/* Pause and STAY paused. The menu and its focus are untouched. */
		return in.paused ? CONTROLLER_RECOVERY_ACT_NONE :
						   CONTROLLER_RECOVERY_ACT_PAUSE;
	}
	if(next->phase == CONTROLLER_RECOVERY_NONE)
		return CONTROLLER_RECOVERY_ACT_NONE;
	/* Back, but never resumed by coming back. */
	if(next->phase == CONTROLLER_RECOVERY_LOST)
		next->phase = CONTROLLER_RECOVERY_NEUTRAL;
	if(next->phase == CONTROLLER_RECOVERY_READY && !in.neutral)
		next->phase = CONTROLLER_RECOVERY_NEUTRAL;
	if(next->phase == CONTROLLER_RECOVERY_NEUTRAL && in.neutral)
		next->phase = CONTROLLER_RECOVERY_READY;
	/* Only a press that arrives on a frame the gate was ALREADY open counts:
	 * the neutral pass and the deliberate press can never be the same frame,
	 * so a button held across the reconnect cannot resume. */
	if(recovery->phase == CONTROLLER_RECOVERY_READY && in.resume
	   && next->phase == CONTROLLER_RECOVERY_READY && in.paused)
		return next->owns_pause ? CONTROLLER_RECOVERY_ACT_RESUME :
			CONTROLLER_RECOVERY_ACT_DISMISS;
	return CONTROLLER_RECOVERY_ACT_NONE;
}

void controller_recovery_commit(struct controller_recovery* recovery,
								const struct controller_recovery* next,
								enum controller_recovery_action action,
								bool succeeded) {
	if(!recovery || !next)
		return;
	if(action != CONTROLLER_RECOVERY_ACT_NONE && !succeeded)
		/* The RPC was rejected. Keep the machine where the world is; the next
		 * frame retries the same transition. */
		return;
	*recovery = *next;
	if(action == CONTROLLER_RECOVERY_ACT_PAUSE)
		recovery->owns_pause = true;
	else if(action == CONTROLLER_RECOVERY_ACT_RESUME
			|| action == CONTROLLER_RECOVERY_ACT_DISMISS) {
		recovery->owns_pause = false;
		recovery->phase = CONTROLLER_RECOVERY_NONE;
		recovery->reason = CONTROLLER_EVENT_NONE;
	}
}

bool controller_recovery_card(const struct controller_recovery* recovery,
							  enum input_category family, bool split,
							  struct controller_recovery_card* out) {
	if(!recovery || !out || recovery->phase == CONTROLLER_RECOVERY_NONE)
		return false;
	memset(out, 0, sizeof(*out));
	/* "In split-screen, local pause does not pretend the world is stopped." */
	snprintf(out->status, sizeof(out->status), "%s",
			 split ? "World continues" : "Game paused");
	out->picture = controller_picture_of(
		(struct input_device) {family == INPUT_CAT_GAMECUBE ?
								   INPUT_DEVICE_GAMECUBE :
								   INPUT_DEVICE_WPAD,
							   0},
		family);
	if(recovery->phase == CONTROLLER_RECOVERY_LOST) {
		if(recovery->reason == CONTROLLER_EVENT_ATTACHMENT_MISSING) {
			snprintf(out->title, sizeof(out->title),
					 "Attach Nunchuk or Classic");
			snprintf(out->detail, sizeof(out->detail), "Wii Remote");
			out->picture = CONTROLLER_PICTURE_REMOTE;
		} else {
			snprintf(out->title, sizeof(out->title),
					 "Controller disconnected");
			snprintf(out->detail, sizeof(out->detail),
					 "Reconnect your controller.");
		}
		return true;
	}
	snprintf(out->title, sizeof(out->title), "%s",
			 recovery->reason == CONTROLLER_EVENT_ATTACHMENT_CHANGED ?
				 "Controller changed" :
				 "Controller reconnected");
	if(recovery->phase == CONTROLLER_RECOVERY_NEUTRAL) {
		/* Deviation (docs/wii_controller_playability.md): the approved
		 * prototype's "centre" is kept verbatim. */
		snprintf(out->detail, sizeof(out->detail),
				 "Release buttons and centre the stick.");
		return true;
	}
	snprintf(out->detail, sizeof(out->detail), "%s",
			 controller_family_name(family));
	snprintf(out->action, sizeof(out->action), "Resume");
	return true;
}
