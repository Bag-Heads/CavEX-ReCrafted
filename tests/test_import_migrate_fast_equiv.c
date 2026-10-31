/*
	IMP-MIGRATE-FAST equivalence test (T1, same output).

	The save-version chain a world load runs (server_local_save_version_chain:
	v2 wall signs, v3 iron chests, v4 spawner TEs, v5 bed metadata) now feeds
	stages 2-4 from ONE read-only, file-order survey instead of three full
	staged passes. This test runs the PRODUCTION chain twice on identical
	copies of each world -- once with the survey disabled (the pre-change
	staged behaviour, the reference) and once with it enabled -- and requires:
	  - the same chain result (load proceeds / load aborted);
	  - byte-identical world trees afterwards (every region file, level.dat,
	    any other file), with the region timestamp clock pinned;
	and, at the region_archive API level, identical per-stage stats and
	resolutions between each *_surveyed entry point and its staged pass.

	Fixtures (synthesized here, both dimensions, two overworld regions):
	  beta       Beta import: wall signs meta 2/3, spawner WITH TE, a bare
	             (genuine locked-chest) 95 -- every stage a no-op (fast path)
	  beta_spawn Beta import with a TE-less spawner (v4 must write)
	  legacy     CaveWii-created, legacy signs 6/7, TE-proven + bare 95,
	             TE-less spawner (v2 intent+migrate, v3 and v4 write)
	  mixed      Beta 2/3 AND legacy 6/7 signs -> v2 REFUSED, nothing stamped
	  crash_v2   legacy world whose sign conversion committed but whose
	             level.dat stamp did not (intent recorded, markers present)
	  v3_only    a version-3 world: only the spawner stage runs
	  corrupt    one record's payload damaged -> every stage would fail;
	             the load must abort identically
	Optional real worlds: CAVEWII_IMPORT_EQUIV_WORLDS="<dir>;<dir>" (private
	copies, never committed) are copied twice into the fixture area, run
	both ways with wall-clock timing printed, and compared the same way.
*/

#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks_data.h"
#include "cNBT/nbt.h"
#include "network/level_archive.h"
#include "network/region_archive.h"
#include "network/server_local.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "platform/time.h"

#define COLUMN_BLOCKS ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)

static char s_root[160];

static size_t column_index(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * CHUNK_SIZE) * WORLD_HEIGHT;
}
static void metadata_set(uint8_t* data, size_t index, uint8_t value) {
	unsigned shift = (unsigned)(index & 1) * 4;
	data[index / 2] = (uint8_t)((data[index / 2] & ~(0x0FU << shift))
		| ((value & 0x0F) << shift));
}

/* ---- filesystem helpers ---------------------------------------------- */

static bool is_dir(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void path_join(char* out, size_t n, const char* a, const char* b) {
	int w = snprintf(out, n, "%s/%s", a, b);
	assert(w > 0 && (size_t)w < n);
}

static void tree_remove(const char* path) {
	if(!is_dir(path)) {
		remove(path);
		return;
	}
	DIR* d = opendir(path);
	assert(d);
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char child[512];
		path_join(child, sizeof child, path, e->d_name);
		tree_remove(child);
	}
	closedir(d);
	TEST_FIXTURE_RMDIR(path);
}

static struct buffer fx_file_read(const char* path) {
	struct buffer out = {0};
	FILE* f = fopen(path, "rb");
	if(!f)
		fprintf(stderr, "cannot open %s\n", path);
	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long n = ftell(f);
	assert(n >= 0 && fseek(f, 0, SEEK_SET) == 0);
	out.data = malloc((size_t)n + 1);
	assert(out.data);
	out.len = (size_t)n;
	assert(fread(out.data, 1, out.len, f) == out.len);
	assert(fclose(f) == 0);
	return out;
}

static void fx_file_write(const char* path, const struct buffer* b) {
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(b->data, 1, b->len, f) == b->len);
	assert(fclose(f) == 0);
}

static void tree_copy(const char* src, const char* dst) {
	if(!is_dir(src)) {
		struct buffer b = fx_file_read(src);
		fx_file_write(dst, &b);
		buffer_free(&b);
		return;
	}
	assert(is_dir(dst) || MKDIR(dst) == 0);
	DIR* d = opendir(src);
	assert(d);
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char a[512], b[512];
		path_join(a, sizeof a, src, e->d_name);
		path_join(b, sizeof b, dst, e->d_name);
		tree_copy(a, b);
	}
	closedir(d);
}

/* Every file under `a` exists under `b` with identical bytes. */
static bool tree_subset_equal(const char* a, const char* b, const char* tag) {
	if(!is_dir(a)) {
		FILE* probe = fopen(b, "rb");
		if(!probe) {
			fprintf(stderr, "[%s] missing in other tree: %s\n", tag, b);
			return false;
		}
		fclose(probe);
		struct buffer x = fx_file_read(a), y = fx_file_read(b);
		bool same = x.len == y.len && !memcmp(x.data, y.data, x.len);
		if(!same)
			fprintf(stderr, "[%s] bytes differ: %s (%zu vs %zu bytes)\n", tag,
					a, x.len, y.len);
		buffer_free(&x);
		buffer_free(&y);
		return same;
	}
	if(!is_dir(b)) {
		fprintf(stderr, "[%s] not a directory in other tree: %s\n", tag, b);
		return false;
	}
	bool ok = true;
	DIR* d = opendir(a);
	assert(d);
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char x[512], y[512];
		path_join(x, sizeof x, a, e->d_name);
		path_join(y, sizeof y, b, e->d_name);
		ok = tree_subset_equal(x, y, tag) && ok;
	}
	closedir(d);
	return ok;
}

static bool tree_equal(const char* a, const char* b, const char* tag) {
	bool ab = tree_subset_equal(a, b, tag);
	bool ba = tree_subset_equal(b, a, tag);
	return ab && ba;
}

/* ---- fixture builders --------------------------------------------------- */

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
	for(int x = 0; x < CHUNK_SIZE; x++)
		for(int z = 0; z < CHUNK_SIZE; z++)
			for(int y = 0; y < 60; y++)
				c->ids[column_index(x, y, z)] = BLOCK_STONE;
}
static void fx_chunk_free(struct server_chunk* c) {
	free(c->ids);
	free(c->metadata);
	free(c->lighting_sky);
	free(c->lighting_torch);
	free(c->heightmap);
}

enum fx {
	FX_BETA,
	FX_BETA_SPAWN,
	FX_LEGACY,
	FX_MIXED,
	FX_CRASH_V2,
	FX_V3_ONLY,
	FX_CORRUPT,
};

struct fx_flags {
	uint8_t sign_meta_a, sign_meta_b; /* 0 = no sign */
	bool spawner_te, spawner_bare;
	bool chest_proven, chest_bare;
};

static void write_chunk(struct region_archive* ra, w_coord_t cx, w_coord_t cz,
						const struct fx_flags* fl) {
	struct server_chunk c;
	fx_chunk_alloc(&c);
	if(fl->sign_meta_a) {
		c.ids[column_index(3, 64, 3)] = BLOCK_SIGN;
		metadata_set(c.metadata, column_index(3, 64, 3), fl->sign_meta_a);
	}
	if(fl->sign_meta_b) {
		c.ids[column_index(4, 64, 3)] = BLOCK_SIGN;
		metadata_set(c.metadata, column_index(4, 64, 3), fl->sign_meta_b);
	}
	struct te_spawner spawner = {0};
	struct te_chest chest;
	memset(&chest, 0, sizeof chest);
	struct chunk_tile_entities te = {0};
	if(fl->spawner_te) {
		c.ids[column_index(8, 30, 8)] = BLOCK_MOB_SPAWNER;
		spawner.x = cx * CHUNK_SIZE + 8;
		spawner.y = 30;
		spawner.z = cz * CHUNK_SIZE + 8;
		snprintf(spawner.mob_id, sizeof spawner.mob_id, "Zombie");
		spawner.delay = 20;
		te.spawners = &spawner;
		te.n_spawner = 1;
	}
	if(fl->spawner_bare) {
		c.ids[column_index(9, 31, 9)] = BLOCK_MOB_SPAWNER;
		metadata_set(c.metadata, column_index(9, 31, 9), 2); /* legacy pick */
	}
	if(fl->chest_proven) {
		c.ids[column_index(2, 64, 2)] = 95;
		chest.x = cx * CHUNK_SIZE + 2;
		chest.y = 64;
		chest.z = cz * CHUNK_SIZE + 2;
		chest.items[0] = (struct item_data) {.id = 95, .count = 1};
		te.chests = &chest;
		te.n_chest = 1;
	}
	if(fl->chest_bare)
		c.ids[column_index(5, 64, 5)] = 95;
	assert(region_archive_set_blocks(ra, cx, cz, &c, &te));
	fx_chunk_free(&c);
}

struct strip {
	bool generator, version;
};
static bool keep_tag(const nbt_node* node, void* aux) {
	const struct strip* st = aux;
	if(!node->name)
		return true;
	if(st->generator && !strcmp(node->name, "CaveWiiGenerator"))
		return false;
	if(st->version && !strcmp(node->name, "CaveWiiSaveVersion"))
		return false;
	return true;
}

static void write_level(const char* dir, bool cavewii_created, int version,
						bool intent) {
	string_t world;
	string_init_set_str(world, dir);
	struct level_archive level;
	assert(level_archive_create_new(&level, world, 173,
									(vec3) {0.5F, 65.62F, 0.5F}));
	nbt_node* v = nbt_find_by_path(level.data, LEVEL_CAVEWII_SAVE_VERSION.name);
	assert(v && v->type == TAG_INT);
	v->payload.tag_int = version;
	struct strip st = {.generator = !cavewii_created,
					   .version = version == CAVEWII_SAVE_VERSION_LEGACY
						   && !cavewii_created};
	level.data = nbt_filter_inplace(level.data, keep_tag, &st);
	assert(level.data);
	char path[512];
	path_join(path, sizeof path, dir, "level.dat");
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(nbt_dump_file(level.data, f, STRAT_GZIP) == NBT_OK);
	assert(fclose(f) == 0);
	level_archive_discard(&level);
	if(intent) {
		assert(level_archive_create(&level, world));
		assert(level_archive_record_wall_sign_legacy_intent(&level));
		level_archive_discard(&level);
	}
	string_clear(world);
}

static void build_fixture(const char* dir, enum fx kind) {
	tree_remove(dir);
	assert(MKDIR(dir) == 0);
	char sub[512];
	path_join(sub, sizeof sub, dir, "region");
	assert(MKDIR(sub) == 0);
	path_join(sub, sizeof sub, dir, "DIM-1");
	assert(MKDIR(sub) == 0);
	path_join(sub, sizeof sub, dir, "DIM-1/region");
	assert(MKDIR(sub) == 0);

	struct fx_flags plain = {0};
	struct fx_flags a = {0}, b = {0}, n = {0};
	bool legacy_world = kind == FX_LEGACY || kind == FX_CRASH_V2;
	switch(kind) {
		case FX_BETA:
		case FX_V3_ONLY:
		case FX_CORRUPT:
			a = (struct fx_flags) {.sign_meta_a = 2, .sign_meta_b = 3,
								   .spawner_te = true, .chest_bare = true};
			b = (struct fx_flags) {.sign_meta_a = 5, .spawner_te = true};
			n = (struct fx_flags) {.sign_meta_a = 3};
			break;
		case FX_BETA_SPAWN:
			a = (struct fx_flags) {.sign_meta_a = 2, .spawner_te = true};
			b = (struct fx_flags) {.spawner_bare = true};
			n = (struct fx_flags) {.sign_meta_a = 3};
			break;
		case FX_LEGACY:
		case FX_CRASH_V2:
			a = (struct fx_flags) {.sign_meta_a = 6, .sign_meta_b = 7,
								   .chest_proven = true, .chest_bare = true};
			b = (struct fx_flags) {.sign_meta_a = 4, .spawner_bare = true};
			n = (struct fx_flags) {.sign_meta_a = 7, .chest_proven = true};
			break;
		case FX_MIXED:
			a = (struct fx_flags) {.sign_meta_a = 2, .sign_meta_b = 6};
			b = (struct fx_flags) {.sign_meta_a = 7, .spawner_bare = true};
			n = (struct fx_flags) {.sign_meta_a = 3};
			break;
	}

	string_t world;
	string_init_set_str(world, dir);
	struct region_archive ra;
	/* r.0.0: a scattered set of chunks, written out of header order so the
	 * file order differs from the (z,x) walk order. */
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	write_chunk(&ra, 5, 7, &a);
	write_chunk(&ra, 0, 0, &plain);
	write_chunk(&ra, 31, 31, &b);
	write_chunk(&ra, 1, 0, &plain);
	write_chunk(&ra, 0, 1, &a);
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	assert(region_archive_create_new(&ra, world, -1, 0, WORLD_DIM_OVERWORLD));
	write_chunk(&ra, -1, 3, &b);
	write_chunk(&ra, -32, 0, &plain);
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_NETHER));
	write_chunk(&ra, 2, 2, &n);
	write_chunk(&ra, 0, 0, &plain);
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));

	int version = CAVEWII_SAVE_VERSION_LEGACY;
	if(kind == FX_V3_ONLY)
		version = CAVEWII_SAVE_VERSION_IRON_CHEST_IDS;
	write_level(dir, legacy_world, version, kind == FX_CRASH_V2);

	if(kind == FX_CRASH_V2) {
		/* The sign conversion committed (per-chunk markers durable) but the
		 * level.dat version stamp never landed. */
		enum wall_sign_metadata_resolution r;
		struct wall_sign_metadata_migration_stats st;
		assert(region_archive_prepare_wall_sign_metadata(world, true, true,
														 &r, &st));
		assert(r == WALL_SIGN_METADATA_MIGRATED && st.signs_rewritten > 0);
	}
	if(kind == FX_CORRUPT) {
		/* Damage the first payload byte after r.0.0's two header sectors:
		 * the zlib header of whichever record the file holds first. */
		char path[512];
		path_join(path, sizeof path, dir, "region/r.0.0.mcr");
		struct buffer bytes = fx_file_read(path);
		assert(bytes.len > 8192 + 8);
		bytes.data[8192 + 5] ^= 0xFF;
		fx_file_write(path, &bytes);
		buffer_free(&bytes);
	}
	string_clear(world);
}

/* ---- runners ---------------------------------------------------------- */

static int read_version(const char* dir) {
	string_t world;
	string_init_set_str(world, dir);
	struct level_archive level;
	assert(level_archive_create(&level, world));
	int v = level_archive_get_save_version(&level);
	level_archive_discard(&level);
	string_clear(world);
	return v;
}

static bool run_chain(const char* dir, bool survey, double* ms) {
	server_local_test_migration_survey_enable(survey);
	string_t world;
	string_init_set_str(world, dir);
	struct level_archive level;
	assert(level_archive_create(&level, world));
	ptime_t start = time_get();
	bool ok = server_local_save_version_chain(&level, world, false, 0);
	if(ms)
		*ms = (double)time_diff_us(start, time_get()) / 1000.0;
	uint32_t pct = 0;
	assert(!server_local_import_progress(&pct) && "progress cleared at end");
	level_archive_discard(&level);
	string_clear(world);
	server_local_test_migration_survey_enable(true);
	return ok;
}

static void check_chain_equiv(const char* src, const char* name,
							  bool expect_ok, int expect_version) {
	char ref[512], fast[512];
	snprintf(ref, sizeof ref, "%s/%s_ref", s_root, name);
	snprintf(fast, sizeof fast, "%s/%s_fast", s_root, name);
	tree_remove(ref);
	tree_remove(fast);
	tree_copy(src, ref);
	tree_copy(src, fast);
	double ref_ms = 0, fast_ms = 0;
	bool ref_ok = run_chain(ref, false, &ref_ms);
	bool fast_ok = run_chain(fast, true, &fast_ms);
	int ref_v = read_version(ref), fast_v = read_version(fast);
	printf("[%s] staged: ok=%d v=%d %.1f ms | survey: ok=%d v=%d %.1f ms\n",
		   name, ref_ok, ref_v, ref_ms, fast_ok, fast_v, fast_ms);
	assert(ref_ok == fast_ok);
	assert(ref_v == fast_v);
	if(expect_version >= 0) {
		assert(ref_ok == expect_ok);
		assert(ref_v == expect_version);
	}
	assert(tree_equal(ref, fast, name) && "worlds must be byte-identical");
	tree_remove(ref);
	tree_remove(fast);
}

/* API level: every *_surveyed entry point returns the staged pass's exact
 * stats/resolution, on the stage's own starting state. */
static void check_stage_stats(const char* src, const char* name) {
	char ref[512], fast[512];
	snprintf(ref, sizeof ref, "%s/%s_sref", s_root, name);
	snprintf(fast, sizeof fast, "%s/%s_sfast", s_root, name);
	for(int stage = 2; stage <= 4; stage++) {
		for(int flags = 0; flags < (stage == 3 ? 4 : (stage == 2 ? 4 : 1));
			flags++) {
			tree_remove(ref);
			tree_remove(fast);
			tree_copy(src, ref);
			tree_copy(src, fast);
			string_t wr, wf;
			string_init_set_str(wr, ref);
			string_init_set_str(wf, fast);
			struct region_migration_survey survey;
			region_archive_migration_survey(wf, &survey, NULL, NULL);
			bool f0 = flags & 1, f1 = (flags & 2) != 0;
			if(stage == 2) {
				enum wall_sign_metadata_resolution rr, rf;
				struct wall_sign_metadata_migration_stats sr, sf;
				bool okr = region_archive_prepare_wall_sign_metadata(
					wr, f0, f1, &rr, &sr);
				bool okf = region_archive_prepare_wall_sign_metadata_surveyed(
					wf, f0, f1, &survey, &rf, &sf);
				assert(okr == okf);
				if(okr) {
					assert(rr == rf);
					assert(sr.regions_scanned == sf.regions_scanned);
					assert(sr.chunks_scanned == sf.chunks_scanned);
					assert(sr.signs_rewritten == sf.signs_rewritten);
				}
			} else if(stage == 3) {
				struct iron_chest_migration_stats sr, sf;
				bool okr
					= region_archive_migrate_iron_chest_ids(wr, f0, f1, &sr);
				bool okf = region_archive_migrate_iron_chest_ids_surveyed(
					wf, f0, f1, &survey, &sf);
				assert(okr == okf);
				if(okr)
					assert(!memcmp(&sr, &sf, sizeof sr));
			} else {
				struct mob_spawner_te_migration_stats sr, sf;
				bool okr = region_archive_migrate_mob_spawner_tes(wr, &sr);
				bool okf = region_archive_migrate_mob_spawner_tes_surveyed(
					wf, &survey, &sf);
				assert(okr == okf);
				if(okr)
					assert(!memcmp(&sr, &sf, sizeof sr));
			}
			assert(tree_equal(ref, fast, name));
			string_clear(wr);
			string_clear(wf);
		}
	}
	tree_remove(ref);
	tree_remove(fast);
}

static void progress_probe(uint32_t permille, void* ctx) {
	uint32_t* last = ctx;
	assert(permille <= 1000);
	assert(permille >= *last && "progress is monotonic");
	*last = permille;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* interleave with the chain's stderr log */
	test_fixture_dir("test_import_migrate_fast_equiv_tmp", s_root,
					 sizeof s_root);
	assert(is_dir(s_root) || MKDIR(s_root) == 0);
	region_archive_test_clock(1700000000u); /* pin per-chunk timestamps */

	static const struct {
		enum fx kind;
		const char* name;
		bool ok;
		int version;
	} cases[] = {
		{FX_BETA, "beta", true, CAVEWII_SAVE_VERSION_CURRENT},
		{FX_BETA_SPAWN, "beta_spawn", true, CAVEWII_SAVE_VERSION_CURRENT},
		{FX_LEGACY, "legacy", true, CAVEWII_SAVE_VERSION_CURRENT},
		{FX_MIXED, "mixed", true, CAVEWII_SAVE_VERSION_LEGACY},
		{FX_CRASH_V2, "crash_v2", true, CAVEWII_SAVE_VERSION_CURRENT},
		{FX_V3_ONLY, "v3_only", true, CAVEWII_SAVE_VERSION_CURRENT},
		{FX_CORRUPT, "corrupt", false, CAVEWII_SAVE_VERSION_LEGACY},
	};
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		char src[512];
		snprintf(src, sizeof src, "%s/%s_src", s_root, cases[i].name);
		build_fixture(src, cases[i].kind);
		check_chain_equiv(src, cases[i].name, cases[i].ok, cases[i].version);
		check_stage_stats(src, cases[i].name);
		tree_remove(src);
	}

	/* The fast path really is taken for a Beta import: the survey alone
	 * decides all three stages, and its progress is monotonic to 1000. */
	{
		char src[512];
		snprintf(src, sizeof src, "%s/beta_probe", s_root);
		build_fixture(src, FX_BETA);
		string_t w;
		string_init_set_str(w, src);
		struct region_migration_survey survey;
		uint32_t last = 0;
		region_archive_migration_survey(w, &survey, progress_probe, &last);
		assert(survey.valid && last == 1000);
		assert(survey.regions_scanned == 3 && survey.chunks_scanned == 9);
		assert(!survey.ws_failed && survey.ws_beta && !survey.ws_legacy);
		assert(!survey.iron_failed && survey.iron_proven_blocks == 0
			   && survey.iron_bare_blocks == 2 && survey.iron_items == 0);
		assert(!survey.spawner_failed && survey.spawners_missing_te == 0);
		string_clear(w);
		tree_remove(src);
		printf("[beta_probe] survey decides all stages (fast path)\n");
	}

	/* Private real Beta worlds (never committed): equivalence + timing. */
	const char* worlds = getenv("CAVEWII_IMPORT_EQUIV_WORLDS");
	if(worlds && *worlds) {
		char* list = malloc(strlen(worlds) + 1);
		assert(list);
		strcpy(list, worlds);
		int k = 0;
		for(char* tok = strtok(list, ";"); tok; tok = strtok(NULL, ";")) {
			char name[32];
			snprintf(name, sizeof name, "real%d", k++);
			printf("[%s] %s\n", name, tok);
			check_chain_equiv(tok, name, true, -1);
		}
		free(list);
	}

	region_archive_test_clock(0);
	test_fixture_dir_cleanup(s_root);
	printf("test_import_migrate_fast_equiv: ALL PASS\n");
	return 0;
}
