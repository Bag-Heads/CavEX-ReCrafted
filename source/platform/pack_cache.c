/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Generic pack-cache publication layer. Contract: pack_cache.h. */

#include "pack_cache.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define RECORD_MAGIC "CWAR"
#define RECORD_BYTES 40u

uint64_t pack_cache_fnv(uint64_t h, const void* data, size_t n) {
	const uint8_t* b = data;
	for(size_t k = 0; k < n; k++) {
		h ^= b[k];
		h *= UINT64_C(0x100000001b3);
	}
	return h;
}

/* The source-tree identity: CW_BUILD_ID is "<worktree-stamp>_<date>[-dev]"
   on both builds (Wii Makefile CFLAGS; CMake source property on this file),
   so the part before the LAST '_' is the tree stamp (git hash + dirty digest)
   -- identical for the PC tool and the console built from the same tree,
   unlike the whole id (date, -dev suffix). */
#ifndef CW_BUILD_ID
#define CW_BUILD_ID "unstamped"
#endif
static const char* code_identity(void) {
	static char id[96];
	if(!id[0]) {
		snprintf(id, sizeof(id), "%s", CW_BUILD_ID);
		char* cut = strrchr(id, '_');
		if(cut && cut != id)
			*cut = '\0';
	}
	return id;
}
static const char* s_code_identity_override;
#ifdef CAVEWII_TESTING
void pack_cache_test_set_code_identity(const char* id) {
	s_code_identity_override = id;
}
#endif

uint64_t pack_cache_fold_code_identity(uint64_t h) {
	const char* id = s_code_identity_override ? s_code_identity_override
											  : code_identity();
	h = pack_cache_fnv(h, "CODE", 4);
	return pack_cache_fnv(h, id, strlen(id) + 1);
}

uint64_t pack_cache_fnv_u32(uint64_t h, uint32_t v) {
	uint8_t b[4];
	pack_cache_put_u32(b, v);
	return pack_cache_fnv(h, b, 4);
}

void pack_cache_put_u32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

void pack_cache_put_u64(uint8_t* p, uint64_t v) {
	pack_cache_put_u32(p, (uint32_t)v);
	pack_cache_put_u32(p + 4, (uint32_t)(v >> 32));
}

uint32_t pack_cache_get_u32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
		| (uint32_t)p[3] << 24;
}

uint64_t pack_cache_get_u64(const uint8_t* p) {
	return (uint64_t)pack_cache_get_u32(p)
		| (uint64_t)pack_cache_get_u32(p + 4) << 32;
}

void pack_cache_gen_path(char* out, size_t size, const char* dir, uint32_t gen) {
	snprintf(out, size, "%s/g%08u.bin", dir, (unsigned)gen);
}

static void record_path(char* out, size_t size, const char* dir, int slot) {
	snprintf(out, size, "%s/commit-%c.rec", dir, slot ? 'b' : 'a');
}

static void record_encode(uint8_t* b, uint32_t schema,
						  const struct pack_cache_record* r) {
	memcpy(b, RECORD_MAGIC, 4);
	pack_cache_put_u32(b + 4, schema);
	pack_cache_put_u32(b + 8, r->generation);
	pack_cache_put_u64(b + 12, r->key);
	pack_cache_put_u32(b + 20, r->file_len);
	pack_cache_put_u64(b + 24, r->file_checksum);
	pack_cache_put_u64(b + 32, pack_cache_fnv(PACK_CACHE_FNV_INIT, b, 32));
}

static struct pack_cache_record record_read(const char* dir, uint32_t schema,
											int slot) {
	struct pack_cache_record r = {0};
	char path[512];
	record_path(path, sizeof(path), dir, slot);
	FILE* f = fopen(path, "rb");
	if(!f)
		return r;
	uint8_t b[RECORD_BYTES + 1];
	size_t got = fread(b, 1, sizeof(b), f);
	fclose(f);
	if(got != RECORD_BYTES || memcmp(b, RECORD_MAGIC, 4) != 0
	   || pack_cache_get_u32(b + 4) != schema
	   || pack_cache_get_u64(b + 32)
		   != pack_cache_fnv(PACK_CACHE_FNV_INIT, b, 32))
		return r; /* absent, torn, foreign or corrupt: ignored */
	r.generation = pack_cache_get_u32(b + 8);
	r.key = pack_cache_get_u64(b + 12);
	r.file_len = pack_cache_get_u32(b + 20);
	r.file_checksum = pack_cache_get_u64(b + 24);
	r.valid = r.generation != 0;
	return r;
}

void pack_cache_records(const char* dir, uint32_t schema,
						struct pack_cache_record out[2]) {
	out[0] = record_read(dir, schema, 0);
	out[1] = record_read(dir, schema, 1);
}

void pack_cache_order(const struct pack_cache_record r[2], int order[2]) {
	order[0] = 0;
	order[1] = 1;
	if(r[1].valid && (!r[0].valid || r[1].generation > r[0].generation)) {
		order[0] = 1;
		order[1] = 0;
	}
}

static bool make_dir(const char* path) {
	struct stat st;
	if(stat(path, &st) == 0)
		return S_ISDIR(st.st_mode);
#ifdef _WIN32
	if(_mkdir(path) == 0)
		return true;
#else
	if(mkdir(path, 0777) == 0)
		return true;
#endif
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool pack_cache_ensure_dir(const char* dir) {
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "%s", dir);
	if(n <= 0 || (size_t)n >= sizeof(buf))
		return false;
	for(char* p = buf + 1; *p; p++) {
		if(*p != '/')
			continue;
		if(p[-1] == ':')
			continue; /* "sd:/" -- the device root is not a directory to make */
		*p = '\0';
		bool ok = make_dir(buf);
		*p = '/';
		if(!ok)
			return false;
	}
	return make_dir(buf);
}

static bool parse_gen_name(const char* name, unsigned* g) {
	char tail[8] = {0};
	return strlen(name) == 13 && sscanf(name, "g%8u%4s", g, tail) == 2
		&& strcmp(tail, ".bin") == 0;
}

static uint32_t max_gen_file(const char* dir) {
	uint32_t best = 0;
	DIR* d = opendir(dir);
	if(!d)
		return 0;
	struct dirent* e;
	while((e = readdir(d))) {
		unsigned g = 0;
		if(parse_gen_name(e->d_name, &g) && g > best)
			best = g;
	}
	closedir(d);
	return best;
}

static void prune(const char* dir, uint32_t keep_a, uint32_t keep_b) {
	uint32_t victims[64];
	int n = 0;
	DIR* d = opendir(dir);
	if(!d)
		return;
	struct dirent* e;
	while((e = readdir(d)) && n < 64) {
		unsigned g = 0;
		if(parse_gen_name(e->d_name, &g) && g != keep_a && g != keep_b)
			victims[n++] = g;
	}
	closedir(d);
	for(int k = 0; k < n; k++) {
		char path[512];
		pack_cache_gen_path(path, sizeof(path), dir, victims[k]);
		remove(path);
	}
}

bool pack_cache_begin(struct pack_cache_txn* t, const char* dir,
					  uint32_t schema) {
	memset(t, 0, sizeof(*t));
	if(snprintf(t->dir, sizeof(t->dir), "%s", dir) >= (int)sizeof(t->dir)
	   || !pack_cache_ensure_dir(dir))
		return false;
	struct pack_cache_record r[2];
	pack_cache_records(dir, schema, r);
	int order[2];
	pack_cache_order(r, order);
	t->cur_slot = r[order[0]].valid ? order[0] : -1;
	t->prev_generation = t->cur_slot >= 0 ? r[t->cur_slot].generation : 0;
	uint32_t gen = max_gen_file(dir);
	for(int s = 0; s < 2; s++)
		if(r[s].valid && r[s].generation > gen)
			gen = r[s].generation;
	gen++;
	if(gen == 0)
		return false; /* 2^32 generations: refuse rather than reuse */
	t->schema = schema;
	t->generation = gen;
	pack_cache_gen_path(t->path, sizeof(t->path), dir, gen);
	t->f = fopen(t->path, "wb");
	t->h = PACK_CACHE_FNV_INIT;
	t->ok = t->f != NULL;
	return t->ok;
}

void pack_cache_put(struct pack_cache_txn* t, const void* data, size_t n) {
	if(!t->ok || !n)
		return;
	if(fwrite(data, 1, n, t->f) != n) {
		t->ok = false;
		return;
	}
	t->h = pack_cache_fnv(t->h, data, n);
	t->n += n;
}

void pack_cache_abort(struct pack_cache_txn* t) {
	if(t->f)
		fclose(t->f);
	t->f = NULL;
	if(t->path[0])
		remove(t->path);
	t->ok = false;
}

bool pack_cache_commit(struct pack_cache_txn* t, uint64_t key) {
	if(!t->f) {
		t->ok = false;
		return false;
	}
	uint8_t tail[8];
	uint64_t checksum = t->h;
	pack_cache_put_u64(tail, checksum);
	if(t->ok && fwrite(tail, 1, 8, t->f) != 8)
		t->ok = false;
	uint64_t total = t->n + 8;
	if(fclose(t->f) != 0)
		t->ok = false;
	t->f = NULL;
	if(!t->ok || total > UINT32_MAX) {
		remove(t->path); /* a partial generation is never committed */
		return false;
	}
	struct pack_cache_record nr = {.valid = true, .generation = t->generation,
								   .key = key, .file_len = (uint32_t)total,
								   .file_checksum = checksum};
	uint8_t rb[RECORD_BYTES];
	record_encode(rb, t->schema, &nr);
	int slot = t->cur_slot == 0 ? 1 : 0;
	char rpath[512];
	record_path(rpath, sizeof(rpath), t->dir, slot);
	FILE* rf = fopen(rpath, "wb");
	bool rok = rf && fwrite(rb, 1, sizeof(rb), rf) == sizeof(rb);
	if(rf && fclose(rf) != 0)
		rok = false;
	if(!rok) {
		/* The previous record (other slot) stays in force; a torn record
		   here fails its own checksum. */
		remove(t->path);
		return false;
	}
	prune(t->dir, t->generation, t->prev_generation);
	return true;
}

const char* pack_cache_read_open(struct pack_cache_reader* r, const char* dir,
								 const struct pack_cache_record* rec) {
	memset(r, 0, sizeof(*r));
	char path[512];
	pack_cache_gen_path(path, sizeof(path), dir, rec->generation);
	r->f = fopen(path, "rb");
	if(!r->f)
		return "generation file missing";
	long len = -1;
	if(fseek(r->f, 0, SEEK_END) == 0)
		len = ftell(r->f);
	if(len < 8 || (uint32_t)len != rec->file_len
	   || fseek(r->f, 0, SEEK_SET) != 0) {
		pack_cache_read_close(r);
		return "generation length differs from its commit record";
	}
	r->h = PACK_CACHE_FNV_INIT;
	r->remaining = (uint64_t)len - 8u;
	r->rec = rec;
	return NULL;
}

bool pack_cache_read(struct pack_cache_reader* r, void* dst, size_t n) {
	if(!r->f || n > r->remaining || fread(dst, 1, n, r->f) != n)
		return false;
	r->h = pack_cache_fnv(r->h, dst, n);
	r->remaining -= n;
	return true;
}

const char* pack_cache_read_finish(struct pack_cache_reader* r) {
	if(!r->f)
		return "not open";
	if(r->remaining)
		return "trailing bytes before the checksum";
	uint8_t tail[9];
	if(fread(tail, 1, sizeof(tail), r->f) != 8)
		return "missing checksum";
	if(pack_cache_get_u64(tail) != r->h || r->h != r->rec->file_checksum)
		return "payload checksum mismatch";
	return NULL;
}

void pack_cache_read_close(struct pack_cache_reader* r) {
	if(r->f)
		fclose(r->f);
	r->f = NULL;
}
