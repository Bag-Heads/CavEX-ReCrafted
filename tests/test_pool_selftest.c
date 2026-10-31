/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * colcap-1006 (ASTRA-CC-R5 item 2): the pool_selftest_3m driver on the PC
 * column-pool emulation (CW_TEST_COLUMN_POOL; the same mem2_pool.h code, take
 * policy, demand growth and hand-back rule as the Wii pool, plus the same
 * self-test hooks), with the REAL ordered scene-hash accumulator
 * (source/platform/displaylist_scene_hash.c). What the Wii run adds is the real MEM2 heap,
 * its canary/validator and Broadway timing; this pins the driver's logic so
 * a hardware slot is never spent on a driver bug.
 *
 *   1. boot churn, lowest placement: ok, zero violations, forced refusal kept
 *      the slab and it was reused, then handed back; 48 ballast left.
 *   2. scene: three identical frames -> gx_scene_hash stable, final ok.
 *   3. rotating placement: ok; a later run of identical takes lands in
 *      different slots (placement digest differs) while the ballast and the
 *      frame hashes are unaffected.
 *   4. unstable frames -> gx_scene_unstable after 5 attempts.
 *   5. a corrupted ballast byte -> final not ok (ballast_or_guard_failed).
 *   6. never settled -> scene_never_settled at the 170 s cap.
 *   7. the accumulator: chunk-only, ordered vs set witnesses behave.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/pool_selftest.h"
#include "platform/mem2.h"
#include "platform/displaylist_scene_hash.h"

/* pool_selftest.c's default sink; the test installs its own. */
bool hw_telemetry_emit_observer_retry(const char* line) {
	(void)line;
	return true;
}

static char s_lines[16][1100];
static unsigned s_nlines;
/* --emit-events: also print every event line (the C formatter's real output)
 * under "#scenario" markers, for tests/test_pool_selftest_collector.py. */
static bool s_dump;
static void scenario(const char* name) {
	if(s_dump)
		printf("#scenario %s\n", name);
}
static bool capture(const char* line) {
	if(s_dump)
		printf("%s\n", line);
	assert(s_nlines < 16);
	snprintf(s_lines[s_nlines++], sizeof s_lines[0], "%s", line);
	return true;
}
static const char* last_event(const char* kind) {
	char key[64];
	snprintf(key, sizeof key, "\"event\":\"%s\"", kind);
	for(unsigned k = s_nlines; k-- > 0;)
		if(strstr(s_lines[k], key))
			return s_lines[k];
	return NULL;
}
static void has(const char* line, const char* frag) {
	if(!line || !strstr(line, frag)) {
		fprintf(stderr, "missing %s in: %s\n", frag, line ? line : "(none)");
		abort();
	}
}

/* One synthetic chunk frame: n chunks, each one list of `bytes` bytes. */
static void render_frame(unsigned n, uint8_t salt) {
	static uint8_t data[512];
	for(unsigned c = 0; c < n; c++) {
		for(unsigned k = 0; k < sizeof data; k++)
			data[k] = (uint8_t)(k * 31u + c * 7u + salt);
		dl_scene_hash_chunk_begin((int32_t)c * 16, 0, 0, false, NULL);
		dl_scene_hash_content((int32_t)c * 16, 0, 0, 0x1234u + c);
		dl_scene_hash_list(data, sizeof data);
		dl_scene_hash_chunk_end();
		/* an entity list outside any chunk bracket is NOT hashed */
		dl_scene_hash_list(data, 17);
	}
}

static const float CAM[5] = {8.5f, 70.0f, 8.5f, 0.0f, 0.0f};

static void run_scene(uint32_t* t_ms, unsigned frames, bool vary) {
	for(unsigned f = 0; f < frames && !pool_selftest_status(); f++) {
		/* every 3rd frame has a remesh in flight: never hashed */
		pool_selftest_frame_begin(true, f % 3 != 1, CAM, *t_ms);
		assert(!g_dl_scene_hash_armed || f % 3 != 1);
		render_frame(5, (vary || f % 3 == 1) ? (uint8_t)f : 0);
		pool_selftest_frame_end(*t_ms);
		*t_ms += 16;
	}
}

static uint32_t digest_after_takes(void) {
	void* p[40];
	for(unsigned k = 0; k < 40; k++)
		assert((p[k] = mem2_alloc_pooled(MEM2_POOL_COLUMN_OBJECT, MEM2_POOL_COLUMN)));
	uint32_t takes = 0, d = mem2_pool_selftest_placement_digest(&takes);
	assert(takes == 40);
	for(unsigned k = 0; k < 40; k++)
		mem2_free(p[k]);
	return d;
}

int main(int argc, char** argv) {
	s_dump = argc > 1 && !strcmp(argv[1], "--emit-events");
#ifdef _WIN32
	_putenv("CW_TEST_COLUMN_POOL=lowest");
#else
	setenv("CW_TEST_COLUMN_POOL", "lowest", 1);
#endif
	pool_selftest_set_emit(capture);

	/* 7. accumulator semantics */
	{
		struct dl_scene_frame a, b, c;
		dl_scene_hash_frame_begin();
		render_frame(3, 0);
		dl_scene_hash_frame_end(&a);
		assert(a.lists == 3 && a.chunks == 3 && a.content_chunks == 3);
		assert(a.bytes == 3 * 512); /* the 17-byte entity lists excluded */
		dl_scene_hash_frame_begin();
		render_frame(3, 0);
		dl_scene_hash_frame_end(&b);
		assert(!memcmp(&a, &b, sizeof a));
		dl_scene_hash_frame_begin(); /* same chunks, reversed order */
		static uint8_t data[512];
		for(int c2 = 2; c2 >= 0; c2--) {
			for(unsigned k = 0; k < sizeof data; k++)
				data[k] = (uint8_t)(k * 31u + (unsigned)c2 * 7u);
			dl_scene_hash_chunk_begin(c2 * 16, 0, 0, false, NULL);
			dl_scene_hash_content(c2 * 16, 0, 0, 0x1234u + (unsigned)c2);
			dl_scene_hash_list(data, sizeof data);
			dl_scene_hash_chunk_end();
		}
		dl_scene_hash_frame_end(&c);
		assert(c.gx_set == a.gx_set && c.content_set == a.content_set);
		assert(c.gx_ordered != a.gx_ordered && c.pos_ordered != a.pos_ordered);
		assert(!g_dl_scene_hash_armed);
		dl_scene_hash_list(data, 9); /* disarmed: ignored */
		printf("PASS accumulator: chunk lists only; set witnesses order-free, "
			   "ordered witnesses order-sensitive\n");
	}

	/* 1 + 2. lowest placement, boot churn then a settled scene */
	scenario("lowest");
	char why[96];
	bool ok = pool_selftest_boot(false, why, sizeof why);
	if(!ok)
		fprintf(stderr, "churn failed: %s\n%s\n", why, last_event("pool_selftest"));
	assert(ok);
	const char* churn = last_event("pool_selftest");
	has(churn, "\"phase\":\"churn\",\"ok\":true");
	has(churn, "\"placement\":\"lowest\"");
	has(churn, "\"shrink_fail\":1,");
	has(churn, "\"refusal_kept_slab\":true,\"kept_slab_reused\":true");
	has(churn, "\"slabs_start\":4,\"slabs_peak\":7");
	has(churn, "\"lowest_bad\":0");
	has(churn, "\"pattern_bad\":0,\"owner_bad\":0,\"addr_bad\":0,\"used_bad\":0");
	has(churn, "\"ballast\":48}");
	assert(!strstr(churn, "\"lowest_checks\":0,"));
	printf("PASS churn (lowest): %s\n", churn);
	/* the "game" takes 40 columns around the ballast */
	uint32_t d_lowest = digest_after_takes();
	uint32_t t = 0;
	/* not settled long enough: nothing hashed before 30 s / 120 frames */
	run_scene(&t, 130, false);
	assert(!pool_selftest_status() && !last_event("gx_scene_hash"));
	t = 30000;
	run_scene(&t, 200, false);
	assert(pool_selftest_status() && !strcmp(pool_selftest_status(), "ok"));
	const char* gx_lowest = last_event("gx_scene_hash");
	has(gx_lowest, "\"stable\":true");
	has(gx_lowest, "\"lists\":5,\"bytes\":2560,\"chunks\":5");
	has(last_event("pool_selftest"), "\"phase\":\"final\",\"ok\":true");
	has(last_event("pool_selftest"), "\"ballast\":48,\"ballast_bad\":0");
	printf("PASS scene (lowest): %s\n", gx_lowest);
	has(gx_lowest, "\"ptakes\":40,");
	char gx_a[64];
	snprintf(gx_a, sizeof gx_a, "%.40s", strstr(gx_lowest, "\"gx\":"));

	/* 3. rotating placement */
	scenario("rotating");
	s_nlines = 0;
	ok = pool_selftest_boot(true, why, sizeof why);
	assert(ok);
	churn = last_event("pool_selftest");
	has(churn, "\"placement\":\"rotating\"");
	has(churn, "\"lowest_checks\":0,");
	has(churn, "\"refusal_kept_slab\":true,\"kept_slab_reused\":true");
	uint32_t d_rot = digest_after_takes();
	assert(d_rot != d_lowest); /* the same 40 takes landed elsewhere */
	t = 30000;
	run_scene(&t, 200, false);
	assert(!strcmp(pool_selftest_status(), "ok"));
	const char* gx_rot = last_event("gx_scene_hash");
	has(gx_rot, gx_a); /* identical ordered hash under the other placement */
	printf("PASS rotating: digest %08x != %08x, scene hash identical\n",
		   (unsigned)d_rot, (unsigned)d_lowest);

	/* 4. unstable frames */
	scenario("unstable");
	s_nlines = 0;
	assert(pool_selftest_boot(false, why, sizeof why));
	t = 30000;
	run_scene(&t, 2000, true);
	assert(!strcmp(pool_selftest_status(), "gx_scene_unstable"));
	has(last_event("gx_scene_hash"), "\"stable\":false,\"attempts\":5");
	has(last_event("pool_selftest"), "\"phase\":\"final\",\"ok\":false");
	printf("PASS unstable frames -> gx_scene_unstable after 5 attempts\n");

	/* 5. ballast corruption is caught at the end */
	scenario("ballast");
	s_nlines = 0;
	assert(pool_selftest_boot(false, why, sizeof why));
	uint8_t* victim = pool_selftest_ballast_ptr(17);
	assert(victim && !pool_selftest_ballast_ptr(48));
	victim[MEM2_POOL_COLUMN_OBJECT - 1] ^= 0x40u;
	t = 30000;
	run_scene(&t, 200, false);
	assert(!strcmp(pool_selftest_status(), "ballast_or_guard_failed"));
	has(last_event("pool_selftest"), "\"ballast_bad\":1");
	printf("PASS corrupted ballast byte -> ballast_or_guard_failed\n");

	/* 6. never settled */
	scenario("unsettled");
	s_nlines = 0;
	assert(pool_selftest_boot(false, why, sizeof why));
	pool_selftest_frame_begin(false, false, CAM, 1000);
	pool_selftest_frame_end(1000);
	assert(!pool_selftest_status());
	pool_selftest_frame_end(170000); /* no world render needed for the cap */
	assert(!strcmp(pool_selftest_status(), "scene_never_settled"));
	printf("PASS never settled -> scene_never_settled at the cap\n");

	/* A streamed, stationary scene can settle without ever being clean;
	 * that is insufficient for certification. A dirty frame must not arm
	 * the hash or contribute even if its bytes happen to match. */
	scenario("never_clean");
	s_nlines = 0;
	assert(pool_selftest_boot(false, why, sizeof why));
	for(unsigned f = 0; f < 200; f++) {
		pool_selftest_frame_begin(true, false, CAM, 30000 + f * 16);
		assert(!g_dl_scene_hash_armed);
		render_frame(5, 0);
		pool_selftest_frame_end(30000 + f * 16);
	}
	assert(!last_event("gx_scene_hash"));
	pool_selftest_frame_end(170000);
	assert(!strcmp(pool_selftest_status(), "gx_scene_unstable"));
	printf("PASS streamed but never clean cannot certify\n");

	/* Changing the camera or losing streamed coverage resets an in-progress
	 * sample, including its already collected hashes. */
	scenario("settle_reset");
	s_nlines = 0;
	assert(pool_selftest_boot(false, why, sizeof why));
	t = 30000;
	for(unsigned f = 0; f < 120; f++) {
		pool_selftest_frame_begin(true, true, CAM, t++);
		render_frame(5, 0);
		pool_selftest_frame_end(t);
	}
	assert(!pool_selftest_status()); /* exactly one hash so far */
	float moved[5];
	memcpy(moved, CAM, sizeof moved);
	moved[0] += 1;
	pool_selftest_frame_begin(true, true, moved, t++);
	assert(!g_dl_scene_hash_armed);
	pool_selftest_frame_end(t);
	for(unsigned f = 0; f < 125; f++) {
		pool_selftest_frame_begin(f % 100 != 0, true, moved, t++);
		assert(!g_dl_scene_hash_armed);
		pool_selftest_frame_end(t);
	}
	run_scene(&t, 200, false);
	assert(pool_selftest_status() && !strcmp(pool_selftest_status(), "ok"));
	printf("PASS camera/coverage change resets incomplete hash window\n");

	/* the pool is back to its 4 empty boot slabs */
	struct mem2_pool_stat st;
	uint8_t occ[16];
	mem2_pool_selftest_stats(&st, occ, 16);
	assert(st.used == 0 && st.slabs == MEM2_POOL_COLUMN_BOOT_SLABS);
	printf("PASS test_pool_selftest\n");
	return 0;
}
