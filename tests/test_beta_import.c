/* In-game Beta importer (source/platform/beta_import*.c), PC unit tests:
 *   1. zip reader: stored, deflated, deflated + data descriptor, large
 *      multi-chunk entry; corrupt CRC, truncated deflate, encrypted, bad
 *      method, not-a-zip, ratio bomb, bad local header;
 *   2. SHA-256 known answers (manifest hashes);
 *   3. swap + power-cut safety: a simulated cut after each swap step, then
 *      boot recovery -- "Beta 1.7.3" is always either the complete previous
 *      pack or the complete new one, leftovers are removed, a foreign
 *      folder is never deleted, an incomplete staging is never installed.
 * The texture-output equality with the Pack Helper is test_beta_import_e2e
 * (tests/test_beta_import.py). */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "platform/beta_import_internal.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir1(p) _mkdir(p)
#else
#define mkdir1(p) mkdir((p), 0777)
#endif

static int failures;
#define CHECK(c)                                                               \
	do {                                                                       \
		if(!(c)) {                                                             \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
			failures++;                                                        \
		}                                                                      \
	} while(0)

/* ---- zip writer for fixtures ------------------------------------------- */

struct zent {
	const char* name;
	const uint8_t* data;
	size_t len;
	int method;		 /* 0 store, 8 deflate */
	bool descriptor; /* bit 3: sizes/CRC after data, zeros in local header */
	bool bad_crc;
	size_t truncate; /* drop this many bytes from the compressed payload */
	uint16_t flags_or;
	uint16_t method_override; /* nonzero: claim this method in both headers */
	uint32_t usize_override;
};

static void w16(FILE* f, unsigned v) {
	fputc(v & 255, f);
	fputc((v >> 8) & 255, f);
}
static void w32(FILE* f, uint32_t v) {
	w16(f, v & 0xffff);
	w16(f, v >> 16);
}

static uint8_t* deflate_raw(const uint8_t* in, size_t n, size_t* out_n) {
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	assert(deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY)
		   == Z_OK);
	size_t cap = deflateBound(&zs, (uLong)n) + 16;
	uint8_t* out = malloc(cap);
	zs.next_in = (Bytef*)in;
	zs.avail_in = (uInt)n;
	zs.next_out = out;
	zs.avail_out = (uInt)cap;
	assert(deflate(&zs, Z_FINISH) == Z_STREAM_END);
	*out_n = zs.total_out;
	deflateEnd(&zs);
	return out;
}

static void write_zip(const char* path, const struct zent* e, size_t n) {
	FILE* f = fopen(path, "wb");
	assert(f);
	uint32_t* offs = calloc(n, sizeof(uint32_t));
	uint32_t* crcs = calloc(n, sizeof(uint32_t));
	uint32_t* csz = calloc(n, sizeof(uint32_t));
	for(size_t i = 0; i < n; i++) {
		size_t clen = e[i].len;
		uint8_t* comp = NULL;
		const uint8_t* payload = e[i].data;
		if(e[i].method == 8) {
			comp = deflate_raw(e[i].data, e[i].len, &clen);
			payload = comp;
		}
		clen -= e[i].truncate;
		uint32_t crc = (uint32_t)crc32(0, e[i].data, (uInt)e[i].len);
		if(e[i].bad_crc)
			crc ^= 0xdeadbeef;
		crcs[i] = crc;
		csz[i] = (uint32_t)clen;
		offs[i] = (uint32_t)ftell(f);
		uint16_t flags = (e[i].descriptor ? 8 : 0) | e[i].flags_or;
		uint16_t method = e[i].method_override ? e[i].method_override
											   : (uint16_t)e[i].method;
		w32(f, 0x04034b50);
		w16(f, 20);
		w16(f, flags);
		w16(f, method);
		w16(f, 0);
		w16(f, 0);
		w32(f, e[i].descriptor ? 0 : crc);
		w32(f, e[i].descriptor ? 0 : (uint32_t)clen);
		w32(f, e[i].descriptor ? 0 : (uint32_t)e[i].len);
		w16(f, (unsigned)strlen(e[i].name));
		w16(f, 0);
		fwrite(e[i].name, 1, strlen(e[i].name), f);
		fwrite(payload, 1, clen, f);
		if(e[i].descriptor) {
			w32(f, 0x08074b50);
			w32(f, crc);
			w32(f, (uint32_t)clen);
			w32(f, (uint32_t)e[i].len);
		}
		free(comp);
	}
	uint32_t cd = (uint32_t)ftell(f);
	for(size_t i = 0; i < n; i++) {
		uint16_t flags = (e[i].descriptor ? 8 : 0) | e[i].flags_or;
		uint16_t method = e[i].method_override ? e[i].method_override
											   : (uint16_t)e[i].method;
		w32(f, 0x02014b50);
		w16(f, 20);
		w16(f, 20);
		w16(f, flags);
		w16(f, method);
		w16(f, 0);
		w16(f, 0);
		w32(f, crcs[i]);
		w32(f, csz[i]);
		w32(f, e[i].usize_override ? e[i].usize_override : (uint32_t)e[i].len);
		w16(f, (unsigned)strlen(e[i].name));
		w16(f, 0);
		w16(f, 0);
		w16(f, 0);
		w16(f, 0);
		w32(f, 0);
		w32(f, offs[i]);
		fwrite(e[i].name, 1, strlen(e[i].name), f);
	}
	uint32_t cd_end = (uint32_t)ftell(f);
	w32(f, 0x06054b50);
	w16(f, 0);
	w16(f, 0);
	w16(f, (unsigned)n);
	w16(f, (unsigned)n);
	w32(f, cd_end - cd);
	w32(f, cd);
	w16(f, 0);
	fclose(f);
	free(offs);
	free(crcs);
	free(csz);
}

struct membuf {
	uint8_t* p;
	size_t n, cap;
};

static bool mem_sink(void* user, const uint8_t* d, size_t n) {
	struct membuf* m = user;
	if(m->n + n > m->cap) {
		m->cap = (m->n + n) * 2;
		m->p = realloc(m->p, m->cap);
	}
	memcpy(m->p + m->n, d, n);
	m->n += n;
	return true;
}

static enum bi_zip_err extract_named(const char* zip, const char* name,
									 struct membuf* out) {
	bi_zip* z = NULL;
	enum bi_zip_err err = bi_zip_open(zip, &z);
	if(err != BI_ZIP_OK)
		return err;
	err = BI_ZIP_BAD_CD;
	for(size_t i = 0; i < bi_zip_count(z); i++) {
		bi_zip_entry e;
		bi_zip_entry_at(z, i, &e);
		if(!strcmp(e.name, name)) {
			out->n = 0;
			err = bi_zip_extract(z, &e, mem_sink, out, NULL);
			break;
		}
	}
	bi_zip_close(z);
	return err;
}

static void test_zip(const char* dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/fixture.zip", dir);
	/* Seeded noise + a compressible run; > 16 KB chunk to cross buffers. */
	size_t big_n = 300000;
	uint8_t* big = malloc(big_n);
	uint32_t s = 12345;
	for(size_t i = 0; i < big_n; i++) {
		s = s * 1103515245u + 12345u;
		big[i] = i % 3 ? (uint8_t)(s >> 16) : (uint8_t)(i & 7);
	}
	static const uint8_t small[] = "beta importer fixture, never Mojang bytes";
	struct zent ents[] = {
		{.name = "stored.bin", .data = small, .len = sizeof(small), .method = 0},
		{.name = "deflated.bin", .data = big, .len = big_n, .method = 8},
		{.name = "dd.bin", .data = big, .len = big_n, .method = 8,
		 .descriptor = true},
		{.name = "dd_stored.bin", .data = small, .len = sizeof(small),
		 .method = 0, .descriptor = true},
		{.name = "empty.bin", .data = small, .len = 0, .method = 0},
		{.name = "badcrc.bin", .data = big, .len = 5000, .method = 8,
		 .bad_crc = true},
		{.name = "trunc.bin", .data = big, .len = big_n, .method = 8,
		 .truncate = 4000},
		{.name = "enc.bin", .data = small, .len = sizeof(small), .method = 0,
		 .flags_or = 1},
		{.name = "lzma.bin", .data = small, .len = sizeof(small), .method = 0,
		 .method_override = 14},
		{.name = "short.bin", .data = big, .len = 1000, .method = 8,
		 .usize_override = 2000},
	};
	write_zip(path, ents, sizeof(ents) / sizeof(*ents));

	struct membuf m = {0};
	CHECK(extract_named(path, "stored.bin", &m) == BI_ZIP_OK);
	CHECK(m.n == sizeof(small) && !memcmp(m.p, small, m.n));
	CHECK(extract_named(path, "deflated.bin", &m) == BI_ZIP_OK);
	CHECK(m.n == big_n && !memcmp(m.p, big, m.n));
	CHECK(extract_named(path, "dd.bin", &m) == BI_ZIP_OK);
	CHECK(m.n == big_n && !memcmp(m.p, big, m.n));
	CHECK(extract_named(path, "dd_stored.bin", &m) == BI_ZIP_OK);
	CHECK(m.n == sizeof(small) && !memcmp(m.p, small, m.n));
	CHECK(extract_named(path, "empty.bin", &m) == BI_ZIP_OK && m.n == 0);
	CHECK(extract_named(path, "badcrc.bin", &m) == BI_ZIP_CORRUPT);
	CHECK(extract_named(path, "trunc.bin", &m) == BI_ZIP_CORRUPT);
	CHECK(extract_named(path, "enc.bin", &m) == BI_ZIP_ENCRYPTED);
	CHECK(extract_named(path, "lzma.bin", &m) == BI_ZIP_METHOD);
	CHECK(extract_named(path, "short.bin", &m) == BI_ZIP_CORRUPT);

	/* Ratio bomb: 2 MB of zeros deflates to ~2 KB (> 200:1). */
	size_t zn = 2u << 20;
	uint8_t* zeros = calloc(1, zn);
	struct zent bomb = {.name = "bomb.bin", .data = zeros, .len = zn,
						.method = 8};
	write_zip(path, &bomb, 1);
	CHECK(extract_named(path, "bomb.bin", &m) == BI_ZIP_TOO_BIG);
	free(zeros);

	/* Not a zip / truncated archive (EOCD gone). */
	FILE* f = fopen(path, "wb");
	fputs("this is not a zip file at all, just text padding padding", f);
	fclose(f);
	bi_zip* z = NULL;
	CHECK(bi_zip_open(path, &z) == BI_ZIP_NOT_ZIP && !z);

	/* Local header offset pointing at garbage. */
	write_zip(path, ents, 1);
	f = fopen(path, "r+b");
	fputc('X', f); /* break the PK\3\4 signature */
	fclose(f);
	CHECK(extract_named(path, "stored.bin", &m) == BI_ZIP_BAD_LOCAL);

	/* Cancel stops a streaming entry. */
	write_zip(path, &ents[1], 1);
	CHECK(bi_zip_open(path, &z) == BI_ZIP_OK);
	bi_zip_entry e;
	bi_zip_entry_at(z, 0, &e);
	volatile bool cancel = true;
	CHECK(bi_zip_extract(z, &e, mem_sink, &m, &cancel) == BI_ZIP_CANCELLED);
	bi_zip_close(z);

	free(m.p);
	free(big);
}

static void test_sha(void) {
	static const struct {
		const char* in;
		const char* hex;
	} kat[] = {
		{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
		{"abc",
		 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
		{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
	};
	for(size_t k = 0; k < sizeof(kat) / sizeof(*kat); k++) {
		bi_sha256 s;
		bi_sha256_init(&s);
		/* feed byte by byte to exercise the buffering */
		for(const char* p = kat[k].in; *p; p++)
			bi_sha256_update(&s, p, 1);
		uint8_t d[32];
		bi_sha256_final(&s, d);
		char hex[65];
		for(int i = 0; i < 32; i++)
			snprintf(hex + 2 * i, 3, "%02x", d[i]);
		CHECK(!strcmp(hex, kat[k].hex));
	}
}

/* ---- swap / power cut ---------------------------------------------------- */

static void put(const char* dir, const char* rel, const char* text) {
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", dir, rel);
	FILE* f = fopen(p, "wb");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static bool exists(const char* dir, const char* rel) {
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", dir, rel);
	struct stat st;
	return stat(p, &st) == 0;
}

static bool has(const char* dir, const char* rel, const char* text) {
	char p[512], buf[64] = "";
	snprintf(p, sizeof(p), "%s/%s", dir, rel);
	FILE* f = fopen(p, "rb");
	if(!f)
		return false;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return !strcmp(buf, text);
}

/* packs/: "Beta 1.7.3" = old (ours or foreign), .beta-import-tmp = new. */
static void layout(struct bi_paths* paths, const char* root, bool old_ours,
				   bool old_present, bool staged_complete) {
	bi_rmtree(root);
	memset(paths, 0, sizeof(*paths));
	snprintf(paths->packs_dir, sizeof(paths->packs_dir), "%s/texturepacks",
			 root);
	char d[600];
	snprintf(d, sizeof(d), "%s/" BI_PACK_NAME, paths->packs_dir);
	bi_mkdirs(d);
	if(old_present) {
		put(d, "terrain.png", "old");
		if(old_ours)
			put(d, BI_MANIFEST, "{}");
	} else {
		rmdir(d);
	}
	snprintf(d, sizeof(d), "%s/" BI_STAGING_NAME, paths->packs_dir);
	bi_mkdirs(d);
	put(d, "terrain.png", "new");
	if(staged_complete)
		put(d, BI_MANIFEST, "{}");
}

static void check_clean(const struct bi_paths* paths) {
	CHECK(!exists(paths->packs_dir, BI_STAGING_NAME));
	CHECK(!exists(paths->packs_dir, BI_OLD_NAME));
}

static void test_swap(const char* dir) {
	char root[512];
	snprintf(root, sizeof(root), "%s/swap", dir);
	struct bi_paths paths;
	bi_ctx ctx;

	/* Clean swap over our previous import. */
	layout(&paths, root, true, true, true);
	memset(&ctx, 0, sizeof(ctx));
	CHECK(bi_swap_into_place(&paths, &ctx));
	CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "new"));
	check_clean(&paths);

	/* First import (no previous pack). */
	layout(&paths, root, true, false, true);
	CHECK(bi_swap_into_place(&paths, &ctx));
	CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "new"));
	check_clean(&paths);

	/* Power cut after each step, then boot recovery. */
	for(int step = 1; step <= 2; step++) {
		layout(&paths, root, true, true, true);
		memset(&ctx, 0, sizeof(ctx));
		ctx.crash_after_step = step;
		CHECK(bi_swap_into_place(&paths, &ctx));
		/* At the cut: the selected name holds a complete pack, or is
		 * absent (never a half pack: renames are whole-folder). */
		CHECK(!exists(paths.packs_dir, BI_PACK_NAME "/terrain.png")
			  || has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "new"));
		bi_recover(&paths);
		CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "new"));
		check_clean(&paths);
	}

	/* Cut between renames but staging incomplete (cannot happen through
	 * bi_swap_into_place, recovery must still be safe): roll back. */
	layout(&paths, root, true, true, false);
	char t[600], o[600];
	snprintf(t, sizeof(t), "%s/" BI_PACK_NAME, paths.packs_dir);
	snprintf(o, sizeof(o), "%s/" BI_OLD_NAME, paths.packs_dir);
	CHECK(rename(t, o) == 0);
	bi_recover(&paths);
	CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "old"));
	check_clean(&paths);

	/* Cut during staging (no manifest yet): previous pack untouched. */
	layout(&paths, root, true, true, false);
	bi_recover(&paths);
	CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "old"));
	check_clean(&paths);

	/* Complete staging but never swapped (cut right after the manifest):
	 * discarded, previous selection untouched. */
	layout(&paths, root, true, true, true);
	bi_recover(&paths);
	CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "old"));
	check_clean(&paths);

	/* A foreign "Beta-1.7.3" (no manifest) is kept aside, never deleted. */
	layout(&paths, root, false, true, true);
	memset(&ctx, 0, sizeof(ctx));
	CHECK(bi_swap_into_place(&paths, &ctx));
	CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "new"));
	CHECK(has(paths.packs_dir, BI_PACK_NAME "-old/terrain.png", "old"));
	check_clean(&paths);

	/* An older helper's lowercase "beta-1.7.3" (the same folder on FAT) is
	 * REPLACED, never merged into: its extra file must not survive, and only
	 * one spelling remains -- also across a power cut between the renames. */
	for(int step = 0; step <= 1; step++) {
		layout(&paths, root, true, false, true);
		char lower[600];
		snprintf(lower, sizeof(lower), "%s/beta-1.7.3", paths.packs_dir);
		bi_mkdirs(lower);
		put(lower, "terrain.png", "old");
		put(lower, "stale-only-in-old.png", "x");
		put(lower, BI_MANIFEST, "{}");
		memset(&ctx, 0, sizeof(ctx));
		ctx.crash_after_step = step;
		CHECK(bi_swap_into_place(&paths, &ctx));
		bi_recover(&paths);
		CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "new"));
		CHECK(!exists(paths.packs_dir, BI_PACK_NAME "/stale-only-in-old.png"));
		DIR* d = opendir(paths.packs_dir);
		int spellings = 0;
		struct dirent* e;
		while((e = readdir(d)))
			if(!strcmp(e->d_name, "beta-1.7.3")
			   || !strcmp(e->d_name, BI_PACK_NAME))
				spellings++;
		closedir(d);
		CHECK(spellings == 1);
		check_clean(&paths);
	}

	/* The earlier "Beta 1.7.3" import (manifest) is retired only after the
	 * new pack is selected: one rename to a hidden name, deleted by the next
	 * recovery. A manifest-less "Beta 1.7.3" (player's own) is untouched. */
	layout(&paths, root, true, false, true);
	memset(&ctx, 0, sizeof(ctx));
	CHECK(bi_swap_into_place(&paths, &ctx));
	char legacy[600];
	snprintf(legacy, sizeof(legacy), "%s/" BI_LEGACY_NAME, paths.packs_dir);
	bi_mkdirs(legacy);
	put(legacy, "terrain.png", "legacy");
	put(legacy, BI_MANIFEST, "{}");
	CHECK(bi_retire_legacy(&paths));
	CHECK(!exists(paths.packs_dir, BI_LEGACY_NAME));
	CHECK(exists(paths.packs_dir, BI_LEGACY_HIDDEN));
	bi_recover(&paths);
	CHECK(!exists(paths.packs_dir, BI_LEGACY_HIDDEN));
	CHECK(has(paths.packs_dir, BI_PACK_NAME "/terrain.png", "new"));
	bi_mkdirs(legacy);
	put(legacy, "terrain.png", "mine");
	CHECK(!bi_retire_legacy(&paths));
	CHECK(has(paths.packs_dir, BI_LEGACY_NAME "/terrain.png", "mine"));
	bi_rmtree(root);
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;
	char dir[256];
	snprintf(dir, sizeof(dir), "test_beta_import_tmp_%d", (int)getpid());
	bi_rmtree(dir);
	mkdir1(dir);
	test_zip(dir);
	test_sha();
	test_swap(dir);
	bi_rmtree(dir);
	if(failures) {
		fprintf(stderr, "test_beta_import: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_beta_import: PASS (zip reader, sha256, swap/power-cut)\n");
	return 0;
}
