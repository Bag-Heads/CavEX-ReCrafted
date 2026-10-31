/*
	Batch H (docs/parity_audit_2026-08-25.md, items 1-3): the dig-speed cluster.
	Golden goldens for tool_dig_delay_ms(&block, item), following
	test_redstone_ore_dig.c's pattern (a pure function; no server needed for
	the speed matrix). One live-server drop-gate check closes item 3 (the
	wooden pressure plate is now tool-free harvestable, unlike before).

	Ground truth [decompile 740c583], all independently re-derived (not just
	copied from the audit register) -- see the block_*.c comments landed
	alongside this fix for the exact Block.java/ItemPickaxe.java/ItemAxe.java
	citations per block.

	(1) Obsidian (block_obsidian.c): hardness 10000->15000 (Block.java:641
	    setHardness(10.0F)*1500), .best MAX->ANY. ItemPickaxe.canHarvestBlock
	    requires harvestLevel==3 (diamond only) for obsidian, so every
	    non-diamond tool -- including a mismatched-tier pickaxe -- takes
	    Block.blockStrength's "cannot harvest" 1/hardness/100 branch
	    (tool.c's digging.min mismatch -> *10/3 branch): 15000*10/3 = 50000ms.
	    Only a diamond pickaxe harvests it, and even then gets NO per-tier
	    bonus (obsidian is omitted from blocksEffectiveAgainst): 15000/1 =
	    15000ms flat -- NOT divided by tool_tier_divider(DIAMOND)=8, which is
	    exactly the ~12x-too-fast bug this fixes (was 1250ms).

	(2) The 9-block .best sweep: furnace(x2)/dispenser/spawner/iron
	    door/bricks/stone plate (.tool=PICKAXE, .min=WOOD unchanged,
	    .best WOOD->ANY) and workbench/glowstone (.best MAX->ANY). Every one
	    of these blocks is omitted from its tool's blocksEffectiveAgainst
	    array, so ANY qualifying tool digs at the SAME flat hardness/30 rate
	    -- no per-tier speedup. Only the harvest-capable case is checked here
	    (a below-.min tool, e.g. hand on a PICKAXE-gated block, still takes
	    the *10/3 mismatched-tool branch -- unaffected by this fix, so
	    covered as a sanity/regression check, not the fix's own claim).

	(3) Wooden pressure plate (block_pressure_plate.c): .tool/.min/.best all
	    ANY (was PICKAXE/WOOD/WOOD) -- Material.wood is never gated by
	    Material.java's setNoHarvest, so InventoryPlayer.canHarvestBlock
	    returns true unconditionally (bare hand included) and
	    Block.blockStrength always takes the flat hardness/30 branch: 750ms
	    with ANY tool OR bare hands, matching block_wool.c/block_leaves.c's
	    existing tool-free idiom. A live-server drop-gate check confirms a
	    bare-handed break both breaks AND drops the plate (server_local.c's
	    digging.min==ANY / digging.tool==ANY drop-gate disjunction).
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
#include "entity/entity.h"
#include "item/items.h"
#include "item/tool.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_dig_speed_batch_h_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define BLOCK_WOODEN_PLATE 72

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

static struct item* tool_item(uint16_t id) {
	struct item_data d = {.id = id, .count = 1};
	struct item* it = item_get(&d);
	assert(it && "tool item id not registered");
	return it;
}

// ------------------------------------------------------------------------
// (1) Obsidian: only a diamond pickaxe harvests it, and even then with NO
// per-tier speed bonus.
// ------------------------------------------------------------------------
static void check_obsidian(void) {
	int hand = tool_dig_delay_ms(&block_obsidian, NULL);
	int wood = tool_dig_delay_ms(&block_obsidian, tool_item(ITEM_WOOD_PICKAXE));
	int stone = tool_dig_delay_ms(&block_obsidian, tool_item(ITEM_STONE_PICKAXE));
	int iron = tool_dig_delay_ms(&block_obsidian, tool_item(ITEM_IRON_PICKAXE));
	int gold = tool_dig_delay_ms(&block_obsidian, tool_item(ITEM_GOLD_PICKAXE));
	int diamond
		= tool_dig_delay_ms(&block_obsidian, tool_item(ITEM_DIAMOND_PICKAXE));

	if(hand != 50000 || wood != 50000 || stone != 50000 || iron != 50000
	   || gold != 50000) {
		fprintf(stderr,
				"test_dig_speed_batch_h: obsidian cannot-harvest speed wrong -- "
				"hand=%d wood=%d stone=%d iron=%d gold=%d (want all 50000: "
				"hardness 15000 * 10/3, the mismatched-tier-min penalty -- "
				"only a DIAMOND pickaxe harvests obsidian)\n",
				hand, wood, stone, iron, gold);
		assert(0 && "hand/wood/stone/iron/gold pickaxe must be the slow cannot-harvest rate");
	}
	if(diamond != 15000) {
		fprintf(stderr,
				"test_dig_speed_batch_h: obsidian diamond-pickaxe speed wrong -- "
				"got %d, want 15000 (hardness 15000 / tool_tier_divider(ANY)=1, "
				"NOT /8 -- obsidian is omitted from blocksEffectiveAgainst)\n",
				diamond);
		assert(0 && "diamond pickaxe must dig obsidian at the flat, un-bonused rate");
	}
	printf("  obsidian: hand/wood/stone/gold pickaxe = %dms (cannot-harvest), "
		   "diamond pickaxe = %dms (flat hardness/30, no per-tier bonus) OK\n",
		   hand, diamond);
}

// ------------------------------------------------------------------------
// (2) The 9-block .best sweep: qualifying tools all dig at the SAME flat
// rate (no per-tier bonus); a below-.min tool still gets the *10/3 penalty
// (unaffected regression check).
// ------------------------------------------------------------------------
struct pickaxe_block_case {
	struct block* blk;
	const char* name;
	int hardness;
};

static void check_pickaxe_gated_block(struct pickaxe_block_case c) {
	int hand = tool_dig_delay_ms(c.blk, NULL);
	int wood = tool_dig_delay_ms(c.blk, tool_item(ITEM_WOOD_PICKAXE));
	int stone = tool_dig_delay_ms(c.blk, tool_item(ITEM_STONE_PICKAXE));
	int iron = tool_dig_delay_ms(c.blk, tool_item(ITEM_IRON_PICKAXE));
	int gold = tool_dig_delay_ms(c.blk, tool_item(ITEM_GOLD_PICKAXE));
	int diamond = tool_dig_delay_ms(c.blk, tool_item(ITEM_DIAMOND_PICKAXE));

	int want_penalty = ((c.hardness * 10 / 3) + 49) / 50 * 50; // round up to tick
	if(hand != want_penalty) {
		fprintf(stderr,
				"test_dig_speed_batch_h: %s bare-hand speed wrong -- got %d, "
				"want %d (unaffected mismatched-tool-min *10/3 branch)\n",
				c.name, hand, want_penalty);
		assert(0 && "bare-hand penalty speed regressed");
	}
	if(wood != c.hardness || stone != c.hardness || iron != c.hardness
	   || gold != c.hardness || diamond != c.hardness) {
		fprintf(stderr,
				"test_dig_speed_batch_h: %s per-tier speed wrong -- "
				"wood=%d stone=%d iron=%d gold=%d diamond=%d (want ALL == "
				"hardness %d: no tier is a blocksEffectiveAgainst member, so "
				"every qualifying pickaxe digs at the same flat rate)\n",
				c.name, wood, stone, iron, gold, diamond, c.hardness);
		assert(0 && "every qualifying pickaxe tier must dig at the identical flat rate");
	}
	printf("  %s: bare hand = %dms (unaffected penalty), any pickaxe tier = "
		   "%dms flat (no per-tier bonus) OK\n",
		   c.name, hand, wood);
}

static void check_axe_gated_block(struct block* blk, const char* name,
								  int hardness) {
	// digging.min == TOOL_TIER_ANY for workbench, so bare hand ALSO takes the
	// unpenalized flat branch (no mismatched-tool *10/3 -- min==ANY skips it).
	int hand = tool_dig_delay_ms(blk, NULL);
	int wood = tool_dig_delay_ms(blk, tool_item(ITEM_WOOD_AXE));
	if(hand != hardness || wood != hardness) {
		fprintf(stderr,
				"test_dig_speed_batch_h: %s speed wrong -- hand=%d wood_axe=%d "
				"(want both == hardness %d: workbench's .min=ANY means even "
				"bare hands take the un-penalized flat branch, and .best=ANY "
				"means no axe tier gets a bonus either)\n",
				name, hand, wood, hardness);
		assert(0 && "workbench's flat, tool-agnostic speed regressed");
	}
	printf("  %s: bare hand AND wood axe = %dms flat (Material.wood is "
		   "harvestable by anything; no per-tier bonus) OK\n",
		   name, hand);
}

static void check_nine_block_sweep(void) {
	struct pickaxe_block_case cases[]
		= {{&block_furnaceoff, "furnace (idle)", 5250},
		   {&block_furnaceon, "furnace (lit)", 5250},
		   {&block_dispenser, "dispenser", 5250},
		   {&block_spawner, "mob spawner", 7500},
		   {&block_iron_door, "iron door", 7500},
		   {&block_bricks, "bricks", 3000},
		   {&block_stone_pressure_plate, "stone pressure plate", 750}};
	for(size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
		check_pickaxe_gated_block(cases[i]);

	check_axe_gated_block(&block_workbench, "workbench", 3750);

	// Glowstone is pickaxe-gated like the first 7, but its .min stayed WOOD
	// (unlike workbench's ANY), so it belongs with check_pickaxe_gated_block.
	struct pickaxe_block_case glowstone_case
		= {&block_glowstone, "glowstone", 450};
	check_pickaxe_gated_block(glowstone_case);
}

// ------------------------------------------------------------------------
// (3) Wooden pressure plate: tool-free flat harvest, ANY tool or bare hand.
// ------------------------------------------------------------------------
static void check_wooden_plate_speed(void) {
	int hand = tool_dig_delay_ms(&block_wooden_pressure_plate, NULL);
	int pickaxe
		= tool_dig_delay_ms(&block_wooden_pressure_plate, tool_item(ITEM_WOOD_PICKAXE));
	int axe = tool_dig_delay_ms(&block_wooden_pressure_plate, tool_item(ITEM_WOOD_AXE));
	if(hand != 750 || pickaxe != 750 || axe != 750) {
		fprintf(stderr,
				"test_dig_speed_batch_h: wooden pressure plate speed wrong -- "
				"hand=%d pickaxe=%d axe=%d (want all 750: Material.wood is "
				"unconditionally harvestable, so EVERY tool -- including bare "
				"hands -- takes the flat hardness/30 branch, no penalty)\n",
				hand, pickaxe, axe);
		assert(0 && "wooden pressure plate must dig at the same flat rate for any tool");
	}
	printf("  wooden pressure plate: hand/pickaxe/axe = %dms flat (tool-free "
		   "harvest, Material.wood) OK\n",
		   hand);
}

// ------------------------------------------------------------------------
// Live-server drop gate: a bare-handed break must both break AND drop the
// plate (server_local.c's digging.min==ANY/digging.tool==ANY disjunction).
// ------------------------------------------------------------------------
static void set_block(int x, int y, int z, int type) {
	struct block_data blk = {.type = (uint8_t)type, .metadata = 0};
	server_world_set_block(&s.worlds[0], x, y, z, blk);
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	return blk.type;
}

static void give(uint16_t id) {
	inventory_set_hotbar(&s.player.inventory, 0);
	if(id == 0)
		inventory_clear_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR);
	else
		inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
						   (struct item_data) {.id = id, .count = 1, .durability = 0});
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
	for(size_t i = 0; i < n; i++) {
		struct entity** pp = dict_entity_get(s.entities, keys[i]);
		if(pp && *pp && (*pp)->type == ENTITY_ITEM) {
			struct entity* e = *pp;
			if(out)
				*out = e->data.item.item;
			free(e);
			dict_entity_erase(s.entities, keys[i]);
			return true;
		}
	}
	return false;
}

static void check_wooden_plate_bare_hand_drop(void) {
	set_block(5, 70, 5, BLOCK_WOODEN_PLATE);
	give(0); // bare hand
	dig(5, 70, 5);
	assert(type_at(5, 70, 5) == BLOCK_AIR && "the plate must break bare-handed");

	struct item_data d;
	bool got = take_one_item_drop(&d);
	if(!got) {
		fprintf(stderr,
				"test_dig_speed_batch_h: wooden pressure plate did NOT drop when "
				"broken bare-handed -- Batch H item 3's harvest fix regressed\n");
		assert(0 && "bare-handed break must still drop the plate");
	}
	assert(d.id == BLOCK_WOODEN_PLATE && d.count == 1);
	printf("  wooden pressure plate: bare-handed break drops the plate "
		   "(id=%d count=%d) OK\n",
		   d.id, d.count);
}

int main(void) {
	test_fixture_dir("test_dig_speed_batch_h_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_dig_speed_batch_h: Batch H items 1-3 -- obsidian 15000/ANY, "
		   "the 9-block .best->ANY sweep, wooden plate tool-free harvest\n");

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	check_obsidian();
	check_nine_block_sweep();
	check_wooden_plate_speed();

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
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	check_wooden_plate_bare_hand_drop();

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_dig_speed_batch_h: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
