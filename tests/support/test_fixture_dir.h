#ifndef TEST_FIXTURE_DIR_H
#define TEST_FIXTURE_DIR_H

/*
	Shared test-fixture path helper (root-caused 2026-09-02, lane
	lane/test-fixtures).

	PROBLEM: a large family of env/golden tests build their on-disk world
	fixture at a FIXED literal directory name in the tree root (e.g.
	"test_map_item_tmp"). Two processes racing on that SAME path --
	`ctest -jN`, two independent `ctest` invocations sharing a build dir, or
	the Linux verify job colliding with a Windows one against the same
	checkout -- interleave their mkdir/write/rmdir calls on one directory.
	The next reader sees a half-written tree and either crashes outright
	(0xc0000409 on Windows) or trips a region_archive_create() assertion
	(Linux) reading the other run's partial state. An interrupted single run
	(Ctrl-C, a killed CI job) leaves the same kind of half-written directory
	behind for the NEXT run to trip over, even with no concurrency at all.

	FIX: give every fixture path a PID suffix ("<base>_<pid>"). Two
	processes -- concurrent or merely back-to-back with a stale leftover --
	can never collide on the literal string again, because the OS guarantees
	distinct PIDs for processes alive at the same time. The remaining risk
	(PID reuse across a long time span colliding with a stale, crashed-run
	leftover under the same PID) is handled defensively: test_fixture_dir()
	wipes any directory already sitting at the computed path before handing
	it back, and test_fixture_dir_cleanup() wipes it again once the test is
	done, so a passing run leaves nothing behind and a crashed run's mess
	cannot outlive one lucky PID collision.

	Header-only (tests/wolf_test_common.h's precedent): any test, pooled or
	focused, picks this up with a bare #include and no CMakeLists change --
	nothing here is a new engine translation unit, so it does not touch the
	manifest/pooling rules in docs/build_conventions.md. Every function is
	`static inline` so an unused one in a given TU is not a -Wunused-function
	warning (GCC/Clang only warn on unused non-inline `static` functions).
*/

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <dirent.h> /* opendir/readdir/closedir -- MinGW's dirent.h shim
                        supports this on Windows too (see the "d_type is not
                        populated by every libc" note this file mirrors from
                        source/game/gui/screen_select_world.c). */

#ifdef _WIN32
#include <direct.h>	 /* _rmdir */
#include <process.h> /* _getpid */
#define TEST_FIXTURE_RMDIR(p) _rmdir(p)
#define TEST_FIXTURE_GETPID() ((long)_getpid())
#else
#include <unistd.h> /* getpid, rmdir */
#define TEST_FIXTURE_RMDIR(p) rmdir(p)
#define TEST_FIXTURE_GETPID() ((long)getpid())
#endif

/* Portable "is <parent>/<name> a directory?" -- stat(), never dirent.d_type
   (unpopulated / DT_UNKNOWN on some libcs, notably the Windows dirent shim
   the PC build already works around in world_dir_is_directory()). */
static inline int test_fixture_is_dir(const char* parent, const char* name) {
	char p[1024];
	/* Precision-bounded (%.511s) so GCC can prove no truncation regardless of
	   how long an arbitrary directory-entry name theoretically could be --
	   real fixture paths never come close to 511 bytes per component. */
	snprintf(p, sizeof(p), "%.511s/%.511s", parent, name);
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Recursively remove a directory tree. Best-effort and silently a no-op
   when `path` does not exist (the common case: no stale fixture) --
   opendir() just returns NULL. Depth-bounded (16 comfortably covers any
   test-fixture nesting: level root + region/ or data/ + DIM-1/region/ +
   per-chunk/per-map files) as a hard stop against symlink cycles on exotic
   filesystems, mirroring sworld_delete_tree's convention in
   source/game/gui/screen_select_world.c. */
static inline void test_fixture_rm_rf_at_depth(const char* path, int depth) {
	if(depth > 16)
		return;
	DIR* d = opendir(path);
	if(d) {
		struct dirent* e;
		while((e = readdir(d))) {
			if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char p[1024];
			snprintf(p, sizeof(p), "%.511s/%.511s", path, e->d_name);
			if(test_fixture_is_dir(path, e->d_name))
				test_fixture_rm_rf_at_depth(p, depth + 1);
			else
				(void)remove(p);
		}
		closedir(d);
	}
	/* `path` itself may be a plain FILE, not a directory -- opendir() above
	   then fails and this is the only cleanup step it gets (the fixed-temp-
	   FILE hazard class, e.g. a captured-stderr log or a scratch .json,
	   uses this same helper against a single file rather than a directory
	   tree). remove() unlinks a file on every platform; Windows' remove()
	   specifically does NOT fall back to rmdir() the way some POSIX libcs'
	   remove() does, so a directory still needs the separate rmdir() call
	   below. Try both -- each is a silent no-op against a path of the wrong
	   kind, or one that's already gone. */
	(void)remove(path);
	(void)TEST_FIXTURE_RMDIR(path);
}

static inline void test_fixture_rm_rf(const char* path) {
	test_fixture_rm_rf_at_depth(path, 0);
}

/* Builds a unique per-process fixture PATH ("<base>_<pid>") into
   out[0..out_size), and wipes any directory tree already sitting there --
   almost always a no-op, and exactly the defensive step described above
   when it isn't. Does NOT create the directory itself: callers already
   MKDIR/mkdir their own tree shape immediately after calling this, exactly
   as they did with the old fixed literal. Returns `out`. */
static inline const char* test_fixture_dir(const char* base, char* out,
											size_t out_size) {
	snprintf(out, out_size, "%s_%ld", base, TEST_FIXTURE_GETPID());
	test_fixture_rm_rf(out);
	return out;
}

/* Cleanup counterpart: call once the test is done with a given fixture
   directory (normally right before `return` in main()) so a PASSING run
   leaves no directory behind in the tree root. Safe to call on a path that
   was already torn down by the test's own explicit cleanup -- rm_rf is a
   no-op on a path that doesn't exist. */
static inline void test_fixture_dir_cleanup(const char* path) {
	test_fixture_rm_rf(path);
}

/* Drop-in replacement for the adjacent-string-literal-concatenation idiom
   `BASE_MACRO "suffix"` that every one of these tests used while BASE_MACRO
   was itself a literal. Now that a fixture macro expands to a `char*`
   runtime variable instead of a literal token, that concatenation no longer
   compiles, so every such call site becomes
   `test_fixture_join(BASE_MACRO, "suffix")` instead -- same resulting
   string, built at runtime rather than by the preprocessor.

   Backed by a small ring of static buffers so a handful of calls can be
   alive at once within one expression/statement (e.g. two arguments to the
   same snprintf) without clobbering each other. Not thread-safe, same as
   every other piece of test-fixture state in these files (single
   drain/worker thread only, never itself touching fixture paths). */
static inline const char* test_fixture_join(const char* base,
											 const char* suffix) {
	static char bufs[4][384];
	static int slot = -1;
	slot = (slot + 1) & 3;
	snprintf(bufs[slot], sizeof(bufs[slot]), "%s%s", base, suffix);
	return bufs[slot];
}

#endif
