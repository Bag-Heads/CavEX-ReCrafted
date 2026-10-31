/* Pack Helper audio/texture separation: transaction and real registry proof. */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#include "support/test_fixture_dir.h"

#include "config.h"
#include "game/game_state.h"
#include "parson/parson.h"
#include "platform/audio.h"
#include "platform/audio_music.h"
#include "texturepacks.h"

/* Test-only seam, compiled into audio_music.c under CAVEWII_TESTING and
 * deliberately absent from audio_music.h and from every shipping binary
 * (the displaylist_test_fail_* precedent). */
extern const char* audio_music_test_effective_pack(void);
/* Same shape in screen_texturepacks.c: aim the seam's two external
 * dependencies at the test's own, then run the REAL texpacks_apply_now. */
extern void screen_texturepacks_test_arm(const char* config_path,
										 texturepack_reload_fn reload);
extern bool screen_texturepacks_test_apply(const char* pack_value, bool ingame);
/* The EXPLICIT audio choice, the same seam for the other category. */
extern bool screen_texturepacks_test_apply_audio(const char* pack_value,
												 bool ingame);
extern enum texturepacks_result screen_texturepacks_test_last_result(void);
/* Arena re-decodes: "no reload happened" is only observable as a count. */
extern unsigned audio_registry_test_reload_count(void);
extern void audio_registry_test_reload_count_reset(void);
/* Observer-telemetry sinks (brief 08). A non-NULL sink both ARMS the events
 * (standing in for hw_telemetry_active()) and captures the exact line, so the
 * JSON the console will emit is asserted here without a telemetry backend.
 * Three sinks because the three producers share no header. */
extern void audio_registry_test_set_observer(bool (*sink)(const char* line));
extern void audio_music_test_set_observer(bool (*sink)(const char* line));
extern void screen_texturepacks_test_set_observer(bool (*sink)(const char* line));

#include "platform/watchdog.h" /* the seam's declared-stall bracket */

static struct config* s_reload_config;
static const char* s_rejected_pack;
static int s_reload_calls;
static int s_reload_phase_seen;
/* Which pack each reload() actually saw. M6: after a rejected switch the
 * loader must re-run from the PREVIOUS pack, never leave the rejected one
 * live -- a claim only observable per call. */
#define RELOAD_OBS_MAX 8
static char s_reload_saw[RELOAD_OBS_MAX][384];

static bool texture_reload(void) {
	const char* current = config_read_string(
		s_reload_config, "paths.texturepack", "missing");
	if(s_reload_calls < RELOAD_OBS_MAX)
		snprintf(s_reload_saw[s_reload_calls], sizeof(s_reload_saw[0]), "%s",
				 current);
	s_reload_calls++;
	/* The loader runs INSIDE the seam's declared multi-second stall window;
	 * record the phase the watchdog is actually in while it does. */
	if(watchdog_phase_main_current() == (uint32_t)WDM_PACK_RELOAD)
		s_reload_phase_seen++;
	return !s_rejected_pack || strcmp(current, s_rejected_pack) != 0;
}

/* Drive the REAL screen seam (texpacks_apply_now), not a copy of it: the
 * production texpacks_value_eq normalisation, the !ingame_mode gate, the
 * watchdog brackets, the music-first reload order and the lost-music rollback
 * all execute here (2026-09-17 DELTA review, test gap (a)). */
static bool apply_via_screen_seam(const char* pack, bool ingame) {
	return screen_texturepacks_test_apply(pack, ingame);
}

/* The same real seam, Sounds category: texturepacks_apply_audio driving the
 * screen's own music-first / reject-empty-music / decode-only-if-moved
 * callback. */
static bool apply_audio_via_screen_seam(const char* pack, bool ingame) {
	return screen_texturepacks_test_apply_audio(pack, ingame);
}

static void write_config(const char* path, const char* texture,
						 const char* audio) {
	JSON_Value* root = json_value_init_object();
	assert(root);
	JSON_Object* object = json_object(root);
	if(texture)
		assert(json_object_dotset_string(object, "paths.texturepack", texture)
			   == JSONSuccess);
	if(audio)
		assert(json_object_dotset_string(object, "paths.audiopack", audio)
			   == JSONSuccess);
	assert(json_serialize_to_file(root, path) == JSONSuccess);
	json_value_free(root);
}

static size_t wav_build(uint8_t* buf, uint32_t rate) {
	static const int16_t samples[] = {101, -202, 303};
	uint32_t frames = (uint32_t)(sizeof(samples) / sizeof(samples[0]));
	uint32_t data_bytes = frames * 2;
	uint32_t riff = 36 + data_bytes;
	size_t o = 0;
	memcpy(buf + o, "RIFF", 4), o += 4;
	for(int i = 0; i < 4; i++) buf[o++] = (uint8_t)(riff >> (8 * i));
	memcpy(buf + o, "WAVEfmt ", 8), o += 8;
	buf[o++] = 16, buf[o++] = 0, buf[o++] = 0, buf[o++] = 0;
	buf[o++] = 1, buf[o++] = 0; /* PCM */
	buf[o++] = 1, buf[o++] = 0; /* mono */
	for(int i = 0; i < 4; i++) buf[o++] = (uint8_t)(rate >> (8 * i));
	uint32_t byte_rate = rate * 2;
	for(int i = 0; i < 4; i++) buf[o++] = (uint8_t)(byte_rate >> (8 * i));
	buf[o++] = 2, buf[o++] = 0;
	buf[o++] = 16, buf[o++] = 0;
	memcpy(buf + o, "data", 4), o += 4;
	for(int i = 0; i < 4; i++) buf[o++] = (uint8_t)(data_bytes >> (8 * i));
	for(uint32_t i = 0; i < frames; i++) {
		uint16_t v = (uint16_t)samples[i];
		buf[o++] = (uint8_t)v;
		buf[o++] = (uint8_t)(v >> 8);
	}
	return o;
}

static void write_bytes(const char* path, const void* data, size_t size) {
	FILE* file = fopen(path, "wb");
	assert(file);
	assert(fwrite(data, 1, size, file) == size);
	assert(fclose(file) == 0);
}

static void make_audio_pack(const char* root, uint32_t rate, int tracks) {
	assert(MKDIR(root) == 0);
	assert(MKDIR(test_fixture_join(root, "/sound")) == 0);
	assert(MKDIR(test_fixture_join(root, "/sound/step")) == 0);
	assert(MKDIR(test_fixture_join(root, "/music")) == 0);
	assert(MKDIR(test_fixture_join(root, "/records")) == 0);
	uint8_t wav[64];
	write_bytes(test_fixture_join(root, "/sound/step/grass1.wav"), wav,
				wav_build(wav, rate));
	static const uint8_t marker[] = {0, 1, 2, 3};
	write_bytes(test_fixture_join(root, "/music/calm1.ogg"), marker,
				sizeof(marker));
	if(tracks == 2)
		write_bytes(test_fixture_join(root, "/music/calm2.ogg"), marker,
					sizeof(marker));
	write_bytes(test_fixture_join(root, "/records/13.ogg"), marker,
				sizeof(marker));
}

static bool has_audio_key(struct config* config) {
	return json_object_dothas_value(json_object(config->root),
									"paths.audiopack");
}

static void assert_disk_selection(const char* path, const char* texture,
								  const char* audio) {
	struct config disk;
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "paths.texturepack", "missing"),
				  texture)
		   == 0);
	assert(has_audio_key(&disk) == (audio != NULL));
	if(audio)
		assert(strcmp(config_read_string(&disk, "paths.audiopack", "missing"),
					  audio)
			   == 0);
	config_destroy(&disk);
}

static void test_transaction_rollbacks(const char* fixture,
								   const char* legacy,
								   const char* texture,
								   const char* explicit_audio) {
	char reload_path[384], save_path[384], absent_save_path[384];
	char absent_both_path[384], wrong_type_path[384], wrong_type_ok_path[384];
	snprintf(reload_path, sizeof(reload_path), "%s/reload.json", fixture);
	snprintf(save_path, sizeof(save_path), "%s/save.json", fixture);
	snprintf(absent_save_path, sizeof(absent_save_path), "%s/absent-save.json",
			 fixture);
	snprintf(absent_both_path, sizeof(absent_both_path), "%s/absent-both.json",
			 fixture);
	snprintf(wrong_type_path, sizeof(wrong_type_path), "%s/wrong-type.json",
			 fixture);
	snprintf(wrong_type_ok_path, sizeof(wrong_type_ok_path),
			 "%s/wrong-type-ok.json", fixture);

	/* Reload rejection restores the old texture and exact key absence both
	 * in memory and in the persisted config. */
	write_config(reload_path, legacy, NULL);
	struct config config;
	assert(config_create(&config, reload_path));
	s_reload_config = &config;
	s_rejected_pack = texture;
	s_reload_calls = 0;
	assert(!texturepacks_apply(&config, texture, reload_path, texture_reload));
	assert(s_reload_calls == 2);
	/* M6: call 1 saw the candidate (its textures went live), call 2 saw the
	 * PREVIOUS pack -- the rejected pack is not what stayed loaded. */
	assert(strcmp(s_reload_saw[0], texture) == 0);
	assert(strcmp(s_reload_saw[1], legacy) == 0);
	assert(!has_audio_key(&config));
	assert(strcmp(config_read_string(&config, "paths.texturepack", "missing"),
				  legacy)
		   == 0);
	assert_disk_selection(reload_path, legacy, NULL);
	config_destroy(&config);

	/* A config with neither key must remain byte-semantically absent after a
	 * rejected change; fallback values must not materialize on rollback. */
	write_config(absent_both_path, NULL, NULL);
	assert(config_create(&config, absent_both_path));
	s_reload_config = &config;
	s_rejected_pack = texture;
	s_reload_calls = 0;
	assert(!texturepacks_apply(&config, texture, absent_both_path,
							 texture_reload));
	assert(s_reload_calls == 2);
	/* M6 again, with nothing to restore TO: the re-run still must not be the
	 * rejected pack (the key is gone, so the loader falls back). */
	assert(strcmp(s_reload_saw[0], texture) == 0);
	assert(strcmp(s_reload_saw[1], texture) != 0);
	JSON_Object* object = json_object(config.root);
	assert(!json_object_dothas_value(object, "paths.texturepack"));
	assert(!json_object_dothas_value(object, "paths.audiopack"));
	config_destroy(&config);
	assert(config_create(&config, absent_both_path));
	object = json_object(config.root);
	assert(!json_object_dothas_value(object, "paths.texturepack"));
	assert(!json_object_dothas_value(object, "paths.audiopack"));
	config_destroy(&config);

	/* A wrong-typed audio key behaves as legacy fallback during the attempt,
	 * but rejection restores the original JSON number exactly. */
	write_config(wrong_type_path, legacy, NULL);
	assert(config_create(&config, wrong_type_path));
	object = json_object(config.root);
	assert(json_object_dotset_number(object, "paths.audiopack", 42)
		   == JSONSuccess);
	assert(config_save(&config, wrong_type_path));
	s_reload_config = &config;
	s_rejected_pack = texture;
	s_reload_calls = 0;
	assert(!texturepacks_apply(&config, texture, wrong_type_path,
							 texture_reload));
	assert(s_reload_calls == 2);
	assert(json_object_dothas_value_of_type(object, "paths.audiopack",
										JSONNumber));
	assert(json_object_dotget_number(object, "paths.audiopack") == 42);
	config_destroy(&config);
	assert(config_create(&config, wrong_type_path));
	object = json_object(config.root);
	assert(json_object_dothas_value_of_type(object, "paths.audiopack",
										JSONNumber));
	assert(json_object_dotget_number(object, "paths.audiopack") == 42);
	config_destroy(&config);

	/* Success replaces a wrong-typed key with the old effective fallback,
	 * preventing the texture switch from changing audio on the next launch. */
	write_config(wrong_type_ok_path, legacy, NULL);
	assert(config_create(&config, wrong_type_ok_path));
	object = json_object(config.root);
	assert(json_object_dotset_boolean(object, "paths.audiopack", 1)
		   == JSONSuccess);
	assert(config_save(&config, wrong_type_ok_path));
	s_reload_config = &config;
	s_rejected_pack = NULL;
	s_reload_calls = 0;
	assert(texturepacks_apply(&config, texture, wrong_type_ok_path,
							texture_reload));
	assert(s_reload_calls == 1);
	assert(strcmp(config_read_string(&config, "paths.audiopack", "missing"),
				  legacy)
		   == 0);
	config_destroy(&config);
	assert_disk_selection(wrong_type_ok_path, texture, legacy);

	/* A save failure after a successful reload restores an existing explicit
	 * audio value without replacing it with the texture fallback. */
	write_config(save_path, legacy, explicit_audio);
	assert(config_create(&config, save_path));
	s_reload_config = &config;
	s_rejected_pack = NULL;
	s_reload_calls = 0;
	char missing_parent[512];
	snprintf(missing_parent, sizeof(missing_parent),
			 "%s/missing/config.json", fixture);
	assert(!texturepacks_apply(&config, texture, missing_parent,
							 texture_reload));
	assert(s_reload_calls == 2);
	assert(strcmp(config_read_string(&config, "paths.texturepack", "missing"),
				  legacy)
		   == 0);
	assert(strcmp(config_read_string(&config, "paths.audiopack", "missing"),
				  explicit_audio)
		   == 0);
	assert_disk_selection(save_path, legacy, explicit_audio);
	config_destroy(&config);

	/* The same save-failure path also removes a provisional legacy pin. */
	write_config(absent_save_path, legacy, NULL);
	assert(config_create(&config, absent_save_path));
	s_reload_config = &config;
	s_reload_calls = 0;
	assert(!texturepacks_apply(&config, texture, missing_parent,
							 texture_reload));
	assert(s_reload_calls == 2);
	assert(!has_audio_key(&config));
	assert_disk_selection(absent_save_path, legacy, NULL);
	config_destroy(&config);
}

/* `path` outlives every stage below: screen_texturepacks_test_arm keeps the
 * pointer, so it must not be a local of this function. */
static void test_runtime_selection(const char* path, const char* legacy,
								   const char* texture,
								   const char* explicit_audio,
								   const char* loud) {
	write_config(path, legacy, NULL);
	assert(config_create(&gstate.config_user, path));

	/* Legacy boot: absent paths.audiopack resolves effects, music, and records
	 * through paths.texturepack. This exercises the real decoders/scanners. */
	audio_init(false, 100);
	uint32_t frames = 0, rate = 0;
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 11025);
	audio_music_init(100);
	assert(audio_music_track_count() == 1);
	assert(strcmp(audio_music_test_effective_pack(), legacy) == 0);

	/* First successful texture selection materializes the old effective audio
	 * root. It persists, while the already-loaded effects/music stay intact. */
	s_reload_config = &gstate.config_user;
	s_rejected_pack = NULL;
	s_reload_calls = 0;
	assert(texturepacks_apply(&gstate.config_user, texture, path,
							texture_reload));
	assert(s_reload_calls == 1);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  legacy)
		   == 0);
	assert_disk_selection(path, texture, legacy);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 11025);
	assert(audio_music_track_count() == 1);
	assert(strcmp(audio_music_test_effective_pack(), legacy) == 0);

	/* Pack Helper writes the independent audio choice. The same load routines
	 * used at boot now select effects/music/records from that root. */
	assert(config_set_string(&gstate.config_user, "paths.audiopack",
							 explicit_audio));
	assert(config_save(&gstate.config_user, path));
	audio_registry_reload();
	audio_music_registry_reload();
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 8000);
	assert(audio_music_track_count() == 2);
	assert(strcmp(audio_music_test_effective_pack(), explicit_audio) == 0);

	/* Records keep the stable effective boot/reload root even if texture state
	 * changes later; no borrowed config pointer is retained. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack", legacy));
	audio_music_play_record_for_item(2256);
	assert(audio_music_is_playing_record());
	assert(strcmp(audio_music_test_effective_pack(), explicit_audio) == 0);
	audio_music_stop_record();

	/* ---- H1 (2026-09-17 review): the registries follow the selected pack.
	 * Every apply below runs the REAL screen seam (texpacks_apply_now) with
	 * the texture loader and config path aimed at this test -- the production
	 * texpacks_value_eq normalisation, the !ingame_mode gate, the reload order
	 * and the lost-music rollback all execute. Asserted each time: which
	 * DIRECTORY each registry resolved (the effects sample rate and the music
	 * track count are the per-pack fingerprints) and what the selection says
	 * in memory and on disk. */
	uint32_t seam_rate = 0;
	screen_texturepacks_test_arm(path, texture_reload);
	s_reload_config = &gstate.config_user;

	/* A (Astra gate 1 items 2-3): the audio root is an EXPLICIT choice, made
	 * on its own. There is no explicit AUDIO apply seam yet -- when there is,
	 * these four lines become a call to it (RED-RETURN "later audio cases") --
	 * so the choice is written the way tools/pack_helper writes it and both
	 * registries are rescanned through their real entry points. */
	assert(config_set_string(&gstate.config_user, "paths.audiopack", loud));
	assert(config_save(&gstate.config_user, path));
	audio_music_registry_reload();
	audio_registry_reload();
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &seam_rate));
	assert(frames == 3 && seam_rate == 22050);

	/* Applying that same pack as the TEXTURE choice moves nothing: the root is
	 * already where it is. What this leg still proves is the seam's declared
	 * stall bracket -- the texture loader runs inside WDM_PACK_RELOAD and the
	 * phase is handed back to the screen afterwards. */
	s_rejected_pack = NULL;
	s_reload_calls = 0;
	s_reload_phase_seen = 0;
	assert(apply_via_screen_seam(loud, false));
	assert(s_reload_calls == 1);
	/* The loader ran inside the seam's declared multi-second stall window. */
	assert(s_reload_phase_seen == 1);
	assert(watchdog_phase_main_current() == (uint32_t)WDM_SCREEN_UPDATE);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0);
	assert_disk_selection(path, loud, loud);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &seam_rate));
	assert(frames == 3 && seam_rate == 22050);
	assert(audio_music_track_count() == 2);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);

	/* B: a texture-only pack keeps the audio the player already has. The
	 * selection does not move, so nothing is drained or rescanned. */
	s_reload_calls = 0;
	assert(apply_via_screen_seam(texture, false));
	assert(s_reload_calls == 1);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0);
	assert_disk_selection(path, texture, loud);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &seam_rate));
	assert(frames == 3 && seam_rate == 22050);
	assert(audio_music_track_count() == 2);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);

	/* A REJECTED pack -- one that carries audio, so it would otherwise have
	 * moved the selection -- changes neither choice, and the loader re-runs
	 * from the previous pack (M6) instead of leaving the rejected one live. */
	s_rejected_pack = explicit_audio;
	s_reload_calls = 0;
	assert(!apply_via_screen_seam(explicit_audio, false));
	assert(s_reload_calls == 2);
	assert(strcmp(s_reload_saw[0], explicit_audio) == 0);
	assert(strcmp(s_reload_saw[1], texture) == 0);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									 "missing"),
				  texture)
		   == 0);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0);
	assert_disk_selection(path, texture, loud);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &seam_rate));
	assert(frames == 3 && seam_rate == 22050);
	assert(audio_music_track_count() == 2);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	s_rejected_pack = NULL;
}

/* ===== THE CONTRACT (Astra gate 1, items 2 and 7) ========================
 * An explicit TEXTURE choice changes paths.texturepack and NOTHING else. A
 * pack that happens to ship sound/, music/ and records/ does not get to take
 * the audio root the player chose separately -- "never steal audio because a
 * texture directory ships sound files" (gate 1 item 2). Driven through the
 * REAL native apply seam (screen_texturepacks_test_apply -> texpacks_apply_now
 * -> texturepacks_apply), so the production normalisation, the !ingame gate
 * and the reload order all execute.
 *
 * Asserted on both sides of the line: the SELECTION (memory and disk) and the
 * REAL loaded registries -- the effects sample rate and the music track count
 * are the per-pack fingerprints, and audio_registry_test_reload_count() is the
 * only way "no reload happened" is observable at all.
 *
 * RED today: the seam piggybacks the audio root onto the texture apply
 * (texturepacks.c:220-226 moves paths.audiopack when the new pack carries
 * audio, and screen_texturepacks.c:497-539 then rescans both registries), so
 * the audiopack assertion below is the first to fire. */
static void test_texture_apply_never_takes_audio(const char* path,
												 const char* texture,
												 const char* loud,
												 const char* explicit_audio) {
	uint32_t frames = 0, rate = 0;
	/* Precondition: an explicit audio root, live, with its own fingerprints. */
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050);

	s_reload_config = &gstate.config_user;
	s_rejected_pack = NULL;
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	/* `explicit_audio` ships sound/ at 8000 Hz, music/ with two resolvable
	 * tracks and records/ -- everything that makes it a legitimate AUDIO pack.
	 * Chosen here as a TEXTURE pack, and only as one. */
	assert(apply_via_screen_seam(explicit_audio, false));
	assert(s_reload_calls == 1); /* the textures really did reload */
	assert(strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									 "missing"),
				  explicit_audio)
		   == 0);
	/* THE CLAIM -- the audio selection did not move, in memory or on disk. */
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0
		   && "an explicit texture apply moved the audio selection");
	assert_disk_selection(path, explicit_audio, loud);
	/* ...and nothing was drained, rescanned or re-decoded on the way: the
	 * arena still holds `loud`'s effects and the music registry `loud`'s
	 * tracks. A reload that ended where it started is still ~25 s on the
	 * console (test_no_sound_reload_when_the_root_did_not_move), so the count
	 * is asserted, not just the outcome. */
	assert(audio_registry_test_reload_count() == 0
		   && "an explicit texture apply re-decoded the sound arena");
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050);

	/* Re-applying the SAME texture pack is still a no-op on both selections. */
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	assert(apply_via_screen_seam(explicit_audio, false));
	assert(audio_registry_test_reload_count() == 0);
	assert_disk_selection(path, explicit_audio, loud);

	/* Leave the texture selection where the stages below expect it. */
	s_reload_calls = 0;
	assert(apply_via_screen_seam(texture, false));
	assert_disk_selection(path, texture, loud);
}

/* ===== the OTHER half of the contract: an EXPLICIT audio choice ==========
 * paths.audiopack only, through the real seam. Every leg asserts the
 * SELECTION (memory + disk) and the REAL registries (effects sample rate and
 * music track count are the per-pack fingerprints), plus
 * audio_registry_test_reload_count(), which is the only way "no decode was
 * paid for" is observable.
 *
 * Leaves textures on `texture` and audio on `loud`, which is what the stages
 * after it expect. */
static void test_explicit_audio_choice(const char* fixture, const char* path,
									   const char* texture, const char* loud,
									   const char* explicit_audio,
									   const char* mislabelled,
									   const char* sounds_only) {
	uint32_t frames = 0, rate = 0;
	char missing_parent[512];
	snprintf(missing_parent, sizeof(missing_parent), "%s/missing/config.json",
			 fixture);
	assert_disk_selection(path, texture, loud);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);

	/* (1) It moves the audio root and NOTHING else -- the texture loader is
	 * not even called, and paths.texturepack does not move. */
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	assert(apply_audio_via_screen_seam(explicit_audio, false));
	assert(s_reload_calls == 0 && "an audio apply reloaded the textures");
	assert_disk_selection(path, texture, explicit_audio);
	assert(audio_registry_test_reload_count() == 1);
	assert(strcmp(audio_music_test_effective_pack(), explicit_audio) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 8000);

	/* (2) Repeating it is NO_CHANGE: no decode, no rescan, no write. */
	audio_registry_test_reload_count_reset();
	assert(apply_audio_via_screen_seam(explicit_audio, false));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_NO_CHANGE);
	assert(audio_registry_test_reload_count() == 0);

	/* (3) A TEXTURE swap afterwards leaves it exactly where it is. */
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	assert(apply_via_screen_seam(loud, false));
	assert(s_reload_calls == 1);
	assert(audio_registry_test_reload_count() == 0);
	assert_disk_selection(path, loud, explicit_audio);
	assert(strcmp(audio_music_test_effective_pack(), explicit_audio) == 0);

	/* (4) A pack that CLAIMS music and resolves no playable track is REJECTED
	 * -- not "accepted, then quietly given a different root". The previous
	 * root stays live and stays persisted, and the arena is never touched:
	 * music is checked first precisely so the expensive half is never paid on
	 * a root that is about to be refused. */
	audio_registry_test_reload_count_reset();
	assert(!apply_audio_via_screen_seam(mislabelled, false));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_RELOAD_FAILED);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  explicit_audio)
		   == 0
		   && "a rejected audio root was left in the config");
	assert_disk_selection(path, loud, explicit_audio);
	assert(audio_registry_test_reload_count() == 0
		   && "a rejected root re-decoded the arena anyway");
	assert(strcmp(audio_music_test_effective_pack(), explicit_audio) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 8000);

	/* (5) A pack that claims NO music is a legitimate sound-only choice: it is
	 * accepted, and it really does leave the player with no music. That is the
	 * shape of the bundled default too. */
	audio_registry_test_reload_count_reset();
	assert(apply_audio_via_screen_seam(sounds_only, false));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_OK);
	assert_disk_selection(path, loud, sounds_only);
	assert(audio_music_track_count() == 0);
	assert(audio_registry_test_reload_count() == 1);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 32000);

	/* (6) A SAVE failure after an accepted reload: the audio key goes back
	 * exactly, nothing is persisted, and the previous root is live again --
	 * which costs one decode out and one decode back. */
	screen_texturepacks_test_arm(missing_parent, texture_reload);
	audio_registry_test_reload_count_reset();
	assert(!apply_audio_via_screen_seam(explicit_audio, false));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_SAVE_FAILED);
	screen_texturepacks_test_arm(path, texture_reload);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  sounds_only)
		   == 0);
	assert_disk_selection(path, loud, sounds_only);
	assert(audio_registry_test_reload_count() == 2);
	assert(audio_music_track_count() == 0);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 32000);

	/* (7) IN-GAME an audio choice is refused outright: not applied, not
	 * recorded for next boot, not reloaded. */
	audio_registry_test_reload_count_reset();
	assert(!apply_audio_via_screen_seam(explicit_audio, true));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_INVALID);
	assert(audio_registry_test_reload_count() == 0);
	assert_disk_selection(path, loud, sounds_only);
	assert(audio_music_track_count() == 0);

	/* Restore what the stages below expect: textures `texture`, audio `loud`. */
	audio_registry_test_reload_count_reset();
	assert(apply_audio_via_screen_seam(loud, false));
	s_reload_calls = 0;
	assert(apply_via_screen_seam(texture, false));
	assert_disk_selection(path, texture, loud);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050);
}

/* ===== rollback vs. validation, and creative-only music ==================
 * REVIEW-SCREEN-1. Two claims the earlier legs could not make:
 *
 * (HIGH) The transaction runs the reload callback twice on a failure -- once
 * for the pick, once after restoring the previous JSON -- and those two calls
 * have OPPOSITE jobs. A root that boot accepted can be one a NEW pick would be
 * refused (effects present, music/ present but resolving nothing). If the
 * restore re-run applies the new-pick check to it, it refuses, and the failed
 * apply leaves the NEW effects live on top of the OLD config.
 *
 * (MEDIUM) "Playable tracks" is not only the survival list: the creative
 * playlist is registered separately, so a creative-only pack was counted as
 * zero and refused. */
static void test_rollback_restores_and_creative_counts(
	const char* fixture, const char* path, const char* texture,
	const char* loud, const char* explicit_audio, const char* stubborn,
	const char* creative_only) {
	uint32_t frames = 0, rate = 0;
	char missing_parent[512];
	snprintf(missing_parent, sizeof(missing_parent), "%s/missing/config.json",
			 fixture);

	/* The boot state: the live root ships effects AND a music/ directory that
	 * resolves no known track. Selected the way a config on disk selects it,
	 * not through the screen -- this root would NOT pass the new-pick check,
	 * which is exactly the situation. */
	assert(config_set_string(&gstate.config_user, "paths.audiopack", stubborn));
	assert(config_save(&gstate.config_user, path));
	audio_music_registry_reload();
	audio_registry_reload();
	assert(audio_music_track_count() == 0);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 44100);

	/* A valid new root whose SAVE fails. The rollback must put the OLD effects
	 * back even though that old root would be refused as a new pick. */
	screen_texturepacks_test_arm(missing_parent, texture_reload);
	audio_registry_test_reload_count_reset();
	assert(!apply_audio_via_screen_seam(explicit_audio, false));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_SAVE_FAILED);
	screen_texturepacks_test_arm(path, texture_reload);
	assert(audio_registry_test_reload_count() == 2); /* decoded out, and back */
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  stubborn)
		   == 0);
	assert_disk_selection(path, texture, stubborn);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 44100
		   && "the failed apply left the NEW effects live");

	/* ...and the new-pick check is NOT weakened by that: picking the same root
	 * deliberately is still refused. (From a different root, or the library
	 * would short-circuit it as NO_CHANGE.) */
	assert(apply_audio_via_screen_seam(loud, false));
	assert(!apply_audio_via_screen_seam(stubborn, false));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_RELOAD_FAILED);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0);

	/* A CREATIVE-ONLY pack registers no survival track and six-catalog ones.
	 * That is real music, and the choice must stand. */
	audio_registry_test_reload_count_reset();
	assert(apply_audio_via_screen_seam(creative_only, false));
	assert(screen_texturepacks_test_last_result() == TEXTUREPACKS_OK
		   && "a creative-only music pack was refused as having no music");
	assert(audio_music_track_count() == 0);
	assert(audio_music_creative_track_count() >= 1);
	assert_disk_selection(path, texture, creative_only);
	/* None of this moved the texture choice. */
	assert(strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									 "missing"),
				  texture)
		   == 0);

	/* Restore what the stages below expect. */
	assert(apply_audio_via_screen_seam(loud, false));
	assert_disk_selection(path, texture, loud);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050);
}

/* 2026-09-17 DELTA review, HIGH: a pack whose claimed audio does not actually
 * load must NOT take the audio selection, because music has no bundled
 * fallback to land on -- the player would simply go silent behind a success
 * message. Two shapes, caught by two different guards.
 *
 * KEPT, BUT IT NO LONGER PROVES ITS CLAIM (this slice): once a texture apply
 * cannot move the audio root at all, "the hollow pack did not steal the
 * selection" is true for the wrong reason. The two guards themselves
 * (texturepacks_carries_audio's claimed-but-empty probe, and the post-scan
 * zero-track check) still matter -- they are what an EXPLICIT audio apply has
 * to run. Re-point both legs at that seam when it lands; do not delete them
 * (RED-RETURN "later audio cases", 1). */
static void test_empty_audio_never_steals_selection(const char* path,
													const char* texture,
													const char* loud,
													const char* hollow,
													const char* mislabelled) {
	uint32_t frames = 0, rate = 0;
	/* Precondition: a working audio pack with music is selected and live. */
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);

	/* (1) `hollow` ships loadable sounds but an EMPTY music/ directory. The
	 * transaction's probe sees a claimed category that delivers nothing, so
	 * the audio selection never moves at all -- not even provisionally. */
	s_reload_calls = 0;
	assert(apply_via_screen_seam(hollow, false));
	assert(s_reload_calls == 1); /* textures still applied */
	assert(strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									 "missing"),
				  hollow)
		   == 0);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0);
	assert_disk_selection(path, hollow, loud);
	assert(audio_music_track_count() == 2);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050); /* still `loud`'s effects */

	/* (2) `mislabelled` ships a real .ogg under music/, so the file probe is
	 * satisfied -- but its name matches no music key, so the SCAN resolves
	 * zero tracks. Only the post-scan check can catch this one: the seam puts
	 * the selection back and rescans before anything else reads it. */
	s_reload_calls = 0;
	assert(apply_via_screen_seam(mislabelled, false));
	assert(s_reload_calls == 1);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									 "missing"),
				  mislabelled)
		   == 0);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0);
	assert_disk_selection(path, mislabelled, loud);
	assert(audio_music_track_count() == 2);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);

	/* Put the texture selection back for the next stage. */
	s_reload_calls = 0;
	assert(apply_via_screen_seam(texture, false));
	assert_disk_selection(path, texture, loud);
}

/* Real-Wii A/B, 2026-09-17: applying the DEFAULT pack cost ~25 s twice per
 * walk. "assets" ships sound/ but no music/, so it takes the audio selection,
 * the music rescan finds nothing, the net puts the selection back -- and the
 * sound arena was re-decoded from SD anyway, against the root it already held.
 * The reload must be paid for only when the root really moved. */
static void test_no_sound_reload_when_the_root_did_not_move(
	const char* path, const char* texture, const char* loud,
	const char* sounds_only) {
	uint32_t frames = 0, rate = 0;
	/* Precondition: the live audio root has music (the Beta-pack case). */
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);

	/* The regression leg: a pack with sound/ and NO music/. It DOES carry
	 * loadable audio, so the transaction moves the selection and the music
	 * rescan runs -- but the net then restores the previous root, leaving the
	 * final selection exactly where it started, so nothing may be re-decoded. */
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	assert(apply_via_screen_seam(sounds_only, false));
	assert(s_reload_calls == 1); /* textures applied */
	assert(audio_registry_test_reload_count() == 0); /* no arena re-decode */
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0);
	assert_disk_selection(path, sounds_only, loud);
	assert(audio_music_track_count() == 2);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050); /* still `loud`'s effects */

	/* Positive control: a root that really moves still costs exactly one.
	 * It is driven by an EXPLICIT audio selection, not by a texture apply --
	 * a texture apply can no longer move the root, so it can no longer be the
	 * control here. When the explicit audio seam lands this becomes a call to
	 * it (RED-RETURN "later audio cases", 2); the claim under test -- one
	 * arena re-decode per real root move, none otherwise -- is unchanged. */
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	assert(apply_via_screen_seam(texture, false)); /* park on a texture pack */
	assert(audio_registry_test_reload_count() == 0);
	assert(config_set_string(&gstate.config_user, "paths.audiopack", texture));
	assert(config_save(&gstate.config_user, path));
	audio_music_registry_reload();
	audio_registry_test_reload_count_reset();
	assert(config_set_string(&gstate.config_user, "paths.audiopack", loud));
	assert(config_save(&gstate.config_user, path));
	audio_music_registry_reload();
	audio_registry_reload();
	assert(audio_registry_test_reload_count() == 1);
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050);

	/* Leave the texture selection where the next stage expects it. */
	s_reload_calls = 0;
	assert(apply_via_screen_seam(texture, false));
	assert_disk_selection(path, texture, loud);
}

/* Astra ui9: inside a running world the packs screen is textures-only. The
 * choice is recorded for the next launch; the live sets are NOT swapped. */
static void test_ingame_apply_does_not_reload_audio(const char* path,
													const char* texture,
													const char* loud,
													const char* explicit_audio) {
	uint32_t frames = 0, rate = 0;
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);

	s_reload_calls = 0;
	assert(apply_via_screen_seam(explicit_audio, true));
	assert(s_reload_calls == 1); /* textures did reload */
	/* The config records the new TEXTURE choice, and only that one. Gate 1
	 * item 6: an in-game texture choice never touches the audio selection --
	 * not even as a next-boot value, which would be a selection the player
	 * never made and cannot see (the screen's own note says sounds and music
	 * stay unchanged). */
	assert(strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									 "missing"),
				  explicit_audio)
		   == 0);
	assert(strcmp(config_read_string(&gstate.config_user, "paths.audiopack",
									 "missing"),
				  loud)
		   == 0
		   && "an in-game texture apply wrote a next-boot audio selection");
	assert_disk_selection(path, explicit_audio, loud);
	/* ...and NOTHING about the live audio moved: still `loud`'s music and
	 * `loud`'s 22050 Hz effects, not explicit_audio's 8000 Hz. */
	assert(strcmp(audio_music_test_effective_pack(), loud) == 0);
	assert(audio_music_track_count() == 2);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 22050);

	/* Restore a coherent selection for the load-revalidation stage. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack", texture));
	assert(config_set_string(&gstate.config_user, "paths.audiopack", loud));
	assert(config_save(&gstate.config_user, path));
}

static size_t read_all(const char* path, char* out, size_t size) {
	FILE* f = fopen(path, "rb");
	assert(f);
	size_t n = fread(out, 1, size, f);
	assert(fclose(f) == 0);
	return n;
}

/* 2026-09-17 DELTA review, HIGH: paths.audiopack can outlive the pack it
 * names (apply A -> apply texture-only B -> delete A), and no screen can move
 * it once the directory is gone. audio_registry_load/audio_registry_reload and
 * audio_music_init/audio_music_registry_reload share ONE resolver each
 * (audio_effective_pack / audio_music_select_pack), so driving the reload
 * drives the load path's resolution by construction. */
static void test_missing_audio_root_falls_back(const char* path,
											   const char* legacy,
											   const char* gone) {
	uint32_t frames = 0, rate = 0;
	char before[4096], after[4096];

	/* Fallback 1: a deleted audio pack falls back to the texture pack. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack", legacy));
	assert(config_set_string(&gstate.config_user, "paths.audiopack", gone));
	assert(config_save(&gstate.config_user, path));
	audio_music_registry_reload();
	audio_registry_reload();
	assert(strcmp(audio_music_test_effective_pack(), legacy) == 0);
	assert(audio_music_track_count() == 1);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == 3 && rate == 11025);
	/* Nothing was written back: a launch must not rewrite settings. */
	assert_disk_selection(path, legacy, gone);

	/* Fallback 2: with the texture pack gone too, the bundled default. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack", gone));
	assert(config_save(&gstate.config_user, path));
	audio_music_registry_reload();
	assert(strcmp(audio_music_test_effective_pack(), "assets") == 0);
	assert(audio_music_track_count() == 0); /* no bundled music, by design */
	assert_disk_selection(path, gone, gone);

	/* Default path: NO paths.audiopack key at all -> no probe, no fallback,
	 * no write. The root stays exactly what paths.texturepack says even when
	 * that directory does not exist -- byte-identical to the pre-audiopack
	 * behaviour for a stock config. */
	assert(json_object_dotremove(json_object(gstate.config_user.root),
								 "paths.audiopack")
		   == JSONSuccess);
	assert(config_save(&gstate.config_user, path));
	size_t n_before = read_all(path, before, sizeof(before));
	audio_music_registry_reload();
	assert(strcmp(audio_music_test_effective_pack(), gone) == 0);
	size_t n_after = read_all(path, after, sizeof(after));
	assert(n_before == n_after && memcmp(before, after, n_before) == 0);
	assert(!has_audio_key(&gstate.config_user));
}

/* ===== observer telemetry (Astra gate 3 condition 3) =====================
 * The console emits nothing today about an audio-root switch. These four
 * records are the evidence a real-Wii run will carry; assert their shape and,
 * more importantly, WHEN they do and do not appear -- an apply that does not
 * move the root must not claim a reload it did not pay for. */
#define TEL_MAX 24
static char s_tel_lines[TEL_MAX][1400];
static int s_tel_count;

static bool tel_capture(const char* line) {
	if(s_tel_count < TEL_MAX)
		snprintf(s_tel_lines[s_tel_count], sizeof(s_tel_lines[0]), "%s", line);
	s_tel_count++;
	return true;
}

static void tel_reset(void) {
	s_tel_count = 0;
}

static int tel_count(const char* event) {
	char needle[64];
	snprintf(needle, sizeof(needle), "{\"event\":\"%s\"", event);
	int n = 0;
	for(int i = 0; i < s_tel_count && i < TEL_MAX; i++)
		if(strncmp(s_tel_lines[i], needle, strlen(needle)) == 0)
			n++;
	return n;
}

static const char* tel_line(const char* event) {
	char needle[64];
	snprintf(needle, sizeof(needle), "{\"event\":\"%s\"", event);
	for(int i = 0; i < s_tel_count && i < TEL_MAX; i++)
		if(strncmp(s_tel_lines[i], needle, strlen(needle)) == 0)
			return s_tel_lines[i];
	return NULL;
}

/* Every record must be a complete, single-line JSON object: the emitter
 * rejects anything whose last byte is not '}' (hw_telemetry.c:2700), and a
 * truncated line would be dropped on the wire rather than noticed here. */
static void tel_assert_wellformed(const char* line) {
	assert(line);
	size_t n = strlen(line);
	assert(n > 2 && line[0] == '{' && line[n - 1] == '}');
	assert(strchr(line, '\n') == NULL);
	assert(strstr(line, "\"run_id\"") == NULL); /* the emitter appends it */
}

static void tel_assert_member(const char* line, const char* key,
							  const char* value) {
	char needle[512];
	snprintf(needle, sizeof(needle), "\"%s\":%s", key, value);
	assert(strstr(line, needle) != NULL);
}

static void tel_assert_string_member(const char* line, const char* key,
									 const char* value) {
	char needle[512];
	snprintf(needle, sizeof(needle), "\"%s\":\"%s\"", key, value);
	assert(strstr(line, needle) != NULL);
}

static void test_observer_events(const char* path, const char* texture,
								 const char* loud, const char* explicit_audio) {
	audio_registry_test_set_observer(tel_capture);
	audio_music_test_set_observer(tel_capture);
	screen_texturepacks_test_set_observer(tel_capture);
	s_reload_config = &gstate.config_user;
	s_rejected_pack = NULL;

	/* (1a) A TEXTURE apply of a pack that carries audio. The record must say
	 * the audio root never moved and that neither registry was reloaded --
	 * this is the line a collector reads to prove the console did not pay the
	 * ~25 s re-decode for a texture choice. */
	tel_reset();
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	assert(apply_via_screen_seam(explicit_audio, false));
	assert(audio_registry_test_reload_count() == 0);

	const char* apply = tel_line("pack_apply");
	tel_assert_wellformed(apply);
	tel_assert_string_member(apply, "pack", explicit_audio);
	tel_assert_string_member(apply, "audio_before", loud);
	tel_assert_string_member(apply, "audio_after", loud);
	tel_assert_string_member(apply, "audio_final", loud);
	tel_assert_member(apply, "applied", "true");
	tel_assert_member(apply, "ingame", "false");
	tel_assert_member(apply, "sound_reloaded", "false");
	tel_assert_member(apply, "music_reloaded", "false");
	tel_assert_member(apply, "tracks", "2");
	tel_assert_member(apply, "grace_ms", "60000"); /* the budget, on the record */
	assert(strstr(apply, "\"total_ms\":") != NULL);
	assert(tel_count("audio_reload") == 0 && tel_count("music_reload") == 0
		   && "a texture apply emitted an audio reload record");

	/* (1b) A root that really MOVES, and the two registry records it emits.
	 * Driven by an EXPLICIT audio selection because a texture apply can no
	 * longer move the root; when the explicit audio seam lands, these three
	 * lines become a call to it and the pack_apply half of the record comes
	 * back (RED-RETURN "later audio cases", 3). The record SHAPES below are
	 * emitted by audio_music.c / audio_registry.c themselves, so they are the
	 * same lines either way. */
	tel_reset();
	assert(config_set_string(&gstate.config_user, "paths.audiopack",
							 explicit_audio));
	assert(config_save(&gstate.config_user, path));
	audio_registry_test_reload_count_reset();
	audio_music_registry_reload();
	audio_registry_reload();
	assert(audio_registry_test_reload_count() == 1);

	const char* music = tel_line("music_reload");
	tel_assert_wellformed(music);
	tel_assert_string_member(music, "root", explicit_audio);
	tel_assert_member(music, "tracks", "2");
	tel_assert_member(music, "records", "1"); /* the fixture ships 13.ogg */
	assert(strstr(music, "\"ms\":") != NULL);

	const char* reload = tel_line("audio_reload");
	tel_assert_wellformed(reload);
	tel_assert_string_member(reload, "root", explicit_audio);
	tel_assert_string_member(reload, "fallback", "none");
	/* The drain really finished before the arena was rewritten. */
	tel_assert_member(reload, "voices_active_after_stop", "0");
	tel_assert_member(reload, "arena_rejects", "0");
	tel_assert_member(reload, "arena_cap", "12582912");
	assert(strstr(reload, "\"variants\":0,") == NULL); /* something loaded */
	assert(strstr(reload, "\"arena_used\":0,") == NULL);
	assert(strstr(reload, "\"keys_fallback\":") != NULL);
	assert(strstr(reload, "\"stop_ms\":") != NULL);
	assert(strstr(reload, "\"mem1_free_kb\":[") != NULL);
	assert(strstr(reload, "\"mem2_free_kb\":[") != NULL);
	/* One record per registry rebuild, and no transaction record: the two
	 * reloads above were driven explicitly, not by an apply. (The ORDERING
	 * claim -- the pack_apply record last, after both registries -- belongs to
	 * the explicit audio seam; RED-RETURN "later audio cases", 3.) */
	assert(tel_count("audio_reload") == 1 && tel_count("music_reload") == 1
		   && tel_count("pack_apply") == 0);

	/* ...and exactly ONE first-play record, emitted when a VOICE really
	 * starts -- not when a variant is merely picked. A pick with no start is
	 * exactly the shape of audio_play3d's distance cull (pick, then return
	 * before audio_backend_voice_start), and it must produce nothing. */
	assert(tel_count("audio_first_play") == 0);
	const struct audio_sample* culled = audio_registry_pick(SOUND_STEP_GRASS);
	assert(culled != NULL);
	assert(tel_count("audio_first_play") == 0); /* culled: no voice, no record */

	/* The real start, driven through the production entry point audio.c calls
	 * immediately after audio_backend_voice_start. (The device is closed in
	 * this test -- audio_init(false, ...) -- so audio_play3d/audio_play_ui
	 * return before picking at all; this is the same call with the same
	 * arguments audio.c passes.) */
	const struct audio_sample* started = audio_registry_pick(SOUND_STEP_GRASS);
	assert(started != NULL);
	audio_registry_note_voice_started(7, started);
	assert(tel_count("audio_first_play") == 1);
	const char* first = tel_line("audio_first_play");
	tel_assert_wellformed(first);
	tel_assert_string_member(first, "key", "step.grass");
	tel_assert_string_member(first, "root", explicit_audio);
	tel_assert_member(first, "rate", "8000"); /* explicit_audio's fingerprint */
	tel_assert_member(first, "frames", "3");
	tel_assert_member(first, "voice", "7");		 /* a real slot, never null */
	tel_assert_member(first, "in_arena", "true"); /* decoded by THIS reload */
	tel_assert_string_member(first, "evidence", "voice_started");
	/* Never a second one, however many more plays follow. */
	for(int i = 0; i < 4; i++) {
		const struct audio_sample* again
			= audio_registry_pick(SOUND_STEP_GRASS);
		assert(again != NULL);
		audio_registry_note_voice_started(i, again);
	}
	assert(tel_count("audio_first_play") == 1);
	/* A start with no reload behind it stays silent too. */
	audio_registry_note_voice_started(0, started);
	assert(tel_count("audio_first_play") == 1);

	/* Print what this build actually puts on the wire. The claim being made is
	 * a JSON shape a collector has to parse, and for a change with nothing to
	 * photograph the passing evidence IS the proof -- so show the records
	 * rather than only asserting about them. Four lines, once. */
	printf("test_pack_helper_audio: observer records emitted by this build\n");
	for(int i = 0; i < s_tel_count && i < TEL_MAX; i++)
		printf("  %s\n", s_tel_lines[i]);

	/* (2) An apply that does NOT move the root: re-applying the pack that is
	 * already the audio selection. The record must say so, and no reload
	 * record may appear at all -- this is the ~25 s the real Wii used to pay
	 * twice per walk (test_no_sound_reload_when_the_root_did_not_move). */
	tel_reset();
	s_reload_calls = 0;
	audio_registry_test_reload_count_reset();
	assert(apply_via_screen_seam(explicit_audio, false));
	assert(audio_registry_test_reload_count() == 0);
	apply = tel_line("pack_apply");
	tel_assert_wellformed(apply);
	tel_assert_member(apply, "sound_reloaded", "false");
	tel_assert_member(apply, "music_reloaded", "false");
	tel_assert_string_member(apply, "audio_before", explicit_audio);
	tel_assert_string_member(apply, "audio_final", explicit_audio);
	assert(tel_count("audio_reload") == 0);
	assert(tel_count("music_reload") == 0);
	assert(tel_count("audio_first_play") == 0);

	/* (3) A pack root is a folder name the player chose; a quote or a
	 * backslash in it must be escaped, not emitted raw into the record. Driven
	 * through paths.texturepack with no paths.audiopack key, the one branch
	 * that resolves a root verbatim without probing that it exists -- a
	 * directory with a quote in its name cannot be created on Windows. */
	static const char* const odd = "we\"ird\\root";
	assert(json_object_dotremove(json_object(gstate.config_user.root),
								 "paths.audiopack")
		   == JSONSuccess);
	assert(config_set_string(&gstate.config_user, "paths.texturepack", odd));
	assert(config_save(&gstate.config_user, path));
	tel_reset();
	audio_registry_reload();
	reload = tel_line("audio_reload");
	tel_assert_wellformed(reload);
	assert(strstr(reload, "\"root\":\"we\\\"ird\\\\root\"") != NULL);
	/* Nothing resolves under that root, so every key that has one falls back
	 * to the bundled default -- the number that says how much of what the
	 * player hears is still CaveWii's own audio. */
	assert(strstr(reload, "\"keys_fallback\":0,") == NULL);
	tel_assert_string_member(reload, "fallback", "none");

	/* Disarm, and leave a coherent selection for the next stage. */
	audio_registry_test_set_observer(NULL);
	audio_music_test_set_observer(NULL);
	screen_texturepacks_test_set_observer(NULL);
	tel_reset();
	assert(config_set_string(&gstate.config_user, "paths.texturepack", texture));
	assert(config_set_string(&gstate.config_user, "paths.audiopack", loud));
	assert(config_save(&gstate.config_user, path));
	audio_registry_reload();
	audio_music_registry_reload();
	assert(s_tel_count == 0); /* disarmed: not one line */
}

static void test_runtime_teardown(void) {
	audio_music_shutdown();
	audio_shutdown();
	config_destroy(&gstate.config_user);
}

/* test_fixture_join's shared 384-byte buffer makes GCC reason about a
 * worst-case 384-character root once these single-call helpers inline into
 * main, which it reports as a possible truncation. The paths are ~50
 * characters; give these two their own buffer with real headroom rather than
 * work around the warning. */
static char g_join[512];
static const char* join_pack(const char* root, const char* rel) {
	snprintf(g_join, sizeof(g_join), "%s%s", root, rel);
	return g_join;
}

/* A pack that CLAIMS music/ but delivers nothing loadable there. */
static void make_hollow_pack(const char* root, uint32_t rate) {
	assert(MKDIR(root) == 0);
	assert(MKDIR(join_pack(root, "/sound")) == 0);
	assert(MKDIR(join_pack(root, "/sound/step")) == 0);
	assert(MKDIR(join_pack(root, "/music")) == 0); /* empty on purpose */
	uint8_t wav[64];
	size_t n = wav_build(wav, rate);
	write_bytes(join_pack(root, "/sound/step/grass1.wav"), wav, n);
}

/* The default-pack shape: loadable sounds, NO music/ and no records/ at all
 * (what "assets" looks like on the card). */
static void make_sounds_only_pack(const char* root, uint32_t rate) {
	assert(MKDIR(root) == 0);
	assert(MKDIR(join_pack(root, "/sound")) == 0);
	assert(MKDIR(join_pack(root, "/sound/step")) == 0);
	uint8_t wav[64];
	size_t n = wav_build(wav, rate);
	write_bytes(join_pack(root, "/sound/step/grass1.wav"), wav, n);
}

/* The shape boot accepts but a NEW pick must not: loadable effects AND a
 * music/ directory whose one file matches no music key. */
static void make_stubborn_pack(const char* root, uint32_t rate) {
	static const uint8_t marker[] = {0, 1, 2, 3};
	assert(MKDIR(root) == 0);
	assert(MKDIR(join_pack(root, "/sound")) == 0);
	assert(MKDIR(join_pack(root, "/sound/step")) == 0);
	uint8_t wav[64];
	write_bytes(join_pack(root, "/sound/step/grass1.wav"), wav,
				wav_build(wav, rate));
	assert(MKDIR(join_pack(root, "/music")) == 0);
	write_bytes(join_pack(root, "/music/not-a-beta-key.ogg"), marker,
				sizeof(marker));
}

/* Music that only the CREATIVE catalog knows (audio_music_table.h's
 * AUDIO_MUSIC_CREATIVE_TABLE): zero survival tracks, one creative track. */
static void make_creative_only_pack(const char* root) {
	static const uint8_t marker[] = {0, 1, 2, 3};
	assert(MKDIR(root) == 0);
	assert(MKDIR(join_pack(root, "/music")) == 0);
	assert(MKDIR(join_pack(root, "/music/creative")) == 0);
	write_bytes(join_pack(root, "/music/creative/aria_math.ogg"), marker,
				sizeof(marker));
}

/* A pack whose music/ holds a real .ogg under a name no music key resolves. */
static void make_mislabelled_pack(const char* root) {
	static const uint8_t marker[] = {0, 1, 2, 3};
	assert(MKDIR(root) == 0);
	assert(MKDIR(join_pack(root, "/music")) == 0);
	write_bytes(join_pack(root, "/music/not-a-beta-key.ogg"), marker,
				sizeof(marker));
}

int main(void) {
	char fixture[256];
	test_fixture_dir("test_pack_helper_audio", fixture, sizeof(fixture));
	assert(MKDIR(fixture) == 0);
	char legacy[384], texture[384], explicit_audio[384], loud[384];
	char hollow[384], mislabelled[384], gone[384], runtime_path[384];
	char sounds_only[384];
	snprintf(legacy, sizeof(legacy), "%s/legacy", fixture);
	snprintf(texture, sizeof(texture), "%s/texture", fixture);
	snprintf(explicit_audio, sizeof(explicit_audio), "%s/audio", fixture);
	snprintf(loud, sizeof(loud), "%s/loud", fixture);
	snprintf(hollow, sizeof(hollow), "%s/hollow", fixture);
	snprintf(mislabelled, sizeof(mislabelled), "%s/mislabelled", fixture);
	snprintf(gone, sizeof(gone), "%s/deleted-pack", fixture); /* never created */
	snprintf(runtime_path, sizeof(runtime_path), "%s/runtime.json", fixture);
	make_audio_pack(legacy, 11025, 1);
	assert(MKDIR(texture) == 0); /* texture-only: no sound/, music/, records/ */
	make_audio_pack(explicit_audio, 8000, 2);
	make_audio_pack(loud, 22050, 2); /* the pack a player selects in-menu */
	make_hollow_pack(hollow, 44100);
	make_mislabelled_pack(mislabelled);
	snprintf(sounds_only, sizeof(sounds_only), "%s/sounds-only", fixture);
	make_sounds_only_pack(sounds_only, 32000); /* the "assets" shape */
	char stubborn[384], creative_only[384];
	snprintf(stubborn, sizeof(stubborn), "%s/stubborn", fixture);
	make_stubborn_pack(stubborn, 44100);
	snprintf(creative_only, sizeof(creative_only), "%s/creative-only", fixture);
	make_creative_only_pack(creative_only);

	memset(&gstate, 0, sizeof(gstate));
	test_transaction_rollbacks(fixture, legacy, texture, explicit_audio);
	test_runtime_selection(runtime_path, legacy, texture, explicit_audio, loud);
	test_texture_apply_never_takes_audio(runtime_path, texture, loud,
										 explicit_audio);
	test_explicit_audio_choice(fixture, runtime_path, texture, loud,
							   explicit_audio, mislabelled, sounds_only);
	test_rollback_restores_and_creative_counts(fixture, runtime_path, texture,
											   loud, explicit_audio, stubborn,
											   creative_only);
	test_empty_audio_never_steals_selection(runtime_path, texture, loud, hollow,
											mislabelled);
	test_no_sound_reload_when_the_root_did_not_move(runtime_path, texture, loud,
													sounds_only);
	test_ingame_apply_does_not_reload_audio(runtime_path, texture, loud,
											explicit_audio);
	test_observer_events(runtime_path, texture, loud, explicit_audio);
	test_missing_audio_root_falls_back(runtime_path, legacy, gone);
	test_runtime_teardown();

	test_fixture_dir_cleanup(fixture);
	puts("test_pack_helper_audio: PASS legacy fallback, independent audio root, "
		 "transaction rollback/persistence, stable records, real-seam texture "
		 "apply never takes the audio root (selection and both live registries "
		 "unchanged, no re-decode), explicit audio choice (independent of "
		 "textures and kept across a texture swap, repeat = no work, "
		 "claimed-but-unplayable music rejected to the previous root with no "
		 "decode, sound-only accepted with no music, save-failure exact "
		 "rollback, in-game refused), empty/unresolvable "
		 "audio never steals the selection, no sound re-decode when the audio "
		 "root did not move, in-game apply reloads no audio, deleted audio root "
		 "falls back at load without writing the config, observer records "
		 "(pack_apply + music_reload + audio_reload + exactly one "
		 "audio_first_play on a moved root; pack_apply alone with "
		 "sound_reloaded=false on an unmoved one; JSON-escaped pack roots; "
		 "silent when disarmed)");
	return 0;
}
