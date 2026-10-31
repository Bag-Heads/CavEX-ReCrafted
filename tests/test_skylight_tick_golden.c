/* next-gaps-0924/w2 -- Beta 1.7.3 skylightSubtracted is a per-tick CACHED world field
 * (docs/intent/w2-skylight-tick-cache.md).
 *
 * The spec (decompile 740c583, net/minecraft/src):
 *   Minecraft.runTick:1147/1152   theWorld.updateEntities() runs BEFORE theWorld.tick()
 *   World.tick:1740-1772          updateWeather; sleep check / performSleepSpawning; performSpawning;
 *                                 skylightSubtracted = calculateSkylightSubtracted(1.0F) at the tick-start
 *                                 time T (:1757-1759); time = T + 1; TickUpdates; random block ticks
 *   World.isDaytime:692-694       skylightSubtracted < 4 (read by EntityPlayer.onUpdate:69, sleepInBedAt:547,
 *                                 EntityZombie/EntitySkeleton sun-burn)
 *   World.calculateInitialSkylight:1726-1731, called by every World constructor (:99/:141/:206)
 *   World.getCelestialAngle(1.0F) -> WorldProvider.calculateCelestialAngle(time, 1.0F) (WorldProvider.java:44-56):
 *                                 the +1.0F partial means the value computed at tick-start T is the angle
 *                                 of T+1. Call g(c) the value computed in the tick that advanced the clock to c.
 *   MathHelper.cos                SIN_TABLE[(int)(x * 10430.378F + 16384.0F) & 65535] (the final cosine)
 * So, in the tick that starts at T (CaveWii clock T+1 after its world_time++): entity updates, the sleep
 * check and natural spawning read g(T) (the previous tick's value); random block ticks read g(T+1).
 *
 * j_sub() below is an independent transcription of that Java (its own SIN_TABLE). Part (0) sweeps every
 * tick of the day through every reader (client lightmap, server, isDaytime) against it; parts (1)-(2) drive
 * the REAL server update (server_local_test_update_once) with the weather strengths at 0.
 */
#include "support/split_residency_fixture.h"
#include "entity/combat.h"
#include "game/difficulty.h"
#include "item/items.h"
#include "platform/audio.h"
#include <math.h>
#include <stdio.h>
#include "daytime.h"
#include "game/game_state.h"

void server_local_test_set_difficulty(int d);

static int failures = 0;
static volatile int draining = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}
static void check(bool ok, const char* what) {
	if(!ok) {
		failures++;
		fprintf(stderr, "RED %s\n", what);
	}
}

#include <time.h>

/* ---- independent Beta transcription (decompile 740c583) ------------------------------------------- */
static float j_sin_table[65536];
static void j_init(void) {
	for(int i = 0; i < 65536; i++) // MathHelper.<clinit>
		j_sin_table[i] = (float)sin((double)i * 3.141592653589793 * 2.0 / 65536.0);
}
static int j_f2i(float f) { // Java (int) of a float: truncation, NaN 0, saturating
	if(f != f) return 0;
	if(f >= 2147483647.0f) return 2147483647;
	if(f <= -2147483648.0f) return (int)-2147483647 - 1;
	return (int)f;
}
static float j_cos(float x) { return j_sin_table[j_f2i(x * 10430.378F + 16384.0F) & 65535]; }
static float j_angle(int64_t time, float partial) { // WorldProvider.calculateCelestialAngle:44-56
	int var4 = (int)(time % 24000LL);
	float var5 = ((float)var4 + partial) / 24000.0F - 0.25F;
	if(var5 < 0.0F) ++var5;
	if(var5 > 1.0F) --var5;
	float var6 = var5;
	var5 = 1.0F - (float)((cos((double)var5 * 3.141592653589793) + 1.0) / 2.0);
	var5 = var6 + (var5 - var6) / 3.0F;
	return var5;
}
static int j_calc(int64_t time, float rain, float thunder) { // World.calculateSkylightSubtracted(1.0F)
	float var2 = j_angle(time, 1.0F);
	float var3 = 1.0F - (j_cos(var2 * 3.1415927F * 2.0F) * 2.0F + 0.5F);
	if(var3 < 0.0F) var3 = 0.0F;
	if(var3 > 1.0F) var3 = 1.0F;
	var3 = 1.0F - var3;
	var3 = (float)((double)var3 * (1.0 - (double)(rain * 5.0F) / 16.0));
	var3 = (float)((double)var3 * (1.0 - (double)(thunder * rain * 5.0F) / 16.0));
	var3 = 1.0F - var3;
	return (int)(var3 * 11.0F);
}
// g(c): the value Beta holds while its clock reads c (computed by the tick that started at c-1).
static int j_sub(uint64_t clock, float rain, float thunder) { return j_calc((int64_t)clock - 1, rain, thunder); }

// (0) Every tick of the day, every reader, clear + weather points.
static void case_curve_all_readers(void) {
	const float rains[] = {0.0F, 0.5F, 1.0F};
	const float thunders[] = {0.0F, 1.0F};
	gstate.world.dimension = WORLD_DIM_OVERWORLD;
	int bad_server = 0, bad_client = 0, bad_daytime = 0, n = 0;
	for(uint64_t c = 0; c < 24000; c++)
		for(size_t r = 0; r < 3; r++)
			for(size_t h = 0; h < 2; h++) {
				int want = j_sub(c, rains[r], thunders[h]);
				n++;
				int server = daytime_skylight_subtracted_at(c, rains[r], thunders[h]);
				float frac = (float)(c % 24000) / (float)24000;
				int client = daytime_skylight_subtracted(daytime_light_sun(frac), rains[r], thunders[h]);
				bool day = daytime_is_daytime_ticks(c, rains[r], thunders[h]);
				if(server != want) { if(bad_server < 4) fprintf(stderr, "  server c=%llu r=%g t=%g: %d want %d\n",
					(unsigned long long)c, rains[r], thunders[h], server, want); bad_server++; }
				if(client != want) { if(bad_client < 4) fprintf(stderr, "  client c=%llu r=%g t=%g: %d want %d\n",
					(unsigned long long)c, rains[r], thunders[h], client, want); bad_client++; }
				if(day != (want < 4)) bad_daytime++;
			}
	int clear_bad = 0;
	for(uint64_t c = 0; c < 24000; c++)
		if(server_local_sky_subtract(c) != j_sub(c, 0.0F, 0.0F)) clear_bad++;
	printf("24000 ticks x 6 weather points (%d reads each): server %d, client %d, isDaytime %d, "
		"server_local_sky_subtract %d mismatches vs the Java\n", n, bad_server, bad_client, bad_daytime, clear_bad);
	check(bad_server == 0 && bad_client == 0 && bad_daytime == 0 && clear_bad == 0,
		"client lightmap == server == isDaytime == Beta calculateSkylightSubtracted on every tick");
}

/* (W2b) A freshly LOADED world, before its first tick. Every World constructor calls
 * calculateInitialSkylight (World.java:99/:141/:206 -> :1726-1731) = calculateSkylightSubtracted(1.0F) at the
 * SAVED worldTime C, i.e. angle(C, 1.0F) = j_calc(C) = g(C+1) -- the value a tick starting at C would compute.
 * Driven through the real server_local_load_world_impl (the ordinary load; portal travel reuses its _ex). */
static struct server_local s_load;
static void load_at(string_t lvl, int64_t clock, enum world_dim dim) {
	struct level_archive la;
	assert(level_archive_create(&la, lvl));
	assert(level_archive_write(&la, LEVEL_TIME, &clock));
	assert(level_archive_write_player(&la, (vec3) {8.5F, 4.0F + PLAYER_EYE_HEIGHT, 8.5F}, (vec2) {0}, NULL, dim));
	assert(level_archive_save(&la));
	level_archive_destroy(&la);
	assert(server_local_load_world_impl(&s_load, lvl));
}
static void case_initial_skylight_on_load(void) {
	char dir[128];
	test_fixture_dir("test_skylight_initial", dir, sizeof(dir));
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0777);
#endif
	server_local_test_create_without_thread(&s_load, false, false, false, false, false, true /*superflat*/, false);
	string_t lvl;
	string_init_set_str(lvl, dir);
	{
		struct level_archive la;
		assert(level_archive_create_new(&la, lvl, 4242, (vec3) {8.5F, 4.0F + PLAYER_EYE_HEIGHT, 8.5F}));
		level_archive_destroy(&la);
	}
	// Both clear-weather day/night boundaries (C where g(C) != g(C+1)), plus an interior noon/midnight control.
	uint64_t dawn = 22000, dusk = 12000;
	while(j_sub(dawn + 1, 0.0F, 0.0F) >= 4) dawn++; // load at `dawn`: Beta initial day, g(dawn) night
	while(j_sub(dusk + 1, 0.0F, 0.0F) < 4) dusk++;  // load at `dusk`: Beta initial night, g(dusk) day
	const uint64_t clocks[] = {dawn, dusk, 6000, 18000, 24000 * 3 + dawn};
	int mism = 0;
	for(size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		load_at(lvl, (int64_t)clocks[i], WORLD_DIM_OVERWORLD);
		int want = j_calc((int64_t)clocks[i], 0.0F, 0.0F); // calculateInitialSkylight at the saved time
		int got = server_local_skylight_subtracted(&s_load);
		bool day = server_local_is_daytime(&s_load);
		printf("load at clock %llu: skylightSubtracted %d isDaytime %d (Beta initial %d/%d, per-tick g(C) %d)\n",
			(unsigned long long)clocks[i], got, day, want, want < 4, j_sub(clocks[i], 0.0F, 0.0F));
		if(got != want || day != (want < 4)) mism++;
		server_local_unload_world_impl(&s_load, false);
	}
	check(mism == 0, "a loaded world holds calculateInitialSkylight = angle(saved time, 1.0F) before its first tick");

	load_at(lvl, (int64_t)dawn, WORLD_DIM_NETHER);
	printf("nether load at clock %llu: dimension %d skylightSubtracted %d isDaytime %d\n", (unsigned long long)dawn,
		(int)s_load.worlds[0].dimension, server_local_skylight_subtracted(&s_load), server_local_is_daytime(&s_load));
	check(s_load.worlds[0].dimension == WORLD_DIM_NETHER && server_local_skylight_subtracted(&s_load) == 11
			&& !server_local_is_daytime(&s_load),
		"a loaded Nether holds 11, never daytime (WorldProviderHell.calculateCelestialAngle 0.5F)");
	server_local_unload_world_impl(&s_load, false);
	string_clear(lvl);
	test_fixture_dir_cleanup(dir);
}

static void tick(struct server_local* s) {
	clock_t c0 = clock();
	server_local_test_update_once(s);
	clin_drain_discard();
	double ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
	if(ms > 200.0)
		fprintf(stderr, "  slow tick at clock %llu: %.0f ms\n", (unsigned long long)s->world_time, ms);
}

int main(void) {
	assert(clin_init());
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	j_init();
	case_curve_all_readers();
	case_initial_skylight_on_load();
	struct server_local* s = split_fixture();
	s->rand_src = (struct random_gen) {.seed = 24680};
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	s->far_gen_budget_ms = INT_MAX;
	s->far_view_distance = 0; /* no far ring: this golden only needs the clock */
	s->player.active_inventory = &s->player.inventory;
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL); // no mob can wake the sleeper
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	for(unsigned load = 0; load < 64; load++)
		tick(s);
	s->input_ack_pending = 0;

	// First clock value whose Beta value g(c) is daytime (isDaytime: < 4).
	uint64_t d0 = 22000;
	while(j_sub(d0, 0.0F, 0.0F) >= 4) d0++;
	printf("Beta clear curve: g(%llu)=%d, g(%llu)=%d (first daytime clock %llu)\n", (unsigned long long)(d0 - 1),
		j_sub(d0 - 1, 0.0F, 0.0F), (unsigned long long)d0, j_sub(d0, 0.0F, 0.0F), (unsigned long long)d0);

	// (1) Between ticks (after the tick that started at T, clock T+1) the world holds g(T+1).
	s->world_time = d0 - 3;
	tick(s); // settle one tick at the new clock
	int mism = 0;
	for(int i = 0; i < 4; i++) {
		uint64_t t_start = s->world_time;
		tick(s);
		int got = server_local_skylight_subtracted(s);
		if(got != j_sub(t_start + 1, 0.0F, 0.0F)) {
			fprintf(stderr, "  after the tick starting at T=%llu (clock %llu): read %d, Beta g(T+1)=%d\n",
				(unsigned long long)t_start, (unsigned long long)s->world_time, got, j_sub(t_start + 1, 0.0F, 0.0F));
			mism++;
		}
	}
	check(mism == 0, "between ticks the world holds g(T+1) (World.tick:1757, partial 1.0F)");

	// (2) isDaytime for the sleeper: EntityPlayer.onUpdate runs in updateEntities BEFORE World.tick, so the
	// sleeper of the tick starting at T sees the previous tick's value g(T); the daylight wake happens in
	// the tick starting at T = d0 (a live read of the clock T+1 would wake one tick early, at d0 - 1).
	const int bx = 0, by = 4, bz = 0;
	(void)server_world_set_block(&s->worlds[0], bx, by, bz - 1, (struct block_data) {.type = BLOCK_BED, .metadata = 0});
	(void)server_world_set_block(&s->worlds[0], bx, by, bz, (struct block_data) {.type = BLOCK_BED, .metadata = 8});
	s->world_time = d0 - 12;
	tick(s);
	server_local_begin_sleep(s, bx, by, bz);
	clin_drain_discard();
	uint64_t woke_at = 0;
	for(int i = 0; i < 40 && s->player.sleeping; i++) {
		uint64_t t_start = s->world_time;
		tick(s);
		if(!s->player.sleeping)
			woke_at = t_start;
	}
	printf("daylight wake in the tick starting at T=%llu (Beta: d0 = %llu)\n", (unsigned long long)woke_at,
		(unsigned long long)d0);
	check(woke_at == d0, "the sleeper is woken by daylight in the tick starting at d0 (reads g(T))");

	draining = 0;
	thread_join(&drain);
	split_destroy(s);
	if(failures) {
		fprintf(stderr, "test_skylight_tick_golden: %d FAILED\n", failures);
		return 1;
	}
	printf("test_skylight_tick_golden: all PASS\n");
	return 0;
}
