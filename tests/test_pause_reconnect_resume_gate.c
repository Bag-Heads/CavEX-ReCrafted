/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The approved loss/recovery gate (docs/ui_design/approved/specs/menus.md):
 * "Lost controller / required attachment missing retain the player's menu and
 * focus. Reconnection requires neutral input then a fresh Resume action.
 * Never auto-activate a held button or close/reset the menu on reconnect."
 *
 * The base tree AUTO-RESUMED the moment the connection came back
 * (source/game/gui/screen.c: `else if(!lost && recovery_paused) ... paused =
 * false`). This pure-machine test requires the new API; the production
 * screen regression is covered by test_input_wii_adapter and
 * test_container_menu. It cannot be compiled unchanged against the base. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/controller_events.h"

/* controller_events.c also carries the cue bridge, which this test never
 * reaches. Supplying the two dispatcher symbols keeps the recovery machine a
 * pure link with no audio device anywhere near it. */
void audio_menu_device_event(enum menu_sound_event event,
							 struct input_device device, ptime_t now) {
	(void)event;
	(void)device;
	(void)now;
	assert(false);
}
void audio_menu_battery_rearm(struct input_device device) {
	(void)device;
	assert(false);
}

/* One frame of the gate, with the caller's RPC result. Mirrors exactly what
 * screen_controller_recovery() does around the machine. */
static enum controller_recovery_action
step(struct controller_recovery* r, bool present, bool neutral, bool resume,
	 bool* paused, bool rpc_ok, enum controller_event reason) {
	struct controller_recovery next;
	struct controller_recovery_input in = {.present = present,
										   .neutral = neutral,
										   .resume = resume,
										   .paused = *paused,
										   .reason = reason};
	enum controller_recovery_action action
		= controller_recovery_step(r, in, &next);
	bool sent = (action == CONTROLLER_RECOVERY_ACT_PAUSE
		|| action == CONTROLLER_RECOVERY_ACT_RESUME) && rpc_ok;
	if(sent)
		*paused = action == CONTROLLER_RECOVERY_ACT_PAUSE;
	controller_recovery_commit(r, &next, action,
							   action == CONTROLLER_RECOVERY_ACT_NONE
		|| action == CONTROLLER_RECOVERY_ACT_DISMISS || sent);
	return action;
}

static void test_pause_stays_paused_until_a_fresh_press(void) {
	struct controller_recovery r = {0};
	bool paused = false;

	/* Loss pauses once. */
	assert(step(&r, false, false, false, &paused, true,
				CONTROLLER_EVENT_DISCONNECTED)
		   == CONTROLLER_RECOVERY_ACT_PAUSE);
	assert(paused && r.owns_pause && r.phase == CONTROLLER_RECOVERY_LOST);
	/* ...and only once, however long the controller stays away. */
	for(unsigned f = 0; f < 100; f++)
		assert(step(&r, false, false, false, &paused, true,
					CONTROLLER_EVENT_DISCONNECTED)
			   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(paused);

	/* The controller comes back while a button is still held: STILL PAUSED,
	 * and the gate is waiting for neutral, not offering Resume. */
	assert(step(&r, true, false, true, &paused, true, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(paused && r.phase == CONTROLLER_RECOVERY_NEUTRAL);
	for(unsigned f = 0; f < 50; f++)
		assert(step(&r, true, false, true, &paused, true, CONTROLLER_EVENT_NONE)
			   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(paused && r.phase == CONTROLLER_RECOVERY_NEUTRAL);

	/* Neutral opens the gate -- and does not, by itself, resume. */
	assert(step(&r, true, true, false, &paused, true, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(paused && r.phase == CONTROLLER_RECOVERY_READY);
	for(unsigned f = 0; f < 50; f++)
		assert(step(&r, true, true, false, &paused, true, CONTROLLER_EVENT_NONE)
			   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(paused);

	/* A fresh deliberate press, and only then, resumes. */
	assert(step(&r, true, true, true, &paused, true, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_RESUME);
	assert(!paused && !r.owns_pause && r.phase == CONTROLLER_RECOVERY_NONE);
	/* The gate is closed: it never resumes twice, and it never re-pauses a
	 * running game on its own. */
	assert(step(&r, true, true, true, &paused, true, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(!paused);
	puts("PASS pause recovery: loss pauses once, reconnect does NOT resume, "
		 "neutral then a fresh press is required");
}

static void test_held_button_cannot_resume(void) {
	struct controller_recovery r = {0};
	bool paused = false;
	step(&r, false, false, false, &paused, true, CONTROLLER_EVENT_DISCONNECTED);
	/* The neutral pass and the deliberate press can never be the same frame:
	 * a device that reports neutral AND a press together (a button pressed in
	 * the very report that opened the gate) does not resume. */
	assert(step(&r, true, true, true, &paused, true, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(r.phase == CONTROLLER_RECOVERY_READY && paused);
	assert(step(&r, true, true, true, &paused, true, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_RESUME);
	assert(!paused);
	puts("PASS pause recovery: a button held across the reconnect cannot "
		 "resume in the frame the neutral gate opens");
}

static void test_attachment_missing_and_rejected_rpc(void) {
	struct controller_recovery r = {0};
	struct controller_recovery_card card;
	bool paused = false;

	/* A required attachment going missing is a loss, with its own dialog. */
	assert(step(&r, false, false, false, &paused, true,
				CONTROLLER_EVENT_ATTACHMENT_MISSING)
		   == CONTROLLER_RECOVERY_ACT_PAUSE);
	assert(r.reason == CONTROLLER_EVENT_ATTACHMENT_MISSING);
	assert(controller_recovery_card(&r, INPUT_CAT_NUNCHUK, false, &card));
	assert(!strcmp(card.title, "Attach Nunchuk or Classic"));
	assert(!strcmp(card.detail, "Wii Remote"));
	assert(!strcmp(card.status, "Game paused"));
	assert(!card.action[0]); /* nothing to resume with yet */

	/* Split-screen never pretends the shared world stopped. */
	assert(controller_recovery_card(&r, INPUT_CAT_NUNCHUK, true, &card));
	assert(!strcmp(card.status, "World continues"));

	step(&r, true, false, false, &paused, true, CONTROLLER_EVENT_NONE);
	assert(controller_recovery_card(&r, INPUT_CAT_NUNCHUK, false, &card));
	assert(!strcmp(card.title, "Controller reconnected"));
	assert(!strcmp(card.detail, "Release buttons and centre the stick."));
	assert(!card.action[0]);
	step(&r, true, true, false, &paused, true, CONTROLLER_EVENT_NONE);
	assert(controller_recovery_card(&r, INPUT_CAT_NUNCHUK, false, &card));
	assert(!strcmp(card.detail, "Wii Remote + Nunchuk"));
	assert(!strcmp(card.action, "Resume"));

	/* A rejected pause RPC leaves the machine consistent with the world: the
	 * next frame retries rather than believing a pause that never happened. */
	struct controller_recovery q = {0};
	bool unpaused = false;
	assert(step(&q, false, false, false, &unpaused, false,
				CONTROLLER_EVENT_DISCONNECTED)
		   == CONTROLLER_RECOVERY_ACT_PAUSE);
	assert(!unpaused && !q.owns_pause);
	assert(step(&q, false, false, false, &unpaused, true,
				CONTROLLER_EVENT_DISCONNECTED)
		   == CONTROLLER_RECOVERY_ACT_PAUSE);
	assert(unpaused && q.owns_pause);
	/* A rejected RESUME leaves the gate open and the world paused. */
	step(&q, true, true, false, &unpaused, true, CONTROLLER_EVENT_NONE);
	assert(q.phase == CONTROLLER_RECOVERY_READY);
	assert(step(&q, true, true, true, &unpaused, false, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_RESUME);
	assert(unpaused && q.phase == CONTROLLER_RECOVERY_READY && q.owns_pause);
	puts("PASS pause recovery: attachment-missing dialog, split status, "
		 "rejected pause/resume RPCs never desynchronise the gate");
}

static void test_only_the_affected_player_resumes(void) {
	/* One machine per player, each fed ONLY from its own owner frame -- which
	 * is why another player's press can never resume player 1. Player 2 here
	 * mashes Resume through every frame of player 1's recovery. */
	struct controller_recovery p1 = {0}, p2 = {0};
	bool paused = false, p2_paused = false;
	step(&p1, false, false, false, &paused, true, CONTROLLER_EVENT_DISCONNECTED);
	assert(paused);
	for(unsigned f = 0; f < 20; f++) {
		/* Player 2's controller is fine and its Resume press is live. */
		assert(step(&p2, true, true, true, &p2_paused, true,
					CONTROLLER_EVENT_NONE)
			   == CONTROLLER_RECOVERY_ACT_NONE);
		/* Player 1's own frame reports nothing: still lost, still paused. */
		assert(step(&p1, false, false, false, &paused, true,
					CONTROLLER_EVENT_DISCONNECTED)
			   == CONTROLLER_RECOVERY_ACT_NONE);
		assert(paused && p1.phase == CONTROLLER_RECOVERY_LOST);
	}
	assert(!p2.owns_pause && p2.phase == CONTROLLER_RECOVERY_NONE);
	puts("PASS pause recovery: another player's input during recovery cannot "
		 "resume the affected player");
}

static void test_player_owned_pause_is_never_taken_away(void) {
	struct controller_recovery r = {0};
	bool paused = true; /* the player opened the pause menu themselves */
	/* The controller is fine: the gate does nothing at all. */
	for(unsigned f = 0; f < 10; f++)
		assert(step(&r, true, true, true, &paused, true, CONTROLLER_EVENT_NONE)
			   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(paused && !r.owns_pause);
	/* Now the controller is lost while already paused: nothing to pause. */
	assert(step(&r, false, false, false, &paused, true,
				CONTROLLER_EVENT_DISCONNECTED)
		   == CONTROLLER_RECOVERY_ACT_NONE);
	assert(paused && !r.owns_pause && r.phase == CONTROLLER_RECOVERY_LOST);
	/* It comes back, goes neutral, and is pressed -- but this machine does
	 * not own that pause, so it does not resume out of the player's menu. */
	step(&r, true, false, false, &paused, true, CONTROLLER_EVENT_NONE);
	step(&r, true, true, false, &paused, true, CONTROLLER_EVENT_NONE);
	assert(r.phase == CONTROLLER_RECOVERY_READY);
	assert(step(&r, true, true, true, &paused, true, CONTROLLER_EVENT_NONE)
		   == CONTROLLER_RECOVERY_ACT_DISMISS);
	assert(paused && r.phase == CONTROLLER_RECOVERY_NONE);
	puts("PASS pause recovery: a pause the player opened is never resumed by "
		 "the recovery gate");
}

int main(void) {
	test_pause_stays_paused_until_a_fresh_press();
	test_held_button_cannot_resume();
	test_attachment_missing_and_rejected_rpc();
	test_only_the_affected_player_resumes();
	test_player_owned_pause_is_never_taken_away();
	return 0;
}
