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
	S3 background music (docs/finish_line_plan.md S3). audio_music.h is the
	engine-facing surface; audio_music_timer.h/audio_music_ring.h/
	audio_music_table.h hold the pure, golden-tested decision logic this
	file wires up to real files, a real thread, and the audio.h
	audio_voice_music_* hardware voice (source/platform/audio.c + its
	wii/pc backend splice).

	STREAMING, not preloading: a real Beta background track is a 2-4MB
	compressed .ogg / several minutes of PCM (~15MB decoded mono @ 44.1kHz)
	-- the S0/S1/S2 decode-AT-LOAD-into-one-fixed-arena model
	(audio_registry.c) is the wrong tool here on purpose (finish_line_plan.md
	S3's own note: "streaming ogg decode is real CPU; revisit with measured
	headroom"). This file instead keeps the encoded file on disk (SD card on
	Wii, same libfat fopen/fread path audio_file_read already uses) and
	pull-decodes it a small chunk at a time via stb_vorbis's stdio API
	(stb_vorbis_open_filename + stb_vorbis_get_samples_short_interleaved),
	off the render path, into a 2 x MUSIC_CHUNK_BYTES PCM16-mono double
	buffer (audio_music_ring.h). The decode work runs on ONE portable
	worker thread (thread.h; the chunk_mesher_local_thread/net_bringup_thread
	precedent -- proven off-render-thread work on real Wii hardware
	already), used identically on both platforms rather than diverging to a
	Wii-only per-frame budget: this is real streaming CPU work exactly like
	the mesher's, and the mesher's answer to "keep it off the render
	thread on a single-core Wii" is already a worker thread, not a budget.

	stb_vorbis config: audio_registry.c is the ONE implementation TU
	(third_party/stb/PATCHES.md); this TU links against it header-only
	(STB_VORBIS_HEADER_ONLY, the tools/sound_transcode.c
	SOUND_TRANSCODE_EXTERN_VORBIS precedent) with the EXACT SAME macro set
	audio_registry.c compiles the implementation with -- audio_registry.c
	dropped STB_VORBIS_NO_STDIO specifically so this file's
	stb_vorbis_open_filename/get_samples_short_interleaved/close calls have
	real symbols to link against.
*/

#include <stdio.h>
#include <string.h>
#include <sys/stat.h> /* the selected audio pack may have been deleted */

#include "../game/game_state.h"
#include "../util.h"
#include "audio.h"
#include "audio_music.h"
#include "audio_music_record.h"
#include "audio_music_ring.h"
#include "audio_music_table.h"
#include "audio_music_timer.h"
#include "audio_resample.h"
#include "audio_stream_ring.h"
#include "hw_telemetry.h" /* observer sink for the music_reload evidence line */
#include "mem2.h" /* the read-ahead ring lives in the MEM2 carve */
#include "thread.h"
#include "time.h" /* the music rescan's own wall-clock */

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#include <stb/stb_vorbis.c>
#pragma GCC diagnostic pop

/* One buffer half. 8KB mono16 (docs/finish_line_plan.md S3 sizing) is
   ~93-186ms of audio at real Beta music rates (22-44kHz) -- small on
   purpose (this is memory the fixed 12MB sound arena's budget already runs
   tight against, per audio_registry.c's own history), refilled well ahead
   of need by the worker thread. */
#define MUSIC_CHUNK_BYTES (8u * 1024u)
#define MUSIC_CHUNK_SAMPLES (MUSIC_CHUNK_BYTES / 2u)

/* ONE constant, two owners: the Wii backend's looped ring is sliced into
   slots of exactly one chunk (audio_stream_ring.h), so a chunk that did not
   fit a slot would either be truncated or would break the slot arithmetic
   the read-head estimate depends on. Keep them in lockstep here rather than
   discovering the mismatch as a clipped track on hardware. */
_Static_assert(MUSIC_CHUNK_SAMPLES == AUDIO_STREAM_RING_SLOT_SAMPLES,
			   "streaming chunk size must equal one audio_stream_ring slot");

#define AUDIO_MUSIC_DEFAULT_PACK "assets" /* == audio_registry.c's AUDIO_DEFAULT_PACK; no bundled assets/music/ exists (2026-08-31) -- kept for forward-compat, presently always a no-op */

/* Records are resolved later than the boot-time music scan. Keep a session
   copy of the effective audio root so a texture selection cannot redirect a
   record path through a borrowed pointer or a later config mutation. */
static char s_audio_pack[256] = AUDIO_MUSIC_DEFAULT_PACK;

static bool audio_music_root_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Same resolution + existence proof as audio_registry.c's
   audio_effective_pack, for the same reason: paths.audiopack can name a pack
   that has since been deleted, and nothing in the UI can move it once the
   directory is gone (2026-09-17 DELTA review, HIGH). Music feels this hardest
   because audio_music_registry_scan has no bundled fallback to land on.
   Nothing is written at boot, and a config WITHOUT paths.audiopack is not
   probed at all -- that path resolves exactly as it did before. */
static const char* audio_music_select_pack(void) {
	if(!config_read_string(&gstate.config_user, "paths.audiopack", NULL)) {
		snprintf(s_audio_pack, sizeof(s_audio_pack), "%s",
				 config_read_path(&gstate.config_user, "paths.texturepack",
								  AUDIO_MUSIC_DEFAULT_PACK));
		return s_audio_pack;
	}

	const char* legacy = config_read_string(&gstate.config_user,
										"paths.texturepack",
										AUDIO_MUSIC_DEFAULT_PACK);
	snprintf(s_audio_pack, sizeof(s_audio_pack), "%s",
			 config_read_path(&gstate.config_user, "paths.audiopack", legacy));
	if(audio_music_root_exists(s_audio_pack))
		return s_audio_pack;
	fprintf(stderr,
			"[music] selected audio pack \"%s\" is gone; falling back to the "
			"texture pack\n",
			s_audio_pack);

	snprintf(s_audio_pack, sizeof(s_audio_pack), "%s",
			 config_read_path(&gstate.config_user, "paths.texturepack",
							  AUDIO_MUSIC_DEFAULT_PACK));
	if(audio_music_root_exists(s_audio_pack))
		return s_audio_pack;
	fprintf(stderr, "[music] texture pack \"%s\" is gone too; using the bundled "
					"default\n",
			s_audio_pack);
	snprintf(s_audio_pack, sizeof(s_audio_pack), "%s", AUDIO_MUSIC_DEFAULT_PACK);
	return s_audio_pack;
}

#ifdef CAVEWII_TESTING
/* Test-only read of the session's effective audio root
   (tests/test_pack_helper_audio.c). Gated on CAVEWII_TESTING -- THE explicit
   test-instrumentation switch (CMakeLists.txt CAVEX_TEST_ENGINE_DEFINES; the
   playable-target boundary check rejects it at configure time and the
   post-link marker gates keep it out of shipping binaries) -- so no test
   symbol is exported from the game, the displaylist_test_fail_* /
   CAVEWII_DISPLAYLIST_TEST precedent (source/platform/displaylist.h:180-184).
   Declared here rather than in audio_music.h because no production caller may
   ever see it; the test declares the matching extern itself. */
const char* audio_music_test_effective_pack(void);
const char* audio_music_test_effective_pack(void) {
	return s_audio_pack;
}
#endif

/* Flat catalog: every discovered file across every key, because
   SoundPool.getRandomSound() (the method Beta's timer actually calls) picks
   UNIFORMLY over the whole pool, not per-key -- audio_music_table.h. Paths
   only; nothing here is decoded until a track is actually picked.

   `key` is a plain int, not `enum audio_music_key` -- this ONE struct shape
   is shared by s_catalog (the Beta survival pool, keyed by
   enum audio_music_key) AND s_creative_catalog below (the creative-mode
   add-on pool, keyed by the SEPARATE enum audio_music_creative_key) so the
   scan helper can be written once; each catalog's own accessor casts back
   to its own enum, so the two key spaces are never confused at the call
   sites that matter (audio_music_current_key/_telemetry_key). */
struct audio_music_catalog_entry {
	char path[192];
	int key;
	int variant; /* filename digit suffix (0 = bare), debug.music_pick only */
};
static struct audio_music_catalog_entry
	s_catalog[AUDIO_MUSIC_KEY_COUNT * AUDIO_MUSIC_MAX_VARIANTS];
static int s_catalog_count = 0;

/* CREATIVE playlist catalog (audio_music_table.h's AUDIO_MUSIC_CREATIVE_TABLE
   banner) -- structurally identical to s_catalog above but a SEPARATE array
   and a SEPARATE counter, never merged into it: a survival world's uniform
   pick over s_catalog must be bit-for-bit unaffected by whether this catalog
   is empty, partially, or fully populated. See audio_music_tick()'s
   "use_creative" gate for the per-world selection. */
static struct audio_music_catalog_entry
	s_creative_catalog[AUDIO_MUSIC_CREATIVE_KEY_COUNT * AUDIO_MUSIC_MAX_VARIANTS];
static int s_creative_catalog_count = 0;

/* The playRandomMusicIfReady timer state (audio_music_timer.h). Seeded once
   at audio_music_init -- SoundManager's ticksBeforeMusic lives on the
   SoundManager instance, which persists for the whole client session, NOT
   per-World (unlike client_display_tick.c's cave-mood counter, which IS a
   World field and does reseed on world load/unload). The countdown only
   ever advances from audio_music_tick, which the caller (client_display_tick)
   already gates to world-loaded-and-unpaused ticks -- exactly mirroring
   Beta's own `!isGamePaused && theWorld != null` gate on the call site. */
static int s_ticks_before_music = 0;
static bool s_timer_seeded = false;

/* The decode worker + its double buffer, all guarded by `lock` except
   `vorbis`/`native_channels` which only the worker thread itself ever
   touches (never read cross-thread). `gen` invalidates an in-flight decode
   that a stop/reopen request raced against: the worker snapshots it before
   unlocking to decode, and only commits the result if it is unchanged --
   the epoch-guard precedent that keeps a stale chunk from clobbering
   freshly-reset ring state. */
struct music_ring {
	struct thread_mutex lock;
	struct thread_cond wake;
	struct thread worker;
	volatile bool want_stop;
	uint32_t gen;

	bool want_open;			/* main thread -> worker: (re)open pending_path,
								   or close if pending_path is empty */
	char pending_path[192];
	/* Plain int, not enum audio_music_key -- holds either an
	   enum audio_music_key (survival pick) or an
	   enum audio_music_creative_key (creative pick, is_creative below);
	   which one it is is decided ENTIRELY by is_creative at read time
	   (audio_music_current_key/_telemetry_key), the same convention
	   audio_music_play_record's AUDIO_MUSIC_KEY_COUNT sentinel already
	   relies on for "not a real background key". */
	int pending_key;
	bool pending_continuous; /* diagnostic background request, under lock */

	/* The decode ring (audio_music_ring.h). Widened 2 -> 4 on 2026-09-01
	   (two halves could bank only ONE chunk ahead, so a 100-300ms
	   world-load frame handed over less audio than it consumed), then
	   4 -> 32 with a refill low-water mark on 2026-09-26 (T1 read-ahead:
	   the header carries the telemetry that sized it). `buf` points at
	   either the 256KB MEM2 block audio_music_init takes once, or -- if
	   that allocation fails -- the 32KB four-half static fallback below;
	   `halves`/`refill_low` are the ACTIVE policy for whichever it is. The
	   per-half bookkeeping arrays are always sized for the maximum. Only
	   ever changed with no track open (init, or the test-only policy
	   setter), so the worker and main thread never disagree on them. */
	int16_t (*buf)[MUSIC_CHUNK_SAMPLES];
	int halves;
	int refill_low;
	uint32_t buf_frames[MUSIC_RING_HALVES];
	bool ready[MUSIC_RING_HALVES];
	bool eof[MUSIC_RING_HALVES];
	int playing_half; /* -1 = nothing started for the current track yet */

	bool track_active;	 /* the timer's "track_playing" gate */
	bool file_open;		 /* worker has a vorbis handle open right now */
	uint32_t rate;		 /* native sample rate of the open file */
	int current_key;	 /* see pending_key's comment above */
	unsigned underrun_count;
	/* jukebox-slice: true while the CURRENT track_active source is a
	   record (audio_music_play_record), not a random background pick.
	   Guaranteed false whenever track_active is false -- the only paths
	   that clear track_active (audio_music_stop, the EOF branch of
	   audio_music_update, which calls it) also clear this. */
	bool is_record;
	/* creative-mode add-on (docs/creative_mode.md): true while the CURRENT
	   track_active source was picked from s_creative_catalog rather than
	   s_catalog -- set SYNCHRONOUSLY by audio_music_tick() at the moment a
	   track is picked (the same synchronous-set discipline is_record above
	   documents for records, not deferred to the decode worker), so
	   current_key/is_creative are always in agreement and never read a
	   stale combination. Guaranteed false whenever track_active is false
	   (audio_music_stop clears it, same as is_record). Mutually exclusive
	   with is_record: a record always sets is_creative false (see
	   audio_music_play_record). */
	bool is_creative;
	/* Telemetry-only: decoded frames actually handed to the voice for the
	   CURRENTLY open track/record, reset to 0 by the SAME want_open branch
	   that resets ready/eof/playing_half (a fresh open OR a plain close).
	   audio_music_telemetry_ms() divides this by `rate`. */
	uint32_t total_frames_played;

	/* G9 decode deadline promotion (audio_music_ring.h banner). promote_gate
	   is refreshed by MAIN each update (configure && local tick-boost owner);
	   promoted is the worker's CURRENT scheduling state. MAIN applies both
	   directions under this lock; the worker only ever DEMOTES itself, after
	   dropping the lock, so it is never preempted at 48 while holding it. */
	bool promote_gate;
	bool promoted;
	struct audio_music_promotion_stats promo;

	/* worker-thread-only */
	void* vorbis; /* stb_vorbis* */
	int native_channels;
	bool continuous; /* worker-only snapshot for the open decoder */
	/* Device-rate conversion (audio_resample.h). `rate` above is the OUTPUT
	   rate handed to the voice; this is the file's own. On PC the two are
	   always equal and the resampler stays inactive. */
	uint32_t src_rate;
	struct audio_resample_state resample;
	int16_t in_buf[MUSIC_CHUNK_SAMPLES]; /* source frames not yet consumed */
	uint32_t in_len;
	uint32_t in_pos;
};
static struct music_ring s_ring;
static bool s_worker_started = false;
static struct {
	bool running, transferring, submitted, wake;
	uint32_t generation;
	int previous, half, passes;
} s_music_service;
#ifdef CAVEWII_TESTING
/* Real-worker contention injection, not a replacement ring/decoder. */
struct thread_mutex* audio_music_test_service_mutex(void);
struct thread_mutex* audio_music_test_service_mutex(void) { return &s_ring.lock; }
#endif

/* The pre-2026-09-26 ring storage, kept as the fallback: if the one-time
   MEM2 allocation of the full ring ever fails, music runs exactly as it did
   before the read-ahead change instead of going silent. Same 32KB of .bss
   the ring always occupied. */
static int16_t s_ring_fallback[MUSIC_RING_HALVES_LEGACY][MUSIC_CHUNK_SAMPLES];
/* The full MUSIC_RING_HALVES ring: taken once at the first audio_music_init
   and kept for the session (never freed -- a session-lifetime block, the
   audio arena precedent; taking it at boot, before any display-list churn,
   keeps it at the bottom of the carve instead of fragmenting it). */
static int16_t (*s_ring_full)[MUSIC_CHUNK_SAMPLES] = NULL;

/* ===== test instrumentation (CAVEWII_TESTING only) =======================
   tests/test_music_readahead_equiv.c drives the REAL ring through the real
   worker thread and needs to (a) see every chunk handed to the voice, in
   order, (b) script how many chunks the "device" accepts per frame so the
   refill low-water path is actually exercised headless (a headless
   backend otherwise accepts everything, every frame), (c) replay the
   legacy 4-half/refill-at-1 policy against the production one, and (d)
   force a device rate so the resampling decode path runs too. All of it
   compiles out of every shipping binary (the audio_music_test_* precedent
   above). */
#ifdef CAVEWII_TESTING
static void (*s_test_pcm_sink)(const int16_t* pcm, uint32_t frames,
							   uint32_t rate);
static bool (*s_test_voice_playing)(void);
static uint32_t s_test_device_rate;
/* Worker-side counters (read/written under s_ring.lock): how often the
   worker came back from cond_wait, and how many chunks it committed. Their
   ratio is the "fewer, larger wakes" claim, measured. */
static unsigned s_test_worker_wakes;
static unsigned s_test_chunks_committed;
void audio_music_test_worker_counters(unsigned* wakes, unsigned* chunks);
void audio_music_test_worker_counters(unsigned* wakes, unsigned* chunks) {
	thread_mutex_lock(&s_ring.lock);
	*wakes = s_test_worker_wakes;
	*chunks = s_test_chunks_committed;
	thread_mutex_unlock(&s_ring.lock);
}
void audio_music_test_set_pcm_sink(void (*sink)(const int16_t* pcm,
												uint32_t frames,
												uint32_t rate));
void audio_music_test_set_pcm_sink(void (*sink)(const int16_t* pcm,
												uint32_t frames,
												uint32_t rate)) {
	s_test_pcm_sink = sink;
}
void audio_music_test_set_voice_playing(bool (*playing)(void));
void audio_music_test_set_voice_playing(bool (*playing)(void)) {
	s_test_voice_playing = playing;
}
void audio_music_test_set_device_rate(uint32_t rate);
void audio_music_test_set_device_rate(uint32_t rate) {
	s_test_device_rate = rate;
}
/* debug.music_pick test (e): a headless backend is never "ready", so the
   timer can never fire; 1/0 overrides audio_music_backend_ready() for the
   timer call only, -1 restores it. */
static int s_test_backend_ready = -1;
void audio_music_test_set_backend_ready(int ready);
void audio_music_test_set_backend_ready(int ready) {
	s_test_backend_ready = ready;
}
void audio_music_test_set_timer(int ticks_before_music);
#endif

static void music_voice_start(const int16_t* pcm, uint32_t frames,
							  uint32_t rate) {
#ifdef CAVEWII_TESTING
	if(s_test_pcm_sink)
		s_test_pcm_sink(pcm, frames, rate);
#endif
	audio_voice_music_start(pcm, frames, (float)rate);
}

static bool music_voice_playing(void) {
#ifdef CAVEWII_TESTING
	if(s_test_voice_playing)
		return s_test_voice_playing();
#endif
	return audio_voice_music_playing();
}

static uint32_t music_device_rate(void) {
#ifdef CAVEWII_TESTING
	if(s_test_device_rate)
		return s_test_device_rate;
#endif
	return audio_voice_music_native_rate();
}

/* Caller holds s_ring.lock. The one predicate both sides use to decide
   whether an idle worker should start a refill burst (audio_music_ring.h). */
static bool music_ring_refill_due_locked(void) {
	return music_ring_refill_due_n(s_ring.ready, s_ring.halves,
								   s_ring.playing_half, s_ring.refill_low);
}

/* ===== G9 decode deadline promotion ======================================
   audio_music_ring.h carries the rationale. The priorities must sit above
   the boosted local server and its raised telemetry sender and below the
   Wiimote/WPAD stack (80) and the watchdog (100, wii/watchdog.c). PROMOTE_BANK
   must cover one whole MAIN hand-off pass (the S-2 hardware-ring guard). */
_Static_assert(MUSIC_DECODE_PRIORITY_PROMOTED > G9_TICK_BOOST_SENDER_PRIORITY
				   && MUSIC_DECODE_PRIORITY_PROMOTED < 80,
			   "promoted decode must outrank the boosted server/sender and "
			   "stay below WPAD (80) and the watchdog (100)");
_Static_assert(MUSIC_DECODE_PRIORITY_BASE == 48,
			   "the unpromoted worker keeps its pre-G9 priority");
_Static_assert(MUSIC_DECODE_PROMOTE_BANK >= AUDIO_STREAM_RING_SLOTS - 2 + 1
				   && MUSIC_DECODE_PROMOTE_BANK <= MUSIC_RING_HALVES - 1,
			   "the promote bank must exceed one MAIN hand-off pass");

static bool s_promote_enabled = false; /* MAIN-only (configure) */
#ifdef CAVEWII_TESTING
static int s_test_promote_gate = -1; /* -1 real gate, 0/1 forced */
static void (*s_test_priority_hook)(uint8_t priority);
void audio_music_test_set_promotion_gate(int forced);
void audio_music_test_set_promotion_gate(int forced) {
	s_test_promote_gate = forced;
}
void audio_music_test_set_priority_hook(void (*hook)(uint8_t priority));
void audio_music_test_set_priority_hook(void (*hook)(uint8_t priority)) {
	s_test_priority_hook = hook;
}
#endif

static void music_worker_set_priority(uint8_t priority) {
#ifdef CAVEWII_TESTING
	if(s_test_priority_hook)
		s_test_priority_hook(priority);
#endif
	thread_set_priority(&s_ring.worker, priority);
}

/* MAIN-only, NOT under s_ring.lock (it takes g9_lock): local owner present. */
static bool music_promote_gate_now(void) {
#ifdef CAVEWII_TESTING
	if(s_test_promote_gate >= 0)
		return s_test_promote_gate != 0;
#endif
	return s_promote_enabled && thread_g9_tick_boost_local_present();
}

/* Caller holds s_ring.lock. Should the worker be promoted right now? */
static bool music_promote_want_locked(void) {
	return s_ring.promote_gate && s_ring.track_active && s_ring.file_open
		&& !s_ring.want_open
		&& music_decode_promote_due_n(s_ring.ready, s_ring.halves,
									  s_ring.playing_half,
									  MUSIC_DECODE_PROMOTE_BANK);
}

/* MAIN-only, caller holds s_ring.lock: apply the wanted state in either
   direction. Applying it under the lock is safe from MAIN: raising a READY
   worker above MAIN at most makes it run its (unlocked) decode now, and if it
   then needs this lock it blocks until MAIN's unlock a few lines later. */
static void music_promote_sync_main_locked(void) {
	bool want = music_promote_want_locked();
	if(want == s_ring.promoted)
		return;
	s_ring.promoted = want;
	if(want)
		s_ring.promo.promotions++;
	else
		s_ring.promo.demotions++;
	music_worker_set_priority(want ? MUSIC_DECODE_PRIORITY_PROMOTED
								   : MUSIC_DECODE_PRIORITY_BASE);
}

/* WORKER-only, caller holds s_ring.lock: if the worker is promoted but no
   longer should be, clear the flag and return true -- the caller then drops
   the lock BEFORE lowering its own priority (a 48-priority lock holder could
   be preempted by MAIN/server and invert MAIN's next lock). A worker that
   goes to sleep while promotion is still wanted (possible only for small
   test ring policies) stays promoted: a blocked thread costs no CPU, and its
   next wake is exactly the refill the promotion exists for. */
static bool music_promote_demote_worker_locked(void) {
	if(!s_ring.promoted || music_promote_want_locked())
		return false;
	s_ring.promoted = false;
	s_ring.promo.demotions++;
	return true;
}

/* WORKER-only, caller holds s_ring.lock and music_promote_demote_worker_locked
   just returned true. Lowers the worker's own priority with the lock DROPPED,
   then re-takes it; if MAIN re-promoted in that window (its priority call may
   have landed before ours), re-apply the promotion so the real priority always
   matches s_ring.promoted once the worker holds the lock again. On the Wii the
   window cannot interleave (the worker runs at 70 until its own call), but the
   PC/host scheduler can, and the invariant should not depend on priority. */
static void music_worker_demote_relock(void) {
	thread_mutex_unlock(&s_ring.lock);
	music_worker_set_priority(MUSIC_DECODE_PRIORITY_BASE);
	thread_mutex_lock(&s_ring.lock);
	if(s_ring.promoted)
		music_worker_set_priority(MUSIC_DECODE_PRIORITY_PROMOTED);
}

void audio_music_decode_promotion_configure(bool enabled) {
	s_promote_enabled = enabled;
}

void audio_music_decode_promotion_snapshot(
	struct audio_music_promotion_stats* out) {
	memset(out, 0, sizeof(*out));
	if(!s_worker_started)
		return;
	thread_mutex_lock(&s_ring.lock);
	*out = s_ring.promo;
	out->promoted = s_ring.promoted;
	thread_mutex_unlock(&s_ring.lock);
}

/* ===== registry scan (paths only, no decode) ============================ */

static bool music_file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

/* Key -> name, generated from the same X-macro table (== s_names in
   audio_registry.c, one array indexed by the enum it is derived from). */
static const char* const s_music_key_names[AUDIO_MUSIC_KEY_COUNT] = {
#define AUDIO_MUSIC_TABLE_X(id, name) [id] = name,
	AUDIO_MUSIC_TABLE(AUDIO_MUSIC_TABLE_X)
#undef AUDIO_MUSIC_TABLE_X
};

/* Same generation, for the creative-mode add-on group
   (audio_music_table.h's AUDIO_MUSIC_CREATIVE_TABLE). */
static const char* const
	s_music_creative_key_names[AUDIO_MUSIC_CREATIVE_KEY_COUNT] = {
#define AUDIO_MUSIC_CREATIVE_TABLE_X(id, name) [id] = name,
		AUDIO_MUSIC_CREATIVE_TABLE(AUDIO_MUSIC_CREATIVE_TABLE_X)
#undef AUDIO_MUSIC_CREATIVE_TABLE_X
};

/* Shared scan core: appends every existing "<root>/music/<key_names[key]>
   [digit].ogg" variant to `catalog` (capacity `catalog_cap`, current fill
   `*catalog_count`). Used by BOTH music_scan_key (s_catalog) and
   music_scan_creative_key (s_creative_catalog) below -- the two thin
   wrappers exist only so each call site keeps its own enum type at the
   call, never passing a survival key into the creative catalog or vice
   versa. */
static void music_scan_into(const char* key_name, const char* root, int key,
							struct audio_music_catalog_entry* catalog,
							int catalog_cap, int* catalog_count, int* found) {
	char rel[128];
	audio_music_table_subpath(key_name, rel, sizeof(rel));
	for(int v = 0; v < AUDIO_MUSIC_MAX_VARIANTS && *catalog_count < catalog_cap;
		v++) {
		char rel_base[144];
		if(v == 0)
			snprintf(rel_base, sizeof(rel_base), "%s", rel);
		else
			snprintf(rel_base, sizeof(rel_base), "%s%d", rel, v);
		char path[192];
		snprintf(path, sizeof(path), "%s/%s.ogg", root, rel_base);
		if(music_file_exists(path)) {
			struct audio_music_catalog_entry* e = &catalog[(*catalog_count)++];
			snprintf(e->path, sizeof(e->path), "%s", path);
			e->key = key;
			e->variant = v;
			(*found)++;
		}
	}
}

static void music_scan_key(enum audio_music_key key, const char* root,
						   int* found) {
	music_scan_into(s_music_key_names[key], root, (int)key, s_catalog,
					(int)(sizeof(s_catalog) / sizeof(s_catalog[0])),
					&s_catalog_count, found);
}

static void music_scan_creative_key(enum audio_music_creative_key key,
									const char* root, int* found) {
	music_scan_into(s_music_creative_key_names[key], root, (int)key,
					s_creative_catalog,
					(int)(sizeof(s_creative_catalog)
						  / sizeof(s_creative_catalog[0])),
					&s_creative_catalog_count, found);
}

static void audio_music_registry_scan(const char* pack) {
	s_catalog_count = 0;
	for(int key = 0; key < AUDIO_MUSIC_KEY_COUNT; key++) {
		int found = 0;
		music_scan_key((enum audio_music_key)key, pack, &found);
		if(found == 0 && strcmp(pack, AUDIO_MUSIC_DEFAULT_PACK) != 0)
			music_scan_key((enum audio_music_key)key, AUDIO_MUSIC_DEFAULT_PACK,
						   &found);
		/* Unlike audio_registry_scan, a key resolving nothing is NOT logged
		   as a gap: music has no bundled default (the legal model -- NEVER
		   bundle/commit audio, docs/deviations.md) and a client with no
		   imported pack is correctly, silently musicless. */
	}

	/* creative-mode add-on: same scan, same fallback-to-default shape,
	   into the SEPARATE catalog -- never merged with s_catalog above. */
	s_creative_catalog_count = 0;
	for(int key = 0; key < AUDIO_MUSIC_CREATIVE_KEY_COUNT; key++) {
		int found = 0;
		music_scan_creative_key((enum audio_music_creative_key)key, pack,
								&found);
		if(found == 0 && strcmp(pack, AUDIO_MUSIC_DEFAULT_PACK) != 0)
			music_scan_creative_key((enum audio_music_creative_key)key,
									AUDIO_MUSIC_DEFAULT_PACK, &found);
	}
}

int audio_music_track_count(void) {
	return s_catalog_count;
}

/* creative-mode add-on: how many of the six creative-playlist files are
   currently registered (0..AUDIO_MUSIC_CREATIVE_KEY_COUNT, since none of
   them ship with variants). audio_music_tick()'s per-world selection reads
   this indirectly via s_creative_catalog_count; exposed here for the F3/
   options surface and tests, the audio_music_track_count() precedent. */
int audio_music_creative_track_count(void) {
	return s_creative_catalog_count;
}

/* ===== decode worker ===================================================== */

static void music_close_vorbis(void) {
	if(s_ring.vorbis) {
		stb_vorbis_close((stb_vorbis*)s_ring.vorbis);
		s_ring.vorbis = NULL;
	}
}

/* Worker-thread only; opens (or, on failure/empty path, just closes). */
static bool music_open_file(const char* path) {
	music_close_vorbis();
	if(!path || path[0] == '\0')
		return false;
	int error = 0;
	stb_vorbis* v = stb_vorbis_open_filename(path, &error, NULL);
	if(!v) {
		fprintf(stderr,
				"[audio_music] failed to open \"%s\" (stb_vorbis error %d); "
				"track skipped\n",
				path, error);
		return false;
	}
	stb_vorbis_info info = stb_vorbis_get_info(v);
	s_ring.vorbis = v;
	s_ring.native_channels = info.channels;
	s_ring.src_rate = (uint32_t)info.sample_rate;

	/* PRE-RESAMPLE TO THE DEVICE RATE (2026-09-01, real-Wii LISTEN: "C418
	   songs play, still glitchy, almost TINNY like a cheap speaker; other
	   sounds fine"). AESND's DSP mixer converts every voice to its fixed
	   48kHz output by NEAREST-NEIGHBOUR stepping -- no interpolation --
	   which on 44.1kHz tonal material repeats a sample roughly every 11 and
	   reads as thin, tinny distortion. One-shot effects hide it (broadband
	   noise) and Dolphin hides it (its HLE path interpolates), which is why
	   only hardware caught it. Converting HERE, on the decode worker, makes
	   the mixer's ratio exactly 1.0 so it copies samples straight through.
	   PC reports 0 = "keep the native rate": miniaudio resamples properly
	   and a second conversion would only lose quality. */
	uint32_t device_rate = music_device_rate();
	uint32_t out_rate = (device_rate != 0) ? device_rate : s_ring.src_rate;
	audio_resample_init(&s_ring.resample, s_ring.src_rate, out_rate);
	s_ring.in_len = 0;
	s_ring.in_pos = 0;
	/* The rate the ENGINE and the backend see is the OUTPUT rate -- it is
	   what audio_voice_music_start hands the device and what
	   audio_music_telemetry_ms divides by, and both want played-audio
	   wall-clock, not source frames. */
	s_ring.rate = out_rate;
	return true;
}

/* Worker-thread only; pulls up to `want` (<= MUSIC_CHUNK_SAMPLES) mono
   frames AT THE FILE'S NATIVE RATE. Native-channel decode + a manual
   average downmix -- the SAME formula audio_registry.c's ogg load path uses
   for stereo, deliberately NOT the stb_vorbis channel-coercion API (that
   SUMS channels, which can clip; Beta's own sources are already
   near-full-scale). A short read (fewer frames than requested, including
   zero) is the end of the stream. */
static uint32_t music_decode_mono(int16_t* out, uint32_t want) {
	if(!s_ring.vorbis || want == 0)
		return 0;
	if(want > MUSIC_CHUNK_SAMPLES)
		want = MUSIC_CHUNK_SAMPLES;
	int channels = s_ring.native_channels;
	if(channels < 1)
		channels = 1;
	if(channels > 2)
		channels = 2; /* STB_VORBIS_MAX_CHANNELS clamp, belt-and-braces */

	static short s_scratch[2 * MUSIC_CHUNK_SAMPLES]; /* worker-thread-only:
														 exactly one decode
														 thread ever runs */
	int n = stb_vorbis_get_samples_short_interleaved(
		(stb_vorbis*)s_ring.vorbis, channels, s_scratch,
		(int)(channels * want));
	uint32_t got = (n <= 0) ? 0u : (uint32_t)n;
	if(channels == 1) {
		memcpy(out, s_scratch, (size_t)got * sizeof(int16_t));
	} else {
		for(uint32_t k = 0; k < got; k++)
			out[k] = (int16_t)(((int)s_scratch[2 * k] + (int)s_scratch[2 * k + 1])
							   / 2);
	}
	return got;
}

/* Diagnostic fixture ONLY: concatenate the same background track at EOF,
   filling the current source chunk without stopping/repriming the voice.
   Keeping the resampler state joins repeats at the source sample boundary.
   OFF makes exactly the original single decode call. Each successful repeat
   consumes >=1 sample, so even a tiny stream has <=want successful seeks;
   an empty/error stream or failed seek exits instead of spinning. */
static uint32_t music_decode_source(int16_t* out, uint32_t want) {
	if(!s_ring.continuous)
		return music_decode_mono(out, want);
	uint32_t total = 0;
	bool rewound = false;
	while(total < want) {
		uint32_t got = music_decode_mono(out + total, want - total);
		total += got;
		if(total == want)
			break;
		if(!s_ring.vorbis || (rewound && got == 0)
		   || stb_vorbis_get_error((stb_vorbis*)s_ring.vorbis) != VORBIS__no_error
		   || !stb_vorbis_seek_start((stb_vorbis*)s_ring.vorbis))
			break;
		rewound = true;
	}
	return total;
}

/* Worker-thread only; fills one ring half with up to MUSIC_CHUNK_SAMPLES
   mono frames AT THE OUTPUT RATE, resampling when the device needs it. */
static void music_decode_chunk(int16_t* out, uint32_t* out_frames,
							   bool* out_eof) {
	if(!s_ring.vorbis) {
		*out_frames = 0;
		*out_eof = true;
		return;
	}
	if(!audio_resample_active(&s_ring.resample)) {
		/* Rates already match (every PC track, and any 48kHz Wii track):
		   decode straight into the half, exactly as before. */
		uint32_t got = music_decode_source(out, MUSIC_CHUNK_SAMPLES);
		*out_frames = got;
		*out_eof = got < MUSIC_CHUNK_SAMPLES;
		return;
	}

	/* Resampling: the OUTPUT chunk is the fixed size, so pull as much source
	   as it takes. `in_buf` carries whatever the previous chunk did not
	   consume, and the resampler state carries the fractional phase and the
	   last source sample, so chunk boundaries join seamlessly. */
	uint32_t produced = 0;
	bool src_done = false;
	while(produced < MUSIC_CHUNK_SAMPLES) {
		if(s_ring.in_pos >= s_ring.in_len) {
			if(src_done)
				break;
			uint32_t got = music_decode_source(s_ring.in_buf, MUSIC_CHUNK_SAMPLES);
			if(got < MUSIC_CHUNK_SAMPLES)
				src_done = true; /* short read = end of stream */
			s_ring.in_len = got;
			s_ring.in_pos = 0;
			if(got == 0)
				break;
		}
		uint32_t used = 0;
		uint32_t before = produced;
		produced += audio_resample_run(
			&s_ring.resample, s_ring.in_buf + s_ring.in_pos,
			s_ring.in_len - s_ring.in_pos, &used, out + produced,
			MUSIC_CHUNK_SAMPLES - produced);
		s_ring.in_pos += used;
		/* Unreachable by audio_resample_run's contract (with input and
		   output room it always advances one or the other), but a decode
		   worker that could spin is not a risk worth leaving implicit. */
		if(used == 0 && produced == before)
			break;
	}
	*out_frames = produced;
	*out_eof = produced < MUSIC_CHUNK_SAMPLES;
}

static void* music_decode_thread(void* arg) {
	(void)arg;
	/* Worker-local burst latch (audio_music_ring.h REFILL_LOW): set when an
	   idle worker finds a refill due, held until the ring is full, so each
	   wake decodes a run of chunks instead of one. */
	bool bursting = false;
	thread_mutex_lock(&s_ring.lock);
	while(!s_ring.want_stop) {
		if(s_ring.want_open) {
			char path[192];
			snprintf(path, sizeof(path), "%s", s_ring.pending_path);
			int key = s_ring.pending_key;
			uint32_t open_gen = s_ring.gen;
			s_ring.continuous = s_ring.pending_continuous;
			s_ring.want_open = false;
			thread_mutex_unlock(&s_ring.lock);
			bool ok = music_open_file(path);
			thread_mutex_lock(&s_ring.lock);
			for(int h = 0; h < MUSIC_RING_HALVES; h++) {
				s_ring.ready[h] = false;
				s_ring.eof[h] = false;
			}
			s_ring.playing_half = -1;
			s_ring.file_open = ok;
			s_ring.current_key = key;
			s_ring.total_frames_played = 0;
			/* Diagnostic failures must not claim ACTIVE without a decoder.
			   Do not clear a newer request that raced this failed open.
			   Keep the ordinary OFF failure path unchanged. */
			if(!ok && s_ring.continuous && s_ring.gen == open_gen
			   && !s_ring.want_open) {
				s_ring.track_active = false;
				s_ring.is_creative = false;
			}
			bursting = false; /* playing_half -1 => the next pass is due */
			if(music_promote_demote_worker_locked()) {
				music_worker_demote_relock();
			}
			continue;
		}

		int fill = music_ring_fill_target_n(s_ring.ready, s_ring.halves,
											s_ring.playing_half);
		if(fill < 0)
			bursting = false; /* ring full: this burst is done */
		else if(!bursting && music_ring_refill_due_locked())
			bursting = true;
		/* Sleep when full, when idle below the low-water mark, or with no
		   file. Every state change that can make a refill due happens under
		   this lock on the main thread and signals when the SAME predicate
		   turns true (audio_music_update / _tick / _stop / _play_record), so
		   no wake is lost. */
		if(fill < 0 || !bursting || !s_ring.file_open) {
			/* Never sleep promoted when promotion is no longer wanted: drop
			   to 48 with the lock released, then re-evaluate from the top
			   (MAIN may have changed the state meanwhile). */
			if(music_promote_demote_worker_locked()) {
				music_worker_demote_relock();
				continue;
			}
			thread_cond_wait(&s_ring.wake, &s_ring.lock);
#ifdef CAVEWII_TESTING
			s_test_worker_wakes++;
#endif
			continue;
		}
		uint32_t my_gen = s_ring.gen;
		bool decode_promoted = s_ring.promoted;
		thread_mutex_unlock(&s_ring.lock);

		int16_t scratch[MUSIC_CHUNK_SAMPLES];
		uint32_t frames = 0;
		bool eof = false;
		ptime_t decode_t0 = time_get();
		music_decode_chunk(scratch, &frames, &eof);
		uint64_t decode_us = time_diff_us(decode_t0, time_get());

		thread_mutex_lock(&s_ring.lock);
		/* Diagnostics only: a promotion that began mid-decode is not
		   counted for this chunk (decode_promoted was sampled at its start). */
		if(decode_promoted && s_ring.promoted) {
			s_ring.promo.promoted_chunks++;
			if(decode_us > s_ring.promo.promoted_max_us)
				s_ring.promo.promoted_max_us = decode_us > UINT32_MAX
					? UINT32_MAX
					: (uint32_t)decode_us;
		}
		/* A stop/reopen bumped `gen` while we were decoding: this chunk
		   belongs to a request that no longer applies. Discard it -- the
		   loop re-evaluates want_open/fill_target fresh on the next
		   iteration, exactly as if this decode never happened. */
		if(s_ring.gen == my_gen && !s_ring.want_open) {
			memcpy(s_ring.buf[fill], scratch, (size_t)frames * sizeof(int16_t));
			s_ring.buf_frames[fill] = frames;
			s_ring.eof[fill] = eof;
			s_ring.ready[fill] = true;
#ifdef CAVEWII_TESTING
			s_test_chunks_committed++;
#endif
		}
		/* The bank is back at MUSIC_DECODE_PROMOTE_BANK: the rest of the
		   burst (if any) runs at 48 from idle time only, as before. */
		if(music_promote_demote_worker_locked()) {
			music_worker_demote_relock();
		}
	}
	thread_mutex_unlock(&s_ring.lock);
	music_close_vorbis();
	return NULL;
}

/* ===== lifecycle ========================================================== */

void audio_music_init(int volume0_100) {
	if(!s_worker_started) {
		memset(&s_ring, 0, sizeof(s_ring));
		s_ring.playing_half = -1;
		/* The read-ahead ring: 32 x 8KB = 256KB, once per session from the
		   MEM2 carve (mem2_alloc falls back to the normal heap by itself;
		   NULL means both failed). On failure run the legacy four-half
		   ring from .bss -- the exact pre-2026-09-26 behaviour -- and say
		   so, never a silent downgrade. */
		if(!s_ring_full)
			s_ring_full = (int16_t(*)[MUSIC_CHUNK_SAMPLES])mem2_alloc(
				(size_t)MUSIC_RING_HALVES * sizeof(s_ring_fallback[0]));
		if(s_ring_full) {
			s_ring.buf = s_ring_full;
			s_ring.halves = MUSIC_RING_HALVES;
			s_ring.refill_low = MUSIC_RING_REFILL_LOW;
		} else {
			fprintf(stderr,
					"[audio_music] %u KB read-ahead ring allocation failed; "
					"using the %u-half fallback ring\n",
					(unsigned)(MUSIC_RING_HALVES * sizeof(s_ring_fallback[0])
							   / 1024u),
					(unsigned)MUSIC_RING_HALVES_LEGACY);
			s_ring.buf = s_ring_fallback;
			s_ring.halves = MUSIC_RING_HALVES_LEGACY;
			s_ring.refill_low = MUSIC_RING_REFILL_LOW_LEGACY;
		}
		bool mtx_ok = thread_mutex_init(&s_ring.lock);
		bool cond_ok = mtx_ok && thread_cond_init(&s_ring.wake);
		/* Priority 48: ABOVE the chunk mesher (32, chunk_mesher.c), still
		   BELOW the ~64 sim/render threads -- libogc's convention is
		   higher number = higher priority (platform/thread.c). At 32 the
		   decoder shared leftover time round-robin with the mesher and
		   fell behind during a world-load chunk-build storm: Dolphin
		   measured EVERY residual streaming gap inside the first ~30s
		   after a world loads, and zero for the rest of the track. Safe
		   to raise because the decode is a ~17ms burst per chunk and a
		   chunk is 93-186ms of audio -- <20% duty even at the worst rate,
		   so it cannot hog the core the way an unbounded worker would.
		   PC is unaffected: thread.c ignores the priority argument there. */
		bool started
			= cond_ok
			&& thread_create(&s_ring.worker, music_decode_thread, NULL, 48);
		if(!started) {
			if(cond_ok)
				thread_cond_destroy(&s_ring.wake);
			if(mtx_ok)
				thread_mutex_destroy(&s_ring.lock);
			fprintf(stderr, "[audio_music] decode thread failed to start; S3 "
							"background music is disabled for this session\n");
		} else {
			s_worker_started = true;
		}
	}

	audio_music_registry_scan(audio_music_select_pack());

	if(!s_timer_seeded) {
		/* SoundManager(): ticksBeforeMusic = rand.nextInt(12000). */
		int draw = rand_gen_int(&gstate.rand_src, 12000);
		/* debug.music_force ("on"): the debug.weather_force lever
		   precedent (server_weather.c/main.c) -- read directly off the
		   already-loaded client config, the SAME way this function reads
		   "paths.texturepack" a few lines up, since S3 music is a purely
		   client-side concept with no server layering to go through.
		   Applied AFTER the faithful RNG draw so the draw itself stays
		   byte-identical; only the resulting timer value is overridden,
		   and only when armed. "off" (default) leaves the sim untouched. */
		bool force = strcmp(config_read_string(&gstate.config_user,
											   "debug.music_force", "off"),
							"on")
			== 0;
		s_ticks_before_music
			= music_timer_force_lever(music_timer_initial_delay(draw), force);
		s_timer_seeded = true;
	}

	audio_music_set_volume(volume0_100);
}

/* debug.music_pick (audio_music_timer.h's music_pick_lever_decode): with
   debug.music_force "on" and a valid pick, replace the ALREADY-DRAWN survival
   index with the matching catalog entry. Read at timer-fire time (rare), so
   --cw-set / config changes need no re-init. Unset, music_force off, or a
   pick naming a file the pack lacks => `drawn` unchanged. */
static int music_pick_lever_apply(int drawn) {
	if(strcmp(config_read_string(&gstate.config_user, "debug.music_force",
								 "off"),
			  "on")
	   != 0)
		return drawn;
	int key, variant;
	if(!music_pick_lever_decode(
		   config_read_int(&gstate.config_user, "debug.music_pick", 0),
		   AUDIO_MUSIC_KEY_COUNT, AUDIO_MUSIC_MAX_VARIANTS, &key, &variant))
		return drawn;
	for(int i = 0; i < s_catalog_count; i++)
		if(s_catalog[i].key == key && s_catalog[i].variant == variant)
			return i;
	fprintf(stderr, "[music] debug.music_pick names no catalog file; "
					"keeping the random pick\n");
	return drawn;
}

#ifdef CAVEWII_TESTING
void audio_music_test_set_timer(int ticks_before_music) {
	s_ticks_before_music = ticks_before_music;
}
/* The survival key the last pick REQUESTED (synchronous with the tick);
   current_key only catches up once the worker has opened it. */
int audio_music_test_pending_key(void);
int audio_music_test_pending_key(void) {
	thread_mutex_lock(&s_ring.lock);
	int key = s_ring.pending_key;
	thread_mutex_unlock(&s_ring.lock);
	return key;
}
#endif

void audio_music_stop(void) {
	memset(&s_music_service, 0, sizeof s_music_service);
	if(!s_worker_started)
		return;
	audio_voice_music_stop();
#ifdef PLATFORM_WII
	/* Mirrors audio_registry_reload's drain margin exactly: AESND's stop
	   lands at the next 2ms DSP block, not instantly, so give it one
	   block's worth of headroom before the worker is allowed to overwrite
	   the buffer that voice was reading. Cosmetic-glitch-only if skipped
	   (the buffer is a session-lifetime block, never freed) -- not a memory
	   safety issue -- but there is no reason not to pay the same 3ms this
	   codebase already pays for the identical hazard elsewhere. */
	thread_msleep(3);
#endif
	thread_mutex_lock(&s_ring.lock);
	s_ring.pending_path[0] = '\0';
	s_ring.want_open = true; /* empty path == "just close" */
	for(int h = 0; h < MUSIC_RING_HALVES; h++) {
		s_ring.ready[h] = false;
		s_ring.eof[h] = false;
	}
	s_ring.playing_half = -1;
	s_ring.track_active = false;
	s_ring.file_open = false;
	s_ring.is_record = false;
	s_ring.is_creative = false;
	s_ring.gen++;
	music_promote_sync_main_locked(); /* nothing sounding: back to 48 */
	thread_cond_signal(&s_ring.wake);
	thread_mutex_unlock(&s_ring.lock);
}

void audio_music_shutdown(void) {
	memset(&s_music_service, 0, sizeof s_music_service);
	if(!s_worker_started)
		return;
	audio_voice_music_stop();
	thread_mutex_lock(&s_ring.lock);
	s_ring.want_stop = true;
	thread_cond_signal(&s_ring.wake);
	thread_mutex_unlock(&s_ring.lock);
	thread_join(&s_ring.worker);
	thread_cond_destroy(&s_ring.wake);
	thread_mutex_destroy(&s_ring.lock);
	s_worker_started = false;
}

#ifdef CAVEWII_TESTING
/* Test-only: select the ring policy the NEXT track runs with -- the legacy
   4-half/refill-at-1 ring or the production read-ahead ring -- so
   tests/test_music_readahead_equiv.c can prove both hand the voice the same
   PCM. Only legal with no track open (the header's "never changed under a
   live track" rule); returns false otherwise, or for a policy the arrays
   and the allocated ring cannot hold. */
bool audio_music_test_set_ring_policy(int halves, int refill_low);
bool audio_music_test_set_ring_policy(int halves, int refill_low) {
	if(!s_worker_started || halves < 2 || refill_low < 1
	   || refill_low > halves - 1)
		return false;
	thread_mutex_lock(&s_ring.lock);
	bool ok = !s_ring.track_active && !s_ring.file_open
		&& (halves <= MUSIC_RING_HALVES_LEGACY
			|| (s_ring_full && halves <= MUSIC_RING_HALVES));
	if(ok) {
		s_ring.buf = s_ring_full ? s_ring_full : s_ring_fallback;
		s_ring.halves = halves;
		s_ring.refill_low = refill_low;
		for(int h = 0; h < MUSIC_RING_HALVES; h++) {
			s_ring.ready[h] = false;
			s_ring.eof[h] = false;
		}
		s_ring.playing_half = -1;
	}
	thread_mutex_unlock(&s_ring.lock);
	return ok;
}
#endif

/* ===== observer telemetry (real-Wii audio-switch evidence) ===============
   The music half of the pack-switch record (audio_registry.c carries the same
   gate, sink and escaper -- three files, no shared header, because none of
   their headers is part of this change). Inert unless telemetry is armed:
   audio_tel_armed() is tested before any clock read, any record probe and any
   formatting, and it is the same hw_telemetry_active() && !quiesced() gate
   main.c:2387 puts in front of every other observer producer. */

#ifdef CAVEWII_TESTING
static bool (*s_music_observer_sink)(const char* line);
void audio_music_test_set_observer(bool (*sink)(const char* line));
void audio_music_test_set_observer(bool (*sink)(const char* line)) {
	s_music_observer_sink = sink;
}
#endif

static bool music_tel_armed(void) {
#ifdef CAVEWII_TESTING
	if(s_music_observer_sink)
		return true;
#endif
	return hw_telemetry_active() && !hw_telemetry_quiesced();
}

static bool music_tel_emit(const char* line) {
#ifdef CAVEWII_TESTING
	if(s_music_observer_sink)
		return s_music_observer_sink(line);
#endif
	return hw_telemetry_emit_observer(line);
}

/* A pack root is a player-chosen folder name; escape what JSON requires so a
   quote or backslash in it cannot break the record. */
static size_t music_tel_json_str(char* dst, size_t size, size_t off,
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

/* Records are resolved on demand (audio_music_play_record_for_item), never
   scanned, so there is no count to read -- probe the two Beta record stems
   against the effective root the same way that function builds its path.
   ONLY under armed telemetry: two stat-equivalent opens on a path that has
   just done dozens of them, and zero work otherwise. */
static int music_tel_record_count(void) {
	static const uint16_t item_ids[] = {2256, 2257}; /* 13, cat */
	int found = 0;
	for(size_t i = 0; i < sizeof(item_ids) / sizeof(item_ids[0]); i++) {
		const char* stem = audio_music_record_file_stem(item_ids[i]);
		if(!stem)
			continue;
		char path[sizeof(s_audio_pack) + 32];
		snprintf(path, sizeof(path), "%s/records/%s.ogg", s_audio_pack, stem);
		if(music_file_exists(path))
			found++;
	}
	return found;
}

void audio_music_registry_reload(void) {
	if(!s_worker_started) {
		audio_music_init(0); /* first touch: a plain load, mirrors
								 audio_registry_reload's own fallback */
		return;
	}
	bool tel = music_tel_armed();
	ptime_t t_start = time_get();
	audio_music_stop(); /* the currently-open path may not survive the pack swap */
	audio_music_registry_scan(audio_music_select_pack());
	if(!tel)
		return;
	char line[512];
	size_t off = (size_t)snprintf(line, sizeof(line),
								  "{\"event\":\"music_reload\",\"root\":");
	off = music_tel_json_str(line, sizeof(line), off, s_audio_pack);
	if(off >= sizeof(line))
		return;
	off += (size_t)snprintf(line + off, sizeof(line) - off,
							",\"tracks\":%d,\"creative\":%d,\"records\":%d,"
							"\"ms\":%d}",
							s_catalog_count, s_creative_catalog_count,
							music_tel_record_count(),
							(int)time_diff_ms(t_start, time_get()));
	if(off >= sizeof(line))
		return;
	(void)music_tel_emit(line);
}

/* 2026-09-26 musicra hal1_pinned run: debug.music_force armed, but the
   active pack (paths.texturepack = sd:/assets) had no music/, so the timer
   silently returned on catalog_count == 0 for all 5 minutes -- music_starts
   0 AND music_rejects 0, indistinguishable from a broken lever. When the
   lever is armed, say so ONCE (stderr always; a telemetry event too when
   telemetry is armed). No effect when music_force is off. Hits on a
   0-track tick of either catalog. */
static bool s_force_notice_done = false;
static void music_force_no_tracks_notice(void) {
	if(s_force_notice_done)
		return;
	if(strcmp(config_read_string(&gstate.config_user, "debug.music_force",
								 "off"),
			  "on")
	   != 0) {
		s_force_notice_done = true;
		return;
	}
	/* hw_telemetry is initialised before config load on the Wii, so it is
	   already armed by the first world tick on a hardware run. */
	bool tel = music_tel_armed();
	s_force_notice_done = true;
	fprintf(stderr,
			"[music] debug.music_force is on but pack \"%s\" has 0 music "
			"tracks -- nothing can play\n",
			s_audio_pack);
	if(!tel)
		return;
	char line[320];
	size_t off = (size_t)snprintf(line, sizeof(line),
								  "{\"event\":\"music_force_no_tracks\","
								  "\"root\":");
	off = music_tel_json_str(line, sizeof(line), off, s_audio_pack);
	if(off + 16 >= sizeof(line))
		return;
	snprintf(line + off, sizeof(line) - off, ",\"tracks\":0}");
	(void)music_tel_emit(line);
}

bool audio_music_set_volume(int volume0_100) {
	return audio_set_music_volume(volume0_100);
}

/* ===== per-tick / per-frame hooks ========================================= */

void audio_music_tick(void) {
	if(!s_worker_started)
		return;

	/* Per-world playlist selection (creative-mode add-on,
	   docs/creative_mode.md, maintainer ruling 2026-09-03): a creative
	   world draws from the CREATIVE catalog when it has >=1 registered
	   file; every other case -- survival, or a creative world whose player
	   never imported the six creative tracks -- draws from the Beta
	   survival catalog EXACTLY as before this feature existed.
	   gstate.config_creative is the client-side per-world flag
	   (game_state.h; armed at SRPC_LOAD_WORLD, screen_select_world.c).

	   SURVIVAL EQUIVALENCE: whenever gstate.config_creative is false (every
	   existing test, every pre-2026-09-03 build), use_creative is always
	   false, so `catalog`/`catalog_count` are exactly s_catalog/
	   s_catalog_count and every line below runs identically to the
	   pre-creative-music version of this function -- same early-return
	   shape (no RNG draw at all when the active catalog is empty), same
	   timer call, same pick. Nothing about the survival path's RNG stream
	   or timing changes. */
	bool use_creative = audio_music_use_creative_catalog(
		gstate.config_creative, s_creative_catalog_count);
	const struct audio_music_catalog_entry* catalog
		= use_creative ? s_creative_catalog : s_catalog;
	int catalog_count = use_creative ? s_creative_catalog_count : s_catalog_count;
	if(catalog_count == 0) {
		music_force_no_tracks_notice();
		return;
	}

	thread_mutex_lock(&s_ring.lock);
	bool track_playing = s_ring.track_active;
	thread_mutex_unlock(&s_ring.lock);

	/* Client-side cosmetic roll -- the sanctioned gstate.rand_src
	   substitution client_display_tick.c's cave_mood_tick already uses for
	   Beta randoms that are not gameplay-visible. Drawn every tick
	   regardless of whether the timer is about to fire, the same
	   over-drawing cave_mood_tick already does per-chunk; harmless. */
	int draw = rand_gen_int(&gstate.rand_src, 12000);
	bool backend_ready = audio_music_backend_ready();
#ifdef CAVEWII_TESTING
	if(s_test_backend_ready >= 0)
		backend_ready = s_test_backend_ready != 0;
#endif
	if(!music_timer_tick(&s_ticks_before_music, backend_ready, track_playing,
						 catalog_count > 0, draw))
		return;

	/* The faithful uniform pick is ALWAYS drawn, lever or not, so
	   gstate.rand_src advances identically either way. */
	int idx = rand_gen_int(&gstate.rand_src, catalog_count);
	if(!use_creative)
		idx = music_pick_lever_apply(idx);
	const struct audio_music_catalog_entry* e = &catalog[idx];

	thread_mutex_lock(&s_ring.lock);
	snprintf(s_ring.pending_path, sizeof(s_ring.pending_path), "%s", e->path);
	s_ring.pending_key = e->key;
	/* Read config only on MAIN; the worker snapshots this under the lock.
	   Default OFF preserves Beta silence/timer/RNG and finite PCM exactly. */
	s_ring.pending_continuous = config_read_int(&gstate.config_user,
		"debug.music_continuous_int", 0) == 1;
	s_ring.want_open = true;
	s_ring.track_active = true;
	/* Synchronous, like is_record's own set in audio_music_play_record --
	   correct the instant this call returns, never deferred to the
	   worker's later want_open handling (see is_creative's field
	   comment). */
	s_ring.is_creative = use_creative;
	s_ring.gen++;
	thread_cond_signal(&s_ring.wake);
	thread_mutex_unlock(&s_ring.lock);
}

void audio_music_update(void) {
	if(s_music_service.running || s_music_service.wake) {
		/* Ordinary service owns the escape: complete retained PCM once,
		 * perform any global-CV wake/EOF close here, with NO AUDIO credit. */
		struct audio_service_budget b;
		audio_service_get_budget(&b, 500);
		b.ordinary = true;
		(void)audio_music_update_service(&b);
		return;
	}
	if(!s_worker_started)
		return;

	/* G9 promotion gate, sampled once per frame OUTSIDE s_ring.lock (it
	   reads the tick-boost owner under g9_lock; no nested locks). */
	bool promote_gate = music_promote_gate_now();
	thread_mutex_lock(&s_ring.lock);
	s_ring.promote_gate = promote_gate;
	music_promote_sync_main_locked();
	if(!s_ring.track_active) {
		thread_mutex_unlock(&s_ring.lock);
		return;
	}

	if(s_ring.playing_half < 0) {
		/* Nothing started yet for this track: wait for the first chunk. */
		if(!s_ring.ready[0]) {
			thread_mutex_unlock(&s_ring.lock);
			return;
		}
		s_ring.playing_half = 0;
		int16_t* pcm = s_ring.buf[0];
		uint32_t frames = s_ring.buf_frames[0];
		uint32_t rate = s_ring.rate;
		/* DEADLOCK FIX (2026-09-01, probe-proven on hw: music_starts pinned
		 * at 1 forever): with playing_half==-1 the two-half
		 * music_ring_fill_target could only ever point at half 0, so the
		 * worker filled it once and cond_waited; setting playing_half=0 is
		 * what made half 1 fillable, so the worker HAD to be woken here or
		 * it slept forever (run staged_far_idle_2m_20260901_070110 is the
		 * receipt). The 4-half fill_target now also fills 1..N-1 while
		 * playing_half is still -1, so this is no longer the only thing
		 * that can wake a stalled worker -- but it is still correct and
		 * still needed to wake it for the half this start just freed up. */
		thread_cond_signal(&s_ring.wake);
		thread_mutex_unlock(&s_ring.lock);
		music_voice_start(pcm, frames, rate);
		/* ...and fall straight into the drain loop: during the load storm
		   the frame that starts a track is itself 100-300ms long, and
		   waiting for the next one to hand over chunk 2 is exactly the
		   supply cap this change exists to remove. */
	} else {
		thread_mutex_unlock(&s_ring.lock);
	}

	/* DRAIN LOOP (2026-09-01). Previously this function handed the backend
	   AT MOST ONE chunk per frame. Dolphin measured 13.8s of silence in the
	   first 40s after a world load and zero gaps thereafter: with frame
	   times of 100-300ms and a chunk worth only 93-186ms of audio, one
	   chunk per frame is BELOW consumption however fast the decoder runs.
	   So keep handing chunks over while the backend will take them.

	   The bound is music_ring_ready_run() + 1: every banked chunk gets
	   handed over, then exactly ONE final pass runs the EOF/underrun check
	   -- so the underrun dial still ticks at most once per frame, the same
	   rate the single-hand-off version had, and the loop provably cannot
	   spin (ready_run is capped at halves-1 by construction). */
	thread_mutex_lock(&s_ring.lock);
	int passes = music_ring_ready_run_n(s_ring.ready, s_ring.halves,
										s_ring.playing_half)
		+ 1;
	thread_mutex_unlock(&s_ring.lock);

	for(int pass = 0; pass < passes; pass++) {
		if(music_voice_playing())
			return; /* backend full / still sounding: nothing more this frame */
		thread_mutex_lock(&s_ring.lock);
		if(!s_ring.track_active || s_ring.playing_half < 0) {
			thread_mutex_unlock(&s_ring.lock);
			return;
		}

		int finished_half = s_ring.playing_half;
		/* NOTE (2026-09-01): the frames credit used to live HERE, before
		 * the advance check -- so a stalled advance re-credited the same
		 * finished chunk EVERY frame (telemetry music_ms raced 4.1x
		 * realtime while music_starts sat at 1; same hw receipt as the
		 * deadlock fix above). It now lives in the EOF branch and after a
		 * SUCCESSFUL advance only: each chunk is counted exactly once,
		 * whether the frame hands over one chunk or four. */
		if(s_ring.eof[finished_half]) {
			uint32_t last_frames = s_ring.buf_frames[finished_half];
			thread_mutex_unlock(&s_ring.lock);
			/* DRAINED GATE (2026-09-01, with the Wii looped-ring backend):
			 * audio_voice_music_playing() answering false means "the device
			 * can take another chunk", NOT "the device finished sounding".
			 * On the Wii ring up to S-2 slots (~0.9s) are still queued at
			 * that moment, so stopping here -- which is what this branch
			 * used to do -- would clip the tail off EVERY track. Wait,
			 * re-polled once per frame, until the device says it has played
			 * everything handed over. Deliberately checked with the lock
			 * DROPPED: the backend query takes its own lock on PC, and
			 * every audio_voice_music_* call in this file is already made
			 * unlocked (no nested-lock ordering to reason about). Headless
			 * (no live device) reports drained immediately, so the test
			 * path is unchanged. */
			if(!audio_voice_music_drained())
				return;
			thread_mutex_lock(&s_ring.lock);
			/* Re-check what we sampled before the unlock. Only the main
			 * thread (this function, audio_music_stop,
			 * audio_music_play_record) ever moves playing_half/
			 * track_active, so this cannot actually change under us today
			 * -- it is a cheap guard against that stopping being true
			 * later, not a live race. */
			if(!s_ring.track_active || s_ring.playing_half != finished_half) {
				thread_mutex_unlock(&s_ring.lock);
				return;
			}
			s_ring.total_frames_played += last_frames;
			/* Track over. The timer already reseeded when it fired; nothing
			   here restarts it early -- the next pick waits for the SAME
			   already-scheduled window (Beta's own "long silent gaps"
			   shape, audio_music_timer.h's top-of-file note). */
			thread_mutex_unlock(&s_ring.lock);
			audio_music_stop();
			return;
		}

		int next = music_ring_advance_n(s_ring.ready, s_ring.halves,
										finished_half);
		if(next < 0) {
			/* Underrun: the decoder has not kept up. Do NOT hand the device
			   a not-yet-ready buffer -- just wait and re-poll next frame;
			   the voice stays silent for this gap rather than playing
			   garbage. Belt (2026-09-01): the decoder is needed RIGHT NOW
			   -- wake it unconditionally so a missed/spurious earlier
			   signal can never strand it in cond_wait while we spin on
			   underruns. */
			s_ring.underrun_count++;
			music_promote_sync_main_locked();
			thread_cond_signal(&s_ring.wake);
			thread_mutex_unlock(&s_ring.lock);
			return;
		}
		/* Successful advance: NOW the finished half's frames are credited
		   -- exactly once per chunk (see the note above). */
		s_ring.total_frames_played += s_ring.buf_frames[finished_half];

		/* Safe to free the just-finished half NOW: audio_voice_music_playing()
		   already confirmed the device is done reading it
		   (audio_music_ring.h's ordering note). */
		s_ring.ready[finished_half] = false;
		int16_t* pcm = s_ring.buf[next];
		uint32_t frames = s_ring.buf_frames[next];
		uint32_t rate = s_ring.rate;
		s_ring.playing_half = next;
		/* A half just freed for refilling. Wake the worker only once a
		   refill is DUE (audio_music_ring.h REFILL_LOW): below the low-water
		   mark it would only go straight back to sleep, and those per-chunk
		   wakes are exactly what the read-ahead ring exists to remove. The
		   worker evaluates the same predicate under this same lock, so the
		   wake that makes it due is never lost. */
		if(music_ring_refill_due_locked())
			thread_cond_signal(&s_ring.wake);
		/* The bank just shrank by one: promote the worker if it is now
		   below MUSIC_DECODE_PROMOTE_BANK (G9 only; a no-op otherwise). */
		music_promote_sync_main_locked();
		thread_mutex_unlock(&s_ring.lock);
		music_voice_start(pcm, frames, rate);
	}
}

static enum audio_service_result music_service_lock(struct audio_service_budget* b,
	struct thread_mutex_guard* guard, ptime_t* entered) {
	enum audio_service_result r = audio_service_enter(b, AUDIO_UNIT_STATE);
	if(r != AUDIO_SERVICE_DONE) return r;
	*entered = time_get();
	if(b->ordinary) { thread_mutex_lock(&s_ring.lock); return AUDIO_SERVICE_DONE; }
	if(thread_mutex_try_guard(&s_ring.lock, guard)) {
		audio_service_cover(b, AUDIO_UNIT_STATE, MUSIC_RING_HALVES, 8u);
		return AUDIO_SERVICE_DONE;
	}
	audio_service_cover(b, AUDIO_UNIT_STATE, 0, 16u);
	(void)audio_service_leave(b, AUDIO_UNIT_STATE, *entered);
	return AUDIO_SERVICE_UNSAFE;
}
static bool music_service_unlock(struct audio_service_budget* b,
	struct thread_mutex_guard* guard, ptime_t entered) {
	if(b->ordinary) thread_mutex_unlock(&s_ring.lock);
	else thread_mutex_release_guard(&s_ring.lock, guard);
	return audio_service_leave(b, AUDIO_UNIT_STATE, entered);
}

enum audio_service_result audio_music_update_service(struct audio_service_budget* b) {
	if(!s_worker_started) return AUDIO_SERVICE_DONE;
	/* A service pass holds a finite ready-run snapshot (<=32 entries).
	 * A READY half stays READY and owned until native completion; worker
	 * fill_target never overwrites it. MAIN stop/restart resets the cursor. */
	for(unsigned step = 0; step < MUSIC_RING_HALVES+2; step++) {
		struct thread_mutex_guard guard;
		ptime_t entered;
		enum audio_service_result r = music_service_lock(b, &guard, &entered);
		if(r != AUDIO_SERVICE_DONE) return r;
		if(!s_ring.track_active || s_ring.want_open) {
			bool inactive = !s_ring.track_active;
			memset(&s_music_service, 0, sizeof s_music_service);
			bool ok = music_service_unlock(b, &guard, entered);
			return ok && inactive ? AUDIO_SERVICE_DONE : AUDIO_SERVICE_UNSAFE;
		}
		if(s_music_service.running && s_music_service.generation != s_ring.gen) {
			memset(&s_music_service, 0, sizeof s_music_service);
			(void)music_service_unlock(b, &guard, entered);
			return AUDIO_SERVICE_UNSAFE;
		}
		if(s_music_service.wake) {
			/* Tuxedo's global CV queue has no supported finite-snapshot
			 * signalling API. This path is explicitly ordinary/UNSAFE. */
			if(!b->ordinary) { (void)music_service_unlock(b, &guard, entered); return AUDIO_SERVICE_UNSAFE; }
			thread_cond_signal(&s_ring.wake);
			s_music_service.wake = false;
		}
		if(!s_music_service.running) {
			s_music_service.running = true;
			s_music_service.generation = s_ring.gen;
			s_music_service.passes = music_ring_ready_run_n(s_ring.ready,
				s_ring.halves, s_ring.playing_half < 0 ? 0 : s_ring.playing_half) + 1;
			if(s_ring.playing_half < 0) {
				if(!s_ring.ready[0]) {
					s_music_service.running = false;
					(void)music_service_unlock(b, &guard, entered);
					return AUDIO_SERVICE_MORE;
				}
				s_music_service.transferring = true;
				s_music_service.previous = -1;
				s_music_service.half = 0;
			}
		}
		int half = s_music_service.transferring ? s_music_service.half : s_ring.playing_half;
		const int16_t* pcm = s_ring.buf[half];
		uint32_t frames = s_ring.buf_frames[half], rate = s_ring.rate;
		bool eof = s_ring.eof[half], transfer = s_music_service.transferring;
		if(!music_service_unlock(b, &guard, entered)) return AUDIO_SERVICE_UNSAFE;
		if(transfer) {
			if(!s_music_service.submitted) {
				r = audio_voice_music_service(pcm, frames, rate, b);
#ifdef PLATFORM_PC
				if(b->ordinary && r == AUDIO_SERVICE_UNSAFE) {
					audio_voice_music_start(pcm, frames, (float)rate);
					r = AUDIO_SERVICE_DONE;
				}
#endif
				if(r != AUDIO_SERVICE_DONE) return r;
				s_music_service.submitted = true;
			}
			r = music_service_lock(b, &guard, &entered);
			if(r != AUDIO_SERVICE_DONE) return r;
			int previous = s_music_service.previous;
			if(previous >= 0) {
				s_ring.total_frames_played += s_ring.buf_frames[previous];
				s_ring.ready[previous] = false;
				s_music_service.passes--;
			}
			s_ring.playing_half = half;
			s_music_service.transferring = s_music_service.submitted = false;
			s_music_service.wake = previous < 0 || music_ring_refill_due_locked();
			bool ok = music_service_unlock(b, &guard, entered);
#ifdef CAVEWII_TESTING
			if(s_test_pcm_sink) s_test_pcm_sink(pcm, frames, rate);
#endif
			if(!ok) return AUDIO_SERVICE_UNSAFE;
			continue;
		}
		bool full;
		r = audio_voice_music_poll_service(false, &full, b);
		if(r != AUDIO_SERVICE_DONE) return r;
		if(full || s_music_service.passes == 0) {
			s_music_service.running = false;
			return AUDIO_SERVICE_DONE;
		}
		if(eof) {
			bool drained;
			r = audio_voice_music_poll_service(true, &drained, b);
			if(r != AUDIO_SERVICE_DONE) return r;
			if(!drained) { s_music_service.running = false; return AUDIO_SERVICE_DONE; }
			/* EOF stop/close/drain includes an ordinary 3ms pause and CV
			 * wake. Never bury it in an admitted unit or grant credit here. */
			if(!b->ordinary) return AUDIO_SERVICE_UNSAFE;
			thread_mutex_lock(&s_ring.lock);
			s_ring.total_frames_played += frames;
			thread_mutex_unlock(&s_ring.lock);
			audio_music_stop();
			return AUDIO_SERVICE_DONE;
		}
		r = music_service_lock(b, &guard, &entered);
		if(r != AUDIO_SERVICE_DONE) return r;
		int next = music_ring_advance_n(s_ring.ready, s_ring.halves, half);
		if(next < 0) {
			s_ring.underrun_count++;
			s_music_service.wake = true;
			s_music_service.passes = 0;
		} else {
			s_music_service.previous = half;
			s_music_service.half = next;
			s_music_service.transferring = true;
		}
		if(!music_service_unlock(b, &guard, entered)) return AUDIO_SERVICE_UNSAFE;
	}
	return AUDIO_SERVICE_MORE;
}

bool audio_music_service_witness(struct audio_service_witness* w) {
	*w = (struct audio_service_witness){0};
	bool active = false, pending = false;
	if(s_worker_started) {
		struct thread_mutex_guard guard;
		if(!thread_mutex_try_guard(&s_ring.lock, &guard)) return false;
		active = s_ring.track_active;
		w->generation = s_ring.gen;
		pending = s_ring.want_open || s_ring.want_stop || s_music_service.running
			|| s_music_service.wake || (active && (s_ring.playing_half < 0
			|| music_ring_ready_run_n(s_ring.ready, s_ring.halves, s_ring.playing_half) == 0));
		thread_mutex_release_guard(&s_ring.lock, &guard);
	}
	if(!audio_voice_music_witness(w)) return false;
	w->pending |= pending;
	w->inactive = !active;
	struct audio_service_budget b;
	audio_service_get_budget(&b, 500);
	uint64_t margin = (uint64_t)b.limits->wake_refill_us + b.limits->backend_us + b.limits->drift_us;
	w->latest_refill_us = w->reserve_us > margin ? w->reserve_us-(uint32_t)margin : 0;
	return w->valid && !w->pending;
}

bool audio_music_is_playing(void) {
	if(!s_worker_started)
		return false;
	thread_mutex_lock(&s_ring.lock);
	bool active = s_ring.track_active;
	thread_mutex_unlock(&s_ring.lock);
	return active;
}

uint32_t audio_music_lease_reserve_us(bool* worker_pending) {
	*worker_pending = false;
	if(!s_worker_started) return UINT32_MAX;
	thread_mutex_lock(&s_ring.lock);
	bool active = s_ring.track_active;
	*worker_pending = active && (s_ring.want_open || (s_ring.file_open
		&& music_ring_ready_run_n(s_ring.ready, s_ring.halves, s_ring.playing_half) == 0));
	thread_mutex_unlock(&s_ring.lock);
	return active ? audio_voice_music_reserve_us() : UINT32_MAX;
}

const char* audio_music_current_key(void) {
	if(!s_worker_started)
		return NULL;
	thread_mutex_lock(&s_ring.lock);
	bool active = s_ring.track_active;
	bool creative = s_ring.is_creative;
	int key = s_ring.current_key;
	thread_mutex_unlock(&s_ring.lock);
	if(!active)
		return NULL;
	/* creative-mode add-on: a creative pick indexes s_music_creative_key_names,
	   a DIFFERENT array than the survival s_music_key_names below -- the
	   two enums are never confused because is_creative alone decides which
	   table `key` is read against. */
	if(creative) {
		if(key < 0 || key >= AUDIO_MUSIC_CREATIVE_KEY_COUNT)
			return NULL;
		return s_music_creative_key_names[key];
	}
	if(key < 0 || key >= AUDIO_MUSIC_KEY_COUNT)
		return NULL;
	return s_music_key_names[key];
}

unsigned audio_music_underrun_count(void) {
	if(!s_worker_started)
		return 0;
	thread_mutex_lock(&s_ring.lock);
	unsigned n = s_ring.underrun_count;
	thread_mutex_unlock(&s_ring.lock);
	return n;
}

/* ===== jukebox record playback ============================================
   See audio_music.h's group banner + audio_music_record.h for the full
   preempt citation ([decompile 740c583: SoundManager.playStreaming]). */

const char* audio_music_record_file_stem(uint16_t item_id) {
	switch(item_id) {
		case 2256: return "13";  /* ITEM_RECORD_13 (item_types.h) */
		case 2257: return "cat"; /* ITEM_RECORD_CAT (item_types.h) -- the
									 in-game item NAME is "Cat"
									 (items_object.h), but Beta's real
									 streaming/ resource file is lowercase
									 "cat.ogg"; only the file lookup folds
									 case, the display name is untouched. */
		default: return NULL;
	}
}

/* Snapshot of "what is sounding right now", for feeding
   audio_music_record_transition -- shared by play_record and stop_record so
   the classification logic lives in exactly one place. MUST be called with
   s_ring.lock held. */
static enum audio_music_source music_current_source_locked(void) {
	if(!s_ring.track_active)
		return AUDIO_MUSIC_SOURCE_NONE;
	return s_ring.is_record ? AUDIO_MUSIC_SOURCE_RECORD
							: AUDIO_MUSIC_SOURCE_BGMUSIC;
}

void audio_music_play_record(const char* path) {
	if(!s_worker_started || !path || path[0] == '\0')
		return;

	thread_mutex_lock(&s_ring.lock);
	enum audio_music_source current = music_current_source_locked();
	thread_mutex_unlock(&s_ring.lock);

	bool stop_previous = false;
	audio_music_record_transition(current, /*starting_record=*/true,
								  &stop_previous);
	if(stop_previous)
		audio_music_stop(); /* preempt whatever was sounding -- the ONE
								voice this engine has for both background
								music and records means "stop streaming" +
								"stop BgMusic" collapse into this one call */

	thread_mutex_lock(&s_ring.lock);
	snprintf(s_ring.pending_path, sizeof(s_ring.pending_path), "%s", path);
	/* Not a real background-music key -- current_key stays out of
	   [0, AUDIO_MUSIC_KEY_COUNT), which audio_music_current_key() already
	   treats as "no key" (NULL); audio_music_telemetry_key() instead
	   consults is_record directly for the authoritative "record" report.
	   Set SYNCHRONOUSLY here (not left to the worker's later want_open
	   handling, which runs on its own thread's schedule) so there is no
	   window where is_record/track_active already read true but
	   current_key still holds a STALE background key from whatever played
	   before -- audio_music_current_key()/_telemetry_key() must be correct
	   the instant this call returns, not just eventually. */
	s_ring.pending_key = AUDIO_MUSIC_KEY_COUNT;
	s_ring.pending_continuous = false; /* records never repeat */
	s_ring.current_key = AUDIO_MUSIC_KEY_COUNT;
	s_ring.want_open = true;
	s_ring.track_active = true;
	s_ring.is_record = true;
	/* A record is never a creative pick -- cleared here too (belt over
	   audio_music_stop's own clear above) so is_creative can never read
	   true at the same time as is_record. */
	s_ring.is_creative = false;
	s_ring.gen++;
	thread_cond_signal(&s_ring.wake);
	thread_mutex_unlock(&s_ring.lock);
}

void audio_music_play_record_for_item(uint16_t item_id) {
	const char* stem = audio_music_record_file_stem(item_id);
	if(!stem)
		return;
	char path[sizeof(s_audio_pack) + 32];
	snprintf(path, sizeof(path), "%s/records/%s.ogg", s_audio_pack, stem);
	audio_music_play_record(path);
}

void audio_music_stop_record(void) {
	if(!s_worker_started)
		return;

	thread_mutex_lock(&s_ring.lock);
	enum audio_music_source current = music_current_source_locked();
	thread_mutex_unlock(&s_ring.lock);

	bool stop_previous = false;
	audio_music_record_transition(current, /*starting_record=*/false,
								  &stop_previous);
	if(stop_previous)
		audio_music_stop();
}

bool audio_music_is_playing_record(void) {
	if(!s_worker_started)
		return false;
	thread_mutex_lock(&s_ring.lock);
	bool active = s_ring.track_active && s_ring.is_record;
	thread_mutex_unlock(&s_ring.lock);
	return active;
}

/* ===== hw_telemetry numeric surface ======================================= */

unsigned audio_music_telemetry_key(void) {
	if(!s_worker_started)
		return 0;
	thread_mutex_lock(&s_ring.lock);
	bool active = s_ring.track_active;
	bool rec = s_ring.is_record;
	bool creative = s_ring.is_creative;
	int key = s_ring.current_key;
	thread_mutex_unlock(&s_ring.lock);
	if(!active)
		return 0;
	if(rec)
		return (unsigned)AUDIO_MUSIC_KEY_COUNT + 1u;
	if(creative)
		return audio_music_creative_telemetry_key(key);
	if(key < 0 || key >= AUDIO_MUSIC_KEY_COUNT)
		return 0; /* defensive; should not happen for a non-record track */
	return (unsigned)key + 1u;
}

uint32_t audio_music_telemetry_ms(void) {
	if(!s_worker_started)
		return 0;
	thread_mutex_lock(&s_ring.lock);
	bool active = s_ring.track_active;
	uint32_t frames = s_ring.total_frames_played;
	uint32_t rate = s_ring.rate;
	thread_mutex_unlock(&s_ring.lock);
	if(!active || rate == 0)
		return 0;
	return (uint32_t)(((uint64_t)frames * 1000u) / rate);
}
