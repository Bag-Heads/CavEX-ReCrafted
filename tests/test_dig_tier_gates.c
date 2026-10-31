/*
	parity_audit_2026-08-25 lower-tier trio (verified against the decompile
	before fixing -- the audit swept these but did not verify them):

	(1) Gold ore (block_ore.c): .digging.min STONE->IRON.
	    ItemPickaxe.canHarvestBlock [740c583]: oreGold/blockGold need
	    harvestLevel >= 2 -- IRON(2)/DIAMOND(3) only; stone is level 1,
	    wood/gold level 0. oreGold IS in blocksEffectiveAgainst, so the
	    qualifying tiers keep their own efficiencyOnProperMaterial
	    (.best = MAX). A below-min dig takes Block.blockStrength's
	    cannot-harvest branch [Block.java:331], which is a LITERAL
	    1.0F/hardness/100F -- the tool's efficiency does NOT apply there in
	    Beta 1.7.3 (that arrived in later versions), so tool.c's flat *10/3
	    penalty is byte-faithful. Hardness: Block.java:606 setHardness(3.0F)
	    -> 4500ms at the FH-01 scale.

	(2) Block of Iron (block_cast_block.c): .digging.min IRON->STONE.
	    ItemPickaxe.canHarvestBlock: blockSteel/oreIron need harvestLevel
	    >= 1 -- STONE and up. The old gate refused the stone pickaxe Beta
	    accepts: the opposite-direction twin of (1). Hardness: Block.java:634
	    setHardness(5.0F) -> 7500ms.

	(3) Farmland (block_farmland.c): .digging.tool ANY->SHOVEL,
	    .best ANY->MAX. Block.tilledField is an ItemSpade
	    blocksEffectiveAgainst member [ItemSpade.java static init:
	    grass/dirt/sand/gravel/snow/blockSnow/blockClay/tilledField], the
	    same registration block_dirt.c already carries. .min stays ANY:
	    Material.ground is never tool-gated, so bare hands keep the
	    unpenalized flat branch AND the drop. Hardness: Block.java:652
	    setHardness(0.6F) -> 900ms.

	Every pinned ms value below equals Beta's per-tick damage accumulation
	(ceil(1/strength) ticks * 50ms): tool.c's integer divide + round-up-to-
	tick reproduces the Beta tick count in every cell of the matrix.

	Live-server drop-gate checks (server_local.c's dig finish disjunction --
	`(tool matches && tier >= min) || min==ANY || tool==ANY`):
	  - gold ore broken with a STONE pickaxe breaks but must NOT drop
	    (the old min=STONE wrongly dropped it -- free gold);
	  - gold ore broken with an IRON pickaxe drops itself;
	  - Block of Iron broken with a STONE pickaxe drops itself
	    (the old min=IRON wrongly withheld it);
	  - farmland broken bare-handed drops DIRT (Beta idDropped).

	SWORD DIG-SPEED FIX (item 1, this lane, verified 740c583):
	tool.c's `else if(item_type == TOOL_TYPE_SWORD) delay = delay*2/3;`
	unconditionally gave EVERY block a sword speed bonus, including blocks
	that REQUIRE a tool (rock/iron/snow/ore -- Material.setNoHarvest()).
	Ground truth:
	  - ItemSword.canHarvestBlock [ItemSword.java]: `return var1.blockID ==
	    Block.web.blockID;` -- true ONLY for cobweb. Everywhere else,
	    InventoryPlayer.canHarvestBlock is false for a tool-gated material
	    (Material.getIsHarvestable()==false and the sword's own
	    canHarvestBlock is false too), so Block.blockStrength
	    [Block.java:327-333] takes the SAME "cannot harvest" branch
	    (1.0F/hardness/100.0F) a bare hand does -- a sword must get NO bonus
	    on gold ore / Block of Iron / stone, matching tool.c's *10/3 penalty
	    exactly like a bare hand or wrong-type tool.
	  - ItemSword.getStrVsBlock [ItemSword.java]: `return var2.blockID ==
	    Block.web.blockID ? 15.0F : 1.5F;` -- on a block the material
	    already lets ANY item harvest (digging.min==ANY, e.g. dirt/farmland),
	    canHarvestBlock is true regardless of held item, so the sword's flat
	    1.5F strVsBlock IS a real (if small) bonus: delay*2/3. This part of
	    the old code was correct and is preserved/tested below as a
	    regression, not re-derived.
	  - On cobweb (block_cobweb.c: digging.tool=SWORD, min=best=WOOD), a
	    sword of ANY tier gets a FLAT 15.0F (not tier-scaled like
	    ItemPickaxe/ItemSpade/ItemAxe's efficiencyOnProperMaterial) --
	    tool.c's matched-tool branch now divides by a flat 15 instead of
	    tool_tier_divider() for TOOL_TYPE_SWORD. Cobweb hardness_field=6000
	    (4.0F*1500, block_cobweb.c) / 15 = 400ms = 8 ticks, matching Beta's
	    web-with-sword ground truth exactly (was 3000ms via the tier-2
	    divider -- 7.5x too slow).
	See check_sword_regression_matrix() and the added sword rows on the
	three matrices above.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_dig_tier_gates_tmp' (a literal) */
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

static struct item* tool_item(uint16_t id) {
	struct item_data d = {.id = id, .count = 1};
	struct item* it = item_get(&d);
	assert(it && "tool item id not registered");
	return it;
}

static void check_ms(struct block* blk, const char* blk_name, uint16_t tool_id,
					 const char* tool_name, int want) {
	struct item* it = tool_id ? tool_item(tool_id) : NULL;
	int got = tool_dig_delay_ms(blk, it);
	if(got != want) {
		fprintf(stderr,
				"test_dig_tier_gates: %s with %s = %dms, want %dms "
				"(Beta-derived tick count * 50)\n",
				blk_name, tool_name, got, want);
		assert(0 && "dig delay diverges from the Beta-derived golden");
	}
	printf("  %s + %s = %dms OK\n", blk_name, tool_name, got);
}

/* Gold ore: hardness 4500, PICKAXE, min IRON, best MAX.
 * Cannot-harvest (hand/wood/gold/stone) = 4500*10/3 = 15000ms (Beta 300
 * ticks: 1.0F/3.0/100). Iron = 4500/6 = 750ms (15 ticks). Diamond =
 * 4500/8 -> 600ms rounded to tick (Beta ceil(1/(8/3/30)) = 12 ticks). */
static void check_gold_ore_matrix(void) {
	check_ms(&block_goldore, "gold ore", 0, "bare hand", 15000);
	check_ms(&block_goldore, "gold ore", ITEM_WOOD_PICKAXE, "wood pickaxe",
			 15000);
	check_ms(&block_goldore, "gold ore", ITEM_GOLD_PICKAXE, "gold pickaxe",
			 15000);
	check_ms(&block_goldore, "gold ore", ITEM_STONE_PICKAXE, "stone pickaxe",
			 15000);
	check_ms(&block_goldore, "gold ore", ITEM_IRON_PICKAXE, "iron pickaxe",
			 750);
	check_ms(&block_goldore, "gold ore", ITEM_DIAMOND_PICKAXE,
			 "diamond pickaxe", 600);
	// Sword regression: ItemSword.canHarvestBlock is false for ore (not
	// web), so a sword must get NO bonus here -- same 15000ms as bare hand,
	// NOT the old bug's 4500*2/3=3000ms free speed-up.
	check_ms(&block_goldore, "gold ore", ITEM_WOOD_SWORD, "wood sword",
			 15000);
	check_ms(&block_goldore, "gold ore", ITEM_DIAMOND_SWORD, "diamond sword",
			 15000);
}

/* Block of Iron: hardness 7500, PICKAXE, min STONE, best MAX.
 * Cannot-harvest (hand/wood/gold) = 25000ms (Beta 500 ticks). Stone =
 * 7500/4 -> 1900ms (38 ticks), iron = 1250ms (25 ticks), diamond =
 * 7500/8 -> 950ms (19 ticks). */
static void check_iron_block_matrix(void) {
	check_ms(&block_iron, "iron block", 0, "bare hand", 25000);
	check_ms(&block_iron, "iron block", ITEM_WOOD_PICKAXE, "wood pickaxe",
			 25000);
	check_ms(&block_iron, "iron block", ITEM_GOLD_PICKAXE, "gold pickaxe",
			 25000);
	check_ms(&block_iron, "iron block", ITEM_STONE_PICKAXE, "stone pickaxe",
			 1900);
	check_ms(&block_iron, "iron block", ITEM_IRON_PICKAXE, "iron pickaxe",
			 1250);
	check_ms(&block_iron, "iron block", ITEM_DIAMOND_PICKAXE,
			 "diamond pickaxe", 950);
	// Sword regression: same 25000ms as bare hand, not the old bug's
	// 7500*2/3=5000ms free speed-up.
	check_ms(&block_iron, "iron block", ITEM_WOOD_SWORD, "wood sword",
			 25000);
}

/* Farmland: hardness 900, SHOVEL, min ANY, best MAX.
 * Bare hand and any non-shovel = flat 900ms (18 ticks -- min==ANY means no
 * penalty branch, matching Material.ground's unconditional harvestability).
 * Shovels get their tier's efficiency: wood 450 (9 ticks), stone 250
 * (5 ticks), iron 150 (3 ticks), diamond 150 (3 ticks), gold 100 (2 ticks). */
static void check_farmland_matrix(void) {
	check_ms(&block_farmland, "farmland", 0, "bare hand", 900);
	check_ms(&block_farmland, "farmland", ITEM_WOOD_PICKAXE, "wood pickaxe",
			 900);
	check_ms(&block_farmland, "farmland", ITEM_WOOD_SHOVEL, "wood shovel",
			 450);
	check_ms(&block_farmland, "farmland", ITEM_STONE_SHOVEL, "stone shovel",
			 250);
	check_ms(&block_farmland, "farmland", ITEM_IRON_SHOVEL, "iron shovel",
			 150);
	check_ms(&block_farmland, "farmland", ITEM_DIAMOND_SHOVEL,
			 "diamond shovel", 150);
	check_ms(&block_farmland, "farmland", ITEM_GOLD_SHOVEL, "gold shovel",
			 100);
	// Sword regression (min==ANY, hand-harvestable): the *2/3 bonus IS
	// correct here (ItemSword.getStrVsBlock's flat 1.5F applies -- material
	// is harvestable by anything), so this must stay 600ms, unchanged by
	// the fix -- proves the fix only removed the bonus from tool-GATED
	// blocks, not from hand-harvestable ones.
	check_ms(&block_farmland, "farmland", ITEM_WOOD_SWORD, "wood sword",
			 600);
}

/* Cobweb (block_cobweb.c): hardness 6000, tool SWORD, min=best=WOOD -- the
 * ONLY block whose digging.tool is SWORD, and the ONLY block
 * ItemSword.canHarvestBlock returns true for. A sword of ANY tier gets a
 * FLAT 15x (ItemSword.getStrVsBlock's `blockID==web ? 15.0F : 1.5F`, no
 * tier scaling): 6000/15 = 400ms (8 ticks), same for wood through diamond.
 * Bare hand and any non-sword tool take the "cannot harvest" *10/3 penalty,
 * same as any other tool-gated block: 6000*10/3 = 20000ms. */
static void check_cobweb_matrix(void) {
	check_ms(&block_cobweb, "cobweb", ITEM_WOOD_SWORD, "wood sword", 400);
	check_ms(&block_cobweb, "cobweb", ITEM_STONE_SWORD, "stone sword", 400);
	check_ms(&block_cobweb, "cobweb", ITEM_IRON_SWORD, "iron sword", 400);
	check_ms(&block_cobweb, "cobweb", ITEM_GOLD_SWORD, "gold sword", 400);
	check_ms(&block_cobweb, "cobweb", ITEM_DIAMOND_SWORD, "diamond sword",
			 400);
	check_ms(&block_cobweb, "cobweb", 0, "bare hand", 20000);
	check_ms(&block_cobweb, "cobweb", ITEM_DIAMOND_PICKAXE,
			 "diamond pickaxe (wrong tool)", 20000);
}

/* Stone (block_stone.c): hardness 2250, tool PICKAXE, min WOOD. A sword
 * never matches PICKAXE, so ItemSword.canHarvestBlock (false, not web)
 * puts it on the SAME *10/3 cannot-harvest branch as bare hand: 7500ms.
 * This is the headline regression for the tool.c fix -- the old code gave
 * 2250*2/3=1500ms, a ~5x unfaithful mining speed-up with a sword. */
static void check_stone_sword_regression(void) {
	check_ms(&block_stone, "stone", 0, "bare hand", 7500);
	check_ms(&block_stone, "stone", ITEM_WOOD_SWORD, "wood sword", 7500);
	check_ms(&block_stone, "stone", ITEM_DIAMOND_SWORD, "diamond sword",
			 7500);
}

/* Stone qualifying-pickaxe matrix (block_stone.c: min WOOD, best MAX --
 * stone IS in ItemPickaxe.blocksEffectiveAgainst [740c583], so every tier
 * that clears .min gets its own EnumToolMaterial.getEfficiencyOnProperMaterial()
 * bonus via tool_tier_divider(), unlike gold-ore/iron-block's below-min
 * rejections tested above. These are the literal sword-dig-audit A3
 * headline numbers (stone.c:47-54 was the buggy branch order) and match the
 * well-known Beta/vanilla stone-mining-speed table exactly: wood 1.15s,
 * stone 0.6s, iron 0.4s, gold 0.2s (fastest -- EnumToolMaterial.GOLD's
 * efficiency is 12.0F despite harvestLevel 0), diamond 0.3s. */
static void check_stone_pickaxe_matrix(void) {
	check_ms(&block_stone, "stone", ITEM_WOOD_PICKAXE, "wood pickaxe", 1150);
	check_ms(&block_stone, "stone", ITEM_STONE_PICKAXE, "stone pickaxe", 600);
	check_ms(&block_stone, "stone", ITEM_IRON_PICKAXE, "iron pickaxe", 400);
	check_ms(&block_stone, "stone", ITEM_GOLD_PICKAXE, "gold pickaxe", 200);
	check_ms(&block_stone, "stone", ITEM_DIAMOND_PICKAXE, "diamond pickaxe",
			 300);
}

/* Dirt (block_dirt.c): hardness 750, tool SHOVEL, min ANY, best MAX --
 * Material.ground is never .setNoHarvest()'d, so bare hand and a mismatched
 * pickaxe both take the unpenalized flat branch (matches farmland's min==ANY
 * shape above, but dirt itself was never golden-pinned). A qualifying
 * shovel gets efficiencyOnProperMaterial via tool_tier_divider(), same
 * mechanism as farmland scaled to dirt's own hardness: wood 0.4s, stone
 * 0.2s, iron 0.15s, diamond/gold tie at 0.1s (post-round-up-to-tick). */
static void check_dirt_shovel_matrix(void) {
	check_ms(&block_dirt, "dirt", 0, "bare hand", 750);
	check_ms(&block_dirt, "dirt", ITEM_WOOD_PICKAXE, "wood pickaxe (mismatched)",
			 750);
	check_ms(&block_dirt, "dirt", ITEM_WOOD_SHOVEL, "wood shovel", 400);
	check_ms(&block_dirt, "dirt", ITEM_STONE_SHOVEL, "stone shovel", 200);
	check_ms(&block_dirt, "dirt", ITEM_IRON_SHOVEL, "iron shovel", 150);
	check_ms(&block_dirt, "dirt", ITEM_DIAMOND_SHOVEL, "diamond shovel", 100);
	check_ms(&block_dirt, "dirt", ITEM_GOLD_SHOVEL, "gold shovel", 100);
	// min==ANY (hand-harvestable): the sword's flat 1.5F strVsBlock still
	// applies here (same shape as check_farmland_matrix's sword row) --
	// 750*2/3 = 500, rounded to the tick.
	check_ms(&block_dirt, "dirt", ITEM_WOOD_SWORD, "wood sword", 500);
}

/* Log/"wood" (block_log.c): hardness 3000, tool AXE, min ANY, best MAX --
 * Block.wood IS in ItemAxe.blocksEffectiveAgainst [740c583] (planks,
 * bookShelf, wood, chest), so a qualifying axe gets the same
 * tool_tier_divider() bonus as stone's pickaxe matrix, scaled to log's own
 * hardness. Matches the well-known Beta/vanilla log-chopping table: hand/
 * mismatched-tool 3.0s (60 ticks -- the famous "punch a tree" number), wood
 * axe 1.5s, stone 0.75s, iron 0.5s, gold 0.25s, diamond 0.4s. */
static void check_log_axe_matrix(void) {
	check_ms(&block_log, "log", 0, "bare hand", 3000);
	check_ms(&block_log, "log", ITEM_WOOD_PICKAXE, "wood pickaxe (mismatched)",
			 3000);
	check_ms(&block_log, "log", ITEM_WOOD_AXE, "wood axe", 1500);
	check_ms(&block_log, "log", ITEM_STONE_AXE, "stone axe", 750);
	check_ms(&block_log, "log", ITEM_IRON_AXE, "iron axe", 500);
	check_ms(&block_log, "log", ITEM_GOLD_AXE, "gold axe", 250);
	check_ms(&block_log, "log", ITEM_DIAMOND_AXE, "diamond axe", 400);
	// min==ANY (hand-harvestable): sword's flat 1.5F bonus applies:
	// 3000*2/3 = 2000, rounded to the tick.
	check_ms(&block_log, "log", ITEM_WOOD_SWORD, "wood sword", 2000);
}

// ------------------------------------------------------------------------
// Live-server drop gates (pattern: test_dig_speed_batch_h.c).
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

static void check_drop_gate(int block_id, const char* blk_name,
							uint16_t tool_id, const char* tool_name,
							bool want_drop, int want_drop_id) {
	set_block(5, 70, 5, block_id);
	give(tool_id);
	dig(5, 70, 5);
	assert(type_at(5, 70, 5) == BLOCK_AIR && "the block must break");

	struct item_data d;
	bool got = take_one_item_drop(&d);
	if(got != want_drop) {
		fprintf(stderr,
				"test_dig_tier_gates: %s broken with %s %s -- Beta "
				"canHarvestBlock says it must%s\n",
				blk_name, tool_name, got ? "DROPPED" : "did NOT drop",
				want_drop ? "" : " NOT");
		assert(0 && "harvest/drop gate diverges from Beta canHarvestBlock");
	}
	if(want_drop) {
		assert(d.id == want_drop_id && d.count == 1);
		printf("  %s + %s: breaks and drops id=%d OK\n", blk_name, tool_name,
			   d.id);
	} else {
		printf("  %s + %s: breaks, no drop (below harvest tier) OK\n",
			   blk_name, tool_name);
	}
}

int main(void) {
	test_fixture_dir("test_dig_tier_gates_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_dig_tier_gates: gold ore min IRON, iron block min STONE, "
		   "farmland shovel class (parity_audit_2026-08-25 lower tier)\n");

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	check_gold_ore_matrix();
	check_iron_block_matrix();
	check_farmland_matrix();
	check_cobweb_matrix();
	check_stone_sword_regression();
	check_stone_pickaxe_matrix();
	check_dirt_shovel_matrix();
	check_log_axe_matrix();

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

	check_drop_gate(BLOCK_GOLD_ORE, "gold ore", ITEM_STONE_PICKAXE,
					"stone pickaxe", false, 0);
	check_drop_gate(BLOCK_GOLD_ORE, "gold ore", ITEM_IRON_PICKAXE,
					"iron pickaxe", true, BLOCK_GOLD_ORE);
	check_drop_gate(BLOCK_IRON_CAST, "iron block", ITEM_STONE_PICKAXE,
					"stone pickaxe", true, BLOCK_IRON_CAST);
	check_drop_gate(BLOCK_FARMLAND, "farmland", 0, "bare hand", true,
					BLOCK_DIRT);
	// Cobweb drop gate: ItemSword.canHarvestBlock is true ONLY for web, so
	// only a sword satisfies server_local.c's drop disjunction here (unlike
	// the timing bug above, this gate already keyed on digging.tool/min
	// correctly -- included to prove the full sword-vs-web path, speed AND
	// drop, now matches Beta end to end).
	check_drop_gate(BLOCK_COBWEB, "cobweb", ITEM_WOOD_SWORD, "wood sword",
					true, ITEM_STRING);
	check_drop_gate(BLOCK_COBWEB, "cobweb", 0, "bare hand", false, 0);

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_dig_tier_gates: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
