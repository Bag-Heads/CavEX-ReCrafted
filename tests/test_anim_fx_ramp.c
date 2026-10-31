/* Exercise the actual offline generator, including file loading and all six
 * filmstrips. Expected frames come from the original Beta colour functions and
 * direct state reads, independently of the new accessors/ramp mapper. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#ifdef _WIN32
#define fx_chdir _chdir
#define fx_getcwd _getcwd
#define fx_mkdir(p) _mkdir(p)
#else
#define fx_chdir chdir
#define fx_getcwd getcwd
#define fx_mkdir(p) mkdir(p, 0700)
#endif

// Compile the tool itself into this focused test, with only its entry renamed.
#define main anim_fx_gen_main
#include "../tools/gen_anim_fx/fx_gen.c"
#undef main

#define TEST_FRAMES 28
#define TEST_BYTES (TEST_FRAMES * 1024)
static uint8_t plain[6][TEST_BYTES];
static uint8_t ramped[6][TEST_BYTES];
static uint8_t synthetic[1024];
static const char* const frame_names[6] = {
	"lava_frames.bin", "flame_frames.bin", "flame2_frames.bin",
	"water_frames.bin", "water_flow_frames.bin", "portal_frames.bin"
};

static void write_ramp(const char* name, size_t bytes) {
	FILE* f = fopen(name, "wb");
	assert(f);
	size_t n = bytes > sizeof(synthetic) ? sizeof(synthetic) : bytes;
	assert(fwrite(synthetic, 1, n, f) == n);
	if(bytes > sizeof(synthetic))
		assert(fputc(17, f) != EOF);
	assert(fclose(f) == 0);
}

static void read_frames(uint8_t frames[6][TEST_BYTES]) {
	for(int i = 0; i < 6; i++) {
		FILE* f = fopen(frame_names[i], "rb");
		assert(f);
		assert(fread(frames[i], 1, TEST_BYTES, f) == TEST_BYTES);
		assert(fgetc(f) == EOF && !ferror(f));
		assert(fclose(f) == 0);
	}
}

typedef void (*beta_color_fn)(float, uint8_t[4]);

static void check_pixel(const uint8_t* actual, const uint8_t beta[4], float c) {
	c = fminf(1.0f, fmaxf(0.0f, c));
	int k = (int)floorf(c * 255.0f + 0.5f);
	assert(actual[0] == k);
	assert(actual[1] == 255 - k);
	assert(actual[2] == k / 2);
	assert(actual[3] == beta[3]); // not the ramp's alpha or a quantised alpha
}

static void check_texel(int strip, int frame, int texel, float value, float c,
						beta_color_fn color) {
	uint8_t beta[4];
	color(value, beta);
	size_t offset = (size_t)frame * 1024 + (size_t)texel * 4;
	assert(memcmp(plain[strip] + offset, beta, 4) == 0);
	check_pixel(ramped[strip] + offset, beta, c);
}

static void check_filmstrips(void) {
	struct lava_fx lava;
	struct flame_fx_pair flame;
	struct water_fx water;
	struct water_flow_fx flow;
	lava_fx_init(&lava, 0x9E3779B9u);
	flame_fx_pair_init(&flame, 0x1234567u);
	water_fx_init(&water, 0xCAFEF00Du);
	water_flow_fx_init(&flow, 0xF10FF10Fu);
	// Pin the bake schedule independently of the generator's macros.
	for(int tick = 0; tick < 400 + TEST_FRAMES * 2; tick++) {
		lava_fx_tick(&lava);
		flame_fx_pair_tick(&flame);
		water_fx_tick(&water);
		water_flow_fx_tick(&flow);
		if(tick < 400 || (tick - 400) % 2 == 0)
			continue;
		int frame = (tick - 400) / 2;
		for(int i = 0; i < 256; i++) {
			float v = lava.field[i];
			assert(lava_fx_field_value(&lava, i) == v);
			check_texel(0, frame, i, v, v * 2.0f, lava_fx_color);
			for(int instance = 0; instance < 2; instance++) {
				v = flame.fx[instance].field[i];
				assert(flame_fx_field_value(&flame.fx[instance], i) == v);
				assert(flame_fx_pair_field_value(&flame, instance, i) == v);
				check_texel(instance + 1, frame, i, v, v * 1.8f, flame_fx_color);
			}
			v = water.field[i];
			assert(water_fx_field_value(&water, i) == v);
			float c = fminf(1.0f, fmaxf(0.0f, v));
			check_texel(3, frame, i, v, c * c, water_fx_color);
			v = flow.field[((uint32_t)i - flow.ticks * 16u) & 255u];
			assert(water_flow_fx_field_value(&flow, i) == v);
			c = fminf(1.0f, fmaxf(0.0f, v));
			check_texel(4, frame, i, v, c * c, water_fx_color);
		}
	}
	static float intensity[32 * 256];
	static uint8_t portal[32 * 1024];
	portal_fx_generate_intensity(intensity);
	portal_fx_generate(portal);
	for(int f = 0; f < 32; f++) {
		for(int i = 0; i < 256; i++) {
			uint8_t beta[4];
			portal_fx_color(intensity[f * 256 + i], beta);
			assert(memcmp(portal + f * 1024 + i * 4, beta, 4) == 0);
		}
	}
	for(int f = 0; f < TEST_FRAMES; f++) {
		for(int i = 0; i < 256; i++) {
			float v = intensity[(f * 32 / TEST_FRAMES) * 256 + i];
			check_texel(5, f, i, v, v, portal_fx_color);
		}
	}
	puts("PASS: all 28 frames x 256 texels x six strips: ramp RGB + Beta alpha;"
		 " no-ramp equals Beta colour path; portal split agrees for all 32 frames");
}

static void check_color_edges(void) {
	// Include the flame cutoff neighbourhood, quantisation ties, water alpha
	// fractions and portal >1 wrap. None may quantise the Beta alpha input.
	const float values[] = { -0.1f, 0.0f, 0.25f, 0.2777f, 0.2778f,
		0.5f / 255.0f, 0.5f, 0.5001f, 0.9999f, 1.0f, 1.05f };
	const beta_color_fn colors[FX_COUNT] = {
		lava_fx_color, flame_fx_color, water_fx_color, water_fx_color, portal_fx_color
	};
	for(int kind = 0; kind < FX_COUNT; kind++) {
		for(size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
			float v = values[i];
			uint8_t beta[4], out[4];
			colors[kind](v, beta);
			fx_color((enum fx_kind)kind, v, NULL, out);
			assert(memcmp(out, beta, 4) == 0);
			fx_color((enum fx_kind)kind, v, synthetic, out);
			float c = v;
			if(kind == FX_LAVA) c = v * 2.0f;
			if(kind == FX_FLAME) c = v * 1.8f;
			if(kind == FX_WATER || kind == FX_WATER_FLOW) {
				c = fminf(1.0f, fmaxf(0.0f, v));
				c *= c;
			}
			check_pixel(out, beta, c);
		}
	}
}

static void check_file_errors(char** ramp_args) {
	char* bad_args[] = { "fx_gen", "--ramp-dir" };
	assert(anim_fx_gen_main(2, bad_args) != 0);
	char* unknown_args[] = { "fx_gen", "--unknown", "." };
	assert(anim_fx_gen_main(3, unknown_args) != 0);
	uint8_t loaded[FX_COUNT][RAMP_BYTES];
	for(int i = 0; i < FX_COUNT; i++) {
		assert(remove(ramp_names[i]) == 0);
		assert(anim_fx_gen_main(3, ramp_args) != 0);
		write_ramp(ramp_names[i], 1023);
		assert(anim_fx_gen_main(3, ramp_args) != 0);
		write_ramp(ramp_names[i], 1025);
		assert(anim_fx_gen_main(3, ramp_args) != 0);
		write_ramp(ramp_names[i], 1024);
		assert(load_ramps(".", loaded));
		for(int k = 0; k < FX_COUNT; k++)
			assert(memcmp(loaded[k], synthetic, 1024) == 0);
	}
	puts("PASS: each required ramp rejects missing/1023/1025-byte files");
}

int main(void) {
	char cwd[4096], fixture[256];
	assert(fx_getcwd(cwd, sizeof(cwd)));
	test_fixture_dir("test_anim_fx_ramp_tmp", fixture, sizeof(fixture));
	assert(fx_mkdir(fixture) == 0);
	assert(fx_chdir(fixture) == 0);
	for(int k = 0; k < 256; k++) {
		synthetic[k * 4] = (uint8_t)k;
		synthetic[k * 4 + 1] = (uint8_t)(255 - k);
		synthetic[k * 4 + 2] = (uint8_t)(k / 2);
		synthetic[k * 4 + 3] = (uint8_t)(k ^ 0xa5);
	}
	for(int i = 0; i < FX_COUNT; i++)
		write_ramp(ramp_names[i], 1024);
	char* ramp_args[] = { "fx_gen", "--ramp-dir", "." };
	assert(anim_fx_gen_main(3, ramp_args) == 0);
	read_frames(ramped);
	char* plain_args[] = { "fx_gen" };
	assert(anim_fx_gen_main(1, plain_args) == 0);
	read_frames(plain);
	check_filmstrips();
	check_color_edges();
	check_file_errors(ramp_args);
	assert(fx_chdir(cwd) == 0);
	test_fixture_dir_cleanup(fixture);
	puts("test_anim_fx_ramp: ALL OK");
	return 0;
}
