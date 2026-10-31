/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	dropin-1029 (maintainer-approved; findings tmp/jobs/dropin-1029/REPORT.md):
	real Minecraft Beta 1.7.3 worlds load drop-in.

	  D1  origin / notice: import_convert_notice_needed() is FALSE for a real
	      Beta world with no CaveWii evidence (the JAR-made fixture; a server
	      level.dat without Player; a bare world) and for a CaveWii world;
	      TRUE whenever there is CaveWii-made evidence (a CaveWii* chunk tag, a
	      template-born Player), which wins over Beta evidence (review M1). The
	      answer is cached per level.dat path + size + mtime (review M2).
	  D2  the production save-version chain on COPIES of real Beta worlds:
	      every region file byte-identical, the version stamped current, and
	      level.dat written ONCE -- level.dat_old afterwards is byte-for-byte
	      the level.dat Beta wrote. Always: tests/fixtures/beta_import/world.
	      Extra worlds: CW_DROPIN_EXTRA_WORLDS=<dir>;<dir> (the 30 MB / 173 MB
	      real worlds are too large to commit; run recorded in RETURN.md).
	  D3  wall signs, decided PER CHUNK (review B1): a chunk with its own
	      LastUpdate > 0 (ChunkLoader :112), no CaveWii* tag and signs in 2..5
	      (ItemSign.onItemUse :10-41; BlockSign.setBlockBoundsBasedOnState
	      :37-51) keeps them and is never rewritten; every other chunk follows
	      the unchanged world rule (no evidence -> 4/5 REFUSED). Staged pass ==
	      survey; the decision writes nothing; a mixed world converts only the
	      CaveWii chunk.
	  D4  review B2: a power cut right after stage 3 leaves version 3 on disk
	      (stages 2 and 3 rewrote regions, so each stamp was flushed), and the
	      resumed chain migrates nothing twice.
*/
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif
#include "support/test_fixture_dir.h"
#include "cNBT/nbt.h"
#include "network/level_archive.h"
#include "network/region_archive.h"
#include "network/server_local.h"
#include "game/gui/import_convert_notice.h"

static char s_root[512];

/* ---- small file helpers ------------------------------------------------- */
static unsigned char* slurp(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* buf = malloc(n > 0 ? (size_t)n : 1);
	assert(buf);
	assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static void file_copy(const char* from, const char* to) {
	size_t n;
	unsigned char* b = slurp(from, &n);
	assert(b);
	FILE* f = fopen(to, "wb");
	assert(f && fwrite(b, 1, n, f) == n && fclose(f) == 0);
	free(b);
}

static void tree_copy(const char* from, const char* to) {
	assert(MKDIR(to) == 0);
	DIR* dir = opendir(from);
	assert(dir);
	struct dirent* e;
	while((e = readdir(dir))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char a[1024], b[1024];
		snprintf(a, sizeof a, "%s/%s", from, e->d_name);
		snprintf(b, sizeof b, "%s/%s", to, e->d_name);
		if(test_fixture_is_dir(from, e->d_name))
			tree_copy(a, b);
		else
			file_copy(a, b);
	}
	closedir(dir);
}

/* Every regular file under a/<sub> equals b/<sub>; returns files compared. */
static unsigned tree_equal(const char* a, const char* b) {
	DIR* dir = opendir(a);
	if(!dir)
		return 0;
	unsigned files = 0;
	struct dirent* e;
	while((e = readdir(dir))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char ap[1024], bp[1024];
		snprintf(ap, sizeof ap, "%s/%s", a, e->d_name);
		snprintf(bp, sizeof bp, "%s/%s", b, e->d_name);
		if(test_fixture_is_dir(a, e->d_name)) {
			files += tree_equal(ap, bp);
			continue;
		}
		size_t an, bn;
		unsigned char* ab = slurp(ap, &an);
		unsigned char* bb = slurp(bp, &bn);
		if(!ab || !bb || an != bn || memcmp(ab, bb, an)) {
			printf("FAIL D2 region bytes differ: %s\n", bp);
			assert(!"region file changed");
		}
		free(ab);
		free(bb);
		files++;
	}
	closedir(dir);
	return files;
}

/* ---- level.dat builders -------------------------------------------------- */
struct strip {
	bool generator, version, player;
};
static bool keep_tag(const nbt_node* node, void* aux) {
	const struct strip* st = aux;
	if(!node->name)
		return true;
	if(st->generator && !strcmp(node->name, "CaveWiiGenerator"))
		return false;
	if(st->version && !strcmp(node->name, "CaveWiiSaveVersion"))
		return false;
	if(st->player && !strcmp(node->name, "Player"))
		return false;
	return true;
}

/* A level.dat from the CaveWii template (its Player is template-born: no
 * Score/Sleeping/SleepTimer). keep_generator=false strips CaveWiiGenerator +
 * CaveWiiSaveVersion (a tagless world); keep_generator=true strips only the
 * version (a CaveWii world at legacy version 0). no_player drops Player (the
 * shape a Beta SERVER writes). */
static void make_world(const char* dir, bool keep_generator, bool no_player) {
	char region[600], path[600];
	assert(MKDIR(dir) == 0);
	snprintf(region, sizeof region, "%s/region", dir);
	assert(MKDIR(region) == 0);
	string_t world;
	string_init_set_str(world, dir);
	struct level_archive level;
	assert(level_archive_create_new(&level, world, 173,
									(vec3) {0.5F, 65.62F, 0.5F}));
	struct strip st = {.generator = !keep_generator, .version = true,
					   .player = no_player};
	level.data = nbt_filter_inplace(level.data, keep_tag, &st);
	assert(level.data);
	snprintf(path, sizeof path, "%s/level.dat", dir);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(nbt_dump_file(level.data, f, STRAT_GZIP) == NBT_OK);
	assert(fclose(f) == 0);
	level_archive_discard(&level);
	string_clear(world);
}

/* ---- raw Beta chunk + McRegion writer (independent of region_archive) ---- */
struct nbtw {
	unsigned char* b;
	size_t n, cap;
};
static void w_raw(struct nbtw* w, const void* p, size_t n) {
	if(w->n + n > w->cap) {
		w->cap = (w->n + n) * 2;
		w->b = realloc(w->b, w->cap);
		assert(w->b);
	}
	memcpy(w->b + w->n, p, n);
	w->n += n;
}
static void w_u8(struct nbtw* w, unsigned v) {
	unsigned char c = (unsigned char)v;
	w_raw(w, &c, 1);
}
static void w_be(struct nbtw* w, uint64_t v, int bytes) {
	for(int i = bytes - 1; i >= 0; i--)
		w_u8(w, (unsigned)((v >> (8 * i)) & 0xFF));
}
static void w_name(struct nbtw* w, unsigned type, const char* name) {
	w_u8(w, type);
	w_be(w, strlen(name), 2);
	w_raw(w, name, strlen(name));
}
static void w_bytes(struct nbtw* w, const char* name, const unsigned char* d,
					size_t n) {
	w_name(w, TAG_BYTE_ARRAY, name);
	w_be(w, n, 4);
	w_raw(w, d, n);
}

#define MAX_SIGNS 4
struct chunk_spec {
	int cx;				  /* chunk x in region 0.0, z = 0 */
	int64_t last_update;
	bool cavewii_marker;
	int signs[MAX_SIGNS]; /* wall-sign metadata, -1 terminated */
	bool bare_iron95;	  /* a bare legacy iron chest (block 95, no TE) */
};
/* Sign k at local (1+k, 64, 1); the bare 95 at (8, 64, 8). Beta
 * ChunkLoader.storeChunkInCompound field set; Blocks index y + z*128 + x*2048,
 * Data nibble low = even index. */
#define SIGN_INDEX(k) ((size_t)(64 + 1 * 128 + (1 + (k)) * 2048))
#define IRON_INDEX ((size_t)(64 + 8 * 128 + 8 * 2048))

static void chunk_nbt(const struct chunk_spec* cs, struct nbtw* w) {
	static unsigned char blocks[32768], data[16384], light[16384], hm[256];
	memset(blocks, 0, sizeof blocks);
	memset(data, 0, sizeof data);
	memset(hm, 0, sizeof hm);
	for(int i = 0; i < 32768; i++)
		if((i & 127) < 60)
			blocks[i] = 1; /* stone below y 60 */
	for(int k = 0; k < MAX_SIGNS && cs->signs[k] >= 0; k++) {
		size_t idx = SIGN_INDEX(k);
		unsigned shift = (unsigned)(idx & 1) * 4;
		blocks[idx] = 68;
		data[idx / 2] = (unsigned char)((data[idx / 2] & ~(0x0Fu << shift))
										| ((unsigned)(cs->signs[k] & 0x0F) << shift));
	}
	if(cs->bare_iron95)
		blocks[IRON_INDEX] = 95;
	w_name(w, TAG_COMPOUND, "");
	w_name(w, TAG_COMPOUND, "Level");
	w_name(w, TAG_INT, "xPos");
	w_be(w, (uint32_t)cs->cx, 4);
	w_name(w, TAG_INT, "zPos");
	w_be(w, 0, 4);
	w_name(w, TAG_LONG, "LastUpdate");
	w_be(w, (uint64_t)cs->last_update, 8);
	w_name(w, TAG_BYTE, "TerrainPopulated");
	w_u8(w, 1);
	w_bytes(w, "Blocks", blocks, sizeof blocks);
	w_bytes(w, "Data", data, sizeof data);
	memset(light, 0xFF, sizeof light);
	w_bytes(w, "SkyLight", light, sizeof light);
	memset(light, 0, sizeof light);
	w_bytes(w, "BlockLight", light, sizeof light);
	w_bytes(w, "HeightMap", hm, sizeof hm);
	w_name(w, TAG_LIST, "Entities");
	w_u8(w, TAG_COMPOUND);
	w_be(w, 0, 4);
	w_name(w, TAG_LIST, "TileEntities");
	w_u8(w, TAG_COMPOUND);
	w_be(w, 0, 4);
	if(cs->cavewii_marker) {
		w_name(w, TAG_BYTE, "CaveWiiTileEntitiesComplete");
		w_u8(w, 1);
	}
	w_u8(w, 0); /* TAG_End: Level */
	w_u8(w, 0); /* TAG_End: root */
}

static void write_region_n(const char* dir, const struct chunk_spec* cs, int n) {
	size_t cap = 8192 + (size_t)n * 64 * 4096;
	unsigned char* file = calloc(1, cap);
	assert(file);
	size_t sector = 2;
	for(int c = 0; c < n; c++) {
		struct nbtw w = {0};
		chunk_nbt(&cs[c], &w);
		uLongf zlen = compressBound(w.n);
		unsigned char* z = malloc(zlen);
		assert(z && compress2(z, &zlen, w.b, w.n, 6) == Z_OK);
		size_t sectors = (5 + zlen + 4095) / 4096;
		assert(sector + sectors <= cap / 4096);
		size_t slot = (size_t)cs[c].cx * 4; /* local (cx, 0) */
		file[slot + 0] = (unsigned char)(sector >> 16);
		file[slot + 1] = (unsigned char)(sector >> 8);
		file[slot + 2] = (unsigned char)sector;
		file[slot + 3] = (unsigned char)sectors;
		file[4096 + slot + 3] = 1; /* timestamp */
		unsigned char* rec = file + sector * 4096;
		uint32_t len = (uint32_t)zlen + 1;
		rec[0] = (unsigned char)(len >> 24);
		rec[1] = (unsigned char)(len >> 16);
		rec[2] = (unsigned char)(len >> 8);
		rec[3] = (unsigned char)len;
		rec[4] = 2;
		memcpy(rec + 5, z, zlen);
		sector += sectors;
		free(z);
		free(w.b);
	}
	char path[700];
	snprintf(path, sizeof path, "%s/region/r.0.0.mcr", dir);
	FILE* f = fopen(path, "wb");
	size_t total = sector * 4096;
	assert(f && fwrite(file, 1, total, f) == total && fclose(f) == 0);
	free(file);
}
static void write_region(const char* dir, const struct chunk_spec* cs) {
	write_region_n(dir, cs, 1);
}

/* Block id + metadata at a raw index, through the production reader. */
static void read_block(const char* dir, int cx, size_t idx, uint8_t* id,
					   uint8_t* meta) {
	static uint8_t ids[32768], data[16384];
	string_t world;
	string_init_set_str(world, dir);
	struct region_archive ra;
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_get_block_ids_metadata(&ra, cx, 0, ids, data)
		   == REGION_ARCHIVE_READ_OK);
	assert(region_archive_destroy(&ra));
	string_clear(world);
	*id = ids[idx];
	*meta = (uint8_t)((data[idx / 2] >> ((idx & 1) * 4)) & 0x0F);
}

static enum level_world_origin origin_of(const char* dir, bool* notice) {
	string_t world;
	string_init_set_str(world, dir);
	struct level_archive la;
	assert(level_archive_create(&la, world));
	enum level_world_origin o = level_archive_world_origin(&la);
	*notice = import_convert_notice_needed(&la);
	level_archive_destroy(&la);
	string_clear(world);
	return o;
}

static void sub(char* out, size_t cap, const char* name) {
	snprintf(out, cap, "%s/%s", s_root, name);
}

#define SIGNS(...) {__VA_ARGS__, -1}
#define NOSIGN {-1}

/* ---- D1 ---------------------------------------------------------------- */
static void check_origin(void) {
	char d[600], sp[700], lp[700];
	bool notice;

	/* Real JAR-made Beta world (Player has Sleeping/SleepTimer; all 32 of its
	 * chunks carry LastUpdate 0 -- why LastUpdate is not origin evidence). */
	assert(origin_of("tests/fixtures/beta_import/world", &notice)
		   == LEVEL_ORIGIN_BETA);
	assert(!notice);

	/* Beta server world: no Player; Beta chunks. */
	sub(d, sizeof d, "server_beta");
	make_world(d, false, true);
	write_region(d, &(struct chunk_spec) {.last_update = 98765, .signs = NOSIGN});
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_BETA && !notice);

	/* No Player, no chunks, no CaveWii evidence anywhere -> Beta. */
	sub(d, sizeof d, "bare");
	make_world(d, false, true);
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_BETA && !notice);

	/* Pre-06-11 CavEX dev save, server-style level.dat, CaveWii chunk. */
	sub(d, sizeof d, "cavex_chunk");
	make_world(d, false, true);
	write_region(d, &(struct chunk_spec) {.cavewii_marker = true, .signs = NOSIGN});
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_CAVEWII_LEGACY && notice);

	/* Review M1: CaveWii evidence beats Beta evidence -- a Beta chunk resaved
	 * by CaveWii (LastUpdate kept + tag). */
	sub(d, sizeof d, "beta_resaved");
	make_world(d, false, true);
	write_region(d, &(struct chunk_spec) {.last_update = 5, .cavewii_marker = true,
										  .signs = NOSIGN});
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_CAVEWII_LEGACY && notice);

	/* Review M1: Beta-born Player (the real fixture's level.dat) + a CaveWii
	 * chunk = a Beta world played in an old CaveWii -> backup question. */
	sub(d, sizeof d, "beta_played_old");
	assert(MKDIR(d) == 0);
	snprintf(sp, sizeof sp, "%s/region", d);
	assert(MKDIR(sp) == 0);
	snprintf(lp, sizeof lp, "%s/level.dat", d);
	file_copy("tests/fixtures/beta_import/world/level.dat", lp);
	write_region(d, &(struct chunk_spec) {.last_update = 77, .cavewii_marker = true,
										  .signs = NOSIGN});
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_CAVEWII_LEGACY && notice);

	/* Review M2: cached per level.dat (path + size + mtime): region gone,
	 * level.dat untouched -> same answer, no re-probe. */
	snprintf(sp, sizeof sp, "%s/region/r.0.0.mcr", d);
	assert(remove(sp) == 0);
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_CAVEWII_LEGACY && notice);

	/* Pre-06-11 CavEX dev save with its template-born Player. */
	sub(d, sizeof d, "cavex_player");
	make_world(d, false, false);
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_CAVEWII_LEGACY && notice);

	/* A current CaveWii world. */
	sub(d, sizeof d, "cavewii");
	make_world(d, true, false);
	assert(origin_of(d, &notice) == LEVEL_ORIGIN_CAVEWII && !notice);
	puts("PASS D1 origin/notice: real Beta (fixture, server, bare) -> no tag; "
		 "any CaveWii evidence (CaveWii chunk, resaved Beta chunk, Beta player "
		 "+ CaveWii chunk, template Player) -> tag; cached per level.dat; "
		 "CaveWii world -> no tag");
}

/* ---- D2 ---------------------------------------------------------------- */
static void chain_on_copy(const char* src, const char* label) {
	char dst[700], a[800], b[800];
	snprintf(dst, sizeof dst, "%s/chain_%s", s_root, label);
	tree_copy(src, dst);
	snprintf(a, sizeof a, "%s/level.dat", src);
	size_t orig_n;
	unsigned char* orig = slurp(a, &orig_n);
	assert(orig);

	string_t world;
	string_init_set_str(world, dst);
	struct level_archive la;
	assert(level_archive_create(&la, world));
	assert(level_archive_world_origin(&la) == LEVEL_ORIGIN_BETA);
	assert(!import_convert_notice_needed(&la));
	assert(server_local_save_version_chain(&la, world, false, 0));
	assert(level_archive_get_save_version(&la) == CAVEWII_SAVE_VERSION_CURRENT);
	level_archive_discard(&la);

	/* level.dat_old is exactly the level.dat Beta wrote: one rotation. */
	snprintf(b, sizeof b, "%s/level.dat_old", dst);
	size_t old_n;
	unsigned char* old = slurp(b, &old_n);
	if(!old || old_n != orig_n || memcmp(old, orig, orig_n)) {
		printf("FAIL D2 %s: level.dat_old is not Beta's level.dat "
			   "(%zu vs %zu bytes)\n", label, old ? old_n : 0, orig_n);
		assert(!"level.dat_old overwritten");
	}
	free(old);
	free(orig);

	/* Every region byte unchanged, both dimensions. */
	unsigned files = 0;
	snprintf(a, sizeof a, "%s/region", src);
	snprintf(b, sizeof b, "%s/region", dst);
	files += tree_equal(a, b);
	snprintf(a, sizeof a, "%s/DIM-1/region", src);
	snprintf(b, sizeof b, "%s/DIM-1/region", dst);
	files += tree_equal(a, b);
	assert(files > 0);

	assert(level_archive_create(&la, world));
	assert(level_archive_get_save_version(&la) == CAVEWII_SAVE_VERSION_CURRENT);
	assert(!import_convert_notice_needed(&la));
	level_archive_destroy(&la);
	string_clear(world);
	printf("PASS D2 %s: chain stamped v%d, %u region files byte-identical, "
		   "level.dat_old == Beta's level.dat\n",
		   label, CAVEWII_SAVE_VERSION_CURRENT, files);
	test_fixture_rm_rf(dst);
}

/* ---- D3 ---------------------------------------------------------------- */
static enum wall_sign_metadata_resolution resolve_world(const char* d) {
	char mcr[700];
	snprintf(mcr, sizeof mcr, "%s/region/r.0.0.mcr", d);
	size_t before_n;
	unsigned char* before = slurp(mcr, &before_n);
	string_t world;
	string_init_set_str(world, d);
	enum wall_sign_metadata_resolution staged, surveyed;
	struct wall_sign_metadata_migration_stats st;
	assert(region_archive_prepare_wall_sign_metadata(world, false, false,
													 &staged, &st));
	struct region_migration_survey survey;
	memset(&survey, 0, sizeof survey);
	region_archive_migration_survey(world, &survey, NULL, NULL);
	assert(region_archive_prepare_wall_sign_metadata_surveyed(
		world, false, false, &survey, &surveyed, &st));
	assert(staged == surveyed);
	size_t after_n;
	unsigned char* after = slurp(mcr, &after_n);
	assert(after_n == before_n && !memcmp(before, after, before_n));
	free(before);
	free(after);
	string_clear(world);
	return staged;
}

static void run_chain(const char* d, bool expect_ok) {
	string_t world;
	string_init_set_str(world, d);
	struct level_archive la;
	assert(level_archive_create(&la, world));
	assert(server_local_save_version_chain(&la, world, false, 0) == expect_ok);
	level_archive_discard(&la);
	string_clear(world);
}

static int32_t disk_version(const char* d) {
	string_t world;
	string_init_set_str(world, d);
	struct level_archive la;
	assert(level_archive_create(&la, world));
	int32_t v = level_archive_get_save_version(&la);
	level_archive_discard(&la);
	string_clear(world);
	return v;
}

static enum wall_sign_metadata_resolution sign_case(const char* name,
													const struct chunk_spec* cs) {
	char d[600];
	sub(d, sizeof d, name);
	make_world(d, false, true);
	write_region(d, cs);
	return resolve_world(d);
}

static void check_wall_signs(void) {
	char d[600];
	uint8_t id, meta;
	/* A Beta-proven chunk (its own LastUpdate > 0, no CaveWii* tag, signs in
	 * 2..5) keeps every Beta value; the full chain stamps it current. */
	for(int m = 2; m <= 5; m++) {
		char n[32];
		snprintf(n, sizeof n, "sign_beta_%d", m);
		assert(sign_case(n, &(struct chunk_spec) {.last_update = 4242,
												  .signs = SIGNS(m)})
			   == WALL_SIGN_METADATA_ALREADY_BETA);
		if(m == 4) {
			sub(d, sizeof d, n);
			run_chain(d, true);
			assert(disk_version(d) == CAVEWII_SAVE_VERSION_CURRENT);
			read_block(d, 0, SIGN_INDEX(0), &id, &meta);
			assert(id == 68 && meta == 4);
		}
	}
	/* Review B1b: no evidence either way (LastUpdate 0, no tag) -> refused. */
	assert(sign_case("sign_noevidence_5", &(struct chunk_spec) {.signs = SIGNS(5)})
		   == WALL_SIGN_METADATA_REFUSED_UNKNOWN);
	/* A CaveWii* tag means the chunk is not Beta-proven: 4 stays ambiguous. */
	assert(sign_case("sign_resaved_4",
					 &(struct chunk_spec) {.last_update = 7, .cavewii_marker = true,
										   .signs = SIGNS(4)})
		   == WALL_SIGN_METADATA_REFUSED_UNKNOWN);
	/* Outside both encodings: not Beta-proven, never authorizes. */
	assert(sign_case("sign_bad_0", &(struct chunk_spec) {.last_update = 9,
														 .signs = SIGNS(0)})
		   == WALL_SIGN_METADATA_REFUSED_UNKNOWN);
	assert(sign_case("sign_bad_9", &(struct chunk_spec) {.last_update = 9,
														 .signs = SIGNS(9)})
		   == WALL_SIGN_METADATA_REFUSED_UNKNOWN);
	assert(sign_case("sign_legacy_6", &(struct chunk_spec) {.last_update = 9,
															.signs = SIGNS(6)})
		   == WALL_SIGN_METADATA_NEEDS_LEGACY_INTENT);

	/* Review B1a: mixed world. Chunk 0 = untouched imported Beta chunk with an
	 * east-facing 4; chunk 1 = CaveWii-resaved (tag, LastUpdate 0) with legacy
	 * signs 4 and 6. The Beta chunk must neither approve chunk 1's 4 nor be
	 * rewritten itself -> legacy intent; the chain converts chunk 1 only. */
	sub(d, sizeof d, "sign_mixed");
	make_world(d, false, true);
	struct chunk_spec mixed[2] = {
		{.cx = 0, .last_update = 31337, .signs = SIGNS(4)},
		{.cx = 1, .cavewii_marker = true, .signs = SIGNS(4, 6)},
	};
	write_region_n(d, mixed, 2);
	assert(resolve_world(d) == WALL_SIGN_METADATA_NEEDS_LEGACY_INTENT);
	run_chain(d, true);
	assert(disk_version(d) == CAVEWII_SAVE_VERSION_CURRENT);
	read_block(d, 0, SIGN_INDEX(0), &id, &meta);
	assert(id == 68 && meta == 4); /* Beta chunk untouched */
	read_block(d, 1, SIGN_INDEX(0), &id, &meta);
	assert(id == 68 && meta == 2); /* legacy 4 -> Beta 2 */
	read_block(d, 1, SIGN_INDEX(1), &id, &meta);
	assert(id == 68 && meta == 4); /* legacy 6 -> Beta 4 */
	puts("PASS D3 wall signs: per-chunk Beta proof (own LastUpdate>0, no "
		 "CaveWii tag, 2..5) keeps 2..5; staged == survey; the decision writes "
		 "nothing; no-evidence 4/5 refused; tagged 4 / 0 / 9 refused; 6 needs "
		 "intent; mixed world converts only the CaveWii chunk");
}

/* ---- D4 (review B2): power cut between two region-writing stages -------- */
static void check_power_cut(void) {
	char d[600];
	uint8_t id, meta;
	sub(d, sizeof d, "power_cut");
	make_world(d, true, false); /* CaveWii world at version 0 */
	write_region(d, &(struct chunk_spec) {.signs = SIGNS(6), .bare_iron95 = true});
	assert(disk_version(d) == CAVEWII_SAVE_VERSION_LEGACY);

	/* Stage 2 rewrites the sign and stage 3 the bare 95; the power fails
	 * right after stage 3. Both rewrote region bytes, so both stamps must
	 * already be on disk: version 3. A batched-only chain leaves 0 here, and
	 * the next load would rerun both stages over migrated data. */
	server_local_test_chain_power_cut_after(CAVEWII_SAVE_VERSION_IRON_CHEST_IDS);
	run_chain(d, false);
	assert(disk_version(d) == CAVEWII_SAVE_VERSION_IRON_CHEST_IDS);
	read_block(d, 0, SIGN_INDEX(0), &id, &meta);
	assert(id == 68 && meta == 4);
	read_block(d, 0, IRON_INDEX, &id, &meta);
	assert(id == 200);

	/* Power back: the chain resumes at stage 4; nothing migrates twice. */
	run_chain(d, true);
	assert(disk_version(d) == CAVEWII_SAVE_VERSION_CURRENT);
	read_block(d, 0, SIGN_INDEX(0), &id, &meta);
	assert(id == 68 && meta == 4);
	read_block(d, 0, IRON_INDEX, &id, &meta);
	assert(id == 200);
	puts("PASS D4 power cut after stage 3: v3 durable (stages 2+3 rewrote "
		 "regions), resume to v5, sign 6->4 once, iron 95->200 once");
}

int main(void) {
	char base[512];
	assert(test_fixture_dir("test_dropin_beta_import", base, sizeof base));
	snprintf(s_root, sizeof s_root, "%s", base);
	assert(MKDIR(s_root) == 0);
	check_origin();
	chain_on_copy("tests/fixtures/beta_import/world", "fixture");
	const char* extra = getenv("CW_DROPIN_EXTRA_WORLDS");
	if(extra && *extra) {
		char list[4096];
		snprintf(list, sizeof list, "%s", extra);
		int i = 0;
		for(char* p = strtok(list, ";"); p; p = strtok(NULL, ";")) {
			char label[32];
			snprintf(label, sizeof label, "extra%d", i++);
			chain_on_copy(p, label);
		}
	} else {
		puts("NOTE D2 extra real worlds not given (CW_DROPIN_EXTRA_WORLDS); "
			 "fixture world only");
	}
	check_wall_signs();
	check_power_cut();
	test_fixture_dir_cleanup(s_root);
	puts("test_dropin_beta_import: PASS");
	return 0;
}
