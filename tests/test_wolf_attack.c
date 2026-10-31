/* Goldens: [740c583: EntityWolf.attackEntity]. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "entity/combat.h"
#include "wolf_test_common.h"

static int feq(float a, float b) { return fabsf(a - b) < 1.0e-6F; }

int main(void) {
	assert(!wolf_leap_in_band(4.0F));
	assert(wolf_leap_in_band(25.0F));
	assert(!wolf_leap_in_band(36.0F));
	/* The wolf shares EntitySpider's leap arithmetic (combat.h
	 * mob_leap_impulse_beta, the helper entity_monster.c runs). */
	double m[3] = {1.0, 0.0, -1.0};
	mob_leap_impulse_beta(3.0, 4.0, m);
	assert(feq((float)m[0], 0.44F));
	assert(feq((float)m[2], 0.12F));
	assert(m[1] == (double)WOLF_LEAP_VEL_Y);
	assert(feq(WOLF_LEAP_VEL_Y, 0.4F));
	assert(WOLF_LEAP_CHANCE_DEN == 10);

	/* [740c583: EntityWolf.attackEntity] target_id==0 is ALSO the benign
	 * owner-follow state -- only angry+wild may read it as "attack the
	 * player". The leap gate in entity_monster.c used to skip this check
	 * (melee already had it), so a calm tamed wolf mid-follow could pounce
	 * on its own owner; both gates now share this one predicate. */
	assert(!wolf_target_is_hostile_player(0, 0)); /* wild, not angry: no target */
	assert(!wolf_target_is_hostile_player(0,
		WOLF_FLAG_TAMED)); /* tamed + calm follow: NOT hostile */
	assert(!wolf_target_is_hostile_player(0,
		(uint8_t)(WOLF_FLAG_TAMED | WOLF_FLAG_ANGRY))); /* tamed+angry: retaliation still routes via a real id, not this */
	assert(wolf_target_is_hostile_player(0,
		WOLF_FLAG_ANGRY)); /* wild + angry: the one true hostile-player case */
	assert(!wolf_target_is_hostile_player(5, WOLF_FLAG_ANGRY)); /* real id: never this path */

	struct server_local s = {0};
	dict_entity_init(s.entities);
	struct entity* wolf = wolf_test_make(&s, 1, MONSTER_WOLF);
	int damage = 0;
	assert(entity_wolf_melee_in_range(wolf, 1.499F, 0.0F, 0.8F));
	assert(entity_wolf_begin_melee(wolf, 1.499F, 0.0F, 0.8F, &damage));
	assert(wolf->data.monster.attack_time == 20 && damage == 2);
	wolf->data.monster.attack_time = 0;
	assert(!entity_wolf_begin_melee(wolf, 1.5F, 0.0F, 0.8F, &damage));
	assert(!entity_wolf_begin_melee(wolf, 1.0F,
		wolf->pos[1] + 0.8F, wolf->pos[1] + 1.8F, &damage));
	wolf_flag_set(&wolf->data.monster.wolf_flags, WOLF_FLAG_TAMED, true);
	assert(entity_wolf_begin_melee(wolf, 1.0F, 0.1F, 0.7F, &damage));
	assert(damage == 4 && wolf->data.monster.attack_time == 20);

	wolf_test_clear(&s);
	puts("test_wolf_attack: PASS");
	return 0;
}
