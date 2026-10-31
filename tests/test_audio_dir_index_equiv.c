/*
	sound-dir-index (docs/intent/sound-dir-index.md): the boot/reload sound
	scan lists each sound directory ONCE and opens only names the listing
	holds. This test proves that filter is bit-exact (T1): the SAME tree
	loaded through the per-file probe (index forced off,
	audio_registry_test_set_dir_index(false)) and through the listing gives
	an identical registry -- per key: variant count; per variant: frames,
	rate, arena OFFSET (so load order is covered) and every PCM byte -- and
	opens exactly the same files (open_hit_n and read_bytes equal per root),
	while the listing path issues no fopen for a name that is not there.

	Trees (each one is a separate pack root under the fixture; "assets" is
	the registry's literal bundled-default root, shared by all of them):
	  normal    every key present, bare/suffixed/two-digit variants, a .wav
	            and an .ogg for the same basename (wav wins, ogg never
	            opened), an ogg-only basename (real vorbis), a corrupt .wav
	            with a real .ogg sibling (wav opens+rejects, ogg loads)
	  missing   the pack root exists, but has no sound/ directory at all
	            (every key falls back to assets/)
	  partial   some sound/ subdirectories absent, some keys absent from a
	            directory that exists (per-key fallback, mixed)
	  cased     upper/mixed-case FILE and DIRECTORY names ("Grass1.WAV",
	            sound/STEP/). The rule is ASCII case-insensitive: that is
	            what fopen does on FAT (the Wii's libfat) and on a Windows
	            host; on a case-sensitive host the index admits a superset and
	            fopen decides, so the probe/listing equality holds everywhere.
	  odd       a DIRECTORY named like a candidate file ("x2.wav/"), and a
	            directory holding a non-ASCII name (that directory is not
	            indexed: dir_probe_n counts it, fopen decides as before)
	  deferred  "normal" again with audio.deferred_cave on (the probe the
	            deferred cave key runs goes through the same filter)

	Optional real-pack leg (not in ctest): CAVEWII_AUDIO_DIR_REAL_ROOT=<dir>
	plus CAVEWII_AUDIO_DIR_REAL_PACKS=<root1;root2;...> runs the same A/B on
	real packs relative to <dir> (whose assets/ is the fallback) and prints
	the reload wall time of each path.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
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
#include "platform/audio_wav.h"

extern void audio_registry_test_set_dir_index(bool on);
extern void audio_registry_test_scan_ctr(int root, unsigned long long out[7]);
extern void audio_registry_test_set_dir_read_error_after(int entries);

enum { C_MISS = 0, C_HIT, C_BYTES, C_DIR, C_ABSENT, C_PROBE, C_SKIP };

static char s_fixture[128];
static char s_srcroot[1024];

#define TEST_RATE 22050u
#define MAX_FRAMES 4096u

static uint8_t* s_ogg;
static bool s_deferred_on;
static size_t s_ogg_len;

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

static void mkparent(const char* path) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%.511s", path);
	char* slash = strrchr(dir, '/');
	assert(slash);
	*slash = '\0';
	mkdir_p(dir);
}

static int16_t s_pcm[MAX_FRAMES];
static uint8_t s_wav[wavh_write_size(MAX_FRAMES)];

static void write_wav(const char* path, uint32_t frames, uint32_t seed,
					  bool corrupt) {
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
	mkparent(path);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(s_wav, 1, n, f) == n);
	fclose(f);
}

static void write_bytes(const char* path, const uint8_t* b, size_t n) {
	mkparent(path);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(b, 1, n, f) == n);
	fclose(f);
}

static void write_junk(const char* path, uint32_t bytes) {
	mkparent(path);
	FILE* f = fopen(path, "wb");
	assert(f);
	for(uint32_t k = 0; k < bytes; k++)
		fputc((int)((k * 31u + 7u) & 0xFF), f);
	fclose(f);
}

/* "<root>/<sound/dir>/<base><v><ext>" with optional ASCII case games on the
   directory part (dir_upper) and the file part (file_mode: 0 as-is,
   1 = upper, 2 = first letter upper + upper extension). */
static void key_path(char* out, size_t n, const char* root, uint16_t id, int v,
					 const char* ext, bool dir_upper, int file_mode) {
	char rel[128];
	audio_sound_table_subpath(audio_sound_name(id), rel, sizeof(rel));
	char* slash = strrchr(rel, '/');
	assert(slash);
	*slash = '\0';
	char dir[128], file[128];
	snprintf(dir, sizeof(dir), "%s", rel);
	if(v == 0)
		snprintf(file, sizeof(file), "%s%s", slash + 1, ext);
	else
		snprintf(file, sizeof(file), "%s%d%s", slash + 1, v, ext);
	if(dir_upper)
		for(char* p = dir + sizeof("sound/") - 1; *p; p++)
			if(*p >= 'a' && *p <= 'z')
				*p = (char)(*p - 'a' + 'A');
	if(file_mode == 1) {
		for(char* p = file; *p; p++)
			if(*p >= 'a' && *p <= 'z')
				*p = (char)(*p - 'a' + 'A');
	} else if(file_mode == 2) {
		if(file[0] >= 'a' && file[0] <= 'z')
			file[0] = (char)(file[0] - 'a' + 'A');
		for(char* p = strrchr(file, '.'); p && *p; p++)
			if(*p >= 'a' && *p <= 'z')
				*p = (char)(*p - 'a' + 'A');
	}
	snprintf(out, n, "%s/%s/%s", root, dir, file);
}

static void put_wav(const char* root, uint16_t id, int v, uint32_t frames,
					bool corrupt) {
	char p[512];
	key_path(p, sizeof(p), root, id, v, ".wav", false, 0);
	write_wav(p, frames, (uint32_t)(id * 100 + v + (uint32_t)strlen(root)),
			  corrupt);
}

static void put_ogg(const char* root, uint16_t id, int v) {
	char p[512];
	key_path(p, sizeof(p), root, id, v, ".ogg", false, 0);
	write_bytes(p, s_ogg, s_ogg_len);
}

/* ===== the trees ======================================================== */

/* Files build_assets() wrote for the bundled-only Warm bell
   (menu.achievement). The loader ALWAYS scans that key from "assets" and
   charges the scan to the primary-root (index 0) counters, whatever pack is
   selected (audio_scan_one_key), so these hits show up on the pack side. */
static unsigned long long s_bundled_files;

static void build_assets(void) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(id % 7 == 3)
			continue; /* silent everywhere: nobody carries it */
		int nv = 1 + id % 2;
		if(id == SOUND_MENU_ACHIEVEMENT)
			s_bundled_files = (unsigned long long)nv;
		for(int k = 0; k < nv; k++)
			put_wav("assets", id, 1 + k, 500u + id * 13u + (uint32_t)k, false);
	}
}

static void build_normal(const char* root) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		uint32_t frames = 600u + (uint32_t)id * 37u;
		switch(id % 6) {
			case 0: /* bare + two-digit */
				put_wav(root, id, 0, frames, false);
				put_wav(root, id, 10, frames + 3, false);
				put_wav(root, id, 13, frames + 7, false);
				break;
			case 1: /* .wav AND .ogg for the same basename: wav wins */
				put_wav(root, id, 1, frames, false);
				put_ogg(root, id, 1);
				put_wav(root, id, 2, frames + 1, false);
				break;
			case 2: /* ogg-only basename (real vorbis) */
				put_ogg(root, id, 1);
				put_wav(root, id, 2, frames, false);
				break;
			case 3: /* corrupt wav, real ogg sibling; then a junk ogg */
				put_wav(root, id, 1, frames, true);
				put_ogg(root, id, 1);
				{
					char p[512];
					key_path(p, sizeof(p), root, id, 3, ".ogg", false, 0);
					write_junk(p, 333);
				}
				break;
			default: {
				int nv = 1 + id % 3;
				for(int k = 0; k < nv; k++)
					put_wav(root, id, 1 + k, frames + (uint32_t)k * 11u, false);
			}
		}
	}
}

static void build_partial(const char* root) {
	/* Keep only keys whose directory is NOT "sound/step" or "sound/random"
	   (those directories never exist here), and in the remaining ones skip
	   every third key (directory exists, key absent). */
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const char* n = audio_sound_name(id);
		if(strncmp(n, "step.", 5) == 0 || strncmp(n, "random.", 7) == 0)
			continue;
		if(id % 3 == 0)
			continue;
		put_wav(root, id, 1, 700u + id, false);
		if(id % 4 == 1)
			put_wav(root, id, 5, 710u + id, false);
	}
}

static void build_cased(const char* root) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const char* n = audio_sound_name(id);
		/* sound/STEP/: the whole directory upper-cased. */
		bool dir_upper = strncmp(n, "step.", 5) == 0;
		char p[512];
		key_path(p, sizeof(p), root, id, 1, ".wav", dir_upper, (int)(id % 3));
		write_wav(p, 650u + id, id, false);
		if(id % 5 == 0) {
			key_path(p, sizeof(p), root, id, 2, ".ogg", dir_upper, 1);
			write_bytes(p, s_ogg, s_ogg_len); /* "X2.OGG" */
		}
	}
}

static void build_odd(const char* root) {
	build_normal(root);
	/* A directory posing as a candidate file, in a listed directory: the
	   index admits the name, fopen refuses it on either path. */
	char p[512];
	key_path(p, sizeof(p), root, 0, 4, ".wav", false, 0);
	mkdir_p(p);
	/* A non-ASCII name in key 1's directory: that directory is not indexed. */
	key_path(p, sizeof(p), root, 1, 0, "", false, 0);
	char* slash = strrchr(p, '/');
	assert(slash);
	snprintf(slash + 1, sizeof(p) - (size_t)(slash + 1 - p),
			 "caf\xC3\xA9.wav");
	write_junk(p, 64);
}

/* ===== registry hash (test_audio_load_counters' definition, extended to
   the deferred key's pcm == NULL entries: frames + rate only) ============ */

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
	*total_variants = total;
	return h;
}

/* ===== one A/B ========================================================== */

struct scan_result {
	uint64_t hash;
	int variants;
	unsigned long long c[2][7];
	double ms;
};

static double now_ms(void) {
#ifdef _WIN32
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static void run_scan(const char* pack, bool index_on, struct scan_result* r) {
	assert(config_set_string(&gstate.config_user, "paths.texturepack", pack));
	/* resident scan: the opt-out of the 2026-10-21 deferred-cave default */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "off"));
	audio_registry_test_set_dir_index(index_on);
	double t0 = now_ms();
	audio_registry_reload();
	r->ms = now_ms() - t0;
	r->hash = registry_hash(&r->variants);
	audio_registry_test_scan_ctr(0, r->c[0]);
	audio_registry_test_scan_ctr(1, r->c[1]);
}

/* A/B one pack. `expect_zero_miss`: the listing path must issue no failed
   fopen at all (true for every tree without a directory-posing-as-file or
   an unindexed directory, on a case-insensitive host). Returns the pair. */
static void ab(const char* label, const char* pack, bool expect_zero_miss,
			   struct scan_result* probe, struct scan_result* idx) {
	run_scan(pack, false, probe);
	run_scan(pack, true, idx);
	printf("  %-9s probe: hash 0x%016llx v=%d miss=%llu+%llu hit=%llu+%llu "
		   "%.1f ms | index: hash 0x%016llx v=%d miss=%llu+%llu hit=%llu+%llu "
		   "dirs=%llu+%llu absent=%llu+%llu probe_dirs=%llu+%llu "
		   "skip=%llu+%llu %.1f ms\n",
		   label, (unsigned long long)probe->hash, probe->variants,
		   probe->c[0][C_MISS], probe->c[1][C_MISS], probe->c[0][C_HIT],
		   probe->c[1][C_HIT], probe->ms, (unsigned long long)idx->hash,
		   idx->variants, idx->c[0][C_MISS], idx->c[1][C_MISS],
		   idx->c[0][C_HIT], idx->c[1][C_HIT], idx->c[0][C_DIR],
		   idx->c[1][C_DIR], idx->c[0][C_ABSENT], idx->c[1][C_ABSENT],
		   idx->c[0][C_PROBE], idx->c[1][C_PROBE], idx->c[0][C_SKIP],
		   idx->c[1][C_SKIP], idx->ms);

	/* THE equivalence: byte-identical registry, same files opened. */
	assert(idx->hash == probe->hash);
	assert(idx->variants == probe->variants);
	for(int root = 0; root < 2; root++) {
		assert(idx->c[root][C_HIT] == probe->c[root][C_HIT]);
		assert(idx->c[root][C_BYTES] == probe->c[root][C_BYTES]);
		/* Every probe-path miss is either skipped by the index or still a
		   miss on the listing path -- never silently lost or gained. */
		/* The deferred cave probe (audio_deferred_probe_ex) opens outside
		   audio_file_read, so its probe-path misses were never counted,
		   while its index skips are: at most one key x 14 variants x 2
		   extensions extra, and only with audio.deferred_cave on. */
		unsigned long long lhs = idx->c[root][C_SKIP] + idx->c[root][C_MISS];
		if(s_deferred_on) {
			assert(lhs >= probe->c[root][C_MISS]);
			assert(lhs - probe->c[root][C_MISS]
				   <= 2ull * AUDIO_MAX_VARIANTS);
		} else {
			assert(lhs == probe->c[root][C_MISS]);
		}
		/* The forced-off path really was the per-file probe. */
		assert(probe->c[root][C_DIR] == 0 && probe->c[root][C_SKIP] == 0
			   && probe->c[root][C_ABSENT] == 0);
	}
	if(expect_zero_miss) {
		assert(idx->c[0][C_MISS] == 0);
		assert(idx->c[1][C_MISS] == 0);
	}
}

static bool host_case_insensitive(void) {
	FILE* f = fopen("CaseProbe.tmp", "wb");
	assert(f);
	fclose(f);
	f = fopen("caseprobe.TMP", "rb");
	bool ci = f != NULL;
	if(f)
		fclose(f);
	remove("CaseProbe.tmp");
	return ci;
}

static void real_packs(void) {
	const char* root = getenv("CAVEWII_AUDIO_DIR_REAL_ROOT");
	const char* packs = getenv("CAVEWII_AUDIO_DIR_REAL_PACKS");
	if(!root || !packs || !*root || !*packs)
		return;
	char here[1024];
	assert(GETCWD(here, sizeof(here)));
	assert(CHDIR(root) == 0);
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", packs);
	for(char* tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
		struct scan_result p, i;
		/* Warm both paths once so the OS file cache is equal for the timed
		   pair (PC only; the Wii's SD has no such cache). */
		run_scan(tok, false, &p);
		run_scan(tok, true, &i);
		ab(tok, tok, false, &p, &i);
		printf("  REAL %s: probe %.1f ms -> index %.1f ms, miss %llu -> %llu\n",
			   tok, p.ms, i.ms, p.c[0][C_MISS] + p.c[1][C_MISS],
			   i.c[0][C_MISS] + i.c[1][C_MISS]);
	}
	assert(CHDIR(here) == 0);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_audio_dir_index_equiv");
	assert(GETCWD(s_srcroot, sizeof(s_srcroot)));

	/* The real vorbis sample, read before leaving the source root. */
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

	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);

	test_fixture_dir("test_audio_dir_index_equiv_tmp", s_fixture,
					 sizeof(s_fixture));
	MKDIR(s_fixture);
	assert(CHDIR(s_fixture) == 0);

	bool ci = host_case_insensitive();
	printf("  host fopen is case-%s\n", ci ? "INSENSITIVE" : "sensitive");

	build_assets();
	build_normal("normal");
	mkdir_p("missing");
	build_partial("partial");
	build_cased("cased");
	build_odd("odd");

	/* Boot load through the (default-on) index, headless. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "normal"));
	/* resident scan: the opt-out of the 2026-10-21 deferred-cave default */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "off"));
	audio_init(false, 0);

	struct scan_result p, i;

	ab("normal", "normal", true, &p, &i);
	assert(i.c[0][C_DIR] > 0 && i.c[0][C_SKIP] > 0); /* anti-vacuity */
	/* Every key the fallback root was asked for came from a listed dir. */

	ab("missing", "missing", true, &p, &i);
	/* Nothing came from the absent pack root: the only primary-side hits are
	   the bundled bell's own "assets" files (see s_bundled_files). */
	assert(s_bundled_files > 0); /* the bell really is exercised */
	assert(i.c[0][C_HIT] == s_bundled_files && i.c[0][C_ABSENT] > 0);
	assert(i.c[1][C_HIT] > 0); /* all from assets/ */

	ab("partial", "partial", true, &p, &i);
	assert(i.c[0][C_ABSENT] >= 2); /* sound/step, sound/random */
	assert(i.c[0][C_HIT] > 0 && i.c[1][C_HIT] > 0);

	ab("assets", "assets", true, &p, &i);
	assert(i.c[1][C_HIT] == 0 && i.c[1][C_MISS] == 0); /* no fallback root */

	/* Case: on a case-insensitive host (FAT, NTFS: the Wii and Windows) the
	   listing admits exactly what fopen opens -- zero misses, and the cased
	   files really are found (the case games are not vacuous). On a
	   case-sensitive host fopen misses them either way and the equality
	   still holds. */
	ab("cased", "cased", ci, &p, &i);
	if(ci) {
		assert(i.c[0][C_HIT] >= (unsigned long long)AUDIO_SOUND_COUNT);
		assert(i.c[1][C_HIT] == 0); /* nothing fell back */
	}

	/* Odd: the posing directory is a miss on both paths; the non-ASCII
	   directory is left to the per-file probe. */
	ab("odd", "odd", false, &p, &i);
	assert(i.c[0][C_PROBE] >= 1);
	assert(i.c[0][C_MISS] > 0);
	assert(i.c[0][C_MISS] < p.c[0][C_MISS]);

	/* Deferred cave key: its probe goes through the same filter. */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave",
							 "on"));
	s_deferred_on = true;
	ab("deferred", "normal", true, &p, &i);
	s_deferred_on = false;
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave",
							 "off"));

	/* Mid-listing read error (Fable review MEDIUM): every listing fails
	   after 2 entries. A partial listing must not be trusted -- each such
	   directory falls back to the per-file probe, so the SAME files load
	   (same hash, hits, bytes as the probe path) and nothing is skipped as
	   "proven absent" in a directory that failed. Directories with <= 2
	   entries finish before the error and stay LISTED, which is correct. */
	{
		struct scan_result ref;
		run_scan("normal", false, &ref);
		audio_registry_test_set_dir_read_error_after(2);
		ab("readerr", "normal", false, &p, &i);
		audio_registry_test_set_dir_read_error_after(0);
		assert(i.hash == ref.hash);
		assert(i.c[0][C_PROBE] > 0);	   /* failed listings fell back */
		assert(i.c[0][C_MISS] > 0);		   /* ...and were really probed */
		assert(i.c[0][C_HIT] == ref.c[0][C_HIT]);
		assert(i.c[0][C_BYTES] == ref.c[0][C_BYTES]);
		/* Contrast: without the fallback a 2-entry partial listing would
		   have hidden files, so a lower hit count would show here. */
	}

	/* Leave the default (index on) behind for anything that runs after. */
	audio_registry_test_set_dir_index(true);

	assert(CHDIR(s_srcroot) == 0);
	test_fixture_dir_cleanup(s_fixture);

	real_packs();

	free(s_ogg);
	puts("test_audio_dir_index_equiv OK");
	return 0;
}
