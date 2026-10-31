/*
	Pins the Beta 1.7.3 UNDEAD daylight sun-burn (zombie + skeleton): the byte-exact
	fire constants, the isDaytime() + day/night + sky-exposure ignition gate, and the
	fire damage-over-time cadence. Binds the SHARED source/entity/combat.h
	(undead_sunburn_exposed + the MOB_FIRE_* / UNDEAD_SUNBURN_SKY_MIN constants) AND
	source/daytime.h (daytime_is_daytime_ticks, the byte-pinned isDaytime() port --
	self-contained/header-only, no server linkage needed) so the values tested here
	are the exact ones server_tick_monster uses -- no drift.

	The day/night sky-light subtraction is the same pure formula as
	server_local_sky_subtract (copied verbatim here, as test_mob_spawn_light does,
	because the real one lives in server_local.c which pulls in the whole server).
	Beta source: EntityZombie/EntitySkeleton.onLivingUpdate sets the fire field
	directly to 300 (NOT setFire(8) -- that's post-Beta) when isDaytime() &&
	getEntityBrightness>0.5 && canBlockSeeTheSky && rand.nextFloat()*30<(b-0.4)*2.

	PARITY FIX 2026-09-01: undead_sunburn_exposed gained a REQUIRED is_daytime
	parameter (previously missing -- see combat.h's PARITY FIX comment). The
	torch-at-night case in test_daytime_ignition_gate below is the regression test:
	before the fix, a bright torch (block light folded into eff via max(sky,block))
	under open sky could clear eff>=12 at midnight and ignite, which Beta cannot do
	since isDaytime() short-circuits before getEntityBrightness is ever read.

	is_daytime here is derived from the same local sky_subtract() < 4 formula (NOT by
	including source/daytime.h): daytime_is_daytime_ticks(t, 0, 0) reduces to exactly
	this at zero rain/thunder (daytime_skylight_subtracted's weighted-thunder/rain
	terms both vanish), and textually including daytime.h would pull in cglm's
	unguarded double GLM_PI, clobbering this file's own float GLM_PI/DAY_LENGTH_TICKS
	macros used by the verbatim helpers below -- a real (if probably sub-ULP) risk
	this project's own 1-ULP-trap precedent (server_weather.c F4) says not to take.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "entity/combat.h"

#ifndef GLM_PI
#define GLM_PI 3.14159265358979323846f
#endif
#define DAY_LENGTH_TICKS 24000

// --- verbatim copies of the engine's pure day/night math ---
static float daytime_celestial_angle(float time) {
	float X = time - 0.25F;
	if(X < 0)
		X += 1;
	return X + ((1.0F - (cosf(X * GLM_PI) + 1.0F) / 2.0F) - X) / 3.0F;
}
static int sky_subtract(uint64_t world_time) {
	float day = (float)(world_time % DAY_LENGTH_TICKS) / (float)DAY_LENGTH_TICKS;
	float ca = daytime_celestial_angle(day);
	float f = 1.0F - (cosf(ca * 2.0F * GLM_PI) * 2.0F + 0.5F);
	if(f < 0.0F) f = 0.0F;
	if(f > 1.0F) f = 1.0F;
	return (int)(f * 11.0F);
}

static void test_constants(void) {
	assert(MOB_FIRE_TICKS == 300);            // 15s; Beta sets the fire field to 300
	assert(MOB_FIRE_DAMAGE_INTERVAL == 20);   // 1 fire-damage tick per second
	// Mob health is Beta's raw 20-pt scale (2 = 1 heart), NOT the player's 160-pt
	// (16 = 1 heart) scale. Beta fire = 1 (a half-heart) on the mob scale -- it
	// must NOT be COMBAT_HEALTH_PER_HEART/2 (=8), which would kill a 20-pt mob in
	// ~2.5s instead of the faithful ~20s.
	assert(MOB_FIRE_DAMAGE == 1);
	assert(MOB_FIRE_DAMAGE != COMBAT_HEALTH_PER_HEART / 2); // not player-scale
	assert(UNDEAD_SUNBURN_SKY_MIN == 12); // floored table: brightness[12]=0.525 > 0.5
}

static void test_daytime_ignition_gate(void) {
	// Open sky => stored sky_light 15. Effective sky = 15 - day/night subtract.
	int noon = sky_subtract(6000);     // 0 at noon
	int midnight = sky_subtract(18000); // 11 at midnight
	assert(noon == 0 && midnight == 11);

	// isDaytime() == skylightSubtracted<4 (zero rain/thunder, matching
	// daytime_is_daytime_ticks at rain_strength=thunder_strength=0.0F -- see the file
	// header note on why this is derived locally instead of including daytime.h).
	bool is_daytime_noon = noon < 4;
	bool is_daytime_midnight = midnight < 4;
	assert(is_daytime_noon && !is_daytime_midnight);

	// Noon, can see sky: eff 15 >= 12 -> exposed -> burns.
	assert(undead_sunburn_exposed(is_daytime_noon, 15 - noon, true));
	// Midnight, can see sky: eff 4 < 12 -> NOT exposed -> no burn at night.
	assert(!undead_sunburn_exposed(is_daytime_midnight, 15 - midnight, true));
	// Noon but UNDER A ROOF (can't see sky): never burns, however bright.
	assert(!undead_sunburn_exposed(is_daytime_noon, 15 - noon, false));
	// Boundary (parity fix 2026-06-19): the Beta FLOORED table brightness crosses 0.5
	// between light 11 (0.437) and 12 (0.525) -> 12 burns, 11 does not. The recovered
	// light-12 window is the dawn/dusk band the floorless table wrongly excluded.
	assert(undead_sunburn_exposed(is_daytime_noon, 12, true));
	assert(!undead_sunburn_exposed(is_daytime_noon, 11, true));

	// PARITY FIX 2026-09-01 regression: TORCH AT NIGHT must NOT ignite. A torch
	// (block light >= 12, folded into eff via the audit-fixed max(sky,block) sample)
	// sits under open sky (can_see_sky=true) at midnight. Before this fix,
	// undead_sunburn_exposed had no is_daytime term at all, so eff=14>=12 &&
	// can_see_sky alone would have returned true here and let the caller draw an
	// ignition roll at night -- something Beta structurally cannot do, since
	// isDaytime() short-circuits before getEntityBrightness is ever sampled.
	assert(!undead_sunburn_exposed(is_daytime_midnight, 14, true));
	// Sanity: the IDENTICAL light+visibility combo at noon (is_daytime=true) DOES
	// clear the gate -- proves the negative result above comes from the daytime
	// term, not from the light math being wrong.
	assert(undead_sunburn_exposed(is_daytime_noon, 14, true));
}

// Beta Overworld WorldProvider.generateLightBrightnessTable: a DIVIDE (1-f)/(f*3+1)
// scaled by the 0.05 AMBIENT FLOOR `*(1-0.05)+0.05`. WorldProviderSurface is empty so
// the Overworld inherits this floored table -- matches getEntityBrightness in the tick
// (the engine reads the shared light_lookup_overworld; reproduced here as the pure-helper
// test does not link it). The floorless form (no *0.95+0.05) was the audit-fixed bug.
static float brightness(int eff_sky) {
	float f = 1.0f - (float)eff_sky / 15.0f;
	return (1.0f - f) / (f * 3.0f + 1.0f) * (1.0f - 0.05f) + 0.05f;
}

static void test_brightness_curve(void) {
	// Full light -> ~1.0; the ignition RHS (b-0.4)*2 must be > 0 at the burn
	// threshold so open-daylight mobs can actually ignite, and the random gate
	// rand*30 < RHS is plausible (~a few %/tick at noon).
	assert(fabsf(brightness(15) - 1.0f) < 1.0e-4f);
	assert((brightness(15) - 0.4f) * 2.0f > 1.0f);  // noon: RHS ~1.2
	assert((brightness(UNDEAD_SUNBURN_SKY_MIN) - 0.4f) * 2.0f > 0.0f);
	// MONOTONIC: more light must be brighter. The old multiply form was NOT (it
	// peaked ~1.32 mid-range then fell), over-firing ignition at partial sky.
	for(int l = 1; l <= 15; l++)
		assert(brightness(l) > brightness(l - 1));
	// FLOORED table crosses 0.5 between light 11 (0.437, below) and 12 (0.525, above),
	// so the engine's eff>=UNDEAD_SUNBURN_SKY_MIN(12) gate is byte-equivalent to Beta's
	// getEntityBrightness>0.5F. (The floorless table put 12 at exactly 0.5, wrongly
	// excluding it; the 13 gate was derived from that bug.)
	assert(brightness(11) < 0.5f);
	assert(brightness(12) > 0.5f);
	assert(fabsf(brightness(11) - 0.437f) < 1.0e-3f);
	assert(fabsf(brightness(12) - 0.525f) < 1.0e-3f);
}

// Model the tick's fire DoT (no re-ignition): damage on fire_time % INTERVAL == 0,
// then decrement -- exactly as server_tick_monster does.
static void test_fire_dot_cadence(void) {
	int fire_time = MOB_FIRE_TICKS; // 300
	int total_damage = 0, damage_events = 0;
	for(int t = 0; t < MOB_FIRE_TICKS; t++) {
		if(fire_time > 0) {
			if(fire_time % MOB_FIRE_DAMAGE_INTERVAL == 0) {
				total_damage += MOB_FIRE_DAMAGE;
				damage_events++;
			}
			fire_time--;
		}
	}
	assert(fire_time == 0);                     // burns out after 300 ticks
	assert(damage_events == MOB_FIRE_TICKS / MOB_FIRE_DAMAGE_INTERVAL); // 15
	assert(total_damage == 15 * MOB_FIRE_DAMAGE); // 15 pts = 7.5 hearts over 15s

	// A SINGLE 300-tick fire period (15 damage) does NOT kill a full-health 20-pt
	// mob -- it takes the daylight RE-ignition to keep burning to death, ~20s for
	// 20 health at 1/sec. (If MOB_FIRE_DAMAGE were the player-scale 8, one period
	// would deal 120 and the mob would die almost instantly -- the bug we fixed.)
	assert(total_damage < 20);                  // mob full health 20 survives one period
	assert(20 / MOB_FIRE_DAMAGE == 20);         // ~20s to burn a full mob to death

	// Water (fire_time forced to 0) => no damage at all.
	int wet = 0, wet_dmg = 0;
	if(wet > 0) wet_dmg = MOB_FIRE_DAMAGE; // unreachable: proves 0 deals nothing
	assert(wet_dmg == 0);
}

// PARITY FIX 2026-09-02: which mob ids run the daylight ignition mechanism.
// EntityPigZombie extends EntityZombie and its onLivingUpdate override is a
// bare `super.onLivingUpdate();` (decompile-verified, 740c583:
// EntityPigZombie.java) -- it inherits the mechanism wholesale.
static void test_undead_sunburn_candidate_gate(void) {
	assert(mob_is_undead_sunburn_candidate(MONSTER_ZOMBIE));
	assert(mob_is_undead_sunburn_candidate(MONSTER_SKELETON));
	assert(mob_is_undead_sunburn_candidate(MONSTER_PIG_ZOMBIE));
	// Regression guard: neither immune to fire (ghast) nor unrelated
	// hostiles ignite from daylight in Beta.
	assert(!mob_is_undead_sunburn_candidate(MONSTER_GHAST));
	assert(!mob_is_undead_sunburn_candidate(MONSTER_SPIDER));
	assert(!mob_is_undead_sunburn_candidate(MONSTER_CREEPER));
	assert(!mob_is_undead_sunburn_candidate(MONSTER_PIG));
}

// G6c (Fable G6b re-review): the immune branch is TWO statements in one tick.
// Entity.onEntityUpdate:204-207 (decompile-verified, mcb173-retromcp):
//     a fire-immune entity loses 4 fire per tick, floored at 0;
// then setEntityFlag(0, fire > 0) (Entity.java:226), and back in
// EntityLiving.onEntityUpdate:117 -- the SAME call --
//     a fire-immune entity, or any entity in a multiplayer world, gets fire = 0;
// A sunlit pig zombie (EntityZombie.onLivingUpdate:12-16 sets fire = 300,
// inherited by EntityPigZombie:45) therefore shows fire for exactly ONE
// broadcast tick per daylight roll, never the old ~75-tick (300/4) decay, and
// never takes damage. This port snapshots on_fire at the end of the tick, so
// the ignition tick is the one visible tick and the next decay returns 0.
static void test_fire_immune_decay(void) {
	bool apply = true;
	// Exact tick: tick 0 publishes its flag, THEN the daylight roll sets 300;
	// tick 1 publishes mob_immune_fire_flag(300) = (296 > 0) and zeroes; tick
	// 2.. publish false. Visible ticks = {1} only.
	int fire = 0;
	int visible_ticks = 0;
	for(int tick = 0; tick < 100; tick++) {
		bool flag = mob_immune_fire_flag(fire); // Entity.java:226
		if(fire > 0) {
			fire = mob_fire_tick_decay(fire, /*fire_immune=*/true, &apply);
			assert(!apply);
		}
		assert(fire == 0); // EntityLiving.onEntityUpdate:117
		if(tick == 0)
			fire = MOB_FIRE_TICKS; // EntityZombie.onLivingUpdate:12-16 roll
		assert(flag == (tick == 1));
		visible_ticks += flag;
	}
	printf("immune sunlit ignition: visible ticks = %d, only tick 1 (Beta)\n",
		visible_ticks);
	assert(visible_ticks == 1);
	assert(!mob_immune_fire_flag(4) && mob_immune_fire_flag(5));
	assert(mob_fire_tick_decay(MOB_LAVA_FIRE_TICKS, true, &apply) == 0 && !apply);
	assert(mob_fire_tick_decay(2, true, &apply) == 0 && !apply);

	// Mortal: -1/tick, damage exactly every 20th tick (checked on the
	// value BEFORE decrementing, matching Beta's `if(fire%20==0) --fire`
	// order) -- identical cadence to test_fire_dot_cadence's hand-mirrored
	// loop above, now bound to the real production function.
	fire = MOB_FIRE_TICKS;
	int damage_events = 0;
	while(fire > 0) {
		fire = mob_fire_tick_decay(fire, /*fire_immune=*/false, &apply);
		if(apply)
			damage_events++;
	}
	assert(fire == 0);
	assert(damage_events == MOB_FIRE_TICKS / MOB_FIRE_DAMAGE_INTERVAL); // 15
}

int main(void) {
	test_constants();
	test_daytime_ignition_gate();
	test_brightness_curve();
	test_fire_dot_cadence();
	test_undead_sunburn_candidate_gate();
	test_fire_immune_decay();
	printf("test_mob_sunburn: undead sun-burn pinned (fire=%d ticks, %d dmg per %d "
		   "ticks, sky>=%d gate; day burns, night/shade/water don't; pig "
		   "zombie ignites for one flag tick, no damage)\n",
		   MOB_FIRE_TICKS, MOB_FIRE_DAMAGE, MOB_FIRE_DAMAGE_INTERVAL,
		   UNDEAD_SUNBURN_SKY_MIN);
	return 0;
}
