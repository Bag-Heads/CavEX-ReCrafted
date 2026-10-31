/*
	test_mob_water -- pins the Beta 1.7.3 mob water movement (M2: the
	mass-drowning fix; constants + order corrected by the 2026-06-11
	adversarial review). Binds the SHARED source/entity/combat.h constants.
	Mob motion units S2 moved the water step onto the mob's authoritative
	doubles in entity_monster.c; the float step helpers this test used to call
	were retired as dead, so sections 2-5 now run Beta's recurrence in doubles
	with the same constants and ORDER the production branch uses (that branch
	itself is pinned per tick by test_mob_move_parity_golden and
	test_mob_trajectory_golden).

	THE BYTES ARE THE RATIOS: this engine integrates pos += vel once per 20Hz
	tick exactly like Beta, so Beta's RAW per-tick water constants (0.02
	gravity, 0.04 swim impulse, 0.8 drags, 0.02 moveFlying input) are the
	faithful values. The first cut scaled them off the engine's land GRAVITY
	(2.0) -- 25x too large -- and applied gravity BEFORE drag (the air-branch
	order); both are pinned against here.

	Pinned:
	  1. The Beta constants: 80% per-tick swim roll, gravity 0.02, vertical
	     drag 0.8, swim impulse 0.04, horizontal drag 0.8, flat input 0.02.
	  2. Step order: impulse, then DRAG, then UNDAMPED gravity (Beta
	     moveEntityWithHeading: motionY *= 0.8; motionY -= 0.02). From rest a
	     swim tick is +0.012, a non-swim tick is -0.02; the gravity-then-drag
	     reordering would give +0.016/-0.016 and fails.
	  3. Fixed points: always-swim rise +0.06, never-swim terminal sink -0.1
	     (= Beta's water terminal; the old scaling sank at -2.0 and ROSE at
	     +1.2/tick -- a porpoise, not a paddle).
	  4. Submerged trajectory at the Beta swim rate: over 200 ticks with a
	     4-of-5 swim pattern the mob rises -- but SLOWLY (a 2-deep pool takes
	     ~1-2 s to surface, not 2 ticks).
	  5. Horizontal: the water input step + 0.8 drag settle at Beta's 0.1
	     blocks/tick equilibrium -- visibly SLOWER than the 0.18 Wii land cap
	     (water is an escape mechanic again), and idle input is a no-op.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

/* Beta's water tail in doubles, the order entity_monster.c runs: the isJumping
 * +(double)0.04F before the move, then motionY *= (double)0.8F, motionY -= 0.02D. */
static float water_vy_step(float vel_y, bool swim_jump) {
	double v = (double)vel_y;
	if(swim_jump)
		v += (double)MOB_WATER_SWIM_IMPULSE;
	v *= (double)MOB_WATER_VEL_DAMP;
	v -= 0.02; /* Beta 0.02D (MOB_WATER_GRAVITY is its float spelling) */
	return (float)v;
}

/* moveFlying(dir, 0.02F) in the water branch: the flat input along the steer. */
static void water_input_step(float* vx, float* vz, float dir_x, float dir_z) {
	*vx = (float)((double)*vx + (double)(MOB_WATER_INPUT * dir_x));
	*vz = (float)((double)*vz + (double)(MOB_WATER_INPUT * dir_z));
}

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-5f;
}

int main(void) {
	// --- 1. constants are Beta's raw bytes -------------------------------------
	assert(feq(MOB_WATER_JUMP_CHANCE, 0.8f));
	assert(feq(MOB_WATER_GRAVITY, 0.02f));      // Beta motionY -= 0.02
	assert(feq(MOB_WATER_VEL_DAMP, 0.8f));      // Beta motionY *= 0.8
	assert(feq(MOB_WATER_SWIM_IMPULSE, 0.04f)); // Beta isJumping-in-water += 0.04
	assert(feq(MOB_WATER_DRAG_XZ, 0.8f));       // Beta motionX/Z *= 0.8
	assert(feq(MOB_WATER_INPUT, 0.02f));        // Beta moveFlying(.., 0.02F), flat

	// --- 2. step order: impulse, then drag, THEN undamped gravity --------------
	assert(feq(water_vy_step(0.0f, true), 0.012f));  // (0+0.04)*0.8 - 0.02
	assert(feq(water_vy_step(0.0f, false), -0.02f)); // 0*0.8 - 0.02
	// The wrong (air-branch) order would damp gravity: (0+0.04-0.02)*0.8 = 0.016.
	assert(!feq(water_vy_step(0.0f, true), 0.016f));

	// --- 3. fixed points: Beta's water equilibria -------------------------------
	{
		// Always swimming: v = (v + 0.04)*0.8 - 0.02 -> +0.06 rise.
		float vel = 0.0f;
		for(int t = 0; t < 400; t++)
			vel = water_vy_step(vel, true);
		assert(feq(vel, 0.06f));
		// Never swimming: v = v*0.8 - 0.02 -> -0.1 terminal sink (Beta's gentle
		// water sink; the old 25x scaling gave -2.0 = a whole pool per tick).
		vel = 0.0f;
		for(int t = 0; t < 400; t++)
			vel = water_vy_step(vel, false);
		assert(feq(vel, -0.1f));
	}

	// --- 4. submerged mob surfaces SLOWLY (paddle, not porpoise) ---------------
	{
		float vel = 0.0f, y = 0.0f;
		float y_after_40 = 0.0f;
		for(int t = 0; t < 200; t++) {
			bool swim = (t % 5) != 4; // 4-of-5 deterministic stand-in for 0.8
			vel = water_vy_step(vel, swim);
			y += vel; // engine integrates vel into the next tick's Y-move
			if(t == 39)
				y_after_40 = y;
		}
		assert(y > 4.0f);          // strongly upward over 10 s: it surfaces
		assert(y_after_40 > 0.5f); // ~2 s to clear a 2-deep pool floor...
		assert(y_after_40 < 2.5f); // ...but NOT a 2-tick breach (old scaling)
		// per-tick rise never exceeds the always-swim fixed point
		assert(vel <= 0.06f + 1.0e-5f);
	}

	// --- 5. horizontal: input + drag settle at Beta's 0.1, below the land cap --
	{
		float vx = 0.0f, vz = 0.0f;
		for(int t = 0; t < 400; t++) {
			water_input_step(&vx, &vz, 1.0f, 0.0f); // full steer along +X
			vx *= MOB_WATER_DRAG_XZ;                    // the branch's drag
			vz *= MOB_WATER_DRAG_XZ;
		}
		assert(feq(vx, MOB_WATER_INPUT * MOB_WATER_DRAG_XZ
						   / (1.0f - MOB_WATER_DRAG_XZ))); // 0.064.. pre-input,
		// equilibrium displacement per tick (vel right after input, which is what
		// the axis move integrates): 0.02/(1-0.8) = 0.1 -- Beta's water speed.
		float disp = vx + MOB_WATER_INPUT;
		assert(feq(disp, 0.1f));
		// And that is decisively SLOWER than the Wii land cap (0.18): water
		// slows every mob, restoring Beta's escape mechanic.
		assert(disp < MOB_MAX_SPEED_WII - 0.05f);
		// Idle steer (zero heading) adds nothing.
		float ix = 0.123f, iz = -0.456f;
		water_input_step(&ix, &iz, 0.0f, 0.0f);
		assert(feq(ix, 0.123f) && feq(iz, -0.456f));
	}

	printf("test_mob_water: OK (Beta raw bytes, drag-then-gravity order, slow "
		   "paddle, 0.1 water speed < land cap)\n");
	return 0;
}
