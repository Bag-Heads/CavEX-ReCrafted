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

#ifndef PLATFORM_AUDIO_MUSIC_TIMER_H
#define PLATFORM_AUDIO_MUSIC_TIMER_H

#include <stdbool.h>

/*
	S3 background-music delay/pick state machine
	[740c583: SoundManager.<init> / SoundManager.playRandomMusicIfReady;
	call-site cadence PlayerControllerSP.updateController /
	PlayerControllerMP.updateController, both invoked from
	Minecraft.runTick's `if(!isGamePaused && theWorld != null)` gate -- the
	SAME cadence/gate this engine's client_display_tick() already runs
	cave_mood_tick() under]:

		SoundManager():
			ticksBeforeMusic = rand.nextInt(12000);           // [0,12000)

		playRandomMusicIfReady():
			if (loaded && musicVolume != 0) {
			  if (!playing("BgMusic") && !playing("streaming")) {
			    if (ticksBeforeMusic > 0) { ticksBeforeMusic--; return; }
			    SoundPoolEntry e = soundPoolMusic.getRandomSound();
			    if (e != null) {
			      ticksBeforeMusic = rand.nextInt(12000) + 12000; // [12000,24000)
			      play(e);   // sets playing("BgMusic") true
			    }
			  }
			}

	Three load-bearing details this state machine preserves exactly:
	  1. The WHOLE block, decrement included, is nested inside BOTH
	     `loaded && musicVolume != 0` AND "nothing is currently playing" --
	     ticksBeforeMusic is FROZEN whenever EITHER condition is false, not
	     just while a track sounds. Dropping the music slider to 0 for a
	     moment, or running headless/audio-disabled, pauses the countdown
	     exactly like a track already playing does; it does not lose
	     progress, it just stops advancing.
	  2. Because of (1), the next track's delay window always starts
	     counting from the moment the CURRENT one finishes (and audio is
	     ready), never from when it started -- Beta's well-known "long
	     silent gaps between tracks" behaviour falls out of this for free.
	  3. getRandomSound() (soundPoolMusic, fed by BOTH "music" and
	     "newmusic" -- see audio_music_table.h) picks UNIFORMLY across every
	     individual file, not per-key; the pick itself is therefore the
	     caller's job (audio_music.c), not this header's.

	Pure + header-only (audio_mix.h / jukebox_logic.h precedent) so the
	state transitions are golden-testable independent of the client RNG
	stream, the file decoder, and the platform voice: the caller supplies
	the RNG draw it already rolled (rand_gen_int(&gstate.rand_src, 12000),
	the sanctioned client-RNG substitution used elsewhere for cosmetic-only
	Beta randoms, e.g. client_display_tick.c's cave_mood_tick) rather than
	this header calling into any RNG itself.
*/

/* SoundManager's constructor seed: ticksBeforeMusic = rand.nextInt(12000).
   Trivial by design -- kept as a named function so the citation lives next
   to the one call site (audio_music.c) instead of a bare assignment. */
static inline int music_timer_initial_delay(int draw_0_11999) {
	return draw_0_11999;
}

/* One 20Hz tick of playRandomMusicIfReady's inner state machine.
     ticks_before_music  - in/out: SoundManager's ticksBeforeMusic field.
     audio_ready         - the outer `loaded && musicVolume != 0` gate:
                            true only while the shared backend device is
                            open AND music.volume is currently nonzero
                            (audio_music.c queries audio_ready_for_music,
                            audio.c). False freezes the countdown exactly
                            like a track already playing does -- see the
                            top-of-file note.
     track_playing       - the inner !playing("BgMusic")&&!playing("streaming")
                            gate: true while a track is currently assigned to
                            the voice (audio_music.c's track_active, set the
                            instant a NEW track is picked, cleared only once
                            it fully finishes or is stopped -- NOT merely
                            "has a PCM chunk been handed to the device yet",
                            which would let this fire twice for one pick).
     track_available     - soundPoolMusic non-empty (audio_music_track_count()
                            > 0). A client with no imported music/ pack keeps
                            e == null forever: Beta does nothing on that tick
                            (no crash, no progress, retried every tick) --
                            mirrored here by returning false without touching
                            *ticks_before_music (it is already 0 and stays 0).
     draw_next_0_11999   - the caller's ALREADY-ROLLED rand.nextInt(12000)
                            for the POST-fire reseed. An explicit parameter,
                            never an internal RNG call, is what keeps this
                            function pure.
   Returns true exactly on the tick a NEW track should start; the reseed has
   already happened by the time it returns true, matching Beta's ordering
   (ticksBeforeMusic is written BEFORE the play() call). */
static inline bool music_timer_tick(int* ticks_before_music, bool audio_ready,
									bool track_playing, bool track_available,
									int draw_next_0_11999) {
	if(!audio_ready || track_playing)
		return false;
	if(*ticks_before_music > 0) {
		(*ticks_before_music)--;
		return false;
	}
	if(!track_available)
		return false;
	*ticks_before_music = draw_next_0_11999 + 12000;
	return true;
}

/* debug.music_force ("on") lever -- the debug.weather_force precedent
   (server_weather.c/main.c). Applied AFTER the faithful seed above (the RNG
   draw stays byte-identical either way; only the resulting timer value is
   overridden, and only when armed) so a hardware/capture run can prove the
   S3 decode+voice within a couple of 20Hz ticks instead of waiting up to the
   real Beta ~10-minute [0,12000) window. "off" (the default) returns
   `seeded_delay` UNCHANGED -- byte-identical sim, same class as
   debug.superflat/debug.weather_force. Pure so the lever itself is
   golden-testable without a world, a config, or the client RNG stream. */
static inline int music_timer_force_lever(int seeded_delay, bool force_on) {
	return force_on ? 1 : seeded_delay;
}

/* debug.music_pick (integer, --cw-set-able) -- companion to debug.music_force
   for hardware perf runs that must hit ONE specific track (the long "hal"
   track that exposed the music-decode stall; music_force alone lands on a
   uniform random pick). Encoding: 100 * telemetry key + file digit suffix,
   where telemetry key is audio_music_telemetry_key()'s value (enum + 1:
   calm=1, hal=2, nuance=3, piano=4) and the suffix is the filename's own
   digit (0 = bare "hal.ogg"). So hal1.ogg = 201, piano3.ogg = 403.
   Decodes to *key (enum audio_music_key) / *variant; returns false (lever
   inert) for 0/unset, negatives, or anything out of range. Only consulted
   when debug.music_force is "on", and only AFTER the faithful uniform pick
   draw -- the RNG stream is identical whether or not the lever is set. */
static inline bool music_pick_lever_decode(int pick, int key_count,
										   int max_variants, int* key,
										   int* variant) {
	if(pick <= 0)
		return false;
	int k = pick / 100 - 1;
	int v = pick % 100;
	if(k < 0 || k >= key_count || v >= max_variants)
		return false;
	*key = k;
	*variant = v;
	return true;
}

#endif
