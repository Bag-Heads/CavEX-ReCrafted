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

#include "texturepacks.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "util.h" /* path_from_sd_root: launcher-independent pack compare */
#include "parson/parson.h"
#include "platform/tex_beta_layout.h" /* the raw-jar layout rule */

size_t texturepacks_list(const char* dir, string_t* out, size_t max) {
	if(!dir || !out || max == 0)
		return 0;

	DIR* d = opendir(dir);
	if(!d)
		return 0;

	size_t n = 0;
	struct dirent* e;
	while(n < max && (e = readdir(d))) {
		if(e->d_name[0] == '.') // skip ".", "..", and hidden entries
			continue;

		string_t path;
		string_init_printf(path, "%s/%s", dir, e->d_name);
		struct stat st;
		bool is_dir
			= stat(string_get_cstr(path), &st) == 0 && S_ISDIR(st.st_mode);
		string_clear(path);

		if(is_dir) {
			string_init_set_str(out[n], e->d_name);
			n++;
		}
	}

	closedir(d);
	return n;
}

/* Case-insensitive "does this file name end in `ext`". */
static bool name_has_ext(const char* name, const char* ext) {
	size_t n = strlen(name), e = strlen(ext);
	if(n <= e)
		return false;
	for(size_t i = 0; i < e; i++) {
		char a = name[n - e + i];
		if(a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if(a != ext[i])
			return false;
	}
	return true;
}

/* Is there at least ONE non-empty file the audio scanners could open under
 * `dir`, within `depth` further directory levels? */
static bool dir_has_loadable(const char* dir, const char* const* exts,
							 size_t ext_count, int depth) {
	DIR* d = opendir(dir);
	if(!d)
		return false; /* unreadable counts as "delivers nothing", not as absent */

	bool found = false;
	struct dirent* e;
	while(!found && (e = readdir(d))) {
		if(e->d_name[0] == '.')
			continue;
		char child[512];
		int written = snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);
		if(written < 0 || (size_t)written >= sizeof(child))
			continue;
		struct stat st;
		if(stat(child, &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode)) {
			if(depth > 0)
				found = dir_has_loadable(child, exts, ext_count, depth - 1);
			continue;
		}
		if(!S_ISREG(st.st_mode) || st.st_size <= 0)
			continue;
		for(size_t i = 0; i < ext_count && !found; i++)
			found = name_has_ext(e->d_name, exts[i]);
	}

	closedir(d);
	return found;
}

/* Does this pack directory carry audio of its own that actually LOADS?
 *
 * <pack>/sound is what audio_registry_scan walks (.wav then .ogg,
 * audio_registry.c:272-286), <pack>/music (+ the newmusic union) what
 * audio_music_registry_scan walks (.ogg, audio_music.c:283-297), and
 * <pack>/records what audio_music_play_record_for_item resolves against --
 * the same three roots the Pack Helper's own has_audio test uses
 * (tools/pack_helper/core.py install()).
 *
 * The rule, and why it is not a bare directory stat (2026-09-17 DELTA review,
 * HIGH): music has NO bundled default -- audio_music_registry_scan's fallback
 * root is "assets", which ships no music at all (audio_music.c:319-323) -- so
 * handing the audio selection to a pack whose music/ is empty or unreadable
 * silences the player's music while the apply reports success. A category the
 * pack CLAIMS (its directory exists) must therefore DELIVER at least one
 * loadable file, and at least one category must be claimed. A pack that claims
 * nothing, or claims something it cannot deliver, does not take the audio
 * selection at all, and the transaction leaves the previous effective audio
 * root exactly where it was.
 *
 * `directory` is the already-SD-anchored form the caller stat()ed, so this
 * probe is correct on the Wii as well as on PC. */
static bool texturepacks_carries_audio(const char* directory) {
	static const char* const sound_exts[] = {".wav", ".ogg"};
	static const char* const ogg_ext[] = {".ogg"};
	static const struct {
		const char* name;
		const char* const* exts;
		size_t ext_count;
		int depth; /* extra directory levels the scanner descends */
	} categories[] = {
		{"sound", sound_exts, 2, 3},
		{"music", ogg_ext, 1, 2},
		{"records", ogg_ext, 1, 1},
	};

	bool claimed = false;
	for(size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++) {
		char probe[512];
		int written = snprintf(probe, sizeof(probe), "%s/%s", directory,
							   categories[i].name);
		if(written < 0 || (size_t)written >= sizeof(probe))
			continue; /* unrepresentable => treat as absent, never truncate */
		struct stat st;
		if(stat(probe, &st) != 0 || !S_ISDIR(st.st_mode))
			continue; /* not claimed */
		claimed = true;
		if(!dir_has_loadable(probe, categories[i].exts, categories[i].ext_count,
							 categories[i].depth))
			return false; /* claimed and did not deliver */
	}
	return claimed;
}

#define TEXPACKS_KEY_TEXTURE "paths.texturepack"
#define TEXPACKS_KEY_AUDIO "paths.audiopack"
#define TEXPACKS_BUNDLED "assets"

/* The config may hold either the relative form the screen writes
 * ("texturepacks/<name>") or the absolute field-mitigation form
 * ("sd:/texturepacks/<name>") -- both name the same pack. Same normalisation
 * the screen's active-row highlight uses (screen_texturepacks.c), so "is this
 * already my selection?" cannot answer differently in the two places.
 * Unconditional: both sides get the same treatment, so equality is unaffected
 * on PC. */
/* The anchored form of `value` WITHOUT a buffer: the only thing
 * path_from_sd_root adds is a literal "sd:/" prefix in front of a relative
 * value, so two values are equal after anchoring exactly when they are equal
 * with that prefix removed from whichever side carries it.
 *
 * Fixing REVIEW-LIBRARY-1 (MEDIUM): the buffered version normalised through
 * fixed 256-byte buffers, so two DISTINCT roots sharing their first 251 bytes
 * both truncated to the same string, compared equal, and the explicit APIs
 * answered NO_CHANGE for a real change -- the player's pick silently ignored.
 * Comparing the complete roots has no length limit at all.
 *
 * "sd:/" is stripped only when what follows is itself relative -- i.e. only
 * when path_from_sd_root could have PRODUCED it. "sd:/usb:/x" or "sd://x"
 * compare verbatim, which is what the anchoring comparison did. */
static const char* pack_value_unanchored(const char* value) {
	if(strncmp(value, "sd:/", 4) != 0)
		return value;
	const char* rest = value + 4;
	if(strchr(rest, ':') || rest[0] == '/')
		return value;
	return rest;
}

bool texturepacks_value_eq(const char* a, const char* b) {
	if(!a || !b)
		return a == b;
	return strcmp(pack_value_unanchored(a), pack_value_unanchored(b)) == 0;
}

#define pack_value_eq texturepacks_value_eq

/* The SD-anchored directory a pack value names, or NULL when it is unusable.
 * A partial pack may omit files, but the pack directory itself must exist --
 * otherwise the fallback loader can falsely report success. */
static const char* pack_directory(const char* pack, char* buf, size_t size) {
	if(!pack || !*pack)
		return NULL;
	const char* directory = pack;
#ifdef PLATFORM_WII
	if(strlen(pack) + sizeof("sd:/") > size)
		return NULL;
	directory = path_from_sd_root(pack, buf, size);
#else
	(void)buf;
	(void)size;
#endif
	struct stat st;
	if(stat(directory, &st) != 0 || !S_ISDIR(st.st_mode))
		return NULL;
	return directory;
}

bool texturepacks_has_audio(const char* pack) {
	char anchored[256];
	const char* directory = pack_directory(pack, anchored, sizeof(anchored));
	return directory && texturepacks_carries_audio(directory);
}

bool texturepacks_is_raw_jar(const char* pack) {
	char anchored[256];
	const char* directory = pack_directory(pack, anchored, sizeof(anchored));
	return directory && tex_beta_raw_jar_layout(directory);
}

bool texturepacks_display_name(const char* pack, char* out, size_t out_len) {
	char anchored[256];
	const char* directory = pack_directory(pack, anchored, sizeof(anchored));
	if(!directory || !out || out_len < 2)
		return false;
	char path[512];
	int written = snprintf(path, sizeof(path), "%s/.cavewii-pack.json",
						   directory);
	if(written < 0 || (size_t)written >= sizeof(path))
		return false;
	struct stat st;
	/* A manifest lists every file (a few KB); refuse anything absurd. */
	if(stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (1 << 20))
		return false;
	JSON_Value* root = json_parse_file(path);
	const char* name = root ? json_object_get_string(json_object(root), "name")
							: NULL;
	bool ok = name && name[0];
	for(const char* c = name; ok && *c; c++)
		ok = (unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7f;
	if(ok)
		snprintf(out, out_len, "%s", name);
	if(root)
		json_value_free(root);
	return ok;
}

bool texturepacks_claims_music(const char* pack) {
	char anchored[256];
	const char* directory = pack_directory(pack, anchored, sizeof(anchored));
	if(!directory)
		return false;
	char probe[512];
	int written = snprintf(probe, sizeof(probe), "%s/music", directory);
	if(written < 0 || (size_t)written >= sizeof(probe))
		return false;
	struct stat st;
	return stat(probe, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ===== the ONE transaction =============================================
 * Every entry point below is this function with a different set of keys to
 * write; there is exactly one rollback routine in this file (gate 1 item 1:
 * "do not duplicate the full rollback engine").
 *
 * Order, fixed (gate 1 item 4): snapshot -> set every tentative value ->
 * runtime reload callback -> save. Any failure restores the EXACT previous
 * JSON -- an absent key goes back to absent, a wrong-typed key goes back to
 * that exact value, and a "paths" object this attempt created is removed
 * again -- and then re-runs the callback, because the moment the callback was
 * attempted the rejected state is what is live. Nothing is persisted unless
 * the config was really put back. */
#define PACK_TXN_MAX_WRITES 2

struct pack_write {
	const char* key;
	const char* value;
	/* Best effort only: what to force this key to if the exact JSON restore
	 * itself failed, so the re-run callback cannot simply re-load the rejected
	 * state (M6). NULL leaves the key alone. */
	const char* fallback;
};

static enum texturepacks_result pack_transaction(struct config* config,
												 const char* config_path,
												 texturepack_reload_fn reload,
												 const struct pack_write* writes,
												 size_t count) {
	JSON_Object* root = json_object(config->root);
	JSON_Value* snapshot[PACK_TXN_MAX_WRITES] = {NULL, NULL};
	bool had[PACK_TXN_MAX_WRITES] = {false, false};
	/* config_set_string's dotset CREATES the intermediate "paths" object. A
	 * rollback that only removes the leaves would leave an empty "paths": {}
	 * behind in a config that never had one (2026-09-17 DELTA review, LOW), so
	 * remember whether it was ours to create. */
	bool had_paths_object = json_object_get_value(root, "paths") != NULL;

	/* Snapshot before the first mutation. If any copy cannot be made, the
	 * transaction cannot promise exact rollback, so leave the config alone. */
	for(size_t i = 0; i < count; i++) {
		JSON_Value* original = json_object_dotget_value(root, writes[i].key);
		had[i] = original != NULL;
		if(!original)
			continue;
		snapshot[i] = json_value_deep_copy(original);
		if(!snapshot[i]) {
			for(size_t k = 0; k < i; k++)
				json_value_free(snapshot[k]);
			return TEXTUREPACKS_INVALID;
		}
	}

	size_t attempted = 0;
	bool prepared = true;
	for(size_t i = 0; i < count && prepared; i++) {
		attempted = i + 1;
		prepared = config_set_string(config, writes[i].key, writes[i].value);
	}
	bool reloaded = prepared && reload();
	bool saved = reloaded && config_save(config, config_path);

	enum texturepacks_result result;
	if(saved)
		result = TEXTUREPACKS_OK;
	else if(!prepared)
		result = TEXTUREPACKS_INVALID;
	else if(!reloaded)
		result = TEXTUREPACKS_RELOAD_FAILED;
	else
		result = TEXTUREPACKS_SAVE_FAILED;

	if(result != TEXTUREPACKS_OK) {
		bool restored = true;
		for(size_t i = 0; i < attempted; i++) {
			if(had[i]) {
				if(json_object_dotset_value(root, writes[i].key, snapshot[i])
				   == JSONSuccess)
					snapshot[i] = NULL; /* ownership transferred */
				else
					restored = false;
			} else if(json_object_dothas_value(root, writes[i].key)) {
				restored = json_object_dotremove(root, writes[i].key)
						== JSONSuccess
					&& restored;
			}
		}
		/* LOW: leave no empty "paths": {} that only this attempt created. */
		if(restored && !had_paths_object) {
			JSON_Object* paths = json_object_get_object(root, "paths");
			if(paths && json_object_get_count(paths) == 0)
				restored = json_object_remove(root, "paths") == JSONSuccess;
		}
		if(prepared) {
			/* M6 (2026-09-17 review): the moment reload() was ATTEMPTED the
			 * rejected selection is live, so re-running the loader is
			 * unconditional on `restored`. */
			bool forced = false;
			if(!restored)
				for(size_t i = 0; i < attempted; i++)
					if(writes[i].fallback)
						forced = config_set_string(config, writes[i].key,
												   writes[i].fallback)
							|| forced;
			(void)reload();
			/* Persist only a config we actually put back; never write out a
			 * half-restored tree. */
			if(restored || forced)
				(void)config_save(config, config_path);
		}
	}

	for(size_t i = 0; i < count; i++)
		json_value_free(snapshot[i]);
	return result;
}

bool texturepacks_apply(struct config* config, const char* pack,
						const char* config_path,
						texturepack_reload_fn reload) {
	if(!config || !pack || !config_path || !reload)
		return false;
	char anchored[256]; /* Match config_read_path's SD-root buffer. */
	const char* directory = pack_directory(pack, anchored, sizeof(anchored));
	if(!directory)
		return false;

	string_t previous;
	string_init_set_str(previous,
				config_read_string(config, TEXPACKS_KEY_TEXTURE, TEXPACKS_BUNDLED));
	/* LEGACY behaviour, deliberately preserved for the old callers and their
	 * tests (gate 1 item 1). Two cases, one write:
	 *   - the new pack CARRIES audio -> it becomes the audio pack too.
	 *   - the new pack is TEXTURE-ONLY -> pin the effective audio root so the
	 *     two selections separate without changing what the player hears.
	 * The native screen no longer calls this; it calls the explicit pair
	 * below, which never takes the audio selection. */
	bool new_carries_audio = texturepacks_carries_audio(directory);
	bool replace_audio = new_carries_audio
		|| json_object_dotget_string(json_object(config->root),
									 TEXPACKS_KEY_AUDIO)
			== NULL;
	struct pack_write writes[PACK_TXN_MAX_WRITES];
	size_t count = 0;
	if(replace_audio) {
		writes[count].key = TEXPACKS_KEY_AUDIO;
		writes[count].value
			= new_carries_audio ? pack : string_get_cstr(previous);
		writes[count].fallback = NULL;
		count++;
	}
	writes[count].key = TEXPACKS_KEY_TEXTURE;
	writes[count].value = pack;
	writes[count].fallback = string_get_cstr(previous);
	count++;

	enum texturepacks_result result
		= pack_transaction(config, config_path, reload, writes, count);
	string_clear(previous);
	return result == TEXTUREPACKS_OK;
}

enum texturepacks_result texturepacks_apply_textures(
	struct config* config, const char* pack, const char* config_path,
	texturepack_reload_fn reload) {
	if(!config || !pack || !config_path || !reload)
		return TEXTUREPACKS_INVALID;
	char anchored[256];
	if(!pack_directory(pack, anchored, sizeof(anchored)))
		return TEXTUREPACKS_INVALID;

	string_t previous;
	string_init_set_str(previous,
				config_read_string(config, TEXPACKS_KEY_TEXTURE, TEXPACKS_BUNDLED));
	if(pack_value_eq(string_get_cstr(previous), pack)) {
		/* Same effective selection: not one reload, not one byte written. */
		string_clear(previous);
		return TEXTUREPACKS_NO_CHANGE;
	}

	/* An absent or wrong-typed paths.audiopack is not an explicit choice: the
	 * effective audio root is whatever paths.texturepack says, so pin it here
	 * before the texture choice moves. A STRING audiopack is the player's own
	 * choice and is never written, whatever this pack ships. */
	bool pin_audio = json_object_dotget_string(json_object(config->root),
											   TEXPACKS_KEY_AUDIO)
		== NULL;
	struct pack_write writes[PACK_TXN_MAX_WRITES];
	size_t count = 0;
	if(pin_audio) {
		writes[count].key = TEXPACKS_KEY_AUDIO;
		writes[count].value = string_get_cstr(previous);
		writes[count].fallback = NULL;
		count++;
	}
	writes[count].key = TEXPACKS_KEY_TEXTURE;
	writes[count].value = pack;
	writes[count].fallback = string_get_cstr(previous);
	count++;

	enum texturepacks_result result
		= pack_transaction(config, config_path, reload, writes, count);
	string_clear(previous);
	return result;
}

enum texturepacks_result texturepacks_apply_audio(struct config* config,
												  const char* pack,
												  const char* config_path,
												  texturepack_reload_fn reload) {
	if(!config || !pack || !config_path || !reload)
		return TEXTUREPACKS_INVALID;
	char anchored[256];
	const char* directory = pack_directory(pack, anchored, sizeof(anchored));
	if(!directory)
		return TEXTUREPACKS_INVALID;
	/* Gate 1 item 3: "reject missing directories AND claimed-but-empty audio."
	 * A pack that ships no audio at all, or that CLAIMS a category (the
	 * directory is there) and delivers no loadable file in it, is refused here
	 * -- before the callback runs and before anything is written -- rather
	 * than being set up to fail later. The bundled default keeps its meaning:
	 * it claims sound/ and delivers it, so it passes; its lack of music/ is a
	 * truthful sound-only choice, not a claim it failed to keep.
	 *
	 * This is FILE ELIGIBILITY only. It proves files are present and
	 * non-empty; it does NOT prove every sample decodes, and the callback
	 * still has to validate that music resolves known tracks. */
	if(!texturepacks_carries_audio(directory))
		return TEXTUREPACKS_INVALID;

	/* The EFFECTIVE audio root, resolved the way audio_registry_load() and
	 * audio_music_init() resolve it: the explicit choice, falling back to the
	 * texture pack for a legacy config. Copied out, because the transaction
	 * rewrites the JSON tree the returned pointer lives in. */
	string_t previous;
	string_init_set_str(
		previous,
		config_read_string(config, TEXPACKS_KEY_AUDIO,
					config_read_string(config, TEXPACKS_KEY_TEXTURE, TEXPACKS_BUNDLED)));
	/* Same root AND already an explicit choice -> nothing to do. A legacy
	 * config whose effective root happens to be this pack still gets the key
	 * written, so the choice stops depending on the texture selection. */
	bool explicit_before = json_object_dotget_string(json_object(config->root),
													TEXPACKS_KEY_AUDIO)
		!= NULL;
	if(explicit_before && pack_value_eq(string_get_cstr(previous), pack)) {
		string_clear(previous);
		return TEXTUREPACKS_NO_CHANGE;
	}

	struct pack_write writes[PACK_TXN_MAX_WRITES];
	writes[0].key = TEXPACKS_KEY_AUDIO;
	writes[0].value = pack;
	writes[0].fallback = string_get_cstr(previous);

	enum texturepacks_result result
		= pack_transaction(config, config_path, reload, writes, 1);
	string_clear(previous);
	return result;
}
