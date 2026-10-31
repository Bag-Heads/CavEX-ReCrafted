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

#ifndef MOB_MOTION_H
#define MOB_MOTION_H

/* Mob motion units, slice S1 (docs/intent/mob-motion-units.md; Astra rulings
 * 2026-09-27, tmp/jobs/next-gaps-0924/codex/astra-motionq-0927.out.md).
 *
 * Beta 1.7.3 keeps an entity's physical state in DOUBLES -- posX/Y/Z,
 * prevPosX/Y/Z, motionX/Y/Z and the persistent AxisAlignedBB boundingBox --
 * and only a few literals are floats ((double)0.98F, (double)0.42F, ySize,
 * stepHeight, width/height, fallDistance). This module is the mob-only
 * authoritative double state (Q1 ruling (a)) plus a faithful double port of
 * Entity.moveEntity (the collision/step math) and the mutation API every
 * writer of mob position/motion goes through.
 *
 * S2 (ACTIVE, MOB_MOTION_ACTIVE 1): the doubles ARE the mob's state. The
 * monster tick integrates them in Beta's order (entity_monster.c) and every
 * external writer goes through the mutation API; e->pos / e->vel are
 * write-only float mirrors (each the single rounding of its double) that
 * never feed back: the tick bracket counts any direct float write
 * (mob_motion_bypass_count) and restores the mirror. (S1 shipped the same
 * plumbing inactive, with the float tick authoritative.)
 *
 * This header is deliberately dependency-free (it is embedded in
 * struct entity_monster by entity.h). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MOB_MOTION_ACTIVE 1

/* AxisAlignedBB in doubles: [0..2] = minX,minY,minZ, [3..5] = maxX,maxY,maxZ. */
struct mob_aabb_d {
	double min[3];
	double max[3];
};

struct mob_motion {
	double pos[3];      /* posX, posY, posZ */
	double prev_pos[3]; /* prevPosX/Y/Z (Entity.onEntityUpdate) */
	double motion[3];   /* motionX/Y/Z, blocks per tick */
	struct mob_aabb_d bb; /* the persistent Entity.boundingBox */
	bool valid;         /* false until seeded (spawn/load/teleport) */
};

struct entity;

/* ---- AxisAlignedBB (verbatim AxisAlignedBB.java arithmetic) ------------- */
double mob_aabb_calc_x_offset(const struct mob_aabb_d* self,
							  const struct mob_aabb_d* mover, double d);
double mob_aabb_calc_y_offset(const struct mob_aabb_d* self,
							  const struct mob_aabb_d* mover, double d);
double mob_aabb_calc_z_offset(const struct mob_aabb_d* self,
							  const struct mob_aabb_d* mover, double d);
bool mob_aabb_intersects(const struct mob_aabb_d* a, const struct mob_aabb_d* b);
/* AxisAlignedBB.addCoord: extend min or max per sign. */
struct mob_aabb_d mob_aabb_add_coord(const struct mob_aabb_d* a, double x,
									 double y, double z);
void mob_aabb_offset(struct mob_aabb_d* a, double x, double y, double z);

/* ---- colliding boxes (World.getCollidingBoundingBoxes) ------------------ */
/* Growable list of collision boxes. */
struct mob_box_list {
	struct mob_aabb_d* boxes;
	size_t count;
	size_t cap;
	bool oom; /* an append failed; the move treats the query as incomplete */
};
void mob_box_list_clear(struct mob_box_list* l);
void mob_box_list_free(struct mob_box_list* l);
bool mob_box_list_push(struct mob_box_list* l, const struct mob_aabb_d* b);

/* Optional extra-box source (World.getCollidingBoundingBoxes' entity half:
 * other entities' getBoundingBox() -- boats/minecarts -- inside
 * query.expand(0.25) and intersecting the query). NULL = blocks only. */
typedef void (*mob_entity_boxes_fn)(void* ctx, const struct entity* self,
									const struct mob_aabb_d* query,
									struct mob_box_list* out);

/* The world context of a collision query. `piston` is the moving-piston
 * lookup a block_info needs (blocks_data.h get_piston_moving; w_coord_t is
 * int32_t) so a BlockPistonMoving cell yields its tile-entity box exactly
 * like the float path's entity_piston_moving_lookup. Any member may be NULL
 * (no entity boxes / no piston boxes). */
struct piston_moving_state;
struct mob_collide_env {
	mob_entity_boxes_fn entity_boxes;
	void* entity_ctx;
	bool (*piston)(void* ctx, int32_t x, int32_t y, int32_t z,
				   struct piston_moving_state* out);
	void* piston_ctx;
};

/* Block half: for x in floor(minX) .. floor(maxX + 1) exclusive, z likewise,
 * skipping unloaded columns (blockExists(x, 64, z)), y from floor(minY) - 1
 * to floor(maxY + 1) exclusive; each block's collision boxes that intersect
 * the query (Block.getCollidingBoundingBoxes, strict intersectsWith). The
 * engine's block boxes are floats; every Beta block-box bound is dyadic
 * (n/16, n/8, 1.5, ...), so the float -> double widening is exact and the
 * box equals Beta's (double)x + minX. */
void mob_colliding_boxes(struct entity* e, const struct mob_aabb_d* query,
						 const struct mob_collide_env* env,
						 struct mob_box_list* out);

/* ---- Entity.moveEntity (double port) ------------------------------------ */
struct mob_move_result {
	double dx, dy, dz; /* var1/var3/var5 after clipping (the applied move) */
	bool collided_horizontally;
	bool collided_vertically;
	bool on_ground;
	bool incomplete; /* box query hit OOM: nothing was moved */
};

/* Entity.moveEntity(var1, var3, var5) for a mob (noClip false, never
 * sneaking, field_9293_aM true): ySize *= 0.4F; web scaling (and motion
 * zero); Y then X then Z clip against the swept box list; the stepHeight
 * retry when grounded (or landing) and ySize < 0.05F and a horizontal clip
 * happened, keeping the longer horizontal move and banking ySize; pos from
 * the box; collided flags; onGround; motion zeroed on each clipped axis.
 * Reads/writes e->y_size, e->in_web, e->on_ground and the collided flags on
 * the entity (Beta floats/booleans) and m->pos/bb/motion (doubles). Does NOT
 * run updateFallState, walking sounds or block-collide callbacks: the caller
 * owns those (it gets the applied dy and on_ground in the result). */
struct mob_move_result mob_move_entity_d(struct entity* e, struct mob_motion* m,
										 double dx, double dy, double dz,
										 const struct mob_collide_env* env);

/* ---- mutation API (every writer of mob position / motion) -------------- */
/* Entity.setPosition: pos, and the box from width/height/yOffset/ySize. */
void mob_motion_set_position(struct entity* e, struct mob_motion* m, double x,
							 double y, double z);
/* Seed from a teleport/spawn/load: setPosition + prevPos = pos, ySize 0
 * (Entity.setPositionAndRotation), motion kept as given. */
void mob_motion_seed(struct entity* e, struct mob_motion* m, double x,
					 double y, double z, double mx, double my, double mz);
void mob_motion_set_velocity(struct entity* e, struct mob_motion* m, double mx,
							 double my, double mz);
/* Entity.addVelocity shape: motion += d in double. Inactive: reproduces the
 * legacy `v = (float)((double)v + d)` write. */
void mob_motion_add_velocity(struct entity* e, struct mob_motion* m, double dx,
							 double dy, double dz);
/* Same in active mode; inactive it reproduces the legacy float-arithmetic
 * site shape `v += (float)(d)` (the blast impulse / fishing-pull sites) so
 * S1 stays byte-identical. S2 retires it with the float integrator. */
void mob_motion_add_velocity_f(struct entity* e, struct mob_motion* m,
							   double dx, double dy, double dz);
/* Write the float mirrors e->pos/e->vel from the doubles. */
void mob_motion_mirror(struct entity* e, const struct mob_motion* m);
/* True when the float mirrors equal (float) of the doubles. */
bool mob_motion_mirror_matches(const struct entity* e,
							   const struct mob_motion* m);
/* Inactive-mode tick bracket: at tick start, count a bypass if the mirror no
 * longer matches (then re-seed from the floats so the diagnostic does not
 * cascade); at tick end, re-seed from the float result. Both are removed by
 * S2's activation. */
void mob_motion_tick_begin(struct entity* e, struct mob_motion* m);
void mob_motion_tick_end(struct entity* e, struct mob_motion* m);
uint32_t mob_motion_bypass_count(void);
void mob_motion_bypass_reset(void);

/* Fixture seam: declare the float fields the authoritative input (a test
 * that hand-places a mob). No production caller -- production writers use the
 * mutation API; the tick bracket discards (and counts) any direct write. */
void mob_motion_adopt_floats(struct entity* e, struct mob_motion* m);

/* ---- S2: Beta EntityLiving pieces -------------------------------------- */
struct random_gen;
/* EntityLiving.attackEntityFrom's knockBack for a FRESH hit by a non-null
 * attacker: var3/var5 = attacker.posX - posX, attacker.posZ - posZ. Halves
 * motion, pushes 0.4 away, motionY += (double)0.4F capped at (double)0.4F.
 * `jitter` stands in for Math.random in the same-column case (non-sim
 * stream; may be NULL). */
void mob_motion_knockback(struct entity* e, struct mob_motion* m, double var3,
						  double var5, struct random_gen* jitter);
/* EntityLiving.jump: motionY = (double)0.42F. */
void mob_motion_jump(struct entity* e, struct mob_motion* m);
/* Entity.isOffsetPositionInLiquid. */
bool mob_offset_in_liquid(struct entity* e, const struct mob_motion* m,
						  double dx, double dy, double dz,
						  const struct mob_collide_env* env);

/* ---- mob-move cost counters (Fable S2 F2) --------------------------------
 * mob_move_entity_d wall time (includes the entity-box walk and the per-cell
 * box queries) and the entity-box walks, per heartbeat window. Enabled only by
 * an armed hw_run profile; a disabled counter costs one relaxed load per move. */
struct mob_motion_cost {
	uint32_t moves;        /* mob_move_entity_d calls */
	uint32_t move_us;      /* summed wall time inside them */
	uint32_t env_walks;    /* entity-dict walks for the box query */
	uint32_t env_entities; /* entities visited by those walks */
};
/* now_ns: a monotonic nanosecond clock (main.c); NULL disables. */
void mob_motion_cost_enable(uint64_t (*now_ns)(void));
bool mob_motion_cost_enabled(void);
void mob_motion_cost_note_env(uint32_t entities_visited);
/* Read and reset (the server thread writes, any thread may take). */
void mob_motion_cost_take(struct mob_motion_cost* out);

#endif
