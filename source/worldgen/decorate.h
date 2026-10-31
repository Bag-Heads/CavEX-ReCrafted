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

#ifndef WORLDGEN_DECORATE_H
#define WORLDGEN_DECORATE_H

#include "biome.h"

struct wgen_world;
struct java_random;

/*
	Beta 1.7.3 small decorations -- the populate steps that run AFTER the trees,
	sharing the per-chunk decorate Random: dandelions, tall grass/fern, dead
	bush, roses, brown/red mushrooms, sugar cane, pumpkins, cacti, and the one-
	block water/lava springs, in Beta's exact order.

	The draw order + count is load-bearing (every later step's positions depend
	on it). `corner_biome` gates the per-biome counts (Beta samples it once at the
	chunk's +16,+16 corner). The snow pass is NOT here: it draws no randomness and
	needs the per-column temperature, so it is a separate (desync-safe) step.

	block_x/block_z are the chunk's block origin (chunk_x*16, chunk_z*16).
*/
void worldgen_decorate(struct wgen_world* w, struct java_random* rand,
					   enum biome corner_biome, int block_x, int block_z);

/*
	Beta 1.7.3 snow pass -- places one snow layer on each cold-enough exposed
	column (temp below 0.5 after the altitude lapse, over a solid non-ice block).
	Draws NO randomness, so it runs after the decorations with the per-column
	temperature the caller sampled from the generator (worldgen_temperature_grid,
	the +8,+8 16x16 field as `temp[localX*16 + localZ]`). block_x/block_z are the
	chunk's block origin.
*/
void worldgen_snow(struct wgen_world* w, const double* temp, int block_x,
				   int block_z);

#endif
