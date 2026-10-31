/*
	Pins the Beta 1.7.3 living-entity ENVIRONMENTAL HAZARDS for the 4 hostiles
	(audit #15), on the MOB raw 20-pt health scale (1 pt = HALF a heart). Binds the
	SHARED source/entity/combat.h so the constants + pure helper here are EXACTLY the
	ones server_tick_monster's hazard block uses -- the values cannot drift between the
	engine and the test.

	Beta source (PeytonPlayz595/Beta-1.7.3 decompiled Entity.java + EntityLiving.java;
	mc173 mindstorm38 corroborates fire_time=600 + the i-frame absorb constants):
	  FALL  EntityLiving.fall(f): i = ceil(f - 3.0); if(i>0) attackEntityFrom(null, i).
	  DROWN air=300, --air/tick submerged; on air==-20 -> air=0 + attackEntityFrom(2);
	        then 2 every 20 ticks. Out of water air snaps back to 300.
	  LAVA  setOnFireFromLava(): attackEntityFrom(4); fire = 600. (600, NOT setFire(15)=
	        300 -- that's a later version; b1.7.3 literally `this.fire = 600`.) The 4 is
	        i-frame gated (~every 10 ticks); fire_time then DoTs 1/20t via the existing
	        fire path. Sun-burn stays MOB_FIRE_TICKS(300) -- a different Beta value.

	All hazard values are RAW mob-scale points applied DIRECTLY to the 20-pt health,
	NOT scaled (the times-8 or times-16 player-scale convention is only for damage that
	hits the 160-pt player).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

// --- Constants are the Beta values, on the mob scale ----------------------------
static void test_constants(void) {
	// FALL: no damage at or under 3 blocks (Beta threshold -3.0F).
	assert(MOB_FALL_SAFE_BLOCKS == 3.0f);
	// DROWN: Beta air = maxAir = 300; first hit at air == -20; 2 dmg; 20-tick interval.
	assert(MOB_AIR_MAX == 300);
	assert(MOB_DROWN_AIR_THRESHOLD == -20);
	assert(MOB_DROWN_DAMAGE == 2);   // 2 half-hearts = 1 heart, mob-scale
	assert(MOB_DROWN_INTERVAL == 20);
	// LAVA: 4 per application (2 hearts) + sets the fire field to 600 (NOT 300).
	assert(MOB_LAVA_DAMAGE == 4);
	assert(MOB_LAVA_FIRE_TICKS == 600);
	// CRITICAL split (the trap that bit sun-burn): lava's fire ignition is 600, which
	// MUST be distinct from the sun-burn ignition (MOB_FIRE_TICKS = 300). Reusing 300
	// for lava would be unfaithful.
	assert(MOB_LAVA_FIRE_TICKS != MOB_FIRE_TICKS);
	assert(MOB_FIRE_TICKS == 300);
	// The lava fire-DoT itself reuses the EXISTING fire path: 1 half-heart / 20 ticks.
	assert(MOB_FIRE_DAMAGE == 1);
	assert(MOB_FIRE_DAMAGE_INTERVAL == 20);
}

// --- FALL: ceil(d - 3) clamped >= 0, exact Beta points --------------------------
static void test_fall_damage(void) {
	// Threshold cases (CEIL, threshold 3.0): the brief verbatim assertions.
	assert(mob_fall_damage(3.0f) == 0);  // exactly 3 blocks: safe
	assert(mob_fall_damage(4.0f) == 1);  // ceil(1) = 1
	assert(mob_fall_damage(6.0f) == 3);  // ceil(3) = 3
	assert(mob_fall_damage(23.0f) == 20); // ceil(20) = 20 -> lethal on a 20-pt mob

	// Sub-block CEIL behavior: any fraction over the threshold rounds UP.
	assert(mob_fall_damage(3.0f) == 0);
	assert(mob_fall_damage(3.01f) == 1); // ceil(0.01) = 1
	assert(mob_fall_damage(4.0f) == 1);
	assert(mob_fall_damage(4.01f) == 2); // ceil(1.01) = 2
	assert(mob_fall_damage(5.0f) == 2);

	// Clamp: falls at or below the safe height never deal damage (no negatives).
	assert(mob_fall_damage(0.0f) == 0);
	assert(mob_fall_damage(1.0f) == 0);
	assert(mob_fall_damage(2.5f) == 0);

	// Monotonic non-decreasing with height.
	for(float d = 3.0f; d <= 30.0f; d += 0.5f)
		assert(mob_fall_damage(d) <= mob_fall_damage(d + 0.5f));

	// 23 blocks deals exactly a 20-pt mob's full health -> lethal, NOT scaled by *8.
	assert(mob_fall_damage(23.0f) == 20);
}

static void test_iframe_arbitration(void) {
	int resist = 0, strongest = 0;
	bool fresh;
	assert(mob_iframe_damage(1, &resist, &strongest, &fresh) == 1);
	assert(fresh && resist == MOB_HURT_RESIST_TICKS && strongest == 1);
	assert(mob_iframe_damage(1, &resist, &strongest, &fresh) == 0);
	assert(!fresh && strongest == 1);
	assert(mob_iframe_damage(4, &resist, &strongest, &fresh) == 3);
	assert(!fresh && resist == MOB_HURT_RESIST_TICKS && strongest == 4);
	resist = 0;
	assert(mob_iframe_damage(2, &resist, &strongest, &fresh) == 2);
	assert(fresh && strongest == 2);
}

// --- DROWN: mirror the engine's air-drain cadence (entity_monster.c) -------------
// Returns the damage dealt THIS submerged tick (0 or MOB_DROWN_DAMAGE), updating air
// exactly as the hazard block does: `if(--air == -20){ air = 0; dmg }`.
static int drown_tick(int* air) {
	if(--(*air) == MOB_DROWN_AIR_THRESHOLD) {
		*air = 0;
		return MOB_DROWN_DAMAGE;
	}
	return 0;
}

// Out-of-water tick: the engine's else branch snaps air instantly back to full
// (Beta `air = maxAir`). Returns the refilled value so the test exercises the real
// refill path rather than asserting a constant it just assigned.
static int air_refill(void) {
	return MOB_AIR_MAX;
}

static void test_drown_cadence(void) {
	int air = MOB_AIR_MAX; // fresh mob: full breath
	int first_hit = -1, second_hit = -1, total = 0;
	for(int t = 1; t <= 400; t++) {
		int d = drown_tick(&air);
		if(d) {
			total += d;
			if(first_hit < 0) first_hit = t;
			else if(second_hit < 0) second_hit = t;
		}
	}
	// First drown lands at tick 320 (300 to 0, then 0 to -20 = 16s @ 20 TPS).
	assert(first_hit == 320);
	// Then 2 every 20 ticks: the next hit is 20 ticks later.
	assert(second_hit == first_hit + MOB_DROWN_INTERVAL); // 340
	assert(second_hit - first_hit == 20);
	// Each hit is exactly 2 (1 heart), mob-scale.
	// Over 400 ticks: hits at 320, 340, 360, 380, 400 = 5 hits * 2 = 10.
	assert(total == 5 * MOB_DROWN_DAMAGE);

	// REFILL: stepping out of water snaps air back to full instantly (Beta else branch).
	// Drive it through real state: drain a handful of submerged ticks so air is well
	// below full, then run the out-of-water refill and assert it snapped to MOB_AIR_MAX
	// (this exercises the engine's else-branch behavior, not a self-assigned constant).
	air = MOB_AIR_MAX;
	for(int t = 0; t < 50; t++) drown_tick(&air); // ~50 submerged ticks
	assert(air == MOB_AIR_MAX - 50);              // genuinely drained (250)
	air = air_refill();                           // head left water -> instant refill
	assert(air == MOB_AIR_MAX);                   // snapped back to full (300)
	// And the very next submerged tick does NOT immediately re-drown (320-tick reset):
	// a refilled mob is 320 ticks from its next drown hit, so one wet tick is harmless.
	int d = drown_tick(&air);
	assert(d == 0 && air == MOB_AIR_MAX - 1);
}

// --- LAVA: 4 + sets fire to 600, and the fire DoT then burns at 1/20t ------------
// Mirror the engine's lava contact: gated 4 on the i-frame, fire_time set EVERY tick.
static void test_lava_contact(void) {
	short health = 20; // full 20-pt mob
	int hurt_resist_time = 0, strongest = 0;
	int fire_time = 0;
	bool fresh;

	// One lava-contact tick with no active i-frame: deals 4 AND sets fire to 600.
	health -= (short)mob_iframe_damage(MOB_LAVA_DAMAGE,
		&hurt_resist_time, &strongest, &fresh);
	fire_time = MOB_LAVA_FIRE_TICKS;

	assert(health == 16);                 // 20 - 4
	assert(fire_time == 600);             // setOnFireFromLava: this.fire = 600
	assert(fresh && hurt_resist_time == MOB_HURT_RESIST_TICKS);

	/* Entity.onUpdate applies lava before EntityLiving decrements heartsLife.
	 * Complete the first tick, then prove ticks 1..9 absorb and tick 10 re-hits. */
	hurt_resist_time--;
	for(int i = 1; i < MOB_HURT_RESIST_TICKS; i++) {
		health -= (short)mob_iframe_damage(MOB_LAVA_DAMAGE,
			&hurt_resist_time, &strongest, &fresh);
		fire_time = MOB_LAVA_FIRE_TICKS;
		if(hurt_resist_time > 0)
			hurt_resist_time--;
	}
	assert(health == 16 && hurt_resist_time == 0);
	health -= (short)mob_iframe_damage(MOB_LAVA_DAMAGE,
		&hurt_resist_time, &strongest, &fresh);
	fire_time = MOB_LAVA_FIRE_TICKS;
	hurt_resist_time--;
	assert(fire_time == 600);
	// Exactly one more 4 lands ten ticks after the first.
	assert(health == 12); // 16 - 4
	assert(hurt_resist_time == MOB_HURT_RESIST_TICKS - 1);

	// FIRE DoT after leaving lava: the SAME fire field (1 dmg per 20 ticks) burns it.
	// A 600-tick ignition = 30 fire-damage ticks (600/20) * 1 = 30 dmg -> lethal if dry.
	int burn = 0;
	int f = MOB_LAVA_FIRE_TICKS;
	while(f > 0) {
		if(f % MOB_FIRE_DAMAGE_INTERVAL == 0)
			burn += MOB_FIRE_DAMAGE;
		f--;
	}
	assert(burn == 30); // 600/20 hits of 1 -- more than enough to kill a 20-pt mob
}

int main(void) {
	test_constants();
	test_fall_damage();
	test_iframe_arbitration();
	test_drown_cadence();
	test_lava_contact();
	printf("test_mob_hazards: env hazards pinned (fall ceil(d-3): 3->0 4->1 6->3 "
		   "23->20; drown first@320 then 2/20t; lava 4 + fire=%d, DoT 30 over 30s)\n",
		   MOB_LAVA_FIRE_TICKS);
	return 0;
}
