/*
	Pins the Beta 1.7.3 skeleton ranged attack + arrow constants and the arrow's
	gravity/drag flight, binding the SHARED source/entity/combat.h so the values
	here are the exact ones the skeleton AI (entity_monster.c) + entity_arrow.c use.

	Beta source: EntitySkeleton.attackEntity fires within 10 blocks (dist^2 < 100)
	every ~30 ticks; EntityArrow drops 0.03/tick under 0.99 air drag and a skeleton
	arrow deals a flat 4 half-hearts (player scale). v2 simplifications (no LOS,
	vanish-on-impact, no spread) are documented in combat.h.
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
	assert(feq(SKELETON_SHOOT_RANGE_SQ, 100.0f)); // 10 blocks
	assert(SKELETON_ARROW_COOLDOWN_TICKS == 30);
	assert(feq(ARROW_SPEED, 0.6f));
	assert(feq(ARROW_GRAVITY, 0.03f));
	assert(!feq(ARROW_GRAVITY, 0.05f)); // NOT the player-bow/1.4 gravity path
	assert(feq(ARROW_AIR_DRAG, 0.99f));
	// Skeleton arrow damage hits the PLAYER -> player scale (16/heart): 4 half-
	// hearts = 32 = 2 hearts. (Distinct from the mob-scale sun-burn damage of 1.)
	assert(ARROW_DAMAGE == 32);
	assert(ARROW_DAMAGE == 4 * (COMBAT_HEALTH_PER_HEART / 2));
	// Launch geometry + hit-box expansion (parity fix 2026-06-19).
	// 2.83 = 1.53 eye - 0.1 ctor + 1.4 (EntitySkeleton.java:40 `var7.posY += (double)1.4F`).
	// Re-pinned 2026-11-01 from the prior 2.43, which read the +1.4 bump as +1.0; the
	// parent build reproduced the old pin (this assert held at 2.43 before the fix).
	assert(feq(SKELETON_ARROW_LAUNCH_DY, 2.83f)); // feet + 1.53 eye - 0.1 ctor + 1.4 lift
	assert(feq(SKELETON_ARROW_TARGET_DY, 0.08f)); // aim at player feet - 0.08
	assert(feq(ARROW_HIT_EXPAND, 0.3f));          // AxisAlignedBB.expand(0.3) before intercept
}

// Beta EntitySkeleton.attackEntity launch geometry: from skel feet + 2.83, aiming at the
// player's POSY - 0.08. Beta's player posY IS the eye (EntityPlayer.yOffset 1.62), so the
// target is the player's eye - 0.08 (the head), NOT the feet. The pre-lead vertical
// component (before the horiz*0.2 gravity lead) must match Beta's arithmetic.
static void test_launch_geometry(void) {
	const float skel_feet = 0.0f, player_feet = 0.0f;         // flat ground
	const float player_eye = player_feet + PLAYER_EYE_HEIGHT; // Beta player.posY == the eye
	float from_y = skel_feet + SKELETON_ARROW_LAUNCH_DY;      // feet + 2.83
	// Faithful target: player.posY(eye) + getEyeHeight(0.12) - 0.2 == eye - 0.08.
	float target_y = player_eye - SKELETON_ARROW_TARGET_DY;
	float ddy = target_y - from_y;
	assert(fabsf(ddy - (-1.29f)) < 1.0e-4f); // Beta: (1.62 - 0.08) - 2.83 = -1.29
	// The first parity pass aimed at the player FEET - 0.08 (= eye - 1.70), burying the shot
	// a full eye-height too low (0.08 below the feet): ddy -2.91. That is the corrected bug.
	float feet_target_ddy = (player_feet - SKELETON_ARROW_TARGET_DY) - from_y;
	assert(fabsf(feet_target_ddy - (-2.91f)) < 1.0e-4f);
	// The pre-parity engine launched at feet+1.55 and aimed at the eye: ddy +0.07 (slightly high).
	float old_ddy = player_eye - (skel_feet + 1.55f);
	assert(fabsf(old_ddy - 0.07f) < 1.0e-4f);
	// The faithful aim points DOWN from the high (2.83) launch; the buggy feet-target
	// pointed far steeper down; the old engine pointed slightly up. All three are distinct.
	assert(ddy < 0.0f && feet_target_ddy < ddy && old_ddy > 0.0f);
}

// Beta EntityArrow.onUpdate integrates DRAG then GRAVITY: motionY_next = motionY*0.99 -
// 0.03. The old gravity-then-drag order gave motionY*0.99 - 0.0297 (1% under-gravity).
static void test_gravity_drag_order(void) {
	float vy = 0.0f;
	vy = vy * ARROW_AIR_DRAG - ARROW_GRAVITY; // tick1
	assert(feq(vy, -0.03f));
	vy = vy * ARROW_AIR_DRAG - ARROW_GRAVITY; // tick2
	assert(fabsf(vy - (-0.0597f)) < 1.0e-5f);
	vy = vy * ARROW_AIR_DRAG - ARROW_GRAVITY; // tick3
	assert(fabsf(vy - (-0.089103f)) < 1.0e-5f);
	// The WRONG (gravity-then-drag) order yields -0.0297 on tick 1, not -0.03.
	float wrong = (0.0f - ARROW_GRAVITY) * ARROW_AIR_DRAG;
	assert(fabsf(wrong - (-0.0297f)) < 1.0e-5f);
	assert(!feq(wrong, -0.03f));
}

static void test_range_ordering(void) {
	// The skeleton ACQUIRES (chases) within the 16-block follow range, but only
	// FIRES within 10 -- so it chases into bow range first.
	assert(SKELETON_SHOOT_RANGE_SQ < MOB_FOLLOW_RANGE_SQ); // 100 < 256
	// And the shoot range is well outside melee reach (it's a ranged threat).
	assert(SKELETON_SHOOT_RANGE_SQ > MOB_ATTACK_REACH_SQ); // 100 > 4
}

static void test_arrow_flight_drops(void) {
	// Launch level at ARROW_SPEED and integrate the engine's per-tick model
	// (vel.y -= gravity; vel *= drag). The arrow must lose height (gravity wins)
	// and slow down (drag). EntityArrow.java:107-125 has no air-age cap.
	float vx = ARROW_SPEED, vy = 0.0f;
	float y = 0.0f, x = 0.0f;
	int age = 0;
	for(int t = 0; t < 101; t++) {
		++age;
		x += vx;
		y += vy;
		// drag THEN gravity (Beta order; parity fix 2026-06-19)
		vx *= ARROW_AIR_DRAG;
		vy *= ARROW_AIR_DRAG;
		vy -= ARROW_GRAVITY;
	}
	assert(age == 101);
	assert(y < 0.0f);                       // dropped below the launch height
	assert(vx < ARROW_SPEED);               // air drag bled horizontal speed
	assert(x > 0.0f);                        // still travelled forward
}

int main(void) {
	test_constants();
	test_range_ordering();
	test_arrow_flight_drops();
	test_launch_geometry();
	test_gravity_drag_order();
	printf("test_skeleton_arrow: ranged attack pinned (fire <10 blocks every %d "
		   "ticks, arrow grav %.2f / drag %.2f / dmg %d, no air-age cap)\n",
		   SKELETON_ARROW_COOLDOWN_TICKS, ARROW_GRAVITY, ARROW_AIR_DRAG,
		   ARROW_DAMAGE);
	return 0;
}
