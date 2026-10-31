/* IMP-ENTITY-PRESERVE: imported real Beta 1.7.3 entities must round-trip
 * through the REAL region save (region_archive_set_chunk ->
 * chunk_entities_preserve) without failing the chunk and without losing data.
 *
 * Repro of the 2026-10-21 imported-world bug (tmp/imported-worlds-1021/
 * REPORT.md): quitting PMC "Beta World 2" / "Dream Journal" failed with
 * stage=entity_preserve (sub CEP_IMPORTED_UNMATCHED) on every chunk holding a
 * painting, minecart or boat, because those kinds were outside the canonical
 * imported set, the fallback matched owners by exact Pos bytes, and the
 * runtime stores positions as float, so a Beta double that a float cannot
 * hold (Entity.writeToNBT writes posX/posY/posZ as doubles [740c583
 * Entity.java:751]) never matched.
 *
 * Scenario A (canonical set): one chunk with a painting, a chest minecart, a
 * boat and two mobs, every position a non-float-exact double. Save unchanged,
 * then save with the boat removed, then with the pig moved, reloading after
 * each: every save succeeds, unchanged records keep their exact doubles, the
 * removed one is gone and nothing else is lost.
 *
 * Scenario B (preserving fallback): a minecart carrying a tag CaveWii does not
 * own (a server-plugin extra) beside a mob. Unchanged save must succeed and
 * keep both the unknown tag and the exact doubles. */

#include <assert.h>
#include <math.h>
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

/* ---- hand-built NBT (heap-owned, freed by nbt_free) ---- */
static nbt_node* mk(nbt_type type, const char* name) {
	nbt_node* n = calloc(1, sizeof(*n));
	assert(n);
	n->type = type;
	n->name = name ? strdup(name) : NULL;
	return n;
}
static struct nbt_list* sentinel(nbt_type element) {
	struct nbt_list* s = calloc(1, sizeof(*s));
	assert(s);
	INIT_LIST_HEAD(&s->entry);
	if(element != TAG_INVALID) {
		s->data = calloc(1, sizeof(*s->data));
		assert(s->data);
		s->data->type = element;
	}
	return s;
}
static nbt_node* mk_compound(const char* name) {
	nbt_node* n = mk(TAG_COMPOUND, name);
	n->payload.tag_compound = sentinel(TAG_INVALID);
	return n;
}
static nbt_node* mk_list(const char* name, nbt_type element) {
	nbt_node* n = mk(TAG_LIST, name);
	n->payload.tag_list = sentinel(element);
	return n;
}
static void append(struct nbt_list* s, nbt_node* child) {
	struct nbt_list* w = malloc(sizeof(*w));
	assert(w);
	w->data = child;
	list_add_tail(&w->entry, &s->entry);
}
static void put(nbt_node* comp, nbt_node* child) {
	append(comp->payload.tag_compound, child);
}
static void put_byte(nbt_node* c, const char* name, int8_t v) {
	nbt_node* n = mk(TAG_BYTE, name);
	n->payload.tag_byte = v;
	put(c, n);
}
static void put_short(nbt_node* c, const char* name, int16_t v) {
	nbt_node* n = mk(TAG_SHORT, name);
	n->payload.tag_short = v;
	put(c, n);
}
static void put_int(nbt_node* c, const char* name, int32_t v) {
	nbt_node* n = mk(TAG_INT, name);
	n->payload.tag_int = v;
	put(c, n);
}
static void put_float(nbt_node* c, const char* name, float v) {
	nbt_node* n = mk(TAG_FLOAT, name);
	n->payload.tag_float = v;
	put(c, n);
}
static void put_double(nbt_node* c, const char* name, double v) {
	nbt_node* n = mk(TAG_DOUBLE, name);
	n->payload.tag_double = v;
	put(c, n);
}
static void put_string(nbt_node* c, const char* name, const char* v) {
	nbt_node* n = mk(TAG_STRING, name);
	n->payload.tag_string = strdup(v);
	put(c, n);
}
static void put_doubles(nbt_node* c, const char* name, const double v[3]) {
	nbt_node* l = mk_list(name, TAG_DOUBLE);
	for(int i = 0; i < 3; i++) {
		nbt_node* d = mk(TAG_DOUBLE, NULL);
		d->payload.tag_double = v[i];
		append(l->payload.tag_list, d);
	}
	put(c, l);
}

/* Beta Entity.writeToNBT common block [740c583 Entity.java:750-758]. */
static nbt_node* beta_entity(const char* id, const double pos[3],
							 const double motion[3], float yaw) {
	nbt_node* c = mk_compound(NULL);
	put_string(c, "id", id);
	put_doubles(c, "Pos", pos);
	put_doubles(c, "Motion", motion);
	nbt_node* rot = mk_list("Rotation", TAG_FLOAT);
	nbt_node* r0 = mk(TAG_FLOAT, NULL);
	nbt_node* r1 = mk(TAG_FLOAT, NULL);
	r0->payload.tag_float = yaw;
	r1->payload.tag_float = 0.0f;
	append(rot->payload.tag_list, r0);
	append(rot->payload.tag_list, r1);
	put(c, rot);
	put_float(c, "FallDistance", 0.0f);
	put_short(c, "Fire", -1);
	put_short(c, "Air", 300);
	put_byte(c, "OnGround", 1);
	return c;
}

/* EntityLiving.writeEntityToNBT [740c583]: Health/HurtTime/DeathTime/
 * AttackTime shorts. */
static nbt_node* beta_mob(const char* id, const double pos[3]) {
	static const double motion[3] = {0.0, -0.0784000015258789, 0.0};
	nbt_node* c = beta_entity(id, pos, motion, 37.5f);
	put_short(c, "Health", 10);
	put_short(c, "HurtTime", 0);
	put_short(c, "DeathTime", 0);
	put_short(c, "AttackTime", 0);
	if(!strcmp(id, "Pig"))
		put_byte(c, "Saddle", 0); /* EntityPig.writeEntityToNBT */
	return c;
}

static const double ZERO3[3] = {0.0, 0.0, 0.0};

/* Scenario A chunk (7,2): x in [112,128), z in [32,48). */
#define CX 7
#define CZ 2
static const double PAINTING_POS[3] = {120.53125000001, 70.5, 35.0624999999};
static const double CART_POS[3] = {123.456789012, 64.0000001, 40.123456789012};
static const double BOAT_POS[3] = {125.987654321, 63.0000001, 44.111111111};
static const double PIG_POS[3] = {114.333333333, 64.0, 36.777777777};
static const double COW_POS[3] = {117.1, 64.0, 39.9};

static nbt_node* scenario_a_entities(void) {
	nbt_node* list = mk_list("Entities", TAG_COMPOUND);
	/* EntityPainting.writeEntityToNBT [740c583 EntityPainting.java:210-216]. */
	nbt_node* painting = beta_entity("Painting", PAINTING_POS, ZERO3, 0.0f);
	put_byte(painting, "Dir", 1);
	put_string(painting, "Motive", "Kebab");
	put_int(painting, "TileX", 120);
	put_int(painting, "TileY", 70);
	put_int(painting, "TileZ", 35);
	append(list->payload.tag_list, painting);
	/* EntityMinecart.writeEntityToNBT, chest cart [740c583
	 * EntityMinecart.java:598-617]: Type + Items (Slot byte + ItemStack). */
	nbt_node* cart = beta_entity("Minecart", CART_POS, ZERO3, 90.0f);
	put_int(cart, "Type", 1);
	nbt_node* items = mk_list("Items", TAG_COMPOUND);
	nbt_node* stack = mk_compound(NULL);
	put_short(stack, "id", 4);
	put_byte(stack, "Count", 12);
	put_short(stack, "Damage", 0);
	put_byte(stack, "Slot", 3);
	append(items->payload.tag_list, stack);
	put(cart, items);
	append(list->payload.tag_list, cart);
	/* EntityBoat.writeEntityToNBT is empty [740c583 EntityBoat.java:326]. */
	append(list->payload.tag_list,
		   beta_entity("Boat", BOAT_POS, ZERO3, 180.0f));
	append(list->payload.tag_list, beta_mob("Pig", PIG_POS));
	append(list->payload.tag_list, beta_mob("Cow", COW_POS));
	return list;
}

/* Scenario B chunk (8,2): x in [128,144). */
#define BX 8
#define BZ 2
static const double PLUGIN_CART_POS[3] = {130.123456789, 64.0000001, 33.3};
static const double SHEEPLESS_COW_POS[3] = {137.7, 64.0, 41.41};

static nbt_node* scenario_b_entities(void) {
	nbt_node* list = mk_list("Entities", TAG_COMPOUND);
	nbt_node* cart = beta_entity("Minecart", PLUGIN_CART_POS, ZERO3, 0.0f);
	put_int(cart, "Type", 0);
	put_int(cart, "PluginCartOwner", 4242); /* unknown to CaveWii */
	append(list->payload.tag_list, cart);
	append(list->payload.tag_list, beta_mob("Cow", SHEEPLESS_COW_POS));
	return list;
}

/* ---- raw .mcr record access for chunk (cx, cz) of r.0.0 ---- */
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
static long index_of(int cx, int cz) {
	return 4L * (long)(cx + cz * 32);
}
static nbt_node* read_record_nbt(int cx, int cz) {
	FILE* f = fopen(REGION_PATH, "rb");
	assert(f);
	uint8_t raw[4];
	assert(fseek(f, index_of(cx, cz), SEEK_SET) == 0);
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
/* Append at EOF, repoint the index, clear the timestamp (bit 31 = canonical
 * would skip the read-back merge under test). */
static void write_record_nbt(int cx, int cz, nbt_node* tree) {
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
	assert(fseek(f, index_of(cx, cz), SEEK_SET) == 0);
	assert(fwrite(raw, 4, 1, f) == 1);
	put_be32(raw, 0);
	assert(fseek(f, REGION_SECTOR_SIZE + index_of(cx, cz), SEEK_SET) == 0);
	assert(fwrite(raw, 4, 1, f) == 1);
	assert(fclose(f) == 0);
	buffer_free(&res);
}

static nbt_node* child(nbt_node* comp, const char* name) {
	if(!comp || comp->type != TAG_COMPOUND)
		return NULL;
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* c = list_entry(pos, struct nbt_list, entry)->data;
		if(c && c->name && !strcmp(c->name, name))
			return c;
	}
	return NULL;
}
static void remove_child(nbt_node* comp, const char* name) {
	struct list_head *pos, *next;
	list_for_each_safe(pos, next, &comp->payload.tag_compound->entry) {
		struct nbt_list* w = list_entry(pos, struct nbt_list, entry);
		if(w->data && w->data->name && !strcmp(w->data->name, name)) {
			list_del(pos);
			nbt_free(w->data);
			free(w);
		}
	}
}

/* Turn the chunk's CaveWii record into an imported Beta record: drop the
 * native marker and replace Level.Entities with the given Beta list. */
static void make_imported(int cx, int cz, nbt_node* entities) {
	nbt_node* tree = read_record_nbt(cx, cz);
	nbt_node* level = nbt_find_by_path(tree, ".Level");
	assert(level && level->type == TAG_COMPOUND);
	remove_child(level, "CaveWiiTileEntitiesComplete");
	remove_child(level, "Entities");
	put(level, entities);
	write_record_nbt(cx, cz, tree);
	nbt_free(tree);
}

/* Find the on-disk record with this id; NULL if absent. */
static nbt_node* raw_record(nbt_node* tree, const char* id, size_t* count) {
	nbt_node* list = nbt_find_by_path(tree, ".Level.Entities");
	assert(list && list->type == TAG_LIST);
	nbt_node* found = NULL;
	size_t n = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry) {
		nbt_node* rec = list_entry(pos, struct nbt_list, entry)->data;
		n++;
		nbt_node* rid = child(rec, "id");
		if(!found && rid && rid->type == TAG_STRING
		   && !strcmp(rid->payload.tag_string, id))
			found = rec;
	}
	if(count)
		*count = n;
	return found;
}
static bool pos_exact(nbt_node* rec, const double want[3]) {
	nbt_node* pos = child(rec, "Pos");
	if(!pos || pos->type != TAG_LIST)
		return false;
	int i = 0;
	struct list_head* p;
	list_for_each(p, &pos->payload.tag_list->entry) {
		nbt_node* d = list_entry(p, struct nbt_list, entry)->data;
		if(i >= 3 || d->type != TAG_DOUBLE || d->payload.tag_double != want[i])
			return false;
		i++;
	}
	return i == 3;
}

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

static size_t kind_count(const struct chunk_entities* e,
						 enum archived_entity_kind kind) {
	size_t n = 0;
	for(size_t i = 0; i < e->count; i++)
		n += e->entities[i].kind == kind;
	return n;
}

static bool save(string_t world, int cx, int cz, struct server_chunk* sc,
				 const struct chunk_tile_entities* te,
				 const struct chunk_entities* ents) {
	struct region_archive ra;
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	bool ok = region_archive_set_chunk(&ra, cx, cz, sc, te, ents);
	if(!ok)
		fprintf(stderr, "  save (%d,%d) failed: stage=%u sub=%u\n", cx, cz,
				(unsigned)region_save_failure_stage(),
				region_save_failure_sub());
	assert(region_archive_destroy(&ra));
	return ok;
}

static void load(string_t world, int cx, int cz, struct server_chunk* sc,
				 struct chunk_tile_entities* te, struct chunk_entities* out) {
	struct region_archive ra;
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	memset(sc, 0, sizeof(*sc));
	memset(te, 0, sizeof(*te));
	memset(out, 0, sizeof(*out));
	assert(region_archive_get_chunk(&ra, cx, cz, sc, te, out));
	assert(region_archive_destroy(&ra));
}

static void scenario_a(string_t world) {
	make_imported(CX, CZ, scenario_a_entities());

	struct server_chunk sc;
	struct chunk_tile_entities te;
	struct chunk_entities en;
	load(world, CX, CZ, &sc, &te, &en);
	assert(en.count == 5 && "all five imported Beta records are admitted");
	assert(kind_count(&en, ARCHIVED_ENTITY_PAINTING) == 1);
	assert(kind_count(&en, ARCHIVED_ENTITY_MINECART) == 1);
	assert(kind_count(&en, ARCHIVED_ENTITY_BOAT) == 1);
	assert(kind_count(&en, ARCHIVED_ENTITY_MONSTER) == 2);

	/* 1. unchanged re-save (the quit path on an untouched imported chunk) */
	sc.ids[0] = 1; /* block delta: the record cannot be a whole-record no-op */
	assert(save(world, CX, CZ, &sc, &te, &en)
		   && "unchanged imported painting/cart/boat chunk must save");
	{
		nbt_node* tree = read_record_nbt(CX, CZ);
		size_t n = 0;
		assert(pos_exact(raw_record(tree, "Painting", &n), PAINTING_POS));
		assert(n == 5);
		assert(pos_exact(raw_record(tree, "Minecart", NULL), CART_POS));
		assert(pos_exact(raw_record(tree, "Boat", NULL), BOAT_POS));
		assert(pos_exact(raw_record(tree, "Pig", NULL), PIG_POS));
		assert(pos_exact(raw_record(tree, "Cow", NULL), COW_POS));
		nbt_free(tree);
	}

	/* 2. save after removing one entity (the boat was broken) */
	struct archived_entity kept[4];
	size_t k = 0;
	for(size_t i = 0; i < en.count; i++)
		if(en.entities[i].kind != ARCHIVED_ENTITY_BOAT)
			kept[k++] = en.entities[i];
	assert(k == 4);
	struct chunk_entities fewer = {.entities = kept, .count = 4};
	sc.ids[0] = 2;
	assert(save(world, CX, CZ, &sc, &te, &fewer)
		   && "imported chunk must save after an entity is removed");
	{
		nbt_node* tree = read_record_nbt(CX, CZ);
		size_t n = 0;
		assert(!raw_record(tree, "Boat", &n) && n == 4);
		assert(pos_exact(raw_record(tree, "Painting", NULL), PAINTING_POS));
		assert(pos_exact(raw_record(tree, "Minecart", NULL), CART_POS));
		nbt_free(tree);
	}

	/* 3. reload: nothing else lost */
	struct server_chunk sc2;
	struct chunk_tile_entities te2;
	struct chunk_entities back;
	load(world, CX, CZ, &sc2, &te2, &back);
	assert(back.count == 4);
	assert(kind_count(&back, ARCHIVED_ENTITY_BOAT) == 0);
	assert(kind_count(&back, ARCHIVED_ENTITY_PAINTING) == 1);
	assert(kind_count(&back, ARCHIVED_ENTITY_MINECART) == 1);
	assert(kind_count(&back, ARCHIVED_ENTITY_MONSTER) == 2);
	for(size_t i = 0; i < back.count; i++) {
		const struct archived_entity* e = &back.entities[i];
		if(e->kind == ARCHIVED_ENTITY_PAINTING) {
			assert(e->data.painting.tile_x == 120);
			assert(e->data.painting.tile_y == 70);
			assert(e->data.painting.tile_z == 35);
			assert(e->data.painting.direction == 1);
			assert(!strcmp(e->data.painting.art_name, "Kebab"));
		} else if(e->kind == ARCHIVED_ENTITY_MINECART) {
			assert(e->data.minecart.minecart_type == 1);
			assert(e->data.minecart.inventory[3].id == 4);
			assert(e->data.minecart.inventory[3].count == 12);
		}
	}

	/* 4. a moved mob (position is not identity in the canonical set) */
	for(size_t i = 0; i < back.count; i++)
		if(back.entities[i].kind == ARCHIVED_ENTITY_MONSTER
		   && back.entities[i].data.monster.monster_id == MONSTER_PIG)
			back.entities[i].pos[0] += 0.5f;
	sc2.ids[0] = 3;
	assert(save(world, CX, CZ, &sc2, &te2, &back)
		   && "imported chunk must save after a mob moved");
	{
		nbt_node* tree = read_record_nbt(CX, CZ);
		size_t n = 0;
		assert(pos_exact(raw_record(tree, "Painting", &n), PAINTING_POS));
		assert(n == 4);
		assert(pos_exact(raw_record(tree, "Minecart", NULL), CART_POS));
		assert(pos_exact(raw_record(tree, "Cow", NULL), COW_POS));
		assert(!pos_exact(raw_record(tree, "Pig", NULL), PIG_POS));
		nbt_free(tree);
	}
	struct server_chunk sc3;
	struct chunk_tile_entities te3;
	struct chunk_entities again;
	load(world, CX, CZ, &sc3, &te3, &again);
	assert(again.count == 4);

	chunk_entities_free(&again);
	tile_entities_parse_free(&te3);
	server_chunk_arrays_free(&sc3);
	chunk_entities_free(&back);
	tile_entities_parse_free(&te2);
	server_chunk_arrays_free(&sc2);
	chunk_entities_free(&en);
	tile_entities_parse_free(&te);
	server_chunk_arrays_free(&sc);
	printf("scenario A (canonical painting/cart/boat + mobs): PASS\n");
}

static void scenario_b(string_t world) {
	make_imported(BX, BZ, scenario_b_entities());
	struct server_chunk sc;
	struct chunk_tile_entities te;
	struct chunk_entities en;
	load(world, BX, BZ, &sc, &te, &en);
	assert(en.count == 2);
	sc.ids[0] = 1;
	assert(save(world, BX, BZ, &sc, &te, &en)
		   && "unchanged chunk with an unowned-tag cart must still save");
	nbt_node* tree = read_record_nbt(BX, BZ);
	size_t n = 0;
	nbt_node* cart = raw_record(tree, "Minecart", &n);
	assert(n == 2 && cart);
	assert(pos_exact(cart, PLUGIN_CART_POS));
	nbt_node* extra = child(cart, "PluginCartOwner");
	assert(extra && extra->type == TAG_INT && extra->payload.tag_int == 4242);
	assert(pos_exact(raw_record(tree, "Cow", NULL), SHEEPLESS_COW_POS));
	nbt_free(tree);
	chunk_entities_free(&en);
	tile_entities_parse_free(&te);
	server_chunk_arrays_free(&sc);
	printf("scenario B (unowned-tag cart, preserving merge): PASS\n");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_imported_entity_roundtrip_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(REGION_PATH);
	string_t world;
	string_init_set_str(world, TMP_WORLD);

	/* Seed native records for both chunks, then rewrite them as imports. */
	struct server_chunk seed;
	alloc_chunk(&seed);
	struct chunk_tile_entities te = {.legacy_flat_complete = true};
	struct chunk_entities none = {0};
	struct region_archive ra;
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_set_chunk(&ra, CX, CZ, &seed, &te, &none));
	assert(region_archive_set_chunk(&ra, BX, BZ, &seed, &te, &none));
	assert(region_archive_destroy(&ra));
	free_input_chunk(&seed);

	scenario_a(world);
	scenario_b(world);

	string_clear(world);
	printf("test_imported_entity_roundtrip: PASS\n");
	return 0;
}
