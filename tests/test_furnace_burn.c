/*
	Golden behavior test for the Beta 1.7.3 furnace burn/cook model (M4 item 2),
	authored BEFORE the furnace rewrite per the project's golden-test-first rule.

	Drives the REAL production furnace_tick (source/block/furnace_logic.c) over N
	synthetic ticks and asserts per-tick state against values transcribed from the
	decompile (740c583): TileEntityFurnace.updateEntity / canSmelt / smeltItem /
	getItemBurnTime. The recipe + fuel-duration + max-stack tables are injected as
	test stubs (the production wiring injects the item-registry-backed versions), so
	this pins the tick MATH headlessly -- the off-by-one tick IS the behavior.

	Decompile citations (per assert):
	  - cook completes at furnaceCookTime == 200       (updateEntity:128)
	  - --furnaceBurnTime happens FIRST                (updateEntity:108-110)
	  - re-light only when burnTime==0 && canSmelt(), consuming ONE fuel and setting
	    currentItemBurnTime = burnTime = getItemBurnTime (updateEntity:113-124)
	  - cookTime RESETS to 0 (not paused) when not burning or !canSmelt (else:134)
	  - getItemBurnTime: coal 1600, wood-material 300, stick 100, sapling 100,
	    lava bucket 20000, else 0                       (getItemBurnTime:185-202)
	  - canSmelt output-room: count < min(64, maxStackSize)  (canSmelt:158-163)
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/furnace_logic.h"

// Synthetic ids (decoupled from the item enum -- furnace_logic only needs item_data).
enum { ID_IRON_ORE = 1000, ID_IRON = 1001, ID_COAL = 1002, ID_PLANK = 1003,
	   ID_STICK = 1004, ID_SAPLING = 1005, ID_LAVA_BUCKET = 1006, ID_DIRT = 1007 };

// getItemBurnTime, in TICKS (decompile values).
static int t_fuel(struct item_data it) {
	if(it.count == 0)
		return 0;
	switch(it.id) {
		case ID_PLANK: return 300;	  // wood material
		case ID_STICK: return 100;
		case ID_COAL: return 1600;
		case ID_LAVA_BUCKET: return 20000;
		case ID_SAPLING: return 100;
		default: return 0;
	}
}
// FurnaceRecipes: the only smelt the test needs is iron_ore -> iron (count 1).
static bool t_smelt(struct item_data in, struct item_data* out) {
	if(in.id == ID_IRON_ORE) {
		*out = (struct item_data) {.id = ID_IRON, .durability = 0, .count = 1};
		return true;
	}
	return false;
}
static int t_maxstack(struct item_data it) {
	(void)it;
	return 64;
}
static const struct furnace_env ENV = {t_fuel, t_smelt, t_maxstack};

static struct item_data S(int id, int count) {
	return (struct item_data) {.id = (uint16_t)id, .durability = 0, .count = (uint8_t)count};
}


/* ===== IMP-TE-CAPS T1 equivalence golden: dormant-furnace skip ===========
 * furnace_server_tick_w skips a furnace whose tick is a proven fixed point
 * (furnace_tick_is_idle: burnTime == 0, cookTime == 0, !canSmelt). Beta
 * World.updateEntities still runs TileEntityFurnace.updateEntity on it
 * [740c583 TileEntityFurnace.updateEntity]: burnTime > 0 is false, the
 * re-light branch needs canSmelt, isBurning() is false so cookTime is forced
 * to 0 (already 0) and the burning state cannot toggle -- nothing changes.
 * This golden proves byte-identical results: 64 furnaces in mixed states
 * (idle empty, idle unsmeltable, output full, burning, about to light,
 * cooking without fuel, pinned import) driven 5000 ticks with scripted window
 * edits, once ticking EVERY furnace and once skipping idle ones; every
 * furnace field and the derived block id (lit/unlit) must agree each tick. */
static bool fte_equal(const struct furnace_te* a, const struct furnace_te* b) {
	if(a->burn_time != b->burn_time || a->cook_time != b->cook_time
	   || a->current_item_burn_time != b->current_item_burn_time)
		return false;
	for(int k = 0; k < 3; k++)
		if(a->items[k].id != b->items[k].id
		   || a->items[k].count != b->items[k].count
		   || a->items[k].durability != b->items[k].durability
		   || a->items[k].nbt_record != b->items[k].nbt_record)
			return false;
	return true;
}

static uint32_t g_lcg;
static uint32_t lcg(void) {
	g_lcg = g_lcg * 1664525u + 1013904223u;
	return g_lcg >> 8;
}

static void dormant_skip_fixed_point_sweep(void) {
	const struct item_data inputs[] = {S(0, 0), S(ID_DIRT, 5), S(ID_IRON_ORE, 3),
									   S(ID_IRON_ORE, 64)};
	const struct item_data fuels[] = {S(0, 0), S(ID_COAL, 1), S(ID_PLANK, 7),
									  S(ID_DIRT, 2)};
	const struct item_data outputs[] = {S(0, 0), S(ID_IRON, 63), S(ID_IRON, 64),
										S(ID_DIRT, 1)};
	int idle_states = 0;
	for(int bt = 0; bt < 3; bt++)
		for(int ct = 0; ct < 3; ct++)
			for(unsigned a = 0; a < 4; a++)
				for(unsigned b = 0; b < 4; b++)
					for(unsigned c = 0; c < 4; c++)
						for(int cur = 0; cur < 2; cur++) {
							struct furnace_te f = {0};
							f.burn_time = bt * 37;
							f.cook_time = ct * 61;
							f.current_item_burn_time = cur * 1600;
							f.items[0] = inputs[a];
							f.items[1] = fuels[b];
							f.items[2] = outputs[c];
							bool idle = furnace_tick_is_idle(&f, &ENV);
							/* exactly the documented predicate */
							assert(idle
								   == (f.burn_time == 0 && f.cook_time == 0
									   && !furnace_can_smelt(&f, &ENV)));
							if(!idle)
								continue;
							idle_states++;
							struct furnace_te g = f;
							bool toggled = furnace_tick(&g, &ENV);
							assert(!toggled && fte_equal(&f, &g));
						}
	assert(idle_states > 0);
	printf("  dormant skip: %d idle states are exact fixed points of furnace_tick\n",
		   idle_states);
}

enum { EQ_FURNACES = 64, EQ_TICKS = 5000 };

static void eq_seed(struct furnace_te* f, int i) {
	memset(f, 0, sizeof(*f));
	switch(i % 8) {
		case 0: break;										  /* idle, empty */
		case 1: f->items[0] = S(ID_DIRT, 9); break;			  /* unsmeltable */
		case 2: f->items[0] = S(ID_IRON_ORE, 5);			  /* output full */
				f->items[2] = S(ID_IRON, 64); break;
		case 3: f->burn_time = 400; f->current_item_burn_time = 1600;
				f->items[0] = S(ID_IRON_ORE, 3); break;	  /* burning */
		case 4: f->items[0] = S(ID_IRON_ORE, 2);			  /* will light */
				f->items[1] = S(ID_PLANK, 2); break;
		case 5: f->cook_time = 120; f->items[0] = S(ID_IRON_ORE, 1);
				break;										  /* cook, no fuel */
		case 6: f->burn_time = 3; f->current_item_burn_time = 100;
				break;										  /* fuel dying, idle */
		case 7: f->items[0] = S(ID_IRON_ORE, 4);			  /* pinned import */
				f->items[0].nbt_record = 255;
				f->items[1] = S(ID_COAL, 1); break;
	}
}

/* A scripted "window edit": what a player can do to a furnace's slots
 * (Beta has no hoppers; the furnace window is the only other writer). The
 * production wake rule is implicit: idleness is re-evaluated every tick. */
static void eq_edit(struct furnace_te* f, uint32_t r) {
	switch(r % 6) {
		case 0: f->items[0] = S(ID_IRON_ORE, 1 + (int)(r >> 4) % 8); break;
		case 1: f->items[1] = S((r >> 3) & 1 ? ID_COAL : ID_STICK, 1); break;
		case 2: f->items[2] = S(0, 0); break;
		case 3: f->items[0] = S(ID_DIRT, 1); break;
		case 4: f->items[0] = S(0, 0); break;
		case 5: f->items[2] = S(ID_IRON, 64); break;
	}
}

static void dormant_skip_equivalence_golden(void) {
	static struct furnace_te full[EQ_FURNACES], skip[EQ_FURNACES];
	bool lit_full[EQ_FURNACES], lit_skip[EQ_FURNACES];
	for(int i = 0; i < EQ_FURNACES; i++) {
		eq_seed(&full[i], i);
		eq_seed(&skip[i], i);
		lit_full[i] = lit_skip[i] = furnace_is_burning(&full[i]);
	}
	g_lcg = 0xC0FFEEu;
	long skipped = 0, toggles = 0, smelted = 0;
	for(int t = 0; t < EQ_TICKS; t++) {
		/* the same window edits land on both copies before the tick */
		if(t % 7 == 0) {
			uint32_t r = lcg();
			int i = (int)(r % EQ_FURNACES);
			uint32_t e = lcg();
			eq_edit(&full[i], e);
			eq_edit(&skip[i], e);
		}
		for(int i = 0; i < EQ_FURNACES; i++) {
			int before = full[i].items[2].count;
			/* reference: Beta ticks every loaded furnace */
			if(furnace_tick(&full[i], &ENV)) {
				lit_full[i] = furnace_is_burning(&full[i]);
				toggles++;
			}
			if(full[i].items[2].count > before)
				smelted++;
			/* T1 path: production predicate decides the skip */
			if(furnace_tick_is_idle(&skip[i], &ENV))
				skipped++;
			else if(furnace_tick(&skip[i], &ENV))
				lit_skip[i] = furnace_is_burning(&skip[i]);
			assert(fte_equal(&full[i], &skip[i]));
			assert(lit_full[i] == lit_skip[i]);
		}
	}
	/* the golden must exercise every path, not pass vacuously */
	assert(skipped > EQ_TICKS && toggles > 10 && smelted > 10);
	printf("  dormant skip golden: %d furnaces x %d ticks byte-identical "
		   "(%ld idle ticks skipped, %ld lit/unlit toggles, %ld smelts)\n",
		   EQ_FURNACES, EQ_TICKS, skipped, toggles, smelted);
}

int main(void) {
	dormant_skip_fixed_point_sweep();
	dormant_skip_equivalence_golden();
	// (1) COOK == EXACTLY 200 TICKS/SMELT + (5) re-light consumes one fuel at burn==0.
	{
		struct furnace_te f = {0};
		f.items[FURNACE_SLOT_INPUT_TE] = S(ID_IRON_ORE, 1);
		f.items[FURNACE_SLOT_FUEL_TE] = S(ID_COAL, 1);

		bool toggled = furnace_tick(&f, &ENV); // tick 1: re-light + first cook step
		assert(toggled);						// off -> on (updateFurnaceBlockState fires)
		assert(f.burn_time == 1600);			// lit to coal's full duration
		assert(f.current_item_burn_time == 1600);
		assert(f.items[FURNACE_SLOT_FUEL_TE].count == 0); // ONE fuel consumed at re-light
		assert(f.cook_time == 1);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 0); // nothing smelted yet

		for(int t = 2; t <= 199; t++)
			furnace_tick(&f, &ENV);
		assert(f.cook_time == 199);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 0); // still not at 200

		furnace_tick(&f, &ENV); // tick 200
		assert(f.cook_time == 0);							// reset after the smelt
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].id == ID_IRON);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 1); // one output at tick 200
		assert(f.items[FURNACE_SLOT_INPUT_TE].count == 0);	// input consumed -> null
		assert(f.burn_time == 1600 - 199);					// burned ticks 2..200
	}

	// (2) cookTime RESETS to 0 (not paused) when canSmelt() becomes false mid-cook.
	{
		struct furnace_te f = {0};
		f.items[FURNACE_SLOT_INPUT_TE] = S(ID_IRON_ORE, 2); // 2 so it keeps cooking
		f.items[FURNACE_SLOT_FUEL_TE] = S(ID_COAL, 1);
		for(int t = 0; t < 50; t++)
			furnace_tick(&f, &ENV);
		assert(f.cook_time == 50);
		f.items[FURNACE_SLOT_INPUT_TE] = (struct item_data) {0}; // pull input
		furnace_tick(&f, &ENV);
		assert(f.cook_time == 0); // RESET, not held at 50 (the `else` branch)
		assert(f.burn_time > 0);  // fuel still burning -- only the cook progress reset
	}

	// (3) FUEL DURATIONS map to exact smelt counts (input saturated so output is the
	//     only limiter): coal 1600 = 8 cooks; a wood plank 300 = 1 cook (+100 residual).
	{
		struct furnace_te f = {0};
		f.items[FURNACE_SLOT_INPUT_TE] = S(ID_IRON_ORE, 64);
		f.items[FURNACE_SLOT_FUEL_TE] = S(ID_COAL, 1);
		for(int t = 0; t < 2000; t++)
			furnace_tick(&f, &ENV);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 8); // 1600 / 200
		assert(f.burn_time == 0);							// coal exhausted
		assert(f.items[FURNACE_SLOT_INPUT_TE].count == 64 - 8);
	}
	{
		struct furnace_te f = {0};
		f.items[FURNACE_SLOT_INPUT_TE] = S(ID_IRON_ORE, 64);
		f.items[FURNACE_SLOT_FUEL_TE] = S(ID_PLANK, 1);
		for(int t = 0; t < 400; t++)
			furnace_tick(&f, &ENV);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 1); // 300 burn = 1 cook
		assert(f.burn_time == 0);
	}

	// (4) RE-LIGHT across the fuel boundary (ordering): 2 coal -> 3200 burn = 16 cooks.
	{
		struct furnace_te f = {0};
		f.items[FURNACE_SLOT_INPUT_TE] = S(ID_IRON_ORE, 64);
		f.items[FURNACE_SLOT_FUEL_TE] = S(ID_COAL, 2);
		for(int t = 0; t < 3300; t++)
			furnace_tick(&f, &ENV);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 16); // 3200 / 200
		assert(f.items[FURNACE_SLOT_FUEL_TE].count == 0);	 // both coal consumed
	}

	// (6) OUTPUT-FULL STALL: canSmelt() false -> NO fuel consumed, no light, cook 0.
	{
		struct furnace_te f = {0};
		f.items[FURNACE_SLOT_INPUT_TE] = S(ID_IRON_ORE, 64);
		f.items[FURNACE_SLOT_FUEL_TE] = S(ID_COAL, 1);
		f.items[FURNACE_SLOT_OUTPUT_TE] = S(ID_IRON, 64); // full output
		for(int t = 0; t < 10; t++)
			furnace_tick(&f, &ENV);
		assert(f.items[FURNACE_SLOT_FUEL_TE].count == 1); // fuel NOT wasted
		assert(f.burn_time == 0);						  // never lit
		assert(f.cook_time == 0);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 64); // unchanged
	}

	// Non-fuel / non-smeltable safety: dirt fuel + dirt input -> nothing happens.
	{
		struct furnace_te f = {0};
		f.items[FURNACE_SLOT_INPUT_TE] = S(ID_DIRT, 1);
		f.items[FURNACE_SLOT_FUEL_TE] = S(ID_DIRT, 1);
		for(int t = 0; t < 250; t++)
			furnace_tick(&f, &ENV);
		assert(f.burn_time == 0 && f.cook_time == 0);
		assert(f.items[FURNACE_SLOT_OUTPUT_TE].count == 0);
		assert(f.items[FURNACE_SLOT_FUEL_TE].count == 1); // dirt isn't fuel
	}

	// (7) FACING metadata from placer yaw (Beta BlockFurnace.onBlockPlacedBy):
	//     facing = floor(yaw*4/360 + 0.5) masked to 0..3 [BlockFurnace.java:123]; 0->2,1->5,2->3,3->4.
	//     player.rx is degrees in the Beta rotationYaw convention (0=+Z south,
	//     90=-X west, 180=-Z north, 270/-90=+X east). The front faces the placer.
	//     Regression pin for the ~57x radian-conversion bug (front faced ~randomly).
	{
		assert(furnace_facing_meta(0.0F) == 2);	  // face south -> front north
		assert(furnace_facing_meta(90.0F) == 5);  // face west  -> front east
		assert(furnace_facing_meta(180.0F) == 3); // face north -> front south
		assert(furnace_facing_meta(270.0F) == 4); // face east  -> front west
		assert(furnace_facing_meta(-90.0F) == 4); // -90 == 270 (east)
		assert(furnace_facing_meta(360.0F) == 2); // wraps to 0
		assert(furnace_facing_meta(45.0F) == 5);  // floor(0.5+0.5)=1 -> 5
		// A ~1-degree yaw must stay quadrant 0 (the bug flipped it: 1*57.3/90 rounds
		// to quadrant 1). Faithful math: floor(1/90 + 0.5) = 0 -> meta 2.
		assert(furnace_facing_meta(1.0F) == 2);
	}

	printf("test_furnace_burn: OK (Beta cook=200t, fuel durations, re-light, reset, "
		   "output-stall, facing-meta all pinned)\n");
	return 0;
}
