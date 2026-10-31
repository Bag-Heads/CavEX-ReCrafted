/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PLATFORM_MEM2_POOL_EVQ_H
#define PLATFORM_MEM2_POOL_EVQ_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hw_telemetry.h"

/*
	COLUMN-POOL EVENT QUEUE (colcap-1006, ASTRA-CC-R4 point 9; Sol review R3
	Major 2).

	WHY. Grow/shrink events used to be formatted and published by whichever
	thread made the change, right after it released mem2_lock. Two threads can
	change the pool back to back (seq 5 then seq 6) and publish in the other
	order; the collector then saw seq 6 before seq 5 and rejected the earlier
	one as a regression. So an event is now QUEUED under the allocator lock --
	that is where its sequence number is assigned, so queue order IS sequence
	order -- and published outside the lock by exactly one publisher at a
	time, oldest first.

	SEQUENCE. `seq` counts queued events only: an event that is not queued
	(a repeated carve failure at an unchanged slab count, already counted in
	carve_fail) takes no number, so a gap in the delivered sequence always
	means a lost event. The heartbeat stamps its pool snapshot with the seq
	current at the snapshot (read under the same lock), which is how the
	collector reconciles snapshots with events even when the heartbeat's
	priority lane overtakes a queued event on the wire.

	DELIVERY. The publisher offers the OLDEST event and only drops it from the
	queue once the transport accepted it (a lossless, retrying producer: the
	ring counts the refusal as deferred, not as a drop). A refused event stays
	at the head and is retried on the next allocator call or heartbeat, so
	order survives a full ring. If the queue itself overflows (more than
	MEM2_POOL_EVQ_SLOTS undelivered changes) the newest event is not queued and
	`lost` counts it; the heartbeat carries `lost`, and the collector's chain
	check fails on the missing link -- an overflow is never silent.

	CONTRACT. mem2_pool_evq_push_locked: allocator lock held. mem2_pool_evq_
	publish: allocator lock NOT held; it takes the lock (through the supplied
	callbacks) only to read the head and to retire it, never across the emit.
*/

#define MEM2_POOL_EVQ_SLOTS 16u

enum mem2_pool_ev_kind {
	MEM2_POOL_EV_GROW = 1,
	MEM2_POOL_EV_SHRINK = 2,
};

struct mem2_pool_ev {
	uint32_t kind;
	struct hw_column_pool_grow grow;
	struct hw_column_pool_shrink shrink;
};

struct mem2_pool_evq {
	struct mem2_pool_ev ev[MEM2_POOL_EVQ_SLOTS];
	uint32_t head, tail; /* under the allocator lock */
	uint32_t seq;		 /* last assigned; under the allocator lock */
	uint32_t lost;		 /* events not queued (queue full); cumulative */
	uint32_t publisher;	 /* atomic flag: one publisher at a time */
};

/* Assign the next seq and queue `ev` (its seq field is overwritten). False,
 * and `lost` incremented, when the queue is full. Allocator lock held. */
static inline bool mem2_pool_evq_push_locked(struct mem2_pool_evq* q,
											 struct mem2_pool_ev* ev) {
	if(q->head - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE)
	   >= MEM2_POOL_EVQ_SLOTS) {
		q->lost++;
		return false;
	}
	const uint32_t seq = ++q->seq;
	if(ev->kind == MEM2_POOL_EV_GROW)
		ev->grow.seq = seq;
	else
		ev->shrink.seq = seq;
	q->ev[q->head % MEM2_POOL_EVQ_SLOTS] = *ev;
	__atomic_store_n(&q->head, q->head + 1, __ATOMIC_RELEASE);
	return true;
}

static inline bool mem2_pool_evq_pending(const struct mem2_pool_evq* q) {
	return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE)
		!= __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}

/* Deliver at most `budget` queued events oldest first; stop at the first
 * refusal (it stays at the head for the next call). Returns the number
 * delivered. Allocator lock NOT held. The budget bounds the work of one call
 * (Sol review R4): the Wii calls this once per heartbeat, never on the
 * allocation path. A caller that finds another publisher active returns at once:
 * that publisher re-checks the queue after releasing the flag, so an event
 * queued meanwhile is never stranded. */
static inline unsigned mem2_pool_evq_publish(struct mem2_pool_evq* q,
	void (*lock)(void), void (*unlock)(void),
	bool (*emit)(const struct mem2_pool_ev* ev, void* user), void* user,
	unsigned budget) {
	unsigned delivered = 0;
	for(;;) {
		uint32_t expected = 0;
		if(!__atomic_compare_exchange_n(&q->publisher, &expected, 1u, false,
										__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return delivered;
		bool refused = false;
		while(delivered < budget) {
			struct mem2_pool_ev ev;
			lock();
			const bool empty = q->head == q->tail;
			if(!empty)
				ev = q->ev[q->tail % MEM2_POOL_EVQ_SLOTS];
			unlock();
			if(empty)
				break;
			if(!emit(&ev, user)) {
				refused = true;
				break;
			}
			lock();
			/* Only the flag holder advances tail; readers outside the lock
			 * (mem2_pool_evq_pending, push) load it atomically. */
			__atomic_store_n(&q->tail, q->tail + 1, __ATOMIC_RELEASE);
			unlock();
			delivered++;
		}
		__atomic_store_n(&q->publisher, 0u, __ATOMIC_RELEASE);
		if(refused || delivered >= budget || !mem2_pool_evq_pending(q))
			return delivered;
	}
}

#endif
