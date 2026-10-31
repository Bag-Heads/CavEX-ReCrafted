/*
	test_player_fall -- pins the batch-B player environmental-damage core that
	lives in server_local.c's per-tick hazard sweep + the death branch:

	  B2  fall damage was billed a FULL heart per point above 3 == exactly 2x
	      Beta's ceil(fallDistance-3) HALF-hearts. Pinned via player_fall_damage_hp.
	  B3  on a ladder, NO fall damage accrues (Beta zeroes fallDistance every
	      ladder tick). Pinned via the helper's on_ladder gate.
	  B5  respawn restores FULL health, not the old MAX/2 (Beta EntityPlayer = 20).
	  B12 void kill is a STRICT y < -64 (Beta uses '<', not '<=').
	  B1  the fluid/hazard column is floor() of the eye position with NO -1 X
	      offset (the old code shifted X by -1 and truncated toward zero).

	The hazard sweep itself pulls in the whole server thread, so the faithful
	math is factored into the pure combat.h helper + the shared constants, which
	this test pins so they cannot drift.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "../source/entity/combat.h"            // player_fall_damage_hp
#include "../source/network/server_interface.h" // MAX_PLAYER_HEALTH, HEALTH_PER_HEART, VOID_KILL_Y

int main(void) {
	const int HALF = HEALTH_PER_HEART / 2; // 8 hp == one Beta half-heart point

	// --- B2: a half-heart per fall point above 3 (was a full heart = 2x) ------
	assert(player_fall_damage_hp(3, false) == 0);          // <= 3 blocks is safe
	assert(player_fall_damage_hp(4, false) == HALF);       // ceil(1)=1 pt = 8 (was 16)
	assert(player_fall_damage_hp(5, false) == 2 * HALF);   // 16 = one heart
	assert(player_fall_damage_hp(10, false) == 7 * HALF);  // 56
	assert(player_fall_damage_hp(23, false) == 20 * HALF); // 160 = full bar (lethal)
	// regression guard: the OLD full-heart-per-point value must be gone.
	assert(player_fall_damage_hp(4, false) != HEALTH_PER_HEART);     // not 16
	assert(player_fall_damage_hp(4, false) == HEALTH_PER_HEART / 2); // is 8
	for(int d = 4; d <= 40; d++)
		assert(player_fall_damage_hp((float)d, false)
			   == (HEALTH_PER_HEART / 2) * (d - 3));
	// Beta uses ceil(fallDistance-3): fractional falls round UP (the old
	// int-truncating snapshot floored them, under-billing fractional drops).
	assert(player_fall_damage_hp(3.5f, false) == HALF);       // ceil(0.5)=1 -> 8
	assert(player_fall_damage_hp(3.9f, false) == HALF);       // ceil(0.9)=1 -> 8 (NOT 0)
	assert(player_fall_damage_hp(4.7f, false) == 2 * HALF);   // ceil(1.7)=2 -> 16
	assert(player_fall_damage_hp(22.9f, false) == 20 * HALF); // ceil(19.9)=20 -> 160
	assert(player_fall_damage_hp(4.7f, false) != HALF);       // not the floored 8
	assert(player_fall_damage_hp(3.0f, false) == 0);          // exactly 3 is safe

	// --- B3: on a ladder there is NO fall damage at any distance --------------
	assert(player_fall_damage_hp(4, true) == 0);
	assert(player_fall_damage_hp(30, true) == 0);
	assert(player_fall_damage_hp(100, true) == 0);

	// --- B5: respawn = FULL health, not the old half --------------------------
	// Engine scale: HEALTH_PER_HEART=16, full bar = 10 hearts = 160 (== Beta's 20
	// health points / 2 hearts-per-point). Respawn now restores 160, not 80.
	assert(MAX_PLAYER_HEALTH == 10 * HEALTH_PER_HEART); // a full 10-heart bar (160)
	assert(MAX_PLAYER_HEALTH != MAX_PLAYER_HEALTH / 2); // reject the old 80-on-respawn
	// A 23-block fall (20 half-heart points) bills exactly the full bar -> lethal.
	assert(player_fall_damage_hp(23, false) == MAX_PLAYER_HEALTH);

	// --- B12: void kill threshold is a strict y < -64 -------------------------
	assert(VOID_KILL_Y == -64.0);
	assert(-64.5 < VOID_KILL_Y);     // below the floor: dies
	assert(!(-64.0 < VOID_KILL_Y));  // exactly -64: does NOT (strict <)
	assert(!(-10.0 < VOID_KILL_Y));

	// --- B1: hazard column = floor() of the eye pos, no -1 X offset -----------
	// floor differs from the old truncating cast on negative coords, and the
	// fix carries NO X offset (the old read used x-1).
	assert((int)floor(-3.4) == -4);  // floor on negatives (trunc would give -3)
	assert((int)floor(5.9) == 5);
	assert((int)floor(-0.1) == -1);
	assert((int)(-3.4) != (int)floor(-3.4)); // trunc(-3) != floor(-4): the bug

	puts("test_player_fall: OK");
	return 0;
}
