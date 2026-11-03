/*
	Copyright (c) 2026

	This file is part of CavEX.
*/

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../block/blocks.h"
#include "../profile.h"
#include "server_world.h"
#include "server_local_state.h"

// Set on an original entry the drain popped but did not fire (not yet due, freeze-
// retained, or a budget-deferred fluid). Such an entry is left IN PLACE in the popped
// region [head - kept_span, head) and is still pending (it stays in the containment
// set). When the drain ends the kept entries are compacted to sit directly before the
// head, i.e. ahead of the unprocessed suffix and of callback-enqueued children: Beta
// NextTickListEntry.comparer order (scheduledTime, then tickEntryID) with no reorder
// pass. In the saturated-ring fallback (see server_world_scheduled_tick_drain_fallback)
// the same bit marks those originals after they were re-appended to the live window.
// Cleared before the drain returns.
#define SCHEDULED_TICK_FLAG_KEPT 0x01u

// server_world.scheduled_tick_drain_mode
#define SCHEDULED_TICK_DRAIN_IDLE 0u
#define SCHEDULED_TICK_DRAIN_IN_PLACE 1u
#define SCHEDULED_TICK_DRAIN_REQUEUE 2u

// ---- Beta World.scheduledTickSet: (x,y,z,id) containment over the ring -----------------
// Slot values (uint16): 0 empty, SCHEDULED_TICK_HASH_TOMB tombstone, else physical index + 1.
// A slot is only TRUSTED when the physical index it names is a pending entry (inside the
// live window [head, head + count), or a kept entry of the running drain) AND that entry
// still holds the probed key. A ring reset that bypassed server_world_scheduled_ticks_resync
// therefore cannot make a stale slot report "already pending" (it is skipped and probing
// continues); identity is always the full (x,y,z,id) tuple, only the hash value wraps.
#define SCHEDULED_TICK_HASH_TOMB 0xFFFFu

static uint32_t server_world_scheduled_tick_hash_key(w_coord_t x, w_coord_t y,
													 w_coord_t z, uint8_t id) {
	// NextTickListEntry.hashCode [740c583 NextTickListEntry.java:28-30]
	// `(x * 128 * 1024 + z * 128 + y) * 256 + blockID` in Java int arithmetic, i.e. two's
	// complement wrap. Done in uint32_t: same bit pattern, no signed-overflow UB for
	// |x| >= 64 (the sanitizer build aborted on it). Then a 32-bit finaliser so the
	// power-of-two mask sees every key bit (Java's HashMap also spreads).
	uint32_t h = (((uint32_t)x * 131072u + (uint32_t)z * 128u + (uint32_t)y) * 256u)
		+ (uint32_t)id;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}

static bool server_world_scheduled_tick_same_key(const struct scheduled_tick* e,
												 w_coord_t x, w_coord_t y,
												 w_coord_t z, uint8_t id) {
	return e->x == x && e->y == y && e->z == z && e->block_type == id;
}

// Is physical ring index p a pending entry? The live window, or (only while a drain
// runs) a kept original in the popped region just behind the head.
static bool server_world_scheduled_tick_pending_at(const struct server_world* w,
												   size_t p) {
	const size_t cap = w->scheduled_tick_capacity;
	if(p >= cap)
		return false;
	const size_t offset = (p + cap - w->scheduled_tick_head) % cap;
	if(offset < w->scheduled_tick_count)
		return true;
	return w->scheduled_tick_kept_span > 0
		&& offset >= cap - w->scheduled_tick_kept_span
		&& (w->scheduled_ticks[p].queue_flags & SCHEDULED_TICK_FLAG_KEPT);
}

// Physical ring index of the pending entry with this key, or SIZE_MAX.
static size_t server_world_scheduled_tick_hash_find(const struct server_world* w,
													w_coord_t x, w_coord_t y,
													w_coord_t z, uint8_t id) {
	if((w->scheduled_tick_count == 0 && w->scheduled_tick_kept == 0)
	   || !w->scheduled_tick_hash)
		return SIZE_MAX;
	const size_t mask = w->scheduled_tick_hash_cap - 1;
	size_t slot = server_world_scheduled_tick_hash_key(x, y, z, id) & mask;
	for(size_t probes = 0; probes < w->scheduled_tick_hash_cap; probes++) {
		uint16_t v = w->scheduled_tick_hash[slot];
		if(v == 0)
			return SIZE_MAX;
		if(v != SCHEDULED_TICK_HASH_TOMB
		   && server_world_scheduled_tick_pending_at(w, (size_t)v - 1)
		   && server_world_scheduled_tick_same_key(&w->scheduled_ticks[v - 1], x,
												   y, z, id))
			return (size_t)v - 1;
		slot = (slot + 1) & mask;
	}
	return SIZE_MAX;
}

static void server_world_scheduled_tick_hash_insert_raw(struct server_world* w,
														size_t physical) {
	const size_t mask = w->scheduled_tick_hash_cap - 1;
	const struct scheduled_tick* e = &w->scheduled_ticks[physical];
	size_t slot
		= server_world_scheduled_tick_hash_key(e->x, e->y, e->z, e->block_type)
		& mask;
	while(w->scheduled_tick_hash[slot] != 0
		  && w->scheduled_tick_hash[slot] != SCHEDULED_TICK_HASH_TOMB)
		slot = (slot + 1) & mask;
	if(w->scheduled_tick_hash[slot] == 0)
		w->scheduled_tick_hash_used++;
	w->scheduled_tick_hash[slot] = (uint16_t)(physical + 1);
}

void server_world_scheduled_ticks_resync(struct server_world* w) {
	assert(w);
	if(!w->scheduled_tick_hash)
		return;
	memset(w->scheduled_tick_hash, 0,
		   w->scheduled_tick_hash_cap * sizeof(*w->scheduled_tick_hash));
	w->scheduled_tick_hash_used = 0;
	const size_t cap = w->scheduled_tick_capacity;
	for(size_t i = 0; i < w->scheduled_tick_count; i++)
		server_world_scheduled_tick_hash_insert_raw(
			w, (w->scheduled_tick_head + i) % cap);
	// Mid-drain (a populate rollback inside a fired callback): the kept originals
	// behind the head are still pending.
	for(size_t j = 0; j < w->scheduled_tick_kept_span; j++) {
		size_t p = (w->scheduled_tick_head + cap - w->scheduled_tick_kept_span + j)
			% cap;
		if(w->scheduled_ticks[p].queue_flags & SCHEDULED_TICK_FLAG_KEPT)
			server_world_scheduled_tick_hash_insert_raw(w, p);
	}
}

static void server_world_scheduled_tick_hash_insert(struct server_world* w,
													size_t physical) {
	// Tombstones count toward the load; a rebuild drops them. hash_cap >= 2 *
	// capacity, so live entries alone never exceed half the table.
	if((w->scheduled_tick_hash_used + 1) * 4 > w->scheduled_tick_hash_cap * 3) {
		server_world_scheduled_ticks_resync(w);
		return; // the entry at `physical` is already pending and was re-inserted
	}
	server_world_scheduled_tick_hash_insert_raw(w, physical);
}

// Slot that names physical index p for the entry's own key (the probe sequence of
// that key), or SIZE_MAX. Requiring the key as well as the index means a stale slot
// left by a different entry that once occupied p is never the one acted on.
static size_t server_world_scheduled_tick_hash_slot_of(
	const struct server_world* w, size_t physical) {
	const struct scheduled_tick* e = &w->scheduled_ticks[physical];
	const size_t mask = w->scheduled_tick_hash_cap - 1;
	size_t slot
		= server_world_scheduled_tick_hash_key(e->x, e->y, e->z, e->block_type)
		& mask;
	for(size_t probes = 0; probes < w->scheduled_tick_hash_cap; probes++) {
		uint16_t v = w->scheduled_tick_hash[slot];
		if(v == 0)
			return SIZE_MAX;
		if(v == (uint16_t)(physical + 1))
			return slot;
		slot = (slot + 1) & mask;
	}
	return SIZE_MAX;
}

static void server_world_scheduled_tick_hash_remove(struct server_world* w,
													size_t physical) {
	size_t slot = server_world_scheduled_tick_hash_slot_of(w, physical);
	assert(slot != SIZE_MAX && "containment set out of sync with the ring");
	if(slot != SIZE_MAX)
		w->scheduled_tick_hash[slot] = SCHEDULED_TICK_HASH_TOMB;
}

// The pending entry at physical `from` is being moved to `to` (same key).
static void server_world_scheduled_tick_hash_move(struct server_world* w,
												  size_t from, size_t to) {
	size_t slot = server_world_scheduled_tick_hash_slot_of(w, from);
	assert(slot != SIZE_MAX && "containment set out of sync with the ring");
	if(slot != SIZE_MAX)
		w->scheduled_tick_hash[slot] = (uint16_t)(to + 1);
}

// Move the kept originals of the popped region [head - span, head) up so they sit
// directly below the head, stable (top-down), and shrink the region to them. Slots
// [head - span, head - span + holes_top) may hold fired holes; everything above is
// already kept and contiguous, so only entries below the topmost hole move. Each
// moved entry retargets its own containment slot (O(1)), no rebuild.
static void server_world_scheduled_tick_compact_kept(struct server_world* w,
													 size_t holes_top) {
	const size_t cap = w->scheduled_tick_capacity;
	const size_t span = w->scheduled_tick_kept_span;
	if(holes_top > span)
		holes_top = span;
	const size_t base = (w->scheduled_tick_head + cap - span) % cap;
	size_t wr = holes_top; // relative write cursor (exclusive), moves down
	for(size_t idx = holes_top; idx-- > 0;) {
		size_t from = (base + idx) % cap;
		if(!(w->scheduled_ticks[from].queue_flags & SCHEDULED_TICK_FLAG_KEPT))
			continue;
		wr--;
		if(wr != idx) {
			size_t to = (base + wr) % cap;
			server_world_scheduled_tick_hash_move(w, from, to);
			w->scheduled_ticks[to] = w->scheduled_ticks[from];
			w->scheduled_ticks[from].queue_flags = 0; // now a hole
		}
	}
	assert(span - wr == w->scheduled_tick_kept);
	w->scheduled_tick_kept_span = span - wr;
}

// Saturated ring mid-drain: a fired callback appends while the live window plus the
// popped region fill the ring, so the only free slots are fired holes among the kept
// originals. Closing them per append would cost O(kept) per fire. Instead switch this
// drain once to the re-append policy: move the kept originals, oldest first, to the
// tail (still flagged, still pending; O(span) once), and let the drain re-append later
// kept originals the same way; the drain then ends with one stable partition
// (server_world_scheduled_tick_restore_requeued). Occupancy, and so drops, are the
// same either way.
static void server_world_scheduled_tick_drain_fallback(struct server_world* w) {
	assert(w->scheduled_tick_drain_mode == SCHEDULED_TICK_DRAIN_IN_PLACE);
	const size_t cap = w->scheduled_tick_capacity;
	const size_t span = w->scheduled_tick_kept_span;
	const size_t base = (w->scheduled_tick_head + cap - span) % cap;
	// The tail never passes the region entry being moved: it starts at or below base
	// and advances at most one slot per region slot.
	for(size_t j = 0; j < span; j++) {
		size_t from = (base + j) % cap;
		if(!(w->scheduled_ticks[from].queue_flags & SCHEDULED_TICK_FLAG_KEPT))
			continue;
		size_t to = (w->scheduled_tick_head + w->scheduled_tick_count) % cap;
		if(to != from) {
			server_world_scheduled_tick_hash_move(w, from, to);
			w->scheduled_ticks[to] = w->scheduled_ticks[from];
			w->scheduled_ticks[from].queue_flags = 0;
		}
		w->scheduled_tick_count++;
	}
	w->scheduled_tick_kept = 0;
	w->scheduled_tick_kept_span = 0;
	w->scheduled_tick_drain_mode = SCHEDULED_TICK_DRAIN_REQUEUE;
}

// End of a REQUEUE-mode drain: put every flagged original back ahead of both the
// unprocessed original suffix and the callback-enqueued children (stable within all
// three groups), i.e. Beta's (scheduledTime, tickEntryID) order. The destination
// labels use the containment set's own storage as scratch (hash_cap >= 2 * capacity
// >= count uint16 slots); the set is rebuilt right after, because physical indices
// moved anyway.
static void server_world_scheduled_tick_restore_requeued(
	struct server_world* w, size_t remaining_original) {
	const size_t count = w->scheduled_tick_count;
	const size_t cap = w->scheduled_tick_capacity;
	assert(remaining_original <= count && count <= w->scheduled_tick_hash_cap);
	uint16_t* destination = w->scheduled_tick_hash;
#define TQ_AT(i) (&w->scheduled_ticks[(w->scheduled_tick_head + (i)) % cap])
	size_t next = 0;
	for(size_t source = remaining_original; source < count; source++)
		if(TQ_AT(source)->queue_flags & SCHEDULED_TICK_FLAG_KEPT)
			destination[source] = (uint16_t)next++;
	for(size_t source = 0; source < remaining_original; source++)
		destination[source] = (uint16_t)next++;
	for(size_t source = remaining_original; source < count; source++)
		if(!(TQ_AT(source)->queue_flags & SCHEDULED_TICK_FLAG_KEPT))
			destination[source] = (uint16_t)next++;
	assert(next == count);
	// destination[source] is the destination of the element currently at source.
	// Swap whole elements and their destination labels until every cycle is fixed.
	for(size_t i = 0; i < count; i++) {
		while(destination[i] != i) {
			size_t j = destination[i];
			struct scheduled_tick tmp = *TQ_AT(i);
			*TQ_AT(i) = *TQ_AT(j);
			*TQ_AT(j) = tmp;
			uint16_t d = destination[i];
			destination[i] = destination[j];
			destination[j] = d;
		}
	}
	for(size_t i = 0; i < count; i++)
		TQ_AT(i)->queue_flags &= (uint8_t)~SCHEDULED_TICK_FLAG_KEPT;
#undef TQ_AT
	server_world_scheduled_ticks_resync(w);
}

void server_world_scheduled_ticks_create(struct server_world* w,
										 size_t capacity) {
	assert(w && capacity > 0);

	// The stable fluid/non-fluid partition is part of the queue's timing
	// contract, not an optional optimization, and so is the (x,y,z,id)
	// containment set. Allocate entries + hash as one ownership unit so a live
	// queue can never silently fall back to a degraded policy under partial OOM.
	// The production capacity is 4096 (ring 64 KiB + hash 16 KiB).
	w->scheduled_ticks = NULL;
	w->scheduled_tick_kept = 0;
	w->scheduled_tick_kept_span = 0;
	w->scheduled_tick_drain_mode = 0;
	w->scheduled_tick_hash = NULL;
	w->scheduled_tick_hash_cap = 0;
	w->scheduled_tick_hash_used = 0;
	w->scheduled_tick_count = 0;
	w->scheduled_tick_head = 0;
	w->scheduled_tick_drops = 0;
	// Physical index + 1 must fit a uint16 below the tombstone value.
	if(capacity >= SCHEDULED_TICK_HASH_TOMB - 1
	   || capacity > SIZE_MAX / (sizeof(struct scheduled_tick)
								  + 2 * sizeof(uint16_t))) {
		w->scheduled_tick_capacity = 0;
		return;
	}
	size_t hash_cap = 16;
	while(hash_cap < 2 * capacity)
		hash_cap <<= 1;
	size_t entry_bytes = capacity * sizeof(struct scheduled_tick);
	size_t hash_bytes = hash_cap * sizeof(*w->scheduled_tick_hash);
	w->scheduled_ticks = malloc(entry_bytes + hash_bytes);
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
	w->scheduled_tick_hash
		= (uint16_t*)((unsigned char*)w->scheduled_ticks + entry_bytes);
	w->scheduled_tick_hash_cap = hash_cap;
	memset(w->scheduled_tick_hash, 0, hash_bytes);
}

void server_world_scheduled_ticks_destroy(struct server_world* w) {
	assert(w);
	free(w->scheduled_ticks);
	w->scheduled_ticks = NULL;
	w->scheduled_tick_hash = NULL;
	w->scheduled_tick_hash_cap = 0;
	w->scheduled_tick_hash_used = 0;
	w->scheduled_tick_kept = 0;
	w->scheduled_tick_kept_span = 0;
	w->scheduled_tick_drain_mode = 0;
	w->scheduled_tick_capacity = 0;
	w->scheduled_tick_count = 0;
	w->scheduled_tick_head = 0;
}

// Append at the tail. Occupancy is the live window plus the drain's kept originals,
// exactly the count the old pop/re-append drain had at this point, so drops are
// unchanged. If the popped region's fired holes are what blocks the tail physically
// (only a saturated ring mid-drain), switch the drain to the re-append fallback.
static bool server_world_scheduled_tick_append(struct server_world* w,
											   struct scheduled_tick tick) {
	if(w->scheduled_tick_count + w->scheduled_tick_kept
	   >= w->scheduled_tick_capacity) {
		w->scheduled_tick_drops++;
		return false;
	}
	if(w->scheduled_tick_count + w->scheduled_tick_kept_span
	   >= w->scheduled_tick_capacity)
		server_world_scheduled_tick_drain_fallback(w);
	size_t idx = (w->scheduled_tick_head + w->scheduled_tick_count)
		% w->scheduled_tick_capacity;
	w->scheduled_ticks[idx] = tick;
	w->scheduled_tick_count++;
	server_world_scheduled_tick_hash_insert(w, idx);
	return true;
}

static bool server_world_schedule_tick_at_policy(struct server_world* w,
												 struct scheduled_tick tick) {
	assert(w && w->scheduled_ticks);
	tick.queue_flags = 0;

	// Beta World.scheduleBlockUpdate [740c583 World.java:1161] gates every insert behind
	// scheduledTickSet.contains(): NextTickListEntry.equals keys on (x,y,z,blockID) ONLY
	// (scheduledTime excluded), so at most ONE pending tick exists per (position, block id). On
	// a duplicate Beta DISCARDS the new entry and KEEPS THE FIRST-INSERTED one with its original
	// fire time -- a later re-schedule with an EARLIER delay does NOT pull the fire time in (a
	// repeater scheduled mid-delay must IGNORE a re-trigger and fire at its original time; this
	// is keep-FIRST, NOT keep-earliest). The containment set is the O(1) hash above (== Beta's
	// HashSet), so the check runs at EVERY depth; the old 256-entry linear-scan cutoff
	// (SCHEDULED_TICK_DEDUP_SCAN_MAX) let a duplicate in past it (HUNT-14 item 10). Entity
	// explosions retain their source under their own cap and never enter this ring.
	if(server_world_scheduled_tick_hash_find(w, tick.x, tick.y, tick.z,
											 tick.block_type)
	   != SIZE_MAX)
		return true; // already pending: keep the FIRST-INSERTED entry (Beta contains() skips)

	return server_world_scheduled_tick_append(w, tick);
}

bool server_world_schedule_tick_at_raw(struct server_world* w,
									   struct scheduled_tick tick) {
	return server_world_schedule_tick_at_policy(w, tick);
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
	 * still queued; Beta's keep-first identity (the same policy as every other
	 * insert) stops a retry from duplicating its prefix. */
	return server_world_schedule_tick_at_policy(
		w,
		(struct scheduled_tick) {
			.x = x,
			.y = y,
			.z = z,
			.fire_at_tick = w->current_tick + delay,
			.block_type = block_type,
		});
}

bool server_world_scheduled_tick_pop(struct server_world* w,
									 struct scheduled_tick* tick) {
	assert(w && tick && w->scheduled_ticks);

	if(w->scheduled_tick_count == 0)
		return false;

	// Beta World.TickUpdates removes the entry from BOTH sets before updateTick,
	// so a callback may schedule the same (x,y,z,id) again.
	server_world_scheduled_tick_hash_remove(w, w->scheduled_tick_head);
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
	// immediately precedes every requeue guarantees one free slot and no callback
	// ran in between, so the key cannot already be present: append it raw and
	// re-enter it in the containment set.
	tick.queue_flags &= (uint8_t)~SCHEDULED_TICK_FLAG_KEPT;
	(void)server_world_scheduled_tick_append(w, tick);
}


void server_world_drain_scheduled(
	struct server_world* w, struct server_local* s, uint32_t budget,
	void (*fire)(struct server_world*, struct server_local*, struct scheduled_tick),
	void (*yield)(void)) {
	assert(w && fire);
	assert(w->scheduled_tick_drain_mode == SCHEDULED_TICK_DRAIN_IDLE
		   && w->scheduled_tick_kept == 0 && w->scheduled_tick_kept_span == 0);

	// Snapshot the queue length so ticks ENQUEUED during this drain (a fluid cell
	// that flows schedules its neighbours) are deferred to the next game-tick.
	const size_t entries = w->scheduled_tick_count;
	uint32_t fluid_fired = 0;
	uint32_t total_fired = 0;
	size_t processed = 0;
	// Relative index (from the popped region's base) just above the topmost fired
	// hole; kept originals above it are already contiguous below the head.
	size_t holes_top = 0;
	bool any_kept = false;
	w->scheduled_tick_drain_mode = SCHEDULED_TICK_DRAIN_IN_PLACE;

	for(; processed < entries; processed++) {
		if(server_world_simulation_stopped(w) || w->light_fault)
			break; /* G9 R3: a light stop ends the loop at once */
		// Beta World.tickUpdates executes at most 1000 due callbacks per tick.
		if(budget && total_fired >= SERVER_WORLD_SCHEDULED_TICK_BETA_CAP)
			break;
		if(w->scheduled_tick_count == 0)
			break;

		const size_t cap = w->scheduled_tick_capacity;
		const size_t p = w->scheduled_tick_head;
		struct scheduled_tick* slot = &w->scheduled_ticks[p];
		const bool flowing_fluid = slot->block_type == BLOCK_WATER_FLOW
			|| slot->block_type == BLOCK_LAVA_FLOW;
		// Not fired this drain, the original stays pending at its Beta position:
		// - a future entry consumes neither the fluid cap nor Beta's fire cap (Beta
		//   leaves it in the TreeSet, i.e. at its insertion position);
		// - R6: freeze retains the original due fluid, including its timestamp,
		//   without a no-op callback and without blocking other work;
		// - the Wii fluid sub-cap: keep scanning the initial snapshot, so timing-
		//   sensitive non-fluid entries behind a settling fluid backlog still fire.
		const bool keep = slot->fire_at_tick > w->current_tick
			|| (s && s->pool_selftest_freeze
				&& (flowing_fluid || slot->block_type == BLOCK_WATER_STILL
					|| slot->block_type == BLOCK_LAVA_STILL))
			|| (budget && flowing_fluid && fluid_fired >= budget);

		w->scheduled_tick_head = (p + 1) % cap;
		w->scheduled_tick_count--;
		if(keep) {
			// Still in the containment set (Beta never removed it).
			slot->queue_flags |= SCHEDULED_TICK_FLAG_KEPT;
			any_kept = true;
			if(w->scheduled_tick_drain_mode == SCHEDULED_TICK_DRAIN_IN_PLACE) {
				// Now behind the head, in place.
				w->scheduled_tick_kept_span++;
				w->scheduled_tick_kept++;
			} else {
				// Saturated fallback: re-append it (the pop just freed a slot).
				size_t to = (w->scheduled_tick_head + w->scheduled_tick_count) % cap;
				if(to != p) {
					server_world_scheduled_tick_hash_move(w, p, to);
					w->scheduled_ticks[to] = *slot;
				}
				w->scheduled_tick_count++;
			}
			continue;
		}

		// Beta World.TickUpdates removes the entry from BOTH sets before updateTick,
		// so a callback may schedule the same (x,y,z,id) again.
		server_world_scheduled_tick_hash_remove(w, p);
		struct scheduled_tick tick = *slot;
		tick.queue_flags = 0;
		slot->queue_flags = 0; // a fired hole
		if(w->scheduled_tick_drain_mode == SCHEDULED_TICK_DRAIN_IN_PLACE) {
			w->scheduled_tick_kept_span++;
			holes_top = w->scheduled_tick_kept_span;
		}

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

	if(w->scheduled_tick_drain_mode == SCHEDULED_TICK_DRAIN_REQUEUE) {
		// Saturated-ring fallback ran: one stable partition puts the originals back.
		if(any_kept)
			server_world_scheduled_tick_restore_requeued(w, entries - processed);
	} else {
		// Close the fired holes under the kept originals, then hand them back to the
		// live window just ahead of the unprocessed suffix and the callback-enqueued
		// children. Only kept entries below the topmost hole move (each retargets its
		// own set slot); a drain that fires first and defers after moves almost nothing.
		const size_t kept = w->scheduled_tick_kept;
		const size_t cap = w->scheduled_tick_capacity;
		if(kept > 0) {
			server_world_scheduled_tick_compact_kept(w, holes_top);
			w->scheduled_tick_head = (w->scheduled_tick_head + cap - kept) % cap;
			for(size_t i = 0; i < kept; i++)
				w->scheduled_ticks[(w->scheduled_tick_head + i) % cap].queue_flags
					&= (uint8_t)~SCHEDULED_TICK_FLAG_KEPT;
			w->scheduled_tick_count += kept;
		}
	}
	w->scheduled_tick_kept = 0;
	w->scheduled_tick_kept_span = 0;
	w->scheduled_tick_drain_mode = SCHEDULED_TICK_DRAIN_IDLE;
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
