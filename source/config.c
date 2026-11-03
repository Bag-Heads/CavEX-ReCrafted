#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "game/difficulty.h" // next-gaps-0924 W1F: difficulty key
#ifdef PLATFORM_WII
// path_from_sd_root only; conditional so platform-define-less test targets
// (test_config_fallback) don't inherit util.h's platform/time.h dependency.
#include "util.h"
#endif

bool config_read_flag(const struct config* c, const char* key, bool fallback) {
	JSON_Object* object = c ? json_object(c->root) : NULL;
	if(json_object_dothas_value_of_type(object, key, JSONBoolean))
		return json_object_dotget_boolean(object, key) == 1;
	if(json_object_dothas_value_of_type(object, key, JSONNumber))
		return json_object_dotget_number(object, key) != 0;
	return fallback;
}

enum config_profiles_status config_read_profiles(const struct config* c,
												 char out[2][CONFIG_PROFILE_ID_CAP]) {
	if(!out)
		return CONFIG_PROFILES_BAD_KEY;
	memset(out, 0, 2 * CONFIG_PROFILE_ID_CAP);
	if(!c || !c->root || json_value_get_type(c->root) != JSONObject)
		return CONFIG_PROFILES_BAD_KEY;
	JSON_Object* object = json_value_get_object(c->root);
	JSON_Value* profile = json_object_get_value(object, "profile");
	if(profile && json_value_get_type(profile) != JSONObject)
		return CONFIG_PROFILES_BAD_KEY;
	const char* keys[2] = {"profile.player1", "profile.player2"};
	const char* defaults[2] = {"player1", "player2"};
	const char* ids[2];
	for(unsigned i = 0; i < 2; i++) {
		JSON_Value* value = json_object_dotget_value(object, keys[i]);
		ids[i] = value ? json_value_get_string(value) : defaults[i];
		if(!ids[i] || !ids[i][0])
			return CONFIG_PROFILES_BAD_KEY;
		/* Same 1..16 [a-z0-9_] rule as stats_profile_id_valid. Keep config
		 * independent of the achievement object pool; lifecycle tests compare
		 * both validators, including the length boundary. */
		for(size_t n = 0; ids[i][n]; n++) {
			char ch = ids[i][n];
			if(n >= CONFIG_PROFILE_ID_CAP - 1
			   || !((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
					|| ch == '_'))
				return CONFIG_PROFILES_BAD_KEY;
		}
	}
	if(strcmp(ids[0], ids[1]) == 0)
		return CONFIG_PROFILES_DUPLICATE;
	for(unsigned i = 0; i < 2; i++)
		memcpy(out[i], ids[i], strlen(ids[i]) + 1);
	return CONFIG_PROFILES_OK;
}

struct config_g9_pacing config_read_g9_pacing(struct config* c) {
	assert(c && c->root);
	JSON_Object* object = json_object(c->root);
	const char* key = "experimental.g9_main_pacing_int";
	/* REL-10 (maintainer ruling 2026-10-17): whole-tick MAIN pacing ships ON.
	 * An ABSENT key reads preset 1; an explicit 0 turns it off, and every
	 * provided-but-invalid value (wrong type, non-integer, out of range)
	 * keeps the OFF safety policy. docs/g9_main_pacing.md, docs/deviations.md. */
	/* REL-10-slice (T2, SHIPPED default, maintainer ruling 2026-10-21): the
	 * preset-1 slice is 1000 us (sleep 1 ms and cap 8 ms unchanged). The only
	 * opt-out is experimental.g9_main_slice_1ms_int exactly numeric 0, which
	 * restores 2000. Absent, 1 and every invalid value (string, boolean, 2, -1,
	 * 1.5, ...) give 1000. Preset 0 stays OFF and preset 2 is unchanged.
	 * docs/g9_main_pacing.md, docs/deviations.md. */
	const bool slice_2000 = json_object_dothas_value_of_type(object,
		"experimental.g9_main_slice_1ms_int", JSONNumber)
		&& json_object_dotget_number(object, "experimental.g9_main_slice_1ms_int") == 0;
	const int slice_us = slice_2000 ? 2000 : 1000;
	if(!json_object_dothas_value(object, key)) return (struct config_g9_pacing){1, slice_us, 1, 8};
	const double value = json_object_dothas_value_of_type(object, key, JSONNumber)
		? json_object_dotget_number(object, key) : 0;
	const int preset = value == 1 ? 1 : value == 2 ? 2 : 0;
	return (struct config_g9_pacing){preset, preset == 2 ? 500 : preset == 1 ? slice_us : 2000,
		1, preset == 2 ? 16 : 8};
}

static int config_g9_bounded_int(JSON_Object* object, const char* key,
	int fallback, int lo, int hi) {
	if(!json_object_dothas_value_of_type(object, key, JSONNumber)) return fallback;
	double value = json_object_dotget_number(object, key);
	return value >= lo && value <= hi && value == (int)value ? (int)value : fallback;
}
struct config_g9_lease config_read_g9_lease(struct config* c) {
	assert(c && c->root);
	JSON_Object* object = json_object(c->root);
	return (struct config_g9_lease){
		.enabled = config_g9_bounded_int(object,
			"experimental.g9_priority_lease_int", 0, 0, 1) == 1,
		.main_handoff = config_g9_bounded_int(object,
			"experimental.g9_main_handoff_int", 0, 0, 1) == 1,
		.max_us = config_g9_bounded_int(object,
			"experimental.g9_priority_lease_us_int", 8000, 500, 8000),
		.service_us = config_g9_bounded_int(object,
			"experimental.g9_priority_service_us_int", 500, 100, 2000)};
}

struct config_g9_tick_boost config_read_g9_tick_boost(struct config* c) {
	assert(c && c->root);
	JSON_Object* object = json_object(c->root);
	return (struct config_g9_tick_boost){
		.enabled = config_g9_bounded_int(object,
			"experimental.g9_tick_boost_int", 0, 0, 1) == 1,
		.cap_ms = config_g9_bounded_int(object,
			"experimental.g9_tick_boost_cap_ms_int", 60, 5, 200)};
}

bool config_g9_settled_light_reuse(struct config* c) {
	return c && c->root && config_g9_bounded_int(json_object(c->root),
		"experimental.g9_settled_light_reuse_int", 0, 0, 1) == 1;
}

bool config_g9_save_io(struct config* c) {
	return c && c->root && config_g9_bounded_int(json_object(c->root),
		"experimental.g9_save_io_int", 0, 0, 1) == 1;
}

int config_g9_save_zlib_level(struct config* c) {
	return c && c->root ? config_g9_bounded_int(json_object(c->root),
		"experimental.g9_save_zlib_level_int", 6, 1, 6) : 6;
}

bool config_mob_attack_windup_cue(const struct config* c) {
	return c && c->root && json_object_dotget_boolean(
		json_value_get_object(c->root), "ui.mob_attack_windup_cue") == 1;
}

static char* config_sibling_path(const char* filename, const char* suffix) {
	size_t length = strlen(filename), extra = strlen(suffix) + 1;
	if(length > (size_t)-1 - extra) return NULL;
	char* path = malloc(length + extra);
	if(path) {
		memcpy(path, filename, length);
		memcpy(path + length, suffix, extra);
	}
	return path;
}

/* libfat refuses rename-over-existing. Retain the previous complete file
 * across rotation instead of unlinking it. This is recoverable replacement,
 * not a guarantee against torn FAT metadata or power loss during SD writes.
 * Configuration writes are main-thread only, like the existing callers. */
/* Every failure exit of config_save_tree names its step, path and errno on
 * stderr (the game's log on PC and the Wii telemetry/console log), so a
 * "Could not save" line on screen always has a cause in the log. There is no
 * project-wide log macro; config.c logs with fprintf like its other paths. */
#define CONFIG_SAVE_LOG(step, path)                                          \
	fprintf(stderr, "config_save %s: %s failed on %s: errno %d (%s)\n",      \
			filename, (step), (path), errno, strerror(errno))

static bool config_save_tree(JSON_Value* root, const char* filename) {
	char* temporary = config_sibling_path(filename, ".cw-tmp");
	char* backup = config_sibling_path(filename, ".cw-backup");
	bool ok = false, rotated = false, unverified_backup = false;
	struct stat st;
	if(!temporary || !backup) {
		errno = ENOMEM;
		CONFIG_SAVE_LOG("sibling path alloc", filename);
		goto done;
	}
	if(stat(filename, &st) == 0) {
		if(!S_ISREG(st.st_mode)) {
			errno = EISDIR;
			CONFIG_SAVE_LOG("main is not a regular file", filename);
			goto done;
		}
		/* If startup recovered the backup beside an invalid main file,
		 * that backup is still the previous committed copy. Do not rotate
		 * malformed main bytes over it on the next save attempt. */
		if(stat(backup, &st) == 0) {
			JSON_Value* main_value = json_parse_file(filename);
			bool valid = main_value && json_value_get_type(main_value) == JSONObject;
			json_value_free(main_value);
			if(!valid) {
				JSON_Value* old_value = json_parse_file(backup);
				bool recoverable = old_value && json_value_get_type(old_value) == JSONObject;
				json_value_free(old_value);
				if(!recoverable) {
					/* R9: json_parse_file also returns NULL on a transient
					 * read/allocation failure, so "invalid" may be the only
					 * valid copy. Keep it: the normal path below removes the
					 * backup only after the new temporary file is written
					 * and verified, so a failed write leaves it intact. */
					unverified_backup = true;
					fprintf(stderr, "config_save %s: main and backup both "
						"unreadable; saving in-memory settings (backup %s kept "
						"until the new copy is written)\n", filename, backup);
				} else {
					if(remove(filename) != 0) {
						CONFIG_SAVE_LOG("remove invalid main", filename);
						goto done;
					}
					if(rename(backup, filename) != 0) {
						CONFIG_SAVE_LOG("restore backup over invalid main", backup);
						goto done;
					}
				}
			}
		} else if(errno != ENOENT) {
			CONFIG_SAVE_LOG("stat backup", backup);
			goto done;
		}
	} else if(errno != ENOENT) {
		CONFIG_SAVE_LOG("stat main", filename);
		goto done;
	} else if(stat(backup, &st) == 0) {
		/* An earlier promotion and rollback both failed. Restore that old
		 * copy before allowing a new transaction to replace its backup. */
		if(!S_ISREG(st.st_mode)) {
			errno = EISDIR;
			CONFIG_SAVE_LOG("backup is not a regular file", backup);
			goto done;
		}
		if(rename(backup, filename) != 0) {
			CONFIG_SAVE_LOG("restore orphaned backup", backup);
			goto done;
		}
	} else if(errno != ENOENT) {
		CONFIG_SAVE_LOG("stat backup", backup);
		goto done;
	}
	if(json_serialize_to_file_pretty(root, temporary) != JSONSuccess) {
		CONFIG_SAVE_LOG("write temporary", temporary);
		remove(temporary);
		goto done;
	}
	if(unverified_backup) {
		/* The backup is about to be replaced: prove the new copy reads back
		 * as a settings object first. */
		JSON_Value* written = json_parse_file(temporary);
		bool readable = written && json_value_get_type(written) == JSONObject;
		json_value_free(written);
		if(!readable) {
			errno = EIO;
			CONFIG_SAVE_LOG("verify temporary", temporary);
			goto discard;
		}
	}
	if(stat(filename, &st) == 0) {
		if(remove(backup) != 0 && errno != ENOENT) {
			CONFIG_SAVE_LOG("remove old backup", backup);
			goto discard;
		}
		if(rename(filename, backup) != 0) {
			CONFIG_SAVE_LOG("rotate main to backup", filename);
			goto discard;
		}
		rotated = true;
	} else if(errno != ENOENT) {
		CONFIG_SAVE_LOG("stat main before promote", filename);
		goto discard;
	}
	if(rename(temporary, filename) != 0) {
		CONFIG_SAVE_LOG("promote temporary", temporary);
		if(rotated && rename(backup, filename) != 0)
			CONFIG_SAVE_LOG("roll back backup to main", backup);
		goto discard;
	}
	ok = true;
	/* If cleanup fails, the main file remains the committed authority; the
	 * stale backup is re-checked and reused by the next save (above). */
	if(rotated && remove(backup) != 0)
		CONFIG_SAVE_LOG("remove backup after commit (main still committed)",
						backup);
	goto done;
discard:
	remove(temporary);
done:
	free(temporary);
	free(backup);
	return ok;
}

bool config_create(struct config* c, const char* filename) {
	assert(c && filename);
	// config_create is the de facto initializer for struct config: every
	// caller in the tree declares `struct config c;` on the bare stack (no
	// `= {0}`) and trusts this call to fully populate it -- true before
	// ephemeral tracking existed (the only field was `root`, set
	// unconditionally below), broken the moment CONFIG_MAX_EPHEMERAL_KEYS
	// fields joined the struct without a matching reset here. Leaving
	// ephemeral_key_count uninitialised turned config_save() into a wild
	// read: garbage stack bytes as a key COUNT, then garbage pointers off
	// ephemeral_keys[] passed to json_object_dotremove/strcmp -- exactly what
	// segfaulted test_texture_packs and test_options_env (neither zero-inits
	// its local `struct config`, and neither should have to; that convention
	// predates this fix). Reset FIRST and unconditionally, regardless of the
	// parse outcome below, so every exit path leaves a consistent, safe 0.
	c->ephemeral_key_count = 0;
	c->root = json_parse_file(filename);
	if(!c->root || json_value_get_type(c->root) != JSONObject) {
		char* backup = config_sibling_path(filename, ".cw-backup");
		JSON_Value* recovered = backup ? json_parse_file(backup) : NULL;
		free(backup);
		if(recovered && json_value_get_type(recovered) == JSONObject) {
			json_value_free(c->root);
			c->root = recovered;
		} else json_value_free(recovered);
	}

	if(!c->root)
		return false;

	if(json_value_get_type(c->root) != JSONObject) {
		config_destroy(c);
		return false;
	}

	return true;
}

bool config_load_user_or_default(struct config* c, const char* user_filename,
								 const char* default_filename, FILE* log) {
	assert(c && user_filename && default_filename);

	if(config_create(c, user_filename))
		return true;

	if(log)
		fprintf(log, "Failed to load config %s; trying default %s\n",
				user_filename, default_filename);

	if(config_create(c, default_filename))
		return true;

	if(log)
		fprintf(log, "Failed to load default config %s; aborting\n",
				default_filename);
	return false;
}

const char* config_read_string(struct config* c, const char* key,
							   const char* fallback) {
	assert(c && key);
	// TODO: only give out copy
	const char* res = json_object_dotget_string(json_object(c->root), key);
	return res ? res : fallback;
}

const char* config_read_path(struct config* c, const char* key,
							 const char* fallback) {
	const char* res = config_read_string(c, key, fallback);
#ifdef PLATFORM_WII
	// ponytail: one static buffer -- every consumer is main-thread and uses
	// the result immediately (scan/fopen); make it caller-supplied if a
	// second concurrent reader ever appears.
	static char buf[256];
	res = path_from_sd_root(res, buf, sizeof(buf));
#endif
	return res;
}

bool config_read_int_array(struct config* c, const char* key, int* dest,
						   size_t* length) {
	assert(c && key && dest);

	JSON_Array* entry = json_object_dotget_array(json_object(c->root), key);

	if(!entry)
		return false;

	*length = *length < json_array_get_count(entry) ?
		*length :
		json_array_get_count(entry);

	for(size_t k = 0; k < *length; k++)
		dest[k] = json_array_get_number(entry, k);

	return true;
}

int config_read_int(struct config* c, const char* key, int fallback) {
	assert(c && key);

	// dotget_number returns 0 on miss, so check presence+type first to keep a
	// real 0 distinct from "key absent" (use the fallback).
	if(json_object_dothas_value_of_type(json_object(c->root), key, JSONNumber))
		return (int)json_object_dotget_number(json_object(c->root), key);

	return fallback;
}

bool config_set_string(struct config* c, const char* key, const char* value) {
	assert(c && key && value);
	// dotset creates intermediate objects as needed (e.g. "paths.texturepack").
	return json_object_dotset_string(json_object(c->root), key, value)
		== JSONSuccess;
}

bool config_set_int(struct config* c, const char* key, int value) {
	assert(c && key);
	return json_object_dotset_number(json_object(c->root), key, value)
		== JSONSuccess;
}

/* next-gaps-0924 W1F. Beta persists GameSettings.difficulty as options.txt
 * "difficulty:<n>" (GameSettings.java:234-235,287); CaveWii's options store is
 * config.json, key DIFFICULTY_CONFIG_KEY. Absent, non-integral or out-of-range
 * values read as the Beta default (Normal, GameSettings.java:41) instead of
 * config_read_int's raw (int) cast of an arbitrary JSON double. */
int config_read_difficulty(struct config* c) {
	assert(c);
	JSON_Object* root = json_object(c->root);
	if(!json_object_dothas_value_of_type(root, DIFFICULTY_CONFIG_KEY,
										 JSONNumber))
		return DIFFICULTY_DEFAULT;
	double v = json_object_dotget_number(root, DIFFICULTY_CONFIG_KEY);
	if(!(v >= (double)DIFFICULTY_PEACEFUL && v <= (double)DIFFICULTY_HARD)
	   || v != (double)(int)v)
		return DIFFICULTY_DEFAULT;
	return (int)v;
}

bool config_set_difficulty(struct config* c, int difficulty) {
	assert(c);
	return config_set_int(c, DIFFICULTY_CONFIG_KEY,
						  difficulty_sanitize(difficulty));
}

void config_mark_ephemeral(struct config* c, const char* key) {
	assert(c && key);
	// Idempotent: two hw_run flags can route through the same key in one boot
	// (e.g. both autopilot_walk and autopilot_aim set "debug.autopilot") --
	// don't burn a second slot on a key already marked.
	for(size_t i = 0; i < c->ephemeral_key_count; i++) {
		if(strcmp(c->ephemeral_keys[i], key) == 0)
			return;
	}
	// Never silently drop a mark: running out of slots and returning quietly
	// would let an unmarked hw_run key slip onto disk unstripped -- exactly
	// the bug this exists to close. Grow CONFIG_MAX_EPHEMERAL_KEYS instead.
	assert(c->ephemeral_key_count < CONFIG_MAX_EPHEMERAL_KEYS
		   && "config: CONFIG_MAX_EPHEMERAL_KEYS too small for the hw_run "
			  "injection block -- grow it (config.h)");
	if(c->ephemeral_key_count >= CONFIG_MAX_EPHEMERAL_KEYS)
		return;
	c->ephemeral_keys[c->ephemeral_key_count++] = key;
}

bool config_save(struct config* c, const char* filename) {
	assert(c && filename);

	if(c->ephemeral_key_count == 0)
		return config_save_tree(c->root, filename);

	// hw_run injected ephemeral keys into the in-memory tree THIS PROCESS
	// (main.c's hw_run config-injection block, each call paired with
	// config_mark_ephemeral -- see config.h). Strip exactly those from a
	// SCRATCH COPY before writing: the in-memory tree -- and therefore this
	// process's own runtime behaviour -- is untouched, but the on-disk
	// config.json never gains a capture-only ephemeral like "autoload":"+new"
	// just because an Options visit happened to call config_save() during an
	// hw-run-armed session (screen_options.c's options_commit; the same
	// scratch-copy path also protects texturepacks.c's config_save call).
	JSON_Value* scratch = json_value_deep_copy(c->root);
	if(!scratch)
		// Fail rather than silently fall back to writing the un-stripped
		// tree: that would defeat the whole point of this function on the
		// one path (OOM) where it matters most. Callers already handle a
		// false return (options_commit's save-failed path, texturepacks.c).
		return false;

	JSON_Object* scratch_obj = json_object(scratch);
	for(size_t i = 0; i < c->ephemeral_key_count; i++)
		json_object_dotremove(scratch_obj, c->ephemeral_keys[i]);

	bool ok = config_save_tree(scratch, filename);
	json_value_free(scratch);
	return ok;
}

bool config_write_server_list(struct config* c,
							  const struct config_mp_server* list,
							  size_t count) {
	assert(c && (list || count == 0));

	JSON_Value* arr_val = json_value_init_array();
	if(!arr_val)
		return false;
	JSON_Array* arr = json_value_get_array(arr_val);

	for(size_t i = 0; i < count; i++) {
		JSON_Value* row_val = json_value_init_object();
		if(!row_val) {
			json_value_free(arr_val);
			return false;
		}
		JSON_Object* row = json_value_get_object(row_val);
		// Plain (non-dot) setters: "name"/"address" are fixed literal keys,
		// never derived from the stored VALUE, so there is no dot-path
		// ambiguity even though an address value routinely contains ':'.
		// Both results are checked (audit finding 3): an unchecked failure
		// here (e.g. invalid UTF-8 in a hand-edited/hostile field -- parson's
		// json_value_init_string_with_len rejects it) would silently commit
		// a PARTIAL row (a name with no address, or vice versa) to disk.
		// Fail the whole write instead -- `arr_val`/`c->root` are untouched
		// either way (arr_val is only merged into c->root at the very end,
		// on success), so this stays atomic: no partial row, no partial
		// list.
		if(json_object_set_string(row, "name", list[i].name) != JSONSuccess
		   || json_object_set_string(row, "address", list[i].address)
			   != JSONSuccess) {
			json_value_free(row_val);
			json_value_free(arr_val);
			return false;
		}
		if(json_array_append_value(arr, row_val) != JSONSuccess) {
			// append failed WITHOUT taking ownership (parson: a failed
			// json_array_append leaves the value untouched) -- free it
			// ourselves or it leaks.
			json_value_free(row_val);
			json_value_free(arr_val);
			return false;
		}
	}

	// json_object_dotset_value takes ownership of arr_val on SUCCESS only;
	// a failure path (e.g. "mp" already exists as a non-object) leaves it
	// unowned, so it must be freed here too.
	if(json_object_dotset_value(json_object(c->root), "mp.servers", arr_val)
	   != JSONSuccess) {
		json_value_free(arr_val);
		return false;
	}
	return true;
}

bool config_read_server_list(struct config* c, struct config_mp_server* out,
							 size_t cap, size_t* count) {
	assert(c && out && count);
	*count = 0;

	JSON_Array* arr
		= json_object_dotget_array(json_object(c->root), "mp.servers");
	if(arr) {
		size_t n = json_array_get_count(arr);
		for(size_t i = 0; i < n && *count < cap; i++) {
			JSON_Object* row = json_array_get_object(arr, i);
			if(!row)
				continue; // hostile/hand-edited entry -- skip, don't crash
			const char* name = json_object_get_string(row, "name");
			const char* addr = json_object_get_string(row, "address");
			snprintf(out[*count].name, sizeof(out[*count].name), "%s",
					name ? name : "");
			snprintf(out[*count].address, sizeof(out[*count].address), "%s",
					addr ? addr : "");
			(*count)++;
		}
		return true;
	}

	// MIGRATION (see config.h): no mp.servers array yet. A configured
	// mp.server scalar -- the WHOLE contract screen_multiplayer_connect.c's
	// mpc_begin_connect predates this slice with -- becomes servers[0], and
	// the synthesized array is written straight into the live tree so it
	// self-heals on the very next config_save, no separate migration step
	// to forget.
	const char* legacy
		= json_object_dotget_string(json_object(c->root), "mp.server");
	if(legacy && legacy[0] && cap > 0) {
		struct config_mp_server seed;
		snprintf(seed.name, sizeof(seed.name), "Default");
		snprintf(seed.address, sizeof(seed.address), "%s", legacy);
		if(config_write_server_list(c, &seed, 1)) {
			out[0] = seed;
			*count = 1;
		}
	}
	return true;
}

void config_destroy(struct config* c) {
	assert(c && c->root);
	json_value_free(c->root);
}
