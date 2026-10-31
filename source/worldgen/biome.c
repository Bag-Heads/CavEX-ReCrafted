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

/*
	Beta 1.7.3 biome classifier (BiomeGenBase / WorldChunkManager), ported to
	match vanilla. A column's biome is a pure function of its temperature and
	humidity: Beta builds a 64x64 lookup table once from a decision tree, then
	classifies each column by quantizing temp/humidity to that grid. Only the
	overworld land biomes are produced here; the per-biome surface block choice
	(desert -> sand, else grass/dirt) feeds replaceBlocksForBiome.
*/

#include <stdbool.h>

#include "../block/blocks_data.h"
#include "biome.h"

// The decision tree (Beta BiomeGenBase.getBiome). NOTE: humidity is multiplied
// by temperature up front, so every humidity threshold below is really against
// rainfall*temperature. Double precision matches vanilla Beta.
static enum biome biome_get(double temperature, double humidity) {
	humidity *= temperature;

	if(temperature < 0.1)
		return BIOME_TUNDRA;

	if(humidity < 0.2) {
		if(temperature < 0.5)
			return BIOME_TUNDRA;
		if(temperature < 0.95)
			return BIOME_SAVANNA;
		return BIOME_DESERT;
	}

	if(humidity > 0.5 && temperature < 0.7)
		return BIOME_SWAMPLAND;

	if(temperature < 0.5)
		return BIOME_TAIGA;

	if(temperature < 0.97) {
		if(humidity < 0.35)
			return BIOME_SHRUBLAND;
		return BIOME_FOREST;
	}

	if(humidity < 0.45)
		return BIOME_PLAINS;
	if(humidity < 0.9)
		return BIOME_SEASONAL_FOREST;
	return BIOME_RAINFOREST;
}

static uint8_t biome_lut[64 * 64];
static bool biome_lut_ready = false;

void biome_lut_init(void) {
	if(biome_lut_ready)
		return;
	for(int temp = 0; temp < 64; temp++)
		for(int humi = 0; humi < 64; humi++)
			biome_lut[temp + humi * 64]
				= (uint8_t)biome_get((double)temp / 63.0, (double)humi / 63.0);
	biome_lut_ready = true;
}

enum biome biome_classify(double temperature, double humidity) {
	if(!biome_lut_ready)
		biome_lut_init();

	int temp = (int)(temperature * 63.0);
	int humi = (int)(humidity * 63.0);

	// Inputs are clamped to [0,1] upstream, so this index is in range; clamp
	// defensively so a stray out-of-range value can never read past the table.
	if(temp < 0)
		temp = 0;
	if(temp > 63)
		temp = 63;
	if(humi < 0)
		humi = 0;
	if(humi > 63)
		humi = 63;

	return (enum biome)biome_lut[temp + humi * 64];
}

uint8_t biome_top_block(enum biome b) {
	if(b == BIOME_DESERT || b == BIOME_ICE_DESERT)
		return BLOCK_SAND;
	return BLOCK_GRASS;
}

uint8_t biome_filler_block(enum biome b) {
	if(b == BIOME_DESERT || b == BIOME_ICE_DESERT)
		return BLOCK_SAND;
	return BLOCK_DIRT;
}
