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

#ifndef AABB_H
#define AABB_H

#include <stdbool.h>

struct AABB {
	float x1, y1, z1;
	float x2, y2, z2;
};

struct ray {
	float x, y, z;
	float dx, dy, dz;
};

#include "blocks_data.h"

void aabb_setsize(struct AABB* a, float sx, float sy, float sz);
void aabb_setsize_centered(struct AABB* a, float sx, float sy, float sz);
void aabb_translate(struct AABB* a, float x, float y, float z);
bool aabb_intersection_ray(struct AABB* a, struct ray* r, enum side* s);
bool aabb_intersection(struct AABB* a, struct AABB* b);
// Beta AxisAlignedBB.intersectsWith: STRICT (>/<) -- two boxes sharing only a
// face/edge do NOT intersect. aabb_intersection above is INCLUSIVE (<=), so an
// entity box planted exactly on a floor "touches" it; use THIS one wherever
// Beta semantics distinguish touching from overlapping (spawn clearance, the
// embedded-box exclusion in entity_intersection_threshold).
bool aabb_intersection_strict(struct AABB* a, struct AABB* b);
bool aabb_intersection_point(struct AABB* a, float x, float y, float z);
void aabb_setsize_centered_offset(struct AABB* a,
                                  float sx, float sy, float sz,
                                  float ox, float oy, float oz);
/* Slab ray/AABB entry-distance test: does the ray (origin + t*dir, dir
 * ALREADY unit length) enter `box` at some t in [0, +inf)? Writes the entry
 * t on a hit. Distinct from aabb_intersection_ray above (side-only, no
 * distance -- the camera/selection raycast's primitive): several per-tick
 * entity sweeps need to RANK candidates by closest hit, which needs the
 * distance. Reuse this instead of adding a fourth duplicate -- egg's and
 * arrow's thrown-projectile sweeps (entity_egg.c, entity_arrow.c) each
 * already carry their own file-scope copy of this exact math (CODESTYLE.md
 * S3.2: extract on the SECOND transcription -- this makes a third). float[3]
 * (not cglm's vec3) so this header does not gain a cglm dependency -- a
 * vec3 argument is layout-compatible at any call site. */
bool aabb_ray_entry_distance(const float origin[3], const float dir[3],
							 const struct AABB* box, float* out_t);
/* Beta AxisAlignedBB.expand(d,d,d): grow every face outward by d. Arrow's,
 * egg's, and fish's entity-hit sweeps all widen the candidate box by this
 * exact idiom before the intercept test -- same extraction rationale as
 * aabb_ray_entry_distance above. */
void aabb_expand(struct AABB* a, float d);
#endif
