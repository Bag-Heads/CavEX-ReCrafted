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

#ifndef BIOME_COLOR_H
#define BIOME_COLOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Beta 1.7.3 biome grass/foliage colormaps (misc/grasscolor.png and
// misc/foliagecolor.png, 256x256), loaded as CPU lookup tables. The server
// ships two colormap indices per column (ci, cj); the mesher turns them into a
// per-column tint with biome_grass_color/biome_foliage_color, reproducing Beta
// ColorizerGrass/ColorizerFoliage: colour = map[cj << 8 | ci]. Returns 0xRRGGBB.
// If a pack omits the colormap, lookups return white (0xFFFFFF = no tint).
void biome_color_init(void);
/* pack-cache-0927: the loaded colormaps (RGBA8, NULL/0 when absent), for
   the pack cache. import takes ownership of buffers allocated with
   mem2_alloc (the placement load_colormap uses) and frees the current ones. */
void biome_color_export(const uint8_t** grass, size_t* gw, size_t* gh,
						const uint8_t** foliage, size_t* fw, size_t* fh);
void biome_color_import(uint8_t* grass, size_t gw, size_t gh, uint8_t* foliage,
						size_t fw, size_t fh);

uint32_t biome_grass_color(uint8_t ci, uint8_t cj);
// True when a grass colormap is loaded (biome_grass_color then returns the
// real map entry rather than the white "no map" value).
bool biome_grass_colormap_loaded(void);

// DC-GRASS G2: CaveWii UNAVAILABLE-DATA policy, not Beta (Beta always has a
// biome colour, BlockGrass.java:23-28). When a grass column has no section
// tint OR the pack has no grass colormap, the grass block is tinted ONCE with
// this deterministic colour -- the static colour the pre-G2 atlas baked into
// grass, so the missing-data look is unchanged. Grass block only; foliage /
// tall grass keep their existing handling.
#define BIOME_GRASS_FALLBACK 0x6EC63Fu /* (110,198,63) */
uint32_t biome_foliage_color(uint8_t ci, uint8_t cj);
// True when a foliage colormap is loaded (biome_foliage_color then returns
// the real map entry rather than white).
bool biome_foliage_colormap_loaded(void);

// Fixed Beta foliage colours for spruce (leaf metadata bit 0) and birch
// (bit 1); these ignore biome and position.
#define BIOME_FOLIAGE_PINE 0x619961u
#define BIOME_FOLIAGE_BIRCH 0x80A755u

#endif
