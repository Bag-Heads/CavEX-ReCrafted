/*
	Pins the Beta 1.7.3 PLAYER bow + the arrow entity-sweep / stick / pickup /
	despawn-split PURE bits (audit #17c / #17d), binding the SHARED
	source/entity/combat.h so the values + decisions here are the exact ones the
	bow input path (server_local.c), the skeleton AI (entity_monster.c), and the
	arrow tick (entity_arrow.c) use.

	CRITICAL Beta-version note (pinned here so it can't be "fixed" back to the
	wrong version): Beta 1.7.3's bow is the OLD INSTANT-FIRE bow -- ItemBow has no
	draw/charge mechanic. The (f*f+f*2)/3 charge curve, the 0.1 min-charge gate,
	charge-scaled velocity, and velocity-scaled ceil(velocity*base) damage with
	crits were ALL added in Beta 1.8 / Release 1.0 and are UNFAITHFUL to 1.7.3.
	So: launch speed is a FIXED multiplier (bow 1.5, skeleton 0.6), on-hit damage
	is a flat value (player-scale 32 vs mob-scale 4), and there is no min-charge.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "entity/combat.h"
#include "entity/actor_ident.h" /* S1-a3: per-candidate grace identities */

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

static void test_constants(void) {
	// Faithful b1.7.3 launch speeds: bow 1.5, skeleton 0.6 (bow is FASTER, not
	// higher-damage). The bow speed must outpace the skeleton's so the player's
	// arrow flies flatter/farther -- the only faithful advantage in 1.7.3.
	assert(feq(ARROW_SPEED_BOW, 1.5f));
	assert(ARROW_SPEED_BOW > ARROW_SPEED);
	// Flat mob-scale damage = 4 (Beta attackEntityFrom(owner, 4)). NOT *8 -- that
	// *8 is only the PLAYER-scale ARROW_DAMAGE (a skeleton arrow hitting the
	// player). 4 half-hearts on a 20-pt mob = 2 hearts.
	assert(ARROW_DAMAGE_BOW_BASE == 4);
	assert(ARROW_DAMAGE == 32);
	assert(ARROW_DAMAGE == ARROW_DAMAGE_BOW_BASE * COMBAT_HEALTH_PER_HEART / 2);
	// Stick + despawn (audit #17d).
	assert(ARROW_STUCK_DESPAWN_TICKS == 1200); // Beta ticksInGround == 1200 (60s)
	assert(ARROW_SHAKE_TICKS == 7);            // Beta arrowShake = 7 on stick
	assert(ARROW_OWNER_GRACE_TICKS == 5);      // Beta owner immune while ticksInAir < 5
	// b1.7.3 has NO minimum draw (instant fire); the charge gate is effectively a
	// pass-through. A charge curve / 0.1 min-charge would be the 1.8 bow.
	assert(feq(ARROW_BOW_MIN_CHARGE, 0.0f));
	// Aim-assist (Wii playability, NOT Beta): a cone cos in (0,1) and a positive
	// range. Documented carve-out -- just sanity-bound it here.
	assert(ARROW_AIM_ASSIST_CONE_COS > 0.0f && ARROW_AIM_ASSIST_CONE_COS < 1.0f);
	assert(ARROW_AIM_ASSIST_RANGE > 0.0f);
	assert(feq(ARROW_AIM_ASSIST_RANGE_SQ,
			   ARROW_AIM_ASSIST_RANGE * ARROW_AIM_ASSIST_RANGE));
}

static void test_launch_speed(void) {
	// from_player == true -> the bow's 1.5; false -> the skeleton's 0.6. (Charge
	// is irrelevant in 1.7.3 -- a fixed multiplier per shooter.)
	assert(feq(arrow_launch_speed(true), ARROW_SPEED_BOW));
	assert(feq(arrow_launch_speed(false), ARROW_SPEED));
}

static void test_hit_damage_scales(void) {
	// The scale is chosen by the VICTIM, not by from_player: a hit on the PLAYER
	// is player-scale (32); a hit on a MOB is mob-scale (4). This is the doctrine's
	// #1 hazard -- feeding 32 to a 20-pt mob would one-shot everything.
	assert(arrow_hit_damage(true) == ARROW_DAMAGE);        // player victim -> 32
	assert(arrow_hit_damage(false) == ARROW_DAMAGE_BOW_BASE); // mob victim -> 4
	assert(arrow_hit_damage(false) != ARROW_DAMAGE); // the scales never cross
}

static void test_charge_fires(void) {
	// Instant-fire bow: any press fires (charge mapped to 1.0 by the control
	// layer). The min-charge is 0, so even a 0 charge "fires" at the curve level
	// -- the real gate is owning an arrow (consume), enforced at the call site.
	assert(arrow_charge_fires(1.0f));
	assert(arrow_charge_fires(0.5f));
	assert(arrow_charge_fires(0.0f));
	// A negative charge (shouldn't happen) does NOT fire -- defensive lower bound.
	assert(!arrow_charge_fires(-0.1f));
}

static void test_owner_grace(void) {
	const uint32_t owner = 42, mob = 7;
	// A NON-owner (a mob) is hittable from tick 0, before AND after the grace.
	assert(arrow_should_hit(owner, mob, 0));
	assert(arrow_should_hit(owner, mob, ARROW_OWNER_GRACE_TICKS - 1));
	assert(arrow_should_hit(owner, mob, ARROW_OWNER_GRACE_TICKS + 10));
	// The OWNER is immune for the first ARROW_OWNER_GRACE_TICKS ticks, then hittable
	// (Beta: var9 != owner || ticksInAir >= 5).
	assert(!arrow_should_hit(owner, owner, 0));
	assert(!arrow_should_hit(owner, owner, ARROW_OWNER_GRACE_TICKS - 1));
	assert(arrow_should_hit(owner, owner, ARROW_OWNER_GRACE_TICKS));     // exactly 5
	assert(arrow_should_hit(owner, owner, ARROW_OWNER_GRACE_TICKS + 1));
	// NO friendly-fire exemption: a skeleton's arrow hits another mob -- the helper
	// only knows ids, so any other-id is a target (infighting). (Verified above via
	// the non-owner mob being hittable; from_player is NOT consulted here.)
}

/* R-S1a2 F1: WHO the owner grace belongs to. Beta excludes the shooting
 * ENTITY only -- EntityArrow.onUpdate skips a candidate iff it IS
 * this.shootingEntity and ticksInAir < 5 [740c583; cross-checked in
 * ../McBetaCpp src/net/minecraft/src/EntityArrow.cpp:161]. Both local player
 * bodies fire with owner id 0 and are told apart by their actor identity, so
 * the predicate needs BOTH legs: a mob or dispenser arrow carries a NONZERO
 * owner id (and the zero/host identity default), and must never be graced
 * against the player it is flying at. */
static void test_shooter_grace_is_the_shooter_only(void) {
	/* The HOST body against its own arrow: owner 0 + host identity. */
	assert(projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER, true));
	assert(!arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER, true), 0));
	assert(!arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER, true),
		ARROW_OWNER_GRACE_TICKS - 1));
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER, true),
		ARROW_OWNER_GRACE_TICKS));
	/* A GUEST-fired arrow swept against the HOST body: owner 0, but the
	 * identity says the host is not the shooter -- hittable from tick 0
	 * (Beta: players damage players). */
	assert(!projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER, false));
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER, false), 0));
	/* A SKELETON arrow (owner = the skeleton entity id) and a DISPENSED one
	 * (the no-owner sentinel) carry the HOST identity by default, and must
	 * still hit at tick 0 -- the regression this pins. */
	assert(!projectile_shooter_is_this_body(777u, true));
	assert(arrow_should_hit_actor(projectile_shooter_is_this_body(777u, true), 0));
	assert(!projectile_shooter_is_this_body(UINT32_MAX, true));
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(UINT32_MAX, true), 0));
}
static void test_pickable_rule(void) {
	// Player-fired (from_player=true), stuck (in_ground), shake fully decayed -> pick.
	assert(arrow_pickable(true, true, 0));
	// Skeleton-fired arrows are NEVER pickable, even stuck + shake-0.
	assert(!arrow_pickable(false, true, 0));
	// Not yet stuck (still in air) -> no pickup even if player-fired.
	assert(!arrow_pickable(true, false, 0));
	// Stuck but arrowShake still counting down (> 0) -> no pickup yet (Beta gate).
	assert(!arrow_pickable(true, true, ARROW_SHAKE_TICKS));
	assert(!arrow_pickable(true, true, 1));
}

static void test_despawn_split(void) {
	// EntityArrow.java:107-125: air age never reaps an arrow.
	assert(!arrow_should_despawn(false, 0));
	assert(!arrow_should_despawn(false, 2000));
	// EntityArrow.java:111-114: in-ground timeout is exactly 1200 ticks.
	assert(!arrow_should_despawn(true, ARROW_STUCK_DESPAWN_TICKS - 1));
	assert(arrow_should_despawn(true, ARROW_STUCK_DESPAWN_TICKS));
	assert(!arrow_should_despawn(true, 5));
}

// Integrate the bow arrow's flight a few ticks and confirm it is FASTER (covers more
// ground in the same time) AND FLATTER (less vertical drop PER BLOCK travelled) than a
// skeleton arrow -- the faithful 1.5-vs-0.6 advantage -- using the engine's per-tick
// model (vel.y -= gravity; vel *= drag). NOTE: with a level (vy=0) launch both drop the
// SAME amount per TICK (gravity is independent of horizontal speed), so the meaningful
// flatness metric is drop PER HORIZONTAL DISTANCE, which the faster bow arrow minimizes.
static void test_bow_flies_flatter(void) {
	float bx = ARROW_SPEED_BOW, bvy = 0.0f, by = 0.0f, bdist = 0.0f;
	float sx = ARROW_SPEED, svy = 0.0f, sy = 0.0f, sdist = 0.0f;
	for(int t = 0; t < 20; t++) {
		// drag THEN gravity (Beta EntityArrow.onUpdate order; parity fix 2026-06-19)
		bdist += bx; by += bvy; bx *= ARROW_AIR_DRAG; bvy *= ARROW_AIR_DRAG;
		bvy -= ARROW_GRAVITY;
		sdist += sx; sy += svy; sx *= ARROW_AIR_DRAG; svy *= ARROW_AIR_DRAG;
		svy -= ARROW_GRAVITY;
	}
	assert(bdist > sdist);          // bow arrow travels much farther in the same time
	assert(by < 0.0f && sy < 0.0f); // both still fall (gravity wins for both)
	// Drop PER BLOCK is far smaller for the faster bow arrow -> flatter, easier to aim.
	assert(fabsf(by) / bdist < fabsf(sy) / sdist);
}

// Anti-tunneling substep split (audit #17c fix): a per-tick arrow displacement is
// split into sub-steps no longer than ARROW_MAX_SUBSTEP_DIST, so the endpoint-only
// entity_try_move cannot skip a thin wall lying between the tick's start and end. This
// pins (a) every sub-step is <= the substep cap, (b) the cap is < a 1-block wall so a
// wall can never fit fully between two consecutive sub-step boxes, and (c) the concrete
// bow (1.5 -> 3 steps) / skeleton (0.6 -> 2 steps) / tiny-move (-> 1 step) cases.
static void test_move_substeps(void) {
	// The cap must be strictly less than one block so a 1-thick wall can't hide
	// between two adjacent sub-step positions (the whole point of the fix).
	assert(ARROW_MAX_SUBSTEP_DIST < 1.0f);
	// A zero / tiny move still takes at least one step (never 0 -> no div-by-zero at
	// the call site, which divides the velocity by the step count).
	assert(arrow_move_substeps(0.0f) == 1);
	assert(arrow_move_substeps(ARROW_MAX_SUBSTEP_DIST) == 1); // exactly the cap -> 1
	assert(arrow_move_substeps(ARROW_MAX_SUBSTEP_DIST * 0.5f) == 1);
	// The faithful launch speeds: bow 1.5 -> ceil(1.5/0.5)=3, skeleton 0.6 -> ceil(0.6/0.5)=2.
	assert(arrow_move_substeps(ARROW_SPEED_BOW) == 3);
	assert(arrow_move_substeps(ARROW_SPEED) == 2);
	// And the guarantee itself: for a range of per-tick travels, EVERY resulting
	// sub-step length is <= the cap (so no sub-move can tunnel a thin wall).
	for(float len = 0.0f; len <= 3.0f; len += 0.05f) {
		int n = arrow_move_substeps(len);
		assert(n >= 1);
		assert(len / (float)n <= ARROW_MAX_SUBSTEP_DIST + 1.0e-4f);
	}
}

/* S1-a3: with two local bodies the grace is decided PER CANDIDATE, so the
 * predicate has to be asked once per body with THAT body identity. Beta
 * compares the candidate against `this.shootingEntity` [740c583
 * EntityArrow.onUpdate: `var9 != this.shootingEntity || this.ticksInAir >= 5`],
 * which is exactly a per-candidate reference compare -- the sweeps in
 * entity_arrow.c / entity_egg.c / entity_fish.c now loop it, and this pins the
 * truth table they loop over. */
static void test_grace_is_per_candidate(void) {
	const struct actor_ident host = ACTOR_IDENT_HOST;
	const struct actor_ident guest
		= {.slot = ACTOR_SLOT_GUEST, .generation = 41};
	const struct actor_ident rejoined
		= {.slot = ACTOR_SLOT_GUEST, .generation = 42};
	/* A GUEST-fired arrow, asked about the HOST body: not the shooter, so no
	 * grace -- hittable from tick 0 (Beta: players damage players). */
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER,
										actor_ident_equal(guest, host)),
		0));
	/* ...and asked about the GUEST body: it IS the shooter, so the five-tick
	 * self-immunity applies to it and to nobody else. */
	assert(!arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER,
										actor_ident_equal(guest, guest)),
		0));
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER,
										actor_ident_equal(guest, guest)),
		ARROW_OWNER_GRACE_TICKS));
	/* A REJOINED guest is a different body: the old role must not grace it. */
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER,
										actor_ident_equal(guest, rejoined)),
		0));
	/* The host-fired case is unchanged in both directions. */
	assert(!arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER,
										actor_ident_equal(host, host)),
		0));
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(PROJECTILE_OWNER_LOCAL_PLAYER,
										actor_ident_equal(host, guest)),
		0));
	/* R-S1a2 F1 stays closed: a MOB/DISPENSER arrow carries the zero (= host)
	 * identity default with a NONZERO owner id, so it graces NO body, not even
	 * the one whose identity it happens to match. */
	assert(arrow_should_hit_actor(
		projectile_shooter_is_this_body(777u, actor_ident_equal(host, host)),
		0));
}

int main(void) {
	test_constants();
	test_launch_speed();
	test_hit_damage_scales();
	test_charge_fires();
	test_owner_grace();
	test_shooter_grace_is_the_shooter_only();
	test_grace_is_per_candidate();
	test_pickable_rule();
	test_despawn_split();
	test_bow_flies_flatter();
	test_move_substeps();
	printf("test_bow_arrow: bow=%.1f/skel=%.1f speed, dmg player=%d/mob=%d, "
		   "owner-grace=%d, shake=%d, stuck-despawn=%d ticks (b1.7.3 INSTANT-fire, "
		   "no charge)\n",
		   ARROW_SPEED_BOW, ARROW_SPEED, ARROW_DAMAGE, ARROW_DAMAGE_BOW_BASE,
		   ARROW_OWNER_GRACE_TICKS, ARROW_SHAKE_TICKS, ARROW_STUCK_DESPAWN_TICKS);
	return 0;
}
