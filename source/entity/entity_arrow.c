/*
	Beta 1.7.3 EntityArrow, ported from the fCavEX/CaveWii arrow. Now implements the
	full ranged slice (audit #17c / #17d):
	  - ENTITY SWEEP: the arrow ray-tests its per-tick travel against ALL collidable
	    entities (the player + every mob), keeping the CLOSEST hit, excluding only the
	    OWNER during the owner-grace window (Beta: var9 != owner || ticksInAir >= 5).
	    A player-fired arrow (from_player) hits MOBS (mob 20-pt scale); a skeleton
	    arrow hits the PLAYER (player 160-pt scale) AND, faithfully, other MOBS
	    (infighting). from_player selects the HEALTH SCALE on a hit, NOT who can be hit.
	  - STICK: on a BLOCK hit the arrow sticks (in_ground, retained impact vel,
	    arrowShake=7)
	    instead of vanishing, and keeps rendering in place.
	  - PICKUP: a player-fired stuck arrow the player walks over is picked up (+1
	    ITEM_ARROW), once arrowShake has decayed; a skeleton arrow is never pickable.
	  - REAPERS: a stuck arrow dies at ticksInGround 1200; an airborne arrow has
	    no age cap and dies on the tick after its Y crosses below -64.
	Beta physics: gravity 0.03/tick, 0.99 air drag, launch speed 0.6 (skeleton) / 1.5
	(player bow). All constants + the pure decision helpers live in combat.h.
*/

#include <assert.h>
#include <math.h>

#include "../block/aabb.h"
#include "../block/blocks.h"
#include "../block/redstone_piston_server.h"
#include "../game/difficulty.h" // W1F: skeleton-arrow difficulty scale
#include "../game/game_state.h" // gstate.remote_session (MP embed drr)
#include "../game/camera.h"
#include "../graphics/render_item.h"
#include "../item/items.h"
#include "../network/client_interface.h"
#include "../network/piston_moving_transport.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/audio.h" /* S2: random.drr / random.pop */
#include "../platform/gfx.h"
#include "../platform/texture.h"
#include "../worldgen/beta_math.h"
#include "combat.h"
#include "block_ray.h"
#include "entity.h"
#include "entity_monster.h"
#include "entity_wolf.h"

#define ARROW_BBOX_SIZE 0.5f

/* S2: the arrow's OWN impact sound -- "random.drr" (random.bowhit does NOT
   exist in b1.7.3), 1.0F, pitch 1.2F / (r*0.2F + 0.9F), at the arrow.
   Identical for an ACCEPTED entity hit and a block embed (Beta makes no
   distinction); a bounced (attackEntityFrom==false) hit is silent -- the
   callers gate on the accept edges. [740c583: EntityArrow.onUpdate]
   F4-RNG (RNG ruling 9): `this.rand.nextFloat()` at onUpdate:148/175 is the
   ARROW's own Entity.rand -- the same gameplay_rng that drew the heading
   Gaussians and the dislodge floats -- one draw, after the victim's
   processing on an accepted hit; never a cosmetic or sound-only stream. */
static void arrow_emit_drr(struct entity* e) {
	const float r = jrand_next_float(&e->gameplay_rng);
	server_local_emit_sound(
		SOUND_RANDOM_DRR, (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 1.0F,
		1.2F / (r * 0.2F + 0.9F));
}

/* Arrow-local copy of entity.c:37-50: the point ray needs the same moving-TE
 * route for both collision presence and selection bounds. [740c583:
 * BlockPistonMoving.getCollisionBoundingBoxFromPool:80-99] */
static bool arrow_piston_moving_lookup(void* ctx, w_coord_t x, w_coord_t y,
									  w_coord_t z,
									  struct piston_moving_state* out) {
	struct entity* e = ctx;
	if(e->on_server) {
		struct server_world* world = e->world;
		return world && world->local
			&& piston_moving_state_at(world->local, x, y, z, out);
	}
	return client_piston_moving_query(x, y, z, out);
}

static void arrow_ray_box(struct block_ray_cell* out, const struct AABB* b) {
	out->has_ray_box = true;
	out->x1 = b->x1; out->y1 = b->y1; out->z1 = b->z1;
	out->x2 = b->x2; out->y2 = b->y2; out->z2 = b->z2;
}

static bool arrow_ray_cell(int x, int y, int z, bool stop_on_liquid,
						   struct block_ray_cell* out, void* ctx) {
	struct entity* e = ctx;
	struct block_data cell;
	*out = (struct block_ray_cell) {0};
	if(!entity_get_block(e, x, y, z, &cell))
		return false; /* unavailable chunk is not an invented solid cell */
	if(cell.type == BLOCK_AIR || !blocks[cell.type])
		return true;
	out->present = true;
	bool fluid = cell.type == BLOCK_WATER_FLOW || cell.type == BLOCK_WATER_STILL
		|| cell.type == BLOCK_LAVA_FLOW || cell.type == BLOCK_LAVA_STILL;
	/* [740c583 BlockFluid.canCollideCheck:56,70; Block.java:275]. */
	out->collidable = !fluid || (stop_on_liquid && cell.metadata == 0);
	if(cell.type == BLOCK_FIRE)
		out->collidable = false;
	if(!out->collidable)
		return true;
	struct block_info info = {.block = &cell, .x = x, .y = y, .z = z,
		.get_piston_moving = arrow_piston_moving_lookup,
		.get_piston_moving_ctx = e};
	struct block* b = blocks[cell.type];
	if(cell.type == BLOCK_PISTON_MOVING) {
		struct piston_moving_state state;
		if(!arrow_piston_moving_lookup(e, x, y, z, &state)
		   || state.stored_id == BLOCK_AIR
		   || state.stored_id == BLOCK_PISTON_MOVING
		   || !blocks[state.stored_id])
			return true; /* missing TE: Beta collision getter is null */
		/* BlockPistonMoving:102 evaluates the stored block against the MOVING
		 * CELL metadata, for presence and ray. The TE's stored_meta is not the
		 * world metadata at these coordinates. Synthetic mismatch is tested. */
		cell.type = state.stored_id;
		info.block = &cell;
		b = blocks[cell.type];
		out->has_collision_box = b->getBoundingBox(&info, true, NULL) > 0;
		if(!out->has_collision_box)
			return true;
		struct AABB ray;
		if(cell.type == BLOCK_CACTUS) {
			/* BlockCactus:27-38 only insets selection/collision pools; inherited
			 * Block.collisionRayTrace:357 keeps the unit block bounds. */
			aabb_setsize(&ray, 1.0F, 1.0F, 1.0F);
		} else if(b->getBoundingBox(&info, false, NULL) == 1) {
			b->getBoundingBox(&info, false, &ray);
		} else return true;
		float progress = (state.flags & PISTON_MOVING_FLAG_EXTENDING)
			? 1.0F - state.last_progress : state.last_progress;
		int dx, dy, dz;
		piston_moving_facing_offset(state.facing, &dx, &dy, &dz);
		arrow_ray_box(out, &ray);
		/* [740c583 BlockPistonMoving:102-118, TileEntityPiston:49-54]:
		 * float axis*progress, then promotion to double for translation. */
		double sx = (double)((float)dx * progress);
		double sy = (double)((float)dy * progress);
		double sz = (double)((float)dz * progress);
		out->x1 -= sx; out->x2 -= sx;
		out->y1 -= sy; out->y2 -= sy;
		out->z1 -= sz; out->z2 -= sz;
		return true;
	}
	/* World.java:716,838 queries collision presence BEFORE ray geometry.
	 * For stairs this is two local pieces; the Beta ray remains one cube. */
	out->has_collision_box = b->getBoundingBox(&info, true, NULL) > 0;
	if(!out->has_collision_box)
		return true;
	struct AABB ray;
	if(cell.type == BLOCK_CACTUS)
		aabb_setsize(&ray, 1.0F, 1.0F, 1.0F);
	else if(b->getBoundingBox(&info, false, NULL) == 1)
		b->getBoundingBox(&info, false, &ray);
	else return true;
	arrow_ray_box(out, &ray);
	return true;
}

// Beta AxisAlignedBB.expand(d,d,d): grow each face outward by d. EntityArrow.onUpdate
// expands every candidate target box by 0.3 (ARROW_HIT_EXPAND) before the swept-segment
// intercept, so the arrow hits a wider volume than the raw body box (parity fix 2026-06-19).
/* [740c583 AxisAlignedBB.func_1169_a:224-280, Vec3D:101-133]. Test the
 * six faces in Java order, with its per-axis epsilon and inclusive edges.
 * Keeping the original double segment matters after a block shortens it. */
static bool arrow_aabb_intersect_beta(const double from[3], const double to[3],
								  const double lo[3], const double hi[3],
								  double hit[3]) {
	bool found=false;
	double best=0.0;
	for(int face=0;face<6;face++) {
		int axis=face/2, a=(axis+1)%3, b=(axis+2)%3;
		double d=to[axis]-from[axis];
		if(d*d<(double)1.0E-7F)
			continue;
		double plane=(face&1)?hi[axis]:lo[axis];
		double t=(plane-from[axis])/d;
		if(t<0.0 || t>1.0)
			continue;
		double p[3];
		for(int i=0;i<3;i++) p[i]=from[i]+(to[i]-from[i])*t;
		if(p[a]<lo[a] || p[a]>hi[a] || p[b]<lo[b] || p[b]>hi[b])
			continue;
		double x=p[0]-from[0], y=p[1]-from[1], z=p[2]-from[2];
		double dist2=x*x+y*y+z*z;
		if(found && !(dist2<best))
			continue;
		found=true; best=dist2;
		for(int i=0;i<3;i++) hit[i]=p[i];
	}
	return found;
}

static double arrow_beta_distance(const double from[3], const double hit[3]) {
	double x=hit[0]-from[0],y=hit[1]-from[1],z=hit[2]-from[2];
	return (double)(float)sqrt(x*x+y*y+z*z); /* Vec3D.distanceTo */
}

static void arrow_box_from_aabb(const struct AABB* box,
								double lo[3], double hi[3]) {
	const double d=(double)ARROW_HIT_EXPAND;
	lo[0]=(double)box->x1-d; lo[1]=(double)box->y1-d;
	lo[2]=(double)box->z1-d;
	hi[0]=(double)box->x2+d; hi[1]=(double)box->y2+d;
	hi[2]=(double)box->z2+d;
}

// Ray-test the arrow segment (origin + t*dir, t in [0,max_dist]) against the PLAYER
// body box WITHOUT applying damage. Returns the entry distance in *out_t on a hit so
// the sweep can rank it against mob hits and damage only the CLOSEST. The owner-grace
// does not apply to the player (a mob never fires at "itself"; a player-fired arrow
// excludes the player via from_player at the sweep call site).
/* S1-a3 (Astra call #8 Q2, "projectile collision candidates"): this tests ONE
 * NAMED local player body. S1-a2 could only sweep the host because the damage
 * pipeline was host-singular; now that every step is actor-aware, the sweep
 * enumerates both bodies (arrow_sweep_actors below) and damages whichever the
 * segment reaches first. With no admitted guest the enumeration yields exactly
 * &s->player and this is the function it replaces, line for line. */
static bool arrow_test_actor(struct server_local* s,
							 const struct server_player* player,
							 const double from[3], const double to[3],
							 double* out_t) {
	double lo[3],hi[3],hit[3];

	(void)s;
	if(!player || !player->has_pos || !player->finished_loading)
		return false;
	/* NO health gate here, deliberately. Beta excludes a candidate on
	 * canBeCollidedWith() == !isDead [740c583 EntityArrow.onUpdate], and a
	 * player at health 0 is NOT isDead -- the corpse keeps ticking (this
	 * engine models exactly that; see test_death_flop_is_collidable_and_
	 * bounces_arrow). A corpse therefore stays a swept candidate and the HIT
	 * is what gets refused, downstream, by the flat-damage path health<=0
	 * guard, which reports accepted=false and bounces the arrow. That is one
	 * rule for both bodies, and it leaves the host path untouched. */

	/* Player network position is the eye; keep its double position when
	 * constructing Beta's expanded body box. */
	const double half=(double)(0.6F/2.0F), height=(double)1.8F;
	const double feet=player->y-(double)PLAYER_EYE_HEIGHT;
	const double expand=(double)ARROW_HIT_EXPAND;
	lo[0]=player->x-half-expand; hi[0]=player->x+half+expand;
	lo[1]=feet-expand; hi[1]=feet+height+expand;
	lo[2]=player->z-half-expand; hi[2]=player->z+half+expand;
	if(!arrow_aabb_intersect_beta(from,to,lo,hi,hit))
		return false;
	if(out_t)
		*out_t = arrow_beta_distance(from,hit);
	return true;
}

/* S1-a3: the CLOSEST local player body this sub-segment crosses, with the Beta
 * owner grace applied PER CANDIDATE. Beta compares the candidate against
 * `this.shootingEntity` [740c583 EntityArrow.onUpdate: `var9 !=
 * this.shootingEntity || this.ticksInAir >= 5`], so with two local bodies the
 * grace must be an identity compare against each one in turn -- the shooter
 * gets its five ticks, the other body is hittable from tick 0 (Beta: players
 * damage players). projectile_shooter_is_this_body keeps the second leg the
 * R-S1a2 F1 fix added: a mob/dispenser arrow has a NONZERO owner id and
 * therefore graces nobody. Candidate order is fixed (host, then guest) and
 * ties resolve to the FIRST candidate, i.e. the host -- the same shape the
 * pre-split single-candidate sweep had. */
static struct server_player* arrow_sweep_actors(struct server_local* s,
	const struct entity* e, const double from[3], const double to[3],
	double* out_t) {
	struct server_player* hit = NULL;
	double best_t = 0.0;
	/* Beta EntityArrow.java:120 queries this.worldObj for players too. */
	struct server_world* world = server_local_entity_world(s, e);
	if(!world)
		return NULL;
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* cand = server_local_damage_candidate(s, i);
		if(!cand || !server_local_actor_in_world(s, cand, world))
			continue;
		if(!arrow_should_hit_actor(
			   projectile_shooter_is_this_body(
				   e->data.arrow.owner_id,
				   actor_ident_equal(e->data.arrow.shooter_actor,
									 server_local_actor_ident(s, cand))),
			   e->data.arrow.age))
			continue;
		double t;
		if(!arrow_test_actor(s, cand, from, to, &t))
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

// Apply a MOB hit (mob 20-pt scale): subtract the flat Beta arrow damage, arm the
// mob's i-frames, latch aggro onto the player (retaliation), and apply arrow
// knockback along the flight direction. Mirrors the SRPC_ENTITY_ATTACK mob path in
// server_local.c (same i-frame + aggro + knockback bookkeeping), but with the
// arrow's flat damage instead of a melee tool value, and the death reap deferred to
// the mob's own health<=0 branch (entity_monster.c) -- which drops loot. The mob's
// resistance is respected: an equal hit inside hurt_resist_time is absorbed,
// matching Beta routing arrow damage through the same attackEntityFrom gate.
/* Mob motion units S2: the arrow's shootingEntity position for the Beta
 * knockBack -- the firing player (resolved by actor identity) or the firing
 * mob (by entity id, its authoritative double position). Dispenser arrows
 * and a departed shooter have none (Beta's null-attacker branch). */
static bool arrow_shooter_position(struct server_local* s, uint32_t arrow_id,
								   uint32_t owner_id,
								   bool from_player, struct actor_ident shooter,
								   double* x, double* z) {
	/* v4: a RELOADED arrow names itself as owner (server_local.c's archive
	 * restore); Beta never saves EntityArrow.owner, so after a reload it is
	 * null -- no shooter, no knockBack. */
	if(owner_id == arrow_id)
		return false;
	if(from_player) {
		/* A PLAYER shot carries owner id 0 plus the acting actor's identity
		 * (server_local.c's bow path). A from_player arrow with any other
		 * owner is not one: the dispenser's DISPENSER_PROJECTILE_OWNER_
		 * SENTINEL (Beta BlockDispenser builds EntityArrow(world, x, y, z):
		 * owner null, doesArrowBelongToPlayer true) -- no shooter. */
		if(owner_id != 0)
			return false;
		struct server_player* p = server_local_actor_resolve(s, shooter);
		if(!p)
			return false;
		*x = p->x;
		*z = p->z;
		return true;
	}
	struct entity** pp = dict_entity_get(s->entities, owner_id);
	if(!pp || !*pp)
		return false;
	const struct entity* o = *pp;
	*x = o->type == ENTITY_MONSTER ? o->data.monster.motion_d.pos[0]
								   : (double)o->pos[0];
	*z = o->type == ENTITY_MONSTER ? o->data.monster.motion_d.pos[2]
								   : (double)o->pos[2];
	return true;
}

/* v5 (ASTRA-V4-REVIEW arrow owner): EntityArrow.java:147 passes
 * this.owner to attackEntityFrom. Beta never saves EntityArrow.owner, and a
 * dispensed arrow is built with none (BlockDispenser -> EntityArrow(world,
 * x, y, z)), so `var1 instanceof EntityPlayer` (EntityPigZombie.java:49-60,
 * EntityMob / EntityWolf retaliation) holds ONLY for an arrow a player
 * actually fired in this session:
 *   - a player shot: owner id 0 + the acting actor's identity (the bow
 *     path) -> a player attacker;
 *   - a dispenser shot: from_player (pickable) but owner id
 *     DISPENSER_PROJECTILE_OWNER_SENTINEL -> null;
 *   - a reloaded arrow: owner id == its own id (the archive restore) -> null,
 *     whatever from_player says;
 *   - otherwise the mob shooter's id (a skeleton) -> a non-player attacker. */
enum arrow_owner_kind { ARROW_OWNER_NULL, ARROW_OWNER_PLAYER, ARROW_OWNER_MOB };
static enum arrow_owner_kind arrow_owner_kind(uint32_t arrow_id,
	uint32_t owner_id, bool from_player) {
	if(owner_id == arrow_id)
		return ARROW_OWNER_NULL;
	if(from_player)
		return owner_id == 0 ? ARROW_OWNER_PLAYER : ARROW_OWNER_NULL;
	return ARROW_OWNER_MOB;
}

static enum monster_damage_result arrow_damage_mob(
	struct entity* mob, struct server_local* s, uint32_t arrow_id,
	uint32_t owner_id,
	const vec3 dir, bool from_skeleton, bool from_player,
	struct actor_ident shooter) {
	const enum arrow_owner_kind owner
		= arrow_owner_kind(arrow_id, owner_id, from_player);
	const bool player_owner = owner == ARROW_OWNER_PLAYER;
	// v5 + PIGZ-PACK merge: the attacker-is-a-player test is decided by
	// arrow_owner_kind (EntityArrow.java:147 passes the owner): only a
	// genuine player owner; dispensed/reloaded (null) and skeleton owners
	// anger nobody and spend no draw.
	// PIG ZOMBIE PACK ANGER (PIGZ-PACK; decompile-verified):
	//     EntityArrow.onUpdate: the hit entity takes attackEntityFrom(owner, 4)
	//         [server EntityArrow.java:147] -- the SHOOTER, not the arrow
	//     EntityPigZombie.attackEntityFrom(attacker, damage)
	//         [server EntityPigZombie.java:49-64]: when the attacker is an
	//         EntityPlayer, every pig zombie in the box expanded by 32
	//         becomes angry at it, and so does the struck one;
	//         then the base attackEntityFrom result is returned.
	// So a PLAYER-fired arrow alerts the whole pack exactly like a melee hit;
	// a skeleton's arrow (owner = the skeleton) and a dispenser's (owner =
	// null) anger nobody. It runs BEFORE the base attackEntityFrom -- before the
	// i-frame/health gate (an ABSORBED hit and a death-flopping corpse, which
	// EntityLiving.canBeCollidedWith = !isDead keeps hittable, still anger)
	// and before the hurt/death pitch on the struck mob's own Random
	// [EntityLiving.java:293-345].
	if(player_owner && mob->data.monster.id == MONSTER_PIG_ZOMBIE)
		server_local_pig_zombie_pack_anger(s, mob, shooter);

	/* EntityWolf.attackEntityFrom:217-245: a null attacker is neither halved
	 * nor retaliated against (attacker id UINT32_MAX = no entity). */
	enum monster_damage_result result = entity_is_wolf(mob)
		? entity_wolf_attack_from(mob, s, arrow_hit_damage(false),
			owner == ARROW_OWNER_NULL ? UINT32_MAX
				: (player_owner ? 0 : owner_id),
			player_owner, true, from_skeleton, shooter)
		: entity_monster_apply_damage(
			mob, arrow_hit_damage(false), from_skeleton);

	if(result == MONSTER_DAMAGE_ABSORBED)
		return result;
	/* Cave's current target representation can name only a player. Hostiles hit
	 * by a player retaliate; passives never do, and a skeleton-arrow victim
	 * must not be falsely redirected to a player (true mob-vs-mob target state
	 * is a separate AI capability). S1-a2: the latch now records WHICH actor
	 * fired -- Beta sets playerToAttack to the arrow OWNER, not to "the player"
	 * [740c583 EntityMob.attackEntityFrom via EntityArrow.onUpdate passing
	 * this.shootingEntity as the damage source]. */
	/* v5: only a genuine player owner is a player target (EntityMob.
	 * attackEntityFrom:33-38 sets playerToAttack = the ATTACKER = the owner). */
	if(player_owner && !monster_is_passive(mob->data.monster.id)) {
		mob->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
		mob->data.monster.target_actor = shooter;
	}
	if(result != MONSTER_DAMAGE_FRESH)
		return result; // Beta var3=false: excess damage has no fresh-hit knockback

	// Arrow knockback (mob motion units S2, Q3): Beta's EntityArrow calls
	// entityHit.attackEntityFrom(this.shootingEntity, 4) [EntityArrow.java:163],
	// so a FRESH hit takes EntityLiving.knockBack from the SHOOTER's position
	// [EntityLiving.java:336-346, 392-405] -- the "v2 Wii-feel 0.18 shove along
	// the flight direction" is retired. No shooter position -> no knockBack
	// (Beta's null-attacker branch).
	double shooter_x, shooter_z;
	if(arrow_shooter_position(s, arrow_id, owner_id, from_player, shooter,
							  &shooter_x, &shooter_z)) {
		mob_motion_knockback(mob, &mob->data.monster.motion_d,
							 shooter_x - mob->data.monster.motion_d.pos[0],
							 shooter_z - mob->data.monster.motion_d.pos[2],
							 server_local_sound_rng());
		mob->data.monster.knockback_time = MOB_KNOCKBACK_TICKS;
	}
	(void)dir;
	return result;
}

static void arrow_bounce_from_rejected_hit(
	struct entity* arrow, const vec3 planned_travel) {
	arrow->data.arrow.age = 0;
	glm_vec3_copy(arrow->pos_old, arrow->pos);
	for(int axis = 0; axis < 3; axis++) {
		arrow->vel[axis]
			= planned_travel[axis] * -0.10000000149011612F;
		arrow->pos[axis] += arrow->vel[axis];
	}
}

static int arrow_membership_compare(
	int32_t ax, int32_t az, int32_t ay, uint64_t ao, bool ap, uint32_t aid,
	int32_t bx, int32_t bz, int32_t by, uint64_t bo, bool bp, uint32_t bid) {
#define ARROW_CMP(a, b) do { if((a) != (b)) return ((a) > (b)) - ((a) < (b)); } while(0)
	ARROW_CMP(ax, bx);
	ARROW_CMP(az, bz);
	ARROW_CMP(ay, by);
	ARROW_CMP(ao, bo);
	ARROW_CMP(ap, bp);
	ARROW_CMP(aid, bid);
#undef ARROW_CMP
	return 0;
}

static bool arrow_mob_precedes(const struct entity* a,
							   const struct entity* b) {
	return !b || arrow_membership_compare(
		a->chunk_coord_x, a->chunk_coord_z, a->chunk_coord_y,
		a->chunk_list_order, false, a->id,
		b->chunk_coord_x, b->chunk_coord_z, b->chunk_coord_y,
		b->chunk_list_order, false, b->id) < 0;
}

static bool arrow_player_precedes_mob(
	const struct server_local* s, const struct entity* mob) {
	return !mob || arrow_membership_compare(
		s->player.chunk_coord_x, s->player.chunk_coord_z,
		s->player.chunk_coord_y, s->player.chunk_list_order, true, 0,
		mob->chunk_coord_x, mob->chunk_coord_z, mob->chunk_coord_y,
		mob->chunk_list_order, false, mob->id) < 0;
}

// Sweep the arrow's travel segment against every MOB in the world, returning the
// CLOSEST mob hit within max_dist (or NULL). Excludes the owner during the owner-grace
// window (arrow_should_hit). Used for BOTH player-fired arrows (their only targets are
// mobs) and skeleton arrows (Beta infighting: a skeleton arrow can hit another mob).
static struct entity* arrow_sweep_mobs(struct server_local* s, struct entity* arrow,
									   const double from[3], const double to[3],
									   double* out_t) {
	struct entity* closest = NULL;
	double closest_t = 0.0;
	/* Beta EntityArrow.java:120 queries this.worldObj, not the session's
	 * shared dictionary. Resolve both lifetime/world stamps before selection. */
	struct server_world* world = server_local_entity_world(s, arrow);
	if(!world)
		return NULL;

	// TODO(perf): arrow-sweep broad-phase (spatial cull before the per-entity ray test).
	// Deferred -- the spawn cap already bounds N; premature for a 0.3ms tick.
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* cand = dict_entity_ref(it)->value;
		dict_entity_next(it);

		/* EntityLiving.canBeCollidedWith is !isDead: a health-zero death-flop
		 * body remains collidable until its terminal reap. attackEntityFrom then
		 * rejects the hit and the arrow bounces. Cave's immutable pending-blast
		 * owner is the one intentional non-Beta exclusion. */
		if(!cand || cand->type != ENTITY_MONSTER || cand->delay_destroy == 0
		   || cand->data.monster.explosion_pending
		   || server_local_entity_world(s, cand) != world)
			continue;
		if(cand->id == arrow->id)
			continue; // never the arrow itself
		// Owner-grace: the shooter is immune only while the arrow is young; any
		// other mob is a valid target from tick 0 (no friendly-fire exemption).
		if(!arrow_should_hit(arrow->data.arrow.owner_id, cand->id,
							 arrow->data.arrow.age))
			continue;

		// Beta widens the candidate box by 0.3 per axis before its six-face
		// intercept. getBoundingBox returns the world-positioned mob box.
		struct AABB mbox;
		if(cand->getBoundingBox && cand->getBoundingBox(cand, &mbox) > 0) {
			double lo[3],hi[3],hit[3];
			arrow_box_from_aabb(&mbox,lo,hi);
			if(arrow_aabb_intersect_beta(from,to,lo,hi,hit)) {
				double t_hit=arrow_beta_distance(from,hit);
				if(!closest || t_hit < closest_t
				   || (t_hit == closest_t
					   && arrow_mob_precedes(cand, closest))) {
					closest_t = t_hit;
					closest = cand;
				}
			}
		}
	}

	if(closest && out_t)
		*out_t = closest_t;
	return closest;
}

/* EntityArrow.setArrowHeading/onUpdate: float sqrt, double atan2, Java's
 * float PI widened to double, then float rotation. These observations never
 * feed velocity, collision, pickup or RNG. */
static void arrow_visual_direction(const vec3 velocity, vec2 rotation) {
	float horizontal = (float)sqrt((double)velocity[0] * velocity[0]
		+ (double)velocity[2] * velocity[2]);
	rotation[0] = (float)(atan2((double)velocity[0], (double)velocity[2])
		* 180.0 / (double)(float)M_PI);
	rotation[1] = (float)(atan2((double)velocity[1], (double)horizontal)
		* 180.0 / (double)(float)M_PI);
}

static void arrow_visual_archive_rotation(struct entity* e) {
	/* Arrow-only mirror for the existing archive's radians Rotation fields. */
	e->orient[0] = glm_rad(e->data.arrow.visual.rotation[0]);
	e->orient[1] = glm_rad(e->data.arrow.visual.rotation[1]);
}

void entity_arrow_visual_init(struct entity* e) {
	if(e->type != ENTITY_ARROW || (e->data.arrow.item_id
		&& e->data.arrow.item_id != ITEM_ARROW) || e->data.arrow.fireball)
		return;
	arrow_visual_direction(e->vel, e->data.arrow.visual.rotation);
	glm_vec2_copy(e->data.arrow.visual.rotation, e->data.arrow.visual.previous);
	e->data.arrow.visual.valid = true;
	arrow_visual_archive_rotation(e);
}

void entity_arrow_visual_restore(struct entity* e) {
	/* readFromNBT has no prior rotation: restore prev=current at this boundary.
	 * Historical embedded arrows with zero Rotation retain the legacy fallback. */
	if((e->orient[0] == 0.0F && e->orient[1] == 0.0F)
	   || !isfinite(e->orient[0]) || !isfinite(e->orient[1])
	   || fabsf(e->orient[0]) > glm_rad(720.0F)
	   || fabsf(e->orient[1]) > glm_rad(720.0F))
		return; /* hostile/old finite Rotation must not enter wrap loops */
	e->data.arrow.visual.rotation[0] = glm_deg(e->orient[0]);
	e->data.arrow.visual.rotation[1] = glm_deg(e->orient[1]);
	glm_vec2_copy(e->data.arrow.visual.rotation, e->data.arrow.visual.previous);
	e->data.arrow.visual.shake_ticks = e->data.arrow.in_ground
		? (uint8_t)e->data.arrow.shake_ticks : 0;
	e->data.arrow.visual.valid = true;
}

static void arrow_visual_flight(struct entity* e) {
	struct arrow_visual_state* v = &e->data.arrow.visual;
	if(!v->valid)
		return;
	arrow_visual_direction(e->vel, v->rotation);
	for(int axis = 0; axis < 2; axis++) {
		while(v->rotation[axis] - v->previous[axis] < -180.0F)
			v->previous[axis] -= 360.0F;
		while(v->rotation[axis] - v->previous[axis] >= 180.0F)
			v->previous[axis] += 360.0F;
		v->rotation[axis] = v->previous[axis]
			+ (v->rotation[axis] - v->previous[axis]) * 0.2F;
	}
	arrow_visual_archive_rotation(e);
}

static bool entity_client_tick(struct entity* e) {
	assert(e);

	if(e->data.arrow.visual.valid) {
		if(!e->data.arrow.visual_pending) {
			glm_vec2_copy(e->data.arrow.visual.rotation,
				e->data.arrow.visual.previous);
			if(e->data.arrow.visual.shake_ticks > 0)
				e->data.arrow.visual.shake_ticks--;
		}
		e->data.arrow.visual_pending = false;
	}
	glm_vec3_copy(e->pos, e->pos_old);

	if(glm_vec3_distance2(e->pos, e->network_pos) > glm_pow2(0.001f)) {
		e->vel[0] = e->network_pos[0] - e->pos[0];
		e->vel[1] = e->network_pos[1] - e->pos[1];
		e->vel[2] = e->network_pos[2] - e->pos[2];
		glm_vec3_lerp(e->pos, e->network_pos, 0.7f, e->pos);
	} else {
		glm_vec3_copy(e->network_pos, e->pos);
	}

	/* MP client-side embed drr (remote_session only -- SP arrows are server-
	 * simulated above and play theirs in entity_server_tick). Beta MP clients
	 * self-simulate the Packet23 arrow and its own onUpdate plays the embed
	 * drr; this engine's remote arrows are wire-driven, so the embed is the
	 * OBSERVED flew-then-went-still edge (arrow_client_settled, combat.h --
	 * the documented adaptation). Same formula as arrow_emit_drr: "random.drr"
	 * 1.0F, pitch 1.2F/(r*0.2F+0.9F), on the client cosmetic stream.
	 * in_ground latches the play (client copies never write it otherwise) and
	 * excuses this arrow from the destroy-time hit drr (client_interface.c).
	 * [740c583: NetClientHandler.handleVehicleSpawn; EntityArrow.onUpdate] */
	if(gstate.remote_session && !e->data.arrow.in_ground
	   && arrow_client_settled(glm_vec3_distance2(e->pos, e->pos_old),
							   &e->data.arrow.client_moved,
							   &e->data.arrow.client_still_ticks)) {
		e->data.arrow.in_ground = true;
		audio_play3d(SOUND_RANDOM_DRR,
					 (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 1.0F,
					 1.2F / (rand_gen_flt(audio_cosmetic_rng()) * 0.2F + 0.9F));
	}

	return false;
}

/* Stuck-arrow pickup, for EITHER actor (lane C #14).
 * Beta drives it from each living player's own scan, not from the arrow:
 * EntityPlayer.onLivingUpdate [EntityPlayer.java:196-205] -- only while
 * `health > 0` -- queries its OWN world for entities intersecting
 * boundingBox.expand(1.0D, 0.0D, 1.0D) and calls onCollideWithPlayer on each
 * non-dead one. EntityArrow.onCollideWithPlayer [EntityArrow.java:257-266]:
 * inGround && doesArrowBelongToPlayer && arrowShake <= 0 &&
 * addItemStackToInventory(new ItemStack(Item.arrow, 1)) -> random.pop,
 * onItemPickup, setEntityDead. doesArrowBelongToPlayer means "fired by A
 * player", so either actor collects either actor's arrow; a full inventory
 * refuses and the arrow stays. The arrow box is setSize(0.5F, 0.5F), yOffset
 * 0 [EntityArrow.java:20-34]; the player box 0.6 x 1.8 from posY - 1.62
 * [Entity.setPosition] (the same strict test as the item pickup,
 * entity_item.c entity_item_player_overlap). This replaces the host-only
 * 2-block sphere around (x, y - 0.6, z), which took arrows past the expanded
 * X/Z edge and missed ones at its top corners. The host is scanned first
 * (the item pickup order) so a shared spot goes to the host. */
static bool arrow_player_overlap(const struct entity* e,
								 const struct server_player* player) {
	struct AABB player_box;
	aabb_setsize_centered(&player_box, 0.6F, 1.8F, 0.6F);
	aabb_translate(&player_box, (float)player->x,
				   (float)player->y + 0.9F - 1.62F, (float)player->z);
	player_box.x1 -= 1.0F;
	player_box.x2 += 1.0F;
	player_box.z1 -= 1.0F;
	player_box.z2 += 1.0F;

	struct AABB arrow_box;
	entity_make_world_bbox(e, &arrow_box);
	return aabb_intersection_strict(&player_box, &arrow_box);
}

/* One actor's onCollideWithPlayer. `inv` is the inventory the actor's item
 * pickup already uses (host: active_inventory, guest: its own inventory). */
static bool arrow_collect_for(struct entity* e, struct server_local* s,
							  struct server_player* player,
							  struct inventory* inv, uint32_t collector_id) {
	if(player->health <= 0) // [EntityPlayer.java:196]
		return false;
	/* Each player scans only its own world: an arrow in another context is
	 * never visited (NULL = the actor has no live world this tick). The
	 * arrow's side resolves through its (slot, generation) stamp (A2-E M6):
	 * a retired generation reuses the slot storage, so the raw e->world
	 * pointer alone could match the successor world. */
	struct server_world* world = server_local_world_of(s, player);
	if(!world || world != server_local_entity_world(s, e))
		return false;
	if(!arrow_player_overlap(e, player))
		return false;
	if(!(inv && inv->logic && inv->logic->on_collect))
		return false;

	struct item_data arrow_item = {.id = ITEM_ARROW, .durability = 0, .count = 1};
	if(!inv->logic->on_collect(inv, player, &arrow_item))
		return false; // inventory full -> leave the arrow stuck

	// S2: the identical "random.pop" formula as the item pickup, on accept.
	// [740c583: EntityArrow.onCollideWithPlayer]
	{
		struct random_gen* g = server_local_sound_rng();
		server_local_emit_sound(
			SOUND_RANDOM_POP, (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 0.2F,
			((rand_gen_flt(g) - rand_gen_flt(g)) * 0.7F + 1.0F) * 2.0F);
	}
	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_PICKUP_ITEM,
		.payload.pickup_item.entity_id = e->id,
		.payload.pickup_item.collector_id = collector_id,
	});
	return true;
}

// Returns true iff an actor collected the arrow (-> remove it).
static bool arrow_try_pickup(struct entity* e, struct server_local* s) {
	if(!arrow_pickable(e->data.arrow.from_player, e->data.arrow.in_ground,
					   e->data.arrow.shake_ticks))
		return false;
	if(s->player.has_pos
	   && arrow_collect_for(e, s, &s->player, s->player.active_inventory, 0))
		return true;
	struct server_player* guest = s->guest_actor;
	if(guest && s->guest_gameplay && guest->finished_loading
	   && arrow_collect_for(e, s, guest, &guest->inventory,
							s->guest_entity_id))
		return true;
	return false;
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
	vec3 planned_travel, dir;

	assert(e && s);

	if(!e->data.arrow.visual.valid && !e->data.arrow.in_ground)
		entity_arrow_visual_init(e);
	if(e->data.arrow.visual.valid) {
		/* Entity.onEntityUpdate copies previous rotation; arrowShake decreases
		 * before BOTH ground and air branches. Pickup's existing counter stays
		 * untouched, including its existing early-return behavior on unembed. */
		glm_vec2_copy(e->data.arrow.visual.rotation, e->data.arrow.visual.previous);
		if(e->data.arrow.visual.previous[0] == 0.0F
		   && e->data.arrow.visual.previous[1] == 0.0F)
			entity_arrow_visual_init(e);
		if(e->data.arrow.visual.shake_ticks > 0)
			e->data.arrow.visual.shake_ticks--;
	}
	glm_vec3_copy(e->pos, e->pos_old);
	// Entity.onEntityUpdate:222-224 runs before EntityArrow.onUpdate's body.
	if(e->pos[1] < -64.0F)
		return true;

	// --- audit #17d: STUCK (in-ground) arrow -- frozen, pickable, 1200-tick despawn --
	if(e->data.arrow.in_ground) {
		struct block_data block;
		bool same_block = server_world_get_block((struct server_world*)e->world,
			e->data.arrow.stuck_block_x, e->data.arrow.stuck_block_y,
			e->data.arrow.stuck_block_z, &block)
			&& block.type == e->data.arrow.stuck_block_type
			&& block.metadata == e->data.arrow.stuck_block_meta;
		if(!same_block) {
			// CFR EntityArrow.java:128-130: own Random, X/Y/Z order.
			e->data.arrow.in_ground = false;
			e->vel[0] *= jrand_next_float(&e->gameplay_rng) * 0.2F;
			e->vel[1] *= jrand_next_float(&e->gameplay_rng) * 0.2F;
			e->vel[2] *= jrand_next_float(&e->gameplay_rng) * 0.2F;
			e->data.arrow.stuck_ticks = 0;
			e->data.arrow.age = 0;
			return false;
		}
		if(e->data.arrow.shake_ticks > 0)
			e->data.arrow.shake_ticks--; // Beta arrowShake countdown -> enables pickup
		e->data.arrow.stuck_ticks++;

		if(arrow_try_pickup(e, s))
			return true; // player collected it
		// Beta ticksInGround == 1200 -> despawn (in-air age is irrelevant now).
		if(arrow_should_despawn(true, e->data.arrow.stuck_ticks))
			return true;
		// Stay put. (No CRPC_ENTITY_MOVE: it hasn't moved; the client holds its pos.)
		return false;
	}

	glm_vec3_copy(e->vel, planned_travel); // this tick's full intended velocity

	// EntityArrow.onUpdate:124-125 increments air age without an age reaper.
	if(e->data.arrow.age < INT32_MAX)
		e->data.arrow.age++;

	/* [740c583 EntityArrow.onUpdate:124-188] One Beta point ray through
	 * block bounds. The entity segment ends at block hitVec. */
	double ray_from[3], ray_to[3];
	for(int i=0;i<3;i++) {
		ray_from[i]=(double)e->pos[i];
		ray_to[i]=ray_from[i]+(double)planned_travel[i];
	}
	struct block_ray_hit block_hit;
	bool hit_block=block_ray_trace(ray_from,ray_to,false,true,arrow_ray_cell,e,
		&block_hit,NULL);
	const double* seg_end=hit_block?block_hit.hit:ray_to;
	double delta[3],dist2=0.0;
	for(int i=0;i<3;i++) {
		delta[i]=seg_end[i]-ray_from[i];
		dist2+=delta[i]*delta[i];
	}
	bool bounced=false;
	if(dist2>0.0) {
		double dist=sqrt(dist2);
		for(int i=0;i<3;i++) dir[i]=(float)(delta[i]/dist);
		double t_player=0.0,t_mob=0.0;
		struct server_player* hit_actor=arrow_sweep_actors(
			s,e,ray_from,seg_end,&t_player);
		struct entity* mob=arrow_sweep_mobs(s,e,ray_from,seg_end,&t_mob);
		if(hit_actor && (!mob || t_player<t_mob
			|| (t_player==t_mob && arrow_player_precedes_mob(s,mob)))) {
			bool accepted=false;
			/* v4 (ASTRA-V3-REVIEW arrow): EntityArrow.java:147
			 * attackEntityFrom(this.owner, 4) -- the knockBack is measured
			 * from the shooter's CURRENT position (EntityLiving.java:321-332),
			 * never the flight heading; a null owner (dispenser, reloaded,
			 * departed) gets no knockBack. The skeleton's EntityMob scale
			 * stays the from_skeleton fire-time resolution. */
			double shooter_x=0.0,shooter_z=0.0;
			const bool has_shooter=arrow_shooter_position(s,e->id,
				e->data.arrow.owner_id,e->data.arrow.from_player,
				e->data.arrow.shooter_actor,&shooter_x,&shooter_z);
			(void)server_local_damage_actor_from_attacker(s,hit_actor,
				arrow_hit_damage(true),has_shooter,shooter_x,shooter_z,
				e->data.arrow.from_skeleton,&accepted);
			if(accepted) { arrow_emit_drr(e); return true; }
			arrow_bounce_from_rejected_hit(e,planned_travel);
			e->data.arrow.visual.rotation[0] += 180.0F;
			e->data.arrow.visual.previous[0] += 180.0F;
			bounced=true;
		} else if(mob) {
			enum monster_damage_result result=arrow_damage_mob(
				mob,s,e->id,e->data.arrow.owner_id,dir,
				e->data.arrow.from_skeleton,e->data.arrow.from_player,
				e->data.arrow.shooter_actor);
			if(result!=MONSTER_DAMAGE_ABSORBED) {
				arrow_emit_drr(e); return true;
			}
			arrow_bounce_from_rejected_hit(e,planned_travel);
			e->data.arrow.visual.rotation[0] += 180.0F;
			e->data.arrow.visual.previous[0] += 180.0F;
			bounced=true;
		}
	}
	if(!bounced && hit_block) {
		/* [740c583 EntityArrow.onUpdate:175-188,193-213]: exact hit cell,
		 * float-narrowed impact motion, backoff, then ordinary tail. */
		e->data.arrow.stuck_block_x=block_hit.x;
		e->data.arrow.stuck_block_y=block_hit.y;
		e->data.arrow.stuck_block_z=block_hit.z;
		struct block_data embedded;
		if(entity_get_block(e,block_hit.x,block_hit.y,block_hit.z,&embedded)) {
			e->data.arrow.stuck_block_type=embedded.type;
			e->data.arrow.stuck_block_meta=embedded.metadata;
		}
		for(int i=0;i<3;i++)
			e->vel[i]=(float)(block_hit.hit[i]-ray_from[i]);
		double len=sqrt((double)e->vel[0]*e->vel[0]
			+(double)e->vel[1]*e->vel[1]+(double)e->vel[2]*e->vel[2]);
		/* Degenerate zero-distance C safety exception: Beta would produce NaN
		 * here, but passing that into CaveWii's world-coordinate cast is UB. */
		if((float)len!=0.0F)
			for(int i=0;i<3;i++)
				e->pos[i]=(float)((double)e->pos[i]
					-(double)e->vel[i]/(double)(float)len*(double)0.05F);
		e->data.arrow.stuck_ticks=0;
		e->data.arrow.shake_ticks=ARROW_SHAKE_TICKS;
		e->data.arrow.visual.shake_ticks=ARROW_SHAKE_TICKS;
		e->data.arrow.in_ground=true;
		arrow_emit_drr(e);
	}
	if(!bounced)
		for(int i=0;i<3;i++) e->pos[i]+=e->vel[i];

	// S2 review fix: EntityArrow calls super.onUpdate(), so an arrow shot into
	// water splashes in Beta -- and at bow flight speed it is loud (the volume
	// is velocity-derived). Shared post-move edge; the in_ground path above
	// skips it (zero motion -> inaudible volume) and a same-tick water->embed
	// resolves as the embed (Beta's tick-start probe would still see air).
	// [740c583: EntityArrow.onUpdate:87 -> Entity.onEntityUpdate:205-232]
	server_local_splash_tick(s, e, &e->data.arrow.was_in_water);

	/* EntityArrow.onUpdate samples impact/rebound motion BEFORE drag. */
	arrow_visual_flight(e);

	// Integrate drag THEN gravity ONCE per tick (after the point-ray move).
	// Parity fix 2026-06-19: Beta EntityArrow.onUpdate applies drag first, then subtracts
	// gravity -- `motionX *= 0.99; motionY *= 0.99; motionZ *= 0.99; motionY -= 0.03`, so
	// motionY_next = motionY*0.99 - 0.03. The old gravity-then-drag order gave
	// (motionY - 0.03)*0.99 = motionY*0.99 - 0.0297 (1% under-gravity that compounds over
	// the flight). This is the same drag/gravity-order mistake the mob water step already
	// corrects (test_mob_water.c).
	/* [740c583 EntityArrow.onUpdate:200-218]: the drag factor starts at 0.99 and the
	 * gravity at 0.03; while in water the drag factor becomes 0.8; all three velocity
	 * components are scaled by the drag factor, then gravity is subtracted from the
	 * vertical one -- the 0.03 gravity is UNCHANGED in water; only the
	 * drag swaps to 0.8. Beta's isInWater is the bbox handleWaterMovement
	 * verdict; the arrow's own foot-cell probe is this engine's standard body_in_water
	 * approximation (the egg precedent, EntityEgg.onUpdate tail). */
	float drag = ARROW_AIR_DRAG;
	{
		struct block_data foot;
		if(server_world_get_block(block_world(s), (w_coord_t)floorf(e->pos[0]),
								  (w_coord_t)floorf(e->pos[1]),
								  (w_coord_t)floorf(e->pos[2]), &foot)
		   && (foot.type == BLOCK_WATER_FLOW
			   || foot.type == BLOCK_WATER_STILL))
			drag = ARROW_WATER_DRAG;
	}
	e->vel[0] *= drag;
	e->vel[1] *= drag;
	e->vel[2] *= drag;
	e->vel[1] -= ARROW_GRAVITY;

	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_ENTITY_MOVE,
		.payload.entity_move.entity_id = e->id,
		.payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]},
		.payload.entity_move.arrow_visual = e->data.arrow.visual
	});

	return false;
}

static void entity_render(struct entity* e, mat4 view, float tick_delta) {
	// Local true arrows use the authoritative Beta presentation sample; remote
	// arrows without it and the legacy item fallback retain the existing
	// displacement/velocity-derived flight orientation. (The
	// render_item entity path leaving the GX CLR0 descriptor in DIRECT mode used
	// to hang the Wii -- fixed in render_item.c; this is the first item-entity in
	// v2 so it surfaced that latent bug.)
	struct item_data projectile_item = {
		.id = e->data.arrow.item_id ? e->data.arrow.item_id : ITEM_ARROW,
		.durability = 0,
		.count = 1,
	};
	struct item* it = item_get(&projectile_item);
	struct block_data blk;
	vec3 pos_lerp, delta;
	mat4 model, mv;

	if(!it)
		return;

	glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos_lerp);
	delta[0] = e->pos[0] - e->pos_old[0];
	delta[1] = e->pos[1] - e->pos_old[1];
	delta[2] = e->pos[2] - e->pos_old[2];
	if(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]
	   < glm_pow2(0.0001f)) {
		delta[0] = e->vel[0];
		delta[1] = e->vel[1];
		delta[2] = e->vel[2];
	}

	float yaw = atan2f(delta[0], delta[2]);
	float horiz = sqrtf(delta[0] * delta[0] + delta[2] * delta[2]);
	float pitch = -atan2f(delta[1], horiz > 0.001f ? horiz : 0.001f);

	/* Beta RenderArrow.renderArrow: two opposite endcaps and four crossed
	 * shaft faces, scale0.05625, translateX-4 after axial roll45. Vertex/UV
	 * pairs are reversed from Java's GL order for CaveWii's clockwise faces.
	 * Apply the optional authoritative interpolated rotation and embed shake
	 * without mutating entity/tick state; missing samples keep the fallback. */
	if(projectile_item.id == ITEM_ARROW && !e->data.arrow.fireball
	   && texture_arrow.data) {
		static const float xyz[72] = {
			-7,2,-2, -7,2,2, -7,-2,2, -7,-2,-2,
			-7,-2,-2, -7,-2,2, -7,2,2, -7,2,-2,
			-8,0,2, 8,0,2, 8,0,-2, -8,0,-2,
			-8,-2,0, 8,-2,0, 8,2,0, -8,2,0,
			-8,0,-2, 8,0,-2, 8,0,2, -8,0,2,
			-8,2,0, 8,2,0, 8,-2,0, -8,-2,0,
		};
		static const float uv[48] = {
			0,10.0f/32, 5.0f/32,10.0f/32, 5.0f/32,5.0f/32, 0,5.0f/32,
			0,10.0f/32, 5.0f/32,10.0f/32, 5.0f/32,5.0f/32, 0,5.0f/32,
			0,5.0f/32, .5f,5.0f/32, .5f,0, 0,0,
			0,5.0f/32, .5f,5.0f/32, .5f,0, 0,0,
			0,5.0f/32, .5f,5.0f/32, .5f,0, 0,0,
			0,5.0f/32, .5f,5.0f/32, .5f,0, 0,0,
		};
		struct block_data light_block = {0};
		entity_get_block(e, floorf(pos_lerp[0]), floorf(pos_lerp[1]),
					 floorf(pos_lerp[2]), &light_block);
		uint8_t light = (uint8_t)roundf(gfx_lookup_light(
			(light_block.torch_light << 4) | light_block.sky_light) * 255.0f);
		uint8_t rgba[96];
		for(int k = 0; k < 24; k++) {
			rgba[k*4] = rgba[k*4+1] = rgba[k*4+2] = light;
			rgba[k*4+3] = 255;
		}
		const struct arrow_visual_state* visual = &e->data.arrow.visual;
		if(visual->valid) {
			/* RenderArrow: suppress an uninitialized previous rotation. */
			if(visual->previous[0] == 0.0F && visual->previous[1] == 0.0F)
				return;
			yaw = glm_rad(visual->previous[0]
				+ (visual->rotation[0] - visual->previous[0]) * tick_delta);
			pitch = -glm_rad(visual->previous[1]
				+ (visual->rotation[1] - visual->previous[1]) * tick_delta);
		}
		glm_translate_make(model, pos_lerp);
		glm_rotate_y(model, yaw - glm_rad(90.0f), model);
		glm_rotate_z(model, -pitch, model);
		float shake = (float)visual->shake_ticks - tick_delta;
		if(visual->valid && shake > 0.0F)
			glm_rotate_z(model, glm_rad(-mh_sin(shake * 3.0F) * shake), model);
		glm_rotate_x(model, glm_rad(45.0f), model);
		glm_scale_uni(model, 0.05625f);
		glm_translate(model, (vec3) {-4.0f, 0.0f, 0.0f});
		glm_mat4_mul(view, model, mv);
		gfx_matrix_modelview(mv);
		gfx_bind_texture(&texture_arrow);
		gfx_lighting(false);
		gfx_draw_quads_flt(24, xyz, rgba, uv);
		gfx_lighting(true); /* Restore indexed GX CLR0 for the entity pass. */
		gfx_matrix_modelview(GLM_MAT4_IDENTITY);
		gfx_bind_texture(&texture_terrain);
		return;
	}

	entity_get_block(e, floorf(pos_lerp[0]), floorf(pos_lerp[1]),
					 floorf(pos_lerp[2]), &blk);
	render_item_update_light((blk.torch_light << 4) | blk.sky_light);

	glm_translate_make(model, pos_lerp);
	glm_rotate_y(model, yaw, model);
	glm_rotate_x(model, pitch, model);
	glm_scale_uni(model, 0.12f);
	glm_translate(model, (vec3) {-0.5f, -0.5f, -0.5f});
	glm_mat4_mul(view, model, mv);

	it->renderItem(it, &projectile_item, mv, false, R_ITEM_ENV_ENTITY);
}

/* [740c583 RenderSnowball.doRender / RenderManager]: eggs and snowballs use
 * their own item icon on a half-block camera-facing quad. ENTITY_ARROW is only
 * their client transport type; actual arrows retain the flight-oriented path. */
void entity_thrown_render(struct entity* e, struct camera* c,
						  float tick_delta) {
	struct item* icon = item_get(&(struct item_data) {
		.id = e->data.arrow.item_id
	});
	if(!icon)
		return;
	vec3 pos, center;
	glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos);
	glm_mat4_mulv3(c->view, pos, 1.0f, center);
	uint8_t sx = icon->render_data.item.texture_x * 16;
	uint8_t sy = icon->render_data.item.texture_y * 16;
	float u0 = sx / 256.0f, u1 = (sx + 16) / 256.0f;
	float v0 = sy / 256.0f, v1 = (sy + 16) / 256.0f;
	float xyz[12] = {
		center[0]+0.25f, center[1]-0.125f, center[2],
		center[0]-0.25f, center[1]-0.125f, center[2],
		center[0]-0.25f, center[1]+0.375f, center[2],
		center[0]+0.25f, center[1]+0.375f, center[2],
	};
	float uv[8] = {u1,v1, u0,v1, u0,v0, u1,v0};
	struct block_data blk;
	entity_get_block(e, floorf(pos[0]), floorf(pos[1]), floorf(pos[2]), &blk);
	uint8_t light = (uint8_t)roundf(gfx_lookup_light(
		(blk.torch_light << 4) | blk.sky_light) * 255.0f);
	uint8_t rgba[16];
	for(int i = 0; i < 4; i++) {
		rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = light;
		rgba[i*4+3] = 255;
	}
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_bind_texture(&texture_items);
	gfx_lighting(false);
	gfx_draw_quads_flt(4, xyz, rgba, uv);
	gfx_lighting(true);
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_bind_texture(&texture_terrain);
}

/* [740c583 RenderFireball.func_4012_a] snowball icon, two-block camera-facing
 * quad, local Y -0.25..0.75 before the 2x scale. This is only called for an
 * explicit fireball spawn kind; real snowballs use entity_thrown_render. */
void entity_fireball_render(struct entity* e, struct camera* c,
						   float tick_delta) {
	struct item* snowball = item_get(&(struct item_data) {.id = ITEM_SNOW_BALL});
	if(!snowball)
		return;
	vec3 pos, center;
	glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos);
	glm_mat4_mulv3(c->view, pos, 1.0f, center);
	uint8_t sx = snowball->render_data.item.texture_x * 16;
	uint8_t sy = snowball->render_data.item.texture_y * 16;
	float u0 = sx / 256.0f, u1 = (sx + 16) / 256.0f;
	float v0 = sy / 256.0f, v1 = (sy + 16) / 256.0f;
	float xyz[12] = {
		center[0]+1.0f, center[1]-0.5f, center[2],
		center[0]-1.0f, center[1]-0.5f, center[2],
		center[0]-1.0f, center[1]+1.5f, center[2],
		center[0]+1.0f, center[1]+1.5f, center[2],
	};
	float uv[8] = {u1,v1, u0,v1, u0,v0, u1,v0};
	struct block_data blk;
	entity_get_block(e, floorf(pos[0]), floorf(pos[1]), floorf(pos[2]), &blk);
	uint8_t light = (uint8_t)roundf(gfx_lookup_light(
		(blk.torch_light << 4) | blk.sky_light) * 255.0f);
	uint8_t rgba[16];
	for(int i = 0; i < 4; i++) {
		rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = light;
		rgba[i*4+3] = 255;
	}
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_bind_texture(&texture_items);
	gfx_lighting(false); /* gfx_draw_quads_flt uses direct GX CLR0. */
	gfx_draw_quads_flt(4, xyz, rgba, uv);
	gfx_lighting(true); /* Restore the entity pass's indexed CLR0 descriptor. */
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_bind_texture(&texture_terrain);
}

static size_t getBoundingBox_arrow(const struct entity* e, struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

void entity_arrow(uint32_t id, struct entity* e, bool server, void* world,
				  uint32_t owner_id, uint16_t item_id, int damage,
				  bool from_player) {
	assert(e && world);

	entity_default_init(e, server, world);
	entity_set_size(e, ARROW_BBOX_SIZE, ARROW_BBOX_SIZE, 0.0F);
	e->id = id;
	if(server) {
		struct server_world* w = world;
		entity_lifetime_rng_seed(&e->gameplay_rng, w->seed, w->dimension, id);
	}
	e->type = ENTITY_ARROW;
	e->name = "Arrow";
	e->tick_server = entity_server_tick;
	e->tick_client = entity_client_tick;
	e->render = entity_render;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = getBoundingBox_arrow;
	e->onLeftClick = NULL;
	e->leftClickText = NULL;
	e->onRightClick = NULL;
	e->rightClickText = NULL;
	e->delay_destroy = -1;
	/* entity_default_init leaves the shared `health` short alone and
	 * server_entity_malloc does not zero, so an arrow (Beta EntityArrow has no
	 * health) carried heap garbage into the common NBT 'Health' write and any
	 * state hash; 0 matches the painting placeholder (entity_painting.c). */
	e->health = 0;
	e->data.arrow.owner_id = owner_id;
	e->data.arrow.age = 0;
	e->data.arrow.item_id = item_id;
	e->data.arrow.fireball = false;
	e->data.arrow.damage = (int16_t)damage;
	e->data.arrow.from_player = from_player;
	/* S1-a2: stamped by the flush from the fire-time queue entry, like
	 * from_skeleton below; the host zero value is the safe default. */
	e->data.arrow.shooter_actor = ACTOR_IDENT_HOST;
	e->data.arrow.from_skeleton = false; // stamped by the flush from the fire-time
	                                     // resolution (server arrows); client copies
	                                     // never read it
	e->data.arrow.in_ground = false;   // audit #17d: starts in flight
	e->data.arrow.stuck_ticks = 0;
	e->data.arrow.shake_ticks = 0;
	e->data.arrow.was_in_water = true; // isFirstUpdate guard: fired FROM
	                                   // underwater never splashes at spawn
	e->data.arrow.client_moved = false;      // MP embed observation state
	e->data.arrow.client_still_ticks = 0;    // (client copies only)
	e->data.arrow.visual = (struct arrow_visual_state) {0};
	e->data.arrow.visual_pending = false;
	/* MP drr scope: only a REAL arrow thunks -- Beta snowball/egg/fireball
	 * impacts play no drr (EntitySnowball/EntityEgg poof silently; the
	 * fireball's blast arrives as the explosion CRPC) [740c583
	 * EntitySnowball.onUpdate / EntityEgg.onUpdate]. Client copies of the
	 * other projectiles are born with the played-latch set so neither MP drr
	 * path (settle tick / destroy observation) can ever fire for them. */
	if(!server && item_id != ITEM_ARROW)
		e->data.arrow.in_ground = true;
	glm_vec3_copy(e->pos, e->network_pos);
}
