/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	S0 audio: shared voice model + the platform dispatcher (the texture.c
	shared-code-then-#include pattern; gfx.c is the pure-dispatch sibling).

	THREADING CONTRACT: every function here runs on the CLIENT/main thread
	only — clin_process (CRPC_SOUND) and the screen handlers both live
	there. The backends own their off-thread halves: pc/audio.c locks its
	voice array against the miniaudio callback thread; wii/audio.c uses NO
	AESND callbacks at all (they run in audio-IRQ context — the
	finish_line_plan.md §2b zero-alloc rule is load-bearing) and is instead
	polled from audio_update via per-voice playback deadlines.

	VOICE STEALING: a free slot when one exists, else round-robin overwrite —
	the Beta channel model (SoundManager cycles source names 'sound_N',
	(counter+1)%256, and lets a new source displace the oldest).
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "audio_deferred.h" /* C2 P2: the opt-in deferred cave-ambience key */
#include "audio_music.h"
#include "menu_sound.h"
#include "audio_mix.h"
#include "audio_stream_ring.h" /* the Wii music backend's pure bookkeeping;
								   included HERE so wii/audio.c sees it
								   through this TU's #include at the bottom */
#include "time.h"

/* 16 of AESND's 32 hardware voices; the rest stay reserved for S3
   music/streaming. The PC mixer mirrors the same count so both platforms
   steal identically under load. */
#define AUDIO_VOICES 16

struct audio_voice {
	bool active;
	bool positional; /* false = playSoundFX-path voice: fixed gain, no pan */
	vec3 pos;
	float vol; /* the EVENT volume (range multiplier), NOT the gain */
};

static struct audio_voice s_voices[AUDIO_VOICES];
static bool s_backend_live = false;
static uint32_t s_audio_generation;
static struct audio_service_limits s_audio_limits; /* native E/margins UNKNOWN */
static struct audio_service_metrics s_audio_metrics;
static struct { bool active; unsigned voice; vec3 camera; float yaw; } s_audio_cursor;
static bool s_enabled = true;
static float s_master = 1.0F;
/* Beta's options.musicVolume (docs/finish_line_plan.md S3) -- 0.0F until
   audio_set_music_volume is called, so every existing s_master-only path
   (tests included) is byte-identical to before this field existed. */
static float s_music_master = 0.0F;
static vec3 s_cam_pos = {0.0F, 0.0F, 0.0F};
static float s_cam_yaw_rad = 0.0F;
static int s_voice_rr = 0;
static ptime_t s_menu_nav_deadline, s_menu_confirm_deadline;
static ptime_t s_menu_repeat_deadline, s_menu_post_confirm_deadline;
static bool s_menu_nav_guard, s_menu_confirm_guard, s_menu_repeat_guard;
static int s_menu_voice = -1;
static int s_achievement_voice = -1;
static bool s_menu_voice_committed;
static bool s_menu_pending;
static enum menu_sound_event s_menu_pending_event;
static ptime_t s_menu_pending_now;
static struct input_device s_menu_pending_device;

/* PER-DEVICE controller-notice state. struct input_device is {kind, channel}
 * with kind in [INPUT_DEVICE_NONE, INPUT_DEVICE_GAMECUBE] and channel < 4
 * (input_owner_bind enforces that range), so the pair packs into a dense
 * slot. Slot 0..3 (kind == INPUT_DEVICE_NONE) is the "unidentified device"
 * bucket a contract violation folds into -- never a real controller, so it
 * can never collapse or latch against one. */
#define MENU_DEVICE_KINDS 4
#define MENU_DEVICE_CHANNELS 4
#define MENU_DEVICE_SLOTS (MENU_DEVICE_KINDS * MENU_DEVICE_CHANNELS)
enum { MENU_NOTICE_CONNECTED = 0, MENU_NOTICE_ATTENTION, MENU_NOTICE_KINDS };
/* The once-per-episode low-battery latch, PER DEVICE
 * (audio_menu_battery_rearm clears one device's). */
static bool s_menu_battery_latched[MENU_DEVICE_SLOTS];
/* The same-device/same-cue collapse window, PER DEVICE. */
static bool s_menu_notice_guard[MENU_DEVICE_SLOTS][MENU_NOTICE_KINDS];
static ptime_t s_menu_notice_deadline[MENU_DEVICE_SLOTS][MENU_NOTICE_KINDS];
static unsigned s_menu_notices_collapsed, s_menu_contract_violations;

/* Backend hooks, implemented by the #included platform file at the bottom
   of this TU. All main-thread. */
static bool audio_backend_init(void);
static void audio_backend_shutdown(void);
static void audio_backend_voice_start(int slot, const struct audio_sample* smp,
									  float rate_hz, float l, float r);
static void audio_backend_voice_gain(int slot, float l, float r);
static bool audio_backend_voice_playing(int slot);
static void audio_backend_stop_all(void);
/* The ONE dedicated S3 music voice -- same #included-platform-file pattern,
   a distinct group from the AUDIO_VOICES pool above (audio.h's
   audio_voice_music_* group dispatches to these). */
static void audio_backend_music_start(const int16_t* pcm, uint32_t frames,
									  float rate_hz, float gain);
static void audio_backend_music_gain(float gain);
static bool audio_backend_music_playing(void);
static bool audio_backend_music_drained(void);
static void audio_backend_music_stop(void);
static uint32_t audio_backend_music_native_rate(void);
static uint32_t audio_backend_music_reserve_us(void);

/* Beta's `loaded` gate is `soundVolume!=0 || musicVolume!=0`
   [SoundManager.loadSoundSettings] -- the shared device stays open as long
   as EITHER volume justifies it. */
static bool audio_backend_wanted(void) {
	return s_enabled && (s_master > 0.0F || s_music_master > 0.0F);
}

void audio_init(bool enabled, int volume0_100) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	s_achievement_voice = -1;
	s_menu_nav_guard = s_menu_confirm_guard = s_menu_repeat_guard = false;
	s_menu_voice = -1;
	s_menu_voice_committed = false;
	s_menu_pending = false;
	/* A fresh session starts every device's battery notice armed and every
	 * per-device collapse window closed. */
	memset(s_menu_battery_latched, 0, sizeof(s_menu_battery_latched));
	memset(s_menu_notice_guard, 0, sizeof(s_menu_notice_guard));
	s_menu_notices_collapsed = s_menu_contract_violations = 0;
	if(volume0_100 < 0)
		volume0_100 = 0;
	if(volume0_100 > 100)
		volume0_100 = 100;
	s_master = (float)volume0_100 / 100.0F;
	s_enabled = enabled;

	memset(s_voices, 0, sizeof(s_voices));
	s_voice_rr = 0;

	/* The registry (and its arena) loads even when disabled: HEADLESS mode
	   for the test suite (mapping/parse checks with no device) and the F3
	   arena dial stay functional. Playback below stays fully off. */
	audio_registry_load();

	/* Beta opens the audio library lazily and only when the volume option
	   is nonzero [SoundManager.loadSoundSettings/tryToSetLibraryAndCodecs];
	   mirror that: a disabled or muted session never touches the device. */
	s_backend_live = false;
	if(enabled && s_master > 0.0F) {
		s_backend_live = audio_backend_init();
		if(!s_backend_live)
			fprintf(stderr,
					"[audio] backend init failed; continuing silent\n");
	}
}

void audio_shutdown(void) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	s_achievement_voice = -1;
	s_menu_pending = false;
	s_menu_voice = -1;
	s_menu_voice_committed = false;
	/* C2 P2: stop + join the deferred loader before the device goes away (a
	   no-op when the opt-in never brought it up). */
	audio_deferred_shutdown();
	if(s_backend_live) {
		audio_backend_shutdown();
		s_backend_live = false;
	}
}

int audio_voices_active(void) {
	int n = 0;
	for(int k = 0; k < AUDIO_VOICES; k++)
		if(s_voices[k].active)
			n++;
	return n;
}

void audio_stop_all(void) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	s_achievement_voice = -1;
	s_menu_pending = false;
	s_menu_voice = -1;
	s_menu_voice_committed = false;
	/* The audio_registry_reload precondition (main thread only): the backend
	   stop runs FIRST — PC zeroes its voice array under the mixer lock, Wii
	   asserts SetVoiceStop + clears every poll deadline — then the engine
	   slots are cleared, so no path can hand a stale arena pointer to the
	   device after this returns. Headless/no-device: nothing to stop. */
	if(s_backend_live)
		audio_backend_stop_all();
	/* C2 P2: the deferred voice (if any) just let go of the scratch, and a
	   load still on its way must never start a voice after a stop-all. */
	int deferred_slot = audio_deferred_voice_slot();
	if(deferred_slot >= 0)
		audio_deferred_note_voice_released(deferred_slot,
										   AUDIO_DEFERRED_OUT_CANCELLED);
	audio_deferred_cancel();
	memset(s_voices, 0, sizeof(s_voices));
	s_voice_rr = 0;
}

bool audio_set_master_volume(int volume0_100) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	if(volume0_100 < 0)
		volume0_100 = 0;
	if(volume0_100 > 100)
		volume0_100 = 100;
	float requested = (float)volume0_100 / 100.0F;

	/* audio.enabled remains the outer gate. Changing the persisted volume
	 * while that gate is off must not unexpectedly open a device. */
	if(!s_enabled) {
		s_master = requested;
		return true;
	}

	if(requested <= 0.0F) {
		audio_stop_all();
		s_master = 0.0F;
		/* Only close the shared device if MUSIC is not also keeping it open
		   (Beta's `soundVolume!=0 || musicVolume!=0` OR-gate). */
		if(s_backend_live && !audio_backend_wanted()) {
			audio_backend_shutdown();
			s_backend_live = false;
		}
		return true;
	}

	if(!s_backend_live) {
		if(!audio_backend_init())
			return false;
		s_backend_live = true;
	}
	s_master = requested;
	if(s_achievement_voice >= 0 && s_voices[s_achievement_voice].active) {
		float gain = audio_ui_loudness(s_voices[s_achievement_voice].vol,
									 s_master);
		audio_backend_voice_gain(s_achievement_voice, gain, gain);
	}
	/* Menu sounds follow a live slider change too, including their tail. */
	if(s_menu_voice >= 0 && s_voices[s_menu_voice].active) {
		float gain = audio_ui_loudness(s_voices[s_menu_voice].vol, s_master);
		audio_backend_voice_gain(s_menu_voice, gain, gain);
	}
	return true;
}

bool audio_set_music_volume(int volume0_100) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	if(volume0_100 < 0)
		volume0_100 = 0;
	if(volume0_100 > 100)
		volume0_100 = 100;
	float requested = (float)volume0_100 / 100.0F;

	/* audio.enabled remains the outer gate, mirroring audio_set_master_volume. */
	if(!s_enabled) {
		s_music_master = requested;
		return true;
	}

	if(requested <= 0.0F) {
		/* onSoundOptionsChanged: musicVolume==0 -> sndSystem.stop("BgMusic").
		   Stop only the music voice -- the one-shot pool is untouched. */
		if(s_backend_live)
			audio_backend_music_stop();
		s_music_master = 0.0F;
		if(s_backend_live && !audio_backend_wanted()) {
			audio_backend_shutdown();
			s_backend_live = false;
		}
		return true;
	}

	if(!s_backend_live) {
		if(!audio_backend_init())
			return false;
		s_backend_live = true;
	}
	s_music_master = requested;
	if(s_backend_live)
		audio_backend_music_gain(s_music_master);
	return true;
}

bool audio_music_backend_ready(void) {
	/* `s_enabled` (audio_init's `enabled` -- a CaveWii headless/dev switch
	   with no Beta equivalent) is the closest analog to Beta's STICKY
	   `loaded` flag [SoundManager: set true once and never cleared again].
	   Deliberately NOT `s_backend_live`: unlike `loaded`, this engine's
	   device is allowed to CLOSE again when both volumes drop to 0 (a
	   resource-conservation deviation invisible to the timer's own
	   faithfulness -- the device reopens the instant music.volume goes
	   positive again, audio_set_music_volume). Gating on the live-open
	   flag here would make the timer HEADLESS-untestable (test builds run
	   audio_init(false, ...) with no device on purpose, audio_registry.c's
	   own precedent) without changing anything about what is actually
	   audible: audio_voice_music_start already independently no-ops
	   whenever the device is not open. */
	return s_enabled && s_music_master > 0.0F;
}

/* Pacing-bug probe counters (2026-09-01, additive telemetry): the far_idle
 * music run showed playing() answering false every frame (music_ms racing
 * 5.5x realtime, 6628 underruns). starts vs rejects names WHICH gate lies:
 * rejects climbing = a start guard refuses; starts climbing ~60/s = the
 * deadline math expires instantly; starts ~5/s with racing ms = the ring
 * advances without a hardware start at all. */
static uint32_t s_music_probe_starts;
static uint32_t s_music_probe_rejects;

uint32_t audio_music_probe_starts(void) { return s_music_probe_starts; }
uint32_t audio_music_probe_rejects(void) { return s_music_probe_rejects; }

void audio_voice_music_start(const int16_t* pcm, uint32_t frames,
							 float rate_hz) {
	if(!s_backend_live || s_music_master <= 0.0F) {
		s_music_probe_rejects++;
		return;
	}
	s_music_probe_starts++;
	audio_backend_music_start(pcm, frames, rate_hz, s_music_master);
}

bool audio_voice_music_playing(void) {
	if(!s_backend_live)
		return false;
	return audio_backend_music_playing();
}

bool audio_voice_music_drained(void) {
	/* No live device: nothing is queued, so everything handed over is
	   trivially "played". This is what keeps the HEADLESS test path
	   (audio_init(false, ...)) behaving exactly as it did before the
	   drained gate existed -- audio_music.c's EOF branch stops
	   immediately there, with no device to wait on. */
	if(!s_backend_live)
		return true;
	return audio_backend_music_drained();
}

void audio_voice_music_stop(void) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	if(!s_backend_live)
		return;
	audio_backend_music_stop();
}

uint32_t audio_voice_music_native_rate(void) {
	/* Deliberately NOT gated on s_backend_live: this is a PLATFORM constant
	   (the DSP's fixed output rate), not device state, and the decode
	   worker asks for it while opening a file -- possibly before the device
	   has been opened, and from its own thread. */
	return audio_backend_music_native_rate();
}

static int audio_take_slot(void) {
	for(int k = 0; k < AUDIO_VOICES; k++)
		if(!s_voices[k].active) {
			if(s_achievement_voice == k)
				s_achievement_voice = -1;
			if(s_menu_voice == k) {
				s_menu_voice = -1;
				s_menu_voice_committed = false;
			}
			return k;
		}
	/* Pool full: round-robin steal (the SoundManager 'sound_N' cycle). */
	int k = s_voice_rr;
	if(s_achievement_voice == k)
		s_achievement_voice = -1;
	/* C2 P2: a stolen deferred voice lets go of the scratch (no-op otherwise). */
	audio_deferred_note_voice_released(k, AUDIO_DEFERRED_OUT_STOLEN);
	if(s_menu_voice == k) {
		s_menu_voice = -1;
		s_menu_voice_committed = false;
	}
	s_voice_rr = (s_voice_rr + 1) % AUDIO_VOICES;
	return k;
}

static int audio_start_positional(const struct audio_sample* smp, vec3 pos,
								  float vol, float pitch);
static void audio_play3d_sample(uint16_t sound_id,
								const struct audio_sample* smp, vec3 pos,
								float vol, float pitch);

void audio_play3d(uint16_t sound_id, vec3 pos, float vol, float pitch) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	/* SoundManager.playSound no-ops on a null entry or volume <= 0; every
	   play method no-ops when the library is closed / master is 0. */
	if(!s_backend_live || s_master <= 0.0F || vol <= 0.0F
	   || sound_id >= AUDIO_SOUND_COUNT)
		return;
	audio_play3d_sample(sound_id, audio_registry_pick(sound_id), pos, vol,
						pitch);
}

void audio_debug_play_variant(uint16_t sound_id, int n, vec3 pos, float vol,
							  float pitch) {
	/* The C2 P2 evidence lever's entry (client_display_tick.c): the SAME play
	   path as audio_play3d with the variant chosen by index instead of by the
	   cosmetic RNG -- no draw, so an armed lever cannot shift the pick stream. */
	if(!s_backend_live || s_master <= 0.0F || vol <= 0.0F
	   || sound_id >= AUDIO_SOUND_COUNT)
		return;
	int count = audio_registry_variant_count(sound_id);
	if(count <= 0 || n < 0)
		return;
	audio_play3d_sample(sound_id, audio_registry_variant(sound_id, n % count),
						pos, vol, pitch);
}

/* Everything audio_play3d does AFTER the variant is chosen. */
static void audio_play3d_sample(uint16_t sound_id,
								const struct audio_sample* smp, vec3 pos,
								float vol, float pitch) {
	if(!smp)
		return;

	float dx = pos[0] - s_cam_pos[0];
	float dy = pos[1] - s_cam_pos[1];
	float dz = pos[2] - s_cam_pos[2];
	float dist = sqrtf(dx * dx + dy * dy + dz * dz);
	/* The RenderGlobal.playSound distance cull at 16*max(1,vol). */
	if(dist >= audio_range(vol))
		return;

	if(!smp->pcm) {
		/* C2 P2 (opt-in): a DEFERRED variant. The pick above and the cull
		   above are Beta's two decisions, made at the trigger exactly as for
		   a resident sample; only the voice start moves, to audio_update,
		   once the loader has the whole sample resident. A refused request
		   (one already in flight) is accounted for by the module. */
		int variant = -1;
		const struct audio_deferred_source* src
			= audio_registry_deferred_source(sound_id, smp, &variant);
		if(src)
			audio_deferred_request(src, sound_id, variant, pos, vol, pitch);
		return;
	}

	int slot = audio_start_positional(smp, pos, vol, pitch);
	/* Diagnostics only (audio.h): the ONE point a positional play really
	   becomes a started voice -- everything above this line can still return
	   without one, the distance cull most of all. */
	audio_registry_note_voice_started(slot, smp);
}

/* The voice-start tail of audio_play3d, shared with the deferred start so the
   two cannot drift: gain/pan from the listener NOW, slot, unclamped pitch. */
static int audio_start_positional(const struct audio_sample* smp, vec3 pos,
								  float vol, float pitch) {
	float dx = pos[0] - s_cam_pos[0];
	float dy = pos[1] - s_cam_pos[1];
	float dz = pos[2] - s_cam_pos[2];
	float dist = sqrtf(dx * dx + dy * dy + dz * dz);
	float gain = audio_loudness(vol, s_master) * audio_distance_gain(dist, vol);
	float l, r;
	audio_pan_gains(audio_pan(dx, dz, s_cam_yaw_rad), gain, &l, &r);

	int slot = audio_take_slot();
	s_voices[slot].active = true;
	s_voices[slot].positional = true;
	glm_vec3_copy(pos, s_voices[slot].pos);
	s_voices[slot].vol = vol;
	/* Pitch UNCLAMPED -> playback rate = native * pitch [SoundManager]. */
	audio_backend_voice_start(slot, smp,
							  audio_playback_rate((float)smp->rate, pitch), l,
							  r);
	return slot;
}

/* C2 P2: per-frame half of the deferred one-shot. Nothing here runs unless
   the opt-in scan brought the module up (take_ready is then one mutex). */
static void audio_deferred_pump(void) {
	struct audio_deferred_ready rdy;
	if(audio_deferred_take_ready(&rdy)) {
		/* Recheck at the DELAYED start what audio_play3d checked at the
		   trigger (Astra D3): the player may have muted, or the device died,
		   while the sample loaded. No second distance cull -- the sound
		   exists since the trigger, and its gain follows the listener from
		   here exactly like any playing positional voice. */
		if(!s_backend_live || s_master <= 0.0F) {
			audio_deferred_note_not_started(rdy.req_id,
											AUDIO_DEFERRED_OUT_CULLED);
		} else {
			struct audio_sample smp = {rdy.pcm, rdy.frames, rdy.rate};
			vec3 pos = {rdy.pos[0], rdy.pos[1], rdy.pos[2]};
			int slot = audio_start_positional(&smp, pos, rdy.vol, rdy.pitch);
			audio_deferred_note_started(rdy.req_id, slot);
			audio_registry_note_deferred_started(slot, rdy.sound_id,
												 rdy.variant, &smp);
		}
	}
	audio_registry_deferred_telemetry();
}

void audio_cancel_deferred(void) {
	audio_deferred_cancel();
}

static int audio_start_ui(uint16_t sound_id, float vol, float pitch, int slot) {
	/* The playSoundFX parity path: no position, no attenuation, no pan,
	   loudness min(vol,1) * 0.25F * master [SoundManager.playSoundFX]. */
	if(!s_backend_live || s_master <= 0.0F || vol <= 0.0F
	   || sound_id >= AUDIO_SOUND_COUNT)
		return -1;
	const struct audio_sample* smp = audio_registry_pick(sound_id);
	/* !pcm: a DEFERRED variant (C2 P2) has no resident PCM and no FX-path
	   loader -- only ambient.cave.cave is deferred, and it is never a UI key. */
	if(!smp || !smp->pcm)
		return -1;

	float gain = audio_ui_loudness(vol, s_master);
	if(slot < 0) slot = audio_take_slot();
	s_voices[slot].active = true;
	s_voices[slot].positional = false;
	glm_vec3_zero(s_voices[slot].pos);
	s_voices[slot].vol = vol;
	audio_backend_voice_start(slot, smp,
							  audio_playback_rate((float)smp->rate, pitch),
							  gain, gain);
	audio_registry_note_voice_started(slot, smp); /* diagnostics, audio.h */
	return slot;
}

void audio_play_ui(uint16_t sound_id, float vol, float pitch) {
	s_audio_generation++;
	s_audio_cursor.active = false;
	(void)audio_start_ui(sound_id, vol, pitch, -1);
}

void audio_achievement_bell(void) {
	if(s_achievement_voice >= 0 && s_voices[s_achievement_voice].active
	   && audio_backend_voice_playing(s_achievement_voice))
		return; /* both split owners share this voice; never restart it */
	s_achievement_voice
		= audio_start_ui(SOUND_MENU_ACHIEVEMENT, 1.0F, 1.0F, -1);
}

/* The approved same-frame arbitration order, highest first: "newly blocking
 * controller attention, committed action, other controller notice, menu
 * transition, then navigation." MENU_PRIO_NONE rejects an out-of-range event. */
enum {
	MENU_PRIO_NONE = 0,
	MENU_PRIO_NAV,		  /* navigation */
	MENU_PRIO_TRANSITION, /* menu transition */
	MENU_PRIO_NOTICE,	  /* other controller notice */
	MENU_PRIO_COMMITTED,  /* committed action */
	MENU_PRIO_BLOCKING	  /* newly blocking controller attention */
};

static unsigned audio_menu_priority(enum menu_sound_event event) {
	switch(event) {
		case MENU_SOUND_ATTENTION: return MENU_PRIO_BLOCKING;
		/* BACK'S PRIORITY CLASS, decided from the spec text: COMMITTED.
		 * Every Back trigger the action map lists is a COMPLETED operation,
		 * not a selection change -- "Back on successful return/resume",
		 * "Unchanged Done and clean Cancel use Back", "Confirmed discard
		 * closes with Back", "deliberate successful Resume uses Back". The
		 * arbitration rule then places it above navigation and transition:
		 * "One action emits one sound: an open/transfer/type/confirm cue
		 * replaces incidental focus sounds caused by that action."
		 * Its RATE class is a separate axis and stays navigation: the spec
		 * names only two rate limits for menu movement -- "Retain the
		 * approved global 90 ms navigation limit and 60 ms confirmation
		 * guard" -- and the 60 ms one guards the CONFIRMATION cue
		 * (menu.select, the "successful-action confirmation"), which the spec
		 * explicitly distinguishes from Back ("Confirmed discard closes with
		 * Back, never the successful-save cue"). Back is the return half of
		 * menu movement, so it takes the 90 ms navigation limit and the
		 * "120 ms post-confirmation navigation suppression" with it.
		 * The two axes disagreeing is safe now only because guards run
		 * BEFORE arbitration (audio_menu_submit): a Back that its navigation
		 * window rejects never becomes the frame's winner, so it can no
		 * longer silence a same-frame connection cue. */
		case MENU_SOUND_SELECT:
		case MENU_SOUND_BACK:
		case MENU_SOUND_ITEM:
		case MENU_SOUND_TYPE: return MENU_PRIO_COMMITTED;
		case MENU_SOUND_CONNECTED:
		case MENU_SOUND_BATTERY_ATTENTION: return MENU_PRIO_NOTICE;
		case MENU_SOUND_SECTION: return MENU_PRIO_TRANSITION;
		case MENU_SOUND_FOCUS: return MENU_PRIO_NAV;
	}
	return MENU_PRIO_NONE;
}

/* Slot 0 == {INPUT_DEVICE_NONE, 0}: the "unidentified device" bucket. */
static int audio_menu_device_slot(struct input_device device) {
	if((int)device.kind < INPUT_DEVICE_NONE
	   || (int)device.kind > INPUT_DEVICE_GAMECUBE
	   || device.channel >= (unsigned)MENU_DEVICE_CHANNELS)
		return -1;
	return (int)device.kind * MENU_DEVICE_CHANNELS + (int)device.channel;
}

static int audio_menu_notice_kind(enum menu_sound_event event) {
	if(event == MENU_SOUND_CONNECTED) return MENU_NOTICE_CONNECTED;
	if(event == MENU_SOUND_ATTENTION) return MENU_NOTICE_ATTENTION;
	return -1; /* BATTERY_ATTENTION uses the per-device latch instead. */
}

unsigned audio_menu_notices_collapsed(void) { return s_menu_notices_collapsed; }
unsigned audio_menu_contract_violations(void) {
	return s_menu_contract_violations;
}

void audio_menu_battery_rearm(struct input_device device) {
	int slot = audio_menu_device_slot(device);
	/* An unknown device re-arms the bucket its cue was folded into, so the
	 * release-safe fallback path stays symmetric. */
	if(slot < 0) {
		s_menu_contract_violations++;
		slot = 0;
	}
	s_menu_battery_latched[slot] = false;
}

/* Every guard the spec puts on a single cue, evaluated for ONE candidate.
 * Called BEFORE arbitration (see menu_sound.h): a rejected candidate is
 * discarded outright and never becomes the frame's winner, so it cannot
 * silence a surviving lower-priority cue. Nothing here mutates dispatcher
 * state -- guard deadlines and latches are only armed when a cue actually
 * plays, in audio_menu_flush -- so the answer is stable for the whole frame
 * and asking at submit time is equivalent to asking at flush time. */
static bool audio_menu_guard_rejects(enum menu_sound_event event, int slot,
									 ptime_t now) {
	bool confirm = event == MENU_SOUND_SELECT;
	bool nav = event == MENU_SOUND_FOCUS || event == MENU_SOUND_SECTION
		|| event == MENU_SOUND_BACK;
	bool repeat = event == MENU_SOUND_ITEM || event == MENU_SOUND_TYPE;
	/* "90 ms navigation limit" (shared globally, including split-screen). */
	if(nav && s_menu_nav_guard && !time_reached(now, s_menu_nav_deadline))
		return true;
	/* "60 ms confirmation guard". */
	if(confirm && s_menu_confirm_guard
	   && !time_reached(now, s_menu_confirm_deadline)) return true;
	/* "Typing/item sounds also have a shared 90 ms repeat guard". */
	if(repeat && s_menu_repeat_guard
	   && !time_reached(now, s_menu_repeat_deadline)) return true;
	/* "120 ms post-confirmation navigation suppression". */
	if(nav && s_menu_confirm_guard
	   && !time_reached(now, s_menu_post_confirm_deadline)) return true;
	/* "Do not interrupt a committed-action sound for a low-battery notice"
	 * (the visual notice still appears -- that is the GUI's job, not ours). */
	if(event == MENU_SOUND_BATTERY_ATTENTION && s_menu_voice >= 0
	   && s_voices[s_menu_voice].active && s_menu_voice_committed
	   && audio_backend_voice_playing(s_menu_voice)) return true;
	/* "Battery low: attention at 65% of its normal cue gain, ONCE per
	 * low-battery episode, rearmed only after a stable recovery/replacement.
	 * No periodic chirp". PER DEVICE: one Wiimote's chirp must not swallow
	 * another's, and one device's recovery must not re-arm another's. The
	 * device layer owns the recovery edge and calls audio_menu_battery_rearm;
	 * this latch is the fail-safe against a caller that polls that device's
	 * battery instead of edge-triggering it. */
	if(event == MENU_SOUND_BATTERY_ATTENTION && s_menu_battery_latched[slot])
		return true;
	/* "Collapse discovery and player assignment in the same connection flow
	 * into one cue" and "Do not repeat while waiting or retrying unchanged
	 * state." Both are per-DEVICE rules, so they are enforced per device slot
	 * and per cue: a repeat of the SAME cue for the SAME device inside the
	 * collapse window is dropped and counted. A DIFFERENT device is never
	 * touched by this window -- two Wiimotes syncing at once, or a second
	 * controller lost 1 ms after the first, both stay audible -- and a cue of
	 * a different kind (a reconnect after a loss) is a real state change, so
	 * it is not collapsed either. */
	int notice = audio_menu_notice_kind(event);
	if(notice >= 0 && s_menu_notice_guard[slot][notice]
	   && !time_reached(now, s_menu_notice_deadline[slot][notice])) {
		s_menu_notices_collapsed++;
		return true;
	}
	return false;
}

static void audio_menu_submit(enum menu_sound_event event, int slot,
							  ptime_t now) {
	unsigned priority = audio_menu_priority(event);
	if(priority == MENU_PRIO_NONE) return;
	/* GUARDS FIRST, then arbitration. A guard-rejected candidate is discarded
	 * here and never displaces a survivor, so the frame falls through to the
	 * highest-priority cue that CAN play. */
	if(audio_menu_guard_rejects(event, slot, now)) return;
	/* At most one cue per frame among the survivors. A later higher-priority
	 * event replaces the candidate; equal/lower events are discarded, never
	 * queued ("skipped sounds are discarded, never queued"). */
	if(s_menu_pending
	   && priority <= audio_menu_priority(s_menu_pending_event)) return;
	s_menu_pending = true;
	s_menu_pending_event = event;
	s_menu_pending_now = now;
	s_menu_pending_device.kind
		= (enum input_device_kind)(slot / MENU_DEVICE_CHANNELS);
	s_menu_pending_device.channel = (unsigned)(slot % MENU_DEVICE_CHANNELS);
}

void audio_menu_event(enum menu_sound_event event, ptime_t now) {
	/* Menu cues only; the controller three carry a device identity. */
	assert(!menu_sound_is_device_event(event));
	int slot = 0; /* the unidentified-device bucket */
	if(menu_sound_is_device_event(event)) {
		/* Release-safe half of the assert: count it, still play it. */
		s_menu_contract_violations++;
	}
	audio_menu_submit(event, slot, now);
}

void audio_menu_device_event(enum menu_sound_event event,
							 struct input_device device, ptime_t now) {
	assert(menu_sound_is_device_event(event));
	int slot = audio_menu_device_slot(device);
	if(!menu_sound_is_device_event(event) || slot < 0) {
		s_menu_contract_violations++;
		if(slot < 0) slot = 0;
	}
	/* "Battery low: ... never for a wired GameCube controller." A wired pad
	 * has no battery to report; with the device named, the dispatcher can
	 * decide this itself instead of trusting the caller. Counted, not
	 * asserted, so the rule holds identically in a release build. */
	if(event == MENU_SOUND_BATTERY_ATTENTION
	   && device.kind == INPUT_DEVICE_GAMECUBE) {
		s_menu_contract_violations++;
		return;
	}
	audio_menu_submit(event, slot, now);
}

static void audio_menu_flush(void) {
	if(!s_menu_pending) return;
	enum menu_sound_event event = s_menu_pending_event;
	ptime_t now = s_menu_pending_now;
	int slot = audio_menu_device_slot(s_menu_pending_device);
	s_menu_pending = false;
	if(slot < 0) slot = 0;

	bool confirm = event == MENU_SOUND_SELECT;
	bool nav = event == MENU_SOUND_FOCUS || event == MENU_SOUND_SECTION
		|| event == MENU_SOUND_BACK;
	bool repeat = event == MENU_SOUND_ITEM || event == MENU_SOUND_TYPE;

	uint16_t id;
	switch(event) {
		case MENU_SOUND_FOCUS: id = SOUND_MENU_SCROLL; break;
		case MENU_SOUND_SECTION: id = SOUND_MENU_SECTION; break;
		case MENU_SOUND_SELECT: id = SOUND_MENU_SELECT; break;
		case MENU_SOUND_BACK: id = SOUND_MENU_BACK; break;
		case MENU_SOUND_ITEM: id = SOUND_MENU_ITEM; break;
		case MENU_SOUND_TYPE: id = SOUND_MENU_TYPE; break;
		case MENU_SOUND_CONNECTED: id = SOUND_MENU_CONNECTED; break;
		case MENU_SOUND_ATTENTION:
		case MENU_SOUND_BATTERY_ATTENTION: id = SOUND_MENU_ATTENTION; break;
		default: return;
	}
	/* Vanilla reference: 740c583 GuiButton.mousePressed -> GuiScreen.mouseClicked
	 * -> SoundManager.playSoundFX("random.click", 1.0F, 1.0F). Approved original
	 * softer samples replace the asset, retaining this dispatcher's mute/gain.
	 * Reuse one menu voice; missing/failed samples never trigger a fallback. */
	float vol = event == MENU_SOUND_BATTERY_ATTENTION ? 0.65F : 1.0F;
	int voice = audio_start_ui(id, vol, 1.0F, s_menu_voice);
	if(voice < 0) return;
	s_menu_voice = voice;
	s_menu_voice_committed = audio_menu_priority(event) == MENU_PRIO_COMMITTED;
	if(event == MENU_SOUND_BATTERY_ATTENTION)
		/* one cue per episode for THIS device, until its own rearm */
		s_menu_battery_latched[slot] = true;
	int notice = audio_menu_notice_kind(event);
	if(notice >= 0) {
		/* Arm this device's collapse window for this cue only. */
		s_menu_notice_guard[slot][notice] = true;
		s_menu_notice_deadline[slot][notice]
			= time_add_ms(now, MENU_SOUND_NOTICE_COLLAPSE_MS);
	}
	if(confirm) {
		s_menu_confirm_guard = true;
		s_menu_confirm_deadline = time_add_ms(now, 60);
		s_menu_post_confirm_deadline = time_add_ms(now, 120);
	} else if(nav) {
		s_menu_nav_guard = true;
		s_menu_nav_deadline = time_add_ms(now, 90);
	} else if(repeat) {
		s_menu_repeat_guard = true;
		s_menu_repeat_deadline = time_add_ms(now, 90);
	}
}

/* One expression sequence shared by OFF and the finite per-voice cursor. */
static void audio_update_voice_gain(unsigned k) {
	float dx = s_voices[k].pos[0] - s_cam_pos[0];
	float dy = s_voices[k].pos[1] - s_cam_pos[1];
	float dz = s_voices[k].pos[2] - s_cam_pos[2];
	float dist = sqrtf(dx * dx + dy * dy + dz * dz);
	float gain = audio_loudness(s_voices[k].vol, s_master)
		* audio_distance_gain(dist, s_voices[k].vol);
	float l, r;
	audio_pan_gains(audio_pan(dx, dz, s_cam_yaw_rad), gain, &l, &r);
	audio_backend_voice_gain(k, l, r);
}

void audio_update(vec3 cam_pos, float cam_yaw_deg) {
	if(s_audio_cursor.active && !s_menu_pending) {
		/* The immediate ordinary escape finishes the same listener/voice
		 * pass. Re-running completed voice setters would duplicate units. */
		struct audio_service_budget b;
		audio_service_get_budget(&b, 500);
		b.ordinary = true;
		(void)audio_update_service(s_audio_cursor.camera, s_audio_cursor.yaw, &b);
		return;
	}
	s_audio_generation++;
	s_audio_cursor.active = false;
	/* Resolve all menu events submitted during this frame before polling. */
	audio_menu_flush();
	glm_vec3_copy(cam_pos, s_cam_pos);
	s_cam_yaw_rad = glm_rad(cam_yaw_deg);

	/* C2 P2: before the liveness gate, so a load that finishes after the
	   device died is reconciled (CULLED) instead of waiting forever. */
	audio_deferred_pump();

	if(!s_backend_live)
		return;

	for(int k = 0; k < AUDIO_VOICES; k++) {
		if(!s_voices[k].active)
			continue;
		if(!audio_backend_voice_playing(k)) {
			if(s_achievement_voice == k)
				s_achievement_voice = -1;
			s_voices[k].active = false; /* reaped by poll, never a callback */
			audio_deferred_note_voice_released(k,
											   AUDIO_DEFERRED_OUT_COMPLETED);
			if(s_menu_voice == k) {
				s_menu_voice = -1;
				s_menu_voice_committed = false;
			}
			continue;
		}
		if(!s_voices[k].positional)
			continue; /* FX-path voices keep their fixed quarter-volume */

		/* The per-frame recompute (scout DECISIONS: mono + camera-relative
		   attenuation/pan, two multiplies per voice). */
		audio_update_voice_gain((unsigned)k);
	}
}

#ifdef PLATFORM_WII
#include "wii/audio.c"
#endif

#ifdef PLATFORM_PC
#include "pc/audio.c"
#endif

/* Pacing-bug probe (2026-09-01): defined after the platform include so the
 * backend getter is in scope on both legs. */
uint32_t audio_music_probe_dur(void) {
	return audio_backend_music_last_dur_ms();
}

uint32_t audio_voice_music_reserve_us(void) {
	return audio_backend_music_reserve_us();
}

void audio_service_get_budget(struct audio_service_budget* b, uint32_t cap_us) {
	*b = (struct audio_service_budget){.start = time_get(), .cap_us = cap_us,
		.limits = &s_audio_limits, .metrics = &s_audio_metrics};
}
void audio_service_get_metrics(struct audio_service_metrics* metrics) {
	*metrics = s_audio_metrics;
}

enum audio_service_result audio_update_service(vec3 cam_pos, float yaw,
	struct audio_service_budget* b) {
	if(s_menu_pending) return AUDIO_SERVICE_UNSAFE; /* cue/registry work stays ordinary */
	enum audio_service_result r = audio_service_enter(b, AUDIO_UNIT_STATE);
	if(r != AUDIO_SERVICE_DONE) return r;
	ptime_t entered = time_get();
	/* An existing pass proved IDLE before starting. Only MAIN can submit a
	 * new deferred request; immediate ordinary escape retains that proof. */
	bool idle = b->ordinary && s_audio_cursor.active ? true : audio_deferred_service_idle();
	audio_service_cover(b, AUDIO_UNIT_STATE, 0, idle ? 2u : 4u);
	if(!audio_service_leave(b, AUDIO_UNIT_STATE, entered) || !idle)
		return AUDIO_SERVICE_UNSAFE;
#ifdef PLATFORM_PC
	/* miniaudio takes a blocking device lock. Only HEADLESS qualifies. */
	if(s_backend_live) return AUDIO_SERVICE_UNSAFE;
#endif
	if(!s_audio_cursor.active) {
		glm_vec3_copy(cam_pos, s_audio_cursor.camera);
		s_audio_cursor.yaw = yaw;
		s_audio_cursor.voice = 0;
		s_audio_cursor.active = true;
		glm_vec3_copy(cam_pos, s_cam_pos);
		s_cam_yaw_rad = glm_rad(yaw);
	}
	while(s_audio_cursor.voice < AUDIO_VOICES && s_backend_live) {
		unsigned next = s_audio_cursor.voice;
		if(!b->ordinary && s_voices[next].active && s_voices[next].positional
			&& !audio_service_voice_math_finite(s_cam_yaw_rad)) return AUDIO_SERVICE_UNSAFE;
		r = audio_service_enter(b, AUDIO_UNIT_VOICE);
		if(r != AUDIO_SERVICE_DONE) return r;
		entered = time_get();
		unsigned k = s_audio_cursor.voice++;
		audio_service_cover(b, AUDIO_UNIT_VOICE, 0, s_voices[k].active ? 1u : 2u);
		if(s_voices[k].active) {
			if(!audio_backend_voice_playing(k)) {
				audio_service_cover(b, AUDIO_UNIT_VOICE, 0, 4u);
				if(s_achievement_voice == (int)k) s_achievement_voice = -1;
				s_voices[k].active = false;
				/* IDLE snapshot proves there is no deferred voice to release. */
				if(s_menu_voice == (int)k) { s_menu_voice = -1; s_menu_voice_committed = false; }
			} else if(s_voices[k].positional) {
				audio_service_cover(b, AUDIO_UNIT_VOICE, 0, 8u);
				audio_update_voice_gain(k);
			} else {
				audio_service_cover(b, AUDIO_UNIT_VOICE, 0, 16u);
			}
		}
		s_audio_generation++;
		if(!audio_service_leave(b, AUDIO_UNIT_VOICE, entered)) return AUDIO_SERVICE_UNSAFE;
	}
	s_audio_cursor.active = false;
	s_audio_generation++;
	return AUDIO_SERVICE_DONE;
}

enum audio_service_result audio_voice_music_service(const int16_t* pcm,
	uint32_t frames, uint32_t rate, struct audio_service_budget* b) {
	if(!s_backend_live || s_music_master <= 0.0F) {
		s_music_probe_rejects++;
		return AUDIO_SERVICE_DONE;
	}
#ifdef PLATFORM_WII
	enum audio_service_result r = audio_backend_music_service(pcm, frames, rate, s_music_master, b);
	if(r == AUDIO_SERVICE_DONE) s_music_probe_starts++;
	return r;
#else
	(void)pcm; (void)frames; (void)rate; (void)b;
	return AUDIO_SERVICE_UNSAFE;
#endif
}
enum audio_service_result audio_voice_music_poll_service(bool eof, bool* value,
	struct audio_service_budget* b) {
	if(!s_backend_live) { *value = eof; return AUDIO_SERVICE_DONE; }
#ifdef PLATFORM_WII
	return audio_backend_music_poll_service(eof, value, b);
#else
	(void)eof; (void)b; *value = false;
	return AUDIO_SERVICE_UNSAFE;
#endif
}

bool audio_voice_music_witness(struct audio_service_witness* w) {
	w->voice_generation = s_audio_generation;
	w->sampled_at = time_get();
#ifdef PLATFORM_WII
	w->backend_generation = s_wii_audio_generation;
	w->pending = s_wii_audio_cursor.active || s_audio_cursor.active || s_menu_pending;
	w->reserve_us = s_backend_live ? audio_backend_music_reserve_us() : 0;
	w->valid = s_audio_generation != 0;
#else
	w->backend_generation = s_audio_generation;
	w->pending = s_backend_live || s_audio_cursor.active || s_menu_pending;
	w->reserve_us = 0;
	w->valid = s_audio_generation != 0 && !s_backend_live;
#endif
	w->pending |= !audio_deferred_service_idle();
	return w->valid && !w->pending;
}
bool audio_service_wait_reserve(const struct audio_service_witness* w,
	struct audio_service_limits* limits, uint32_t wait_us, uint32_t* reserve) {
	struct audio_service_budget validation = {.start=time_get(), .cap_us=500,
		.limits=limits, .metrics=&s_audio_metrics};
	return audio_service_wait_reserve_budget(w, &validation, wait_us, reserve);
}
bool audio_service_wait_reserve_budget(const struct audio_service_witness* w,
	struct audio_service_budget* validation, uint32_t wait_us, uint32_t* reserve) {
	struct audio_service_limits* limits = validation->limits;
	*reserve = 0;
	if(validation->calibration || validation->ordinary) return false;
	if(!w->valid || w->pending || !limits->margins_verified || !limits->max_age_us
		|| !limits->wake_refill_us || !limits->backend_us || !limits->drift_us)
		return false;
	for(unsigned k = 0; k < AUDIO_UNIT_COUNT; k++)
		if(!limits->e_us[k]) return false;
	if(!limits->bookkeeping_us) return false;
	struct audio_service_witness fresh;
	if(audio_service_enter(validation, AUDIO_UNIT_STATE) != AUDIO_SERVICE_DONE) return false;
	ptime_t entered = time_get();
	bool valid = audio_music_service_witness(&fresh);
	if(!audio_service_leave(validation, AUDIO_UNIT_STATE, entered)) return false;
	/* Latest refill is relative to the original sample, not re-stamped at
	 * entry. Current native evidence may only REDUCE that remaining credit. */
	return valid && audio_service_reserve_aged(w, &fresh, limits, time_get(), wait_us, reserve);
}
