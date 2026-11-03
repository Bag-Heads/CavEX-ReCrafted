/* Full Level.Entities .mcr round trip for every runtime entity kind. */

#include <assert.h>
#include <math.h>
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
#include "entity/combat.h"
#include "entity/monsters_object.h"
#include "item/item_nbt.h"
#include "network/entity_archive.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "platform/mem2.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_region_entities_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE (CHUNK_SIZE * CHUNK_SIZE)

static int feq(float a, float b) {
	return fabsf(a - b) < 1.0e-5f;
}

static nbt_node* child_named(nbt_node* comp, const char* name) {
	assert(comp && comp->type == TAG_COMPOUND);
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* child = list_entry(pos, struct nbt_list, entry)->data;
		if(child && child->name && strcmp(child->name, name) == 0)
			return child;
	}
	return NULL;
}

static size_t child_named_count(nbt_node* comp, const char* name) {
	assert(comp && comp->type == TAG_COMPOUND);
	size_t count = 0;
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* child = list_entry(pos, struct nbt_list, entry)->data;
		if(child && child->name && strcmp(child->name, name) == 0)
			count++;
	}
	return count;
}

static nbt_node* list_compound_at(struct nbt_list* list, size_t index) {
	size_t current = 0;
	struct list_head* pos;
	list_for_each(pos, &list->entry) {
		if(current++ == index)
			return list_entry(pos, struct nbt_list, entry)->data;
	}
	return NULL;
}

static size_t nbt_list_count(const nbt_node* list) {
	assert(list && list->type == TAG_LIST && list->payload.tag_list);
	size_t count = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry)
		count++;
	return count;
}

static bool beta_common_name(const char* name) {
	return !strcmp(name, "id") || !strcmp(name, "Pos")
		|| !strcmp(name, "Motion") || !strcmp(name, "Rotation")
		|| !strcmp(name, "FallDistance") || !strcmp(name, "Fire")
		|| !strcmp(name, "Air") || !strcmp(name, "OnGround");
}

/* Convert this codec's record into the exact pure-Beta shapes present in the
 * recovered world. The arena still owns removed links/nodes; this test only
 * changes which links are reachable from the compound. */
static void make_beta_record(nbt_node* compound, bool item) {
	assert(compound && compound->type == TAG_COMPOUND);
	const char* id = child_named(compound, "id")->payload.tag_string;
	/* java-load-1023: the codec now writes Beta's own sheep "Color"/"Sheared"
	 * beside the CaveWii* copies; the keep-filter below drops the copies. */
	if(!item && !strcmp(id, "Sheep"))
		assert(child_named(compound, "Color")
			&& child_named(compound, "Sheared"));
	/* Reuse a CaveWii-only scalar as Beta Entity.Air before dropping the other
	 * extensions, avoiding test-only heap ownership beside the builder arena. */
	nbt_node* air = child_named(compound, "Air");
	if(!air)
		air = child_named(compound, "CaveWiiType");
	assert(air);
	air->name = "Air";
	air->type = TAG_SHORT;
	air->payload.tag_short = 300;

	struct list_head *pos, *next;
	list_for_each_safe(pos, next, &compound->payload.tag_compound->entry) {
		nbt_node* child = list_entry(pos, struct nbt_list, entry)->data;
		bool keep = child && child->name && beta_common_name(child->name);
		if(item)
			keep = keep || !strcmp(child->name, "Health")
				|| !strcmp(child->name, "Age")
				|| !strcmp(child->name, "Item");
		else
			keep = keep || !strcmp(child->name, "Health")
				|| !strcmp(child->name, "HurtTime")
				|| !strcmp(child->name, "DeathTime")
				|| !strcmp(child->name, "AttackTime")
				|| (!strcmp(id, "Pig") && !strcmp(child->name, "Saddle"))
				|| (!strcmp(id, "Sheep")
					&& (!strcmp(child->name, "Color")
						|| !strcmp(child->name, "Sheared")))
				|| (!strcmp(id, "Wolf")
					&& (!strcmp(child->name, "Angry")
						|| !strcmp(child->name, "Sitting")
						|| !strcmp(child->name, "Owner")));
		if(!keep)
			list_del(pos);
	}

	child_named(compound, "Fire")->payload.tag_short = -1;
	if(item) {
		nbt_node* age = child_named(compound, "Age");
		assert(age);
		age->type = TAG_SHORT;
		age->payload.tag_short = 101;
	} else {
		const char* names[] = {"HurtTime", "DeathTime", "AttackTime"};
		for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			nbt_node* timer = child_named(compound, names[i]);
			assert(timer);
			timer->type = TAG_SHORT;
		}
	}
}

static void test_beta_supported_species_set(void) {
	const int ids[] = {
		MONSTER_SPIDER, MONSTER_COW, MONSTER_SQUID,
		MONSTER_PIG, MONSTER_SHEEP, MONSTER_WOLF,
	};
	struct archived_entity old[6] = {0}, current[6] = {0};
	for(size_t i = 0; i < 6; i++) {
		old[i].kind = ARCHIVED_ENTITY_MONSTER;
		old[i].source_id = (uint32_t)i + 1;
		old[i].health = ids[i] == MONSTER_WOLF ? 8 : 10;
		old[i].fire = -1;
		old[i].air = 300;
		glm_vec3_copy((vec3) {(float)i + 1.25f, 65.0f, 8.25f}, old[i].pos);
		old[i].data.monster = (struct archived_monster_state) {
			.monster_id = ids[i], .fire_time = -1, .air = 300, .fuse = -1,
		};
	}
	old[3].data.monster.saddled = true;
	old[4].data.monster.fleece_color = 12;
	old[4].data.monster.sheared = true;
	old[5].data.monster.wolf_angry = true;
	old[5].data.monster.wolf_sitting = false;
	strcpy(old[5].data.monster.wolf_owner, "BetaOwner");
	memcpy(current, old, sizeof(current));
	for(size_t i = 0; i < 6; i++) {
		current[i].source_id += 100;
		current[i].pos[2] += 0.5f;
	}
	void *old_arena = NULL, *new_arena = NULL;
	struct nbt_list* old_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = old, .count = 6}, &old_arena);
	struct nbt_list* new_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = current, .count = 6}, &new_arena);
	assert(old_list && new_list);
	for(size_t i = 0; i < 6; i++)
		make_beta_record(list_compound_at(old_list, i), false);
	nbt_node old_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = old_list,
	};
	nbt_node new_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = new_list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&old_node, 0, 0, &parsed));
	assert(parsed.count == 6);
	assert(parsed.entities[3].data.monster.saddled);
	assert(parsed.entities[4].data.monster.fleece_color == 12);
	assert(parsed.entities[4].data.monster.sheared);
	assert(parsed.entities[5].data.monster.wolf_angry);
	assert(!parsed.entities[5].data.monster.wolf_sitting);
	assert(!strcmp(parsed.entities[5].data.monster.wolf_owner, "BetaOwner"));
	chunk_entities_free(&parsed);
	struct chunk_entities snapshot = {.entities = current, .count = 6};
	nbt_node* merged = chunk_entities_preserve(
		&old_node, &new_node, &snapshot, false, 0, 0);
	assert(merged && item_nbt_equal(merged, &new_node));
	nbt_free(merged);
	void* stationary_arena = NULL;
	struct nbt_list* stationary_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = old, .count = 6},
		&stationary_arena);
	assert(stationary_list);
	nbt_node stationary_node = {
		.type = TAG_LIST, .name = "Entities",
		.payload.tag_list = stationary_list,
	};
	struct chunk_entities stationary = {.entities = old, .count = 6};

	/* Every species-specific scalar is part of the exact-owned certificate.
	 * Invalid values must abort the recovery-cohort pre-scan, even at unchanged
	 * positions, instead of normalizing state and deleting the originals. */
	nbt_node* saddle = child_named(list_compound_at(old_list, 3), "Saddle");
	assert(saddle);
	saddle->payload.tag_byte = 2;
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	/* Arbitrary, partial, or malformed CaveWii markers are not provenance and
	 * cannot let an invalid Beta scalar reach the normalizing Pos rewrite. */
	nbt_node marker = {
		.type = TAG_INT, .name = "CaveWiiFoo", .payload.tag_int = 1,
	};
	struct nbt_list marker_wrap = {.data = &marker};
	INIT_LIST_HEAD(&marker_wrap.entry);
	list_add_tail(&marker_wrap.entry,
		&list_compound_at(old_list, 3)->payload.tag_compound->entry);
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &snapshot, false, 0, 0));
	list_del(&marker_wrap.entry);

	marker.name = "CaveWiiRuntimeId";
	marker.type = TAG_LONG;
	marker.payload.tag_long = 1234;
	list_add_tail(&marker_wrap.entry,
		&list_compound_at(old_list, 3)->payload.tag_compound->entry);
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	list_del(&marker_wrap.entry);

	nbt_node runtime_marker = {
		.type = TAG_LONG, .name = "CaveWiiRuntimeId", .payload.tag_long = 1234,
	};
	struct nbt_list runtime_wrap = {.data = &runtime_marker};
	INIT_LIST_HEAD(&runtime_wrap.entry);
	marker.name = "CaveWiiType";
	marker.type = TAG_INT;
	marker.payload.tag_int = ARCHIVED_ENTITY_ITEM; /* conflicts with id=Pig */
	list_add_tail(&marker_wrap.entry,
		&list_compound_at(old_list, 3)->payload.tag_compound->entry);
	list_add_tail(&runtime_wrap.entry,
		&list_compound_at(old_list, 3)->payload.tag_compound->entry);
	/* The conflicting type makes this supported-ID row parse-rejected, so it
	 * never entered runtime. It remains byte-exact and the current snapshot
	 * contains only the five rows that were actually admitted. */
	struct archived_entity admitted[5];
	for(size_t src = 0, dst = 0; src < 6; src++)
		if(src != 3)
			admitted[dst++] = old[src];
	void* admitted_arena = NULL;
	struct nbt_list* admitted_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = admitted, .count = 5},
		&admitted_arena);
	assert(admitted_list);
	nbt_node admitted_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = admitted_list,
	};
	nbt_node* opaque_exact = item_nbt_clone_lossless(
		list_compound_at(old_list, 3));
	assert(opaque_exact);
	struct chunk_entities admitted_snapshot = {
		.entities = admitted, .count = 5,
	};
	merged = chunk_entities_preserve(
		&old_node, &admitted_node, &admitted_snapshot, false, 0, 0);
	assert(merged && nbt_list_count(merged) == 6);
	assert(item_nbt_equal(
		list_compound_at(merged->payload.tag_list, 0), opaque_exact));
	nbt_free(opaque_exact);
	nbt_free(merged);
	chunk_entities_build_free(admitted_arena);
	list_del(&runtime_wrap.entry);
	list_del(&marker_wrap.entry);

	runtime_marker.type = TAG_STRING;
	runtime_marker.payload.tag_string = "1234";
	list_add_tail(&runtime_wrap.entry,
		&list_compound_at(old_list, 3)->payload.tag_compound->entry);
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	list_del(&runtime_wrap.entry);
	saddle->payload.tag_byte = 1;

	nbt_node* color = child_named(list_compound_at(old_list, 4), "Color");
	nbt_node* sheared = child_named(list_compound_at(old_list, 4), "Sheared");
	assert(color && sheared);
	color->payload.tag_byte = 16;
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	color->payload.tag_byte = 12;
	sheared->payload.tag_byte = 2;
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	sheared->payload.tag_byte = 1;

	nbt_node* angry = child_named(list_compound_at(old_list, 5), "Angry");
	nbt_node* sitting = child_named(list_compound_at(old_list, 5), "Sitting");
	nbt_node* owner = child_named(list_compound_at(old_list, 5), "Owner");
	assert(angry && sitting && owner);
	angry->payload.tag_byte = 2;
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	angry->payload.tag_byte = 1;
	sitting->payload.tag_byte = 2;
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	sitting->payload.tag_byte = 0;
	char* saved_owner = owner->payload.tag_string;
	owner->payload.tag_string =
		"owner-name-that-does-not-fit-the-runtime-wolf-owner-buffer";
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	owner->payload.tag_string = saved_owner;

	/* A duplicate species tag is not an exact schema even when both values are
	 * individually valid. */
	nbt_node duplicate_saddle = {
		.type = TAG_BYTE, .name = "Saddle", .payload.tag_byte = 1,
	};
	struct nbt_list duplicate_wrap = {.data = &duplicate_saddle};
	INIT_LIST_HEAD(&duplicate_wrap.entry);
	list_add_tail(&duplicate_wrap.entry,
		&list_compound_at(old_list, 3)->payload.tag_compound->entry);
	assert(!chunk_entities_preserve(
		&old_node, &stationary_node, &stationary, false, 0, 0));
	list_del(&duplicate_wrap.entry);
	chunk_entities_build_free(stationary_arena);
	chunk_entities_build_free(new_arena);
	chunk_entities_build_free(old_arena);
}

static void make_beta_arrow(nbt_node* compound) {
	assert(compound && compound->type == TAG_COMPOUND);
	struct {
		const char* from;
		const char* to;
		nbt_type type;
		int value;
	} fields[] = {
		{"CaveWiiType", "Air", TAG_SHORT, 300},
		{"xTile", "xTile", TAG_SHORT, 3},
		{"yTile", "yTile", TAG_SHORT, 64},
		{"zTile", "zTile", TAG_SHORT, 4},
		{"inTile", "inTile", TAG_BYTE, 1},
		{"inData", "inData", TAG_BYTE, 0},
		{"shake", "shake", TAG_BYTE, 5},
		{"player", "player", TAG_BYTE, 0},
	};
	for(size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		nbt_node* n = child_named(compound, fields[i].from);
		assert(n);
		n->name = (char*)fields[i].to;
		n->type = fields[i].type;
		if(fields[i].type == TAG_BYTE)
			n->payload.tag_byte = (int8_t)fields[i].value;
		else
			n->payload.tag_short = (int16_t)fields[i].value;
	}
	const char* keep[] = {
		"id", "Pos", "Motion", "Rotation", "FallDistance", "Fire", "Air",
		"OnGround", "xTile", "yTile", "zTile", "inTile", "inData", "shake",
		"inGround", "player",
	};
	struct list_head *pos, *next;
	list_for_each_safe(pos, next, &compound->payload.tag_compound->entry) {
		nbt_node* n = list_entry(pos, struct nbt_list, entry)->data;
		bool retain = false;
		for(size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
			retain = retain || (n && n->name && !strcmp(n->name, keep[i]));
		if(!retain)
			list_del(pos);
	}
	child_named(compound, "Fire")->payload.tag_short = 0;
}

static void test_beta_owned_set_reconciliation(void) {
	/* Two identical-position imported items cannot be paired by Pos. The
	 * authoritative current set moved both; preservation must remove the whole
	 * proven-owned old set and append the current set exactly once. */
	struct archived_entity old_records[2] = {0};
	for(size_t i = 0; i < 2; i++) {
		old_records[i].kind = ARCHIVED_ENTITY_ITEM;
		old_records[i].source_id = (uint32_t)i + 1;
		glm_vec3_copy((vec3) {4.25f, 65.0f, 4.25f}, old_records[i].pos);
		old_records[i].fire = -1;
		old_records[i].health = 5;
		old_records[i].data.item.item
			= (struct item_data) {.id = 287, .count = 1};
		old_records[i].data.item.age = 101;
	}
	struct archived_entity current_records[2];
	memcpy(current_records, old_records, sizeof(current_records));
	current_records[0].source_id = 101;
	current_records[1].source_id = 102;
	current_records[0].pos[0] = 5.0f;
	current_records[1].pos[0] = 5.5f;

	void *old_arena = NULL, *new_arena = NULL;
	struct nbt_list* old_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = old_records, .count = 2},
		&old_arena);
	struct nbt_list* new_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = current_records, .count = 2},
		&new_arena);
	assert(old_list && new_list);
	make_beta_record(list_compound_at(old_list, 0), true);
	make_beta_record(list_compound_at(old_list, 1), true);
	nbt_node old_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = old_list,
	};
	nbt_node new_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = new_list,
	};
	struct chunk_entities current = {
		.entities = current_records, .count = 2,
	};
	nbt_node* merged = chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0);
	assert(merged && nbt_list_count(merged) == 2);
	assert(item_nbt_equal(merged, &new_node));
	nbt_free(merged);

	/* Pickup/death and a cross-chunk departure are the same source-chunk set
	 * transition: an empty complete snapshot removes the owned old rows. */
	struct nbt_list empty_list = {.data = &(nbt_node) {.type = TAG_COMPOUND}};
	INIT_LIST_HEAD(&empty_list.entry);
	nbt_node empty_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = &empty_list,
	};
	struct chunk_entities empty = {0};
	merged = chunk_entities_preserve(
		&old_node, &empty_node, &empty, false, 0, 0);
	assert(merged && nbt_list_count(merged) == 0);
	nbt_free(merged);

	chunk_entities_build_free(new_arena);
	chunk_entities_build_free(old_arena);
}

static void test_beta_mob_fire_and_unowned_fail_closed(void) {
	struct archived_entity old_mob = {0};
	old_mob.kind = ARCHIVED_ENTITY_MONSTER;
	old_mob.source_id = 7;
	old_mob.health = 18;
	old_mob.fire = -1;
	glm_vec3_copy((vec3) {3.25f, 65.0f, 3.25f}, old_mob.pos);
	old_mob.data.monster = (struct archived_monster_state) {
		.monster_id = MONSTER_ZOMBIE,
		.fire_time = -1,
		.fuse = -1,
		.air = 300,
	};
	struct archived_entity current_mob = old_mob;
	current_mob.source_id = 77;
	current_mob.pos[0] = 4.0f;

	void *old_arena = NULL, *new_arena = NULL;
	struct nbt_list* old_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &old_mob, .count = 1},
		&old_arena);
	struct nbt_list* new_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &current_mob, .count = 1},
		&new_arena);
	assert(old_list && new_list);
	make_beta_record(list_compound_at(old_list, 0), false);
	nbt_node old_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = old_list,
	};
	nbt_node new_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = new_list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&old_node, 0, 0, &parsed));
	assert(parsed.count == 1);
	/* Pure Beta has no CaveWiiFireTime. Its common signed Fire counter is the
	 * authoritative monster counter and must seed the runtime mirror. */
	assert(parsed.entities[0].data.monster.fire_time == -1);
	chunk_entities_free(&parsed);
	struct chunk_entities current = {
		.entities = &current_mob, .count = 1,
	};
	nbt_node* merged = chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0);
	assert(merged && item_nbt_equal(merged, &new_node));
	nbt_free(merged);
	/* A moved parse-admitted mob with an unknown field is equally unsafe: the
	 * preserving fallback must refuse rather than retain old + append current. */
	nbt_node mob_unknown = {
		.type = TAG_INT, .name = "ModState", .payload.tag_int = 77,
	};
	struct nbt_list mob_unknown_wrap = {.data = &mob_unknown};
	INIT_LIST_HEAD(&mob_unknown_wrap.entry);
	list_add_tail(&mob_unknown_wrap.entry,
		&list_compound_at(old_list, 0)->payload.tag_compound->entry);
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	list_del(&mob_unknown_wrap.entry);
	const char* required_timers[] = {"HurtTime", "DeathTime", "AttackTime"};
	for(size_t i = 0; i < sizeof(required_timers) / sizeof(required_timers[0]); i++) {
		struct nbt_list* removed = NULL;
		struct list_head* pos;
		list_for_each(pos,
			&list_compound_at(old_list, 0)->payload.tag_compound->entry) {
			struct nbt_list* wrap = list_entry(pos, struct nbt_list, entry);
			if(wrap->data && wrap->data->name
			   && !strcmp(wrap->data->name, required_timers[i])) {
				removed = wrap;
				break;
			}
		}
		assert(removed);
		list_del(&removed->entry);
		assert(!chunk_entities_preserve(
			&old_node, &new_node, &current, false, 0, 0));
		list_add_tail(&removed->entry,
			&list_compound_at(old_list, 0)->payload.tag_compound->entry);
	}
	chunk_entities_build_free(new_arena);
	chunk_entities_build_free(old_arena);

	/* A parseable known row with an unknown outer field is not owned. Once it
	 * moves and loses its unique preserving match, fail the checkpoint instead
	 * of keeping the old ghost and appending a second live copy. */
	struct archived_entity old_item = {0};
	old_item.kind = ARCHIVED_ENTITY_ITEM;
	old_item.source_id = 8;
	old_item.health = 5;
	old_item.fire = -1;
	glm_vec3_copy((vec3) {6.25f, 65.0f, 6.25f}, old_item.pos);
	old_item.data.item.item
		= (struct item_data) {.id = 289, .count = 1};
	old_item.data.item.age = 250;
	struct archived_entity moved_item = old_item;
	moved_item.source_id = 88;
	moved_item.pos[0] = 7.0f;
	old_arena = new_arena = NULL;
	old_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &old_item, .count = 1},
		&old_arena);
	new_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &moved_item, .count = 1},
		&new_arena);
	assert(old_list && new_list);
	nbt_node* old_compound = list_compound_at(old_list, 0);
	make_beta_record(old_compound, true);
	nbt_node unknown = {
		.type = TAG_INT, .name = "ModState", .payload.tag_int = 1234,
	};
	struct nbt_list unknown_wrap = {.data = &unknown};
	INIT_LIST_HEAD(&unknown_wrap.entry);
	list_add_tail(&unknown_wrap.entry,
		&old_compound->payload.tag_compound->entry);
	old_node.payload.tag_list = old_list;
	new_node.payload.tag_list = new_list;
	current = (struct chunk_entities) {
		.entities = &moved_item, .count = 1,
	};
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	/* Duplicate known names and unknown nested Item payload bytes are also
	 * outside the exact-owned schema and remain fail-closed after movement. */
	unknown.name = "Age";
	unknown.type = TAG_SHORT;
	unknown.payload.tag_short = 250;
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	list_del(&unknown_wrap.entry);
	unknown.name = "ModItemState";
	unknown.type = TAG_INT;
	unknown.payload.tag_int = 9;
	list_add_tail(&unknown_wrap.entry,
		&child_named(old_compound, "Item")->payload.tag_compound->entry);
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	list_del(&unknown_wrap.entry);
	nbt_node* age = child_named(old_compound, "Age");
	age->payload.tag_short = -1;
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	age->payload.tag_short = 250;
	nbt_node* health = child_named(old_compound, "Health");
	health->payload.tag_short = 0;
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	health->payload.tag_short = 5;
	nbt_node* on_ground = child_named(old_compound, "OnGround");
	on_ground->payload.tag_byte = 2;
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	on_ground->payload.tag_byte = 0;
	nbt_node* fall = child_named(old_compound, "FallDistance");
	fall->payload.tag_float = NAN;
	assert(!chunk_entities_preserve(
		&old_node, &new_node, &current, false, 0, 0));
	chunk_entities_build_free(new_arena);
	chunk_entities_build_free(old_arena);
}

static void test_beta_mixed_opaque_arrow_and_native_item_compat(void) {
	struct archived_entity imported[3] = {0};
	imported[0].kind = ARCHIVED_ENTITY_ITEM;
	imported[0].source_id = 1;
	imported[0].health = 3;
	imported[0].fire = -1;
	imported[0].air = 300;
	glm_vec3_copy((vec3) {2.25f, 65.0f, 2.25f}, imported[0].pos);
	imported[0].data.item.item
		= (struct item_data) {.id = 262, .count = 1};
	imported[0].data.item.age = 3670;
	imported[1].kind = ARCHIVED_ENTITY_MONSTER;
	imported[1].source_id = 2;
	imported[1].health = 18;
	imported[1].fire = -1;
	imported[1].air = 300;
	glm_vec3_copy((vec3) {3.25f, 65.0f, 3.25f}, imported[1].pos);
	imported[1].data.monster = (struct archived_monster_state) {
		.monster_id = MONSTER_CREEPER, .fire_time = -1, .air = 300,
		.fuse = -1,
	};
	imported[2].kind = ARCHIVED_ENTITY_ARROW;
	imported[2].source_id = 3;
	glm_vec3_copy((vec3) {4.25f, 65.0f, 4.25f}, imported[2].pos);
	imported[2].data.arrow.item_id = 262;
	imported[2].data.arrow.in_ground = true;

	struct archived_entity current[2] = {imported[0], imported[1]};
	current[0].source_id = 101;
	current[1].source_id = 102;
	current[0].pos[0] = 2.75f;
	current[1].pos[0] = 3.75f;
	void *old_arena = NULL, *new_arena = NULL;
	struct nbt_list* old_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = imported, .count = 3},
		&old_arena);
	struct nbt_list* new_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = current, .count = 2},
		&new_arena);
	assert(old_list && new_list);
	make_beta_record(list_compound_at(old_list, 0), true);
	make_beta_record(list_compound_at(old_list, 1), false);
	make_beta_arrow(list_compound_at(old_list, 2));
	nbt_node old_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = old_list,
	};
	nbt_node new_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = new_list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&old_node, 0, 0, &parsed));
	/* ci-1101/41 item 12: an exact pure-Beta Arrow record (no CaveWii tags)
	 * is now admitted with Beta's reload state: EntityArrow.readEntityFromNBT
	 * (EntityArrow.java:246-254) restores only the tile/shake/inGround/player
	 * fields, shootingEntity and ticksInAir/ticksInGround are not persisted
	 * (null owner, 0), and the damage/item are the bow arrow's. */
	assert(parsed.count == 3);
	{
		const struct archived_arrow_state* a = &parsed.entities[2].data.arrow;
		assert(parsed.entities[2].kind == ARCHIVED_ENTITY_ARROW);
		assert(a->item_id == 262 && a->damage == ARROW_DAMAGE);
		assert(a->owner_source_id == UINT32_MAX && !a->from_skeleton);
		assert(!a->from_player && a->shake_ticks == 5 && a->in_ground);
		assert(a->age == 0 && a->stuck_ticks == 0);
		assert(a->stuck_block_x == 3 && a->stuck_block_y == 64
			   && a->stuck_block_z == 4 && a->stuck_block_type == 1
			   && a->stuck_block_meta == 0);
	}
	chunk_entities_free(&parsed);
	nbt_node* arrow_exact = item_nbt_clone_lossless(list_compound_at(old_list, 2));
	assert(arrow_exact);
	/* A runtime that DROPPED the arrow (picked up / despawned) removes the
	 * owned Beta row as part of the imported set, exactly like an item. */
	struct chunk_entities snapshot = {.entities = current, .count = 2};
	nbt_node* merged = chunk_entities_preserve(
		&old_node, &new_node, &snapshot, false, 0, 0);
	assert(merged && nbt_list_count(merged) == 2);
	nbt_free(merged);
	/* A runtime that KEPT it: the owned row is replaced by its canonical
	 * record (never duplicated, never kept beside it). */
	struct archived_entity current3[3] = {current[0], current[1], imported[2]};
	current3[2].source_id = 103;
	current3[2].data.arrow.owner_source_id = UINT32_MAX;
	current3[2].data.arrow.shake_ticks = 5;
	void* arena3 = NULL;
	struct nbt_list* list3 = chunk_entities_build(
		&(struct chunk_entities) {.entities = current3, .count = 3}, &arena3);
	assert(list3);
	nbt_node node3 = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list3,
	};
	struct chunk_entities snapshot3 = {.entities = current3, .count = 3};
	merged = chunk_entities_preserve(&old_node, &node3, &snapshot3, false, 0, 0);
	assert(merged && nbt_list_count(merged) == 3);
	for(size_t k = 0; k < 3; k++)
		assert(item_nbt_equal(list_compound_at(merged->payload.tag_list, k),
			list_compound_at(list3, k)));
	nbt_free(merged);
	chunk_entities_build_free(arena3);
	/* An arrow carrying any non-Beta tag stays runtime-opaque as before. */
	nbt_node extra = {.type = TAG_BYTE, .name = "PluginTag"};
	struct nbt_list extra_wrap = {.data = &extra};
	list_add_tail(&extra_wrap.entry,
		&list_compound_at(old_list, 2)->payload.tag_compound->entry);
	assert(chunk_entities_parse(&old_node, 0, 0, &parsed));
	assert(parsed.count == 2);
	chunk_entities_free(&parsed);
	nbt_free(arrow_exact);
	arrow_exact = item_nbt_clone_lossless(list_compound_at(old_list, 2));
	assert(arrow_exact);
	merged = chunk_entities_preserve(
		&old_node, &new_node, &snapshot, false, 0, 0);
	list_del(&extra_wrap.entry);
	assert(merged && nbt_list_count(merged) == 3);
	assert(item_nbt_equal(list_compound_at(merged->payload.tag_list, 0),
		arrow_exact));
	/* The imported chunk-level marker remains foreign after the first save.
	 * A cold reload remaps IDs and may move entities before saving again; the
	 * exact canonical rows emitted above must converge as a proved set while
	 * the opaque Beta Arrow remains byte-identical. */
	struct archived_entity cold[2] = {current[0], current[1]};
	cold[0].source_id = 201;
	cold[1].source_id = 202;
	cold[0].pos[2] += 0.5f;
	cold[1].pos[2] += 0.5f;
	void* cold_arena = NULL;
	struct nbt_list* cold_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = cold, .count = 2},
		&cold_arena);
	assert(cold_list);
	nbt_node cold_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = cold_list,
	};
	struct chunk_entities cold_snapshot = {.entities = cold, .count = 2};
	nbt_node* converged = chunk_entities_preserve(
		merged, &cold_node, &cold_snapshot, false, 0, 0);
	assert(converged && nbt_list_count(converged) == 3);
	assert(item_nbt_equal(list_compound_at(converged->payload.tag_list, 0),
		arrow_exact));
	assert(item_nbt_equal(list_compound_at(converged->payload.tag_list, 1),
		list_compound_at(cold_list, 0)));
	assert(item_nbt_equal(list_compound_at(converged->payload.tag_list, 2),
		list_compound_at(cold_list, 1)));
	nbt_free(converged);
	chunk_entities_build_free(cold_arena);
	nbt_free(merged);
	nbt_free(arrow_exact);
	chunk_entities_build_free(new_arena);
	chunk_entities_build_free(old_arena);

	/* The immediately preceding native Item schema had no Air tag. Its exact
	 * historical encoding remains safe to delete after pickup/death. */
	struct archived_entity old_native = imported[0];
	old_native.source_id = 55;
	old_arena = NULL;
	old_list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &old_native, .count = 1},
		&old_arena);
	assert(old_list);
	nbt_node* native_compound = list_compound_at(old_list, 0);
	struct list_head *pos, *next;
	list_for_each_safe(pos, next, &native_compound->payload.tag_compound->entry) {
		struct nbt_list* wrap = list_entry(pos, struct nbt_list, entry);
		if(wrap->data && wrap->data->name && !strcmp(wrap->data->name, "Air")) {
			list_del(pos);
			break;
		}
	}
	old_node.payload.tag_list = old_list;
	struct nbt_list empty_list = {.data = &(nbt_node) {.type = TAG_COMPOUND}};
	INIT_LIST_HEAD(&empty_list.entry);
	nbt_node empty_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = &empty_list,
	};
	struct chunk_entities empty = {0};
	merged = chunk_entities_preserve(
		&old_node, &empty_node, &empty, true, 0, 0);
	assert(merged && nbt_list_count(merged) == 0);
	nbt_free(merged);
	chunk_entities_build_free(old_arena);
}

static struct nbt_list* build_single_tnt_list(struct archived_entity* record,
											  void** arena,
											  nbt_node** compound) {
	struct nbt_list* list = chunk_entities_build(
		&(struct chunk_entities) {.entities = record, .count = 1}, arena);
	assert(list && *arena);
	*compound = list_compound_at(list, 0);
	assert(*compound && (*compound)->type == TAG_COMPOUND);
	return list;
}

static void append_legacy_coords(nbt_node* compound, nbt_node nodes[3],
								 struct nbt_list wraps[3], int32_t x,
								 int32_t y, int32_t z, size_t count) {
	assert(compound && count <= 3);
	const char* names[3]
		= {"CaveWiiBlockX", "CaveWiiBlockY", "CaveWiiBlockZ"};
	const int32_t values[3] = {x, y, z};
	for(size_t i = 0; i < count; i++) {
		nodes[i].type = TAG_INT;
		nodes[i].name = (char*)names[i];
		nodes[i].payload.tag_int = values[i];
		wraps[i].data = &nodes[i];
		INIT_LIST_HEAD(&wraps[i].entry);
		list_add_tail(&wraps[i].entry, &compound->payload.tag_compound->entry);
	}
}

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

static void free_input_chunk(struct server_chunk* sc) {
	free(sc->ids);
	free(sc->metadata);
	free(sc->lighting_sky);
	free(sc->lighting_torch);
	free(sc->heightmap);
}

static void free_loaded_chunk(struct server_chunk* sc) {
	mem2_free(sc->ids);
	mem2_free(sc->metadata);
	mem2_free(sc->lighting_sky);
	mem2_free(sc->lighting_torch);
	mem2_free(sc->heightmap);
}

static void fill_records(struct archived_entity records[6]) {
	memset(records, 0, sizeof(struct archived_entity) * 6);
	records[0].kind = ARCHIVED_ENTITY_ITEM;
	records[0].source_id = 41;
	glm_vec3_copy((vec3) {1.25f, 72.5f, 2.75f}, records[0].pos);
	glm_vec3_copy((vec3) {0.1f, -0.2f, 0.3f}, records[0].vel);
	records[0].on_ground = false;
	// FH-18: a POSITIVE fire tick count on a non-monster kind -- the common
	// "Fire" tag is Entity.writeToNBT's base-class field, so every kind
	// round-trips it, not just monsters (see entity_archive.h's comment).
	records[0].fire = 187;
	records[0].fall_distance = 4.25f;
	records[0].data.item.item
		= (struct item_data) {.id = 278, .durability = 1500, .count = 1};
	records[0].data.item.age = 201;

	records[1].kind = ARCHIVED_ENTITY_MONSTER;
	records[1].source_id = 42;
	glm_vec3_copy((vec3) {5.5f, 70.1f, 6.5f}, records[1].pos);
	glm_vec3_copy((vec3) {-0.15f, 0.05f, 0.2f}, records[1].vel);
	glm_vec2_copy((vec2) {1.2f, -0.4f}, records[1].orient);
	records[1].on_ground = true;
	// FH-18: NEGATIVE -- Beta's fire-immunity cooldown (`fire = -fireResistance`
	// [740c583 Entity.java:531,536]), the case a bool could never represent.
	// Deliberately DIFFERENT from data.monster.fire_time (44) below: the two
	// are independent tags ("Fire" vs "CaveWiiFireTime") and must round-trip
	// independently, not mirror each other, to prove neither write clobbers
	// the other.
	records[1].fire = -20;
	records[1].health = 7;
	records[1].fall_distance = 1.75f;
	records[1].data.monster = (struct archived_monster_state) {
		.monster_id = MONSTER_SKELETON,
		.ai_state = 1,
		.ai_timer = 12.5f,
		.direction = {0.25f, -0.75f},
		.direction_time = 17,
		.body_yaw = 0.8f,
		.head_yaw = -0.6f,
		.fuse = -1,
		.attack_time = 9,
		.attack_windup = 3,
		.fire_time = 44,
		.hurt_time = 5,
		.knockback_time = 2,
		.aggro_time = 71,
		.limb_swing = 0.45f,
		.air = 287,
		.entity_age = 444,
		.death_time = 0,
		.death_loot_dropped = true,
		.hit_by_skeleton_arrow = true,
		.jump_delay = 13,
		.swim_vec = {0.2f, 0.3f, 0.4f},
		.swim_speed = 0.7f,
		.squid_phase = 1.1f,
		.squid_phase_vel = 0.14f,
	};

	records[2].kind = ARCHIVED_ENTITY_ARROW;
	records[2].source_id = 43;
	records[2].fall_distance = 0.75f;
	glm_vec3_copy((vec3) {7.0f, 71.0f, 8.0f}, records[2].pos);
	glm_vec3_copy((vec3) {0.6f, -0.03f, 0.1f}, records[2].vel);
	records[2].data.arrow = (struct archived_arrow_state) {
		.owner_source_id = 42,
		.age = 2000, /* EntityArrow.java:107-125: no airborne age cap */
		.item_id = 262,
		.damage = 4,
		.from_player = false,
		.from_skeleton = true,
		.in_ground = true,
		.stuck_ticks = 99,
		.shake_ticks = 2,
	};

	records[3].kind = ARCHIVED_ENTITY_MINECART;
	records[3].source_id = 44;
	glm_vec3_copy((vec3) {10.5f, 68.0f, 11.5f}, records[3].pos);
	glm_vec3_copy((vec3) {0.35f, 0.0f, 0.0f}, records[3].vel);
	records[3].on_ground = true;
	records[3].fall_distance = 2.5f;
	records[3].data.minecart = (struct archived_minecart_state) {
		.speed = 0.35f,
		.hx = 1,
		.hz = 0,
		.last_meta = 9,
		.occupied = true,
		.occupant_source_id = 0,
	};

	records[4].kind = ARCHIVED_ENTITY_MONSTER;
	records[4].source_id = 45;
	glm_vec3_copy((vec3) {12.5f, 70.0f, 13.5f}, records[4].pos);
	records[4].health = 10;
	records[4].data.monster = (struct archived_monster_state) {
		.monster_id = MONSTER_SHEEP,
		.fleece_color = 6, /* pink: explicit state, independent of runtime id */
		.fuse = -1,
		.air = 300,
		.squid_phase_vel = 0.15f,
	};

	records[5].kind = ARCHIVED_ENTITY_PRIMED_TNT;
	records[5].source_id = 46;
	records[5].fall_distance = 3.5f;
	glm_vec3_copy((vec3) {14.5f, 64.5f, 15.5f}, records[5].pos);
	records[5].data.primed_tnt = (struct archived_primed_tnt_state) {
		.fuse = 0,
		.explosion_pending = true,
	};
}

static void assert_records(const struct chunk_entities* out) {
	assert(out->count == 6);
	assert(out->entities[0].kind == ARCHIVED_ENTITY_ITEM);
	assert(out->entities[0].source_id == 41);
	assert(out->entities[0].data.item.item.id == 278);
	assert(out->entities[0].data.item.item.durability == 1500);
	assert(out->entities[0].data.item.age == 201);
	assert(feq(out->entities[0].fall_distance, 4.25f));
	assert(out->entities[0].fire == 187); // FH-18: positive, non-monster kind

	const struct archived_entity* mob = &out->entities[1];
	assert(mob->kind == ARCHIVED_ENTITY_MONSTER && mob->source_id == 42);
	assert(mob->data.monster.monster_id == MONSTER_SKELETON);
	assert(mob->health == 7);
	// FH-18: negative round-trips exactly (fire-immunity cooldown), and stays
	// INDEPENDENT of the monster's own fire_time tag (44 != -20).
	assert(mob->fire == -20);
	assert(mob->data.monster.fire_time == 44);
	assert(mob->data.monster.hurt_time == 5);
	assert(mob->data.monster.entity_age == 444);
	assert(mob->data.monster.death_time == 0
		   && mob->data.monster.death_loot_dropped);
	assert(feq(mob->fall_distance, 1.75f));
	assert(feq(mob->data.monster.squid_phase_vel, 0.14f));

	const struct archived_entity* arrow = &out->entities[2];
	assert(arrow->kind == ARCHIVED_ENTITY_ARROW && arrow->source_id == 43);
	assert(arrow->data.arrow.owner_source_id == 42);
	assert(arrow->data.arrow.from_skeleton && arrow->data.arrow.in_ground);
	assert(arrow->data.arrow.age == 2000);
	assert(arrow->data.arrow.stuck_ticks == 99);
	assert(arrow->data.arrow.shake_ticks == 2 && !arrow->data.arrow.from_player);
	assert(feq(arrow->fall_distance, 0.75f));

	const struct archived_entity* cart = &out->entities[3];
	assert(cart->kind == ARCHIVED_ENTITY_MINECART && cart->source_id == 44);
	assert(cart->data.minecart.occupied);
	assert(cart->data.minecart.occupant_source_id == 0);
	assert(feq(cart->data.minecart.speed, 0.35f));
	assert(feq(cart->fall_distance, 2.5f));

	const struct archived_entity* sheep = &out->entities[4];
	assert(sheep->kind == ARCHIVED_ENTITY_MONSTER);
	assert(sheep->data.monster.monster_id == MONSTER_SHEEP);
	assert(sheep->data.monster.fleece_color == 6);

	const struct archived_entity* tnt = &out->entities[5];
	assert(tnt->kind == ARCHIVED_ENTITY_PRIMED_TNT && tnt->source_id == 46);
	assert(tnt->data.primed_tnt.fuse == 0);
	assert(tnt->data.primed_tnt.explosion_pending);
	assert(!tnt->data.primed_tnt.legacy_block_backed);
	assert(feq(tnt->fall_distance, 3.5f));
	assert(feq(tnt->pos[0], 14.5f) && feq(tnt->pos[1], 64.5f)
		   && feq(tnt->pos[2], 15.5f));
}

static void test_common_fall_distance_is_single_tag(
	const struct chunk_entities* input) {
	/* Entity.writeToNBT writes FallDistance once in the common base method for
	 * items, living mobs, carts, arrows, and primed TNT alike. In particular the
	 * monster codec must not also emit a subclass duplicate. [740c583 Entity]
	 */
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(input, &arena);
	assert(list && arena);
	const size_t indices[] = {0, 1, 2, 3, 5};
	const float expected[] = {4.25f, 1.75f, 0.75f, 2.5f, 3.5f};
	for(size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
		nbt_node* entity = list_compound_at(list, indices[i]);
		assert(entity);
		assert(child_named_count(entity, "FallDistance") == 1);
		nbt_node* fall = child_named(entity, "FallDistance");
		assert(fall && fall->type == TAG_FLOAT);
		assert(feq(fall->payload.tag_float, expected[i]));
	}

	/* Older CaveWii item/cart/arrow/TNT records predate the common tag. Its
	 * absence must retain the Entity constructor's zero, while the historical
	 * monster FallDistance tag continues to parse through the common field. */
	nbt_node* old_item_fall
		= child_named(list_compound_at(list, 0), "FallDistance");
	assert(old_item_fall);
	old_item_fall->name = "CaveWiiPreCommonFallDistance";
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == input->count);
	assert(parsed.entities[0].fall_distance == 0.0f);
	assert(feq(parsed.entities[1].fall_distance, 1.75f));
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

static void test_malformed_record_is_skipped(const struct chunk_entities* input) {
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(input, &arena);
	assert(list && arena);
	struct nbt_list* first = list_entry(list->entry.flink, struct nbt_list, entry);
	nbt_node* compound = first->data;
	assert(compound && compound->type == TAG_COMPOUND);

	nbt_node* pos_list = NULL;
	struct list_head* p;
	list_for_each(p, &compound->payload.tag_compound->entry) {
		nbt_node* child = list_entry(p, struct nbt_list, entry)->data;
		if(child && child->name && strcmp(child->name, "Pos") == 0)
			pos_list = child;
	}
	assert(pos_list && pos_list->type == TAG_LIST);
	struct nbt_list* x = list_entry(pos_list->payload.tag_list->entry.flink,
									 struct nbt_list, entry);
	x->data->payload.tag_double = NAN;

	nbt_node list_node = {
		.type = TAG_LIST,
		.name = "Entities",
		.payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 5); /* bad item skipped; remaining records survive */
	assert(parsed.entities[0].kind == ARCHIVED_ENTITY_MONSTER);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

static void test_structural_nonempty_survives_unknown_record_filter(void) {
	/* Persistence must remember the raw list, not only recognized records. An
	 * unknown/forward-version compound parses to zero runtime entities, but a
	 * later empty checkpoint still has to rewrite the chunk to clear it. */
	struct nbt_list unknown_fields = {.data = NULL};
	struct nbt_list entities = {
		.data = &(nbt_node) {.type = TAG_COMPOUND},
	};
	nbt_node unknown = {
		.type = TAG_COMPOUND,
		.payload.tag_compound = &unknown_fields,
	};
	struct nbt_list unknown_link = {.data = &unknown};
	INIT_LIST_HEAD(&unknown_fields.entry);
	INIT_LIST_HEAD(&entities.entry);
	INIT_LIST_HEAD(&unknown_link.entry);
	list_add_tail(&unknown_link.entry, &entities.entry);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = &entities,
	};

	assert(chunk_entities_nbt_list_nonempty(&list_node));
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 0 && parsed.entities == NULL);

	list_del(&unknown_link.entry);
	assert(!chunk_entities_nbt_list_nonempty(&list_node));
}

static void test_parse_attempts_are_bounded(const struct chunk_entities* input) {
	/* The fail-closed ceiling applies to REAL candidate records (TAG_COMPOUND
	 * entries): more than CHUNK_ENTITY_ARCHIVE_READ_MAX compounds refuses the
	 * chunk rather than truncating the unseen tail. (Non-compound garbage no
	 * longer counts toward this bound -- see the scalar-garbage test below,
	 * bug-hunt D2.) The same compound node is linked repeatedly: the counting
	 * pass only inspects entry types, so identity reuse is fine here. */
	void* arena = NULL;
	struct nbt_list* valid = chunk_entities_build(input, &arena);
	assert(valid && arena);
	struct nbt_list* first
		= list_entry(valid->entry.flink, struct nbt_list, entry);

	struct nbt_list bounded;
	memset(&bounded, 0, sizeof(bounded));
	INIT_LIST_HEAD(&bounded.entry);
	struct nbt_list* links
		= calloc(CHUNK_ENTITY_ARCHIVE_READ_MAX + 1, sizeof(*links));
	assert(links);
	for(size_t i = 0; i < CHUNK_ENTITY_ARCHIVE_READ_MAX + 1; i++) {
		INIT_LIST_HEAD(&links[i].entry);
		links[i].data = first->data;
		list_add_tail(&links[i].entry, &bounded.entry);
	}

	nbt_node list_node = {
		.type = TAG_LIST,
		.name = "Entities",
		.payload.tag_list = &bounded,
	};
	struct chunk_entities parsed = {0};
	assert(!chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 0 && parsed.entities == NULL);
	free(links);
	chunk_entities_build_free(arena);
}

static void test_scalar_garbage_is_skipped_not_amplified(
	const struct chunk_entities* input) {
	/* Bug-hunt D2: the pre-parse candidate count feeds a calloc, and it used to
	 * count RAW list entries -- a corrupt/hostile list of tiny scalar entries
	 * (a few encoded bytes each) amplified into a transient allocation of up to
	 * READ_MAX * sizeof(struct archived_entity) on the 24MB-MEM1 Wii, and any
	 * list longer than READ_MAX failed the whole chunk closed even when every
	 * excess entry was unparseable garbage. Now only TAG_COMPOUND entries count
	 * (Beta's writer emits nothing else; parse_entity skips non-compounds
	 * regardless), so scalar padding neither amplifies the allocation nor costs
	 * the chunk: the real records still load. */
	void* arena = NULL;
	struct nbt_list* valid = chunk_entities_build(input, &arena);
	assert(valid && arena);

	struct nbt_list padded;
	memset(&padded, 0, sizeof(padded));
	INIT_LIST_HEAD(&padded.entry);
	nbt_node scalar = {.type = TAG_BYTE, .payload.tag_byte = 7};
	struct nbt_list* links
		= calloc(CHUNK_ENTITY_ARCHIVE_READ_MAX + 1, sizeof(*links));
	assert(links);
	for(size_t i = 0; i < CHUNK_ENTITY_ARCHIVE_READ_MAX + 1; i++) {
		INIT_LIST_HEAD(&links[i].entry);
		links[i].data = &scalar;
		list_add_tail(&links[i].entry, &padded.entry);
	}
	/* The six real records ride behind the garbage, in build order. */
	struct nbt_list record_links[6];
	struct list_head* p = valid->entry.flink;
	for(size_t i = 0; i < input->count; i++, p = p->flink) {
		record_links[i].data = list_entry(p, struct nbt_list, entry)->data;
		INIT_LIST_HEAD(&record_links[i].entry);
		list_add_tail(&record_links[i].entry, &padded.entry);
	}

	nbt_node list_node = {
		.type = TAG_LIST,
		.name = "Entities",
		.payload.tag_list = &padded,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == input->count);
	chunk_entities_free(&parsed);
	free(links);
	chunk_entities_build_free(arena);
}

static void test_unsigned_fields_round_trip(const struct chunk_entities* input) {
	struct archived_entity records[6];
	memcpy(records, input->entities, sizeof(records));
	records[0].source_id = UINT32_C(0xf0000029);
	records[0].data.item.item.id = UINT16_C(40000);
	records[0].data.item.item.durability = UINT16_C(50000);
	records[0].data.item.item.count = UINT8_C(200);
	records[2].data.arrow.owner_source_id = UINT32_MAX;
	records[3].data.minecart.last_meta = UINT8_MAX;

	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(
		&(struct chunk_entities) {.entities = records, .count = 6}, &arena);
	assert(list && arena);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 6);
	assert(parsed.entities[0].source_id == UINT32_C(0xf0000029));
	assert(parsed.entities[0].data.item.item.id == UINT16_C(40000));
	assert(parsed.entities[0].data.item.item.durability == UINT16_C(50000));
	assert(parsed.entities[0].data.item.item.count == UINT8_C(200));
	assert(parsed.entities[2].data.arrow.owner_source_id == UINT32_MAX);
	assert(parsed.entities[3].data.minecart.last_meta == UINT8_MAX);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

/* ci-1101/41 item 12: EntityArrow.writeEntityToNBT (RetroMCP
 * EntityArrow.java:241,243) names the shake counter "shake" (byte) and the
 * pickup flag "player" (boolean byte). The writer uses Beta's names; the
 * reader still accepts the pre-fix CaveWiiShakeTicks (int) /
 * CaveWiiFromPlayer (byte) pair, and such an old native record stays
 * byte-proven deletion-safe and converges to the Beta names on rewrite. */
static void test_arrow_beta_tag_names(const struct chunk_entities* input) {
	struct archived_entity one = input->entities[2];
	assert(one.kind == ARCHIVED_ENTITY_ARROW);
	one.data.arrow.from_player = true;
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &one, .count = 1}, &arena);
	assert(list && arena);
	nbt_node* arrow = list_compound_at(list, 0);
	nbt_node* player = child_named(arrow, "player");
	nbt_node* shake = child_named(arrow, "shake");
	assert(player && player->type == TAG_BYTE && player->payload.tag_byte == 1);
	assert(shake && shake->type == TAG_BYTE && shake->payload.tag_byte == 2);
	assert(!child_named(arrow, "CaveWiiFromPlayer"));
	assert(!child_named(arrow, "CaveWiiShakeTicks"));
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 1 && parsed.entities[0].data.arrow.from_player
		   && parsed.entities[0].data.arrow.shake_ticks == 2);
	chunk_entities_free(&parsed);
	/* The generated (new-name) list, kept for the convergence check. */
	nbt_node* generated = nbt_clone(&list_node);
	assert(generated);

	/* Rewrite it into the exact pre-fix shape, in place. */
	player->name = (char*)"CaveWiiFromPlayer";
	shake->name = (char*)"CaveWiiShakeTicks";
	shake->type = TAG_INT;
	shake->payload.tag_int = 2;
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 1 && parsed.entities[0].data.arrow.from_player
		   && parsed.entities[0].data.arrow.shake_ticks == 2
		   && parsed.entities[0].data.arrow.item_id == 262
		   && parsed.entities[0].data.arrow.owner_source_id == 42);
	/* Native snapshot WITHOUT the arrow: the old record is deletion-safe. */
	struct nbt_list empty_list = {.data = &(nbt_node) {.type = TAG_COMPOUND}};
	INIT_LIST_HEAD(&empty_list.entry);
	nbt_node empty_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = &empty_list,
	};
	struct chunk_entities empty = {0};
	nbt_node* merged = chunk_entities_preserve(
		&list_node, &empty_node, &empty, true, 0, 0);
	assert(merged && nbt_list_count(merged) == 0);
	nbt_free(merged);
	/* Native snapshot WITH the arrow: the record converges to Beta names. */
	struct chunk_entities same = {.entities = &parsed.entities[0], .count = 1};
	merged = chunk_entities_preserve(&list_node, generated, &same, true, 0, 0);
	assert(merged && nbt_list_count(merged) == 1);
	assert(item_nbt_equal(list_compound_at(merged->payload.tag_list, 0),
						  list_compound_at(generated->payload.tag_list, 0)));
	nbt_free(merged);
	chunk_entities_free(&parsed);
	nbt_free(generated);
	chunk_entities_build_free(arena);
}

static void test_corrupt_optional_fields_are_sanitized(
	const struct chunk_entities* input) {
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(input, &arena);
	assert(list && arena);

	nbt_node* monster = list_compound_at(list, 1);
	nbt_node* arrow = list_compound_at(list, 2);
	nbt_node* cart = list_compound_at(list, 3);
	nbt_node* sheep = list_compound_at(list, 4);
	assert(monster && arrow && cart && sheep);
	child_named(monster, "FallDistance")->payload.tag_float = NAN;
	child_named(monster, "CaveWiiEntityAge")->payload.tag_int = INT32_MAX;
	child_named(arrow, "CaveWiiAge")->payload.tag_int = -1;
	child_named(arrow, "CaveWiiStuckTicks")->payload.tag_int = INT32_MAX;
	child_named(cart, "CaveWiiSpeed")->payload.tag_float = INFINITY;
	child_named(cart, "CaveWiiHeadingX")->payload.tag_byte = INT8_MAX;
	child_named(sheep, "Health")->type = TAG_STRING;

	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 5); /* wrong-type required Health drops only the sheep */
	assert(parsed.entities[1].kind == ARCHIVED_ENTITY_MONSTER);
	assert(parsed.entities[1].fall_distance == 0.0f);
	assert(parsed.entities[1].data.monster.entity_age == 0);
	assert(parsed.entities[2].kind == ARCHIVED_ENTITY_ARROW);
	assert(parsed.entities[2].data.arrow.age == 0);
	assert(parsed.entities[2].data.arrow.stuck_ticks == 0);
	assert(parsed.entities[3].kind == ARCHIVED_ENTITY_MINECART);
	assert(parsed.entities[3].data.minecart.speed == 0.0f);
	assert(parsed.entities[3].data.minecart.hx == 0);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

// FH-18: a hostile/malformed "Fire" tag -- a real TAG_SHORT can never hold an
// out-of-int16_t value, but read_int() also accepts TAG_BYTE/TAG_INT for this
// name (entity_archive.c's read_int type switch), so a type-confused hostile
// chunk CAN present a wider int. That must CLAMP, not overflow/UB the
// (int16_t) cast -- the same defensive-parsing framing entity_archive.h's own
// header documents for this whole file, and the same clamp shape as Health.
static void test_fire_value_clamps_to_int16_range(
	const struct chunk_entities* input) {
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(input, &arena);
	assert(list && arena);

	nbt_node* item = list_compound_at(list, 0);
	nbt_node* monster = list_compound_at(list, 1);
	assert(item && monster);
	nbt_node* item_fire = child_named(item, "Fire");
	nbt_node* monster_fire = child_named(monster, "Fire");
	assert(item_fire->type == TAG_SHORT && monster_fire->type == TAG_SHORT);
	item_fire->type = TAG_INT;
	item_fire->payload.tag_int = INT32_MAX;
	monster_fire->type = TAG_INT;
	monster_fire->payload.tag_int = INT32_MIN;

	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 6);
	assert(parsed.entities[0].fire == INT16_MAX);
	assert(parsed.entities[1].fire == INT16_MIN);
	// Nothing else on either record was disturbed by the corruption.
	assert(parsed.entities[1].data.monster.fire_time == 44);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

static void test_far_tnt_uses_runtime_position_chunk_ownership(void) {
	const int32_t x = INT32_C(16777231); /* rounds into the next chunk as float */
	struct archived_entity record = {0};
	record.kind = ARCHIVED_ENTITY_PRIMED_TNT;
	record.pos[0] = (float)x + 0.5f;
	record.pos[1] = 64.5f;
	record.pos[2] = 0.5f;
	record.data.primed_tnt = (struct archived_primed_tnt_state) {
		.fuse = 37,
	};
	void* arena = NULL;
	nbt_node* compound = NULL;
	struct nbt_list* list
		= build_single_tnt_list(&record, &arena, &compound);
	(void)compound;
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	w_coord_t owner = (w_coord_t)floorf(record.pos[0] / CHUNK_SIZE);
	assert(chunk_entities_parse(&list_node, owner, 0, &parsed));
	assert(parsed.count == 1);
	assert(feq(parsed.entities[0].pos[0], record.pos[0]));
	assert(!parsed.entities[0].data.primed_tnt.legacy_block_backed);
	chunk_entities_free(&parsed);
	assert(chunk_entities_parse(
		&list_node, (w_coord_t)(x / CHUNK_SIZE), 0, &parsed));
	assert(parsed.count == 0 && parsed.entities == NULL);
	chunk_entities_build_free(arena);
}

static void test_legacy_block_tnt_record_is_detected(void) {
	struct archived_entity record = {0};
	record.kind = ARCHIVED_ENTITY_PRIMED_TNT;
	record.pos[0] = 14.5f;
	record.pos[1] = 64.5f;
	record.pos[2] = 15.5f;
	record.data.primed_tnt.fuse = 37;
	void* arena = NULL;
	nbt_node* compound = NULL;
	struct nbt_list* list
		= build_single_tnt_list(&record, &arena, &compound);
	nbt_node coords[3] = {0};
	struct nbt_list wraps[3] = {0};
	append_legacy_coords(compound, coords, wraps, 14, 64, 15, 3);

	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 1);
	const struct archived_primed_tnt_state* t
		= &parsed.entities[0].data.primed_tnt;
	assert(t->legacy_block_backed);
	assert(t->x == 14 && t->y == 64 && t->z == 15 && t->fuse == 37);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

static void test_far_legacy_tnt_uses_integer_chunk_ownership(void) {
	const int32_t x = INT32_C(16777231); /* float Pos rounds into next chunk */
	struct archived_entity record = {0};
	record.kind = ARCHIVED_ENTITY_PRIMED_TNT;
	record.pos[0] = (float)x + 0.5f;
	record.pos[1] = 64.5f;
	record.pos[2] = 0.5f;
	record.data.primed_tnt.fuse = 37;
	void* arena = NULL;
	nbt_node* compound = NULL;
	struct nbt_list* list
		= build_single_tnt_list(&record, &arena, &compound);
	nbt_node coords[3] = {0};
	struct nbt_list wraps[3] = {0};
	append_legacy_coords(compound, coords, wraps, x, 64, 0, 3);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	w_coord_t integer_owner = (w_coord_t)(x / CHUNK_SIZE);
	w_coord_t rounded_owner
		= (w_coord_t)floorf(record.pos[0] / CHUNK_SIZE);
	assert(integer_owner != rounded_owner);
	assert(chunk_entities_parse(&list_node, integer_owner, 0, &parsed));
	assert(parsed.count == 1);
	assert(parsed.entities[0].data.primed_tnt.legacy_block_backed);
	assert(parsed.entities[0].data.primed_tnt.x == x);
	chunk_entities_free(&parsed);
	assert(chunk_entities_parse(&list_node, rounded_owner, 0, &parsed));
	assert(parsed.count == 0 && parsed.entities == NULL);
	chunk_entities_build_free(arena);
}

static void test_legacy_coordinate_tags_must_be_complete_and_numeric(void) {
	struct archived_entity record = {0};
	record.kind = ARCHIVED_ENTITY_PRIMED_TNT;
	glm_vec3_copy((vec3) {14.5f, 64.5f, 15.5f}, record.pos);
	record.data.primed_tnt.fuse = 37;

	/* All tags are present, but X has the wrong type: this is malformed legacy
	 * data, not a modern moving-TNT record with three ignorable extensions. */
	void* arena = NULL;
	nbt_node* compound = NULL;
	struct nbt_list* list
		= build_single_tnt_list(&record, &arena, &compound);
	nbt_node coords[3] = {0};
	struct nbt_list wraps[3] = {0};
	append_legacy_coords(compound, coords, wraps, 14, 64, 15, 3);
	coords[0].type = TAG_STRING;
	coords[0].payload.tag_string = "not-an-integer";
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 0 && parsed.entities == NULL);
	chunk_entities_build_free(arena);

	/* A numeric X/Y pair with Z absent is likewise an invalid partial legacy
	 * triple and must not silently become a modern record. */
	arena = NULL;
	compound = NULL;
	list = build_single_tnt_list(&record, &arena, &compound);
	memset(coords, 0, sizeof(coords));
	memset(wraps, 0, sizeof(wraps));
	append_legacy_coords(compound, coords, wraps, 14, 64, 15, 2);
	list_node.payload.tag_list = list;
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 0 && parsed.entities == NULL);
	chunk_entities_build_free(arena);
}

static void test_writer_has_no_legacy_608_cap(void) {
	/* The deleted block-fuse table once made 608 look like a runtime bound.
	 * Dense real EntityTNTPrimed chains can exceed it, so the writer must encode
	 * the complete live set (or report an allocation/format failure), never
	 * reject/truncate merely because record 609 exists. */
	enum { COUNT = 609 };
	struct archived_entity* records = calloc(COUNT, sizeof(*records));
	assert(records);
	for(size_t i = 0; i < COUNT; i++) {
		records[i].kind = ARCHIVED_ENTITY_PRIMED_TNT;
		records[i].source_id = (uint32_t)i + 1;
		glm_vec3_copy((vec3) {8.5f, 70.5f, 8.5f}, records[i].pos);
		records[i].data.primed_tnt.fuse = (uint16_t)(10 + i % 20);
	}

	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(
		&(struct chunk_entities) {.entities = records, .count = COUNT}, &arena);
	assert(list && arena);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == COUNT);
	assert(parsed.entities[COUNT - 1].source_id == COUNT);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
	free(records);
}

int main(void) {
	test_fixture_dir("test_region_entities_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(region_archive_tile_entities_shape_valid(false, false, false));
	assert(region_archive_tile_entities_shape_valid(false, false, true));
	assert(!region_archive_tile_entities_shape_valid(true, false, false));
	assert(!region_archive_tile_entities_shape_valid(true, true, false));
	assert(region_archive_tile_entities_shape_valid(true, true, true));
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	struct region_archive ra;
	assert(region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));

	struct server_chunk sc;
	init_chunk(&sc);
	struct archived_entity records[6];
	fill_records(records);
	struct chunk_entities input = {.entities = records, .count = 6};
	struct chunk_tile_entities complete_te = {.legacy_flat_complete = true};
	assert(region_archive_set_chunk(
		&ra, 0, 0, &sc, &complete_te, &input));

	struct server_chunk loaded = {0};
	struct chunk_tile_entities loaded_te = {0};
	struct chunk_entities output = {0};
	assert(region_archive_get_chunk(
		&ra, 0, 0, &loaded, &loaded_te, &output));
	assert_records(&output);
	assert(loaded.persisted_tile_entities_complete
		   && loaded_te.legacy_flat_complete);
	assert(loaded.persisted_entities_nonempty);
	assert(memcmp(sc.ids, loaded.ids, IDS_SIZE) == 0);
	assert(memcmp(sc.metadata, loaded.metadata, NIB_SIZE) == 0);

	test_malformed_record_is_skipped(&input);
	test_structural_nonempty_survives_unknown_record_filter();
	test_parse_attempts_are_bounded(&input);
	test_scalar_garbage_is_skipped_not_amplified(&input);
	test_unsigned_fields_round_trip(&input);
	test_common_fall_distance_is_single_tag(&input);
	test_corrupt_optional_fields_are_sanitized(&input);
	test_arrow_beta_tag_names(&input);
	test_fire_value_clamps_to_int16_range(&input);
	test_beta_owned_set_reconciliation();
	test_beta_mob_fire_and_unowned_fail_closed();
	test_beta_mixed_opaque_arrow_and_native_item_compat();
	test_beta_supported_species_set();
	test_far_tnt_uses_runtime_position_chunk_ownership();
	test_legacy_block_tnt_record_is_detected();
	test_far_legacy_tnt_uses_integer_chunk_ownership();
	test_legacy_coordinate_tags_must_be_complete_and_numeric();
	test_writer_has_no_legacy_608_cap();

	/* Moving/death can leave a chunk with no entities without changing blocks.
	 * An explicit empty rewrite must clear the prior list, not preserve it. */
	struct chunk_entities empty = {0};
	sc.modified = true;
	assert(region_archive_set_chunk(&ra, 0, 0, &sc, NULL, &empty));
	struct server_chunk cleared_chunk = {0};
	struct chunk_entities cleared = {0};
	assert(region_archive_get_chunk(&ra, 0, 0, &cleared_chunk, NULL, &cleared));
	assert(cleared.count == 0 && cleared.entities == NULL);
	assert(!cleared_chunk.persisted_entities_nonempty);
	/* A NULL TE snapshot supplies no authority to revoke the retained scope. */
	assert(cleared_chunk.persisted_tile_entities_complete);
	free_loaded_chunk(&cleared_chunk);
	tile_entities_parse_free(&loaded_te);
	chunk_entities_free(&output);
	free_loaded_chunk(&loaded);
	free_input_chunk(&sc);
	region_archive_destroy(&ra);
	string_clear(level);
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	printf("test_region_entities: runtime entities + sheep color + TNT fuse round-trip\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
