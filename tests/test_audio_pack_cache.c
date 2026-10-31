/*
	pack-cache-0927 (docs/intent/pack-cache.md; Astra astra-pack-0926 item 4
	and astra-packcacheB-0927): the opt-in pre-baked AUDIO cache.

	Every load below is compared against a FRESH decode of the same tree (the
	cache off), by a 64-bit FNV-1a over the whole registry: per key the
	variant count; per variant frames, rate, arena OFFSET and every PCM byte
	(the test_audio_load_counters digest). Equal digest = the cached registry
	is byte-identical to a fresh decode, load order included.

	  C0 cache OFF (default): no .cavewii directory is ever created, the load
	     record carries no cache field, status "off".
	  C1a ON with an UNMANAGED pack that ships an OGG sound: keyed by the
	     full source hash (astra-packkey-0927 "unmanaged keeps full hashing
	     or bypass"): miss -> written, then HIT == fresh; an edit of an
	     unmanaged file is caught by that hash.
	  C1 the helper manifest written (pack_manifest_write, the import step);
	     first ON load: miss (no record) -> fresh decode -> published as
	     generation 1 (commit slot A); digest == fresh.
	  C2 next load: HIT, digest == fresh, the record says "cache":"hit".
	  C3 a pack file CHANGED by hand, NOT re-imported: the manifest key does
	     not see it -- the stale generation still HITS (the documented,
	     Astra-ruled limit of a trusted manifest). Re-imported (manifest
	     rewritten): miss "content changed" -> fresh digest; new generation 2
	     in slot B; generation 1 kept as the previous one.
	  C4 a pack file REMOVED and re-imported: miss -> fresh; generation 3;
	     generation 1 pruned (only the newest two are kept).
	  C5 CORRUPT payload byte in the newest generation: miss (checksum) ->
	     fresh decode, never a crash; republished.
	  C6 TORN publication: a truncated next generation plus a torn commit
	     record in the free slot -> ignored, the committed generation HITs.
	     Then the COMMITTED generation truncated -> miss (length) -> fresh.
	  C7 both producers: a Wii-target generation published into the same
	     directory (the host tool's path) does not satisfy the PC key (target
	     is in the key and the header), so the PC load still HITs its own; the
	     Wii payload words are the byte-swapped PC ones.
	  C8 UNWRITABLE: the audio cache directory is a plain file -> publication
	     fails, status "unwritable", the registry is still the fresh decode.
	  C9 arena capacity is in the key: a squeezed arena misses, decodes fresh
	     (its own digest), and caches separately.
	  C10 bundled root (WAV-only): bypassed -- nothing written anywhere.
	  C11 audio.deferred_cave ON: the cache is bypassed (status "off"), no
	     file is written.
	  C12 async ON: the miss path publishes at the async completion; the next
	     load hits.
	  C13 a CORRUPT manifest (its end hash fails) reads as unmanaged: the
	     full-hash key, never the manifest's; registry == fresh.
	  C14 WAV-only packs bypass the cache (measured: a fresh WAV load beats
	     it), unmanaged or managed: nothing read or written.
	  C15 (Fable M2) a manifest that cannot be READ bypasses the cache for
	     that load (no unmanaged key, no write); (M3) another build's key
	     misses while this build still HITS its own generation.
	The main fixture pack carries one raw OGG variant (the committed CC0
	vorbis fixture) so the measured policy serves it.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef CAVEWII_TESTING
#define CAVEWII_TESTING /* the test hooks (compiled into the test pools) */
#endif
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define CHDIR(p) _chdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define CHDIR(p) chdir(p)
#define RMDIR(p) rmdir(p)
#endif

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "platform/audio.h"
#include "platform/audio_cache.h"
#include "platform/pack_manifest.h"
#include "platform/pack_cache.h"
#include "platform/audio_sound_table.h"
#include "platform/audio_wav.h"

extern void audio_registry_test_set_observer(bool (*sink)(const char* line));
extern void audio_registry_test_set_arena_cap(size_t bytes);
extern void audio_registry_test_async_manual(bool manual);

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static char s_fixture[128];

#define TEST_RATE 22050u
#define MAX_FRAMES 4096u

static void mkdir_p(const char* path) {
	char buf[400];
	snprintf(buf, sizeof(buf), "%.399s", path);
	for(char* p = buf + 1; *p; p++)
		if(*p == '/') {
			*p = '\0';
			MKDIR(buf);
			*p = '/';
		}
	MKDIR(buf);
}

static void variant_path(char* out, size_t size, const char* root, uint16_t id,
						 int v) {
	char rel[128];
	audio_sound_table_subpath(audio_sound_name(id), rel, sizeof(rel));
	char dir[400];
	snprintf(dir, sizeof(dir), "%s/%.200s", root, rel);
	char* slash = strrchr(dir, '/');
	*slash = '\0';
	mkdir_p(dir);
	if(v == 0)
		snprintf(out, size, "%s/%.200s.wav", root, rel);
	else
		snprintf(out, size, "%s/%.200s%d.wav", root, rel, v);
}

static int16_t s_pcm[MAX_FRAMES];
static uint8_t s_wav[wavh_write_size(MAX_FRAMES)];

static void put_wav(const char* root, uint16_t id, int v, uint32_t frames,
					uint32_t seed) {
	uint32_t x = seed * 2654435761u + 12345u;
	for(uint32_t k = 0; k < frames; k++) {
		x = x * 1103515245u + 12345u;
		s_pcm[k] = (int16_t)(x >> 16);
	}
	size_t n = wavh_write(s_wav, TEST_RATE, s_pcm, frames);
	char path[512];
	variant_path(path, sizeof(path), root, id, v);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(s_wav, 1, n, f) == n);
	fclose(f);
}

static uint8_t* s_ogg;
static size_t s_ogg_len;
static uint16_t okey;

static void fixture_build(void) {
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(id % 4 == 3) { /* the pack omits it: the bundled root falls back */
			put_wav("assets", id, 1, 500 + id, 900 + id);
			continue;
		}
		int nv = 1 + id % 3;
		for(int k = 0; k < nv; k++)
			put_wav("pack", id, 1 + k, 600u + id * 17u + (uint32_t)k * 5u,
					(uint32_t)(id * 100 + k));
	}
	put_wav("assets", 0, 1, 700, 7); /* never used: the pack owns key 0 */
	/* One raw OGG variant (the committed CC0 vorbis fixture): the audio
	   cache only serves packs that ship OGG sounds (measured policy). It
	   goes on a note.* key (protected pass B) so pass C stays asyncable:
	   the async path keeps pass C synchronous only for a pass-C .ogg. */
	okey = AUDIO_SOUND_COUNT;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT && okey == AUDIO_SOUND_COUNT;
		id++)
		if(id % 4 != 3 && strncmp(audio_sound_name(id), "note.", 5) == 0)
			okey = id;
	assert(okey < AUDIO_SOUND_COUNT);
	char path[512];
	variant_path(path, sizeof(path), "pack", okey, 2 + okey % 3);
	strcpy(path + strlen(path) - 4, ".ogg");
	FILE* f = fopen(path, "wb");
	assert(f && fwrite(s_ogg, 1, s_ogg_len, f) == s_ogg_len);
	fclose(f);
	/* A managed WAV-only pack (C14). */
	put_wav("pack_wav", 1, 1, 640, 77);
}

/* ===== registry digest ================================================== */

static uint64_t registry_hash(void) {
	const uint8_t* base = NULL;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
		for(int v = 0; v < audio_registry_variant_count(id); v++) {
			const struct audio_sample* s = audio_registry_variant(id, v);
			assert(s && s->pcm);
			if(!base || (const uint8_t*)s->pcm < base)
				base = (const uint8_t*)s->pcm;
		}
	uint64_t h = AUDIO_CACHE_FNV_INIT;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		int32_t n = audio_registry_variant_count(id);
		h = audio_cache_fnv(h, &n, sizeof(n));
		for(int v = 0; v < n; v++) {
			const struct audio_sample* s = audio_registry_variant(id, v);
			uint32_t off = (uint32_t)((const uint8_t*)s->pcm - base);
			h = audio_cache_fnv(h, &s->frames, sizeof(s->frames));
			h = audio_cache_fnv(h, &s->rate, sizeof(s->rate));
			h = audio_cache_fnv(h, &off, sizeof(off));
			h = audio_cache_fnv(h, s->pcm, (size_t)s->frames * 2u);
		}
	}
	return h;
}

/* ===== helpers ========================================================== */

static void set_cache(bool on) {
	assert(config_set_string(&gstate.config_user, "pack.cache",
							 on ? "true" : "false"));
}

static void set_pack(const char* pack) {
	assert(config_set_string(&gstate.config_user, "paths.texturepack", pack));
	/* resident scan: the opt-out of the 2026-10-21 deferred-cave default */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "off"));
}

/* A fresh decode of the CURRENT tree (cache off), then back on. */
static uint64_t fresh_hash(void) {
	set_cache(false);
	audio_registry_reload();
	assert(strcmp(audio_registry_cache_status(), "off") == 0);
	uint64_t h = registry_hash();
	set_cache(true);
	return h;
}

static bool exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static const char* status(void) {
	return audio_registry_cache_status();
}

static bool status_is(const char* s) {
	return strcmp(status(), s) == 0;
}

static char s_line[2400];
static bool sink(const char* line) {
	snprintf(s_line, sizeof(s_line), "%s", line);
	return true;
}

#define DIR_AUDIO "pack/.cavewii/audio"

static void gen_file(char* out, size_t size, const char* dir, unsigned g) {
	snprintf(out, size, "%s/g%08u.bin", dir, g);
}

static long file_size(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fclose(f);
	return n;
}

static void flip_byte(const char* path, long at) {
	FILE* f = fopen(path, "r+b");
	assert(f);
	assert(fseek(f, at, SEEK_SET) == 0);
	int c = fgetc(f);
	assert(c != EOF);
	assert(fseek(f, at, SEEK_SET) == 0);
	fputc(c ^ 0x5A, f);
	fclose(f);
}

static void truncate_to(const char* path, long n) {
	FILE* f = fopen(path, "rb");
	assert(f);
	char* buf = malloc((size_t)n + 1);
	assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	f = fopen(path, "wb");
	assert(f && fwrite(buf, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	free(buf);
}

static void write_bytes(const char* path, const char* data, size_t n) {
	FILE* f = fopen(path, "wb");
	assert(f && fwrite(data, 1, n, f) == n);
	fclose(f);
}

static void rm_cache_dir(const char* dir, unsigned max_gen) {
	char p[512];
	for(unsigned g = 1; g <= max_gen; g++) {
		gen_file(p, sizeof(p), dir, g);
		remove(p);
	}
	snprintf(p, sizeof(p), "%s/commit-a.rec", dir);
	remove(p);
	snprintf(p, sizeof(p), "%s/commit-b.rec", dir);
	remove(p);
	RMDIR(dir);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_audio_pack_cache");
	{
		FILE* f = fopen("tests/fixtures/audio_deferred/cave_mono.ogg", "rb");
		assert(f);
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		assert(n > 0);
		s_ogg = malloc((size_t)n);
		assert(s_ogg && fread(s_ogg, 1, (size_t)n, f) == (size_t)n);
		s_ogg_len = (size_t)n;
		fclose(f);
	}
	test_fixture_dir("test_audio_pack_cache_tmp", s_fixture, sizeof(s_fixture));
	MKDIR(s_fixture);
	assert(CHDIR(s_fixture) == 0);
	fixture_build();
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	set_pack("pack");

	/* C0 */
	audio_init(false, 0);
	uint64_t h0 = registry_hash();
	audio_registry_test_set_observer(sink);
	audio_registry_emit_load_stats();
	check(status_is("off") && !exists("pack/.cavewii") && !exists(".cavewii")
			  && !strstr(s_line, "\"cache\""),
		  "C0 cache off: no directory, no record field, status off");

	check(audio_registry_variant_count(okey) == 2 + okey % 3,
		  "C0 the fixture's OGG variant really decodes (note key + 1)");

	/* C1a: unmanaged OGG pack -> full source hash */
	set_cache(true);
	audio_registry_reload();
	check(status_is("written") && registry_hash() == h0,
		  "C1a an unmanaged OGG pack caches under the full source hash");
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == h0, "C1a then HITS");
	put_wav("pack", 2, 1, 611, 6116);
	uint64_t h0b = fresh_hash();
	audio_registry_reload();
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(), "content changed") == 0
			  && registry_hash() == h0b,
		  "C1a an unmanaged edit is caught by the full hash");
	h0 = h0b;
	rm_cache_dir(DIR_AUDIO, 8);

	/* C1 */
	struct pack_manifest man;
	check(pack_manifest_write("pack", &man) && man.trusted && man.files > 0,
		  "C1 setup: the import step writes a trusted manifest");
	audio_registry_reload();
	char g[512];
	gen_file(g, sizeof(g), DIR_AUDIO, 1);
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(), "no valid commit record")
				  == 0
			  && exists(g) && exists(DIR_AUDIO "/commit-a.rec"),
		  "C1 first load misses, decodes fresh, publishes generation 1");
	check(registry_hash() == h0, "C1 the fresh-decoded registry == cache-off");

	/* C2 */
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == h0,
		  "C2 next load HITS and is byte-identical to a fresh decode");

	/* C3: change one pack file */
	put_wav("pack", 1, 1, 640, 4242);
	uint64_t h1 = fresh_hash();
	check(h1 != h0, "C3 setup: the change moves the fresh digest");
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == h0,
		  "C3 a manual edit without re-import is NOT detected (stale hit, "
		  "the documented manifest limit)");
	assert(pack_manifest_write("pack", &man)); /* re-import */
	audio_registry_reload();
	char g2[512];
	gen_file(g2, sizeof(g2), DIR_AUDIO, 2);
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(), "content changed") == 0
			  && registry_hash() == h1 && exists(g2)
			  && exists(DIR_AUDIO "/commit-b.rec") && exists(g),
		  "C3 changed content misses, decodes fresh, gen 2 in slot B, gen 1 kept");
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == h1, "C3 then HITS gen 2");

	/* C4: remove one pack file */
	char victim[512];
	variant_path(victim, sizeof(victim), "pack", 2, 2);
	assert(remove(victim) == 0);
	assert(pack_manifest_write("pack", &man)); /* re-import */
	uint64_t h2 = fresh_hash();
	audio_registry_reload();
	char g3[512];
	gen_file(g3, sizeof(g3), DIR_AUDIO, 3);
	check(status_is("written") && registry_hash() == h2 && exists(g3)
			  && exists(g2) && !exists(g),
		  "C4 a removed file misses; gen 3 published, gen 2 kept, gen 1 pruned");

	/* C5: corrupt the newest generation's payload */
	long n3 = file_size(g3);
	flip_byte(g3, n3 - 40);
	audio_registry_reload();
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(),
						"payload checksum mismatch") == 0
			  && registry_hash() == h2,
		  "C5 a corrupt payload falls back to a fresh decode and republishes");
	char g4[512];
	gen_file(g4, sizeof(g4), DIR_AUDIO, 4);
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == h2 && exists(g4),
		  "C5 then HITS the republished gen 4");

	/* C6: a torn publication of gen 5 (partial file + torn record in the free
	   slot, which is the one gen 4 did not use) */
	char g5[512];
	gen_file(g5, sizeof(g5), DIR_AUDIO, 5);
	write_bytes(g5, "CWAC\x01\x00", 6);
	/* gen 4 went to slot B (gen 3 held A): the free slot is A. */
	write_bytes(DIR_AUDIO "/commit-a.rec", "CWAR\x01\x00\x00\x00\x05", 9);
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == h2,
		  "C6 a torn next generation + torn record are ignored: gen 4 HITS");
	truncate_to(g4, file_size(g4) / 2);
	audio_registry_reload();
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(),
						"generation length differs from its commit record")
				  == 0
			  && registry_hash() == h2,
		  "C6 the committed generation truncated: miss, fresh decode");
	char g6[512];
	gen_file(g6, sizeof(g6), DIR_AUDIO, 6);
	check(exists(g6), "C6 the new generation never reuses a number (gen 6)");

	/* C7: the host tool's Wii-target producer into the same directory */
	char dir[320];
	check(audio_registry_cache_publish_target(AUDIO_CACHE_TARGET_WII_BE, dir,
											  sizeof(dir))
			  && strcmp(dir, DIR_AUDIO) == 0,
		  "C7 a Wii-target generation is published (gen 7)");
	char g7[512];
	gen_file(g7, sizeof(g7), DIR_AUDIO, 7);
	FILE* fw = fopen(g7, "rb");
	FILE* fp = fopen(g6, "rb");
	assert(fw && fp);
	static uint8_t bw[65536], bp[65536];
	size_t nw = fread(bw, 1, sizeof(bw), fw), np = fread(bp, 1, sizeof(bp), fp);
	fclose(fw);
	fclose(fp);
	/* header 56 + table: the payload follows; compare the first sample word */
	size_t table = (size_t)AUDIO_SOUND_COUNT * (1u + AUDIO_MAX_VARIANTS * 12u);
	size_t pay = 56u + table;
	check(nw > pay + 2 && np > pay + 2 && bw[8] == AUDIO_CACHE_TARGET_WII_BE
			  && bp[8] == AUDIO_CACHE_TARGET_PC_LE && bw[pay] == bp[pay + 1]
			  && bw[pay + 1] == bp[pay],
		  "C7 the Wii payload words are the byte-swapped PC words");
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == h2,
		  "C7 the PC load ignores the Wii generation and HITS its own");

	/* C8: unwritable cache location */
	rm_cache_dir(DIR_AUDIO, 8);
	write_bytes(DIR_AUDIO, "not a directory", 15);
	audio_registry_reload();
	check(status_is("unwritable") && registry_hash() == h2,
		  "C8 an unwritable card disables caching; the registry is fresh");
	remove(DIR_AUDIO);

	/* C9: arena capacity is part of the key */
	audio_registry_reload(); /* republish at the full arena */
	assert(status_is("written"));
	audio_registry_test_set_arena_cap(64u * 1024u);
	uint64_t hs = fresh_hash();
	audio_registry_reload();
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(), "content changed") == 0
			  && registry_hash() == hs,
		  "C9 a squeezed arena misses (key), decodes fresh, caches separately");
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == hs, "C9 then HITS");
	audio_registry_test_set_arena_cap(0);

	/* C10: the bundled root (WAV-only: bypassed, measured) */
	set_pack("assets");
	uint64_t hb = fresh_hash();
	audio_registry_reload();
	check(status_is("bypass")
			  && strstr(audio_registry_cache_reason(), "bundled WAV root")
			  && !exists(".cavewii") && !exists("assets/.cavewii")
			  && registry_hash() == hb,
		  "C10 the bundled WAV root bypasses the cache; nothing written");
	set_pack("pack");

	/* C11: deferred cave bypasses the cache */
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "on"));
	long before = file_size(DIR_AUDIO "/commit-a.rec")
		+ file_size(DIR_AUDIO "/commit-b.rec");
	audio_registry_reload();
	check(status_is("off")
			  && file_size(DIR_AUDIO "/commit-a.rec")
						 + file_size(DIR_AUDIO "/commit-b.rec")
					 == before,
		  "C11 audio.deferred_cave on: the cache is bypassed, nothing written");
	/* C11b: the DEFAULT (key absent) is deferred ON since 2026-10-21, so it
	   bypasses the cache too; only the explicit "off" opt-out may use it. */
	json_object_dotremove(json_object(gstate.config_user.root),
						  "audio.deferred_cave");
	before = file_size(DIR_AUDIO "/commit-a.rec")
		+ file_size(DIR_AUDIO "/commit-b.rec");
	audio_registry_reload();
	check(status_is("off")
			  && file_size(DIR_AUDIO "/commit-a.rec")
						 + file_size(DIR_AUDIO "/commit-b.rec")
					 == before,
		  "C11b audio.deferred_cave absent (default on): cache bypassed");
	assert(config_set_string(&gstate.config_user, "audio.deferred_cave", "off"));

	/* C12: async on -- publication at the async completion */
	put_wav("pack", 5, 1, 777, 5555);
	assert(pack_manifest_write("pack", &man)); /* re-import */
	uint64_t ha = fresh_hash();
	assert(config_set_string(&gstate.config_user, "audio.async_load", "on"));
	audio_registry_test_async_manual(true);
	audio_registry_reload();
	check(status_is("miss") && audio_registry_async_pending(),
		  "C12 async miss: pass C in flight, nothing published yet");
	while(audio_registry_async_step())
		;
	audio_registry_async_poll();
	check(status_is("written") && !audio_registry_async_pending()
			  && registry_hash() == ha,
		  "C12 published at the async completion; registry == fresh");
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == ha,
		  "C12 the next load HITS (no async run on a hit)");
	audio_registry_test_async_manual(false);
	assert(config_set_string(&gstate.config_user, "audio.async_load", "off"));

	/* C14: WAV-only packs */
	set_pack("pack_wav");
	uint64_t hw = fresh_hash();
	audio_registry_reload();
	check(status_is("bypass")
			  && strstr(audio_registry_cache_reason(), "unmanaged WAV pack")
			  && !exists("pack_wav/.cavewii") && registry_hash() == hw,
		  "C14 an unmanaged WAV-only pack bypasses the cache; nothing written");
	struct pack_manifest wm;
	assert(pack_manifest_write("pack_wav", &wm));
	audio_registry_reload();
	check(status_is("bypass")
			  && strstr(audio_registry_cache_reason(), "no OGG sounds")
			  && !exists("pack_wav/.cavewii/audio") && registry_hash() == hw,
		  "C14 a managed WAV-only pack bypasses the cache; nothing written");
	set_pack("pack");
	audio_registry_reload();
	assert(status_is("hit"));

	/* C13: a corrupt manifest reads as unmanaged */
	long ra = file_size(DIR_AUDIO "/commit-a.rec");
	long rb = file_size(DIR_AUDIO "/commit-b.rec");
	flip_byte("pack/" PACK_MANIFEST_NAME, 20);
	struct pack_manifest bad;
	check(!pack_manifest_load("pack", &bad) && !bad.trusted,
		  "C13 setup: the corrupt manifest fails its own end hash");
	(void)ra;
	(void)rb;
	audio_registry_reload();
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(), "content changed") == 0
			  && registry_hash() == ha,
		  "C13 a corrupt manifest reads as unmanaged: the full-hash key "
		  "(never the manifest's), registry == fresh");

	/* C15: repair the manifest (re-import), then M2 and M3. */
	assert(pack_manifest_write("pack", &man));
	uint64_t hm = fresh_hash();
	/* A clean store, so this build's generation is the newest of the two
	   slots (a later publish replaces the OLDER slot -- the two-slot rule). */
	rm_cache_dir(DIR_AUDIO, 64);
	audio_registry_reload();
	assert(status_is("written"));
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == hm, "C15 setup: HIT");
	long ca = file_size(DIR_AUDIO "/commit-a.rec");
	long cb = file_size(DIR_AUDIO "/commit-b.rec");
	struct pack_cache_record rr[2];
	pack_cache_records(DIR_AUDIO, AUDIO_CACHE_SCHEMA, rr);
	unsigned ga = rr[0].generation, gb = rr[1].generation;
	pack_manifest_test_fail_alloc(true);
	audio_registry_reload();
	pack_manifest_test_fail_alloc(false);
	pack_cache_records(DIR_AUDIO, AUDIO_CACHE_SCHEMA, rr);
	check(status_is("bypass") && registry_hash() == hm
			  && rr[0].generation == ga && rr[1].generation == gb
			  && file_size(DIR_AUDIO "/commit-a.rec") == ca
			  && file_size(DIR_AUDIO "/commit-b.rec") == cb,
		  "C15 an unreadable manifest bypasses (no unmanaged key, no write)");
	audio_registry_reload();
	check(status_is("hit"), "C15 the next readable load HITS");
	pack_cache_test_set_code_identity("some-other-build");
	audio_registry_reload();
	check(status_is("written")
			  && strcmp(audio_registry_cache_reason(), "content changed") == 0
			  && registry_hash() == hm,
		  "C15 another build's key misses");
	pack_cache_test_set_code_identity(NULL);
	audio_registry_reload();
	check(status_is("hit") && registry_hash() == hm,
		  "C15 this build still HITS its own generation");

	/* Fable LOW-5: leave no per-PID fixture tree behind. */
	assert(CHDIR("..") == 0);
	test_fixture_dir_cleanup(s_fixture);
	printf("%s test_audio_pack_cache (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
