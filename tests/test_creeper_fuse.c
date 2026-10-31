/*
	Pins the Beta 1.7.3 creeper proximity fuse: the byte-exact fuse constants, the
	3-block-ignite / 7-block-release hysteresis (creeper_should_prime), and the
	swell -> detonate cadence. Binds the SHARED source/entity/combat.h so the values
	+ helper here are the exact ones server_tick_monster's creeper branch uses.

	Beta source (EntityCreeper.attackEntity, SERVER side): ignite when idle &&
	dist < 3.0 (dist^2 < 9); once swelling keep priming until the player is 7.0+
	away (dist^2 >= 49 deflates); fuseTime = 30 ticks -> createExplosion(power 3).
	A creeper deals ZERO contact melee. The defuse bubble is 7, not 3 (a value the
	research's adversarial pass corrected).

	parity fix 2026-08-31 (dossier finding "Proximity-fuse hysteresis re-arm
	threshold after a partial deflate"): the step() helper below mirrors
	entity_monster.c's TWO independent state variables -- `fuse` (Beta's
	timeSinceIgnited ramp) and `state` (Beta's DataWatcher creeperState hysteresis
	SELECTOR, -1/1). The selector LATCHES to -1 on every failing tick and stays
	there -- forcing the tight 3-block re-ignite test -- until a tick genuinely
	satisfies dist<3.0, regardless of how much ramp remains banked.
	[740c583: EntityCreeper.attackEntity: `int var3 = this.getCreeperState();
	if (var3<=0 && var2<3.0F || var3>0 && var2<7.0F) {...setCreeperState(1);...}
	else {setCreeperState(-1); --timeSinceIgnited;...}`]
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "entity/combat.h"
#include "network/explosion_model.h"

#define FOLLOW_SQ MOB_FOLLOW_RANGE_SQ // 256 (16 blocks)

static void test_constants(void) {
	assert(CREEPER_FUSE_TICKS == 30);          // 1.5s @ 20 TPS
	assert(CREEPER_IGNITE_DIST_SQ == 9.0f);    // 3.0 blocks
	assert(CREEPER_RELEASE_DIST_SQ == 49.0f);  // 7.0 blocks
	assert(CREEPER_EXPLOSION_POWER == 3.0f);
	// The defuse bubble must be LARGER than the ignite bubble (hysteresis), so a
	// creeper at the edge doesn't flicker between priming and deflating.
	assert(CREEPER_RELEASE_DIST_SQ > CREEPER_IGNITE_DIST_SQ);
}

static void test_hysteresis(void) {
	// IDLE: ignites only within 3 blocks. At 2 blocks (dist^2 4) yes; at 5 (25) no.
	assert(creeper_should_prime(4.0f, false, FOLLOW_SQ));
	assert(!creeper_should_prime(25.0f, false, FOLLOW_SQ));
	// SWELLING: stays primed out to 7 blocks. At 5 (25) still primed; at 8 (64) no.
	assert(creeper_should_prime(25.0f, true, FOLLOW_SQ));
	assert(!creeper_should_prime(64.0f, true, FOLLOW_SQ));
	// The hysteresis gap: 5 blocks (dist^2 25) is the key case -- a freshly-idle
	// creeper there does NOT ignite, but a swelling one there stays primed.
	assert(!creeper_should_prime(25.0f, false, FOLLOW_SQ));
	assert(creeper_should_prime(25.0f, true, FOLLOW_SQ));
	// Target lost past the follow range deflates even while swelling.
	assert(!creeper_should_prime(289.0f, true, FOLLOW_SQ)); // 17 blocks > 16
}

// Mirror the engine's creeper branch tick (post parity-fix 2026-08-31): TWO
// independent variables. `fuse` is the ramp (-1 idle .. CREEPER_FUSE_TICKS);
// `state` is the persisted hysteresis SELECTOR (-1/1, Beta's creeperState), fed
// into creeper_should_prime instead of a fuse-derived flag. [740c583:
// EntityCreeper.attackEntity]
static int step(int* fuse, int8_t* state, float dist_sq) {
	bool was_primed = *state > 0;
	if(creeper_should_prime(dist_sq, was_primed, FOLLOW_SQ)) {
		if(*fuse < 0)
			*fuse = 0;
		*state = 1;
		if(++(*fuse) >= CREEPER_FUSE_TICKS)
			return 1; // DETONATE
	} else {
		*state = -1; // latches on EVERY failing tick, unconditionally
		if(*fuse >= 0)
			(*fuse)--; // deflate
	}
	return 0;
}

static void test_detonation_cadence(void) {
	// Held within 2 blocks: detonates exactly CREEPER_FUSE_TICKS ticks after the
	// first prime (1.5s of swell warning).
	int fuse = -1, det = -1;
	int8_t state = -1;
	for(int t = 1; t <= 64 && det < 0; t++)
		if(step(&fuse, &state, 4.0f))
			det = t;
	assert(det == CREEPER_FUSE_TICKS); // 30
}

static void test_deflate_and_survive(void) {
	// Prime for a while, then the player escapes (8 blocks): the fuse must deflate
	// all the way back to idle (-1) WITHOUT detonating -- the creeper survives.
	int fuse = -1;
	int8_t state = -1;
	for(int t = 0; t < 10; t++)
		assert(!step(&fuse, &state, 4.0f)); // priming, not yet detonated (10 < 30)
	assert(fuse == 10);
	assert(state == 1);
	int det = 0;
	for(int t = 0; t < 30; t++)
		det |= step(&fuse, &state, 64.0f); // player at 8 blocks -> deflate
	assert(!det);                          // never detonated
	assert(fuse <= -1);                    // returned to idle
	assert(state == -1);
}

static void test_partial_deflate_forces_tight_rearm(void) {
	// The exact scenario the dossier's adversarial pass caught: a creeper that
	// deflates by only ONE tick must re-pass the TIGHT 3-block test to re-arm --
	// it may NOT re-arm on the loose 7-block test just because unspent ramp is
	// still banked. [740c583: EntityCreeper.attackEntity]
	int fuse = -1;
	int8_t state = -1;
	// Prime hard at 2 blocks (dist^2 4) for 20 ticks -> fuse=20, state=1 (primed).
	for(int t = 0; t < 20; t++)
		assert(!step(&fuse, &state, 4.0f));
	assert(fuse == 20);
	assert(state == 1);
	// ONE tick at 8 blocks (dist^2 64, >= the 49 release threshold): deflates
	// once -- fuse 19, state latches to -1.
	assert(!step(&fuse, &state, 64.0f));
	assert(fuse == 19);
	assert(state == -1);
	// Contrast: the OLD (buggy) derivation swelling_old = (fuse>=0) would read TRUE
	// here (19>=0), incorrectly re-applying the loose <49 test. Demonstrate the
	// divergence directly against creeper_should_prime at 5 blocks (dist^2 25):
	assert(creeper_should_prime(25.0f, /*swelling_old=*/true, FOLLOW_SQ));  // old: re-arms (WRONG)
	assert(!creeper_should_prime(25.0f, /*state==1*/false, FOLLOW_SQ));     // new: still fails (RIGHT)
	// Back to 5 blocks: NOT <3.0, so the latched state=-1 keeps failing the tight
	// test -- the creeper must KEEP deflating (fuse 18), not re-prime to fuse 20
	// the way the old fuse>=0-derived model would have.
	assert(!step(&fuse, &state, 25.0f));
	assert(fuse == 18); // continued deflating, NOT re-armed
	assert(state == -1);
	// Only genuinely re-arms once inside 3.0 blocks again -- and resumes from the
	// banked ramp (18 -> 19), not a fresh reset to 0.
	assert(step(&fuse, &state, 4.0f) == 0);
	assert(fuse == 19);
	assert(state == 1);
}

// Beta 1.7.3 Explosion.doExplosionA entity damage (raw Beta points; the player
// converts x8 at the boundary), pinned on the server formula
// (network/explosion_model.h): ratio = dist/size2 with size2 = 2*power,
// v = (1 - ratio) * density, damage = (int)((v*v+v)/2*8*size2+1).
// The caller gates ratio <= 1.0 (INCLUSIVE), so the exact-radius shell deals
// the +1 floor. [740c583 Explosion.doExplosionA:89-100]
static int blast_damage(float dist, float power, float density) {
	float size2 = power * 2.0f;
	double impulse;
	return explosion_entity_damage((double)dist / (double)size2, density, size2,
								   &impulse);
}
static void test_explosion_damage(void) {
	assert(blast_damage(0.0f, 3.0f, 1.0f) == 49); // point-blank: 1*8*6+1 = 49
	assert(blast_damage(6.0f, 3.0f, 1.0f) == 1);  // exactly the radius: floor + 1 = 1
	assert(blast_damage(6.0f, 3.0f, 0.0f) == 1);  // fully occluded: the +1 floor
	assert(blast_damage(3.0f, 3.0f, 0.5f) < blast_damage(3.0f, 3.0f, 1.0f)); // exposure scales
	// Monotonic falloff with distance inside the radius.
	assert(blast_damage(1.0f, 3.0f, 1.0f) > blast_damage(3.0f, 3.0f, 1.0f));
	assert(blast_damage(3.0f, 3.0f, 1.0f) > blast_damage(5.0f, 3.0f, 1.0f));
	// 49 half-hearts -> 49*8 = 392 engine units > player max 160 -> point-blank kills.
	assert(blast_damage(0.0f, 3.0f, 1.0f) * (COMBAT_HEALTH_PER_HEART / 2) > 160);
}

int main(void) {
	test_constants();
	test_hysteresis();
	test_detonation_cadence();
	test_deflate_and_survive();
	test_partial_deflate_forces_tight_rearm();
	test_explosion_damage();
	printf("test_creeper_fuse: proximity fuse pinned (ignite <3, release >=7 "
		   "hysteresis, %d-tick swell -> power %.0f blast, deflate-and-survive, "
		   "partial-deflate forces tight re-arm)\n",
		   CREEPER_FUSE_TICKS, CREEPER_EXPLOSION_POWER);
	return 0;
}
