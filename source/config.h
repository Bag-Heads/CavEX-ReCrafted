#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdio.h>

#include "parson/parson.h"

// Room for every key source/main.c's hw_run config-injection block can mark
// (13 today: "autoload" + 12 "debug.*" keys -- see config_mark_ephemeral)
// plus headroom for that block growing. Deliberately generous: overflowing
// this asserts loudly (config_mark_ephemeral) rather than silently dropping a
// mark, which would let an unmarked ephemeral key slip onto disk unstripped.
// Bumped 16 -> 24 (2026-09-08, lane/carve-observer-boot): --cw-set key=value
// (hw_run.h) adds up to HW_RUN_CW_SET_MAX (8) MORE ephemeral marks -- one per
// distinct dotted key a staged run overrides -- on top of the existing 13, so
// a maximally-combined cw_run_args.txt (every debug.* injection plus a full
// 8-entry --cw-set line) could otherwise exceed the old 16-slot cap and hit
// config_mark_ephemeral's overflow assert on real hardware.
#define CONFIG_MAX_EPHEMERAL_KEYS 24

struct config {
	JSON_Value* root;
	// Dot-path keys that hw_run injected into THIS config THIS PROCESS (see
	// main.c's hw_run config-injection block, e.g. "autoload" -> "+new" for a
	// capture/hardware-proof run). config_save() strips exactly these from the
	// on-disk write; the in-memory tree is untouched, so runtime behaviour
	// (e.g. screen_select_world.c reading "autoload") is unaffected. Populated
	// only by config_mark_ephemeral() -- NOT a static key list baked in here,
	// so growth in the hw_run injection block is covered automatically as
	// long as the new call site marks what it injects, same as every existing
	// one does; nothing here can drift out of step with main.c by itself.
	const char* ephemeral_keys[CONFIG_MAX_EPHEMERAL_KEYS];
	size_t ephemeral_key_count;
};

struct config_g9_lease { bool enabled, main_handoff; int max_us, service_us; };
struct config_g9_lease config_read_g9_lease(struct config* c);
/* G9 tick-wide boost (T2, opt-in, docs/g9_tick_boost.md): strict 0|1 knob,
 * default 0 (faithful scheduling); per-tick cap in ms, default 60, 5..200. */
struct config_g9_tick_boost { bool enabled; int cap_ms; };
struct config_g9_tick_boost config_read_g9_tick_boost(struct config* c);

bool config_create(struct config* c, const char* filename);
bool config_load_user_or_default(struct config* c, const char* user_filename,
								 const char* default_filename, FILE* log);
/* Stable achievement file keys. Missing keys default to player1/player2;
 * explicit invalid values and duplicates fail closed without rewriting config.
 * The output is empty on failure. B2 snapshots this pair before admission. */
#define CONFIG_PROFILE_ID_CAP 17
enum config_profiles_status {
	CONFIG_PROFILES_OK = 0,
	CONFIG_PROFILES_BAD_KEY,
	CONFIG_PROFILES_DUPLICATE,
};
enum config_profiles_status config_read_profiles(const struct config* c,
												 char out[2][CONFIG_PROFILE_ID_CAP]);
const char* config_read_string(struct config* c, const char* key,
							   const char* fallback);
// config_read_string for FILESYSTEM PATH values. On PLATFORM_WII a relative
// value is anchored to the SD root ("sd:/<value>") so it resolves identically
// under every launcher (HBC-direct CWD is sd:/apps/CavEX/, chainloaded/
// staged CWD is sd:/ -- the two-config/two-CWD trap, PROJECT_MEMORY parts
// 9-11); absolute values pass through untouched. On PC this is exactly
// config_read_string: the capture rigs depend on CWD-relative resolution.
// Use for paths handed to fopen/opendir; use config_read_string when the
// value is compared or re-persisted (selection screens, texturepacks_apply).
const char* config_read_path(struct config* c, const char* key,
							 const char* fallback);
bool config_read_int_array(struct config* c, const char* key, int* dest,
						   size_t* length);
// Read a scalar integer config value (dot-path key); returns `fallback` when the
// key is absent or not a number.
int config_read_int(struct config* c, const char* key, int fallback);
/* Boolean or numeric nonzero flag; missing/other types use fallback. */
bool config_read_flag(const struct config* c, const char* key, bool fallback);
/* G9 T2 experiment: one startup preset, invalid/absent values are OFF. */
struct config_g9_pacing { int preset, slice_us, sleep_ms, cap_ms; };
struct config_g9_pacing config_read_g9_pacing(struct config* c);
bool config_g9_settled_light_reuse(struct config* c);
/* G9 R19 deferred save IO (T2, opt-in, docs/g9_save_io.md): strict 0|1 knob
 * "experimental.g9_save_io_int", default 0 (every region write on the server
 * thread, as before). */
bool config_g9_save_io(struct config* c);
/* R22 T2 region-payload codec only: strict integer 1..6, default 6. */
int config_g9_save_zlib_level(struct config* c);
/* Opt-in presentation; absent, false and non-boolean values all disable it. */
bool config_mob_attack_windup_cue(const struct config* c);
// Set a (dot-path) string key in the in-memory config, then persist the whole
// config to `filename`. Used to remember an in-game texture-pack choice by
// writing paths.texturepack to the user config (config.json).
bool config_set_string(struct config* c, const char* key, const char* value);
bool config_set_int(struct config* c, const char* key, int value);
// next-gaps-0924 W1F: the Beta difficulty option (0 Peaceful .. 3 Hard,
// default 2 Normal -- source/game/difficulty.h). Read sanitizes a missing or
// invalid value to the default; set stores a sanitized value in memory only
// (callers decide when to config_save, like config_set_int).
int config_read_difficulty(struct config* c);
bool config_set_difficulty(struct config* c, int difficulty);
// Mark `key` (a stable/static string -- not copied, the same invariant every
// other `key` parameter in this API already carries) as an ephemeral value
// hw_run injected into `c` THIS PROCESS. config_save() strips marked keys
// from the on-disk write only; the value config_set_string/config_set_int
// already wrote into the in-memory tree is unaffected, so nothing about this
// process's own runtime behaviour changes -- only what survives to the next
// boot. Idempotent (marking the same key twice costs one slot, not two).
void config_mark_ephemeral(struct config* c, const char* key);
/* Writes a complete sibling before rotating the previous file to .cw-backup.
 * Failed writes preserve the old copy; config_create can recover that backup
 * when promotion/rollback was interrupted. Main-thread only. This is not a
 * guarantee against SD power loss/torn filesystem metadata. */
bool config_save(struct config* c, const char* filename);
void config_destroy(struct config* c);

// ---- Multiplayer saved-server list (screen_multiplayer_setup.c) ----------
// Beta 1.7.3's GuiMultiplayer has no saved list at all (a single address
// field, [740c583 GuiMultiplayer]) -- this is net-new UI on top of that
// faithful base (the carve-out #2 framing screen_multiplayer_setup.c's own
// header documents), so the caps below are a house choice, not a decompiled
// value. CONFIG_MP_SERVER_ADDRESS_CAP mirrors mp_codec.h's
// MP_ADDRESS_HOST_CAPACITY (128-char Beta field cap + NUL); config.c stays
// free of the CAVEX_MP_SOURCES-only mp_codec.h (manifest hygiene --
// docs/build_conventions.md: config.c is engine-core, mp_codec.h's owner is
// MP-only), so the number is duplicated here instead of shared via include.
#define CONFIG_MP_SERVER_NAME_CAP 32
#define CONFIG_MP_SERVER_ADDRESS_CAP 129
#define CONFIG_MP_SERVER_LIST_MAX 16

struct config_mp_server {
	char name[CONFIG_MP_SERVER_NAME_CAP];
	char address[CONFIG_MP_SERVER_ADDRESS_CAP];
};

// Reads mp.servers[] (an array of {"name","address"} objects) into
// out[0..cap), NUL-terminated/truncated per-field to the caps above.
// *count is always set (0 on a miss). MIGRATION: when mp.servers is absent
// (or not an array) but the pre-this-slice scalar mp.server is a non-empty
// string, synthesizes a single entry (name "Default", address = mp.server)
// -- existing pinned-server rigs keep working with no manual re-entry --
// and writes it straight into `c`'s in-memory tree (via
// config_write_server_list) so the very next config_save persists it; no
// separate "migrate" call site to remember. Returns false only on invalid
// arguments (c/out/count NULL); an empty or absent list is a normal true
// return with *count == 0.
bool config_read_server_list(struct config* c, struct config_mp_server* out,
							 size_t cap, size_t* count);

// Serializes list[0..count) to mp.servers[], replacing whatever was there
// (dotset creates "mp" as needed). Purely in-memory -- like
// config_set_string, callers decide when to config_save. Deliberately does
// NOT touch mp.server/mp.username: those stay the separate "active-target"
// scalars mpc_begin_connect reads (screen_multiplayer_connect.c), kept in
// sync by screen_multiplayer_setup.c's own select/quick-connect paths, not
// by list persistence.
bool config_write_server_list(struct config* c,
							  const struct config_mp_server* list,
							  size_t count);

#endif
