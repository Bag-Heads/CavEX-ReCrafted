/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include <assert.h>
#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "../tex_gx_conv.h" /* pack-cache-0927: the shared GX encoders */

// Defined above tex_gfx_load; also used by tex_gfx_load_atlas earlier in the file.
static bool tex_dims_ok(const void* img, size_t width, size_t height,
						enum tex_format type);

static uint8_t conv_color(uint8_t col, size_t depth) {
	return ((col << (8 - depth)) | (col >> (2 * depth - 8)));
}

void tex_init_pre() {
	// GPU FENCE before any texture is replaced (2026-07-31, adversarial review).
	// tex_init is RE-ENTRANT: screen_texturepacks.c and screen_select_world.c both
	// call it at runtime, from the screen update that runs immediately BEFORE
	// gfx_flip_buffers -- so the previous frame's draws are still in flight. The
	// reload path then mem2_free's each old texel buffer (this file's tex_gfx_load
	// and tex_gfx_load_atlas) with no synchronisation, and that memory returns to
	// the very heap the chunk display lists churn through, so the GP could stream
	// display-list bytes as texels. Blocking here once per reload is the cheap fix
	// (vs. a fence per texture) and is the same call gfx_init already makes; the
	// FIFO-overflow deadlock documented in gfx.c was chronic mid-frame saturation,
	// which the 2MB FIFO resolved, not a hazard of a screen-transition fence.
	GX_DrawDone();

	tex_gx_luts_init();
}

void tex_init_post() {
	// GX_TEXMAP1 is a persistent fog binding. tex_init() replaces the backing
	// GXTexObj during a pack reload, so the old binding otherwise references
	// freed storage until a graphics restart that never occurs.
	tex_gfx_bind(&texture_fog, GX_TEXMAP1);
}

void tex_gfx_clear(struct tex_gfx* tex) {
	assert(tex);
	if(tex->data)
		mem2_free(tex->data);
	*tex = (struct tex_gfx) {0};
}

/* pack-cache-0927: the conversions themselves moved VERBATIM in arithmetic to
   tex_gx_conv.c (shared with the PC host tool that bakes Wii cache bytes;
   byte identity proven by tests/test_tex_gx_conv_equiv.c). Placement is
   unchanged: MEM2, tagged CARVE_TEXTURE, flushed for the GP. */
static void* tex_conv(enum tex_format type, uint8_t* image, size_t width,
					  size_t height, size_t* bytes_out) {
	size_t bytes = tex_gx_bytes(type, width, height);
	uint8_t* output = mem2_tag(mem2_alloc(bytes), bytes, CARVE_TEXTURE);
	if(!output)
		return NULL;
	if(!tex_gx_encode(type, image, width, height, output)) {
		mem2_free(output);
		return NULL;
	}
	DCFlushRange(output, bytes);
	*bytes_out = bytes;
	return output;
}

// Build a GX_TF_RGB5A3 mip chain (LODs 0..3) from an RGBA8 atlas with
// the given tile pitch: one 32-byte-aligned MEM2 buffer with all four LODs
// back-to-back, the layout GX expects when GX_InitTexObj is called with the
// mipmap flag set. Total size is width*height*2 * 85/64 ~= 1.33x LOD 0.
// A level that is not 4-aligned (a 16-multiple that isn't a power of two,
// e.g. 336) or a failed allocation returns NULL and the caller falls back to
// the single-LOD path. The chain is encoded straight into the combined
// buffer (pack-cache-0927: the old per-LOD MEM2 staging buffers + memcpy
// produced the identical bytes with a higher peak).
enum { LODS = TEX_GX_ATLAS_LODS };

static void* tex_build_mip_chain_rgba16(uint8_t* image, size_t width,
										size_t height,
										const struct tex_atlas_grid* grid,
										size_t* out_total) {
	size_t total = tex_gx_atlas_bytes(width, height);
	if(!total)
		return NULL;
	uint8_t* combined = mem2_tag(mem2_alloc(total), total, CARVE_TEXTURE); // persistent GX mip buffer -> MEM2
	if(!combined)
		return NULL;
	if(!tex_gx_encode_atlas(image, width, height, grid, combined)) {
		mem2_free(combined); // MEM1 downsample alloc failure
		return NULL;
	}
	DCFlushRange(combined, total);
	*out_total = total;
	return combined;
}

bool tex_gfx_load_atlas(struct tex_gfx* tex, void* img, size_t width,
						size_t height, enum tex_format type,
						const struct tex_atlas_grid* grid) {
	assert(tex && img && width > 0 && height > 0 && grid);

	// Only the atlas format currently flows through here in this
	// build (terrain.png, particles.png). Anything else falls back to
	// the existing single-LOD path so we don't silently change
	// behaviour for callers not asking for mipmaps.
	if(type != TEX_FMT_RGBA16) {
		return tex_gfx_load(tex, img, width, height, type, true);
	}

	if(!tex_dims_ok(img, width, height, type) || !grid || !grid->cell_pitch
	   || grid->origin_x >= width || grid->origin_y >= height) {
		fprintf(stderr,
				"[texture] rejecting %ux%u atlas (GX needs 4-aligned dims "
				"<=1024)\n",
				(unsigned)width, (unsigned)height);
		free(img);
		return false;
	}

	size_t total;
	void* output
		= tex_build_mip_chain_rgba16(img, width, height, grid, &total);
	if(!output) {
		// Mip chain unbuildable (odd-but-valid dims at deep LODs, or MEM1/MEM2
		// pressure -- the latter is exactly the crashed-run condition; the old
		// assert(output) compiled out under NDEBUG and memcpy'd through NULL).
		// Fall back to the plain single-LOD load: worse minification, alive.
		fprintf(stderr,
				"[texture] atlas mip chain failed (%ux%u); falling back to "
				"single-LOD load\n",
				(unsigned)width, (unsigned)height);
		return tex_gfx_load(tex, img, width, height, type, true);
	}

	if(tex->data) // re-entrant reload (pack switch): free the previous atlas first
		mem2_free(tex->data);
	tex->fmt = type;
	tex->data = output;
	tex->width = width;
	tex->height = height;
	tex->data_bytes = total;
	tex->lods = LODS;
	tex->linear = true;

	// GX_TRUE here tells GX that `output` is the head of a mip chain
	// laid out LOD 0..N back-to-back.
	GX_InitTexObj(&tex->obj, output, width, height, GX_TF_RGB5A3, GX_CLAMP,
				  GX_CLAMP, GX_TRUE);
	GX_InitTexObjMaxAniso(&tex->obj, GX_ANISO_1);
	// Minification is trilinear (linear within each LOD + linear between LODs)
	// so distant terrain stays clean; max_lod = 3 because we built 4 levels
	// (0..3). Magnification is GX_NEAR so close-up texels stay crisp and blocky
	// like Java Minecraft, instead of being smeared by bilinear upscaling.
	GX_InitTexObjLOD(&tex->obj, GX_LIN_MIP_LIN, GX_NEAR, 0.0f, 3.0f, 0.0f,
					 GX_FALSE, GX_FALSE, GX_ANISO_1);
	GX_InvalidateTexAll();

	free(img);
	return true;
}

// RUNTIME dimension gate (not just asserts -- the shipping Wii build is NDEBUG,
// and USER texture-pack files reach the tex_conv_* converters). The converters
// walk 4x4 (16-bit) / 8x4 (8-bit) GX tiles: a non-aligned image makes them read
// past the source AND write past the mem2_alloc'd output -- a MEM2 heap OVERRUN,
// the exact lwp_heap free-list corruption class behind the real-Wii watchdog
// freeze. GX also caps texture dimensions at 1024. Reject -> the texture stays
// unloaded (renderers fall back / skip), the console stays alive.
static bool tex_dims_ok(const void* img, size_t width, size_t height,
						enum tex_format type) {
	return img && tex_gfx_dimensions_valid(width, height, type);
}

static uint8_t tex_gx_format(enum tex_format type) {
	switch(type) {
		case TEX_FMT_RGBA32: return GX_TF_RGBA8;
		case TEX_FMT_RGBA16: return GX_TF_RGB5A3;
		case TEX_FMT_RGB16: return GX_TF_RGB565;
		case TEX_FMT_I8: return GX_TF_I8;
		case TEX_FMT_IA4: return GX_TF_IA4;
		default: return 0xFF;
	}
}

/* pack-cache-0927: the cache's GX-byte buffers live where tex_conv puts
   them (MEM2, CARVE_TEXTURE) so a restored texture is indistinguishable from
   a converted one, and tex_gfx_clear frees it the same way. */
void* tex_cache_texel_alloc(size_t bytes) {
	return mem2_tag(mem2_alloc(bytes), bytes, CARVE_TEXTURE);
}

void tex_cache_texel_free(void* p) {
	if(p)
		mem2_free(p);
}

/* Install cache-restored GX bytes exactly as tex_gfx_load (lods 1) or
   tex_gfx_load_atlas (lods 4) finishes after its conversion: same GXTexObj
   calls, same fields. Takes ownership of `data` on every path. */
bool tex_gfx_restore(struct tex_gfx* tex, void* data, size_t data_bytes,
					 size_t width, size_t height, enum tex_format type,
					 unsigned lods, bool linear, bool atlas) {
	(void)atlas;
	uint8_t fmt = tex_gx_format(type);
	bool shape_ok = lods == 1
		? data_bytes == tex_gx_bytes(type, width, height)
		: (lods == LODS && type == TEX_FMT_RGBA16
		   && data_bytes == tex_gx_atlas_bytes(width, height));
	if(fmt == 0xFF || !shape_ok
	   || !tex_gfx_dimensions_valid(width, height, type)) {
		tex_cache_texel_free(data);
		return false;
	}
	DCFlushRange(data, data_bytes);
	if(tex->data)
		mem2_free(tex->data);
	tex->fmt = type;
	tex->data = data;
	tex->width = width;
	tex->height = height;
	tex->data_bytes = data_bytes;
	tex->lods = (uint8_t)lods;
	tex->linear = linear;
	if(lods == LODS) {
		GX_InitTexObj(&tex->obj, data, width, height, GX_TF_RGB5A3, GX_CLAMP,
					  GX_CLAMP, GX_TRUE);
		GX_InitTexObjMaxAniso(&tex->obj, GX_ANISO_1);
		GX_InitTexObjLOD(&tex->obj, GX_LIN_MIP_LIN, GX_NEAR, 0.0f, 3.0f, 0.0f,
						 GX_FALSE, GX_FALSE, GX_ANISO_1);
	} else {
		GX_InitTexObj(&tex->obj, data, width, height, fmt, GX_CLAMP, GX_CLAMP,
					  GX_FALSE);
		GX_InitTexObjMaxAniso(&tex->obj, GX_ANISO_1);
		GX_InitTexObjFilterMode(&tex->obj, linear ? GX_LINEAR : GX_NEAR,
								linear ? GX_LINEAR : GX_NEAR);
	}
	GX_InvalidateTexAll();
	return true;
}

bool tex_gfx_load(struct tex_gfx* tex, void* img, size_t width, size_t height,
				  enum tex_format type, bool linear) {
	assert(tex && img && width > 0 && height > 0);

	if(!tex_dims_ok(img, width, height, type)) {
		fprintf(stderr,
				"[texture] rejecting %ux%u texture (GX needs 4/8-aligned dims "
				"<=1024)\n",
				(unsigned)width, (unsigned)height);
		free(img); // same ownership as the success path below
		return false;
	}

	void* output = NULL;
	size_t bytes = 0;
	uint8_t fmt = tex_gx_format(type);

	if(fmt == 0xFF) {
		free(img);
		return false;
	}
	output = tex_conv(type, img, width, height, &bytes);

	if(output) {
		if(tex->data) // re-entrant reload (pack switch): free the previous texture
			mem2_free(tex->data);
		tex->fmt = type;
		tex->data = output;
		tex->width = width;
		tex->height = height;
		tex->data_bytes = bytes;
		tex->lods = 1;
		tex->linear = linear;

		GX_InitTexObj(&tex->obj, output, width, height, fmt, GX_CLAMP, GX_CLAMP,
					  GX_FALSE);
		GX_InitTexObjMaxAniso(&tex->obj, GX_ANISO_1);
		GX_InitTexObjFilterMode(&tex->obj, linear ? GX_LINEAR : GX_NEAR,
								linear ? GX_LINEAR : GX_NEAR);
		GX_InvalidateTexAll();
	}

	free(img);
	return output != NULL;
}

// In-place LOD0 update of the RGB5A3 terrain atlas (animated water FX). LOD0 ONLY:
// near water animates; distant water keeps its static (blue) mip -- both blue, the
// seam is sub-perceptible under fog, and this avoids the intricate per-LOD origin math
// (a documented T2 limitation, follow-up to extend to all mips). Safe vs the freeze
// class: no allocation, destination provably inside the atlas, store-then-invalidate.
void tex_gfx_subimage(struct tex_gfx* tex, int x, int y, int w, int h,
					  const uint8_t* rgba) {
	assert(tex && rgba && w > 0 && h > 0);
	if(!tex->data || tex->fmt != TEX_FMT_RGBA16)
		return;
	int aw = (int)tex->width, ah = (int)tex->height;
	if(x < 0 || y < 0 || x + w > aw || y + h > ah)
		return; // never write outside LOD0
	uint16_t* lod0 = (uint16_t*)tex->data; // LOD0 sits at the head of the mip chain
	for(int ty = 0; ty < h; ty++) {
		for(int tx = 0; tx < w; tx++) {
			const uint8_t* c = rgba + ((size_t)ty * w + tx) * 4;
			// same RGB5A3 quantize path as the atlas conversion (tex_gx_conv)
			lod0[water_fx_swizzle_index(x + tx, y + ty, aw)] = tex_gx_rgb5a3(c);
		}
	}
	// Flush the touched 4x4 blocks (block-aligned contiguous superset) then drop the
	// stale GP TMEM copy so the next draw refetches -- the loader's discipline.
	int bw = aw / 4;
	int bmin = (x / 4) + (y / 4) * bw;
	int bmax = ((x + w - 1) / 4) + ((y + h - 1) / 4) * bw;
	DCStoreRange(lod0 + (size_t)bmin * 16,
				 (size_t)(bmax - bmin + 1) * 16 * sizeof(uint16_t));
	GX_InvalidateTexAll();
}

void tex_gfx_wrap_mode(struct tex_gfx* tex, bool repeat) {
	assert(tex);
	GX_InitTexObjWrapMode(&tex->obj, repeat ? GX_REPEAT : GX_CLAMP,
						  repeat ? GX_REPEAT : GX_CLAMP);
}

void tex_gfx_bind(struct tex_gfx* tex, int slot) {
	assert(tex && slot >= GX_TEXMAP0 && slot < GX_MAX_TEXMAP);
	// A texture that never loaded (file absent in the pack -- shadow/water/cart
	// on a default install -- or rejected by tex_dims_ok) has a ZEROED GXTexObj:
	// loading it feeds the GP malformed register state (format 0, size 0,
	// physical address 0). Keep the previously bound texture instead -- visually
	// wrong for that draw, but valid GP state. (Draws whose entire purpose is
	// the missing texture should skip themselves; see entity_shadow / the
	// underwater overlay.)
	if(!tex->data)
		return;
	GX_LoadTexObj(&tex->obj, slot);
}

void tex_gfx_lookup(struct tex_gfx* tex, int x, int y, uint8_t* color) {
	assert(tex && color);

	x = ((unsigned int)x) % tex->width;
	y = ((unsigned int)y) % tex->height;

	switch(tex->fmt) {
		case TEX_FMT_IA4: {
			uint8_t* blk
				= tex->data + ((x / 8) + (y / 4) * (tex->width / 8)) * 32;
			uint8_t pixel = blk[(x % 8) + (y % 4) * 8];
			color[0] = color[1] = color[2] = conv_color(pixel & 0x0F, 4);
			color[3] = conv_color((pixel & 0xF0) >> 4, 4);
		} break;
		case TEX_FMT_I8: {
			uint8_t* blk
				= tex->data + ((x / 8) + (y / 4) * (tex->width / 8)) * 32;
			uint8_t pixel = blk[(x % 8) + (y % 4) * 8];
			color[0] = color[1] = color[2] = color[3] = pixel;
		} break;
		default: assert(false);
	}
}
