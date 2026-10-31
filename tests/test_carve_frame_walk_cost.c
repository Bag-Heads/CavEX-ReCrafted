/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * test_carve_frame_walk_cost -- the carve observer must not make the SERVER
 * thread's far-skin extraction crawl.
 *
 * Real-Wii evidence (build 3cdbc5a8, split_anchor_6m seed 42 sep 256 far 6
 * voxel, WITH --cw-carve-census): wd_srv sat in WDS_FAR_EXTRACT on 708 of the
 * last 863 heartbeats and srv_far_us_max climbed 52 ms -> 1.15 s -> 6.07 s ->
 * 15.0 s for ONE far column, srv_loop_it fell to 1-2 per second, anchor_ready
 * never arrived. The same scene on the same build family WITHOUT the observer
 * peaked at 578 ms per far column and reached anchor_ready in 48 s.
 *
 * Root cause: the far path itself allocates almost nothing (part 1 below counts
 * it), so the cost is not per-allocation bookkeeping. It is that
 * carve_observer_frame ran the observer's WHOLE reconciliation -- the same
 * carve_observer_world() + carve_observer_snapshot() pair the once-per-second
 * census (stage_set) already runs -- on EVERY main-loop frame, i.e. 60 times a
 * second: two O(ledger) mark sweeps, a hash probe per plane of every resident
 * chunk, a 4096-cell all-air scan of every far-LOD chunk, the whole-carve
 * boundary-tag walk, and a third O(ledger) attribution walk, the last two while
 * holding mem2_lock. On a single-core Wii that starves the server thread, whose
 * far extraction needs mem2_lock for every mem2_alloc/mem2_free, and the cost
 * grows with the resident population -- which is why it CLIMBED.
 *
 * This test pins all three halves of the finding:
 *   part 1  how many carve events one far column extraction actually produces
 *           (the number that rules per-allocation bookkeeping out);
 *   part 2  the cost gate: a modelled main-loop/server interleave with the
 *           observer armed must stay under 2x the unarmed cost;
 *   part 3  the identity gate: with the walk interval forced back to 1 (the
 *           pre-fix per-frame cadence) the emitted carve_attrib / carve_tenant
 *           / carve_census records for a scripted allocation sequence are
 *           byte-identical to the pre-fix build's.
 */
#include "support/split_residency_fixture.h"
#include "carve_observer.h"
#include "platform/time.h"
#include "world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Captured from the PRE-fix build (carve_observer_frame reconciling on every
 * frame) by running this same scripted sequence through it; re-measured on the
 * post-fix build with carve_observer_walk_interval(1). Both must match.
 *
 * REBASED 2026-09-08 (lane/dl-ladder-pools) from 71 / 0x5eb43b3f3ff35283. What
 * this gate protects is the walk CADENCE: that forcing the interval back to 1
 * reproduces the pre-cadence-fix record stream exactly. It is a content hash
 * over a fixed-size record SET, so it moves whenever the set legitimately
 * changes shape. Two changes landed here, both deliberate:
 *
 *   1. the carve_census line gained the fixed-tenant pool keys (pool_bytes,
 *      pool_{far,view}_{slots,used,peak,over}; carve_observer.c's stage_set) --
 *      content only, no extra record;
 *   2. a TWELFTH tenant, CARVE_SLOTS, owns the slot-pool reservations that
 *      previously bypassed carve_note entirely (carve_observer.h). A census set
 *      is CARVE_TENANTS carve_tenant records plus one carve_census, so every
 *      set gains exactly one record.
 *
 * That accounts for the count precisely: 5 sets in this 300-frame script, so
 * 71 -> 76 and not one record more. A cadence regression hiding behind the
 * rebase would have moved the count by a multiple of the SET size (12 + 1) or
 * changed the number of sets, neither of which is +5.
 *
 * RE-BASED AGAIN 2026-09-08 (same lane, pools round 3) from
 * 0xdb56fb5a17e90a37. CONTENT ONLY: the carve_census line gained the demand
 * carve keys (pool_{far,view}_{slabs,slabs_max,cfail}) and the ledger keys
 * (ledger, ledger_cap, ledger_live, ledger_peak, ledger_full), and pool_bytes
 * now reports the CARVED total instead of the boot reservation -- which on PC
 * is 0 either way, because PC has no carve and therefore no pools. The RECORD
 * COUNT is the invariant that would catch a cadence regression, and it is
 * unchanged at 76: no record was added, removed or reordered. */
/* RE-BASED 2026-09-13 (mp-split-0912 W2), 76 -> 81, for exactly the reason
 * change 2 above describes and by exactly the same arithmetic: a THIRTEENTH
 * tenant, CARVE_MP_BACKING, owns the MP compressed-body reservation
 * (platform/mem2.h, MEM2_MP_BACKING_BYTES) that would otherwise be several MB
 * missing from the per-tenant census. A census set is CARVE_TENANTS
 * carve_tenant records plus one carve_census, and this 300-frame script emits
 * 5 sets, so the count gains exactly 5 -- not a multiple of the SET size
 * (13 + 1) and not a change in the number of sets, which is what a cadence
 * regression hiding behind this re-base would have looked like. */
#define CARVE_FRAME_WALK_GOLDEN_RECORDS 81u
/* 2026-09-09 M2 mesh blocks: struct chunk_mesher_rpc grew by a block pointer
 * + size per result (8 slots x 16 B), so the census "rpc_static" field moved
 * 1,539,912 -> 1,540,040. Every other byte of the 76 records is unchanged
 * (verified by dumping both trees' records and diffing: one field). */
/* 2026-09-11: the carve_census record gained the six pool_col_* fields
 * (MEM2_POOL_COLUMN); the 76 records are otherwise identical (PC has no
 * pools, so every new field is 0). Previous golden 0x57c4f5217c7b53db. */
/* 2026-09-13 (mp-split-0912 W2): five new carve_tenant records (the
 * CARVE_MP_BACKING line, one per census set) change the record SET, so the
 * content hash moves with the count. Previous golden 0x3be6e1fc35c48df0. */
/* 2026-09-24 (integration-0924): the hash now skips the VALUES of the four
 * static-footprint fields (hash_sink's k_abi_fields), which are host struct
 * sizes: "obs" differed by 32 B between MinGW and Linux glibc, so the old
 * golden held on Windows x64 only. Recomputed once, not re-captured: the
 * pre-change Windows record dump and the Linux dump, both normalized, hash to
 * this same value, so no record content or order changed. Previous golden
 * 0x23445235c006be33. */
#define CARVE_FRAME_WALK_GOLDEN_HASH UINT64_C(0x7a2efff6642dc8c7)

/* ------------------------------------------------------------------ part 1 */
static unsigned long ev_count[CARVE_OWNER + 1];
static void counting(enum carve_action action, uintptr_t p, uintptr_t q,
					 size_t bytes, size_t aux) {
	(void)p; (void)q; (void)bytes; (void)aux;
	if((unsigned)action <= CARVE_OWNER)
		ev_count[action]++;
}

static unsigned long far_column_events(struct server_local* s, int cx, int cz) {
	static struct far_rpc_slot slot;
	memset(ev_count, 0, sizeof(ev_count));
	mem2_carve_observe = counting;
	bool got = server_world_extract_far_skin(&s->world, cx, cz, &slot, false);
	mem2_carve_observe = NULL;
	assert(got);
	return ev_count[CARVE_ALLOC] + ev_count[CARVE_FREE] + ev_count[CARVE_TAG]
		+ ev_count[CARVE_RESIZE] + ev_count[CARVE_TRANSFER];
}

/* ------------------------------------------------------------------ part 2 */
/* The ledger's size is what makes a reconciliation expensive, so the model has
 * to carry a realistic resident population: the Wii scene holds one voxel block
 * per client chunk, five planes per resident server column and a display list
 * per meshed chunk -- thousands of live allocations. These stand in for them,
 * and are allocated in BOTH the armed and the unarmed run so the only thing
 * measured is the observer's own work. A walk costs O(ledger capacity), so the
 * ratios below scale with LEDGER: measured on this machine, 4,000 live
 * allocations give 14x before / 1.4x after, 8,000 give 43x before / 1.9x after.
 * The ledger-size-INDEPENDENT regression guard is the reconciliation COUNT
 * asserted below -- 16 for 960 frames, against 960 before. */
enum { LEDGER = 4000, COLUMNS = 64, FRAMES_PER_COLUMN = 15 };
static void* ledger[LEDGER];
static struct far_rpc_slot slot;

static void ledger_fill(void) {
	for(unsigned i = 0; i < LEDGER; i++) {
		size_t n = 64 + (i % 5) * 16;
		ledger[i] = mem2_tag(mem2_alloc(n), n,
							 (i % 3) ? CARVE_COLUMN : CARVE_LIST);
		assert(ledger[i]);
	}
}
static void ledger_drop(void) {
	for(unsigned i = 0; i < LEDGER; i++)
		mem2_free(ledger[i]);
}

/* Far columns only -- the server thread's own work, with the observer armed
 * (every mem2_alloc/mem2_free of the extraction going through the ledger) or
 * not. This is the brief's literal gate, and the measurement that rules per-
 * allocation bookkeeping out as the cause. */
static double extract_only(struct server_local* s, bool armed, int base) {
	if(armed) assert(carve_observer_start());
	ledger_fill();
	ptime_t t0 = time_get();
	for(unsigned c = 0; c < COLUMNS; c++)
		assert(server_world_extract_far_skin(&s->world, base + (int)c * 3,
											 base - (int)c * 3, &slot, false));
	double dt = time_diff_s(t0, time_get());
	ledger_drop();
	if(armed) assert(carve_observer_stop());
	return dt;
}

/* The real mechanism, modelled on one core: source/main.c calls
 * carve_observer_frame once per 60 Hz frame while the server thread extracts
 * far columns. FRAMES_PER_COLUMN = 15 is four far columns per modelled second,
 * the rate the observer-free real-Wii run sustained (anchor_ready at 48 s).
 * `interval` 1 is the pre-fix per-frame reconciliation, 0 means leave the
 * production default. */
static double interleaved(struct server_local* s, struct world* w, bool armed,
						  uint32_t interval, int base, unsigned* walks) {
	if(armed) {
		assert(carve_observer_start());
		if(interval) carve_observer_walk_interval(interval);
	}
	ledger_fill();
	uint32_t frame = 1;
	ptime_t t0 = time_get();
	for(unsigned c = 0; c < COLUMNS; c++) {
		for(unsigned f = 0; f < FRAMES_PER_COLUMN; f++, frame++)
			carve_observer_frame(w, frame, c, 0, false);
		assert(server_world_extract_far_skin(&s->world, base + (int)c * 3,
											 base - (int)c * 3, &slot, false));
	}
	double dt = time_diff_s(t0, time_get());
	*walks = armed ? carve_observer_walk_count() : 0;
	ledger_drop();
	if(armed) assert(carve_observer_stop());
	return dt;
}

/* ------------------------------------------------------------------ part 3 */
#define FNV_OFFSET UINT64_C(14695981039346656037)
static uint64_t record_hash;
static unsigned record_count;
/* Static-footprint fields: sizeof() of host structures, not observer
 * behaviour. "obs" is sizeof(obs) + the slot/burst arrays and measured 32 B
 * larger under Linux glibc than MinGW (thread primitives differ);
 * mesh_static / rpc_static / light_heap are likewise struct sizes that agree
 * on today's two x64 hosts only because both have 8-byte pointers. What this
 * gate protects is the walk CADENCE and the record content it produces, so a
 * key listed here is hashed as its name alone and its value is skipped. */
static const char* const k_abi_fields[] = {"\"obs\":", "\"mesh_static\":",
										   "\"rpc_static\":", "\"light_heap\":"};
static const char* abi_field_at(const char* p) {
	for(size_t k = 0; k < sizeof(k_abi_fields) / sizeof(k_abi_fields[0]); k++) {
		size_t n = strlen(k_abi_fields[k]);
		if(!strncmp(p, k_abi_fields[k], n))
			return p + n;
	}
	return NULL;
}
static bool hash_sink(const char* line) {
	if(getenv("CW_DUMP_RECORDS")) fprintf(stderr, "REC %s\n", line);
	record_count++;
	for(const char* p = line; *p; p++) {
		const char* value = abi_field_at(p);
		for(; value && p < value; p++) {
			record_hash ^= (unsigned char)*p;
			record_hash *= UINT64_C(1099511628211);
		}
		if(value) {
			while(*p && *p != ',' && *p != '}')
				p++;
			if(!*p)
				break;
		}
		record_hash ^= (unsigned char)*p;
		record_hash *= UINT64_C(1099511628211);
	}
	record_hash ^= (unsigned char)'\n'; /* the wire's framing byte */
	record_hash *= UINT64_C(1099511628211);
	return true;
}

/* A scripted allocation sequence driven at the pre-fix per-frame walk cadence.
 * Every record the observer can emit -- the three carve_attrib points, the
 * eleven carve_tenant lines and the carve_census that closes each set -- goes
 * through hash_sink, so the constant below is a byte-for-byte fingerprint of
 * the observer's whole OUTPUT for this input. */
enum { SCRIPT_FRAMES = 300 };
static uint64_t scripted_records(void) {
	assert(carve_observer_start());
	carve_observer_walk_interval(1); /* pre-fix cadence: reconcile every frame */
	struct world w;
	world_create(&w);
	record_hash = FNV_OFFSET;
	record_count = 0;
	static void* churn[SCRIPT_FRAMES];
	for(unsigned frame = 1; frame <= SCRIPT_FRAMES; frame++) {
		size_t n = 24 + (frame % 11) * 8;
		churn[frame - 1] = mem2_tag(mem2_alloc(n), n,
									(frame % 4) ? CARVE_COLUMN : CARVE_LIST);
		assert(churn[frame - 1]);
		carve_observer_frame(&w, frame, frame, frame % 5, frame >= 150);
		assert(carve_observer_emit_next(hash_sink));
		if(frame % 60 == 0)
			assert(carve_observer_emit(&w, frame, true, hash_sink));
	}
	for(unsigned i = 0; i < SCRIPT_FRAMES; i++)
		mem2_free(churn[i]);
	world_destroy(&w);
	assert(carve_observer_stop());
	return record_hash;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	blocks_init();
	struct server_local* s = split_fixture();
	struct world w;
	world_create(&w);

	/* Part 1 -- the per-column allocation count. Five worldgen plane
	 * allocations and five frees is the whole of it, so no plausible
	 * per-allocation bookkeeping cost can explain seconds per column. */
	unsigned long per_column = far_column_events(s, 500, 500);
	printf("far column: %lu observed carve events "
		   "(alloc %lu, free %lu, tag %lu, resize %lu, transfer %lu)\n",
		   per_column, ev_count[CARVE_ALLOC], ev_count[CARVE_FREE],
		   ev_count[CARVE_TAG], ev_count[CARVE_RESIZE],
		   ev_count[CARVE_TRANSFER]);
	assert(per_column < 64);

	/* Part 3 -- the identity gate. */
	uint64_t h = scripted_records();
	printf("scripted %u frames: %u records, hash 0x%016llx\n",
		   (unsigned)SCRIPT_FRAMES, record_count,
		   (unsigned long long)h);
	assert(record_count == CARVE_FRAME_WALK_GOLDEN_RECORDS);
	if(h != CARVE_FRAME_WALK_GOLDEN_HASH)
		fprintf(stderr, "carve frame walk hash %016llx (golden %016llx)\n",
				(unsigned long long)h, (unsigned long long)CARVE_FRAME_WALK_GOLDEN_HASH);
	assert(h == CARVE_FRAME_WALK_GOLDEN_HASH);

	/* Part 2 -- the cost gates. Every leg runs over its own band of columns
	 * so no leg can be served by another's worldgen cache. */
	/* Best of three per leg: the gate is a cost ratio, and a scheduler
	 * hiccup on a shared CI box is noise, not evidence. */
	double x_off = 1e9, x_on = 1e9;
	for(unsigned r = 0; r < 3; r++) {
		double a = extract_only(s, false, 2000 + (int)r * 400);
		double b = extract_only(s, true, 4000 + (int)r * 400);
		if(a < x_off) x_off = a;
		if(b < x_on) x_on = b;
	}
	double x_ratio = x_off > 0 ? x_on / x_off : 0;
	printf("far extraction only, %u columns: unarmed %.4f s, armed %.4f s "
		   "(%.2fx) -- %.1f us/column unarmed\n", (unsigned)COLUMNS, x_off,
		   x_on, x_ratio, x_off * 1e6 / COLUMNS);
	assert(x_ratio < 2.0);

	unsigned walks_pre = 0, walks_post = 0, walks_none = 0;
	double i_off = 1e9, i_pre = 1e9, i_post = 1e9;
	for(unsigned r = 0; r < 3; r++) {
		double a = interleaved(s, &w, false, 0, 6000 + (int)r * 400, &walks_none);
		double b = interleaved(s, &w, true, 1, 8000 + (int)r * 400, &walks_pre);
		double c = interleaved(s, &w, true, 0, 10000 + (int)r * 400, &walks_post);
		if(a < i_off) i_off = a;
		if(b < i_pre) i_pre = b;
		if(c < i_post) i_post = c;
	}
	double pre_ratio = i_off > 0 ? i_pre / i_off : 0;
	double post_ratio = i_off > 0 ? i_post / i_off : 0;
	printf("main-loop interleave (%u columns, %u frames each): unarmed %.4f s, "
		   "armed BEFORE %.4f s (%.2fx, %u reconciliations), armed AFTER "
		   "%.4f s (%.2fx, %u reconciliations)\n", (unsigned)COLUMNS,
		   (unsigned)FRAMES_PER_COLUMN, i_off, i_pre, pre_ratio, walks_pre,
		   i_post, post_ratio, walks_post);
	assert(walks_pre == COLUMNS * FRAMES_PER_COLUMN);
	assert(walks_post <= COLUMNS * FRAMES_PER_COLUMN
			   / CARVE_FRAME_WALK_FRAMES + 2);
	assert(post_ratio < 2.0);
	assert(post_ratio < pre_ratio);

	world_destroy(&w);
	split_destroy(s);
	printf("PASS carve frame-walk cost: %lu carve events per far column, "
		   "extraction %.2fx, main-loop interleave %.2fx (was %.2fx), "
		   "records byte-identical\n", per_column, x_ratio, post_ratio,
		   pre_ratio);
	return 0;
}
