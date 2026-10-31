/* [740c583 World.func_28105_a:704-851, Block.collisionRayTrace:357-420,
 * Vec3D.getIntermediateWith{X,Y,Z}Value:101-137]. World-free Beta point ray. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "block_ray.h"

static double face_min(const struct block_ray_cell* b, int axis) {
	return axis == 0 ? b->x1 : axis == 1 ? b->y1 : b->z1;
}
static double face_max(const struct block_ray_cell* b, int axis) {
	return axis == 0 ? b->x2 : axis == 1 ? b->y2 : b->z2;
}

bool block_ray_collision_trace(const struct block_ray_cell* b,
							   const double from[3], const double to[3],
							   struct block_ray_hit* out) {
	/* Block.java:357-420 tries minX,maxX,minY,maxY,minZ,maxZ, keeps
	 * the first candidate on a distance tie, with inclusive other bounds. */
	static const int side[6] = {4, 5, 0, 1, 2, 3};
	bool found = false;
	float best = 0;
	for(int i = 0; i < 6; i++) {
		int axis = i / 2, a = (axis + 1) % 3, c = (axis + 2) % 3;
		double delta = to[axis] - from[axis];
		if(delta * delta < (double)1.0E-7F)
			continue;
		double plane = (i & 1) ? face_max(b, axis) : face_min(b, axis);
		double t = (plane - from[axis]) / delta;
		if(t < 0.0 || t > 1.0)
			continue;
		double p[3] = {from[0] + (to[0] - from[0]) * t,
			from[1] + (to[1] - from[1]) * t,
			from[2] + (to[2] - from[2]) * t};
		if(p[a] < face_min(b, a) || p[a] > face_max(b, a)
		   || p[c] < face_min(b, c) || p[c] > face_max(b, c))
			continue;
		double dx = p[0] - from[0], dy = p[1] - from[1], dz = p[2] - from[2];
		float distance = (float)sqrt(dx * dx + dy * dy + dz * dz);
		if(found && !(distance < best))
			continue;
		found = true;
		best = distance;
		if(out) {
			out->side = side[i];
			out->hit[0] = p[0]; out->hit[1] = p[1]; out->hit[2] = p[2];
		}
	}
	return found;
}

static bool cell_hit(int x, int y, int z, const double from[3],
				 const double to[3], bool liquid, bool skip,
				 block_ray_cell_fn fn, void* ctx, struct block_ray_hit* out,
				 struct block_ray_stats* stats) {
	struct block_ray_cell b = {0};
	if(stats) stats->cells++;
	if(!fn(x, y, z, liquid, &b, ctx) || !b.present || !b.collidable
	   || (skip && !b.has_collision_box) || !b.has_ray_box)
		return false;
	if(stats) stats->box_tests++;
	double local_from[3] = {from[0] - x, from[1] - y, from[2] - z};
	double local_to[3] = {to[0] - x, to[1] - y, to[2] - z};
	struct block_ray_hit hit;
	if(!block_ray_collision_trace(&b, local_from, local_to, &hit))
		return false;
	if(out) {
		*out = hit;
		out->x = x; out->y = y; out->z = z;
		out->hit[0] += x; out->hit[1] += y; out->hit[2] += z;
	}
	return true;
}

bool block_ray_trace(const double from[3], const double to[3],
					 bool stop_on_liquid, bool skip_no_collision_box,
					 block_ray_cell_fn fn, void* ctx,
					 struct block_ray_hit* out, struct block_ray_stats* stats) {
	if(stats) *stats = (struct block_ray_stats) {0};
	if(!from || !to || !fn)
		return false;
	for(int i = 0; i < 3; i++)
		if(isnan(from[i]) || isnan(to[i])) return false;
	int end[3] = {(int)floor(to[0]), (int)floor(to[1]), (int)floor(to[2])};
	int cell[3] = {(int)floor(from[0]), (int)floor(from[1]), (int)floor(from[2])};
	double cur[3] = {from[0], from[1], from[2]};
	if(cell_hit(cell[0],cell[1],cell[2],cur,to,stop_on_liquid,
			skip_no_collision_box,fn,ctx,out,stats)) return true;
	/* Java var11=200; while(var11-- >= 0) visits up to 201 later cells. */
	for(int remaining = 200; remaining-- >= 0;) {
		for(int i=0;i<3;i++) if(isnan(cur[i])) return false;
		if(cell[0]==end[0] && cell[1]==end[1] && cell[2]==end[2])
			return false;
		bool active[3]; double plane[3], frac[3] = {999.0,999.0,999.0};
		for(int i=0;i<3;i++) {
			active[i] = end[i] != cell[i];
			plane[i] = end[i] > cell[i] ? (double)cell[i]+1.0 : (double)cell[i];
			if(active[i]) frac[i] = (plane[i]-cur[i])/(to[i]-cur[i]);
		}
		int axis = frac[0] < frac[1] && frac[0] < frac[2] ? 0
			: frac[1] < frac[2] ? 1 : 2;
		bool negative = end[axis] < cell[axis];
		double t = frac[axis];
		for(int i=0;i<3;i++) if(i != axis) cur[i] += (to[i]-cur[i])*t;
		cur[axis] = plane[axis];
		for(int i=0;i<3;i++) cell[i] = (int)floor(cur[i]);
		if(negative) {
			/* Negative-face step: floor(exact plane) belongs to previous cell. */
			cell[axis]--;
		}
		if(cell_hit(cell[0],cell[1],cell[2],cur,to,stop_on_liquid,
				skip_no_collision_box,fn,ctx,out,stats)) return true;
	}
	return false;
}
