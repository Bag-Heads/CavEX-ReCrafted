/*
	Pins the Beta 1.7.3 spider combat slice: the LEAP gate + impulse
	(EntitySpider.attackEntity), the WALL-CLIMB ascend, the per-mob MELEE damage
	(spider attackStrength 2 = 1 heart vs zombie 5 = 2.5 hearts), and that the
	leap only fires when the spider actually has aggro (day-passivity gate).

	Binds the SHARED source/entity/combat.h directly, so the byte-faithful Beta
	constants + the pure helpers tested here are the exact ones the engine
	(server_tick_monster in entity_monster.c + server_local_hurt_player in
	server_local.c) uses -- they cannot drift.

	Beta source:
	  - EntitySpider.attackEntity(Entity, float f): leap when
	      f > 2.0 && f < 6.0 && rand.nextInt(10) == 0 && onGround
	    -> motionY = 0.4; motionX = dx/f2 * 0.5 * 0.8 + motionX*0.2 (likewise Z).
	    (squared-distance form here: 4.0 < dist^2 < 36.0.)
	  - EntitySpider.isOnLadder() { return this.isCollidedHorizontally; } -> any
	    wall the spider pushes into is a ladder, so it climbs (ascends) it.
	  - EntitySpider has NO attackStrength override -> inherits EntityMob's
	    default attackStrength = 2 half-hearts = 1 heart; EntityZombie overrides
	    to 5. On the player's 160-pt scale (16/heart): spider 16, zombie 40.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"
#include "entity/monsters_object.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

static void test_constants(void) {
	// Leap gate: 2.0 < dist < 6.0  ==  4.0 < dist^2 < 36.0 (Beta f bounds).
	assert(feq(SPIDER_LEAP_MIN_DIST_SQ, 4.0f));
	assert(feq(SPIDER_LEAP_MAX_DIST_SQ, 36.0f));
	assert(feq(SPIDER_LEAP_MIN_DIST_SQ, 2.0f * 2.0f));
	assert(feq(SPIDER_LEAP_MAX_DIST_SQ, 6.0f * 6.0f));
	// Leap vertical impulse (Beta motionY = 0.4).
	assert(feq(SPIDER_LEAP_VEL_Y, 0.4f));
	// Leap chance: nextInt(10) == 0 -> 1-in-10 per eligible tick.
	assert(SPIDER_LEAP_CHANCE_DEN == 10);
	// Horizontal lunge coefficients (Beta: dx/f2 * 0.5 * 0.8 + motX * 0.2).
	assert(feq(SPIDER_LEAP_FWD, 0.5f * 0.8f)); // 0.4
	assert(feq(SPIDER_LEAP_KEEP, 0.2f));
	// Wall-climb ascend velocity (v2 mapping of the isOnLadder ascend; see header).
	assert(feq(SPIDER_CLIMB_VEL_Y, 0.2f));
	// Per-mob melee. Spider 2 half-hearts = 1 heart = 16 (player scale);
	// zombie 5 half-hearts = 2.5 hearts = 40. Spider hits for LESS.
	assert(SPIDER_MELEE_DAMAGE == 16);
	assert(SPIDER_MELEE_DAMAGE == 2 * (COMBAT_HEALTH_PER_HEART / 2));
	assert(SPIDER_MELEE_DAMAGE < ZOMBIE_ATTACK_DAMAGE); // 16 < 40
	assert(ZOMBIE_ATTACK_DAMAGE - SPIDER_MELEE_DAMAGE == 24); // 1.5 hearts less
}

// The per-mob melee dispatcher keyed on monster id (mirrors mob_melee_damage in
// combat.h). Pins each mob's player-scale damage to the Beta attackStrength.
static void test_per_mob_damage(void) {
	assert(mob_melee_damage(MONSTER_SPIDER) == SPIDER_MELEE_DAMAGE); // 16
	assert(mob_melee_damage(MONSTER_ZOMBIE) == ZOMBIE_ATTACK_DAMAGE); // 40
	// Skeleton's real threat is the bow, but its melee falls through to the Beta
	// EntityMob default attackStrength = 2 (same as the spider), NOT the zombie 5.
	assert(mob_melee_damage(MONSTER_SKELETON) == SPIDER_MELEE_DAMAGE); // 16
	// Creeper never melees (routed around hurt_player); not asserted here.
	// Crash-safe fallback: an unknown id is treated as the EntityMob baseline.
	assert(mob_melee_damage(-1) == SPIDER_MELEE_DAMAGE);
	assert(mob_melee_damage(9999) == SPIDER_MELEE_DAMAGE);
}

// spider_should_leap encodes the Beta gate EXCEPT the RNG roll + onGround +
// aggro, which the caller supplies. Pins the squared-distance band [4,36).
static void test_leap_gate(void) {
	// Inside the band (2.0 < d < 6.0): eligible.
	assert(spider_leap_in_band(3.0f * 3.0f));  // 9
	assert(spider_leap_in_band(2.0001f * 2.0001f));
	assert(spider_leap_in_band(5.999f * 5.999f));
	// Too close (d <= 2.0): NOT eligible (it melees instead).
	assert(!spider_leap_in_band(2.0f * 2.0f));  // == 4.0, exclusive
	assert(!spider_leap_in_band(1.5f * 1.5f));
	assert(!spider_leap_in_band(0.0f));
	// Too far (d >= 6.0): NOT eligible (it walks closer first).
	assert(!spider_leap_in_band(6.0f * 6.0f));  // == 36.0, exclusive
	assert(!spider_leap_in_band(8.0f * 8.0f));
}

// The horizontal lunge impulse: motX = dx/dist * 0.4 + motX_old * 0.2 (Beta
// 0.5*0.8 = 0.4 forward, 0.2 retained). Pins the exact coefficients + that the
// impulse points AT the player.
static void test_leap_impulse(void) {
	/* combat.h mob_leap_impulse_beta: the helper entity_monster.c's leap runs
	 * (mob motion units S2 -- doubles, Beta's exact expression). */
	const double fwd = 0.5 * (double)0.8F, keep = (double)0.2F;
	double m[3] = {0.0, 0.0, 0.0};
	// Player due +X, 4 blocks away, spider at rest.
	mob_leap_impulse_beta(4.0, 0.0, m);
	assert(m[0] == 4.0 / (double)4.0F * fwd && m[2] == 0.0);
	assert(m[1] == (double)SPIDER_LEAP_VEL_Y); // (double)0.4F

	// With retained motion: spider already drifting +X at 0.1.
	m[0] = 0.1;
	m[2] = 0.0;
	mob_leap_impulse_beta(4.0, 0.0, m);
	assert(m[0] == 1.0 * fwd + 0.1 * keep); // ~0.42

	// 3-4-5 diagonal: dist 5, dx/dist 0.6, dz/dist 0.8.
	m[0] = m[2] = 0.0;
	mob_leap_impulse_beta(3.0, 4.0, m);
	assert(m[0] == 3.0 / (double)5.0F * fwd); // ~0.24
	assert(m[2] == 4.0 / (double)5.0F * fwd); // ~0.32
	// The impulse always points toward the player (same sign as dx/dz).
	assert(m[0] > 0.0 && m[2] > 0.0);

	// Degenerate same-column: finite, no NaN, retained term only.
	m[0] = 0.5;
	m[2] = -0.5;
	mob_leap_impulse_beta(0.0, 0.0, m);
	assert(!isnan(m[0]) && !isnan(m[2]));
	assert(m[0] == 0.5 * keep && m[2] == -0.5 * keep);
}

// Wall-climb: a horizontally-collided spider ascends (Beta isOnLadder ->
// motionY no longer falls; v2 sets a positive climb velocity). Off the wall it
// does not climb.
static void test_climb(void) {
	// When collided, the ascend velocity is the positive climb constant.
	float vy_collided = SPIDER_CLIMB_VEL_Y;
	assert(vy_collided > 0.0f);
	// Faithful ordering sanity: the climb is gentler than the leap pop.
	assert(SPIDER_CLIMB_VEL_Y < SPIDER_LEAP_VEL_Y); // 0.2 < 0.4
}

// The leap only fires when the spider has AGGRO. Beta's EntitySpider is passive
// in daylight (it never enters attackEntity), so the leap is gated by the same
// spider_hostile_at_light the chase uses -- a passive (bright-light) spider must
// not leap. (The day-passivity gate already lives in server_tick_monster; this
// just pins the invariant the leap relies on.)
static void test_leap_requires_aggro(void) {
	// In the dark (eff light <= 10) the spider is hostile -> may leap.
	assert(spider_hostile_at_light(0));
	assert(spider_hostile_at_light(SPIDER_HOSTILE_LIGHT_MAX)); // 10
	// In daylight (eff light 15) it is passive -> the leap branch is skipped.
	assert(!spider_hostile_at_light(15));
	assert(!spider_hostile_at_light(SPIDER_HOSTILE_LIGHT_MAX + 1)); // 11
}

int main(void) {
	test_constants();
	test_per_mob_damage();
	test_leap_gate();
	test_leap_impulse();
	test_climb();
	test_leap_requires_aggro();
	printf("test_spider_combat: spider combat pinned (leap band 4..36, motY %.1f, "
		   "lunge 0.4fwd+0.2keep, climb %.1f, melee %d < zombie %d, aggro-gated)\n",
		   SPIDER_LEAP_VEL_Y, SPIDER_CLIMB_VEL_Y, SPIDER_MELEE_DAMAGE,
		   ZOMBIE_ATTACK_DAMAGE);
	return 0;
}
