/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef ENTITY_ARCHIVE_H
#define ENTITY_ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../cNBT/nbt.h"
#include "../item/item_types.h"
#include "../world.h"

/*
 * Hostile-input parse ceiling, deliberately separate from the runtime writer.
 * cNBT rejects decompressed payloads above 8 MiB and every supported entity
 * compound occupies well over 128 encoded bytes, so no valid CaveWii chunk can
 * approach 65,536 records. The separately named writer ceiling is a file-format
 * safety boundary, not the deleted 608 runtime cap: a complete set through that
 * ceiling is encoded, while a larger/self-unreadable set fails transactionally
 * so the owning world remains resident rather than being truncated.
 */
#define CHUNK_ENTITY_ARCHIVE_READ_MAX 65536u
#define CHUNK_ENTITY_ARCHIVE_WRITE_MAX 65536u
#define LEGACY_BLOCK_TNT_ARCHIVE_MAX 96

enum archived_entity_kind {
	ARCHIVED_ENTITY_ITEM = 1,
	ARCHIVED_ENTITY_MONSTER = 2,
	ARCHIVED_ENTITY_MINECART = 3,
	ARCHIVED_ENTITY_ARROW = 4,
	ARCHIVED_ENTITY_PRIMED_TNT = 5,
	/* M4 S5 thrown egg (appended; Beta also persists EntityEgg). The arrow's
	 * shape minus the arrow-specific fields (no stick/pickup/damage/shooter
	 * class): just the owner relation + in-air age. */
	ARCHIVED_ENTITY_EGG = 6,
	/* Batch 1a thrown snowball (appended; Beta persists EntitySnowball as
	 * "Snowball"). Byte-identical record shape to the egg -- shares the
	 * archived egg state struct. */
	ARCHIVED_ENTITY_SNOWBALL = 7,
	/* Batch 1b ghast fireball (appended; Beta persists "Fireball"). The egg
	 * record shape again (owner + age); the ACCELERATION is deliberately not
	 * persisted -- Beta's own writeEntityToNBT drops it too, so a reloaded
	 * mid-flight fireball is the same drifting dud both there and here. */
	ARCHIVED_ENTITY_FIREBALL = 8,
	/* Boat-slice (appended). Beta's OWN writeEntityToNBT/readEntityFromNBT
	 * are BOTH EMPTY [740c583: EntityBoat] -- damage/shake/rock-direction
	 * are NOT Beta-persisted (a reload resets those to defaults, exactly
	 * like the minecart's own damage_taken/shake_ticks/shake_dir). Unlike
	 * the minecart, occupancy is NOT persisted either: the restore
	 * post-pass (server_local.c) unconditionally drops a restored
	 * "occupied by the player" relationship (the player's own position
	 * comes back at their independently-saved spot, not physically inside
	 * the vehicle -- "Restoring the PLAYER as a rider is ... actively
	 * broken", per that pass's own comment on the minecart). A boat's ONLY
	 * possible occupant IS the player (Beta 1.7.3 has no mob-boat-riding
	 * mechanic, unlike the minecart's separate legacy mob-rider allowance),
	 * so that drop applies to EVERY restored boat occupancy -- persisting
	 * the flag would be dead weight that always reads back false. A boat
	 * therefore has NO kind-specific archive tags at all. */
	ARCHIVED_ENTITY_BOAT = 9,
	/* Painting-slice (gap audit #9; appended, Beta persists "Painting" --
	 * coincidentally also Beta's OWN EntityList numeric id 9 for
	 * EntityPainting [740c583: EntityList.java `addMapping(EntityPainting.
	 * class, "Painting", 9)`], unrelated to this enum's own independent
	 * CaveWiiType numbering -- ARCHIVED_ENTITY_BOAT already claimed 9 here
	 * on the same day this slice's own worktree was created, so the correct
	 * append-only value landing onto tip is 10, not the slice's own
	 * original 9). Beta's writeEntityToNBT does NOT call
	 * super.writeEntityToNBT -- only Dir/Motive/TileX/TileY/TileZ are Beta's
	 * own tags; the shared Pos/Motion/etc. common block is still written (see
	 * struct archived_painting_state) purely so this framework's
	 * unconditional Pos read on load has something to restore. */
	ARCHIVED_ENTITY_PAINTING = 10,
	/* Batch I (appended; Beta persists "FallingSand" -- also coincidentally
	 * Beta's own EntityList numeric id 21 for EntityFallingSand [740c583:
	 * EntityList.java `addMapping(EntityFallingSand.class, "FallingSand",
	 * 21)`], unrelated to this enum's own independent CaveWiiType numbering,
	 * same convention as ARCHIVED_ENTITY_PAINTING's own comment). Beta's
	 * writeEntityToNBT/readEntityFromNBT [740c583: EntityFallingSand] write
	 * exactly ONE kind-specific tag: `setByte("Tile", (byte)this.blockID)`. */
	ARCHIVED_ENTITY_FALLING_SAND = 11,
};

struct archived_item_state {
	struct item_data item;
	int32_t age;
	uint32_t death_transaction;
	uint16_t death_sequence;
};

struct archived_monster_state {
	int32_t monster_id;
	bool wolf_angry;
	bool wolf_sitting;
	char wolf_owner[32];
	uint8_t fleece_color;
	bool sheared;      /* sheep; Beta "Sheared" -> CaveWiiSheared, absent = false */
	bool saddled;      /* pig; Beta's OWN tag "Saddle", absent = false (dataWatcher default) */
	int32_t egg_timer; /* chicken; CaveWiiEggTimer, absent = 0 -> re-roll on load */
	int32_t anger;     /* pig zombie; Beta's own "Anger" short, absent = 0 (calm) */
	bool charged;      /* creeper; Beta's own "powered" byte, absent = false */
	int32_t ai_state;
	float ai_timer;
	float direction[2];
	int32_t direction_time;
	float body_yaw;
	float head_yaw;
	int32_t fuse;
	int32_t attack_time;
	int32_t attack_windup;
	int32_t fire_time;
	int32_t hurt_time;
	int32_t knockback_time;
	int32_t aggro_time;
	float limb_swing;
	int32_t air;
	int32_t entity_age;
	int32_t death_time;
	bool death_loot_dropped;
	bool hit_by_skeleton_arrow;
	int32_t jump_delay;
	float swim_vec[3];
	float swim_speed;
	float squid_phase;
	float squid_phase_vel;
};

struct archived_minecart_state {
	float speed;
	int8_t hx, hz;
	uint8_t last_meta;
	bool occupied;
	uint32_t occupant_source_id;
	/* Beta minecartType [740c583: EntityMinecart.writeEntityToNBT "Type"].
	 * 0 rideable / 1 chest / 2 furnace. */
	uint8_t minecart_type;
	/* CaveWii-only tag "CaveWiiBlastDamage": the explosion entity pass can
	 * push minecartCurrentDamage past 40 while the entity dict is being
	 * iterated, and the break then runs on the cart's next tick. Beta has
	 * no such window (the cart dies inside attackEntityFrom) and persists
	 * no damage; a save landing in that one-tick window must not revive
	 * the cart, so the counter rides along. Absent -> 0. */
	int16_t blast_damage;
	/* Beta pushX/pushZ, furnace-cart (type 2) only [740c583:
	 * EntityMinecart.writeEntityToNBT "PushX"/"PushZ", TAG_DOUBLE]. Written/read
	 * for every minecart kind to keep chunk_entities_build's per-kind footprint
	 * fixed (the monster archive's own "written for every X, 0 for non-Y"
	 * precedent) -- harmless for type 0/1, which never populate it. */
	float push_x, push_z;
	/* Beta fuel, furnace-cart (type 2) only [740c583:
	 * EntityMinecart.writeEntityToNBT "Fuel", TAG_SHORT]. Same
	 * written-for-every-kind convention as push_x/push_z above. */
	int32_t fuel;
	/* Beta cargoItems, chest-cart (type 1) only [740c583:
	 * EntityMinecart.writeEntityToNBT "Items"]. getSizeInventory() is 27 for
	 * EVERY minecart kind in Beta (EntityMinecart.getSizeInventory,
	 * TileEntityChest.getSizeInventory agree) -- a plain literal here, not
	 * entity.h's MINECART_CHEST_SLOTS, matching this header's existing
	 * hand-mirrored-shape convention (no entity headers included here).
	 * Written/read for
	 * every minecart kind (all-empty for type 0/2), same fixed-footprint
	 * reasoning as push_x/push_z. */
	struct item_data inventory[27];
};

struct archived_arrow_state {
	uint32_t owner_source_id;
	int32_t age;
	uint16_t item_id;
	int16_t damage;
	bool from_player;
	bool from_skeleton;
	bool in_ground;
	int32_t stuck_ticks;
	int32_t shake_ticks;
	int16_t stuck_block_x, stuck_block_y, stuck_block_z;
	uint8_t stuck_block_type, stuck_block_meta;
};

struct archived_egg_state {
	uint32_t owner_source_id; /* thrower relation; remapped like the arrow's */
	int32_t age;              /* Beta ticksinAir (owner grace; no air cap) */
};

struct archived_primed_tnt_state {
	/* x/y/z are populated only for migration of old CaveWiiBlock* records. */
	int32_t x, y, z;
	uint16_t fuse;
	bool explosion_pending;
	bool legacy_block_backed;
};

/* Beta's OWN tag [740c583: EntityFallingSand.writeEntityToNBT]: `setByte(
 * "Tile", (byte)this.blockID)`. The single carried block id (BLOCK_SAND or
 * BLOCK_GRAVEL). */
struct archived_falling_sand_state {
	uint8_t block_id;
};

struct archived_painting_state {
	/* Beta's OWN tag names/types [740c583: EntityPainting.writeEntityToNBT]:
	 * TileX/TileY/TileZ (ints), Dir (a byte). */
	int32_t tile_x, tile_y, tile_z;
	int32_t direction;
	/* Beta's OWN tag name: Motive (a string, EnumArt.title). Longest title is
	 * "SkullAndRoses" (13 chars); +1 NUL fits in 16 with headroom (matches
	 * entity_painting.h's PAINTING_ART_TITLE_MAX, kept as a literal here to
	 * preserve this header's existing independence from the live entity
	 * headers -- see the archived_minecart_state precedent). */
	char art_name[16];
};

struct archived_entity {
	enum archived_entity_kind kind;
	uint32_t source_id; /* relation key only; never reused as a runtime id */
	/* NBT Pos, including Beta's saved Y adjustment: logical posY + ySize. */
	vec3 pos;
	vec3 vel;
	/* save-enc-1030: Beta saves Pos/Motion as doubles [Entity.writeToNBT
	 * newDoubleNBTList]. When the source holds real doubles (a live mob's
	 * authoritative mob_motion state, or a parsed record whose Pos/Motion
	 * are TAG_DOUBLE) they ride here and are written exactly; pos/vel stay
	 * their float images ((float)pos_d). Without them the float is widened. */
	double pos_d[3];
	double vel_d[3];
	bool has_pos_d;
	bool has_vel_d;
	vec2 orient;
	bool on_ground;
	/* FH-18 (docs/fork_hack_audit_2026-08-21.md:192): Beta Entity.fire is a
	 * signed tick countdown, NOT a bool [740c583 Entity.java:53 `public int
	 * fire;`; onEntityUpdate: 300 on direct fire contact / lightning, 600 on
	 * lava (setOnFireFromLava), decrements 1/tick while >0 (dealing 1 damage
	 * every 20 ticks: `fire % 20 == 0`), and goes NEGATIVE as a
	 * fire-immunity cooldown (`fire = -fireResistance` on water/rain
	 * contact or once a burn naturally expires) -- writeToNBT/readFromNBT
	 * round-trip it as a literal `(short)this.fire`
	 * [740c583 Entity.java:796,827], hence int16_t here, matching TAG_SHORT
	 * exactly. This was previously collapsed to a bool (on_fire ? 1 : 0),
	 * losing both the countdown length AND the negative-cooldown state.
	 * `fire > 0` is Beta's own `isBurning()`/`getEntityFlag(0)` condition
	 * for "currently on fire". */
	int16_t fire;
	/* Beta Entity.writeToNBT common Air tag. Item imports retain it even though
	 * CaveWii does not yet simulate item drowning. Other nonliving kinds use
	 * the Beta constructor default; monsters carry their live counter. */
	int16_t air;
	int16_t health;
	/* Beta Entity.writeToNBT common tag, inherited by every entity kind. */
	float fall_distance;

	union {
		struct archived_item_state item;
		struct archived_monster_state monster;
		struct archived_minecart_state minecart;
		struct archived_arrow_state arrow;
		struct archived_egg_state egg;
		struct archived_primed_tnt_state primed_tnt;
		/* The boat's ONLY kind-specific tag is the CaveWii blast-damage
		 * latch (same rationale as archived_minecart_state.blast_damage);
		 * everything else Beta leaves unpersisted stays unpersisted. */
		struct archived_boat_state {
			int16_t blast_damage;
		} boat;
		struct archived_painting_state painting;
		struct archived_falling_sand_state falling_sand;
	} data;
};

struct chunk_entities {
	struct archived_entity* entities;
	size_t count;
	/* save-enc-1030: World.getWorldTime() at the moment this snapshot was
	 * gathered for a save. region_archive_set_chunk stamps it into
	 * Level.LastUpdate, as Beta ChunkLoader.storeChunkInCompound does
	 * [McBetaCpp/srcorigjava ChunkLoader.java:140 setLong("LastUpdate",
	 * world.getWorldTime())]. Read only when has_world_time is set: a
	 * zero-initialized snapshot (tests, tools, recovery writers, a gatherer
	 * that does not know the time) keeps the pre-save-enc-1030 behaviour --
	 * a stored record keeps its LastUpdate and a new one writes 0 -- rather
	 * than resetting a real stamp to 0. Not used by the entity codec. */
	int64_t world_time;
	bool has_world_time;
};

/* Build/free a dump-only cNBT TAG_LIST. A non-empty input returning NULL is an
 * allocation failure and must abort the containing chunk save. */
struct nbt_list* chunk_entities_build(const struct chunk_entities* entities,
									 void** arena_out);
void chunk_entities_build_free(void* arena);
nbt_node* chunk_entities_preserve(const nbt_node* original,
	const nbt_node* generated, const struct chunk_entities* current,
	bool native, w_coord_t x, w_coord_t z);

/* lava-aabb-0924/H1: WHICH fail-closed exit chunk_entities_preserve took the
 * last time it returned NULL. Latched (never cleared by a later success), read
 * by the region save-failure publication (region_archive.h) when the region
 * stage is "entity_preserve", and from there by the heartbeat
 * ("save_fail_sub"). The wire carries the ORDINAL: append, never reorder. */
enum chunk_entities_preserve_fail {
	CEP_NONE = 0,
	CEP_LIST_TYPE,          /* original/generated not TAG_LIST */
	CEP_IMPORT_COHORT,      /* imported cohort row without paired provenance */
	CEP_CLONE,              /* nbt_clone(original) failed (OOM) */
	CEP_COUNT_MISMATCH,     /* generated count != current->count */
	CEP_ALLOC,              /* matched[] / append node allocation failed */
	CEP_DUP_OWNER,          /* two generated rows (or current==NULL) for one owner */
	CEP_NATIVE_UNSAFE,      /* departed native record not byte-provably deletable */
	CEP_IMPORTED_UNMATCHED, /* parse-admitted imported record with no current owner */
	CEP_ITEM_PARSE,         /* saved Item compound unparseable */
	CEP_PINNED_MISMATCH,    /* pinned item id/count/durability drifted */
	CEP_REPLACE,            /* item_nbt_replace failed */
	CEP_MAX
};
enum chunk_entities_preserve_fail chunk_entities_preserve_last_fail(void);

/* lava-aabb-0924/H2: the SAME merge with the pre-H2 linear owner scan in place
 * of the O(n log n) owner index -- the equivalence oracle for
 * tests/test_entity_preserve_equiv.c. Output bytes and fail sub-codes are
 * identical by construction (one shared body, one swapped search); the test
 * proves it on randomized and adversarial input. Not for production use:
 * O(n_old * n_generated). */
nbt_node* chunk_entities_preserve_reference(const nbt_node* original,
	const nbt_node* generated, const struct chunk_entities* current,
	bool native, w_coord_t x, w_coord_t z);
/* Test hook: clear the fail sub-code (CEP_NONE). Since review fix (7) every
 * merge call resets it itself; kept so the equivalence test can pin that. */
void chunk_entities_preserve_test_reset_fail(void);
/* Review fix (4): merges whose owner-index scratch malloc failed and which
 * fell back to the linear scan (heartbeat "save_idx_fb"). */
uint32_t chunk_entities_preserve_index_fallbacks(void);
#ifdef CAVEWII_PRESERVE_TEST
/* Review fix (3), test builds only: hash every POS key to one constant. */
void chunk_entities_preserve_test_constant_hash(bool on);
#endif

/* Raw-list predicate for persistence bookkeeping. Unlike parse count, this is
 * true for unknown/malformed elements that the optional entity codec skips. */
bool chunk_entities_nbt_list_nonempty(const nbt_node* entities_list);

/* Parse valid supported compounds and skip malformed/unknown entries. Records
 * whose position does not belong to (chunk_x, chunk_z) are rejected. */
bool chunk_entities_parse(const nbt_node* entities_list, w_coord_t chunk_x,
							  w_coord_t chunk_z, struct chunk_entities* out);
void chunk_entities_free(struct chunk_entities* entities);

#endif
