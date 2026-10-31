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

#ifndef TOOLS_SOUND_TRANSCODE_H
#define TOOLS_SOUND_TRANSCODE_H

/* The S1 host transcode core (tools/sound_transcode.c), exposed so
   test_audio_env can drive it in-process. See the .c for the contract. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decode one ogg/wav byte buffer (detected by magic) to PCM16 MONO at the
   source's native rate. Ogg: whole-file stb_vorbis decode, stereo averaged
   down exactly like the engine loader. WAV: validated by the ENGINE's own
   parser (shared audio_wav.h) — rejects precisely what the engine rejects.
   On success *out_pcm is a malloc'd HOST-order buffer (caller frees). */
bool sound_transcode_buffer(const uint8_t* data, size_t size,
							int16_t** out_pcm, uint32_t* out_frames,
							uint32_t* out_rate);

/* S2b: pack-build sample-rate CEILING (T2 deviation, docs/deviations.md;
   maintainer-approved 2026-07-28). Anything decoded above the cap is
   downsampled with an anti-aliased scaled-sinc kernel (Lanczos-3) before
   the WAV is emitted; at/below the cap is an exact no-op, and upsampling
   never happens. Chosen over the recorded PCM8 fallback: keeps 16-bit
   depth, fits the FULL real Beta pack in the 12MB arena (measured
   15.99MB -> 9.87MB, all 13 cave variants resident), and fixes a real
   hardware bug — the two 192kHz cave files exceeded AESND's 144kHz
   voice-frequency clamp and would have played SLOWED on the Wii. */
#define SOUND_TRANSCODE_RATE_CAP_HZ 32000u

/* Apply the cap in place: on a downsample *pcm is replaced by a fresh
   malloc'd buffer (the old one freed) and *frames/*rate updated. Returns
   true on success (including the no-op case); false only on allocation
   failure, with the inputs left intact. */
bool sound_transcode_rate_cap(int16_t** pcm, uint32_t* frames,
							  uint32_t* rate);

/* Emit the canonical minimal PCM16-mono WAV (the S0 wav_build layout) into
   `out` (sized wavh_write_size(frames) = 44 + frames*2). Returns the bytes
   written. */
size_t sound_transcode_wav_emit(uint8_t* out, uint32_t rate,
								const int16_t* pcm, uint32_t frames);

/* One manifest row, "<id>\t<key>\t<glob>" (e.g. "3\tstep.grass\t
   sound/step/grass*.wav"), from the SAME table the engine scans
   (audio_sound_table.h). Returns snprintf-style length, or -1 for an
   out-of-range id. */
int sound_transcode_manifest_line(int id, char* out, size_t out_size);

#endif
