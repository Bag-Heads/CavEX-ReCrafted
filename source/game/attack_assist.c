/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Soft-snap melee assist; the contract is in attack_assist.h. */

#include <assert.h>
#include <float.h>
#include <math.h>

#include "../block/aabb.h"
#include "../entity/entity_painting.h"
#include "../entity/monsters_object.h"
#include "../world.h"
#include "attack_assist.h"

/* Ranking treats angles inside the same 0.1-degree bucket as equal, so the
 * "then distance" tie-break is reachable with float geometry (and two targets
 * a hair apart in angle do not flicker between frames). */
#define ATTACK_ASSIST_ANGLE_BUCKET_DEG 0.1F
/* Distance from the look ray to the hitbox: ternary-search iterations over
 * the ray parameter (the squared distance is convex along the ray). 48
 * iterations shrink [0, 3.0] below 1e-7 blocks. */
#define ATTACK_ASSIST_SEARCH_ITERS 48
/* The corrected ray aims this far inside the hitbox (per axis, capped at the
 * half-extent) so a surface-grazing float ray cannot miss the box it aims at. */
#define ATTACK_ASSIST_AIM_INSET 1e-3F

float attack_assist_half_angle_deg(int level, bool remote_session) {
	if(remote_session)
		return 0.0F;
	switch(level) {
		case ATTACK_ASSIST_LOW: return 3.0F;
		case ATTACK_ASSIST_NORMAL: return 6.0F;
		default: return 0.0F;
	}
}

bool attack_assist_eligible(const struct entity* e) {
	if(!e || e->id == 0 || e->delay_destroy == 0 || e->type != ENTITY_MONSTER
	   || !e->getBoundingBox)
		return false;
	if(e->data.monster.death_time > 0)
		return false;
	switch(e->data.monster.id) {
		case MONSTER_ZOMBIE:
		case MONSTER_CREEPER:
		case MONSTER_SKELETON:
		case MONSTER_SPIDER:
		case MONSTER_SLIME_SMALL:
		case MONSTER_SLIME_MEDIUM:
		case MONSTER_SLIME_BIG:
		case MONSTER_GHAST:
		case MONSTER_GIANT: return true;
		/* Angry neutrals. A wild wolf's ANGRY bit (Beta dataWatcher 16) is
		 * set by a player attack and targets players; a tamed wolf is a pet
		 * whatever its flags say. The pigman bit mirrors anger_level > 0. */
		case MONSTER_WOLF:
			return (e->data.monster.wolf_flags & WOLF_FLAG_ANGRY)
				&& !(e->data.monster.wolf_flags & WOLF_FLAG_TAMED);
		case MONSTER_PIG_ZOMBIE: return e->data.monster.pig_zombie_angry;
		default: return false; /* passive animals, squid */
	}
}

/* Beta canBeCollidedWith() for the client entity kinds [b1.7.3: Entity.java
 * :710 false by default; true in EntityLiving:81 (mobs, players), Minecart
 * :132, Boat:98, Painting:196, TNTPrimed:34, FallingSand:33, Fireball:194].
 * Items, arrows, eggs, snowballs and the fishing bobber keep the default
 * false. A fireball reaches the client as an ENTITY_ARROW display with
 * data.arrow.fireball set. Local player bodies are never collidable here:
 * the own body is the viewer, and the split peer is admitted separately
 * (attack_live_peer). Dead (isDead) = delay_destroy == 0. */
static bool attack_pick_collidable(const struct entity* e) {
	if(!e || e->delay_destroy == 0)
		return false;
	switch(e->type) {
		case ENTITY_MONSTER:
		case ENTITY_REMOTE_PLAYER:
		case ENTITY_MINECART:
		case ENTITY_BOAT:
		case ENTITY_PAINTING:
		case ENTITY_PRIMED_TNT:
		case ENTITY_FALLING_SAND:
		case ENTITY_FIREBALL: return true;
		case ENTITY_ARROW: return e->data.arrow.fireball;
		default: return false;
	}
}

/* Beta getCollisionBorderSize [Entity.java:989-991 0.1F;
 * EntityFireball.java:198-200 1.0F]. */
static float attack_pick_border(const struct entity* e) {
	return e->type == ENTITY_FIREBALL
			|| (e->type == ENTITY_ARROW && e->data.arrow.fireball)
		? 1.0F : 0.1F;
}

/* The entity's grown pick box [EntityRenderer.java:111-112]. Paintings keep
 * their click box in their own placement fields (entity_aabb_intersect_ray's
 * rule: getBoundingBox stays NULL for them on purpose). */
static bool attack_pick_box(const struct entity* e, struct AABB* box) {
	if(e->type == ENTITY_PAINTING) {
		painting_compute_geometry(e->data.painting.art_index,
			e->data.painting.direction, e->data.painting.tile_x,
			e->data.painting.tile_y, e->data.painting.tile_z, NULL, box);
	} else if(!e->getBoundingBox || e->getBoundingBox(e, box) == 0) {
		return false;
	}
	float g = attack_pick_border(e);
	box->x1 -= g; box->y1 -= g; box->z1 -= g;
	box->x2 += g; box->y2 += g; box->z2 += g;
	return true;
}

/* [EntityRenderer.java:113-125]: an eye inside the grown box picks it at
 * distance 0 (isVecInside); otherwise the slab clip's entry point within
 * `reach` of the eye. dir is unit length, so t is a distance. */
static bool attack_pick_ray(const struct entity* e, const vec3 o,
							const vec3 dir, float reach, float* out_t) {
	struct AABB b;
	if(!attack_pick_box(e, &b))
		return false;
	if(o[0] > b.x1 && o[0] < b.x2 && o[1] > b.y1 && o[1] < b.y2
	   && o[2] > b.z1 && o[2] < b.z2) {
		*out_t = 0.0F;
		return true;
	}
	float tmin = 0.0F, tmax = reach;
	const float lo[3] = {b.x1, b.y1, b.z1}, hi[3] = {b.x2, b.y2, b.z2};
	for(int a = 0; a < 3; a++) {
		if(fabsf(dir[a]) < 1e-12F) {
			if(o[a] < lo[a] || o[a] > hi[a])
				return false;
			continue;
		}
		float t1 = (lo[a] - o[a]) / dir[a], t2 = (hi[a] - o[a]) / dir[a];
		if(t1 > t2) { float s = t1; t1 = t2; t2 = s; }
		if(t1 > tmin) tmin = t1;
		if(t2 < tmax) tmax = t2;
		if(tmin > tmax)
			return false;
	}
	*out_t = tmin;
	return true;
}

/* Never ray targets and never occluders: this pane's body and every entity
 * Beta's pick ignores (not canBeCollidedWith). */
static bool attack_ray_skips(const struct entity* e,
							 const struct entity* self) {
	return !e || e == self || !attack_pick_collidable(e);
}

/* The local split peer body, when it may take part in this pick (see
 * attack_pick_input.peer): present in this pane's entity view under its own
 * id, not this pane's body, alive, and boxed. */
static const struct entity* attack_live_peer(const struct attack_pick_input* in) {
	const struct entity* p = in->peer;
	if(!p || p == in->self)
		return NULL;
	/* Membership first: only a body this view really holds is dereferenced. */
	struct entity** slot = dict_entity_get(*in->entities, p->id);
	if(!slot || *slot != p)
		return NULL;
	return p->health > 0 && p->getBoundingBox ? p : NULL;
}

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static float ray_box_dist2(const vec3 o, const vec3 d, float s,
						   const struct AABB* b, vec3 on_box) {
	vec3 q = {o[0] + d[0] * s, o[1] + d[1] * s, o[2] + d[2] * s};
	on_box[0] = clampf(q[0], b->x1, b->x2);
	on_box[1] = clampf(q[1], b->y1, b->y2);
	on_box[2] = clampf(q[2], b->z1, b->z2);
	return glm_vec3_distance2(q, on_box);
}

/* The hitbox point nearest the look ray over [0, reach]. Among equally near
 * points the search keeps the one nearest the eye (the larger, conservative
 * angle). */
static void nearest_box_point(const vec3 o, const vec3 d, float reach,
							  const struct AABB* b, vec3 out) {
	float lo = 0.0F, hi = reach;
	vec3 tmp;
	for(int i = 0; i < ATTACK_ASSIST_SEARCH_ITERS; i++) {
		float m1 = lo + (hi - lo) / 3.0F;
		float m2 = hi - (hi - lo) / 3.0F;
		if(ray_box_dist2(o, d, m1, b, tmp) <= ray_box_dist2(o, d, m2, b, tmp))
			hi = m2;
		else
			lo = m1;
	}
	ray_box_dist2(o, d, lo, b, out);
}

static float inset_axis(float v, float lo, float hi) {
	float half = (hi - lo) * 0.5F;
	float inset = half < ATTACK_ASSIST_AIM_INSET ? half : ATTACK_ASSIST_AIM_INSET;
	return clampf(v, lo + inset, hi - inset);
}

struct assist_candidate {
	const struct entity* e;
	int bucket;
	float dist;
};

static bool candidate_better(const struct assist_candidate* a,
							 const struct assist_candidate* b) {
	if(!b->e)
		return true;
	if(a->bucket != b->bucket)
		return a->bucket < b->bucket;
	if(a->dist != b->dist)
		return a->dist < b->dist;
	return a->e->id < b->e->id;
}

/* Is the eye -> aim segment free of blocks, and is no other ray-targetable
 * entity hit before `target` along the corrected ray? */
static bool assist_line_clear(const struct attack_pick_input* in,
							  const struct entity* target, const vec3 o,
							  const vec3 aim, const vec3 dir, float t_target) {
	struct camera_ray_result blk = {0};
	camera_ray_walk(o[0], o[1], o[2], aim[0], aim[1], aim[2], in->cell,
					in->cell_user, &blk);
	if(blk.hit)
		return false;
	dict_entity_it_t it;
	for(dict_entity_it(it, *in->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		const struct entity* e = dict_entity_cref(it)->value;
		if(e == target || attack_ray_skips(e, in->self))
			continue;
		float t;
		if(entity_aabb_intersect_ray(o, dir, e, &t) && t >= 0.0F
		   && t <= t_target)
			return false;
	}
	/* The split peer body occludes like any other ray-targetable entity. */
	const struct entity* peer = attack_live_peer(in);
	float tp;
	if(peer && peer != target && entity_aabb_intersect_ray(o, dir, peer, &tp)
	   && tp >= 0.0F && tp <= t_target)
		return false;
	return true;
}

static const struct entity* assist_find(const struct attack_pick_input* in,
										const vec3 o, const vec3 d,
										float half_angle_deg) {
	const float cos_limit = cosf(glm_rad(half_angle_deg));
	struct assist_candidate best = {0};
	dict_entity_it_t it;
	for(dict_entity_it(it, *in->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		const struct entity* e = dict_entity_cref(it)->value;
		if(attack_ray_skips(e, in->self) || !attack_assist_eligible(e))
			continue;
		struct AABB box;
		if(e->getBoundingBox(e, &box) == 0)
			continue;
		vec3 p, v;
		nearest_box_point(o, d, ATTACK_ENTITY_REACH, &box, p);
		glm_vec3_sub(p, (float*)o, v);
		float dist = glm_vec3_norm(v);
		if(!(dist > 1e-6F) || dist > ATTACK_ENTITY_REACH)
			continue;
		float cosang = glm_vec3_dot(v, (float*)d) / dist;
		if(cosang < cos_limit)
			continue;
		/* The corrected ray must really hit the unchanged hitbox in reach. */
		vec3 aim = {inset_axis(p[0], box.x1, box.x2),
					inset_axis(p[1], box.y1, box.y2),
					inset_axis(p[2], box.z1, box.z2)};
		vec3 dir;
		glm_vec3_sub(aim, (float*)o, dir);
		glm_vec3_normalize(dir);
		float t;
		if(!entity_aabb_intersect_ray(o, dir, e, &t) || t < 0.0F
		   || t > ATTACK_ENTITY_REACH)
			continue;
		if(!assist_line_clear(in, e, o, aim, dir, t))
			continue;
		float deg = glm_deg(acosf(clampf(cosang, -1.0F, 1.0F)));
		struct assist_candidate cand = {
			.e = e,
			.bucket = (int)floorf(deg / ATTACK_ASSIST_ANGLE_BUCKET_DEG),
			.dist = dist,
		};
		if(candidate_better(&cand, &best))
			best = cand;
	}
	return best.e;
}

void attack_pick(const struct attack_pick_input* in,
				 struct camera_ray_result* res) {
	assert(in && in->entities && in->camera && in->cell && res);
	const struct camera* c = in->camera;
	vec3 origin, dir;
	camera_get_ray(c, origin, dir);
	/* ---- Beta EntityRenderer.getMouseOver [b1.7.3 EntityRenderer.java:
	 * 80-131] ----
	 *  :83-84  the BLOCK ray first, Beta reach 4.0 (ATTACK_PICK_REACH);
	 *  :85-99  entity reach = min(block-hit distance, 3.0) (survival; the
	 *          32.0 branch is PlayerControllerTest, Beta's debug controller,
	 *          not a game mode -- our creative keeps the survival rule);
	 *  :105    every entity except the viewer (renderViewEntity);
	 *  :110    only canBeCollidedWith() ones (attack_pick_collidable);
	 *  :111-112 each box grown by getCollisionBorderSize (0.1, fireball 1.0);
	 *  :114-118 an eye inside a grown box picks it at distance 0;
	 *  :119-125 else the nearest clip within reach;
	 *  :129-131 a picked entity replaces the block result.
	 * The min(block, 3.0) clamp is applied as reach 3.0 plus "no block on the
	 * eye -> entity segment" (the same block walk assist_line_clear uses):
	 * the nearest candidate is behind the block iff the block hit is nearer. */
	res->assist_hit = false;
	res->assist_entity_id = 0;
	res->entity_hit = false;
	res->entity_id = 0;
	camera_ray_walk(c->x, c->y, c->z,
					c->x + sinf(c->rx) * sinf(c->ry) * ATTACK_PICK_REACH,
					c->y + cosf(c->ry) * ATTACK_PICK_REACH,
					c->z + cosf(c->rx) * sinf(c->ry) * ATTACK_PICK_REACH, in->cell,
					in->cell_user, res);
	const struct entity* peer = attack_live_peer(in);
	const struct entity* best = NULL;
	float best_t = 0.0F;
	dict_entity_it_t it;
	for(dict_entity_it(it, *in->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		const struct entity* e = dict_entity_cref(it)->value;
		if(!e || e == in->self || e->delay_destroy == 0)
			continue;
		if(e != peer && !attack_pick_collidable(e))
			continue;
		float t;
		if(!attack_pick_ray(e, origin, dir, ATTACK_ENTITY_REACH, &t))
			continue;
		if(!best || t < best_t) {
			best = e;
			best_t = t;
		}
	}
	if(best) {
		struct camera_ray_result blk = {0};
		camera_ray_walk(origin[0], origin[1], origin[2],
						origin[0] + dir[0] * best_t,
						origin[1] + dir[1] * best_t,
						origin[2] + dir[2] * best_t, in->cell, in->cell_user,
						&blk);
		if(!blk.hit) {
			res->entity_hit = true;
			res->entity_id = best->id;
			res->hit = false;
			return;
		}
	}
	/* ---- the assist: a complete miss only ---- */
	if(res->hit)
		return;
	float half_angle
		= attack_assist_half_angle_deg(in->assist_level, in->remote_session);
	if(half_angle <= 0.0F)
		return;
	const struct entity* target = assist_find(in, origin, dir, half_angle);
	if(target) {
		res->assist_hit = true;
		res->assist_entity_id = target->id;
	}
}

static bool attack_pick_world_cell(void* user, w_coord_t x, w_coord_t y,
								   w_coord_t z, struct ray* r, enum side* s) {
	return world_block_intersection((struct world*)user, r, x, y, z, s);
}

void attack_pick_world(dict_entity_t* entities, const struct entity* self,
					   const struct entity* peer,
					   const struct camera* camera, struct world* w,
					   int assist_level, bool remote_session,
					   struct camera_ray_result* res) {
	assert(w && res);
	attack_pick(&(struct attack_pick_input) {
					.entities = entities,
					.self = self,
					.camera = camera,
					.peer = peer,
					.cell = attack_pick_world_cell,
					.cell_user = w,
					.assist_level = assist_level,
					.remote_session = remote_session,
				},
				res);
}
