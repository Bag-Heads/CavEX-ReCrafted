/*
	test_music_readahead_equiv -- T1 proof for the 2026-09-26 music decode
	read-ahead (docs/intent/music-readahead.md, audio_music_ring.h's
	THIRTY-TWO HALVES banner).

	The change widens the decode ring 4 -> 32 halves and makes the worker
	sleep until MUSIC_RING_REFILL_LOW halves are free, then refill the whole
	ring in one burst. It is a T1 (bit-exact) claim: the PCM handed to the
	voice must be byte-identical to the legacy ring's, and nothing about the
	Beta music timer / pick RNG may move. This test proves it END TO END
	through the REAL audio_music.c -- the real decode worker thread, the real
	ring bookkeeping, the real drain loop -- not a model of them:

	  A. Pure goldens for the two new helpers (free_count / refill_due),
	     hand-derived at n = 32 / low = 8.
	  B. PCM equivalence, passthrough rate: a synthetic 12 s stereo Vorbis
	     fixture (tests/fixtures/music_readahead, ~130 chunks, wraps the
	     32-half ring four times) streamed through the legacy policy
	     (4 halves, refill at 1 free == the pre-change behaviour), the
	     production policy (32 / 8), and two small stress policies that wrap
	     many times (5 / 3, 8 / 4). Every captured hand-off stream must be
	     byte-identical to each other AND to an independent whole-file
	     stb_vorbis decode with the same average downmix, requested in
	     unrelated piece sizes. The consumer is a scripted "device" that
	     accepts 0..4 chunks per frame with stalls, so the worker genuinely
	     fills the ring, idles below the low-water mark, and is woken by the
	     main thread's refill_due signal -- every new branch runs.
	  C. PCM equivalence, resampling path (forced 48 kHz device rate, the
	     real Wii configuration): legacy == production == 32 / 8 stress, and
	     == the reference run through audio_resample_run in 1000-frame pieces.
	  D. "Fewer, larger wakes", measured: under a steady one-chunk-per-frame
	     consumer the production policy must commit >= 4 chunks per worker
	     wake and wake at most half as often as the legacy policy.
	  E. Music timer / pick RNG untouched: the same audio_music_tick
	     sequence from the same seed leaves gstate.rand_src identical under
	     both policies.
	  F. Optional real Beta track: if CW_MUSIC_EQUIV_TRACK names an owned
	     Beta .ogg (never committed -- per-client-import-only), B and C are
	     repeated on it. Skipped (and said so) when unset.
	  G. debug.music_pick (hardware-run lever, 100 * telemetry key + file
	     digit; hal1 = 201): with debug.music_force on, 201 opens hal1 on
	     every seed; absent / file-not-in-pack / force-off all give the
	     random pick; gstate.rand_src is identical in all four modes.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "platform/audio.h"
#include "platform/audio_music.h"
#include "platform/audio_music_ring.h"
#include "platform/audio_music_timer.h"
#include "platform/audio_resample.h"
#include "platform/thread.h"
#include "util.h"

/* Same macro set audio_music.c / audio_registry.c compile stb_vorbis with
   (audio_registry.c is the one implementation TU in the test pool). */
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#include <stb/stb_vorbis.c>
#pragma GCC diagnostic pop

/* CAVEWII_TESTING hooks in audio_music.c (declared there, not in the header,
   so no production caller can see them -- the audio_music_test_* precedent). */
void audio_music_test_set_pcm_sink(void (*sink)(const int16_t* pcm,
												uint32_t frames,
												uint32_t rate));
void audio_music_test_set_voice_playing(bool (*playing)(void));
void audio_music_test_set_device_rate(uint32_t rate);
bool audio_music_test_set_ring_policy(int halves, int refill_low);
void audio_music_test_worker_counters(unsigned* wakes, unsigned* chunks);
void audio_music_test_set_backend_ready(int ready);
void audio_music_test_set_timer(int ticks_before_music);
int audio_music_test_pending_key(void);
void audio_music_test_set_observer(bool (*sink)(const char* line));

#define CHUNK 4096u
#define FIXTURE "tests/fixtures/music_readahead/stream_stereo_12s.ogg"

static char s_fixture_PACK[128];

/* ===== A: pure goldens for the new helpers ================================ */

static void test_pure_refill_helpers(void) {
	bool ready[MUSIC_RING_HALVES];
	const int n = MUSIC_RING_HALVES, low = MUSIC_RING_REFILL_LOW;
	assert(n == 32 && low == 8);

	/* Nothing started: every half is free, a refill is due at once. */
	for(int h = 0; h < n; h++)
		ready[h] = false;
	assert(music_ring_free_count_n(ready, n, -1) == 32);
	assert(music_ring_refill_due_n(ready, n, -1, low));
	/* Whole ring banked before the first start: nothing to fill. */
	for(int h = 0; h < n; h++)
		ready[h] = true;
	assert(music_ring_free_count_n(ready, n, -1) == 0);
	assert(!music_ring_refill_due_n(ready, n, -1, low));

	/* Half 5 playing, everything else banked: 0 free (the playing half is
	   never counted, even when its ready flag is clear). */
	ready[5] = false;
	assert(music_ring_free_count_n(ready, n, 5) == 0);
	assert(!music_ring_refill_due_n(ready, n, 5, low));
	/* Free 6..12 (7 halves): one short of the low-water mark -- idle. */
	for(int h = 6; h <= 12; h++)
		ready[h] = false;
	assert(music_ring_free_count_n(ready, n, 5) == 7);
	assert(!music_ring_refill_due_n(ready, n, 5, low));
	/* The 8th free half makes it due. */
	ready[13] = false;
	assert(music_ring_free_count_n(ready, n, 5) == 8);
	assert(music_ring_refill_due_n(ready, n, 5, low));
	/* Wraparound counting: playing 30, halves 31,0..6 free = 8. */
	for(int h = 0; h < n; h++)
		ready[h] = true;
	ready[30] = false;
	ready[31] = false;
	for(int h = 0; h <= 6; h++)
		ready[h] = false;
	assert(music_ring_free_count_n(ready, n, 30) == 8);
	assert(music_ring_refill_due_n(ready, n, 30, low));
	assert(music_ring_fill_target_n(ready, n, 30) == 31);
	/* An underrun (next half not ready, nothing banked) is always due. */
	for(int h = 0; h < n; h++)
		ready[h] = false;
	assert(music_ring_advance_n(ready, n, 9) == -1);
	assert(music_ring_refill_due_n(ready, n, 9, low));
	/* Legacy policy (refill at 1): ANY free half is due -- the old
	   wake-per-freed-half behaviour. */
	bool lr[MUSIC_RING_HALVES_LEGACY] = {true, true, false, true};
	assert(music_ring_refill_due_n(lr, MUSIC_RING_HALVES_LEGACY, 1,
								   MUSIC_RING_REFILL_LOW_LEGACY));
	lr[2] = true;
	assert(!music_ring_refill_due_n(lr, MUSIC_RING_HALVES_LEGACY, 1,
									MUSIC_RING_REFILL_LOW_LEGACY));
	/* ready_run is capped at n-1 at the new size too (drain loop bound). */
	for(int h = 0; h < n; h++)
		ready[h] = true;
	assert(music_ring_ready_run_n(ready, n, 0) == 31);
	puts("  A: free_count/refill_due goldens (n=32, low=8: idle at 7 free, "
		 "due at 8, wraparound, underrun always due, legacy = any free) OK");
}

/* ===== capture sink + scripted device ===================================== */

struct pcm_buf {
	int16_t* s;
	size_t len, cap;
	unsigned chunks;
	uint32_t rate;
};

static struct pcm_buf s_cap;

static void pcm_push(struct pcm_buf* b, const int16_t* s, size_t n) {
	if(b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 65536;
		while(cap < b->len + n)
			cap *= 2;
		int16_t* p = (int16_t*)realloc(b->s, cap * sizeof(int16_t));
		assert(p);
		b->s = p;
		b->cap = cap;
	}
	if(n)
		memcpy(b->s + b->len, s, n * sizeof(int16_t));
	b->len += n;
}

static void cap_sink(const int16_t* pcm, uint32_t frames, uint32_t rate) {
	assert(frames <= CHUNK);
	if(s_cap.chunks == 0)
		s_cap.rate = rate;
	assert(rate == s_cap.rate); /* one track, one rate */
	pcm_push(&s_cap, pcm, frames);
	s_cap.chunks++;
}

/* The "device": accepts s_budget more chunks this frame, then reports
   itself full (audio_voice_music_playing() == true). */
static int s_budget;
static bool dev_playing(void) {
	if(s_budget <= 0)
		return true;
	s_budget--;
	return false;
}

enum sched { SCHED_JITTER, SCHED_STEADY };

static uint32_t lcg(uint32_t* x) {
	*x = *x * 1664525u + 1013904223u;
	return *x >> 8;
}

/* Streams `path` through the real audio_music.c under one ring policy and
   returns the captured hand-off stream in `out` (caller frees out->s). */
static void run_stream(const char* path, int halves, int low,
					   uint32_t device_rate, enum sched sched,
					   struct pcm_buf* out, unsigned* wakes,
					   unsigned* chunks) {
	assert(audio_music_test_set_ring_policy(halves, low));
	audio_music_test_set_device_rate(device_rate);
	memset(&s_cap, 0, sizeof(s_cap));
	audio_music_test_set_pcm_sink(cap_sink);
	audio_music_test_set_voice_playing(dev_playing);

	unsigned w0, c0;
	audio_music_test_worker_counters(&w0, &c0);

	audio_music_play_record(path);
	assert(audio_music_is_playing());

	uint32_t rng = 0x5eed1234u;
	unsigned long frame = 0;
	while(audio_music_is_playing()) {
		assert(frame < 4000000ul); /* a wedged ring fails, never hangs */
		if(sched == SCHED_STEADY) {
			s_budget = 1;
			audio_music_update();
			thread_msleep(1);
		} else {
			uint32_t r = lcg(&rng);
			unsigned kind = r % 16u;
			if(kind < 2u) {
				s_budget = 0; /* stall: let the worker bank the ring */
				audio_music_update();
				thread_msleep(2);
			} else if(kind < 12u) {
				s_budget = 1;
				audio_music_update();
				thread_yield();
			} else {
				s_budget = (int)((r >> 4) % 5u); /* burst drain 0..4 */
				audio_music_update();
				if((frame & 7u) == 0)
					thread_msleep(1);
			}
		}
		frame++;
	}

	audio_music_test_set_voice_playing(NULL);
	audio_music_test_set_pcm_sink(NULL);
	audio_music_test_set_device_rate(0);
	unsigned w1, c1;
	audio_music_test_worker_counters(&w1, &c1);
	if(wakes)
		*wakes = w1 - w0;
	if(chunks)
		*chunks = c1 - c0;
	*out = s_cap;
	memset(&s_cap, 0, sizeof(s_cap));
}

/* Independent reference: whole-file stb_vorbis decode requested in pieces
   of 1000 frames (deliberately NOT the 4096 chunk), downmixed with the same
   (L+R)/2 truncating average audio_music.c documents. */
static void decode_reference(const char* path, struct pcm_buf* out,
							 uint32_t* src_rate) {
	memset(out, 0, sizeof(*out));
	int err = 0;
	stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
	assert(v);
	stb_vorbis_info info = stb_vorbis_get_info(v);
	int ch = info.channels < 1 ? 1 : (info.channels > 2 ? 2 : info.channels);
	*src_rate = info.sample_rate;
	short piece[2 * 1000];
	int16_t mono[1000];
	for(;;) {
		int n = stb_vorbis_get_samples_short_interleaved(v, ch, piece, ch * 1000);
		if(n <= 0)
			break;
		for(int k = 0; k < n; k++)
			mono[k] = (ch == 1) ? piece[k]
								: (int16_t)(((int)piece[2 * k]
											 + (int)piece[2 * k + 1])
											/ 2);
		pcm_push(out, mono, (size_t)n);
	}
	stb_vorbis_close(v);
}

static void assert_same(const struct pcm_buf* a, const struct pcm_buf* b,
						const char* what) {
	if(a->len != b->len
	   || (a->len && memcmp(a->s, b->s, a->len * sizeof(int16_t)) != 0)) {
		size_t i = 0;
		size_t m = a->len < b->len ? a->len : b->len;
		while(i < m && a->s[i] == b->s[i])
			i++;
		fprintf(stderr, "MISMATCH %s: len %zu vs %zu, first diff at %zu\n",
				what, a->len, b->len, i);
		assert(0);
	}
}

/* ===== B + C: PCM equivalence on one file ================================= */

static void equiv_file(const char* path, const char* label, bool stress) {
	struct pcm_buf ref;
	uint32_t src_rate = 0;
	decode_reference(path, &ref, &src_rate);
	assert(ref.len > 0);

	/* --- B: passthrough (headless PC native rate) --- */
	struct pcm_buf legacy, prod;
	run_stream(path, MUSIC_RING_HALVES_LEGACY, MUSIC_RING_REFILL_LOW_LEGACY, 0,
			   SCHED_JITTER, &legacy, NULL, NULL);
	run_stream(path, MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, 0, SCHED_JITTER,
			   &prod, NULL, NULL);
	assert(legacy.rate == src_rate && prod.rate == src_rate);
	assert_same(&legacy, &ref, "legacy vs reference (passthrough)");
	assert_same(&prod, &ref, "read-ahead vs reference (passthrough)");
	assert(legacy.chunks == prod.chunks);
	/* Every hand-off but the last is a full chunk: nothing was skipped,
	   duplicated or split differently. */
	assert(prod.chunks == (unsigned)((ref.len + CHUNK) / CHUNK));
	if(stress) {
		static const int pol[][2] = {{5, 3}, {8, 4}};
		for(size_t i = 0; i < sizeof(pol) / sizeof(pol[0]); i++) {
			struct pcm_buf s;
			run_stream(path, pol[i][0], pol[i][1], 0, SCHED_JITTER, &s, NULL,
					   NULL);
			assert_same(&s, &ref, "stress policy vs reference");
			free(s.s);
		}
	}
	printf("  B[%s]: passthrough %u Hz, %zu frames in %u chunks -- legacy "
		   "4/1 == read-ahead 32/8%s == independent decode, byte-for-byte "
		   "OK\n",
		   label, (unsigned)src_rate, ref.len, prod.chunks,
		   stress ? " == stress 5/3, 8/4" : "");
	free(legacy.s);
	free(prod.s);

	/* --- C: resampling to the Wii's 48 kHz --- */
	const uint32_t dev = 48000u;
	struct pcm_buf rl, rp;
	run_stream(path, MUSIC_RING_HALVES_LEGACY, MUSIC_RING_REFILL_LOW_LEGACY,
			   dev, SCHED_JITTER, &rl, NULL, NULL);
	run_stream(path, MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, dev,
			   SCHED_JITTER, &rp, NULL, NULL);
	assert(rl.rate == dev && rp.rate == dev);
	assert_same(&rl, &rp, "legacy vs read-ahead (48 kHz)");
	if(stress) {
		struct pcm_buf s;
		run_stream(path, 8, 4, dev, SCHED_JITTER, &s, NULL, NULL);
		assert_same(&s, &rp, "stress 8/4 vs read-ahead (48 kHz)");
		free(s.s);
	}
	/* Independent: the whole reference through the resampler in fixed
	   1000-frame input pieces (deliberately unrelated to the worker's
	   4096-frame source reads -- audio_resample_run's 16.16 phase caps one
	   call below 65536 input frames, so "one call" is not an option). */
	struct audio_resample_state st;
	audio_resample_init(&st, src_rate, dev);
	/* Room for the 16.16 step's truncation surplus (the step is rounded
	   DOWN, so the resampler emits slightly more than len*dev/src). */
	size_t cap = (size_t)((uint64_t)ref.len * dev / src_rate) * 101 / 100 + 64;
	struct pcm_buf whole;
	memset(&whole, 0, sizeof(whole));
	whole.s = (int16_t*)malloc(cap * sizeof(int16_t));
	assert(whole.s);
	size_t pos = 0;
	while(pos < ref.len) {
		uint32_t piece = (ref.len - pos) < 1000 ? (uint32_t)(ref.len - pos)
												: 1000u;
		uint32_t off = 0;
		while(off < piece) {
			uint32_t used = 0;
			assert(whole.len < cap);
			whole.len += audio_resample_run(&st, ref.s + pos + off, piece - off,
											&used, whole.s + whole.len,
											(uint32_t)(cap - whole.len));
			assert(used > 0);
			off += used;
		}
		pos += piece;
	}
	assert_same(&rp, &whole, "read-ahead (48 kHz) vs piecewise resample");
	printf("  C[%s]: resampled %u -> 48000 Hz, %zu frames -- legacy == "
		   "read-ahead == piecewise resample of the reference, byte-for-byte "
		   "OK\n",
		   label, (unsigned)src_rate, rp.len);
	free(rl.s);
	free(rp.s);
	free(whole.s);
	free(ref.s);
}

/* ===== D: fewer, larger wakes ============================================= */

static void test_wake_counts(void) {
	struct pcm_buf a, b;
	unsigned lw, lc, pw, pc;
	run_stream(FIXTURE, MUSIC_RING_HALVES_LEGACY, MUSIC_RING_REFILL_LOW_LEGACY,
			   0, SCHED_STEADY, &a, &lw, &lc);
	run_stream(FIXTURE, MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, 0,
			   SCHED_STEADY, &b, &pw, &pc);
	assert_same(&a, &b, "steady legacy vs read-ahead");
	printf("  D: steady 1 chunk/frame consumer -- legacy %u wakes for %u "
		   "chunks, read-ahead %u wakes for %u chunks\n",
		   lw, lc, pw, pc);
	assert(pw >= 1 && pc >= a.chunks);
	assert(pc >= 4u * pw);		/* >= 4 chunks decoded per wake */
	assert(2u * pw <= lw);		/* at most half the legacy wake count */
	puts("  D: read-ahead decodes in bursts (>= 4 chunks/wake, <= 1/2 the "
		 "legacy wakes) OK");
	free(a.s);
	free(b.s);
}

/* ===== E: music timer / pick RNG untouched ================================ */

static void copy_file(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	assert(in);
	FILE* out = fopen(to, "wb");
	assert(out);
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), in)) > 0)
		assert(fwrite(buf, 1, n, out) == n);
	fclose(in);
	fclose(out);
}

/* Diagnostic repeat uses the real background pick, worker and ACTIVE accessor.
   Independent decode concatenated before resampling is the PCM oracle. */
static void test_continuous_background(void) {
	struct pcm_buf ref;
	uint32_t src;
	assert(config_set_string(&gstate.config_user, "debug.music_force", "on"));
	decode_reference(FIXTURE, &ref, &src);
	MKDIR(s_fixture_PACK);
	MKDIR(test_fixture_join(s_fixture_PACK, "/music"));
	copy_file(FIXTURE, test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	assert(config_set_string(&gstate.config_user, "paths.texturepack", s_fixture_PACK));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 1);
	struct random_gen absent_rng[2];
	for(int mode = 0; mode < 3; mode++) {
		/* absent, explicit OFF, ON */
		json_object_dotremove(json_object(gstate.config_user.root), "debug.music_continuous_int");
		if(mode)
			assert(config_set_int(&gstate.config_user, "debug.music_continuous_int", mode == 2));
		for(int resampled = 0; resampled < 2; resampled++) {
			uint32_t rate = resampled ? 48000u : src;
			struct pcm_buf input = {0}, expected = {0};
			for(int repeat = 0; repeat < (mode == 2 ? 4 : 1); repeat++)
				pcm_push(&input, ref.s, ref.len);
			struct audio_resample_state st;
			audio_resample_init(&st, src, rate);
			if(!resampled)
				pcm_push(&expected, input.s, input.len);
			for(size_t pos = 0; resampled && pos < input.len;) {
				uint32_t n = input.len - pos < 1000 ? (uint32_t)(input.len - pos) : 1000;
				uint32_t used = 0;
				int16_t out[4096];
				uint32_t got = audio_resample_run(&st, input.s + pos, n, &used, out, 4096);
				assert(used || got);
				pcm_push(&expected, out, got);
				pos += used;
			}
			memset(&s_cap, 0, sizeof(s_cap));
			audio_music_test_set_device_rate(resampled ? rate : 0);
			audio_music_test_set_pcm_sink(cap_sink);
			audio_music_test_set_voice_playing(dev_playing);
			memset(&gstate.rand_src, 0, sizeof(gstate.rand_src));
			gstate.rand_src.seed = 12345u;
			audio_music_test_set_backend_ready(1);
			audio_music_test_set_timer(0);
			audio_music_tick();
			audio_music_test_set_backend_ready(-1);
			assert(audio_music_is_playing());
			size_t target = expected.len - (mode == 2 ? CHUNK * 2 : 0);
			for(unsigned ms = 0; audio_music_is_playing() && s_cap.len < target; ms++) {
				assert(ms < 10000);
				s_budget = 1;
				audio_music_update();
				if(mode == 2)
					assert(audio_music_is_playing()); /* RED: EOF creates silence */
				thread_msleep(1);
			}
			if(mode == 2) {
				assert(s_cap.len >= target && s_cap.len > 3 * ref.len);
				assert(s_cap.len <= expected.len);
				assert(memcmp(s_cap.s, expected.s, s_cap.len * sizeof(int16_t)) == 0);
				audio_music_stop();
			} else {
				for(int ms = 0; audio_music_is_playing() && ms < 100; ms++) {
					s_budget = 1;
					audio_music_update();
					thread_msleep(1);
				}
				assert(!audio_music_is_playing());
				assert_same(&s_cap, &expected, "continuous absent/OFF finite background");
				/* Even music_force must retain the post-track silence OFF. */
				audio_music_test_set_backend_ready(1);
				for(int tick = 0; tick < 200; tick++) {
					audio_music_tick();
					assert(!audio_music_is_playing());
				}
				audio_music_test_set_backend_ready(-1);
				if(mode == 0)
					absent_rng[resampled] = gstate.rand_src;
				else {
					assert(gstate.rand_src.seed == absent_rng[resampled].seed);
					assert(gstate.rand_src.has_next_gaussian
						== absent_rng[resampled].has_next_gaussian);
				}
			}
			audio_music_test_set_pcm_sink(NULL);
			audio_music_test_set_voice_playing(NULL);
			audio_music_test_set_device_rate(0);
			free(s_cap.s);
			memset(&s_cap, 0, sizeof(s_cap));
			free(input.s);
			free(expected.s);
		}
	}
	/* Records stay finite with the diagnostic switch ON. */
	struct pcm_buf record;
	run_stream(FIXTURE, MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, 0,
		SCHED_STEADY, &record, NULL, NULL);
	assert_same(&record, &ref, "continuous ON record still finite");
	free(record.s);
	/* Synthetic Vorbis with zero final granule: real decoder opens, but
	   emits no samples. FFmpeg libvorbis silence, EOS granule set to zero
	   and page CRC recomputed; no copyrighted audio in this fixture. */
	const char* empty = "tests/fixtures/music_readahead/empty_stereo.ogg";
	int err = 0;
	stb_vorbis* v = stb_vorbis_open_filename(empty, &err, NULL);
	assert(v);
	short scratch[64];
	assert(stb_vorbis_get_samples_short_interleaved(v, 2, scratch, 64) == 0);
	stb_vorbis_close(v);
	for(int bad = 0; bad < 3; bad++) {
		const char* path = test_fixture_join(s_fixture_PACK, "/music/calm1.ogg");
		if(bad == 0) {
			copy_file(empty, path);
		} else {
			FILE* f = fopen(path, "wb");
			assert(f);
			if(bad == 1)
				assert(fwrite("corrupt", 1, 7, f) == 7);
			fclose(f); /* bad==2: zero-byte file */
		}
		audio_music_registry_reload();
		memset(&s_cap, 0, sizeof(s_cap));
		audio_music_test_set_pcm_sink(cap_sink);
		audio_music_test_set_voice_playing(dev_playing);
		audio_music_test_set_backend_ready(1);
		audio_music_test_set_timer(0);
		audio_music_tick();
		audio_music_test_set_backend_ready(-1);
		for(int ms = 0; audio_music_is_playing() && ms < 2000; ms++) {
			s_budget = 1;
			audio_music_update();
			thread_msleep(1);
		}
		assert(!audio_music_is_playing() && s_cap.len == 0);
		audio_music_test_set_pcm_sink(NULL);
		audio_music_test_set_voice_playing(NULL);
		free(s_cap.s);
		memset(&s_cap, 0, sizeof(s_cap));
	}
	json_object_dotremove(json_object(gstate.config_user.root), "debug.music_continuous_int");
	json_object_dotremove(json_object(gstate.config_user.root), "debug.music_force");
	assert(config_set_string(&gstate.config_user, "paths.texturepack", "assets"));
	audio_music_registry_reload();
	remove(test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	free(ref.s);
	puts("  I: diagnostic ACTIVE through >3 EOFs, native/48k PCM == concatenated oracle; absent/OFF and ON record finite; empty/corrupt exit OK");
}

static struct random_gen tick_run(int halves, int low) {
	assert(audio_music_test_set_ring_policy(halves, low));
	memset(&gstate.rand_src, 0, sizeof(gstate.rand_src));
	gstate.rand_src.seed = 12345u;
	for(int t = 0; t < 3000; t++) {
		audio_music_tick();
		audio_music_update();
	}
	return gstate.rand_src;
}

static void test_rng_untouched(void) {
	MKDIR(s_fixture_PACK);
	MKDIR(test_fixture_join(s_fixture_PACK, "/music"));
	copy_file(FIXTURE, test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 s_fixture_PACK));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 1);

	struct random_gen a
		= tick_run(MUSIC_RING_HALVES_LEGACY, MUSIC_RING_REFILL_LOW_LEGACY);
	struct random_gen b = tick_run(MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW);
	assert(a.seed != 12345u); /* the per-tick cosmetic draw really ran */
	assert(a.seed == b.seed && a.has_next_gaussian == b.has_next_gaussian);
	assert(!audio_music_is_playing()); /* headless: the timer never fires */

	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_music_registry_reload();
	remove(test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	puts("  E: 3000 audio_music_tick()s from one seed leave rand_src "
		 "identical under legacy and read-ahead policies OK");
}

/* ===== G: debug.music_pick lever ========================================= */

enum pick_mode { PICK_ABSENT, PICK_HAL1, PICK_MISSING, PICK_NO_FORCE };

/* One timer firing from `seed` through the REAL audio_music_tick: the timer
   is armed to fire on the first tick, then 20 more ticks draw while the
   track plays. Returns the telemetry key the worker actually opened (waited
   for until it matches the synchronous request) and the resulting rand_src. */
static unsigned pick_run(uint32_t seed, enum pick_mode mode,
						 struct random_gen* rng_out) {
	JSON_Object* root = json_object(gstate.config_user.root);
	json_object_dotremove(root, "debug.music_pick");
	assert(config_set_string(&gstate.config_user, "debug.music_force",
							 mode == PICK_NO_FORCE ? "off" : "on"));
	if(mode == PICK_HAL1 || mode == PICK_NO_FORCE)
		assert(config_set_int(&gstate.config_user, "debug.music_pick", 201));
	else if(mode == PICK_MISSING) /* nuance1: not in this fixture pack */
		assert(config_set_int(&gstate.config_user, "debug.music_pick", 301));

	memset(&gstate.rand_src, 0, sizeof(gstate.rand_src));
	gstate.rand_src.seed = seed;
	audio_music_test_set_backend_ready(1);
	audio_music_test_set_timer(0);
	for(int t = 0; t < 21; t++)
		audio_music_tick();
	audio_music_test_set_backend_ready(-1);
	*rng_out = gstate.rand_src;

	/* The request is synchronous; then wait for the worker to actually open
	   that track (current_key -> telemetry key catches up). */
	assert(audio_music_is_playing());
	unsigned want = (unsigned)audio_music_test_pending_key() + 1u;
	unsigned key = 0;
	for(int ms = 0; ms < 5000 && key != want; ms++) {
		key = audio_music_telemetry_key();
		if(key != want)
			thread_msleep(1);
	}
	assert(key == want);
	audio_music_stop();
	return key;
}

static void test_pick_lever(void) {
	MKDIR(s_fixture_PACK);
	MKDIR(test_fixture_join(s_fixture_PACK, "/music"));
	copy_file(FIXTURE, test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	copy_file(FIXTURE, test_fixture_join(s_fixture_PACK, "/music/hal1.ogg"));
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 s_fixture_PACK));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 2);

	int unset_calm = 0, unset_hal = 0;
	for(uint32_t s = 1; s <= 16; s++) {
		/* Spread seeds: consecutive tiny raw LCG states all land on the
		   same nextInt(2) this early in the stream. */
		uint32_t seed = s * 2654435761u;
		struct random_gen r_abs, r_hal, r_miss, r_off;
		unsigned k_abs = pick_run(seed, PICK_ABSENT, &r_abs);
		unsigned k_hal = pick_run(seed, PICK_HAL1, &r_hal);
		unsigned k_miss = pick_run(seed, PICK_MISSING, &r_miss);
		unsigned k_off = pick_run(seed, PICK_NO_FORCE, &r_off);
		assert(k_abs == 1 || k_abs == 2);
		unset_calm += k_abs == 1;
		unset_hal += k_abs == 2;
		assert(k_hal == 2);		   /* set => hal1 plays, every seed */
		assert(k_miss == k_abs);   /* file absent => the random pick */
		assert(k_off == k_abs);	   /* pick without music_force => inert */
		/* The RNG stream is byte-identical in all four modes. */
		assert(r_abs.seed != seed); /* the draws really ran */
		assert(r_abs.seed == r_hal.seed && r_abs.seed == r_miss.seed
			   && r_abs.seed == r_off.seed);
		assert(r_abs.has_next_gaussian == r_hal.has_next_gaussian);
	}
	/* The uniform draw really varied, so the override changed real picks. */
	assert(unset_calm > 0 && unset_hal > 0);

	JSON_Object* root = json_object(gstate.config_user.root);
	json_object_dotremove(root, "debug.music_pick");
	json_object_dotremove(root, "debug.music_force");
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_music_registry_reload();
	remove(test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	remove(test_fixture_join(s_fixture_PACK, "/music/hal1.ogg"));
	printf("  G: debug.music_pick=201 plays hal1 on 16/16 seeds (unset drew "
		   "calm %d / hal %d); absent, missing-file and force-off picks match "
		   "the random pick; rand_src identical in all four modes OK\n",
		   unset_calm, unset_hal);
}

/* ===== H: the hardware-run config actually starts playback ================
   Reproduces the 2026-09-26 musicra_hal1_pinned Wii run on PC: config
   {"debug":{"music_force":"on"}} read at audio_music_init (main() below
   sets it BEFORE init, so the REAL init-time seeding path arms the timer),
   plus --cw-set debug.music_pick=201 applied as an integer. "Starts" is the
   voice hand-off: music_voice_start -> audio_voice_music_start, which on
   hardware bumps music_starts (or music_rejects); headless it always counts
   a reject, so starts+rejects > 0 is the exact hardware predicate the Wii
   run reported as 0+0. */

static unsigned s_handoffs;
static void count_sink(const int16_t* pcm, uint32_t frames, uint32_t rate) {
	(void)pcm;
	(void)frames;
	(void)rate;
	s_handoffs++;
}
static char s_obs_last[512];
static bool obs_sink(const char* line) {
	snprintf(s_obs_last, sizeof(s_obs_last), "%s", line);
	return true;
}

/* Tick at most `ticks` times (20 Hz world ticks, one update per tick, the
   frame loop's own order) and report whether the voice got a hand-off. */
static unsigned run_until_handoff(int ticks) {
	uint32_t before = audio_music_probe_starts() + audio_music_probe_rejects();
	s_handoffs = 0;
	audio_music_test_set_pcm_sink(count_sink);
	audio_music_test_set_backend_ready(1);
	for(int t = 0; t < ticks && s_handoffs == 0; t++) {
		audio_music_tick();
		for(int ms = 0; ms < 50 && s_handoffs == 0; ms++) {
			audio_music_update();
			if(s_handoffs == 0)
				thread_msleep(1);
		}
	}
	audio_music_test_set_backend_ready(-1);
	audio_music_test_set_pcm_sink(NULL);
	uint32_t after = audio_music_probe_starts() + audio_music_probe_rejects();
	assert((after > before) == (s_handoffs > 0));
	return s_handoffs;
}

static void test_hw_config_starts_playback(void) {
	/* The init-time seed really took the force path: the very next ready
	   tick counts 1 -> 0, the one after fires. */
	assert(strcmp(config_read_string(&gstate.config_user, "debug.music_force",
									 "off"),
				  "on")
		   == 0);

	/* (1) THE WII FAILURE MODE: a pack with no music/ (the run's
	   paths.texturepack was sd:/assets). Force + pick => nothing, 0+0,
	   exactly the run's telemetry -- and now a music_force_no_tracks event
	   names the pack instead of silence. */
	MKDIR(s_fixture_PACK);
	MKDIR(test_fixture_join(s_fixture_PACK, "/music"));
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 s_fixture_PACK));
	assert(config_set_int(&gstate.config_user, "debug.music_pick", 201));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 0);
	audio_music_test_set_observer(obs_sink);
	s_obs_last[0] = '\0';
	assert(run_until_handoff(40) == 0);
	assert(!audio_music_is_playing());
	assert(strstr(s_obs_last, "\"event\":\"music_force_no_tracks\""));
	audio_music_test_set_observer(NULL);

	/* (2) Beta-like pack, force ALONE (pick absent): the init-seeded timer
	   fires within 2 ticks and the voice gets PCM. */
	copy_file(FIXTURE, test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	copy_file(FIXTURE, test_fixture_join(s_fixture_PACK, "/music/hal1.ogg"));
	json_object_dotremove(json_object(gstate.config_user.root),
						  "debug.music_pick");
	audio_music_registry_reload();
	assert(audio_music_track_count() == 2);
	assert(run_until_handoff(2) > 0);
	assert(audio_music_is_playing());
	audio_music_stop();

	/* (3) force + pick 201 (--cw-set's integer), timer re-armed to the
	   force lever's own value: hal1 is what reaches the voice. */
	assert(config_set_int(&gstate.config_user, "debug.music_pick", 201));
	audio_music_test_set_timer(music_timer_force_lever(0, true));
	assert(run_until_handoff(2) > 0);
	assert(audio_music_telemetry_key() == 2u);
	assert(strcmp(audio_music_current_key(), "hal") == 0);
	audio_music_stop();

	json_object_dotremove(json_object(gstate.config_user.root),
						  "debug.music_pick");
	json_object_dotremove(json_object(gstate.config_user.root),
						  "debug.music_force");
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_music_registry_reload();
	remove(test_fixture_join(s_fixture_PACK, "/music/calm1.ogg"));
	remove(test_fixture_join(s_fixture_PACK, "/music/hal1.ogg"));
	puts("  H: hardware config (music_force at init + --cw-set pick=201): "
		 "no-music pack = 0 hand-offs + music_force_no_tracks event; Beta "
		 "pack force alone starts within 2 ticks; force+pick starts hal OK");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* keep progress lines on an abort */
	test_fixture_dir("test_music_readahead_pack_tmp", s_fixture_PACK,
					 sizeof s_fixture_PACK);
	memset(&gstate, 0, sizeof(gstate));
	gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);

	test_pure_refill_helpers();

	/* HEADLESS (audio.enabled=off): the real decode worker runs, the voice
	   is never opened; the scripted device above stands in for it. */
	audio_init(false, 100);
	/* H: the hardware run's sd:/config.json {"debug":{"music_force":"on"}}
	   is in place BEFORE init, so init's real seeding path takes it. */
	assert(config_set_string(&gstate.config_user, "debug.music_force", "on"));
	audio_music_init(100);
	test_hw_config_starts_playback();
	/* The production ring is in effect by default (the 256KB allocation
	   succeeded) -- the policy setter accepts the full size. */
	assert(audio_music_test_set_ring_policy(MUSIC_RING_HALVES,
											MUSIC_RING_REFILL_LOW));

	equiv_file(FIXTURE, "fixture 12s stereo", true);
	test_wake_counts();
	test_rng_untouched();
	test_pick_lever();
	test_continuous_background();

	const char* real = getenv("CW_MUSIC_EQUIV_TRACK");
	FILE* rf = (real && real[0]) ? fopen(real, "rb") : NULL;
	if(rf) {
		fclose(rf);
		equiv_file(real, real, false);
		puts("  F: real Beta track equivalence OK");
	} else {
		puts("  F: SKIPPED (set CW_MUSIC_EQUIV_TRACK to an owned Beta .ogg to "
			 "run B/C on a real track)");
	}

	audio_music_shutdown();
	config_destroy(&gstate.config_user);
	test_fixture_dir_cleanup(s_fixture_PACK);
	puts("test_music_readahead_equiv: PASS");
	return 0;
}
