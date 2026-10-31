/* lava-aabb-0924/H1: autosave "entity storm" -- PC stress repro for the
 * intermittent far_walk_6m hang seen during an autosave window.
 *
 * The suspect (H0 deep-dive, tmp/jobs/lava-aabb-0924/H0-fable-deepdive.md) is
 * the chunk save's entity-preserve merge running long while the main thread
 * contends on the allocator lock. This test drives the REAL region save
 * (region_archive_set_chunk -> chunk_entities_preserve) for one chunk holding
 * a storm of native item records, twice, while a second thread hammers the
 * allocator (malloc/free, plus mallinfo() on the Wii where it exists), and
 * asserts: every save succeeds, the publication epoch advances, no failure
 * stage is published, and each save finishes under the 1 s budget.
 *
 * Between the two saves the stored record is rewritten BEHIND the archive to
 * the shapes the merge must cope with in a real world:
 *   - LEGACY no-Air items (one released Item schema predates the common Air
 *     scalar): the merge must byte-prove them deletable via the legacy path;
 *   - DEPARTED natives: the second snapshot lacks the last quarter of the
 *     records, so the merge must delete them (native deletion-safe path);
 *   - ONE non-native (unknown-id) record: unparseable, must be preserved
 *     byte-for-byte AND -- the load-bearing part -- it defeats the "every old
 *     record is codec-complete" fast path, forcing the full per-record merge.
 * Then a third save with current == NULL and parsed old records pins the
 * fail-closed exit (entity_archive.c "goto fail" for a parse-admitted record
 * with no current owner): the save FAILS cleanly, publishes the failure stage
 * + sub-stage, does not move the epoch, and loses no data.
 *
 * Record count: CW_STORM_COUNT in the environment (default STORM_DEFAULT).
 * The brief asked for CHUNK_ENTITY_ARCHIVE_WRITE_MAX; see main() for why the
 * default is lower and what WRITE_MAX measured. */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif
#ifdef PLATFORM_WII
#include <malloc.h>
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "network/entity_archive.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "platform/thread.h"
#include "platform/time.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define REGION_PATH test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")
#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE (CHUNK_SIZE * CHUNK_SIZE)

/* Default storm size. MEASURED 2026-09-24 on the PC build (MinGW, this test,
 * CW_STORM_BUDGET_S=600 to read past the budget), save 2 = the merge:
 *     512 records  0.141 s      (save 4, the unchanged no-op: 0.056 s)
 *    1024 records  0.276 s      (0.162 s)
 *    2048 records  0.641 s      (0.472 s)
 *    4096 records  3.744 s      (1.844 s)
 * i.e. worse than quadratic past 2048. ROOT CAUSE: once ONE old record
 * defeated the codec-complete fast path, chunk_entities_preserve's owner
 * search was O(n_old * n_current) (every old record walked the whole
 * generated list; entity_same_saved_owner = two nbt_dump_binary mallocs per
 * pair), and even the whole-record no-op path paid the full merge before it
 * could compare.
 * FIXED by lava-aabb-0924/H2 (sorted owner-key index, entity_archive.c;
 * byte-identical to the old merge -- tests/test_entity_preserve_equiv.c).
 * Same PC, same test, AFTER:
 *     512 records  0.042 s      (save 4: 0.033 s)
 *    4096 records  0.405 s      (save 4: 0.346 s)      -- was 3.744 s
 * CHUNK_ENTITY_ARCHIVE_WRITE_MAX (65,536) cannot go through THIS (region)
 * path at all: the read-back parse refuses records above cNBT's 8 MiB
 * decompressed cap (nbt_loading.c NBT_MAX_DECOMPRESSED_BYTES; 8192 item
 * records already exceed it -- read_record_nbt below trips on it), which is
 * the file-format boundary CHUNK_ENTITY_ARCHIVE_READ_MAX's comment describes.
 * The WRITE_MAX merge is timed directly in test_entity_preserve_equiv. The
 * default stays 512 so the suite's budget assert guards the constant factor
 * without a multi-hundred-MB fixture. */
#define STORM_DEFAULT 512u
#define SAVE_BUDGET_S 1.0f

/* ---- the allocator hammer (the main thread's F3 mallinfo + GUI malloc) ---- */
static bool g_hammer_run = true;
static uint32_t g_hammer_iterations;
static void* hammer_entry(void* arg) {
	(void)arg;
	uint32_t k = 0;
	while(__atomic_load_n(&g_hammer_run, __ATOMIC_ACQUIRE)) {
#ifdef PLATFORM_WII
		struct mallinfo mi = mallinfo();
		(void)mi;
#endif
		void* p = malloc(64 + (k & 1023));
		assert(p);
		((volatile char*)p)[0] = (char)k;
		free(p);
		k++;
	}
	__atomic_store_n(&g_hammer_iterations, k, __ATOMIC_RELEASE);
	return NULL;
}

/* ---- chunk + entity fixtures ---- */
static void alloc_chunk(struct server_chunk* sc) {
	memset(sc, 0, sizeof(*sc));
	sc->ids = calloc(1, IDS_SIZE);
	sc->metadata = calloc(1, NIB_SIZE);
	sc->lighting_sky = calloc(1, NIB_SIZE);
	sc->lighting_torch = calloc(1, NIB_SIZE);
	sc->heightmap = calloc(1, HM_SIZE);
	assert(sc->ids && sc->metadata && sc->lighting_sky && sc->lighting_torch
		   && sc->heightmap);
	sc->populated = true;
}

static void free_input_chunk(struct server_chunk* sc) {
	free(sc->ids);
	free(sc->metadata);
	free(sc->lighting_sky);
	free(sc->lighting_torch);
	free(sc->heightmap);
}

static struct archived_entity* make_items(size_t n) {
	struct archived_entity* e = calloc(n, sizeof(*e));
	assert(e);
	for(size_t i = 0; i < n; i++) {
		e[i].kind = ARCHIVED_ENTITY_ITEM;
		e[i].source_id = (uint32_t)i + 1;
		e[i].pos[0] = (float)(i % 16) + 0.5f;
		e[i].pos[1] = 64.0f + (float)((i / 256) % 32);
		e[i].pos[2] = (float)((i / 16) % 16) + 0.5f;
		e[i].fire = -1;
		e[i].air = 300;
		e[i].health = 5;
		e[i].data.item.item.id = 4; /* cobblestone */
		e[i].data.item.item.count = 1;
		e[i].data.item.age = (int32_t)(i % 6000);
	}
	return e;
}

/* ---- raw .mcr record rewrite (behind the archive handle) ---- */
static void put_be32(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}
static uint32_t get_be32(const uint8_t* in) {
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16)
		| ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static nbt_node* read_record_nbt(void) {
	FILE* f = fopen(REGION_PATH, "rb");
	assert(f);
	uint8_t raw[4];
	assert(fread(raw, 4, 1, f) == 1); /* index entry of chunk (0,0) */
	uint32_t entry = get_be32(raw);
	uint32_t offset = entry >> 8;
	assert(offset >= 2 && (entry & 0xFF) > 0);
	assert(fseek(f, (long)offset * REGION_SECTOR_SIZE, SEEK_SET) == 0);
	assert(fread(raw, 4, 1, f) == 1);
	uint32_t length = get_be32(raw);
	assert(length >= 2);
	uint8_t* data = malloc(length);
	assert(data);
	assert(fread(data, length, 1, f) == 1);
	assert(fclose(f) == 0);
	assert(data[0] == 2 && "MCR zlib record");
	nbt_node* tree = nbt_parse_compressed(data + 1, length - 1);
	free(data);
	assert(tree && "record parses (an 8 MiB+ decompressed record is refused by "
				   "cNBT: the region path cannot carry WRITE_MAX records)");
	return tree;
}

/* Append the rewritten record at EOF, repoint the index, clear the
 * timestamp (bit 31 is the CANONICAL flag: a canonical record skips the
 * read-back and with it the very merge under test). */
static void write_record_nbt(nbt_node* tree) {
	struct buffer res = nbt_dump_compressed(tree, STRAT_INFLATE);
	assert(res.data && res.len > 0);
	FILE* f = fopen(REGION_PATH, "rb+");
	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long end = ftell(f);
	assert(end > 0 && end % REGION_SECTOR_SIZE == 0);
	uint32_t offset = (uint32_t)(end / REGION_SECTOR_SIZE);
	size_t record = 4 + 1 + res.len;
	uint32_t sectors = (uint32_t)((record + REGION_SECTOR_SIZE - 1)
								  / REGION_SECTOR_SIZE);
	assert(sectors > 0 && sectors < 256);
	uint8_t hdr[5];
	put_be32(hdr, (uint32_t)(1 + res.len));
	hdr[4] = 2;
	assert(fwrite(hdr, sizeof(hdr), 1, f) == 1);
	assert(fwrite(res.data, res.len, 1, f) == 1);
	size_t pad = (size_t)sectors * REGION_SECTOR_SIZE - record;
	if(pad) {
		uint8_t* zero = calloc(1, pad);
		assert(zero);
		assert(fwrite(zero, pad, 1, f) == 1);
		free(zero);
	}
	uint8_t raw[4];
	put_be32(raw, (offset << 8) | sectors);
	assert(fseek(f, 0, SEEK_SET) == 0);
	assert(fwrite(raw, 4, 1, f) == 1);
	put_be32(raw, 0);
	assert(fseek(f, REGION_SECTOR_SIZE, SEEK_SET) == 0);
	assert(fwrite(raw, 4, 1, f) == 1);
	assert(fclose(f) == 0);
	buffer_free(&res);
}

static nbt_node* compound_child(nbt_node* comp, const char* name) {
	assert(comp && comp->type == TAG_COMPOUND);
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* child = list_entry(pos, struct nbt_list, entry)->data;
		if(child && child->name && !strcmp(child->name, name))
			return child;
	}
	return NULL;
}

static size_t list_count(const nbt_node* list) {
	assert(list && list->type == TAG_LIST);
	size_t n = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry)
		n++;
	return n;
}

/* Strip "Air" from the first `legacy` item records and append one clone
 * re-labelled with an id the codec does not know. */
static void inject_legacy_shapes(size_t legacy) {
	nbt_node* tree = read_record_nbt();
	nbt_node* list = nbt_find_by_path(tree, ".Level.Entities");
	assert(list && list->type == TAG_LIST);
	size_t k = 0;
	nbt_node* first = NULL;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry) {
		nbt_node* rec = list_entry(pos, struct nbt_list, entry)->data;
		if(!first)
			first = rec;
		if(k++ >= legacy)
			break;
		struct list_head *p, *next;
		list_for_each_safe(p, next, &rec->payload.tag_compound->entry) {
			struct nbt_list* w = list_entry(p, struct nbt_list, entry);
			if(w->data && w->data->name && !strcmp(w->data->name, "Air")) {
				list_del(p);
				nbt_free(w->data);
				free(w);
				break;
			}
		}
	}
	assert(first);
	nbt_node* foreign = nbt_clone(first);
	assert(foreign);
	nbt_node* id = compound_child(foreign, "id");
	assert(id && id->type == TAG_STRING);
	free(id->payload.tag_string);
	id->payload.tag_string = strdup("StormForeignKind");
	assert(id->payload.tag_string);
	/* A NATIVE record carries CaveWiiType, which the codec trusts over the
	 * id string (kind_from_compound): keeping it would make this a mangled
	 * native item -- parse-admitted, not byte-provable, fail-closed by design
	 * (CEP_NATIVE_UNSAFE). A foreign record has no CaveWii* tags at all. */
	{
		struct list_head *p, *next;
		list_for_each_safe(p, next, &foreign->payload.tag_compound->entry) {
			struct nbt_list* w2 = list_entry(p, struct nbt_list, entry);
			if(w2->data && w2->data->name
			   && !strncmp(w2->data->name, "CaveWii", 7)) {
				list_del(p);
				nbt_free(w2->data);
				free(w2);
			}
		}
	}
	struct nbt_list* w = malloc(sizeof(*w));
	assert(w);
	w->data = foreign;
	list_add_tail(&w->entry, &list->payload.tag_list->entry);
	write_record_nbt(tree);
	nbt_free(tree);
}

static size_t raw_entity_records(void) {
	nbt_node* tree = read_record_nbt();
	nbt_node* list = nbt_find_by_path(tree, ".Level.Entities");
	size_t n = list ? list_count(list) : 0;
	nbt_free(tree);
	return n;
}

static bool open_region(struct region_archive* ra, string_t world) {
	FILE* probe = fopen(REGION_PATH, "rb");
	if(!probe) /* server_world's own first-write path: create_new */
		return region_archive_create_new(ra, world, 0, 0, WORLD_DIM_OVERWORLD);
	fclose(probe);
	return region_archive_create(ra, world, 0, 0, WORLD_DIM_OVERWORLD);
}

static float timed_save(struct region_archive* ra, struct server_chunk* sc,
						const struct chunk_tile_entities* te,
						const struct chunk_entities* ents, bool* ok) {
	ptime_t t0 = time_get();
	*ok = region_archive_set_chunk(ra, 0, 0, sc, te, ents);
	return time_diff_s(t0, time_get());
}

static size_t parsed_entities_on_disk(struct region_archive* ra) {
	struct server_chunk loaded = {0};
	struct chunk_entities out = {0};
	assert(region_archive_get_chunk(ra, 0, 0, &loaded, NULL, &out));
	size_t n = out.count;
	chunk_entities_free(&out);
	server_chunk_arrays_free(&loaded);
	return n;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* keep the timings if an assert fires */
	size_t n = STORM_DEFAULT;
	const char* env = getenv("CW_STORM_COUNT");
	if(env && atol(env) > 0)
		n = (size_t)atol(env);
	/* Measurement runs only: CW_STORM_BUDGET_S relaxes the per-save budget so
	 * the scaling can be read past the point where the default would abort. */
	float budget = SAVE_BUDGET_S;
	env = getenv("CW_STORM_BUDGET_S");
	if(env && atof(env) > 0.0)
		budget = (float)atof(env);
	assert(n <= CHUNK_ENTITY_ARCHIVE_WRITE_MAX);
	size_t legacy = n / 4, departed = n / 4;
	printf("storm: %zu native items (%zu legacy no-Air, %zu departed, 1 "
		   "foreign)\n", n, legacy, departed);

	test_fixture_dir("test_autosave_entity_storm_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(REGION_PATH);
	string_t world;
	string_init_set_str(world, TMP_WORLD);

	struct thread hammer;
	assert(thread_create(&hammer, hammer_entry, NULL, 64));

	struct server_chunk sc;
	alloc_chunk(&sc);
	sc.ids[0] = 1;
	struct chunk_tile_entities te = {.legacy_flat_complete = true};
	struct archived_entity* items = make_items(n);
	struct chunk_entities all = {.entities = items, .count = n};
	struct chunk_entities survivors = {.entities = items, .count = n - departed};

	uint32_t fail0 = region_save_failure_count();
	uint32_t skip0 = region_save_skip_unchanged_count();

	/* ---- save 1: fresh record (no read-back, plain dump) ---- */
	struct region_archive ra;
	assert(open_region(&ra, world));
	uint32_t epoch0 = region_archive_save_epoch(&ra);
	bool ok;
	float s1 = timed_save(&ra, &sc, &te, &all, &ok);
	printf("save 1 (fresh, %zu records): ok=%d %.3fs\n", n, ok, s1);
	assert(ok);
	assert(region_archive_save_epoch(&ra) > epoch0);
	assert(region_save_failure_count() == fail0);
	assert(s1 < budget);
	assert(region_archive_destroy(&ra));
	assert(raw_entity_records() == n);

	/* ---- rewrite behind the archive: legacy shapes + one foreign record ---- */
	inject_legacy_shapes(legacy);
	assert(raw_entity_records() == n + 1);

	/* ---- save 2: the storm merge (read-back + preserve, fast path defeated) ---- */
	assert(open_region(&ra, world));
	uint32_t epoch1 = region_archive_save_epoch(&ra);
	sc.ids[0] = 2; /* block delta so the record cannot be a whole-record no-op */
	float s2 = timed_save(&ra, &sc, &te, &survivors, &ok);
	printf("save 2 (merge: %zu old + 1 foreign vs %zu current): ok=%d %.3fs\n",
		   n + 1, survivors.count, ok, s2);
	if(!ok)
		fprintf(stderr, "  fail stage=%u sub=%u\n", (unsigned)region_save_failure_stage(),
			   region_save_failure_sub());
	assert(ok);
	assert(region_archive_save_epoch(&ra) > epoch1);
	assert(region_save_failure_count() == fail0);
	assert(region_save_failure_stage() == RSF_NONE || fail0 > 0);
	assert(s2 < budget);
	assert(region_archive_destroy(&ra));
	/* departed natives deleted, foreign record byte-preserved, survivors kept */
	assert(raw_entity_records() == survivors.count + 1);
	assert(open_region(&ra, world));
	assert(parsed_entities_on_disk(&ra) == survivors.count);

	/* ---- save 3: current == NULL with parsed old records -> fail closed ---- */
	uint32_t epoch2 = region_archive_save_epoch(&ra);
	sc.ids[0] = 3;
	float s3 = timed_save(&ra, &sc, &te, NULL, &ok);
	printf("save 3 (current==NULL, %zu parsed old): ok=%d %.3fs\n",
		   survivors.count, ok, s3);
	assert(!ok && "a NULL snapshot over parsed native records must fail closed");
	assert(region_archive_save_epoch(&ra) == epoch2);
	assert(region_save_failure_count() == fail0 + 1);
	assert(region_save_failure_stage() == RSF_ENTITY_PRESERVE);
	assert(region_save_failure_sub() == (uint32_t)CEP_IMPORTED_UNMATCHED);
	assert(chunk_entities_preserve_last_fail() == CEP_IMPORTED_UNMATCHED);
	assert(s3 < budget);
	/* no data loss: the stored record is the save-2 record, untouched */
	assert(parsed_entities_on_disk(&ra) == survivors.count);
	{
		struct server_chunk loaded = {0};
		assert(region_archive_get_blocks(&ra, 0, 0, &loaded, NULL));
		assert(loaded.ids[0] == 2);
		server_chunk_arrays_free(&loaded);
	}

	/* ---- save 4: identical snapshot again = whole-record no-op (skip) ---- */
	sc.ids[0] = 2;
	float s4 = timed_save(&ra, &sc, &te, &survivors, &ok);
	printf("save 4 (unchanged): ok=%d %.3fs skip_n=%u\n", ok, s4,
		   region_save_skip_unchanged_count() - skip0);
	assert(ok);
	assert(region_save_skip_unchanged_count() == skip0 + 1);
	assert(region_save_failure_count() == fail0 + 1);
	assert(region_archive_destroy(&ra));

	__atomic_store_n(&g_hammer_run, false, __ATOMIC_RELEASE);
	thread_join(&hammer);
	printf("allocator hammer: %u malloc/free cycles alongside the saves\n",
		   __atomic_load_n(&g_hammer_iterations, __ATOMIC_ACQUIRE));
	assert(__atomic_load_n(&g_hammer_iterations, __ATOMIC_ACQUIRE) > 0);

	free(items);
	free_input_chunk(&sc);
	remove(REGION_PATH);
	string_clear(world);
	test_fixture_dir_cleanup(TMP_WORLD);
	puts("test_autosave_entity_storm: PASS");
	return 0;
}
