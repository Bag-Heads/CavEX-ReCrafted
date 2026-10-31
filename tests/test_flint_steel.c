/*
	Golden test for Beta 1.7.3 flint & steel (ItemFlintAndSteel.onItemUse): light fire in the
	cell adjacent to the clicked face when that cell is air. Beta has NO flammable-block gate
	(the prior CaveWii code wrongly required the clicked block to be flammable) -- you can light
	a transient fire on any face; the fire block's tick extinguishes a fuel-less one.

	Pins: fire placed on a NON-flammable (stone) face; the per-side adjacent-cell mapping; and
	the no-op when the adjacent cell is not air.

	Decompile: 740c583:1.7.3-LTS ItemFlintAndSteel.onItemUse.
*/

#include <assert.h>
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
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_flint_steel_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct item item_flint_steel;

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

static void set(int x, int y, int z, int type) {
	struct block_data expected = {
		.type = (uint8_t)type,
		.metadata = 0,
	};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	/* Beta returns false for an identical id+metadata. Fixture setup accepts
	 * only that exact no-op; an unavailable chunk or wrong live state still
	 * fails the test. */
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	return blk.type;
}

static void flint_use_stack(struct item_data* flint, int x, int y, int z,
							enum side side) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	struct block_info on = {
		.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
	assert(item_flint_steel.onUseOnBlock);
	assert(item_flint_steel.onUseOnBlock(&s, &s.player, flint, &on, side));
}

// Right-click the (x,y,z) face `side` with a fresh flint & steel.
static void flint_use(int x, int y, int z, enum side side) {
	struct item_data flint = {
		.id = ITEM_FLINT_STEEL, .durability = 0, .count = 1};
	flint_use_stack(&flint, x, y, z, side);
	assert(flint.id == ITEM_FLINT_STEEL && flint.count == 1
		   && flint.durability == 1);
}

static void test_fire_on_non_flammable_face(void) {
	// stone is NOT flammable -- Beta still lights fire above it (the fix). Click the top face.
	set(8, 70, 8, BLOCK_STONE);
	set(8, 71, 8, BLOCK_AIR);
	flint_use(8, 70, 8, SIDE_TOP);
	assert(type_at(8, 71, 8) == BLOCK_FIRE);
	printf("  fire lit above a stone (non-flammable) face: OK\n");
}

static void test_side_mapping(void) {
	// +X face (SIDE_RIGHT) -> fire at x+1
	set(8, 70, 10, BLOCK_STONE);
	set(9, 70, 10, BLOCK_AIR);
	flint_use(8, 70, 10, SIDE_RIGHT);
	assert(type_at(9, 70, 10) == BLOCK_FIRE);

	// SIDE_BACK -> fire at z+1 and SIDE_FRONT -> fire at z-1 (canonical blocks_side_offset;
	// these two were SWAPPED in the old hand-rolled switch -- this pins the fix).
	set(8, 70, 12, BLOCK_STONE);
	set(8, 70, 13, BLOCK_AIR);
	flint_use(8, 70, 12, SIDE_BACK);
	assert(type_at(8, 70, 13) == BLOCK_FIRE);

	set(8, 70, 20, BLOCK_STONE);
	set(8, 70, 19, BLOCK_AIR);
	flint_use(8, 70, 20, SIDE_FRONT);
	assert(type_at(8, 70, 19) == BLOCK_FIRE);

	// -Y face (SIDE_BOTTOM) -> fire at y-1
	set(8, 70, 16, BLOCK_STONE);
	set(8, 69, 16, BLOCK_AIR);
	flint_use(8, 70, 16, SIDE_BOTTOM);
	assert(type_at(8, 69, 16) == BLOCK_FIRE);
	printf("  per-side adjacent-cell mapping (+X / BACK=+Z / FRONT=-Z / -Y): OK\n");
}

static void test_no_fire_if_adjacent_not_air(void) {
	set(8, 70, 14, BLOCK_STONE);
	set(8, 71, 14, BLOCK_DIRT); // the cell above is not air
	flint_use(8, 70, 14, SIDE_TOP);
	assert(type_at(8, 71, 14) == BLOCK_DIRT); // unchanged, no fire
	printf("  adjacent cell not air: no fire OK\n");
}

static void test_durability_on_every_use_and_break_after_65(void) {
	const int x = 12, y = 70, z = 24;
	set(x, y, z, BLOCK_STONE);
	set(x, y + 1, z, BLOCK_DIRT); // blocked target: Beta still damages the tool
	struct item_data flint = {
		.id = ITEM_FLINT_STEEL, .durability = 0, .count = 1};
	for(int use = 1; use <= 64; use++) {
		flint_use_stack(&flint, x, y, z, SIDE_TOP);
		assert(flint.id == ITEM_FLINT_STEEL && flint.count == 1);
		assert(flint.durability == use);
	}
	flint_use_stack(&flint, x, y, z, SIDE_TOP);
	assert(flint.id == 0 && flint.count == 0 && flint.durability == 0);
	assert(type_at(x, y + 1, z) == BLOCK_DIRT);
	puts("  durability: blocked and successful uses damage; use 65 breaks maxDamage=64 OK");
}

static void test_server_dispatch_mutates_real_hotbar_stack(void) {
	const int x = 20, y = 70, z = 20;
	set(x, y, z, BLOCK_STONE);
	set(x, y + 1, z, BLOCK_AIR);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;
	inventory_set_hotbar(&s.player.inventory, 0);
	inventory_set_slot(
		&s.player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = ITEM_FLINT_STEEL, .count = 1});
	struct server_rpc use = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_TOP},
	};
	server_local_test_process_rpc(&s, &use);
	struct item_data held;
	assert(inventory_get_slot(
		&s.player.inventory, INVENTORY_SLOT_HOTBAR, &held));
	assert(held.id == ITEM_FLINT_STEEL && held.count == 1
		   && held.durability == 1);
	assert(type_at(x, y + 1, z) == BLOCK_FIRE);
	puts("  server dispatch: real hotbar stack damages and fire placement commits OK");
}

static void test_tnt_controller_ignition_damages_real_stack(void) {
	const int x = 22, y = 70, z = 20;
	set(x, y, z, BLOCK_TNT);
	set(x, y + 1, z, BLOCK_AIR);
	struct server_rpc use = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_TOP},
	};
	server_local_test_process_rpc(&s, &use);
	assert(type_at(x, y, z) == BLOCK_AIR);
	bool found_primed = false;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_PRIMED_TNT)
			found_primed = true;
		dict_entity_next(it);
	}
	assert(found_primed);
	struct item_data held;
	assert(inventory_get_slot(
		&s.player.inventory, INVENTORY_SLOT_HOTBAR, &held));
	assert(held.id == ITEM_FLINT_STEEL && held.count == 1
		   && held.durability == 2);
	puts("  Wii TNT ignition: primes entity and damages the real flint stack OK");
}

static void test_tnt_non_flint_falls_through_to_item_placement(void) {
	const int x = 24, y = 70, z = 20;
	set(x, y, z, BLOCK_TNT);
	set(x, y + 1, z, BLOCK_AIR);
	inventory_set_slot(
		&s.player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = BLOCK_STONE, .count = 2});

	struct server_rpc use = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_TOP},
	};
	server_local_test_process_rpc(&s, &use);

	/* Beta BlockTNT.blockActivated returns false, so PlayerController then uses
	 * the held stack. The old void callback swallowed this entire fallback. */
	assert(type_at(x, y, z) == BLOCK_TNT);
	assert(type_at(x, y + 1, z) == BLOCK_STONE);
	struct item_data held;
	assert(inventory_get_slot(
		&s.player.inventory, INVENTORY_SLOT_HOTBAR, &held));
	assert(held.id == BLOCK_STONE && held.count == 1);
	puts("  TNT non-flint click: adjacent block placement and consumption fall through OK");
}

int main(void) {
	test_fixture_dir("test_flint_steel_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	assert(item_flint_steel.has_damage && item_flint_steel.max_damage == 64);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
						INVENTORY_SIZE, 0, 0, 0));
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	printf("test_flint_steel:\n");
	test_fire_on_non_flammable_face();
	test_side_mapping();
	test_no_fire_if_adjacent_not_air();
	test_durability_on_every_use_and_break_after_65();
	test_server_dispatch_mutates_real_hotbar_stack();
	test_tnt_controller_ignition_damages_real_stack();
	test_tnt_non_flint_falls_through_to_item_placement();

	g_drain_run = 0;
	thread_join(&drain);
	inventory_destroy(&s.player.inventory, &s.player);
	printf("PASS test_flint_steel\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
