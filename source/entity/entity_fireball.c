/*
	Beta 1.7.3 EntityFireball (batch 1b) -- the ghast's shot, cloned from the
	thrown-projectile template (entity_egg.c, itself cloned from the arrow)
	with the fireball's own physics + payload.

	Ground truth [740c583: EntityFireball]:
	  - size 1.0F cube; motion starts 0; acceleration = dir/|dir| * 0.1 added
	    to the motion EVERY tick, then drag *0.95F (0.8F in water). NO gravity.
	    (Beta's 0.4-sigma gaussian launch jitter is omitted -- the same
	    documented sub-degree cosmetic simplification the bow/egg carry.)
	  - shooter immune while ticksInAir < 25 (vs the egg's 5).
	  - on ANY hit (block or entity): the struck entity takes
	    attackEntityFrom(shooter, 0) -- ZERO direct damage -- then
	    worldObj.newExplosion(null, x, y, z, 1.0F, TRUE): the strength-1.0
	    FLAMING explosion is the entire payload. Then setEntityDead.
	  - punch-deflection (attackEntityFrom): motion = attacker's look vector,
	    accel = look * 0.1 -- entity_fireball_deflect, called from the
	    SRPC_ENTITY_ATTACK dispatch.
	  - Beta persists only tile/shake/inGround state -- the ACCELERATION is
	    lost on reload, leaving a drifting dud. This port persists the egg's
	    owner/age pair and likewise drops accel: the same reload behavior.
	  - fire = 10 every tick (self-ignite visual): the marked client
	    CRPC_SPAWN_ARROW display uses Beta's snowball icon on a 2-block,
	    camera-facing billboard with a display-only burn overlay and 256-block
	    entity render range. Server fire/physics state remains unchanged.

	REAPERS: no in-air cap; Entity.onEntityUpdate kills below Y -64 at tick start.
	Beta's inGround 1200-tick rot is unreachable (contact explodes).
*/

#include <assert.h>
#include <math.h>

#include "../block/aabb.h"
#include "../block/blocks.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../util.h"
#include "combat.h"
#include "entity.h"
#include "entity_monster.h"

#define FIREBALL_BBOX_SIZE 1.0f /* Beta EntityFireball setSize(1.0F, 1.0F) */
#define FIREBALL_OWNER_GRACE_TICKS 25 /* Beta `ticksInAir >= 25` */
#define FIREBALL_AIR_DRAG 0.95f
#define FIREBALL_WATER_DRAG 0.8f
#define FIREBALL_POWER 1.0f /* Beta newExplosion(..., 1.0F, true) */

static struct AABB make_fireball_bbox_ret(void) {
	struct AABB bbox;
	aabb_setsize_centered_offset(&bbox, FIREBALL_BBOX_SIZE, FIREBALL_BBOX_SIZE,
								 FIREBALL_BBOX_SIZE, 0.0F,
								 FIREBALL_BBOX_SIZE * 0.5F, 0.0F);
	return bbox;
}

/* Slab ray/AABB entry test -- the arrow's, duplicated at file scope per the
 * established template idiom (static in entity_arrow.c/entity_egg.c). */
static bool fb_aabb_intersect_ray(const vec3 origin, const vec3 dir,
								  const struct AABB* box, float* out_t) {
	float inv_dx = 1.0f
		/ (fabsf(dir[0]) > GLM_FLT_EPSILON ? dir[0]
										   : copysignf(GLM_FLT_EPSILON, dir[0]));
	float inv_dy = 1.0f
		/ (fabsf(dir[1]) > GLM_FLT_EPSILON ? dir[1]
										   : copysignf(GLM_FLT_EPSILON, dir[1]));
	float inv_dz = 1.0f
		/ (fabsf(dir[2]) > GLM_FLT_EPSILON ? dir[2]
										   : copysignf(GLM_FLT_EPSILON, dir[2]));

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

static inline void fb_aabb_expand(struct AABB* b, float d) {
	b->x1 -= d; b->x2 += d;
	b->y1 -= d; b->y2 += d;
	b->z1 -= d; b->z2 += d;
}

static bool fb_test_actor(const struct server_player* player, const vec3 origin,
						  const vec3 dir, float max_dist, float* out_t) {
	struct AABB bbox;
	float t_hit;

	if(!player || !player->has_pos || !player->finished_loading)
		return false;

	aabb_setsize_centered(&bbox, 0.6f, 1.8f, 0.6f);
	aabb_translate(&bbox, (float)player->x,
				   (float)player->y + 1.8f / 2.0f - PLAYER_EYE_HEIGHT,
				   (float)player->z);
	fb_aabb_expand(&bbox, ARROW_HIT_EXPAND); /* Beta expand(0.3F) */

	if(!fb_aabb_intersect_ray(origin, dir, &bbox, &t_hit) || t_hit < 0.0f
	   || t_hit > max_dist)
		return false;
	if(out_t)
		*out_t = t_hit;
	return true;
}

/* recovery-0916/01: the NEAREST local player body on this sub-step, not just
 * the host. Same enumeration and same fixed order (index 0 = host, index 1 =
 * the admitted guest or NULL) as the arrow/egg/fish sweeps
 * (egg_sweep_actors), so with the split switch off index 1 is NULL and this
 * is byte-for-byte the single s->player test it replaces. Beta's own
 * EntityFireball picks the CLOSEST intersected entity, so a tie or a nearer
 * host keeps the host: `t < best_t` is strict. */
static struct server_player* fb_sweep_actors(struct server_local* s,
											 const struct entity* fb,
											 const vec3 origin, const vec3 dir,
											 float max_dist, float* out_t) {
	struct server_player* hit = NULL;
	float best_t = max_dist + 1.0f;
	/* Beta EntityFireball.java:82 queries this.worldObj for every victim. */
	struct server_world* world = server_local_entity_world(s, fb);
	if(!world)
		return NULL;
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* cand = server_local_damage_candidate(s, i);
		float t = max_dist + 1.0f;
		if(!cand || !server_local_actor_in_world(s, cand, world))
			continue;
		if(!fb_test_actor(cand, origin, dir, max_dist, &t))
			continue;
		if(!hit || t < best_t) {
			hit = cand;
			best_t = t;
		}
	}
	if(hit && out_t)
		*out_t = best_t;
	return hit;
}

/* Closest-mob sweep -- the egg's, with the fireball's shooter-grace window.
 * The shooting ghast itself is excluded while young. */
static struct entity* fb_sweep_mobs(struct server_local* s,
									struct entity* fb, const vec3 origin,
									const vec3 dir, float max_dist,
									float* out_t) {
	struct entity* closest = NULL;
	float closest_t = max_dist;
	/* Beta EntityFireball.java:82 queries this.worldObj. */
	struct server_world* world = server_local_entity_world(s, fb);
	if(!world)
		return NULL;

	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* cand = dict_entity_ref(it)->value;
		dict_entity_next(it);

		if(!cand || cand->type != ENTITY_MONSTER || cand->delay_destroy == 0
		   || cand->data.monster.explosion_pending
		   || server_local_entity_world(s, cand) != world)
			continue;
		if(cand->id == fb->id)
			continue;
		if(cand->id == fb->data.fireball.owner_id
		   && fb->data.fireball.age < FIREBALL_OWNER_GRACE_TICKS)
			continue; /* Beta `var9 != shootingEntity || ticksInAir >= 25` */

		struct AABB mbox;
		float t_hit;
		if(cand->getBoundingBox && cand->getBoundingBox(cand, &mbox) > 0) {
			fb_aabb_expand(&mbox, ARROW_HIT_EXPAND);
			if(fb_aabb_intersect_ray(origin, dir, &mbox, &t_hit)
			   && t_hit >= 0.0f && t_hit < closest_t) {
				closest_t = t_hit;
				closest = cand;
			}
		}
	}

	if(closest && out_t)
		*out_t = closest_t;
	return closest;
}

/* Terminal impact: the strength-1.0 FLAMING explosion at the fireball, then
 * die. The zero-damage entity hit (when one was struck) happens at the call
 * sites BEFORE this, mirroring Beta's statement order. */
static void fb_impact(struct entity* e, struct server_local* s) {
	server_world_explode_flaming(
		(struct server_world*)e->world, (vec3) {e->pos[0], e->pos[1], e->pos[2]}, FIREBALL_POWER);
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
	struct AABB bbox = make_fireball_bbox_ret();
	aabb_translate(&bbox, 0.0F, e->y_size, 0.0F);
	vec3 planned_travel, travel, dir;

	assert(e && s);

	glm_vec3_copy(e->pos, e->pos_old);
	if(e->pos[1] < -64.0F)
		return true; // Entity.onEntityUpdate:222-224, before fireball motion

	/* Beta order: pos += motion FIRST, then accel, then drag (onUpdate tail).
	 * The sub-stepped sweep below integrates the position; accel+drag follow
	 * after, exactly once per tick. */
	glm_vec3_copy(e->vel, planned_travel);

	if(e->data.fireball.age < INT32_MAX)
		e->data.fireball.age++;

	float plan_len = sqrtf(planned_travel[0] * planned_travel[0]
						   + planned_travel[1] * planned_travel[1]
						   + planned_travel[2] * planned_travel[2]);
	int substeps = arrow_move_substeps(plan_len);
	vec3 step_vel = {planned_travel[0] / substeps, planned_travel[1] / substeps,
					 planned_travel[2] / substeps};

	for(int sstep = 0; sstep < substeps; sstep++) {
		vec3 sub_origin, sub_move;
		glm_vec3_copy(e->pos, sub_origin);
		glm_vec3_copy(step_vel, sub_move);

		bool sub_collision = false;
		entity_try_move(e, e->pos, sub_move, &bbox, 1, &sub_collision,
						&e->on_ground);
		entity_try_move(e, e->pos, sub_move, &bbox, 0, &sub_collision,
						&e->on_ground);
		entity_try_move(e, e->pos, sub_move, &bbox, 2, &sub_collision,
						&e->on_ground);

		travel[0] = e->pos[0] - sub_origin[0];
		travel[1] = e->pos[1] - sub_origin[1];
		travel[2] = e->pos[2] - sub_origin[2];
		float dist2 = travel[0] * travel[0] + travel[1] * travel[1]
			+ travel[2] * travel[2];

		if(dist2 > glm_pow2(0.0001f)) {
			float dist = sqrtf(dist2);
			dir[0] = travel[0] / dist;
			dir[1] = travel[1] / dist;
			dir[2] = travel[2] / dist;

			float t_player = dist + 1.0f, t_mob = dist + 1.0f;
			struct server_player* hit_player
				= fb_sweep_actors(s, e, sub_origin, dir, dist, &t_player);
			struct entity* mob
				= fb_sweep_mobs(s, e, sub_origin, dir, dist, &t_mob);

			if(hit_player && (!mob || t_player <= t_mob)) {
				/* attackEntityFrom(shooter, 0): the fresh-window zero hit
				 * (the egg's accuracy note applies); the EXPLOSION is the
				 * real damage. Billed to the body the sweep actually struck
				 * -- with no guest admitted that is &s->player, so this is
				 * the server_local_damage_player_result call it replaces. */
				(void)server_local_damage_actor_result(s, hit_player, 0,
													   dir[0], dir[2], NULL);
				fb_impact(e, s);
				return true;
			}
			if(mob) {
				/* Zero direct damage through the standard path (hurt flash
				 * only), then the explosion does the rest. No retaliation
				 * special-casing: the explosion's damage attribution covers
				 * it, as in Beta (the fireball's attacker is the ghast). */
				(void)entity_monster_apply_damage(mob, 0, false);
				fb_impact(e, s);
				return true;
			}
		}

		if(sub_collision || e->on_ground
		   || fabsf(travel[0] - sub_move[0]) > 0.001f
		   || fabsf(travel[1] - sub_move[1]) > 0.001f
		   || fabsf(travel[2] - sub_move[2]) > 0.001f) {
			fb_impact(e, s);
			return true;
		}
	}

	/* Beta tail: motion += acceleration, then drag. NO gravity. Water picks
	 * the 0.8 drag (foot-cell probe, the egg's convention). */
	float drag = FIREBALL_AIR_DRAG;
	{
		struct block_data foot;
		if(server_world_get_block(block_world(s), (w_coord_t)floorf(e->pos[0]),
								  (w_coord_t)floorf(e->pos[1]),
								  (w_coord_t)floorf(e->pos[2]), &foot)
		   && (foot.type == BLOCK_WATER_FLOW || foot.type == BLOCK_WATER_STILL))
			drag = FIREBALL_WATER_DRAG;
	}
	e->vel[0] = (e->vel[0] + e->data.fireball.accel[0]) * drag;
	e->vel[1] = (e->vel[1] + e->data.fireball.accel[1]) * drag;
	e->vel[2] = (e->vel[2] + e->data.fireball.accel[2]) * drag;

	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_ENTITY_MOVE,
		.payload.entity_move.entity_id = e->id,
		.payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]}
	});

	return false;
}

static size_t getBoundingBox_fireball(const struct entity* e,
									  struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

void entity_fireball(uint32_t id, struct entity* e, bool server, void* world,
					 uint32_t owner_id, const float dir[3]) {
	assert(e && world && dir);

	entity_default_init(e, server, world);
	entity_set_size(e, FIREBALL_BBOX_SIZE, FIREBALL_BBOX_SIZE, 0.0F);
	e->id = id;
	e->type = ENTITY_FIREBALL;
	e->name = "Fireball";
	e->tick_server = entity_server_tick;
	e->tick_client = entity_default_client_tick;
	e->render = NULL;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = getBoundingBox_fireball;
	e->onLeftClick = NULL;
	e->leftClickText = NULL;
	e->onRightClick = NULL;
	e->rightClickText = NULL;
	e->delay_destroy = -1;
	e->data.fireball.owner_id = owner_id;
	e->data.fireball.age = 0;
	float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if(n < 1.0e-6f)
		n = 1.0f;
	e->data.fireball.accel[0] = dir[0] / n * 0.1f;
	e->data.fireball.accel[1] = dir[1] / n * 0.1f;
	e->data.fireball.accel[2] = dir[2] / n * 0.1f;
	glm_vec3_copy(e->pos, e->network_pos);
}

void entity_fireball_deflect(struct entity* e, const float look[3]) {
	assert(e && e->type == ENTITY_FIREBALL && look);
	e->vel[0] = look[0];
	e->vel[1] = look[1];
	e->vel[2] = look[2];
	e->data.fireball.accel[0] = look[0] * 0.1f;
	e->data.fireball.accel[1] = look[1] * 0.1f;
	e->data.fireball.accel[2] = look[2] * 0.1f;
	/* Beta resets no timers on deflection; the 25-tick shooter grace has long
	 * expired by the time a player can punch it. */
}
