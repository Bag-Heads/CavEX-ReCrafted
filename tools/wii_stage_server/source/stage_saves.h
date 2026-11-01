#ifndef CAVEWII_STAGE_SAVES_H
#define CAVEWII_STAGE_SAVES_H

/* Saves LIST / HASH / MOVE policy for the Wii stage server (main.c) and its
 * PC unit test (tests/test_stage_saves_validator.c). Pure, header-only
 * (static inline); every filesystem touch is injected through
 * cw_saves_fs_ops and every wire byte through cw_saves_emit_ops, so the PC
 * test drives the SAME decision code main.c compiles, against an in-memory
 * fake card. This is the stage_send.h / stage_netinit.h precedent: the C
 * server is Wii-only (gccore/wiiuse/network headers) and cannot be linked
 * into the PC object pools, so anything that must be tested lives here.
 *
 * ------------------------------------------------------------------------
 * WHAT THIS IS FOR
 * ------------------------------------------------------------------------
 * The rig's card has accumulated ~646 world directories (every hw_run
 * profile sets autoload_new, source/game/hw_run.c:183), which the game's
 * Select World screen re-scans live with one stat() per row -- minutes to
 * settle and a standing watchdog risk. The card must stay in the Wii, so the
 * clean-up happens over this protocol.
 *
 * The rules, none of which are negotiable (design tmp/jobs/push-0921/
 * design-saves-archive.md v2, ratified by tmp/jobs/push-0921/
 * astra-ruling-4.raw):
 *
 *   - There is NO delete verb, NO overwrite, and NO copy/delete fallback.
 *     A world is displaced by exactly ONE rename() into
 *     sd:/cw_saves_archive/<batch>/, and put back by exactly one rename the
 *     other way. The maintainer empties the archive by hand, off-rig.
 *   - The archive root is the fixed constant sd:/cw_saves_archive/ -- top
 *     level, OUTSIDE saves (a folder inside saves would show up in Select
 *     World as an unreadable world row, design S2) and deliberately NOT
 *     under sd:/apps/ (HBC enumerates app subdirectories, so 600 folders
 *     there would pollute the HBC menu).
 *   - Roots are a closed ENUM, never a path: no client string ever reaches
 *     opendir(). Names cross the wire as HEX of the exact bytes readdir
 *     gave, because what libfat returns for a long name (UTF-8? CP437?
 *     locale-dependent?) is not determinable from this repo -- and because
 *     the header line is whitespace-tokenised while the live names contain
 *     spaces ("New World 152").
 *   - A name the validator refuses is REPORTED (supported:false) and left
 *     untouched -- never silently skipped, never renamed.
 *   - RESTORE goes back to the root NAMED IN THE REQUEST, which the PC tool
 *     takes from its write-ahead manifest -- never to a re-resolved default
 *     (Astra change 1: the saves root is resolved once and BOUND).
 *
 * ------------------------------------------------------------------------
 * STACK (measured, not estimated)
 * ------------------------------------------------------------------------
 * gcc -O2 -fstack-usage on this header (x86-64, so indicative rather than
 * Broadway-exact -- the dominant terms are the same char arrays):
 *
 *   cw_saves_list_run   5696   cw_saves_hash_run    4688
 *   cw_saves__walk_world 2192  cw_saves__hash_file  3376
 *   cw_saves_meta_run   3952   cw_saves_move_run    2784
 *   sizeof(struct cw_saves_list_req) = 944, move_req = 968
 *
 * Deepest path list_run -> hash_run -> walk_world -> hash_file = 15,952 B;
 * with handle_saves_list's request struct and stage_file's header/target
 * buffers, about 17.7 KB. The `meta` path (list_run -> meta_run) is 9,648 B
 * and adds no new worst case. An earlier note in this tree said ~7.7 KB,
 * which was a 2x UNDERESTIMATE (C9 re-review, N7).
 *
 * Margin: this all runs on the stage server's MAIN thread, whose stack is
 * whatever devkitPPC/libogc gives main() -- this repo does not set it
 * anywhere (tools/wii_stage_server/Makefile has no stack flag and main.c
 * creates no thread), so the number is libogc's default and is NOT recorded
 * here as fact. The game hit a 128 KB main-thread stack once
 * (PROJECT_MEMORY, "188KB main() frame > 128KB stack"); if that is also the
 * stage server's figure then 17.7 KB leaves ~7x headroom, but nobody has
 * measured the stage server's own limit and this comment does not pretend
 * otherwise.
 *
 * ------------------------------------------------------------------------
 * WHAT IS NOT PROVEN
 * ------------------------------------------------------------------------
 * libfat's rename() of a DIRECTORY across parents is UNPROVEN on this card:
 * every rename() in this repo today is file-to-file inside one directory,
 * and main.c:973-977 already records that libfat's rename "does not reliably
 * replace an existing target" (which is why dest_exists is a hard refusal
 * here instead of the PUT path's remove-then-rename). devkitPro's libfat is
 * now libdvm+fatfs-mod, whose f_rename does move a sub-directory and fix its
 * ".." entry -- that is upstream reading, not a measurement on this card.
 * Rename is also NOT proven power-loss-safe. The operator canary in
 * docs/wii_saves_archive.md is the proof, and a canary failure ENDS this
 * approach.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define CW_SAVES_SAVES_ABS "sd:/saves"
#define CW_SAVES_SAVES_APP_ABS "sd:/apps/CavEX/saves"
#define CW_SAVES_ARCHIVE_ABS "sd:/cw_saves_archive"
#define CW_SAVES_LEVEL_DAT "level.dat"

/* A world name may be at most 200 bytes: the header line is capped at 512 B
 * (main.c's HEADER_BUF) and the fixed tokens of the longest request
 * ("CWARCHIVE1 restore saves_app <32-byte batch> ") cost ~48 B, leaving 400
 * hex characters. A longer name is listed as supported:false and refused by
 * MOVE with unsupported_name -- never truncated. */
#define CW_SAVES_NAME_MAX_BYTES 200
/* What readdir may hand back: FAT long names go to 255 bytes, and one that
 * is too long to MOVE must still be LISTED (that is the whole point of
 * reporting unsupported names instead of hiding them). */
#define CW_SAVES_DIRENT_MAX 256
#define CW_SAVES_BATCH_MAX_BYTES 32
#define CW_SAVES_LIST_MAX_LIMIT 256
/* summary mode costs ~5-10 FAT ops per world (four known subdirs + a stat
 * per file), so its page is much smaller than a plain listing's. */
#define CW_SAVES_SUMMARY_MAX_LIMIT 32
#define CW_SAVES_HASH_MAX_FILES 4096
#define CW_SAVES_SHA256_HEX_LENGTH 64

/* The ONE metadata file a client may read, and only through CWSAVES1's
 * `meta` mode -- addressed by the world's name_hex, never by a path.
 *
 * This replaces the earlier plan of widening fetch_file()'s predicate to
 * <root>/<name>/level.dat, which could not work and should not have been
 * added (C9 review, H1/H2/M5): CWFETCH1's header is parsed with
 * sscanf("%15s %255s"), which truncates at the first space, so every live
 * "New World <n>" was refused with bad_target; the caller had to open a
 * SECOND connection against a server that accepts exactly one at a time and
 * reaps the idle first one only after 20 s, against a 5 s fetch timeout; and
 * the widened fetch sat behind the OLD auth gate, which is inert in
 * soft-launch, so it never inherited the CWAUTH1 strengthening the design
 * wrote for exactly this data. Serving it here instead fixes all three at
 * once and removes the new read surface from fetch_file() entirely. */
#define CW_SAVES_META_FILE "level.dat"
/* Matches the PC side's bounded decode budget (design S3: 64 KiB fetched,
 * 1 MiB gunzip cap, depth 32, 4096 tags). A real level.dat is a few hundred
 * bytes; anything past this is reported truncated, never streamed on. */
#define CW_SAVES_META_MAX_BYTES 65536u
#define CW_SAVES_META_CHUNK 256u

/* Longest path this header ever builds:
 *   "sd:/cw_saves_archive" (20) + "/" + batch (32) + "/" + name (200)
 *   + "/" + "DIM-1/region" (12) + "/" + a 255-byte file name = 523 + NUL. */
#define CW_SAVES_PATH_MAX 576

/* The ONLY subdirectories a world is walked through, for summary and hash.
 * Never an unbounded recursion: this is exactly the set the game writes
 * (level_archive.c, region_archive.c, map_data_archive.c, map_idcounts.c).
 * "" is the world root itself (level.dat, level.dat_old, level.dat_new). */
#define CW_SAVES_KNOWN_SUBDIR_COUNT 4
static const char* const CW_SAVES_KNOWN_SUBDIRS[CW_SAVES_KNOWN_SUBDIR_COUNT]
	= { "", "region", "DIM-1/region", "data" };

/* ------------------------------------------------------------------ */
/* Small ASCII helpers (deliberately NOT stage_auth.h's ci_* -- this    */
/* header must compile standalone in the PC test, and main.c includes   */
/* both, so the names must not collide).                                */
/* ------------------------------------------------------------------ */

static inline int cw_saves__lower(int c) {
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static inline int cw_saves__ci_streq(const char* a, const char* b) {
	if(!a || !b)
		return 0;
	for(; *a && *b; ++a, ++b) {
		if(cw_saves__lower((unsigned char)*a)
		   != cw_saves__lower((unsigned char)*b))
			return 0;
	}
	return *a == *b;
}

static inline int cw_saves__ci_has_prefix(const char* s, const char* prefix) {
	for(; *prefix; ++s, ++prefix) {
		if(!*s
		   || cw_saves__lower((unsigned char)*s)
				  != cw_saves__lower((unsigned char)*prefix))
			return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* 1. Name validation                                                  */
/* ------------------------------------------------------------------ */

enum cw_saves_name_verdict {
	CW_SAVES_NAME_OK = 0,
	CW_SAVES_NAME_EMPTY,
	CW_SAVES_NAME_TOO_LONG,
	CW_SAVES_NAME_SEPARATOR,
	CW_SAVES_NAME_DOTDOT,
	CW_SAVES_NAME_DOT,
	CW_SAVES_NAME_LEADING_DOT,
	CW_SAVES_NAME_TRAILING_DOT_OR_SPACE,
	CW_SAVES_NAME_CONTROL,
	CW_SAVES_NAME_FAT_ILLEGAL
};

/* A world directory name, as raw BYTES. Deliberately conservative: a name
 * this refuses is not lost, it is reported supported:false and left exactly
 * where it is, so over-rejecting costs an operator one manual step while
 * under-rejecting could move the wrong thing.
 *
 * Order of the checks is load-bearing for the reported reason -- separators
 * are looked for before the "a..b" rule so that "../etc" and "world/.."
 * report SEPARATOR (the traversal shape) rather than DOTDOT.
 *
 * High bytes (>= 0x80) are ACCEPTED and never interpreted: the name is
 * opaque, travels as hex, and is compared byte-for-byte. */
static inline enum cw_saves_name_verdict
cw_saves_name_check(const unsigned char* b, size_t len) {
	size_t i;
	if(!b || len == 0)
		return CW_SAVES_NAME_EMPTY;
	if(len > CW_SAVES_NAME_MAX_BYTES)
		return CW_SAVES_NAME_TOO_LONG;
	for(i = 0; i < len; ++i) {
		if(b[i] == '/' || b[i] == '\\')
			return CW_SAVES_NAME_SEPARATOR;
	}
	if(len == 1 && b[0] == '.')
		return CW_SAVES_NAME_DOT;
	/* The single most important name to refuse reports the DOTDOT reason,
	 * not the generic DOT one -- the check order below is documented as
	 * load-bearing for the reported reason, so it has to be right here too
	 * (C9 review, L1). */
	if(len == 2 && b[0] == '.' && b[1] == '.')
		return CW_SAVES_NAME_DOTDOT;
	for(i = 0; i + 1 < len; ++i) {
		if(b[i] == '.' && b[i + 1] == '.')
			return CW_SAVES_NAME_DOTDOT;
	}
	if(b[0] == '.')
		return CW_SAVES_NAME_LEADING_DOT; /* hidden; the game skips these */
	/* FAT normalisation silently drops a trailing '.' or ' ', so a name that
	 * ends in one cannot be reproduced on the destination -- refuse rather
	 * than create a directory under a name the operator did not ask for. */
	if(b[len - 1] == '.' || b[len - 1] == ' ')
		return CW_SAVES_NAME_TRAILING_DOT_OR_SPACE;
	for(i = 0; i < len; ++i) {
		unsigned char c = b[i];
		if(c < 0x20 || c == 0x7f)
			return CW_SAVES_NAME_CONTROL;
		if(c == ':' || c == '*' || c == '?' || c == '"' || c == '<'
		   || c == '>' || c == '|')
			return CW_SAVES_NAME_FAT_ILLEGAL;
	}
	return CW_SAVES_NAME_OK;
}

static inline int cw_saves_name_ok(const unsigned char* b, size_t len) {
	return cw_saves_name_check(b, len) == CW_SAVES_NAME_OK;
}

/* FAT is case-insensitive, so two names differing only by ASCII case are the
 * SAME directory entry and cannot coexist. Folding is ASCII-only on purpose:
 * what libfat does to a high byte depends on a codepage this repo cannot
 * determine, so high bytes compare exactly rather than through a guess. */
static inline int cw_saves_name_ci_equal(const unsigned char* a, size_t alen,
										 const unsigned char* b, size_t blen) {
	size_t i;
	if(alen != blen)
		return 0;
	for(i = 0; i < alen; ++i) {
		if(cw_saves__lower(a[i]) != cw_saves__lower(b[i]))
			return 0;
	}
	return 1;
}

/* Batch ids are FAT directory names too, so they carry the same
 * normalisation rules -- plus a much tighter charset, because unlike a world
 * name a batch id is something THIS tooling generates (yyyymmdd-hhmmss). */
static inline int cw_saves_batch_id_valid(const char* s) {
	size_t len, i;
	if(!s)
		return 0;
	len = strlen(s);
	if(len == 0 || len > CW_SAVES_BATCH_MAX_BYTES)
		return 0;
	for(i = 0; i < len; ++i) {
		char c = s[i];
		int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
				 || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
		if(!ok)
			return 0;
	}
	return cw_saves_name_check((const unsigned char*)s, len)
		   == CW_SAVES_NAME_OK;
}

/* ------------------------------------------------------------------ */
/* 2. Root enum                                                        */
/* ------------------------------------------------------------------ */

enum cw_saves_root_kind {
	CW_SAVES_ROOT_BAD = 0,
	CW_SAVES_ROOT_SAVES,
	CW_SAVES_ROOT_SAVES_APP,
	CW_SAVES_ROOT_ARCHIVE,
	CW_SAVES_ROOT_ARCHIVE_BATCH
};

struct cw_saves_root {
	enum cw_saves_root_kind kind;
	char token[CW_SAVES_BATCH_MAX_BYTES + 16]; /* echoed back verbatim */
	char batch[CW_SAVES_BATCH_MAX_BYTES + 1];  /* ARCHIVE_BATCH only */
	char abs[CW_SAVES_PATH_MAX];
};

/* The only four forms: "saves", "saves_app", "archive", "archive/<batch>".
 * Matched EXACTLY (not case-folded) -- these are protocol tokens, not
 * filenames. Everything else is bad_root, and the absolute path is always
 * built here from constants, never taken from the client. */
static inline int cw_saves_root_parse(const char* token,
									  struct cw_saves_root* out) {
	if(!token || !out)
		return 0;
	memset(out, 0, sizeof(*out));
	if(strcmp(token, "saves") == 0) {
		out->kind = CW_SAVES_ROOT_SAVES;
		snprintf(out->abs, sizeof(out->abs), "%s", CW_SAVES_SAVES_ABS);
	} else if(strcmp(token, "saves_app") == 0) {
		out->kind = CW_SAVES_ROOT_SAVES_APP;
		snprintf(out->abs, sizeof(out->abs), "%s", CW_SAVES_SAVES_APP_ABS);
	} else if(strcmp(token, "archive") == 0) {
		out->kind = CW_SAVES_ROOT_ARCHIVE;
		snprintf(out->abs, sizeof(out->abs), "%s", CW_SAVES_ARCHIVE_ABS);
	} else if(strncmp(token, "archive/", 8) == 0) {
		const char* batch = token + 8;
		if(!cw_saves_batch_id_valid(batch))
			return 0;
		out->kind = CW_SAVES_ROOT_ARCHIVE_BATCH;
		snprintf(out->batch, sizeof(out->batch), "%s", batch);
		snprintf(out->abs, sizeof(out->abs), "%s/%s", CW_SAVES_ARCHIVE_ABS,
				 batch);
	} else {
		return 0;
	}
	snprintf(out->token, sizeof(out->token), "%s", token);
	return 1;
}

/* A world root is one of the two SAVES roots: the archive side of a move is
 * always rebuilt from the batch id, never sent as a root. */
static inline int cw_saves_root_is_world(const struct cw_saves_root* r) {
	return r
		   && (r->kind == CW_SAVES_ROOT_SAVES
			   || r->kind == CW_SAVES_ROOT_SAVES_APP);
}

/* ------------------------------------------------------------------ */
/* 3. Hex name transport + path building                               */
/* ------------------------------------------------------------------ */

static inline int cw_saves__hex_nibble(int c) {
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static inline int cw_saves_hex_encode(const unsigned char* b, size_t len,
									  char* out, size_t cap) {
	static const char digits[] = "0123456789abcdef";
	size_t i;
	if(!b || !out || cap < len * 2 + 1)
		return 0;
	for(i = 0; i < len; ++i) {
		out[i * 2] = digits[(b[i] >> 4) & 0xf];
		out[i * 2 + 1] = digits[b[i] & 0xf];
	}
	out[len * 2] = '\0';
	return 1;
}

static inline int cw_saves_hex_decode(const char* hex, unsigned char* out,
									  size_t cap, size_t* out_len) {
	size_t len, i;
	if(!hex || !out || !out_len)
		return 0;
	len = strlen(hex);
	if(len == 0 || (len & 1u) != 0)
		return 0;
	if(len / 2 > cap)
		return 0;
	for(i = 0; i < len; i += 2) {
		int hi = cw_saves__hex_nibble((unsigned char)hex[i]);
		int lo = cw_saves__hex_nibble((unsigned char)hex[i + 1]);
		if(hi < 0 || lo < 0)
			return 0;
		out[i / 2] = (unsigned char)((hi << 4) | lo);
	}
	*out_len = len / 2;
	return 1;
}

/* dir_abs + '/' + name-bytes. Refuses rather than truncates: a truncated
 * path would name a DIFFERENT directory, which is the one mistake this whole
 * design exists to make impossible. */
static inline int cw_saves_path_join(const char* dir_abs,
									 const unsigned char* name, size_t name_len,
									 char* out, size_t cap) {
	size_t dlen;
	if(!dir_abs || !name || !out)
		return 0;
	dlen = strlen(dir_abs);
	if(dlen + 1 + name_len + 1 > cap)
		return 0;
	memcpy(out, dir_abs, dlen);
	out[dlen] = '/';
	memcpy(out + dlen + 1, name, name_len);
	out[dlen + 1 + name_len] = '\0';
	return 1;
}

/* ------------------------------------------------------------------ */
/* 5. Auth                                                             */
/* ------------------------------------------------------------------ */

/* The LIST-family modes. Declared here, ahead of the auth section, so
 * cw_saves_verb_for_mode() below can state which rule each one rides. */
enum cw_saves_list_mode {
	CW_SAVES_LIST_PLAIN = 0,
	CW_SAVES_LIST_SUMMARY = 1,
	CW_SAVES_LIST_HASH = 2,
	CW_SAVES_LIST_META = 3
};


enum cw_saves_verb {
	CW_SAVES_VERB_LIST = 0,
	CW_SAVES_VERB_HASH = 1,
	CW_SAVES_VERB_MOVE = 2,
	/* `meta` reads a world's level.dat -- its name, seed and player
	 * position. It rides the SAME rule as LIST/HASH, and having its own
	 * enumerator is what lets a test state that, instead of it being an
	 * argument main.c happens to pass (C9 re-review, N8). */
	CW_SAVES_VERB_META = 3
};

enum cw_saves_auth_verdict {
	CW_SAVES_AUTH_OK = 0,
	CW_SAVES_AUTH_REQUIRED = 1,
	CW_SAVES_AUTH_TOKEN_REQUIRED = 2,
	/* C32 (inventory stage_saves.h:467-476): a verb value this build does
	 * not know is not a read verb. The chain below used to fall through to
	 * its final `return CW_SAVES_AUTH_OK` for every enumerator it had no
	 * test for, so ANY future or garbled verb id inherited the weakest rule
	 * on the table. */
	CW_SAVES_AUTH_UNKNOWN_VERB = 3
};

/* LIST/HASH ride the connection's ordinary auth state, but -- unlike every
 * other read verb -- they require an explicit CWAUTH1 even in SOFT-LAUNCH
 * (no token on the card). That is a deliberate strengthening: a world's
 * level.dat carries its name, seed and player position, and every PC driver
 * already sends CWAUTH1 immediately after CWHELLO1, so it costs a legitimate
 * client nothing.
 *
 * MOVE/RESTORE go further and require a REAL token (Astra Q3): in
 * soft-launch, handle_auth() marks EVERY connection authed with no secret
 * checked (main.c:1208-1213), so "authed" alone is the state every subnet
 * peer is already in. Displacing a world must never be reachable from there,
 * even though it is reversible. */
static inline enum cw_saves_auth_verdict
cw_saves_auth_check(enum cw_saves_verb verb, int conn_authed,
					int token_provisioned) {
	/* Written as an explicit list rather than "anything that is not MOVE",
	 * so a new verb has to declare which side of the token line it is on --
	 * and a verb that is on NEITHER side is refused before the connection
	 * auth state is even consulted (C32). */
	switch(verb) {
	case CW_SAVES_VERB_LIST:
	case CW_SAVES_VERB_HASH:
	case CW_SAVES_VERB_MOVE:
	case CW_SAVES_VERB_META:
		break;
	default:
		return CW_SAVES_AUTH_UNKNOWN_VERB;
	}
	if(!conn_authed)
		return CW_SAVES_AUTH_REQUIRED;
	if(verb == CW_SAVES_VERB_MOVE && !token_provisioned)
		return CW_SAVES_AUTH_TOKEN_REQUIRED;
	return CW_SAVES_AUTH_OK;
}

/* Which verb's auth rule a LIST-family mode rides. main.c used to pass
 * CW_SAVES_VERB_LIST for every mode including `meta`, which was correct but
 * was a choice made in untested code; now it is a decision with a test
 * (C9 re-review, N8). */
static inline enum cw_saves_verb
cw_saves_verb_for_mode(enum cw_saves_list_mode mode) {
	if(mode == CW_SAVES_LIST_HASH)
		return CW_SAVES_VERB_HASH;
	if(mode == CW_SAVES_LIST_META)
		return CW_SAVES_VERB_META;
	return CW_SAVES_VERB_LIST;
}

/* Both move directions carry the real-token requirement. The auth check runs
 * BEFORE the header is parsed, so it cannot know the direction -- this
 * function exists so that "skip auth for RESTORE" is impossible to write as
 * a one-line edit in an untested file, and is caught here instead. */
static inline enum cw_saves_verb cw_saves_move_auth_verb(int restore) {
	(void)restore;
	return CW_SAVES_VERB_MOVE;
}

/* The two protocol tags this header owns. main.c dispatches on exactly
 * these, after the connection's auth gate. */
static inline int cw_saves_dispatch_is_saves_verb(const char* proto) {
	if(!proto)
		return 0;
	return strcmp(proto, "CWSAVES1") == 0 || strcmp(proto, "CWARCHIVE1") == 0;
}

static inline const char*
cw_saves_auth_reason(enum cw_saves_auth_verdict v) {
	if(v == CW_SAVES_AUTH_REQUIRED)
		return "auth_required";
	if(v == CW_SAVES_AUTH_TOKEN_REQUIRED)
		return "token_required";
	if(v == CW_SAVES_AUTH_UNKNOWN_VERB)
		return "unknown_verb";
	return "";
}

/* ------------------------------------------------------------------ */
/* 6. Injected I/O                                                     */
/* ------------------------------------------------------------------ */

struct cw_saves_stat {
	int exists;
	int is_dir;
	unsigned long bytes;
	long mtime; /* FAT's 2-second-granularity, timezone-free LOCAL console
				 * time. Label it; never diff it against PC UTC. */
};

/* Note there is no is_dir out-param on readdir: libfat's dirent d_type is
 * not dependable here (the game's own Select World scan was fixed to
 * stat() + S_ISDIR for exactly this reason, CLAUDE.md's
 * screen_select_world.c note), so the driver stats the entries whose type it
 * actually needs -- the page window, never the whole 646-entry directory. */
struct cw_saves_fs_ops {
	/* NULL = could not open (missing or not a directory). */
	void* (*opendir)(void* user, const char* dir_abs);
	/* 1 = one entry written, 0 = end of directory, < 0 = read error. */
	int (*readdir)(void* user, void* dir, unsigned char* name_out,
				   size_t name_cap, size_t* name_len);
	/* 0 = closed successfully, < 0 = close error; last_error supplies errno. */
	int (*closedir)(void* user, void* dir);
	/* 1 = the stat ran (out->exists says whether the path is there),
	 * 0 = the stat itself failed. */
	int (*stat)(void* user, const char* abs, struct cw_saves_stat* out);
	/* 1 = the directory exists afterwards (EEXIST counts), 0 = failed. */
	int (*mkdir)(void* user, const char* abs);
	int (*rename)(void* user, const char* from, const char* to);
	/* hex_out is CW_SAVES_SHA256_HEX_LENGTH + 1 bytes. */
	int (*hash_file)(void* user, const char* abs, char* hex_out,
					 unsigned long* bytes_out);
	/* Sequential read of one regular file, for the `meta` mode. Separate
	 * from hash_file on purpose: the bytes are streamed out as they are
	 * read, in ONE pass, so the declared length (emitted in the trailer)
	 * covers exactly the bytes that preceded it -- the same single-pass
	 * discipline fetch_file() was fixed to on 2026-09-02. */
	void* (*openfile)(void* user, const char* abs); /* NULL = failed */
	/* > 0 bytes, 0 = EOF, < 0 = read error. */
	int (*readfile)(void* user, void* fh, unsigned char* buf, size_t cap);
	void (*closefile)(void* user, void* fh);
	/* errno from the immediately preceding failed stat/readdir op. */
	int (*last_error)(void* user);
};

struct cw_saves_emit_ops {
	/* 1 = queued, 0 = the connection is unusable. A 0 is ALWAYS fatal to the
	 * reply: an unknown prefix may already be on the wire and the stream
	 * cannot be resynchronised (stage_send.h's contract). */
	int (*emit)(void* user, const char* text, size_t len);
};

enum cw_saves_rc {
	CW_SAVES_RC_OK = 0,
	/* Nothing was emitted; the caller sends one FAIL line with *reason. */
	CW_SAVES_RC_FAIL = 1,
	/* A partial reply is on the wire; the caller closes the connection and
	 * sends nothing more on it. */
	CW_SAVES_RC_ABORT = 2
};

/* ------------------------------------------------------------------ */
/* 7. Request parsing                                                  */
/* ------------------------------------------------------------------ */

struct cw_saves_list_req {
	struct cw_saves_root root;
	long offset;
	long limit;
	enum cw_saves_list_mode mode;
	unsigned char name[CW_SAVES_DIRENT_MAX]; /* hash mode only */
	size_t name_len;
};

struct cw_saves_move_req {
	int restore; /* 0 = archive, 1 = restore */
	struct cw_saves_root root;
	char batch[CW_SAVES_BATCH_MAX_BYTES + 1];
	unsigned char name[CW_SAVES_DIRENT_MAX];
	size_t name_len;
};

/* Hand-rolled because sscanf's "%255s" is the exact shape that cannot carry
 * a name with a space (main.c:1292,1315) and silently truncates an overlong
 * token. Returns the position after the token, or NULL when there is none;
 * *overflow is set when the token did not fit, so the caller can report
 * unsupported_name rather than acting on a truncated value. */
static inline const char* cw_saves__tok(const char* s, char* out, size_t cap,
										int* overflow) {
	size_t n = 0;
	*overflow = 0;
	if(!s)
		return NULL;
	while(*s == ' ' || *s == '\t')
		++s;
	if(!*s)
		return NULL;
	while(*s && *s != ' ' && *s != '\t') {
		if(n + 1 < cap)
			out[n] = *s;
		++n;
		++s;
	}
	if(n + 1 > cap) {
		*overflow = 1;
		out[cap - 1] = '\0';
		return s;
	}
	out[n] = '\0';
	return s;
}

/* Strict non-negative decimal. "x", "" and "12x" are all bad_range rather
 * than a silent atoi() 0. */
static inline int cw_saves__parse_long(const char* s, long* out) {
	long v = 0;
	size_t i;
	if(!s || !s[0])
		return 0;
	for(i = 0; s[i]; ++i) {
		if(s[i] < '0' || s[i] > '9')
			return 0;
		if(v > (2000000000L - 9) / 10)
			return 0;
		v = v * 10 + (s[i] - '0');
	}
	*out = v;
	return 1;
}

/* C32 (inventory stage_saves.h:690-735,794-811): a header whose fields all
 * parse but which carries MORE tokens after them was accepted on its prefix.
 * The extra bytes were never seen by anything, so two different lines --
 * "CWARCHIVE1 archive saves b <hex>" and that same line plus anything at all
 * -- were the same request. A request the server cannot account for in full
 * is not a request it may act on. */
static inline int cw_saves__no_trailing(const char* p, const char** reason) {
	char tok[8];
	int overflow = 0;
	if(cw_saves__tok(p, tok, sizeof(tok), &overflow)) {
		*reason = "trailing_token";
		return 0;
	}
	return 1;
}

/* CWSAVES1 <root> <offset> <limit> [plain|summary|hash <name_hex>] */
static inline int cw_saves_parse_list_header(const char* header,
											 struct cw_saves_list_req* out,
											 const char** reason) {
	char tok[CW_SAVES_DIRENT_MAX * 2 + 2];
	const char* p = header;
	int overflow = 0;
	if(!out || !reason)
		return 0;
	memset(out, 0, sizeof(*out));
	*reason = "bad_header";
	p = cw_saves__tok(p, tok, sizeof(tok), &overflow); /* verb, ignored */
	if(!p)
		return 0;
	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p)
		return 0;
	if(overflow || !cw_saves_root_parse(tok, &out->root)) {
		*reason = "bad_root";
		return 0;
	}
	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p)
		return 0;
	if(overflow || !cw_saves__parse_long(tok, &out->offset)) {
		*reason = "bad_range";
		return 0;
	}
	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p)
		return 0;
	if(overflow || !cw_saves__parse_long(tok, &out->limit)) {
		*reason = "bad_range";
		return 0;
	}
	if(out->limit < 1 || out->limit > CW_SAVES_LIST_MAX_LIMIT) {
		*reason = "bad_range";
		return 0;
	}

	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p) {
		out->mode = CW_SAVES_LIST_PLAIN;
		*reason = "";
		return 1;
	}
	if(!overflow && strcmp(tok, "plain") == 0) {
		out->mode = CW_SAVES_LIST_PLAIN;
	} else if(!overflow && strcmp(tok, "summary") == 0) {
		out->mode = CW_SAVES_LIST_SUMMARY;
		if(out->limit > CW_SAVES_SUMMARY_MAX_LIMIT) {
			*reason = "bad_range";
			return 0;
		}
	} else if(!overflow && strcmp(tok, "hash") == 0) {
		out->mode = CW_SAVES_LIST_HASH;
	} else if(!overflow && strcmp(tok, "meta") == 0) {
		out->mode = CW_SAVES_LIST_META;
	} else {
		*reason = "bad_mode";
		return 0;
	}
	if(out->mode != CW_SAVES_LIST_HASH && out->mode != CW_SAVES_LIST_META) {
		if(!cw_saves__no_trailing(p, reason))
			return 0;
		*reason = "";
		return 1;
	}

	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p) {
		*reason = "bad_name";
		return 0;
	}
	if(overflow) {
		*reason = "unsupported_name";
		return 0;
	}
	if(!cw_saves_hex_decode(tok, out->name, sizeof(out->name),
							&out->name_len)) {
		*reason = "bad_name";
		return 0;
	}
	if(!cw_saves_name_ok(out->name, out->name_len)) {
		*reason = "unsupported_name";
		return 0;
	}
	if(!cw_saves__no_trailing(p, reason))
		return 0;
	*reason = "";
	return 1;
}

/* CWARCHIVE1 <archive|restore> <root> <batch-id> <name_hex> */
static inline int cw_saves_parse_move_header(const char* header,
											 struct cw_saves_move_req* out,
											 const char** reason) {
	char tok[CW_SAVES_DIRENT_MAX * 2 + 2];
	const char* p = header;
	int overflow = 0;
	if(!out || !reason)
		return 0;
	memset(out, 0, sizeof(*out));
	*reason = "bad_header";
	p = cw_saves__tok(p, tok, sizeof(tok), &overflow); /* verb, ignored */
	if(!p)
		return 0;

	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p)
		return 0;
	if(overflow || (strcmp(tok, "archive") != 0 && strcmp(tok, "restore") != 0)) {
		*reason = "bad_direction";
		return 0;
	}
	out->restore = strcmp(tok, "restore") == 0;

	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p)
		return 0;
	if(overflow || !cw_saves_root_parse(tok, &out->root)
	   || !cw_saves_root_is_world(&out->root)) {
		*reason = "bad_root";
		return 0;
	}

	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p)
		return 0;
	if(overflow || !cw_saves_batch_id_valid(tok)) {
		*reason = "bad_batch";
		return 0;
	}
	/* cw_saves_batch_id_valid() has already bounded this at
	 * CW_SAVES_BATCH_MAX_BYTES, but copy it with the length spelled out
	 * rather than through snprintf: a truncation here would silently name a
	 * DIFFERENT archive directory, and -Wformat-truncation is right to
	 * refuse to take the validator's word for it from the token buffer. */
	{
		size_t blen = strlen(tok);
		if(blen > CW_SAVES_BATCH_MAX_BYTES) {
			*reason = "bad_batch";
			return 0;
		}
		memcpy(out->batch, tok, blen);
		out->batch[blen] = '\0';
	}

	p = cw_saves__tok(p, tok, sizeof(tok), &overflow);
	if(!p)
		return 0;
	if(overflow) {
		*reason = "unsupported_name";
		return 0;
	}
	if(!cw_saves_hex_decode(tok, out->name, sizeof(out->name),
							&out->name_len)) {
		*reason = "bad_name";
		return 0;
	}
	if(!cw_saves_name_ok(out->name, out->name_len)) {
		*reason = "unsupported_name";
		return 0;
	}
	if(!cw_saves__no_trailing(p, reason))
		return 0;
	*reason = "";
	return 1;
}

/* ------------------------------------------------------------------ */
/* 8. Reply helpers                                                    */
/* ------------------------------------------------------------------ */

#define CW_SAVES_FNV1A_INIT 0x811c9dc5u

static inline uint32_t cw_saves_fnv1a(uint32_t h, const char* s) {
	for(; s && *s; ++s) {
		h ^= (uint32_t)(unsigned char)*s;
		h *= 16777619u;
	}
	return h;
}

/* A name may be printed only when every byte is printable ASCII and needs no
 * JSON escaping. FAT cannot even hold a '"' or '\\' in a name, so this never
 * costs a real world its readable label -- and it means no encoding or
 * escaping question can arise anywhere in the protocol. name_hex is always
 * present and is what every other message keys on. */
static inline int cw_saves_name_json_safe(const unsigned char* b, size_t len) {
	size_t i;
	if(!b || len == 0)
		return 0;
	for(i = 0; i < len; ++i) {
		if(b[i] < 0x20 || b[i] > 0x7e || b[i] == '"' || b[i] == '\\')
			return 0;
	}
	return 1;
}

static inline int cw_saves__emit(const struct cw_saves_emit_ops* out,
								 void* user, const char* text) {
	return out && out->emit && out->emit(user, text, strlen(text));
}

/* ------------------------------------------------------------------ */
/* 9. LIST / HASH                                                      */
/* ------------------------------------------------------------------ */

static inline int cw_saves__stat_dir(const struct cw_saves_fs_ops* fs,
									 void* user, const char* abs) {
	struct cw_saves_stat st;
	if(!fs->stat(user, abs, &st))
		return 0;
	return st.exists && st.is_dir;
}

/* Walks the four KNOWN subdirs of one world. `on_file` is called for every
 * regular file found, with its rel_path ("level.dat",
 * "region/r.0.0.mcr", ...). Returns the number of files visited, or -1 on a
 * hard error / -2 when the cap was exceeded. */
static inline int
cw_saves__walk_world(const struct cw_saves_fs_ops* fs, void* user,
					 const char* world_abs,
					 int (*on_file)(void* ctx, const char* rel_path,
									const char* abs,
									const struct cw_saves_stat* st),
					 void* ctx) {
	int total = 0;
	int s;
	for(s = 0; s < CW_SAVES_KNOWN_SUBDIR_COUNT; ++s) {
		char dir_abs[CW_SAVES_PATH_MAX];
		void* dir;
		const char* sub = CW_SAVES_KNOWN_SUBDIRS[s];
		if(sub[0] == '\0')
			snprintf(dir_abs, sizeof(dir_abs), "%s", world_abs);
		else
			snprintf(dir_abs, sizeof(dir_abs), "%s/%s", world_abs, sub);
		/* C32 (inventory stage_saves.h:882-917): opendir() returns NULL for
		 * ANY failure, so "a world without a nether directory is normal"
		 * and "libfat ran out of directory handles" were the same answer --
		 * and the SUMMARY caller turned that into a byte total the archive
		 * tiny-world check then consumed as fact. Absence is now
		 * established by a stat; everything else is a hard error. */
		{
			struct cw_saves_stat dst;
			if(!fs->stat(user, dir_abs, &dst))
				return -1; /* could not even look */
			if(!dst.exists || !dst.is_dir)
				continue;  /* genuinely not there: a normal world shape */
		}
		dir = fs->opendir(user, dir_abs);
		if(!dir)
			return -1; /* it IS a directory and we could not read it */
		for(;;) {
			unsigned char name[CW_SAVES_DIRENT_MAX];
			size_t name_len = 0;
			char abs[CW_SAVES_PATH_MAX];
			char rel[CW_SAVES_PATH_MAX];
			struct cw_saves_stat st;
			int r = fs->readdir(user, dir, name, sizeof(name), &name_len);
			if(r < 0) {
				fs->closedir(user, dir);
				return -1; /* a read error is not the end of the directory */
			}
			if(r == 0)
				break;
			if(name_len == 0 || name[0] == '.')
				continue;
			if(total >= CW_SAVES_HASH_MAX_FILES) {
				fs->closedir(user, dir);
				return -2;
			}
			if(!cw_saves_path_join(dir_abs, name, name_len, abs, sizeof(abs))) {
				fs->closedir(user, dir);
				return -1; /* a file we cannot name is a file we cannot skip */
			}
			if(!fs->stat(user, abs, &st)) {
				fs->closedir(user, dir);
				return -1;
			}
			if(!st.exists || st.is_dir)
				continue;
			if(sub[0] == '\0')
				snprintf(rel, sizeof(rel), "%.*s", (int)name_len,
						 (const char*)name);
			else
				snprintf(rel, sizeof(rel), "%s/%.*s", sub, (int)name_len,
						 (const char*)name);
			++total;
			if(on_file && !on_file(ctx, rel, abs, &st)) {
				fs->closedir(user, dir);
				return -1;
			}
		}
		if(fs->closedir(user, dir) < 0)
			return -1;
	}
	return total;
}

struct cw_saves__summary {
	int files;
	unsigned long total_bytes;
	long newest;
	long oldest;
};

static inline int cw_saves__summary_file(void* ctx, const char* rel,
										 const char* abs,
										 const struct cw_saves_stat* st) {
	struct cw_saves__summary* s = (struct cw_saves__summary*)ctx;
	(void)rel;
	(void)abs;
	if(s->files == 0) {
		s->newest = st->mtime;
		s->oldest = st->mtime;
	} else {
		if(st->mtime > s->newest)
			s->newest = st->mtime;
		if(st->mtime < s->oldest)
			s->oldest = st->mtime;
	}
	++s->files;
	s->total_bytes += st->bytes;
	return 1;
}

struct cw_saves__hash_ctx {
	const struct cw_saves_fs_ops* fs;
	void* fs_user;
	const struct cw_saves_emit_ops* out;
	void* out_user;
	int emitted;
	int failed;
};

static inline int cw_saves__hash_file(void* ctx, const char* rel,
									  const char* abs,
									  const struct cw_saves_stat* st) {
	struct cw_saves__hash_ctx* h = (struct cw_saves__hash_ctx*)ctx;
	char sha[CW_SAVES_SHA256_HEX_LENGTH + 1];
	char hex[CW_SAVES_PATH_MAX * 2 + 1];
	char line[CW_SAVES_PATH_MAX * 3 + 256];
	unsigned long bytes = 0;
	if(!h->fs->hash_file(h->fs_user, abs, sha, &bytes)) {
		h->failed = 1;
		return 0;
	}
	if(!cw_saves_hex_encode((const unsigned char*)rel, strlen(rel), hex,
							sizeof(hex))) {
		h->failed = 1;
		return 0;
	}
	if(cw_saves_name_json_safe((const unsigned char*)rel, strlen(rel))) {
		snprintf(line, sizeof(line),
				 "%s{\"rel_path_hex\":\"%s\",\"rel_path\":\"%s\","
				 "\"bytes\":%lu,\"mtime\":%ld,\"sha256\":\"%s\"}",
				 h->emitted ? "," : "", hex, rel, bytes, st->mtime, sha);
	} else {
		snprintf(line, sizeof(line),
				 "%s{\"rel_path_hex\":\"%s\",\"bytes\":%lu,\"mtime\":%ld,"
				 "\"sha256\":\"%s\"}",
				 h->emitted ? "," : "", hex, bytes, st->mtime, sha);
	}
	if(!cw_saves__emit(h->out, h->out_user, line)) {
		h->failed = 1;
		return 0;
	}
	++h->emitted;
	return 1;
}

/* CWSAVES1 ... hash <name_hex> -- {rel_path, bytes, sha256} for every
 * regular file of ONE world over the known subdirs. Chosen over widening the
 * fetch allow-list to whole region files: no new readable path, ~100 bytes
 * per file instead of megabytes, and it reuses the sha256 already in the
 * binary (main.c's hash_file_hex / source/sha256.c). */
static inline int cw_saves_hash_run(const struct cw_saves_fs_ops* fs,
									void* fs_user,
									const struct cw_saves_emit_ops* out,
									void* out_user,
									const struct cw_saves_list_req* req,
									const char** reason) {
	char world[CW_SAVES_PATH_MAX];
	char hex[CW_SAVES_DIRENT_MAX * 2 + 1];
	char head[CW_SAVES_PATH_MAX + CW_SAVES_DIRENT_MAX * 2 + 256];
	char tail[64];
	struct cw_saves__hash_ctx hctx;
	int counted;

	*reason = "bad_name";
	if(req->name_len == 0)
		return CW_SAVES_RC_FAIL;
	if(!cw_saves_path_join(req->root.abs, req->name, req->name_len, world,
						   sizeof(world))) {
		*reason = "unsupported_name";
		return CW_SAVES_RC_FAIL;
	}
	if(!cw_saves__stat_dir(fs, fs_user, world)) {
		*reason = "src_missing";
		return CW_SAVES_RC_FAIL;
	}
	/* Counting pass FIRST, so the cap is reported as a clean FAIL instead of
	 * truncating a reply that already claimed PASS. */
	counted = cw_saves__walk_world(fs, fs_user, world, NULL, NULL);
	if(counted < 0) {
		*reason = counted == -2 ? "too_many_files" : "read_failed";
		return CW_SAVES_RC_FAIL;
	}
	if(!cw_saves_hex_encode(req->name, req->name_len, hex, sizeof(hex))) {
		*reason = "unsupported_name";
		return CW_SAVES_RC_FAIL;
	}

	snprintf(head, sizeof(head),
			 "{\"status\":\"PASS\",\"reason\":\"saves_hash\",\"root\":\"%s\","
			 "\"root_abs\":\"%s\",\"name_hex\":\"%s\",\"files\":[",
			 req->root.token, req->root.abs, hex);
	if(!cw_saves__emit(out, out_user, head))
		return CW_SAVES_RC_ABORT;

	memset(&hctx, 0, sizeof(hctx));
	hctx.fs = fs;
	hctx.fs_user = fs_user;
	hctx.out = out;
	hctx.out_user = out_user;
	/* The directory can change or fail between the counting pass and the
	 * streamed pass.  Once PASS has begun we cannot replace it with FAIL, so
	 * abort the connection on ANY second-walk failure; a partial JSON reply
	 * must never receive a successful trailer. */
	if(cw_saves__walk_world(fs, fs_user, world, cw_saves__hash_file, &hctx) < 0)
		return CW_SAVES_RC_ABORT;
	if(hctx.emitted != counted)
		return CW_SAVES_RC_ABORT;

	snprintf(tail, sizeof(tail), "],\"count\":%d}\n", hctx.emitted);
	if(!cw_saves__emit(out, out_user, tail))
		return CW_SAVES_RC_ABORT;
	*reason = "";
	return CW_SAVES_RC_OK;
}


/* CWSAVES1 <root> <offset> <limit> meta <name_hex>
 *
 * Streams the bounded bytes of ONE world's level.dat as hex, on the same
 * connection, addressed by the world's name_hex. See CW_SAVES_META_FILE for
 * why the metadata read lives here instead of in fetch_file().
 *
 * `bytes` and `truncated` are in the TRAILER, so the declared length covers
 * exactly the bytes that preceded it -- there is no second pass free to
 * disagree with the first (the 2026-09-02 fetch_file corruption class).
 * Integrity is the gzip container's own CRC32 + ISIZE: a corrupted read
 * fails the PC side's gunzip and is reported "name undecodable" for that one
 * world, which is the same outcome as any other unreadable metadata. */
static inline int cw_saves_meta_run(const struct cw_saves_fs_ops* fs,
									void* fs_user,
									const struct cw_saves_emit_ops* out,
									void* out_user,
									const struct cw_saves_list_req* req,
									const char** reason) {
	char world[CW_SAVES_PATH_MAX];
	char abs[CW_SAVES_PATH_MAX];
	char hex[CW_SAVES_DIRENT_MAX * 2 + 1];
	char head[CW_SAVES_PATH_MAX + CW_SAVES_DIRENT_MAX * 2 + 256];
	char tail[96];
	void* fh;
	unsigned long total = 0;
	int truncated = 0;
	int read_error = 0;

	*reason = "bad_name";
	if(req->name_len == 0)
		return CW_SAVES_RC_FAIL;
	if(!cw_saves_path_join(req->root.abs, req->name, req->name_len, world,
						   sizeof(world))
	   || !cw_saves_path_join(world, (const unsigned char*)CW_SAVES_META_FILE,
							  strlen(CW_SAVES_META_FILE), abs, sizeof(abs))) {
		*reason = "unsupported_name";
		return CW_SAVES_RC_FAIL;
	}
	if(!cw_saves__stat_dir(fs, fs_user, world)) {
		*reason = "src_missing";
		return CW_SAVES_RC_FAIL;
	}
	if(!fs->openfile) {
		*reason = "meta_unsupported";
		return CW_SAVES_RC_FAIL;
	}
	fh = fs->openfile(fs_user, abs);
	if(!fh) {
		*reason = "meta_missing";
		return CW_SAVES_RC_FAIL;
	}
	if(!cw_saves_hex_encode(req->name, req->name_len, hex, sizeof(hex))) {
		fs->closefile(fs_user, fh);
		*reason = "unsupported_name";
		return CW_SAVES_RC_FAIL;
	}

	snprintf(head, sizeof(head),
			 "{\"status\":\"PASS\",\"reason\":\"saves_meta\",\"root\":\"%s\","
			 "\"root_abs\":\"%s\",\"name_hex\":\"%s\",\"file\":\"%s\","
			 "\"data_hex\":\"",
			 req->root.token, req->root.abs, hex, CW_SAVES_META_FILE);
	if(!cw_saves__emit(out, out_user, head)) {
		fs->closefile(fs_user, fh);
		return CW_SAVES_RC_ABORT;
	}
	for(;;) {
		unsigned char buf[CW_SAVES_META_CHUNK];
		char chunk_hex[CW_SAVES_META_CHUNK * 2 + 1];
		int n = fs->readfile(fs_user, fh, buf, sizeof(buf));
		/* The ops contract has always said "< 0 = read error" and this loop
		 * used to take the SAME exit for it as for EOF -- producing
		 * status:PASS, truncated:false and a `bytes` that understated the
		 * file, i.e. exactly the shape LIST was fixed not to emit (C9
		 * re-review, N5). The status line is already on the wire and cannot
		 * be retracted (stage_send.h), so the TRAILER carries the flag and
		 * the client refuses the content on it. */
		if(n < 0) {
			read_error = 1;
			break;
		}
		if(n == 0)
			break;
		if(total + (unsigned long)n > CW_SAVES_META_MAX_BYTES) {
			truncated = 1;
			break;
		}
		if(!cw_saves_hex_encode(buf, (size_t)n, chunk_hex, sizeof(chunk_hex))
		   || !cw_saves__emit(out, out_user, chunk_hex)) {
			fs->closefile(fs_user, fh);
			return CW_SAVES_RC_ABORT;
		}
		total += (unsigned long)n;
	}
	fs->closefile(fs_user, fh);

	snprintf(tail, sizeof(tail),
			 "\",\"bytes\":%lu,\"truncated\":%s,\"read_error\":%s}\n", total,
			 truncated ? "true" : "false", read_error ? "true" : "false");
	if(!cw_saves__emit(out, out_user, tail))
		return CW_SAVES_RC_ABORT;
	*reason = "";
	return CW_SAVES_RC_OK;
}

/* CWSAVES1 <root> <offset> <limit> [plain|summary]
 *
 * ONE directory pass. Entries before `offset` are hashed into the running
 * FNV-1a and skipped without a stat; entries inside the page window are
 * stat()ed and streamed one at a time (so the 20 s silence deadline re-arms
 * per entry, main.c:96-113); entries after it are only counted, which is
 * what makes `total` free. "." ".." and every other leading-dot name are
 * skipped -- exactly the filter the game's own Select World scan applies
 * (screen_select_world.c:189-228).
 *
 * The computed fields (count/next/total/unsupported/prefix_fnv1a) are in the
 * TRAILER, not the prefix: the reply STREAMS like send_manifest()
 * (main.c:475-491) and none of them are knowable before the walk. (The
 * design sketch showed them in the header; that is the one thing in it that
 * cannot be implemented as written.)
 *
 * prefix_fnv1a covers the name_hex of entries [0, min(offset+limit, total)),
 * i.e. exactly [0, offset+count) whenever the page is non-empty, with a '\n'
 * after each so two different splits of the same hex stream cannot collide.
 * The client recomputes it from the pages it already holds; a mismatch, or a
 * `total` that moved between pages, means the directory changed mid-walk and
 * the WHOLE inventory must be rejected. Both checks are needed: an entry
 * appended at the end leaves the prefix intact and only moves `total`. */
static inline int cw_saves_list_run(const struct cw_saves_fs_ops* fs,
									void* fs_user,
									const struct cw_saves_emit_ops* out,
									void* out_user,
									const struct cw_saves_list_req* req,
									const char** reason) {
	void* dir;
	long index = 0;
	long count = 0;
	long total = 0;
	long unsupported = 0;
	long stat_failed = 0;
	int read_error = 0;
	long window_end;
	uint32_t fnv = CW_SAVES_FNV1A_INIT;
	uint32_t fnv_all = CW_SAVES_FNV1A_INIT;
	char head[CW_SAVES_PATH_MAX + 256];
	char tail[256];

	if(!fs || !out || !req || !reason)
		return CW_SAVES_RC_FAIL;
	if(req->mode == CW_SAVES_LIST_HASH)
		return cw_saves_hash_run(fs, fs_user, out, out_user, req, reason);
	if(req->mode == CW_SAVES_LIST_META)
		return cw_saves_meta_run(fs, fs_user, out, out_user, req, reason);

	{
		struct cw_saves_stat root_stat;
		memset(&root_stat, 0, sizeof(root_stat));
		*reason = "root_missing";
		if(!fs->stat(fs_user, req->root.abs, &root_stat)) {
			int error_no = fs->last_error(fs_user);
			if(error_no <= 0)
				error_no = 5; /* EIO when a backend failed without setting errno */
			snprintf(head, sizeof(head),
					 "{\"status\":\"ERROR\",\"reason\":\"root_error\",\"errno\":%d}\n",
					 error_no);
			if(!cw_saves__emit(out, out_user, head))
				return CW_SAVES_RC_ABORT;
			*reason = "";
			return CW_SAVES_RC_OK;
		}
		if(!root_stat.exists || !root_stat.is_dir)
			return CW_SAVES_RC_FAIL;
	}
	dir = fs->opendir(fs_user, req->root.abs);
	if(!dir) {
		*reason = "open_failed";
		return CW_SAVES_RC_FAIL;
	}
	window_end = req->offset + req->limit;

	snprintf(head, sizeof(head),
			 "{\"status\":\"PASS\",\"reason\":\"saves_list\",\"root\":\"%s\","
			 "\"root_abs\":\"%s\",\"offset\":%ld,\"limit\":%ld,\"mode\":\"%s\","
			 "\"entries\":[",
			 req->root.token, req->root.abs, req->offset, req->limit,
			 req->mode == CW_SAVES_LIST_SUMMARY ? "summary" : "plain");
	if(!cw_saves__emit(out, out_user, head)) {
		fs->closedir(fs_user, dir);
		return CW_SAVES_RC_ABORT;
	}

	for(;;) {
		unsigned char name[CW_SAVES_DIRENT_MAX];
		size_t name_len = 0;
		char hex[CW_SAVES_DIRENT_MAX * 2 + 1];
		int r = fs->readdir(fs_user, dir, name, sizeof(name), &name_len);
		if(r <= 0) {
			if(r < 0) {
				read_error = fs->last_error(fs_user);
				if(read_error <= 0)
					read_error = 5; /* error can never become a PASS trailer */
			}
			break;
		}
		if(name_len == 0 || name[0] == '.')
			continue;
		++total;
		if(!cw_saves_hex_encode(name, name_len, hex, sizeof(hex)))
			hex[0] = '\0';
		/* Over EVERY entry of this walk, not just the delivered prefix: a
		 * DELETE paired with an INSERT at or after the page boundary -- what
		 * FAT does when a directory slot is freed and reused -- leaves
		 * `total` unchanged and every prefix matching, so the client used to
		 * accept an inventory that had silently lost a world (C9 review,
		 * M2). Every page of one walk reports the same all_fnv1a; a client
		 * that sees two different values across its pages has been handed a
		 * mix of views and must reject the whole inventory. */
		fnv_all = cw_saves_fnv1a(fnv_all, hex);
		fnv_all = cw_saves_fnv1a(fnv_all, "\n");
		if(index >= window_end) {
			++index;
			continue; /* only the counts matter out here */
		}
		fnv = cw_saves_fnv1a(fnv, hex);
		fnv = cw_saves_fnv1a(fnv, "\n");
		if(index < req->offset) {
			++index;
			continue; /* skipped without a stat -- paging is cheap */
		}
		{
			char abs[CW_SAVES_PATH_MAX];
			char line[CW_SAVES_DIRENT_MAX * 3 + 384];
			struct cw_saves_stat st;
			int supported = cw_saves_name_ok(name, name_len);
			int stat_ok = 0;
			size_t used;
			memset(&st, 0, sizeof(st));
			if(cw_saves_path_join(req->root.abs, name, name_len, abs,
								  sizeof(abs)))
				stat_ok = fs->stat(fs_user, abs, &st);
			/* A stat that FAILED is NOT a stat that said "absent". Without
			 * this the row was wire-identical to a genuine zero-byte file,
			 * the operator's np_rows cross-check silently under-counted
			 * against the game's own list, and nothing reported why (C9
			 * review, M1). */
			if(!stat_ok)
				++stat_failed;
			if(!supported)
				++unsupported;
			/* ...and it is not a stat that said "not a directory" either.
			 * `st` is memset to 0 before the call, so a failed stat used to
			 * emit "is_dir":false -- a claim this code cannot back. The
			 * client's directory filter then dropped the row before it ever
			 * looked at stat_ok, so the guards that exist to refuse such a
			 * row BY NAME were unreachable in production and the operator
			 * got "matched 0 directories" instead of the documented "could
			 * not stat" refusal (C13 review, T9). The type is therefore
			 * reported as JSON null when it is not known. The field is
			 * always present, so no client has to infer it from a missing
			 * key. */
			used = (size_t)snprintf(
				line, sizeof(line),
				"%s{\"name_hex\":\"%s\",\"is_dir\":%s,\"bytes\":%lu,"
				"\"mtime\":%ld,\"supported\":%s,\"stat_ok\":%s",
				count ? "," : "", hex,
				stat_ok ? (st.is_dir ? "true" : "false") : "null", st.bytes,
				st.mtime, supported ? "true" : "false",
				stat_ok ? "true" : "false");
			if(cw_saves_name_json_safe(name, name_len) && used < sizeof(line))
				used += (size_t)snprintf(line + used, sizeof(line) - used,
										 ",\"name\":\"%.*s\"", (int)name_len,
										 (const char*)name);
			if(req->mode == CW_SAVES_LIST_SUMMARY && stat_ok && st.is_dir
			   && used < sizeof(line)) {
				struct cw_saves__summary sum;
				int walked;
				memset(&sum, 0, sizeof(sum));
				walked = cw_saves__walk_world(fs, fs_user, abs,
											  cw_saves__summary_file, &sum);
				if(walked >= 0) {
					used += (size_t)snprintf(
						line + used, sizeof(line) - used,
						",\"files\":%d,\"total_bytes\":%lu,\"newest\":%ld,"
						"\"oldest\":%ld",
						sum.files, sum.total_bytes, sum.newest, sum.oldest);
				} else {
					/* C32 (inventory stage_saves.h:1327-1339): a failed walk
					 * used to OMIT the four fields, and the client read a
					 * row with no total_bytes as one more ordinary row. They
					 * are now present and JSON null -- "this size is not
					 * known" -- with the reason beside them, which is what
					 * makes the tiny-world check refuse the row. */
					int se = fs->last_error(fs_user);
					if(se <= 0)
						se = 5;
					used += (size_t)snprintf(
						line + used, sizeof(line) - used,
						",\"files\":null,\"total_bytes\":null,"
						"\"newest\":null,\"oldest\":null,"
						"\"summary_error\":\"%s\",\"summary_errno\":%d",
						walked == -2 ? "too_many_files" : "read_failed",
						walked == -2 ? 0 : se);
				}
			}
			if(used < sizeof(line))
				snprintf(line + used, sizeof(line) - used, "}");
			if(!cw_saves__emit(out, out_user, line)) {
				fs->closedir(fs_user, dir);
				return CW_SAVES_RC_ABORT;
			}
			++count;
		}
		++index;
	}
	if(fs->closedir(fs_user, dir) < 0 && !read_error) {
		read_error = fs->last_error(fs_user);
		if(read_error <= 0)
			read_error = 5;
	}

	snprintf(tail, sizeof(tail),
			 "],\"count\":%ld,\"next\":%ld,\"total\":%ld,\"unsupported\":%ld,"
			 "\"stat_failed\":%ld,\"prefix_fnv1a\":\"%08x\","
			 "\"all_fnv1a\":\"%08x\",\"trailer_status\":\"%s\",\"errno\":%d}\n",
			 count, (req->offset + count < total) ? req->offset + count : -1L,
			 total, unsupported, stat_failed, (unsigned)fnv,
			 (unsigned)fnv_all, read_error ? "list_error" : "PASS", read_error);
	if(!cw_saves__emit(out, out_user, tail))
		return CW_SAVES_RC_ABORT;
	*reason = "";
	return CW_SAVES_RC_OK;
}

/* ------------------------------------------------------------------ */
/* 10. MOVE                                                            */
/* ------------------------------------------------------------------ */

/* Case-INSENSITIVE "does this directory already hold this name". A bare
 * stat() is not enough on its own: whether libfat's lookup folds case is not
 * something this repo can prove, and on FAT "World" and "WORLD" are the same
 * entry -- so a case-sensitive existence test could rename straight onto an
 * existing world. Costs one name-only scan of the destination directory (no
 * stats); the source-root scan below short-circuits after the first other
 * directory it finds. */
/* Returns 1 = holds it, 0 = does not, -1 = COULD NOT LOOK.
 *
 * opendir() returns NULL for ANY failure, not only "absent", and libfat's
 * open-directory table is finite while this server opens a directory per
 * page, per world walk and per collision scan. Reading NULL as "nothing can
 * collide" silently removed the case-insensitive backstop exactly when the
 * card was under pressure (C9 review, M3), so absence is now established by
 * a stat and everything else is reported as unknown. */
static inline int cw_saves__dir_holds_name(const struct cw_saves_fs_ops* fs,
										   void* user, const char* dir_abs,
										   const unsigned char* name,
										   size_t name_len) {
	void* dir;
	int found = 0;
	struct cw_saves_stat dst;
	if(!fs->stat(user, dir_abs, &dst))
		return -1; /* could not even stat the parent */
	if(!dst.exists)
		return 0; /* genuinely not there, so nothing can collide */
	if(!dst.is_dir)
		return -1; /* a FILE where the batch directory should be */
	dir = fs->opendir(user, dir_abs);
	if(!dir)
		return -1; /* it exists but we could not read it */
	for(;;) {
		unsigned char entry[CW_SAVES_DIRENT_MAX];
		size_t entry_len = 0;
		int r = fs->readdir(user, dir, entry, sizeof(entry), &entry_len);
		/* C32 (inventory stage_saves.h:1395-1418): `r <= 0` folded a READ
		 * ERROR into "end of directory", i.e. into "no entry here collides"
		 * -- the one answer this scan exists to be sure of. A scan that did
		 * not finish proves nothing about the names it never reached. */
		if(r < 0) {
			fs->closedir(user, dir);
			return -1;
		}
		if(r == 0)
			break;
		if(entry_len == 0 || entry[0] == '.')
			continue;
		if(cw_saves_name_ci_equal(entry, entry_len, name, name_len)) {
			found = 1;
			break;
		}
	}
	/* A close that failed after a MATCH changes nothing -- the collision is
	 * already proven. After a clean sweep it means the sweep itself cannot
	 * be trusted to have seen every slot. */
	if(fs->closedir(user, dir) < 0 && !found)
		return -1;
	return found;
}

/* Is there at least one OTHER world directory in `root_abs` besides
 * `except`? Short-circuits on the first one, so the cost is ~2 stats in
 * practice even on the 646-entry live card. */
/* Returns 1 = yes, 0 = no, -1 = COULD NOT LOOK (same reasoning as above;
 * this one used to fail in the safe direction -- would_empty_saves -- but it
 * reported the wrong reason, which sends the operator hunting the wrong
 * problem). */
static inline int cw_saves__has_other_dir(const struct cw_saves_fs_ops* fs,
										  void* user, const char* root_abs,
										  const unsigned char* except,
										  size_t except_len) {
	void* dir = fs->opendir(user, root_abs);
	int found = 0;
	if(!dir)
		return -1;
	for(;;) {
		unsigned char entry[CW_SAVES_DIRENT_MAX];
		size_t entry_len = 0;
		char abs[CW_SAVES_PATH_MAX];
		int r = fs->readdir(user, dir, entry, sizeof(entry), &entry_len);
		if(r <= 0)
			break;
		if(entry_len == 0 || entry[0] == '.')
			continue;
		if(cw_saves_name_ci_equal(entry, entry_len, except, except_len))
			continue;
		if(!cw_saves_path_join(root_abs, entry, entry_len, abs, sizeof(abs)))
			continue;
		if(cw_saves__stat_dir(fs, user, abs)) {
			found = 1;
			break;
		}
	}
	fs->closedir(user, dir);
	return found;
}

/* CWARCHIVE1 <archive|restore> <root> <batch> <name_hex>.
 *
 * ONE verb with a direction token, not two: the constraints are identical
 * modulo swapping the two roots, so one implementation = one validator = one
 * test surface, and the direction is a closed enum with no path in it.
 *
 * Returns 1 on success with *from_abs / *to_abs filled, or 0 with *reason
 * set. Nothing is ever removed, overwritten or copied on any path through
 * this function: a destination that exists is a hard refusal, and a failed
 * rename is reported as-is with no fallback.
 *
 * "In use" is structurally impossible and therefore not checked: CWRUN1
 * unmounts FAT and chainloads the game (main.c:874-899), so this process is
 * gone while the game runs and the game is not running while this answers.
 * There is no lock file in a Beta world to honour either (no session.lock
 * anywhere in source/). */
static inline int cw_saves_move_run(const struct cw_saves_fs_ops* fs,
									void* fs_user,
									const struct cw_saves_move_req* req,
									char* from_abs, size_t from_cap,
									char* to_abs, size_t to_cap,
									const char** reason) {
	char batch_dir[CW_SAVES_PATH_MAX];
	char world_side[CW_SAVES_PATH_MAX];
	char archive_side[CW_SAVES_PATH_MAX];
	const char* src;
	const char* dst;
	const char* dst_parent;

	if(!fs || !req || !reason)
		return 0;
	*reason = "bad_header";
	if(!cw_saves_root_is_world(&req->root)) {
		*reason = "bad_root";
		return 0;
	}
	if(!cw_saves_batch_id_valid(req->batch)) {
		*reason = "bad_batch";
		return 0;
	}
	if(!cw_saves_name_ok(req->name, req->name_len)) {
		*reason = "unsupported_name";
		return 0;
	}
	if(snprintf(batch_dir, sizeof(batch_dir), "%s/%s", CW_SAVES_ARCHIVE_ABS,
				req->batch)
	   >= (int)sizeof(batch_dir)) {
		*reason = "path_too_long";
		return 0;
	}
	if(!cw_saves_path_join(req->root.abs, req->name, req->name_len, world_side,
						   sizeof(world_side))
	   || !cw_saves_path_join(batch_dir, req->name, req->name_len,
							  archive_side, sizeof(archive_side))) {
		*reason = "path_too_long";
		return 0;
	}

	if(req->restore) {
		src = archive_side;
		dst = world_side;
		dst_parent = req->root.abs;
		/* The destination root must already exist: nothing is EVER created
		 * under saves by this verb. */
		if(!cw_saves__stat_dir(fs, fs_user, dst_parent)) {
			*reason = "dst_root_missing";
			return 0;
		}
	} else {
		src = world_side;
		dst = archive_side;
		dst_parent = batch_dir;
	}

	{
		struct cw_saves_stat st;
		if(!fs->stat(fs_user, src, &st)) {
			*reason = "stat_failed";
			return 0;
		}
		if(!st.exists) {
			*reason = "src_missing";
			return 0;
		}
		if(!st.is_dir) {
			*reason = "src_not_dir";
			return 0;
		}
	}

	/* Never leave Select World with nothing to show: the last world in the
	 * saves root cannot be archived. (Restore only ever adds one.) */
	if(!req->restore) {
		int other = cw_saves__has_other_dir(fs, fs_user, req->root.abs,
											req->name, req->name_len);
		if(other < 0) {
			*reason = "scan_failed";
			return 0;
		}
		if(!other) {
			*reason = "would_empty_saves";
			return 0;
		}
	}

	{
		struct cw_saves_stat st;
		int holds;
		if(!fs->stat(fs_user, dst, &st)) {
			*reason = "stat_failed";
			return 0;
		}
		if(st.exists) {
			*reason = "dest_exists";
			return 0;
		}
		holds = cw_saves__dir_holds_name(fs, fs_user, dst_parent, req->name,
										 req->name_len);
		if(holds < 0) {
			/* The backstop could not run. Refusing is the only honest
			 * answer: libfat's rename "does not reliably replace an
			 * existing target" is not "never replaces one". */
			*reason = "collision_check_failed";
			return 0;
		}
		if(holds) {
			*reason = "dest_exists";
			return 0;
		}
	}

	if(!req->restore) {
		if(!fs->mkdir(fs_user, CW_SAVES_ARCHIVE_ABS)
		   || !fs->mkdir(fs_user, batch_dir)) {
			*reason = "mkdir_failed";
			return 0;
		}
	}

	if(!fs->rename(fs_user, src, dst)) {
		*reason = "rename_failed";
		return 0;
	}
	if(from_abs && from_cap)
		snprintf(from_abs, from_cap, "%s", src);
	if(to_abs && to_cap)
		snprintf(to_abs, to_cap, "%s", dst);
	*reason = "";
	return 1;
}

#endif /* CAVEWII_STAGE_SAVES_H */
