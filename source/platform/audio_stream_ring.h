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

#ifndef PLATFORM_AUDIO_STREAM_RING_H
#define PLATFORM_AUDIO_STREAM_RING_H

#include <stdbool.h>
#include <stdint.h>

/*
	PURE bookkeeping for a CONTINUOUSLY LOOPING streaming-audio ring: a
	fixed array of S equally-sized slots that the playback device reads
	end-to-end and wraps around forever, refilled AHEAD of the device's
	read head by the main thread. No platform types, no device calls, no
	clock of its own -- every query takes the caller's millisecond
	timestamp. The Wii backend (source/platform/wii/audio.c) owns the real
	int16 buffer, the AESND calls and time_get(); this header owns every
	DECISION the two make, so the state machine is testable without a DSP
	(the audio_music_timer.h / audio_music_ring.h precedent).

	WHY A LOOPED RING AT ALL (2026-09-01). The previous Wii music backend
	started each 4096-frame chunk as its own ONE-SHOT AESND voice and only
	reported it finished after frames/rate + a 40ms tail margin. Every
	chunk therefore ended in >= 40ms of margin plus a frame of poll slop
	plus the 2ms-block restart latency of a fresh voice: measured on real
	hardware, music advanced at ~0.69x wall clock and sounded like
	"beeping to a melody". A looped voice never stops between chunks, so
	there is no restart latency and no tail margin to pay -- the gap
	disappears by construction rather than by tuning a margin down.

	WHY A TIME-BASED READ HEAD. aesndlib exposes no voice-position query,
	and this file's whole platform layer is CALLBACK-FREE BY DESIGN
	(wii/audio.c's top-of-file rule: an AESND callback runs in audio-IRQ
	context every 2ms). The only honest read-head estimate left is the
	arithmetic one: a voice started at `start_ms` and playing at `rate`
	samples/second has consumed (now - start) * rate / 1000 samples. That
	estimate is EARLY by however long AESND takes to actually begin (a
	fresh voice starts on the next 2ms DSP block and skips one block, so
	2-4ms ~= 90 samples at 22050Hz) and then drifts only by the difference
	between the system timebase and the DSP's 48kHz clock.

	WHY THE S-2 GUARD. `has_free_slot` refuses to hand over a chunk once
	S-2 slots are already queued, so a write never lands closer than one
	whole FREE slot behind the estimated head. That converts the few-ms
	estimate error above into a >= 1 slot (186ms at 22050Hz) safety
	margin: even if the estimate is a whole slot early, the write index
	still trails the true head by a slot and can neither tear the block
	being read nor clobber a queued-but-unplayed one.

	WHY PRIMING (2026-09-01, from the Dolphin verification of the first
	landing). The 6-slot ring was gapless in steady state -- 0 gaps after
	the first ~20s in-world -- but the first seconds produced
	underrun->restart clusters with gaps of 15-420ms: the voice began
	playing the moment chunk 1 landed, and the world-load stall then kept
	the decoder from filling slot 1 inside one slot-duration. The fix is
	not a bigger margin but a LATER start: the ring accepts chunks with the
	device SILENT and only calls PlayVoice once AUDIO_STREAM_RING_PRIME_SLOTS
	are queued. A silent device cannot underrun, so the stall is absorbed
	instead of heard. The same priming applies after an underrun restart --
	resuming off a single slot is what made those clusters repeat.

	UNDERRUN POLICY. If the estimated head reaches data that was never
	written (the main thread stalled -- a long chunk load, a world save),
	the device would loop and replay STALE audio. `underrun()` reports
	that; the backend's answer is to stop the voice and restart fresh from
	slot 0 on the next chunk, i.e. music resumes cleanly rather than
	stuttering old audio. `start_chunk` folds the same check in, so a
	caller that never polls still cannot write into the slot the head sits
	on.

	NOT PROVEN HERE: that the resulting audio is gapless on real silicon.
	This header proves the bookkeeping; only a real-Wii LISTEN can prove
	the sound. Everything below is arithmetic over uint64 milliseconds and
	sample counts -- no floats, so the estimate is bit-identical on PPC and
	on the test host.
*/

/* The ring geometry. SLOT_SAMPLES is THE chunk size for streaming music:
   audio_music.c's MUSIC_CHUNK_SAMPLES must equal it (that file
   _Static_asserts the lockstep), because the engine hands this backend
   exactly one decoded chunk per slot. 9 slots x 4096 mono frames = 72KB of
   int16, of which the S-2 guard keeps at most 7 filled. Production feeds
   this at the Wii device rate of 48000Hz (audio_music.c pre-resamples --
   see audio_resample.h), so a slot is ~85ms and the queue is ~597ms deep.

   WHY EXACTLY NINE, not 8 (2026-09-01, real-Wii LISTEN). libaesnd's Wii
   __aesndfillbuffer serves a streaming voice in DSP_STREAMBUFFER_SIZE
   (1152-byte, aesndlib.h) blocks: on reaching the buffer end it copies what
   remains and ZERO-PADS the rest of the block, only wrapping to the start
   on the NEXT request. A ring whose byte length is not a whole multiple of
   1152 therefore injects up to one partial block of silence PER LAP -- at 8
   slots, 65536 = 56*1152 + 1024, i.e. 64 samples of silence every lap, an
   audible periodic click. 9 slots is 73728 = 64*1152 exactly, so the wrap
   never lands mid-block. wii/audio.c _Static_asserts that. Widened 6 -> 8
   after Dolphin showed the world-load stall exhausting a 4-slot queue; F3
   reports >10MB MEM1 free, so neither step is a budget question. */
#define AUDIO_STREAM_RING_SLOTS 9u
#define AUDIO_STREAM_RING_SLOT_SAMPLES 4096u
/* Slots deliberately left unwritten between the write index and the
   estimated read head -- one for the head's own slot, one for the
   estimate error described above. */
#define AUDIO_STREAM_RING_GUARD_SLOTS 2u
/* PRIMING (2026-09-01, from the Dolphin verification of the 6-slot ring):
   steady state was gapless, but the first ~20s in-world produced
   underrun->restart clusters (15-420ms) because the voice began playing the
   instant chunk 1 landed, while the world-load stall kept the decoder from
   filling slot 1 in time. So the voice is NOT started on the first chunk:
   the ring accepts chunks with the device silent and only calls PlayVoice
   once this many slots are queued, buying a whole slot of head start before
   a single sample is played. Same rule after an underrun restart. */
#define AUDIO_STREAM_RING_PRIME_SLOTS 2u

struct audio_stream_ring {
	uint32_t slots;		   /* S */
	uint32_t slot_samples; /* frames per slot */
	uint32_t rate;		   /* Hz of the current track; 0 = nothing accepted */
	uint64_t start_ms;	   /* caller clock at the PlayVoice of the live voice */
	uint64_t written_slots;	  /* slots handed over since that start */
	uint64_t written_samples; /* REAL frames handed over (short final chunk
								 counts its real length, not the zero pad) */
	bool running;			  /* a looped voice is live right now */
	/* PENDING (pre-start) state: chunks already copied into slots
	   0..pending_slots-1 with the device still silent. Mutually exclusive
	   with `running` -- the transition zeroes these and seeds written_*. */
	uint64_t pending_slots;
	uint64_t pending_samples;
};

/* Where a chunk goes, and what the caller must do to the hardware voice
   around copying it in. The two flags are independent now that the ring
   primes: a first chunk after a stop/underrun sets reset_ring (stop the
   voice, zero the buffer) but NOT start_voice; the chunk that crosses the
   prime threshold sets start_voice (PlayVoice, then stamp start_ms) but
   not reset_ring. */
struct audio_stream_ring_write {
	uint32_t slot;
	bool reset_ring;
	bool start_voice;
};

static inline void audio_stream_ring_init(struct audio_stream_ring* r,
										  uint32_t slots,
										  uint32_t slot_samples) {
	r->slots = slots;
	r->slot_samples = slot_samples;
	r->rate = 0;
	r->start_ms = 0;
	r->written_slots = 0;
	r->written_samples = 0;
	r->running = false;
	r->pending_slots = 0;
	r->pending_samples = 0;
}

static inline void audio_stream_ring_stop(struct audio_stream_ring* r) {
	r->running = false;
	r->written_slots = 0;
	r->written_samples = 0;
	r->pending_slots = 0;
	r->pending_samples = 0;
}

/* The time-based read head, in samples since the live voice started. Zero
   while PENDING: nothing has been played yet, by construction. */
static inline uint64_t
audio_stream_ring_consumed_samples(const struct audio_stream_ring* r,
								   uint64_t now_ms) {
	if(!r->running || r->rate == 0 || now_ms <= r->start_ms)
		return 0;
	return ((now_ms - r->start_ms) * (uint64_t)r->rate) / UINT64_C(1000);
}

/* ...expressed in WHOLE slots fully passed (== the slot the head is in). */
static inline uint64_t
audio_stream_ring_consumed_total(const struct audio_stream_ring* r,
								 uint64_t now_ms) {
	if(r->slot_samples == 0)
		return 0;
	return audio_stream_ring_consumed_samples(r, now_ms)
		/ (uint64_t)r->slot_samples;
}

/* Which physical slot the head is reading right now (test/diagnostic). */
static inline uint32_t
audio_stream_ring_head_slot(const struct audio_stream_ring* r,
							uint64_t now_ms) {
	if(r->slots == 0)
		return 0;
	return (uint32_t)(audio_stream_ring_consumed_total(r, now_ms)
					  % (uint64_t)r->slots);
}

/* The head has run past everything ever handed over: the device is about
   to (or already did) replay stale slots. NEVER true while stopped or
   PENDING -- a silent device cannot outrun anything, which is precisely
   what priming buys at world-load time. */
static inline bool audio_stream_ring_underrun(const struct audio_stream_ring* r,
											  uint64_t now_ms) {
	if(!r->running)
		return false;
	return audio_stream_ring_consumed_total(r, now_ms) >= r->written_slots;
}

/* Queued-but-not-yet-played slots. While PENDING every accepted chunk is
   queued (none can have played). Saturates at 0 on an underrun. */
static inline uint64_t
audio_stream_ring_filled(const struct audio_stream_ring* r, uint64_t now_ms) {
	if(!r->running)
		return r->pending_slots;
	uint64_t consumed = audio_stream_ring_consumed_total(r, now_ms);
	if(consumed >= r->written_slots)
		return 0;
	return r->written_slots - consumed;
}

/* "Hand me a chunk now?" -- false exactly while S-2 slots are already
   queued. While PENDING the answer stays yes until the ring is physically
   full: priming exists precisely to get chunks in fast. */
static inline bool
audio_stream_ring_has_free_slot(const struct audio_stream_ring* r,
								uint64_t now_ms) {
	if(r->slots <= AUDIO_STREAM_RING_GUARD_SLOTS)
		return false; /* degenerate geometry: never claim room */
	if(!r->running)
		return r->pending_slots < (uint64_t)r->slots;
	return audio_stream_ring_filled(r, now_ms)
		< (uint64_t)(r->slots - AUDIO_STREAM_RING_GUARD_SLOTS);
}

/* Everything handed over has been played. This is what "the track is really
   over" means for a looped ring -- `has_free_slot` goes true long before the
   queued tail has sounded, so a caller that stopped on that would cut the
   last ~0.9s off every track.
   FALSE WHILE PENDING: chunks sitting in a primed-but-unstarted ring have
   not been played either, and calling them drained would silently drop any
   track shorter than the prime threshold. The caller resolves that with
   audio_stream_ring_flush_pending() below. */
static inline bool audio_stream_ring_drained(const struct audio_stream_ring* r,
											 uint64_t now_ms) {
	if(!r->running)
		return r->pending_slots == 0;
	return audio_stream_ring_consumed_samples(r, now_ms) >= r->written_samples;
}

/* R19: conservative device-queued reserve, not decoded read-ahead. Subtract
 * one DSP input block (576 mono16 samples) and a millisecond of clock slop.
 * This is the same existing time-based read-head estimate, not a new claim
 * that AESND exposes a hardware playback-position query. */
static inline uint32_t audio_stream_ring_reserve_us(const struct audio_stream_ring* r,
	uint64_t now_ms) {
	if(!r->running || !r->rate) return 0;
	uint64_t consumed = audio_stream_ring_consumed_samples(r, now_ms);
	if(consumed >= r->written_samples) return 0;
	uint64_t frames = r->written_samples-consumed;
	uint64_t margin = 576u + (r->rate+999u)/1000u;
	if(frames <= margin) return 0;
	uint64_t us = (frames-margin)*1000000u/r->rate;
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* Nominal wall-clock duration of one slot, ms (probe/telemetry only). */
static inline uint32_t
audio_stream_ring_slot_ms(const struct audio_stream_ring* r) {
	if(r->rate == 0)
		return 0;
	return (uint32_t)(((uint64_t)r->slot_samples * UINT64_C(1000))
					  / (uint64_t)r->rate);
}

/* Promote a PENDING ring to running right now, however few slots it holds.
   Returns true if the caller must PlayVoice (and then stamp start_ms);
   false if there was nothing pending.

   THIS IS THE END-OF-TRACK ESCAPE HATCH, and deliberately the ONLY one. A
   track can end before the prime threshold is reached: a one-chunk record,
   or a file whose length is an exact multiple of the chunk size -- there
   the final decode returns 0 frames (rejected by the backend before it ever
   reaches this ring) and the last REAL chunk is a FULL one. Neither shape
   can be spotted from `frames < slot_samples`, so instead of a heuristic
   the backend flushes here whenever the engine asks "are we drained yet?":
   still pending => start the voice and answer no. One mechanism, covering
   every short-track shape. */
static inline bool audio_stream_ring_flush_pending(struct audio_stream_ring* r,
												   uint64_t now_ms) {
	if(r->running || r->pending_slots == 0)
		return false;
	r->running = true;
	r->start_ms = now_ms;
	r->written_slots = r->pending_slots;
	r->written_samples = r->pending_samples;
	r->pending_slots = 0;
	r->pending_samples = 0;
	return true;
}

/* Book a chunk of `frames` real frames into the ring at `now_ms`.

   reset_ring (=> slot 0) whenever there is no live voice and nothing is
   pending, the rate changed under us (a new track -- tracks are
   homogeneous, so this only happens across a track boundary that skipped a
   stop), or the head has underrun the written data: the caller stops the
   voice and zeroes the buffer, and the ring (re-)enters the PENDING state.
   start_voice on the chunk that brings the pending count up to
   AUDIO_STREAM_RING_PRIME_SLOTS -- that is the one the caller PlayVoices,
   stamping start_ms immediately after. Every other chunk needs no device
   work at all.

   The CALLER is responsible for having asked has_free_slot() first; the
   folded-in underrun check is what makes a caller that did not still safe
   (it re-primes rather than writing into the head's own slot). */
static inline struct audio_stream_ring_write
audio_stream_ring_start_chunk(struct audio_stream_ring* r, uint64_t now_ms,
							  uint32_t rate, uint32_t frames) {
	struct audio_stream_ring_write w;
	w.reset_ring = false;
	w.start_voice = false;

	if(r->running
	   && (r->rate != rate || audio_stream_ring_underrun(r, now_ms))) {
		/* Drop back to PENDING and re-prime rather than resuming playback
		   off a single slot: the Dolphin finding is that a one-slot head
		   start is exactly what produced the restart clusters. */
		r->running = false;
		r->pending_slots = 0;
		r->pending_samples = 0;
	}

	if(!r->running) {
		if(r->pending_slots == 0 || r->rate != rate) {
			w.reset_ring = true;
			r->pending_slots = 0;
			r->pending_samples = 0;
			r->rate = rate;
			r->written_slots = 0;
			r->written_samples = 0;
		}
		w.slot = (uint32_t)(r->pending_slots % (uint64_t)r->slots);
		r->pending_slots++;
		r->pending_samples += frames;
		if(r->pending_slots >= (uint64_t)AUDIO_STREAM_RING_PRIME_SLOTS)
			w.start_voice = audio_stream_ring_flush_pending(r, now_ms);
		return w;
	}

	w.slot = (uint32_t)(r->written_slots % (uint64_t)r->slots);
	r->written_slots++;
	r->written_samples += frames;
	return w;
}

#endif
