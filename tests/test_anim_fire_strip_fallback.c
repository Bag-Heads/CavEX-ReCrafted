/*
	test_anim_fire_strip_fallback -- fire-anim-0924/F1 pack-load fallback.

	Beta's fire+16 tile lives in anim.png cols 6 & 13 (flame_fx.h). A pack that
	ships its own anim.png without that strip -- fully transparent there (the CC0
	Earth & Ash pack), or the magenta placeholder the bundled atlas carried before
	2026-09-24 -- must not draw invisible or magenta flames. Textures can be
	re-applied IN-GAME (pause menu -> texture packs) while chunk display lists
	already hold baked fire UVs, and the mesher runs on its own thread, so the
	fallback may NOT be a runtime flag: tex_init repairs the DECODED IMAGE before
	upload (flame_fx_anim_prepare_pack_atlas copies cols 3/10 into 6/13), and every
	consumer samples cols 6/13 for fire+16 unconditionally.

	(1) bundled assets/anim.png (real strip): prepare leaves it byte-identical.
	(2) cols 6/13 transparent, (3) magenta placeholder, (4) partly blank, and
	(5) the checked-in Earth & Ash anim.png: since 2026-10-11 it carries a real baked
	fire+16 strip (Beta motion in E&A colours), so prepare keeps it byte-identical.
	Before that (historical): after prepare, every texel of every
	cols-6/13 cell block (18x18 incl. border, 28 frames) equals the matching
	cols-3/10 texel, and nothing else in the atlas changed.
	(6) F1b (Opus F1 review, LOW): the repair is scale-aware. The bundled atlas
	nearest-scaled to a 16x-scaled 4096x4096, a 128x128 half-scale, and the odd
	300x300 and 383x281 sizes: a real strip is kept byte-identical; a blank strip
	is repaired so every pixel whose centre lies in the cols-6/13 band holds the
	pixel containing that centre 54 atlas units left (checked with independent
	double arithmetic), nothing else changes, every nearest sample of a content
	texel lands on a repaired pixel, and the result classifies as a real strip.
	Under 128 px on an axis the atlas is left untouched.
	(7) source guard: tex_init runs prepare on anim.png BEFORE tex_gfx_load, and
	no consumer or loader carries a fallback flag (the removed
	g_flame_fx_anim_second_strip); the overlay's quad 1 is always col 6/13.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/fire_overlay.h"
#include "graphics/flame_fx.h"
#include "lodepng/lodepng.h"

#define DEFAULT_ATLAS "assets/anim.png"
#define EARTH_ASH_ATLAS "artwork/cc0-pack/earth-and-ash-complete/anim.png"

static unsigned char* load(const char* path, unsigned* w, unsigned* h) {
	unsigned char* rgba = NULL;
	unsigned err = lodepng_decode32_file(&rgba, w, h, path);
	if(err) {
		fprintf(stderr, "cannot decode %s: %s (WORKING_DIRECTORY = repo root?)\n",
				path, lodepng_error_text(err));
		assert(0);
	}
	return rgba;
}

static char* slurp(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f) {
		fprintf(stderr, "cannot open %s\n", path);
		assert(0);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* s = malloc((size_t)n + 1);
	assert(s && fread(s, 1, (size_t)n, f) == (size_t)n);
	s[n] = 0;
	fclose(f);
	return s;
}

enum paint { PAINT_TRANSPARENT, PAINT_MAGENTA };
static void paint_second_strip(unsigned char* rgba, unsigned w, enum paint p,
							   int rows) {
	static const unsigned char magenta[2][4]
		= {{107, 63, 127, 255}, {214, 127, 255, 255}};
	for(int c = 0; c < 2; c++) {
		int col = FLAME_FX_ANIM_COL1 + c * 7;
		for(int row = 0; row < rows; row++)
			for(int y = 0; y < 18; y++)
				for(int x = 0; x < 18; x++) {
					unsigned char* px = rgba
						+ (((size_t)(row * 18 + 2 + y)) * w + (col * 18 + 2 + x)) * 4;
					if(p == PAINT_TRANSPARENT)
						px[3] = 0;
					else
						memcpy(px, magenta[((x / 2) + (y / 2)) & 1], 4);
				}
	}
}

static bool in_second_strip(unsigned x, unsigned y) {
	for(int c = 0; c < 2; c++) {
		unsigned x0 = (unsigned)((FLAME_FX_ANIM_COL1 + c * 7) * 18 + 2);
		if(x >= x0 && x < x0 + 18 && y >= 2 && y < 14 * 18 + 2)
			return true;
	}
	return false;
}

// After prepare: cols 6/13 cell blocks == cols 3/10 cell blocks, texel for
// texel; every texel outside cols 6/13 equals the input image.
static void check_copied(const unsigned char* before, const unsigned char* after,
						 unsigned w, unsigned h, const char* label) {
	int copied = 0;
	for(unsigned y = 0; y < h; y++)
		for(unsigned x = 0; x < w; x++) {
			const unsigned char* a = after + ((size_t)y * w + x) * 4;
			if(in_second_strip(x, y)) {
				unsigned sx = x - (FLAME_FX_ANIM_COL1 - FLAME_FX_ANIM_COL0) * 18;
				assert(memcmp(a, after + ((size_t)y * w + sx) * 4, 4) == 0);
				copied++;
			} else {
				assert(memcmp(a, before + ((size_t)y * w + x) * 4, 4) == 0);
			}
		}
	assert(copied == 2 * 18 * 14 * 18);
	// And the result now classifies as a real strip.
	assert(flame_fx_anim_second_strip_present(after, w, h));
	printf("  %-40s blank 6/13 -> %d texels copied from 3/10\n", label, copied);
}

static void case_blank(const unsigned char* base, unsigned w, unsigned h,
					   enum paint p, int rows, const char* label) {
	size_t bytes = (size_t)w * h * 4;
	unsigned char* before = malloc(bytes);
	unsigned char* after = malloc(bytes);
	assert(before && after);
	memcpy(before, base, bytes);
	paint_second_strip(before, w, p, rows);
	memcpy(after, before, bytes);
	assert(!flame_fx_anim_second_strip_present(after, w, h));
	assert(flame_fx_anim_prepare_pack_atlas(after, w, h)); // copied
	check_copied(before, after, w, h, label);
	free(before);
	free(after);
}

// ---- (6) scale-aware repair. Independent double-precision geometry: atlas
// unit u (0..256) on an n-pixel axis; pixel p's centre is (p+0.5)*256/n.
static double centre_u(unsigned p, unsigned n) { return (p + 0.5) * 256.0 / n; }

static bool band_px(unsigned x, unsigned y, unsigned w, unsigned h) {
	double u = centre_u(x, w), v = centre_u(y, h);
	if(v < 2.0 || v >= 14 * 18 + 2.0)
		return false;
	for(int c = 0; c < 2; c++) {
		double a = (FLAME_FX_ANIM_COL1 + c * 7) * 18 + 2.0;
		if(u >= a && u < a + 18.0)
			return true;
	}
	return false;
}

// Nearest-scale the 256x256 base to w x h (each pixel = the base texel under
// its centre), the way an artist-upscaled pack of any size is laid out.
static void scale_nearest(const unsigned char* base, unsigned char* out, unsigned w,
						  unsigned h) {
	for(unsigned y = 0; y < h; y++)
		for(unsigned x = 0; x < w; x++) {
			unsigned bx = (unsigned)floor(centre_u(x, w));
			unsigned by = (unsigned)floor(centre_u(y, h));
			memcpy(out + ((size_t)y * w + x) * 4, base + ((size_t)by * 256 + bx) * 4, 4);
		}
}

static void case_scaled(const unsigned char* base, unsigned w, unsigned h,
						const char* label) {
	size_t bytes = (size_t)w * h * 4;
	unsigned char* before = malloc(bytes);
	unsigned char* after = malloc(bytes);
	assert(before && after);
	scale_nearest(base, before, w, h);

	// Real strip at this size: classified present, left byte-identical.
	memcpy(after, before, bytes);
	assert(flame_fx_anim_second_strip_present(after, w, h));
	assert(!flame_fx_anim_prepare_pack_atlas(after, w, h));
	assert(memcmp(after, before, bytes) == 0);

	// Blank strip (every band pixel transparent): repaired.
	size_t band = 0;
	for(unsigned y = 0; y < h; y++)
		for(unsigned x = 0; x < w; x++)
			if(band_px(x, y, w, h)) {
				before[((size_t)y * w + x) * 4 + 3] = 0;
				band++;
			}
	memcpy(after, before, bytes);
	assert(!flame_fx_anim_second_strip_present(after, w, h));
	assert(flame_fx_anim_prepare_pack_atlas(after, w, h));
	size_t copied = 0;
	const double shift_px = 54.0 * w / 256.0;
	for(unsigned y = 0; y < h; y++)
		for(unsigned x = 0; x < w; x++) {
			const unsigned char* a = after + ((size_t)y * w + x) * 4;
			if(band_px(x, y, w, h)) {
				unsigned sx = (unsigned)floor(x + 0.5 - shift_px);
				// the source pixel sits in the cols-3/10 block and is untouched
				double su = centre_u(sx, w);
				assert(su > (FLAME_FX_ANIM_COL0 * 18 + 2.0) - 1.0);
				assert(!band_px(sx, y, w, h));
				if(memcmp(a, before + ((size_t)y * w + sx) * 4, 4) != 0) {
					fprintf(stderr, "%s: (%u,%u) != source (%u,%u)\n", label, x, y,
							sx, y);
					assert(0);
				}
				copied++;
			} else {
				assert(memcmp(a, before + ((size_t)y * w + x) * 4, 4) == 0);
			}
		}
	assert(copied == band && copied > 0);
	// Every nearest sample of a cols-6/13 content texel lands on a repaired
	// pixel (quarter-unit steps over [c+3, c+19) x [r+3, r+19)).
	for(int c = 0; c < 2; c++)
		for(int row = 0; row < 14; row++)
			for(int j = 0; j < 64; j++)
				for(int k = 0; k < 64; k++) {
					double u = (FLAME_FX_ANIM_COL1 + c * 7) * 18 + 3 + j / 4.0;
					double v = row * 18 + 3 + k / 4.0;
					unsigned px = (unsigned)floor(u * w / 256.0);
					unsigned py = (unsigned)floor(v * h / 256.0);
					assert(band_px(px, py, w, h));
				}
	assert(flame_fx_anim_second_strip_present(after, w, h));
	printf("  %-40s blank 6/13 -> %zu px copied (scale-aware)\n", label, copied);
	free(before);
	free(after);
}

int main(void) {
	unsigned w, h;
	unsigned char* def = load(DEFAULT_ATLAS, &w, &h);
	size_t bytes = (size_t)w * h * 4;

	// (1) bundled atlas: real strip, untouched.
	unsigned char* img = malloc(bytes);
	assert(img);
	memcpy(img, def, bytes);
	assert(!flame_fx_anim_prepare_pack_atlas(img, w, h));
	assert(memcmp(img, def, bytes) == 0);
	printf("  %-40s real 6/13 -> kept byte-identical\n", "assets/anim.png");

	case_blank(def, w, h, PAINT_TRANSPARENT, 14, "cols 6/13 transparent");  // (2)
	case_blank(def, w, h, PAINT_MAGENTA, 14, "cols 6/13 magenta placeholder"); // (3)
	case_blank(def, w, h, PAINT_TRANSPARENT, 5, "cols 6/13 partly blank"); // (4)

	// (5) the real Earth & Ash anim.png (read only; the copy is in memory).
	unsigned ew, eh;
	unsigned char* ea = load(EARTH_ASH_ATLAS, &ew, &eh);
	unsigned char* ea_before = malloc((size_t)ew * eh * 4);
	assert(ea_before);
	memcpy(ea_before, ea, (size_t)ew * eh * 4);
	assert(!flame_fx_anim_prepare_pack_atlas(ea, ew, eh));
	assert(memcmp(ea_before, ea, (size_t)ew * eh * 4) == 0);
	printf("  %-40s real 6/13 -> kept byte-identical\n", "Earth & Ash anim.png");

	// (6) scale-aware: any size >= 128 px per axis is classified and repaired.
	assert(w == 256 && h == 256);
	case_scaled(def, 4096, 4096, "16x-scaled 4096x4096");
	case_scaled(def, 512, 512, "2x-scaled 512x512");
	case_scaled(def, 128, 128, "half-scale 128x128");
	case_scaled(def, 300, 300, "odd 300x300");
	case_scaled(def, 383, 281, "odd non-square 383x281");
	// ...and under 128 px on an axis (a cell under 9 px) it is left untouched.
	memcpy(img, def, bytes);
	paint_second_strip(img, w, PAINT_TRANSPARENT, 14);
	unsigned char* snap = malloc(bytes);
	assert(snap);
	memcpy(snap, img, bytes);
	assert(!flame_fx_anim_prepare_pack_atlas(img, 127, 256));
	assert(!flame_fx_anim_prepare_pack_atlas(img, 256, 127));
	assert(memcmp(img, snap, bytes) == 0);

	// (7) consumers are unconditional: quad 1 always samples cols 6/13.
	for(int frame = 0; frame < 28; frame++) {
		struct fire_overlay_quad q1 = fire_overlay_quad(frame, 1);
		float u0, v0;
		fire_anim_cell_origin(frame, 1, &u0, &v0);
		assert(q1.uv[2] == u0 && q1.uv[4] == u0 && q1.uv[5] == v0);
		assert((int)(u0 * 256.0f + 0.5f) == (6 + (frame / 14) * 7) * 18 + 3);
	}
	// ...and the loader repairs the image before upload, with no flag anywhere.
	const char* flag = "g_flame_fx_anim_second_strip";
	static const char* const no_flag[] = {
		"source/platform/texture.c", "source/graphics/flame_fx.h",
		"source/graphics/fire_overlay.h", "source/graphics/render_block.c",
		"source/graphics/render_entity_fire.c", "source/main.c",
	};
	for(size_t i = 0; i < sizeof(no_flag) / sizeof(no_flag[0]); i++) {
		char* src = slurp(no_flag[i]);
		if(strstr(src, flag)) {
			fprintf(stderr, "%s still carries %s\n", no_flag[i], flag);
			assert(0);
		}
		free(src);
	}
	char* tex = slurp("source/platform/texture.c");
	const char* rd = strstr(tex, "tex_read_gfx(\"anim.png\"");
	assert(rd && "tex_init must decode anim.png itself");
	const char* prep = strstr(rd, "flame_fx_anim_prepare_pack_atlas(");
	const char* up = strstr(rd, "tex_gfx_load(&texture_anim");
	assert(prep && up && prep < up && "prepare must run BEFORE the upload");
	free(tex);
	printf("  source guard: prepare before tex_gfx_load(&texture_anim), no flag\n");

	free(snap);
	free(ea_before);
	free(ea);
	free(img);
	free(def);
	printf("test_anim_fire_strip_fallback: PASS\n");
	return 0;
}
