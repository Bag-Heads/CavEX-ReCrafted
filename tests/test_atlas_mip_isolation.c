/* ci-1101/06: test the sampled UV footprint, not just each filter box.
 * texture_atlas.c:tex_atlas_tile_uv spans the full content rectangle.
 * Beta RenderEngine.java:20,164-170 defaults to unmipped GL_NEAREST;
 * this checks the explicitly enabled mip deviation. */
#include "graphics/render_block.h"
#include "graphics/texture_atlas.h"
#include "platform/tex_gx_conv.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only tex_atlas_tile_uv is used; no image decoder is involved. */
uint8_t* tex_read_atlas(const char* name, size_t* w, size_t* h) {
	(void)name;
	(void)w;
	(void)h;
	assert(!"unexpected decoder call");
	return NULL;
}

static unsigned failures;
static size_t checked;
static void check_size(size_t side) {
	struct tex_atlas_grid grid;
	assert(tex_atlas_grid_init(&grid, side, side));
	uint8_t* src = malloc(side * side * 4);
	assert(src);
	memset(src, 255, side * side * 4);
	for(size_t ty = 0; ty < 14; ty++)
		for(size_t tx = 0; tx < 14; tx++) {
			size_t x0 = grid.origin_x + tx * grid.cell_pitch;
			size_t y0 = grid.origin_y + ty * grid.cell_pitch;
			for(size_t y = y0; y < y0 + grid.cell_pitch; y++)
				for(size_t x = x0; x < x0 + grid.cell_pitch; x++) {
					assert(x < side && y < side);
					uint8_t* p = src + (y * side + x) * 4;
					p[0] = (uint8_t)tx;
					p[1] = (uint8_t)ty;
					p[2] = (uint8_t)(ty * 14 + tx);
					p[3] = 255;
				}
		}
	unsigned levels = tex_atlas_mip_levels(&grid);
	assert(levels == (side % 256 == 0 && !(side & (side - 1)) ? TEX_GX_ATLAS_LODS : 1));
	if(side == 768)
		assert(levels == 1);
	for(unsigned lod = 0; lod < levels; lod++) {
		size_t factor = (size_t)1 << lod, width = side >> lod;
		uint8_t* mip
			= lod ? tex_mip_downsample_rgba8(src, side, side, &grid, lod) : src;
		assert(mip);
		for(size_t ty = 0; ty < 14; ty++)
			for(size_t tx = 0; tx < 14; tx++) {
				double x0 = (double)(grid.origin_x + grid.border_pixels
									 + tx * grid.cell_pitch)
					/ factor;
				double y0 = (double)(grid.origin_y + grid.border_pixels
									 + ty * grid.cell_pitch)
					/ factor;
				double span = (double)grid.content_pixels / factor;
				double x1 = x0 + span, y1 = y0 + span;
				if(levels > 1) {
					/* Pin both real UV conventions: packed particles and
					 * TEX_OFFSET terrain vertices normalized as GX_U8/256. */
					float uv[4];
					assert(tex_atlas_tile_uv(side, side,
											 (uint8_t)(ty * 14 + tx), uv));
					double bx0 = (double)TEX_OFFSET(tx) * side / (256 * factor);
					double by0 = (double)TEX_OFFSET(ty) * side / (256 * factor);
					assert(bx0 == x0 && by0 == y0);
					assert((double)16 * side / (256 * factor) == span);
					/* Union of both rectangles conservatively includes every
					 * tap. */
					x0 = fmin(x0, (double)uv[0] * width);
					y0 = fmin(y0, (double)uv[1] * width);
					x1 = fmax(x1, (double)uv[2] * width);
					y1 = fmax(y1, (double)uv[3] * width);
				}
				/* All texels with nonzero bilinear weight anywhere in the
				 * closed UV rectangle (also covers between-LOD blending). */
				int xmin = (int)floor(x0 - 0.5), xmax = (int)ceil(x1 - 0.5);
				int ymin = (int)floor(y0 - 0.5), ymax = (int)ceil(y1 - 0.5);
				if(levels == 1) {
					/* Nearest fallback; raster samples lie inside the UV rect.
					 */
					xmin = (int)floor(x0);
					xmax = (int)ceil(x0 + span) - 1;
					ymin = (int)floor(y0);
					ymax = (int)ceil(y0 + span) - 1;
				}
				/* GX clamps texture-edge taps; interior tile contamination
				 * must still be rejected. */
				if(xmin < 0)
					xmin = 0;
				if(ymin < 0)
					ymin = 0;
				if(xmax >= (int)width)
					xmax = (int)width - 1;
				if(ymax >= (int)width)
					ymax = (int)width - 1;
				for(int y = ymin; y <= ymax; y++)
					for(int x = xmin; x <= xmax; x++) {
						assert(x >= 0 && y >= 0 && (size_t)x < width
							   && (size_t)y < width);
						const uint8_t* p
							= mip + ((size_t)y * width + (size_t)x) * 4;
						checked++;
						if(p[0] != tx || p[1] != ty || p[2] != ty * 14 + tx
						   || p[3] != 255) {
							if(failures < 8)
								printf("FAIL side=%zu tile=%zu,%zu lod=%u "
									   "texel=%d,%d owner=%u,%u\n",
									   side, tx, ty, lod, x, y, p[0], p[1]);
							failures++;
						}
					}
			}
		if(lod)
			free(mip);
	}
	free(src);
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	for(size_t side = 16; side <= 1024; side += 16)
		check_size(side);
	printf("atlas mip isolation: %zu texels checked, %u failures\n", checked,
		   failures);
	return failures ? 1 : 0;
}
