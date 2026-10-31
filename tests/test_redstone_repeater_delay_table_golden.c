/*
	Golden behavior test for Beta 1.7.3's redstone repeater delay table (M4 redstone step 6),
	authored before wiring the repeater into the game (golden-suite-first).

	Drives the pure source/block/redstone_repeater.c through the REAL scheduled-tick queue
	(source/network/server_world_tick_queue.c, the test_scheduled_tick_budget harness) over a
	stub repeater cell -- proving the metadata-selected delay END-TO-END, not just the toggle.

	Beta citations (740c583, BlockRedstoneRepeater):
	  - field_22023_b = {1,2,3,4}; the scheduled delay is field_22023_b[idx] * 2 -> {2,4,6,8}
	    game-ticks (idx = (meta & 12) >> 2). The * 2 is applied ONCE (the classic trap is to
	    double it).
	  - onNeighborBlockChange arms scheduleBlockUpdate when the input and output states disagree;
	    updateTick performs the idle<->active swap. BOTH edges are delayed.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/redstone_repeater.h"
#include "network/server_world.h"

struct block* blocks[256]; // referenced by server_world_tick_queue.c; never dereferenced

#define RX 5
#define RY 64
#define RZ 5

static int g_rep_id;	 // RS_REPEATER_IDLE_ID / _ACTIVE_ID
static int g_rep_meta;	 // facing (low 2 bits) | delay index (bits 2-3)
static bool g_input_on;	 // is the repeater's input side powered?
static struct server_world* g_w;

static bool is_rep(int x, int y, int z) {
	return x == RX && y == RY && z == RZ;
}
static int t_get_type(void* c, int x, int y, int z) {
	(void)c;
	return is_rep(x, y, z) ? g_rep_id : 0;
}
static int t_get_meta(void* c, int x, int y, int z) {
	(void)c;
	return is_rep(x, y, z) ? g_rep_meta : 0;
}
static bool t_is_ipp(void* c, int x, int y, int z, int side) {
	(void)c;
	(void)side;
	// facing 0 -> input cell is (RX, RY, RZ+1).
	return (x == RX && y == RY && z == RZ + 1) ? g_input_on : false;
}
static void t_set_active(void* c, int x, int y, int z, bool active) {
	(void)c;
	if(is_rep(x, y, z))
		g_rep_id = active ? RS_REPEATER_ACTIVE_ID : RS_REPEATER_IDLE_ID;
}
static void t_schedule(void* c, int x, int y, int z, int id, int delay) {
	(void)c;
	assert(server_world_schedule_tick_raw(g_w, x, y, z, id, delay));
}

static struct redstone_repeater_world RW;
static void fire(struct server_world* w, struct server_local* s, struct scheduled_tick t) {
	(void)w;
	(void)s;
	if(is_rep(t.x, t.y, t.z))
		redstone_repeater_tick(&RW, RX, RY, RZ);
}
static void do_tick(struct server_world* w) {
	w->current_tick++; // server_world_tick increments before draining
	server_world_drain_scheduled(w, NULL, 0 /*unlimited: redstone goldens at budget=0*/, fire,
								 NULL);
}

int main(void) {
	struct server_world w;
	memset(&w, 0, sizeof(w));
	server_world_scheduled_ticks_create(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	g_w = &w;
	RW = (struct redstone_repeater_world) {
		.get_type = t_get_type,
		.get_meta = t_get_meta,
		.is_indirectly_providing_power = t_is_ipp,
		.set_active = t_set_active,
		.schedule = t_schedule,
	};

	// The delay table itself, byte-for-byte (field_22023_b * 2, applied once).
	assert(redstone_repeater_delay[0] == 2);
	assert(redstone_repeater_delay[1] == 4);
	assert(redstone_repeater_delay[2] == 6);
	assert(redstone_repeater_delay[3] == 8);

	for(int idx = 0; idx < 4; idx++) {
		int delay = redstone_repeater_delay[idx]; // 2, 4, 6, 8
		g_rep_meta = (idx << 2) | 0;			  // facing 0 (input from +Z)
		g_rep_id = RS_REPEATER_IDLE_ID;
		g_input_on = false; // idle + input off is the stable starting state

		// --- RISING edge: input ON -> the output turns ON exactly `delay` ticks later ----------
		g_input_on = true;
		redstone_repeater_neighbour_change(&RW, RX, RY, RZ); // arms the toggle at +delay
		for(int k = 0; k < delay - 1; k++) {
			do_tick(&w);
			assert(g_rep_id == RS_REPEATER_IDLE_ID); // still idle every tick before the delay
		}
		do_tick(&w);
		assert(g_rep_id == RS_REPEATER_ACTIVE_ID); // ON at exactly `delay` -- not 2x, not early

		// --- FALLING edge: input OFF -> the output turns OFF exactly `delay` ticks later --------
		g_input_on = false;
		redstone_repeater_neighbour_change(&RW, RX, RY, RZ);
		for(int k = 0; k < delay - 1; k++) {
			do_tick(&w);
			assert(g_rep_id == RS_REPEATER_ACTIVE_ID); // still active every tick before the delay
		}
		do_tick(&w);
		assert(g_rep_id == RS_REPEATER_IDLE_ID); // OFF at exactly `delay` (both edges delayed)
	}

	server_world_scheduled_ticks_destroy(&w);
	printf("test_redstone_repeater_delay_table: OK (Beta delay table {2,4,6,8} game-ticks, *2 "
		   "applied once, both edges delayed through the real scheduled-tick queue)\n");
	return 0;
}
