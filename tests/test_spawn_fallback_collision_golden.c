/*
	next-gaps-0924 W3 SIM s4: the world-spawn fallback's push-up uses Beta's
	collision boxes on populated terrain.

	Every expected value below is derived by hand from the decompile
	(mcb173-retromcp/minecraft/src_original/net/minecraft/), never read back
	from CaveWii:

	  client/Minecraft.java:1418-1453  respawn with no usable bed: createPlayer
	      -> EntityPlayer ctor at the world spawn column (x+0.5, z+0.5), then
	      thePlayer.preparePlayerToSpawn()
	  src/EntityPlayer.java:150-153    preparePlayerToSpawn: yOffset = 1.62F,
	      setSize(0.6F, 1.8F), super.preparePlayerToSpawn()
	  src/Entity.java:96-110           while(posY > 0) { setPosition(...);
	      if(getCollidingBoundingBoxes(this, boundingBox).size() == 0) break;
	      ++posY; }
	  src/World.java:951-971           the box test: every block collision box
	      in the cell walk (feet-1 .. head), strict intersection
	  src/BlockFlower.java:43 (tall grass extends it), BlockFluid.java:70,
	  BlockWeb.java:18                  collision box null -> never blocks
	  src/BlockSnow.java:12-14          layer meta&7 < 3 -> null; >= 3 -> box
	                                    to y + 0.5F
	  src/BlockFence.java:12-13         box to y + 1.5F (reaches the cell above)
	  src/BlockStep.java (slab)         half-height box -> blocks its own cell

	CaveWii keeps the world SpawnY as the FEET cell (worldgen_find_spawn's
	surface+1), so the scan starts at spawn_y; the cases below all put the
	spawn feet cell at y = 90 over a stone floor at 89 unless stated.

	Also pinned: the live fallback POPULATES the spawn chunk before scanning
	(the first-join rule, W1A-b) -- a never-loaded spawn chunk is decorated,
	not scanned as raw terrain.
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
#include "block/piston_moving.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "platform/watchdog.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define FEET 90

static struct server_local s;
static volatile int g_drain_run = 1;
static int g_failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

#define CHECK(cond, ...)                                                   \
	do {                                                                   \
		if(!(cond)) {                                                      \
			g_failures++;                                                  \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
			printf(__VA_ARGS__);                                           \
			printf("\n");                                                  \
		}                                                                  \
	} while(0)

static void set(int x, int y, int z, int type, int meta) {
	(void)server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)meta});
	struct block_data got;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &got)
		   && got.type == type);
}

/* Die anywhere, respawn with no bed; return the fresh body's EYE y. The plan
 * carries feet + PLAYER_EYE_HEIGHT as int + float (the arrival formula), so
 * callers compare against that exact expression, never a subtraction. */
static double respawn_eye(int sx, int sy, int sz) {
	s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	s.player.active_inventory = &s.player.inventory;
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++)
		inventory_clear_slot(&s.player.inventory, slot);
	inventory_clear_picked_item(&s.player.inventory);
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 8.5;
	s.player.y = 200.0; /* wherever they died */
	s.player.z = 8.5;
	s.player.has_death_pos = false;
	s.player.spawn_x = sx;
	s.player.spawn_y = sy;
	s.player.spawn_z = sz;
	s.player.has_bed_spawn = false;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	CHECK(s.player.x == sx + 0.5 && s.player.z == sz + 0.5,
		  "column (%d,%d): XZ drifted to (%f,%f)", sx, sz, s.player.x,
		  s.player.z);
	double eye = s.player.y;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	return eye;
}

struct column_case {
	const char* name;
	int floor_type;          /* block at FEET-1 */
	int feet_type, feet_meta; /* block at FEET */
	int head_type;           /* block at FEET+1 */
	int above_type;          /* block at FEET+2 */
	int beta_feet;           /* hand-evaluated Beta result */
};

/* Beta, per case (box = [x+0.2,x+0.8] x [feet, feet+1.8]):
 *  tall grass/flower/water/web/snow<3: no box anywhere -> stays at 90
 *  fence under the feet: fence box tops at 89+1.5 = 90.5 > 90 -> blocked;
 *      at 91 the box starts at 91 > 90.5 -> clear -> 91
 *  snow meta 3: box to 90.5 -> blocked; 91 clear -> 91
 *  slab at 90: box to 90.5 -> 91
 *  stone at 90, 91 (two solid): 90, 91 blocked (91's box [91,92] hits 91..92.8);
 *      92 clear -> 92
 *  water at feet and head: fluids have no box -> 90 */
static const struct column_case CASES[] = {
	{"tall grass at feet", BLOCK_GRASS, BLOCK_TALL_GRASS, 1, BLOCK_AIR,
	 BLOCK_AIR, 90},
	{"flower at feet", BLOCK_GRASS, BLOCK_FLOWER, 0, BLOCK_AIR, BLOCK_AIR, 90},
	{"still water at feet+head", BLOCK_STONE, BLOCK_WATER_STILL, 0,
	 BLOCK_WATER_STILL, BLOCK_AIR, 90},
	{"cobweb at head", BLOCK_STONE, BLOCK_AIR, 0, BLOCK_COBWEB, BLOCK_AIR, 90},
	{"snow layer meta 0", BLOCK_STONE, BLOCK_SNOW, 0, BLOCK_AIR, BLOCK_AIR, 90},
	{"fence under the feet", BLOCK_FENCE, BLOCK_AIR, 0, BLOCK_AIR, BLOCK_AIR,
	 91},
	{"snow layer meta 3", BLOCK_STONE, BLOCK_SNOW, 3, BLOCK_AIR, BLOCK_AIR, 91},
	{"slab at feet", BLOCK_STONE, BLOCK_SLAB, 0, BLOCK_AIR, BLOCK_AIR, 91},
	{"two stone", BLOCK_STONE, BLOCK_STONE, 0, BLOCK_STONE, BLOCK_AIR, 92},
	{"clear column", BLOCK_STONE, BLOCK_AIR, 0, BLOCK_AIR, BLOCK_AIR, 90},
};

static void test_columns(void) {
	for(size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
		const struct column_case* c = &CASES[i];
		const int sx = 2 + 2 * (int)i, sz = 6; /* one column per case, chunk 0/1 */
		set(sx, FEET - 1, sz, c->floor_type, 0);
		set(sx, FEET, sz, c->feet_type, c->feet_meta);
		set(sx, FEET + 1, sz, c->head_type, 0);
		set(sx, FEET + 2, sz, c->above_type, 0);
		for(int y = FEET + 3; y <= FEET + 6; y++)
			set(sx, y, sz, BLOCK_AIR, 0);
		double eye = respawn_eye(sx, FEET, sz);
		CHECK(eye == c->beta_feet + PLAYER_EYE_HEIGHT,
			  "%s: feet %.4f, Beta %d", c->name,
			  eye - (double)PLAYER_EYE_HEIGHT, c->beta_feet);
	}
	puts("  columns: no-box blocks keep the spawn cell, fence/slab/snow>=3/"
		 "stone push up (Beta getCollidingBoundingBoxes)");
}

/* The fallback populates a never-loaded spawn chunk before it scans it. */
static void test_populates_spawn_chunk(void) {
	const w_coord_t cx = 6, cz = 6; /* never touched by the fixture */
	assert(!server_world_is_chunk_loaded(&s.worlds[0], cx, cz));
	const int sx = cx * CHUNK_SIZE + 5, sz = cz * CHUNK_SIZE + 5;
	/* W3-s6: the populate is stamped WDS_ENSURE_POPULATED and the caller's
	 * phase is handed back afterwards. */
	watchdog_phase_srv(WDS_RPC);
	double eye = respawn_eye(sx, 4, sz);
	CHECK(watchdog_phase_srv_current() == WDS_RPC,
		  "respawn populate must restore the caller's watchdog phase (%u)",
		  (unsigned)watchdog_phase_srv_current());
	/* Superflat: grass at 3, air above -> the feet cell 4 is clear. */
	CHECK(eye == 4 + PLAYER_EYE_HEIGHT, "far spawn: feet %.4f, want 4",
		  eye - (double)PLAYER_EYE_HEIGHT);
	struct server_chunk* sc
		= dict_server_chunks_get(s.worlds[0].chunks, S_CHUNK_ID(cx, cz));
	CHECK(sc && sc->populated,
		  "the world-spawn fallback must populate the spawn chunk before "
		  "scanning it (loaded %d, populated %d)",
		  sc != NULL, sc ? (int)sc->populated : -1);
	puts("  population: a never-loaded spawn chunk is decorated before the scan");
}

int main(void) {
	test_fixture_dir("test_spawn_fallback_collision_golden_tmp",
					 s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	puts("test_spawn_fallback_collision_golden: Beta preparePlayerToSpawn "
		 "box push-up (next-gaps-0924 W3 s4)");
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x5A4F0924u;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -1; cx <= 2; cx++)
		for(w_coord_t cz = -1; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	test_columns();
	test_populates_spawn_chunk();

	inventory_destroy(&s.player.inventory, &s.player);
	g_drain_run = 0;
	thread_join(&drain);
	if(g_failures) {
		printf("test_spawn_fallback_collision_golden: FAIL (%d mismatches)\n",
			   g_failures);
		return 1;
	}
	puts("test_spawn_fallback_collision_golden: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
