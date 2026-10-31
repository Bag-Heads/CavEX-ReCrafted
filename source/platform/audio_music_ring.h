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

#ifndef PLATFORM_AUDIO_MUSIC_RING_H
#define PLATFORM_AUDIO_MUSIC_RING_H

#include <stdbool.h>

/*
	Pure N-half decode-ring bookkeeping for the S3 streaming voice.
	audio_music.c owns the real MUSIC_RING_HALVES x MUSIC_CHUNK_BYTES
	PCM16-mono halves, the mutex guarding them, and the decode thread that
	fills them off the render path; this header is only the DECISION logic
	those pieces consult, extracted so the state transitions are
	golden-testable without a real decoder, a device, or a thread (the
	audio_music_timer.h precedent).

	WHY FOUR HALVES, NOT TWO (2026-09-01). Dolphin measured every remaining
	streaming gap inside the first ~40s after a world loads -- 13.8s of
	silence there, and zero gaps for the whole rest of the track, even after
	the decode worker was lifted above the chunk mesher. The cause is not
	decoder speed: audio_music_update hands the backend AT MOST ONE chunk
	per frame, and with only two halves it could hold at most one chunk in
	reserve. During the load storm a frame takes 100-300ms while a chunk is
	only 93-186ms of audio, so supply was capped BELOW consumption no matter
	how fast the decode ran. Four halves (32KB, both platforms) let the
	worker bank three chunks ahead, and audio_music_update's drain loop then
	hands over several of them in ONE slow frame -- enough to refill the Wii
	backend's free ring slots in a single pass.

	Halves are indices 0..MUSIC_RING_HALVES-1 and are played in strictly
	increasing order, wrapping. At most one half is "playing" (handed to the
	platform voice, source/platform/audio.h's audio_voice_music_* group) at
	a time; every OTHER half is a legal decoder fill target. `ready[h]` means
	half h holds a fully decoded, not-yet-consumed chunk. The caller must
	only flip ready[h] back to false -- freeing it for the decoder to
	overwrite -- AFTER audio_voice_music_playing() has already reported it
	can take another chunk (never eagerly): the platform voice may still be
	asynchronously reading that exact memory (the PC mixer callback runs on
	its own device thread) right up until that poll flips false. This is the
	same "stop, THEN rewrite" ordering audio_registry_reload's drain already
	relies on (audio.c/wii/audio.c). The Wii backend additionally COPIES the
	chunk into its own hardware ring (audio_stream_ring.h), so there it is
	safe the moment the call returns.
*/

/* WHY THIRTY-TWO HALVES + A REFILL LOW-WATER MARK (2026-09-26, T1 music
   read-ahead, docs/intent/music-readahead.md). Real-Wii perf_stress_5m with a
   long track (hal, forced by debug.music_force=on) logged 3,988 per-frame
   underruns while the decode worker (pri 48) and the chunk mesher (pri 32)
   starved together under main(64)+server(63). The telemetry says the decoder
   is NOT short of CPU on average -- music_ms advanced at 0.987-0.994x
   realtime across the whole track in all three long-track runs
   (staged_perf_stress_5m_20260926_081655 / _082326 / _090611) -- it is short
   in BURSTS: the worst accumulated hand-over deficit (max drawdown of
   music_ms minus wall-clock from its running peak) was 948 / 1365 / 1943 ms,
   i.e. 11.1 / 16.0 / 22.8 chunks of 85.3 ms (4096 frames at the Wii's 48 kHz
   output rate). Four halves bank at most 3 chunks (256 ms), so every burst
   longer than that became an underrun.

   Coverage is set by the FEWEST chunks that can be banked when a burst
   starts. With the low-water refill below that minimum is
   HALVES - REFILL_LOW (the worker idles until REFILL_LOW halves are free,
   so at most REFILL_LOW-1 are empty -- plus the playing one -- while it is
   not decoding): 32 - 8 = 24 chunks = 2.05 s >= the 1.94 s worst measured
   drawdown (5% margin), on top of the Wii backend's own 7 usable
   audio_stream_ring slots that the drawdown figure already includes. 31
   would clear it by only 0.02 s; 32 keeps the margin for one more 8 KB
   half. Memory: 32 x 8 KB = 256 KB, taken ONCE at music init from the MEM2
   carve (audio_music.c; the carve had ~13 MB free per MEM-HEADROOM-0925),
   NOT from MEM1 .bss -- the 32 KB four-half static stays as the fallback
   if that allocation ever fails, so the degraded mode is exactly the
   pre-2026-09-26 ring.

   REFILL_LOW is the "fewer, larger wakes" half of the change. Before it,
   every consumed chunk freed one half and woke the worker for ONE ~17 ms
   decode (~12 wakes/s, each a context switch plus a mutex round-trip
   against the main thread). Now the worker sleeps until 8 halves are free,
   then decodes until the ring is full again: ~1.5 wakes/s, eight chunks per
   burst. The TOTAL decode work is unchanged (same chunks, same decoder) --
   what changes is that it banks ahead whenever the CPU has room instead of
   running at most three chunks ahead of the voice.

   OUTPUT IS BYTE-IDENTICAL (T1). The worker still decodes chunks strictly
   in stream order into halves in strictly increasing (wrapping) order, and
   the consumer still plays them in that order and never skips (advance
   below). How far ahead the worker runs changes WHEN a chunk is decoded,
   never WHICH samples it holds or the order they are handed over;
   tests/test_music_readahead_equiv.c proves it end to end through the real
   audio_music.c for both ring policies. */
#define MUSIC_RING_HALVES 32
#define MUSIC_RING_REFILL_LOW 8
/* The pre-2026-09-26 ring: the fallback storage size, and the "legacy"
   policy the equivalence test replays (4 halves, refill at 1 free = wake on
   every freed half, which is exactly the old behaviour). */
#define MUSIC_RING_HALVES_LEGACY 4
#define MUSIC_RING_REFILL_LOW_LEGACY 1

_Static_assert(MUSIC_RING_REFILL_LOW >= 1
				   && MUSIC_RING_REFILL_LOW <= MUSIC_RING_HALVES - 1,
			   "refill low-water must be reachable (1..HALVES-1 free halves)");
_Static_assert(MUSIC_RING_HALVES_LEGACY <= MUSIC_RING_HALVES,
			   "the legacy policy must fit the production arrays");

/* Every helper takes the ACTIVE half count `n` (2..MUSIC_RING_HALVES): the
   production ring runs with n = MUSIC_RING_HALVES, the fallback and the
   equivalence test's legacy replay with n = MUSIC_RING_HALVES_LEGACY. The
   `ready` array must hold at least n entries. */

/* Which half the decoder should fill next -- the first NOT-ready half at or
   after the one following `playing_half`, never the playing half itself --
   or -1 if every fillable half already holds a chunk (the decoder should
   idle/sleep on its condvar).

   playing_half may be -1 (no track started yet). In that state NOTHING has
   been handed to the device, so EVERY half is fillable and the scan starts
   at 0: the worker can bank the whole ring before the first chunk is even
   played, which is exactly the head start the world-load storm needs. (The
   two-half version returned -1 as soon as half 0 was ready, which is what
   made the first-start cond_signal in audio_music_update load-bearing; that
   signal is still there and still correct, it is simply no longer the only
   thing that can wake a stalled worker.) */
static inline int music_ring_fill_target_n(const bool* ready, int n,
										   int playing_half) {
	int start = (playing_half < 0) ? 0 : playing_half + 1;
	int span = (playing_half < 0) ? n : n - 1;
	for(int k = 0; k < span; k++) {
		int h = (start + k) % n;
		if(!ready[h])
			return h;
	}
	return -1;
}

/* How many halves the decoder could fill right now: the not-ready halves,
   excluding the playing one (which is never a fill target). */
static inline int music_ring_free_count_n(const bool* ready, int n,
										  int playing_half) {
	int free_halves = 0;
	for(int h = 0; h < n; h++)
		if(h != playing_half && !ready[h])
			free_halves++;
	return free_halves;
}

/* Should an IDLE decoder start a refill burst? Yes when nothing has been
   handed to the device yet (a fresh track: bank at once, the first chunk is
   needed now) or when at least `refill_low` halves are free. Once bursting,
   the worker keeps going until fill_target reports the ring full -- this
   predicate only gates the START of a burst, and audio_music_update
   evaluates the same predicate before signalling, so the consumer never
   wakes a worker that would only go straight back to sleep. An underrun
   always satisfies it (next half not ready => nothing banked => n-1 >=
   refill_low halves free). */
static inline bool music_ring_refill_due_n(const bool* ready, int n,
										   int playing_half, int refill_low) {
	if(playing_half < 0)
		return music_ring_fill_target_n(ready, n, playing_half) >= 0;
	return music_ring_free_count_n(ready, n, playing_half) >= refill_low;
}

/* The playing half's chunk is done with (the platform voice reported it can
   take another). Returns the new playing half -- the NEXT one in order, if
   it is ready -- or -1 on an UNDERRUN: the decoder has not kept up, and the
   caller must NOT hand the device a not-yet-ready buffer. Never skips ahead
   to a later ready half: halves carry consecutive audio, so playing them
   out of order would drop a chunk rather than merely stall.

   On -1 the caller should count the underrun (an F3 dial, the
   s_arena_rejects precedent -- never a silent stall) and simply re-poll next
   frame without disturbing `ready`/`playing_half`; the decoder will flip the
   target half ready as soon as it catches up, and the very next call with
   the SAME arguments then returns that half normally.
   playing_half may be -1 at the very first start (nothing to advance FROM;
   the caller only reaches this path once something was already playing). */
static inline int music_ring_advance_n(const bool* ready, int n,
									   int playing_half) {
	int next = (playing_half < 0) ? 0 : (playing_half + 1) % n;
	return ready[next] ? next : -1;
}

/* How many halves in a row, starting after `playing_half`, are ready -- i.e.
   how many chunks audio_music_update's drain loop can hand over in ONE frame
   before it hits a not-ready half. This is the loop's BOUND, not a mirror of
   it: the loop runs `music_ring_ready_run_n(...) + 1` passes, so it drains
   every banked chunk and then spends exactly ONE final pass on the
   EOF/underrun check -- one underrun count per frame at most, the same rate
   the old single-hand-off loop had, and never a spin (the result is capped
   at n-1 by construction, since the playing half is never counted). */
static inline int music_ring_ready_run_n(const bool* ready, int n,
										 int playing_half) {
	int h = (playing_half < 0) ? 0 : playing_half;
	int count = 0;
	for(int k = 0; k < n - 1; k++) {
		h = (h + 1) % n;
		if(!ready[h])
			break;
		count++;
	}
	return count;
}

/* G9 DECODE DEADLINE PROMOTION (2026-10-09, T2 scheduling only, opt-in with
   experimental.g9_tick_boost_int on a LOCAL server; docs/g9_tick_boost.md
   "Audio"). The counted music_underruns are THIS ring running dry (advance
   -> -1 while the device has room): the pri-48 decode worker gets only the
   CPU left over by MAIN (64) and the server (63, boosted 65). The tick boost
   lets the server finish more of its backlog (+8% ticks on perf_stress_5m),
   which is exactly the leftover time the worker used to bank ahead in, so
   the bank drains during long heavy stretches.

   The fix is WHEN the worker runs, never WHAT it decodes: while a track is
   sounding and fewer than MUSIC_DECODE_PROMOTE_BANK chunks are banked ahead
   of the playing half, the worker runs at MUSIC_DECODE_PRIORITY_PROMOTED
   (above the boosted server and the raised telemetry sender, below the
   Wiimote (80) and the watchdog (100)); at or above the bank it drops back
   to 48 and banks the rest of the ring only from idle time, as before.
   Each promoted stretch is bounded by the bank: it ends as soon as the bank
   is back at PROMOTE_BANK, so it can never run ahead of playback at the
   high priority -- its promoted CPU is at most what realtime playback needs.

   PROMOTE_BANK = AUDIO_STREAM_RING_SLOTS - 1 (8): MAIN hands at most S-2 = 7
   chunks to the hardware ring in one frame (the S-2 guard), so a bank of 8
   means no single MAIN hand-off pass can find the next half not ready.
   Same decoder, same order, same halves: the PCM hand-off stream is
   byte-identical (tests/test_music_decode_promotion.c). */
#define MUSIC_DECODE_PRIORITY_BASE 48
#define MUSIC_DECODE_PRIORITY_PROMOTED 70
#define MUSIC_DECODE_PROMOTE_BANK 8

/* Should the decode worker run promoted right now? Only while something is
   sounding (playing_half >= 0: the first-chunk bank of a new track stays at
   48, exactly as before) and the banked run ahead of it is below the bank.
   The bank is clamped to n-1 so the 4-half fallback ring can still reach
   "not due" when fully banked. */
static inline bool music_decode_promote_due_n(const bool* ready, int n,
											  int playing_half, int bank) {
	if(playing_half < 0)
		return false;
	if(bank > n - 1)
		bank = n - 1;
	return music_ring_ready_run_n(ready, n, playing_half) < bank;
}

#endif
