/*
	Beta 1.7.3 wolf state/interaction/retaliation helpers.
	Ground truth: [740c583: EntityWolf]. Locomotion remains in entity_monster.c
	beside the shared direct-steer chassis; the exact teleport scan lives here.
*/
#include "entity_wolf.h"
#include "combat.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "../block/blocks.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../platform/audio.h"
#include "../util.h"

bool entity_is_wolf(const struct entity* e) {
	return e && e->type == ENTITY_MONSTER
		&& e->data.monster.id == MONSTER_WOLF;
}

static bool ascii_equal_ignore_case(const char* a, const char* b) {
	if(!a || !b)
		return false;
	while(*a && *b) {
		if(tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
			return false;
	}
	return *a == *b;
}

bool entity_wolf_owner_is_local(const struct entity* e) {
	return entity_is_wolf(e)
		&& wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_TAMED)
		&& ascii_equal_ignore_case(e->data.monster.wolf_owner,
								 WOLF_LOCAL_PLAYER_NAME);
}

/* CaveWii persists one Player and one CaveWiiGuest record per world, without
 * a username registry. Keep the historical host name byte-for-byte and give
 * the guest record a distinct stable name; never persist a runtime wire id or
 * admission generation as Beta's Owner string. */
static const char* wolf_actor_name(struct server_local* s,
	const struct server_player* actor) {
	if(actor == &s->player)
		return WOLF_LOCAL_PLAYER_NAME;
	if(actor && actor == s->guest_actor && server_local_actor_resolve(s,
		server_local_actor_ident(s, actor)) == actor)
		return WOLF_LOCAL_GUEST_NAME;
	return NULL;
}

bool entity_wolf_owner_is_actor(const struct entity* e, struct server_local* s,
	const struct server_player* actor) {
	return s && entity_is_wolf(e)
		&& wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_TAMED)
		&& ascii_equal_ignore_case(e->data.monster.wolf_owner,
			wolf_actor_name(s, actor));
}

struct server_player* entity_wolf_owner_actor(const struct entity* e,
	struct server_local* s) {
	if(!s)
		return NULL;
	struct server_player* owner = entity_wolf_owner_is_actor(e, s, &s->player)
		? &s->player : entity_wolf_owner_is_actor(e, s, s->guest_actor)
		? s->guest_actor : NULL;
	/* EntityWolf.java:85: this.worldObj.getPlayerEntityByName(getOwner()).
	 * Guest ownership always resolves in the validated server world, independent
	 * of client presentation. Only an unstamped host in a standalone fixture
	 * keeps the legacy SP route (no live server world to resolve). */
	if(owner && (owner != &s->player || e->world_generation != 0
		|| server_local_world_live(&s->worlds[0]))
	   && !server_local_actor_in_world(s, owner, server_local_entity_world(s, e)))
		return NULL;
	return owner;
}

uint8_t wolf_restored_flags(bool angry, bool sitting, const char* owner) {
	uint8_t flags = 0;
	wolf_flag_set(&flags, WOLF_FLAG_ANGRY, angry);
	wolf_flag_set(&flags, WOLF_FLAG_SITTING, sitting);
	wolf_flag_set(&flags, WOLF_FLAG_TAMED, owner && owner[0] != '\0');
	return flags;
}

void wolf_update_automatic_sit(uint8_t* flags, bool owner_present,
	bool in_water, bool has_retaliation_target) {
	if(!flags)
		return;
	if(in_water)
		wolf_flag_set(flags, WOLF_FLAG_SITTING, false);
	else if(wolf_flag_get(*flags, WOLF_FLAG_TAMED) && !owner_present
			&& !has_retaliation_target)
		wolf_flag_set(flags, WOLF_FLAG_SITTING, true);
}

// Ambient look-at-nearby-player latch [740c583: EntityLiving.
// hasCurrentTarget/getCurrentTarget/currentTarget's 2%-roll/8-block/
// 10+rand.nextInt(20)-hold idle latch, reached via EntityCreature's "no
// active path this tick" fallback: `pathToEntity==null || rand.nextInt(100)
// ==0` in updatePlayerActionState]. Scoped to the wolf because only the
// looksWithInterest gate below (wolf_looks_with_interest) consumes it in
// this port -- the SAME base-class idle behaviour every mob inherits, not
// promoted to a shared mob field.
//
// hasCurrentTarget()/getCurrentTarget() is UNRELATED to wolf_target_id
// (EntityCreature.playerToAttack: the retaliation/sheep-chase/owner-follow
// target) -- Beta keeps the two fields entirely separate, and this latch
// runs REGARDLESS of tamed/angry/sitting state (EntityWolf.
// updatePlayerActionState calls super.updatePlayerActionState()
// unconditionally, first); the `!angry` restriction only applies at
// wolf_looks_with_interest, not here.
//
// Gated on !wolf_has_path (the pathToEntity!=null boolean test): a wolf
// mid-path-follow this tick does not reach the ambient latch at all in
// Beta. The remaining Beta nuance -- pathToEntity!=null but the 1-in-100
// skip roll ALSO reaches the ambient latch that tick -- is not reproduced
// here (it would require sharing a single roll with the caller's own,
// already-drawn, path-follow-tail roll); the residual gap is a <1%-per-tick
// chance of an extra/missing latch evaluation while actively
// path-following, bounded by this latch's own 10-30 tick hold and the 0.4
// exponential head-turn smoothing wolf_looks_with_interest feeds, so it is
// not player-perceptible. [740c583: EntityCreature.updatePlayerActionState]
//
// player_dist_sq is the caller's already-resolved 3D distance-squared to
// the local player (Beta's getClosestPlayerToEntity uses the same feet-to-
// feet 3D basis the rest of this port's mob gates share). Draws from
// rand_src exactly like every other wolf/mob RNG use in this file.
void wolf_update_current_target_latch(struct entity* wolf,
	struct java_random* rand_src, bool wolf_has_path, bool player_has_pos,
	float player_dist_sq) {
	if(!entity_is_wolf(wolf) || !rand_src || wolf_has_path)
		return;
	if(wolf->data.monster.wolf_has_current_target) {
		// [740c583: numTicksToChaseTarget--<=0 || currentTarget.isDead ||
		// getDistanceSqToEntity>var2*var2] -- the single local player here
		// never "dies" mid-session, so only the hold timer and the 8-block
		// range can drop the latch.
		if(--wolf->data.monster.wolf_current_target_hold <= 0
		   || !player_has_pos
		   || player_dist_sq > WOLF_CURRENT_TARGET_RANGE_SQ)
			wolf->data.monster.wolf_has_current_target = false;
	} else if(player_has_pos && jrand_next_float(rand_src) < 0.02f) {
		// [740c583: rand.nextFloat()<0.02F -> getClosestPlayerToEntity(8.0)]
		if(player_dist_sq < WOLF_CURRENT_TARGET_RANGE_SQ) {
			wolf->data.monster.wolf_has_current_target = true;
			// [740c583: numTicksToChaseTarget = 10 + rand.nextInt(20)]
			wolf->data.monster.wolf_current_target_hold
				= 10 + jrand_next_int_bound(rand_src, 20);
		}
	}
}

// looksWithInterest gate [740c583: EntityWolf.onLivingUpdate]:
// `hasCurrentTarget() && !hasPath() && !isWolfAngry()`, then (inlined from
// the same method's body) the latched target must be an EntityPlayer
// holding the wolf's favorite item for its tamed state -- the single local
// player is the only EntityPlayer getClosestPlayerToEntity(8.0) could ever
// have returned, so held_favorite_item (the caller's already-resolved
// tamed/untamed item check) stands in for that instanceof + item test.
bool wolf_looks_with_interest(bool has_current_target, bool wolf_has_path,
	bool angry, bool held_favorite_item) {
	return has_current_target && !wolf_has_path && !angry
		&& held_favorite_item;
}

static void wolf_clear_path(struct entity* wolf) {
	wolf->data.monster.wolf_target_id = UINT32_MAX;
	wolf->data.monster.aggro_time = 0;
	wolf->data.monster.direction[0] = 0.0f;
	wolf->data.monster.direction[1] = 0.0f;
	wolf->data.monster.direction_time = 0;
	wolf->ai_state = AI_IDLE;
	// [740c583: EntityWolf.interact/updatePlayerActionState]
	// setPathToEntity(null) itself -- without this, a wolf sitting/re-targeted
	// mid-path keeps its stale in-progress A* path frozen (never advanced,
	// never finished) for as long as it stays ceased, which would also wedge
	// !hasPath() closed for the looksWithInterest gate below.
	path_entity_clear(&wolf->data.monster.path);
}

enum wolf_interact_result entity_wolf_interact(
	struct entity* wolf, struct server_local* s, struct item_data* held) {
	return entity_wolf_interact_for(wolf, s, s ? &s->player : NULL, held);
}

enum wolf_interact_result entity_wolf_interact_for(struct entity* wolf,
	struct server_local* s, struct server_player* actor, struct item_data* held) {
	if(!entity_is_wolf(wolf) || !s || !held || !actor)
		return WOLF_INTERACT_NONE;
	const char* owner_name = wolf_actor_name(s, actor);
	if(!owner_name)
		return WOLF_INTERACT_NONE;
	uint8_t* flags = &wolf->data.monster.wolf_flags;

	// Bone taming: consume first; authoritative nextInt(3)==0 decides hearts.
	// [740c583: EntityWolf.interact]
	if(!wolf_flag_get(*flags, WOLF_FLAG_TAMED)) {
		if(held->count == 0 || held->id != ITEM_BONE
		   || wolf_flag_get(*flags, WOLF_FLAG_ANGRY))
			return WOLF_INTERACT_NONE;
		if(--held->count == 0)
			*held = (struct item_data) {0};
		if(jrand_next_int_bound(&wolf->gameplay_rng, 3) != 0)
			return WOLF_INTERACT_SMOKE;
		wolf_flag_set(flags, WOLF_FLAG_TAMED, true);
		wolf_flag_set(flags, WOLF_FLAG_SITTING, true);
		wolf_flag_set(flags, WOLF_FLAG_ANGRY, false);
		wolf->health = 20;
		wolf->data.monster.wolf_watched_health = 20;
		/* RetroMCP Beta server EntityWolf.java:304: setOwner(var1.username). */
		strncpy(wolf->data.monster.wolf_owner, owner_name,
				sizeof(wolf->data.monster.wolf_owner));
		wolf->data.monster.wolf_owner[
			sizeof(wolf->data.monster.wolf_owner) - 1] = '\0';
		wolf_clear_path(wolf);
		return WOLF_INTERACT_HEARTS;
	}

	// Both Beta favorite meats are accepted, but healing is ALWAYS raw pork's
	// healAmount=3. [740c583: Item.java:337-338; EntityWolf.interact]
	if(held->count > 0
	   && (held->id == ITEM_PORKCHOP || held->id == ITEM_PORKCHOP_COOKED)
	   && wolf->data.monster.wolf_watched_health < 20) {
		if(--held->count == 0)
			*held = (struct item_data) {0};
		// this.heal(porkRaw.getHealAmount()) -> EntityLiving.heal(3): only a
		// living body heals, capped at 20, and heartsLife = 20/2 = 10, i.e.
		// the effective arbitration window (hurt_resist_time, the part of
		// heartsLife above 10) closes. [740c583: EntityLiving.java:296-305]
		if(wolf->health > 0) {
			wolf->health += 3;
			if(wolf->health > 20)
				wolf->health = 20;
			wolf->data.monster.hurt_resist_time = 0;
		}
		wolf->data.monster.wolf_watched_health = wolf->health;
		return WOLF_INTERACT_HANDLED;
	}

	// The acting player's durable name is the owner-name source. Toggle and
	// clear jumping/direct path state exactly like setPathToEntity(null).
	// [740c583: EntityWolf.interact] `setWolfSitting(!isWolfSitting());
	// isJumping = false; setPathToEntity(null);` -- NO motionY write. A wolf
	// toggled mid-jump/mid-fall keeps its existing vertical momentum in Beta
	// (isJumping=false has no persistent velocity effect); wolf_clear_path's
	// direction[]/aggro_time/wolf_target_id reset already covers the
	// isJumping+setPathToEntity(null) half faithfully.
	if(entity_wolf_owner_is_actor(wolf, s, actor)) {
		wolf_flag_set(flags, WOLF_FLAG_SITTING,
					  !wolf_flag_get(*flags, WOLF_FLAG_SITTING));
		wolf_clear_path(wolf);
		return WOLF_INTERACT_HANDLED;
	}
	return WOLF_INTERACT_NONE;
}

static bool wolf_in_search_box(const struct entity* wolf,
	const struct entity* candidate) {
	/* EntityWolf builds (pos .. pos+1)^3, then expand(16,4,16), rather
	 * than expanding a point-centered box. Preserve the resulting asymmetric
	 * bounds and the Java loop's position basis. [740c583: EntityWolf]
	 */
	return candidate->pos[0] >= wolf->pos[0] - 16.0F
		&& candidate->pos[0] <= wolf->pos[0] + 17.0F
		&& candidate->pos[1] >= wolf->pos[1] - 4.0F
		&& candidate->pos[1] <= wolf->pos[1] + 5.0F
		&& candidate->pos[2] >= wolf->pos[2] - 16.0F
		&& candidate->pos[2] <= wolf->pos[2] + 17.0F;
}

enum monster_damage_result entity_wolf_attack_from(
	struct entity* wolf, struct server_local* s, int damage,
	uint32_t attacker_id, bool attacker_is_player, bool attacker_is_arrow,
	bool from_skeleton_arrow, struct actor_ident attacker_actor) {
	if(!entity_is_wolf(wolf) || !s)
		return MONSTER_DAMAGE_ABSORBED;
	// Every hit attempt unsits first. [740c583: EntityWolf.attackEntityFrom]
	wolf_flag_set(&wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING, false);

	// Owner immunity is RETALIATION-only in Beta: super.attackEntityFrom runs
	// UNCONDITIONALLY before the owner check, so an owner's hit still deals
	// full damage (a player attacker also skips the non-player/-arrow halving
	// below). The owner branch only skips the playerToAttack assignment --
	// it does not return early or absorb the hit. Owner resolution is
	// case-insensitive, as Beta's equalsIgnoreCase.
	// [740c583: EntityWolf.attackEntityFrom]
	bool owner_hit = attacker_is_player && entity_wolf_owner_is_actor(wolf, s,
		server_local_actor_resolve(s, attacker_actor));

	// Beta's halving gate is THREE conditions: var1 != null && !player &&
	// !arrow [740c583: EntityWolf.attackEntityFrom]. Environmental sources
	// (fire, lava, fall, drowning, cactus -- attacker_id==UINT32_MAX here)
	// carry no attacker entity at all, and Beta's own damageEntity call
	// sites for every one of those pass a null Entity
	// [740c583: Entity.java (cactus/fire/lava) + EntityLiving.java
	// (fall/drown)] -- so they must NEVER be halved. Only a real non-player,
	// non-arrow MOB attacker is.
	if(attacker_id != UINT32_MAX)
		damage = wolf_adjust_incoming_damage(damage, attacker_is_player,
											attacker_is_arrow);
	enum monster_damage_result result = entity_monster_apply_damage(
		wolf, damage, from_skeleton_arrow);
	if(result == MONSTER_DAMAGE_ABSORBED)
		return result;

	uint8_t* flags = &wolf->data.monster.wolf_flags;
	bool living_attacker = attacker_is_player || attacker_id != UINT32_MAX;
	if(!wolf_flag_get(*flags, WOLF_FLAG_TAMED)
	   && !wolf_flag_get(*flags, WOLF_FLAG_ANGRY)) {
		if(attacker_is_player) {
			wolf_flag_set(flags, WOLF_FLAG_ANGRY, true);
			wolf->data.monster.wolf_target_id = 0;
			/* S1-a2: wolf_target_id 0 means "a local player body"; WHICH one is
			 * the identity. Without this an angered wolf chased the host after
			 * a guest punch. */
			wolf->data.monster.target_actor = attacker_actor;
		}
		// EntityArrow redirects to its owner before the living-attacker/pack pass.
		if(living_attacker) {
			dict_entity_it_t it;
			dict_entity_it(it, s->entities);
			while(!dict_entity_end_p(it)) {
				struct entity* pack = dict_entity_ref(it)->value;
				dict_entity_next(it);
				if(!entity_is_wolf(pack) || pack->delay_destroy == 0
				   || server_local_entity_world(s, pack)
					   != server_local_entity_world(s, wolf)
				   || wolf_flag_get(pack->data.monster.wolf_flags,
									WOLF_FLAG_TAMED)
				   || pack->data.monster.wolf_target_id != UINT32_MAX
				   || !wolf_in_search_box(wolf, pack))
					continue;
				pack->data.monster.wolf_target_id = attacker_id;
				pack->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
				if(attacker_is_player) {
					wolf_flag_set(&pack->data.monster.wolf_flags,
								  WOLF_FLAG_ANGRY, true);
					pack->data.monster.target_actor = attacker_actor;
				}
			}
		}
	} else if(!owner_hit && attacker_id != wolf->id && living_attacker) {
		wolf->data.monster.wolf_target_id = attacker_id;
		wolf->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
	}
	return result;
}

static bool wolf_normal_cube_at(int x, int y, int z, void* context) {
	struct server_world* w = context;
	struct block_data b;
	return server_world_get_block(w, x, y, z, &b)
		&& block_is_normal_cube(b.type);
}

bool wolf_find_teleport_site(float owner_x, float owner_feet_y, float owner_z,
	wolf_normal_cube_query normal_cube, void* context, vec3 out) {
	if(!normal_cube || !out)
		return false;
	int x0 = (int)floorf(owner_x) - 2;
	int z0 = (int)floorf(owner_z) - 2;
	int y = (int)floorf(owner_feet_y);
	for(int row = 0; row <= 4; row++)
		for(int col = 0; col <= 4; col++) {
			if(!(row < 1 || col < 1 || row > 3 || col > 3))
				continue;
			int x = x0 + row, z = z0 + col;
			if(normal_cube(x, y - 1, z, context)
			   && !normal_cube(x, y, z, context)
			   && !normal_cube(x, y + 1, z, context)) {
				glm_vec3_copy((vec3) {(float)x + 0.5F, (float)y,
					(float)z + 0.5F}, out);
				return true;
			}
		}
	return false;
}

bool entity_wolf_try_teleport_to_owner(struct entity* wolf,
	struct server_local* s, float owner_x, float owner_feet_y, float owner_z) {
	if(!entity_is_wolf(wolf) || !s)
		return false;
	struct server_world* w = server_local_entity_world(s, wolf);
	if(!w)
		return false;
	vec3 dest;
	if(!wolf_find_teleport_site(owner_x, owner_feet_y, owner_z,
		wolf_normal_cube_at, w, dest))
		return false;
	wolf->teleport(wolf, dest);
	return true;
}

bool entity_wolf_melee_in_range(const struct entity* wolf, float distance,
	float target_min_y, float target_max_y) {
	return entity_is_wolf(wolf) && distance < WOLF_MELEE_REACH
		&& target_max_y > wolf->pos[1]
		&& target_min_y < wolf->pos[1] + mob_body_height(MONSTER_WOLF);
}

bool entity_wolf_begin_melee(struct entity* wolf, float distance,
	float target_min_y, float target_max_y, int* raw_damage) {
	if(!entity_wolf_melee_in_range(wolf, distance,
		target_min_y, target_max_y))
		return false;
	wolf->data.monster.attack_time = 20;
	if(raw_damage)
		*raw_damage = wolf_flag_get(wolf->data.monster.wolf_flags,
			WOLF_FLAG_TAMED) ? 4 : 2;
	return true;
}

struct entity* entity_wolf_target(struct entity* wolf, struct server_local* s) {
	if(!entity_is_wolf(wolf) || !s
	   || wolf->data.monster.wolf_target_id == UINT32_MAX
	   || wolf->data.monster.wolf_target_id == 0)
		return NULL;
	struct entity* target = server_local_entity_ref(
		s, wolf, wolf->data.monster.wolf_target_id);
	if(!target || target->type != ENTITY_MONSTER || target->health <= 0
	   || target->data.monster.death_time > 0 || target->delay_destroy == 0) {
		wolf->data.monster.wolf_target_id = UINT32_MAX;
		return NULL;
	}
	return target;
}

void entity_wolf_try_find_sheep(struct entity* wolf, struct server_local* s) {
	if(!entity_is_wolf(wolf) || !s
	   || wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_TAMED)
	   || wolf->data.monster.wolf_target_id != UINT32_MAX
	   || jrand_next_int_bound(&wolf->gameplay_rng, 100) != 0)
		return;
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_SHEEP
		   && server_local_entity_world(s, e) == server_local_entity_world(s, wolf)
		   && e->health > 0 && wolf_in_search_box(wolf, e))
			count++;
	}
	if(count == 0)
		return;
	size_t pick = (size_t)jrand_next_int_bound(&wolf->gameplay_rng, (int)count);
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(!e || e->type != ENTITY_MONSTER
		   || server_local_entity_world(s, e) != server_local_entity_world(s, wolf)
		   || e->data.monster.id != MONSTER_SHEEP || e->health <= 0
		   || !wolf_in_search_box(wolf, e))
			continue;
		if(pick-- == 0) {
			wolf->data.monster.wolf_target_id = e->id;
			wolf->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
			return;
		}
	}
}

void entity_wolf_shake_tick(struct entity* wolf, struct server_local* s,
	bool is_wet, bool pathless, bool grounded) {
	if(!entity_is_wolf(wolf) || !s)
		return;
	if(is_wet) {
		wolf->data.monster.wolf_shake_armed = true;
		wolf->data.monster.wolf_shake_active = false;
		wolf->data.monster.wolf_shake_time = 0.0f;
		wolf->data.monster.wolf_prev_shake_time = 0.0f;
		return;
	}
	if(wolf->data.monster.wolf_shake_armed
	   && !wolf->data.monster.wolf_shake_active && pathless && grounded) {
		wolf->data.monster.wolf_shake_active = true;
		wolf->data.monster.wolf_shake_time = 0.0f;
		wolf->data.monster.wolf_prev_shake_time = 0.0f;
	}
	if(!wolf->data.monster.wolf_shake_active)
		return;
	/* LIVING-SOUND-RNG: the shake pitch floats are the wolf's own Random.
	 * RESIDUAL: Beta's splash-particle loop after this draws 2 more floats
	 * per particle on that Random; not modelled server-side yet.
	 * [server EntityWolf.onUpdate] */
	if(wolf->data.monster.wolf_shake_time == 0.0f)
		server_local_emit_sound(SOUND_MOB_WOLF_SHAKE,
			(vec3) {wolf->pos[0], wolf->pos[1], wolf->pos[2]}, 0.4f,
			mob_living_sound_pitch(wolf));
	wolf->data.monster.wolf_prev_shake_time
		= wolf->data.monster.wolf_shake_time;
	wolf->data.monster.wolf_shake_time += 0.05f;
	if(wolf->data.monster.wolf_prev_shake_time >= 2.0f) {
		wolf->data.monster.wolf_shake_armed = false;
		wolf->data.monster.wolf_shake_active = false;
		wolf->data.monster.wolf_shake_time = 0.0f;
		wolf->data.monster.wolf_prev_shake_time = 0.0f;
	}
}
