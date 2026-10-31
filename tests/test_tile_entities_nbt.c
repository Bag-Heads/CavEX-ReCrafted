/*
	Round-trip test for the chest/sign tile-entity NBT (de)serialization
	(source/network/tile_entities_nbt.c) -- Stage 1 of the chest-store redesign
	(docs/chest_store_redesign.md). Builds a chunk's chests + signs, serialises to
	the Beta TileEntities NBT, parses it back, and asserts every coord / item /
	sign survives. In-memory only: no region file, so a build/parse bug is caught
	here before region_archive (which rewrites the whole chunk NBT) ever uses it.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "network/tile_entities_nbt.h"

#define ROUNDTRIP_CHUNK_X 0
#define ROUNDTRIP_CHUNK_Z -1

static void assert_empty(const struct chunk_tile_entities* te) {
	assert(!te->chests && te->n_chest == 0);
	assert(!te->signs && te->n_sign == 0);
	assert(!te->furnaces && te->n_furnace == 0);
	assert(!te->spawners && te->n_spawner == 0);
	assert(!te->piston_movings && te->n_piston_moving == 0);
}

static nbt_node* direct_child(nbt_node* comp, const char* name) {
	assert(comp && comp->type == TAG_COMPOUND && comp->payload.tag_compound);
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* child = list_entry(pos, struct nbt_list, entry)->data;
		if(child && child->name && strcmp(child->name, name) == 0)
			return child;
	}
	return NULL;
}

// CR-04: build the packed grid te_sign.text holds internally -- 4 lines of
// TE_SIGN_LINE_COLS columns, each right-space-padded, content <=
// TE_SIGN_LINE_MAX chars/line -- matching how the live system actually
// produces it (server_local's sign_texts / block_sign.c's seed both
// space-fill unused columns; inventory_sign.c never lets a real character
// land past column TE_SIGN_LINE_MAX-1). A test that instead strcpy's a raw
// NUL-padded string across line boundaries would exercise a shape the codec
// never actually receives.
static void set_sign_grid(char text[TE_SIGN_SIZE + 1], const char* l0,
						  const char* l1, const char* l2, const char* l3) {
	const char* lines[4] = {l0, l1, l2, l3};
	memset(text, ' ', TE_SIGN_SIZE);
	text[TE_SIGN_SIZE] = '\0';
	for(int i = 0; i < 4; i++) {
		size_t len = strlen(lines[i]);
		assert(len <= TE_SIGN_LINE_MAX);
		memcpy(&text[i * TE_SIGN_LINE_COLS], lines[i], len);
	}
}

static void assert_parse_rejected(nbt_node* list, int32_t chunk_x,
							  int32_t chunk_z) {
	struct chunk_tile_entities out;
	assert(!tile_entities_parse(list, chunk_x, chunk_z, &out));
	assert_empty(&out);
}

static void test_supported_record_bound(void) {
	struct nbt_list kids = {.data = NULL};
	INIT_LIST_HEAD(&kids.entry);
	nbt_node id = {.type = TAG_STRING, .name = "id",
				   .payload.tag_string = "Chest"};
	struct nbt_list id_link = {.data = &id};
	INIT_LIST_HEAD(&id_link.entry);
	list_add_tail(&id_link.entry, &kids.entry);
	nbt_node chest = {.type = TAG_COMPOUND, .payload.tag_compound = &kids};

	nbt_node compound_type = {.type = TAG_COMPOUND};
	struct nbt_list outer = {.data = &compound_type};
	INIT_LIST_HEAD(&outer.entry);
	/* IMP-TE-CAPS: the per-kind caps are gone; the total examined-record
	 * bound (one chunk's block count) is what still refuses a flood. */
	size_t count = TE_RECORDS_EXAMINED_MAX + 1;
	struct nbt_list* links = calloc(count, sizeof(*links));
	assert(links);
	for(size_t i = 0; i < count; i++) {
		links[i].data = &chest;
		INIT_LIST_HEAD(&links[i].entry);
		list_add_tail(&links[i].entry, &outer.entry);
	}

	nbt_node list = {.type = TAG_LIST, .payload.tag_list = &outer};
	struct chunk_tile_entities out;
	assert(!tile_entities_parse(&list, 0, 0, &out));
	assert_empty(&out);
	for(size_t i = 0; i < count; i++)
		list_del(&links[i].entry);
	free(links);
}

static void test_item_record_bound(void) {
	struct te_chest chest = {.x = 1, .y = 64, .z = 1};
	chest.items[0] = (struct item_data) {.id = 1, .count = 1};
	struct chunk_tile_entities input = {.chests = &chest, .n_chest = 1};
	void* arena = NULL;
	struct nbt_list* outer = tile_entities_build(&input, 0, 0, &arena);
	assert(outer && arena);
	struct nbt_list* first
		= list_entry(outer->entry.flink, struct nbt_list, entry);
	nbt_node* items = direct_child(first->data, "Items");
	assert(items && items->type == TAG_LIST && items->payload.tag_list);

	nbt_node malformed = {.type = TAG_INVALID};
	struct nbt_list* extras
		= calloc(TE_ITEM_RECORDS_EXAMINED_MAX, sizeof(*extras));
	assert(extras);
	for(size_t i = 0; i < TE_ITEM_RECORDS_EXAMINED_MAX; i++) {
		extras[i].data = &malformed;
		INIT_LIST_HEAD(&extras[i].entry);
		list_add_tail(&extras[i].entry, &items->payload.tag_list->entry);
	}

	nbt_node list = {.type = TAG_LIST, .payload.tag_list = outer};
	struct chunk_tile_entities out;
	assert(!tile_entities_parse(&list, 0, 0, &out));
	assert_empty(&out);
	for(size_t i = 0; i < TE_ITEM_RECORDS_EXAMINED_MAX; i++)
		list_del(&extras[i].entry);
	free(extras);
	tile_entities_build_free(arena);
}

static nbt_node* tile_entity_at(struct nbt_list* sentinel, int index) {
	int current = 0;
	struct list_head* pos;
	list_for_each(pos, &sentinel->entry) {
		if(current++ == index)
			return list_entry(pos, struct nbt_list, entry)->data;
	}
	return NULL;
}

static void assert_build_rejected(const struct chunk_tile_entities* te,
								  int32_t chunk_x, int32_t chunk_z) {
	void* arena = (void*)0x1;
	assert(tile_entities_build(te, chunk_x, chunk_z, &arena) == NULL);
	assert(arena == NULL);
}

static void assert_int_field_rejected(nbt_node* list, nbt_node* comp,
								  const char* name, int32_t value,
								  int32_t chunk_x, int32_t chunk_z) {
	nbt_node* field = direct_child(comp, name);
	assert(field && field->type == TAG_INT);
	int32_t saved = field->payload.tag_int;
	field->payload.tag_int = value;
	assert_parse_rejected(list, chunk_x, chunk_z);
	field->payload.tag_int = saved;
}

static void assert_float_field_rejected(nbt_node* list, nbt_node* comp,
									const char* name, float value,
									int32_t chunk_x, int32_t chunk_z) {
	nbt_node* field = direct_child(comp, name);
	assert(field && field->type == TAG_FLOAT);
	float saved = field->payload.tag_float;
	field->payload.tag_float = value;
	assert_parse_rejected(list, chunk_x, chunk_z);
	field->payload.tag_float = saved;
}

static void assert_byte_field_rejected(nbt_node* list, nbt_node* comp,
								   const char* name, int8_t value,
								   int32_t chunk_x, int32_t chunk_z) {
	nbt_node* field = direct_child(comp, name);
	assert(field && field->type == TAG_BYTE);
	int8_t saved = field->payload.tag_byte;
	field->payload.tag_byte = value;
	assert_parse_rejected(list, chunk_x, chunk_z);
	field->payload.tag_byte = saved;
}

// FH-17: MobSpawner round-trip + ownership/type-rejection, mirroring the
// shape test_chunk_ownership_and_piston_ranges below uses for its 4 kinds --
// kept as its own function rather than a 5th slot in that already-dense one.
static void test_spawner_ownership_and_rejection(void) {
	const int32_t chunk_x = 2;	// owns world x [32,47]
	const int32_t chunk_z = -3; // owns world z [-48,-33]
	struct te_spawner spawner = {.x = 32, .y = 60, .z = -48};
	strcpy(spawner.mob_id, "Zombie");
	spawner.delay = 45;
	struct chunk_tile_entities te = {
		.spawners = &spawner,
		.n_spawner = 1,
	};

	void* arena = NULL;
	struct nbt_list* sentinel
		= tile_entities_build(&te, chunk_x, chunk_z, &arena);
	assert(sentinel && arena);
	nbt_node list = {
		.type = TAG_LIST,
		.name = "TileEntities",
		.payload.tag_list = sentinel,
	};

	struct chunk_tile_entities parsed;
	assert(tile_entities_parse(&list, chunk_x, chunk_z, &parsed));
	assert(parsed.n_spawner == 1);
	assert(parsed.spawners[0].x == 32 && parsed.spawners[0].y == 60
		   && parsed.spawners[0].z == -48);
	assert(strcmp(parsed.spawners[0].mob_id, "Zombie") == 0);
	assert(parsed.spawners[0].delay == 45);
	tile_entities_parse_free(&parsed);

	nbt_node* comp = tile_entity_at(sentinel, 0);
	assert(comp && comp->type == TAG_COMPOUND);

	// Both chunk edges are owned (proven above); one block beyond each edge,
	// and out-of-world y at both ends, reject the complete parse transaction.
	assert_int_field_rejected(&list, comp, "x", 16, chunk_x, chunk_z);
	assert_int_field_rejected(&list, comp, "z", -32, chunk_x, chunk_z);
	assert_int_field_rejected(&list, comp, "y", -1, chunk_x, chunk_z);
	assert_int_field_rejected(&list, comp, "y", 128, chunk_x, chunk_z);

	// A supported field with the wrong NBT type rejects the complete parse
	// rather than being silently coerced or defaulted.
	nbt_node* entity_field = direct_child(comp, "EntityId");
	assert(entity_field && entity_field->type == TAG_STRING);
	entity_field->type = TAG_INT;
	assert_parse_rejected(&list, chunk_x, chunk_z);
	entity_field->type = TAG_STRING;

	nbt_node* delay_field = direct_child(comp, "Delay");
	assert(delay_field && delay_field->type == TAG_SHORT);
	delay_field->type = TAG_INT;
	assert_parse_rejected(&list, chunk_x, chunk_z);
	delay_field->type = TAG_SHORT;

	// Build applies the same ownership/y policy to malformed in-memory records.
	spawner.x = 16;
	assert_build_rejected(&te, chunk_x, chunk_z);
	spawner.x = 32;
	spawner.y = -1;
	assert_build_rejected(&te, chunk_x, chunk_z);
	spawner.y = 128;
	assert_build_rejected(&te, chunk_x, chunk_z);
	spawner.y = 60;

	// An EntityId buffer with no NUL anywhere is rejected at build -- the same
	// defensive contract the sign codec's memchr check enforces.
	memset(spawner.mob_id, 'A', sizeof(spawner.mob_id));
	assert_build_rejected(&te, chunk_x, chunk_z);
	memset(spawner.mob_id, 0, sizeof(spawner.mob_id));
	strcpy(spawner.mob_id, "Zombie");

	tile_entities_build_free(arena);
}

static void test_chunk_ownership_and_piston_ranges(void) {
	const int32_t chunk_x = -1;
	const int32_t chunk_z = -1;
	struct te_chest chest = {.x = -16, .y = 0, .z = -16};
	struct te_sign sign = {.x = -1, .y = 127, .z = -1};
	set_sign_grid(sign.text, "negative bound", "ary", "", "");
	struct te_furnace furnace = {.x = -8, .y = 64, .z = -9};
	struct te_piston_moving piston = {
		.x = -2,
		.y = 1,
		.z = -15,
		.stored_id = UINT8_MAX,
		.stored_meta = 0x0F,
		.facing = 5,
		.progress = 1.0F,
		.extending = 1,
	};
	struct chunk_tile_entities te = {
		.chests = &chest,
		.n_chest = 1,
		.signs = &sign,
		.n_sign = 1,
		.furnaces = &furnace,
		.n_furnace = 1,
		.piston_movings = &piston,
		.n_piston_moving = 1,
	};

	void* arena = NULL;
	struct nbt_list* sentinel
		= tile_entities_build(&te, chunk_x, chunk_z, &arena);
	assert(sentinel && arena);
	nbt_node list = {
		.type = TAG_LIST,
		.name = "TileEntities",
		.payload.tag_list = sentinel,
	};
	struct chunk_tile_entities parsed;
	assert(tile_entities_parse(&list, chunk_x, chunk_z, &parsed));
	assert(parsed.n_chest == 1 && parsed.n_sign == 1
		   && parsed.n_furnace == 1 && parsed.n_piston_moving == 1);
	assert(parsed.chests[0].x == -16 && parsed.signs[0].x == -1);
	assert(parsed.piston_movings[0].stored_id == UINT8_MAX
		   && parsed.piston_movings[0].stored_meta == 0x0F);
	tile_entities_parse_free(&parsed);

	nbt_node* compounds[4] = {
		tile_entity_at(sentinel, 0),
		tile_entity_at(sentinel, 1),
		tile_entity_at(sentinel, 2),
		tile_entity_at(sentinel, 3),
	};
	for(int i = 0; i < 4; i++)
		assert(compounds[i] && compounds[i]->type == TAG_COMPOUND);

	// Unknown/modded IDs remain ignored even when their coordinates are not
	// owned by this chunk. Ownership is enforced only for supported records.
	nbt_node* chest_id = direct_child(compounds[0], "id");
	nbt_node* chest_x = direct_child(compounds[0], "x");
	assert(chest_id && chest_id->type == TAG_STRING && chest_x);
	char* saved_id = chest_id->payload.tag_string;
	int32_t saved_x = chest_x->payload.tag_int;
	chest_id->payload.tag_string = "ModdedChest";
	chest_x->payload.tag_int = INT32_MAX;
	assert(tile_entities_parse(&list, chunk_x, chunk_z, &parsed));
	assert(parsed.n_chest == 0 && parsed.n_sign == 1
		   && parsed.n_furnace == 1 && parsed.n_piston_moving == 1);
	tile_entities_parse_free(&parsed);
	chest_id->payload.tag_string = saved_id;
	chest_x->payload.tag_int = saved_x;

	// Both edges of a negative chunk are accepted above. One block beyond each
	// edge, for every supported kind, rejects the complete parse transaction.
	assert_int_field_rejected(&list, compounds[0], "x", -17, chunk_x, chunk_z);
	assert_int_field_rejected(&list, compounds[1], "x", 0, chunk_x, chunk_z);
	assert_int_field_rejected(&list, compounds[2], "z", -17, chunk_x, chunk_z);
	assert_int_field_rejected(&list, compounds[3], "z", 0, chunk_x, chunk_z);

	// Every supported kind rejects y below the world and at/above its ceiling.
	for(int i = 0; i < 4; i++) {
		assert_int_field_rejected(
			&list, compounds[i], "y", -1, chunk_x, chunk_z);
		assert_int_field_rejected(
			&list, compounds[i], "y", 128, chunk_x, chunk_z);
	}

	// Piston fields are persisted as wider NBT types but narrow into byte-backed
	// runtime stores. Reject every nonrepresentable value before those casts.
	assert_int_field_rejected(
		&list, compounds[3], "blockId", -1, chunk_x, chunk_z);
	assert_int_field_rejected(
		&list, compounds[3], "blockId", 256, chunk_x, chunk_z);
	assert_int_field_rejected(
		&list, compounds[3], "blockData", -1, chunk_x, chunk_z);
	assert_int_field_rejected(
		&list, compounds[3], "blockData", 16, chunk_x, chunk_z);
	assert_int_field_rejected(
		&list, compounds[3], "facing", -1, chunk_x, chunk_z);
	assert_int_field_rejected(
		&list, compounds[3], "facing", 6, chunk_x, chunk_z);
	assert_float_field_rejected(
		&list, compounds[3], "progress", -0.01F, chunk_x, chunk_z);
	assert_float_field_rejected(
		&list, compounds[3], "progress", 1.01F, chunk_x, chunk_z);
	assert_float_field_rejected(
		&list, compounds[3], "progress", NAN, chunk_x, chunk_z);
	assert_float_field_rejected(
		&list, compounds[3], "progress", INFINITY, chunk_x, chunk_z);
	assert_byte_field_rejected(
		&list, compounds[3], "extending", -1, chunk_x, chunk_z);
	assert_byte_field_rejected(
		&list, compounds[3], "extending", 2, chunk_x, chunk_z);

	// Build applies the same ownership/y/value policy, so malformed in-memory
	// records cannot be truncated while saving.
	chest.x = -17;
	assert_build_rejected(&te, chunk_x, chunk_z);
	chest.x = -16;
	sign.x = 0;
	assert_build_rejected(&te, chunk_x, chunk_z);
	sign.x = -1;
	furnace.z = -17;
	assert_build_rejected(&te, chunk_x, chunk_z);
	furnace.z = -9;
	piston.z = 0;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.z = -15;

	chest.y = -1;
	assert_build_rejected(&te, chunk_x, chunk_z);
	chest.y = 0;
	sign.y = 128;
	assert_build_rejected(&te, chunk_x, chunk_z);
	sign.y = 127;
	furnace.y = -1;
	assert_build_rejected(&te, chunk_x, chunk_z);
	furnace.y = 64;
	piston.y = 128;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.y = 1;

	piston.stored_id = -1;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.stored_id = 256;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.stored_id = UINT8_MAX;
	piston.stored_meta = -1;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.stored_meta = 16;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.stored_meta = 0x0F;
	piston.facing = -1;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.facing = 6;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.facing = 5;
	piston.progress = -0.01F;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.progress = 1.01F;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.progress = NAN;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.progress = INFINITY;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.progress = 1.0F;
	piston.extending = -1;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.extending = 2;
	assert_build_rejected(&te, chunk_x, chunk_z);
	piston.extending = 1;

	tile_entities_build_free(arena);
}

static void test_more_than_64_pistons_binary_round_trip(void) {
	/* Beta 1.7.3 ChunkLoader.storeChunkInCompound (decompile 740c583)
	 * serializes every value in chunkTileEntityMap into TileEntities, with no
	 * 64-record policy cap. Five disjoint maximum piston moves can own 13
	 * moving TEs each (moving head + 12 moved blocks), so 65 is a reachable
	 * per-chunk checkpoint image and must survive the complete NBT path. */
	enum { PISTON_RECORDS = 65 };
	struct te_piston_moving pistons[PISTON_RECORDS];
	memset(pistons, 0, sizeof(pistons));
	for(int i = 0; i < PISTON_RECORDS; i++) {
		pistons[i].x = i & 15;
		pistons[i].y = 64;
		pistons[i].z = i >> 4;
		pistons[i].stored_id = 1 + i;
		pistons[i].stored_meta = i & 15;
		pistons[i].facing = i % 6;
		pistons[i].progress = (float)(i % 3) * 0.5F;
		pistons[i].extending = (int8_t)(i & 1);
	}
	struct chunk_tile_entities input = {
		.piston_movings = pistons,
		.n_piston_moving = PISTON_RECORDS,
	};

	void* arena = NULL;
	struct nbt_list* sentinel
		= tile_entities_build(&input, 0, 0, &arena);
	assert(sentinel && arena);
	nbt_node list = {
		.type = TAG_LIST,
		.name = "TileEntities",
		.payload.tag_list = sentinel,
	};
	struct nbt_list root_sentinel = {.data = NULL};
	INIT_LIST_HEAD(&root_sentinel.entry);
	struct nbt_list te_link = {.data = &list};
	list_add_tail(&te_link.entry, &root_sentinel.entry);
	nbt_node root = {
		.type = TAG_COMPOUND,
		.name = "",
		.payload.tag_compound = &root_sentinel,
	};

	struct buffer dumped = nbt_dump_binary(&root);
	assert(dumped.data && dumped.len > 0);
	nbt_node* reparsed = nbt_parse(dumped.data, dumped.len);
	assert(reparsed);
	nbt_node* reparsed_list = nbt_find_by_path(reparsed, ".TileEntities");
	assert(reparsed_list && reparsed_list->type == TAG_LIST);

	struct chunk_tile_entities output;
	assert(tile_entities_parse(reparsed_list, 0, 0, &output));
	assert(output.n_piston_moving == PISTON_RECORDS);
	for(int i = 0; i < PISTON_RECORDS; i++) {
		assert(output.piston_movings[i].x == pistons[i].x);
		assert(output.piston_movings[i].y == pistons[i].y);
		assert(output.piston_movings[i].z == pistons[i].z);
		assert(output.piston_movings[i].stored_id == pistons[i].stored_id);
		assert(output.piston_movings[i].stored_meta == pistons[i].stored_meta);
		assert(output.piston_movings[i].facing == pistons[i].facing);
		assert(output.piston_movings[i].progress == pistons[i].progress);
		assert(output.piston_movings[i].extending == pistons[i].extending);
	}

	tile_entities_parse_free(&output);
	nbt_free(reparsed);
	buffer_free(&dumped);
	tile_entities_build_free(arena);
}

// CR-04: hand-authored Beta-layout golden. Every node below is built by hand
// -- tile_entities_build is NEVER called here -- so this proves the READER
// matches Beta's actual on-disk Sign{Text1..Text4} shape independently of
// whatever this port's own writer happens to emit (the audit's exact
// complaint about the old test: a self-round-trip that could hide either
// side breaking to match the other). Reference: Beta
// TileEntitySign.write/readFromNBT [740c583] -- writeToNBT writes each of the
// 4 signText[] entries verbatim as Text1..Text4; readFromNBT hard-clamps any
// line over 15 chars to its first 15 (`substring(0, 15)`).
static void test_four_line_sign_beta_golden(void) {
	nbt_node id = {.type = TAG_STRING, .name = "id",
				   .payload.tag_string = "Sign"};
	nbt_node x = {.type = TAG_INT, .name = "x", .payload.tag_int = 3};
	nbt_node y = {.type = TAG_INT, .name = "y", .payload.tag_int = 65};
	/* z must be OWNED by the chunk this parse claims -- chunk (0,0) spans
	 * x,z [0,16), and te_position_is_owned correctly rejects strays (an
	 * earlier draft used -9 and was rightly refused). */
	nbt_node z = {.type = TAG_INT, .name = "z", .payload.tag_int = 9};
	// Four genuinely distinct lines, each <= Beta's 15-char cap, so none are
	// truncated by THIS sub-case (the clamp itself is exercised separately
	// below).
	nbt_node text1 = {.type = TAG_STRING, .name = "Text1",
					  .payload.tag_string = "First line!"};
	nbt_node text2 = {.type = TAG_STRING, .name = "Text2",
					  .payload.tag_string = "Second line"};
	nbt_node text3 = {.type = TAG_STRING, .name = "Text3",
					  .payload.tag_string = "3rd: numbers"};
	nbt_node text4 = {.type = TAG_STRING, .name = "Text4",
					  .payload.tag_string = "-- fin --"};
	nbt_node* fields[8] = {&id, &x, &y, &z, &text1, &text2, &text3, &text4};

	struct nbt_list kids = {.data = NULL};
	INIT_LIST_HEAD(&kids.entry);
	struct nbt_list links[8];
	for(int i = 0; i < 8; i++) {
		links[i].data = fields[i];
		INIT_LIST_HEAD(&links[i].entry);
		list_add_tail(&links[i].entry, &kids.entry);
	}
	nbt_node sign_comp = {.type = TAG_COMPOUND, .payload.tag_compound = &kids};

	nbt_node compound_type = {.type = TAG_COMPOUND};
	struct nbt_list outer = {.data = &compound_type};
	INIT_LIST_HEAD(&outer.entry);
	struct nbt_list sign_link = {.data = &sign_comp};
	INIT_LIST_HEAD(&sign_link.entry);
	list_add_tail(&sign_link.entry, &outer.entry);

	nbt_node list = {.type = TAG_LIST, .name = "TileEntities",
					 .payload.tag_list = &outer};

	struct chunk_tile_entities out;
	assert(tile_entities_parse(&list, 0, 0, &out));
	assert(out.n_sign == 1);
	assert(out.signs[0].x == 3 && out.signs[0].y == 65
		   && out.signs[0].z == 9);

	char expected[TE_SIGN_SIZE + 1];
	set_sign_grid(expected, "First line!", "Second line", "3rd: numbers",
				 "-- fin --");
	assert(memcmp(out.signs[0].text, expected, TE_SIGN_SIZE) == 0);
	tile_entities_parse_free(&out);

	// Beta's own import clamp [740c583 TileEntitySign.readFromNBT]: a line
	// over 15 chars lands as exactly its first 15 characters (COUNT-only,
	// maintainer ruling 2026-08-17) -- not silently accepted whole, not
	// rejected outright.
	nbt_node text1_over = {.type = TAG_STRING, .name = "Text1",
						   .payload.tag_string = "0123456789ABCDEFGHIJ"}; // 20 chars
	links[4].data = &text1_over;

	struct chunk_tile_entities out_clamped;
	assert(tile_entities_parse(&list, 0, 0, &out_clamped));
	assert(out_clamped.n_sign == 1);
	char expected_clamped[TE_SIGN_SIZE + 1];
	set_sign_grid(expected_clamped, "0123456789ABCDE", "Second line",
				 "3rd: numbers", "-- fin --");
	assert(memcmp(out_clamped.signs[0].text, expected_clamped, TE_SIGN_SIZE)
		   == 0);
	tile_entities_parse_free(&out_clamped);

	// 2026-09-01 fix: Beta's substring(0, 15) clamps by CHARACTER count, not
	// byte count. A line whose 15th BYTE lands inside a multibyte UTF-8
	// sequence must clamp BEFORE that sequence, never split it in half. 14
	// ASCII bytes (indices 0-13) then U+00E9 'e-acute' encoded as the 2-byte
	// UTF-8 sequence C3 A9 (indices 14-15) then more trailing content: a
	// byte-count clamp at 15 keeps indices [0,15) -- the 14 ASCII bytes plus
	// ONLY the LEAD byte 0xC3 of the accented character, a truncated/invalid
	// sequence with its continuation byte 0xA9 chopped off. The fix must
	// instead stop BEFORE that whole codepoint (14 bytes/chars: the accented
	// character does not fit within the 15-byte column once its full 2-byte
	// encoding is counted), never emitting a lone lead byte.
	nbt_node text1_multibyte
		= {.type = TAG_STRING,
		   .name = "Text1",
		   .payload.tag_string = "0123456789ABCD\xC3\xA9XYZ"};
	links[4].data = &text1_multibyte;

	struct chunk_tile_entities out_multibyte;
	assert(tile_entities_parse(&list, 0, 0, &out_multibyte));
	assert(out_multibyte.n_sign == 1);
	char expected_multibyte[TE_SIGN_SIZE + 1];
	set_sign_grid(expected_multibyte, "0123456789ABCD", "Second line",
				 "3rd: numbers", "-- fin --");
	assert(memcmp(out_multibyte.signs[0].text, expected_multibyte,
				 TE_SIGN_SIZE)
		   == 0
		   && "a multibyte UTF-8 sequence straddling the 15-byte clamp must "
			  "be dropped WHOLE (14 complete chars), never split -- a lone "
			  "0xC3 lead byte with its continuation chopped off is exactly "
			  "the pre-fix corruption");
	tile_entities_parse_free(&out_multibyte);
}


/* IMP-TE-CAPS: a real Beta chunk is bounded only by its block count, never by
 * a per-kind record cap (Beta Chunk.chunkTileEntityMap is an unbounded
 * HashMap). Dream Journal's densest chunk holds 384 note blocks ("Music");
 * this one also carries more than the old 64/256 per-kind limits of every
 * other kind. Build -> binary dump -> reparse -> parse must keep every record
 * in order. */
static void dense_pos(int i, int32_t* x, int32_t* y, int32_t* z) {
	/* distinct positions inside chunk (0,-1): x 0..15, z -16..-1, y 0.. */
	*x = i % 16;
	*z = -16 + (i / 16) % 16;
	*y = i / 256;
}

static void test_dense_chunk_no_per_kind_cap(void) {
	enum { NOTES = 384, CHESTS = 300, RECORDS = 44, FURNACES = 70,
		   DISPENSERS = 70, SIGNS = 300, SPAWNERS = 70 };
	struct chunk_tile_entities in = {0};
	in.notes = calloc(NOTES, sizeof(*in.notes));
	in.chests = calloc(CHESTS, sizeof(*in.chests));
	in.record_players = calloc(RECORDS, sizeof(*in.record_players));
	in.furnaces = calloc(FURNACES, sizeof(*in.furnaces));
	in.dispensers = calloc(DISPENSERS, sizeof(*in.dispensers));
	in.signs = calloc(SIGNS, sizeof(*in.signs));
	in.spawners = calloc(SPAWNERS, sizeof(*in.spawners));
	assert(in.notes && in.chests && in.record_players && in.furnaces
		   && in.dispensers && in.signs && in.spawners);
	int at = 0;
	for(int i = 0; i < NOTES; i++, at++) {
		dense_pos(at, &in.notes[i].x, &in.notes[i].y, &in.notes[i].z);
		in.notes[i].note = (int8_t)(i % 25);
	}
	for(int i = 0; i < CHESTS; i++, at++) {
		dense_pos(at, &in.chests[i].x, &in.chests[i].y, &in.chests[i].z);
		in.chests[i].items[i % TE_CHEST_SLOTS]
			= (struct item_data) {.id = 1 + i % 90, .count = 1 + i % 64};
	}
	for(int i = 0; i < RECORDS; i++, at++) {
		dense_pos(at, &in.record_players[i].x, &in.record_players[i].y,
				  &in.record_players[i].z);
		in.record_players[i].record = i % 2 ? 2256 : 0;
	}
	for(int i = 0; i < FURNACES; i++, at++) {
		dense_pos(at, &in.furnaces[i].x, &in.furnaces[i].y, &in.furnaces[i].z);
		in.furnaces[i].burn_time = (int16_t)i;
		in.furnaces[i].cook_time = (int16_t)(i % 200);
	}
	for(int i = 0; i < DISPENSERS; i++, at++)
		dense_pos(at, &in.dispensers[i].x, &in.dispensers[i].y,
				  &in.dispensers[i].z);
	for(int i = 0; i < SIGNS; i++, at++) {
		dense_pos(at, &in.signs[i].x, &in.signs[i].y, &in.signs[i].z);
		char line[16];
		snprintf(line, sizeof(line), "sign %d", i);
		set_sign_grid(in.signs[i].text, line, "", "", "");
	}
	for(int i = 0; i < SPAWNERS; i++, at++) {
		dense_pos(at, &in.spawners[i].x, &in.spawners[i].y, &in.spawners[i].z);
		strcpy(in.spawners[i].mob_id, "Zombie");
		in.spawners[i].delay = (int16_t)(20 + i);
	}
	in.n_note = NOTES;
	in.n_chest = CHESTS;
	in.n_record_player = RECORDS;
	in.n_furnace = FURNACES;
	in.n_dispenser = DISPENSERS;
	in.n_sign = SIGNS;
	in.n_spawner = SPAWNERS;

	void* arena = NULL;
	struct nbt_list* outer = tile_entities_build(&in, 0, -1, &arena);
	assert(outer && arena); /* the build no longer refuses >64 / >256 */
	nbt_node list = {.type = TAG_LIST, .payload.tag_list = outer};
	struct chunk_tile_entities out;
	assert(tile_entities_parse(&list, 0, -1, &out));
	assert(out.n_note == NOTES && out.n_chest == CHESTS
		   && out.n_record_player == RECORDS && out.n_furnace == FURNACES
		   && out.n_dispenser == DISPENSERS && out.n_sign == SIGNS
		   && out.n_spawner == SPAWNERS);
	for(int i = 0; i < NOTES; i++)
		assert(out.notes[i].x == in.notes[i].x && out.notes[i].y == in.notes[i].y
			   && out.notes[i].z == in.notes[i].z
			   && out.notes[i].note == in.notes[i].note);
	for(int i = 0; i < CHESTS; i++)
		assert(memcmp(&out.chests[i], &in.chests[i], sizeof(in.chests[i])) == 0);
	for(int i = 0; i < RECORDS; i++)
		assert(out.record_players[i].record == in.record_players[i].record
			   && out.record_players[i].x == in.record_players[i].x);
	for(int i = 0; i < FURNACES; i++)
		assert(out.furnaces[i].burn_time == in.furnaces[i].burn_time
			   && out.furnaces[i].cook_time == in.furnaces[i].cook_time
			   && out.furnaces[i].z == in.furnaces[i].z);
	for(int i = 0; i < SIGNS; i++)
		assert(strcmp(out.signs[i].text, in.signs[i].text) == 0
			   && out.signs[i].y == in.signs[i].y);
	for(int i = 0; i < SPAWNERS; i++)
		assert(out.spawners[i].delay == in.spawners[i].delay
			   && strcmp(out.spawners[i].mob_id, "Zombie") == 0);
	tile_entities_parse_free(&out);
	tile_entities_build_free(arena);
	tile_entities_parse_free(&in);
	printf("  dense chunk: %d Music + %d Chest + %d RecordPlayer + %d Furnace + "
		   "%d Trap + %d Sign + %d MobSpawner round-trip with no per-kind cap\n",
		   NOTES, CHESTS, RECORDS, FURNACES, DISPENSERS, SIGNS, SPAWNERS);
}


/* IMP-TE-CAPS: Dream Journal chunk (11,-3) holds four Beta "Furnace" records
 * on bedrock (y=0, empty Items). Beta's own load drops a tile entity whose
 * host block is not a BlockContainer [740c583 Chunk.setChunkBlockTileEntity],
 * and so does CaveWii's orphan sweep; the imported record must then not make
 * the lossless save refuse the chunk forever (it carried no items). A record
 * that still holds items keeps refusing: nothing may vanish without proof. */
static void test_preserve_empty_orphan_owner(void) {
	struct te_furnace f = {.x = 185, .y = 0, .z = -44};
	struct chunk_tile_entities orig_te = {.furnaces = &f, .n_furnace = 1};
	void* arena = NULL;
	struct nbt_list* orig = tile_entities_build(&orig_te, 11, -3, &arena);
	assert(orig && arena);
	nbt_node original = {.type = TAG_LIST, .payload.tag_list = orig};
	nbt_node compound_type = {.type = TAG_COMPOUND};
	struct nbt_list empty = {.data = &compound_type};
	INIT_LIST_HEAD(&empty.entry);
	nbt_node generated = {.type = TAG_LIST, .payload.tag_list = &empty};
	struct chunk_tile_entities current = {0};
	nbt_node* out = tile_entities_preserve(&original, &generated, &current,
										   false, 11, -3);
	assert(out && "an empty-inventory orphan never blocks the save");
	assert(tile_entity_at(out->payload.tag_list, 0) != NULL);
	nbt_free(out);
	tile_entities_build_free(arena);

	f.items[0] = (struct item_data) {.id = 4, .count = 3};
	arena = NULL;
	orig = tile_entities_build(&orig_te, 11, -3, &arena);
	assert(orig && arena);
	original.payload.tag_list = orig;
	assert(!tile_entities_preserve(&original, &generated, &current, false, 11, -3)
		   && "an orphan still holding items keeps the lossless refusal");
	tile_entities_build_free(arena);
	printf("  preserve: empty orphan Furnace retained, item-holding one refused\n");
}

int main(void) {
	test_four_line_sign_beta_golden();
	test_preserve_empty_orphan_owner();
	test_dense_chunk_no_per_kind_cap();
	test_spawner_ownership_and_rejection();
	// Input: 2 chests (sparse items, incl. slot 0 and the last slot) + 1 sign.
	struct te_chest chests[2];
	memset(chests, 0, sizeof(chests));
	chests[0].x = 10;
	chests[0].y = 64;
	chests[0].z = -5;
	chests[0].items[0] = (struct item_data) {.id = 265, .count = 3}; // iron x3
	chests[0].items[26]
		= (struct item_data) {.id = 331, .count = 64};	// redstone x64
	chests[1].x = 4;
	chests[1].y = 12;
	chests[1].z = -16;
	chests[1].items[53]
		= (struct item_data) {.id = 351, .count = 1, .durability = 3}; // cocoa dye

	struct te_sign signs[1];
	memset(signs, 0, sizeof(signs));
	signs[0].x = 1;
	signs[0].y = 70;
	signs[0].z = -15;
	// CR-04: 4 genuinely DISTINCT lines (not one line + 3 blanks) so this
	// round-trip actually exercises the Text1..Text4 split/rejoin, not just
	// a one-line self-roundtrip (the audit's original complaint).
	set_sign_grid(signs[0].text, "hello sign", "second line", "3rd: numbers!",
				 "-- the end --");

	// 1 furnace mid-smelt: input + fuel + a partial output stack, with non-zero
	// BurnTime/CookTime that must survive the short round-trip.
	struct te_furnace furnaces[1];
	memset(furnaces, 0, sizeof(furnaces));
	furnaces[0].x = 7;
	furnaces[0].y = 65;
	furnaces[0].z = -3;
	furnaces[0].items[0] = (struct item_data) {.id = 15, .count = 5};	// iron ore
	furnaces[0].items[1] = (struct item_data) {.id = 263, .count = 2}; // coal fuel
	furnaces[0].items[2]
		= (struct item_data) {.id = 265, .count = 1, .durability = 0}; // iron ingot
	furnaces[0].burn_time = 137;
	furnaces[0].cook_time = 88;

	// 1 in-flight piston moving block: the int fields + the FLOAT progress + the
	// extending boolean byte must survive the cNBT binary round-trip (incl. the
	// big-endian float swap on a Wii dump).
	struct te_piston_moving pistons[1];
	memset(pistons, 0, sizeof(pistons));
	pistons[0].x = 5;
	pistons[0].y = 70;
	pistons[0].z = -8;
	pistons[0].stored_id = 1;	// stone being pushed
	pistons[0].stored_meta = 4; // arbitrary metadata nibble
	pistons[0].facing = 3;		// +Z
	pistons[0].progress = 0.5F; // exactly representable -> bit-preserved round-trip
	pistons[0].extending = 1;

	// FH-17: 1 dungeon mob spawner, mid-countdown (a fresh, never-ticked one
	// would carry Delay=20; a nonzero/non-20 value proves the round-trip
	// carries whatever was actually persisted, not a hardcoded default).
	struct te_spawner spawners[1];
	memset(spawners, 0, sizeof(spawners));
	spawners[0].x = 2;
	spawners[0].y = 60;
	spawners[0].z = -1;
	strcpy(spawners[0].mob_id, "Spider");
	spawners[0].delay = 342;

	struct chunk_tile_entities in = {
		.chests = chests,
		.n_chest = 2,
		.signs = signs,
		.n_sign = 1,
		.furnaces = furnaces,
		.n_furnace = 1,
		.spawners = spawners,
		.n_spawner = 1,
		.piston_movings = pistons,
		.n_piston_moving = 1,
	};

	// Build -> wrap the sentinel as a TAG_LIST node -> parse.
	void* arena = NULL;
	struct nbt_list* sentinel = tile_entities_build(
		&in, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z, &arena);
	assert(sentinel && arena);
	nbt_node list = {.type = TAG_LIST,
					 .name = "TileEntities",
					 .payload.tag_list = sentinel};

	struct chunk_tile_entities out;
	assert(tile_entities_parse(
		&list, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z, &out));

	assert(out.n_chest == 2);
	assert(out.n_sign == 1);
	for(int c = 0; c < 2; c++) {
		assert(out.chests[c].x == chests[c].x);
		assert(out.chests[c].y == chests[c].y);
		assert(out.chests[c].z == chests[c].z);
		for(int s = 0; s < TE_CHEST_SLOTS; s++) {
			assert(out.chests[c].items[s].id == chests[c].items[s].id);
			assert(out.chests[c].items[s].count == chests[c].items[s].count);
			assert(out.chests[c].items[s].durability
				   == chests[c].items[s].durability);
		}
	}
	assert(out.signs[0].x == 1 && out.signs[0].y == 70
		   && out.signs[0].z == -15);
	// CR-04: writer(canonical grid) -> reader must reproduce the EXACT
	// canonical grid (trim-to-trailing-spaces then re-pad-with-spaces is
	// idempotent on already-canonical input), across all 4 lines, not just
	// line 0.
	assert(memcmp(out.signs[0].text, signs[0].text, TE_SIGN_SIZE) == 0);

	assert(out.n_furnace == 1);
	assert(out.furnaces[0].x == 7 && out.furnaces[0].y == 65
		   && out.furnaces[0].z == -3);
	assert(out.furnaces[0].burn_time == 137 && out.furnaces[0].cook_time == 88);
	for(int s = 0; s < TE_FURNACE_SLOTS; s++) {
		assert(out.furnaces[0].items[s].id == furnaces[0].items[s].id);
		assert(out.furnaces[0].items[s].count == furnaces[0].items[s].count);
		assert(out.furnaces[0].items[s].durability
			   == furnaces[0].items[s].durability);
	}

	assert(out.n_spawner == 1);
	assert(out.spawners[0].x == 2 && out.spawners[0].y == 60
		   && out.spawners[0].z == -1);
	assert(strcmp(out.spawners[0].mob_id, "Spider") == 0);
	assert(out.spawners[0].delay == 342);

	assert(out.n_piston_moving == 1);
	assert(out.piston_movings[0].x == 5 && out.piston_movings[0].y == 70
		   && out.piston_movings[0].z == -8);
	assert(out.piston_movings[0].stored_id == 1
		   && out.piston_movings[0].stored_meta == 4
		   && out.piston_movings[0].facing == 3);
	assert(out.piston_movings[0].progress == 0.5F);
	assert(out.piston_movings[0].extending == 1);

	// A later allocation failure must release the earlier successful allocation
	// and publish no partial arrays or counts.
	struct chunk_tile_entities oom_out;
	tile_entities_nbt_test_fail_allocation_after(1);
	assert(!tile_entities_parse(
		&list, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z, &oom_out));
	assert_empty(&oom_out);

	// Once a supported TE/item is identified, wrong-typed vanilla-required
	// fields reject the complete parse rather than becoming coordinate/item 0.
	struct nbt_list* first_te
		= list_entry(sentinel->entry.flink, struct nbt_list, entry);
	nbt_node* x_field = direct_child(first_te->data, "x");
	nbt_type saved_type = x_field->type;
	x_field->type = TAG_SHORT;
	assert_parse_rejected(&list, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z);
	x_field->type = saved_type;

	nbt_node* items_field = direct_child(first_te->data, "Items");
	saved_type = items_field->type;
	items_field->type = TAG_INT;
	struct chunk_tile_entities pinned_shape;
	assert(tile_entities_parse(&list,ROUNDTRIP_CHUNK_X,ROUNDTRIP_CHUNK_Z,&pinned_shape));
	for(size_t k=0;k<TE_CHEST_SLOTS;k++) assert(item_data_pinned(&pinned_shape.chests[0].items[k]));
	tile_entities_parse_free(&pinned_shape);
	items_field->type = saved_type;
	struct nbt_list* first_item = list_entry(
		items_field->payload.tag_list->entry.flink, struct nbt_list, entry);
	nbt_node* item_id = direct_child(first_item->data, "id");
	saved_type = item_id->type;
	item_id->type = TAG_INT;
	struct chunk_tile_entities pinned;
	assert(tile_entities_parse(&list, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z, &pinned));
	assert(item_data_pinned(&pinned.chests[0].items[0]));
	tile_entities_parse_free(&pinned);
	item_id->type = saved_type;

	struct nbt_list* sign_link = first_te;
	sign_link = list_entry(sign_link->entry.flink, struct nbt_list, entry);
	sign_link = list_entry(sign_link->entry.flink, struct nbt_list, entry);
	nbt_node* text4 = direct_child(sign_link->data, "Text4");
	saved_type = text4->type;
	text4->type = TAG_BYTE;
	assert_parse_rejected(&list, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z);
	text4->type = saved_type;

	tile_entities_parse_free(&out);

	// Full cNBT serialize round-trip: this is the path region_archive takes --
	// wrap the same TileEntities node in a root compound, dump to the Beta binary
	// NBT, parse it back, and re-extract. Unlike the in-memory parse above, this
	// drives cNBT's *serializer*, which writes the list element type from the
	// sentinel's type node; a bug there would corrupt the chunk record without
	// ever tripping the in-memory check.
	struct nbt_list root_sentinel = {.data = NULL};
	INIT_LIST_HEAD(&root_sentinel.entry);
	struct nbt_list te_wrap = {.data = &list}; // reuse the TileEntities node
	list_add_tail(&te_wrap.entry, &root_sentinel.entry);
	nbt_node root = {.type = TAG_COMPOUND,
					 .name = "",
					 .payload.tag_compound = &root_sentinel};

	struct buffer buf = nbt_dump_binary(&root);
	assert(buf.data && buf.len > 0);

	nbt_node* reparsed = nbt_parse(buf.data, buf.len);
	assert(reparsed);
	nbt_node* te2 = nbt_find_by_path(reparsed, ".TileEntities");
	assert(te2 && te2->type == TAG_LIST);

	struct chunk_tile_entities out2;
	assert(tile_entities_parse(
		te2, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z, &out2));
	assert(out2.n_chest == 2 && out2.n_sign == 1 && out2.n_furnace == 1);
	assert(out2.chests[0].x == 10 && out2.chests[0].y == 64
		   && out2.chests[0].z == -5);
	assert(out2.chests[0].items[0].id == 265
		   && out2.chests[0].items[0].count == 3);
	assert(out2.chests[0].items[26].id == 331
		   && out2.chests[0].items[26].count == 64);
	assert(out2.chests[1].x == 4 && out2.chests[1].y == 12
		   && out2.chests[1].z == -16);
	assert(out2.chests[1].items[53].id == 351
		   && out2.chests[1].items[53].count == 1
		   && out2.chests[1].items[53].durability == 3);
	assert(out2.signs[0].x == 1 && out2.signs[0].y == 70
		   && out2.signs[0].z == -15);
	// Full cNBT binary round-trip must reproduce the same canonical grid too
	// (proves the FOUR separate Text1..Text4 tags, not just one, survive an
	// actual on-disk-format dump/parse -- the Wii save path).
	assert(memcmp(out2.signs[0].text, signs[0].text, TE_SIGN_SIZE) == 0);
	assert(out2.furnaces[0].x == 7 && out2.furnaces[0].y == 65
		   && out2.furnaces[0].z == -3);
	assert(out2.furnaces[0].burn_time == 137 && out2.furnaces[0].cook_time == 88);
	assert(out2.furnaces[0].items[0].id == 15 && out2.furnaces[0].items[0].count == 5);
	assert(out2.furnaces[0].items[1].id == 263
		   && out2.furnaces[0].items[1].count == 2);
	assert(out2.furnaces[0].items[2].id == 265
		   && out2.furnaces[0].items[2].count == 1);
	// The piston survived the FULL cNBT binary serialize+parse (the Wii save path):
	// the float progress is bit-preserved through the big-endian dump/load.
	assert(out2.n_piston_moving == 1);
	assert(out2.piston_movings[0].x == 5 && out2.piston_movings[0].y == 70
		   && out2.piston_movings[0].z == -8);
	assert(out2.piston_movings[0].stored_id == 1
		   && out2.piston_movings[0].stored_meta == 4
		   && out2.piston_movings[0].facing == 3);
	assert(out2.piston_movings[0].progress == 0.5F);
	assert(out2.piston_movings[0].extending == 1);

	tile_entities_parse_free(&out2);
	nbt_free(reparsed);
	buffer_free(&buf);

	tile_entities_build_free(arena);

	// Empty input -> NULL build (caller keeps the empty list, no arena).
	struct chunk_tile_entities empty = {0};
	void* arena2 = (void*)0x1;
	assert(tile_entities_build(&empty, 0, 0, &arena2) == NULL);
	assert(arena2 == NULL);

	// Invalid/nonrepresentable non-empty input must fail instead of letting a
	// caller serialize an empty replacement list.
	struct chunk_tile_entities too_many = {
		.chests = chests,
		.n_chest = TE_RECORDS_EXAMINED_MAX + 1, /* over the total bound */
	};
	void* invalid_arena = (void*)0x1;
	assert(tile_entities_build(&too_many, 0, 0, &invalid_arena) == NULL);
	assert(invalid_arena == NULL);
	tile_entities_nbt_test_fail_allocation_after(0);
	assert(tile_entities_build(
		&in, ROUNDTRIP_CHUNK_X, ROUNDTRIP_CHUNK_Z, &invalid_arena)
		   == NULL);
	assert(invalid_arena == NULL);
	struct te_sign unterminated_sign = {0};
	memset(unterminated_sign.text, 'x', sizeof(unterminated_sign.text));
	struct chunk_tile_entities bad_sign = {
		.signs = &unterminated_sign,
		.n_sign = 1,
	};
	assert(tile_entities_build(&bad_sign, 0, 0, &invalid_arena) == NULL);
	assert(invalid_arena == NULL);

	// Empty-Items element-type byte: a furnace with NO items serializes an EMPTY
	// Items TAG_LIST. Beta NBTTagList.writeTagContents writes element-type byte 1
	// (TAG_BYTE) for any empty list; cNBT would otherwise emit the sentinel's
	// element type (TAG_COMPOUND, 10). Build one empty furnace, dump, and scan the
	// binary for the Items list header [09 00 05 "Items" <type> <len32>], asserting
	// <type> == 1 and length == 0 (regression pin for the byte-faithfulness fix).
	{
		struct te_furnace ef[1];
		memset(ef, 0, sizeof(ef)); // all 3 slots empty, BurnTime/CookTime 0
		ef[0].x = 2;
		ef[0].y = 60;
		ef[0].z = 9;
		struct chunk_tile_entities ein = {
			.furnaces = ef,
			.n_furnace = 1,
		};

		void* earena = NULL;
		struct nbt_list* esent = tile_entities_build(&ein, 0, 0, &earena);
		assert(esent && earena);
		nbt_node elist = {.type = TAG_LIST,
						  .name = "TileEntities",
						  .payload.tag_list = esent};
		struct nbt_list eroot_sentinel = {.data = NULL};
		INIT_LIST_HEAD(&eroot_sentinel.entry);
		struct nbt_list ewrap = {.data = &elist};
		list_add_tail(&ewrap.entry, &eroot_sentinel.entry);
		nbt_node eroot = {.type = TAG_COMPOUND,
						  .name = "",
						  .payload.tag_compound = &eroot_sentinel};

		struct buffer ebuf = nbt_dump_binary(&eroot);
		assert(ebuf.data && ebuf.len > 0);

		// Locate the Items list header: TAG_LIST(0x09) + name-len(0x0005) + "Items".
		static const uint8_t needle[8]
			= {0x09, 0x00, 0x05, 'I', 't', 'e', 'm', 's'};
		int at = -1;
		for(size_t i = 0; i + sizeof(needle) < ebuf.len; i++) {
			if(memcmp((const uint8_t*)ebuf.data + i, needle, sizeof(needle)) == 0) {
				at = (int)i;
				break;
			}
		}
		assert(at >= 0); // the Items tag must be present (Beta always writes Items)
		const uint8_t* p = (const uint8_t*)ebuf.data + at + sizeof(needle);
		assert(p[0] == 1); // element-type byte == TAG_BYTE (Beta), NOT 10 (compound)
		// big-endian 4-byte length, must be 0 for the empty list
		assert(p[1] == 0 && p[2] == 0 && p[3] == 0 && p[4] == 0);

		buffer_free(&ebuf);
		tile_entities_build_free(earena);
	}

	test_supported_record_bound();
	test_item_record_bound();
	test_chunk_ownership_and_piston_ranges();
	test_more_than_64_pistons_binary_round_trip();

	printf("test_tile_entities_nbt: in-memory + full cNBT serialize round-trip OK "
		   "(2 chests, 3 items, 1 sign, 1 furnace w/ burn+cook, 1 spawner; "
		   "empty-Items type==1)\n");
	return 0;
}
