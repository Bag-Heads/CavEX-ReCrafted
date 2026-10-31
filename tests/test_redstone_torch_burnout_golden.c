/*
	Golden behavior test for Beta 1.7.3 redstone-torch BURNOUT, authored before wiring the
	torch. Drives redstone_torch_update directly while controlling the world time and the
	input, exercising the static torchUpdates list + the 100-tick window prune.

	Beta citations (740c583, BlockRedstoneTorch.checkForBurnout / updateTick):
	  - A lit->idle toggle RECORDS (x,y,z,worldTime) into torchUpdates (checkForBurnout(...,true)).
	  - A relight (idle->lit) is BLOCKED when checkForBurnout(...,false) finds >= 8 entries at
	    that position -> the torch stays dark ("burns out").
	  - At the top of updateTick, entries with worldTime - updateTime > 100L are FIFO-pruned;
	    once enough age out, the torch can relight again.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "block/redstone_logic.h"

#define TX 5
#define TY 64
#define TZ 5
#define TMETA 5

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

// One inverter cycle: input ON (lit->idle, records a burnout entry), advance time, input OFF
// (idle->relight, unless burnt out), advance time. Returns true if the torch is LIT after the
// relight attempt (i.e. it did relight). Each updateTick advances time by 2 (the torch tick rate).
static bool cycle(void) {
	g_input_on = true;
	redstone_torch_update(&RS, TX, TY, TZ); // lit -> idle (record)
	assert(g_torch_id == REDSTONE_TORCH_IDLE_ID);
	g_time += 2;
	g_input_on = false;
	redstone_torch_update(&RS, TX, TY, TZ); // idle -> lit, unless >= 8 records in the window
	g_time += 2;
	return g_torch_id == REDSTONE_TORCH_LIT_ID;
}

int main(void) {
	redstone_torch_reset_burnout();
	RS = (struct redstone_world) {
		.get_type = b_get_type,
		.get_meta = b_get_meta,
		.set_type = b_set_type,
		.is_indirectly_providing_power = b_is_ipp,
		.get_world_time = b_get_world_time,
	};
	g_torch_id = REDSTONE_TORCH_LIT_ID;
	g_time = 0;

	// Cycles 1..7 each record one lit->idle and relight fine (< 8 entries). Total time ~28 < 100.
	for(int i = 1; i <= 7; i++)
		assert(cycle()); // relit each time

	// Cycle 8: the 8th lit->idle is the 8th record; the relight then sees 8 entries -> BLOCKED.
	bool relit = cycle();
	assert(!relit);								  // burnt out: did NOT relight
	assert(g_torch_id == REDSTONE_TORCH_IDLE_ID); // stuck dark

	// Even with the input held OFF, while burnt out it stays dark (still 8 entries in window).
	redstone_torch_update(&RS, TX, TY, TZ);
	assert(g_torch_id == REDSTONE_TORCH_IDLE_ID);

	// Age every entry past the 100-tick window, then a relight attempt succeeds again.
	g_time += 200; // all torchUpdates entries are now older than 100 ticks -> pruned
	g_input_on = false;
	redstone_torch_update(&RS, TX, TY, TZ);
	assert(g_torch_id == REDSTONE_TORCH_LIT_ID); // relights after the window clears

	printf("test_redstone_torch_burnout: OK (Beta >=8 lit->idle toggles within 100 ticks "
		   "block relight; recovers after the window clears)\n");
	return 0;
}
