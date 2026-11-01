#ifndef CAVEWII_STAGE_DELETE_H
#define CAVEWII_STAGE_DELETE_H

/* CWDELETE1 (2026-10-30, spaced-paths): remove a file, an empty directory,
 * or a directory tree from the card, as pure policy + a walker over an ops
 * table so a PC test runs the SAME code main.c compiles (stage_saves.h
 * precedent).
 *
 *   CWDELETE1 <mode> <path>        mode: file | dir | tree
 *
 * Why: the space-truncating put left a stray FILE
 * "sd:/apps/CavEX/saves/CaveWii" on the card and nothing could remove
 * it without a card reader.
 *
 * Scope is the PUT allow-list (valid_sd_path), never wider, plus floors so
 * one request can never wipe something the rig cannot rebuild over the
 * network:
 *   - sd:/apps/<app>/... : file/dir need >= 2 segments below sd:/apps/
 *     (never an app dir itself); tree needs >= 3 (never a top-level app
 *     subdir such as saves/ -- one world at a time);
 *   - sd:/texturepacks/..., sd:/assets/... and sd:/beta/... : >= 1 segment below the root
 *     (never the root itself);
 *   - the exact root telemetry files of CW_STAGE_MANIFEST_PATHS: file mode;
 *   - never any ".../boot.dol" directly in an app dir (a missing boot.dol
 *     loses the HBC entry; PUT replaces it, nothing deletes it), never
 *     anything under sd:/apps/CaveWiiStage/ (this server's own app), never
 *     the token file (valid_sd_path already refuses it).
 * Every entry the tree walker removes re-passes valid_sd_path() and the
 * token check. Trailing '/' on the request is ignored. Depth and entry
 * count are bounded; on a bound the walk stops and reports, it never
 * silently leaves a half-answer as PASS. */

#include "stage_auth.h"

#include <stddef.h>
#include <string.h>

enum {
	CW_DELETE_MODE_BAD = 0,
	CW_DELETE_MODE_FILE = 1,
	CW_DELETE_MODE_DIR = 2,
	CW_DELETE_MODE_TREE = 3
};

#define CW_DELETE_MAX_DEPTH 24
#define CW_DELETE_MAX_ENTRIES 20000u
#define CW_DELETE_PATH_MAX 300

static inline int cw_delete_parse_mode(const char* s, size_t n) {
	if(n == 4 && strncmp(s, "file", 4) == 0)
		return CW_DELETE_MODE_FILE;
	if(n == 3 && strncmp(s, "dir", 3) == 0)
		return CW_DELETE_MODE_DIR;
	if(n == 4 && strncmp(s, "tree", 4) == 0)
		return CW_DELETE_MODE_TREE;
	return CW_DELETE_MODE_BAD;
}

/* Strips trailing '/' in place (keeps "sd:/"). */
static inline void cw_delete_strip_slashes(char* path) {
	size_t n = strlen(path);
	while(n > 4 && path[n - 1] == '/')
		path[--n] = '\0';
}

/* Non-empty '/'-separated segments in s (s has no "//"). */
static inline int cw_delete_segments(const char* s) {
	int count = 0;
	int in = 0;
	for(; *s; ++s) {
		if(*s == '/') {
			in = 0;
		} else if(!in) {
			in = 1;
			count++;
		}
	}
	return count;
}

/* 1 when `path` (trailing slashes already stripped) may be deleted in
 * `mode`. */
static inline int cw_delete_target_ok(const char* path, int mode) {
	if(mode == CW_DELETE_MODE_BAD || !valid_sd_path(path))
		return 0;
	if(ci_has_prefix(path, "sd:/apps/CaveWiiStage/") ||
	   ci_streq(path, "sd:/apps/CaveWiiStage"))
		return 0;
	if(ci_has_prefix(path, "sd:/apps/")) {
		const char* rest = path + 9;
		int segs = cw_delete_segments(rest);
		if(segs < (mode == CW_DELETE_MODE_TREE ? 3 : 2))
			return 0;
		if(segs == 2) {
			const char* slash = strchr(rest, '/');
			if(slash && ci_streq(slash + 1, "boot.dol"))
				return 0;
		}
		return 1;
	}
	if(ci_has_prefix(path, "sd:/texturepacks/"))
		return cw_delete_segments(path + 17) >= 1;
	if(ci_has_prefix(path, "sd:/assets/"))
		return cw_delete_segments(path + 11) >= 1;
	if(ci_has_prefix(path, "sd:/beta/"))
		return cw_delete_segments(path + 9) >= 1;
	/* An exact manifest root file: valid_sd_path passed, so it is one. */
	return mode == CW_DELETE_MODE_FILE;
}

/* Filesystem seam. stat: 1 = looked (exists/is_dir set), 0 = could not
 * look. readdir: 1 = entry, 0 = end, -1 = error. remove_file/remove_dir:
 * 1 = removed. */
struct cw_delete_fs_ops {
	int (*stat)(void* user, const char* path, int* exists, int* is_dir);
	void* (*opendir)(void* user, const char* path);
	int (*readdir)(void* user, void* dir, char* name, size_t name_cap);
	void (*closedir)(void* user, void* dir);
	int (*remove_file)(void* user, const char* path);
	int (*remove_dir)(void* user, const char* path);
	void* user;
};

enum {
	CW_DELETE_OK = 0,
	CW_DELETE_ABSENT = 1,	   /* nothing there (PASS, idempotent cleanup) */
	CW_DELETE_REFUSED = -1,	   /* bad_target */
	CW_DELETE_NOT_FILE = -2,   /* file mode on a directory */
	CW_DELETE_NOT_DIR = -3,	   /* dir/tree mode on a file */
	CW_DELETE_NOT_EMPTY = -4,  /* dir mode on a non-empty directory */
	CW_DELETE_IO = -5,		   /* stat/readdir/remove failed */
	CW_DELETE_TOO_DEEP = -6,   /* tree deeper than CW_DELETE_MAX_DEPTH */
	CW_DELETE_TOO_MANY = -7,   /* more than CW_DELETE_MAX_ENTRIES */
	CW_DELETE_TOO_LONG = -8	   /* a child path does not fit */
};

static inline const char* cw_delete_reason(int rc) {
	switch(rc) {
	case CW_DELETE_OK:
		return "deleted";
	case CW_DELETE_ABSENT:
		return "absent";
	case CW_DELETE_REFUSED:
		return "bad_target";
	case CW_DELETE_NOT_FILE:
		return "not_a_file";
	case CW_DELETE_NOT_DIR:
		return "not_a_dir";
	case CW_DELETE_NOT_EMPTY:
		return "dir_not_empty";
	case CW_DELETE_TOO_DEEP:
		return "too_deep";
	case CW_DELETE_TOO_MANY:
		return "too_many_entries";
	case CW_DELETE_TOO_LONG:
		return "path_too_long";
	default:
		return "delete_failed";
	}
}

/* First real child name of dir `path` into name; 1 found, 0 empty, -1 io.
 * The directory is reopened for every child instead of deleting while
 * iterating: whether libfat's readdir survives an unlink of the entry it
 * just returned is not something this tool should depend on. */
static inline int cw_delete_first_child(const struct cw_delete_fs_ops* fs,
										const char* path, char* name,
										size_t cap) {
	void* d = fs->opendir(fs->user, path);
	if(!d)
		return -1;
	int r;
	for(;;) {
		r = fs->readdir(fs->user, d, name, cap);
		if(r <= 0)
			break;
		if(strcmp(name, ".") != 0 && strcmp(name, "..") != 0)
			break;
	}
	fs->closedir(fs->user, d);
	return r < 0 ? -1 : r;
}

/* Recursive tree removal; path is a buffer of CW_DELETE_PATH_MAX that the
 * walk appends to and restores. */
static inline int cw_delete_tree_walk(const struct cw_delete_fs_ops* fs,
									  char* path, int depth,
									  unsigned* removed) {
	if(depth > CW_DELETE_MAX_DEPTH)
		return CW_DELETE_TOO_DEEP;
	size_t base = strlen(path);
	char name[CW_DELETE_PATH_MAX];
	for(;;) {
		int r = cw_delete_first_child(fs, path, name, sizeof(name));
		if(r < 0)
			return CW_DELETE_IO;
		if(r == 0)
			break;
		size_t nlen = strlen(name);
		if(base + 1 + nlen >= CW_DELETE_PATH_MAX)
			return CW_DELETE_TOO_LONG;
		path[base] = '/';
		memcpy(path + base + 1, name, nlen + 1);
		int rc;
		int exists = 0, is_dir = 0;
		if(!valid_sd_path(path) || ci_streq(path, CW_STAGE_TOKEN_PATH)) {
			rc = CW_DELETE_REFUSED;
		} else if(!fs->stat(fs->user, path, &exists, &is_dir) || !exists) {
			rc = CW_DELETE_IO;
		} else if(*removed >= CW_DELETE_MAX_ENTRIES) {
			rc = CW_DELETE_TOO_MANY;
		} else if(is_dir) {
			rc = cw_delete_tree_walk(fs, path, depth + 1, removed);
		} else {
			rc = fs->remove_file(fs->user, path) ? CW_DELETE_OK
												 : CW_DELETE_IO;
			if(rc == CW_DELETE_OK)
				(*removed)++;
		}
		path[base] = '\0';
		if(rc != CW_DELETE_OK)
			return rc;
	}
	if(*removed >= CW_DELETE_MAX_ENTRIES)
		return CW_DELETE_TOO_MANY;
	if(!fs->remove_dir(fs->user, path))
		return CW_DELETE_IO;
	(*removed)++;
	return CW_DELETE_OK;
}

/* The whole verb after parsing: policy, type check, removal. `target` is
 * modified (trailing slashes stripped). *removed = entries removed (files
 * plus directories), also on a partial failure. */
static inline int cw_stage_delete(const struct cw_delete_fs_ops* fs,
								  char* target, int mode,
								  unsigned* removed) {
	*removed = 0;
	cw_delete_strip_slashes(target);
	if(strlen(target) >= CW_DELETE_PATH_MAX)
		return CW_DELETE_TOO_LONG;
	if(!cw_delete_target_ok(target, mode))
		return CW_DELETE_REFUSED;
	int exists = 0, is_dir = 0;
	if(!fs->stat(fs->user, target, &exists, &is_dir))
		return CW_DELETE_IO;
	if(!exists)
		return CW_DELETE_ABSENT;
	if(mode == CW_DELETE_MODE_FILE) {
		if(is_dir)
			return CW_DELETE_NOT_FILE;
		if(!fs->remove_file(fs->user, target))
			return CW_DELETE_IO;
		*removed = 1;
		return CW_DELETE_OK;
	}
	if(!is_dir)
		return CW_DELETE_NOT_DIR;
	if(mode == CW_DELETE_MODE_DIR) {
		char name[CW_DELETE_PATH_MAX];
		int r = cw_delete_first_child(fs, target, name, sizeof(name));
		if(r < 0)
			return CW_DELETE_IO;
		if(r > 0)
			return CW_DELETE_NOT_EMPTY;
		if(!fs->remove_dir(fs->user, target))
			return CW_DELETE_IO;
		*removed = 1;
		return CW_DELETE_OK;
	}
	char path[CW_DELETE_PATH_MAX];
	memcpy(path, target, strlen(target) + 1);
	return cw_delete_tree_walk(fs, path, 0, removed);
}

#endif
