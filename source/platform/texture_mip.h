/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).
*/

#ifndef TEXTURE_MIP_H
#define TEXTURE_MIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pixel geometry produced by texture_atlas.c. The first cell begins after the
 * atlas' leading margin; cell_pitch includes the duplicated edge border. */
struct tex_atlas_grid {
	size_t content_pixels;
	size_t border_pixels;
	size_t cell_pitch;
	size_t origin_x;
	size_t origin_y;
};

bool tex_atlas_grid_init(struct tex_atlas_grid* grid, size_t width,
						 size_t height);

/* Box-filter one mip level from the original RGBA8 atlas. Samples are clamped
 * to the logical atlas cell containing each output pixel, so an unaligned cell
 * boundary cannot pull pixels from its neighbour. Caller owns the result. */
uint8_t* tex_mip_downsample_rgba8(const uint8_t* src, size_t src_w,
								  size_t src_h,
								  const struct tex_atlas_grid* grid,
								  unsigned lod);

#endif
