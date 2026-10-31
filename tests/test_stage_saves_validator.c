/* Stage-server saves LIST / HASH / MOVE verbs (push-0921/C8, design
 * tmp/jobs/push-0921/design-saves-archive.md v2 + astra-ruling-4).
 *
 * The stage server itself is Wii-only (gccore/wiiuse/network headers) and is
 * not part of the ctest-buildable PC object pools -- the same constraint
 * tests/test_fetch_wii_stage_file_sha_mismatch.py:9-21 records. So the whole
 * decision surface of the new verbs lives in the pure, header-only
 * tools/wii_stage_server/source/stage_saves.h (the precedent is stage_send.h
 * / stage_netinit.h: policy in a header, I/O injected through an ops struct),
 * and main.c is a thin wiring layer that supplies the real FAT calls. THIS
 * test drives exactly the code main.c compiles, against an in-memory fake
 * card.
 *
 * Covered here:
 *   1. the name validator's hostile table (traversal, mixed separators, 8.3
 *      aliases, case collisions, overlong, empty, "."/"..", NUL, trailing
 *      dot/space, unicode),
 *   2. the root ENUM (no client string ever reaches opendir), batch ids,
 *      the lossless hex name transport, and the CWSAVES1 `meta` read (which
 *      REPLACED the cw_saves_meta_path() fetch predicate -- that function is
 *      gone and fetch_file's allow-list is untouched by this work),
 *   3. the auth truth table (MOVE/RESTORE refuse soft-launch auto-auth),
 *   4. the LIST/HASH/MOVE drivers against a fake filesystem: paging,
 *      prefix_fnv1a + total (torn-directory detection), unsupported names
 *      reported and untouched, dest_exists / src_missing / would_empty_saves,
 *      and the "exactly one rename, never a delete/overwrite/copy" invariant.
 */

#include "stage_line.h"
#include "stage_saves.h"
#include "stage_token_bootstrap.h"

/* The loader reads at most CW_STAGE_TOKEN_MAX_BYTES; main.c owns the
 * constant, so the test states the value it is pinning. */
#define CW_STAGE_TOKEN_MAX_BYTES_FOR_TEST 128

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fake card: a flat table of absolute paths.                          */
/* ------------------------------------------------------------------ */

#define FAKE_MAX_NODES 128
#define FAKE_MAX_DIRS 8

struct fake_node {
	char path[CW_SAVES_PATH_MAX];
	/* FAT keeps ONE entry per directory slot with both a long name and an
	 * 8.3 short name; stat() resolves the entry under EITHER spelling while
	 * readdir hands back only the long one. `alias` is that second spelling
	 * (a full path), empty when the node has none. */
	char alias[CW_SAVES_PATH_MAX];
	int is_dir;
	unsigned long bytes;
	long mtime;
	char sha[CW_SAVES_SHA256_HEX_LENGTH + 1];
	unsigned char data[512];
	size_t data_len;
	/* Lets a node be LARGER than the fake's own buffer, by cycling: the
	 * meta cap is 64 KiB and no test wants a 64 KiB literal. */
	size_t virtual_len;
	/* Byte offset at which readfile returns a hard error instead of data
	 * or EOF. 0 = never. The ops contract has said "< 0 = read error" all
	 * along; nothing exercised it (C9 re-review, N5). */
	size_t read_fails_at;
	int open_fails;
	int present;
};

struct fake_dir {
	char path[CW_SAVES_PATH_MAX];
	int cursor;
	int entries_read;
	int in_use;
};

struct fake_fs {
	struct fake_node nodes[FAKE_MAX_NODES];
	int count;
	struct fake_dir dirs[FAKE_MAX_DIRS];
	/* Instrumentation for the invariants this design promises. */
	int renames;
	int mkdirs;
	int removes;    /* must stay 0 -- there is no delete path, ever */
	int overwrites; /* must stay 0 -- the SERVER's checks are what stop it */
	int fail_rename;
	int fail_mkdir;
	int fail_opendir; /* libfat's open-directory table is finite */
	int readdir_fails_after; /* -1 disabled; counts non-dot entries */
	int readdir_errno;
	int fail_closedir;
	int fail_hash_file;
	int last_error;
	/* Whether the fake folds case on lookup. BOTH settings are real
	 * possibilities -- stage_saves.h says in writing that "whether libfat's
	 * lookup folds case is not something this repo can prove" -- and each
	 * one makes a DIFFERENT destination guard the only thing standing
	 * between a request and an overwrite. Every destination scenario
	 * therefore runs twice (C9 re-review, N4). */
	int case_sensitive_lookup;
	char stat_fails[CW_SAVES_PATH_MAX]; /* this path's stat() itself fails */
};

static void fake_reset(struct fake_fs* fs) {
	memset(fs, 0, sizeof(*fs));
	fs->readdir_fails_after = -1;
}

static void fake_add(struct fake_fs* fs, const char* path, int is_dir,
					 unsigned long bytes, long mtime, const char* sha) {
	struct fake_node* n;
	assert(fs->count < FAKE_MAX_NODES);
	n = &fs->nodes[fs->count++];
	snprintf(n->path, sizeof(n->path), "%s", path);
	n->is_dir = is_dir;
	n->bytes = bytes;
	n->mtime = mtime;
	n->present = 1;
	if(sha) {
		snprintf(n->sha, sizeof(n->sha), "%s", sha);
	} else {
		memset(n->sha, '0', CW_SAVES_SHA256_HEX_LENGTH);
		n->sha[CW_SAVES_SHA256_HEX_LENGTH] = '\0';
	}
}

/* Adds a directory entry whose name is given as raw BYTES, so a test can
 * plant a non-ASCII or otherwise unsupported name on the fake card. */
static void fake_add_named(struct fake_fs* fs, const char* parent,
						   const unsigned char* name, size_t name_len,
						   int is_dir) {
	char path[CW_SAVES_PATH_MAX];
	size_t plen = strlen(parent);
	assert(plen + 1 + name_len + 1 < sizeof(path));
	memcpy(path, parent, plen);
	path[plen] = '/';
	memcpy(path + plen + 1, name, name_len);
	path[plen + 1 + name_len] = '\0';
	fake_add(fs, path, is_dir, 0, 0, NULL);
}

/* FAT is case-INSENSITIVE, so the fake card must be too. With a
 * case-SENSITIVE (POSIX) lookup, "sd:/saves/World" and "sd:/saves/WORLD"
 * are two nodes and the server's PRIMARY dest-exists guard -- the plain
 * stat(dst) -- can never fire on a case-differing collision, leaving only
 * the belt-and-braces CI scan under test. */
static int fake_ci_streq(const char* a, const char* b) {
	for(; *a && *b; ++a, ++b) {
		int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
		if(ca != cb)
			return 0;
	}
	return *a == *b;
}

static struct fake_node* fake_find(struct fake_fs* fs, const char* path) {
	int i;
	for(i = 0; i < fs->count; ++i) {
		struct fake_node* n = &fs->nodes[i];
		if(!n->present)
			continue;
		/* Either spelling resolves the SAME entry, exactly as FAT's
		 * long-name/short-name pair does. */
		if(fs->case_sensitive_lookup) {
			if(strcmp(n->path, path) == 0)
				return n;
			if(n->alias[0] && strcmp(n->alias, path) == 0)
				return n;
		} else {
			if(fake_ci_streq(n->path, path))
				return n;
			if(n->alias[0] && fake_ci_streq(n->alias, path))
				return n;
		}
	}
	return NULL;
}

static void fake_set_alias(struct fake_fs* fs, const char* path,
						   const char* alias) {
	struct fake_node* n = fake_find(fs, path);
	assert(n);
	snprintf(n->alias, sizeof(n->alias), "%s", alias);
}

static void fake_set_file(struct fake_fs* fs, const char* path,
						  const unsigned char* data, size_t len) {
	struct fake_node* n = fake_find(fs, path);
	assert(n && len <= sizeof(n->data));
	memcpy(n->data, data, len);
	n->data_len = len;
	n->virtual_len = len;
	n->bytes = (unsigned long)len;
}

static void fake_set_read_error(struct fake_fs* fs, const char* path,
								size_t at) {
	struct fake_node* n = fake_find(fs, path);
	assert(n);
	n->read_fails_at = at;
}

static void fake_set_virtual_len(struct fake_fs* fs, const char* path,
								 size_t len) {
	struct fake_node* n = fake_find(fs, path);
	assert(n && n->data_len > 0);
	n->virtual_len = len;
	n->bytes = (unsigned long)len;
}

/* Drops a node and its descendants -- used ONLY to simulate another session
 * changing the card between two LIST pages. The server has no delete path. */
static void fake_drop_tree(struct fake_fs* fs, const char* path) {
	size_t plen = strlen(path);
	int i;
	for(i = 0; i < fs->count; ++i) {
		struct fake_node* n = &fs->nodes[i];
		if(!n->present)
			continue;
		if(strcmp(n->path, path) == 0 ||
		   (strncmp(n->path, path, plen) == 0 && n->path[plen] == '/'))
			n->present = 0;
	}
}

/* True iff `path` is a direct child of `dir` (no further '/' after the one
 * that separates them). */
static int fake_is_child(const char* path, const char* dir) {
	size_t dlen = strlen(dir);
	size_t i;
	for(i = 0; i < dlen; ++i) {
		int ca = (path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i];
		int cb = (dir[i] >= 'A' && dir[i] <= 'Z') ? dir[i] + 32 : dir[i];
		if(!path[i] || ca != cb)
			return 0;
	}
	if(path[dlen] != '/')
		return 0;
	return strchr(path + dlen + 1, '/') == NULL;
}

static void* fake_opendir(void* user, const char* dir_abs) {
	struct fake_fs* fs = (struct fake_fs*)user;
	struct fake_node* n = fake_find(fs, dir_abs);
	int i;
	if(fs->fail_opendir)
		return NULL; /* libfat's open-directory table is finite */
	if(!n || !n->is_dir)
		return NULL;
	for(i = 0; i < FAKE_MAX_DIRS; ++i) {
		if(!fs->dirs[i].in_use) {
			fs->dirs[i].in_use = 1;
			fs->dirs[i].cursor = -2; /* "." then ".." then real entries */
			fs->dirs[i].entries_read = 0;
			snprintf(fs->dirs[i].path, sizeof(fs->dirs[i].path), "%s", dir_abs);
			return &fs->dirs[i];
		}
	}
	return NULL;
}

/* Deliberately hands back NO type flag, mirroring the ops contract: libfat's
 * dirent d_type is not dependable, so the driver has to stat what it needs
 * typed (the same reason screen_select_world.c uses stat() + S_ISDIR). */
static int fake_readdir(void* user, void* dirp, unsigned char* name_out,
						size_t name_cap, size_t* name_len) {
	struct fake_fs* fs = (struct fake_fs*)user;
	struct fake_dir* d = (struct fake_dir*)dirp;
	if(fs->readdir_fails_after >= 0 && d->entries_read >= fs->readdir_fails_after) {
		fs->last_error = fs->readdir_errno;
		return -1;
	}
	/* libfat hands "." and ".." back for any sub-directory, exactly like the
	 * game's own Select World scan has to cope with
	 * (screen_select_world.c:189-228 skips names starting '.'). */
	if(d->cursor == -2) {
		d->cursor = -1;
		name_out[0] = '.';
		name_out[1] = '\0';
		*name_len = 1;
		return 1;
	}
	if(d->cursor == -1) {
		d->cursor = 0;
		name_out[0] = '.';
		name_out[1] = '.';
		name_out[2] = '\0';
		*name_len = 2;
		return 1;
	}
	while(d->cursor < fs->count) {
		struct fake_node* n = &fs->nodes[d->cursor++];
		size_t dlen = strlen(d->path);
		size_t nlen;
		if(!n->present || !fake_is_child(n->path, d->path))
			continue;
		nlen = strlen(n->path) - dlen - 1;
		if(nlen + 1 > name_cap)
			return -1;
		memcpy(name_out, n->path + dlen + 1, nlen);
		name_out[nlen] = '\0';
		*name_len = nlen;
		++d->entries_read;
		return 1;
	}
	return 0;
}

static int fake_closedir(void* user, void* dirp) {
	struct fake_fs* fs = (struct fake_fs*)user;
	((struct fake_dir*)dirp)->in_use = 0;
	if(fs->fail_closedir) {
		fs->last_error = EIO;
		return -1;
	}
	return 0;
}

static int fake_stat(void* user, const char* abs, struct cw_saves_stat* out) {
	struct fake_fs* fs = (struct fake_fs*)user;
	struct fake_node* n;
	memset(out, 0, sizeof(*out));
	/* A stat that FAILED is not a stat that said "absent" -- main.c's
	 * cw_fs_stat keeps ENOENT (1, exists=0) apart from any other error (0),
	 * and every caller has to honour that distinction. */
	if(fs->stat_fails[0] && fake_ci_streq(fs->stat_fails, abs)) {
		fs->last_error = EIO;
		return 0;
	}
	n = fake_find(fs, abs);
	if(!n)
		return 1; /* stat ran; the path simply does not exist */
	out->exists = 1;
	out->is_dir = n->is_dir;
	out->bytes = n->bytes;
	out->mtime = n->mtime;
	return 1;
}

static int fake_mkdir(void* user, const char* abs) {
	struct fake_fs* fs = (struct fake_fs*)user;
	if(fs->fail_mkdir)
		return 0;
	if(fake_find(fs, abs))
		return 1; /* EEXIST is success, same as main.c's mkdir idiom */
	fs->mkdirs++;
	fake_add(fs, abs, 1, 0, 0, NULL);
	return 1;
}

/* Moves the node AND every descendant, which is what a FAT directory rename
 * does -- and is why the caller never has to copy or delete anything. */
/* Deliberately does NOT refuse an existing destination. main.c:973-977
 * records that libfat's rename "does not reliably replace an existing
 * target" -- and "not reliably" is not "never", so a fake that hard-refuses
 * is KINDER than the card and would let a missing server-side dest_exists
 * check pass every test. Here the clobber is allowed and COUNTED, so
 * fs.overwrites == 0 is a statement about the server's own checks. */
static int fake_rename(void* user, const char* from, const char* to) {
	struct fake_fs* fs = (struct fake_fs*)user;
	char real_from[CW_SAVES_PATH_MAX];
	size_t flen;
	int i;
	int moved = 0;
	struct fake_node* src;
	struct fake_node* victim;
	if(fs->fail_rename)
		return 0;
	src = fake_find(fs, from);
	if(!src)
		return 0;
	/* `from` may be EITHER spelling of the entry (long name or 8.3 alias);
	 * the children hang off the stored long-name path, so re-parent from
	 * that. The alias does not survive the move -- FAT derives a fresh short
	 * name in the destination directory. */
	snprintf(real_from, sizeof(real_from), "%s", src->path);
	src->alias[0] = 0;
	flen = strlen(real_from);
	victim = fake_find(fs, to);
	if(victim) {
		fs->overwrites++;
		victim->present = 0;
	}
	for(i = 0; i < fs->count; ++i) {
		struct fake_node* n = &fs->nodes[i];
		char rebuilt[CW_SAVES_PATH_MAX];
		if(!n->present)
			continue;
		if(strcmp(n->path, real_from) == 0) {
			snprintf(rebuilt, sizeof(rebuilt), "%s", to);
		} else if(strncmp(n->path, real_from, flen) == 0
				  && n->path[flen] == '/') {
			snprintf(rebuilt, sizeof(rebuilt), "%s%s", to, n->path + flen);
		} else {
			continue;
		}
		snprintf(n->path, sizeof(n->path), "%s", rebuilt);
		moved++;
	}
	fs->renames++;
	return moved > 0;
}

static int fake_hash_file(void* user, const char* abs, char* hex_out,
						  unsigned long* bytes_out) {
	struct fake_fs* fs = (struct fake_fs*)user;
	struct fake_node* n = fake_find(fs, abs);
	if(fs->fail_hash_file || !n || n->is_dir)
		return 0;
	snprintf(hex_out, CW_SAVES_SHA256_HEX_LENGTH + 1, "%s", n->sha);
	*bytes_out = n->bytes;
	return 1;
}

struct fake_file {
	struct fake_node* node;
	size_t pos;
	int in_use;
};

static struct fake_file g_fake_files[4];

static void* fake_openfile(void* user, const char* abs) {
	struct fake_fs* fs = (struct fake_fs*)user;
	struct fake_node* n = fake_find(fs, abs);
	int i;
	if(!n || n->is_dir || n->open_fails)
		return NULL;
	for(i = 0; i < 4; ++i) {
		if(!g_fake_files[i].in_use) {
			g_fake_files[i].in_use = 1;
			g_fake_files[i].node = n;
			g_fake_files[i].pos = 0;
			return &g_fake_files[i];
		}
	}
	return NULL;
}

static int fake_readfile(void* user, void* fh, unsigned char* buf,
						 size_t cap) {
	struct fake_file* f = (struct fake_file*)fh;
	size_t remain;
	(void)user;
	if(f->node->read_fails_at && f->pos >= f->node->read_fails_at)
		return -1;
	remain = f->node->virtual_len - f->pos;
	if(remain == 0)
		return 0;
	if(remain > cap)
		remain = cap;
	{
		size_t i;
		for(i = 0; i < remain; ++i)
			buf[i] = f->node->data[(f->pos + i) % f->node->data_len];
	}
	f->pos += remain;
	return (int)remain;
}

static void fake_closefile(void* user, void* fh) {
	(void)user;
	((struct fake_file*)fh)->in_use = 0;
}

static int fake_last_error(void* user) {
	return ((struct fake_fs*)user)->last_error;
}

static const struct cw_saves_fs_ops fake_ops
	= { fake_opendir,  fake_readdir,  fake_closedir, fake_stat,
		fake_mkdir,    fake_rename,   fake_hash_file,
		fake_openfile, fake_readfile, fake_closefile, fake_last_error };

/* ------------------------------------------------------------------ */
/* Reply capture                                                       */
/* ------------------------------------------------------------------ */

struct capture {
	char buf[16384];
	size_t len;
	int fail_after; /* 0 = never fail; else fail from the Nth emit on */
	int emits;
	/* A 64 KiB metadata body is 128 KiB of hex on the wire, which no test
	 * wants to hold. In sink mode the overflow is DISCARDED rather than
	 * treated as a dead connection, and the last piece emitted is kept so
	 * the trailer can still be asserted on. */
	int sink;
	char tail[256];
};

static int capture_emit(void* user, const char* text, size_t len) {
	struct capture* c = (struct capture*)user;
	c->emits++;
	if(c->fail_after && c->emits >= c->fail_after)
		return 0;
	if(c->sink) {
		size_t keep = len < sizeof(c->tail) - 1 ? len : sizeof(c->tail) - 1;
		memcpy(c->tail, text + (len - keep), keep);
		c->tail[keep] = 0;
		if(c->len + len + 1 > sizeof(c->buf))
			return 1; /* dropped on the floor, not a failure */
	}
	if(c->len + len + 1 > sizeof(c->buf))
		return 0;
	memcpy(c->buf + c->len, text, len);
	c->len += len;
	c->buf[c->len] = '\0';
	return 1;
}

static const struct cw_saves_emit_ops capture_ops = { capture_emit };

static void capture_reset(struct capture* c) {
	memset(c, 0, sizeof(*c));
}

static int has(const struct capture* c, const char* needle) {
	return strstr(c->buf, needle) != NULL;
}

/* ------------------------------------------------------------------ */
/* 1. The name validator's hostile table                               */
/* ------------------------------------------------------------------ */

struct name_case {
	const char* label;
	const unsigned char* bytes;
	size_t len;
	enum cw_saves_name_verdict want;
};

#define LIT(s) (const unsigned char*)(s), (sizeof(s) - 1)

static void test_name_validator(void) {
	/* Built out of line so a NUL byte and a >200-byte name can be expressed
	 * exactly, which a C string literal in the table cannot. */
	static unsigned char with_nul[] = { 'N', 'e', 'w', 0x00, 'W' };
	static unsigned char overlong[CW_SAVES_NAME_MAX_BYTES + 1];
	static unsigned char exactly_max[CW_SAVES_NAME_MAX_BYTES];
	static unsigned char utf8_name[] = { 0xe4, 0xb8, 0x96, 0xe7, 0x95, 0x8c };
	static unsigned char cp437_name[] = { 'c', 'a', 0xe4, 'e' };
	static unsigned char del_byte[] = { 'a', 0x7f, 'b' };
	static const struct name_case cases[] = {
		/* --- accepted --- */
		{ "plain world dir", LIT("New World 152"), CW_SAVES_NAME_OK },
		{ "space-free dialog dir", LIT("cworld152"), CW_SAVES_NAME_OK },
		{ "single char", LIT("a"), CW_SAVES_NAME_OK },
		{ "inner dot", LIT("save.bak"), CW_SAVES_NAME_OK },
		{ "leading space is a legal FAT LFN byte", LIT(" leading space"),
		  CW_SAVES_NAME_OK },
		/* An 8.3 short-name ALIAS is itself a perfectly legal FAT name --
		 * '~' is not reserved. It is accepted, and resolves the same way
		 * libfat would; see docs/wii_saves_archive.md for why that is safe
		 * (every path is server-built, the destination must not exist, and
		 * nothing is ever deleted or overwritten). */
		{ "8.3 alias", LIT("NEWWOR~1"), CW_SAVES_NAME_OK },
		{ "digits only", LIT("1"), CW_SAVES_NAME_OK },

		/* --- traversal / separators --- */
		{ "dot", LIT("."), CW_SAVES_NAME_DOT },
		/* L1: the single most important name to refuse must report the
		 * DOTDOT reason, not the generic DOT one -- the check order is
		 * documented as load-bearing for the reported reason. */
		{ "dotdot", LIT(".."), CW_SAVES_NAME_DOTDOT },
		{ "traversal prefix", LIT("../etc"), CW_SAVES_NAME_SEPARATOR },
		{ "traversal suffix", LIT("world/.."), CW_SAVES_NAME_SEPARATOR },
		{ "embedded dotdot", LIT("a..b"), CW_SAVES_NAME_DOTDOT },
		{ "forward slash", LIT("a/b"), CW_SAVES_NAME_SEPARATOR },
		{ "backslash", LIT("a\\b"), CW_SAVES_NAME_SEPARATOR },
		{ "mixed separators", LIT("a/..\\b"), CW_SAVES_NAME_SEPARATOR },
		{ "leading slash (absolute)", LIT("/saves"),
		  CW_SAVES_NAME_SEPARATOR },
		{ "device-absolute sd path", LIT("sd:/saves/x"),
		  CW_SAVES_NAME_SEPARATOR },
		{ "drive-absolute windows path", LIT("C:\\saves"),
		  CW_SAVES_NAME_SEPARATOR },
		{ "bare colon", LIT("a:b"), CW_SAVES_NAME_FAT_ILLEGAL },

		/* --- FAT-illegal bytes --- */
		{ "star", LIT("a*b"), CW_SAVES_NAME_FAT_ILLEGAL },
		{ "question", LIT("a?b"), CW_SAVES_NAME_FAT_ILLEGAL },
		{ "quote", LIT("a\"b"), CW_SAVES_NAME_FAT_ILLEGAL },
		{ "lt", LIT("a<b"), CW_SAVES_NAME_FAT_ILLEGAL },
		{ "gt", LIT("a>b"), CW_SAVES_NAME_FAT_ILLEGAL },
		{ "pipe", LIT("a|b"), CW_SAVES_NAME_FAT_ILLEGAL },

		/* --- FAT normalisation --- */
		{ "trailing dot", LIT("world."),
		  CW_SAVES_NAME_TRAILING_DOT_OR_SPACE },
		{ "trailing space", LIT("world "),
		  CW_SAVES_NAME_TRAILING_DOT_OR_SPACE },
		{ "leading dot (hidden; the game skips these too)", LIT(".hidden"),
		  CW_SAVES_NAME_LEADING_DOT },

		/* --- control bytes --- */
		{ "newline", LIT("a\nb"), CW_SAVES_NAME_CONTROL },
		{ "tab", LIT("a\tb"), CW_SAVES_NAME_CONTROL },

		/* --- length --- */
		{ "empty", LIT(""), CW_SAVES_NAME_EMPTY }
	};
	size_t i;

	for(i = 0; i < sizeof(overlong); ++i)
		overlong[i] = 'w';
	for(i = 0; i < sizeof(exactly_max); ++i)
		exactly_max[i] = 'w';

	for(i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		const struct name_case* c = &cases[i];
		enum cw_saves_name_verdict got = cw_saves_name_check(c->bytes, c->len);
		if(got != c->want) {
			fprintf(stderr, "name case '%s': got %d want %d\n", c->label,
					(int)got, (int)c->want);
			assert(0 && "name validator hostile table");
		}
	}

	/* Cases the table cannot express as string literals. */
	assert(cw_saves_name_check(with_nul, sizeof(with_nul))
		   == CW_SAVES_NAME_CONTROL);
	assert(cw_saves_name_check(del_byte, sizeof(del_byte))
		   == CW_SAVES_NAME_CONTROL);
	assert(cw_saves_name_check(overlong, sizeof(overlong))
		   == CW_SAVES_NAME_TOO_LONG);
	assert(cw_saves_name_check(exactly_max, sizeof(exactly_max))
		   == CW_SAVES_NAME_OK);
	/* Names are opaque BYTES (design S3): high bytes are NOT decoded, so both
	 * a UTF-8 and a CP437 name are accepted and travel losslessly as hex. The
	 * server never has to know which encoding libfat handed it. */
	assert(cw_saves_name_check(utf8_name, sizeof(utf8_name))
		   == CW_SAVES_NAME_OK);
	assert(cw_saves_name_check(cp437_name, sizeof(cp437_name))
		   == CW_SAVES_NAME_OK);
	assert(cw_saves_name_check(NULL, 4) == CW_SAVES_NAME_EMPTY);
}

static void test_name_case_insensitive_equality(void) {
	/* FAT is case-insensitive, so two names differing only by ASCII case
	 * CANNOT coexist -- every existence test has to fold. */
	assert(cw_saves_name_ci_equal(LIT("New World"), LIT("NEW WORLD")));
	assert(cw_saves_name_ci_equal(LIT("newwor~1"), LIT("NEWWOR~1")));
	assert(!cw_saves_name_ci_equal(LIT("New World"), LIT("New World 2")));
	assert(!cw_saves_name_ci_equal(LIT("New World 2"), LIT("New World")));
	/* Folding is ASCII-ONLY on purpose: what libfat does to a high byte is
	 * codepage-dependent and not determinable from this repo, so high bytes
	 * compare EXACTLY rather than through a guessed case table. */
	{
		static const unsigned char a[] = { 0xe4, 'x' };
		static const unsigned char b[] = { 0xc4, 'X' };
		assert(!cw_saves_name_ci_equal(a, sizeof(a), b, sizeof(b)));
	}
	/* Two batch ids differing only by case are the same FAT directory. */
	assert(cw_saves_name_ci_equal(LIT("20260921-A"), LIT("20260921-a")));
}

static void test_batch_ids(void) {
	assert(cw_saves_batch_id_valid("20260921-151200"));
	assert(cw_saves_batch_id_valid("a"));
	assert(cw_saves_batch_id_valid("a_b-c.d"));
	assert(cw_saves_batch_id_valid("0123456789012345678901234567890a")); /* 32 */

	assert(!cw_saves_batch_id_valid("")); /* empty */
	assert(!cw_saves_batch_id_valid("0123456789012345678901234567890ab")); /* 33 */
	assert(!cw_saves_batch_id_valid("."));
	assert(!cw_saves_batch_id_valid(".."));
	assert(!cw_saves_batch_id_valid(".hidden"));
	assert(!cw_saves_batch_id_valid("trailing.")); /* FAT drops it */
	assert(!cw_saves_batch_id_valid("trailing ")); /* FAT drops it */
	assert(!cw_saves_batch_id_valid("a..b"));
	assert(!cw_saves_batch_id_valid("a/b"));
	assert(!cw_saves_batch_id_valid("a\\b"));
	assert(!cw_saves_batch_id_valid("a b")); /* space is not in the charset */
	assert(!cw_saves_batch_id_valid("a\tb"));
	assert(!cw_saves_batch_id_valid("\xe4\xb8\x96")); /* non-ASCII */
	assert(!cw_saves_batch_id_valid(NULL));
}

/* ------------------------------------------------------------------ */
/* 2. Root enum, hex transport, meta-path predicate                    */
/* ------------------------------------------------------------------ */

static void test_root_enum(void) {
	struct cw_saves_root r;

	assert(cw_saves_root_parse("saves", &r));
	assert(r.kind == CW_SAVES_ROOT_SAVES);
	assert(strcmp(r.abs, "sd:/saves") == 0);

	assert(cw_saves_root_parse("saves_app", &r));
	assert(r.kind == CW_SAVES_ROOT_SAVES_APP);
	assert(strcmp(r.abs, "sd:/apps/CavEX/saves") == 0);

	assert(cw_saves_root_parse("archive", &r));
	assert(r.kind == CW_SAVES_ROOT_ARCHIVE);
	assert(strcmp(r.abs, "sd:/cw_saves_archive") == 0);

	assert(cw_saves_root_parse("archive/20260921-151200", &r));
	assert(r.kind == CW_SAVES_ROOT_ARCHIVE_BATCH);
	assert(strcmp(r.batch, "20260921-151200") == 0);
	assert(strcmp(r.abs, "sd:/cw_saves_archive/20260921-151200") == 0);

	/* Anything that is not one of the four forms is refused BEFORE any path
	 * is built -- no client string ever reaches opendir (design S4). */
	assert(!cw_saves_root_parse("sd:/saves", &r));
	assert(!cw_saves_root_parse("saves/", &r));
	assert(!cw_saves_root_parse("saves/..", &r));
	assert(!cw_saves_root_parse("../saves", &r));
	assert(!cw_saves_root_parse("archive/", &r));
	assert(!cw_saves_root_parse("archive/..", &r));
	assert(!cw_saves_root_parse("archive/a/b", &r));
	assert(!cw_saves_root_parse("archive/trailing.", &r));
	assert(!cw_saves_root_parse("SAVES", &r)); /* enum tokens are exact */
	assert(!cw_saves_root_parse("", &r));
	assert(!cw_saves_root_parse(NULL, &r));
}

static void test_hex_transport(void) {
	unsigned char out[CW_SAVES_DIRENT_MAX];
	char hex[CW_SAVES_DIRENT_MAX * 2 + 1];
	size_t n = 0;

	/* Round trip: a name with a SPACE -- the reason names travel as hex at
	 * all (the header line is whitespace-tokenised, design S3). */
	assert(cw_saves_hex_encode(LIT("New World 152"), hex, sizeof(hex)));
	assert(strcmp(hex, "4e657720576f726c6420313532") == 0);
	assert(cw_saves_hex_decode(hex, out, sizeof(out), &n));
	assert(n == 13 && memcmp(out, "New World 152", 13) == 0);

	/* Non-ASCII round trip, byte for byte. */
	{
		static const unsigned char u[] = { 0xe4, 0xb8, 0x96, 0x20, 0x41 };
		assert(cw_saves_hex_encode(u, sizeof(u), hex, sizeof(hex)));
		assert(strcmp(hex, "e4b8962041") == 0);
		assert(cw_saves_hex_decode(hex, out, sizeof(out), &n));
		assert(n == sizeof(u) && memcmp(out, u, n) == 0);
	}

	/* Uppercase hex decodes too (the tools emit lowercase). */
	assert(cw_saves_hex_decode("4E4557", out, sizeof(out), &n));
	assert(n == 3 && memcmp(out, "NEW", 3) == 0);

	/* Rejections. */
	assert(!cw_saves_hex_decode("abc", out, sizeof(out), &n));   /* odd */
	assert(!cw_saves_hex_decode("zz", out, sizeof(out), &n));    /* non-hex */
	assert(!cw_saves_hex_decode("41 41", out, sizeof(out), &n)); /* space */
	assert(!cw_saves_hex_decode("", out, sizeof(out), &n));      /* empty */
	assert(!cw_saves_hex_decode(NULL, out, sizeof(out), &n));
	/* Over-long input must not write past the caller's buffer. */
	{
		char big[65]; /* 64 hex chars = 32 bytes, into an 8-byte buffer */
		size_t i;
		for(i = 0; i + 1 < sizeof(big); ++i)
			big[i] = '4';
		big[sizeof(big) - 1] = '\0';
		assert(!cw_saves_hex_decode(big, out, 8, &n));
	}
	/* Encode refuses a buffer that cannot hold the hex + NUL rather than
	 * emitting a truncated (silently wrong) name. */
	assert(!cw_saves_hex_encode(LIT("abcd"), hex, 8));
	assert(cw_saves_hex_encode(LIT("abcd"), hex, 9));
}

static void test_fnv1a_matches_the_published_vectors(void) {
	/* The PC tools recompute this hash from the pages they hold, so the two
	 * implementations have to agree on the STANDARD algorithm, not merely on
	 * each other. Pinned here (and identically in tools/cw_saves_common.ps1's
	 * selftest) against the published 32-bit FNV-1a vectors. */
	assert(cw_saves_fnv1a(CW_SAVES_FNV1A_INIT, "") == 0x811c9dc5u);
	assert(cw_saves_fnv1a(CW_SAVES_FNV1A_INIT, "a") == 0xe40c292cu);
	assert(cw_saves_fnv1a(CW_SAVES_FNV1A_INIT, "foobar") == 0xbf9cf968u);
	/* Chaining is the same as hashing the concatenation, which is what lets
	 * the server accumulate entry by entry while the client accumulates page
	 * by page. */
	assert(cw_saves_fnv1a(cw_saves_fnv1a(CW_SAVES_FNV1A_INIT, "foo"), "bar")
		   == 0xbf9cf968u);
}

/* ------------------------------------------------------------------ */
/* 3. Auth truth table                                                 */
/* ------------------------------------------------------------------ */

static void test_auth_truth_table(void) {
	/* LIST / HASH ride the connection's existing auth state: a connection
	 * that has not handshaken is refused, because level.dat exposes world
	 * names, seed and player position (design S11 risk 8). */
	assert(cw_saves_auth_check(CW_SAVES_VERB_LIST, 0, 0)
		   == CW_SAVES_AUTH_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_LIST, 0, 1)
		   == CW_SAVES_AUTH_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_LIST, 1, 0) == CW_SAVES_AUTH_OK);
	assert(cw_saves_auth_check(CW_SAVES_VERB_LIST, 1, 1) == CW_SAVES_AUTH_OK);
	assert(cw_saves_auth_check(CW_SAVES_VERB_HASH, 0, 0)
		   == CW_SAVES_AUTH_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_HASH, 1, 0) == CW_SAVES_AUTH_OK);

	/* MOVE / RESTORE additionally require a REAL token (Astra Q3): the
	 * soft-launch auto-auth -- authed=1 with no token on the card -- is
	 * exactly the state every subnet peer is in today, and it must NOT be
	 * able to displace a world. */
	assert(cw_saves_auth_check(CW_SAVES_VERB_MOVE, 0, 0)
		   == CW_SAVES_AUTH_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_MOVE, 0, 1)
		   == CW_SAVES_AUTH_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_MOVE, 1, 0)
		   == CW_SAVES_AUTH_TOKEN_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_MOVE, 1, 1) == CW_SAVES_AUTH_OK);
}

/* ------------------------------------------------------------------ */
/* 4. Header parsing                                                   */
/* ------------------------------------------------------------------ */

static void test_header_parse(void) {
	struct cw_saves_list_req lq;
	struct cw_saves_move_req mq;
	const char* reason = NULL;

	assert(cw_saves_parse_list_header("CWSAVES1 saves 0 128", &lq, &reason));
	assert(lq.root.kind == CW_SAVES_ROOT_SAVES);
	assert(lq.offset == 0 && lq.limit == 128);
	assert(lq.mode == CW_SAVES_LIST_PLAIN);

	assert(cw_saves_parse_list_header("CWSAVES1 saves 256 32 summary", &lq,
									  &reason));
	assert(lq.mode == CW_SAVES_LIST_SUMMARY && lq.offset == 256);

	assert(cw_saves_parse_list_header(
		"CWSAVES1 saves 0 1 hash 4e657720576f726c64203135", &lq, &reason));
	assert(lq.mode == CW_SAVES_LIST_HASH);
	assert(lq.name_len == 12 && memcmp(lq.name, "New World 15", 12) == 0);

	/* Rejections, each with a distinct reason a client can act on. */
	assert(!cw_saves_parse_list_header("CWSAVES1", &lq, &reason));
	assert(strcmp(reason, "bad_header") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 bogus 0 8", &lq, &reason));
	assert(strcmp(reason, "bad_root") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves -1 8", &lq, &reason));
	assert(strcmp(reason, "bad_range") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 0", &lq, &reason));
	assert(strcmp(reason, "bad_range") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 257", &lq, &reason));
	assert(strcmp(reason, "bad_range") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 33 summary", &lq,
									   &reason));
	assert(strcmp(reason, "bad_range") == 0); /* summary caps at 32 */
	assert(!cw_saves_parse_list_header("CWSAVES1 saves x 8", &lq, &reason));
	assert(strcmp(reason, "bad_range") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 8 bogus", &lq,
									   &reason));
	assert(strcmp(reason, "bad_mode") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 1 hash", &lq,
									   &reason));
	assert(strcmp(reason, "bad_name") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 1 hash zz", &lq,
									   &reason));
	assert(strcmp(reason, "bad_name") == 0);
	/* 2e2e == "..", a name the validator refuses -- refused at PARSE time,
	 * so no path is ever built from it. */
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 1 hash 2e2e", &lq,
									   &reason));
	assert(strcmp(reason, "unsupported_name") == 0);

	assert(cw_saves_parse_move_header(
		"CWARCHIVE1 archive saves 20260921-151200 4e6577", &mq, &reason));
	assert(mq.restore == 0);
	assert(mq.root.kind == CW_SAVES_ROOT_SAVES);
	assert(strcmp(mq.batch, "20260921-151200") == 0);
	assert(mq.name_len == 3 && memcmp(mq.name, "New", 3) == 0);

	assert(cw_saves_parse_move_header(
		"CWARCHIVE1 restore saves_app 20260921-151200 4e6577", &mq, &reason));
	assert(mq.restore == 1);
	assert(mq.root.kind == CW_SAVES_ROOT_SAVES_APP);

	assert(!cw_saves_parse_move_header("CWARCHIVE1 sideways saves b 41", &mq,
									   &reason));
	assert(strcmp(reason, "bad_direction") == 0);
	/* The archive roots are NOT valid as the world root of a move: the
	 * archive side is always built from the batch id, never sent as a
	 * path. */
	assert(!cw_saves_parse_move_header("CWARCHIVE1 archive archive b 41", &mq,
									   &reason));
	assert(strcmp(reason, "bad_root") == 0);
	assert(!cw_saves_parse_move_header("CWARCHIVE1 archive archive/b b 41",
									   &mq, &reason));
	assert(strcmp(reason, "bad_root") == 0);
	assert(!cw_saves_parse_move_header("CWARCHIVE1 archive saves ... 41", &mq,
									   &reason));
	assert(strcmp(reason, "bad_batch") == 0);
	assert(!cw_saves_parse_move_header("CWARCHIVE1 archive saves b 2f", &mq,
									   &reason)); /* '/' */
	assert(strcmp(reason, "unsupported_name") == 0);
	assert(!cw_saves_parse_move_header("CWARCHIVE1 archive saves b", &mq,
									   &reason));
	assert(strcmp(reason, "bad_header") == 0);
	/* A 201-byte name (402 hex chars) is over the wire budget and is refused
	 * rather than truncated. */
	{
		char hdr[1024];
		int n = snprintf(hdr, sizeof(hdr), "CWARCHIVE1 archive saves b ");
		size_t i;
		for(i = 0; i < (CW_SAVES_NAME_MAX_BYTES + 1) * 2; ++i)
			hdr[(size_t)n + i] = '4';
		hdr[(size_t)n + i] = '\0';
		assert(!cw_saves_parse_move_header(hdr, &mq, &reason));
		assert(strcmp(reason, "unsupported_name") == 0);
	}
}

/* ------------------------------------------------------------------ */
/* 5. LIST driver                                                      */
/* ------------------------------------------------------------------ */

static void build_card(struct fake_fs* fs, int worlds) {
	int i;
	fake_reset(fs);
	fake_add(fs, "sd:/saves", 1, 0, 0, NULL);
	for(i = 0; i < worlds; ++i) {
		char dir[CW_SAVES_PATH_MAX];
		char lvl[CW_SAVES_PATH_MAX];
		snprintf(dir, sizeof(dir), "sd:/saves/New World %d", i + 1);
		snprintf(lvl, sizeof(lvl), "sd:/saves/New World %d/level.dat", i + 1);
		fake_add(fs, dir, 1, 0, 1757000000L + i, NULL);
		fake_add(fs, lvl, 0, 400 + (unsigned long)i, 1757000000L + i, NULL);
	}
}

static int run_list(struct fake_fs* fs, struct capture* cap, const char* hdr) {
	struct cw_saves_list_req req;
	const char* reason = NULL;
	capture_reset(cap);
	if(!cw_saves_parse_list_header(hdr, &req, &reason))
		return -1;
	return cw_saves_list_run(&fake_ops, fs, &capture_ops, cap, &req, &reason);
}

static void test_root_stat_error_is_not_missing(void) {
	struct fake_fs fs;
	struct capture cap;
	const char* reason = NULL;
	struct cw_saves_list_req req;
	fake_reset(&fs);
	assert(cw_saves_parse_list_header("CWSAVES1 archive 0 128 plain", &req, &reason));
	capture_reset(&cap);
	assert(cw_saves_list_run(&fake_ops, &fs, &capture_ops, &cap, &req, &reason)
	       == CW_SAVES_RC_FAIL);
	assert(strcmp(reason, "root_missing") == 0); /* genuine ENOENT */
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	snprintf(fs.stat_fails, sizeof(fs.stat_fails), "%s", "sd:/cw_saves_archive");
	capture_reset(&cap);
	assert(cw_saves_list_run(&fake_ops, &fs, &capture_ops, &cap, &req, &reason)
	       == CW_SAVES_RC_OK);
	assert(has(&cap, "\"status\":\"ERROR\""));
	assert(has(&cap, "\"reason\":\"root_error\""));
	assert(has(&cap, "\"errno\":5"));
	assert(!has(&cap, "root_missing"));
	printf("PASS test_root_stat_error_is_not_missing\n");
}

static void test_readdir_error_is_not_end_of_list(void) {
	struct fake_fs fs;
	struct capture cap;
	int n;
	for(n = 0; n <= 2; n += 2) {
		build_card(&fs, 2);
		fs.readdir_fails_after = n;
		fs.readdir_errno = EIO;
		assert(run_list(&fs, &cap, "CWSAVES1 saves 0 128 plain") == CW_SAVES_RC_OK);
		assert(has(&cap, "\"trailer_status\":\"list_error\""));
		assert(has(&cap, "\"errno\":5"));
		assert(has(&cap, n == 0 ? "\"count\":0" : "\"count\":2"));
		assert(!has(&cap, "\"trailer_status\":\"PASS\""));
	}
	build_card(&fs, 0);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 128 plain") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"count\":0"));
	assert(has(&cap, "\"trailer_status\":\"PASS\""));
	printf("PASS test_readdir_error_is_not_end_of_list\n");
}

static void test_closedir_error_is_not_end_of_list(void) {
	struct fake_fs fs;
	struct capture cap;
	build_card(&fs, 2);
	fs.fail_closedir = 1;
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 128 plain") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"trailer_status\":\"list_error\""));
	assert(has(&cap, "\"errno\":5"));
	assert(has(&cap, "\"count\":2"));
	assert(!has(&cap, "\"trailer_status\":\"PASS\""));
	printf("PASS test_closedir_error_is_not_end_of_list\n");
}

struct fake_token_temp {
	int present;
	int calls;
};

static int fake_clear_token_temp(void* user) {
	struct fake_token_temp* temp = (struct fake_token_temp*)user;
	++temp->calls;
	if(temp->present) {
		temp->present = 0;
		return 2;
	}
	return 1;
}

static void test_stale_token_temp_reprovisions_only_without_final(void) {
	struct fake_token_temp temp = { 1, 0 };
	assert(cw_stage_token_prepare_bootstrap(0, 0, fake_clear_token_temp, &temp) == 0);
	assert(temp.present && temp.calls == 0); /* final exists: never touched */
	assert(cw_stage_token_prepare_bootstrap(1, ENOENT, fake_clear_token_temp, &temp) == 2);
	assert(!temp.present && temp.calls == 1); /* stale temp removed: provision */
	assert(cw_stage_token_prepare_bootstrap(1, ENOENT, fake_clear_token_temp, &temp) == 1);
	assert(temp.calls == 2); /* ordinary first boot also provisions */
	printf("PASS test_stale_token_temp_reprovisions_only_without_final\n");
}

static void test_stage_token_state_table(void) {
	assert(cw_stage_token_state(1, 0, 0, 0, 32, 128) == CW_STAGE_TOKEN_PROVISIONED);
	assert(cw_stage_token_state(1, 1, ENOENT, 0, 0, 128) == CW_STAGE_TOKEN_BOOTSTRAP);
	assert(cw_stage_token_state(1, 1, EIO, 0, 0, 128) == CW_STAGE_TOKEN_LOCKED);
	assert(cw_stage_token_state(1, 0, 0, 1, 0, 128) == CW_STAGE_TOKEN_LOCKED);
	assert(cw_stage_token_state(1, 0, 0, 0, 0, 128) == CW_STAGE_TOKEN_LOCKED);
	assert(cw_stage_token_state(1, 0, 0, 0, 129, 128) == CW_STAGE_TOKEN_LOCKED);
	assert(cw_stage_token_state(0, 1, ENOENT, 0, 0, 128) == CW_STAGE_TOKEN_SOFT_LAUNCH);
	assert(cw_stage_token_state(0, 1, EIO, 0, 0, 128) == CW_STAGE_TOKEN_SOFT_LAUNCH);
	assert(cw_stage_token_state(0, 0, 0, 1, 0, 128) == CW_STAGE_TOKEN_SOFT_LAUNCH);
	assert(cw_stage_token_state(0, 0, 0, 0, 0, 128) == CW_STAGE_TOKEN_SOFT_LAUNCH);
	assert(cw_stage_token_state(0, 0, 0, 0, 129, 128) == CW_STAGE_TOKEN_PROVISIONED);
	printf("PASS test_stage_token_state_table\n");
}

/* ------------------------------------------------------------------ */
/* 2b. CWSAVES1 meta -- the metadata read, INSIDE the saves protocol      */
/* ------------------------------------------------------------------ */

static void test_meta_mode(void) {
	struct fake_fs fs;
	struct capture cap;
	static const unsigned char level[] = { 0x1f, 0x8b, 0x08, 0x00, 0x41, 0x42 };

	fake_reset(&fs);
	fake_add(&fs, "sd:/saves", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/saves/New World 152", 1, 0, 1757000000L, NULL);
	fake_add(&fs, "sd:/saves/New World 152/level.dat", 0, 0, 1757000000L, NULL);
	fake_set_file(&fs, "sd:/saves/New World 152/level.dat", level,
				  sizeof(level));

	/* The whole point: a name with SPACES, addressed by name_hex, answered
	 * on the SAME connection. The old route (CWFETCH1 <path>) could not do
	 * either -- main.c's sscanf("%15s %255s") truncates at the first space,
	 * so every live "New World <n>" fetch was refused with bad_target, and
	 * the caller had to open a second connection against a server that
	 * accepts exactly one. */
	assert(run_list(&fs, &cap,
					"CWSAVES1 saves 0 1 meta 4e657720576f726c6420313532")
		   == CW_SAVES_RC_OK);
	assert(has(&cap, "\"reason\":\"saves_meta\""));
	assert(has(&cap, "\"file\":\"level.dat\""));
	assert(has(&cap, "\"name_hex\":\"4e657720576f726c6420313532\""));
	assert(has(&cap, "\"data_hex\":\"1f8b08004142\""));
	/* bytes is in the TRAILER, so it counts exactly what was streamed and
	 * cannot disagree with the body the way a declared-then-read header
	 * can (the 2026-09-02 fetch_file bug). */
	assert(has(&cap, "\"bytes\":6"));
	assert(has(&cap, "\"truncated\":false"));
	assert(fs.renames == 0 && fs.overwrites == 0 && fs.removes == 0);

	/* Only the one allow-listed metadata file is reachable -- the request
	 * carries a world NAME, never a path, so nothing else can be named. */
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 1 meta 6e6f7065")
		   == CW_SAVES_RC_FAIL);
	assert(cap.len == 0);

	/* A world with no level.dat is a clean refusal, not a half reply. */
	fake_add(&fs, "sd:/saves/bare", 1, 0, 0, NULL);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 1 meta 62617265")
		   == CW_SAVES_RC_FAIL);
	assert(cap.len == 0);

	/* Over-cap content is reported truncated rather than streamed forever. */
	{
		unsigned char big[512];
		size_t i;
		for(i = 0; i < sizeof(big); ++i)
			big[i] = (unsigned char)i;
		fake_add(&fs, "sd:/saves/huge", 1, 0, 0, NULL);
		fake_add(&fs, "sd:/saves/huge/level.dat", 0, 0, 0, NULL);
		fake_set_file(&fs, "sd:/saves/huge/level.dat", big, sizeof(big));
		fake_set_virtual_len(&fs, "sd:/saves/huge/level.dat",
							 CW_SAVES_META_MAX_BYTES + 1024u);
		capture_reset(&cap);
		cap.sink = 1;
		{
			struct cw_saves_list_req req;
			const char* reason = NULL;
			assert(cw_saves_parse_list_header(
				"CWSAVES1 saves 0 1 meta 68756765", &req, &reason));
			assert(cw_saves_list_run(&fake_ops, &fs, &capture_ops, &cap, &req,
									 &reason)
				   == CW_SAVES_RC_OK);
		}
		assert(strstr(cap.tail, "\"truncated\":true") != NULL);
	}
}

static void test_list_basic_and_paging(void) {
	struct fake_fs fs;
	struct capture cap;

	build_card(&fs, 3);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 128") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"status\":\"PASS\""));
	assert(has(&cap, "\"reason\":\"saves_list\""));
	assert(has(&cap, "\"root\":\"saves\""));
	assert(has(&cap, "\"root_abs\":\"sd:/saves\""));
	/* Lossless name transport: hex is ALWAYS present, the readable name only
	 * when every byte is printable ASCII. */
	assert(has(&cap, "\"name_hex\":\"4e657720576f726c642031\"")); /* New World 1 */
	assert(has(&cap, "\"name\":\"New World 1\""));
	assert(has(&cap, "\"is_dir\":true"));
	assert(has(&cap, "\"supported\":true"));
	assert(has(&cap, "\"total\":3"));
	assert(has(&cap, "\"count\":3"));
	assert(has(&cap, "\"next\":-1"));
	assert(has(&cap, "\"prefix_fnv1a\":\""));
	/* "." and ".." never appear as rows -- the same filter the game's own
	 * Select World scan applies. */
	assert(!has(&cap, "\"name\":\".\""));
	assert(!has(&cap, "\"name\":\"..\""));

	/* Page 1 of 3. */
	build_card(&fs, 5);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"count\":2"));
	assert(has(&cap, "\"next\":2"));
	assert(has(&cap, "\"total\":5"));
	assert(has(&cap, "\"name\":\"New World 1\""));
	assert(has(&cap, "\"name\":\"New World 2\""));
	assert(!has(&cap, "\"name\":\"New World 3\""));

	/* Last page: next == -1. */
	assert(run_list(&fs, &cap, "CWSAVES1 saves 4 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"count\":1"));
	assert(has(&cap, "\"next\":-1"));
	assert(has(&cap, "\"name\":\"New World 5\""));

	/* Past the end: an empty, well-formed page, not an error. */
	assert(run_list(&fs, &cap, "CWSAVES1 saves 99 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"count\":0"));
	assert(has(&cap, "\"entries\":[]"));
	assert(has(&cap, "\"next\":-1"));

	/* A root that does not exist on the card fails cleanly, before any
	 * partial reply is emitted (so the caller can still send one FAIL). */
	fake_reset(&fs);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 8") == CW_SAVES_RC_FAIL);
	assert(cap.len == 0);
}

static void test_list_prefix_hash_detects_a_torn_directory(void) {
	struct fake_fs fs;
	struct capture cap;
	char tail_a[256];
	char tail_b[256];

	build_card(&fs, 4);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 2 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"total\":4"));
	snprintf(tail_a, sizeof(tail_a), "%s", strstr(cap.buf, "\"prefix_fnv1a\""));

	/* Unchanged directory -> byte-identical trailer, so the client's own
	 * recomputation over the pages it holds agrees. */
	assert(run_list(&fs, &cap, "CWSAVES1 saves 2 2") == CW_SAVES_RC_OK);
	snprintf(tail_b, sizeof(tail_b), "%s", strstr(cap.buf, "\"prefix_fnv1a\""));
	assert(strcmp(tail_a, tail_b) == 0);

	/* A world DISAPPEARS between pages (another session, or the game): the
	 * prefix hash over [0, offset+count) now covers a different set, and
	 * `total` moved too. Either check alone lets the client reject the whole
	 * inventory instead of executing against a torn list. */
	fake_drop_tree(&fs, "sd:/saves/New World 1");
	assert(run_list(&fs, &cap, "CWSAVES1 saves 2 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"total\":3"));
	snprintf(tail_b, sizeof(tail_b), "%s", strstr(cap.buf, "\"prefix_fnv1a\""));
	assert(strcmp(tail_a, tail_b) != 0);

	/* A world APPEARS between pages: `total` moves, which is the check that
	 * catches an append (an appended entry leaves the earlier prefix intact
	 * by construction, so the hash alone is NOT sufficient -- both checks are
	 * required, which is why the reply carries both). */
	build_card(&fs, 4);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"total\":4"));
	fake_add(&fs, "sd:/saves/zzz new world", 1, 0, 1757000100L, NULL);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 2 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"total\":5"));
}

static void test_list_reports_unsupported_names_untouched(void) {
	struct fake_fs fs;
	struct capture cap;
	static const unsigned char weird[] = { 'w', 0xe4, 'r', 'l', 'd' };
	static const unsigned char trailing_dot[] = { 'b', 'a', 'd', '.' };

	build_card(&fs, 1);
	fake_add_named(&fs, "sd:/saves", weird, sizeof(weird), 1);
	fake_add_named(&fs, "sd:/saves", trailing_dot, sizeof(trailing_dot), 1);

	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 32") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"total\":3"));
	/* Non-ASCII: hex present, readable name absent, but SUPPORTED -- opaque
	 * bytes are fine, they just cannot be printed. */
	assert(has(&cap, "\"name_hex\":\"77e4726c64\""));
	/* Trailing dot: FAT normalisation would eat it, so it is explicitly
	 * unsupported -- listed, reported, and never touched by a move. */
	assert(has(&cap, "\"name_hex\":\"6261642e\""));
	assert(has(&cap, "\"supported\":false"));
	assert(has(&cap, "\"unsupported\":1"));
	assert(fs.renames == 0 && fs.removes == 0);
}

static void test_list_summary_and_hash(void) {
	struct fake_fs fs;
	struct capture cap;

	fake_reset(&fs);
	fake_add(&fs, "sd:/saves", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/saves/cworld1", 1, 0, 1757000000L, NULL);
	fake_add(&fs, "sd:/saves/cworld1/level.dat", 0, 400, 1757000000L,
			 "aa11111111111111111111111111111111111111111111111111111111111111");
	fake_add(&fs, "sd:/saves/cworld1/level.dat_old", 0, 380, 1756999000L,
			 "bb22222222222222222222222222222222222222222222222222222222222222");
	fake_add(&fs, "sd:/saves/cworld1/region", 1, 0, 1757000500L, NULL);
	fake_add(&fs, "sd:/saves/cworld1/region/r.0.0.mcr", 0, 8192, 1757000500L,
			 "cc33333333333333333333333333333333333333333333333333333333333333");
	fake_add(&fs, "sd:/saves/cworld1/DIM-1", 1, 0, 1757000400L, NULL);
	fake_add(&fs, "sd:/saves/cworld1/DIM-1/region", 1, 0, 1757000400L, NULL);
	fake_add(&fs, "sd:/saves/cworld1/DIM-1/region/r.-1.0.mcr", 0, 4096,
			 1757000400L,
			 "dd44444444444444444444444444444444444444444444444444444444444444");
	fake_add(&fs, "sd:/saves/cworld1/data", 1, 0, 1757000300L, NULL);
	fake_add(&fs, "sd:/saves/cworld1/data/idcounts.dat", 0, 12, 1757000300L,
			 "ee55555555555555555555555555555555555555555555555555555555555555");

	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 32 summary")
		   == CW_SAVES_RC_OK);
	/* 5 regular files over the four KNOWN subdirs -- never an unbounded
	 * recursive walk (design S4). newest/oldest are over FILES only. */
	assert(has(&cap, "\"files\":5"));
	assert(has(&cap, "\"total_bytes\":13080"));
	assert(has(&cap, "\"newest\":1757000500"));
	assert(has(&cap, "\"oldest\":1756999000"));

	/* HASH: the server-side content check that needs NO new readable path. */
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 1 hash 63776f726c6431")
		   == CW_SAVES_RC_OK); /* cworld1 */
	assert(has(&cap, "\"reason\":\"saves_hash\""));
	assert(has(&cap, "\"rel_path\":\"level.dat\""));
	assert(has(&cap, "\"rel_path\":\"region/r.0.0.mcr\""));
	assert(has(&cap, "\"rel_path\":\"DIM-1/region/r.-1.0.mcr\""));
	assert(has(&cap, "\"rel_path\":\"data/idcounts.dat\""));
	assert(has(&cap,
			   "\"sha256\":\"cc3333333333333333333333333333333333333333333333"
			   "3333333333333333\""));
	assert(has(&cap, "\"count\":5"));
	/* Nothing was moved, created or removed by a read verb. */
	assert(fs.renames == 0 && fs.mkdirs == 0 && fs.removes == 0);

	/* A world that is not there fails before any output. */
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 1 hash 6e6f7065")
		   == CW_SAVES_RC_FAIL);
	assert(cap.len == 0);
}

static void test_list_emit_failure_is_an_abort_not_a_second_reply(void) {
	struct fake_fs fs;
	struct capture cap;
	struct cw_saves_list_req req;
	const char* reason = NULL;

	build_card(&fs, 4);
	capture_reset(&cap);
	cap.fail_after = 2; /* die right after the streamed prefix */
	assert(cw_saves_parse_list_header("CWSAVES1 saves 0 4", &req, &reason));
	/* Once ANY byte of a streamed reply is on the wire the stream cannot be
	 * resynchronised (the stage_send.h contract), so the driver reports ABORT
	 * and main.c closes the connection instead of appending a FAIL line. */
	assert(cw_saves_list_run(&fake_ops, &fs, &capture_ops, &cap, &req, &reason)
		   == CW_SAVES_RC_ABORT);
}

/* ------------------------------------------------------------------ */
/* 6. MOVE driver                                                      */
/* ------------------------------------------------------------------ */

static int run_move(struct fake_fs* fs, const char* hdr, const char** reason,
					char* from, size_t from_cap, char* to, size_t to_cap) {
	struct cw_saves_move_req req;
	*reason = NULL;
	if(!cw_saves_parse_move_header(hdr, &req, reason))
		return 0;
	return cw_saves_move_run(&fake_ops, fs, &req, from, from_cap, to, to_cap,
							 reason);
}

static void test_move_archive_and_restore(void) {
	struct fake_fs fs;
	const char* reason = NULL;
	char from[CW_SAVES_PATH_MAX];
	char to[CW_SAVES_PATH_MAX];

	build_card(&fs, 3);
	/* 4e657720576f726c642032 == "New World 2" */
	assert(run_move(&fs,
					"CWARCHIVE1 archive saves 20260921-151200 "
					"4e657720576f726c642032",
					&reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(from, "sd:/saves/New World 2") == 0);
	assert(strcmp(to, "sd:/cw_saves_archive/20260921-151200/New World 2") == 0);
	/* EXACTLY one rename, no delete, no copy -- the whole safety claim. */
	assert(fs.renames == 1);
	assert(fs.removes == 0);
	/* The fake ALLOWS a clobbering rename, so this is a statement about
	 * the server's own dest_exists check, not about the fake's kindness. */
	assert(fs.overwrites == 0);
	assert(fake_find(&fs, "sd:/saves/New World 2") == NULL);
	assert(fake_find(&fs, "sd:/cw_saves_archive/20260921-151200/New World 2")
		   != NULL);
	/* The world's CONTENTS came with it (a FAT directory rename re-parents
	 * the children; nothing was copied). */
	assert(fake_find(&fs, "sd:/cw_saves_archive/20260921-151200/New World 2/"
						  "level.dat")
		   != NULL);
	/* The archive root and the batch dir were created; nothing under saves. */
	assert(fake_find(&fs, "sd:/cw_saves_archive") != NULL);
	assert(fake_find(&fs, "sd:/cw_saves_archive/20260921-151200") != NULL);

	/* Restore puts it back into the root NAMED IN THE REQUEST (Astra 1) --
	 * never a re-resolved default. */
	assert(run_move(&fs,
					"CWARCHIVE1 restore saves 20260921-151200 "
					"4e657720576f726c642032",
					&reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(from, "sd:/cw_saves_archive/20260921-151200/New World 2")
		   == 0);
	assert(strcmp(to, "sd:/saves/New World 2") == 0);
	assert(fs.renames == 2);
	assert(fs.removes == 0);
	assert(fake_find(&fs, "sd:/saves/New World 2/level.dat") != NULL);
}

static void test_move_refusals(void) {
	struct fake_fs fs;
	const char* reason = NULL;
	char from[CW_SAVES_PATH_MAX];
	char to[CW_SAVES_PATH_MAX];

	/* Source missing. */
	build_card(&fs, 3);
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 6e6f7065", &reason, from,
					 sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "src_missing") == 0);
	assert(fs.renames == 0);

	/* Source is a FILE, not a world directory. */
	fake_add(&fs, "sd:/saves/notadir", 0, 10, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 6e6f7461646972", &reason,
					 from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "src_not_dir") == 0);
	assert(fs.renames == 0);

	/* Destination already exists -> refuse. NEVER remove(target) first, which
	 * is exactly what the PUT path does (main.c:1408) and must not happen
	 * here. */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/New World 2", 1, 0, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dest_exists") == 0);
	assert(fs.renames == 0);
	assert(fs.overwrites == 0);
	assert(fake_find(&fs, "sd:/saves/New World 2") != NULL); /* untouched */

	/* Never empty the saves root: the LAST world cannot be archived. */
	build_card(&fs, 1);
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642031",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "would_empty_saves") == 0);
	assert(fs.renames == 0);
	/* Two worlds -> the first may go. */
	build_card(&fs, 2);
	assert(run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642031",
					&reason, from, sizeof(from), to, sizeof(to)));

	/* Restore into a saves root that does not exist: refuse rather than
	 * create anything under saves. */
	fake_reset(&fs);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/w", 1, 0, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 restore saves b 77", &reason, from,
					 sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dst_root_missing") == 0);
	assert(fs.mkdirs == 0);

	/* Restore of a name a later "+new" run re-created: refused, never
	 * overwritten (design S11 risk 5). */
	fake_reset(&fs);
	fake_add(&fs, "sd:/saves", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/saves/w", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/w", 1, 0, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 restore saves b 77", &reason, from,
					 sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dest_exists") == 0);
	assert(fs.renames == 0);

	/* A restore whose archived source is missing. */
	fake_reset(&fs);
	fake_add(&fs, "sd:/saves", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/saves/keep", 1, 0, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 restore saves b 77", &reason, from,
					 sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "src_missing") == 0);

	/* A failed rename is reported, and nothing else is attempted to "fix"
	 * it -- no copy fallback, no delete. */
	build_card(&fs, 3);
	fs.fail_rename = 1;
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "rename_failed") == 0);
	assert(fs.removes == 0);
	assert(fake_find(&fs, "sd:/saves/New World 2") != NULL);

	/* A failed mkdir of the batch directory stops before the rename. */
	build_card(&fs, 3);
	fs.fail_mkdir = 1;
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "mkdir_failed") == 0);
	assert(fs.renames == 0);
}

static void test_move_case_insensitive_collision(void) {
	struct fake_fs fs;
	const char* reason = NULL;
	char from[CW_SAVES_PATH_MAX];
	char to[CW_SAVES_PATH_MAX];

	/* FAT cannot hold both "World" and "WORLD" in one directory, so a
	 * destination that differs only by case IS a collision and must be
	 * refused -- a case-SENSITIVE existence test (a bare stat on a
	 * case-preserving host) would have renamed straight onto it. */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/NEW WORLD 2", 1, 0, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dest_exists") == 0);
	assert(fs.renames == 0);
}

static void test_list_reports_a_failed_stat_as_such(void) {
	struct fake_fs fs;
	struct capture cap;

	/* M1: a stat that FAILED must not be wire-identical to a real
	 * zero-byte file. Before this, a failed stat emitted
	 * "is_dir":false,"bytes":0,"mtime":0 and the operator's np_rows
	 * cross-check silently under-counted against the game's own list. */
	build_card(&fs, 3);
	snprintf(fs.stat_fails, sizeof(fs.stat_fails), "%s",
			 "sd:/saves/New World 2");
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 32") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"stat_ok\":false"));
	assert(has(&cap, "\"stat_failed\":1"));
	assert(has(&cap, "\"total\":3"));
	/* The two healthy rows still carry their real values. */
	assert(has(&cap, "\"stat_ok\":true"));
	/* T9: nor is it a stat that said "not a directory". `st` is memset to 0
	 * before the call, so the row used to claim "is_dir":false -- a fact
	 * this code does not have. The client's directory filter then dropped
	 * the row before it ever looked at stat_ok, which made the guards that
	 * refuse such a row BY NAME unreachable on a real card. The type is
	 * reported as null when it is not known, and the field is always
	 * present so nothing has to infer it from a missing key. */
	assert(has(&cap, "\"is_dir\":null"));
	assert(has(&cap, "\"is_dir\":true"));
}

static void test_list_all_hash_detects_a_slot_reuse_past_the_page(void) {
	struct fake_fs fs;
	struct capture cap;
	char page0_all[64];
	char page1_all[64];
	const char* p;

	/* M2: prefix_fnv1a covers only [0, offset+count), so a DELETE paired
	 * with an INSERT at or after the current page boundary -- exactly what
	 * FAT does when a directory slot is freed and reused -- left every
	 * prefix matching and `total` unchanged, and the client accepted an
	 * inventory that had silently lost a world. The trailer now also
	 * carries all_fnv1a over EVERY entry of the same walk. */
	build_card(&fs, 4);
	fake_add(&fs, "sd:/saves/Keeper", 1, 0, 1757000009L, NULL);

	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 2") == CW_SAVES_RC_OK);
	p = strstr(cap.buf, "\"all_fnv1a\":\"");
	assert(p);
	snprintf(page0_all, sizeof(page0_all), "%.21s", p);

	/* Free "Keeper"'s slot and reuse it for a DIFFERENT world, entirely
	 * past page 0's window -- a delete paired with an insert, which is what
	 * FAT does when a directory slot is freed and reused. The entry count is
	 * unchanged, so `total` cannot see it, and page 0's delivered prefix is
	 * unchanged, so prefix_fnv1a cannot either. */
	fake_drop_tree(&fs, "sd:/saves/Keeper");
	fake_add(&fs, "sd:/saves/New World 9", 1, 0, 1757000010L, NULL);

	assert(run_list(&fs, &cap, "CWSAVES1 saves 2 2") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"total\":5")); /* total is UNCHANGED -- the trap */
	p = strstr(cap.buf, "\"all_fnv1a\":\"");
	assert(p);
	snprintf(page1_all, sizeof(page1_all), "%.21s", p);
	assert(strcmp(page0_all, page1_all) != 0);
}

static void test_collision_check_failure_is_not_a_clean_bill(void) {
	struct fake_fs fs;
	const char* reason = NULL;
	char from[CW_SAVES_PATH_MAX];
	char to[CW_SAVES_PATH_MAX];

	/* M3: opendir returns NULL for ANY failure, not only "absent", and
	 * libfat's open-directory table is finite. Reading that as "nothing can
	 * collide" silently removed the case-insensitive backstop. A directory
	 * that EXISTS but cannot be opened is now a refusal, not a pass. */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fs.fail_opendir = 1;
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "scan_failed") == 0
		   || strcmp(reason, "collision_check_failed") == 0);
	assert(fs.renames == 0 && fs.overwrites == 0);
}

static void test_move_resolves_an_83_alias_to_the_same_entry(void) {
	struct fake_fs fs;
	const char* reason = NULL;
	char from[CW_SAVES_PATH_MAX];
	char to[CW_SAVES_PATH_MAX];

	/* On FAT a long name and its 8.3 short name are ONE directory entry
	 * that stat() resolves under both spellings, while readdir returns only
	 * the long one. The validator accepts "NEWWOR~1" as a legal name, so a
	 * client may address a world by its alias -- and it must land on the
	 * same directory, with the same refusals. */
	build_card(&fs, 3);
	fake_set_alias(&fs, "sd:/saves/New World 2", "sd:/saves/NEWWOR~1");

	/* 4e4557574f527e31 == "NEWWOR~1" */
	assert(run_move(&fs, "CWARCHIVE1 archive saves b 4e4557574f527e31",
					&reason, from, sizeof(from), to, sizeof(to)));
	assert(fs.renames == 1 && fs.overwrites == 0 && fs.removes == 0);
	/* The entry the LONG name pointed at is the one that moved. */
	assert(fake_find(&fs, "sd:/saves/New World 2") == NULL);
	assert(fake_find(&fs, "sd:/cw_saves_archive/b/NEWWOR~1") != NULL);

	/* And the destination guard still sees an alias collision: archiving a
	 * second world under a name that ALIASES an existing archived entry is
	 * refused, not clobbered. */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/Something Else", 1, 0, 0, NULL);
	fake_set_alias(&fs, "sd:/cw_saves_archive/b/Something Else",
				   "sd:/cw_saves_archive/b/New World 2");
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dest_exists") == 0);
	assert(fs.renames == 0 && fs.overwrites == 0);
}

static void test_meta_read_error_is_not_a_short_success(void) {
	struct fake_fs fs;
	struct capture cap;
	static const unsigned char level[] = { 0x1f, 0x8b, 0x08, 0x00, 0x41, 0x42,
										   0x43, 0x44 };

	/* N5: the ops contract distinguishes "< 0 = read error" from "0 = EOF",
	 * and the whole M1 fix was "a failure is not an absence" -- but the meta
	 * path folded them into one `break`, so a failed read produced
	 * status:PASS, truncated:false and a `bytes` that quietly understated
	 * the file. The status line is already on the wire by then and cannot be
	 * retracted (the stage_send.h contract), so the TRAILER has to carry the
	 * error and the client has to honour it. */
	fake_reset(&fs);
	fake_add(&fs, "sd:/saves", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/saves/w", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/saves/w/level.dat", 0, 0, 0, NULL);
	fake_set_file(&fs, "sd:/saves/w/level.dat", level, sizeof(level));
	fake_set_read_error(&fs, "sd:/saves/w/level.dat", 4);

	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 1 meta 77") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"read_error\":true"));
	assert(has(&cap, "\"truncated\":false"));
	/* A clean read says so just as explicitly, so the field is never absent
	 * and the client never has to guess from a missing key. */
	fake_set_read_error(&fs, "sd:/saves/w/level.dat", 0);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 1 meta 77") == CW_SAVES_RC_OK);
	assert(has(&cap, "\"read_error\":false"));
	assert(has(&cap, "\"bytes\":8"));
}

/* N8: the pieces of main.c's saves wiring that are DECISIONS rather than
 * plumbing now live here, where a test can reach them -- which is also what
 * makes the review's mutations (d) "skip auth for RESTORE" and (e) "skip
 * auth for meta" expressible at all. main.c calls these; it no longer picks
 * a verb or an order by hand. */
static void test_dispatch_decisions(void) {
	/* Each mode maps to the verb whose auth rule it must obey. `meta` reads
	 * a world's level.dat -- name, seed, player position -- so it rides the
	 * same rule as LIST and HASH, and must NOT be silently weaker. */
	assert(cw_saves_verb_for_mode(CW_SAVES_LIST_PLAIN) == CW_SAVES_VERB_LIST);
	assert(cw_saves_verb_for_mode(CW_SAVES_LIST_SUMMARY) == CW_SAVES_VERB_LIST);
	assert(cw_saves_verb_for_mode(CW_SAVES_LIST_HASH) == CW_SAVES_VERB_HASH);
	assert(cw_saves_verb_for_mode(CW_SAVES_LIST_META) == CW_SAVES_VERB_META);

	/* meta is NOT reachable without a handshake, in any auth state. */
	assert(cw_saves_auth_check(CW_SAVES_VERB_META, 0, 0)
		   == CW_SAVES_AUTH_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_META, 0, 1)
		   == CW_SAVES_AUTH_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_META, 1, 0) == CW_SAVES_AUTH_OK);

	/* BOTH move directions carry the real-token requirement. The auth check
	 * runs before the header is parsed, so it cannot know the direction --
	 * which is exactly why a restore-specific weakening must be impossible
	 * by construction rather than by inspection. */
	assert(cw_saves_move_auth_verb(0) == CW_SAVES_VERB_MOVE);
	assert(cw_saves_move_auth_verb(1) == CW_SAVES_VERB_MOVE);
	assert(cw_saves_auth_check(cw_saves_move_auth_verb(1), 1, 0)
		   == CW_SAVES_AUTH_TOKEN_REQUIRED);

	/* The dispatch contract main.c implements: a saves verb is only ever
	 * reached with the connection's auth state already decided, and the
	 * auth verdict is computed before any parse. */
	assert(cw_saves_dispatch_is_saves_verb("CWSAVES1"));
	assert(cw_saves_dispatch_is_saves_verb("CWARCHIVE1"));
	assert(!cw_saves_dispatch_is_saves_verb("CWFETCH1"));
	assert(!cw_saves_dispatch_is_saves_verb("CWSTAGE4"));
	assert(!cw_saves_dispatch_is_saves_verb("CWHELLO1"));
	assert(!cw_saves_dispatch_is_saves_verb(""));
	assert(!cw_saves_dispatch_is_saves_verb(NULL));
}

/* N4: every destination scenario, run against BOTH fake lookups. With a
 * case-FOLDING lookup the plain stat(dst) catches a case-differing
 * collision and the backstop scan is redundant; with a case-SENSITIVE one
 * only the scan can catch it. Running just one of them leaves the other
 * guard with no test at all -- which is how "remove only the backstop"
 * survived the C10 suite. */
static void run_destination_guard_scenarios(int case_sensitive) {
	struct fake_fs fs;
	const char* reason = NULL;
	char from[CW_SAVES_PATH_MAX];
	char to[CW_SAVES_PATH_MAX];

	/* (1) a destination differing only by CASE */
	build_card(&fs, 3);
	fs.case_sensitive_lookup = case_sensitive;
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/NEW WORLD 2", 1, 0, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dest_exists") == 0);
	assert(fs.renames == 0 && fs.overwrites == 0);

	/* (2) a destination with the EXACT name */
	build_card(&fs, 3);
	fs.case_sensitive_lookup = case_sensitive;
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/New World 2", 1, 0, 0, NULL);
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dest_exists") == 0);
	assert(fs.renames == 0 && fs.overwrites == 0);

	/* (3) a destination reachable only through an 8.3 ALIAS */
	build_card(&fs, 3);
	fs.case_sensitive_lookup = case_sensitive;
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/Something Else", 1, 0, 0, NULL);
	fake_set_alias(&fs, "sd:/cw_saves_archive/b/Something Else",
				   "sd:/cw_saves_archive/b/New World 2");
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(strcmp(reason, "dest_exists") == 0);
	assert(fs.renames == 0 && fs.overwrites == 0);

	/* (4) a clear destination still moves, under both lookups */
	build_card(&fs, 3);
	fs.case_sensitive_lookup = case_sensitive;
	assert(run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					&reason, from, sizeof(from), to, sizeof(to)));
	assert(fs.renames == 1 && fs.overwrites == 0 && fs.removes == 0);
}

static void test_destination_guards_under_both_lookups(void) {
	run_destination_guard_scenarios(0); /* FAT-realistic: folds case */
	run_destination_guard_scenarios(1); /* the case libfat may actually be */
}

/* ================================================================== */
/* push-0921/C32 -- the TIER A fail-open defaults of the inventory.    */
/* Each one is a row of tmp/jobs/push-0921/inventory-C31-sol.md that   */
/* used to answer a question it could not answer, in the direction     */
/* that lets a MOVE proceed.                                           */
/* ================================================================== */

/* Row stage_saves.h:467-476. */
static void test_unknown_saves_verb_is_refused_before_auth(void) {
	enum cw_saves_verb bogus = (enum cw_saves_verb)99;
	/* Not "authed, so OK": a verb this build has no rule for has no rule,
	 * and the refusal does not depend on the connection state -- it is
	 * decided BEFORE conn_authed is looked at. */
	assert(cw_saves_auth_check(bogus, 1, 1) == CW_SAVES_AUTH_UNKNOWN_VERB);
	assert(cw_saves_auth_check(bogus, 1, 0) == CW_SAVES_AUTH_UNKNOWN_VERB);
	assert(cw_saves_auth_check(bogus, 0, 1) == CW_SAVES_AUTH_UNKNOWN_VERB);
	assert(strcmp(cw_saves_auth_reason(CW_SAVES_AUTH_UNKNOWN_VERB),
				  "unknown_verb") == 0);
	/* and every KNOWN verb keeps exactly the rule the truth table pins */
	assert(cw_saves_auth_check(CW_SAVES_VERB_LIST, 1, 0) == CW_SAVES_AUTH_OK);
	assert(cw_saves_auth_check(CW_SAVES_VERB_META, 1, 0) == CW_SAVES_AUTH_OK);
	assert(cw_saves_auth_check(CW_SAVES_VERB_HASH, 1, 0) == CW_SAVES_AUTH_OK);
	assert(cw_saves_auth_check(CW_SAVES_VERB_MOVE, 1, 0)
		   == CW_SAVES_AUTH_TOKEN_REQUIRED);
	assert(cw_saves_auth_check(CW_SAVES_VERB_LIST, 0, 1)
		   == CW_SAVES_AUTH_REQUIRED);
}

static void test_hash_file_failure_never_gets_a_pass_trailer(void) {
	struct fake_fs fs;
	struct capture cap;

	build_card(&fs, 1);
	fs.fail_hash_file = 1;
	assert(run_list(&fs, &cap,
			   "CWSAVES1 saves 0 1 hash 4e657720576f726c642031")
		   == CW_SAVES_RC_ABORT);
	assert(has(&cap, "\"status\":\"PASS\""));
	assert(!has(&cap, "],\"count\":"));
}

/* Row stage_saves.h:690-735,794-811. */
static void test_trailing_tokens_are_a_parse_error(void) {
	struct cw_saves_list_req lreq;
	struct cw_saves_move_req mreq;
	const char* reason = NULL;

	/* the exact same request, plus bytes nothing ever looks at */
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 10 plain junk", &lreq,
									   &reason));
	assert(strcmp(reason, "trailing_token") == 0);
	assert(!cw_saves_parse_list_header("CWSAVES1 saves 0 10 summary junk",
									   &lreq, &reason));
	assert(strcmp(reason, "trailing_token") == 0);
	assert(!cw_saves_parse_list_header(
		"CWSAVES1 saves 0 1 meta 4e657720576f726c642032 junk", &lreq, &reason));
	assert(strcmp(reason, "trailing_token") == 0);
	assert(!cw_saves_parse_move_header(
		"CWARCHIVE1 archive saves b 4e657720576f726c642032 junk", &mreq,
		&reason));
	assert(strcmp(reason, "trailing_token") == 0);

	/* ...and the well-formed lines still parse, trailing spaces included */
	assert(cw_saves_parse_list_header("CWSAVES1 saves 0 10", &lreq, &reason));
	assert(cw_saves_parse_list_header("CWSAVES1 saves 0 10 plain  ", &lreq,
									  &reason));
	assert(lreq.mode == CW_SAVES_LIST_PLAIN);
	assert(cw_saves_parse_list_header(
		"CWSAVES1 saves 0 1 meta 4e657720576f726c642032", &lreq, &reason));
	assert(cw_saves_parse_move_header(
		"CWARCHIVE1 archive saves b 4e657720576f726c642032 ", &mreq, &reason));
	assert(mreq.restore == 0);
}

/* Row stage_saves.h:1395-1418: the collision scan. */
static void test_collision_scan_truncated_is_not_a_clean_bill(void) {
	struct fake_fs fs;
	const char* reason = NULL;
	char from[CW_SAVES_PATH_MAX];
	char to[CW_SAVES_PATH_MAX];
	static const unsigned char want[] = "New World 2";

	/* (a) readdir FAILING part way through the batch directory */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/Something Else", 1, 0, 0, NULL);
	fs.readdir_fails_after = 0;
	assert(cw_saves__dir_holds_name(&fake_ops, &fs, "sd:/cw_saves_archive/b",
									want, sizeof(want) - 1) == -1);

	/* (b) closedir failing after a sweep that found nothing: the sweep
	 * itself is what cannot be trusted, so it is not an "absent" either */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fs.fail_closedir = 1;
	assert(cw_saves__dir_holds_name(&fake_ops, &fs, "sd:/cw_saves_archive/b",
									want, sizeof(want) - 1) == -1);

	/* (c) a PROVEN collision survives a failing close -- it is already
	 * proven, and downgrading it would refuse for the wrong reason */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b/New World 2", 1, 0, 0, NULL);
	fs.fail_closedir = 1;
	assert(cw_saves__dir_holds_name(&fake_ops, &fs, "sd:/cw_saves_archive/b",
									want, sizeof(want) - 1) == 1);

	/* and end to end: no rename is issued off a scan that did not finish */
	build_card(&fs, 3);
	fake_add(&fs, "sd:/cw_saves_archive", 1, 0, 0, NULL);
	fake_add(&fs, "sd:/cw_saves_archive/b", 1, 0, 0, NULL);
	fs.readdir_fails_after = 0;
	assert(!run_move(&fs, "CWARCHIVE1 archive saves b 4e657720576f726c642032",
					 &reason, from, sizeof(from), to, sizeof(to)));
	assert(fs.renames == 0 && fs.overwrites == 0 && fs.removes == 0);
}

/* Rows stage_saves.h:882-917 + 1327-1339: the SUMMARY walk. */
static void test_summary_walk_failure_is_reported_not_omitted(void) {
	struct fake_fs fs;
	struct capture cap;

	/* A world whose region/ directory cannot be STATTED. The walk used to
	 * skip such a subdirectory and the row went out with a byte total that
	 * silently excluded it -- which is the number the archive tool checks
	 * against its 262144-byte "rig leftover" bound before a MOVE. */
	build_card(&fs, 1);
	fake_add(&fs, "sd:/saves/New World 1/region", 1, 0, 1757000000L, NULL);
	fake_add(&fs, "sd:/saves/New World 1/region/r.0.0.mcr", 0, 9000000,
			 1757000000L, NULL);
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 10 summary")
		   == CW_SAVES_RC_OK);
	assert(has(&cap, "\"total_bytes\":9000400"));

	snprintf(fs.stat_fails, sizeof(fs.stat_fails), "%s",
			 "sd:/saves/New World 1/region");
	assert(run_list(&fs, &cap, "CWSAVES1 saves 0 10 summary")
		   == CW_SAVES_RC_OK);
	assert(!has(&cap, "\"total_bytes\":400"));
	assert(has(&cap, "\"total_bytes\":null"));
	assert(has(&cap, "\"files\":null"));
	assert(has(&cap, "\"summary_error\":\"read_failed\""));

	assert(has(&cap, "\"summary_errno\""));
	/* the row is still DELIVERED -- one unreadable world does not reject
	   the whole 646-row listing -- it just cannot claim a size */
	assert(has(&cap, "\"stat_ok\":true"));
}

/* Row main.c:289-305: the request-line reader. */
struct fake_line_src {
	const char* bytes;
	size_t pos;
	size_t len;
};

static int fake_line_recv(void* user, char* c) {
	struct fake_line_src* s = (struct fake_line_src*)user;
	if(s->pos >= s->len)
		return 0;
	*c = s->bytes[s->pos++];
	return 1;
}

static void test_an_overlong_request_line_is_refused_and_drained(void) {
	struct fake_line_src src;
	char dst[16];
	int r;

	/* An ordinary line still reads, \r stripped, and the reader stops at
	 * the newline with the NEXT line untouched. */
	src.bytes = "CWHELLO1\r\nCWSAVES1 saves 0 1\n";
	src.pos = 0;
	src.len = strlen(src.bytes);
	r = cw_stage_recv_line(fake_line_recv, &src, dst, sizeof(dst));
	assert(r == 8 && strcmp(dst, "CWHELLO1") == 0);

	/* The fail-open: a line longer than the buffer used to come back as a
	 * VALID request holding its truncated prefix, and its tail was then
	 * read as the next request -- one line parsed as two, the second of
	 * them entirely attacker-chosen. */
	src.bytes = "CWARCHIVE1 archive saves b 4e65\nCWHELLO1\n";
	src.pos = 0;
	src.len = strlen(src.bytes);
	r = cw_stage_recv_line(fake_line_recv, &src, dst, sizeof(dst));
	assert(r == CW_STAGE_LINE_TOO_LONG);
	/* ...and the refused line was DRAINED, so the stream is still framed:
	 * the very next read is the next line, not the tail of the last one. */
	r = cw_stage_recv_line(fake_line_recv, &src, dst, sizeof(dst));
	assert(r == 8 && strcmp(dst, "CWHELLO1") == 0);

	/* closed before a byte vs closed mid-line stay distinguishable */
	src.bytes = "";
	src.pos = 0;
	src.len = 0;
	assert(cw_stage_recv_line(fake_line_recv, &src, dst, sizeof(dst))
		   == CW_STAGE_LINE_CLOSED);
	src.bytes = "CWHELL";
	src.pos = 0;
	src.len = 6;
	assert(cw_stage_recv_line(fake_line_recv, &src, dst, sizeof(dst))
		   == CW_STAGE_LINE_TRUNCATED);
}

/* Row main.c:485-502: the token loader. */
static void test_token_read_failure_locks_a_baked_build(void) {
	const size_t cap = CW_STAGE_TOKEN_MAX_BYTES_FOR_TEST;
	/* ferror was the ONLY one of the three the loader passed on. */
	assert(cw_stage_token_read_failed(1, 0, 0));
	assert(cw_stage_token_read_failed(0, 1, 0)); /* fclose failed */
	assert(cw_stage_token_read_failed(0, 0, 1)); /* short read */
	assert(!cw_stage_token_read_failed(0, 0, 0));
	/* ...and each of them LOCKS a baked build rather than soft-launching
	 * on whatever bytes happened to be in the buffer. */
	assert(cw_stage_token_state(1, 0, 0, cw_stage_token_read_failed(0, 1, 0),
								32, cap) == CW_STAGE_TOKEN_LOCKED);
	assert(cw_stage_token_state(1, 0, 0, cw_stage_token_read_failed(0, 0, 1),
								32, cap) == CW_STAGE_TOKEN_LOCKED);
	assert(cw_stage_token_state(1, 0, 0, cw_stage_token_read_failed(0, 0, 0),
								32, cap) == CW_STAGE_TOKEN_PROVISIONED);
	/* a non-baked staging build is still allowed its soft launch */
	assert(cw_stage_token_state(0, 0, 0, cw_stage_token_read_failed(0, 1, 0),
								32, cap) == CW_STAGE_TOKEN_SOFT_LAUNCH);
}

int main(void) {
	/* The Wii loader only attempts the baked write after fopen's ENOENT.
	 * An existing empty file opens successfully and remains untouched. */
	assert(cw_stage_token_should_bootstrap(1, ENOENT));
	assert(!cw_stage_token_should_bootstrap(1, EIO));
	assert(!cw_stage_token_should_bootstrap(0, 0));
	test_root_stat_error_is_not_missing();
	test_readdir_error_is_not_end_of_list();
	test_closedir_error_is_not_end_of_list();
	test_stale_token_temp_reprovisions_only_without_final();
	test_stage_token_state_table();
	test_name_validator();
	test_name_case_insensitive_equality();
	test_batch_ids();
	test_root_enum();
	test_hex_transport();
	test_fnv1a_matches_the_published_vectors();
	test_meta_mode();
	test_auth_truth_table();
	test_header_parse();
	test_list_basic_and_paging();
	test_list_prefix_hash_detects_a_torn_directory();
	test_list_reports_unsupported_names_untouched();
	test_list_summary_and_hash();
	test_hash_file_failure_never_gets_a_pass_trailer();
	test_list_emit_failure_is_an_abort_not_a_second_reply();
	test_move_archive_and_restore();
	test_move_refusals();
	test_move_case_insensitive_collision();
	test_list_reports_a_failed_stat_as_such();
	test_list_all_hash_detects_a_slot_reuse_past_the_page();
	test_collision_check_failure_is_not_a_clean_bill();
	test_move_resolves_an_83_alias_to_the_same_entry();
	test_meta_read_error_is_not_a_short_success();
	test_dispatch_decisions();
	test_destination_guards_under_both_lookups();
	test_unknown_saves_verb_is_refused_before_auth();
	test_trailing_tokens_are_a_parse_error();
	test_collision_scan_truncated_is_not_a_clean_bill();
	test_summary_walk_failure_is_reported_not_omitted();
	test_an_overlong_request_line_is_refused_and_drained();
	test_token_read_failure_locks_a_baked_build();
	printf("PASS test_stage_saves_validator\n");
	return 0;
}
