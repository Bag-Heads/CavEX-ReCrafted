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

#ifndef WORLDGEN_CAVE_H
#define WORLDGEN_CAVE_H

#include <stdint.h>

// Minecraft Beta 1.7.3 cave generator (MapGenCaves / Beta173Caver). For the
// chunk at (chunk_x, chunk_z), seed tunnels from the world seed across the
// surrounding 17x17 chunk region and carve the parts that fall inside this
// chunk into `ids` (block-id array, layout (x*16 + z)*128 + y == Beta
// x<<11|z<<7|y). Carvable blocks (stone/dirt/grass) become air, or flowing
// lava below y=10; bedrock is safe (carving is clamped to y in [1,120]). A
// segment that would intersect water is skipped. Self-contained: only this
// chunk's `ids` are read/written. Run AFTER surface replacement.
void cave_generate(int64_t seed, int32_t chunk_x, int32_t chunk_z, uint8_t* ids);

#endif
