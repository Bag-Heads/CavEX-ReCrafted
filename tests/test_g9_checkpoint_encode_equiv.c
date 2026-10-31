/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Cached vs real uncached codec: exact region/sidecar bytes and ordered
 * persistence traces at EVERY barrier, including native TE changes, an
 * unknown imported root tag, eviction, high-entropy bypass and optional
 * cache OOM. Link wrappers inject cache allocation faults only; no engine
 * test profile or production failpoint is added.
 */
#include <errno.h>
#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "network/entity_archive.h"
#include "item/item_nbt.h"
#include "tps_observer.h"

static bool compressing, fail_owner, fail_hit;
static unsigned fail_mem2, faults;
#define INPUT_HINT (32768 + 3 * 16384 + 256 + 4096)
void* __real_malloc(size_t n);
void* __real_calloc(size_t n, size_t size);
void* __real_mem2_alloc_strict(size_t n);
void* __wrap_malloc(size_t n) {
	if(compressing && fail_hit && n != INPUT_HINT) {
		fail_hit = false; faults++; errno = ENOMEM; return NULL;
	}
	return __real_malloc(n);
}
void* __wrap_calloc(size_t n, size_t size) {
	if(compressing && fail_owner) {
		fail_owner = false; faults++; errno = ENOMEM; return NULL;
	}
	return __real_calloc(n, size);
}
void* __wrap_mem2_alloc_strict(size_t n) {
	if(compressing && fail_mem2 && --fail_mem2 == 0) {
		faults++; errno = ENOMEM; return NULL;
	}
	return __real_mem2_alloc_strict(n);
}
static void part(enum tps_action action, unsigned which) {
	if(which == TPS_REGION_COMPRESS) compressing = action == TPS_ENTER;
}
static char root[2][220];
static struct region_archive ra[2];
static struct g9io_trace traces[2];
static struct region_encode_cache* cache;
static struct server_chunk sc;
static uint8_t ids[32768], meta[16384], sky[16384], torch[16384], height[256];
static struct te_chest chest;
static struct te_furnace furnace;
static struct chunk_tile_entities te;
static struct chunk_entities entities;
static nbt_node* read_tree(unsigned arm);

static void compare_bytes(void) {
	struct g9io_snap a, b;
	g9io_snap_take(&a, root[0]); g9io_snap_take(&b, root[1]);
	assert(g9io_snap_equal(&a, &b, "checkpoint encode"));
	for(size_t k = 0; k < a.n; k++) {
		char path[2][512]; FILE* f[2];
		for(unsigned arm = 0; arm < 2; arm++) {
			snprintf(path[arm], sizeof path[arm], "%s%s", root[arm], a.f[k].path);
			f[arm] = fopen(path[arm], "rb"); assert(f[arm]);
		}
		for(;;) {
			uint8_t bytes[2][4096];
			size_t x = fread(bytes[0], 1, sizeof bytes[0], f[0]);
			size_t y = fread(bytes[1], 1, sizeof bytes[1], f[1]);
			assert(x == y && !memcmp(bytes[0], bytes[1], x));
			if(!x) break;
		}
		for(unsigned arm = 0; arm < 2; arm++) {
			assert(!ferror(f[arm])); assert(!fclose(f[arm]));
		}
	}
	for(unsigned arm = 0; arm < 2; arm++) {
		nbt_node* tree = read_tree(arm); assert(tree);
		nbt_node* exact = item_nbt_clone_lossless(tree); assert(exact);
		struct buffer old = nbt_dump_binary(exact);
		struct buffer fast = nbt_dump_binary_lossless_hint(tree, INPUT_HINT);
		assert(old.data && fast.data && old.len == fast.len && !memcmp(old.data, fast.data, old.len));
		const unsigned char* image; size_t length;
		assert(item_nbt_scope_image(tree, &image, &length));
		assert(item_nbt_scope_writable(tree) == (old.len == length && !memcmp(old.data, image, length)));
		buffer_free(&old); buffer_free(&fast); nbt_free(exact); nbt_free(tree);
	}
}
#define TEST_IDX(x, y, z) (((x) * 16u + (z)) * 128u + (y))
static nbt_node* read_tree(unsigned arm) {
	FILE* f = ra[arm].file;
	uint32_t sector = ra[arm].offsets[33] >> 8;
	assert(!fseek(f, (long)sector * REGION_SECTOR_SIZE, SEEK_SET));
	uint8_t header[5]; assert(fread(header, 1, 5, f) == 5 && header[4] == 2);
	size_t n = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16)
		| ((uint32_t)header[2] << 8) | header[3];
	assert(n > 1 && n < 1024 * 1024);
	uint8_t* bytes = malloc(n - 1); assert(bytes);
	assert(fread(bytes, 1, n - 1, f) == n - 1);
	nbt_node* tree = nbt_parse_compressed(bytes, n - 1);
	free(bytes); return tree;
}
static void import_marker(unsigned arm) {
	nbt_node* tree = read_tree(arm);
	assert(tree);
	nbt_node* marker = calloc(1, sizeof *marker); assert(marker);
	marker->type = TAG_INT;
	marker->name = malloc(5); assert(marker->name); memcpy(marker->name, "kept", 5);
	marker->payload.tag_int = 42;
	assert(item_nbt_replace(tree, "kept", marker));
	struct buffer record = nbt_dump_compressed(tree, STRAT_INFLATE);
	assert(record.data && region_archive_recommit_record(&ra[arm], 1, 1, &record));
	buffer_free(&record); nbt_free(tree);
}
static uint32_t random_byte(uint32_t* r) {
	*r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5;
	return *r & 255;
}
static void fill(unsigned step) {
	const unsigned state = step & 1;
	memset(ids, 1, sizeof ids); memset(meta, state, sizeof meta);
	memset(sky, state ? 0x9f : 0xee, sizeof sky);
	memset(torch, state ? 0x76 : 0x12, sizeof torch);
	memset(height, 80 + state, sizeof height);
	if(step >= 26) { /* compressed record exceeds the optional cache cap */
		uint32_t r = 4242 + state;
		for(unsigned k = 0; k < sizeof ids; k++) ids[k] = random_byte(&r);
		for(unsigned k = 0; k < sizeof meta; k++) {
			meta[k] = random_byte(&r); sky[k] = random_byte(&r); torch[k] = random_byte(&r);
		}
	}
	ids[TEST_IDX(1, 63, 5)] = 54;
	ids[TEST_IDX(6, 64, 9)] = 61;
	sc.populated = true;
	chest.items[0] = (struct item_data){.id = 278, .count = 1, .durability = 1500 + state};
	furnace.burn_time = 100 + (step >= 12 ? step / 2 : 0);
	furnace.cook_time = 170 + state;
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	/* An outer string containing NUL cannot be re-emitted verbatim; neither
	 * scope validator may authorize rewriting it. Retained item bodies with
	 * no End byte are also refused, matching clone_lossless's NULL result. */
	const uint8_t nul_scope[] = {10,0,0,8,0,1,'x',0,3,'a',0,'b',0};
	nbt_node* bad = nbt_parse(nul_scope, sizeof nul_scope); assert(bad);
	assert(!item_nbt_scope_writable(bad)); nbt_free(bad);
	struct nbt_list empty; INIT_LIST_HEAD(&empty.entry);
	nbt_node marker = {.type=TAG_BYTE_ARRAY, .payload.tag_byte_array={.length=0,.data=NULL}};
	empty.data = &marker;
	nbt_node invalid = {.type=TAG_COMPOUND, .name=NULL, .payload.tag_compound=&empty};
	assert(!item_nbt_clone_lossless(&invalid));
	struct buffer refused = nbt_dump_binary_lossless_hint(&invalid, 0);
	assert(!refused.data);
	region_archive_io_async_enable(false);
	region_archive_test_clock(0x5F5E1234u);
	__atomic_store_n(&region_tps_observe, part, __ATOMIC_RELEASE);
	sc.ids = ids; sc.metadata = meta; sc.lighting_sky = sky;
	sc.lighting_torch = torch; sc.heightmap = height;
	chest = (struct te_chest){.x = 17, .y = 63, .z = 21};
	furnace = (struct te_furnace){.x = 22, .y = 64, .z = 25};
	furnace.items[0] = (struct item_data){.id = 15, .count = 8};
	te = (struct chunk_tile_entities){.chests = &chest, .n_chest = 1,
		.furnaces = &furnace, .n_furnace = 1, .legacy_flat_complete = true};
	fill(1);
	for(unsigned arm = 0; arm < 2; arm++) {
		test_fixture_dir(arm ? "g9_encode_cached" : "g9_encode_reference", root[arm], sizeof root[arm]);
		G9IO_MKDIR(root[arm]); G9IO_MKDIR(test_fixture_join(root[arm], "/region"));
		string_t level; string_init_set_str(level, root[arm]);
		assert(region_archive_create_new(&ra[arm], level, 0, 0, WORLD_DIM_OVERWORLD));
		string_clear(level);
		assert(region_archive_set_chunk(&ra[arm], 1, 1, &sc, &te, &entities));
		import_marker(arm);
		assert(region_archive_sync(&ra[arm]));
	}
	compare_bytes();
	for(unsigned step = 0; step < 32; step++) {
		fill(step);
		for(unsigned arm = 0; arm < 2; arm++) {
			region_archive_test_encode_reference(!arm);
			g9io_trace_begin(&traces[arm], root[arm]);
			if(arm) {
				fail_owner = step == 0;
				fail_mem2 = step == 1 ? 1 : step == 2 ? 2 : 0;
				fail_hit = step == 5;
				assert(region_archive_set_chunk_cached(&ra[arm], 1, 1, &sc, &te, &entities, &cache));
				assert(!fail_owner && !fail_mem2 && !fail_hit);
			} else assert(region_archive_set_chunk(&ra[arm], 1, 1, &sc, &te, &entities));
			assert(region_archive_sync(&ra[arm]));
			g9io_trace_end();
		}
		assert(g9io_trace_equal(&traces[0], &traces[1], "encode barrier"));
		compare_bytes();
	}
	uint32_t stats[4]; region_archive_encode_cache_stats(cache, stats);
	assert(faults == 4 && stats[0] >= 5 && stats[1] >= 2 && stats[3] > 0
		&& stats[2] <= 1133 * 1024);
	printf("encode memo hits %u (prefix %u) misses %u retained %u bytes\n",
		stats[0], stats[3], stats[1], stats[2]);
	/* Fail EVERY allocator position in a cold prefix encoder (five working
	 * allocations, five Copy allocations, prefix output, full key/value).
	 * Also fail all five hit-Copy allocations and its key/value replacement.
	 * In particular Copy's first-allocation failure aliases source->state in
	 * zlib: the fallback must not End that borrowed state. Retry after every
	 * failure proves a retained prefix remains usable. */
	fill(12);
	region_archive_test_encode_reference(true);
	assert(region_archive_set_chunk(&ra[0], 1, 1, &sc, &te, &entities));
	region_archive_test_encode_reference(false);
	assert(region_archive_set_chunk_cached(&ra[1], 1, 1, &sc, &te, &entities, &cache));
	assert(region_archive_sync(&ra[0]) && region_archive_sync(&ra[1]));
	compare_bytes();
	for(unsigned cold = 0; cold < 2; cold++)
		for(unsigned nth = 1; nth <= (cold ? 13u : 7u); nth++) {
			if(cold) region_archive_encode_cache_free(&cache);
			fill(12);
			furnace.burn_time = 300 + 3 * nth + cold * 60;
			for(unsigned retry = 0; retry < 2; retry++) {
				for(unsigned arm = 0; arm < 2; arm++) {
					region_archive_test_encode_reference(!arm);
					g9io_trace_begin(&traces[arm], root[arm]);
					if(arm) {
						fail_mem2 = retry ? 0 : nth;
						unsigned before = faults;
						assert(region_archive_set_chunk_cached(&ra[arm], 1, 1, &sc, &te, &entities, &cache));
						if(fail_mem2 || faults != before + !retry)
							printf("OOM injection missed: cold %u nth %u retry %u remaining %u faults %u/%u\n",
								cold, nth, retry, fail_mem2, faults, before);
						assert(!fail_mem2 && faults == before + !retry);
					} else assert(region_archive_set_chunk(&ra[arm], 1, 1, &sc, &te, &entities));
					assert(region_archive_sync(&ra[arm]));
					g9io_trace_end();
				}
				assert(g9io_trace_equal(&traces[0], &traces[1], "prefix OOM barrier"));
				compare_bytes();
				furnace.burn_time++; /* retry uses the same prefix, a NEW tail */
			}
		}
	assert(faults == 24);
	region_archive_test_encode_reference(false);
	printf("optional codec OOM fallbacks: %u; byte-equal retries at 40 more barriers\n", faults);
	region_archive_encode_cache_free(&cache); assert(!cache);
	for(unsigned arm = 0; arm < 2; arm++) {
		nbt_node* tree = read_tree(arm);
		assert(tree);
		nbt_node* marker = item_nbt_child(tree, "kept");
		assert(marker && marker->type == TAG_INT && marker->payload.tag_int == 42);
		nbt_free(tree);
		assert(region_archive_destroy(&ra[arm])); test_fixture_dir_cleanup(root[arm]);
	}
	__atomic_store_n(&region_tps_observe, NULL, __ATOMIC_RELEASE);
	region_archive_test_clock(0);
	puts("test_g9_checkpoint_encode_equiv: PASS (uncached deflate, imports/TE changes, all compressed "
		"region+sidecar bytes and ordered persistence equal at 72 barriers, streaming lossless, 24 OOM fallbacks)");
	return 0;
}
