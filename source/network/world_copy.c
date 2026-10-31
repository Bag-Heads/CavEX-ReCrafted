#if !defined(_WIN32) && !defined(PLATFORM_WII) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "world_copy.h"
#include "../cNBT/nbt.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define copy_open _open
#define copy_fdopen _fdopen
#define copy_sync(f) _commit(_fileno(f))
#include <windows.h>
#define copy_mkdir(p) _mkdir(p)
#define copy_rmdir(p) _rmdir(p)
#else
#include <unistd.h>
#define copy_open open
#define copy_fdopen fdopen
#define copy_sync(f) fsync(fileno(f))
#ifdef PLATFORM_WII
FILE* fdopen(int fd, const char* mode);
int fileno(FILE* f);
int fsync(int fd);
#endif
#define copy_mkdir(p) mkdir(p, 0777)
#define copy_rmdir(p) rmdir(p)
#endif

/* Test targets include the standalone copier and the shared engine test pool. */
#if defined(CAVEWII_TESTING) || (!defined(PLATFORM_PC) && !defined(PLATFORM_WII))
static size_t copy_test_open_handles;
size_t world_copy_test_open_handles(void) { return copy_test_open_handles; }
static DIR* copy_test_opendir(const char* path) {
	DIR* d = opendir(path);
	if(d) copy_test_open_handles++;
	return d;
}
static int copy_test_closedir(DIR* d) {
	copy_test_open_handles--;
	return closedir(d);
}
static FILE* copy_test_fopen(const char* path, const char* mode) {
	FILE* f = fopen(path, mode);
	if(f) copy_test_open_handles++;
	return f;
}
static FILE* copy_test_fdopen(int fd, const char* mode) {
	FILE* f = copy_fdopen(fd, mode);
	if(f) copy_test_open_handles++;
	return f;
}
static int copy_test_fclose(FILE* f) {
	copy_test_open_handles--;
	return fclose(f);
}
#define opendir copy_test_opendir
#define closedir copy_test_closedir
#define fopen copy_test_fopen
#define fclose copy_test_fclose
#undef copy_fdopen
#define copy_fdopen copy_test_fdopen
#endif

enum copy_phase { COPY_VALIDATE, COPY_RECOVER, COPY_RECOVER_UNMARKED, COPY_OPEN, COPY_WALK,
	COPY_DEST, COPY_JOURNAL_NAME, COPY_JOURNAL, COPY_CREATE, COPY_MARKER, COPY_DATA_OPEN, COPY_DATA_WALK, COPY_FILE,
	COPY_LEVEL_OPEN, COPY_LEVEL_READ, COPY_LEVEL_EDIT, COPY_LEVEL_WRITE,
	COPY_PUBLISH, COPY_FINISH, COPY_CLEAN_OPEN, COPY_CLEAN_WALK };

/* Shared only during a synchronous step; no Wii stack-sized transfer arrays. */
static unsigned char copy_buffer[WORLD_COPY_BUFFER_BYTES];
#define COPY_NBT_LIMIT (8u * 1024u * 1024u) /* Existing cNBT input ceiling. */
struct copy_journal {
	char magic[16];
	char source[WORLD_COPY_PATH_MAX];
	char destination[WORLD_COPY_PATH_MAX];
};

struct copy_frame {
	DIR* directory;
	size_t length;
	uint64_t entry_bytes;
};

struct world_copy {
	struct world_copy_result result;
	struct copy_frame frames[WORLD_COPY_DEPTH_MAX + 1];
	char path[WORLD_COPY_PATH_MAX];
	char source[WORLD_COPY_PATH_MAX];
	size_t root_length, parent_length, validate_at, longest_relative;
	size_t path_overhead; /* Absolute Windows path includes the working directory. */
	uint32_t cluster_bytes, name_index;
	unsigned depth;
	enum copy_phase phase;
	bool owns_destination, owns_journal, started, recovering;
	DIR* recovery_directory;
	char journal_path[WORLD_COPY_PATH_MAX];
	char marker_path[WORLD_COPY_PATH_MAX];
	char target[WORLD_COPY_PATH_MAX];
	FILE *input, *output;
	uint64_t remaining, free_bytes, level_bytes;
	struct buffer metadata;
	size_t metadata_at;
	enum world_copy_state cleanup_target;
	enum world_copy_fault fault;
	size_t fault_after;
};

static void copy_begin_cleanup(struct world_copy* c, enum world_copy_state target);

static void copy_fail(struct world_copy* c, enum world_copy_error error,
	const char* path, int system_error) {
	if(c->result.state == WORLD_COPY_CLEANING) {
		if(!c->result.cleanup_error) c->result.cleanup_error = system_error ? system_error : EIO;
		return;
	}
	c->result.state = WORLD_COPY_FAILED;
	c->result.error = error;
	c->result.system_error = system_error;
	/* path can be result.error_path during cleanup. */
	if(path != c->result.error_path)
		snprintf(c->result.error_path, sizeof(c->result.error_path), "%s", path);
	if(c->started || (c->owns_journal && !c->recovering))
		copy_begin_cleanup(c, WORLD_COPY_FAILED);
}

static bool copy_add(struct world_copy* c, uint64_t* value, uint64_t add) {
	if(UINT64_MAX - *value < add) {
		copy_fail(c, WORLD_COPY_SIZE_OVERFLOW, c->path, 0);
		return false;
	}
	*value += add;
	return true;
}

static bool copy_allocate(struct world_copy* c, uint64_t bytes) {
	uint64_t clusters = bytes / c->cluster_bytes + (bytes % c->cluster_bytes != 0);
	if(clusters > UINT64_MAX / c->cluster_bytes) {
		copy_fail(c, WORLD_COPY_SIZE_OVERFLOW, c->path, 0);
		return false;
	}
	return copy_add(c, &c->result.required_bytes, clusters * c->cluster_bytes);
}

/* One short entry plus a conservative LFN slot for every 13 UTF-8 bytes.
 * UTF-8 byte length bounds the number of UTF-16 code units on FAT. */
static uint64_t copy_entry_bytes(const char* name) {
	return 32 * (1 + (strlen(name) + 12) / 13);
}

static bool copy_classify(struct world_copy* c, const char* path,
	bool* directory, uint64_t* size) {
#ifdef _WIN32
	HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE
		| FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	BY_HANDLE_FILE_INFORMATION info;
	if(h == INVALID_HANDLE_VALUE) {
		copy_fail(c, WORLD_COPY_IO_ERROR, path, (int)GetLastError());
		return false;
	}
	BOOL ok = GetFileInformationByHandle(h, &info);
	DWORD error = ok ? 0 : GetLastError();
	if(!CloseHandle(h) && ok) {
		ok = FALSE;
		error = GetLastError();
	}
	if(!ok) {
		copy_fail(c, WORLD_COPY_IO_ERROR, path, (int)error);
		return false;
	}
	*directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	if((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
	   || (!*directory && info.nNumberOfLinks > 1)) {
		copy_fail(c, WORLD_COPY_UNSUPPORTED_LINK, path, 0);
		return false;
	}
	if(info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) {
		copy_fail(c, WORLD_COPY_UNSUPPORTED_TYPE, path, 0);
		return false;
	}
	*size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
#else
	struct stat st;
	/* libfat has no links; host filesystems must classify without following. */
#ifdef PLATFORM_WII
	int rc = stat(path, &st);
#else
	int rc = lstat(path, &st);
#endif
	if(rc != 0) {
		copy_fail(c, WORLD_COPY_IO_ERROR, path, errno);
		return false;
	}
	if(S_ISLNK(st.st_mode) || (S_ISREG(st.st_mode) && st.st_nlink > 1)) {
		copy_fail(c, WORLD_COPY_UNSUPPORTED_LINK, path, 0);
		return false;
	}
	*directory = S_ISDIR(st.st_mode);
	if(!*directory && !S_ISREG(st.st_mode)) {
		copy_fail(c, WORLD_COPY_UNSUPPORTED_TYPE, path, 0);
		return false;
	}
	if(st.st_size < 0) {
		copy_fail(c, WORLD_COPY_SIZE_OVERFLOW, path, 0);
		return false;
	}
	*size = (uint64_t)st.st_size;
#endif
	return true;
}

struct world_copy* world_copy_survey_begin(const char* source,
	uint32_t cluster_bytes) {
	struct world_copy* c = calloc(1, sizeof(*c));
	if(!c) return NULL;
	c->result.state = WORLD_COPY_SURVEYING;
	c->cluster_bytes = cluster_bytes;
	c->name_index = 1;
	if(!source || !source[0] || cluster_bytes < 512
	   || (cluster_bytes & (cluster_bytes - 1))) {
		copy_fail(c, WORLD_COPY_INVALID_ARGUMENT, "", 0);
		return c;
	}
	size_t length = 0;
	while(length < WORLD_COPY_PATH_MAX && source[length]) length++;
	if(length == WORLD_COPY_PATH_MAX) {
		copy_fail(c, WORLD_COPY_PATH_TOO_LONG, "", 0);
		return c;
	}
	while(length && source[length - 1] == '/') length--;
	if(!length) {
		copy_fail(c, WORLD_COPY_INVALID_ARGUMENT, source, 0);
		return c;
	}
	memcpy(c->source, source, length);
	c->root_length = length;
#ifdef _WIN32
	char absolute[WORLD_COPY_PATH_MAX];
	DWORD absolute_length = GetFullPathNameA(c->source, sizeof(absolute), absolute, NULL);
	if(!absolute_length || absolute_length >= sizeof(absolute)) {
		copy_fail(c, absolute_length ? WORLD_COPY_PATH_TOO_LONG : WORLD_COPY_IO_ERROR,
			c->source, absolute_length ? 0 : (int)GetLastError());
		return c;
	}
	if(absolute_length > length) c->path_overhead = absolute_length - length;
#endif
	/* Reject traversal, ambiguous Windows separators and empty components.
	 * The volume prefix (C:/ or sd:/) and leading POSIX slash are allowed. */
	size_t start = 0;
	for(size_t i = 0; i <= length; i++) {
		if(c->source[i] == '\\') {
			copy_fail(c, WORLD_COPY_INVALID_ARGUMENT, c->source, 0);
			return c;
		}
		if(c->source[i] != '/' && i != length) continue;
		size_t n = i - start;
		if((!n && i != 0) || (n == 1 && c->source[start] == '.')
		   || (n == 2 && !memcmp(c->source + start, "..", 2))) {
			copy_fail(c, WORLD_COPY_INVALID_ARGUMENT, c->source, 0);
			return c;
		}
		if(i != length) c->parent_length = i + 1;
		start = i + 1;
	}
	if(!length || c->parent_length == length
	   || strchr(c->source + c->parent_length, ':')) {
		copy_fail(c, WORLD_COPY_INVALID_ARGUMENT, c->source, 0);
		return c;
	}
	strcpy(c->path, c->source);
	c->validate_at = c->source[0] == '/' ? 1 : 0;
	return c;
}

static void copy_validate_step(struct world_copy* c) {
	size_t end = c->validate_at;
	while(c->source[end] && c->source[end] != '/') end++;
	c->validate_at = end + 1;
	/* Skip the volume prefix; validate every real ancestor separately. */
	if(end && c->source[end - 1] == ':' && c->source[end] == '/') return;
	char saved = c->path[end];
	c->path[end] = '\0';
	bool directory;
	uint64_t size;
	if(copy_classify(c, c->path, &directory, &size) && !directory)
		copy_fail(c, WORLD_COPY_UNSUPPORTED_TYPE, c->path, 0);
	c->path[end] = saved;
	if(end == c->root_length && c->result.state != WORLD_COPY_FAILED) {
		memcpy(c->target, c->source, c->parent_length);
		c->target[c->parent_length] = '\0';
		c->recovery_directory = opendir(c->parent_length ? c->target : ".");
		if(!c->recovery_directory) copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno);
		else c->phase = COPY_RECOVER;
	}
}

static void copy_walk_step(struct world_copy* c) {
	struct copy_frame* frame = &c->frames[c->depth];
	c->path[frame->length] = '\0';
	errno = 0;
	struct dirent* entry = readdir(frame->directory);
	if(!entry) {
		int error = errno;
		if(closedir(frame->directory) != 0 && !error) error = errno;
		frame->directory = NULL;
		if(error) copy_fail(c, WORLD_COPY_IO_ERROR, c->path, error);
		if(c->result.state == WORLD_COPY_FAILED
		   || !copy_allocate(c, frame->entry_bytes)) return;
		if(c->depth) c->depth--;
		else c->phase = COPY_DEST;
		return;
	}
	if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) return;
	if(!c->depth && !strcmp(entry->d_name, ".incomplete")) {
		copy_fail(c, WORLD_COPY_INVALID_ARGUMENT, c->path, 0); return;
	}
	size_t n = strlen(entry->d_name);
	if(frame->length + 1 + n + c->path_overhead >= sizeof(c->path)) {
		copy_fail(c, WORLD_COPY_PATH_TOO_LONG, c->path, 0);
		return;
	}
	c->path[frame->length] = '/';
	memcpy(c->path + frame->length + 1, entry->d_name, n + 1);
	size_t relative = frame->length + 1 + n - c->root_length;
	if(relative > c->longest_relative) c->longest_relative = relative;
	if(!copy_add(c, &frame->entry_bytes, copy_entry_bytes(entry->d_name))) return;
	bool directory;
	uint64_t size;
	if(!copy_classify(c, c->path, &directory, &size)) return;
	if(directory) {
		if(c->depth == WORLD_COPY_DEPTH_MAX) {
			copy_fail(c, WORLD_COPY_UNSUPPORTED_DEPTH, c->path, 0);
			return;
		}
		c->depth++;
		c->phase = COPY_OPEN;
	} else if(copy_add(c, &c->result.files, 1)
		&& copy_add(c, &c->result.bytes, size)) copy_allocate(c, size);
}

/* Select names without creating anything. mkdir at COPY_CREATE is exclusive. */
static void copy_name_step(struct world_copy* c, bool journal) {
	char* path = journal ? c->journal_path : c->result.destination;
	int n;
	if(journal)
		n = snprintf(path, WORLD_COPY_PATH_MAX, "%.*s.world-copy-%u.owner",
			(int)c->parent_length, c->source, (unsigned)c->name_index);
	else if(c->name_index == 1)
		n = snprintf(path, WORLD_COPY_PATH_MAX, "%s (backup)", c->source);
	else
		n = snprintf(path, WORLD_COPY_PATH_MAX, "%s (backup %u)", c->source,
			(unsigned)c->name_index);
	size_t relative = c->longest_relative > 12 ? c->longest_relative : 12;
	if(n < 0 || (size_t)n + (journal ? 0 : relative) + c->path_overhead >= WORLD_COPY_PATH_MAX) {
		copy_fail(c, WORLD_COPY_PATH_TOO_LONG, path, 0); return;
	}
	struct stat st;
#if defined(_WIN32) || defined(PLATFORM_WII)
	int rc = stat(path, &st);
#else
	int rc = lstat(path, &st);
#endif
	if(rc == 0) {
		if(c->name_index == UINT32_MAX) copy_fail(c, WORLD_COPY_NAME_EXHAUSTED, path, 0);
		else c->name_index++;
		return;
	}
	if(errno != ENOENT) { copy_fail(c, WORLD_COPY_IO_ERROR, path, errno); return; }
	if(!copy_allocate(c, copy_entry_bytes(path + c->parent_length))) return;
	if(journal) {
		/* Journal data and the marker's data/entry allocation. */
		if(copy_allocate(c, sizeof(struct copy_journal)) && copy_allocate(c, 1)
		   && copy_allocate(c, copy_entry_bytes(".incomplete")))
			c->result.state = WORLD_COPY_READY;
	} else {
		memcpy(c->marker_path, path, (size_t)n);
		memcpy(c->marker_path + n, "/.incomplete", 13);
		c->phase = COPY_JOURNAL_NAME;
		c->name_index = 1;
	}
}

/* T08 implementation below. */
const struct world_copy_result* world_copy_get_result(const struct world_copy* c) {
	return c ? &c->result : NULL;
}

const char* world_copy_error_name(enum world_copy_error error) {
	switch(error) {
	case WORLD_COPY_OK: return "ok";
	case WORLD_COPY_INVALID_ARGUMENT: return "invalid_argument";
	case WORLD_COPY_PATH_TOO_LONG: return "path_too_long";
	case WORLD_COPY_UNSUPPORTED_DEPTH: return "unsupported_depth";
	case WORLD_COPY_UNSUPPORTED_LINK: return "unsupported_link";
	case WORLD_COPY_UNSUPPORTED_TYPE: return "unsupported_type";
	case WORLD_COPY_IO_ERROR: return "io_error";
	case WORLD_COPY_SIZE_OVERFLOW: return "size_overflow";
	case WORLD_COPY_NAME_EXHAUSTED: return "name_exhausted";
	case WORLD_COPY_CLEANUP_ERROR: return "cleanup_error";
	case WORLD_COPY_LOW_SPACE: return "low_space";
	case WORLD_COPY_INVALID_LEVEL: return "invalid_level";
	case WORLD_COPY_COLLISION: return "collision";
	}
	return "unknown_error";
}

void world_copy_test_fail_after(struct world_copy* c,
	enum world_copy_fault fault, size_t successes) {
	if(c) { c->fault = fault; c->fault_after = successes; }
}

static bool copy_fault(struct world_copy* c, enum world_copy_fault fault) {
	if(c->fault != fault) return false;
	if(c->fault_after) { c->fault_after--; return false; }
	c->fault = WORLD_COPY_FAULT_NONE;
	errno = EIO;
	return true;
}

static bool copy_write(struct world_copy* c, const void* bytes, size_t n) {
	if(copy_fault(c, WORLD_COPY_FAULT_WRITE)) {
		/* A genuine partial output, with the real stream still closed later. */
		if(n > 1 && fwrite(bytes, 1, n / 2, c->output) != n / 2) return false;
		errno = EIO;
		return false;
	}
	return fwrite(bytes, 1, n, c->output) == n && !ferror(c->output);
}

static int copy_close(struct world_copy* c, FILE** fp, bool output) {
	if(!*fp) return 0;
	int error = 0;
	if(output) {
		if(fflush(*fp) != 0) error = errno ? errno : EIO;
		if(copy_fault(c, WORLD_COPY_FAULT_FLUSH) && !error) error = EIO;
		if(!error && copy_sync(*fp) != 0) error = errno ? errno : EIO;
	}
	if(fclose(*fp) != 0 && !error) error = errno ? errno : EIO;
	*fp = NULL;
	if(copy_fault(c, WORLD_COPY_FAULT_CLOSE) && !error) error = EIO;
	return error;
}

static int copy_close_handles(struct world_copy* c) {
	int error = copy_close(c, &c->output, true);
	int e = copy_close(c, &c->input, false);
	if(!error) error = e;
	for(unsigned i = 0; i <= WORLD_COPY_DEPTH_MAX; i++) {
		if(c->frames[i].directory) {
			if(closedir(c->frames[i].directory) != 0 && !error)
				error = errno ? errno : EIO;
			c->frames[i].directory = NULL;
		}
	}
	return error;
}

static void copy_begin_cleanup(struct world_copy* c, enum world_copy_state target) {
	int error = copy_close_handles(c);
	if(error && !c->result.cleanup_error) c->result.cleanup_error = error;
	buffer_free(&c->metadata);
	c->cleanup_target = target;
	c->result.state = WORLD_COPY_CLEANING;
	c->depth = 0;
	strcpy(c->path, c->result.destination);
	c->phase = COPY_CLEAN_OPEN;
}

static void copy_cleanup_fail(struct world_copy* c, const char* path, int error) {
	if(!c->result.cleanup_error) c->result.cleanup_error = error ? error : EIO;
	int close_error = copy_close_handles(c);
	if(!c->result.error) {
		c->result.error = WORLD_COPY_CLEANUP_ERROR;
		c->result.system_error = c->result.cleanup_error;
		snprintf(c->result.error_path, sizeof(c->result.error_path), "%s", path);
	}
	if(close_error && !c->result.cleanup_error) c->result.cleanup_error = close_error;
	c->result.state = WORLD_COPY_FAILED;
}

/* Only a destination created by us, or authorized by journal AND marker. */
static void copy_cleanup_done(struct world_copy* c) {
	if(c->owns_journal) {
		if(remove(c->journal_path) != 0) {
			copy_cleanup_fail(c, c->journal_path, errno);
			return;
		}
		c->owns_journal = false;
	}
	if(c->recovering) {
		c->recovering = false;
		c->started = false;
		c->result.destination[0] = 0;
		c->result.state = WORLD_COPY_SURVEYING;
		c->phase = COPY_RECOVER;
		strcpy(c->path, c->source);
	} else {
		c->started = false;
		c->result.state = c->result.cleanup_error ? WORLD_COPY_FAILED : c->cleanup_target;
		if(c->result.cleanup_error && !c->result.error)
			c->result.error = WORLD_COPY_CLEANUP_ERROR;
	}
}

static void copy_cleanup_step(struct world_copy* c) {
	struct copy_frame* f = &c->frames[c->depth];
	if(c->phase == COPY_CLEAN_OPEN) {
		if(!c->owns_destination) { copy_cleanup_done(c); return; }
		bool directory;
		uint64_t size;
		/* Cleanup may already have removed the root. */
		struct stat st;
		if(stat(c->path, &st) != 0 && errno == ENOENT && !c->depth) {
			c->owns_destination = false;
			copy_cleanup_done(c);
			return;
		}
		/* Classify without following links before opening for deletion. */
		bool ok = copy_classify(c, c->path, &directory, &size);
		if(!ok || !directory) { copy_cleanup_fail(c, c->path, EIO); return; }
		f->directory = opendir(c->path);
		if(!f->directory) { copy_cleanup_fail(c, c->path, errno); return; }
		f->length = strlen(c->path);
		c->phase = COPY_CLEAN_WALK;
		return;
	}
	c->path[f->length] = 0;
	errno = 0; /* libsysbase does not clear errno at end-of-directory. */
	struct dirent* entry = readdir(f->directory);
	if(!entry) {
		int error = errno;
		if(closedir(f->directory) != 0 && !error) error = errno;
		f->directory = NULL;
		/* Keep the root marker through every bounded cleanup step. */
		if(!error && !c->depth && remove(c->marker_path) != 0 && errno != ENOENT)
			error = errno;
		if(error || copy_rmdir(c->path) != 0) {
			copy_cleanup_fail(c, c->path, error ? error : errno); return;
		}
		if(c->depth) c->depth--;
		else { c->owns_destination = false; copy_cleanup_done(c); }
		return;
	}
	if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) return;
	if(!c->depth && !strcmp(entry->d_name, ".incomplete")) return;
	int n = snprintf(c->path + f->length, sizeof(c->path) - f->length,
		"/%s", entry->d_name);
	if(n < 0 || (size_t)n >= sizeof(c->path) - f->length) {
		copy_cleanup_fail(c, c->path, ENAMETOOLONG); return;
	}
	bool directory;
	uint64_t size;
	bool ok = copy_classify(c, c->path, &directory, &size);
	if(!ok) { copy_cleanup_fail(c, c->path, EIO); return; }
	if(directory) {
		if(c->depth == WORLD_COPY_DEPTH_MAX) { copy_cleanup_fail(c, c->path, EIO); return; }
		c->depth++;
		c->phase = COPY_CLEAN_OPEN;
	} else if(remove(c->path) != 0) copy_cleanup_fail(c, c->path, errno);
}

static bool copy_backup_path(const char* source, const char* path) {
	size_t length = strlen(source);
	if(strncmp(path, source, length)) return false;
	const char* suffix = path + length;
	if(!strcmp(suffix, " (backup)")) return true;
	if(strncmp(suffix, " (backup ", 9)) return false;
	suffix += 9;
	if(*suffix < '1' || *suffix > '9') return false;
	while(*suffix >= '0' && *suffix <= '9') suffix++;
	return !strcmp(suffix, ")");
}

static bool copy_valid_journal(const struct copy_journal* record) {
	return !memcmp(record->magic, "CaveWii-copy-v2", 16)
		&& memchr(record->source, 0, sizeof(record->source))
		&& memchr(record->destination, 0, sizeof(record->destination))
		&& copy_backup_path(record->source, record->destination);
}

static bool copy_regular_file(const char* path, uint64_t* size) {
	struct stat st;
#ifdef _WIN32
	DWORD attrs = GetFileAttributesA(path);
	if(attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
#if defined(_WIN32) || defined(PLATFORM_WII)
	if(stat(path, &st) != 0) return false;
#else
	if(lstat(path, &st) != 0 || st.st_nlink != 1) return false;
#endif
	if(!S_ISREG(st.st_mode) || st.st_size < 0) return false;
	*size = (uint64_t)st.st_size;
	return true;
}

struct world_copy_journals {
	char destination[WORLD_COPY_PATH_MAX];
	struct world_copy_journals* next;
};

void world_copy_journals_free(struct world_copy_journals* journals) {
	while(journals) {
		struct world_copy_journals* next = journals->next;
		free(journals);
		journals = next;
	}
}

bool world_copy_journals_read(const char* parent, struct world_copy_journals** journals) {
	*journals = NULL;
	DIR* d = opendir(parent);
	if(!d) return false;
	struct copy_journal* record = NULL;
	bool ok = true;
	for(;;) {
		errno = 0;
		struct dirent* entry = readdir(d);
		if(!entry) { if(errno) ok = false; break; }
		unsigned index;
		char tail, name[48], path[WORLD_COPY_PATH_MAX];
		if(sscanf(entry->d_name, ".world-copy-%u.owner%c", &index, &tail) != 1 || !index) continue;
		snprintf(name, sizeof(name), ".world-copy-%u.owner", index);
		if(strcmp(name, entry->d_name)) continue;
		int n = snprintf(path, sizeof(path), "%s/%s", parent, name);
		if(n < 0 || (size_t)n >= sizeof(path)) continue;
		uint64_t size;
		if(!copy_regular_file(path, &size) || size != sizeof(*record)) continue;
		if(!record) record = malloc(sizeof(*record));
		if(!record) { ok = false; break; }
		FILE* f = fopen(path, "rb");
		if(!f) { ok = false; break; }
		bool read = fread(record, 1, sizeof(*record), f) == sizeof(*record) && !ferror(f);
		if(fclose(f) != 0) { ok = false; break; }
		if(!read || !copy_valid_journal(record)) continue;
		struct world_copy_journals* item = malloc(sizeof(*item));
		if(!item) { ok = false; break; }
		strcpy(item->destination, record->destination);
		item->next = *journals;
		*journals = item;
	}
	free(record);
	if(closedir(d) != 0) ok = false;
	if(!ok) { world_copy_journals_free(*journals); *journals = NULL; }
	return ok;
}

bool world_copy_journals_contains(const struct world_copy_journals* journals,
	const char* destination) {
	for(; journals; journals = journals->next)
		if(!strcmp(journals->destination, destination)) return true;
	return false;
}

/* An unmarked nonempty tree is never deleted. A readable final LevelName
 * identifies the already-published case, where only the journal is stale. */
static bool copy_complete_destination(struct world_copy* c) {
	int n = snprintf(c->target, sizeof(c->target), "%s/level.dat", c->result.destination);
	uint64_t size;
	if(n < 0 || (size_t)n + c->path_overhead >= sizeof(c->target)
	   || !copy_regular_file(c->target, &size) || size > COPY_NBT_LIMIT) return false;
	FILE* f = fopen(c->target, "rb");
	if(!f) return false;
	nbt_node* root = nbt_parse_file(f);
	bool closed = fclose(f) == 0;
	nbt_node* name = root ? nbt_find_by_path(root, ".Data.LevelName") : NULL;
	bool complete = closed && name && name->type == TAG_STRING && name->payload.tag_string;
	if(complete) {
		size_t length = strlen(name->payload.tag_string);
		complete = length >= 9 && !strcmp(name->payload.tag_string + length - 9, " (backup)");
	}
	nbt_free(root);
	return complete;
}

static void copy_retire_journal(struct world_copy* c) {
	if(remove(c->journal_path) != 0)
		copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, errno);
}

static void copy_recover_unmarked_step(struct world_copy* c) {
	DIR* d = c->frames[0].directory;
	errno = 0;
	struct dirent* entry = readdir(d);
	if(entry && (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))) return;
	bool empty = !entry;
	int error = empty ? errno : 0;
	if(closedir(d) != 0 && !error) error = errno;
	c->frames[0].directory = NULL;
	c->phase = COPY_RECOVER;
	if(error) { copy_fail(c, WORLD_COPY_IO_ERROR, c->result.destination, error); return; }
	if(empty) {
		/* Check emptiness first; rmdir also refuses newly added entries. */
		if(copy_rmdir(c->result.destination) != 0) {
			if(errno != ENOTEMPTY && errno != EEXIST)
				copy_fail(c, WORLD_COPY_IO_ERROR, c->result.destination, errno);
			return;
		}
		copy_retire_journal(c);
	} else if(copy_complete_destination(c)) copy_retire_journal(c);
}

static void copy_recover_step(struct world_copy* c) {
	errno = 0;
	struct dirent* entry = readdir(c->recovery_directory);
	if(!entry) {
		int error = errno;
		if(closedir(c->recovery_directory) != 0 && !error) error = errno;
		c->recovery_directory = NULL;
		if(error) copy_fail(c, WORLD_COPY_IO_ERROR, c->source, error);
		else { strcpy(c->path, c->source); c->phase = COPY_OPEN; }
		return;
	}
	unsigned index;
	char tail;
	if(sscanf(entry->d_name, ".world-copy-%u.owner%c", &index, &tail) != 1 || !index) return;
	int n = snprintf(c->journal_path, sizeof(c->journal_path), "%.*s.world-copy-%u.owner",
		(int)c->parent_length, c->source, index);
	if(n < 0 || (size_t)n + c->path_overhead >= sizeof(c->journal_path)) return;
	if(strcmp(c->journal_path + c->parent_length, entry->d_name)) return;
	bool directory;
	uint64_t size;
	if(!copy_classify(c, c->journal_path, &directory, &size)) return;
	if(directory || size != sizeof(struct copy_journal)) return; /* Not ours. */
	c->input = fopen(c->journal_path, "rb");
	if(!c->input) { copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, errno); return; }
	size_t got = fread(copy_buffer, 1, sizeof(struct copy_journal), c->input);
	int error = ferror(c->input) ? EIO : 0;
	int close_error = copy_close(c, &c->input, false);
	if(got != sizeof(struct copy_journal) || error || close_error) {
		copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, error ? error : close_error); return;
	}
	/* memcpy avoids alignment/aliasing assumptions for the static byte buffer. */
	struct copy_journal* record = malloc(sizeof(*record));
	if(!record) { copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, ENOMEM); return; }
	memcpy(record, copy_buffer, sizeof(*record));
	bool ours = copy_valid_journal(record) && !strcmp(record->source, c->source);
	if(ours) strcpy(c->result.destination, record->destination);
	free(record);
	if(!ours) return;
	size_t length = strlen(c->result.destination);
	if(length + sizeof("/.incomplete") + c->path_overhead > sizeof(c->marker_path)) return;
	memcpy(c->marker_path, c->result.destination, length);
	memcpy(c->marker_path + length, "/.incomplete", sizeof("/.incomplete"));
#ifdef _WIN32
	DWORD attrs = GetFileAttributesA(c->result.destination);
	if(attrs == INVALID_FILE_ATTRIBUTES) {
		if(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)
			copy_retire_journal(c);
		return;
	}
	if(!(attrs & FILE_ATTRIBUTE_DIRECTORY)
	   || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return;
	attrs = GetFileAttributesA(c->marker_path);
	bool unmarked = attrs == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
	if(!unmarked && (attrs == INVALID_FILE_ATTRIBUTES
	   || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))) return;
#else
	struct stat st;
#ifdef PLATFORM_WII
	int rc = stat(c->result.destination, &st);
#else
	int rc = lstat(c->result.destination, &st);
#endif
	if(rc != 0) { if(errno == ENOENT) copy_retire_journal(c); return; }
	if(!S_ISDIR(st.st_mode)) return;
#ifdef PLATFORM_WII
	rc = stat(c->marker_path, &st);
#else
	rc = lstat(c->marker_path, &st);
#endif
	bool unmarked = rc != 0 && errno == ENOENT;
	if(!unmarked && (rc != 0 || !S_ISREG(st.st_mode))) return;
#ifndef PLATFORM_WII
	if(!unmarked && st.st_nlink != 1) return;
#endif
#endif
	if(unmarked) {
		c->frames[0].directory = opendir(c->result.destination);
		if(!c->frames[0].directory) {
			copy_fail(c, WORLD_COPY_IO_ERROR, c->result.destination, errno); return;
		}
		c->phase = COPY_RECOVER_UNMARKED;
		return;
	}
	c->owns_destination = c->owns_journal = c->recovering = true;
	copy_begin_cleanup(c, WORLD_COPY_FAILED);
}

static void copy_journal_step(struct world_copy* c) {
	/* Sync the exact destination record before mkdir or any copy writes. */
	int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef _WIN32
	flags |= O_BINARY;
#endif
	int fd = copy_open(c->journal_path, flags, 0600);
	if(fd < 0) { copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, errno); return; }
	c->owns_journal = true;
	c->output = copy_fdopen(fd, "wb");
	if(!c->output) {
		int error = errno;
#ifdef _WIN32
		if(_close(fd) != 0) c->result.cleanup_error = errno;
#else
		if(close(fd) != 0) c->result.cleanup_error = errno;
#endif
		copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, error); return;
	}
	if(setvbuf(c->output, NULL, _IONBF, 0) != 0) {
		copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, EIO); return;
	}
	struct copy_journal* record = calloc(1, sizeof(*record));
	if(!record) { copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, ENOMEM); return; }
	memcpy(record->magic, "CaveWii-copy-v2", 15);
	strcpy(record->source, c->source);
	strcpy(record->destination, c->result.destination);
	bool ok = copy_write(c, record, sizeof(*record));
	int error = ok ? 0 : (errno ? errno : EIO);
	free(record);
	int close_error = copy_close(c, &c->output, true);
	if(!error) error = close_error;
	if(error) { copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, error); return; }
	c->phase = COPY_CREATE;
}

static void copy_create_step(struct world_copy* c) {
	if(copy_mkdir(c->result.destination) != 0) {
		copy_fail(c, errno == EEXIST ? WORLD_COPY_COLLISION : WORLD_COPY_IO_ERROR,
			c->result.destination, errno); return;
	}
	c->owns_destination = true;
	c->phase = COPY_MARKER;
}

static void copy_marker_step(struct world_copy* c) {
	int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef _WIN32
	flags |= O_BINARY;
#endif
	int fd = copy_open(c->marker_path, flags, 0600);
	if(fd < 0) { copy_fail(c, WORLD_COPY_IO_ERROR, c->marker_path, errno); return; }
	c->output = copy_fdopen(fd, "wb");
	if(!c->output) {
		int error = errno;
#ifdef _WIN32
		_close(fd);
#else
		close(fd);
#endif
		copy_fail(c, WORLD_COPY_IO_ERROR, c->marker_path, error); return;
	}
	if(setvbuf(c->output, NULL, _IONBF, 0) != 0) {
		copy_fail(c, WORLD_COPY_IO_ERROR, c->marker_path, EIO); return;
	}
	bool ok = copy_write(c, "1", 1);
	int error = ok ? 0 : EIO;
	int close_error = copy_close(c, &c->output, true);
	if(!error) error = close_error;
	if(error) { copy_fail(c, WORLD_COPY_IO_ERROR, c->marker_path, error); return; }
	c->phase = COPY_DATA_OPEN;
}

enum world_copy_state world_copy_start(struct world_copy* c, uint64_t free_bytes) {
	if(!c) return WORLD_COPY_FAILED;
	if(c->result.state != WORLD_COPY_READY) return c->result.state;
	c->started = true;
	c->free_bytes = free_bytes;
	if(free_bytes < c->result.required_bytes) {
		copy_fail(c, WORLD_COPY_LOW_SPACE, c->result.destination, ENOSPC);
		return c->result.state;
	}
	c->depth = 0;
	strcpy(c->path, c->source);
	c->phase = COPY_JOURNAL;
	c->result.state = WORLD_COPY_COPYING;
	return c->result.state;
}

void world_copy_cancel(struct world_copy* c) {
	if(c && c->recovering && c->result.state == WORLD_COPY_CLEANING) {
		/* Finish removing the abandoned tree, then stop instead of resuming
		 * the survey that discovered it. */
		c->recovering = false;
		c->cleanup_target = WORLD_COPY_CANCELLED;
		return;
	}
	if(!c || c->result.state == WORLD_COPY_DONE || c->result.state == WORLD_COPY_CANCELLED
	   || c->result.state == WORLD_COPY_FAILED || c->result.state == WORLD_COPY_CLEANING) return;
	copy_begin_cleanup(c, WORLD_COPY_CANCELLED);
}

static bool copy_target_path(struct world_copy* c) {
	int n = snprintf(c->target, sizeof(c->target), "%s%s",
		c->result.destination, c->path + c->root_length);
	if(n < 0 || (size_t)n + c->path_overhead >= sizeof(c->target)) {
		copy_fail(c, WORLD_COPY_PATH_TOO_LONG, c->path, 0); return false;
	}
	return true;
}

static void copy_data_walk(struct world_copy* c) {
	struct copy_frame* f = &c->frames[c->depth];
	c->path[f->length] = 0;
	errno = 0;
	struct dirent* entry = readdir(f->directory);
	if(!entry) {
		int error = errno;
		if(closedir(f->directory) != 0 && !error) error = errno;
		f->directory = NULL;
		if(error) { copy_fail(c, WORLD_COPY_IO_ERROR, c->path, error); return; }
		if(c->depth) c->depth--;
		else c->phase = COPY_LEVEL_OPEN;
		return;
	}
	if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) return;
	int n = snprintf(c->path + f->length, sizeof(c->path) - f->length, "/%s", entry->d_name);
	if(n < 0 || (size_t)n + c->path_overhead >= sizeof(c->path) - f->length) {
		copy_fail(c, WORLD_COPY_PATH_TOO_LONG, c->path, 0); return;
	}
	bool directory;
	uint64_t size;
	if(!copy_classify(c, c->path, &directory, &size) || !copy_target_path(c)) return;
	if(directory) {
		if(c->depth == WORLD_COPY_DEPTH_MAX) {
			copy_fail(c, WORLD_COPY_UNSUPPORTED_DEPTH, c->path, 0); return;
		}
		if(copy_mkdir(c->target) != 0) { copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno); return; }
		c->depth++;
		c->phase = COPY_DATA_OPEN;
	} else {
		c->input = fopen(c->path, "rb"); /* Source is only ever opened read-only. */
		if(!c->input) { copy_fail(c, WORLD_COPY_IO_ERROR, c->path, errno); return; }
		c->output = fopen(c->target, "wb");
		if(c->output && setvbuf(c->output, NULL, _IONBF, 0) != 0) {
			copy_fail(c, WORLD_COPY_IO_ERROR, c->target, EIO); return;
		}
		if(!c->output) { copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno); return; }
		c->remaining = size;
		c->phase = COPY_FILE;
	}
}

static void copy_file_step(struct world_copy* c, size_t* bytes) {
	if(c->remaining) {
		size_t n = *bytes / 2; /* Budget counts both the read and the write. */
		if(n > sizeof(copy_buffer)) n = sizeof(copy_buffer);
		if(n > c->remaining) n = (size_t)c->remaining;
		if(!n) return;
		*bytes -= 2 * n;
		if(fread(copy_buffer, 1, n, c->input) != n || ferror(c->input)) {
			copy_fail(c, WORLD_COPY_IO_ERROR, c->path, errno ? errno : EIO); return;
		}
		if(!copy_write(c, copy_buffer, n)) {
			copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno ? errno : EIO); return;
		}
		c->remaining -= n;
		c->result.copied_bytes += n;
		return;
	}
	/* Detect a source that grew despite the quiescence contract. */
	if(fgetc(c->input) != EOF || ferror(c->input)) {
		copy_fail(c, WORLD_COPY_IO_ERROR, c->path, EIO); return;
	}
	int error = copy_close(c, &c->output, true);
	int close_error = copy_close(c, &c->input, false);
	if(!error) error = close_error;
	if(error) copy_fail(c, WORLD_COPY_IO_ERROR, c->target, error);
	else c->phase = COPY_DATA_WALK;
}

static void copy_level_step(struct world_copy* c, size_t* bytes) {
	if(c->phase == COPY_LEVEL_OPEN) {
		int n = snprintf(c->target, sizeof(c->target), "%s/level.dat", c->result.destination);
		if(n < 0 || (size_t)n + c->path_overhead >= sizeof(c->target)) {
			copy_fail(c, WORLD_COPY_PATH_TOO_LONG, c->result.destination, 0); return;
		}
		bool directory;
		if(!copy_classify(c, c->target, &directory, &c->level_bytes)) return;
		if(directory || c->level_bytes > COPY_NBT_LIMIT) {
			copy_fail(c, WORLD_COPY_INVALID_LEVEL, c->target, NBT_EREFUSED); return;
		}
		c->input = fopen(c->target, "rb");
		if(!c->input) { copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno); return; }
		c->remaining = c->level_bytes;
		c->phase = COPY_LEVEL_READ;
	} else if(c->phase == COPY_LEVEL_READ) {
		if(c->remaining) {
			size_t n = *bytes;
			if(n > sizeof(copy_buffer)) n = sizeof(copy_buffer);
			if(n > c->remaining) n = (size_t)c->remaining;
			if(!n) return;
			*bytes -= n;
			if(fread(copy_buffer, 1, n, c->input) != n || ferror(c->input)) {
				copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno ? errno : EIO); return;
			}
			if(buffer_append(&c->metadata, copy_buffer, n)) {
				copy_fail(c, WORLD_COPY_IO_ERROR, c->target, ENOMEM); return;
			}
			c->remaining -= n;
			return;
		}
		int error = copy_close(c, &c->input, false);
		if(error) copy_fail(c, WORLD_COPY_IO_ERROR, c->target, error);
		else c->phase = COPY_LEVEL_EDIT;
	} else if(c->phase == COPY_LEVEL_EDIT) {
		nbt_node* root = nbt_parse_compressed(c->metadata.data, c->metadata.len);
		int error = errno;
		buffer_free(&c->metadata);
		nbt_node* name = root ? nbt_find_by_path(root, ".Data.LevelName") : NULL;
		if(!name || name->type != TAG_STRING || !name->payload.tag_string
		   || strlen(name->payload.tag_string) > 32767 - 9) {
			nbt_free(root);
			copy_fail(c, WORLD_COPY_INVALID_LEVEL, c->target, error); return;
		}
		size_t len = strlen(name->payload.tag_string);
		char* backup = malloc(len + 10);
		if(!backup) { nbt_free(root); copy_fail(c, WORLD_COPY_IO_ERROR, c->target, ENOMEM); return; }
		memcpy(backup, name->payload.tag_string, len);
		memcpy(backup + len, " (backup)", 10);
		free(name->payload.tag_string);
		name->payload.tag_string = backup;
		/* The lossless serializer preserves parser-captured opaque item images
		 * while rewriting only the outer LevelName. No archive save/rotation,
		 * migrations or fallback recovery can touch the source world. */
		struct buffer binary = nbt_dump_binary_lossless_hint(root, 0);
		nbt_free(root);
		if(binary.data) c->metadata = nbt_compress_binary(binary.data, binary.len, STRAT_GZIP);
		error = errno;
		buffer_free(&binary);
		if(!c->metadata.data || c->metadata.len > COPY_NBT_LIMIT) {
			copy_fail(c, WORLD_COPY_INVALID_LEVEL, c->target,
				c->metadata.data ? NBT_EREFUSED : error); return;
		}
		uint64_t old_clusters = (c->level_bytes + c->cluster_bytes - 1) / c->cluster_bytes;
		uint64_t new_clusters = (c->metadata.len + c->cluster_bytes - 1) / c->cluster_bytes;
		if(new_clusters > old_clusters && !copy_add(c, &c->result.required_bytes,
			(new_clusters - old_clusters) * c->cluster_bytes)) return;
		if(c->result.required_bytes > c->free_bytes) {
			copy_fail(c, WORLD_COPY_LOW_SPACE, c->target, ENOSPC); return;
		}
		c->output = fopen(c->target, "wb");
		if(c->output && setvbuf(c->output, NULL, _IONBF, 0) != 0) {
			copy_fail(c, WORLD_COPY_IO_ERROR, c->target, EIO); return;
		}
		if(!c->output) { copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno); return; }
		c->metadata_at = 0;
		c->phase = COPY_LEVEL_WRITE;
	} else {
		size_t n = c->metadata.len - c->metadata_at;
		if(n) {
			if(n > sizeof(copy_buffer)) n = sizeof(copy_buffer);
			if(n > *bytes) n = *bytes;
			if(!n) return;
			*bytes -= n;
			if(!copy_write(c, c->metadata.data + c->metadata_at, n)) {
				copy_fail(c, WORLD_COPY_IO_ERROR, c->target, errno ? errno : EIO); return;
			}
			c->metadata_at += n;
			return;
		}
		int error = copy_close(c, &c->output, true);
		buffer_free(&c->metadata);
		if(error) copy_fail(c, WORLD_COPY_IO_ERROR, c->target, error);
		else c->phase = COPY_PUBLISH;
	}
}

static void copy_publish(struct world_copy* c) {
	/* All payload and edited metadata handles are synced and closed. The
	 * marker is the publication gate; no directory entry is ever renamed. */
	if(remove(c->marker_path) != 0) {
		copy_fail(c, WORLD_COPY_IO_ERROR, c->marker_path, errno); return;
	}
	c->owns_destination = false;
	c->phase = COPY_FINISH;
}

static void copy_finish(struct world_copy* c) {
	if(remove(c->journal_path) != 0) {
		copy_fail(c, WORLD_COPY_IO_ERROR, c->journal_path, errno); return;
	}
	c->owns_journal = c->started = false;
	c->result.state = WORLD_COPY_DONE;
}

enum world_copy_state world_copy_step(struct world_copy* c, size_t budget) {
	if(!c) return WORLD_COPY_FAILED;
	if(budget > WORLD_COPY_STEP_MAX) budget = WORLD_COPY_STEP_MAX;
	size_t bytes = WORLD_COPY_BYTES_PER_STEP;
	while(budget-- && bytes && (c->result.state == WORLD_COPY_SURVEYING
		|| c->result.state == WORLD_COPY_COPYING || c->result.state == WORLD_COPY_CLEANING)) {
		switch(c->phase) {
		case COPY_VALIDATE: copy_validate_step(c); break;
		case COPY_RECOVER: copy_recover_step(c); break;
		case COPY_RECOVER_UNMARKED: copy_recover_unmarked_step(c); break;
		case COPY_OPEN: case COPY_DATA_OPEN: {
			bool survey = c->phase == COPY_OPEN;
			struct copy_frame* frame = &c->frames[c->depth];
			frame->directory = opendir(c->path);
			if(!frame->directory) { copy_fail(c, WORLD_COPY_IO_ERROR, c->path, errno); break; }
			frame->length = strlen(c->path);
			frame->entry_bytes = 96;
			if(survey) copy_add(c, &c->result.directories, 1);
			c->phase = survey ? COPY_WALK : COPY_DATA_WALK;
			break;
		}
		case COPY_WALK: copy_walk_step(c); break;
		case COPY_DEST: copy_name_step(c, false); break;
		case COPY_JOURNAL_NAME: copy_name_step(c, true); break;
		case COPY_JOURNAL: copy_journal_step(c); break;
		case COPY_CREATE: copy_create_step(c); break;
		case COPY_MARKER: copy_marker_step(c); break;
		case COPY_DATA_WALK: copy_data_walk(c); break;
		case COPY_FILE: copy_file_step(c, &bytes); break;
		case COPY_LEVEL_OPEN: case COPY_LEVEL_READ: case COPY_LEVEL_EDIT:
		case COPY_LEVEL_WRITE: copy_level_step(c, &bytes); break;
		case COPY_PUBLISH: copy_publish(c); break;
		case COPY_FINISH: copy_finish(c); break;
		case COPY_CLEAN_OPEN: case COPY_CLEAN_WALK: copy_cleanup_step(c); break;
		}
	}
	return c->result.state;
}

enum world_copy_error world_copy_destroy(struct world_copy* c) {
	if(!c) return WORLD_COPY_OK;
	if(c->result.state != WORLD_COPY_DONE) {
		if(c->result.state != WORLD_COPY_CLEANING)
			copy_begin_cleanup(c, c->result.state == WORLD_COPY_FAILED ? WORLD_COPY_FAILED : WORLD_COPY_CANCELLED);
		while(c->result.state == WORLD_COPY_CLEANING) world_copy_step(c, WORLD_COPY_STEP_MAX);
	}
	int error = copy_close_handles(c);
	if(c->recovery_directory && closedir(c->recovery_directory) != 0 && !error)
		error = errno ? errno : EIO;
	enum world_copy_error result = error || c->result.cleanup_error
		? WORLD_COPY_CLEANUP_ERROR : WORLD_COPY_OK;
	buffer_free(&c->metadata);
	free(c);
	return result;
}

void world_copy_release(struct world_copy* c) {
	if(!c) return;
	/* Output streams are unbuffered: close without flush/sync or fault retries. */
	if(c->output) fclose(c->output);
	if(c->input) fclose(c->input);
	if(c->recovery_directory) closedir(c->recovery_directory);
	for(unsigned i = 0; i <= WORLD_COPY_DEPTH_MAX; i++)
		if(c->frames[i].directory) closedir(c->frames[i].directory);
	buffer_free(&c->metadata);
	free(c);
}
