/*
	Regression golden for the torch-burnout list across a WORLD RELOAD (M4 redstone, R1).

	The burnout list (redstone_logic.c s_torch_updates) is process-static and keyed only by
	(x,y,z) -- it survives an in-process world switch. Its 100-tick prune compares each entry's
	stamped .time against the live world clock (world.current_tick), which server_world_create
	resets to 0 on every (re)create. So after world A burns a torch out at a large tick and the
	player returns to the menu and loads world B (clock back at ~0), a stale entry's
	`t - .time` is NEGATIVE -- it can never satisfy `> 100` -- and a torch placed at a reused
	(x,y,z) in world B would load PRE-BURNT (stuck dark with no in-world cause).

	Two independent guards fix this; this test pins BOTH at the logic level:
	  A. redstone_torch_reset_burnout() clears the list (the production fix calls it at the
	     SRPC_UNLOAD_WORLD / world-load boundary, mirroring the next_entity_id reset).
	  B. the prune in redstone_torch_update also drops any entry whose .time is in the FUTURE
	     (t < .time) -- defence-in-depth for the non-monotonic session clock.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "block/redstone_logic.h"

#define TX 5
#define TY 64
#define TZ 5
#define TMETA 5 // floor mount: attachment is (x, y-1, z), side 0

static int g_torch_id;
static bool g_input_on;
static int64_t g_time;

static int b_get_type(void* c, int x, int y, int z) {
	(void)c;
	return (x == TX && y == TY && z == TZ) ? g_torch_id : 0;
}
static int b_get_meta(void* c, int x, int y, int z) {
	(void)c;
	return (x == TX && y == TY && z == TZ) ? TMETA : 0;
}
static void b_set_type(void* c, int x, int y, int z, int type) {
	(void)c;
	if(x == TX && y == TY && z == TZ)
		g_torch_id = type;
}
static bool b_is_ipp(void* c, int x, int y, int z, int side) {
	(void)c;
	(void)side;
	return (x == TX && y == TY - 1 && z == TZ) ? g_input_on : false;
}
static int64_t b_get_world_time(void* c) {
	(void)c;
	return g_time;
}

static struct redstone_world RS;

// One inverter cycle (records one lit->idle), like test_redstone_torch_burnout_golden.
static bool cycle(void) {
	g_input_on = true;
	redstone_torch_update(&RS, TX, TY, TZ);
	assert(g_torch_id == REDSTONE_TORCH_IDLE_ID);
	g_time += 2;
	g_input_on = false;
	redstone_torch_update(&RS, TX, TY, TZ);
	g_time += 2;
	return g_torch_id == REDSTONE_TORCH_LIT_ID;
}

// Drive the torch to burnout starting at world time `t0`. Returns with the torch stuck idle.
static void burn_out_at(int64_t t0) {
	g_time = t0;
	g_torch_id = REDSTONE_TORCH_LIT_ID;
	for(int i = 1; i <= 7; i++)
		assert(cycle()); // relights (< 8 records in the window)
	assert(!cycle());	 // 8th lit->idle -> burnt out, does NOT relight
	assert(g_torch_id == REDSTONE_TORCH_IDLE_ID);
}

int main(void) {
	RS = (struct redstone_world) {
		.get_type = b_get_type,
		.get_meta = b_get_meta,
		.set_type = b_set_type,
		.is_indirectly_providing_power = b_is_ipp,
		.get_world_time = b_get_world_time,
	};

	// --- Guard A: explicit reset clears the burnout list at the world boundary. ---
	redstone_torch_reset_burnout();
	burn_out_at(1000); // world A: torch burnt out, list holds 8 entries near t=1000

	redstone_torch_reset_burnout(); // <- the production reset on world unload/load
	// Same coords, still idle, input off: with the list cleared it must relight immediately.
	g_input_on = false;
	redstone_torch_update(&RS, TX, TY, TZ);
	assert(g_torch_id == REDSTONE_TORCH_LIT_ID); // reset un-stuck the torch

	// --- Guard B: a backwards clock (reload WITHOUT reset) prunes the stale future-dated entries. ---
	redstone_torch_reset_burnout();
	burn_out_at(1000); // world A again: 8 entries stamped ~t=1000, torch stuck idle

	// Simulate loading world B in-process: world clock resets to ~0, but the static list is NOT
	// cleared (exercise the prune guard alone). A torch at the reused coords must NOT be pre-burnt.
	g_time = 0;
	g_input_on = false;
	redstone_torch_update(&RS, TX, TY, TZ);
	assert(g_torch_id == REDSTONE_TORCH_LIT_ID); // future-dated entries pruned -> relights

	printf("test_redstone_torch_burnout_reload: OK (reset clears burnout at the world boundary; "
		   "the prune drops future-dated stale entries on a backwards session clock)\n");
	return 0;
}
