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
	S1 HOST transcode tool (never built for the Wii): converts one Beta
	resource sound file into the exact cache format the engine's registry
	loads (canonical minimal PCM16-mono WAV, native rate — audio_registry.c
	decode-at-load). Driven per file by tools/make_beta_pack.ps1's sound
	stage; the transcode-at-PACK-BUILD-time keeps stb_vorbis off the Wii's
	load path for user packs entirely (the finish_line_plan.md §2b rule made
	structural: a built pack contains only .wav).

	  sound_transcode <in.(ogg|wav)> <out.wav>
	      ogg (stb_vorbis, whole-file): stereo -> mono by average — the SAME
	      formula as the engine's own ogg path.
	      wav: validate with the ENGINE parser (shared audio_wav.h) and
	      passthrough-normalize to the canonical 44-byte-header layout —
	      anything the engine would reject, this rejects.
	      S2b: either path then runs through the 32kHz rate cap
	      (sound_transcode_rate_cap — anti-aliased Lanczos-3 downsample;
	      rates at/below the cap pass through untouched).
	  sound_transcode --manifest
	      one row per registry id, "<id>\t<key>\t<glob>", generated from the
	      SAME X-macro table the engine scans (audio_sound_table.h) so the
	      pack builder can never drift from the loader.

	The core is in-process testable (tools/sound_transcode.h); test_audio_env
	compiles this file with SOUND_TRANSCODE_NO_MAIN (no main) and
	SOUND_TRANSCODE_EXTERN_VORBIS (stb_vorbis declarations only — the test
	binary already links the implementation from audio_registry.c's TU).
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/audio_sound_table.h"
#include "platform/audio_wav.h"
#include "sound_transcode.h"

/* Same single-TU stb_vorbis configuration as audio_registry.c (the flags
   shape the API surface, so they MUST match the engine's copy for the
   header-only test build to link against it). NO_STDIO was dropped from
   BOTH files together (S3, audio_music.c): the engine now needs
   stb_vorbis_open_filename's pull-decode API for streaming, and this
   file's EXTERN_VORBIS header-only mode (test_audio_env) must declare the
   identical symbol set the engine TU it links against actually emits, or
   the two disagree and the test build fails to link. */
#ifdef SOUND_TRANSCODE_EXTERN_VORBIS
#define STB_VORBIS_HEADER_ONLY
#endif
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#include <stb/stb_vorbis.c>
#pragma GCC diagnostic pop

/* Key table, generated from the shared X-macro (== the engine's s_names). */
static const char* const s_keys[AUDIO_SOUND_COUNT] = {
#define AUDIO_SOUND_TABLE_X(id, name) [id] = name,
	AUDIO_SOUND_TABLE(AUDIO_SOUND_TABLE_X)
#undef AUDIO_SOUND_TABLE_X
};

bool sound_transcode_buffer(const uint8_t* data, size_t size,
							int16_t** out_pcm, uint32_t* out_frames,
							uint32_t* out_rate) {
	if(!data || !out_pcm || !out_frames || !out_rate || size < 4)
		return false;

	if(memcmp(data, "OggS", 4) == 0) {
		int channels = 0, rate = 0;
		short* dec = NULL;
		int frames
			= stb_vorbis_decode_memory(data, (int)size, &channels, &rate, &dec);
		if(frames <= 0 || !dec || (channels != 1 && channels != 2)
		   || rate <= 0) {
			free(dec);
			return false;
		}
		int16_t* pcm = malloc((size_t)frames * 2);
		if(!pcm) {
			free(dec);
			return false;
		}
		if(channels == 1) {
			memcpy(pcm, dec, (size_t)frames * 2);
		} else {
			/* Average-downmix — MUST match the engine's own ogg load path
			   (audio_registry.c) so a pack-build transcode and a direct
			   in-engine ogg load yield identical samples. */
			for(int k = 0; k < frames; k++)
				pcm[k] = (int16_t)(((int)dec[2 * k] + (int)dec[2 * k + 1]) / 2);
		}
		free(dec);
		*out_pcm = pcm;
		*out_frames = (uint32_t)frames;
		*out_rate = (uint32_t)rate;
		return true;
	}

	/* WAV: the ENGINE's parser is the validator (shared header — rejects
	   stereo/8-bit/float/malformed/crafted-size exactly like the loader). */
	const int16_t* le_pcm;
	uint32_t frames, rate;
	if(!wavh_parse(data, size, &le_pcm, &frames, &rate))
		return false;
	int16_t* pcm = malloc((size_t)frames * 2);
	if(!pcm)
		return false;
	for(uint32_t k = 0; k < frames; k++)
		pcm[k] = wavh_sample_from_le(le_pcm, k);
	*out_pcm = pcm;
	*out_frames = frames;
	*out_rate = rate;
	return true;
}

size_t sound_transcode_wav_emit(uint8_t* out, uint32_t rate,
								const int16_t* pcm, uint32_t frames) {
	return wavh_write(out, rate, pcm, frames);
}

/* S2b rate cap (see the header for the why). Downsampling is a scaled
   windowed-sinc (Lanczos-3): the kernel is stretched by the decimation
   ratio so its cutoff sits at the OUTPUT Nyquist — that IS the
   anti-aliasing low-pass, fused with the interpolation (a naive decimator
   would fold everything above 16kHz back into the audible band). Per-tap
   weight-sum normalization gives exact unity DC gain and edge-safe
   windows. O(frames * 6 * ratio) on the host at pack-build time — never on
   the Wii. Pinned by test_audio_env B3 (no-op / DC golden / alias-tone
   rejection). */
bool sound_transcode_rate_cap(int16_t** pcm, uint32_t* frames,
							  uint32_t* rate) {
	if(!pcm || !*pcm || !frames || !rate || *rate == 0)
		return false;
	if(*rate <= SOUND_TRANSCODE_RATE_CAP_HZ)
		return true; /* at/below the cap: exact no-op */

	const int16_t* in = *pcm;
	uint32_t in_frames = *frames;
	double ratio = (double)*rate / (double)SOUND_TRANSCODE_RATE_CAP_HZ;
	uint64_t of64
		= (uint64_t)in_frames * SOUND_TRANSCODE_RATE_CAP_HZ / *rate;
	if(of64 == 0)
		of64 = 1; /* a sub-sample-length sound still yields one frame */
	uint32_t out_frames = (uint32_t)of64;
	int16_t* out = malloc((size_t)out_frames * 2);
	if(!out)
		return false;

	const double kpi = 3.14159265358979323846;
	int half = (int)(3.0 * ratio) + 1; /* Lanczos a=3 support, source units */
	for(uint32_t n = 0; n < out_frames; n++) {
		double center = (double)n * ratio;
		int c0 = (int)center;
		double acc = 0.0, wsum = 0.0;
		for(int k = c0 - half; k <= c0 + half; k++) {
			/* x in OUTPUT-sample units: the ratio-stretched kernel. */
			double x = ((double)k - center) / ratio;
			double w;
			if(x > -1.0e-9 && x < 1.0e-9) {
				w = 1.0;
			} else if(x <= -3.0 || x >= 3.0) {
				continue; /* outside the a=3 window (sinc(±3) == 0) */
			} else {
				double px = kpi * x;
				w = 3.0 * sin(px) * sin(px / 3.0) / (px * px);
			}
			int idx = k;
			if(idx < 0)
				idx = 0; /* clamp-extend the edges */
			else if(idx >= (int)in_frames)
				idx = (int)in_frames - 1;
			acc += w * (double)in[idx];
			wsum += w;
		}
		double v = (wsum > 1.0e-12 || wsum < -1.0e-12) ? acc / wsum : 0.0;
		if(v > 32767.0)
			v = 32767.0;
		else if(v < -32768.0)
			v = -32768.0;
		out[n] = (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
	}

	free(*pcm);
	*pcm = out;
	*frames = out_frames;
	*rate = SOUND_TRANSCODE_RATE_CAP_HZ;
	return true;
}

int sound_transcode_manifest_line(int id, char* out, size_t out_size) {
	if(id < 0 || id >= AUDIO_SOUND_COUNT)
		return -1;
	char sub[128];
	audio_sound_table_subpath(s_keys[id], sub, sizeof(sub));
	return snprintf(out, out_size, "%d\t%s\t%s*.wav", id, s_keys[id], sub);
}

#ifndef SOUND_TRANSCODE_NO_MAIN

static uint8_t* read_whole_file(const char* path, size_t* out_size) {
	/* Mirror of the engine's (static) audio_file_read — a 12-line stdio
	   read; not worth a third shared header for a host-only tool. */
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(len <= 0) {
		fclose(f);
		return NULL;
	}
	uint8_t* buf = malloc((size_t)len);
	if(!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if(got != (size_t)len) {
		free(buf);
		return NULL;
	}
	*out_size = got;
	return buf;
}

int main(int argc, char** argv) {
	if(argc == 2 && strcmp(argv[1], "--manifest") == 0) {
		for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
			char line[256];
			sound_transcode_manifest_line(id, line, sizeof(line));
			puts(line);
		}
		return 0;
	}
	if(argc != 3) {
		fprintf(stderr,
				"usage: sound_transcode <in.(ogg|wav)> <out.wav>\n"
				"       sound_transcode --manifest\n");
		return 2;
	}

	size_t in_size = 0;
	uint8_t* in = read_whole_file(argv[1], &in_size);
	if(!in) {
		fprintf(stderr, "sound_transcode: cannot read %s\n", argv[1]);
		return 1;
	}

	int16_t* pcm = NULL;
	uint32_t frames = 0, rate = 0;
	if(!sound_transcode_buffer(in, in_size, &pcm, &frames, &rate)) {
		fprintf(stderr,
				"sound_transcode: %s: not a decodable ogg or an "
				"engine-loadable PCM16 mono WAV\n",
				argv[1]);
		free(in);
		return 1;
	}
	free(in);

	uint32_t src_rate = rate;
	if(!sound_transcode_rate_cap(&pcm, &frames, &rate)) {
		free(pcm);
		fprintf(stderr, "sound_transcode: out of memory\n");
		return 1;
	}

	size_t out_size = wavh_write_size(frames);
	uint8_t* out = malloc(out_size);
	if(!out) {
		free(pcm);
		fprintf(stderr, "sound_transcode: out of memory\n");
		return 1;
	}
	size_t written = sound_transcode_wav_emit(out, rate, pcm, frames);
	free(pcm);

	FILE* f = fopen(argv[2], "wb");
	if(!f || fwrite(out, 1, written, f) != written) {
		if(f)
			fclose(f);
		free(out);
		fprintf(stderr, "sound_transcode: cannot write %s\n", argv[2]);
		return 1;
	}
	fclose(f);
	free(out);

	if(src_rate != rate)
		printf("%s -> %s (%u frames @ %u Hz, PCM16 mono, capped from %u Hz)\n",
			   argv[1], argv[2], (unsigned)frames, (unsigned)rate,
			   (unsigned)src_rate);
	else
		printf("%s -> %s (%u frames @ %u Hz, PCM16 mono)\n", argv[1], argv[2],
			   (unsigned)frames, (unsigned)rate);
	return 0;
}

#endif /* SOUND_TRANSCODE_NO_MAIN */
