/* Beta server World.func_28096_a:1210-1213: aux SFX 1004 BEFORE remove.
 * Client RenderGlobal:1379-1380: random.fizz 0.5 at fire cell center,
 * 2.6 + (first nextFloat - second nextFloat)*0.8 (client world RNG). */
#include "support/sound_action_env.h"

static void fire_capture(const struct client_rpc* call, void* user) {
	sound_capture(call, user);
	if(call->type == CRPC_SOUND)
		assert(sound_block(8, gy + 1, 8).type == BLOCK_FIRE);
}

static void punch(int x) {
	struct server_rpc r = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.x = x, .y = gy, .z = 8,
			.side = SIDE_TOP, .finished = false}};
	server_local_test_process_rpc(&s, &r);
}

int main(void) {
	sound_env_init("test_fire_punch_fizz");
	sound_set(8, gy, 8, BLOCK_STONE, 0);
	sound_set(8, gy + 1, 8, BLOCK_FIRE, 0);
	clin_test_set_rpc_observer(fire_capture, NULL);
	struct random_gen before = s.rand_src;
	struct random_gen* cosmetic = server_local_sound_rng();
	*cosmetic = (struct random_gen) {.seed = UINT32_C(0x12345678)};
	struct random_gen expected = *cosmetic;
	const float first = rand_gen_flt(&expected);
	const float second = rand_gen_flt(&expected);
	const float pitch = 2.6F + (first - second) * 0.8F;
	punch(8);
	assert(sound_block(8, gy + 1, 8).type == BLOCK_AIR);
	sound_assert(SOUND_RANDOM_FIZZ, 0.5F, pitch, 8, gy + 1, 8);
	assert(last_sound.payload.sound.pitch >= 1.8F);
	assert(last_sound.payload.sound.pitch <= 3.4F);
	assert(memcmp(&before, &s.rand_src, sizeof before) == 0);
	assert(memcmp(&expected, cosmetic, sizeof expected) == 0);
	clin_test_set_rpc_observer(sound_capture, NULL);
	sound_set(10, gy + 1, 8, BLOCK_STONE, 0);
	sound_count = 0;
	punch(10);
	assert(sound_count == 0 && sound_block(10, gy + 1, 8).type == BLOCK_STONE);
	assert(memcmp(&before, &s.rand_src, sizeof before) == 0);
	assert(memcmp(&expected, cosmetic, sizeof expected) == 0);
	sound_env_finish();
	puts("test_fire_punch_fizz: PASS (fizz before removal; exact two cosmetic draws/pitch/position; non-fire silent; simulation RNG unchanged)");
	return 0;
}
