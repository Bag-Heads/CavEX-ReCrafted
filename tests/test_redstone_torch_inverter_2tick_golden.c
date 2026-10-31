/*
	Golden behavior test for Beta 1.7.3 redstone-torch inversion + the 2-game-tick scheduled
	delay (the classic redstone-torch 0.1s delay), authored BEFORE wiring the torch.

	Drives the production torch logic (source/block/redstone_logic.c redstone_torch_update)
	through the REAL scheduled-tick queue (source/network/server_world_tick_queue.c, the
	test_scheduled_tick_budget harness: setup + do_tick + server_world_schedule_tick) over a
	stub torch cell -- proving the 2-tick timing end-to-end, not just the toggle.

	Beta citations (740c583, BlockRedstoneTorch):
	  - tickRate() = 2; onNeighborBlockChange => scheduleBlockUpdate(...,tickRate()).
	  - updateTick: input ON (attachment isIndirectlyPowered) => lit->idle; input OFF and not
	    burnt out => idle->lit. The torch is an INVERTER with a 2-tick scheduled delay.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/redstone_logic.h"
#include "network/server_world.h"

struct block* blocks[256]; // referenced by server_world_tick_queue.c; never dereferenced

#define TX 5
#define TY 64
#define TZ 5
#define TMETA 5 // mounted on the block below -> attachment cell is (TX, TY-1, TZ)

static int g_torch_id;	   // REDSTONE_TORCH_LIT_ID / _IDLE_ID
static bool g_input_on;	   // is the attachment block powered?
static struct server_world* g_w;

static int t_get_type(void* c, int x, int y, int z) {
	(void)c;
	return (x == TX && y == TY && z == TZ) ? g_torch_id : 0;
}
static int t_get_meta(void* c, int x, int y, int z) {
	(void)c;
	return (x == TX && y == TY && z == TZ) ? TMETA : 0;
}
static void t_set_type(void* c, int x, int y, int z, int type) {
	(void)c;
	if(x == TX && y == TY && z == TZ)
		g_torch_id = type;
}
static bool t_is_ipp(void* c, int x, int y, int z, int side) {
	(void)c;
	(void)side;
	return (x == TX && y == TY - 1 && z == TZ) ? g_input_on : false; // the mount block
}
static int64_t t_get_world_time(void* c) {
	(void)c;
	return (int64_t)g_w->current_tick;
}

static struct redstone_world RS;
static void fire(struct server_world* w, struct server_local* s, struct scheduled_tick t) {
	(void)w;
	(void)s;
	if(t.x == TX && t.y == TY && t.z == TZ)
		redstone_torch_update(&RS, TX, TY, TZ);
}
static void do_tick(struct server_world* w) {
	w->current_tick++; // server_world_tick increments before draining
	server_world_drain_scheduled(w, NULL, 0 /*unlimited: redstone goldens at budget=0*/,
								 fire, NULL);
}

int main(void) {
	redstone_torch_reset_burnout();
	struct server_world w;
	memset(&w, 0, sizeof(w));
	server_world_scheduled_ticks_create(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	g_w = &w;
	RS = (struct redstone_world) {
		.get_type = t_get_type,
		.get_meta = t_get_meta,
		.set_type = t_set_type,
		.is_indirectly_providing_power = t_is_ipp,
		.get_world_time = t_get_world_time,
	};

	g_torch_id = REDSTONE_TORCH_LIT_ID; // start lit
	g_input_on = false;					// input off (lit + off is stable)

	// --- ON edge: the attachment powers ON; the neighbour change schedules the torch at +2.
	g_input_on = true;
	assert(server_world_schedule_tick_raw(&w, TX, TY, TZ, REDSTONE_TORCH_LIT_ID,
									  REDSTONE_TORCH_TICK_RATE)); // fire_at = 0 + 2
	do_tick(&w);								   // t=1: not due
	assert(g_torch_id == REDSTONE_TORCH_LIT_ID); // still lit one tick after the input change
	do_tick(&w);								   // t=2: scheduled tick fires
	assert(g_torch_id == REDSTONE_TORCH_IDLE_ID); // OFF exactly 2 ticks after input ON

	// --- OFF edge: the attachment powers OFF; schedule at +2 again.
	g_input_on = false;
	assert(server_world_schedule_tick_raw(&w, TX, TY, TZ, REDSTONE_TORCH_IDLE_ID,
									  REDSTONE_TORCH_TICK_RATE)); // fire_at = 2 + 2 = 4
	do_tick(&w);								   // t=3: not due
	assert(g_torch_id == REDSTONE_TORCH_IDLE_ID); // still idle one tick after the input change
	do_tick(&w);								   // t=4: fires
	assert(g_torch_id == REDSTONE_TORCH_LIT_ID); // ON again exactly 2 ticks after input OFF

	server_world_scheduled_ticks_destroy(&w);
	printf("test_redstone_torch_inverter_2tick: OK (Beta inverter, 2-game-tick scheduled "
		   "delay on both edges)\n");
	return 0;
}
