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

#include <assert.h>
#include <math.h>

#include "aabb.h"

void aabb_setsize(struct AABB* a, float sx, float sy, float sz) {
	assert(a);

	a->x1 = 0.5F - sx / 2.0F;
	a->y1 = 0;
	a->z1 = 0.5F - sz / 2.0F;

	a->x2 = 0.5F + sx / 2.0F;
	a->y2 = sy;
	a->z2 = 0.5F + sz / 2.0F;
}

// Sets an AABB centered at the origin with size (sx, sy, sz),
// then shifts it by (ox, oy, oz) before any world‐position translation.
void aabb_setsize_centered_offset(struct AABB* a,
                                  float sx, float sy, float sz,
                                  float ox, float oy, float oz) {
    assert(a);

    // half‐extents
    float hx = sx * 0.5f;
    float hy = sy * 0.5f;
    float hz = sz * 0.5f;

    // Centered around origin, then apply offset:
    a->x1 = -hx + ox;
    a->x2 =  hx + ox;
    a->y1 = -hy + oy;
    a->y2 =  hy + oy;
    a->z1 = -hz + oz;
    a->z2 =  hz + oz;
}


void aabb_setsize_centered(struct AABB* a, float sx, float sy, float sz) {
	assert(a);

	a->x2 = sx / 2.0F;
	a->y2 = sy / 2.0F;
	a->z2 = sz / 2.0F;

	a->x1 = -a->x2;
	a->y1 = -a->y2;
	a->z1 = -a->z2;
}

void aabb_translate(struct AABB* a, float x, float y, float z) {
	assert(a);

	a->x1 += x;
	a->y1 += y;
	a->z1 += z;

	a->x2 += x;
	a->y2 += y;
	a->z2 += z;
}

// see: https://tavianator.com/2011/ray_box.html
bool aabb_intersection_ray(struct AABB* a, struct ray* r, enum side* s) {
	assert(a && r);

	float inv_x = 1.0F / r->dx;
	float tx1 = (a->x1 - r->x) * inv_x;
	float tx2 = (a->x2 - r->x) * inv_x;

	float tmin = fminf(tx1, tx2);
	float tmax = fmaxf(tx1, tx2);

	float inv_y = 1.0F / r->dy;
	float ty1 = (a->y1 - r->y) * inv_y;
	float ty2 = (a->y2 - r->y) * inv_y;

	tmin = fmaxf(tmin, fminf(fminf(ty1, ty2), tmax));
	tmax = fminf(tmax, fmaxf(fmaxf(ty1, ty2), tmin));

	float inv_z = 1.0F / r->dz;
	float tz1 = (a->z1 - r->z) * inv_z;
	float tz2 = (a->z2 - r->z) * inv_z;

	tmin = fmaxf(tmin, fminf(fminf(tz1, tz2), tmax));
	tmax = fminf(tmax, fmaxf(fmaxf(tz1, tz2), tmin));

	// is fine since fmaxf and fminf return the same value as one of the inputs
	if(s) {
		if(tmin == tx1)
			*s = SIDE_LEFT;
		else if(tmin == tx2)
			*s = SIDE_RIGHT;
		else if(tmin == ty1)
			*s = SIDE_BOTTOM;
		else if(tmin == ty2)
			*s = SIDE_TOP;
		else if(tmin == tz1)
			*s = SIDE_FRONT;
		else if(tmin == tz2)
			*s = SIDE_BACK;
		else {
			// Unreachable for every real block AABB: tmin is always one of
			// the six slab boundaries above. C99 fminf/fmaxf filter a LONE
			// NaN (0*inf on one boundary of an axis), so reaching this
			// branch needs NaN on BOTH boundaries of an axis -- a box with
			// zero extent there (tests/test_block_pick_axis.c pins the
			// interior axis-aligned picks that ruled out an epsilon guard;
			// entity_shadow no longer admits zero-width boxes). Write a
			// valid side so a broken invariant degrades to a drawable face
			// in release builds; assert so debug builds catch it.
			*s = SIDE_BOTTOM;
			assert(0 && "aabb_intersection_ray: degenerate box or NaN slab");
		}
	}

	return tmax > fmax(tmin, 0.0F);
}

bool aabb_intersection(struct AABB* a, struct AABB* b) {
	return (a->x1 <= b->x2 && b->x1 <= a->x2)
		&& (a->y1 <= b->y2 && b->y1 <= a->y2)
		&& (a->z1 <= b->z2 && b->z1 <= a->z2);
}

/* Beta AxisAlignedBB.intersectsWith, verbatim strictness [740c583:
 * AxisAlignedBB.java:185: `var1.maxX > this.minX && var1.minX < this.maxX`
 * per axis]: sharing only a face/edge is NOT an intersection. This is the
 * primitive behind Beta's getCanSpawnHere clearance and the calculateOffset
 * overlap pre-conditions; keep it distinct from the inclusive test above,
 * which existing collision callers depend on. */
bool aabb_intersection_strict(struct AABB* a, struct AABB* b) {
	return (a->x1 < b->x2 && b->x1 < a->x2) && (a->y1 < b->y2 && b->y1 < a->y2)
		&& (a->z1 < b->z2 && b->z1 < a->z2);
}

bool aabb_intersection_point(struct AABB* a, float x, float y, float z) {
	return (x >= a->x1 && x <= a->x2) && (y >= a->y1 && y <= a->y2)
		&& (z >= a->z1 && z <= a->z2);
}

// Extracted from entity_arrow.c's / entity_egg.c's identical file-scope slab
// test (CODESTYLE.md S3.2: extract on the second transcription -- this is
// the third caller, entity_fish.c). Hardcoded as a bare literal (not
// GLM_FLT_EPSILON) so this file does not need a cglm include for one
// constant -- but the VALUE must still match it exactly: GLM_FLT_EPSILON
// resolves to 1e-5f in this build (cglm/common.h -- CGLM_USE_DEFAULT_EPSILON,
// which would swap it to FLT_EPSILON instead, is never defined anywhere in
// the project; confirmed by a tree-wide grep). A prior 1e-8f here was 1000x
// SMALLER than the callers it claimed to mirror (adversarial-review finding
// 6/9): any direction component landing in (1e-8f, 1e-5f] -- plausible near
// axis-aligned throws, or at a fish's cast-arc apex where motionY crosses
// zero -- divided by the tiny raw value instead of the wider floor arrow/egg
// use, risking a different inv_d/tmin/tmax (and higher NaN/Inf exposure)
// than the "identical" logic this was extracted from.
#define AABB_RAY_ENTRY_EPS 1e-5f
bool aabb_ray_entry_distance(const float origin[3], const float dir[3],
							 const struct AABB* box, float* out_t) {
	assert(origin && dir && box);

	float inv_dx = 1.0f
		/ (fabsf(dir[0]) > AABB_RAY_ENTRY_EPS ? dir[0]
											  : copysignf(AABB_RAY_ENTRY_EPS, dir[0]));
	float inv_dy = 1.0f
		/ (fabsf(dir[1]) > AABB_RAY_ENTRY_EPS ? dir[1]
											  : copysignf(AABB_RAY_ENTRY_EPS, dir[1]));
	float inv_dz = 1.0f
		/ (fabsf(dir[2]) > AABB_RAY_ENTRY_EPS ? dir[2]
											  : copysignf(AABB_RAY_ENTRY_EPS, dir[2]));

	float t1 = (box->x1 - origin[0]) * inv_dx;
	float t2 = (box->x2 - origin[0]) * inv_dx;
	float t3 = (box->y1 - origin[1]) * inv_dy;
	float t4 = (box->y2 - origin[1]) * inv_dy;
	float t5 = (box->z1 - origin[2]) * inv_dz;
	float t6 = (box->z2 - origin[2]) * inv_dz;

	float tmin = fmaxf(fmaxf(fminf(t1, t2), fminf(t3, t4)), fminf(t5, t6));
	float tmax = fminf(fminf(fmaxf(t1, t2), fmaxf(t3, t4)), fmaxf(t5, t6));

	if(tmax < 0.0f || tmin > tmax)
		return false;
	if(out_t)
		*out_t = (tmin >= 0.0f) ? tmin : tmax;
	return true;
}
#undef AABB_RAY_ENTRY_EPS

void aabb_expand(struct AABB* a, float d) {
	assert(a);
	a->x1 -= d;
	a->x2 += d;
	a->y1 -= d;
	a->y2 += d;
	a->z1 -= d;
	a->z2 += d;
}
