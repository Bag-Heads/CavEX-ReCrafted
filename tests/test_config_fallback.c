#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "config.h"
#include "platform/displaylist_stats.h"

static bool fail_write;
static int rename_calls, fail_rename_first, fail_rename_last;
JSON_Status __real_json_serialize_to_file_pretty(const JSON_Value*, const char*);
int __real_rename(const char*, const char*);
JSON_Status __wrap_json_serialize_to_file_pretty(const JSON_Value* value,
												const char* path) {
	if(!fail_write) return __real_json_serialize_to_file_pretty(value, path);
	FILE* file = fopen(path, "wb");
	assert(file);
	assert(fputs("{partial", file) >= 0);
	assert(fclose(file) == 0);
	return JSONFailure;
}
int __wrap_rename(const char* old_path, const char* new_path) {
	rename_calls++;
	if(rename_calls >= fail_rename_first && rename_calls <= fail_rename_last) {
		errno = EIO;
		return -1;
	}
	return __real_rename(old_path, new_path);
}

static void test_save_recovery(void) {
	char path[128], backup[160], temporary[160];
	test_fixture_dir("test_config_recovery.json", path, sizeof(path));
	snprintf(backup, sizeof(backup), "%s.cw-backup", path);
	snprintf(temporary, sizeof(temporary), "%s.cw-tmp", path);
	remove(path); remove(backup); remove(temporary);
	struct config c = {0}, loaded;
	c.root = json_parse_string("{\"controls\":{\"look_sensitivity\":3}}");
	assert(config_save(&c, path));
	assert(config_set_int(&c, "controls.look_sensitivity", 5));
	/* A serializer that has already truncated and partially written its
	 * output must leave the old user configuration intact. */
	fail_write = true;
	assert(!config_save(&c, path));
	fail_write = false;
	assert(config_create(&loaded, path));
	assert(config_read_int(&loaded, "controls.look_sensitivity", -1) == 3);
	config_destroy(&loaded);
	for(int last = 2; last <= 3; last++) {
		rename_calls = 0; fail_rename_first = 2; fail_rename_last = last;
		assert(!config_save(&c, path)); /* promote, optionally rollback fail */
		fail_rename_first = fail_rename_last = 0;
		assert(config_create(&loaded, path));
		assert(config_read_int(&loaded, "controls.look_sensitivity", -1) == 3);
		config_destroy(&loaded);
	}
	assert(config_save(&c, path)); /* repairs missing main from backup first */
	assert(config_create(&loaded, path));
	assert(config_read_int(&loaded, "controls.look_sensitivity", -1) == 5);
	config_destroy(&loaded);
	/* A torn main beside a valid backup must not replace that backup with
	 * torn bytes when the subsequent promotion also fails. */
	assert(rename(path, backup) == 0);
	FILE* broken = fopen(path, "wb");
	assert(broken && fputs("{broken", broken) >= 0 && fclose(broken) == 0);
	assert(config_create(&loaded, path));
	assert(config_read_int(&loaded, "controls.look_sensitivity", -1) == 5);
	config_destroy(&loaded);
	rename_calls = 0; fail_rename_first = 3; fail_rename_last = 4;
	assert(!config_save(&c, path));
	fail_rename_first = fail_rename_last = 0;
	assert(config_create(&loaded, path));
	assert(config_read_int(&loaded, "controls.look_sensitivity", -1) == 5);
	config_destroy(&loaded);
	assert(config_save(&c, path));
	struct stat st;
	assert(stat(backup, &st) != 0 && stat(temporary, &st) != 0);
	/* Neither on-disk copy is recoverable; valid memory must still save. */
	broken = fopen(path, "wb");
	assert(broken && fputs("{broken", broken) >= 0 && fclose(broken) == 0);
	broken = fopen(backup, "wb");
	assert(broken && fputs("{also broken", broken) >= 0 && fclose(broken) == 0);
	/* R9: an unreadable backup may only have failed a transient read, so a
	 * save whose new copy cannot be written must leave it exactly as it was
	 * (it is never removed before the replacement exists). */
	fail_write = true;
	assert(!config_save(&c, path));
	fail_write = false;
	{
		char kept[32] = {0};
		FILE* f = fopen(backup, "rb");
		assert(f && "the backup survives a failed write");
		size_t n = fread(kept, 1, sizeof kept - 1, f);
		assert(fclose(f) == 0);
		assert(n == strlen("{also broken") && !strcmp(kept, "{also broken"));
		assert(stat(temporary, &st) != 0);
	}
	assert(config_save(&c, path));
	assert(config_create(&loaded, path));
	assert(config_read_int(&loaded, "controls.look_sensitivity", -1) == 5);
	config_destroy(&loaded);
	assert(stat(backup, &st) != 0 && stat(temporary, &st) != 0);
	assert(config_save(&c, path)); /* later saves remain usable */
	assert(remove(path) == 0);
	config_destroy(&c);
	puts("PASS config recovery: partial write, failed promotion/rollback, reload and subsequent save");
}

static void test_fallback_to_default(void) {
	struct config config;
	FILE* log = tmpfile();
	assert(log);

	bool ok = config_load_user_or_default(&config,
										  "missing-user-config.json",
										  "missing-default-config.json", log);
	assert(!ok);

	fflush(log);
	rewind(log);

	char buffer[256];
	size_t read = fread(buffer, 1, sizeof(buffer) - 1, log);
	buffer[read] = '\0';
	fclose(log);

	assert(strstr(buffer, "Failed to load config missing-user-config.json"));
	assert(strstr(buffer,
				  "Failed to load default config missing-default-config.json"));
}

/* ---- (c): the autoload "+new" sentinel leak, layer 3 --------------------
 * config_save() must strip exactly the keys hw_run marked ephemeral on THIS
 * config (main.c's hw_run_config_set_string/hw_run_config_set_int pairing)
 * from the ON-DISK write, while a key the user set by hand through Options
 * (never marked) survives -- the "amplifier" fix: options_commit's
 * config_save("config.json") must not permanently bake a capture-only
 * "autoload":"+new" (or any other hw_run ephemeral) into config.json just
 * because an Options visit happened during an hw-run-armed session. */
static void test_ephemeral_keys_stripped_on_save(void) {
	struct config c = {0};
	c.root = json_value_init_object();
	assert(c.root);

	// Mirrors main.c's hw_run_config_set_string pairing exactly: set, then
	// mark. Two keys, not one, so the strip isn't proven on a
	// single-key coincidence -- "autoload" plus a debug.* key, matching the
	// shape of main.c's real injection block.
	assert(config_set_string(&c, "autoload", "+new"));
	config_mark_ephemeral(&c, "autoload");
	assert(config_set_string(&c, "debug.overlay", "force"));
	config_mark_ephemeral(&c, "debug.overlay");

	// A key the USER set by hand through Options -- never marked ephemeral --
	// must survive exactly like it does today.
	assert(config_set_string(&c, "paths.texturepack",
							 "texturepacks/beta-1.7.3"));

	char path[128];
	test_fixture_dir("test_config_ephemeral_tmp.json", path, sizeof path);
	remove(path);
	assert(config_save(&c, path));
	config_destroy(&c);

	struct config reloaded;
	assert(config_create(&reloaded, path));
	assert(strcmp(config_read_string(&reloaded, "autoload", "ABSENT"), "ABSENT")
			   == 0
		   && "hw_run-injected ephemeral key leaked onto disk");
	assert(strcmp(config_read_string(&reloaded, "debug.overlay", "ABSENT"),
				  "ABSENT")
			   == 0
		   && "a second ephemeral key leaked onto disk");
	assert(strcmp(config_read_string(&reloaded, "paths.texturepack", "MISSING"),
				  "texturepacks/beta-1.7.3")
			   == 0
		   && "a user-set key was incorrectly stripped alongside the "
			  "ephemeral ones");
	config_destroy(&reloaded);
	remove(path);
	fprintf(stderr,
			"(c) ephemeral keys stripped on save, user key survives -- OK\n");
}

// The common case (no hw_run this process, ephemeral_key_count == 0):
// config_save must behave EXACTLY as before -- no scratch copy, nothing
// stripped. In particular an "autoload" key the user saved themselves (e.g.
// resuming a specific world by directory name) must never be treated as
// ephemeral just because the key name matches -- only an explicit
// config_mark_ephemeral() call marks a key, never the key's name or value.
static void test_non_ephemeral_save_is_unaffected(void) {
	struct config c = {0};
	c.root = json_value_init_object();
	assert(c.root);
	assert(config_set_string(&c, "autoload", "some-real-world"));

	char path[128];
	test_fixture_dir("test_config_no_ephemeral_tmp.json", path, sizeof path);
	remove(path);
	assert(config_save(&c, path));
	config_destroy(&c);

	struct config reloaded;
	assert(config_create(&reloaded, path));
	assert(strcmp(config_read_string(&reloaded, "autoload", "ABSENT"),
				  "some-real-world")
		   == 0
		   && "an UNMARKED autoload key (e.g. a user's own saved world name) "
			  "must never be stripped");
	config_destroy(&reloaded);
	remove(path);
	fprintf(stderr, "(c) non-ephemeral save is unaffected -- OK\n");
}

// --cw-set memory.displaylist_compact=1 (run-scoped hardware-run override,
// 2026-09-08, lane/carve-observer-boot -- hw_run.h's cw_set field comment).
// Mirrors main.c's exact override sequence: config_set_int + config_mark_
// ephemeral (the same pair hw_run_config_set_int wraps), THEN
// displaylist_set_compact(config_read_int(...) == 1) reads the OVERRIDDEN
// value -- proving the earliest-reader-vs-apply-order fix actually landed,
// not just that the config key changed. Then config_save() must NOT
// persist the override (the GB18 "+new"-sentinel-leak class of bug) --
// reloading from disk must see the key absent/reverted, even though THIS
// process's live displaylist_compact_enabled() stays flipped.
static void test_cw_set_displaylist_override_not_persisted(void) {
	struct config c = {0};
	c.root = json_value_init_object();
	assert(c.root);

	// Baseline: absent key reads fallback 0 -> compaction starts OFF, same
	// as main.c's config_load_ok-adjacent displaylist_set_compact call on
	// an un-overridden boot.
	displaylist_set_compact(
		config_read_int(&c, "memory.displaylist_compact", 0) == 1);
	assert(!displaylist_compact_enabled());

	// The --cw-set override lands as one config_set_int + config_mark_
	// ephemeral pair (main.c's hw_run_config_set_int wrapper), applied
	// BEFORE the (re-)read that flips displaylist_set_compact -- exactly
	// main.c's ordering fix (config_load_ok, then cw_set overrides, THEN
	// the "memory.displaylist_compact" read).
	assert(config_set_int(&c, "memory.displaylist_compact", 1));
	config_mark_ephemeral(&c, "memory.displaylist_compact");
	displaylist_set_compact(
		config_read_int(&c, "memory.displaylist_compact", 0) == 1);
	assert(displaylist_compact_enabled()
		   && "the --cw-set override must change displaylist_compact_enabled()");

	char path[128];
	test_fixture_dir("test_config_cw_set_displaylist_tmp.json", path,
					 sizeof path);
	remove(path);
	assert(config_save(&c, path));
	config_destroy(&c);

	struct config reloaded;
	assert(config_create(&reloaded, path));
	assert(config_read_int(&reloaded, "memory.displaylist_compact", 0) == 0
		   && "a --cw-set override must be run-scoped only -- it must never "
			  "survive into the saved config.json");
	config_destroy(&reloaded);
	remove(path);
	fprintf(stderr,
			"(d) --cw-set displaylist_compact override applies live and is "
			"NOT persisted by config_save -- OK\n");
}

/* REL-10-tests: the maintainer's 2026-10-17 ruling ships whole-tick MAIN
 * pacing ON -- an ABSENT experimental.g9_main_pacing_int reads preset 1
 * (1000-us slice since the 2026-10-21 REL-10-slice ruling, 1-ms sleep, 8-ms
 * frame cap). Explicit 0 stays OFF and every
 * provided-but-invalid value keeps the existing OFF safety policy. A missed
 * default is a controlled "REL-10-tests RED:" line + exit 1, not an abort. */
static int pacing_default_red;
static void expect_pacing_default_on(struct config* c, const char* what) {
	struct config_g9_pacing p = config_read_g9_pacing(c);
	if(p.preset == 1 && p.slice_us == 1000 && p.sleep_ms == 1 && p.cap_ms == 8)
		return;
	printf("REL-10-tests RED: g9 pacing default (%s): preset %d slice %d sleep %d "
		   "cap %d; want default ON preset 1 = 1/1000/1/8\n",
		   what, p.preset, p.slice_us, p.sleep_ms, p.cap_ms);
	pacing_default_red++;
}
static void test_g9_pacing_presets(void) {
	struct config c = {0}; c.root = json_value_init_object(); assert(c.root);
	expect_pacing_default_on(&c, "key absent, empty config");
	assert(config_set_int(&c, "experimental.g9_main_pacing_typo", 2));
	expect_pacing_default_on(&c, "key absent, sibling typo key present");
	const int values[] = {0, 1, 2, -1, 3};
	for(unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
		assert(config_set_int(&c, "experimental.g9_main_pacing_int", values[i]));
		struct config_g9_pacing p = config_read_g9_pacing(&c);
		int expected = values[i] == 1 || values[i] == 2 ? values[i] : 0;
		assert(p.preset == expected && p.sleep_ms == 1);
		assert(p.slice_us == (expected == 2 ? 500 : expected == 1 ? 1000 : 2000));
		assert(p.cap_ms == (expected == 2 ? 16 : 8));
	}
	assert(json_object_dotset_number(json_object(c.root),
		"experimental.g9_main_pacing_int", 2.5) == JSONSuccess);
	assert(config_read_g9_pacing(&c).preset == 0);
	assert(json_object_dotset_number(json_object(c.root),
		"experimental.g9_main_pacing_int", 1e100) == JSONSuccess);
	assert(config_read_g9_pacing(&c).preset == 0);
	assert(config_set_string(&c, "experimental.g9_main_pacing_int", "2"));
	assert(config_read_g9_pacing(&c).preset == 0);
	config_destroy(&c);
}

/* REL-10-slice (shipped default 1000 us, maintainer ruling 2026-10-21): the
 * only opt-out is the exact number 0, which restores 2000. Absent, 1 and every
 * invalid value give 1000. Preset 0 stays OFF and preset 2 is unchanged; the
 * key is ignored for them. */
static void expect_pacing(struct config* c, int preset, int slice, int cap, const char* what) {
	struct config_g9_pacing p = config_read_g9_pacing(c);
	if(p.preset != preset || p.slice_us != slice || p.sleep_ms != 1 || p.cap_ms != cap) {
		printf("REL-10-slice RED: %s: preset %d slice %d sleep %d cap %d; want %d/%d/1/%d\n",
			   what, p.preset, p.slice_us, p.sleep_ms, p.cap_ms, preset, slice, cap);
		pacing_default_red++;
	}
}
static void test_g9_pacing_slice_1ms(void) {
	const char* const opt = "experimental.g9_main_slice_1ms_int";
	const char* const pre = "experimental.g9_main_pacing_int";
	struct config c = {0}; c.root = json_value_init_object(); assert(c.root);
	expect_pacing(&c, 1, 1000, 8, "both keys absent");
	assert(config_set_int(&c, opt, 1));
	expect_pacing(&c, 1, 1000, 8, "slice key 1, preset absent");
	assert(config_set_int(&c, opt, 0));
	expect_pacing(&c, 1, 2000, 8, "opt-out 0, preset absent (effective 1)");
	assert(config_set_int(&c, pre, 1));
	expect_pacing(&c, 1, 2000, 8, "opt-out 0, preset 1");
	assert(config_set_int(&c, pre, 0));
	expect_pacing(&c, 0, 2000, 8, "opt-out 0, preset 0 stays OFF");
	assert(config_set_int(&c, pre, 2));
	expect_pacing(&c, 2, 500, 16, "opt-out 0, preset 2 unchanged");
	assert(config_set_int(&c, pre, 3));
	expect_pacing(&c, 0, 2000, 8, "opt-out 0, invalid preset stays OFF");
	assert(config_set_int(&c, opt, 1));
	expect_pacing(&c, 0, 2000, 8, "slice key 1, invalid preset stays OFF");
	assert(config_set_int(&c, pre, 1));
	expect_pacing(&c, 1, 1000, 8, "slice key 1, preset 1");
	const double bad[] = {2, -1, 1.5, 0.999, -0.5, 1e100};
	for(unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		assert(json_object_dotset_number(json_object(c.root), opt, bad[i]) == JSONSuccess);
		expect_pacing(&c, 1, 1000, 8, "invalid numeric slice key keeps 1000");
	}
	assert(config_set_string(&c, opt, "0"));
	expect_pacing(&c, 1, 1000, 8, "string 0 is not the opt-out");
	assert(json_object_dotset_boolean(json_object(c.root), opt, 0) == JSONSuccess);
	expect_pacing(&c, 1, 1000, 8, "boolean false is not the opt-out");
	config_destroy(&c);
}

/* ---- REL-01a-tests: a missing/corrupt user config.json reaches the
 * shipped config_wii.json defaults through the SAME loader main.c's startup
 * calls (config_load_user_or_default("config.json", default_config, ...)).
 * Each case reports a case-specific "REL-01a-tests RED:" line and the
 * process exits 1 instead of asserting, so a regression reads as a named
 * failure. --fallback-disabled-mutant suppresses the loader's result at
 * this test boundary (production source untouched) to prove the checks are
 * sensitive: every case must then go RED. */
#ifndef CW_TEST_CONFIG_WII_JSON
#error "CW_TEST_CONFIG_WII_JSON must name the shipped config_wii.json"
#endif

static bool fallback_mutant;
static int fallback_failures;

static void fallback_red(const char* name, const char* why) {
	printf("REL-01a-tests RED: %s: %s\n", name, why);
	fflush(stdout);
	fallback_failures++;
}

static bool fallback_boundary_load(struct config* c, const char* user,
								   const char* def, FILE* log) {
	bool ok = config_load_user_or_default(c, user, def, log);
	if(fallback_mutant) {
		if(ok) config_destroy(c);
		c->root = NULL;
		return false;
	}
	return ok;
}

static bool fallback_write_file(const char* path, const char* bytes,
								size_t length) {
	FILE* file = fopen(path, "wb");
	if(!file) return false;
	bool ok = fwrite(bytes, 1, length, file) == length;
	return fclose(file) == 0 && ok;
}

static char* fallback_slurp(const char* path, size_t* length) {
	FILE* file = fopen(path, "rb");
	if(!file) return NULL;
	char* data = NULL;
	if(fseek(file, 0, SEEK_END) == 0) {
		long size = ftell(file);
		if(size >= 0 && fseek(file, 0, SEEK_SET) == 0) {
			data = malloc((size_t)size + 1);
			if(data && fread(data, 1, (size_t)size, file) == (size_t)size) {
				data[size] = '\0';
				*length = (size_t)size;
			} else {
				free(data);
				data = NULL;
			}
		}
	}
	fclose(file);
	return data;
}

/* What the startup path needs from the shipped defaults: the whole tree
 * (the shipped JSON, value-for-value), the asset/world paths, controller
 * bindings and the controls block, plus a working save/reload. */
static void fallback_check_usable(const char* name, struct config* c,
								  const JSON_Value* shipped,
								  const char* save_path) {
	if(!json_value_equals(c->root, shipped)) {
		fallback_red(name, "loaded tree differs from shipped config_wii.json");
		return;
	}
	if(strcmp(config_read_string(c, "paths.texturepack", ""), "assets") != 0)
		fallback_red(name, "paths.texturepack is not the shipped \"assets\"");
	if(strcmp(config_read_string(c, "paths.worlds", ""), "saves") != 0)
		fallback_red(name, "paths.worlds is not the shipped \"saves\"");
	int keys[8];
	size_t count = 8;
	if(!config_read_int_array(c, "input.player_forward", keys, &count)
	   || count != 3 || keys[0] != 0 || keys[1] != 910 || keys[2] != 930)
		fallback_red(name, "input.player_forward bindings missing");
	count = 8;
	if(!config_read_int_array(c, "input.gui_click", keys, &count)
	   || count != 3 || keys[0] != 4 || keys[1] != 204 || keys[2] != 404)
		fallback_red(name, "input.gui_click bindings missing");
	if(config_read_int(c, "controls.wiimote_ir_look", -1) != 0
	   || config_read_int(c, "controls.wiimote_sneak_toggle", -1) != 1
	   || config_read_int(c, "controls.classic_sneak_toggle", -1) != 1
	   || config_read_int(c, "controls.gamecube_sneak_toggle", -1) != 1)
		fallback_red(name, "controls block is not the shipped defaults");
	remove(save_path);
	struct config reloaded;
	if(!config_save(c, save_path)) {
		fallback_red(name, "config_save of the fallback config failed");
	} else if(!config_create(&reloaded, save_path)) {
		fallback_red(name, "saved fallback config does not reload");
	} else {
		if(!json_value_equals(reloaded.root, shipped))
			fallback_red(name, "saved fallback config reloads different");
		config_destroy(&reloaded);
	}
	remove(save_path);
}

struct fallback_case {
	const char* name;
	const char* bytes; /* NULL = no user file at all */
	size_t length;
};

static void test_user_config_reaches_shipped_defaults(void) {
	size_t shipped_length = 0;
	char* shipped_bytes = fallback_slurp(CW_TEST_CONFIG_WII_JSON,
										 &shipped_length);
	JSON_Value* shipped = json_parse_file(CW_TEST_CONFIG_WII_JSON);
	if(!shipped_bytes || !shipped
	   || json_value_get_type(shipped) != JSONObject) {
		fallback_red("setup", "cannot read shipped " CW_TEST_CONFIG_WII_JSON);
		free(shipped_bytes);
		json_value_free(shipped);
		return;
	}
	static const char invalid[] = "this is not json {";
	static const char array_root[] = "[{\"paths\":{\"worlds\":\"saves\"}}]";
	static const char number_root[] = "42";
	const struct fallback_case cases[] = {
		{"absent_file", NULL, 0},
		{"invalid_json", invalid, sizeof invalid - 1},
		/* Half of the real shipped file: a torn write of a valid config. */
		{"truncated_json", shipped_bytes, shipped_length / 2},
		{"empty_file", "", 0},
		{"non_object_root_array", array_root, sizeof array_root - 1},
		{"non_object_root_number", number_root, sizeof number_root - 1},
	};
	char user[128], backup[160], save_path[128];
	test_fixture_dir("test_config_rel01a_user.json", user, sizeof user);
	test_fixture_dir("test_config_rel01a_save.json", save_path,
					 sizeof save_path);
	snprintf(backup, sizeof backup, "%s.cw-backup", user);
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const struct fallback_case* k = &cases[i];
		int before = fallback_failures;
		remove(user);
		remove(backup); /* no backup recovery: the default must be reached */
		if(k->bytes && !fallback_write_file(user, k->bytes, k->length)) {
			fallback_red(k->name, "could not write the user config fixture");
			continue;
		}
		FILE* log = tmpfile();
		struct config c = {0};
		bool ok = fallback_boundary_load(&c, user, CW_TEST_CONFIG_WII_JSON,
										 log);
		if(!ok || !c.root) {
			fallback_red(k->name, "loader did not fall back to config_wii.json");
		} else {
			fallback_check_usable(k->name, &c, shipped, save_path);
			config_destroy(&c);
		}
		if(log) {
			char text[512];
			fflush(log);
			rewind(log);
			size_t n = fread(text, 1, sizeof text - 1, log);
			text[n] = '\0';
			fclose(log);
			if(!strstr(text, "Failed to load config"))
				fallback_red(k->name, "user-config failure was not logged");
			if(strstr(text, "aborting"))
				fallback_red(k->name, "loader logged a default-config abort");
		}
		if(k->bytes) {
			size_t left = 0;
			char* now = fallback_slurp(user, &left);
			if(!now || left != k->length
			   || memcmp(now, k->bytes, left) != 0)
				fallback_red(k->name, "loader changed the broken user file");
			free(now);
		}
		if(fallback_failures == before)
			printf("REL-01a-tests GREEN: %s -> shipped config_wii.json "
				   "(paths, input, controls, save/reload)\n", k->name);
	}
	remove(user);
	remove(backup);
	remove(save_path);
	json_value_free(shipped);
	free(shipped_bytes);
}

static void test_mipmap_flags(void) {
	const char* values[] = {"true", "1", "false", "0", "-2", "0.5",
		"null", "\"1\"", "[]", "{}"};
	const bool expected[] = {true, true, false, false, true, true,
		false, false, false, false};
	for(size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
		char text[80];
		snprintf(text, sizeof text, "{\"render\":{\"mipmaps\":%s}}", values[i]);
		struct config c = {0};
		c.root = json_parse_string(text);
		assert(c.root);
		assert(config_read_flag(&c, "render.mipmaps", false) == expected[i]);
		json_value_free(c.root);
	}
	struct config c = {0};
	c.root = json_parse_string("{}");
	assert(!config_read_flag(&c, "render.mipmaps", false));
	assert(config_read_flag(&c, "render.mipmaps", true));
	json_value_free(c.root);
}

int main(int argc, char** argv) {
	test_mipmap_flags();
	for(int i = 1; i < argc; i++) {
		if(strcmp(argv[i], "--fallback-disabled-mutant") == 0)
			fallback_mutant = true;
		else {
			fprintf(stderr, "unknown argument %s\n", argv[i]);
			return 2;
		}
	}
	test_user_config_reaches_shipped_defaults();
	if(fallback_failures) {
		printf("REL-01a-tests RED: %d fallback check(s) failed%s\n",
			   fallback_failures, fallback_mutant ? " (mutant)" : "");
		return 1;
	}
	if(fallback_mutant) {
		puts("mutant mode produced no RED: fallback checks are insensitive");
		return 2;
	}
	test_g9_pacing_presets();
	test_g9_pacing_slice_1ms();
	test_save_recovery();
	test_fallback_to_default();
	test_ephemeral_keys_stripped_on_save();
	test_non_ephemeral_save_is_unaffected();
	test_cw_set_displaylist_override_not_persisted();
	if(pacing_default_red) {
		printf("REL-10-tests RED: %d g9 pacing default expectation(s) unmet\n",
			   pacing_default_red);
		return 1;
	}
	fprintf(stderr, "test_config_fallback: all OK\n");
	return 0;
}
