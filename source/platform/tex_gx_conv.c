/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Platform-neutral GX encoders. Contract: tex_gx_conv.h. The arithmetic is
   the original wii/texture.c conversion code (ByteBit/xtreme8000, GPLv3),
   with each 16-bit store written as two big-endian bytes. */

#include "tex_gx_conv.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "texture.h"

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
static bool s_luts_ready;

static uint8_t conv_color(uint8_t col, size_t depth) {
	return ((col << (8 - depth)) | (col >> (2 * depth - 8)));
}

static void lut_gen(int depth, uint8_t* lut) {
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

void tex_gx_luts_init(void) {
	if(s_luts_ready)
		return;
	lut_gen(3, lut_3);
	lut_gen(4, lut_4);
	lut_gen(5, lut_5);
	lut_gen(6, lut_6);
	s_luts_ready = true;
}

static inline void put16(uint8_t* out, size_t idx, int v) {
	out[idx * 2] = (uint8_t)(v >> 8);
	out[idx * 2 + 1] = (uint8_t)v;
}

uint16_t tex_gx_rgb5a3(const uint8_t* col) {
	int alpha = lut_3[col[3]];
	if(alpha < 7)
		return (uint16_t)rgba16(lut_4[col[0]], lut_4[col[1]], lut_4[col[2]],
								alpha);
	return (uint16_t)rgb16_2(lut_5[col[0]], lut_5[col[1]], lut_5[col[2]]);
}

void tex_gx_rgb5a3_decode(uint16_t v, uint8_t* rgba) {
	if(v & 0x8000) {
		for(int k = 0; k < 3; k++) {
			uint8_t c = (v >> (10 - 5 * k)) & 0x1F;
			rgba[k] = (uint8_t)((c << 3) | (c >> 2));
		}
		rgba[3] = 255;
	} else {
		for(int k = 0; k < 3; k++)
			rgba[k] = (uint8_t)(((v >> (8 - 4 * k)) & 0x0F) * 17);
		/* lut_3 is built from conv_color(i, 3) == i << 5 (its low-bit term
		 * shifts by a negative count and is 0 on both hosts), so expand the
		 * same way and the encoder maps it straight back. */
		rgba[3] = (uint8_t)(((v >> 12) & 0x07) << 5);
	}
}

size_t tex_gx_bytes(enum tex_format type, size_t width, size_t height) {
	switch(type) {
		case TEX_FMT_RGBA32: return width * height * 4;
		case TEX_FMT_RGBA16:
		case TEX_FMT_RGB16: return width * height * 2;
		case TEX_FMT_I8:
		case TEX_FMT_IA4: return width * height;
		default: return 0;
	}
}

static void conv_rgba32(const uint8_t* image, size_t width, size_t height,
						uint8_t* out) {
	size_t idx = 0;
	for(size_t y = 0; y < height; y += 4) {
		for(size_t x = 0; x < width; x += 4) {
			for(size_t by = 0; by < 4; by++)
				for(size_t bx = 0; bx < 4; bx++) {
					const uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					put16(out, idx++, xy16(col[3], col[0]));
				}
			for(size_t by = 0; by < 4; by++)
				for(size_t bx = 0; bx < 4; bx++) {
					const uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					put16(out, idx++, xy16(col[1], col[2]));
				}
		}
	}
}

static void conv_rgba16(const uint8_t* image, size_t width, size_t height,
						uint8_t* out) {
	size_t idx = 0;
	for(size_t y = 0; y < height; y += 4)
		for(size_t x = 0; x < width; x += 4)
			for(size_t by = 0; by < 4; by++)
				for(size_t bx = 0; bx < 4; bx++)
					put16(out, idx++,
						  tex_gx_rgb5a3(image + (x + bx + (y + by) * width) * 4));
}

static void conv_rgb16(const uint8_t* image, size_t width, size_t height,
					   uint8_t* out) {
	size_t idx = 0;
	for(size_t y = 0; y < height; y += 4)
		for(size_t x = 0; x < width; x += 4)
			for(size_t by = 0; by < 4; by++)
				for(size_t bx = 0; bx < 4; bx++) {
					const uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					put16(out, idx++,
						  rgb16(lut_5[col[0]], lut_6[col[1]], lut_5[col[2]]));
				}
}

static void conv_i8(const uint8_t* image, size_t width, size_t height,
					uint8_t* out) {
	size_t idx = 0;
	for(size_t y = 0; y < height; y += 4)
		for(size_t x = 0; x < width; x += 8)
			for(size_t by = 0; by < 4; by++)
				for(size_t bx = 0; bx < 8; bx++) {
					const uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					out[idx++] = ((int)col[0] + (int)col[1] + (int)col[2]) / 3;
				}
}

static void conv_ia4(const uint8_t* image, size_t width, size_t height,
					 uint8_t* out) {
	size_t idx = 0;
	for(size_t y = 0; y < height; y += 4)
		for(size_t x = 0; x < width; x += 8)
			for(size_t by = 0; by < 4; by++)
				for(size_t bx = 0; bx < 8; bx++) {
					const uint8_t* col = image + (x + bx + (y + by) * width) * 4;
					out[idx++] = ia8(
						lut_4[((int)col[0] + (int)col[1] + (int)col[2]) / 3],
						lut_4[col[3]]);
				}
}

bool tex_gx_encode(enum tex_format type, const uint8_t* rgba, size_t width,
				   size_t height, uint8_t* out) {
	tex_gx_luts_init();
	switch(type) {
		case TEX_FMT_RGBA32: conv_rgba32(rgba, width, height, out); return true;
		case TEX_FMT_RGBA16: conv_rgba16(rgba, width, height, out); return true;
		case TEX_FMT_RGB16: conv_rgb16(rgba, width, height, out); return true;
		case TEX_FMT_I8: conv_i8(rgba, width, height, out); return true;
		case TEX_FMT_IA4: conv_ia4(rgba, width, height, out); return true;
		default: return false;
	}
}

size_t tex_gx_atlas_bytes(size_t width, size_t height) {
	struct tex_atlas_grid grid;
	if(!tex_atlas_grid_init(&grid, width, height)
	   || tex_atlas_mip_levels(&grid) != TEX_GX_ATLAS_LODS)
		return 0;
	size_t total = 0;
	for(int n = 0; n < TEX_GX_ATLAS_LODS; n++) {
		size_t lw = width >> n, lh = height >> n;
		/* Each level must stay 4-aligned for the RGB5A3 tile encoder. */
		if((lw % 4) != 0 || (lh % 4) != 0)
			return 0;
		total += lw * lh * 2;
	}
	return total;
}

bool tex_gx_encode_atlas(const uint8_t* rgba, size_t width, size_t height,
						 const struct tex_atlas_grid* grid, uint8_t* out) {
	if(!tex_gx_atlas_bytes(width, height)
	   || tex_atlas_mip_levels(grid) != TEX_GX_ATLAS_LODS)
		return false;
	tex_gx_luts_init();
	size_t off = 0;
	for(int n = 0; n < TEX_GX_ATLAS_LODS; n++) {
		size_t lw = width >> n, lh = height >> n;
		if((lw % 4) != 0 || (lh % 4) != 0)
			return false;
		const uint8_t* level = rgba;
		uint8_t* tmp = NULL;
		if(n > 0) {
			tmp = tex_mip_downsample_rgba8(rgba, width, height, grid,
										   (unsigned)n);
			if(!tmp)
				return false;
			level = tmp;
		}
		conv_rgba16(level, lw, lh, out + off);
		free(tmp);
		off += lw * lh * 2;
	}
	return true;
}
