/* Copyright (c) 2026 CaveWii_v2 contributors
 * G6 separation (astra-journal-0926): the session checkpoint JOURNAL is
 * deferred beyond G6 -- this tree neither writes nor recovers one. A save that
 * carries an experimental `checkpoint.journal` from a journal-enabled build
 * must therefore be PRESERVED and VISIBLY REFUSED when it is unresolved (an
 * INTENT whose epoch reached neither COMPLETE nor RECOVERED: that root's
 * region headers / level.dat may be a half-published epoch only the deferred
 * recovery can repair), never ignored. A resolved journal (its last epoch
 * ended in COMPLETE or RECOVERED) is inert and the world loads normally.
 *
 * Record framing mirrors the deferred module (magic "CWCK" v1; records
 * [type u32][len u32][payload][crc32 over type+len+payload]; types 1 INTENT,
 * 2 COMPLETE, 3 RECOVERED, 4 EXTEND). A torn record ends the trusted prefix:
 * a torn INTENT with nothing valid before it means nothing was published
 * (loads); an INTENT followed by a torn tail is unresolved (refused).
 *
 * Opus review G6SEP M1 (2026-09-26): a file SHORTER than the 8-byte header, or
 * one whose magic/version is not ours, is INERT (loads, noted on stderr) -- the
 * deferred module truncates the journal (`fopen "wb"`) before writing the
 * header of every new epoch, so a crash in that window leaves 0-7 bytes behind
 * with the previous epoch COMPLETE and nothing new published. Refusing it would
 * brick a healthy save. MALFORMED is reserved for a file that exists but cannot
 * be READ (io_error). */
#ifndef CHECKPOINT_JOURNAL_GUARD_H
#define CHECKPOINT_JOURNAL_GUARD_H

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stdint.h>

#define CHECKPOINT_JOURNAL_GUARD_API 1
#define CHECKPOINT_JOURNAL_GUARD_NAME "checkpoint.journal"

enum checkpoint_journal_state {
	CHECKPOINT_JOURNAL_NONE = 0,	  /* no file */
	CHECKPOINT_JOURNAL_RESOLVED,	  /* last epoch COMPLETE or RECOVERED */
	CHECKPOINT_JOURNAL_UNRESOLVED,	  /* INTENT without COMPLETE/RECOVERED */
	CHECKPOINT_JOURNAL_MALFORMED,	  /* exists but could not be read */
};

struct checkpoint_journal_verdict {
	enum checkpoint_journal_state state;
	uint32_t epoch;	  /* of the last valid INTENT, 0 if none */
	bool torn_tail;	  /* a CRC-invalid / truncated record ended the scan */
	bool io_error;	  /* the file could not be read */
	bool inert;		  /* shorter than the header, or foreign magic/version:
					   * RESOLVED without a record scan (M1) */
};

/* Classify `<root>/checkpoint.journal`. Read-only; never modifies the file. */
struct checkpoint_journal_verdict checkpoint_journal_inspect(string_t root);
/* True when a world under `root` may load: NONE or RESOLVED. UNRESOLVED and
 * MALFORMED refuse (the caller reports and preserves the files). */
bool checkpoint_journal_allows_load(string_t root,
								   struct checkpoint_journal_verdict* out);
/* The one-line, player-readable reason for a refusal (L1: shown on the load
 * screen's failure card, not only on stderr). Static storage. */
const char* checkpoint_journal_refusal_text(
	const struct checkpoint_journal_verdict* v);

#endif
