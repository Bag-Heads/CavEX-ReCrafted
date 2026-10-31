/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	S0 sound registry (shared PC+Wii TU).

	NAME -> FILE mapping is Beta's SoundPool rule run IN REVERSE
	[740c583: SoundPool.addSound strips the extension, then ALL trailing
	digits, then maps '/'->'.']: the registry key "step.grass" therefore
	scans sound/step/grass.*, sound/step/grass1.* .. grass13.* and collects
	EVERY file that exists — they are that key's variant set. Playback picks
	one variant uniformly at random, CLIENT-side, exactly where Beta rolled
	it [SoundManager.playSound -> SoundPool.getRandomSoundFromSoundPool];
	server-side RNG rolls (pitch/volume formulas) stay server-side.

	FILE RESOLUTION falls back from <paths.texturepack>/sound/... to the
	bundled assets/sound/... per KEY, not per file (S1; the texture per-FILE
	clone S0 shipped mixed pools: a pack variant set numbered differently
	from the defaults let placeholder files leak IN as extra variants of an
	overridden key and play a fraction of the time). A pack that provides
	ANY variant for a key owns that key's WHOLE pool — the faithful analog
	of Beta's single resources tree feeding one SoundPool — and a key the
	pack omits falls back to the complete default set. .wav is tried before
	.ogg within one pack root (the bundled CC0 defaults are wav; Beta-era
	packs ship ogg).

	DECODE-AT-LOAD: every variant is decoded ONCE here into a single fixed
	PCM16-mono arena claimed at init (mem2_audio_region — its OWN MEM2 region
	on Wii, reserved beside the display-list carve rather than inside it; a
	plain malloc on PC. No eviction, the texture-residency precedent).
	stb_vorbis runs only inside this load path — never on the Broadway core at
	runtime (finish_line_plan.md §2b hardening rule).
*/

#include <assert.h>
#include <dirent.h> /* sound-dir-index: one listing per probed directory */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* audio_root_exists: the selected pack may be deleted */

#include "../game/game_state.h"
#include "../util.h"
#include "audio.h"
#include "audio_cache.h" /* pack-cache-0927: opt-in pre-baked registry */
#include "pack_manifest.h" /* pack-cache-0927: helper digest manifest */
#include "pack_cache.h"
#include "audio_deferred.h" /* C2 P2: the opt-in deferred cave-ambience key */
#include "hw_telemetry.h" /* observer sink for the pack-reload evidence line */
#include "mem2.h"
#include "thread.h" /* thread_msleep: the Wii reload's one-DSP-block margin */
#include "time.h"	/* the reload's own wall-clock, and the stop+drain's */

#ifdef PLATFORM_WII
#include <ogc/cache.h>	/* DCFlushRange: the DSP DMA reads the arena */
#include <ogc/system.h> /* SYS_GetArena1Size/2Size: the MEM1/MEM2 gauges */
#include <ogc/lwp_watchdog.h> /* TB_TIMER_CLOCK: scan counters, tick -> us */
#endif

/* Single-TU stb_vorbis (third_party/stb/PATCHES.md: pristine v1.22, no
   patches). No pushdata API. STDIO stayed compiled OUT here through S0/S1/S2
   (every decode in THIS file is stb_vorbis_decode_memory -- files are read
   by audio_file_read, not by stb_vorbis itself); S3 (audio_music.c, a
   SEPARATE TU) needs the stdio-based stb_vorbis_open_filename/
   get_samples_short_interleaved/close pull-decode API for streaming
   multi-minute tracks without holding a whole compressed .ogg (up to a few
   MB per real Beta track) resident, so it is compiled in from here on --
   this is still the ONE implementation TU (PATCHES.md), and
   tools/sound_transcode.c's STB_VORBIS_EXTERN_VORBIS header-only mirror
   MUST keep matching this exact macro set or its declarations stop lining
   up with the symbols actually emitted here. */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#include <stb/stb_vorbis.c>
#pragma GCC diagnostic pop

/* Boot/Apply service belongs to its registering thread, never the optional
 * pass-C worker. No registry lock is held at any of these service points. */
static audio_registry_service_fn s_load_service;
static void* s_load_service_ctx;
#ifdef PLATFORM_PC
static pthread_t s_load_service_thread;
#else
static lwp_t s_load_service_thread;
#endif
static bool s_load_servicing, s_load_scanning, s_load_cancelled;
static bool s_load_clock_live;
static ptime_t s_load_last_service;
static uint64_t s_load_service_gap_max_us;
static enum audio_registry_ready s_load_ready = AUDIO_REGISTRY_LOADING;
static char s_load_failure[128];

void audio_registry_set_service(audio_registry_service_fn fn, void* ctx) {
	s_load_service = fn;
	s_load_service_ctx = ctx;
#ifdef PLATFORM_PC
	s_load_service_thread = pthread_self();
#else
	s_load_service_thread = LWP_GetSelf();
#endif
}

static void audio_load_service(void) {
	if(!s_load_service) return;
#ifdef PLATFORM_PC
	if(!pthread_equal(pthread_self(), s_load_service_thread)) return;
#else
	if(LWP_GetSelf() != s_load_service_thread) return;
#endif
	if(s_load_servicing) return;
	ptime_t now = time_get();
	if(s_load_clock_live) {
		uint64_t gap = time_diff_us(s_load_last_service, now);
		if(gap > s_load_service_gap_max_us) s_load_service_gap_max_us = gap;
	}
	s_load_last_service = now; /* start-to-start includes presentation time */
	s_load_clock_live = true;
	s_load_servicing = true;
	s_load_service(s_load_service_ctx);
	s_load_servicing = false;
}

enum audio_registry_ready audio_registry_ready_state(void) { return s_load_ready; }
const char* audio_registry_failure_reason(void) { return s_load_failure; }
uint64_t audio_registry_service_gap_max_us(void) { return s_load_service_gap_max_us; }
void audio_registry_cancel_load(void) { s_load_cancelled = true; }

static void audio_load_begin(void) {
	s_load_scanning = true;
	s_load_ready = AUDIO_REGISTRY_LOADING;
	s_load_failure[0] = '\0';
	s_load_cancelled = false;
	s_load_clock_live = false;
	s_load_service_gap_max_us = 0;
	audio_load_service();
}

static void audio_load_fail(const char* reason) {
	s_load_ready = AUDIO_REGISTRY_FAILED;
	snprintf(s_load_failure, sizeof(s_load_failure), "%s", reason);
}

static void audio_load_disable(const char* reason) {
	s_load_ready = AUDIO_REGISTRY_DISABLED;
	snprintf(s_load_failure, sizeof(s_load_failure), "%s", reason);
	fprintf(stderr, "[audio] readiness DISABLED: %s\n", s_load_failure);
}

/* ===== registry tables ================================================== */

/* Beta SoundPool keys, index == enum audio_sound_id — BOTH generated from
   the one shared X-macro (audio_sound_table.h; AUDIO_MAX_VARIANTS lives
   there too), so the ids, this array and the pack tool's manifest cannot
   drift. */
static const char* const s_names[AUDIO_SOUND_COUNT] = {
#define AUDIO_SOUND_TABLE_X(id, name) [id] = name,
	AUDIO_SOUND_TABLE(AUDIO_SOUND_TABLE_X)
#undef AUDIO_SOUND_TABLE_X
};

static struct audio_sample s_variants[AUDIO_SOUND_COUNT][AUDIO_MAX_VARIANTS];
static int s_variant_count[AUDIO_SOUND_COUNT];
static bool s_registry_loaded = false;

/* Client-side cosmetic variant-pick stream (SoundPool's Random lived on the
   client; this must never touch a server sim RNG). */
static struct random_gen s_pick_rng;

/* ===== fixed PCM arena ================================================== */

/* S0 scouted ~4MB; S1 MEASURED the full real Beta 1.7.3 resources overlay
   (32 keys, sound+newsound union, PCM16 at native rates) at 6.04MB aligned
   — 4MB starved the late-table mob keys silent. S2 adds 14 keys whose real
   Beta files skew LONG (13 multi-second cave moods, portal/fire/water
   loops): the 8MB arena's ~1.96MB S1 headroom cannot hold them, so the
   arena is 12MB. It used to be carved out of the 40MB MEM2 lwp heap with
   mem2_alloc, which silently took 30% of the carve that the display-list
   realloc churn had been measured to need; it now has its OWN MEM2 region
   (mem2.h MEM2_AUDIO_REGION_BYTES), reserved at init. The S2 real-pack
   re-measure is recorded in docs/deviations.md. PCM8 (2:1) stays the
   sanctioned budget fallback if a future pack overflows even this. */
#define AUDIO_ARENA_BYTES (12u * 1024u * 1024u)

/* The region reserved at mem2_init and the arena that fills it are two
   constants in two files; pin them together so a future edit to either cannot
   silently leave the arena short (or the region idle). */
_Static_assert(AUDIO_ARENA_BYTES == MEM2_AUDIO_REGION_BYTES,
			   "AUDIO_ARENA_BYTES must match the MEM2 region reserved for it");

static uint8_t* s_arena = NULL;
static size_t s_arena_used = 0;
static size_t s_arena_size = 0;
static unsigned s_arena_rejects = 0; /* samples skipped on a full arena */

#ifdef CAVEWII_TESTING
/* Test-only arena squeeze. mem2_audio_region is first-caller-wins by contract
   (that is what makes audio_registry_reload safe), so a test process cannot
   ask for a second, smaller region once the registry has loaded -- yet
   tests/test_audio_registry_priority.c has to measure the SAME pack against a
   roomy arena and then a starved one to prove the load order does its job.
   The grant is left alone; only the usable size the scan bumps against is
   clamped, which is exactly the state a clipped MEM2 region already puts the
   loader in. Dormant instrumentation, the displaylist_test_fail_* precedent:
   no production build carries either variable. */
static size_t s_test_arena_cap;
static size_t s_test_arena_granted;

void audio_registry_test_set_arena_cap(size_t bytes);
void audio_registry_test_set_arena_cap(size_t bytes) {
	s_test_arena_cap = bytes;
}

/* The three numbers the arena-full stderr line reports, for a test that
   must assert them rather than scrape a log. */
void audio_registry_test_arena_stats(unsigned* rejects,
									unsigned* protected_silent,
									unsigned* protected_short);
/* The order the last scan visited keys in -- the Wii-visible witness to the
   three passes, independent of the PC-only stderr diagnostic. Returns how
   many ids were written (<= cap). */
int audio_registry_test_scan_order(uint16_t* ids, int cap);
#endif

size_t audio_arena_used(void) {
	return s_arena_used;
}

size_t audio_arena_total(void) {
	return s_arena_size;
}

/* Bump allocation, 32-byte aligned per sample (the AESND/DSP-DMA buffer
   rule; harmless on PC). No free — the arena lives for the session. */
static void* audio_arena_alloc(size_t bytes) {
	size_t aligned = (bytes + 31u) & ~(size_t)31u;
	/* Align-up wrap guard (review F1 belt-and-braces): on 32-bit size_t a
	 * near-UINT32_MAX request wraps aligned to 0 and the bump "succeeds"
	 * while granting zero bytes. */
	if(aligned < bytes) {
		s_arena_rejects++;
		return NULL;
	}
	if(!s_arena || s_arena_used + aligned > s_arena_size) {
		s_arena_rejects++;
		return NULL;
	}
	void* p = s_arena + s_arena_used;
	s_arena_used += aligned;
	return p;
}

/* ===== opt-in per-file arena report (PC only) ===========================

   Discovery instrumentation for the 12MB arena's overflow: which candidate
   files the bump arena accepts and which it rejects, in the exact id-order /
   v=0..AUDIO_MAX_VARIANTS-1 scan order the registry already walks. It changes
   NOTHING about what is loaded -- no reordering, no allocation, no extra file
   access -- and stays silent unless the environment names it, so an ordinary
   run behaves byte-for-byte as before. Compiled OUT of the Wii build: this is
   a host-side measurement aid, and the NDEBUG console build carries no stderr
   sink for it anyway.

   Emission point: the ARENA DECISION, i.e. every file that opened AND decoded
   -- the only place `rate`/`frames`/the byte ask exist. A file that opens but
   is not a decodable PCM16-mono WAV (or a bad ogg) asks the arena for nothing,
   so it has no accepted/rejected status to report; it still prints its own
   pre-existing "[audio] ... skipped" line. */
#ifndef PLATFORM_WII
/* Scan context for the line below: which key/variant audio_scan_key is
   currently resolving. Written only on the (main-thread, load-time) scan path
   that the reader runs on, and read only by the report itself. */
static const char* s_report_key = "";
static int s_report_variant = -1;

static bool audio_arena_report_on(void) {
	static int cached = -1;
	if(cached < 0) {
		const char* v = getenv("CAVEWII_AUDIO_ARENA_REPORT");
		cached = (v && strcmp(v, "1") == 0) ? 1 : 0;
	}
	return cached > 0;
}

static void audio_arena_report(const char* path, uint32_t rate, uint32_t frames,
							   size_t bytes, bool accepted) {
	if(!audio_arena_report_on())
		return;
	fprintf(stderr,
			"[audio-arena] key=%s variant=%d path=%s rate=%u frames=%u "
			"bytes=%u status=%s used_after=%u\n",
			s_report_key, s_report_variant, path, (unsigned)rate,
			(unsigned)frames, (unsigned)bytes,
			accepted ? "accepted" : "rejected", (unsigned)s_arena_used);
}

#define AUDIO_ARENA_REPORT_SCAN(key, v)                                        \
	do {                                                                       \
		s_report_key = (key);                                                  \
		s_report_variant = (v);                                                \
	} while(0)
#define AUDIO_ARENA_REPORT(path, rate, frames, bytes, accepted)                \
	audio_arena_report((path), (rate), (frames), (bytes), (accepted))
#else
#define AUDIO_ARENA_REPORT_SCAN(key, v) ((void)0)
#define AUDIO_ARENA_REPORT(path, rate, frames, bytes, accepted) ((void)0)
#endif

/* ===== WAV parsing ====================================================== */

/* S1: the parser body moved VERBATIM into the shared header (audio_wav.h)
   so the host transcode tool rejects exactly what this loader rejects, by
   construction. These public wrappers keep the S0 engine surface. */
#include "audio_wav.h"

int16_t audio_pcm_sample_from_le(const int16_t* pcm, size_t index) {
	return wavh_sample_from_le(pcm, index);
}

bool audio_wav_parse(const uint8_t* data, size_t size, const int16_t** pcm,
					 uint32_t* frames, uint32_t* rate) {
	return wavh_parse(data, size, pcm, frames, rate);
}

/* ===== scan counters (boot-sound-measure, 2026-09-26) ==================

   The real-Wii boot spends ~31 s in the scan below, and how that splits
   between the ~2,200 fopen probes (most of them misses), the reads and the
   byte-swap/decode was never measured. These counters attribute it. They are
   OBSERVATION ONLY: nothing below branches on them, no file is opened, read or
   skipped because of them, and the loaded registry is byte-identical with or
   without them (tests/test_audio_load_counters.c pins a hash captured from
   the pre-counter loader).

   Always on, like the scan's existing ms clock: the boot scan runs before
   telemetry can be armed, so a gate on audio_tel_armed() would zero the one
   record they exist for. Cost: two or three time_get() reads per probe (mftb
   on the Wii) -- a few thousand timebase reads against a 31 s scan.

   Raw deltas are accumulated (timebase ticks on the Wii, ns on PC) and turned
   into microseconds only at stash time, so the phase slices -- which never
   overlap and all lie inside the scan -- cannot sum past scan_us through
   per-slice rounding. Slot 0 counts the selected root, slot 1 the bundled
   default root (per-key fallback probes). Opens made by the opt-in deferred
   cave probe (audio_deferred_probe_ex) do not pass through audio_file_read
   and land in the unattributed remainder of scan_us, as does the final
   DCFlushRange. */
enum { AUDIO_CTR_PACK = 0, AUDIO_CTR_DEFAULT = 1, AUDIO_CTR_ROOTS };

struct audio_scan_ctr {
	unsigned open_miss_n;
	unsigned open_hit_n;
	uint64_t open_miss_raw;
	uint64_t open_hit_raw;
	uint64_t read_raw; /* fseek/ftell/malloc/fread/fclose of an opened file */
	uint64_t conv_raw; /* parse + arena copy/byte-swap or vorbis decode, free */
	uint64_t read_bytes;
	/* sound-dir-index: directories listed / proven absent / left to the
	   per-file probe (listing unavailable), the listing time, and the
	   candidate paths the index proved absent so no fopen was issued. */
	unsigned dir_n;
	unsigned dir_absent_n;
	unsigned dir_probe_n;
	uint64_t dir_raw;
	unsigned skip_n;
};

static struct audio_scan_ctr s_ctr[AUDIO_CTR_ROOTS];
static int s_ctr_root = AUDIO_CTR_PACK;
static uint64_t s_ctr_file_max_raw; /* one candidate path, open..free */
static uint64_t s_ctr_key_max_raw;	/* one key, all roots and variants */
static int s_ctr_key_max_id = -1;

static uint64_t audio_ctr_raw(ptime_t a, ptime_t b) {
#ifdef PLATFORM_WII
	return (uint64_t)(b - a);
#else
	int64_t ns = ((int64_t)b.tv_sec - (int64_t)a.tv_sec) * INT64_C(1000000000)
		+ ((int64_t)b.tv_nsec - (int64_t)a.tv_nsec);
	return ns > 0 ? (uint64_t)ns : 0u;
#endif
}

static unsigned audio_ctr_us(uint64_t raw) {
#ifdef PLATFORM_WII
	uint64_t us = raw * 1000u / (uint64_t)TB_TIMER_CLOCK;
#else
	uint64_t us = raw / 1000u;
#endif
	return us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned)us;
}

static void audio_ctr_reset(void) {
	memset(s_ctr, 0, sizeof(s_ctr));
	s_ctr_root = AUDIO_CTR_PACK;
	s_ctr_file_max_raw = 0;
	s_ctr_key_max_raw = 0;
	s_ctr_key_max_id = -1;
}

/* ===== directory index (sound-dir-index, 2026-09-26) ===================

   Real-Wii boot, Beta pack: 26.2 s of a 30.8 s scan was ~1,900 fopen probes
   for variant names that do not exist (a libfat miss walks the directory on
   the card every time; tmp/jobs/next-gaps-0924/BOOT-SOUND-MEASURE-0926.md).
   So each directory a candidate lives in is listed ONCE per scan, and a
   candidate whose name the listing proves absent is not opened at all.

   T1, bit-exact BY CONSTRUCTION: the index is only a conservative FILTER in
   front of the unchanged loader. It never adds a candidate, never reorders
   one and never decides a hit -- a name the index admits still goes through
   the very same fopen/read/parse as before, and the SAME candidate wins. The
   one obligation is "fopen(path) would succeed => the index admits path",
   met by these rules:
     - Name match is ASCII case-INSENSITIVE, whole name. FAT (libfat on the
       Wii) and NTFS/FAT on a Windows host are case-insensitive, so this is
       exactly what fopen matches there; on a case-sensitive host it admits a
       superset (fopen then misses, as it always did). Every probe name is
       plain ASCII ("grass1.wav"), and FAT 8.3 aliases of a long name always
       carry "~N", which no probe name does.
     - A directory holding ANY name with a byte >= 0x80 is not indexed (the
       per-file probe runs there): Unicode case folding is the filesystem's,
       not ours to re-derive.
     - opendir fails and stat says the path does not exist (ENOENT/ENOTDIR)
       or is not a directory: nothing below it can open either, so every
       candidate there is absent. Any other opendir failure, a failed
       allocation, or a full table: that directory falls back to today's
       per-file probe.
   The index lives for ONE scan (boot load or pack reload) and is freed at its
   end, so a card edited between reloads is re-listed. Heap only -- no static
   tables, so the Wii .bss layout (the sd0 DMA-buffer gate) barely moves. */
enum audio_dir_state {
	AUDIO_DIR_LISTED = 0, /* names[] is authoritative */
	AUDIO_DIR_ABSENT,	  /* the directory does not exist */
	AUDIO_DIR_PROBE,	  /* listing unavailable: per-file probe */
};

struct audio_dir_entry {
	char* path;	 /* "<root>/sound/step", exactly as the probe spells it */
	char* names; /* NUL-separated lowercased names, names_len bytes */
	size_t names_len;
	enum audio_dir_state state;
};

#define AUDIO_DIR_INDEX_MAX 96 /* distinct sound dirs x 2 roots, with room */

static struct audio_dir_entry* s_dir_idx; /* NULL = index off this scan */
static int s_dir_idx_n;

#ifdef CAVEWII_TESTING
/* Test-only switch: false forces today's per-file probe for EVERY candidate,
   so tests/test_audio_dir_index_equiv.c can load the same tree both ways and
   compare. Dormant instrumentation; production has no way to turn it off. */
static bool s_dir_idx_test_off;
void audio_registry_test_set_dir_index(bool on);
void audio_registry_test_set_dir_index(bool on) {
	s_dir_idx_test_off = !on;
}

/* Test-only read-error seam (Fable review MEDIUM, 2026-09-26): when > 0,
   every directory listing reports a read error (readdir -> NULL with
   errno = EIO) after returning this many entries, the way a libfat
   mid-listing sector read failure does. 0 = off. */
static int s_dir_idx_test_fail_after;
void audio_registry_test_set_dir_read_error_after(int entries);
void audio_registry_test_set_dir_read_error_after(int entries) {
	s_dir_idx_test_fail_after = entries;
}
#endif

/* readdir with the test seam above. errno is cleared before every call, so
   a NULL return with errno set is a READ ERROR, not end-of-directory. */
static struct dirent* audio_readdir(DIR* d, int* returned) {
#ifdef CAVEWII_TESTING
	if(s_dir_idx_test_fail_after > 0
	   && *returned >= s_dir_idx_test_fail_after) {
		errno = EIO;
		return NULL;
	}
#endif
	errno = 0;
	struct dirent* e = readdir(d);
	if(e)
		(*returned)++;
	return e;
}

static char audio_ascii_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static void audio_dir_index_begin(void) {
	s_dir_idx = NULL;
	s_dir_idx_n = 0;
#ifdef CAVEWII_TESTING
	if(s_dir_idx_test_off)
		return;
#endif
	/* A failed calloc just means every candidate is probed, as before. */
	s_dir_idx = calloc(AUDIO_DIR_INDEX_MAX, sizeof(*s_dir_idx));
}

static void audio_dir_index_end(void) {
	if(!s_dir_idx)
		return;
	for(int k = 0; k < s_dir_idx_n; k++) {
		free(s_dir_idx[k].path);
		free(s_dir_idx[k].names);
	}
	free(s_dir_idx);
	s_dir_idx = NULL;
	s_dir_idx_n = 0;
}

/* List `dir` into `ent` (state + names). */
static void audio_dir_list(struct audio_dir_entry* ent, const char* dir) {
	struct audio_scan_ctr* c = &s_ctr[s_ctr_root];
	ptime_t t0 = time_get();
	ent->state = AUDIO_DIR_PROBE;
	ent->names = NULL;
	ent->names_len = 0;
	DIR* d = opendir(dir);
	if(!d) {
		struct stat st;
		errno = 0;
		if(stat(dir, &st) != 0) {
			if(errno == ENOENT || errno == ENOTDIR)
				ent->state = AUDIO_DIR_ABSENT;
		} else if(!S_ISDIR(st.st_mode)) {
			ent->state = AUDIO_DIR_ABSENT;
		}
	} else {
		size_t cap = 0;
		bool ok = true;
		int returned = 0;
		struct dirent* e;
		while(ok && (e = audio_readdir(d, &returned))) {
			audio_load_service();
			const char* n = e->d_name;
			if(strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
				continue;
			size_t len = strlen(n);
			for(size_t i = 0; i < len; i++)
				if((unsigned char)n[i] >= 0x80)
					ok = false; /* non-ASCII: leave it to fopen */
			if(!ok)
				break;
			if(ent->names_len + len + 1 > cap) {
				size_t ncap = cap ? cap * 2 : 1024;
				while(ncap < ent->names_len + len + 1)
					ncap *= 2;
				char* nb = realloc(ent->names, ncap);
				if(!nb) {
					ok = false;
					break;
				}
				ent->names = nb;
				cap = ncap;
			}
			for(size_t i = 0; i <= len; i++)
				ent->names[ent->names_len + i] = audio_ascii_lower(n[i]);
			ent->names_len += len + 1;
		}
		/* readdir's NULL is either end-of-directory or a read error (a
		   libfat sector read failing mid-listing). A partial listing must
		   never be trusted as proof of absence: the directory falls back to
		   the per-file probe (Fable review MEDIUM, 2026-09-26). */
		if(ok && !e && errno != 0)
			ok = false;
		closedir(d);
		if(ok) {
			ent->state = AUDIO_DIR_LISTED;
		} else {
			free(ent->names);
			ent->names = NULL;
			ent->names_len = 0;
		}
	}
	if(ent->state == AUDIO_DIR_LISTED)
		c->dir_n++;
	else if(ent->state == AUDIO_DIR_ABSENT)
		c->dir_absent_n++;
	else
		c->dir_probe_n++;
	c->dir_raw += audio_ctr_raw(t0, time_get());
}

/* false ONLY when the index proves fopen(path) would fail; true means "ask
   fopen", exactly as every candidate was asked before. */
/* asyncsnd-0926: the raw-OGG probe consults the index without spending the
   scan's skip counter (its absent names are asked again by the scan proper). */
static bool s_dir_idx_quiet;

static bool audio_dir_index_admits(const char* path) {
	if(!s_dir_idx)
		return true;
	const char* slash = strrchr(path, '/');
	if(!slash || slash == path || !slash[1])
		return true;
	size_t dir_len = (size_t)(slash - path);
	const char* name = slash + 1;

	struct audio_dir_entry* ent = NULL;
	for(int k = 0; k < s_dir_idx_n; k++) {
		if(strlen(s_dir_idx[k].path) == dir_len
		   && memcmp(s_dir_idx[k].path, path, dir_len) == 0) {
			ent = &s_dir_idx[k];
			break;
		}
	}
	if(!ent) {
		if(s_dir_idx_n >= AUDIO_DIR_INDEX_MAX)
			return true; /* table full: per-file probe */
		char* dir = malloc(dir_len + 1);
		if(!dir)
			return true;
		memcpy(dir, path, dir_len);
		dir[dir_len] = '\0';
		ent = &s_dir_idx[s_dir_idx_n++];
		ent->path = dir;
		audio_dir_list(ent, dir);
	}

	bool admit;
	if(ent->state == AUDIO_DIR_PROBE) {
		admit = true;
	} else if(ent->state == AUDIO_DIR_ABSENT) {
		admit = false;
	} else {
		admit = false;
		size_t nlen = strlen(name);
		for(size_t off = 0; off < ent->names_len && !admit;) {
			const char* cand = ent->names + off;
			size_t clen = strlen(cand);
			if(clen == nlen) {
				size_t i = 0;
				while(i < nlen && cand[i] == audio_ascii_lower(name[i]))
					i++;
				admit = i == nlen;
			}
			off += clen + 1;
		}
	}
	if(!admit && !s_dir_idx_quiet)
		s_ctr[s_ctr_root].skip_n++;
	return admit;
}

/* ===== file loading ===================================================== */

/* Same result as the pre-counter body for every input (NULL on a failed open,
   empty file, failed malloc or short read; the whole file otherwise); only
   the early returns were folded so the read slice is timed once. */
static uint8_t* audio_file_read(const char* path, size_t* out_size) {
	struct audio_scan_ctr* c = &s_ctr[s_ctr_root];
	ptime_t t0 = time_get();
	FILE* f = fopen(path, "rb");
	ptime_t t1 = time_get();
	if(!f) {
		c->open_miss_n++;
		c->open_miss_raw += audio_ctr_raw(t0, t1);
		return NULL;
	}
	c->open_hit_n++;
	c->open_hit_raw += audio_ctr_raw(t0, t1);
	uint8_t* buf = NULL;
	size_t got = 0;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(len > 0) {
		buf = malloc((size_t)len);
		if(buf) {
			while(got < (size_t)len) {
				size_t chunk = (size_t)len - got;
				if(chunk > 16384) chunk = 16384;
				size_t n = fread(buf + got, 1, chunk, f);
				got += n;
				audio_load_service();
				if(n != chunk) break;
			}
		}
	}
	fclose(f);
	c->read_raw += audio_ctr_raw(t1, time_get());
	c->read_bytes += got;
	if(!buf)
		return NULL;
	if(got != (size_t)len) {
		free(buf);
		return NULL;
	}
	*out_size = got;
	return buf;
}

/* Heap low-water sampler, defined with the rest of the observer telemetry
 * below (it needs AUDIO_DEFAULT_PACK). Called from inside the two decode
 * branches while their transient scratch is still held; a no-op unless a
 * reload armed it. */
static void audio_tel_note_low(void);

/* Candidate files that OPENED and DECODED in the current scan -- whether or
   not the arena then took them. This is the "offered" side of the per-key
   short/silent accounting: a key that loaded fewer variants than its pool
   offered lost them to the arena, while a key that was offered nothing has a
   missing or unreadable file instead. Bumped at the single point where both
   decode branches know the sample is real (review MEDIUM-5). */
static unsigned s_scan_offered;

/* asyncsnd-0926: an OGG decode while pass C runs on the loader thread would
   break the "raw-OGG inputs retain the synchronous path" clause; the probe in
   audio_async_begin keeps it from ever happening, and this counts any escape. */
static void audio_async_note_ogg_decode(void);

/* Decode one candidate file into the arena; true on success. */
static bool audio_load_file(const char* path, bool is_ogg,
							struct audio_sample* out) {
	size_t size = 0;
	uint8_t* data = audio_file_read(path, &size);
	if(!data)
		return false;

	ptime_t t_conv = time_get();
	bool ok = false;
	if(!is_ogg) {
		const int16_t* pcm;
		uint32_t frames, rate;
		if(audio_wav_parse(data, size, &pcm, &frames, &rate)) {
			int16_t* dst = audio_arena_alloc((size_t)frames * 2);
			s_scan_offered++;
			AUDIO_ARENA_REPORT(path, rate, frames, (size_t)frames * 2,
							   dst != NULL);
			if(dst) {
				/* WAV sample words are little-endian on disk; the arena
				   holds HOST-order samples (big-endian on the Wii = what
				   the DSP DMA reads, native on PC). */
				for(uint32_t k = 0; k < frames; k++) {
					dst[k] = audio_pcm_sample_from_le(pcm, k);
					if((k & 2047u) == 2047u) audio_load_service();
				}
				out->pcm = dst;
				out->frames = frames;
				out->rate = rate;
				ok = true;
				/* Heap low-water: the whole encoded file is still
				   held here, so this is the deepest this decode
				   goes. No-op unless a reload armed the sampler. */
				audio_tel_note_low();
			}
		} else {
			fprintf(stderr, "[audio] %s: not a PCM16 mono WAV, skipped\n",
					path);
		}
	} else {
		/* Load-path-only ogg decode (stb_vorbis mallocs transiently; the
		   decoded PCM is copied into the arena and the scratch freed). */
		int channels = 0, rate = 0;
		short* dec = NULL;
		int frames
			= stb_vorbis_decode_memory(data, (int)size, &channels, &rate, &dec);
		audio_load_service(); /* whole decode gap is measured, never hidden */
		audio_async_note_ogg_decode(); /* loader thread: never, by the probe */
		if(frames > 0 && dec && (channels == 1 || channels == 2)) {
			int16_t* dst = audio_arena_alloc((size_t)frames * 2);
			s_scan_offered++;
			AUDIO_ARENA_REPORT(path, (uint32_t)rate, (uint32_t)frames,
							   (size_t)frames * 2, dst != NULL);
			if(dst) {
				if(channels == 1) {
					memcpy(dst, dec, (size_t)frames * 2);
				} else {
					/* Average-downmix to the mono voice model. */
					for(int k = 0; k < frames; k++)
						dst[k] = (int16_t)(((int)dec[2 * k]
											+ (int)dec[2 * k + 1])
										   / 2);
				}
				out->pcm = dst;
				out->frames = (uint32_t)frames;
				out->rate = (uint32_t)rate;
				ok = true;
				/* Deeper still: the encoded file AND stb_vorbis's
				   decoded block are both outstanding right now. */
				audio_tel_note_low();
			}
		} else {
			fprintf(stderr, "[audio] %s: ogg decode failed, skipped\n", path);
		}
		if(dec)
			free(dec);
	}

	free(data);
	s_ctr[s_ctr_root].conv_raw += audio_ctr_raw(t_conv, time_get());
	return ok;
}

/* Resolve ONE candidate basename ("sound/step/grass1") against ONE pack
   root. Extension order .wav then .ogg. The pack-vs-default decision is the
   CALLER's, per KEY (audio_registry_scan) — see the FILE RESOLUTION note. */
#define AUDIO_DEFAULT_PACK "assets" /* == TEX_DEFAULT_PACK, texture.c:301 */

/* ===== observer telemetry (real-Wii audio-switch evidence) ===============
   Astra gate 3 condition 3: a real Wii must SHOW a changed audio root being
   decoded and used. None of that reaches the console today -- the reload's
   only outputs are two stderr lines the NDEBUG Wii build does not carry, and
   the F3 arena row. One observer JSON record per reload, and one the first
   time a sample from the new registry is handed to the play path, puts the
   four numbers Astra asked for on the wire.

   INERT when telemetry is not armed: audio_tel_armed() is the same
   hw_telemetry_active() && !hw_telemetry_quiesced() gate main.c puts in front
   of every other observer producer (main.c:2387), and it is tested BEFORE any
   measurement -- no clock reads beyond the ones the reload already needs, no
   MEM1/MEM2 sampling inside the decode loop, no formatting. The play path
   pays one static bool test (s_first_play_pending), which only a reload under
   armed telemetry ever sets. */

#ifdef CAVEWII_TESTING
/* Test sink (tests/test_pack_helper_audio.c). Non-NULL both ARMS the events
   and captures the exact lines, so the JSON shape is asserted without a
   telemetry backend. Same dormant-instrumentation rule as
   audio_registry_test_reload_count below. */
static bool (*s_observer_sink)(const char* line);
void audio_registry_test_set_observer(bool (*sink)(const char* line));
void audio_registry_test_set_observer(bool (*sink)(const char* line)) {
	s_observer_sink = sink;
}
#endif

static bool audio_tel_armed(void) {
#ifdef CAVEWII_TESTING
	if(s_observer_sink)
		return true;
#endif
	return hw_telemetry_active() && !hw_telemetry_quiesced();
}

static bool audio_tel_emit(const char* line) {
#ifdef CAVEWII_TESTING
	if(s_observer_sink)
		return s_observer_sink(line);
#endif
	return hw_telemetry_emit_observer(line);
}

/* An OWED record (re-offered every frame until admitted): a full ring defers
 * it, so it must not count as a telemetry drop each frame (real-Wii
 * split_offer_piston_2m: tel_drop_names {"audio_deferred_scan":14}). */
static bool audio_tel_emit_owed(const char* line) {
#ifdef CAVEWII_TESTING
	if(s_observer_sink)
		return s_observer_sink(line);
#endif
	return hw_telemetry_emit_observer_retry(line);
}

/* A pack root is a folder name on the player's card: it can legally contain a
   quote or a backslash, either of which would produce a record the collector
   cannot parse. Escape exactly what JSON requires, and drop control bytes.
   (hw_telemetry.c has the same helper, but it is static to that TU.) */
static size_t audio_tel_json_str(char* dst, size_t size, size_t off,
								 const char* src) {
	if(off + 1 >= size)
		return size;
	dst[off++] = '"';
	for(const char* p = src; p && *p; p++) {
		char esc[7];
		size_t n;
		if(*p == '"' || *p == '\\') {
			esc[0] = '\\';
			esc[1] = *p;
			n = 2;
		} else if((unsigned char)*p < 0x20) {
			n = (size_t)snprintf(esc, sizeof(esc), "\\u%04x",
								 (unsigned)(unsigned char)*p);
		} else {
			esc[0] = *p;
			n = 1;
		}
		if(off + n + 1 >= size)
			return size;
		memcpy(dst + off, esc, n);
		off += n;
	}
	dst[off++] = '"';
	return off;
}

/* MEM1/MEM2 gauges, the same two readings main.c puts in every heartbeat
   (main.c:740-741) and screen_ingame.c puts in the F3 row -- read here
   directly because neither of those is callable from this file. PC has no
   arena split, so both are -1 and the collector must not read them as 0. */
static int audio_tel_mem1_free_kb(void) {
#ifdef PLATFORM_WII
	return (int)(SYS_GetArena1Size() / 1024);
#else
	return -1;
#endif
}

static int audio_tel_mem2_free_kb(void) {
#ifdef PLATFORM_WII
	return (int)(SYS_GetArena2Size() / 1024);
#else
	return -1;
#endif
}

/* The LOW point Astra asked for: sampled inside the decode loop with the
   transient scratch (the whole file buffer, and stb_vorbis's decoded block for
   an ogg) STILL held, which is the deepest the heap ever goes during a reload.
   Armed only for the duration of one scan. */
static bool s_tel_low_active;
static int s_tel_low_mem1 = -1;
static int s_tel_low_mem2 = -1;

static void audio_tel_note_low(void) {
	if(!s_tel_low_active)
		return;
	int m1 = audio_tel_mem1_free_kb();
	int m2 = audio_tel_mem2_free_kb();
	if(s_tel_low_mem1 < 0 || m1 < s_tel_low_mem1)
		s_tel_low_mem1 = m1;
	if(s_tel_low_mem2 < 0 || m2 < s_tel_low_mem2)
		s_tel_low_mem2 = m2;
}

/* The resolved root of the CURRENT registry contents, and which step of
   audio_effective_pack's revalidation chain supplied it. Both are what the
   evidence line reports, and s_effective_root is also what audio_first_play
   names as the directory a playing sample was decoded from. */
static char s_effective_root[256] = AUDIO_DEFAULT_PACK;
static const char* s_effective_fallback = "none";
static unsigned s_keys_fallback;
static bool s_first_play_pending;
/* What the pending first-play record will name: stashed by the pick,
   consumed by the voice start. See audio_registry_note_voice_started. */
static int s_pending_sound_id = -1;
static int s_pending_variant = -1;

static bool audio_load_candidate(const char* pack, const char* rel_base,
								 struct audio_sample* out) {
	static const struct {
		const char* ext;
		bool is_ogg;
	} exts[] = {{".wav", false}, {".ogg", true}};
	char path[512];

	for(size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
		audio_load_service();
		snprintf(path, sizeof(path), "%s/%s%s", pack, rel_base, exts[e].ext);
		ptime_t t0 = time_get();
		/* sound-dir-index: a name the listing proves absent is not opened
		   (same outcome as the fopen miss it replaces; the next extension /
		   variant is tried exactly as before). */
		bool ok = audio_dir_index_admits(path)
			&& audio_load_file(path, exts[e].is_ogg, out);
		uint64_t d = audio_ctr_raw(t0, time_get());
		if(d > s_ctr_file_max_raw)
			s_ctr_file_max_raw = d;
		if(ok)
			return true;
	}
	return false;
}

/* Collect one key's whole variant set from ONE root: the bare base plus
   every trailing suffix 1..13, two-digit included (the digit-strip rule in
   reverse; Beta's cave set is cave1..cave13 — AUDIO_MAX_VARIANTS).

   Returns how many candidates this root OFFERED (decoded successfully), which
   is >= the number actually stored when the arena refused some of them. */
static int audio_scan_key(int id, const char* root, const char* rel) {
	unsigned offered_before = s_scan_offered;
	for(int v = 0; v < AUDIO_MAX_VARIANTS; v++) {
		char rel_base[144];
		if(v == 0)
			snprintf(rel_base, sizeof(rel_base), "%s", rel);
		else
			snprintf(rel_base, sizeof(rel_base), "%s%d", rel, v);

		AUDIO_ARENA_REPORT_SCAN(s_names[id], v);
		struct audio_sample smp;
		if(audio_load_candidate(root, rel_base, &smp))
			s_variants[id][s_variant_count[id]++] = smp;
	}
	return (int)(s_scan_offered - offered_before);
}

/* ===== load-order passes (C2 P1, Astra D1) ==============================

   The arena is a bump allocator with no eviction, so whatever is scanned LAST
   is what a full arena drops. Measured 2026-09-17 against the real Beta sound
   set (tmp/jobs/c2-audio-coverage-0917/report.md): the pack asks for 14.38MB
   against a 12,443,648-byte granted region, and because the menu keys sit at
   the very END of the id table, the files the arena refused were the eight
   MAINTAINER-APPROVED UI sounds plus note.bassattack -- the console lost its
   whole menu/UI voice to a pack that never intended to touch it.

   So the scan runs three passes over the SAME key table:
     A  the approved UI sound package -- every "menu." key (today exactly the
        eight in audio_sound_table.h: scroll, category, select, back, item,
        type, connected, attention). A future approved UI sound joins this
        pass by its name alone.
     B  every "note." key (the five BlockNote instruments).
     C  everything else, in unchanged table order.
   Within a pass the table order is untouched, and every key is scanned exactly
   once through the SAME audio_scan_key/audio_load_candidate path, so per-key
   pack-then-default precedence, the variant set, the arena and the reject
   accounting are all unchanged. Only WHO gets the bytes first moves. */
enum audio_key_pass {
	AUDIO_PASS_UI = 0,	 /* the approved UI sound package */
	AUDIO_PASS_NOTE = 1, /* noteblock instruments */
	AUDIO_PASS_REST = 2, /* everything else, table order */
	AUDIO_PASS_COUNT
};

static int audio_key_pass(const char* name) {
	if(strncmp(name, "menu.", 5) == 0)
		return AUDIO_PASS_UI;
	if(strncmp(name, "note.", 5) == 0)
		return AUDIO_PASS_NOTE;
	return AUDIO_PASS_REST;
}

/* A key whose silence would be a REGRESSION rather than a pack's own choice:
   passes A and B. Counted in the arena-full report so "the arena overflowed"
   and "the arena overflowed onto the protected set" can never read alike. */
static bool audio_key_protected(const char* name) {
	return audio_key_pass(name) != AUDIO_PASS_REST;
}

/* The protected set's outcome in the CURRENT scan, computed AFTER each key's
   pack + fallback resolution from its FINAL variant count (review MEDIUM-5 --
   bracketing arena rejects per key over-counted a key whose fallback then
   succeeded and missed a key silenced for a non-arena reason):
     silent -- ended with ZERO variants, for ANY reason (arena, missing file,
               unreadable file). This is the number that says the console lost
               a voice; the three passes exist to keep it at 0.
     short  -- loaded FEWER variants than the pool that supplied it offered, so
               the key still speaks but with a thinner pool than the pack
               intended. Only the arena can cause this.
   Both count KEYS, not files, and only protected ones. */
static unsigned s_protected_silent;
static unsigned s_protected_short;

static void audio_load_finish(void) {
	/* Only called on main after all async publication has retired. */
	audio_load_service();
	if(s_load_cancelled) {
		audio_load_fail("Sound loading cancelled. Retry to load sounds.");
	} else {
		s_load_ready = AUDIO_REGISTRY_READY;
		for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
			if(audio_key_protected(s_names[id]) && !s_variant_count[id]) {
				char reason[128];
				snprintf(reason, sizeof(reason),
					"Required sound unavailable: %s (missing, decode or PCM arena failure).",
					s_names[id]);
				/* Pre-T2b: this event is silent, other loaded keys still play. */
				audio_load_disable(reason);
				break;
			}
		}
	}
	s_load_clock_live = false;
	s_load_scanning = false;
}

#ifdef CAVEWII_TESTING
/* The order the scan actually visited keys in, recorded live. The pass order
   is a Wii-visible property, but the only other witness to it is the PC-only
   stderr diagnostic (review MEDIUM-4), so the test reads it from here
   instead. Dormant instrumentation: no production build carries it. */
static uint16_t s_scan_order[AUDIO_SOUND_COUNT];
static int s_scan_order_n;
#endif

/* Resolve the effective audio root AND prove it still exists.

   paths.audiopack can outlive the pack it names: apply pack A (which carries
   audio), then apply a texture-only pack B -- audiopack stays A by design --
   then delete A from texturepacks/. Every later launch would then scan a root
   that is not there, and no screen can move the selection back, so audio is
   silent forever (2026-09-17 DELTA review, HIGH). Fall back to the texture
   pack's root, then to the bundled default.

   Two things this deliberately does NOT do: it never rewrites the config at
   boot (a launch must not mutate settings the player did not touch), and when
   paths.audiopack is ABSENT it performs no probe at all and resolves exactly
   as the pre-audiopack code did -- so a stock config takes a byte-identical
   path through here.

   `out` receives an owned copy: config_read_path returns a pointer to one
   static buffer on the Wii, so each candidate must be copied before the next
   call overwrites it. */
static bool audio_root_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void audio_effective_pack(char* out, size_t out_size) {
	/* Which step of the chain below ended up supplying the root -- the
	   "fallback" field of the audio_reload record, so a card whose selected
	   pack silently vanished is distinguishable from one that resolved
	   straight through. */
	s_effective_fallback = "none";
	if(!config_read_string(&gstate.config_user, "paths.audiopack", NULL)) {
		/* No explicit choice (stock/legacy config): unchanged behaviour. */
		snprintf(out, out_size, "%s",
				 config_read_path(&gstate.config_user, "paths.texturepack",
								  AUDIO_DEFAULT_PACK));
		return;
	}

	const char* legacy = config_read_string(&gstate.config_user,
										"paths.texturepack", AUDIO_DEFAULT_PACK);
	snprintf(out, out_size, "%s",
			 config_read_path(&gstate.config_user, "paths.audiopack", legacy));
	if(audio_root_exists(out))
		return;
	s_effective_fallback = "texturepack";
	fprintf(stderr,
			"[audio] selected audio pack \"%s\" is gone; falling back to the "
			"texture pack\n",
			out);

	snprintf(out, out_size, "%s",
			 config_read_path(&gstate.config_user, "paths.texturepack",
							  AUDIO_DEFAULT_PACK));
	if(audio_root_exists(out))
		return;
	s_effective_fallback = "default";
	fprintf(stderr, "[audio] texture pack \"%s\" is gone too; using the bundled "
					"default\n",
			out);
	snprintf(out, out_size, "%s", AUDIO_DEFAULT_PACK);
}

/* ===== deferred keys (C2 P2, Astra ruling 3) ============================

   SHIPPED-BY-DEFAULT T2 deviation (maintainer ruling 2026-10-21), config
   "audio.deferred_cave": default "on"; "off" is the explicit opt-out (every
   line below is skipped and the scan is the resident one, byte-identical to
   the pre-deferred scan). When "on" (the default),
   ambient.cave.cave -- 13 variants, ~4.4MB, the single pool that overflows the
   arena -- is PROBED instead of decoded: its variants keep pcm == NULL in
   s_variants and are loaded one at a time, on demand, into ONE scratch buffer
   (audio_deferred.h). The scratch is reserved from this arena AFTER the
   protected passes A and B and BEFORE pass C, so it can never cost the approved
   UI package or note.* a byte, and it is sized to the largest ELIGIBLE variant.

   Accounting: discovered = files that probed OK; eligible = discovered and no
   larger than AUDIO_DEFERRED_MAX_BYTES (oversize is COUNTED, never truncated);
   the pick is uniform over the eligible ones. Per-KEY pack-then-default
   precedence is the resident rule, unchanged: the default pool is asked only
   when the pack offers NOTHING eligible. If the scratch or the loader cannot
   be had, the key falls back to the ordinary resident scan in pass C
   (deferred_unavailable counts it) -- the faithful path, thinner pool. */
static bool s_deferred_active; /* this scan deferred the cave key */
static struct audio_deferred_source s_deferred_src[AUDIO_MAX_VARIANTS];
static unsigned s_deferred_discovered;
static unsigned s_deferred_oversize;
static unsigned s_deferred_unavailable;
static unsigned s_deferred_scratch_bytes;
static unsigned s_deferred_decoder_bytes;
static bool s_deferred_scan_pending; /* the scan record is owed to telemetry */

static bool audio_deferred_wanted(void) {
	return strcmp(config_read_string(&gstate.config_user,
									 "audio.deferred_cave", "on"),
				  "off")
		!= 0;
}

/* Probe one root for the key's variant set; fills s_variants[id][0..n) (pcm
   NULL) + s_deferred_src[0..n). Returns n, the ELIGIBLE count. */
static int audio_deferred_probe_root(int id, const char* root, const char* rel,
									 uint32_t* max_frames,
									 size_t* max_decoder) {
	static const struct {
		const char* ext;
		bool is_ogg;
	} exts[] = {{".wav", false}, {".ogg", true}};
	int n = 0;
	*max_frames = 0;
	*max_decoder = 0;
	for(int v = 0; v < AUDIO_MAX_VARIANTS; v++)
		for(size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
			struct audio_deferred_source src;
			memset(&src, 0, sizeof(src));
			int len = v == 0 ?
				snprintf(src.path, sizeof(src.path), "%s/%s%s", root, rel,
						 exts[e].ext) :
				snprintf(src.path, sizeof(src.path), "%s/%s%d%s", root, rel, v,
						 exts[e].ext);
			if(len < 0 || (size_t)len >= sizeof(src.path))
				continue; /* unrepresentable path: cannot be loaded later */
			if(!audio_dir_index_admits(src.path))
				continue; /* proven absent: the probe could not open it */
			src.is_ogg = exts[e].is_ogg;
			size_t decoder = 0;
			if(!audio_deferred_probe_ex(src.path, src.is_ogg, &src.frames,
										&src.rate, &decoder))
				continue;
			s_deferred_discovered++;
			if(src.frames > AUDIO_DEFERRED_MAX_BYTES / 2u
			   || decoder > AUDIO_DEFERRED_MAX_DECODER_BYTES) {
				s_deferred_oversize++;
				fprintf(stderr,
						"[audio] %s: %u frames / %u decoder bytes exceeds a "
						"deferred ceiling, variant not eligible\n",
						src.path, (unsigned)src.frames, (unsigned)decoder);
				/* A basename's .wav, once it PROBES, owns that basename --
				   the resident loader's rule too (audio_load_candidate takes
				   the .wav whenever it parses). So an oversize .wav is not
				   rescued by an .ogg sibling: one rule for both paths. */
				break;
			}
			if(decoder > *max_decoder)
				*max_decoder = decoder;
			s_deferred_src[n] = src;
			s_variants[id][n].pcm = NULL;
			s_variants[id][n].frames = src.frames;
			s_variants[id][n].rate = src.rate;
			if(src.frames > *max_frames)
				*max_frames = src.frames;
			n++;
			break; /* .wav wins over .ogg per basename (audio_load_candidate) */
		}
	return n;
}

/* Runs once per scan, between pass B and pass C. */
static void audio_deferred_setup(const char* pack) {
	s_deferred_active = false;
	s_deferred_discovered = 0;
	s_deferred_oversize = 0;
	s_deferred_unavailable = 0;
	s_deferred_scratch_bytes = 0;
	s_deferred_decoder_bytes = 0;
	/* Cleared WITH the counters (hostile review 06 #6): a record still owed
	   from an earlier opt-in scan must not be emitted with this scan's zeros. */
	s_deferred_scan_pending = false;
	if(!audio_deferred_wanted())
		return;
	s_deferred_scan_pending = true;

	const int id = SOUND_AMBIENT_CAVE;
	char rel[128];
	audio_sound_table_subpath(s_names[id], rel, sizeof(rel));
	uint32_t max_frames = 0;
	size_t max_decoder = 0;
	int n = audio_deferred_probe_root(id, pack, rel, &max_frames, &max_decoder);
	bool fell_back = false;
	if(n == 0 && strcmp(pack, AUDIO_DEFAULT_PACK) != 0) {
		s_ctr_root = AUDIO_CTR_DEFAULT; /* dir-index attribution only */
		n = audio_deferred_probe_root(id, AUDIO_DEFAULT_PACK, rel, &max_frames,
									  &max_decoder);
		s_ctr_root = AUDIO_CTR_PACK;
		fell_back = n > 0;
	}
	if(n == 0) {
		/* Nothing to defer; pass C reports the silent key as it always did. */
		memset(s_variants[id], 0, sizeof(s_variants[id]));
		return;
	}

	/* The scratch is not a sample: its refusal is reported as
	   deferred_unavailable, never as an arena reject, and the bump is rolled
	   back so pass C sees the arena exactly as if nothing had been tried.
	   max_frames <= AUDIO_DEFERRED_MAX_BYTES / 2, so the multiply cannot wrap. */
	size_t saved_used = s_arena_used;
	unsigned saved_rejects = s_arena_rejects;
	size_t cap = ((size_t)max_frames * 2u + (AUDIO_DEFERRED_ALIGN - 1u))
		& ~(size_t)(AUDIO_DEFERRED_ALIGN - 1u);
	int16_t* scratch = audio_arena_alloc(cap);
	/* OGG variants only (0 for an all-WAV pool): the one block stb_vorbis may
	   use for decoder state during a load, sized for the neediest variant. */
	size_t decoder_cap
		= (max_decoder + (AUDIO_DEFERRED_ALIGN - 1u))
		& ~(size_t)(AUDIO_DEFERRED_ALIGN - 1u);
	void* decoder = (scratch && decoder_cap) ? audio_arena_alloc(decoder_cap)
											 : NULL;
	if(!scratch || (decoder_cap && !decoder)
	   || !audio_deferred_attach(scratch, cap)) {
		s_arena_used = saved_used;
		s_arena_rejects = saved_rejects;
		memset(s_variants[id], 0, sizeof(s_variants[id]));
		s_deferred_unavailable++;
		fprintf(stderr,
				"[audio] deferred cave ambience unavailable (%s); using the "
				"resident path for \"%s\"\n",
				!scratch					   ? "no room for the scratch" :
					(decoder_cap && !decoder) ? "no room for the decoder block" :
												"no loader",
				s_names[id]);
		return;
	}
	audio_deferred_set_decoder_memory(decoder, decoder_cap);
	s_variant_count[id] = n;
	s_deferred_scratch_bytes = (unsigned)cap;
	s_deferred_decoder_bytes = (unsigned)decoder_cap;
	s_deferred_active = true;
	if(fell_back)
		s_keys_fallback++;
}

/* Resolve ONE key of the current scan: record the visit order, skip the
   deferred cave key, then pack-then-default per-key resolution with the
   fallback/protected/silent accounting. The loop body of the three passes,
   VERBATIM, factored out so the async loader (below) can run pass C one key
   at a time through exactly the same code. Whichever thread calls this is the
   arena's sole writer at that moment (audio_registry_scan's contract). */
static void audio_scan_one_key(int id, const char* pack) {
	audio_load_service();
	/* Brief E: the approved Warm bell is bundled and never pack-overridden. */
	if(id == SOUND_MENU_ACHIEVEMENT)
		pack = AUDIO_DEFAULT_PACK;
#ifdef CAVEWII_TESTING
	s_scan_order[s_scan_order_n++] = (uint16_t)id;
#endif
	if(s_deferred_active && id == SOUND_AMBIENT_CAVE)
		return; /* probed, not decoded: audio_deferred_setup */
	/* "step.grass" -> "sound/step/grass" (the shared table helper). */
	char rel[128];
	audio_sound_table_subpath(s_names[id], rel, sizeof(rel));

	s_variant_count[id] = 0;
	ptime_t t_key = time_get();
	s_ctr_root = AUDIO_CTR_PACK;
	int offered = audio_scan_key(id, pack, rel);
	/* Per-KEY fallback: only a key the pack provides NOTHING for falls
	   back — never mix pack and default variants in one pool. */
	if(s_variant_count[id] == 0 && strcmp(pack, AUDIO_DEFAULT_PACK) != 0) {
		/* The default pool now OWNS this key, so it is also the pool
		   the short/offered comparison below must be made against: a
		   pack file the arena refused is not a loss if the bundled
		   default then loaded whole. */
		s_ctr_root = AUDIO_CTR_DEFAULT;
		offered = audio_scan_key(id, AUDIO_DEFAULT_PACK, rel);
		s_ctr_root = AUDIO_CTR_PACK;
		/* Counted, not just done: "keys_fallback" is how a real-Wii run
		   says how much of what the player hears is still CaveWii's own
		   audio rather than the pack they selected. */
		if(s_variant_count[id] > 0)
			s_keys_fallback++;
	}
	uint64_t d_key = audio_ctr_raw(t_key, time_get());
	if(d_key > s_ctr_key_max_raw) {
		s_ctr_key_max_raw = d_key;
		s_ctr_key_max_id = id;
	}

	if(audio_key_protected(s_names[id])) {
		if(s_variant_count[id] == 0)
			s_protected_silent++;
		else if(s_variant_count[id] < offered)
			s_protected_short++;
	}

	if(s_variant_count[id] == 0)
		fprintf(stderr,
				"[audio] no file for \"%s\" (sound/%s*) in \"%s\" or "
				"the default pack; the event will be silent\n",
				s_names[id], rel + sizeof("sound/") - 1, pack);
}

/* The tail of every scan, once the LAST key is resolved: frees the directory
   index, reports a full arena, flushes the arena for the DSP. Runs on the
   thread that finished the scan's last key -- the main thread for a
   synchronous scan and for the async completion (audio_registry_async_poll,
   after the loader is joined). `flush` is false when every published key was
   already flushed one by one (the async path). */
static void audio_scan_epilogue(bool flush) {
	audio_dir_index_end();

	/* LOW-7: the diagnostic's scan context is only meaningful DURING a scan. */
	AUDIO_ARENA_REPORT_SCAN("", -1);

	if(s_arena_rejects)
		fprintf(stderr,
				"[audio] PCM arena full: %u sample file(s) skipped "
				"(%u/%u bytes used), %u protected key(s) silent, %u short\n",
				s_arena_rejects, (unsigned)s_arena_used,
				(unsigned)s_arena_size, s_protected_silent, s_protected_short);

#ifdef PLATFORM_WII
	/* The AESND DSP reads voice buffers by DMA from main memory: flush the
	   whole written arena once, after load (32-byte alignment is already
	   guaranteed per sample by audio_arena_alloc + mem2_alloc). */
	if(flush && s_arena_used)
		DCFlushRange(s_arena, (u32)s_arena_used);
#else
	(void)flush;
#endif
}

/* ===== pack cache, audio slice (pack-cache-0927) =======================

   OPT-IN, config "pack.cache" (default false; Astra astra-packcacheB-0927
   item 2). OFF: audio_pack_cache_wanted() is false and NOTHING below runs --
   no key, no read, no directory, no write; the scan is byte-identical.

   ON: the scan first computes this load's KEY and asks audio_cache_read for
   the newest valid generation built under it. HIT: the completed registry is
   restored in one sequential read (tables from offsets, arena bytes,
   counters) and the scan ends there -- no decode. MISS for any reason (none,
   content changed, corrupt/torn, other target): the scan runs exactly as
   with the cache off, and once it has COMPLETED (synchronously, or at the
   async completion on the main thread) the result is published as a new
   generation. A card that cannot take it just disables caching.

   KEY (Astra's audio key list): schema, converter, scan-policy and target
   versions; the ACTUAL arena capacity and the 32-byte alignment; the key
   table (count, names, AUDIO_MAX_VARIANTS); the deferred-cave setting and its
   policy version; whether the pack IS the bundled root; for a HELPER-
   MANAGED pack its manifest digest (astra-packkey-0927: the pack's files are
   trusted to the manifest the helper wrote from the installed bytes -- a
   manual edit that is not re-imported is NOT detected); and for every key,
   every variant and both extensions in every root not covered by a manifest
   (an unmanaged pack, and always the bundled default): each candidate file
   that exists, by root, relative name, size and EVERY byte. Size or mtime alone
   never establishes freshness. The async opt-in is not part of it: sync and
   async completions are identical.

   WHICH PACKS (audio_cache_policy below, measured): only a root that ships
   OGG sounds -- keyed by the manifest when helper-managed, by the full
   source hash otherwise; WAV-only roots bypass the cache (status "bypass"
   + the reason).

   audio.deferred_cave ON (the DEFAULT since 2026-10-21) is not cached in
   this slice (its variants live outside the arena as file references): the
   cache is bypassed -- neither read nor written -- exactly as if pack.cache
   were off. Only the explicit opt-out ("off") can use the cache; the key
   therefore always encodes "off" (the only mode that ever reads or writes an
   image), so an image can never be applied under the wrong mode. */

#define AUDIO_CACHE_DEFERRED_POLICY_VERSION 1u

static bool s_cache_write_pending;
static uint64_t s_cache_key;
static char s_cache_dir[320];
static const char* s_cache_state; /* NULL = cache off for this scan */
static const char* s_cache_reason = "";
static unsigned s_cache_key_us, s_cache_read_us, s_cache_write_us;
static struct audio_cache_image s_cache_img;

static bool audio_pack_cache_wanted(void) {
	JSON_Object* o = json_object(gstate.config_user.root);
	if(!o)
		return false;
	if(json_object_dothas_value_of_type(o, "pack.cache", JSONBoolean))
		return json_object_dotget_boolean(o, "pack.cache") == 1;
	const char* v = json_object_dotget_string(o, "pack.cache");
	return v && (strcmp(v, "true") == 0 || strcmp(v, "on") == 0);
}

static void audio_cache_dir_for(const char* pack, char* out, size_t size) {
	/* Bundled defaults live beside the application (sd:/apps/CavEX on
	   the Wii, the working directory on PC), never inside assets/. */
	if(strcmp(pack, AUDIO_DEFAULT_PACK) == 0)
		snprintf(out, size, ".cavewii/bundled-audio");
	else
		snprintf(out, size, "%.290s/.cavewii/audio", pack);
}

static uint64_t audio_cache_fnv_u32(uint64_t h, uint32_t v) {
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
					(uint8_t)(v >> 24)};
	return audio_cache_fnv(h, b, 4);
}

/* One candidate file into the digest: root tag, relative name, size, bytes.
   A file that does not exist contributes nothing -- its absence is part of
   the inventory through the names that ARE hashed. */
static uint64_t audio_cache_hash_file(uint64_t h, int root_tag,
									  const char* root, const char* rel) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if(!audio_dir_index_admits(path))
		return h;
	FILE* f = fopen(path, "rb");
	if(!f)
		return h;
	static uint8_t buf[4096];
	uint64_t size = 0;
	uint64_t body = AUDIO_CACHE_FNV_INIT;
	size_t got;
	while((got = fread(buf, 1, sizeof(buf), f)) > 0) {
		body = audio_cache_fnv(body, buf, got);
		size += got;
	}
	fclose(f);
	h = audio_cache_fnv_u32(h, (uint32_t)root_tag);
	h = audio_cache_fnv(h, rel, strlen(rel) + 1);
	h = audio_cache_fnv_u32(h, (uint32_t)size);
	h = audio_cache_fnv_u32(h, (uint32_t)(size >> 32));
	h = audio_cache_fnv(h, &body, sizeof(body)); /* host order: same host */
	return h;
}

static uint64_t audio_cache_key_compute(const char* pack, uint32_t target,
										size_t arena_cap,
										const struct pack_manifest* manifest) {
	bool own_index = s_dir_idx == NULL;
	if(own_index)
		audio_dir_index_begin();
	bool quiet = s_dir_idx_quiet;
	s_dir_idx_quiet = true; /* the key never spends the scan's skip counter */
	uint64_t h = AUDIO_CACHE_FNV_INIT;
	h = pack_cache_fold_code_identity(h); /* Fable M3: this build's code */
	h = audio_cache_fnv(h, "CWAUDIOKEY", 10);
	h = audio_cache_fnv_u32(h, AUDIO_CACHE_SCHEMA);
	h = audio_cache_fnv_u32(h, AUDIO_CACHE_CONVERTER_VERSION);
	h = audio_cache_fnv_u32(h, AUDIO_CACHE_POLICY_VERSION);
	h = audio_cache_fnv_u32(h, target);
	h = audio_cache_fnv_u32(h, (uint32_t)arena_cap);
	h = audio_cache_fnv_u32(h, 32u); /* audio_arena_alloc's alignment */
	h = audio_cache_fnv_u32(h, AUDIO_SOUND_COUNT);
	h = audio_cache_fnv_u32(h, AUDIO_MAX_VARIANTS);
	h = audio_cache_fnv_u32(h, AUDIO_PASS_COUNT);
	h = audio_cache_fnv_u32(h, 0u); /* audio.deferred_cave: off (the only cached mode; on, the default, bypasses) */
	h = audio_cache_fnv_u32(h, AUDIO_CACHE_DEFERRED_POLICY_VERSION);
	bool is_default = strcmp(pack, AUDIO_DEFAULT_PACK) == 0;
	h = audio_cache_fnv_u32(h, is_default ? 1u : 0u);
	/* astra-packkey-0927: a helper-managed pack is keyed by its digest
	   manifest (names, sizes and bytes the helper hashed at import) instead
	   of every source byte; the bundled root is always hashed in full. */
	h = audio_cache_fnv_u32(h, manifest ? 1u : 0u);
	if(manifest) {
		h = audio_cache_fnv(h, &manifest->digest, sizeof(manifest->digest));
		h = audio_cache_fnv_u32(h, manifest->files);
	}
	static const char* const exts[] = {".wav", ".ogg"};
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
		h = audio_cache_fnv(h, s_names[id], strlen(s_names[id]) + 1);
		char sub[128];
		audio_sound_table_subpath(s_names[id], sub, sizeof(sub));
		for(int r = 0; r < (is_default ? 1 : 2); r++) {
			if(r == 0 && manifest)
				continue; /* the pack root: covered by the manifest digest */
			const char* root = r == 0 ? pack : AUDIO_DEFAULT_PACK;
			s_ctr_root = r == 0 ? AUDIO_CTR_PACK : AUDIO_CTR_DEFAULT;
			for(int v = 0; v < AUDIO_MAX_VARIANTS; v++)
				for(int e = 0; e < 2; e++) {
					char rel[160];
					if(v == 0)
						snprintf(rel, sizeof(rel), "%s%s", sub, exts[e]);
					else
						snprintf(rel, sizeof(rel), "%s%d%s", sub, v, exts[e]);
					h = audio_cache_hash_file(h, r, root, rel);
				}
		}
	}
	s_ctr_root = AUDIO_CTR_PACK;
	s_dir_idx_quiet = quiet;
	if(own_index)
		audio_dir_index_end();
	return h;
}

/* The completed registry as a cache image (offsets, never pointers). False
   when it cannot be represented (a deferred/NULL variant). */
static bool audio_cache_image_build(struct audio_cache_image* img,
									uint64_t key, uint32_t target) {
	memset(img, 0, sizeof(*img));
	img->key = key;
	img->target = target;
	img->arena_used = (uint32_t)s_arena_used;
	img->keys_fallback = s_keys_fallback;
	img->protected_silent = s_protected_silent;
	img->protected_short = s_protected_short;
	img->arena_rejects = s_arena_rejects;
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
		img->counts[id] = (uint8_t)s_variant_count[id];
		for(int v = 0; v < s_variant_count[id]; v++) {
			const struct audio_sample* smp = &s_variants[id][v];
			if(!smp->pcm)
				return false;
			img->v[id][v].offset
				= (uint32_t)((const uint8_t*)smp->pcm - s_arena);
			img->v[id][v].frames = smp->frames;
			img->v[id][v].rate = smp->rate;
		}
	}
	return true;
}

/* Where the audio cache is used (astra-packkey-0927 "keep audio only where
   measured beneficial"). Measured on PC 2026-09-27 (tools/audio_cache_tool
   --bench, host numbers; the console numbers need a real Wii run):
     shipped beta-1.7.3 pack (163 WAV sounds):
       manifest key:  fresh decode 10.3 ms/load, cache hit 24.1 -> SLOWER
       full hash:     fresh decode 12.8 ms/load, cache hit 49.4 -> SLOWER
     ogg-bench pack (283 OGG sounds):
       manifest key:  fresh decode 141.0 ms/load, cache hit 24.1 -> 5.9x
   A WAV registry is a copy of the file bytes, so a cache that reads the
   same bytes plus a checksum cannot win; an OGG registry skips the vorbis
   decode. So the cache serves a root only when it ships an OGG sound:
     helper-managed pack  manifest lists a sound/ .ogg file -> manifest key
     unmanaged pack       an .ogg candidate exists -> full source hash
                          (Astra: unmanaged keeps full hashing or bypass)
     bundled root         an .ogg candidate exists -> full source hash
   and bypasses it otherwise -- no key, no read, no write. The Pack Helper
   transcodes sounds to PCM16 WAV at import, so a helper-managed pack is
   normally WAV-only and bypassed: the audio slice pays off only for packs
   installed with raw OGG sounds.
   Returns NULL = use the cache (*manifest_out = the manifest, or NULL for
   the full hash), else the bypass reason. */
static struct pack_manifest s_cache_manifest;

static bool audio_root_has_ogg(const char* root) {
	bool quiet = s_dir_idx_quiet;
	s_dir_idx_quiet = true;
	bool found = false;
	for(int id = 0; id < AUDIO_SOUND_COUNT && !found; id++) {
		char rel[128], path[512];
		audio_sound_table_subpath(s_names[id], rel, sizeof(rel));
		for(int v = 0; v < AUDIO_MAX_VARIANTS && !found; v++) {
			if(v == 0)
				snprintf(path, sizeof(path), "%s/%s.ogg", root, rel);
			else
				snprintf(path, sizeof(path), "%s/%s%d.ogg", root, rel, v);
			/* The directory index filters (no file opened); a name it admits
			   -- or every name, outside a scan -- is confirmed by opening. */
			if(audio_dir_index_admits(path)) {
				FILE* f = fopen(path, "rb");
				if(f) {
					fclose(f);
					found = true;
				}
			}
		}
	}
	s_dir_idx_quiet = quiet;
	return found;
}

static const char* audio_cache_policy(const char* pack,
									  const struct pack_manifest** manifest_out) {
	*manifest_out = NULL;
	if(strcmp(pack, AUDIO_DEFAULT_PACK) == 0)
		return audio_root_has_ogg(pack)
			? NULL
			: "bundled WAV root (a fresh load is faster, measured)";
	bool managed = pack_manifest_load(pack, &s_cache_manifest);
	if(s_cache_manifest.status == PACK_MANIFEST_ERROR)
		/* Fable M2: never key a managed pack as unmanaged for one load. */
		return "helper manifest unreadable this load (no memory / I/O)";
	if(managed) {
		if(s_cache_manifest.sound_ogg == 0)
			return "no OGG sounds in the pack (a fresh WAV load is faster, "
				   "measured)";
		*manifest_out = &s_cache_manifest;
		return NULL;
	}
	return audio_root_has_ogg(pack)
		? NULL
		: "unmanaged WAV pack (a fresh load is faster, measured)";
}

const char* audio_registry_cache_bypass_reason(void) {
	/* The root of the last completed scan (what publish_target publishes). */
	char pack[320];
	snprintf(pack, sizeof(pack), "%s", s_effective_root);
	const struct pack_manifest* manifest;
	return audio_cache_policy(pack, &manifest);
}

/* Scan prologue with the cache ON: true = restored (the scan is complete). */
static bool audio_cache_try_restore(const char* pack) {
	audio_cache_dir_for(pack, s_cache_dir, sizeof(s_cache_dir));
	const struct pack_manifest* manifest;
	const char* bypass = audio_cache_policy(pack, &manifest);
	if(bypass) {
		s_cache_state = "bypass";
		s_cache_reason = bypass;
		return false; /* no key, no read, no write: a plain fresh decode */
	}
	ptime_t t0 = time_get();
	s_cache_key = audio_cache_key_compute(pack, AUDIO_CACHE_HOST_TARGET,
										  s_arena_size, manifest);
	ptime_t t1 = time_get();
	s_cache_key_us = (unsigned)(time_diff_s(t0, t1) * 1.0e6f);
	const char* reason = "";
	enum audio_cache_result res = audio_cache_read(
		s_cache_dir, s_cache_key, AUDIO_CACHE_HOST_TARGET, &s_cache_img,
		s_arena, s_arena_size, &reason);
	s_cache_read_us = (unsigned)(time_diff_s(t1, time_get()) * 1.0e6f);
	s_cache_reason = reason;
	if(res != AUDIO_CACHE_HIT) {
		s_cache_state = "miss";
		s_cache_write_pending = true; /* publish once this scan completes */
		return false;
	}
	/* Validated in full by audio_cache_read: publish. */
	s_arena_used = s_cache_img.arena_used;
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
		s_variant_count[id] = s_cache_img.counts[id];
		for(int v = 0; v < AUDIO_MAX_VARIANTS; v++) {
			if(v < s_variant_count[id]) {
				s_variants[id][v].pcm
					= (const int16_t*)(s_arena + s_cache_img.v[id][v].offset);
				s_variants[id][v].frames = s_cache_img.v[id][v].frames;
				s_variants[id][v].rate = s_cache_img.v[id][v].rate;
			} else {
				memset(&s_variants[id][v], 0, sizeof(s_variants[id][v]));
			}
		}
	}
	s_keys_fallback = s_cache_img.keys_fallback;
	s_protected_silent = s_cache_img.protected_silent;
	s_protected_short = s_cache_img.protected_short;
	s_arena_rejects = s_cache_img.arena_rejects;
	s_cache_state = "hit";
	return true;
}

/* After a completed scan (sync tail, or the async completion on the main
   thread): publish the generation the prologue owed. */
static void audio_cache_after_scan(void) {
	if(!s_cache_write_pending)
		return;
	s_cache_write_pending = false;
	ptime_t t0 = time_get();
	bool ok = audio_cache_image_build(&s_cache_img, s_cache_key,
									  AUDIO_CACHE_HOST_TARGET)
		&& audio_cache_write(s_cache_dir, &s_cache_img, s_arena);
	s_cache_write_us = (unsigned)(time_diff_s(t0, time_get()) * 1.0e6f);
	s_cache_state = ok ? "written" : "unwritable";
	if(!ok)
		fprintf(stderr, "[audio] pack cache: could not publish into \"%s\"; "
						"caching disabled for this load\n",
				s_cache_dir);
}

const char* audio_registry_cache_status(void) {
	return s_cache_state ? s_cache_state : "off";
}

const char* audio_registry_cache_reason(void) {
	return s_cache_reason;
}

bool audio_registry_cache_publish_target(uint32_t target, char* dir_out,
										 size_t dir_size) {
	if(!s_registry_loaded || !s_arena || audio_registry_async_pending()
	   || s_deferred_active)
		return false;
	char pack[320];
	snprintf(pack, sizeof(pack), "%s", s_effective_root);
	char dir[320];
	audio_cache_dir_for(pack, dir, sizeof(dir));
	if(dir_out)
		snprintf(dir_out, dir_size, "%s", dir);
	const struct pack_manifest* manifest;
	if(audio_cache_policy(pack, &manifest))
		return false; /* bypassed pack: nothing to publish (see the policy) */
	uint64_t key = audio_cache_key_compute(pack, target, s_arena_size, manifest);
	return audio_cache_image_build(&s_cache_img, key, target)
		&& audio_cache_write(dir, &s_cache_img, s_arena);
}

/* Async pass C: declared here, defined with the rest of the module below.
   Returns true when pass C was handed to the loader thread (the caller must
   NOT run it, nor the epilogue); false = run pass C synchronously as before. */
static bool audio_async_begin(const char* pack);

/* Fill the variant tables + arena from the given pack (per-KEY default
   fallback). Precondition: arena allocated, bump/tables reset. */
static void audio_registry_scan(const char* pack) {
	/* The root these contents came from, for the audio_reload record and for
	   audio_first_play's "this sample was decoded from HERE" claim. */
	snprintf(s_effective_root, sizeof(s_effective_root), "%s", pack);
	s_keys_fallback = 0;
	s_protected_silent = 0;
	s_protected_short = 0;
	audio_ctr_reset();
	audio_dir_index_begin();
#ifdef CAVEWII_TESTING
	s_scan_order_n = 0;
#endif
	/* pack-cache-0927: opt-in prologue. OFF (the default): skipped entirely. */
	s_cache_write_pending = false;
	s_cache_state = NULL;
	s_cache_reason = "";
	s_cache_key_us = s_cache_read_us = s_cache_write_us = 0;
	if(audio_pack_cache_wanted() && !audio_deferred_wanted()
	   && audio_cache_try_restore(pack)) {
		audio_scan_epilogue(true); /* frees the index, flushes for the DSP */
		return;
	}
	/* Three passes over the one table (audio_key_pass): the approved UI sound
	   package, then note.*, then everything else in unchanged table order. */
	for(int pass = 0; pass < AUDIO_PASS_COUNT; pass++) {
		if(s_load_cancelled) break;
		/* C2 P2: the deferred key's scratch is reserved after the protected
		   passes and before the rest. A no-op unless the opt-in is on. */
		if(pass == AUDIO_PASS_REST) {
			audio_deferred_setup(pack);
			if(s_load_cancelled) break;
			/* asyncsnd-0926: with the opt-in on, pass C leaves this thread
			   here; the epilogue then runs at completion. Off (or the loader
			   thread unavailable): fall through, byte-identical to before. */
			if(audio_async_begin(pack))
				return;
		}
		for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
			if(s_load_cancelled) break;
			if(audio_key_pass(s_names[id]) != pass)
				continue;
			audio_scan_one_key(id, pack);
		}
	}

	audio_scan_epilogue(true);
	if(!s_load_cancelled)
		audio_cache_after_scan(); /* no-op unless the opt-in prologue owed one */
}

/* ===== boot-scan stats + its deferred observer record (HIGH-1b) =========

   audio_registry_reload emits its own audio_reload record inline, because a
   reload happens long after the observer is up. The BOOT scan cannot: it runs
   inside audio_init, before net bringup, so there is nothing to emit INTO yet
   -- which is why the one scan that decides what the whole session can play
   was, until now, invisible to every armed run (and to the NDEBUG Wii build,
   which carries none of this file's stderr). So the boot scan stashes its
   numbers here and main.c drains them once per frame, next to the other
   retained observer records. */
struct audio_load_stats {
	uint64_t service_gap_max_us;
	char root[256];
	unsigned variants;
	unsigned keys_fallback;
	unsigned arena_used;
	unsigned arena_cap;
	unsigned arena_rejects;
	unsigned protected_silent;
	unsigned protected_short;
	int ms;
	/* boot-sound-measure counters, microseconds (see "scan counters"). */
	unsigned scan_us;
	struct {
		unsigned open_miss_n, open_miss_us;
		unsigned open_hit_n, open_hit_us;
		unsigned read_bytes, read_us;
		unsigned conv_us;
		unsigned dir_n, dir_absent_n, dir_probe_n, dir_us, skip_n;
	} ctr[AUDIO_CTR_ROOTS];
	unsigned file_max_us;
	unsigned key_max_us;
	char key_max[48];
	/* asyncsnd-0926 (audio.h "async pass C"). */
	bool async;
	int sync_ms, ready_ms;
	unsigned async_skips, async_protected_pending, async_thread_failures,
		async_stalls;
	/* pack-cache-0927: empty = the opt-in was off for this scan. */
	char cache_state[16];
	char cache_reason[64];
	unsigned cache_key_us, cache_read_us, cache_write_us;
	bool pending_emit;
};

static struct audio_load_stats s_load_stats;

static void audio_load_stats_stash(const char* root, int ms,
								   uint64_t scan_raw) {
	unsigned variants = 0;
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++)
		variants += (unsigned)s_variant_count[id];
	snprintf(s_load_stats.root, sizeof(s_load_stats.root), "%s", root);
	s_load_stats.variants = variants;
	s_load_stats.keys_fallback = s_keys_fallback;
	s_load_stats.arena_used = (unsigned)s_arena_used;
	s_load_stats.arena_cap = (unsigned)s_arena_size;
	s_load_stats.arena_rejects = s_arena_rejects;
	s_load_stats.protected_silent = s_protected_silent;
	s_load_stats.protected_short = s_protected_short;
	s_load_stats.ms = ms;
	s_load_stats.service_gap_max_us = s_load_service_gap_max_us;
	s_load_stats.scan_us = audio_ctr_us(scan_raw);
	for(int r = 0; r < AUDIO_CTR_ROOTS; r++) {
		const struct audio_scan_ctr* c = &s_ctr[r];
		s_load_stats.ctr[r].open_miss_n = c->open_miss_n;
		s_load_stats.ctr[r].open_miss_us = audio_ctr_us(c->open_miss_raw);
		s_load_stats.ctr[r].open_hit_n = c->open_hit_n;
		s_load_stats.ctr[r].open_hit_us = audio_ctr_us(c->open_hit_raw);
		s_load_stats.ctr[r].read_bytes = c->read_bytes > 0xFFFFFFFFu ?
			0xFFFFFFFFu :
			(unsigned)c->read_bytes;
		s_load_stats.ctr[r].read_us = audio_ctr_us(c->read_raw);
		s_load_stats.ctr[r].conv_us = audio_ctr_us(c->conv_raw);
		s_load_stats.ctr[r].dir_n = c->dir_n;
		s_load_stats.ctr[r].dir_absent_n = c->dir_absent_n;
		s_load_stats.ctr[r].dir_probe_n = c->dir_probe_n;
		s_load_stats.ctr[r].dir_us = audio_ctr_us(c->dir_raw);
		s_load_stats.ctr[r].skip_n = c->skip_n;
	}
	s_load_stats.file_max_us = audio_ctr_us(s_ctr_file_max_raw);
	s_load_stats.key_max_us = audio_ctr_us(s_ctr_key_max_raw);
	snprintf(s_load_stats.key_max, sizeof(s_load_stats.key_max), "%s",
			 s_ctr_key_max_id >= 0 ? s_names[s_ctr_key_max_id] : "");
	{
		struct audio_async_status st;
		audio_registry_async_status(&st);
		s_load_stats.async = st.state == AUDIO_ASYNC_DONE;
		s_load_stats.sync_ms = s_load_stats.async ? st.sync_ms : ms;
		s_load_stats.ready_ms = ms;
		s_load_stats.async_skips = st.skips;
		s_load_stats.async_protected_pending = st.protected_pending;
		s_load_stats.async_thread_failures = st.thread_failures;
		s_load_stats.async_stalls = st.stalls;
	}
	/* pack-cache-0927: empty state = the opt-in was off (record unchanged). */
	snprintf(s_load_stats.cache_state, sizeof(s_load_stats.cache_state), "%s",
			 s_cache_state ? s_cache_state : "");
	snprintf(s_load_stats.cache_reason, sizeof(s_load_stats.cache_reason),
			 "%s", s_cache_state ? s_cache_reason : "");
	s_load_stats.cache_key_us = s_cache_key_us;
	s_load_stats.cache_read_us = s_cache_read_us;
	s_load_stats.cache_write_us = s_cache_write_us;
	s_load_stats.pending_emit = true;
}

void audio_registry_emit_load_stats(void) {
	/* Cheapest possible no-op on every frame of a normal run: one static bool.
	   Never blocks and never retries into a full ring -- a ring-full frame just
	   leaves the record pending for the next one, exactly like
	   carve_observer_emit_next's rule at the call site in main.c. */
	if(!s_load_stats.pending_emit || !audio_tel_armed())
		return;

	char line[1536];
	size_t off = (size_t)snprintf(line, sizeof(line),
								  "{\"event\":\"audio_load\",\"root\":");
	off = audio_tel_json_str(line, sizeof(line), off, s_load_stats.root);
	if(off >= sizeof(line)) {
		s_load_stats.pending_emit = false; /* unrepresentable; never retry */
		return;
	}
	off += (size_t)snprintf(
		line + off, sizeof(line) - off,
		",\"variants\":%u,\"keys_fallback\":%u,\"arena_used\":%u,"
		"\"arena_cap\":%u,\"arena_rejects\":%u,\"protected_silent\":%u,"
		"\"protected_short\":%u,\"ms\":%d",
		s_load_stats.variants, s_load_stats.keys_fallback,
		s_load_stats.arena_used, s_load_stats.arena_cap,
		s_load_stats.arena_rejects, s_load_stats.protected_silent,
		s_load_stats.protected_short, s_load_stats.ms);
	/* boot-sound-measure: appended AFTER "ms" so every existing consumer of
	   the record prefix is untouched. Unprefixed = the selected root;
	   fb_ = the bundled default root, per-key fallback probes. */
	for(int r = 0; r < AUDIO_CTR_ROOTS && off < sizeof(line); r++) {
		const char* px = r == AUDIO_CTR_PACK ? "" : "fb_";
		off += (size_t)snprintf(
			line + off, sizeof(line) - off,
			",\"%sopen_miss_n\":%u,\"%sopen_miss_us\":%u,"
			"\"%sopen_hit_n\":%u,\"%sopen_hit_us\":%u,"
			"\"%sread_bytes\":%u,\"%sread_us\":%u,\"%sconv_us\":%u",
			px, s_load_stats.ctr[r].open_miss_n, px,
			s_load_stats.ctr[r].open_miss_us, px, s_load_stats.ctr[r].open_hit_n,
			px, s_load_stats.ctr[r].open_hit_us, px,
			s_load_stats.ctr[r].read_bytes, px, s_load_stats.ctr[r].read_us, px,
			s_load_stats.ctr[r].conv_us);
	}
	if(off < sizeof(line))
		off += (size_t)snprintf(line + off, sizeof(line) - off,
								",\"scan_us\":%u,\"file_max_us\":%u,"
								"\"key_max_us\":%u,\"key_max\":",
								s_load_stats.scan_us, s_load_stats.file_max_us,
								s_load_stats.key_max_us);
	if(off < sizeof(line))
		off = audio_tel_json_str(line, sizeof(line), off, s_load_stats.key_max);
	/* sound-dir-index: appended after key_max (prefix unchanged again). */
	for(int r = 0; r < AUDIO_CTR_ROOTS && off < sizeof(line); r++) {
		const char* px = r == AUDIO_CTR_PACK ? "" : "fb_";
		off += (size_t)snprintf(
			line + off, sizeof(line) - off,
			",\"%sdir_n\":%u,\"%sdir_absent_n\":%u,\"%sdir_probe_n\":%u,"
			"\"%sdir_us\":%u,\"%sskip_n\":%u",
			px, s_load_stats.ctr[r].dir_n, px, s_load_stats.ctr[r].dir_absent_n,
			px, s_load_stats.ctr[r].dir_probe_n, px, s_load_stats.ctr[r].dir_us,
			px, s_load_stats.ctr[r].skip_n);
	}
	/* asyncsnd-0926: appended last (prefix unchanged again). async=false is
	   the synchronous scan; sync_ms is passes A+B, ready_ms the whole scan,
	   skips the plays dropped while pass C was pending. */
	if(off < sizeof(line))
		off += (size_t)snprintf(
			line + off, sizeof(line) - off,
			",\"async\":%s,\"sync_ms\":%d,\"ready_ms\":%d,\"async_skips\":%u,"
			"\"async_protected_pending\":%u,\"async_thread_failures\":%u,"
			"\"async_stalls\":%u,\"service_gap_max_us\":%llu}",
			s_load_stats.async ? "true" : "false", s_load_stats.sync_ms,
			s_load_stats.ready_ms, s_load_stats.async_skips,
			s_load_stats.async_protected_pending,
			s_load_stats.async_thread_failures, s_load_stats.async_stalls,
			(unsigned long long)s_load_stats.service_gap_max_us);
	/* pack-cache-0927: appended only when the opt-in was on for this scan,
	   so a cache-off record is byte-identical to before. */
	if(s_load_stats.cache_state[0] && off < sizeof(line)) {
		off--; /* reopen the object */
		off += (size_t)snprintf(
			line + off, sizeof(line) - off,
			",\"cache\":\"%s\",\"cache_reason\":",
			s_load_stats.cache_state);
		if(off < sizeof(line))
			off = audio_tel_json_str(line, sizeof(line), off,
									 s_load_stats.cache_reason);
		if(off < sizeof(line))
			off += (size_t)snprintf(
				line + off, sizeof(line) - off,
				",\"cache_key_us\":%u,\"cache_read_us\":%u,"
				"\"cache_write_us\":%u}",
				s_load_stats.cache_key_us, s_load_stats.cache_read_us,
				s_load_stats.cache_write_us);
	}
	if(off >= sizeof(line)) {
		s_load_stats.pending_emit = false;
		return;
	}
	if(audio_tel_emit_owed(line))
		s_load_stats.pending_emit = false;
}

/* ===== async pass C (asyncsnd-0926, Astra ruling 3) ======================

   Contract and threading rules: audio.h ("async pass C"). The whole safety
   argument in one place:

   ARENA WRITER. audio_registry_scan runs passes A and B (and the deferred
   setup) on the calling thread, then audio_async_begin hands pass C to the
   loader thread. From that moment until audio_registry_async_join returns,
   the loader is the ONLY code that touches the arena bump, s_variants,
   s_variant_count, the directory index, the scan counters and the PC arena
   report context. The main thread reads the tables only through the readers
   below, which consult the per-key `published` flags under s_async_lock while
   a run is in flight. The loader publishes a key AFTER its variants are
   complete and flushed, under the lock, and only if the generation it started
   under is still current. s_async_state itself is written by the main thread
   only (begin / poll / join), so the main thread reads it unlocked.

   MAIN THREAD NEVER BLOCKS on the loader except in audio_registry_async_join
   (shutdown, reload, retry), which is exactly the pack-switch "cancel and
   join" the ruling asks for. The watchdog's HANG rule therefore stays armed
   for the main thread throughout; a loader wedged inside libfat shows up as
   a progress timeout (FAILED, gate offers retry/back), and a retry's join
   then either returns or is the genuine hang the watchdog exists to report. */

#define AUDIO_ASYNC_THREAD_PRIO 44 /* deferred 40 < this < music/region-io 48 */
#define AUDIO_ASYNC_KEY_SLEEP_MS 1 /* a REAL sleep after every key */

static struct thread_mutex s_async_lock;
static bool s_async_sync_ready;
static enum audio_async_state s_async_state = AUDIO_ASYNC_OFF;
static uint32_t s_async_gen;	  /* bumped by every begin and every cancel */
static bool s_async_cancel;		  /* loader: stop before the next key */
static bool s_async_finished;	  /* loader has left its loop */
static bool s_async_thread_up;
static struct thread s_async_thread;
static uint8_t s_async_published[AUDIO_SOUND_COUNT]; /* 1 = key visible */
static uint16_t s_async_keys[AUDIO_SOUND_COUNT];	 /* pass C, table order */
static int s_async_key_n, s_async_next, s_async_done_n;
static char s_async_pack[256];
static int64_t s_async_progress_ms; /* last publication, module clock */
static ptime_t s_async_t_start;		/* the scan's own start (load/reload) */
static int s_async_sync_ms, s_async_ready_ms = -1;
static bool s_async_is_reload;		/* which record the completion owes */
static unsigned s_async_skips, s_async_protected_pending,
	s_async_thread_failures, s_async_stalls;
static unsigned s_async_ogg_sync;	 /* runs kept synchronous by the OGG probe */
static unsigned s_async_ogg_decoded; /* OGG decodes on the loader: must be 0 */

static void audio_async_note_ogg_decode(void) {
	if(s_async_state == AUDIO_ASYNC_RUNNING && s_async_thread_up)
		s_async_ogg_decoded++; /* benign race: a diagnostic counter */
}

#ifdef CAVEWII_TESTING
static bool s_test_async_manual;	  /* no thread: tests step by hand */
static bool s_test_async_fail_thread; /* simulate thread_create failure */
static int64_t s_test_async_clock_ms = -1;
void audio_registry_test_async_manual(bool manual);
void audio_registry_test_async_manual(bool manual) {
	s_test_async_manual = manual;
}
void audio_registry_test_async_fail_thread(bool fail);
void audio_registry_test_async_fail_thread(bool fail) {
	s_test_async_fail_thread = fail;
}
void audio_registry_test_async_set_clock_ms(int64_t ms);
void audio_registry_test_async_set_clock_ms(int64_t ms) {
	s_test_async_clock_ms = ms;
}
#endif

static ptime_t s_async_clock_origin;

static int64_t audio_async_now_ms(void) {
#ifdef CAVEWII_TESTING
	if(s_test_async_clock_ms >= 0)
		return s_test_async_clock_ms;
#endif
	return (int64_t)time_diff_ms(s_async_clock_origin, time_get());
}

static bool audio_async_wanted(void) {
	return strcmp(config_read_string(&gstate.config_user, "audio.async_load",
									 "off"),
				  "on")
		== 0;
}

static bool audio_async_sync_init(void) {
	if(s_async_sync_ready)
		return true;
	if(!thread_mutex_init(&s_async_lock))
		return false;
	s_async_clock_origin = time_get();
	s_async_sync_ready = true;
	return true;
}

/* Is key `id` visible to a reader RIGHT NOW? Unlocked fast path while no run
   is in flight (the common case, and the whole cost of the opt-in being off:
   one static compare). During a run the lock is the acquire barrier that
   pairs with the loader's publication. */
static bool audio_key_visible(uint16_t id, bool is_pick) {
	if(s_async_state == AUDIO_ASYNC_OFF || s_async_state == AUDIO_ASYNC_DONE)
		return true;
	thread_mutex_lock(&s_async_lock);
	bool vis = s_async_published[id] != 0;
	if(!vis && is_pick) {
		/* Counted for PICKS only: the number of plays actually dropped. */
		s_async_skips++;
		if(audio_key_protected(s_names[id]))
			s_async_protected_pending++; /* invariant failure, never normal */
	}
	thread_mutex_unlock(&s_async_lock);
	return vis;
}

static void* audio_async_thread_main(void* arg) {
	(void)arg;
	while(audio_registry_async_step())
		/* A REAL sleep, not a yield (the deferred loader's Astra D2 rule):
		   bounds this thread's share against lower-priority peers, and lets
		   the card breathe between keys. */
		thread_msleep(AUDIO_ASYNC_KEY_SLEEP_MS);
	return NULL;
}

static bool audio_async_begin(const char* pack) {
	if(!audio_async_wanted())
		return false;
	if(!audio_async_sync_init())
		return false; /* no lock, no thread: the synchronous path */
	/* Never two arena writers: every caller (load/reload) has joined the
	   previous run already, so this is a guard, not a path -- and the guard
	   JOINS rather than falling through into a synchronous pass C beside a
	   live loader (review L3). */
	if(s_async_state == AUDIO_ASYNC_RUNNING || s_async_thread_up) {
		assert(!"audio_async_begin: a loader run is still in flight");
		audio_registry_async_join();
	}

	/* Astra clause: "unsupported raw-OGG inputs retain the existing synchronous
	   path". Prepared packs are PCM16 WAV (the Pack Helper transcodes); a raw
	   Beta-era pack ships .ogg, and its stb_vorbis decodes (whole-file buffer
	   plus the full decoded block, transiently malloc'd) must not run beside
	   the title's own allocations on a second thread with no measurement of
	   the overlapped heap peaks. So: if ANY pass-C candidate .ogg exists in the
	   selected root, the WHOLE pass C stays synchronous -- the same bytes, the
	   same order, just no early title -- and the run is counted (ogg_sync).
	   Whole-scan, not per key, because pulling one key forward would change
	   the arena allocation order the equality proof pins. Asked through the
	   directory index only (no file is opened; a directory the index cannot
	   list answers "maybe", which keeps the synchronous path). The bundled
	   fallback root is the CC0 WAV set by construction (this file's header)
	   and is not probed; audio_async_note_ogg_decode counts any OGG that
	   reaches the loader regardless. */
	{
		bool ogg = false;
		s_dir_idx_quiet = true;
		for(int id = 0; id < AUDIO_SOUND_COUNT && !ogg; id++) {
			if(audio_key_pass(s_names[id]) != AUDIO_PASS_REST)
				continue;
			char rel[128], path[512];
			audio_sound_table_subpath(s_names[id], rel, sizeof(rel));
			for(int v = 0; v < AUDIO_MAX_VARIANTS && !ogg; v++) {
				if(v == 0)
					snprintf(path, sizeof(path), "%s/%s.ogg", pack, rel);
				else
					snprintf(path, sizeof(path), "%s/%s%d.ogg", pack, rel, v);
				ogg = audio_dir_index_admits(path);
			}
		}
		s_dir_idx_quiet = false;
		if(ogg) {
			s_async_ogg_sync++;
			fprintf(stderr,
					"[audio] async sound load: \"%s\" carries raw .ogg sounds; "
					"pass C stays synchronous\n",
					pack);
			return false;
		}
	}

	thread_mutex_lock(&s_async_lock);
	s_async_key_n = 0;
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
		bool rest = audio_key_pass(s_names[id]) == AUDIO_PASS_REST;
		/* Passes A/B are complete and, with the cave key, already final. */
		s_async_published[id] = rest ? 0 : 1;
		if(rest)
			s_async_keys[s_async_key_n++] = (uint16_t)id;
	}
	snprintf(s_async_pack, sizeof(s_async_pack), "%s", pack);
	s_async_gen++;
	s_async_cancel = false;
	s_async_finished = false;
	s_async_next = 0;
	s_async_done_n = 0;
	s_async_progress_ms = audio_async_now_ms();
	s_async_sync_ms = (int)time_diff_ms(s_async_t_start, time_get());
	s_async_ready_ms = -1;
	thread_mutex_unlock(&s_async_lock);

#ifdef PLATFORM_WII
	/* Passes A/B are about to be PLAYABLE while pass C is still loading: the
	   DSP must see them now, not at the epilogue's whole-arena flush. */
	if(s_arena_used)
		DCFlushRange(s_arena, (u32)s_arena_used);
#endif

	bool thread_ok;
#ifdef CAVEWII_TESTING
	thread_ok = !s_test_async_fail_thread;
	if(thread_ok && s_test_async_manual) {
		s_async_state = AUDIO_ASYNC_RUNNING;
		return true; /* the test drives audio_registry_async_step */
	}
#else
	thread_ok = true;
#endif
	if(thread_ok)
		thread_ok = thread_create(&s_async_thread, audio_async_thread_main,
								  NULL, AUDIO_ASYNC_THREAD_PRIO);
	if(!thread_ok) {
		/* Thread-creation failure: pass C completes synchronously right here
		   (the caller falls through) -- same bytes, just no early title. The
		   flags stay "all published" for the synchronous readers. */
		s_async_thread_failures++;
		fprintf(stderr, "[audio] async sound load: loader thread unavailable; "
						"finishing the scan synchronously\n");
		thread_mutex_lock(&s_async_lock);
		memset(s_async_published, 1, sizeof(s_async_published));
		thread_mutex_unlock(&s_async_lock);
		return false;
	}
	s_async_thread_up = true;
	s_async_state = AUDIO_ASYNC_RUNNING;
	return true;
}

bool audio_registry_async_step(void) {
	if(!s_async_sync_ready)
		return false;
	thread_mutex_lock(&s_async_lock);
	if(s_async_cancel || s_async_next >= s_async_key_n) {
		s_async_finished = true;
		thread_mutex_unlock(&s_async_lock);
		return false;
	}
	int id = s_async_keys[s_async_next];
	uint32_t gen = s_async_gen;
	thread_mutex_unlock(&s_async_lock);

	/* The one bounded unit of arena work: one key, pack then fallback. */
	audio_scan_one_key(id, s_async_pack);
#ifdef PLATFORM_WII
	/* Flushed for the DSP BEFORE publication, per variant (each is 32-byte
	   aligned by audio_arena_alloc). */
	for(int v = 0; v < s_variant_count[id]; v++)
		if(s_variants[id][v].pcm)
			DCFlushRange((void*)s_variants[id][v].pcm,
						 (u32)s_variants[id][v].frames * 2u);
#endif

	thread_mutex_lock(&s_async_lock);
	bool more;
	if(gen != s_async_gen || s_async_cancel) {
		/* Stale completion: a cancel landed while this key loaded. The bytes
		   are in an arena the main thread is about to reset; publish nothing. */
		s_async_finished = true;
		more = false;
	} else {
		s_async_published[id] = 1;
		s_async_done_n++;
		s_async_next++;
		s_async_progress_ms = audio_async_now_ms();
		more = s_async_next < s_async_key_n;
		if(!more)
			s_async_finished = true;
	}
	thread_mutex_unlock(&s_async_lock);
	return more;
}

void audio_registry_async_join(void) {
	if(!s_async_sync_ready)
		return;
	thread_mutex_lock(&s_async_lock);
	s_async_cancel = true;
	s_async_gen++; /* whatever the loader still publishes is stale */
	thread_mutex_unlock(&s_async_lock);
	if(s_async_thread_up) {
		thread_join(&s_async_thread);
		s_async_thread_up = false;
	}
	/* A joined run is over, complete or not; the next scan resets everything
	   (memset of the tables + arena bump), and the directory index a
	   cancelled run left behind is freed with it. */
	if(s_async_state == AUDIO_ASYNC_RUNNING || s_async_state == AUDIO_ASYNC_FAILED)
		audio_dir_index_end();
	s_async_state = AUDIO_ASYNC_OFF;
}

bool audio_registry_async_pending(void) {
	return s_async_state == AUDIO_ASYNC_RUNNING
		|| s_async_state == AUDIO_ASYNC_FAILED;
}

void audio_registry_async_status(struct audio_async_status* out) {
	if(!out)
		return;
	memset(out, 0, sizeof(*out));
	out->state = s_async_state;
	out->ready_ms = -1;
	if(!s_async_sync_ready)
		return;
	thread_mutex_lock(&s_async_lock);
	out->keys_done = s_async_done_n;
	out->keys_total = s_async_key_n;
	out->generation = s_async_gen;
	out->skips = s_async_skips;
	out->protected_pending = s_async_protected_pending;
	out->thread_failures = s_async_thread_failures;
	out->stalls = s_async_stalls;
	out->sync_ms = s_async_sync_ms;
	out->ready_ms = s_async_ready_ms;
	out->ogg_sync = s_async_ogg_sync;
	out->ogg_decoded_async = s_async_ogg_decoded;
	thread_mutex_unlock(&s_async_lock);
}

/* The reload's telemetry record, factored so the async completion can emit
   it with the FULL numbers instead of the pass-A/B partial. */
struct audio_reload_tel {
	bool armed;
	ptime_t t_start;
	int mem1_before, mem2_before;
	int32_t stop_ms;
	int voices_after_stop;
};
static struct audio_reload_tel s_reload_tel;
static void audio_reload_record_emit(void);

void audio_registry_async_poll(void) {
	if(s_load_servicing) return;
	/* FAILED is polled too (review L1): a loader that was merely slow and
	   then finished whole becomes DONE -- world entry opens, no re-decode. */
	if(s_async_state != AUDIO_ASYNC_RUNNING && s_async_state != AUDIO_ASYNC_FAILED)
		return;
	/* The main loop is servicing an async load too. Record those turns rather
	 * than treating the worker's entire lifetime as one main-thread gap. */
	audio_load_service();
	thread_mutex_lock(&s_async_lock);
	bool finished = s_async_finished;
	bool complete = s_async_done_n == s_async_key_n;
	int64_t since_progress = audio_async_now_ms() - s_async_progress_ms;
	thread_mutex_unlock(&s_async_lock);

	if(finished && complete) {
		if(s_async_thread_up) {
			thread_join(&s_async_thread); /* it has left its loop: prompt */
			s_async_thread_up = false;
		}
		/* Now the ONLY thread touching the scan state again: the epilogue,
		   then the record the run owes (boot: stash; reload: emit). */
		audio_scan_epilogue(false);
		audio_cache_after_scan(); /* pack-cache-0927: no-op when off */
		audio_load_finish();
		int total_ms = (int)time_diff_ms(s_async_t_start, time_get());
		thread_mutex_lock(&s_async_lock);
		s_async_ready_ms = total_ms;
		thread_mutex_unlock(&s_async_lock);
		s_async_state = AUDIO_ASYNC_DONE;
		if(s_async_is_reload) {
			s_tel_low_active = false;
			fprintf(stderr,
					"[audio] registry reloaded from \"%s\" (%u/%u bytes, async "
					"%d ms)\n",
					s_effective_root, (unsigned)s_arena_used,
					(unsigned)s_arena_size, total_ms);
			audio_reload_record_emit();
		} else {
			audio_load_stats_stash(s_effective_root, total_ms,
								   audio_ctr_raw(s_async_t_start, time_get()));
		}
		return;
	}
	if(s_async_state == AUDIO_ASYNC_RUNNING && !finished
	   && since_progress > AUDIO_ASYNC_PROGRESS_TIMEOUT_MS) {
		/* Separate loader-progress timeout: declared once, never renewed. The
		   thread is NOT joined here (it may be wedged in a card read); a retry
		   (audio_registry_reload) joins it, and a genuine wedge there is the
		   main-thread hang the watchdog reports. */
		s_async_stalls++;
		s_async_state = AUDIO_ASYNC_FAILED;
		audio_load_fail("Sound loader stalled; waiting for its current key to retire.");
		fprintf(stderr,
				"[audio] async sound load stalled: no key published for %d ms "
				"(%d/%d done); world entry refused until a retry succeeds\n",
				(int)since_progress, s_async_done_n, s_async_key_n);
	}
}

void audio_registry_load(void) {
	if(s_load_servicing) return;
	if(s_registry_loaded)
		return;
	audio_load_begin();
	s_registry_loaded = true;

	rand_gen_seed(&s_pick_rng);

	/* The arena is the reserved MEM2 audio region, NOT a carve allocation.
	   The grant may be SHORT of the request when arena2 could not hold the
	   full region behind the carve (mem2.c's ordering argument) — honour what
	   we were given and let the scan below drop what does not fit, exactly as
	   it already does for a pack that overflows a full-size arena. */
	size_t granted = 0;
	s_arena = mem2_audio_region(AUDIO_ARENA_BYTES, &granted);
	if(!s_arena || !granted) {
		s_arena = NULL;
		audio_load_disable("No PCM arena available for required sounds.");
		s_load_scanning = false;
		s_load_clock_live = false;
		fprintf(stderr, "[audio] no PCM arena reserved (wanted %u bytes); "
						"audio disabled for this session\n",
				(unsigned)AUDIO_ARENA_BYTES);
		return;
	}
	s_arena_size = granted;
	s_arena_used = 0;
#ifdef CAVEWII_TESTING
	s_test_arena_granted = granted;
	if(s_test_arena_cap && s_test_arena_cap < s_arena_size)
		s_arena_size = s_test_arena_cap;
#endif
	/* Never a silent success: a short region means resident samples WILL be
	   dropped, and audio_arena_total() carries the shortfall to the F3 "snd:
	   arena used/total" row — the only sink that exists in the NDEBUG Wii
	   build, where this stderr line does not. */
	if(granted < AUDIO_ARENA_BYTES)
		fprintf(stderr,
				"[audio] PCM arena CLIPPED to %u of %u bytes (MEM2 arena2 "
				"could not hold the full region behind the carve); the "
				"keys scanned last (ordinary keys, after the protected "
				"UI/note passes) will be silent\n",
				(unsigned)granted, (unsigned)AUDIO_ARENA_BYTES);

	char pack[256]; /* == config_read_path's SD-root buffer */
	ptime_t t_start = time_get();
	audio_effective_pack(pack, sizeof(pack));
	ptime_t t_scan = time_get();
	s_async_t_start = t_start; /* the async completion's clock origin */
	s_async_is_reload = false;
	audio_registry_scan(pack);
	if(s_async_state == AUDIO_ASYNC_RUNNING)
		return; /* pass C in flight: audio_registry_async_poll stashes */
	audio_load_finish();
	uint64_t scan_raw = audio_ctr_raw(t_scan, time_get());
	/* HIGH-1: the boot scan is what decides what the whole session can
	   play, and until now it reported NOTHING to an armed run -- only the
	   reload did, and the NDEBUG Wii build carries none of the stderr
	   above. Stash the numbers; main.c drains them through
	   audio_registry_emit_load_stats() once the observer can take a
	   record. */
	audio_load_stats_stash(pack, (int)time_diff_ms(t_start, time_get()),
						   scan_raw);
}

#ifdef CAVEWII_TESTING
/* How many times the arena was re-decoded. The pack screen must not pay for a
   reload when the audio root did not actually move (real-Wii A/B 2026-09-17:
   ~25 s per needless reload), and "did not reload" is only observable as a
   count. Test-only, so no production build carries it -- the
   displaylist_test_fail_* precedent (source/platform/displaylist.h:180-184). */
static unsigned s_test_reload_count;
unsigned audio_registry_test_reload_count(void);
unsigned audio_registry_test_reload_count(void) { return s_test_reload_count; }
void audio_registry_test_reload_count_reset(void);
void audio_registry_test_reload_count_reset(void) { s_test_reload_count = 0; }

void audio_registry_test_arena_stats(unsigned* rejects,
									 unsigned* protected_silent,
									 unsigned* protected_short) {
	if(rejects)
		*rejects = s_arena_rejects;
	if(protected_silent)
		*protected_silent = s_protected_silent;
	if(protected_short)
		*protected_short = s_protected_short;
}

/* The last scan's counters for one root (0 = selected, 1 = bundled default),
   for a test that must compare a pack reload's probe work without scraping
   the boot-only observer record: out[] = open_miss_n, open_hit_n,
   read_bytes, dir_n, dir_absent_n, dir_probe_n, skip_n. */
void audio_registry_test_scan_ctr(int root, unsigned long long out[7]);
void audio_registry_test_scan_ctr(int root, unsigned long long out[7]) {
	const struct audio_scan_ctr* c
		= &s_ctr[root == AUDIO_CTR_DEFAULT ? AUDIO_CTR_DEFAULT : AUDIO_CTR_PACK];
	out[0] = c->open_miss_n;
	out[1] = c->open_hit_n;
	out[2] = c->read_bytes;
	out[3] = c->dir_n;
	out[4] = c->dir_absent_n;
	out[5] = c->dir_probe_n;
	out[6] = c->skip_n;
}

int audio_registry_test_scan_order(uint16_t* ids, int cap) {
	int n = s_scan_order_n < cap ? s_scan_order_n : cap;
	for(int k = 0; k < n; k++)
		ids[k] = s_scan_order[k];
	return n;
}
#endif

void audio_registry_reload(void) {
	if(s_load_servicing) return;
	/* MAIN THREAD ONLY — the audio_* contract (audio.c header): the packs
	   screen, clin_process and audio_update all run there, so no engine-side
	   caller can race this. The off-thread readers are handled below. */
	/* asyncsnd-0926: a pass C still loading (or stalled) is cancelled and
	   JOINED before anything else -- before the counters below are cleared
	   (the loader bumps s_arena_rejects on a full arena), before one voice is
	   stopped and before one arena byte is rewritten. No-op when nothing is
	   in flight. Its generation is retired; nothing it published survives the
	   memset below and nothing it still completes is ever published. */
	audio_registry_async_join();
	/* LOW-6: these describe the LAST scan. Clear them FIRST, before either
	   early return below, so a reload that cannot rescan can never leave a
	   previous scan's numbers standing in the stderr line, the observer
	   record or the F3 row. audio_registry_scan re-zeroes them anyway on
	   every path that does rescan. */
	s_arena_rejects = 0;
	s_protected_silent = 0;
	s_protected_short = 0;
#ifdef CAVEWII_TESTING
	s_test_reload_count++;
#endif
	if(!s_registry_loaded) {
		audio_registry_load(); /* first touch: a plain load */
		return;
	}
	if(!s_arena) {
		/* Boot-time region reservation failed => audio is off for the session
		   (the S0 rule); a pack swap cannot resurrect it, and re-asking for
		   the region would get the same latched empty grant. */
		fprintf(stderr, "[audio] reload skipped: no PCM arena (audio was "
						"disabled at init)\n");
		audio_load_disable("No PCM arena available for required sounds.");
		return;
	}
	audio_load_begin();

	/* Order is load-bearing: every voice must be STOPPED before one arena
	   byte is rewritten. PC: audio_stop_all zeroes the voice array under
	   the mixer lock, so the device callback observes only stopped voices
	   from here on. Wii: AESND_SetVoiceStop on every slot + cleared poll
	   deadlines; the stop takes effect at the next 2ms DSP block, so give
	   it one block of margin before rewriting. (The arena itself is never
	   freed/unmapped — the S0 steal-safety argument — so the failure mode
	   this margin closes is a cosmetic last-block glitch, not memory
	   unsafety.) */
	/* Evidence gate, decided ONCE before any measurement: everything below
	   that exists only for telemetry is behind `tel`. */
	bool tel = audio_tel_armed();
	ptime_t t_start = time_get();
	int mem1_before = tel ? audio_tel_mem1_free_kb() : -1;
	int mem2_before = tel ? audio_tel_mem2_free_kb() : -1;

	ptime_t t_stop = time_get();
	audio_stop_all();
#ifdef PLATFORM_WII
	thread_msleep(3); /* > one 2ms AESND block: DMA of stopping voices done */
#endif
	/* C2 P2: the deferred scratch lives INSIDE this arena. audio_stop_all has
	   already cancelled any pending load; this waits until the loader thread
	   is outside its current step (at most one outstanding file read), after
	   which it can never write the scratch again until the rescan re-attaches
	   it. Only then may one arena byte be rewritten. No-op when the opt-in is
	   off (the module was never brought up). */
	audio_deferred_quiesce();
	/* Check (1): the drain really finished before one arena byte is rewritten.
	   audio_voices_active() (audio.h) counts the ENGINE-side voice records the
	   mixer reads; AESND's own per-voice DSP state is not queryable from this
	   file -- see the audio_first_play note for the accessor that is missing.
	   Both readings are behind `tel`: a disarmed reload pays the two
	   unconditional time_get() calls above and nothing else. */
	int32_t stop_ms = 0;
	int voices_after_stop = 0;
	if(tel) {
		stop_ms = time_diff_ms(t_stop, time_get());
		voices_after_stop = audio_voices_active();
	}

	/* Reset the bump arena IN PLACE (the SAME reserved region — mem2.h's
	   first-caller-wins grant is what guarantees that) + the variant tables,
	   then re-scan against the CURRENT pack choice. s_arena_size keeps the
	   granted size, so a clipped region stays clipped across reloads. */
	s_arena_used = 0;
#ifdef CAVEWII_TESTING
	/* Re-derive the usable size from the ORIGINAL grant, so one test process
	   can measure a roomy arena and then a starved one. Production leaves
	   s_arena_size exactly as the load set it -- a clipped region stays
	   clipped across reloads, which is the documented behaviour. */
	if(s_test_arena_granted) {
		s_arena_size = s_test_arena_granted;
		if(s_test_arena_cap && s_test_arena_cap < s_arena_size)
			s_arena_size = s_test_arena_cap;
	}
#endif
	memset(s_variants, 0, sizeof(s_variants));
	memset(s_variant_count, 0, sizeof(s_variant_count));

	char pack[256]; /* == config_read_path's SD-root buffer */
	audio_effective_pack(pack, sizeof(pack));
	s_tel_low_mem1 = -1;
	s_tel_low_mem2 = -1;
	s_tel_low_active = tel;
	s_reload_tel.armed = tel;
	s_reload_tel.t_start = t_start;
	s_reload_tel.mem1_before = mem1_before;
	s_reload_tel.mem2_before = mem2_before;
	s_reload_tel.stop_ms = stop_ms;
	s_reload_tel.voices_after_stop = voices_after_stop;
	s_async_t_start = t_start;
	s_async_is_reload = true;
	audio_registry_scan(pack);
	if(s_async_state == AUDIO_ASYNC_RUNNING)
		return; /* pass C in flight: audio_registry_async_poll finishes this */
	audio_load_finish();
	s_tel_low_active = false;
	fprintf(stderr, "[audio] registry reloaded from \"%s\" (%u/%u bytes)\n",
			pack, (unsigned)s_arena_used, (unsigned)s_arena_size);
	audio_reload_record_emit();
}

bool audio_registry_retry(void) {
	if(s_load_ready == AUDIO_REGISTRY_DISABLED || s_load_servicing
	   || s_load_scanning || audio_registry_async_pending())
		return false;
	audio_registry_reload();
	return true;
}

/* The audio_reload record + first-play arming, from the numbers the reload
   captured before its scan (s_reload_tel). Inline for a synchronous reload,
   from audio_registry_async_poll once an async pass C has completed. */
static void audio_reload_record_emit(void) {
	if(!s_reload_tel.armed)
		return;

	int variants = 0;
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++)
		variants += s_variant_count[id];

	char line[768];
	size_t off = 0;
	off += (size_t)snprintf(line, sizeof(line),
							"{\"event\":\"audio_reload\",\"root\":");
	off = audio_tel_json_str(line, sizeof(line), off, s_effective_root);
	if(off >= sizeof(line))
		return;
	off += (size_t)snprintf(
		line + off, sizeof(line) - off,
		",\"fallback\":\"%s\",\"variants\":%d,\"keys_fallback\":%u,"
		"\"arena_used\":%u,\"arena_cap\":%u,\"arena_rejects\":%u,"
		"\"protected_silent\":%u,\"protected_short\":%u,"
		"\"ms\":%d,\"stop_ms\":%d,\"voices_active_after_stop\":%d,"
		"\"mem1_free_kb\":[%d,%d,%d],\"mem2_free_kb\":[%d,%d,%d],"
		"\"carve_free_kb\":%d,\"service_gap_max_us\":%llu,\"async\":%s}",
		s_effective_fallback, variants, s_keys_fallback,
		(unsigned)s_arena_used, (unsigned)s_arena_size, s_arena_rejects,
		s_protected_silent, s_protected_short,
		(int)time_diff_ms(s_reload_tel.t_start, time_get()),
		(int)s_reload_tel.stop_ms, s_reload_tel.voices_after_stop,
		s_reload_tel.mem1_before, s_tel_low_mem1, audio_tel_mem1_free_kb(),
		s_reload_tel.mem2_before, s_tel_low_mem2, audio_tel_mem2_free_kb(),
		mem2_stat_carve_free_kb(),
		(unsigned long long)s_load_service_gap_max_us,
		s_async_state == AUDIO_ASYNC_DONE ? "true" : "false");
	if(off >= sizeof(line))
		return;
	(void)audio_tel_emit(line);
	/* Arm the "it actually plays" record for this registry's contents. The
	   stash starts empty: nothing picked from the PREVIOUS contents may be
	   reported against this root. */
	s_pending_sound_id = -1;
	s_pending_variant = -1;
	s_first_play_pending = true;
}

/* ===== registry queries ================================================= */

const char* audio_sound_name(uint16_t sound_id) {
	return sound_id < AUDIO_SOUND_COUNT ? s_names[sound_id] : NULL;
}

int audio_registry_variant_count(uint16_t sound_id) {
	if(sound_id >= AUDIO_SOUND_COUNT || !audio_key_visible(sound_id, false))
		return 0;
	return s_variant_count[sound_id];
}

bool audio_registry_variant_info(uint16_t sound_id, int variant,
								 uint32_t* frames, uint32_t* rate) {
	if(sound_id >= AUDIO_SOUND_COUNT || variant < 0
	   || !audio_key_visible(sound_id, false)
	   || variant >= s_variant_count[sound_id])
		return false;
	if(frames)
		*frames = s_variants[sound_id][variant].frames;
	if(rate)
		*rate = s_variants[sound_id][variant].rate;
	return true;
}

/* Check (5), "the selected audio actually PLAYS".

   A pick is not a play. audio_play3d picks a variant and only then applies
   [RenderGlobal.playSound]'s distance cull, so a record emitted at pick time
   would claim a 16-block-away footstep the player never heard. The record is
   therefore assembled in two steps: the pick stashes what only this file
   knows (the key, the chosen variant, and the ROOT the arena was filled
   from), and audio_registry_note_voice_started -- which audio.c calls at the
   single point a voice really starts, immediately after
   audio_backend_voice_start -- completes and emits it with the slot index.
   A culled play leaves the record pending, and the next real start emits it.

   Cost when nothing is pending (every play outside an armed hw_run): one
   static bool test in each half. s_first_play_pending is only ever set by a
   reload that found telemetry armed.

   `in_arena` is the claim that matters: the started sample's PCM lies inside
   the arena the last audio_reload filled, so the voice is playing bytes
   decoded from the root named on the same line -- not a leftover pointer.

   The backend's own per-voice state (audio_backend_voice_playing) is
   deliberately NOT read here: it is static to audio.c, and on PC it takes the
   mixer lock while on Wii it can reclaim an expired slot as a side effect --
   neither belongs on the play path. `voices_active` is the engine-side
   evidence that the voice is MARKED playing: audio.c sets s_voices[slot].active
   before it calls in, so on a real play the count includes this slot and is
   >= 1. (A test that calls this entry point directly, without audio.c having
   marked anything, correctly reports 0 -- the field says what the mixer would
   see, it is not a restatement of the slot argument.) */
static void audio_tel_first_play(int slot, const struct audio_sample* smp) {
	s_first_play_pending = false;
	const char* key = (s_pending_sound_id >= 0
					   && s_pending_sound_id < AUDIO_SOUND_COUNT
					   && s_names[s_pending_sound_id])
		? s_names[s_pending_sound_id]
		: "";
	bool in_arena = s_arena && smp->pcm
		&& (const uint8_t*)smp->pcm >= s_arena
		&& (const uint8_t*)smp->pcm < s_arena + s_arena_used;
	char line[640];
	size_t off = (size_t)snprintf(line, sizeof(line),
								  "{\"event\":\"audio_first_play\",\"key\":");
	off = audio_tel_json_str(line, sizeof(line), off, key);
	if(off >= sizeof(line))
		return;
	off += (size_t)snprintf(line + off, sizeof(line) - off, ",\"root\":");
	off = audio_tel_json_str(line, sizeof(line), off, s_effective_root);
	if(off >= sizeof(line))
		return;
	off += (size_t)snprintf(
		line + off, sizeof(line) - off,
		",\"variant\":%d,\"frames\":%u,\"rate\":%u,\"voice\":%d,"
		"\"in_arena\":%s,\"voices_active\":%d,"
		"\"evidence\":\"voice_started\"}",
		s_pending_variant, (unsigned)smp->frames, (unsigned)smp->rate, slot,
		in_arena ? "true" : "false", audio_voices_active());
	if(off >= sizeof(line))
		return;
	(void)audio_tel_emit(line);
}

void audio_registry_note_voice_started(int slot,
									   const struct audio_sample* smp) {
	if(!s_first_play_pending || !smp)
		return;
	audio_tel_first_play(slot, smp);
}

/* ===== deferred keys: the audio.c-facing half (C2 P2) =================== */

const struct audio_sample* audio_registry_variant(uint16_t sound_id,
												  int variant) {
	if(sound_id >= AUDIO_SOUND_COUNT || variant < 0
	   || !audio_key_visible(sound_id, false)
	   || variant >= s_variant_count[sound_id])
		return NULL;
	return &s_variants[sound_id][variant];
}

const struct audio_deferred_source*
audio_registry_deferred_source(uint16_t sound_id,
							   const struct audio_sample* smp, int* variant) {
	if(!s_deferred_active || sound_id != SOUND_AMBIENT_CAVE || !smp
	   || smp->pcm)
		return NULL;
	ptrdiff_t v = smp - s_variants[sound_id];
	if(v < 0 || v >= s_variant_count[sound_id])
		return NULL;
	if(variant)
		*variant = (int)v;
	return &s_deferred_src[v];
}

void audio_registry_note_deferred_started(int slot, uint16_t sound_id,
										  int variant,
										  const struct audio_sample* smp) {
	if(!s_first_play_pending || !smp)
		return;
	/* REQUEST-LOCAL attribution (Astra D5): other sounds were picked while
	   this one loaded, and each pick restashed the pending key. The record
	   must name the sample that is actually starting. */
	s_pending_sound_id = (int)sound_id;
	s_pending_variant = variant;
	audio_tel_first_play(slot, smp);
}

static const char* audio_deferred_outcome_name(enum audio_deferred_outcome o) {
	switch(o) {
		case AUDIO_DEFERRED_OUT_BUSY: return "busy";
		case AUDIO_DEFERRED_OUT_CULLED: return "culled";
		case AUDIO_DEFERRED_OUT_FAILED: return "failed";
		case AUDIO_DEFERRED_OUT_CANCELLED: return "cancelled";
		case AUDIO_DEFERRED_OUT_STOLEN: return "stolen";
		case AUDIO_DEFERRED_OUT_COMPLETED: return "completed";
		default: return "none";
	}
}

void audio_registry_deferred_telemetry(void) {
	/* Cost on a normal run: one static bool, then the armed test. Terminal
	   records of an unarmed run simply age out of the module's bounded ring. */
	if(!s_deferred_active && !s_deferred_scan_pending)
		return;
	if(!audio_tel_armed())
		return;

	char line[384];
	if(s_deferred_scan_pending) {
		int len = snprintf(
			line, sizeof(line),
			"{\"event\":\"audio_deferred_scan\",\"active\":%s,"
			"\"eligible\":%d,\"discovered\":%u,\"oversize\":%u,"
			"\"unavailable\":%u,\"scratch_bytes\":%u,\"decoder_bytes\":%u}",
			s_deferred_active ? "true" : "false",
			s_deferred_active ? s_variant_count[SOUND_AMBIENT_CAVE] : 0,
			s_deferred_discovered, s_deferred_oversize, s_deferred_unavailable,
			s_deferred_scratch_bytes, s_deferred_decoder_bytes);
		/* A truncated line is unparseable JSON: dropped, never emitted (the
		   audio_reload rule; hostile review 06 #8). */
		if(len > 0 && (size_t)len < sizeof(line) && !audio_tel_emit_owed(line))
			return; /* ring full: owed again next frame */
		s_deferred_scan_pending = false;
	}

	/* One terminal record per frame at most: never a burst into the ring. */
	struct audio_deferred_record r;
	if(!audio_deferred_pop_record(&r))
		return;
	struct audio_deferred_counters c;
	audio_deferred_counters(&c);
	int len = snprintf(line, sizeof(line),
			 "{\"event\":\"audio_deferred\",\"req\":%u,\"epoch\":%u,"
			 "\"key\":\"%s\",\"variant\":%d,\"bytes\":%u,\"frames\":%u,"
			 "\"rate\":%u,\"load_ms\":%d,\"start_delay_ms\":%d,"
			 "\"started\":%s,\"outcome\":\"%s\",\"max_step_ms\":%d,"
			 "\"requests\":%u}",
			 (unsigned)r.req_id, (unsigned)r.epoch,
			 r.sound_id < AUDIO_SOUND_COUNT ? s_names[r.sound_id] : "",
			 r.variant, (unsigned)r.bytes, (unsigned)r.frames,
			 (unsigned)r.rate, (int)r.load_ms, (int)r.start_delay_ms,
			 r.started ? "true" : "false",
			 audio_deferred_outcome_name(r.outcome), (int)c.max_step_ms,
			 (unsigned)c.requests);
	/* A full ring (or a truncated line) drops this one record; the counters
	   in the next record still reconcile it. */
	if(len > 0 && (size_t)len < sizeof(line))
		(void)audio_tel_emit(line);
}

const struct audio_sample* audio_registry_pick(uint16_t sound_id) {
	if(sound_id >= AUDIO_SOUND_COUNT)
		return NULL;
	/* asyncsnd-0926: a PENDING key (pass C still loading) is skipped -- no
	   RNG draw, no queue, no later replay; counted in audio_key_visible. */
	if(!audio_key_visible(sound_id, true) || s_variant_count[sound_id] == 0)
		return NULL;
	/* Uniform random pick among the key's variants — Beta rolled this on
	   the CLIENT at play time [SoundPool.getRandomSoundFromSoundPool]. */
	int v = rand_gen_int(&s_pick_rng, s_variant_count[sound_id]);
	if(v >= s_variant_count[sound_id]) /* rand contract belt-and-braces */
		v = s_variant_count[sound_id] - 1;
	if(s_first_play_pending) {
		/* Restashed on every pick while pending, so a culled play cannot
		   leave a stale key behind for the next real start to report. */
		s_pending_sound_id = (int)sound_id;
		s_pending_variant = v;
	}
	return &s_variants[sound_id][v];
}

struct random_gen* audio_cosmetic_rng(void) {
	return &s_pick_rng;
}

/* ===== Beta StepSound tables ============================================ */

/* The NINE StepSound instances [740c583: Block.java:7-15]:
   powder("stone",1,1) wood gravel grass stone metal("stone",1,1.5)
   glass(StepSoundStone: BREAK->random.glass) cloth
   sand(StepSoundSand: BREAK->step.gravel). Volume is 1.0F for all nine. */
enum step_family {
	SS_POWDER = 0, /* the Block ctor default [Block.java:141] */
	SS_WOOD,
	SS_GRAVEL,
	SS_GRASS,
	SS_STONE,
	SS_METAL,
	SS_GLASS,
	SS_CLOTH,
	SS_SAND,
};

/* Per-block-id assignment, transcribed from the static initializer
   [740c583: Block.java:593-688]. Unlisted ids keep SS_POWDER ("stone") —
   fluids 8-11, noteblock 25, bed 26, pistons 29/33/34/36, web 30 and
   redstone wire 55 (explicitly soundPowderFootstep) all sound as stone in
   Beta. Stairs COPY their model block's sound [BlockStairs ctor:14]:
   53 = planks (wood), 67 = cobblestone (stone). Id 95 is Beta's own
   lockedChest again since FH-07 (soundWoodFootstep [740c583 Block.java:687]);
   the fork iron chest now lives at net-new 200 and keeps the wood step sound
   it always had here. */
static const uint8_t s_block_step[256] = {
	[1] = SS_STONE,	  [2] = SS_GRASS,	[3] = SS_GRAVEL,  [4] = SS_STONE,
	[5] = SS_WOOD,	  [6] = SS_GRASS,	[7] = SS_STONE,	  [12] = SS_SAND,
	[13] = SS_GRAVEL, [14] = SS_STONE,	[15] = SS_STONE,  [16] = SS_STONE,
	[17] = SS_WOOD,	  [18] = SS_GRASS,	[19] = SS_GRASS,  [20] = SS_GLASS,
	[21] = SS_STONE,  [22] = SS_STONE,	[23] = SS_STONE,  [24] = SS_STONE,
	[27] = SS_METAL,  [28] = SS_METAL,	[31] = SS_GRASS,  [32] = SS_GRASS,
	[35] = SS_CLOTH,  [37] = SS_GRASS,	[38] = SS_GRASS,  [39] = SS_GRASS,
	[40] = SS_GRASS,  [41] = SS_METAL,	[42] = SS_METAL,  [43] = SS_STONE,
	[44] = SS_STONE,  [45] = SS_STONE,	[46] = SS_GRASS,  [47] = SS_WOOD,
	[48] = SS_STONE,  [49] = SS_STONE,	[50] = SS_WOOD,	  [51] = SS_WOOD,
	[52] = SS_METAL,  [53] = SS_WOOD,	[54] = SS_WOOD,	  [56] = SS_STONE,
	[57] = SS_METAL,  [58] = SS_WOOD,	[59] = SS_GRASS,  [60] = SS_GRAVEL,
	[61] = SS_STONE,  [62] = SS_STONE,	[63] = SS_WOOD,	  [64] = SS_WOOD,
	[65] = SS_WOOD,	  [66] = SS_METAL,	[67] = SS_STONE,  [68] = SS_WOOD,
	[69] = SS_WOOD,	  [70] = SS_STONE,	[71] = SS_METAL,  [72] = SS_WOOD,
	[73] = SS_STONE,  [74] = SS_STONE,	[75] = SS_WOOD,	  [76] = SS_WOOD,
	[77] = SS_STONE,  [78] = SS_CLOTH,	[79] = SS_GLASS,  [80] = SS_CLOTH,
	[81] = SS_CLOTH,  [82] = SS_GRAVEL, [83] = SS_GRASS,  [84] = SS_STONE,
	[85] = SS_WOOD,	  [86] = SS_WOOD,	[87] = SS_STONE,  [88] = SS_SAND,
	[89] = SS_GLASS,  [90] = SS_GLASS,	[91] = SS_WOOD,	  [92] = SS_CLOTH,
	[93] = SS_WOOD,	  [94] = SS_WOOD,	[95] = SS_WOOD,	  [96] = SS_WOOD,
	[200] = SS_WOOD, /* NET-NEW iron chest (FH-07 relocation off 95) */
};

/* stepSoundDir2(): the WALK/PLACE/HIT name — always "step."+family. */
static const uint16_t s_step_sound[9] = {
	[SS_POWDER] = SOUND_STEP_STONE, [SS_WOOD] = SOUND_STEP_WOOD,
	[SS_GRAVEL] = SOUND_STEP_GRAVEL, [SS_GRASS] = SOUND_STEP_GRASS,
	[SS_STONE] = SOUND_STEP_STONE,	[SS_METAL] = SOUND_STEP_STONE,
	[SS_GLASS] = SOUND_STEP_STONE,	[SS_CLOTH] = SOUND_STEP_CLOTH,
	[SS_SAND] = SOUND_STEP_SAND,
};

/* stepSoundDir(): the BREAK name — the two subclass overrides
   [StepSoundStone -> "random.glass"; StepSoundSand -> "step.gravel"]. */
static const uint16_t s_break_sound[9] = {
	[SS_POWDER] = SOUND_STEP_STONE, [SS_WOOD] = SOUND_STEP_WOOD,
	[SS_GRAVEL] = SOUND_STEP_GRAVEL, [SS_GRASS] = SOUND_STEP_GRASS,
	[SS_STONE] = SOUND_STEP_STONE,	[SS_METAL] = SOUND_STEP_STONE,
	[SS_GLASS] = SOUND_RANDOM_GLASS, [SS_CLOTH] = SOUND_STEP_CLOTH,
	[SS_SAND] = SOUND_STEP_GRAVEL,
};

uint16_t audio_block_break_sound_id(uint8_t block_id) {
	if(block_id == 0)
		return AUDIO_SOUND_NONE; /* aux-2001 id>0 guard [RenderGlobal] */
	return s_break_sound[s_block_step[block_id]];
}

uint16_t audio_block_step_sound_id(uint8_t block_id) {
	if(block_id == 0)
		return AUDIO_SOUND_NONE;
	return s_step_sound[s_block_step[block_id]];
}

uint8_t audio_footstep_block_id(uint8_t foot_id, uint8_t below_id) {
	/* Fence id 85 [740c583: Block.java:667]. The substitution is
	   unconditional in Beta — it overrides even a non-air foot cell (e.g. a
	   snow layer atop a fence walks as wood, not cloth).
	   [740c583: Entity.moveEntity:478-483] */
	return below_id == 85 ? below_id : foot_id;
}

float audio_block_sound_volume(uint8_t block_id) {
	(void)block_id;
	return 1.0F; /* every StepSound instance is volume 1.0F [Block.java:7-15] */
}

float audio_block_sound_pitch(uint8_t block_id) {
	/* metal = stone files at pitch 1.5F — the only pitched family. */
	return s_block_step[block_id] == SS_METAL ? 1.5F : 1.0F;
}

uint16_t audio_footstep_params(uint8_t foot_block_id, uint8_t above_block_id,
							   float* vol, float* pitch) {
	/* Raw Beta ids, matching the tables above: 78 = snow LAYER (walking ON a
	   snow BLOCK 80 takes the ordinary path — cloth by its own table entry),
	   8-11 = the four fluid ids (the only isLiquid materials in b1.7.3).
	   Beta's exact gate order: the non-air foot-block test first, THEN the
	   snow-above override, THEN the liquid-material silence.
	   [740c583: Entity.moveEntity:478-499] */
	uint8_t pick;
	if(foot_block_id == 0
	   || (above_block_id != 78 && foot_block_id >= 8
		   && foot_block_id <= 11)) {
		if(vol)
			*vol = 0.0F;
		if(pitch)
			*pitch = 1.0F;
		return AUDIO_SOUND_NONE;
	}
	pick = above_block_id == 78 ? 78 : foot_block_id;
	if(vol)
		*vol = audio_block_sound_volume(pick) * 0.15F;
	if(pitch)
		*pitch = audio_block_sound_pitch(pick);
	return audio_block_step_sound_id(pick);
}

/* ===== Beta per-mob sound table ========================================= */

#include "../entity/monsters_object.h" /* MONSTER_* ids */

void audio_mob_sounds(int monster_id, uint16_t* idle, uint16_t* hurt,
					  uint16_t* death, float* volume) {
	uint16_t i = AUDIO_SOUND_NONE, h = AUDIO_SOUND_NONE,
			 d = AUDIO_SOUND_NONE;
	float v = 1.0F; /* EntityLiving.getSoundVolume default */

	switch(monster_id) {
	case MONSTER_ZOMBIE: /* [EntityZombie get*Sound] */
		i = SOUND_MOB_ZOMBIE;
		h = SOUND_MOB_ZOMBIEHURT;
		d = SOUND_MOB_ZOMBIEDEATH;
		break;
	case MONSTER_PIG_ZOMBIE: /* [EntityPigZombie get*Sound] */
		i = SOUND_MOB_ZOMBIEPIG;
		h = SOUND_MOB_ZOMBIEPIGHURT;
		d = SOUND_MOB_ZOMBIEPIGDEATH;
		break;
	case MONSTER_GHAST: /* [EntityGhast get*Sound + getSoundVolume 10] */
		i = SOUND_MOB_GHAST_MOAN;
		h = SOUND_MOB_GHAST_SCREAM;
		d = SOUND_MOB_GHAST_DEATH;
		v = 10.0F;
		break;
	case MONSTER_WOLF:
		/* Living sound is state/RNG-dependent and selected in entity_monster.c;
		 * bark is only the harmless table default. All wolf sounds use 0.4F.
		 * [740c583: EntityWolf.getLivingSound/getSoundVolume] */
		i = SOUND_MOB_WOLF_BARK;
		h = SOUND_MOB_WOLF_HURT;
		d = SOUND_MOB_WOLF_DEATH;
		v = 0.4F;
		break;
	case MONSTER_SKELETON: /* death == hurt [EntitySkeleton] */
		i = SOUND_MOB_SKELETON;
		h = SOUND_MOB_SKELETONHURT;
		d = SOUND_MOB_SKELETONHURT;
		break;
	case MONSTER_CREEPER: /* NO idle sound [EntityCreeper: no getLivingSound] */
		h = SOUND_MOB_CREEPER;
		d = SOUND_MOB_CREEPERDEATH;
		break;
	case MONSTER_SPIDER: /* idle == hurt [EntitySpider] */
		i = SOUND_MOB_SPIDER;
		h = SOUND_MOB_SPIDER;
		d = SOUND_MOB_SPIDERDEATH;
		break;
	case MONSTER_SHEEP: /* one sound for all three [EntitySheep] */
		i = SOUND_MOB_SHEEP;
		h = SOUND_MOB_SHEEP;
		d = SOUND_MOB_SHEEP;
		break;
	case MONSTER_COW: /* hurt == death; the ONLY volume override: 0.4F
						 [EntityCow get*Sound + getSoundVolume] */
		i = SOUND_MOB_COW;
		h = SOUND_MOB_COWHURT;
		d = SOUND_MOB_COWHURT;
		v = 0.4F;
		break;
	case MONSTER_PIG: /* idle == hurt [EntityPig] */
		i = SOUND_MOB_PIG;
		h = SOUND_MOB_PIG;
		d = SOUND_MOB_PIGDEATH;
		break;
	case MONSTER_CHICKEN: /* hurt == death [EntityChicken] */
		i = SOUND_MOB_CHICKEN;
		h = SOUND_MOB_CHICKENHURT;
		d = SOUND_MOB_CHICKENHURT;
		break;
	case MONSTER_SQUID:
		/* b1.7.3 squid is SILENT: get*Sound all return null
		   [EntitySquid.java:34-45]. Leave every id NONE. */
		break;
	case MONSTER_SLIME_SMALL:
	case MONSTER_SLIME_MEDIUM:
	case MONSTER_SLIME_BIG:
		/* S2: hurt == death == "mob.slime" at getSoundVolume() = 0.6F; NO
		   idle (no getLivingSound override — the size-gated jump/land hops
		   are the "voice", wired in entity_monster.c). "mob.small_slime*"
		   does not exist in b1.7.3. [740c583: EntitySlime] */
		h = SOUND_MOB_SLIME;
		d = SOUND_MOB_SLIME;
		v = 0.6F;
		break;
	case MONSTER_GIANT:
		/* Beta EntityGiantZombie extends EntityMob directly: no get*Sound
		   overrides anywhere on its chain, so it inherits EntityLiving's
		   defaults -- getLivingSound null (silent idle) and getHurtSound/
		   getDeathSound "random.hurt" [740c583 EntityLiving:380-390].
		   random.hurt IS a resident S0 key (the local player's own hurt/death
		   voice already uses it, server_local.c), so this was a stale gap,
		   not a missing asset: wire it here too. getSoundVolume is likewise
		   uninherited on this chain -> EntityLiving's default 1.0F. */
		h = SOUND_RANDOM_HURT;
		d = SOUND_RANDOM_HURT;
		break;
	default:
		/* Any other future id defaults to silent rather than a wrong
		   sound. */
		break;
	}

	if(idle)
		*idle = i;
	if(hurt)
		*hurt = h;
	if(death)
		*death = d;
	if(volume)
		*volume = v;
}
