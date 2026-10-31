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

#ifndef PLATFORM_AUDIO_MUSIC_H
#define PLATFORM_AUDIO_MUSIC_H

#include <stdbool.h>
#include <stdint.h>
#include "audio.h"

enum audio_service_result audio_music_update_service(struct audio_service_budget* budget);
bool audio_music_service_witness(struct audio_service_witness* witness);

/*
	S3 background music (docs/finish_line_plan.md S3; the timer is
	audio_music_timer.h, the ring bookkeeping is audio_music_ring.h, the
	track table is audio_music_table.h -- this is the engine glue: registry
	scan (file paths only, per-client import, NEVER bundled -- the legal
	model docs/deviations.md already applies to sound), a decode WORKER
	THREAD (thread.h; the chunk_mesher/net_bringup precedent) that pulls
	stb_vorbis_get_samples_short_interleaved off an open file handle into a
	small 2x8KB double buffer, and the two per-frame/per-tick hooks that
	drive audio.h's dedicated audio_voice_music_* hardware voice.

	Threading contract: audio_music_registry_load/reload/tick/update/
	set_volume/stop and the query functions are MAIN THREAD ONLY (the same
	contract audio.h documents for the rest of the audio surface). The
	decode thread touches ONLY its own PCM double buffer, under its own
	lock -- never the device, never engine state.

	Diagnostic fixture: debug.music_continuous_int=1 (default 0) repeats
	the selected background decoder at EOF without stopping the voice;
	resampler state survives the join. run_wii_hardware.ps1 arms it only
	with -ServiceDiagnostic, alongside music_force. Records stay finite.
	This is diagnostic exposure, not a gameplay change; normal playback
	retains Beta's silence gap, PCM stream and timer/pick RNG.
*/

/* Claim the registry from paths.audiopack (legacy fallback:
   paths.texturepack; music/ + newmusic/ file paths, no decode) and start
   the decode worker thread (parked on its condvar until a track is
   picked -- headless/test builds pay no real cost). volume0_100 is the
   persisted "music.volume" key (Beta's options.musicVolume). */
void audio_music_init(int volume0_100);
/* Stops the voice, signals + joins the decode worker, closes any open
   file. Call BEFORE audio_shutdown() (main.c) -- shared-device teardown
   order. */
void audio_music_shutdown(void);

/* Re-scan <paths.audiopack>/music/ (+ the per-file "newmusic" union --
   see audio_music_table.h). Stops any in-flight track first: a manual
   audio-pack reload can invalidate the currently-open file path. Idempotent
   before audio_music_init (falls through to a first load). */
void audio_music_registry_reload(void);
int audio_music_track_count(void);

/* creative-mode add-on (docs/creative_mode.md, maintainer ruling
   2026-09-03): how many of the six creative-playlist files
   (audio_music_table.h's AUDIO_MUSIC_CREATIVE_TABLE) are currently
   registered -- 0 unless the player imported them via
   tools/import_owned_beta_assets.py's --modern-minecraft path. Rescanned by
   the SAME audio_music_registry_reload() above (one scan, two catalogs).
   audio_music_tick()'s per-world selection reads this: a creative world
   (gstate.config_creative) plays from the creative catalog only while this
   is >= 1, and falls back to the Beta list otherwise -- survival worlds are
   never affected by this count either way. */
int audio_music_creative_track_count(void);

/* Beta's options.musicVolume -- see audio.h's audio_set_music_volume,
   which this wraps (kept here too so client_display_tick.c/main.c/
   screen_options.c only ever need to #include this one header for the S3
   surface, matching how audio.h's audio_play3d/audio_play_ui are the ONE
   surface for one-shot sound). Returns false if a newly-required backend
   device failed to open. */
bool audio_music_set_volume(int volume0_100);

/* Once per 20Hz client tick (client_display_tick.c, beside cave_mood_tick,
   inside the SAME `!gstate.paused` gate -- matches
   PlayerControllerSP.updateController's `!isGamePaused` gate exactly):
   runs the playRandomMusicIfReady timer and, when it fires, picks a track
   uniformly and hands it to the decode thread to open. No-op with 0
   tracks, 0 volume, or a track already active. Draws from gstate.rand_src
   (the sanctioned client-RNG substitution, cave_mood_tick's own
   precedent). World-loaded gated by the caller, same as cave mood. */
void audio_music_tick(void);

/* Once per frame (main.c, beside audio_update -- but call this
   UNCONDITIONALLY on pause, never inside an `if(!paused)` block: Beta's
   paulscode "BgMusic" source keeps sounding through a paused game, since
   nothing pauses sndSystem itself [no isGamePaused check anywhere in
   SoundManager]). Starts the voice once the first chunk is ready, advances
   the ring buffer per audio_music_ring.h when the current chunk's
   audio_voice_music_playing() goes false, and stops the voice + clears
   track-active once the last (EOF) chunk finishes. Main thread only. */
void audio_music_update(void);

bool audio_music_is_playing(void);
/* Stop outright (registry-reload precondition; also usable as a manual
   skip). Closes the open file, clears the ring, clears track-active --
   does NOT touch the timer's ticksBeforeMusic (matching Beta: nothing
   resets that field except the ~10-20 minute window elapsing). */
void audio_music_stop(void);

/* F3 dials (screen_ingame.c, the "snd: arena ..." row precedent): which
   key is sounding (NULL when silent) and the ring's underrun counter
   (audio_music_ring.h's -1 case -- should read 0 on real hardware; a live
   count is a genuine "the decode thread can't keep up" report, the
   s_arena_rejects precedent -- never hidden). */
const char* audio_music_current_key(void);
unsigned audio_music_underrun_count(void);

/* ===== jukebox record playback (Beta "13"/"Cat" streaming discs) =========
   [decompile 740c583: RenderGlobal.playAuxSFX case 1005 -> World.playRecord
   -> SoundManager.playStreaming] Records use the SAME single hardware voice
   as background music -- there is only ONE audio_voice_music_* voice,
   matching Beta's real "streaming"/"BgMusic" pair never being simultaneously
   active. Inserting a record ALWAYS preempts whatever is currently sounding
   (see audio_music_record.h for the exact preempt shape); the background
   timer will not fire again while a record is active for free, because
   record playback sets the SAME track_active flag audio_music_tick already
   gates the timer on (music_timer_tick's `track_playing` parameter).

   POSITIONING DEVIATION (docs/deviations.md): Beta's playStreaming is a
   POSITIONED source with linear distance falloff (RenderGlobal.playRecord
   passes the jukebox's x/y/z straight through). audio_voice_music_start
   (audio.h) has no 3D positioning parameter -- follow-up work, not blocked
   on here. Records therefore play FLAT, at the same loudness as background
   music, everywhere in the loaded world, rather than attenuating with
   distance from the jukebox. Documented, not silent. */

/* Item id -> "records/<stem>.ogg" basename ("13"/"cat"), or NULL for
   anything that is not one of Beta's two records. Pure, no I/O -- exposed
   for golden testing (item_types.h ITEM_RECORD_13=2256 / ITEM_RECORD_CAT=
   2257, duplicated here as literals rather than pulling that header into
   this platform TU -- the audio_sound_table.h precedent of self-contained
   id tables). */
const char* audio_music_record_file_stem(uint16_t item_id);

/* Preempts whatever is currently sounding (background music or another
   record) and starts streaming `path` -- an sd-relative path the caller has
   already resolved, e.g. "<pack>/records/13.ogg" -- through the SAME S3
   decode worker + double buffer + hardware voice background music uses. A
   NULL/empty path, or the module never having started (audio_music_init not
   yet called), is a no-op. Use audio_music_stop_record to eject/stop
   instead of passing NULL (mirrors playStreaming's var1==null branch, which
   this function does not model -- see audio_music_record.h). */
void audio_music_play_record(const char* path);

/* Convenience for the CRPC_RECORD_EVENT client handler (client_interface.c):
   resolves a Beta record item id against the session's effective audio
   pack records/ directory and calls audio_music_play_record. No-op for
   any id that is not a known record. */
void audio_music_play_record_for_item(uint16_t item_id);

/* Stops a currently-playing record (the jukebox ejected, or the record
   reached EOF). Leaves background music untouched if a record was not
   actually the thing sounding -- becomes eligible again through the normal
   timer, exactly like any other audio_music_stop(). No-op if no record is
   currently active. */
void audio_music_stop_record(void);

/* True while a record (not a random background track) is the thing
   currently sounding through the shared voice. */
bool audio_music_is_playing_record(void);

/* ===== hw_telemetry numeric surface (source/platform/hw_telemetry.c's
   heartbeat; the mp_/wl_ probe field precedent) =============================
   Machine-provable evidence that the decode thread + hardware voice
   actually ran on real silicon, independent of a human's ears. Main thread
   only, like every other query in this file. */

/* 0 = nothing sounding; 1..AUDIO_MUSIC_KEY_COUNT = that Beta background-
   music key + 1 (audio_music_table.h's AUDIO_MUSIC_TABLE);
   AUDIO_MUSIC_KEY_COUNT+1 = a jukebox record is currently sounding (records
   have no background-music table key); AUDIO_MUSIC_KEY_COUNT+2 ..
   AUDIO_MUSIC_KEY_COUNT+1+AUDIO_MUSIC_CREATIVE_KEY_COUNT = that
   creative-mode-add-on key (AUDIO_MUSIC_CREATIVE_TABLE) + that offset,
   2026-09-03. */
unsigned audio_music_telemetry_key(void);

/* Playback position of whatever is CURRENTLY sounding (background track or
   record), in milliseconds since that track/record started -- 0 when
   silent. Derived from the decode worker's own decoded-frame counter and
   the open file's native sample rate; resets to 0 whenever playback
   (re)starts or stops. */
uint32_t audio_music_telemetry_ms(void);

/* MAIN-only: actual backend queue duration and whether decode needs CPU. */
uint32_t audio_music_lease_reserve_us(bool* worker_pending);

/* G9 decode deadline promotion (T2 scheduling, audio_music_ring.h banner).
   MAIN-only configure; `enabled` is experimental.g9_tick_boost_int. Even when
   enabled the worker is promoted only while a LOCAL server owns the tick
   boost (thread_g9_tick_boost_local_present): remote Beta MP and the default
   build keep the pri-48 worker exactly as before. Stats are diagnostics. */
struct audio_music_promotion_stats {
	uint32_t promotions;	  /* 48 -> promoted transitions */
	uint32_t demotions;		  /* promoted -> 48 transitions */
	uint32_t promoted_chunks; /* chunks decoded while promoted */
	uint32_t promoted_max_us; /* longest single promoted chunk decode */
	bool promoted;			  /* current state */
};
void audio_music_decode_promotion_configure(bool enabled);
void audio_music_decode_promotion_snapshot(
	struct audio_music_promotion_stats* out);

#endif
