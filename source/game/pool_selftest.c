/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* colcap-1006 (ASTRA-CC-R5 item 2): see pool_selftest.h. */

#include "pool_selftest.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "../platform/displaylist_scene_hash.h"
#include "../platform/hw_telemetry.h"
#include "../platform/mem2.h"

#ifdef PLATFORM_WII
#include <ogc/lwp_watchdog.h>
static uint64_t st_now_us(void) { return ticks_to_microsecs(gettime()); }
#else
#include <time.h>
static uint64_t st_now_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
#endif

#define ST_OBJ MEM2_POOL_COLUMN_OBJECT
#define ST_WORDS (ST_OBJ / 4u)
#define ST_SLOTS MEM2_POOL_COLUMN_SLOTS
#define ST_BOOT MEM2_POOL_COLUMN_BOOT_SLABS
#define ST_SLAB MEM2_POOL_SLAB_SLOTS
#define ST_OCC_CAP 16u
#define ST_CHURN_OPS 3000u
#define ST_VERIFY_EVERY 256u
#define ST_LIVE_MAX (7u * ST_SLAB) /* churn ceiling: 7 slabs */
#define ST_LIVE_MIN 96u
#define ST_BALLAST_KEEP 48u
#define ST_SETTLE_FRAMES 120u
#define ST_SETTLE_MIN_MS 30000u
#define ST_HASH_FRAMES 3u
#define ST_HASH_ATTEMPTS 5u
#define ST_DEADLINE_MS 170000u
_Static_assert(ST_OBJ % 4u == 0, "column object is whole words");

struct st_obj {
	uint32_t* p;
	uint32_t id;
};

static bool (*s_emit)(const char*) = hw_telemetry_emit_observer_retry;
static struct st_obj s_live[ST_SLOTS];
static unsigned s_nlive;
static uint32_t s_next_id, s_rng;
static bool s_rotating;

/* churn tallies */
static struct {
	uint32_t ops, allocs, frees, lowest_checks, lowest_bad, pattern_checks,
		pattern_bad, owner_bad, addr_bad, used_bad, slabs_peak, refused,
		alloc_n, free_n;
	uint64_t alloc_us, free_us;
	uint32_t alloc_us_max, free_us_max;
	char reason[96];
} s_c;

/* frame driver */
static unsigned s_settle, s_hashed, s_attempts;
static bool s_hashing, s_done;
static float s_cam[5];
static struct dl_scene_frame s_frames[ST_HASH_FRAMES];
static const char* s_status;
static char s_status_buf[48];

void pool_selftest_set_emit(bool (*emit)(const char* line)) {
	s_emit = emit ? emit : hw_telemetry_emit_observer_retry;
}

void pool_selftest_reset(void) {
	memset(s_live, 0, sizeof s_live);
	s_nlive = 0;
	s_next_id = 1;
	s_rng = 0x2545f491u;
	memset(&s_c, 0, sizeof s_c);
	s_settle = s_hashed = s_attempts = 0;
	s_hashing = s_done = false;
	s_status = NULL;
}

static uint32_t st_rand(void) {
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static void st_fail(const char* what) {
	if(!s_c.reason[0])
		snprintf(s_c.reason, sizeof s_c.reason, "%s", what);
}

static uint32_t st_word(uint32_t id, uint32_t i) {
	return (id * 2654435761u) ^ (i * 0x9e3779b9u) ^ 0x5a5aa5a5u;
}

static void st_fill(struct st_obj* o) {
	for(uint32_t i = 0; i < ST_WORDS; i++)
		o->p[i] = st_word(o->id, i);
}

static bool st_check(const struct st_obj* o) {
	s_c.pattern_checks++;
	for(uint32_t i = 0; i < ST_WORDS; i++)
		if(o->p[i] != st_word(o->id, i)) {
			s_c.pattern_bad++;
			st_fail("object bytes changed while live (overlap/overrun/move)");
			return false;
		}
	return true;
}

static unsigned st_stats(struct mem2_pool_stat* st, uint8_t* occ) {
	return mem2_pool_selftest_stats(st, occ, ST_OCC_CAP);
}

static bool st_alloc(void) {
	if(s_nlive >= ST_SLOTS)
		return false;
	struct mem2_pool_stat st;
	uint8_t occ[ST_OCC_CAP];
	unsigned n = st_stats(&st, occ);
	unsigned expect = st.slabs;
	for(unsigned i = 0; i < n; i++)
		if(occ[i] < ST_SLAB) {
			expect = i;
			break;
		}
	uint64_t t0 = st_now_us();
	uint32_t* p = mem2_alloc_pooled(ST_OBJ, MEM2_POOL_COLUMN);
	uint32_t dt = (uint32_t)(st_now_us() - t0);
	s_c.alloc_us += dt;
	s_c.alloc_n++;
	if(dt > s_c.alloc_us_max)
		s_c.alloc_us_max = dt;
	s_c.allocs++;
	s_c.ops++;
	int slab = -1, slot = -1;
	if(!p || !mem2_pool_column_locate(p, &slab, &slot)) {
		s_c.owner_bad++;
		st_fail(p ? "allocation not owned by the column pool"
				  : "column allocation failed");
		if(p)
			mem2_free(p);
		return false;
	}
	if(((uintptr_t)p & 31u) != 0) {
		s_c.addr_bad++;
		st_fail("column slot not 32-byte aligned");
	}
	for(unsigned k = 0; k < s_nlive; k++)
		if(s_live[k].p == p) {
			s_c.addr_bad++;
			st_fail("a live slot was handed out twice");
		}
	if(!s_rotating) {
		s_c.lowest_checks++;
		if((unsigned)slab != expect) {
			s_c.lowest_bad++;
			st_fail("lowest-first placement violated");
		}
	}
	struct st_obj* o = &s_live[s_nlive++];
	o->p = p;
	o->id = s_next_id++;
	st_fill(o);
	st_stats(&st, occ);
	if(st.used != s_nlive) {
		s_c.used_bad++;
		st_fail("pool used != live objects");
	}
	if(st.slabs > s_c.slabs_peak)
		s_c.slabs_peak = st.slabs;
	return true;
}

static void st_free_at(unsigned k) {
	struct st_obj o = s_live[k];
	st_check(&o);
	uint64_t t0 = st_now_us();
	mem2_free_sized(o.p, ST_OBJ); /* the canary-verifying free */
	uint32_t dt = (uint32_t)(st_now_us() - t0);
	s_c.free_us += dt;
	s_c.free_n++;
	if(dt > s_c.free_us_max)
		s_c.free_us_max = dt;
	s_c.frees++;
	s_c.ops++;
	s_live[k] = s_live[--s_nlive];
	if(mem2_pool_column_locate(o.p, NULL, NULL)) {
		s_c.owner_bad++;
		st_fail("freed slot still owned");
	}
	struct mem2_pool_stat st;
	uint8_t occ[ST_OCC_CAP];
	st_stats(&st, occ);
	if(st.used != s_nlive) {
		s_c.used_bad++;
		st_fail("pool used != live objects");
	}
}

static int st_slab_of(unsigned k) {
	int slab = -1;
	return mem2_pool_column_locate(s_live[k].p, &slab, NULL) ? slab : -1;
}

/* Free every live object whose slab index is >= `from` (highest first). */
static void st_free_slabs_from(int from) {
	for(unsigned k = s_nlive; k-- > 0;)
		if(st_slab_of(k) >= from)
			st_free_at(k);
}

/* Free boot-slab objects until at most `keep` live remain. */
static void st_free_down_to(unsigned keep) {
	while(s_nlive > keep)
		st_free_at(st_rand() % s_nlive);
}

/* Free boot-slab objects (random) until `keep` remain in the boot slabs. */
static void st_free_boot_down_to(unsigned keep) {
	for(;;) {
		unsigned in_boot = 0;
		for(unsigned k = 0; k < s_nlive; k++) {
			int slab = st_slab_of(k);
			if(slab >= 0 && slab < (int)ST_BOOT)
				in_boot++;
		}
		if(in_boot <= keep || s_c.reason[0])
			return;
		unsigned start = st_rand() % s_nlive;
		for(unsigned n = 0; n < s_nlive; n++) {
			unsigned k = (start + n) % s_nlive;
			int slab = st_slab_of(k);
			if(slab >= 0 && slab < (int)ST_BOOT) {
				st_free_at(k);
				break;
			}
		}
	}
}

static void st_verify_all(void) {
	for(unsigned k = 0; k < s_nlive; k++)
		st_check(&s_live[k]);
}

static bool st_emit_line(const char* line) {
	return s_emit && s_emit(line);
}

bool pool_selftest_boot(bool rotating, char* reason, size_t reason_size) {
	pool_selftest_reset();
	s_rotating = rotating;
	const char* placement = rotating ? "rotating" : "lowest";
	struct mem2_pool_stat st0, st;
	uint8_t occ[ST_OCC_CAP];
	bool armed = mem2_pool_selftest_arm(rotating ? MEM2_POOL_PLACE_ROTATING
												 : MEM2_POOL_PLACE_LOWEST);
	st_stats(&st0, occ);
	const uint32_t canary0 = mem2_canary_hits();
	const size_t carve0 = mem2_carve_free_bytes();
	uint32_t grows = 0, shrinks = 0, shrink_fail = 0;
	bool refusal_ok = false, kept_slab_used = false;
	if(!armed)
		st_fail("no column pool to drive (selftest not armed)");
	else if(st0.slabs != ST_BOOT || st0.used != 0)
		st_fail("column pool not idle at boot (expected 4 empty boot slabs)");
	else {
		/* 1. grow to 7 slabs (3 demand carves on a virgin heap) */
		while(s_nlive < ST_LIVE_MAX && !s_c.reason[0])
			st_alloc();
		st_verify_all();
		/* 2. random churn inside [96, 224] live */
		for(uint32_t i = 0; i < ST_CHURN_OPS && !s_c.reason[0]; i++) {
			bool alloc = s_nlive <= ST_LIVE_MIN
				|| (s_nlive < ST_LIVE_MAX && (st_rand() & 1u));
			if(alloc)
				st_alloc();
			else
				st_free_at(st_rand() % s_nlive);
			if(i % ST_VERIFY_EVERY == ST_VERIFY_EVERY - 1)
				st_verify_all();
		}
		/* 3. drain: empty every non-boot slab, then thin the boot slabs to
		 * 100 live, so each hand-back has its 16-slot headroom. */
		st_free_slabs_from((int)ST_BOOT);
		st_free_down_to(100);
		st_free_slabs_from((int)ST_BOOT);
		st_stats(&st, occ);
		if(!s_c.reason[0] && st.slabs != ST_BOOT)
			st_fail("drained pool did not return to the 4 boot slabs");
		/* 4. forced heap-return refusal: regrow ONE slab (boot full + 4),
		 * thin the boot slabs to 108 so the hand-back has exactly its
		 * headroom, refuse it, and prove the kept slab is still served
		 * (no new carve) before letting it go. */
		while(!s_c.reason[0] && s_nlive < ST_BOOT * ST_SLAB + 4u)
			st_alloc();
		st_free_boot_down_to(108u);
		struct mem2_pool_stat before;
		st_stats(&before, occ);
		mem2_pool_selftest_refuse_handbacks(1);
		st_free_slabs_from((int)ST_BOOT); /* empties slab 4: refused */
		st_stats(&st, occ);
		refusal_ok = before.slabs == ST_BOOT + 1
			&& st.shrink_fail == before.shrink_fail + 1
			&& st.slabs == ST_BOOT + 1 && st.shrinks == before.shrinks
			&& mem2_pool_selftest_refusals_pending() == 0;
		if(!s_c.reason[0] && !refusal_ok)
			st_fail("forced hand-back refusal did not keep the slab");
		/* the kept slab must still serve, without a new carve */
		while(!s_c.reason[0] && s_nlive < ST_BOOT * ST_SLAB + 1u)
			st_alloc();
		for(unsigned k = 0; k < s_nlive; k++)
			if(st_slab_of(k) == (int)ST_BOOT)
				kept_slab_used = true;
		st_stats(&st, occ);
		if(!s_c.reason[0] && (!kept_slab_used || st.slabs != ST_BOOT + 1))
			st_fail("refused (kept) slab was not reused");
		/* now the hand-back succeeds (the refusal was consumed) */
		struct mem2_pool_stat kept;
		st_stats(&kept, occ);
		st_free_slabs_from((int)ST_BOOT);
		st_free_down_to(100);
		st_stats(&st, occ);
		if(!s_c.reason[0] && (st.slabs != ST_BOOT || st.shrinks != kept.shrinks + 1
							  || st.shrink_fail != kept.shrink_fail))
			st_fail("kept slab never handed back after the refusal");
		/* 5. everything freed */
		st_verify_all();
		st_free_down_to(0);
		st_stats(&st, occ);
		if(!s_c.reason[0] && (st.used != 0 || st.slabs != ST_BOOT))
			st_fail("pool not back to 4 empty boot slabs");
		shrinks = st.shrinks - st0.shrinks;
		shrink_fail = st.shrink_fail - st0.shrink_fail;
		grows = (st.slabs + shrinks) - st0.slabs; /* net carves */
		/* 6. ballast: fill the boot slabs, keep a scattered 48 */
		while(!s_c.reason[0] && s_nlive < ST_BOOT * ST_SLAB)
			st_alloc();
		for(unsigned k = s_nlive; k-- > 0;) {
			if(s_nlive <= ST_BALLAST_KEEP)
				break;
			uint32_t h = s_live[k].id * 2654435761u;
			if((h >> 29) >= 3u) /* keep ~3/8, thin the rest */
				st_free_at(k);
		}
		st_free_down_to(ST_BALLAST_KEEP);
		st_verify_all();
	}
	const uint32_t canary = mem2_canary_hits() - canary0;
	const uint32_t diag = mem2_diag_bad();
	size_t largest = 0;
	const bool heap_valid = mem2_carve_largest_allocatable_bytes(&largest)
#ifndef PLATFORM_WII
		|| true /* PC: no carve heap to validate */
#endif
		;
	if(canary)
		st_fail("canary (guard) hits during churn");
	if(diag)
		st_fail("heap validator flagged the carve");
	if(!heap_valid)
		st_fail("carve heap walk invalid");
	const size_t carve1 = mem2_carve_free_bytes();
	/* The game's own column takes start here. */
	if(armed)
		(void)mem2_pool_selftest_arm(rotating ? MEM2_POOL_PLACE_ROTATING
											  : MEM2_POOL_PLACE_LOWEST);
	char line[1024];
	snprintf(line, sizeof line,
		"{\"event\":\"pool_selftest\",\"phase\":\"churn\",\"ok\":%s,"
		"\"reason\":\"%s\",\"placement\":\"%s\",\"ops\":%" PRIu32 ","
		"\"allocs\":%" PRIu32 ",\"frees\":%" PRIu32 ",\"grows\":%" PRIu32 ","
		"\"shrinks\":%" PRIu32 ",\"shrink_fail\":%" PRIu32 ","
		"\"refusal_kept_slab\":%s,\"kept_slab_reused\":%s,"
		"\"slabs_start\":%" PRIu32 ",\"slabs_peak\":%" PRIu32 ","
		"\"lowest_checks\":%" PRIu32 ",\"lowest_bad\":%" PRIu32 ","
		"\"pattern_checks\":%" PRIu32 ",\"pattern_bad\":%" PRIu32 ","
		"\"owner_bad\":%" PRIu32 ",\"addr_bad\":%" PRIu32 ",\"used_bad\":%" PRIu32 ","
		"\"canary_delta\":%" PRIu32 ",\"diag_bad\":%" PRIu32 ",\"heap_valid\":%s,"
		"\"carve_free_before\":%lu,\"carve_free_after\":%lu,"
		"\"alloc_n\":%" PRIu32 ",\"alloc_us\":%" PRIu32 ",\"alloc_us_max\":%" PRIu32 ","
		"\"free_n\":%" PRIu32 ",\"free_us\":%" PRIu32 ",\"free_us_max\":%" PRIu32 ","
		"\"ballast\":%u}",
		s_c.reason[0] ? "false" : "true", s_c.reason, placement, s_c.ops,
		s_c.allocs, s_c.frees, grows, shrinks, shrink_fail,
		refusal_ok ? "true" : "false", kept_slab_used ? "true" : "false",
		st0.slabs, s_c.slabs_peak, s_c.lowest_checks, s_c.lowest_bad,
		s_c.pattern_checks, s_c.pattern_bad, s_c.owner_bad, s_c.addr_bad,
		s_c.used_bad, canary, diag, heap_valid ? "true" : "false",
		(unsigned long)(carve0 == (size_t)-1 ? 0 : carve0),
		(unsigned long)(carve1 == (size_t)-1 ? 0 : carve1),
		s_c.alloc_n, (uint32_t)s_c.alloc_us, s_c.alloc_us_max,
		s_c.free_n, (uint32_t)s_c.free_us, s_c.free_us_max, s_nlive);
	(void)st_emit_line(line);
	if(reason && reason_size)
		snprintf(reason, reason_size, "%s", s_c.reason);
	if(s_c.reason[0]) {
		snprintf(s_status_buf, sizeof s_status_buf, "pool_churn_failed");
		s_status = s_status_buf;
		s_done = true;
	}
	return !s_c.reason[0];
}

static bool st_frames_equal(const struct dl_scene_frame* a,
							const struct dl_scene_frame* b) {
	return a->gx_ordered == b->gx_ordered && a->gx_set == b->gx_set
		&& a->pos_ordered == b->pos_ordered
		&& a->content_ordered == b->content_ordered
		&& a->content_set == b->content_set && a->lists == b->lists
		&& a->bytes == b->bytes && a->chunks == b->chunks
		&& a->content_chunks == b->content_chunks;
}

static void st_finish(const char* status) {
	/* The ballast held its bytes through the whole run (neighbouring slots
	 * churned by the real game), then goes back through the canary free. */
	uint32_t canary0 = mem2_canary_hits();
	uint32_t bad0 = s_c.pattern_bad;
	unsigned ballast = s_nlive;
	st_verify_all();
	uint32_t ballast_bad = s_c.pattern_bad - bad0;
	st_free_down_to(0);
	uint32_t canary = mem2_canary_hits() - canary0;
	uint32_t diag = mem2_diag_bad();
	uint32_t takes = 0;
	uint32_t digest = mem2_pool_selftest_placement_digest(&takes);
	bool ok = !strcmp(status, "ok") && !ballast_bad && !canary && !diag;
	char line[512];
	snprintf(line, sizeof line,
		"{\"event\":\"pool_selftest\",\"phase\":\"final\",\"ok\":%s,"
		"\"status\":\"%s\",\"placement\":\"%s\",\"ballast\":%u,"
		"\"ballast_bad\":%" PRIu32 ",\"canary_delta\":%" PRIu32 ","
		"\"diag_bad\":%" PRIu32 ",\"pdigest\":\"%08" PRIx32 "\","
		"\"ptakes\":%" PRIu32 "}",
		ok ? "true" : "false", status, s_rotating ? "rotating" : "lowest",
		ballast, ballast_bad, canary, diag, digest, takes);
	(void)st_emit_line(line);
	snprintf(s_status_buf, sizeof s_status_buf, "%s",
			 ok ? "ok" : (strcmp(status, "ok") ? status : "ballast_or_guard_failed"));
	s_status = s_status_buf;
	s_done = true;
}

static void st_emit_scene(bool stable) {
	const struct dl_scene_frame* f = &s_frames[0];
	uint32_t takes = 0;
	uint32_t digest = mem2_pool_selftest_placement_digest(&takes);
	char line[768];
#define ST_H(v) (uint32_t)((v) >> 32), (uint32_t)(v)
	snprintf(line, sizeof line,
		"{\"event\":\"gx_scene_hash\",\"placement\":\"%s\",\"stable\":%s,"
		"\"attempts\":%u,\"frames\":%u,"
		"\"gx\":\"%08" PRIx32 "%08" PRIx32 "\",\"gx_set\":\"%08" PRIx32 "%08" PRIx32 "\","
		"\"pos\":\"%08" PRIx32 "%08" PRIx32 "\","
		"\"content\":\"%08" PRIx32 "%08" PRIx32 "\","
		"\"content_set\":\"%08" PRIx32 "%08" PRIx32 "\","
		"\"lists\":%" PRIu32 ",\"bytes\":%" PRIu32 ",\"chunks\":%" PRIu32 ","
		"\"content_chunks\":%" PRIu32 ",\"pdigest\":\"%08" PRIx32 "\","
		"\"ptakes\":%" PRIu32 ",\"cam\":[%.4f,%.4f,%.4f,%.4f,%.4f]}",
		s_rotating ? "rotating" : "lowest", stable ? "true" : "false",
		s_attempts, (unsigned)ST_HASH_FRAMES, ST_H(f->gx_ordered),
		ST_H(f->gx_set), ST_H(f->pos_ordered), ST_H(f->content_ordered),
		ST_H(f->content_set), f->lists, (uint32_t)f->bytes, f->chunks,
		f->content_chunks, digest, takes, (double)s_cam[0], (double)s_cam[1],
		(double)s_cam[2], (double)s_cam[3], (double)s_cam[4]);
#undef ST_H
	(void)st_emit_line(line);
}

/* R5 tapes: both placements hold about 60 fps before the final self-test
 * work, but keep remeshing while stationary. Requiring the mesher to be
 * idle for 120 consecutive frames was a
 * lottery, not a property of the placement. The window now settles on what
 * must be still -- streamed near ring, camera -- and only MESH-CLEAN frames
 * (nothing pending or in flight at the render boundary) are
 * hashed; three clean frames must agree. A remesh of unchanged content
 * yields identical bytes; a real content change shows in the content
 * witness and makes a cross-run comparison INCONCLUSIVE, never PASS. */
void pool_selftest_frame_begin(bool settled, bool clean, const float cam[5],
							   uint32_t elapsed_ms) {
	if(s_done)
		return;
	if(elapsed_ms >= ST_DEADLINE_MS) {
		st_finish(s_attempts ? "gx_scene_unstable" : "scene_never_settled");
		return;
	}
	/* The camera must hold still for the whole settle window (1 mm / 0.001
	 * degree: physics on solid ground is exact, but never trust a float
	 * equality across frames for a liveness condition). */
	bool same_cam = s_settle != 0;
	for(unsigned k = 0; k < 5 && same_cam; k++)
		same_cam = cam[k] - s_cam[k] < 0.001f && s_cam[k] - cam[k] < 0.001f;
	if(!settled || !same_cam) {
		if(s_hashing) {
			s_hashing = false;
			s_hashed = 0;
		}
		s_settle = settled ? 1u : 0u;
		memcpy(s_cam, cam, sizeof s_cam);
		return;
	}
	if(!s_hashing) {
		if(++s_settle < ST_SETTLE_FRAMES || elapsed_ms < ST_SETTLE_MIN_MS)
			return;
		s_hashing = true;
		s_hashed = 0;
		s_attempts++;
	}
	if(clean)
		dl_scene_hash_frame_begin();
}

void pool_selftest_frame_end(uint32_t elapsed_ms) {
	if(s_done)
		return;
	/* The cap also fires on frames that never reach the world render (a
	 * world that never loads must still end the run with a verdict). */
	if(!g_dl_scene_hash_armed) {
		if(elapsed_ms >= ST_DEADLINE_MS)
			st_finish(s_attempts ? "gx_scene_unstable" : "scene_never_settled");
		return;
	}
	if(!s_hashing)
		return;
	dl_scene_hash_frame_end(&s_frames[s_hashed++]);
	if(s_hashed < ST_HASH_FRAMES)
		return;
	s_hashing = false;
	bool stable = s_frames[0].lists > 0;
	for(unsigned k = 1; k < ST_HASH_FRAMES; k++)
		stable = stable && st_frames_equal(&s_frames[0], &s_frames[k]);
	if(stable) {
		st_emit_scene(true);
		st_finish("ok");
		return;
	}
	if(s_attempts >= ST_HASH_ATTEMPTS) {
		st_emit_scene(false);
		st_finish("gx_scene_unstable");
		return;
	}
	/* still settled: retry at once with fresh clean frames */
	s_hashing = true;
	s_hashed = 0;
	s_attempts++;
}

void* pool_selftest_ballast_ptr(unsigned k) {
	return k < s_nlive ? s_live[k].p : NULL;
}

const char* pool_selftest_status(void) {
	return s_done ? s_status : NULL;
}
