/*
	Golden test: Beta 1.7.3 World.updateWeather (weather state machine) and the
	WorldInfo NBT round-trip [740c583]. Hand-computed expectations, independent
	of the production implementation's own units where a scale crosses a
	domain boundary (none here -- weather timers/strengths are dimensionless
	Beta-native values, no x8 health-scale conversion applies to this test).

	Decompile citation (World.updateWeather, verbatim structure):
		if (!hasNoSky) {
			if (field_27168_F > 0) --field_27168_F;
			thunderTime <= 0 ? refill(thundering ? [3600,15600) : [12000,180000))
			                 : (--thunderTime; ==0 -> thundering = !thundering)
			rainTime    <= 0 ? refill(raining    ? [12000,24000) : [12000,180000))
			                 : (--rainTime;    ==0 -> raining = !raining)
			prevRainingStrength = rainingStrength; rainingStrength += raining?+.01:-.01; clamp[0,1]
			prevThunderingStrength = thunderingStrength; += thundering?+.01:-.01; clamp[0,1]
		}
	World.wakeUpAllPlayers -> stopPrecipitation(): raining=false, rainTime=0,
	thundering=false, thunderTime=0 (strengths untouched, ramp down naturally).
	WorldInfo NBT tags (updateTagCompound): "raining" (byte), "rainTime" (int),
	"thundering" (byte), "thunderTime" (int); strengths are NOT persisted.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "world.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_weather_timers_save_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void reset_bare(void) {
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	s.world.dimension = WORLD_DIM_OVERWORLD;
	s.rand_src.seed = 0x51DEB001u; /* xorshift needs nonzero; reproducible */
}

/* --- (1) thunderTime / rainTime countdown + refill ranges, verbatim order -- */

static void test_timer_countdown_and_flip(void) {
	reset_bare();
	s.weather.thunder_time = 5;
	s.weather.rain_time = 3;
	s.weather.thundering = false;
	s.weather.raining = false;

	/* Countdown ticks: decrements by exactly 1/tick while > 0, no flip yet. */
	server_weather_tick(&s);
	assert(s.weather.thunder_time == 4 && s.weather.rain_time == 2);
	assert(!s.weather.thundering && !s.weather.raining);

	for(int i = 0; i < 3; i++)
		server_weather_tick(&s);
	/* thunder_time: 4,3,2,1 after 3 more ticks (started at 4). rain_time hit 0
	 * on the 2nd of these ticks (2,1,0->flip), then REFILLS on the 3rd (since
	 * a refill only happens when the timer is READ as <=0, one tick after the
	 * flip -- Beta's exact two-tick shape: the tick that decrements-to-zero
	 * flips; the FOLLOWING tick's <=0 branch refills). */
	assert(s.weather.thunder_time == 1);
	assert(s.weather.raining && "rainTime hit 0 by decrement -> flipped raining true");
	assert(s.weather.rain_time > 0 && "the tick immediately after the flip refills");
	/* raining is now true, so the refill just observed must be the RAINING
	 * range [12000, 24000) -- rand.nextInt(12000) + 12000. */
	assert(s.weather.rain_time >= 12000 && s.weather.rain_time < 24000);

	/* One more tick: thunder_time 1->0, flips thundering true; no refill yet
	 * (refill happens the tick AFTER, matching the two-tick shape above). */
	int32_t rain_time_before = s.weather.rain_time;
	server_weather_tick(&s);
	assert(s.weather.thunder_time == 0);
	assert(s.weather.thundering && "thunderTime hit 0 by decrement -> flipped thundering true");
	assert(s.weather.rain_time == rain_time_before - 1);

	/* Next tick: thunder_time reads <=0 (it's exactly 0) -> refills using the
	 * NOW-thundering=true range [3600, 15600). */
	server_weather_tick(&s);
	assert(s.weather.thunder_time >= 3600 && s.weather.thunder_time < 15600);

	puts("  timer countdown/flip/refill (verbatim two-tick shape) OK");
}

static void test_refill_ranges_not_thundering_not_raining(void) {
	/* Both timers start already <=0 and both flags false: the VERY FIRST tick
	 * refills using the "not thundering"/"not raining" ranges directly (no
	 * flip needed -- flags were already at their refill-branch value). */
	for(int trial = 0; trial < 20; trial++) {
		reset_bare();
		s.rand_src.seed = 0x1000u + (uint32_t)trial * 7919u;
		s.weather.thunder_time = 0;
		s.weather.rain_time = 0;
		s.weather.thundering = false;
		s.weather.raining = false;
		server_weather_tick(&s);
		assert(s.weather.thunder_time >= 12000 && s.weather.thunder_time < 180000);
		assert(s.weather.rain_time >= 12000 && s.weather.rain_time < 180000);
		/* A fresh refill with the flag already false does not itself set the
		 * flag true -- only a decrement-to-zero flips. */
		assert(!s.weather.thundering && !s.weather.raining);
	}
	puts("  refill ranges (not thundering/not raining: [12000,180000)) OK");
}

/* --- (2) strength lerp +-0.01/tick, clamped [0,1] -------------------------- */

static void test_strength_lerp_and_clamp(void) {
	reset_bare();
	s.weather.thunder_time = 1000000; /* never expires during this test */
	s.weather.rain_time = 1000000;
	s.weather.raining = true;
	s.weather.thundering = true;
	s.weather.rain_strength = 0.0f;
	s.weather.thunder_strength = 0.0f;

	for(int i = 1; i <= 5; i++) {
		float prev_rain = s.weather.rain_strength;
		float prev_thunder = s.weather.thunder_strength;
		server_weather_tick(&s);
		assert(s.weather.prev_rain_strength == prev_rain);
		assert(s.weather.prev_thunder_strength == prev_thunder);
		float expect = (float)i * 0.01f;
		assert(fabsf(s.weather.rain_strength - expect) < 1e-6f);
		assert(fabsf(s.weather.thunder_strength - expect) < 1e-6f);
	}

	/* Clamp at 1.0: run past the point strength would exceed 1 without it. */
	for(int i = 0; i < 200; i++)
		server_weather_tick(&s);
	assert(s.weather.rain_strength == 1.0f);
	assert(s.weather.thunder_strength == 1.0f);

	/* Now ramp down and clamp at 0.0. */
	s.weather.raining = false;
	s.weather.thundering = false;
	for(int i = 0; i < 200; i++)
		server_weather_tick(&s);
	assert(s.weather.rain_strength == 0.0f);
	assert(s.weather.thunder_strength == 0.0f);

	puts("  strength lerp +-0.01/tick, clamp [0,1] OK");
}

/* --- (2b) F4 (adversarial review): the double-promotion shape is NOT the
 * same as a plain float accumulation -- New-Hire measured a systematic 1-ULP
 * divergence from tick 5 onward against a naive `+= 0.01f`. A 1e-6 tolerance
 * (as used above) cannot discriminate a single-ULP difference, so this test
 * asserts BIT-EXACT equality (raw float compare, no epsilon) against an
 * INDEPENDENTLY hand-modeled double-promotion reference sequence computed
 * right here, not by calling the production code and comparing it to
 * itself. [740c583: World.updateWeather -- `this.rainingStrength =
 * (float)((double)this.rainingStrength + 0.01D);`] ------------------------- */
static void test_strength_double_promotion_bit_exact(void) {
	reset_bare();
	s.weather.thunder_time = 1000000;
	s.weather.rain_time = 1000000;
	s.weather.raining = true;
	s.weather.thundering = true;
	s.weather.rain_strength = 0.0f;
	s.weather.thunder_strength = 0.0f;

	/* The independent reference: the SAME (float)((double)x + 0.01) shape,
	 * typed out here rather than reusing any production helper. */
	float ref_rain = 0.0f, ref_thunder = 0.0f;

	/* Also prove the DIVERGENCE is real: a naive plain-float accumulator,
	 * run in parallel, must NOT stay bit-identical to the double-promoted
	 * reference for the full 100 ticks (else this test would be unable to
	 * tell the two shapes apart, and a regression back to `+= 0.01f` would
	 * silently pass). */
	float naive_rain = 0.0f;
	bool naive_diverged = false;

	for(int i = 0; i < 100; i++) {
		server_weather_tick(&s);

		ref_rain = (float)((double)ref_rain + 0.01);
		ref_thunder = (float)((double)ref_thunder + 0.01);
		naive_rain = naive_rain + 0.01f;

		if(s.weather.rain_strength >= 1.0f || s.weather.thunder_strength >= 1.0f)
			break; /* clamp reached -- the pre-clamp bit-exact race is over */

		if(memcmp(&s.weather.rain_strength, &ref_rain, sizeof(float)) != 0) {
			fprintf(stderr,
					"tick %d: rain_strength bit mismatch: got %.10f (0x%08x), "
					"expected (double-promoted) %.10f (0x%08x)\n",
					i, (double)s.weather.rain_strength,
					*(uint32_t*)&s.weather.rain_strength, (double)ref_rain,
					*(uint32_t*)&ref_rain);
			assert(0 && "rain_strength must be BIT-EXACT to the double-"
						"promoted reference -- a plain float += drifts by "
						"1 ULP");
		}
		if(memcmp(&s.weather.thunder_strength, &ref_thunder, sizeof(float))
		   != 0) {
			assert(0 && "thunder_strength must be BIT-EXACT to the double-"
						"promoted reference");
		}

		if(!naive_diverged
		   && memcmp(&s.weather.rain_strength, &naive_rain, sizeof(float))
				  != 0)
			naive_diverged = true;
	}

	assert(naive_diverged
		   && "sanity: the naive plain-float accumulator must ACTUALLY "
			  "diverge from the double-promoted production value at some "
			  "point in the first 100 ticks -- otherwise this test cannot "
			  "discriminate the two shapes and a regression would pass "
			  "silently");

	puts("  F4: strength lerp is BIT-EXACT to Beta's (float)((double)x+0.01) "
		 "double-promotion shape for 100 ticks (and provably NOT equal to a "
		 "naive plain-float accumulator) OK");
}

/* --- (3) isRaining / getIsThundering thresholds ---------------------------- */

static void test_raining_thundering_thresholds(void) {
	reset_bare();
	/* isRaining(): strength > 0.2 STRICTLY. */
	s.weather.rain_strength = 0.20f;
	assert(!server_weather_is_raining(&s));
	s.weather.rain_strength = 0.21f;
	assert(server_weather_is_raining(&s));

	/* getIsThundering(): thunder_strength * rain_strength > 0.9. Rain fully
	 * ramped (1.0) but thunder only 0.9 -> product 0.9, NOT thundering
	 * (strict >). */
	s.weather.rain_strength = 1.0f;
	s.weather.thunder_strength = 0.9f;
	assert(!server_weather_is_thundering(&s));
	s.weather.thunder_strength = 0.95f;
	assert(server_weather_is_thundering(&s));
	/* Thunder maxed but rain only half-ramped: product 0.5, well under 0.9 --
	 * thunder is WEIGHTED by rain strength, not independent. */
	s.weather.thunder_strength = 1.0f;
	s.weather.rain_strength = 0.5f;
	assert(!server_weather_is_thundering(&s));

	puts("  isRaining >0.2 / getIsThundering (thunder*rain)>0.9 OK");
}

/* --- (3b) F3 (adversarial review): pos_lcg/soundCounter reseeded at every --
 * world create/load, from s->rand_src, matching Beta's World constructors
 * (`field_9437_g = new Random().nextInt(); soundCounter =
 * this.rand.nextInt(12000);` -- all three ctor overloads, identical).
 * Neither was previously seeded on load (both stayed at their zero-init
 * default forever), unlike Beta which reseeds field_9437_g/soundCounter on
 * EVERY construction. Two different rand seeds must produce different
 * first-strike-position state (pos_lcg) -- proof the seeding actually reads
 * from a live, seed-dependent stream rather than leaving the old fixed
 * default in place. --------------------------------------------------------- */
static void test_seed_produces_different_lcg(void) {
	reset_bare();
	s.rand_src.seed = 0x1111u;
	server_weather_seed(&s);
	uint32_t lcg_a = s.weather.pos_lcg;
	int32_t sc_a = s.weather.sound_counter;
	assert(sc_a >= 0 && sc_a < 12000
		   && "soundCounter = rand.nextInt(12000) must land in [0,12000)");

	reset_bare();
	s.rand_src.seed = 0x2222u;
	server_weather_seed(&s);
	uint32_t lcg_b = s.weather.pos_lcg;

	assert(lcg_a != lcg_b
		   && "two different rand seeds at world load must produce different "
			  "pos_lcg (field_9437_g) -- proves it is reseeded on load, not "
			  "left at a fixed/zero default (the pre-fix bug)");
	assert(lcg_a != 0 && lcg_b != 0
		   && "sanity: neither seed coincidentally reproduced the old "
			  "zero-init default");

	puts("  F3: world load reseeds pos_lcg/soundCounter from s->rand_src -- "
		 "two different seeds -> different first-strike-position state OK");
}

/* --- (4) hasNoSky (Nether) fully inert -------------------------------------- */

static void test_nether_fully_inert(void) {
	reset_bare();
	s.world.dimension = WORLD_DIM_NETHER;
	s.weather.thunder_time = 5;
	s.weather.rain_time = 5;
	s.weather.raining = true;
	s.weather.thundering = true;
	s.weather.rain_strength = 0.42f;
	s.weather.thunder_strength = 0.37f;
	s.weather.flash_ticks = 2;

	struct { int32_t tt, rt; bool r, th; float rs, ts; int32_t ft; } before = {
		s.weather.thunder_time, s.weather.rain_time, s.weather.raining,
		s.weather.thundering,   s.weather.rain_strength,
		s.weather.thunder_strength, s.weather.flash_ticks,
	};

	for(int i = 0; i < 500; i++)
		server_weather_tick(&s);

	assert(s.weather.thunder_time == before.tt);
	assert(s.weather.rain_time == before.rt);
	assert(s.weather.raining == before.r);
	assert(s.weather.thundering == before.th);
	assert(s.weather.rain_strength == before.rs);
	assert(s.weather.thunder_strength == before.ts);
	assert(s.weather.flash_ticks == before.ft);
	/* isRaining/isThundering still just read the (frozen) strengths -- no
	 * special-case needed, but confirm they still answer sanely. */
	assert(server_weather_is_raining(&s) == (before.rs > 0.2f));

	puts("  hasNoSky (Nether): 500 ticks -> zero state change OK");
}

/* --- (5) sleep clears weather (World.wakeUpAllPlayers -> stopPrecipitation) - */

static void test_sleep_clears_weather(void) {
	reset_bare();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 424242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);
	/* A bed HEAD cell at the sleep position -- server_local_sleep_tick's
	 * !isInBed check only tests the block TYPE at sleep_x/y/z. */
	assert(server_world_set_block(&s.worlds[0], 8, 70, 8,
								  (struct block_data) {.type = BLOCK_BED}));

	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.sleeping = true;
	s.player.sleep_timer = 100; /* fully asleep -- Beta isPlayerFullyAsleep */
	s.player.sleep_x = 8;
	s.player.sleep_y = 70;
	s.player.sleep_z = 8;
	s.world_time = 13000; /* deep night: daytime_is_daytime_ticks is false */

	s.weather.raining = true;
	s.weather.thundering = true;
	s.weather.rain_time = 4242;
	s.weather.thunder_time = 1337;
	s.weather.rain_strength = 0.8f; /* untouched by stopPrecipitation itself */
	s.weather.thunder_strength = 0.6f;

	server_local_sleep_tick(&s);

	assert(!s.weather.raining && "stopPrecipitation: raining -> false");
	assert(s.weather.rain_time == 0 && "stopPrecipitation: rainTime -> 0");
	assert(!s.weather.thundering && "stopPrecipitation: thundering -> false");
	assert(s.weather.thunder_time == 0 && "stopPrecipitation: thunderTime -> 0");
	/* Strengths are NOT touched by stopPrecipitation -- only the ordinary
	 * updateWeather lerp ramps them down over subsequent ticks. */
	assert(s.weather.rain_strength == 0.8f);
	assert(s.weather.thunder_strength == 0.6f);

	puts("  sleep-to-morning clears raining/thundering flags+timers "
		 "(World.wakeUpAllPlayers -> stopPrecipitation) OK");

	server_world_destroy(&s.world);
	string_clear(level);
}

/* --- (6) NBT round-trip through a real level_archive save/load ------------- */

static void remove_save_files(void) {
	char path[256];
	const char* files[] = {"level.dat", "level.dat_new", "level.dat_old"};
	for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", TMP_WORLD, files[i]);
		remove(path);
	}
}

static void test_nbt_round_trip(void) {
	remove_save_files();
	MKDIR(TMP_WORLD);

	string_t name;
	string_init_set_str(name, TMP_WORLD);
	struct level_archive level;
	assert(level_archive_create_new(&level, name, 4242, (vec3) {8.5F, 70.0F, 8.5F}));

	int8_t raining = 1, thundering = 1;
	int32_t rain_time = 987654, thunder_time = -12345; /* Beta ints, signed OK */
	assert(level_archive_write(&level, LEVEL_WEATHER_RAINING, &raining));
	assert(level_archive_write(&level, LEVEL_WEATHER_RAIN_TIME, &rain_time));
	assert(level_archive_write(&level, LEVEL_WEATHER_THUNDERING, &thundering));
	assert(level_archive_write(&level, LEVEL_WEATHER_THUNDER_TIME, &thunder_time));
	assert(level_archive_save(&level));
	level_archive_destroy(&level);
	string_clear(name);

	string_t name2;
	string_init_set_str(name2, TMP_WORLD);
	struct level_archive level2;
	assert(level_archive_create(&level2, name2));

	int8_t raining_in = 0, thundering_in = 0;
	int32_t rain_time_in = 0, thunder_time_in = 0;
	assert(level_archive_read(&level2, LEVEL_WEATHER_RAINING, &raining_in, 0));
	assert(level_archive_read(&level2, LEVEL_WEATHER_RAIN_TIME, &rain_time_in, 0));
	assert(level_archive_read(&level2, LEVEL_WEATHER_THUNDERING, &thundering_in, 0));
	assert(level_archive_read(&level2, LEVEL_WEATHER_THUNDER_TIME, &thunder_time_in, 0));
	level_archive_destroy(&level2);
	string_clear(name2);

	assert(raining_in == 1);
	assert(rain_time_in == 987654);
	assert(thundering_in == 1);
	assert(thunder_time_in == -12345);

	/* Round 2: false/0, to prove the byte isn't just "always reads back 1". */
	remove_save_files();
	string_t name3;
	string_init_set_str(name3, TMP_WORLD);
	struct level_archive level3;
	assert(level_archive_create_new(&level3, name3, 1, (vec3) {8.5F, 70.0F, 8.5F}));
	int8_t f0 = 0;
	int32_t z0 = 0;
	assert(level_archive_write(&level3, LEVEL_WEATHER_RAINING, &f0));
	assert(level_archive_write(&level3, LEVEL_WEATHER_RAIN_TIME, &z0));
	assert(level_archive_write(&level3, LEVEL_WEATHER_THUNDERING, &f0));
	assert(level_archive_write(&level3, LEVEL_WEATHER_THUNDER_TIME, &z0));
	assert(level_archive_save(&level3));
	level_archive_destroy(&level3);
	string_clear(name3);

	string_t name4;
	string_init_set_str(name4, TMP_WORLD);
	struct level_archive level4;
	assert(level_archive_create(&level4, name4));
	int8_t raining_in2 = 1, thundering_in2 = 1;
	assert(level_archive_read(&level4, LEVEL_WEATHER_RAINING, &raining_in2, 0));
	assert(level_archive_read(&level4, LEVEL_WEATHER_THUNDERING, &thundering_in2, 0));
	level_archive_destroy(&level4);
	string_clear(name4);
	assert(raining_in2 == 0);
	assert(thundering_in2 == 0);

	remove_save_files();
	puts("  NBT round-trip: raining/rainTime/thundering/thunderTime (level.dat "
		 "save -> destroy -> reopen -> read) byte-exact OK");
}

int main(void) {
	test_fixture_dir("test_weather_timers_save_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	puts("test_weather_timers_env: Beta 1.7.3 World.updateWeather + WorldInfo NBT");
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	test_timer_countdown_and_flip();
	test_refill_ranges_not_thundering_not_raining();
	test_strength_lerp_and_clamp();
	test_strength_double_promotion_bit_exact();
	test_raining_thundering_thresholds();
	test_seed_produces_different_lcg();
	test_nether_fully_inert();
	test_sleep_clears_weather();
	test_nbt_round_trip();

	g_drain_run = 0;
	thread_join(&drain);
	puts("test_weather_timers_env: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
