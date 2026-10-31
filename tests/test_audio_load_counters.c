/*
	boot-sound-measure (docs/intent/boot-sound-measure.md): the boot sound
	scan's per-phase COUNTERS, carried on the existing audio_load observer
	record, and proof that adding them changed nothing that was loaded.

	WHY. The real-Wii boot spends ~31 s in audio_registry_load before the
	title (audio_load ms=30,934..32,275 across 36 runs). ~2,200 fopen probes
	go out and most miss, 11.3 MB is read, and every WAV sample is byte-swapped
	into the arena -- but how the 31 s splits between those was never measured
	(tmp/jobs/next-gaps-0924/PACK-LOADING-RESEARCH-0926.md). "Measure first":
	the counters come before any optimisation claim.

	WHAT THIS PINS.
	  1. BYTE IDENTITY. A 64-bit FNV-1a over the whole loaded registry -- per
	     key: variant count; per variant: frames, rate, its OFFSET in the arena
	     (so the load order is covered too) and every PCM byte -- equals
	     K_REGISTRY_HASH, a constant captured from this very fixture against
	     the loader at d6c1cbda, BEFORE the counters existed (the test was
	     built with TEST_AUDIO_LOAD_BASELINE against the untouched
	     audio_registry.c and printed it). Counters are observation only.
	  2. EXACT COUNTS. The fixture knows every file it wrote, so the test
	     models the loader's probe order independently (per key: the pack
	     root, then -- only when the pack gave the key NOTHING -- the "assets"
	     root; per root: variants 0..13; per variant: .wav, then .ogg only if
	     the .wav did not load) and derives the expected open-miss / open-hit
	     counts and read bytes for BOTH roots. The record must match exactly.
	  3. TIME SANITY. The phase timers are non-overlapping slices of the scan,
	     so their sum can never exceed scan_us; file_max_us <= key_max_us <=
	     scan_us; the key named by key_max is a real table key.

	The fixture mixes every path the loader has: bare and suffixed variants,
	two-digit suffixes, a key only the "assets" root carries (fallback), a key
	nobody carries (silent), a corrupt 2-channel .wav (opens + reads, rejected,
	then an .ogg probe), and a junk .ogg (opens + reads, vorbis refuses it).
*/

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define CHDIR(p) _chdir(p)
#define GETCWD(b, n) _getcwd(b, n)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define CHDIR(p) chdir(p)
#define GETCWD(b, n) getcwd(b, n)
#endif

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "platform/audio.h"
#include "platform/audio_sound_table.h"
#include "platform/audio_wav.h" /* wavh_write: the fixture writer only */
#include "platform/mem2.h"

extern void audio_registry_test_set_observer(bool (*sink)(const char* line));
extern void audio_registry_test_set_dir_index(bool on);
extern void audio_registry_test_scan_ctr(int root, unsigned long long out[7]);

/* Captured at d6c1cbda (pre-counter loader) with -DTEST_AUDIO_LOAD_BASELINE.
   On a big-endian host the arena holds byte-swapped samples, so the constant
   is little-endian-host only (every PC CI leg).
   Re-pinned 0x288ff801d8e02857 -> 0x60b0f6c675eab2e0 for ONE change: the table
   gained its 80th key, menu.achievement (the bundled Warm bell, a1ff94544),
   which this fixture now writes to "assets" (2 variants) because the loader
   reads that key ONLY from the bundled root. Proven: with that key removed
   from the table (and nothing else) this fixture hashes to the old constant
   again (154 variants; 156 with the bell). */
#define K_REGISTRY_HASH UINT64_C(0x60b0f6c675eab2e0)

static char s_fixture[128];
static char s_srcroot[1024];

#define PACK_DIR "pack"
#define DEF_DIR "assets" /* the registry's literal fallback root */
#define TEST_RATE 22050u
#define MAX_FRAMES 4096u

enum { R_PACK = 0, R_DEF = 1, R_COUNT };

/* What the fixture put on disk, per root / key / variant. */
struct probe_file {
	bool wav;		/* a .wav exists */
	bool wav_ok;	/* ...and parses as PCM16 mono */
	uint32_t wav_bytes;
	bool ogg;		/* an .ogg exists (always junk here) */
	uint32_t ogg_bytes;
};
static struct probe_file s_disk[R_COUNT][AUDIO_SOUND_COUNT][AUDIO_MAX_VARIANTS];

static void mkdir_p(const char* path) {
	char buf[400];
	snprintf(buf, sizeof(buf), "%.399s", path);
	for(char* p = buf + 1; *p; p++) {
		if(*p == '/') {
			*p = '\0';
			MKDIR(buf);
			*p = '/';
		}
	}
	MKDIR(buf);
}

static const char* root_name(int r) {
	return r == R_PACK ? PACK_DIR : DEF_DIR;
}

/* "<root>/sound/<key as path><variant or nothing><ext>", dirs created. */
static void variant_path(char* out, size_t out_size, int r, uint16_t id,
						 int v, const char* ext) {
	char rel[128];
	audio_sound_table_subpath(audio_sound_name(id), rel, sizeof(rel));
	char dir[400];
	snprintf(dir, sizeof(dir), "%s/%.200s", root_name(r), rel);
	char* slash = strrchr(dir, '/');
	assert(slash);
	*slash = '\0';
	mkdir_p(dir);
	if(v == 0)
		snprintf(out, out_size, "%s/%.200s%s", root_name(r), rel, ext);
	else
		snprintf(out, out_size, "%s/%.200s%d%s", root_name(r), rel, v, ext);
}

static int16_t s_pcm[MAX_FRAMES];
static uint8_t s_wav[wavh_write_size(MAX_FRAMES)];

static uint32_t write_wav(const char* path, uint32_t frames, uint32_t seed,
						  bool corrupt) {
	assert(frames <= MAX_FRAMES);
	uint32_t x = seed * 2654435761u + 12345u;
	for(uint32_t k = 0; k < frames; k++) {
		x = x * 1103515245u + 12345u;
		s_pcm[k] = (int16_t)(x >> 16);
	}
	size_t n = wavh_write(s_wav, TEST_RATE, s_pcm, frames);
	if(corrupt) {
		s_wav[22] = 2; /* 2 channels: wavh_parse accepts mono only */
		s_wav[23] = 0;
	}
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(s_wav, 1, n, f) == n);
	fclose(f);
	return (uint32_t)n;
}

static uint32_t write_junk(const char* path, uint32_t bytes) {
	FILE* f = fopen(path, "wb");
	assert(f);
	for(uint32_t k = 0; k < bytes; k++)
		fputc((int)((k * 31u + 7u) & 0xFF), f);
	fclose(f);
	return bytes;
}

static void put_wav(int r, uint16_t id, int v, uint32_t frames, bool corrupt) {
	char path[512];
	variant_path(path, sizeof(path), r, id, v, ".wav");
	s_disk[r][id][v].wav = true;
	s_disk[r][id][v].wav_ok = !corrupt;
	s_disk[r][id][v].wav_bytes
		= write_wav(path, frames, (uint32_t)(r * 100000 + id * 100 + v), corrupt);
}

static void put_ogg(int r, uint16_t id, int v, uint32_t bytes) {
	char path[512];
	variant_path(path, sizeof(path), r, id, v, ".ogg");
	s_disk[r][id][v].ogg = true;
	s_disk[r][id][v].ogg_bytes = write_junk(path, bytes);
}

/* Special keys, chosen by table index so the fixture never depends on a
   particular name surviving in the table. */
#define K_FALLBACK 3  /* pack lacks it; "assets" has 2 variants */
#define K_SILENT 5	  /* nobody has it */
#define K_CORRUPT 7	  /* variant 1 is a 2-channel wav; variant 2 fine */
#define K_JUNK_OGG 9  /* variant 2 exists only as a junk .ogg */
#define K_TWO_DIGIT 11 /* variants 1, 10, 13 */
/* The Warm bell is bundled-only: audio_scan_one_key always scans it from the
   "assets" root, never the pack, and charges that scan to the PACK-side
   (primary root) counters -- there is no fallback step for it. */
#define K_BUNDLED SOUND_MENU_ACHIEVEMENT

static void fixture_build(void) {
	memset(s_disk, 0, sizeof(s_disk));
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		uint32_t frames = 600u + (uint32_t)id * 37u;
		switch(id) {
			case K_FALLBACK:
				put_wav(R_DEF, id, 1, 900, false);
				put_wav(R_DEF, id, 2, 950, false);
				break;
			case K_SILENT: break;
			case K_BUNDLED: {
				/* Same 1..3-variant shape as the default arm, but in the
				   only root the loader reads for this key. */
				int nv = 1 + id % 3;
				int first = (id % 5 == 0) ? 0 : 1;
				for(int k = 0; k < nv; k++)
					put_wav(R_DEF, id, first + k, frames + (uint32_t)k * 11u,
							false);
				break;
			}
			case K_CORRUPT:
				put_wav(R_PACK, id, 1, frames, true);
				put_wav(R_PACK, id, 2, frames + 5, false);
				break;
			case K_JUNK_OGG:
				put_wav(R_PACK, id, 1, frames, false);
				put_ogg(R_PACK, id, 2, 777);
				break;
			case K_TWO_DIGIT:
				put_wav(R_PACK, id, 1, frames, false);
				put_wav(R_PACK, id, 10, frames + 10, false);
				put_wav(R_PACK, id, 13, frames + 13, false);
				break;
			default: {
				/* 1..3 variants, every fifth key uses the bare base name. */
				int nv = 1 + id % 3;
				int first = (id % 5 == 0) ? 0 : 1;
				for(int k = 0; k < nv; k++)
					put_wav(R_PACK, id, first + k, frames + (uint32_t)k * 11u,
							false);
			}
		}
	}
	/* Extra default-root files for keys the pack DOES carry: never probed
	   (per-key precedence), so they must not move any counter. */
	put_wav(R_DEF, 0, 1, 700, false);
	put_wav(R_DEF, 1, 0, 700, false);
}

/* ===== the independent model of the probe order ======================== */

struct expect_ctr {
	unsigned miss_n, hit_n;
	unsigned long long read_bytes;
};

/* Returns how many variants the root yields for the key. */
static int model_root(int r, uint16_t id, struct expect_ctr* e) {
	int loaded = 0;
	for(int v = 0; v < AUDIO_MAX_VARIANTS; v++) {
		const struct probe_file* p = &s_disk[r][id][v];
		if(p->wav) {
			e->hit_n++;
			e->read_bytes += p->wav_bytes;
			if(p->wav_ok) {
				loaded++;
				continue; /* .wav loaded: no .ogg probe */
			}
		} else {
			e->miss_n++;
		}
		if(p->ogg) {
			e->hit_n++;
			e->read_bytes += p->ogg_bytes; /* junk: never loads */
		} else {
			e->miss_n++;
		}
	}
	return loaded;
}

/* One key, the loader's per-key root order, into the per-bucket totals. */
static void model_key(uint16_t id, struct expect_ctr e[R_COUNT]) {
	if(id == K_BUNDLED) {
		model_root(R_DEF, id, &e[R_PACK]); /* bundled root, primary bucket */
		return;
	}
	if(model_root(R_PACK, id, &e[R_PACK]) == 0)
		model_root(R_DEF, id, &e[R_DEF]);
}

/* ===== registry hash ==================================================== */

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
	int total = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
		for(int v = 0; v < audio_registry_variant_count(id); v++) {
			const struct audio_sample* s = audio_registry_variant(id, v);
			assert(s && s->pcm);
			if(!base || (const uint8_t*)s->pcm < base)
				base = (const uint8_t*)s->pcm;
		}
	uint64_t h = UINT64_C(0xcbf29ce484222325);
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		int32_t n = audio_registry_variant_count(id);
		h = fnv(h, &n, sizeof(n));
		for(int v = 0; v < n; v++) {
			const struct audio_sample* s = audio_registry_variant(id, v);
			uint32_t off = (uint32_t)((const uint8_t*)s->pcm - base);
			h = fnv(h, &s->frames, sizeof(s->frames));
			h = fnv(h, &s->rate, sizeof(s->rate));
			h = fnv(h, &off, sizeof(off));
			h = fnv(h, s->pcm, (size_t)s->frames * 2u);
			total++;
		}
	}
	*total_variants = total;
	return h;
}

/* ===== the observer record ============================================== */

static char s_line[2400];
static int s_lines;

static bool sink(const char* line) {
	snprintf(s_line, sizeof(s_line), "%s", line);
	s_lines++;
	return true;
}

#ifndef TEST_AUDIO_LOAD_BASELINE
/* The unsigned value of "key":<n> in the record; asserts the key exists. */
static unsigned long long field_u(const char* key) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char* p = strstr(s_line, pat);
	assert(p && "field missing from audio_load");
	p += strlen(pat);
	assert(*p >= '0' && *p <= '9');
	return strtoull(p, NULL, 10);
}

static void check_counters(void) {
	struct expect_ctr e[R_COUNT];
	memset(e, 0, sizeof(e));
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
		model_key(id, e);

	/* Anti-vacuity: the fixture must exercise both roots and both outcomes. */
	assert(e[R_PACK].miss_n > 0 && e[R_PACK].hit_n > 0);
	assert(e[R_DEF].miss_n > 0 && e[R_DEF].hit_n > 0);
	/* Every key probes 14 variants x (1 or 2) extensions per visited root. */
	assert(e[R_PACK].miss_n + e[R_PACK].hit_n
		   >= (unsigned)AUDIO_SOUND_COUNT * AUDIO_MAX_VARIANTS);

	assert(field_u("open_miss_n") == e[R_PACK].miss_n);
	assert(field_u("open_hit_n") == e[R_PACK].hit_n);
	assert(field_u("read_bytes") == e[R_PACK].read_bytes);
	assert(field_u("fb_open_miss_n") == e[R_DEF].miss_n);
	assert(field_u("fb_open_hit_n") == e[R_DEF].hit_n);
	assert(field_u("fb_read_bytes") == e[R_DEF].read_bytes);

	unsigned long long scan = field_u("scan_us");
	unsigned long long parts = field_u("open_miss_us") + field_u("open_hit_us")
		+ field_u("read_us") + field_u("conv_us") + field_u("fb_open_miss_us")
		+ field_u("fb_open_hit_us") + field_u("fb_read_us")
		+ field_u("fb_conv_us");
	assert(scan > 0);
	assert(parts <= scan);
	unsigned long long file_max = field_u("file_max_us");
	unsigned long long key_max = field_u("key_max_us");
	assert(file_max <= key_max);
	assert(key_max <= scan);

	/* key_max names a real table key. */
	const char* p = strstr(s_line, "\"key_max\":\"");
	assert(p);
	p += strlen("\"key_max\":\"");
	const char* q = strchr(p, '"');
	assert(q && q > p);
	bool found = false;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const char* n = audio_sound_name(id);
		if(strlen(n) == (size_t)(q - p) && strncmp(n, p, (size_t)(q - p)) == 0)
			found = true;
	}
	assert(found);

	printf("  C1: pack miss=%u hit=%u bytes=%llu | assets miss=%u hit=%u "
		   "bytes=%llu | parts %llu <= scan %llu us OK\n",
		   e[R_PACK].miss_n, e[R_PACK].hit_n, e[R_PACK].read_bytes,
		   e[R_DEF].miss_n, e[R_DEF].hit_n, e[R_DEF].read_bytes, parts, scan);
}
#endif

static unsigned s_service_calls;
static bool s_saw_loading;
static bool s_serviced_key[AUDIO_SOUND_COUNT];
static void service_scan(void* ctx) {
	assert(ctx == &s_service_calls);
	s_service_calls++;
	assert(audio_registry_ready_state() == AUDIO_REGISTRY_LOADING);
	s_saw_loading = true;
	uint16_t ids[AUDIO_SOUND_COUNT];
	extern int audio_registry_test_scan_order(uint16_t*, int);
	int n = audio_registry_test_scan_order(ids, AUDIO_SOUND_COUNT);
	if(n) s_serviced_key[ids[n - 1]] = true;
}

int main(int argc, char** argv) {
	puts("test_audio_load_counters");
	if(argc == 2 && strcmp(argv[1], "--no-arena") == 0) {
		/* The real first-caller-wins MEM2 seam latches an empty grant. */
		size_t granted = 1;
		assert(!mem2_audio_region(0, &granted) && granted == 0);
		audio_init(false, 0);
		assert(audio_registry_ready_state() == AUDIO_REGISTRY_DISABLED);
		assert(strstr(audio_registry_failure_reason(), "No PCM arena"));
		assert(!audio_registry_retry());
		audio_registry_reload();
		assert(audio_registry_ready_state() == AUDIO_REGISTRY_DISABLED);
		assert(strstr(audio_registry_failure_reason(), "No PCM arena"));
		puts("PASS no arena: DISABLED at boot and reload, no retry");
		return 0;
	}
	/* A fresh process is needed because the region is never freed/reset. */
	char command[2048];
	int n = snprintf(command, sizeof(command), "\"%s\" --no-arena", argv[0]);
	assert(n > 0 && (size_t)n < sizeof(command));
	assert(system(command) == 0);

	assert(GETCWD(s_srcroot, sizeof(s_srcroot)));
	test_fixture_dir("test_audio_load_counters_tmp", s_fixture,
					 sizeof(s_fixture));
	MKDIR(s_fixture);
	assert(CHDIR(s_fixture) == 0);

	fixture_build();
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 PACK_DIR));
	/* resident scan: the opt-out of the 2026-10-21 deferred-cave default */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "off"));

	/* sound-dir-index: the exact-count model below is the PER-FILE probe's,
	   so the boot load runs with the directory index forced off; the index
	   leg after it reloads the same tree through the listing. */
	audio_registry_test_set_dir_index(false);
	audio_registry_set_service(service_scan, &s_service_calls);
	audio_init(false, 0); /* headless: registry + arena, no device */
	assert(s_saw_loading && s_service_calls >= AUDIO_SOUND_COUNT);
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) assert(s_serviced_key[id]);
	assert(audio_registry_ready_state() == AUDIO_REGISTRY_READY);
	assert(!audio_registry_failure_reason()[0]);

	int total = 0;
	uint64_t h = registry_hash(&total);
	printf("  H1: registry hash 0x%016llx over %d variants\n",
		   (unsigned long long)h, total);
	assert(audio_registry_variant_count(K_SILENT) == 0);
	assert(audio_registry_variant_count(K_FALLBACK) == 2);
	assert(audio_registry_variant_count(K_CORRUPT) == 1);
	assert(audio_registry_variant_count(K_JUNK_OGG) == 1);
	assert(audio_registry_variant_count(K_TWO_DIGIT) == 3);
	assert(audio_registry_variant_count(K_BUNDLED) == 1 + K_BUNDLED % 3);

	s_lines = 0;
	audio_registry_test_set_observer(sink);
	audio_registry_emit_load_stats();
	assert(s_lines == 1);
	printf("  E1: %s\n", s_line);
	/* The pre-existing prefix is untouched: new fields go AFTER "ms". */
	assert(strncmp(s_line, "{\"event\":\"audio_load\",\"root\":\"pack\","
						   "\"variants\":",
				   strlen("{\"event\":\"audio_load\",\"root\":\"pack\","
						  "\"variants\":"))
		   == 0);
	assert(s_line[strlen(s_line) - 1] == '}');
	assert(strstr(s_line, "\"service_gap_max_us\":"));

#ifndef TEST_AUDIO_LOAD_BASELINE
	assert(h == K_REGISTRY_HASH);
	check_counters();

	/* sound-dir-index leg: the same tree reloaded through the listing must
	   hash to the SAME constant, open exactly the files the model says hit
	   (same bytes), and issue no failed fopen at all. */
	{
		struct expect_ctr e[R_COUNT];
		memset(e, 0, sizeof(e));
		for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
			model_key(id, e);
		audio_registry_test_set_dir_index(true);
		audio_registry_reload();
		int total2 = 0;
		uint64_t h2 = registry_hash(&total2);
		unsigned long long c[R_COUNT][7];
		audio_registry_test_scan_ctr(0, c[R_PACK]);
		audio_registry_test_scan_ctr(1, c[R_DEF]);
		printf("  I1: index hash 0x%016llx over %d variants; pack miss=%llu "
			   "hit=%llu skip=%llu dirs=%llu | assets miss=%llu hit=%llu "
			   "skip=%llu dirs=%llu\n",
			   (unsigned long long)h2, total2, c[R_PACK][0], c[R_PACK][1],
			   c[R_PACK][6], c[R_PACK][3], c[R_DEF][0], c[R_DEF][1],
			   c[R_DEF][6], c[R_DEF][3]);
		assert(h2 == K_REGISTRY_HASH);
		assert(total2 == total);
		for(int r = 0; r < R_COUNT; r++) {
			assert(c[r][0] == 0);				 /* open_miss_n */
			assert(c[r][1] == e[r].hit_n);		 /* open_hit_n */
			assert(c[r][2] == e[r].read_bytes); /* read_bytes */
			assert(c[r][6] == e[r].miss_n);		 /* every miss skipped */
			assert(c[r][3] > 0);				 /* dirs really listed */
		}
	}
#endif
	audio_registry_test_set_observer(NULL);

	/* All variants of a required bundled-only key disappear. A successful
	 * scan with optional gaps above was READY; this one is degraded by name. */
	for(int v = 0; v < AUDIO_MAX_VARIANTS; v++) {
		char path[512];
		variant_path(path, sizeof(path), R_DEF, K_BUNDLED, v, ".wav");
		remove(path);
	}
	assert(audio_registry_retry());
	assert(audio_registry_ready_state() == AUDIO_REGISTRY_DISABLED);
	assert(strstr(audio_registry_failure_reason(), audio_sound_name(K_BUNDLED)));
	assert(!audio_registry_pick(K_BUNDLED));
	assert(audio_registry_pick(SOUND_MENU_SELECT)); /* other keys still play */
	assert(!audio_registry_retry());
	fixture_build();
	audio_registry_reload(); /* an explicit pack reload may supply the key */
	assert(audio_registry_ready_state() == AUDIO_REGISTRY_READY);
	audio_registry_set_service(NULL, NULL);
	puts("PASS serviced scan: every key, LOADING -> READY, missing key DISABLED, reload READY");

	assert(CHDIR(s_srcroot) == 0);
	test_fixture_dir_cleanup(s_fixture);
	puts("test_audio_load_counters OK");
	return 0;
}
