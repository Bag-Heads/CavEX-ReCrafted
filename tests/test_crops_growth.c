/*
	Faithfulness golden for the Beta 1.7.3 crop growth gate (source/block/block_crops.c onRandomTick).
	Beta BlockCrops.updateTick advances a stage only when the cell ABOVE has light >= 9 AND a roll
	nextInt((int)(100/growthRate)) == 0 succeeds -- it is PROBABILISTIC, never every tick. The old code
	incremented the metadata on every qualifying random tick (~100x too fast) and ignored farmland
	fertility. This pins: (1) the light-above gate (dark -> never grows), (2) growth is NOT deterministic
	per tick (7 lit ticks do not reach stage 7), (3) growth still happens eventually, and (4) hydrated
	farmland grows faster than dry (the getGrowthRate fertility term).

	Production path: real superflat server_world + the block registry; calls the registered onRandomTick.
	Robust to the RNG (no fixed-seed dependence): the "not deterministic" check would need 7 consecutive
	1/100 hits to false-fail (~1e-14); "eventually grows" needs ~700 expected ticks vs a 200000 budget.
*/

#include <assert.h>
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
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_crops_growth_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70 // above the superflat surface -> open sky (sky_light 15) above the crop

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
static void set(int x, int y, int z, int type, int meta) {
	struct block_data expected = {
		.type = (uint8_t)type,
		.metadata = (uint8_t)(meta & 0x0F),
	};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static int light_above(int x, int y, int z) {
	struct block_data b;
	if(!server_world_get_block(&s.world, x, y + 1, z, &b))
		return -1;
	return b.sky_light > b.torch_light ? b.sky_light : b.torch_light;
}
// one crop random tick on the live cell (re-reads the world state each call, like the random-tick driver)
static void crop_tick(int x, int y, int z) {
	struct block_data cd;
	assert(server_world_get_block(&s.world, x, y, z, &cd));
	struct block_info info = {.block = &cd, .neighbours = NULL, .x = x, .y = y, .z = z};
	blocks[BLOCK_CROPS]->onRandomTick(&s, &info);
}

int main(void) {
	test_fixture_dir("test_crops_growth_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	// crop on DRY farmland, open sky above (lit)
	const int X = 4, Z = 4;
	set(X, Y, Z, BLOCK_FARMLAND, 0);	 // dry farmland (metadata 0)
	set(X, Y + 1, Z, BLOCK_CROPS, 0);	 // crop stage 0 on top
	assert(getmeta(X, Y + 1, Z) == 0);
	assert(light_above(X, Y + 1, Z) >= 9); // the cell above the crop is lit (else the test setup is wrong)

	// (2) growth is NOT deterministic per tick: 7 lit ticks must not reach stage 7
	for(int k = 0; k < 7; k++)
		crop_tick(X, Y + 1, Z);
	assert(getmeta(X, Y + 1, Z) < 7); // the old deterministic +1 would have hit 7 here

	// (3) but it does grow eventually
	for(int k = 0; k < 200000 && getmeta(X, Y + 1, Z) < 7; k++)
		crop_tick(X, Y + 1, Z);
	assert(getmeta(X, Y + 1, Z) == 7);

	// (1) the light gate: a crop with a SOLID block directly above never grows (light 0)
	const int X2 = 8, Z2 = 8;
	set(X2, Y, Z2, BLOCK_FARMLAND, 0);
	set(X2, Y + 1, Z2, BLOCK_CROPS, 0);
	set(X2, Y + 2, Z2, BLOCK_STONE, 0); // caps the crop -> the cell above reads light 0
	assert(light_above(X2, Y + 1, Z2) < 9);
	for(int k = 0; k < 2000; k++)
		crop_tick(X2, Y + 1, Z2);
	assert(getmeta(X2, Y + 1, Z2) == 0); // dark -> never advances, regardless of RNG

	// (4) the getGrowthRate fertility term: HYDRATED farmland (metadata>0 -> fertility 3.0) grows the
	//     crop faster than DRY (1.0). Statistical over many independent restarts so it is RNG-robust:
	//     dry rate ~2.0 (bound 50, p=0.02) vs hydrated ~4.0 (bound 25, p=0.04) -> hydrated ~2x. Over
	//     200 restarts the summed stages separate by a wide, never-crossing margin.
	const int XD = 12, ZD = 12, XH = 12, ZH = 16;
	set(XD, Y, ZD, BLOCK_FARMLAND, 0); // dry farmland
	set(XH, Y, ZH, BLOCK_FARMLAND, 7); // hydrated farmland (moisture metadata > 0)
	int sum_dry = 0, sum_hyd = 0;
	const int TRIALS = 200, BUDGET = 40;
	for(int t = 0; t < TRIALS; t++) {
		set(XD, Y + 1, ZD, BLOCK_CROPS, 0); // fresh stage-0 crop each trial
		set(XH, Y + 1, ZH, BLOCK_CROPS, 0);
		for(int k = 0; k < BUDGET; k++) {
			crop_tick(XD, Y + 1, ZD);
			crop_tick(XH, Y + 1, ZH);
		}
		sum_dry += getmeta(XD, Y + 1, ZD);
		sum_hyd += getmeta(XH, Y + 1, ZH);
	}
	assert(sum_hyd > sum_dry); // hydrated farmland advances the crop faster (the fertility term is wired)

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_crops_growth: OK (light-above gate, probabilistic not-per-tick growth, eventual growth, "
		   "fertility wired)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
