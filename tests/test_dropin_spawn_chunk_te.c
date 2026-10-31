/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	hw1029c (data loss): a real Beta 1.7.3 map shipped WITHOUT a Player tag
	(the usual downloadable-map shape; level.dat LastPlayed != 0) loads at the
	world spawn. The load's spawn probe (server_local_anti_suffocate_column)
	loads the spawn chunk synchronously. It used to run BEFORE the load's
	world-boundary resets (flat-archive restore commit, slot-0 TE row release,
	dict_entity_reset), which dropped every chest/sign/furnace row that chunk
	had just inserted while the chunk itself stayed resident. Symptom on the
	real map (Dream Journal, chunk (10,-2)): blank signs / empty chests in play,
	and the unload checkpoint refused the chunk ("tile_entity_preserve":
	a Chest record with items and no live owner) -> "unload aborted".

	Beta attaches a chunk's TileEntities at chunk load and nothing clears them
	afterwards [ChunkLoader.java:188-194 loadChunkIntoWorldFromCompound].

	Synthetic world (no third-party map content): 3x3 Beta chunks (0..2,0..2)
	in r.0.0.mcr, stone below y=64, spawn (24,64,24) in chunk (1,1), which
	holds a Chest with two stacks plus an unknown tag, a Sign and a lit
	Furnace (id 62, BurnTime 0, empty Items list of element type 1, exactly
	as Beta writes an empty furnace).
	  1. after load the live store owns all three tile entities of (1,1);
	  2. unload succeeds (the checkpoint does not refuse the chunk);
	  3. the saved record keeps all three, the chest's items, and the unknown
	     tag verbatim;
	  4. a reload sees the same three tile entities again.
*/
#include "support/split_residency_fixture.h"

#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "item/items.h"
#include "network/tile_entities_nbt.h"
#include "network/tile_entities_store.h"

#include <stdio.h>
#include <zlib.h>

/* ---- tiny big-endian NBT writer ------------------------------------------ */
struct nb { unsigned char* p; size_t n, cap; };
static void nb_put(struct nb* b, const void* d, size_t n) {
	if(b->n + n > b->cap) {
		b->cap = (b->n + n) * 2;
		b->p = realloc(b->p, b->cap);
		assert(b->p);
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}
static void nb_u8(struct nb* b, unsigned v) { unsigned char c = (unsigned char)v; nb_put(b, &c, 1); }
static void nb_u16(struct nb* b, unsigned v) { nb_u8(b, v >> 8); nb_u8(b, v); }
static void nb_u32(struct nb* b, uint32_t v) { nb_u16(b, v >> 16); nb_u16(b, v & 0xFFFF); }
static void nb_u64(struct nb* b, uint64_t v) { nb_u32(b, (uint32_t)(v >> 32)); nb_u32(b, (uint32_t)v); }
static void nb_name(struct nb* b, unsigned type, const char* name) {
	nb_u8(b, type);
	nb_u16(b, (unsigned)strlen(name));
	nb_put(b, name, strlen(name));
}
static void t_byte(struct nb* b, const char* n, int v) { nb_name(b, 1, n); nb_u8(b, (unsigned)v & 255); }
static void t_short(struct nb* b, const char* n, int v) { nb_name(b, 2, n); nb_u16(b, (unsigned)v & 0xFFFF); }
static void t_int(struct nb* b, const char* n, int32_t v) { nb_name(b, 3, n); nb_u32(b, (uint32_t)v); }
static void t_long(struct nb* b, const char* n, int64_t v) { nb_name(b, 4, n); nb_u64(b, (uint64_t)v); }
static void t_bytes(struct nb* b, const char* n, const unsigned char* d, uint32_t len) {
	nb_name(b, 7, n); nb_u32(b, len); nb_put(b, d, len);
}
static void t_str(struct nb* b, const char* n, const char* s) {
	nb_name(b, 8, n); nb_u16(b, (unsigned)strlen(s)); nb_put(b, s, strlen(s));
}
static void t_list(struct nb* b, const char* n, unsigned elem, uint32_t count) {
	nb_name(b, 9, n); nb_u8(b, elem); nb_u32(b, count);
}
static void t_compound(struct nb* b, const char* n) { nb_name(b, 10, n); }
static void t_end(struct nb* b) { nb_u8(b, 0); }
static void item(struct nb* b, int slot, int id, int count) {
	t_byte(b, "Slot", slot); t_short(b, "id", id); t_byte(b, "Count", count);
	t_short(b, "Damage", 0); t_end(b);
}

enum { CHEST_X = 20, CHEST_Y = 64, CHEST_Z = 20, SIGN_X = 21, FURNACE_X = 22 };

static void chunk_nbt(struct nb* b, int cx, int cz) {
	static unsigned char blocks[32768], data[16384], sky[16384], light[16384], hm[256];
	memset(blocks, 0, sizeof blocks);
	memset(data, 0, sizeof data);
	memset(light, 0, sizeof light);
	memset(sky, 0, sizeof sky);
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			hm[z << 4 | x] = 64;
			for(int y = 0; y < 128; y++) {
				int i = x << 11 | z << 7 | y; /* Beta Chunk block index */
				blocks[i] = y == 0 ? 7 : y < 64 ? 1 : 0;
				if(y >= 64)
					sky[i >> 1] |= (unsigned char)(i & 1 ? 0xF0 : 0x0F);
			}
		}
	bool spawn = cx == 1 && cz == 1;
	if(spawn) {
		int lz = CHEST_Z & 15;
		blocks[(CHEST_X & 15) << 11 | lz << 7 | CHEST_Y] = 54;
		blocks[(SIGN_X & 15) << 11 | lz << 7 | CHEST_Y] = 63;
		int fi = (FURNACE_X & 15) << 11 | lz << 7 | CHEST_Y;
		blocks[fi] = 62; /* lit furnace, BurnTime 0 */
		data[fi >> 1] |= (unsigned char)(fi & 1 ? 2 << 4 : 2);
	}
	t_compound(b, "");
	t_compound(b, "Level");
	t_int(b, "xPos", cx);
	t_int(b, "zPos", cz);
	t_long(b, "LastUpdate", 1000);
	t_byte(b, "TerrainPopulated", 1);
	t_bytes(b, "Blocks", blocks, sizeof blocks);
	t_bytes(b, "Data", data, sizeof data);
	t_bytes(b, "SkyLight", sky, sizeof sky);
	t_bytes(b, "BlockLight", light, sizeof light);
	t_bytes(b, "HeightMap", hm, sizeof hm);
	t_list(b, "Entities", 10, 0);
	if(!spawn) {
		t_list(b, "TileEntities", 10, 0);
	} else {
		t_list(b, "TileEntities", 10, 3);
		/* Chest: Beta field set + one tag Beta never writes (preserved verbatim) */
		t_str(b, "id", "Chest");
		t_int(b, "x", CHEST_X); t_int(b, "y", CHEST_Y); t_int(b, "z", CHEST_Z);
		t_list(b, "Items", 10, 2);
		item(b, 0, 50, 64);
		item(b, 13, 4, 12);
		t_int(b, "CwTestUnknown", 7);
		t_end(b);
		t_str(b, "id", "Sign");
		t_int(b, "x", SIGN_X); t_int(b, "y", CHEST_Y); t_int(b, "z", CHEST_Z);
		t_str(b, "Text1", "spawn");
		t_str(b, "Text2", "chunk");
		t_str(b, "Text3", "");
		t_str(b, "Text4", "kept");
		t_end(b);
		t_short(b, "BurnTime", 0);
		t_short(b, "CookTime", 0);
		t_str(b, "id", "Furnace");
		t_int(b, "x", FURNACE_X); t_int(b, "y", CHEST_Y); t_int(b, "z", CHEST_Z);
		t_list(b, "Items", 1, 0); /* Beta's empty NBTTagList: element type 1 */
		t_end(b);
	}
	t_end(b); /* Level */
	t_end(b); /* root */
}

static void write_region(const char* path) {
	static unsigned char header[8192];
	memset(header, 0, sizeof header);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(header, 1, sizeof header, f) == sizeof header);
	uint32_t sector = 2;
	for(int cz = 0; cz < 3; cz++)
		for(int cx = 0; cx < 3; cx++) {
			struct nb raw = {0};
			chunk_nbt(&raw, cx, cz);
			uLongf zlen = compressBound((uLong)raw.n);
			unsigned char* z = malloc(zlen);
			assert(z && compress(z, &zlen, raw.p, (uLong)raw.n) == Z_OK);
			uint32_t len = (uint32_t)zlen + 1;
			uint32_t sectors = (len + 4 + 4095) / 4096;
			unsigned char* rec = calloc(sectors, 4096);
			assert(rec);
			rec[0] = (unsigned char)(len >> 24); rec[1] = (unsigned char)(len >> 16);
			rec[2] = (unsigned char)(len >> 8); rec[3] = (unsigned char)len;
			rec[4] = 2; /* zlib */
			memcpy(rec + 5, z, zlen);
			assert(fwrite(rec, 4096, sectors, f) == sectors);
			uint32_t entry = sector << 8 | sectors;
			int i = cx + cz * 32;
			header[i * 4] = (unsigned char)(entry >> 24);
			header[i * 4 + 1] = (unsigned char)(entry >> 16);
			header[i * 4 + 2] = (unsigned char)(entry >> 8);
			header[i * 4 + 3] = (unsigned char)entry;
			header[4096 + i * 4 + 3] = 1; /* timestamp */
			sector += sectors;
			free(rec); free(z); free(raw.p);
		}
	assert(fseek(f, 0, SEEK_SET) == 0);
	assert(fwrite(header, 1, sizeof header, f) == sizeof header);
	assert(fclose(f) == 0);
}

/* A Beta server/map level.dat: no Player tag, LastPlayed set. */
static void write_level(const char* path) {
	struct nb b = {0};
	t_compound(&b, "");
	t_compound(&b, "Data");
	t_long(&b, "RandomSeed", 4242);
	t_int(&b, "SpawnX", 24); t_int(&b, "SpawnY", 64); t_int(&b, "SpawnZ", 24);
	t_long(&b, "Time", 1000);
	t_long(&b, "LastPlayed", 1750000000000LL);
	t_str(&b, "LevelName", "spawn te");
	t_int(&b, "version", 19132);
	t_long(&b, "SizeOnDisk", 0);
	t_byte(&b, "raining", 0); t_int(&b, "rainTime", 99999999);
	t_byte(&b, "thundering", 0); t_int(&b, "thunderTime", 99999999);
	t_end(&b);
	t_end(&b);
	gzFile g = gzopen(path, "wb");
	assert(g && gzwrite(g, b.p, (unsigned)b.n) == (int)b.n && gzclose(g) == Z_OK);
	free(b.p);
}

/* ---- checks ----------------------------------------------------------------- */
static int s_failures;
#define CHECK(c, ...) do { if(!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while(0)

static void check_live(struct server_local* s, const char* when) {
	struct te_store st = server_world_tile_store(&s->worlds[0]);
	struct chunk_tile_entities te;
	assert(te_store_gather(&st, 1, 1, &te));
	printf("[%s] live chunk (1,1): chests=%d signs=%d furnaces=%d\n", when,
		   te.n_chest, te.n_sign, te.n_furnace);
	CHECK(te.n_chest == 1 && te.n_sign == 1 && te.n_furnace == 1,
		  "%s: spawn chunk lost tile entities (chests=%d signs=%d furnaces=%d)",
		  when, te.n_chest, te.n_sign, te.n_furnace);
	if(te.n_chest == 1) {
		CHECK(te.chests[0].items[0].id == 50 && te.chests[0].items[0].count == 64
				  && te.chests[0].items[13].id == 4 && te.chests[0].items[13].count == 12,
			  "%s: chest contents changed", when);
	}
	tile_entities_parse_free(&te);
}

static nbt_node* read_chunk(const char* region, int cx, int cz) {
	FILE* f = fopen(region, "rb");
	assert(f);
	unsigned char h[4];
	assert(fseek(f, (cx + cz * 32) * 4, SEEK_SET) == 0 && fread(h, 1, 4, f) == 4);
	uint32_t entry = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
	assert(entry >> 8);
	assert(fseek(f, (long)(entry >> 8) * 4096, SEEK_SET) == 0 && fread(h, 1, 4, f) == 4);
	uint32_t len = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
	unsigned char* rec = malloc(len);
	assert(rec && fread(rec, 1, len, f) == len && rec[0] == 2);
	fclose(f);
	nbt_node* n = nbt_parse_compressed(rec + 1, len - 1);
	free(rec);
	return n;
}

static nbt_node* child(nbt_node* c, const char* name) {
	if(!c || c->type != TAG_COMPOUND) return NULL;
	struct list_head* p;
	list_for_each(p, &c->payload.tag_compound->entry) {
		nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(n->name && !strcmp(n->name, name)) return n;
	}
	return NULL;
}

static void check_saved(const char* region) {
	nbt_node* root = read_chunk(region, 1, 1);
	assert(root);
	nbt_node* tes = nbt_find_by_path(root, ".Level.TileEntities");
	assert(tes && tes->type == TAG_LIST);
	int chests = 0, signs = 0, furnaces = 0, unknown = 0, chest_items = 0;
	struct list_head* p;
	list_for_each(p, &tes->payload.tag_list->entry) {
		nbt_node* te = list_entry(p, struct nbt_list, entry)->data;
		nbt_node* id = child(te, "id");
		assert(id && id->type == TAG_STRING);
		if(!strcmp(id->payload.tag_string, "Chest")) {
			chests++;
			nbt_node* u = child(te, "CwTestUnknown");
			unknown += u && u->type == TAG_INT && u->payload.tag_int == 7;
			nbt_node* items = child(te, "Items");
			if(items && items->type == TAG_LIST) {
				struct list_head* q;
				list_for_each(q, &items->payload.tag_list->entry) chest_items++;
			}
		} else if(!strcmp(id->payload.tag_string, "Sign")) {
			signs++;
		} else if(!strcmp(id->payload.tag_string, "Furnace")) {
			furnaces++;
		}
	}
	printf("[saved] chunk (1,1) TileEntities: chests=%d (items=%d unknown_tag=%d) signs=%d furnaces=%d\n",
		   chests, chest_items, unknown, signs, furnaces);
	CHECK(chests == 1 && signs == 1 && furnaces == 1, "saved record lost tile entities");
	CHECK(chest_items == 2, "saved chest has %d item records, want 2", chest_items);
	CHECK(unknown == 1, "unknown chest tag not preserved verbatim");
	nbt_free(root);
}

static struct server_local s_load;

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	char dir[256], region_dir[320], region[384], level[320];
	test_fixture_dir("test_dropin_spawn_chunk_te", dir, sizeof dir);
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0777);
#endif
	snprintf(region_dir, sizeof region_dir, "%s/region", dir);
#ifdef _WIN32
	_mkdir(region_dir);
#else
	mkdir(region_dir, 0777);
#endif
	snprintf(region, sizeof region, "%s/r.0.0.mcr", region_dir);
	snprintf(level, sizeof level, "%s/level.dat", dir);
	write_region(region);
	write_level(level);

	server_local_test_create_without_thread(&s_load, false, false, false,
											false, false, false, false);
	string_t lvl;
	string_init_set_str(lvl, dir);
	assert(server_local_load_world_impl(&s_load, lvl));
	check_live(&s_load, "after load");
	bool unloaded = server_local_unload_world_impl(&s_load, false);
	clin_drain_discard();
	printf("[unload] %s\n", unloaded ? "ok" : "REFUSED (checkpoint failed)");
	CHECK(unloaded, "unload refused: the world checkpoint failed");
	if(unloaded) {
		check_saved(region);
		assert(server_local_load_world_impl(&s_load, lvl));
		/* The saved Player tag now skips the spawn probe: load the chunk. */
		struct server_chunk* c = NULL;
		if(!dict_server_chunks_get(s_load.worlds[0].chunks, S_CHUNK_ID(1, 1)))
			assert(server_world_load_chunk(&s_load.worlds[0], 1, 1, &c) && c);
		check_live(&s_load, "after reload");
		CHECK(server_local_unload_world_impl(&s_load, false), "second unload refused");
		clin_drain_discard();
	}
	string_clear(lvl);
	test_fixture_dir_cleanup(dir);
	if(s_failures) {
		printf("test_dropin_spawn_chunk_te: FAIL (%d)\n", s_failures);
		return 1;
	}
	puts("test_dropin_spawn_chunk_te: PASS");
	return 0;
}
