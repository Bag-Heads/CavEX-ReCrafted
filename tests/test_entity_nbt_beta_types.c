/* java-load-1023: every Beta-NAMED tag CaveWii writes for entities and tile
 * entities must carry the exact Beta 1.7.3 NBT tag TYPE. Beta's
 * NBTTagCompound.getShort/getInteger/... cast the stored tag to the concrete
 * class (NBTTagCompound.java:98-104), so a wrong type throws
 * ClassCastException in Entity.readFromNBT and vanilla drops every entity of
 * that chunk (real-Beta server console, tmp/jobs/java-load-1023/RESULT.md).
 *
 * The table below is cited from the RetroMCP b1.7.3 client decompile
 * (../mcb173-retromcp/minecraft/src/net/minecraft/src):
 *   Entity.java:742-757         id str, Pos/Motion dbl list, Rotation flt list,
 *                               FallDistance flt, Fire short, Air short,
 *                               OnGround bool(byte)
 *   EntityLiving.java:558-561   Health/HurtTime/DeathTime/AttackTime short
 *   EntityItem.java:98-100      Health short, Age short, Item compound
 *   ItemStack.java:76-78        id short, Count byte, Damage short
 *   EntityPig.java:16 Saddle bool; EntitySheep.java:57-58 Sheared bool
 *   (written for every sheep), Color byte; EntityWolf.java:40-45 Angry/Sitting bool, Owner str;
 *   EntityPigZombie.java:33 Anger short; EntityCreeper.java:21 powered bool;
 *   EntitySlime.java:35 Size int (= getSlimeSize() - 1)
 *   EntityMinecart.java:599-616 Type int, PushX/PushZ dbl, Fuel short,
 *                               Items list, Slot byte
 *   EntityArrow.java:236-243    x/y/zTile short, inTile/inData/shake/inGround
 *                               byte, player bool
 *   EntityTNTPrimed.java:72 Fuse byte; EntityFallingSand.java:74 Tile byte
 *   EntityPainting.java:211-215 Dir byte, Motive str, TileX/Y/Z int
 *   TileEntity.java:35-38       id str, x/y/z int
 *   TileEntityFurnace.java:69-70 BurnTime/CookTime short
 *   TileEntitySign.java:10-13   Text1..4 str
 *   TileEntityMobSpawner.java:101-102 EntityId str, Delay short
 *   TileEntityNote.java:9 note byte; TileEntityRecordPlayer.java:14 Record int
 *   TileEntityPiston.java:139-143 blockId/blockData/facing int, progress flt,
 *                               extending bool
 * CaveWii* extension tags are ignored by vanilla and are not checked.
 *
 * Optional argv[1]: dump the built {Entities, TileEntities} compound
 * (uncompressed NBT) there for the external static checker. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "entity/monsters_object.h"
#include "network/entity_archive.h"
#include "network/tile_entities_nbt.h"

struct beta_tag {
	const char* name;
	nbt_type type;
};

/* Record-level tags (entity and tile-entity compounds). */
static const struct beta_tag k_record[] = {
	{"id", TAG_STRING}, {"Pos", TAG_LIST}, {"Motion", TAG_LIST},
	{"Rotation", TAG_LIST}, {"FallDistance", TAG_FLOAT}, {"Fire", TAG_SHORT},
	{"Air", TAG_SHORT}, {"OnGround", TAG_BYTE}, {"Health", TAG_SHORT},
	{"HurtTime", TAG_SHORT}, {"DeathTime", TAG_SHORT},
	{"AttackTime", TAG_SHORT}, {"Age", TAG_SHORT}, {"Item", TAG_COMPOUND},
	{"Saddle", TAG_BYTE}, {"Sheared", TAG_BYTE}, {"Color", TAG_BYTE},
	{"Angry", TAG_BYTE}, {"Sitting", TAG_BYTE}, {"Owner", TAG_STRING},
	{"Anger", TAG_SHORT}, {"powered", TAG_BYTE}, {"Size", TAG_INT},
	{"Type", TAG_INT}, {"PushX", TAG_DOUBLE}, {"PushZ", TAG_DOUBLE},
	{"Fuel", TAG_SHORT}, {"Items", TAG_LIST}, {"xTile", TAG_SHORT},
	{"yTile", TAG_SHORT}, {"zTile", TAG_SHORT}, {"inTile", TAG_BYTE},
	{"inData", TAG_BYTE}, {"shake", TAG_BYTE}, {"inGround", TAG_BYTE},
	{"player", TAG_BYTE}, {"Fuse", TAG_BYTE}, {"Tile", TAG_BYTE},
	{"Dir", TAG_BYTE}, {"Motive", TAG_STRING}, {"TileX", TAG_INT},
	{"TileY", TAG_INT}, {"TileZ", TAG_INT}, {"x", TAG_INT}, {"y", TAG_INT},
	{"z", TAG_INT}, {"BurnTime", TAG_SHORT}, {"CookTime", TAG_SHORT},
	{"Text1", TAG_STRING}, {"Text2", TAG_STRING}, {"Text3", TAG_STRING},
	{"Text4", TAG_STRING}, {"EntityId", TAG_STRING}, {"Delay", TAG_SHORT},
	{"note", TAG_BYTE}, {"Record", TAG_INT}, {"blockId", TAG_INT},
	{"blockData", TAG_INT}, {"facing", TAG_INT}, {"progress", TAG_FLOAT},
	{"extending", TAG_BYTE},
};
/* ItemStack compounds ("Item", and every "Items" element). */
static const struct beta_tag k_stack[] = {
	{"Slot", TAG_BYTE}, {"id", TAG_SHORT}, {"Count", TAG_BYTE},
	{"Damage", TAG_SHORT},
};

static int s_fail;
static int s_checked;

static const struct beta_tag* lookup(const struct beta_tag* t, size_t n,
									 const char* name) {
	for(size_t i = 0; i < n; i++)
		if(!strcmp(t[i].name, name))
			return &t[i];
	return NULL;
}

static nbt_node* child(const nbt_node* comp, const char* name) {
	struct list_head* p;
	list_for_each(p, &comp->payload.tag_compound->entry) {
		nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(n && n->name && !strcmp(n->name, name))
			return n;
	}
	return NULL;
}

static void check_compound(const nbt_node* comp, bool stack, const char* rid);

/* Typed-list shape, independent of the codec's own (lenient) reader. Beta's
 * Entity.readFromNBT casts each element [Entity.java:765-784]:
 * ((NBTTagDouble)Pos/Motion.tagAt(i)), ((NBTTagFloat)Rotation.tagAt(i)), so a
 * float Pos or double Rotation throws. Items elements are ItemStack compounds
 * [EntityMinecart.java:599-616, TileEntityChest/Furnace/Dispenser
 * writeToNBT]. want_count < 0 = any length. An EMPTY list may carry any
 * header type (Beta's own empty NBTTagList writes type 1, see
 * tile_entities_nbt.c), but a non-empty one must declare want_elem. */
static void check_list(const nbt_node* n, const char* rid, nbt_type want_elem,
					   int want_count) {
	if(n->type != TAG_LIST || !n->payload.tag_list) {
		fprintf(stderr, "BADLIST %s.%s not a list\n", rid, n->name);
		s_fail++;
		return;
	}
	int count = 0;
	struct list_head* p;
	list_for_each(p, &n->payload.tag_list->entry) {
		const nbt_node* e = list_entry(p, struct nbt_list, entry)->data;
		if(!e || e->type != want_elem) {
			fprintf(stderr, "BADLIST %s.%s[%d] element type %d, Beta wants %d\n",
					rid, n->name, count, e ? (int)e->type : -1, want_elem);
			s_fail++;
		}
		count++;
	}
	const nbt_node* header = n->payload.tag_list->data;
	if(count > 0 && (!header || header->type != want_elem)) {
		fprintf(stderr, "BADLIST %s.%s header type %d, Beta wants %d\n", rid,
				n->name, header ? (int)header->type : -1, want_elem);
		s_fail++;
	}
	if(want_count >= 0 && count != want_count) {
		fprintf(stderr, "BADLIST %s.%s length %d, Beta wants %d\n", rid,
				n->name, count, want_count);
		s_fail++;
	}
}

static void check_stack_list(const nbt_node* list, const char* rid) {
	struct list_head* p;
	list_for_each(p, &list->payload.tag_list->entry) {
		const nbt_node* e = list_entry(p, struct nbt_list, entry)->data;
		if(!e || e->type != TAG_COMPOUND) continue; /* check_list reported it */
		check_compound(e, true, rid);
	}
}

static void check_compound(const nbt_node* comp, bool stack, const char* rid) {
	assert(comp && comp->type == TAG_COMPOUND);
	struct list_head* p;
	list_for_each(p, &comp->payload.tag_compound->entry) {
		const nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(!n || !n->name || !strncmp(n->name, "CaveWii", 7))
			continue;
		const struct beta_tag* want = stack
			? lookup(k_stack, sizeof(k_stack) / sizeof(k_stack[0]), n->name)
			: lookup(k_record, sizeof(k_record) / sizeof(k_record[0]), n->name);
		if(!want) {
			/* An unknown non-CaveWii name would be invisible to vanilla but
			 * also unreviewed here: flag it so the table stays complete. */
			fprintf(stderr, "UNLISTED %s.%s (type %d)\n", rid, n->name, n->type);
			s_fail++;
			continue;
		}
		s_checked++;
		if(n->type != want->type) {
			fprintf(stderr, "WRONGTYPE %s.%s = %d, Beta wants %d\n", rid,
					n->name, n->type, want->type);
			s_fail++;
		}
		if(!stack && !strcmp(n->name, "Item") && n->type == TAG_COMPOUND)
			check_compound(n, true, rid);
		if(!stack && !strcmp(n->name, "Pos"))
			check_list(n, rid, TAG_DOUBLE, 3);
		if(!stack && !strcmp(n->name, "Motion"))
			check_list(n, rid, TAG_DOUBLE, 3);
		if(!stack && !strcmp(n->name, "Rotation"))
			check_list(n, rid, TAG_FLOAT, 2);
		if(!strcmp(n->name, "Items") && n->type == TAG_LIST) {
			check_list(n, rid, TAG_COMPOUND, -1);
			check_stack_list(n, rid);
		}
	}
}

static void require(const nbt_node* comp, const char* rid, const char* name) {
	if(!child(comp, name)) {
		fprintf(stderr, "MISSING %s.%s\n", rid, name);
		s_fail++;
	}
}

/* ---- fixtures -------------------------------------------------------- */

static struct archived_entity base(enum archived_entity_kind kind, uint32_t id) {
	struct archived_entity e;
	memset(&e, 0, sizeof(e));
	e.kind = kind;
	e.source_id = id;
	e.pos[0] = 3.5f; e.pos[1] = 70.0f; e.pos[2] = 4.5f;
	e.air = 300;
	e.health = 10;
	return e;
}

static size_t make_entities(struct archived_entity* out) {
	size_t n = 0;
	uint32_t id = 100;
	struct archived_entity e = base(ARCHIVED_ENTITY_ITEM, id++);
	e.health = 5;
	e.data.item.item = (struct item_data) {.id = 4, .count = 3};
	e.data.item.age = 1234;
	out[n++] = e;
	for(int m = 0; m < MONSTER_COUNT; m++) {
		e = base(ARCHIVED_ENTITY_MONSTER, id++);
		e.data.monster.monster_id = m;
		e.data.monster.fuse = -1;
		e.data.monster.air = 300;
		e.data.monster.hurt_time = 7;
		e.data.monster.death_time = 0;
		e.data.monster.attack_time = 11;
		e.data.monster.anger = m == MONSTER_PIG_ZOMBIE ? 400 : 0;
		if(m == MONSTER_SHEEP) { /* non-white, sheared: both non-default */
			e.data.monster.fleece_color = 14;
			e.data.monster.sheared = true;
		}
		strcpy(e.data.monster.wolf_owner, "");
		out[n++] = e;
	}
	for(int t = 0; t < 3; t++) {
		e = base(ARCHIVED_ENTITY_MINECART, id++);
		e.data.minecart.minecart_type = (uint8_t)t;
		if(t == 1)
			e.data.minecart.inventory[2] = (struct item_data) {.id = 1, .count = 5};
		if(t == 2)
			e.data.minecart.fuel = 1200;
		out[n++] = e;
	}
	e = base(ARCHIVED_ENTITY_ARROW, id++);
	e.data.arrow.item_id = 262;
	e.data.arrow.damage = 2;
	out[n++] = e;
	e = base(ARCHIVED_ENTITY_PRIMED_TNT, id++);
	e.data.primed_tnt.fuse = 80;
	out[n++] = e;
	out[n++] = base(ARCHIVED_ENTITY_EGG, id++);
	out[n++] = base(ARCHIVED_ENTITY_SNOWBALL, id++);
	out[n++] = base(ARCHIVED_ENTITY_FIREBALL, id++);
	out[n++] = base(ARCHIVED_ENTITY_BOAT, id++);
	e = base(ARCHIVED_ENTITY_PAINTING, id++);
	e.data.painting.tile_x = 3; e.data.painting.tile_y = 70;
	e.data.painting.tile_z = 4;
	strcpy(e.data.painting.art_name, "Kebab");
	out[n++] = e;
	e = base(ARCHIVED_ENTITY_FALLING_SAND, id++);
	e.data.falling_sand.block_id = 12;
	out[n++] = e;
	return n;
}

static void wrap_list(nbt_node* node, struct nbt_list* sentinel) {
	memset(node, 0, sizeof(*node));
	node->type = TAG_LIST;
	node->payload.tag_list = sentinel;
}

static nbt_node* record_at(const nbt_node* list, size_t index) {
	size_t i = 0;
	struct list_head* p;
	list_for_each(p, &list->payload.tag_list->entry) {
		if(i++ == index)
			return list_entry(p, struct nbt_list, entry)->data;
	}
	return NULL;
}

static const char* rec_id(const nbt_node* rec) {
	const nbt_node* id = child(rec, "id");
	return id && id->type == TAG_STRING ? id->payload.tag_string : "?";
}

static void dump_world(const char* path, struct nbt_list* ents,
					   struct nbt_list* tes) {
	nbt_node e_node, t_node, root;
	wrap_list(&e_node, ents);
	e_node.name = "Entities";
	wrap_list(&t_node, tes);
	t_node.name = "TileEntities";
	struct nbt_list root_kids, w1, w2;
	INIT_LIST_HEAD(&root_kids.entry);
	w1.data = &e_node; w2.data = &t_node;
	list_add_tail(&w1.entry, &root_kids.entry);
	list_add_tail(&w2.entry, &root_kids.entry);
	memset(&root, 0, sizeof(root));
	root.type = TAG_COMPOUND;
	root.name = "";
	root.payload.tag_compound = &root_kids;
	struct buffer b = nbt_dump_binary(&root);
	assert(b.data);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(b.data, 1, b.len, f) == b.len);
	fclose(f);
	buffer_free(&b);
}

/* Dump a TAG_LIST of entity records under a root compound as "Entities"
 * and parse it back, exactly as a chunk save + reload does. */
static nbt_node* disk_roundtrip(nbt_node* list) {
	char* saved_name = list->name;
	list->name = "Entities";
	struct nbt_list kids, w;
	INIT_LIST_HEAD(&kids.entry);
	w.data = list;
	list_add_tail(&w.entry, &kids.entry);
	nbt_node root = {.type = TAG_COMPOUND, .name = "",
					 .payload.tag_compound = &kids};
	struct buffer b = nbt_dump_binary(&root);
	list->name = saved_name;
	assert(b.data);
	nbt_node* disk = nbt_parse(b.data, b.len);
	buffer_free(&b);
	assert(disk && child(disk, "Entities"));
	return disk;
}

/* ---- tests ----------------------------------------------------------- */

static void test_entity_types(struct nbt_list** keep_list, void** keep_arena) {
	struct archived_entity ents[64];
	size_t n = make_entities(ents);
	struct chunk_entities ce = {.entities = ents, .count = n};
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&ce, &arena);
	assert(list && arena);
	nbt_node node;
	wrap_list(&node, list);
	for(size_t i = 0; i < n; i++) {
		nbt_node* rec = record_at(&node, i);
		assert(rec);
		const char* rid = rec_id(rec);
		check_compound(rec, false, rid);
		/* Entity.readFromNBT reads these for every entity. */
		require(rec, rid, "Pos"); require(rec, rid, "Motion");
		require(rec, rid, "Rotation"); require(rec, rid, "Fire");
		require(rec, rid, "OnGround"); require(rec, rid, "FallDistance");
		if(ents[i].kind == ARCHIVED_ENTITY_MONSTER) {
			require(rec, rid, "Health"); require(rec, rid, "HurtTime");
			require(rec, rid, "DeathTime"); require(rec, rid, "AttackTime");
			require(rec, rid, "Air");
			int mid = ents[i].data.monster.monster_id;
			if(mid == MONSTER_SHEEP) {
				require(rec, rid, "Sheared"); require(rec, rid, "Color");
				assert(child(rec, "Color")->payload.tag_byte == 14);
				assert(child(rec, "Sheared")->payload.tag_byte == 1);
			}
			if(mid == MONSTER_SLIME_SMALL || mid == MONSTER_SLIME_MEDIUM
			   || mid == MONSTER_SLIME_BIG) {
				require(rec, rid, "Size");
				const nbt_node* size = child(rec, "Size");
				/* Beta Size = getSlimeSize() - 1, sizes 1/2/4. */
				assert(size && size->payload.tag_int
					== (mid == MONSTER_SLIME_SMALL ? 0
						: mid == MONSTER_SLIME_MEDIUM ? 1 : 3));
			}
		}
		if(ents[i].kind == ARCHIVED_ENTITY_ITEM) {
			require(rec, rid, "Health"); require(rec, rid, "Age");
			require(rec, rid, "Item"); require(rec, rid, "Air");
		}
	}
	/* Round trip through the production reader. */
	struct chunk_entities back = {0};
	assert(chunk_entities_parse(&node, 0, 0, &back));
	assert(back.count == n);
	assert(back.entities[0].data.item.age == 1234);
	assert(back.entities[1].data.monster.hurt_time == 7);
	assert(back.entities[1].data.monster.attack_time == 11);
	assert(back.entities[1 + MONSTER_SHEEP].data.monster.fleece_color == 14);
	assert(back.entities[1 + MONSTER_SHEEP].data.monster.sheared);
	assert(back.entities[1 + MONSTER_SLIME_BIG].data.monster.monster_id
		== MONSTER_SLIME_BIG);
	assert(back.entities[1 + MONSTER_SLIME_MEDIUM].data.monster.monster_id
		== MONSTER_SLIME_MEDIUM);
	chunk_entities_free(&back);
	*keep_list = list;
	*keep_arena = arena;
}

static void test_tile_types(struct nbt_list** keep_list, void** keep_arena) {
	struct te_chest chest = {.x = 1, .y = 64, .z = 1};
	chest.items[0] = (struct item_data) {.id = 1, .count = 2};
	struct te_sign sign = {.x = 2, .y = 64, .z = 2};
	strcpy(sign.text, "hello");
	struct te_furnace furnace = {.x = 3, .y = 64, .z = 3, .burn_time = 100,
								 .cook_time = 50};
	furnace.items[1] = (struct item_data) {.id = 263, .count = 1};
	struct te_dispenser disp = {.x = 4, .y = 64, .z = 4};
	disp.items[0] = (struct item_data) {.id = 262, .count = 9};
	struct te_note note = {.x = 5, .y = 64, .z = 5, .note = 3};
	struct te_record_player rec = {.x = 6, .y = 64, .z = 6, .record = 2256};
	struct te_spawner sp = {.x = 7, .y = 20, .z = 7, .delay = 20};
	strcpy(sp.mob_id, "Zombie");
	struct te_piston_moving pm = {.x = 8, .y = 64, .z = 8, .stored_id = 1,
								  .facing = 2, .progress = 0.5f,
								  .extending = 1};
	struct chunk_tile_entities te = {
		.chests = &chest, .n_chest = 1, .signs = &sign, .n_sign = 1,
		.furnaces = &furnace, .n_furnace = 1, .dispensers = &disp,
		.n_dispenser = 1, .notes = &note, .n_note = 1,
		.record_players = &rec, .n_record_player = 1, .spawners = &sp,
		.n_spawner = 1, .piston_movings = &pm, .n_piston_moving = 1,
	};
	void* arena = NULL;
	struct nbt_list* list = tile_entities_build(&te, 0, 0, &arena);
	assert(list && arena);
	nbt_node node;
	wrap_list(&node, list);
	size_t count = 0;
	struct list_head* p;
	list_for_each(p, &list->entry) {
		const nbt_node* r = list_entry(p, struct nbt_list, entry)->data;
		const char* rid = rec_id(r);
		check_compound(r, false, rid);
		require(r, rid, "x"); require(r, rid, "y"); require(r, rid, "z");
		count++;
	}
	assert(count == 8);
	*keep_list = list;
	*keep_arena = arena;
}

/* Pre-fix CaveWii saves wrote Age / AttackTime / HurtTime / Air / DeathTime
 * as TAG_INT. They must still load with their values, and the native save
 * path must still be able to drop them (pickup/death) instead of failing the
 * chunk save closed. */
static void test_legacy_int_blob(void) {
	struct archived_entity ents[4];
	ents[0] = base(ARCHIVED_ENTITY_ITEM, 7);
	ents[0].health = 5;
	ents[0].data.item.item = (struct item_data) {.id = 4, .count = 3};
	ents[0].data.item.age = 4321;
	ents[1] = base(ARCHIVED_ENTITY_MONSTER, 8);
	ents[1].data.monster.monster_id = MONSTER_ZOMBIE;
	ents[1].data.monster.fuse = -1;
	ents[1].data.monster.air = -5;
	ents[1].data.monster.hurt_time = 6;
	ents[1].data.monster.death_time = 3;
	ents[1].data.monster.attack_time = 13;
	ents[2] = base(ARCHIVED_ENTITY_MONSTER, 9);
	ents[2].data.monster.monster_id = MONSTER_SHEEP;
	ents[2].data.monster.fuse = -1;
	ents[2].data.monster.air = 300;
	ents[2].data.monster.fleece_color = 14;
	ents[2].data.monster.sheared = true;
	ents[3] = base(ARCHIVED_ENTITY_MONSTER, 10);
	ents[3].data.monster.monster_id = MONSTER_SLIME_BIG;
	ents[3].data.monster.fuse = -1;
	ents[3].data.monster.air = 300;
	struct chunk_entities ce = {.entities = ents, .count = 4};
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&ce, &arena);
	assert(list);
	nbt_node node;
	wrap_list(&node, list);
	const char* item_tags[] = {"Age"};
	const char* mob_tags[] = {"AttackTime", "HurtTime", "Air", "DeathTime"};
	for(size_t i = 0; i < 1; i++) {
		nbt_node* t = child(record_at(&node, 0), item_tags[i]);
		assert(t && t->type == TAG_SHORT);
		int32_t v = t->payload.tag_short;
		t->type = TAG_INT; t->payload.tag_int = v;
	}
	for(size_t r = 1; r < 4; r++) {
		nbt_node* rec = record_at(&node, r);
		for(size_t i = 0; i < 4; i++) {
			nbt_node* t = child(rec, mob_tags[i]);
			assert(t && t->type == TAG_SHORT);
			int32_t v = t->payload.tag_short;
			t->type = TAG_INT; t->payload.tag_int = v;
		}
		/* Pre-fix records had only CaveWiiFleeceColor/CaveWiiSheared and the
		 * CaveWiiMonsterId-encoded slime size: drop the Beta tags (the arena
		 * still owns the unlinked nodes). */
		const char* beta_only[] = {"Color", "Sheared", "Size"};
		for(size_t i = 0; i < 3; i++) {
			struct list_head* p;
			list_for_each(p, &rec->payload.tag_compound->entry) {
				nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
				if(n && n->name && !strcmp(n->name, beta_only[i])) {
					list_del(p);
					break;
				}
			}
			assert(!child(rec, beta_only[i]));
		}
	}
	assert(child(record_at(&node, 2), "CaveWiiFleeceColor")->payload.tag_byte
		== 14);
	/* Serialize + reparse so the blob is a real on-disk old-format record. */
	node.name = "Entities";
	struct nbt_list root_kids, w;
	INIT_LIST_HEAD(&root_kids.entry);
	w.data = &node;
	list_add_tail(&w.entry, &root_kids.entry);
	nbt_node root = {.type = TAG_COMPOUND, .name = "",
					 .payload.tag_compound = &root_kids};
	struct buffer b = nbt_dump_binary(&root);
	assert(b.data);
	nbt_node* disk = nbt_parse(b.data, b.len);
	buffer_free(&b);
	assert(disk);
	nbt_node* disk_list = child(disk, "Entities");
	assert(disk_list && disk_list->type == TAG_LIST);
	assert(child(record_at(disk_list, 0), "Age")->type == TAG_INT);
	assert(child(record_at(disk_list, 1), "HurtTime")->type == TAG_INT);
	struct chunk_entities back = {0};
	assert(chunk_entities_parse(disk_list, 0, 0, &back));
	assert(back.count == 4);
	assert(back.entities[0].data.item.age == 4321);
	assert(back.entities[2].data.monster.monster_id == MONSTER_SHEEP);
	assert(back.entities[2].data.monster.fleece_color == 14);
	assert(back.entities[2].data.monster.sheared);
	assert(back.entities[3].data.monster.monster_id == MONSTER_SLIME_BIG);
	assert(back.entities[1].data.monster.air == -5);
	assert(back.entities[1].data.monster.hurt_time == 6);
	assert(back.entities[1].data.monster.death_time == 3);
	assert(back.entities[1].data.monster.attack_time == 13);

	/* Native snapshot where both left the chunk: the legacy rows must be
	 * deletion-safe (no CEP_NATIVE_UNSAFE fail-closed save). */
	struct nbt_list empty_sentinel;
	INIT_LIST_HEAD(&empty_sentinel.entry);
	nbt_node empty_type = {.type = TAG_COMPOUND};
	empty_sentinel.data = &empty_type;
	nbt_node generated;
	wrap_list(&generated, &empty_sentinel);
	struct chunk_entities none = {.entities = NULL, .count = 0};
	chunk_entities_preserve_test_reset_fail();
	nbt_node* merged = chunk_entities_preserve(disk_list, &generated, &none,
											   true, 0, 0);
	if(!merged)
		fprintf(stderr, "legacy delete failed: preserve fail code %d\n",
				(int)chunk_entities_preserve_last_fail());
	assert(merged);
	assert(list_empty(&merged->payload.tag_list->entry));
	nbt_free(merged);

	/* Native snapshot where both are still present: the merge must rewrite
	 * the legacy ints to Beta shorts (migration on first save). */
	struct chunk_entities same = {.entities = back.entities, .count = 4};
	void* arena2 = NULL;
	struct nbt_list* fresh = chunk_entities_build(&same, &arena2);
	assert(fresh);
	nbt_node fresh_node;
	wrap_list(&fresh_node, fresh);
	merged = chunk_entities_preserve(disk_list, &fresh_node, &same, true, 0, 0);
	assert(merged);
	int before = s_fail;
	for(size_t i = 0; i < 4; i++)
		check_compound(record_at(merged, i), false, rec_id(record_at(merged, i)));
	assert(s_fail == before);
	/* First save migrates: the sheep/slime rows now carry Beta's own tags. */
	assert(child(record_at(merged, 2), "Color")
		&& child(record_at(merged, 2), "Color")->payload.tag_byte == 14);
	assert(child(record_at(merged, 2), "Sheared")
		&& child(record_at(merged, 2), "Sheared")->payload.tag_byte == 1);
	assert(child(record_at(merged, 3), "Size")
		&& child(record_at(merged, 3), "Size")->payload.tag_int == 3);
	nbt_free(merged);

	/* Review r1: the same legacy rows in a native chunk that ALSO holds an
	 * opaque (unparseable) row. The complete fast path is defeated, so the
	 * preserving merge rewrites the legacy rows in place; a later save where
	 * the sheep and slime have gone must still delete them (no
	 * CEP_NATIVE_UNSAFE) and keep the opaque row. */
	nbt_node* opaque = nbt_clone(record_at(disk_list, 0));
	assert(opaque);
	{
		struct list_head *p, *nx;
		list_for_each_safe(p, nx, &opaque->payload.tag_compound->entry) {
			struct nbt_list* ow = list_entry(p, struct nbt_list, entry);
			if(ow->data && ow->data->name
			   && (!strcmp(ow->data->name, "CaveWiiType") || !strcmp(ow->data->name, "CaveWiiRuntimeId"))) {
				list_del(p); nbt_free(ow->data); free(ow);
			}
		}
		nbt_node* oid = child(opaque, "id");
		free(oid->payload.tag_string);
		oid->payload.tag_string = malloc(sizeof("ModEntity"));
		strcpy(oid->payload.tag_string, "ModEntity");
		struct nbt_list* ow = malloc(sizeof(*ow));
		ow->data = opaque;
		list_add_tail(&ow->entry, &disk_list->payload.tag_list->entry);
	}
	struct chunk_entities probe = {0};
	assert(chunk_entities_parse(disk_list, 0, 0, &probe));
	assert(probe.count == 4); /* the opaque row is not admitted */
	chunk_entities_free(&probe);
	chunk_entities_preserve_test_reset_fail();
	nbt_node* save1 = chunk_entities_preserve(disk_list, &fresh_node, &same,
											  true, 0, 0);
	if(!save1)
		fprintf(stderr, "opaque save1 failed: code %d\n",
				(int)chunk_entities_preserve_last_fail());
	assert(save1);
	/* The real chunk save dumps and re-parses between saves (the merge can
	 * leave lazily-encoded item payloads that only exist as bytes). */
	nbt_node* disk1 = disk_roundtrip(save1);
	nbt_free(save1);
	save1 = child(disk1, "Entities");
	/* The sheep and slime leave; item and zombie stay. */
	struct chunk_entities remain = {.entities = back.entities, .count = 2};
	void* arena3 = NULL;
	struct nbt_list* gen2 = chunk_entities_build(&remain, &arena3);
	assert(gen2);
	nbt_node gen2_node;
	wrap_list(&gen2_node, gen2);
	chunk_entities_preserve_test_reset_fail();
	nbt_node* save2 = chunk_entities_preserve(save1, &gen2_node, &remain,
											  true, 0, 0);
	if(!save2)
		fprintf(stderr, "opaque save2 failed: code %d\n",
				(int)chunk_entities_preserve_last_fail());
	assert(save2);
	assert(chunk_entities_preserve_last_fail() == CEP_NONE);
	size_t rows = 0, sheep = 0, slime = 0, mod = 0;
	struct list_head* rp;
	list_for_each(rp, &save2->payload.tag_list->entry) {
		const char* rid = rec_id(list_entry(rp, struct nbt_list, entry)->data);
		rows++;
		sheep += !strcmp(rid, "Sheep");
		slime += !strcmp(rid, "Slime");
		mod += !strcmp(rid, "ModEntity");
	}
	if(!(rows == 3 && sheep == 0 && slime == 0 && mod == 1))
		fprintf(stderr, "save2 rows=%zu sheep=%zu slime=%zu mod=%zu\n", rows, sheep, slime, mod);
	assert(rows == 3 && sheep == 0 && slime == 0 && mod == 1);
	nbt_free(save2);
	nbt_free(disk1);
	chunk_entities_build_free(arena3);
	nbt_free(disk);
	chunk_entities_build_free(arena2);
	chunk_entities_free(&back);
	chunk_entities_build_free(arena);
}

static void unlink_named(nbt_node* rec, const char* name) {
	struct list_head *p, *next;
	list_for_each_safe(p, next, &rec->payload.tag_compound->entry) {
		nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(n && n->name && !strcmp(n->name, name))
			list_del(p);
	}
}

/* Beta's own tags win over the CaveWii fallbacks, and a vanilla slime record
 * (no CaveWii markers) takes its size from Beta's "Size". */
static void test_beta_tags_win(void) {
	struct archived_entity ents[2];
	ents[0] = base(ARCHIVED_ENTITY_MONSTER, 0);
	ents[0].data.monster.monster_id = MONSTER_SHEEP;
	ents[0].data.monster.fuse = -1;
	ents[0].data.monster.air = 300;
	ents[0].data.monster.fleece_color = 14;
	ents[1] = base(ARCHIVED_ENTITY_MONSTER, 0);
	ents[1].data.monster.monster_id = MONSTER_SLIME_MEDIUM;
	ents[1].data.monster.fuse = -1;
	ents[1].data.monster.air = 300;
	struct chunk_entities ce = {.entities = ents, .count = 2};
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&ce, &arena);
	assert(list);
	nbt_node node;
	wrap_list(&node, list);
	/* Disagreeing CaveWii copy: Beta's Color (14) must win. */
	child(record_at(&node, 0), "CaveWiiFleeceColor")->payload.tag_byte = 2;
	/* Vanilla-shaped slime: no CaveWii type/id markers at all. */
	unlink_named(record_at(&node, 1), "CaveWiiType");
	unlink_named(record_at(&node, 1), "CaveWiiMonsterId");
	struct chunk_entities back = {0};
	assert(chunk_entities_parse(&node, 0, 0, &back));
	assert(back.count == 2);
	assert(back.entities[0].data.monster.fleece_color == 14);
	assert(back.entities[1].data.monster.monster_id == MONSTER_SLIME_MEDIUM);
	chunk_entities_free(&back);
	chunk_entities_build_free(arena);
}

/* Out-of-range runtime values saturate instead of failing the chunk save or
 * wrapping (see add_short_clamped). */
static void test_short_clamp(void) {
	struct archived_entity e = base(ARCHIVED_ENTITY_ITEM, 9);
	e.health = 5;
	e.data.item.item = (struct item_data) {.id = 4, .count = 1};
	e.data.item.age = 100000;
	struct chunk_entities ce = {.entities = &e, .count = 1};
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&ce, &arena);
	assert(list);
	nbt_node node;
	wrap_list(&node, list);
	nbt_node* age = child(record_at(&node, 0), "Age");
	assert(age && age->type == TAG_SHORT && age->payload.tag_short == 32767);
	chunk_entities_build_free(arena);
}

int main(int argc, char** argv) {
	struct nbt_list *ents, *tes;
	void *ea, *ta;
	test_entity_types(&ents, &ea);
	test_tile_types(&tes, &ta);
	if(argc > 1)
		dump_world(argv[1], ents, tes);
	chunk_entities_build_free(ea);
	tile_entities_build_free(ta);
	if(s_fail) {
		fprintf(stderr, "test_entity_nbt_beta_types: %d Beta tag-type "
				"violation(s)\n", s_fail);
		return 1;
	}
	test_legacy_int_blob();
	test_short_clamp();
	test_beta_tags_win();
	printf("test_entity_nbt_beta_types: PASS (%d Beta-named tags checked)\n",
		   s_checked);
	return 0;
}
