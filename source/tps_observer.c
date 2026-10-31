/* Copyright (c) 2026 CaveWii_v2 contributors
 * Opt-in PC thread-CPU evidence. No simulation values or budget decisions are
 * supplied by this observer. Legacy wall-time counters keep their own clocks.
 */
#include "tps_observer.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "network/server_local_state.h"
#ifdef PLATFORM_PC
#ifdef _WIN32
#include <windows.h>
#include <x86intrin.h>
#include <cpuid.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#endif
#ifdef PLATFORM_WII
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#endif

/* Closing frames yield between attempts, for at most 2 s / 120 opportunities.
 * Lifecycle close has no later frame guarantee: four final nonwaiting passes.
 * These bound diagnostic admission only, never the server tick producer. */
#define TPS_CLOSE_NS UINT64_C(2000000000)
#define TPS_CLOSE_OPPORTUNITIES 120u
#define TPS_FINAL_FLUSH_ATTEMPTS 4u

/* mode 1: calibrated thread cycles, 2: POSIX CPU, 3: activity-masked
 * QPC/timebase wall estimate. Mode 3 cannot exclude involuntary preemption. */
static unsigned clock_mode;
static double cycles_per_ns;
static double calibration_spread;
static uint32_t clock_failed;
#if defined(PLATFORM_PC) && defined(_WIN32)
static uint64_t sanity_start, sanity_wall;
#endif
static uint64_t (*test_clock)(void);
static bool test_wall_mask;
static uint64_t (*test_emit_clock)(void);
static void (*test_publication)(void);
static void (*test_row_publication)(void);
static const char* clock_name(void) {
	return test_clock ? "test" : clock_mode == 1 ? "thread_cycles_tsc_qpc_estimate"
		: clock_mode == 2 ? "thread_cpu" : "activity_masked_wall_estimate";
}

uint64_t tps_wall_now_ns(void) {
#ifdef PLATFORM_PC
#ifdef _WIN32
	LARGE_INTEGER tick, frequency;
	if(!QueryPerformanceCounter(&tick) || !QueryPerformanceFrequency(&frequency))
		return 0;
	return (uint64_t)(tick.QuadPart / frequency.QuadPart) * UINT64_C(1000000000)
		+ (uint64_t)(tick.QuadPart % frequency.QuadPart) * UINT64_C(1000000000)
			/ (uint64_t)frequency.QuadPart;
#else
	/* Same clock as platform/time.c's time_get (CLOCK_MONOTONIC_RAW, with the
	 * same fallback), so an observer wall reading and an engine deadline agree.
	 * Measured in WSL2: CLOCK_MONOTONIC ran 4.4% slower than _RAW (NTP slew),
	 * so the 25 ms column_view recovery deadline read as ~24 ms and
	 * test_column_dedup_pressure's lower bound failed ~70% of runs. */
#ifdef CLOCK_MONOTONIC_RAW
	const clockid_t id = CLOCK_MONOTONIC_RAW;
#else
	const clockid_t id = CLOCK_MONOTONIC;
#endif
	struct timespec t;
	if(clock_gettime(id, &t)) return 0;
	return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
#endif
#else
	uint64_t ticks = gettime();
	return (ticks / TB_TIMER_CLOCK) * UINT64_C(1000000)
		+ (ticks % TB_TIMER_CLOCK) * UINT64_C(1000000) / TB_TIMER_CLOCK;
#endif
}

#if defined(PLATFORM_PC) && defined(_WIN32)
static uint64_t cpu_handle_ns(HANDLE handle) {
	FILETIME created, exited, kernel, user;
	if(!GetThreadTimes(handle, &created, &exited, &kernel, &user)) return 0;
	uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
	uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
	return (k + u) * 100;
}
static uint64_t cycle_handle_ns(HANDLE handle) {
	ULONG64 cycles;
	if(clock_mode != 1) return tps_wall_now_ns();
	if(!QueryThreadCycleTime(handle, &cycles)) {
		__atomic_store_n(&clock_failed, 1, __ATOMIC_RELAXED);
		return 0;
	}
	return (uint64_t)((double)cycles / cycles_per_ns);
}
#endif

uint64_t tps_cpu_now_ns(void) {
	if(test_clock) return test_clock();
#ifdef PLATFORM_PC
#ifdef _WIN32
	return cycle_handle_ns(GetCurrentThread());
#else
	struct timespec t;
	if(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) return 0;
	return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
#endif
#else
	return tps_wall_now_ns();
#endif
}

static void qualify_clock(void) {
	if(test_clock || clock_mode) return;
	clock_mode = 3;
#if defined(PLATFORM_PC) && defined(_WIN32)
	unsigned a, b, c, d;
	ULONG64 cycles;
	/* Only use TSC scaling when the CPU advertises an invariant TSC.
	 * Three independent QPC intervals expose calibration spread in clock.json. */
	if(__get_cpuid(0x80000007, &a, &b, &c, &d) && (d & (1u << 8))
	   && QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
		double lo = 1e30, hi = 0, sum = 0;
		for(unsigned i = 0; i < 3; i++) {
			uint64_t w = tps_wall_now_ns(), t = __rdtsc(), end;
			do { end = tps_wall_now_ns(); } while(end - w < 100000000);
			double rate = (double)(__rdtsc() - t) / (double)(end - w);
			if(rate < lo) lo = rate;
			if(rate > hi) hi = rate;
			sum += rate;
		}
		if(lo > 0 && hi / lo < 1.001) {
			cycles_per_ns = sum / 3;
			calibration_spread = hi / lo - 1;
			clock_mode = 1;
		}
	}
#elif defined(PLATFORM_PC)
	clock_mode = 2;
#endif
}

uint32_t tps_cpu_probe_step_ns(void) {
	qualify_clock();
	uint64_t start = tps_wall_now_ns();
	if(!start) return 0;
	uint64_t last = tps_cpu_now_ns(), smallest = UINT64_MAX;
	unsigned changes = 0;
	do {
		uint64_t now = tps_cpu_now_ns();
		if(now < last) return 0;
		if(now > last) {
			if(now - last < smallest) smallest = now - last;
			changes++;
		}
		last = now;
	} while(tps_wall_now_ns() - start < UINT64_C(150000000));
	return changes >= 2 && smallest <= UINT32_MAX ? (uint32_t)smallest : 0;
}

static struct {
	bool enabled, failed, tick_active, tick_measuring, frame_measuring, sealed, truncated;
	uint32_t measuring, clock_step, published_seq, frame_failed, server_ready;
	struct tps_census census;
	uint8_t scan_bits[4 * 32768 / 8];
	uint8_t vertical_height[256];
	const uint8_t *vertical_sky_input, *vertical_height_input;
	uint64_t scan_start, save_compare_start, save_compress_ns, save_commit_ns, light_epoch;
	/* Direct mapped, exact key check; eviction only loses a certificate. */
	struct {
		const void* world;
		const void* column;
		uint64_t epoch;
		int32_t x, z, dimension;
		unsigned count, kind;
	} light_history[256];
	struct tps_cpu_evidence published;
	struct tps_cpu_evidence cached; /* main-thread-only last complete publication */
	FILE* ticks;
	FILE* frames;
	uint64_t tick_cpu, tick_wall, phase_start;
	uint64_t phase[TPS_PHASE_COUNT], phase_total[TPS_PHASE_COUNT];
	uint64_t nested[TPS_PART_COUNT], nested_total[TPS_PART_COUNT];
	uint64_t wait_ns[3], wait_total[3];
	uint32_t waits[3], waits_total[3];
	struct { unsigned part; uint64_t cpu, wall; } stack[64];
	unsigned depth, phase_id;
	uint32_t tick_count, frame_count;
	uint64_t main_start, main_last, mesher_start, mesher_last;
	uint32_t mesher_ready;
	uint64_t packed_start, packed_total, masked_ns, wait_start;
	unsigned wait_depth;
	bool (*emit)(const char*);
	uint32_t read, write, stream_failed, server_writing, server_failed;
	/* SPSC queue. Exact ticks, bounded storage, overflow invalidates evidence. */
	struct {
		struct tps_census census;
		uint32_t tick;
		uint64_t world, cpu, wall, phase[TPS_PHASE_COUNT], nested[TPS_PART_COUNT];
		double x, y, z;
		int sep;
	} rows[32];
	/* Main-thread aggregate: exact sums/count/max, no percentile claim. */
	struct {
		struct tps_census census;
		uint32_t count, first, last, over50;
		uint64_t cpu, wall, maximum, phase[TPS_PHASE_COUNT], nested[TPS_PART_COUNT];
	} summary;
	uint64_t last_emit, measuring_start;
	uint32_t summaries;
	/* Main-thread only: one immutable refused summary plus the newer aggregate
	 * above. Backpressure coalesces newer ticks without dropping any accounting. */
	char pending_summary[2048];
	bool summary_pending, closing, close_complete;
	uint32_t close_records, close_opportunities;
	uint64_t close_started, close_measured_ns;

#if defined(PLATFORM_PC) && defined(_WIN32)
	DWORD server_id;
	HANDLE mesher_handle;
#elif defined(PLATFORM_PC)
	pthread_t server_id;
	clockid_t mesher_clock;
#else
	lwp_t server_id;
#endif
} obs;

static bool on_server(void) {
	if(!__atomic_load_n(&obs.server_ready, __ATOMIC_ACQUIRE)) return false;
#if defined(PLATFORM_PC) && defined(_WIN32)
	return GetCurrentThreadId() == __atomic_load_n(&obs.server_id, __ATOMIC_ACQUIRE);
#elif defined(PLATFORM_PC)
	return pthread_equal(pthread_self(), __atomic_load_n(&obs.server_id, __ATOMIC_ACQUIRE));
#else
	return LWP_GetSelf() == obs.server_id;
#endif
}

static uint64_t mesher_cpu(void) {
	if(!__atomic_load_n(&obs.mesher_ready, __ATOMIC_ACQUIRE)) return 0;
#if defined(PLATFORM_PC) && defined(_WIN32)
	return cycle_handle_ns(obs.mesher_handle);
#elif defined(PLATFORM_PC)
	struct timespec t;
	if(clock_gettime(obs.mesher_clock, &t)) return 0;
	return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
#else
	return 0;
#endif
}

static void mesher_boundary(bool enter) {
	if(!enter || __atomic_load_n(&obs.mesher_ready, __ATOMIC_RELAXED)) return;
#if defined(PLATFORM_PC) && defined(_WIN32)
	obs.mesher_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
		GetCurrentThreadId());
	if(!obs.mesher_handle) return;
#elif defined(PLATFORM_PC)
	if(pthread_getcpuclockid(pthread_self(), &obs.mesher_clock)) return;
#endif
	obs.mesher_start = tps_cpu_now_ns();
	__atomic_store_n(&obs.mesher_ready, 1, __ATOMIC_RELEASE);
}

static void census_save(enum tps_save_action action, struct tps_save_input* in) {
	if(!on_server() || !obs.tick_active || obs.failed) return;
	if(action == TPS_SAVE_COMPARE_BEGIN) {
		obs.save_compare_start = tps_cpu_now_ns();
		in->active = true;
		obs.census.value[TPS_C_COMPARE_BYTES] += in->length;
		/* FNV is only a cheap rejection filter. Even a deliberate collision
		 * cannot produce an unchanged verdict without the disk byte check. */
		in->hash = UINT64_C(14695981039346656037);
		const uint8_t* bytes = in->bytes;
		for(size_t i = 0; i < in->length; i++) {
			in->hash ^= bytes[i];
			in->hash *= UINT64_C(1099511628211);
		}
		uint64_t previous = (uint64_t)in->previous[0] | (uint64_t)in->previous[1] << 32;
		in->candidate = previous && previous == in->hash;
		in->known = previous && !in->candidate;
	} else if(action == TPS_SAVE_COMPARE_END) {
		obs.census.value[TPS_C_SAVE_COMPARE_NS] += tps_cpu_now_ns() - obs.save_compare_start;
	} else if(in->success) {
		obs.census.value[TPS_C_SAVE_WRITES]++;
		obs.census.value[TPS_C_SAVE_UNCHANGED] += in->equal && in->known;
		obs.census.value[TPS_C_SAVE_UNKNOWN] += !in->known;
		if(in->equal && in->known) {
			obs.census.value[TPS_C_UNCHANGED_COMPRESS_NS] += obs.save_compress_ns;
			obs.census.value[TPS_C_UNCHANGED_COMMIT_NS] += obs.save_commit_ns;
		}
		in->previous[0] = (uint32_t)in->hash;
		in->previous[1] = (uint32_t)(in->hash >> 32);
	} else {
		/* Failed/torn writes do not become a new successful baseline. A retry
		 * re-verifies disk bytes before making any equality claim. */
		in->previous[0] = in->previous[1] = 0;
	}
}

static void census_mutation(unsigned mask) {
	if(!on_server() || !obs.tick_active || obs.failed) return;
	if(mask == 4 && obs.vertical_sky_input && obs.vertical_height_input) {
		bool changed = memcmp(obs.scan_bits, obs.vertical_sky_input, 16384)
			|| memcmp(obs.vertical_height, obs.vertical_height_input, 256);
		obs.vertical_sky_input = obs.vertical_height_input = NULL;
		if(!changed) return;
	}
	obs.census.value[TPS_C_MUTATIONS]++;
	obs.light_epoch++;
	if(!obs.light_epoch) {
		memset(obs.light_history, 0, sizeof(obs.light_history));
		obs.light_epoch = 1;
	}
}

static void census_light(const struct tps_light_input* in) {
	if(!on_server() || !obs.tick_active || obs.failed) return;
	if(in->kind > 2 || !in->count || in->count > 4) { obs.failed = true; return; }
	unsigned base = TPS_C_VERTICAL_CALLS + in->kind * 2;
	uint64_t epoch = obs.light_epoch;
	if(in->kind == 0 && in->columns[1] && in->columns[2]) {
		/* Shared scratch: vertical and source scans never overlap. Compare
		 * actual writes so an idempotent vertical pass retains its certificate. */
		obs.vertical_sky_input = in->columns[1];
		obs.vertical_height_input = in->columns[2];
		memcpy(obs.scan_bits, obs.vertical_sky_input, 16384);
		memcpy(obs.vertical_height, obs.vertical_height_input, 256);
	}
	for(unsigned i = 0; i < in->count; i++) {
		unsigned slot = ((uintptr_t)in->columns[i] / 8 + in->kind * 73) % 256;
		bool key = obs.light_history[slot].world == in->world
			&& obs.light_history[slot].column == in->columns[i]
			&& obs.light_history[slot].kind == in->kind;
		bool same = key && obs.light_history[slot].epoch == epoch
			&& obs.light_history[slot].x == in->x && obs.light_history[slot].z == in->z
			&& obs.light_history[slot].dimension == in->dimension
			&& obs.light_history[slot].count == in->count;
		obs.census.value[base]++;
		obs.census.value[base + 1] += same;
		obs.census.value[TPS_C_LIGHT_LOOKUP_MISSES] += !key;
		obs.light_history[slot].world = in->world;
		obs.light_history[slot].column = in->columns[i];
		obs.light_history[slot].kind = in->kind;
		obs.light_history[slot].epoch = epoch;
		obs.light_history[slot].x = in->x;
		obs.light_history[slot].z = in->z;
		obs.light_history[slot].dimension = in->dimension;
		obs.light_history[slot].count = in->count;
	}
}

static void census_scan(enum tps_scan_action action, struct tps_scan* scan) {
	if(!on_server() || !obs.tick_active || obs.failed) return;
	if(action == TPS_SCAN_MEMO_FALLBACK) {
		obs.census.value[TPS_C_BLOCK_MEMO_FALLBACKS + scan->kind]++;
		return;
	}
	unsigned base = scan->kind ? TPS_C_SKY_COUNT_NS : TPS_C_BLOCK_COUNT_NS;
	uint64_t now = tps_cpu_now_ns();
	if(action == TPS_SCAN_BEGIN) {
		scan->bits = obs.scan_bits;
		obs.scan_start = now;
	} else if(action == TPS_SCAN_COUNT_END) {
		obs.census.value[base] += now - obs.scan_start;
		obs.census.value[base + 4] += scan->count_cells;
	} else if(action == TPS_SCAN_FILL_BEGIN) {
		obs.scan_start = now;
	} else {
		obs.census.value[base + 1] += now - obs.scan_start;
		obs.census.value[base + 2]++;
		/* Sky fill stops at the final positive. Its unvisited suffix is not
		 * evaluated twice; equality means identical emitted source sets. */
		obs.census.value[base + 3] += !scan->memoised && !scan->mismatches
			&& scan->positives == scan->fill_positives;
		obs.census.value[base + 5] += scan->fill_cells;
		unsigned consumed = TPS_C_BLOCK_CONSUMED_CELLS + 2 * scan->kind;
		obs.census.value[consumed] += scan->consumed_cells;
		obs.census.value[consumed + 1] += scan->consumed_sources;
	}
}

static void observe(enum tps_action action, unsigned part) {
	if(!on_server() || !obs.tick_active || obs.failed) return;
	uint64_t now = tps_cpu_now_ns();
	if((clock_mode == 3 && !test_clock) || test_wall_mask) {
		if(part >= TPS_CHANNEL_WAIT && part <= TPS_MEM2_WAIT && action != TPS_PHASE) {
			if(action == TPS_ENTER) {
				if(!obs.wait_depth++) obs.wait_start = now;
			} else if(obs.wait_depth && !--obs.wait_depth) {
				obs.masked_ns += now - obs.wait_start;
			}
		}
		now -= obs.masked_ns;
		if(obs.wait_depth) now = obs.wait_start - obs.masked_ns;
	}
	if(action == TPS_PHASE) {
		if(obs.phase_id < TPS_PHASE_COUNT)
			obs.phase[obs.phase_id] += now - obs.phase_start;
		obs.phase_id = part;
		obs.phase_start = now;
	} else if(action == TPS_ENTER) {
		if(part >= TPS_PART_COUNT || obs.depth == 64) {
			obs.failed = true;
			return;
		}
		if(part == TPS_SAVE) obs.census.value[TPS_C_SAVE_CALLS]++;
		obs.stack[obs.depth].part = part;
		obs.stack[obs.depth].cpu = now;
		obs.stack[obs.depth++].wall = tps_wall_now_ns();
	} else {
		if(!obs.depth || obs.stack[obs.depth - 1].part != part) {
			obs.failed = true;
			return;
		}
		obs.depth--;
		obs.nested[part] += now - obs.stack[obs.depth].cpu;
		if(part == TPS_REGION_COMPRESS) {
			obs.save_compress_ns = now - obs.stack[obs.depth].cpu;
			obs.census.value[TPS_C_SAVE_COMPRESS_NS] += obs.save_compress_ns;
		}
		if(part == TPS_REGION_COMMIT) {
			obs.save_commit_ns = now - obs.stack[obs.depth].cpu;
			obs.census.value[TPS_C_SAVE_COMMIT_NS] += obs.save_commit_ns;
		}
		if(part >= TPS_CHANNEL_WAIT && part <= TPS_MEM2_WAIT) {
			unsigned i = part - TPS_CHANNEL_WAIT;
			obs.waits[i]++;
			obs.wait_ns[i] += tps_wall_now_ns() - obs.stack[obs.depth].wall;
		}
	}
}

static void tick_boundary(struct server_local* s, bool enter) {
	if(enter) {
#if defined(PLATFORM_PC) && defined(_WIN32)
		__atomic_store_n(&obs.server_id, GetCurrentThreadId(), __ATOMIC_RELEASE);
#elif defined(PLATFORM_PC)
		__atomic_store_n(&obs.server_id, pthread_self(), __ATOMIC_RELEASE);
#else
		obs.server_id = LWP_GetSelf();
#endif
		__atomic_store_n(&obs.server_ready, 1, __ATOMIC_RELEASE);
		memset(&obs.census, 0, sizeof(obs.census));
		memset(obs.phase, 0, sizeof(obs.phase));
		memset(obs.nested, 0, sizeof(obs.nested));
		memset(obs.waits, 0, sizeof(obs.waits));
		memset(obs.wait_ns, 0, sizeof(obs.wait_ns));
		obs.depth = 0;
		obs.masked_ns = 0;
		obs.wait_depth = 0;
		obs.phase_id = TPS_PHASE_COUNT;
		obs.tick_wall = tps_wall_now_ns();
		obs.tick_cpu = tps_cpu_now_ns();
		obs.tick_measuring = __atomic_load_n(&obs.measuring, __ATOMIC_RELAXED);
		obs.tick_active = true;
		/* Out-of-update boot/teardown/tool writes have no active certificate. */
		census_mutation(3);
		return;
	}
	uint64_t cpu = tps_cpu_now_ns() - obs.tick_cpu - obs.masked_ns;
	uint64_t wall = tps_wall_now_ns() - obs.tick_wall;
	__atomic_store_n(&obs.server_writing, 1, __ATOMIC_SEQ_CST);
	obs.tick_active = false;
	if(obs.depth || __atomic_load_n(&clock_failed, __ATOMIC_RELAXED)) obs.failed = true;
	obs.tick_count++;
	if(obs.emit && obs.tick_measuring && __atomic_load_n(&obs.measuring, __ATOMIC_SEQ_CST)) {
		uint32_t w = __atomic_load_n(&obs.write, __ATOMIC_RELAXED);
		if(w - __atomic_load_n(&obs.read, __ATOMIC_ACQUIRE) == 32) {
			obs.failed = true;
		} else {
			unsigned i = w % 32;
			obs.rows[i].census = obs.census;
			obs.rows[i].tick = obs.tick_count;
			obs.rows[i].world = s->world_time;
			obs.rows[i].cpu = cpu;
			obs.rows[i].wall = wall;
			obs.rows[i].x = s->player.x;
			obs.rows[i].y = s->player.y;
			obs.rows[i].z = s->player.z;
			obs.rows[i].sep = s->anchor_sep;
			memcpy(obs.rows[i].phase, obs.phase, sizeof(obs.phase));
			memcpy(obs.rows[i].nested, obs.nested, sizeof(obs.nested));
			if(test_row_publication) test_row_publication();
			__atomic_store_n(&obs.write, w + 1, __ATOMIC_RELEASE);
		}
	}
#ifdef PLATFORM_PC
	if(fprintf(obs.ticks, "%u,%" PRIu64 ",%u,%.17g,%.17g,%.17g,%" PRIu64 ",%" PRIu64,
		obs.tick_count, s->world_time, (unsigned)(obs.tick_measuring
			&& __atomic_load_n(&obs.measuring, __ATOMIC_RELAXED)),
		s->player.x, s->player.y, s->player.z, cpu, wall) < 0) obs.failed = true;
	for(unsigned i = 0; i < TPS_PHASE_COUNT; i++) {
		obs.phase_total[i] += obs.phase[i];
		if(fprintf(obs.ticks, ",%" PRIu64, obs.phase[i]) < 0) obs.failed = true;
	}
	for(unsigned i = 0; i < TPS_PART_COUNT; i++) {
		obs.nested_total[i] += obs.nested[i];
		if(fprintf(obs.ticks, ",%" PRIu64, obs.nested[i]) < 0) obs.failed = true;
	}
	for(unsigned i = 0; i < 3; i++) {
		obs.wait_total[i] += obs.wait_ns[i];
		obs.waits_total[i] += obs.waits[i];
		if(fprintf(obs.ticks, ",%u,%" PRIu64, obs.waits[i], obs.wait_ns[i]) < 0) obs.failed = true;
	}
	if(fputs("\n", obs.ticks) == EOF) obs.failed = true;
#else
	for(unsigned i = 0; i < TPS_PHASE_COUNT; i++) obs.phase_total[i] += obs.phase[i];
	for(unsigned i = 0; i < TPS_PART_COUNT; i++) obs.nested_total[i] += obs.nested[i];
	for(unsigned i = 0; i < 3; i++) {
		obs.wait_total[i] += obs.wait_ns[i];
		obs.waits_total[i] += obs.waits[i];
	}
#endif
	/* Never hold the publication sequence odd across file I/O: heartbeat
	 * readers must not spin behind an observer's filesystem wait. */
	__atomic_fetch_add(&obs.published_seq, 1, __ATOMIC_ACQ_REL);
	if(test_publication) test_publication();
	for(unsigned i = 0; i < TPS_PHASE_COUNT; i++)
		__atomic_store_n(&obs.published.phase_us[i], (uint32_t)(obs.phase_total[i] / 1000), __ATOMIC_RELAXED);
	for(unsigned i = 0; i < TPS_PART_COUNT; i++)
		__atomic_store_n(&obs.published.nested_us[i], (uint32_t)(obs.nested_total[i] / 1000), __ATOMIC_RELAXED);
	for(unsigned i = 0; i < 3; i++) {
		__atomic_store_n(&obs.published.wait_us[i], (uint32_t)(obs.wait_total[i] / 1000), __ATOMIC_RELAXED);
		__atomic_store_n(&obs.published.wait_count[i], obs.waits_total[i], __ATOMIC_RELAXED);
	}
	__atomic_store_n(&obs.published.ticks, obs.tick_count, __ATOMIC_RELAXED);
	__atomic_store_n(&obs.published.valid, !obs.failed, __ATOMIC_RELAXED);
	__atomic_fetch_add(&obs.published_seq, 1, __ATOMIC_RELEASE);
	if(obs.failed) __atomic_store_n(&obs.server_failed, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&obs.server_writing, 0, __ATOMIC_SEQ_CST);
}

static void set_hooks(tps_callback callback) {
	__atomic_store_n(&region_tps_save, callback ? census_save : NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&world_tps_mutation, callback ? census_mutation : NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&world_tps_light, callback ? census_light : NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&world_tps_scan, callback ? census_scan : NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&server_tps_observe, callback, __ATOMIC_RELEASE);
	__atomic_store_n(&world_tps_observe, callback, __ATOMIC_RELEASE);
	__atomic_store_n(&region_tps_observe, callback, __ATOMIC_RELEASE);
	__atomic_store_n(&worldgen_tps_observe, callback, __ATOMIC_RELEASE);
	__atomic_store_n(&thread_tps_observe, callback, __ATOMIC_RELEASE);
	__atomic_store_n(&mem2_tps_observe, callback, __ATOMIC_RELEASE);
}

static void client_observe(enum tps_action action, unsigned part) {
	(void)part;
	if(action == TPS_ENTER) obs.packed_start = tps_cpu_now_ns();
	else obs.packed_total += tps_cpu_now_ns() - obs.packed_start;
}

void tps_observer_output(bool (*emit)(const char*)) {
	obs.emit = emit; /* set before starting server threads */
}

/* All persistent TPS buffers are static. No general-MEM2 allocation. */
_Static_assert(sizeof(struct tps_census) == 288, "Census queue budget changed");
_Static_assert(sizeof(obs) <= 65536, "TPS observer census static budget exceeds 64 KiB");

static void drain_rows(void) {
	if(!obs.emit || obs.closing || obs.sealed) return;
	uint32_t r = __atomic_load_n(&obs.read, __ATOMIC_RELAXED);
	uint32_t w = __atomic_load_n(&obs.write, __ATOMIC_ACQUIRE);
	while(r != w) {
		unsigned i = r % 32;
		if(!obs.summary.count) obs.summary.first = obs.rows[i].tick;
		obs.summary.last = obs.rows[i].tick;
		obs.summary.count++;
		obs.summary.cpu += obs.rows[i].cpu;
		obs.summary.wall += obs.rows[i].wall;
		if(obs.rows[i].wall > obs.summary.maximum) obs.summary.maximum = obs.rows[i].wall;
		obs.summary.over50 += obs.rows[i].wall > UINT64_C(50000000);
		for(unsigned j = 0; j < TPS_PHASE_COUNT; j++) obs.summary.phase[j] += obs.rows[i].phase[j];
		for(unsigned j = 0; j < TPS_PART_COUNT; j++) obs.summary.nested[j] += obs.rows[i].nested[j];
		for(unsigned j = 0; j < TPS_C_COUNT; j++)
			obs.summary.census.value[j] += obs.rows[i].census.value[j];

		__atomic_store_n(&obs.read, ++r, __ATOMIC_RELEASE);
	}
}

static void stage_summary(bool closing) {
	uint64_t now = test_emit_clock ? test_emit_clock() : tps_wall_now_ns();
	if(!obs.emit || obs.summary_pending || !obs.summary.count
	   || (!closing && now - obs.last_emit < UINT64_C(1000000000))) return;
	/* Maximum-width payload: 1754 bytes (786 census bytes included).
	 * Fits pending_summary[2048]; even 63 six-byte run-ID escapes plus wrapper/LF
	 * give 2145 bytes, within the telemetry wrapper buffer of 2304. */
	char* line = obs.pending_summary;
	const size_t size = sizeof(obs.pending_summary);
	int n = snprintf(line, size, "{\"event\":\"tps_summary\",\"seq\":%u,\"count\":%u,"
		"\"first\":%u,\"last\":%u,\"clock\":\"%s\",\"step_ns\":%u,"
		"\"cpu_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64
		",\"over50\":%u,\"phase_ns\":[", ++obs.summaries, obs.summary.count,
		obs.summary.first, obs.summary.last, clock_name(), obs.clock_step,
		obs.summary.cpu, obs.summary.wall, obs.summary.maximum, obs.summary.over50);
	for(unsigned j = 0; j < TPS_PHASE_COUNT; j++)
		n += snprintf(line + n, size - n, "%s%" PRIu64, j ? "," : "", obs.summary.phase[j]);
	n += snprintf(line + n, size - n, "],\"nested_ns\":[");
	for(unsigned j = 0; j < TPS_PART_COUNT; j++)
		n += snprintf(line + n, size - n, "%s%" PRIu64, j ? "," : "", obs.summary.nested[j]);
	n += snprintf(line + n, size - n, "],\"census_version\":3,\"census\":[");
	for(unsigned j = 0; j < TPS_C_COUNT; j++)
		n += snprintf(line + n, size - n, "%s%" PRIu64,
			j ? "," : "", obs.summary.census.value[j]);
	n += snprintf(line + n, size - n, "]}");
	if(n < 0 || (size_t)n >= size) obs.stream_failed = 1;
	else obs.summary_pending = true;
	memset(&obs.summary, 0, sizeof(obs.summary));
	obs.last_emit = now;
}

/* At most two summary admissions per opportunity. Never bypass the retained
 * head, renumber it, or change its bytes on a refused admission. */
static bool publish_summaries(bool closing) {
	if(obs.summary_pending) {
		if(!obs.emit(obs.pending_summary)) return false;
		obs.summary_pending = false;
	}
	stage_summary(closing);
	if(obs.summary_pending) {
		if(!obs.emit(obs.pending_summary)) return false;
		obs.summary_pending = false;
	}
	return true;
}

static void publish_output(bool final_attempt) {
	if(!obs.emit || obs.sealed) return;
	bool admitted = publish_summaries(obs.closing);
	if(!obs.closing) return;
	if(obs.close_opportunities < TPS_CLOSE_OPPORTUNITIES) obs.close_opportunities++;
	uint64_t now = test_emit_clock ? test_emit_clock() : tps_wall_now_ns();
	if(!admitted) {
		if(!final_attempt && obs.close_opportunities < TPS_CLOSE_OPPORTUNITIES
		   && now - obs.close_started < TPS_CLOSE_NS) return;
		/* A terminal bound is an actual loss. Keep records equal to the frozen
		 * drained prefix, not the admitted subset: the collector must reject it. */
		obs.stream_failed = 1;
		obs.summary_pending = false;
		memset(&obs.summary, 0, sizeof(obs.summary));
	}
	bool valid = !obs.stream_failed && !__atomic_load_n(&obs.server_failed, __ATOMIC_ACQUIRE);
	char line[224];
	snprintf(line, sizeof(line), "{\"event\":\"tps_end\",\"records\":%u,\"stream_valid\":%s,\"reason\":\"%s\",\"measured_ns\":%" PRIu64 "}",
		obs.close_records,
		(valid && obs.close_complete && !obs.truncated) ? "true" : "false",
		!valid ? "observer_error" : obs.truncated ? "publication_race" :
			obs.close_complete ? "window_complete" : "aborted_or_quiesced",
		obs.close_measured_ns);
	/* A refused seal remains eligible on later opportunities. Admission
	 * never promises delivery after a permanently closed/broken transport. */
	if(obs.emit(line)) obs.sealed = true;
}

size_t tps_observer_static_bytes(void) { return sizeof(obs); }
bool tps_observer_test_emit_clock(uint64_t (*clock)(void)) {
	if(obs.enabled) return false;
	test_emit_clock = clock;
	return true;
}

bool tps_observer_start(const char* prefix) {
	if(obs.enabled || !prefix) return false;
	memset(&obs, 0, sizeof(obs));
	__atomic_store_n(&clock_failed, 0, __ATOMIC_RELAXED);
	obs.clock_step = tps_cpu_probe_step_ns();
	char path[512];
	int n = snprintf(path, sizeof(path), "%sclock.json", prefix);
	if(n < 0 || (size_t)n >= sizeof(path)) return false;
	FILE* clock = fopen(path, "wb");
	if(!clock) return false;
	bool qualified = obs.clock_step && obs.clock_step <= 100000;
	bool saved = fprintf(clock, "{\"observed_cpu_step_ns\":%u,\"qualified\":%s,"
		"\"clock\":\"%s\",\"cycles_per_ns\":%.12g,\"calibration_spread_ppm\":%.6g,"
		"\"preemption_trace_available\":false}\n", obs.clock_step,
		qualified ? "true" : "false", clock_name(), cycles_per_ns, calibration_spread * 1e6) > 0;
	if(fclose(clock)) saved = false;
	if(!qualified || !saved) {
		fprintf(stderr, "[tps] CPU clock rejected: observed step %u ns; maximum 100000 ns. No CPU evidence run was armed.\n", obs.clock_step);
		return false;
	}
#ifdef PLATFORM_PC
	const char* suffix[2] = {"ticks.csv", "frames.csv"};
	FILE** files[2] = {&obs.ticks, &obs.frames};
	for(unsigned i = 0; i < 2; i++) {
		n = snprintf(path, sizeof(path), "%s%s", prefix, suffix[i]);
		if(n < 0 || (size_t)n >= sizeof(path) || !(*files[i] = fopen(path, "wb"))) {
			if(obs.ticks) fclose(obs.ticks);
			return false;
		}
	}
	fputs("update,world_tick,measuring,x,y,z,cpu_ns,wall_ns,prep,sim,save,evict,scan,gen,slot,copy,send,far,tail,scheduler,load,terrain,initial_light,decorate,vertical_quad,block_quad,sky_quad,vertical_final,sky_final,save_object,rearm,region_read,region_parse,region_get,region_compress,region_commit,region_sync,channel_cpu,mutex_cpu,mem2_cpu,channel_waits,channel_wall,mutex_calls,mutex_wall,mem2_waits,mem2_wall\n", obs.ticks);
	fputs("frame,measuring,main_cpu_ns,mesher_cpu_ns\n", obs.frames);
#endif
	obs.last_emit = test_emit_clock ? test_emit_clock() : tps_wall_now_ns();
	obs.enabled = true;
#if defined(PLATFORM_PC) && defined(_WIN32)
	sanity_start = cpu_handle_ns(GetCurrentThread());
	sanity_wall = tps_wall_now_ns();
#endif
	obs.main_start = obs.main_last = tps_cpu_now_ns();
	set_hooks(observe);
	__atomic_store_n(&client_tps_observe, client_observe, __ATOMIC_RELEASE);
	server_tps_boundary = tick_boundary;
	mesher_tps_boundary = mesher_boundary;
	return true;
}

static void observer_frame(bool measuring, bool complete) {
	if(!obs.enabled) return;
	if(obs.closing || obs.sealed) measuring = false;
	bool was_measuring = __atomic_load_n(&obs.measuring, __ATOMIC_RELAXED);
	if(measuring && !was_measuring)
		obs.measuring_start = test_emit_clock ? test_emit_clock() : tps_wall_now_ns();
	__atomic_store_n(&obs.measuring, measuring, __ATOMIC_SEQ_CST);
	/* A close racing publication must fail closed, never seal a prefix as the
	 * complete history. Do not wait: the producer can be lower priority on Wii.
	 * The SC handshake also ensures a later writer observes measuring=false. */
	if(was_measuring && !measuring
	   && __atomic_load_n(&obs.server_writing, __ATOMIC_SEQ_CST)) obs.truncated = true;
	drain_rows();
	if(was_measuring && !measuring && obs.emit) {
		/* A writer already past admission may publish after our drain. The
		 * invalid seal describes only this prefix; never emit rows after it. */
		obs.closing = true;
		obs.close_complete = complete;
		obs.close_records = __atomic_load_n(&obs.read, __ATOMIC_RELAXED);
		obs.close_started = test_emit_clock ? test_emit_clock() : tps_wall_now_ns();
		obs.close_measured_ns = obs.close_started - obs.measuring_start;
	}
	publish_output(false);
	uint64_t main = tps_cpu_now_ns(), mesh = mesher_cpu();
	obs.frame_count++;
#ifdef PLATFORM_PC
	uint64_t mesh_delta = obs.mesher_last && mesh >= obs.mesher_last ? mesh - obs.mesher_last : 0;
	if(fprintf(obs.frames, "%u,%u,%" PRIu64 ",%" PRIu64 "\n", obs.frame_count,
		(unsigned)(measuring && obs.frame_measuring), main - obs.main_last, mesh_delta) < 0)
		obs.frame_failed = 1;
#endif
	obs.frame_measuring = measuring;
	obs.main_last = main;
	obs.mesher_last = mesh;
}

void tps_observer_frame(bool measuring) { observer_frame(measuring, true); }
void tps_observer_close(bool complete) {
	observer_frame(false, complete);
	/* Shutdown/quiescence closes transport admission immediately after this
	 * call. Fixed nonblocking attempts; no sleep, spin-until-ready, or tick wait. */
	for(unsigned i = 0; obs.enabled && obs.closing && !obs.sealed
	    && i < TPS_FINAL_FLUSH_ATTEMPTS; i++)
		publish_output(i + 1 == TPS_FINAL_FLUSH_ATTEMPTS);
}

bool tps_observer_test_row_publication(void (*hook)(void)) {
	if(obs.enabled) return false;
	test_row_publication = hook;
	return true;
}

void tps_observer_snapshot(struct tps_cpu_evidence* out) {
	memset(out, 0, sizeof(*out));
	if(!obs.enabled) return;
	uint32_t seq = __atomic_load_n(&obs.published_seq, __ATOMIC_ACQUIRE);
	/* Never spin behind a preempted lower-priority Broadway server. Reuse the
	 * previous complete cumulative sample and catch up at the next heartbeat. */
	if(!(seq & 1)) {
		out->valid = __atomic_load_n(&obs.published.valid, __ATOMIC_RELAXED);
		out->ticks = __atomic_load_n(&obs.published.ticks, __ATOMIC_RELAXED);
		for(unsigned i = 0; i < TPS_PHASE_COUNT; i++)
			out->phase_us[i] = __atomic_load_n(&obs.published.phase_us[i], __ATOMIC_RELAXED);
		for(unsigned i = 0; i < TPS_PART_COUNT; i++)
			out->nested_us[i] = __atomic_load_n(&obs.published.nested_us[i], __ATOMIC_RELAXED);
		for(unsigned i = 0; i < 3; i++) {
			out->wait_us[i] = __atomic_load_n(&obs.published.wait_us[i], __ATOMIC_RELAXED);
			out->wait_count[i] = __atomic_load_n(&obs.published.wait_count[i], __ATOMIC_RELAXED);
		}
		if(seq == __atomic_load_n(&obs.published_seq, __ATOMIC_ACQUIRE))
			obs.cached = *out;
	}
	*out = obs.cached;
	out->enabled = true;
	out->clock_step_ns = obs.clock_step;
	out->clock_mode = clock_mode;
	out->packed_us = (uint32_t)(obs.packed_total / 1000);
	out->main_us = (uint32_t)((obs.main_last - obs.main_start) / 1000);
	if(__atomic_load_n(&obs.mesher_ready, __ATOMIC_ACQUIRE))
		out->mesher_us = obs.mesher_last >= obs.mesher_start ? (uint32_t)((obs.mesher_last - obs.mesher_start) / 1000) : 0;
	out->frames = obs.frame_count;
	if(obs.frame_failed || obs.stream_failed || __atomic_load_n(&clock_failed, __ATOMIC_RELAXED)) out->valid = 0;
}

bool tps_observer_test_clock(uint64_t (*clock)(void)) {
	if(obs.enabled) return false;
	test_clock = clock;
	return true;
}

bool tps_observer_test_wall_mask(bool enable) {
	if(obs.enabled) return false;
	test_wall_mask = enable;
	return true;
}

bool tps_observer_test_publication(void (*hook)(void)) {
	if(obs.enabled) return false;
	test_publication = hook;
	return true;
}

bool tps_observer_stop(void) {
	if(!obs.enabled) return false;
	set_hooks(NULL);
	__atomic_store_n(&client_tps_observe, NULL, __ATOMIC_RELEASE);
	server_tps_boundary = NULL;
	mesher_tps_boundary = NULL;
	tps_observer_close(false);
	bool ok = !obs.failed && !obs.frame_failed && !obs.stream_failed && !obs.truncated
		&& !obs.summary_pending && (!obs.closing || obs.sealed)
		&& !__atomic_load_n(&clock_failed, __ATOMIC_RELAXED);
#ifdef PLATFORM_PC
	ok = ok && !ferror(obs.ticks) && !ferror(obs.frames);
	if(fclose(obs.ticks)) ok = false;
	if(fclose(obs.frames)) ok = false;
#endif
#if defined(PLATFORM_PC) && defined(_WIN32)
	fprintf(stderr, "[tps] sanity main: GetThreadTimes=%.3f ms cycles=%.3f ms wall=%.3f ms\n",
		(double)(cpu_handle_ns(GetCurrentThread()) - sanity_start) / 1e6,
		(double)(tps_cpu_now_ns() - obs.main_start) / 1e6,
		(double)(tps_wall_now_ns() - sanity_wall) / 1e6);
	if(obs.mesher_handle) CloseHandle(obs.mesher_handle);
#endif
	obs.enabled = false;
	return ok;
}
