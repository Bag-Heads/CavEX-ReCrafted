/* save-enc-1030: CaveWii's chunk saves use Beta 1.7.3's own NBT encoding.
 *
 * Found by the hw1030a NBT diff of the real Beta world dj_orig after a
 * CaveWii save. Each check is against the Beta-encoded reference, cited from
 * the decompile (../McBetaCpp/srcorigjava):
 *   1 ChunkLoader.java:140  setLong("LastUpdate", world.getWorldTime()) on
 *     every chunk Beta writes; a chunk Beta does not save keeps its stamp.
 *   2 NBTTagList.java:25-31  an EMPTY list is written with element type 1
 *     (TAG_BYTE): "Entities", "TileEntities" and a cart's "Items".
 *   3 EntityWolf.java:56-68 Angry/Sitting/Owner, EntityPig.java:28-32 Saddle,
 *     EntityPigZombie.java:43-47 Anger, EntityCreeper.java:29-36 powered
 *     (only when charged) -- each only on its own class.
 *   4 Entity.writeToNBT newDoubleNBTList: Pos/Motion are TAG_DOUBLE lists.
 *     archived_entity (and the server snapshot that fills it) holds float
 *     positions, so the double is the float widened -- no invented precision.
 * Also: records written by every build before save-enc-1030 (the old fixed
 * monster footprint) are still byte-proven deletable, and the preserving
 * merge sheds their stale class tags. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "entity/monsters_object.h"
#include "network/entity_archive.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "platform/mem2.h"

static char s_world[128];
#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE (CHUNK_SIZE * CHUNK_SIZE)

static nbt_node* child(const nbt_node* comp, const char* name) {
	assert(comp && comp->type == TAG_COMPOUND);
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* n = list_entry(pos, struct nbt_list, entry)->data;
		if(n && n->name && !strcmp(n->name, name))
			return n;
	}
	return NULL;
}

static size_t list_len(const nbt_node* list) {
	assert(list && list->type == TAG_LIST && list->payload.tag_list);
	size_t n = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry)
		n++;
	return n;
}

static nbt_node* list_at(const nbt_node* list, size_t index) {
	size_t i = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry)
		if(i++ == index)
			return list_entry(pos, struct nbt_list, entry)->data;
	return NULL;
}

/* ---- raw region record reader (independent of region_archive.c) ------- */

static unsigned char* read_record(int cx, int cz, size_t* out_len) {
	FILE* f = fopen(test_fixture_join(s_world, "/region/r.0.0.mcr"), "rb");
	assert(f);
	unsigned char head[4];
	assert(!fseek(f, 4L * (cx + cz * 32), SEEK_SET));
	assert(fread(head, 1, 4, f) == 4);
	long offset = ((long)head[0] << 16) | ((long)head[1] << 8) | head[2];
	assert(offset >= 2 && head[3] > 0);
	assert(!fseek(f, offset * 4096L, SEEK_SET));
	unsigned char frame[5];
	assert(fread(frame, 1, 5, f) == 5);
	uint32_t length = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16)
		| ((uint32_t)frame[2] << 8) | frame[3];
	assert(frame[4] == 2 && length > 1);
	unsigned char* packed = malloc(length - 1);
	assert(packed && fread(packed, 1, length - 1, f) == length - 1);
	fclose(f);
	uLongf cap = 1u << 20;
	unsigned char* raw = malloc(cap);
	assert(raw);
	assert(uncompress(raw, &cap, packed, length - 1) == Z_OK);
	free(packed);
	*out_len = cap;
	return raw;
}

/* The exact Beta bytes of an empty named list: TAG_List, name, element type
 * 1, count 0 [NBTTagList.writeTagContents]. */
static bool has_beta_empty_list(const unsigned char* raw, size_t len,
								const char* name) {
	unsigned char want[64];
	size_t n = strlen(name), k = 0;
	want[k++] = TAG_LIST;
	want[k++] = (unsigned char)(n >> 8);
	want[k++] = (unsigned char)n;
	memcpy(want + k, name, n);
	k += n;
	want[k++] = TAG_BYTE;
	want[k++] = 0; want[k++] = 0; want[k++] = 0; want[k++] = 0;
	for(size_t i = 0; i + k <= len; i++)
		if(!memcmp(raw + i, want, k))
			return true;
	return false;
}

static int64_t stored_last_update(int cx, int cz) {
	size_t len;
	unsigned char* raw = read_record(cx, cz, &len);
	nbt_node* root = nbt_parse(raw, len);
	assert(root);
	nbt_node* lu = nbt_find_by_path(root, ".Level.LastUpdate");
	assert(lu && lu->type == TAG_LONG);
	int64_t v = lu->payload.tag_long;
	nbt_free(root);
	free(raw);
	return v;
}

/* ---- fixtures ------------------------------------------------------------ */

static void init_chunk(struct server_chunk* sc) {
	memset(sc, 0, sizeof(*sc));
	sc->ids = calloc(1, IDS_SIZE);
	sc->metadata = calloc(1, NIB_SIZE);
	sc->lighting_sky = calloc(1, NIB_SIZE);
	sc->lighting_torch = calloc(1, NIB_SIZE);
	sc->heightmap = calloc(1, HM_SIZE);
	assert(sc->ids && sc->metadata && sc->lighting_sky && sc->lighting_torch
		   && sc->heightmap);
	sc->modified = true;
	sc->populated = true;
}

static void free_chunk(struct server_chunk* sc) {
	free(sc->ids);
	free(sc->metadata);
	free(sc->lighting_sky);
	free(sc->lighting_torch);
	free(sc->heightmap);
}

static struct archived_entity mob(uint32_t id, int32_t monster, float x) {
	struct archived_entity e;
	memset(&e, 0, sizeof(e));
	e.kind = ARCHIVED_ENTITY_MONSTER;
	e.source_id = id;
	e.pos[0] = x;
	e.pos[1] = 70.1f;
	e.pos[2] = 6.3f;
	e.vel[0] = -0.15f;
	e.vel[1] = 0.05f;
	e.vel[2] = 0.2f;
	e.health = 10;
	e.air = 300;
	e.data.monster.monster_id = monster;
	e.data.monster.air = 300;
	e.data.monster.fuse = -1;
	return e;
}

/* ---- 3 + 4: per-class tags, double Pos/Motion, empty Items type ---------- */

static void test_entity_record_encoding(void) {
	struct archived_entity r[7];
	r[0] = mob(1, MONSTER_ZOMBIE, 1.5f);
	/* A live mob carries exact doubles (mob_motion): they are saved as-is. */
	r[0].pos_d[0] = 1.123456789012345; r[0].pos_d[1] = 70.1000000001;
	r[0].pos_d[2] = 6.3000000002; r[0].vel_d[0] = -0.15000000003;
	r[0].vel_d[1] = 0.0500000004; r[0].vel_d[2] = 0.2000000005;
	for(int k = 0; k < 3; k++) {
		r[0].pos[k] = (float)r[0].pos_d[k];
		r[0].vel[k] = (float)r[0].vel_d[k];
	}
	r[0].has_pos_d = r[0].has_vel_d = true;
	r[1] = mob(2, MONSTER_WOLF, 2.5f);
	r[1].data.monster.wolf_angry = true;
	r[1].data.monster.wolf_sitting = true;
	strcpy(r[1].data.monster.wolf_owner, "Steve");
	r[2] = mob(3, MONSTER_PIG, 3.5f);
	r[2].data.monster.saddled = true;
	r[3] = mob(4, MONSTER_PIG_ZOMBIE, 4.5f);
	r[3].data.monster.anger = 321;
	r[4] = mob(5, MONSTER_CREEPER, 5.5f);
	r[4].data.monster.charged = true;
	r[5] = mob(6, MONSTER_CREEPER, 6.5f);
	memset(&r[6], 0, sizeof(r[6]));
	r[6].kind = ARCHIVED_ENTITY_MINECART;
	r[6].source_id = 7;
	r[6].pos[0] = 7.5f; r[6].pos[1] = 64.0f; r[6].pos[2] = 7.5f;
	r[6].air = 300;
	struct chunk_entities in = {.entities = r, .count = 7};
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&in, &arena);
	assert(list);
	nbt_node holder = {.type = TAG_LIST, .name = "Entities",
		.payload.tag_list = list};
	assert(list_len(&holder) == 7);
	static const char* const cls[] = {
		"Angry", "Sitting", "Owner", "Saddle", "Anger", "powered",
	};
	/* expected presence per record: zombie none; wolf A/S/O; pig Saddle;
	 * pig zombie Anger; charged creeper powered; plain creeper none. */
	static const bool want[6][6] = {
		{0, 0, 0, 0, 0, 0}, {1, 1, 1, 0, 0, 0}, {0, 0, 0, 1, 0, 0},
		{0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0},
	};
	for(size_t i = 0; i < 6; i++)
		for(size_t k = 0; k < 6; k++) {
			bool present = child(list_at(&holder, i), cls[k]) != NULL;
			if(present != want[i][k]) {
				fprintf(stderr, "FAIL record %zu tag %s present=%d want=%d\n",
					i, cls[k], present, want[i][k]);
				assert(0);
			}
		}
	nbt_node* wolf = list_at(&holder, 1);
	assert(child(wolf, "Angry")->type == TAG_BYTE
		   && child(wolf, "Angry")->payload.tag_byte == 1);
	assert(child(wolf, "Sitting")->payload.tag_byte == 1);
	assert(child(wolf, "Owner")->type == TAG_STRING
		   && !strcmp(child(wolf, "Owner")->payload.tag_string, "Steve"));
	nbt_node* pig = list_at(&holder, 2);
	assert(child(pig, "Saddle")->type == TAG_BYTE
		   && child(pig, "Saddle")->payload.tag_byte == 1);
	nbt_node* pz = list_at(&holder, 3);
	assert(child(pz, "Anger")->type == TAG_SHORT
		   && child(pz, "Anger")->payload.tag_short == 321);
	nbt_node* creeper = list_at(&holder, 4);
	assert(child(creeper, "powered")->type == TAG_BYTE
		   && child(creeper, "powered")->payload.tag_byte == 1);
	puts("PASS 3 Beta class tags only on their own class (wolf A/S/O, pig "
		 "Saddle, pig zombie Anger, creeper powered only when charged)");

	/* 4: Pos/Motion are three TAG_DOUBLEs, each exactly the float widened. */
	for(size_t i = 0; i < 7; i++) {
		nbt_node* rec = list_at(&holder, i);
		const char* names[2] = {"Pos", "Motion"};
		const float* src[2] = {r[i].pos, r[i].vel};
		for(int v = 0; v < 2; v++) {
			nbt_node* l = child(rec, names[v]);
			assert(l && l->type == TAG_LIST && list_len(l) == 3);
			assert(l->payload.tag_list->data->type == TAG_DOUBLE);
			for(size_t k = 0; k < 3; k++) {
				nbt_node* d = list_at(l, k);
				assert(d->type == TAG_DOUBLE
					   && d->payload.tag_double == ((v ? r[i].has_vel_d : r[i].has_pos_d) ? (v ? r[i].vel_d : r[i].pos_d)[k] : (double)src[v][k]));
			}
		}
	}
	puts("PASS 4 Pos/Motion TAG_DOUBLE: a mob's exact doubles, else the float widened "
		 "(archived_entity holds floats; no precision invented)");

	/* 2 (cart): an empty Items list dumps element type 1. */
	nbt_node* cart = list_at(&holder, 6);
	nbt_node* items = child(cart, "Items");
	assert(items && list_len(items) == 0
		   && items->payload.tag_list->data->type == TAG_BYTE);
	struct buffer bytes = nbt_dump_binary(cart);
	assert(bytes.data && has_beta_empty_list(bytes.data, bytes.len, "Items"));
	buffer_free(&bytes);
	puts("PASS 2 empty minecart Items list written as Beta (09 'Items' 01 "
		 "00000000)");

	/* Round trip: every per-class value comes back; absent tags load as the
	 * defaults the other classes always carried. */
	struct chunk_entities out = {0};
	assert(chunk_entities_parse(&holder, 0, 0, &out) && out.count == 7);
	for(size_t i = 0; i < out.count; i++) {
		const struct archived_entity* e = &out.entities[i];
		size_t j = 0;
		while(j < 7 && r[j].source_id != e->source_id) j++;
		assert(j < 7);
		if(e->kind != ARCHIVED_ENTITY_MONSTER) continue;
		if(e->source_id == 1)
			assert(e->has_pos_d && e->has_vel_d
				   && e->pos_d[0] == 1.123456789012345
				   && e->vel_d[2] == 0.2000000005);
		const struct archived_monster_state* a = &e->data.monster;
		const struct archived_monster_state* b = &r[j].data.monster;
		assert(a->wolf_angry == b->wolf_angry && a->wolf_sitting == b->wolf_sitting
			   && !strcmp(a->wolf_owner, b->wolf_owner)
			   && a->saddled == b->saddled && a->anger == b->anger
			   && a->charged == b->charged);
	}
	chunk_entities_free(&out);
	chunk_entities_build_free(arena);
}

/* ---- 1 + 2: LastUpdate and empty chunk lists in the stored record -------- */

static void test_chunk_record_encoding(struct region_archive* ra) {
	struct server_chunk sc;
	init_chunk(&sc);
	struct chunk_tile_entities te = {.legacy_flat_complete = true};
	struct chunk_entities none = {.world_time = 123456, .has_world_time = true};
	assert(region_archive_set_chunk(ra, 0, 0, &sc, &te, &none));
	assert(region_archive_sync(ra));
	size_t len;
	unsigned char* raw = read_record(0, 0, &len);
	assert(has_beta_empty_list(raw, len, "Entities"));
	assert(has_beta_empty_list(raw, len, "TileEntities"));
	free(raw);
	assert(stored_last_update(0, 0) == 123456);
	puts("PASS 1 new chunk LastUpdate = world time (123456)");
	puts("PASS 2 empty Entities/TileEntities written as Beta (element type 1)");

	/* A changed re-save (canonical record, block edit) stamps the new time. */
	sc.ids[0] = 1;
	none.world_time = 200000;
	assert(region_archive_set_chunk(ra, 0, 0, &sc, &te, &none));
	assert(region_archive_sync(ra));
	assert(stored_last_update(0, 0) == 200000);

	/* Chunk (1,0) with a pig: the preserving-merge path (not canonical). */
	struct server_chunk sc2;
	init_chunk(&sc2);
	struct archived_entity pig = mob(77, MONSTER_PIG, 20.5f);
	struct chunk_entities one = {.entities = &pig, .count = 1,
		.world_time = 300000, .has_world_time = true};
	assert(region_archive_set_chunk(ra, 1, 0, &sc2, &te, &one));
	assert(region_archive_sync(ra));
	assert(stored_last_update(1, 0) == 300000);
	/* Unchanged content at a later time: a no-op save, the stamp stays (Beta
	 * does not save an unchanged chunk, so its LastUpdate is the last real
	 * write's). */
	one.world_time = 300500;
	assert(region_archive_set_chunk(ra, 1, 0, &sc2, &te, &one));
	assert(region_archive_sync(ra));
	assert(stored_last_update(1, 0) == 300000);
	/* The pig moved: a real write, stamped with the new time. */
	pig.pos[0] = 21.25f;
	one.world_time = 301000;
	assert(region_archive_set_chunk(ra, 1, 0, &sc2, &te, &one));
	assert(region_archive_sync(ra));
	assert(stored_last_update(1, 0) == 301000);
	/* A snapshot that does not know the time never resets a real stamp. */
	pig.pos[0] = 22.0f;
	struct chunk_entities untimed = {.entities = &pig, .count = 1};
	assert(region_archive_set_chunk(ra, 1, 0, &sc2, &te, &untimed));
	assert(region_archive_sync(ra));
	assert(stored_last_update(1, 0) == 301000);
	puts("PASS 1 re-save: changed -> new world time; unchanged -> stamp kept; "
		 "unknown time -> stamp kept");
	free_chunk(&sc);
	free_chunk(&sc2);
}

/* ---- legacy (pre-save-enc-1030) native records ----------------------------- */

static nbt_node* new_node(nbt_type type, const char* name) {
	nbt_node* n = calloc(1, sizeof(*n));
	assert(n);
	n->type = type;
	n->name = malloc(strlen(name) + 1);
	assert(n->name);
	strcpy(n->name, name);
	return n;
}

static void insert_after(nbt_node* comp, const char* after, nbt_node* n) {
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* c = list_entry(pos, struct nbt_list, entry)->data;
		if(c && c->name && !strcmp(c->name, after)) {
			struct nbt_list* w = malloc(sizeof(*w));
			assert(w);
			w->data = n;
			list_add_head(&w->entry, pos);
			return;
		}
	}
	assert(!"insert_after: anchor missing");
}

/* The exact record the pre-save-enc-1030 builder wrote for a zombie: the
 * current one plus the fixed footprint at its old positions. */
static void make_legacy_zombie(nbt_node* rec) {
	nbt_node* n = new_node(TAG_BYTE, "Angry");
	insert_after(rec, "CaveWiiMonsterId", n);
	insert_after(rec, "Angry", new_node(TAG_BYTE, "Sitting"));
	n = new_node(TAG_STRING, "Owner");
	n->payload.tag_string = calloc(1, 1);
	insert_after(rec, "Sitting", n);
	insert_after(rec, "CaveWiiSheared", new_node(TAG_BYTE, "Saddle"));
	insert_after(rec, "CaveWiiEggTimer", new_node(TAG_SHORT, "Anger"));
	insert_after(rec, "Anger", new_node(TAG_BYTE, "powered"));
}

/* Rewrite chunk (cx,0)'s stored first entity through `edit`, typed the old
 * way (empty lists TAG_COMPOUND as well), and commit it raw. */
static void store_legacy(struct region_archive* ra, int cx, bool extra_tag) {
	size_t len;
	unsigned char* raw = read_record(cx, 0, &len);
	nbt_node* root = nbt_parse(raw, len);
	free(raw);
	assert(root);
	nbt_node* ents = nbt_find_by_path(root, ".Level.Entities");
	assert(ents && list_len(ents) == 1);
	nbt_node* rec = list_at(ents, 0);
	make_legacy_zombie(rec);
	if(extra_tag) {
		nbt_node* n = new_node(TAG_INT, "CaveWiiFutureField");
		n->payload.tag_int = 5;
		insert_after(rec, "powered", n);
	}
	nbt_find_by_path(root, ".Level.TileEntities")
		->payload.tag_list->data->type = TAG_COMPOUND;
	struct buffer packed = nbt_dump_compressed(root, STRAT_INFLATE);
	assert(packed.data);
	nbt_free(root);
	assert(region_archive_recommit_record(ra, cx, 0, &packed));
	buffer_free(&packed);
	assert(region_archive_sync(ra));
}

static void test_legacy_native_records(struct region_archive* ra) {
	struct chunk_tile_entities te = {.legacy_flat_complete = true};
	/* (a) a departed legacy zombie is still byte-proven deletable. */
	struct server_chunk sc;
	init_chunk(&sc);
	struct archived_entity z = mob(91, MONSTER_ZOMBIE, 36.5f);
	struct chunk_entities one = {.entities = &z, .count = 1};
	assert(region_archive_set_chunk(ra, 2, 0, &sc, &te, &one));
	assert(region_archive_sync(ra));
	store_legacy(ra, 2, false);
	struct chunk_entities none = {.world_time = 5, .has_world_time = true};
	if(!region_archive_set_chunk(ra, 2, 0, &sc, &te, &none)) {
		fprintf(stderr, "FAIL legacy zombie not deletable, preserve fail=%d\n",
			(int)chunk_entities_preserve_last_fail());
		assert(0);
	}
	assert(region_archive_sync(ra));
	size_t len;
	unsigned char* raw = read_record(2, 0, &len);
	assert(has_beta_empty_list(raw, len, "Entities"));
	/* TileEntities was stored empty (typed 10 by the old build) and stays
	 * empty: an unchanged list keeps its stored bytes verbatim. */
	assert(!has_beta_empty_list(raw, len, "TileEntities"));
	free(raw);
	puts("PASS legacy footprint zombie (pre-save-enc-1030) departs cleanly; "
		 "lists rewritten as Beta");

	/* (b) the same zombie still present, with an unknown tag that forces the
	 * preserving merge: the stale class tags go, the unknown tag stays. */
	struct server_chunk sc3;
	init_chunk(&sc3);
	struct archived_entity z2 = mob(92, MONSTER_ZOMBIE, 52.5f);
	struct chunk_entities keep = {.entities = &z2, .count = 1};
	assert(region_archive_set_chunk(ra, 3, 0, &sc3, &te, &keep));
	assert(region_archive_sync(ra));
	store_legacy(ra, 3, true);
	z2.pos[0] = 53.0f;
	assert(region_archive_set_chunk(ra, 3, 0, &sc3, &te, &keep));
	assert(region_archive_sync(ra));
	raw = read_record(3, 0, &len);
	nbt_node* root = nbt_parse(raw, len);
	free(raw);
	assert(root);
	nbt_node* ents = nbt_find_by_path(root, ".Level.Entities");
	assert(ents && list_len(ents) == 1);
	nbt_node* rec = list_at(ents, 0);
	static const char* const cls[] = {
		"Angry", "Sitting", "Owner", "Saddle", "Anger", "powered",
	};
	for(size_t k = 0; k < 6; k++)
		if(child(rec, cls[k])) {
			fprintf(stderr, "FAIL stale %s kept on a zombie\n", cls[k]);
			assert(0);
		}
	assert(child(rec, "CaveWiiFutureField"));
	nbt_node* pos = child(rec, "Pos");
	assert(list_at(pos, 0)->payload.tag_double == (double)53.0f);
	nbt_free(root);
	puts("PASS legacy footprint zombie kept by the preserving merge sheds "
		 "its stale wolf/pig/pig-zombie/creeper tags");
	free_chunk(&sc);

	/* (c) a mob saved with exact doubles departs: the deletion proof
	 * re-encodes the parsed doubles byte for byte. */
	struct server_chunk sc4;
	init_chunk(&sc4);
	struct archived_entity zd = mob(93, MONSTER_ZOMBIE, 68.0f);
	zd.pos_d[0] = 68.123456789012345; zd.pos_d[1] = 70.25; zd.pos_d[2] = 6.3;
	zd.vel_d[0] = 0.0123456789; zd.vel_d[1] = -0.0784000015258789; zd.vel_d[2] = 0;
	for(int k = 0; k < 3; k++) {
		zd.pos[k] = (float)zd.pos_d[k];
		zd.vel[k] = (float)zd.vel_d[k];
	}
	zd.has_pos_d = zd.has_vel_d = true;
	struct chunk_entities dmob = {.entities = &zd, .count = 1};
	assert(region_archive_set_chunk(ra, 4, 0, &sc4, &te, &dmob));
	assert(region_archive_sync(ra));
	if(!region_archive_set_chunk(ra, 4, 0, &sc4, &te, &none)) {
		fprintf(stderr, "FAIL double mob not deletable, preserve fail=%d\n",
			(int)chunk_entities_preserve_last_fail());
		assert(0);
	}
	free_chunk(&sc4);
	puts("PASS mob saved with exact doubles departs cleanly");
	free_chunk(&sc3);
}

int main(void) {
	test_fixture_dir("test_save_beta_encoding_tmp", s_world, sizeof s_world);
	MKDIR(s_world);
	MKDIR(test_fixture_join(s_world, "/region"));
	remove(test_fixture_join(s_world, "/region/r.0.0.mcr"));

	test_entity_record_encoding();

	string_t level;
	string_init_set_str(level, s_world);
	struct region_archive ra;
	assert(region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
	test_chunk_record_encoding(&ra);
	test_legacy_native_records(&ra);
	region_archive_destroy(&ra);
	string_clear(level);
	remove(test_fixture_join(s_world, "/region/r.0.0.mcr"));
	test_fixture_dir_cleanup(s_world);
	puts("test_save_beta_encoding: all PASS");
	return 0;
}
