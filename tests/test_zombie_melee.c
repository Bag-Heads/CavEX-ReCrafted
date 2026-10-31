/*
	Pins the Beta 1.7.3 mob->player melee slice: damage value, attack reach,
	vertical overlap, knockback direction, and the i-frame / swing-cooldown state
	machine.

	It binds the SHARED source/entity/combat.h directly, so the byte-faithful Beta
	constants and the pure helpers tested here are the exact ones the engine
	(entity_monster.c sweep + server_local_hurt_player) uses -- they cannot drift.
	The per-tick state machine is modelled to mirror the engine's stepping order
	exactly:
	  1. server_local_update decrements the player's hurt_resist_time ONCE/tick
	     (before the entity loop),
	  2. server_tick_monster decrements the mob's attack_time, then
	  3. if attack_time==0 and the player is in reach, it swings ->
	     server_local_hurt_player (which absorbs the hit while i-framed, else
	     applies damage and opens a fresh 10-tick window) and resets attack_time=20.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

// --- model of the engine's per-tick melee stepping (mirrors the engine) -------
struct sim {
	short health;
	short hurt_resist_time; // player i-frame counter
	int attack_time;		// mob swing cooldown
	int damage_events;		// applied (non-absorbed) hits
	bool died;
};

// Mirrors server_local_hurt_player's i-frame + damage gate (knockback is checked
// separately via melee_knockback_dir).
static void sim_hurt(struct sim* s) {
	if(s->hurt_resist_time > 0)
		return; // absorbed by i-frames
	s->hurt_resist_time = PLAYER_HURT_RESIST_TICKS;
	s->health -= ZOMBIE_ATTACK_DAMAGE;
	s->damage_events++;
	if(s->health <= 0)
		s->died = true;
}

// One server tick against a single mob at a fixed reach state. force_atk0 /
// force_hrt0 let a test isolate one counter to prove the two are independent.
static void sim_tick(struct sim* s, bool in_reach, bool force_atk0,
					 bool force_hrt0) {
	if(s->hurt_resist_time > 0)
		s->hurt_resist_time--; // (1) once-per-tick player i-frame decrement
	if(force_hrt0)
		s->hurt_resist_time = 0;

	if(force_atk0)
		s->attack_time = 0;
	if(s->attack_time > 0)
		s->attack_time--; // (2) mob swing-cooldown decrement
	if(s->attack_time == 0 && in_reach) { // (3) swing
		sim_hurt(s);
		s->attack_time = MOB_ATTACK_COOLDOWN_TICKS;
	}
}

static void test_constants(void) {
	// Beta 1.7.3 values, pinned. Zombie melee = 5 half-hearts (2.5 hearts), flat.
	assert(COMBAT_HEALTH_PER_HEART == 16);
	assert(ZOMBIE_ATTACK_DAMAGE == 40);
	assert(ZOMBIE_ATTACK_DAMAGE == 5 * (COMBAT_HEALTH_PER_HEART / 2));
	assert(MOB_ATTACK_COOLDOWN_TICKS == 20);
	assert(PLAYER_HURT_RESIST_TICKS == 10);
	assert(feq(MOB_ATTACK_REACH_SQ, 4.0f));
	assert(feq(KNOCKBACK_H, 0.4f));
	assert(feq(KNOCKBACK_V, 0.4f));
}

static void test_reach_and_overlap(void) {
	// 3D reach: dist^2 < 4.0 (2 blocks). On flat ground (dy=0) this is the old
	// horizontal gate, byte-identical.
	assert(melee_in_reach_sq(1.9f, 0.0f, 0.0f));  // 3.61 < 4
	assert(!melee_in_reach_sq(2.1f, 0.0f, 0.0f)); // 4.41 >= 4
	assert(melee_in_reach_sq(1.4f, 0.0f, 1.4f));  // 3.92 < 4
	assert(!melee_in_reach_sq(1.5f, 0.0f, 1.5f)); // 4.50 >= 4
	assert(melee_in_reach_sq(0.0f, 0.0f, 0.0f));  // touching
	// Parity fix 2026-06-19: the Y term couples vertical separation. A target that is
	// horizontally in-reach but vertically offset can exceed the 2.0-block 3D radius --
	// Beta (getDistanceToEntity < 2.0) rejects it; the old X/Z-only gate wrongly admitted it.
	assert(melee_in_reach_sq(1.9f, 0.0f, 0.0f));  // flat: 3.61 < 4 (hits)
	assert(!melee_in_reach_sq(1.9f, 0.9f, 0.0f)); // +0.9 up: 3.61+0.81=4.42 >= 4 (no hit)
	assert(melee_in_reach_sq(1.2f, 1.0f, 1.0f));  // 1.44+1+1=3.44 < 4 (hits)
	assert(!melee_in_reach_sq(1.2f, 1.5f, 1.0f)); // 1.44+2.25+1=4.69 >= 4 (no hit)

	// Vertical AABB overlap: player [feet, feet+1.8]. zh is the biped COLLISION box
	// (mob_body_height == MOB_BODY_HEIGHT, Entity's inherited 1.8).
	const float zh = mob_body_height(MONSTER_ZOMBIE);
	assert(feq(zh, MOB_BODY_HEIGHT));
	assert(melee_y_overlap(64.0f, 64.0f, zh));	   // same ground
	assert(melee_y_overlap(64.0f, 65.7f, zh));	   // mob just below player's head
	assert(!melee_y_overlap(64.0f, 65.81f, zh));   // mob feet above player's head
	assert(!melee_y_overlap(64.0f, 67.0f, zh));	   // mob 3 blocks above
	assert(!melee_y_overlap(64.0f, 61.0f, zh));	   // mob 3 blocks below

	// MELEE GATE box height: Beta Entity's constructor initializes width=0.6F,
	// height=1.8F, and EntityLiving inherits that collision box unchanged. The
	// melee gate and collision box therefore use the same faithful 1.8 height;
	// CaveWii's former 1.7 collision box was the parity bug.
	// [decompile 740c583 Entity.<init>, EntityLiving.<init>]
	const float zmh = mob_melee_box_height(MONSTER_ZOMBIE);
	assert(feq(zmh, MOB_BODY_HEIGHT_BETA)); // 1.8
	assert(feq(zmh, zh));                    // melee and collision boxes agree
	// Player feet 1.75 above the mob's feet: inside the 1.8 melee box (m_top 65.8 > 65.75)
	// but the old 1.7 CaveWii box (m_top 65.7) would MISS.
	assert(melee_y_overlap(65.75f, 64.0f, zmh));  // faithful 1.8 melee box reaches
	assert(!melee_y_overlap(65.75f, 64.0f, 1.7f)); // old short box would miss
	// Non-biped mobs keep their faithful box_h for the melee gate (spider 0.9):
	assert(feq(mob_melee_box_height(MONSTER_SPIDER), mob_body_height(MONSTER_SPIDER)));

	// SPIDER (audit #17a): its melee box is only 0.9 tall, so it CANNOT bite a player
	// whose feet sit between 0.9 and 1.7 above the spider's feet -- the upper 0.8 blocks
	// that the old fixed-1.7 height phantom-covered. Same scene as above proves the
	// per-id difference: a player standing 1.0 block above the spider's feet is OUT of
	// the 0.9 spider box but WAS inside the 1.7 biped box.
	const float sh = mob_body_height(MONSTER_SPIDER);
	assert(feq(sh, SPIDER_BBOX_HEIGHT)); // 0.9
	assert(sh < zh);                     // squat: 0.9 < 1.7
	// player feet 1.0 above the spider's feet: spider_top = m_feet+0.9 = m_feet+0.9,
	// i.e. 64.9 < player feet 65.0 -> NO overlap (the 0.9 body stops below the player).
	assert(!melee_y_overlap(65.0f, 64.0f, sh)); // spider 1.0 below player: no bite
	assert(melee_y_overlap(64.85f, 64.0f, sh)); //  within the 0.9 body: bite
	// The SAME geometry against a 1.7 biped box WOULD overlap (phantom reach the spider
	// no longer has): biped_top 64+1.7=65.7 > player feet 65.0.
	assert(melee_y_overlap(65.0f, 64.0f, zh));  // biped reaches; spider (above) does not
}

static void test_knockback_dir(void) {
	float kx, kz;
	// Mob due-west (player - mob = +x) -> player pushed east (+x).
	melee_knockback_dir(1.0f, 0.0f, &kx, &kz);
	assert(feq(kx, 1.0f) && feq(kz, 0.0f));
	// Mob due-east -> player pushed west (-x).
	melee_knockback_dir(-1.0f, 0.0f, &kx, &kz);
	assert(feq(kx, -1.0f) && feq(kz, 0.0f));
	// 3-4-5: normalized to (0.6, 0.8).
	melee_knockback_dir(3.0f, 4.0f, &kx, &kz);
	assert(feq(kx, 0.6f) && feq(kz, 0.8f));
	// Perfect overlap: finite fallback (+x), no NaN.
	melee_knockback_dir(0.0f, 0.0f, &kx, &kz);
	assert(!isnan(kx) && !isnan(kz));
	assert(feq(kx, 1.0f) && feq(kz, 0.0f));
}

// Continuous contact: exactly one hit, then no second hit until BOTH the 10-tick
// i-frame window and the 20-tick mob cooldown have elapsed -> the slower (20)
// gates, so the second hit lands at tick 21.
static void test_single_hit_then_iframes(void) {
	struct sim s = {.health = 160};
	sim_tick(&s, true, false, false); // tick 1
	assert(s.damage_events == 1);
	assert(s.health == 120); // 160 - 40
	assert(s.hurt_resist_time == 10);
	assert(s.attack_time == 20);

	for(int t = 2; t <= 20; t++) {
		sim_tick(&s, true, false, false);
		assert(s.damage_events == 1); // cooldown blocks every swing
	}
	sim_tick(&s, true, false, false); // tick 21: cooldown expired
	assert(s.damage_events == 2);
	assert(s.health == 80);
}

// i-frames in isolation: mob always ready to swing (force attack cooldown 0) ->
// re-hit cadence is the 10-tick player invulnerability window.
static void test_iframe_window_isolated(void) {
	struct sim s = {.health = 1600};
	for(int t = 1; t <= 10; t++) {
		sim_tick(&s, true, /*force_atk0*/ true, false);
		assert(s.damage_events == 1); // i-framed after the tick-1 hit
	}
	sim_tick(&s, true, true, false); // tick 11: i-frames just expired
	assert(s.damage_events == 2);
}

// Mob cooldown in isolation: player never i-framed (force hurt_resist 0) ->
// re-hit cadence is the 20-tick swing cooldown. Proves the counters are separate.
static void test_cooldown_isolated(void) {
	struct sim s = {.health = 16000};
	for(int t = 1; t <= 20; t++) {
		sim_tick(&s, true, false, /*force_hrt0*/ true);
		assert(s.damage_events == 1); // cooldown blocks
	}
	sim_tick(&s, true, false, true); // tick 21
	assert(s.damage_events == 2);
}

// Two mobs in melee range on the SAME tick land only ONE hit: the first opens the
// i-frame window, which absorbs the second. (Guards against per-mob i-frame
// decrement / same-tick double damage.)
static void test_two_mobs_same_tick(void) {
	struct sim s = {.health = 160};
	// per-tick i-frame decrement (once)
	if(s.hurt_resist_time > 0)
		s.hurt_resist_time--;
	// mob A swings
	if(s.attack_time == 0) {
		sim_hurt(&s);
		s.attack_time = MOB_ATTACK_COOLDOWN_TICKS;
	}
	int atk_b = 0; // a second, independent mob, also off cooldown
	if(atk_b == 0) {
		sim_hurt(&s); // absorbed: A already set hurt_resist_time
		atk_b = MOB_ATTACK_COOLDOWN_TICKS;
	}
	assert(s.damage_events == 1);
	assert(s.health == 120);
}

// A hit that drops health to <= 0 registers death exactly once; a same-tick
// second swing is absorbed by the fresh i-frame window.
static void test_death(void) {
	struct sim s = {.health = ZOMBIE_ATTACK_DAMAGE}; // exactly one hit of headroom
	if(s.attack_time == 0) {
		sim_hurt(&s);
		s.attack_time = MOB_ATTACK_COOLDOWN_TICKS;
	}
	assert(s.died);
	assert(s.damage_events == 1);
	assert(s.health <= 0);
	// second mob, same tick -> absorbed.
	sim_hurt(&s);
	assert(s.damage_events == 1);
}

int main(void) {
	test_constants();
	test_reach_and_overlap();
	test_knockback_dir();
	test_single_hit_then_iframes();
	test_iframe_window_isolated();
	test_cooldown_isolated();
	test_two_mobs_same_tick();
	test_death();
	printf("test_zombie_melee: all Beta melee mechanics pinned (damage, reach, "
		   "overlap, knockback, i-frames, cooldown, death)\n");
	return 0;
}
