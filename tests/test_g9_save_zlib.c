/* R22: codec switches with live prefix caches, exact decoded NBT, actual
 * loader, large sector COW, injected failures and every persistence cut.
 * Optional argv[1] exports final regions/raw NBT for the owned Beta reader. */
#include <errno.h>
#include <zlib.h>
#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "network/entity_archive.h"
#include "config.h"

static uint8_t ids[32768], meta[16384], sky[16384], torch[16384], height[256];
static struct server_chunk sc;
static struct chunk_tile_entities te = {.legacy_flat_complete = true};
static struct chunk_entities entities;
static char root[2][220], cut_root[220];
static unsigned cuts;
static uint8_t old_ids[32768];
static uint8_t old_meta[16384], old_sky[16384], old_torch[16384], old_height[256];
static void remember_old(void) {
	memcpy(old_ids, ids, sizeof ids); memcpy(old_meta, meta, sizeof meta);
	memcpy(old_sky, sky, sizeof sky); memcpy(old_torch, torch, sizeof torch);
	memcpy(old_height, height, sizeof height);
}
static int write_fault, write_ready, write_faults;
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* f) {
	if(__atomic_exchange_n(&write_ready, 0, __ATOMIC_ACQ_REL)) {
		int mode = __atomic_load_n(&write_fault, __ATOMIC_ACQUIRE);
		assert(size > 1 && count == 1);
		/* A real partial payload reaches disk before the short return. */
		if(mode == 1) assert(__real_fwrite(data, 1, size / 2, f) == size / 2);
		__atomic_fetch_add(&write_faults, 1, __ATOMIC_RELAXED);
		errno = mode == 2 ? ENOSPC : EIO; return 0;
	}
	return __real_fwrite(data, size, count, f);
}
static void payload_fault(void) { __atomic_store_n(&write_ready, 1, __ATOMIC_RELEASE); }

static struct buffer record(struct region_archive* ra) {
	uint32_t entry = ra->offsets[33], sectors = entry & 255;
	assert((entry >> 8) >= 2 && sectors && sectors < 256);
	FILE* f = ra->file; long at = (long)(entry >> 8) * REGION_SECTOR_SIZE;
	assert(!fseek(f, 0, SEEK_END) && (uint64_t)ftell(f) >=
		(uint64_t)at + sectors * REGION_SECTOR_SIZE);
	assert(!fseek(f, at, SEEK_SET));
	uint8_t h[5]; assert(fread(h, 1, 5, f) == 5 && h[4] == 2);
	size_t n = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16)
		| ((uint32_t)h[2] << 8) | h[3];
	assert(n > 1 && n + 4 <= sectors * REGION_SECTOR_SIZE);
	struct buffer b = BUFFER_INIT;
	assert(!buffer_reserve(&b, n - 1)); b.len = n - 1;
	assert(fread(b.data, 1, b.len, f) == b.len); return b;
}
static struct buffer decoded(struct buffer b) {
	struct buffer raw = BUFFER_INIT; assert(!buffer_reserve(&raw, 1024 * 1024));
	uLongf n = raw.cap;
	assert(uncompress(raw.data, &n, b.data, b.len) == Z_OK); raw.len = n; return raw;
}
static void loader(struct region_archive* ra, bool allow_old) {
	struct server_chunk loaded = {0};
	assert(region_archive_get_chunk(ra, 1, 1, &loaded, NULL, NULL));
	bool current = !memcmp(loaded.ids, ids, sizeof ids);
	assert(current || (allow_old && !memcmp(loaded.ids, old_ids, sizeof ids)));
	assert(!memcmp(loaded.metadata, current ? meta : old_meta, sizeof meta));
	assert(!memcmp(loaded.lighting_sky, current ? sky : old_sky, sizeof sky));
	assert(!memcmp(loaded.lighting_torch, current ? torch : old_torch, sizeof torch));
	assert(!memcmp(loaded.heightmap, current ? height : old_height, sizeof height));
	server_chunk_arrays_free(&loaded);
}
static void reopen(const char* path, bool allow_old) {
	string_t name; string_init_set_str(name, path);
	for(unsigned twice = 0; twice < 2; twice++) {
		struct region_archive ra = {0};
		assert(region_archive_create(&ra, name, 0, 0, WORLD_DIM_OVERWORLD));
		loader(&ra, allow_old); struct buffer b = record(&ra); buffer_free(&b);
		assert(region_archive_destroy(&ra));
	}
	string_clear(name);
}
static void cut(int op, const char* rel) {
	(void)op; (void)rel;
	char dst[260]; snprintf(dst, sizeof dst, "%s_%u", cut_root, cuts++);
	assert(cuts <= 64); g9io_copy_tree(root[1], dst);
}
static uint32_t rng(uint32_t* r) {
	*r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5; return *r;
}
static void fill(bool entropy, unsigned revision) {
	uint32_t r = 4242 + revision;
	for(size_t k = 0; k < sizeof ids; k++) ids[k] = entropy ? rng(&r) % 85 : 1;
	for(size_t k = 0; k < sizeof meta; k++) {
		meta[k] = entropy ? rng(&r) : 0;
		sky[k] = entropy ? rng(&r) : 0xee; torch[k] = entropy ? rng(&r) : 0;
	}
	memset(height, 80, sizeof height); sc.populated = true;
}
static void config_checks(void) {
	assert(config_g9_save_zlib_level(NULL) == 6);
	const char* invalid[] = {"null", "true", "\"1\"", "0", "7", "-1", "1.5", "1e99"};
	for(size_t k = 0; k < sizeof invalid / sizeof *invalid; k++) {
		char json[160]; snprintf(json, sizeof json,
			"{\"experimental\":{\"g9_save_zlib_level_int\":%s}}", invalid[k]);
		struct config c = {.root = json_parse_string(json)}; assert(c.root);
		assert(config_g9_save_zlib_level(&c) == 6); config_destroy(&c);
	}
	struct config c = {.root = json_value_init_object()};
	assert(config_g9_save_zlib_level(&c) == 6);
	for(int level = 1; level <= 6; level++) {
		assert(config_set_int(&c, "experimental.g9_save_zlib_level_int", level));
		assert(config_g9_save_zlib_level(&c) == level);
	}
	config_destroy(&c);
}
int main(int argc, char** argv) {
	config_checks(); region_archive_io_async_enable(false);
	region_archive_test_clock(0x5F5E1234u);
	sc.ids = ids; sc.metadata = meta; sc.lighting_sky = sky;
	sc.lighting_torch = torch; sc.heightmap = height;
	struct region_archive ra[2]; struct region_encode_cache* cache = NULL;
	for(unsigned arm = 0; arm < 2; arm++) {
		test_fixture_dir(arm ? "g9_zlib_opt" : "g9_zlib_ref", root[arm], sizeof root[arm]);
		assert(!G9IO_MKDIR(root[arm])); assert(!G9IO_MKDIR(test_fixture_join(root[arm], "/region")));
		string_t name; string_init_set_str(name, root[arm]);
		assert(region_archive_create_new(&ra[arm], name, 0, 0, WORLD_DIM_OVERWORLD)); string_clear(name);
	}
	/* Same input, changing suffix, and entropy; cache stays alive across all
 * 6->1->...->6 transitions (both exact and prefix matches must miss). */
	for(unsigned kind = 0; kind < 4; kind++) for(int level = 1; level <= 6; level++)
		for(unsigned repeat = 0; repeat < 3; repeat++) {
			fill(kind == 2, kind == 1 ? repeat : 0);
			if(kind == 1) height[255] += repeat;
			if(kind == 3) {
				uint32_t r = 4242;
				for(size_t k = 0; k < 4000; k++) ids[k] = rng(&r) % 85;
				for(size_t k = 4000; k < sizeof ids; k++) ids[k] = ids[k % 4000];
			}
			region_archive_save_zlib_level_configure(6);
			assert(region_archive_set_chunk(&ra[0], 1, 1, &sc, &te, &entities));
			region_archive_save_zlib_level_configure(level);
			assert(region_archive_set_chunk_cached(&ra[1], 1, 1, &sc, &te, &entities, &cache));
			assert(region_archive_sync(&ra[0]) && region_archive_sync(&ra[1]));
			struct buffer a = record(&ra[0]), b = record(&ra[1]);
			struct buffer x = decoded(a), y = decoded(b);
			assert(x.len == y.len && !memcmp(x.data, y.data, x.len));
			/* Independent fresh encoder proves selected level, even after reuse. */
			uLongf n = compressBound(y.len); uint8_t* fresh = malloc(n); assert(fresh);
			assert(compress2(fresh, &n, y.data, y.len, level) == Z_OK);
			assert(n == b.len && !memcmp(fresh, b.data, n)); free(fresh);
			if(level == 6) assert(a.len == b.len && !memcmp(a.data, b.data, a.len));
			if(kind == 2) assert((ra[1].offsets[33] & 255) >= 19);
			if(kind == 3 && level == 1) {
				assert((ra[0].offsets[33] & 255) == 1);
				assert((ra[1].offsets[33] & 255) == 2);
			}
			loader(&ra[1], false);
			char copy[260]; snprintf(copy, sizeof copy, "%s_reload", root[1]);
			g9io_copy_tree(root[1], copy); reopen(copy, false); test_fixture_dir_cleanup(copy);
			if(argc == 2 && !repeat) {
				char dir[512], dst[540]; snprintf(dir, sizeof dir, "%s/l%d-k%u", argv[1], level, kind);
				g9io_copy_tree(root[1], dir); snprintf(dst, sizeof dst, "%s/expected.nbt", dir);
				FILE* f = fopen(dst, "wb"); assert(f && fwrite(y.data, 1, y.len, f) == y.len && !fclose(f));
			}
			buffer_free(&a); buffer_free(&b); buffer_free(&x); buffer_free(&y);
		}
	region_archive_encode_cache_free(&cache);
	assert(region_archive_destroy(&ra[0]) && region_archive_destroy(&ra[1]));
	/* Large COW record at every cut, inline and deferred publication paths.
	 * Unchanged durability suites separately exercise the worker barriers. */
	for(int arm = 0; arm <= 2; arm += 2) for(int level = 1; level <= 6; level++) {
		g9io_arm(arm); region_archive_save_zlib_level_configure(level);
		string_t name; string_init_set_str(name, root[1]);
		assert(region_archive_create(&ra[1], name, 0, 0, WORLD_DIM_OVERWORLD)); string_clear(name);
		fill(false, 0); assert(region_archive_set_chunk(&ra[1], 1, 1, &sc, &te, &entities));
		assert(region_archive_sync(&ra[1])); remember_old();
		fill(true, level); cuts = 0; snprintf(cut_root, sizeof cut_root, "%s_cut", root[1]);
		static struct g9io_trace tr; g9io_trace_begin(&tr, root[1]); g9io_point_cb = cut;
		assert(region_archive_set_chunk(&ra[1], 1, 1, &sc, &te, &entities));
		assert(region_archive_sync(&ra[1])); g9io_point_cb = NULL; g9io_trace_end();
		assert(cuts >= 5); assert(region_archive_destroy(&ra[1]));
		for(unsigned k = 0; k < cuts; k++) {
			char dst[260]; snprintf(dst, sizeof dst, "%s_%u", cut_root, k);
			reopen(dst, true); test_fixture_dir_cleanup(dst);
		}
		printf("level %d arm %d: %u large-record persistence cuts reopened twice\n", level, arm, cuts);
		/* Failed large-record payload/index writes cannot acknowledge a torn
		 * revision; clean retry must converge to the current complete image. */
		string_init_set_str(name, root[1]);
		assert(region_archive_create(&ra[1], name, 0, 0, WORLD_DIM_OVERWORLD)); string_clear(name);
		const enum region_archive_test_fail_stage stages[] = {
			REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE, REGION_ARCHIVE_TEST_FAIL_PAYLOAD_FLUSH,
			REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE, REGION_ARCHIVE_TEST_FAIL_INDEX_FLUSH,
			REGION_ARCHIVE_TEST_FAIL_SYNC};
		for(unsigned k = 0; k < 7; k++) {
			remember_old(); fill(true, level + k + 50);
			if(k < 5) region_archive_test_fail_next(stages[k]);
			else {
				__atomic_store_n(&write_fault, k - 4, __ATOMIC_RELEASE);
				region_archive_test_payload_hook(payload_fault);
			}
			bool saved = region_archive_set_chunk(&ra[1], 1, 1, &sc, &te, &entities);
			bool synced = region_archive_sync(&ra[1]); assert(!(saved && synced));
			region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_NONE);
			region_archive_test_payload_hook(NULL);
			char dst[260]; snprintf(dst, sizeof dst, "%s_fault", root[1]);
			g9io_copy_tree(root[1], dst); reopen(dst, true); test_fixture_dir_cleanup(dst);
			/* Retry clears any reported publication failure before re-encoding. */
			(void)region_archive_sync(&ra[1]);
			assert(region_archive_set_chunk(&ra[1], 1, 1, &sc, &te, &entities));
			assert(region_archive_sync(&ra[1])); loader(&ra[1], false);
		}
		/* Flip the zlib checksum in an isolated copy. The actual loader must
		 * refuse it, and an attempted preserving save must fail closed. */
		char bad[260]; snprintf(bad, sizeof bad, "%s_corrupt", root[1]);
		g9io_copy_tree(root[1], bad);
		string_init_set_str(name, bad); struct region_archive broken = {0};
		assert(region_archive_create(&broken, name, 0, 0, WORLD_DIM_OVERWORLD)); string_clear(name);
		struct buffer bytes = record(&broken);
		long last = (long)(broken.offsets[33] >> 8) * REGION_SECTOR_SIZE + 4 + (long)bytes.len;
		assert(!fseek(broken.file, last, SEEK_SET)); int byte = fgetc(broken.file); assert(byte != EOF);
		assert(!fseek(broken.file, last, SEEK_SET) && fputc(byte ^ 1, broken.file) != EOF && !fflush(broken.file));
		struct server_chunk rejected = {0};
		assert(!region_archive_get_chunk(&broken, 1, 1, &rejected, NULL, NULL));
		assert(!region_archive_set_chunk(&broken, 1, 1, &sc, &te, &entities));
		buffer_free(&bytes); assert(region_archive_destroy(&broken)); test_fixture_dir_cleanup(bad);
		assert(region_archive_destroy(&ra[1]));
	}
	for(unsigned arm = 0; arm < 2; arm++) test_fixture_dir_cleanup(root[arm]);
	region_archive_save_zlib_level_configure(6); region_archive_test_clock(0);
	assert(write_faults == 24);
	puts("test_g9_save_zlib: PASS (72 exact decoded/reuse checks, induced 1->2 sector allocation, 60 cuts reopened twice, 84 large-record faults, 24 real short-write/ENOSPC injections, 12 corrupt refusals)");
	return 0;
}
