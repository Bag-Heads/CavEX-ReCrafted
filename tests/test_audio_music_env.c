/*
	S3 background-music env test (docs/finish_line_plan.md S3). Decompile
	740c583 is the spec for every constant; every expected value below is
	HAND-COMPUTED from audio_music_timer.h / audio_music_ring.h's own
	documented formulas, never derived by running the unit under test (the
	poisoned-golden rule).

	Coverage:
	  - A: the PURE playRandomMusicIfReady timer state machine
	    (audio_music_timer.h) -- decrement while counting, freeze while a
	    track plays OR audio is not ready, fire+reseed at 0, retried-forever
	    with an empty pool (no crash, no progress, ticks stay 0).
	  - B: the PURE N-half decode-ring bookkeeping (audio_music_ring.h,
	    widened 2 -> 4 halves) -- fill_target banks the WHOLE ring before
	    the first start and otherwise scans forward from the playing half,
	    wrapping, never targeting the playing half; advance hands off in
	    strict +1 order (never skipping to a later ready half, which would
	    DROP a chunk) or reports an UNDERRUN (-1); ready_run gives the drain
	    loop its bound, so one slow world-load frame can hand over three
	    banked chunks instead of one.
	  - C: headless registry scan (audio_music_track_count() against a
	    scratch music/ pack -- catalogs FILE PATHS only, no decode; a
	    fixture full of garbage bytes proves this, since decode would
	    reject it but the scan does not even try) and the
	    audio_music_registry_reload drain, the audio_registry_reload
	    precedent. Also pins audio_music_table.h's documented real Beta
	    1.7.3 file layout (calm x3, hal x4, nuance x2, piano x3 = 12
	    files, NOT 10 -- a prior header-comment arithmetic slip) against
	    the scan, so the table and the enumerated files cannot drift
	    apart again (test_registry_scan_real_beta_layout).
	  - D: music.volume config round-trip (config_set_int/config_read_int),
	    the test_options_env.c audio.volume precedent.
	  - E: the audio_ready gate wired end-to-end, headless: with
	    audio.enabled=off (audio_init(false, ...), the SAME headless mode
	    test_audio_env.c uses -- no real device is ever touched, keeping
	    this test environment-independent), audio_music_tick() must NEVER
	    flip track_active even after far more ticks than the documented max
	    12000-tick initial delay -- the exact regression this file's own
	    fix (music_timer_tick's `audio_ready` parameter) targets: Beta
	    nests the WHOLE block, including the countdown, under
	    `loaded && musicVolume != 0`.
	  - F: Music is now a live, enabled options-screen control
	    (screen_options.c) -- covered by tests/test_options_env.c's updated
	    control-inventory row; not duplicated here.
	  - G: the PURE jukebox record-preempt transition
	    (audio_music_record.h) -- inserting always preempts whatever was
	    sounding (another record OR background music), ejecting only stops
	    a record that was actually active, background music is never
	    special-cased away.
	  - H: the PURE debug.music_force lever (audio_music_timer.h's
	    music_timer_force_lever) -- "off" leaves the faithful seed
	    untouched, "on" collapses it to 1 regardless of the seed.
	  - J: the PURE looped-ring bookkeeping the Wii streaming backend now
	    runs on (audio_stream_ring.h) -- PRIMING (chunk 1 lands with the
	    device silent, chunk 2 arms the voice; a pending ring can never
	    underrun however long the main thread stalls), the S-2 fill guard,
	    one slot freed per slot-duration of clock, the write index never
	    landing on the slot the read head is in (swept every ms over
	    several laps), the underrun -> re-prime policy, drained becoming
	    true only once the head passes the LAST written sample, a short
	    final chunk, and a one-short-chunk track that only the
	    end-of-track flush can arm. Plus the headless half of the engine's
	    drained gate: with no live device audio_voice_music_drained() is
	    true, which is exactly what keeps sections E/I's EOF behaviour
	    unchanged (a headless backend has no queue, so it cannot express
	    "not drained" -- that half of the gate is proven on the pure
	    header, not through audio_music_update).

	  - I: record playback headless end-to-end (audio_music_play_record/
	    _for_item/_stop_record/_is_playing_record/_record_file_stem) --
	    state bookkeeping only (no real device, the test_audio_env.c
	    precedent): a garbage-byte "records/13.ogg" proves play_record
	    flips track-active/is-record immediately, independent of whether
	    the decode worker can actually open the file, and that it preempts
	    (does not stack with) a background pick.

	  - L: the creative-mode playlist add-on (maintainer ruling
	    2026-09-03, docs/creative_mode.md) -- the PURE per-world selection
	    gate and telemetry-band arithmetic (audio_music_table.h's
	    audio_music_use_creative_catalog/_creative_telemetry_key), a
	    registry scan proving the Beta and creative catalogs populate and
	    report SEPARATELY (never merged), and a headless end-to-end run
	    proving section E's audio_ready freeze holds unchanged for a
	    creative world with a fully-populated creative catalog. Survival
	    behaviour itself is proven byte-identical by construction, not by a
	    new test: audio_music_tick()'s creative gate only ever activates
	    when gstate.config_creative is true, which every OTHER test in this
	    file leaves at its zeroed default -- sections A-K above already ARE
	    the survival-equivalence proof.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "platform/audio_music_record.h"
#include "platform/audio_music_ring.h"
#include "platform/audio_music_table.h"
#include "platform/audio_music_timer.h"
#include "platform/audio_resample.h"
#include "platform/audio_stream_ring.h"

static char s_fixture_TMP_PACK[128]; /* was #define TMP_PACK='test_audio_music_env_pack_tmp' (a literal) */
#define TMP_PACK s_fixture_TMP_PACK /* scratch pack (gitignored) */

/* ===== A: the pure timer state machine ================================== */

static void test_timer_initial_delay(void) {
	assert(music_timer_initial_delay(0) == 0);
	assert(music_timer_initial_delay(11999) == 11999);
	assert(music_timer_initial_delay(6000) == 6000);
	puts("  A: initial delay = the raw nextInt(12000) draw, unchanged OK");
}

static void test_timer_tick(void) {
	/* Plain countdown: audio ready, nothing playing, tracks available --
	   decrements by exactly one per call, no reseed, never fires early. */
	int ticks = 5;
	for(int expect = 4; expect >= 0; expect--) {
		bool fired = music_timer_tick(&ticks, true, false, true, 999);
		assert(!fired);
		assert(ticks == expect);
	}
	/* Fires at exactly 0: reseeds to draw+12000 [SoundManager.
	   playRandomMusicIfReady: ticksBeforeMusic = rand.nextInt(12000)+12000]. */
	assert(music_timer_tick(&ticks, true, false, true, 0));
	assert(ticks == 12000);
	ticks = 0;
	assert(music_timer_tick(&ticks, true, false, true, 11999));
	assert(ticks == 23999);

	/* FROZEN while a track is playing: neither decrements nor fires, ticks
	   already at 0 stays exactly 0 [the WHOLE block, decrement included,
	   nests inside !playing(...)]. */
	ticks = 0;
	assert(!music_timer_tick(&ticks, true, true, true, 555));
	assert(ticks == 0);
	ticks = 7;
	assert(!music_timer_tick(&ticks, true, true, true, 555));
	assert(ticks == 7); /* untouched, not merely "didn't fire" */

	/* FROZEN while audio is not ready (device off / music.volume 0) --
	   the SAME freeze as track_playing, matching Beta's
	   `loaded && musicVolume != 0` outer gate wrapping the decrement too. */
	ticks = 3;
	assert(!music_timer_tick(&ticks, false, false, true, 555));
	assert(ticks == 3);
	ticks = 0;
	assert(!music_timer_tick(&ticks, false, false, true, 555));
	assert(ticks == 0);

	/* Empty pool at zero: retried every tick, doing nothing -- Beta's
	   `SoundPoolEntry e = getRandomSound(); if (e != null) {...}` leaves
	   ticksBeforeMusic untouched at 0 forever when the pool is empty. */
	ticks = 0;
	for(int k = 0; k < 5; k++) {
		assert(!music_timer_tick(&ticks, true, false, false, 42));
		assert(ticks == 0);
	}
	/* A nonzero countdown does still tick down even with an empty pool --
	   only the FIRE branch requires track_available; Beta's decrement
	   happens unconditionally inside `!playing()`. */
	ticks = 2;
	assert(!music_timer_tick(&ticks, true, false, false, 42));
	assert(ticks == 1);
	assert(!music_timer_tick(&ticks, true, false, false, 42));
	assert(ticks == 0);
	assert(!music_timer_tick(&ticks, true, false, false, 42)); /* now stalls */
	assert(ticks == 0);

	puts("  A: tick state machine (decrement/freeze-on-playing/"
		 "freeze-on-not-ready/fire+reseed/empty-pool-stall) OK");
}

/* ===== B: the pure ring bookkeeping ====================================== */

/* The hand-checked expectations below assume a FOUR-half ring played in the
   order 0,1,2,3,0,... The helpers take the active half count, so they are
   pinned at n = MUSIC_RING_HALVES_LEGACY (4) -- the ring these goldens were
   derived for and still the fallback ring audio_music.c runs when the
   read-ahead allocation fails. The production 32-half ring and its refill
   low-water mark (2026-09-26) are proven end to end, against this legacy
   policy, by tests/test_music_readahead_equiv.c; the constants are pinned
   here so a future change to either fails loudly. */
#define RING_N MUSIC_RING_HALVES_LEGACY
static void test_ring_halves_constant(void) {
	assert(MUSIC_RING_HALVES_LEGACY == 4);
	assert(MUSIC_RING_REFILL_LOW_LEGACY == 1);
	assert(MUSIC_RING_HALVES == 32);
	assert(MUSIC_RING_REFILL_LOW == 8);
}

static void test_ring_fill_target(void) {
	bool ready[RING_N];
	test_ring_halves_constant();

	/* Nothing started (playing_half -1): NOTHING has been handed to the
	   device, so every half is fillable and the scan starts at 0. The
	   worker can bank the WHOLE ring before playback begins -- the head
	   start the world-load storm needs (this is the 2->4 widening's real
	   payoff, and a deliberate change from the two-half version, which
	   returned -1 the moment half 0 was ready). */
	for(int h = 0; h < RING_N; h++)
		ready[h] = false;
	assert(music_ring_fill_target_n(ready, RING_N, -1) == 0);
	ready[0] = true;
	assert(music_ring_fill_target_n(ready, RING_N, -1) == 1);
	ready[1] = true;
	assert(music_ring_fill_target_n(ready, RING_N, -1) == 2);
	ready[2] = true;
	assert(music_ring_fill_target_n(ready, RING_N, -1) == 3);
	ready[3] = true;
	assert(music_ring_fill_target_n(ready, RING_N, -1) == -1); /* whole ring banked */

	/* Half 0 playing: the scan runs 1,2,3 and NEVER returns the playing
	   half, however free it looks. */
	ready[0] = false; /* the playing half: must still not be a target */
	ready[1] = false;
	ready[2] = true;
	ready[3] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 0) == 1);
	ready[1] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 0) == -1);

	/* WRAPAROUND. Half 2 playing: the scan runs 3,0,1 in that order. */
	for(int h = 0; h < RING_N; h++)
		ready[h] = false;
	assert(music_ring_fill_target_n(ready, RING_N, 2) == 3);
	ready[3] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 2) == 0);
	ready[0] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 2) == 1);
	ready[1] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 2) == -1); /* half 2 is playing */

	/* Half 3 playing: the scan runs 0,1,2. */
	for(int h = 0; h < RING_N; h++)
		ready[h] = false;
	assert(music_ring_fill_target_n(ready, RING_N, 3) == 0);
	ready[0] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 3) == 1);
	ready[1] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 3) == 2);
	ready[2] = true;
	assert(music_ring_fill_target_n(ready, RING_N, 3) == -1);

	puts("  B: fill_target over 4 halves (banks the WHOLE ring before the "
		 "first start, in order, wrapping, never the playing half, idle when "
		 "all are ready) OK");
}

static void test_ring_advance(void) {
	bool ready[RING_N];
	test_ring_halves_constant();

	/* Strict order: half h hands off to h+1, never further ahead. */
	for(int h = 0; h < RING_N; h++)
		ready[h] = true;
	assert(music_ring_advance_n(ready, RING_N, 0) == 1);
	assert(music_ring_advance_n(ready, RING_N, 1) == 2);
	assert(music_ring_advance_n(ready, RING_N, 2) == 3);
	assert(music_ring_advance_n(ready, RING_N, 3) == 0); /* wraparound */
	assert(music_ring_advance_n(ready, RING_N, -1) == 0);

	/* NOT ready: underrun (-1) -- never hand over a not-yet-decoded half. */
	ready[1] = false;
	assert(music_ring_advance_n(ready, RING_N, 0) == -1);
	/* ...and it must NOT skip to a later ready half: halves carry
	   consecutive audio, so skipping would DROP a chunk, not just stall. */
	assert(ready[2] && ready[3]);
	assert(music_ring_advance_n(ready, RING_N, 0) == -1);
	ready[1] = true;
	assert(music_ring_advance_n(ready, RING_N, 0) == 1);

	ready[0] = false;
	assert(music_ring_advance_n(ready, RING_N, 3) == -1); /* wraparound underrun */

	puts("  B: advance over 4 halves (strict +1 order with wraparound, -1 "
		 "UNDERRUN rather than skipping to a later ready half) OK");
}

/* The drain loop's bound: how many chunks ONE frame can hand over. */
static void test_ring_ready_run(void) {
	bool ready[RING_N];
	test_ring_halves_constant();

	/* Three halves banked behind the playing one: a single slow world-load
	   frame can hand over all three (the loop then spends its one extra
	   pass on the EOF/underrun check). Capped at HALVES-1 by construction
	   -- the playing half is never counted, so the loop cannot spin. */
	for(int h = 0; h < RING_N; h++)
		ready[h] = true;
	assert(music_ring_ready_run_n(ready, RING_N, 0) == RING_N - 1);
	assert(music_ring_ready_run_n(ready, RING_N, 0) == 3);
	assert(music_ring_ready_run_n(ready, RING_N, 2) == 3); /* wraparound: 3,0,1 */

	/* It counts a RUN, not a total: a gap stops it even if later halves
	   are ready (advance would underrun there anyway). */
	ready[2] = false;
	assert(music_ring_ready_run_n(ready, RING_N, 0) == 1); /* half 1 only */
	ready[1] = false;
	assert(music_ring_ready_run_n(ready, RING_N, 0) == 0); /* nothing banked */
	ready[1] = true;
	ready[2] = true;
	ready[0] = false; /* stop the wrap-around run after half 3 */
	assert(music_ring_ready_run_n(ready, RING_N, 1) == 2); /* halves 2,3, then 0 is
													not ready */

	/* Nothing playing yet behaves like half 0 playing (the drain loop only
	   ever asks after the first-start path has set playing_half = 0). */
	for(int h = 0; h < RING_N; h++)
		ready[h] = true;
	assert(music_ring_ready_run_n(ready, RING_N, -1) == RING_N - 1);

	puts("  B: ready_run (3 banked halves => 3 hand-offs in ONE frame, stops "
		 "at the first gap, capped at HALVES-1 so the drain loop cannot "
		 "spin) OK");
}

/* ===== C: headless registry scan ========================================= */

static void config_root_ensure(void) {
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
}

static void write_garbage_file(const char* path) {
	/* The registry scan only checks EXISTENCE (fopen succeeds) -- it never
	   decodes at scan time, the whole point of streaming vs the S0/S1/S2
	   decode-AT-LOAD model. Garbage bytes prove that: a real .ogg decoder
	   would reject this instantly, but the catalog still counts it. */
	FILE* f = fopen(path, "wb");
	assert(f);
	static const uint8_t junk[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	assert(fwrite(junk, 1, sizeof(junk), f) == sizeof(junk));
	fclose(f);
}

static void test_registry_scan(void) {
	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/music"));
	write_garbage_file(test_fixture_join(TMP_PACK, "/music/calm1.ogg"));
	write_garbage_file(test_fixture_join(TMP_PACK, "/music/calm2.ogg"));
	write_garbage_file(test_fixture_join(TMP_PACK, "/music/piano1.ogg"));
	/* A file OUTSIDE the digit-suffix scan namespace (calm99 exceeds no
	   cap here -- AUDIO_MUSIC_MAX_VARIANTS=14 -- but a wrong extension
	   must NOT be picked up). */
	write_garbage_file(test_fixture_join(TMP_PACK, "/music/calm3.wav"));

	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 3); /* calm1, calm2, piano1 -- NOT the .wav */

	/* Back to the (music-less) default pack: the catalog empties out --
	   there is no bundled assets/music/, the legal per-client-import-only
	   model (docs/deviations.md's sound precedent, extended). */
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 0);

	remove(test_fixture_join(TMP_PACK, "/music/calm1.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/calm2.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/piano1.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/calm3.wav"));
	puts("  C: headless registry scan (3 real keys found, wrong extension "
		 "skipped, empties out on a music-less pack) OK");
}

/* Pins audio_music_table.h's documented real Beta 1.7.3 file layout --
   calm{1,2,3} + hal{1,2,3,4} + nuance{1,2} + piano{1,2,3} = 12 files -- so a
   future edit to that comment, AUDIO_MUSIC_TABLE, or the scan cap
   (AUDIO_MUSIC_MAX_VARIANTS) cannot silently drift the two apart again (a
   prior revision of the header comment claimed 10, an arithmetic slip: the
   real total is 12). The real Beta .ogg files are never committed to this
   repo (legal: per-client-import-only); this fixture stands in for them
   the same way test_registry_scan's garbage bytes do -- the scan only
   checks existence, never decodes. */
static void test_registry_scan_real_beta_layout(void) {
	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/music"));
	static const char* const real_beta_files[] = {
		"calm1", "calm2", "calm3",
		"hal1", "hal2", "hal3", "hal4",
		"nuance1", "nuance2",
		"piano1", "piano2", "piano3",
	};
	const int real_beta_file_count =
		(int)(sizeof(real_beta_files) / sizeof(real_beta_files[0]));
	assert(real_beta_file_count == 12);
	for(int i = 0; i < real_beta_file_count; i++) {
		char rel[64];
		snprintf(rel, sizeof(rel), "/music/%s.ogg", real_beta_files[i]);
		write_garbage_file(test_fixture_join(TMP_PACK, rel));
	}

	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 12);

	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_music_registry_reload();

	for(int i = 0; i < real_beta_file_count; i++) {
		char rel[64];
		snprintf(rel, sizeof(rel), "/music/%s.ogg", real_beta_files[i]);
		remove(test_fixture_join(TMP_PACK, rel));
	}
	puts("  C: real Beta 1.7.3 music layout (calm x3, hal x4, nuance x2, "
		 "piano x3) scans to exactly 12 tracks OK");
}

/* ===== D: music.volume config round-trip ================================= */

static void test_config_round_trip(void) {
	char path[96];
	snprintf(path, sizeof(path), "test_audio_music_env_%d.json", 1);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fputs("{\"music\":{\"volume\":100}}\n", f) >= 0);
	assert(fclose(f) == 0);

	struct config first;
	assert(config_create(&first, path));
	assert(config_set_int(&first, "music.volume", 42));
	assert(config_save(&first, path));
	config_destroy(&first);

	struct config second;
	assert(config_create(&second, path));
	assert(config_read_int(&second, "music.volume", -1) == 42);
	config_destroy(&second);
	assert(remove(path) == 0);
	puts("  D: music.volume config round-trip 1/1");
}

/* ===== E: the audio_ready gate, headless end-to-end ====================== */

static void test_audio_disabled_never_fires(void) {
	/* HEADLESS: no real device is ever opened here (the test_audio_env.c
	   precedent) -- audio.enabled=off keeps this environment-independent. */
	audio_init(false, 100);

	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/music"));
	write_garbage_file(test_fixture_join(TMP_PACK, "/music/calm1.ogg"));
	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));

	audio_music_init(100); /* music.volume=100, but audio.enabled=off */
	assert(audio_music_track_count() >= 1);
	assert(!audio_music_backend_ready());

	/* Far more ticks than the documented max 12000-tick initial delay:
	   if the `audio_ready` freeze (this session's own fix) ever regressed,
	   this would flip track_active well before the loop ends. */
	for(int k = 0; k < 20000; k++)
		audio_music_tick();
	assert(!audio_music_is_playing());
	assert(audio_music_current_key() == NULL);
	assert(audio_music_underrun_count() == 0);

	audio_music_shutdown();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	remove(test_fixture_join(TMP_PACK, "/music/calm1.ogg"));
	puts("  E: audio-disabled headless run never fires the timer even past "
		 "the max initial delay (the loaded&&musicVolume!=0 freeze) OK");
}

/* ===== G: the pure record-preempt transition ============================= */

static void test_record_transition(void) {
	bool stop_previous;

	/* Insert into silence: new source RECORD, nothing to preempt. */
	stop_previous = true; /* poison: must be overwritten */
	assert(audio_music_record_transition(AUDIO_MUSIC_SOURCE_NONE, true,
										 &stop_previous)
		   == AUDIO_MUSIC_SOURCE_RECORD);
	assert(!stop_previous);

	/* Insert while background music is sounding: preempts it. */
	assert(audio_music_record_transition(AUDIO_MUSIC_SOURCE_BGMUSIC, true,
										 &stop_previous)
		   == AUDIO_MUSIC_SOURCE_RECORD);
	assert(stop_previous);

	/* Insert while ANOTHER record is sounding: preempts it too (Beta:
	   "if playing(streaming) stop it" runs unconditionally, before the new
	   source is even resolved). */
	assert(audio_music_record_transition(AUDIO_MUSIC_SOURCE_RECORD, true,
										 &stop_previous)
		   == AUDIO_MUSIC_SOURCE_RECORD);
	assert(stop_previous);

	/* Eject while a record IS sounding: stops it, new source NONE. */
	assert(audio_music_record_transition(AUDIO_MUSIC_SOURCE_RECORD, false,
										 &stop_previous)
		   == AUDIO_MUSIC_SOURCE_NONE);
	assert(stop_previous);

	/* Eject while NOTHING is sounding (a defensive double-eject): no-op,
	   stays NONE. */
	assert(audio_music_record_transition(AUDIO_MUSIC_SOURCE_NONE, false,
										 &stop_previous)
		   == AUDIO_MUSIC_SOURCE_NONE);
	assert(!stop_previous);

	/* Eject while BACKGROUND MUSIC is sounding: Beta never actually reaches
	   this state from the real call sites (eject only ever follows an
	   insert), but the model still leaves background music alone rather
	   than stopping it -- playStreaming(null,...) never touches "BgMusic". */
	assert(audio_music_record_transition(AUDIO_MUSIC_SOURCE_BGMUSIC, false,
										 &stop_previous)
		   == AUDIO_MUSIC_SOURCE_BGMUSIC);
	assert(!stop_previous);

	/* NULL out_stop_previous is tolerated (audio_music.c never actually
	   passes NULL, but the header's contract says "may be NULL"). */
	assert(audio_music_record_transition(AUDIO_MUSIC_SOURCE_NONE, true, NULL)
		   == AUDIO_MUSIC_SOURCE_RECORD);

	puts("  G: record-preempt transition (insert always preempts, eject "
		 "only stops an actual record, NULL out-param tolerated) OK");
}

/* ===== H: the pure debug.music_force lever ================================ */

static void test_force_lever(void) {
	assert(music_timer_force_lever(9999, false) == 9999);
	assert(music_timer_force_lever(0, false) == 0);
	assert(music_timer_force_lever(11999, false) == 11999);
	/* "on" collapses ANY seeded delay to 1, regardless of the faithful draw
	   underneath it. */
	assert(music_timer_force_lever(9999, true) == 1);
	assert(music_timer_force_lever(0, true) == 1);
	assert(music_timer_force_lever(11999, true) == 1);
	puts("  H: debug.music_force lever (off = untouched, on = collapses to "
		 "1 regardless of the seed) OK");
}

/* H2: the pure debug.music_pick decode (100 * telemetry key + digit). */
static void test_pick_lever_decode(void) {
	int k = -7, v = -7;
	assert(!music_pick_lever_decode(0, AUDIO_MUSIC_KEY_COUNT,
									AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(!music_pick_lever_decode(-201, AUDIO_MUSIC_KEY_COUNT,
									AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(!music_pick_lever_decode(99, AUDIO_MUSIC_KEY_COUNT,
									AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(!music_pick_lever_decode(501, AUDIO_MUSIC_KEY_COUNT,
									AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(!music_pick_lever_decode(214, AUDIO_MUSIC_KEY_COUNT,
									AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(k == -7 && v == -7); /* untouched when inert */
	assert(music_pick_lever_decode(201, AUDIO_MUSIC_KEY_COUNT,
								   AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(k == MUSIC_HAL && v == 1);
	assert(music_pick_lever_decode(100, AUDIO_MUSIC_KEY_COUNT,
								   AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(k == MUSIC_CALM && v == 0);
	assert(music_pick_lever_decode(413, AUDIO_MUSIC_KEY_COUNT,
								   AUDIO_MUSIC_MAX_VARIANTS, &k, &v));
	assert(k == MUSIC_PIANO && v == 13);
	puts("  H2: debug.music_pick decode (201 = hal1, 0/out of range inert) "
		 "OK");
}

/* ===== I: record playback, headless end-to-end ============================ */

static void test_record_file_stem(void) {
	assert(strcmp(audio_music_record_file_stem(2256), "13") == 0);
	assert(strcmp(audio_music_record_file_stem(2257), "cat") == 0);
	assert(audio_music_record_file_stem(0) == NULL);
	assert(audio_music_record_file_stem(2255) == NULL);
	assert(audio_music_record_file_stem(2258) == NULL);
	puts("  I: record item id -> file stem (13/cat, NULL for anything "
		 "else) OK");
}

static void test_record_playback_headless(void) {
	/* HEADLESS: no real device (the test_audio_env.c precedent) -- the
	   decode worker still runs (it is a real thread pulling a real, if
	   garbage, file), only the hardware voice is never opened. */
	audio_init(false, 100);

	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/records"));
	write_garbage_file(test_fixture_join(TMP_PACK, "/records/13.ogg"));
	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));

	audio_music_init(100);
	assert(!audio_music_is_playing());
	assert(!audio_music_is_playing_record());
	assert(audio_music_telemetry_key() == 0);
	assert(audio_music_telemetry_ms() == 0);

	/* Unknown item id: no-op, nothing starts. */
	audio_music_play_record_for_item(1);
	assert(!audio_music_is_playing_record());

	/* A real record id resolves to <pack>/records/13.ogg -- track/record
	   state flips IMMEDIATELY (synchronously, under the caller's own lock),
	   independent of whether the decode worker's async fopen/vorbis_open
	   ever succeeds on the garbage bytes. */
	audio_music_play_record_for_item(2256);
	assert(audio_music_is_playing_record());
	assert(audio_music_is_playing());
	assert(audio_music_telemetry_key()
		   == (unsigned)AUDIO_MUSIC_KEY_COUNT + 1u);
	/* Not a real background key -- current_key must read as "no key". */
	assert(audio_music_current_key() == NULL);

	/* Preempt: inserting a SECOND record while the first is still "active"
	   (garbage bytes never produced a decodable stream, so this is testing
	   the synchronous re-arm, not an audible swap) must not crash and must
	   still report exactly one record active afterwards. */
	audio_music_play_record_for_item(2257);
	assert(audio_music_is_playing_record());

	/* Eject: stops the record outright. */
	audio_music_stop_record();
	assert(!audio_music_is_playing_record());
	assert(!audio_music_is_playing());
	assert(audio_music_telemetry_key() == 0);
	assert(audio_music_telemetry_ms() == 0);

	/* A second stop_record with nothing active is a documented no-op, not
	   a crash. */
	audio_music_stop_record();
	assert(!audio_music_is_playing_record());

	/* A NULL/empty path is a no-op, not a crash. */
	audio_music_play_record(NULL);
	assert(!audio_music_is_playing_record());
	audio_music_play_record("");
	assert(!audio_music_is_playing_record());

	audio_music_shutdown();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	remove(test_fixture_join(TMP_PACK, "/records/13.ogg"));
	puts("  I: record playback headless (play/eject state, telemetry key, "
		 "preempt, NULL/empty/unknown-id no-ops) OK");
}

/* ===== J: the pure looped-ring bookkeeping ================================
   Every expected value below is HAND-COMPUTED from audio_stream_ring.h's
   documented formulas (consumed = elapsed_ms * rate / 1000, floored;
   consumed_total = consumed / slot_samples), never read back out of the
   unit under test -- the poisoned-golden rule.

   The fixture rate is 22050Hz mono, so one 4096-frame slot is
   4096 * 1000 / 22050 = 185.759... ms. (PRODUCTION now feeds this ring at
   the Wii device rate of 48000Hz, where a slot is 85.3ms -- the decode
   worker pre-resamples, section K -- but the ring's arithmetic is entirely
   rate-agnostic, so the fixture keeps the rate whose boundaries are already
   hand-derived here. The ring GEOMETRY is production's: 9 slots, guard 2.)
   Hand-derived boundaries used below:
     - the head first leaves slot 0 at t = 186ms
       (185 * 22050 / 1000 = 4079 < 4096; 186 * 22050 / 1000 = 4101 >= 4096)
     - slot_ms floors to 185 (4096000 / 22050 = 185.76)
*/
#define J_RATE 22050u
#define J_SLOT AUDIO_STREAM_RING_SLOT_SAMPLES
#define J_SLOTS AUDIO_STREAM_RING_SLOTS

/* The header's own formula, restated independently here so the test does
   not consult the unit under test for its expectations. */
static uint64_t j_expect_consumed(uint64_t elapsed_ms) {
	return (elapsed_ms * (uint64_t)J_RATE) / 1000u;
}

/* Priming: the ring accepts chunks with the device SILENT and only asks for
   a PlayVoice on the chunk that brings the queue to PRIME_SLOTS. */
static void test_stream_ring_prime_and_guard(void) {
	struct audio_stream_ring r;
	audio_stream_ring_init(&r, J_SLOTS, J_SLOT);

	/* Stopped: always room, never underrunning, trivially drained. */
	assert(audio_stream_ring_has_free_slot(&r, 0));
	assert(!audio_stream_ring_underrun(&r, 0));
	assert(audio_stream_ring_drained(&r, 0));

	/* (1) The FIRST chunk resets the ring into slot 0 but must NOT start the
	   voice -- that one-slot head start is exactly what produced the
	   world-load restart clusters Dolphin found. */
	struct audio_stream_ring_write w
		= audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
	assert(w.reset_ring);
	assert(!w.start_voice);
	assert(w.slot == 0);
	assert(audio_stream_ring_filled(&r, 0) == 1); /* pending counts as queued */
	assert(!audio_stream_ring_drained(&r, 0));	  /* pending != played */
	assert(audio_stream_ring_has_free_slot(&r, 0));
	/* A SILENT device cannot underrun, however long the main thread stalls:
	   this is the whole point of priming. */
	assert(!audio_stream_ring_underrun(&r, 0));
	assert(!audio_stream_ring_underrun(&r, 60000));
	assert(audio_stream_ring_has_free_slot(&r, 60000));
	assert(!audio_stream_ring_drained(&r, 60000));

	/* (2) The chunk that crosses PRIME_SLOTS is the one that arms the voice,
	   and it does NOT reset the ring (slot 0 keeps chunk 1). Even after a
	   long stall the second chunk still lands in slot 1, never slot 0. */
	assert(AUDIO_STREAM_RING_PRIME_SLOTS == 2u);
	w = audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
	assert(!w.reset_ring);
	assert(w.start_voice);
	assert(w.slot == 1);
	assert(audio_stream_ring_filled(&r, 0) == 2);
	assert(audio_stream_ring_head_slot(&r, 0) == 0);
	assert(audio_stream_ring_slot_ms(&r) == 185);
	/* Now running: a flush has nothing left to do. */
	assert(!audio_stream_ring_flush_pending(&r, 0));

	/* (3) Slots keep filling ahead until EXACTLY S-2 are queued, then the
	   guard shuts the door. No further chunk touches the device. */
	for(uint32_t k = 2; k <= J_SLOTS - AUDIO_STREAM_RING_GUARD_SLOTS - 1; k++) {
		assert(audio_stream_ring_has_free_slot(&r, 0));
		w = audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
		assert(!w.reset_ring);
		assert(!w.start_voice);
		assert(w.slot == k);
		assert(audio_stream_ring_filled(&r, 0) == k + 1);
	}
	assert(audio_stream_ring_filled(&r, 0)
		   == J_SLOTS - AUDIO_STREAM_RING_GUARD_SLOTS);
	assert(!audio_stream_ring_has_free_slot(&r, 0)); /* full: 7 of 9 queued */

	/* (4) One slot-duration of clock frees EXACTLY one slot. At t=186 the
	   head has passed slot 0 and sits in slot 1 (hand-derived above). */
	assert(j_expect_consumed(185) == 4079); /* < 4096: still slot 0 */
	assert(j_expect_consumed(186) == 4101); /* >= 4096: now slot 1 */
	assert(audio_stream_ring_head_slot(&r, 185) == 0);
	assert(!audio_stream_ring_has_free_slot(&r, 185));
	assert(audio_stream_ring_head_slot(&r, 186) == 1);
	assert(audio_stream_ring_filled(&r, 186)
		   == J_SLOTS - AUDIO_STREAM_RING_GUARD_SLOTS - 1);
	assert(audio_stream_ring_has_free_slot(&r, 186));

	/* (5) Sweep EVERY millisecond over several laps of the ring, handing a
	   chunk over the instant the guard allows one. Three invariants: the
	   write never lands on the slot the head is reading, never more than
	   S-3 slots ahead of it, and never re-arms the device -- a steadily fed
	   ring neither underruns nor re-primes. */
	unsigned writes = 0;
	for(uint64_t t = 187; t <= 3000; t++) {
		assert(!audio_stream_ring_underrun(&r, t));
		if(!audio_stream_ring_has_free_slot(&r, t))
			continue;
		uint32_t head = audio_stream_ring_head_slot(&r, t);
		w = audio_stream_ring_start_chunk(&r, t, J_RATE, J_SLOT);
		assert(!w.reset_ring);
		assert(!w.start_voice);
		assert(w.slot != head);
		uint32_t ahead = (w.slot + J_SLOTS - head) % J_SLOTS;
		assert(ahead >= 1);
		assert(ahead <= J_SLOTS - AUDIO_STREAM_RING_GUARD_SLOTS - 1);
		assert(audio_stream_ring_filled(&r, t)
			   <= J_SLOTS - AUDIO_STREAM_RING_GUARD_SLOTS);
		writes++;
	}
	/* ~2.8s of clock at 185.76ms per slot frees 15 slots; the sweep must
	   actually have exercised the wrap, not silently no-opped. */
	assert(writes >= J_SLOTS);

	puts("  J: looped ring priming (chunk 1 silent, chunk 2 arms the voice, "
		 "a pending ring never underruns) + S-2 guard, one slot freed per "
		 "slot-duration, write never on the head's slot over several laps OK");
}

static void test_stream_ring_underrun_and_drain(void) {
	struct audio_stream_ring r;
	audio_stream_ring_init(&r, J_SLOTS, J_SLOT);

	/* (6) A clock jump past everything written IS the underrun: the head
	   would otherwise loop onto stale slots. Prime with two chunks at t=0,
	   so 2 slots = 8192 frames are queued from t=0.
	   371 * 22050 / 1000 = 8180 -> consumed_total 1 (still inside slot 1);
	   372 * 22050 / 1000 = 8202 -> consumed_total 2 == written: underrun. */
	(void)audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
	struct audio_stream_ring_write w
		= audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
	assert(w.start_voice && w.slot == 1);
	assert(j_expect_consumed(371) == 8180);
	assert(j_expect_consumed(372) == 8202);
	assert(!audio_stream_ring_underrun(&r, 371));
	assert(audio_stream_ring_underrun(&r, 372));
	assert(audio_stream_ring_filled(&r, 372) == 0);
	assert(audio_stream_ring_has_free_slot(&r, 372));

	/* (7) The restart RE-PRIMES rather than resuming off one slot: the next
	   chunk resets the ring and stays silent, the one after it arms the
	   voice. Folded into start_chunk, so even a caller that never polled
	   cannot write into the slot the head is sitting on. */
	w = audio_stream_ring_start_chunk(&r, 5000, J_RATE, J_SLOT);
	assert(w.reset_ring);
	assert(!w.start_voice);
	assert(w.slot == 0);
	assert(audio_stream_ring_filled(&r, 5000) == 1);
	assert(!audio_stream_ring_underrun(&r, 99999)); /* silent again */
	w = audio_stream_ring_start_chunk(&r, 5000, J_RATE, J_SLOT);
	assert(!w.reset_ring && w.start_voice && w.slot == 1);

	/* A rate change is treated the same way (a new track that skipped a stop
	   must not inherit the old track's read-head estimate). */
	w = audio_stream_ring_start_chunk(&r, 5000, 44100u, J_SLOT);
	assert(w.reset_ring);
	assert(!w.start_voice);
	assert(w.slot == 0);

	/* (8) DRAINED trails has_free_slot: it goes true only once the head has
	   passed the LAST written sample. Two 4096-frame chunks at 22050Hz are
	   8192 frames, done at t=372 (hand-derived above), not at t=371. */
	audio_stream_ring_init(&r, J_SLOTS, J_SLOT);
	(void)audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
	(void)audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
	assert(!audio_stream_ring_drained(&r, 0));
	assert(!audio_stream_ring_drained(&r, 371));
	assert(audio_stream_ring_drained(&r, 372));
	/* ...while has_free_slot said "room" from the very first millisecond,
	   which is exactly why the engine's EOF branch may not stop on it. */
	assert(audio_stream_ring_has_free_slot(&r, 0));
	/* A stopped ring is drained by definition, pending state cleared. */
	audio_stream_ring_stop(&r);
	assert(audio_stream_ring_drained(&r, 0));
	assert(!audio_stream_ring_flush_pending(&r, 0));

	/* (9) A SHORT final chunk counts its real length, not the zero pad:
	   3 full slots + 1000 frames = 13288 frames, all queued from t=0.
	   602 * 22050 / 1000 = 13274 < 13288 -> not drained;
	   603 * 22050 / 1000 = 13296 >= 13288 -> drained. */
	audio_stream_ring_init(&r, J_SLOTS, J_SLOT);
	for(int k = 0; k < 3; k++)
		(void)audio_stream_ring_start_chunk(&r, 0, J_RATE, J_SLOT);
	w = audio_stream_ring_start_chunk(&r, 0, J_RATE, 1000u);
	assert(!w.reset_ring && !w.start_voice);
	assert(w.slot == 3);
	assert(j_expect_consumed(602) == 13274);
	assert(j_expect_consumed(603) == 13296);
	assert(!audio_stream_ring_drained(&r, 601));
	assert(!audio_stream_ring_drained(&r, 602));
	assert(audio_stream_ring_drained(&r, 603));

	/* (10) A track of ONE short chunk -- a one-chunk record -- never reaches
	   the prime threshold, so nothing ever armed the voice. It must still
	   play: drained stays FALSE while pending (never silently dropped), and
	   the caller's end-of-track flush arms it.
	   1000 frames at 22050Hz play out 46ms after the PlayVoice
	   (45 * 22050 / 1000 = 992 < 1000; 46 * 22050 / 1000 = 1014 >= 1000). */
	audio_stream_ring_init(&r, J_SLOTS, J_SLOT);
	w = audio_stream_ring_start_chunk(&r, 0, J_RATE, 1000u);
	assert(w.reset_ring);
	assert(!w.start_voice); /* below the threshold: still silent */
	assert(w.slot == 0);
	assert(!audio_stream_ring_drained(&r, 0));
	assert(!audio_stream_ring_drained(&r, 100000)); /* NOT dropped */
	assert(audio_stream_ring_flush_pending(&r, 1000)); /* the escape hatch */
	assert(!audio_stream_ring_flush_pending(&r, 1000)); /* idempotent */
	assert(j_expect_consumed(45) == 992);
	assert(j_expect_consumed(46) == 1014);
	assert(!audio_stream_ring_drained(&r, 1045));
	assert(audio_stream_ring_drained(&r, 1046));

	puts("  J: looped ring underrun (clock jump -> re-prime from slot 0, "
		 "rate change too) + drained (only past the LAST written sample, "
		 "short final chunk at its real length, one-short-chunk track still "
		 "armed by the end-of-track flush) OK");
}


/* ===== K: the pure device-rate resampler ==================================
   audio_resample.h exists because AESND's DSP mixer converts every voice to
   its fixed 48kHz output by NEAREST-NEIGHBOUR stepping -- no interpolation
   at all -- which a real-Wii LISTEN caught as thin/tinny distortion on
   44.1kHz music ("almost TINNY like a cheap speaker"; one-shot effects are
   broadband noise and hide it, and Dolphin's HLE audio interpolates and
   hides it too). The decode worker now converts to the device rate itself
   so the mixer's ratio is exactly 1.0.

   The expectations below are hand-derived from the header's documented
   arithmetic (step = (src << 16) / dst, truncated; output position advances
   by step per output sample), never read back out of the unit under test.
   Note the ONE-SAMPLE LATENCY the streaming contract implies: the last
   input frame of a call is carried in `prev` and emitted at the head of the
   next call, so a first call over n frames emits n-1 and every later call
   over n frames emits n. */
#define K_SINE_HZ 440u
#define K_SRC_RATE 44100u
#define K_DST_RATE 48000u
#define K_IN_FRAMES K_SRC_RATE /* exactly one second */
#define K_OUT_CAP (K_DST_RATE + 256u)
#define K_FEED_CHUNK 1024u

static int16_t k_in[K_IN_FRAMES];
static int16_t k_out[K_OUT_CAP];
static uint32_t k_join[(K_IN_FRAMES / K_FEED_CHUNK) + 2];

/* Push `n` frames through the resampler, appending to k_out. Returns the new
   total. */
static uint32_t k_feed(struct audio_resample_state* st, const int16_t* in,
					   uint32_t n, uint32_t total) {
	uint32_t pos = 0;
	while(pos < n && total < K_OUT_CAP) {
		uint32_t used = 0;
		uint32_t got = audio_resample_run(st, in + pos, n - pos, &used,
										  k_out + total, K_OUT_CAP - total);
		total += got;
		pos += used;
		if(used == 0 && got == 0)
			break;
	}
	return total;
}

static void test_resample_sine_44k_to_48k(void) {
	struct audio_resample_state st;
	audio_resample_init(&st, K_SRC_RATE, K_DST_RATE);
	assert(audio_resample_active(&st));
	/* step = (44100 << 16) / 48000 = 2890137600 / 48000 = 60211 (truncated),
	   i.e. 0.918655 input samples per output sample. */
	assert(st.step == 60211u);

	for(uint32_t k = 0; k < K_IN_FRAMES; k++) {
		double ph = 2.0 * 3.14159265358979323846 * (double)K_SINE_HZ
			* (double)k / (double)K_SRC_RATE;
		k_in[k] = (int16_t)(30000.0 * sin(ph));
	}

	uint32_t total = 0;
	int njoin = 0;
	for(uint32_t off = 0; off < K_IN_FRAMES; off += K_FEED_CHUNK) {
		uint32_t n = K_IN_FRAMES - off;
		if(n > K_FEED_CHUNK)
			n = K_FEED_CHUNK;
		total = k_feed(&st, k_in + off, n, total);
		/* The NEXT output sample begins the next input chunk's contribution:
		   record the boundary so the join can be checked for a step. */
		if(off + n < K_IN_FRAMES && total > 0
		   && njoin < (int)(sizeof(k_join) / sizeof(k_join[0])))
			k_join[njoin++] = total;
	}

	/* LENGTH. One second of 44.1kHz input becomes one second at 48kHz:
	   44100 * 65536 / 60211 = 48000.4, minus the one-sample carry. */
	assert(total >= K_DST_RATE - 4u);
	assert(total <= K_DST_RATE + 4u);

	/* FREQUENCY IS PRESERVED. A 440Hz tone crosses zero going positive
	   exactly 440 times per second, whatever the sample rate; a resampler
	   that dropped or duplicated whole periods would not land here. */
	int crossings = 0;
	for(uint32_t k = 1; k < total; k++)
		if(k_out[k - 1] <= 0 && k_out[k] > 0)
			crossings++;
	assert(crossings >= (int)K_SINE_HZ - 2);
	assert(crossings <= (int)K_SINE_HZ + 2);

	/* NO DISCONTINUITY AT A CHUNK JOIN. The state carries the fractional
	   phase AND the previous chunk's last sample across the boundary, so a
	   join must be no steeper than the steepest step inside a chunk. (If
	   the phase were reset per chunk, or `prev` dropped, a join would show
	   a visible jump -- exactly the per-chunk click this design avoids.) */
	int32_t max_in_chunk = 0;
	int32_t max_join = 0;
	int ji = 0;
	for(uint32_t k = 1; k < total; k++) {
		int32_t d = (int32_t)k_out[k] - (int32_t)k_out[k - 1];
		if(d < 0)
			d = -d;
		bool is_join = false;
		while(ji < njoin && k_join[ji] < k)
			ji++;
		if(ji < njoin && k_join[ji] == k)
			is_join = true;
		if(is_join) {
			if(d > max_join)
				max_join = d;
		} else if(d > max_in_chunk) {
			max_in_chunk = d;
		}
	}
	assert(njoin > 0);
	assert(max_in_chunk > 0);
	assert(max_join <= max_in_chunk);

	puts("  K: 44100 -> 48000 linear resample (length, 440Hz preserved by "
		 "zero-crossing count, chunk joins no steeper than in-chunk steps) "
		 "OK");
}

static void test_resample_lengths_and_passthrough(void) {
	struct audio_resample_state st;

	/* 22050 -> 48000. step = (22050 << 16) / 48000 = 1445068800 / 48000
	   = 30105 (truncated from 30105.6), so 22050 input frames become
	   22050 * 65536 / 30105 = 47999.96 -> 47999 output frames plus the
	   one-sample carry. */
	audio_resample_init(&st, 22050u, 48000u);
	assert(audio_resample_active(&st));
	assert(st.step == 30105u);
	assert(audio_resample_out_frames(&st, 22050u) == 48000u);

	static int16_t ramp[22050];
	for(uint32_t k = 0; k < 22050u; k++)
		ramp[k] = (int16_t)((int)(k % 2000u) - 1000);
	uint32_t total = 0;
	for(uint32_t off = 0; off < 22050u; off += K_FEED_CHUNK) {
		uint32_t n = 22050u - off;
		if(n > K_FEED_CHUNK)
			n = K_FEED_CHUNK;
		total = k_feed(&st, ramp + off, n, total);
	}
	assert(total >= 47996u);
	assert(total <= 48002u);

	/* PASS-THROUGH. Equal rates leave step at 0 and audio_resample_active
	   false -- audio_music.c takes the plain decode path there and never
	   calls run() at all. Running it anyway must still be BIT-EXACT (with
	   the documented one-sample carry), so an accidental conversion can
	   never sneak in. */
	audio_resample_init(&st, 48000u, 48000u);
	assert(!audio_resample_active(&st));
	assert(st.step == 0u);
	assert(audio_resample_out_frames(&st, 1234u) == 1234u);

	int16_t a[8] = {10, -20, 30, -40, 50, -60, 70, -80};
	int16_t b[4] = {90, -100, 110, -120};
	int16_t got[16];
	uint32_t used = 0;
	uint32_t n = audio_resample_run(&st, a, 8u, &used, got, 16u);
	assert(used == 8u);
	assert(n == 7u); /* a[7] is carried, emitted at the head of the next call */
	for(uint32_t k = 0; k < 7u; k++)
		assert(got[k] == a[k]);
	n = audio_resample_run(&st, b, 4u, &used, got, 16u);
	assert(used == 4u);
	assert(n == 4u); /* the carry plus b[0..2]; b[3] becomes the new carry */
	assert(got[0] == a[7]);
	for(uint32_t k = 0; k < 3u; k++)
		assert(got[1 + k] == b[k]);

	/* Degenerate inputs are no-ops, not crashes. */
	n = audio_resample_run(&st, a, 0u, &used, got, 16u);
	assert(n == 0u && used == 0u);
	n = audio_resample_run(&st, a, 8u, &used, got, 0u);
	assert(n == 0u && used == 0u);

	puts("  K: 22050 -> 48000 length math + equal-rate pass-through "
		 "(bit-exact, one-sample carry, degenerate inputs are no-ops) OK");
}

/* ===== L: creative-mode playlist selection (maintainer ruling 2026-09-03,
   docs/creative_mode.md) ====================================================
   Coverage:
	 L1: audio_music_use_creative_catalog -- the PURE per-world selection
	     gate (audio_music_table.h) -- a truth table over
	     {world_is_creative, creative_catalog_count}, hand-computed, no
	     unit-under-test read-back.
	 L2: audio_music_creative_telemetry_key -- the PURE telemetry-band
	     arithmetic, hand-computed against the documented
	     AUDIO_MUSIC_KEY_COUNT+2.. band, including the out-of-range
	     defensive case.
	 L3: registry scan populates BOTH catalogs independently -- a pack with
	     only Beta files leaves the creative catalog at 0 and vice versa; a
	     pack with both populates both, and audio_music_track_count() (the
	     PRE-EXISTING accessor) is completely unaffected by the creative
	     files' presence -- proving the two catalogs never merge.
	 L4: headless end-to-end (audio.enabled=off, the test_audio_env.c /
	     section E precedent): with gstate.config_creative=true and a FULLY
	     POPULATED creative catalog, audio_music_tick() still never fires
	     (E's own proof, that `audio_ready` freezes the WHOLE block, holds
	     regardless of which catalog would have been drawn from) -- and
	     nothing crashes walking the new is_creative-aware accessors while
	     silent. */

static void test_creative_use_catalog_gate(void) {
	/* Not creative: NEVER uses the creative catalog, however many files it
	   has (a survival world must be unaffected by an imported creative
	   pack). */
	assert(!audio_music_use_creative_catalog(false, 0));
	assert(!audio_music_use_creative_catalog(false, 1));
	assert(!audio_music_use_creative_catalog(false, 6));
	/* Creative, but the pool is empty (nothing imported yet): falls back to
	   survival -- the documented "creative worlds fall back to the Beta
	   1.7.3 list" behaviour. */
	assert(!audio_music_use_creative_catalog(true, 0));
	/* Creative with >=1 registered file: uses the creative catalog, however
	   short of all six it is. */
	assert(audio_music_use_creative_catalog(true, 1));
	assert(audio_music_use_creative_catalog(true, 6));
	puts("  L1: creative-catalog selection gate (off unless the world is "
		 "creative AND the pool is non-empty) OK");
}

static void test_creative_telemetry_band(void) {
	/* AUDIO_MUSIC_KEY_COUNT is 4 (calm/hal/nuance/piano) -- band starts at
	   4+2=6 for creative key 0, one slot past the record sentinel (5). */
	assert(AUDIO_MUSIC_KEY_COUNT == 4);
	assert(audio_music_creative_telemetry_key(0) == 6u);
	assert(audio_music_creative_telemetry_key(1) == 7u);
	assert(audio_music_creative_telemetry_key(AUDIO_MUSIC_CREATIVE_KEY_COUNT - 1)
		   == (unsigned)(4 + 2 + AUDIO_MUSIC_CREATIVE_KEY_COUNT - 1));
	/* Out of range (should not happen in practice) reads as silent, the
	   same defensive shape the survival band already uses. */
	assert(audio_music_creative_telemetry_key(-1) == 0u);
	assert(audio_music_creative_telemetry_key(AUDIO_MUSIC_CREATIVE_KEY_COUNT)
		   == 0u);
	puts("  L2: creative telemetry band (AUDIO_MUSIC_KEY_COUNT+2.., one past "
		 "the record sentinel; out-of-range reads silent) OK");
}

static void test_creative_registry_scan_separate_catalogs(void) {
	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/music"));
	MKDIR(test_fixture_join(TMP_PACK, "/music/creative"));
	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));

	/* Only Beta files: the creative catalog stays at 0, survival sees them
	   normally (the test_registry_scan precedent). */
	write_garbage_file(test_fixture_join(TMP_PACK, "/music/calm1.ogg"));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 1);
	assert(audio_music_creative_track_count() == 0);

	/* Add three of the six real creative basenames (audio_music_table.h's
	   AUDIO_MUSIC_CREATIVE_TABLE): the creative catalog picks up EXACTLY
	   those three, and the survival count is COMPLETELY unaffected -- the
	   two catalogs never merge. */
	write_garbage_file(
		test_fixture_join(TMP_PACK, "/music/creative/aria_math.ogg"));
	write_garbage_file(
		test_fixture_join(TMP_PACK, "/music/creative/dreiton.ogg"));
	write_garbage_file(
		test_fixture_join(TMP_PACK, "/music/creative/taswell.ogg"));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 1); /* unchanged */
	assert(audio_music_creative_track_count() == 3);

	/* All six: full pool. */
	write_garbage_file(
		test_fixture_join(TMP_PACK, "/music/creative/biome_fest.ogg"));
	write_garbage_file(
		test_fixture_join(TMP_PACK, "/music/creative/blind_spots.ogg"));
	write_garbage_file(
		test_fixture_join(TMP_PACK, "/music/creative/haunt_muskie.ogg"));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 1);
	assert(audio_music_creative_track_count() == 6);

	/* Back to the music-less default pack: BOTH catalogs empty out (the
	   test_registry_scan precedent, extended). */
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_music_registry_reload();
	assert(audio_music_track_count() == 0);
	assert(audio_music_creative_track_count() == 0);

	remove(test_fixture_join(TMP_PACK, "/music/calm1.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/creative/aria_math.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/creative/dreiton.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/creative/taswell.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/creative/biome_fest.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/creative/blind_spots.ogg"));
	remove(test_fixture_join(TMP_PACK, "/music/creative/haunt_muskie.ogg"));
	puts("  L3: registry scan populates the Beta and creative catalogs "
		 "SEPARATELY -- neither's count is affected by the other's files "
		 "OK");
}

static void test_creative_headless_never_fires(void) {
	/* HEADLESS: no real device (the test_audio_env.c / section E
	   precedent). Proves E's own freeze holds even for a creative world
	   with a FULLY populated creative catalog -- audio_ready gates the
	   WHOLE block before the catalog selection is ever consulted, so which
	   catalog would have been picked cannot matter here. */
	audio_init(false, 100);

	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/music/creative"));
	static const char* const creative_files[] = {
		"aria_math", "biome_fest", "blind_spots",
		"dreiton", "haunt_muskie", "taswell",
	};
	for(int i = 0; i < 6; i++) {
		char rel[64];
		snprintf(rel, sizeof(rel), "/music/creative/%s.ogg", creative_files[i]);
		write_garbage_file(test_fixture_join(TMP_PACK, rel));
	}
	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));

	audio_music_init(100);
	assert(audio_music_creative_track_count() == 6);

	gstate.config_creative = true;
	for(int k = 0; k < 20000; k++)
		audio_music_tick();
	assert(!audio_music_is_playing());
	assert(audio_music_current_key() == NULL);
	assert(audio_music_telemetry_key() == 0);
	gstate.config_creative = false;

	audio_music_shutdown();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	for(int i = 0; i < 6; i++) {
		char rel[64];
		snprintf(rel, sizeof(rel), "/music/creative/%s.ogg", creative_files[i]);
		remove(test_fixture_join(TMP_PACK, rel));
	}
	puts("  L4: creative headless run never fires the timer either (the "
		 "audio_ready freeze holds regardless of which catalog would have "
		 "been drawn from) OK");
}

static void test_music_drained_gate_headless(void) {
	/* The engine half of the gate. A headless backend (audio_init(false,
	   ...), the test_audio_env.c precedent) has no device and therefore no
	   queue, so it cannot express "not drained" -- audio.c answers true and
	   audio_music_update's EOF branch behaves exactly as it did before the
	   gate existed, which is what sections E/I keep proving end-to-end.
	   The "must NOT stop before drained" half is proven on the pure header
	   above (test_stream_ring_underrun_and_drain (8) and (10)), since there
	   is no headless backend that can hold a tail. */
	audio_init(false, 100);
	assert(!audio_voice_music_playing());
	assert(audio_voice_music_drained());
	/* Idempotent, and a stop while nothing plays keeps it true. */
	audio_voice_music_stop();
	assert(audio_voice_music_drained());
	puts("  J: headless drained gate (no live device => drained, so the EOF "
		 "branch is unchanged headless) OK");
}

int main(void) {
	test_fixture_dir("test_audio_music_env_pack_tmp", s_fixture_TMP_PACK, sizeof s_fixture_TMP_PACK);
	test_timer_initial_delay();
	test_timer_tick();
	test_ring_fill_target();
	test_ring_advance();
	test_ring_ready_run();
	test_record_transition();
	test_force_lever();
	test_pick_lever_decode();
	test_record_file_stem();
	test_stream_ring_prime_and_guard();
	test_stream_ring_underrun_and_drain();

	test_creative_use_catalog_gate();
	test_creative_telemetry_band();

	memset(&gstate, 0, sizeof(gstate));
	test_registry_scan();
	test_registry_scan_real_beta_layout();
	test_config_round_trip();
	test_audio_disabled_never_fires();
	test_record_playback_headless();
	test_music_drained_gate_headless();
	test_resample_sine_44k_to_48k();
	test_resample_lengths_and_passthrough();
	test_creative_registry_scan_separate_catalogs();
	test_creative_headless_never_fires();

	config_destroy(&gstate.config_user);
	puts("test_audio_music_env: PASS 20/20");
	test_fixture_dir_cleanup(TMP_PACK);
	return 0;
}
