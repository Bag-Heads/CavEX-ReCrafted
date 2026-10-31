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

#ifndef WORLDGEN_BIOME_H
#define WORLDGEN_BIOME_H

#include <stdint.h>

// Minecraft Beta 1.7.3 biomes (BiomeGenBase). Values match Beta's biome ids.
// The decision tree only ever returns RAINFOREST..TUNDRA (1..11); NONE/HELL/SKY
// exist for completeness but are not produced by the overworld classifier.
enum biome {
	BIOME_NONE = 0,
	BIOME_RAINFOREST = 1,
	BIOME_SWAMPLAND = 2,
	BIOME_SEASONAL_FOREST = 3,
	BIOME_FOREST = 4,
	BIOME_SAVANNA = 5,
	BIOME_SHRUBLAND = 6,
	BIOME_TAIGA = 7,
	BIOME_DESERT = 8,
	BIOME_PLAINS = 9,
	BIOME_ICE_DESERT = 10,
	BIOME_TUNDRA = 11,
	BIOME_HELL = 12,
	BIOME_SKY = 13,
};

// Build Beta's 64x64 temperature/humidity -> biome lookup table. Idempotent and
// seed-independent (only the temp/humidity noise depends on the seed). Call once
// before classifying.
void biome_lut_init(void);

// Classify a column from its (transformed, clamped-to-[0,1]) temperature and
// humidity, reproducing Beta's getBiomeFromLookup: quantize each to a 0..63 grid
// then index the table built from the getBiome decision tree. Uses double
// precision to match vanilla Beta (the BetrockServer reference uses float; that
// is a deviation we intentionally do not copy).
enum biome biome_classify(double temperature, double humidity);

// Beta surface blocks per biome: desert / ice-desert -> sand top and filler,
// every other biome -> grass over dirt. Returns enum block_type values.
uint8_t biome_top_block(enum biome b);
uint8_t biome_filler_block(enum biome b);

#endif
