/*
	Golden test for Beta 1.7.3 TOOL DURABILITY (handoff 2026-07-29 P0(d) + P1(b)).

	Two defects in one feature, so one test pins both:

	P0(d) -- every tool/sword entry stored Beta's maxDamage PLUS ONE (60/132/251/1562/33),
	so every tool lasted one extra swing and the durability bar's denominator was one too
	large. Beta EnumToolMaterial.java:4-8 is WOOD(0,59) STONE(1,131) IRON(2,250)
	EMERALD(3,1561) GOLD(0,32), and ItemTool.java:14 / ItemSword.java:9 / ItemHoe.java:7 all
	call setMaxDamage(var.getMaxUses()) -- so Beta's maxDamage IS that number. Do not restate
	this as "maxUses = maxDamage+1": Beta's field is *named* maxUses and IS the maxDamage.
	Only the folklore use-count ("a wood tool gets 60 uses") is one higher, and that is a
	consequence of the strict `>` break test, not a field.

	P1(b) -- the wear was never applied at all. item_stack_damage had exactly three call
	sites tree-wide (flint & steel x2, sheep shearing); block-break and melee never touched
	the held item, so no tool ever showed a bar. The draw path was always fine -- flint &
	steel's bar renders on real hardware -- which is why this is a SIMULATION gap.

	THE TRAP THIS TEST EXISTS TO PIN (docs/briefs/01_durability_combat.md was WRONG about it,
	and is now corrected): the wear is NOT gated on a successful harvest.
	PlayerControllerSP.java:28-38 --
	    removed = the base sendBlockRemoved(...);        // removal succeeded?
	    held = getCurrentEquippedItem();
	    canHarvest = canHarvestBlock(...);               // harvest gate
	    if (held != null) { held.onDestroyBlock(...); }    // WEAR  -- closes here
	    if (removed && canHarvest) { harvestBlock(...); }           // DROP  -- opens here
	The wear block closes before the harvest block opens. Same shape in PlayerControllerMP
	and, decisively for CaveWii (whose break handling is server-side),
	ItemInWorldManager.java:98-109. So breaking stone with a WOODEN pickaxe drops nothing and
	still costs a point, and so does breaking a hardness-0 torch -- ItemTool has no hardness
	guard in 1.7.3.

	Decompile: 740c583:1.7.3-LTS EnumToolMaterial, ItemTool, ItemSword, ItemShears, ItemHoe,
	ItemFlintAndSteel, Item, ItemStack, PlayerControllerSP/MP, ItemInWorldManager,
	EntityPlayer.attackTargetEntityWithCurrentItem.
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
#include "block/blocks_data.h"
#include "entity/combat.h" // PLAYER_EYE_HEIGHT
#include "entity/entity.h"
#include "entity/monsters_object.h" // MONSTER_PIG
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_tool_durability_tmp' (a literal) */
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

static void set(int x, int y, int z, int type) {
	struct block_data expected = {.type = (uint8_t)type, .metadata = 0};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	return blk.type;
}

static void give(uint16_t id) {
	inventory_set_hotbar(&s.player.inventory, 0);
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = id, .count = 1, .durability = 0});
}

// Read the RAW slot the server writes through, not inventory_get_slot -- that returns false
// for an empty slot, and "the tool broke" is a state this test must be able to observe.
static struct item_data held(void) {
	return s.player.inventory
		.items[INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&s.player.inventory)];
}

// `finished` = the completed break. Without it the handler takes the
// server_local_extinguish_fire_on_hit path (a dig-in-progress tick) and returns, so the
// block survives and nothing wears.
static void dig(int x, int y, int z) {
	struct server_rpc r = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.x = x, .y = y, .z = z, .finished = true}};
	server_local_test_process_rpc(&s, &r);
}

// --- (1) the 25 tier constants are Beta's maxDamage, not maxDamage+1 --------------
static void check_tier_constants(void) {
	const struct {
		uint16_t id;
		uint16_t want;
		const char* why;
	} table[] = {
		{ITEM_WOOD_PICKAXE, 59, "EnumToolMaterial WOOD"},
		{ITEM_WOOD_SWORD, 59, "EnumToolMaterial WOOD"},
		{ITEM_STONE_PICKAXE, 131, "EnumToolMaterial STONE"},
		{ITEM_STONE_SWORD, 131, "EnumToolMaterial STONE"},
		{ITEM_IRON_PICKAXE, 250, "EnumToolMaterial IRON"},
		{ITEM_IRON_SWORD, 250, "EnumToolMaterial IRON"},
		{ITEM_DIAMOND_PICKAXE, 1561, "EnumToolMaterial EMERALD"},
		{ITEM_DIAMOND_SWORD, 1561, "EnumToolMaterial EMERALD"},
		{ITEM_GOLD_PICKAXE, 32, "EnumToolMaterial GOLD"},
		{ITEM_GOLD_SWORD, 32, "EnumToolMaterial GOLD"},
		// NOT part of the EnumToolMaterial family: both classes pass a literal to
		// setMaxDamage, so these already held Beta's value and must be untouched.
		{ITEM_SHEARS, 238, "ItemShears.java:7 setMaxDamage(238)"},
		{ITEM_FLINT_STEEL, 64, "ItemFlintAndSteel.java:7 setMaxDamage(64)"},
	};
	for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		struct item_data d = {.id = table[i].id, .count = 1};
		struct item* it = item_get(&d);
		assert(it && "item id not registered");
		if(it->max_damage != table[i].want) {
			fprintf(stderr,
					"test_tool_durability: %s max_damage %u, want %u (%s)\n", it->name,
					(unsigned)it->max_damage, (unsigned)table[i].want, table[i].why);
			assert(0 && "tool max_damage is not Beta's maxDamage");
		}
	}
	printf("  [1] tier constants are Beta maxDamage (59/131/250/1561/32; shears 238, "
		   "flint 64)\n");
}

// --- (2) the break boundary: N survivable uses, destroyed on the N+1th --------------
// Beta ItemStack.damageItem accumulates then breaks on `itemDamage > getMaxDamage()`,
// STRICTLY. This is the assertion that reads wrong with the old +1 constants.
static void check_break_boundary(void) {
	const struct {
		uint16_t id;
		uint16_t max;
	} table[] = {
		{ITEM_WOOD_PICKAXE, 59},   {ITEM_STONE_PICKAXE, 131},
		{ITEM_IRON_SWORD, 250},      {ITEM_DIAMOND_PICKAXE, 1561},
		{ITEM_GOLD_PICKAXE, 32},
	};
	for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		struct item_data stack = {.id = table[i].id, .count = 1, .durability = 0};
		for(uint16_t use = 1; use <= table[i].max; use++) {
			bool broke = item_stack_damage(&stack, 1);
			assert(!broke && "the tool broke before its maxDamage was exceeded");
			assert(stack.id == table[i].id && stack.count == 1);
			assert(stack.durability == use);
		}
		// Use max+1 pushes damage to max+1, which IS > max -- this is the one that breaks.
		assert(item_stack_damage(&stack, 1) && "the tool survived past its maxDamage");
		assert(stack.id == 0 && stack.count == 0
			   && "a broken tool must clear the slot completely");
	}
	// A 2-point hit must be able to OVERSHOOT the limit, not be clamped: the break tests an
	// accumulated total, never a decrementing uses-remaining counter.
	struct item_data sword = {.id = ITEM_IRON_SWORD, .count = 1, .durability = 249};
	assert(!item_stack_damage(&sword, 1) && sword.durability == 250);
	sword.durability = 249;
	assert(item_stack_damage(&sword, 2) && sword.id == 0);
	printf("  [2] break boundary: max survivable uses, destroyed on the next one\n");
}

// --- (3) the wear-amount rule table ------------------------------------------------
// Gated on tool.type + the shears id, NEVER on has_damage: hoes, shears and flint & steel
// are all has_damage = true, so a has_damage gate would wear a hoe on every dig.
static void check_wear_rule_table(void) {
	const struct {
		uint16_t id;
		uint8_t block;
		uint16_t want_break;
		uint16_t want_melee;
		const char* why;
	} table[] = {
		// ItemTool: break 1, melee 2. ItemSword: break 2, melee 1. INVERTED -- the single
		// easiest thing to get backwards.
		{ITEM_DIAMOND_PICKAXE, BLOCK_STONE, 1, 2, "ItemTool"},
		{ITEM_IRON_SHOVEL, BLOCK_DIRT, 1, 2, "ItemTool"},
		{ITEM_STONE_AXE, BLOCK_LOG, 1, 2, "ItemTool"},
		{ITEM_IRON_SWORD, BLOCK_STONE, 2, 1, "ItemSword"},
		// ItemShears.onBlockDestroyed: 1 for leaves OR web only; no hitEntity override.
		{ITEM_SHEARS, BLOCK_LEAVES, 1, 0, "ItemShears leaves"},
		{ITEM_SHEARS, BLOCK_COBWEB, 1, 0, "ItemShears web"},
		{ITEM_SHEARS, BLOCK_WOOL, 0, 0, "shears cut wool fast but take no damage"},
		{ITEM_SHEARS, BLOCK_STONE, 0, 0, "ItemShears ignores other blocks"},
		// ItemHoe extends Item and overrides NEITHER -- tilling is its only wear path.
		{ITEM_IRON_HOE, BLOCK_DIRT, 0, 0, "ItemHoe overrides neither"},
		// ItemFlintAndSteel overrides neither -- it wears only via its own onItemUse.
		{ITEM_FLINT_STEEL, BLOCK_STONE, 0, 0, "ItemFlintAndSteel overrides neither"},
		// Item.java:197-203 base returns false for both -- plain items never wear.
		{ITEM_BOW, BLOCK_STONE, 0, 0, "Item base"},
		{BLOCK_DIRT, BLOCK_STONE, 0, 0, "a plain block item"},
	};
	for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		struct item_data d = {.id = table[i].id, .count = 1};
		struct item* it = item_get(&d);
		assert(it);
		uint16_t br = item_wear_on_block_break(table[i].id, it, table[i].block);
		uint16_t me = item_wear_on_hit_entity(it);
		if(br != table[i].want_break || me != table[i].want_melee) {
			fprintf(stderr,
					"test_tool_durability: %s vs block %u -> break %u melee %u, want "
					"%u/%u (%s)\n",
					it->name, (unsigned)table[i].block, (unsigned)br, (unsigned)me,
					(unsigned)table[i].want_break, (unsigned)table[i].want_melee,
					table[i].why);
			assert(0 && "wear rule table mismatch");
		}
	}
	// An empty hand has no definition at all and must not crash the helpers.
	assert(item_wear_on_block_break(0, NULL, BLOCK_STONE) == 0);
	assert(item_wear_on_hit_entity(NULL) == 0);
	printf("  [3] wear rules: tool 1/2, sword 2/1, shears leaves+web only, hoe/flint/bow "
		   "never\n");
}

// --- (4) a real dig through the RPC handler wears the REAL hotbar stack -------------
// Reads 0 before the fix: SRPC_BLOCK_DIG consulted the held item only to decide drops.
static void check_dig_wears_the_real_stack(void) {
	const int x = 10, y = 70, z = 10;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;

	set(x, y, z, BLOCK_STONE);
	give(ITEM_DIAMOND_PICKAXE);
	dig(x, y, z);
	assert(type_at(x, y, z) == BLOCK_AIR);
	assert(held().durability == 1
		   && "one block break must cost a pickaxe exactly 1 -- Beta shows the bar after "
			  "exactly one use");

	// A sword breaking a block costs 2, not 1.
	set(x, y, z, BLOCK_STONE);
	give(ITEM_IRON_SWORD);
	dig(x, y, z);
	assert(held().durability == 2 && "ItemSword.onBlockDestroyed is damageItem(2)");
	printf("  [4] one dig wears the real stack (pickaxe 1, sword 2)\n");
}

// --- (5) THE REFUTED-BRIEF CASE: a break that drops NOTHING still wears -------------
// The wear is outside both the removal-success flag and the canHarvestBlock gate.
static void check_wrong_tool_break_still_wears(void) {
	const int x = 12, y = 70, z = 10;
	// Stone needs a pickaxe; a SHOVEL cannot harvest it, so this drops nothing...
	set(x, y, z, BLOCK_STONE);
	give(ITEM_WOOD_SHOVEL);
	dig(x, y, z);
	assert(type_at(x, y, z) == BLOCK_AIR);
	assert(held().durability == 1
		   && "a break that drops nothing STILL wears -- the harvest gate guards only the "
			  "drop (PlayerControllerSP.java:28-38). The repo brief claimed otherwise.");

	// ...and a hardness-0 block costs a full point too: ItemTool has no hardness guard in
	// 1.7.3 (the getHardness() != 0 check is a later version).
	set(x, y, z, BLOCK_TORCH);
	give(ITEM_DIAMOND_PICKAXE);
	dig(x, y, z);
	assert(held().durability == 1
		   && "breaking a hardness-0 torch with a pickaxe costs 1 in 1.7.3");
	printf("  [5] no-drop break and hardness-0 break both still wear\n");
}

// --- (6) the items that must NOT wear on a dig --------------------------------------
static void check_non_wearing_items_survive_a_dig(void) {
	const int x = 14, y = 70, z = 10;
	const struct {
		uint16_t id;
		const char* why;
	} table[] = {
		{ITEM_IRON_HOE, "ItemHoe overrides neither onBlockDestroyed nor hitEntity"},
		{ITEM_FLINT_STEEL, "ItemFlintAndSteel wears only through its own onItemUse"},
		{ITEM_SHEARS, "ItemShears wears only on leaves/web"},
	};
	for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		set(x, y, z, BLOCK_STONE);
		give(table[i].id);
		dig(x, y, z);
		struct item_data h = held();
		if(h.durability != 0) {
			fprintf(stderr, "test_tool_durability: id %u wore %u on a stone dig -- %s\n",
					(unsigned)table[i].id, (unsigned)h.durability, table[i].why);
			assert(0 && "an item that must not wear on a dig wore");
		}
	}
	// ...but shears DO wear on leaves.
	set(x, y, z, BLOCK_LEAVES);
	give(ITEM_SHEARS);
	dig(x, y, z);
	assert(held().durability == 1 && "shears must wear 1 on leaves");
	printf("  [6] hoe / flint & steel / shears do not wear on a stone dig; shears do on "
		   "leaves\n");
}

// --- (7) a tool that runs out mid-dig clears the slot --------------------------------
static void check_dig_can_destroy_the_tool(void) {
	const int x = 16, y = 70, z = 10;
	give(ITEM_GOLD_PICKAXE); // maxDamage 32 -- cheap to exhaust
	struct item_data* slot
		= &s.player.inventory.items[INVENTORY_SLOT_HOTBAR
									+ inventory_get_hotbar(&s.player.inventory)];
	slot->durability = 32; // one more point is the fatal one
	set(x, y, z, BLOCK_STONE);
	dig(x, y, z);
	struct item_data h = held();
	assert(h.id == 0 && h.count == 0
		   && "the 33rd use must destroy a golden pickaxe and clear the slot");
	assert(type_at(x, y, z) == BLOCK_AIR && "the block still breaks on the fatal swing");
	printf("  [7] the fatal swing breaks the block AND clears the slot\n");
}

// --- (8) melee wears the weapon, EVEN when the hit is absorbed by i-frames -----------
// Beta discards attackEntityFrom's boolean: hitEntity runs regardless. Reads 0 before the
// fix, and reads 1 (not 2) if someone "improves" it by gating on the hit landing.
static void check_melee_wears_even_when_absorbed(void) {
	vec3 pos = {30.5f, 70.0f, 30.5f};
	set(30, 69, 30, BLOCK_STONE);
	struct entity* mob = server_local_spawn_monster(pos, MONSTER_PIG, &s);
	assert(mob && mob->type == ENTITY_MONSTER);
	s.player.x = 30.5f;
	s.player.y = 70.0f + PLAYER_EYE_HEIGHT;
	s.player.z = 30.5f;

	give(ITEM_IRON_SWORD);
	struct server_rpc atk = {.type = SRPC_ENTITY_ATTACK,
							 .payload.entity_attack.entity_id = mob->id};
	server_local_test_process_rpc(&s, &atk);
	assert(held().durability == 1 && "ItemSword.hitEntity is damageItem(1)");

	// Immediately again: the mob is inside its invulnerability window, so this hit is
	// ABSORBED and deals no damage -- but Beta still wears the sword.
	server_local_test_process_rpc(&s, &atk);
	assert(held().durability == 2
		   && "an i-frame-absorbed hit STILL wears the weapon -- Beta discards "
			  "attackEntityFrom's return value (EntityPlayer.java:495-498)");

	// A TOOL used as a weapon costs 2, not 1 -- the inversion.
	give(ITEM_DIAMOND_PICKAXE);
	server_local_test_process_rpc(&s, &atk);
	assert(held().durability == 2 && "ItemTool.hitEntity is damageItem(2)");
	printf("  [8] melee wears sword 1 / tool 2, and an absorbed hit still wears\n");
}

// --- (9) the client mirror is told, or the bar never appears -------------------------
// gui_util.c draws the bar from the CLIENT's inventory mirror, so a server-only wear is
// invisible -- indistinguishable from the bug this fixes.
// SOURCE SCAN, and deliberately so: client_interface.h exposes no receive/peek API for a
// test to intercept the queue with (tests INJECT with clin_process; clin_drain_discard only
// drains). The next best instrument is to pin that the writer still contains the push and
// that both wear sites still route through it -- which is what would actually be deleted in
// a regression. Same read_source pattern as tests/test_watchdog_phase.c.
static char* read_source(const char* path) {
	FILE* f = fopen(path, "rb");
	assert(f && "source file not found -- is WORKING_DIRECTORY the repo root?");
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
	char* buf = malloc((size_t)size + 1);
	assert(buf);
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

static void check_client_is_resynced(void) {
	char* src = read_source("source/network/server_local.c");
	const char* fn = strstr(src, "static void server_local_wear_held_item(");
	assert(fn && "the wear writer is gone");
	const char* fn_end = strstr(fn, "\n}");
	assert(fn_end);
	size_t len = (size_t)(fn_end - fn);
	char* body = malloc(len + 1);
	assert(body);
	memcpy(body, fn, len);
	body[len] = '\0';
	assert(strstr(body, "CRPC_INVENTORY_SLOT")
		   && "the wear writer no longer mirrors the slot to the client -- the durability "
			  "bar draws from the CLIENT's inventory copy, so a server-only wear is "
			  "invisible and indistinguishable from the bug this fixes");
	assert(strstr(body, "item_stack_damage") && "the wear writer no longer damages");
	free(body);

	// ...and both Beta wear sites still go through that writer. Count every mention and
	// subtract the definition; matching an argument list would break on reformatting, which
	// is exactly the kind of false failure that gets a guard deleted.
	int mentions = 0;
	for(const char* p = src; (p = strstr(p, "server_local_wear_held_item(")); p++)
		mentions++;
	assert(mentions >= 3
		   && "a wear site stopped calling server_local_wear_held_item -- block-break and "
			  "melee are the two Beta call sites, plus the definition");
	free(src);
	printf("  [9] the wear writer pushes CRPC_INVENTORY_SLOT and both sites use it\n");
}

int main(void) {
	test_fixture_dir("test_tool_durability_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_tool_durability_env: Beta 1.7.3 tool wear (P0(d) + P1(b))\n");
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	// Pure halves first -- they need no world and pin the constants + the rule table.
	check_tier_constants();
	check_break_boundary();
	check_wear_rule_table();

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
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	// Chunks 0..1 on both axes cover x/z 0..31 -- the dig fixtures sit near 10..18 and the
	// melee pig at 30.5.
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	check_dig_wears_the_real_stack();
	check_wrong_tool_break_still_wears();
	check_non_wearing_items_survive_a_dig();
	check_dig_can_destroy_the_tool();
	check_melee_wears_even_when_absorbed();
	check_client_is_resynced();

	g_drain_run = 0;
	printf("test_tool_durability_env: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
