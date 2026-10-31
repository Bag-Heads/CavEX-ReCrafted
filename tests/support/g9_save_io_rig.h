/* Copyright (c) 2026 CaveWii_v2 contributors
 * G9 R19 (ASTRA-G9-R19-IO) save-IO rig, shared by test_g9_save_io_equiv and
 * test_g9_save_io_durability:
 *   - three ARMS: 0 = every publication inline on the server thread (the
 *     synchronous reference, region_archive_io_async_enable(false)), 1 = the
 *     shipping default (round-5 helper publishes, payload writes inline),
 *     2 = experimental.g9_save_io_int (deferred IO: every region-file write
 *     on the helper as immutable ops);
 *   - an ordered PER-FILE mutation trace (region_archive_test_trace), so two
 *     arms are compared op by op for every file, with the save root stripped;
 *   - a full save-directory SNAPSHOT: every file under the root, relative
 *     path + size + FNV-1a-64 over ALL bytes (no timestamp masking -- the
 *     publication stamp is pinned with region_archive_test_clock instead);
 *   - directory COPY (the durability test's power-cut images).
 * Header-only, static inline (tests/support/test_fixture_dir.h precedent). */
#ifndef TESTS_G9_SAVE_IO_RIG_H
#define TESTS_G9_SAVE_IO_RIG_H

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define G9IO_MKDIR(p) _mkdir(p)
#else
#define G9IO_MKDIR(p) mkdir(p, 0777)
#endif

#include "network/region_archive.h"
#include "platform/thread.h"

#define G9IO_FNV_BASIS UINT64_C(14695981039346656037)
#define G9IO_FNV_PRIME UINT64_C(1099511628211)
static inline uint64_t g9io_fnv(uint64_t h, const void* data, size_t n) {
	const uint8_t* p = data;
	while(n--) {
		h ^= *p++;
		h *= G9IO_FNV_PRIME;
	}
	return h;
}

/* ---- arms --------------------------------------------------------------- */
static const char* const G9IO_ARM_NAME[3] = {"inline", "helper", "deferred"};
static inline void g9io_arm(int arm) {
	region_archive_deferred_io_enable(arm == 2);
	region_archive_io_async_enable(arm != 0);
}

/* ---- the per-file trace ----------------------------------------------- */
#define G9IO_TRACE_FILES 48
#define G9IO_TRACE_OPS 1024
struct g9io_trace_file {
	char path[160];
	uint32_t n;
	uint64_t hash;
	uint32_t op[G9IO_TRACE_OPS];
	uint64_t a[G9IO_TRACE_OPS], b[G9IO_TRACE_OPS];
};
struct g9io_trace {
	char root[256];
	size_t root_len;
	size_t files;
	struct g9io_trace_file file[G9IO_TRACE_FILES];
	uint32_t total;
	uint32_t persist_points; /* fsync / rename / remove transitions */
};
static struct g9io_trace* g9io_cur;
static struct thread_mutex g9io_trace_lock;
static bool g9io_trace_lock_ready;
/* Called (on the mutating thread, possibly the IO worker) after a
 * persistence transition; the durability test snapshots the disk here. */
static void (*g9io_point_cb)(int op, const char* rel);

static inline bool g9io_is_persist_point(int op) {
	return op == RT_SYNC_REGION || op == RT_JOURNAL || op == RT_SECTORS
		|| op == RT_RETIRE || op == RT_ROLLBACK || op == RT_JOURNAL_OPEN
		|| op == RT_JOURNAL_REMOVE || op == RT_LEVEL_SYNC
		|| op == RT_LEVEL_ROTATE;
}
static inline void g9io_hook(int op, const char* path, uint64_t a,
							 uint64_t b) {
	struct g9io_trace* t = g9io_cur;
	if(!t)
		return;
	thread_mutex_lock(&g9io_trace_lock);
	const char* rel = path;
	if(!strncmp(path, t->root, t->root_len))
		rel = path + t->root_len;
	struct g9io_trace_file* f = NULL;
	for(size_t k = 0; k < t->files; k++)
		if(!strcmp(t->file[k].path, rel)) {
			f = &t->file[k];
			break;
		}
	if(!f) {
		assert(t->files < G9IO_TRACE_FILES);
		f = &t->file[t->files++];
		memset(f, 0, sizeof(*f));
		snprintf(f->path, sizeof f->path, "%s", rel);
		f->hash = G9IO_FNV_BASIS;
	}
	if(f->n < G9IO_TRACE_OPS) {
		f->op[f->n] = (uint32_t)op;
		f->a[f->n] = a;
		f->b[f->n] = b;
	}
	f->n++;
	uint64_t rec[3] = {(uint64_t)op, a, b};
	f->hash = g9io_fnv(f->hash, rec, sizeof rec);
	t->total++;
	bool point = g9io_is_persist_point(op);
	if(point)
		t->persist_points++;
	thread_mutex_unlock(&g9io_trace_lock);
	if(point && g9io_point_cb)
		g9io_point_cb(op, rel);
}
static inline void g9io_trace_begin(struct g9io_trace* t, const char* root) {
	if(!g9io_trace_lock_ready) {
		assert(thread_mutex_init(&g9io_trace_lock));
		g9io_trace_lock_ready = true;
	}
	memset(t, 0, sizeof(*t));
	snprintf(t->root, sizeof t->root, "%s", root);
	t->root_len = strlen(t->root);
	g9io_cur = t;
	region_archive_test_trace(g9io_hook);
}
static inline void g9io_trace_end(void) {
	region_archive_test_trace(NULL);
	g9io_cur = NULL;
}
static inline const struct g9io_trace_file* g9io_trace_find(
	const struct g9io_trace* t, const char* rel) {
	for(size_t k = 0; k < t->files; k++)
		if(!strcmp(t->file[k].path, rel))
			return &t->file[k];
	return NULL;
}
/* First per-file divergence between two traces; prints it. */
static inline bool g9io_trace_equal(const struct g9io_trace* x,
									const struct g9io_trace* y,
									const char* label) {
	bool ok = x->files == y->files;
	if(!ok)
		printf("  [%s] trace file count %zu != %zu\n", label, x->files,
			   y->files);
	for(size_t k = 0; k < x->files; k++) {
		const struct g9io_trace_file* a = &x->file[k];
		const struct g9io_trace_file* b = g9io_trace_find(y, a->path);
		if(!b) {
			printf("  [%s] %s traced in one arm only\n", label, a->path);
			ok = false;
			continue;
		}
		if(a->n == b->n && a->hash == b->hash)
			continue;
		ok = false;
		uint32_t n = a->n < b->n ? a->n : b->n;
		if(n > G9IO_TRACE_OPS)
			n = G9IO_TRACE_OPS;
		uint32_t i = 0;
		while(i < n && a->op[i] == b->op[i] && a->a[i] == b->a[i]
			  && a->b[i] == b->b[i])
			i++;
		printf("  [%s] %s: %u vs %u ops, first divergence at op %u", label,
			   a->path, a->n, b->n, i);
		if(i < n)
			printf(" (op %u/%u a %" PRIx64 "/%" PRIx64 " b %" PRIx64
				   "/%" PRIx64 ")",
				   a->op[i], b->op[i], a->a[i], b->a[i], a->b[i], b->b[i]);
		printf("\n");
		if(getenv("G9IO_TRACE_TAIL")) {
			uint32_t lo = i > 6 ? i - 6 : 0;
			for(uint32_t j = lo; j < a->n && j < i + 4 && j < G9IO_TRACE_OPS; j++)
				printf("     A[%u] op %u a %" PRIx64 " b %" PRIx64 "\n", j,
					   a->op[j], a->a[j], a->b[j]);
			for(uint32_t j = lo; j < b->n && j < i + 4 && j < G9IO_TRACE_OPS; j++)
				printf("     B[%u] op %u a %" PRIx64 " b %" PRIx64 "\n", j,
					   b->op[j], b->a[j], b->b[j]);
		}
	}
	return ok;
}

/* ---- the directory snapshot -------------------------------------------- */
#define G9IO_SNAP_FILES 96
struct g9io_snap {
	size_t n;
	struct {
		char path[160];
		uint64_t size;
		uint64_t hash;
	} f[G9IO_SNAP_FILES];
};
static inline int g9io_path_cmp(const void* a, const void* b) {
	return strcmp((const char*)a, (const char*)b);
}
static inline void g9io_snap_walk(struct g9io_snap* s, const char* root,
								  const char* rel) {
	char dir[512];
	snprintf(dir, sizeof dir, "%s%s", root, rel);
	DIR* d = opendir(dir);
	if(!d)
		return;
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char child_rel[256], full[640];
		snprintf(child_rel, sizeof child_rel, "%s/%s", rel, e->d_name);
		snprintf(full, sizeof full, "%s%s", root, child_rel);
		struct stat st;
		if(stat(full, &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode)) {
			g9io_snap_walk(s, root, child_rel);
			continue;
		}
		assert(s->n < G9IO_SNAP_FILES);
		snprintf(s->f[s->n].path, sizeof s->f[s->n].path, "%s", child_rel);
		FILE* fp = fopen(full, "rb");
		uint64_t h = G9IO_FNV_BASIS, size = 0;
		if(fp) {
			uint8_t buf[4096];
			size_t got;
			while((got = fread(buf, 1, sizeof buf, fp)) > 0) {
				h = g9io_fnv(h, buf, got);
				size += got;
			}
			fclose(fp);
		}
		s->f[s->n].size = size;
		s->f[s->n].hash = h;
		s->n++;
	}
	closedir(d);
}
static inline void g9io_snap_take(struct g9io_snap* s, const char* root) {
	memset(s, 0, sizeof(*s));
	g9io_snap_walk(s, root, "");
	qsort(s->f, s->n, sizeof(s->f[0]), g9io_path_cmp);
}
static inline bool g9io_snap_equal(const struct g9io_snap* a,
								   const struct g9io_snap* b,
								   const char* label) {
	bool ok = a->n == b->n;
	if(!ok)
		printf("  [%s] file count %zu != %zu\n", label, a->n, b->n);
	size_t n = a->n < b->n ? a->n : b->n;
	for(size_t k = 0; k < n; k++)
		if(strcmp(a->f[k].path, b->f[k].path) || a->f[k].size != b->f[k].size
		   || a->f[k].hash != b->f[k].hash) {
			printf("  [%s] %s (%" PRIu64 " B %016" PRIx64 ") vs %s (%" PRIu64
				   " B %016" PRIx64 ")\n",
				   label, a->f[k].path, a->f[k].size, a->f[k].hash,
				   b->f[k].path, b->f[k].size, b->f[k].hash);
			ok = false;
		}
	return ok;
}
static inline uint64_t g9io_snap_digest(const struct g9io_snap* s) {
	uint64_t h = G9IO_FNV_BASIS;
	for(size_t k = 0; k < s->n; k++) {
		h = g9io_fnv(h, s->f[k].path, strlen(s->f[k].path));
		h = g9io_fnv(h, &s->f[k].size, sizeof s->f[k].size);
		h = g9io_fnv(h, &s->f[k].hash, sizeof s->f[k].hash);
	}
	return h;
}

/* ---- directory copy (power-cut image) ---------------------------------- */
static inline bool g9io_copy_file(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	if(!in)
		return false;
	FILE* out = fopen(to, "wb");
	if(!out) {
		fclose(in);
		return false;
	}
	uint8_t buf[8192];
	size_t got;
	bool ok = true;
	while((got = fread(buf, 1, sizeof buf, in)) > 0)
		ok = ok && fwrite(buf, 1, got, out) == got;
	fclose(in);
	if(fclose(out))
		ok = false;
	return ok;
}
static inline void g9io_copy_tree(const char* from, const char* to) {
	G9IO_MKDIR(to);
	DIR* d = opendir(from);
	if(!d)
		return;
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char src[640], dst[640];
		snprintf(src, sizeof src, "%s/%s", from, e->d_name);
		snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
		struct stat st;
		if(stat(src, &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode))
			g9io_copy_tree(src, dst);
		else
			(void)g9io_copy_file(src, dst);
	}
	closedir(d);
}

#endif
