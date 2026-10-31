/* next-gaps-0924/F1 -- Beta 1.7.3 World.getBlockLightValue golden (docs/intent/f1-block-light-value.md).
 *
 * The spec (decompile 740c583, net/minecraft/src):
 *   World.getBlockLightValue_do(x,y,z,true)   World.java:537-583
 *     - |x|/|z| outside the +-32000000 window -> 15
 *     - the queried id is stairSingle(44) / tilledField(60) / stairCompactCobblestone(67) /
 *       stairCompactPlanks(53) -> max of the 5 neighbours (y+1, x+1, x-1, z+1, z-1), each read
 *       with the neighbour branch OFF                                   World.java:540-563
 *     - y < 0 -> 0; y >= 128 -> clamp to 127                            World.java:567-573
 *     - Chunk.getBlockLightValue(x,y,z,skylightSubtracted) = max(sky - subtract, block)
 *                                                                       Chunk.java:337-350
 *     - an out-of-range chunk is EmptyChunk: light 0, id 0              EmptyChunk.java:37-39,63-65
 *   World.calculateSkylightSubtracted(1.0F)    World.java:991-1007
 *     - the clear sun term, then * (1 - rain*5/16) and * (1 - thunder*rain*5/16), each promoted to
 *       double and narrowed back to float; thunder is weighted by rain (func_27166_f, World.java:2398)
 *   Block ids/opacity: Block.java:67,76,83,90; BlockStep/BlockStairs/BlockFarmland setLightOpacity(255)
 *   Callers: BlockCrops.java:20 (>= 9), BlockSapling.java:15 (>= 9), BlockGrass.java:32-43 (< 4 decay,
 *   >= 9 spread, target >= 4), EntityMob.getCanSpawnHere:69-86 (thunder forces subtract 10 for the read).
 *
 * The expectations below are computed by an independent transcription of that Java (beta_* in this
 * file) from the STORED light nibbles and ids -- never snapshotted from CaveWii's own helpers. The
 * clear-weather sun term is taken from daytime_celestial_angle + libm cos exactly as production computes
 * it (the MathHelper sin-table difference is pre-existing and out of scope, test_daylight_curve SCOPE);
 * every step after the cos -- incl. Java's 1-(clamp(1-x)) double inversion -- is the Java.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "daytime.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/spawner.h"
#include "platform/thread.h"

#define Y 100
static char fixture[128];
static struct server_local s;
static volatile int draining = 1;
static int failures = 0;

static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	if(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta})) return;
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	assert(b.type == type && b.metadata == meta);
}

static struct block_data at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b;
}

static void relight(void) {
	for(int i = 0; i < 6; i++) server_world_tick(&s.worlds[0], &s);
	server_world_light_drain(&s.worlds[0]); /* G9: World.tick's light drain */
}

static void check(bool ok, const char* what) {
	if(!ok) {
		failures++;
		fprintf(stderr, "RED %s\n", what);
	}
}

/* ---- independent Beta transcription ---------------------------------------------------------- */

// World.calculateSkylightSubtracted(1.0F), World.java:991-1007, from the clear sun term.
static int beta_subtract(uint64_t time, float rain, float thunder) {
	// The day/night sun term is the ONE Beta curve (daytime_sun_from_angle on MathHelper.cos, the 1.0F
	// partial), pinned tick by tick against an independent Java transcription in
	// test_skylight_tick_golden (w2). Everything from the weather multiply on is the Java, line for line.
	float var3 = 1.0F - daytime_sun_from_angle(daytime_beta_celestial_angle((int64_t)time - 1, 1.0F));
	if(var3 < 0.0F) var3 = 0.0F;
	if(var3 > 1.0F) var3 = 1.0F;
	var3 = 1.0F - var3;
	float weighted_thunder = thunder * rain; // func_27166_f(1.0F) = thunder * func_27162_g(1.0F)
	var3 = (float)((double)var3 * (1.0 - (double)(rain * 5.0F) / 16.0));
	var3 = (float)((double)var3 * (1.0 - (double)(weighted_thunder * 5.0F) / 16.0));
	var3 = 1.0F - var3;
	return (int)(var3 * 11.0F);
}

// World.getBlockId, World.java:311-313 (EmptyChunk id 0 for a non-resident chunk).
static int beta_block_id(int x, int y, int z) {
	if(!(x >= -32000000 && z >= -32000000 && x < 32000000 && z <= 32000000)) return 0;
	if(y < 0 || y >= 128) return 0;
	struct block_data b;
	if(!server_world_get_block(&s.worlds[0], x, y, z, &b)) return 0;
	return b.type;
}

// World.getBlockLightValue_do, World.java:537-583.
static int beta_light_do(int x, int y, int z, bool var4, int subtract) {
	if(!(x >= -32000000 && z >= -32000000 && x < 32000000 && z <= 32000000)) return 15;
	if(var4) {
		int id = beta_block_id(x, y, z);
		if(id == 44 || id == 60 || id == 67 || id == 53) {
			int v6 = beta_light_do(x, y + 1, z, false, subtract);
			int v7 = beta_light_do(x + 1, y, z, false, subtract);
			int v8 = beta_light_do(x - 1, y, z, false, subtract);
			int v9 = beta_light_do(x, y, z + 1, false, subtract);
			int v10 = beta_light_do(x, y, z - 1, false, subtract);
			if(v7 > v6) v6 = v7;
			if(v8 > v6) v6 = v8;
			if(v9 > v6) v6 = v9;
			if(v10 > v6) v6 = v10;
			return v6;
		}
	}
	if(y < 0) return 0;
	if(y >= 128) y = 127;
	struct block_data b;
	if(!server_world_get_block(&s.worlds[0], x, y, z, &b)) return 0; // EmptyChunk
	int v = (int)b.sky_light - subtract;
	if((int)b.torch_light > v) v = (int)b.torch_light;
	return v;
}

static int beta_light(int x, int y, int z, int subtract) {
	return beta_light_do(x, y, z, true, subtract);
}

// The pre-F1 read: the cell's own nibbles only, clear-weather subtract.
static int own_cell_light(int x, int y, int z, int subtract) {
	struct block_data b = at(x, y, z);
	int v = (int)b.sky_light - subtract;
	return (int)b.torch_light > v ? (int)b.torch_light : v;
}

/* ---- RNG helpers ------------------------------------------------------------------------------ */

static uint32_t hit_seed(int bound) {
	for(uint32_t seed = 1; seed < 1000000; seed++) {
		struct random_gen trial = {.seed = seed};
		if(rand_gen_int(&trial, bound) == 0) return seed;
	}
	assert(0 && "no deterministic hit seed");
	return 0;
}

static void set_weather(float rain, float thunder) {
	s.weather.raining = rain > 0.0F;
	s.weather.rain_strength = rain;
	s.weather.prev_rain_strength = rain;
	s.weather.thundering = thunder > 0.0F;
	s.weather.thunder_strength = thunder;
	s.weather.prev_thunder_strength = thunder;
}

/* One plant random tick; returns true when Beta's growth draw was consumed exactly as `eligible`
 * says (bound = the plant's roll), and the plant advanced iff eligible. */
static bool plant_tick(int type, int x, int z, bool eligible, int grown_meta) {
	int bound = type == BLOCK_CROPS ? 50 : 30; // dry isolated crop: growthRate 2 -> 50
	set(x, Y, z, type, 0); // every probe starts from a fresh stage-0 plant
	s.rand_src.seed = hit_seed(bound);
	s.rand_src.has_next_gaussian = false;
	struct random_gen expected = s.rand_src;
	if(eligible) assert(rand_gen_int(&expected, bound) == 0);
	struct block_data b = at(x, Y, z);
	struct block_info info = {.block = &b, .neighbours = NULL, .x = x, .y = Y, .z = z};
	blocks[type]->onRandomTick(&s, &info);
	bool draws_ok = s.rand_src.seed == expected.seed;
	struct block_data after = at(x, Y, z);
	bool meta_ok = after.type == type && after.metadata == (eligible ? grown_meta : 0);
	return draws_ok && meta_ok;
}

/* Grass random tick with the three spread draws (nextInt(3), nextInt(5), nextInt(3)) or the one
 * decay draw (nextInt(4)) or none; `expect` is 's' spread, 'd' decay, '-' nothing. The spread
 * neighbourhood is cleared to air so the spread itself converts nothing. */
static bool grass_tick(int x, int z, char expect) {
	s.rand_src.seed = 12345;
	s.rand_src.has_next_gaussian = false;
	struct random_gen expected = s.rand_src;
	if(expect == 's') {
		(void)rand_gen_int(&expected, 3);
		(void)rand_gen_int(&expected, 5);
		(void)rand_gen_int(&expected, 3);
	} else if(expect == 'd') {
		(void)rand_gen_int(&expected, 4);
	}
	struct block_data b = at(x, Y, z);
	struct block_info info = {.block = &b, .neighbours = NULL, .x = x, .y = Y, .z = z};
	blocks[BLOCK_GRASS]->onRandomTick(&s, &info);
	return s.rand_src.seed == expected.seed && at(x, Y, z).type == BLOCK_GRASS;
}

static void clear_box(int x0, int y0, int z0, int x1, int y1, int z1) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				set(x, y, z, BLOCK_AIR, 0);
}

/* ---- behavioural goldens (exercise the real block random ticks) ------------------------------- */

static void case_crop_under_slab_noon(void) {
	const int x = 2, z = 4;
	clear_box(x - 1, Y, z - 1, x + 1, Y + 3, z + 1);
	set(x, Y - 1, z, BLOCK_FARMLAND, 0);
	/* G9: cover first, settle, then plant. A slab placed straight onto a
	 * standing crop zeroes its cell at once (Chunk.func_339_g) and the
	 * neighbour notify pops it (BlockFlower.canBlockStay) -- Beta too. */
	set(x, Y + 1, z, BLOCK_SLAB, 0);
	relight();
	set(x, Y, z, BLOCK_CROPS, 0);
	relight();
	set_weather(0.0F, 0.0F);
	s.world_time = 6000;
	int sub = beta_subtract(6000, 0.0F, 0.0F);
	int beta = beta_light(x, Y + 1, z, sub);
	int own = own_cell_light(x, Y + 1, z, sub);
	printf("crop under slab, noon: beta light=%d (neighbour max), own-cell=%d\n", beta, own);
	assert(beta >= 9 && own < 9 && "fixture must separate neighbour max from the own-cell read");
	check(plant_tick(BLOCK_CROPS, x, z, true, 1), "crop under slab at noon: Beta grows (neighbour max >= 9)");
}

static void case_sapling_under_stairs_midnight_torch(void) {
	const int x = 13, z = 4;
	clear_box(x - 1, Y, z - 1, x + 1, Y + 3, z + 1);
	set(x, Y - 1, z, BLOCK_DIRT, 0);
	/* G9: cover first, settle, then plant (see case_crop_under_slab_noon). */
	set(x, Y + 1, z, BLOCK_WOODEN_STAIRS, 0);
	set(x + 1, Y, z, BLOCK_STONE, 0); // torch support
	set(x + 1, Y + 1, z, BLOCK_TORCH, 0);
	relight();
	set(x, Y, z, BLOCK_SAPLING, 0);
	relight();
	set_weather(0.0F, 0.0F);
	s.world_time = 18000;
	int sub = beta_subtract(18000, 0.0F, 0.0F);
	int beta = beta_light(x, Y + 1, z, sub);
	int own = own_cell_light(x, Y + 1, z, sub);
	printf("sapling under wooden stairs, midnight, torch beside: beta light=%d, own-cell=%d\n", beta, own);
	assert(beta >= 9 && own < 9 && "fixture must separate neighbour max from the own-cell read");
	check(plant_tick(BLOCK_SAPLING, x, z, true, 8), "sapling under stairs + torch at midnight: Beta ages it");
}

static void case_grass_under_slab_noon(void) {
	const int x = 8, z = 10;
	clear_box(x - 1, Y - 3, z - 1, x + 1, Y + 3, z + 1);
	set(x, Y, z, BLOCK_GRASS, 0);
	set(x, Y + 1, z, BLOCK_SLAB, 0);
	relight();
	set_weather(0.0F, 0.0F);
	s.world_time = 6000;
	int sub = beta_subtract(6000, 0.0F, 0.0F);
	int beta = beta_light(x, Y + 1, z, sub);
	int own = own_cell_light(x, Y + 1, z, sub);
	printf("grass under slab, noon: beta light=%d, own-cell=%d\n", beta, own);
	assert(beta >= 9 && own < 4);
	// Beta: not < 4, so no decay; >= 9 -> the spread branch's three draws.
	check(grass_tick(x, z, 's'), "grass under slab at noon: Beta spreads (3 draws), never decays");
}

/* Dusk tick where full rain moves the crop/grass gate: clear subtract 6 (light 9), rain 7 (light 8). */
static uint64_t find_rain_gate_tick(void) {
	for(uint64_t t = 12000; t < 14000; t++)
		if(beta_subtract(t, 0.0F, 0.0F) == 6 && beta_subtract(t, 1.0F, 0.0F) == 7)
			return t;
	assert(0 && "no dusk tick where rain moves 6 -> 7");
	return 0;
}

static void case_rain_at_dusk(void) {
	uint64_t t = find_rain_gate_tick();
	const int cx = 5, cz = 12, gx = 11, gz = 13;
	clear_box(cx - 1, Y, cz - 1, cx + 1, Y + 3, cz + 1);
	set(cx, Y - 1, cz, BLOCK_FARMLAND, 0);
	set(cx, Y, cz, BLOCK_CROPS, 0);
	clear_box(gx - 1, Y - 3, gz - 1, gx + 1, Y + 3, gz + 1);
	set(gx, Y, gz, BLOCK_GRASS, 0);
	relight();
	assert(at(cx, Y + 1, cz).sky_light == 15 && at(gx, Y + 1, gz).sky_light == 15);
	s.world_time = t;

	set_weather(0.0F, 0.0F);
	printf("dusk t=%llu clear: subtract %d -> light %d\n", (unsigned long long)t,
		beta_subtract(t, 0.0F, 0.0F), beta_light(cx, Y + 1, cz, beta_subtract(t, 0.0F, 0.0F)));
	check(plant_tick(BLOCK_CROPS, cx, cz, true, 1), "clear dusk: crop grows (light 9)");
	check(grass_tick(gx, gz, 's'), "clear dusk: grass spreads (light 9)");

	set_weather(1.0F, 0.0F);
	int rain_sub = beta_subtract(t, 1.0F, 0.0F);
	printf("dusk t=%llu full rain: subtract %d -> light %d\n", (unsigned long long)t, rain_sub,
		beta_light(cx, Y + 1, cz, rain_sub));
	check(plant_tick(BLOCK_CROPS, cx, cz, false, 1), "rainy dusk: Beta crop does NOT grow (light 8)");
	check(grass_tick(gx, gz, '-'), "rainy dusk: Beta grass neither spreads nor decays (light 8, air above)");

	// Full thunder at noon: subtract 5 (World.java:1003-1004) -> light 10, crops still grow.
	s.world_time = 6000;
	set_weather(1.0F, 1.0F);
	assert(beta_subtract(6000, 1.0F, 1.0F) == 5);
	check(plant_tick(BLOCK_CROPS, cx, cz, true, 1), "thunderstorm noon: crop grows (light 10)");
	set_weather(0.0F, 0.0F);
}


/* ---- helper-level goldens (the new shared read; stage 2) --------------------------------------- */

// Clear weather: the new weather-aware subtract is byte-identical to the pinned clear curve for every
// tick; with weather it equals the Java transcription on a strength grid.
static void case_subtract_identity(void) {
	int mism = 0;
	const float rains[] = {0.0F, 0.01F, 0.2F, 0.21F, 0.5F, 0.99F, 1.0F};
	const float thunders[] = {0.0F, 0.5F, 0.9F, 1.0F};
	for(uint64_t t = 0; t < DAY_LENGTH_TICKS; t++) {
		s.world_time = t;
		set_weather(0.0F, 0.0F);
		if(server_local_skylight_subtracted(&s) != server_local_sky_subtract(t)) {
			fprintf(stderr, "  clear t=%llu new=%d old=%d beta=%d\n", (unsigned long long)t,
				server_local_skylight_subtracted(&s), server_local_sky_subtract(t),
				beta_subtract(t, 0.0F, 0.0F));
			mism++;
		}
		for(size_t r = 0; r < sizeof rains / sizeof rains[0]; r++)
			for(size_t h = 0; h < sizeof thunders / sizeof thunders[0]; h++) {
				s.weather.rain_strength = rains[r];
				s.weather.thunder_strength = thunders[h];
				if(server_local_skylight_subtracted(&s) != beta_subtract(t, rains[r], thunders[h])) {
					fprintf(stderr, "  weather t=%llu rain=%g thunder=%g new=%d beta=%d\n",
						(unsigned long long)t, rains[r], thunders[h],
						server_local_skylight_subtracted(&s), beta_subtract(t, rains[r], thunders[h]));
					mism++;
				}
			}
	}
	set_weather(0.0F, 0.0F);
	printf("skylightSubtracted: 24000 ticks x (clear identity + 28 weather points), mismatches=%d\n", mism);
	check(mism == 0, "server_local_skylight_subtracted == clear curve / Beta weather formula");
	// Landmarks: noon full rain 3, noon full thunderstorm 5 (World.java:1003-1004).
	s.world_time = 6000;
	set_weather(1.0F, 0.0F);
	check(server_local_skylight_subtracted(&s) == 3, "noon full rain subtract 3");
	set_weather(1.0F, 1.0F);
	check(server_local_skylight_subtracted(&s) == 5, "noon full thunderstorm subtract 5");
	set_weather(0.0F, 1.0F);
	check(server_local_skylight_subtracted(&s) == 0, "thunder without rain darkens nothing (weighted)");
	set_weather(0.0F, 0.0F);
}

// Every cell of a region (incl. the non-resident ring around chunk 0,0 and the y bounds) against
// the Java transcription, for three subtracts.
static void case_helper_region(void) {
	// Extra fixtures: a slab on the chunk edge (x-1 neighbour non-resident), farmland with a
	// torch beside it, stone stairs, a double slab (NOT in the Beta list), a slab at the world top.
	clear_box(0, Y + 5, 0, 3, Y + 8, 3);
	set(0, Y + 6, 1, BLOCK_SLAB, 0);
	set(2, Y + 6, 2, BLOCK_FARMLAND, 0);
	set(3, Y + 6, 2, BLOCK_STONE, 0);
	set(3, Y + 7, 2, BLOCK_TORCH, 0);
	set(1, Y + 7, 3, BLOCK_STONE_STAIRS, 0);
	set(2, Y + 8, 0, BLOCK_DOUBLE_SLAB, 0);
	set(6, 127, 6, BLOCK_SLAB, 0);
	relight();
	const int subs[] = {0, 6, 11};
	const int ys[] = {-1, 0, Y - 2, Y - 1, Y, Y + 1, Y + 2, Y + 3, Y + 5, Y + 6,
		Y + 7, Y + 8, Y + 9, 126, 127, 128, 200};
	int cells = 0, mism = 0, branch = 0;
	for(size_t k = 0; k < sizeof subs / sizeof subs[0]; k++)
		for(int x = -2; x <= 17; x++)
			for(int z = -2; z <= 17; z++)
				for(size_t yi = 0; yi < sizeof ys / sizeof ys[0]; yi++) {
					int y = ys[yi];
					int got = server_local_block_light_value_sub(&s, x, y, z, subs[k]);
					int want = beta_light(x, y, z, subs[k]);
					cells++;
					int id = beta_block_id(x, y, z);
					if(id == 44 || id == 60 || id == 53 || id == 67) branch++;
					if(got != want) {
						if(mism < 8)
							fprintf(stderr, "  mismatch (%d,%d,%d) sub=%d got=%d want=%d\n", x, y, z,
								subs[k], got, want);
						mism++;
					}
				}
	printf("getBlockLightValue region: %d reads (%d neighbour-max cells), mismatches=%d\n", cells,
		branch, mism);
	check(mism == 0 && branch > 0, "server_local_block_light_value_sub == Beta getBlockLightValue_do");
	// Named landmarks.
	check(server_local_block_light_value_sub(&s, 0, Y + 6, 1, 0) == 15,
		"edge slab: non-resident x-1 neighbour reads 0, max still from the resident ones");
	check(server_local_block_light_value_sub(&s, 2, Y + 8, 0, 11) == beta_light_do(2, Y + 8, 0, false, 11),
		"double slab 43 is NOT a neighbour-max block");
	check(server_local_block_light_value_sub(&s, 6, 128, 6, 0) == beta_light_do(6, 127, 6, false, 0),
		"y=128 reads air id (no neighbour branch) and the y=127 light (clamp)");
	check(server_local_block_light_value_sub(&s, 5, -1, 5, 0) == 0, "y<0 -> 0");
	check(server_local_block_light_value_sub(&s, 31999999, 64, 0, 0) == 0, "non-resident in window -> 0");
	check(server_local_block_light_value_sub(&s, 32000000, 64, 0, 0) == 15, "x=32000000 outside -> 15");
	check(server_local_block_light_value_sub(&s, 0, 64, 32000000, 0) == 0, "z=32000000 inside (<=) -> 0");
	check(server_local_block_light_value_sub(&s, 0, 64, 32000001, 0) == 15, "z=32000001 outside -> 15");
	check(server_local_block_light_value_sub(&s, -32000001, 64, 0, 0) == 15, "x=-32000001 outside -> 15");
}

// EntityMob.getCanSpawnHere var4: thunder forces subtract 10 (neighbour max included); rain alone
// darkens by the weather subtract; the Nether never thunders and subtracts 11.
static void case_hostile_spawn_light(void) {
	const int x = 9, z = 2;
	clear_box(x - 1, Y + 10, z - 1, x + 1, Y + 13, z + 1);
	set(x, Y + 10, z, BLOCK_STONE, 0);
	set(x, Y + 11, z, BLOCK_SLAB, 0); // a mob can stand in a slab cell (not a normal cube)
	relight();
	s.world_time = 6000;
	set_weather(1.0F, 1.0F);
	int thunder = server_local_hostile_spawn_light(&s, x, Y + 11, z);
	set_weather(1.0F, 0.0F);
	int rain = server_local_hostile_spawn_light(&s, x, Y + 11, z);
	set_weather(0.5F, 1.0F); // weighted thunder 0.5 <= 0.9: not thundering (func_27160_B)
	int half = server_local_hostile_spawn_light(&s, x, Y + 11, z);
	set_weather(0.0F, 0.0F);
	int clear = server_local_hostile_spawn_light(&s, x, Y + 11, z);
	printf("hostile spawn var4 in a slab cell at noon: thunder=%d rain=%d half-rain+thunder=%d clear=%d\n",
		thunder, rain, half, clear);
	check(thunder == beta_light(x, Y + 11, z, 10) && thunder == 5, "thunder override 10 with neighbour max");
	check(rain == beta_light(x, Y + 11, z, beta_subtract(6000, 1.0F, 0.0F)) && rain == 12,
		"rain alone: weather subtract 3, no override");
	check(half == beta_light(x, Y + 11, z, beta_subtract(6000, 0.5F, 1.0F)),
		"weighted thunder below 0.9: no override");
	check(clear == 15, "clear noon slab cell: neighbour max 15");
	enum world_dim dim = s.worlds[0].dimension;
	s.worlds[0].dimension = WORLD_DIM_NETHER;
	set_weather(1.0F, 1.0F); // a frozen overworld storm state must not leak into the Nether
	check(server_local_skylight_subtracted(&s) == 11, "Nether skylightSubtracted 11 (WorldProviderHell angle 0.5)");
	check(server_local_hostile_spawn_light(&s, x, Y + 11, z) == beta_light(x, Y + 11, z, 11),
		"Nether: no thunder override");
	s.worlds[0].dimension = dim;
	set_weather(0.0F, 0.0F);
}

// The spawner refactor: spawner_hostile_light_ok == spawner_hostile_light_value_ok(max(sky-sub,torch))
// in result AND RNG state, exhaustively over the nibble/subtract space.
static void case_spawner_gate_equiv(void) {
	int mism = 0, n = 0;
	for(uint32_t seed = 1; seed <= 64; seed++)
		for(int sky = 0; sky <= 15; sky++)
			for(int torch = 0; torch <= 15; torch++)
				for(int sub = 0; sub <= 11; sub++) {
					struct random_gen a = {.seed = seed * 2654435761u}, b = a;
					int v = sky - sub > torch ? sky - sub : torch;
					bool ra = spawner_hostile_light_ok(&a, sky, torch, sub);
					bool rb = spawner_hostile_light_value_ok(&b, sky, v);
					n++;
					if(ra != rb || a.seed != b.seed) mism++;
				}
	printf("spawner gate refactor: %d cases, mismatches=%d\n", n, mism);
	check(mism == 0, "spawner_hostile_light_ok == spawner_hostile_light_value_ok (result + draws)");
}

int main(void) {
	test_fixture_dir("test_block_light_value_golden_tmp", fixture, sizeof fixture);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	remove(test_fixture_join(fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);

	case_crop_under_slab_noon();
	case_sapling_under_stairs_midnight_torch();
	case_grass_under_slab_noon();
	case_rain_at_dusk();
	case_subtract_identity();
	case_helper_region();
	case_hostile_spawn_light();
	case_spawner_gate_equiv();

	draining = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	if(failures) {
		fprintf(stderr, "test_block_light_value_golden: %d FAILED\n", failures);
		return 1;
	}
	printf("test_block_light_value_golden: all PASS\n");
	return 0;
}
