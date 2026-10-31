/*
	pack-cache-0927: byte identity of the shared GX encoders
	(source/platform/tex_gx_conv.c) with the ORIGINAL Wii conversion code.

	The block between the REFERENCE markers is the pre-change
	source/platform/wii/texture.c (commit 1c32d356) copied verbatim by
	script, with only the platform calls replaced: mem2_alloc/mem2_tag ->
	malloc, mem2_free -> free, DCFlushRange removed, function names prefixed
	ref_. Its uint16 stores are native; the Wii is big-endian, so the Wii's
	memory image of a reference buffer is each uint16 as (hi, lo). The shared
	encoder writes bytes in that order explicitly; this test compares them
	over every format, many sizes and random / edge-valued images, plus the
	4-LOD atlas mip chain and the RGB5A3 single texel (tex_gfx_subimage).
*/

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/tex_gx_conv.h"
#include "platform/texture.h"

/* ---- REFERENCE (verbatim copy, see header) ---- */
#define rgba16(r, g, b, a)                                                     \
	(((int)(a) << 12) | ((int)(r) << 8) | ((int)(g) << 4) | (int)(b))

#define rgb16_2(r, g, b)                                                       \
	(0x8000 | ((int)(r) << 10) | ((int)(g) << 5) | (int)(b))
#define rgb16(r, g, b) (((int)(r) << 11) | ((int)(g) << 5) | (int)(b))
#define xy16(x, y) (((int)(x) << 8) | (int)(y))
#define ia8(i, a) (((int)(a) << 4) | (int)(i))

static uint8_t lut_3[256];
static uint8_t lut_4[256];
static uint8_t lut_5[256];
static uint8_t lut_6[256];

static uint8_t conv_color(uint8_t col, size_t depth) {
	return ((col << (8 - depth)) | (col >> (2 * depth - 8)));
}

static void lut_gen(int depth, uint8_t* lut) {
	assert(depth >= 1 && depth <= 8 && lut);

	for(int k = 0; k < 256; k++) {
		int best = 0;
		int best_error = INT_MAX;

		for(int i = 0; i < (1 << depth); i++) {
			int col = conv_color(i, depth);
			int error = abs(col - k);

			if(error < best_error) {
				best_error = error;
				best = i;
			}
		}

		lut[k] = best;
	}
}

static void* ref_tex_conv_rgba32(uint8_t* image, size_t width, size_t height) {
	assert(image && (width % 4) == 0 && (height % 4) == 0);
	uint16_t* output = malloc(width * height * sizeof(uint16_t) * 2);

	if(!output)
		return NULL;

	size_t output_idx = 0;

	for(size_t y = 0; y < height; y += 4) {
		for(size_t x = 0; x < width; x += 4) {
			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 4; bx++) {
					uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					output[output_idx++] = xy16(col[3], col[0]);
				}
			}

			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 4; bx++) {
					uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					output[output_idx++] = xy16(col[1], col[2]);
				}
			}
		}
	}

	return output;
}

static void* ref_tex_conv_rgba16(uint8_t* image, size_t width, size_t height) {
	assert(image && (width % 4) == 0 && (height % 4) == 0);
	uint16_t* output = malloc(width * height * sizeof(uint16_t));

	if(!output)
		return NULL;

	size_t output_idx = 0;

	for(size_t y = 0; y < height; y += 4) {
		for(size_t x = 0; x < width; x += 4) {
			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 4; bx++) {
					uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					int alpha = lut_3[col[3]];

					if(alpha < 7) {
						output[output_idx++] = rgba16(
							lut_4[col[0]], lut_4[col[1]], lut_4[col[2]], alpha);
					} else {
						output[output_idx++] = rgb16_2(
							lut_5[col[0]], lut_5[col[1]], lut_5[col[2]]);
					}
				}
			}
		}
	}

	return output;
}

static void* ref_tex_conv_rgb16(uint8_t* image, size_t width, size_t height) {
	assert(image && (width % 4) == 0 && (height % 4) == 0);
	uint16_t* output = malloc(width * height * sizeof(uint16_t));

	if(!output)
		return NULL;

	size_t output_idx = 0;

	for(size_t y = 0; y < height; y += 4) {
		for(size_t x = 0; x < width; x += 4) {
			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 4; bx++) {
					uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					output[output_idx++]
						= rgb16(lut_5[col[0]], lut_6[col[1]], lut_5[col[2]]);
				}
			}
		}
	}

	return output;
}

static void* ref_tex_conv_i8(uint8_t* image, size_t width, size_t height) {
	assert(image && (width % 8) == 0 && (height % 4) == 0);
	uint8_t* output = malloc(width * height * sizeof(uint8_t));

	if(!output)
		return NULL;

	size_t output_idx = 0;

	for(size_t y = 0; y < height; y += 4) {
		for(size_t x = 0; x < width; x += 8) {
			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 8; bx++) {
					uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					output[output_idx++]
						= ((int)col[0] + (int)col[1] + (int)col[2]) / 3;
				}
			}
		}
	}

	return output;
}

static void* ref_tex_conv_ia4(uint8_t* image, size_t width, size_t height) {
	assert(image && (width % 8) == 0 && (height % 4) == 0);
	uint8_t* output = malloc(width * height * sizeof(uint8_t));

	if(!output)
		return NULL;

	size_t output_idx = 0;

	for(size_t y = 0; y < height; y += 4) {
		for(size_t x = 0; x < width; x += 8) {
			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 8; bx++) {
					uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					output[output_idx++] = ia8(
						lut_4[((int)col[0] + (int)col[1] + (int)col[2]) / 3],
						lut_4[col[3]]);
				}
			}
		}
	}

	return output;
}

// Build a GX_TF_RGB5A3 mip chain (LODs 0..3) from an RGBA8 atlas with
// the given tile pitch. Returns a single 32-byte-aligned buffer with
// all four LODs laid out back-to-back, the layout GX expects when
// GX_InitTexObj is called with the mipmap flag set. Total size is
// width*height*2 * 85/64 ≈ 1.33× the LOD 0 size.
// enum gives a proper integer-constant-expression so the arrays below
// aren't VLAs (which C99 forbids initializing). devkitPPC's stricter
// c99-pedantic mode caught this where MSYS gcc had let it slide.
enum { LODS = 4 };

static void* ref_tex_build_mip_chain_rgba16(uint8_t* image, size_t width,
										size_t height,
										const struct tex_atlas_grid* grid,
										size_t* out_total) {
	void* lod_bufs[LODS] = {0};
	size_t lod_sizes[LODS] = {0};
	uint8_t* tmp_rgba[LODS] = {0};
	size_t total = 0;

	tmp_rgba[0] = image; // LOD 0 = the source the caller handed us
	for(int n = 0; n < LODS; n++) {
		size_t lw = width >> n;
		size_t lh = height >> n;
		// Each LOD must remain divisible by 4 — the RGB5A3 tile encoder
		// reads 4×4 source blocks. For a 256-wide atlas this holds
		// through LOD 6, well past our LODS=4 cap. RUNTIME-checked (NDEBUG
		// ships): a 16-multiple that isn't a power of two (e.g. 336) breaks
		// at deeper LODs -- bail and let the caller fall back to the
		// single-LOD path rather than overrun the tile encoder.
		if((lw % 4) != 0 || (lh % 4) != 0)
			goto fail;

		if(n > 0) {
			tmp_rgba[n] = tex_mip_downsample_rgba8(image, width, height, grid,
											(unsigned)n);
			if(!tmp_rgba[n])
				goto fail; // MEM1 alloc failure (the crashed run was HERE-adjacent)
		}

		lod_bufs[n] = ref_tex_conv_rgba16(tmp_rgba[n], lw, lh);
		if(!lod_bufs[n])
			goto fail; // MEM2 alloc failure
		lod_sizes[n] = lw * lh * sizeof(uint16_t);
		total += lod_sizes[n];
	}

	{
		uint8_t* combined = malloc(total); 
		if(!combined)
			goto fail;

		size_t off = 0;
		for(int n = 0; n < LODS; n++) {
			memcpy(combined + off, lod_bufs[n], lod_sizes[n]);
			off += lod_sizes[n];
			free(lod_bufs[n]); // per-LOD tex_conv buffers are MEM2 -> mem2_free
			if(n > 0)
				free(tmp_rgba[n]); // LOD 0 is `image`, owned by caller (plain malloc)
		}
		*out_total = total;
		return combined;
	}

fail:
	// Unwind whatever this attempt allocated; LOD 0 (`image`) stays caller-owned.
	for(int n = 0; n < LODS; n++) {
		if(lod_bufs[n])
			free(lod_bufs[n]);
		if(n > 0 && tmp_rgba[n])
			free(tmp_rgba[n]);
	}
	return NULL;
}

/* ---- end REFERENCE ---- */

static uint32_t s_rng = 0x12345678u;
static uint32_t rnd(void) {
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static unsigned failures, checks;

static void fill(uint8_t* img, size_t n, int mode) {
	static const uint8_t edges[] = {0, 1, 7, 8, 15, 16, 31, 32, 127, 128,
									223, 224, 239, 240, 254, 255};
	for(size_t k = 0; k < n; k++)
		img[k] = mode == 0 ? (uint8_t)rnd()
			: mode == 1	   ? edges[rnd() % sizeof(edges)]
						   : (uint8_t)(k * 37u + (k >> 7));
}

static bool same16(const uint16_t* ref, const uint8_t* be, size_t n) {
	for(size_t k = 0; k < n; k++)
		if(be[2 * k] != (uint8_t)(ref[k] >> 8)
		   || be[2 * k + 1] != (uint8_t)ref[k])
			return false;
	return true;
}

static void check(bool ok, const char* what, size_t w, size_t h) {
	checks++;
	if(!ok) {
		failures++;
		printf("FAIL %s %zux%zu\n", what, w, h);
	}
}

int main(void) {
	lut_gen(3, lut_3);
	lut_gen(4, lut_4);
	lut_gen(5, lut_5);
	lut_gen(6, lut_6);
	tex_gx_luts_init();

	static const size_t sizes[][2] = {{8, 4}, {16, 16}, {24, 8}, {64, 32},
									  {128, 128}, {256, 256}, {40, 40},
									  {256, 128}, {1024, 8}, {8, 1024}};
	for(size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		size_t w = sizes[s][0], h = sizes[s][1];
		uint8_t* img = malloc(w * h * 4);
		uint8_t* out = malloc(w * h * 4);
		assert(img && out);
		for(int mode = 0; mode < 3; mode++) {
			fill(img, w * h * 4, mode);
			uint16_t* r;
			uint8_t* r8;

			r = ref_tex_conv_rgba32(img, w, h);
			assert(tex_gx_bytes(TEX_FMT_RGBA32, w, h) == w * h * 4);
			check(tex_gx_encode(TEX_FMT_RGBA32, img, w, h, out)
					  && same16(r, out, w * h * 2),
				  "rgba32", w, h);
			free(r);

			r = ref_tex_conv_rgba16(img, w, h);
			assert(tex_gx_bytes(TEX_FMT_RGBA16, w, h) == w * h * 2);
			check(tex_gx_encode(TEX_FMT_RGBA16, img, w, h, out)
					  && same16(r, out, w * h),
				  "rgb5a3", w, h);
			bool texel_ok = true;
			for(size_t k = 0; k < 64 && k < w * h; k++) {
				size_t i = rnd() % (w * h);
				size_t x = i % w, y = i / w;
				/* position of (x,y) in the 4x4-tiled reference */
				size_t t = ((y / 4) * (w / 4) + x / 4) * 16 + (y % 4) * 4 + x % 4;
				texel_ok = texel_ok && tex_gx_rgb5a3(img + i * 4) == r[t];
			}
			check(texel_ok, "rgb5a3 single texel", w, h);
			free(r);

			r = ref_tex_conv_rgb16(img, w, h);
			check(tex_gx_encode(TEX_FMT_RGB16, img, w, h, out)
					  && same16(r, out, w * h),
				  "rgb565", w, h);
			free(r);

			if(w % 8 == 0) {
				r8 = ref_tex_conv_i8(img, w, h);
				assert(tex_gx_bytes(TEX_FMT_I8, w, h) == w * h);
				check(tex_gx_encode(TEX_FMT_I8, img, w, h, out)
						  && memcmp(r8, out, w * h) == 0,
					  "i8", w, h);
				free(r8);
				r8 = ref_tex_conv_ia4(img, w, h);
				check(tex_gx_encode(TEX_FMT_IA4, img, w, h, out)
						  && memcmp(r8, out, w * h) == 0,
					  "ia4", w, h);
				free(r8);
			}
		}
		free(img);
		free(out);
	}

	/* The atlas mip chain: the atlas sizes a pack can ship (square, 16-
	   multiples) including one whose deep LOD is not 4-aligned (336 ->
	   both sides refuse and fall back). */
	static const size_t atlases[] = {128, 256, 512, 336, 1024};
	for(size_t a = 0; a < sizeof(atlases) / sizeof(atlases[0]); a++) {
		size_t w = atlases[a];
		struct tex_atlas_grid grid;
		assert(tex_atlas_grid_init(&grid, w, w));
		uint8_t* img = malloc(w * w * 4);
		assert(img);
		fill(img, w * w * 4, 0);
		size_t ref_total = 0;
		uint16_t* r = ref_tex_build_mip_chain_rgba16(img, w, w, &grid,
													 &ref_total);
		size_t total = tex_gx_atlas_bytes(w, w);
		if(!r) {
			check(total == 0, "atlas chain refusal agrees", w, w);
		} else {
			uint8_t* out = malloc(total ? total : 1);
			assert(out);
			check(total == ref_total
					  && tex_gx_encode_atlas(img, w, w, &grid, out)
					  && same16(r, out, total / 2),
				  "atlas 4-LOD chain", w, w);
			free(out);
			free(r);
		}
		free(img);
	}

	printf("%s test_tex_gx_conv_equiv (%u checks, %u failure%s)\n",
		   failures ? "FAIL" : "PASS", checks, failures,
		   failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
