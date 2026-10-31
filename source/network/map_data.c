/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

// Beta MapData's defaults and ItemMap.updateMapData's pure scan math
// [740c583], faithfully transcribed. See MAPDATA_SLICE_NOTES.md for the
// full citation list and the explicit seams left for part 2 (ItemMap: id
// allocation, the colour-palette block-histogram scan, any client render).

#include <assert.h>
#include <string.h>

#include "map_data.h"
#include "../worldgen/jmath.h"

void map_data_set_defaults(struct map_data* md, int32_t x_center,
						   int32_t z_center, int8_t dimension) {
	// CODESTYLE 1.6's house-style compound precondition assert
	// (`assert(w && dimension >= -1 && dimension <= 0);`, server_world.c:202)
	// -- world_dim's only two valid values are WORLD_DIM_NETHER (-1) and
	// WORLD_DIM_OVERWORLD (0); a stray dimension byte should trip this on a
	// PC/debug build rather than silently pass through.
	assert(md && dimension >= -1 && dimension <= 0);
	// [740c583: ItemMap.getMapData / ItemMap.onCreated] Both Beta call sites
	// assign scale=3 and leave step at Java's untouched int default (0) the
	// same way; only xCenter/zCenter's SOURCE differs between them, which is
	// why this function takes them as parameters instead of deriving either
	// internally (see map_data.h's doc comment on the two call sites).
	md->x_center = x_center;
	md->z_center = z_center;
	md->dimension = dimension;
	md->scale = 3;
	md->step = 0;
	// [740c583: MapData] The newly allocated colour array contains zeros.
	// Java supplies those zeros as part of array allocation.
	memset(md->colors, 0, sizeof(md->colors));
}

int32_t map_data_scan_blocks_per_pixel(int8_t scale) {
	assert(scale >= 0 && scale <= 4);
	// [740c583: ItemMap.updateMapData] Pixel width is two to the scale.
	return 1 << scale;
}

int32_t map_data_scan_radius(int32_t blocks_per_pixel, bool has_no_sky) {
	assert(blocks_per_pixel >= 1);
	// [740c583: ItemMap.updateMapData] Divide the 128-block scan reach by
	// pixel width; halve that radius again in a world without sky.
	int32_t radius = 128 / blocks_per_pixel;
	if(has_no_sky)
		radius /= 2;
	return radius;
}

int32_t map_data_scan_pixel_center(double entity_pos, int32_t map_center,
								   int32_t blocks_per_pixel) {
	assert(blocks_per_pixel >= 1);
	// [740c583: ItemMap.updateMapData] Floor the entity offset first, then
	// divide the integer result by pixel width and add half the map width.
	// The initial operation is a true floor (java_floor_double, jmath.h).
	// The subsequent Java integer division truncates toward zero, as in C,
	// so negative offsets must not receive a second floor operation.
	int32_t floored = java_floor_double(entity_pos - (double)map_center);
	return floored / blocks_per_pixel + MAP_DATA_DIM / 2;
}

bool map_data_scan_stripe_selected(int32_t pixel_x,
								   int32_t step_after_increment) {
	// [740c583: ItemMap.updateMapData] Increment the scan step before
	// comparing its low four bits with those of the pixel column.
	// CODESTYLE 2.3 requires preserving this bit-mask idiom; it also
	// preserves Java selection behavior for negative columns and steps.
	return (pixel_x & 15) == (step_after_increment & 15);
}

bool map_data_scan_should_update(enum world_dim world_dimension,
								 int8_t map_dimension) {
	// CODESTYLE 1.6's house-style compound precondition assert
	// (`assert(w && dimension >= -1 && dimension <= 0);`, server_world.c:202)
	// applied to BOTH dimension values this function takes -- server_world.c
	// bounds-checks its own `enum world_dim` parameter the same way rather
	// than trusting the static type, since nothing stops a caller widening a
	// stray int into one.
	assert(world_dimension >= -1 && world_dimension <= 0
		   && map_dimension >= -1 && map_dimension <= 0);
	// [740c583: ItemMap.updateMapData] Scan only in the map's dimension.
	// Java sign-extends the saved dimension byte for the comparison.
	// Narrowing world_dimension here instead is lossless because the
	// only valid world_dim values fit in that byte: WORLD_DIM_NETHER
	// is -1 and WORLD_DIM_OVERWORLD is 0.
	return (int8_t)world_dimension == map_dimension;
}

bool map_data_scan_nether_column_is_stone(int32_t world_x, int32_t world_z) {
	// [740c583: ItemMap.updateMapData, hasNoSky branch] Derive a column
	// colour from the world coordinates using the fixed integer mix.
	// Bit 20 selects dirt when clear and stone when set. All arithmetic
	// wraps as Java int32 -- computed here entirely in uint32_t (CODESTYLE
	// 2.3: do overflow-relying int math in an unsigned type, cast back only
	// where the sign matters). The sign never matters for this bit test:
	// it reads bit 20 of the 32-bit pattern. Arithmetic sign extension
	// and logical zero filling during the right shift affect only the
	// vacated high bits (result bits 12-31), never the low result bit
	// tested here. The coordinate mix constants and operation order
	// remain unchanged, as does the low-bit test after shifting.
	// The same argument applies to server_weather.c's F6(c) comment.
	uint32_t v = (uint32_t)world_x + (uint32_t)world_z * 231871u;
	v = v * v * 31287121u + v * 11u;
	return ((v >> 20) & 1u) != 0;
}
