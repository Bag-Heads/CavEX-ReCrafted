/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * "Battery low: attention at 65% of its normal cue gain, once per low-battery
 * episode, rearmed only after a stable recovery/replacement. No periodic
 * chirp, and never for a wired GameCube controller."
 * (docs/ui_design/approved/specs/sound-events.md)
 *
 * Detection lives in input_inventory (hysteresis + a stable window); the cue
 * latch lives in the shared dispatcher. Both are real here, joined by the
 * production controller_events_announce. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/audio.h"
#include "platform/controller_events.h"
#include "platform/input_inventory.h"
#include "platform/menu_sound.h"

/* Same focused-TU boundary test_menu_sound_events.c uses. */
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
static const struct input_device remote = {INPUT_DEVICE_WPAD, 0};
static const struct input_device other = {INPUT_DEVICE_WPAD, 2};
static const struct input_device gc = {INPUT_DEVICE_GAMECUBE, 0};
static const struct input_identity nunchuk = {true, INPUT_CAT_NUNCHUK};
static const struct input_identity none = {false, INPUT_CAT_NONE};

static ptime_t at(unsigned ms) {
	return time_add_ms((ptime_t) {0}, ms);
}
/* One frame: the identity poll, the battery read, the announce, the mixer. */
static void frame(unsigned ms, struct input_device d, struct input_identity id,
				  bool supported, unsigned percent) {
	input_inventory_poll(&inv, d, id, true, true, true, at(ms), owners,
						 INPUT_LOCAL_PLAYERS);
	input_inventory_battery(&inv, d, supported, percent, at(ms), owners,
							INPUT_LOCAL_PLAYERS);
	controller_events_announce(&inv, at(ms));
	audio_update((vec3) {0}, 0);
}
static void run(unsigned from, unsigned to, unsigned stepms,
				struct input_device d, struct input_identity id, bool supported,
				unsigned percent) {
	for(unsigned ms = from; ms <= to; ms += stepms)
		frame(ms, d, id, supported, percent);
}
static const struct input_device_status* stat(struct input_device d) {
	return input_inventory_device(&inv, d);
}

static void begin(void) {
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));
	audio_init(true, 100);
	starts = 0;
}

static void test_one_cue_per_episode(void) {
	begin();
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	/* Connect, and let the connection cue land and clear. */
	run(0, 400, 1, remote, nunchuk, true, 90);
	unsigned base = starts;

	/* Drop to a low reading and hold it past the stable window. */
	run(500, 500 + INPUT_BATTERY_STABLE_MS, 10, remote, nunchuk, true, 12);
	assert(stat(remote)->battery_low);
	assert(starts == base + 1 && picked == SOUND_MENU_ATTENTION);
	/* The approved 65% gain, not the full attention level. */
	assert(last_gain == audio_ui_loudness(.65F, 1.0F));

	/* No periodic chirp: ten more seconds of the same low reading is silent,
	 * and no second card is raised either. */
	unsigned after = 500 + INPUT_BATTERY_STABLE_MS;
	run(after + 10, after + 10000, 10, remote, nunchuk, true, 12);
	assert(starts == base + 1);
	puts("PASS low battery: one attention cue at the approved 65% gain, then "
		 "no periodic chirp while the episode stands");
}

static void test_hysteresis_and_rearm(void) {
	begin();
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	run(0, 400, 1, remote, nunchuk, true, 90);
	unsigned base = starts;
	run(500, 500 + INPUT_BATTERY_STABLE_MS, 10, remote, nunchuk, true,
		INPUT_BATTERY_LOW_PERCENT);
	assert(starts == base + 1 && stat(remote)->battery_low);
	unsigned t = 500 + INPUT_BATTERY_STABLE_MS + 10;

	/* A reading between the two thresholds is NOT a recovery: the episode
	 * stands and, crucially, the cue stays re-armed-free. */
	run(t, t + 4 * INPUT_BATTERY_STABLE_MS, 10, remote, nunchuk, true,
		INPUT_BATTERY_RECOVER_PERCENT - 1);
	assert(stat(remote)->battery_low && starts == base + 1);
	t += 4 * INPUT_BATTERY_STABLE_MS + 10;

	/* Crossing the recovery threshold, held for the stable window, ends the
	 * episode -- silently, and with no card. */
	run(t, t + INPUT_BATTERY_STABLE_MS, 10, remote, nunchuk, true,
		INPUT_BATTERY_RECOVER_PERCENT);
	assert(!stat(remote)->battery_low && starts == base + 1);
	t += INPUT_BATTERY_STABLE_MS + 10;

	/* A SECOND genuine episode now sounds again, because the recovery
	 * re-armed the latch. */
	run(t, t + INPUT_BATTERY_STABLE_MS, 10, remote, nunchuk, true, 3);
	assert(stat(remote)->battery_low);
	assert(starts == base + 2 && picked == SOUND_MENU_ATTENTION);
	assert(!audio_menu_contract_violations());
	puts("PASS low battery: a reading inside the hysteresis band is not a "
		 "recovery; a stable recovery re-arms exactly one further episode");
}

static void test_replacement_rearms_and_wired_never_fires(void) {
	begin();
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	run(0, 400, 1, remote, nunchuk, true, 90);
	unsigned base = starts;
	run(500, 500 + INPUT_BATTERY_STABLE_MS, 10, remote, nunchuk, true, 5);
	assert(starts == base + 1);
	unsigned t = 500 + INPUT_BATTERY_STABLE_MS + 10;

	/* Swapping the controller out and back in is a replacement: the new pack
	 * gets its own episode. */
	run(t, t + 400, 1, remote, none, false, 0);
	t += 410;
	run(t, t + 400, 1, remote, nunchuk, true, 95);
	t += 410;
	base = starts;
	run(t, t + INPUT_BATTERY_STABLE_MS, 10, remote, nunchuk, true, 4);
	assert(starts == base + 1 && picked == SOUND_MENU_ATTENTION);

	/* A wired GameCube pad never produces one, at any reading, for any
	 * length of time -- the input layer refuses to call it supported, so the
	 * dispatcher never even sees a contract violation. */
	begin();
	run(0, 20000, 10, gc, (struct input_identity) {true, INPUT_CAT_GAMECUBE},
		false, 1);
	assert(!stat(gc)->battery_low && !stat(gc)->battery_known);
	assert(picked != SOUND_MENU_ATTENTION);
	assert(!audio_menu_contract_violations());
	puts("PASS low battery: a replacement re-arms the episode; a wired "
		 "GameCube pad never reports one");
}

static void test_latch_is_per_device(void) {
	begin();
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 0, remote));
	assert(input_owner_bind(owners, INPUT_LOCAL_PLAYERS, 1, other));
	for(unsigned ms = 0; ms <= 400; ms++) {
		frame(ms, remote, nunchuk, true, 90);
		frame(ms, other, nunchuk, true, 90);
	}
	unsigned base = starts;
	/* Both packs go low together. One device's latch must never swallow the
	 * other's episode. */
	for(unsigned ms = 500; ms <= 500 + INPUT_BATTERY_STABLE_MS; ms += 10) {
		frame(ms, remote, nunchuk, true, 8);
		frame(ms, other, nunchuk, true, 8);
	}
	assert(stat(remote)->battery_low && stat(other)->battery_low);
	assert(starts == base + 2);
	puts("PASS low battery: the episode latch is per device -- two low packs "
		 "produce two cues");
}

int main(void) {
	test_one_cue_per_episode();
	test_hysteresis_and_rearm();
	test_replacement_rearms_and_wired_never_fires();
	test_latch_is_per_device();
	return 0;
}
