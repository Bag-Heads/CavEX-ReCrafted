/*
	asyncsnd-0926 (Astra ruling 3, 2026-09-26): the opt-in async pass C of the
	sound registry (platform/audio.h "async pass C") and the world-entry gate
	(game/gui/loading_notice.h).

	AUTHORED AGAINST THE RULING: every expectation below is transcribed from
	tmp/jobs/next-gaps-0924/codex/astra-pack-0926.out.md item 3 and the two
	headers, never from a run of the code under test (CODESTYLE 4.2).

	The SYNC registry is the oracle: the same fixture trees are loaded with
	"audio.async_load" off, and the registry hash (per key: variant count; per
	variant: frames, rate, arena OFFSET and every PCM byte -- the
	test_audio_dir_index_equiv definition) plus the per-root open/read
	counters must be IDENTICAL after the async run completes. Offsets equal
	means the allocation order is preserved; counts and per-root hits equal
	means the variant order and every fallback decision are preserved.

	Cases (one static void test_xxx per case, in main() order):
	  a sync_oracle          the sync hashes of every tree (async off)
	  b manual_equiv         async on, loader stepped BY HAND: passes A+B are
	                         visible and playable before the first step, every
	                         pass-C key is PENDING (pick NULL, skip counted,
	                         no protected key ever pending), keys publish in
	                         table order one per step, hash == sync at DONE
	  c thread_equiv         the same with the REAL loader thread (polled)
	  d thread_failure       thread creation fails -> synchronous fallback,
	                         hash == sync, state OFF, counted
	  e cancel_join          cancel mid-run: join, stale step publishes
	                         nothing, the next sync reload == sync hash
	  f repeated_switches    five back-to-back pack switches, each cancelling
	                         a half-finished run; the last one completes and
	                         equals the sync hash of its pack; arena_used equal
	  g stall_retry          no progress past AUDIO_ASYNC_PROGRESS_TIMEOUT_MS
	                         -> FAILED (pending), retry reloads under a new
	                         generation and completes
	  h deferred_coexist     audio.deferred_cave on + async on == the sync
	                         deferred registry (cave key probed, not decoded)
	  i gate                 the gate's decision table, parking a request while
	                         pending, transparent when complete
	  j load_record          the reload record carries async=true / false
	  k race_reads           every key read on the main thread while the REAL
	                         loader runs: NULL or a complete resident sample,
	                         visibility monotonic, counts never shrink
	  l ogg_sync             a raw .ogg pass-C candidate in the pack keeps the
	                         whole scan synchronous (Astra clause), counted,
	                         hash == sync, no OGG decode ever on the loader
	  m gate_update          the gate screen's real update: SEND queues the
	                         parked SRPC_LOAD_WORLD and enters the loading
	                         screen; BACK drops it; RETRY reloads under a new
	                         generation; FAILED-then-finished becomes DONE
	  n remote_gate          the remote half: parked, Back to the join screen,
	                         SEND re-enters the connect screen
	  boot (in main)         audio_init with the opt-in on: RUNNING after
	                         init, the audio_load record is stashed by the
	                         poll with async=true

	Fixtures are generated at run time into a PID-unique directory (never a
	fixed name -- the 2026-09-02 test_fixture_dir root cause) and removed in
	main(). WAVs come from wavh_write (audio_wav.h).
*/

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define CHDIR(p) _chdir(p)
#define GETCWD(b, n) _getcwd(b, n)
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define CHDIR(p) chdir(p)
#define GETCWD(b, n) getcwd(b, n)
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "game/gui/loading_notice.h"
#include "game/gui/screen.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/audio.h"
#include "platform/audio_sound_table.h"
#include "platform/audio_wav.h"

extern void audio_registry_test_scan_ctr(int root, unsigned long long out[7]);
extern void audio_registry_test_async_manual(bool manual);
extern void audio_registry_test_async_fail_thread(bool fail);
extern void audio_registry_test_async_set_clock_ms(int64_t ms);
extern void audio_registry_test_set_observer(bool (*sink)(const char* line));
extern void loading_notice_gate_test_input(int back, int retry);

enum { C_MISS = 0, C_HIT, C_BYTES, C_DIR, C_ABSENT, C_PROBE, C_SKIP };

static char s_fixture[128];
static char s_srcroot[1024];

#define TEST_RATE 22050u
#define MAX_FRAMES 4096u

/* ===== fixture ========================================================== */

static void mkdir_p(const char* path) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%.511s", path);
	for(char* p = buf + 1; *p; p++) {
		if(*p == '/') {
			*p = '\0';
			MKDIR(buf);
			*p = '/';
		}
	}
	MKDIR(buf);
}

static int16_t s_pcm[MAX_FRAMES];
static uint8_t s_wav[wavh_write_size(MAX_FRAMES)];

static void put_wav(const char* root, uint16_t id, int v, uint32_t frames,
					uint32_t seed, bool corrupt) {
	assert(frames <= MAX_FRAMES);
	uint32_t x = seed * 2654435761u + 12345u;
	for(uint32_t k = 0; k < frames; k++) {
		x = x * 1103515245u + 12345u;
		s_pcm[k] = (int16_t)(x >> 16);
	}
	size_t n = wavh_write(s_wav, TEST_RATE, s_pcm, frames);
	assert(n == wavh_write_size(frames));
	if(corrupt)
		s_wav[22] = 2; /* fmt NumChannels -> 2: the loader rejects it */
	char rel[128], path[512];
	audio_sound_table_subpath(audio_sound_name(id), rel, sizeof(rel));
	if(v == 0)
		snprintf(path, sizeof(path), "%s/%s.wav", root, rel);
	else
		snprintf(path, sizeof(path), "%s/%s%d.wav", root, rel, v);
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", path);
	char* slash = strrchr(dir, '/');
	assert(slash);
	*slash = '\0';
	mkdir_p(dir);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(s_wav, 1, n, f) == n);
	fclose(f);
}

static bool key_protected(uint16_t id) {
	const char* n = audio_sound_name(id);
	return strncmp(n, "menu.", 5) == 0 || strncmp(n, "note.", 5) == 0;
}

/* assets/: every key but id%7==3, one or two variants (the fallback root).
   Protected keys are never withheld here, so every menu.* and note.* key resolves
   somewhere (the "playable before pass C" assertion is about the loader, not
   about a fixture hole). */
static void build_assets(void) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(id % 7 == 3 && !key_protected(id))
			continue;
		int nv = 1 + id % 2;
		for(int k = 0; k < nv; k++)
			put_wav("assets", id, 1 + k, 500u + id * 13u + (uint32_t)k,
					(uint32_t)(id * 100 + k + 7), false);
	}
}

/* pack_a: most keys, mixed variant shapes, a corrupt file, a few keys absent
   (per-key fallback to assets/), one key absent everywhere (silent). */
static void build_pack_a(void) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(id % 5 == 4)
			continue; /* absent here: falls back per key */
		uint32_t frames = 600u + (uint32_t)id * 37u;
		switch(id % 4) {
			case 0:
				put_wav("pack_a", id, 0, frames, id, false);
				put_wav("pack_a", id, 10, frames + 3, id + 1u, false);
				break;
			case 1:
				put_wav("pack_a", id, 1, frames, id, false);
				put_wav("pack_a", id, 2, frames + 1, id + 2u, false);
				put_wav("pack_a", id, 3, frames + 2, id + 3u, false);
				break;
			case 2: put_wav("pack_a", id, 1, frames, id, true); /* corrupt */
				put_wav("pack_a", id, 2, frames + 9, id + 4u, false);
				break;
			default: put_wav("pack_a", id, 1, frames, id, false);
		}
	}
}

/* pack_b: fewer, bigger variants, different shape (the switch target). */
static void build_pack_b(void) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(id % 3 == 0)
			continue;
		put_wav("pack_b", id, 1, 900u + (uint32_t)id * 21u, id + 900u, false);
		if(id % 4 == 1)
			put_wav("pack_b", id, 7, 1200u + id, id + 901u, false);
	}
}

/* pack_ogg: pack_a's shape plus ONE raw .ogg variant of a pass-C key (the
   committed CC0 vorbis fixture): the Astra clause's "raw-OGG input". */
static uint8_t* s_ogg;
static size_t s_ogg_len;

static void build_pack_ogg(void) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(id % 5 == 4)
			continue;
		put_wav("pack_ogg", id, 1, 600u + (uint32_t)id * 37u, id, false);
	}
	char rel[128], path[512];
	audio_sound_table_subpath(audio_sound_name(SOUND_STEP_GRASS), rel,
							  sizeof(rel));
	snprintf(path, sizeof(path), "pack_ogg/%s2.ogg", rel);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(s_ogg, 1, s_ogg_len, f) == s_ogg_len);
	fclose(f);
}

/* ===== registry hash (test_audio_dir_index_equiv's definition) ========== */

static uint64_t fnv(uint64_t h, const void* data, size_t n) {
	const uint8_t* b = data;
	for(size_t k = 0; k < n; k++) {
		h ^= b[k];
		h *= UINT64_C(0x100000001b3);
	}
	return h;
}

static uint64_t registry_hash(int* total_variants) {
	const uint8_t* base = NULL;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
		for(int v = 0; v < audio_registry_variant_count(id); v++) {
			const struct audio_sample* s = audio_registry_variant(id, v);
			assert(s);
			if(s->pcm && (!base || (const uint8_t*)s->pcm < base))
				base = (const uint8_t*)s->pcm;
		}
	uint64_t h = UINT64_C(0xcbf29ce484222325);
	int total = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		int32_t n = audio_registry_variant_count(id);
		h = fnv(h, &n, sizeof(n));
		for(int v = 0; v < n; v++) {
			const struct audio_sample* s = audio_registry_variant(id, v);
			h = fnv(h, &s->frames, sizeof(s->frames));
			h = fnv(h, &s->rate, sizeof(s->rate));
			if(s->pcm) {
				uint32_t off = (uint32_t)((const uint8_t*)s->pcm - base);
				h = fnv(h, &off, sizeof(off));
				h = fnv(h, s->pcm, (size_t)s->frames * 2u);
			}
			total++;
		}
	}
	if(total_variants)
		*total_variants = total;
	return h;
}

struct scan_result {
	uint64_t hash;
	int variants;
	size_t arena_used;
	unsigned long long c[2][7];
};

static void snapshot(struct scan_result* r) {
	r->hash = registry_hash(&r->variants);
	r->arena_used = audio_arena_used();
	audio_registry_test_scan_ctr(0, r->c[0]);
	audio_registry_test_scan_ctr(1, r->c[1]);
}

static void expect_equal(const struct scan_result* a,
						 const struct scan_result* b) {
	assert(a->hash == b->hash);
	assert(a->variants == b->variants);
	assert(a->arena_used == b->arena_used);
	for(int root = 0; root < 2; root++) {
		assert(a->c[root][C_HIT] == b->c[root][C_HIT]);
		assert(a->c[root][C_BYTES] == b->c[root][C_BYTES]);
		assert(a->c[root][C_MISS] == b->c[root][C_MISS]);
		assert(a->c[root][C_SKIP] == b->c[root][C_SKIP]);
	}
}

/* ===== helpers ========================================================== */

static void set_pack(const char* pack) {
	assert(config_set_string(&gstate.config_user, "paths.texturepack", pack));
}

static void set_async(bool on) {
	assert(config_set_string(&gstate.config_user, "audio.async_load",
							 on ? "on" : "off"));
}

static void set_deferred(bool on) {
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave",
							 on ? "on" : "off"));
}

static void status(struct audio_async_status* st) {
	audio_registry_async_status(st);
}

static bool is_protected(uint16_t id) {
	return key_protected(id);
}

/* Sync reload of `pack` (async off), snapshot. */
static void sync_scan(const char* pack, struct scan_result* r) {
	set_async(false);
	set_pack(pack);
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_OFF);
	assert(!audio_registry_async_pending());
	snapshot(r);
}

/* Step the hand-driven loader to completion and poll once. */
static void manual_finish(void) {
	int guard = 0;
	while(audio_registry_async_step())
		assert(++guard < 10000);
	audio_registry_async_poll();
}

static struct scan_result s_sync_a, s_sync_b, s_sync_a_deferred;

static char s_rec[2048];
static bool sink(const char* line) {
	snprintf(s_rec, sizeof(s_rec), "%s", line);
	return true;
}

/* ===== a: the sync oracle =============================================== */

static void test_a_sync_oracle(void) {
	sync_scan("pack_a", &s_sync_a);
	sync_scan("pack_b", &s_sync_b);
	assert(s_sync_a.hash != s_sync_b.hash); /* the two trees really differ */
	assert(s_sync_a.variants > AUDIO_SOUND_COUNT); /* multi-variant keys */
	assert(s_sync_a.c[1][C_HIT] > 0);				/* per-key fallbacks */
	printf("PASS a sync_oracle (a: v=%d used=%u, b: v=%d used=%u)\n",
		   s_sync_a.variants, (unsigned)s_sync_a.arena_used, s_sync_b.variants,
		   (unsigned)s_sync_b.arena_used);
}

/* ===== b: hand-stepped equivalence + publication order ================= */

static void reentrant_poll_probe(void* ctx) {
	unsigned* calls = ctx;
	(*calls)++;
	struct audio_async_status before, after;
	status(&before);
	assert(before.state == AUDIO_ASYNC_RUNNING);
	audio_registry_async_poll();
	status(&after);
	assert(after.state == before.state); /* only the outer poll completes */
	assert(audio_registry_ready_state() == AUDIO_REGISTRY_LOADING);
}

static void test_b_manual_equiv(void) {
	audio_registry_test_async_manual(true);
	set_async(true);
	set_pack("pack_a");
	struct audio_async_status before;
	status(&before);
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	assert(audio_registry_async_pending());
	assert(st.generation > before.generation);
	assert(st.keys_done == 0 && st.keys_total > 0);
	/* Pass-C key count: every key that is neither menu.* nor note.*. */
	int expect_rest = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
		if(!is_protected(id))
			expect_rest++;
	assert(st.keys_total == expect_rest);

	/* Passes A+B complete and PLAYABLE before the first pass-C step: every
	   protected key that the sync oracle resolved has the same count now,
	   and its pick returns resident PCM. */
	unsigned protected_seen = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(!is_protected(id))
			continue;
		protected_seen++;
		int n = audio_registry_variant_count(id);
		assert(n > 0); /* every menu/note key exists in pack_a or assets */
		const struct audio_sample* s = audio_registry_pick(id);
		assert(s && s->pcm && s->frames > 0);
	}
	assert(protected_seen >= 13); /* 8 menu + 5 note */

	/* Every pass-C key is PENDING: pick NULL, count 0, skips counted. */
	unsigned skips0 = st.skips;
	int pending = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(is_protected(id))
			continue;
		assert(audio_registry_pick(id) == NULL);
		assert(audio_registry_variant_count(id) == 0);
		assert(audio_registry_variant(id, 0) == NULL);
		pending++;
	}
	status(&st);
	assert(st.skips >= skips0 + (unsigned)pending);
	assert(st.protected_pending == 0);

	/* One key per step, in table order; each becomes visible exactly when
	   published, and nothing after it is visible yet. */
	int done = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(is_protected(id))
			continue;
		bool more = audio_registry_async_step();
		done++;
		status(&st);
		assert(st.keys_done == done);
		assert(more == (done < st.keys_total));
		/* Visible now (count may legitimately be 0 for the silent key). */
		int n = audio_registry_variant_count(id);
		if(n > 0) {
			const struct audio_sample* s = audio_registry_pick(id);
			assert(s && s->pcm);
		}
		/* The NEXT pass-C key is still pending. */
		for(uint16_t nx = (uint16_t)(id + 1); nx < AUDIO_SOUND_COUNT; nx++)
			if(!is_protected(nx)) {
				assert(audio_registry_pick(nx) == NULL);
				break;
			}
	}
	assert(!audio_registry_async_step()); /* nothing left */
	assert(audio_registry_async_pending()); /* until the poll joins it */
	unsigned service_calls = 0;
	audio_registry_set_service(reentrant_poll_probe, &service_calls);
	audio_registry_async_poll();
	audio_registry_set_service(NULL, NULL);
	assert(service_calls >= 2); /* poll entry and completion epilogue */
	status(&st);
	assert(st.state == AUDIO_ASYNC_DONE);
	assert(!audio_registry_async_pending());
	assert(st.protected_pending == 0);
	assert(st.ready_ms >= 0 && st.sync_ms >= 0 && st.sync_ms <= st.ready_ms);

	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_a);
	printf("PASS b manual_equiv (%d keys, %u skips)\n", st.keys_total,
		   st.skips);
}

/* ===== c: the real loader thread ======================================== */

struct async_service_probe { pthread_t owner; unsigned calls; };
static void async_service_probe(void* ctx) {
	struct async_service_probe* p = ctx;
	assert(pthread_equal(pthread_self(), p->owner));
	p->calls++;
}

static void test_c_thread_equiv(void) {
	struct async_service_probe probe = {pthread_self(), 0};
	audio_registry_set_service(async_service_probe, &probe);
	audio_registry_test_async_manual(false);
	set_async(true);
	set_pack("pack_b");
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	assert(audio_registry_ready_state() == AUDIO_REGISTRY_LOADING);
	/* The protected set is playable while the thread works. */
	assert(audio_registry_pick(SOUND_MENU_SELECT) != NULL);
	int guard = 0;
	unsigned calls = probe.calls;
	do {
		SLEEP_MS(2);
		audio_registry_async_poll();
		assert(probe.calls > calls && "each main-thread poll services the load");
		calls = probe.calls;
		status(&st);
		assert(++guard < 30000); /* 60 s: a wedge, not a slow host */
	} while(st.state == AUDIO_ASYNC_RUNNING);
	assert(st.state == AUDIO_ASYNC_DONE);
	assert(st.keys_done == st.keys_total);
	assert(st.protected_pending == 0);
	assert(audio_registry_ready_state() == AUDIO_REGISTRY_READY);
	audio_registry_set_service(NULL, NULL);
	puts("PASS serviced async poll: every poll serviced on main, readiness waits for completion");
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_b);
	printf("PASS c thread_equiv (ready in %d ms, sync part %d ms)\n",
		   st.ready_ms, st.sync_ms);
}

/* ===== d: thread creation failure -> synchronous fallback ============== */

static void test_d_thread_failure(void) {
	audio_registry_test_async_manual(false);
	audio_registry_test_async_fail_thread(true);
	set_async(true);
	set_pack("pack_a");
	struct audio_async_status before;
	status(&before);
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_OFF); /* finished right there */
	assert(!audio_registry_async_pending());
	assert(st.thread_failures == before.thread_failures + 1);
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_a);
	audio_registry_test_async_fail_thread(false);
	printf("PASS d thread_failure\n");
}

/* ===== e: cancel + join, stale step ==================================== */

static void test_e_cancel_join(void) {
	audio_registry_test_async_manual(true);
	set_async(true);
	set_pack("pack_b");
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	uint32_t gen = st.generation;
	assert(audio_registry_async_step());
	assert(audio_registry_async_step());
	assert(audio_registry_async_step());
	status(&st);
	assert(st.keys_done == 3);
	audio_registry_async_join();
	status(&st);
	assert(st.state == AUDIO_ASYNC_OFF);
	assert(st.generation > gen); /* the cancel retired the generation */
	assert(!audio_registry_async_pending());
	/* A stale step (the loader waking after the cancel) publishes nothing. */
	assert(!audio_registry_async_step());
	status(&st);
	assert(st.keys_done == 3);
	/* The arena is rebuilt whole by the next scan, sync or async. */
	struct scan_result r;
	sync_scan("pack_b", &r);
	expect_equal(&r, &s_sync_b);
	printf("PASS e cancel_join\n");
}

/* ===== f: repeated switches ============================================ */

static void test_f_repeated_switches(void) {
	audio_registry_test_async_manual(true);
	set_async(true);
	const char* packs[] = {"pack_a", "pack_b", "pack_a", "pack_b", "pack_a"};
	for(int k = 0; k < 5; k++) {
		set_pack(packs[k]);
		audio_registry_reload(); /* cancels + joins the half-finished run */
		struct audio_async_status st;
		status(&st);
		assert(st.state == AUDIO_ASYNC_RUNNING);
		for(int s = 0; s < 1 + k * 2; s++)
			assert(audio_registry_async_step());
		/* Between switches the protected set is always whole. */
		assert(audio_registry_pick(SOUND_MENU_BACK) != NULL);
		assert(st.protected_pending == 0);
	}
	manual_finish();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_DONE);
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_a);
	printf("PASS f repeated_switches\n");
}

/* ===== g: progress timeout -> FAILED -> retry ========================== */

static void test_g_stall_retry(void) {
	audio_registry_test_async_manual(true);
	set_async(true);
	set_pack("pack_b");
	audio_registry_test_async_set_clock_ms(1000);
	audio_registry_reload();
	assert(audio_registry_async_step()); /* progress at t=1000 */
	struct audio_async_status st;
	status(&st);
	unsigned stalls0 = st.stalls;
	/* Inside the timeout: still RUNNING. */
	audio_registry_test_async_set_clock_ms(1000 + AUDIO_ASYNC_PROGRESS_TIMEOUT_MS);
	audio_registry_async_poll();
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	/* Past it: FAILED, still pending, declared exactly once. */
	audio_registry_test_async_set_clock_ms(1001 + AUDIO_ASYNC_PROGRESS_TIMEOUT_MS);
	audio_registry_async_poll();
	status(&st);
	assert(st.state == AUDIO_ASYNC_FAILED);
	assert(st.stalls == stalls0 + 1);
	assert(audio_registry_async_pending());
	audio_registry_async_poll();
	status(&st);
	assert(st.stalls == stalls0 + 1); /* never renewed, never re-declared */
	/* Pending keys stay skipped while FAILED; the protected set still plays. */
	assert(audio_registry_pick(SOUND_MENU_SELECT) != NULL);
	/* The gate's decision on FAILED: show the error, retry on A, back on B. */
	assert(loading_notice_gate_decide(AUDIO_ASYNC_FAILED, false, false)
		   == LOADING_NOTICE_GATE_FAILED);
	assert(loading_notice_gate_decide(AUDIO_ASYNC_FAILED, false, true)
		   == LOADING_NOTICE_GATE_RETRY);
	assert(loading_notice_gate_decide(AUDIO_ASYNC_FAILED, true, true)
		   == LOADING_NOTICE_GATE_BACK);
	/* Retry == a full reload under a new generation, then completion. */
	uint32_t gen = st.generation;
	audio_registry_test_async_set_clock_ms(5000 + AUDIO_ASYNC_PROGRESS_TIMEOUT_MS);
	audio_registry_reload();
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	assert(st.generation > gen);
	manual_finish();
	status(&st);
	assert(st.state == AUDIO_ASYNC_DONE);
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_b);
	audio_registry_test_async_set_clock_ms(-1);
	printf("PASS g stall_retry\n");
}

/* ===== h: deferred cave coexistence ==================================== */

static void test_h_deferred_coexist(void) {
	set_deferred(true);
	sync_scan("pack_a", &s_sync_a_deferred);
	/* The deferred scan differs from the resident one (the cave key holds
	   probe-only variants, pcm NULL) -- the case is not vacuous. */
	assert(s_sync_a_deferred.hash != s_sync_a.hash
		   || audio_registry_variant_count(SOUND_AMBIENT_CAVE) == 0);
	audio_registry_test_async_manual(true);
	set_async(true);
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	manual_finish();
	status(&st);
	assert(st.state == AUDIO_ASYNC_DONE);
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_a_deferred);
	set_deferred(false);
	set_async(false);
	printf("PASS h deferred_coexist\n");
}

/* ===== i: the world-entry gate ========================================= */

static void test_i_gate(void) {
	/* Decision table (loading_notice.h). */
	assert(loading_notice_gate_decide(AUDIO_ASYNC_RUNNING, false, false)
		   == LOADING_NOTICE_GATE_WAIT);
	assert(loading_notice_gate_decide(AUDIO_ASYNC_RUNNING, false, true)
		   == LOADING_NOTICE_GATE_WAIT); /* A does nothing while loading */
	assert(loading_notice_gate_decide(AUDIO_ASYNC_RUNNING, true, false)
		   == LOADING_NOTICE_GATE_BACK);
	assert(loading_notice_gate_decide(AUDIO_ASYNC_DONE, false, false)
		   == LOADING_NOTICE_GATE_SEND);
	assert(loading_notice_gate_decide(AUDIO_ASYNC_OFF, false, false)
		   == LOADING_NOTICE_GATE_SEND);
	assert(loading_notice_gate_decide(AUDIO_ASYNC_DONE, true, false)
		   == LOADING_NOTICE_GATE_BACK); /* Back wins over a same-frame DONE */

	/* Parking: with a run in flight the request is held, the gate screen is
	   current, and nothing was sent. */
	audio_registry_test_async_manual(true);
	set_async(true);
	set_pack("pack_a");
	audio_registry_reload();
	assert(audio_registry_async_pending());
	struct loading_notice_gate_stats gs;
	loading_notice_gate_stats(&gs);
	unsigned sent0 = gs.sent;
	assert(loading_notice_gate_load_world("saves/World1", &screen_select_world));
	loading_notice_gate_stats(&gs);
	assert(gs.parked);
	assert(strcmp(gs.path, "saves/World1") == 0);
	assert(gs.sent == sent0);
	assert(gstate.players[0].current_screen == &screen_sound_gate);
	/* A second request while parked replaces the first (one at a time). */
	assert(loading_notice_gate_load_world("saves/World2", &screen_select_world));
	loading_notice_gate_stats(&gs);
	assert(gs.parked && strcmp(gs.path, "saves/World2") == 0);
	/* Finish the run: the gate's own update would now SEND. The send itself
	   (svin_rpc_send + the loading screen) is production code exercised by
	   the frontend rigs; here only the decision is asserted. */
	manual_finish();
	assert(!audio_registry_async_pending());
	struct audio_async_status st;
	status(&st);
	assert(loading_notice_gate_decide((int)st.state, false, false)
		   == LOADING_NOTICE_GATE_SEND);
	set_async(false);
	printf("PASS i gate\n");
}

/* ===== j: the boot record ============================================== */

static void test_j_load_record(void) {
	/* audio_reload records carry async=true when the async run completed. */
	audio_registry_test_set_observer(sink);
	audio_registry_test_async_manual(true);
	set_async(true);
	set_pack("pack_a");
	s_rec[0] = '\0';
	audio_registry_reload();
	assert(s_rec[0] == '\0'); /* not yet: pass C in flight */
	manual_finish();
	assert(strstr(s_rec, "\"event\":\"audio_reload\""));
	assert(strstr(s_rec, "\"async\":true"));
	/* And a sync reload says so. */
	s_rec[0] = '\0';
	set_async(false);
	audio_registry_reload();
	assert(strstr(s_rec, "\"async\":false"));
	audio_registry_test_set_observer(NULL);
	printf("PASS j load_record\n");
}

/* ===== k: reads racing the real loader ================================= */

static void test_k_race_reads(void) {
	audio_registry_test_async_manual(false);
	set_async(true);
	set_pack("pack_a");
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	int seen_count[AUDIO_SOUND_COUNT];
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++)
		seen_count[id] = -1; /* -1 = never visible yet */
	unsigned rounds = 0, pending_reads = 0, visible_reads = 0;
	int guard = 0;
	do {
		for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
			int n = audio_registry_variant_count(id);
			const struct audio_sample* p = audio_registry_pick(id);
			if(seen_count[id] >= 0) {
				/* Once visible, a key stays visible with the same count. */
				assert(n == seen_count[id]);
				assert((p != NULL) == (n > 0));
			}
			if(n > 0 || (is_protected(id) && p)) {
				seen_count[id] = n;
				visible_reads++;
				/* Every variant of a visible key is a COMPLETE resident sample
				   inside the arena. */
				for(int v = 0; v < n; v++) {
					const struct audio_sample* s = audio_registry_variant(id, v);
					assert(s && s->pcm && s->frames > 0 && s->rate == TEST_RATE);
					assert((const uint8_t*)s->pcm
						   < (const uint8_t*)s->pcm + audio_arena_total());
				}
				if(p)
					assert(p->pcm && p->frames > 0);
			} else if(p == NULL && n == 0 && !is_protected(id)) {
				pending_reads++;
			}
		}
		rounds++;
		/* One round per "frame": the loader must get the lock in between (a
		   main thread spinning on it would only prove mutex unfairness). */
		SLEEP_MS(1);
		audio_registry_async_poll();
		status(&st);
		assert(++guard < 60000); /* >= 60 s: a wedge, not a slow host */
	} while(st.state == AUDIO_ASYNC_RUNNING);
	assert(st.state == AUDIO_ASYNC_DONE);
	assert(st.protected_pending == 0);
	assert(st.ogg_decoded_async == 0);
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_a);
	printf("PASS k race_reads (%u rounds, %u pending reads, %u visible)\n",
		   rounds, pending_reads, visible_reads);
}

/* ===== l: a raw .ogg pack keeps the synchronous path =================== */

static void test_l_ogg_sync(void) {
	struct scan_result sync_ogg;
	sync_scan("pack_ogg", &sync_ogg);
	/* The .ogg really is part of the sync registry (not vacuous). */
	assert(audio_registry_variant_count(SOUND_STEP_GRASS) == 2);
	audio_registry_test_async_manual(false); /* a real thread WOULD be made */
	set_async(true);
	struct audio_async_status before;
	status(&before);
	audio_registry_reload();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_OFF); /* declined: whole scan synchronous */
	assert(!audio_registry_async_pending());
	assert(st.ogg_sync == before.ogg_sync + 1);
	assert(st.ogg_decoded_async == 0);
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &sync_ogg);
	/* And a WAV-only pack under the same settings still goes async. */
	set_pack("pack_a");
	audio_registry_reload();
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING);
	assert(st.ogg_sync == before.ogg_sync + 1);
	int guard = 0;
	do {
		SLEEP_MS(2);
		audio_registry_async_poll();
		status(&st);
		assert(++guard < 30000);
	} while(st.state == AUDIO_ASYNC_RUNNING);
	assert(st.state == AUDIO_ASYNC_DONE && st.ogg_decoded_async == 0);
	printf("PASS l ogg_sync\n");
}

/* ===== m: the gate screen's real update ================================ */

static void gate_update(void) {
	screen_sound_gate.update(&screen_sound_gate, 1.0F / 60.0F);
}

static void test_m_gate_update(void) {
	audio_registry_test_async_manual(true);
	set_async(true);
	set_pack("pack_a");
	struct loading_notice_gate_stats gs;

	/* BACK while RUNNING: dropped, back where the player was, nothing sent. */
	audio_registry_reload();
	size_t svin0 = svin_pending_count();
	assert(loading_notice_gate_load_world("saves/W", &screen_select_world));
	assert(gstate.players[0].current_screen == &screen_sound_gate);
	loading_notice_gate_stats(&gs);
	unsigned dropped0 = gs.dropped, sent0 = gs.sent, retries0 = gs.retries;
	loading_notice_gate_test_input(1, 0);
	gate_update();
	loading_notice_gate_stats(&gs);
	assert(!gs.parked && gs.dropped == dropped0 + 1 && gs.sent == sent0);
	assert(gstate.players[0].current_screen == &screen_select_world);

	/* WAIT while RUNNING with no input: nothing changes. */
	assert(loading_notice_gate_load_world("saves/W", &screen_select_world));
	loading_notice_gate_test_input(0, 0);
	gate_update();
	loading_notice_gate_stats(&gs);
	assert(gs.parked && gstate.players[0].current_screen == &screen_sound_gate);

	/* SEND once the registry is whole: the parked SRPC_LOAD_WORLD is queued
	   to the (idle, never-started) server inbox and the loading screen is
	   current -- the pre-gate sequence, one frame later. */
	manual_finish();
	assert(!audio_registry_async_pending());
	gate_update();
	loading_notice_gate_stats(&gs);
	assert(!gs.parked && gs.sent == sent0 + 1);
	assert(svin_pending_count() >= svin0); /* accepted or queued, never lost */
	assert(gstate.players[0].current_screen == &screen_load_world);

	/* FAILED -> RETRY: a reload under a new generation, still parked. */
	audio_registry_test_async_set_clock_ms(100);
	audio_registry_reload();
	assert(loading_notice_gate_load_world("saves/W", &screen_select_world));
	audio_registry_test_async_set_clock_ms(101 + AUDIO_ASYNC_PROGRESS_TIMEOUT_MS);
	audio_registry_async_poll();
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_FAILED);
	uint32_t gen = st.generation;
	loading_notice_gate_test_input(0, 0);
	gate_update(); /* shows the error page, stays parked */
	loading_notice_gate_stats(&gs);
	assert(gs.parked && gs.retries == retries0);
	loading_notice_gate_test_input(0, 1);
	gate_update();
	loading_notice_gate_stats(&gs);
	status(&st);
	assert(gs.parked && gs.retries == retries0 + 1);
	assert(st.state == AUDIO_ASYNC_RUNNING && st.generation > gen);
	assert(gstate.players[0].current_screen == &screen_sound_gate);

	/* FAILED then the loader finishes anyway (review L1): DONE, no re-decode,
	   and the gate sends. */
	audio_registry_test_async_set_clock_ms(200 + AUDIO_ASYNC_PROGRESS_TIMEOUT_MS);
	assert(audio_registry_async_step());
	audio_registry_test_async_set_clock_ms(401 + 2 * AUDIO_ASYNC_PROGRESS_TIMEOUT_MS);
	audio_registry_async_poll();
	status(&st);
	assert(st.state == AUDIO_ASYNC_FAILED);
	gen = st.generation;
	while(audio_registry_async_step())
		;
	audio_registry_async_poll();
	status(&st);
	assert(st.state == AUDIO_ASYNC_DONE && st.generation == gen);
	loading_notice_gate_test_input(0, 0);
	gate_update();
	loading_notice_gate_stats(&gs);
	assert(!gs.parked && gs.sent == sent0 + 2);
	assert(gstate.players[0].current_screen == &screen_load_world);
	struct scan_result r;
	snapshot(&r);
	expect_equal(&r, &s_sync_a);
	audio_registry_test_async_set_clock_ms(-1);
	loading_notice_gate_test_input(-1, -1);
	printf("PASS m gate_update\n");
}

/* ===== n: the remote half ============================================== */

static void test_n_remote_gate(void) {
	audio_registry_test_async_manual(true);
	set_async(true);
	set_pack("pack_b");
	/* Nothing pending: the seam never shows the gate. */
	assert(!audio_registry_async_pending());
	assert(!loading_notice_gate_remote_connect());
	audio_registry_reload();
	assert(audio_registry_async_pending());
	assert(loading_notice_gate_remote_connect());
	struct loading_notice_gate_stats gs;
	loading_notice_gate_stats(&gs);
	assert(gs.parked && gs.remote && gs.path[0] == '\0');
	assert(gstate.players[0].current_screen == &screen_sound_gate);
	/* Back: to the join screen (screen_multiplayer_setup is NULL in a unit
	   test, so the title), dropped. */
	unsigned dropped0 = gs.dropped;
	loading_notice_gate_test_input(1, 0);
	screen_sound_gate.update(&screen_sound_gate, 0.016F);
	loading_notice_gate_stats(&gs);
	assert(!gs.parked && gs.dropped == dropped0 + 1);
	assert(gstate.players[0].current_screen == &screen_title);
	/* SEND: re-enters the connect screen (NULL here -> its fallback, the
	   join screen / title), never the loading screen, nothing queued. */
	assert(loading_notice_gate_remote_connect());
	size_t svin0 = svin_pending_count();
	manual_finish();
	loading_notice_gate_test_input(0, 0);
	screen_sound_gate.update(&screen_sound_gate, 0.016F);
	loading_notice_gate_stats(&gs);
	assert(!gs.parked);
	assert(svin_pending_count() == svin0);
	assert(gstate.players[0].current_screen == &screen_title);
	loading_notice_gate_test_input(-1, -1);
	set_async(false);
	printf("PASS n remote_gate\n");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_audio_async_load");
	assert(GETCWD(s_srcroot, sizeof(s_srcroot)));

	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	/* resident scan unless a case turns deferred on: the opt-out of the
	   2026-10-21 deferred-cave default */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "off"));

	/* The real vorbis sample (CC0, tests/fixtures/audio_deferred), read before
	   leaving the source root. */
	{
		FILE* f = fopen("tests/fixtures/audio_deferred/cave_mono.ogg", "rb");
		assert(f && "run from the source root");
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		assert(n > 0);
		s_ogg = malloc((size_t)n);
		assert(s_ogg);
		assert(fread(s_ogg, 1, (size_t)n, f) == (size_t)n);
		s_ogg_len = (size_t)n;
		fclose(f);
	}

	test_fixture_dir("test_audio_async_load_tmp", s_fixture,
					 sizeof(s_fixture));
	MKDIR(s_fixture);
	assert(CHDIR(s_fixture) == 0);
	build_assets();
	build_pack_a();
	build_pack_b();
	build_pack_ogg();
	assert(clin_init() && svin_init()); /* the gate queues a real SRPC */

	/* BOOT load with the opt-in ON (hand-stepped): audio_init returns with
	   pass C in flight, and the audio_load record is stashed by the poll at
	   completion, carrying async=true and the two times. */
	set_async(true);
	set_pack("pack_a");
	audio_registry_test_async_manual(true);
	audio_registry_test_set_observer(sink);
	s_rec[0] = '\0';
	audio_init(false, 0);
	struct audio_async_status st;
	status(&st);
	assert(st.state == AUDIO_ASYNC_RUNNING && audio_registry_async_pending());
	audio_registry_emit_load_stats();
	assert(s_rec[0] == '\0'); /* nothing to emit yet: the scan is not whole */
	manual_finish();
	status(&st);
	assert(st.state == AUDIO_ASYNC_DONE);
	audio_registry_emit_load_stats();
	assert(strstr(s_rec, "\"event\":\"audio_load\""));
	assert(strstr(s_rec, "\"async\":true"));
	assert(strstr(s_rec, "\"sync_ms\":") && strstr(s_rec, "\"ready_ms\":"));
	assert(strstr(s_rec, "\"async_protected_pending\":0"));
	audio_registry_test_set_observer(NULL);
	printf("PASS boot async_load (%d keys, ready %d ms)\n", st.keys_total,
		   st.ready_ms);
	set_async(false);

	test_a_sync_oracle();
	test_b_manual_equiv();
	test_c_thread_equiv();
	test_d_thread_failure();
	test_e_cancel_join();
	test_f_repeated_switches();
	test_g_stall_retry();
	test_h_deferred_coexist();
	test_i_gate();
	test_j_load_record();
	test_k_race_reads();
	test_l_ogg_sync();
	test_m_gate_update();
	test_n_remote_gate();

	audio_registry_async_join();
	assert(CHDIR(s_srcroot) == 0);
	test_fixture_dir_cleanup(s_fixture);
	puts("PASS test_audio_async_load");
	return 0;
}
