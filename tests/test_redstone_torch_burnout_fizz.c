/* Beta server BlockRedstoneTorch.updateTick:122-136: set idle, record/check
 * >=8 toggles in 100 ticks, fizz (two World.rand.nextFloat), five smoke
 * particles (3 x var5.nextDouble each; var5 IS World.rand, World.java:833/
 * 1540); relight is silent. Two-tick edges use the real scheduled-tick queue.
 * Golden: the burnout tick consumes EXACTLY 17 simulation (s->rand_src) draws
 * in Beta order and every other tick consumes zero; the cosmetic sound stream
 * is untouched. The pure layer fires the hook once, on the eighth OFF edge. */
#include "support/sound_action_env.h"
#include "block/redstone_logic.h"
#include "block/redstone_server.h"

#define TX 8
#define TY 101
#define TZ 8
#define TRACE_TICKS 234
static struct server_world pure_world;
static struct redstone_world pure_rs;
static int pure_type, burnouts;
static bool input;
static uint8_t traces[2][TRACE_TICKS][2];

static int pure_get_type(void* ctx, int x, int y, int z) {
	(void)ctx; (void)x; (void)y; (void)z;
	return pure_type;
}
static int pure_get_meta(void* ctx, int x, int y, int z) {
	(void)ctx; (void)x; (void)y; (void)z;
	return 5;
}
static void pure_set(void* ctx, int x, int y, int z, int type) {
	(void)ctx;
	assert(x == TX && y == TY && z == TZ);
	pure_type = type;
}
static bool pure_power(void* ctx, int x, int y, int z, int side) {
	(void)ctx;
	assert(x == TX && y == TY - 1 && z == TZ && side == 0);
	return input;
}
static int64_t pure_time(void* ctx) {
	(void)ctx;
	return pure_world.current_tick;
}
static void pure_burn(void* ctx, int x, int y, int z) {
	(void)ctx;
	assert(x == TX && y == TY && z == TZ);
	assert(pure_type == REDSTONE_TORCH_IDLE_ID);
	assert(pure_world.current_tick == 30); // eighth OFF edge, 2+7*4
	assert(++burnouts == 1);
}
static void pure_fire(struct server_world* w, struct server_local* local,
					  struct scheduled_tick tick) {
	(void)w; (void)local;
	redstone_torch_update(&pure_rs, tick.x, tick.y, tick.z);
}
static void pure_tick(int run, int expected) {
	pure_world.current_tick++;
	server_world_drain_scheduled(&pure_world, NULL, 0, pure_fire, NULL);
	assert(pure_type == expected);
	traces[run][pure_world.current_tick - 1][0] = pure_type;
	traces[run][pure_world.current_tick - 1][1] = pure_get_meta(NULL, TX, TY, TZ);
}
static void pure_schedule(void) {
	assert(server_world_schedule_tick_raw(&pure_world, TX, TY, TZ,
		pure_type, REDSTONE_TORCH_TICK_RATE));
}
static void check_pure(int run) {
	redstone_torch_reset_burnout();
	memset(&pure_world, 0, sizeof pure_world);
	server_world_scheduled_ticks_create(&pure_world,
		SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	pure_rs = (struct redstone_world) {.get_type = pure_get_type,
		.get_meta = pure_get_meta, .set_type = pure_set,
		.is_indirectly_providing_power = pure_power, .get_world_time = pure_time,
		.on_burnout = run ? pure_burn : NULL};
	pure_type = REDSTONE_TORCH_LIT_ID;
	burnouts = 0;
	for(int cycle = 0; cycle < 8; cycle++) {
		input = true;
		pure_schedule();
		pure_tick(run, REDSTONE_TORCH_LIT_ID);
		pure_tick(run, REDSTONE_TORCH_IDLE_ID);
		assert(burnouts == (run && cycle == 7 ? 1 : 0));
		input = false;
		pure_schedule();
		pure_tick(run, REDSTONE_TORCH_IDLE_ID);
		pure_tick(run, cycle < 7 ? REDSTONE_TORCH_LIT_ID : REDSTONE_TORCH_IDLE_ID);
	}
	pure_schedule(); // blocked relight while still inside the burnout window
	while(pure_world.current_tick < 232)
		pure_tick(run, REDSTONE_TORCH_IDLE_ID);
	pure_schedule(); // all records now older than 100 ticks
	pure_tick(run, REDSTONE_TORCH_IDLE_ID);
	pure_tick(run, REDSTONE_TORCH_LIT_ID);
	assert(burnouts == run);
	server_world_scheduled_ticks_destroy(&pure_world);
}

static int smoke_count;
static void burnout_capture(const struct client_rpc* call, void* user) {
	sound_capture(call, user);
	if(call->type == CRPC_SOUND) {
		assert(sound_block(TX, TY, TZ).type == BLOCK_REDSTONE_TORCH);
		assert(sound_block(TX, TY, TZ).metadata == 5);
		assert(s.worlds[0].current_tick == 30);
		assert(smoke_count == 0); // sound before smoke
	} else if(call->type == CRPC_SMOKE_PUFF) {
		assert(sound_count == 1);
		assert(call->payload.smoke_puff.intensity == 1.25F); // ceil(4*1.25)=5
		assert(call->payload.smoke_puff.pos[0] == TX + 0.5F);
		assert(call->payload.smoke_puff.pos[1] == TY + 0.5F);
		assert(call->payload.smoke_puff.pos[2] == TZ + 0.5F);
		assert(call->world_index == s.worlds[0].world_index);
		assert(call->world_generation == s.worlds[0].generation);
		assert(++smoke_count == 1);
	}
}
#define BURNOUT_TICK 30
#define BURNOUT_DRAWS 17 /* 2 nextFloat + 5 x 3 nextDouble */
static struct random_gen burn_ref; // rand_src as it must be after the burnout
static float burn_pitch;
static double burn_smoke[REDSTONE_TORCH_BURNOUT_SMOKE][3];
static int draw_ticks; // ticks on which rand_src moved

/* Struct assignment need not preserve C padding. Compare every RNG member,
 * including the cached float's exact bits even when the cache is inactive. */
static bool rng_equal(const struct random_gen* a, const struct random_gen* b) {
	return a->seed == b->seed && a->has_next_gaussian == b->has_next_gaussian
		&& memcmp(&a->next_gaussian, &b->next_gaussian,
			sizeof a->next_gaussian) == 0;
}

/* Independent Beta-order reference for one burnout from `state`: raw draws
 * mapped exactly as Beta's argument evaluation order consumes them. */
static void burnout_reference(struct random_gen state) {
	struct random_gen g = state;
	const float r1 = rand_gen_flt(&g); // fizz: first nextFloat
	const float r2 = rand_gen_flt(&g); // fizz: second nextFloat
	burn_pitch = 2.6F + (r1 - r2) * 0.8F;
	for(int i = 0; i < REDSTONE_TORCH_BURNOUT_SMOKE; i++) {
		const int base[3] = {TX, TY, TZ};
		for(int a = 0; a < 3; a++) // var8 (x), var10 (y), var12 (z)
			burn_smoke[i][a] = (double)base[a]
				+ (double)rand_gen(&g) / 4294967296.0 * 0.6 + 0.2;
	}
	struct random_gen counted = state;
	for(int i = 0; i < BURNOUT_DRAWS; i++)
		(void)rand_gen(&counted);
	assert(rng_equal(&counted, &g)); // reference used exactly 17
	burn_ref = g;
	// The production sampler, fed the same state, reproduces it bit-for-bit.
	struct random_gen p = state;
	double smoke[REDSTONE_TORCH_BURNOUT_SMOKE][3];
	assert(redstone_torch_burnout_effect_draws(&p, TX, TY, TZ, smoke) == burn_pitch);
	assert(rng_equal(&p, &burn_ref));
	assert(memcmp(smoke, burn_smoke, sizeof smoke) == 0);
	for(int i = 0; i < REDSTONE_TORCH_BURNOUT_SMOKE; i++)
		for(int a = 0; a < 3; a++) {
			const int base = a == 0 ? TX : a == 1 ? TY : TZ;
			assert(smoke[i][a] >= base + 0.2 && smoke[i][a] < base + 0.8);
		}
}
static void env_tick(int expected) {
	const struct random_gen before = s.rand_src;
	const int64_t tick = s.worlds[0].current_tick + 1;
	if(tick == BURNOUT_TICK)
		burnout_reference(before);
	server_world_tick(&s.worlds[0], &s);
	assert(sound_block(TX, TY, TZ).type == expected);
	assert(sound_block(TX, TY, TZ).metadata == 5);
	if(tick == BURNOUT_TICK) {
		assert(rng_equal(&burn_ref, &s.rand_src));
		draw_ticks++;
	} else {
		assert(rng_equal(&before, &s.rand_src));
	}
}
static void env_input(bool on) {
	// A wall lever at +X (meta 1) powers its stone mount, the torch's support.
	// Silent fixture input write, then checked production scheduling at +2.
	assert(server_world_set_block_silent(&s.worlds[0], TX + 1, TY - 1, TZ,
		(struct block_data) {.type = BLOCK_LEVER, .metadata = 1 | (on ? 8 : 0)}));
	assert(server_world_schedule_block_tick(&s.worlds[0], TX, TY, TZ,
		sound_block(TX, TY, TZ).type));
}
int main(void) {
	check_pure(0);
	check_pure(1);
	assert(memcmp(traces[0], traces[1], sizeof traces[0]) == 0);
	sound_env_init("test_redstone_torch_burnout_fizz");
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			if(cx || cz) // sound_env_init already loaded (0,0)
				assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	redstone_torch_reset_burnout();
	sound_set(TX, TY - 1, TZ, BLOCK_STONE, 0);
	sound_set(TX + 1, TY - 1, TZ, BLOCK_LEVER, 1);
	sound_set(TX, TY, TZ, BLOCK_REDSTONE_TORCH_LIT, 5);
	clin_test_set_rpc_observer(burnout_capture, NULL);
	s.rand_src = (struct random_gen) {.seed = UINT32_C(0x9E3779B9)};
	struct random_gen* cosmetic = server_local_sound_rng();
	*cosmetic = (struct random_gen) {.seed = UINT32_C(0x12345678)};
	const struct random_gen cosmetic_before = *cosmetic;
	for(int cycle = 0; cycle < 8; cycle++) {
		env_input(true);
		env_tick(BLOCK_REDSTONE_TORCH_LIT);
		env_tick(BLOCK_REDSTONE_TORCH);
		assert(sound_count == (cycle == 7 ? 1 : 0));
		env_input(false);
		env_tick(BLOCK_REDSTONE_TORCH);
		env_tick(cycle < 7 ? BLOCK_REDSTONE_TORCH_LIT : BLOCK_REDSTONE_TORCH);
	}
	sound_assert(SOUND_RANDOM_FIZZ, 0.5F, burn_pitch, TX, TY, TZ);
	assert(burn_pitch >= 1.8F && burn_pitch <= 3.4F && smoke_count == 1);
	assert(draw_ticks == 1);
	// Silent blocked retry, then silent relight after entries age out:
	// zero simulation draws on every one of these ticks (env_tick checks).
	assert(server_world_schedule_block_tick(&s.worlds[0], TX, TY, TZ, BLOCK_REDSTONE_TORCH));
	while(s.worlds[0].current_tick < 232)
		env_tick(BLOCK_REDSTONE_TORCH);
	assert(server_world_schedule_block_tick(&s.worlds[0], TX, TY, TZ, BLOCK_REDSTONE_TORCH));
	env_tick(BLOCK_REDSTONE_TORCH);
	env_tick(BLOCK_REDSTONE_TORCH_LIT);
	assert(sound_count == 1 && smoke_count == 1 && draw_ticks == 1);
	assert(rng_equal(&burn_ref, &s.rand_src));
	// The fizz no longer touches the cosmetic sound stream.
	assert(rng_equal(&cosmetic_before, cosmetic));
	sound_env_finish();
	puts("test_redstone_torch_burnout_fizz: PASS (burnout tick 30 consumes exactly 17 rand_src draws in Beta order: pitch 2.6+(f1-f2)*0.8 then 5x(x,y,z) smoke at cell+d*0.6+0.2; all 233 other ticks consume 0; cosmetic stream untouched; relight silent; one world-gated smoke RPC)");
	return 0;
}
