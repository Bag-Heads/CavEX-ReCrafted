/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* In-game Beta importer, part A1: worker thread, staging, manifest,
 * validation, atomic swap, power-cut recovery, selection hand-off.
 * Contract: beta_import.h; tmp/jobs/ingame-import-1030/CONTRACT.md.
 *
 * On-disk protocol (all inside texturepacks/, whose scanner hides dot
 * folders, so the game never lists a partial pack):
 *   1. build everything in .beta-import-tmp/, write .cavewii-pack.json LAST
 *      (via a temp name + rename): a staging folder WITH a manifest is a
 *      complete, validated pack; without one it is garbage;
 *   2. swap: "Beta 1.7.3" -> .beta-import-old, .beta-import-tmp ->
 *      "Beta 1.7.3", delete .beta-import-old;
 *   3. only then (main thread, poll) is the pack selected.
 * A power cut at any point leaves either the previous pack or the complete
 * new one under "Beta 1.7.3"; bi_recover() (boot, and before every import)
 * finishes the one interrupted case -- the gap between the two renames --
 * and deletes leftovers. The selection never points at a half pack.
 *
 * The folder is "Beta-1.7.3" (no space: the rig cannot stage spaced names
 * and the hardware path for them is unproven); the list shows the manifest
 * name "Beta 1.7.3" (texturepacks_display_name). Any spelling of the
 * previous import ("beta-1.7.3" from older helpers is the same folder on
 * FAT) is replaced whole, never merged into. One the importer/helper did not
 * write (no manifest) is never deleted: it is renamed "Beta-1.7.3-old"
 * ("-old2"...). An earlier "Beta 1.7.3" import (with manifest) is hidden
 * as .beta-import-legacy right after the new pack is selected and deleted
 * by the next recovery -- never before, so a cut cannot leave the
 * selection pointing at a deleted folder. */

#include "beta_import_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> /* rmdir */

#include "thread.h"
#include "../util.h"

#ifdef _WIN32
#include <direct.h>
#define bi_mkdir(p) _mkdir(p)
#else
#define bi_mkdir(p) mkdir((p), 0777)
#endif

/* Below the server (63), region I/O (48) and mesher (32): the import only
 * uses time nobody else wants, so it can never starve the main thread or the
 * watchdog's feeders (libogc: higher number = higher priority). */
#define BI_THREAD_PRIORITY 16
#define BI_MANIFEST_MAX_FILES 8192 /* helper DEFAULT_LIMITS.files */

/* ---- sounds hook: coder B's beta_import_sound.c overrides this ---------- */

static bool s_sound_stub_ran;

__attribute__((weak)) int bi_sounds_import(const char* resources_dir,
										   const char* staging_dir,
										   bi_ctx* ctx) {
	(void)resources_dir;
	(void)staging_dir;
	(void)ctx;
	s_sound_stub_ran = true; /* "sounds: not yet" (B not linked) */
	return 0;
}

/* ---- small fs helpers ----------------------------------------------------- */

static bool is_dir(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool join(char* out, size_t len, const char* a, const char* b) {
	int n = snprintf(out, len, "%s/%s", a, b);
	return n >= 0 && (size_t)n < len;
}

bool bi_mkdirs(const char* path) {
	char buf[BI_PATH_MAX];
	if(snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
		return false;
	for(char* p = buf + 1; *p; p++) {
		if(*p != '/')
			continue;
		if(p[-1] == ':') /* "sd:/" device root */
			continue;
		*p = '\0';
		if(!is_dir(buf))
			bi_mkdir(buf);
		*p = '/';
	}
	if(!is_dir(buf))
		bi_mkdir(buf);
	return is_dir(buf);
}

bool bi_rmtree(const char* path) {
	if(!is_dir(path))
		return remove(path) == 0 || !is_file(path);
	DIR* d = opendir(path);
	if(!d)
		return false;
	bool ok = true;
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char child[BI_PATH_MAX];
		if(!join(child, sizeof(child), path, e->d_name)) {
			ok = false;
			continue;
		}
		if(is_dir(child))
			ok = bi_rmtree(child) && ok;
		else
			ok = remove(child) == 0 && ok;
	}
	closedir(d);
	return rmdir(path) == 0 && ok;
}

/* ---- SHA-256 (FIPS 180-4) -------------------------------------------------- */

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(bi_sha256* s, const uint8_t* p) {
	uint32_t w[64];
	for(int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16)
			| ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
	for(int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4],
			 f = s->h[5], g = s->h[6], h = s->h[7];
	for(int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25))
			+ ((e & f) ^ (~e & g)) + K256[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22))
			+ ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
	s->h[5] += f;
	s->h[6] += g;
	s->h[7] += h;
}

void bi_sha256_init(bi_sha256* s) {
	static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
								   0xa54ff53a, 0x510e527f, 0x9b05688c,
								   0x1f83d9ab, 0x5be0cd19};
	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->fill = 0;
}

void bi_sha256_update(bi_sha256* s, const void* data, size_t n) {
	const uint8_t* p = data;
	s->len += n;
	while(n) {
		size_t take = 64 - s->fill < n ? 64 - s->fill : n;
		memcpy(s->buf + s->fill, p, take);
		s->fill += take;
		p += take;
		n -= take;
		if(s->fill == 64) {
			sha256_block(s, s->buf);
			s->fill = 0;
		}
	}
}

void bi_sha256_final(bi_sha256* s, uint8_t out[32]) {
	uint64_t bits = s->len * 8;
	uint8_t pad = 0x80;
	bi_sha256_update(s, &pad, 1);
	pad = 0;
	while(s->fill != 56)
		bi_sha256_update(s, &pad, 1);
	uint8_t lenb[8];
	for(int i = 0; i < 8; i++)
		lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
	bi_sha256_update(s, lenb, 8);
	for(int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

/* ---- progress (shared with the main thread) ------------------------------- */

static void ctx_lock(bi_ctx* ctx) {
	if(ctx->lock)
		thread_mutex_lock(ctx->lock);
}
static void ctx_unlock(bi_ctx* ctx) {
	if(ctx->lock)
		thread_mutex_unlock(ctx->lock);
}

void bi_progress_set(bi_ctx* ctx, bi_state state, int done, int total,
					 const char* msg) {
	ctx_lock(ctx);
	ctx->p.state = state;
	ctx->p.done = done;
	ctx->p.total = total;
	if(msg)
		snprintf(ctx->p.msg, sizeof(ctx->p.msg), "%s", msg);
	ctx_unlock(ctx);
}

void bi_progress_step(bi_ctx* ctx, int done) {
	ctx_lock(ctx);
	ctx->p.done = done;
	ctx_unlock(ctx);
}

bool bi_cancel_requested(bi_ctx* ctx) {
	return ctx->cancel;
}

/* ---- paths, jar discovery ---------------------------------------------- */

void bi_default_paths(struct bi_paths* out) {
	memset(out, 0, sizeof(*out));
#ifdef PLATFORM_WII
	/* SD-root anchored whatever the launch CWD (screen_texturepacks.c). */
	path_from_sd_root("beta", out->beta_dir, sizeof(out->beta_dir));
	path_from_sd_root("beta/resources", out->resources_dir,
					  sizeof(out->resources_dir));
	path_from_sd_root("texturepacks", out->packs_dir, sizeof(out->packs_dir));
#else
	snprintf(out->beta_dir, sizeof(out->beta_dir), "beta");
	snprintf(out->resources_dir, sizeof(out->resources_dir), "beta/resources");
	snprintf(out->packs_dir, sizeof(out->packs_dir), "texturepacks");
#endif
	/* The bundled default pack, resolved exactly as texture.c's
	 * TEX_DEFAULT_PACK (raw-jar compose reads the same files). */
	snprintf(out->assets_dir, sizeof(out->assets_dir), "assets");
}

static bool ends_with_jar(const char* name) {
	size_t n = strlen(name);
	if(n < 5)
		return false;
	const char* x = name + n - 4;
	return x[0] == '.' && (x[1] | 32) == 'j' && (x[2] | 32) == 'a'
		&& (x[3] | 32) == 'r';
}

bool bi_find_jar(const struct bi_paths* paths, char* out, size_t out_len) {
	char preferred[BI_PATH_MAX];
	if(join(preferred, sizeof(preferred), paths->beta_dir, "b1.7.3.jar")
	   && is_file(preferred)) {
		snprintf(out, out_len, "%s", preferred);
		return true;
	}
	DIR* d = opendir(paths->beta_dir);
	if(!d)
		return false;
	char best[256] = "";
	struct dirent* e;
	while((e = readdir(d))) {
		if(e->d_name[0] == '.' || !ends_with_jar(e->d_name)
		   || strlen(e->d_name) >= sizeof(best))
			continue;
		char full[BI_PATH_MAX];
		if(!join(full, sizeof(full), paths->beta_dir, e->d_name)
		   || !is_file(full))
			continue;
		if(!best[0] || strcmp(e->d_name, best) < 0)
			memcpy(best, e->d_name, strlen(e->d_name) + 1);
	}
	closedir(d);
	if(!best[0])
		return false;
	int n = snprintf(out, out_len, "%s/%s", paths->beta_dir, best);
	return n >= 0 && (size_t)n < out_len;
}

/* ---- manifest (helper core._prepare's .cavewii-pack.json) ------------------ */

struct file_list {
	char** rel;
	size_t count, cap;
};

static bool list_files(const char* root, const char* rel, struct file_list* fl) {
	char dir[BI_PATH_MAX];
	if(rel[0] ? !join(dir, sizeof(dir), root, rel)
			  : snprintf(dir, sizeof(dir), "%s", root) >= (int)sizeof(dir))
		return false;
	DIR* d = opendir(dir);
	if(!d)
		return false;
	bool ok = true;
	struct dirent* e;
	while(ok && (e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char sub[BI_PATH_MAX], full[BI_PATH_MAX];
		if(rel[0] ? !join(sub, sizeof(sub), rel, e->d_name)
				  : snprintf(sub, sizeof(sub), "%s", e->d_name)
					  >= (int)sizeof(sub)) {
			ok = false;
			break;
		}
		if(!join(full, sizeof(full), root, sub)) {
			ok = false;
			break;
		}
		if(is_dir(full)) {
			ok = list_files(root, sub, fl);
			continue;
		}
		if(!rel[0] && (!strcmp(sub, BI_MANIFEST)
					   || !strcmp(sub, BI_MANIFEST ".tmp")))
			continue;
		if(fl->count >= BI_MANIFEST_MAX_FILES) {
			ok = false;
			break;
		}
		if(fl->count == fl->cap) {
			size_t cap = fl->cap ? fl->cap * 2 : 128;
			char** grown = realloc(fl->rel, cap * sizeof(char*));
			if(!grown) {
				ok = false;
				break;
			}
			fl->rel = grown;
			fl->cap = cap;
		}
		size_t len = strlen(sub) + 1;
		fl->rel[fl->count] = malloc(len);
		if(!fl->rel[fl->count]) {
			ok = false;
			break;
		}
		memcpy(fl->rel[fl->count++], sub, len);
	}
	closedir(d);
	return ok;
}

static void free_list(struct file_list* fl) {
	for(size_t i = 0; i < fl->count; i++)
		free(fl->rel[i]);
	free(fl->rel);
	memset(fl, 0, sizeof(*fl));
}

static int cmp_str(const void* a, const void* b) {
	return strcmp(*(char* const*)a, *(char* const*)b);
}

static bool hash_file(const char* path, char hex[65], long* size,
					  uint8_t* buf, size_t buf_len) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	bi_sha256 s;
	bi_sha256_init(&s);
	long total = 0;
	size_t n;
	while((n = fread(buf, 1, buf_len, f)) > 0) {
		bi_sha256_update(&s, buf, n);
		total += (long)n;
	}
	bool ok = !ferror(f);
	fclose(f);
	uint8_t d[32];
	bi_sha256_final(&s, d);
	for(int i = 0; i < 32; i++)
		snprintf(hex + 2 * i, 3, "%02x", d[i]);
	*size = total;
	return ok;
}

/* JSON string body for a path we wrote ourselves (ASCII, no quotes or
 * backslashes in any pack path; anything else is refused). */
static bool json_safe(const char* s) {
	for(; *s; s++)
		if((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f || *s == '"'
		   || *s == '\\')
			return false;
	return true;
}

/* Hashes every staged file (this is also the read-back check: every byte
 * the pack will be loaded from is read once more from the card) and writes
 * the manifest in json.dumps(indent=2, sort_keys=True) form. */
static bool write_manifest(const char* staging, const char* jar_stem,
						   bi_ctx* ctx) {
	struct file_list fl = {0};
	if(!list_files(staging, "", &fl)) {
		free_list(&fl);
		return false;
	}
	qsort(fl.rel, fl.count, sizeof(char*), cmp_str);
	char tmp[BI_PATH_MAX], final_path[BI_PATH_MAX];
	if(!join(tmp, sizeof(tmp), staging, BI_MANIFEST ".tmp")
	   || !join(final_path, sizeof(final_path), staging, BI_MANIFEST)) {
		free_list(&fl);
		return false;
	}
	uint8_t* buf = malloc(16384);
	FILE* f = buf ? fopen(tmp, "wb") : NULL;
	bool ok = f != NULL && json_safe(jar_stem);
	if(ok)
		fprintf(f, "{\n  \"files\": [\n");
	for(size_t i = 0; ok && i < fl.count; i++) {
		if(bi_cancel_requested(ctx)) {
			ok = false;
			break;
		}
		char full[BI_PATH_MAX], hex[65];
		long size = 0;
		ok = json_safe(fl.rel[i]) && join(full, sizeof(full), staging, fl.rel[i])
			&& hash_file(full, hex, &size, buf, 16384);
		if(ok)
			fprintf(f,
					"    {\n      \"path\": \"%s\",\n      \"sha256\": \"%s\",\n"
					"      \"size\": %ld\n    }%s\n",
					fl.rel[i], hex, size, i + 1 < fl.count ? "," : "");
		bi_progress_step(ctx, (int)i + 1);
	}
	if(ok)
		fprintf(f,
				"  ],\n  \"look\": \"crisp\",\n  \"name\": \"%s\",\n"
				"  \"schema_version\": 1,\n  \"source\": {\n"
				"    \"kind\": \"zip\",\n    \"name\": \"%s\"\n  }\n}\n",
				BI_PACK_DISPLAY, jar_stem);
	if(f && fclose(f) != 0)
		ok = false;
	free(buf);
	free_list(&fl);
	if(ok) {
		remove(final_path);
		ok = rename(tmp, final_path) == 0;
	}
	if(!ok)
		remove(tmp);
	return ok;
}

/* Every file the helper writes for a client jar is present and non-empty. */
static bool validate_staging(const char* staging, char* why, size_t why_len) {
	static const char* const extra[] = {"gui_2.png", "anim.png", "pointer.png"};
	for(size_t i = 0; i < BI_TEXTURE_COUNT + 3; i++) {
		const char* rel = i < BI_TEXTURE_COUNT ? bi_textures[i].dest
											   : extra[i - BI_TEXTURE_COUNT];
		char full[BI_PATH_MAX];
		struct stat st;
		if(!join(full, sizeof(full), staging, rel) || stat(full, &st) != 0
		   || !S_ISREG(st.st_mode) || st.st_size <= 0) {
			snprintf(why, why_len, "%s", rel);
			return false;
		}
	}
	return true;
}

/* ---- swap and recovery -------------------------------------------------- */

static bool crash_here(bi_ctx* ctx, int step) {
	return ctx && ctx->crash_after_step == step;
}

/* packs_dir entry whose name equals `name` ignoring ASCII case. FAT (the SD
 * card) and NTFS are case-insensitive, so an older helper's "beta-1.7.3" IS
 * the "Beta-1.7.3" folder there; scanning makes PC/Linux behave the same.
 * Writes the full path of the entry as it is spelled on disk. */
static bool find_ci(const char* packs_dir, const char* name, char* out,
					size_t out_len) {
	DIR* d = opendir(packs_dir);
	if(!d)
		return false;
	bool found = false;
	struct dirent* e;
	while(!found && (e = readdir(d))) {
		const char* a = e->d_name;
		const char* b = name;
		while(*a && *b
			  && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
			a++;
			b++;
		}
		if(*a || *b)
			continue;
		found = join(out, out_len, packs_dir, e->d_name) && is_dir(out);
	}
	closedir(d);
	return found;
}

bool bi_swap_into_place(const struct bi_paths* paths, bi_ctx* ctx) {
	char staging[BI_PATH_MAX], old[BI_PATH_MAX], target[BI_PATH_MAX],
		existing[BI_PATH_MAX], existing_manifest[BI_PATH_MAX];
	if(!join(staging, sizeof(staging), paths->packs_dir, BI_STAGING_NAME)
	   || !join(old, sizeof(old), paths->packs_dir, BI_OLD_NAME)
	   || !join(target, sizeof(target), paths->packs_dir, BI_PACK_NAME))
		return false;
	if(is_dir(old) && !bi_rmtree(old))
		return false;
	/* The previous import under any spelling: "Beta-1.7.3" (ours) or an
	 * older helper's "beta-1.7.3". It is REPLACED as a whole folder (moved
	 * aside, then deleted), never merged into. */
	bool had_target = find_ci(paths->packs_dir, BI_PACK_NAME, existing,
							  sizeof(existing));
	if(had_target
	   && (!join(existing_manifest, sizeof(existing_manifest), existing,
				 BI_MANIFEST)
		   || !is_file(existing_manifest))) {
		/* Not written by the importer or helper: keep it, renamed. */
		char aside[BI_PATH_MAX];
		for(int i = 1; i < 100; i++) {
			int n = i == 1 ? snprintf(aside, sizeof(aside), "%s-old", target)
						   : snprintf(aside, sizeof(aside), "%s-old%d", target,
									  i);
			if(n < 0 || (size_t)n >= sizeof(aside))
				return false;
			if(!is_dir(aside) && !is_file(aside))
				break;
		}
		if(rename(existing, aside) != 0)
			return false;
		had_target = false;
	} else if(had_target && rename(existing, old) != 0) {
		return false;
	}
	if(crash_here(ctx, 1))
		return true; /* power cut: old aside, new not yet in place */
	if(rename(staging, target) != 0) {
		if(had_target)
			rename(old, existing); /* put the previous pack back */
		return false;
	}
	if(crash_here(ctx, 2))
		return true; /* power cut: new in place, old not yet deleted */
	if(had_target)
		bi_rmtree(old); /* best effort; recovery retries */
	return true;
}

bool bi_retire_legacy(const struct bi_paths* paths) {
	char legacy[BI_PATH_MAX], manifest[BI_PATH_MAX], hidden[BI_PATH_MAX];
	if(!join(legacy, sizeof(legacy), paths->packs_dir, BI_LEGACY_NAME)
	   || !join(manifest, sizeof(manifest), legacy, BI_MANIFEST)
	   || !join(hidden, sizeof(hidden), paths->packs_dir, BI_LEGACY_HIDDEN))
		return false;
	if(!is_dir(legacy) || !is_file(manifest) || is_dir(hidden))
		return false;
	return rename(legacy, hidden) == 0;
}

void bi_recover(const struct bi_paths* paths) {
	char staging[BI_PATH_MAX], old[BI_PATH_MAX], target[BI_PATH_MAX],
		staged_manifest[BI_PATH_MAX], hidden[BI_PATH_MAX], any[BI_PATH_MAX];
	if(!join(staging, sizeof(staging), paths->packs_dir, BI_STAGING_NAME)
	   || !join(old, sizeof(old), paths->packs_dir, BI_OLD_NAME)
	   || !join(target, sizeof(target), paths->packs_dir, BI_PACK_NAME)
	   || !join(staged_manifest, sizeof(staged_manifest), staging, BI_MANIFEST)
	   || !join(hidden, sizeof(hidden), paths->packs_dir, BI_LEGACY_HIDDEN))
		return;
	if(!find_ci(paths->packs_dir, BI_PACK_NAME, any, sizeof(any))
	   && is_dir(old)) {
		/* Cut between the two renames: finish with the complete new pack if
		 * it validated (manifest is written last), else roll back (under
		 * the new spelling; on FAT the old spelling is the same folder). */
		if(!(is_file(staged_manifest) && rename(staging, target) == 0))
			rename(old, target);
	}
	if(is_dir(old))
		bi_rmtree(old);
	if(is_dir(staging))
		bi_rmtree(staging);
	if(is_dir(hidden))
		bi_rmtree(hidden); /* a retired "Beta 1.7.3" import */
}

/* ---- the pipeline --------------------------------------------------------- */

static bi_state finish(bi_ctx* ctx, const char* staging, bi_state st,
					   const char* msg) {
	if(st != BI_DONE && staging)
		bi_rmtree(staging);
	if(st == BI_CANCELLED)
		msg = "Import cancelled. Nothing was changed.";
	ctx_lock(ctx);
	ctx->p.state = st;
	if(msg)
		snprintf(ctx->p.msg, sizeof(ctx->p.msg), "%s", msg);
	ctx_unlock(ctx);
	return st;
}

bi_state bi_run_import(const struct bi_paths* paths, bi_ctx* ctx) {
	ctx->assets_dir = paths->assets_dir;
	s_sound_stub_ran = false;
	bi_progress_set(ctx, BI_SCANNING, 0, 0, "Looking for b1.7.3.jar");
	bi_recover(paths);

	char jar[BI_PATH_MAX];
	if(!bi_find_jar(paths, jar, sizeof(jar)))
		return finish(ctx, NULL, BI_FAILED,
					  "No jar found. Copy b1.7.3.jar to sd:/beta/ first.");
	if(!bi_mkdirs(paths->packs_dir))
		return finish(ctx, NULL, BI_FAILED,
					  "Can't create the texturepacks folder on the SD card.");
	char staging[BI_PATH_MAX];
	if(!join(staging, sizeof(staging), paths->packs_dir, BI_STAGING_NAME))
		return finish(ctx, NULL, BI_FAILED, "SD card path too long.");
	if(is_dir(staging) && !bi_rmtree(staging))
		return finish(ctx, NULL, BI_FAILED,
					  "Can't clear the old import folder on the SD card.");
	if(!bi_mkdirs(staging))
		return finish(ctx, NULL, BI_FAILED,
					  "Can't write to the SD card. Is it locked or full?");
	if(bi_cancel_requested(ctx))
		return finish(ctx, staging, BI_CANCELLED, NULL);

	if(!bi_jar_extract_textures(jar, staging, ctx))
		return finish(ctx, staging,
					  bi_cancel_requested(ctx) ? BI_CANCELLED : BI_FAILED,
					  NULL);

	int sounds = 0;
	bool have_resources = is_dir(paths->resources_dir);
	if(have_resources) {
		bi_progress_set(ctx, BI_SOUNDS, 0, 0, "Converting sounds");
		sounds = bi_sounds_import(paths->resources_dir, staging, ctx);
		if(sounds < 0) {
			bool said = ctx->p.state == BI_FAILED; /* B set the reason */
			return finish(ctx, staging,
						  bi_cancel_requested(ctx) ? BI_CANCELLED : BI_FAILED,
						  said ? NULL : "Sound import failed. Nothing was changed.");
		}
	}
	ctx_lock(ctx);
	ctx->p.sounds_imported = sounds > 0;
	ctx->p.sounds_count = sounds;
	ctx_unlock(ctx);

	char why[64];
	if(!validate_staging(staging, why, sizeof(why))) {
		char msg[128];
		snprintf(msg, sizeof(msg), "Import check failed: %s is missing.", why);
		return finish(ctx, staging, BI_FAILED, msg);
	}
	/* jar stem, like helper source.name for a zip (Path.stem) */
	const char* base = strrchr(jar, '/');
	base = base ? base + 1 : jar;
	char stem[128];
	snprintf(stem, sizeof(stem), "%.100s", base);
	char* dot = strrchr(stem, '.');
	if(dot)
		*dot = '\0';
	bi_progress_set(ctx, BI_SWAPPING, 0, 0, "Checking the new pack");
	if(!write_manifest(staging, stem, ctx))
		return finish(ctx, staging,
					  bi_cancel_requested(ctx) ? BI_CANCELLED : BI_FAILED,
					  "Can't verify the new pack on the SD card.");
	/* Past this point cancel is ignored: the swap is two renames. */
	bi_progress_set(ctx, BI_SWAPPING, 0, 0, "Installing the pack");
	if(!bi_swap_into_place(paths, ctx))
		return finish(ctx, staging, BI_FAILED,
					  "Can't install the pack on the SD card. Your previous "
					  "pack is unchanged.");

	const char* msg;
	if(sounds > 0)
		msg = "Beta 1.7.3 textures and sounds imported.";
	else if(have_resources && s_sound_stub_ran)
		msg = "Textures imported. Sounds: not yet in this build.";
	else if(have_resources)
		msg = "Textures imported. No sounds found in sd:/beta/resources.";
	else
		msg = "Textures imported. For sounds, copy .minecraft/resources "
			  "to sd:/beta/resources and import again.";
	return finish(ctx, NULL, BI_DONE, msg);
}

/* ---- public API (main thread) ---------------------------------------------- */

static struct thread_mutex s_lock;
static bool s_lock_ready;
static bi_ctx s_ctx;
static struct thread s_thread;
static bool s_thread_live; /* created and not yet joined */
static bool s_select_pending;
static bi_select_fn s_selector;

static void* worker(void* arg) {
	(void)arg;
	struct bi_paths paths;
	bi_default_paths(&paths);
	bi_run_import(&paths, &s_ctx);
	return NULL;
}

static bool terminal(bi_state s) {
	return s == BI_IDLE || s == BI_DONE || s == BI_FAILED
		|| s == BI_CANCELLED;
}

void beta_import_set_selector(bi_select_fn fn) {
	s_selector = fn;
}

void beta_import_recover(void) {
	struct bi_paths paths;
	bi_default_paths(&paths);
	bi_recover(&paths);
}

bool beta_import_available(void) {
	struct bi_paths paths;
	bi_default_paths(&paths);
	char jar[BI_PATH_MAX];
	return bi_find_jar(&paths, jar, sizeof(jar));
}

bool beta_import_start(void) {
	if(!s_lock_ready) {
		if(!thread_mutex_init(&s_lock))
			return false;
		s_lock_ready = true;
	}
	if(s_thread_live)
		return false; /* busy (or finished but not yet polled) */
	thread_mutex_lock(&s_lock);
	memset(&s_ctx, 0, sizeof(s_ctx));
	s_ctx.lock = &s_lock;
	s_ctx.p.state = BI_SCANNING;
	snprintf(s_ctx.p.msg, sizeof(s_ctx.p.msg), "Starting");
	thread_mutex_unlock(&s_lock);
	s_select_pending = false;
	if(!thread_create(&s_thread, worker, NULL, BI_THREAD_PRIORITY)) {
		bi_progress_set(&s_ctx, BI_FAILED, 0, 0,
						"Can't start the import (out of memory).");
		return false;
	}
	s_thread_live = true;
	s_select_pending = true;
	return true;
}

void beta_import_poll(bi_progress* out) {
	if(!s_lock_ready) {
		memset(out, 0, sizeof(*out));
		out->state = BI_IDLE;
		return;
	}
	thread_mutex_lock(&s_lock);
	*out = s_ctx.p;
	thread_mutex_unlock(&s_lock);
	if(s_thread_live && terminal(out->state)) {
		thread_join(&s_thread); /* the worker has returned: prompt */
		s_thread_live = false;
	}
	if(!s_thread_live && s_select_pending && out->state == BI_DONE) {
		s_select_pending = false;
		bool selected = s_selector && s_selector("texturepacks/" BI_PACK_NAME);
		if(selected) {
			/* The earlier "Beta 1.7.3" import (helper or in-game, has a
			 * manifest) is no longer selected: hide it with ONE rename
			 * (cheap on the main thread); recovery deletes it later. */
			struct bi_paths paths;
			bi_default_paths(&paths);
			bi_retire_legacy(&paths);
		} else {
			char done_msg[sizeof(out->msg)];
			thread_mutex_lock(&s_lock);
			memcpy(done_msg, s_ctx.p.msg, sizeof(done_msg));
			snprintf(s_ctx.p.msg, sizeof(s_ctx.p.msg), "%.80s %s", done_msg,
					 "Pick Beta 1.7.3 in Texture Packs.");
			*out = s_ctx.p;
			thread_mutex_unlock(&s_lock);
		}
	}
}

void beta_import_cancel(void) {
	s_ctx.cancel = true;
}
