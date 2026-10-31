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

#ifndef PLATFORM_AUDIO_WAV_H
#define PLATFORM_AUDIO_WAV_H

/*
	S1: the PCM16-mono RIFF/WAVE parse + write core, shared header-style
	(static functions, no deps) between the engine registry
	(audio_registry.c — whose public audio_wav_parse/audio_pcm_sample_from_le
	wrap these) and the host transcode tool (tools/sound_transcode.c). The
	parser is the S0 body moved VERBATIM — including the review-F1
	subtraction-form bounds guard — so the tool rejects exactly what the
	engine rejects, by construction rather than by mirroring.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t wavh_rd_u32le(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}

static inline uint16_t wavh_rd_u16le(const uint8_t* p) {
	return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static inline int16_t wavh_sample_from_le(const int16_t* pcm, size_t index) {
	const uint8_t* p = (const uint8_t*)(pcm + index);
	return (int16_t)wavh_rd_u16le(p);
}

static inline bool wavh_parse(const uint8_t* data, size_t size,
							  const int16_t** pcm, uint32_t* frames,
							  uint32_t* rate) {
	if(!data || !pcm || !frames || !rate || size < 12
	   || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
		return false;

	bool have_fmt = false;
	uint32_t sample_rate = 0;
	const int16_t* samples = NULL;
	uint32_t sample_frames = 0;

	size_t pos = 12;
	while(pos + 8 <= size) {
		const uint8_t* ch = data + pos;
		uint32_t ch_size = wavh_rd_u32le(ch + 4);
		/* Subtraction form: on the Wii size_t is 32-bit, and the additive
		 * check (pos + 8 + ch_size > size) WRAPS for a crafted ch_size near
		 * UINT32_MAX -- passing the bounds check with a ~2G sample count and
		 * corrupting MEM2 through the zero-byte wrapped arena grant
		 * (adversarial review F1). pos + 8 <= size is guaranteed by the loop
		 * condition, so size - pos - 8 cannot underflow. */
		if(ch_size > size - pos - 8)
			return false;
		if(memcmp(ch, "fmt ", 4) == 0 && ch_size >= 16) {
			/* PCM (1), MONO, 16-bit only — the S0 cache format (scout
			   DECISIONS: PCM16 mono; aesndlib has no ADPCM format). */
			if(wavh_rd_u16le(ch + 8) != 1 || wavh_rd_u16le(ch + 10) != 1
			   || wavh_rd_u16le(ch + 22) != 16)
				return false;
			sample_rate = wavh_rd_u32le(ch + 12);
			have_fmt = true;
		} else if(memcmp(ch, "data", 4) == 0) {
			samples = (const int16_t*)(ch + 8);
			sample_frames = ch_size / 2;
		}
		pos += 8 + ch_size + (ch_size & 1u); /* chunks are word-padded */
	}

	if(!have_fmt || !samples || !sample_frames || !sample_rate)
		return false;
	*pcm = samples;
	*frames = sample_frames;
	*rate = sample_rate;
	return true;
}

/* C2 P2: the SAME parse as wavh_parse, walked over an open FILE instead of a
   whole-file buffer, for the deferred loader (audio_deferred.c) that must not
   malloc a whole encoded file during gameplay. Chunk walk, the subtraction-form
   bounds guard, the PCM/mono/16-bit gate, word padding and "the LAST data chunk
   wins" are wavh_parse's, line for line; test_audio_deferred proves the two
   agree on frames, rate and the PCM bytes for every fixture. On success
   `*data_off` is the file offset of the first sample word. Needs <stdio.h>
   from the includer (only audio_deferred.c uses it). */
#ifdef WAVH_WITH_FILE_PROBE
static inline bool wavh_probe_file(FILE* f, uint32_t* data_off,
								   uint32_t* frames, uint32_t* rate) {
	if(!f || !data_off || !frames || !rate || fseek(f, 0, SEEK_END) != 0)
		return false;
	long len = ftell(f);
	if(len < 12)
		return false;
	size_t size = (size_t)len;
	uint8_t head[24];
	if(fseek(f, 0, SEEK_SET) != 0 || fread(head, 1, 12, f) != 12
	   || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0)
		return false;

	bool have_fmt = false;
	uint32_t sample_rate = 0;
	uint32_t sample_off = 0;
	uint32_t sample_frames = 0;

	size_t pos = 12;
	while(pos + 8 <= size) {
		if(fseek(f, (long)pos, SEEK_SET) != 0 || fread(head, 1, 8, f) != 8)
			return false;
		uint32_t ch_size = wavh_rd_u32le(head + 4);
		if(ch_size > size - pos - 8)
			return false;
		if(memcmp(head, "fmt ", 4) == 0 && ch_size >= 16) {
			/* head[8..23] = the first 16 fmt bytes, so the field offsets
			   below are wavh_parse's ch + 8 / + 10 / + 12 / + 22. */
			if(fread(head + 8, 1, 16, f) != 16)
				return false;
			if(wavh_rd_u16le(head + 8) != 1 || wavh_rd_u16le(head + 10) != 1
			   || wavh_rd_u16le(head + 22) != 16)
				return false;
			sample_rate = wavh_rd_u32le(head + 12);
			have_fmt = true;
		} else if(memcmp(head, "data", 4) == 0) {
			sample_off = (uint32_t)(pos + 8);
			sample_frames = ch_size / 2;
		}
		pos += 8 + (size_t)ch_size + (ch_size & 1u);
	}

	if(!have_fmt || !sample_off || !sample_frames || !sample_rate)
		return false;
	*data_off = sample_off;
	*frames = sample_frames;
	*rate = sample_rate;
	return true;
}
#endif

/* Canonical minimal PCM16-mono WAV: 44-byte header (RIFF/fmt/data, no extra
   chunks) + LE sample words — the exact layout the S0 suite's wav_build
   golden generates and wavh_parse round-trips. `pcm` is HOST-order; each
   sample is written little-endian explicitly (Wii-safe). `out` must hold
   wavh_write_size(frames) bytes. Returns the bytes written. */
#define wavh_write_size(frames) ((size_t)44 + (size_t)(frames) * 2)

static inline size_t wavh_write(uint8_t* out, uint32_t rate,
								const int16_t* pcm, uint32_t frames) {
	uint32_t data_bytes = frames * 2;
	uint32_t riff = 36 + data_bytes;
	size_t o = 0;
	memcpy(out + o, "RIFF", 4), o += 4;
	for(int i = 0; i < 4; i++)
		out[o++] = (uint8_t)(riff >> (8 * i));
	memcpy(out + o, "WAVE", 4), o += 4;
	memcpy(out + o, "fmt ", 4), o += 4;
	uint32_t fmt_sz = 16;
	for(int i = 0; i < 4; i++)
		out[o++] = (uint8_t)(fmt_sz >> (8 * i));
	out[o++] = 1, out[o++] = 0; /* PCM */
	out[o++] = 1, out[o++] = 0; /* mono */
	for(int i = 0; i < 4; i++)
		out[o++] = (uint8_t)(rate >> (8 * i));
	uint32_t byte_rate = rate * 2;
	for(int i = 0; i < 4; i++)
		out[o++] = (uint8_t)(byte_rate >> (8 * i));
	out[o++] = 2, out[o++] = 0;	 /* block align */
	out[o++] = 16, out[o++] = 0; /* bits */
	memcpy(out + o, "data", 4), o += 4;
	for(int i = 0; i < 4; i++)
		out[o++] = (uint8_t)(data_bytes >> (8 * i));
	for(uint32_t k = 0; k < frames; k++) {
		uint16_t v = (uint16_t)pcm[k];
		out[o++] = (uint8_t)v;
		out[o++] = (uint8_t)(v >> 8);
	}
	return o;
}

#endif
