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

#ifndef PLATFORM_AUDIO_RESAMPLE_H
#define PLATFORM_AUDIO_RESAMPLE_H

#include <stdbool.h>
#include <stdint.h>

/*
	PURE streaming linear-interpolation resampler for PCM16 mono. No floats,
	no libm, no allocation, no platform types -- integer 16.16 fixed point
	throughout, so it produces bit-identical output on PPC and on the test
	host. audio_music.c's decode worker owns the buffers and the file; this
	header owns the arithmetic, so it is golden-testable without a decoder
	(the audio_music_ring.h / audio_stream_ring.h precedent).

	WHY THIS EXISTS (2026-09-01, real-Wii LISTEN of 2b15972: "C418 songs
	play, still glitchy, almost TINNY like a cheap speaker; other sounds
	fine"). AESND's DSP mixer resamples every voice to its fixed 48 kHz
	output by stepping a 16.16 counter through the source and taking the
	NEAREST sample -- libaesnd's dspmixer.s does no interpolation at all.
	Feeding it 44.1 kHz music means a source sample is simply repeated about
	every 11 output samples, which is a ~3.9 kHz square-ish modulation
	riding on the signal: audible as thin, tinny distortion on TONAL
	material. One-shot sound effects hide it because footsteps and hits are
	broadband noise, and Dolphin hides it because its HLE audio path
	interpolates -- which is exactly why this survived every Dolphin pass
	and only a hardware LISTEN caught it.

	THE FIX IS TO REMOVE THE DSP'S JOB, not to improve it: resample music and
	records to the device rate on the DECODE WORKER (off the render path,
	where a 4096-frame chunk is ~85 ms of audio), so AESND's ratio is exactly
	1.0 and its mixer copies samples straight through. Linear interpolation
	is a large quality step over nearest-neighbour and is cheap; a polyphase
	FIR would be better still and is a possible later T2 refinement, but the
	defect being fixed here is nearest-neighbour, not interpolation order.

	STREAMING CONTRACT. Chunks must join seamlessly, so the state carries the
	last input sample of the previous chunk (`prev`) and the fractional phase
	across the boundary. Conceptually the input is the virtual sequence
	v[0] = prev, v[1..n] = in[0..n-1]; output sample k is the linear blend of
	v[floor(p)] and v[floor(p)+1] at p = phase, and phase advances by `step`
	= (src_rate << 16) / dst_rate per output sample. After a call the state
	is rebased so v[0] is the last input sample actually consumed. On the
	FIRST chunk phase starts at 1.0, which makes a rate-equal stream an exact
	bit-for-bit pass-through rather than a one-sample-delayed copy.
*/

struct audio_resample_state {
	uint32_t src_rate;
	uint32_t dst_rate;
	uint32_t step;	/* 16.16 input samples per output sample; 0 = disabled */
	uint32_t phase; /* 16.16 position in the virtual sequence above */
	int16_t prev;	/* v[0]: last input sample consumed so far */
	bool started;	/* false until the first input sample seeded `prev` */
};

/* Arm the resampler. src_rate == dst_rate (or either being 0) leaves `step`
   at 0, which callers read as "no conversion needed, hand the PCM straight
   through" -- audio_resample_run still behaves as an exact pass-through if
   they run it anyway. */
static inline void audio_resample_init(struct audio_resample_state* st,
									   uint32_t src_rate, uint32_t dst_rate) {
	st->src_rate = src_rate;
	st->dst_rate = dst_rate;
	st->phase = 0;
	st->prev = 0;
	st->started = false;
	if(src_rate == 0 || dst_rate == 0 || src_rate == dst_rate) {
		st->step = 0;
		return;
	}
	st->step = (uint32_t)(((uint64_t)src_rate << 16) / (uint64_t)dst_rate);
	if(st->step == 0)
		st->step = 1; /* absurd ratios still make forward progress */
}

/* True when the stream needs conversion at all. */
static inline bool
audio_resample_active(const struct audio_resample_state* st) {
	return st->step != 0;
}

/* Consume input frames and emit output frames until either the output is
   full or the input runs out. Returns frames written to `out`; sets
   *in_used to the input frames consumed (the caller advances its own read
   cursor by that and calls again with the remainder / the next chunk).

   Both buffers are PCM16 mono. `out` and `in` must not overlap. */
static inline uint32_t audio_resample_run(struct audio_resample_state* st,
										  const int16_t* in, uint32_t in_n,
										  uint32_t* in_used, int16_t* out,
										  uint32_t out_cap) {
	uint32_t produced = 0;
	uint32_t consumed = 0;
	uint32_t step = st->step ? st->step : (uint32_t)0x10000; /* 1.0 */

	if(in_n == 0 || out_cap == 0) {
		*in_used = 0;
		return 0;
	}
	if(!st->started) {
		/* Seed v[0] with the very first input sample and start the read
		   head at 1.0, i.e. AT that sample -- see the pass-through note. */
		st->prev = in[0];
		st->phase = 0x10000u;
		st->started = true;
	}

	while(produced < out_cap) {
		uint32_t i = st->phase >> 16;
		if(i >= in_n)
			break; /* v[i+1] == in[i] does not exist yet: need more input */
		int32_t s0 = (i == 0) ? (int32_t)st->prev : (int32_t)in[i - 1];
		int32_t s1 = (int32_t)in[i];
		uint32_t f = st->phase & 0xFFFFu;
		/* 64-bit product: (s1-s0) spans +-65535 and f spans 0..65535, which
		   overflows a signed 32-bit multiply. This runs on the decode
		   worker at 4096 samples per ~85 ms chunk, so the wider multiply
		   costs nothing worth a precision compromise. */
		int32_t blend
			= s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)f) >> 16);
		out[produced++] = (int16_t)blend;
		st->phase += step;
	}

	consumed = st->phase >> 16;
	if(consumed > in_n)
		consumed = in_n; /* the head ran past this buffer (downsampling) */
	if(consumed > 0) {
		st->prev = in[consumed - 1];
		st->phase -= (consumed << 16);
	}
	*in_used = consumed;
	return produced;
}

/* Output frames a whole stream of `in_frames` input frames becomes, for
   sizing/telemetry. Exact for the steady state; the very first and last
   chunk can differ by one frame because of the phase seed and the tail the
   stream ends on. */
static inline uint32_t
audio_resample_out_frames(const struct audio_resample_state* st,
						  uint32_t in_frames) {
	if(!audio_resample_active(st))
		return in_frames;
	return (uint32_t)(((uint64_t)in_frames * (uint64_t)st->dst_rate)
					  / (uint64_t)st->src_rate);
}

#endif
