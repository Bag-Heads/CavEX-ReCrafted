#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform/texture_mip.h"

static void assert_rgb(const uint8_t* image, size_t width, size_t x, size_t y,
					   uint8_t r, uint8_t g, uint8_t b) {
	const uint8_t* p = image + (y * width + x) * 4;
	assert(p[0] == r);
	assert(p[1] == g);
	assert(p[2] == b);
	assert(p[3] == 255);
}

static void test_standard_grid_geometry(void) {
	struct tex_atlas_grid grid;
	assert(tex_atlas_grid_init(&grid, 256, 256));
	assert(grid.content_pixels == 16);
	assert(grid.border_pixels == 1);
	assert(grid.cell_pitch == 18);
	assert(grid.origin_x == 2 && grid.origin_y == 2);

	assert(tex_atlas_grid_init(&grid, 512, 512));
	assert(grid.content_pixels == 32);
	assert(grid.border_pixels == 2);
	assert(grid.cell_pitch == 36);
	assert(grid.origin_x == 4 && grid.origin_y == 4);

	assert(tex_atlas_grid_init(&grid, 128, 128));
	assert(grid.content_pixels == 8);
	assert(grid.border_pixels == 0);
	assert(grid.cell_pitch == 8);
	assert(grid.origin_x == 0 && grid.origin_y == 0);

	assert(!tex_atlas_grid_init(&grid, 256, 128));
	assert(!tex_atlas_grid_init(&grid, 250, 250));
}

static void test_unaligned_cell_boundary_never_bleeds(void) {
	enum { SIDE = 64 };
	uint8_t src[SIDE * SIDE * 4];
	memset(src, 255, sizeof(src));

	/* Standard 256px-atlas geometry scaled into a small fixture: origin 2,
	 * pitch 18. The boundary at x=38 is not aligned to a LOD2 4-pixel box. */
	struct tex_atlas_grid grid = {
		.content_pixels = 16,
		.border_pixels = 1,
		.cell_pitch = 18,
		.origin_x = 2,
		.origin_y = 2,
	};
	const uint8_t colors[3][3] = {{220, 10, 20}, {10, 40, 230}, {20, 210, 30}};
	for(size_t y = 2; y < 56; y++) {
		for(size_t cell = 0; cell < 3; cell++) {
			for(size_t x = 2 + cell * 18; x < 2 + (cell + 1) * 18; x++) {
				uint8_t* p = src + (y * SIDE + x) * 4;
				p[0] = colors[cell][0];
				p[1] = colors[cell][1];
				p[2] = colors[cell][2];
				p[3] = 255;
			}
		}
	}

	uint8_t* lod1 = tex_mip_downsample_rgba8(src, SIDE, SIDE, &grid, 1);
	uint8_t* lod2 = tex_mip_downsample_rgba8(src, SIDE, SIDE, &grid, 2);
	uint8_t* lod3 = tex_mip_downsample_rgba8(src, SIDE, SIDE, &grid, 3);
	assert(lod1 && lod2 && lod3);
	assert_rgb(lod1, SIDE >> 1, 18, 2, 10, 40, 230);
	/* Source box x=36..39 straddles the real x=38 boundary. It belongs to
	 * cell 1, so x=38..39 must clamp to blue rather than sample green. */
	assert_rgb(lod2, SIDE >> 2, 9, 1, 10, 40, 230);
	assert_rgb(lod3, SIDE >> 3, 4, 1, 10, 40, 230);
	free(lod1);
	free(lod2);
	free(lod3);
}

int main(void) {
	test_standard_grid_geometry();
	test_unaligned_cell_boundary_never_bleeds();
	return 0;
}
