/* Generate 28-frame filmstrips of flame_fx (BOTH Beta TextureFlamesFX
 * instances, fire + fire+16) + lava_fx (still) + portal_fx for anim.png. */
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lava_fx.h"
#include "water_fx.h"
#include "water_flow_fx.h"
#include "flame_fx.h"
#include "portal_fx.h"

#define FRAMES 28
#define WARMUP 400
#define STRIDE 2 /* FX ticks per filmstrip frame (~match Beta 20fps vs 8.75fps scroll) */

enum fx_kind { FX_LAVA, FX_FLAME, FX_WATER, FX_WATER_FLOW, FX_PORTAL, FX_COUNT };
#define RAMP_BYTES (256 * 4)

static const char* const ramp_names[FX_COUNT] = {
	"lava.ramp", "flame.ramp", "water.ramp", "water_flow.ramp", "portal.ramp"
};

static float clamp01(float v) {
	if(v > 1.0f)
		v = 1.0f;
	if(v < 0.0f)
		v = 0.0f;
	return v;
}

static void fx_color(enum fx_kind kind, float value, const uint8_t* ramp,
					 uint8_t out[4]) {
	// Always use the original, UNQUANTISED Beta input for alpha. With no ramp
	// this is the entire colour path: no quantisation or altered float math.
	switch(kind) {
	case FX_LAVA: lava_fx_color(value, out); break;
	case FX_FLAME: flame_fx_color(value, out); break;
	case FX_WATER:
	case FX_WATER_FLOW: water_fx_color(value, out); break;
	case FX_PORTAL: portal_fx_color(value, out); break;
	default: return;
	}
	if(!ramp)
		return;

	// Monotonic Beta colour inputs (before channel-specific powers/offsets):
	// lava: c = clamp(field * 2, 0, 1).
	// flame (both instances): c = clamp(field * 1.8, 0, 1).
	// water: c = clamp(field, 0, 1)^2 (Beta's v2 brightness term).
	// water_flow: the same v2, from field[(i - ticks*16) & 255].
	// portal: c = the generated intensity, clamped ONLY for ramp indexing;
	// its Beta colour/alpha map retains the authentic >1 byte wrap.
	float c = value;
	if(kind == FX_LAVA)
		c = clamp01(value * 2.0f);
	else if(kind == FX_FLAME)
		c = clamp01(value * 1.8f);
	else if(kind == FX_WATER || kind == FX_WATER_FLOW) {
		c = clamp01(value);
		c *= c;
	}
	int index = (int)floorf(clamp01(c) * 255.0f + 0.5f);
	memcpy(out, ramp + index * 4, 3); // ramp alpha is deliberately ignored
}

static int load_ramps(const char* dir, uint8_t ramps[FX_COUNT][RAMP_BYTES]) {
	for(int i = 0; i < FX_COUNT; i++) {
		size_t n = strlen(dir) + strlen(ramp_names[i]) + 2;
		char* path = malloc(n);
		if(!path) {
			fprintf(stderr, "fx_gen: out of memory loading ramps\n");
			return 0;
		}
		snprintf(path, n, "%s/%s", dir, ramp_names[i]);
		FILE* f = fopen(path, "rb");
		if(!f) {
			fprintf(stderr, "fx_gen: cannot open required ramp %s: %s\n",
					path, strerror(errno));
			free(path);
			return 0;
		}
		int ok = fread(ramps[i], 1, RAMP_BYTES, f) == RAMP_BYTES;
		if(fgetc(f) != EOF || ferror(f))
			ok = 0;
		if(fclose(f) != 0)
			ok = 0;
		if(!ok)
			fprintf(stderr, "fx_gen: ramp %s must be exactly 1024 raw bytes\n", path);
		free(path);
		if(!ok)
			return 0;
	}
	return 1;
}

int main(int argc, char** argv) {
	uint8_t ramps[FX_COUNT][RAMP_BYTES];
	int use_ramps = argc == 3 && !strcmp(argv[1], "--ramp-dir");
	if(argc != 1 && !use_ramps) {
		fprintf(stderr, "usage: fx_gen [--ramp-dir <dir>]\n");
		return 1;
	}
	if(use_ramps && !load_ramps(argv[2], ramps))
		return 1;
	struct lava_fx lv;
	lava_fx_init(&lv, 0x9E3779B9u);
	/* Beta registers TextureFlamesFX(0) and (1) (Minecraft.java:282-283); one
	 * pair = two states on one shared RNG stream, ticked 0 then 1. */
	struct flame_fx_pair fl;
	flame_fx_pair_init(&fl, 0x1234567u);
	struct water_fx wa;
	water_fx_init(&wa, 0xCAFEF00Du);
	struct water_flow_fx wfl;
	water_flow_fx_init(&wfl, 0xF10FF10Fu);
	for(int i = 0; i < WARMUP; i++) {
		lava_fx_tick(&lv);
		flame_fx_pair_tick(&fl);
		water_fx_tick(&wa);
		water_flow_fx_tick(&wfl);
	}
	FILE* lf = fopen("lava_frames.bin", "wb");
	FILE* ff = fopen("flame_frames.bin", "wb");   /* fx[0]: fire     -> cols 3 & 10 */
	FILE* ff2 = fopen("flame2_frames.bin", "wb"); /* fx[1]: fire+16  -> cols 6 & 13 */
	FILE* wf = fopen("water_frames.bin", "wb");
	FILE* wff = fopen("water_flow_frames.bin", "wb");
	FILE* pf = NULL;
	int result = 1;
	if(!lf || !ff || !ff2 || !wf || !wff) {
		fprintf(stderr, "fx_gen: cannot open output filmstrips\n");
		goto cleanup;
	}
	for(int f = 0; f < FRAMES; f++) {
		for(int s = 0; s < STRIDE; s++) {
			lava_fx_tick(&lv);
			flame_fx_pair_tick(&fl);
			water_fx_tick(&wa);
			water_flow_fx_tick(&wfl);
		}
		uint8_t tiles[5][LAVA_FX_BYTES];
		for(int i = 0; i < LAVA_FX_TEXELS; i++) {
			fx_color(FX_LAVA, lava_fx_field_value(&lv, i),
					 use_ramps ? ramps[FX_LAVA] : NULL, tiles[0] + i * 4);
			fx_color(FX_FLAME, flame_fx_pair_field_value(&fl, 0, i),
					 use_ramps ? ramps[FX_FLAME] : NULL, tiles[1] + i * 4);
			fx_color(FX_FLAME, flame_fx_pair_field_value(&fl, 1, i),
					 use_ramps ? ramps[FX_FLAME] : NULL, tiles[2] + i * 4);
			fx_color(FX_WATER, water_fx_field_value(&wa, i),
					 use_ramps ? ramps[FX_WATER] : NULL, tiles[3] + i * 4);
			fx_color(FX_WATER_FLOW, water_flow_fx_field_value(&wfl, i),
					 use_ramps ? ramps[FX_WATER_FLOW] : NULL, tiles[4] + i * 4);
		}
		if(fwrite(tiles[0], 1, LAVA_FX_BYTES, lf) != LAVA_FX_BYTES
		   || fwrite(tiles[1], 1, FLAME_FX_BYTES, ff) != FLAME_FX_BYTES
		   || fwrite(tiles[2], 1, FLAME_FX_BYTES, ff2) != FLAME_FX_BYTES
		   || fwrite(tiles[3], 1, WATER_FX_BYTES, wf) != WATER_FX_BYTES
		   || fwrite(tiles[4], 1, WATER_FX_BYTES, wff) != WATER_FX_BYTES) {
			fprintf(stderr, "fx_gen: cannot write output filmstrips\n");
			goto cleanup;
		}
	}

	/* Portal: Beta precomputes 32 frames (fixed seed 100 -- deterministic).
	 * Sample 28 of them evenly across the FULL spiral revolution
	 * (idx = f*32/28 = f*8/7, skipping every 8th frame) so the 28-frame
	 * scroll loops seamlessly; every emitted frame is a byte-authentic Beta
	 * frame. See portal_fx.h for the documented T2 cadence note. */
	static uint8_t portal_all[PORTAL_FX_FRAMES * PORTAL_FX_BYTES];
	if(use_ramps) {
		float intensity[PORTAL_FX_FRAMES * PORTAL_FX_TEXELS];
		portal_fx_generate_intensity(intensity);
		for(int i = 0; i < PORTAL_FX_FRAMES * PORTAL_FX_TEXELS; i++)
			fx_color(FX_PORTAL, intensity[i], ramps[FX_PORTAL], portal_all + i * 4);
	} else {
		portal_fx_generate(portal_all);
	}
	pf = fopen("portal_frames.bin", "wb");
	if(!pf) {
		fprintf(stderr, "fx_gen: cannot open portal_frames.bin\n");
		goto cleanup;
	}
	for(int f = 0; f < FRAMES; f++) {
		int idx = f * PORTAL_FX_FRAMES / FRAMES;
		if(fwrite(portal_all + (size_t)idx * PORTAL_FX_BYTES, 1,
				  PORTAL_FX_BYTES, pf) != PORTAL_FX_BYTES) {
			fprintf(stderr, "fx_gen: cannot write portal_frames.bin\n");
			goto cleanup;
		}
	}
	result = 0;

cleanup:
	if(lf && fclose(lf) != 0) result = 1;
	if(ff && fclose(ff) != 0) result = 1;
	if(ff2 && fclose(ff2) != 0) result = 1;
	if(wf && fclose(wf) != 0) result = 1;
	if(wff && fclose(wff) != 0) result = 1;
	if(pf && fclose(pf) != 0) result = 1;
	if(result) {
		fprintf(stderr, "fx_gen: filmstrip bake failed\n");
		return result;
	}

	printf("ok: %d frames each (lava_frames.bin, flame_frames.bin, "
		   "flame2_frames.bin, "
		   "water_frames.bin, water_flow_frames.bin, portal_frames.bin)\n",
		   FRAMES);
	return 0;
}
