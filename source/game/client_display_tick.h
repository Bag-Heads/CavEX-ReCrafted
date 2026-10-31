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

#ifndef GAME_CLIENT_DISPLAY_TICK_H
#define GAME_CLIENT_DISPLAY_TICK_H

#include <stdint.h>

#include "../cglm/cglm.h"
#include "../world.h"

// Beta 1.7.3 client "random display tick" (Block.randomDisplayTick): purely
// cosmetic per-block effects sampled around the camera each client tick, on
// the CLIENT thread. Today: torch flame/smoke + redstone torch sparks.
// Replaces the old server-side torch onWorldTick, which raced the render
// thread on the particle array and the client RNG.
void client_display_tick(void);

// Where a torch's flame particle spawns, given the torch's placement
// metadata. Offsets are Beta 1.7.3 randomDisplayTick's constants (base
// x+0.5/y+0.7/z+0.5; wall torches shift 0.27 along the facing axis and
// +0.22 in y); the per-meta direction signs follow THIS engine's torch
// metadata convention (1:+x, 2:-x, 3:+z, 4:-z -- same mapping the old
// bbox-derived code used, so flames stay on the leaning side). Pure
// function, pinned by tests/test_torch_display.c.
void torch_display_pos(uint8_t meta, w_coord_t x, w_coord_t y, w_coord_t z,
					   vec3 out);

#endif
