/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Pack digest manifest. Contract: pack_manifest.h. */

#include "pack_manifest.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pack_cache.h"

#define MANIFEST_HEAD "CWMANIFEST 1\n"
#define MANIFEST_MAX_BYTES (1u << 20)
#define MANIFEST_MAX_FILES 8192

static bool s_test_fail_alloc;
#ifdef CAVEWII_TESTING
void pack_manifest_test_fail_alloc(bool fail) {
	s_test_fail_alloc = fail;
}
#endif

/* Reads and verifies the manifest. Returns the text (caller frees) with
   *end at the "end" line, or NULL when absent/untrusted. */
static char* manifest_read(const char* pack_root, struct pack_manifest* out,
						   char** end_out) {
	memset(out, 0, sizeof(*out));
	out->status = PACK_MANIFEST_ABSENT;
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", pack_root, PACK_MANIFEST_NAME);
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;
	/* Fable M2: size the buffer by the file (a few KB for a real pack), not
	   a fixed 1 MiB on the Wii's MEM1 heap. */
	long len = -1;
	if(fseek(f, 0, SEEK_END) == 0)
		len = ftell(f);
	if(len <= 0 || len > (long)MANIFEST_MAX_BYTES
	   || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		out->status = PACK_MANIFEST_UNTRUSTED; /* empty/oversized: unmanaged */
		return NULL;
	}
	char* buf = s_test_fail_alloc ? NULL : malloc((size_t)len + 1);
	size_t got = buf ? fread(buf, 1, (size_t)len, f) : 0;
	fclose(f);
	if(!buf || got != (size_t)len) {
		free(buf);
		out->status = PACK_MANIFEST_ERROR; /* unreadable, not unmanaged */
		return NULL;
	}
	buf[got] = '\0';
	bool ok = strncmp(buf, MANIFEST_HEAD, strlen(MANIFEST_HEAD)) == 0;
	/* The end line is the last line; everything before it is hashed. */
	char* end = NULL;
	for(char* p = buf; ok && (p = strstr(p, "\nend ")); p++)
		end = p + 1;
	unsigned long long want = 0;
	if(ok && end && sscanf(end, "end %16llx", &want) == 1) {
		uint64_t h = pack_cache_fnv(PACK_CACHE_FNV_INIT, buf,
									(size_t)(end - buf));
		if(h == (uint64_t)want) {
			uint32_t lines = 0;
			for(char* p = buf + strlen(MANIFEST_HEAD); p < end; p++)
				if(*p == '\n')
					lines++;
			out->trusted = lines > 0;
			out->digest = h;
			out->files = lines;
		}
	}
	if(!out->trusted) {
		free(buf);
		out->status = PACK_MANIFEST_UNTRUSTED;
		return NULL;
	}
	out->status = PACK_MANIFEST_TRUSTED;
	/* The audio policy's question, answered from the same buffer. */
	for(char* line = buf + strlen(MANIFEST_HEAD); line < end;) {
		char* nl = strchr(line, '\n');
		if(!nl || nl > end)
			break;
		char* sp1 = memchr(line, ' ', (size_t)(nl - line));
		char* sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(nl - sp1 - 1)) : NULL;
		if(sp2) {
			const char* rel = sp2 + 1;
			size_t n = (size_t)(nl - rel);
			if(n >= 10 && strncmp(rel, "sound/", 6) == 0
			   && strncmp(nl - 4, ".ogg", 4) == 0)
				out->sound_ogg++;
		}
		line = nl + 1;
	}
	*end_out = end;
	return buf;
}

bool pack_manifest_load(const char* pack_root, struct pack_manifest* out) {
	char* end;
	char* buf = manifest_read(pack_root, out, &end);
	free(buf);
	return out->trusted;
}

uint32_t pack_manifest_count(const char* pack_root, const char* prefix,
							 const char* suffix) {
	struct pack_manifest m;
	char* end;
	char* buf = manifest_read(pack_root, &m, &end);
	if(!buf)
		return 0;
	uint32_t n = 0;
	size_t np = strlen(prefix), ns = strlen(suffix);
	char* line = buf + strlen(MANIFEST_HEAD);
	while(line < end) {
		char* nl = strchr(line, '\n');
		if(!nl || nl > end)
			break;
		/* "<hex16> <size> <relpath>" */
		char* sp1 = memchr(line, ' ', (size_t)(nl - line));
		char* sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(nl - sp1 - 1)) : NULL;
		if(sp2) {
			char* rel = sp2 + 1;
			size_t len = (size_t)(nl - rel);
			if(len >= np + ns && strncmp(rel, prefix, np) == 0
			   && strncmp(nl - ns, suffix, ns) == 0)
				n++;
		}
		line = nl + 1;
	}
	free(buf);
	return n;
}

/* ---- producer ---- */

struct walk {
	char** paths;
	size_t n, cap;
	bool ok;
};

static void walk_add(struct walk* w, const char* rel) {
	if(!w->ok)
		return;
	if(w->n == w->cap) {
		size_t ncap = w->cap ? w->cap * 2 : 256;
		if(ncap > MANIFEST_MAX_FILES) {
			w->ok = false;
			return;
		}
		char** np = realloc(w->paths, ncap * sizeof(*np));
		if(!np) {
			w->ok = false;
			return;
		}
		w->paths = np;
		w->cap = ncap;
	}
	w->paths[w->n] = malloc(strlen(rel) + 1);
	if(!w->paths[w->n]) {
		w->ok = false;
		return;
	}
	strcpy(w->paths[w->n++], rel);
}

static void walk_dir(struct walk* w, const char* root, const char* rel) {
	char dir[600];
	snprintf(dir, sizeof(dir), rel[0] ? "%s/%s" : "%s%s", root, rel);
	DIR* d = opendir(dir);
	if(!d) {
		w->ok = false;
		return;
	}
	struct dirent* e;
	while(w->ok && (e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if(!rel[0] && !strcmp(e->d_name, ".cavewii"))
			continue; /* derived data is never part of the pack */
		char sub[400];
		int n = rel[0] ? snprintf(sub, sizeof(sub), "%s/%s", rel, e->d_name)
					   : snprintf(sub, sizeof(sub), "%s", e->d_name);
		if(n <= 0 || (size_t)n >= sizeof(sub)) {
			w->ok = false;
			break;
		}
		char full[800];
		snprintf(full, sizeof(full), "%s/%s", root, sub);
		struct stat st;
		if(stat(full, &st) != 0) {
			w->ok = false;
			break;
		}
		if(S_ISDIR(st.st_mode))
			walk_dir(w, root, sub);
		else if(S_ISREG(st.st_mode))
			walk_add(w, sub);
	}
	closedir(d);
}

static int cmp_str(const void* a, const void* b) {
	return strcmp(*(char* const*)a, *(char* const*)b);
}

bool pack_manifest_write(const char* pack_root, struct pack_manifest* out) {
	memset(out, 0, sizeof(*out));
	struct walk w = {.ok = true};
	walk_dir(&w, pack_root, "");
	if(w.ok)
		qsort(w.paths, w.n, sizeof(*w.paths), cmp_str);
	char dir[512], tmp[560], final[560];
	snprintf(dir, sizeof(dir), "%s/.cavewii", pack_root);
	snprintf(tmp, sizeof(tmp), "%s/manifest.tmp", dir);
	snprintf(final, sizeof(final), "%s/%s", pack_root, PACK_MANIFEST_NAME);
	FILE* f = w.ok && pack_cache_ensure_dir(dir) ? fopen(tmp, "wb") : NULL;
	bool ok = f != NULL && w.n > 0;
	uint64_t h = PACK_CACHE_FNV_INIT;
	if(ok) {
		ok = fputs(MANIFEST_HEAD, f) >= 0;
		h = pack_cache_fnv(h, MANIFEST_HEAD, strlen(MANIFEST_HEAD));
	}
	static uint8_t buf[4096];
	for(size_t k = 0; ok && k < w.n; k++) {
		char full[800];
		snprintf(full, sizeof(full), "%s/%s", pack_root, w.paths[k]);
		FILE* in = fopen(full, "rb");
		if(!in) {
			ok = false;
			break;
		}
		uint64_t fh = PACK_CACHE_FNV_INIT;
		unsigned long long size = 0;
		size_t got;
		while((got = fread(buf, 1, sizeof(buf), in)) > 0) {
			fh = pack_cache_fnv(fh, buf, got);
			size += got;
		}
		fclose(in);
		char line[600];
		int n = snprintf(line, sizeof(line), "%016llx %llu %s\n",
						 (unsigned long long)fh, size, w.paths[k]);
		if(n <= 0 || (size_t)n >= sizeof(line) || fputs(line, f) < 0) {
			ok = false;
			break;
		}
		h = pack_cache_fnv(h, line, (size_t)n);
	}
	if(ok) {
		char tail[40];
		snprintf(tail, sizeof(tail), "end %016llx\n", (unsigned long long)h);
		ok = fputs(tail, f) >= 0;
	}
	if(f && fclose(f) != 0)
		ok = false;
	if(ok) {
		remove(final);
		ok = rename(tmp, final) == 0;
	}
	if(!ok && f)
		remove(tmp);
	if(ok) {
		out->trusted = true;
		out->digest = h;
		out->files = (uint32_t)w.n;
	}
	for(size_t k = 0; k < w.n; k++)
		free(w.paths[k]);
	free(w.paths);
	return ok;
}
