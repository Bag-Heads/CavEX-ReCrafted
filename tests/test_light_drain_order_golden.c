/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_light_drain_order_golden -- G9 consumer goldens (KANBAN "server light
 * updates are synchronous; Beta drains them after World.tick"). Golden-first:
 * these pin Beta's light values AND the simulation consumers that read them
 * BEFORE the queue/drain exists. Each case is its own ctest entry
 * (test_light_drain_order_golden__<case>); RED (WILL_FAIL) at 06b7f2a06,
 * GREEN since the server light queue landed (G9 B2-B5). Astra ruling
 * ASTRA-1005B lines 6-7 (B1): consumer/callback traces, placement AND removal,
 * multiple writes, spawn/crop/ice outcomes and RNG, across chunks and worlds.
 *
 * Spec (CFR server decompile, /c/tmp/mcb173-server-decompile/out/net/
 * minecraft/src/):
 *   Chunk.setBlockIDWithMetadata:208-239 -- an opaque block at or above the
 *     column height calls func_339_g(x, y + 1, z) IMMEDIATELY; a transparent
 *     block at height-1 calls func_339_g(x, y, z) IMMEDIATELY; then Sky (:230)
 *     and Block (:232) lighting are only SCHEDULED
 *     (World.scheduleLightingUpdate -> World.func_484_a queues a
 *     MetadataChunkBlock, writes nothing; World.java:1293-1330).
 *   Chunk.func_339_g:141-200 -- moves heightMap and writes the column's
 *     skylight at once: a raise sets [old, new) to 0 (:178-180), a lowering
 *     sets [new, old) to 15 (:174-176), then the attenuation walk below the
 *     new top. Nothing else (no lateral cell, no block light) is written.
 *   MinecraftServer.doTick:340-350 -- per world: World.tick() (spawning,
 *     TickUpdates, random ticks), THEN `while (func_6156_d())` drains the
 *     whole queue (World.java:1269-1291), then updateEntities; packets are
 *     handled after every world (:351 handleNetworkListenThread).
 * So a write made outside the drain (a packet, an entity, a tile entity)
 * stays pending through the NEXT World.tick's spawning, scheduled and random
 * ticks, and is settled only by that tick's drain. Consumers:
 *   BlockCrops.updateTick:43 -- getBlockLightValue(x, y+1, z) >= 9 gates the
 *     growth nextInt draw (short-circuit: no draw when dark);
 *   BlockIce.updateTick:44 -- getSavedLightValue(Block) > 11 - 3 melts;
 *   EntityMob.getCanSpawnHere -- getBlockLightValue at the spawn cell.
 * Settled values after the drain equal production's synchronous flood (the
 * "post" checks are GREEN today and must stay GREEN). */

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
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h"
#include "util.h"

static struct server_local s;
static char s_dir[200];
static char s_dir1[220];
static int s_fail;
static int h0; /* superflat column height: the first open cell above ground */
static const int top = 100;

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static void lsetw(struct server_world* w, int x, int y, int z, int type) {
	assert(server_world_set_block(w, x, y, z,
								  (struct block_data) {.type = (uint8_t)type}));
}
static void set(int x, int y, int z, int type) {
	lsetw(&s.worlds[0], x, y, z, type);
}

static struct block_data lgetw(struct server_world* w, int x, int y, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(w, x, y, z, &b));
	return b;
}
static struct block_data get(int x, int y, int z) {
	return lgetw(&s.worlds[0], x, y, z);
}

/* "settled" mode (argv[1] == "settled") runs every case but counts only the
 * post-drain and production-equal checks: the GREEN half that must stay GREEN
 * while the pre-drain (".pre") checks are RED. */
static bool s_settled_only;
static void check(const char* id, bool ok, const char* what) {
	const bool pre = strstr(id, ".pre") != NULL;
	printf("  [%s] %s%s: %s\n", id, ok ? "PASS" : "FAIL",
		   pre && s_settled_only ? " (not counted: settled mode)" : "", what);
	if(!ok && !(pre && s_settled_only))
		s_fail++;
}

/* World.tick + func_6156_d for one world (MinecraftServer:346-348). No random
 * pass (anchored=false) so the case's own blocks are not randomly ticked. */
static void world_tick_and_drain(struct server_world* w) {
	(void)server_local_world_block_ticks_w(&s, w, false, 0, 0);
	server_world_light_drain(w);
}

static void make_world(struct server_world* w, const char* dir, int index,
					   enum world_dim dim) {
	MKDIR(dir);
	char region[260];
	snprintf(region, sizeof region, "%s/region", dir);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, dir);
	server_world_create(w, level, dim, 4242, true /* superflat */);
	string_clear(level);
	w->session = &s;
	w->local = &s;
	w->world_index = (uint8_t)index;
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
		}
}

static void fixture(const char* name, bool second_world) {
	char id[96];
	snprintf(id, sizeof id, "test_light_drain_order_golden_%s", name);
	test_fixture_dir(id, s_dir, sizeof s_dir);
	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	dict_entity_init(s.entities);
	make_world(&s.worlds[0], s_dir, 0, WORLD_DIM_OVERWORLD);
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = 0x11D7A1u;
	s.world_time = 6000; /* noon: skylightSubtracted 0 */
	server_local_skylight_recompute(&s);
	if(second_world) {
		snprintf(s_dir1, sizeof s_dir1, "%s_nether", s_dir);
		make_world(&s.worlds[1], s_dir1, 1, WORLD_DIM_NETHER);
	}
	uint8_t hh = 0;
	assert(server_world_get_height(&s.worlds[0], 8, 8, &hh));
	h0 = hh;
	assert(h0 > 0 && h0 < 90);
	assert(get(8, h0, 8).sky_light == 15 && get(8, top - 1, 8).sky_light == 15);
}

static void teardown(bool second_world) {
	dict_entity_clear(s.entities);
	server_world_discard(&s.worlds[0]);
	if(second_world) {
		server_world_discard(&s.worlds[1]);
		test_fixture_dir_cleanup(s_dir1);
	}
	test_fixture_dir_cleanup(s_dir);
}

/* An opaque roof outside the drain. func_339_g zeroes [h0, top) at once; the
 * lateral 14 waits for the drain. */
static void case_roof_place(void) {
	set(8, top, 8, BLOCK_STONE);
	int lo = get(8, h0, 8).sky_light, hi = get(8, top - 1, 8).sky_light;
	printf("  before the drain: sky(8,%d,8)=%d sky(8,%d,8)=%d (Beta 0, 0)\n",
		   h0, lo, top - 1, hi);
	check("roof_place.pre", lo == 0 && hi == 0,
		  "Chunk.func_339_g:178-180 zeroes [oldHeight, newHeight) at once");
	world_tick_and_drain(&s.worlds[0]);
	lo = get(8, h0, 8).sky_light;
	hi = get(8, top - 1, 8).sky_light;
	printf("  after World.tick + drain: %d/%d (Beta 14/14)\n", lo, hi);
	check("roof_place.post", lo == 14 && hi == 14,
		  "the drained skylight: lateral open sky 15 - 1");
}

/* Roof REMOVAL: a settled 5x5 roof (x,z 6..10 at y=top); the centre is
 * removed outside the drain. func_339_g's lowering branch sets the centre
 * column [h0, top+1) to 15 at once (:174-176); the lateral neighbour under the
 * remaining roof keeps its old 13 until the drain raises it to 14. */
static void case_roof_remove(void) {
	for(int x = 6; x <= 10; x++)
		for(int z = 6; z <= 10; z++)
			set(x, top, z, BLOCK_STONE);
	world_tick_and_drain(&s.worlds[0]);
	assert(get(8, top - 1, 8).sky_light == 12);
	assert(get(8, top - 1, 7).sky_light == 13);
	set(8, top, 8, BLOCK_AIR);
	int col = get(8, top - 1, 8).sky_light, low = get(8, h0, 8).sky_light;
	int side = get(8, top - 1, 7).sky_light;
	printf("  before the drain: column %d/%d (Beta 15/15), neighbour %d "
		   "(Beta 13)\n", col, low, side);
	check("roof_remove.column", col == 15 && low == 15,
		  "Chunk.func_339_g:174-176 lowering sets [newHeight, oldHeight) to 15");
	check("roof_remove.pre_neighbour", side == 13,
		  "the lateral cell under the remaining roof is only scheduled");
	world_tick_and_drain(&s.worlds[0]);
	side = get(8, top - 1, 7).sky_light;
	printf("  after the drain: neighbour %d (Beta 14)\n", side);
	check("roof_remove.post", side == 14, "drained: open column 15 - 1");
}

/* Torch placement across the x=15|16 chunk seam: Block light is only
 * scheduled (Chunk:232). */
static void case_torch_place(void) {
	set(15, h0, 4, BLOCK_TORCH);
	int c = get(15, h0, 4).torch_light, n = get(16, h0, 4).torch_light;
	printf("  before the drain: torch %d seam neighbour %d (Beta 0, 0)\n", c, n);
	check("torch_place.pre", c == 0 && n == 0,
		  "Chunk.setBlockIDWithMetadata:232 only schedules Block lighting");
	world_tick_and_drain(&s.worlds[0]);
	c = get(15, h0, 4).torch_light;
	n = get(16, h0, 4).torch_light;
	printf("  after the drain: %d/%d (Beta 14/13)\n", c, n);
	check("torch_place.post", c == 14 && n == 13, "drained torch 14, seam 13");
}

/* Torch REMOVAL across the seam: the old block light stays until the drain. */
static void case_torch_remove(void) {
	set(15, h0, 4, BLOCK_TORCH);
	world_tick_and_drain(&s.worlds[0]);
	assert(get(15, h0, 4).torch_light == 14 && get(16, h0, 4).torch_light == 13);
	set(15, h0, 4, BLOCK_AIR);
	int c = get(15, h0, 4).torch_light, n = get(16, h0, 4).torch_light;
	printf("  before the drain: %d/%d (Beta 14/13)\n", c, n);
	check("torch_remove.pre", c == 14 && n == 13,
		  "removal schedules Block lighting; the stored 14/13 remain");
	world_tick_and_drain(&s.worlds[0]);
	c = get(15, h0, 4).torch_light;
	n = get(16, h0, 4).torch_light;
	printf("  after the drain: %d/%d (Beta 0/0)\n", c, n);
	check("torch_remove.post", c == 0 && n == 0, "drained: dark");
}

/* Several writes before one drain: place, remove, place again. The Beta trace
 * at the torch cell is 0,0,0 (nothing drains in between), then 14. */
static void case_multi_write(void) {
	int t[4];
	set(15, h0, 4, BLOCK_TORCH);
	t[0] = get(15, h0, 4).torch_light;
	set(15, h0, 4, BLOCK_AIR);
	t[1] = get(15, h0, 4).torch_light;
	set(15, h0, 4, BLOCK_TORCH);
	t[2] = get(15, h0, 4).torch_light;
	world_tick_and_drain(&s.worlds[0]);
	t[3] = get(15, h0, 4).torch_light;
	printf("  trace %d,%d,%d | drained %d (Beta 0,0,0 | 14)\n", t[0], t[1],
		   t[2], t[3]);
	check("multi_write.pre", t[0] == 0 && t[1] == 0 && t[2] == 0,
		  "no write in the batch is lit before the drain");
	check("multi_write.post", t[3] == 14, "drained: the final torch is lit");
}

static void random_tick(int type, int x, int y, int z) {
	struct block_data b = get(x, y, z);
	assert(b.type == type && blocks[type] && blocks[type]->onRandomTick);
	struct block_info info = {.block = &b, .x = x, .y = y, .z = z};
	struct server_world* prev = block_world_enter(&s, &s.worlds[0]);
	blocks[type]->onRandomTick(&s, &info);
	block_world_leave(&s, prev);
}

/* Crop growth gate + RNG: a sky-exposed crop at night (skylightSubtracted 11
 * -> sky 4 above it) beside a torch placed outside the drain. The Beta
 * getBlockLightValue above the crop is max(4, 0) < 9, so updateTick returns
 * before nextInt (no World.rand draw); after the drain it is 12 >= 9 and the
 * draw happens. */
static void case_crop_rng(void) {
	s.world_time = 18000;
	server_local_skylight_recompute(&s);
	set(4, h0 - 1, 4, BLOCK_FARMLAND);
	set(4, h0, 4, BLOCK_CROPS);
	world_tick_and_drain(&s.worlds[0]);
	set(5, h0, 4, BLOCK_TORCH); /* the packet-phase write */
	int above = server_local_block_light_value(&s, 4, h0 + 1, 4);
	uint32_t r0 = s.rand_src.seed;
	random_tick(BLOCK_CROPS, 4, h0, 4);
	bool drew = s.rand_src.seed != r0;
	printf("  before the drain: light above %d (Beta 4), drew %d (Beta 0), "
		   "crop %d meta %d\n", above, drew, get(4, h0, 4).type,
		   get(4, h0, 4).metadata);
	check("crop_rng.pre", above == 4 && !drew && get(4, h0, 4).type == BLOCK_CROPS
			  && get(4, h0, 4).metadata == 0,
		  "BlockCrops.updateTick:43 light gate fails -> no nextInt draw");
	world_tick_and_drain(&s.worlds[0]);
	above = server_local_block_light_value(&s, 4, h0 + 1, 4);
	r0 = s.rand_src.seed;
	random_tick(BLOCK_CROPS, 4, h0, 4);
	drew = s.rand_src.seed != r0;
	printf("  after the drain: light above %d (Beta 12), drew %d (Beta 1)\n",
		   above, drew);
	check("crop_rng.post", above == 12 && drew, "drained: the gate passes");
}

/* Ice melt: BlockIce.updateTick:44 reads the SAVED block light (> 8 melts).
 * A torch beside the ice outside the drain leaves it at 0 (no melt); after
 * the drain it is 14 - 3 = 11 and the ice melts to still water. */
static void case_ice_melt(void) {
	set(10, h0, 10, BLOCK_ICE);
	world_tick_and_drain(&s.worlds[0]);
	set(11, h0, 10, BLOCK_TORCH);
	int bl = get(10, h0, 10).torch_light;
	random_tick(BLOCK_ICE, 10, h0, 10);
	int after = get(10, h0, 10).type;
	printf("  before the drain: ice block light %d (Beta 0), block %d (Beta %d "
		   "ice)\n", bl, after, BLOCK_ICE);
	check("ice_melt.pre", bl == 0 && after == BLOCK_ICE,
		  "BlockIce.updateTick:44 reads the pending (unlit) saved light");
	if(after != BLOCK_ICE)
		set(10, h0, 10, BLOCK_ICE);
	world_tick_and_drain(&s.worlds[0]);
	bl = get(10, h0, 10).torch_light;
	random_tick(BLOCK_ICE, 10, h0, 10);
	after = get(10, h0, 10).type;
	printf("  after the drain: %d (Beta 11), block %d (Beta %d water)\n", bl,
		   after, BLOCK_WATER_STILL);
	check("ice_melt.post", bl == 11 && after == BLOCK_WATER_STILL,
		  "drained: 11 > 8 melts");
}

/* Mob spawn light: the hostile gate reads getBlockLightValue at the cell;
 * under a roof placed outside the drain Beta reads the func_339_g column 0. */
static void case_spawn_light(void) {
	set(8, top, 8, BLOCK_STONE);
	int l = server_local_hostile_spawn_light(&s, 8, h0, 8);
	printf("  before the drain: spawn light %d (Beta 0)\n", l);
	check("spawn_light.pre", l == 0,
		  "EntityMob.getCanSpawnHere reads the zeroed column");
	world_tick_and_drain(&s.worlds[0]);
	l = server_local_hostile_spawn_light(&s, 8, h0, 8);
	printf("  after the drain: %d (Beta 14)\n", l);
	check("spawn_light.post", l == 14, "drained: lateral 14");
}

/* Two worlds: each World owns its queue and drains only in its own doTick
 * slot (MinecraftServer:342-348). A torch in the Nether (slot 1) stays unlit
 * through the Overworld tick and drain, and is settled by the Nether drain. */
static void case_two_worlds(void) {
	struct server_world* n = &s.worlds[1];
	const int ny = 64;
	lsetw(n, 4, ny, 4, BLOCK_STONE);
	(void)server_world_set_block(n, 4, ny + 1, 4, (struct block_data) {0});
	world_tick_and_drain(n);
	lsetw(n, 4, ny + 1, 4, BLOCK_TORCH);
	set(15, h0, 4, BLOCK_TORCH);
	int a = lgetw(n, 4, ny + 1, 4).torch_light;
	world_tick_and_drain(&s.worlds[0]);
	int b = lgetw(n, 4, ny + 1, 4).torch_light;
	int o = get(15, h0, 4).torch_light;
	world_tick_and_drain(n);
	int c = lgetw(n, 4, ny + 1, 4).torch_light;
	printf("  nether torch: written %d, after the Overworld drain %d, after the "
		   "Nether drain %d (Beta 0, 0, 14); overworld torch after its drain %d "
		   "(Beta 14)\n", a, b, c, o);
	check("two_worlds.pre", a == 0 && b == 0,
		  "the Nether pending light is not drained by the Overworld");
	check("two_worlds.post", c == 14 && o == 14,
		  "each world drain settles its own writes");
}

struct test_case {
	const char* name;
	void (*run)(void);
	bool second_world;
};
static const struct test_case cases[] = {
	{"roof_place", case_roof_place, false},
	{"roof_remove", case_roof_remove, false},
	{"torch_place", case_torch_place, false},
	{"torch_remove", case_torch_remove, false},
	{"multi_write", case_multi_write, false},
	{"crop_rng", case_crop_rng, false},
	{"ice_melt", case_ice_melt, false},
	{"spawn_light", case_spawn_light, false},
	{"two_worlds", case_two_worlds, true},
};

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	int ran = 0;
	s_settled_only = argc > 1 && strcmp(argv[1], "settled") == 0;
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		if(argc > 1 && !s_settled_only && strcmp(argv[1], cases[i].name) != 0)
			continue;
		printf("G9 case %s\n", cases[i].name);
		fixture(cases[i].name, cases[i].second_world);
		cases[i].run();
		teardown(cases[i].second_world);
		ran++;
	}
	assert(ran > 0 && "unknown case");
	if(s_fail) {
		printf("test_light_drain_order_golden: %d FAIL\n", s_fail);
		return 1;
	}
	printf("test_light_drain_order_golden: PASS\n");
	return 0;
}
