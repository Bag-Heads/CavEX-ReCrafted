/* Batch J hurt-feedback pin (render/feel layer).
 *
 * Pins the client hurt-state derivation and camera-roll math of
 * source/game/hud_hurt.h against the decompile citations:
 *   [740c583 EntityPlayerSP.setHealth:148-161]  health-delta arming (the
 *                                               Packet8 MP-client contract)
 *   [740c583 EntityLiving.onEntityUpdate:147-157] per-tick decay + deathTime
 *   [740c583 GuiIngame:60-66]                   blink phase + jitter seed
 *   [740c583 EntityRenderer.hurtCameraEffect:152-169] hurt/death roll
 * A drifted constant fails here before it can drift the feel. */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../source/game/hud_hurt.h"

#define FEQ(a, b) (fabsf((a) - (b)) < 1e-6f)

static void test_join_latch_no_flash(void) {
	struct hud_hurt_state s;
	hud_hurt_reset(&s);
	/* First observation (world join) latches silently: no timers armed. */
	hud_hurt_tick(&s, 160, true);
	assert(s.last_health == 160);
	assert(s.hurt_time == 0 && s.hearts_life == 0 && s.death_time == 0);
	assert(!hud_hurt_blink(&s));
	/* Steady health: still nothing. */
	hud_hurt_tick(&s, 160, true);
	assert(s.hurt_time == 0 && s.hearts_life == 0);
}

static void test_damage_arms_and_decays(void) {
	struct hud_hurt_state s;
	hud_hurt_reset(&s);
	hud_hurt_tick(&s, 160, true);

	/* Damage: 160 -> 120. [EntityPlayerSP.setHealth:155-161] arms
	 * prevHealth=old, heartsLife=20, hurtTime=maxHurtTime=10. */
	hud_hurt_tick(&s, 120, true);
	assert(s.prev_health == 160);
	assert(s.hearts_life == 20);
	assert(s.hurt_time == 10 && s.max_hurt_time == 10);
	assert(s.last_health == 120);

	/* Blink phase over the decaying window [GuiIngame:60-63]:
	 * heartsLife/3 % 2 == 1, forced off below 10. Expected per value:
	 * 20..18 off, 17..15 ON, 14..12 off, 11..10 ON, 9 off. */
	static const int expect_on[] = {17, 16, 15, 11, 10};
	for(int t = 0; t < 12; t++) {
		bool on = false;
		for(unsigned i = 0; i < sizeof(expect_on) / sizeof(expect_on[0]); i++)
			if(s.hearts_life == expect_on[i])
				on = true;
		assert(hud_hurt_blink(&s) == on);
		hud_hurt_tick(&s, 120, true); /* steady: pure decay tick */
	}
	/* hurtTime hit 0 within 10 ticks [EntityLiving:147-149]. */
	assert(s.hurt_time == 0);
	assert(s.hearts_life == 20 - 12);
}

static void test_heal_flash(void) {
	struct hud_hurt_state s;
	hud_hurt_reset(&s);
	hud_hurt_tick(&s, 100, true);
	/* Heal: heartsLife = heartsHalvesLife/2 = 10, no hurt kick
	 * [EntityPlayerSP.setHealth:152-154; EntityLiving.heal:303]. */
	hud_hurt_tick(&s, 120, true);
	assert(s.hearts_life == 10);
	assert(s.hurt_time == 0);
	assert(hud_hurt_blink(&s)); /* 10/3=3 odd and >= 10: the brief flash */
	hud_hurt_tick(&s, 120, true);
	assert(s.hearts_life == 9);
	assert(!hud_hurt_blink(&s));
}

static void test_death_and_respawn(void) {
	struct hud_hurt_state s;
	hud_hurt_reset(&s);
	hud_hurt_tick(&s, 40, true);
	hud_hurt_tick(&s, 0, true); /* lethal hit: arms hurt AND starts dying */
	assert(s.hurt_time == 10 && s.prev_health == 40);
	assert(s.death_time == 1);

	/* deathTime rises while dead, frozen once past 20 (Beta removes the
	 * entity at >20 and it stops ticking) [EntityLiving:155-160]. */
	for(int t = 0; t < 30; t++)
		hud_hurt_tick(&s, 0, true);
	assert(s.death_time == 21);

	/* Death roll [EntityRenderer:156-158]: 40 - 8000/(t + partial + 200). */
	float expect = 40.0F - 8000.0F / (21.0F + 0.25F + 200.0F);
	assert(FEQ(hud_hurt_death_roll_deg(&s, 0.25F), expect));

	/* Respawn (dead -> alive) = a fresh Beta MP entity: everything cleared,
	 * no heal flash, no roll. */
	hud_hurt_tick(&s, 160, true);
	assert(s.last_health == 160);
	assert(s.hearts_life == 0 && s.hurt_time == 0 && s.death_time == 0);
	assert(!hud_hurt_blink(&s));
	assert(FEQ(hud_hurt_death_roll_deg(&s, 0.5F), 0.0F));
}

static void test_camera_roll_formula(void) {
	struct hud_hurt_state s;
	hud_hurt_reset(&s);
	hud_hurt_tick(&s, 160, true);
	hud_hurt_tick(&s, 120, true); /* hurt_time = max_hurt_time = 10 */

	/* [EntityRenderer:161-166]: f = (hurtTime - partial)/maxHurtTime;
	 * roll = -sin(f^4 * pi) * 14. Recomputed here with the same float ops. */
	float partial = 0.5F;
	float f = (10.0F - partial) / 10.0F;
	float expect = -sinf(f * f * f * f * (float)M_PI) * 14.0F;
	assert(FEQ(hud_hurt_camera_roll_deg(&s, partial), expect));
	assert(expect < 0.0F); /* the kick tilts, sanity on sign */

	/* Window expiry: hurtTime 0 and partial > 0 -> f < 0 -> no roll
	 * [EntityRenderer:161 gate]. */
	for(int t = 0; t < 10; t++)
		hud_hurt_tick(&s, 120, true);
	assert(s.hurt_time == 0);
	assert(FEQ(hud_hurt_camera_roll_deg(&s, 0.5F), 0.0F));

	/* Never-hurt state at partial exactly 0: Beta reaches 0.0F/0 (NaN);
	 * our guard returns the 0 that frame renders anyway. */
	struct hud_hurt_state fresh;
	hud_hurt_reset(&fresh);
	assert(FEQ(hud_hurt_camera_roll_deg(&fresh, 0.0F), 0.0F));
}

static void test_jitter_seed_java_int_wrap(void) {
	struct hud_hurt_state s;
	hud_hurt_reset(&s);
	/* [GuiIngame:66] setSeed((long)(updateCounter * 312871)) is Java INT
	 * arithmetic: it wraps at 32 bits before widening. 10000 * 312871 =
	 * 3128710000 overflows int32 to -1166257296. */
	s.update_counter = 10000;
	assert(hud_hurt_jitter_seed(&s) == -1166257296LL);
	/* Small counters stay verbatim. */
	s.update_counter = 7;
	assert(hud_hurt_jitter_seed(&s) == 7LL * 312871LL);
}

int main(void) {
	test_join_latch_no_flash();
	test_damage_arms_and_decays();
	test_heal_flash();
	test_death_and_respawn();
	test_camera_roll_formula();
	test_jitter_seed_java_int_wrap();
	printf("test_hud_hurt: all passed\n");
	return 0;
}
