/*
	FH-08 (docs/fork_hack_audit_2026-08-21.md): the fork's zombie spawn egg is
	entirely net-new content -- Beta 1.7.3 has no spawn-egg concept anywhere in
	Item.java's static initializer -- and it was inherited from fCavEX squatting
	item id 360, one past Beta's own sequential ceiling (359, shears)
	[740c583: Item.java static initializer]. Relocated here to id 500, mirroring
	the FH-07 `BLOCK_IRON_CHEST` 95->200 precedent (docs/deviations.md "Zombie
	spawn egg" entry has the full id-choice rationale).

	No save migration is required (unlike FH-07's CaveWiiSaveVersion 3 region
	rewrite): the egg has never been obtainable in any released build of this
	port -- no recipe, no debug/creative kit entry, no mob drop, no give
	command (verified by a full-repo grep before this test was written) -- so
	no legacy CaveWii save can hold an item stack under the old id for this
	definition. Beta 1.7.3 itself never defines id 360 either, so a genuine
	Beta save can't collide either. `items[360]` is intentionally left
	unregistered.

	Proven here:
	  1) Registry: the enum value moved to 500; items[500] is the egg;
	     items[360] is unregistered (NULL) -- the FH-08 regression guard.
	  2) item_get() safety: a synthetic stack carrying the OLD id 360 (as a
	     foreign/imported save byte would) resolves to NULL ("unknown item"),
	     not this egg -- the collision FH-08 flagged is closed in both
	     directions.
	  3) Identity: name/has_damage/max_stack/fuel/tool/icon are byte-identical
	     to before the relocation -- only the id moved.
	  4) Functional: item_egg_zombie.onItemPlace still spawns exactly one
	     MONSTER_ZOMBIE entity and returns false (infinite uses), proving the
	     relocation didn't disturb behaviour -- the item-scale analogue of
	     FH-07's "full feature set unchanged" claim for the relocated block.
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
#include "entity/monsters_object.h"
#include "item/items.h"
#include "item/items_object.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_egg_zombie_item_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 96

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

int main(void) {
	test_fixture_dir("test_egg_zombie_item_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	// --- 1) Registry: the FH-08 relocation -------------------------------
	assert(ITEM_SHEARS == 359);	// Beta's own ceiling, unmoved
	assert(ITEM_EGG_ZOMBIE == 500); // relocated off the squatted 360
	assert(items[ITEM_EGG_ZOMBIE] == &item_egg_zombie);
	assert(items[360] == NULL); // the id is deliberately left free

	// --- 2) item_get() safety: a foreign/legacy 360 stack -----------------
	struct item_data foreign_360 = {.id = 360, .count = 1, .durability = 0};
	assert(item_get(&foreign_360) == NULL); // "unknown item", not this egg
	struct item_data relocated = {.id = ITEM_EGG_ZOMBIE, .count = 1};
	assert(item_get(&relocated) == &item_egg_zombie);

	// --- 3) Identity: unchanged by the move --------------------------------
	assert(strcmp(item_egg_zombie.name, "Zombie Egg") == 0);
	assert(!item_egg_zombie.has_damage);
	assert(item_egg_zombie.max_stack == 64);
	assert(item_egg_zombie.fuel == 0);
	assert(!item_egg_zombie.armor.is_armor);
	assert(item_egg_zombie.tool.type == TOOL_TYPE_ANY);
	assert(item_egg_zombie.render_data.item.texture_x == 8);
	assert(item_egg_zombie.render_data.item.texture_y == 4);
	assert(item_egg_zombie.onItemPlace != NULL);
	printf("[registry+identity] OK\n");

	// --- 4) Functional: onItemPlace still spawns exactly one zombie --------
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	assert(dict_entity_size(s.entities) == 0);
	struct item_data egg_stack = {.id = ITEM_EGG_ZOMBIE, .count = 1};
	struct block_data air = {.type = BLOCK_AIR, .metadata = 0};
	struct block_data ground = {.type = BLOCK_STONE, .metadata = 0};
	struct block_info where = {.block = &air, .x = 4, .y = Y, .z = 4};
	struct block_info on = {.block = &ground, .x = 4, .y = Y - 1, .z = 4};
	bool consumed
		= item_egg_zombie.onItemPlace(&s, &s.player, &egg_stack, &where, &on, SIDE_TOP);
	assert(!consumed); // "spawn eggs have infinite uses" -- unchanged by the move

	assert(dict_entity_size(s.entities) == 1);
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	assert(!dict_entity_end_p(it));
	struct entity* e = dict_entity_ref(it)->value;
	assert(e->type == ENTITY_MONSTER);
	assert(e->data.monster.id == MONSTER_ZOMBIE);
	dict_entity_next(it);
	assert(dict_entity_end_p(it)); // exactly one spawn, no side effects
	printf("[onItemPlace spawns MONSTER_ZOMBIE, infinite-use] OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_egg_zombie_item: ALL PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
