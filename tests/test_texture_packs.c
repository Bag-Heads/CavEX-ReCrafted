/*
	Unit tests for the texture-pack backend (config write/save + pack scan)
	that the in-game pack selector will build on. Fully headless-verifiable.

	1. config set -> save -> reload roundtrip: setting paths.texturepack and
	   saving must persist to disk AND leave other keys intact (the selector
	   writes the pack choice into the user config without clobbering it).
	2. texturepacks_list: lists only subdirectories of a folder (the packs),
	   not files, not "."/"..".
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#define RMDIR(p) rmdir(p)
#endif

#include "support/test_fixture_dir.h"

#include "config.h"
#include "parson/parson.h"
#include "platform/texture.h"
#include "texturepacks.h"
#include "util.h"

static struct config* reload_config;
static int reload_calls;
static const char* rejected_pack;

static bool fake_reload(void) {
	reload_calls++;
	return strcmp(config_read_string(reload_config, "paths.texturepack", "?"),
				  rejected_pack)
		!= 0;
}

static void test_config_roundtrip(void) {
	char path[128];
	test_fixture_dir("test_cfg_roundtrip.json", path, sizeof path);

	// Seed a config with an existing key + the default pack.
	JSON_Value* root = json_value_init_object();
	json_object_dotset_string(json_object(root), "paths.worlds", "saves");
	json_object_dotset_string(json_object(root), "paths.texturepack", "assets");
	assert(json_serialize_to_file(root, path) == JSONSuccess);
	json_value_free(root);

	struct config c;
	assert(config_create(&c, path));
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), "assets")
		   == 0);

	// Change the pack choice and persist.
	assert(config_set_string(&c, "paths.texturepack", "texturepacks/beta"));
	assert(config_set_int(&c, "debug.autopilot_pitch_deg", 108));
	assert(config_save(&c, path));
	config_destroy(&c);

	// Reload from disk: the change persisted and the other key survived.
	struct config c2;
	assert(config_create(&c2, path));
	assert(strcmp(config_read_string(&c2, "paths.texturepack", "?"),
				  "texturepacks/beta")
		   == 0);
	assert(strcmp(config_read_string(&c2, "paths.worlds", "?"), "saves") == 0);
	assert(config_read_int(&c2, "debug.autopilot_pitch_deg", 0) == 108);
	config_destroy(&c2);

	remove(path);
}

static void test_pack_scan(void) {
	char base[128];
	test_fixture_dir("test_tp_dir", base, sizeof base);
	char p[256];
	MKDIR(base);
	snprintf(p, sizeof(p), "%s/packA", base);
	MKDIR(p);
	snprintf(p, sizeof(p), "%s/packB", base);
	MKDIR(p);
	// A regular file must NOT be listed as a pack.
	snprintf(p, sizeof(p), "%s/notes.txt", base);
	FILE* f = fopen(p, "w");
	if(f)
		fclose(f);

	string_t names[8];
	size_t n = texturepacks_list(base, names, 8);
	assert(n == 2); // packA + packB only (not notes.txt, "." or "..")

	bool gotA = false, gotB = false;
	for(size_t i = 0; i < n; i++) {
		if(strcmp(string_get_cstr(names[i]), "packA") == 0)
			gotA = true;
		if(strcmp(string_get_cstr(names[i]), "packB") == 0)
			gotB = true;
		string_clear(names[i]);
	}
	assert(gotA && gotB);

	// A missing directory yields zero, not a crash.
	string_t none[2];
	assert(texturepacks_list("test_tp_dir_does_not_exist", none, 2) == 0);

	// best-effort cleanup
	snprintf(p, sizeof(p), "%s/notes.txt", base);
	remove(p);
	snprintf(p, sizeof(p), "%s/packA", base);
	RMDIR(p);
	snprintf(p, sizeof(p), "%s/packB", base);
	RMDIR(p);
	RMDIR(base);
}

static void test_apply_is_transactional(void) {
	char path[128], base[128], good[256], bad[256];
	test_fixture_dir("test_tp_apply_dirs", base, sizeof base);
	assert(MKDIR(base) == 0);
	snprintf(good, sizeof good, "%s/good", base);
	snprintf(bad, sizeof bad, "%s/broken", base);
	assert(MKDIR(good) == 0 && MKDIR(bad) == 0);
	rejected_pack = bad;
	test_fixture_dir("test_tp_apply.json", path, sizeof path);
	JSON_Value* root = json_value_init_object();
	assert(root);
	assert(json_object_dotset_string(json_object(root), "paths.texturepack",
								 "assets") == JSONSuccess);
	assert(json_serialize_to_file(root, path) == JSONSuccess);
	json_value_free(root);

	struct config c;
	assert(config_create(&c, path));
	reload_config = &c;

	/* Reject missing directories and regular files before reload or persistence. */
	reload_calls = 0;
	assert(!texturepacks_apply(&c, "does-not-exist-texture-pack", path, fake_reload));
	assert(!texturepacks_apply(&c, path, path, fake_reload));
	assert(!texturepacks_apply(&c, "", path, fake_reload));
	assert(reload_calls == 0);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), "assets") == 0);

	/* A renderer rejection restores both the live pack and the saved choice. */
	reload_calls = 0;
	assert(!texturepacks_apply(&c, bad, path, fake_reload));
	assert(reload_calls == 2);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), "assets")
		   == 0);
	struct config disk;
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "paths.texturepack", "?"), "assets")
		   == 0);
	config_destroy(&disk);

	/* Persistence failure also rolls back the already-loaded new pack. */
	reload_calls = 0;
	assert(!texturepacks_apply(&c, good,
							 "test_tp_missing_parent/config.json", fake_reload));
	assert(reload_calls == 2);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), "assets")
		   == 0);

	/* A successful reload is the only path that commits the choice. */
	reload_calls = 0;
	assert(texturepacks_apply(&c, good, path, fake_reload));
	assert(reload_calls == 1);
	config_destroy(&c);
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "paths.texturepack", "?"),
				  good)
		   == 0);
	config_destroy(&disk);
	remove(path);
	assert(RMDIR(good) == 0 && RMDIR(bad) == 0 && RMDIR(base) == 0);
}

/* ===== the two EXPLICIT transactions ====================================
 * texturepacks_apply (legacy, above) is unchanged and keeps its own tests.
 * These drive the pair that the native screen actually uses now, directly --
 * no screen, no registries -- so the CONFIG claims are made against the
 * library itself: which key moved, which did not, what survived a rollback,
 * and how many times the reload callback ran. */

static int audio_calls;
static bool audio_accepts = true;

static bool fake_audio_reload(void) {
	audio_calls++;
	return audio_accepts;
}

static void write_stub_file(const char* path) {
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fputc('x', f) != EOF);
	assert(fclose(f) == 0);
}

/* A pack directory. `sound` gives it one loadable effect (what makes it
 * eligible as an audio choice); `empty_music` gives it a music/ directory with
 * nothing in it -- the "claimed but not delivered" shape. */
static void make_pack_dir(const char* base, const char* name, char* out,
						  size_t out_size, bool sound, bool empty_music) {
	char p[512];
	snprintf(out, out_size, "%s/%s", base, name);
	assert(MKDIR(out) == 0);
	if(sound) {
		snprintf(p, sizeof(p), "%s/sound", out);
		assert(MKDIR(p) == 0);
		snprintf(p, sizeof(p), "%s/sound/step", out);
		assert(MKDIR(p) == 0);
		snprintf(p, sizeof(p), "%s/sound/step/grass1.wav", out);
		write_stub_file(p);
	}
	if(empty_music) {
		snprintf(p, sizeof(p), "%s/music", out);
		assert(MKDIR(p) == 0);
	}
}

static bool has_key(struct config* c, const char* key) {
	return json_object_dothas_value(json_object(c->root), key);
}

static void seed_config(const char* path, const char* texture,
						const char* audio_string, bool audio_number) {
	JSON_Value* root = json_value_init_object();
	assert(root);
	JSON_Object* o = json_object(root);
	if(texture)
		assert(json_object_dotset_string(o, "paths.texturepack", texture)
			   == JSONSuccess);
	if(audio_string)
		assert(json_object_dotset_string(o, "paths.audiopack", audio_string)
			   == JSONSuccess);
	else if(audio_number)
		assert(json_object_dotset_number(o, "paths.audiopack", 42)
			   == JSONSuccess);
	assert(json_serialize_to_file(root, path) == JSONSuccess);
	json_value_free(root);
}

static void test_explicit_transactions(void) {
	char base[128], path[128];
	char tex_only[256], carries[256], audio_a[256], audio_b[256], hollow[256];
	test_fixture_dir("test_tp_explicit", base, sizeof base);
	assert(MKDIR(base) == 0);
	make_pack_dir(base, "texonly", tex_only, sizeof tex_only, false, false);
	make_pack_dir(base, "carries", carries, sizeof carries, true, false);
	make_pack_dir(base, "audioa", audio_a, sizeof audio_a, true, false);
	make_pack_dir(base, "audiob", audio_b, sizeof audio_b, true, false);
	make_pack_dir(base, "hollow", hollow, sizeof hollow, true, true);
	test_fixture_dir("test_tp_explicit.json", path, sizeof path);
	struct config c, disk;

	/* (1) A TEXTURE apply of a pack that SHIPS audio leaves an explicit audio
	 * choice exactly where it is -- the whole point of the split. */
	seed_config(path, tex_only, audio_a, false);
	assert(config_create(&c, path));
	reload_config = &c;
	rejected_pack = "";
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, carries, path, fake_reload)
		   == TEXTUREPACKS_OK);
	assert(reload_calls == 1);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), carries) == 0);
	assert(strcmp(config_read_string(&c, "paths.audiopack", "?"), audio_a) == 0
		   && "a texture apply took the audio selection");
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "paths.audiopack", "?"), audio_a) == 0);
	assert(strcmp(config_read_string(&disk, "paths.texturepack", "?"), carries)
		   == 0);
	config_destroy(&disk);

	/* (2) The SAME texture choice: no callback at all. (The no-write half of
	 * NO_CHANGE is proven with a disk sentinel in leg 9, on the audio side --
	 * one transaction core serves both.) */
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, carries, path, fake_reload)
		   == TEXTUREPACKS_NO_CHANGE);
	assert(reload_calls == 0);
	config_destroy(&c);

	/* (3) A legacy config -- audiopack ABSENT -- is pinned to the root the
	 * player was already hearing, i.e. the OLD texture pack. */
	seed_config(path, tex_only, NULL, false);
	assert(config_create(&c, path));
	reload_config = &c;
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, carries, path, fake_reload)
		   == TEXTUREPACKS_OK);
	assert(strcmp(config_read_string(&c, "paths.audiopack", "?"), tex_only) == 0);
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "paths.audiopack", "?"), tex_only)
		   == 0);
	config_destroy(&disk);
	config_destroy(&c);

	/* (4) A WRONG-TYPED audiopack is not an explicit choice either: same pin
	 * on success. */
	seed_config(path, tex_only, NULL, true);
	assert(config_create(&c, path));
	reload_config = &c;
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, carries, path, fake_reload)
		   == TEXTUREPACKS_OK);
	assert(strcmp(config_read_string(&c, "paths.audiopack", "?"), tex_only) == 0);
	config_destroy(&c);

	/* (5) RELOAD failure with a wrong-typed key: the exact JSON number comes
	 * back, in memory and on disk, and the texture choice never moved. */
	seed_config(path, tex_only, NULL, true);
	assert(config_create(&c, path));
	reload_config = &c;
	rejected_pack = carries;
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, carries, path, fake_reload)
		   == TEXTUREPACKS_RELOAD_FAILED);
	assert(reload_calls == 2); /* the candidate, then the restore re-run */
	assert(json_object_dothas_value_of_type(json_object(c.root),
										"paths.audiopack", JSONNumber));
	assert(json_object_dotget_number(json_object(c.root), "paths.audiopack")
		   == 42);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), tex_only)
		   == 0);
	config_destroy(&c);
	assert(config_create(&disk, path));
	assert(json_object_dotget_number(json_object(disk.root), "paths.audiopack")
		   == 42);
	assert(strcmp(config_read_string(&disk, "paths.texturepack", "?"), tex_only)
		   == 0);
	config_destroy(&disk);

	/* (6) RELOAD failure on a config with NO "paths" object at all: the keys
	 * stay absent AND the object this attempt created is removed again. */
	seed_config(path, NULL, NULL, false);
	assert(config_create(&c, path));
	assert(!json_object_get_value(json_object(c.root), "paths"));
	reload_config = &c;
	rejected_pack = carries;
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, carries, path, fake_reload)
		   == TEXTUREPACKS_RELOAD_FAILED);
	assert(!has_key(&c, "paths.texturepack") && !has_key(&c, "paths.audiopack"));
	assert(!json_object_get_value(json_object(c.root), "paths")
		   && "the rollback left an empty paths object behind");
	config_destroy(&c);

	/* (7) SAVE failure: same exact restore, via the unwritable path. */
	seed_config(path, tex_only, NULL, true);
	assert(config_create(&c, path));
	reload_config = &c;
	rejected_pack = "";
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, carries,
									   "test_tp_missing_parent/config.json",
									   fake_reload)
		   == TEXTUREPACKS_SAVE_FAILED);
	assert(reload_calls == 2);
	assert(json_object_dotget_number(json_object(c.root), "paths.audiopack")
		   == 42);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), tex_only)
		   == 0);
	config_destroy(&c);

	/* (8) An AUDIO apply moves paths.audiopack and NOTHING else. */
	seed_config(path, tex_only, audio_a, false);
	assert(config_create(&c, path));
	reload_config = &c;
	audio_calls = 0;
	audio_accepts = true;
	assert(texturepacks_apply_audio(&c, audio_b, path, fake_audio_reload)
		   == TEXTUREPACKS_OK);
	assert(audio_calls == 1);
	assert(strcmp(config_read_string(&c, "paths.audiopack", "?"), audio_b) == 0);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), tex_only) == 0
		   && "an audio apply moved the texture selection");
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "paths.audiopack", "?"), audio_b) == 0);
	assert(strcmp(config_read_string(&disk, "paths.texturepack", "?"), tex_only)
		   == 0);
	config_destroy(&disk);

	/* (9) The same audio choice again: no callback AND no save. Proven by a
	 * sentinel written to the FILE only -- a save would serialise the
	 * in-memory tree over it. */
	{
		struct config sentinel;
		assert(config_create(&sentinel, path));
		assert(config_set_string(&sentinel, "mp.username", "sentinel"));
		assert(config_save(&sentinel, path));
		config_destroy(&sentinel);
	}
	audio_calls = 0;
	assert(texturepacks_apply_audio(&c, audio_b, path, fake_audio_reload)
		   == TEXTUREPACKS_NO_CHANGE);
	assert(audio_calls == 0);
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "mp.username", "?"), "sentinel") == 0
		   && "a NO_CHANGE audio apply rewrote the config file");
	config_destroy(&disk);

	/* (10) A REJECTED audio root: exact restore, callback re-run from the old
	 * root, nothing persisted. */
	audio_calls = 0;
	audio_accepts = false;
	assert(texturepacks_apply_audio(&c, audio_a, path, fake_audio_reload)
		   == TEXTUREPACKS_RELOAD_FAILED);
	assert(audio_calls == 2);
	assert(strcmp(config_read_string(&c, "paths.audiopack", "?"), audio_b) == 0);
	audio_accepts = true;

	/* (11) Validation, before any callback or write: a missing directory, and
	 * a pack that ships no audio, and one that CLAIMS music and delivers
	 * none (gate 1 item 3). */
	audio_calls = 0;
	assert(texturepacks_apply_audio(&c, "test_tp_no_such_pack", path,
									fake_audio_reload)
		   == TEXTUREPACKS_INVALID);
	assert(texturepacks_apply_audio(&c, tex_only, path, fake_audio_reload)
		   == TEXTUREPACKS_INVALID
		   && "a pack with no audio was accepted as an audio choice");
	assert(texturepacks_apply_audio(&c, hollow, path, fake_audio_reload)
		   == TEXTUREPACKS_INVALID
		   && "a claimed-but-empty music/ was accepted");
	reload_calls = 0;
	assert(texturepacks_apply_textures(&c, "test_tp_no_such_pack", path,
									   fake_reload)
		   == TEXTUREPACKS_INVALID);
	assert(audio_calls == 0 && reload_calls == 0);
	assert(strcmp(config_read_string(&c, "paths.audiopack", "?"), audio_b) == 0);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), tex_only) == 0);
	config_destroy(&c);
	remove(path);
}

/* REVIEW-LIBRARY-1 (MEDIUM): two DISTINCT roots that share their first 251
 * bytes used to normalise through 256-byte buffers, compare equal, and make
 * the explicit APIs answer NO_CHANGE -- the player's pick silently ignored.
 *
 * The equality claim is proven on the complete strings (no filesystem needed).
 * The APPLY claim needs two real directories that deep, which not every host
 * allows (Windows resolves a relative path against a ~260-character limit), so
 * it runs only when both were really created, and says so when it cannot. */
static void test_long_roots_are_distinct(void) {
	char base[128], deep_a[512], deep_b[512], mid[512];
	static const char seg[]
		= "0123456789012345678901234567890123456789012345678901234567890123"
		  "4567890123456789012345678901234567890123456789012345678901234567";

	/* Complete-root equality, unconditionally. */
	{
		char a[600], b[600];
		snprintf(a, sizeof(a), "texturepacks/%s/%s/alpha", seg, seg);
		snprintf(b, sizeof(b), "texturepacks/%s/%s/beta", seg, seg);
		assert(strlen(a) > 260 && strncmp(a, b, 251) == 0);
		assert(!texturepacks_value_eq(a, b)
			   && "two distinct long roots compared equal (truncation)");
		/* ...and the relative vs sd:/ contract still holds at that length. */
		char anchored[600];
		snprintf(anchored, sizeof(anchored), "sd:/%s", a);
		assert(texturepacks_value_eq(a, anchored));
	}

	test_fixture_dir("test_tp_long", base, sizeof base);
	if(MKDIR(base) != 0)
		return;
	snprintf(mid, sizeof(mid), "%s/%s", base, seg);
	int ok = MKDIR(mid) == 0;
	if(ok) {
		snprintf(deep_a, sizeof(deep_a), "%s/%s", mid, seg);
		ok = MKDIR(deep_a) == 0;
	}
	if(ok) {
		snprintf(deep_b, sizeof(deep_b), "%s/alpha", deep_a);
		snprintf(mid, sizeof(mid), "%s/beta", deep_a);
		ok = MKDIR(deep_b) == 0 && MKDIR(mid) == 0
			&& strlen(deep_b) > 251;
	}
	if(!ok) {
		printf("test_texture_packs: long-root APPLY leg SKIPPED -- this host "
			   "would not create a >251-character pack path (the equality "
			   "claim above still ran)\n");
		return;
	}

	char path[128];
	test_fixture_dir("test_tp_long.json", path, sizeof path);
	seed_config(path, deep_b, NULL, false);
	struct config c;
	assert(config_create(&c, path));
	reload_config = &c;
	rejected_pack = "";
	reload_calls = 0;
	/* `mid` is the sibling ".../beta": distinct from ".../alpha" only after
	 * byte 251. It must really reload and really persist. */
	assert(texturepacks_apply_textures(&c, mid, path, fake_reload)
		   == TEXTUREPACKS_OK
		   && "a distinct long root was mistaken for the current one");
	assert(reload_calls == 1);
	assert(strcmp(config_read_string(&c, "paths.texturepack", "?"), mid) == 0);
	config_destroy(&c);
	struct config disk;
	assert(config_create(&disk, path));
	assert(strcmp(config_read_string(&disk, "paths.texturepack", "?"), mid) == 0);
	config_destroy(&disk);
	remove(path);
}

static void test_wii_dimension_gate_is_shared(void) {
	assert(tex_image_dimensions_valid(1, 1));
	assert(tex_image_dimensions_valid(1024, 1024));
	assert(!tex_image_dimensions_valid(0, 16));
	assert(!tex_image_dimensions_valid(1025, 16));
	assert(tex_gfx_dimensions_valid(16, 16, TEX_FMT_I8));
	assert(!tex_gfx_dimensions_valid(12, 16, TEX_FMT_I8));
	assert(tex_gfx_dimensions_valid(20, 12, TEX_FMT_RGBA16));
	assert(!tex_gfx_dimensions_valid(18, 12, TEX_FMT_RGBA16));
	assert(!tex_gfx_dimensions_valid(1028, 16, TEX_FMT_RGBA16));
}

/* path_from_sd_root: the Wii launch-CWD anchor (config_read_path's core).
   A relative pack path must resolve to the SAME sd:/ location under every
   launcher (HBC-direct CWD sd:/apps/CavEX/ vs chainloaded CWD sd:/ --
   the two-config/two-CWD trap); absolute forms pass through untouched. */
static void test_path_from_sd_root(void) {
	char buf[64];

	/* relative -> anchored to the SD root */
	assert(strcmp(path_from_sd_root("texturepacks/beta-1.7.3", buf, sizeof(buf)),
				  "sd:/texturepacks/beta-1.7.3")
		   == 0);
	assert(strcmp(path_from_sd_root("assets", buf, sizeof(buf)), "sd:/assets")
		   == 0);

	/* absolute (device prefix or leading '/') -> untouched, SAME pointer */
	const char* abs1 = "sd:/texturepacks/beta-1.7.3";
	assert(path_from_sd_root(abs1, buf, sizeof(buf)) == abs1);
	const char* abs2 = "usb:/packs/x";
	assert(path_from_sd_root(abs2, buf, sizeof(buf)) == abs2);
	const char* abs3 = "/texturepacks/x";
	assert(path_from_sd_root(abs3, buf, sizeof(buf)) == abs3);

	/* both spellings of the same pack normalize equal (the picker's
	   active-row match across the absolute field-mitigation config) */
	char buf2[64];
	assert(strcmp(path_from_sd_root("texturepacks/beta-1.7.3", buf, sizeof(buf)),
				  path_from_sd_root("sd:/texturepacks/beta-1.7.3", buf2,
									sizeof(buf2)))
		   == 0);
}

int main(void) {
	test_config_roundtrip();
	test_pack_scan();
	test_apply_is_transactional();
	test_explicit_transactions();
	test_long_roots_are_distinct();
	test_wii_dimension_gate_is_shared();
	test_path_from_sd_root();
	puts("test_texture_packs: PASS config roundtrip, pack scan, legacy "
		 "transaction, explicit texture/audio transactions (independent keys, "
		 "legacy pin, exact rollback after reload and save failure incl. "
		 "absent/wrong-typed keys and the paths object, no-change writes "
		 "nothing, validation rejects missing and claimed-but-empty audio), "
		 "complete-root equality");
	return 0;
}
