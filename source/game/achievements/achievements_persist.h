/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * Versioned per-profile progress file (`.cwstats`) -- write and read.
 *
 * Spec: docs/achievements_design.md section 4. Beta keeps one file per USER,
 * global across worlds (StatFileWriter); CaveWii keeps that property and keys
 * it by `profile_id` [design 3]. The file lives at
 * `<dir>/<profile_id>.cwstats` where `dir` is the caller's `<data root>/stats`
 * -- never inside a world folder, level.dat or any player NBT.
 *
 * Format v1 (text, LF, ASCII, one record per line):
 *
 *	cwstats 1
 *	profile player1
 *	saved 1789000000
 *	incomplete 1                (only when something is missing; see below)
 *	s 1100 12345 0
 *	a 5242881 1 0 L
 *	end 2 c0ffee12
 *
 * - `s <stat id> <local> <remote>` for a non-zero general/array counter,
 *   `a <stat id> <local> <remote> <L|R>` for a non-zero achievement, ALL of
 *   them in ascending stat-id order (so general 1000..2025, then the
 *   achievements at CW_ACH_BASE, then mine/craft/use/break). The design's
 *   illustrative snippet lists them out of order; ascending is what the writer
 *   does, because a deterministic order is what makes the golden test possible.
 * - `end <count> <crc32 hex>`: `count` is the number of `s` + `a` records
 *   above (the only records that carry counters), and the crc32 is zlib's
 *   crc32() over EVERY byte above the `end` line. A record letter this build
 *   does not know is skipped and does NOT count, so a later version may add
 *   lines without breaking a v1 reader.
 * - Only non-zero counters are written; Beta writes only touched stats too.
 * - The `profile` header must equal the key the file was opened for; a
 *   mismatch is treated as a corrupt file (identity check). So is a DUPLICATE
 *   `s`/`a` id: last-wins silently changes which value survives, and a file
 *   our writer produced never contains one [design 4, R1 LOW 12].
 * - `incomplete <0|1>` is an optional header line carrying
 *   stats_profile::progress_incomplete. It is written only when the flag is
 *   set, so a complete profile's file is byte-identical to what a build
 *   without this line would have produced.
 * - A record this build cannot hold -- an `s`/`a` id past its arrays, or a
 *   record letter it does not know -- is NOT dropped: it is kept verbatim in
 *   the profile's bounded `foreign` list, re-emitted after every known
 *   counter on the next save, and sets `progress_incomplete`. Overflowing
 *   that list write-refuses the profile (fail closed) rather than losing it
 *   [design 4, R1 HIGH 2]. Foreign `s`/`a` lines still count toward
 *   `end <count>`, exactly as the reader counted them.
 *
 * The crc32 replaces Beta's MD5-of-"local" checksum -- that guarded Mojang's
 * online sync, which does not exist here. Same intent (ignore a torn file),
 * documented deviation, no gameplay effect [design 4, 8].
 *
 * The caller owns the directory: neither call creates `<dir>`. A save into a
 * missing directory fails loudly with STATS_SAVE_ERR_TMP_OPEN rather than
 * silently succeeding somewhere else.
 */

#ifndef CW_ACHIEVEMENTS_PERSIST_H
#define CW_ACHIEVEMENTS_PERSIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "achievements_state.h"

/* The only version this build writes, and the highest it reads. */
#define STATS_FILE_VERSION 1

/* Longest `<dir>/<profile_id><ext><suffix>` this module will build. */
#define CW_STATS_PATH_MAX 256

enum stats_save_status {
	STATS_SAVE_OK = 0,
	/* The loaded file was newer than this build: writing would destroy
	 * progress this build cannot represent [design 4]. */
	STATS_SAVE_REFUSED_NEWER,
	STATS_SAVE_ERR_BAD_ARG,
	STATS_SAVE_ERR_TMP_OPEN,
	STATS_SAVE_ERR_WRITE,
	STATS_SAVE_ERR_FSYNC,
	STATS_SAVE_ERR_RENAME_BAK,
	STATS_SAVE_ERR_RENAME_MAIN,
	/* The loaded file carried more records this build cannot hold than the
	 * `foreign` list can keep, so writing would drop them. Fail closed
	 * [design 4, R1 HIGH 2]. */
	STATS_SAVE_REFUSED_FOREIGN,
};

enum stats_load_status {
	STATS_LOAD_OK = 0,
	/* No file yet: a fresh profile, not an error. */
	STATS_LOAD_MISSING,
	/* The main file was unusable; the `.bak` rotation loaded instead. */
	STATS_LOAD_RECOVERED_BAK,
	/* Neither the main file nor `.bak` was usable: empty state. */
	STATS_LOAD_CORRUPT,
	/* The file declares a version this build does not understand. It is left
	 * untouched, the state is empty, and the profile's write_refused flag is
	 * set so the next save cannot overwrite it. */
	STATS_LOAD_UNSUPPORTED_VERSION,
	STATS_LOAD_BAD_ARG,
};

/* Fault-injection stages, in the order the writer performs them [design 4
 * "Failure-stage tests"]. Runtime-armed, always compiled in -- never a new
 * #ifdef profile (docs/build_conventions.md, the runtime-armed-seam rule). */
enum stats_io_stage {
	STATS_IO_STAGE_NONE = -1,
	STATS_IO_STAGE_TMP_OPEN = 0,
	STATS_IO_STAGE_WRITE,
	STATS_IO_STAGE_FSYNC,
	STATS_IO_STAGE_RENAME_BAK,
	STATS_IO_STAGE_RENAME_MAIN,
	/* The durability barrier AFTER both renames (R1 MED 6). Reached only when
	 * the promote succeeded, which is what makes it a usable ordering probe. */
	STATS_IO_STAGE_RENAME_SYNC,
};

/* Arm the next save to fail at `stage`; STATS_IO_STAGE_NONE (-1) disarms.
 * One-shot: the stage is cleared as soon as it fires. */
void stats_io_fail_at(int stage);

/* Which stage is armed right now, or STATS_IO_STAGE_NONE. A test uses it to
 * assert a stage was NOT reached (still armed) or WAS (consumed); nothing in
 * the engine reads it. */
int stats_io_fail_armed(void);

/* Inject the clock used for the `saved` header (unix seconds, 0 when the
 * clock is unavailable) so the golden test writes byte-exact files. NULL
 * restores the default (time(NULL)). */
void stats_set_clock(uint32_t (*now)(void));

/* Write `<dir>/<profile_id>.cwstats` durably: serialize to `.tmp`, fsync,
 * rotate the committed file to `.bak`, rename `.tmp` over the committed name,
 * then reopen the promoted file and fsync it once more (on libfat that flushes
 * the partition cache including the directory entries the two renames touched;
 * on Windows/POSIX it is a harmless no-op and can never fail the save, whose
 * data is already committed by the rename) [design 4 "Write", R1 MED 6].
 *
 * `p->profile_id` must pass stats_profile_id_valid(); an invalid key is
 * STATS_SAVE_ERR_BAD_ARG and writes nothing, so save and load can never build
 * different paths from the same key (R1 MED 3).
 *
 * The rotation to `.bak` is SKIPPED when `p->main_was_corrupt` is set: a file
 * our own parser rejected is removed instead, so `.bak` only ever holds a file
 * this writer produced (R1 HIGH 1).
 *
 * On ANY failure the committed file is at worst the previous good one, never a
 * torn one, and `*p` is not modified at all -- the caller's dirty flag is
 * theirs to keep set. */
enum stats_save_status stats_profile_save(const char* dir,
										  const struct stats_profile* p);

/* Main-thread cooperative writer. The profile must remain immutable until
 * done/cancel; the consumer owns a MEM2 snapshot. A step scans at most 128
 * records, or performs one filesystem stage. Filesystem latency itself is
 * device-dependent; no CPU budget can bound a failing SD driver's syscall. */
#define CW_STATS_SAVE_STEP_RECORDS 128u
struct stats_save_job {
	const struct stats_profile* profile;
	FILE* file;
	char main_path[CW_STATS_PATH_MAX], tmp_path[CW_STATS_PATH_MAX];
	char bak_path[CW_STATS_PATH_MAX];
	uint32_t crc, records, cursor, saved;
	unsigned phase, scanned;
	enum stats_save_status result;
	bool done, ok, rescue;
};
void stats_save_begin(struct stats_save_job*, const char* dir,
	const struct stats_profile*);
bool stats_save_step(struct stats_save_job*);
void stats_rescue_begin(struct stats_save_job*, const char* dir, const struct stats_profile*);
void stats_save_cancel(struct stats_save_job*);
/* Best-effort independent survivor, never overwrites a newer/foreign main.
 * Readable with stats_profile_load from a recovery directory. */
enum stats_save_status stats_profile_rescue(const char* dir,
	const struct stats_profile*);

/* Load `<dir>/<profile_id>.cwstats` into `*p` (always reset first, so a
 * failed load yields a clean empty profile bound to `profile_id`). A short /
 * bad-crc / identity-mismatched / duplicate-id main file is moved aside to
 * `<profile_id>.cwstats.bad` -- the NEWEST quarantine wins, replacing an older
 * one, because a corrupt main file must never be left in place for a later
 * save to rotate over the only good `.bak` (R1 HIGH 1) -- and `<profile_id>.bak`
 * is tried instead. `p->main_was_corrupt` records that for the writer.
 * An invalid key is STATS_LOAD_BAD_ARG. `status` may be NULL. */
void stats_profile_load(const char* dir, const char* profile_id,
						struct stats_profile* p, enum stats_load_status* status);

/* Path helpers, exposed so tests and the caller name the same files.
 * `suffix` is one of "" (the committed file), ".bak", ".tmp", ".bad"; the
 * result is `<dir>/<profile_id>.cwstats<suffix>` except for ".bak"/".tmp",
 * which replace the extension: `<dir>/<profile_id>.bak`, `<...>.tmp`.
 * Returns false (and leaves `out` empty) if the result would not fit. */
bool stats_profile_path(char* out, size_t out_size, const char* dir,
						const char* profile_id, const char* suffix);

#endif /* CW_ACHIEVEMENTS_PERSIST_H */
