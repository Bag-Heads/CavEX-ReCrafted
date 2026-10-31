/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#ifndef CHUNK_DEFS_H
#define CHUNK_DEFS_H

#include <stdint.h>

// Chunk-dimension constants and coordinate macros, split out of chunk.h
// (step 7 interface split) so value-only consumers -- world.h needs
// CHUNK_SIZE for COLUMN_HEIGHT and its heightmap array dims -- don't pull
// the whole chunk API and re-create the world<->chunk include cycle the
// header-hygiene gate now forbids. Definitions are verbatim from chunk.h.

#define CHUNK_SIZE_BITS 0xF
#define CHUNK_SIZE 16

// World-dimension constants (moved from world.h in the same split: chunk.h
// and world.h both need them, and neither may include the other's API).
#define WORLD_HEIGHT 128
#define COLUMN_HEIGHT ((WORLD_HEIGHT + CHUNK_SIZE - 1) / CHUNK_SIZE)

#define WCOORD_CHUNK_OFFSET(x)                                                 \
	((x) < 0 ? (((x) + 1) / CHUNK_SIZE - 1) : (x) / CHUNK_SIZE)
// Shift in UNSIGNED then cast (same UB-avoidance as SECTION_TO_ID/S_CHUNK_ID): masking a
// negative coord yields a large positive whose << 34 overflows int64 -> signed-shift UB.
// The masks keep the byte-pattern identical to the old key for any coord.
#define CHUNK_TO_ID(x, y, z)                                                   \
	((int64_t)((((uint64_t)((z) & 0x3FFFFFFF)) << 34)                          \
			   | (((uint64_t)((x) & 0x3FFFFFFF)) << 4)                         \
			   | ((uint64_t)((y) & 0xF))))
#define W2C_COORD(x) ((x)&CHUNK_SIZE_BITS)

#endif
