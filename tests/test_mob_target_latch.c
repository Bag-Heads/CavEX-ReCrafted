/*
	Pins the Beta 1.7.3 persistent TARGET LATCH + retaliation (audit #14): the
	retention/leash constants and the pure mob_should_hold_target() decision, plus a
	tiny state machine that mirrors server_tick_monster's arm / refresh / decrement
	loop (NO heavy world mock -- the latch logic is pure given {aggro_time, dist_sq,
	can_acquire, can_see}). Binds the SHARED source/entity/combat.h so the constants +
	helper tested here are the exact ones the engine uses -- no drift.

	Beta (EntityCreature.updatePlayerActionState / EntityMob.attackEntityFrom):
	  - entityToAttack persists across ticks once acquired; a canEntityBeSeen miss only
	    suppresses the per-tick swing (attackBlockedEntity is empty), the mob keeps
	    pathing/walking toward the latched target.
	  - dropped only on target-dead or (spider) a daytime give-up roll; there is no
	    generic distance drop (the 16.0 only bounds the path search). v2 adds a leash so
	    a fled player frees the beeline chaser (the documented stand-in).
	  - retaliation: a damaging hit sets entityToAttack = attacker UNCONDITIONALLY, with
	    no light/LOS/distance gate -- a daytime-passive spider you hit chases.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

#define LEASH_SQ MOB_LEASH_RANGE_SQ // 256 (16 blocks)

static void test_constants(void) {
	// A fixed feel window (sanctioned playability stand-in for Beta's open-ended
	// persistence) -- must be a few seconds: long enough to outlast a wall-corner LOS
	// flicker, not so long it trails a fully-fled player forever.
	assert(MOB_TARGET_RETENTION_TICKS == 60);   // 3.0s @ 20 TPS
	assert(MOB_TARGET_RETENTION_TICKS > 0);
	// The leash matches the acquire range: hold aggro out to exactly where it could
	// have acquired, no farther.
	assert(MOB_LEASH_RANGE_SQ == MOB_FOLLOW_RANGE_SQ); // 256
}

static void test_hold_predicate(void) {
	// Patience + inside leash -> hold. Either missing -> drop.
	assert(mob_should_hold_target(1, 4.0f, LEASH_SQ));                 // 1 tick, 2 blocks
	assert(mob_should_hold_target(MOB_TARGET_RETENTION_TICKS, 100.0f, LEASH_SQ)); // 10 blocks
	assert(!mob_should_hold_target(0, 4.0f, LEASH_SQ));               // no patience -> drop
	assert(!mob_should_hold_target(MOB_TARGET_RETENTION_TICKS, 256.0f, LEASH_SQ)); // at leash
	assert(!mob_should_hold_target(MOB_TARGET_RETENTION_TICKS, 289.0f, LEASH_SQ)); // 17 > 16
	// The leash is strict '<' (same convention as the follow-range gate): exactly at
	// the boundary is OUT.
	assert(!mob_should_hold_target(5, LEASH_SQ, LEASH_SQ));
	assert(mob_should_hold_target(5, LEASH_SQ - 1.0f, LEASH_SQ));
}

// One tick of the engine's latch bookkeeping (the arm/refresh + chase/wander split in
// server_tick_monster). Mutates *aggro. Returns true iff the mob CHASES this tick.
//  - can_acquire: spider day-passivity gate (true for every other mob); irrelevant once
//    latched -- only gates the fresh (re)acquire/refresh, never the retained chase.
//  - can_see: instantaneous LOS this tick.
static bool latch_tick(int* aggro, float dist_sq, bool can_acquire, bool can_see) {
	bool in_follow = dist_sq < MOB_FOLLOW_RANGE_SQ;
	// (Re)arm/refresh on sustained sight.
	if(can_acquire && can_see && in_follow)
		*aggro = MOB_TARGET_RETENTION_TICKS;
	bool latched = mob_should_hold_target(*aggro, dist_sq, MOB_LEASH_RANGE_SQ);
	// Decrement happens on BOTH branches in the engine; do it once here.
	if(*aggro > 0)
		(*aggro)--;
	return latched;
}

// Retaliation: the SRPC_ENTITY_ATTACK handler sets aggro_time directly, bypassing both
// gates. Modeled as a direct write (what the engine does).
static void retaliate(int* aggro) {
	*aggro = MOB_TARGET_RETENTION_TICKS;
}

static void test_fresh_acquire_arms(void) {
	int aggro = 0; // no target
	// Player visible at 5 blocks (dist^2 25): acquires + chases, latch armed to full
	// (then decremented once this tick).
	assert(latch_tick(&aggro, 25.0f, true, true));
	assert(aggro == MOB_TARGET_RETENTION_TICKS - 1);
}

static void test_los_flicker_keeps_chase(void) {
	int aggro = 0;
	assert(latch_tick(&aggro, 25.0f, true, true)); // acquire, aggro -> 59
	// LOS breaks at a wall corner for a handful of ticks: the mob KEEPS chasing
	// (latched) and does NOT drop to wander, even though can_see is false. Patience
	// bleeds down by one per missed tick.
	int before = aggro;
	for(int t = 0; t < 5; t++) {
		assert(latch_tick(&aggro, 25.0f, true, false)); // still chasing
	}
	assert(aggro == before - 5); // 5 ticks of LOS-miss bled off
	// LOS returns -> refreshed back to full.
	assert(latch_tick(&aggro, 25.0f, true, true));
	assert(aggro == MOB_TARGET_RETENTION_TICKS - 1);
}

static void test_expiry_drops_to_wander(void) {
	int aggro = 0;
	assert(latch_tick(&aggro, 25.0f, true, true)); // acquire
	// Player stays in leash range but the mob never sees it again (e.g. permanently
	// walled but close). After MOB_TARGET_RETENTION_TICKS LOS-miss ticks the latch
	// expires and the mob reverts to wander.
	int chasing_ticks = 0;
	for(int t = 0; t < MOB_TARGET_RETENTION_TICKS + 5; t++) {
		if(latch_tick(&aggro, 25.0f, true, false))
			chasing_ticks++;
		else
			break;
	}
	// It chased for exactly the remaining patience (the acquire tick already spent 1),
	// then dropped. Must have given up (not chased forever).
	assert(chasing_ticks == MOB_TARGET_RETENTION_TICKS - 1);
	assert(aggro == 0);
	// And the very next tick with no sight is wander, not chase.
	assert(!latch_tick(&aggro, 25.0f, true, false));
}

static void test_out_of_leash_drops_immediately(void) {
	int aggro = 0;
	assert(latch_tick(&aggro, 25.0f, true, true)); // acquire near, full patience
	assert(aggro == MOB_TARGET_RETENTION_TICKS - 1); // plenty of patience left
	// Player sprints past the leash (17 blocks, dist^2 289 > 256). Even with a full
	// patience tank the mob drops the chase THIS tick -- the leash overrides patience.
	assert(!latch_tick(&aggro, 289.0f, true, false));
	// (And we don't refresh out there even if can_see were somehow true: dist > follow.)
	int a2 = MOB_TARGET_RETENTION_TICKS;
	assert(!latch_tick(&a2, 289.0f, true, true));
}

static void test_retaliation_arms_regardless_of_light_and_los(void) {
	// A DAYTIME-passive spider: can_acquire is false (bright), and the player is walled
	// off so can_see is false. It would NEVER acquire on its own.
	int aggro = 0;
	assert(!latch_tick(&aggro, 25.0f, /*can_acquire*/false, /*can_see*/false));
	assert(aggro == 0); // confirmed: no self-acquire

	// Player HITS it -> retaliation latches the target directly, bypassing both gates.
	retaliate(&aggro);
	assert(aggro == MOB_TARGET_RETENTION_TICKS);

	// Now it CHASES on the latch even though it still can't acquire (daylight) and still
	// can't see the player -- the retained chase ignores can_acquire/can_see.
	assert(latch_tick(&aggro, 25.0f, false, false));
	assert(aggro == MOB_TARGET_RETENTION_TICKS - 1);

	// A retaliated mob hit from OUTSIDE its LOS likewise pursues (same path).
	int aggro2 = 0;
	retaliate(&aggro2);
	assert(latch_tick(&aggro2, 100.0f, true, false)); // 10 blocks, no LOS -> still chase
}

int main(void) {
	test_constants();
	test_hold_predicate();
	test_fresh_acquire_arms();
	test_los_flicker_keeps_chase();
	test_expiry_drops_to_wander();
	test_out_of_leash_drops_immediately();
	test_retaliation_arms_regardless_of_light_and_los();
	printf("test_mob_target_latch: target latch pinned (acquire arms %d-tick patience, "
		   "LOS flicker keeps chase, expiry -> wander, leash drop, retaliation bypasses "
		   "light/LOS)\n", MOB_TARGET_RETENTION_TICKS);
	return 0;
}
