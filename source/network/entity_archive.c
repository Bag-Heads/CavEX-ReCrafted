/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "entity_archive.h"
#include "../item/item_nbt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../block/blocks_data.h" // BLOCK_SAND / BLOCK_GRAVEL (Tile validation)
#include "../cNBT/list.h"
#include "../entity/combat.h"
#include "../entity/entity_primed_tnt.h"
#include "../entity/monsters_object.h"

#define ARCHIVED_ITEM_AGE_MAX (5 * 60 * 20)
#define ARCHIVED_ENTITY_AGE_MAX (INT32_MAX / 2)
#define ARCHIVED_DIRECTION_TIME_MAX 59
#define ARCHIVED_JUMP_DELAY_MAX 30
#define ARCHIVED_MINECART_SPEED_MAX 100.0f
#define ARCHIVED_ENTITY_CHILD_MAX 128

/* Exact builder footprints keep the transient Wii MEM1 arena proportional to
 * record kind: a block-backed TNT needs far less state than a monster. Every
 * plain tag added to build_entity costs +1 node and +1 wrap in its kind's row
 * (a vector adds 1 + its element count) -- an undercount makes the build fail,
 * which the M4 animal env test's disk round trip would catch. */
static size_t entity_node_count(enum archived_entity_kind kind) {
	switch(kind) {
	case ARCHIVED_ENTITY_ITEM: return 30;
	/* Wolf adds the three exact Beta tags Angry, Sitting and Owner. Weather
	 * slice adds ONE plain tag: "powered" (creeper charged); the saddle slice adds ONE more:
	 * "Saddle" (pig saddled). MERGE NOTE: both slices edited this switch from
	 * different bases -- 63 is minecart-era 62 + saddle's tag; MINECART stays
	 * at the minecart slice's hand-counted 169. Taking either side wholesale
	 * would silently break chunk eviction for the other entity type. */
	/* java-load-1023: +2 for the Beta-named sheep pair (Color, Sheared);
	 * the slime's single "Size" is the other per-species maximum (1). */
	case ARCHIVED_ENTITY_MONSTER: return 65;
	/* minecart-slice budget, hand-counted against build_entity's actual calls
	 * (verify by re-deriving, never by running the builder -- CODESTYLE.md
	 * S4.2): the pre-existing 28 (6 common Entity tags -> 22 nodes + 6 plain
	 * CaveWiiXxx minecart tags) + 4 new plain tags (Type/PushX/PushZ/Fuel: +4)
	 * + the "Items" TAG_LIST worst case (a full 27-slot chest cart): list
	 * header 2 (the list node + its element-type sentinel node) + 27 * (1
	 * compound node + 4 leaf nodes [Slot/id/Count/Damage]) = 2 + 135 = 137.
	 * Total: 28 + 4 + 137 = 169. */
	case ARCHIVED_ENTITY_MINECART: return 170; /* +1: CaveWiiBlastDamage */
	case ARCHIVED_ENTITY_ARROW: return 36;
	/* Egg = the common Entity block + 2 plain tags (owner, age) -- the same
	 * shape as PrimedTnt's Fuse + ExplosionPending pair. The snowball is the
	 * same record with a different id string. */
	case ARCHIVED_ENTITY_EGG: return 24;
	case ARCHIVED_ENTITY_SNOWBALL: return 24;
	case ARCHIVED_ENTITY_FIREBALL: return 24;
	case ARCHIVED_ENTITY_PRIMED_TNT: return 24;
	/* Common block ONLY -- see ARCHIVED_ENTITY_BOAT's comment for why a
	 * boat has zero kind-specific tags (unlike every other kind above). */
	case ARCHIVED_ENTITY_BOAT: return 23; /* +1: CaveWiiBlastDamage */
	/* Painting-slice budget, hand-counted against build_entity's actual calls
	 * (re-derived, never run through the builder -- CODESTYLE.md S4.2): the
	 * common Entity block is 22 nodes (re-verified against TWO existing
	 * rows before trusting the method, per the batch-f landing brief -- the
	 * EGG row's declared 24 = 22 + its own 2 plain tags, and the ARROW row's
	 * declared 36 = 22 + its own 14; ARCHIVED_ENTITY_BOAT's own declared 22
	 * with ZERO kind-specific tags directly confirms the common block is
	 * exactly 22 nodes, independent of the egg/arrow cross-check) + 5 plain
	 * painting tags (Dir/Motive/TileX/TileY/TileZ, 1 node each). Total:
	 * 22 + 5 = 27. */
	case ARCHIVED_ENTITY_PAINTING: return 27;
	/* Batch I: re-derived against TWO existing rows before trusting the
	 * method (the same discipline PAINTING's own comment above documents):
	 * EGG's declared 24 = 22 + its own 2 plain tags, and BOAT's declared 22
	 * = 22 + 0, both confirm the common Entity block is exactly 22 nodes.
	 * FallingSand writes exactly ONE plain tag ("Tile"). Total: 22 + 1 = 23. */
	case ARCHIVED_ENTITY_FALLING_SAND: return 23;
	default: return 0;
	}
}

static size_t entity_wrap_count(enum archived_entity_kind kind) {
	switch(kind) {
	case ARCHIVED_ENTITY_ITEM: return 32;
	case ARCHIVED_ENTITY_MONSTER: return 66; /* +1: saddle slice's "Saddle" tag; +2: sheep Color/Sheared */
	/* Mirrors entity_node_count's derivation: pre-existing 29 + 4 new plain
	 * tags (Type/PushX/PushZ/Fuel: +4) + "Items" worst case: list header 2
	 * (sentinel wrap + the link wrapping the list into its parent) + 27 * (2
	 * compound wraps [its own kids-sentinel + its link into the Items
	 * sentinel] + 4 leaf wraps) = 2 + 27*6 = 164. Total: 29 + 4 + 164 = 197. */
	case ARCHIVED_ENTITY_MINECART: return 198; /* +1 wrap: CaveWiiBlastDamage */
	case ARCHIVED_ENTITY_ARROW: return 37;
	case ARCHIVED_ENTITY_EGG: return 25;
	case ARCHIVED_ENTITY_SNOWBALL: return 25;
	case ARCHIVED_ENTITY_FIREBALL: return 25;
	case ARCHIVED_ENTITY_PRIMED_TNT: return 25;
	/* Common block ONLY -- matches entity_node_count's 22 above (23 = the
	 * compound's extra wrap; see build_entity's common-prefix recount in
	 * the node-count comment). */
	case ARCHIVED_ENTITY_BOAT: return 24; /* +1 wrap: CaveWiiBlastDamage */
	/* Mirrors entity_node_count's derivation: common Entity block is 23
	 * wraps (EGG's declared 25 = 23 + 2; ARROW's declared 37 = 23 + 14;
	 * BOAT's own declared 23 with zero kind-specific tags directly confirms
	 * it) + 5 plain painting tags (1 wrap each). Total: 23 + 5 = 28. */
	case ARCHIVED_ENTITY_PAINTING: return 28;
	/* Mirrors entity_node_count's derivation: common Entity block is 23 wraps
	 * (EGG's declared 25 = 23 + 2; BOAT's declared 23 = 23 + 0) + 1 plain tag.
	 * Total: 23 + 1 = 24. */
	case ARCHIVED_ENTITY_FALLING_SAND: return 24;
	default: return 0;
	}
}

struct entity_nbt_arena {
	nbt_node* nodes;
	struct nbt_list* wraps;
	size_t nodes_cap, wraps_cap;
	size_t ni, nw;
};

static nbt_node* node_alloc(struct entity_nbt_arena* a) {
	if(a->ni >= a->nodes_cap)
		return NULL;
	nbt_node* n = &a->nodes[a->ni++];
	memset(n, 0, sizeof(*n));
	return n;
}

static struct nbt_list* wrap_alloc(struct entity_nbt_arena* a) {
	if(a->nw >= a->wraps_cap)
		return NULL;
	struct nbt_list* w = &a->wraps[a->nw++];
	memset(w, 0, sizeof(*w));
	INIT_LIST_HEAD(&w->entry);
	return w;
}

static bool link_child(struct entity_nbt_arena* a, struct nbt_list* parent,
					   nbt_node* node) {
	struct nbt_list* w = wrap_alloc(a);
	if(!parent || !node || !w)
		return false;
	w->data = node;
	list_add_tail(&w->entry, &parent->entry);
	return true;
}

static nbt_node* make_num(struct entity_nbt_arena* a, nbt_type type,
						  const char* name, double value) {
	nbt_node* n = node_alloc(a);
	if(!n)
		return NULL;
	n->type = type;
	n->name = (char*)name;
	switch(type) {
	case TAG_BYTE:
		if(!isfinite(value) || trunc(value) != value || value < INT8_MIN
		   || value > UINT8_MAX)
			return NULL;
		if(value > INT8_MAX)
			value -= (double)UINT8_MAX + 1.0;
		n->payload.tag_byte = (int8_t)value;
		break;
	case TAG_SHORT:
		if(!isfinite(value) || trunc(value) != value || value < INT16_MIN
		   || value > UINT16_MAX)
			return NULL;
		if(value > INT16_MAX)
			value -= (double)UINT16_MAX + 1.0;
		n->payload.tag_short = (int16_t)value;
		break;
	case TAG_INT:
		if(!isfinite(value) || trunc(value) != value || value < INT32_MIN
		   || value > INT32_MAX)
			return NULL;
		n->payload.tag_int = (int32_t)value;
		break;
	case TAG_FLOAT:
		n->payload.tag_float = (float)value;
		if(!isfinite(value) || !isfinite(n->payload.tag_float))
			return NULL;
		break;
	case TAG_DOUBLE:
		if(!isfinite(value))
			return NULL;
		n->payload.tag_double = value;
		break;
	default: return NULL;
	}
	return n;
}

static nbt_node* make_u32(struct entity_nbt_arena* a, const char* name,
						  uint32_t value) {
	nbt_node* n = node_alloc(a);
	if(!n)
		return NULL;
	n->type = TAG_LONG;
	n->name = (char*)name;
	n->payload.tag_long = (int64_t)value;
	return n;
}

static nbt_node* make_string(struct entity_nbt_arena* a, const char* name,
							 const char* value) {
	nbt_node* n = node_alloc(a);
	if(!n)
		return NULL;
	n->type = TAG_STRING;
	n->name = (char*)name;
	n->payload.tag_string = (char*)value;
	return n;
}

static bool add_num(struct entity_nbt_arena* a, struct nbt_list* parent,
					nbt_type type, const char* name, double value) {
	return link_child(a, parent, make_num(a, type, name, value));
}

/* java-load-1023: a Beta-NAMED tag that Beta reads with getShort MUST be
 * written as TAG_SHORT -- NBTTagCompound.getShort casts to NBTTagShort, so a
 * TAG_INT throws ClassCastException in Entity.readFromNBT and vanilla drops
 * every entity of that chunk. Beta writes these with a truncating (short) cast
 * [EntityLiving.java:558-561 setShort("HurtTime"/"DeathTime"/"AttackTime",
 * (short)...), Entity.java:756 setShort("Air", (short)this.air),
 * EntityItem.java:99 setShort("Age", (short)this.age)]. Every runtime value
 * this codec stores is already far inside the short range (Age <= 6000,
 * |Air| <= 300, timers <= 20), so the clamp below is a guard, not a value
 * change: an out-of-range runtime value saturates instead of wrapping (Beta
 * would wrap; saturating keeps the sign and order, and add_num would otherwise
 * reject the value and fail the whole chunk's entity archive). Readers accept
 * TAG_INT too (read_int), so pre-fix CaveWii saves keep loading. */
static bool add_short_clamped(struct entity_nbt_arena* a, struct nbt_list* parent,
							  const char* name, int32_t value) {
	if(value < INT16_MIN)
		value = INT16_MIN;
	else if(value > INT16_MAX)
		value = INT16_MAX;
	return add_num(a, parent, TAG_SHORT, name, (double)value);
}

static bool add_u32(struct entity_nbt_arena* a, struct nbt_list* parent,
					const char* name, uint32_t value) {
	return link_child(a, parent, make_u32(a, name, value));
}

static bool add_string(struct entity_nbt_arena* a, struct nbt_list* parent,
					   const char* name, const char* value) {
	return link_child(a, parent, make_string(a, name, value));
}

static bool add_compound(struct entity_nbt_arena* a, struct nbt_list* parent,
						 const char* name, struct nbt_list** kids_out) {
	nbt_node* n = node_alloc(a);
	struct nbt_list* kids = wrap_alloc(a);
	if(!n || !kids)
		return false;
	n->type = TAG_COMPOUND;
	n->name = (char*)name;
	kids->data = NULL;
	n->payload.tag_compound = kids;
	if(!link_child(a, parent, n))
		return false;
	*kids_out = kids;
	return true;
}

static bool add_vector(struct entity_nbt_arena* a, struct nbt_list* parent,
					   const char* name, nbt_type type, const float* values,
					   size_t count) {
	nbt_node* list = node_alloc(a);
	struct nbt_list* sentinel = wrap_alloc(a);
	nbt_node* elem_type = node_alloc(a);
	if(!list || !sentinel || !elem_type)
		return false;
	elem_type->type = type;
	sentinel->data = elem_type;
	list->type = TAG_LIST;
	list->name = (char*)name;
	list->payload.tag_list = sentinel;
	if(!link_child(a, parent, list))
		return false;
	for(size_t i = 0; i < count; i++)
		if(!link_child(a, sentinel, make_num(a, type, NULL, values[i])))
			return false;
	return true;
}

/* save-enc-1030: add_vector's shape for three exact doubles (Beta
 * newDoubleNBTList) -- the same 5 nodes / 4 wraps, so the per-kind budget
 * is unchanged. */
static bool add_vector_d(struct entity_nbt_arena* a, struct nbt_list* parent,
						 const char* name, const double values[3]) {
	nbt_node* list = node_alloc(a);
	struct nbt_list* sentinel = wrap_alloc(a);
	nbt_node* elem_type = node_alloc(a);
	if(!list || !sentinel || !elem_type)
		return false;
	elem_type->type = TAG_DOUBLE;
	sentinel->data = elem_type;
	list->type = TAG_LIST;
	list->name = (char*)name;
	list->payload.tag_list = sentinel;
	if(!link_child(a, parent, list))
		return false;
	for(size_t i = 0; i < 3; i++)
		if(!link_child(a, sentinel, make_num(a, TAG_DOUBLE, NULL, values[i])))
			return false;
	return true;
}

/* The doubles are the exact source of the float fields only while their float
 * images still equal them; a float written after the doubles (a caller that
 * moved the entity through pos/vel) wins and is widened. */
static bool doubles_current(bool has, const double d[3], const float* f) {
	return has && (float)d[0] == f[0] && (float)d[1] == f[1]
		&& (float)d[2] == f[2];
}

static bool add_item(struct entity_nbt_arena* a, struct nbt_list* parent,
					 const char* name, struct item_data item) {
	if(item.nbt_record) {
		nbt_node* raw = node_alloc(a);
		if(!raw || !item_nbt_emit(raw,&item,-1)) return false;
		raw->name = (char*)name;
		return link_child(a,parent,raw);
	}
	struct nbt_list* kids;
	return add_compound(a, parent, name, &kids)
		&& add_num(a, kids, TAG_SHORT, "id", item.id)
		&& add_num(a, kids, TAG_BYTE, "Count", item.count)
		&& add_num(a, kids, TAG_SHORT, "Damage", item.durability);
}

/* Beta EntityMinecart.writeEntityToNBT's "Items" list [740c583]: an unnamed
 * TAG_COMPOUND per OCCUPIED slot (a slot with id==0 is skipped, exactly like
 * Beta's `if (this.cargoItems[var3] != null)`), each carrying "Slot" (the
 * index, TAG_BYTE) plus the item's own id/Count/Damage (add_item's shape,
 * inlined here since add_item's compound is NAMED and a list element must
 * not be). Mirrors add_vector's TAG_LIST-of-scalars shape, but the sentinel's
 * element-type node is TAG_COMPOUND and each element is itself built through
 * add_compound instead of make_num. entity_node_count/entity_wrap_count size
 * the caller's arena for the WORST CASE (every slot occupied) since the
 * budget is fixed per entity KIND, not per instance. */
static bool add_item_list(struct entity_nbt_arena* a, struct nbt_list* parent,
						  const char* name, const struct item_data* items,
						  size_t count, bool legacy_footprint) {
	nbt_node* list = node_alloc(a);
	struct nbt_list* sentinel = wrap_alloc(a);
	nbt_node* elem_type = node_alloc(a);
	if(!list || !sentinel || !elem_type)
		return false;
	elem_type->type = TAG_COMPOUND;
	sentinel->data = elem_type;
	list->type = TAG_LIST;
	list->name = (char*)name;
	list->payload.tag_list = sentinel;
	if(!link_child(a, parent, list))
		return false;
	for(size_t i = 0; i < count; i++) {
		if(items[i].id == 0 || items[i].count == 0)
			continue;
		if(items[i].nbt_record) {
			nbt_node* raw = node_alloc(a);
			if(!raw || !item_nbt_emit(raw,&items[i],(int)i)
			   || !link_child(a,sentinel,raw)) return false;
			continue;
		}
		struct nbt_list* kids;
		if(!add_compound(a, sentinel, NULL, &kids)
		   || !add_num(a, kids, TAG_BYTE, "Slot", (double)i)
		   || !add_num(a, kids, TAG_SHORT, "id", items[i].id)
		   || !add_num(a, kids, TAG_BYTE, "Count", items[i].count)
		   || !add_num(a, kids, TAG_SHORT, "Damage", items[i].durability))
			return false;
	}
	/* save-enc-1030: Beta NBTTagList.writeTagContents writes element type 1
	 * (TAG_BYTE) for an EMPTY list [McBetaCpp/srcorigjava NBTTagList.java:
	 * 25-31]; cNBT dumps an empty list with its sentinel's type, so an empty
	 * cart inventory points the sentinel at TAG_BYTE. The legacy footprint
	 * (pre-save-enc-1030 records, entity_native_deletion_safe only) keeps
	 * the old TAG_COMPOUND. */
	if(!legacy_footprint && list_empty(&sentinel->entry))
		elem_type->type = TAG_BYTE;
	return true;
}

static const char* entity_name(const struct archived_entity* e) {
	switch(e->kind) {
	case ARCHIVED_ENTITY_ITEM: return "Item";
	case ARCHIVED_ENTITY_MINECART: return "Minecart";
	case ARCHIVED_ENTITY_ARROW: return "Arrow";
	case ARCHIVED_ENTITY_EGG: return "Egg";
	case ARCHIVED_ENTITY_SNOWBALL: return "Snowball";
	case ARCHIVED_ENTITY_FIREBALL: return "Fireball";
	case ARCHIVED_ENTITY_PRIMED_TNT: return "PrimedTnt";
	case ARCHIVED_ENTITY_BOAT: return "Boat"; /* Beta's OWN EntityList
		addMapping(EntityBoat.class, "Boat", 41) [740c583] */
	case ARCHIVED_ENTITY_PAINTING: return "Painting";
	case ARCHIVED_ENTITY_FALLING_SAND: return "FallingSand";
	case ARCHIVED_ENTITY_MONSTER:
		switch(e->data.monster.monster_id) {
		case MONSTER_ZOMBIE: return "Zombie";
		case MONSTER_CREEPER: return "Creeper";
		case MONSTER_SKELETON: return "Skeleton";
		case MONSTER_SPIDER: return "Spider";
		case MONSTER_PIG: return "Pig";
		case MONSTER_COW: return "Cow";
		case MONSTER_CHICKEN: return "Chicken";
		case MONSTER_SHEEP: return "Sheep";
		case MONSTER_SQUID: return "Squid";
		case MONSTER_PIG_ZOMBIE: return "PigZombie"; /* Beta EntityList */
		case MONSTER_GHAST: return "Ghast";
		case MONSTER_WOLF: return "Wolf";
		case MONSTER_GIANT: return "Giant"; /* Beta EntityList id 53 */
		default: return "Slime";
		}
	default: return "CaveWiiUnknown";
	}
}

/* save-enc-1030: `legacy_footprint` reproduces the encoding every CaveWii
 * build wrote before save-enc-1030 -- the wolf/pig/pig-zombie/creeper tags on
 * EVERY monster (the old "fixed builder footprint") and TAG_COMPOUND as the
 * element type of an empty cart "Items" list. It exists only so
 * entity_native_deletion_safe can still byte-prove a record written by those
 * builds; nothing is ever saved with it. */
static bool build_entity(struct entity_nbt_arena* a, struct nbt_list* list,
						 const struct archived_entity* e,
						 bool legacy_footprint) {
	struct nbt_list* kids;
	if(!add_compound(a, list, NULL, &kids)
	   || !add_string(a, kids, "id", entity_name(e))
	   || !add_num(a, kids, TAG_INT, "CaveWiiType", e->kind)
	   || !add_u32(a, kids, "CaveWiiRuntimeId", e->source_id)
	   || !(doubles_current(e->has_pos_d, e->pos_d, e->pos)
			? add_vector_d(a, kids, "Pos", e->pos_d)
			: add_vector(a, kids, "Pos", TAG_DOUBLE, e->pos, 3))
	   || !(doubles_current(e->has_vel_d, e->vel_d, e->vel)
			? add_vector_d(a, kids, "Motion", e->vel_d)
			: add_vector(a, kids, "Motion", TAG_DOUBLE, e->vel, 3))
	   || !add_vector(a, kids, "Rotation", TAG_FLOAT, e->orient, 2)
	   || !add_num(a, kids, TAG_FLOAT, "FallDistance", e->fall_distance)
	   || !add_num(a, kids, TAG_BYTE, "OnGround", e->on_ground ? 1 : 0)
	   /* FH-18: [740c583 Entity.writeToNBT]: var1.setShort("Fire",
	    * (short)this.fire) -- the real signed counter, not a derived bool. */
	   || !add_num(a, kids, TAG_SHORT, "Fire", e->fire)
	   || !add_num(a, kids, TAG_SHORT, "Health", e->health))
		return false;

	switch(e->kind) {
	case ARCHIVED_ENTITY_ITEM:
		return add_num(a, kids, TAG_SHORT, "Air", e->air)
			&& add_short_clamped(a, kids, "Age", e->data.item.age)
			&& add_u32(a, kids, "CaveWiiDeathTransaction",
					   e->data.item.death_transaction)
			&& add_num(a, kids, TAG_SHORT, "CaveWiiDeathSequence",
					   e->data.item.death_sequence)
			&& add_item(a, kids, "Item", e->data.item.item);
	case ARCHIVED_ENTITY_MONSTER: {
		const struct archived_monster_state* m = &e->data.monster;
		/* save-enc-1030: each Beta-named class tag is written only by its own
		 * class, exactly as Beta [McBetaCpp/srcorigjava]:
		 *   EntityWolf.writeEntityToNBT :56-68   Angry, Sitting (bool), Owner
		 *     (string, "" when there is no owner) -- every wolf;
		 *   EntityPig.writeEntityToNBT :28-32    Saddle (bool) -- every pig;
		 *   EntityPigZombie.writeEntityToNBT :43-47  Anger (short) -- every
		 *     pig zombie;
		 *   EntityCreeper.writeEntityToNBT :29-36  powered (bool) ONLY when
		 *     dataWatcher byte 17 == 1, i.e. only a charged creeper.
		 * They used to be written on every monster to keep a "fixed builder
		 * footprint"; the node budget is a per-kind MAXIMUM, so writing fewer
		 * stays inside it. parse_entity reads each absent tag as its default
		 * (false / "" / 0), the values the other classes carried anyway. */
		bool wolf = legacy_footprint || m->monster_id == MONSTER_WOLF;
		bool pig = legacy_footprint || m->monster_id == MONSTER_PIG;
		bool pig_zombie
			= legacy_footprint || m->monster_id == MONSTER_PIG_ZOMBIE;
		bool powered = legacy_footprint
			|| (m->monster_id == MONSTER_CREEPER && m->charged);
		return add_num(a, kids, TAG_INT, "CaveWiiMonsterId", m->monster_id)
			&& (!wolf
				|| (add_num(a, kids, TAG_BYTE, "Angry", m->wolf_angry ? 1 : 0)
					&& add_num(a, kids, TAG_BYTE, "Sitting",
							   m->wolf_sitting ? 1 : 0)
					&& add_string(a, kids, "Owner", m->wolf_owner)))
			&& add_num(a, kids, TAG_BYTE, "CaveWiiFleeceColor",
						   m->fleece_color)
			/* Beta EntitySheep NBT tag is "Sheared" (with "Color"); the CaveWii-
			 * prefixed name keeps this archive's namespace convention while the
			 * absent->false read migrates every pre-shearing save. */
			&& add_num(a, kids, TAG_BYTE, "CaveWiiSheared",
						   m->sheared ? 1 : 0)
			/* java-load-1023: Beta's OWN sheep tags, sheep only, exactly as
			 * EntitySheep.writeEntityToNBT [EntitySheep.java:57-58]:
			 * setBoolean("Sheared", ...) (a TAG_BYTE) and setByte("Color",
			 * (byte)getFleeceColor()). Without them vanilla's getByte/getBoolean
			 * default to 0 and every CaveWii sheep loads white and woolly. The
			 * CaveWii-prefixed copies above stay (same values): the reader
			 * still falls back to them for pre-fix saves, and fixtures read
			 * them. parse_entity reads the Beta pair first. */
			&& (m->monster_id != MONSTER_SHEEP
				|| (add_num(a, kids, TAG_BYTE, "Sheared", m->sheared ? 1 : 0)
					&& add_num(a, kids, TAG_BYTE, "Color", m->fleece_color)))
			/* java-load-1023: Beta's OWN slime tag [EntitySlime.java:35]:
			 * setInteger("Size", getSlimeSize() - 1). CaveWii encodes the size
			 * in the monster id (1/2/4 -> SMALL/MEDIUM/BIG), so Size is 0/1/3.
			 * Absent, vanilla's getInteger default 0 makes every slime size 1. */
			&& (!monster_is_slime(m->monster_id)
				|| add_num(a, kids, TAG_INT, "Size",
						   slime_beta_size(m->monster_id) - 1))
			/* Beta's OWN tag name: EntityPig.writeEntityToNBT writes
			 * `setBoolean("Saddle", getSaddled())` [740c583] -- unlike Sheared
			 * this field has no CaveWii-only sibling packed into the same
			 * compound, so it uses Beta's literal tag. Pigs only
			 * (save-enc-1030); absent = 0 migrates old saves unsaddled. The
			 * rider relationship is deliberately NOT written here -- Beta
			 * persists no riding relationship for any entity (confirmed by
			 * the minecart slice), so a reload always comes back unridden via
			 * entity_monster()'s constructor default. */
			&& (!pig
				|| add_num(a, kids, TAG_BYTE, "Saddle", m->saddled ? 1 : 0))
			&& add_num(a, kids, TAG_INT, "CaveWiiEggTimer", m->egg_timer)
			/* Beta's OWN tag name: EntityPigZombie writes "Anger" (a short).
			 * Pig zombies only (save-enc-1030); absent = 0 migrates old saves
			 * calm. */
			&& (!pig_zombie
				|| add_num(a, kids, TAG_SHORT, "Anger", m->anger))
			/* Beta's OWN tag name: EntityCreeper writes "powered" (a bool) ONLY
			 * when true [740c583: EntityCreeper.writeEntityToNBT]. Charged
			 * creepers only (save-enc-1030); absent = 0 loads uncharged. */
			&& (!powered
				|| add_num(a, kids, TAG_BYTE, "powered", m->charged ? 1 : 0))
			&& add_num(a, kids, TAG_INT, "CaveWiiAiState", m->ai_state)
			&& add_num(a, kids, TAG_FLOAT, "CaveWiiAiTimer", m->ai_timer)
			&& add_vector(a, kids, "CaveWiiDirection", TAG_FLOAT,
						  m->direction, 2)
			&& add_num(a, kids, TAG_INT, "CaveWiiDirectionTime", m->direction_time)
			&& add_num(a, kids, TAG_FLOAT, "CaveWiiBodyYaw", m->body_yaw)
			&& add_num(a, kids, TAG_FLOAT, "CaveWiiHeadYaw", m->head_yaw)
			&& add_num(a, kids, TAG_INT, "CaveWiiFuse", m->fuse)
			&& add_short_clamped(a, kids, "AttackTime", m->attack_time)
			&& add_num(a, kids, TAG_INT, "CaveWiiAttackWindup", m->attack_windup)
			&& add_num(a, kids, TAG_INT, "CaveWiiFireTime", m->fire_time)
			&& add_short_clamped(a, kids, "HurtTime", m->hurt_time)
			&& add_num(a, kids, TAG_INT, "CaveWiiKnockbackTime", m->knockback_time)
			&& add_num(a, kids, TAG_INT, "CaveWiiAggroTime", m->aggro_time)
			&& add_num(a, kids, TAG_FLOAT, "CaveWiiLimbSwing", m->limb_swing)
			&& add_short_clamped(a, kids, "Air", m->air)
			&& add_num(a, kids, TAG_INT, "CaveWiiEntityAge", m->entity_age)
			&& add_short_clamped(a, kids, "DeathTime", m->death_time)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiDeathLootDropped",
						   m->death_loot_dropped ? 1 : 0)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiSkeletonArrow",
						   m->hit_by_skeleton_arrow ? 1 : 0)
			&& add_num(a, kids, TAG_INT, "CaveWiiJumpDelay", m->jump_delay)
			&& add_vector(a, kids, "CaveWiiSwimVec", TAG_FLOAT, m->swim_vec, 3)
			&& add_num(a, kids, TAG_FLOAT, "CaveWiiSwimSpeed", m->swim_speed)
			&& add_num(a, kids, TAG_FLOAT, "CaveWiiSquidPhase", m->squid_phase)
			&& add_num(a, kids, TAG_FLOAT, "CaveWiiSquidPhaseVel",
						   m->squid_phase_vel);
	}
	case ARCHIVED_ENTITY_MINECART:
		return add_num(a, kids, TAG_FLOAT, "CaveWiiSpeed", e->data.minecart.speed)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiHeadingX", e->data.minecart.hx)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiHeadingZ", e->data.minecart.hz)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiLastRail",
						   e->data.minecart.last_meta)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiOccupied",
						   e->data.minecart.occupied ? 1 : 0)
			&& add_u32(a, kids, "CaveWiiOccupantId",
					   e->data.minecart.occupant_source_id)
			/* Beta's own tag name + type: setInteger("Type", minecartType).
			 * [740c583: EntityMinecart.writeEntityToNBT] */
			&& add_num(a, kids, TAG_INT, "Type", e->data.minecart.minecart_type)
			/* Beta's own tag names + type: setDouble("PushX"/"PushZ", ...).
			 * Our runtime field is float (CODESTYLE.md S2.4); widen on write,
			 * narrow on read (read_float already accepts TAG_DOUBLE). */
			&& add_num(a, kids, TAG_DOUBLE, "PushX",
						   (double)e->data.minecart.push_x)
			&& add_num(a, kids, TAG_DOUBLE, "PushZ",
						   (double)e->data.minecart.push_z)
			/* Beta's own tag name + type: setShort("Fuel", (short)this.fuel).
			 * Beta's runtime `fuel` is an uncapped int (coal is +1200 with no
			 * upper bound -- see MINECART_SLICE_NOTES.md), so Beta's own save
			 * path silently narrows it with a C-style truncating cast. Replicate
			 * that EXACT narrowing ourselves via (int16_t) before handing it to
			 * add_num -- add_num's TAG_SHORT case only accepts
			 * [INT16_MIN, UINT16_MAX] and returns NULL (aborting the WHOLE
			 * chunk's entity archive) outside that, so feeding it a raw
			 * arbitrarily-large int32 fuel value would turn one overfed furnace
			 * cart into a silent save failure for every entity in the chunk --
			 * exactly the undercount-shaped landmine this slice was warned
			 * about, just on value range instead of node/wrap count. */
			&& add_num(a, kids, TAG_SHORT, "Fuel",
						   (double)(int16_t)e->data.minecart.fuel)
			/* Beta's own tag name: "Items" (740c583, chest-cart cargoItems).
			 * Written for every minecart kind (all-empty for type 0/2), the
			 * same fixed-footprint convention entity_node_count's comment
			 * documents. */
			/* CaveWii-only: the explosion break latch (entity_archive.h),
			 * written ONLY inside the break window so an ordinary record
			 * carries no non-Beta tag (the node budget counts it as a max). */
			&& (e->data.minecart.blast_damage <= 40
				|| add_num(a, kids, TAG_SHORT, "CaveWiiBlastDamage",
						   (double)e->data.minecart.blast_damage))
			&& add_item_list(a, kids, "Items", e->data.minecart.inventory,
							 sizeof(e->data.minecart.inventory)
								 / sizeof(e->data.minecart.inventory[0]),
							 legacy_footprint);
	case ARCHIVED_ENTITY_ARROW:
		/* Intentional save-durability deviation: Beta does not serialize
		 * shootingEntity or ticksInAir. CaveWii persists both the owner relation
		 * and flight age so a chunk unload/reload cannot silently discard kill
		 * attribution or restart the five-tick owner-collision grace window. The
		 * live collision/damage rules remain Beta-faithful. */
		return add_u32(a, kids, "CaveWiiOwnerId",
					   e->data.arrow.owner_source_id)
			&& add_num(a, kids, TAG_INT, "CaveWiiAge", e->data.arrow.age)
			&& add_num(a, kids, TAG_SHORT, "CaveWiiItemId", e->data.arrow.item_id)
			&& add_num(a, kids, TAG_SHORT, "CaveWiiDamage", e->data.arrow.damage)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiFromPlayer",
						   e->data.arrow.from_player ? 1 : 0)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiFromSkeleton",
						   e->data.arrow.from_skeleton ? 1 : 0)
			&& add_num(a, kids, TAG_BYTE, "inGround",
						   e->data.arrow.in_ground ? 1 : 0)
			&& add_num(a, kids, TAG_INT, "CaveWiiStuckTicks",
						   e->data.arrow.stuck_ticks)
			&& add_num(a, kids, TAG_INT, "CaveWiiShakeTicks",
						   e->data.arrow.shake_ticks)
			&& add_num(a, kids, TAG_SHORT, "xTile", e->data.arrow.stuck_block_x)
			&& add_num(a, kids, TAG_SHORT, "yTile", e->data.arrow.stuck_block_y)
			&& add_num(a, kids, TAG_SHORT, "zTile", e->data.arrow.stuck_block_z)
			&& add_num(a, kids, TAG_BYTE, "inTile", e->data.arrow.stuck_block_type)
			&& add_num(a, kids, TAG_BYTE, "inData", e->data.arrow.stuck_block_meta);
	case ARCHIVED_ENTITY_EGG:
	case ARCHIVED_ENTITY_SNOWBALL:
	case ARCHIVED_ENTITY_FIREBALL:
		/* The arrow's owner/age pair minus every arrow-specific field (the egg
		 * has no stick/pickup/damage/shooter-class state). Persisting keeps a
		 * mid-flight egg through an unload/reload boundary, chick roll intact.
		 * The snowball is the identical record under id "Snowball". */
		return add_u32(a, kids, "CaveWiiOwnerId",
					   e->data.egg.owner_source_id)
			&& add_num(a, kids, TAG_INT, "CaveWiiAge", e->data.egg.age);
	case ARCHIVED_ENTITY_PRIMED_TNT:
		/* Fuse is Beta's field. The CaveWii bit distinguishes the normal live
		 * fuse-zero interval from a final step already rejected by the Wii cap;
		 * CaveWiiBlockX/Y/Z remain migration-only legacy fields. */
		return add_num(a, kids, TAG_BYTE, "Fuse",
					   e->data.primed_tnt.fuse)
			&& add_num(a, kids, TAG_BYTE, "CaveWiiExplosionPending",
					   e->data.primed_tnt.explosion_pending ? 1 : 0);
	case ARCHIVED_ENTITY_BOAT:
		/* The ONE CaveWii-only tag (the explosion break latch, see
		 * archived_boat_state); shake/rock-direction/occupancy still reset to
		 * constructor defaults on reload as Beta's empty writeEntityToNBT. */
		return e->data.boat.blast_damage <= 40
			|| add_num(a, kids, TAG_SHORT, "CaveWiiBlastDamage",
					   (double)e->data.boat.blast_damage);
	case ARCHIVED_ENTITY_PAINTING:
		/* Beta's OWN tag names [740c583: EntityPainting.writeEntityToNBT] --
		 * note Beta does NOT call super.writeEntityToNBT for this entity, so
		 * the common Pos/Motion/etc. block above (always written by this
		 * framework) is a CaveWii superset, not a Beta tag. */
		return add_num(a, kids, TAG_BYTE, "Dir", e->data.painting.direction)
			&& add_string(a, kids, "Motive", e->data.painting.art_name)
			&& add_num(a, kids, TAG_INT, "TileX", e->data.painting.tile_x)
			&& add_num(a, kids, TAG_INT, "TileY", e->data.painting.tile_y)
			&& add_num(a, kids, TAG_INT, "TileZ", e->data.painting.tile_z);
	case ARCHIVED_ENTITY_FALLING_SAND:
		/* Beta's OWN tag: `setByte("Tile", (byte)this.blockID)`. [740c583
		 * EntityFallingSand.writeEntityToNBT] */
		return add_num(a, kids, TAG_BYTE, "Tile",
					   e->data.falling_sand.block_id);
	default: return false;
	}
}

static struct nbt_list* chunk_entities_build_ex(
	const struct chunk_entities* entities, void** arena_out,
	bool legacy_footprint) {
	if(!arena_out)
		return NULL;
	*arena_out = NULL;
	if(!entities || entities->count == 0)
		return NULL;
	if(!entities->entities
	   || entities->count > CHUNK_ENTITY_ARCHIVE_WRITE_MAX)
		return NULL;

	size_t nodes_cap = 1;
	size_t wraps_cap = 1;
	for(size_t i = 0; i < entities->count; i++) {
		size_t nodes = entity_node_count(entities->entities[i].kind);
		size_t wraps = entity_wrap_count(entities->entities[i].kind);
		if(nodes == 0 || wraps == 0
		   || nodes_cap > SIZE_MAX - nodes
		   || wraps_cap > SIZE_MAX - wraps)
			return NULL;
		nodes_cap += nodes;
		wraps_cap += wraps;
	}
	if(nodes_cap > (SIZE_MAX - sizeof(struct entity_nbt_arena))
				   / sizeof(nbt_node))
		return NULL;
	size_t bytes = sizeof(struct entity_nbt_arena)
		+ nodes_cap * sizeof(nbt_node);
	if(wraps_cap > (SIZE_MAX - bytes) / sizeof(struct nbt_list))
		return NULL;
	bytes += wraps_cap * sizeof(struct nbt_list);
	char* block = calloc(1, bytes);
	if(!block)
		return NULL;
	struct entity_nbt_arena* a = (struct entity_nbt_arena*)block;
	a->nodes = (nbt_node*)(block + sizeof(*a));
	a->wraps = (struct nbt_list*)((char*)a->nodes
									 + nodes_cap * sizeof(nbt_node));
	a->nodes_cap = nodes_cap;
	a->wraps_cap = wraps_cap;

	nbt_node* compound_type = node_alloc(a);
	struct nbt_list* sentinel = wrap_alloc(a);
	if(!compound_type || !sentinel)
		goto fail;
	compound_type->type = TAG_COMPOUND;
	sentinel->data = compound_type;

	for(size_t i = 0; i < entities->count; i++)
		if(!build_entity(a, sentinel, &entities->entities[i], legacy_footprint))
			goto fail;

	*arena_out = block;
	return sentinel;

fail:
	chunk_entities_build_free(block);
	return NULL;
}

struct nbt_list* chunk_entities_build(const struct chunk_entities* entities,
									 void** arena_out) {
	return chunk_entities_build_ex(entities, arena_out, false);
}

void chunk_entities_build_free(void* arena) {
	struct entity_nbt_arena* a = arena;
	if(a) for(size_t k=0;k<a->ni;k++)
		if(a->nodes[k].type == TAG_RAW_COMPOUND)
			free(a->nodes[k].payload.tag_byte_array.data);
	free(arena);
}

static const nbt_node* find_child(const nbt_node* comp, const char* name) {
	if(!comp || comp->type != TAG_COMPOUND || !comp->payload.tag_compound)
		return NULL;
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		const nbt_node* n = list_entry(pos, struct nbt_list, entry)->data;
		if(n && n->name && strcmp(n->name, name) == 0)
			return n;
	}
	return NULL;
}

static bool read_int(const nbt_node* comp, const char* name, int32_t* out) {
	const nbt_node* n = find_child(comp, name);
	if(!n)
		return false;
	switch(n->type) {
	case TAG_BYTE: *out = n->payload.tag_byte; return true;
	case TAG_SHORT: *out = n->payload.tag_short; return true;
	case TAG_INT: *out = n->payload.tag_int; return true;
	default: return false;
	}
}

static bool read_int_range(const nbt_node* comp, const char* name,
						   int32_t min_value, int32_t max_value,
						   int32_t* out) {
	int32_t value;
	if(!read_int(comp, name, &value) || value < min_value || value > max_value)
		return false;
	*out = value;
	return true;
}

static bool read_u8(const nbt_node* comp, const char* name, uint8_t* out) {
	const nbt_node* n = find_child(comp, name);
	if(!n)
		return false;
	if(n->type == TAG_BYTE) {
		*out = (uint8_t)n->payload.tag_byte;
		return true;
	}
	if(n->type == TAG_SHORT && n->payload.tag_short >= 0
	   && n->payload.tag_short <= UINT8_MAX) {
		*out = (uint8_t)n->payload.tag_short;
		return true;
	}
	if(n->type == TAG_INT && n->payload.tag_int >= 0
	   && n->payload.tag_int <= UINT8_MAX) {
		*out = (uint8_t)n->payload.tag_int;
		return true;
	}
	return false;
}

static bool read_u16(const nbt_node* comp, const char* name, uint16_t* out) {
	const nbt_node* n = find_child(comp, name);
	if(!n)
		return false;
	if(n->type == TAG_SHORT) {
		*out = (uint16_t)n->payload.tag_short;
		return true;
	}
	if(n->type == TAG_INT && n->payload.tag_int >= 0
	   && n->payload.tag_int <= UINT16_MAX) {
		*out = (uint16_t)n->payload.tag_int;
		return true;
	}
	return false;
}

static bool read_u32(const nbt_node* comp, const char* name, uint32_t* out) {
	const nbt_node* n = find_child(comp, name);
	if(!n)
		return false;
	if(n->type == TAG_LONG && n->payload.tag_long >= 0
	   && (uint64_t)n->payload.tag_long <= UINT32_MAX) {
		*out = (uint32_t)n->payload.tag_long;
		return true;
	}
	if(n->type == TAG_INT) {
		/* Legacy archives used TAG_INT. Preserve its 32-bit payload, including
		 * the UINT32_MAX orphan-reference sentinel written as -1. */
		*out = (uint32_t)n->payload.tag_int;
		return true;
	}
	return false;
}

static bool read_float(const nbt_node* comp, const char* name, float* out) {
	const nbt_node* n = find_child(comp, name);
	if(!n)
		return false;
	float value;
	if(n->type == TAG_FLOAT)
		value = n->payload.tag_float;
	else if(n->type == TAG_DOUBLE)
		value = (float)n->payload.tag_double;
	else
		return false;
	if(!isfinite(value))
		return false;
	*out = value;
	return true;
}

static bool read_float_range(const nbt_node* comp, const char* name,
							 float min_value, float max_value, float* out) {
	float value;
	if(!read_float(comp, name, &value) || value < min_value || value > max_value)
		return false;
	*out = value;
	return true;
}

static bool read_vector(const nbt_node* comp, const char* name, float* out,
						size_t count) {
	const nbt_node* list = find_child(comp, name);
	if(!list || list->type != TAG_LIST || !list->payload.tag_list)
		return false;
	if(count > 3)
		return false;
	float values[3] = {0};
	size_t i = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry) {
		const nbt_node* n = list_entry(pos, struct nbt_list, entry)->data;
		if(i >= count || !n)
			return false;
		double v;
		if(n->type == TAG_DOUBLE)
			v = n->payload.tag_double;
		else if(n->type == TAG_FLOAT)
			v = n->payload.tag_float;
		else
			return false;
		values[i++] = (float)v;
		if(!isfinite(v) || !isfinite(values[i - 1]))
			return false;
	}
	if(i != count)
		return false;
	memcpy(out, values, count * sizeof(*out));
	return true;
}

/* save-enc-1030: the exact doubles of a three-element TAG_DOUBLE list whose
 * float image is `image` (what read_vector admitted). Anything else (float
 * elements, wrong count) answers false and the float path stays in charge. */
static bool read_vector_d(const nbt_node* comp, const char* name,
						  const float* image, double out[3]) {
	const nbt_node* list = find_child(comp, name);
	if(!list || list->type != TAG_LIST || !list->payload.tag_list)
		return false;
	size_t i = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry) {
		const nbt_node* n = list_entry(pos, struct nbt_list, entry)->data;
		if(i >= 3 || !n || n->type != TAG_DOUBLE
		   || !isfinite(n->payload.tag_double)
		   || (float)n->payload.tag_double != image[i])
			return false;
		out[i++] = n->payload.tag_double;
	}
	return i == 3;
}

static bool read_vector_range(const nbt_node* comp, const char* name, float* out,
							  size_t count, float min_value,
							  float max_value) {
	float values[3];
	if(count > 3 || !read_vector(comp, name, values, count))
		return false;
	for(size_t i = 0; i < count; i++)
		if(values[i] < min_value || values[i] > max_value)
			return false;
	memcpy(out, values, count * sizeof(*out));
	return true;
}

static int monster_id_from_name(const char* id) {
	if(!id)
		return -1;
	if(strcmp(id, "Zombie") == 0) return MONSTER_ZOMBIE;
	if(strcmp(id, "Creeper") == 0) return MONSTER_CREEPER;
	if(strcmp(id, "Skeleton") == 0) return MONSTER_SKELETON;
	if(strcmp(id, "Spider") == 0) return MONSTER_SPIDER;
	if(strcmp(id, "Pig") == 0) return MONSTER_PIG;
	if(strcmp(id, "Cow") == 0) return MONSTER_COW;
	if(strcmp(id, "Chicken") == 0) return MONSTER_CHICKEN;
	if(strcmp(id, "Sheep") == 0) return MONSTER_SHEEP;
	if(strcmp(id, "Squid") == 0) return MONSTER_SQUID;
	if(strcmp(id, "Slime") == 0) return MONSTER_SLIME_SMALL;
	if(strcmp(id, "PigZombie") == 0) return MONSTER_PIG_ZOMBIE;
	if(strcmp(id, "Ghast") == 0) return MONSTER_GHAST;
	if(strcmp(id, "Wolf") == 0) return MONSTER_WOLF;
	if(strcmp(id, "Giant") == 0) return MONSTER_GIANT; /* Beta EntityList 53 */
	return -1;
}

static enum archived_entity_kind kind_from_compound(const nbt_node* comp,
											 int* monster_id) {
	int32_t type;
	if(read_int(comp, "CaveWiiType", &type)
	   && type >= ARCHIVED_ENTITY_ITEM
	   && type <= ARCHIVED_ENTITY_FALLING_SAND)
		return (enum archived_entity_kind)type;
	const nbt_node* id = find_child(comp, "id");
	if(!id || id->type != TAG_STRING || !id->payload.tag_string)
		return 0;
	if(strcmp(id->payload.tag_string, "Item") == 0)
		return ARCHIVED_ENTITY_ITEM;
	if(strcmp(id->payload.tag_string, "Minecart") == 0)
		return ARCHIVED_ENTITY_MINECART;
	if(strcmp(id->payload.tag_string, "Arrow") == 0)
		return ARCHIVED_ENTITY_ARROW;
	if(strcmp(id->payload.tag_string, "Egg") == 0)
		return ARCHIVED_ENTITY_EGG;
	if(strcmp(id->payload.tag_string, "Snowball") == 0)
		return ARCHIVED_ENTITY_SNOWBALL;
	if(strcmp(id->payload.tag_string, "Fireball") == 0)
		return ARCHIVED_ENTITY_FIREBALL;
	if(strcmp(id->payload.tag_string, "PrimedTnt") == 0)
		return ARCHIVED_ENTITY_PRIMED_TNT;
	if(strcmp(id->payload.tag_string, "Boat") == 0)
		return ARCHIVED_ENTITY_BOAT;
	if(strcmp(id->payload.tag_string, "Painting") == 0)
		return ARCHIVED_ENTITY_PAINTING;
	if(strcmp(id->payload.tag_string, "FallingSand") == 0)
		return ARCHIVED_ENTITY_FALLING_SAND;
	*monster_id = monster_id_from_name(id->payload.tag_string);
	return *monster_id >= 0 ? ARCHIVED_ENTITY_MONSTER : 0;
}

static bool parse_item(const nbt_node* comp, struct item_data* out) {
	uint16_t id, damage = 0;
	uint8_t count;
	if(!comp || comp->type != TAG_COMPOUND || !read_u16(comp, "id", &id)
	   || !read_u8(comp, "Count", &count))
		return false;
	read_u16(comp, "Damage", &damage);
	if(id == 0 || count == 0)
		return false;
	*out = (struct item_data) {
		.id = id,
		.count = count,
		.durability = damage,
	};
	item_nbt_capture(comp,out);
	return true;
}

static w_coord_t block_chunk_coord(int32_t value) {
	int32_t q = value / CHUNK_SIZE;
	if(value < 0 && value % CHUNK_SIZE != 0)
		q--;
	return (w_coord_t)q;
}

static bool parse_entity(const nbt_node* comp, w_coord_t chunk_x,
						 w_coord_t chunk_z, struct archived_entity* out) {
	memset(out, 0, sizeof(*out));
	if(!comp || comp->type != TAG_COMPOUND || !comp->payload.tag_compound)
		return false;
	size_t child_count = 0;
	struct list_head* child;
	list_for_each(child, &comp->payload.tag_compound->entry)
		if(++child_count > ARCHIVED_ENTITY_CHILD_MAX)
			return false;
	int inferred_monster = -1;
	out->kind = kind_from_compound(comp, &inferred_monster);
	if(!out->kind || !read_vector(comp, "Pos", out->pos, 3)
	   || !read_vector(comp, "Motion", out->vel, 3))
		return false;
	/* save-enc-1030: a monster's runtime keeps double Pos/Motion
	 * (mob_motion), and its record stores them exactly; keep the doubles so
	 * a re-encode of the decoded record (the deletion proof) is byte-exact. */
	if(out->kind == ARCHIVED_ENTITY_MONSTER) {
		out->has_pos_d = read_vector_d(comp, "Pos", out->pos, out->pos_d);
		out->has_vel_d = read_vector_d(comp, "Motion", out->vel, out->vel_d);
	}

	/* Rotation is required for CaveWii records but optional in foreign Beta NBT. */
	if(!read_vector(comp, "Rotation", out->orient, 2)) {
		out->orient[0] = 0.0f;
		out->orient[1] = 0.0f;
	}

	/* One released CaveWii build serialized a stationary primed-TNT block
	 * with explicit integer coordinates. Presence and conversion are distinct:
	 * a present wrong-type tag is malformed legacy data, not an absent extension
	 * on a modern moving entity. Legacy ownership follows the exact block triple;
	 * modern PrimedTnt ownership follows its live floating-point position. */
	bool legacy_tnt = false;
	if(out->kind == ARCHIVED_ENTITY_PRIMED_TNT) {
		struct archived_primed_tnt_state* t = &out->data.primed_tnt;
		bool tag_x = find_child(comp, "CaveWiiBlockX") != NULL;
		bool tag_y = find_child(comp, "CaveWiiBlockY") != NULL;
		bool tag_z = find_child(comp, "CaveWiiBlockZ") != NULL;
		if(tag_x || tag_y || tag_z) {
			if(!(tag_x && tag_y && tag_z)
			   || !read_int(comp, "CaveWiiBlockX", &t->x)
			   || !read_int(comp, "CaveWiiBlockY", &t->y)
			   || !read_int(comp, "CaveWiiBlockZ", &t->z)
			   || t->y < 0 || t->y >= WORLD_HEIGHT
			   || block_chunk_coord(t->x) != chunk_x
			   || block_chunk_coord(t->z) != chunk_z)
				return false;
			t->legacy_block_backed = true;
			legacy_tnt = true;
		}
	}
	if(fabsf(out->pos[0]) > 30000000.0f || fabsf(out->pos[1]) > 4096.0f
	   || fabsf(out->pos[2]) > 30000000.0f || fabsf(out->vel[0]) > 10000.0f
	   || fabsf(out->vel[1]) > 10000.0f || fabsf(out->vel[2]) > 10000.0f
	   || (!legacy_tnt
		   && ((w_coord_t)floorf(out->pos[0] / CHUNK_SIZE) != chunk_x
			   || (w_coord_t)floorf(out->pos[2] / CHUNK_SIZE) != chunk_z)))
		return false;

	int32_t v;
	uint32_t u32;
	/* Beta Entity constructor default. Every exact Beta record carries Air as a
	 * SHORT; keeping the default here also preserves older CaveWii records that
	 * predate the common tag. */
	out->air = 300;
	if(read_u32(comp, "CaveWiiRuntimeId", &u32))
		out->source_id = u32;
	if(read_int(comp, "OnGround", &v))
		out->on_ground = v != 0;
	/* FH-18: [740c583 Entity.readFromNBT]: this.fire = var1.getShort("Fire").
	 * Beta narrows through a Java (short) cast on write (never on read, but
	 * the write side already guarantees the stored value fits); an NBT_INT
	 * read here is defensive against a hostile/malformed oversized payload
	 * (see entity_archive.h's own hostile-input framing), mirroring the
	 * existing Health clamp immediately below. Migration note (the fork-hack
	 * audit's "silent reinterpret" trap): an OLD save's Fire tag only ever
	 * held 0 or 1 (this port's prior bool encoding). Both values remain
	 * correct under the new reading -- 0 stays "not on fire", and 1 still
	 * satisfies `fire > 0` ("currently on fire", Beta's own isBurning()
	 * condition) -- so no explicit migration/sentinel is needed: the only
	 * kind this port ever actually ignites (monsters) already carries its
	 * OWN authoritative counter under the separate "CaveWiiFireTime" tag
	 * (see server_local.c's restore path), which this common tag was never
	 * the source of truth for. */
	if(read_int(comp, "Fire", &v))
		out->fire = (int16_t)(v < INT16_MIN ? INT16_MIN
										: (v > INT16_MAX ? INT16_MAX : v));
	if(read_int(comp, "Air", &v))
		out->air = (int16_t)(v < INT16_MIN ? INT16_MIN
									   : (v > INT16_MAX ? INT16_MAX : v));
	/* Entity.readFromNBT owns this common tag. It is optional for older
	 * CaveWii non-living records; malformed/out-of-range input sanitizes to the
	 * constructor default of zero. A legacy monster's existing tag is the same
	 * Beta field and therefore migrates through this common read. */
	read_float_range(comp, "FallDistance", 0.0f, 4096.0f,
				 &out->fall_distance);
	bool have_health = read_int(comp, "Health", &v);
	if(have_health)
		out->health = (int16_t)(v < INT16_MIN ? INT16_MIN
										: (v > INT16_MAX ? INT16_MAX : v));

	switch(out->kind) {
	case ARCHIVED_ENTITY_ITEM: {
		const nbt_node* item = find_child(comp, "Item");
		if(!parse_item(item, &out->data.item.item))
			return false;
		if(read_int_range(comp, "Age", 0, ARCHIVED_ITEM_AGE_MAX, &v))
			out->data.item.age = v;
		/* Both tags form one identity. A foreign/older item with neither (or a
		 * malformed half-pair) remains an ordinary untagged EntityItem. */
		uint32_t transaction;
		uint16_t sequence;
		if(read_u32(comp, "CaveWiiDeathTransaction", &transaction)
		   && read_u16(comp, "CaveWiiDeathSequence", &sequence)
		   && transaction != 0) {
			out->data.item.death_transaction = transaction;
			out->data.item.death_sequence = sequence;
		}
		return true;
	}
	case ARCHIVED_ENTITY_MONSTER: {
		int32_t mid = inferred_monster;
		read_int(comp, "CaveWiiMonsterId", &mid);
		/* java-load-1023: Beta's "Size" [EntitySlime.java:40 setSlimeSize(
		 * getInteger("Size") + 1)] is authoritative for a slime; the
		 * CaveWiiMonsterId / bare-"Slime" inference above is the fallback for
		 * pre-fix CaveWii saves. Sizes other than Beta's 1/2/4 have no CaveWii
		 * slime id and keep the fallback. */
		if(monster_is_slime(mid) && read_int(comp, "Size", &v)) {
			if(v == 0) mid = MONSTER_SLIME_SMALL;
			else if(v == 1) mid = MONSTER_SLIME_MEDIUM;
			else if(v == 3) mid = MONSTER_SLIME_BIG;
		}
		if(mid < 0 || mid >= MONSTER_COUNT || !have_health)
			return false;
		struct archived_monster_state* m = &out->data.monster;
		m->monster_id = mid;
		/* Foreign Beta records have only the common signed Fire tag. CaveWii's
		 * extension, when present, remains authoritative below. */
		m->fire_time = out->fire >= -1 && out->fire <= MOB_LAVA_FIRE_TICKS
			? out->fire : 0;
		if(read_int(comp, "Angry", &v)) m->wolf_angry = v != 0;
		if(read_int(comp, "Sitting", &v)) m->wolf_sitting = v != 0;
		const nbt_node* owner = find_child(comp, "Owner");
		if(owner && owner->type == TAG_STRING && owner->payload.tag_string) {
			strncpy(m->wolf_owner, owner->payload.tag_string,
				sizeof(m->wolf_owner) - 1);
			m->wolf_owner[sizeof(m->wolf_owner) - 1] = '\0';
		}
		/* java-load-1023: Beta's own "Color"/"Sheared" [EntitySheep.java:
		 * 62-64] win; the CaveWii-prefixed names are the fallback for pre-fix
		 * CaveWii saves, which carry only those. */
		m->fleece_color = (uint8_t)sheep_fleece_color(out->source_id);
		if(read_int(comp, "CaveWiiFleeceColor", &v) && v >= 0 && v <= 15)
			m->fleece_color = (uint8_t)v;
		if(read_int(comp, "Color", &v) && v >= 0 && v <= 15)
			m->fleece_color = (uint8_t)v;
		/* Absent = false: every pre-shearing save loads woolly (legacy). */
		if(read_int(comp, "CaveWiiSheared", &v))
			m->sheared = v != 0;
		if(read_int(comp, "Sheared", &v))
			m->sheared = v != 0;
		/* Absent = false: every pre-saddle save loads unsaddled, matching
		 * Beta's own dataWatcher default (EntityPig.entityInit adds byte 16
		 * as 0). Beta's own tag name -- see the build_entity comment. */
		if(read_int(comp, "Saddle", &v))
			m->saddled = v != 0;
		/* Absent/malformed = 0: the chicken tick re-rolls Beta's 6000..11999 on
		 * the first server tick (the documented unrolled sentinel). */
		read_int_range(comp, "CaveWiiEggTimer", 0, 11999, &m->egg_timer);
		/* Absent = 0 (calm). 0..799 covers Beta's 400+rand(400) arming range;
		 * b1.7.3 never decrements it, so any nonzero value means angry. */
		read_int_range(comp, "Anger", 0, 799, &m->anger);
		/* Absent = false (uncharged): every pre-weather save loads unpowered. */
		if(read_int(comp, "powered", &v)) m->charged = v != 0;
		if(read_int(comp, "CaveWiiAiState", &v)) m->ai_state = v;
		read_float_range(comp, "CaveWiiAiTimer", 0.0f,
					 SKELETON_ARROW_COOLDOWN_TICKS, &m->ai_timer);
		read_vector_range(comp, "CaveWiiDirection", m->direction, 2, -1.0f,
					  1.0f);
		read_int_range(comp, "CaveWiiDirectionTime", 0,
					   ARCHIVED_DIRECTION_TIME_MAX, &m->direction_time);
		read_float(comp, "CaveWiiBodyYaw", &m->body_yaw);
		read_float(comp, "CaveWiiHeadYaw", &m->head_yaw);
		m->fuse = -1;
		read_int_range(comp, "CaveWiiFuse", -1, CREEPER_FUSE_TICKS, &m->fuse);
		/* No CaveWiiCreeperState counterpart, deliberately: unlike the v2
		 * "CaveWiiFuse" continuity extension above, `creeper_state` (Beta's
		 * creeperState DataWatcher byte, the hysteresis SELECTOR -- see entity.h)
		 * is never persisted in real Beta either, so it always resets to
		 * entityInit's -1 default on load, exactly like a fresh spawn (the
		 * entity_monster() call in server_local.c's restore path already sets it;
		 * `struct archived_monster_state` intentionally carries no field for it).
		 * A reloaded mid-swell creeper simply re-evaluates the hysteresis selector
		 * fresh on its next tick -- the ramp/flash the player sees is untouched,
		 * only which re-arm bubble (3 vs 7 blocks) applies next tick.
		 * [740c583: EntityCreeper -- no creeperState entry in writeEntityToNBT] */
		read_int_range(comp, "AttackTime", 0, MOB_ATTACK_COOLDOWN_TICKS,
					   &m->attack_time);
		m->attack_windup = MOB_ATTACK_WINDUP_TICKS;
		read_int_range(comp, "CaveWiiAttackWindup", 0,
					   MOB_ATTACK_WINDUP_TICKS, &m->attack_windup);
		/* Beta Entity.writeToNBT/readFromNBT:755,786 stores signed Fire;
		 * moveEntity:489-495 may set -fireResistance (-1 for mobs). */
		read_int_range(comp, "CaveWiiFireTime", -1, MOB_LAVA_FIRE_TICKS,
						   &m->fire_time);
		read_int_range(comp, "HurtTime", 0, MOB_HURT_ANIMATION_TICKS,
					   &m->hurt_time);
		read_int_range(comp, "CaveWiiKnockbackTime", 0, MOB_KNOCKBACK_TICKS,
					   &m->knockback_time);
		read_int_range(comp, "CaveWiiAggroTime", 0, ARCHIVED_ENTITY_AGE_MAX,
					   &m->aggro_time);
		read_float(comp, "CaveWiiLimbSwing", &m->limb_swing);
		m->air = MOB_AIR_MAX;
		read_int_range(comp, "Air", MOB_DROWN_AIR_THRESHOLD, MOB_AIR_MAX,
						   &m->air);
		read_int_range(comp, "CaveWiiEntityAge", 0, ARCHIVED_ENTITY_AGE_MAX,
					   &m->entity_age);
		read_int_range(comp, "DeathTime", 0, MOB_DEATH_TIME_MAX, &m->death_time);
		/* Old records with a positive death clock necessarily ran onDeath. The
		 * explicit bit preserves the exact late-kill state deathTime=0/onDeath=true. */
		m->death_loot_dropped = m->death_time > 0;
		if(read_int(comp, "CaveWiiDeathLootDropped", &v))
			m->death_loot_dropped = v != 0;
		if(read_int(comp, "CaveWiiSkeletonArrow", &v))
			m->hit_by_skeleton_arrow = v != 0;
		read_int_range(comp, "CaveWiiJumpDelay", 0, ARCHIVED_JUMP_DELAY_MAX,
					   &m->jump_delay);
		read_vector_range(comp, "CaveWiiSwimVec", m->swim_vec, 3, -1.0f,
					  1.0f);
		read_float_range(comp, "CaveWiiSwimSpeed", 0.0f, 1.0f,
					 &m->swim_speed);
		read_float_range(comp, "CaveWiiSquidPhase", 0.0f,
					 2.0f * (float)M_PI, &m->squid_phase);
		m->squid_phase_vel = 0.15f;
		read_float_range(comp, "CaveWiiSquidPhaseVel", 0.0f, 1.0f,
					 &m->squid_phase_vel);
		return true;
	}
	case ARCHIVED_ENTITY_MINECART: {
		read_float_range(comp, "CaveWiiSpeed", -ARCHIVED_MINECART_SPEED_MAX,
					 ARCHIVED_MINECART_SPEED_MAX, &out->data.minecart.speed);
		if(read_int_range(comp, "CaveWiiHeadingX", -1, 1, &v))
			out->data.minecart.hx = (int8_t)v;
		if(read_int_range(comp, "CaveWiiHeadingZ", -1, 1, &v))
			out->data.minecart.hz = (int8_t)v;
		read_u8(comp, "CaveWiiLastRail", &out->data.minecart.last_meta);
		if(read_int(comp, "CaveWiiOccupied", &v)) out->data.minecart.occupied = v != 0;
		read_u32(comp, "CaveWiiOccupantId",
				 &out->data.minecart.occupant_source_id);
		/* Absent (a pre-minecart-slice or foreign-Beta record) -> 0: rideable,
		 * matching the constructor default and every pre-existing save. */
		if(read_int_range(comp, "Type", 0, 2, &v))
			out->data.minecart.minecart_type = (uint8_t)v;
		read_float(comp, "PushX", &out->data.minecart.push_x);
		read_float(comp, "PushZ", &out->data.minecart.push_z);
		/* The writer already narrowed to a valid int16 (see build_entity); the
		 * bound here is the round-trip range, not a fresh cap. */
		if(read_int_range(comp, "Fuel", INT16_MIN, INT16_MAX, &v))
			out->data.minecart.fuel = v;
		/* Clamped read: a negative would heal a vehicle past its counter;
		 * absent or out of range -> 0 (pre-tag records, foreign saves). */
		if(read_int_range(comp, "CaveWiiBlastDamage", 0, INT16_MAX, &v))
			out->data.minecart.blast_damage = (int16_t)v;
		const nbt_node* items_list = find_child(comp, "Items");
		if(!item_nbt_read_list(items_list,out->data.minecart.inventory,
			sizeof(out->data.minecart.inventory)/sizeof(out->data.minecart.inventory[0]),false))
			return false;
		return true;
	}
	case ARCHIVED_ENTITY_ARROW:
		read_u32(comp, "CaveWiiOwnerId", &out->data.arrow.owner_source_id);
		read_int_range(comp, "CaveWiiAge", 0, INT32_MAX,
					   &out->data.arrow.age);
		read_u16(comp, "CaveWiiItemId", &out->data.arrow.item_id);
		if(read_int_range(comp, "CaveWiiDamage", INT16_MIN, INT16_MAX, &v))
			out->data.arrow.damage = (int16_t)v;
		if(read_int(comp, "CaveWiiFromPlayer", &v)) out->data.arrow.from_player = v != 0;
		if(read_int(comp, "CaveWiiFromSkeleton", &v)) out->data.arrow.from_skeleton = v != 0;
		if(read_int(comp, "inGround", &v)) out->data.arrow.in_ground = v != 0;
		read_int_range(comp, "CaveWiiStuckTicks", 0,
					   ARROW_STUCK_DESPAWN_TICKS, &out->data.arrow.stuck_ticks);
		read_int_range(comp, "CaveWiiShakeTicks", 0, ARROW_SHAKE_TICKS,
					   &out->data.arrow.shake_ticks);
		if(read_int_range(comp, "xTile", INT16_MIN, INT16_MAX, &v)) out->data.arrow.stuck_block_x = (int16_t)v;
		if(read_int_range(comp, "yTile", INT16_MIN, INT16_MAX, &v)) out->data.arrow.stuck_block_y = (int16_t)v;
		if(read_int_range(comp, "zTile", INT16_MIN, INT16_MAX, &v)) out->data.arrow.stuck_block_z = (int16_t)v;
		if(read_int_range(comp, "inTile", -128, 255, &v)) out->data.arrow.stuck_block_type = (uint8_t)v;
		if(read_int_range(comp, "inData", -128, 255, &v)) out->data.arrow.stuck_block_meta = (uint8_t)v;
		return out->data.arrow.item_id != 0;
	case ARCHIVED_ENTITY_EGG:
	case ARCHIVED_ENTITY_SNOWBALL:
	case ARCHIVED_ENTITY_FIREBALL:
		read_u32(comp, "CaveWiiOwnerId", &out->data.egg.owner_source_id);
		read_int_range(comp, "CaveWiiAge", 0, INT32_MAX,
					   &out->data.egg.age);
		return true;
	case ARCHIVED_ENTITY_PRIMED_TNT: {
		struct archived_primed_tnt_state* t = &out->data.primed_tnt;
		if(!read_int(comp, "Fuse", &v) || v < 0 || v > TNT_FUSE_DEFAULT)
			return false;
		t->fuse = (uint16_t)v;
		if(read_int(comp, "CaveWiiExplosionPending", &v))
			t->explosion_pending = v != 0;
		if(t->explosion_pending
		   && (t->fuse != 0 || t->legacy_block_backed))
			return false;
		return true;
	}
	case ARCHIVED_ENTITY_BOAT: {
		int bd;
		if(read_int_range(comp, "CaveWiiBlastDamage", 0, INT16_MAX, &bd))
			out->data.boat.blast_damage = (int16_t)bd;
		return true;
	}
	case ARCHIVED_ENTITY_FALLING_SAND: {
		/* Beta reads `this.blockID = var1.getByte("Tile") & 255;`
		 * unconditionally -- but this port's own entity_falling_sand()
		 * constructor contract only ever expects BLOCK_SAND/BLOCK_GRAVEL
		 * (the render/dig/drop paths all key off one of those two), so a
		 * malformed or hostile save carrying any other id is rejected here
		 * rather than reviving an entity no live code path can otherwise
		 * construct. [740c583 EntityFallingSand.readEntityFromNBT] */
		uint8_t tile;
		if(!read_u8(comp, "Tile", &tile))
			return false;
		if(tile != BLOCK_SAND && tile != BLOCK_GRAVEL)
			return false;
		out->data.falling_sand.block_id = tile;
		return true;
	}
	case ARCHIVED_ENTITY_PAINTING: {
		struct archived_painting_state* p = &out->data.painting;
		if(!read_int_range(comp, "TileX", -30000000, 30000000, &p->tile_x)
		   || !read_int_range(comp, "TileY", 0, WORLD_HEIGHT - 1, &p->tile_y)
		   || !read_int_range(comp, "TileZ", -30000000, 30000000, &p->tile_z)
		   || !read_int_range(comp, "Dir", 0, 3, &p->direction))
			return false;
		const nbt_node* motive = find_child(comp, "Motive");
		if(motive && motive->type == TAG_STRING && motive->payload.tag_string) {
			strncpy(p->art_name, motive->payload.tag_string,
				sizeof(p->art_name) - 1);
			p->art_name[sizeof(p->art_name) - 1] = '\0';
		}
		/* [740c583: EntityPainting.readEntityFromNBT] `if (this.art == null)
		 * this.art = EnumArt.Kebab;` -- an absent/unmatched Motive still
		 * loads (as Kebab), it does not reject the whole record. */
		return true;
	}
	default: return false;
	}
}

bool chunk_entities_nbt_list_nonempty(const nbt_node* entities_list) {
	return entities_list && entities_list->type == TAG_LIST
		&& entities_list->payload.tag_list
		&& !list_empty(&entities_list->payload.tag_list->entry);
}

bool chunk_entities_parse(const nbt_node* entities_list, w_coord_t chunk_x,
							  w_coord_t chunk_z, struct chunk_entities* out) {
	if(!out)
		return false;
	memset(out, 0, sizeof(*out));
	if(!entities_list)
		return true;
	if(entities_list->type != TAG_LIST || !entities_list->payload.tag_list)
		return true; /* malformed optional list must not make terrain unreadable */

	size_t candidates = 0;
	struct list_head* pos;
	list_for_each(pos, &entities_list->payload.tag_list->entry) {
		const nbt_node* entry = list_entry(pos, struct nbt_list, entry)->data;
		/* Only TAG_COMPOUND entries can be entities (Beta's writer emits nothing
		 * else), and parse_entity below rejects and skips every non-compound
		 * anyway. Not counting them bounds the candidate calloc by the size of
		 * the already-parsed compound subtree instead of the raw list length,
		 * so a corrupt/hostile list of tiny scalar entries can no longer
		 * amplify into a multi-MB transient allocation on the 24MB-MEM1 Wii
		 * (bug-hunt D2). The >=MAX fail-closed guard stays: too many REAL
		 * compounds still refuses the chunk rather than truncating. */
		if(!entry || entry->type != TAG_COMPOUND)
			continue;
		if(candidates >= CHUNK_ENTITY_ARCHIVE_READ_MAX)
			return false; /* fail closed: truncation would erase the unseen tail */
		candidates++;
	}
	if(candidates == 0)
		return true;
	out->entities = calloc(candidates, sizeof(*out->entities));
	if(!out->entities)
		return false;

	list_for_each(pos, &entities_list->payload.tag_list->entry) {
		const nbt_node* comp = list_entry(pos, struct nbt_list, entry)->data;
		/* Mirror the counting skip so allocated slots always cover successes. */
		if(!comp || comp->type != TAG_COMPOUND)
			continue;
		if(parse_entity(comp, chunk_x, chunk_z, &out->entities[out->count]))
			out->count++;
	}
	if(out->count == 0) {
		free(out->entities);
		out->entities = NULL;
	}
	return true;
}

void chunk_entities_free(struct chunk_entities* entities) {
	if(!entities)
		return;
	free(entities->entities);
	entities->entities = NULL;
	entities->count = 0;
}

/* lava-aabb-0924/H3: the saved runtime identity of a record, exactly as the
 * parser admits it (read_u32: the CURRENT writer's TAG_LONG -- make_u32 has
 * emitted TAG_LONG since 34e50899, so every card on disk carries LONG -- and
 * the legacy TAG_INT of older archives). Zero / absent / out-of-range = no
 * identity, and the owner match falls back to Pos bytes. Before H3 the owner
 * comparison below honoured TAG_INT only, so against every real record the
 * runtime-id branch was dead: two entities at one exact position (an item
 * sink, same-tick death drops) made the chunk save fail closed (DUP_OWNER)
 * whenever the codec-complete fast path was defeated -- that chunk never
 * saved again. The ONE shared reader for both the linear reference scan and
 * the H2 owner index, so the two stay identical by construction. */
static bool owner_runtime_id(const nbt_node* rec, uint32_t* out) {
	uint32_t value;
	/* H5 (Opus H3 nit): read_u32 maps a legacy TAG_INT -1 to UINT32_MAX, the
	 * orphan-reference sentinel of the relation fields. A record carrying it
	 * as its OWN id has no identity either -- two such records must match by
	 * position, never collapse into one owner (DUP_OWNER). */
	if(!read_u32(rec, "CaveWiiRuntimeId", &value) || value == 0
	   || value == UINT32_MAX)
		return false;
	*out = value;
	return true;
}

/* IMP-ENTITY-PRESERVE: the position identity key is the FLOAT image of Pos.
 * Beta writes posX/posY/posZ as doubles [740c583 Entity.java:751
 * newDoubleNBTList], but archived_entity.pos (and the runtime) is float, so a
 * generated record carries (double)(float)saved. Comparing the raw double
 * bytes meant an imported Beta position a float cannot hold exactly (e.g.
 * 123.456789012) never found its own unchanged owner and the whole chunk save
 * failed closed (CEP_IMPORTED_UNMATCHED). Keying both sides by (float)value is
 * exactly the precision parse_entity admits, and is byte-identical to the old
 * key for every CaveWii-written (float-exact) record. Records whose Pos is not
 * three finite numbers have no key and never match by position, as before
 * (parse_entity rejects them anyway). */
#define OWNER_POS_KEY_BYTES 12
static bool owner_pos_key(const nbt_node* rec, uint8_t out[OWNER_POS_KEY_BYTES]) {
	const nbt_node* list = find_child(rec, "Pos");
	if(!list || list->type != TAG_LIST || !list->payload.tag_list)
		return false;
	size_t i = 0;
	struct list_head* p;
	list_for_each(p, &list->payload.tag_list->entry) {
		const nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(i >= 3 || !n)
			return false;
		double v;
		if(n->type == TAG_DOUBLE)
			v = n->payload.tag_double;
		else if(n->type == TAG_FLOAT)
			v = n->payload.tag_float;
		else
			return false;
		float f = (float)v;
		if(!isfinite(v) || !isfinite(f))
			return false;
		memcpy(out + 4 * i, &f, 4);
		i++;
	}
	return i == 3;
}

static bool entity_same_saved_owner(const nbt_node* a,const nbt_node* b) {
	const nbt_node* ai=find_child(a,"id"), *bi=find_child(b,"id");
	if(!ai || !bi || ai->type!=TAG_STRING || bi->type!=TAG_STRING
	   || strcmp(ai->payload.tag_string,bi->payload.tag_string)) return false;
	uint32_t ar, br;
	if(owner_runtime_id(a, &ar) && owner_runtime_id(b, &br)) return ar == br;
	uint8_t ka[OWNER_POS_KEY_BYTES], kb[OWNER_POS_KEY_BYTES];
	return owner_pos_key(a, ka) && owner_pos_key(b, kb)
		&& !memcmp(ka, kb, sizeof(ka));
}

/* IMP-ENTITY-PRESERVE: a generated double that is the float image of the
 * saved one is the codec's rendering of the SAME value; keep the saved bytes
 * so an unchanged imported entity keeps Beta's exact double. Lists (Pos,
 * Motion) qualify only when every element does. */
static bool double_value(const nbt_node* n, double* out) {
	if(!n || n->type != TAG_DOUBLE)
		return false;
	*out = n->payload.tag_double;
	return true;
}
static bool saved_double_is_image(const nbt_node* saved,
								  const nbt_node* generated) {
	double s, g;
	if(double_value(saved, &s) && double_value(generated, &g))
		return isfinite(s) && (double)(float)s == g;
	if(!saved || !generated || saved->type != TAG_LIST
	   || generated->type != TAG_LIST || !saved->payload.tag_list
	   || !generated->payload.tag_list)
		return false;
	struct list_head* ps = saved->payload.tag_list->entry.flink;
	struct list_head* pg = generated->payload.tag_list->entry.flink;
	size_t n = 0;
	while(ps != &saved->payload.tag_list->entry
		  && pg != &generated->payload.tag_list->entry) {
		if(!double_value(list_entry(ps, struct nbt_list, entry)->data, &s)
		   || !double_value(list_entry(pg, struct nbt_list, entry)->data, &g)
		   || !isfinite(s) || (double)(float)s != g)
			return false;
		ps = ps->flink;
		pg = pg->flink;
		n++;
	}
	return n > 0 && ps == &saved->payload.tag_list->entry
		&& pg == &generated->payload.tag_list->entry;
}

static bool beta_vector_shape(const nbt_node* n, nbt_type element_type,
							  size_t count) {
	if(!n || n->type != TAG_LIST || !n->payload.tag_list
	   || !n->payload.tag_list->data
	   || n->payload.tag_list->data->type != element_type)
		return false;
	size_t seen = 0;
	struct list_head* p;
	list_for_each(p, &n->payload.tag_list->entry) {
		const nbt_node* element = list_entry(p, struct nbt_list, entry)->data;
		if(!element || element->type != element_type || ++seen > count)
			return false;
	}
	return seen == count;
}

static bool beta_item_payload_owned(const nbt_node* item,
								struct item_data* decoded) {
	if(!item || item->type != TAG_COMPOUND || !item->payload.tag_compound)
		return false;
	unsigned mask = 0;
	struct list_head* p;
	list_for_each(p, &item->payload.tag_compound->entry) {
		const nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		unsigned bit = 0;
		nbt_type type = TAG_INVALID;
		if(n && n->name && !strcmp(n->name, "id")) {
			bit = 1u << 0;
			type = TAG_SHORT;
		} else if(n && n->name && !strcmp(n->name, "Count")) {
			bit = 1u << 1;
			type = TAG_BYTE;
		} else if(n && n->name && !strcmp(n->name, "Damage")) {
			bit = 1u << 2;
			type = TAG_SHORT;
		} else
			return false;
		if(n->type != type || (mask & bit))
			return false;
		mask |= bit;
	}
	if(mask != 0x7u || !parse_item(item, decoded)
	   || item_data_pinned(decoded))
		return false;
	nbt_node* emitted = calloc(1, sizeof(*emitted));
	if(!emitted)
		return false;
	bool ok = item_nbt_emit(emitted, decoded, -1);
	nbt_node unnamed = *item;
	unnamed.name = NULL;
	ok = ok && item_nbt_equal(&unnamed, emitted);
	nbt_free(emitted);
	return ok;
}

/* IMP-ENTITY-PRESERVE: Beta EnumArt titles [740c583 EnumArt.java], kept
 * here (not entity_painting.c's table) so this codec stays free of live
 * entity code, like the rest of this file's hand-mirrored shapes. An unknown
 * title loads as Kebab in Beta and here, so it is NOT owned: the record keeps
 * its exact bytes through the preserving merge instead of being rewritten. */
static bool beta_art_title(const char* title) {
	static const char* const titles[] = {
		"Kebab", "Aztec", "Alban", "Aztec2", "Bomb", "Plant", "Wasteland",
		"Pool", "Courbet", "Sea", "Sunset", "Creebet", "Wanderer", "Graham",
		"Match", "Bust", "Stage", "Void", "SkullAndRoses", "Fighters",
		"Pointer", "Pigscene", "BurningSkull", "Skeleton", "DonkeyKong",
	};
	for(size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
		if(!strcmp(title, titles[i]))
			return true;
	return false;
}

/* Chest-cart cargo [740c583 EntityMinecart.java:604-616]: one compound per
 * non-empty slot, `Slot` byte plus ItemStack.writeToNBT (id/Count/Damage).
 * Owned only when every element decoded to the canonical plain record
 * (nbt_record == 0: byte-identical to the codec's own encoding) in a unique
 * slot and no decoded slot lacks its element. */
static bool beta_cart_items_owned(const nbt_node* list,
								  const struct item_data* inventory,
								  size_t slots) {
	if(!list || list->type != TAG_LIST || !list->payload.tag_list)
		return false;
	bool seen[27] = {false};
	if(slots > sizeof(seen) / sizeof(seen[0]))
		return false;
	size_t elements = 0;
	struct list_head* p;
	list_for_each(p, &list->payload.tag_list->entry) {
		const nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(!n || n->type != TAG_COMPOUND)
			return false;
		const nbt_node* slot = find_child(n, "Slot");
		if(!slot || slot->type != TAG_BYTE || slot->payload.tag_byte < 0
		   || (size_t)slot->payload.tag_byte >= slots
		   || seen[slot->payload.tag_byte])
			return false;
		const struct item_data* it = &inventory[slot->payload.tag_byte];
		if(!it->id || !it->count || item_data_pinned(it))
			return false;
		seen[slot->payload.tag_byte] = true;
		elements++;
	}
	size_t occupied = 0;
	for(size_t i = 0; i < slots; i++)
		occupied += inventory[i].id != 0 || inventory[i].count != 0;
	return occupied == elements;
}

/* IMP-ENTITY-PRESERVE: exact pure-Beta painting / minecart / boat records.
 * These are not EntityLiving: Beta never changes their Air from the
 * constructor value 300 (only EntityLiving.onEntityUpdate touches air
 * [740c583 Entity.java:57, EntityLiving.java:123-139]), and the codec's
 * canonical record omits Air for them, so any other value is not owned.
 * Health is not a Beta tag for these kinds. */
static bool beta_nonliving_owned(const nbt_node* comp, const char* id,
								 unsigned specific,
								 const struct archived_entity* decoded) {
	const nbt_node* air = find_child(comp, "Air");
	const nbt_node* fire = find_child(comp, "Fire");
	if(air->payload.tag_short != 300 || decoded->air != 300
	   || fire->payload.tag_short != decoded->fire)
		return false;
	if(decoded->kind == ARCHIVED_ENTITY_BOAT)
		return !strcmp(id, "Boat") && specific == 0
			&& decoded->data.boat.blast_damage == 0;
	if(decoded->kind == ARCHIVED_ENTITY_PAINTING) {
		const unsigned want = (1u << 13) | (1u << 14) | (1u << 15)
			| (1u << 16) | (1u << 17);
		if(strcmp(id, "Painting") || specific != want)
			return false;
		const struct archived_painting_state* ps = &decoded->data.painting;
		const nbt_node* motive = find_child(comp, "Motive");
		return find_child(comp, "Dir")->payload.tag_byte == ps->direction
			&& find_child(comp, "TileX")->payload.tag_int == ps->tile_x
			&& find_child(comp, "TileY")->payload.tag_int == ps->tile_y
			&& find_child(comp, "TileZ")->payload.tag_int == ps->tile_z
			&& motive->payload.tag_string
			&& strlen(motive->payload.tag_string) < sizeof(ps->art_name)
			&& !strcmp(motive->payload.tag_string, ps->art_name)
			&& beta_art_title(ps->art_name);
	}
	const struct archived_minecart_state* mc = &decoded->data.minecart;
	if(strcmp(id, "Minecart") || !(specific & (1u << 18)))
		return false;
	int32_t type = find_child(comp, "Type")->payload.tag_int;
	if(type < 0 || type > 2 || (uint8_t)type != mc->minecart_type
	   || mc->speed != 0.0f || mc->hx || mc->hz || mc->last_meta
	   || mc->occupied || mc->occupant_source_id || mc->blast_damage)
		return false;
	const size_t slots = sizeof(mc->inventory) / sizeof(mc->inventory[0]);
	if(type == 0) {
		for(size_t i = 0; i < slots; i++)
			if(mc->inventory[i].id || mc->inventory[i].count)
				return false;
		return specific == (1u << 18) && mc->push_x == 0.0f
			&& mc->push_z == 0.0f && mc->fuel == 0;
	}
	if(type == 1)
		return specific == ((1u << 18) | (1u << 22)) && mc->push_x == 0.0f
			&& mc->push_z == 0.0f && mc->fuel == 0
			&& beta_cart_items_owned(find_child(comp, "Items"),
									 mc->inventory, slots);
	/* Furnace cart: PushX/PushZ doubles narrowed to the float runtime field
	 * (the same admitted precision as Pos/Motion), Fuel short. */
	const nbt_node* px = find_child(comp, "PushX");
	const nbt_node* pz = find_child(comp, "PushZ");
	if(specific != ((1u << 18) | (1u << 19) | (1u << 20) | (1u << 21))
	   || !isfinite(px->payload.tag_double) || !isfinite(pz->payload.tag_double)
	   || (float)px->payload.tag_double != mc->push_x
	   || (float)pz->payload.tag_double != mc->push_z
	   || find_child(comp, "Fuel")->payload.tag_short != mc->fuel)
		return false;
	for(size_t i = 0; i < slots; i++)
		if(mc->inventory[i].id || mc->inventory[i].count)
			return false;
	return true;
}

/* Strict ownership for the exact pure-Beta records admitted from the recovered
 * world. Every admitted scalar is represented by archived_entity; unknown,
 * duplicate or wrong-type tags remain outside this destructive set path. */
static bool entity_beta_import_owned(const nbt_node* comp, w_coord_t x,
									 w_coord_t z,
									 struct archived_entity* decoded) {
	if(!parse_entity(comp, x, z, decoded) || decoded->source_id != 0)
		return false;
	const char* id = NULL;
	unsigned common = 0, specific = 0;
	struct list_head* p;
	list_for_each(p, &comp->payload.tag_compound->entry) {
		const nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(!n || !n->name)
			return false;
		unsigned bit = 0;
		nbt_type type = TAG_INVALID;
		if(!strcmp(n->name, "id")) { bit = 1u << 0; type = TAG_STRING; }
		else if(!strcmp(n->name, "Pos")) { bit = 1u << 1; type = TAG_LIST; }
		else if(!strcmp(n->name, "Motion")) { bit = 1u << 2; type = TAG_LIST; }
		else if(!strcmp(n->name, "Rotation")) { bit = 1u << 3; type = TAG_LIST; }
		else if(!strcmp(n->name, "FallDistance")) { bit = 1u << 4; type = TAG_FLOAT; }
		else if(!strcmp(n->name, "Fire")) { bit = 1u << 5; type = TAG_SHORT; }
		else if(!strcmp(n->name, "Air")) { bit = 1u << 6; type = TAG_SHORT; }
		else if(!strcmp(n->name, "OnGround")) { bit = 1u << 7; type = TAG_BYTE; }
		if(bit) {
			if(n->type != type || (common & bit))
				return false;
			common |= bit;
			if(bit == (1u << 0))
				id = n->payload.tag_string;
			continue;
		}

		bit = 0;
		if(!strcmp(n->name, "Health")) { bit = 1u << 0; type = TAG_SHORT; }
		else if(!strcmp(n->name, "Age")) { bit = 1u << 1; type = TAG_SHORT; }
		else if(!strcmp(n->name, "Item")) { bit = 1u << 2; type = TAG_COMPOUND; }
		else if(!strcmp(n->name, "HurtTime")) { bit = 1u << 3; type = TAG_SHORT; }
		else if(!strcmp(n->name, "DeathTime")) { bit = 1u << 4; type = TAG_SHORT; }
		else if(!strcmp(n->name, "AttackTime")) { bit = 1u << 5; type = TAG_SHORT; }
		else if(!strcmp(n->name, "powered")) { bit = 1u << 6; type = TAG_BYTE; }
		else if(!strcmp(n->name, "Saddle")) { bit = 1u << 7; type = TAG_BYTE; }
		else if(!strcmp(n->name, "Color")) { bit = 1u << 8; type = TAG_BYTE; }
		else if(!strcmp(n->name, "Sheared")) { bit = 1u << 9; type = TAG_BYTE; }
		else if(!strcmp(n->name, "Angry")) { bit = 1u << 10; type = TAG_BYTE; }
		else if(!strcmp(n->name, "Sitting")) { bit = 1u << 11; type = TAG_BYTE; }
		else if(!strcmp(n->name, "Owner")) { bit = 1u << 12; type = TAG_STRING; }
		/* IMP-ENTITY-PRESERVE: EntityPainting.writeEntityToNBT [740c583
		 * EntityPainting.java:210-216] and EntityMinecart.writeEntityToNBT
		 * [740c583 EntityMinecart.java:598-617]. EntityBoat writes none
		 * [740c583 EntityBoat.java:326]. */
		else if(!strcmp(n->name, "Dir")) { bit = 1u << 13; type = TAG_BYTE; }
		else if(!strcmp(n->name, "Motive")) { bit = 1u << 14; type = TAG_STRING; }
		else if(!strcmp(n->name, "TileX")) { bit = 1u << 15; type = TAG_INT; }
		else if(!strcmp(n->name, "TileY")) { bit = 1u << 16; type = TAG_INT; }
		else if(!strcmp(n->name, "TileZ")) { bit = 1u << 17; type = TAG_INT; }
		else if(!strcmp(n->name, "Type")) { bit = 1u << 18; type = TAG_INT; }
		else if(!strcmp(n->name, "PushX")) { bit = 1u << 19; type = TAG_DOUBLE; }
		else if(!strcmp(n->name, "PushZ")) { bit = 1u << 20; type = TAG_DOUBLE; }
		else if(!strcmp(n->name, "Fuel")) { bit = 1u << 21; type = TAG_SHORT; }
		else if(!strcmp(n->name, "Items")) { bit = 1u << 22; type = TAG_LIST; }
		else return false;
		if(n->type != type || (specific & bit))
			return false;
		specific |= bit;
	}
	if(common != 0xffu || !id || !beta_vector_shape(find_child(comp, "Pos"), TAG_DOUBLE, 3)
	   || !beta_vector_shape(find_child(comp, "Motion"), TAG_DOUBLE, 3)
	   || !beta_vector_shape(find_child(comp, "Rotation"), TAG_FLOAT, 2))
		return false;
	const nbt_node* on_ground = find_child(comp, "OnGround");
	if(on_ground->payload.tag_byte != 0 && on_ground->payload.tag_byte != 1)
		return false;
	const nbt_node* fall = find_child(comp, "FallDistance");
	if(!isfinite(fall->payload.tag_float) || fall->payload.tag_float < 0.0f
	   || fall->payload.tag_float > 4096.0f
	   || fall->payload.tag_float != decoded->fall_distance)
		return false;

	if(decoded->kind == ARCHIVED_ENTITY_ITEM) {
		if(strcmp(id, "Item") || specific != 0x7u)
			return false;
		const nbt_node* health = find_child(comp, "Health");
		const nbt_node* age = find_child(comp, "Age");
		if(health->payload.tag_short <= 0 || health->payload.tag_short > 5
		   || health->payload.tag_short != decoded->health
		   || age->payload.tag_short < 0
		   || age->payload.tag_short > ARCHIVED_ITEM_AGE_MAX
		   || age->payload.tag_short != decoded->data.item.age)
			return false;
		struct item_data exact;
		return beta_item_payload_owned(find_child(comp, "Item"), &exact);
	}
	if(decoded->kind == ARCHIVED_ENTITY_PAINTING
	   || decoded->kind == ARCHIVED_ENTITY_MINECART
	   || decoded->kind == ARCHIVED_ENTITY_BOAT)
		return beta_nonliving_owned(comp, id, specific, decoded);
	if(decoded->kind != ARCHIVED_ENTITY_MONSTER
	   || (strcmp(id, "Creeper") && strcmp(id, "Zombie")
		   && strcmp(id, "Skeleton") && strcmp(id, "Chicken")
		   && strcmp(id, "Spider") && strcmp(id, "Cow")
		   && strcmp(id, "Squid") && strcmp(id, "Pig")
		   && strcmp(id, "Sheep") && strcmp(id, "Wolf")))
		return false;
	unsigned required = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5);
	bool creeper = !strcmp(id, "Creeper");
	unsigned expected = required;
	if(!strcmp(id, "Pig"))
		expected |= 1u << 7;
	else if(!strcmp(id, "Sheep"))
		expected |= (1u << 8) | (1u << 9);
	else if(!strcmp(id, "Wolf"))
		expected |= (1u << 10) | (1u << 11) | (1u << 12);
	if((creeper && specific != expected
		&& specific != (expected | (1u << 6)))
	   || (!creeper && specific != expected))
		return false;
	const nbt_node* fire = find_child(comp, "Fire");
	const nbt_node* air = find_child(comp, "Air");
	const nbt_node* hurt = find_child(comp, "HurtTime");
	const nbt_node* death = find_child(comp, "DeathTime");
	const nbt_node* attack = find_child(comp, "AttackTime");
	if(fire->payload.tag_short < -1
	   || fire->payload.tag_short > MOB_LAVA_FIRE_TICKS
	   || fire->payload.tag_short != decoded->data.monster.fire_time
	   || air->payload.tag_short < MOB_DROWN_AIR_THRESHOLD
	   || air->payload.tag_short > MOB_AIR_MAX
	   || air->payload.tag_short != decoded->data.monster.air
	   || hurt->payload.tag_short != decoded->data.monster.hurt_time
	   || death->payload.tag_short != decoded->data.monster.death_time
	   || attack->payload.tag_short != decoded->data.monster.attack_time)
		return false;
	if(creeper) {
		const nbt_node* powered = find_child(comp, "powered");
		return !powered || powered->payload.tag_byte == 0
			|| powered->payload.tag_byte == 1;
	}
	if(!strcmp(id, "Pig")) {
		const nbt_node* saddle = find_child(comp, "Saddle");
		return (saddle->payload.tag_byte == 0 || saddle->payload.tag_byte == 1)
			&& decoded->data.monster.saddled
				== (saddle->payload.tag_byte != 0);
	}
	if(!strcmp(id, "Sheep")) {
		const nbt_node* color = find_child(comp, "Color");
		const nbt_node* sheared = find_child(comp, "Sheared");
		return color->payload.tag_byte >= 0 && color->payload.tag_byte <= 15
			&& (sheared->payload.tag_byte == 0 || sheared->payload.tag_byte == 1)
			&& decoded->data.monster.fleece_color
				== (uint8_t)color->payload.tag_byte
			&& decoded->data.monster.sheared
				== (sheared->payload.tag_byte != 0);
	}
	if(!strcmp(id, "Wolf")) {
		const nbt_node* angry = find_child(comp, "Angry");
		const nbt_node* sitting = find_child(comp, "Sitting");
		const nbt_node* owner = find_child(comp, "Owner");
		return (angry->payload.tag_byte == 0 || angry->payload.tag_byte == 1)
			&& (sitting->payload.tag_byte == 0
				|| sitting->payload.tag_byte == 1)
			&& owner->payload.tag_string
			&& strlen(owner->payload.tag_string)
				< sizeof(decoded->data.monster.wolf_owner)
			&& decoded->data.monster.wolf_angry
				== (angry->payload.tag_byte != 0)
			&& decoded->data.monster.wolf_sitting
				== (sitting->payload.tag_byte != 0)
			&& !strcmp(decoded->data.monster.wolf_owner,
				owner->payload.tag_string);
	}
	return specific == required;
}

/* CaveWiiType and CaveWiiRuntimeId were introduced together by the original
 * archive writer (34e50899). Only that exact pair is provenance: an arbitrary
 * CaveWii-prefixed field must not let malformed imported state escape the
 * strict recovery classifier. */
static bool entity_has_legacy_provenance(
	const nbt_node* comp, const struct archived_entity* decoded) {
	if(!decoded || decoded->source_id == 0)
		return false;
	const nbt_node *type = NULL, *runtime_id = NULL;
	struct list_head* p;
	list_for_each(p, &comp->payload.tag_compound->entry) {
		const nbt_node* n = list_entry(p, struct nbt_list, entry)->data;
		if(!n || !n->name)
			continue;
		if(!strcmp(n->name, "CaveWiiType")) {
			if(type)
				return false;
			type = n;
		} else if(!strcmp(n->name, "CaveWiiRuntimeId")) {
			if(runtime_id)
				return false;
			runtime_id = n;
		}
	}
	if(!type || type->type != TAG_INT
	   || type->payload.tag_int != (int32_t)decoded->kind || !runtime_id)
		return false;
	uint32_t value;
	return read_u32(comp, "CaveWiiRuntimeId", &value)
		&& value != 0 && value == decoded->source_id;
}

/* A malformed record from the supported pure-Beta recovery cohort must never
 * fall through to the Pos-based preserving rewrite: that would normalize an
 * invalid/unknown Beta field while retaining the same apparent owner. Proven
 * legacy CaveWii records keep the legacy preserving path. Paintings, minecarts
 * and boats are owned (IMP-ENTITY-PRESERVE) but deliberately NOT in this
 * fail-closed cohort: an unowned variant of them (e.g. a server-plugin tag)
 * takes the preserving merge instead of blocking the save; other Beta kinds
 * (arrows, etc.) remain outside this bounded repair. */
static bool entity_beta_import_cohort(const nbt_node* comp,
									 const struct archived_entity* decoded) {
	const nbt_node* id = find_child(comp, "id");
	if(!id || id->type != TAG_STRING || !id->payload.tag_string)
		return false;
	const char* value = id->payload.tag_string;
	bool supported = !strcmp(value, "Item") || !strcmp(value, "Creeper")
		|| !strcmp(value, "Zombie") || !strcmp(value, "Skeleton")
		|| !strcmp(value, "Chicken") || !strcmp(value, "Spider")
		|| !strcmp(value, "Cow") || !strcmp(value, "Squid")
		|| !strcmp(value, "Pig") || !strcmp(value, "Sheep")
		|| !strcmp(value, "Wolf");
	if(!supported)
		return false;
	return !entity_has_legacy_provenance(comp, decoded);
}

/* Deleting a native record is safe only when the runtime codec accounts for
 * every byte. Otherwise absence might hide unknown state or a lost identity. */
/* java-load-1023: the Beta-named short tags that pre-fix CaveWii writers
 * emitted as TAG_INT (see add_short_clamped). Item: Age. Monster: AttackTime,
 * HurtTime, Air, DeathTime. A legacy record differs from today's encoding ONLY
 * in those tags' type byte (same names, order and value). */
static bool legacy_int_tag(enum archived_entity_kind kind, const char* name) {
	if(kind == ARCHIVED_ENTITY_ITEM)
		return !strcmp(name, "Age");
	if(kind == ARCHIVED_ENTITY_MONSTER)
		return !strcmp(name, "AttackTime") || !strcmp(name, "HurtTime")
			|| !strcmp(name, "Air") || !strcmp(name, "DeathTime");
	return false;
}

/* Rewrite a lossless clone of a fresh encoding into a historical shape:
 * int_tags turns legacy_int_tag()s back into TAG_INT with the same value;
 * strip_air removes the item's common Air scalar. int_tags also drops the
 * monster's Beta sheep/slime tags (Sheared, Color, Size) that pre-fix
 * records never had. */
static void legacy_shape(nbt_node* rec, enum archived_entity_kind kind,
						 bool int_tags, bool strip_air) {
	struct list_head *p, *next;
	list_for_each_safe(p, next, &rec->payload.tag_compound->entry) {
		struct nbt_list* w = list_entry(p, struct nbt_list, entry);
		nbt_node* n = w->data;
		if(!n || !n->name)
			continue;
		if((strip_air && !strcmp(n->name, "Air"))
		   || (int_tags && kind == ARCHIVED_ENTITY_MONSTER
			   && (!strcmp(n->name, "Sheared") || !strcmp(n->name, "Color")
				   || !strcmp(n->name, "Size")))) {
			list_del(p); nbt_free(n); free(w); continue;
		}
		if(int_tags && n->type == TAG_SHORT && legacy_int_tag(kind, n->name)) {
			int32_t v = n->payload.tag_short;
			n->type = TAG_INT;
			n->payload.tag_int = v;
		}
	}
}

static bool entity_native_deletion_safe(const nbt_node* old,
	struct archived_entity* decoded) {
	struct chunk_entities one={.entities=decoded,.count=1};
	void* arena=NULL;
	struct nbt_list* list=chunk_entities_build(&one,&arena);
	nbt_node* exact=item_nbt_clone_lossless(old);
	bool same=list && exact && !list_empty(&list->entry)
		&& item_nbt_equal(exact,list_entry(list->entry.flink,struct nbt_list,entry)->data);
	/* save-enc-1030: every build before it wrote the legacy footprint (the
	 * wolf/pig/pig-zombie/creeper tags on every monster, an empty cart Items
	 * list typed TAG_COMPOUND). Those records are still byte-proven against
	 * that exact encoding, and the historical variants below are rewrites of
	 * it -- they all predate save-enc-1030. Kinds the footprint does not
	 * touch encode identically either way and reuse `list`. */
	void* legacy_arena=NULL;
	struct nbt_list* base=list;
	bool footprint_kind = decoded->kind == ARCHIVED_ENTITY_MONSTER
		|| decoded->kind == ARCHIVED_ENTITY_MINECART;
	if(!same && list && footprint_kind) {
		base=chunk_entities_build_ex(&one,&legacy_arena,true);
		same=base && exact && !list_empty(&base->entry)
			&& item_nbt_equal(exact,list_entry(base->entry.flink,struct nbt_list,entry)->data);
	}
	/* Released native schemas that predate the current encoding, each proven
	 * by rewriting a freshly encoded clone into that exact historical shape; no
	 * other mismatch is deletion-safe:
	 *  - pre-java-load-1023 records wrote the legacy_int_tag()s as TAG_INT and
	 *    had no Beta sheep "Sheared"/"Color" or slime "Size" (CaveWii* copies
	 *    and the monster id carried that state);
	 *  - one released Item schema (int Age) predates the common Air scalar.
	 *    The no-Air check is also kept for a short Age (the pre-existing
	 *    "current encoding minus Air" proof the preserve/storm tests pin). */
	bool item = decoded->kind == ARCHIVED_ENTITY_ITEM;
	bool legacy_kind = item || decoded->kind == ARCHIVED_ENTITY_MONSTER;
	/* The variants run over BOTH encodings: the legacy footprint is the true
	 * historical base, and the current one keeps every pre-existing proof
	 * (fixtures that rewrite the current encoding into an old shape). Either
	 * way the whole record is still re-derived from `decoded` byte for byte,
	 * so no unaccounted state can be deleted. */
	struct nbt_list* bases[2] = {list, base != list ? base : NULL};
	for(int b = 0; !same && b < 2; b++)
	for(int variant = 0; !same && legacy_kind && bases[b]
		&& !list_empty(&bases[b]->entry) && variant < 3; variant++) {
		bool int_tags = variant != 1;
		bool strip_air = variant != 0;
		if(strip_air && (!item || find_child(old, "Air")))
			continue;
		nbt_node* legacy = item_nbt_clone_lossless(
			list_entry(bases[b]->entry.flink, struct nbt_list, entry)->data);
		if(!legacy)
			break;
		legacy_shape(legacy, decoded->kind, int_tags, strip_air);
		same = item_nbt_equal(old, legacy);
		nbt_free(legacy);
	}
	nbt_free(exact);chunk_entities_build_free(arena);
	chunk_entities_build_free(legacy_arena);
	return same;
}

/* lava-aabb-0924/H1: last fail-closed exit taken (see the header). Written on
 * the saving (server) thread, read on main by the heartbeat: relaxed atomic. */
static uint32_t s_preserve_last_fail = CEP_NONE;
enum chunk_entities_preserve_fail chunk_entities_preserve_last_fail(void) {
	return (enum chunk_entities_preserve_fail)__atomic_load_n(
		&s_preserve_last_fail, __ATOMIC_RELAXED);
}
static void preserve_note_fail(enum chunk_entities_preserve_fail why) {
	__atomic_store_n(&s_preserve_last_fail, (uint32_t)why, __ATOMIC_RELAXED);
}
#define PRESERVE_FAIL(why) do { preserve_note_fail(why); goto fail; } while(0)

/* ---- lava-aabb-0924/H2: O(n log n) owner search for the preserving merge ----
 *
 * The merge below used to find each old record's generated owner by scanning
 * the WHOLE generated list with entity_same_saved_owner (two nbt_dump_binary
 * mallocs per pair): O(n_old * n_generated) pairs, measured 3.7 s for a 4096-
 * record chunk on PC (tests/test_autosave_entity_storm.c) -- the autosave
 * stall candidate. This index reproduces entity_same_saved_owner EXACTLY:
 *   match(a, n) := id strings equal
 *                  && (both carry a nonzero CaveWiiRuntimeId (owner_runtime_id:
 *                      TAG_LONG or legacy TAG_INT, H3)
 *                        ? the ids are equal
 *                        : owner_pos_key(a) == owner_pos_key(n), both
 *                          present -- the float image of Pos, IMP-ENTITY-
 *                          PRESERVE)
 * Every generated record contributes up to two keys: a RID key when it has a
 * nonzero runtime id, and a POS key (FNV-1a 64 of its 12-byte float Pos key,
 * flagged with whether the record ALSO has a runtime id). An old record probes
 * the RID range when it has a runtime id, then the POS range restricted to
 * records WITHOUT one (or, without a runtime id of its own, the whole POS
 * range); every POS hash hit is verified byte-for-byte before it counts, so a
 * hash collision can only cost a verify, never a false match. The search stops
 * at the second confirmed match: the merge fails closed there anyway.
 *
 * Scratch: ONE malloc of (2 * generated count) keys, 40 bytes each (5.2 MB at
 * CHUNK_ENTITY_ARCHIVE_WRITE_MAX, freed before the merge returns); nothing is
 * retained across saves. If that malloc fails the merge falls back to the
 * linear reference scan (identical output, only slower) -- no new failure
 * mode. tests/test_entity_preserve_equiv.c proves the two paths emit identical
 * bytes and identical fail sub-codes on randomized and adversarial input. */
enum { OWNER_KEY_RID = 0, OWNER_KEY_POS = 1 };
struct owner_key {
	const nbt_node* node; /* the generated record */
	const char* id;       /* its id string (TAG_STRING payload, non-NULL) */
	uint64_t hash;        /* POS: FNV-1a 64 of owner_pos_key */
	uint32_t len;         /* POS: OWNER_POS_KEY_BYTES */
	uint32_t rid;         /* RID: the nonzero CaveWiiRuntimeId (owner_runtime_id) */
	uint32_t gen_index;   /* position in `generated` == current->entities[] */
	uint8_t kind;         /* OWNER_KEY_RID / OWNER_KEY_POS */
	uint8_t has_rid;      /* POS: the record ALSO carries a nonzero runtime id */
};
struct owner_index {
	struct owner_key* keys;
	size_t count;
	bool reference; /* no index: the linear entity_same_saved_owner scan */
};
struct owner_match {
	size_t count; /* confirmed owners found, saturates at 2 */
	size_t index; /* generated index of the FIRST confirmed owner */
	const nbt_node* node;
};

#ifdef CAVEWII_PRESERVE_TEST
/* Review fix (3): with this set every POS key hashes to one constant, so every
 * probe walks the whole equal-range and is decided by the byte verify alone --
 * the collision path the equivalence test must also prove. */
static bool s_preserve_test_constant_hash;
void chunk_entities_preserve_test_constant_hash(bool on) {
	s_preserve_test_constant_hash = on;
}
#endif
/* Review fix (4): index scratch mallocs that failed and fell back to the
 * linear reference scan (identical output, quadratic time). Heartbeat
 * "save_idx_fb"; relaxed atomic, saving thread writes, main reads. */
static uint32_t s_preserve_index_fallback_n;
uint32_t chunk_entities_preserve_index_fallbacks(void) {
	return __atomic_load_n(&s_preserve_index_fallback_n, __ATOMIC_RELAXED);
}
static uint64_t owner_fnv1a(const uint8_t* p, size_t n) {
#ifdef CAVEWII_PRESERVE_TEST
	if(s_preserve_test_constant_hash)
		return UINT64_C(0x5eed5eed5eed5eed);
#endif
	uint64_t h = UINT64_C(14695981039346656037);
	for(size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= UINT64_C(1099511628211);
	}
	return h;
}
/* The id string exactly as entity_same_saved_owner reads it; NULL never
 * matches anything (that function returns false for a non-string id). */
static const char* owner_id_string(const nbt_node* rec) {
	const nbt_node* id = find_child(rec, "id");
	return id && id->type == TAG_STRING ? id->payload.tag_string : NULL;
}
/* The same nonzero-runtime-id gate entity_same_saved_owner applies (H3: one
 * shared reader, TAG_LONG or legacy TAG_INT). */
static bool owner_rid(const nbt_node* rec, uint32_t* out) {
	return owner_runtime_id(rec, out);
}
/* Key order without the has_rid tail: an equal-range probe over this order
 * finds every candidate; has_rid is filtered while walking the range. */
static int owner_key_core_cmp(const struct owner_key* a,
							  const struct owner_key* b) {
	int c = strcmp(a->id, b->id);
	if(c)
		return c;
	if(a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	if(a->kind == OWNER_KEY_RID)
		return a->rid < b->rid ? -1 : a->rid > b->rid;
	if(a->len != b->len)
		return a->len < b->len ? -1 : 1;
	if(a->hash != b->hash)
		return a->hash < b->hash ? -1 : 1;
	return 0;
}
static int owner_key_qsort_cmp(const void* x, const void* y) {
	const struct owner_key *a = x, *b = y;
	int c = owner_key_core_cmp(a, b);
	if(c)
		return c;
	if(a->has_rid != b->has_rid)
		return a->has_rid < b->has_rid ? -1 : 1;
	return a->gen_index < b->gen_index ? -1 : a->gen_index > b->gen_index;
}
static bool owner_index_build(struct owner_index* idx,
							  const nbt_node* generated) {
	idx->keys = NULL;
	idx->count = 0;
	idx->reference = false;
	size_t n = 0;
	struct list_head* q;
	list_for_each(q, &generated->payload.tag_list->entry)
		n++;
	if(!n)
		return true;
	if(n > SIZE_MAX / (2 * sizeof(*idx->keys)))
		return false;
	idx->keys = malloc(2 * n * sizeof(*idx->keys));
	if(!idx->keys)
		return false;
	uint32_t gi = 0;
	list_for_each(q, &generated->payload.tag_list->entry) {
		const nbt_node* rec = list_entry(q, struct nbt_list, entry)->data;
		const char* id = owner_id_string(rec);
		if(id) {
			uint32_t rid = 0;
			bool has = owner_rid(rec, &rid);
			if(has)
				idx->keys[idx->count++] = (struct owner_key) {
					.node = rec, .id = id, .rid = rid, .gen_index = gi,
					.kind = OWNER_KEY_RID,
				};
			/* A record without a float Pos key never matches by position in
			 * entity_same_saved_owner either: no key. */
			uint8_t key[OWNER_POS_KEY_BYTES];
			if(owner_pos_key(rec, key))
				idx->keys[idx->count++] = (struct owner_key) {
					.node = rec, .id = id, .gen_index = gi,
					.kind = OWNER_KEY_POS, .has_rid = has,
					.len = OWNER_POS_KEY_BYTES,
					.hash = owner_fnv1a(key, sizeof(key)),
				};
		}
		gi++;
	}
	qsort(idx->keys, idx->count, sizeof(*idx->keys), owner_key_qsort_cmp);
	return true;
}
static void owner_index_free(struct owner_index* idx) {
	free(idx->keys);
	idx->keys = NULL;
	idx->count = 0;
}
/* First key whose core order is >= probe (lower bound). */
static size_t owner_index_lower(const struct owner_index* idx,
								const struct owner_key* probe) {
	size_t lo = 0, hi = idx->count;
	while(lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if(owner_key_core_cmp(&idx->keys[mid], probe) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}
static void owner_match_add(struct owner_match* m, size_t index,
							const nbt_node* node) {
	if(m->count == 0) {
		m->index = index;
		m->node = node;
	}
	if(m->count < 2)
		m->count++;
}
/* Owners of `old` among `generated` -- count (saturating at 2) and the first
 * one. The reference path is the pre-H2 scan, kept verbatim as the oracle. */
static void owner_search(const struct owner_index* idx, const nbt_node* old,
						 const nbt_node* generated, struct owner_match* m) {
	m->count = 0;
	m->index = 0;
	m->node = NULL;
	if(idx->reference) {
		struct list_head* q;
		size_t index = 0;
		list_for_each(q, &generated->payload.tag_list->entry) {
			const nbt_node* n = list_entry(q, struct nbt_list, entry)->data;
			if(entity_same_saved_owner(old, n)) {
				owner_match_add(m, index, n);
				if(m->count >= 2)
					return;
			}
			index++;
		}
		return;
	}
	const char* id = owner_id_string(old);
	if(!id)
		return;
	uint32_t rid = 0;
	bool has = owner_rid(old, &rid);
	if(has) {
		struct owner_key probe = {.id = id, .kind = OWNER_KEY_RID, .rid = rid};
		for(size_t k = owner_index_lower(idx, &probe); k < idx->count; k++) {
			const struct owner_key* e = &idx->keys[k];
			if(owner_key_core_cmp(e, &probe) != 0)
				break;
			owner_match_add(m, e->gen_index, e->node); /* exact key */
			if(m->count >= 2)
				return;
		}
	}
	uint8_t key[OWNER_POS_KEY_BYTES];
	if(!owner_pos_key(old, key))
		return;
	struct owner_key probe = {.id = id, .kind = OWNER_KEY_POS,
							  .len = OWNER_POS_KEY_BYTES,
							  .hash = owner_fnv1a(key, sizeof(key))};
	for(size_t k = owner_index_lower(idx, &probe); k < idx->count; k++) {
		const struct owner_key* e = &idx->keys[k];
		if(owner_key_core_cmp(e, &probe) != 0)
			break;
		/* Both sides carry a runtime id: entity_same_saved_owner decides by
		 * the ids alone (already probed above), never by position. */
		if(has && e->has_rid)
			continue;
		uint8_t g[OWNER_POS_KEY_BYTES];
		if(!owner_pos_key(e->node, g) || memcmp(g, key, sizeof(g)))
			continue; /* hash collision: verified, not counted */
		owner_match_add(m, e->gen_index, e->node);
		if(m->count >= 2)
			break;
	}
}

/* ---- IMP-ENTITY-PRESERVE: keep unchanged pure-Beta rows byte-for-byte ----
 *
 * The imported SET path below replaces every owned Beta row with the codec's
 * canonical record. That is correct but not lossless: the codec holds Pos,
 * Motion and PushX/PushZ as float, so an untouched imported entity would have
 * Beta's exact doubles rounded on its first save. Here an old pure-Beta row
 * is KEPT verbatim when some generated record is the codec's own encoding of
 * that row's decode: build(decode(old)) == build(current[i]) byte-for-byte,
 * with the runtime id zeroed on both sides (an imported row has none). The
 * owned certificate (entity_beta_import_owned) already proves decode(old)
 * captures every tag of old up to that admitted float precision, so reloading
 * the kept row yields exactly current[i]'s persisted state -- with Beta's
 * doubles intact. A current entity that another current record names as a
 * relation (arrow/egg owner, cart occupant) is never retained, because the
 * kept row carries no runtime id for that relation to resolve against.
 * Each pairing consumes one generated record, so identical duplicates pair
 * one-to-one. Any allocation failure just skips retention (the plain set
 * rewrite is still correct): no new failure mode. */
struct retain_key {
	uint64_t hash;
	struct buffer bytes;
	size_t gen_index;
	bool taken;
};
static int retain_key_cmp(const void* x, const void* y) {
	const struct retain_key *a = x, *b = y;
	if(a->hash != b->hash)
		return a->hash < b->hash ? -1 : 1;
	if(a->bytes.len != b->bytes.len)
		return a->bytes.len < b->bytes.len ? -1 : 1;
	return a->gen_index < b->gen_index ? -1 : a->gen_index > b->gen_index;
}
static int u32_cmp(const void* x, const void* y) {
	uint32_t a = *(const uint32_t*)x, b = *(const uint32_t*)y;
	return a < b ? -1 : a > b;
}
static bool retain_kind(enum archived_entity_kind kind) {
	return kind == ARCHIVED_ENTITY_ITEM || kind == ARCHIVED_ENTITY_MONSTER
		|| kind == ARCHIVED_ENTITY_MINECART || kind == ARCHIVED_ENTITY_BOAT
		|| kind == ARCHIVED_ENTITY_PAINTING;
}
/* Canonical bytes of one record with its runtime id zeroed. */
static struct buffer retain_encode(const struct archived_entity* e) {
	struct buffer out = {0};
	struct archived_entity copy = *e;
	copy.source_id = 0;
	/* save-enc-1030: this compares a decoded saved row against the runtime
	 * at the float precision both share (an imported Beta double is only
	 * admitted to the runtime as its float image); exact doubles would make
	 * an unchanged Beta row look moved. */
	copy.has_pos_d = copy.has_vel_d = false;
	struct chunk_entities one = {.entities = &copy, .count = 1};
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&one, &arena);
	if(list && !list_empty(&list->entry))
		out = nbt_dump_binary(
			list_entry(list->entry.flink, struct nbt_list, entry)->data);
	chunk_entities_build_free(arena);
	return out;
}
/* keep[j] = true for each old row j (in `rows` order) to keep verbatim;
 * matched[i] = true for each generated record it stands for. */
static void imported_retain_plan(const nbt_node* rows,
	const struct chunk_entities* current, w_coord_t x, w_coord_t z,
	bool* keep, bool* matched) {
	size_t m = current->count;
	if(!m)
		return;
	uint32_t* refs = malloc(3 * m * sizeof(*refs));
	struct retain_key* keys = calloc(m, sizeof(*keys));
	size_t nrefs = 0, nkeys = 0;
	if(!refs || !keys)
		goto done;
	for(size_t i = 0; i < m; i++) {
		const struct archived_entity* e = &current->entities[i];
		if(e->kind == ARCHIVED_ENTITY_ARROW && e->data.arrow.owner_source_id)
			refs[nrefs++] = e->data.arrow.owner_source_id;
		if((e->kind == ARCHIVED_ENTITY_EGG || e->kind == ARCHIVED_ENTITY_SNOWBALL
			|| e->kind == ARCHIVED_ENTITY_FIREBALL)
		   && e->data.egg.owner_source_id)
			refs[nrefs++] = e->data.egg.owner_source_id;
		if(e->kind == ARCHIVED_ENTITY_MINECART
		   && e->data.minecart.occupant_source_id)
			refs[nrefs++] = e->data.minecart.occupant_source_id;
	}
	qsort(refs, nrefs, sizeof(*refs), u32_cmp);
	for(size_t i = 0; i < m; i++) {
		const struct archived_entity* e = &current->entities[i];
		if(!retain_kind(e->kind) || matched[i]
		   || (e->source_id && nrefs
			   && bsearch(&e->source_id, refs, nrefs, sizeof(*refs), u32_cmp)))
			continue;
		struct buffer b = retain_encode(e);
		if(!b.data)
			continue;
		keys[nkeys++] = (struct retain_key) {
			.hash = owner_fnv1a(b.data, b.len), .bytes = b, .gen_index = i,
		};
	}
	qsort(keys, nkeys, sizeof(*keys), retain_key_cmp);
	size_t j = 0;
	struct list_head* p;
	list_for_each(p, &rows->payload.tag_list->entry) {
		const nbt_node* old = list_entry(p, struct nbt_list, entry)->data;
		struct archived_entity decoded;
		size_t row = j++;
		if(!parse_entity(old, x, z, &decoded) || !retain_kind(decoded.kind)
		   || !entity_beta_import_owned(old, x, z, &decoded))
			continue;
		struct buffer b = retain_encode(&decoded);
		if(!b.data)
			continue;
		uint64_t h = owner_fnv1a(b.data, b.len);
		size_t lo = 0, hi = nkeys;
		while(lo < hi) { /* first key with (hash, len) >= (h, b.len) */
			size_t mid = lo + (hi - lo) / 2;
			if(keys[mid].hash < h
			   || (keys[mid].hash == h && keys[mid].bytes.len < b.len))
				lo = mid + 1;
			else
				hi = mid;
		}
		for(size_t k = lo; k < nkeys && keys[k].hash == h
			&& keys[k].bytes.len == b.len; k++) {
			if(keys[k].taken || memcmp(keys[k].bytes.data, b.data, b.len))
				continue;
			keys[k].taken = true;
			keep[row] = true;
			matched[keys[k].gen_index] = true;
			break;
		}
		buffer_free(&b);
	}
done:
	for(size_t k = 0; keys && k < nkeys; k++)
		buffer_free(&keys[k].bytes);
	free(keys);
	free(refs);
}

static nbt_node* chunk_entities_preserve_impl(const nbt_node* original,
	const nbt_node* generated,const struct chunk_entities* current,
	bool native, w_coord_t x, w_coord_t z, bool reference) {
	/* Review fix (7): the sub-code describes THIS call (CEP_NONE on success),
	 * and the no-original clone reports its own OOM. */
	preserve_note_fail(CEP_NONE);
	if(!original) {
		nbt_node* fresh = nbt_clone((nbt_node*)generated);
		if(!fresh) preserve_note_fail(CEP_CLONE);
		return fresh;
	}
	if(original->type!=TAG_LIST || generated->type!=TAG_LIST) {
		preserve_note_fail(CEP_LIST_TYPE); return NULL;
	}
	if(native && current) {
		/* Native runtime IDs can be remapped at reload, and journal drops can
			 * share a position. Regenerate only after byte-proving that every old
			 * record is fully described by the codec. Unknown fields fail this
			 * proof and take the preserving merge below. The enclosing chunk
			 * still performs its whole-record no-op comparison. */
		bool complete=true;
		struct list_head* p;
		list_for_each(p,&original->payload.tag_list->entry) {
			const nbt_node* old=list_entry(p,struct nbt_list,entry)->data;
			struct archived_entity decoded;
			if(!parse_entity(old,x,z,&decoded) || !entity_native_deletion_safe(old,&decoded)) {
				complete=false;break;
			}
		}
		/* save-enc-1030: an empty list that stays empty keeps its stored bytes
		 * verbatim (an unchanged chunk is never re-encoded); only a list that
		 * changes takes the Beta empty-list type 1. */
		if(complete) return nbt_clone((nbt_node*)(list_empty(&original->payload.tag_list->entry)
			&& list_empty(&generated->payload.tag_list->entry) ? original : generated));
	}
	/* Destructive import reconciliation is all-or-nothing for runtime-admitted
	 * originals. Mixing owned and unowned rows would let an unowned Pos match
	 * consume an owned row's generated entity. Unsupported parse-rejected rows
	 * may coexist and remain opaque; parse-admitted supported recovery IDs
	 * without the exact paired legacy provenance markers fail closed below. */
	bool imported_set = !native && current;
	if(imported_set) {
		struct list_head* p;
		list_for_each(p, &original->payload.tag_list->entry) {
			const nbt_node* old = list_entry(p, struct nbt_list, entry)->data;
			struct archived_entity decoded;
			bool parsed = parse_entity(old, x, z, &decoded);
			if(parsed && entity_beta_import_owned(old, x, z, &decoded))
				continue;
			/* Imported chunks retain their foreign chunk-level marker after the
			 * first save, but their admitted rows are now canonical CaveWii NBT.
			 * Admit those rows to the same set only after exact byte proof; this
			 * gives cold saves convergence without discarding legacy extensions. */
			if(parsed && entity_has_legacy_provenance(old, &decoded)
			   && entity_native_deletion_safe(old, &decoded))
				continue;
			if(parsed && entity_beta_import_cohort(old, &decoded)) {
				preserve_note_fail(CEP_IMPORT_COHORT); return NULL;
			}
			if(parsed) {
				imported_set = false;
				break;
			}
		}
	}
	nbt_node* out=nbt_clone((nbt_node*)original);
	if(!out) { preserve_note_fail(CEP_CLONE); return NULL; }
	bool* matched = NULL;
	bool* keep = NULL; /* IMP-ENTITY-PRESERVE: imported rows kept verbatim */
	/* H2: the owner index (one scratch block, freed on every exit below). A
	 * failed scratch malloc degrades to the linear reference scan -- same
	 * output, no new failure mode. */
	struct owner_index idx = {0};
	if(reference || !owner_index_build(&idx, generated)) {
		owner_index_free(&idx);
		if(!reference) /* review fix (4): the fallback is counted, not silent */
			__atomic_fetch_add(&s_preserve_index_fallback_n, 1u, __ATOMIC_RELAXED);
		idx.reference = true;
	}
	if(current) {
		size_t generated_count = 0;
		struct list_head* count_pos;
		list_for_each(count_pos, &generated->payload.tag_list->entry)
			generated_count++;
		if(generated_count != current->count)
			PRESERVE_FAIL(CEP_COUNT_MISMATCH);
		if(current->count) {
			matched = calloc(current->count, sizeof(*matched));
			if(!matched)
				PRESERVE_FAIL(CEP_ALLOC);
		}
	}
	if(imported_set && matched) {
		size_t rows = 0;
		struct list_head* r;
		list_for_each(r, &out->payload.tag_list->entry)
			rows++;
		keep = rows ? calloc(rows, sizeof(*keep)) : NULL;
		if(keep) /* allocation failure only skips retention */
			imported_retain_plan(out, current, x, z, keep, matched);
	}
	size_t row_index = 0;
	struct list_head *p,*next;
	list_for_each_safe(p,next,&out->payload.tag_list->entry) {
		struct nbt_list* owner=list_entry(p,struct nbt_list,entry);
		nbt_node* old=owner->data;
		size_t row = row_index++;
		struct archived_entity decoded;
		bool parsed = parse_entity(old, x, z, &decoded);
		if(imported_set && parsed && keep && keep[row])
			continue; /* unchanged pure-Beta row: exact bytes, exact doubles */
		if(imported_set && parsed) {
			/* The complete current snapshot owns this exact Beta record as part
			 * of a SET. Position is not identity: movement, pickup/death and two
			 * identical-position items all remove the old rows here. */
			list_del(p); nbt_free(old); free(owner); continue;
		}
		/* Unknown/unparseable records were never admitted to runtime. Preserve
		 * their bytes and never let a coincident position claim them. */
		if(!parsed)
			continue;
		/* H2: indexed owner search (see owner_search). The pre-H2 scan failed
		 * DUP_OWNER at the second owner, or at the FIRST one when there is no
		 * current snapshot; generated count == current->count was checked
		 * above, so a found index always addresses current->entities[]. */
		struct owner_match m;
		owner_search(&idx, old, generated, &m);
		if(m.count && (!current || m.count > 1))
			PRESERVE_FAIL(CEP_DUP_OWNER);
		const nbt_node* candidate = m.count ? m.node : NULL;
		const struct archived_entity* runtime
			= m.count ? &current->entities[m.index] : NULL;
		size_t candidate_index = m.index;
		struct list_head* q;
		if(!candidate) {
			if(native && current) {
				if(!entity_native_deletion_safe(old,&decoded))
					PRESERVE_FAIL(CEP_NATIVE_UNSAFE);
				/* An explicit native snapshot authoritatively removes known
				 * departed/dead owners. Unknown owners remain byte-preserved. */
				list_del(p); nbt_free(old); free(owner); continue;
			}
			/* A parse-admitted imported record with unaccounted fields may have
			 * moved. Keeping it and appending the current record would duplicate
			 * one owner; deleting it could lose unknown state. Fail closed. */
			PRESERVE_FAIL(CEP_IMPORTED_UNMATCHED);
		}
		if(matched[candidate_index]) PRESERVE_FAIL(CEP_DUP_OWNER);
		matched[candidate_index]=true;
		/* java-load-1023 (review r1): a legacy native CaveWii monster gets its
		 * Beta-named sheep/slime tags (Sheared/Color/Size) in the SAME rewrite
		 * that turns its legacy int timers into shorts, inserted at the codec's
		 * own position (after the candidate's preceding sibling). The record
		 * thereby becomes exactly the current encoding; skipping them would
		 * leave a mixed shape that neither entity_native_deletion_safe proof
		 * admits, so a later death/departure would fail the save closed
		 * (CEP_NATIVE_UNSAFE). Foreign (imported) records keep their schema. */
		bool inject_beta = parsed && runtime->kind == ARCHIVED_ENTITY_MONSTER
			&& entity_has_legacy_provenance(old, &decoded);
		const char* prev_name = NULL;
		list_for_each(q,&candidate->payload.tag_compound->entry) {
			nbt_node* n=list_entry(q,struct nbt_list,entry)->data;
			const char* this_prev = prev_name;
			prev_name = n->name;
			const nbt_node* saved=find_child(old,n->name);
			if(!saved && inject_beta && n->name
			   && (!strcmp(n->name, "Sheared") || !strcmp(n->name, "Color")
				   || !strcmp(n->name, "Size"))) {
				struct list_head* after = &old->payload.tag_compound->entry;
				struct list_head* r;
				list_for_each(r, &old->payload.tag_compound->entry) {
					nbt_node* o = list_entry(r, struct nbt_list, entry)->data;
					if(this_prev && o && o->name && !strcmp(o->name, this_prev)) {
						after = r;
						break;
					}
				}
				struct nbt_list* w = malloc(sizeof(*w));
				if(!w) PRESERVE_FAIL(CEP_ALLOC);
				w->data = nbt_clone(n);
				if(!w->data) { free(w); PRESERVE_FAIL(CEP_ALLOC); }
				if(after == &old->payload.tag_compound->entry)
					list_add_tail(&w->entry, after);
				else
					list_add_head(&w->entry, after);
				continue;
			}
			/* Imported entities keep their existing schema; CaveWii-only
			 * bookkeeping is not injected during a lossless rewrite. */
			if(!saved) continue;
			nbt_node* replacement=NULL;
			if(!strcmp(n->name,"Items") && runtime->kind==ARCHIVED_ENTITY_MINECART)
				replacement=item_nbt_write_list(saved,runtime->data.minecart.inventory,27,false);
			else if(!strcmp(n->name,"Item") && runtime->kind==ARCHIVED_ENTITY_ITEM) {
				struct item_data before;
				if(!parse_item(saved,&before)) PRESERVE_FAIL(CEP_ITEM_PARSE);
				if(item_data_pinned(&runtime->data.item.item)) {
					if(before.id!=runtime->data.item.item.id || before.count!=runtime->data.item.item.count
					   || before.durability!=runtime->data.item.item.durability)
						PRESERVE_FAIL(CEP_PINNED_MISMATCH);
					replacement=item_nbt_clone_lossless(saved);
				} else {
					replacement=calloc(1,sizeof(*replacement));
					if(replacement && !item_nbt_emit(replacement,&runtime->data.item.item,-1)) {
						free(replacement); replacement=NULL;
					}
				}
			} else if(saved_double_is_image(saved,n)) {
				continue; /* IMP-ENTITY-PRESERVE: keep Beta's exact double */
			} else replacement=nbt_clone(n);
			/* save-enc-1030: an empty list of OUR record is written the Beta
			 * way, element type 1 (NBTTagList.java:25-31); item_nbt_write_list
			 * keeps the saved sentinel type, which pre-save-enc-1030 records
			 * had as TAG_COMPOUND. Imported records keep their own bytes. */
			if(replacement && replacement->type == TAG_LIST
			   && replacement->payload.tag_list
			   && replacement->payload.tag_list->data
			   && list_empty(&replacement->payload.tag_list->entry)
			   && entity_has_legacy_provenance(old, &decoded))
				replacement->payload.tag_list->data->type = TAG_BYTE;
			if(!item_nbt_replace(old,n->name,replacement)) PRESERVE_FAIL(CEP_REPLACE);
		}
		/* save-enc-1030: a pre-save-enc-1030 native monster record carries
		 * the old fixed footprint (Angry/Sitting/Owner/Saddle/Anger/powered on
		 * every monster). The loop above only rewrites tags the candidate
		 * has, so drop each of those six the current encoding no longer
		 * writes for this monster -- otherwise a stale "powered"/"Saddle"
		 * would survive and a non-wolf would keep wolf tags forever. Only
		 * OUR records (paired legacy provenance) are touched; imported ones
		 * keep their schema. */
		if(parsed && runtime->kind == ARCHIVED_ENTITY_MONSTER
		   && entity_has_legacy_provenance(old, &decoded)) {
			static const char* const footprint[] = {
				"Angry", "Sitting", "Owner", "Saddle", "Anger", "powered",
			};
			for(size_t k = 0; k < sizeof(footprint) / sizeof(*footprint); k++) {
				if(find_child(candidate, footprint[k]))
					continue;
				struct list_head *r, *rn;
				list_for_each_safe(r, rn, &old->payload.tag_compound->entry) {
					struct nbt_list* w = list_entry(r, struct nbt_list, entry);
					if(w->data && w->data->name
					   && !strcmp(w->data->name, footprint[k])) {
						list_del(r); nbt_free(w->data); free(w);
					}
				}
			}
		}
	}
	size_t generated_index=0;
	list_for_each(p,&generated->payload.tag_list->entry) {
		nbt_node* n=list_entry(p,struct nbt_list,entry)->data;
		bool found=current ? matched[generated_index] : false;
		if(!found) {
			struct nbt_list* w=malloc(sizeof(*w));
			if(!w) PRESERVE_FAIL(CEP_ALLOC);
			w->data=nbt_clone(n);
			if(!w->data) { free(w); PRESERVE_FAIL(CEP_ALLOC); }
			list_add_tail(&w->entry,&out->payload.tag_list->entry);
		}
		generated_index++;
	}
	/* save-enc-1030: an EMPTY Entities list is written with element type 1,
	 * as Beta's NBTTagList.writeTagContents does for every empty list
	 * [McBetaCpp/srcorigjava NBTTagList.java:25-31] -- including the list a
	 * chunk is left with after every entity departed from a typed list. */
	if(!list_empty(&out->payload.tag_list->entry)) out->payload.tag_list->data->type=TAG_COMPOUND;
	else if(out->payload.tag_list->data
		&& !list_empty(&original->payload.tag_list->entry)) out->payload.tag_list->data->type=TAG_BYTE;
	free(matched);
	free(keep);
	owner_index_free(&idx);
	return out;
fail:
	free(matched); free(keep); owner_index_free(&idx); nbt_free(out); return NULL;
}

nbt_node* chunk_entities_preserve(const nbt_node* original,
	const nbt_node* generated, const struct chunk_entities* current,
	bool native, w_coord_t x, w_coord_t z) {
	return chunk_entities_preserve_impl(original, generated, current, native,
										x, z, false);
}
nbt_node* chunk_entities_preserve_reference(const nbt_node* original,
	const nbt_node* generated, const struct chunk_entities* current,
	bool native, w_coord_t x, w_coord_t z) {
	return chunk_entities_preserve_impl(original, generated, current, native,
										x, z, true);
}
void chunk_entities_preserve_test_reset_fail(void) {
	preserve_note_fail(CEP_NONE);
}
