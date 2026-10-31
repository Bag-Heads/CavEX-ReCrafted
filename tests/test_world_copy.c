#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "network/world_copy.h"
#include "cNBT/nbt.h"
#include "network/level_template.h"
#include <errno.h>

#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define make_dir(p) _mkdir(p)
#define remove_dir(p) _rmdir(p)
#define process_id() _getpid()
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#define make_dir(p) mkdir(p, 0777)
#define remove_dir(p) rmdir(p)
#define process_id() getpid()
#endif

static char fixture[80], source[128];

static void path_join(char* out, size_t cap, const char* parent, const char* name) {
	int n = snprintf(out, cap, "%s/%s", parent, name);
	assert(n >= 0 && (size_t)n < cap);
}

#ifdef _WIN32
static void native_path(const char* path, char* out, size_t cap) {
	char current[1024];
	DWORD n = GetCurrentDirectoryA(sizeof(current), current);
	assert(n && n < sizeof(current));
	int len = snprintf(out, cap, "\\\\?\\%s\\%s", current, path);
	assert(len > 0 && (size_t)len < cap);
	for(char* p = out; *p; p++) if(*p == '/') *p = '\\';
}
#endif

static void write_file(const char* path, size_t size) {
#ifdef _WIN32
	/* Long-path fixture creation must not be limited by Win32 MAX_PATH. */
	char native[1100];
	native_path(path, native, sizeof(native));
	HANDLE file = CreateFileA(native, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL, NULL);
	assert(file != INVALID_HANDLE_VALUE);
	for(size_t i = 0; i < size; i++) {
		DWORD written;
		assert(WriteFile(file, "x", 1, &written, NULL) && written == 1);
	}
	assert(CloseHandle(file));
#else
	FILE* file = fopen(path, "wb");
	assert(file);
	for(size_t i = 0; i < size; i++) assert(fputc('x', file) == 'x');
	assert(fclose(file) == 0);
#endif
}

static void erase_file(const char* path) {
#ifdef _WIN32
	char native[1100];
	native_path(path, native, sizeof(native));
	assert(DeleteFileA(native));
#else
	assert(unlink(path) == 0);
#endif
}

static bool exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static size_t child_count(const char* path) {
	DIR* d = opendir(path);
	assert(d);
	size_t count = 0;
	struct dirent* e;
	while((e = readdir(d)))
		if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) count++;
	assert(closedir(d) == 0);
	return count;
}

static const struct world_copy_result* finish(struct world_copy* c) {
	assert(c);
	size_t calls = 0;
	enum world_copy_state state;
	do {
		state = world_copy_step(c, 1);
		assert(++calls < 10000);
	} while(state == WORLD_COPY_SURVEYING || state == WORLD_COPY_CLEANING);
	return world_copy_get_result(c);
}

static void expect_error(const char* path, enum world_copy_error error) {
	struct world_copy* c = world_copy_survey_begin(path, 4096);
	const struct world_copy_result* r = finish(c);
	if(r->error != error)
		fprintf(stderr, "expected %s; got %s at %s (system=%d)\n",
			world_copy_error_name(error), world_copy_error_name(r->error),
			r->error_path, r->system_error);
	assert(r->state == WORLD_COPY_FAILED && r->error == error);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
}

static void test_counts_and_exclusive_names(void) {
	char region[180], file[220], occupied[180], hidden[180];
	path_join(region, sizeof(region), source, "region");
	assert(make_dir(region) == 0);
	path_join(file, sizeof(file), source, "level.dat");
	write_file(file, 1);
	path_join(file, sizeof(file), region, "r.0.0.mcr");
	write_file(file, 4097);
	path_join(file, sizeof(file), source, "empty");
	write_file(file, 0);
	snprintf(occupied, sizeof(occupied), "%s (backup)", source);
	write_file(occupied, 3); /* Files, as well as dirs, occupy a name. */
	path_join(hidden, sizeof(hidden), fixture, ".world-copy-1.tmp");
	assert(make_dir(hidden) == 0);
	path_join(file, sizeof(file), hidden, "sentinel");
	write_file(file, 2);

	struct world_copy* a = world_copy_survey_begin(source, 4096);
	assert(a);
	assert(world_copy_step(a, 0) == WORLD_COPY_SURVEYING);
	assert(world_copy_get_result(a)->directories == 0);
	const struct world_copy_result* r = finish(a);
	assert(r->state == WORLD_COPY_READY && r->error == WORLD_COPY_OK);
	assert(r->files == 3 && r->directories == 2 && r->bytes == 4098);
	/* File data: 3 clusters; copied dirs: 2; destination/journal parent
	 * entries: 2; journal data: 1; marker data/entry growth: 2. */
	assert(r->required_bytes == 10 * 4096);
	char expected[180];
	snprintf(expected, sizeof(expected), "%s (backup 2)", source);
	assert(!strcmp(r->destination, expected));
	assert(!r->temporary[0] && !exists(r->destination));
	assert(child_count(fixture) == 3); /* Survey creates no artifacts. */
	assert(child_count(source) == 3 && child_count(hidden) == 1);

	/* Recovery must not run against another live owner in the same parent.
	 * Keep its name occupied by an unjournaled user directory instead. */
	char user_directory[WORLD_COPY_PATH_MAX];
	strcpy(user_directory, r->destination);
	assert(world_copy_destroy(a) == WORLD_COPY_OK);
	assert(make_dir(user_directory) == 0);
	struct world_copy* b = world_copy_survey_begin(source, 4096);
	const struct world_copy_result* second = finish(b);
	assert(second->state == WORLD_COPY_READY);
	snprintf(expected, sizeof(expected), "%s (backup 3)", source);
	assert(!strcmp(second->destination, expected));
	assert(child_count(user_directory) == 0);
	assert(world_copy_destroy(b) == WORLD_COPY_OK);
	assert(remove_dir(user_directory) == 0);
	assert(exists(occupied) && exists(file));
	erase_file(file);
	assert(remove_dir(hidden) == 0);
	erase_file(occupied);
	path_join(file, sizeof(file), source, "level.dat"); erase_file(file);
	path_join(file, sizeof(file), region, "r.0.0.mcr"); erase_file(file);
	path_join(file, sizeof(file), source, "empty"); erase_file(file);
	assert(remove_dir(region) == 0);
}

static void test_step_cap_and_directory_clusters(void) {
	char file[220];
	for(unsigned i = 0; i < 140; i++) {
		snprintf(file, sizeof(file), "%s/f%u", source, i);
		write_file(file, 0);
	}
	struct world_copy* c = world_copy_survey_begin(source, 512);
	assert(world_copy_step(c, (size_t)-1) == WORLD_COPY_SURVEYING);
	assert(world_copy_get_result(c)->files <= WORLD_COPY_STEP_MAX);
	const struct world_copy_result* r = finish(c);
	assert(r->state == WORLD_COPY_READY && r->files == 140 && r->bytes == 0);
	/* 18 copied-dir clusters + 2 parent entries + 2 journal data
	 * clusters + marker data/entry growth. */
	assert(r->required_bytes == 24 * 512);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
	for(unsigned i = 0; i < 140; i++) {
		snprintf(file, sizeof(file), "%s/f%u", source, i);
		erase_file(file);
	}
	/* Destroying a mid-survey object closes handles without claiming names. */
	c = world_copy_survey_begin(source, 512);
	world_copy_step(c, 3);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
	assert(child_count(fixture) == 1);
}

static void test_depth_and_paths(void) {
	char nested[220];
	strcpy(nested, source);
	for(unsigned i = 0; i <= WORLD_COPY_DEPTH_MAX; i++) {
		strcat(nested, "/d");
		assert(make_dir(nested) == 0);
	}
	expect_error(source, WORLD_COPY_UNSUPPORTED_DEPTH);
	assert(child_count(fixture) == 1);
	assert(remove_dir(nested) == 0);
	nested[strlen(nested) - 2] = '\0';
	struct world_copy* c = world_copy_survey_begin(source, 4096);
	assert(finish(c)->state == WORLD_COPY_READY); /* Exact limit is supported. */
	assert(world_copy_get_result(c)->directories == WORLD_COPY_DEPTH_MAX + 1);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
	for(unsigned i = 0; i < WORLD_COPY_DEPTH_MAX; i++) {
		assert(remove_dir(nested) == 0);
		nested[strlen(nested) - 2] = '\0';
	}
	char too_long[WORLD_COPY_PATH_MAX + 2];
	memset(too_long, 'a', sizeof(too_long) - 1);
	too_long[sizeof(too_long) - 1] = '\0';
	expect_error(too_long, WORLD_COPY_PATH_TOO_LONG);
	/* An entry must not disappear silently because the joined path is long. */
	char name[250], file[400];
	memset(name, 'n', sizeof(name) - 1); name[sizeof(name) - 1] = '\0';
	path_join(file, sizeof(file), source, name);
	write_file(file, 0);
	expect_error(source, WORLD_COPY_PATH_TOO_LONG);
	erase_file(file);
	/* Source path fits; the mirrored backup path does not. */
	size_t n = WORLD_COPY_PATH_MAX - strlen(source) - 5;
	memset(name, 'n', n); name[n] = '\0';
	path_join(file, sizeof(file), source, name);
	write_file(file, 0);
	expect_error(source, WORLD_COPY_PATH_TOO_LONG);
	erase_file(file);
	assert(child_count(fixture) == 1);
	expect_error(NULL, WORLD_COPY_INVALID_ARGUMENT);
	expect_error("", WORLD_COPY_INVALID_ARGUMENT);
	expect_error("../world", WORLD_COPY_INVALID_ARGUMENT);
	expect_error("./world", WORLD_COPY_INVALID_ARGUMENT);
	expect_error("missing-world-copy-fixture", WORLD_COPY_IO_ERROR);
	expect_error("bad\\path", WORLD_COPY_INVALID_ARGUMENT);
	c = world_copy_survey_begin(source, 0);
	assert(world_copy_get_result(c)->error == WORLD_COPY_INVALID_ARGUMENT);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
}

static void test_links_and_special_files(void) {
	char target[220], link[220];
	path_join(target, sizeof(target), source, "target");
	path_join(link, sizeof(link), source, "link");
	write_file(target, 4);
#ifdef _WIN32
	assert(CreateHardLinkA(link, target, NULL));
#else
	assert(linkat(AT_FDCWD, target, AT_FDCWD, link, 0) == 0);
#endif
	expect_error(source, WORLD_COPY_UNSUPPORTED_LINK);
	erase_file(link);
	expect_error(target, WORLD_COPY_UNSUPPORTED_TYPE);
	erase_file(target);
#ifndef _WIN32
	assert(symlink("missing", link) == 0);
	expect_error(source, WORLD_COPY_UNSUPPORTED_LINK);
	erase_file(link);
	assert(symlink("world", link) == 0);
	expect_error(link, WORLD_COPY_UNSUPPORTED_LINK);
	erase_file(link);
	assert(mkfifo(link, 0600) == 0);
	expect_error(source, WORLD_COPY_UNSUPPORTED_TYPE);
	erase_file(link);
	/* An ancestor link is rejected too. */
	path_join(link, sizeof(link), fixture, "alias");
	assert(symlink("world", link) == 0);
	path_join(target, sizeof(target), source, "child");
	assert(make_dir(target) == 0);
	strcat(link, "/child");
	expect_error(link, WORLD_COPY_UNSUPPORTED_LINK);
	link[strlen(link) - 6] = '\0';
	erase_file(link);
	assert(remove_dir(target) == 0);
#endif
	assert(child_count(fixture) == 1);
}

static void test_foreign_destination(void) {
	struct world_copy* c = world_copy_survey_begin(source, 4096);
	const struct world_copy_result* r = finish(c);
	assert(r->state == WORLD_COPY_READY);
	char destination[WORLD_COPY_PATH_MAX], file[400];
	strcpy(destination, r->destination);
	assert(make_dir(destination) == 0);
	path_join(file, sizeof(file), destination, "foreign-data");
	write_file(file, 5);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
	assert(exists(file)); /* A destination name never grants ownership. */
	erase_file(file);
	assert(remove_dir(destination) == 0);

}


static const char* copied_files[] = {
	"level.dat", "session.lock", "empty", "region/r.0.0.mcr", "DIM-1/region/r.-1.2.mcr"
};
static uint64_t source_hashes[5];

static uint64_t hash_file(const char* path) {
	FILE* f = fopen(path, "rb");
	assert(f);
	uint64_t hash = UINT64_C(14695981039346656037);
	int ch;
	while((ch = fgetc(f)) != EOF) hash = (hash ^ (unsigned char)ch) * UINT64_C(1099511628211);
	assert(!ferror(f) && fclose(f) == 0);
	return hash;
}

static void check_source(void) {
	char path[WORLD_COPY_PATH_MAX];
	for(size_t i = 0; i < 5; i++) {
		path_join(path, sizeof(path), source, copied_files[i]);
		assert(hash_file(path) == source_hashes[i]);
	}
}

static void make_copy_fixture(void) {
	char path[WORLD_COPY_PATH_MAX];
	const char* dirs[] = {"region", "DIM-1", "DIM-1/region", "empty-dir"};
	for(size_t i = 0; i < 4; i++) {
		path_join(path, sizeof(path), source, dirs[i]); assert(make_dir(path) == 0);
	}
	path_join(path, sizeof(path), source, "level.dat");
	FILE* f = fopen(path, "wb"); assert(f);
	assert(fwrite(LEVEL_DAT_TEMPLATE, 1, sizeof(LEVEL_DAT_TEMPLATE), f) == sizeof(LEVEL_DAT_TEMPLATE));
	assert(fclose(f) == 0);
	static unsigned char data[4096];
	uint32_t rng = 7;
	for(size_t i = 0; i < sizeof(data); i++) { rng = rng * 1664525u + 1013904223u; data[i] = rng >> 24; }
	for(size_t i = 1; i < 5; i++) {
		path_join(path, sizeof(path), source, copied_files[i]);
		f = fopen(path, "wb"); assert(f);
		size_t length = i == 2 ? 0 : i == 3 ? 1024 * 1024 + 31 : 73;
		while(length) {
			size_t n = length > sizeof(data) ? sizeof(data) : length;
			assert(fwrite(data, 1, n, f) == n); length -= n;
		}
		assert(fclose(f) == 0);
	}
	for(size_t i = 0; i < 5; i++) {
		path_join(path, sizeof(path), source, copied_files[i]); source_hashes[i] = hash_file(path);
	}
}

static void erase_copy_fixture(const char* root) {
	char path[WORLD_COPY_PATH_MAX];
	for(size_t i = 0; i < 5; i++) {
		path_join(path, sizeof(path), root, copied_files[i]); erase_file(path);
	}
	const char* dirs[] = {"region", "DIM-1/region", "DIM-1", "empty-dir"};
	for(size_t i = 0; i < 4; i++) {
		path_join(path, sizeof(path), root, dirs[i]); assert(remove_dir(path) == 0);
	}
}

static const struct world_copy_result* drain(struct world_copy* c) {
	const struct world_copy_result* r = world_copy_get_result(c);
	size_t calls = 0;
	while(r->state == WORLD_COPY_COPYING || r->state == WORLD_COPY_CLEANING) {
		uint64_t before = r->copied_bytes;
		errno = EIO; /* A stale errno must never turn readdir EOF into failure. */
		world_copy_step(c, (size_t)-1);
		assert(r->copied_bytes - before <= WORLD_COPY_BYTES_PER_STEP / 2);
		assert(++calls < 10000);
	}
	return r;
}

static void check_backup(const char* backup) {
	char original[WORLD_COPY_PATH_MAX], copy[WORLD_COPY_PATH_MAX];
	for(size_t i = 1; i < 5; i++) {
		path_join(copy, sizeof(copy), backup, copied_files[i]);
		assert(hash_file(copy) == source_hashes[i]);
	}
	path_join(copy, sizeof(copy), backup, "empty-dir");
	assert(exists(copy) && child_count(copy) == 0);
	path_join(original, sizeof(original), source, "level.dat");
	path_join(copy, sizeof(copy), backup, "level.dat");
	assert(hash_file(copy) != source_hashes[0]);
	FILE* f = fopen(copy, "rb"); assert(f);
	assert(fgetc(f) == 0x1f && fgetc(f) == 0x8b && fclose(f) == 0);
	nbt_node *a = nbt_parse_path(original), *b = nbt_parse_path(copy);
	assert(a && b);
	nbt_node *an = nbt_find_by_path(a, ".Data.LevelName"), *bn = nbt_find_by_path(b, ".Data.LevelName");
	assert(an && bn && an->type == TAG_STRING && bn->type == TAG_STRING);
	char expected[300];
	snprintf(expected, sizeof(expected), "%s (backup)", an->payload.tag_string);
	assert(!strcmp(expected, bn->payload.tag_string));
	free(bn->payload.tag_string);
	bn->payload.tag_string = malloc(strlen(an->payload.tag_string) + 1);
	assert(bn->payload.tag_string);
	strcpy(bn->payload.tag_string, an->payload.tag_string);
	struct buffer ab = nbt_dump_binary_lossless_hint(a, 0), bb = nbt_dump_binary_lossless_hint(b, 0);
	assert(ab.data && bb.data && ab.len == bb.len && !memcmp(ab.data, bb.data, ab.len));
	buffer_free(&ab); buffer_free(&bb); nbt_free(a); nbt_free(b);
	check_source();
}

static struct world_copy* ready_copy(void) {
	struct world_copy* c = world_copy_survey_begin(source, 4096);
	assert(finish(c)->state == WORLD_COPY_READY);
	return c;
}

static void test_copy_success_and_collision(void) {
	struct world_copy* c = ready_copy();
	const struct world_copy_result* r = world_copy_get_result(c);
	assert(world_copy_start(c, UINT64_MAX) == WORLD_COPY_COPYING);
	assert(world_copy_step(c, 0) == WORLD_COPY_COPYING && r->copied_bytes == 0);
	world_copy_step(c, (size_t)-1);
	assert(r->state == WORLD_COPY_COPYING && r->copied_bytes < r->bytes);
	assert(drain(c)->state == WORLD_COPY_DONE);
	assert(r->copied_bytes == r->bytes);
	char marker[WORLD_COPY_PATH_MAX]; path_join(marker, sizeof(marker), r->destination, ".incomplete");
	assert(!exists(marker));
	char backup[WORLD_COPY_PATH_MAX]; strcpy(backup, r->destination);
	check_backup(backup);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
	assert(exists(backup));
	/* The published backup occupies its name; a later copy gets backup 2. */
	c = ready_copy(); r = world_copy_get_result(c);
	char expected[WORLD_COPY_PATH_MAX]; snprintf(expected, sizeof(expected), "%s (backup 2)", source);
	assert(!strcmp(expected, r->destination));
	assert(world_copy_start(c, UINT64_MAX) == WORLD_COPY_COPYING);
	assert(drain(c)->state == WORLD_COPY_DONE); check_backup(r->destination);
	erase_copy_fixture(r->destination); assert(remove_dir(r->destination) == 0);
	assert(world_copy_destroy(c) == WORLD_COPY_OK);
	erase_copy_fixture(backup); assert(remove_dir(backup) == 0);
}

static void test_copy_faults(void) {
	const enum world_copy_fault faults[] = {WORLD_COPY_FAULT_WRITE, WORLD_COPY_FAULT_CLOSE, WORLD_COPY_FAULT_FLUSH};
	for(size_t i = 0; i < 3; i++) {
		/* Journal, marker, and actual copied file. */
		for(unsigned successes = 0; successes < 3; successes++) {
			struct world_copy* c = ready_copy();
			const struct world_copy_result* r = world_copy_get_result(c);
			world_copy_start(c, UINT64_MAX);
			world_copy_test_fail_after(c, faults[i], successes);
			assert(drain(c)->state == WORLD_COPY_FAILED && r->error == WORLD_COPY_IO_ERROR);
			assert(!exists(r->destination));
			check_source(); assert(world_copy_destroy(c) == WORLD_COPY_OK);
			assert(child_count(fixture) == 1);
		}
	}
	struct world_copy* c = ready_copy();
	const struct world_copy_result* r = world_copy_get_result(c);
	world_copy_start(c, UINT64_MAX);
	while(r->copied_bytes != r->bytes) assert(world_copy_step(c, 1) == WORLD_COPY_COPYING);
	world_copy_test_fail_after(c, WORLD_COPY_FAULT_WRITE, 0);
	assert(drain(c)->state == WORLD_COPY_FAILED && r->error == WORLD_COPY_IO_ERROR);
	check_source(); assert(world_copy_destroy(c) == WORLD_COPY_OK);
	assert(child_count(fixture) == 1);
}

static void test_low_space_cancel_and_creation_collision(void) {
	struct world_copy* c = ready_copy();
	const struct world_copy_result* r = world_copy_get_result(c);
	world_copy_start(c, r->required_bytes - 1);
	assert(drain(c)->state == WORLD_COPY_FAILED && r->error == WORLD_COPY_LOW_SPACE);
	assert(!exists(r->destination));
	assert(world_copy_destroy(c) == WORLD_COPY_OK); check_source();
	c = ready_copy(); r = world_copy_get_result(c); world_copy_start(c, UINT64_MAX);
	while(!r->copied_bytes) assert(world_copy_step(c, 1) == WORLD_COPY_COPYING);
	assert(r->copied_bytes < r->bytes);
	world_copy_cancel(c);
	assert(drain(c)->state == WORLD_COPY_CANCELLED);
	assert(!exists(r->destination));
	assert(world_copy_destroy(c) == WORLD_COPY_OK); check_source();
	/* An occupied final path between survey and mkdir is never claimed. */
	for(unsigned kind = 0; kind < 3; kind++) {
		c = ready_copy(); r = world_copy_get_result(c); world_copy_start(c, UINT64_MAX);
		char foreign[WORLD_COPY_PATH_MAX], destination[WORLD_COPY_PATH_MAX];
		strcpy(destination, r->destination);
		if(kind < 2) assert(make_dir(destination) == 0);
		else write_file(destination, 3);
		path_join(foreign, sizeof(foreign), destination, "foreign");
		if(kind == 1) write_file(foreign, 3);
		assert(drain(c)->state == WORLD_COPY_FAILED && r->error == WORLD_COPY_COLLISION);
		assert(exists(destination) && !r->cleanup_error);
		assert(kind != 1 || exists(foreign));
		assert(world_copy_destroy(c) == WORLD_COPY_OK);
		c = ready_copy(); assert(world_copy_destroy(c) == WORLD_COPY_OK);
		assert(exists(destination));
		if(kind == 1) erase_file(foreign);
		if(kind < 2) assert(remove_dir(destination) == 0);
		else erase_file(destination);
		assert(child_count(fixture) == 1); check_source();
	}
}

/* Actual process death, rather than release, with durable marker/journal. */
static void interrupt_copy(const char* path, unsigned published) {
	struct world_copy* c = world_copy_survey_begin(path, 4096);
	assert(finish(c)->state == WORLD_COPY_READY);
	assert(world_copy_start(c, UINT64_MAX) == WORLD_COPY_COPYING);
	const struct world_copy_result* r = world_copy_get_result(c);
	while(r->copied_bytes < WORLD_COPY_BUFFER_BYTES) assert(world_copy_step(c, 1) == WORLD_COPY_COPYING);
	char marker[WORLD_COPY_PATH_MAX]; path_join(marker, sizeof(marker), r->destination, ".incomplete");
	assert(exists(marker));
	if(published) {
		while(exists(marker)) assert(world_copy_step(c, 1) == WORLD_COPY_COPYING);
		/* Marker removed, journal still present; the parent checks all bytes. */
	}
	exit(0);
}

static void test_interruption_recovery(const char* executable) {
	for(unsigned published = 0; published < 2; published++) {
#ifdef _WIN32
		const char* args[] = {executable, published ? "--published" : "--interrupt", source, NULL};
		assert(_spawnv(_P_WAIT, executable, args) == 0);
#else
		pid_t pid = fork(); assert(pid >= 0);
		if(!pid) { execl(executable, executable, published ? "--published" : "--interrupt", source, (char*)NULL); _exit(127); }
		int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
		assert(child_count(fixture) == 3);
		struct world_copy* c = ready_copy();
		assert(world_copy_destroy(c) == WORLD_COPY_OK);
		char backup[WORLD_COPY_PATH_MAX], journal[WORLD_COPY_PATH_MAX];
		snprintf(backup, sizeof(backup), "%s (backup)", source);
		path_join(journal, sizeof(journal), fixture, ".world-copy-1.owner");
		if(published) {
			check_backup(backup); assert(!exists(journal));
			erase_copy_fixture(backup); assert(remove_dir(backup) == 0);
		} else assert(!exists(backup) && !exists(journal));
		assert(child_count(fixture) == 1); check_source();
	}
}

/* Compare every entry name and file byte, including partial outputs/journals. */
static uint64_t snapshot_tree(const char* path) {
	DIR* d = opendir(path); assert(d);
	uint64_t hash = 0;
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char child[400]; path_join(child, sizeof(child), path, e->d_name);
		struct stat st; assert(stat(child, &st) == 0);
		uint64_t entry = UINT64_C(14695981039346656037);
		for(const char* n = e->d_name; *n; n++)
			entry = (entry ^ (unsigned char)*n) * UINT64_C(1099511628211);
		entry ^= S_ISDIR(st.st_mode) ? snapshot_tree(child) : hash_file(child);
		hash += entry;
	}
	assert(closedir(d) == 0);
	return hash;
}

static void erase_tree(const char* path) {
	DIR* d = opendir(path); assert(d);
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char child[400]; path_join(child, sizeof(child), path, e->d_name);
		struct stat st; assert(stat(child, &st) == 0);
		if(S_ISDIR(st.st_mode)) { erase_tree(child); assert(remove_dir(child) == 0); }
		else erase_file(child);
	}
	assert(closedir(d) == 0);
}

static bool has_journal(const char* destination) {
	struct world_copy_journals* journals;
	assert(world_copy_journals_read(fixture, &journals));
	bool found = world_copy_journals_contains(journals, destination);
	world_copy_journals_free(journals);
	return found;
}

static void check_world_list(bool published) {
	char backup[WORLD_COPY_PATH_MAX];
	snprintf(backup, sizeof(backup), "%s (backup)", source);
	DIR* d = opendir(fixture); assert(d);
	struct dirent* e;
	size_t visible = 0;
	while((e = readdir(d))) {
		if(e->d_name[0] == '.') continue;
		char path[WORLD_COPY_PATH_MAX], marker[WORLD_COPY_PATH_MAX];
		path_join(path, sizeof(path), fixture, e->d_name);
		path_join(marker, sizeof(marker), path, ".incomplete");
		if(exists(marker) || has_journal(path)) continue;
		visible++;
		if(strcmp(path, source)) { assert(published && !strcmp(path, backup)); check_backup(path); }
	}
	assert(closedir(d) == 0);
	assert(visible == (published ? 2 : 1)); check_source();
}

static void test_crash_every_boundary(void) {
	bool journal_only = false, empty_unmarked = false, marked = false, ready = false, mid_copy = false;
	bool published_journal = false, done_seen = false;
	/* Restart at every step, including mkdir-to-marker and marker-to-journal
	 * removal. The listing hides journal destinations at both boundaries. */
	for(unsigned mode = 0; mode < 2; mode++) {
		for(unsigned stop = 0; ; stop++) {
			assert(stop < 1000);
			struct world_copy* c = world_copy_survey_begin(source, 4096);
			const struct world_copy_result* r = world_copy_get_result(c);
			bool cancelled = false;
			unsigned step;
			for(step = 0; step < stop; step++) {
				if(mode && !cancelled && r->copied_bytes) { world_copy_cancel(c); cancelled = true; }
				if(r->state == WORLD_COPY_DONE || r->state == WORLD_COPY_CANCELLED) break;
				if(r->state == WORLD_COPY_READY) world_copy_start(c, UINT64_MAX);
				else world_copy_step(c, 1);
				assert(r->state != WORLD_COPY_FAILED);
			}
			char backup[WORLD_COPY_PATH_MAX], journal[WORLD_COPY_PATH_MAX], marker[WORLD_COPY_PATH_MAX];
			snprintf(backup, sizeof(backup), "%s (backup)", source);
			path_join(marker, sizeof(marker), backup, ".incomplete");
			path_join(journal, sizeof(journal), fixture, ".world-copy-1.owner");
			bool empty = exists(backup) && child_count(backup) == 0;
			bool published = exists(backup) && !exists(marker) && !empty;
			journal_only |= exists(journal) && !exists(backup);
			empty_unmarked |= empty && !exists(marker) && exists(journal);
			marked |= exists(marker); ready |= r->state == WORLD_COPY_READY;
			mid_copy |= r->copied_bytes && r->copied_bytes < r->bytes;
			published_journal |= published && exists(journal);
			done_seen |= r->state == WORLD_COPY_DONE;
			bool journaled = exists(journal);
			assert(has_journal(backup) == journaled);
			world_copy_release(c); check_world_list(published && !journaled);
			c = ready_copy(); assert(world_copy_destroy(c) == WORLD_COPY_OK);
			assert(!exists(journal));
			if(empty) assert(!exists(backup));
			check_world_list(published);
			if(published) { erase_copy_fixture(backup); assert(remove_dir(backup) == 0); }
			assert(child_count(fixture) == 1);
			if(step < stop) break;
		}
	}
	assert(journal_only && empty_unmarked && marked && ready && mid_copy && published_journal && done_seen);
}

static void test_recovery_ownership(void) {
	char user[WORLD_COPY_PATH_MAX], other[WORLD_COPY_PATH_MAX], marker[WORLD_COPY_PATH_MAX];
	char journal[WORLD_COPY_PATH_MAX], file[WORLD_COPY_PATH_MAX];
	snprintf(user, sizeof(user), "%s (backup)", source);
	snprintf(other, sizeof(other), "%s (backup 2)", source);
	path_join(journal, sizeof(journal), fixture, ".world-copy-1.owner");
	path_join(marker, sizeof(marker), user, ".incomplete");
	/* Neither a marker alone nor an invalid or mismatched journal grants ownership. */
	for(unsigned kind = 0; kind < 8; kind++) {
		assert(make_dir(user) == 0 && make_dir(other) == 0);
		path_join(file, sizeof(file), user, "user-data"); write_file(file, 7);
		char other_file[WORLD_COPY_PATH_MAX];
		path_join(other_file, sizeof(other_file), other, "user-data");
		if(kind == 2) write_file(other_file, 7); /* Nonempty, unmarked destination stays. */
		if(kind != 1) write_file(marker, 1);
		if(kind >= 1) {
			struct { char magic[16], source[WORLD_COPY_PATH_MAX], destination[WORLD_COPY_PATH_MAX]; } record = {0};
			memcpy(record.magic, "CaveWii-copy-v2", 15);
			strcpy(record.source, source); strcpy(record.destination, kind == 2 ? other : user);
			if(kind == 3) record.magic[0] = 'X';
			if(kind == 4) memset(record.destination, 'x', sizeof(record.destination));
			if(kind == 5) strcat(record.source, "X");
			if(kind == 6) memcpy(record.magic, "CaveWii-copy-v1", 15);
			FILE* f = fopen(journal, "wb"); assert(f);
			size_t bytes = kind == 7 ? sizeof(record) / 2 : sizeof(record);
			assert(fwrite(&record, 1, bytes, f) == bytes); assert(fclose(f) == 0);
		}
		uint64_t before = snapshot_tree(fixture);
		assert(has_journal(user) == (kind == 1));
		assert(has_journal(other) == (kind == 2));
		struct world_copy* c = ready_copy(); assert(world_copy_destroy(c) == WORLD_COPY_OK);
		assert(snapshot_tree(fixture) == before);
		if(exists(journal)) erase_file(journal);
		if(exists(marker)) erase_file(marker);
		if(kind == 2) erase_file(other_file);
		erase_file(file); assert(remove_dir(user) == 0 && remove_dir(other) == 0);
	}
	/* Old temp names, even empty ones, are now just user folders. */
	path_join(user, sizeof(user), fixture, ".world-copy-7.tmp"); assert(make_dir(user) == 0);
	uint64_t before = snapshot_tree(fixture);
	struct world_copy* c = ready_copy(); assert(world_copy_destroy(c) == WORLD_COPY_OK);
	assert(snapshot_tree(fixture) == before); assert(remove_dir(user) == 0);
	check_source();
}

static void test_unmarked_recovery(void) {
	char backup[WORLD_COPY_PATH_MAX], journal[WORLD_COPY_PATH_MAX], file[WORLD_COPY_PATH_MAX];
	snprintf(backup, sizeof(backup), "%s (backup)", source);
	path_join(journal, sizeof(journal), fixture, ".world-copy-1.owner");
	for(unsigned kind = 0; kind < 4; kind++) {
		/* Crash after the synced journal, before mkdir; optionally materialize
		 * the empty mkdir gap or an unmarked nonempty tree of unknown origin. */
		struct world_copy* c = ready_copy();
		assert(world_copy_start(c, UINT64_MAX) == WORLD_COPY_COPYING);
		world_copy_step(c, 1);
		world_copy_release(c);
		assert(exists(journal) && !exists(backup));
		if(kind) assert(make_dir(backup) == 0);
		if(kind >= 2) {
			path_join(file, sizeof(file), backup, kind == 2 ? "user-data" : "level.dat");
			write_file(file, 7); /* An invalid level.dat is not completion proof. */
		}
		uint64_t before = snapshot_tree(fixture);
		assert(has_journal(backup));
		c = ready_copy(); assert(world_copy_destroy(c) == WORLD_COPY_OK);
		if(kind < 2) assert(!exists(backup) && !exists(journal));
		else {
			assert(snapshot_tree(fixture) == before && exists(journal));
			assert(has_journal(backup));
			erase_file(file); assert(remove_dir(backup) == 0); erase_file(journal);
		}
		assert(child_count(fixture) == 1); check_source();
	}
}

static void test_release_all_states(void) {
	unsigned states = 0;
	size_t max_handles = 0;
	world_copy_release(NULL);
	/* Restart at every single-step boundary, including open input/output,
	 * metadata parsing/writing, nested survey, cleanup and recovery frames. */
	for(unsigned mode = 0; mode < 4; mode++) {
		for(unsigned stop = 0; ; stop++) {
			assert(stop < 1000);
			if(mode >= 2) {
				struct world_copy* interrupted = ready_copy();
				world_copy_start(interrupted, UINT64_MAX);
				world_copy_step(interrupted, 1); /* Durable journal. */
				world_copy_step(interrupted, 1); /* Empty destination, no marker yet. */
				if(mode == 2) world_copy_step(interrupted, 1); /* Synced marker, or the empty mkdir gap. */
				world_copy_release(interrupted);
			}
			struct world_copy* c = world_copy_survey_begin(source, 4096);
			const struct world_copy_result* r = world_copy_get_result(c);
			bool cancelled = false;
			unsigned step;
			for(step = 0; step < stop; step++) {
				if(r->state == WORLD_COPY_READY) world_copy_start(c, UINT64_MAX);
				if(mode == 1 && !cancelled && r->copied_bytes) {
					world_copy_cancel(c); cancelled = true;
				}
				if(r->state == WORLD_COPY_DONE || r->state == WORLD_COPY_CANCELLED
				   || r->state == WORLD_COPY_FAILED) break;
				world_copy_step(c, 1);
			}
			states |= 1u << r->state;
			size_t handles = world_copy_test_open_handles();
			if(handles > max_handles) max_handles = handles;
			uint64_t before = snapshot_tree(fixture);
			/* Release must ignore armed write/flush/close faults too. */
			world_copy_test_fail_after(c, WORLD_COPY_FAULT_CLOSE, 0);
			world_copy_release(c);
			assert(world_copy_test_open_handles() == 0);
			assert(snapshot_tree(fixture) == before);
			check_source();
			/* Only the test removes leftovers, after comparing the disk. */
			DIR* d = opendir(fixture); assert(d);
			struct dirent* e;
			while((e = readdir(d))) {
				if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")
				   || !strcmp(e->d_name, "world")) continue;
				char path[400]; path_join(path, sizeof(path), fixture, e->d_name);
				struct stat st; assert(stat(path, &st) == 0);
				if(S_ISDIR(st.st_mode)) { erase_tree(path); assert(remove_dir(path) == 0); }
				else erase_file(path);
			}
			assert(closedir(d) == 0);
			if(step < stop) break;
		}
	}
	struct world_copy* failed = world_copy_survey_begin(NULL, 4096);
	states |= 1u << world_copy_get_result(failed)->state;
	uint64_t before = snapshot_tree(fixture);
	world_copy_release(failed);
	assert(snapshot_tree(fixture) == before && world_copy_test_open_handles() == 0);
	assert(states == (1u << (WORLD_COPY_FAILED + 1)) - 1);
	assert(max_handles >= 3); /* Nested directory plus both file streams. */
}

int main(int argc, char** argv) {
	if(argc == 3) interrupt_copy(argv[2], !strcmp(argv[1], "--published"));
	snprintf(fixture, sizeof(fixture), "world_copy_fixture_%ld", (long)process_id());
	assert(make_dir(fixture) == 0);
	path_join(source, sizeof(source), fixture, "world");
	assert(make_dir(source) == 0);
	test_counts_and_exclusive_names();
	test_step_cap_and_directory_clusters();
	test_depth_and_paths();
	test_links_and_special_files();
	test_foreign_destination();
	make_copy_fixture();
	test_copy_success_and_collision();
	test_copy_faults();
	test_low_space_cancel_and_creation_collision();
	test_interruption_recovery(argv[0]);
	test_crash_every_boundary();
	test_recovery_ownership();
	test_unmarked_recovery();
	test_release_all_states();
	erase_copy_fixture(source);
	assert(child_count(fixture) == 1 && child_count(source) == 0);
	assert(remove_dir(source) == 0 && remove_dir(fixture) == 0);
	puts("world_copy: PASS (bounded survey/copy, hashes, gzip LevelName, collisions, write/close/flush faults, low space, cancel, journal-gated every-boundary visibility including mkdir before marker, absent/empty/complete recovery, nonempty unmarked preservation, disk-neutral release)");
	return 0;
}
