/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).
*/

#include "texture_mip.h"

#include <limits.h>
#include <stdlib.h>

bool tex_atlas_grid_init(struct tex_atlas_grid* grid, size_t width,
						 size_t height) {
	if(!grid || width != height || width < 16 || (width % 16) != 0)
		return false;

	grid->content_pixels = width / 16;
	grid->border_pixels = width / 256;
	grid->cell_pitch = grid->content_pixels + 2 * grid->border_pixels;
	/* texture_atlas.c places the first content pixel at 3 * border and
	 * duplicates one border-width on either side of the content. */
	grid->origin_x = 2 * grid->border_pixels;
	grid->origin_y = 2 * grid->border_pixels;
	return grid->cell_pitch > 0 && grid->origin_x < width
		   && grid->origin_y < height;
}

static bool axis_cell_bounds(size_t coord, size_t extent, size_t origin,
							 size_t pitch, size_t* first, size_t* end) {
	if(!extent || !pitch || origin >= extent || coord >= extent || !first || !end)
		return false;

	if(coord < origin) {
		*first = 0;
		*end = origin;
	} else {
		size_t cell = (coord - origin) / pitch;
		if(cell > (SIZE_MAX - origin) / pitch)
			return false;
		*first = origin + cell * pitch;
		*end = *first > SIZE_MAX - pitch ? extent : *first + pitch;
		if(*end > extent)
			*end = extent;
	}
	return *first < *end;
}

uint8_t* tex_mip_downsample_rgba8(const uint8_t* src, size_t src_w,
								  size_t src_h,
								  const struct tex_atlas_grid* grid,
								  unsigned lod) {
	if(!src || !grid || !grid->cell_pitch || lod == 0 || lod > 15
	   || lod >= sizeof(size_t) * CHAR_BIT || !src_w || !src_h
	   || src_h > SIZE_MAX / src_w || src_w * src_h > SIZE_MAX / 4)
		return NULL;

	size_t factor = (size_t)1 << lod;
	size_t out_w = src_w >> lod;
	size_t out_h = src_h >> lod;
	if(!out_w || !out_h || out_h > SIZE_MAX / out_w
	   || out_w * out_h > SIZE_MAX / 4)
		return NULL;

	uint8_t* out = malloc(out_w * out_h * 4);
	if(!out)
		return NULL;

	for(size_t oy = 0; oy < out_h; oy++) {
		for(size_t ox = 0; ox < out_w; ox++) {
			size_t base_x = ox << lod;
			size_t base_y = oy << lod;
			size_t tile_x0, tile_x1, tile_y0, tile_y1;
			if(!axis_cell_bounds(base_x, src_w, grid->origin_x,
								 grid->cell_pitch, &tile_x0, &tile_x1)
			   || !axis_cell_bounds(base_y, src_h, grid->origin_y,
									grid->cell_pitch, &tile_y0, &tile_y1)) {
				free(out);
				return NULL;
			}

			uint64_t r = 0, g = 0, b = 0, a = 0;
			for(size_t dy = 0; dy < factor; dy++) {
				for(size_t dx = 0; dx < factor; dx++) {
					size_t sx = base_x + dx;
					size_t sy = base_y + dy;
					if(sx >= tile_x1)
						sx = tile_x1 - 1;
					if(sy >= tile_y1)
						sy = tile_y1 - 1;
					if(sx < tile_x0)
						sx = tile_x0;
					if(sy < tile_y0)
						sy = tile_y0;
					const uint8_t* p = src + (sy * src_w + sx) * 4;
					r += p[0];
					g += p[1];
					b += p[2];
					a += p[3];
				}
			}
			uint64_t count = (uint64_t)factor * factor;
			uint8_t* pixel = out + (oy * out_w + ox) * 4;
			pixel[0] = (uint8_t)(r / count);
			pixel[1] = (uint8_t)(g / count);
			pixel[2] = (uint8_t)(b / count);
			pixel[3] = (uint8_t)(a / count);
		}
	}
	return out;
}
