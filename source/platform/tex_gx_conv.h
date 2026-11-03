/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	GX texture encoders, platform-neutral (pack-cache-0927).

	The exact RGBA8 -> GX tile conversions the Wii texture backend
	(wii/texture.c) has always used -- RGBA8 (AR/GB tile pairs), RGB5A3,
	RGB565, I8, IA4, and the RGB5A3 atlas mip chain -- moved here
	unchanged in arithmetic so that the PC host tool (tools/audio_cache_tool)
	can produce the Wii's texture bytes for the pack cache. Output is written
	as BYTES in the GX's big-endian order, so the buffer is identical on the
	Wii (where it used to be native uint16 stores) and on a little-endian
	host. Byte identity with the original Wii functions is proven by
	tests/test_tex_gx_conv_equiv.c (a verbatim reference copy).

	No allocation except tex_gx_encode_atlas's temporary downsampled LODs
	(plain malloc, as before); the caller owns every output buffer, so the Wii
	keeps its MEM2 placement and cache flushes.
*/

#ifndef TEX_GX_CONV_H
#define TEX_GX_CONV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "texture.h" /* enum tex_format, struct tex_atlas_grid */

/* Bumped whenever any encoder's output for the same input could change. */
#define TEX_GX_CONV_VERSION 2u
/* Shared PC/GX isolation cap (LODs 0..1). */
#define TEX_GX_ATLAS_LODS TEX_ATLAS_MIP_LEVELS

/* Build the quantization tables (idempotent, cheap). */
void tex_gx_luts_init(void);

/* Bytes of one GX image of `type` (no mips). */
size_t tex_gx_bytes(enum tex_format type, size_t width, size_t height);

/* Encode one level. `rgba` is width*height RGBA8; dims must satisfy
   tex_gfx_dimensions_valid(width, height, type). False = unknown format. */
bool tex_gx_encode(enum tex_format type, const uint8_t* rgba, size_t width,
				   size_t height, uint8_t* out);

/* One RGB5A3 texel (the tex_gfx_subimage path). */
uint16_t tex_gx_rgb5a3(const uint8_t* rgba);
/* Inverse of tex_gx_rgb5a3: one RGB5A3 texel to 8-bit RGBA. */
void tex_gx_rgb5a3_decode(uint16_t v, uint8_t* rgba);

/* Total bytes of the RGB5A3 atlas mip chain, or 0 when a level would not be
   4-aligned or the grid cannot isolate linear samples (nearest fallback). */
size_t tex_gx_atlas_bytes(size_t width, size_t height);

/* Encode the chain LOD 0..1 back to back into `out` (tex_gx_atlas_bytes).
   False on a temporary allocation failure (the caller falls back). */
bool tex_gx_encode_atlas(const uint8_t* rgba, size_t width, size_t height,
						 const struct tex_atlas_grid* grid, uint8_t* out);

#endif
