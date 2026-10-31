/*
	Golden test: Beta 1.7.3 snow/ice melt (BlockSnow.updateTick, BlockIce.updateTick),
	snowfall placement + water freeze (World.updateBlocksAndPlayCaveSounds's
	snow/freeze branch), and the World.canBlockSeeTheSky / canBlockBeRainedOn
	primitives [740c583].

	MELT is BLOCK-light driven, NOT temperature (gap #2, decompile-corrected --
	see WEATHER_DISPATCH.md):
		BlockSnow.updateTick:  getSavedLightValue(BLOCK,...) >  11              -> melt (air, no drop)
		BlockIce.updateTick:   getSavedLightValue(BLOCK,...) >  11 - opacity(3) -> melt (waterStill, no drop)
	Both BlockSnow.quantityDropped() and BlockIce.quantityDropped() return 0, so
	dropBlockAsItem's own draw loop runs ZERO iterations on melt -- no item
	spawns (verified below), unlike the player-mined harvestBlock path.

	Hand-computed melt thresholds are read back from the REAL lighting engine
	(a straight glowstone corridor, opacity-adjusted per placed block) rather
	than hand-poking the light nibble -- the engine finalizes/recomputes the
	light field on every set_block (server_world.c), so an arbitrary hinted
	torch_light on a non-luminant block would just be overwritten by the real
	flood; reading back the ACTUAL computed value is the only faithful way to
	test an exact-nibble boundary (matches test_crops_growth.c's own light-gate
	convention, generalized from a bucket check to the two exact boundary
	nibbles the dispatch calls out).
*/

#include <assert.h>
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
#include "entity/entity.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "worldgen/worldgen.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_weather_snow_ice_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70

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
	struct block_data expected
		= {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)};
	/* G9: these melt-threshold fixtures want settled light; each write is
	 * followed by the tick light drain (pending-light reads of ice are
	 * covered by test_light_drain_order_golden__ice_melt). */
	if(server_world_set_block(&s.worlds[0], x, y, z, expected)) {
		server_world_light_drain(&s.worlds[0]);
		return;
	}
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static struct block_data get(int x, int y, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b;
}

static void tick_block(int type, int x, int y, int z) {
	struct block_data cd = get(x, y, z);
	struct block_info info = {.block = &cd, .neighbours = NULL, .x = x, .y = y, .z = z};
	assert(blocks[type]->onRandomTick);
	blocks[type]->onRandomTick(&s, &info);
}

static int count_items(void) {
	int n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM)
			n++;
		dict_entity_next(it);
	}
	return n;
}

/* --- corridor fixture: glowstone at X=0, an open 1-wide air corridor along +X,
 * walled top/bottom/sides with stone -- a straight, unobstructed light BFS
 * path so the falloff at each X is the engine's real, exact computation. --- */
#define CORRIDOR_Z 8
#define CORRIDOR_LEN 20

static void build_corridor(void) {
	for(int x = -1; x <= CORRIDOR_LEN; x++) {
		set(x, Y - 1, CORRIDOR_Z, BLOCK_STONE, 0); // floor
		set(x, Y + 1, CORRIDOR_Z, BLOCK_STONE, 0); // ceiling
		set(x, Y, CORRIDOR_Z - 1, BLOCK_STONE, 0); // wall
		set(x, Y, CORRIDOR_Z + 1, BLOCK_STONE, 0); // wall
		set(x, Y, CORRIDOR_Z, BLOCK_AIR, 0);
	}
	set(0, Y, CORRIDOR_Z, BLOCK_GLOWSTONE, 0);
}

/* Place `type` at corridor position x, read back its ACTUAL torch_light, then
 * restore the cell to air (so later scans at other x are not attenuated by
 * an opaque leftover -- ice is opacity 3, unlike snow/air). */
static int light_of_placed(int type, int x) {
	set(x, Y, CORRIDOR_Z, type, 0);
	server_world_light_drain(&s.worlds[0]); /* G9: the settled value */
	struct block_data b = get(x, Y, CORRIDOR_Z);
	int light = (int)b.torch_light;
	set(x, Y, CORRIDOR_Z, BLOCK_AIR, 0);
	return light;
}

/* Scan the corridor for the two positions bracketing the given threshold:
 * *at_thresh gets the light == threshold itself (must NOT melt); *above gets
 * light == threshold+1 (must melt). Asserts both are found -- a corridor too
 * short (or a lighting-engine mismatch) fails loudly rather than silently
 * testing the wrong boundary. */
static void find_boundary(int type, int threshold, int* x_at, int* x_above) {
	*x_at = -1;
	*x_above = -1;
	for(int x = 1; x <= CORRIDOR_LEN; x++) {
		int light = light_of_placed(type, x);
		if(light == threshold && *x_at < 0)
			*x_at = x;
		if(light == threshold + 1 && *x_above < 0)
			*x_above = x;
	}
	assert(*x_at >= 0
		   && "corridor fixture must reach the exact threshold light value");
	assert(*x_above >= 0
		   && "corridor fixture must reach exactly one light level above the threshold");
}

static void test_snow_melt_boundary(void) {
	build_corridor();
	int x_at11, x_above11;
	find_boundary(BLOCK_SNOW, 11, &x_at11, &x_above11);

	/* light == 11: NOT > 11 -> no melt. */
	set(x_at11, Y, CORRIDOR_Z, BLOCK_SNOW, 0);
	assert(get(x_at11, Y, CORRIDOR_Z).torch_light == 11);
	int items_before = count_items();
	tick_block(BLOCK_SNOW, x_at11, Y, CORRIDOR_Z);
	assert(get(x_at11, Y, CORRIDOR_Z).type == BLOCK_SNOW
		   && "light exactly 11 must NOT melt snow (Beta: strictly > 11)");
	assert(count_items() == items_before);

	/* light == 12: > 11 -> melts to air, drops NOTHING (quantityDropped==0). */
	set(x_above11, Y, CORRIDOR_Z, BLOCK_SNOW, 0);
	assert(get(x_above11, Y, CORRIDOR_Z).torch_light == 12);
	items_before = count_items();
	tick_block(BLOCK_SNOW, x_above11, Y, CORRIDOR_Z);
	assert(get(x_above11, Y, CORRIDOR_Z).type == BLOCK_AIR
		   && "light 12 (>11) must melt snow to air");
	assert(count_items() == items_before
		   && "melt drops NOTHING -- BlockSnow.quantityDropped()==0, unlike harvestBlock");

	puts("  BlockSnow.updateTick: light>11 melts (no drop), ==11 does not "
		 "(exact-nibble boundary via real corridor light) OK");
}

static void test_ice_melt_boundary(void) {
	build_corridor();
	/* threshold = 11 - lightOpacity[ice] = 11 - 3 = 8. */
	int x_at8, x_above8;
	find_boundary(BLOCK_ICE, 8, &x_at8, &x_above8);

	set(x_at8, Y, CORRIDOR_Z, BLOCK_ICE, 0);
	assert(get(x_at8, Y, CORRIDOR_Z).torch_light == 8);
	int items_before = count_items();
	tick_block(BLOCK_ICE, x_at8, Y, CORRIDOR_Z);
	assert(get(x_at8, Y, CORRIDOR_Z).type == BLOCK_ICE
		   && "light exactly 8 (== 11-opacity) must NOT melt ice");
	assert(count_items() == items_before);

	set(x_above8, Y, CORRIDOR_Z, BLOCK_ICE, 0);
	assert(get(x_above8, Y, CORRIDOR_Z).torch_light == 9);
	items_before = count_items();
	tick_block(BLOCK_ICE, x_above8, Y, CORRIDOR_Z);
	assert(get(x_above8, Y, CORRIDOR_Z).type == BLOCK_WATER_STILL
		   && "light 9 (>8) must melt ice to still water");
	assert(count_items() == items_before
		   && "melt drops NOTHING -- BlockIce.quantityDropped()==0 too");

	puts("  BlockIce.updateTick: light>8 (11-opacity3) melts to waterStill "
		 "(no drop), ==8 does not OK");
}

static void test_solid_snow_block_never_melts(void) {
	/* Beta's solid "Snow Block" (id 80) has NO updateTick at all -- confirmed
	 * structurally (onRandomTick must be NULL) rather than by absence of
	 * effect, which could pass for the wrong reason under any light. */
	assert(blocks[BLOCK_SNOW_BLOCK]->onRandomTick == NULL
		   && "Beta's solid snow block has no melt updateTick");
	puts("  Snow Block (id 80): no onRandomTick registered (Beta has no "
		 "melt on the solid block) OK");
}

/* --- canBlockSeeTheSky / canBlockBeRainedOn (world geometry only) ---------- */

static void test_can_see_sky_and_rained_on(void) {
	/* Open superflat column: nothing above the surface. */
	set(30, Y, 30, BLOCK_AIR, 0);
	assert(world_can_block_see_sky(&s, 30, Y, 30));

	/* Cap it with a solid roof: no longer sees sky. */
	set(30, Y + 3, 30, BLOCK_STONE, 0);
	assert(!world_can_block_see_sky(&s, 30, Y, 30));
	set(30, Y + 3, 30, BLOCK_AIR, 0); // uncap

	/* canBlockBeRainedOn requires isRaining() -- false while not raining,
	 * regardless of sky visibility. */
	s.weather.rain_strength = 0.0f;
	s.weather.prev_rain_strength = 0.0f;
	assert(!server_weather_is_raining(&s));
	assert(!world_can_block_be_rained_on(&s, 30, Y, 30));

	/* Now raining: an open column in a non-snow, rain-enabled biome must be
	 * rainable. worldgen_biome_at is queried directly (not the private
	 * weather_biome_enable_snow/rain helpers) so this assertion is an
	 * independent check, not a tautology against the implementation. */
	s.weather.rain_strength = 1.0f;
	s.weather.prev_rain_strength = 1.0f;
	assert(server_weather_is_raining(&s));
	enum biome b = worldgen_biome_at(s.world.generator, 30, 30);
	bool enable_snow = (b == BIOME_TAIGA || b == BIOME_ICE_DESERT || b == BIOME_TUNDRA);
	bool enable_rain = (b != BIOME_DESERT && b != BIOME_ICE_DESERT);
	bool expect_rained_on = !enable_snow && enable_rain;
	assert(world_can_block_be_rained_on(&s, 30, Y, 30) == expect_rained_on);

	puts("  canBlockSeeTheSky (open/capped) + canBlockBeRainedOn "
		 "(isRaining && sky && biome gate) OK");
}

/* --- snowfall placement + water freeze (biome-gated; needs a real taiga/
 * tundra/ice-desert column, per the decompile's getEnableSnow() gate around
 * BOTH branches) ------------------------------------------------------------- */

static bool biome_enables_snow(enum biome b) {
	return b == BIOME_TAIGA || b == BIOME_ICE_DESERT || b == BIOME_TUNDRA;
}

/* Scan outward from spawn for a chunk whose corner biome enables snow. Fixed
 * seed -> deterministic biome map -> deterministic scan result; if worldgen
 * or the biome table ever changes shape, this fails loudly (assert) instead
 * of silently skipping the mechanic it exists to prove. */
static bool find_snow_biome_chunk(int* out_cx, int* out_cz) {
	for(int r = 0; r <= 96; r++) {
		for(int cx = -r; cx <= r; cx++) {
			for(int cz = -r; cz <= r; cz++) {
				if(abs(cx) != r && abs(cz) != r)
					continue; // only scan the growing ring's edge
				enum biome b = worldgen_biome_at(
					s.world.generator, cx * 16 + 8, cz * 16 + 8);
				if(biome_enables_snow(b)) {
					*out_cx = cx;
					*out_cz = cz;
					return true;
				}
			}
		}
	}
	return false;
}

static void test_snowfall_and_freeze(void) {
	int cx, cz;
	bool found = find_snow_biome_chunk(&cx, &cz);
	assert(found && "fixed-seed worldgen must contain a snow-enabled biome "
					"within a 96-chunk scan (taiga/tundra/ice-desert)");

	int bx = cx * 16 + 4, bz = cz * 16 + 4; // a column inside that chunk
	struct server_chunk* sc = NULL;
	/* px/pz below are set to cx/cz (the target chunk itself), so only that
	 * one chunk needs to be resident for the radius-9 walk to visit it. */
	assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = bx + 0.5;
	s.player.y = Y;
	s.player.z = bz + 0.5;
	s.rand_src.seed = 0xC0FFEEu;

	/* Blanket the WHOLE chunk floor with a solid non-ice, non-air block open
	 * to the sky, so ANY position the position-LCG picks inside this chunk
	 * qualifies -- makes the probabilistic per-tick gate (1/16 chunk chance x
	 * 1/256 position choice) converge in a bounded trial budget instead of
	 * needing ~4096 tries for one specific column.
	 * F7(b) (adversarial review): clear the ENTIRE column above the forced
	 * floor (not just the one Y/Y-1 pair) -- without this, a natural
	 * worldgen decoration reaching above Y in this chunk (a tree, since
	 * populate() still runs on a superflat base) would make
	 * findTopSolidBlock(x,z) return THAT block's height instead of the
	 * forced floor, and the test would silently exercise whatever natural
	 * terrain happens to be there instead of the deterministic fixture this
	 * test is supposed to be. */
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++) {
			int x = cx * 16 + dx, z = cz * 16 + dz;
			for(int y = Y; y < WORLD_HEIGHT; y++)
				set(x, y, z, BLOCK_AIR, 0);
			set(x, Y - 1, z, BLOCK_DIRT, 0); // solid, opaque, not ice
			set(x, Y, z, BLOCK_AIR, 0);       // open air above it
		}

	s.weather.raining = true;
	s.weather.rain_strength = 1.0f;
	s.weather.prev_rain_strength = 1.0f;
	s.weather.thundering = false;
	s.weather.thunder_strength = 0.0f;

	int w_px = cx, w_pz = cz;
	bool snowed = false;
	const int BUDGET = 20000;
	int i;
	for(i = 0; i < BUDGET && !snowed; i++) {
		server_weather_effects_tick(&s, w_px, w_pz);
		for(int dx = 0; dx < 16 && !snowed; dx++)
			for(int dz = 0; dz < 16 && !snowed; dz++) {
				int x = cx * 16 + dx, z = cz * 16 + dz;
				if(get(x, Y, z).type == BLOCK_SNOW)
					snowed = true;
			}
	}
	assert(snowed && "snowfall must place BLOCK_SNOW somewhere in a "
					 "snow-enabled biome while raining, within the trial budget");
	printf("  snowfall: BLOCK_SNOW placed after %d effects-tick trial(s) "
		   "in a snow-enabled biome OK\n", i);

	/* Water freeze: same branch, same getEnableSnow() gate. Lay a still-water
	 * pool under the open sky (metadata 0 -- Beta's freeze gate also checks
	 * this) across the same chunk and run the trial again. */
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++) {
			int x = cx * 16 + dx, z = cz * 16 + dz;
			set(x, Y, z, BLOCK_AIR, 0);        // clear any snow from above
			set(x, Y - 1, z, BLOCK_WATER_STILL, 0);
		}

	bool froze = false;
	for(i = 0; i < BUDGET && !froze; i++) {
		server_weather_effects_tick(&s, w_px, w_pz);
		for(int dx = 0; dx < 16 && !froze; dx++)
			for(int dz = 0; dz < 16 && !froze; dz++) {
				int x = cx * 16 + dx, z = cz * 16 + dz;
				if(get(x, Y - 1, z).type == BLOCK_ICE)
					froze = true;
			}
	}
	assert(froze && "still water must freeze to ice somewhere in a "
					"snow-enabled biome, within the trial budget");
	printf("  water freeze: BLOCK_ICE formed after %d effects-tick trial(s) "
		   "in a snow-enabled biome OK\n", i);
}

/* Negative control: NOT raining -> snowfall never places snow, no matter how
 * many trials, in the same snow-enabled setup. */
static void test_no_snowfall_without_rain(void) {
	int cx, cz;
	assert(find_snow_biome_chunk(&cx, &cz));
	int bx = cx * 16 + 4, bz = cz * 16 + 4;
	if(!server_world_is_chunk_loaded(&s.world, cx, cz)) {
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
	}

	// F7(b): clear the full column above the forced floor -- see the
	// identical comment in test_snowfall_and_freeze.
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++) {
			int x = cx * 16 + dx, z = cz * 16 + dz;
			for(int y = Y; y < WORLD_HEIGHT; y++)
				set(x, y, z, BLOCK_AIR, 0);
			set(x, Y - 1, z, BLOCK_DIRT, 0);
			set(x, Y, z, BLOCK_AIR, 0);
		}

	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = bx + 0.5;
	s.player.y = Y;
	s.player.z = bz + 0.5;
	s.weather.raining = false;
	s.weather.rain_strength = 0.0f;
	s.weather.prev_rain_strength = 0.0f;

	for(int i = 0; i < 4000; i++)
		server_weather_effects_tick(&s, cx, cz);

	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++) {
			int x = cx * 16 + dx, z = cz * 16 + dz;
			assert(get(x, Y, z).type == BLOCK_AIR
				   && "no rain -> no snow, ever, regardless of biome/light");
		}
	puts("  negative control: not raining -> zero snowfall over 4000 trials OK");
}

int main(void) {
	test_fixture_dir("test_weather_snow_ice_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	puts("test_weather_snow_ice_env: Beta 1.7.3 BlockSnow/BlockIce melt + "
		 "snowfall/freeze");
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 424242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.rand_src.seed = 0x51DEB001u;

	struct server_chunk* sc = NULL;
	/* Corridor spans x=[-1,20] at z=8 -> chunks x={-1,0,1}, z=0. Plus (1,1)
	 * for the sky-visibility test at (30,*,30). */
	assert(server_world_load_chunk(&s.world, -1, 0, &sc) && sc);
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);
	assert(server_world_load_chunk(&s.world, 1, 0, &sc) && sc);
	assert(server_world_load_chunk(&s.world, 1, 1, &sc) && sc);

	test_snow_melt_boundary();
	test_ice_melt_boundary();
	test_solid_snow_block_never_melts();
	test_can_see_sky_and_rained_on();
	test_snowfall_and_freeze();
	test_no_snowfall_without_rain();

	g_drain_run = 0;
	thread_join(&drain);
	puts("test_weather_snow_ice_env: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
