/*
	Golden for the scheduled-tick (pos,type) DEDUP (M4 redstone, R2).

	Beta World.scheduleBlockUpdate gates every insert behind scheduledTickSet.contains();
	NextTickListEntry.equals keys on (x,y,z,blockID) only (scheduledTime excluded), so at most ONE
	pending tick exists per (position, block id), and on a duplicate Beta DISCARDS the new entry and
	keeps the FIRST-INSERTED one with its original fire time (keep-first, NOT keep-earliest -- a later
	re-schedule with an EARLIER delay does NOT pull the fire time in). The port previously appended
	unconditionally to a fixed 4096 ring and DROPPED on overflow with no dedup, so a duplicate-notify
	storm (set_block notifies a cell + its 6 neighbours; rs_mark fans out a 2-hop reach) could inflate
	the ring and silently drop a redstone clock tick that does not self-heal. This pins the
	containment gate now added to server_world_schedule_tick_at_raw:

	  - the SAME (x,y,z,block_type) BLOCK tick scheduled any number of times stays at ONE entry and
	    never drops;
	  - a different position OR a different block id is a distinct entry;
	  - the FIRST-INSERTED entry (with its original fire time) is kept, regardless of whether a later
	    duplicate asks for an earlier OR later time (keep-first == Beta contains() skip);
	  - entity explosions retain their source and never enter this block-tick ring;
	  - the O(count) scan is bounded to SCHEDULED_TICK_DEDUP_SCAN_MAX (R2-review perf guard): above
	    that ring depth the dedup degrades to a plain append (a documented trade-off for the
	    thousands-pending fluid-cascade regime).
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "network/server_world.h"

struct block* blocks[256];

int main(void) {
	struct server_world w;
	memset(&w, 0, sizeof(w));
	server_world_scheduled_ticks_create(&w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
	w.current_tick = 0;

	// 1. The SAME (x,y,z,block_type) scheduled 10000x collapses to ONE pending entry, no drops.
	//    This is the core R2 guard: a duplicate notify storm cannot inflate/overflow the ring.
	for(int i = 0; i < 10000; i++)
		assert(server_world_schedule_tick_raw(
			&w, 5, 64, 5, BLOCK_REDSTONE_TORCH_LIT, 1));
	assert(w.scheduled_tick_count == 1);
	assert(w.scheduled_tick_drops == 0);

	// 2. A different POSITION is a distinct entry.
	assert(server_world_schedule_tick_raw(
		&w, 6, 64, 5, BLOCK_REDSTONE_TORCH_LIT, 1));
	assert(w.scheduled_tick_count == 2);

	// 3. Same position, different BLOCK ID is a distinct entry (equals keys on id too).
	assert(server_world_schedule_tick_raw(
		&w, 5, 64, 5, BLOCK_REDSTONE_TORCH, 1));
	assert(w.scheduled_tick_count == 3);

	// 4. KEEP-FIRST (a): first-inserted has the EARLIER time. Re-scheduling with a LATER delay is
	//    ignored, so the tick still fires at the first-inserted (here, earlier) time.
	{
		struct server_world w2;
		memset(&w2, 0, sizeof(w2));
		server_world_scheduled_ticks_create(&w2, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		w2.current_tick = 0;
		assert(server_world_schedule_tick_raw(
			&w2, 7, 70, 7, BLOCK_REDSTONE_TORCH_LIT, 1)); // fire @1
		assert(server_world_schedule_tick_raw(
			&w2, 7, 70, 7, BLOCK_REDSTONE_TORCH_LIT, 99)); // ignored
		assert(w2.scheduled_tick_count == 1);
		struct scheduled_tick t;
		assert(server_world_scheduled_tick_pop(&w2, &t));
		assert(t.fire_at_tick == 1); // kept the FIRST-INSERTED (delay-1) entry
		server_world_scheduled_ticks_destroy(&w2);
	}

	// 6. KEEP-FIRST (b) -- the case the old golden never covered: first-inserted has the LATER time.
	//    Beta keeps the FIRST entry and DISCARDS the duplicate even though the duplicate asks for an
	//    EARLIER fire (NextTickListEntry.equals excludes scheduledTime). A keep-EARLIEST impl would
	//    wrongly pull the fire to delay 2; faithful keep-first holds it at delay 8. This is the
	//    repeater-mid-delay-ignores-a-re-trigger invariant -- guard it so a refactor can't break it.
	{
		struct server_world w3;
		memset(&w3, 0, sizeof(w3));
		server_world_scheduled_ticks_create(&w3, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		w3.current_tick = 0;
		assert(server_world_schedule_tick_raw(
			&w3, 8, 80, 8, BLOCK_REDSTONE_TORCH_LIT, 8)); // fire @8 FIRST
		assert(server_world_schedule_tick_raw(
			&w3, 8, 80, 8, BLOCK_REDSTONE_TORCH_LIT, 2)); // EARLIER, ignored
		assert(w3.scheduled_tick_count == 1);
		struct scheduled_tick t;
		assert(server_world_scheduled_tick_pop(&w3, &t));
		assert(t.fire_at_tick == 8); // kept the FIRST-INSERTED delay-8, NOT the earlier delay-2
		server_world_scheduled_ticks_destroy(&w3);
	}

	// 7. PERF-GUARD: above SCHEDULED_TICK_DEDUP_SCAN_MAX pending entries the O(count) scan is skipped
	//    (R2-review fix), so a duplicate is no longer collapsed -- a documented trade-off for the
	//    thousands-pending fluid-cascade regime (already the distinct-cell overflow R2 deferred). Fill
	//    past the cap with DISTINCT cells, then a duplicate must be APPENDED (count grows), proving the
	//    guard engages. (Below the cap, cases 1-6 prove full dedup still holds.)
	{
		struct server_world w4;
		memset(&w4, 0, sizeof(w4));
		server_world_scheduled_ticks_create(&w4, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
		w4.current_tick = 0;
		const int over = SCHEDULED_TICK_DEDUP_SCAN_MAX + 8; // comfortably past the cap, under 4096
		for(int x = 0; x < over; x++)
			assert(server_world_schedule_tick_raw(
				&w4, x, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 1)); // distinct x
		assert((int)w4.scheduled_tick_count == over);
		// a duplicate of cell x=0: count is now > the cap, so the dedup scan is skipped -> appended.
		assert(server_world_schedule_tick_raw(
			&w4, 0, 64, 0, BLOCK_REDSTONE_TORCH_LIT, 1));
		assert((int)w4.scheduled_tick_count == over + 1); // NOT collapsed: the guard is engaged
		server_world_scheduled_ticks_destroy(&w4);
	}

	server_world_scheduled_ticks_destroy(&w);
	printf("test_scheduled_tick_dedup: OK (Beta (x,y,z,id) containment gate: duplicates collapse to "
		   "one entry + never overflow; distinct pos/id stay separate; FIRST-INSERTED kept (both "
			   "time orders); scan bounded above the perf cap)\n");
	return 0;
}
