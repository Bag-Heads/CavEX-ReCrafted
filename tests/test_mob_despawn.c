/*
	Pins the Beta 1.7.3 natural mob despawn (audit #16): the byte-exact distance/age/
	roll constants and the pure mob_despawn_action() decision that server_tick_monster's
	despawn branch drives. Binds the SHARED source/entity/combat.h so the values + helper
	here are the exact ones the engine uses -- they cannot drift apart.

	Beta source (EntityLiving.func_27021_X == despawnEntity(), run once per tick after
	++entityAge):
	    p = getClosestPlayerToEntity(this, -1.0); // any distance
	    if(canDespawn() && p != null) {           // all 4 hostiles canDespawn==true
	        d2 = getDistanceSqToEntity(p, this);  // 3D feet-to-feet, SQUARED
	        if(d2 > 16384.0)              setEntityDead();          // > 128: INSTANT
	        if(entityAge > 600 && rand.nextInt(800) == 0) {        // aged + 1/800 roll
	            if(d2 < 1024.0) entityAge = 0;                     // < 32: RESET, stays
	            else            setEntityDead();                   // 32..128: despawn
	        }
	    }
	The two distance tests are SEQUENTIAL (not else-if); the >128 branch is
	AGE-INDEPENDENT. Despawn is SILENT (setEntityDead only sets isDead -- no loot/XP).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

static void test_constants(void) {
	// Byte-exact Beta values: 128^2, 32^2, the 600-tick age gate, the 1/800 roll.
	assert(MOB_DESPAWN_FAR == 128.0f);
	assert(MOB_DESPAWN_FAR_SQ == 16384.0f); // 128^2
	assert(MOB_DESPAWN_NEAR == 32.0f);
	assert(MOB_DESPAWN_NEAR_SQ == 1024.0f); // 32^2
	assert(MOB_DESPAWN_MIN_AGE == 600);     // 30s @ 20 TPS
	assert(MOB_DESPAWN_ROLL_DEN == 800);    // rand.nextInt(800)==0
	// The keep-band must sit strictly inside the instant-despawn radius.
	assert(MOB_DESPAWN_NEAR_SQ < MOB_DESPAWN_FAR_SQ);
}

// > 128 blocks despawns INSTANTLY, independent of age and of the roll. This is the
// branch that is easiest to get wrong (the later persistenceRequired version gates
// it differently) -- pin it hard across young/old and roll-hit/roll-miss.
static void test_far_is_instant_and_age_independent(void) {
	float just_past = MOB_DESPAWN_FAR_SQ + 1.0f; // 129^2-ish, comfortably > 128^2
	// Brand-new mob (age 0), roll missed: STILL despawns because it's > 128.
	assert(mob_despawn_action(0, just_past, false) == MOB_DESPAWN_DESPAWN);
	// Young, roll hit: despawns.
	assert(mob_despawn_action(1, just_past, true) == MOB_DESPAWN_DESPAWN);
	// Old, roll missed: despawns (the >128 branch never consults the roll).
	assert(mob_despawn_action(10000, just_past, false) == MOB_DESPAWN_DESPAWN);
	// Way out (e.g. 200 blocks): despawns.
	assert(mob_despawn_action(0, 200.0f * 200.0f, false) == MOB_DESPAWN_DESPAWN);
	// Exactly AT 128 blocks is NOT > 128^2 -> not the instant branch (and too young
	// for the roll branch) -> KEEP.
	assert(mob_despawn_action(0, MOB_DESPAWN_FAR_SQ, true) == MOB_DESPAWN_KEEP);
}

// age <= 600: the aged-roll branch never fires, so within 128 blocks the mob is
// always KEPT regardless of the roll. (The >128 instant case is age-independent and
// is covered separately above -- do not conflate the two.)
static void test_young_never_despawns_within_128(void) {
	// At the age gate boundary: age == 600 is NOT > 600, so still too young.
	assert(mob_despawn_action(600, 50.0f * 50.0f, true) == MOB_DESPAWN_KEEP);
	assert(mob_despawn_action(600, 10.0f * 10.0f, true) == MOB_DESPAWN_KEEP);
	// Younger still, roll hit, mid-band: KEEP.
	assert(mob_despawn_action(0, 50.0f * 50.0f, true) == MOB_DESPAWN_KEEP);
	assert(mob_despawn_action(599, 50.0f * 50.0f, true) == MOB_DESPAWN_KEEP);
	// Young + roll hit + near: KEEP (no reset either -- the whole roll branch is gated
	// on age > 600).
	assert(mob_despawn_action(599, 5.0f * 5.0f, true) == MOB_DESPAWN_KEEP);
}

// age > 600 + roll hit + NEAR (< 32 blocks): RESET the age, mob persists.
static void test_aged_roll_near_resets(void) {
	// age 601 just clears the gate; player at 5 blocks (dist^2 25 < 1024) -> RESET.
	assert(mob_despawn_action(601, 5.0f * 5.0f, true) == MOB_DESPAWN_RESET_AGE);
	// At ~31 blocks (dist^2 961 < 1024) still inside the keep radius -> RESET.
	assert(mob_despawn_action(1000, 31.0f * 31.0f, true) == MOB_DESPAWN_RESET_AGE);
	// Point-blank: RESET.
	assert(mob_despawn_action(1000, 0.0f, true) == MOB_DESPAWN_RESET_AGE);
}

// age > 600 + roll hit + MID band (32..128 blocks): DESPAWN.
static void test_aged_roll_mid_despawns(void) {
	// 50 blocks (dist^2 2500): inside 128, outside 32 -> DESPAWN.
	assert(mob_despawn_action(601, 50.0f * 50.0f, true) == MOB_DESPAWN_DESPAWN);
	// Exactly AT 32 blocks: dist^2 == 1024 is NOT < 1024 -> the else (DESPAWN) branch.
	assert(mob_despawn_action(700, MOB_DESPAWN_NEAR_SQ, true) == MOB_DESPAWN_DESPAWN);
	// Just inside 128 (e.g. 100 blocks): DESPAWN.
	assert(mob_despawn_action(5000, 100.0f * 100.0f, true) == MOB_DESPAWN_DESPAWN);
}

// roll MISS within 128 blocks: KEEP, regardless of age or which band. The mob simply
// lives on and the clock keeps accumulating (the engine does NOT reset age on a miss).
static void test_roll_miss_keeps(void) {
	// Aged, near, roll missed: KEEP (NOT reset -- reset only on a hit).
	assert(mob_despawn_action(1000, 5.0f * 5.0f, false) == MOB_DESPAWN_KEEP);
	// Aged, mid-band, roll missed: KEEP (this is the "player near, no age/roll" case
	// where the mob just persists).
	assert(mob_despawn_action(1000, 50.0f * 50.0f, false) == MOB_DESPAWN_KEEP);
	// Aged, just inside 128, roll missed: KEEP.
	assert(mob_despawn_action(1000, 100.0f * 100.0f, false) == MOB_DESPAWN_KEEP);
}

// The roll has ZERO effect on the decision while the mob is young (age <= 600) OR
// outside 128 blocks: in both cases mob_despawn_action() returns the SAME action for
// roll_hit == true and roll_hit == false. This is the invariant that justifies the
// engine SHORT-CIRCUITING the rand_gen_int draw -- it gates the draw behind
// `age > 600 && dist_sq <= 128^2` (Beta's `entityAge > 600 && nextInt(800)==0` &&),
// so the shared seeded stream is consumed on EXACTLY the ticks Beta consumes it and
// the downstream sun-burn/wander RNG sequence is not shifted (determinism doctrine).
static void test_roll_is_a_noop_when_not_drawn(void) {
	// Young, every band inside 128: roll value cannot change the KEEP outcome.
	for(int age = 0; age <= MOB_DESPAWN_MIN_AGE; age += 100) {
		assert(mob_despawn_action(age, 5.0f * 5.0f, true)
			   == mob_despawn_action(age, 5.0f * 5.0f, false));   // near
		assert(mob_despawn_action(age, 50.0f * 50.0f, true)
			   == mob_despawn_action(age, 50.0f * 50.0f, false)); // mid-band
		assert(mob_despawn_action(age, MOB_DESPAWN_FAR_SQ, true)
			   == mob_despawn_action(age, MOB_DESPAWN_FAR_SQ, false)); // at 128
	}
	// Far (>128): instant DESPAWN regardless of the roll, so the roll is a no-op there
	// too (the engine skips the draw via the dist_sq <= FAR_SQ guard).
	for(int age = 0; age <= 2000; age += 500) {
		assert(mob_despawn_action(age, 200.0f * 200.0f, true)
			   == mob_despawn_action(age, 200.0f * 200.0f, false));
		assert(mob_despawn_action(age, 200.0f * 200.0f, true)
			   == MOB_DESPAWN_DESPAWN);
	}
	// The roll DOES matter only when aged AND in 32..128: there true->DESPAWN but
	// false->KEEP. This is the sole band where the engine actually draws.
	assert(mob_despawn_action(601, 50.0f * 50.0f, true) == MOB_DESPAWN_DESPAWN);
	assert(mob_despawn_action(601, 50.0f * 50.0f, false) == MOB_DESPAWN_KEEP);
}

// Simulate the engine's per-tick loop with the roll forced (deterministic) to verify
// the lifecycle: a mob held in the mid-band (32..128) survives until age>600, then the
// first rolled tick after that despawns it; a mob held near (<32) instead resets and
// never despawns no matter how many rolls hit.
static int sim_tick(int* age, float dist_sq, bool roll_hit) {
	(*age)++;
	switch(mob_despawn_action(*age, dist_sq, roll_hit)) {
	case MOB_DESPAWN_DESPAWN:   return 1;       // removed
	case MOB_DESPAWN_RESET_AGE: *age = 0; break; // persists, clock reset
	default:                    break;          // keep
	}
	return 0;
}

static void test_lifecycle(void) {
	// MID-BAND, roll always hits: must survive every tick until age clears 600, then
	// despawn on the very next tick (age becomes 601 > 600).
	int age = 0, dead = 0, when = -1;
	for(int t = 1; t <= 1000 && !dead; t++) {
		dead = sim_tick(&age, 50.0f * 50.0f, true);
		if(dead) when = t;
	}
	assert(dead);
	assert(when == MOB_DESPAWN_MIN_AGE + 1); // 601: first tick the age gate is cleared

	// NEAR (<32), roll always hits: the age is reset every time it would fire, so the
	// mob NEVER despawns -- it just lives forever near the player.
	age = 0; dead = 0;
	for(int t = 1; t <= 100000 && !dead; t++)
		dead = sim_tick(&age, 5.0f * 5.0f, true);
	assert(!dead);
	assert(age <= MOB_DESPAWN_MIN_AGE + 1); // never accumulates a full despawn window

	// FAR (>128): despawns on tick 1 even though age starts at 0 (instant branch).
	age = 0;
	assert(sim_tick(&age, 200.0f * 200.0f, false) == 1);
	assert(age == 1); // it incremented once, then despawned -- age/roll irrelevant
}

int main(void) {
	test_constants();
	test_far_is_instant_and_age_independent();
	test_young_never_despawns_within_128();
	test_aged_roll_near_resets();
	test_aged_roll_mid_despawns();
	test_roll_miss_keeps();
	test_roll_is_a_noop_when_not_drawn();
	test_lifecycle();
	printf("test_mob_despawn: Beta despawn pinned (>%.0f instant age-independent, "
		   "age>%d + 1/%d roll -> <%.0f resets / %.0f..%.0f despawns, miss keeps)\n",
		   MOB_DESPAWN_FAR, MOB_DESPAWN_MIN_AGE, MOB_DESPAWN_ROLL_DEN,
		   MOB_DESPAWN_NEAR, MOB_DESPAWN_NEAR, MOB_DESPAWN_FAR);
	return 0;
}
