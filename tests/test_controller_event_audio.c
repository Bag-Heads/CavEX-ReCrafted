/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * The controller cues, through the REAL shared dispatcher and the REAL
 * detection layer: the approved map (connection cue for a genuine detected/
 * usable device, attention for lost/missing/join-failed, battery attention at
 * its own gain, silence for the pointer), each cue named with the actual
 * device identity, one cue per edge, and no duplicate when a device flaps
 * inside the collapse window. Frame-batched, two devices. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/audio.h"
#include "platform/controller_events.h"
#include "platform/input_inventory.h"
#include "platform/menu_sound.h"

/* Same focused-TU boundary test_menu_sound_events.c uses: keep PC time's ABI,
 * substitute only the platform audio device. */
#undef PLATFORM_PC
static unsigned starts, picked;
static float last_gain;
static uint32_t audio_backend_music_last_dur_ms(void) {
	return 0;
}
#include "../source/platform/audio.c"
#include "support/audio_deferred_stubs.h" /* C2 P2 seam, inert: opt-in off */
static bool audio_backend_init(void) {
	return true;
}
static void audio_backend_shutdown(void) {}
static void audio_backend_voice_start(int slot, const struct audio_sample* smp,
									  float rate, float l, float r) {
	(void)slot;
	(void)smp;
	(void)rate;
	(void)r;
	starts++;
	last_gain = l;
}
static void audio_backend_voice_gain(int slot, float l, float r) {
	(void)slot;
	(void)r;
	last_gain = l;
}
static bool audio_backend_voice_playing(int slot) {
	(void)slot;
	return true;
}
static void audio_backend_stop_all(void) {}
static void audio_backend_music_start(const int16_t* pcm, uint32_t frames,
									  float rate, float gain) {
	(void)pcm;
	(void)frames;
	(void)rate;
	(void)gain;
}
static void audio_backend_music_gain(float gain) {
	(void)gain;
}
static bool audio_backend_music_playing(void) {
	return false;
}
static bool audio_backend_music_drained(void) {
	return true;
}
static void audio_backend_music_stop(void) {}
static uint32_t audio_backend_music_native_rate(void) {
	return 0;
}
/* Diagnostics-only registry hook audio.c calls after every voice start
   (audio.h): the real one lives in audio_registry.c, which these
   include-the-TU tests deliberately do not link -- same reason
   audio_registry_load/_pick are stubbed just below. */
void audio_registry_note_voice_started(int slot,
                                       const struct audio_sample* smp) {
    (void)slot;
    (void)smp;
}
void audio_registry_load(void) {}
const struct audio_sample* audio_registry_pick(uint16_t id) {
	static const int16_t pcm[] = {0, 1, 0};
	static const struct audio_sample sample
		= {.pcm = pcm, .frames = 3, .rate = 48000};
	picked = id;
	return &sample;
}

static struct input_inventory inv;
static struct input_owner owners[INPUT_LOCAL_PLAYERS];

static ptime_t at(unsigned ms) {
	return time_add_ms((ptime_t) {0}, ms);
}
/* One main-loop frame: poll every device, then announce, then let the
 * dispatcher arbitrate -- exactly the order input_poll + audio_update use. */
static void frame(unsigned ms) {
	controller_events_announce(&inv, at(ms));
	audio_update((vec3) {0}, 0);
}
static void hold(struct input_device d, struct input_identity id, unsigned from,
				 unsigned to) {
	for(unsigned ms = from; ms <= to; ms++) {
		input_inventory_poll(&inv, d, id, true, true, true, at(ms), owners,
							 INPUT_LOCAL_PLAYERS);
		frame(ms);
	}
}

static const struct input_device remote = {INPUT_DEVICE_WPAD, 0};
static const struct input_device other = {INPUT_DEVICE_WPAD, 3};
static const struct input_device gc = {INPUT_DEVICE_GAMECUBE, 1};
static const struct input_identity none = {false, INPUT_CAT_NONE};
static const struct input_identity bare = {true, INPUT_CAT_WIIMOTE};
static const struct input_identity nunchuk = {true, INPUT_CAT_NUNCHUK};

static void test_cue_map(void) {
	enum menu_sound_event cue;
	assert(controller_event_cue(CONTROLLER_EVENT_DETECTED, &cue)
		   && cue == MENU_SOUND_CONNECTED);
	assert(controller_event_cue(CONTROLLER_EVENT_CONNECTED, &cue)
		   && cue == MENU_SOUND_CONNECTED);
	assert(controller_event_cue(CONTROLLER_EVENT_RECONNECTED, &cue)
		   && cue == MENU_SOUND_CONNECTED);
	assert(controller_event_cue(CONTROLLER_EVENT_ATTACHMENT_CHANGED, &cue)
		   && cue == MENU_SOUND_CONNECTED);
	assert(controller_event_cue(CONTROLLER_EVENT_DISCONNECTED, &cue)
		   && cue == MENU_SOUND_ATTENTION);
	assert(controller_event_cue(CONTROLLER_EVENT_ATTACHMENT_MISSING, &cue)
		   && cue == MENU_SOUND_ATTENTION);
	assert(controller_event_cue(CONTROLLER_EVENT_JOIN_FAILED, &cue)
		   && cue == MENU_SOUND_ATTENTION);
	assert(controller_event_cue(CONTROLLER_EVENT_LOW_BATTERY, &cue)
		   && cue == MENU_SOUND_BATTERY_ATTENTION);
	/* "Pointer out of view: visual notice only, no audio." The join offer and
	 * its cancellation are ordinary menu transitions, not device events. */
	assert(!controller_event_cue(CONTROLLER_EVENT_POINTER_LOST, &cue));
	assert(!controller_event_cue(CONTROLLER_EVENT_POINTER_REGAINED, &cue));
	assert(!controller_event_cue(CONTROLLER_EVENT_JOIN_OFFERED, &cue));
	assert(!controller_event_cue(CONTROLLER_EVENT_JOIN_CANCELLED, &cue));
	puts("PASS controller audio: approved cue map, including the events the "
		 "map keeps silent");
}

static void test_edges_are_cued_once(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	audio_init(true, 100);
	starts = 0;

	hold(remote, nunchuk, 0, 300);
	assert(starts == 1 && picked == SOUND_MENU_CONNECTED);
	/* Standing still raises no further cue, however many frames pass. */
	hold(remote, nunchuk, 301, 900);
	assert(starts == 1);

	/* A disconnect is a NEW event for the same device, well outside the
	 * collapse window: it must stay audible. */
	hold(remote, none, 1000, 1300);
	assert(starts == 2 && picked == SOUND_MENU_ATTENTION);
	hold(remote, nunchuk, 1400, 1700);
	assert(starts == 3 && picked == SOUND_MENU_CONNECTED);
	hold(remote, bare, 1800, 2100);
	assert(starts == 4 && picked == SOUND_MENU_ATTENTION);
	puts("PASS controller audio: one cue per real edge, none while the state "
		 "stands, a genuine reconnect stays audible");
}

static void test_flap_collapse_two_devices(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	audio_init(true, 100);
	starts = 0;

	/* Discovery and player assignment in the SAME connection flow: two
	 * genuine edges, one cue -- the approved collapse. */
	hold(remote, nunchuk, 0, 300);
	assert(starts == 1);
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	hold(remote, nunchuk, 301, 400);
	assert(inv.devices[0].published_assigned);
	assert(starts == 1); /* inside MENU_SOUND_NOTICE_COLLAPSE_MS */

	/* A DIFFERENT device is never collapsed into the first one's window. Two
	 * remotes that sync a few frames apart both stay audible, even though the
	 * second lands well inside the first's 500 ms window. */
	unsigned collapsed_flow = audio_menu_notices_collapsed();
	assert(collapsed_flow == 1); /* the assignment folded into the discovery */
	unsigned before = starts;
	for(unsigned ms = 401; ms <= 700; ms++) {
		input_inventory_poll(&inv, remote, nunchuk, true, true, true, at(ms),
							 owners, INPUT_LOCAL_PLAYERS);
		input_inventory_poll(&inv, other, nunchuk, true, true, true, at(ms),
							 owners, INPUT_LOCAL_PLAYERS);
		frame(ms);
	}
	assert(starts == before + 1); /* `other` published at 401 + 250 */
	before = starts;
	for(unsigned ms = 701; ms <= 1000; ms++) {
		input_inventory_poll(&inv, gc,
							 (struct input_identity) {true, INPUT_CAT_GAMECUBE},
							 true, true, true, at(ms), owners,
							 INPUT_LOCAL_PLAYERS);
		frame(ms);
	}
	assert(starts == before + 1);
	/* Nothing was collapsed on the way: a second and third device inside the
	 * first one's window are separate cues, not repeats of it. */
	assert(audio_menu_notices_collapsed() == collapsed_flow);

	/* Two devices raising the SAME cue in ONE frame is the spec's competing-
	 * event case: "emit at most one cue". The loser is discarded, never
	 * queued to a later frame. */
	before = starts;
	audio_menu_device_event(MENU_SOUND_CONNECTED, remote, at(4000));
	audio_menu_device_event(MENU_SOUND_CONNECTED, gc, at(4000));
	frame(4000);
	assert(starts == before + 1);
	frame(4001);
	assert(starts == before + 1);

	/* And a repeat of the SAME cue for the SAME device inside the collapse
	 * window is collapsed -- counted by the dispatcher, not lost silently. */
	unsigned collapsed = audio_menu_notices_collapsed();
	audio_menu_device_event(MENU_SOUND_CONNECTED, other, at(5000));
	frame(5000);
	audio_menu_device_event(MENU_SOUND_CONNECTED, other,
							at(5000 + MENU_SOUND_NOTICE_COLLAPSE_MS - 1));
	frame(5000 + MENU_SOUND_NOTICE_COLLAPSE_MS - 1);
	assert(audio_menu_notices_collapsed() == collapsed + 1);
	assert(!audio_menu_contract_violations());
	puts("PASS controller audio: discovery + assignment collapse to one cue, "
		 "separate devices are never collapsed into each other, one cue wins a "
		 "shared frame, same-device flapping collapses");
}

int main(void) {
	test_cue_map();
	test_edges_are_cued_once();
	test_flap_collapse_two_devices();
	return 0;
}
