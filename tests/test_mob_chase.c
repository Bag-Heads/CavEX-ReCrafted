/*
	Pins the Beta 1.7.3 mob chase/approach steering: the 16-block follow range and
	the direct-approach heading. Binds the SHARED source/entity/combat.h directly
	(like test_zombie_melee), so the constant + helper tested here are the exact
	ones pinned.

	FH-13 UPDATE: mob_chase_dir is RETIRED from production (server_tick_monster's
	chase branch now steers off real A* pathfinding waypoints -- see
	source/entity/pathfinding.h + combat.h's mob_chase_dir comment). This test
	deliberately still pins the helper's own behaviour: it is kept as the
	documented T2 fallback candidate if the hardware round's pathfinding_perf_
	counters show A* too expensive on Broadway, so its exact contract must not
	silently drift while unused. MOB_FOLLOW_RANGE/_SQ ARE still live (they gate
	the real A* search range too -- see mob_acquire_chase_path).

	The helper mob_chase_dir(dx, dz, follow_range_sq, &x, &z) takes dx/dz =
	(player - mob) on XZ; returns true with a unit heading toward the player when
	in range, or false with the heading zeroed when out of range.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

static void test_constants(void) {
	assert(feq(MOB_FOLLOW_RANGE, 16.0f));		 // Beta findPlayerToAttack range
	assert(feq(MOB_FOLLOW_RANGE_SQ, 256.0f));	 // 16^2
	assert(MOB_FOLLOW_RANGE_SQ > MOB_ATTACK_REACH_SQ); // detect before you can hit
}

static void test_in_range_heading(void) {
	float x, z;
	// 3-4-5: in range -> unit heading (0.6, 0.8) pointing at the player.
	assert(mob_chase_dir(3.0f, 4.0f, MOB_FOLLOW_RANGE_SQ, &x, &z));
	assert(feq(x, 0.6f) && feq(z, 0.8f));
	assert(feq(x * x + z * z, 1.0f)); // unit length

	// Player due east (+x) and due west (-x): heading sign follows.
	assert(mob_chase_dir(5.0f, 0.0f, MOB_FOLLOW_RANGE_SQ, &x, &z));
	assert(feq(x, 1.0f) && feq(z, 0.0f));
	assert(mob_chase_dir(-8.0f, 0.0f, MOB_FOLLOW_RANGE_SQ, &x, &z));
	assert(feq(x, -1.0f) && feq(z, 0.0f));
}

static void test_out_of_range_wanders(void) {
	float x = 9.0f, z = 9.0f; // sentinels: must be zeroed on the false path
	// Exactly at the boundary (dist^2 == 256): gate is strict '<' -> NOT in range.
	assert(!mob_chase_dir(16.0f, 0.0f, MOB_FOLLOW_RANGE_SQ, &x, &z));
	assert(feq(x, 0.0f) && feq(z, 0.0f));
	// Beyond the boundary: not in range, heading left zeroed (caller wanders).
	x = 9.0f; z = 9.0f;
	assert(!mob_chase_dir(20.0f, 0.0f, MOB_FOLLOW_RANGE_SQ, &x, &z));
	assert(feq(x, 0.0f) && feq(z, 0.0f));
}

// The mob acquires within 16 blocks and gives up once the player leaves it
// (mirrors Beta's 16-block A* path-range leash for this v1).
static void test_acquire_then_give_up(void) {
	float x, z;
	assert(mob_chase_dir(8.0f, 0.0f, MOB_FOLLOW_RANGE_SQ, &x, &z));  // 64 < 256: chase
	assert(!mob_chase_dir(17.0f, 0.0f, MOB_FOLLOW_RANGE_SQ, &x, &z)); // 289 > 256: give up
}

static void test_degenerate_overlap(void) {
	float x, z;
	// Player and mob share an XZ column: still in range, finite unit fallback (+x).
	assert(mob_chase_dir(0.0f, 0.0f, MOB_FOLLOW_RANGE_SQ, &x, &z));
	assert(!isnan(x) && !isnan(z));
	assert(feq(x, 1.0f) && feq(z, 0.0f));
}

int main(void) {
	test_constants();
	test_in_range_heading();
	test_out_of_range_wanders();
	test_acquire_then_give_up();
	test_degenerate_overlap();
	printf("test_mob_chase: chase steering pinned (16-block follow range, unit "
		   "heading toward player, give-up, degenerate guard)\n");
	return 0;
}
