#ifndef DISPLAYLIST_STATS_H
#define DISPLAYLIST_STATS_H
#include "displaylist.h"
/* Included by each backend: O(1), allocation-free, 32-bit Broadway atomics.
 * No global lock or per-heartbeat list traversal. No emitted-byte changes. */
static uint32_t s_storage_lists, s_storage_slack, s_storage_pad;
/* colcap-1006 (ASTRA-CC-R3 item 3): resident finalized-list capacity bytes
 * (sum of l->length; Wii: excludes each block's 256 B mem2 canary). The same
 * finalize/retire pair as the counters above, so the same invariant holds:
 * l->length is fixed between storage_finalize and storage_retire. */
static uint32_t s_storage_bytes;
static uint32_t s_compact_enabled;
inline void displaylist_set_compact(bool enabled) {
	__atomic_store_n(&s_compact_enabled, enabled, __ATOMIC_RELAXED);
}
inline bool displaylist_compact_enabled(void) {
	return __atomic_load_n(&s_compact_enabled, __ATOMIC_RELAXED) != 0;
}
static inline uint32_t storage_slack(const struct displaylist* l) {
#ifdef PLATFORM_WII
	return (uint32_t)(l->length - ((l->index + 31) / 32 * 32));
#else
	return (uint32_t)(l->length - l->index * (l->tinted ? 25 : 22));
#endif
}
static inline uint32_t storage_pad(const struct displaylist* l) {
#ifdef PLATFORM_WII
	/* A failed shrink retains the original extra recording line. */
	return storage_slack(l) ? 32 : 0;
#else
	(void)l;
	return 0;
#endif
}
static inline void storage_retire(struct displaylist* l) {
	if(!l->accounted) return;
	__atomic_fetch_sub(&s_storage_slack, storage_slack(l), __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_storage_pad, storage_pad(l), __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_storage_lists, 1, __ATOMIC_RELAXED);
	__atomic_fetch_sub(&s_storage_bytes, (uint32_t)l->length, __ATOMIC_RELAXED);
	l->accounted = false;
}
static inline void storage_finalize(struct displaylist* l) {
	__atomic_fetch_add(&s_storage_slack, storage_slack(l), __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_storage_pad, storage_pad(l), __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_storage_lists, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_storage_bytes, (uint32_t)l->length, __ATOMIC_RELAXED);
	l->accounted = true;
}
inline void displaylist_storage_stats(uint32_t* lists, uint32_t* slack, uint32_t* pad) {
	*lists = __atomic_load_n(&s_storage_lists, __ATOMIC_RELAXED);
	*slack = __atomic_load_n(&s_storage_slack, __ATOMIC_RELAXED);
	/* Required rounded GX NOPs are in used bytes, canaries in mem2. */
	*pad = __atomic_load_n(&s_storage_pad, __ATOMIC_RELAXED);
}
inline void displaylist_storage_bytes(uint32_t* lists, uint32_t* bytes) {
	*lists = __atomic_load_n(&s_storage_lists, __ATOMIC_RELAXED);
	*bytes = __atomic_load_n(&s_storage_bytes, __ATOMIC_RELAXED);
}

#endif
