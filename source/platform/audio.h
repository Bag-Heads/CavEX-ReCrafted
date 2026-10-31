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

#ifndef PLATFORM_AUDIO_H
#define PLATFORM_AUDIO_H

/*
	S0 audio foundation (docs/finish_line_plan.md §2b). One platform module in
	the gfx.c/thread.h mold: this header is the whole engine-facing surface,
	platform/audio.c holds the shared voice/mix logic and #includes the
	backend (wii/audio.c = AESND, pc/audio.c = miniaudio low-level device).

	Model (Beta 1.7.3, decompile 740c583 = the spec):
	  - MONO one-shot samples, decoded AT LOAD into one fixed PCM arena
	    (mem2_audio_region; 12MB wanted — S1 measured the 32-key real Beta
	    overlay at 6.04MB, S2's long cave/portal/fire moods pushed it past
	    8MB — reserved once at init, no eviction; the texture precedent).
	    On Wii that is its OWN MEM2 region, NOT part of the display-list
	    carve, and the grant may be SHORT (mem2.c's ordering argument):
	    audio_arena_total() is the granted size, not the constant. No
	    runtime ogg decode outside the load path, ever.
	  - Fixed voice pool; camera-relative linear attenuation + subtle L/R pan
	    recomputed once per frame in audio_update (a sanctioned T2 stand-in
	    for Beta's paulscode 3D panner — docs/deviations.md).
	  - Beta rules exactly (audio_mix.h): audible range 16*max(1,vol) with
	    LINEAR falloff to zero [SoundManager.playSound rolloff model 2;
	    RenderGlobal.playSound cull], playback loudness min(vol,1)*master
	    [SoundManager volume clamp], pitch UNCLAMPED -> playback rate =
	    native_rate*pitch [SoundManager sets pitch unclamped — the 0.5..2.0
	    clamp people remember is from later versions].
	  - SERVER-caused sounds arrive via CRPC_SOUND (client_interface.h, the
	    CRPC_SMOKE_PUFF boundary rule) -> audio_play3d. Client-LOCAL sounds
	    (GUI click, own mining hit ticks) call audio_play3d/audio_play_ui
	    directly, exactly the paths that bypass World in Beta SP
	    [PlayerControllerSP.java:74; GuiScreen.mouseClicked:57-59].
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "time.h"
#include "../cglm/cglm.h"

/* R18: engineering admission, never a hard-real-time WCET claim. Zero E
 * means uncalibrated. Native defaults stay zero until covered by the shared
 * exact-release calibration. Every turn shares ONE <=500-us deadline. */
enum audio_service_result {
	AUDIO_SERVICE_DONE, AUDIO_SERVICE_MORE, AUDIO_SERVICE_UNSAFE
};
enum audio_service_unit {
	AUDIO_UNIT_STATE, AUDIO_UNIT_COPY, AUDIO_UNIT_ZERO, AUDIO_UNIT_FLUSH,
	AUDIO_UNIT_STOP, AUDIO_UNIT_GAIN, AUDIO_UNIT_START, AUDIO_UNIT_VOICE,
	AUDIO_UNIT_COUNT
};
#define AUDIO_SERVICE_BYTES 256u /* copy/zero <=9 lines; aligned flush =8 */
/* Audited newlib small-angle reduction only; larger/nonfinite yaw escapes
 * ordinary with the original expression, never clamped or approximated. */
static inline bool audio_service_voice_math_finite(float yaw_rad) {
	return isfinite(yaw_rad) && fabsf(yaw_rad) <= 64.0F;
}
struct audio_service_limits {
	uint32_t e_us[AUDIO_UNIT_COUNT], bookkeeping_us;
	uint32_t wake_refill_us, backend_us, drift_us, max_age_us;
	bool margins_verified;
};
struct audio_service_metrics {
	uint32_t maximum_us[AUDIO_UNIT_COUNT], count[AUDIO_UNIT_COUNT];
	uint32_t overruns[AUDIO_UNIT_COUNT];
	uint32_t maximum_bytes[AUDIO_UNIT_COUNT], coverage[AUDIO_UNIT_COUNT];
	uint32_t bookkeeping_maximum_us, bookkeeping_count;
};
struct audio_service_budget {
	ptime_t start, unit_start, last_end;
	bool have_last_end;
	uint32_t cap_us;
	struct audio_service_limits* limits;
	struct audio_service_metrics* metrics;
	/* Calibration executes finite units WITHOUT granting wait credit.
	 * Ordinary escape finishes retained work without engineering admission. */
	bool calibration, ordinary;
};
static inline void audio_service_cover(struct audio_service_budget* b,
	enum audio_service_unit unit, uint32_t bytes, uint32_t conditions) {
	if(b->ordinary || !b->metrics) return;
	if(bytes > b->metrics->maximum_bytes[unit]) b->metrics->maximum_bytes[unit] = bytes;
	b->metrics->coverage[unit] |= conditions;
}
struct audio_service_witness {
	uint32_t generation, backend_generation, voice_generation;
	ptime_t sampled_at;
	uint32_t reserve_us, latest_refill_us;
	bool valid, inactive, pending;
};
/* Pure part of wait-entry validation, also driven by the native ring tape.
 * Inactive evidence expires just like active evidence; it is never infinity. */
static inline bool audio_service_reserve_aged(const struct audio_service_witness* w,
	const struct audio_service_witness* fresh, const struct audio_service_limits* limits,
	ptime_t now, uint32_t wait_us, uint32_t* reserve) {
	*reserve = 0;
	if(!w->valid || !fresh->valid || w->pending || fresh->pending
		|| fresh->generation != w->generation || fresh->backend_generation != w->backend_generation
		|| fresh->voice_generation != w->voice_generation || fresh->inactive != w->inactive)
		return false;
	uint64_t age = time_diff_us(w->sampled_at, now);
	if(age >= limits->max_age_us) return false;
	uint64_t margin = (uint64_t)limits->wake_refill_us + limits->backend_us + limits->drift_us;
	uint64_t available = w->inactive ? limits->max_age_us : w->reserve_us;
	if(available <= age + margin + wait_us) return false;
	available -= age + margin;
	if(!w->inactive && (w->latest_refill_us <= age + wait_us
		|| fresh->reserve_us <= margin + wait_us)) return false;
	if(!w->inactive && fresh->reserve_us-margin < available) available = fresh->reserve_us-margin;
	*reserve = available > UINT32_MAX ? UINT32_MAX : (uint32_t)available;
	return true;
}
static inline enum audio_service_result audio_service_enter(
	struct audio_service_budget* b, enum audio_service_unit unit) {
	if(b->ordinary) return AUDIO_SERVICE_DONE;
	ptime_t now = time_get();
	/* Include admission in the unit, and record finite inter-unit work
	 * separately. The gap also covers the next admission timer read. */
	if(b->metrics) {
		uint64_t gap = time_diff_us(b->have_last_end ? b->last_end : b->start, now);
		uint32_t us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
		b->metrics->bookkeeping_count++;
		if(us > b->metrics->bookkeeping_maximum_us) b->metrics->bookkeeping_maximum_us = us;
	}
	b->unit_start = now;
	uint64_t used = time_diff_us(b->start, now);
	if(used >= b->cap_us || b->cap_us > 500u) return AUDIO_SERVICE_MORE;
	if(b->calibration) return AUDIO_SERVICE_DONE;
	uint32_t e = b->limits->e_us[unit];
	if(!e || !b->limits->bookkeeping_us) return AUDIO_SERVICE_UNSAFE;
	return (uint64_t)e + b->limits->bookkeeping_us <= b->cap_us-used
		? AUDIO_SERVICE_DONE : AUDIO_SERVICE_MORE;
}
static inline bool audio_service_leave(struct audio_service_budget* b,
	enum audio_service_unit unit, ptime_t entered) {
	if(b->ordinary) return true;
	(void)entered; /* call-site stamp retained; measurement begins at admission */
	uint64_t elapsed = time_diff_us(b->unit_start, time_get());
	uint32_t us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
	if(b->metrics) {
		b->metrics->count[unit]++;
		if(us > b->metrics->maximum_us[unit]) b->metrics->maximum_us[unit] = us;
	}
	b->last_end = time_get();
	b->have_last_end = true;
	bool ok = time_diff_us(b->start, b->last_end) <= b->cap_us
		&& (b->calibration || elapsed <= b->limits->e_us[unit]);
	if(!ok) {
		if(b->metrics) b->metrics->overruns[unit]++;
		b->limits->e_us[unit] = 0; /* invalidate; no retroactive prevention */
	}
	return ok;
}
/* MAIN only. No allocation/decode/I/O; unsupported work escapes ordinary.
 * MORE/UNSAFE never imply completion or permission to sleep. */
enum audio_service_result audio_update_service(vec3 cam_pos, float yaw,
	struct audio_service_budget* budget);
enum audio_service_result audio_voice_music_service(const int16_t* pcm,
	uint32_t frames, uint32_t rate, struct audio_service_budget* budget);
enum audio_service_result audio_voice_music_poll_service(bool eof,
	bool* full_or_drained, struct audio_service_budget* budget);
void audio_service_get_budget(struct audio_service_budget* budget, uint32_t cap_us);
void audio_service_get_metrics(struct audio_service_metrics* metrics);
bool audio_service_wait_reserve(const struct audio_service_witness* witness,
	struct audio_service_limits* limits, uint32_t wait_us,
	uint32_t* reserve_us);
bool audio_service_wait_reserve_budget(const struct audio_service_witness* witness,
	struct audio_service_budget* budget, uint32_t wait_us, uint32_t* reserve_us);
bool audio_voice_music_witness(struct audio_service_witness* witness);


/* Registry ids. Names (audio_sound_name) are the Beta SoundPool keys — the
   file path is derived by '.'->'/' (the SoundPool slash-to-dot rule in
   reverse [SoundPool.addSound]). S1: `enum audio_sound_id` (incl.
   AUDIO_SOUND_COUNT) is derived from the ONE id/key X-macro table shared
   with the host pack tool, so the engine scan and the pack builder's
   manifest cannot drift — the table header carries the per-entry notes. */
#include "audio_sound_table.h"

/* "no sound" sentinel for table lookups (air break, silent mobs — squid's
   get*Sound all return null in b1.7.3 [EntitySquid.java:34-45]; creeper and
   slime have NO idle). audio_play3d/emit no-op on it. */
#define AUDIO_SOUND_NONE 0xFFFFu

/* ===== engine lifecycle (called from main.c) ============================ */

/* Claim the PCM arena and load the registry from paths.audiopack, falling
   back to paths.texturepack for legacy configs (decode-at-load); open the
   platform backend only when `enabled` AND volume > 0 — Beta's lazy OpenAL
   init rule [SoundManager.loadSoundSettings]. HEADLESS MODE (tests):
   enabled=false still loads the registry + samples so mapping/parse checks
   run with no audio device; every audio_play* is then a no-op. */
void audio_init(bool enabled, int volume0_100);
void audio_shutdown(void);

/* Apply the existing audio.volume master knob without rebuilding the decoded
 * registry. Opens/closes the backend on zero crossings; false means a newly
 * requested backend could not be opened and the old volume remains live. */
bool audio_set_master_volume(int volume0_100);

/* Positional one-shot. vol > 1 extends the audible range (16*vol), it never
   plays louder; pitch is unclamped. Cheap distance cull at the Beta range
   [RenderGlobal.playSound]. Client/main thread only (clin_process + screens
   both run there). */
void audio_play3d(uint16_t sound_id, vec3 pos, float vol, float pitch);

/* The Beta playSoundFX path: non-positional, no attenuation, loudness
   min(vol,1) * 0.25F * master [SoundManager.playSoundFX]. GUI clicks. */
void audio_play_ui(uint16_t sound_id, float vol, float pitch);

/* Once per frame: store the listener and recompute every live voice's L/R
   gain from the current camera (the per-frame pan/attenuation decision —
   scout DECISIONS). Also reaps finished voices (poll model; the Wii backend
   uses no AESND callbacks — those run in audio-IRQ context). */
void audio_update(vec3 cam_pos, float cam_yaw_deg);

/* F3 dials: fixed-arena fill (the MEM2 dial precedent) + live voices.
   audio_arena_total() is the GRANTED region size, so it doubles as the
   shipping-build report that the MEM2 reservation came up short: a healthy
   console reads "snd: arena <used>/12288KB" with used strictly below total; a
   total below 12288KB means arena2 could not hold the full region behind the
   carve and the keys scanned last (ordinary keys, after the protected UI/note
   passes) are silent. */
size_t audio_arena_used(void);
size_t audio_arena_total(void);
int audio_voices_active(void);

/* ===== registry (shared TU audio_registry.c; usable server-side) ======== */

/* Idempotent; audio_init calls it. Public so headless tests may load the
   registry without any backend at all. */
void audio_registry_load(void);

/* ===== async pass C (asyncsnd-0926, Astra ruling 3, 2026-09-26) ==========
   OPT-IN T2 deviation, config "audio.async_load" ("off" default = the whole
   scan runs synchronously inside audio_init/audio_registry_reload, byte-for-
   byte as before). When "on", the protected passes A (menu.*) and B (note.*)
   still complete SYNCHRONOUSLY before the call returns -- the approved UI
   package is ready before the title can take input -- and pass C (every
   other key, unchanged table order) is handed to ONE loader thread that walks
   the very same audio_scan_key/audio_load_candidate path into the same arena.
   The arena has exactly one writer at any time: the main thread until the
   thread starts, the loader thread until it is joined.

   PUBLICATION. A pass-C key becomes visible to audio_registry_pick and the
   other readers only once ALL its variants (pack, then per-key fallback) are
   resident, its PCM is flushed for the DSP (Wii), and the key's "published"
   flag is set under the module lock; a reader never sees a partial variant
   table. Until then the key is PENDING: a pick returns NULL and the play is
   SKIPPED -- never queued, never replayed later -- and the skip is counted
   (audio_registry_async_status). A protected (pass A/B) key can never be
   pending; a pick that finds one so is counted as an invariant failure.

   WORLD ENTRY is gated behind completion of the SELECTED generation
   (loading_notice.h's gate screen; main.c's remote-connect seam): no local
   world load and no remote connection starts while audio_registry_async_
   pending() is true, so gameplay audio is always the complete registry.

   GENERATIONS. Every scan start bumps the generation; a cancel bumps it again.
   The loader publishes a key only under the generation it started with, so a
   completion that lands after a cancel (a pack switch, shutdown) is ignored.
   A pack switch (audio_registry_reload) cancels and JOINS the loader before
   any voice is stopped or one arena byte is rewritten, then restarts pass C
   under the new generation. The loader thread's creation failing falls back
   to finishing pass C synchronously on the calling thread -- the same bytes,
   later title. RAW-OGG PACKS (any pass-C .ogg candidate present in the
   selected root, asked through the directory index) keep the WHOLE scan
   synchronous too (the ruling's "unsupported raw-OGG inputs retain the
   existing synchronous path"): stb_vorbis never runs on the loader thread.

   PROGRESS TIMEOUT. The main thread never blocks on the loader, so the
   watchdog's ordinary 3 s HANG rule stays fully armed; the loader has its own
   separate progress timeout (AUDIO_ASYNC_PROGRESS_TIMEOUT_MS between two key
   publications) after which the run is FAILED and the gate offers Retry
   (a full reload) or Back. Nothing renews that grace. */
enum audio_async_state {
	AUDIO_ASYNC_OFF = 0, /* opt-in off, or no loader run in flight */
	AUDIO_ASYNC_RUNNING, /* pass C on the loader thread */
	AUDIO_ASYNC_DONE,	 /* the selected generation completed */
	AUDIO_ASYNC_FAILED	 /* progress timeout: retry (reload) or back */
};
struct audio_async_status {
	enum audio_async_state state;
	int keys_done, keys_total; /* pass-C keys published / total */
	uint32_t generation;
	unsigned skips;			   /* picks of a PENDING key (skipped plays) */
	unsigned protected_pending; /* invariant failures: must stay 0 */
	unsigned thread_failures;  /* loader thread creation failed -> sync */
	unsigned stalls;		   /* progress timeouts declared */
	int sync_ms;			   /* passes A+B (+deferred setup) wall time */
	int ready_ms;			   /* scan start -> full registry, -1 = not yet */
	unsigned ogg_sync;		   /* runs kept synchronous: raw .ogg in the pack */
	unsigned ogg_decoded_async; /* OGG decodes on the loader thread: must be 0 */
};
void audio_registry_async_status(struct audio_async_status* out);
/* True while world entry must wait: RUNNING or FAILED. False with the opt-in
   off, before the first scan and once the registry is complete. */
bool audio_registry_async_pending(void);
/* Once per frame, main thread: joins a finished loader, runs the scan
   epilogue (dir index freed, records stashed/emitted) and declares the
   progress timeout. One static test when nothing is running. */
void audio_registry_async_poll(void);
/* Cancel the in-flight run and JOIN the loader thread (shutdown, and the
   first step of every reload). Safe with nothing running. Main thread. */
void audio_registry_async_join(void);
/* ONE bounded unit of loader work: the next pass-C key, or false when the run
   is complete/cancelled. The loader thread's loop; tests drive it by hand
   (no thread is created when audio_registry_test_async_manual is set). */
bool audio_registry_async_step(void);
/* Wall time without a single published key before RUNNING becomes FAILED. */
#define AUDIO_ASYNC_PROGRESS_TIMEOUT_MS 20000

/* Drain the BOOT scan's one observer record ({"event":"audio_load", ...}:
   root, variants, keys_fallback, arena_used/cap/rejects, protected_silent,
   protected_short, ms). The boot scan runs inside audio_init, long before the
   observer exists, so the record is stashed and emitted from the frame loop
   instead -- otherwise the one scan that decides what the whole session can
   play is invisible to an armed run, and on the NDEBUG Wii build there is no
   stderr to fall back on. Call once per frame beside the other retained
   observer emits: it is a single static-bool test when there is nothing
   pending, never blocks, and leaves the record pending if the ring is full.
   Main thread only, like every other audio_* entry point. */
void audio_registry_emit_load_stats(void);

/* Stop EVERY voice, then re-run the registry load against the CURRENT
   paths.audiopack (legacy fallback: paths.texturepack) into the SAME arena
   (bump reset to 0; the mem2
   region is granted once and never freed or replaced). MAIN THREAD ONLY — the same
   contract as every other audio_* call (clin_process + the screens run
   there); the off-thread halves are excluded by audio_stop_all's backend
   stop BEFORE any arena byte is rewritten. Headless-safe (no backend =
   nothing to stop). */
void audio_registry_reload(void);

/* Main-thread load service. Register before audio_init/reload. Called between
 * keys and I/O/conversion chunks, never on the async worker or under its lock.
 * The callback may present a pack-independent frame, poll input and pet the
 * watchdog. It must not re-enter audio, change config or free scanner resources;
 * only the readiness getters and cancel_load flag are safe here. Hooks can
 * run several times per display frame; the presenter should throttle redraws,
 * while still polling input/watchdog. Change registration only with no loader
 * in flight. A single libc I/O or Vorbis decode is measured, not preemptible. */
typedef void (*audio_registry_service_fn)(void* ctx);
void audio_registry_set_service(audio_registry_service_fn fn, void* ctx);
enum audio_registry_ready { AUDIO_REGISTRY_LOADING, AUDIO_REGISTRY_READY,
	AUDIO_REGISTRY_FAILED, AUDIO_REGISTRY_DISABLED };
/* Required set = the existing protected menu.* and note.* keys. Other keys
 * retain their existing optional/fallback policy. DISABLED is a completed,
 * non-retryable degraded load (no arena or a silent protected key); existing
 * loaded sounds remain playable. Reason is stable until an explicit reload.
 * Poll async completion on main BEFORE querying this non-mutating state. */
enum audio_registry_ready audio_registry_ready_state(void);
const char* audio_registry_failure_reason(void);
uint64_t audio_registry_service_gap_max_us(void);
void audio_registry_cancel_load(void);
/* False when DISABLED, scanning or an async generation still owns the arena. */
bool audio_registry_retry(void);

/* pack-cache-0927 (opt-in "pack.cache", audio_cache.h). Diagnostics: the last
   scan's cache outcome -- "off" (the opt-in was off or bypassed), "hit",
   "miss" (then decoded fresh), "written" (fresh decode published),
   "unwritable" (publication failed; caching disabled for that load),
   "bypass" (a WAV-only root, which the audio cache does not serve: a fresh
   WAV load measured faster -- astra-packkey-0927) -- and the reason. */
const char* audio_registry_cache_status(void);
const char* audio_registry_cache_reason(void);
/* The PC host tool's producer (tools/audio_cache_tool.c): publish the
   CURRENT completed registry as a cache for `target` (an enum
   audio_cache_target), keyed exactly as that target computes it. False when
   there is no completed resident registry or the card refuses the write.
   `dir_out` (optional) receives the cache directory. */
/* The measured pack policy for the CURRENT pack: NULL = the audio cache
   serves it, else why it is bypassed (e.g. a WAV-only root). */
const char* audio_registry_cache_bypass_reason(void); /* of the last scan */
bool audio_registry_cache_publish_target(uint32_t target, char* dir_out,
										 size_t dir_size);

/* Stop-and-reap every voice NOW, engine slots and backend both (PC: the
   voice array zeroed UNDER the mixer lock, so the device callback can never
   touch a stopping voice again; Wii: AESND_SetVoiceStop on every slot AND
   the poll deadlines cleared, so audio_update reaps them all immediately).
   The reload precondition; harmless on its own. Main thread only. */
void audio_stop_all(void);

/* One decoded PCM16-mono variant, resident in the fixed arena (host byte
   order: big-endian on Wii = what the DSP DMA reads, native on PC). */
struct audio_sample {
	const int16_t* pcm;
	uint32_t frames;
	uint32_t rate;
};

/* Uniform random variant pick for one play — CLIENT-side cosmetic roll,
   exactly where Beta rolled it [SoundPool.getRandomSoundFromSoundPool];
   NULL when the key has no loaded variants. */
const struct audio_sample* audio_registry_pick(uint16_t sound_id);

/* DIAGNOSTICS ONLY -- observer telemetry, no gameplay meaning, no return
   value any caller may branch on. Tells the sound registry that `smp` was
   just started on voice `slot`, which is the one moment a pack's audio can be
   said to be PLAYING rather than merely selected: the registry completes the
   `audio_first_play` record there (slot, whether the sample lies inside the
   arena the last reload filled, the key and root captured at pick time).
   audio_registry_pick alone is not that moment -- audio_play3d picks and may
   then cull by distance without ever starting a voice.

   Cost when nothing is pending, which is every play outside an armed hw_run:
   one call and one static bool test. Main thread only (the audio_* contract).
   Call it immediately after audio_backend_voice_start, once per start. */
void audio_registry_note_voice_started(int slot,
									   const struct audio_sample* smp);

/* C2 P2, the opt-in deferred cave-ambience key (audio_deferred.h; config
   "audio.deferred_cave", default ON, "off" opts out). A deferred variant is PICKED like any
   other -- same single cosmetic RNG draw -- but its sample has pcm == NULL:
   the registry hands audio.c the file to load instead, and the voice starts
   from audio_update once the whole sample is resident. Main thread only. */
struct audio_deferred_source;
const struct audio_deferred_source*
audio_registry_deferred_source(uint16_t sound_id,
							   const struct audio_sample* smp, int* variant);
/* audio_registry_note_voice_started for a deferred start: names the request's
   OWN key/variant, not whatever was picked while it loaded. */
void audio_registry_note_deferred_started(int slot, uint16_t sound_id,
										  int variant,
										  const struct audio_sample* smp);
/* Per-frame observer drain (scan record + one terminal record); one static
   bool when the opt-in is off. */
void audio_registry_deferred_telemetry(void);
/* Drop a deferred load that has not started playing yet (world left). */
void audio_cancel_deferred(void);
/* Variant `variant` of a key (NULL when out of range): the pick WITHOUT the
   RNG draw, for the evidence lever below and for tests. */
const struct audio_sample* audio_registry_variant(uint16_t sound_id,
												  int variant);
/* DEBUG / hardware-evidence only (config "debug.cave_sound_every_s"): play
   variant `n % count` of a key through the audio_play3d path with no cosmetic
   RNG draw. Never called by gameplay. */
void audio_debug_play_variant(uint16_t sound_id, int n, vec3 pos, float vol,
							  float pitch);

/* The client cosmetic RNG stream backing the variant pick above, exposed for
   the OTHER client-local cosmetic rolls Beta drew from each client entity's
   own unsynced java.util.Random (MP idle-voice ramp, pitch wobbles). Never a
   simulation stream; main thread only (the audio_* contract). */
struct random_gen;
struct random_gen* audio_cosmetic_rng(void);

const char* audio_sound_name(uint16_t sound_id);
int audio_registry_variant_count(uint16_t sound_id);
bool audio_registry_variant_info(uint16_t sound_id, int variant,
								 uint32_t* frames, uint32_t* rate);

/* Tiny PCM16-mono RIFF/WAVE parser (no library). Returns a pointer INTO
   `data` (little-endian file order — convert per sample with
   audio_pcm_sample_from_le when reading on a big-endian host). */
bool audio_wav_parse(const uint8_t* data, size_t size, const int16_t** pcm,
					 uint32_t* frames, uint32_t* rate);
int16_t audio_pcm_sample_from_le(const int16_t* pcm, size_t index);

/* ===== Beta StepSound tables (pure lookups, no file I/O) ================ */

/* Per-block-id StepSound mapping [740c583: Block.java:593-688; default
   soundPowderFootstep for every unassigned id, Block.java:141].
   break = stepSoundDir(): the BREAK name — glass-type breaks as
   random.glass (StepSoundStone), sand breaks as step.gravel (StepSoundSand).
   step  = stepSoundDir2(): the WALK/PLACE/HIT name — always "step."+family.
   volume is 1.0F for all nine instances; pitch is 1.5F for metal, else 1.0F
   [Block.java:7-15]. Callers apply the per-event transforms themselves
   (break/place (v+1)/2 @ p*0.8, mining tick (v+1)/8 @ p*0.5).
   block_id 0 (air) returns AUDIO_SOUND_NONE [RenderGlobal aux-2001 id>0]. */
uint16_t audio_block_break_sound_id(uint8_t block_id);
uint16_t audio_block_step_sound_id(uint8_t block_id);
/* Beta footstep block resolution (moveEntity's var28): the foot-cell id,
   EXCEPT a fence one below substitutes UNCONDITIONALLY — fences are 1.5
   blocks tall, so standing on one leaves the foot cell itself air, yet Beta
   still walks it as fence wood. [740c583: Entity.moveEntity:478-483] */
uint8_t audio_footstep_block_id(uint8_t foot_id, uint8_t below_id);
float audio_block_sound_volume(uint8_t block_id);
float audio_block_sound_pitch(uint8_t block_id);

/* One footstep DECISION (Beta Entity.moveEntity's playStepSound tail), pure:
   foot_block_id is the cell under the feet (floor(y - 0.2 - yOffset)),
   above_block_id the cell the feet occupy. Order is Beta's exactly:
   SNOW-layer-above override first (above == Block.snow id 78 -> snow's cloth
   sound), else the foot block's own stepSoundDir2() UNLESS the foot cell is
   air or liquid (-> AUDIO_SOUND_NONE, silent). *vol getVolume()*0.15F,
   *pitch getPitch(). Shared by the SP server emit (server_local_emit_footstep)
   and the MP client's own-movement footsteps, so the two paths cannot drift.
   [740c583: Entity.moveEntity:478-499] */
uint16_t audio_footstep_params(uint8_t foot_block_id, uint8_t above_block_id,
							   float* vol, float* pitch);

/* Per-mob idle/hurt/death sound ids + getSoundVolume() [740c583:
   EntityLiving defaults + the per-mob overrides; EntityCow.getSoundVolume =
   0.4F is the only S0 volume override]. Any output pointer may be NULL. */
void audio_mob_sounds(int monster_id, uint16_t* idle, uint16_t* hurt,
					  uint16_t* death, float* volume);

/* ===== S3 streaming-music voice (docs/finish_line_plan.md S3) ===========
   ONE dedicated hardware voice, separate from the AUDIO_VOICES one-shot
   pool above (this file's AUDIO_VOICES=16 comment already reserves the
   other half of AESND's 32 Wii hw voices for it). audio_music.c -- a
   SEPARATE translation unit that owns the S3 registry/timer/decode-thread
   layer -- is the only caller of this group; every other engine site uses
   audio_music.h instead. Keeping the group here (not duplicated into a
   second per-platform backend init) means there is still exactly ONE
   device-open lifecycle (ONE AESND_Init / ONE ma_device) shared by sound
   effects and music, which is what lets `audio.volume`==0 with
   `music.volume`>0 (or vice versa) keep the shared backend alive -- Beta's
   own rule [SoundManager.loadSoundSettings: `loaded` opens on
   `soundVolume!=0 || musicVolume!=0`]. Main thread only, same contract as
   the rest of this file; the decode thread (audio_music.c) never touches
   the device directly, only its own PCM double buffer under its own lock. */

/* Beta's options.musicVolume -- a slider INDEPENDENT of options.soundVolume
   [SoundManager.onSoundOptionsChanged / playRandomMusicIfReady's own
   `musicVolume != 0` gate]. Opens the shared backend if either volume is
   now nonzero; may close it if BOTH are now zero. A volume of 0 stops the
   music voice outright [onSoundOptionsChanged: `musicVolume==0 ->
   sndSystem.stop("BgMusic")`]. Returns false if a newly-required device
   failed to open (the old volume stays live, mirrors
   audio_set_master_volume's contract). */
bool audio_set_music_volume(int volume0_100);

/* Beta's own `loaded && musicVolume != 0` outer gate on
   playRandomMusicIfReady [SoundManager] -- audio_music.c's timer
   (audio_music_timer.h's `audio_ready` parameter) freezes ticksBeforeMusic
   whenever this is false, exactly like Beta freezes it. True only once the
   shared backend device is open AND music.volume is currently nonzero. */
bool audio_music_backend_ready(void);

/* Start the ONE music voice on this PCM16-mono chunk (host byte order,
   audio_music.c already downmixed any stereo source -- the registry's own
   average-downmix rule). No positional range/pan model, and no PER-EVENT
   volume either: Beta's "BgMusic" source is never given a world position
   and its loudness is `sndSystem.setVolume("BgMusic", musicVolume)`
   directly, with no clamp/attenuation formula of its own
   [SoundManager.playRandomMusicIfReady / onSoundOptionsChanged] -- so
   unlike the one-shot audio_play3d/audio_play_ui, there is no `vol`
   parameter here: audio.c applies the live `music.volume` (via
   audio_set_music_volume) as this voice's ONLY gain, at start time and on
   every later slider drag. No-ops (does not start a voice) while
   music.volume is 0 or the backend is closed. */
void audio_voice_music_start(const int16_t* pcm, uint32_t frames,
							 float rate_hz);
/* MAIN-only conservative duration queued on the actual backend. */
uint32_t audio_voice_music_reserve_us(void);
/* Pacing-bug probe counters (2026-09-01, additive telemetry -- see audio.c):
   successful backend chunk starts vs wrapper-gate rejects. */
uint32_t audio_music_probe_starts(void);
uint32_t audio_music_probe_rejects(void);
uint32_t audio_music_probe_dur(void); /* Wii: last computed chunk deadline ms */
/* Poll: can the backend take another chunk right now? FALSE means "hand me
   the next one". The two backends reach that answer differently and the
   difference matters:
     - PC: the mixer callback owns a read cursor over the ONE chunk it was
       given, and flips false when that cursor reaches the end.
     - Wii: the chunk was COPIED into a looped hardware ring
       (audio_stream_ring.h); this answers "the ring is full" -- true while
       S-2 slots are still queued -- NOT "the last chunk finished
       sounding". Several chunks may still be queued behind a false.
   Either way, false means it is safe to overwrite the PCM memory the last
   start was given (see audio_music_ring.h's ordering note): PC is done
   reading it, Wii already copied it. */
bool audio_voice_music_playing(void);
/* Has the device played out EVERYTHING handed over -- the queued tail
   included? On PC that is just "not playing"; on the Wii's looped ring it
   is strictly later than audio_voice_music_playing() going false, because
   up to S-2 slots are still queued at that point. audio_music.c's EOF
   branch MUST gate the end-of-track stop on this rather than on
   !playing(), or the ring's queued tail (~0.7s) is cut off every track.
   True when no backend device is live (nothing can be queued). */
bool audio_voice_music_drained(void);
void audio_voice_music_stop(void);
/* The rate the device ACTUALLY plays at, or 0 for "hand me the file's own
   rate, I resample properly". Wii returns 48000: AESND's DSP mixer converts
   every voice to its fixed 48kHz output by NEAREST-NEIGHBOUR stepping (no
   interpolation at all, libaesnd's dspmixer.s), which a real-hardware LISTEN
   caught as thin/tinny distortion on 44.1kHz music -- so audio_music.c's
   decode worker pre-resamples to this rate (audio_resample.h) and the
   mixer's ratio becomes exactly 1.0. PC returns 0: miniaudio's own pipeline
   resamples properly and a second conversion would only lose quality.
   A platform constant, safe to call from any thread. */
uint32_t audio_voice_music_native_rate(void);

#endif
