/*
	Golden (HUNT-14 item 10, Bucket A): the scheduled-tick queue keeps Beta's two invariants
	at ANY depth, not just below a perf cutoff.

	1. ALWAYS dedup. World.scheduleBlockUpdate [740c583 World.java:1144-1168] inserts only
	   `if(!this.scheduledTickSet.contains(var6))` -- a HashSet keyed by
	   NextTickListEntry.equals/hashCode = (xCoord, yCoord, zCoord, blockID)
	   [NextTickListEntry.java:17-30]. There is no depth at which the containment check is
	   skipped. The port used to skip its O(count) scan above SCHEDULED_TICK_DEDUP_SCAN_MAX
	   (256) pending entries, so a duplicate (x,y,z,id) could enter the ring and fire twice.

	2. Same-tick entries fire in INSERTION order. TickUpdates walks the TreeSet ordered by
	   NextTickListEntry.comparer [NextTickListEntry.java:37-39]: scheduledTime first, then
	   tickEntryID (a global insertion counter, :8-16). Entries that are not yet due are left
	   in the set, so an entry inserted LATER (a child scheduled by a callback this tick) can
	   never fire before an older entry with the same scheduledTime. The port's FIFO ring
	   pops a not-yet-due entry and re-appends it at the TAIL, behind children enqueued by
	   callbacks fired in between -- the older entry then fires AFTER the newer child.

	Driven through server_world_drain_scheduled with a recording fire callback (the queue TU
	only, like test_scheduled_tick_budget).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "network/server_world.h"
#include "network/server_local_state.h"

struct block* blocks[256]; // referenced by server_world_tick_queue.c; never dereferenced

static int g_fires;
static w_coord_t g_fired_x[SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY];
static uint8_t g_fired_type[SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY];
// Children to schedule from inside a fire: when entry x == g_child_parent_x fires, schedule
// (g_child_x, delay g_child_delay).
static w_coord_t g_child_parent_x = -1;
static w_coord_t g_child_x;
static uint32_t g_child_delay;
// Case 4 mode: every parent x < 10 schedules child (200 + x) at g_child_delay.
static bool g_child_per_parent;

static void record_fire(struct server_world* w, struct server_local* s,
						struct scheduled_tick t) {
	(void)s;
	assert(g_fires < SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	g_fired_x[g_fires] = t.x;
	g_fired_type[g_fires] = t.block_type;
	g_fires++;
	if(t.x == g_child_parent_x)
		assert(server_world_schedule_tick_raw(w, g_child_x, t.y, t.z, t.block_type,
											  g_child_delay));
	if(g_child_per_parent && t.x < 10)
		assert(server_world_schedule_tick_raw(w, 200 + t.x, t.y, t.z, t.block_type,
											  g_child_delay));
}

static void setup(struct server_world* w) {
	memset(w, 0, sizeof(*w));
	server_world_scheduled_ticks_create(w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	g_child_parent_x = -1;
	g_child_per_parent = false;
}

static int do_tick_budget(struct server_world* w, uint32_t budget) {
	w->current_tick++;
	g_fires = 0;
	server_world_drain_scheduled(w, NULL, budget, record_fire, NULL);
	return g_fires;
}
// budget 0 = unlimited (no Wii fluid cap, no Beta 1000 ceiling): fire everything due.
static int do_tick(struct server_world* w) {
	return do_tick_budget(w, 0);
}

int main(void) {
	struct server_world w;

	// ===== 1. Dedup holds ABOVE 256 pending entries (the old scan cutoff). =====
	{
		setup(&w);
		const int deep = SCHEDULED_TICK_DEDUP_SCAN_MAX + 300; // 556 distinct cells
		for(int x = 0; x < deep; x++)
			assert(server_world_schedule_tick_raw(&w, x, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 1));
		assert((int)w.scheduled_tick_count == deep);
		// Duplicates of the first, a middle and the last cell: contains() -> all discarded.
		assert(server_world_schedule_tick_raw(&w, 0, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 1));
		assert(server_world_schedule_tick_raw(&w, deep / 2, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 7));
		assert(server_world_schedule_tick_raw(&w, deep - 1, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 1));
		assert((int)w.scheduled_tick_count == deep
			   && "World.scheduleBlockUpdate dedups (x,y,z,id) at every depth");
		// Distinct id at an existing cell is still a new entry (equals keys on blockID).
		assert(server_world_schedule_tick_raw(&w, 0, 64, 0, BLOCK_REDSTONE_TORCH, 1));
		assert((int)w.scheduled_tick_count == deep + 1);
		// Fire them all: each cell exactly once.
		assert(do_tick(&w) == deep + 1);
		for(int i = 0; i < deep; i++)
			assert(g_fired_x[i] == i && g_fired_type[i] == BLOCK_REDSTONE_TORCH_LIT);
		assert(w.scheduled_tick_count == 0 && w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_order_golden 1: dedup above 256 pending OK\n");

	// ===== 2. Dedup still holds after the ring has WRAPPED and after pops (set == ring). =====
	{
		setup(&w);
		// Push/pop cycles move head around the ring; the containment set must track it.
		for(int round = 0; round < 6; round++) {
			for(int x = 0; x < 1500; x++)
				assert(server_world_schedule_tick_raw(&w, x, 64, round, BLOCK_WATER_FLOW, 0));
			assert(w.scheduled_tick_count == 1500);
			assert(server_world_schedule_tick_raw(&w, 7, 64, round, BLOCK_WATER_FLOW, 0));
			assert(w.scheduled_tick_count == 1500); // duplicate discarded
			// A budgeted drain stops at Beta's 1000/tick ceiling, leaving 500 pending with the
			// ring head moved by 1000 -- the set must track both the pops and the suffix.
			assert(do_tick_budget(&w, SERVER_WORLD_SCHEDULED_TICK_BETA_CAP) == 1000);
			assert(w.scheduled_tick_count == 500);
			// ...a cell that already FIRED is gone from the set: it may be scheduled again
			assert(server_world_schedule_tick_raw(&w, 7, 64, round, BLOCK_WATER_FLOW, 0));
			assert(w.scheduled_tick_count == 501);
			// ...but a still-pending one is not.
			assert(server_world_schedule_tick_raw(&w, 1400, 64, round, BLOCK_WATER_FLOW, 0));
			assert(w.scheduled_tick_count == 501);
			assert(do_tick(&w) == 501);
			assert(w.scheduled_tick_count == 0);
		}
		assert(w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_order_golden 2: dedup tracks pops and ring wrap OK\n");

	// ===== 3. Same-tick insertion order survives a not-yet-due requeue. =====
	// A(x=1,@2) B(x=2,@1) C(x=3,@2); firing B schedules child D(x=4,@2).
	// Beta TreeSet at tick 2: A(id0) C(id2) D(id3) -> fires 1,3,4. A tail requeue gives 1,4,3.
	{
		setup(&w);
		assert(server_world_schedule_tick_raw(&w, 1, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 2));
		assert(server_world_schedule_tick_raw(&w, 2, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 1));
		assert(server_world_schedule_tick_raw(&w, 3, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 2));
		g_child_parent_x = 2;
		g_child_x = 4;
		g_child_delay = 1; // scheduled at tick 1 -> due at 2, same as A and C
		assert(do_tick(&w) == 1 && g_fired_x[0] == 2);
		g_child_parent_x = -1;
		assert(w.scheduled_tick_count == 3);
		assert(do_tick(&w) == 3);
		assert(g_fired_x[0] == 1);
		assert(g_fired_x[1] == 3
			   && "an older same-tick entry fires BEFORE a child inserted later "
				  "(NextTickListEntry.comparer: scheduledTime, then tickEntryID)");
		assert(g_fired_x[2] == 4);
		assert(w.scheduled_tick_count == 0 && w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_order_golden 3: requeue keeps insertion order ahead of children OK\n");

	// ===== 4. Many not-yet-due entries interleaved with due ones that spawn children, over
	//          several ticks: the fire order each tick is exactly insertion order. =====
	{
		setup(&w);
		// 40 futures due @3 (x=100..139) with 10 due-@1 parents (x=0..9) interleaved; each
		// parent's child (x=200+parent) is scheduled from its fire at tick 1, ALSO due @3.
		for(int i = 0; i < 10; i++) {
			for(int k = 0; k < 4; k++)
				assert(server_world_schedule_tick_raw(&w, 100 + i * 4 + k, 64, 0,
													  BLOCK_REDSTONE_TORCH_LIT, 3));
			assert(server_world_schedule_tick_raw(&w, i, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 1));
		}
		g_child_per_parent = true;
		g_child_delay = 2; // inserted at tick 1 -> due @3
		assert(do_tick(&w) == 10); // tick 1: the parents, in order
		for(int i = 0; i < 10; i++)
			assert(g_fired_x[i] == i);
		g_child_per_parent = false;
		assert(w.scheduled_tick_count == 50);
		assert(do_tick(&w) == 0); // tick 2: nothing due; 50 requeues must keep their order
		assert(w.scheduled_tick_count == 50);
		assert(do_tick(&w) == 50); // tick 3: futures (older) first, then the children
		for(int i = 0; i < 40; i++)
			assert(g_fired_x[i] == 100 + i);
		for(int i = 0; i < 10; i++)
			assert(g_fired_x[40 + i] == 200 + i);
		assert(w.scheduled_tick_drops == 0);
		server_world_scheduled_ticks_destroy(&w);
	}
	printf("test_scheduled_tick_order_golden 4: multi-tick requeue keeps insertion order OK\n");

	printf("test_scheduled_tick_order_golden: all OK\n");
	return 0;
}
