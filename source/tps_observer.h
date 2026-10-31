/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef TPS_OBSERVER_H
#define TPS_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Observation only. Each instrumented TU owns its nullable callback, so narrow
 * fixtures do not acquire the collector or another engine compile profile. */
enum tps_action { TPS_ENTER, TPS_LEAVE, TPS_PHASE };
enum tps_part {
	TPS_SCHEDULER, TPS_LOAD, TPS_TERRAIN, TPS_INITIAL_LIGHT, TPS_DECORATE,
	TPS_VERTICAL_QUAD, TPS_BLOCK_QUAD, TPS_SKY_QUAD,
	TPS_VERTICAL_FINAL, TPS_SKY_FINAL, TPS_SAVE, TPS_REARM,
	TPS_REGION_READ, TPS_REGION_PARSE, TPS_REGION_GET,
	TPS_REGION_COMPRESS, TPS_REGION_COMMIT, TPS_REGION_SYNC,
	TPS_CHANNEL_WAIT, TPS_MUTEX_WAIT, TPS_MEM2_WAIT,
	/* Appended, never inserted: nested_ns[] is positional in every aggregator
	 * that already reads a captured run, so a new part must not renumber the
	 * existing ones. PRESERVE is the chest-sort lossless read-modify-compare
	 * inside region_archive_set_chunk -- the block between the read-back
	 * (READ/PARSE) and the encode (COMPRESS) that had no slot at all. */
	TPS_REGION_PRESERVE, TPS_PART_COUNT
};
typedef void (*tps_callback)(enum tps_action, unsigned);
extern tps_callback server_tps_observe, world_tps_observe;
extern tps_callback region_tps_observe, thread_tps_observe;
extern tps_callback mem2_tps_observe;
extern tps_callback worldgen_tps_observe;
extern tps_callback client_tps_observe;
struct server_local;
extern void (*server_tps_boundary)(struct server_local*, bool);
extern void (*mesher_tps_boundary)(bool);

/* The callback can be retired while unrelated audio/network threads remain
 * alive. Acquire/release avoids a plain function-pointer read/write race. */
#define tps_note(callback, action, part) do { \
	tps_callback tps_cb_ = __atomic_load_n(&(callback), __ATOMIC_ACQUIRE); \
	if(tps_cb_) tps_cb_((action), (part)); \
} while(0)

/* Census is separate from tps_cpu_evidence: heartbeat ABI stays unchanged. */
enum tps_census_field {
	TPS_C_SAVE_CALLS, TPS_C_SAVE_WRITES, TPS_C_SAVE_UNCHANGED,
	TPS_C_SAVE_UNKNOWN, TPS_C_SAVE_COMPRESS_NS, TPS_C_SAVE_COMMIT_NS,
	TPS_C_SAVE_COMPARE_NS,
	TPS_C_BLOCK_COUNT_NS, TPS_C_BLOCK_FILL_NS, TPS_C_BLOCK_PAIRS,
	TPS_C_BLOCK_DUPLICATES, TPS_C_BLOCK_COUNT_CELLS, TPS_C_BLOCK_FILL_CELLS,
	TPS_C_SKY_COUNT_NS, TPS_C_SKY_FILL_NS, TPS_C_SKY_PAIRS,
	TPS_C_SKY_DUPLICATES, TPS_C_SKY_COUNT_CELLS, TPS_C_SKY_FILL_CELLS,
	TPS_C_VERTICAL_CALLS, TPS_C_VERTICAL_UNCHANGED,
	TPS_C_BLOCK_CALLS, TPS_C_BLOCK_UNCHANGED,
	TPS_C_SKY_CALLS, TPS_C_SKY_UNCHANGED,
	TPS_C_LIGHT_LOOKUP_MISSES,
	TPS_C_UNCHANGED_COMPRESS_NS, TPS_C_UNCHANGED_COMMIT_NS,
	TPS_C_COMPARE_BYTES, TPS_C_MUTATIONS,
	TPS_C_BLOCK_MEMO_FALLBACKS, TPS_C_SKY_MEMO_FALLBACKS,
	TPS_C_BLOCK_CONSUMED_CELLS, TPS_C_BLOCK_CONSUMED_SOURCES,
	TPS_C_SKY_CONSUMED_CELLS, TPS_C_SKY_CONSUMED_SOURCES, TPS_C_COUNT
};
struct tps_census { uint64_t value[TPS_C_COUNT]; };
enum tps_save_action { TPS_SAVE_COMPARE_BEGIN, TPS_SAVE_COMPARE_END, TPS_SAVE_RESULT };
struct tps_save_input {
	const void* bytes;
	size_t length;
	uint32_t* previous; /* low/high words, avoids 8-byte column alignment */
	uint64_t hash;
	bool active, candidate, equal, known, success;
};
typedef void (*tps_save_callback)(enum tps_save_action, struct tps_save_input*);
extern tps_save_callback region_tps_save;

/* Conservative certificates cover all five planes and global residency,
 * including old light values a repeated pass would overwrite. */
typedef void (*tps_mutation_callback)(unsigned); /* 1/2/3 write, 4 compare vertical output */
extern tps_mutation_callback world_tps_mutation;
#define tps_mutated(mask) do { \
	tps_mutation_callback cb_ = __atomic_load_n(&world_tps_mutation, __ATOMIC_ACQUIRE); \
	if(cb_) cb_(mask); \
} while(0)
struct tps_light_input {
	const void* world;
	const void* columns[4]; /* vertical: IDs, sky, height; floods: ordered IDs */
	int32_t x, z, dimension;
	unsigned count, kind; /* vertical=0, block=1, sky=2 */
};
typedef void (*tps_light_callback)(const struct tps_light_input*);
extern tps_light_callback world_tps_light;

enum tps_scan_action { TPS_SCAN_BEGIN, TPS_SCAN_COUNT_END,
	TPS_SCAN_FILL_BEGIN, TPS_SCAN_END, TPS_SCAN_MEMO_FALLBACK };
struct tps_scan {
	uint8_t* bits;
	uint32_t count_cells, fill_cells, mismatches, positives, fill_positives;
	uint32_t consumed_cells, consumed_sources;
	bool memoised, memo_fallback;
	unsigned kind; /* 0 block-source, 1 sky-frontier */
};
typedef void (*tps_scan_callback)(enum tps_scan_action, struct tps_scan*);
extern tps_scan_callback world_tps_scan;
/* Called only behind a non-NULL scan.bits check. Each bit is indexed by the
 * physical cell, so differing loop orders do not masquerade as differences. */
static inline void tps_scan_count(struct tps_scan* s, unsigned i, bool value) {
	uint8_t mask = (uint8_t)(1u << (i & 7));
	s->bits[i >> 3] = (uint8_t)((s->bits[i >> 3] & ~mask) | (value ? mask : 0));
	s->count_cells++;
	s->positives += value;
}
static inline void tps_scan_fill(struct tps_scan* s, unsigned i, bool value) {
	s->mismatches += !!(s->bits[i >> 3] & (1u << (i & 7))) != value;
	s->fill_cells++;
	s->fill_positives += value;
}

#define TPS_PHASE_COUNT 11u
struct tps_cpu_evidence {
	bool enabled;
	uint32_t clock_step_ns, valid, ticks;
	/* Cumulative microseconds, modulo 2^32; subtract adjacent heartbeats. */
	uint32_t phase_us[TPS_PHASE_COUNT];
	uint32_t nested_us[TPS_PART_COUNT];
	uint32_t wait_us[3], wait_count[3];
	uint32_t main_us, mesher_us, frames, packed_us, clock_mode;
};

/* Probe before arming anything. Mode 1 is TSC-scaled thread cycles (estimate),
 * mode 2 is POSIX thread CPU, mode 3 is activity-masked QPC/Wii timebase WALL.
 * The fallback excludes bracketed waits, not preemption or unbracketed I/O.
 * False means clock/file failure. Never label mode 3 as measured CPU time. */
bool tps_observer_start(const char* prefix);
bool tps_observer_stop(void); /* after joining server and mesher */
void tps_observer_output(bool (*emit)(const char*)); /* main-thread drain */
/* A closing frame freezes the window, then retries refused summary/seal
 * admission on later frames (at most 2 s / 120 opportunities for summaries).
 * One retained summary and one newer aggregate; no wait or heap allocation. */
void tps_observer_frame(bool measuring);
/* Main thread, before telemetry admission closes. Idempotent for a closed
 * window. Four final nonwaiting flush passes after the frame attempt; loss
 * invalidates the seal. A refused seal retries on later frame/close calls.
 * Incomplete windows carry an explicit abort/quiesce reason. */
void tps_observer_close(bool complete);
void tps_observer_snapshot(struct tps_cpu_evidence* out); /* main thread only */
uint64_t tps_cpu_now_ns(void);
uint64_t tps_wall_now_ns(void);
uint32_t tps_cpu_probe_step_ns(void);
/* Dormant test seam. Only accepted while the collector is stopped. */
size_t tps_observer_static_bytes(void);
bool tps_observer_test_emit_clock(uint64_t (*clock)(void));
bool tps_observer_test_clock(uint64_t (*clock)(void));
bool tps_observer_test_wall_mask(bool enable);
bool tps_observer_test_publication(void (*hook)(void));
bool tps_observer_test_row_publication(void (*hook)(void));

#endif
