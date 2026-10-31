/*
	PAR-WIRE-POP env test. Beta BlockRedstoneWire.onNeighborBlockChange (src_original :268-279): if
	!canPlaceBlockAt (:41-43, isBlockNormalCube below) -> dropBlockAsItem(meta) (idDropped = redstone,
	:282-284) + setBlockWithNotify(0), whose onBlockRemoved re-floods the cut line. Real production
	path over a live superflat world (harness of test_redstone_torch_support_drop_env.c). Expected:
	AIR, exactly ONE redstone item, line 15/14/13 cut to 15/air/0 (wire beside a lit torch = 15, -1
	per step); the CONTROL row (support intact) is untouched.
*/

#include <assert.h>
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
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y 101  // circuit plane

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
	assert(server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static void tick(void) { server_world_tick(&s.worlds[0], &s); }
static size_t entity_count(void) { return dict_entity_size(s.entities); }
static int redstone_items(void) {
	int count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_REDSTONE)
			count += e->data.item.item.count;
		dict_entity_next(it);
	}
	return count;
}

int main(void) {
	test_fixture_dir("test_rs_wire_supportdrop_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src); // dropBlockAsItem spawns an item entity (RNG + the dict)
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);

	for(int x = 1; x <= 8; x++)
		for(int z = 4; z <= 8; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	const int ZT = 5; // row under test: the stone under the middle wire is mined
	const int ZC = 8; // control row: support intact
	const int rows[2] = {ZT, ZC};
	for(int r = 0; r < 2; r++) {
		set(2, Y, rows[r], BLOCK_REDSTONE_TORCH_LIT, 5); // floor torch on the stone
		set(3, Y, rows[r], BLOCK_REDSTONE_WIRE, 0);
		set(4, Y, rows[r], BLOCK_REDSTONE_WIRE, 0);
		set(5, Y, rows[r], BLOCK_REDSTONE_WIRE, 0);
	}
	for(int k = 0; k < 30; k++)
		tick();

	for(int r = 0; r < 2; r++) {
		assert(get(2, Y, rows[r]) == BLOCK_REDSTONE_TORCH_LIT);
		assert(getmeta(3, Y, rows[r]) == 15);
		assert(getmeta(4, Y, rows[r]) == 14);
		assert(getmeta(5, Y, rows[r]) == 13);
	}
	const size_t base_entities = entity_count();
	assert(redstone_items() == 0);

	// Mine the support under the middle wire: notify -> wire onNeighbourBlockChange ->
	// canPlaceBlockAt false -> dropBlockAsItem + setBlockWithNotify(0).
	set(4, YF, ZT, BLOCK_AIR, 0);

	// Synchronous in Beta: checked BEFORE any tick runs.
	printf("after mine: wire cell type=%d (air=%d), redstone items=%d, entities +%d\n",
		   get(4, Y, ZT), BLOCK_AIR, redstone_items(), (int)(entity_count() - base_entities));
	assert(get(4, Y, ZT) == BLOCK_AIR);			 // no floating dust
	assert(redstone_items() == 1);				 // exactly one redstone dust item
	assert(entity_count() == base_entities + 1); // and nothing else spawned
	assert(getmeta(3, Y, ZT) == 15);			 // torch side keeps its power
	assert(getmeta(5, Y, ZT) == 0);				 // far side is cut off at once

	for(int k = 0; k < 30; k++)
		tick();
	assert(get(4, Y, ZT) == BLOCK_AIR);
	assert(getmeta(3, Y, ZT) == 15);
	assert(getmeta(5, Y, ZT) == 0);
	assert(get(5, Y, ZT) == BLOCK_REDSTONE_WIRE); // still supported: stays

	// CONTROL: untouched line, no extra drop.
	assert(getmeta(3, Y, ZC) == 15);
	assert(getmeta(4, Y, ZC) == 14);
	assert(getmeta(5, Y, ZC) == 13);
	assert(redstone_items() == 1);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	printf("test_redstone_wire_support_drop_env: OK (support mined -> air + 1 redstone item; line "
		   "cut 15/air/0; control 15/14/13)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
