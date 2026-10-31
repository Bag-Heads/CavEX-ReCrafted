/*
	Copyright (c) 2026

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	Faithfulness gate for the per-game-tick scheduled-tick FLOWING-FLUID BUDGET
	(server_world_drain_scheduled, applied via server_local.scheduled_tick_budget).

	The budget kills the 6-8s in-play freeze: a fluid cascade can make the ready-tick
	queue reach thousands, and firing them ALL in one game-tick (the old unbudgeted
	drain) is the freeze. The Wii cap applies only to flowing water/lava; non-fluid
	timing work passes through, while Beta's absolute 1000-fire ceiling still applies.

	The pure-fluid preservation argument has TWO independently-proven halves:
	  1. CONFLUENCE (tests/test_fluid_spread.c): the Beta water/lava flow core reaches
	     the SAME fixed point regardless of the order/timing in which cells are ticked.
	     So spreading the same fluid ticks across more game-ticks cannot change the
	     SETTLED block+light state -- only the settle RATE.
	  2. CONSERVATION (this test): server_world_drain_scheduled fires EXACTLY the same
	     flowing-fluid entries, just spread across game-ticks; it never drops,
	     duplicates, or starves one, and future-dated ticks consume no budget.
	Together: a pure-fluid cohort is conserved to its fixed point while the freeze
	becomes a smooth multi-tick settle. Mixed-class behavior is intentionally NOT
	byte-equivalent: test 4 pins the Wii policy that later TNT/fire/redstone
	callbacks overtake capped fluid, and test 7 ensures newly scheduled children never
	overtake the older deferred fluid cohort. Beta's absolute 1000-fire ceiling remains.

	We drive server_world_drain_scheduled directly with a COUNTING fire callback (no
	world mutation needed), so the real drain/budget policy is exercised while the test
	links only the lightweight queue translation unit -- no chunk dict, generator, or
	lighting. `blocks` is defined only because server_world_tick_queue.c references it.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "network/server_world.h"
#include "network/server_local_state.h"

struct block* blocks[256]; // referenced by server_world_tick_queue.c; never dereferenced

#define ANY_BLOCK BLOCK_WATER_FLOW // a real budgeted flowing-fluid tick

static int g_fires;
static uint8_t g_fired_block[SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY];
static w_coord_t g_fired_x[SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY];
static bool g_rearm_fluids;
static void count_fire(struct server_world* w, struct server_local* s,
					   struct scheduled_tick t) {
	(void)s;
	assert(g_fires < SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	g_fired_block[g_fires] = t.block_type;
	g_fired_x[g_fires] = t.x;
	g_fires++;
	if(g_rearm_fluids && (t.block_type == BLOCK_WATER_FLOW
		   || t.block_type == BLOCK_LAVA_FLOW))
		assert(server_world_schedule_tick_raw(
			w, 1000 + t.x, t.y, t.z, t.block_type, 5));
}

static void setup(struct server_world* w, uint32_t cap) {
	memset(w, 0, sizeof(*w));
	g_rearm_fluids = false;
	server_world_scheduled_ticks_create(w, cap);
}

// Simulate one game-tick: advance the clock (server_world_tick does current_tick++
// before draining), then drain under `budget`. Returns the number of ticks fired.
static size_t do_tick(struct server_world* w, uint32_t budget) {
	w->current_tick++;
	g_fires = 0;
	memset(g_fired_block, 0, sizeof(g_fired_block));
	memset(g_fired_x, 0, sizeof(g_fired_x));
	server_world_drain_scheduled(w, NULL, budget, count_fire, NULL);
	return (size_t)g_fires;
}

static bool fired_block(uint8_t type) {
	for(int i = 0; i < g_fires; i++)
		if(g_fired_block[i] == type)
			return true;
	return false;
}

int main(void) {
	struct server_world w;

	// === 1. CONSERVATION: N ready ticks drain at exactly `budget`/game-tick, over
	//        ceil(N/budget) ticks, none lost, none dropped, none extra, FIFO. ===
	const int Ns[] = {1, 3, 7, 10, 100, 1000};
	const uint32_t budgets[] = {1, 3, 128, 1000};
	for(size_t bi = 0; bi < sizeof(budgets) / sizeof(budgets[0]); bi++) {
		for(size_t ni = 0; ni < sizeof(Ns) / sizeof(Ns[0]); ni++) {
			int N = Ns[ni];
			uint32_t B = budgets[bi];
			setup(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
			for(int i = 0; i < N; i++) // ready ticks: delay 0 -> fire_at == current_tick
				assert(server_world_schedule_tick_raw(
					&w, i, 64, 0, ANY_BLOCK, 0));

			size_t total_fired = 0;
			int ticks = 0;
			while(w.scheduled_tick_count > 0) {
				size_t before = w.scheduled_tick_count;
				size_t fired = do_tick(&w, B);
				size_t expect = before < (size_t)B ? before : (size_t)B;
				assert(fired == expect);					 // exactly min(budget, left)
				assert(w.scheduled_tick_count == before - fired); // none lost/dup
				total_fired += fired;
				assert(++ticks <= N + 2); // hard stop vs an infinite loop
			}
			assert(total_fired == (size_t)N);			   // every tick fired once
			assert(w.scheduled_tick_drops == 0);		   // nothing dropped
			assert(ticks == (int)(((size_t)N + B - 1) / B)); // minimal spread
			server_world_scheduled_ticks_destroy(&w);
		}
	}
	printf("test_scheduled_tick_budget 1: conservation (lossless, FIFO, "
		   "ceil(N/budget) ticks) OK\n");

	// === 2. budget == 0 is UNLIMITED (legacy behaviour): all N fire in ONE game-tick.
	//        This is the freezing path the capped path above is the safe alternative to. ===
	setup(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	for(int i = 0; i < 4000; i++)
		assert(server_world_schedule_tick_raw(&w, i, 64, 0, ANY_BLOCK, 0));
	assert(do_tick(&w, 0) == 4000);		   // drained in a single tick
	assert(w.scheduled_tick_count == 0);
	assert(w.scheduled_tick_drops == 0);
	server_world_scheduled_ticks_destroy(&w);
	printf("test_scheduled_tick_budget 2: budget=0 fires all in one tick "
		   "(unlimited) OK\n");

	// === 3. FUTURE ticks never consume the budget and are never lost: requeued each
	//        game-tick until due, then fire exactly on schedule. ===
	{
		const int R = 20;	   // ready ticks (delay 0)
		const int F = 8;	   // future ticks (delay D)
		const uint32_t D = 50; // fire_at == 50
		const uint32_t B = 3;
		setup(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		for(int i = 0; i < R; i++)
			assert(server_world_schedule_tick_raw(
				&w, i, 64, 0, ANY_BLOCK, 0));
		for(int i = 0; i < F; i++)
			assert(server_world_schedule_tick_raw(
				&w, 1000 + i, 64, 0, ANY_BLOCK, D));

		// Drain the ready ticks; futures must NOT steal budget, so each game-tick still
		// fires min(B, ready-remaining) and the future count stays put.
		int ready_left = R;
		while(ready_left > 0) {
			size_t fired = do_tick(&w, B);
			size_t expect = (size_t)(ready_left < (int)B ? ready_left : (int)B);
			assert(fired == expect);
			ready_left -= (int)fired;
			assert(w.current_tick < D); // ready drains well before the futures are due
		}
		assert(w.scheduled_tick_count == (size_t)F); // exactly the futures remain
		assert(w.scheduled_tick_drops == 0);

		// Futures must NOT fire before they are due: while current_tick < D every tick
		// fires nothing and leaves all F queued (requeued, never dropped).
		while(w.current_tick < D - 1) {
			size_t fired = do_tick(&w, B);
			assert(fired == 0);
			assert(w.scheduled_tick_count == (size_t)F);
		}
		// current_tick is now D-1; the next tick makes them due (fire_at D <= current).
		// They then drain at the SAME budget rate as any ready tick: min(B, left) per
		// game-tick, ceil(F/B) ticks, none lost. (The budget applies to a future the
		// moment it becomes ready -- it gets no exemption.)
		int fut_left = F;
		while(fut_left > 0) {
			size_t before = w.scheduled_tick_count;
			size_t fired = do_tick(&w, B);
			size_t expect = before < (size_t)B ? before : (size_t)B;
			assert(fired == expect);
			fut_left -= (int)fired;
		}
		assert(w.scheduled_tick_count == 0);
		// First fire at current_tick == D, then ceil(F/B) ticks total.
		assert(w.current_tick == D + (uint32_t)((F + B - 1) / B) - 1);
		assert(w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_budget 3: futures never consume budget, never "
		   "lost, fire exactly when due OK\n");

	// === 4. CLASS-SPLIT Wii pacing: cap only flowing-fluid work. The hardware
	// capture put TNT/fire/portal-redstone behind a large fluid backlog, stretching
	// their Beta tick timing by seconds. Beta World.TickUpdates processes up to 1000
	// time/insertion-ordered entries per tick [decompile 740c583 World.TickUpdates;
	// NextTickListEntry.comparer]. The Wii's 128-fluid carve-out may spread confluent
	// fluid work, but must not spend that fluid budget on one-shot/timing-sensitive
	// TNT, fire, torch, or repeater entries in the same ready snapshot. Explosion
	// cap deferrals retain their source entity and never enter this ring.
	{
		const uint32_t B = 3;
		const int FLUIDS = 32;
		setup(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		for(int i = 0; i < FLUIDS; i++)
			assert(server_world_schedule_tick_raw(
				&w, i, 64, 0, (i & 1) ? BLOCK_WATER_FLOW : BLOCK_LAVA_FLOW, 0));
		assert(server_world_schedule_tick_raw(&w, 100, 64, 0, BLOCK_TNT, 0));
		assert(server_world_schedule_tick_raw(&w, 101, 64, 0, BLOCK_FIRE, 0));
		assert(server_world_schedule_tick_raw(
			&w, 102, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 0));
		assert(server_world_schedule_tick_raw(
			&w, 103, 64, 0, BLOCK_REDSTONE_REPEATER, 0));

		assert(do_tick(&w, B) == B + 4);
		for(int i = 0; i < (int)B; i++) {
			assert(g_fired_block[i] == ((i & 1) ? BLOCK_WATER_FLOW
											  : BLOCK_LAVA_FLOW));
		}
		assert(g_fired_block[B + 0] == BLOCK_TNT);
		assert(g_fired_block[B + 1] == BLOCK_FIRE);
		assert(g_fired_block[B + 2] == BLOCK_REDSTONE_TORCH_LIT);
		assert(g_fired_block[B + 3] == BLOCK_REDSTONE_REPEATER);
		assert(fired_block(BLOCK_TNT));
		assert(fired_block(BLOCK_FIRE));
		assert(fired_block(BLOCK_REDSTONE_TORCH_LIT));
		assert(fired_block(BLOCK_REDSTONE_REPEATER));
		assert(w.scheduled_tick_count == (size_t)(FLUIDS - B));
		assert(w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_budget 4: non-fluid timing ticks pass through "
		   "a capped fluid backlog OK\n");

	// === 5. The class split does not make non-fluid work unlimited. Beta clamps
	// World.TickUpdates to 1000 entries per game tick; retain that absolute ceiling
	// whenever the production budget is enabled. (budget==0 remains the explicit
	// legacy/unlimited diagnostic mode covered by case 2.)
	{
		const int FLUIDS = 4;
		const int FIRES = 1100;
		setup(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		for(int i = 0; i < FLUIDS; i++)
			assert(server_world_schedule_tick_raw(
				&w, i, 64, 0, (i & 1) ? BLOCK_WATER_FLOW : BLOCK_LAVA_FLOW, 0));
		for(int i = 0; i < FIRES; i++)
			assert(server_world_schedule_tick_raw(
				&w, 100 + i, 64, 0, BLOCK_FIRE, 0));
		assert(do_tick(&w, 3) == SERVER_WORLD_SCHEDULED_TICK_BETA_CAP);
		int first_fluids = 0;
		int first_fires = 0;
		for(int i = 0; i < g_fires; i++) {
			if(g_fired_block[i] == BLOCK_WATER_FLOW
			   || g_fired_block[i] == BLOCK_LAVA_FLOW)
				first_fluids++;
			else if(g_fired_block[i] == BLOCK_FIRE)
				first_fires++;
		}
		assert(first_fluids == 3);
		assert(first_fires == 997);
		assert(w.scheduled_tick_count == 104);
		assert(do_tick(&w, 3) == 104);
		assert(w.scheduled_tick_count == 0);
		assert(w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_budget 5: Beta absolute cap stays 1000/tick OK\n");

	// === 6. A future timing entry stays future, then passes through immediately
	// when due even though the flowing-fluid backlog is still capped. ===
	{
		setup(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		for(int i = 0; i < 10; i++)
			assert(server_world_schedule_tick_raw(
				&w, i, 64, 0, (i & 1) ? BLOCK_WATER_FLOW : BLOCK_LAVA_FLOW, 0));
		assert(server_world_schedule_tick_raw(&w, 100, 64, 0, BLOCK_TNT, 2));
		assert(do_tick(&w, 1) == 1);
		assert(!fired_block(BLOCK_TNT));
		assert(w.scheduled_tick_count == 10);
		assert(do_tick(&w, 1) == 2);
		assert(fired_block(BLOCK_TNT));
		assert(w.scheduled_tick_count == 8);
		while(w.scheduled_tick_count > 0)
			(void)do_tick(&w, 1);
		assert(w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_budget 6: future timing tick passes through when due OK\n");

	// === 7. Callback-enqueued children never overtake the older due-fluid cohort.
	// Stable precedence is required for conservation: without it, once the first
	// child becomes due it can repeatedly consume B=1 ahead of originals 5/6. ===
	{
		setup(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		for(int i = 0; i < 7; i++)
			assert(server_world_schedule_tick_raw(
				&w, i, 64, 0, BLOCK_WATER_FLOW, 0));
		g_rearm_fluids = true;
		for(int expected = 0; expected < 7; expected++) {
			assert(do_tick(&w, 1) == 1);
			assert(g_fired_x[0] == expected);
		}
		g_rearm_fluids = false;
		while(w.scheduled_tick_count > 0)
			(void)do_tick(&w, 0);
		assert(w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_budget 7: old fluid cohort precedes rearmed children OK\n");
	{
		struct server_world w;
		static struct server_local s;
		setup(&w, 16);
		s.pool_selftest_freeze = true;
		const uint8_t types[] = {BLOCK_WATER_FLOW, BLOCK_WATER_STILL,
			BLOCK_LAVA_FLOW, BLOCK_LAVA_STILL};
		for(unsigned i = 0; i < 4; i++)
			assert(server_world_schedule_tick_raw(&w, i, 64, 0, types[i], 1));
		assert(server_world_schedule_tick_raw(&w, 8, 64, 0, BLOCK_FIRE, 1));
		w.current_tick = 1;
		g_fires = 0;
		server_world_drain_scheduled(&w, &s, 0, count_fire, NULL);
		assert(g_fires == 1 && g_fired_block[0] == BLOCK_FIRE);
		assert(w.scheduled_tick_count == 4 && !w.scheduled_tick_drops);
		for(unsigned i = 0; i < 4; i++) {
			const struct scheduled_tick* t = &w.scheduled_ticks[
				(w.scheduled_tick_head + i) % w.scheduled_tick_capacity];
			assert(t->block_type == types[i] && t->x == (w_coord_t)i
				   && t->fire_at_tick == 1);
		}
		s.pool_selftest_freeze = false;
		g_fires = 0;
		server_world_drain_scheduled(&w, &s, 0, count_fire, NULL);
		assert(g_fires == 4 && w.scheduled_tick_count == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_budget 8: pool-only freeze retains all four fluid IDs; non-fluid and normal paths execute OK\n");

	printf("test_scheduled_tick_budget: all OK\n");
	return 0;
}
