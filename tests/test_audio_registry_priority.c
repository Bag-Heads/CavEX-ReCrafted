/*
	C2 P1 (Astra ruling 1 D1): the approved UI sound package and the noteblock
	instruments load BEFORE any other key -- plus the review fixes on top of it.

	WHY. The PCM arena is a bump allocator with no eviction, so whatever the
	registry scans LAST is what a full arena drops. The menu.* keys sit at the
	very END of the id table, so with a real Beta 1.7.3 sound pack selected the
	files the arena refused were the eight maintainer-approved UI sounds plus
	note.bassattack -- measured on real hardware 2026-09-17 and re-derived
	per-file in tmp/jobs/c2-audio-coverage-0917/report.md (the pack asks 14.38MB
	of a 12,443,648-byte region). A pack that never intended to touch the UI
	silenced the whole UI.

	WHAT THIS PINS.
	  A. GOLDEN / ORDER-INDEPENDENCE. With a roomy arena the three-pass scan
	     resolves EXACTLY what a plain table-order walk did: every key keeps its
	     variant count AND the identical file. Checked against a golden list --
	     the fixture writes one file per key whose first PCM sample is a per-id
	     signature, so "same count" and "same file" are both checked by value.
	  B. PROTECTION. With an arena sized to hold the protected set plus exactly
	     three ordinary keys, all eight menu keys and all five note keys keep
	     their full variant set, the ordinary tail loses its, the reject counter
	     moves, and protected_silent/protected_short stay 0. Every fixture file
	     is the SAME size and the squeeze is asserted tighter than the table
	     distance to the first protected key, so the pre-change loader cannot
	     pass this: it would spend the whole arena before reaching menu.scroll.
	  C. SCAN ORDER, from the engine itself. audio_registry_test_scan_order() is
	     the Wii-visible witness (review MEDIUM-4); the PC-only stderr
	     diagnostic is kept as a secondary cross-check.
	  D. FINAL-COUNT SEMANTICS (review MEDIUM-5). protected_silent counts keys
	     that ended with ZERO variants for ANY reason; protected_short counts
	     keys that loaded fewer than their pool offered. So a protected key
	     whose pack file the arena refused but whose bundled default then loaded
	     whole is NEITHER (scenario S1), and a protected key whose only file is
	     unreadable is silent WITHOUT the arena being involved (scenario S2).
	  E. THE BOOT RECORD (review HIGH-1). audio_registry_emit_load_stats()
	     emits exactly one audio_load observer line, once.
	  F. REVERSIBILITY. Lifting each squeeze reloads byte-exactly back to A.

	HERMETIC FIXTURE. The registry's fallback root is the literal "assets",
	resolved against the working directory, so the test chdir()s into its own
	fixture directory and builds BOTH roots there: "pack" (every key) and
	"assets" (S1 fallback and built-in bell). Nothing here depends on the repo's own
	assets/sound sizes, and scenario S2 can have a protected key with no
	fallback at all -- impossible against the shipped defaults, which carry
	every menu and note key.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define CHDIR(p) _chdir(p)
#define GETCWD(b, n) _getcwd(b, n)
#define DUPFD(fd) _dup(fd)
#define DUPFD2(a, b) _dup2(a, b)
#define CLOSEFD(fd) _close(fd)
#define FILENO(f) _fileno(f)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define CHDIR(p) chdir(p)
#define GETCWD(b, n) getcwd(b, n)
#define DUPFD(fd) dup(fd)
#define DUPFD2(a, b) dup2(a, b)
#define CLOSEFD(fd) close(fd)
#define FILENO(f) fileno(f)
#endif

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "platform/audio.h"
#include "platform/audio_sound_table.h"
#include "platform/audio_wav.h" /* wavh_write: the fixture writer only */

/* Test-only seams in audio_registry.c (CAVEWII_TESTING), same extern-declare
   idiom test_pack_helper_audio.c uses for audio_registry_test_set_observer. */
extern void audio_registry_test_set_arena_cap(size_t bytes);
extern void audio_registry_test_arena_stats(unsigned* rejects,
											unsigned* protected_silent,
											unsigned* protected_short);
extern int audio_registry_test_scan_order(uint16_t* ids, int cap);
extern void audio_registry_test_set_observer(bool (*sink)(const char* line));

/* ===== fixture ========================================================== */

/* Scratch root, PID-suffixed. The "_tmp" in the base name is load-bearing:
   .gitignore covers test fixtures with a "test_", "_tmp" directory glob, so a
   crashed run cannot leave an UNTRACKED directory in the tree root. Everything
   the test writes lives inside it, including the captured stderr log. */
static char s_fixture[128];
static char s_srcroot[1024]; /* the CWD to come back to for cleanup */

#define PACK_DIR "pack"
#define LOG_FILE "stderr.txt"

#define TEST_RATE 22050u
/* UNIFORM file size in the pack, and that is load-bearing. The arena keeps
   trying after a refusal, so a small file slips into whatever crumbs a big one
   left behind -- give the protected keys tiny files and they would survive a
   plain table-order walk too, and this test would pass against the very loader
   it exists to reject. One size means the arena holds exactly N files and
   "which N" is purely the scan order. */
#define FILE_FRAMES 4096u
#define ALIGN32(n) (((n) + 31u) & ~31u)
#define FILE_BYTES ALIGN32(FILE_FRAMES * 2u)
/* The ONE bundled-default file the fixture ships (scenario S1), deliberately
   small enough to fit a crumb the pack file could not. */
#define FALLBACK_FRAMES 16u
#define FALLBACK_BYTES ALIGN32(FALLBACK_FRAMES * 2u)
/* How many ORDINARY keys the squeezed arena is sized to still hold. >0 so the
   test also proves pass C keeps table order among the survivors. */
#define ROOMY_KEYS 3

/* The nine maintainer-approved UI sounds, restated here on purpose: a test
   that derives the protected set from the code it guards would pass just as
   happily if that code stopped protecting anything. */
static const char* const k_approved_ui[] = {
	"menu.scroll", "menu.category", "menu.select",	  "menu.back",
	"menu.item",   "menu.type",		"menu.connected", "menu.attention", "menu.achievement"};
#define APPROVED_UI_COUNT (sizeof(k_approved_ui) / sizeof(k_approved_ui[0]))

/* S2's victim: a protected key whose pack file is made unreadable and which
   the fixture's "assets" root deliberately does NOT carry. */
#define BAD_KEY "menu.type"
/* S1's victim: the LAST protected key in scan order, so the squeeze can starve
   exactly it. The fixture's "assets" root carries this one. */
#define FALLBACK_KEY "note.bassattack"

static bool key_is_ui(const char* n) {
	return strncmp(n, "menu.", 5) == 0;
}

static bool key_is_note(const char* n) {
	return strncmp(n, "note.", 5) == 0;
}

static bool key_is_protected(const char* n) {
	return key_is_ui(n) || key_is_note(n);
}

/* The per-id signature the fixture stamps into sample 0 -- with one uniform
   file size this is what makes "the same key resolved the same FILE"
   checkable by value. */
static int16_t sig(uint16_t id) {
	return (int16_t)(id * 7 + 11);
}

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

static int16_t s_pcm[FILE_FRAMES];
static uint8_t s_wav[wavh_write_size(FILE_FRAMES)];

static void write_wav(const char* path, uint32_t frames, int16_t signature,
					  bool corrupt_channels) {
	s_pcm[0] = signature;
	for(uint32_t k = 1; k < frames; k++)
		s_pcm[k] = (int16_t)(k * 3);
	size_t n = wavh_write(s_wav, TEST_RATE, s_pcm, frames);
	if(corrupt_channels) {
		/* Canonical 44-byte header: the fmt chunk's channel count is at byte
		   22. Two channels is exactly what wavh_parse refuses (PCM16 MONO
		   only), so the file opens, is read, and is rejected BEFORE the arena
		   is ever asked for a byte -- which is the distinction scenario S2
		   exists to pin. */
		s_wav[22] = 2;
		s_wav[23] = 0;
	}
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(s_wav, 1, n, f) == n);
	fclose(f);
}

/* "<root>/sound/<key with dots as slashes>1.wav", directories created. */
static void key_path(char* out, size_t out_size, const char* root,
					 const char* key) {
	char rel[128];
	audio_sound_table_subpath(key, rel, sizeof(rel));
	snprintf(out, out_size, "%.100s/%.200s1.wav", root, rel);
	char dir[400];
	snprintf(dir, sizeof(dir), "%.100s/%.200s", root, rel);
	char* slash = strrchr(dir, '/');
	assert(slash);
	*slash = '\0';
	mkdir_p(dir);
}

/* One file per key in "pack"; the S1 fallback and built-in bell in "assets". Variant 1 only, so
   every key resolves to exactly one variant and the whole byte budget is the
   test's own arithmetic. */
static void fixture_build(void) {
	char path[400];
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const char* name = audio_sound_name(id);
		assert(name);
		key_path(path, sizeof(path), PACK_DIR, name);
		write_wav(path, FILE_FRAMES, sig(id), false);
		/* E2: Warm bell is built-in; keep this uniform-sized fixture
		 * hermetic and retain every arena/order/signature assertion. */
		if(id == SOUND_MENU_ACHIEVEMENT) {
			key_path(path, sizeof(path), "assets", name);
			write_wav(path, FILE_FRAMES, sig(id), false);
		}
	}
	/* The bundled-default root: S1 fallback besides the built-in bell, so S1 has a fallback to land on and
	   S2's victim provably has none. */
	key_path(path, sizeof(path), "assets", FALLBACK_KEY);
	write_wav(path, FALLBACK_FRAMES, (int16_t)-4242, false);
}

static void config_point_at_pack(void) {
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 PACK_DIR));
	/* resident scan: the opt-out of the 2026-10-21 deferred-cave default */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "off"));
}

/* ===== the protected set, as the TABLE spells it ======================== */

static uint16_t s_ui_ids[APPROVED_UI_COUNT];
static uint16_t s_note_ids[AUDIO_SOUND_COUNT];
static int s_note_count;
static int s_protected_count;
static int s_ordinary_count;
/* Table index of the FIRST protected key -- how many ordinary keys a plain
   table-order walk would have to fund before reaching the approved package at
   all. The anti-vacuity number (see test_starved_arena_keeps_the_protected_set). */
static int s_first_protected_index = -1;

static uint16_t key_id(const char* name) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
		if(strcmp(audio_sound_name(id), name) == 0)
			return id;
	assert(0 && "key not in the table");
	return 0;
}

static void test_protected_set_is_the_approved_package(void) {
	int ui_seen = 0;
	s_note_count = s_protected_count = s_ordinary_count = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const char* name = audio_sound_name(id);
		if(key_is_protected(name) && s_first_protected_index < 0)
			s_first_protected_index = (int)id;
		if(key_is_ui(name)) {
			/* Every menu.* key in the table must be one of the approved nine
			   -- a new UI sound joining the protected pass is a decision, not
			   a drift, and this is where it gets noticed. */
			assert(ui_seen < (int)APPROVED_UI_COUNT);
			assert(strcmp(name, k_approved_ui[ui_seen]) == 0);
			s_ui_ids[ui_seen++] = id;
		} else if(key_is_note(name)) {
			s_note_ids[s_note_count++] = id;
		} else {
			s_ordinary_count++;
		}
		if(key_is_protected(name))
			s_protected_count++;
	}
	assert(ui_seen == (int)APPROVED_UI_COUNT);
	assert(s_note_count > 0);
	assert(s_protected_count == ui_seen + s_note_count);
	/* The squeezes below only mean anything if there is a real tail to lose. */
	assert(s_ordinary_count > ROOMY_KEYS);
	/* S1 starves the LAST protected key in scan order; pin that it is the one
	   the fixture gave a fallback to. */
	assert(strcmp(audio_sound_name(s_note_ids[s_note_count - 1]), FALLBACK_KEY)
		   == 0);
	assert(key_is_protected(BAD_KEY));
	printf("  P0: protected set = %d approved UI + %d note keys, %d ordinary "
		   "OK\n",
		   ui_seen, s_note_count, s_ordinary_count);
}

/* ===== C: the scan order, from the ENGINE (Wii-visible) ================= */

static void assert_scan_order(const char* label) {
	uint16_t ids[AUDIO_SOUND_COUNT];
	int n = audio_registry_test_scan_order(ids, (int)AUDIO_SOUND_COUNT);
	assert(n == (int)AUDIO_SOUND_COUNT); /* every key visited exactly once */

	/* Pass A: the nine approved UI keys, in table order, first. */
	for(int i = 0; i < (int)APPROVED_UI_COUNT; i++)
		assert(strcmp(audio_sound_name(ids[i]), k_approved_ui[i]) == 0);
	/* Pass B: every note key, in table order, next. */
	for(int i = 0; i < s_note_count; i++)
		assert(ids[APPROVED_UI_COUNT + i] == s_note_ids[i]);
	/* Pass C: nothing protected after an ordinary key, and table order kept. */
	int prev = -1;
	for(int i = (int)APPROVED_UI_COUNT + s_note_count; i < n; i++) {
		assert(!key_is_protected(audio_sound_name(ids[i])));
		assert((int)ids[i] > prev);
		prev = (int)ids[i];
	}
	/* ...and no key appears twice. */
	bool seen[AUDIO_SOUND_COUNT];
	memset(seen, 0, sizeof(seen));
	for(int i = 0; i < n; i++) {
		assert(!seen[ids[i]]);
		seen[ids[i]] = true;
	}
	printf("  %s: scan order = %s .. %s | %s .. %s | %s .. (%d ordinary) OK\n",
		   label, audio_sound_name(ids[0]),
		   audio_sound_name(ids[APPROVED_UI_COUNT - 1]),
		   audio_sound_name(ids[APPROVED_UI_COUNT]),
		   audio_sound_name(ids[APPROVED_UI_COUNT + s_note_count - 1]),
		   audio_sound_name(ids[APPROVED_UI_COUNT + s_note_count]),
		   s_ordinary_count);
}

/* ===== A/F: the golden every roomy load must reproduce ================== */

static size_t golden_total_bytes(void) {
	return (size_t)AUDIO_SOUND_COUNT * FILE_BYTES;
}

static void assert_full_golden(const char* label) {
	unsigned rejects = 1, silent = 1, shortfall = 1;
	audio_registry_test_arena_stats(&rejects, &silent, &shortfall);
	assert(rejects == 0 && silent == 0 && shortfall == 0);

	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		assert(audio_registry_variant_count(id) == 1);
		uint32_t frames = 0, rate = 0;
		assert(audio_registry_variant_info(id, 0, &frames, &rate));
		assert(rate == TEST_RATE);
		assert(frames == FILE_FRAMES);
		/* ...and it is the SAME file the plain table-order walk resolved. */
		const struct audio_sample* smp = audio_registry_pick(id);
		assert(smp && smp->pcm && smp->pcm[0] == sig(id));
	}
	assert(audio_arena_used() == golden_total_bytes());
	printf("  %s: all %d keys resolve their own file, %u bytes, 0 rejects OK\n",
		   label, (int)AUDIO_SOUND_COUNT, (unsigned)audio_arena_used());
}

/* ===== B: the squeeze =================================================== */

/* Room for the protected set plus exactly ROOMY_KEYS ordinary keys, and not
   one byte more -- every pack file is the same size, so this arena holds
   exactly that many files and the ONLY question is which ones. */
static size_t squeeze_cap(void) {
	return (size_t)(s_protected_count + ROOMY_KEYS) * FILE_BYTES;
}

static void test_starved_arena_keeps_the_protected_set(void) {
	/* ANTI-VACUITY. This whole test is worthless unless the squeeze is tight
	   enough that a PLAIN table-order walk could not have reached the
	   protected set at all: the approved package sits at the END of the id
	   table, so a loader without the three passes would spend every one of
	   these bytes on ordinary keys and leave the menus silent. */
	int capacity_files = (int)(squeeze_cap() / FILE_BYTES);
	assert(s_first_protected_index > capacity_files);

	audio_registry_test_set_arena_cap(squeeze_cap());
	audio_registry_reload();
	assert(audio_arena_total() == squeeze_cap());

	/* Every protected key is whole -- count, rate, length AND file. */
	for(int i = 0; i < (int)APPROVED_UI_COUNT; i++) {
		uint16_t id = s_ui_ids[i];
		assert(audio_registry_variant_count(id) == 1);
		const struct audio_sample* smp = audio_registry_pick(id);
		assert(smp && smp->frames == FILE_FRAMES && smp->rate == TEST_RATE);
		assert(smp->pcm[0] == sig(id));
	}
	for(int i = 0; i < s_note_count; i++) {
		uint16_t id = s_note_ids[i];
		assert(audio_registry_variant_count(id) == 1);
		const struct audio_sample* smp = audio_registry_pick(id);
		assert(smp && smp->frames == FILE_FRAMES && smp->pcm[0] == sig(id));
	}

	/* ...and the loss landed on the ordinary tail instead. The survivors are
	   the FIRST ROOMY_KEYS ordinary keys in table order: pass C does not
	   reorder within itself. */
	int loaded = 0, silent_keys = 0, ordinary_seen = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const char* name = audio_sound_name(id);
		if(key_is_protected(name))
			continue;
		if(audio_registry_variant_count(id) > 0) {
			loaded++;
			assert(ordinary_seen < ROOMY_KEYS); /* in order, no leapfrogging */
			const struct audio_sample* smp = audio_registry_pick(id);
			assert(smp && smp->frames == FILE_FRAMES && smp->pcm[0] == sig(id));
		} else {
			silent_keys++;
		}
		ordinary_seen++;
	}
	assert(loaded == ROOMY_KEYS);
	assert(silent_keys >= 1);
	assert(silent_keys == s_ordinary_count - ROOMY_KEYS);

	/* The remaining losses are still REPORTED -- and reported as not having
	   touched the protected set. */
	unsigned rejects = 0, silent = 1, shortfall = 1;
	audio_registry_test_arena_stats(&rejects, &silent, &shortfall);
	assert(rejects > 0);
	assert(silent == 0 && shortfall == 0);
	/* Full to the byte: nothing the tail asked for could have fit. */
	assert(audio_arena_used() == squeeze_cap());
	assert_scan_order("P1b");

	printf("  P1: %u-byte arena -> %d/%d protected keys whole, %d ordinary "
		   "silent, %u rejects, silent=%u short=%u OK\n",
		   (unsigned)squeeze_cap(), s_protected_count, s_protected_count,
		   silent_keys, rejects, silent, shortfall);
}

/* ===== D (review MEDIUM-5): the two counters mean FINAL counts =========== */

/* S1: a protected key whose PACK file the arena refuses, but whose bundled
   default then loads whole. It is neither silent nor short -- the pool that
   supplies the key is the one the comparison must be made against. */
static void test_rejected_pack_file_with_working_fallback(void) {
	uint16_t victim = key_id(FALLBACK_KEY);
	/* Room for every protected key but the LAST one at pack size, plus exactly
	   the small bundled default that last key falls back to. */
	size_t cap = (size_t)(s_protected_count - 1) * FILE_BYTES + FALLBACK_BYTES;
	audio_registry_test_set_arena_cap(cap);
	audio_registry_reload();
	assert(audio_arena_total() == cap);

	/* The victim speaks -- from the DEFAULT root, not the pack. */
	assert(audio_registry_variant_count(victim) == 1);
	uint32_t frames = 0, rate = 0;
	assert(audio_registry_variant_info(victim, 0, &frames, &rate));
	assert(frames == FALLBACK_FRAMES);
	const struct audio_sample* smp = audio_registry_pick(victim);
	assert(smp && smp->pcm[0] == (int16_t)-4242); /* the fallback file */

	/* Every other protected key is whole, including the built-in bell. */
	for(int i = 0; i < (int)APPROVED_UI_COUNT; i++)
		assert(audio_registry_variant_count(s_ui_ids[i]) == 1);
	for(int i = 0; i < s_note_count - 1; i++)
		assert(audio_registry_variant_count(s_note_ids[i]) == 1);

	unsigned rejects = 0, silent = 1, shortfall = 1;
	audio_registry_test_arena_stats(&rejects, &silent, &shortfall);
	assert(rejects > 0);	 /* the pack file WAS refused ... */
	assert(silent == 0);	 /* ... but the key is not silent ... */
	assert(shortfall == 0);	 /* ... and its pool loaded whole. */
	assert(audio_arena_used() == cap);
	printf("  S1: %s pack file refused, bundled default loaded whole -> "
		   "silent=%u short=%u (rejects=%u) OK\n",
		   FALLBACK_KEY, silent, shortfall, rejects);
}

/* S2: a protected key whose only file is not PCM16 mono. It never reaches the
   arena, so it is silent with the reject counter untouched -- the case the old
   reject-bracketing counter could not see at all. */
static void test_unreadable_protected_file_is_silent(void) {
	uint16_t victim = key_id(BAD_KEY);
	char path[400];
	key_path(path, sizeof(path), PACK_DIR, BAD_KEY);
	write_wav(path, FILE_FRAMES, sig(victim), true); /* stereo header */

	audio_registry_test_set_arena_cap(0); /* roomy: the arena is NOT the cause */
	audio_registry_reload();
	assert(audio_arena_total() == 12u * 1024u * 1024u);

	assert(audio_registry_variant_count(victim) == 0);
	assert(audio_registry_pick(victim) == NULL);

	unsigned rejects = 1, silent = 0, shortfall = 1;
	audio_registry_test_arena_stats(&rejects, &silent, &shortfall);
	assert(rejects == 0); /* the arena was never asked */
	assert(silent == 1);
	assert(shortfall == 0);
	/* Exactly one key is missing from the golden fill. */
	assert(audio_arena_used() == golden_total_bytes() - FILE_BYTES);
	printf("  S2: %s unreadable -> silent=%u with rejects=%u (arena "
		   "uninvolved) OK\n",
		   BAD_KEY, silent, rejects);

	/* Put it back and prove the registry recovers completely. */
	write_wav(path, FILE_FRAMES, sig(victim), false);
	audio_registry_reload();
	assert_full_golden("S2b");
}

/* ===== E (review HIGH-1): the boot record ================================ */

static char s_obs[4][2304]; /* hw_telemetry_emit_observer accepts <= 2303 */
static int s_obs_n;

static bool observer_sink(const char* line) {
	if(s_obs_n < (int)(sizeof(s_obs) / sizeof(s_obs[0])))
		snprintf(s_obs[s_obs_n], sizeof(s_obs[0]), "%s", line);
	s_obs_n++;
	return true;
}

static void test_boot_load_record(void) {
	/* The stash holds the BOOT scan's numbers: a roomy arena, every key from
	   the pack, nothing refused. Reloads emit their own audio_reload record
	   and never touch this one. */
	s_obs_n = 0;
	audio_registry_test_set_observer(observer_sink);
	audio_registry_emit_load_stats();
	assert(s_obs_n == 1);

	char expect[512];
	snprintf(expect, sizeof(expect),
			 "{\"event\":\"audio_load\",\"root\":\"%s\",\"variants\":%d,"
			 "\"keys_fallback\":0,\"arena_used\":%u,\"arena_cap\":%u,"
			 "\"arena_rejects\":0,\"protected_silent\":0,"
			 "\"protected_short\":0,\"ms\":",
			 PACK_DIR, (int)AUDIO_SOUND_COUNT,
			 (unsigned)golden_total_bytes(), 12u * 1024u * 1024u);
	assert(strncmp(s_obs[0], expect, strlen(expect)) == 0);
	assert(s_obs[0][strlen(s_obs[0]) - 1] == '}');

	/* ONE record, once: a second drain emits nothing. */
	audio_registry_emit_load_stats();
	assert(s_obs_n == 1);
	audio_registry_test_set_observer(NULL);
	printf("  E1: %s\n", s_obs[0]);
}

/* ===== C-secondary: the same order, as the PC diagnostic prints it ======= */

#ifndef PLATFORM_WII
static void test_report_line_order(void) {
	/* Roomy, so every key emits exactly one line and the sequence IS the scan
	   order. Secondary to assert_scan_order(): this one also proves the
	   diagnostic itself reports in scan order. stderr is captured for the
	   reload only, then restored so a later assertion stays readable
	   (test_far_slot_deadlock_env's rule). */
	int saved = DUPFD(FILENO(stderr));
	assert(saved >= 0);
	FILE* captured = freopen(LOG_FILE, "w", stderr);
	assert(captured);
	audio_registry_reload();
	fflush(stderr);
	DUPFD2(saved, FILENO(stderr));
	CLOSEFD(saved);
	clearerr(stderr);

	FILE* f = fopen(LOG_FILE, "rb");
	assert(f);
	char line[1024];
	char keys[AUDIO_SOUND_COUNT][64];
	int n = 0;
	while(fgets(line, sizeof(line), f)) {
		static const char tag[] = "[audio-arena] key=";
		if(strncmp(line, tag, sizeof(tag) - 1) != 0)
			continue;
		assert(n < (int)AUDIO_SOUND_COUNT);
		const char* p = line + sizeof(tag) - 1;
		size_t k = 0;
		while(p[k] && p[k] != ' ' && k + 1 < sizeof(keys[0])) {
			keys[n][k] = p[k];
			k++;
		}
		keys[n][k] = '\0';
		n++;
	}
	fclose(f);
	assert(n == (int)AUDIO_SOUND_COUNT); /* one decoded candidate per key */

	uint16_t ids[AUDIO_SOUND_COUNT];
	int m = audio_registry_test_scan_order(ids, (int)AUDIO_SOUND_COUNT);
	assert(m == n);
	for(int i = 0; i < n; i++)
		assert(strcmp(keys[i], audio_sound_name(ids[i])) == 0);
	printf("  P2: the PC diagnostic prints the same %d-key order OK\n", n);
}
#endif

int main(void) {
	puts("test_audio_registry_priority");

	/* Armed BEFORE the first decode: the loader latches the environment read
	   on its first candidate file. */
	static char env[] = "CAVEWII_AUDIO_ARENA_REPORT=1";
	putenv(env);

	assert(GETCWD(s_srcroot, sizeof(s_srcroot)));
	test_fixture_dir("test_audio_registry_priority_tmp", s_fixture,
					 sizeof(s_fixture));
	MKDIR(s_fixture);
	/* Hermetic from here: "pack" AND the registry's literal "assets" fallback
	   root both resolve inside the fixture. */
	assert(CHDIR(s_fixture) == 0);

	test_protected_set_is_the_approved_package();
	fixture_build();
	config_point_at_pack();

	audio_init(false, 0); /* headless: registry + arena, no device */
	assert(audio_arena_total() == 12u * 1024u * 1024u);
	assert_full_golden("A1");
	assert_scan_order("A1b");
	test_boot_load_record();

	test_starved_arena_keeps_the_protected_set();

	/* F: lifting the squeeze reloads byte-exactly back to the golden. */
	audio_registry_test_set_arena_cap(0);
	audio_registry_reload();
	assert(audio_arena_total() == 12u * 1024u * 1024u);
	assert_full_golden("A2");

	test_rejected_pack_file_with_working_fallback();
	audio_registry_test_set_arena_cap(0);
	audio_registry_reload();
	assert_full_golden("A3");

	test_unreadable_protected_file_is_silent();

#ifndef PLATFORM_WII
	test_report_line_order();
	assert_full_golden("P3");
#endif

	assert(CHDIR(s_srcroot) == 0);
	test_fixture_dir_cleanup(s_fixture); /* takes the log down with it */
	puts("test_audio_registry_priority OK");
	return 0;
}
