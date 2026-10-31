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

#ifndef PLATFORM_AUDIO_DEFERRED_H
#define PLATFORM_AUDIO_DEFERRED_H

/*
	C2 P2 (Astra ruling 3, 2026-09-19): the DEFERRED ONE-SHOT loader.

	SHIPPED-DEFAULT T2 deviation (config `audio.deferred_cave`, default ON
	since 2026-10-21; "off" = the resident arena path, unchanged). When on, a DEFERRED sound key
	(ambient.cave.cave, ~4.4MB of PCM that overflows the 12MB arena) is not
	held resident: the variant the pick chose is loaded from the card ON DEMAND
	into ONE bounded scratch buffer, and only once the WHOLE sample is resident
	is it started through the ordinary one-shot voice path in audio.c. So the
	voice never depends on SD delivery while it plays -- the real-time-feed
	failure class of the music ring (the 2026-09-01 "beeping") cannot occur
	here by construction -- and volume, 3D gain/pan and the unclamped Beta pitch
	are inherited from the one-shot path rather than re-implemented.

	What deviates from Beta: the audible start lags the trigger by the load
	time. Position, volume, pitch, the variant roll and the trigger-time
	distance cull are all decided at the trigger, exactly as before; no RNG draw
	is added or moved.

	THREADING CONTRACT
	  main thread  : every function here except audio_deferred_loader_step.
	  loader thread: audio_deferred_loader_step only (priority 40: above the
	                 mesher's 32, below music/region-io's 48, and unique --
	                 equal-priority LWPs never preempt each other).
	One mutex guards the state record. It is NEVER held across file I/O or a
	decode call; the scratch buffer is written by the loader only while the
	state is LOADING and read by a voice only while it is PLAYING, with a drain
	margin (DRAINING) between a voice letting go and the next write.

	Every accepted or refused request ends in exactly ONE terminal outcome.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Nonblocking snapshot. Only IDLE/UNAVAILABLE qualify for bounded voice
 * polling; all deferred completion/cancel/drain paths escape ordinary. */
bool audio_deferred_service_idle(void);

/* Per-variant ceiling: a deferred variant whose PCM is larger than this is
   never eligible (counted, never truncated). 1MiB = 16.4s at 32kHz mono. */
#define AUDIO_DEFERRED_MAX_BYTES (1024u * 1024u)
/* PCM bytes moved per loader step (one fread / one decode burst). */
#define AUDIO_DEFERRED_CHUNK_BYTES (16u * 1024u)
/* Real sleep between loader steps: bounds the loader's CPU share. */
#define AUDIO_DEFERRED_STEP_SLEEP_MS 2
/* A released voice may still be read by the DSP until the next 2ms AESND
   block (audio_registry_reload's 3ms margin); the scratch is not rewritten
   until this long after the release. */
#define AUDIO_DEFERRED_DRAIN_MS 10
/* The scratch must be DSP-DMA friendly on the Wii. */
#define AUDIO_DEFERRED_ALIGN 32u
/* OGG variants only: stb_vorbis's setup/codebook/temp state comes out of ONE
   block sized at scan and carved from the arena beside the scratch -- the
   loader never mallocs decoder state during gameplay. A variant that would
   need more than the ceiling is not eligible (counted with the oversize
   ones). The slack covers stb_vorbis rounding each carve up to 8 bytes. */
#define AUDIO_DEFERRED_MAX_DECODER_BYTES (512u * 1024u)
#define AUDIO_DEFERRED_DECODER_SLACK 4096u

enum audio_deferred_state {
	AUDIO_DEFERRED_UNAVAILABLE = 0, /* off, no scratch, or no loader thread */
	AUDIO_DEFERRED_IDLE,
	AUDIO_DEFERRED_REQUESTED, /* queued, loader not started on it yet */
	AUDIO_DEFERRED_LOADING,	  /* the loader owns the scratch */
	AUDIO_DEFERRED_READY,	  /* whole sample resident, waiting for main */
	AUDIO_DEFERRED_PLAYING,	  /* a voice reads the scratch */
	AUDIO_DEFERRED_DRAINING	  /* voice let go; margin before reuse */
};

enum audio_deferred_outcome {
	AUDIO_DEFERRED_OUT_NONE = 0, /* request still in flight */
	AUDIO_DEFERRED_OUT_BUSY,	 /* refused: another request in flight */
	AUDIO_DEFERRED_OUT_CULLED,	 /* loaded, not started: muted/backend dead */
	AUDIO_DEFERRED_OUT_FAILED,	 /* open/probe/read/decode/size failure */
	AUDIO_DEFERRED_OUT_CANCELLED, /* unload/stop/reload before or during play */
	AUDIO_DEFERRED_OUT_STOLEN,	 /* started, slot stolen before the end */
	AUDIO_DEFERRED_OUT_COMPLETED, /* started and played to its end */
	AUDIO_DEFERRED_OUT_COUNT
};

/* Where one deferred variant lives on the card, from the scan-time probe. */
struct audio_deferred_source {
	char path[256];
	bool is_ogg;
	uint32_t frames; /* probe-time values; the load re-probes */
	uint32_t rate;
};

/* What audio.c needs to start the voice once the sample is resident. */
struct audio_deferred_ready {
	uint32_t req_id;
	uint16_t sound_id;
	int variant;
	const int16_t* pcm; /* == the scratch */
	uint32_t frames;	/* ACTUAL loaded values */
	uint32_t rate;
	float pos[3];
	float vol;
	float pitch;
};

/* Terminal record of one request (diagnostics + tests). */
struct audio_deferred_record {
	uint32_t req_id;
	uint32_t epoch;
	uint16_t sound_id;
	int variant;
	uint32_t bytes;
	uint32_t frames;
	uint32_t rate;
	int32_t load_ms;		/* -1 = never finished loading */
	int32_t start_delay_ms; /* request -> voice start; -1 = never started */
	bool started;
	enum audio_deferred_outcome outcome;
};

struct audio_deferred_counters {
	uint32_t requests; /* every call to audio_deferred_request */
	uint32_t outcome[AUDIO_DEFERRED_OUT_COUNT];
	int32_t max_load_ms;
	int32_t max_step_ms; /* longest single uninterrupted loader step */
};

/* Hand the module its scratch (from the sound arena; `scratch` must be
   AUDIO_DEFERRED_ALIGN-aligned, `capacity_bytes` a multiple of it) and bring
   up the loader thread on first use. NULL/0 or a thread-creation failure
   leaves the module UNAVAILABLE (reported, recoverable at the next call).
   Requires the module to be quiescent (see audio_deferred_quiesce). */
bool audio_deferred_attach(int16_t* scratch, size_t capacity_bytes);

/* The decoder-state block for OGG loads (see AUDIO_DEFERRED_MAX_DECODER_BYTES).
   Takes effect only while IDLE; NULL/0 = none, and an ogg request then ends
   FAILED. Forgotten by audio_deferred_quiesce, like the scratch. */
void audio_deferred_set_decoder_memory(void* block, size_t bytes);

/* Ask for one variant to be loaded and then played. Returns true when the
   request was accepted (state was IDLE); otherwise the NEW request ends at
   once as BUSY (or FAILED when UNAVAILABLE / `src` is NULL) and the one in
   flight is untouched. */
bool audio_deferred_request(const struct audio_deferred_source* src,
							uint16_t sound_id, int variant, const float pos[3],
							float vol, float pitch);

/* Per-frame, main thread: advances DRAINING -> IDLE once the margin elapsed.
   Returns true exactly once per loaded request, handing over what to start;
   the caller MUST answer with audio_deferred_note_started or
   audio_deferred_note_not_started before the next call. */
bool audio_deferred_take_ready(struct audio_deferred_ready* out);
void audio_deferred_note_started(uint32_t req_id, int slot);
void audio_deferred_note_not_started(uint32_t req_id,
									 enum audio_deferred_outcome why);

/* audio.c tells the module that voice `slot` stopped reading its sample:
   COMPLETED (reaped by the poll), STOLEN (audio_take_slot) or CANCELLED
   (audio_stop_all). A no-op unless `slot` is the deferred voice. */
void audio_deferred_note_voice_released(int slot,
										enum audio_deferred_outcome why);
/* The slot the deferred voice occupies, or -1. */
int audio_deferred_voice_slot(void);

/* Cancel whatever is REQUESTED/LOADING/READY (world left, stop-all). Never
   waits. The request is CANCELLED and the state IDLE on return, except when
   the cancel lands while the loader is INSIDE a step: then the state stays
   LOADING (new requests are BUSY) until that step's own epilogue ends it. */
void audio_deferred_cancel(void);

/* Cancel AND wait until the loader is outside any step, so the caller may
   rewrite the arena the scratch lives in. The wait is as long as one
   outstanding file read -- callers are the reload and the shutdown, which
   already block. Leaves the module UNAVAILABLE until the next attach. */
void audio_deferred_quiesce(void);

/* Stop the loader thread for good (audio_shutdown). */
void audio_deferred_shutdown(void);

enum audio_deferred_state audio_deferred_state(void);
void audio_deferred_counters(struct audio_deferred_counters* out);

/* Drain the oldest undelivered terminal record (observer telemetry; bounded
   ring, oldest dropped). Returns false when none is pending. */
bool audio_deferred_pop_record(struct audio_deferred_record* out);

/* LOADER THREAD (or a test driving it by hand): do one bounded unit of work.
   Returns true when more work is pending for the current job. */
bool audio_deferred_loader_step(void);

/* Scan-time probe: does `path` hold a loadable PCM16-mono WAV / a mono or
   stereo Ogg Vorbis, and how much PCM is it? No PCM is decoded or kept. */
bool audio_deferred_probe(const char* path, bool is_ogg, uint32_t* frames,
						  uint32_t* rate);
/* As above, and for an OGG also how large a decoder-state block a load of
   THIS file needs (0 for a WAV). `decoder_bytes` may be NULL. */
bool audio_deferred_probe_ex(const char* path, bool is_ogg, uint32_t* frames,
							 uint32_t* rate, size_t* decoder_bytes);

#ifdef CAVEWII_TESTING
/* Tests drive the loader by hand: no thread is ever spawned. */
void audio_deferred_test_reset(void);
void audio_deferred_test_fail_thread(bool fail);
/* Fake millisecond clock for the DRAINING margin and the *_ms fields;
   negative = use the real clock. */
void audio_deferred_test_set_clock_ms(int64_t ms);
/* Called INSIDE every loader step, after the step's I/O and before its
   epilogue, with the lock free and the step marked in progress: lets a
   single-threaded test act as the main thread racing a running step. */
void audio_deferred_test_set_step_hook(void (*hook)(void));
#endif

#endif
