/* Four complete records, including NUL, PLUS a one-deep priority lane that
 * only the heartbeat uses. One consumer; producers try once.
 * A consumer owns its slot until pop, including while IOS is stalled.
 * No mutex, spin, allocation, sleep, or socket operation in publish. */
#ifndef CW_HW_TEL_RING_H
#define CW_HW_TEL_RING_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define CW_TEL_SLOTS 4u
/* = HW_TELEMETRY_HEARTBEAT_LINE_SIZE (asserted in hw_telemetry.c). Was 9216
 * while the formatter accepted up to 13,311 B: the widest heartbeat was
 * formatted, then refused by the ring (colcap-1006, Sol review R3 Major 1).
 * Cost: 5 lines x 4,096 B = +20,480 B of .bss for the ring. */
/* R7 retained receipts: another 5 x 256 = 1280 bytes of ring storage. */
/* integ-1008: colcap + G9 merged widest row measured 13,795 B (saturated P2 +
 * dims + pool + lqd + g9p) -> 13,995 keeps the 200-byte spare; +5 x 427 B. */
#define CW_TEL_LINE 13995u

/* Priority-lane state machine. Only a PRODUCER performs EMPTY->WRITING and
 * FULL->WRITING->FULL; only the CONSUMER performs FULL->SENDING->EMPTY. Every
 * transition is a compare-exchange, so a producer can never overwrite the
 * buffer the consumer is transmitting (it falls back to the FIFO instead) and
 * the consumer can never hand out a half-written line. */
#define CW_TEL_HB_EMPTY 0u
#define CW_TEL_HB_WRITING 1u
#define CW_TEL_HB_FULL 2u
#define CW_TEL_HB_SENDING 3u

struct cw_tel_ring {
	char lines[CW_TEL_SLOTS][CW_TEL_LINE];
	/* +9,216 B of .bss over the pre-2026-09-07 ring. Paid on purpose: the
	 * FIFO is strictly in-order, so a heartbeat admitted behind three
	 * observer records only reaches the wire after all three. On the real
	 * Wii (build 0077cd09, split_anchor_6m) the priority-40 sender drained
	 * ~2 records/s during the two-anchor reload while the carve observer
	 * offered ~12/s, so the heartbeat's own reserved FIFO slot bought it
	 * admission and nothing else -- 571 ring drops, tel_fail 0, heartbeat
	 * stale after 53 hb with the game still at 61 fps. */
	char hb_line[CW_TEL_LINE];
	uint32_t head, tail, producer, drops, closed;
	uint32_t hb_state;
	/* Heartbeats superseded in the lane before they could be sent. Not
	 * folded into `drops`: carve_census/the collector read `drops` as
	 * "observer records refused admission", and a replaced heartbeat is a
	 * liveness sample deliberately traded for a fresher one. */
	uint32_t hb_replaced;
	/* O22: refusals of a LOSSLESS retrying producer (it keeps the line and
	 * resends it). Not folded into `drops`: nothing was lost. */
	uint32_t deferred;
	/* High-water total occupancy (FIFO records + the heartbeat lane), 0..5.
	 * Monotonic since the ring was zeroed; published in the heartbeat as
	 * tel_qmax so a console run can tell "the sender never runs" (pinned at
	 * 5) from "the sender keeps up" (1-2). */
	uint32_t occupancy_max;
};

/* Producer-side helpers. Both assume the caller already owns `producer`. */
static inline void cw_tel_note_occupancy(struct cw_tel_ring* q) {
	uint32_t occ = __atomic_load_n(&q->head, __ATOMIC_RELAXED)
		- __atomic_load_n(&q->tail, __ATOMIC_RELAXED)
		+ (__atomic_load_n(&q->hb_state, __ATOMIC_RELAXED) == CW_TEL_HB_EMPTY
			   ? 0u : 1u);
	if(occ > __atomic_load_n(&q->occupancy_max, __ATOMIC_RELAXED))
		__atomic_store_n(&q->occupancy_max, occ, __ATOMIC_RELAXED);
}

/* `reserve` slots are kept free for higher-priority producers. */
static inline bool cw_tel_fifo_store(struct cw_tel_ring* q, const char* line,
									 size_t n, uint32_t reserve) {
	uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
	uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
	if(__atomic_load_n(&q->closed, __ATOMIC_ACQUIRE)
	   || head - tail >= CW_TEL_SLOTS - reserve)
		return false;
	memcpy(q->lines[head % CW_TEL_SLOTS], line, n + 1);
	__atomic_store_n(&q->head, head + 1, __ATOMIC_RELEASE);
	return true;
}

static inline size_t cw_tel_line_len(const char* line) {
	size_t n = 0;
	if(line)
		while(n < CW_TEL_LINE && line[n])
			n++;
	return n;
}

static inline bool cw_tel_publish(struct cw_tel_ring* q, const char* line) {
	uint32_t expected = 0;
	if(!__atomic_compare_exchange_n(&q->producer, &expected, 1, false,
								   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		goto drop;
	size_t n = cw_tel_line_len(line);
	bool ok = line && n < CW_TEL_LINE && cw_tel_fifo_store(q, line, n, 0u);
	if(ok)
		cw_tel_note_occupancy(q);
	__atomic_store_n(&q->producer, 0, __ATOMIC_RELEASE);
	if(ok)
		return true;
drop:
	__atomic_fetch_add(&q->drops, 1, __ATOMIC_RELAXED);
	return false;
}

/* Low-priority observer records leave one slot free for the next heartbeat.
 * A full/contended FIFO drops the observer record and increments the same
 * monotonic drop counter; attribution never evicts or delays an admitted
 * heartbeat. */
static inline bool cw_tel_publish_observer(struct cw_tel_ring* q,
	const char* line) {
	uint32_t expected = 0;
	if(!__atomic_compare_exchange_n(&q->producer, &expected, 1, false,
								   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		goto drop;
	size_t n = cw_tel_line_len(line);
	bool ok = line && n < CW_TEL_LINE && cw_tel_fifo_store(q, line, n, 1u);
	if(ok)
		cw_tel_note_occupancy(q);
	__atomic_store_n(&q->producer, 0, __ATOMIC_RELEASE);
	if(ok)
		return true;
drop:
	__atomic_fetch_add(&q->drops, 1, __ATOMIC_RELAXED);
	return false;
}

/* O22: the observer admission for a producer that RETAINS a refused line and
 * offers it again later (vehicle_smoke_emit). Same admission rule as
 * cw_tel_publish_observer (one slot left for the heartbeat); a refusal is
 * counted in `deferred`, never in `drops`. */
static inline bool cw_tel_try_publish_observer(struct cw_tel_ring* q,
	const char* line) {
	uint32_t expected = 0;
	if(!__atomic_compare_exchange_n(&q->producer, &expected, 1, false,
								   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		goto deferred;
	size_t n = cw_tel_line_len(line);
	bool ok = line && n < CW_TEL_LINE && cw_tel_fifo_store(q, line, n, 1u);
	if(ok)
		cw_tel_note_occupancy(q);
	__atomic_store_n(&q->producer, 0, __ATOMIC_RELEASE);
	if(ok)
		return true;
deferred:
	__atomic_fetch_add(&q->deferred, 1, __ATOMIC_RELAXED);
	return false;
}

/* HEARTBEAT LANE. The consumer always drains this before any FIFO record, so
 * the liveness signal never waits behind observer traffic no matter how slowly
 * the sender is being scheduled. A newer heartbeat REPLACES an undelivered
 * older one (freshness beats completeness for liveness) instead of queueing
 * behind it. If the consumer happens to be transmitting the lane, the new
 * heartbeat falls back to the ordinary full-priority FIFO rather than being
 * dropped. */
static inline bool cw_tel_publish_hb(struct cw_tel_ring* q, const char* line) {
	uint32_t expected = 0;
	if(!__atomic_compare_exchange_n(&q->producer, &expected, 1, false,
								   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		goto drop;
	size_t n = cw_tel_line_len(line);
	bool ok = line && n < CW_TEL_LINE
		&& !__atomic_load_n(&q->closed, __ATOMIC_ACQUIRE);
	bool replaced = false;
	if(ok) {
		uint32_t state = CW_TEL_HB_EMPTY;
		bool claimed = __atomic_compare_exchange_n(&q->hb_state, &state,
			CW_TEL_HB_WRITING, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
		if(!claimed) {
			state = CW_TEL_HB_FULL;
			claimed = replaced = __atomic_compare_exchange_n(&q->hb_state,
				&state, CW_TEL_HB_WRITING, false, __ATOMIC_ACQUIRE,
				__ATOMIC_RELAXED);
		}
		if(claimed) {
			memcpy(q->hb_line, line, n + 1);
			__atomic_store_n(&q->hb_state, CW_TEL_HB_FULL, __ATOMIC_RELEASE);
			if(replaced)
				__atomic_fetch_add(&q->hb_replaced, 1, __ATOMIC_RELAXED);
		} else {
			/* CW_TEL_HB_SENDING: the consumer owns hb_line right now. */
			ok = cw_tel_fifo_store(q, line, n, 0u);
		}
	}
	if(ok)
		cw_tel_note_occupancy(q);
	__atomic_store_n(&q->producer, 0, __ATOMIC_RELEASE);
	if(ok)
		return true;
drop:
	__atomic_fetch_add(&q->drops, 1, __ATOMIC_RELAXED);
	return false;
}

static inline const char* cw_tel_fifo_peek(struct cw_tel_ring* q) {
	uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
	return tail == __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) ? NULL :
		q->lines[tail % CW_TEL_SLOTS];
}

/* Consumer only. Idempotent: repeated peeks before a pop return the same line
 * (sender_entry peeks once to inspect the record, then again inside
 * sender_drain_one). */
static inline const char* cw_tel_peek(struct cw_tel_ring* q) {
	if(__atomic_load_n(&q->hb_state, __ATOMIC_ACQUIRE) == CW_TEL_HB_SENDING)
		return q->hb_line;
	uint32_t expected = CW_TEL_HB_FULL;
	if(__atomic_compare_exchange_n(&q->hb_state, &expected, CW_TEL_HB_SENDING,
								   false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return q->hb_line;
	return cw_tel_fifo_peek(q);
}

static inline void cw_tel_pop(struct cw_tel_ring* q) {
	uint32_t expected = CW_TEL_HB_SENDING;
	if(__atomic_compare_exchange_n(&q->hb_state, &expected, CW_TEL_HB_EMPTY,
								   false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		return;
	uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
	__atomic_store_n(&q->tail, tail + 1, __ATOMIC_RELEASE);
}

static inline void cw_tel_close(struct cw_tel_ring* q) {
	__atomic_store_n(&q->closed, 1, __ATOMIC_RELEASE);
}
/* Call after close. An admitted producer may still be finishing its copy. */
static inline bool cw_tel_drained(struct cw_tel_ring* q) {
	return !__atomic_load_n(&q->producer, __ATOMIC_ACQUIRE)
		&& __atomic_load_n(&q->hb_state, __ATOMIC_ACQUIRE) == CW_TEL_HB_EMPTY
		&& !cw_tel_fifo_peek(q);
}
#endif
