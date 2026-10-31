/*
	C2 P2 -- REGISTRY-level contract test for the opt-in deferred cave key
	(config "audio.deferred_cave"), through the REAL three-pass scan.

	tests/test_audio_deferred.c drives the loader module in isolation, with
	the test itself playing both audio.c and the registry. This file is the
	other half: it runs the ACTUAL audio_registry_load/reload scan against a
	scratch pack on disk and asserts what the design contract
	(tmp/jobs/c2-audio-coverage-0917/design-p2-deferred.md, sections 1/2/6,
	Fable, ratified by Astra ruling 3) says the opt-in must do to it --
	including the one property no isolated test can reach: that the PCM the
	deferred loader puts in the scratch is byte-identical to what the RESIDENT
	loader would have decoded into the arena for the same file.

	Every expectation below is transcribed from that document, from
	source/platform/audio_deferred.h and from audio.h -- never from a run of
	the code under test (CODESTYLE 4.2, the poisoned-golden rule).

	Cases (one static void test_x per case, in main() order):
	  a optin_off       key ABSENT and key "off": 13 cave variants RESIDENT,
	                    module UNAVAILABLE, the two scans byte-identical.
	  b optin_on        13 deferred variants (pcm == NULL, probe frames/rate),
	                    audio_registry_deferred_source names the right file for
	                    each, module IDLE, every NON-cave key byte-identical to
	                    the OFF scan.
	  c equivalence     per variant: deferred scratch == the OFF scan's resident
	                    PCM, byte for byte, same frames and rate -- WAV and
	                    OGG, then again with the pool requested OUT of order
	                    (ogg -> wav -> ogg, so the one decoder block and the
	                    one scratch are both reused across a codec change).
	  h rng_baseline    200 consecutive audio_registry_pick draws give the SAME
	                    variant-index sequence resident and deferred.
	  g reload_loading  audio_registry_reload() with a load mid-file: CANCELLED,
	                    IDLE after the rescan, and the next load is correct.
	  d precedence      pack offers 0 cave files -> the DEFAULT pool is probed;
	                    pack offers 1 -> no default variant is mixed in.
	  e oversize        AUDIO_DEFERRED_MAX_BYTES is the ceiling, both sides of
	                    it, and an ineligible variant in the MIDDLE of the pool
	                    compacts the indices rather than leaving a hole.
	  f unavailable     the scratch cannot be reserved: the key is scanned
	                    RESIDENT and the arena is left EXACTLY as if nothing had
	                    been tried (design section 6's rollback).

	HERMETIC FIXTURE, the test_audio_registry_priority pattern: the registry's
	fallback root is the literal "assets" resolved against the working
	directory, so the test chdir()s into its own PID-unique directory and
	builds BOTH roots ("pack" and "assets") inside it. Nothing here depends on
	the repo's own assets/sound sizes.

	OGG. Two of the thirteen cave variants are the committed Vorbis fixtures
	(tests/fixtures/audio_deferred/README.md: synthetic sine tones made for
	this project, CC0 -- a mono 32kHz one and a stereo 44.1kHz one whose two
	channels carry DIFFERENT tones, so the average downmix is really
	exercised). They are COPIED into the pack (and default) pools at run time
	and never written to. Nothing about them is hard-coded here: the resident
	OFF scan decodes them with stb_vorbis_decode_memory and that result is the
	golden every deferred load is diffed against, so re-encoding a fixture
	cannot stale anything. They sit at indices OGG_MONO_INDEX / OGG_STEREO_INDEX
	-- neither is index 0 (case d2 keeps an all-WAV pool, which must report
	decoder_bytes == 0) nor OVERSIZE_INDEX (case e rewrites that file).
*/

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
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
#include "platform/audio_deferred.h"
#include "platform/audio_sound_table.h"
#include "platform/audio_wav.h" /* wavh_write: the fixture writer only */
#include "util.h"				/* struct random_gen: the deterministic reseed */

/* Test-only seams in audio_registry.c (CAVEWII_TESTING), the same
   extern-declare idiom tests/test_audio_registry_priority.c uses. */
extern void audio_registry_test_set_arena_cap(size_t bytes);
extern void audio_registry_test_arena_stats(unsigned* rejects,
											unsigned* protected_silent,
											unsigned* protected_short);
extern void audio_registry_test_set_observer(bool (*sink)(const char* line));

/* ===== fixture shape ==================================================== */

#define PACK_DIR "pack"
#define DEFAULT_DIR "assets" /* == AUDIO_DEFAULT_PACK, audio_registry.c:387 */

/* Every key but the cave pool gets ONE tiny file, so the whole byte budget
   below is the test's own arithmetic. */
#define ORD_FRAMES 32u
#define ORD_RATE 22050u
#define ALIGN32(n) (((n) + 31u) & ~(size_t)31u)
#define ORD_BYTES ALIGN32((size_t)ORD_FRAMES * 2u)

/* The cave pool: cave1..cave13, DISTINCT frame counts (so a variant can be
   identified from its resident sample alone) and distinct rates. */
#define CAVE_COUNT 13
#define CAVE_FRAMES(i) (9000u + (uint32_t)(i) * 500u)
#define CAVE_RATE(i) (16000u + (uint32_t)(i) * 500u)
/* First-sample signature: proves variant v really is the file cave(v+1) from
   the root the signature names, not merely "a file of the right length". */
#define CAVE_SIG_PACK(i) ((int16_t)(1000 + (i)))
#define CAVE_SIG_DEFAULT(i) ((int16_t)(2000 + (i)))
/* The variant case (e) makes oversize -- deliberately in the MIDDLE of the
   pool, so the case also proves the eligible indices COMPACT. */
#define OVERSIZE_INDEX 6
/* Both sides of the ceiling, exactly. */
#define CEILING_FRAMES (AUDIO_DEFERRED_MAX_BYTES / 2u)
/* An ogg fixture bigger than this would not fit the snapshot pool below. */
#define OGG_MAX_FIXTURE_FRAMES 120000u
/* Which two variants are the committed .ogg fixtures (see the header note):
   never 0 (case d2 needs an all-WAV pool) and never OVERSIZE_INDEX (case e
   overwrites that file), and not adjacent, so the pool really is mixed. */
#define OGG_MONO_INDEX 4
#define OGG_STEREO_INDEX 9
/* Case (c)'s out-of-order leg: ogg -> wav -> ogg, twice, so both the scratch
   and the single decoder block are reused across a change of codec. */
#define MIXED_ORDER_N 6

#define MAX_FIXTURE_FRAMES 16384u
#define PUMP_LIMIT 4096 /* bounded: a contract break must FAIL, never hang */
#define PICK_LIMIT 20000
#define RNG_SEED 0x5EED1234u
#define RNG_SEQUENCE 200

static char s_fixture[128];
static char s_srcroot[1024]; /* the CWD to come back to for cleanup */
/* The committed fixtures, as ABSOLUTE paths: resolved before the chdir into
   the hermetic fixture directory, and only ever read from. */
static char s_ogg_mono_src[1024];
static char s_ogg_stereo_src[1024];

/* Which cave variants are .ogg, and where each one is copied FROM. */
static bool s_cave_is_ogg[CAVE_COUNT];
static const char* s_cave_ogg_src[CAVE_COUNT];

/* What the scan must reserve for stb_vorbis's decoder state with BOTH oggs in
   the pool: the neediest variant's own requirement (audio_registry.c's
   max_decoder), rounded up to the DSP alignment. Derived from
   audio_deferred_probe_ex on the same files -- nothing outside the decoder
   can predict the number, so the test asks the same question the scan does
   rather than hard-coding an answer. */
static size_t s_decoder_cap;

/* Case (c)'s out-of-order request sequence, by variant index. */
static const int s_mixed_order[MIXED_ORDER_N]
	= { OGG_MONO_INDEX, 0, OGG_STEREO_INDEX, OGG_MONO_INDEX, 2,
		OGG_STEREO_INDEX };

static const float s_pos[3] = { 4.5f, 70.0f, -12.25f };

/* ===== fixture writing ================================================== */

static int16_t s_pcm[MAX_FIXTURE_FRAMES];
static uint8_t s_wav[wavh_write_size(MAX_FIXTURE_FRAMES)];

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

/* File I/O is kept OUT of assert() on purpose: an NDEBUG build would drop the
   call itself and silently produce no fixture (tests/test_audio_deferred.c's
   note on the assert-under-NDEBUG trap). */
static void write_bytes(const char* path, const uint8_t* data, size_t n) {
	FILE* f = fopen(path, "wb");
	size_t wrote;
	int closed;
	assert(f);
	wrote = fwrite(data, 1, n, f);
	closed = fclose(f);
	assert(wrote == n);
	assert(closed == 0);
}

static void write_wav(const char* path, uint32_t rate, uint32_t frames,
					  int16_t signature) {
	assert(frames > 0 && frames <= MAX_FIXTURE_FRAMES);
	s_pcm[0] = signature;
	for(uint32_t k = 1; k < frames; k++)
		s_pcm[k] = (int16_t)(k * 31u + 7u + (uint32_t)(uint16_t)signature);
	write_bytes(path, s_wav, wavh_write(s_wav, rate, s_pcm, frames));
}

/* The one fixture too big for the static buffers above (case e): built on the
   heap and released straight away. */
static void write_big_wav(const char* path, uint32_t rate, uint32_t frames) {
	size_t bytes = wavh_write_size(frames);
	int16_t* pcm = malloc((size_t)frames * 2u);
	uint8_t* buf = malloc(bytes);
	assert(pcm && buf);
	for(uint32_t k = 0; k < frames; k++)
		pcm[k] = (int16_t)(k * 13u + 5u);
	assert(wavh_write(buf, rate, pcm, frames) == bytes);
	write_bytes(path, buf, bytes);
	free(buf);
	free(pcm);
}

static void copy_file(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	FILE* out;
	uint8_t buf[8192];
	size_t n;
	assert(in);
	out = fopen(to, "wb");
	assert(out);
	while((n = fread(buf, 1, sizeof(buf), in)) > 0)
		assert(fwrite(buf, 1, n, out) == n);
	assert(fclose(in) == 0);
	assert(fclose(out) == 0);
}

/* "<root>/sound/<key with dots as slashes>.wav", directories created. */
static void key_path(char* out, size_t out_size, const char* root,
					 const char* key) {
	char rel[128];
	audio_sound_table_subpath(key, rel, sizeof(rel));
	snprintf(out, out_size, "%.100s/%.200s.wav", root, rel);
	char dir[400];
	snprintf(dir, sizeof(dir), "%.100s/%.200s", root, rel);
	char* slash = strrchr(dir, '/');
	assert(slash);
	*slash = '\0';
	mkdir_p(dir);
}

/* "<root>/sound/ambient/cave/cave<v+1><ext>" -- the deferred scan's own
   naming (audio_registry.c:730-734: v == 0 is the bare base, which this
   fixture deliberately never provides, so variant v is always file v+1). */
static void cave_path(char* out, size_t out_size, const char* root, int v) {
	char rel[128];
	audio_sound_table_subpath(audio_sound_name(SOUND_AMBIENT_CAVE), rel,
							  sizeof(rel));
	snprintf(out, out_size, "%.100s/%.200s%d%s", root, rel, v + 1,
			 s_cave_is_ogg[v] ? ".ogg" : ".wav");
	char dir[400];
	snprintf(dir, sizeof(dir), "%.100s/%.200s", root, rel);
	char* slash = strrchr(dir, '/');
	assert(slash);
	*slash = '\0';
	mkdir_p(dir);
}

static void write_cave_variant(const char* root, int v, bool is_default) {
	char path[400];
	cave_path(path, sizeof(path), root, v);
	if(s_cave_is_ogg[v])
		copy_file(s_cave_ogg_src[v], path);
	else
		write_wav(path, CAVE_RATE(v), CAVE_FRAMES(v),
				  is_default ? CAVE_SIG_DEFAULT(v) : CAVE_SIG_PACK(v));
}

static int16_t ord_sig(uint16_t id) {
	return (int16_t)(id * 7 + 11);
}

static void fixture_build(void) {
	char path[400];
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(id == SOUND_AMBIENT_CAVE)
			continue; /* the cave key gets its 13-file pool instead */
		const char* name = audio_sound_name(id);
		assert(name);
		/* The Warm bell (menu.achievement) is bundled-only: the scan always
		   reads it from the "assets" root and never from the pack
		   (audio_scan_one_key). A pack-only copy would leave a PROTECTED
		   key silent and trip the arena stats below, so it lives where the
		   loader looks; same one-file shape and signature as every key. */
		key_path(path, sizeof(path),
				 id == SOUND_MENU_ACHIEVEMENT ? DEFAULT_DIR : PACK_DIR, name);
		write_wav(path, ORD_RATE, ORD_FRAMES, ord_sig(id));
	}
	for(int v = 0; v < CAVE_COUNT; v++) {
		write_cave_variant(PACK_DIR, v, false);
		write_cave_variant(DEFAULT_DIR, v, true); /* case (d)'s fallback pool */
	}
}

static void config_point_at_pack(void) {
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 PACK_DIR));
}

static void set_optin(const char* value) {
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave",
							 value));
}

/* ===== the committed ogg fixtures (see the header note) ================= */

/* Resolve both fixtures against the source root (this must run BEFORE the
   chdir into the hermetic fixture directory), check the engine's own probe
   accepts each one, and record the decoder block the scan will have to
   reserve for the pair. A missing or unreadable fixture is a hard setup
   failure -- the ogg legs are never silently skipped.

   The probe calls are kept OUT of assert() on purpose: an NDEBUG build would
   drop the call itself and leave the sizes at zero (the assert-under-NDEBUG
   trap this file's fixture writers already guard against). */
static void ogg_fixtures_resolve(void) {
	uint32_t frames = 0, rate = 0;
	size_t mono_dec = 0, stereo_dec = 0;
	bool ok;

	snprintf(s_ogg_mono_src, sizeof(s_ogg_mono_src),
			 "%.900s/tests/fixtures/audio_deferred/cave_mono.ogg", s_srcroot);
	snprintf(s_ogg_stereo_src, sizeof(s_ogg_stereo_src),
			 "%.900s/tests/fixtures/audio_deferred/cave_stereo.ogg", s_srcroot);

	ok = audio_deferred_probe_ex(s_ogg_mono_src, true, &frames, &rate,
								 &mono_dec);
	assert(ok);
	assert(frames > 0 && rate > 0 && frames <= OGG_MAX_FIXTURE_FRAMES);
	ok = audio_deferred_probe_ex(s_ogg_stereo_src, true, &frames, &rate,
								 &stereo_dec);
	assert(ok);
	assert(frames > 0 && rate > 0 && frames <= OGG_MAX_FIXTURE_FRAMES);

	s_cave_is_ogg[OGG_MONO_INDEX] = true;
	s_cave_is_ogg[OGG_STEREO_INDEX] = true;
	s_cave_ogg_src[OGG_MONO_INDEX] = s_ogg_mono_src;
	s_cave_ogg_src[OGG_STEREO_INDEX] = s_ogg_stereo_src;

	s_decoder_cap = ALIGN32(mono_dec > stereo_dec ? mono_dec : stereo_dec);
	assert(s_decoder_cap > 0);
	assert(s_decoder_cap <= AUDIO_DEFERRED_MAX_DECODER_BYTES);
	printf("  ogg fixtures: variant %d = %s, variant %d = %s\n"
		   "  decoder block the scan must reserve for the pair: %u bytes\n",
		   OGG_MONO_INDEX, s_ogg_mono_src, OGG_STEREO_INDEX, s_ogg_stereo_src,
		   (unsigned)s_decoder_cap);
}

/* ===== registry snapshots =============================================== */

/* A whole registry state, captured through the PUBLIC query surface, so two
   scans can be compared key by key, variant by variant, byte by byte. */
#define SNAP_WORDS 280000u
#define NOPCM ((size_t)-1)

struct key_snap {
	int count;
	uint32_t frames[AUDIO_MAX_VARIANTS];
	uint32_t rate[AUDIO_MAX_VARIANTS];
	size_t off[AUDIO_MAX_VARIANTS]; /* into snap.pcm, or NOPCM when deferred */
};

struct snap {
	struct key_snap key[AUDIO_SOUND_COUNT];
	int16_t pcm[SNAP_WORDS];
	size_t used;
	bool cave_deferred;
};

static struct snap s_off;	/* the resident golden, kept for the whole run */
static struct snap s_alt;	/* transient comparand */
static struct snap s_alt2;	/* transient comparand (case f needs two) */

static void reseed_cosmetic(void) {
	struct random_gen* g = audio_cosmetic_rng();
	assert(g);
	g->seed = RNG_SEED;
	g->has_next_gaussian = false;
}

static size_t snap_store(struct snap* s, const int16_t* pcm, uint32_t frames) {
	size_t at = s->used;
	assert(at + frames <= SNAP_WORDS);
	memcpy(s->pcm + at, pcm, (size_t)frames * 2u);
	s->used += frames;
	return at;
}

static const int16_t* snap_pcm(const struct snap* s, uint16_t id, int v) {
	assert(s->key[id].off[v] != NOPCM);
	return s->pcm + s->key[id].off[v];
}

/* Which cave variant a picked sample IS. The registry hands out samples only
   through the RANDOM pick, so the variant has to be recovered from the sample
   itself: deferred samples carry their source (and therefore their index)
   through audio_registry_deferred_source, resident ones are identified by
   their (frames, rate) pair, which the fixture makes unique per variant. */
static int cave_variant_of(const struct audio_sample* smp,
						   const struct audio_deferred_source** out_src) {
	assert(smp);
	if(!smp->pcm) {
		int v = -1;
		const struct audio_deferred_source* src
			= audio_registry_deferred_source(SOUND_AMBIENT_CAVE, smp, &v);
		assert(src);
		assert(v >= 0 && v < audio_registry_variant_count(SOUND_AMBIENT_CAVE));
		if(out_src)
			*out_src = src;
		return v;
	}
	if(out_src)
		*out_src = NULL;
	int found = -1;
	int n = audio_registry_variant_count(SOUND_AMBIENT_CAVE);
	for(int v = 0; v < n; v++) {
		uint32_t frames = 0, rate = 0;
		assert(audio_registry_variant_info(SOUND_AMBIENT_CAVE, v, &frames,
										   &rate));
		if(frames == smp->frames && rate == smp->rate) {
			assert(found < 0); /* the fixture's frame counts ARE unique */
			found = v;
		}
	}
	assert(found >= 0);
	return found;
}

static void snapshot_take(struct snap* s) {
	s->used = 0;
	s->cave_deferred = false;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		struct key_snap* k = &s->key[id];
		memset(k, 0, sizeof(*k));
		k->count = audio_registry_variant_count(id);
		for(int v = 0; v < k->count; v++) {
			assert(audio_registry_variant_info(id, v, &k->frames[v],
											   &k->rate[v]));
			k->off[v] = NOPCM;
		}
		/* Past the last loaded variant the query must refuse. */
		assert(!audio_registry_variant_info(id, k->count, NULL, NULL));
		if(k->count == 0) {
			assert(audio_registry_pick(id) == NULL);
			continue;
		}
		if(id != SOUND_AMBIENT_CAVE) {
			/* Every other key has exactly one file in the fixture, so its one
			   pick is deterministic. */
			assert(k->count == 1);
			const struct audio_sample* smp = audio_registry_pick(id);
			assert(smp && smp->pcm);
			assert(smp->frames == k->frames[0] && smp->rate == k->rate[0]);
			assert(smp->pcm[0] == ord_sig(id));
			k->off[0] = snap_store(s, smp->pcm, smp->frames);
			continue;
		}
		/* The cave pool: collect every variant out of the random stream. */
		reseed_cosmetic();
		int seen = 0, tries = 0;
		bool got[AUDIO_MAX_VARIANTS];
		memset(got, 0, sizeof(got));
		while(seen < k->count) {
			const struct audio_sample* smp = audio_registry_pick(id);
			assert(smp);
			assert(++tries <= PICK_LIMIT);
			int v = cave_variant_of(smp, NULL);
			assert(smp->frames == k->frames[v] && smp->rate == k->rate[v]);
			if(got[v])
				continue;
			got[v] = true;
			seen++;
			if(smp->pcm)
				k->off[v] = snap_store(s, smp->pcm, smp->frames);
			else
				s->cave_deferred = true;
		}
	}
}

static void expect_same(const struct snap* a, const struct snap* b,
						bool include_cave, const char* label) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(!include_cave && id == SOUND_AMBIENT_CAVE)
			continue;
		const struct key_snap* ka = &a->key[id];
		const struct key_snap* kb = &b->key[id];
		assert(ka->count == kb->count);
		for(int v = 0; v < ka->count; v++) {
			assert(ka->frames[v] == kb->frames[v]);
			assert(ka->rate[v] == kb->rate[v]);
			assert((ka->off[v] == NOPCM) == (kb->off[v] == NOPCM));
			if(ka->off[v] == NOPCM)
				continue;
			assert(memcmp(a->pcm + ka->off[v], b->pcm + kb->off[v],
						  (size_t)ka->frames[v] * 2u)
				   == 0);
		}
	}
	printf("    %s: every key identical (count, frames, rate, PCM bytes)\n",
		   label);
}

/* ===== the deferred scan record ========================================= */

static char s_obs[8][512];
static int s_obs_n;

static bool observer_sink(const char* line) {
	if(s_obs_n < (int)(sizeof(s_obs) / sizeof(s_obs[0])))
		snprintf(s_obs[s_obs_n], sizeof(s_obs[0]), "%s", line);
	s_obs_n++;
	return true;
}

/* The module's record ring is BOUNDED and drops its oldest entry when it
   overflows (audio_deferred.h:187-189), so every request this file makes is
   reconciled ONE AT A TIME, right where it ends -- never batched up. */
static void expect_no_records(void) {
	struct audio_deferred_record r;
	assert(!audio_deferred_pop_record(&r));
}

/* `req_id` 0 means "the id was never handed back" (a request cancelled before
   it was ever ready); ids are monotonic from 1, so 0 can never collide. */
static void expect_record(enum audio_deferred_outcome want, uint32_t req_id,
						  int variant) {
	struct audio_deferred_record r;
	assert(audio_deferred_pop_record(&r));
	assert(r.outcome == want);
	assert(r.req_id > 0);
	if(req_id)
		assert(r.req_id == req_id);
	assert(r.sound_id == SOUND_AMBIENT_CAVE);
	assert(r.variant == variant);
	assert(!r.started);
	assert(r.start_delay_ms == -1); /* no voice ever ran */
	expect_no_records();			/* exactly one terminal record, no more */
}

/* The one scan fact the registry publishes about the opt-in
   (audio_registry.c:1373-1380). `active == false` with `unavailable == 1` is
   design section 6's fallback-to-resident; no line at all is the OFF path,
   which must not touch the module.

   Every field is pinned to an exact value the test derived for itself.
   `decoder_bytes` is stb_vorbis's own working-set requirement for the neediest
   ogg variant in the pool, which nothing outside the decoder can predict --
   so `want_decoder` is what ogg_fixtures_resolve() got by asking
   audio_deferred_probe_ex the same question the scan asks, and 0 says the pool
   is all-WAV, where the code states the answer must be EXACTLY 0. The stated
   rules are checked alongside it: a multiple of AUDIO_DEFERRED_ALIGN, and
   never past AUDIO_DEFERRED_MAX_DECODER_BYTES. */
static void expect_scan_record(const char* label, bool active, int eligible,
							   unsigned discovered, unsigned oversize,
							   unsigned unavailable, unsigned scratch_bytes,
							   size_t want_decoder) {
	/* The same drain also emits ONE pending terminal record, so insist the
	   ring is already empty rather than let this swallow one. */
	expect_no_records();
	s_obs_n = 0;
	audio_registry_test_set_observer(observer_sink);
	audio_registry_deferred_telemetry();
	audio_registry_test_set_observer(NULL);

	char want[384];
	int len = snprintf(want, sizeof(want),
					   "{\"event\":\"audio_deferred_scan\",\"active\":%s,"
					   "\"eligible\":%d,\"discovered\":%u,\"oversize\":%u,"
					   "\"unavailable\":%u,\"scratch_bytes\":%u,"
					   "\"decoder_bytes\":",
					   active ? "true" : "false", eligible, discovered,
					   oversize, unavailable, scratch_bytes);
	assert(len > 0 && (size_t)len < sizeof(want));
	int hit = -1;
	for(int i = 0; i < s_obs_n && i < (int)(sizeof(s_obs) / sizeof(s_obs[0]));
		i++)
		if(strncmp(s_obs[i], "{\"event\":\"audio_deferred_scan\"", 30) == 0)
			hit = i;
	assert(hit >= 0);
	if(strncmp(s_obs[hit], want, (size_t)len) != 0)
		printf("    %s MISMATCH\n      got  %s\n      want %s...\n", label,
			   s_obs[hit], want);
	assert(strncmp(s_obs[hit], want, (size_t)len) == 0);

	char* end = NULL;
	unsigned long decoder = strtoul(s_obs[hit] + len, &end, 10);
	assert(end != s_obs[hit] + len); /* a number really followed */
	assert(strcmp(end, "}") == 0);	 /* ...and NOTHING but the brace after it */
	if(want_decoder > 0) {
		assert(decoder > 0);
		assert(decoder % AUDIO_DEFERRED_ALIGN == 0);
		assert((size_t)decoder <= AUDIO_DEFERRED_MAX_DECODER_BYTES);
		assert((size_t)decoder == want_decoder);
	} else {
		assert(decoder == 0);
	}
	printf("    %s: %s\n", label, s_obs[hit]);
}

static void expect_no_scan_record(const char* label) {
	s_obs_n = 0;
	audio_registry_test_set_observer(observer_sink);
	audio_registry_deferred_telemetry();
	audio_registry_test_set_observer(NULL);
	for(int i = 0; i < s_obs_n && i < (int)(sizeof(s_obs) / sizeof(s_obs[0]));
		i++)
		assert(strncmp(s_obs[i], "{\"event\":\"audio_deferred_scan\"", 30)
			   != 0);
	printf("    %s: the OFF path emits no deferred scan record\n", label);
}

/* ===== deferred sources, and driving one load by hand =================== */

/* audio_registry_deferred_source for every variant, recovered through the
   pick stream (the only way in). Re-derived after every rescan. */
static struct audio_deferred_source s_src[AUDIO_MAX_VARIANTS];
static int s_src_n;

static void collect_sources(void) {
	int n = audio_registry_variant_count(SOUND_AMBIENT_CAVE);
	bool got[AUDIO_MAX_VARIANTS];
	int seen = 0, tries = 0;
	assert(n > 0 && n <= AUDIO_MAX_VARIANTS);
	memset(got, 0, sizeof(got));
	memset(s_src, 0, sizeof(s_src));
	s_src_n = n;
	reseed_cosmetic();
	while(seen < n) {
		const struct audio_deferred_source* src = NULL;
		const struct audio_sample* smp
			= audio_registry_pick(SOUND_AMBIENT_CAVE);
		assert(smp);
		assert(++tries <= PICK_LIMIT);
		/* The deferred contract: the sample is a PROBE, never PCM. */
		assert(smp->pcm == NULL);
		int v = cave_variant_of(smp, &src);
		assert(src);
		uint32_t frames = 0, rate = 0;
		assert(audio_registry_variant_info(SOUND_AMBIENT_CAVE, v, &frames,
										   &rate));
		assert(smp->frames == frames && smp->rate == rate);
		assert(src->frames == frames && src->rate == rate);
		/* The codec flag must agree with the file the scan actually resolved,
		   checked against that PATH rather than against s_cave_is_ogg[v]: the
		   pool index is not the file index once case (e) drops a variant from
		   the middle and the survivors compact. */
		{
			size_t plen = strlen(src->path);
			assert(plen > 4);
			assert(src->is_ogg == (strcmp(src->path + plen - 4, ".ogg") == 0));
		}
		if(!got[v]) {
			got[v] = true;
			seen++;
			s_src[v] = *src;
		} else {
			assert(memcmp(&s_src[v], src, sizeof(*src)) == 0);
		}
	}
}

/* Expect every collected source to name file cave(v+1) under `root`. */
static void expect_sources_under(const char* root, int count) {
	assert(s_src_n == count);
	for(int v = 0; v < count; v++) {
		char want[400];
		char rel[128];
		audio_sound_table_subpath(audio_sound_name(SOUND_AMBIENT_CAVE), rel,
								  sizeof(rel));
		snprintf(want, sizeof(want), "%.100s/%.200s%d%s", root, rel, v + 1,
				 s_cave_is_ogg[v] ? ".ogg" : ".wav");
		assert(strcmp(s_src[v].path, want) == 0);
	}
}

/* Load ONE variant through the module, by hand (no loader thread exists under
   CAVEWII_TESTING), and hand the "voice" back without starting it. */
static uint32_t deferred_load(int v, const int16_t** out_pcm,
							  uint32_t* out_frames, uint32_t* out_rate) {
	struct audio_deferred_ready rdy;
	int steps = 0;
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	assert(audio_deferred_request(&s_src[v], SOUND_AMBIENT_CAVE, v, s_pos,
								  0.75f, 1.25f));
	while(audio_deferred_state() != AUDIO_DEFERRED_READY) {
		audio_deferred_loader_step();
		assert(++steps <= PUMP_LIMIT);
	}
	memset(&rdy, 0, sizeof(rdy));
	assert(audio_deferred_take_ready(&rdy));
	assert(rdy.sound_id == SOUND_AMBIENT_CAVE && rdy.variant == v);
	assert(rdy.vol == 0.75f && rdy.pitch == 1.25f);
	assert(rdy.pos[0] == s_pos[0] && rdy.pos[1] == s_pos[1]
		   && rdy.pos[2] == s_pos[2]);
	*out_pcm = rdy.pcm;
	*out_frames = rdy.frames;
	*out_rate = rdy.rate;
	return rdy.req_id;
}

/* ===== a. the opt-in is OFF ============================================= */

static void test_a_optin_off(void) {
	puts("a_optin_off");
	/* a1: the explicit OPT-OUT ("off", the maintainer ruling 2026-10-21 made
	   "on" the default) -- the resident path a byte-identical OFF has to
	   hold for. */
	set_optin("off");
	audio_registry_reload();
	assert(audio_arena_total() == 12u * 1024u * 1024u);
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	snapshot_take(&s_off);
	assert(!s_off.cave_deferred);
	for(int v = 0; v < CAVE_COUNT; v++) {
		assert(s_off.key[SOUND_AMBIENT_CAVE].off[v] != NOPCM);
		if(s_cave_is_ogg[v])
			continue; /* frames/rate are the encoder's, not the fixture's */
		assert(s_off.key[SOUND_AMBIENT_CAVE].frames[v] == CAVE_FRAMES(v));
		assert(s_off.key[SOUND_AMBIENT_CAVE].rate[v] == CAVE_RATE(v));
		/* ...and it is the PACK's file, not the default pool's. */
		assert(snap_pcm(&s_off, SOUND_AMBIENT_CAVE, v)[0] == CAVE_SIG_PACK(v));
	}
	unsigned rejects = 1, silent = 1, shortfall = 1;
	audio_registry_test_arena_stats(&rejects, &silent, &shortfall);
	assert(rejects == 0 && silent == 0 && shortfall == 0);
	expect_no_scan_record("a1");
	printf("PASS a_optin_off_explicit (%d resident cave variants, module "
		   "UNAVAILABLE)\n",
		   CAVE_COUNT);

	/* a2: the key ABSENT from the config is the DEFAULT, which is now ON:
	   the cave key is probed, not decoded, and the module comes up. The
	   resident scan above stays the opt-out's, byte for byte. */
	json_object_dotremove(json_object(gstate.config_user.root),
						  "audio.deferred_cave");
	audio_registry_reload();
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	snapshot_take(&s_alt);
	assert(s_alt.cave_deferred);
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	printf("PASS a_optin_default_on (key absent == deferred ON, %d probed "
		   "variants)\n",
		   CAVE_COUNT);
	set_optin("off");
	audio_registry_reload();
}

/* ===== b. the opt-in is ON ============================================== */

static void test_b_optin_on(void) {
	puts("b_optin_on");
	set_optin("on");
	audio_registry_reload();

	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);

	/* Every variant: probed, not decoded -- and the probe agrees with the
	   resident scan's own frames/rate for the same file. */
	for(int v = 0; v < CAVE_COUNT; v++) {
		uint32_t frames = 0, rate = 0;
		assert(audio_registry_variant_info(SOUND_AMBIENT_CAVE, v, &frames,
										   &rate));
		assert(frames == s_off.key[SOUND_AMBIENT_CAVE].frames[v]);
		assert(rate == s_off.key[SOUND_AMBIENT_CAVE].rate[v]);
	}
	collect_sources();
	expect_sources_under(PACK_DIR, CAVE_COUNT);

	/* The scratch is sized to the LARGEST eligible variant, rounded up to the
	   DSP alignment (design section 2). */
	uint32_t max_frames = 0;
	for(int v = 0; v < CAVE_COUNT; v++)
		if(s_off.key[SOUND_AMBIENT_CAVE].frames[v] > max_frames)
			max_frames = s_off.key[SOUND_AMBIENT_CAVE].frames[v];
	expect_scan_record("b", true, CAVE_COUNT, CAVE_COUNT, 0, 0,
					   (unsigned)ALIGN32((size_t)max_frames * 2u),
					   s_decoder_cap);

	/* Nothing else moved: every NON-cave key is still resident and identical
	   to the OFF scan, which is what "the OFF path stays intact underneath"
	   means for a scan that now also reserves a scratch. */
	snapshot_take(&s_alt);
	assert(s_alt.cave_deferred);
	expect_same(&s_off, &s_alt, false, "b");
	unsigned rejects = 1, silent = 1, shortfall = 1;
	audio_registry_test_arena_stats(&rejects, &silent, &shortfall);
	assert(rejects == 0 && silent == 0 && shortfall == 0);
	printf("PASS b_optin_on (%d deferred variants, module IDLE, every other "
		   "key untouched)\n",
		   CAVE_COUNT);
}

/* ===== c. EQUIVALENCE =================================================== */

/* Load variant `v` through the module and require the scratch to equal the
   OFF scan's resident PCM for the same file, byte for byte. THE assertion
   this file exists for -- and, for the two ogg variants, the proof that the
   pull-API chunked decode plus the average downmix equals what
   stb_vorbis_decode_memory produced in one go. */
static void expect_equivalent(int v) {
	const int16_t* pcm = NULL;
	uint32_t frames = 0, rate = 0;
	uint32_t req = deferred_load(v, &pcm, &frames, &rate);

	assert(frames == s_off.key[SOUND_AMBIENT_CAVE].frames[v]);
	assert(rate == s_off.key[SOUND_AMBIENT_CAVE].rate[v]);
	assert(memcmp(pcm, snap_pcm(&s_off, SOUND_AMBIENT_CAVE, v),
				  (size_t)frames * 2u)
		   == 0);

	audio_deferred_note_not_started(req, AUDIO_DEFERRED_OUT_CULLED);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	expect_record(AUDIO_DEFERRED_OUT_CULLED, req, v);
}

static void test_c_equivalence(void) {
	int wav = 0, ogg = 0;
	puts("c_equivalence");
	for(int v = 0; v < CAVE_COUNT; v++) {
		expect_equivalent(v);
		if(s_cave_is_ogg[v])
			ogg++;
		else
			wav++;
	}
	assert(ogg == 2 && wav == CAVE_COUNT - 2);
	printf("    in pool order: %d WAV + %d OGG variants byte-identical to the "
		   "resident decode\n",
		   wav, ogg);

	/* ...and again in a MIXED order, with an ogg immediately before and after
	   a wav. One scratch and one decoder block serve every variant, so a load
	   that left either of them in a codec-specific state would show up here
	   and nowhere in the in-order pass above. */
	for(int k = 0; k < MIXED_ORDER_N; k++) {
		int v = s_mixed_order[k];
		assert(v >= 0 && v < CAVE_COUNT);
		expect_equivalent(v);
	}
	/* Anti-vacuity: the sequence really does change codec back and forth. */
	{
		int flips = 0;
		for(int k = 1; k < MIXED_ORDER_N; k++)
			if(s_cave_is_ogg[s_mixed_order[k]]
			   != s_cave_is_ogg[s_mixed_order[k - 1]])
				flips++;
		assert(flips >= 4);
	}
	printf("PASS c_equivalence (%d in-order + %d out-of-order loads, every one "
		   "byte-identical to the resident decode)\n",
		   CAVE_COUNT, MIXED_ORDER_N);
}

/* ===== h. the RNG baseline ============================================== */

static void pick_sequence(int* out) {
	reseed_cosmetic();
	for(int k = 0; k < RNG_SEQUENCE; k++) {
		const struct audio_sample* smp
			= audio_registry_pick(SOUND_AMBIENT_CAVE);
		assert(smp);
		out[k] = cave_variant_of(smp, NULL);
	}
}

static void test_h_rng_baseline(void) {
	static int resident[RNG_SEQUENCE];
	static int deferred[RNG_SEQUENCE];
	int distinct = 0;
	bool seen[AUDIO_MAX_VARIANTS];
	puts("h_rng_baseline");

	set_optin("off");
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	pick_sequence(resident);

	set_optin("on");
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	pick_sequence(deferred);

	/* Anti-vacuity: a sequence stuck on one variant would match trivially. */
	memset(seen, 0, sizeof(seen));
	for(int k = 0; k < RNG_SEQUENCE; k++)
		if(!seen[resident[k]]) {
			seen[resident[k]] = true;
			distinct++;
		}
	assert(distinct == CAVE_COUNT);
	assert(memcmp(resident, deferred, sizeof(resident)) == 0);
	printf("PASS h_rng_baseline (%d draws, %d distinct variants, identical "
		   "index sequence resident vs deferred)\n",
		   RNG_SEQUENCE, distinct);
}

/* ===== g. a reload while a load is in flight ============================ */

static void test_g_reload_while_loading(void) {
	int biggest = 0;
	puts("g_reload_while_loading");
	collect_sources();
	for(int v = 1; v < CAVE_COUNT; v++)
		if(s_off.key[SOUND_AMBIENT_CAVE].frames[v]
		   > s_off.key[SOUND_AMBIENT_CAVE].frames[biggest])
			biggest = v;
	/* The load must be genuinely MID-FILE when the reload lands, or the case
	   proves nothing: more than one chunk of PCM. */
	assert(s_off.key[SOUND_AMBIENT_CAVE].frames[biggest] * 2u
		   > AUDIO_DEFERRED_CHUNK_BYTES);

	assert(audio_deferred_request(&s_src[biggest], SOUND_AMBIENT_CAVE, biggest,
								  s_pos, 1.0f, 1.0f));
	assert(audio_deferred_loader_step()); /* open + probe */
	assert(audio_deferred_loader_step()); /* one chunk, more to come */
	assert(audio_deferred_state() == AUDIO_DEFERRED_LOADING);

	audio_registry_reload();

	/* audio_stop_all cancels the request, audio_deferred_quiesce takes the
	   scratch back, and the rescan re-attaches it (audio_registry.c:1106). */
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	expect_record(AUDIO_DEFERRED_OUT_CANCELLED, 0, biggest);
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);

	/* ...and a fresh request loads correctly afterwards. */
	collect_sources();
	{
		const int16_t* pcm = NULL;
		uint32_t frames = 0, rate = 0;
		uint32_t req = deferred_load(biggest, &pcm, &frames, &rate);
		assert(frames == s_off.key[SOUND_AMBIENT_CAVE].frames[biggest]);
		assert(rate == s_off.key[SOUND_AMBIENT_CAVE].rate[biggest]);
		assert(memcmp(pcm, snap_pcm(&s_off, SOUND_AMBIENT_CAVE, biggest),
					  (size_t)frames * 2u)
			   == 0);
		audio_deferred_note_not_started(req, AUDIO_DEFERRED_OUT_CULLED);
		expect_record(AUDIO_DEFERRED_OUT_CULLED, req, biggest);
	}
	printf("PASS g_reload_while_loading (variant %d cancelled mid-file, IDLE "
		   "after the rescan, next load byte-identical)\n",
		   biggest);
}

/* ===== d. pack-then-default precedence, per KEY ========================= */

static void test_d_precedence(void) {
	char path[400];
	puts("d_precedence");

	/* d1: the pack offers NOTHING -- the whole key comes from the default
	   pool, and every probed path points there. */
	for(int v = 0; v < CAVE_COUNT; v++) {
		cave_path(path, sizeof(path), PACK_DIR, v);
		assert(remove(path) == 0);
	}
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	collect_sources();
	expect_sources_under(DEFAULT_DIR, CAVE_COUNT);
	printf("    d1: 0 pack files -> all %d variants probed under \"%s\"\n",
		   CAVE_COUNT, DEFAULT_DIR);

	/* d2: the pack offers ONE -- and the default pool must not top it up.
	   (The resident rule, unchanged: a pool is never mixed across roots.) */
	write_cave_variant(PACK_DIR, 0, false);
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == 1);
	collect_sources();
	expect_sources_under(PACK_DIR, 1);
	/* An ALL-WAV pool: the scan must reserve no decoder state at all. */
	assert(!s_cave_is_ogg[0]);
	expect_scan_record("d2", true, 1, 1, 0, 0,
					   (unsigned)ALIGN32((size_t)s_src[0].frames * 2u), 0);
	printf("    d2: 1 pack file -> exactly 1 variant, no default mixed in\n");

	for(int v = 1; v < CAVE_COUNT; v++)
		write_cave_variant(PACK_DIR, v, false);
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	puts("PASS d_precedence");
}

/* ===== e. the AUDIO_DEFERRED_MAX_BYTES ceiling ========================== */

static void test_e_oversize(void) {
	char path[400];
	puts("e_oversize");
	assert(!s_cave_is_ogg[OVERSIZE_INDEX]); /* the fixture writes this one */
	cave_path(path, sizeof(path), PACK_DIR, OVERSIZE_INDEX);

	/* e1: EXACTLY at the ceiling is still eligible. */
	write_big_wav(path, 32000u, CEILING_FRAMES);
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	collect_sources();
	assert(s_src[OVERSIZE_INDEX].frames == CEILING_FRAMES);
	expect_scan_record("e1", true, CAVE_COUNT, CAVE_COUNT, 0, 0,
					   (unsigned)ALIGN32((size_t)CEILING_FRAMES * 2u),
					   s_decoder_cap);
	printf("    e1: %u frames == the ceiling -> still eligible\n",
		   (unsigned)CEILING_FRAMES);

	/* e2: one frame past it is NOT, it is COUNTED, and -- because the
	   ineligible file sits in the MIDDLE of the pool -- the surviving
	   variants compact rather than leaving a hole. */
	write_big_wav(path, 32000u, CEILING_FRAMES + 1u);
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT - 1);
	collect_sources();
	for(int v = 0; v < CAVE_COUNT - 1; v++) {
		int file = v < OVERSIZE_INDEX ? v : v + 1; /* the hole is closed */
		char want[400];
		char rel[128];
		audio_sound_table_subpath(audio_sound_name(SOUND_AMBIENT_CAVE), rel,
								  sizeof(rel));
		snprintf(want, sizeof(want), "%.100s/%.200s%d%s", PACK_DIR, rel,
				 file + 1, s_cave_is_ogg[file] ? ".ogg" : ".wav");
		assert(strcmp(s_src[v].path, want) == 0);
		assert(s_src[v].frames == s_off.key[SOUND_AMBIENT_CAVE].frames[file]);
		assert(s_src[v].rate == s_off.key[SOUND_AMBIENT_CAVE].rate[file]);
	}
	uint32_t max_frames = 0;
	for(int v = 0; v < CAVE_COUNT - 1; v++)
		if(s_src[v].frames > max_frames)
			max_frames = s_src[v].frames;
	/* The dropped variant is a WAV, so both oggs survive and the decoder
	   block is unchanged by the compaction. */
	expect_scan_record("e2", true, CAVE_COUNT - 1, CAVE_COUNT, 1, 0,
					   (unsigned)ALIGN32((size_t)max_frames * 2u),
					   s_decoder_cap);
	printf("    e2: %u frames -> ineligible, counted, indices compacted\n",
		   (unsigned)CEILING_FRAMES + 1u);

	write_cave_variant(PACK_DIR, OVERSIZE_INDEX, false);
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == CAVE_COUNT);
	puts("PASS e_oversize");
}

/* ===== f. the scratch cannot be reserved ================================ */

static void test_f_unavailable(void) {
	puts("f_unavailable");
	/* Room for the protected passes A and B and then ONE alignment unit less
	   than the scratch needs -- so the reservation is the only thing that
	   fails, by 32 bytes. */
	int protected_keys = 0;
	uint32_t max_frames = 0, min_frames = 0xFFFFFFFFu;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const char* n = audio_sound_name(id);
		if(strncmp(n, "menu.", 5) == 0 || strncmp(n, "note.", 5) == 0)
			protected_keys++;
	}
	for(int v = 0; v < CAVE_COUNT; v++) {
		uint32_t f = s_off.key[SOUND_AMBIENT_CAVE].frames[v];
		if(f > max_frames)
			max_frames = f;
		if(f < min_frames)
			min_frames = f;
	}
	size_t scratch = ALIGN32((size_t)max_frames * 2u);
	size_t cap = (size_t)protected_keys * ORD_BYTES + scratch
		- AUDIO_DEFERRED_ALIGN;
	/* ANTI-VACUITY: after the refused reservation pass C must still have room
	   for every ordinary key AHEAD of the cave key in table order plus at
	   least one cave variant, or "scanned RESIDENT" would be vacuous. Every
	   id below SOUND_AMBIENT_CAVE is an ordinary key (the protected ones sit
	   at the end of the table). */
	assert(scratch - AUDIO_DEFERRED_ALIGN
		   >= (size_t)SOUND_AMBIENT_CAVE * ORD_BYTES
			   + ALIGN32((size_t)min_frames * 2u));

	/* The reference: the SAME cap with the opt-in OFF. */
	set_optin("off");
	audio_registry_test_set_arena_cap(cap);
	audio_registry_reload();
	assert(audio_arena_total() == cap);
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	snapshot_take(&s_alt);
	assert(!s_alt.cave_deferred);
	unsigned off_rejects = 0, off_silent = 0, off_short = 0;
	audio_registry_test_arena_stats(&off_rejects, &off_silent, &off_short);
	size_t off_used = audio_arena_used();

	/* The same cap with the opt-in ON: the scratch cannot be had. */
	set_optin("on");
	audio_registry_reload();
	assert(audio_arena_total() == cap);
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	snapshot_take(&s_alt2);
	assert(!s_alt2.cave_deferred); /* scanned RESIDENT, the faithful fallback */
	assert(s_alt2.key[SOUND_AMBIENT_CAVE].count >= 1);
	assert(audio_registry_pick(SOUND_AMBIENT_CAVE)->pcm != NULL);
	expect_scan_record("f", false, 0, CAVE_COUNT, 0, 1, 0, 0);

	/* Design section 6: the bump AND the reject counter are rolled back, so
	   pass C sees the arena exactly as if nothing had been tried -- which is
	   checkable as "the whole scan is identical to the OFF one". */
	unsigned on_rejects = 0, on_silent = 0, on_short = 0;
	audio_registry_test_arena_stats(&on_rejects, &on_silent, &on_short);
	assert(on_rejects == off_rejects);
	assert(on_silent == off_silent && on_short == off_short);
	assert(on_silent == 0 && on_short == 0); /* the protected set is whole */
	assert(audio_arena_used() == off_used);
	expect_same(&s_alt, &s_alt2, true, "f");
	printf("PASS f_unavailable (%u-byte arena: scratch refused, cave scanned "
		   "resident with %d variant(s), arena byte-identical to the OFF "
		   "scan)\n",
		   (unsigned)cap, s_alt2.key[SOUND_AMBIENT_CAVE].count);

	/* Reversibility: lifting the squeeze reloads back to the golden. */
	audio_registry_test_set_arena_cap(0);
	set_optin("off");
	audio_registry_reload();
	assert(audio_arena_total() == 12u * 1024u * 1024u);
	snapshot_take(&s_alt);
	expect_same(&s_off, &s_alt, true, "f2");
	puts("PASS f_reversible");
}

/* ======================================================================== */

int main(void) {
	/* Unbuffered: an assert()/abort() must not swallow the PASS lines that say
	   how far the run got (Windows' _IOLBF is silently _IOFBF). */
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_audio_deferred_registry");

	assert(GETCWD(s_srcroot, sizeof(s_srcroot)));
	/* The committed ogg fixtures are resolved against the REPO root (the
	   test's WORKING_DIRECTORY) BEFORE the chdir into the hermetic fixture --
	   after it, relative paths would land inside the scratch pack. */
	ogg_fixtures_resolve();

	test_fixture_dir("test_audio_deferred_registry_tmp", s_fixture,
					 sizeof(s_fixture));
	MKDIR(s_fixture);
	/* Hermetic from here: "pack" AND the registry's literal "assets" fallback
	   root both resolve inside the fixture. */
	assert(CHDIR(s_fixture) == 0);

	fixture_build();
	config_point_at_pack();

	audio_init(false, 0); /* headless: registry + arena, no device */

	test_a_optin_off();
	test_b_optin_on();
	test_c_equivalence();
	test_h_rng_baseline();
	test_g_reload_while_loading();
	test_d_precedence();
	test_e_oversize();
	test_f_unavailable();

	/* Every request this file made reached exactly ONE terminal outcome, and
	   the two the registry path actually produces were both exercised (design
	   section 3's accounting rule, seen from the registry side). */
	{
		struct audio_deferred_counters c;
		uint32_t sum = 0;
		audio_deferred_counters(&c);
		for(int i = 0; i < AUDIO_DEFERRED_OUT_COUNT; i++)
			sum += c.outcome[i];
		assert(c.outcome[AUDIO_DEFERRED_OUT_NONE] == 0u);
		assert(c.requests > 0u && c.requests == sum);
		/* case (c): CAVE_COUNT in pool order + MIXED_ORDER_N out of order;
		   case (g): one more after the reload. */
		assert(c.outcome[AUDIO_DEFERRED_OUT_CULLED]
			   == (uint32_t)(CAVE_COUNT + MIXED_ORDER_N) + 1u);
		assert(c.outcome[AUDIO_DEFERRED_OUT_CANCELLED] == 1u);
		printf("PASS accounting (%u requests, all reconciled)\n", c.requests);
	}
	expect_no_records();

	audio_deferred_shutdown();
	assert(CHDIR(s_srcroot) == 0);
	test_fixture_dir_cleanup(s_fixture);
	puts("ALL PASS test_audio_deferred_registry");
	return 0;
}
