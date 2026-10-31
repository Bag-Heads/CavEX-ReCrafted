/*
	FH-01 REWRITTEN, then CORRECTED (docs/fork_hack_audit_2026-08-21.md, "FH-01 IS
	BACKWARDS -- DO NOT IMPLEMENT IT AS WRITTEN"; PROJECT_MEMORY.md Batch B item 4):
	redstone ore digging has TWO Beta mechanisms, both cited below, and this test pins
	their combined result rather than either one alone.

	Beta ground truth [740c583:1.7.3-LTS]:
	  - Block.blockStrength(EntityPlayer) [Block.java:327-333]:
	      !canHarvestBlock(this) ? 1.0F/blockHardness/100.0F
	                              : getCurrentPlayerStrVsBlock(this)/blockHardness/30.0F
	    i.e. a block you CANNOT harvest digs at a flat 1/100 rate; a block you CAN
	    harvest digs at your held item's getStrVsBlock() applied to a 1/30 rate --
	    100/30 = 10/3, the "wrong tool" penalty ratio.
	  - EntityPlayer.canHarvestBlock -> InventoryPlayer.canHarvestBlock:
	    `var1.blockMaterial.getIsHarvestable() ? true : (held item != null ?
	    item.canHarvestBlock(var1) : false)`. Material.rock calls setNoHarvest()
	    [Material.java], so redstone ore falls straight to the held item's own
	    canHarvestBlock -- false for an empty hand.
	  - ItemPickaxe.canHarvestBlock: `oreRedstone`/`oreRedstoneGlowing` require
	    `toolMaterial.getHarvestLevel() >= 2` -- iron and diamond only; wood, stone,
	    and gold (harvest level 0/1/0) fail, landing in the SAME "cannot harvest"
	    bucket as a bare hand.
	  - EntityPlayer.getCurrentPlayerStrVsBlock -> InventoryPlayer.getStrVsBlock ->
	    the held ItemStack's getStrVsBlock -> ItemTool.getStrVsBlock, which loops
	    the STATIC `blocksEffectiveAgainst` array and returns the tier's
	    efficiencyOnProperMaterial ONLY on a match, else a flat 1.0F. That array
	    OMITS `oreRedstone`/`oreRedstoneGlowing` entirely, so a QUALIFYING (iron or
	    diamond) pickaxe still gets no per-tier speed bonus beyond the flat 1.0F --
	    iron and diamond dig at the exact same speed as each other.
	  - ItemPickaxe.canHarvestBlock gates the DROP the same way (harvest level >= 2).
	  - BlockRedstoneOre.idDropped/quantityDropped: Item.redstone.shiftedIndex,
	    `4 + nextInt(2)` (4 or 5), one item-entity per unit (Block.dropBlockAsItem_do).

	Combined result, with the port's own hardness-field scale (hardness_field =
	real_beta_hardness * 1500 = real_hardness * 30 ticks * 50ms/tick -- verified
	against block_stone.c: 2250 = 1.5 * 1500):
	  - Iron and Diamond pickaxe: CAN harvest, flat 1.0F multiplier -> hardness*30
	    ticks = 4500ms (90 ticks @ 4500/50). IRON == DIAMOND, not diamond faster.
	  - Hand, Wood, Stone, Gold pickaxe: CANNOT harvest -> hardness*100 ticks =
	    15000ms (300 ticks). All four equal (this is a MATERIAL/harvest-level gate,
	    not a "matches the tool TYPE" gate -- wood/stone/gold pickaxes fail exactly
	    like a bare hand, because ItemPickaxe.canHarvestBlock rejects them by tier).
	  - Drop: iron/diamond -> redstone dust (4 or 5, one entity per unit); everyone
	    else -> nothing.

	The FIRST attempt at this fix added a `.digging.flat_speed` escape hatch (WRONG:
	missed that Beta already gives iron/diamond a real speed ADVANTAGE over the
	wrong-tool case via the /30-vs-/100 divisor split -- flat_speed made them equal
	to hand/wood/stone/gold too). An orchestrator correction then proposed reverting
	fully to the port's pre-existing `.min=IRON, .best=IRON` (WRONG: that caps BOTH
	iron and diamond through tool_tier_divider(IRON)=6, giving 750ms, not the
	Beta-faithful 4500ms -- it silently reintroduced a 6x-too-fast bug). The value
	that actually reproduces Block.blockStrength's flat-1.0F/30 branch, verified by
	hand against the port's own scale constant, is `.min=IRON` (harvest gate,
	unchanged) with `.best=TOOL_TIER_ANY` (caps both qualifying tiers to
	tool_tier_divider's no-bonus default of 1, not IRON's own divider of 6) -- see
	source/block/block_ore.c for the applied fix. No blocks.h/tool.c changes were
	needed: the EXISTING two-field mechanism already expresses this combination.

	Three groups:
	  (1) SPEED -- tool_dig_delay_ms(&block_redstoneore / _lit, ...): iron ==
	      diamond == 4500ms; hand == wood == stone == gold == 15000ms (a pure
	      function; no server needed).
	  (2) DROP GATE -- driving the real SRPC_BLOCK_DIG path over a live server_local:
	      iron/diamond pickaxe yield redstone dust (4 or 5, one count=1 entity per
	      unit -- Beta's dropBlockAsItem_do), hand/wood/stone/gold yield nothing.
	      Covers both the unlit (73) and lit (74) block ids.
	  (3) REGRESSION GUARD -- diamond ore (block id 56, untouched by this fix, still
	      `.min=IRON .best=TOOL_TIER_MAX`) keeps its REAL per-tier speed advantage
	      (it IS a blocksEffectiveAgainst member) and its iron+ drop gate.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_redstone_ore_dig_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define BLOCK_REDSTONE_ORE     73
#define BLOCK_REDSTONE_ORE_LIT 74
#define BLOCK_DIAMOND_ORE      56

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

// ------------------------------------------------------------------------
// (1) SPEED: tool_dig_delay_ms is a pure function of (block, held item) --
// no server/world needed.
// ------------------------------------------------------------------------

static struct item* pickaxe(uint16_t id) {
	struct item_data d = {.id = id, .count = 1};
	struct item* it = item_get(&d);
	assert(it && "pickaxe item id not registered");
	return it;
}

// Iron and diamond CAN harvest (ItemPickaxe.canHarvestBlock: level >= 2) and share
// the SAME flat speed (Block.blockStrength's /30 branch with getStrVsBlock's flat
// 1.0F -- oreRedstone is omitted from blocksEffectiveAgainst, so no per-tier bonus).
// Hand/wood/stone/gold CANNOT harvest (level 0/1/0/none < 2) and share the SAME
// slower flat speed (the /100 branch) -- a harvest-level gate, not a tool-type gate.
static void check_speed_matrix_for_block(struct block* ore, const char* name) {
	int hand = tool_dig_delay_ms(ore, NULL);
	int wood = tool_dig_delay_ms(ore, pickaxe(ITEM_WOOD_PICKAXE));
	int stone = tool_dig_delay_ms(ore, pickaxe(ITEM_STONE_PICKAXE));
	int iron = tool_dig_delay_ms(ore, pickaxe(ITEM_IRON_PICKAXE));
	int gold = tool_dig_delay_ms(ore, pickaxe(ITEM_GOLD_PICKAXE));
	int diamond = tool_dig_delay_ms(ore, pickaxe(ITEM_DIAMOND_PICKAXE));

	if(hand != 15000 || wood != 15000 || stone != 15000 || gold != 15000) {
		fprintf(stderr,
				"test_redstone_ore_dig: %s cannot-harvest speed wrong -- hand=%d "
				"wood=%d stone=%d gold=%d (want all 15000, Block.blockStrength's "
				"1/hardness/100 branch)\n",
				name, hand, wood, stone, gold);
		assert(0 && "hand/wood/stone/gold must dig at the slow cannot-harvest rate");
	}
	if(iron != 4500 || diamond != 4500) {
		fprintf(stderr,
				"test_redstone_ore_dig: %s can-harvest speed wrong -- iron=%d "
				"diamond=%d (want both 4500, Block.blockStrength's "
				"getCurrentPlayerStrVsBlock/hardness/30 branch with the flat 1.0F "
				"blocksEffectiveAgainst-omission multiplier)\n",
				name, iron, diamond);
		assert(0 && "iron and diamond must dig at the same fast can-harvest rate");
	}
	printf("  %s: hand/wood/stone/gold = %dms (cannot-harvest, hardness*100), "
		   "iron/diamond = %dms (can-harvest, hardness*30, no per-tier bonus)\n",
		   name, hand, iron);
}

static void check_speed_matrix(void) {
	check_speed_matrix_for_block(&block_redstoneore, "redstone ore (unlit)");
	check_speed_matrix_for_block(&block_redstoneore_lit, "redstone ore (lit)");
}

// ------------------------------------------------------------------------
// (2) + (3): drive the real SRPC_BLOCK_DIG path over a live server_local.
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

static void tick(void) { server_world_tick(&s.world, &s); }

// id == 0 means "no tool" (clears the hotbar slot -- bare hand).
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

// Snapshot-then-search (same pattern as test_redstone_loop_close_drops_env.c's
// entity_tick_loop): finds at most one ENTITY_ITEM, returns its item stack, and
// consumes (frees + erases) it so the world is clean for the next dig.
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

// Beta's Block.dropBlockAsItem_do spawns ONE ENTITY PER UNIT (server_local's
// server_local_spawn_drop_unit/server_local_plan_drop_unit both hardcode
// drop.item.count = 1), not a single stacked entity -- so redstone ore's
// quantityDropped 4-or-5 shows up as 4 or 5 separate count=1 entities.
// Drains every ENTITY_ITEM currently in the world, asserting each one carries
// exactly 1 unit of `want_id` (id is ignored, only count checked, when
// want_id == 0), and returns how many were found.
static int take_all_item_drops(uint16_t want_id) {
	int total = 0;
	struct item_data d;
	while(take_one_item_drop(&d)) {
		assert(d.count == 1
			   && "each per-unit drop entity must carry exactly 1 (Beta's "
				  "dropBlockAsItem_do spawns one entity per unit)");
		if(want_id != 0)
			assert(d.id == want_id && "unexpected item id among the drops");
		total++;
	}
	return total;
}

// Places `block_type` at (x,y,z), digs it with hotbar item `tool_id` (0 = bare
// hand), and asserts the drop count matches `want_total` exactly -- consuming
// every drop either way so the fixture is clean for the caller's next trial.
static void assert_dig_drop_count(int x, int y, int z, int block_type,
								  uint16_t tool_id, uint16_t want_item_id,
								  int want_total, const char* trial_name) {
	set_block(x, y, z, block_type);
	give(tool_id);
	dig(x, y, z);
	assert(type_at(x, y, z) == BLOCK_AIR && "the block must break regardless of drop");

	int got = take_all_item_drops(want_item_id);
	if(got != want_total) {
		fprintf(stderr,
				"test_redstone_ore_dig: %s -- dropped %d units, want %d\n",
				trial_name, got, want_total);
		assert(0 && "harvest-gate drop-count mismatch");
	}
}

static void check_redstone_drop_gate_for_block(int block_type, const char* label,
											   int x) {
	// wood/stone/gold pickaxe (harvest level 0/1/0, all < 2): no drop.
	assert_dig_drop_count(x, 70, 10, block_type, ITEM_WOOD_PICKAXE, 0, 0,
						  "wood pickaxe on redstone ore");
	assert_dig_drop_count(x, 70, 11, block_type, ITEM_STONE_PICKAXE, 0, 0,
						  "stone pickaxe on redstone ore");
	assert_dig_drop_count(x, 70, 12, block_type, ITEM_GOLD_PICKAXE, 0, 0,
						  "gold pickaxe on redstone ore");
	// bare hand: no drop either (Material.rock + no item -> canHarvestBlock false).
	assert_dig_drop_count(x, 70, 13, block_type, 0, 0, 0,
						  "bare hand on redstone ore");

	// iron/diamond pickaxe (harvest level 2/3, both >= 2): redstone dust,
	// quantityDropped = 4 + nextInt(2) -> 4 or 5 SEPARATE count=1 entities.
	set_block(x, 70, 14, block_type);
	give(ITEM_IRON_PICKAXE);
	dig(x, 70, 14);
	int iron_n = take_all_item_drops(ITEM_REDSTONE);
	if(iron_n != 4 && iron_n != 5) {
		fprintf(stderr,
				"test_redstone_ore_dig: iron pickaxe on %s dropped %d units, want "
				"4 or 5\n",
				label, iron_n);
		assert(0 && "iron pickaxe redstone dust count out of range");
	}

	set_block(x, 70, 15, block_type);
	give(ITEM_DIAMOND_PICKAXE);
	dig(x, 70, 15);
	int diamond_n = take_all_item_drops(ITEM_REDSTONE);
	if(diamond_n != 4 && diamond_n != 5) {
		fprintf(stderr,
				"test_redstone_ore_dig: diamond pickaxe on %s dropped %d units, "
				"want 4 or 5\n",
				label, diamond_n);
		assert(0 && "diamond pickaxe redstone dust count out of range");
	}

	printf("  %s: wood/stone/gold/hand -> nothing; iron -> %d dust, diamond -> %d "
		   "dust\n",
		   label, iron_n, diamond_n);
}

static void check_drop_gate(void) {
	check_redstone_drop_gate_for_block(BLOCK_REDSTONE_ORE, "redstone ore (unlit, id 73)",
									   20);
	check_redstone_drop_gate_for_block(BLOCK_REDSTONE_ORE_LIT,
									   "redstone ore (lit, id 74)", 24);
}

// (3) REGRESSION GUARD: diamond ore is untouched by this fix (`.min=IRON
// .best=TOOL_TIER_MAX`, unlike redstone ore's `.best=TOOL_TIER_ANY`) -- it IS a
// blocksEffectiveAgainst member, so it keeps a REAL per-tier speed advantage
// (diamond faster than iron), and its drop gate stays iron+.
static void check_diamond_ore_unchanged(void) {
	int wood = tool_dig_delay_ms(&block_diamondore, pickaxe(ITEM_WOOD_PICKAXE));
	int iron = tool_dig_delay_ms(&block_diamondore, pickaxe(ITEM_IRON_PICKAXE));
	int diamond = tool_dig_delay_ms(&block_diamondore, pickaxe(ITEM_DIAMOND_PICKAXE));
	if(wood != 15000) {
		fprintf(stderr,
				"test_redstone_ore_dig: diamond ore speed regressed -- wood=%d, "
				"want 15000 (cannot-harvest branch, unchanged)\n",
				wood);
		assert(0 && "diamond ore's wrong-tier speed changed");
	}
	if(iron != 750) {
		fprintf(stderr,
				"test_redstone_ore_dig: diamond ore speed regressed -- iron=%d, "
				"want 750 (hardness/tool_tier_divider(IRON)=6, unchanged)\n",
				iron);
		assert(0 && "diamond ore's iron-pickaxe speed changed");
	}
	if(diamond != 600) {
		fprintf(stderr,
				"test_redstone_ore_dig: diamond ore speed regressed -- diamond=%d, "
				"want 600 (hardness/tool_tier_divider(DIAMOND)=8, unchanged -- "
				"diamond ore IS a blocksEffectiveAgainst member, so diamond really "
				"is faster than iron here, unlike redstone ore)\n",
				diamond);
		assert(0 && "diamond ore's diamond-pickaxe speed changed");
	}
	assert(diamond < iron
		   && "diamond pickaxe must still be strictly faster than iron on diamond "
			  "ore (its own tier bonus applies -- unlike redstone ore)");
	assert(block_diamondore.digging.best == TOOL_TIER_MAX
		   && "diamond ore must not have picked up redstone ore's TOOL_TIER_ANY cap");

	// Drop gate: wood -> nothing, iron -> exactly 1 diamond (unchanged
	// harvest-level-2 gate; drop_diamond's quantityDropped is a fixed 1, unlike
	// redstone's 4-or-5, so this stays a single-entity check).
	assert_dig_drop_count(40, 70, 10, BLOCK_DIAMOND_ORE, ITEM_WOOD_PICKAXE, 0, 0,
						  "wood pickaxe on diamond ore (regression)");
	assert_dig_drop_count(40, 70, 11, BLOCK_DIAMOND_ORE, ITEM_IRON_PICKAXE,
						  ITEM_DIAMOND, 1, "iron pickaxe on diamond ore (regression)");

	printf("  diamond ore regression guard: wood=%d, iron=%d, diamond=%d (diamond "
		   "still strictly fastest -- real per-tier bonus, unlike redstone), drop "
		   "gate still iron+ OK\n",
		   wood, iron, diamond);
}

// ------------------------------------------------------------------------
// (4) GLOW/REVERT: the onNeighbourBlockChange-shaped fix's OTHER half --
// BlockRedstoneOre.func_320_h (right-click glow) + updateTick (RANDOM-tick
// revert). [decompile 740c583 BlockRedstoneOre]
//
// next-gaps-0924/W2-R1b: the old expectation here ("reverts on exactly the
// 30th tick") was NOT Beta. Nothing in 1.7.3 ever schedules a tick for the ore
// (no scheduleBlockUpdate in BlockRedstoneOre; World.setBlockWithNotify
// schedules nothing). The lit ore is `new BlockRedstoneOre(74, 51, true)` ->
// setTickOnLoad(true) [BlockRedstoneOre.java:8-14, Block.java:218], and the
// ONLY caller of updateTick for such a block is World.updateBlocksAndPlayCave-
// Sounds' random tick: 80 samples per chunk per tick, position from the
// field_9437_g LCG, `if(Block.tickOnLoad[id]) blocksList[id].updateTick(...,
// this.rand)` [World.java:1945-1956]. updateTick [BlockRedstoneOre.java:41-46]
// draws nothing from its Random. tickRate()==30 is dead for this block. The
// unlit id (73) is NOT tickOnLoad, so it never random-ticks.
// ------------------------------------------------------------------------
static struct block_info ore_info(int x, int y, int z, struct block_data* out) {
	assert(server_world_get_block(&s.world, x, y, z, out));
	return (struct block_info) {.block = out, .x = x, .y = y, .z = z};
}

static void right_click_ore(int x, int y, int z) {
	struct block_data bd;
	struct block_info bi = ore_info(x, y, z, &bd);
	block_redstoneore.onRightClick(&s, &s.player, NULL, &bi, &bi, SIDE_TOP);
}

static void check_glow_and_revert(void) {
	// server_world_schedule_tick gates on server_world_scheduled_tick_area_loaded
	// (a radius-8-BLOCK checkChunksExist, [decompile-equivalent] World's own
	// scheduled-tick admission gate) -- must stay >=8 blocks from any unloaded
	// chunk edge (only cx 0..2, cz 0..1 are loaded here) or the schedule is
	// silently refused and the ore would glow forever.
	int x = 24, y = 90, z = 8;

	// Right-click an UNLIT ore -> glows immediately (synchronous, like Beta's
	// blockActivated -> func_320_h -> setBlockWithNotify, no tick delay).
	set_block(x, y, z, BLOCK_REDSTONE_ORE);
	right_click_ore(x, y, z);
	assert(type_at(x, y, z) == BLOCK_REDSTONE_ORE_LIT
		   && "right-click must glow an unlit redstone ore immediately");

	// Right-clicking an ALREADY-lit ore is a no-op (Beta's func_320_h only
	// swaps when `this.blockID == oreRedstone.blockID`; the lit variant fails
	// that check, so blockActivated has no server-visible effect on it).
	right_click_ore(x, y, z);
	assert(type_at(x, y, z) == BLOCK_REDSTONE_ORE_LIT
		   && "right-clicking an already-lit ore must stay lit (no-op)");

	// No scheduled revert exists in Beta: hundreds of world ticks (the
	// scheduled-tick pass, no random ticks) leave it lit.
	for(int i = 0; i < 300; i++)
		tick();
	assert(type_at(x, y, z) == BLOCK_REDSTONE_ORE_LIT
		   && "Beta schedules no revert: scheduled ticks alone must never unlight it");

	// Block.tickOnLoad: only the LIT id random-ticks.
	assert(blocks[BLOCK_REDSTONE_ORE_LIT]->onRandomTick
		   && "oreRedstoneGlowing is setTickOnLoad(true)");
	assert(!blocks[BLOCK_REDSTONE_ORE]->onRandomTick
		   && "oreRedstone is not tickOnLoad: no random tick");

	// updateTick itself: revert to 73 and draw NOTHING from the world RNG
	// (production passes s->rand_src as the random-tick generator, so a draw
	// here would shift every later world.rand consumer).
	{
		struct random_gen before = s.rand_src;
		struct block_data bd;
		struct block_info bi = ore_info(x, y, z, &bd);
		blocks[BLOCK_REDSTONE_ORE_LIT]->onRandomTick(&s, &bi);
		assert(type_at(x, y, z) == BLOCK_REDSTONE_ORE
			   && "BlockRedstoneOre.updateTick must revert the glow");
		assert(memcmp(&before, &s.rand_src, sizeof before) == 0
			   && "updateTick draws no world RNG (Beta ignores its Random)");
	}

	// Production path: re-light, then drive the real random-tick sampler over
	// the loaded chunks until a sample lands on the ore. Expected ~32768/80 =
	// 410 sampler calls; the bound is far past any plausible streak.
	right_click_ore(x, y, z);
	assert(type_at(x, y, z) == BLOCK_REDSTONE_ORE_LIT);
	int calls = 0;
	while(type_at(x, y, z) == BLOCK_REDSTONE_ORE_LIT && calls < 20000) {
		server_world_random_tick(&s.worlds[0], &s.rand_src, &s, 1, 0, 2);
		calls++;
	}
	assert(type_at(x, y, z) == BLOCK_REDSTONE_ORE
		   && "a random tick landing on the lit ore must revert it");

	printf("  glow/revert: right-click lights it immediately, re-click on a lit "
		   "ore is a no-op, 300 scheduled ticks leave it lit, updateTick reverts "
		   "with 0 world-RNG draws, random-tick sampler reverted it after %d "
		   "calls OK\n", calls);
}

int main(void) {
	test_fixture_dir("test_redstone_ore_dig_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_redstone_ore_dig: FH-01 corrected -- Block.blockStrength's "
		   "100-vs-30 divisor split, iron==diamond, gated drop\n");
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	check_speed_matrix();

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
	s.player.health = MAX_PLAYER_HEALTH; // zero-init health reads as dead
	s.player.active_inventory = &s.player.inventory;

	// x 0..7 and 20..31 both fit chunk columns (0,0)/(1,0); the diamond-ore
	// regression fixtures at x=40 need column (2,0) too.
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 2; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	check_drop_gate();
	check_diamond_ore_unchanged();
	check_glow_and_revert();

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_redstone_ore_dig: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
