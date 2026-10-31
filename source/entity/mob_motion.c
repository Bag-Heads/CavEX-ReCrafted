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

/* Mob motion units S1 -- see mob_motion.h. Every formula below is the
 * decompile's own (740c583, ../mcb173-retromcp src):
 *   AxisAlignedBB.calculateX/Y/ZOffset, intersectsWith, addCoord, offset;
 *   World.getCollidingBoundingBoxes:951-987;
 *   Block.getCollidingBoundingBoxes:259-265;
 *   Entity.setPosition:126-132, Entity.moveEntity:252-434 (noClip false,
 *   isSneaking false for mobs, field_9293_aM true so its `!field_9293_aM`
 *   branches never run). */

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mob_motion.h"
#include "entity.h"
#include "../block/blocks.h"
#include "../block/blocks_data.h"
#include "../util.h"

/* ---- AxisAlignedBB --------------------------------------------------------- */

double mob_aabb_calc_x_offset(const struct mob_aabb_d* self,
							  const struct mob_aabb_d* var1, double var2) {
	if(var1->max[1] > self->min[1] && var1->min[1] < self->max[1]) {
		if(var1->max[2] > self->min[2] && var1->min[2] < self->max[2]) {
			double var4;
			if(var2 > 0.0 && var1->max[0] <= self->min[0]) {
				var4 = self->min[0] - var1->max[0];
				if(var4 < var2)
					var2 = var4;
			}
			if(var2 < 0.0 && var1->min[0] >= self->max[0]) {
				var4 = self->max[0] - var1->min[0];
				if(var4 > var2)
					var2 = var4;
			}
		}
	}
	return var2;
}

double mob_aabb_calc_y_offset(const struct mob_aabb_d* self,
							  const struct mob_aabb_d* var1, double var2) {
	if(var1->max[0] > self->min[0] && var1->min[0] < self->max[0]) {
		if(var1->max[2] > self->min[2] && var1->min[2] < self->max[2]) {
			double var4;
			if(var2 > 0.0 && var1->max[1] <= self->min[1]) {
				var4 = self->min[1] - var1->max[1];
				if(var4 < var2)
					var2 = var4;
			}
			if(var2 < 0.0 && var1->min[1] >= self->max[1]) {
				var4 = self->max[1] - var1->min[1];
				if(var4 > var2)
					var2 = var4;
			}
		}
	}
	return var2;
}

double mob_aabb_calc_z_offset(const struct mob_aabb_d* self,
							  const struct mob_aabb_d* var1, double var2) {
	if(var1->max[0] > self->min[0] && var1->min[0] < self->max[0]) {
		if(var1->max[1] > self->min[1] && var1->min[1] < self->max[1]) {
			double var4;
			if(var2 > 0.0 && var1->max[2] <= self->min[2]) {
				var4 = self->min[2] - var1->max[2];
				if(var4 < var2)
					var2 = var4;
			}
			if(var2 < 0.0 && var1->min[2] >= self->max[2]) {
				var4 = self->max[2] - var1->min[2];
				if(var4 > var2)
					var2 = var4;
			}
		}
	}
	return var2;
}

bool mob_aabb_intersects(const struct mob_aabb_d* a,
						 const struct mob_aabb_d* var1) {
	/* AxisAlignedBB.intersectsWith: `var1.maxX > minX && var1.minX < maxX`
	 * per axis (strict). */
	return var1->max[0] > a->min[0] && var1->min[0] < a->max[0]
		&& var1->max[1] > a->min[1] && var1->min[1] < a->max[1]
		&& var1->max[2] > a->min[2] && var1->min[2] < a->max[2];
}

struct mob_aabb_d mob_aabb_add_coord(const struct mob_aabb_d* a, double x,
									 double y, double z) {
	struct mob_aabb_d r = *a;
	const double d[3] = {x, y, z};
	for(int i = 0; i < 3; i++) {
		if(d[i] < 0.0)
			r.min[i] += d[i];
		if(d[i] > 0.0)
			r.max[i] += d[i];
	}
	return r;
}

void mob_aabb_offset(struct mob_aabb_d* a, double x, double y, double z) {
	a->min[0] += x;
	a->min[1] += y;
	a->min[2] += z;
	a->max[0] += x;
	a->max[1] += y;
	a->max[2] += z;
}

#define MOB_MOTION_CELL_BOXES 8

/* ---- box list ---------------------------------------------------------------- */

void mob_box_list_clear(struct mob_box_list* l) {
	l->count = 0;
	l->oom = false;
}

void mob_box_list_free(struct mob_box_list* l) {
	free(l->boxes);
	l->boxes = NULL;
	l->count = l->cap = 0;
	l->oom = false;
}

bool mob_box_list_push(struct mob_box_list* l, const struct mob_aabb_d* b) {
	if(l->count == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 16;
		struct mob_aabb_d* g = realloc(l->boxes, cap * sizeof(*g));
		if(!g) {
			l->oom = true;
			return false;
		}
		l->boxes = g;
		l->cap = cap;
	}
	l->boxes[l->count++] = *b;
	return true;
}

/* ---- World.getCollidingBoundingBoxes ---------------------------------------- */

static int32_t floor_d(double v) {
	/* MathHelper.floor_double: (int)v, minus one when v < (int)v. */
	int32_t i = (int32_t)v;
	return v < (double)i ? i - 1 : i;
}

void mob_colliding_boxes(struct entity* e, const struct mob_aabb_d* q,
						 const struct mob_collide_env* env,
						 struct mob_box_list* out) {
	assert(e && q && out);
	mob_box_list_clear(out);
	const int32_t x0 = floor_d(q->min[0]);
	const int32_t x1 = floor_d(q->max[0] + 1.0);
	const int32_t y0 = floor_d(q->min[1]);
	const int32_t y1 = floor_d(q->max[1] + 1.0);
	const int32_t z0 = floor_d(q->min[2]);
	const int32_t z1 = floor_d(q->max[2] + 1.0);
	for(int32_t x = x0; x < x1; x++) {
		for(int32_t z = z0; z < z1; z++) {
			/* blockExists(x, 64, z): an unresolved column contributes no box. */
			struct block_data probe;
			if(!entity_get_block(e, x, 64, z, &probe))
				continue;
			for(int32_t y = y0 - 1; y < y1; y++) {
				struct block_data blk;
				/* Beta getBlockId answers 0 (air) outside 0..127. */
				if(y < 0 || y >= WORLD_HEIGHT)
					continue;
				if(!entity_get_block(e, x, y, z, &blk) || !blocks[blk.type])
					continue;
				struct block_info bi = {
					.block = &blk,
					.neighbours = NULL,
					.x = x,
					.y = y,
					.z = z,
					.get_piston_moving = env ? env->piston : NULL,
					.get_piston_moving_ctx = env ? env->piston_ctx : NULL,
				};
				struct block* b = blocks[blk.type];
				size_t count = b->getBoundingBox(&bi, true, NULL);
				if(!count)
					continue;
				/* Fable S1 F4: no per-cell VLA on the Wii stack. Every block
				 * returns a handful of boxes (stairs 2, the rest 1); a larger
				 * count takes the heap, and an allocation failure marks the
				 * query incomplete (the move is then skipped, never wrong). */
				struct AABB small[MOB_MOTION_CELL_BOXES];
				struct AABB* boxes = small;
				if(count > MOB_MOTION_CELL_BOXES) {
					boxes = malloc(count * sizeof(*boxes));
					if(!boxes) {
						out->oom = true;
						continue;
					}
				}
				b->getBoundingBox(&bi, true, boxes);
				for(size_t k = 0; k < count; k++) {
					/* (double)x + (double)minX -- the float bound widens
					 * exactly (every Beta bound is dyadic). */
					struct mob_aabb_d bd = {
						.min = {(double)x + (double)boxes[k].x1,
								(double)y + (double)boxes[k].y1,
								(double)z + (double)boxes[k].z1},
						.max = {(double)x + (double)boxes[k].x2,
								(double)y + (double)boxes[k].y2,
								(double)z + (double)boxes[k].z2},
					};
					if(mob_aabb_intersects(q, &bd))
						mob_box_list_push(out, &bd);
				}
				if(boxes != small)
					free(boxes);
			}
		}
	}
	if(env && env->entity_boxes)
		env->entity_boxes(env->entity_ctx, e, q, out);
}

/* ---- Entity.moveEntity ---------------------------------------------------------- */

static void clip_pass(const struct mob_box_list* l, struct mob_aabb_d* bb,
					  double* var1, double* var3, double* var5) {
	for(size_t i = 0; i < l->count; i++)
		*var3 = mob_aabb_calc_y_offset(&l->boxes[i], bb, *var3);
	mob_aabb_offset(bb, 0.0, *var3, 0.0);
	for(size_t i = 0; i < l->count; i++)
		*var1 = mob_aabb_calc_x_offset(&l->boxes[i], bb, *var1);
	mob_aabb_offset(bb, *var1, 0.0, 0.0);
	for(size_t i = 0; i < l->count; i++)
		*var5 = mob_aabb_calc_z_offset(&l->boxes[i], bb, *var5);
	mob_aabb_offset(bb, 0.0, 0.0, *var5);
}

static struct mob_move_result
mob_move_entity_d_impl(struct entity* e, struct mob_motion* m, double var1,
					   double var3, double var5,
					   const struct mob_collide_env* env) {
	assert(e && m);
	struct mob_move_result r = {0};

	/* One reusable list: the server tick is single-threaded and moveEntity
	 * never re-enters itself (no per-mob-tick heap churn on the Wii). */
	static struct mob_box_list s_list;
#define list s_list
	/* Pre-query the swept box BEFORE mutating anything, so an OOM leaves the
	 * entity exactly as it was (a C allocation guard, no Beta analogue). The
	 * web scaling below changes the sweep, so it is folded in first. */
	double w1 = var1, w3 = var3, w5 = var5;
	if(e->in_web) {
		w1 *= 0.25;
		w3 *= (double)0.05F;
		w5 *= 0.25;
	}
	struct mob_aabb_d sweep = mob_aabb_add_coord(&m->bb, w1, w3, w5);
	mob_colliding_boxes(e, &sweep, env, &list);
	if(list.oom) {
		r.incomplete = true;
		return r;
	}

	e->y_size *= 0.4F;
	if(e->in_web) {
		e->in_web = false;
		var1 = w1;
		var3 = w3;
		var5 = w5;
		m->motion[0] = 0.0;
		m->motion[1] = 0.0;
		m->motion[2] = 0.0;
	}

	const double var11 = var1;
	const double var13 = var3;
	const double var15 = var5;
	const struct mob_aabb_d var17 = m->bb;

	/* Y, X, Z against the swept list (Entity.java:303-335). */
	for(size_t i = 0; i < list.count; i++)
		var3 = mob_aabb_calc_y_offset(&list.boxes[i], &m->bb, var3);
	mob_aabb_offset(&m->bb, 0.0, var3, 0.0);
	const bool var36 = e->on_ground || (var13 != var3 && var13 < 0.0);
	for(size_t i = 0; i < list.count; i++)
		var1 = mob_aabb_calc_x_offset(&list.boxes[i], &m->bb, var1);
	mob_aabb_offset(&m->bb, var1, 0.0, 0.0);
	for(size_t i = 0; i < list.count; i++)
		var5 = mob_aabb_calc_z_offset(&list.boxes[i], &m->bb, var5);
	mob_aabb_offset(&m->bb, 0.0, 0.0, var5);

	/* stepHeight retry (Entity.java:340-404); `var18` (sneaking) is false. */
	if(e->step_height > 0.0F && var36 && e->y_size < 0.05F
	   && (var11 != var1 || var15 != var5)) {
		const double var37 = var1;
		const double var23 = var3;
		const double var25 = var5;
		var1 = var11;
		var3 = (double)e->step_height;
		var5 = var15;
		const struct mob_aabb_d var27 = m->bb;
		m->bb = var17;
		struct mob_aabb_d sweep2
			= mob_aabb_add_coord(&m->bb, var11, var3, var15);
		mob_colliding_boxes(e, &sweep2, env, &list);
		if(list.oom) {
			/* Keep the un-stepped result (the first pass is complete). */
			m->bb = var27;
			var1 = var37;
			var3 = var23;
			var5 = var25;
		} else {
			clip_pass(&list, &m->bb, &var1, &var3, &var5);
			/* `!field_9293_aM && var13 != var3` is false: step back down. */
			var3 = (double)(-e->step_height);
			for(size_t i = 0; i < list.count; i++)
				var3 = mob_aabb_calc_y_offset(&list.boxes[i], &m->bb, var3);
			mob_aabb_offset(&m->bb, 0.0, var3, 0.0);

			if(var37 * var37 + var25 * var25 >= var1 * var1 + var5 * var5) {
				var1 = var37;
				var3 = var23;
				var5 = var25;
				m->bb = var27;
			} else {
				double var41 = m->bb.min[1] - (double)((int32_t)m->bb.min[1]);
				if(var41 > 0.0)
					e->y_size = (float)((double)e->y_size + var41 + 0.01);
			}
		}
	}
#undef list

	m->pos[0] = (m->bb.min[0] + m->bb.max[0]) / 2.0;
	m->pos[1] = m->bb.min[1] + (double)e->y_offset - (double)e->y_size;
	m->pos[2] = (m->bb.min[2] + m->bb.max[2]) / 2.0;
	e->collided_horizontally = var11 != var1 || var15 != var5;
	e->collided_vertically = var13 != var3;
	e->on_ground = var13 != var3 && var13 < 0.0;
	e->collided = e->collided_horizontally || e->collided_vertically;
	if(var11 != var1)
		m->motion[0] = 0.0;
	if(var13 != var3)
		m->motion[1] = 0.0;
	if(var15 != var5)
		m->motion[2] = 0.0;

	r.dx = var1;
	r.dy = var3;
	r.dz = var5;
	r.collided_horizontally = e->collided_horizontally;
	r.collided_vertically = e->collided_vertically;
	r.on_ground = e->on_ground;
	mob_motion_mirror(e, m);
	return r;
}

/* ---- mutation API -------------------------------------------------------------- */

/* Inactive (S1) write semantics: the float fields are still what the live
 * tick integrates, so every API write reproduces the legacy float write BIT
 * FOR BIT (float = float op (float)arg, exactly the expression the converted
 * call site used) and the double state then follows the float
 * ((double)float). That keeps existing behaviour byte-identical while
 * proving the writer is routed through the API. Active (S2) semantics: the
 * double is authoritative and the float is its mirror. */

static void set_box_from_pos(struct entity* e, struct mob_motion* m) {
	/* Entity.setPosition's box from width/height/yOffset/ySize. */
	const float var7 = e->width / 2.0F;
	const float var8 = e->height;
	const double x = m->pos[0], y = m->pos[1], z = m->pos[2];
	m->bb.min[0] = x - (double)var7;
	m->bb.min[1] = y - (double)e->y_offset + (double)e->y_size;
	m->bb.min[2] = z - (double)var7;
	m->bb.max[0] = x + (double)var7;
	m->bb.max[1] = y - (double)e->y_offset + (double)e->y_size + (double)var8;
	m->bb.max[2] = z + (double)var7;
}

void mob_motion_set_position(struct entity* e, struct mob_motion* m, double x,
							 double y, double z) {
	assert(e && m);
#if MOB_MOTION_ACTIVE
	m->pos[0] = x;
	m->pos[1] = y;
	m->pos[2] = z;
	for(int i = 0; i < 3; i++)
		e->pos[i] = (float)m->pos[i];
#else
	e->pos[0] = (float)x;
	e->pos[1] = (float)y;
	e->pos[2] = (float)z;
	for(int i = 0; i < 3; i++)
		m->pos[i] = (double)e->pos[i];
#endif
	set_box_from_pos(e, m);
	for(int i = 0; i < 3; i++)
		m->motion[i] = m->valid ? m->motion[i] : (double)e->vel[i];
	m->valid = true;
	for(int i = 0; i < 3; i++)
		e->vel[i] = (float)m->motion[i];
}

void mob_motion_seed(struct entity* e, struct mob_motion* m, double x,
					 double y, double z, double mx, double my, double mz) {
	assert(e && m);
	/* Entity.setPositionAndRotation: prevPos = pos, ySize = 0, setPosition. */
	e->y_size = 0.0F;
	m->valid = true;
#if MOB_MOTION_ACTIVE
	m->motion[0] = mx;
	m->motion[1] = my;
	m->motion[2] = mz;
#else
	m->motion[0] = (double)(float)mx;
	m->motion[1] = (double)(float)my;
	m->motion[2] = (double)(float)mz;
#endif
	mob_motion_set_position(e, m, x, y, z);
	for(int i = 0; i < 3; i++)
		m->prev_pos[i] = m->pos[i];
}

void mob_motion_set_velocity(struct entity* e, struct mob_motion* m, double mx,
							 double my, double mz) {
	assert(e && m);
#if MOB_MOTION_ACTIVE
	m->motion[0] = mx;
	m->motion[1] = my;
	m->motion[2] = mz;
	for(int i = 0; i < 3; i++)
		e->vel[i] = (float)m->motion[i];
#else
	e->vel[0] = (float)mx;
	e->vel[1] = (float)my;
	e->vel[2] = (float)mz;
	for(int i = 0; i < 3; i++)
		m->motion[i] = (double)e->vel[i];
#endif
}

void mob_motion_add_velocity(struct entity* e, struct mob_motion* m, double dx,
							 double dy, double dz) {
	assert(e && m);
	const double d[3] = {dx, dy, dz};
#if MOB_MOTION_ACTIVE
	/* Entity.addVelocity / `motionX += d`: every axis, zero included. */
	for(int i = 0; i < 3; i++) {
		m->motion[i] += d[i];
		e->vel[i] = (float)m->motion[i];
	}
#else
	/* Legacy site shape `v = (float)((double)v + d)`; an axis the legacy
	 * site did not write (delta exactly 0) is left untouched, so a -0.0F
	 * survives exactly as before. */
	for(int i = 0; i < 3; i++) {
		if(d[i] != 0.0)
			e->vel[i] = (float)((double)e->vel[i] + d[i]);
		m->motion[i] = (double)e->vel[i];
	}
#endif
}

void mob_motion_add_velocity_f(struct entity* e, struct mob_motion* m,
							   double dx, double dy, double dz) {
	assert(e && m);
#if MOB_MOTION_ACTIVE
	mob_motion_add_velocity(e, m, dx, dy, dz);
#else
	/* Legacy site shape `v += (float)(d)` (float arithmetic). */
	const double d[3] = {dx, dy, dz};
	for(int i = 0; i < 3; i++) {
		if(d[i] != 0.0)
			e->vel[i] += (float)d[i];
		m->motion[i] = (double)e->vel[i];
	}
#endif
}

void mob_motion_mirror(struct entity* e, const struct mob_motion* m) {
	for(int i = 0; i < 3; i++) {
		e->pos[i] = (float)m->pos[i];
		e->vel[i] = (float)m->motion[i];
	}
}

bool mob_motion_mirror_matches(const struct entity* e,
							   const struct mob_motion* m) {
	for(int i = 0; i < 3; i++) {
		/* Compare representations, so a NaN written by the same path still
		 * matches and a bypassing writer never hides behind -0.0 == 0.0. */
		float p = (float)m->pos[i], v = (float)m->motion[i];
		if(memcmp(&p, &e->pos[i], sizeof p) != 0
		   || memcmp(&v, &e->vel[i], sizeof v) != 0)
			return false;
	}
	return true;
}

static uint32_t s_bypass;

static void adopt_floats(struct entity* e, struct mob_motion* m) {
	for(int i = 0; i < 3; i++) {
		m->prev_pos[i] = (double)e->pos_old[i];
		m->motion[i] = (double)e->vel[i];
		m->pos[i] = (double)e->pos[i];
	}
	set_box_from_pos(e, m);
	m->valid = true;
}

void mob_motion_adopt_floats(struct entity* e, struct mob_motion* m) {
	assert(e && m);
	if(!m->valid) {
		adopt_floats(e, m);
		return;
	}
	/* Adopt only what the fixture actually changed: an axis whose float still
	 * equals the mirror keeps its exact double (re-widening an untouched
	 * float would round a body a hair into a face it rests flush against).
	 * The box is rebuilt (Entity.setPosition) only when a position axis or
	 * ySize changed -- after a move the box, not pos, is primary. */
	bool pos_changed = false;
	for(int i = 0; i < 3; i++) {
		float pm = (float)m->pos[i], vm = (float)m->motion[i];
		float om = (float)m->prev_pos[i];
		if(memcmp(&pm, &e->pos[i], sizeof pm) != 0) {
			m->pos[i] = (double)e->pos[i];
			pos_changed = true;
		}
		if(memcmp(&vm, &e->vel[i], sizeof vm) != 0)
			m->motion[i] = (double)e->vel[i];
		if(memcmp(&om, &e->pos_old[i], sizeof om) != 0)
			m->prev_pos[i] = (double)e->pos_old[i];
	}
	double want_min_y = m->pos[1] - (double)e->y_offset + (double)e->y_size;
	if(pos_changed || m->bb.min[1] - want_min_y > 1e-9
	   || want_min_y - m->bb.min[1] > 1e-9)
		set_box_from_pos(e, m);
}

static void report_bypass(const struct entity* e, const char* what) {
	s_bypass++;
	if(s_bypass <= 8)
		fprintf(stderr,
				"[mob_motion] entity %u: %s -- a writer bypassed the mob motion "
				"API (the float write is discarded; doubles are authoritative)\n",
				(unsigned)e->id, what);
}

void mob_motion_tick_begin(struct entity* e, struct mob_motion* m) {
	assert(e && m);
	if(!m->valid) {
		/* Never seeded: no construction path may leave a mob like this (the
		 * constructor seeds). Adopt once so the mob is usable, and count it. */
		report_bypass(e, "unseeded state");
		adopt_floats(e, m);
		return;
	}
	if(!mob_motion_mirror_matches(e, m)) {
#if MOB_MOTION_ACTIVE
		/* Astra Q1: floats never feed back. Restore the mirror. */
		report_bypass(e, "float mirror changed");
		mob_motion_mirror(e, m);
		for(int i = 0; i < 3; i++)
			e->pos_old[i] = (float)m->prev_pos[i];
#else
		report_bypass(e, "float mirror changed");
		adopt_floats(e, m);
#endif
	}
}

void mob_motion_tick_end(struct entity* e, struct mob_motion* m) {
	assert(e && m);
#if !MOB_MOTION_ACTIVE
	adopt_floats(e, m);
#else
	mob_motion_mirror(e, m);
	for(int i = 0; i < 3; i++)
		e->pos_old[i] = (float)m->prev_pos[i];
#endif
}

uint32_t mob_motion_bypass_count(void) {
	return s_bypass;
}

void mob_motion_bypass_reset(void) {
	s_bypass = 0;
}

/* ---- S2: Beta EntityLiving motion pieces -------------------------------------- */

void mob_motion_knockback(struct entity* e, struct mob_motion* m, double var3,
						  double var5, struct random_gen* jitter) {
	assert(e && m);
	/* EntityLiving.attackEntityFrom:338-343: an attacker at (nearly) the same
	 * column is jittered by (Math.random() - Math.random()) * 0.01D until the
	 * squared horizontal distance reaches 1.0E-4D. Math.random is not the
	 * world RNG; a non-simulation stream stands in for it (the caller passes
	 * the cosmetic stream), so rand_src is untouched. */
	int guard = 0;
	while(var3 * var3 + var5 * var5 < 1.0E-4 && guard++ < 64) {
		if(jitter) {
			var3 = ((double)rand_gen_flt(jitter) - (double)rand_gen_flt(jitter))
				* 0.01;
			var5 = ((double)rand_gen_flt(jitter) - (double)rand_gen_flt(jitter))
				* 0.01;
		} else {
			var3 = 0.01;
			var5 = 0.0;
		}
	}
	/* EntityLiving.knockBack:392-405. */
	float var7 = (float)sqrt(var3 * var3 + var5 * var5);
	float var8 = 0.4F;
	m->motion[0] /= 2.0;
	m->motion[1] /= 2.0;
	m->motion[2] /= 2.0;
	m->motion[0] -= var3 / (double)var7 * (double)var8;
	m->motion[1] += (double)0.4F;
	m->motion[2] -= var5 / (double)var7 * (double)var8;
	if(m->motion[1] > (double)0.4F)
		m->motion[1] = (double)0.4F;
	mob_motion_mirror(e, m);
}

void mob_motion_jump(struct entity* e, struct mob_motion* m) {
	/* EntityLiving.jump:659-661. */
	m->motion[1] = (double)0.42F;
	mob_motion_mirror(e, m);
}

bool mob_offset_in_liquid(struct entity* e, const struct mob_motion* m,
						  double dx, double dy, double dz,
						  const struct mob_collide_env* env) {
	/* Entity.isOffsetPositionInLiquid:246-250: the box offset by (dx,dy,dz)
	 * is free iff it collides with nothing AND contains no liquid. */
	struct mob_aabb_d b = m->bb;
	mob_aabb_offset(&b, dx, dy, dz);
	struct mob_box_list l = {0};
	mob_colliding_boxes(e, &b, env, &l);
	bool hit = l.count > 0 || l.oom;
	mob_box_list_free(&l);
	if(hit)
		return false;
	/* World.getIsAnyLiquid:1362-1392 (incl. its negative-min decrements). */
	int32_t x0 = floor_d(b.min[0]), x1 = floor_d(b.max[0] + 1.0);
	int32_t y0 = floor_d(b.min[1]), y1 = floor_d(b.max[1] + 1.0);
	int32_t z0 = floor_d(b.min[2]), z1 = floor_d(b.max[2] + 1.0);
	if(b.min[0] < 0.0)
		x0--;
	if(b.min[1] < 0.0)
		y0--;
	if(b.min[2] < 0.0)
		z0--;
	for(int32_t x = x0; x < x1; x++)
		for(int32_t y = y0; y < y1; y++)
			for(int32_t z = z0; z < z1; z++) {
				struct block_data blk;
				if(y < 0 || y >= WORLD_HEIGHT
				   || !entity_get_block(e, x, y, z, &blk))
					continue;
				if(blk.type == BLOCK_WATER_FLOW || blk.type == BLOCK_WATER_STILL
				   || blk.type == BLOCK_LAVA_FLOW
				   || blk.type == BLOCK_LAVA_STILL)
					return false;
			}
	return true;
}

/* ---- mob-move cost counters (Fable S2 F2; mob_motion_2m / perf_stress_5m) --
 * Written by the server thread, drained by the main thread's once-per-second
 * heartbeat. Relaxed 32-bit atomics: each field is an independent counter; a
 * take may straddle one move, which is noise at a 1 s window. The clock is
 * injected (a monotonic nanosecond counter from main.c) so the entity stratum
 * takes no platform edge; a NULL clock = disabled = one relaxed load per move. */
typedef uint64_t (*mob_motion_clock_fn)(void);
static mob_motion_clock_fn s_cost_clock;
static uint32_t s_cost_moves, s_cost_move_ns, s_cost_env_walks,
	s_cost_env_entities;

void mob_motion_cost_enable(uint64_t (*now_ns)(void)) {
	__atomic_store_n(&s_cost_clock, now_ns, __ATOMIC_RELAXED);
}

bool mob_motion_cost_enabled(void) {
	return __atomic_load_n(&s_cost_clock, __ATOMIC_RELAXED) != NULL;
}

void mob_motion_cost_note_env(uint32_t entities_visited) {
	if(!mob_motion_cost_enabled())
		return;
	__atomic_add_fetch(&s_cost_env_walks, 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&s_cost_env_entities, entities_visited,
					   __ATOMIC_RELAXED);
}

void mob_motion_cost_take(struct mob_motion_cost* out) {
	assert(out);
	out->moves = __atomic_exchange_n(&s_cost_moves, 0u, __ATOMIC_RELAXED);
	uint32_t ns = __atomic_exchange_n(&s_cost_move_ns, 0u, __ATOMIC_RELAXED);
	out->move_us = ns / 1000u;
	out->env_walks
		= __atomic_exchange_n(&s_cost_env_walks, 0u, __ATOMIC_RELAXED);
	out->env_entities
		= __atomic_exchange_n(&s_cost_env_entities, 0u, __ATOMIC_RELAXED);
}

struct mob_move_result mob_move_entity_d(struct entity* e, struct mob_motion* m,
										 double var1, double var3, double var5,
										 const struct mob_collide_env* env) {
	mob_motion_clock_fn clock
		= __atomic_load_n(&s_cost_clock, __ATOMIC_RELAXED);
	if(!clock)
		return mob_move_entity_d_impl(e, m, var1, var3, var5, env);
	uint64_t t0 = clock();
	struct mob_move_result r = mob_move_entity_d_impl(e, m, var1, var3, var5,
													  env);
	uint64_t dt = clock() - t0;
	__atomic_add_fetch(&s_cost_moves, 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&s_cost_move_ns,
					   dt > UINT32_MAX ? UINT32_MAX : (uint32_t)dt,
					   __ATOMIC_RELAXED);
	return r;
}
