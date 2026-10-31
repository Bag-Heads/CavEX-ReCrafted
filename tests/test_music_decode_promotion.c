/*
	test_music_decode_promotion -- G9 decode deadline promotion
	(audio_music_ring.h "G9 DECODE DEADLINE PROMOTION", docs/g9_tick_boost.md).

	The change only decides WHEN the real decode worker runs (priority 48 vs
	MUSIC_DECODE_PRIORITY_PROMOTED while fewer than MUSIC_DECODE_PROMOTE_BANK
	chunks are banked ahead of the playing half). This test drives the REAL
	audio_music.c worker + drain loop (CAVEWII_TESTING hooks, the
	test_music_readahead_equiv harness) and proves:

	  A. Pure goldens for music_decode_promote_due_n (bank 8, clamp to n-1,
	     never before the first chunk is handed over).
	  B. PCM bytes/order: promotion forced OFF vs forced ON, under a jitter
	     consumer and a DRAIN consumer that outruns the decoder (bank
	     collapses -> real promotions AND real underruns), passthrough and
	     48 kHz resampled, production 32/8 and legacy 4/1 rings: every
	     hand-off stream is byte-identical to the others and to an
	     independent whole-file stb_vorbis decode. ON runs really promoted
	     (promotions > 0, promoted chunks > 0); OFF runs never touched the
	     priority. Every run ends demoted (stats + last priority applied).
	  C. Contention/race: a CPU-hog thread plus a MAIN loop that randomly
	     restarts the track (track change), stops it, flips the promotion gate
	     every few frames and lets some runs reach EOF -- while the worker is
	     decoding/committing/demoting. Every segment's PCM is an exact PREFIX
	     of the reference (complete ones equal it): no stale chunk from a
	     stopped track, no reorder, no duplicate. At every quiescent point the
	     worker's flag is demoted, promotions == demotions, and the LAST
	     priority actually applied is the base priority. Bounded: a wedge
	     fails, never hangs.
	  D. Real gate: configure(true) with the tick boost enabled but NO local
	     owner (remote Beta MP / no local server) -> zero promotions; with a
	     registered local owner thread -> promotions; owner gone -> none.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "game/game_state.h"
#include "platform/audio.h"
#include "platform/audio_music.h"
#include "platform/audio_music_ring.h"
#include "platform/audio_resample.h"
#include "platform/thread.h"
#include "util.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#include <stb/stb_vorbis.c>
#pragma GCC diagnostic pop

/* CAVEWII_TESTING hooks in audio_music.c. */
void audio_music_test_set_pcm_sink(void (*sink)(const int16_t* pcm,
												uint32_t frames,
												uint32_t rate));
void audio_music_test_set_voice_playing(bool (*playing)(void));
void audio_music_test_set_device_rate(uint32_t rate);
bool audio_music_test_set_ring_policy(int halves, int refill_low);
void audio_music_test_set_promotion_gate(int forced);
void audio_music_test_set_priority_hook(void (*hook)(uint8_t priority));

#define CHUNK 4096u
#define FIXTURE "tests/fixtures/music_readahead/stream_stereo_12s.ogg"

/* ===== A: pure goldens ==================================================== */

static void test_pure(void) {
	bool r[MUSIC_RING_HALVES];
	const int n = MUSIC_RING_HALVES, bank = MUSIC_DECODE_PROMOTE_BANK;
	assert(bank == 8 && MUSIC_DECODE_PRIORITY_BASE == 48
		   && MUSIC_DECODE_PRIORITY_PROMOTED == 70);
	for(int h = 0; h < n; h++)
		r[h] = false;
	/* Nothing handed over yet: the first-chunk bank stays at 48. */
	assert(!music_decode_promote_due_n(r, n, -1, bank));
	/* Playing 3, nothing banked: due. */
	assert(music_decode_promote_due_n(r, n, 3, bank));
	/* 7 banked ahead (4..10): still due; the 8th (11) ends it. */
	for(int h = 4; h <= 10; h++)
		r[h] = true;
	assert(music_decode_promote_due_n(r, n, 3, bank));
	r[11] = true;
	assert(!music_decode_promote_due_n(r, n, 3, bank));
	/* A gap breaks the run: 4..10 + 12.. ready but 11 missing = 7 = due. */
	r[11] = false;
	for(int h = 12; h < n; h++)
		r[h] = true;
	assert(music_decode_promote_due_n(r, n, 3, bank));
	/* Wraparound: playing 28, 29..31,0..4 ready = 8 = not due. */
	for(int h = 0; h < n; h++)
		r[h] = false;
	for(int h = 29; h < n; h++)
		r[h] = true;
	for(int h = 0; h <= 4; h++)
		r[h] = true;
	assert(!music_decode_promote_due_n(r, n, 28, bank));
	r[4] = false;
	assert(music_decode_promote_due_n(r, n, 28, bank));
	/* Legacy 4-half fallback: bank clamps to 3 (fully banked = not due). */
	bool l[4] = {false, true, true, true};
	assert(!music_decode_promote_due_n(l, 4, 0, bank));
	l[3] = false;
	assert(music_decode_promote_due_n(l, 4, 0, bank));
	puts("  A: promote_due goldens (bank 8, gap breaks the run, wraparound, "
		 "4-half clamp to 3, never before the first hand-off) OK");
}

/* ===== capture + scripted device ========================================= */

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
	assert(rate == s_cap.rate);
	pcm_push(&s_cap, pcm, frames);
	s_cap.chunks++;
}

static int s_budget;
static bool dev_playing(void) {
	if(s_budget <= 0)
		return true;
	s_budget--;
	return false;
}

/* Priority hook: called from MAIN and from the worker. */
static struct thread_mutex s_hook_lock;
static unsigned s_hook_calls;
static unsigned s_hook_promoted_calls;
static uint8_t s_hook_last = MUSIC_DECODE_PRIORITY_BASE;
static void prio_hook(uint8_t p) {
	assert(p == MUSIC_DECODE_PRIORITY_BASE
		   || p == MUSIC_DECODE_PRIORITY_PROMOTED);
	thread_mutex_lock(&s_hook_lock);
	s_hook_calls++;
	if(p == MUSIC_DECODE_PRIORITY_PROMOTED)
		s_hook_promoted_calls++;
	s_hook_last = p;
	thread_mutex_unlock(&s_hook_lock);
}
static uint8_t hook_last(void) {
	thread_mutex_lock(&s_hook_lock);
	uint8_t p = s_hook_last;
	thread_mutex_unlock(&s_hook_lock);
	return p;
}

static uint32_t lcg(uint32_t* x) {
	*x = *x * 1664525u + 1013904223u;
	return *x >> 8;
}

/* Quiescent: let the worker finish whatever it was doing and assert the
   promotion state is fully unwound (flag, counters, real priority). */
static void assert_quiescent_demoted(const char* where) {
	struct audio_music_promotion_stats st;
	for(int ms = 0; ms < 2000; ms++) {
		audio_music_decode_promotion_snapshot(&st);
		if(!st.promoted && hook_last() == MUSIC_DECODE_PRIORITY_BASE
		   && st.promotions == st.demotions)
			break;
		thread_msleep(1);
	}
	audio_music_decode_promotion_snapshot(&st);
	if(st.promoted || st.promotions != st.demotions
	   || hook_last() != MUSIC_DECODE_PRIORITY_BASE) {
		fprintf(stderr,
				"%s: promoted=%d promotions=%u demotions=%u last_prio=%u\n",
				where, (int)st.promoted, st.promotions, st.demotions,
				(unsigned)hook_last());
		assert(0);
	}
}

enum sched { SCHED_JITTER, SCHED_DRAIN };

struct run_result {
	struct pcm_buf pcm;
	uint32_t promotions, promoted_chunks;
	unsigned underruns;
};

static void run_stream(const char* path, int halves, int low,
					   uint32_t device_rate, enum sched sched, int gate,
					   struct run_result* out) {
	assert(audio_music_test_set_ring_policy(halves, low));
	audio_music_test_set_device_rate(device_rate);
	audio_music_test_set_promotion_gate(gate);
	memset(&s_cap, 0, sizeof(s_cap));
	audio_music_test_set_pcm_sink(cap_sink);
	audio_music_test_set_voice_playing(dev_playing);
	struct audio_music_promotion_stats st0, st1;
	audio_music_decode_promotion_snapshot(&st0);
	unsigned hook0 = s_hook_calls;
	unsigned u0 = audio_music_underrun_count();

	audio_music_play_record(path);
	assert(audio_music_is_playing());
	uint32_t rng = 0x5eed1234u;
	unsigned long frame = 0;
	while(audio_music_is_playing()) {
		assert(frame < 4000000ul);
		if(sched == SCHED_DRAIN) {
			/* Outrun the decoder: up to 4 chunks per frame, no sleeps
			   except a rare 1 ms -- the bank collapses repeatedly. */
			s_budget = 4;
			audio_music_update();
			if((frame & 63u) == 0)
				thread_msleep(1);
		} else {
			uint32_t r = lcg(&rng);
			unsigned kind = r % 16u;
			if(kind < 2u) {
				s_budget = 0;
				audio_music_update();
				thread_msleep(2);
			} else if(kind < 12u) {
				s_budget = 1;
				audio_music_update();
				thread_yield();
			} else {
				s_budget = (int)((r >> 4) % 5u);
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
	assert_quiescent_demoted("run_stream end");
	audio_music_decode_promotion_snapshot(&st1);
	out->pcm = s_cap;
	memset(&s_cap, 0, sizeof(s_cap));
	out->promotions = st1.promotions - st0.promotions;
	out->promoted_chunks = st1.promoted_chunks - st0.promoted_chunks;
	out->underruns = audio_music_underrun_count() - u0;
	if(gate == 0)
		assert(out->promotions == 0 && s_hook_calls == hook0);
	audio_music_test_set_promotion_gate(-1);
}

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

static void assert_prefix(const struct pcm_buf* seg, const struct pcm_buf* ref,
						  const char* what) {
	if(seg->len > ref->len
	   || (seg->len && memcmp(seg->s, ref->s, seg->len * sizeof(int16_t)) != 0)) {
		size_t i = 0;
		size_t m = seg->len < ref->len ? seg->len : ref->len;
		while(i < m && seg->s[i] == ref->s[i])
			i++;
		fprintf(stderr, "NOT A PREFIX %s: len %zu (ref %zu), first diff %zu\n",
				what, seg->len, ref->len, i);
		assert(0);
	}
}

/* Whole reference through the resampler in 1000-frame pieces. */
static void resample_reference(const struct pcm_buf* ref, uint32_t src_rate,
							   uint32_t dev, struct pcm_buf* out) {
	struct audio_resample_state st;
	audio_resample_init(&st, src_rate, dev);
	size_t cap = (size_t)((uint64_t)ref->len * dev / src_rate) * 101 / 100 + 64;
	memset(out, 0, sizeof(*out));
	out->s = (int16_t*)malloc(cap * sizeof(int16_t));
	assert(out->s);
	size_t pos = 0;
	while(pos < ref->len) {
		uint32_t piece = (ref->len - pos) < 1000 ? (uint32_t)(ref->len - pos)
												 : 1000u;
		uint32_t off = 0;
		while(off < piece) {
			uint32_t used = 0;
			assert(out->len < cap);
			out->len += audio_resample_run(&st, ref->s + pos + off, piece - off,
										   &used, out->s + out->len,
										   (uint32_t)(cap - out->len));
			assert(used > 0);
			off += used;
		}
		pos += piece;
	}
}

/* ===== B: PCM equivalence OFF vs ON ====================================== */

static void test_pcm_equivalence(void) {
	struct pcm_buf ref, ref48;
	uint32_t src_rate = 0;
	decode_reference(FIXTURE, &ref, &src_rate);
	assert(ref.len > 0);
	resample_reference(&ref, src_rate, 48000u, &ref48);

	static const struct {
		int halves, low;
		uint32_t dev;
		enum sched sched;
		const char* name;
	} cases[] = {
		{MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, 0, SCHED_JITTER, "32/8 jitter"},
		{MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, 0, SCHED_DRAIN, "32/8 drain"},
		{MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, 48000u, SCHED_DRAIN,
		 "32/8 drain 48k"},
		{MUSIC_RING_HALVES_LEGACY, MUSIC_RING_REFILL_LOW_LEGACY, 0, SCHED_DRAIN,
		 "4/1 drain"},
		{MUSIC_RING_HALVES_LEGACY, MUSIC_RING_REFILL_LOW_LEGACY, 48000u,
		 SCHED_JITTER, "4/1 jitter 48k"},
	};
	uint32_t total_promotions = 0, total_chunks = 0;
	unsigned total_underruns = 0;
	for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct pcm_buf* want = cases[i].dev ? &ref48 : &ref;
		struct run_result off, on;
		run_stream(FIXTURE, cases[i].halves, cases[i].low, cases[i].dev,
				   cases[i].sched, 0, &off);
		run_stream(FIXTURE, cases[i].halves, cases[i].low, cases[i].dev,
				   cases[i].sched, 1, &on);
		assert_same(&off.pcm, want, "promotion OFF vs reference");
		assert_same(&on.pcm, want, "promotion ON vs reference");
		assert_same(&on.pcm, &off.pcm, "promotion ON vs OFF");
		assert(on.pcm.chunks == off.pcm.chunks);
		printf("    %-16s %zu frames/%u chunks; ON: %u promotions, %u "
			   "promoted chunks, %u underruns; OFF: 0 promotions, %u "
			   "underruns\n",
			   cases[i].name, on.pcm.len, on.pcm.chunks, on.promotions,
			   on.promoted_chunks, on.underruns, off.underruns);
		total_promotions += on.promotions;
		total_chunks += on.promoted_chunks;
		total_underruns += on.underruns;
		free(off.pcm.s);
		free(on.pcm.s);
	}
	/* The drain cases really exercised the new path. */
	assert(total_promotions > 0 && total_chunks > 0);
	assert(total_underruns > 0);
	free(ref.s);
	free(ref48.s);
	printf("  B: promotion OFF == ON == independent decode, byte-for-byte, "
		   "across 5 ring/consumer/rate cases (%u promotions, %u promoted "
		   "chunks, %u underruns exercised) OK\n",
		   total_promotions, total_chunks, total_underruns);
}

/* ===== C: contention / race ============================================== */

static volatile bool s_hog_stop;
static void* hog_main(void* arg) {
	(void)arg;
	volatile uint32_t x = 1;
	while(!s_hog_stop) {
		for(int k = 0; k < 20000; k++)
			x = x * 1103515245u + 12345u;
		thread_yield();
	}
	return NULL;
}

static void test_contention(void) {
	struct pcm_buf ref;
	uint32_t src_rate = 0;
	decode_reference(FIXTURE, &ref, &src_rate);
	assert(audio_music_test_set_ring_policy(MUSIC_RING_HALVES,
											MUSIC_RING_REFILL_LOW));
	audio_music_test_set_device_rate(0);
	audio_music_test_set_voice_playing(dev_playing);

	struct thread hog[2];
	s_hog_stop = false;
	for(int k = 0; k < 2; k++)
		assert(thread_create(&hog[k], hog_main, NULL, 32));

	uint32_t rng = 0xc0ffee11u;
	unsigned segments = 0, complete = 0, stops = 0, restarts = 0, flips = 0;
	struct audio_music_promotion_stats st0, st1;
	audio_music_decode_promotion_snapshot(&st0);
	for(int round = 0; round < 60; round++) {
		memset(&s_cap, 0, sizeof(s_cap));
		audio_music_test_set_pcm_sink(cap_sink);
		int gate = (int)(lcg(&rng) & 1u);
		audio_music_test_set_promotion_gate(gate);
		audio_music_play_record(FIXTURE);
		/* Some rounds play to EOF, the rest are cut short at a random
		   frame by a stop or by a restart (track change). */
		uint32_t mode = lcg(&rng) % 4u;
		unsigned long limit = 3ul + (unsigned long)(lcg(&rng) % 90u);
		unsigned long frame = 0;
		bool cut = false;
		while(audio_music_is_playing()) {
			assert(frame < 4000000ul);
			uint32_t r = lcg(&rng);
			s_budget = (int)(r % 5u);
			if((r >> 8) % 7u == 0) {
				gate ^= 1;
				audio_music_test_set_promotion_gate(gate);
				flips++;
			}
			audio_music_update();
			if((r >> 12) % 8u == 0)
				thread_msleep(1);
			else
				thread_yield();
			frame++;
			if(mode != 0 && frame == limit) {
				cut = true;
				break;
			}
		}
		audio_music_test_set_pcm_sink(NULL);
		if(cut && mode == 1) {
			audio_music_stop();
			stops++;
		} else if(cut) {
			/* Track change: the next round's play_record preempts it; stop
			   here only so this segment is closed before checking it. */
			audio_music_play_record(FIXTURE);
			audio_music_stop();
			restarts++;
		}
		assert(!audio_music_is_playing());
		assert_prefix(&s_cap, &ref, "contention segment");
		if(!cut) {
			assert_same(&s_cap, &ref, "contention complete segment");
			complete++;
		}
		segments++;
		free(s_cap.s);
		memset(&s_cap, 0, sizeof(s_cap));
		assert_quiescent_demoted("contention round");
	}
	s_hog_stop = true;
	for(int k = 0; k < 2; k++)
		thread_join(&hog[k]);
	audio_music_test_set_voice_playing(NULL);
	audio_music_test_set_promotion_gate(-1);
	audio_music_decode_promotion_snapshot(&st1);
	assert(complete > 0 && stops > 0 && restarts > 0);
	assert(st1.promotions > st0.promotions);
	free(ref.s);
	printf("  C: contention -- %u segments (%u complete, %u stops, %u track "
		   "changes, %u gate flips, %u promotions): every segment an exact "
		   "prefix of the reference, every quiescent point demoted OK\n",
		   segments, complete, stops, restarts, flips,
		   st1.promotions - st0.promotions);
}

/* ===== D: the real gate (local tick-boost owner) ========================== */

static struct thread_mutex s_owner_lock;
static struct thread_cond s_owner_cond;
static int s_owner_cmd; /* 1 register, 2 unregister + exit */
static bool s_owner_done;
static void* owner_main(void* arg) {
	(void)arg;
	thread_mutex_lock(&s_owner_lock);
	while(s_owner_cmd != 1)
		thread_cond_wait(&s_owner_cond, &s_owner_lock);
	thread_g9_tick_boost_local(true, 63);
	s_owner_done = true;
	thread_cond_broadcast(&s_owner_cond);
	while(s_owner_cmd != 2)
		thread_cond_wait(&s_owner_cond, &s_owner_lock);
	thread_g9_tick_boost_local(false, 63);
	s_owner_done = false;
	thread_cond_broadcast(&s_owner_cond);
	thread_mutex_unlock(&s_owner_lock);
	return NULL;
}
static void owner_command(int cmd, bool want_done) {
	thread_mutex_lock(&s_owner_lock);
	s_owner_cmd = cmd;
	thread_cond_broadcast(&s_owner_cond);
	while(s_owner_done != want_done)
		thread_cond_wait(&s_owner_cond, &s_owner_lock);
	thread_mutex_unlock(&s_owner_lock);
}

static uint32_t drain_promotions(void) {
	struct run_result r;
	run_stream(FIXTURE, MUSIC_RING_HALVES, MUSIC_RING_REFILL_LOW, 0,
			   SCHED_DRAIN, -1, &r);
	free(r.pcm.s);
	return r.promotions;
}

static void test_real_gate(void) {
	/* Default build: knob off -> never. */
	audio_music_decode_promotion_configure(false);
	assert(!thread_g9_tick_boost_local_present());
	assert(drain_promotions() == 0);
	/* Knob on, boost enabled, but no local owner (remote MP shape). */
	thread_g9_tick_boost_configure(true, 60);
	audio_music_decode_promotion_configure(true);
	assert(thread_g9_tick_boost_enabled());
	assert(!thread_g9_tick_boost_local_present());
	assert(drain_promotions() == 0);
	/* A local server thread owns the boost: promotion is live. */
	assert(thread_mutex_init(&s_owner_lock) && thread_cond_init(&s_owner_cond));
	struct thread owner;
	s_owner_cmd = 0;
	s_owner_done = false;
	assert(thread_create(&owner, owner_main, NULL, 63));
	owner_command(1, true);
	assert(thread_g9_tick_boost_local_present());
	uint32_t live = drain_promotions();
	assert(live > 0);
	/* Owner unregistered (server shut down before a remote connect). */
	owner_command(2, false);
	thread_join(&owner);
	assert(!thread_g9_tick_boost_local_present());
	assert(drain_promotions() == 0);
	audio_music_decode_promotion_configure(false);
	thread_g9_tick_boost_configure(false, 60);
	thread_cond_destroy(&s_owner_cond);
	thread_mutex_destroy(&s_owner_lock);
	printf("  D: real gate -- knob off 0, boost on without a local owner 0, "
		   "local owner %u promotions, owner gone 0 OK\n",
		   live);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&gstate, 0, sizeof(gstate));
	gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	assert(thread_mutex_init(&s_hook_lock));

	test_pure();

	audio_init(false, 100); /* headless: real worker, scripted device */
	audio_music_init(100);
	assert(audio_music_test_set_ring_policy(MUSIC_RING_HALVES,
											MUSIC_RING_REFILL_LOW));
	audio_music_test_set_priority_hook(prio_hook);

	test_pcm_equivalence();
	test_contention();
	test_real_gate();

	audio_music_test_set_priority_hook(NULL);
	audio_music_shutdown();
	thread_mutex_destroy(&s_hook_lock);
	config_destroy(&gstate.config_user);
	puts("test_music_decode_promotion: PASS");
	return 0;
}
