/* Goldens: [740c583: EntityWolf.interact]. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wolf_test_common.h"

static uint32_t seed_for_roll(int wanted_zero) {
	for(uint32_t seed = 1; seed < 100000; seed++) {
		struct java_random r;
		jrand_set_seed(&r, (int64_t)(uint64_t)seed);
		if((jrand_next_int_bound(&r, 3) == 0) == (wanted_zero != 0))
			return seed;
	}
	assert(!"reachable tame seed");
	return 0;
}

/* Algorithm witness at a declared seed, separately from factory seed policy.
 * The actual Oracle JRE8 nextInt(3) vector must be retained before execution.
 * Legacy RED assigns its current raw state; Java GREEN uses setSeed(42). */
static void test_declared_seed_tame(void) {
	struct server_local s = {0};
	dict_entity_init(s.entities);
	struct entity* wolf = wolf_test_make(&s, 42, MONSTER_WOLF);
	jrand_set_seed(&wolf->gameplay_rng, (int64_t)(uint64_t)42);
	struct item_data bones = {.id = ITEM_BONE, .count = 2};
	enum wolf_interact_result result = entity_wolf_interact(wolf, &s, &bones);
	fprintf(stderr, "declared-seed 42 wolf tame: result=%d expected=%d bones=%d\n",
		result, WOLF_INTERACT_SMOKE, bones.count);
	assert(result == WOLF_INTERACT_SMOKE);
	assert(wolf->gameplay_rng.seed == UINT64_C(204790973191750));
	assert(bones.count == 1);
	assert(!wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_TAMED));
	wolf_test_clear(&s);
}

int main(void) {
	test_declared_seed_tame();
	struct server_local s = {0};
	dict_entity_init(s.entities);
	struct entity* wolf = wolf_test_make(&s, 1, MONSTER_WOLF);
	struct item_data bones = {.id = ITEM_BONE, .count = 2};

	uint32_t fail_seed = seed_for_roll(0);
	struct java_random fail_expected;
	jrand_set_seed(&fail_expected, (int64_t)(uint64_t)fail_seed);
	assert(jrand_next_int_bound(&fail_expected, 3) != 0);
	jrand_set_seed(&wolf->gameplay_rng, (int64_t)(uint64_t)fail_seed);
	assert(entity_wolf_interact(wolf, &s, &bones) == WOLF_INTERACT_SMOKE);
	assert(bones.count == 1 && wolf->gameplay_rng.seed == fail_expected.seed);
	assert(!wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_TAMED));

	uint32_t success_seed = seed_for_roll(1);
	struct java_random success_expected;
	jrand_set_seed(&success_expected, (int64_t)(uint64_t)success_seed);
	assert(jrand_next_int_bound(&success_expected, 3) == 0);
	jrand_set_seed(&wolf->gameplay_rng, (int64_t)(uint64_t)success_seed);
	assert(entity_wolf_interact(wolf, &s, &bones) == WOLF_INTERACT_HEARTS);
	assert(bones.count == 0 && wolf->gameplay_rng.seed == success_expected.seed);
	assert(wolf->health == 20 && wolf->data.monster.wolf_watched_health == 20);
	assert(wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_TAMED));
	assert(wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING));
	assert(strcmp(wolf->data.monster.wolf_owner, WOLF_LOCAL_PLAYER_NAME) == 0);

	struct entity* angry = wolf_test_make(&s, 2, MONSTER_WOLF);
	wolf_flag_set(&angry->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
	struct item_data refused = {.id = ITEM_BONE, .count = 1};
	uint64_t before = angry->gameplay_rng.seed;
	assert(entity_wolf_interact(angry, &s, &refused) == WOLF_INTERACT_NONE);
	assert(refused.count == 1 && angry->gameplay_rng.seed == before);

	wolf_test_clear(&s);
	puts("test_wolf_tame: PASS");
	return 0;
}
