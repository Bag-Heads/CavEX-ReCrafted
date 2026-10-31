/*
	Requirement 5 (lane/craft-rpc): persistent map creation goes through the
	SAME server path as the manual grid -- no client-side map id allocation.

	A crafted map is the one recipe whose result carries identity rather than
	just a count: [740c583 ItemMap.onCreated] stamps the stack with
	`MapStorage.getUniqueDataId("map")` and seeds a MapData centred on the
	player. In this engine that id is DURABLE -- source/item/items/item_map.c's
	item_map_on_created reserves it through map_idcounts_get_unique_id, which
	writes idcounts.dat before returning -- so two players (or a replay) can
	never be handed the same map. inventory_click.c's craft_created already
	splits the two sides: with a server it calls Item.onCreated; without one it
	falls back to the client-world counter for prediction only.

	The guided craft therefore MUST hand its server down. This file proves it,
	and proves the one hazard the transaction's two-pass (dry run, then
	committing run) design creates: that a map craft could quietly burn TWO
	durable ids, one of them thrown away with the dry run. The dry run is passed
	server = NULL precisely so it cannot, and the assertion here is exact --
	N successful map crafts advance the durable counter by exactly N.
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
#include "item/inventory.h"
#include "item/items.h"
#include "item/items/item_map.h"
#include "item/recipe.h"
#include "network/client_interface.h"
#include "network/inventory_crafting.h"
#include "network/map_data.h"
#include "network/map_idcounts.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "platform/thread.h"
#include "worldgen/jmath.h"

/* A second, non-map Item.onCreated -- the case the engine does not ship yet and
 * whose absence hid a client-prediction bug. Ids high in the registry (ITEMS_MAX
 * is 2304) so nothing real is shadowed; asserted free before installing. */
#define SYNTH_CREATED 2300
#define SYNTH_INGREDIENT 2301
static unsigned synth_created_calls;
static bool synth_on_created(struct server_local* server,
	struct server_player* actor, struct item_data* stack) {
	(void)server;
	/* S1: onCreated takes the acting crafter; this synthetic hook has no
	 * position to stamp, so it only counts calls. */
	(void)actor;
	(void)stack;
	synth_created_calls++;
	return true;
}
static struct item synth_created
	= {.max_stack = 64, .onCreated = synth_on_created};
static struct item synth_ingredient = {.max_stack = 64};

static char s_fixture_TMP_WORLD[128];
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

static uint16_t map_recipe(void) {
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++)
		if(array_recipe_cget(recipes_crafting, n)->result.id == ITEM_MAP)
			return (uint16_t)n;
	assert(!"map recipe not registered");
	return RECIPE_CRAFT_INDEX_NONE;
}

/* 8 paper + 1 compass, enough for `batches` maps, plus room to hold them. */
static void seed_map_ingredients(struct inventory* inv, unsigned batches) {
	inventory_clear(inv);
	inv->items[CRAFTING_SLOT_HOTBAR + 0]
		= (struct item_data) {.id = ITEM_PAPER, .count = (uint8_t)(8 * batches)};
	inv->items[CRAFTING_SLOT_HOTBAR + 1]
		= (struct item_data) {.id = ITEM_COMPASS, .count = (uint8_t)batches};
}

static bool find_map(const struct inventory* inv, uint16_t* durability) {
	for(size_t k = CRAFTING_SLOT_MAIN; k < CRAFTING_SIZE; k++)
		if(inv->items[k].id == ITEM_MAP) {
			*durability = inv->items[k].durability;
			return true;
		}
	return false;
}

int main(void) {
	test_fixture_dir("test_recipe_craft_map_tmp", s_fixture_TMP_WORLD,
		sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	recipe_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/data"));
	remove(test_fixture_join(TMP_WORLD, "/data/idcounts.dat"));
	remove(test_fixture_join(TMP_WORLD, "/data/idcounts.dat_new"));
	for(int mid = 0; mid < 6; mid++) {
		char path[192];
		snprintf(path, sizeof(path), "%s/data/map_%d.dat", TMP_WORLD, mid);
		remove(path);
		snprintf(path, sizeof(path), "%s/data/map_%d.dat_new", TMP_WORLD, mid);
		remove(path);
	}

	memset(&s, 0, sizeof(s));
	/* MAX_MAP_CACHE free-slot sentinel, exactly as test_map_item.c sets it up:
	 * a raw memset(0) would misread slot 0 as already caching map id 0. */
	for(int i = 0; i < MAX_MAP_CACHE; i++) s.map_cache_id[i] = INT32_MIN;
	string_init_set_str(s.level_name, TMP_WORLD);
	/* Non-integer position, so a truncating port would visibly disagree with
	 * [740c583 ItemMap.onCreated]'s floor(posX)/floor(posZ). */
	s.player.x = 10.7;
	s.player.y = 64.0;
	s.player.z = -3.2;
	s.player.dimension = WORLD_DIM_OVERWORLD;

	uint16_t recipe = map_recipe();
	struct inventory inv;
	uint16_t client_counter = 500;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	/* Deliberately give the inventory a client counter as well: if the guided
	 * craft ever fell back to the client path, this would move. */
	inv.client_map_counter = &client_counter;

	/* --- one guided craft, driven with the real server ------------------- */
	seed_map_ingredients(&inv, 1);
 for(int check=0;check<3;check++){
  struct inventory before;memcpy(&before,&inv,sizeof before);
  struct item_data cells[CRAFTING_SIZE];memcpy(cells,inv.items,sizeof cells);
  assert(inventory_crafting_one_batch_preflight(&inv,WINDOW_TYPE_WORKBENCH,recipe)==RECIPE_CRAFT_OK);
  assert(!memcmp(&before,&inv,sizeof before)&&!memcmp(cells,inv.items,sizeof cells)&&client_counter==500);
 }
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, recipe, &s,
			   NULL)
		== RECIPE_CRAFT_OK);
	uint16_t first = 0;
	assert(find_map(&inv, &first));
	/* Beta's getUniqueDataId returns 0 for a never-used key; this engine
	 * reserves zero (maintainer ruling, cited in inventory_click.c), so the
	 * first durable allocation is 1 -- the same value test_map_item.c pins for
	 * a direct item_map.onCreated call. Reaching it THROUGH the craft is the
	 * point: the guided path and the manual path allocate identically. */
	assert(first == 1);
	assert(client_counter == 500); /* the client counter was never touched */

	/* The full server seeding ran, not just an id stamp. */
	struct map_data* md = item_map_cache_resident(&s, 1);
	assert(md != NULL);
	assert(md->x_center == 10);	 /* floor(10.7) */
	assert(md->z_center == -4);	 /* floor(-3.2), NOT truncation's -3 */
	assert(md->scale == 3);
	assert(md->dimension == WORLD_DIM_OVERWORLD);

	/* Ingredients consumed exactly once; the grid handed back empty. */
	assert(inv.items[CRAFTING_SLOT_HOTBAR + 0].count == 0);
	assert(inv.items[CRAFTING_SLOT_HOTBAR + 1].count == 0);
	for(size_t k = CRAFTING_SLOT_OUTPUT; k < CRAFTING_SLOT_MAIN; k++)
		assert(!inv.items[k].id);
	puts("  guided craft allocated durable map id 1 via the server path");

	/* --- a second craft, then probe the durable counter ------------------ */
	seed_map_ingredients(&inv, 1);
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, recipe, &s,
			   NULL)
		== RECIPE_CRAFT_OK);
	uint16_t second = 0;
	assert(find_map(&inv, &second));
	assert(second == 2);
	assert(client_counter == 500);

	/* THE dry-run assertion: two successful crafts must have advanced the
	 * durable counter by exactly two, so the thrown-away preflight pass spent
	 * nothing. The probe itself takes the next id, which must be 3. */
	int32_t probe = 0;
	assert(map_idcounts_get_unique_id(s.level_name, "map", &probe));
	assert(probe == 3);
	puts("  two crafts consumed exactly two durable ids: the dry run spends none");

	/* --- a REJECTED map craft must spend no id at all -------------------- */
	inventory_clear(&inv);
	/* Every player slot full and unmergeable: the map cannot be placed, so the
	 * craft is refused before any allocation. */
	for(size_t k = CRAFTING_SLOT_MAIN; k < CRAFTING_SIZE; k++)
		inv.items[k] = (struct item_data) {.id = BLOCK_COBBLESTONE, .count = 64};
	inv.items[CRAFTING_SLOT_MAIN + 0]
		= (struct item_data) {.id = ITEM_PAPER, .count = 64};
	inv.items[CRAFTING_SLOT_MAIN + 1]
		= (struct item_data) {.id = ITEM_COMPASS, .count = 64};
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, inv.items, sizeof(before));
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, recipe, &s,
			   NULL)
		== RECIPE_CRAFT_NO_CAPACITY);
	assert(!memcmp(before, inv.items, sizeof(before)));
	int32_t after_reject = 0;
	assert(map_idcounts_get_unique_id(s.level_name, "map", &after_reject));
	assert(after_reject == 4); /* only this probe moved it, not the rejection */
	puts("  a rejected map craft allocates no durable id and mutates nothing");

	/* --- and the client-side fallback stays test/prediction-only --------- */
	seed_map_ingredients(&inv, 1);
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, recipe,
			   NULL, NULL)
		== RECIPE_CRAFT_OK);
	uint16_t predicted = 0;
	assert(find_map(&inv, &predicted));
	assert(predicted == 501);	   /* the CLIENT counter, seeded at 500 */
	assert(client_counter == 501); /* advanced by exactly one */
	/* The ALLOCATION itself is carried back out of the transaction's scratch
	 * copy, not just the counter (hostile review, finding 4): revision.
	 * created_map_id is what lets a correction replay reuse this id instead of
	 * minting another, and map_counter_before is what hands the counter back if
	 * the action is never sent -- the same pair inventory_click.c's craft_created
	 * writes for a hand-clicked craft. */
	assert(inv.revision.created_map_id == 501);
	assert(inv.revision.map_counter_before == 500);
	int32_t untouched = 0;
	assert(map_idcounts_get_unique_id(s.level_name, "map", &untouched));
	assert(untouched == 5); /* the durable counter saw only the probes */
	puts("  serverless prediction uses the client counter and never idcounts");

	/* --- a NON-map onCreated must predict SUCCESS ------------------------ */
	/* [740c583 Item.onCreated] returns void: a craft with an onCreated hook
	 * always completes. The map is the sole hook in this engine that can refuse,
	 * because its id comes from a durable on-disk counter no client may spend.
	 * craft_created used to answer false for EVERY non-map onCreated in the
	 * serverless path, so the very first additional hook would have made the
	 * client predict a failed craft the server happily performs. Register one and
	 * pin the predicted success. */
	assert(!items[SYNTH_CREATED] && !items[SYNTH_INGREDIENT]);
	items[SYNTH_CREATED] = &synth_created;
	items[SYNTH_INGREDIENT] = &synth_ingredient;
	recipe_add(recipes_crafting,
		(struct item_data) {.id = SYNTH_CREATED, .count = 1}, 1, 1,
		(uint8_t[]) {1}, (struct item_data) {.id = SYNTH_INGREDIENT}, false);
	uint16_t synth_recipe = (uint16_t)(array_recipe_size(recipes_crafting) - 1);
	inventory_clear(&inv);
	inv.items[CRAFTING_SLOT_HOTBAR]
		= (struct item_data) {.id = SYNTH_INGREDIENT, .count = 1};
	synth_created_calls = 0;
 assert(inventory_crafting_one_batch_preflight(&inv,WINDOW_TYPE_WORKBENCH,synth_recipe)==RECIPE_CRAFT_OK);
 assert(synth_created_calls==0);
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH,
			   synth_recipe, NULL, NULL)
		== RECIPE_CRAFT_OK);
	/* Predicted, so the hook itself never ran -- only the server may run it. */
	assert(synth_created_calls == 0);
	assert(client_counter == 501); /* and it is not a map: no id was spent */
	unsigned made = 0;
	for(size_t k = CRAFTING_SLOT_MAIN; k < CRAFTING_SIZE; k++)
		if(inv.items[k].id == SYNTH_CREATED) made += inv.items[k].count;
	assert(made == 1);
	puts("  a non-map onCreated predicts success instead of failing the craft");

	inventory_destroy(&inv, NULL);
	string_clear(s.level_name);
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	puts("PASS test_recipe_craft_map_server: map identity is allocated by the "
		 "server path, once per successful batch");
	return 0;
}
