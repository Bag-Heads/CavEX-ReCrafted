/*
	Axis-aligned block picking must hit and report the true face.

	WHY THIS EXISTS. The bug hunt (build 5fabbb5) flagged aabb.c's
	aabb_intersection_ray as missing the epsilon guard its two sibling
	raycasts (entity_aabb_intersect_ray, arrow_aabb_intersect_ray) carry,
	with the claimed symptom "player can't target blocks they're looking
	directly along an axis at": with e.g. dy=dz=0, inv_y/inv_z are +-inf
	and 0*inf can produce NaN.

	Independent reachability reads (Honey, Qwen, 2026-08-05, CaveWii Dev):
	the branchless slab test is DESIGNED for +-inf. C99 fminf/fmaxf return
	the non-NaN operand, so a lone NaN is filtered at every step; NaN can
	only survive on BOTH slab boundaries of an axis when the box has ZERO
	extent there -- which no real block AABB has. This test is the gate the
	channel agreed on: if any real block AABB mis-picks under an
	axis-aligned ray, the epsilon guard is justified; if every case hits
	with the correct face, the honest fix is the defensive closure in
	aabb.c (final else + assert), not a behavior change to working inf
	math.

	COVERAGE. Unit cube (stone/dirt/sand...) and the half-height slab --
	the two shapes that bound real pick geometry -- against all six axis
	directions, rays through the interior (the symptom case), rays through
	the empty half over a slab (must MISS), rays pointing away (must miss),
	and the s==NULL path. Rays grazing a face plane exactly are
	measure-zero and assert nothing about hit/miss.

	GROUND TRUTH for the sides: aabb.c maps the winning slab boundary
	tmin==tx1/tx2/ty1/ty2/tz1/tz2 to LEFT/RIGHT/BOTTOM/TOP/FRONT/BACK, so
	a ray entering through the low-x face reports SIDE_LEFT, etc.
*/

#include <assert.h>
#include <math.h>
#include <stddef.h>

#include "block/aabb.h"

/* One axis-aligned ray through a real block AABB must hit and name the
 * entry face. */
static void hit_axis(struct AABB* a, float ox, float oy, float oz, float dx,
					 float dy, float dz, enum side want) {
	struct ray r = {.x = ox, .y = oy, .z = oz, .dx = dx, .dy = dy, .dz = dz};
	enum side s = SIDE_TOP; /* any init: a true return must overwrite it */
	assert(aabb_intersection_ray(a, &r, &s)
		   && "an axis-aligned ray through a real block AABB must hit "
			  "(the 'can't target blocks along an axis' symptom)");
	assert(s == want && "the hit must report the true entry face");
}

/* A ray that cannot reach the box must miss. */
static void miss_axis(struct AABB* a, float ox, float oy, float oz, float dx,
					  float dy, float dz) {
	struct ray r = {.x = ox, .y = oy, .z = oz, .dx = dx, .dy = dy, .dz = dz};
	enum side s = SIDE_TOP;
	assert(!aabb_intersection_ray(a, &r, &s)
		   && "an axis-aligned ray outside the box must not pick it");
}

int main(void) {
	/* ------------------------------------------------------------------
	 * UNIT CUBE -- the full-block shape, block-local coords [0,1]^3.
	 * Rays start 2 blocks out and travel along each world axis; the
	 * transverse coordinates sit at the cell CENTER so the transverse
	 * slab boundaries are +-inf (the exact inf math under test).
	 * ------------------------------------------------------------------ */
	struct AABB cube;
	aabb_setsize(&cube, 1.0F, 1.0F, 1.0F);

	hit_axis(&cube, -2.0F, 0.5F, 0.5F, +1.0F, 0.0F, 0.0F, SIDE_LEFT);
	hit_axis(&cube, +2.0F, 0.5F, 0.5F, -1.0F, 0.0F, 0.0F, SIDE_RIGHT);
	hit_axis(&cube, 0.5F, -2.0F, 0.5F, 0.0F, +1.0F, 0.0F, SIDE_BOTTOM);
	hit_axis(&cube, 0.5F, +2.0F, 0.5F, 0.0F, -1.0F, 0.0F, SIDE_TOP);
	hit_axis(&cube, 0.5F, 0.5F, -2.0F, 0.0F, 0.0F, +1.0F, SIDE_FRONT);
	hit_axis(&cube, 0.5F, 0.5F, +2.0F, 0.0F, 0.0F, -1.0F, SIDE_BACK);

	/* Origin INSIDE the box, axis-aligned: the player's own cell is still
	 * targetable; tmin<0 must not veto (tmax > fmax(tmin,0)). The side
	 * reported is the tmin slab -- the entry face BEHIND the origin --
	 * which is what aabb.c computes and callers that care about the hit
	 * (camera_ray_pick's start cell) accept. */
	hit_axis(&cube, 0.5F, 0.5F, 0.5F, +1.0F, 0.0F, 0.0F, SIDE_LEFT);

	/* s == NULL is a legal caller shape (hit test only). */
	{
		struct ray r = {.x = -2.0F, .y = 0.5F, .z = 0.5F,
						.dx = +1.0F, .dy = 0.0F, .dz = 0.0F};
		assert(aabb_intersection_ray(&cube, &r, NULL));
	}

	/* Pointing straight AWAY along the same axis: the box is behind. */
	miss_axis(&cube, -2.0F, 0.5F, 0.5F, -1.0F, 0.0F, 0.0F);
	miss_axis(&cube, 0.5F, +2.0F, 0.5F, 0.0F, +1.0F, 0.0F);

	/* Transverse offset past the face: axis-aligned ray in the NEIGHBOUR
	 * column must not pick this block (no false positives across the
	 * slab). */
	miss_axis(&cube, -2.0F, 1.5F, 0.5F, +1.0F, 0.0F, 0.0F);
	miss_axis(&cube, -2.0F, 0.5F, -0.5F, +1.0F, 0.0F, 0.0F);

	/* ------------------------------------------------------------------
	 * HALF-HEIGHT SLAB (step block): [0,0.5] in Y. The ray along the top
	 * empty half must MISS -- the pick a wrong slab test would let
	 * through -- and a vertical ray must still hit the true top face.
	 * ------------------------------------------------------------------ */
	struct AABB slab;
	aabb_setsize(&slab, 1.0F, 0.5F, 1.0F);

	hit_axis(&slab, -2.0F, 0.25F, 0.5F, +1.0F, 0.0F, 0.0F, SIDE_LEFT);
	hit_axis(&slab, 0.5F, -2.0F, 0.5F, 0.0F, +1.0F, 0.0F, SIDE_BOTTOM);
	hit_axis(&slab, 0.5F, +2.0F, 0.5F, 0.0F, -1.0F, 0.0F, SIDE_TOP);
	miss_axis(&slab, -2.0F, 0.75F, 0.5F, +1.0F, 0.0F, 0.0F);
	miss_axis(&slab, -2.0F, 1.5F, 0.5F, +1.0F, 0.0F, 0.0F);

	/* ------------------------------------------------------------------
	 * GRAZING RAYS ARE OUT OF SCOPE, BY ANALYSIS. With a zero direction
	 * component whose origin coordinate sits EXACTLY on a box boundary
	 * plane (e.g. dy=0, r.y == a->y1), the slab boundary is 0*inf = NaN;
	 * C99 fminf/fmaxf filter the lone NaN, but the surviving +inf
	 * collapses tmin up to tmax and the strict tmax > tmax reports a
	 * miss. That is a measure-zero ray lying IN a face plane -- looking
	 * ALONG a face, not AT one -- and it is not the claimed "can't
	 * target blocks along an axis" symptom, which these interior cases
	 * pin. Asserting hits there would demand the epsilon guard the
	 * channel rejected absent a symptom repro (it would replace working
	 * +-inf slab math with a sentinel). B5's zero-width-box kill removes
	 * the only input class where the NaN can survive BOTH boundaries.
	 * ------------------------------------------------------------------ */

	return 0;
}
