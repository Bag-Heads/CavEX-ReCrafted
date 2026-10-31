/*
	push-0921/B10 -- Beta 1.7.3 parity for the player's ENVIRONMENTAL dig-speed
	penalties: strength is divided by 5 while the eye is inside water material,
	and again by 5 while the player is not on the ground.

	========================= THE SPEC (decompile 740c583) =====================
	Every literal in this file is hand-derived from these five sites; none of it
	was produced by running CaveWii's own code (CODESTYLE.md sec 4.2, the
	poisoned-golden rule). Paths are relative to
	../mcb173-retromcp/minecraft/src/net/minecraft/src/.

	EntityPlayer.java:291-302
	    Player strength starts with the inventory-provided block strength.
	    Water at the eye divides strength by five; being off the ground
	    divides it by five again. Both penalties apply when both hold.



	Block.java:313-315
	    Negative hardness yields zero progress. Unharvestable blocks use
	    reciprocal hardness divided by 100; harvestable blocks use player
	    strength divided by hardness and then by 30.
	InventoryPlayer.java:189-196 / :270-277
	    Inventory strength is the held stack strength, or 1.0F bare-handed.
	    Harvesting requires a harvestable material or a capable held stack.
	ItemTool.java:19-27      -> efficiencyOnProperMaterial if the block is in
	    blocksEffectiveAgainst, else 1.0F.  EnumToolMaterial.java:4-8: WOOD 2.0F,
	    STONE 4.0F, IRON 6.0F, EMERALD(diamond) 8.0F, GOLD 12.0F.
	PlayerControllerSP.java:71-83 (and PlayerControllerMP.java:83-96, identical):
	    The controller adds block strength to accumulated damage each tick
	    and removes the block when accumulated damage reaches 1.0F.
	    -- one accumulation per game tick, so the block breaks on the first tick
	    whose running sum reaches 1.0F.
	Entity.java:547-559, isInsideOfMaterial: takes the EYE cell
	    (posY + getEyeHeight()), requires blockMaterial == Material.water, then
	    corrects for the fluid surface height
	    (BlockFluid.getPercentAir(meta) = (meta+1)/9.0F, meta>=8 -> 0) and
	    returns eyeY < (cellY + 1) - (percentAir - 1/9).

	Beta 1.7.3 has NO efficiency enchantment and NO haste effect -- the only
	multipliers on strength in this version are the tool's
	efficiencyOnProperMaterial and these two /5.0F environment divisions.

	============ WHICH BRANCH THE PENALTIES LIVE IN (the easy-to-miss bit) =====
	getCurrentPlayerStrVsBlock is called ONLY on the canHarvestBlock side of
	Block.blockStrength's ternary. The "cannot harvest" arm is a bare
	1.0F/hardness/100.0F, so it never sees either /5.0F: a bare hand digs stone
	in exactly 150 ticks whether the player is standing, swimming, or falling.
	The mc173 oracle agrees -- mc173/mc173/src/world/break.rs:38-53 puts both
	`env_modifier /= 5.0` inside `if self.can_break(...)` and leaves the else
	arm a bare `hardness * 100.0`.

	=================== THE HAND-DERIVED TABLE (ticks, 50ms each) =============
	T = hardness * 30 * P / E ticks when harvestable (P = 1 dry+grounded, 5 for
	either penalty, 25 for both; E = strVsBlock), or hardness * 100 ticks when
	not -- and then the engine's ceil to a whole 20 TPS tick.

	  dirt 0.5F, hand (E=1.0, harvestable)
	    dry  0.5*30*1/1  = 15.0   -> 15 ticks     750 ms
	    water/air        = 75.0   -> 75 ticks    3750 ms
	    water+air        = 375.0  -> 375 ticks  18750 ms
	  stone 1.5F, hand (Material.rock is setNoHarvest -> CANNOT harvest)
	    all four         1.5*100  = 150.0 -> 150 ticks  7500 ms  (no penalty)
	  stone 1.5F, wooden pickaxe (E=2.0; stone IS in blocksEffectiveAgainst)
	    dry  1.5*30/2    = 22.5   -> 23 ticks    1150 ms
	    water/air  *5    = 112.5  -> 113 ticks   5650 ms
	    water+air  *25   = 562.5  -> 563 ticks  28150 ms
	  stone 1.5F, diamond pickaxe (E=8.0)
	    dry  1.5*30/8    = 5.625  -> 6 ticks      300 ms
	    water/air        = 28.125 -> 29 ticks    1450 ms
	    water+air        = 140.625-> 141 ticks   7050 ms
	  sand 0.5F, wooden shovel (E=2.0; sand IS in ItemSpade.blocksEffectiveAgainst)
	    dry  0.5*30/2    = 7.5    -> 8 ticks      400 ms
	    water/air        = 37.5   -> 38 ticks    1900 ms
	    water+air        = 187.5  -> 188 ticks   9400 ms
	  sand 0.5F, diamond shovel (E=8.0)
	    dry 1.875 -> 2 | water/air 9.375 -> 10 | water+air 46.875 -> 47 ticks
	  obsidian 10.0F, diamond pickaxe (harvestLevel 3 required; obsidian is NOT
	  in blocksEffectiveAgainst, so E = 1.0F even for diamond)
	    dry  10*30/1     = 300.0  -> 300 ticks  15000 ms
	    water/air        = 1500.0 -> 1500 ticks 75000 ms
	    water+air        = 7500.0 -> 7500 ticks 375000 ms
	  obsidian 10.0F, iron pickaxe (harvestLevel 2 -> CANNOT harvest)
	    all four         10*100   = 1000.0 -> 1000 ticks 50000 ms (no penalty)
	  cobweb 4.0F, wooden sword (ItemSword.getStrVsBlock = flat 15.0F vs web)
	    dry  4*30/15     = 8.0    -> 8 ticks      400 ms
	    water/air        = 40.0   -> 40 ticks    2000 ms
	    water+air        = 200.0  -> 200 ticks  10000 ms
	  dirt 0.5F, wooden sword (flat 1.5F, the non-web sword branch)
	    dry  0.5*30/1.5  = 10.0   -> 10 ticks     500 ms
	    water/air        = 50.0   -> 50 ticks    2500 ms
	    water+air        = 250.0  -> 250 ticks  12500 ms

	Cross-checked against the mc173 oracle for two of them, by the formula
	transcribed into mc173_break_duration() below:
	  dirt/hand/in-water : 0.5*30/(1.0/5) = 75.0 ticks  -- the exact number the
	    scripted MP run's server logged as `expected <start+52>` (52 =
	    (75*0.7) as u64) while this client finished at tick 15.
	  stone/wood pickaxe/airborne : 1.5*30/(2.0/5) = 112.5 ticks.

	ONE ORACLE DIVERGENCE found while doing that cross-check, recorded not
	followed: Beta's ItemSword.getStrVsBlock gives a sword a flat 1.5F against
	every non-web block, so a wooden sword digs dirt in 10 ticks. mc173 routes
	block::DIRT through get_break_speed's SHOVEL arm (break.rs:167-184), which
	has no sword case, so the oracle returns 1.0 and would expect 15 ticks --
	and 75 where Beta takes 50 in water. Authority order is decompile > mc173
	(CLAUDE.md), so this file keeps Beta's numbers and exempts exactly that one
	row from the acceptance-gate inequality, naming the reason inline.

	=============== A MODELLING DELTA FOUND WHILE DERIVING THIS ================
	Beta accumulates a float every tick and stops at >= 1.0F; this engine stores
	an integer millisecond budget and rounds up to a whole tick. The two agree
	except when T is an exact integer, where the float sum of T copies of 1/T
	lands a hair BELOW 1.0F and Beta needs one more tick. Measured (IEEE-754
	single, the decompile's own arithmetic): dirt/hand dry 15 == 15, but in
	water Beta 76 vs 75 here; stone/hand 151 vs 150; obsidian/diamond 301 vs
	300; cobweb/sword in water 41 vs 40. The delta is always exactly +1 tick
	(50 ms) in Beta's favour, is never more than +1, and it ALREADY applies to
	the dry-and-grounded rows this engine has shipped for months -- it is a
	property of the integer-millisecond dig model, not of the environment fix.
	Changing it would move every existing dig golden, so it is reported, not
	silently absorbed: this file pins the exact-rational value the engine can
	represent (the ceil column above) and records the Beta float count next to
	it in beta_float_ticks[] so the gap stays visible.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "item/tool.h"

#define TICK_MS 50

/* dry+grounded, in water, airborne, in water AND airborne */
enum { ENV_DRY, ENV_WATER, ENV_AIR, ENV_BOTH, ENV_COUNT };
static const bool env_in_water[ENV_COUNT] = {false, true, false, true};
static const bool env_on_ground[ENV_COUNT] = {true, true, false, false};
static const char* env_name[ENV_COUNT]
	= {"dry+on ground", "in water", "airborne", "in water + airborne"};

struct dig_row {
	const char* block_name;
	struct block* blk;
	const char* tool_name;
	uint16_t item; /* 0 = bare hand */
	int want_ms[ENV_COUNT];	 /* hand-derived, see the table above */
	int beta_float_ticks[ENV_COUNT]; /* Beta's float accumulation, for the record */
	/* mc173 oracle inputs, hand-transcribed from mc173/src/world/break.rs */
	float mc_hardness;
	float mc_break_speed;
	bool mc_can_break;
	/* NULL when mc173 agrees with the decompile. Otherwise: mc173 is WRONG
	   against Beta for this row, the acceptance-gate inequality below cannot
	   hold, and this string says why. Authority order is
	   decompile > mc173 (CLAUDE.md), so we stay Beta-faithful and record the
	   oracle's bug rather than bending the client toward it. */
	const char* mc_divergence;
};

static struct item* held(uint16_t id) {
	if(!id)
		return NULL;
	struct item_data d = {.id = id, .count = 1};
	struct item* it = item_get(&d);
	assert(it && "tool item id not registered");
	return it;
}

/* ---------------------------------------------------------------------------
   The mc173 oracle's own formula, hand-transcribed (NOT called through any
   CaveWii helper):

   mc173/mc173/src/world/break.rs:25-57  World::get_break_duration
       let hardness = get_break_hardness(block_id);
       if can_break { let mut env = get_break_speed(item, block);
                      if in_water { env /= 5.0; }
                      if !on_ground { env /= 5.0; }
                      hardness * 30.0 / env }
       else { hardness * 100.0 }
   mc173/mc173-server/src/player.rs:334-339  the acceptance gate
       let min_time = state.start_time + (break_duration * 0.7) as u64;
       if world.get_time() >= min_time { break_block(pos) }
       else { warn!("incoherent break time, expected {min_time} ...") }
   --------------------------------------------------------------------------- */
static float mc173_break_duration(float hardness, float break_speed,
								  bool can_break, bool in_water,
								  bool on_ground) {
	if(!can_break)
		return hardness * 100.0F;
	float env = break_speed;
	if(in_water)
		env /= 5.0F;
	if(!on_ground)
		env /= 5.0F;
	return hardness * 30.0F / env;
}

static uint64_t mc173_min_ticks(float duration) {
	return (uint64_t)(duration * 0.7F); /* Rust `as u64` truncates toward zero */
}

static const struct dig_row rows[] = {
	{"dirt", &block_dirt, "bare hand", 0,
	 {750, 3750, 3750, 18750}, {15, 76, 76, 376}, 0.5F, 1.0F, true, NULL},
	{"dirt", &block_dirt, "wooden sword", ITEM_WOOD_SWORD,
	 {500, 2500, 2500, 12500}, {10, 51, 51, 250}, 0.5F, 1.0F, true,
	 /* Beta: ItemSword.getStrVsBlock returns a flat 1.5F for every non-web
	    block, so a sword really does dig dirt 1.5x faster. mc173's
	    get_break_speed reaches block::DIRT through its SHOVEL arm
	    (break.rs:167-184), whose match has no sword case, so the oracle
	    returns 1.0 and expects a 15/75/75/375-tick dig where Beta takes
	    10/50/50/250. Decompile > mc173: we stay at Beta's number and the
	    acceptance-gate inequality is skipped for this row alone. */
	 "mc173 break.rs:167-184 gives a sword no 1.5F bonus on shovel blocks"},
	{"stone", &block_stone, "bare hand", 0,
	 {7500, 7500, 7500, 7500}, {151, 151, 151, 151}, 1.5F, 1.0F, false, NULL},
	{"stone", &block_stone, "wooden pickaxe", ITEM_WOOD_PICKAXE,
	 {1150, 5650, 5650, 28150}, {23, 113, 113, 563}, 1.5F, 2.0F, true, NULL},
	{"stone", &block_stone, "diamond pickaxe", ITEM_DIAMOND_PICKAXE,
	 {300, 1450, 1450, 7050}, {6, 29, 29, 141}, 1.5F, 8.0F, true, NULL},
	{"sand", &block_sand, "wooden shovel", ITEM_WOOD_SHOVEL,
	 {400, 1900, 1900, 9400}, {8, 38, 38, 188}, 0.5F, 2.0F, true, NULL},
	{"sand", &block_sand, "diamond shovel", ITEM_DIAMOND_SHOVEL,
	 {100, 500, 500, 2350}, {2, 10, 10, 47}, 0.5F, 8.0F, true, NULL},
	{"obsidian", &block_obsidian, "diamond pickaxe", ITEM_DIAMOND_PICKAXE,
	 {15000, 75000, 75000, 375000}, {301, 1500, 1500, 7500}, 10.0F, 1.0F, true,
	 NULL},
	{"obsidian", &block_obsidian, "iron pickaxe", ITEM_IRON_PICKAXE,
	 {50000, 50000, 50000, 50000}, {1001, 1001, 1001, 1001}, 10.0F, 1.0F, false,
	 NULL},
	{"cobweb", &block_cobweb, "wooden sword", ITEM_WOOD_SWORD,
	 {400, 2000, 2000, 10000}, {8, 41, 41, 201}, 4.0F, 15.0F, true, NULL},
};

/* (1) The golden table: every block x tool x environment combination. */
static void check_environment_table(void) {
	unsigned red = 0;
	for(unsigned r = 0; r < sizeof rows / sizeof *rows; r++) {
		const struct dig_row* row = &rows[r];
		for(int e = 0; e < ENV_COUNT; e++) {
			int got = tool_dig_delay_env_ms(row->blk, held(row->item),
											env_in_water[e], env_on_ground[e]);
			if(got != row->want_ms[e]) {
				fprintf(stderr,
						"test_dig_speed_parity: %s with %s, %s -- got %d ms "
						"(%d ticks), want %d ms (%d ticks) from Beta's "
						"getCurrentPlayerStrVsBlock\n",
						row->block_name, row->tool_name, env_name[e], got,
						got / TICK_MS, row->want_ms[e],
						row->want_ms[e] / TICK_MS);
				red++;
			}
		}
	}
	if(red) {
		fprintf(stderr,
				"test_dig_speed_parity: %u of %u environment rows wrong\n", red,
				(unsigned)(sizeof rows / sizeof *rows) * ENV_COUNT);
		assert(0 && "dig speed must apply Beta's in-water and !onGround /5.0F");
	}
	printf("  environment table: %u blocks/tools x %d environments OK\n",
		   (unsigned)(sizeof rows / sizeof *rows), ENV_COUNT);
}

/* (2) The dry, on-ground rows must NOT move: the two-argument entry point is
   still exactly Beta's dry standing case, so every shipped dig golden holds. */
static void check_dry_rows_unmoved(void) {
	for(unsigned r = 0; r < sizeof rows / sizeof *rows; r++) {
		const struct dig_row* row = &rows[r];
		int legacy = tool_dig_delay_ms(row->blk, held(row->item));
		if(legacy != row->want_ms[ENV_DRY]) {
			fprintf(stderr,
					"test_dig_speed_parity: %s with %s -- the 2-argument "
					"tool_dig_delay_ms returned %d ms, want the unchanged dry "
					"%d ms\n",
					row->block_name, row->tool_name, legacy,
					row->want_ms[ENV_DRY]);
			assert(0 && "the dry on-ground dig speeds must not move");
		}
		int env_dry = tool_dig_delay_env_ms(row->blk, held(row->item), false,
											true);
		assert(legacy == env_dry
			   && "tool_dig_delay_ms must be the (dry, on ground) case");
	}
	printf("  dry on-ground rows unchanged (2-arg API == env(false,true)) OK\n");
}

/* (3) The unbreakable sentinel and the cannot-harvest branch are untouched by
   the environment: hardness <= 0 stays -1 in every environment (the creative
   instant-break at the call site keys off that -1/0 contract), and Beta's
   1.0F/hardness/100.0F arm never reads getCurrentPlayerStrVsBlock. */
static void check_penalty_free_paths(void) {
	for(int e = 0; e < ENV_COUNT; e++) {
		int bedrock = tool_dig_delay_env_ms(&block_bedrock, NULL,
											env_in_water[e], env_on_ground[e]);
		if(bedrock != -1) {
			fprintf(stderr,
					"test_dig_speed_parity: bedrock (%s) returned %d, want -1 "
					"-- the hardness<=0 sentinel must survive the environment "
					"arguments\n",
					env_name[e], bedrock);
			assert(0 && "hardness<=0 must stay the -1 never-digs sentinel");
		}
	}
	/* stone and obsidian bare-handed / under-tiered: Beta's cannot-harvest arm.
	   Rows 2 and 8 above already pin the four equal values; assert the
	   relationship explicitly so a future "apply the penalty everywhere"
	   refactor fails loudly here. */
	int stone_dry = tool_dig_delay_env_ms(&block_stone, NULL, false, true);
	int stone_wet = tool_dig_delay_env_ms(&block_stone, NULL, true, false);
	if(stone_dry != stone_wet) {
		fprintf(stderr,
				"test_dig_speed_parity: hand-dug stone changed with the "
				"environment (%d -> %d ms). Block.blockStrength's "
				"!canHarvestBlock arm is 1.0F/hardness/100.0F and never calls "
				"getCurrentPlayerStrVsBlock -- no /5.0F applies there\n",
				stone_dry, stone_wet);
		assert(0 && "the cannot-harvest branch must ignore water/airborne");
	}
	printf("  cannot-harvest + hardness<=0 paths ignore the environment OK\n");
}

/* (4) Multiplayer: our "finished" must never beat mc173's acceptance gate. */
static void check_never_early_for_mc173(void) {
	unsigned checked = 0;
	for(unsigned r = 0; r < sizeof rows / sizeof *rows; r++) {
		const struct dig_row* row = &rows[r];
		if(row->mc_divergence) {
			printf("  mc173 gate SKIPPED for %s with %s: %s\n", row->block_name,
				   row->tool_name, row->mc_divergence);
			continue;
		}
		for(int e = 0; e < ENV_COUNT; e++) {
			float duration = mc173_break_duration(
				row->mc_hardness, row->mc_break_speed, row->mc_can_break,
				env_in_water[e], env_on_ground[e]);
			uint64_t need = mc173_min_ticks(duration);
			uint64_t ours = (uint64_t)(tool_dig_delay_env_ms(
										   row->blk, held(row->item),
										   env_in_water[e], env_on_ground[e])
									   / TICK_MS);
			if(ours < need) {
				fprintf(stderr,
						"test_dig_speed_parity: %s with %s, %s -- we finish at "
						"tick %llu but mc173 accepts no earlier than %llu "
						"(duration %.3f ticks); the server logs \"incoherent "
						"break time\" and REFUSES the break\n",
						row->block_name, row->tool_name, env_name[e],
						(unsigned long long)ours, (unsigned long long)need,
						(double)duration);
				assert(0 && "client dig must never finish before mc173's gate");
			}
			checked++;
		}
	}
	printf("  mc173 acceptance gate: %u block/tool/environment combinations, "
		   "never early OK\n",
		   checked);
}

/* (5) The recorded Beta-float delta stays a +1-tick ceiling artefact. If a
   future change makes it bigger, this says so instead of letting it drift. */
static void check_float_accumulation_delta(void) {
	for(unsigned r = 0; r < sizeof rows / sizeof *rows; r++) {
		const struct dig_row* row = &rows[r];
		for(int e = 0; e < ENV_COUNT; e++) {
			int ours = row->want_ms[e] / TICK_MS;
			int delta = row->beta_float_ticks[e] - ours;
			if(delta < 0 || delta > 1) {
				fprintf(stderr,
						"test_dig_speed_parity: %s with %s, %s -- Beta's float "
						"accumulation takes %d ticks, the integer-ms model %d; "
						"the documented gap is 0 or +1 tick, not %d\n",
						row->block_name, row->tool_name, env_name[e],
						row->beta_float_ticks[e], ours, delta);
				assert(0 && "the float-accumulation gap must stay <= 1 tick");
			}
		}
	}
	printf("  Beta float-accumulation gap within the documented +1 tick OK\n");
}

int main(void) {
	printf("test_dig_speed_parity: Beta 1.7.3 in-water and !onGround dig "
		   "penalties (EntityPlayer.getCurrentPlayerStrVsBlock)\n");
	blocks_init();
	items_init();

	check_dry_rows_unmoved();
	check_environment_table();
	check_penalty_free_paths();
	check_never_early_for_mc173();
	check_float_accumulation_delta();

	printf("test_dig_speed_parity: PASS\n");
	return 0;
}
