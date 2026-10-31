/*
	Pins the Wii-controller PLAYABILITY tunables for mob combat and the attack
	wind-up (telegraph) state machine. These are NOT Beta values -- they are the
	2nd sanctioned carve-out from byte-exact parity (the feel/timing layer), so
	this guard exists to stop a refactor from silently reverting a documented,
	play-tested value back toward the unplayable Beta/fork timing. See CLAUDE.md
	"Beta 1.7.3 parity vs. Wii-controller playability" + docs/
	wii_controller_playability.md. Binds the SHARED source/entity/combat.h, so the
	constants here are the exact ones the engine uses.

	The wind-up machine mirrors server_tick_monster's melee sweep
	(entity_monster.c) tick-for-tick: a mob in reach must hold the strike for
	MOB_ATTACK_WINDUP_TICKS before the hit lands, and the window re-arms whenever
	the mob is not ready-and-in-reach, so every cadence swing is telegraphed.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

#define TPS 20 // engine ticks per second (server_local fixed step)

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

static void test_constants_documented(void) {
	// Drift guard: these are the documented, Dolphin-tuned values. A change here
	// must be deliberate -- update the doc + this pin together, never silently.
	assert(feq(MOB_MAX_SPEED_WII, 0.18f));
	assert(MOB_ATTACK_WINDUP_TICKS == 8);
}

static void test_speed_cap_is_playable(void) {
	// The cap must actually SLOW the mob below the fork's raw creeper tuning
	// (CREEPER_MAX_SPEED 1.25 in entity_monster.c -- referenced here as a literal
	// since it is engine-local) so a chaser cannot close on the player faster than
	// a Wiimote user can react.
	const float fork_creeper_max = 1.25f;
	assert(MOB_MAX_SPEED_WII < fork_creeper_max);

	// And it must land in a sane band: a positive speed (the mob still pursues),
	// and a ground speed that gives a fair reaction window but is not glacial.
	float blocks_per_sec = MOB_MAX_SPEED_WII * TPS;
	assert(MOB_MAX_SPEED_WII > 0.0f);
	assert(blocks_per_sec > 2.0f);  // still a credible threat, not trivially outrun
	assert(blocks_per_sec < 5.0f);  // well under the fork's unplayable ~7-8 b/s
}

// One server tick of the melee sweep, mirroring entity_monster.c exactly.
// Returns true on the tick the hit lands. *attack_time / *windup are the mob's
// persisted state. `in_reach` folds the reach + y-overlap gate. The telegraph is
// FIRST-SWING-ONLY: it re-arms only when the mob is OUT of reach, so the first
// hit of an engagement is warned but subsequent in-reach swings keep Beta's bare
// 1.0s (MOB_ATTACK_COOLDOWN_TICKS) cadence.
static bool melee_tick(int* attack_time, int* windup, bool in_reach) {
	if (*attack_time > 0)
		(*attack_time)--;
	if (!in_reach) {
		*windup = MOB_ATTACK_WINDUP_TICKS; // re-arm: re-entering reach re-telegraphs
		return false;
	}
	if (*attack_time == 0) {
		if (*windup > 0) {
			(*windup)--;
			return false;
		}
		*attack_time = MOB_ATTACK_COOLDOWN_TICKS;
		return true; // windup stays 0 -> next in-reach swing is bare-cadence
	}
	return false; // in reach but on cooldown -- hold windup as-is
}

static void test_first_hit_is_telegraphed(void) {
	// Mob spawns with the window armed (entity_monster factory sets
	// attack_windup = MOB_ATTACK_WINDUP_TICKS). Once in reach, the first hit must
	// not land until the player has had the full wind-up to react.
	int attack_time = 0, windup = MOB_ATTACK_WINDUP_TICKS;
	int first_hit = -1;
	for (int t = 1; t <= 64 && first_hit < 0; t++)
		if (melee_tick(&attack_time, &windup, true))
			first_hit = t;
	// 8 telegraph ticks decrement the window, the hit lands on the 9th in-reach
	// tick -> ~0.45s of warning before the first damage.
	assert(first_hit == MOB_ATTACK_WINDUP_TICKS + 1);
	assert((float)(first_hit - 1) / TPS >= 0.4f); // >= 0.4s reaction window
}

static void test_cadence_after_first_swing_is_beta(void) {
	// Hold the mob in reach and collect the hit ticks. The FIRST hit is delayed by
	// the wind-up, but every swing AFTER it (while the player stays in reach) must
	// fire at Beta's bare MOB_ATTACK_COOLDOWN_TICKS cadence -- the telegraph does
	// NOT slow the sustained DPS, it only warns the opening hit.
	int attack_time = 0, windup = MOB_ATTACK_WINDUP_TICKS;
	int hits[4], n = 0;
	for (int t = 1; t <= 200 && n < 4; t++)
		if (melee_tick(&attack_time, &windup, true))
			hits[n++] = t;
	assert(n == 4);
	assert(hits[0] == MOB_ATTACK_WINDUP_TICKS + 1); // first hit telegraphed
	for (int i = 2; i < n; i++) // gaps between sustained swings == Beta cadence
		assert(hits[i] - hits[i - 1] == MOB_ATTACK_COOLDOWN_TICKS);
	// and the very first gap is just the bare cooldown too (no extra wind-up):
	assert(hits[1] - hits[0] == MOB_ATTACK_COOLDOWN_TICKS);
}

static void test_leaving_reach_resets_windup(void) {
	// A mob that approaches, ticks down part of its wind-up, then is knocked/backed
	// out of reach must RE-telegraph on re-entry -- it can't bank progress and hit
	// instantly when it returns.
	int attack_time = 0, windup = MOB_ATTACK_WINDUP_TICKS;
	assert(!melee_tick(&attack_time, &windup, true));  // windup 8 -> 7
	assert(!melee_tick(&attack_time, &windup, true));  // 7 -> 6
	assert(windup == MOB_ATTACK_WINDUP_TICKS - 2);
	melee_tick(&attack_time, &windup, false);          // out of reach: re-arm
	assert(windup == MOB_ATTACK_WINDUP_TICKS);
	// Back in reach: must serve the FULL wind-up again before any hit.
	int first_hit = -1;
	for (int t = 1; t <= 32 && first_hit < 0; t++)
		if (melee_tick(&attack_time, &windup, true))
			first_hit = t;
	assert(first_hit == MOB_ATTACK_WINDUP_TICKS + 1);
}

int main(void) {
	test_constants_documented();
	test_speed_cap_is_playable();
	test_first_hit_is_telegraphed();
	test_cadence_after_first_swing_is_beta();
	test_leaving_reach_resets_windup();
	printf("test_mob_playability: Wii tunables pinned (speed cap %.2f b/tick, "
		   "%d-tick first-swing telegraph; opening hit warned, sustained swings "
		   "keep Beta %d-tick cadence, re-arms on leaving reach)\n",
		   MOB_MAX_SPEED_WII, MOB_ATTACK_WINDUP_TICKS, MOB_ATTACK_COOLDOWN_TICKS);
	return 0;
}
