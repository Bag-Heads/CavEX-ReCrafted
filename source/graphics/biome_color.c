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

#include <stdlib.h>
#include <string.h>

#include "../platform/mem2.h"
#include "../platform/texture.h"
#include "biome_color.h"

// The colormaps are 256x256 RGBA8 (row-major), the same pixel layout Beta's
// grassBuffer/foliageBuffer use after getRGB(0,0,256,256,...). NULL until
// loaded; a missing colormap yields white so a partial pack still renders.
static uint8_t* grass_map = NULL;
static uint8_t* foliage_map = NULL;
static size_t grass_w = 0, grass_h = 0;
static size_t foliage_w = 0, foliage_h = 0;

// Load a colormap and park it in MEM2. Each map is ~256 KB of long-lived,
// GX-unrelated CPU lookup data the mesher reads per column -- exactly what the
// roomy MEM2 pool is for, instead of the tight MEM1 one tex_read's malloc lands
// in. mem2_alloc falls back to the normal heap, so if MEM2 is unavailable the
// map simply stays where tex_read put it. (On PC mem2_alloc is plain malloc.)
static uint8_t* load_colormap(const char* file, size_t* w, size_t* h) {
	uint8_t* tmp = tex_read(file, w, h);
	if(!tmp)
		return NULL;
	size_t bytes = (*w) * (*h) * 4;
	uint8_t* m2 = mem2_tag(mem2_alloc(bytes), bytes, CARVE_TEXTURE);
	if(!m2)
		return tmp;
	memcpy(m2, tmp, bytes);
	free(tmp);
	return m2;
}

void biome_color_init(void) {
	// Re-entrant: a texture-pack switch re-runs tex_init() -> biome_color_init(),
	// so drop the previous pack's colormaps and reload from the now-active pack
	// (otherwise the grass/foliage tint would keep the first pack's colours).
	if(grass_map) {
		mem2_free(grass_map);
		grass_map = NULL;
	}
	if(foliage_map) {
		mem2_free(foliage_map);
		foliage_map = NULL;
	}
	grass_map = load_colormap("misc/grasscolor.png", &grass_w, &grass_h);
	foliage_map = load_colormap("misc/foliagecolor.png", &foliage_w, &foliage_h);
}

void biome_color_export(const uint8_t** grass, size_t* gw, size_t* gh,
						const uint8_t** foliage, size_t* fw, size_t* fh) {
	*grass = grass_map;
	*gw = grass_map ? grass_w : 0;
	*gh = grass_map ? grass_h : 0;
	*foliage = foliage_map;
	*fw = foliage_map ? foliage_w : 0;
	*fh = foliage_map ? foliage_h : 0;
}

void biome_color_import(uint8_t* grass, size_t gw, size_t gh, uint8_t* foliage,
						size_t fw, size_t fh) {
	if(grass_map)
		mem2_free(grass_map);
	if(foliage_map)
		mem2_free(foliage_map);
	grass_map = grass;
	grass_w = gw;
	grass_h = gh;
	foliage_map = foliage;
	foliage_w = fw;
	foliage_h = fh;
}

static uint32_t sample(const uint8_t* map, size_t w, size_t h, uint8_t ci,
					   uint8_t cj) {
	if(!map || w == 0 || h == 0)
		return 0xFFFFFFu; // no colormap -> no tint

	// Beta indexes map[cj << 8 | ci], i.e. pixel (x = ci, y = cj). Clamp to the
	// map size in case a pack ships a non-256 colormap.
	size_t x = (size_t)ci < w ? (size_t)ci : w - 1;
	size_t y = (size_t)cj < h ? (size_t)cj : h - 1;
	const uint8_t* p = map + (y * w + x) * 4;
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

uint32_t biome_grass_color(uint8_t ci, uint8_t cj) {
	return sample(grass_map, grass_w, grass_h, ci, cj);
}

bool biome_grass_colormap_loaded(void) {
	return grass_map && grass_w && grass_h;
}

bool biome_foliage_colormap_loaded(void) {
	return foliage_map && foliage_w && foliage_h;
}

uint32_t biome_foliage_color(uint8_t ci, uint8_t cj) {
	return sample(foliage_map, foliage_w, foliage_h, ci, cj);
}
