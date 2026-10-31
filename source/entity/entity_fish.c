/*
	Beta 1.7.3 EntityFish (fishing-slice) -- the fishing-rod bobber.

	Ground truth [740c583: EntityFish]:
	  - size 0.25F cube. Cast from EntityFish(World,EntityPlayer): spawn pose
	    = angler eye, offset back 0.16F along yaw and down 0.1F; launch
	    velocity = the look vector at speed 1.5F (the 0.0075-sigma gaussian
	    heading spread is OMITTED, matching the established bow/egg/snowball
	    precedent -- sub-degree cosmetic jitter; see entity_fish.h).
	  - per tick, while NOT hooked and NOT stuck: ray-test the tick's motion
	    against blocks AND every collidable entity (mobs + the player,
	    expanded 0.3F -- ARROW_HIT_EXPAND), owner-grace excluding the angler
	    only while ticksInAir < 5 (arrow_should_hit reuse). An ACCEPTED
	    zero-damage hit (attackEntityFrom(angler,0) returning true) hooks the
	    target (bobber = target) WITHOUT killing the fish -- a REJECTED hit
	    (an absorbed hurt-resist window) changes nothing, the bobber just
	    keeps flying. A pure block hit sticks it (inGround = true).
	  - CaveWii engineering adaptation (documented, matches the established
	    egg/arrow precedent): the true Beta order is raytrace-PREDICT-then-
	    move-with-the-full-original-motion; this engine's entity_try_move is
	    endpoint-only per axis and needs substepping to avoid tunneling a
	    thin wall at this launch speed (combat.h's ARROW_MAX_SUBSTEP_DIST;
	    the SAME 1.5 speed already required this fix for the bow and for
	    eggs/snowballs). We therefore MOVE first via substeps and sweep each
	    sub-segment, like egg/arrow -- with one further simplification this
	    file adds on top of the egg/arrow shape: on an ACCEPTED entity hook,
	    we stop the substep loop for this tick instead of continuing to run
	    the rest of Beta's SAME-tick physics tail (drag/water-check/possible
	    bite-roll) after the hook lands. In real Beta that tail still runs
	    once, using the pre-hook motion, before ride-along takes over on the
	    NEXT tick -- but the position it produces is immediately overwritten
	    by ride-along on that next tick regardless, so the only thing this
	    trims is up to one tick of otherwise-unobservable drift (and it also
	    removes the possibility of a substep loop resolving a SECOND,
	    Beta-couldn't-happen hook later in the same tick, since Beta's own
	    single raytrace can only ever resolve one hit per tick).
	  - while floating (not hooked, not stuck): a 5-slice fractional-
	    submersion probe (Beta World.isAABBInMaterial, per slice); while ANY
	    part is wet, either count down an active bite (ticksCatchable) or
	    roll for a new one at 1/500 (1/300 if the sky above the hook's OWN
	    cell can be rained on -- world_can_block_be_rained_on, reused
	    verbatim from the weather slice, queried at Y+1). A hit bite sets
	    ticksCatchable = nextInt(30)+10, kicks motionY -= 0.2, and plays the
	    splash sound + two cosmetic particle-position loops (see
	    fish_roll_bite below for why the discarded draws are still drawn).
	    Buoyancy (+-0.04 by wetness) and drag (0.92 air / 0.5 grounded, then
	    an EXTRA 0.9x drag / 0.8x motionY multiplier while wet) close out
	    the tick.
	  - catchFish() (server_local.c's cast/reel dispatch, NOT this file --
	    it needs server_local.c's static entity-spawn/item-spawn helpers):
	    returns 3 (hooked something), 1 (landed a fish), 2 (inGround --
	    OVERRIDES either of the above), or 0 (nothing). Always ends the
	    hook's life and clears the angler's fishEntity reference.

	NOT ported -- Beta's xTile/yTile/zTile/inTile "am I still stuck in the
	SAME block?" recheck, and the `shake` field. Both are cited in detail on
	entity.h's `struct entity_fish` (the fields simply are not declared
	there); the short version: xTile/yTile/zTile/inTile are NEVER assigned a
	real position anywhere in EntityFish.onUpdate (only NBT load could set
	them to something other than their -1/-1/-1/0 constructor defaults), and
	World.getBlockId(x,y,z) returns 0 for any y<0 WITHOUT a chunk lookup
	[740c583: World.getBlockId] -- so the recheck (`getBlockId(-1,-1,-1) ==
	inTile`) is providably always true for a hook that was never NBT-loaded,
	making the "unstick" branch unreachable. `shake` decrements every tick
	but nothing in EntityFish.java ever sets it above 0.

	NOT PERSISTED (entity.h's ENTITY_FISH comment): this engine's chunk-
	entity archive has no case for ENTITY_FISH (server_entity_archive_kind's
	`default: return 0;`), so a bobber does not survive a chunk unload/
	reload -- a deliberate, documented choice, not an oversight. Beta's OWN
	EntityFish.readEntityFromNBT never reassigns `this.angler` (it only
	restores xTile/yTile/zTile/inTile/shake/inGround), so a hook actually
	reconstructed from NBT in real Beta has a NULL angler; the very next
	onUpdate unconditionally calls `this.angler.getCurrentEquippedItem()`
	(the `!multiplayerWorld` liveness branch) and NPEs. That is a latent
	engine bug in Beta itself, not an observable gameplay behavior worth
	reproducing -- CODESTYLE.md S2.3's "genuinely un-portable behavior ->
	deviation record" is exactly this case.

	CLIENT: this type is server-only, exactly like the egg/snowball. The
	client's display counterpart is the CRPC_SPAWN_ARROW entity carrying
	item_id ITEM_FISHING_ROD (server_local.c's cast dispatch) -- the arrow's
	interpolation tick + flight-oriented item billboard is reused as-is.
	Beta's real bobber render (a small dedicated model/sprite, not an item
	icon) is NOT ported; no such asset exists in this engine and inventing
	one is outside a code-only slice. Documented cosmetic substitution, same
	shape as the egg's "no snowballpoof sprite -> one CRPC_SMOKE_PUFF" call.
*/

#include <assert.h>
#include <math.h>

#include "../block/aabb.h"
#include "../block/blocks.h"
#include "../item/inventory.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_weather.h"
#include "../platform/audio.h" /* SOUND_RANDOM_SPLASH */
#include "../util.h"
#include "../worldgen/beta_math.h" /* mh_cos/mh_sin -- see fish_cast_pose */
#include "combat.h"
#include "entity.h"
#include "entity_fish.h"
#include "entity_monster.h"

static struct AABB make_fish_bbox_ret(void) {
	struct AABB bbox;
	/* yOffset = 0, bottom-anchored like the arrow's/egg's box. */
	aabb_setsize_centered_offset(&bbox, FISH_BBOX_SIZE, FISH_BBOX_SIZE,
								 FISH_BBOX_SIZE, 0.0F, FISH_BBOX_SIZE * 0.5F,
								 0.0F);
	return bbox;
}

/* Player body-box ray test without damage (the arrow's/egg's
 * arrow_test_actor/egg_test_actor). S1-a3: one NAMED body. */
static bool fish_test_actor(struct server_local* s,
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
	aabb_expand(&bbox, ARROW_HIT_EXPAND); /* Beta expand(0.3F) */

	if(!aabb_ray_entry_distance(origin, dir, &bbox, &t_hit) || t_hit < 0.0f
	   || t_hit > max_dist)
		return false;
	if(out_t)
		*out_t = t_hit;
	return true;
}

/* S1-a3: the arrow's two-body sweep, for the hook. Beta excludes
 * `this.angler` while ticksInAir < 5 [740c583 EntityFish.onUpdate], so the
 * grace is a per-candidate identity compare: the caster gets its self-hook
 * grace, the OTHER body is snaggable from tick 0. Unlike the arrow/egg there
 * is no owner-id leg -- a hook has no non-player caster (no dispenser casts
 * one), so the angler identity alone IS Beta's reference compare. */
static struct server_player* fish_sweep_actors(struct server_local* s,
	const struct entity* e, const vec3 origin, const vec3 dir, float max_dist,
	float* out_t) {
	struct server_player* hit = NULL;
	float best_t = max_dist + 1.0f;
	/* Beta EntityFish.java:148 queries this.worldObj for players too. */
	struct server_world* world = server_local_entity_world(s, e);
	if(!world)
		return NULL;
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* cand = server_local_damage_candidate(s, i);
		if(!cand || !server_local_actor_in_world(s, cand, world))
			continue;
		if(!arrow_should_hit_actor(
			   actor_ident_equal(e->data.fish.angler_actor,
								 server_local_actor_ident(s, cand)),
			   e->data.fish.ticks_in_air))
			continue;
		float t = max_dist + 1.0f;
		if(!fish_test_actor(s, cand, origin, dir, max_dist, &t))
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

/* Closest-mob sweep along the sub-step (the arrow's/egg's sweep, restricted
 * to ENTITY_MONSTER -- this engine's existing stand-in for Beta's generic
 * canBeCollidedWith() filter; see entity_fish.c's file header). */
static struct entity* fish_sweep_mobs(struct server_local* s,
									  struct entity* fish, const vec3 origin,
									  const vec3 dir, float max_dist,
									  float* out_t) {
	struct entity* closest = NULL;
	float closest_t = max_dist;
	/* Beta EntityFish.java:148 queries this.worldObj. */
	struct server_world* world = server_local_entity_world(s, fish);
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
		if(cand->id == fish->id)
			continue;
		/* Owner-grace: the angler is immune only while the hook is young
		 * (Beta ticksInAir < 5); every other mob is a target from tick 0. */
		if(!arrow_should_hit(0, cand->id, fish->data.fish.ticks_in_air))
			continue;

		struct AABB mbox;
		float t_hit;
		if(cand->getBoundingBox && cand->getBoundingBox(cand, &mbox) > 0) {
			aabb_expand(&mbox, ARROW_HIT_EXPAND);
			if(aabb_ray_entry_distance(origin, dir, &mbox, &t_hit)
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

/* Beta World.isAABBInMaterial(box, Material.water) [740c583], restricted to
 * the water check this file needs. Scans every block cell the box's X/Y/Z
 * extent spans (matching Beta's own three nested loops, not just the
 * entity's single column -- the hook's 0.25-wide box straddling a block
 * boundary is a real, if rare, case). A cell counts when its OWN fluid top
 * surface -- RAW (unmasked) metadata; Beta's `var13 < 8` guard treats 8-15
 * (the "falling" bit-or'd range) as a full block, matching this engine's
 * 4-bit metadata field exactly, no extra masking needed -- is at or above
 * the PROBE box's own minY (the caller passes one 1/5-height SLICE box, not
 * the hook's whole bbox, which is why the compare is against `box->y1`). */
static bool fish_aabb_in_water(struct server_local* s,
							   const struct AABB* box) {
	w_coord_t x0 = (w_coord_t)floorf(box->x1);
	w_coord_t x1 = (w_coord_t)floorf(box->x2 + 1.0f);
	w_coord_t y0 = (w_coord_t)floorf(box->y1);
	w_coord_t y1 = (w_coord_t)floorf(box->y2 + 1.0f);
	w_coord_t z0 = (w_coord_t)floorf(box->z1);
	w_coord_t z1 = (w_coord_t)floorf(box->z2 + 1.0f);

	for(w_coord_t x = x0; x < x1; x++)
		for(w_coord_t y = y0; y < y1; y++)
			for(w_coord_t z = z0; z < z1; z++) {
				struct block_data blk;
				if(!server_world_get_block(block_world(s), x, y, z, &blk))
					continue;
				if(blk.type != BLOCK_WATER_FLOW && blk.type != BLOCK_WATER_STILL)
					continue;
				int meta = blk.metadata;
				float surface = (float)(y + 1);
				if(meta < 8)
					surface -= (float)meta / 8.0f;
				if(surface >= box->y1)
					return true;
			}
	return false;
}

/* The 5-slice submersion fraction [740c583: EntityFish.onUpdate], over the
 * hook's CURRENT world-space bbox. Returns 0..1 in steps of 1/5. */
static float fish_submersion_fraction(struct server_local* s,
									  const struct entity* e) {
	struct AABB world_box;
	entity_make_world_bbox(e, &world_box);

	float submerged = 0.0f;
	float span = world_box.y2 - world_box.y1;
	for(int slice = 0; slice < FISH_SUBMERSION_SLICES; slice++) {
		struct AABB slice_box = world_box;
		slice_box.y1 = world_box.y1
			+ span * (float)(slice + 0) / (float)FISH_SUBMERSION_SLICES;
		slice_box.y2 = world_box.y1
			+ span * (float)(slice + 1) / (float)FISH_SUBMERSION_SLICES;
		if(fish_aabb_in_water(s, &slice_box))
			submerged += 1.0f / (float)FISH_SUBMERSION_SLICES;
	}
	return submerged;
}

/* Apply the zero-damage "hook" hit to a candidate MOB target [740c583:
 * `if (var3.entityHit.attackEntityFrom(this.angler, 0)) this.bobber =
 * var3.entityHit;`]. Routed through the real mob damage path (the egg's
 * zero-damage precedent): an ABSORBED result (an active hurt-resist window)
 * is Beta's `attackEntityFrom` returning false -- the hook does NOT attach,
 * exactly like a rejected arrow bounce. A FRESH result arms the SAME
 * knockback + hostile-aggro side effects any other zero-damage generic hit
 * would (Beta's attackEntityFrom retaliation convention). Returns true iff
 * the hook attached. */
static bool fish_hook_mob(struct entity* mob, struct server_local* s,
						  struct actor_ident angler_id) {
	// PIG ZOMBIE PACK ANGER (PIGZ-PACK) -- same citation as entity_arrow.c's
	// arrow_damage_mob: EntityFish.onUpdate calls
	// entityHit.attackEntityFrom(this.angler, 0) [server EntityFish.java:174]
	// and the angler is always an EntityPlayer, so every hooked pig zombie
	// alerts its pack. BEFORE the damage call: ahead of the i-frame/health
	// gate and of this mob's hurt pitch [EntityLiving.java:293-345].
	if(mob->data.monster.id == MONSTER_PIG_ZOMBIE)
		server_local_pig_zombie_pack_anger(s, mob, angler_id);
	enum monster_damage_result result
		= entity_monster_apply_damage(mob, 0, false);
	if(result == MONSTER_DAMAGE_ABSORBED)
		return false;
	if(result == MONSTER_DAMAGE_FRESH) {
		/* S1-a2: knockback away from the ANGLER and retaliation ONTO the
		 * angler. Beta passes `this.angler` as the damage source [740c583
		 * EntityFish.onUpdate: attackEntityFrom(this.angler, 0)]. A departed
		 * angler falls back to the host (the pre-S1 behaviour, and the only
		 * actor that provably still exists). */
		struct server_player* angler = server_local_actor_resolve(s, angler_id);
		if(!angler)
			angler = &s->player;
		/* Mob motion units S2 (Q3): attackEntityFrom(this.angler, 0) ->
		 * EntityLiving.knockBack from the angler, composed at the hit on the
		 * authoritative doubles. */
		mob_motion_knockback(mob, &mob->data.monster.motion_d,
							 angler->x - mob->data.monster.motion_d.pos[0],
							 angler->z - mob->data.monster.motion_d.pos[2],
							 server_local_sound_rng());
		mob->data.monster.knockback_time = MOB_KNOCKBACK_TICKS;
		if(!monster_is_passive(mob->data.monster.id)) {
			mob->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
			mob->data.monster.target_actor = angler_id;
		}
	}
	return true;
}

/* The bite roll [740c583: EntityFish.onUpdate, the `var27 > 0.0D` block].
 * Draws EVERY RNG value Beta's own body draws, unconditionally, even the
 * ones this engine discards (CODESTYLE.md S2.3: "any RNG draw: draw
 * unconditionally... Beta draws even when the result is discarded" -- the
 * hook-owned gameplay_rng must consume the same COUNT/ORDER of draws a
 * bite spends here, or the next bite/tug silently desyncs).
 * [CFR EntityFish.java:231-255; Entity.java:96 owns the Random]. Beta's two particle-spam loops (6x "bubble" + 6x "splash" at
 * width=0.25 -> `1.0F + width*20.0F` == 6.0F) spend 18 + 12 = 30 nextFloat
 * draws purely on cosmetic position jitter this engine has no bubble/
 * splash particle type for; they are drawn and discarded, and ONE
 * CRPC_SMOKE_PUFF stands in for the whole burst (the egg's "no
 * snowballpoof sprite" substitution, same shape). */
static void fish_roll_bite(struct entity* e, struct server_local* s) {
	int roll = jrand_next_int_bound(&e->gameplay_rng, FISH_BITE_DURATION_RANGE);
	e->data.fish.ticks_catchable = fish_bite_duration(roll);
	e->vel[1] += FISH_BITE_TUG_Y;

	/* TWO nextFloat() draws, in this exact order -- named locals, never
	 * sibling arguments (CODESTYLE.md S2.3). */
	float pitch_a = jrand_next_float(&e->gameplay_rng);
	float pitch_b = jrand_next_float(&e->gameplay_rng);
	server_local_emit_sound(SOUND_RANDOM_SPLASH,
							(vec3) {e->pos[0], e->pos[1], e->pos[2]}, 0.25f,
							fish_splash_pitch(pitch_a, pitch_b));

	/* `1.0F + this.width * 20.0F` at width=FISH_BBOX_SIZE (0.25) == 6.0F ->
	 * var15 runs 0..5 (6 iterations) on both loops. */
	int particle_iterations = (int)(1.0f + FISH_BBOX_SIZE * 20.0f);
	for(int i = 0; i < particle_iterations; i++) {
		(void)jrand_next_float(&e->gameplay_rng); /* bubble X jitter */
		(void)jrand_next_float(&e->gameplay_rng); /* bubble Z jitter */
		(void)jrand_next_float(&e->gameplay_rng); /* bubble Y-motion jitter (inline
											* in Beta's spawnParticle call) */
	}
	for(int i = 0; i < particle_iterations; i++) {
		(void)jrand_next_float(&e->gameplay_rng); /* splash X jitter */
		(void)jrand_next_float(&e->gameplay_rng); /* splash Z jitter */
	}
	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_SMOKE_PUFF,
		.payload.smoke_puff.pos = {e->pos[0], e->pos[1] + 1.0f, e->pos[2]},
		.payload.smoke_puff.intensity = 1.0f,
	});
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
	assert(e && s);
	glm_vec3_copy(e->pos, e->pos_old);

	/* --- liveness: angler still fishing THIS rod, in range [740c583] ----
	 * Beta: `angler.isDead || !isEntityAlive() || heldItem == null ||
	 * heldItem.getItem() != Item.fishingRod || distanceSq(angler) >
	 * 1024.0`. Since the deviation-#18 closure the engine DOES have a
	 * lingering dead-player state (the death-screen dwell between the lethal
	 * hit and the SRPC_RESPAWN transaction), so isEntityAlive translates to
	 * health > 0 alongside has_pos/finished_loading (the established
	 * egg/arrow convention for "is there a valid player to interact with"):
	 * a corpse's bobber reels itself in, exactly like Beta's
	 * !isEntityAlive() exit.
	 *
	 * CORRECTED CITATION (adversarial-review finding 5; this used to
	 * subtract PLAYER_EYE_HEIGHT to reconstruct the player's "feet" here,
	 * on the same false "posY is the feet" premise findings 3/8 name).
	 * Entity.getDistanceSqToEntity [740c583] is `(this.posX-var1.posX)^2 +
	 * (this.posY-var1.posY)^2 + (this.posZ-var1.posZ)^2` -- RAW posY-posY,
	 * NO yOffset term at all. Beta's player posY is already at eye level
	 * (see fish_cast_pose's citation in entity_fish.h for why), which is
	 * exactly this engine's own player.y convention -- so the correct
	 * translation is player.y DIRECTLY, not player.y-PLAYER_EYE_HEIGHT.
	 * (The ride-along block just below this one is a DIFFERENT Beta
	 * formula -- `bobber.boundingBox.minY + height*0.8` -- which genuinely
	 * does need the true bbox bottom; that block is unaffected by this
	 * fix.) */
	/* S1-a2: the angler is the ACTOR named by e->data.fish.angler_actor, not
	 * s->player. This block used to read the host unconditionally, so a
	 * guest-cast hook was reeled in on its very first tick (the host is not
	 * holding a rod) AND the clear below wrote the HOST's fish_entity_id --
	 * one actor's action mutating the other's state, the exact class S1
	 * exists to remove. Re-resolving also supplies Beta's `angler.isDead`
	 * leg for a departed session: a hook whose angler LEFT, was RELEASED or
	 * REJOINED under a new generation no longer resolves, so the bobber is
	 * removed rather than outliving its owner. Host angler: the identity is
	 * the zero value, resolve returns &s->player, and every line is the one
	 * it replaced. */
	{
		struct server_player* angler
			= server_local_actor_resolve(s, e->data.fish.angler_actor);
		if(!angler)
			return true; /* the angler is gone: remove the hook */
		struct item_data held;
		bool has_rod = angler->has_pos && angler->finished_loading
			&& angler->health > 0
			&& inventory_get_hotbar_item(&angler->inventory, &held)
			&& held.id == ITEM_FISHING_ROD;
		bool in_range = false;
		if(has_rod) {
			float ddx = (float)angler->x - e->pos[0];
			float ddy = (float)angler->y - e->pos[1];
			float ddz = (float)angler->z - e->pos[2];
			in_range = (ddx * ddx + ddy * ddy + ddz * ddz)
				<= FISH_ANGLER_MAX_DIST_SQ;
		}
		if(!has_rod || !in_range) {
			angler->fish_entity_id = 0;
			return true;
		}
	}

	/* --- ride-along on a hooked target [740c583: onUpdate top] ----------
	 * `if (bobber != null) { if (!bobber.isDead) { ride...; return; }
	 * bobber = null; }` -- falls through (no return) once the target is
	 * gone, matching Beta's own shape exactly. */
	if(e->data.fish.bobber_hooked) {
		bool still_alive = false;
		float target_x = 0.0f, target_bottom_y = 0.0f, target_z = 0.0f,
			  target_height = 0.0f;

		if(e->data.fish.bobber_id == 0) {
			/* S1-a3: the ride-along follows the body that was actually
			 * snagged (bobber_actor), not "the player". A body whose session
			 * ended stops resolving, which is Beta's `!bobber.isDead` leg --
			 * the hook drops it and falls through, exactly as below. */
			struct server_player* snagged
				= server_local_actor_resolve(s, e->data.fish.bobber_actor);
			/* The ORIGINAL condition (has_pos), now asked of the resolved
			 * body rather than of s->player -- deliberately NOT extended with
			 * a health test: Beta drops the bobber on `!bobber.isDead`, and a
			 * player at health 0 is not isDead, so a corpse still rides. A
			 * body whose SESSION ended does not resolve at all, which is the
			 * real isDead case here. */
			if(snagged && snagged->has_pos) {
				still_alive = true;
				target_x = (float)snagged->x;
				target_bottom_y = (float)(snagged->y - PLAYER_EYE_HEIGHT);
				target_z = (float)snagged->z;
				target_height = PLAYER_BODY_HEIGHT;
			}
		} else {
			struct entity** hp
				= dict_entity_get(s->entities, e->data.fish.bobber_id);
			if(hp && *hp && (*hp)->type == ENTITY_MONSTER
			   && (*hp)->health > 0) {
				struct entity* target = *hp;
				struct AABB tbox;
				if(target->getBoundingBox
				   && target->getBoundingBox(target, &tbox) > 0) {
					still_alive = true;
					target_x = target->pos[0];
					target_bottom_y = tbox.y1;
					target_z = target->pos[2];
					target_height = target->height;
				}
			}
		}

		if(still_alive) {
			/* Beta: posY = bobber.boundingBox.minY + bobber.height*0.8D. */
			e->pos[0] = target_x;
			e->pos[1] = target_bottom_y + target_height * 0.8f;
			e->pos[2] = target_z;
			server_local_send_entity(s, e, (struct client_rpc) {
				.type = CRPC_ENTITY_MOVE,
				.payload.entity_move.entity_id = e->id,
				.payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]},
			});
			return false;
		}
		e->data.fish.bobber_hooked = false;
		e->data.fish.bobber_id = 0;
		e->data.fish.bobber_actor = ACTOR_IDENT_HOST;
	}

	/* --- stuck-in-ground: frozen, 1200-tick despawn [740c583] ------------
	 * See entity.h's entity_fish.ticks_in_ground comment: the "did the
	 * block change?" recheck ahead of this in real Beta is unreachable
	 * here, so this reduces to a plain counter. */
	if(e->data.fish.in_ground) {
		e->data.fish.ticks_in_ground++;
		if(e->data.fish.ticks_in_ground >= FISH_STUCK_DESPAWN_TICKS) {
			/* R-S1a1b F2 / R-S1a2 FIX: clear the ANGLER's bobber id, not the
			 * host's. Beta's setEntityDead() pairs with `angler.fishEntity =
			 * null` [740c583 EntityFish], i.e. the field of the angler this
			 * hook belongs to. Writing s->player unconditionally here was the
			 * WORLD-SAVE regression: a GUEST hook that despawned stuck in the
			 * ground left guest->fish_entity_id set forever, and
			 * server_local_guest_checkpoint (server_local_guest.c) refuses
			 * while it is nonzero -- which is the first line of
			 * server_local_write_level_state, so every later autosave/save
			 * silently failed -- while simultaneously zeroing the HOST's own
			 * live bobber id. A departed angler resolves to NULL: nothing to
			 * clear, because the field died with the body. */
			struct server_player* angler
				= server_local_actor_resolve(s, e->data.fish.angler_actor);
			if(angler)
				angler->fish_entity_id = 0;
			return true;
		}
		return false; /* stay put -- no move happened this tick */
	}
	e->data.fish.ticks_in_air++;

	/* --- movement + hit detection ---------------------------------------
	 * See the file header for why this substeps (anti-tunneling, the same
	 * fix the bow/egg/snowball already needed at this launch speed) rather
	 * than Beta's literal raytrace-then-move-with-full-motion order, and
	 * for the one further simplification this file adds on top of that
	 * (an accepted hook stops the loop instead of still running the rest
	 * of this tick's own physics). */
	struct AABB bbox = make_fish_bbox_ret();
	aabb_translate(&bbox, 0.0F, e->y_size, 0.0F);
	vec3 planned_travel, travel, dir;
	glm_vec3_copy(e->vel, planned_travel);

	float plan_len = sqrtf(planned_travel[0] * planned_travel[0]
						   + planned_travel[1] * planned_travel[1]
						   + planned_travel[2] * planned_travel[2]);
	int substeps = arrow_move_substeps(plan_len);
	vec3 step_vel = {planned_travel[0] / substeps, planned_travel[1] / substeps,
					 planned_travel[2] / substeps};

	bool hit_block = false;
	bool tick_collided_horizontally = false;

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
		tick_collided_horizontally = tick_collided_horizontally || sub_collision;

		travel[0] = e->pos[0] - sub_origin[0];
		travel[1] = e->pos[1] - sub_origin[1];
		travel[2] = e->pos[2] - sub_origin[2];
		float dist2 = travel[0] * travel[0] + travel[1] * travel[1]
			+ travel[2] * travel[2];

		bool entity_hit_this_substep = false;
		if(dist2 > glm_pow2(0.0001f)) {
			float dist = sqrtf(dist2);
			dir[0] = travel[0] / dist;
			dir[1] = travel[1] / dist;
			dir[2] = travel[2] / dist;

			float t_player = dist + 1.0f, t_mob = dist + 1.0f;
			/* S1-a3: both local bodies are snag candidates (see
			 * fish_sweep_actors for the per-candidate angler grace). */
			struct server_player* hit_actor
				= fish_sweep_actors(s, e, sub_origin, dir, dist, &t_player);
			bool hit_player = hit_actor != NULL;
			struct entity* mob
				= fish_sweep_mobs(s, e, sub_origin, dir, dist, &t_mob);

			if(hit_player && (!mob || t_player <= t_mob)) {
				entity_hit_this_substep = true;
				bool accepted = false;
				(void)server_local_damage_actor_result(s, hit_actor, 0, dir[0],
													   dir[2], &accepted);
				if(accepted) {
					e->data.fish.bobber_hooked = true;
					e->data.fish.bobber_id = 0;
					/* Record WHICH body: bobber_id 0 means "a local player",
					 * and both bodies share that sentinel id. */
					e->data.fish.bobber_actor
						= server_local_actor_ident(s, hit_actor);
				}
			} else if(mob) {
				entity_hit_this_substep = true;
				if(fish_hook_mob(mob, s, e->data.fish.angler_actor)) {
					e->data.fish.bobber_hooked = true;
					e->data.fish.bobber_id = mob->id;
				}
			}
		}

		if(e->data.fish.bobber_hooked)
			break; /* documented simplification -- see file header */

		if(!entity_hit_this_substep
		   && (sub_collision || e->on_ground
			   || fabsf(travel[0] - sub_move[0]) > 0.001f
			   || fabsf(travel[1] - sub_move[1]) > 0.001f
			   || fabsf(travel[2] - sub_move[2]) > 0.001f)) {
			hit_block = true;
			break;
		}
	}

	/* --- W2-B1: Beta Entity.moveEntity's footstep tail for the bobber ------
	 * EntityFish extends Entity and does NOT override canTriggerWalking (only
	 * boat/FX/falling sand/item/minecart/spider/primed TNT/wolf do); its
	 * yOffset is 0 [EntityFish.java:68], isSneaking is the never-set entity
	 * flag 1 and it rides nothing, so every moveEntity call -- made whenever
	 * !inGround [:233-234], i.e. the no-block-hit path here (an accepted hook
	 * still moved) -- accumulates distanceWalkedModified and, on the
	 * nextStepDistance crossing with a non-air (fence-substituted) foot cell
	 * floor(posY - 0.2F), plays the step sound then runs onEntityWalking at
	 * the foot cell. Shared helpers: the player/mob odometer, footstep emit and
	 * the R1 walk dispatch (BLOCK_WALKER_MOB = "not a player").
	 * [740c583: Entity.java:437-458] */
	if(!hit_block) {
		w_coord_t sfx = (w_coord_t)floorf(e->pos[0]);
		w_coord_t ffy = (w_coord_t)floor((double)e->pos[1] - (double)0.2F);
		w_coord_t sfz = (w_coord_t)floorf(e->pos[2]);
		struct block_data footc, fence_chk;
		bool solid_below = server_world_get_block((struct server_world*)e->world, sfx, ffy, sfz, &footc)
			&& footc.type != BLOCK_AIR;
		if(!solid_below
		   && server_world_get_block((struct server_world*)e->world, sfx, ffy - 1, sfz, &fence_chk)
		   && audio_footstep_block_id(BLOCK_AIR, fence_chk.type) != BLOCK_AIR)
			solid_below = true;
		if(server_player_walk_step(&e->data.fish.walk_dist,
								   &e->data.fish.walk_next_step,
								   (double)(e->pos[0] - e->pos_old[0]),
								   (double)(e->pos[2] - e->pos_old[2]), solid_below)) {
			server_local_emit_footstep(s, sfx, ffy, sfz,
									   (vec3) {e->pos[0], e->pos[1], e->pos[2]});
			server_local_entity_walked_on_block_w(s, server_local_entity_world(s, e),
												  sfx, ffy, sfz, BLOCK_WALKER_MOB);
		}
	}

	if(e->data.fish.bobber_hooked) {
		server_local_send_entity(s, e, (struct client_rpc) {
			.type = CRPC_ENTITY_MOVE,
			.payload.entity_move.entity_id = e->id,
			.payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]},
		});
		return false;
	}

	if(hit_block) {
		e->data.fish.in_ground = true;
		e->data.fish.ticks_in_ground = 0;
		glm_vec3_zero(e->vel);
		server_local_send_entity(s, e, (struct client_rpc) {
			.type = CRPC_ENTITY_MOVE,
			.payload.entity_move.entity_id = e->id,
			.payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]},
		});
		return false;
	}

	/* --- physics tail: water submersion, bite roll, buoyancy, drag ------ */
	float drag = (e->on_ground || tick_collided_horizontally) ? FISH_DRAG_GROUND
															   : FISH_DRAG_AIR;

	float submerged = fish_submersion_fraction(s, e);

	if(submerged > 0.0f) {
		if(e->data.fish.ticks_catchable > 0) {
			e->data.fish.ticks_catchable--;
		} else {
			w_coord_t fx = (w_coord_t)floorf(e->pos[0]);
			w_coord_t fy = (w_coord_t)floorf(e->pos[1]);
			w_coord_t fz = (w_coord_t)floorf(e->pos[2]);
			/* Note the +1 on Y: the query is the cell ABOVE the hook, not
			 * the hook's own cell [740c583]. */
			bool rained_on = world_can_block_be_rained_on(s, fx, fy + 1, fz);
			int chance = fish_bite_chance(rained_on);
			if(jrand_next_int_bound(&e->gameplay_rng, chance) == 0)
				fish_roll_bite(e, s);
		}
	}

	if(e->data.fish.ticks_catchable > 0) {
		float fa = jrand_next_float(&e->gameplay_rng);
		float fb = jrand_next_float(&e->gameplay_rng);
		float fc = jrand_next_float(&e->gameplay_rng);
		e->vel[1] -= (fa * fb * fc) * 0.2f;
	}

	float buoyancy = submerged * 2.0f - 1.0f;
	e->vel[1] += FISH_BUOYANCY * buoyancy;
	if(submerged > 0.0f) {
		drag *= FISH_DRAG_WET_MULT;
		e->vel[1] *= FISH_MOTION_Y_WET_MULT;
	}
	e->vel[0] *= drag;
	e->vel[1] *= drag;
	e->vel[2] *= drag;

	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_ENTITY_MOVE,
		.payload.entity_move.entity_id = e->id,
		.payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]},
	});

	return false;
}

static size_t getBoundingBox_fish(const struct entity* e, struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

void fish_cast_pose(float eye_x, float eye_y, float eye_z, float yaw_deg,
					float pitch_deg, vec3 out_pos, vec3 out_vel) {
	assert(out_pos && out_vel);

	/* player.rx/player.ry map 1:1 onto Beta's rotationYaw/rotationPitch, in
	 * DEGREES, no negation/offset -- see server_local_drop_hotbar_item's
	 * citation in server_local.c (the same mapping this reuses directly). */
	float yaw = yaw_deg * (float)M_PI / 180.0f;
	float pitch = pitch_deg * (float)M_PI / 180.0f;
	/* MathHelper.sin/cos, NOT libm sinf/cosf (adversarial-review finding
	 * 10): Beta's trig is a 65536-entry table (worldgen/beta_math.c's
	 * mh_sin/mh_cos), and CODESTYLE.md names this exact substitution as a
	 * trap -- "the table's quantization is observable". mh_sin/mh_cos
	 * lazily self-init (idempotent), so there is no ordering hazard calling
	 * them here before any worldgen pass has run. NOTE: the PRE-EXISTING
	 * bow/egg/snowball throw-velocity casts in server_local.c make this
	 * same libm substitution and are NOT touched here -- they are
	 * previously-shipped, hardware-tested code outside this slice's scope
	 * (the same "don't touch a sibling's tested surface" boundary this
	 * slice already applies to entity_arrow.c/entity_egg.c's own
	 * ray/AABB copies). Fixing this file's OWN new code to the byte-exact
	 * primitive is in scope; migrating the siblings is a separate,
	 * explicitly tracked deferral -- see FISHING_SLICE_NOTES.md. */
	float cy = mh_cos(yaw), sy = mh_sin(yaw);
	float cp = mh_cos(pitch), sp = mh_sin(pitch);

	/* Beta: posX -= cos(yaw)*0.16F; posY -= 0.1D; posZ -= sin(yaw)*0.16F. */
	out_pos[0] = eye_x - cy * FISH_CAST_XZ_OFFSET;
	out_pos[1] = eye_y - FISH_CAST_Y_OFFSET;
	out_pos[2] = eye_z - sy * FISH_CAST_XZ_OFFSET;

	/* Beta: motionX=-sin(yaw)*cos(pitch)*0.4; motionZ=cos(yaw)*cos(pitch)*0.4;
	 * motionY=-sin(pitch)*0.4; then calculateVelocity renormalizes (a
	 * no-op here -- this vector is already unit length) and scales to
	 * 1.5F. The 0.0075-sigma gaussian spread calculateVelocity ALSO adds is
	 * OMITTED (see entity_fish.h's FISH_LAUNCH_SPEED comment). */
	out_vel[0] = -sy * cp * FISH_LAUNCH_SPEED;
	out_vel[1] = -sp * FISH_LAUNCH_SPEED;
	out_vel[2] = cy * cp * FISH_LAUNCH_SPEED;
}

void entity_fish(uint32_t id, struct entity* e, bool server, void* world) {
	assert(e && world);

	entity_default_init(e, server, world);
	entity_set_size(e, FISH_BBOX_SIZE, FISH_BBOX_SIZE, 0.0F);
	e->id = id;
	if(server) {
		struct server_world* w = world;
		entity_lifetime_rng_seed(&e->gameplay_rng, w->seed, w->dimension, id);
	}
	e->type = ENTITY_FISH;
	e->name = "Fish";
	e->tick_server = entity_server_tick;
	/* Server-only type: the client's display entity is a CRPC_SPAWN_ARROW
	 * item billboard (see the file header), so these client hooks are
	 * inert defaults, exactly like the egg/snowball. */
	e->tick_client = entity_default_client_tick;
	e->render = NULL;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = getBoundingBox_fish;
	e->onLeftClick = NULL;
	e->leftClickText = NULL;
	e->onRightClick = NULL;
	e->rightClickText = NULL;
	e->delay_destroy = -1;
	/* S1-a2: stamped by the cast site; host zero value is the default. */
	e->data.fish.angler_actor = ACTOR_IDENT_HOST;
	e->data.fish.bobber_actor = ACTOR_IDENT_HOST;
	e->data.fish.bobber_hooked = false;
	e->data.fish.bobber_id = 0;
	e->data.fish.in_ground = false;
	e->data.fish.ticks_in_ground = 0;
	e->data.fish.ticks_in_air = 0;
	e->data.fish.ticks_catchable = 0;
	e->data.fish.walk_dist = 0.0F;
	e->data.fish.walk_next_step = 1; // Beta nextStepDistance = 1
	glm_vec3_copy(e->pos, e->network_pos);
}
