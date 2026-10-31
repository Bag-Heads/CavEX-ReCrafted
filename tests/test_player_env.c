/*
	test_player_env -- pins the batch-B env-damage cadences in server_local.c's
	hazard sweep that aren't a single pure helper:

	  B7  drowning cadence: air 300, first 1-heart hit at air==-20 (tick 320),
	      then every 20 ticks; air refills to 300 out of water.
	  B6  fire: lava ignites a 600-tick burn dealing 1 point (8) every 20 ticks;
	      a fire block ignites after PLAYER_FIRE_RESISTANCE ticks of contact.
	  B11 cactus contact = 1 point (player 8; mob MOB_CACTUS_DAMAGE 1).

	The cadence logic is inline in server_local_update (whole server thread), so
	this mirrors it against the SHARED constants -- the same pattern as
	test_player_fall / test_mob_hazards. The hardcoded Beta tick counts (320, 20,
	600, 30) fail if a constant drifts.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "network/server_local.h"        // MAX_OXYGEN, OXYGEN_THRESHOLD, PLAYER_FIRE_RESISTANCE
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_interface.h"    // HEALTH_PER_HEART
#include "entity/combat.h"               // MOB_CACTUS_DAMAGE

int main(void) {
	// --- shared constants (Beta values) --------------------------------------
	assert(MAX_OXYGEN == 300);            // Beta Entity.maxAir
	assert(OXYGEN_THRESHOLD == -20);      // Beta drown trigger (air == -20)
	assert(PLAYER_FIRE_RESISTANCE == 20); // Beta EntityPlayer.fireResistance
	assert(MOB_CACTUS_DAMAGE == 1);       // Beta BlockCactus attackEntityFrom(1)
	assert(HEALTH_PER_HEART == 16);

	const int POINT = HEALTH_PER_HEART / 2; // one Beta damage point == 8 hp
	assert(POINT == 8);

	// --- B7 drowning cadence (mirror of the in_water branch) ------------------
	{
		int air = MAX_OXYGEN; // 300
		int first_hit = -1, second_hit = -1, hits = 0;
		for(int t = 1; t <= 700; t++) {
			air--;
			if(air <= OXYGEN_THRESHOLD) {
				air = 0; // snap, then 1-heart hit
				hits++;
				if(first_hit < 0)
					first_hit = t;
				else if(second_hit < 0)
					second_hit = t;
			}
		}
		assert(first_hit == 320);             // 300 decrements to -20
		assert(second_hit - first_hit == 20); // then every 20 ticks
		// refill: a single dry tick restores full air.
		air = -5;
		air = MAX_OXYGEN;
		assert(air == 300);
	}

	// --- B6 fire burn DoT (mirror of the lit-but-off-source branch) -----------
	{
		int fire = 600; // just left lava (setOnFireFromLava sets fire=600)
		int dot_hits = 0, first_dot = -1;
		for(int t = 1; t <= 600; t++) {
			if(fire > 0) {
				if(fire % 20 == 0) {
					dot_hits++;
					if(first_dot < 0)
						first_dot = t;
				}
				fire--;
			}
		}
		assert(first_dot == 1);     // fire 600 % 20 == 0 -> burns on entry
		assert(dot_hits == 30);     // 600,580,...,20 -> 30 hits over 30s
		assert(fire == 0);          // burns out exactly
	}

	// --- B6 fire-block ignite ramp (mirror of the in_fire branch) -------------
	{
		int fire = -PLAYER_FIRE_RESISTANCE; // fresh fire-block contact baseline
		int ignite_tick = -1;
		for(int t = 1; t <= 30; t++) {
			fire++;
			if(fire == 0) {
				fire = 300;
				if(ignite_tick < 0)
					ignite_tick = t;
				break;
			}
		}
		assert(ignite_tick == PLAYER_FIRE_RESISTANCE); // 20 ticks of contact to ignite
		assert(fire == 300);                            // 15s burn after ignition
	}

	// --- damage magnitudes (i-frame-gated points) ----------------------------
	assert(4 * POINT == 32); // lava entry (setOnFireFromLava 4 points)
	assert(1 * POINT == 8);  // fire DoT / fire-block contact / player cactus = 1 point
	assert(HEALTH_PER_HEART == 16); // drowning = 1 full heart

	puts("test_player_env: OK");
	return 0;
}
