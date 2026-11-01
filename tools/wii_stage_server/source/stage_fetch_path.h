#ifndef CAVEWII_STAGE_FETCH_PATH_H
#define CAVEWII_STAGE_FETCH_PATH_H

/* CWFETCH1's path argument, as pure policy a PC test can execute (the
 * stage_line.h / stage_saves.h precedent: main.c is Wii-only).
 *
 * hw1028a: the request line "CWFETCH1 <path>" used to be parsed with
 * sscanf("%15s %255s"), which stops at the first space. Every real world
 * backup is named "<world> (backup)", so the rig's backup fetch
 * "sd:/.../cw_wcc_fixture (backup)/level.dat" reached fopen() as
 * "sd:/.../cw_wcc_fixture" -- a directory -- and failed open_failed, and the
 * world_copy_convert ok/recover variants FAILED with "card backup_manifest
 * missing" although the device half was clean.
 *
 * The path is now the REST OF THE LINE after the verb and its separating
 * blanks, so embedded spaces survive. It stays bounded and refused, never
 * truncated: empty, longer than the caller's buffer, any control byte, or a
 * ".." anywhere is rejected here by name, before valid_sd_path() (which keeps
 * its own ".."/"//" and allow-list checks) ever sees it. Trailing blanks are
 * trimmed: FAT cannot store a name ending in a space, and a client's stray
 * trailing blank must not turn into an open_failed. stage_line.h has already
 * removed the '\r' and '\n'. */

#include <stddef.h>
#include <string.h>

enum {
	CW_FETCH_PATH_OK = 0,
	CW_FETCH_PATH_MISSING = -1,	 /* no verb, or nothing after it */
	CW_FETCH_PATH_TOO_LONG = -2, /* does not fit dst (refused, not cut) */
	CW_FETCH_PATH_CONTROL = -3,	 /* a byte < 0x20 or 0x7f inside */
	CW_FETCH_PATH_DOTDOT = -4,	 /* ".." anywhere */
	CW_FETCH_PATH_QUOTE = -5	 /* unbalanced quote, a '"' inside a path,
									or bytes after a closing quote */
};

/* spaced-paths (2026-10-30): the SAME rules, for every path-carrying verb.
 *
 * The CWSTAGE4 put header "CWSTAGE4 <size> <sha> <path>" was parsed with
 * sscanf("%255s") too, so putting ".../saves/CaveWii Tutorial/level.dat"
 * wrote a stray FILE "sd:/apps/CavEX/saves/CaveWii". A path argument
 * is now one of:
 *   - the LAST argument, unquoted: the rest of the line (inner spaces kept,
 *     trailing blanks trimmed) -- CWFETCH1, CWSTAGE*, CWDELETE1;
 *   - any argument, double-quoted: "sd:/a b/c" -- required for CWRUNPREP1 /
 *     CWRUN1's FIRST path when it contains a space, optional elsewhere.
 * No escapes: FAT cannot store '"' in a name, so a '"' inside a path is
 * refused rather than given a meaning. A path without spaces or quotes
 * parses exactly as the old "%255s" did, so old PC tools keep working; a
 * server that has this is advertised by the "spaced-paths" capability, and
 * the PC tools refuse to send a spaced path to a server that lacks it (an
 * old server would truncate it into a stray file). */

static inline int cw_stage_is_blank(char c) { return c == ' ' || c == '\t'; }

/* Skips `count` blank-separated tokens (the verb counts as one) and the
 * blanks after them. Returns NULL when the line has fewer tokens. */
static inline const char* cw_stage_skip_tokens(const char* line, int count) {
	if(!line)
		return NULL;
	const char* p = line;
	while(cw_stage_is_blank(*p))
		p++;
	for(int i = 0; i < count; i++) {
		const char* tok = p;
		while(*p && !cw_stage_is_blank(*p))
			p++;
		if(p == tok)
			return NULL;
		while(cw_stage_is_blank(*p))
			p++;
	}
	return p;
}

static inline int cw_stage_path_check(const char* p, size_t n, char* dst,
									  size_t dst_size) {
	if(n == 0)
		return CW_FETCH_PATH_MISSING;
	if(n >= dst_size)
		return CW_FETCH_PATH_TOO_LONG;
	for(size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];
		if(c < 0x20 || c == 0x7f)
			return CW_FETCH_PATH_CONTROL;
		if(c == '"')
			return CW_FETCH_PATH_QUOTE;
		if(c == '.' && i + 1 < n && p[i + 1] == '.')
			return CW_FETCH_PATH_DOTDOT;
	}
	memcpy(dst, p, n);
	dst[n] = '\0';
	return CW_FETCH_PATH_OK;
}

/* Reads one path argument at *cursor into dst. `last` = this is the final
 * argument (unquoted form takes the rest of the line; nothing may follow a
 * quoted one). Non-last unquoted form ends at the next blank. On success
 * *cursor moves past the argument and its trailing blanks. dst is "" on
 * every failure, so a refused path never reaches the caller. */
static inline int cw_stage_path_arg(const char** cursor, int last, char* dst,
									size_t dst_size) {
	if(!dst || dst_size == 0)
		return CW_FETCH_PATH_TOO_LONG;
	dst[0] = '\0';
	if(!cursor || !*cursor)
		return CW_FETCH_PATH_MISSING;
	const char* p = *cursor;
	while(cw_stage_is_blank(*p))
		p++;
	const char* body;
	size_t n;
	const char* after;
	if(*p == '"') {
		body = p + 1;
		const char* close = strchr(body, '"');
		if(!close)
			return CW_FETCH_PATH_QUOTE;
		n = (size_t)(close - body);
		after = close + 1;
		if(*after && !cw_stage_is_blank(*after))
			return CW_FETCH_PATH_QUOTE;
		while(cw_stage_is_blank(*after))
			after++;
		if(last && *after)
			return CW_FETCH_PATH_QUOTE;
	} else if(last) {
		body = p;
		n = strlen(p);
		while(n && cw_stage_is_blank(p[n - 1]))
			n--;
		after = p + strlen(p);
	} else {
		body = p;
		while(*p && !cw_stage_is_blank(*p))
			p++;
		n = (size_t)(p - body);
		after = p;
		while(cw_stage_is_blank(*after))
			after++;
	}
	int rc = cw_stage_path_check(body, n, dst, dst_size);
	if(rc != CW_FETCH_PATH_OK) {
		dst[0] = '\0';
		return rc;
	}
	*cursor = after;
	return CW_FETCH_PATH_OK;
}

/* The final path argument after `skip` leading tokens (verb included). */
static inline int cw_stage_tail_path(const char* line, int skip, char* dst,
									 size_t dst_size) {
	if(!dst || dst_size == 0)
		return CW_FETCH_PATH_TOO_LONG;
	dst[0] = '\0';
	const char* p = cw_stage_skip_tokens(line, skip);
	if(!p)
		return CW_FETCH_PATH_MISSING;
	return cw_stage_path_arg(&p, 1, dst, dst_size);
}

static inline int cw_stage_fetch_path(const char* line, char* dst,
									  size_t dst_size) {
	return cw_stage_tail_path(line, 1, dst, dst_size);
}

/* CWRUNPREP1 / CWRUN1: either no paths (*have = 0, server defaults) or
 * exactly "<dol> <args>" (*have = 1). One path alone is MISSING, as the old
 * "fields != 1 && fields != 3" check refused it. */
static inline int cw_stage_two_paths(const char* line, char* a, size_t a_cap,
									 char* b, size_t b_cap, int* have) {
	*have = 0;
	if(a && a_cap)
		a[0] = '\0';
	if(b && b_cap)
		b[0] = '\0';
	const char* p = cw_stage_skip_tokens(line, 1);
	if(!p)
		return CW_FETCH_PATH_MISSING;
	if(*p == '\0')
		return CW_FETCH_PATH_OK;
	int rc = cw_stage_path_arg(&p, 0, a, a_cap);
	if(rc == CW_FETCH_PATH_OK)
		rc = cw_stage_path_arg(&p, 1, b, b_cap);
	if(rc != CW_FETCH_PATH_OK) {
		if(a && a_cap)
			a[0] = '\0';
		return rc;
	}
	*have = 1;
	return CW_FETCH_PATH_OK;
}

/* Maps a parse result to the wire reason the server sends. */
static inline const char* cw_stage_path_reason(int rc) {
	switch(rc) {
	case CW_FETCH_PATH_TOO_LONG:
		return "path_too_long";
	case CW_FETCH_PATH_DOTDOT:
	case CW_FETCH_PATH_CONTROL:
	case CW_FETCH_PATH_QUOTE:
		return "bad_target";
	default:
		return "bad_header";
	}
}

#endif
