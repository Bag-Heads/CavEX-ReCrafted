/*
	ci-1101/44 (HUNT-14 item 5): Beta 1.7.3 shears parity.

	Ground truth (../mcb173-retromcp/minecraft/src/net/minecraft/src/):
	  - ItemShears.java:18-20 canHarvestBlock: true ONLY for Block.web.
	  - ItemShears.java:22-24 getStrVsBlock: 15.0F for web AND leaves, 5.0F for
	    cloth (wool), else Item's default 1.0F.
	  - ItemShears.java:10-13 onBlockDestroyed: damageItem(1) for leaves/web.
	  - Material.java:30 field_31068_A (BlockWeb.java:7's material) is
	    setNoHarvest(), so InventoryPlayer.canHarvestBlock (InventoryPlayer.java:
	    270-277) falls through to the held stack: only shears
	    (ItemShears.canHarvestBlock) or a sword (ItemSword.canHarvestBlock)
	    harvest a web; a bare hand does not.
	  - Block.java:53 web hardness 4.0F; :41 leaves 0.2F; :58 cloth 0.8F.
	  - Block.java:314 blockStrength: canHarvest ? strVs/hardness/30
	    : 1/hardness/100 per tick. InventoryPlayer.java:189-196 strVs = 1.0F *
	    held.getStrVsBlock.

	Pinned dig times (ms = ceil(ticks) * 50, the engine's FH-01 scale,
	hardness_field = hardness * 1500):
	  web    hand    : cannot harvest, 1/4.0/100   -> 400 ticks = 20000ms
	  web    shears  : 15/4.0/30 = 0.125            ->   8 ticks =   400ms
	  web    sword   : 15/4.0/30 (ItemSword)        ->   8 ticks =   400ms
	  leaves hand    : 1/0.2/30                     ->   6 ticks =   300ms
	  leaves shears  : 15/0.2/30 = 2.5 (>=1)        ->   1 tick  =    50ms
	  wool   hand    : 1/0.8/30                     ->  24 ticks =  1200ms
	  wool   shears  : 5/0.8/30 = 0.2083            ->   5 ticks =   250ms
	  stone  shears  : ItemShears.canHarvest false  -> same as hand, 7500ms

	Live-server checks (SRPC_BLOCK_DIG): web+shears drops string, web+hand
	drops nothing, web+sword drops string; shears lose 1 durability on
	leaves and on web, 0 on wool and stone.
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
#define MKDIR(p) mkdir(p, 0755)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "item/items.h"
#include "item/tool.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

// Defined in source/item/items/item_shears.c (forward-declared, no header).
extern bool item_shears_can_harvest(uint8_t block_type, uint16_t held_id);

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

// Overworld-only fixture: every world access names &s.worlds[0] (the Overworld slot) explicitly.
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

static struct item* item_of(uint16_t id) {
	struct item_data d = {.id = id, .count = 1};
	struct item* it = item_get(&d);
	assert(it && "item id not registered");
	return it;
}

static void check_ms(struct block* blk, const char* blk_name, uint16_t tool_id,
					 const char* tool_name, bool in_water, bool on_ground,
					 int want) {
	struct item* it = tool_id ? item_of(tool_id) : NULL;
	int got = tool_dig_delay_env_ms(blk, it, in_water, on_ground);
	if(got != want) {
		fprintf(stderr,
				"test_shears_parity: %s with %s (water=%d ground=%d) = %dms, "
				"want %dms (Beta ItemShears.getStrVsBlock math)\n",
				blk_name, tool_name, in_water, on_ground, got, want);
		assert(0 && "dig delay diverges from Beta");
	}
	printf("  %s + %s (water=%d ground=%d) = %dms OK\n", blk_name, tool_name,
		   in_water, on_ground, got);
}

static void check_dig_speed(void) {
	// Web: ItemShears.java:23 15.0F; Material.java:30 setNoHarvest.
	check_ms(&block_cobweb, "web", 0, "hand", false, true, 20000);
	check_ms(&block_cobweb, "web", ITEM_SHEARS, "shears", false, true, 400);
	check_ms(&block_cobweb, "web", ITEM_IRON_SWORD, "iron sword", false, true,
			 400);
	// Shears on web is the canHarvest arm, so EntityPlayer.java:291-302's
	// water/airborne /5.0F applies: 15/5/4/30 -> 40 ticks; both -> 200.
	check_ms(&block_cobweb, "web", ITEM_SHEARS, "shears", true, true, 2000);
	check_ms(&block_cobweb, "web", ITEM_SHEARS, "shears", true, false, 10000);
	// Leaves: ItemShears.java:23 15.0F.
	check_ms(&block_leaves, "leaves", 0, "hand", false, true, 300);
	check_ms(&block_leaves, "leaves", ITEM_SHEARS, "shears", false, true, 50);
	// Wool (Block.cloth): ItemShears.java:23 5.0F.
	check_ms(&block_wool, "wool", 0, "hand", false, true, 1200);
	check_ms(&block_wool, "wool", ITEM_SHEARS, "shears", false, true, 250);
	// Everything else: Item.getStrVsBlock 1.0F; stone is tool-gated and
	// ItemShears.canHarvestBlock is false -> hand's cannot-harvest time.
	check_ms(&block_stone, "stone", ITEM_SHEARS, "shears", false, true, 7500);
	check_ms(&block_dirt, "dirt", ITEM_SHEARS, "shears", false, true,
			 tool_dig_delay_ms(&block_dirt, NULL));
}

static void check_can_harvest(void) {
	// ItemShears.java:18-20.
	assert(item_shears_can_harvest(BLOCK_COBWEB, ITEM_SHEARS));
	assert(!item_shears_can_harvest(BLOCK_STONE, ITEM_SHEARS));
	assert(!item_shears_can_harvest(BLOCK_LEAVES, ITEM_SHEARS));
	assert(!item_shears_can_harvest(BLOCK_COBWEB, ITEM_IRON_SWORD));
	assert(!item_shears_can_harvest(BLOCK_COBWEB, 0));
	printf("  item_shears_can_harvest == ItemShears.canHarvestBlock OK\n");
}

// ---- live server ---------------------------------------------------------
static void set_block(int x, int y, int z, int type) {
	struct block_data blk = {.type = (uint8_t)type, .metadata = 0};
	server_world_set_block(&s.worlds[0], x, y, z, blk);
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &blk));
	return blk.type;
}

static void give(uint16_t id, uint16_t durability) {
	inventory_set_hotbar(&s.player.inventory, 0);
	if(id == 0)
		inventory_clear_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR);
	else
		inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
						   (struct item_data) {
							   .id = id, .count = 1, .durability = durability});
}

static uint16_t held_durability(void) {
	return s.player.inventory.items[INVENTORY_SLOT_HOTBAR].durability;
}

static void dig(int x, int y, int z) {
	struct server_rpc r = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.x = x, .y = y, .z = z, .finished = true}};
	server_local_test_process_rpc(&s, &r);
}

static bool take_one_item_drop(struct item_data* out) {
	enum { SNAP_MAX = 16 };
	uint32_t keys[SNAP_MAX];
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it) && n < SNAP_MAX) {
		keys[n++] = dict_entity_ref(it)->key;
		dict_entity_next(it);
	}
	bool found = false;
	for(size_t i = 0; i < n; i++) {
		struct entity** pp = dict_entity_get(s.entities, keys[i]);
		if(pp && *pp && (*pp)->type == ENTITY_ITEM) {
			struct entity* e = *pp;
			if(out && !found)
				*out = e->data.item.item;
			found = true;
			free(e);
			dict_entity_erase(s.entities, keys[i]);
		}
	}
	return found;
}

static void check_dig(int block_id, const char* blk_name, uint16_t tool_id,
					  const char* tool_name, bool want_drop, int want_drop_id,
					  int want_wear) {
	set_block(5, 70, 5, block_id);
	give(tool_id, 0);
	dig(5, 70, 5);
	assert(type_at(5, 70, 5) == BLOCK_AIR && "the block must break");

	struct item_data d;
	bool got = take_one_item_drop(&d);
	if(got != want_drop) {
		fprintf(stderr,
				"test_shears_parity: %s broken with %s %s -- Beta "
				"canHarvestBlock says it must%s\n",
				blk_name, tool_name, got ? "DROPPED" : "did NOT drop",
				want_drop ? "" : " NOT");
		assert(0 && "harvest/drop gate diverges from Beta");
	}
	if(want_drop && d.id != want_drop_id) {
		fprintf(stderr, "test_shears_parity: %s + %s dropped id=%d, want %d\n",
				blk_name, tool_name, d.id, want_drop_id);
		assert(0 && "wrong drop");
	}
	if(tool_id && (int)held_durability() != want_wear) {
		fprintf(stderr,
				"test_shears_parity: %s + %s wear=%d, want %d "
				"(ItemShears.java:10-13)\n",
				blk_name, tool_name, held_durability(), want_wear);
		assert(0 && "shears wear diverges from Beta");
	}
	printf("  %s + %s: drop=%d id=%d wear=%d OK\n", blk_name, tool_name, got,
		   got ? d.id : 0, tool_id ? held_durability() : 0);
}

int main(void) {
	test_fixture_dir("test_shears_parity_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	printf("test_shears_parity: Beta 1.7.3 ItemShears (ci-1101/44)\n");

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	check_dig_speed();
	check_can_harvest();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);

	// BlockWeb.java:30-32 idDropped = Item.silk (string), gated by
	// InventoryPlayer.canHarvestBlock (Material.java:30 setNoHarvest).
	check_dig(BLOCK_COBWEB, "web", ITEM_SHEARS, "shears", true, ITEM_STRING, 1);
	check_dig(BLOCK_COBWEB, "web", 0, "hand", false, 0, 0);
	check_dig(BLOCK_COBWEB, "web", ITEM_WOOD_SWORD, "wood sword", true,
			  ITEM_STRING, 2 /* ItemSword.onBlockDestroyed damageItem(2) */);
	// Leaves with shears: the leaf block itself (BlockLeaves.harvestBlock),
	// 1 wear (ItemShears.java:11-12).
	check_dig(BLOCK_LEAVES, "leaves", ITEM_SHEARS, "shears", true,
			  BLOCK_LEAVES, 1);
	// Wool: drops itself with any item, NO shears wear (not in :11).
	check_dig(BLOCK_WOOL, "wool", ITEM_SHEARS, "shears", true, BLOCK_WOOL, 0);
	// Stone: ItemShears.canHarvestBlock false -> no drop, no wear.
	check_dig(BLOCK_STONE, "stone", ITEM_SHEARS, "shears", false, 0, 0);

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_shears_parity: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
