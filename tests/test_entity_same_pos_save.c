/* lava-aabb-0924/H3: two entities at ONE exact position must not fail the
 * chunk save closed.
 *
 * The codec has always written CaveWiiRuntimeId as TAG_LONG (make_u32), but
 * the preserving merge's owner comparison (entity_same_saved_owner) honoured a
 * TAG_INT runtime id only. So against every real record the runtime-id branch
 * was dead: owners were matched by Pos bytes, and two records sharing one
 * position -- routine in Beta (item entities do not collide, so a water-stream
 * item sink parks them at identical coordinates; every death drop spawns at
 * the player's exact position on the same tick) -- gave each old record two
 * owners: CEP_DUP_OWNER, the save failed closed, the chunk stayed dirty and
 * NEVER saved again until the entities moved (data loss for every block edit
 * in it on quit). Reachable whenever the codec-complete fast path is defeated
 * (any record the codec cannot byte-prove: a foreign / legacy-shaped row in
 * the same chunk -- here, one such record is injected the way a Beta-era or
 * imported row would sit next to native ones).
 *
 * RED before H3 (measured: region_archive_set_chunk returned false, stage
 * RSF_ENTITY_PRESERVE / sub CEP_DUP_OWNER, and the read-back still showed the
 * save-1 state), GREEN with the shared runtime-id reader: the two items and
 * the mob at one position are matched by runtime id, the save commits, and the
 * reload shows every updated field. Region level = the production
 * region_archive_set_chunk / chunk_entities_preserve path (the server_world
 * layer only chooses erase / retry policy around it). */

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

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "entity/monsters_object.h"
#include "network/entity_archive.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define REGION_PATH test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")
#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE (CHUNK_SIZE * CHUNK_SIZE)

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

/* ---- raw .mcr record access (behind the archive handle) ---- */
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
	assert(fread(raw, 4, 1, f) == 1);
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
	assert(data[0] == 2);
	nbt_node* tree = nbt_parse_compressed(data + 1, length - 1);
	free(data);
	assert(tree);
	return tree;
}
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
	put_be32(raw, 0); /* clear bit 31: the CANONICAL flag would skip the merge */
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
/* The fast-path defeater: one record the codec cannot byte-prove (unknown
 * id, no CaveWii* tags) -- the shape of a Beta-era / imported row. */
static void inject_foreign_record(void) {
	nbt_node* tree = read_record_nbt();
	nbt_node* list = nbt_find_by_path(tree, ".Level.Entities");
	assert(list && list->type == TAG_LIST);
	nbt_node* first
		= list_entry(list->payload.tag_list->entry.flink, struct nbt_list, entry)
			  ->data;
	nbt_node* foreign = nbt_clone(first);
	assert(foreign);
	struct list_head *p, *next;
	list_for_each_safe(p, next, &foreign->payload.tag_compound->entry) {
		struct nbt_list* w = list_entry(p, struct nbt_list, entry);
		if(w->data && w->data->name && !strncmp(w->data->name, "CaveWii", 7)) {
			list_del(p);
			nbt_free(w->data);
			free(w);
		}
	}
	nbt_node* id = compound_child(foreign, "id");
	assert(id && id->type == TAG_STRING);
	free(id->payload.tag_string);
	id->payload.tag_string = strdup("SamePosForeign");
	assert(id->payload.tag_string);
	struct nbt_list* w = malloc(sizeof(*w));
	assert(w);
	w->data = foreign;
	list_add_tail(&w->entry, &list->payload.tag_list->entry);
	/* And prove the on-disk tag type the whole case rests on. */
	nbt_node* rid = compound_child(first, "CaveWiiRuntimeId");
	assert(rid && rid->type == TAG_LONG
		   && "the codec writes CaveWiiRuntimeId as TAG_LONG");
	write_record_nbt(tree);
	nbt_free(tree);
}
static size_t raw_records(void) {
	nbt_node* tree = read_record_nbt();
	nbt_node* list = nbt_find_by_path(tree, ".Level.Entities");
	size_t n = 0;
	struct list_head* pos;
	if(list)
		list_for_each(pos, &list->payload.tag_list->entry)
			n++;
	nbt_free(tree);
	return n;
}
static bool open_region(struct region_archive* ra, string_t world) {
	FILE* probe = fopen(REGION_PATH, "rb");
	if(!probe)
		return region_archive_create_new(ra, world, 0, 0, WORLD_DIM_OVERWORLD);
	fclose(probe);
	return region_archive_create(ra, world, 0, 0, WORLD_DIM_OVERWORLD);
}
static const struct archived_entity* by_source_id(const struct chunk_entities* e,
												 uint32_t id) {
	for(size_t i = 0; i < e->count; i++)
		if(e->entities[i].source_id == id)
			return &e->entities[i];
	return NULL;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_entity_same_pos_save_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(REGION_PATH);
	string_t world;
	string_init_set_str(world, TMP_WORLD);

	struct server_chunk sc;
	alloc_chunk(&sc);
	sc.ids[0] = 1;
	struct chunk_tile_entities te = {.legacy_flat_complete = true};

	/* Two dropped items and a pig at ONE exact position (an item sink). */
	struct archived_entity e[3] = {0};
	for(int i = 0; i < 3; i++) {
		e[i].source_id = (uint32_t)i + 1;
		e[i].pos[0] = 3.5f;
		e[i].pos[1] = 64.0f;
		e[i].pos[2] = 3.5f;
		e[i].fire = -1;
		e[i].air = 300;
	}
	e[0].kind = e[1].kind = ARCHIVED_ENTITY_ITEM;
	e[0].health = e[1].health = 5;
	e[0].data.item.item.id = 4;   /* cobblestone x1 */
	e[0].data.item.item.count = 1;
	e[0].data.item.age = 10;
	e[1].data.item.item.id = 264; /* diamond x1 */
	e[1].data.item.item.count = 1;
	e[1].data.item.age = 20;
	e[2].kind = ARCHIVED_ENTITY_MONSTER;
	e[2].health = 10;
	e[2].data.monster = (struct archived_monster_state) {
		.monster_id = MONSTER_PIG, .fire_time = -1, .air = 300, .fuse = -1,
	};
	struct chunk_entities snap = {.entities = e, .count = 3};

	uint32_t fail0 = region_save_failure_count();
	struct region_archive ra;
	assert(open_region(&ra, world));
	assert(region_archive_set_chunk(&ra, 0, 0, &sc, &te, &snap)); /* save 1 */
	assert(region_archive_destroy(&ra));
	assert(raw_records() == 3);
	inject_foreign_record();
	assert(raw_records() == 4);

	/* The world moves on: ages tick, the diamond stack grows, the pig is hurt,
	 * a block changes. Then the autosave. */
	e[0].data.item.age = 400;
	e[1].data.item.age = 410;
	e[1].data.item.item.count = 3;
	e[2].health = 4;
	sc.ids[0] = 2;
	assert(open_region(&ra, world));
	uint32_t epoch = region_archive_save_epoch(&ra);
	bool ok = region_archive_set_chunk(&ra, 0, 0, &sc, &te, &snap); /* save 2 */
	printf("same-position save: ok=%d fail_n=%u stage=%u sub=%u\n", ok,
		   region_save_failure_count() - fail0,
		   (unsigned)region_save_failure_stage(), region_save_failure_sub());
	/* RED before H3: ok == 0, stage 7 (RSF_ENTITY_PRESERVE), sub 6
	 * (CEP_DUP_OWNER); the archive kept the save-1 record. */
	assert(ok && "two entities at one position must not fail the save closed");
	assert(region_save_failure_count() == fail0);
	assert(region_archive_save_epoch(&ra) > epoch);
	assert(region_archive_destroy(&ra));
	assert(raw_records() == 4); /* three natives + the preserved foreign row */

	/* "Quit and reload": every updated field survived, matched by runtime id
	 * -- not by position, which all three share. */
	assert(open_region(&ra, world));
	struct server_chunk loaded = {0};
	struct chunk_entities back = {0};
	assert(region_archive_get_chunk(&ra, 0, 0, &loaded, NULL, &back));
	assert(loaded.ids[0] == 2);
	assert(back.count == 3);
	const struct archived_entity* cobble = by_source_id(&back, 1);
	const struct archived_entity* diamond = by_source_id(&back, 2);
	const struct archived_entity* pig = by_source_id(&back, 3);
	assert(cobble && cobble->kind == ARCHIVED_ENTITY_ITEM
		   && cobble->data.item.item.id == 4 && cobble->data.item.age == 400);
	assert(diamond && diamond->kind == ARCHIVED_ENTITY_ITEM
		   && diamond->data.item.item.id == 264
		   && diamond->data.item.item.count == 3
		   && diamond->data.item.age == 410);
	assert(pig && pig->kind == ARCHIVED_ENTITY_MONSTER
		   && pig->data.monster.monster_id == MONSTER_PIG && pig->health == 4);
	chunk_entities_free(&back);
	server_chunk_arrays_free(&loaded);

	/* Departure at the shared position: the cobblestone is picked up. The
	 * diamond and pig keep their identity; the cobblestone row is deleted. */
	struct archived_entity rest[2];
	rest[0] = e[1];
	rest[1] = e[2];
	struct chunk_entities remaining = {.entities = rest, .count = 2};
	sc.ids[0] = 3;
	assert(region_archive_set_chunk(&ra, 0, 0, &sc, &te, &remaining));
	assert(region_save_failure_count() == fail0);
	memset(&loaded, 0, sizeof(loaded));
	memset(&back, 0, sizeof(back));
	assert(region_archive_get_chunk(&ra, 0, 0, &loaded, NULL, &back));
	assert(back.count == 2 && !by_source_id(&back, 1) && by_source_id(&back, 2)
		   && by_source_id(&back, 3));
	chunk_entities_free(&back);
	server_chunk_arrays_free(&loaded);
	assert(region_archive_destroy(&ra));
	assert(raw_records() == 3);

	/* Legacy card: the same records with a TAG_INT runtime id (older archives)
	 * are matched too -- flip the stored tags and save again. */
	{
		nbt_node* tree = read_record_nbt();
		nbt_node* list = nbt_find_by_path(tree, ".Level.Entities");
		struct list_head* pos;
		list_for_each(pos, &list->payload.tag_list->entry) {
			nbt_node* rec = list_entry(pos, struct nbt_list, entry)->data;
			nbt_node* rid = compound_child(rec, "CaveWiiRuntimeId");
			if(rid && rid->type == TAG_LONG) {
				int64_t v = rid->payload.tag_long;
				rid->type = TAG_INT;
				rid->payload.tag_int = (int32_t)v;
			}
		}
		write_record_nbt(tree);
		nbt_free(tree);
	}
	rest[0].data.item.age = 900;
	assert(open_region(&ra, world));
	assert(region_archive_set_chunk(&ra, 0, 0, &sc, &te, &remaining));
	assert(region_save_failure_count() == fail0);
	memset(&loaded, 0, sizeof(loaded));
	memset(&back, 0, sizeof(back));
	assert(region_archive_get_chunk(&ra, 0, 0, &loaded, NULL, &back));
	assert(back.count == 2 && by_source_id(&back, 2)
		   && by_source_id(&back, 2)->data.item.age == 900);
	chunk_entities_free(&back);
	server_chunk_arrays_free(&loaded);
	assert(region_archive_destroy(&ra));

	/* H5: the orphan sentinel (legacy TAG_INT -1 == UINT32_MAX; also a LONG
	 * 4294967295) is NO identity: two records carrying it at DIFFERENT
	 * positions each match their own row by position -- never DUP_OWNER --
	 * and their updated fields still land. */
	rest[0].source_id = UINT32_MAX;
	rest[1].source_id = UINT32_MAX;
	rest[1].pos[0] = 7.5f; /* distinct positions: position is the identity now */
	sc.ids[0] = 4;
	assert(open_region(&ra, world));
	assert(region_archive_set_chunk(&ra, 0, 0, &sc, &te, &remaining));
	assert(region_save_failure_count() == fail0);
	assert(region_archive_destroy(&ra));
	{ /* flip the stored LONG sentinels to the legacy TAG_INT -1 */
		nbt_node* tree = read_record_nbt();
		nbt_node* list = nbt_find_by_path(tree, ".Level.Entities");
		size_t flipped = 0;
		struct list_head* pos;
		list_for_each(pos, &list->payload.tag_list->entry) {
			nbt_node* rec = list_entry(pos, struct nbt_list, entry)->data;
			nbt_node* rid = compound_child(rec, "CaveWiiRuntimeId");
			if(rid && rid->type == TAG_LONG
			   && rid->payload.tag_long == (int64_t)UINT32_MAX) {
				rid->type = TAG_INT;
				rid->payload.tag_int = -1;
				flipped++;
			}
		}
		assert(flipped == 2);
		write_record_nbt(tree);
		nbt_free(tree);
	}
	rest[0].data.item.age = 950;
	rest[1].health = 2;
	sc.ids[0] = 5;
	assert(open_region(&ra, world));
	assert(region_archive_set_chunk(&ra, 0, 0, &sc, &te, &remaining));
	assert(region_save_failure_count() == fail0
		   && "sentinel ids must match by position, not fail DUP_OWNER");
	memset(&loaded, 0, sizeof(loaded));
	memset(&back, 0, sizeof(back));
	assert(region_archive_get_chunk(&ra, 0, 0, &loaded, NULL, &back));
	assert(back.count == 2);
	for(size_t i = 0; i < back.count; i++) {
		const struct archived_entity* r = &back.entities[i];
		if(r->kind == ARCHIVED_ENTITY_ITEM)
			assert(r->data.item.age == 950);
		else
			assert(r->kind == ARCHIVED_ENTITY_MONSTER && r->health == 2);
	}
	chunk_entities_free(&back);
	server_chunk_arrays_free(&loaded);
	assert(region_archive_destroy(&ra));

	free_input_chunk(&sc);
	remove(REGION_PATH);
	string_clear(world);
	test_fixture_dir_cleanup(TMP_WORLD);
	puts("test_entity_same_pos_save: PASS");
	return 0;
}
