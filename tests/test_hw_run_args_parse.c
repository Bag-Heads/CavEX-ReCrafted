/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

// TOTAL-ness regression suite for hw_run_parse_file / hw_run_parse_args
// (2026-09-02, cw_run_args hardening lane). Background: three real-hardware
// PRE-FAT boot deaths, each preceded by a stale 175-byte cw_run_args.txt on
// the card from an earlier paused run, with zero boot trace either way. The
// file's role in those deaths is unproven (a layout collision is the other
// candidate -- see source/main.c's BREAKER-#2 LAYOUT GUARD comment), but
// regardless: hw_run_parse_file/hw_run_parse_args must be TOTAL -- any file
// content must degrade to hw_run disabled with an error string, NEVER hang,
// NEVER overrun, so main.c's boot always reaches the wii_after_fat trace
// (main.c's file-based fallback path deliberately IGNORES this function's
// return value -- "a parse error leaves telemetry off and boot continues",
// so the ONLY thing standing between a malformed file and a corrupted
// runtime config is cfg itself always landing in a fully pristine, disabled
// state on any rejection).
//
// Weaknesses found in the pre-hardening implementation (see hw_run.c's
// comments at the fix sites for the same detail):
//   1. hw_run_parse_args' error paths did `return false` WITHOUT resetting
//      cfg, so a file with an early valid flag (e.g. --cw-profile, which
//      applies side-effect fields like autopilot_unstick/superflat/
//      mp_autoconnect via hw_run_apply_profile) followed by a LATER syntax
//      error left cfg in a partially-armed state with those fields still
//      set, even though cfg.enabled correctly went false. main.c reads
//      autopilot_unstick UNCONDITIONALLY (not gated on cfg.enabled), so
//      this was a real, reachable corruption path, not just theoretical --
//      see test_profile_then_syntax_error_leaves_pristine_default below.
//   2. hw_run_parse_file's fread(buf, 1, sizeof(buf)-1, f) had no check for
//      "the file was actually longer than the buffer" -- it silently
//      truncated, then parsed whatever fragment of the last token survived
//      as if it were deliberate content, rather than failing loudly.
//   3. No explicit rejection of NUL/control bytes -- a NUL silently
//      truncated the effective C-string at that point (safe, but silent
//      and undocumented); a stray control byte just became opaque token
//      content.
//   4. The token-count cap (32, i.e. 31 real args) was enforced by simply
//      stopping the tokenizer loop -- extra tokens were silently dropped
//      with no error, rather than the file being rejected.
//
// This file proves the fix: a table of adversarial vectors (empty, no
// trailing newline, doubled/concatenated records, junk/control bytes,
// overlong token count, CRLF, embedded NUL, a file larger than the buffer)
// plus a small deterministic fuzz loop.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/game/hw_run.h"

#define ARGS_PATH "test_hw_run_args_parse.tmp"

static void write_file(const char* path, const unsigned char* data,
						size_t len) {
	FILE* f = fopen(path, "wb");
	assert(f);
	if(len > 0)
		assert(fwrite(data, 1, len, f) == len);
	fclose(f);
}

static void write_text(const char* path, const char* text) {
	write_file(path, (const unsigned char*)text, strlen(text));
}

// A rejected/malformed parse must leave cfg BYTE-IDENTICAL to a freshly
// initialized, disabled default -- not merely cfg.enabled == false. This is
// the direct proof of fix #1 above: comparing the whole struct (rather than
// hand-picking fields) catches ANY side-effect field a future profile might
// add that main.c later reads unconditionally, the exact shape of the bug
// this hardening closes.
static bool cfg_is_pristine_default(const struct hw_run_config* cfg) {
	struct hw_run_config fresh;
	hw_run_config_init(&fresh);
	return memcmp(cfg, &fresh, sizeof(fresh)) == 0;
}

struct vector {
	const char* name;
	const unsigned char* data; // non-NULL: raw bytes (may embed NUL/control)
	size_t len;                // length of `data`, when used
	const char* text;          // used instead of `data` when data == NULL
	bool expect_ok;
	bool expect_enabled; // only checked when expect_ok
	const char* expect_err_substr; // only checked when !expect_ok; NULL = skip
};

static void run_vector(const struct vector* v) {
	if(v->data)
		write_file(ARGS_PATH, v->data, v->len);
	else
		write_text(ARGS_PATH, v->text);

	struct hw_run_config cfg;
	char err[128];
	// Real callers (source/main.c) only ever reach hw_run_parse_file with a
	// cfg that a prior hw_run_parse_args/hw_run_config_init already made
	// valid -- and hw_run_parse_file's own documented "nothing to parse"
	// fast path (a missing file, or one with zero real tokens) deliberately
	// leaves cfg untouched rather than re-initializing it on every call.
	// Starting from that same realistic precondition here (instead of raw
	// poison) is what makes the OK+untouched vectors below (empty file,
	// comment-only) assertable at all; the dedicated
	// test_profile_then_syntax_error_leaves_pristine_default below
	// additionally poisons cfg with garbage first to prove the ERROR path's
	// reset is a real, active re-initialization and not just "started
	// clean, stayed clean".
	hw_run_config_init(&cfg);
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	remove(ARGS_PATH);

	if(ok != v->expect_ok) {
		fprintf(stderr,
				"[%s] hw_run_parse_file returned %d, expected %d (err=\"%s\")\n",
				v->name, (int)ok, (int)v->expect_ok, err);
		assert(0 && "hw_run_parse_file return mismatch");
	}
	if(!ok) {
		if(!cfg_is_pristine_default(&cfg)) {
			fprintf(stderr,
					"[%s] rejected parse left cfg NOT pristine-default (TOTAL-ness fix regressed)\n",
					v->name);
			assert(0 && "cfg not pristine-default after rejected parse");
		}
		if(v->expect_err_substr && !strstr(err, v->expect_err_substr)) {
			fprintf(stderr, "[%s] err \"%s\" missing expected substring \"%s\"\n",
					v->name, err, v->expect_err_substr);
			assert(0 && "error string missing expected substring");
		}
	} else {
		if(cfg.enabled != v->expect_enabled) {
			fprintf(stderr, "[%s] cfg.enabled=%d, expected %d\n", v->name,
					(int)cfg.enabled, (int)v->expect_enabled);
			assert(0 && "cfg.enabled mismatch");
		}
	}
}

// VECTOR_1_FROM_CARD (2026-09-02): the real 175-byte stale cw_run_args.txt
// pulled from the card ahead of the three PRE-FAT boot deaths this
// hardening lane investigates. NOT PROVIDED to this lane run -- paste the
// exact bytes here as a byte-array literal the moment the orchestrator
// supplies them (a `fetch_wii_stage_file.ps1` pull of
// sd:/apps/CavEX/cw_run_args.txt from the affected card, or a
// card-reader copy), e.g.:
//
//   static const unsigned char VECTOR_1_FROM_CARD[] = {
//       0x2d, 0x2d, 0x63, 0x77, /* ... 175 bytes total ... */
//   };
//
// then add it to `vectors[]` below as
//   {"vector_1_from_card", VECTOR_1_FROM_CARD, sizeof(VECTOR_1_FROM_CARD),
//    NULL, <expect_ok>, <expect_enabled>, NULL}
// once its actual content (and therefore the correct expected verdict) is
// known. Left out of the active table on purpose: a guessed expectation
// here would either mask a real divergence or assert a fabricated one --
// neither is better than leaving this slot visibly unfilled.

static const struct vector vectors[] = {
	// --- basic degrade-to-disabled shapes ------------------------------
	{"empty_file", NULL, 0, "", true, false, NULL},
	{"comment_only_with_newline", NULL, 0, "#\n", true, false, NULL},
	// no trailing newline, AND a bare "#" with nothing after it.
	{"comment_only_no_newline", NULL, 0, "#", true, false, NULL},

	// --- valid content shapes (still checked in depth separately below) -
	{"valid_no_trailing_newline", NULL, 0,
	 "--cw-run-id r1 --cw-tel-host 10.0.0.5", true, true, NULL},
	{"valid_crlf_line_endings", NULL, 0,
	 "--cw-run-id\r\nr2\r\n--cw-tel-host\r\n10.0.0.6\r\n", true, true, NULL},
	{"doubled_concatenated_records_last_wins", NULL, 0,
	 "--cw-run-id A --cw-tel-host 1.1.1.1 --cw-run-id B --cw-tel-host 2.2.2.2",
	 true, true, NULL},

	// --- exact byte-count boundary (511 == max, one more must reject) --
	// 511 non-whitespace bytes starting with '#': one giant "comment"
	// token, so it is valid (disabled, no error) shape AT the boundary.
	// Generated + length-verified (511 chars: '#' + 510 filler digits, no
	// whitespace) rather than hand-counted -- see the lane's build notes.
	{"exactly_max_bytes_is_ok", NULL, 0,
	 "#234567892345678923456789234567892345678923456789234567892345678923456"
	 "7892345678923456789234567892345678923456789234567892345678923456789234"
	 "5678923456789234567892345678923456789234567892345678923456789234567892"
	 "3456789234567892345678923456789234567892345678923456789234567892345678"
	 "9234567892345678923456789234567892345678923456789234567892345678923456"
	 "7892345678923456789234567892345678923456789234567892345678923456789234"
	 "5678923456789234567892345678923456789234567892345678923456789234567892"
	 "345678923456789234567",
	 true, false, NULL},

	// --- malformed content: must reject WITH a clear reason ------------
	{"unknown_key", NULL, 0, "--cw-bogus-flag", false, false,
	 "unknown hardware-run option"},
	{"missing_value_at_end", NULL, 0, "--cw-tel-host", false, false,
	 "missing value"},
	{"invalid_port_value", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-tel-port notanumber", false,
	 false, "invalid --cw-tel-port"},
	{"missing_required_tel_host", NULL, 0, "--cw-run-id r_only", false, false,
	 "--cw-tel-host is required"},
	// A profile flag (which applies real side-effect fields via
	// hw_run_apply_profile) followed by a later syntax error -- the exact
	// shape of fix #1's bug. See the deeper check below too.
	{"profile_then_syntax_error", NULL, 0,
	 "--cw-profile mp_lan_walk_10m --cw-tel-host 1.2.3.4 --cw-bogus-flag",
	 false, false, "unknown hardware-run option"},

	// --- byte-content hardening (fix #3) --------------------------------
	{"control_byte_rejected", NULL, 0, "--cw-run-id r\x01x", false, false,
	 "control byte"},

	// --- token-count hardening (fix #4): 40 bare tokens, well past the --
	// 31-real-token cap (32 argv slots incl. argv[0]).
	{"too_many_tokens", NULL, 0,
	 "x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x "
	 "x x x x x x",
	 false, false, "more than"},

	// --- oversized-file hardening (fix #2): 512 bytes, one past the cap -
	{"file_exceeds_buffer", NULL, 0,
	 "#234567892345678923456789234567892345678923456789234567892345678923456"
	 "7892345678923456789234567892345678923456789234567892345678923456789234"
	 "5678923456789234567892345678923456789234567892345678923456789234567892"
	 "3456789234567892345678923456789234567892345678923456789234567892345678"
	 "9234567892345678923456789234567892345678923456789234567892345678923456"
	 "7892345678923456789234567892345678923456789234567892345678923456789234"
	 "5678923456789234567892345678923456789234567892345678923456789234567892"
	 "3456789234567892345678",
	 false, false, "exceeds"},

	// --- --cw-far-view-distance (Stage-2 LOD reach gate, 2026-09-03) ----
	// Malformed: non-numeric value rejected with a clear reason (mirrors
	// invalid_port_value above). Valid/clamped/passthrough behavior is
	// checked in depth by the dedicated field-check tests below (this
	// table only asserts ok/enabled, per run_vector's `struct vector`).
	{"far_view_distance_malformed", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-view-distance notanumber",
	 false, false, "invalid --cw-far-view-distance"},
	{"far_view_distance_missing_value", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-view-distance", false,
	 false, "missing value"},

	// --- --cw-far-storage (S2 measurement, 2026-09-03) ------------------
	// Both valid values parse ok here (enabled); the exact enum value each
	// sets is checked in depth by the dedicated field-check tests below.
	{"far_storage_voxel", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-storage voxel", true, true,
	 NULL},
	{"far_storage_skin", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-storage skin", true, true,
	 NULL},
	{"far_storage_malformed", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-storage bogus", false,
	 false, "invalid --cw-far-storage"},
	{"far_storage_missing_value", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-storage", false, false,
	 "missing value"},

	// --- --cw-seed (harness fixed-seed lane, 2026-09-04) ----------------
	// Malformed: non-numeric, empty (missing value at end), and trailing-
	// garbage-after-digits all rejected with a clear reason (mirrors
	// invalid_port_value above). Valid/negative/absent behavior is checked
	// in depth by the dedicated field-check tests below (this table only
	// asserts ok/enabled, per run_vector's `struct vector`).
	{"seed_malformed", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-seed notanumber", false, false,
	 "invalid --cw-seed"},
	{"seed_trailing_garbage", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-seed 123x", false, false,
	 "invalid --cw-seed"},
	{"seed_missing_value", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-seed", false, false,
	 "missing value"},
	{"seed_valid", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-seed 12345", true, true, NULL},
	{"seed_negative_valid", NULL, 0,
	 "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-seed -987654321", true, true,
	 NULL},
};

// Embedded-NUL vector needs a raw byte array (a C string can't carry an
// embedded NUL), so it is built and run separately rather than through the
// `vectors[]` table's `text` field.
static void test_embedded_nul_rejected(void) {
	static const unsigned char data[] = {
		'-', '-', 'c', 'w', '-', 'r', 'u', 'n', '-', 'i', 'd', ' ', 'r',
		'\0', '3', ' ', '-', '-', 'c', 'w', '-', 't', 'e', 'l', '-', 'h',
		'o', 's', 't', ' ', '1', '.', '1', '.', '1', '.', '1',
	};
	const struct vector v = {"embedded_nul", data, sizeof(data), NULL, false,
							 false, "NUL"};
	run_vector(&v);
}

static void test_missing_file_is_disabled_no_error(void) {
	remove(ARGS_PATH); // ensure absence
	struct hw_run_config cfg;
	char err[128];
	err[0] = '\0';
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	assert(ok);
	// A missing file never touches cfg (see hw_run.c) -- that is only safe
	// because every caller already holds a pristine/valid cfg before this
	// call (source/main.c only reaches the file-fallback path with a cfg
	// that hw_run_parse_args already initialized at the top of main()).
	// Confirm that documented precondition still produces a sane result:
	// starting from a pristine cfg, a missing file leaves it pristine.
	hw_run_config_init(&cfg);
	bool ok2 = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	assert(ok2);
	assert(cfg_is_pristine_default(&cfg));
}

static void test_valid_no_trailing_newline_fields(void) {
	write_text(ARGS_PATH, "--cw-run-id r1 --cw-tel-host 10.0.0.5");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(strcmp(cfg.run_id, "r1") == 0);
	assert(strcmp(cfg.telemetry_host, "10.0.0.5") == 0);
}

static void test_valid_crlf_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id\r\nr2\r\n--cw-tel-host\r\n10.0.0.6\r\n");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(strcmp(cfg.run_id, "r2") == 0);
	assert(strcmp(cfg.telemetry_host, "10.0.0.6") == 0);
}

static void test_doubled_records_last_wins_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id A --cw-tel-host 1.1.1.1 --cw-run-id B "
			   "--cw-tel-host 2.2.2.2");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(strcmp(cfg.run_id, "B") == 0);
	assert(strcmp(cfg.telemetry_host, "2.2.2.2") == 0);
}

// --cw-far-view-distance (Stage-2 LOD reach gate, 2026-09-03) field checks:
// valid in-range value passes through unchanged.
static void test_far_view_distance_valid_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-view-distance "
			   "12");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_view_distance == 12);
	assert(12 >= HW_RUN_FAR_VIEW_DISTANCE_MIN
		   && 12 <= HW_RUN_FAR_VIEW_DISTANCE_MAX);
}

// An out-of-range value (both too low and too high) is CLAMPED into
// [HW_RUN_FAR_VIEW_DISTANCE_MIN, HW_RUN_FAR_VIEW_DISTANCE_MAX], not
// rejected -- this is a tuning knob, not a malformed-input case.
static void test_far_view_distance_clamped_fields(void) {
	struct hw_run_config cfg;
	char err[128];

	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-view-distance "
			   "0");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_view_distance == HW_RUN_FAR_VIEW_DISTANCE_MIN);

	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-view-distance "
			   "99999");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_view_distance == HW_RUN_FAR_VIEW_DISTANCE_MAX);
}

// -1 is the field's own documented "leave the normal config/default in
// control" sentinel and passes through UNCLAMPED -- lets a caller force
// that behavior even on far_idle_2m/far_walk_6m, which otherwise pin the
// far view distance to 6 via hw_run_apply_profile. Also proves the
// override applies regardless of argv order relative to --cw-profile (here
// the override comes AFTER the profile, which already sets far_view_
// distance to 6 for far_idle_2m).
static void test_far_view_distance_minus_one_passthrough_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-profile far_idle_2m "
			   "--cw-far-view-distance -1");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_view_distance == -1);
}

// Default behavior is byte-identical when the flag is absent: far_idle_2m
// still pins 6 (hw_run_apply_profile's own hard-coded default), untouched
// by this feature.
static void test_far_view_distance_absent_is_unchanged(void) {
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile far_idle_2m");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_view_distance == 6);
}

// --cw-far-storage (S2 measurement, 2026-09-03) field checks.
static void test_far_storage_voxel_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-storage voxel");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_storage == HW_RUN_FAR_STORAGE_VOXEL);
}

static void test_far_storage_skin_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-far-storage skin");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_storage == HW_RUN_FAR_STORAGE_SKIN);
}

// Default behavior is byte-identical when the flag is absent: cfg.far_storage
// stays HW_RUN_FAR_STORAGE_UNSET (config.json's lod.far_storage decides).
static void test_far_storage_absent_is_unchanged(void) {
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile far_idle_2m");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.far_storage == HW_RUN_FAR_STORAGE_UNSET);
}

// --cw-settle / --cw-carve-census (2026-09-07, lane/observer-arm-flags):
// before this lane, main.c armed settle_observer_start() on hw_run.tps_cpu,
// so the tps census and the settle observer could never be bisected
// independently on real hardware, and no run arg existed to arm
// carve_census at all (it could only be forced true in-process). These
// three fields (tps_cpu, settle, carve_census) are now set by three fully
// independent flags -- prove each is set/unset on its own and that
// combining them never cross-arms another field.
static void test_settle_flag_sets_field_alone(void) {
	write_text(ARGS_PATH, "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-settle");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.settle);
	assert(!cfg.tps_cpu);
	assert(!cfg.carve_census);
}

/* colcap-1006 (R5 item 2): --cw-pool-placement belongs to pool_selftest_3m
 * alone; the profile defaults to the production (lowest) placement, and the
 * flag is a hard error for every other profile and for a bad value. */
static void test_pool_placement_rules(void) {
	struct hw_run_config cfg;
	char err[128];
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile pool_selftest_3m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(cfg.pool_selftest && !cfg.pool_placement_rotating);
	assert(cfg.seed_set && cfg.seed == 12345 && cfg.force_overworld);
	assert(cfg.far_view_distance == -1 && !cfg.driver_owns_completion);
	write_text(ARGS_PATH, "--cw-pool-placement rotating --cw-tel-host 1.1.1.1 "
						  "--cw-profile pool_selftest_3m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(cfg.pool_selftest && cfg.pool_placement_rotating);
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile pool_selftest_3m "
						  "--cw-pool-placement lowest");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(cfg.pool_selftest && !cfg.pool_placement_rotating);
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile far_walk_6m "
						  "--cw-pool-placement rotating");
	assert(!hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(strstr(err, "requires pool_selftest_3m"));
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile pool_selftest_3m "
						  "--cw-pool-placement middle");
	assert(!hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	/* split_regrow_8m (R5 item 3): the anchor cycle needs a separation */
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile split_regrow_8m");
	assert(!hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(strstr(err, "requires --cw-anchor-sep"));
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile split_regrow_8m "
						  "--cw-anchor-sep 256");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(cfg.anchor_cycle && cfg.anchor_sep == 256 && cfg.autopilot_keepup);
	assert(cfg.seed_set && cfg.seed == 12345 && cfg.duration_seconds == 360);
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile split_anchor_6m "
						  "--cw-anchor-sep 256");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(!cfg.anchor_cycle);
	/* a later --cw-profile never inherits the self-test */
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile pool_selftest_3m "
						  "--cw-profile far_idle_2m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(!cfg.pool_selftest && !cfg.pool_placement_rotating);
	remove(ARGS_PATH);
}

// Default behavior is byte-identical when the flag is absent.
static void test_settle_absent_is_unchanged(void) {
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile far_idle_2m");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(!cfg.settle);
}

static void test_carve_census_flag_sets_field_alone(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-carve-census");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.carve_census);
	assert(!cfg.settle);
	assert(!cfg.tps_cpu);
}

// --cw-set key=value (run-scoped config override for staged hardware runs,
// 2026-09-08, lane/carve-observer-boot; hw_run.h's cw_set field comment has
// the full story). Valid case: two entries parse into cw_set[]/cw_set_n in
// order, do not set cfg.enabled by themselves (same pattern as --cw-settle/
// --cw-carve-census above), and leave cw_set_malformed at zero.
static void test_cw_set_valid_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 "
			   "--cw-set memory.displaylist_compact=1 --cw-set foo.bar=-7");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.cw_set_n == 2);
	assert(cfg.cw_set_malformed == 0);
	assert(strcmp(cfg.cw_set[0].key, "memory.displaylist_compact") == 0);
	assert(cfg.cw_set[0].value == 1);
	assert(strcmp(cfg.cw_set[1].key, "foo.bar") == 0);
	assert(cfg.cw_set[1].value == -7);
}

static void test_weather_force_integer_alias(void) {
	write_text(ARGS_PATH,
		"--cw-set debug.weather_force_int=1 "
		"--cw-set debug.weather_force_int=2 "
		"--cw-set debug.weather_force_int=0 "
		"--cw-set debug.weather_force_int=3");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.cw_set_n == 3);
	assert(cfg.cw_set[0].value == 1);
	assert(cfg.cw_set[1].value == 2);
	assert(cfg.cw_set[2].value == 0);
	assert(cfg.cw_set_malformed == 1);
}

// Absent --cw-tel-host means a bare --cw-set alone must NOT itself arm
// cfg.enabled (it would otherwise fail hw_run_parse_args' "telemetry_host
// required for hardware runs" gate) -- same "does not arm by itself" pattern
// as --cw-settle/--cw-carve-census/--cw-column-census.
static void test_cw_set_alone_does_not_arm_enabled(void) {
	write_text(ARGS_PATH, "--cw-set memory.displaylist_compact=1");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(!cfg.enabled);
	assert(cfg.cw_set_n == 1);
	assert(cfg.cw_set[0].value == 1);
}

// Malformed entries (no '=', empty value, non-integer value) are IGNORED --
// counted in cw_set_malformed, never fatal to the whole parse (unlike every
// other --cw-* flag's malformed value, which calls parse_fail). A valid
// entry on either side of the bad ones still lands.
static void test_cw_set_malformed_ignored_with_counter(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-set good.one=1 "
			   "--cw-set noequalssign --cw-set empty.value= "
			   "--cw-set not.an.int=abc --cw-set good.two=2");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.cw_set_n == 2);
	assert(strcmp(cfg.cw_set[0].key, "good.one") == 0);
	assert(cfg.cw_set[0].value == 1);
	assert(strcmp(cfg.cw_set[1].key, "good.two") == 0);
	assert(cfg.cw_set[1].value == 2);
	assert(cfg.cw_set_malformed == 3);
}

// More than HW_RUN_CW_SET_MAX (8) entries: the first 8 apply, the rest are
// ignored and counted, same non-fatal treatment as any other malformed entry.
static void test_cw_set_over_cap_counted_not_fatal(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 "
			   "--cw-set k0=0 --cw-set k1=1 --cw-set k2=2 --cw-set k3=3 "
			   "--cw-set k4=4 --cw-set k5=5 --cw-set k6=6 --cw-set k7=7 "
			   "--cw-set k8=8 --cw-set k9=9");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.cw_set_n == HW_RUN_CW_SET_MAX);
	assert(cfg.cw_set_malformed == 2);
	assert(strcmp(cfg.cw_set[0].key, "k0") == 0);
	assert(strcmp(cfg.cw_set[HW_RUN_CW_SET_MAX - 1].key, "k7") == 0);
}

// The exact regression this lane exists to fix: --cw-tps-cpu alone must no
// longer imply --cw-settle (main.c used to gate settle_observer_start() on
// tps_cpu), and --cw-settle alone must not imply --cw-tps-cpu -- the two
// observers are independently bisectable. Also proves both flags together
// arm both fields, and neither flag arms carve_census.
static void test_settle_and_tps_cpu_are_independent(void) {
	struct hw_run_config cfg;
	char err[128];

	write_text(ARGS_PATH, "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-tps-cpu");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.tps_cpu);
	assert(!cfg.settle);
	assert(!cfg.carve_census);

	write_text(ARGS_PATH, "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-settle");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(!cfg.tps_cpu);
	assert(cfg.settle);
	assert(!cfg.carve_census);

	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-tps-cpu --cw-settle "
			   "--cw-carve-census");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.tps_cpu);
	assert(cfg.settle);
	assert(cfg.carve_census);
}

// A trailing unknown flag after the new flags still rejects the whole file
// with the same error and resets cfg to pristine-default -- the new flags
// don't carve out an exception to the TOTAL-ness hardening fix #1 guards.
static void test_settle_carve_census_then_unknown_flag_rejects(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-settle "
			   "--cw-carve-census --cw-bogus-flag");
	struct hw_run_config cfg;
	char err[128];
	memset(&cfg, 0xAA, sizeof(cfg)); // poison, like the existing profile test
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	remove(ARGS_PATH);
	assert(!ok);
	assert(strstr(err, "unknown hardware-run option"));
	assert(cfg_is_pristine_default(&cfg));
}

// --cw-tel-nonblock (2026-09-08, lane/carve-observer-boot sender-wedge lane):
// the opt-in non-blocking telemetry-send EXPERIMENT. Like --cw-settle and
// --cw-carve-census it is a DIAGNOSTIC flag, so it must set its own field,
// must not imply cfg.enabled on its own, and must not cross-arm any observer.
static void test_tel_nonblock_flag_sets_field_alone(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-tel-nonblock");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled); // from --cw-tel-host, not from this flag
	assert(cfg.telemetry_nonblock);
	assert(!cfg.settle);
	assert(!cfg.tps_cpu);
	assert(!cfg.carve_census);
}

// THE guarantee the experiment rests on: an unarmed run is byte-identical to
// before the flag existed. Every other arming flag leaves it false.
static void test_tel_nonblock_absent_is_unchanged(void) {
	struct hw_run_config cfg;
	char err[128];

	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile far_idle_2m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(!cfg.telemetry_nonblock);

	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-tps-cpu --cw-settle "
			   "--cw-carve-census");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.tps_cpu && cfg.settle && cfg.carve_census);
	assert(!cfg.telemetry_nonblock);

	// ...and armed alongside them it arms only itself.
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-carve-census "
			   "--cw-tel-nonblock");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.carve_census && cfg.telemetry_nonblock);
	assert(!cfg.settle && !cfg.tps_cpu);
}

// --cw-far-send-near-owed (T2 far-send share, 2026-09-04, lane/far-send-
// share) regression lock. Real-hardware run staged_far_walk_6m_farshare_
// 20260904_091542 staged the byte-exact card order (tools/stage_wii_dol.ps1:
// run-id, tel-host, tel-port, tel-mode, profile, duration, netmask, THEN
// --cw-far-send-near-owed) and the autopilot never displaced the player
// (0.4 blocks over 360s vs. baseline's 24-82). This proves, against the
// EXACT staged token order, that hw_run_parse_args/hw_run_parse_file do
// NOT disarm autopilot_walk (or any other profile side effect) when the
// flag trails a profile on the line: hw_run_apply_profile only ever runs
// from the "--cw-profile" branch, and --cw-far-send-near-owed's handler
// writes only cfg->far_send_near_owed + cfg->enabled (see hw_run.c) --
// there is no re-apply-profile / cfg-reset call on this path for the
// hw_run.c/.h layer to have gotten wrong. (Traced further: a same-seed PC
// A/B walk trace with/without this flag showed byte-identical displacement
// over 480 ticks, and the exact embedded-argv byte count the real run's
// [RUNPREP] log reported (211/221 bytes, argv_count 17) rebuilds and parses
// clean here too -- the real-hardware stall was NOT reproduced at the
// hw_run.c parsing layer.)
static void test_far_send_near_owed_after_profile_leaves_walk_armed(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id staged_far_walk_6m_farshare_20260904_091542 "
			   "--cw-tel-host pull --cw-tel-port 39017 --cw-tel-mode pull "
			   "--cw-profile far_walk_6m --cw-duration 360 "
			   "--cw-netmask 255.255.252.0 --cw-far-send-near-owed 16");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.autopilot_walk);
	assert(cfg.duration_seconds == 360);
	assert(cfg.far_send_near_owed == 16);
	assert(strcmp(cfg.profile_name, "far_walk_6m") == 0);
}

// The flag alone, with NO --cw-profile on the line, must never itself arm
// autopilot: only hw_run_apply_profile's far_walk_6m/mp_lan_walk_10m cases
// (or a hand-authored --cw-autopilot-walk-equivalent, which does not exist)
// ever set autopilot_walk. --cw-far-send-near-owed's handler touches only
// cfg->far_send_near_owed/cfg->enabled -- proving that here closes off the
// "the flag itself forces autopilot on/off" half of the hypothesis.
static void test_far_send_near_owed_alone_leaves_autopilot_off(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 "
			   "--cw-far-send-near-owed 16");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(!cfg.autopilot_walk);
	assert(cfg.far_send_near_owed == 16);
}

// --cw-seed (harness fixed-seed lane, 2026-09-04) field checks.
static void test_seed_valid_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-seed 12345");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.seed_set);
	assert(cfg.seed == 12345);
}

static void test_seed_negative_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-run-id r --cw-tel-host 1.1.1.1 --cw-seed -987654321");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.seed_set);
	assert(cfg.seed == -987654321LL);
}

// Default behavior is byte-identical when the flag is absent, FOR A PROFILE
// THAT DOES NOT PIN ITS OWN SEED: cfg.seed_set stays false and cfg.seed
// stays 0 (sworld_finish_create's random per-create mix stays in control --
// see hw_run.h's cfg->seed comment). menus_dwell is deliberately NOT a
// measurement profile (docs/intent/perf-seed-pin.md), so it stays outside
// hw_run_apply_profile's `!cfg->seed_set` default-seed guard used by
// far_idle_2m/far_walk_6m/keepup_walk_6m/split_anchor_6m/split_play_8m/
// perf_stress_5m/redstone_clock_2m -- see test_measurement_profiles_
// default_seed_12345 below for THOSE profiles' (changed) behavior.
static void test_seed_absent_is_unchanged(void) {
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile menus_dwell");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(!cfg.seed_set);
	assert(cfg.seed == 0);
}

// perf-seed-pin (docs/intent/perf-seed-pin.md, 2026-09-26): every
// measurement profile now defaults to world seed 12345 when no --cw-seed is
// given, so cross-run/cross-build comparisons (and the G1 perf_stress_5m
// baseline specifically -- TICKCOST-BISECT-0926.md traced its drift to an
// unseeded time(NULL) world) sit on the same terrain. Normal play (hw_run
// not armed / cfg.enabled false) is untouched -- this only exercises the
// armed hw_run_apply_profile path.
static void test_measurement_profiles_default_seed_12345(void) {
	static const char* const profiles[] = {
		"far_idle_2m",	  "far_walk_6m",	  "keepup_walk_6m",
		"split_anchor_6m", "split_play_8m",  "perf_stress_5m",
		"redstone_clock_2m",
	};
	for(size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
		struct hw_run_config cfg;
		hw_run_config_init(&cfg);
		enum hw_run_profile p;
		assert(hw_run_profile_from_name(profiles[i], &p));
		hw_run_apply_profile(&cfg, p);
		assert(cfg.seed_set);
		assert(cfg.seed == 12345);
		assert(cfg.seed_from_profile);
	}
}

// Explicit --cw-seed still wins over the new default pin, in either token
// order relative to --cw-profile (the same regression class --cw-far-view-
// distance and --cw-seed itself were already proven against above).
static void test_measurement_profile_explicit_seed_wins(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-profile perf_stress_5m --cw-seed "
			   "999");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.seed_set);
	assert(cfg.seed == 999);
	assert(!cfg.seed_from_profile);

	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-seed 999 --cw-profile "
			   "perf_stress_5m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.seed_set);
	assert(cfg.seed == 999);
	assert(!cfg.seed_from_profile);
}

// Normal play is untouched: hw_run_config_init leaves cfg.enabled false and
// the profile default-init path (FAR_IDLE_2M, applied once at init time
// before any parse) still pins the same 12345 default -- but that is inert
// unless a caller goes on to arm the run (hw_run_parse_args/_file require
// --cw-tel-host to set cfg.enabled). This proves an UNARMED config carries
// no observable difference other than the harmless seed field itself, which
// only hw_run.c's own armed consumers (world create) ever read.
static void test_unarmed_config_seed_pin_is_inert(void) {
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	assert(!cfg.enabled);
	assert(cfg.seed_set);
	assert(cfg.seed == 12345);
	assert(cfg.seed_from_profile);
}

// The regression this task exists to guard against: a NEW --cw-* flag
// (--cw-far-view-distance, landed 2026-09-03) once reset an UNRELATED
// profile field (autopilot_walk) as a side effect of how it was wired.
// --cw-seed must NOT disarm anything else a profile set: --cw-profile
// far_walk_6m's own defaults (autopilot_walk=true, duration_seconds=360)
// stay intact alongside the new seed field, regardless of which flag comes
// first on the line.
static void test_seed_does_not_disarm_profile_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-profile far_walk_6m --cw-seed "
			   "12345");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.autopilot_walk);
	assert(cfg.duration_seconds == 360);
	assert(cfg.seed_set);
	assert(cfg.seed == 12345);

	// Same assertion with the flags in the OPPOSITE order -- --cw-far-view-
	// distance's own bug (hw_run.c's far_view_distance_requested comment)
	// depended on ordering relative to --cw-profile; prove --cw-seed has no
	// such dependency either way.
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-seed 12345 --cw-profile "
			   "far_walk_6m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.autopilot_walk);
	assert(cfg.duration_seconds == 360);
	assert(cfg.seed_set);
	assert(cfg.seed == 12345);
}

// keepup_walk_6m (2026-09-04, deterministic keep-up profile): proves the
// profile name resolves and hw_run_apply_profile sets exactly the intended
// fields -- same duration/reach envelope as far_walk_6m (so the two are
// comparable for chunk-load pressure), but autopilot_keepup (not
// autopilot_walk) is the flag that gets set, and autopilot_unstick stays
// false (this profile's driver -- autopilot_keepup.h -- never uses the
// legacy unstick wobble). See hw_run.h's HW_RUN_PROFILE_KEEPUP_WALK_6M
// comment and autopilot_keepup.h for the RCA this profile exists to fix.
static void test_keepup_walk_6m_profile_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-profile keepup_walk_6m");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_KEEPUP_WALK_6M);
	assert(strcmp(cfg.profile_name, "keepup_walk_6m") == 0);
	assert(cfg.duration_seconds == 360);
	assert(cfg.autopilot_keepup);
	// NOT the legacy walk driver's flags -- a distinct mode, not a reuse.
	assert(!cfg.autopilot_walk);
	assert(!cfg.autopilot_unstick);
	assert(!cfg.autopilot_aim);
	// Same reach as far_walk_6m so tris/chunks_meshed comparisons are
	// apples-to-apples (hw_run_apply_profile's default pin of 6, untouched
	// by this profile's switch case).
	assert(cfg.far_view_distance == 6);
}

// split_actions_6m uses a deterministic flat Overworld so its real
// dig/drop/collect/place sequence has a reliable floor.  split_play_8m remains
// the ordinary-terrain two-player coverage.
static void test_split_action_floor_profile_fields(void) {
	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-profile split_actions_6m");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M);
	assert(strcmp(cfg.profile_name, "split_actions_6m") == 0);
	assert(cfg.duration_seconds == 360);
	assert(cfg.autopilot_keepup);
	assert(cfg.split_guest);
	assert(cfg.split_guest_walk);
	assert(cfg.split_guest_yaw_deg == 180);
	assert(cfg.split_guest_actions);
	assert(cfg.superflat);
	assert(cfg.force_overworld);

	write_text(ARGS_PATH,
			   "--cw-tel-host 1.1.1.1 --cw-profile split_play_8m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_SPLIT_PLAY_8M);
	assert(strcmp(cfg.profile_name, "split_play_8m") == 0);
	assert(cfg.duration_seconds == 360);
	assert(cfg.autopilot_keepup);
	assert(cfg.split_guest);
	assert(cfg.split_guest_walk);
	assert(cfg.split_guest_yaw_deg == 180);
	assert(!cfg.split_guest_actions);
	assert(!cfg.superflat);
	assert(!cfg.force_overworld);
}

// The "keepup_walk" short alias resolves to the same profile as the "_6m"
// canonical name, mirroring far_walk_6m/far_walk's own alias pattern.
static void test_keepup_walk_alias_resolves(void) {
	enum hw_run_profile p;
	assert(hw_run_profile_from_name("keepup_walk", &p));
	assert(p == HW_RUN_PROFILE_KEEPUP_WALK_6M);
}

// cand10 integration lock (2026-09-04): the three harness lanes
// (lane/keepup-profile, lane/far-send-share, lane/harness-fixed-seed) each
// added a flag that main.c reads; this vector proves they COEXIST in the
// exact token order tools/stage_wii_dol.ps1 emits for a staged keep-up run
// (profile, duration, far-send share, seed). Every one of the four fields
// must survive together -- no later flag may clobber an earlier one's
// profile side effects (the --cw-far-view-distance regression class).
static void test_keepup_combined_staged_args(void) {
	// The leading run-id/tel-* preamble is exactly what stage_wii_dol.ps1
	// emits ahead of the profile (and --cw-tel-host is REQUIRED by
	// hw_run_parse_args); the four fields under test keep their real staged
	// relative order: profile, duration, far-send share, seed.
	write_text(ARGS_PATH,
			   "--cw-run-id staged_keepup_walk_6m_cand10 "
			   "--cw-tel-host pull --cw-tel-port 39017 --cw-tel-mode pull "
			   "--cw-profile keepup_walk_6m --cw-duration 360 "
			   "--cw-far-send-near-owed 16 --cw-seed 12345");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_KEEPUP_WALK_6M);
	assert(strcmp(cfg.profile_name, "keepup_walk_6m") == 0);
	assert(cfg.autopilot_keepup);
	assert(!cfg.autopilot_walk);
	assert(cfg.duration_seconds == 360);
	assert(cfg.far_send_near_owed == 16);
	assert(cfg.seed_set);
	assert(cfg.seed == 12345);
}

// Direct proof of fix #1: a config armed by --cw-profile (which flips real
// side-effect fields main.c reads UNCONDITIONALLY, not gated on
// cfg.enabled -- see hw_run.h's autopilot_unstick comment) followed by a
// later syntax error must leave cfg fully reset, not merely disabled.
static void test_profile_then_syntax_error_leaves_pristine_default(void) {
	write_text(ARGS_PATH,
			   "--cw-profile mp_lan_walk_10m --cw-tel-host 1.2.3.4 "
			   "--cw-bogus-flag");
	struct hw_run_config cfg;
	char err[128];
	memset(&cfg, 0x5A, sizeof(cfg));
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	remove(ARGS_PATH);
	assert(!ok);
	assert(!cfg.enabled);
	// Before the fix, these would have been left true by mp_lan_walk_10m's
	// hw_run_apply_profile side effects, even though ok/enabled are false.
	assert(!cfg.autopilot_unstick);
	assert(!cfg.mp_autoconnect);
	assert(!cfg.autopilot_walk);
	assert(!cfg.superflat);
	assert(cfg_is_pristine_default(&cfg));
}

// --- --cw-mp-endpoint (run-local mp_connect_smoke server override,
//     2026-09-12) ---------------------------------------------------------
//
// The flag exists so an unattended mp_connect_smoke run can be pointed at a
// chosen host:port without editing the staged card's config.json. Two
// properties are worth a test each: the accepted value reaches cfg VERBATIM
// (main.c injects that exact string as an ephemeral "mp.server", so any
// truncation/rewrite here would silently join a DIFFERENT server), and every
// shape that mp_address_parse would quietly "fix" downstream -- a missing,
// zero, or out-of-range port, an empty host -- is a hard parse failure here
// instead. Plus the MP-profiles-only gate, in BOTH argv orders.
static void mp_endpoint_expect_ok(const char* args, const char* expect) {
	write_text(ARGS_PATH, args);
	struct hw_run_config cfg;
	char err[128];
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	remove(ARGS_PATH);
	if(!ok)
		fprintf(stderr, "[mp_endpoint ok] \"%s\" rejected: %s\n", args, err);
	assert(ok);
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_MP_CONNECT_SMOKE
		   || cfg.profile == HW_RUN_PROFILE_MP_LAN_WALK_10M
		   || cfg.profile == HW_RUN_PROFILE_MP_RECONNECT_SOAK);
	assert(strcmp(cfg.mp_endpoint, expect) == 0);
}

static void mp_endpoint_expect_reject(const char* args) {
	write_text(ARGS_PATH, args);
	struct hw_run_config cfg;
	char err[128];
	// Poison first: a rejected --cw-mp-endpoint must leave cfg at the pristine
	// disabled default like every other error path (the TOTAL-ness contract),
	// never a half-applied profile with an endpoint attached.
	memset(&cfg, 0x5A, sizeof(cfg));
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	remove(ARGS_PATH);
	if(ok)
		fprintf(stderr, "[mp_endpoint reject] \"%s\" was ACCEPTED\n", args);
	assert(!ok);
	assert(cfg.mp_endpoint[0] == '\0');
	assert(cfg_is_pristine_default(&cfg));
}

static void test_mp_endpoint_valid_stored_verbatim(void) {
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_connect_smoke "
						  "--cw-mp-endpoint 192.0.2.72:25565",
						  "192.0.2.72:25565");
	// Hostname form and a non-default port, both verbatim.
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_connect_smoke "
						  "--cw-mp-endpoint beta-oracle.lan:25566",
						  "beta-oracle.lan:25566");
	// Bracketed IPv6 -- the other two-segment shape mp_address_parse splits.
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_connect_smoke "
						  "--cw-mp-endpoint [fe80::1]:25565",
						  "[fe80::1]:25565");
	// Port bounds that MUST be accepted (1 and 65535).
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_connect_smoke "
						  "--cw-mp-endpoint 10.0.0.5:1",
						  "10.0.0.5:1");
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_connect_smoke "
						  "--cw-mp-endpoint 10.0.0.5:65535",
						  "10.0.0.5:65535");
	// Order-independence: the flag BEFORE --cw-profile must reach the same
	// verdict (the gate is applied after the whole argv, like
	// --cw-far-view-distance's).
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-mp-endpoint 192.0.2.72:25565 "
						  "--cw-profile mp_connect_smoke",
						  "192.0.2.72:25565");
}

static void test_mp_endpoint_invalid_forms_rejected(void) {
	const char* prefix = "--cw-run-id r --cw-tel-host 1.1.1.1 "
						 "--cw-profile mp_connect_smoke --cw-mp-endpoint ";
	char args[640];
	const char* bad[] = {
		"192.0.2.72",      // no port: mp_address_parse would default 25565
		"192.0.2.72:",     // empty port segment: same silent default
		"192.0.2.72:0",    // port 0
		"192.0.2.72:65536",// port > 65535: would silently fall back to 25565
		"192.0.2.72:99999",// still 5 digits, still out of range
		"192.0.2.72:2a",   // non-digit port
		":25565",            // empty host (mp_address_parse accepts this)
		":",                 // mp_address_parse's own documented crash shape
		"[fe80::1]",         // bracketed host with no port
		"[fe80::1]:",        // bracketed host with an empty port
		"[fe80::1:25565",    // unterminated bracket
		"fe80::1:25565",     // >2 segments: collapses to host + default port
	};
	for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		snprintf(args, sizeof(args), "%s%s", prefix, bad[i]);
		mp_endpoint_expect_reject(args);
	}

	// Length boundary, BUILT rather than hand-counted so the two cases cannot
	// drift: 63 characters is the longest value that fits cfg.mp_endpoint with
	// its NUL and must be accepted; 64 must be REJECTED rather than silently
	// snprintf-truncated into a different host.
	const char* tail = ".lan:1";
	size_t tail_len = strlen(tail);
	for(size_t total = 63; total <= 64; total++) {
		char endpoint[72];
		memset(endpoint, 'a', sizeof(endpoint));
		memcpy(endpoint + (total - tail_len), tail, tail_len + 1);
		assert(strlen(endpoint) == total);
		snprintf(args, sizeof(args), "%s%s", prefix, endpoint);
		if(total == 63)
			mp_endpoint_expect_ok(args, endpoint);
		else
			mp_endpoint_expect_reject(args);
	}
	// A missing value after the flag is the standard need_value rejection.
	mp_endpoint_expect_reject("--cw-run-id r --cw-tel-host 1.1.1.1 "
							  "--cw-profile mp_connect_smoke "
							  "--cw-mp-endpoint");
}

// Smoke-only by construction: any other profile is a hard failure, not a
// silently ignored flag -- a harness that asks far_walk_6m to join a server
// has a bug, and a run that ignored the request would look like it honored it.
static void test_mp_endpoint_other_profiles_rejected(void) {
	mp_endpoint_expect_reject("--cw-run-id r --cw-tel-host 1.1.1.1 "
							  "--cw-profile far_walk_6m "
							  "--cw-mp-endpoint 192.0.2.72:25565");
	// Including the other two MP profiles, whose measurements are tied to a
	// known server, ...

	// The other two MP profiles ACCEPT it (widened 2026-09-12: a LAN
	// regression run must be able to target the local mc173 peer while the
	// card's saved mp.server points elsewhere).
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_lan_walk_10m "
						  "--cw-mp-endpoint 192.0.2.72:25565",
						  "192.0.2.72:25565");
	mp_endpoint_expect_ok("--cw-run-id r --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_reconnect_soak "
						  "--cw-mp-endpoint 192.0.2.72:25565",
						  "192.0.2.72:25565");
	// ... and the no-explicit-profile case, which lands on the far_idle_2m
	// default rather than mp_connect_smoke.
	mp_endpoint_expect_reject("--cw-run-id r --cw-tel-host 1.1.1.1 "
							  "--cw-mp-endpoint 192.0.2.72:25565");
	// Order-independence on the rejection side too: a later --cw-profile that
	// moves the run OFF mp_connect_smoke must still reject.
	mp_endpoint_expect_reject("--cw-run-id r --cw-tel-host 1.1.1.1 "
							  "--cw-profile mp_connect_smoke "
							  "--cw-mp-endpoint 192.0.2.72:25565 "
							  "--cw-profile far_walk_6m");
}

// Default behavior is byte-identical when the flag is absent: an
// mp_connect_smoke run with no override keeps reading the card's own
// mp.server (cfg.mp_endpoint stays empty, and main.c injects nothing).
static void test_mp_endpoint_absent_is_unchanged(void) {
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_connect_smoke");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_MP_CONNECT_SMOKE);
	assert(cfg.mp_autoconnect); // the profile's own side effect, untouched
	assert(cfg.mp_endpoint[0] == '\0');
}

/* ---- mp_play_10m (push-0921/B4c) ---------------------------------------
 *
 * The unattended LAN play/rejoin profile. Its shape is mp_chat_login_3m's
 * join (no autoload, mp_autoconnect, driver-owned completion) stretched to a
 * 21-minute window, and the thing it must NOT do is arm any of the scripted
 * harness behaviours: the play driver steers the walker through autopilot.c's
 * runtime seam, one leg at a time, and Astra gate 1 Lane B item 5 forbids any
 * automatic respawn ("No automatic respawn, teleport-assisted displacement or
 * fixture restoration may conceal failure"). The three config-armed
 * behaviours that could produce one are autopilot_walk (mode 0, which is the
 * mode whose autopilot_force_button scripts screen_death's Respawn hold),
 * autopilot_keepup and autopilot_aim/autopilot_place -- all of them must stay
 * off for this profile. test_mp_play_10m_arms_no_death_click below pins that
 * and carries the rest of the argument.
 *
 * NAME-KEYED, not enum-keyed, on purpose: this file must keep compiling
 * before the enum value exists, so the RED half of the tests-first pair is a
 * failing ASSERT (an unregistered name) rather than a build break. */
#define MP_PLAY_10M_DURATION_SECONDS 1260

static void test_split_portal_quit_profile(void) {
	struct hw_run_config cfg;
	enum hw_run_profile profile;
	char err[128];
	assert(hw_run_profile_from_name("split_portal_quit_4m", &profile));
	assert(strcmp(hw_run_profile_name(profile), "split_portal_quit_4m") == 0);
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile "
		"split_portal_quit_4m --cw-portal-remaining-ticks 3");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof err));
	assert(cfg.duration_seconds == 240);
	assert(cfg.split_guest && cfg.split_recovery);
	assert(cfg.portal_remaining_ticks == 3);
	assert(cfg.driver_owns_completion);
	write_text(ARGS_PATH, "--cw-profile split_portal_quit_4m "
		"--cw-portal-remaining-ticks 4");
	assert(!hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof err));
}

static void test_split_dims_profile(void) {
	struct hw_run_config cfg;
	enum hw_run_profile profile;
	char err[128];
	assert(hw_run_profile_from_name("split_dims_6m", &profile));
	assert(strcmp(hw_run_profile_name(profile), "split_dims_6m") == 0);
	/* the profile itself pins Peaceful; no runner -Set game.difficulty=0 */
	assert(hw_run_profile_forces_peaceful(profile));
	assert(!hw_run_profile_forces_peaceful(HW_RUN_PROFILE_SPLIT_PLAY_8M));
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile "
		"split_dims_6m --cw-portal-remaining-ticks 3");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof err));
	assert(cfg.duration_seconds == 360);
	assert(cfg.split_guest && cfg.split_guest_walk && !cfg.split_recovery);
	assert(cfg.split_guest_yaw_deg == 0); /* +z: into the showcase pane */
	assert(!cfg.autopilot_keepup && !cfg.autopilot_walk); /* host stays */
	assert(cfg.superflat && cfg.force_overworld);
	assert(cfg.seed_set && cfg.seed == 12345 && cfg.seed_from_profile);
	assert(cfg.portal_remaining_ticks == 3); /* accepted, inert */
	assert(!cfg.driver_owns_completion);
	/* Readiness-epoch window end, like split_play_8m. */
	assert(!hw_run_tps_window_end(&cfg, 999.0F, false, 999000u));
	assert(!hw_run_tps_window_end(&cfg, 0.0F, true, 357999u));
	assert(hw_run_tps_window_end(&cfg, 0.0F, true, 358000u));
	/* An explicit seed still wins. */
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-seed 77 --cw-profile "
		"split_dims_6m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof err));
	assert(cfg.seed == 77);
}

static void test_mp_play_10m_profile_fields(void) {
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile mp_play_10m");
	struct hw_run_config cfg;
	char err[128];
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	remove(ARGS_PATH);
	if(!ok)
		fprintf(stderr, "[mp_play_10m] rejected: %s\n", err);
	assert(ok);
	assert(cfg.enabled);
	assert(strcmp(cfg.profile_name, "mp_play_10m") == 0);
	/* 1260 s = 21 min. The driver's own 1,200,000 ms total cap
	 * (MPP_TOTAL_CAP_MS, mp_play_core.h) fires INSIDE this window, so a stuck
	 * run always reports its own failure instead of being cut off by the
	 * profile clock -- the split_recovery_6m / mp_chat_login_3m precedent. */
	assert(cfg.duration_seconds == MP_PLAY_10M_DURATION_SECONDS);
	/* Only the driver may complete the run (main.c's generic duration-based
	 * "ok" is excluded for driver_owns_completion profiles). */
	assert(cfg.driver_owns_completion);
	/* No autoload: a loaded world can then ONLY be the remote install, which
	 * is what the readiness marker is measured from. */
	assert(!cfg.autoload_new);
	assert(cfg.autoload_name[0] == '\0');
	assert(cfg.mp_autoconnect);
	/* The server drives what the client receives; the platform default reach
	 * stays in force (-1 = "leave config.json alone"). */
	assert(cfg.far_view_distance == -1);
}

/* Astra gate 1 Lane B item 5: no automatic respawn. The proof has two halves
 * and this is the CONFIG half -- nothing this profile sets can put the
 * autopilot into a mode that scripts screen_death's Respawn button.
 *
 * autopilot_force_button (autopilot.c) forces IB_GUI_CLICK on screen_death
 * ONLY when gstate.autopilot.active is true AND gstate.autopilot.mode != 1;
 * input.c gates every one of its three call sites on gstate.autopilot.active
 * (input_autopilot_edges_advance, input_pressed, input_held). main.c writes
 * "debug.autopilot" -- the only config path to active/mode -- from exactly
 * three fields: autopilot_walk ("walk"), autopilot_keepup ("keepup") and
 * autopilot_aim ("aim", which also gates autopilot_place). All three false
 * here means the config can never arm it.
 *
 * The RUNTIME half needs saying out loud, because the script's own walk seam
 * DOES set active = true / mode = 0 (autopilot.c ap_script_apply), which is
 * the exact state autopilot_force_button scripts the Respawn hold in. Three
 * things make it unreachable anyway, and none of them is this test:
 *
 *   1. A death ENDS the run on the frame it is observed. mp_play_core.h's
 *      continuous latch -- `(in_world || in_world2) && (o->health <= 0 ||
 *      o->screen == MPP_SCREEN_DEATH)` -> mpp_fail(s, "died") -- sets
 *      phase = MPP_PH_FAILED, and mpp_done() reads that immediately; there is
 *      no "next frame" in which a respawn could be wanted.
 *   2. mp_play_frame's LAST call on that same frame is mpp_finish(), whose
 *      first-done branch calls autopilot_script_walk(false) and
 *      autopilot_script_aim(false, 0, 0). ap_script_apply then hands the
 *      PRE-SEAM autopilot state back -- and for this profile that state is
 *      "off", which is exactly what the asserts below pin.
 *   3. That happens BEFORE any consumer can sample it: mp_play_frame runs
 *      after player 0's screen update and before the input_poll at the bottom
 *      of main's loop, and all three readers of autopilot_force_button
 *      (input_autopilot_edges_advance, input_pressed, input_held) are gated
 *      on gstate.autopilot.active, which the release just cleared.
 *
 * Belt and braces, from autopilot.h's own formula: the FIRST walk-mode tick
 * on the death screen is RELEASED by construction --
 * autopilot_death_click_held(1) == ((1 - 1) % 30 != 0) == false -- so even a
 * tick that slipped through between the death and the release could not press
 * the button. That identity is already pinned by
 * tests/test_autopilot_death_recovery.c (its "released for exactly one tick
 * at tick 1" case); it is cited rather than re-asserted here because this
 * target links hw_run.c alone, by design.
 *
 * Only the CONFIG half is testable from here, and that is what follows. */
static void test_mp_play_10m_arms_no_death_click(void) {
	write_text(ARGS_PATH, "--cw-tel-host 1.1.1.1 --cw-profile mp_play_10m");
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(!cfg.autopilot_walk);
	assert(!cfg.autopilot_keepup);
	assert(!cfg.autopilot_aim);
	assert(!cfg.autopilot_place);
	assert(!cfg.autopilot_unstick);
	/* And none of the other scripted drivers either -- two drivers arming at
	 * once is how a run ends up with a second owner of player 0. */
	assert(!cfg.menu_autopilot);
	assert(!cfg.split_guest);
	assert(!cfg.split_guest_walk);
	assert(!cfg.split_guest_actions);
	assert(!cfg.split_recovery);
	assert(!cfg.mp_reconnect_soak);
	assert(!cfg.watchdog_inject);
	assert(!cfg.redstone_clock_stress);
	assert(!cfg.save_migrate);
	assert(!cfg.perf_stress);
	/* No fixture terrain override: this profile plays in the server's world. */
	assert(!cfg.superflat);
	assert(!cfg.force_overworld);
	assert(!cfg.seed_set);
	/* THE FORCED HUD IS OFF, and for this profile that is a functional
	 * requirement, not a preference (push-0921/B9). The chain, end to end:
	 *
	 *   hw_run_apply_profile defaults cfg->force_overlay = true (hw_run.c:192)
	 *   -> main.c:1808-1810 writes "debug.overlay" = "force" into config_user,
	 *      OVERRIDING the card's own config.json, which is loaded first
	 *      (main.c:1774) -- so no rig config can opt out of it
	 *   -> gstate.force_debug_overlay (main.c:2011)
	 *   -> screen_ingame.c:717 `!gstate.force_debug_overlay && step_right`
	 *      DISCARDS the hotbar step, and main.c:4782-4787 spends the very same
	 *      IB_SCROLL_RIGHT press on the far-ring LOD reach dial instead.
	 *
	 * This profile's driver presses MPP_ACT_HOTBAR_NEXT (-> IB_SCROLL_RIGHT,
	 * mp_play_map.h:98) whenever an inventory move has left the held slot
	 * without a placeable block, and gives up after MPP_PLACE_AIM_BUDGET_MS with
	 * build_skipped "hotbar never reached a block" (mp_play_core.h:932-934). The
	 * contract's >= 8 matched places is then unreachable and the device fails
	 * "action floors not met" at p2_end (mp_play_core.h:740-742) -- on real
	 * hardware exactly as on PC, since nothing in this chain is platform-gated.
	 *
	 * mp_chat_chord_1m is the precedent (hw_run.c:485): it clears the same flag
	 * because a forced HUD suppresses the F3/chord toggle it exists to measure.
	 * Two profiles now need the flag off for two different reasons, which is why
	 * test_profile_table_pinned above carries force_overlay as a COLUMN -- every
	 * other profile's value is pinned there too, so turning it off here cannot
	 * quietly turn it off anywhere else. */
	assert(!cfg.force_overlay);
}

/* The canonical name must round-trip, and it must be the LAST registered
 * profile -- appended, so no existing enum value moved. */
static void test_mp_play_10m_name_enum_round_trip(void) {
	enum hw_run_profile p;
	assert(hw_run_profile_from_name("mp_play_10m", &p));
	assert(strcmp(hw_run_profile_name(p), "mp_play_10m") == 0);
	/* Appended after mp_chat_chord_1m: nothing above it moved. */
	enum hw_run_profile chord;
	assert(hw_run_profile_from_name("mp_chat_chord_1m", &chord));
	assert((int)p == (int)chord + 1);
	/* No alias is registered for it (the collector, the rig and the staged
	 * arg files all spell the canonical name), and a near-miss must not
	 * silently resolve to something else. */
	enum hw_run_profile bogus = (enum hw_run_profile)0x5A;
	assert(!hw_run_profile_from_name("mp_play", &bogus));
	assert(!hw_run_profile_from_name("mp_play_10", &bogus));
	assert(!hw_run_profile_from_name("mp_play_10m ", &bogus));
}

/* --cw-mp-endpoint / --cw-mp-username are gated on the MP-profile set. The
 * play profile joins the same run-local mc173-diag fixture the chat-login
 * profile does, under a per-run nonce the collector binds receipts to, so it
 * must be inside that gate -- and a run that silently ignored either flag
 * would join the card's saved server under the card's saved name and look
 * like it had honoured the request. */
static void test_mp_play_10m_takes_the_endpoint_and_the_nonce(void) {
	write_text(ARGS_PATH, "--cw-run-id r42 --cw-tel-host 1.1.1.1 "
						  "--cw-profile mp_play_10m "
						  "--cw-mp-endpoint 192.0.2.72:25599 "
						  "--cw-mp-username cw1a2b3c");
	struct hw_run_config cfg;
	char err[128];
	bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
	remove(ARGS_PATH);
	if(!ok)
		fprintf(stderr, "[mp_play_10m flags] rejected: %s\n", err);
	assert(ok);
	assert(cfg.enabled);
	assert(strcmp(cfg.profile_name, "mp_play_10m") == 0);
	assert(strcmp(cfg.mp_endpoint, "192.0.2.72:25599") == 0);
	assert(strcmp(cfg.mp_username, "cw1a2b3c") == 0);
	/* Order-independence, like every other post-loop gate: the flags written
	 * BEFORE the profile reach the same verdict. */
	write_text(ARGS_PATH, "--cw-run-id r42 --cw-tel-host 1.1.1.1 "
						  "--cw-mp-endpoint 192.0.2.72:25599 "
						  "--cw-mp-username cw1a2b3c "
						  "--cw-profile mp_play_10m");
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	remove(ARGS_PATH);
	assert(strcmp(cfg.mp_endpoint, "192.0.2.72:25599") == 0);
	assert(strcmp(cfg.mp_username, "cw1a2b3c") == 0);
	/* And a LATER --cw-profile that moves the run OFF mp_play_10m still
	 * rejects both, because the gate reads the FINAL profile. */
	mp_endpoint_expect_reject("--cw-run-id r --cw-tel-host 1.1.1.1 "
							  "--cw-profile mp_play_10m "
							  "--cw-mp-endpoint 192.0.2.72:25599 "
							  "--cw-profile far_walk_6m");
}

/* The 21-profile table. Every registered profile's name and the handful of
 * fields that decide what a run DOES, pinned by ordinal so a new profile can
 * neither move an existing value nor arrive without a row here. Keyed on the
 * NAME rather than the enum constant so this file still compiles against a
 * tree where the new value has not landed yet (the row then fails its
 * strcmp, which is the RED). */
static void test_profile_table_pinned(void) {
	static const struct {
		const char* name;
		int duration;
		bool driver_owns_completion;
		bool autoload_new;
		bool mp_autoconnect;
		bool autopilot_walk;
		bool autopilot_keepup;
		bool autopilot_aim;
		bool autopilot_place;
		bool menu_autopilot;
		bool split_guest;
		bool split_recovery;
		int far_view_distance;
		/* push-0921/B9. THE FORCED HUD IS NOT COSMETIC, which is why it is
		 * pinned per profile here rather than left to hw_run_apply_profile's
		 * default. force_overlay = true makes main.c write
		 * "debug.overlay" = "force" OVER whatever the card's config.json says
		 * (main.c:1808-1810), and gstate.force_debug_overlay then REPURPOSES the
		 * hotbar scroll: screen_ingame.c:717 discards the hotbar step
		 * (`!gstate.force_debug_overlay && step_right`) and main.c:4782-4787
		 * turns the very same press into the far-ring LOD reach dial. A profile
		 * whose driver cycles the held slot therefore cannot place a block, on
		 * PC or on real hardware, while this column says 1. */
		bool force_overlay;
	} table[] = {
		{"far_idle_2m",          120, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,  6, 1},
		{"heartbeat_selftest",    60, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		{"far_walk_6m",          360, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0,  6, 1},
		{"perf_stress_5m",       300, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,  6, 1},
		{"watchdog_inject",       30, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		{"redstone_clock_2m",    120, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, -1, 1},
		{"menus_dwell",          240, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, -1, 1},
		{"save_migrate",         300, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		{"mp_connect_smoke",     120, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		{"mp_lan_walk_10m",      600, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, -1, 1},
		{"mp_reconnect_soak",    480, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		{"menus_dwell_creative", 240, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, -1, 1},
		{"keepup_walk_6m",       360, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0,  6, 1},
		{"split_anchor_6m",      360, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0,  6, 1},
		{"split_play_2m",        120, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0,  6, 1},
		{"split_play_8m",        360, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0,  6, 1},
		{"split_actions_6m",     360, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0,  6, 1},
		{"split_recovery_6m",    360, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,  6, 1},
		{"mp_chat_login_3m",     180, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		/* OFF since b1-chat-opener-0919: a forced HUD is re-asserted every frame
		 * and suppresses the very F3/chord toggle this profile measures
		 * (hw_run.c's own case comment). The PRECEDENT for the row below. */
		{"mp_chat_chord_1m",      60, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, 0},
		/* push-0921/B4c. APPENDED: every row above keeps its ordinal.
		 * force_overlay OFF since push-0921/B9, for the mechanism in the column
		 * comment above: this profile's driver presses MPP_ACT_HOTBAR_NEXT to
		 * bring a placeable block into the held slot, and the contract's >= 8
		 * matched places (docs/mp_play_profile.md section 5) is unreachable
		 * while that press is being eaten by the reach dial. Measured, not
		 * argued: run pc_mp_play_20260921_131211 moved its only stack from
		 * hotbar 0 to hotbar 1 with an inventory move and then spent the full
		 * 6 s MPP_PLACE_AIM_BUDGET_MS of cycles 1 and 2 -- about 34 presses --
		 * reporting build_skipped "hotbar never reached a block", while the same
		 * virtual pad delivered attack, use, open-inventory and GUI clicks in
		 * those same phases; places finished at 0 of 8. */
		{"mp_play_10m",         1260, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, 0},
		{"mp_pause_menu_3m",    180, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, 0},
		{"split_portal_quit_4m", 240, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1,  6, 1},
		/* splitdims-profile-0926: walk-based, generic completion, no phase
		 * driver (was the C1 placeholder on the recovery driver). */
		{"split_dims_6m",       360, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0,  6, 1},
		/* motion-proof-0927: the mob motion S2 fixture (superflat, no walk). */
		{"mob_motion_2m",       120, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		/* vehicle-smoke-1004: the host vehicle smoke (driver-owned, guest
		 * auto-joined but not walked, its own rig world, no autoload). */
		{"host_vehicle_smoke_8m", 500, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, -1, 1},
		/* colcap-1006 R5 item 2: the column-pool self-test (seed 12345,
		 * no input, no far ring; completion owned by pool_selftest.c via
		 * its own latch, not driver_owns_completion). */
		{"pool_selftest_3m",    180, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		/* colcap-1006 R5 item 3: split_anchor_6m + the anchor cycle. */
		{"split_regrow_8m",     360, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0,  6, 1},
		/* integ-1011: offered-guest piston proof; driver-owned like the vehicle smoke. */
		{"split_offer_piston_2m", 140, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, -1, 1},
		/* ui-build-1026/T16a: the Back up & convert proof route (its own
		 * copy walker, not menu_autopilot; no autoload, no far ring). */
		{"world_copy_convert",  300, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1},
		/* SPEC R12: save_reload_3m -- its own phase driver owns completion
		 * (title -> create -> fixture -> pause Save and quit -> named
		 * reload); no autoload, no walk, no menu_autopilot script, no far
		 * ring. */
		{"save_reload_3m",      180, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1},
	};
	const size_t rows = sizeof table / sizeof *table;
	struct hw_run_config cfg;
	unsigned failures = 0;
	for(size_t i = 0; i < rows; i++) {
		enum hw_run_profile p;
		if(!hw_run_profile_from_name(table[i].name, &p)) {
			fprintf(stderr, "FAIL profile \"%s\" is not registered\n",
					table[i].name);
			failures++;
			continue;
		}
		if((size_t)p != i) {
			fprintf(stderr, "FAIL profile \"%s\" moved to ordinal %d "
					"(expected %zu) -- an enum value was INSERTED, not "
					"appended\n", table[i].name, (int)p, i);
			failures++;
			continue;
		}
		hw_run_config_init(&cfg);
		hw_run_apply_profile(&cfg, p);
		assert(strcmp(hw_run_profile_name(p), table[i].name) == 0);
		assert(strcmp(cfg.profile_name, table[i].name) == 0);
		assert(cfg.duration_seconds == table[i].duration);
		assert(cfg.driver_owns_completion == table[i].driver_owns_completion);
		assert(cfg.autoload_new == table[i].autoload_new);
		assert(cfg.mp_autoconnect == table[i].mp_autoconnect);
		assert(cfg.autopilot_walk == table[i].autopilot_walk);
		assert(cfg.autopilot_keepup == table[i].autopilot_keepup);
		assert(cfg.autopilot_aim == table[i].autopilot_aim);
		assert(cfg.autopilot_place == table[i].autopilot_place);
		assert(cfg.menu_autopilot == table[i].menu_autopilot);
		assert(cfg.split_guest == table[i].split_guest);
		assert(cfg.split_recovery == table[i].split_recovery);
		assert(cfg.far_view_distance == table[i].far_view_distance);
		if(cfg.force_overlay != table[i].force_overlay) {
			fprintf(stderr, "FAIL profile \"%s\" force_overlay=%d, pinned %d "
					"-- see this table's force_overlay comment: the forced HUD "
					"suppresses the hotbar step and steals its press\n",
					table[i].name, (int)cfg.force_overlay,
					(int)table[i].force_overlay);
			failures++;
		}
	}
	assert(!failures);
	/* The table covers the WHOLE enum: one past the last row is not a
	 * registered profile (hw_run_profile_name's default arm answers
	 * far_idle_2m for an out-of-range value, so the check is that the NAME
	 * the last row + 1 reports is not its own distinct registration). */
	assert(strcmp(hw_run_profile_name((enum hw_run_profile)rows),
				  "far_idle_2m") == 0);
	printf("  profile table: %zu profiles pinned by ordinal, name, duration, "
		   "the seven arming flags and the forced-HUD column\n", rows);
}

// ---- Deterministic fuzz loop -----------------------------------------

static uint32_t xorshift32(uint32_t* state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

#define FUZZ_ITERATIONS 10000
#define FUZZ_MAX_LEN 600 // spans both under and over the 511-byte cap

static void test_fuzz_never_crashes_and_stays_bounded(void) {
	uint32_t rng = 0x9E3779B9u; // fixed seed: fully reproducible
	for(int iter = 0; iter < FUZZ_ITERATIONS; iter++) {
		unsigned char buf[FUZZ_MAX_LEN];
		size_t len = xorshift32(&rng) % (FUZZ_MAX_LEN + 1);
		for(size_t i = 0; i < len; i++) {
			uint32_t r = xorshift32(&rng);
			// Bias toward the parser's actual decision points (delimiters,
			// '#', '-', NUL, low control bytes) rather than pure uniform
			// noise, which would rarely land on any of them at this length.
			switch(r % 8) {
				case 0: buf[i] = ' '; break;
				case 1:
					buf[i] = ((r >> 8) % 3 == 0) ? '\t'
								: (((r >> 8) % 3 == 1) ? '\r' : '\n');
					break;
				case 2: buf[i] = '-'; break;
				case 3: buf[i] = '#'; break;
				case 4: buf[i] = '\0'; break;
				case 5: buf[i] = (unsigned char)(0x20 + (r >> 8) % 95); break;
				case 6: buf[i] = (unsigned char)((r >> 8) % 0x20); break;
				default: buf[i] = (unsigned char)((r >> 8) & 0xFFu); break;
			}
		}
		write_file(ARGS_PATH, buf, len);
		struct hw_run_config cfg;
		char err[128];
		// hw_run_config_init(), not raw poison: a random draw very often
		// lands on the documented "nothing to parse" fast path (zero real
		// tokens after delimiters/'#'), which deliberately leaves cfg
		// untouched -- the same realistic precondition run_vector() uses
		// above, for the identical reason.
		hw_run_config_init(&cfg);
		bool ok = hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err));
		remove(ARGS_PATH);
		if(!ok) {
			assert(cfg_is_pristine_default(&cfg));
		} else {
			// Bounded/sane result: every fixed-size string field stayed
			// NUL-terminated within its own buffer (defensive, on top of
			// the fact this iteration didn't crash at all), and the
			// enabled/telemetry_host invariant hw_run_parse_args itself
			// enforces still holds.
			assert(memchr(cfg.profile_name, '\0', sizeof(cfg.profile_name)));
			assert(memchr(cfg.run_id, '\0', sizeof(cfg.run_id)));
			assert(memchr(cfg.telemetry_host, '\0', sizeof(cfg.telemetry_host)));
			assert(memchr(cfg.telemetry_netmask, '\0',
						  sizeof(cfg.telemetry_netmask)));
			assert(memchr(cfg.autoload_name, '\0', sizeof(cfg.autoload_name)));
			if(cfg.enabled)
				assert(cfg.telemetry_host[0] != '\0');
		}
	}
}

/* ---- debug.hw_overlay: the capture switch (brief 25 item 4) --------------
 * EVERY hardware profile forces the F3 overlay (hw_run_apply_profile sets
 * force_overlay = true; only mp_chat_chord_1m turns it back off, because
 * "debug.overlay" = "force" suppresses the very live F3 toggle that profile
 * exists to observe). main.c applies that by writing "debug.overlay" =
 * "force" into the run config, so no console run could ever photograph a
 * clean HUD.
 *
 * hw_run_force_overlay is the ONE run-scoped switch: the key's value is read
 * where hw_run is applied and passed in (absent => the caller's default 1).
 * Absent or non-zero is today's behaviour EXACTLY; 0 declines the force and
 * the overlay then follows "debug.overlay" like normal play. It reads the
 * profile's own field and writes NOTHING, so no profile's verdict rules can
 * move and the chord profile's override stands. */
static void test_hw_overlay_switch_equivalence(void) {
	int forced = 0, unforced = 0;
	for(int p = 0; p <= (int)HW_RUN_PROFILE_MP_CHAT_CHORD_1M; p++) {
		struct hw_run_config cfg;
		hw_run_config_init(&cfg);
		hw_run_apply_profile(&cfg, (enum hw_run_profile)p);
		bool today = cfg.force_overlay;
		/* The table itself is unchanged: every profile but the chord one
		 * still forces the overlay, which is what the verdict rules read. */
		assert(today == (p != (int)HW_RUN_PROFILE_MP_CHAT_CHORD_1M)
			   && "a profile's force_overlay moved");
		/* EQUIVALENCE: absent (the caller's default) and an explicit 1 are
		 * byte-for-byte today's decision, for every profile in the table. */
		assert(hw_run_force_overlay(&cfg, 1) == today);
		assert(hw_run_force_overlay(&cfg, 2) == today
			   && "only an explicit 0 may decline the force");
		assert(hw_run_force_overlay(&cfg, -1) == today);
		/* 0 declines it -- and never touches the profile's own field, so
		 * "debug.overlay" is simply left as the config wrote it. */
		assert(!hw_run_force_overlay(&cfg, 0));
		assert(cfg.force_overlay == today
			   && "the switch mutated the profile it read");
		struct hw_run_config fresh;
		hw_run_config_init(&fresh);
		hw_run_apply_profile(&fresh, (enum hw_run_profile)p);
		assert(!memcmp(&cfg, &fresh, sizeof cfg)
			   && "the switch is not a pure read of the config");
		if(today)
			forced++;
		else
			unforced++;
	}
	/* The chord profile, named: it turns force_overlay off itself and the
	 * switch cannot turn it back on. */
	{
		struct hw_run_config cfg;
		hw_run_config_init(&cfg);
		hw_run_apply_profile(&cfg, HW_RUN_PROFILE_MP_CHAT_CHORD_1M);
		assert(!cfg.force_overlay && !hw_run_force_overlay(&cfg, 1)
			   && !hw_run_force_overlay(&cfg, 0)
			   && "mp_chat_chord_1m's own override must stand");
	}
	assert(!hw_run_force_overlay(NULL, 1) && !hw_run_force_overlay(NULL, 0)
		   && "a NULL config must force nothing");
	assert(unforced == 1 && forced > 1);
	printf("test_hw_run_args_parse: debug.hw_overlay equivalence over %d "
		   "profiles (%d force, %d do not) PASS\n", forced + unforced, forced,
		   unforced);
}

static void test_staged_autoload_name_with_spaces(void) {
	struct hw_run_config cfg;
	char err[256];
	write_text(ARGS_PATH, "--cw-tel-host pull --cw-profile far_idle_2m --cw-autoload-name-hex "
		"4361766557696920546573742031\n");
	hw_run_config_init(&cfg);
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(strcmp(cfg.autoload_name, "CaveWii Test 1") == 0);
	assert(cfg.profile == HW_RUN_PROFILE_FAR_IDLE_2M);
	write_text(ARGS_PATH, "--cw-tel-host pull --cw-autoload-name-hex "
		"4361766557696920546573742031 --cw-profile far_idle_2m\n");
	hw_run_config_init(&cfg);
	assert(hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
	assert(strcmp(cfg.autoload_name, "CaveWii Test 1") == 0);
	const char* bad[] = {"0", "gg", "410042", "410942", "417f42", "2f",
		"5c", "2e", "2e2e", "2b6e6577"};
	for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char args[256];
		snprintf(args, sizeof(args),
			"--cw-tel-host pull --cw-profile far_idle_2m --cw-autoload-name-hex %s", bad[i]);
		write_text(ARGS_PATH, args);
		hw_run_config_init(&cfg);
		assert(!hw_run_parse_file(&cfg, ARGS_PATH, err, sizeof(err)));
		assert(cfg_is_pristine_default(&cfg));
	}
	char oversized[1024];
	memset(oversized, '4', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	char* argv[] = {"test", "--cw-autoload-name-hex", oversized};
	hw_run_config_init(&cfg);
	assert(!hw_run_parse_args(&cfg, 3, argv, err, sizeof(err)));
	assert(cfg_is_pristine_default(&cfg));
	remove(ARGS_PATH);
	puts("test_hw_run_args_parse: staged save name spaces and rejection PASS");
}

/* residency-1005/4 closure item 12: the default-off runtime residency fault
 * arm (route E), --cw-rz-fault <host|guest>@<1..3600 s>. Absent = off;
 * valid values set exactly the three fields; every malformed value refuses
 * the run (pristine default), never arms. */
static void test_rz_fault_arm(void) {
	struct hw_run_config cfg;
	char err[256];
	hw_run_config_init(&cfg);
	assert(!cfg.rz_fault_set);
	char* off[] = {"test", "--cw-tel-host", "pull", "--cw-profile",
		"split_play_8m"};
	assert(hw_run_parse_args(&cfg, 5, off, err, sizeof err));
	assert(!cfg.rz_fault_set);
	char* host[] = {"test", "--cw-tel-host", "pull", "--cw-rz-fault",
		"host@60"};
	hw_run_config_init(&cfg);
	assert(hw_run_parse_args(&cfg, 5, host, err, sizeof err));
	assert(cfg.rz_fault_set && cfg.rz_fault_actor == 0
		&& cfg.rz_fault_delay_s == 60 && cfg.enabled);
	char* guest[] = {"test", "--cw-tel-host", "pull", "--cw-profile",
		"split_play_8m", "--cw-rz-fault", "guest@3600"};
	hw_run_config_init(&cfg);
	assert(hw_run_parse_args(&cfg, 7, guest, err, sizeof err));
	assert(cfg.rz_fault_set && cfg.rz_fault_actor == 1
		&& cfg.rz_fault_delay_s == 3600
		&& cfg.profile == HW_RUN_PROFILE_SPLIT_PLAY_8M);
	const char* bad[] = {"guest", "guest@", "guest@0", "guest@3601",
		"guest@-5", "guest@12x", "both@10", "Host@10", "@10", ""};
	for(size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		char* argv[] = {"test", "--cw-tel-host", "pull", "--cw-rz-fault",
			(char*)bad[i]};
		hw_run_config_init(&cfg);
		assert(!hw_run_parse_args(&cfg, 5, argv, err, sizeof err));
		assert(cfg_is_pristine_default(&cfg) && !cfg.rz_fault_set);
	}
	char* missing[] = {"test", "--cw-rz-fault"};
	hw_run_config_init(&cfg);
	assert(!hw_run_parse_args(&cfg, 2, missing, err, sizeof err));
	assert(cfg_is_pristine_default(&cfg));
	puts("test_hw_run_args_parse: --cw-rz-fault arm PASS");
}

static void test_periodic_sd_trace_exact_profile_set(void) {
	/* Preserve every pre-trial exclusion; ONLY play_8m/actions_6m are added.
	 * Sweep the complete enum so a diagnostic profile accidentally silenced
	 * by a range check is caught too. */
	const enum hw_run_profile quiet[] = {
		HW_RUN_PROFILE_FAR_IDLE_2M, HW_RUN_PROFILE_FAR_WALK_6M,
		HW_RUN_PROFILE_KEEPUP_WALK_6M, HW_RUN_PROFILE_SPLIT_ANCHOR_6M,
		HW_RUN_PROFILE_SPLIT_REGROW_8M, HW_RUN_PROFILE_SPLIT_PLAY_2M,
		HW_RUN_PROFILE_SPLIT_PLAY_8M, HW_RUN_PROFILE_SPLIT_ACTIONS_6M,
		HW_RUN_PROFILE_SPLIT_RECOVERY_6M, HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M,
		HW_RUN_PROFILE_SPLIT_DIMS_6M, HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M,
		HW_RUN_PROFILE_PERF_STRESS_5M, HW_RUN_PROFILE_MP_LAN_WALK_10M,
		HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M, /* strict tel_drop==0 evidence profile */
	};
	assert(!hw_run_periodic_sd_trace(NULL));
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	for(int p = HW_RUN_PROFILE_FAR_IDLE_2M; p <= HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M; p++) {
		bool expected = true;
		for(size_t i = 0; i < sizeof quiet / sizeof *quiet; i++)
			if(p == (int)quiet[i]) expected = false;
		cfg.profile = p;
		cfg.enabled = false;
		assert(!hw_run_periodic_sd_trace(&cfg));
		cfg.enabled = true;
		assert(hw_run_periodic_sd_trace(&cfg) == expected);
	}
	puts("PASS: SD trace exclusions = pre-trial set + split_play_8m/split_actions_6m trial + split_offer_piston_2m");
}

/* ui-build-1026/T16a: --cw-wcc-variant / --cw-wcc-root belong to
 * world_copy_convert only, the profile needs a root, and both are validated. */
static void test_world_copy_convert_flags(void) {
	struct hw_run_config cfg;
	char err[256];
	char* ok[] = {"t", "--cw-tel-host", "pull", "--cw-profile",
		"world_copy_convert", "--cw-wcc-variant", "write_error",
		"--cw-wcc-root", "wcc_ok_20261027"};
	hw_run_config_init(&cfg);
	assert(hw_run_parse_args(&cfg, 9, ok, err, sizeof err));
	assert(cfg.world_copy_convert && cfg.wcc_variant == 2
		   && strcmp(cfg.wcc_root, "wcc_ok_20261027") == 0
		   && !cfg.menu_autopilot && !cfg.autoload_new);
	char* dflt[] = {"t", "--cw-tel-host", "pull", "--cw-profile",
		"world_copy_convert", "--cw-wcc-root", "r1"};
	hw_run_config_init(&cfg);
	assert(hw_run_parse_args(&cfg, 7, dflt, err, sizeof err));
	assert(cfg.wcc_variant == 0);
	char* noroot[] = {"t", "--cw-tel-host", "pull", "--cw-profile",
		"world_copy_convert"};
	hw_run_config_init(&cfg);
	assert(!hw_run_parse_args(&cfg, 5, noroot, err, sizeof err));
	char* badroot[] = {"t", "--cw-tel-host", "pull", "--cw-profile",
		"world_copy_convert", "--cw-wcc-root", "../saves"};
	hw_run_config_init(&cfg);
	assert(!hw_run_parse_args(&cfg, 7, badroot, err, sizeof err));
	char* badvar[] = {"t", "--cw-tel-host", "pull", "--cw-profile",
		"world_copy_convert", "--cw-wcc-root", "r1", "--cw-wcc-variant", "x"};
	hw_run_config_init(&cfg);
	assert(!hw_run_parse_args(&cfg, 9, badvar, err, sizeof err));
	char* other[] = {"t", "--cw-tel-host", "pull", "--cw-profile",
		"menus_dwell", "--cw-wcc-root", "r1"};
	hw_run_config_init(&cfg);
	assert(!hw_run_parse_args(&cfg, 7, other, err, sizeof err));
	puts("PASS: world_copy_convert flags gated, validated, root required");
}

int main(void) {
	test_world_copy_convert_flags();
	test_periodic_sd_trace_exact_profile_set();
	for(size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
		run_vector(&vectors[i]);

	test_embedded_nul_rejected();
	test_missing_file_is_disabled_no_error();
	test_valid_no_trailing_newline_fields();
	test_valid_crlf_fields();
	test_doubled_records_last_wins_fields();
	test_far_view_distance_valid_fields();
	test_far_view_distance_clamped_fields();
	test_far_view_distance_minus_one_passthrough_fields();
	test_far_view_distance_absent_is_unchanged();
	test_far_storage_voxel_fields();
	test_far_storage_skin_fields();
	test_far_storage_absent_is_unchanged();
	test_settle_flag_sets_field_alone();
	test_pool_placement_rules();
	test_settle_absent_is_unchanged();
	test_carve_census_flag_sets_field_alone();
	test_cw_set_valid_fields();
	test_weather_force_integer_alias();
	test_cw_set_alone_does_not_arm_enabled();
	test_cw_set_malformed_ignored_with_counter();
	test_cw_set_over_cap_counted_not_fatal();
	test_settle_and_tps_cpu_are_independent();
	test_settle_carve_census_then_unknown_flag_rejects();
	test_tel_nonblock_flag_sets_field_alone();
	test_tel_nonblock_absent_is_unchanged();
	test_far_send_near_owed_after_profile_leaves_walk_armed();
	test_far_send_near_owed_alone_leaves_autopilot_off();
	test_seed_valid_fields();
	test_seed_negative_fields();
	test_seed_absent_is_unchanged();
	test_seed_does_not_disarm_profile_fields();
	test_keepup_walk_6m_profile_fields();
	test_split_action_floor_profile_fields();
	test_keepup_walk_alias_resolves();
	test_keepup_combined_staged_args();
	test_profile_then_syntax_error_leaves_pristine_default();
	test_mp_endpoint_valid_stored_verbatim();
	test_mp_endpoint_invalid_forms_rejected();
	test_mp_endpoint_other_profiles_rejected();
	test_mp_endpoint_absent_is_unchanged();
	test_mp_play_10m_profile_fields();
	test_mp_play_10m_arms_no_death_click();
	test_mp_play_10m_name_enum_round_trip();
	test_mp_play_10m_takes_the_endpoint_and_the_nonce();
	test_split_portal_quit_profile();
	test_split_dims_profile();
	test_profile_table_pinned();
	test_fuzz_never_crashes_and_stays_bounded();
	test_hw_overlay_switch_equivalence();
	test_staged_autoload_name_with_spaces();
	test_rz_fault_arm();

	printf("test_hw_run_args_parse: %d vectors + fuzz(%d) PASS\n",
		   (int)(sizeof(vectors) / sizeof(vectors[0])), FUZZ_ITERATIONS);
	return 0;
}
