#include "series.h"
#define CHURN_STEPS (8u * (sizeof(churn_series) / sizeof(churn_series[0])))
static struct { size_t largest, total; unsigned spill, trunc; uint32_t probes; } churn_baseline[CHURN_STEPS];
/* Allocate on the checkpoint state, then restore allocator metadata. A
 * normal allocate/free probe can change first-fit list order when a hole is
 * consumed whole. Save every free header and its successor's back tag; the
 * allocation writes nowhere else except free payload and heap control. */
static uint32_t churn_probe_admissions(const unsigned* sizes, unsigned count) {
	static struct { heap_block* block; heap_block saved; heap_block* next; u32 back; } saved[CHURN_SLOTS];
	heap_cntrl control = churn_heap;
	unsigned n = 0;
	for(heap_block* b = churn_heap.first; b != __lwp_heap_tail(&churn_heap); b = b->next) {
		assert(n < CHURN_SLOTS);
		saved[n].block = b; saved[n].saved = *b;
		saved[n].next = __lwp_heap_nextblock(b);
		saved[n].back = saved[n].next->back_flag;
		n++;
	}
	uint32_t admitted = 0;
	assert(count <= 32);
	for(unsigned p = 0; p < count; p++) {
		if(__lwp_heap_allocate(&churn_heap, sizes[p] + 256)) admitted |= 1u << p;
		for(unsigned i = 0; i < n; i++) {
			*saved[i].block = saved[i].saved;
			saved[i].next->back_flag = saved[i].back;
		}
		churn_heap = control;
	}
	return admitted;
}
static void test_shared_carve_churn(void) {
	/* Isolate lever 2 using the same overlay reserve on both sides.
	 * The historical combined-lever baseline remains selectable explicitly. */
	bool low = getenv("CW_CHURN_LOW_HEADROOM") != NULL;
	bool compact = !low || getenv("CW_CHURN_COMPACT") != NULL;
	bool legacy = !low || getenv("CW_CHURN_LEGACY_BASELINE") != NULL;
	bool count_only = getenv("CW_CHURN_COUNT_ONLY") != NULL;
	static struct displaylist lists[10000];
	if(getenv("CW_CHURN_LOW_HEADROOM")) churn_bytes = 33905u * 1024;
	if(getenv("CW_CHURN_MIN_TAIL")) churn_min_tail = strtoul(getenv("CW_CHURN_MIN_TAIL"), NULL, 10);
	printf("Churn compact=%u legacy_baseline=%u\n", compact, legacy);
	printf("Churn carve=%zu reserve=%u minimum_tail=%zu (MODEL, not captured allocation order)\n", CHURN_BYTES, CHURN_SPILL_BYTES, churn_min_tail);
	fflush(stdout);
	/* Aggregate telemetry does not encode a per-list histogram. Reconstruct
	 * a deterministic geometric mix of 4..1024 vertices (9/12-byte strides),
	 * distributing each heartbeat's slack in proportion to retained bytes.
	 * Both runs execute the same admissions, remeshes and tenant script. */
	static const unsigned vertices[] = {4, 8, 16, 32, 64, 128, 256, 512, 768, 1024};
	size_t worst_largest = 0, worst_total = 0, min_largest[2], min_total[2];
	unsigned spill[2], trunc[2];
	unsigned regressions = 0;
	/* Stress sizes, NOT observed maxima: heartbeat dln/dls/dlp do not
	 * encode individual list sizes. Probe real late carve admissions. */
	static const unsigned probes[] = {12352, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152};
	for(unsigned mode = 0; mode < 2; mode++) {
		churn_active = true; churn_shrink = mode != 0;
		churn_spills = churn_trunc = 0; churn_spill_live = 0;
		memset(churn_tails, 0, sizeof(churn_tails)); churn_tail_index = 0;
		void* raw = malloc(CHURN_BYTES + sizeof(heap_block) + 31); assert(raw);
		churn_arena = (unsigned char*)(((uintptr_t)raw + 31) & ~(uintptr_t)31);
		assert(__lwp_heap_init(&churn_heap, churn_arena, CHURN_BYTES, 32));
		/* Demonstrate that released tails serve another tenant and that free
		 * coalesces predecessor + successor into the original single hole. */
		void* a = churn_allocate(32768), *b = churn_allocate(32768);
		assert(a && b);
		heap_block* oldblock = __lwp_heap_usrblockat(b);
		uintptr_t oldend = (uintptr_t)oldblock + __lwp_heap_blocksize(oldblock);
		if(mode) {
			assert(churn_resize(b, 32768, 1024));
			void* tenant = churn_allocate(10240); assert(tenant);
			assert((uintptr_t)tenant > (uintptr_t)b + 1024 && (uintptr_t)tenant < oldend);
			churn_tail_reuses++;
			churn_free(tenant, 10240);
		}
		churn_free(b, mode ? 1024 : 32768); churn_free(a, 32768);
		size_t largest, total; churn_info(&largest, &total);
		assert(largest == CHURN_BYTES - 8 && total == largest);
		void* columns[128]; void* far[128];
		for(unsigned i = 0; i < 128; i++) {
			columns[i] = churn_allocate(81920); far[i] = churn_allocate(10240);
			assert(columns[i] && far[i]);
		}
		/* The direct tail/coalescence fixture above always exercises the
		 * helper. The workload below executes the configured finalizer. */
		displaylist_set_compact(mode && compact);
		size_t overlay_size = (mode || !legacy) ? 256 * 256 : 8192 * 256;
		void* overlay = churn_allocate(overlay_size); assert(overlay);
		unsigned count = 0;
		min_largest[mode] = min_total[mode] = CHURN_BYTES;
		for(unsigned step = 0; step < CHURN_STEPS; step++) {
			unsigned sample = step % (sizeof(churn_series)/sizeof(churn_series[0]));
			unsigned n = churn_series[sample].lists;
			assert(n < 10000);
			while(count > n) displaylist_destroy(&lists[--count]);
			for(unsigned i = 0; i < n; i++) {
				if(i < count && i % 16 != step % 16) continue;
				if(i < count) displaylist_destroy(&lists[i]);
				unsigned v = vertices[(i * 7 + step) % 10], stride = i % 2 ? 12 : 9;
				size_t used = (35 + v * stride + 31) & ~(size_t)31;
				/* PRESIZE HINT = the prior build FINAL INDEX, which is exactly
				 * what the engine passes: chunk_mesher.c:2072 snapshots
				 * c->mesh[k].index into request.prev_index and
				 * chunk_mesher.c:1557 hands THAT to
				 * displaylist_init_presized. It is NOT the prior CAPACITY.
				 *
				 * This model used to add the heartbeat mean dls/dln slack to
				 * the hint (used + slack - 32), so the hint stood in for the
				 * prior capacity and the retained slack was INJECTED. That was
				 * a faithful stand-in only while the sizing policy generated
				 * no slack of its own: displaylist_init_presized rounded the
				 * hint to the 32 B cache line, so whatever went in came out.
				 * Since the ladder landed (2026-09-08) the policy generates
				 * the slack itself -- capacity is rung(prior_index), and the
				 * measured dls/dln was captured under the PRE-ladder policy --
				 * so injecting it on top double-counted, inflating every list
				 * in the model by up to the rung step. Measured cost of that
				 * double count in this fixture: mode-0 spills 325 -> 3002,
				 * min free 1,496,816 -> 910,312 B, lever min largest
				 * 2,696,232 -> 422,872 B, plus 124 late-admission findings --
				 * all of it an artifact of the hint, none of it reachable from
				 * chunk_mesher.c. With the engine-faithful hint the same
				 * ladder build measures 0 spills, 0 findings, and the floors
				 * asserted below.
				 *
				 * churn_series[].slack is therefore no longer consumed here;
				 * it stays in series.h as the captured record. */
				displaylist_init_presized(&lists[i], 4, stride, stride == 12, used);
				displaylist_set_tinted(&lists[i], stride == 12);
				/* Use the real emitter for every byte and actual finalize/truncation. */
				for(unsigned j = 0; j < v; j++) displaylist_vertex(&lists[i], j, j, j, j, j, j);
				if(!lists[i].truncated) displaylist_finalize(&lists[i], v);
				churn_trunc += lists[i].truncated;
			}
			count = n;
			/* Other tenants churn AFTER tail release, including overlay-sized
			 * page runs. Free order alternates, exercising both merge orders. */
			unsigned k = step % 128;
			churn_free(columns[k], 81920); churn_free(far[k], 10240);
			churn_tenant = true;
			void* pages = churn_allocate(256); assert(pages);
			far[k] = churn_allocate(10240); columns[k] = churn_allocate(81920);
			assert(far[k] && columns[k]); churn_free(pages, 256);
			churn_tenant = false;
			churn_info(&largest, &total);
			if(largest < min_largest[mode]) min_largest[mode] = largest;
			if(total < min_total[mode]) min_total[mode] = total;
			if(!mode) {
				churn_baseline[step].probes = churn_probe_admissions(probes, sizeof(probes)/sizeof(probes[0]));
				churn_baseline[step].largest = largest; churn_baseline[step].total = total;
				churn_baseline[step].spill = churn_spills; churn_baseline[step].trunc = churn_trunc;
			} else {
				uint32_t admitted = churn_probe_admissions(probes, sizeof(probes)/sizeof(probes[0]));
				for(unsigned p = 0; p < sizeof(probes)/sizeof(probes[0]); p++) {
					if(!(admitted & (1u << p)) && (churn_baseline[step].probes & (1u << p))) {
						fprintf(stderr, "FINDING late allocation step=%u sample=%u request=%u baseline_largest=%zu lever_largest=%zu free=%zu\n", step, sample, probes[p], churn_baseline[step].largest, largest, total);
						regressions++;
					}
				}
				if(churn_baseline[step].largest > largest && churn_baseline[step].largest - largest > worst_largest)
					worst_largest = churn_baseline[step].largest - largest;
				if(churn_baseline[step].total > total && churn_baseline[step].total - total > worst_total)
					worst_total = churn_baseline[step].total - total;
				assert(churn_spills <= churn_baseline[step].spill);
				assert(churn_trunc <= churn_baseline[step].trunc);
				/* Retain the legacy 36 MiB bound only for that model.
				 * Low-headroom safety is the late-admission assertion below. */
				if(!count_only) assert(worst_total == 0);
			}
		}
		spill[mode] = churn_spills; trunc[mode] = churn_trunc;
		while(count) displaylist_destroy(&lists[--count]);
		for(unsigned i = 0; i < 128; i++) { churn_free(far[i], 10240); churn_free(columns[i], 81920); }
		churn_free(overlay, overlay_size); churn_info(&largest, &total);
		assert(total == CHURN_BYTES - 8 && largest == total && !churn_spill_live);
		free(raw); churn_active = false;
	}
	printf("Late admission regressions=%u (8 stress sizes per checkpoint; aggregate model only)\n", regressions);
	fflush(stdout);
	if(!count_only) assert(!regressions);
	/* THE GATE, re-derived 2026-09-08 (lane/dl-ladder-pools).
	 *
	 * It used to be RELATIVE: worst_largest -- the worst per-checkpoint
	 * (baseline largest - lever largest) -- had to stay under 1 MB, i.e.
	 * "turning compaction ON must not cost more than 1 MB of contiguity than
	 * leaving it OFF". That reads as a lever check but is really a DIFFERENCE
	 * between two moving arms, so a change that improves the OFF arm more than
	 * the ON arm trips it while making both better. The ladder is exactly such
	 * a change (OFF-arm min largest 9,080 -> 103,088 B, ON-arm 2,696,232 ->
	 * 3,109,248 B: both up, the gap wider), and raising the bound to let that
	 * through would have thrown away the only thing the number was guarding.
	 *
	 * So each arm is now floored ABSOLUTELY, against a measurement rather than
	 * against its sibling. The pre-ladder column below was measured on this
	 * same build with displaylist_rung/_rung_grow neutered to ceil32 -- i.e.
	 * the sizing this fixture was originally calibrated against:
	 *
	 *                            pre-ladder      ladder      floor
	 *   OFF min largest free          9,080     103,088     98,000
	 *   OFF min total free        1,496,816   2,035,048  1,900,000
	 *   OFF carve spills                325           0          0
	 *   ON  min largest free      2,696,232   3,109,248  2,696,232
	 *
	 * WHY THESE. The two OFF-arm floors and the spill floor guard the ladder
	 * WIN -- the shipping configuration is compaction OFF, so if a later change
	 * gives that arm back to fragmentation the fixture says so immediately, and
	 * they sit ~5% under the measured values so that a harmless perturbation
	 * does not cost a bisect. The ON-arm floor is deliberately NOT discounted:
	 * it is the lever arm own PRE-ladder number, so the lever may never be left
	 * worse than the day it was calibrated, whatever happens to the baseline it
	 * used to be compared against. worst_largest is still computed and printed
	 * (it is a useful shape signal) but no longer gates. */
	if(!count_only && !getenv("CW_CHURN_LOW_HEADROOM")) {
		assert(min_largest[0] >= 98000u);
		assert(min_total[0] >= 1900000u);
		assert(spill[0] == 0);
		assert(min_largest[1] >= 2696232u);
	}
	assert(churn_tail_reuses > (compact ? 100u : 0u));
	printf("PASS shared libogc carve %u checkpoints: largest min %zu -> %zu, free min %zu -> %zu B; worst checkpoint regression largest=%zu total=%zu B; spill %u -> %u, mesh_trunc %u -> %u; tail reuse=%u, full coalescence=%zu B\n",
		(unsigned)CHURN_STEPS, min_largest[0], min_largest[1], min_total[0], min_total[1],
		worst_largest, worst_total, spill[0], spill[1], trunc[0], trunc[1], churn_tail_reuses, CHURN_BYTES - 8);
}
