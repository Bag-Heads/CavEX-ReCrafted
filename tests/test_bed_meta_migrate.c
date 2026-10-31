/*
	PAR-BED-META save-version-5 migration test.

	CaveWii saves before version 5 stored bed metadata as index<<1 over
	{+Z,-Z,+X,-X} (0/2/4/6, head |8). Beta 1.7.3 stores meta&3 over
	headBlockToFootBlockMap {+Z,-X,-Z,+X}, 0x4 occupied, 0x8 head
	(RetroMCP BlockBed.java:7,135-145; source/block/bed_meta.h). The load
	stage server_local_migrate_bed_metadata_stage converts a CaveWii-created
	world exactly once and stamps version 5; a real Beta import is stamped
	without a byte of region data changing.

	Fixture world (region r.0.0 overworld + DIM-1 r.0.0):
	  overworld chunk (0,0): eight beds holding every legacy value
	      0,2,4,6,8,10,12,14 at y=64, plus a STONE with metadata 4 (never
	      touched -- only block 26 is a bed);
	  overworld chunk (1,0): only identity beds (legacy 0 and 10 == Beta 0
	      and 10) -> never rewritten;
	  nether chunk (0,0): one legacy +X head (12) -> both dimensions run.

	Proven:
	  1) CaveWii-created, version 4: converted to Beta values (4->3, 6->1,
	     12->11, 14->9, identities kept), stone untouched, version 5 durable.
	  2) A second load (version 5) is a no-op.
	  3) Crash-retry idempotency: re-running the region pass on the already
	     converted world (version still 4 after a crash) rewrites nothing --
	     the per-chunk marker stops a second conversion (4->3->2 would
	     corrupt).
	  4) Imported real-Beta world (no CaveWiiGenerator), version 4: region
	     files byte-identical, version stamped 5.
	  5) A world below version 4 (an earlier stage declined) is not touched
	     and not stamped.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/bed_meta.h"
#include "block/blocks_data.h"
#include "cNBT/nbt.h"
#include "network/level_archive.h"
#include "network/region_archive.h"
#include "network/server_local.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"

static char s_world[128];

#define COLUMN_BLOCKS ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)

static size_t column_index(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * CHUNK_SIZE) * WORLD_HEIGHT;
}
static void metadata_set(uint8_t* data, size_t index, uint8_t value) {
	unsigned shift = (unsigned)(index & 1) * 4;
	data[index / 2] = (uint8_t)((data[index / 2] & ~(0x0FU << shift))
		| ((value & 0x0F) << shift));
}
static uint8_t metadata_get(const uint8_t* data, size_t index) {
	return (data[index / 2] >> ((index & 1) * 4)) & 0x0F;
}

static const uint8_t k_legacy[8] = {0, 2, 4, 6, 8, 10, 12, 14};
static const uint8_t k_beta[8] = {0, 2, 3, 1, 8, 10, 11, 9};

static void fixture_remove(void) {
	remove(test_fixture_join(s_world, "/level.dat"));
	remove(test_fixture_join(s_world, "/level.dat_new"));
	remove(test_fixture_join(s_world, "/level.dat_old"));
	remove(test_fixture_join(s_world, "/region/r.0.0.mcr"));
	remove(test_fixture_join(s_world, "/DIM-1/region/r.0.0.mcr"));
	RMDIR(test_fixture_join(s_world, "/DIM-1/region"));
	RMDIR(test_fixture_join(s_world, "/DIM-1"));
	RMDIR(test_fixture_join(s_world, "/region"));
	RMDIR(s_world);
}

static void fx_chunk_alloc(struct server_chunk* c) {
	memset(c, 0, sizeof(*c));
	c->ids = calloc(COLUMN_BLOCKS, 1);
	c->metadata = calloc(COLUMN_BLOCKS / 2, 1);
	c->lighting_sky = calloc(COLUMN_BLOCKS / 2, 1);
	c->lighting_torch = calloc(COLUMN_BLOCKS / 2, 1);
	c->heightmap = calloc(CHUNK_SIZE * CHUNK_SIZE, 1);
	assert(c->ids && c->metadata && c->lighting_sky && c->lighting_torch
		   && c->heightmap);
	c->populated = true;
}
static void fx_chunk_free(struct server_chunk* c) {
	free(c->ids);
	free(c->metadata);
	free(c->lighting_sky);
	free(c->lighting_torch);
	free(c->heightmap);
}

struct strip {
	bool generator;
};
static bool keep_tag(const nbt_node* node, void* aux) {
	const struct strip* st = aux;
	return !(node->name && st->generator
			 && strcmp(node->name, "CaveWiiGenerator") == 0);
}

static void write_level(string_t world, bool cavewii_created, int version) {
	struct level_archive level;
	assert(level_archive_create_new(&level, world, 173,
									(vec3) {0.5F, 65.62F, 0.5F}));
	nbt_node* v = nbt_find_by_path(level.data, LEVEL_CAVEWII_SAVE_VERSION.name);
	assert(v && v->type == TAG_INT);
	v->payload.tag_int = version;
	struct strip st = {.generator = !cavewii_created};
	level.data = nbt_filter_inplace(level.data, keep_tag, &st);
	assert(level.data);
	FILE* f = fopen(test_fixture_join(s_world, "/level.dat"), "wb");
	assert(f);
	assert(nbt_dump_file(level.data, f, STRAT_GZIP) == NBT_OK);
	assert(fclose(f) == 0);
	level_archive_discard(&level);
}

static void fixture_create(bool cavewii_created, int version) {
	fixture_remove();
	assert(MKDIR(s_world) == 0);
	assert(MKDIR(test_fixture_join(s_world, "/region")) == 0);
	assert(MKDIR(test_fixture_join(s_world, "/DIM-1")) == 0);
	assert(MKDIR(test_fixture_join(s_world, "/DIM-1/region")) == 0);
	string_t world;
	string_init_set_str(world, s_world);

	struct server_chunk c;
	struct region_archive region;
	assert(region_archive_create_new(&region, world, 0, 0, WORLD_DIM_OVERWORLD));
	fx_chunk_alloc(&c);
	for(int i = 0; i < 8; i++) {
		size_t idx = column_index(1 + i, 64, 3);
		c.ids[idx] = BLOCK_BED;
		metadata_set(c.metadata, idx, k_legacy[i]);
	}
	c.ids[column_index(12, 64, 12)] = BLOCK_STONE;
	metadata_set(c.metadata, column_index(12, 64, 12), 4);
	assert(region_archive_set_blocks(&region, 0, 0, &c, NULL));
	fx_chunk_free(&c);
	fx_chunk_alloc(&c);
	c.ids[column_index(2, 64, 2)] = BLOCK_BED;
	metadata_set(c.metadata, column_index(2, 64, 2), 0);
	c.ids[column_index(2, 64, 3)] = BLOCK_BED;
	metadata_set(c.metadata, column_index(2, 64, 3), 10);
	assert(region_archive_set_blocks(&region, 1, 0, &c, NULL));
	fx_chunk_free(&c);
	assert(region_archive_sync(&region));
	assert(region_archive_destroy(&region));

	assert(region_archive_create_new(&region, world, 0, 0, WORLD_DIM_NETHER));
	fx_chunk_alloc(&c);
	c.ids[column_index(5, 40, 5)] = BLOCK_BED;
	metadata_set(c.metadata, column_index(5, 40, 5), 12);
	assert(region_archive_set_blocks(&region, 0, 0, &c, NULL));
	fx_chunk_free(&c);
	assert(region_archive_sync(&region));
	assert(region_archive_destroy(&region));

	write_level(world, cavewii_created, version);
	string_clear(world);
}

static uint8_t read_meta(enum world_dim dim, int cx, int x, int y, int z,
						 uint8_t* id_out) {
	string_t world;
	string_init_set_str(world, s_world);
	struct region_archive region;
	assert(region_archive_create(&region, world, 0, 0, dim));
	struct server_chunk loaded = {0};
	struct chunk_tile_entities te = {0};
	assert(region_archive_get_blocks(&region, cx, 0, &loaded, &te));
	assert(region_archive_destroy(&region));
	size_t idx = column_index(x, y, z);
	if(id_out)
		*id_out = loaded.ids[idx];
	uint8_t m = metadata_get(loaded.metadata, idx);
	tile_entities_parse_free(&te);
	server_chunk_arrays_free(&loaded);
	string_clear(world);
	return m;
}

static int read_version(void) {
	string_t world;
	string_init_set_str(world, s_world);
	struct level_archive level;
	assert(level_archive_create(&level, world));
	int v = level_archive_get_save_version(&level);
	level_archive_discard(&level);
	string_clear(world);
	return v;
}

static bool run_stage(struct bed_metadata_migration_stats* st, bool* ran,
					  bool* converted) {
	string_t world;
	string_init_set_str(world, s_world);
	struct level_archive level;
	assert(level_archive_create(&level, world));
	bool ok = server_local_migrate_bed_metadata_stage(&level, world, st, ran,
													  converted);
	level_archive_destroy(&level); /* persists the stamp like a real load */
	string_clear(world);
	return ok;
}

static struct buffer read_file(const char* rel) {
	struct buffer out = {0};
	FILE* f = fopen(test_fixture_join(s_world, rel), "rb");
	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long n = ftell(f);
	assert(n >= 0 && fseek(f, 0, SEEK_SET) == 0);
	out.data = malloc((size_t)n + 1);
	out.len = (size_t)n;
	assert(out.data);
	assert(fread(out.data, 1, out.len, f) == out.len);
	assert(fclose(f) == 0);
	return out;
}
static bool same_bytes(struct buffer a, struct buffer b) {
	return a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
}

static void assert_overworld(const uint8_t* want) {
	for(int i = 0; i < 8; i++) {
		uint8_t id = 0;
		uint8_t m = read_meta(WORLD_DIM_OVERWORLD, 0, 1 + i, 64, 3, &id);
		if(id != BLOCK_BED || m != want[i]) {
			fprintf(stderr, "bed %d: id=%d meta=%d want %d\n", i, id, m,
					want[i]);
			assert(0 && "bed metadata mismatch");
		}
	}
	uint8_t id = 0;
	assert(read_meta(WORLD_DIM_OVERWORLD, 0, 12, 64, 12, &id) == 4
		   && id == BLOCK_STONE && "non-bed blocks are never touched");
	assert(read_meta(WORLD_DIM_OVERWORLD, 1, 2, 64, 2, NULL) == 0);
	assert(read_meta(WORLD_DIM_OVERWORLD, 1, 2, 64, 3, NULL) == 10);
}

int main(void) {
	test_fixture_dir("test_bed_meta_migrate_tmp", s_world, sizeof s_world);
	struct bed_metadata_migration_stats st;
	bool ran, converted;

	/* 1) CaveWii-created, version 4 -> converted once, version 5. */
	fixture_create(true, CAVEWII_SAVE_VERSION_MOB_SPAWNER_TES);
	assert(run_stage(&st, &ran, &converted));
	assert(ran && converted);
	assert(st.regions_scanned == 2 && st.chunks_scanned == 3);
	assert(st.beds_seen == 11);
	assert(st.beds_rewritten == 5); /* 4,6,12,14 overworld + 12 nether */
	assert_overworld(k_beta);
	assert(read_meta(WORLD_DIM_NETHER, 0, 5, 40, 5, NULL) == 11);
	assert(read_version() == CAVEWII_SAVE_VERSION_BED_META);
	assert(CAVEWII_SAVE_VERSION_CURRENT == CAVEWII_SAVE_VERSION_BED_META);
	printf("  CaveWii world v4: legacy beds -> Beta meta, stamped v5 OK\n");

	/* 2) A later load (version 5) never runs the stage again. */
	struct buffer ow_before = read_file("/region/r.0.0.mcr");
	assert(run_stage(&st, &ran, &converted));
	assert(!ran && !converted && st.beds_rewritten == 0);
	struct buffer ow_after = read_file("/region/r.0.0.mcr");
	assert(same_bytes(ow_before, ow_after));
	free(ow_after.data);
	assert_overworld(k_beta);
	printf("  v5 reload: no-op OK\n");

	/* 3) Crash-retry: the region pass itself, re-run over converted data
	 * (as if the crash hit before the level.dat stamp), must not convert
	 * twice -- the marked chunks are skipped. */
	{
		string_t world;
		string_init_set_str(world, s_world);
		assert(region_archive_migrate_bed_metadata(world, &st));
		string_clear(world);
	}
	assert(st.beds_rewritten == 0);
	assert(st.chunks_already_marked == 2); /* overworld (0,0) + nether (0,0) */
	assert_overworld(k_beta);
	assert(read_meta(WORLD_DIM_NETHER, 0, 5, 40, 5, NULL) == 11);
	ow_after = read_file("/region/r.0.0.mcr");
	assert(same_bytes(ow_before, ow_after));
	free(ow_before.data);
	free(ow_after.data);
	printf("  crash-retry rerun: marked chunks skipped, no double conversion OK\n");

	/* 4) Imported real-Beta world: same bytes (here they ARE Beta values),
	 * no CaveWiiGenerator -> never converted, but stamped 5. */
	fixture_create(false, CAVEWII_SAVE_VERSION_MOB_SPAWNER_TES);
	struct buffer imp_ow = read_file("/region/r.0.0.mcr");
	struct buffer imp_nether = read_file("/DIM-1/region/r.0.0.mcr");
	assert(run_stage(&st, &ran, &converted));
	assert(ran && !converted && st.beds_rewritten == 0 && st.chunks_scanned == 0);
	struct buffer imp_ow2 = read_file("/region/r.0.0.mcr");
	struct buffer imp_nether2 = read_file("/DIM-1/region/r.0.0.mcr");
	assert(same_bytes(imp_ow, imp_ow2) && same_bytes(imp_nether, imp_nether2)
		   && "a real Beta import's regions must be byte-identical");
	free(imp_ow.data);
	free(imp_ow2.data);
	free(imp_nether.data);
	free(imp_nether2.data);
	assert_overworld(k_legacy); /* untouched values, read as-is */
	assert(read_version() == CAVEWII_SAVE_VERSION_BED_META);
	printf("  imported Beta world: regions byte-identical, stamped v5 OK\n");

	/* 5) Below version 4 (an earlier stage declined): untouched, unstamped. */
	fixture_create(true, CAVEWII_SAVE_VERSION_IRON_CHEST_IDS);
	assert(run_stage(&st, &ran, &converted));
	assert(!ran && !converted);
	assert_overworld(k_legacy);
	assert(read_version() == CAVEWII_SAVE_VERSION_IRON_CHEST_IDS);
	printf("  version 3 world: stage waits for version 4 OK\n");

	fixture_remove();
	printf("test_bed_meta_migrate: PASS\n");
	test_fixture_dir_cleanup(s_world);
	return 0;
}
