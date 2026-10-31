/*
	Copyright (c) 2026

	This file is part of CavEX.
*/

#include <assert.h>
#include <stdlib.h>

#include "../block/blocks.h"
#include "../profile.h"
#include "server_world.h"
#include "server_local_state.h"

#define SCHEDULED_TICK_FLAG_DEFERRED_FLUID 0x01u

void server_world_scheduled_ticks_create(struct server_world* w,
										 size_t capacity) {
	assert(w && capacity > 0);

	// The stable fluid/non-fluid partition is part of the queue's timing
	// contract, not an optional optimization. Allocate entries + permutation
	// scratch as one ownership unit so a live queue can never silently fall back
	// to the old shared cap under partial OOM. The production capacity is 4096.
	w->scheduled_ticks = NULL;
	w->scheduled_tick_permutation = NULL;
	w->scheduled_tick_count = 0;
	w->scheduled_tick_head = 0;
	w->scheduled_tick_drops = 0;
	if(capacity > UINT16_MAX
	   || capacity > SIZE_MAX / (sizeof(struct scheduled_tick)
								  + sizeof(uint16_t))) {
		w->scheduled_tick_capacity = 0;
		return;
	}
	size_t entry_bytes = capacity * sizeof(struct scheduled_tick);
	w->scheduled_ticks = malloc(
		entry_bytes + capacity * sizeof(*w->scheduled_tick_permutation));
	if(!w->scheduled_ticks) {
		// MEM exhausted at world-create: run with a 0-capacity queue rather than
		// the NDEBUG-masked NULL deref. schedule_tick_at then drops every tick at
		// the `count >= capacity` guard (0 >= 0, BEFORE the `% capacity`), so
		// scheduled block ticks (fluid spread, fire, crops) silently no-op --
		// degraded simulation, never a crash. pop is unreachable (count stays 0).
		w->scheduled_tick_capacity = 0;
		return;
	}
	w->scheduled_tick_capacity = capacity;
	w->scheduled_tick_permutation
		= (uint16_t*)((unsigned char*)w->scheduled_ticks + entry_bytes);
}

void server_world_scheduled_ticks_destroy(struct server_world* w) {
	assert(w);
	free(w->scheduled_ticks);
	w->scheduled_ticks = NULL;
	w->scheduled_tick_permutation = NULL;
	w->scheduled_tick_capacity = 0;
	w->scheduled_tick_count = 0;
	w->scheduled_tick_head = 0;
}

static bool server_world_schedule_tick_at_policy(
	struct server_world* w, struct scheduled_tick tick, bool always_dedup) {
	assert(w && w->scheduled_ticks);
	tick.queue_flags = 0;

	// Beta World.scheduleBlockUpdate gates every insert behind scheduledTickSet.contains():
	// NextTickListEntry.equals keys on (x,y,z,blockID) ONLY (scheduledTime excluded), so at most ONE
	// pending tick exists per (position, block id). On a duplicate Beta DISCARDS the new entry and
	// KEEPS THE FIRST-INSERTED one with its original fire time -- a later re-schedule with an EARLIER
	// delay does NOT pull the fire time in (a repeater scheduled mid-delay must IGNORE a re-trigger
	// and fire at its original time; this is keep-FIRST, NOT keep-earliest). Mirror that for block
	// ticks: return on the first matching (x,y,z,block_type) entry, keeping the existing one.
	// This removes the duplicate-notify inflation (set_block notifies the cell + 6 neighbours,
	// rs_mark fans out a 2-hop reach) that could fill the 4096 ring and silently DROP a
	// non-self-healing redstone clock tick (redstone review R2). Entity explosions
	// retain their source under their own cap and never enter this ring. The scan is bounded to
	// SCHEDULED_TICK_DEDUP_SCAN_MAX -- see that define for the O(count)-vs-Beta-
	// HashSet perf rationale.
	if(always_dedup
	   || w->scheduled_tick_count <= SCHEDULED_TICK_DEDUP_SCAN_MAX) {
		for(size_t i = 0; i < w->scheduled_tick_count; i++) {
			size_t j = (w->scheduled_tick_head + i) % w->scheduled_tick_capacity;
			const struct scheduled_tick* e = &w->scheduled_ticks[j];
			if(e->x == tick.x && e->y == tick.y
			   && e->z == tick.z && e->block_type == tick.block_type)
				return true; // already pending: keep the FIRST-INSERTED entry (Beta contains() skips)
		}
	}

	if(w->scheduled_tick_count >= w->scheduled_tick_capacity) {
		w->scheduled_tick_drops++;
		return false;
	}

	size_t idx = (w->scheduled_tick_head + w->scheduled_tick_count)
		% w->scheduled_tick_capacity;
	w->scheduled_ticks[idx] = tick;
	w->scheduled_tick_count++;
	return true;
}

bool server_world_schedule_tick_at_raw(struct server_world* w,
									   struct scheduled_tick tick) {
	return server_world_schedule_tick_at_policy(w, tick, false);
}

bool server_world_schedule_tick_raw(struct server_world* w, w_coord_t x,
									w_coord_t y, w_coord_t z,
									uint8_t block_type, uint32_t delay) {
	return server_world_schedule_tick_at_raw(
		w,
		(struct scheduled_tick) {
			.x = x,
			.y = y,
			.z = z,
			.fire_at_tick = w->current_tick + delay,
			.block_type = block_type,
		});
}

static w_coord_t server_world_scheduled_block_to_chunk(int64_t block) {
	return block < 0 ? (w_coord_t)((block + 1) / 16 - 1)
					 : (w_coord_t)(block / 16);
}

/* Beta World.checkChunksExist accepts the box iff its Y interval overlaps
 * [0,127], then requires every intersected X/Z chunk to exist, for an
 * arbitrary caller-supplied radius (Beta itself calls this with two different
 * literal radii: 8 from scheduleBlockUpdate/TickUpdates, 32 from BlockSand.
 * tryToFall's own inline check before deciding entity-vs-direct-settle). Use
 * int64_t for the +/- radius arithmetic so even a malformed extreme
 * coordinate cannot overflow signed w_coord_t arithmetic. [Beta 1.7.3
 * decompile 740c583: World.checkChunksExist, World.scheduleBlockUpdate,
 * World.TickUpdates, BlockSand.tryToFall] */
static bool server_world_area_loaded_radius_impl(struct server_world* w,
											 w_coord_t x, w_coord_t y,
											 w_coord_t z, int64_t radius) {
	assert(w && radius >= 0);
	const int64_t min_y = (int64_t)y - radius;
	const int64_t max_y = (int64_t)y + radius;
	if(max_y < 0 || min_y >= WORLD_HEIGHT)
		return false;

	const w_coord_t min_cx
		= server_world_scheduled_block_to_chunk((int64_t)x - radius);
	const w_coord_t max_cx
		= server_world_scheduled_block_to_chunk((int64_t)x + radius);
	const w_coord_t min_cz
		= server_world_scheduled_block_to_chunk((int64_t)z - radius);
	const w_coord_t max_cz
		= server_world_scheduled_block_to_chunk((int64_t)z + radius);
	for(w_coord_t cx = min_cx; cx <= max_cx; cx++)
		for(w_coord_t cz = min_cz; cz <= max_cz; cz++)
			if(!dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz)))
				return false;
	return true;
}

bool server_world_scheduled_tick_area_loaded(struct server_world* w,
										 w_coord_t x, w_coord_t y,
										 w_coord_t z) {
	return server_world_area_loaded_radius_impl(w, x, y, z, 8);
}

/* Beta BlockSand.tryToFall's own `byte var8 = 32; ... checkChunksExist(x-32,
 * ..., x+32, ...)` -- a wider, SEPARATE check from the radius-8 gate that
 * admits the scheduled tick in the first place. Exported so BlockSand/
 * BlockGravel's fall trigger (block_sand.c) can reuse the exact same
 * predicate rather than re-deriving the chunk-existence loop. */
bool server_world_area_loaded_radius(struct server_world* w, w_coord_t x,
									 w_coord_t y, w_coord_t z,
									 uint32_t radius) {
	return server_world_area_loaded_radius_impl(w, x, y, z, (int64_t)radius);
}

bool server_world_schedule_tick(struct server_world* w, w_coord_t x,
								w_coord_t y, w_coord_t z,
								uint8_t block_type, uint32_t delay) {
	if(!server_world_scheduled_tick_area_loaded(w, x, y, z))
		return false;
	return server_world_schedule_tick_raw(w, x, y, z, block_type, delay);
}

bool server_world_schedule_block_tick_raw(struct server_world* w,
										  w_coord_t x, w_coord_t y,
										  w_coord_t z,
										  uint8_t block_type) {
	const struct block* b = blocks[block_type];
	if(!b || !b->onWorldTick)
		return false;

	uint32_t delay = b->tick_delay ? b->tick_delay : 1;
	return server_world_schedule_tick_raw(w, x, y, z, block_type, delay);
}

bool server_world_schedule_block_tick(struct server_world* w, w_coord_t x,
									  w_coord_t y, w_coord_t z,
									  uint8_t block_type) {
	const struct block* b = blocks[block_type];
	if(!b || !b->onWorldTick)
		return false;

	uint32_t delay = b->tick_delay ? b->tick_delay : 1;
	return server_world_schedule_tick(w, x, y, z, block_type, delay);
}

bool server_world_schedule_block_tick_rearm(struct server_world* w,
										w_coord_t x, w_coord_t y,
										w_coord_t z,
										uint8_t block_type) {
	const struct block* b = blocks[block_type];
	if(!b || !b->onWorldTick
	   || !server_world_scheduled_tick_area_loaded(w, x, y, z))
		return false;

	uint32_t delay = b->tick_delay ? b->tick_delay : 1;
	/* Chunk-load reconstruction can revisit an epoch while its old callback is
	 * still queued. It must retain Beta's keep-first identity even above the
	 * normal queue's Wii performance cutoff, or every retry can duplicate its
	 * prefix and starve later cells forever. */
	return server_world_schedule_tick_at_policy(
		w,
		(struct scheduled_tick) {
			.x = x,
			.y = y,
			.z = z,
			.fire_at_tick = w->current_tick + delay,
			.block_type = block_type,
		},
		true);
}

bool server_world_scheduled_tick_pop(struct server_world* w,
									 struct scheduled_tick* tick) {
	assert(w && tick && w->scheduled_ticks);

	if(w->scheduled_tick_count == 0)
		return false;

	*tick = w->scheduled_ticks[w->scheduled_tick_head];
	w->scheduled_tick_head
		= (w->scheduled_tick_head + 1) % w->scheduled_tick_capacity;
	w->scheduled_tick_count--;
	return true;
}

void server_world_scheduled_tick_requeue(struct server_world* w,
										 struct scheduled_tick tick) {
	assert(w && w->scheduled_ticks);
	// This is an already-owned queue entry, not a fresh schedule. The pop that
	// immediately precedes every requeue guarantees one free slot. Append it raw:
	// running normal dedup here can let a newer callback schedule replace the old
	// entry while it is temporarily absent from the ring.
	if(w->scheduled_tick_count >= w->scheduled_tick_capacity) {
		w->scheduled_tick_drops++;
		return;
	}
	size_t idx = (w->scheduled_tick_head + w->scheduled_tick_count)
		% w->scheduled_tick_capacity;
	w->scheduled_ticks[idx] = tick;
	w->scheduled_tick_count++;
}

static struct scheduled_tick* server_world_scheduled_tick_logical(
	struct server_world* w, size_t logical) {
	return &w->scheduled_ticks[(w->scheduled_tick_head + logical)
							 % w->scheduled_tick_capacity];
}

// Put old, due fluids skipped by the Wii sub-cap back ahead of both the
// unprocessed original suffix and callback-enqueued children. The permutation
// is stable within all three groups and uses 8 KiB of persistent scratch at the
// production 4096-entry capacity, not the server thread's 64 KiB stack.
static void server_world_restore_deferred_fluid_precedence(
	struct server_world* w, size_t remaining_original) {
	assert(w && w->scheduled_tick_permutation);
	const size_t count = w->scheduled_tick_count;
	assert(remaining_original <= count && count <= UINT16_MAX);
	uint16_t* destination = w->scheduled_tick_permutation;
	size_t next = 0;

	// Deferred entries came from the already-scanned original prefix, so they
	// are older than both groups that currently precede/follow them in the ring.
	for(size_t source = remaining_original; source < count; source++)
		if(server_world_scheduled_tick_logical(w, source)->queue_flags
		   & SCHEDULED_TICK_FLAG_DEFERRED_FLUID)
			destination[source] = (uint16_t)next++;
	for(size_t source = 0; source < remaining_original; source++)
		destination[source] = (uint16_t)next++;
	for(size_t source = remaining_original; source < count; source++)
		if(!(server_world_scheduled_tick_logical(w, source)->queue_flags
			 & SCHEDULED_TICK_FLAG_DEFERRED_FLUID))
			destination[source] = (uint16_t)next++;
	assert(next == count);

	// destination[source] is the destination of the element currently at source.
	// Swap whole elements and their destination labels until every cycle is fixed.
	for(size_t i = 0; i < count; i++) {
		while(destination[i] != i) {
			size_t j = destination[i];
			struct scheduled_tick tmp
				= *server_world_scheduled_tick_logical(w, i);
			*server_world_scheduled_tick_logical(w, i)
				= *server_world_scheduled_tick_logical(w, j);
			*server_world_scheduled_tick_logical(w, j) = tmp;
			uint16_t d = destination[i];
			destination[i] = destination[j];
			destination[j] = d;
		}
	}
	for(size_t i = 0; i < count; i++)
		server_world_scheduled_tick_logical(w, i)->queue_flags
			&= (uint8_t)~SCHEDULED_TICK_FLAG_DEFERRED_FLUID;
}

void server_world_drain_scheduled(
	struct server_world* w, struct server_local* s, uint32_t budget,
	void (*fire)(struct server_world*, struct server_local*, struct scheduled_tick),
	void (*yield)(void)) {
	assert(w && fire);

	// Snapshot the queue length so ticks ENQUEUED during this drain (a fluid cell
	// that flows schedules its neighbours) are deferred to the next game-tick.
	const size_t entries = w->scheduled_tick_count;
	uint32_t fluid_fired = 0;
	uint32_t total_fired = 0;
	bool deferred_fluid = false;
	size_t processed = 0;

	for(; processed < entries; processed++) {
		if(server_world_simulation_stopped(w) || w->light_fault)
			break; /* G9 R3: a light stop ends the loop at once */
		// Beta World.tickUpdates executes at most 1000 due callbacks per tick.
		if(budget && total_fired >= SERVER_WORLD_SCHEDULED_TICK_BETA_CAP)
			break;
		struct scheduled_tick tick;
		if(!server_world_scheduled_tick_pop(w, &tick))
			break;

		if(tick.fire_at_tick > w->current_tick) {
			// A future entry consumes neither the fluid cap nor Beta's fire cap.
			server_world_scheduled_tick_requeue(w, tick);
			continue;
		}

		const bool flowing_fluid = tick.block_type == BLOCK_WATER_FLOW
			|| tick.block_type == BLOCK_LAVA_FLOW;
		if(s && s->pool_selftest_freeze
		   && (flowing_fluid || tick.block_type == BLOCK_WATER_STILL
			   || tick.block_type == BLOCK_LAVA_STILL)) {
			/* R6: retain the original due entry, including its timestamp.
			 * Do not consume it via a no-op callback or block other work. */
			server_world_scheduled_tick_requeue(w, tick);
			continue;
		}
		if(budget && flowing_fluid && fluid_fired >= budget) {
			// Keep scanning the initial snapshot: timing-sensitive non-fluid
			// entries behind a settling fluid backlog must still fire on time.
			tick.queue_flags |= SCHEDULED_TICK_FLAG_DEFERRED_FLUID;
			server_world_scheduled_tick_requeue(w, tick);
			deferred_fluid = true;
			continue;
		}

		tick.queue_flags = 0;
		fire(w, s, tick);
		total_fired++;
		if(flowing_fluid)
			fluid_fired++;

		// PACING (single-core Wii): yield every 4 fires so the render thread takes a frame
		// mid-cascade. The yield keeps render alive DURING a batch; the budget bounds the
		// batch SIZE. Together they turn the old multi-second freeze into a smooth settle.
		if(yield && total_fired % 4 == 0)
			yield();
	}

	if(deferred_fluid)
		server_world_restore_deferred_fluid_precedence(
			w, entries - processed);
}

bool server_world_try_begin_explosion(struct server_world* w) {
	assert(w);
	if(w->explosions_this_tick >= FCAVEX_MAX_EXPLOSIONS_PER_TICK)
		return false;
	w->explosions_this_tick++;
	return true;
}

void (*server_column_observe_update)(struct server_world*);

void server_world_begin_update(struct server_world* w) {
	assert(w);
	/* Observe the completed preceding update even while the simulation clock
	 * is frozen for loading/respawn. Never change tick-queue scheduling. */
	if(server_column_observe_update) server_column_observe_update(w);
	w->explosions_this_tick = 0;
}
