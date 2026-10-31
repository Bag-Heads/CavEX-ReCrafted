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
	Wii audio backend: AESND (32 DSP voices, 2ms blocks; scout DECISIONS —
	NOT asnd, the two libs both own the DSP and are mutually exclusive).
	#included by platform/audio.c (the gfx.c pattern). Link: -laesnd
	(Makefile LIBS).

	NO CALLBACKS, BY DESIGN: AESND voice/audio callbacks run in audio-IRQ
	context every 2ms (aesndlib.h:38-41) — a hostile concurrency surface in
	an engine that just survived a corruption campaign (finish_line_plan.md
	§2b). Every voice is allocated with a NULL callback and completion is
	POLLED from audio_update on the main thread via a per-voice playback
	deadline (frames / playback-rate + one-block margin). aesndlib exposes
	no voice-state query, so the deadline IS the completion signal; a
	stolen slot simply restarts its hardware voice via AESND_PlayVoice.

	THE ONE EXCEPTION IS THE STREAMING-MUSIC VOICE (the S3 section at the
	bottom of this file): a deadline-per-chunk model measured ~0.69x wall
	clock on real hardware, so that voice is now a CONTINUOUSLY LOOPED ring
	refilled ahead of a time-based read-head estimate (audio_stream_ring.h).
	It is still callback-free and still polled from the main thread -- only
	the meaning of the poll changed, from "is the chunk done" to "is the
	ring full". See that section's banner.

	Buffers come from the shared PCM arena: 32-byte aligned per sample
	(audio_arena_alloc) and cache-flushed after load (audio_registry_load's
	DCFlushRange) — the DSP-DMA buffer rule mem2_alloc already guarantees
	for its base pointer.
*/

#include <aesndlib.h>
#include "../service_observer.h"
#include <malloc.h>
#include <ogc/cache.h>

/* aesnd unity gain: the voice mixer scales samples by volume/256
   ((sample * volume) >> 8); 255 is the customary full-scale of the
   devkitPro examples. Engine gains are always <= 1.0. */
#define WII_AUDIO_GAIN_SCALE 255.0F

/* Scheduling slop past the computed sample end before a ONE-SHOT voice is
   considered finished: several DSP blocks (2ms each) plus main-loop jitter
   headroom. Harmless for a fire-and-forget sound effect; it is precisely
   what made the old per-chunk music model run at ~0.69x, which is why the
   streaming voice no longer uses it at all. */
#define WII_AUDIO_TAIL_MS 40u

/* aesndlib.h documents the stream input ceiling as 144 kHz; an unclamped Beta
   pitch on a user-pack 44.1 kHz file could exceed it (review F5). Behavior
   past the ceiling is unspecified -- clamp, never exceed the DSP design max. */
#define WII_AUDIO_MAX_RATE_HZ 144000.0F

static AESNDPB* s_wii_pb[AUDIO_VOICES];
static ptime_t s_wii_start[AUDIO_VOICES];
static uint32_t s_wii_dur_ms[AUDIO_VOICES];
static bool s_wii_open = false;

/* S3: the ONE dedicated streaming-music voice -- the 17th of AESND's 32 hw
   voices (the other 15 stay unused headroom; AUDIO_VOICES=16 already
   reserves the sound-fx half). UNLIKE the one-shot voices above this one
   is a CONTINUOUSLY LOOPED voice over a multi-slot ring; see the section
   banner further down for why, and audio_stream_ring.h for the model. */
static AESNDPB* s_wii_music_pb = NULL;
static int16_t* s_wii_music_ring_buf = NULL;
static struct audio_stream_ring s_wii_music_ring;
/* Probe kept from the 2026-09-01 pacing hunt, re-pointed at the new model:
 * the nominal wall-clock ms of ONE ring slot (~186ms for 4096 frames at
 * 22050Hz, ~93ms at 44.1kHz). */
static uint32_t s_wii_music_last_dur;

/* MAIN owns this cursor; the worker keeps the source half READY until the
 * native publication completes. No lock survives a return. */
static uint32_t s_wii_audio_generation;
static struct {
	bool active;
	unsigned phase;
	size_t offset;
	const int16_t* pcm;
	uint32_t frames, rate, generation;
	float gain;
	struct audio_stream_ring next;
	struct audio_stream_ring_write write;
} s_wii_audio_cursor;

#define WII_MUSIC_RING_SAMPLES                                                 	((size_t)AUDIO_STREAM_RING_SLOTS * (size_t)AUDIO_STREAM_RING_SLOT_SAMPLES)
#define WII_MUSIC_RING_BYTES (WII_MUSIC_RING_SAMPLES * sizeof(int16_t))
#define WII_MUSIC_SLOT_BYTES                                                   	((size_t)AUDIO_STREAM_RING_SLOT_SAMPLES * sizeof(int16_t))

/* THE RING LENGTH MUST BE A WHOLE NUMBER OF DSP STREAM BLOCKS (2026-09-01,
   real-Wii LISTEN). libaesnd's Wii __aesndfillbuffer serves a streaming
   voice in DSP_STREAMBUFFER_SIZE (= 1152, aesndlib.h:8) byte blocks: when a
   request reaches mram_end it copies only what remains and ZERO-PADS the
   rest of the block, wrapping to mram_start on the NEXT request. So a ring
   whose byte length is not a multiple of 1152 injects that pad -- up to one
   partial block of silence -- ONCE PER LAP, which is an audible periodic
   click on continuous music. 9 slots x 8192 B = 73728 = 64 x 1152 exactly.
   This assert is the guard: changing AUDIO_STREAM_RING_SLOTS or the slot
   size to a non-multiple fails the build instead of shipping a click. */
_Static_assert(WII_MUSIC_RING_BYTES % (size_t)DSP_STREAMBUFFER_SIZE == 0,
			   "AESND stream ring must be a whole number of 1152-byte DSP "
			   "blocks or __aesndfillbuffer zero-pads once per lap");

/* Monotonic millisecond clock for the ring's read-head estimate. ptime_t is
   an unsigned 64-bit timebase counter and time_diff_ms() narrows to int32
   (~24 days), so rather than diffing against one fixed epoch this walks the
   reference forward by exactly the whole milliseconds it consumed -- the
   sub-millisecond remainder is carried, never dropped, and no long session
   can wrap it. */
static ptime_t s_wii_ms_ref;
static uint64_t s_wii_ms_acc = 0;

static uint64_t wii_music_now_ms(void) {
	ptime_t now = time_get();
	int32_t d = time_diff_ms(s_wii_ms_ref, now);
	if(d > 0) {
		s_wii_ms_acc += (uint64_t)d;
		s_wii_ms_ref = time_add_ms(s_wii_ms_ref, (unsigned int)d);
	}
	return s_wii_ms_acc;
}

static u16 wii_audio_gain_u16(float g) {
	if(g < 0.0F)
		g = 0.0F;
	if(g > 1.0F)
		g = 1.0F;
	return (u16)(g * WII_AUDIO_GAIN_SCALE);
}

static bool audio_backend_init(void) {
	s_wii_audio_generation++;
	s_wii_audio_cursor.active = false;
	AESND_Init();
	for(int k = 0; k < AUDIO_VOICES; k++) {
		s_wii_pb[k] = AESND_AllocateVoice(NULL); /* NULL: no IRQ callback */
		if(!s_wii_pb[k]) {
			for(int j = 0; j < k; j++) {
				AESND_FreeVoice(s_wii_pb[j]);
				s_wii_pb[j] = NULL;
			}
			return false;
		}
		s_wii_dur_ms[k] = 0;
	}
	/* S3: the 17th voice, dedicated to streaming music. */
	s_wii_music_pb = AESND_AllocateVoice(NULL);
	if(!s_wii_music_pb) {
		for(int j = 0; j < AUDIO_VOICES; j++) {
			AESND_FreeVoice(s_wii_pb[j]);
			s_wii_pb[j] = NULL;
		}
		return false;
	}
	/* The looped ring the DSP reads forever. 32-byte aligned: the same
	   DSP-DMA buffer rule the sound arena already honours. 48KB, allocated
	   ONCE here and freed in shutdown -- never per chunk. */
	if(!s_wii_music_ring_buf)
		s_wii_music_ring_buf = (int16_t*)memalign(32, WII_MUSIC_RING_BYTES);
	if(!s_wii_music_ring_buf) {
		AESND_FreeVoice(s_wii_music_pb);
		s_wii_music_pb = NULL;
		for(int j = 0; j < AUDIO_VOICES; j++) {
			AESND_FreeVoice(s_wii_pb[j]);
			s_wii_pb[j] = NULL;
		}
		return false;
	}
	memset(s_wii_music_ring_buf, 0, WII_MUSIC_RING_BYTES);
	audio_stream_ring_init(&s_wii_music_ring, AUDIO_STREAM_RING_SLOTS,
						   AUDIO_STREAM_RING_SLOT_SAMPLES);
	s_wii_ms_ref = time_get();
	s_wii_ms_acc = 0;
	s_wii_open = true;
	return true;
}

static void audio_backend_shutdown(void) {
	SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_SHUTDOWN, false, 0));
	if(!s_wii_open)
		return;
	s_wii_audio_generation++;
	s_wii_audio_cursor.active = false;
	for(int k = 0; k < AUDIO_VOICES; k++) {
		if(s_wii_pb[k]) {
			AESND_SetVoiceStop(s_wii_pb[k], true);
			AESND_FreeVoice(s_wii_pb[k]);
			s_wii_pb[k] = NULL;
		}
	}
	if(s_wii_music_pb) {
		AESND_SetVoiceStop(s_wii_music_pb, true);
		AESND_FreeVoice(s_wii_music_pb);
		s_wii_music_pb = NULL;
	}
	audio_stream_ring_stop(&s_wii_music_ring);
	/* Free AFTER the voice is stopped and freed: the DSP must never be
	   left pointing at released memory (the "stop, THEN rewrite" ordering
	   audio_registry_reload already relies on, taken one step further). */
	free(s_wii_music_ring_buf);
	s_wii_music_ring_buf = NULL;
	s_wii_open = false;
}

static void audio_backend_voice_start(int slot, const struct audio_sample* smp,
									  float rate_hz, float l, float r) {
	/* NaN-rejecting guard form (review F4): (rate <= 0) is FALSE for NaN and
	 * a NaN rate would poison the duration math below. */
	if(!s_wii_open || !s_wii_pb[slot] || !(rate_hz > 0.0F))
		return;
	if(rate_hz > WII_AUDIO_MAX_RATE_HZ)
		rate_hz = WII_AUDIO_MAX_RATE_HZ;
	AESND_SetVoiceVolume(s_wii_pb[slot], wii_audio_gain_u16(l),
						 wii_audio_gain_u16(r));
	/* One-shot: format VOICE_MONO16, per-voice float frequency carries the
	   unclamped Beta pitch (native_rate * pitch), no loop. */
	AESND_PlayVoice(s_wii_pb[slot], VOICE_MONO16, smp->pcm,
					smp->frames * sizeof(int16_t), rate_hz, 0, false);
	s_wii_start[slot] = time_get();
	s_wii_dur_ms[slot]
		= (uint32_t)((float)smp->frames * 1000.0F / rate_hz) + WII_AUDIO_TAIL_MS;
}

static void audio_backend_voice_gain(int slot, float l, float r) {
	if(!s_wii_open || !s_wii_pb[slot])
		return;
	AESND_SetVoiceVolume(s_wii_pb[slot], wii_audio_gain_u16(l),
						 wii_audio_gain_u16(r));
}

static void audio_backend_stop_all(void) {
	/* S1 reload drain: stop every hardware voice AND clear its poll
	   deadline, so audio_backend_voice_playing reports every slot free
	   immediately. The stop lands at the next 2ms DSP block — the reload
	   sleeps one block past this call before rewriting the arena
	   (audio_registry_reload), which the S0 steal-safety argument (the
	   arena is never freed) already made cosmetic-only. */
	if(!s_wii_open)
		return;
	for(int k = 0; k < AUDIO_VOICES; k++) {
		if(s_wii_pb[k])
			AESND_SetVoiceStop(s_wii_pb[k], true);
		s_wii_dur_ms[k] = 0;
	}
}

static bool audio_backend_voice_playing(int slot) {
	if(!s_wii_open || !s_wii_pb[slot] || s_wii_dur_ms[slot] == 0)
		return false;
	if(time_diff_ms(s_wii_start[slot], time_get())
	   < (int32_t)s_wii_dur_ms[slot])
		return true;
	/* Deadline passed: the non-looped voice has consumed its buffer (aesnd
	   parks it stopped); assert the stop as belt-and-braces and free the
	   slot for reuse. */
	AESND_SetVoiceStop(s_wii_pb[slot], true);
	s_wii_dur_ms[slot] = 0;
	return false;
}

/* ===== S3 streaming-music voice: a LOOPED AESND RING ====================
   THE MODEL (rewritten 2026-09-01; audio_stream_ring.h holds the pure
   bookkeeping and the long-form rationale).

   ONE AESND voice is started ONCE per track, LOOPED, over a 9-slot x 4096-
   frame int16 ring, and is never stopped between chunks. The DSP walks the
   ring continuously; the main thread copies each freshly decoded chunk
   (audio_music.c hands them over through audio.c's
   audio_voice_music_start) into a slot AHEAD of the read head. There is no
   per-chunk voice start, so there is no per-chunk restart latency and no
   tail margin -- the gap is gone by construction, not by tuning.

   WHY THE OLD MODEL WAS REPLACED. It armed a fresh ONE-SHOT voice per
   4096-frame chunk and declared it finished at frames/rate +
   WII_AUDIO_TAIL_MS (40ms). Every chunk boundary therefore cost >= 40ms of
   margin, plus up to a frame of poll slop, plus the 2ms-block latency of
   starting a new AESND voice (a fresh voice begins on the next block and
   skips one). Measured on real hardware that ran music at ~0.69x wall
   clock and sounded like "beeping to a melody". No margin tuning fixes a
   model that stops the device between every chunk.

   NO CALLBACKS, STILL. The file's standing rule is intact: this voice is
   allocated with a NULL callback like every other, and nothing here runs
   in audio-IRQ context. What replaces the completion deadline is the
   ring's TIME-BASED read-head estimate -- elapsed_ms * rate / 1000 samples
   since the PlayVoice -- because aesndlib exposes no voice-position query.

   PRIMING (2026-09-01, from the Dolphin verification of the first landing).
   Steady state was gapless -- 0 gaps after the first ~20s in-world -- but
   the first seconds produced underrun->restart clusters with 15-420ms gaps,
   because the voice began playing the instant chunk 1 landed and the
   world-load stall then starved slot 1. So the voice is NOT armed on the
   first chunk: chunks are accepted with the device SILENT and PlayVoice
   only happens once AUDIO_STREAM_RING_PRIME_SLOTS (=2) are queued. A silent
   device cannot underrun, so the stall is absorbed rather than heard. The
   ring also grew 6 -> 9 slots (48KB -> 72KB; F3 shows >10MB MEM1 free), so
   the guard now allows 7 queued slots, ~597ms at the 48kHz device rate the
   decode worker now pre-resamples to. The 9 is not arbitrary: see the
   DSP-block _Static_assert above.

   THE S-2 GUARD. audio_backend_music_playing() no longer means "the chunk
   is still sounding"; it means "the ring is FULL, do not hand me a chunk
   yet". It answers true while S-2 (=7) slots are queued, which keeps the
   write index at least one whole FREE slot behind the estimated head. The
   estimate runs EARLY by the 2-4ms a fresh AESND voice takes to actually
   begin (~190 samples at 48000Hz); one slot is 85ms at that rate, so the
   error cannot reach the block being read.

   UNDERRUN POLICY. If the main thread stalls long enough for the head to
   run past written data, a looped voice would replay STALE audio. The poll
   detects that (consumed_total >= written_total), STOPS the voice and
   clears the ring; the next chunk then RE-PRIMES from slot 0 (silent again
   until 2 slots are queued). Music resumes cleanly instead of stuttering
   the previous lap.

   DRAINED. Because playing() goes false while up to 7 slots are still
   queued, "the last chunk was handed over" is NOT "the track finished".
   audio_backend_music_drained() answers the latter (the head has passed
   every real frame written) and audio_music.c's EOF branch waits on it
   before stopping -- without that gate the ring would cut the last ~0.6s
   off every track. It is ALSO where a still-pending ring is armed, so a
   track too short to ever reach the prime threshold still plays.

   WHAT IS STILL UNVERIFIED. Dolphin confirmed the steady state gapless on
   the 6-slot, no-prime version; the priming and the wider ring are aimed at
   the world-load clusters it also showed, and are so far proven only as
   arithmetic (audio_stream_ring.h + tests/test_audio_music_env.c section J)
   and as a compile. Whether it is gapless out of a real Wii's speakers --
   and whether timebase-vs-DSP-clock drift stays inside the one-slot margin
   across a multi-minute track -- can only be settled by a real-hardware
   LISTEN. Until then this is a fix CANDIDATE, not a fix. */
/* The ONE place the looped voice is actually armed. Flushes the whole ring
   (not just the last slot) because the primed slots were written before the
   DSP was pointed at the buffer, then stamps the estimate's epoch to the
   PlayVoice itself -- the memset/memcpy/flush that preceded it are a few
   hundred microseconds the head has NOT played. */
static void wii_music_play_voice(void) {
	DCFlushRange(s_wii_music_ring_buf, WII_MUSIC_RING_BYTES);
	AESND_PlayVoice(s_wii_music_pb, VOICE_MONO16, s_wii_music_ring_buf,
					(u32)WII_MUSIC_RING_BYTES, (f32)s_wii_music_ring.rate, 0,
					/*looped=*/true);
	s_wii_music_ring.start_ms = wii_music_now_ms();
	SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_PRIME, true, audio_stream_ring_reserve_us(&s_wii_music_ring, s_wii_music_ring.start_ms)));
}

static void audio_backend_music_start(const int16_t* pcm, uint32_t frames,
									  float rate_hz, float gain) {
	/* Ordinary service may supersede incomplete, unpublished native work.
	 * The source half is still owned; the unchanged path copies it in full. */
	s_wii_audio_generation++;
	s_wii_audio_cursor.active = false;
	if(!s_wii_open || !s_wii_music_pb || !s_wii_music_ring_buf
	   || !(rate_hz > 0.0F) || frames == 0 || !pcm)
		return;
	if(rate_hz > WII_AUDIO_MAX_RATE_HZ)
		rate_hz = WII_AUDIO_MAX_RATE_HZ;
	/* A chunk is exactly one slot; audio_music.c never produces more (its
	   MUSIC_CHUNK_SAMPLES is _Static_asserted equal to the slot size), but
	   clamp rather than trust a caller with a memcpy length. */
	if(frames > AUDIO_STREAM_RING_SLOT_SAMPLES)
		frames = AUDIO_STREAM_RING_SLOT_SAMPLES;
	/* INTEGER rate, and the SAME integer handed to AESND: the read-head
	   estimate is only as good as its agreement with what the DSP actually
	   plays, so the two must not differ by a fractional Hz. Beta's music /
	   record files are integer-rate (22050/44100) anyway. */
	uint32_t rate = (uint32_t)rate_hz;
	if(rate == 0)
		rate = 1;

	uint64_t now = wii_music_now_ms();
	SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_SERVICE, s_wii_music_ring.running, audio_stream_ring_reserve_us(&s_wii_music_ring, now)));
	SERVICE_OBSERVER(if(s_wii_music_ring.running && s_wii_music_ring.rate != rate) service_observer_audio(SO_AUDIO_RATE_CHANGE, true, audio_stream_ring_reserve_us(&s_wii_music_ring, now)); else if(audio_stream_ring_underrun(&s_wii_music_ring, now)) service_observer_audio(SO_AUDIO_UNDERRUN, true, 0));
	struct audio_stream_ring_write w
		= audio_stream_ring_start_chunk(&s_wii_music_ring, now, rate, frames);

	if(w.reset_ring) {
		SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_RESET, false, 0));
		/* Stop FIRST: this rewrites slot 0 out from under whatever the old
		   voice was reading (the "stop, THEN rewrite" ordering
		   audio_registry_reload already relies on). */
		AESND_SetVoiceStop(s_wii_music_pb, true);
		memset(s_wii_music_ring_buf, 0, WII_MUSIC_RING_BYTES);
	}

	int16_t* slot = s_wii_music_ring_buf
		+ (size_t)w.slot * (size_t)AUDIO_STREAM_RING_SLOT_SAMPLES;
	memcpy(slot, pcm, (size_t)frames * sizeof(int16_t));
	if(frames < AUDIO_STREAM_RING_SLOT_SAMPLES)
		memset(slot + frames, 0,
			   ((size_t)AUDIO_STREAM_RING_SLOT_SAMPLES - (size_t)frames)
				   * sizeof(int16_t)); /* the short EOF chunk: pad with
										  silence so the loop can never
										  replay the previous lap's tail */
	/* Cache flush of the slot just written. aesndlib mixes on the PPC side,
	   so this is belt-and-braces rather than a hard DMA requirement the way
	   audio_registry_load's flush is -- but the ring is 32-byte aligned and
	   every slot is a whole multiple of a cache line, so the flush is free
	   of partial-line hazards and costs nothing worth saving. */
	DCFlushRange(slot, WII_MUSIC_SLOT_BYTES);

	u16 g = wii_audio_gain_u16(gain);
	AESND_SetVoiceVolume(s_wii_music_pb, g, g);

	if(w.start_voice)
		wii_music_play_voice();
	s_wii_music_last_dur = audio_stream_ring_slot_ms(&s_wii_music_ring);
}

static uint32_t audio_backend_music_last_dur_ms(void) {
	return s_wii_music_last_dur;
}

static uint32_t audio_backend_music_reserve_us(void) {
	return audio_stream_ring_reserve_us(&s_wii_music_ring, wii_music_now_ms());
}

/* The DSP's fixed output rate. AESND_Init programs the AI to
   AI_SAMPLERATE_48KHZ and its mixer resamples every voice to that by
   NEAREST-NEIGHBOUR stepping (dspmixer.s), which is what made 44.1kHz music
   sound tinny on hardware. Reporting the rate here lets audio_music.c's
   decode worker pre-resample properly (audio_resample.h) so the mixer's
   ratio is exactly 1.0 and it copies samples straight through. */
static uint32_t audio_backend_music_native_rate(void) {
	return (uint32_t)DSP_DEFAULT_FREQ; /* aesndlib.h: 48000 on HW_RVL */
}

static void audio_backend_music_gain(float gain) {
	if(!s_wii_open || !s_wii_music_pb)
		return;
	if(s_wii_audio_cursor.active) s_wii_audio_cursor.gain = gain;
	u16 g = wii_audio_gain_u16(gain);
	AESND_SetVoiceVolume(s_wii_music_pb, g, g);
}

/* "The ring has NO free slot -- do not hand me a chunk yet." (NOT "the last
   chunk is still sounding"; see the banner.) Also the one place the
   underrun policy is enforced, because this is what the engine polls every
   frame while a track is active. */
static bool audio_backend_music_playing(void) {
	SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_SERVICE, s_wii_music_ring.running, audio_stream_ring_reserve_us(&s_wii_music_ring, wii_music_now_ms())));
	if(!s_wii_open || !s_wii_music_pb || !s_wii_music_ring.running)
		return false;
	uint64_t now = wii_music_now_ms();
	if(audio_stream_ring_underrun(&s_wii_music_ring, now)) {
		SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_UNDERRUN, true, 0));
		AESND_SetVoiceStop(s_wii_music_pb, true);
		audio_stream_ring_stop(&s_wii_music_ring);
		SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_STOP, false, 0));
		return false;
	}
	return !audio_stream_ring_has_free_slot(&s_wii_music_ring, now);
}

/* Has the read head passed every REAL frame ever handed over?
   Also the END-OF-TRACK flush: the engine only asks this once it has fed
   the last chunk, so a ring still sitting BELOW the prime threshold (a
   one-chunk record, or a track whose final decode returned 0 frames) is
   armed here rather than left silent forever. See
   audio_stream_ring_flush_pending's note for why this, and not a
   `frames < slot_samples` heuristic, is the single mechanism. */
static bool audio_backend_music_drained(void) {
	SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_SERVICE, s_wii_music_ring.running, audio_stream_ring_reserve_us(&s_wii_music_ring, wii_music_now_ms())));
	if(!s_wii_open || !s_wii_music_pb)
		return true;
	if(audio_stream_ring_flush_pending(&s_wii_music_ring, wii_music_now_ms()))
		wii_music_play_voice();
	return audio_stream_ring_drained(&s_wii_music_ring, wii_music_now_ms());
}

static void audio_backend_music_stop(void) {
	SERVICE_OBSERVER(service_observer_audio(SO_AUDIO_TRACK_STOP, false, 0));
	if(!s_wii_open || !s_wii_music_pb)
		return;
	s_wii_audio_generation++;
	s_wii_audio_cursor.active = false;
	AESND_SetVoiceStop(s_wii_music_pb, true);
	audio_stream_ring_stop(&s_wii_music_ring);
}

/* Finite copy/zero/flush units are <=256 B (eight aligned cache lines for
 * flush). DCFlushRange includes its completion sync on EVERY unit. AESND
 * setters/PlayVoice have fixed field work, no callbacks/mailbox polling.
 * Bookkeeping publishes only after all cache/voice operations complete. */
static enum audio_service_result audio_backend_music_service(
	const int16_t* pcm, uint32_t frames, uint32_t rate, float gain,
	struct audio_service_budget* b) {
	if(!s_wii_open || !s_wii_music_pb || !s_wii_music_ring_buf
		|| frames > AUDIO_STREAM_RING_SLOT_SAMPLES || !rate || rate > 144000u)
		return AUDIO_SERVICE_UNSAFE;
	if(!frames && pcm) return AUDIO_SERVICE_DONE; /* exact-multiple EOF: no slot */
	if(!pcm && frames) return AUDIO_SERVICE_UNSAFE;
	if(!s_wii_audio_cursor.active) {
		enum audio_service_result r = audio_service_enter(b, AUDIO_UNIT_STATE);
		if(r != AUDIO_SERVICE_DONE) return r;
		ptime_t entered = time_get();
		if(pcm && !audio_stream_ring_has_free_slot(&s_wii_music_ring, wii_music_now_ms())) {
			bool ok = audio_service_leave(b, AUDIO_UNIT_STATE, entered);
			return ok ? AUDIO_SERVICE_MORE : AUDIO_SERVICE_UNSAFE;
		}
		audio_service_cover(b, AUDIO_UNIT_STATE, sizeof(struct audio_stream_ring), 1);
		s_wii_audio_cursor.pcm = pcm;
		s_wii_audio_cursor.frames = frames;
		s_wii_audio_cursor.rate = rate;
		s_wii_audio_cursor.gain = gain;
		s_wii_audio_cursor.generation = s_wii_audio_generation;
		s_wii_audio_cursor.next = s_wii_music_ring;
		if(pcm) s_wii_audio_cursor.write = audio_stream_ring_start_chunk(
			&s_wii_audio_cursor.next, wii_music_now_ms(), rate, frames);
		else {
			s_wii_audio_cursor.write = (struct audio_stream_ring_write){0};
			s_wii_audio_cursor.write.start_voice = audio_stream_ring_flush_pending(
				&s_wii_audio_cursor.next, wii_music_now_ms());
		}
		s_wii_audio_cursor.offset = 0;
		s_wii_audio_cursor.phase = pcm
			? (s_wii_audio_cursor.write.reset_ring ? 0 : 2) : 6;
		s_wii_audio_cursor.active = true;
		if(!audio_service_leave(b, AUDIO_UNIT_STATE, entered)) return AUDIO_SERVICE_UNSAFE;
	}
	if(s_wii_audio_cursor.generation != s_wii_audio_generation
		|| pcm != s_wii_audio_cursor.pcm || frames != s_wii_audio_cursor.frames
		|| rate != s_wii_audio_cursor.rate) return AUDIO_SERVICE_UNSAFE;
	int16_t* slot = s_wii_music_ring_buf
		+ (size_t)s_wii_audio_cursor.write.slot * AUDIO_STREAM_RING_SLOT_SAMPLES;
	for(unsigned steps = 0; steps < 700; steps++) {
		unsigned phase = s_wii_audio_cursor.phase;
		if(phase == 8) {
			s_wii_audio_cursor.active = false;
			return AUDIO_SERVICE_DONE;
		}
		/* A suspended live write cannot outlast the queued audio protecting
		 * its slot. Refuse before touching a slot the device may now read.
		 * Restart/reset/ordinary escape keeps the original owned PCM. */
		if(!s_wii_audio_cursor.write.reset_ring && s_wii_music_ring.running
			&& audio_stream_ring_underrun(&s_wii_music_ring, wii_music_now_ms())) {
			if(!b->ordinary) return AUDIO_SERVICE_UNSAFE;
			/* Unpublished PCM is still owned by the same engine half. The
			 * unchanged ordinary restart resets the now-expired destination,
			 * rather than wedging forever on an invalid live write cursor. */
			audio_backend_music_start(pcm, frames, (float)rate, gain);
			return AUDIO_SERVICE_DONE;
		}
		enum audio_service_unit unit = AUDIO_UNIT_STATE;
		if(phase == 0) unit = AUDIO_UNIT_STOP;
		if(phase == 1 || phase == 3) unit = AUDIO_UNIT_ZERO;
		if(phase == 2) unit = AUDIO_UNIT_COPY;
		if(phase == 4 || phase == 6) unit = AUDIO_UNIT_FLUSH;
		if(phase == 5) unit = AUDIO_UNIT_GAIN;
		if(phase == 7) unit = AUDIO_UNIT_START;
		enum audio_service_result r = audio_service_enter(b, unit);
		if(r != AUDIO_SERVICE_DONE) return r;
		ptime_t entered = time_get();
		size_t offset = s_wii_audio_cursor.offset;
		size_t end = phase == 1 || phase == 6 ? WII_MUSIC_RING_BYTES
			: phase == 2 ? (size_t)frames * sizeof(int16_t) : WII_MUSIC_SLOT_BYTES;
		size_t n = end-offset;
		if(n > AUDIO_SERVICE_BYTES) n = AUDIO_SERVICE_BYTES;
		audio_service_cover(b, unit, phase >= 1 && phase <= 4 ? (uint32_t)n
			: phase == 6 ? (uint32_t)n : 0,
			phase == 6 ? (pcm ? 2u : 4u) : 1u);
		switch(phase) {
			case 0:
				AESND_SetVoiceStop(s_wii_music_pb, true);
				s_wii_audio_cursor.phase = 1;
				break;
			case 1:
				memset((char*)s_wii_music_ring_buf + offset, 0, n);
				s_wii_audio_cursor.offset += n;
				if(offset+n == end) { s_wii_audio_cursor.phase = 2; s_wii_audio_cursor.offset = 0; }
				break;
			case 2:
				memcpy((char*)slot + offset, (const char*)pcm + offset, n);
				s_wii_audio_cursor.offset += n;
				if(offset+n == end) s_wii_audio_cursor.phase = 3;
				break;
			case 3:
				memset((char*)slot + offset, 0, n);
				s_wii_audio_cursor.offset += n;
				if(offset+n == end) { s_wii_audio_cursor.phase = 4; s_wii_audio_cursor.offset = 0; }
				break;
			case 4: case 6:
				DCFlushRange((char*)(phase == 4 ? slot : s_wii_music_ring_buf) + offset,
					(u32)n);
				s_wii_audio_cursor.offset += n;
				if(offset+n == end) { s_wii_audio_cursor.phase = phase+1; s_wii_audio_cursor.offset = 0; }
				break;
			case 5: {
				u16 g = wii_audio_gain_u16(s_wii_audio_cursor.gain);
				AESND_SetVoiceVolume(s_wii_music_pb, g, g);
				s_wii_audio_cursor.phase = s_wii_audio_cursor.write.start_voice ? 6 : 8;
				break;
			}
			case 7:
				AESND_PlayVoice(s_wii_music_pb, VOICE_MONO16, s_wii_music_ring_buf,
					(u32)WII_MUSIC_RING_BYTES, (f32)rate, 0, true);
				s_wii_audio_cursor.next.start_ms = wii_music_now_ms();
				s_wii_audio_cursor.phase = 8;
				break;
		}
		if(s_wii_audio_cursor.phase == 8) {
			s_wii_music_ring = s_wii_audio_cursor.next;
			s_wii_music_last_dur = audio_stream_ring_slot_ms(&s_wii_music_ring);
			s_wii_audio_cursor.generation = ++s_wii_audio_generation;
		}
		/* Commit progress BEFORE the post timer check: an observed overrun
		 * invalidates admission, but repeating entry cannot duplicate work. */
		if(!audio_service_leave(b, unit, entered)) return AUDIO_SERVICE_UNSAFE;
	}
	return AUDIO_SERVICE_MORE; /* structural loop limit, even a frozen clock */
}

static enum audio_service_result audio_backend_music_poll_service(bool eof,
	bool* value, struct audio_service_budget* b) {
	*value = false;
	if(!s_wii_open || !s_wii_music_pb) return AUDIO_SERVICE_UNSAFE;
	if(s_wii_audio_cursor.active || (eof && !s_wii_music_ring.running
		&& s_wii_music_ring.pending_slots)) {
		if(!eof || (s_wii_audio_cursor.active && s_wii_audio_cursor.pcm))
			return AUDIO_SERVICE_MORE;
		enum audio_service_result r = audio_backend_music_service(NULL, 0,
			s_wii_music_ring.rate, 0, b);
		if(r != AUDIO_SERVICE_DONE) return r;
	}
	enum audio_service_result r = audio_service_enter(b, AUDIO_UNIT_STOP);
	if(r != AUDIO_SERVICE_DONE) return r;
	ptime_t entered = time_get();
	uint64_t now = wii_music_now_ms();
	if(!eof && audio_stream_ring_underrun(&s_wii_music_ring, now)) {
		AESND_SetVoiceStop(s_wii_music_pb, true);
		audio_stream_ring_stop(&s_wii_music_ring);
		s_wii_audio_generation++;
	}
	*value = eof ? audio_stream_ring_drained(&s_wii_music_ring, now)
		: s_wii_music_ring.running && !audio_stream_ring_has_free_slot(&s_wii_music_ring, now);
	return audio_service_leave(b, AUDIO_UNIT_STOP, entered)
		? AUDIO_SERVICE_DONE : AUDIO_SERVICE_UNSAFE;
}
