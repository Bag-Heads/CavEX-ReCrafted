/* Goldens: [740c583: EntityWolf.attackEntityFrom]. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "network/client_interface.h"
#include "wolf_test_common.h"

int main(void) {
	assert(clin_init());
	struct server_local s = {0};
	dict_entity_init(s.entities);
	struct entity* hit = wolf_test_make(&s, 1, MONSTER_WOLF);
	struct entity* near = wolf_test_make(&s, 2, MONSTER_WOLF);
	struct entity* far = wolf_test_make(&s, 3, MONSTER_WOLF);
	hit->pos[0] = near->pos[0] = 0.0F;
	near->pos[0] = 16.99F; /* (pos..pos+1).expand: positive edge is +17 */
	far->pos[0] = 17.01F;
	assert(entity_wolf_attack_from(hit, &s, 2, 0, true, false, false, ACTOR_IDENT_HOST)
		== MONSTER_DAMAGE_FRESH);
	assert(hit->health == 6);
	assert(wolf_flag_get(hit->data.monster.wolf_flags, WOLF_FLAG_ANGRY));
	assert(wolf_flag_get(near->data.monster.wolf_flags, WOLF_FLAG_ANGRY));
	assert(hit->data.monster.wolf_target_id == 0);
	assert(near->data.monster.wolf_target_id == 0);
	assert(far->data.monster.wolf_target_id == UINT32_MAX);

	/* Arrow retaliation is redirected to the shooting owner and does not make
	 * a wild pack angry unless that owner is the player. */
	struct entity* arrowed = wolf_test_make(&s, 4, MONSTER_WOLF);
	arrowed->pos[0] = 40.0F;
	assert(entity_wolf_attack_from(arrowed, &s, 4, 77, false, true, false, ACTOR_IDENT_HOST)
		== MONSTER_DAMAGE_FRESH);
	assert(arrowed->data.monster.wolf_target_id == 77);
	assert(!wolf_flag_get(arrowed->data.monster.wolf_flags, WOLF_FLAG_ANGRY));

	/* [740c583: Entity.java (cactus/fire/lava) + EntityLiving.java
	 * (fall/drown)] every environmental damage call site passes a NULL
	 * Entity attacker. Beta's halving gate is var1!=null && !player &&
	 * !arrow -- a null attacker fails the first clause, so environmental
	 * damage is NEVER halved (distinct from a real non-player mob hit,
	 * which IS halved -- see real_mob_attacker directly below). */
	struct entity* environmental = wolf_test_make(&s, 5, MONSTER_WOLF);
	assert(entity_wolf_attack_from(environmental, &s, 5, UINT32_MAX,
		false, false, false, ACTOR_IDENT_HOST) == MONSTER_DAMAGE_FRESH);
	assert(environmental->health == 3); /* 8 - 5, full damage: no attacker to halve for */

	/* Companion case: a REAL non-player, non-arrow attacker (a mob, not the
	 * environment) still gets the (damage+1)/2 halving -- confirms the fix
	 * above didn't just remove halving outright. Positioned away from every
	 * other test wolf so it can't be pack-aggro'd by a stale target. */
	struct entity* real_mob_attacker = wolf_test_make(&s, 7, MONSTER_WOLF);
	real_mob_attacker->pos[0] = 100.0F;
	assert(entity_wolf_attack_from(real_mob_attacker, &s, 5, 88,
		false, false, false, ACTOR_IDENT_HOST) == MONSTER_DAMAGE_FRESH);
	assert(real_mob_attacker->health == 5); /* 8 - (5+1)/2 == 8-3 == 5 */

	/* [740c583: EntityWolf.attackEntityFrom] super.attackEntityFrom runs
	 * UNCONDITIONALLY before the owner check -- an owner's hit still deals
	 * full damage (player attacker, so no halving). Owner immunity is
	 * RETALIATION-only: the owner branch just skips playerToAttack, so
	 * wolf_target_id must stay untouched (UINT32_MAX, never set). */
	struct entity* owned = wolf_test_make(&s, 6, MONSTER_WOLF);
	owned->data.monster.wolf_flags = WOLF_FLAG_TAMED | WOLF_FLAG_SITTING;
	strcpy(owned->data.monster.wolf_owner, "local player");
	assert(entity_wolf_attack_from(owned, &s, 7, 0, true, false, false, ACTOR_IDENT_HOST)
		== MONSTER_DAMAGE_FRESH);
	assert(owned->health == 1); /* 8 - 7, full damage: no owner exemption */
	assert(!wolf_flag_get(owned->data.monster.wolf_flags, WOLF_FLAG_SITTING));
	assert(owned->data.monster.wolf_target_id == UINT32_MAX); /* no retaliation */

	clin_drain_discard();
	wolf_test_clear(&s);
	puts("test_wolf_anger_pack: PASS");
	return 0;
}
