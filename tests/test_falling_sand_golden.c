/*
	Batch I: Beta 1.7.3 falling sand/gravel golden (audit register
	worldgen-saves #1, the largest confirmed parity gap -- sand/gravel never
	fell). End-to-end env harness (server_local + real world ticking),
	modeled on tests/test_creeper_charged_explosion.c's and
	tests/test_tnt_primed_env.c's shape.

	Ground truth (decompile 740c583):
	  BlockSand.onBlockAdded/onNeighborBlockChange -> scheduleBlockUpdate(...,
	    tickRate()==3);
	  BlockSand.updateTick -> tryToFall: if canFallBelow(below) && y>=0, spawn
	    EntityFallingSand (when the 32-radius area is loaded) else settle
	    directly;
	  EntityFallingSand.onUpdate: gravity/move/drag; clears the origin cell to
	    air on its own first tick; on landing, re-places the carried block
	    UNLESS canBlockBePlacedAt fails (water/lava/fire landing cells) or the
	    landing cell itself has no support below, in which case it drops as an
	    item instead; fallTime>100 with no landing also drops+destroys.

	World layout: superflat (bedrock y0, dirt y1-2, grass y3, air y>=4) --
	see worldgen.c's own superflat branch. "Ground" for every column here is
	therefore y=3 (grass), and y=4 is the first open cell above it.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
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
#include "entity/entity_falling_sand.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_falling_sand_golden_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static void tick_entities(void) {
	assert(server_local_test_tick_entities(&s));
}

// One logical update: entities first, then the world/scheduled-block-tick
// pass -- the exact order tests/test_tnt_primed_env.c's own per-update loop
// uses.
static void step(void) {
	server_world_begin_update(&s.world);
	tick_entities();
	server_world_tick(&s.world, &s);
}

static size_t count_entities_of_type(enum entity_type type) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		if(dict_entity_ref(it)->value->type == type)
			n++;
		dict_entity_next(it);
	}
	return n;
}

static struct entity* find_entity_of_type(enum entity_type type) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e->type == type)
			return e;
		dict_entity_next(it);
	}
	return NULL;
}

static void clear_all_entities(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			uint32_t id = dict_entity_ref(it)->key;
			dict_entity_erase(s.entities, id);
			again = true;
			break;
		}
	}
}

static bool g_saw_spawn_falling_sand;
static uint8_t g_spawn_block_id;
static bool g_saw_spawn_item;
static struct item_data g_spawn_item;
static size_t g_max_concurrent_falling_sand;

static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SPAWN_FALLING_SAND) {
		g_saw_spawn_falling_sand = true;
		g_spawn_block_id = call->payload.spawn_falling_sand.block_id;
	} else if(call->type == CRPC_SPAWN_ITEM) {
		g_saw_spawn_item = true;
		g_spawn_item = call->payload.spawn_item.item;
	}
}

static void reset_observers(void) {
	g_saw_spawn_falling_sand = false;
	g_spawn_block_id = 0;
	g_saw_spawn_item = false;
	memset(&g_spawn_item, 0, sizeof(g_spawn_item));
}

// (1) Scheduled-tick timing: BlockSand.tickRate()==3. Placing sand over open
// air must NOT spawn an entity on ticks 1-2, and MUST spawn exactly one on
// tick 3 -- the source cell itself stays unchanged until the entity's own
// first update (a separate, later pass).
static void test_schedule_and_three_tick_spawn(void) {
	clear_all_entities();
	reset_observers();
	clin_test_set_rpc_observer(observe, NULL);

	w_coord_t x = 8, y = 10, z = 8;
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_SAND}));
	assert(count_entities_of_type(ENTITY_FALLING_SAND) == 0);

	step(); // world tick 1: fire_at_tick (3) not yet reached
	assert(count_entities_of_type(ENTITY_FALLING_SAND) == 0);
	assert(!g_saw_spawn_falling_sand);

	step(); // world tick 2: still not reached
	assert(count_entities_of_type(ENTITY_FALLING_SAND) == 0);
	assert(!g_saw_spawn_falling_sand);

	step(); // world tick 3: BlockSand.tickRate() fires
	assert(count_entities_of_type(ENTITY_FALLING_SAND) == 1
		   && "the scheduled tick must fire on EXACTLY tick 3, not before");
	assert(g_saw_spawn_falling_sand);
	assert(g_spawn_block_id == BLOCK_SAND);

	// tryToFall spawns the entity but does not itself clear the source cell
	// (that happens on the entity's own first onUpdate, a later pass) --
	// [decompile 740c583 BlockSand.tryToFall].
	struct block_data still_there;
	assert(server_world_get_block(&s.world, x, y, z, &still_there));
	assert(still_there.type == BLOCK_SAND
		   && "tryToFall itself must not clear the source cell");

	clin_test_set_rpc_observer(NULL, NULL);
	clear_all_entities();
	server_world_set_block(&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_AIR});
}

// (2) Landing converts back to a block at the right cell, and the origin
// cell is cleared once the entity starts moving.
static void test_landing_places_block_and_clears_origin(void) {
	clear_all_entities();
	w_coord_t x = 8, y = 10, z = 8;
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_SAND}));

	bool landed = false;
	for(int i = 0; i < 200 && !landed; i++) {
		step();
		if(count_entities_of_type(ENTITY_FALLING_SAND) == 0 && i > 2)
			landed = true;
	}
	assert(landed && "the faller must land within a generous tick budget");

	struct block_data origin, rest;
	assert(server_world_get_block(&s.world, x, y, z, &origin));
	assert(origin.type == BLOCK_AIR
		   && "the origin cell must be cleared once the entity is airborne");
	// Falls onto grass (top solid surface at y=3), so it settles at y=4.
	assert(server_world_get_block(&s.world, x, 4, z, &rest));
	assert(rest.type == BLOCK_SAND
		   && "the faller must re-place its carried block at the landing cell");

	clear_all_entities();
	server_world_set_block(&s.worlds[0], x, 4, z, (struct block_data) {.type = BLOCK_AIR});
}

// (5) Gravel shares BlockSand's mechanic unchanged (BlockGravel extends
// BlockSand in Beta) -- same landing behaviour, different carried id.
static void test_gravel_behaves_identically(void) {
	clear_all_entities();
	w_coord_t x = 9, y = 10, z = 8;
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_GRAVEL}));

	bool landed = false;
	for(int i = 0; i < 200 && !landed; i++) {
		step();
		if(count_entities_of_type(ENTITY_FALLING_SAND) == 0 && i > 2)
			landed = true;
	}
	assert(landed);

	struct block_data rest;
	assert(server_world_get_block(&s.world, x, 4, z, &rest));
	assert(rest.type == BLOCK_GRAVEL);

	clear_all_entities();
	server_world_set_block(&s.worlds[0], x, 4, z, (struct block_data) {.type = BLOCK_AIR});
}

// (3) Unsupported-column cascade: three stacked sand blocks over open air
// must cascade bottom-up: the bottom block's OWN scheduled tick is the only
// one that finds real support missing on the FIRST pass (its neighbours are
// still solid sand, so they no-op); only once the bottom block's falling
// entity clears its origin cell (on that entity's own first tick, one step
// AFTER it spawns -- not once it LANDS) does the block above get woken via
// onNeighbourBlockChange and start falling too. Concurrent fallers are
// therefore expected and faithful (Beta's own well-known "the whole column
// peels off and chases itself down" visual) -- what must never happen is
// MORE simultaneous fallers than there are blocks in the column, or a dirty
// final stack (gaps/duplicates/wrong order).
static void test_unsupported_column_cascade_order(void) {
	clear_all_entities();
	w_coord_t x = -8, z = 8;
	assert(server_world_set_block(
		&s.worlds[0], x, 10, z, (struct block_data) {.type = BLOCK_SAND}));
	assert(server_world_set_block(
		&s.worlds[0], x, 11, z, (struct block_data) {.type = BLOCK_SAND}));
	assert(server_world_set_block(
		&s.worlds[0], x, 12, z, (struct block_data) {.type = BLOCK_SAND}));

	g_max_concurrent_falling_sand = 0;
	for(int i = 0; i < 400; i++) {
		step();
		size_t n = count_entities_of_type(ENTITY_FALLING_SAND);
		if(n > g_max_concurrent_falling_sand)
			g_max_concurrent_falling_sand = n;
		assert(n <= 3
			   && "only 3 sand blocks exist in this column -- more than 3 "
				  "simultaneous fallers would mean a duplicate/phantom spawn");
	}
	assert(count_entities_of_type(ENTITY_FALLING_SAND) == 0
		   && "the whole column must have finished settling");

	struct block_data b4, b5, b6, b10, b11, b12;
	assert(server_world_get_block(&s.world, x, 4, z, &b4));
	assert(server_world_get_block(&s.world, x, 5, z, &b5));
	assert(server_world_get_block(&s.world, x, 6, z, &b6));
	assert(server_world_get_block(&s.world, x, 10, z, &b10));
	assert(server_world_get_block(&s.world, x, 11, z, &b11));
	assert(server_world_get_block(&s.world, x, 12, z, &b12));
	assert(b4.type == BLOCK_SAND && b5.type == BLOCK_SAND
		   && b6.type == BLOCK_SAND
		   && "the column must re-stack cleanly on the grass floor");
	assert(b10.type == BLOCK_AIR && b11.type == BLOCK_AIR
		   && b12.type == BLOCK_AIR);

	clear_all_entities();
	for(w_coord_t yy = 4; yy <= 12; yy++)
		server_world_set_block(&s.worlds[0], x, yy, z,
							   (struct block_data) {.type = BLOCK_AIR});
}

// (4) Water/lava fall-through + cannot-place -> item drop: a faller passes
// straight through a fluid cell (no collision box -- block_water.c/
// block_lava.c getBoundingBox return 0) and lands with its floor position
// INSIDE that fluid cell; canBlockBePlacedAt then fails for water/lava (see
// block_sand.h's derivation), so it drops as a raw-id item instead of
// placing, and the fluid cell is left untouched.
static void test_fluid_landing_drops_item(uint8_t fluid_block, w_coord_t x,
										  w_coord_t z) {
	clear_all_entities();
	reset_observers();
	clin_test_set_rpc_observer(observe, NULL);

	assert(server_world_set_block(
		&s.worlds[0], x, 4, z, (struct block_data) {.type = fluid_block}));
	assert(server_world_set_block(
		&s.worlds[0], x, 15, z, (struct block_data) {.type = BLOCK_SAND}));

	bool landed = false;
	for(int i = 0; i < 200 && !landed; i++) {
		step();
		if(count_entities_of_type(ENTITY_FALLING_SAND) == 0 && i > 2)
			landed = true;
	}
	assert(landed);

	assert(g_saw_spawn_item && "a fluid landing must drop an item, never silently vanish");
	assert(g_spawn_item.id == BLOCK_SAND && g_spawn_item.count == 1);

	struct block_data at_fluid, at_origin;
	assert(server_world_get_block(&s.world, x, 4, z, &at_fluid));
	assert(at_fluid.type == fluid_block
		   && "landing IN a fluid must never overwrite it with the block");
	assert(server_world_get_block(&s.world, x, 15, z, &at_origin));
	assert(at_origin.type == BLOCK_AIR);

	clin_test_set_rpc_observer(NULL, NULL);
	clear_all_entities();
	server_world_set_block(&s.worlds[0], x, 4, z, (struct block_data) {.type = BLOCK_AIR});
}

// (8) Negative control: sand resting directly on solid ground must never
// fall, no matter how many ticks pass -- CODESTYLE 4.3.
static void test_negative_control_solid_ground_never_falls(void) {
	clear_all_entities();
	reset_observers();
	clin_test_set_rpc_observer(observe, NULL);

	w_coord_t x = 8, y = 4, z = -8; // resting on grass at y=3
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_SAND}));

	for(int i = 0; i < 30; i++)
		step();

	assert(!g_saw_spawn_falling_sand
		   && "sand on solid ground must never spawn a falling entity");
	assert(count_entities_of_type(ENTITY_FALLING_SAND) == 0);
	struct block_data still;
	assert(server_world_get_block(&s.world, x, y, z, &still));
	assert(still.type == BLOCK_SAND);

	clin_test_set_rpc_observer(NULL, NULL);
	clear_all_entities();
	server_world_set_block(&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_AIR});
}

// (7) Save/load round trip of a mid-air faller: gather -> NBT build -> NBT
// parse -> restore, the full chunk_entities_build/chunk_entities_parse path
// (not just the in-memory gather/restore shortcut), so the "Tile" tag wiring
// in entity_archive.c is genuinely exercised.
static void test_save_load_roundtrip_mid_air(void) {
	clear_all_entities();
	vec3 pos = {8.5f, 50.5f, 8.5f};
	struct entity* faller
		= server_local_spawn_falling_sand(&s, pos, BLOCK_GRAVEL);
	assert(faller);
	// A couple of ticks so velocity/fall_time are nonzero -- proves those
	// fields round-trip through position/velocity (the common block), while
	// fall_time itself is expected to reset (Beta persists no such tag).
	tick_entities();
	tick_entities();
	assert(faller->data.falling_sand.fall_time > 0);
	float vy_before = faller->vel[1];
	assert(vy_before < 0.0f);

	struct chunk_entities gathered = {0};
	assert(server_local_gather_chunk_entities(&s, 0, 0, &gathered));
	bool found = false;
	for(size_t i = 0; i < gathered.count; i++)
		if(gathered.entities[i].kind == ARCHIVED_ENTITY_FALLING_SAND
		   && gathered.entities[i].data.falling_sand.block_id
				  == BLOCK_GRAVEL)
			found = true;
	assert(found);

	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&gathered, &arena);
	assert(list && arena);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	chunk_entities_build_free(arena);
	chunk_entities_free(&gathered);

	clear_all_entities();
	assert(server_local_restore_chunk_entities(&s, &parsed));
	chunk_entities_free(&parsed);

	struct entity* restored = find_entity_of_type(ENTITY_FALLING_SAND);
	assert(restored);
	assert(restored->data.falling_sand.block_id == BLOCK_GRAVEL);
	assert(fabsf(restored->pos[0] - pos[0]) < 0.01f);
	assert(fabsf(restored->pos[2] - pos[2]) < 0.01f);
	// Beta persists no fallTime-equivalent tag; a restored faller starts its
	// air-timer fresh (matches entity_falling_sand()'s constructor default).
	assert(restored->data.falling_sand.fall_time == 0);

	clear_all_entities();
}

int main(void) {
	test_fixture_dir("test_falling_sand_golden_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.rand_src.seed = 0x5A4DB007u;
	s.scheduled_tick_budget = 0; // unlimited: sand is never a fluid entry

	// Load a 7x7 chunk grid around the origin so BlockSand.tryToFall's own
	// 32-radius checkChunksExist passes for every column this file uses.
	// Margin note: a column at block z=-9 needs chunks down to floor((-9-32)
	// /16) = -3, not -2 -- a radius-32 check is NOT symmetric around a small
	// coordinate's OWN chunk the way it looks at first glance, so this grid
	// is one ring wider than the naive "+/-9 blocks needs +/-1 chunk" guess.
	for(w_coord_t cz = -3; cz <= 3; cz++)
		for(w_coord_t cx = -3; cx <= 3; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	test_schedule_and_three_tick_spawn();
	test_landing_places_block_and_clears_origin();
	test_gravel_behaves_identically();
	test_unsupported_column_cascade_order();
	test_fluid_landing_drops_item(BLOCK_WATER_STILL, 8, 9);
	test_fluid_landing_drops_item(BLOCK_LAVA_STILL, 8, -9);
	test_negative_control_solid_ground_never_falls();
	test_save_load_roundtrip_mid_air();

	printf("test_falling_sand_golden: BlockSand.tryToFall (3-tick schedule, "
		   "landing/clear/cascade order, water+lava->item drop, gravel "
		   "parity, solid-ground negative control, mid-air save/load round "
		   "trip) all PASS -- max concurrent column fallers observed: %zu\n",
		   g_max_concurrent_falling_sand);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
