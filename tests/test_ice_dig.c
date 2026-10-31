/*
	Field report fix: (1) "breaking a block of ICE by hand takes far too long";
	(2) "breaking ice with a solid block underneath did NOT turn into water."
	Both are BlockIce-specific Beta 1.7.3 mechanisms, cited from the decompile
	and pinned here -- see source/block/block_ice.c for the full citations.

	(1) SPEED -- Block.blockStrength(EntityPlayer) [740c583: Block.java:327-333]:
	  !canHarvestBlock(this) ? 1.0F/hardness/100.0F
	                         : getCurrentPlayerStrVsBlock(this)/hardness/30.0F
	InventoryPlayer.canHarvestBlock: `blockMaterial.getIsHarvestable() ? true :
	(heldItem != null ? heldItem.canHarvestBlock(block) : false)`. Material.ice
	[740c583: Material.java static init] is a fresh Material with only setIsTranslucent()
	-- it never calls setNoHarvest(), so getIsHarvestable() defaults true and
	canHarvestBlock(ice) is ALWAYS true, even bare hand: ice always takes the
	fast /30 branch, never the /100 "wrong tool" penalty. Ice IS a member of
	ItemPickaxe.blocksEffectiveAgainst [740c583: ItemPickaxe.java static init],
	so a pickaxe still gets ItemTool.getStrVsBlock's flat per-tier
	efficiencyOnProperMaterial bonus (wood=2/stone=4/iron=6/diamond=8/gold=12).
	With the port's own hardness-field scale (hardness_field = real_hardness *
	1500 = real_hardness * 30 ticks * 50ms/tick -- verified against
	block_stone.c: 2250 = 1.5 * 1500), and hardness_field = 750 (0.5F * 1500):
	  hand:     750 / 1  = 750ms (undivided; hand doesn't match TOOL_TYPE_PICKAXE
	                        and .min=TOOL_TIER_ANY skips the penalty branch)
	                      = 15 ticks = 0.75s
	  wood:     750 / 2  = 375  -> round up to next tick ->  400ms =  8 ticks
	  stone:    750 / 4  = 187  -> round up               ->  200ms =  4 ticks
	  iron:     750 / 6  = 125  -> round up               ->  150ms =  3 ticks
	  gold:     750 / 12 =  62  -> round up               ->  100ms =  2 ticks
	  diamond:  750 / 8  =  93  -> round up               ->  100ms =  2 ticks
	(tool.c's tool_dig_delay_ms: `(delay + 49) / 50 * 50`, integer division
	throughout, matching the port's existing rounding convention.)

	(2) WATER -- BlockIce.harvestBlock [740c583: BlockIce.java]:
	  the base harvestBlock runs first;
	  then, when the material below, at (x, y - 1, z),
	  is solid or liquid,
	      the cell is set to waterMoving with notify.
	waterMOVING (flowing, id 8), NOT waterStill (unlike the separate melt path,
	BlockIce.updateTick, which sets waterStill and is already covered by
	tests/test_weather_snow_ice_env.c -- untouched by this fix and not
	re-tested here). harvestBlock is PLAYER-HARVEST ONLY (only reached from the
	digging RPC's harvest gate (removed AND canHarvest), canHarvest =
	canHarvestBlock -- always true for ice per (1) above), driven here over a
	live server_local via the real SRPC_BLOCK_DIG path (server_local_test_
	process_rpc), the same rig as tests/test_redstone_ore_dig.c.

	Three ground-below cases (stone/air/water) plus a below-air regression
	(must stay air, not spuriously become water) and a creative-mode instant-dig
	smoke check (docs/creative_mode.md: creative reuses this exact server path,
	just with a 1ms client dig delay -- no server-side special-casing, so no
	separate server behaviour to pin here beyond "the same conversion still
	happens").
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_ice_dig_tmp' (a literal) */
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

static void check_speed_matrix(void) {
	int hand = tool_dig_delay_ms(&block_ice, NULL);
	int wood = tool_dig_delay_ms(&block_ice, pickaxe(ITEM_WOOD_PICKAXE));
	int stone = tool_dig_delay_ms(&block_ice, pickaxe(ITEM_STONE_PICKAXE));
	int iron = tool_dig_delay_ms(&block_ice, pickaxe(ITEM_IRON_PICKAXE));
	int gold = tool_dig_delay_ms(&block_ice, pickaxe(ITEM_GOLD_PICKAXE));
	int diamond = tool_dig_delay_ms(&block_ice, pickaxe(ITEM_DIAMOND_PICKAXE));

	struct {
		const char* name;
		int got, want;
	} rows[] = {
		{"hand", hand, 750},   {"wood", wood, 400},	 {"stone", stone, 200},
		{"iron", iron, 150},   {"gold", gold, 100},	 {"diamond", diamond, 100},
	};
	for(size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		if(rows[i].got != rows[i].want) {
			fprintf(stderr,
					"test_ice_dig: %s dig delay = %dms, want %dms (Block."
					"blockStrength's /30 can-harvest branch, always taken for "
					"ice -- Material.ice never calls setNoHarvest())\n",
					rows[i].name, rows[i].got, rows[i].want);
			assert(0 && "ice dig-speed regression");
		}
	}
	// Hand must be dramatically faster than the old (wrong) 2500ms/50-tick
	// result -- this IS the field-reported bug, pinned directly.
	assert(hand == 750
		   && "bare-hand ice dig must be Beta's fast 15-tick harvestable rate, "
			  "not the 50-tick 'requires tool' penalty rate");
	// Pickaxe must still be a real speed ADVANTAGE over bare hand (ice is a
	// blocksEffectiveAgainst member, so the tier bonus is real, unlike e.g.
	// redstone ore's flat-1.0F omission).
	assert(wood < hand && diamond < wood
		   && "pickaxe tiers must still speed up ice digging monotonically");

	printf("  ice dig speeds: hand=%dms wood=%dms stone=%dms iron=%dms gold=%dms "
		   "diamond=%dms\n",
		   hand, wood, stone, iron, gold, diamond);
}

// ------------------------------------------------------------------------
// (2) WATER: drive the real SRPC_BLOCK_DIG path over a live server_local.
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

// Places ice at (x, y, z) over `below_type` at (x, y-1, z), digs the ice bare
// hand, and asserts the ice cell ends up as `want_after` -- BlockIce.
// harvestBlock's water-conversion (or its faithful absence).
static void assert_harvest_result(int x, int y, int z, int below_type,
								  int want_after, const char* label) {
	set_block(x, y - 1, z, below_type);
	set_block(x, y, z, BLOCK_ICE);
	give(0); // bare hand -- harvestBlock's water check is unconditional on tool
	dig(x, y, z);
	uint8_t got = type_at(x, y, z);
	if(got != want_after) {
		fprintf(stderr,
				"test_ice_dig: %s -- ice over block id %d became id %d, want id "
				"%d\n",
				label, below_type, got, want_after);
		assert(0 && "harvestBlock water-conversion mismatch");
	}
}

static void check_harvest_water_conversion(void) {
	// Solid block below (stone, Material.rock -> getIsSolid() true) -> flowing
	// water (id 8, Block.waterMoving), metadata 0 (server_world_set_block's
	// zero-initialized struct literal, matching block_ice.c's own melt-path
	// convention).
	assert_harvest_result(0, 70, 0, BLOCK_STONE, BLOCK_WATER_FLOW,
						  "ice over stone");
	{
		struct block_data blk;
		assert(server_world_get_block(&s.world, 0, 70, 0, &blk));
		assert(blk.type == BLOCK_WATER_FLOW && blk.metadata == 0
			   && "flowing water source must land at metadata 0");
	}

	// Air below (Material.air -> MaterialTransparent -> getIsSolid()/
	// getIsLiquid() both false) -> stays air. This is the REGRESSION guard: a
	// naive "always convert" implementation would wrongly water-fill this cell.
	assert_harvest_result(4, 70, 0, BLOCK_AIR, BLOCK_AIR, "ice over air");

	// Water below (MaterialLiquid -> getIsSolid() false, getIsLiquid() true --
	// the OR's second half) -> still converts.
	assert_harvest_result(8, 70, 0, BLOCK_WATER_STILL, BLOCK_WATER_FLOW,
						  "ice over water");

	printf("  ice harvestBlock water conversion: stone->flowing water(meta 0), "
		   "air->air (no spurious fill), water->flowing water OK\n");
}

// Creative mode reuses this exact server path (docs/creative_mode.md: "Server
// does no re-timing" -- only the CLIENT dig delay changes to 1ms). No separate
// server-side creative gate exists for digging, so driving the same
// SRPC_BLOCK_DIG RPC IS the creative-mode-instant-dig server behaviour; this
// smoke-checks that s.creative_mode doesn't accidentally suppress the
// conversion.
static void check_creative_mode_smoke(void) {
	bool prev = s.creative_mode;
	s.creative_mode = true;
	assert_harvest_result(12, 70, 0, BLOCK_STONE, BLOCK_WATER_FLOW,
						  "ice over stone (creative_mode=true)");
	s.creative_mode = prev;
	printf("  creative-mode smoke: harvest water-conversion unaffected by "
		   "server_local.creative_mode\n");
}

int main(void) {
	test_fixture_dir("test_ice_dig_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_ice_dig: field fix -- bare-hand dig speed + harvestBlock water "
		   "conversion (BlockIce, 740c583)\n");
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

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	check_harvest_water_conversion();
	check_creative_mode_smoke();

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_ice_dig: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
