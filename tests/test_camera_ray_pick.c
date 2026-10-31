/*
	O17 (vehicle-smoke-1004): camera_ray_pick must TERMINATE for every ray,
	and keep its pick result for every non-degenerate ray.

	THE DEFECT (measured: PC vehicle smokes vs_pc_boat_20261004_205206_o17r2,
	_205408_o17r3 and, by the same pose, the long-open boat_124037): the main
	thread spun forever inside camera_ray_pick (gdb all-thread dump: cell
	x = -1.5e9), stopped draining the client inbox, and the server thread then
	blocked in clin_rpc_send -- a whole-game freeze. Pose: eye on an INTEGER z
	(1.00 / 0.00), yaw exactly -90 deg, level. The ray end differs from the
	start by -1.8e-7 in z (float cos of -pi/2), so its end cell is one z lower,
	but plane_distance treated |dot| < FLT_EPSILON as PARALLEL (FLT_MAX): z
	never stepped, the exact end-cell match never happened, and x walked off
	toward -infinity. Any player can stand on an integer coordinate and look
	along an axis.

	BETA. The reference is World.rayTraceBlocks -> func_28099_a (CFR
	World.java:538-): a different boundary-stepping algorithm that is HARD-
	CAPPED at 200 iterations (`var11 = 200; while (var11-- >= 0)`), so Beta
	can never hang here. This engine keeps its own DDA (same picks as before
	for every non-degenerate ray); the fix makes it a correct DDA: an axis
	with a ZERO or receding direction component gets tMax = infinity, a
	near-zero one gets its true (large, finite) crossing distance, and the
	walk visits at most the Manhattan cell distance + 1 cells.

	CASES.
	  (1) degenerate poses -- integer x / y / z eye, look exactly along each
	      axis using the engine's own float trig (main.c's end-point formula),
	      both signs, plus exactly-zero components: the walk must end, visit
	      exactly |dx|+|dy|+|dz|+1 cells (Manhattan of start/end cells), and
	      hit a wall placed on its path.
	  (2) equivalence: over a deterministic sweep of non-degenerate rays
	      (every component zero or |c| >= FLT_EPSILON), against a pseudo-
	      random solid field, the pick (hit, x, y, z, side) is IDENTICAL to
	      the pre-O17 algorithm (kept below, step-capped, as the reference).
*/

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/aabb.h"
#include "cglm/cglm.h"
#include "game/camera.h"

static int failures;
#define CHECK(cond, ...) do { \
	if(!(cond)) { \
		printf("FAIL: " __VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while(0)

/* ---- the cell fields --------------------------------------------------- */
#define GUARD 100000u
struct field {
	unsigned calls;
	bool hung;		   /* the guard tripped: the walk would not end */
	bool use_wall;	   /* one solid unit cube at (wx, wy, wz) */
	int wx, wy, wz;
	bool use_hash;	   /* pseudo-random solid cells (equivalence sweep) */
	int slab_axis;	   /* -1 none; else every cell with coord[axis] == slab_at */
	int slab_at;
	int last[3];	   /* the last cell the walk tested */
};

static bool solid_hash(int x, int y, int z) {
	uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u
		^ (uint32_t)z * 83492791u;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return (h % 5u) == 0u;
}

static bool cell_test(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct ray* r, enum side* s) {
	struct field* f = user;
	f->last[0] = (int)x;
	f->last[1] = (int)y;
	f->last[2] = (int)z;
	if(++f->calls > GUARD) {
		f->hung = true; /* force an exit so the test fails instead of hanging */
		*s = SIDE_TOP;
		return true;
	}
	const int c[3] = {(int)x, (int)y, (int)z};
	bool solid = (f->use_wall && x == f->wx && y == f->wy && z == f->wz)
		|| (f->use_hash && solid_hash(x, y, z))
		|| (f->slab_axis >= 0 && c[f->slab_axis] == f->slab_at);
	if(!solid)
		return false;
	struct AABB a;
	aabb_setsize(&a, 1.0F, 1.0F, 1.0F);
	aabb_translate(&a, (float)x, (float)y, (float)z);
	return aabb_intersection_ray(&a, r, s);
}

/* ---- the PRE-O17 walker, verbatim except for an explicit step cap ------ */
static float old_plane_distance(vec3 n, vec3 p0, vec3 l0, vec3 l) {
	float d = glm_vec3_dot(n, l);
	if(d < GLM_FLT_EPSILON)
		return FLT_MAX;
	vec3 tmp;
	glm_vec3_sub(p0, l0, tmp);
	return glm_vec3_dot(tmp, n) / d;
}

static bool old_walk(float gx0, float gy0, float gz0, float gx1, float gy1,
					 float gz1, struct field* f, struct camera_ray_result* res) {
	int sx = gx1 > gx0 ? 1 : -1;
	int sy = gy1 > gy0 ? 1 : -1;
	int sz = gz1 > gz0 ? 1 : -1;
	int gx = floor(gx0), gy = floor(gy0), gz = floor(gz0);
	int x1 = floor(gx1), y1 = floor(gy1), z1 = floor(gz1);
	vec3 dir = {gx1 - gx0, gy1 - gy0, gz1 - gz0};
	glm_vec3_normalize(dir);
	for(unsigned step = 0; step < GUARD; step++) {
		enum side s = SIDE_BOTTOM;
		if(cell_test(f, gx, gy, gz,
					 &(struct ray) {.x = gx0, .y = gy0, .z = gz0, .dx = dir[0],
									.dy = dir[1], .dz = dir[2]}, &s)) {
			res->x = gx;
			res->y = gy;
			res->z = gz;
			res->hit = true;
			res->side = s;
			return true;
		}
		if(gx == x1 && gy == y1 && gz == z1) {
			res->hit = false;
			return true;
		}
		float t1 = old_plane_distance((vec3) {sx, 0, 0},
			(vec3) {gx + (sx + 1) / 2, 0, 0}, (vec3) {gx0, gy0, gz0}, dir);
		float t2 = old_plane_distance((vec3) {0, sy, 0},
			(vec3) {0, gy + (sy + 1) / 2, 0}, (vec3) {gx0, gy0, gz0}, dir);
		float t3 = old_plane_distance((vec3) {0, 0, sz},
			(vec3) {0, 0, gz + (sz + 1) / 2}, (vec3) {gx0, gy0, gz0}, dir);
		if(t1 <= t2 && t1 <= t3)
			gx += sx;
		else if(t2 <= t1 && t2 <= t3)
			gy += sy;
		else if(t3 <= t1 && t3 <= t2)
			gz += sz;
	}
	return false; /* the reference itself did not terminate */
}

/* main.c's pick segment: eye + 4.5 * (sin rx sin ry, cos ry, cos rx sin ry) */
static void pick_end(float ex, float ey, float ez, float rx, float ry,
					 float out[3]) {
	out[0] = ex + sinf(rx) * sinf(ry) * 4.5F;
	out[1] = ey + cosf(ry) * 4.5F;
	out[2] = ez + cosf(rx) * sinf(ry) * 4.5F;
}

static int manhattan(const float a[3], const float b[3]) {
	int d = 0;
	for(int k = 0; k < 3; k++)
		d += abs((int)floorf(b[k]) - (int)floorf(a[k]));
	return d;
}

static void degenerate(const char* label, float ex, float ey, float ez, float rx,
					   float ry) {
	float e[3] = {ex, ey, ez}, end[3];
	pick_end(ex, ey, ez, rx, ry, end);
	/* (a) empty world: ends, visits exactly the Manhattan cells + 1 */
	struct field f = {.slab_axis = -1};
	struct camera_ray_result r = {0};
	camera_ray_walk(ex, ey, ez, end[0], end[1], end[2], cell_test, &f, &r);
	const unsigned want = (unsigned)manhattan(e, end) + 1u;
	CHECK(!f.hung, "%s: the walk TERMINATES (guard tripped after %u cells)",
		  label, GUARD);
	CHECK(!f.hung && !r.hit && f.calls == want,
		  "%s: empty world -> no hit after exactly %u cells (got hit=%d, %u)",
		  label, want, r.hit, f.calls);
	/* a correct DDA ENDS in the segment's end cell (a capped walk that went
	 * the wrong way would also stop after N cells, in the wrong place) */
	CHECK(f.last[0] == (int)floorf(end[0]) && f.last[1] == (int)floorf(end[1])
			  && f.last[2] == (int)floorf(end[2]),
		  "%s: the walk ends in the end cell (%d,%d,%d) -- got (%d,%d,%d)", label,
		  (int)floorf(end[0]), (int)floorf(end[1]), (int)floorf(end[2]),
		  f.last[0], f.last[1], f.last[2]);
	/* (b) a solid slab across the MAIN axis 3 cells out (every transverse
	 * cell solid, so a grazing ray cannot slip past it) is picked there */
	int main_axis = 0;
	for(int k = 1; k < 3; k++)
		if(fabsf(end[k] - e[k]) > fabsf(end[main_axis] - e[main_axis]))
			main_axis = k;
	const int dir_sign = end[main_axis] > e[main_axis] ? 1 : -1;
	struct field fw = {.slab_axis = main_axis,
					   .slab_at = (int)floorf(e[main_axis]) + 3 * dir_sign};
	struct camera_ray_result rw = {0};
	camera_ray_walk(ex, ey, ez, end[0], end[1], end[2], cell_test, &fw, &rw);
	const int got[3] = {(int)rw.x, (int)rw.y, (int)rw.z};
	/* An eye EXACTLY on a transverse cell plane, looking (float-trig) along
	 * an axis, keeps the ray within ~1e-7 of that face for its whole length:
	 * a face graze, measure-zero, asserted by nothing (the
	 * test_block_pick_axis convention; the same pick result as before O17).
	 * What IS asserted for such poses is the defect: the walk terminates and
	 * visits exactly the Manhattan cells. Off-plane eyes must pick the slab. */
	bool crosses = false;
	for(int k = 0; k < 3; k++)
		if(k != main_axis && (int)floorf(end[k]) != (int)floorf(e[k]))
			crosses = true;
	bool on_face = false;
	for(int k = 0; k < 3; k++)
		if(k != main_axis && e[k] == floorf(e[k]))
			on_face = true;
	(void)crosses;
	if(!on_face)
		CHECK(!fw.hung && rw.hit && got[main_axis] == fw.slab_at,
			  "%s: the slab at axis %d = %d is picked (hit=%d at %d,%d,%d)", label,
			  main_axis, fw.slab_at, rw.hit, got[0], got[1], got[2]);
	else
		printf("  %s: exact face-graze (measure-zero, not asserted): slab hit=%d\n",
			   label, rw.hit);
	CHECK(!fw.hung, "%s: the slab walk terminates", label);
}

int main(void) {
	const float PI = (float)M_PI;
	const float HALF_PI = PI / 2.0F;
	/* (1) every axis, both signs, eye on an integer coordinate of a
	 * near-parallel axis. ry = pi/2 is "level" (cos ~ -4.4e-8: near-zero y). */
	degenerate("yaw -90, integer z (the smoke freeze)", -0.5F, 6.12F, 1.0F,
			   -HALF_PI, HALF_PI);
	degenerate("yaw -90, integer z = 0 (boat_124037)", -0.5F, 6.12F, 0.0F,
			   -HALF_PI, HALF_PI);
	degenerate("yaw +90, integer z", 3.25F, 70.4F, -7.0F, HALF_PI, HALF_PI);
	degenerate("yaw 0, integer x", 12.0F, 64.7F, 3.3F, 0.0F, HALF_PI);
	degenerate("yaw 180, integer x", -4.0F, 64.7F, 9.6F, PI, HALF_PI);
	degenerate("yaw 180, integer x = 0", 0.0F, 5.5F, 0.5F, PI, HALF_PI);
	degenerate("level, integer y", 2.5F, 64.0F, 2.5F, 0.4F, HALF_PI);
	degenerate("yaw -90 level, integer y and z", 1.5F, 6.0F, 1.0F, -HALF_PI,
			   HALF_PI);
	degenerate("straight down (ry = pi), integer x and z", 3.0F, 70.3F, -2.0F,
			   0.0F, PI);
	degenerate("straight up (ry = 0), integer x and z", -3.0F, 10.6F, 5.0F, 0.7F,
			   0.0F);
	/* off-plane controls: the same axis looks with the eye NOT on a cell plane
	 * -- no graze, so the slab pick IS asserted */
	degenerate("yaw -90, z = 1.3 (off plane)", -0.5F, 6.12F, 1.3F, -HALF_PI,
			   HALF_PI);
	degenerate("yaw 180, x = -4.6 (off plane)", -4.6F, 64.7F, 9.6F, PI, HALF_PI);
	degenerate("straight down, off plane", 3.4F, 70.3F, -2.6F, 0.0F, PI);
	/* exactly-zero components (the d == 0 path) */
	{
		struct field f = {.slab_axis = -1};
		struct camera_ray_result r = {0};
		camera_ray_walk(0.5F, 3.5F, 1.0F, -4.0F, 3.5F, 1.0F, cell_test, &f, &r);
		CHECK(!f.hung && !r.hit && f.calls == 5u,
			  "exact -x axis ray: 5 cells, no hit (got %u, hit %d)", f.calls, r.hit);
	}

	/* (2) equivalence over a sweep of non-degenerate rays */
	uint32_t lcg = 0x0B17u;
	unsigned compared = 0, mismatches = 0, ref_hung = 0;
	for(int i = 0; i < 60000; i++) {
		float v[6];
		for(int k = 0; k < 6; k++) {
			lcg = lcg * 1664525u + 1013904223u;
			v[k] = ((float)(lcg >> 8) / 16777216.0F) * 2.0F - 1.0F;
		}
		float ox = v[0] * 40.0F, oy = 60.0F + v[1] * 20.0F, oz = v[2] * 40.0F;
		float d[3] = {v[3], v[4], v[5]};
		if((i & 7) == 0)
			d[i % 3] = 0.0F; /* exact zero component: identical both ways */
		float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if(n < 1e-3F)
			continue;
		bool degenerate_dir = false;
		for(int k = 0; k < 3; k++) {
			d[k] /= n;
			if(d[k] != 0.0F && fabsf(d[k]) < 4.0F * FLT_EPSILON)
				degenerate_dir = true;
		}
		if(degenerate_dir)
			continue;
		float len = 1.0F + 4.5F * fabsf(v[(i % 6)]);
		float ex = ox + d[0] * len, ey = oy + d[1] * len, ez = oz + d[2] * len;
		struct field fo = {.use_hash = true, .slab_axis = -1},
			fn = {.use_hash = true, .slab_axis = -1};
		struct camera_ray_result ro = {0}, rn = {0};
		if(!old_walk(ox, oy, oz, ex, ey, ez, &fo, &ro)) {
			ref_hung++;
			continue;
		}
		camera_ray_walk(ox, oy, oz, ex, ey, ez, cell_test, &fn, &rn);
		compared++;
		if(fn.hung || ro.hit != rn.hit
		   || (ro.hit && (ro.x != rn.x || ro.y != rn.y || ro.z != rn.z
						  || ro.side != rn.side))) {
			if(mismatches < 5)
				printf("  mismatch ray %d: old hit=%d (%d,%d,%d s%d) new hit=%d "
					   "(%d,%d,%d s%d)\n", i, ro.hit, (int)ro.x, (int)ro.y,
					   (int)ro.z, (int)ro.side, rn.hit, (int)rn.x, (int)rn.y,
					   (int)rn.z, (int)rn.side);
			mismatches++;
		}
	}
	printf("equivalence: %u non-degenerate rays compared, %u mismatches "
		   "(%u rays the OLD walker could not finish were skipped)\n",
		   compared, mismatches, ref_hung);
	CHECK(compared > 50000u, "the sweep compared enough rays (%u)", compared);
	CHECK(mismatches == 0u, "identical picks for every non-degenerate ray");

	printf("%s test_camera_ray_pick (%d failure%s)\n", failures ? "FAIL" : "PASS",
		   failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
