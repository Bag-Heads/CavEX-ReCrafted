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
	Driver for the in-game importer's sound phase (beta_import_sound.c),
	run by tests/test_beta_import_sound.py which owns the assertions:

	  test_beta_import_sound <resources_dir|-> <staging_dir> [cancel_after]
	  test_beta_import_sound --tool <in.(ogg|wav)> <out.wav>

	Phase form: calls bi_sounds_import() with a stand-alone bi_ctx (the
	orchestrator's beta_import.c is coder A's; this stub implements the three
	progress hooks the header declares, so the phase is tested on its own).
	Prints:
	  RESULT <n>           the return value
	  STATE <n>            last bi_state passed to bi_progress_set
	  MSG <text>           last message
	  PROGRESS <done>/<total>  last done/total seen
	  STEPS <n>            number of bi_progress_step calls
	"-" for resources_dir passes NULL (the "no folder" case). cancel_after N
	makes bi_cancel_requested() answer true once N progress steps happened.

	Tool form: tools/sound_transcode's main() path (whole-file
	stb_vorbis_decode_memory -> cap -> wavh_write), through the tool core
	that beta_import_sound.c textually includes (bi_st_* names). Compiled by
	the same compiler as the engine phase, so the Python side can hand it to
	the Pack Helper as the transcoder and compare the two decode paths
	without a stale prebuilt sound_transcode binary in the way.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/audio_wav.h"
#include "platform/beta_import.h"

/* The tool core's renamed exports (see beta_import_sound.c). */
bool bi_st_sound_transcode_buffer(const uint8_t* data, size_t size,
								  int16_t** out_pcm, uint32_t* out_frames,
								  uint32_t* out_rate);
bool bi_st_sound_transcode_rate_cap(int16_t** pcm, uint32_t* frames,
									uint32_t* rate);
size_t bi_st_sound_transcode_wav_emit(uint8_t* out, uint32_t rate,
									  const int16_t* pcm, uint32_t frames);

static int tool_main(const char* in_path, const char* out_path) {
	FILE* f = fopen(in_path, "rb");
	if(!f)
		return 1;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(len <= 0) {
		fclose(f);
		return 1;
	}
	uint8_t* in = malloc((size_t)len);
	if(!in || fread(in, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(in);
		return 1;
	}
	fclose(f);
	int16_t* pcm = NULL;
	uint32_t frames = 0, rate = 0;
	if(!bi_st_sound_transcode_buffer(in, (size_t)len, &pcm, &frames, &rate)) {
		free(in);
		fprintf(stderr, "%s: not decodable\n", in_path);
		return 1;
	}
	free(in);
	if(!bi_st_sound_transcode_rate_cap(&pcm, &frames, &rate)) {
		free(pcm);
		return 1;
	}
	size_t out_size = wavh_write_size(frames);
	uint8_t* out = malloc(out_size);
	if(!out) {
		free(pcm);
		return 1;
	}
	size_t written = bi_st_sound_transcode_wav_emit(out, rate, pcm, frames);
	free(pcm);
	FILE* o = fopen(out_path, "wb");
	int rc = (o && fwrite(out, 1, written, o) == written) ? 0 : 1;
	if(o)
		fclose(o);
	free(out);
	return rc;
}

struct bi_ctx {
	bi_state state;
	int done, total;
	char msg[128];
	int steps;
	int cancel_after; /* <0: never */
};

void bi_progress_set(bi_ctx* ctx, bi_state state, int done, int total,
					 const char* msg) {
	ctx->state = state;
	ctx->done = done;
	ctx->total = total;
	snprintf(ctx->msg, sizeof(ctx->msg), "%s", msg ? msg : "");
	fprintf(stderr, "[progress] state=%d %d/%d %s\n", (int)state, done, total,
			ctx->msg);
}

void bi_progress_step(bi_ctx* ctx, int done) {
	ctx->done = done;
	ctx->steps++;
}

bool bi_cancel_requested(bi_ctx* ctx) {
	return ctx->cancel_after >= 0 && ctx->steps >= ctx->cancel_after;
}

int main(int argc, char** argv) {
	if(argc == 4 && strcmp(argv[1], "--tool") == 0)
		return tool_main(argv[2], argv[3]);
	if(argc < 3) {
		fprintf(stderr,
				"usage: test_beta_import_sound <resources|-> <staging> "
				"[cancel_after]\n"
				"       test_beta_import_sound --tool <in> <out.wav>\n");
		return 2;
	}
	struct bi_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.cancel_after = argc > 3 ? atoi(argv[3]) : -1;
	const char* res = strcmp(argv[1], "-") == 0 ? NULL : argv[1];
	int r = bi_sounds_import(res, argv[2], &ctx);
	printf("RESULT %d\n", r);
	printf("STATE %d\n", (int)ctx.state);
	printf("MSG %s\n", ctx.msg);
	printf("PROGRESS %d/%d\n", ctx.done, ctx.total);
	printf("STEPS %d\n", ctx.steps);
	return 0;
}
