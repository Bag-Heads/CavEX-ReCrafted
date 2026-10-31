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

#ifndef ENTITY_FALLING_SAND_H
#define ENTITY_FALLING_SAND_H

#include <stdbool.h>
#include <stdint.h>

struct entity;
struct server_local;

/* Beta 1.7.3 EntityFallingSand constants [740c583: EntityFallingSand]. */
#define FALLING_SAND_SIZE 0.98f
/* Gravity/drag: Beta's literal doubles (0.03999999910593033D /
 * 0.9800000190734863D) are exactly (double)0.04F / (double)0.98F -- the same
 * float-promoted-to-double pattern entity_primed_tnt.h already documents and
 * uses (PRIMED_TNT_GRAVITY/PRIMED_TNT_DRAG). */
#define FALLING_SAND_GRAVITY 0.04f
#define FALLING_SAND_DRAG 0.98f
/* On-ground landing drag/bounce -- numerically identical to
 * entity_primed_tnt.h's PRIMED_TNT_GROUND_DRAG/PRIMED_TNT_GROUND_BOUNCE
 * (Beta's Entity/EntityFallingSand/EntityTNTPrimed all use this same 0.7/-0.5
 * pair on landing), kept as its own named pair here rather than reused across
 * headers so each entity's constants stay self-contained and independently
 * citable. */
#define FALLING_SAND_GROUND_DRAG 0.7f
#define FALLING_SAND_GROUND_BOUNCE -0.5f

void entity_falling_sand(uint32_t id, struct entity* e, bool server,
						 void* world, uint8_t block_id);

#endif
