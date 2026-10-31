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

// Beta 1.7.3 map item, part 2b (held-map render), display-conversion half
// ONLY: MapItemRenderer.renderMap's per-pixel colour-byte -> RGBA8888 fill
// loop, transcribed 1:1. See item_map.h for the module split (part 2a is the
// simulation/colour-scan core in item_map.c; this file owns the pure-math
// half of part 2b -- the GL texture upload, quad draw, player-icon draw, and
// map-name draw the rest of renderMap performs are NOT display-conversion
// and stay deferred, see docs/deviations.md).
//
// [decompile 740c583 MapItemRenderer.renderMap]:
//
// The renderer converts all 16384 map colour bytes. Dividing a colour byte
// by four selects its palette index; the low two bits select brightness.
// Palette index zero is unexplored: RGB stays black while alpha alternates
// between 16 and 24 in a checkerboard across the 128-by-128 map.
// Other indices select MapColor RGB, with each channel multiplied by the
// brightness and divided by 255 using integer arithmetic. Brightness is
// 220 for bands 1 and 3, 255 for band 2 and 180 for band 0. Explored pixels
// are fully opaque. The port omits the optional anaglyph channel mix.
//
//
//
//
//
//
//
//
//
//
//
// The renderer stores packed ARGB (alpha in the top byte); this port
// emits the same four channels as separate RGBA8888 bytes instead, so every
// value below is the exact byte MapItemRenderer would have packed into that
// int's corresponding 8-bit lane. The brightness multiplier defaults to 220,
// retained for bands 1 and 3. Two independent checks select 255 for band 2
// and 180 for band 0. Verified against the
// live decompile (not assumed) via `beta.sh MapItemRenderer` before writing
// this file.
//
// Anaglyph 3D further recolours the three RGB channels
// via a red/cyan channel mix; CaveWii has no anaglyph 3D mode, so
// that branch is dead code for this port and is not transcribed.

#include <stdint.h>

#include "item_map.h"
#include "../../network/map_data.h" // MAP_DATA_DIM / MAP_DATA_COLORS_SIZE (128 / 16384) -- single source of truth for the map's fixed geometry, matching struct map_data's own colors[] sizing.

void item_map_colors_to_rgba(const uint8_t* colors, uint8_t* out_rgba) {
	for(int32_t i = 0; i < MAP_DATA_COLORS_SIZE; i++) {
		uint8_t c = colors ? colors[i] : 0; // [740c583] Read the current map colour byte.
		uint8_t* px = &out_rgba[i * 4];

		// [740c583] Palette index zero is unexplored. Every colour byte item_map.c's
		// item_map_update_data ever actually WRITES is `color_index * 4 +
		// shade_band` with color_index in 0..13 and shade_band in 0..2 (max
		// 54), always < 128 -- so the Java-signed-vs-C-unsigned byte
		// distinction never bites for real map data; c/4==0 identically
		// means color_index==0 (MC_AIR, "nothing painted here yet") in both
		// languages over that range.
		if(c / 4 == 0) {
			// [740c583] Alternating row/column parity supplies alpha for the
			// UNEXPLORED checkerboard. RGB stays 0,0,0; only alpha (the top
			// byte of the packed ARGB int) carries the dither, alternating
			// 16/24 in a checkerboard pattern across the 128x128 grid.
			uint8_t alpha = (uint8_t)(((i + i / 128) & 1) * 8 + 16);
			px[0] = 0;
			px[1] = 0;
			px[2] = 0;
			px[3] = alpha;
		} else {
			// [740c583] Look up the palette RGB for the colour index;
			// item_map_color_rgb_for_index is this port's exact transcription
			// of that same 14-entry table (item_map.c:308, declared above in
			// item_map.h), so this calls the real production palette rather
			// than a duplicated copy.
			uint32_t base = item_map_color_rgb_for_index((uint8_t)(c / 4));
			uint8_t band = c & 3; // [740c583] Low two bits select the brightness band.

			// [740c583] Brightness defaults to 220; band 2 selects 255 and
			// band 0 selects 180. Two independent checks occur in
			// the decompile, mirrored here as-is; bands 1 and 3 both fall
			// through untouched to the 220 default.
			uint16_t mul = 220;
			if(band == 2)
				mul = 255;
			if(band == 0)
				mul = 180;

			// [740c583] Scale each RGB channel by brightness over 255. All
			// operands are non-negative and well within int32_t/uint32_t, so
			// plain C multiply/divide is bit-identical to Java's int math.
			px[0] = (uint8_t)(((base >> 16) & 0xFF) * mul / 255);
			px[1] = (uint8_t)(((base >> 8) & 0xFF) * mul / 255);
			px[2] = (uint8_t)((base & 0xFF) * mul / 255);
			// [740c583] Packed ARGB uses alpha 255 for an explored
			// pixel, making it fully opaque.
			px[3] = 255;
		}
	}
}
