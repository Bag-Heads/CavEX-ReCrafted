/* hw1028a/hw1028b: CWFETCH1's path is the rest of the request line, so a
 * real backup folder "<world> (backup)" is fetchable; it stays bounded and
 * refuses '..' and control bytes. Drives the SAME header main.c compiles
 * (tools/wii_stage_server/source/stage_fetch_path.h), then the unchanged
 * valid_sd_path() allow-list the server applies next. */
#include "stage_auth.h"
#include "stage_delete.h"
#include "stage_fetch_path.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int parse(const char* line, char* out, size_t cap) {
	return cw_stage_fetch_path(line, out, cap);
}

/* spaced-paths 2026-10-30: the CWSTAGE4 put header. The old
 * sscanf("%15s %lu %64s %255s") turned the Tutorial world's level.dat into
 * a stray FILE ".../saves/CavEX". main.c now takes the fixed fields by
 * sscanf and the path by cw_stage_tail_path(header, 3, ...). */
static void test_put_paths(void) {
	const char* sha =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char line[512], out[256];
	snprintf(line, sizeof line,
			 "CWSTAGE4R 1234 %s sd:/apps/CavEX/saves/CavEX Tutorial/"
			 "level.dat",
			 sha);
	{
		char proto[16], cut[256], s[65];
		unsigned long size;
		assert(sscanf(line, "%15s %lu %64s %255s", proto, &size, s, cut) == 4);
		assert(strcmp(cut, "sd:/apps/CavEX/saves/CavEX") == 0
			   && "documents the stray-file truncation");
		assert(sscanf(line, "%15s %lu %64s", proto, &size, s) == 3);
		assert(size == 1234 && strcmp(s, sha) == 0);
	}
	assert(cw_stage_tail_path(line, 3, out, sizeof out) == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/CavEX/saves/CavEX Tutorial/level.dat")
		   == 0);
	assert(valid_sd_path(out));

	snprintf(line, sizeof line,
			 "CWSTAGE4 9 %s sd:/texturepacks/isabella-ii/About Isabella.txt",
			 sha);
	assert(cw_stage_tail_path(line, 3, out, sizeof out) == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/texturepacks/isabella-ii/About Isabella.txt") == 0);

	/* LICENSES/ lives under the app dir on the card */
	snprintf(line, sizeof line,
			 "CWSTAGE4 9 %s \"sd:/apps/CavEX/LICENSES/Isabella-II/"
			 "About Isabella.txt\"",
			 sha);
	assert(cw_stage_tail_path(line, 3, out, sizeof out) == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/CavEX/LICENSES/Isabella-II/"
					   "About Isabella.txt") == 0);

	/* Old no-space header: byte-identical result to the old parse. */
	snprintf(line, sizeof line, "CWSTAGE4R 5 %s sd:/apps/CavEX/boot.dol",
			 sha);
	assert(cw_stage_tail_path(line, 3, out, sizeof out) == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/CavEX/boot.dol") == 0);
	/* CWSTAGE1/2 shape: path after 2 tokens */
	assert(cw_stage_tail_path("CWSTAGE2 5 sd:/apps/x y/z", 2, out, sizeof out)
		   == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/x y/z") == 0);

	/* Missing path, '..', control bytes: refused, never cut. */
	snprintf(line, sizeof line, "CWSTAGE4 5 %s", sha);
	assert(cw_stage_tail_path(line, 3, out, sizeof out)
		   == CW_FETCH_PATH_MISSING);
	snprintf(line, sizeof line, "CWSTAGE4 5 %s   ", sha);
	assert(cw_stage_tail_path(line, 3, out, sizeof out)
		   == CW_FETCH_PATH_MISSING);
	snprintf(line, sizeof line, "CWSTAGE4 5 %s sd:/apps/a b/../c", sha);
	assert(cw_stage_tail_path(line, 3, out, sizeof out)
		   == CW_FETCH_PATH_DOTDOT);
	assert(out[0] == '\0');
	assert(cw_stage_tail_path("CWSTAGE4", 3, out, sizeof out)
		   == CW_FETCH_PATH_MISSING);
	/* The allow-list is unchanged after a space-safe parse. */
	snprintf(line, sizeof line, "CWSTAGE4 5 %s sd:/private dir/x", sha);
	assert(cw_stage_tail_path(line, 3, out, sizeof out) == CW_FETCH_PATH_OK);
	assert(!valid_sd_path(out));
}

static void test_run_paths(void) {
	char a[256], b[256];
	int have = 7;
	assert(cw_stage_two_paths("CWRUN1", a, sizeof a, b, sizeof b, &have)
		   == CW_FETCH_PATH_OK && have == 0);
	assert(cw_stage_two_paths("CWRUNPREP1   ", a, sizeof a, b, sizeof b,
							  &have) == CW_FETCH_PATH_OK && have == 0);
	assert(cw_stage_two_paths("CWRUNPREP1 sd:/apps/CavEX/boot.dol "
							  "sd:/apps/CavEX/cw_run_args.txt",
							  a, sizeof a, b, sizeof b, &have)
		   == CW_FETCH_PATH_OK && have == 1);
	assert(strcmp(a, "sd:/apps/CavEX/boot.dol") == 0);
	assert(strcmp(b, "sd:/apps/CavEX/cw_run_args.txt") == 0);
	assert(cw_stage_two_paths("CWRUN1 \"sd:/apps/My App/boot.dol\" "
							  "sd:/apps/My App/args.txt",
							  a, sizeof a, b, sizeof b, &have)
		   == CW_FETCH_PATH_OK && have == 1);
	assert(strcmp(a, "sd:/apps/My App/boot.dol") == 0);
	assert(strcmp(b, "sd:/apps/My App/args.txt") == 0);
	/* one path alone was bad_header before and still is */
	assert(cw_stage_two_paths("CWRUN1 sd:/apps/x/boot.dol", a, sizeof a, b,
							  sizeof b, &have) == CW_FETCH_PATH_MISSING);
	assert(have == 0 && a[0] == '\0');
	assert(cw_stage_two_paths("CWRUN1 \"sd:/apps/x\" \"sd:/apps/../y\"", a,
							  sizeof a, b, sizeof b, &have)
		   == CW_FETCH_PATH_DOTDOT);
}

static void test_delete_policy(void) {
	/* allowed */
	assert(cw_delete_target_ok("sd:/apps/CavEX/saves/CaveWii",
							   CW_DELETE_MODE_FILE));
	assert(cw_delete_target_ok("sd:/apps/CavEX/saves/CavEX Tutorial",
							   CW_DELETE_MODE_TREE));
	assert(cw_delete_target_ok("sd:/apps/CavEX/hang_report_3.txt",
							   CW_DELETE_MODE_FILE));
	assert(cw_delete_target_ok("sd:/apps/CavEX/LICENSES",
							   CW_DELETE_MODE_DIR));
	assert(cw_delete_target_ok("sd:/texturepacks/isabella-ii/About Isabella.txt",
							   CW_DELETE_MODE_FILE));
	assert(cw_delete_target_ok("sd:/texturepacks/old-pack",
							   CW_DELETE_MODE_TREE));
	assert(cw_delete_target_ok("sd:/assets/art/kz.png", CW_DELETE_MODE_FILE));
	assert(cw_delete_target_ok("sd:/cw_boot_trace.txt", CW_DELETE_MODE_FILE));
	assert(cw_delete_target_ok("sd:/apps/CavEX/sub/boot.dol",
							   CW_DELETE_MODE_FILE));

	/* refused: outside the allow-list */
	assert(!cw_delete_target_ok("sd:/private/x", CW_DELETE_MODE_FILE));
	assert(!cw_delete_target_ok("sd:/apps/../x/y", CW_DELETE_MODE_FILE));
	assert(!cw_delete_target_ok("sd:/apps//x/y", CW_DELETE_MODE_FILE));
	assert(!cw_delete_target_ok("usb:/apps/x/y", CW_DELETE_MODE_FILE));
	assert(!cw_delete_target_ok("sd:/", CW_DELETE_MODE_TREE));
	/* refused: the token, boot.dol, the stage server's own app */
	assert(!cw_delete_target_ok("sd:/apps/CavEX/stage_token.txt",
								CW_DELETE_MODE_FILE));
	assert(!cw_delete_target_ok("SD:/APPS/cavewii_v2/STAGE_TOKEN.TXT",
								CW_DELETE_MODE_FILE));
	assert(!cw_delete_target_ok("sd:/apps/CavEX/boot.dol",
								CW_DELETE_MODE_FILE));
	assert(!cw_delete_target_ok("sd:/apps/CaveWiiStage/x.txt",
								CW_DELETE_MODE_FILE));
	/* refused: the floors */
	assert(!cw_delete_target_ok("sd:/apps/CavEX", CW_DELETE_MODE_TREE));
	assert(!cw_delete_target_ok("sd:/apps/CavEX", CW_DELETE_MODE_DIR));
	assert(!cw_delete_target_ok("sd:/apps/CavEX/saves",
								CW_DELETE_MODE_TREE));
	assert(cw_delete_target_ok("sd:/apps/CavEX/saves",
							   CW_DELETE_MODE_DIR)
		   && "an EMPTY saves dir may go, never a recursive one");
	assert(!cw_delete_target_ok("sd:/texturepacks", CW_DELETE_MODE_TREE));
	assert(cw_delete_target_ok("sd:/beta/resources", CW_DELETE_MODE_TREE));
	assert(!cw_delete_target_ok("sd:/beta", CW_DELETE_MODE_TREE));
	assert(!cw_delete_target_ok("sd:/beta/", CW_DELETE_MODE_DIR));
	assert(!cw_delete_target_ok("sd:/texturepacks/", CW_DELETE_MODE_DIR));
	assert(!cw_delete_target_ok("sd:/assets/", CW_DELETE_MODE_TREE));
	assert(!cw_delete_target_ok("sd:/cw_boot_trace.txt", CW_DELETE_MODE_TREE));
	assert(!cw_delete_target_ok("sd:/apps/x/y", CW_DELETE_MODE_BAD));

	assert(cw_delete_parse_mode("tree", 4) == CW_DELETE_MODE_TREE);
	assert(cw_delete_parse_mode("trees", 5) == CW_DELETE_MODE_BAD);
	assert(cw_delete_parse_mode("FILE", 4) == CW_DELETE_MODE_BAD);
}

/* In-memory card for the walker. */
#define FAKE_MAX 64
static struct {
	char path[CW_DELETE_PATH_MAX];
	int is_dir;
	int alive;
} g_fake[FAKE_MAX];
static int g_fake_n;
static int g_fail_remove_of; /* index whose removal fails, or -1 */

static void fake_reset(void) {
	g_fake_n = 0;
	g_fail_remove_of = -1;
}
static void fake_add(const char* p, int is_dir) {
	assert(g_fake_n < FAKE_MAX);
	snprintf(g_fake[g_fake_n].path, CW_DELETE_PATH_MAX, "%s", p);
	g_fake[g_fake_n].is_dir = is_dir;
	g_fake[g_fake_n].alive = 1;
	g_fake_n++;
}
static int fake_find(const char* p) {
	for(int i = 0; i < g_fake_n; i++)
		if(g_fake[i].alive && strcmp(g_fake[i].path, p) == 0)
			return i;
	return -1;
}
static int fake_alive(const char* p) { return fake_find(p) >= 0; }
static int fake_stat(void* u, const char* p, int* ex, int* dir) {
	(void)u;
	int i = fake_find(p);
	*ex = i >= 0;
	*dir = i >= 0 && g_fake[i].is_dir;
	return 1;
}
struct fake_dir {
	char path[CW_DELETE_PATH_MAX];
	int idx;
	int dots;
};
static struct fake_dir g_dir;
static void* fake_opendir(void* u, const char* p) {
	(void)u;
	int i = fake_find(p);
	if(i < 0 || !g_fake[i].is_dir)
		return NULL;
	snprintf(g_dir.path, sizeof g_dir.path, "%s", p);
	g_dir.idx = 0;
	g_dir.dots = 0;
	return &g_dir;
}
static int fake_readdir(void* u, void* d, char* name, size_t cap) {
	(void)u;
	struct fake_dir* fd = d;
	if(fd->dots < 2) { /* libfat-style "." and ".." first */
		snprintf(name, cap, "%s", fd->dots == 0 ? "." : "..");
		fd->dots++;
		return 1;
	}
	size_t n = strlen(fd->path);
	for(; fd->idx < g_fake_n; fd->idx++) {
		const char* c = g_fake[fd->idx].path;
		if(!g_fake[fd->idx].alive || strncmp(c, fd->path, n) != 0
		   || c[n] != '/' || strchr(c + n + 1, '/'))
			continue;
		snprintf(name, cap, "%s", c + n + 1);
		fd->idx++;
		return 1;
	}
	return 0;
}
static void fake_closedir(void* u, void* d) {
	(void)u;
	(void)d;
}
static int fake_rm(const char* p, int want_dir) {
	int i = fake_find(p);
	if(i < 0 || g_fake[i].is_dir != want_dir || i == g_fail_remove_of)
		return 0;
	if(want_dir) {
		size_t n = strlen(p);
		for(int j = 0; j < g_fake_n; j++)
			if(g_fake[j].alive && strncmp(g_fake[j].path, p, n) == 0
			   && g_fake[j].path[n] == '/')
				return 0; /* not empty */
	}
	g_fake[i].alive = 0;
	return 1;
}
static int fake_rm_file(void* u, const char* p) {
	(void)u;
	return fake_rm(p, 0);
}
static int fake_rm_dir(void* u, const char* p) {
	(void)u;
	return fake_rm(p, 1);
}
static const struct cw_delete_fs_ops g_fs = {
	fake_stat,	  fake_opendir, fake_readdir, fake_closedir,
	fake_rm_file, fake_rm_dir,	NULL};

static int del(const char* path, int mode, unsigned* removed) {
	char t[256];
	snprintf(t, sizeof t, "%s", path);
	return cw_stage_delete(&g_fs, t, mode, removed);
}

static void test_delete_walk(void) {
	unsigned removed = 99;
	const char* S = "sd:/apps/CavEX/saves";
	char p[CW_DELETE_PATH_MAX];
	fake_reset();
	fake_add("sd:/apps", 1);
	fake_add("sd:/apps/CavEX", 1);
	fake_add("sd:/apps/CavEX/stage_token.txt", 0);
	fake_add(S, 1);
	fake_add("sd:/apps/CavEX/saves/CaveWii", 0); /* the stray file */
	fake_add("sd:/apps/CavEX/saves/CavEX Tutorial", 1);
	fake_add("sd:/apps/CavEX/saves/CavEX Tutorial/level.dat", 0);
	fake_add("sd:/apps/CavEX/saves/CavEX Tutorial/region", 1);
	fake_add("sd:/apps/CavEX/saves/CavEX Tutorial/region/r.0.0.mcr", 0);
	fake_add("sd:/apps/CavEX/saves/CavEX Tutorial/DIM-1", 1);
	fake_add("sd:/apps/CavEX/saves/New World 1", 1);
	fake_add("sd:/apps/CavEX/saves/New World 1/level.dat", 0);
	fake_add("sd:/apps/CavEX/empty", 1);

	/* the stray file: file mode on a dir / dir mode on a file are refused */
	assert(del("sd:/apps/CavEX/saves/CavEX Tutorial",
			   CW_DELETE_MODE_FILE, &removed) == CW_DELETE_NOT_FILE);
	assert(del("sd:/apps/CavEX/saves/CaveWii", CW_DELETE_MODE_DIR,
			   &removed) == CW_DELETE_NOT_DIR);
	assert(del("sd:/apps/CavEX/saves/CaveWii", CW_DELETE_MODE_FILE,
			   &removed) == CW_DELETE_OK && removed == 1);
	assert(!fake_alive("sd:/apps/CavEX/saves/CaveWii"));
	assert(del("sd:/apps/CavEX/saves/CaveWii", CW_DELETE_MODE_FILE,
			   &removed) == CW_DELETE_ABSENT && removed == 0);

	/* dir mode: empty only */
	assert(del("sd:/apps/CavEX/saves/CavEX Tutorial", CW_DELETE_MODE_DIR,
			   &removed) == CW_DELETE_NOT_EMPTY);
	assert(fake_alive("sd:/apps/CavEX/saves/CavEX Tutorial/level.dat"));
	assert(del("sd:/apps/CavEX/empty/", CW_DELETE_MODE_DIR, &removed)
		   == CW_DELETE_OK && removed == 1);

	/* tree: removes exactly that world, siblings untouched */
	assert(del("sd:/apps/CavEX/saves/CavEX Tutorial/",
			   CW_DELETE_MODE_TREE, &removed) == CW_DELETE_OK);
	assert(removed == 5);
	for(int i = 0; i < g_fake_n; i++)
		if(strstr(g_fake[i].path, "CavEX Tutorial"))
			assert(!g_fake[i].alive);
	assert(fake_alive("sd:/apps/CavEX/saves/New World 1/level.dat"));
	assert(fake_alive(S));
	assert(fake_alive("sd:/apps/CavEX/stage_token.txt"));

	/* refused targets never touch the fake card */
	assert(del(S, CW_DELETE_MODE_TREE, &removed) == CW_DELETE_REFUSED);
	assert(del("sd:/apps/CavEX", CW_DELETE_MODE_TREE, &removed)
		   == CW_DELETE_REFUSED);
	assert(del("sd:/apps/CavEX/stage_token.txt", CW_DELETE_MODE_FILE,
			   &removed) == CW_DELETE_REFUSED);
	assert(fake_alive("sd:/apps/CavEX/saves/New World 1/level.dat"));
	assert(strcmp(cw_delete_reason(CW_DELETE_REFUSED), "bad_target") == 0);

	/* an I/O failure mid-walk is a FAIL with the partial count */
	g_fail_remove_of = fake_find("sd:/apps/CavEX/saves/New World 1/level.dat");
	assert(del("sd:/apps/CavEX/saves/New World 1", CW_DELETE_MODE_TREE,
			   &removed) == CW_DELETE_IO && removed == 0);
	assert(fake_alive("sd:/apps/CavEX/saves/New World 1"));
	g_fail_remove_of = -1;

	/* depth bound: refuses rather than recursing without limit */
	fake_reset();
	snprintf(p, sizeof p, "sd:/apps/CavEX/saves/deep");
	fake_add(p, 1);
	for(int d = 0; d < CW_DELETE_MAX_DEPTH + 2; d++) {
		strcat(p, "/d");
		fake_add(p, 1);
	}
	assert(del("sd:/apps/CavEX/saves/deep", CW_DELETE_MODE_TREE, &removed)
		   == CW_DELETE_TOO_DEEP);
	assert(fake_alive("sd:/apps/CavEX/saves/deep"));
}

static void test_app_folder_migration(void) {
	const char* apps[] = {"CaveWii_v2", "CavEX"};
	char path[128], line[160], out[128];
	for(size_t i = 0; i < sizeof apps / sizeof apps[0]; ++i) {
		snprintf(path, sizeof path, "sd:/apps/%s/saves/New World/level.dat", apps[i]);
		snprintf(line, sizeof line, "CWFETCH1 %s", path);
		assert(parse(line, out, sizeof out) == CW_FETCH_PATH_OK);
		assert(strcmp(out, path) == 0 && valid_sd_path(out));
		assert(cw_delete_target_ok(path, CW_DELETE_MODE_FILE));
		snprintf(path, sizeof path, "sd:/apps/%s/saves/New World", apps[i]);
		assert(cw_delete_target_ok(path, CW_DELETE_MODE_TREE));
		snprintf(path, sizeof path, "sd:/apps/%s/boot.dol", apps[i]);
		assert(!cw_delete_target_ok(path, CW_DELETE_MODE_FILE));
		snprintf(path, sizeof path, "sd:/apps/%s/stage_token.txt", apps[i]);
		assert(!cw_delete_target_ok(path, CW_DELETE_MODE_FILE));
		snprintf(path, sizeof path, "sd:/apps/%s", apps[i]);
		assert(!cw_delete_target_ok(path, CW_DELETE_MODE_DIR));
		assert(!cw_delete_target_ok(path, CW_DELETE_MODE_TREE));
		snprintf(path, sizeof path, "sd:/apps/%s/saves", apps[i]);
		assert(!cw_delete_target_ok(path, CW_DELETE_MODE_TREE));
	}
}

int main(void) {
	test_app_folder_migration();
	char out[256];

	/* The hw1028a failure: the old sscanf("%15s %255s") cut here. */
	const char* bk = "CWFETCH1 sd:/apps/CavEX/saves/hw1028a_x/"
					 "cw_wcc_fixture (backup)/level.dat";
	{
		char proto[16], cut[256];
		assert(sscanf(bk, "%15s %255s", proto, cut) == 2);
		assert(strcmp(cut, "sd:/apps/CavEX/saves/hw1028a_x/"
						   "cw_wcc_fixture") == 0
			   && "documents the old truncation");
	}
	assert(parse(bk, out, sizeof out) == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/CavEX/saves/hw1028a_x/"
					   "cw_wcc_fixture (backup)/level.dat") == 0);
	assert(valid_sd_path(out));

	/* "(backup N)", region subdirs and a space-only DIM name all survive */
	assert(parse("CWFETCH1 sd:/apps/CavEX/saves/New World 152 (backup 2)"
				 "/DIM-1/region/r.0.0.mcr", out, sizeof out) == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/CavEX/saves/New World 152 (backup 2)"
					   "/DIM-1/region/r.0.0.mcr") == 0);

	/* No-space paths parse exactly as before. */
	assert(parse("CWFETCH1 sd:/cw_boot_trace.txt", out, sizeof out)
		   == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/cw_boot_trace.txt") == 0);

	/* Separator blanks (spaces/tabs) and trailing blanks are not the path;
	 * an inner run of spaces is. */
	assert(parse("  CWFETCH1 \t sd:/apps/a  b/c.txt \t ", out, sizeof out)
		   == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/a  b/c.txt") == 0);

	/* Missing path / missing verb. */
	assert(parse("CWFETCH1", out, sizeof out) == CW_FETCH_PATH_MISSING);
	assert(parse("CWFETCH1   ", out, sizeof out) == CW_FETCH_PATH_MISSING);
	assert(parse("", out, sizeof out) == CW_FETCH_PATH_MISSING);
	assert(parse(NULL, out, sizeof out) == CW_FETCH_PATH_MISSING);
	assert(out[0] == '\0');

	/* '..' anywhere is refused here, before valid_sd_path. */
	assert(parse("CWFETCH1 sd:/apps/../stage_token.txt", out, sizeof out)
		   == CW_FETCH_PATH_DOTDOT);
	assert(parse("CWFETCH1 sd:/apps/x (backup)/..", out, sizeof out)
		   == CW_FETCH_PATH_DOTDOT);
	assert(parse("CWFETCH1 sd:/apps/a..b", out, sizeof out)
		   == CW_FETCH_PATH_DOTDOT);
	assert(out[0] == '\0' && "a refused path never reaches the caller");
	/* a single dot (r.0.0.mcr, level.dat_old) is fine */
	assert(parse("CWFETCH1 sd:/apps/w/level.dat_old", out, sizeof out)
		   == CW_FETCH_PATH_OK);

	/* Control bytes (an embedded NUL ends the C string, so test the rest). */
	assert(parse("CWFETCH1 sd:/apps/a\tb", out, sizeof out)
		   == CW_FETCH_PATH_CONTROL);
	assert(parse("CWFETCH1 sd:/apps/a\x01" "b", out, sizeof out)
		   == CW_FETCH_PATH_CONTROL);
	assert(parse("CWFETCH1 sd:/apps/a\x7f" "b", out, sizeof out)
		   == CW_FETCH_PATH_CONTROL);

	/* Bounded: 255 bytes fit target[256], 256 are refused, never cut. */
	{
		char line[600], want[300];
		strcpy(want, "sd:/apps/");
		while(strlen(want) < 255)
			strcat(want, "x");
		snprintf(line, sizeof line, "CWFETCH1 %s", want);
		assert(parse(line, out, sizeof out) == CW_FETCH_PATH_OK);
		assert(strlen(out) == 255 && strcmp(out, want) == 0);
		strcat(want, "y");
		snprintf(line, sizeof line, "CWFETCH1 %s", want);
		assert(parse(line, out, sizeof out) == CW_FETCH_PATH_TOO_LONG);
		assert(out[0] == '\0');
		char tiny[4];
		assert(parse("CWFETCH1 sd:/", tiny, sizeof tiny)
			   == CW_FETCH_PATH_TOO_LONG);
		assert(parse("CWFETCH1 sd:", tiny, sizeof tiny) == CW_FETCH_PATH_OK);
		assert(parse("CWFETCH1 x", tiny, 0) == CW_FETCH_PATH_TOO_LONG);
	}

	/* The allow-list still applies after a space-safe parse. */
	assert(parse("CWFETCH1 sd:/secret dir/x", out, sizeof out)
		   == CW_FETCH_PATH_OK);
	assert(!valid_sd_path(out));
	assert(parse("CWFETCH1 sd:/apps/CavEX/stage_token.txt", out,
				 sizeof out) == CW_FETCH_PATH_OK);
	assert(!valid_sd_path(out));

	/* Quoted form: optional for the last path, same rules inside. */
	assert(parse("CWFETCH1 \"sd:/apps/a b/c\"", out, sizeof out)
		   == CW_FETCH_PATH_OK);
	assert(strcmp(out, "sd:/apps/a b/c") == 0);
	assert(parse("CWFETCH1 \"sd:/apps/a b/c", out, sizeof out)
		   == CW_FETCH_PATH_QUOTE);
	assert(parse("CWFETCH1 \"sd:/apps/a\" junk", out, sizeof out)
		   == CW_FETCH_PATH_QUOTE);
	assert(parse("CWFETCH1 sd:/apps/a\"b", out, sizeof out)
		   == CW_FETCH_PATH_QUOTE);
	assert(parse("CWFETCH1 \"sd:/apps/../x\"", out, sizeof out)
		   == CW_FETCH_PATH_DOTDOT);
	assert(strcmp(cw_stage_path_reason(CW_FETCH_PATH_QUOTE), "bad_target")
		   == 0);

	test_put_paths();
	test_run_paths();
	test_delete_policy();
	test_delete_walk();

	puts("PASS test_stage_fetch_path");
	return 0;
}
