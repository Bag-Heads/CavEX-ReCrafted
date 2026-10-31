/*
	AUDIT #18 mob-polish grab-bag: pins the PURE helpers the engine wires for the
	Beta 1.7.3 mob polish items, all in source/entity/combat.h (so the byte-exact
	values can't drift between the code and this test):

	  ITEM 2  creeper swell INFLATION  -- creeper_swell_scale_xz / _y / _flash_ramp
	          (RenderCreeper.updateCreeperScale flash^4 curve: idle 1.0, max 1.4x XZ /
	          1.1x Y at full fuse, with the f^4 keeping it near-normal until the end).
	  ITEM 3  death animation           -- mob_death_roll_deg / mob_death_finished
	          (EntityLiving.deathTime: 0..20 flop, 90deg roll, removed past 20).
	  ITEM 4  drop COUNT                -- mob_drop_count (rand.nextInt(3) -> {0,1,2}).
	  ITEM 5  pack-spawn SCATTER        -- pack_spawn_scatter_step (the EXACT 6-draw
	          Beta RNG order X:2,Y:2,Z:2; the Y draws cancel in OUTPUT but are consumed).
	  ITEM 7  eye/body light SAMPLE     -- mob_brightness_sample_y (getEntityBrightness
	          feet + 0.66*bodyHeight: the 0.66 fraction is Beta's, applied to v2's own
	          collision box -> biped feet + 0.66*1.8 = feet+1.188, spider feet+0.594).

	Items 1 (skeleton 0.6 arrow speed / flat 4 damage CONFIRMED unchanged) and 6
	(ARROW_KNOCKBACK relabelled a Wii feel value, not Beta) are documentation/no-logic
	resolutions, asserted lightly here against the constants for completeness.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"
#include "entity/monsters_object.h" // MONSTER_* ids

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

// Compares a this-engine RADIANS value against a Beta DEGREES value.
static bool feq_deg(float radians, float degrees) {
	return feq(radians, degrees * (float)M_PI / 180.0f);
}

// --- ITEM 2: creeper swell inflation -----------------------------------------
static void test_creeper_swell_scale(void) {
	// Idle (fuse < 0) -> no inflation, exactly 1.0 on both axes.
	assert(creeper_swell_scale_xz(-1, 0.0f) == 1.0f);
	assert(creeper_swell_scale_y(-1, 0.5f) == 1.0f);
	assert(creeper_flash_ramp(-1, 0.7f) == 0.0f);

	// Ramp f = (fuse + td)/(fuseTime - 2) = /28. At fuse 0 -> f 0 -> scale 1.0.
	assert(creeper_flash_ramp(0, 0.0f) == 0.0f);
	assert(creeper_swell_scale_xz(0, 0.0f) == 1.0f);
	assert(creeper_swell_scale_y(0, 0.0f) == 1.0f);

	// At full prime (fuse 28 -> f 1.0, clamped) the model bulges to the Beta maxima:
	// XZ = 1 + 1^4*0.4 = 1.4, Y = 1 + 1^4*0.1 = 1.1 (WIDER than taller).
	float xz_full = creeper_swell_scale_xz(28, 0.0f);
	float y_full = creeper_swell_scale_y(28, 0.0f);
	assert(xz_full > 1.399f && xz_full < 1.401f); // 1.4x
	assert(y_full > 1.099f && y_full < 1.101f);   // 1.1x
	assert(xz_full > y_full);                     // bulges wider than taller (Beta)

	// f clamps at 1.0 past full fuse (fuse 30 > 28).
	assert(creeper_flash_ramp(30, 0.0f) == 1.0f);
	assert(creeper_swell_scale_xz(30, 0.0f) == xz_full);

	// flash^4 curve: at HALF ramp (f ~ 0.5, fuse 14) the inflation is tiny (0.5^4 =
	// 0.0625 -> XZ 1.025), NOT linear-half (which would be 1.2). This is the whole
	// point of the 4th-power: the creeper stays ~normal until the final ~15%.
	float xz_half = creeper_swell_scale_xz(14, 0.0f); // f = 14/28 = 0.5
	assert(xz_half > 1.02f && xz_half < 1.03f);       // ~1.025, NOT ~1.2
	assert(xz_half < 1.05f);                          // emphatically sub-linear

	// Monotonic: more fuse -> more (or equal) inflation, never less.
	assert(creeper_swell_scale_xz(20, 0.0f) >= creeper_swell_scale_xz(10, 0.0f));
	assert(creeper_swell_scale_xz(28, 0.0f) >= creeper_swell_scale_xz(20, 0.0f));

	// The tint (raw f) and the scale (f^4) read the SAME ramp but differ in shape:
	// at f 0.5 the raw ramp is 0.5 while the scale's f^4 contribution is 0.0625.
	assert(creeper_flash_ramp(14, 0.0f) > 0.49f
		   && creeper_flash_ramp(14, 0.0f) < 0.51f);
}

// --- ITEM 3: death animation (deathTime flop) --------------------------------
static void test_death_animation(void) {
	// Beta removes when deathTime > 20: alive through 20, gone at 21.
	assert(!mob_death_finished(0));
	assert(!mob_death_finished(1));
	assert(!mob_death_finished(MOB_DEATH_TIME_MAX));      // 20: still flopping
	assert(mob_death_finished(MOB_DEATH_TIME_MAX + 1));   // 21: removed
	assert(mob_death_finished(50));

	// Beta RenderLiving.rotateCorpse (next-gaps-0924/G8; the old linear stand-in pinned
	// 45deg here): min(1, sqrt((deathTime + td - 1)/20 * 1.6)) * 90. At death_time 1,
	// td 0 -> 0deg (just dropped); flat once deathTime + td reaches 13.5.
	// test_death_tilt carries the full per-type golden.
	assert(mob_death_roll_deg(1, 0.0f) == 0.0f);
	float mid = mob_death_roll_deg(11, 0.0f); // sqrt(0.8)*90 = 80.498
	assert(mid > 80.49f && mid < 80.51f);
	float flat = mob_death_roll_deg(14, 0.0f); // sqrt(1.04) -> clamped 1 -> 90
	assert(flat == MOB_DEATH_ROLL_MAX_DEG);

	// Clamped: 0 while death_time is 0 (Beta's deathTime > 0 gate) and never above 90.
	assert(mob_death_roll_deg(0, 0.0f) == 0.0f);
	assert(mob_death_roll_deg(40, 0.0f) == MOB_DEATH_ROLL_MAX_DEG); // way past -> 90
	// Monotonic non-decreasing in deathTime.
	assert(mob_death_roll_deg(5, 0.0f) <= mob_death_roll_deg(10, 0.0f));
	assert(mob_death_roll_deg(10, 0.0f) <= mob_death_roll_deg(15, 0.0f));
}

// --- ITEM 4: drop count rand.nextInt(3) --------------------------------------
static void test_drop_count(void) {
	assert(MOB_DROP_COUNT_DEN == 3); // rand.nextInt(3) -> {0,1,2}
	// The three faithful outcomes map straight through.
	assert(mob_drop_count(0) == 0); // Beta drops NOTHING on a 0 roll (loop body skipped)
	assert(mob_drop_count(1) == 1);
	assert(mob_drop_count(2) == 2);
	// rand_gen_int(rng, 3) only ever returns 0..2, but the helper defensively clamps a
	// stray out-of-range value rather than emitting a garbage stack size.
	assert(mob_drop_count(-1) == 0);
	assert(mob_drop_count(3) == MOB_DROP_COUNT_DEN - 1);  // 2
	assert(mob_drop_count(99) == MOB_DROP_COUNT_DEN - 1); // 2
	// A 0 count means "no drop" -- the engine guards server_local_spawn_item on count>0.
	assert(mob_drop_count(0) == 0);
}

// --- ITEM 5: pack-spawn scatter, EXACT 6-draw order --------------------------
// Deterministic RNG stub: replays a scripted sequence of draws, in order, so the test
// can pin the EXACT order pack_spawn_scatter_step consumes them (X:2, Y:2, Z:2) and the
// fact that the Y draws are consumed even though they cancel.
struct stub_rng {
	const int* vals;
	int n;
	int i;
};
static int stub_next(void* ctx, int max) {
	struct stub_rng* r = (struct stub_rng*)ctx;
	assert(r->i < r->n);   // must not over-draw
	(void)max;             // the order, not the bound, is under test here
	return r->vals[r->i++];
}

static void test_pack_scatter(void) {
	assert(PACK_SPAWN_SCATTER == 6); // byte var25 horizontal radius
	assert(PACK_SPAWN_MEMBERS == 4); // 4 members per pack seed

	// Script the 6 draws for ONE member, in Beta order:
	//   X: nextInt(6)=5, nextInt(6)=1  -> dx = 5 - 1 = +4
	//   Y: nextInt(1)=0, nextInt(1)=0  -> dy = 0 (always; both consumed)
	//   Z: nextInt(6)=0, nextInt(6)=3  -> dz = 0 - 3 = -3
	int seq[6] = {5, 1, 0, 0, 0, 3};
	struct stub_rng r = {seq, 6, 0};
	int x = 100, y = 64, z = 200;
	pack_spawn_scatter_step(stub_next, &r, &x, &y, &z);
	assert(r.i == 6);   // ALL SIX draws consumed (incl. the 2 no-op Y draws) -- the
	                    // determinism contract: the seeded stream advances by 6/member.
	assert(x == 104);   // 100 + 4
	assert(y == 64);    // unchanged: the Y draws cancel (nextInt(1) is always 0)
	assert(z == 197);   // 200 - 3

	// A second member walks off the FIRST member's new position (chained random-walk):
	//   X: 2 - 2 = 0 (no move) ; Y: 0 ; Z: 6th... here 4 - 0 = +4
	int seq2[6] = {2, 2, 0, 0, 4, 0};
	struct stub_rng r2 = {seq2, 6, 0};
	pack_spawn_scatter_step(stub_next, &r2, &x, &y, &z);
	assert(r2.i == 6);
	assert(x == 104);   // 104 + 0
	assert(y == 64);
	assert(z == 201);   // 197 + 4 -- chained off the previous member, not the anchor
}

// --- ITEM 7: eye/body light sample point -------------------------------------
static void test_brightness_sample(void) {
	assert(MOB_BRIGHTNESS_SAMPLE_FRAC == 0.66f);
	// Biped: feet + 0.66 * the inherited 1.8 Entity height = feet + 1.188.
	float feet = 64.0f;
	float biped = mob_brightness_sample_y(feet, MONSTER_ZOMBIE);
	float expect_biped = feet + 0.66f * MOB_BODY_HEIGHT; // 64 + 1.188
	assert(biped > expect_biped - 0.001f && biped < expect_biped + 0.001f);
	// The biped sample is ABOVE the feet block (lands ~1 block up) -- the whole point of
	// item 7 vs the old foot-cell sample.
	assert(biped > feet + 1.0f);
	// Skeleton is a biped too: same sample as the zombie.
	assert(mob_brightness_sample_y(feet, MONSTER_SKELETON) == biped);

	// Spider (0.9-tall box): feet + 0.66 * 0.9 = feet + 0.594 -> still the FEET block in
	// most cases (< feet + 1), distinct from the biped's +1.188.
	float spider = mob_brightness_sample_y(feet, MONSTER_SPIDER);
	float expect_spider = feet + 0.66f * SPIDER_BBOX_HEIGHT; // 64 + 0.594
	assert(spider > expect_spider - 0.001f && spider < expect_spider + 0.001f);
	assert(spider < feet + 1.0f); // same (feet) block -- floor(feet+0.594)==floor(feet)
	assert(spider < biped);       // the squat spider samples lower than a biped

	// The 0.66 BODY sample is NOT the 0.85 LOS eye -- they must differ (item 7's whole
	// point: getEntityBrightness != canEntityBeSeen). For a biped, eye = 1.53 > 1.188.
	assert(mob_eye_height(MONSTER_ZOMBIE) > 0.66f * MOB_BODY_HEIGHT);
}

// --- AUDIT-DOSSIER fix: natural-spawn WORLD-SPAWN exclusion (SpawnerAnimals) --
// SpawnerAnimals.performSpawning [740c583] requires EVERY spawn candidate (all
// 3 creature categories: monster/animal/water -- ONE shared check) to clear a
// 24-block (576.0F = 24^2) 3D distance from World.getSpawnPoint() before an
// entity is even constructed. server_local.c's 3 natural spawn passes had NO
// such check at all until this fix; mob_spawn_far_from_world_spawn is the ONE
// canonical gate now shared by the anchor AND every pack member in all 3 passes.
static void test_world_spawn_exclusion(void) {
	assert(WORLD_SPAWN_EXCLUSION_DIST_SQ == 576.0f); // 24*24 [SpawnerAnimals var33>=576.0F]

	// Beta's exact candidate offset: X/Z get +0.5 (block-center), Y does not
	// [var27=x+0.5F, var28=y, var29=z+0.5F vs the raw ChunkCoordinates spawn
	// point var35]. A candidate whose BLOCK coords equal the spawn point's is
	// therefore NOT distance 0 (it's 0.5 off on X and Z) -- still well inside
	// the 24-block bubble, so it must be excluded.
	assert(!mob_spawn_far_from_world_spawn(100.0f, 64.0f, 100.0f, 100, 64, 100));

	// Exactly at the 24-block boundary: Beta's test is `>= 576.0F`, so the
	// boundary itself is INCLUDED (allowed to spawn), not excluded.
	// dx = (123.5+0.5)-100 = 24, dy = 0, dz = (99.5+0.5)-100 = 0 -> distSq=576.
	assert(mob_spawn_far_from_world_spawn(123.5f, 64.0f, 99.5f, 100, 64, 100));
	// Just inside the boundary (dx=23.9): still excluded.
	assert(!mob_spawn_far_from_world_spawn(123.4f, 64.0f, 99.5f, 100, 64, 100));

	// Far away on any single axis clears the exclusion outright.
	assert(mob_spawn_far_from_world_spawn(500.0f, 64.0f, 500.0f, 100, 64, 100));

	// The test is full 3D, not a 2D/XZ ring -- vertical distance counts too.
	// dy=20 (< 24) -> still excluded; dy=25 (> 24) -> clear.
	assert(!mob_spawn_far_from_world_spawn(99.5f, 84.0f, 99.5f, 100, 64, 100));
	assert(mob_spawn_far_from_world_spawn(99.5f, 89.0f, 99.5f, 100, 64, 100));
}

// --- Idle head-turn locomotion (mob-AI fix wave, entity_monster.c's
// mob_idle_look_tick): pins the pure rate-limited turn helper Beta's
// EntityLiving.updateRotation implements [740c583], and the constants it's
// called with. Not part of AUDIT #18 -- colocated here because it is the
// same "pure combat.h helper" shape as the rest of this file.
static void test_idle_turn_toward(void) {
	// Beta constants, pinned against INDEPENDENTLY-computed radian literals
	// (not re-derived via the same *PI/180 expression the macros use, so a
	// typo in the macro's own formula would still be caught): 10deg yaw
	// rate, 8-block range, 20deg random-drift span, in this engine's
	// RADIANS convention.
	assert(feq(MOB_IDLE_YAW_TURN_RATE, 0.17453292f));    // 10 degrees
	assert(feq(MOB_IDLE_RANDOM_YAW_RANGE, 0.34906585f)); // 20 degrees
	assert(feq_deg(MOB_IDLE_YAW_TURN_RATE, 10.0f));      // cross-check both ways
	assert(feq_deg(MOB_IDLE_RANDOM_YAW_RANGE, 20.0f));
	assert(MOB_IDLE_LOOK_RANGE_SQ == 64.0f);
	assert(MOB_IDLE_ACQUIRE_CHANCE == 0.02f);
	assert(MOB_IDLE_YAW_REROLL_CHANCE == 0.05f);

	// Already facing the target: no-op.
	assert(feq(mob_turn_toward(0.0f, 0.0f, MOB_IDLE_YAW_TURN_RATE), 0.0f));

	// Small delta (under the rate cap): closes COMPLETELY in one call.
	float small = mob_turn_toward(0.0f, 0.05f, MOB_IDLE_YAW_TURN_RATE);
	assert(feq(small, 0.05f));

	// Large delta (over the rate cap): steps by EXACTLY the rate, no more --
	// this is what makes the turn gradual/smooth rather than snapping.
	float stepped = mob_turn_toward(0.0f, 3.0f, MOB_IDLE_YAW_TURN_RATE);
	assert(feq(stepped, MOB_IDLE_YAW_TURN_RATE));
	// Repeated calls monotonically approach the target without overshoot.
	float yaw = 0.0f;
	for(int i = 0; i < 200; i++)
		yaw = mob_turn_toward(yaw, 3.0f, MOB_IDLE_YAW_TURN_RATE);
	assert(feq(yaw, 3.0f));

	// Wrap-around: turning from just past +PI toward just past -PI takes the
	// SHORT way (a small step across the seam), not the long way around.
	float pi = (float)M_PI;
	float near_seam
		= mob_turn_toward(pi - 0.01f, -pi + 0.01f, MOB_IDLE_YAW_TURN_RATE);
	// Should step FORWARD across the seam (toward +PI and wrap), not
	// backward toward 0 -- i.e. it should NOT have moved by ~3.13 radians.
	float moved = near_seam - (pi - 0.01f);
	while(moved < -pi)
		moved += 2.0f * pi;
	while(moved >= pi)
		moved -= 2.0f * pi;
	assert(fabsf(moved) <= MOB_IDLE_YAW_TURN_RATE + 1.0e-4f);

	// Direction sign: turning toward a target to the LEFT (negative) moves
	// current DOWN, not up.
	float left = mob_turn_toward(0.0f, -3.0f, MOB_IDLE_YAW_TURN_RATE);
	assert(left < 0.0f);
}

// --- ITEMS 1 + 6: documentation resolutions, light constant guards ------------
static void test_arrow_constants_unchanged(void) {
	// ITEM 1: the b1.7.3 skeleton arrow speed STAYS 0.6 (the 1.6F is post-Beta) and the
	// on-hit damage STAYS the flat Beta values (no velocity scaling). Pin them so a future
	// "correction" to 1.6 / velocity-scaling trips this test.
	assert(ARROW_SPEED == 0.6f);            // skeleton setArrowHeading 4th arg
	assert(ARROW_SPEED_BOW == 1.5f);        // player bow
	assert(arrow_hit_damage(true) == ARROW_DAMAGE);          // player-scale flat 32
	assert(arrow_hit_damage(false) == ARROW_DAMAGE_BOW_BASE); // mob-scale flat 4
	assert(arrow_launch_speed(false) == ARROW_SPEED);   // skeleton 0.6
	assert(arrow_launch_speed(true) == ARROW_SPEED_BOW); // bow 1.5
	// ITEM 6: ARROW_KNOCKBACK is retained (still used by arrow_damage_mob) but is a v2
	// Wii-feel value, documented in combat.h as NON-Beta -- assert it stays the gentler
	// 0.18 (below the melee KNOCKBACK_H 0.4 it is deliberately softer than).
	assert(ARROW_KNOCKBACK == 0.18f);
	assert(ARROW_KNOCKBACK < KNOCKBACK_H); // gentler than the melee/player recoil
}

int main(void) {
	test_creeper_swell_scale();
	test_death_animation();
	test_drop_count();
	test_pack_scatter();
	test_brightness_sample();
	test_world_spawn_exclusion();
	test_arrow_constants_unchanged();
	test_idle_turn_toward();
	printf("test_mob_polish: audit #18 pinned -- creeper swell^4 scale (1.4x/1.1x), "
		   "deathTime flop (>20 removed, 90deg roll), drop nextInt(3), pack scatter "
		   "6-draw order, getEntityBrightness 0.66-body sample, world-spawn 24-block "
		   "exclusion (576.0F, mixed +0.5 offset), arrow 0.6/flat-4 kept, idle "
		   "head-turn rate-limited turn pinned.\n");
	return 0;
}
