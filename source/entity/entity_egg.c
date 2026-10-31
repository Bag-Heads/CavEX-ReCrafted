/*
	Beta 1.7.3 EntityEgg (M4 S5) -- the thrown egg projectile, cloned from the
	arrow template (entity_arrow.c) MINUS stick/pickup: an egg dies on ANY hit.

	ALSO Beta 1.7.3 EntitySnowball (2026-08-07 caveat batch 1a): the decompiles
	[740c583: EntityEgg vs EntitySnowball] differ in EXACTLY one behavior --
	the egg's server-side chick roll on impact. Size (0.25F), heading (1.5F),
	drag (0.99F air / 0.8F water), gravity (0.03F), the 5-tick thrower grace,
	the expand(0.3F) closest-entity sweep, the zero-damage
	attackEntityFrom(thrower, 0), and the 8x "snowballpoof" death are
	byte-identical. So the snowball SHARES this file's tick: entity_snowball()
	constructs the same projectile with type ENTITY_SNOWBALL, and
	egg_impact_spawn gates the chick roll on the entity type (preserving the
	egg's RNG draw order exactly; the snowball consumes NO draws, like Beta).
	Both reuse the entity_egg data union member (owner + in-air age -- the
	fields are the same pair).

	Ground truth [740c583: EntityEgg]:
	  - size 0.25F cube; launch renormalized to 1.5F along the throw heading
	    (setEggHeading; the 0.0075-sigma gaussian spread is omitted, matching the
	    engine's bow which omits its Beta spread -- sub-degree cosmetic jitter).
	  - per tick: pos += motion; drag *0.99F (0.8F in water); gravity -= 0.03F.
	  - entity sweep (server side): candidate boxes expanded 0.3F, CLOSEST hit
	    wins, thrower immune while ticksinAir < 5. The hit entity takes
	    attackEntityFrom(thrower, 0) -- ZERO damage through the standard damage
	    path (hurt flash + knockback away from the thrower, no health change);
	    the result is IGNORED (an absorbed hit still kills the egg -- no
	    arrow-style bounce).
	  - on ANY hit (block or entity), server side: rand.nextInt(8)==0 -> 1
	    chicken, NESTED rand.nextInt(32)==0 -> 4 (net 1/256); FULL-GROWN
	    chickens (no babies in b1.7.3), spawned at the egg position. Then 8
	    "snowballpoof" particles + setEntityDead. v2 particle substitution: no
	    snowballpoof sprite exists, so ONE CRPC_SMOKE_PUFF (the nearest existing
	    render-thread particle helper) marks the impact -- documented cosmetic
	    stand-in, not simulation.
	  - Beta's onCollideWithPlayer arrow-pickup remnant is DEAD CODE (inGround is
	    never set in play) -- deliberately not ported.

	CLIENT: this type is server-only. The client's display counterpart is the
	CRPC_SPAWN_ARROW entity carrying item_id ITEM_EGG -- the arrow's
	interpolation tick + flight-oriented item billboard is exactly the thrown-egg
	render; CRPC_ENTITY_MOVE drives it and CRPC_ENTITY_DESTROY reaps it.

	REAPERS: no in-air cap; Entity.onEntityUpdate kills below Y -64 at tick start.
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

#define EGG_BBOX_SIZE 0.25f /* Beta EntityEgg setSize(0.25F, 0.25F) */

static struct AABB make_egg_bbox_ret(void) {
	struct AABB bbox;
	/* yOffset = 0, bottom-anchored like the arrow's box. */
	aabb_setsize_centered_offset(&bbox, EGG_BBOX_SIZE, EGG_BBOX_SIZE,
								 EGG_BBOX_SIZE, 0.0F, EGG_BBOX_SIZE * 0.5F,
								 0.0F);
	return bbox;
}

/* Slab ray/AABB entry test -- the arrow's (entity_arrow.c) duplicated at file
 * scope because that one is static there; identical math, pinned by the same
 * sweep behavior. */
static bool egg_aabb_intersect_ray(const vec3 origin, const vec3 dir,
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

static inline void egg_aabb_expand(struct AABB* b, float d) {
	b->x1 -= d; b->x2 += d;
	b->y1 -= d; b->y2 += d;
	b->z1 -= d; b->z2 += d;
}

/* Player body-box ray test without damage (the arrow's arrow_test_actor).
 * S1-a3: one NAMED body; egg_sweep_actors enumerates both. */
static bool egg_test_actor(struct server_local* s,
						   const struct server_player* player,
						   const vec3 origin, const vec3 dir, float max_dist,
						   float* out_t) {
	struct AABB bbox;
	float t_hit;

	(void)s;
	if(!player || !player->has_pos || !player->finished_loading)
		return false;
	/* No health gate -- see arrow_test_actor: a corpse stays a candidate and
	 * the HIT is refused downstream, one rule for both bodies. */

	aabb_setsize_centered(&bbox, 0.6f, 1.8f, 0.6f);
	aabb_translate(&bbox, (float)player->x,
				   (float)player->y + 1.8f / 2.0f - PLAYER_EYE_HEIGHT,
				   (float)player->z);
	egg_aabb_expand(&bbox, ARROW_HIT_EXPAND); /* Beta expand(0.3F) */

	if(!egg_aabb_intersect_ray(origin, dir, &bbox, &t_hit) || t_hit < 0.0f
	   || t_hit > max_dist)
		return false;
	if(out_t)
		*out_t = t_hit;
	return true;
}

/* S1-a3: the arrow sweep, for the thrown egg/snowball. Beta applies the very
 * same exclusion to EntityThrowable [cross-checked in ../mc173
 * mc173/src/entity/tick.rs:381-399: owner_id is filtered out of the collision
 * candidates only while state_time < 5, identically for arrow, snowball and
 * egg], so the grace is a per-candidate identity compare here too. */
static struct server_player* egg_sweep_actors(struct server_local* s,
	const struct entity* e, const vec3 origin, const vec3 dir, float max_dist,
	float* out_t) {
	struct server_player* hit = NULL;
	float best_t = max_dist + 1.0f;
	/* Beta EntityEgg/EntitySnowball.java:110 queries this.worldObj. */
	struct server_world* world = server_local_entity_world(s, e);
	if(!world)
		return NULL;
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* cand = server_local_damage_candidate(s, i);
		if(!cand || !server_local_actor_in_world(s, cand, world))
			continue;
		if(!arrow_should_hit_actor(
			   projectile_shooter_is_this_body(
				   e->data.egg.owner_id,
				   actor_ident_equal(e->data.egg.thrower_actor,
									 server_local_actor_ident(s, cand))),
			   e->data.egg.age))
			continue;
		float t = max_dist + 1.0f;
		if(!egg_test_actor(s, cand, origin, dir, max_dist, &t))
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

/* Closest-mob sweep along the sub-step (the arrow's arrow_sweep_mobs minus the
 * membership tie-break: the egg deals no damage, so an exact-tie winner is not
 * loot-attribution-sensitive; first-found at equal t is kept). */
static struct entity* egg_sweep_mobs(struct server_local* s,
									 struct entity* egg, const vec3 origin,
									 const vec3 dir, float max_dist,
									 float* out_t) {
	struct entity* closest = NULL;
	float closest_t = max_dist;
	/* Beta EntityEgg/EntitySnowball.java:110 queries this.worldObj. */
	struct server_world* world = server_local_entity_world(s, egg);
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
		if(cand->id == egg->id)
			continue;
		/* Thrower immune only while young (Beta ticksinAir < 5). */
		if(!arrow_should_hit(egg->data.egg.owner_id, cand->id,
							 egg->data.egg.age))
			continue;

		struct AABB mbox;
		float t_hit;
		if(cand->getBoundingBox && cand->getBoundingBox(cand, &mbox) > 0) {
			egg_aabb_expand(&mbox, ARROW_HIT_EXPAND);
			if(egg_aabb_intersect_ray(origin, dir, &mbox, &t_hit)
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

/* Shared terminal impact: the server-side Beta chick roll (nextInt(8)==0 -> 1,
 * nested nextInt(32)==0 -> 4, adults) + the impact puff. Chickens go through
 * the pending-monster queue (this runs DURING the entity pass -- the slime
 * split precedent) at the egg's exact position. The chick roll is the EGG's
 * alone [740c583: EntitySnowball.onUpdate has no spawn branch]: a snowball
 * impact consumes zero RNG draws and only puffs + dies. */
static void egg_impact_spawn(struct entity* e, struct server_local* s) {
	if(e->type == ENTITY_EGG) {
		/* CFR EntityEgg.java:153-155: the egg's own Random. */
		int roll8 = jrand_next_int_bound(&e->gameplay_rng, EGG_CHICK_DIE);
		int count = 0;
		if(roll8 == 0) {
			/* NESTED draw, only consumed when the 1/8 hit (Beta draw order). */
			int roll32 = jrand_next_int_bound(&e->gameplay_rng, EGG_CHICK_QUAD_DIE);
			count = egg_chick_count(roll8, roll32);
		}
		for(int i = 0; i < count; i++)
			server_local_queue_monster_w(
				s, server_local_entity_world(s, e),
				(vec3) {e->pos[0], e->pos[1], e->pos[2]}, MONSTER_CHICKEN);
	}
	/* Beta: 8x "snowballpoof". v2: one render-thread smoke puff (documented
	 * cosmetic substitution -- see the file header). */
	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_SMOKE_PUFF,
		.payload.smoke_puff.pos = {e->pos[0], e->pos[1], e->pos[2]},
		.payload.smoke_puff.intensity = 1.0f,
	});
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
	struct AABB bbox = make_egg_bbox_ret();
	aabb_translate(&bbox, 0.0F, e->y_size, 0.0F);
	vec3 planned_travel, travel, dir;

	assert(e && s);

	glm_vec3_copy(e->pos, e->pos_old);
	if(e->pos[1] < -64.0F)
		return true; // Entity.onEntityUpdate:222-224, before egg motion
	glm_vec3_copy(e->vel, planned_travel);

	if(e->data.egg.age < INT32_MAX)
		e->data.egg.age++;

	/* Substepped move + sweep (the arrow's anti-tunneling fix): at 1.5 launch
	 * speed a single-tick move can skip a thin wall the Beta swept ray would
	 * hit. Integration (drag/gravity) still runs ONCE per tick, after. */
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

		/* --- ENTITY hit: closest of (player, mobs) along this sub-step ----- */
		if(dist2 > glm_pow2(0.0001f)) {
			float dist = sqrtf(dist2);
			dir[0] = travel[0] / dist;
			dir[1] = travel[1] / dist;
			dir[2] = travel[2] / dist;

			float t_player = dist + 1.0f, t_mob = dist + 1.0f;
			/* S1-a3: both local bodies are candidates, each with its own
			 * thrower grace (egg_sweep_actors carries the citations). This is
			 * what lets a GUEST snowball land on the HOST after the grace, and
			 * a HOST snowball land on the guest. */
			struct server_player* hit_actor
				= egg_sweep_actors(s, e, sub_origin, dir, dist, &t_player);
			bool hit_player = hit_actor != NULL;
			struct entity* mob
				= egg_sweep_mobs(s, e, sub_origin, dir, dist, &t_mob);

			if(hit_player && (!mob || t_player <= t_mob)) {
				/* attackEntityFrom(thrower, 0) on the player. Routed through
				 * the standard player damage path. Accuracy note (review F2):
				 * a FRESH 0-hit DOES open the 10-tick resist window and sets
				 * last_damage=0 (player_iframe_damage mutates before its
				 * apply<=0 return) -- exactly Beta's fresh-window 0-hit
				 * arithmetic, so an in-window follow-up still deals its full
				 * excess on both sides. Only the PRESENTATION of the 0-hit
				 * (hurt animation + recoil) is omitted, in the rare
				 * own-egg-falls-back-on-thrower case. The egg still dies with
				 * the full impact roll. */
				(void)server_local_damage_actor_result(s, hit_actor, 0, dir[0],
													   dir[2], NULL);
				egg_impact_spawn(e, s);
				return true;
			}
			if(mob) {
				/* Zero damage through the standard mob damage path: a FRESH
				 * window arms hurtTime (red flash) with health -= 0; Beta then
				 * applies the generic attackEntityFrom knockback AWAY from the
				 * attacker (the thrower) -- the melee handler's convention. An
				 * ABSORBED in-window hit changes nothing -- and unlike the
				 * arrow there is NO bounce: the egg dies either way (Beta
				 * ignores attackEntityFrom's result). */
				// PIG ZOMBIE PACK ANGER (PIGZ-PACK) -- same citation as
				// entity_arrow.c's arrow_damage_mob: EntityEgg/EntitySnowball.
				// onUpdate call entityHit.attackEntityFrom(this.<thrower>, 0)
				// [server EntityEgg.java:135, EntitySnowball.java:135], so the
				// pack alert fires iff the thrower is an EntityPlayer. A
				// dispensed egg/snowball has a null thrower (owner id
				// DISPENSER_PROJECTILE_OWNER_SENTINEL here) and angers nobody.
				// BEFORE the damage call: ahead of the i-frame/health gate and
				// of this mob's hurt pitch [EntityLiving.java:293-345].
				if(mob->data.monster.id == MONSTER_PIG_ZOMBIE
				   && e->data.egg.owner_id == PROJECTILE_OWNER_LOCAL_PLAYER)
					server_local_pig_zombie_pack_anger(
						s, mob, e->data.egg.thrower_actor);
				enum monster_damage_result result
					= entity_monster_apply_damage(mob, 0, false);
				if(result == MONSTER_DAMAGE_FRESH) {
					/* S1-a2: knockback points away from the THROWER, and the
					 * retaliation latch names the thrower. Beta passes
					 * `this.owner` as the damage source [740c583
					 * EntityThrowable/EntityEgg.onUpdate ->
					 * attackEntityFrom(this.owner, 0)], so both were wrong for
					 * a guest throw (shoved away from the host, chased the
					 * host). A thrower whose session ended falls back to the
					 * host, which is the pre-S1 behaviour and the only actor
					 * that provably still exists. */
					struct server_player* thrower
						= server_local_actor_resolve(
							s, e->data.egg.thrower_actor);
					if(!thrower)
						thrower = &s->player;
					/* Mob motion units S2 (Q3): attackEntityFrom(thrower, 0)
					 * -> EntityLiving.knockBack from the thrower, composed at
					 * the hit on the authoritative doubles. */
					mob_motion_knockback(mob, &mob->data.monster.motion_d,
						thrower->x - mob->data.monster.motion_d.pos[0],
						thrower->z - mob->data.monster.motion_d.pos[2],
						server_local_sound_rng());
					mob->data.monster.knockback_time = MOB_KNOCKBACK_TICKS;
					/* Beta EntityMob.attackEntityFrom retaliates on any
					 * true-returning hit, 0-damage included; passives never
					 * do (the arrow-path precedent). */
					if(!monster_is_passive(mob->data.monster.id)) {
						mob->data.monster.aggro_time
							= MOB_TARGET_RETENTION_TICKS;
						mob->data.monster.target_actor
							= e->data.egg.thrower_actor;
					}
				}
				egg_impact_spawn(e, s);
				return true;
			}
		}

		/* --- BLOCK hit: the move was cut short -> impact + die (no stick) -- */
		if(sub_collision || e->on_ground
		   || fabsf(travel[0] - sub_move[0]) > 0.001f
		   || fabsf(travel[1] - sub_move[1]) > 0.001f
		   || fabsf(travel[2] - sub_move[2]) > 0.001f) {
			egg_impact_spawn(e, s);
			return true;
		}
	}

	/* Beta order: drag first, THEN gravity (motionY = motionY*drag - 0.03).
	 * Water selects the 0.8 drag; the foot-cell probe is the engine's standard
	 * body_in_water approximation of Beta's bbox isInWater (the mob-tick
	 * precedent). [740c583: EntityEgg.onUpdate tail] */
	float drag = EGG_AIR_DRAG;
	{
		struct block_data foot;
		if(server_world_get_block(block_world(s), (w_coord_t)floorf(e->pos[0]),
								  (w_coord_t)floorf(e->pos[1]),
								  (w_coord_t)floorf(e->pos[2]), &foot)
		   && (foot.type == BLOCK_WATER_FLOW || foot.type == BLOCK_WATER_STILL))
			drag = EGG_WATER_DRAG;
	}
	e->vel[0] *= drag;
	e->vel[1] *= drag;
	e->vel[2] *= drag;
	e->vel[1] -= EGG_GRAVITY;

	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_ENTITY_MOVE,
		.payload.entity_move.entity_id = e->id,
		.payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]}
	});

	return false;
}

static size_t getBoundingBox_egg(const struct entity* e, struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

/* Shared thrown-projectile construction: the egg and the snowball are the
 * same entity except for type/name (and the type-gated chick roll above). */
static void entity_thrown_init(uint32_t id, struct entity* e, bool server,
							   void* world, uint32_t owner_id,
							   enum entity_type type, const char* name) {
	assert(e && world);

	entity_default_init(e, server, world);
	entity_set_size(e, EGG_BBOX_SIZE, EGG_BBOX_SIZE, 0.0F);
	e->id = id;
	if(server) {
		struct server_world* w = world;
		entity_lifetime_rng_seed(&e->gameplay_rng, w->seed, w->dimension, id);
	}
	e->type = type;
	e->name = name;
	e->tick_server = entity_server_tick;
	/* Server-only type: the client's display entity is a CRPC_SPAWN_ARROW
	 * item billboard (see the file header), so these client hooks are inert
	 * defaults. */
	e->tick_client = entity_default_client_tick;
	e->render = NULL;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = getBoundingBox_egg;
	e->onLeftClick = NULL;
	e->leftClickText = NULL;
	e->onRightClick = NULL;
	e->rightClickText = NULL;
	e->delay_destroy = -1;
	e->data.egg.owner_id = owner_id;
	/* S1-a2: stamped by the throw site; host zero value is the default. */
	e->data.egg.thrower_actor = ACTOR_IDENT_HOST;
	e->data.egg.age = 0;
	glm_vec3_copy(e->pos, e->network_pos);
}

void entity_egg(uint32_t id, struct entity* e, bool server, void* world,
				uint32_t owner_id) {
	entity_thrown_init(id, e, server, world, owner_id, ENTITY_EGG, "Egg");
}

/* Beta 1.7.3 EntitySnowball [740c583]: identical projectile, no impact spawn
 * (the type gate in egg_impact_spawn), persisted under NBT id "Snowball". */
void entity_snowball(uint32_t id, struct entity* e, bool server, void* world,
					 uint32_t owner_id) {
	entity_thrown_init(id, e, server, world, owner_id, ENTITY_SNOWBALL,
					   "Snowball");
}
