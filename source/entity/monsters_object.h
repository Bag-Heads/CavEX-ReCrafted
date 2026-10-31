/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MONSTERS_OBJECT_H
#define MONSTERS_OBJECT_H

#include <stdbool.h>
#include <stdint.h>

// Canonical hostile-mob identifiers. This is the single source of truth for
// monster types; entity_monster.h includes this header. The values are wire
// identifiers (sent in CRPC_SPAWN_MONSTER) so existing streams keep ZOMBIE=0
// and CREEPER=1; new types are appended.
enum {
	MONSTER_ZOMBIE = 0,
	MONSTER_CREEPER = 1,
	MONSTER_SKELETON = 2,
	MONSTER_SPIDER = 3,
	// Passive animals (Beta 1.7.3 EntityAnimal), appended after the hostiles so the
	// existing wire ids stay stable. They share the ENTITY_MONSTER tick but are flagged
	// `passive` in monster_def (no targeting/attack/sun-burn; separate spawn pass + cap).
	MONSTER_PIG = 4,
	MONSTER_COW = 5,
	MONSTER_CHICKEN = 6,
	MONSTER_SHEEP = 7,
	// M2 final units, appended per the wire rule. The SQUID is Beta's sole water
	// creature (EntityWaterMob: peaceful category, its OWN water spawn pass + cap).
	// The SLIMES are hostile but extend EntityLiving, NOT EntityMob -- no light
	// spawn gate, no peaceful auto-kill. Beta has ONE slime entity with a size
	// field (1/2/4 from 1 << nextInt(3)); v2 encodes SIZE AS THE ID so the const
	// def table + the stateless per-id bbox thunks drive box/health/drops with
	// zero per-entity size state and no CRPC_SPAWN_MONSTER payload change.
	MONSTER_SQUID = 8,
	MONSTER_SLIME_SMALL = 9,   // Beta size 1
	MONSTER_SLIME_MEDIUM = 10, // Beta size 2
	MONSTER_SLIME_BIG = 11,    // Beta size 4
	// Nether natives (2026-08-07 caveat batch 1b), appended per the wire rule.
	// Beta BiomeGenHell clears every spawn list and adds ONLY these two, both
	// weight 10 [740c583: BiomeGenHell ctor]. Neither carries a light spawn
	// gate: EntityPigZombie.getCanSpawnHere overrides EntityMob's (difficulty +
	// AABB-clear only) and EntityGhast extends EntityFlying/EntityLiving (never
	// had one). Both are isImmuneToFire.
	MONSTER_PIG_ZOMBIE = 12,
	MONSTER_GHAST = 13,
	// Beta EntityWolf: creature-list member, biome-restricted to forest/taiga.
	// Appended because this enum is carried by CRPC_SPAWN_MONSTER.
	MONSTER_WOLF = 14,
	// Beta EntityGiantZombie ("Giant", EntityList id 53): extends EntityMob
	// DIRECTLY (not EntityZombie) -- zombie texture/model at 6x, health x10,
	// attackStrength 50, moveSpeed 0.5, and it appears in NO spawn list (never
	// spawns naturally in Beta; reachable only via a spawner TE naming it).
	// Appended per the wire rule. [decompile 740c583 EntityGiantZombie,
	// EntityList addMapping(...,"Giant",53)]
	MONSTER_GIANT = 15,
	MONSTER_COUNT
};

// EntityWolf dataWatcher byte 16, kept byte-for-byte in the C entity state.
// [740c583: EntityWolf is/setWolfSitting/Angry/Tamed]
#define WOLF_FLAG_SITTING 0x01u
#define WOLF_FLAG_ANGRY   0x02u
#define WOLF_FLAG_TAMED   0x04u
#define WOLF_OWNER_MAX 32
#define WOLF_LOCAL_PLAYER_NAME "Local Player"
#define WOLF_SHAKE_MAX_SPLASHES 7

static inline bool wolf_flag_get(uint8_t flags, uint8_t bit) {
	return (flags & bit) != 0;
}

static inline void wolf_flag_set(uint8_t* flags, uint8_t bit, bool value) {
	if(value)
		*flags = (uint8_t)(*flags | bit);
	else
		*flags = (uint8_t)(*flags & (uint8_t)~bit);
}

enum wolf_living_sound {
	WOLF_LIVING_BARK,
	WOLF_LIVING_GROWL,
	WOLF_LIVING_WHINE,
	WOLF_LIVING_PANTING,
};

// Pure EntityWolf goldens. The caller supplies nextInt(3), so state selection
// cannot perturb the simulation stream. [740c583: EntityWolf.getLivingSound]
enum wolf_living_sound wolf_living_sound_pick(uint8_t flags,
										 int watched_health, int roll3);
float wolf_tail_angle(uint8_t flags, int watched_health);
float wolf_shading_while_shaking(float previous, float current, float partial);
float wolf_shake_angle(float previous, float current, float partial,
					   float offset);
float wolf_interested_angle(float previous, float current, float partial);
int wolf_adjust_incoming_damage(int damage, bool attacker_is_player,
								 bool attacker_is_arrow);
bool wolf_can_despawn(uint8_t flags);
// (monster_is_passive + monster_is_water_creature live BELOW monster_def_get: they
// are flag-driven from the def table now that hostile ids exist on BOTH sides of
// the passive 4..7 block -- the old `id >= MONSTER_PIG` boundary compare would
// misclassify every slime as passive.)

// === SHEEP FLEECE COLOUR (Beta 1.7.3) — explicit persistent state ===
// New sheep seed explicit fleece_color state from the uint32_t entity id hash.
// That value is then networked and archived so fresh runtime ids after reload
// cannot change the render tint or wool drop. NON-BETA DEVIATION: Beta rolls the world RNG in
// EntitySheep.getRandomFleeceColor; the id hash supplies only the initial value.
// These helpers are int-only (no cglm); RGB lookup remains in render_entity.c.

// Murmur3 fmix32 finalizer: well-distributed, de-clusters consecutive ids (1,2,3,…).
static inline uint32_t sheep_fleece_hash(uint32_t id) {
	uint32_t h = id;
	h ^= h >> 16;
	h *= 0x7feb352dU;
	h ^= h >> 15;
	h *= 0x846ca68bU;
	h ^= h >> 16;
	return h;
}

// Wool metadata value (0..15), used BOTH as the gfx_entity_tint colour index AND the dropped
// BLOCK_WOOL durability (= block metadata). Beta wool-metadata indices == Beta fleece-colour
// indices == v2 TEXAT_WOOL_* ordering, so NO remap. Buckets to the Beta 1.7.3 fleece distribution
// (white 81.84%, black/gray/lightgray 5% each, brown 3%, pink 0.16%) over a 10000-wide range so
// the 0.16% pink resolves. (Tiny modulo bias < 0.01%, visually irrelevant.)
static inline int sheep_fleece_color(uint32_t id) {
	uint32_t r = sheep_fleece_hash(id) % 10000u;
	if (r < 8184) return 0;   // white       81.84%
	if (r < 8684) return 15;  // black        5.00%
	if (r < 9184) return 7;   // gray         5.00%
	if (r < 9684) return 8;   // light gray   5.00%
	if (r < 9984) return 12;  // brown        3.00%
	return 6;                 // pink         0.16%
}

// Per-mob static definition. Beta 1.7.3 faithful (see monsters_object.c for the
// per-field sourcing). Indexed by the MONSTER_* id.
struct monster_def {
	const char* name;
	int health; // Beta max health (hit points; 2 per heart)
	uint16_t drop; // primary drop item id (enum item_type)
	uint8_t drop_count; // items dropped on death (see note in .c re: Phase 5)
	int spawn_weight; // relative weight in the hostile spawn table (0 for animals)
	// Relative weight in the SEPARATE passive-animal spawn list (Beta 1.7.3
	// EnumCreatureType.creature: sheep 12, pig 10, chicken 10, cow 8); 0 for hostiles.
	// Mirrors Beta's two independent spawn lists so the hostile picker and the animal
	// picker never mix categories (a herd of pigs can't crowd out zombies, and vice versa).
	int animal_weight;
	float move_speed; // Beta 1.7.3 EntityCreature moveSpeed (faithful ordering)
	// Geometry (Beta 1.7.3 setSize), data-driven so a new mob/animal id gets its own
	// collision/melee/LOS/brightness box from the table instead of a hardcoded branch.
	// box_w x box_h = the AABB (feet at the entity origin, centered on X/Z); eye_height
	// = the Beta getEyeHeight (height*0.85) used by the LOS + drown head-cell probe.
	// Biped eye height is Beta's inherited 1.8 * .85 = 1.53. eye_height remains
	// explicit because spiders, animals, squid and variable slimes override size.
	float box_w, box_h;
	float eye_height;
	// Passive (EntityAnimal): wander-only, no target/melee/sun-burn; separate spawn
	// pass + cap. fall_immune: skip fall damage (chicken). Hostiles: both false.
	bool passive;
	bool fall_immune;
	// Water creature (Beta EntityWaterMob -> the squid): spawns from its OWN water
	// pass + cap (the THIRD Beta spawn category, EnumCreatureType.waterCreature),
	// is EXCLUDED from the land-animal cap count, and swims instead of walking.
	// false for hostiles and land animals.
	bool water_creature;
	// Relative weight in the NETHER hostile spawn list (Beta BiomeGenHell:
	// ghast 10, pig zombie 10, everything else absent). 0 keeps the mob out of
	// the Nether table entirely; the overworld table reads spawn_weight and is
	// unaffected by this column.
	int nether_weight;
	// Beta isImmuneToFire (pig zombie, ghast): skip fire DoT + lava ignition
	// damage in the shared hazard block.
	bool fire_immune;
	// True for hostiles WITHOUT the EntityMob eff<=7 darkness spawn gate that
	// are not slimes (pig zombie overrides getCanSpawnHere; ghast never had
	// one). The slime keeps its own dedicated gate path.
	bool no_light_gate;
};

// Returns the definition for a monster id. Out-of-range ids clamp to
// MONSTER_ZOMBIE so callers never dereference garbage (crash-safe).
const struct monster_def* monster_def_get(int monster_id);

// True iff the monster id is a passive animal (vs a hostile). Drives the spawn-cap
// split, the spawn picker split, and the no-acquire/no-retaliate AI gates.
// FLAG-DRIVEN from the def table (single source of truth); pure + id-only.
// Out-of-range ids clamp to zombie = non-passive (monster_def_get's fallback).
static inline bool monster_is_passive(int monster_id) {
	return monster_def_get(monster_id)->passive;
}

// True iff the monster id is a water creature (the squid). Splits the third spawn
// category out of the passive set: the water pass counts ONLY these; the land-animal
// pass count EXCLUDES them (a squid must never consume the 8-animal land cap).
static inline bool monster_is_water_creature(int monster_id) {
	return monster_def_get(monster_id)->water_creature;
}

// --- SLIME size-as-id mapping (Beta EntitySlime size field 1/2/4) -------------
// The three ids are CONSECUTIVE (SMALL=9, MEDIUM=10, BIG=11), which makes the
// Beta size roll `1 << nextInt(3)` a plain offset. Pure + pinned by test_slime.
static inline bool monster_is_slime(int monster_id) {
	return monster_id >= MONSTER_SLIME_SMALL && monster_id <= MONSTER_SLIME_BIG;
}
// Beta constructor: size = 1 << rand.nextInt(3) -> 1/2/4, uniform. roll3 in 0..2.
static inline int slime_size_from_roll(int roll3) {
	return MONSTER_SLIME_SMALL + roll3;
}
// The Beta size NUMBER (1/2/4) for a slime id; 0 for non-slimes. Drives the
// split offsets (size/8) and any size-scaled rule.
static inline int slime_beta_size(int monster_id) {
	switch(monster_id) {
	case MONSTER_SLIME_SMALL: return 1;
	case MONSTER_SLIME_MEDIUM: return 2;
	case MONSTER_SLIME_BIG: return 4;
	default: return 0;
	}
}
// Death-split child id (Beta setEntityDead: children are setSlimeSize(size/2)):
// BIG -> MEDIUM -> SMALL; SMALL (and non-slimes) -> -1 (no split).
static inline int slime_child_id(int monster_id) {
	switch(monster_id) {
	case MONSTER_SLIME_BIG: return MONSTER_SLIME_MEDIUM;
	case MONSTER_SLIME_MEDIUM: return MONSTER_SLIME_SMALL;
	default: return -1;
	}
}

// The FULL Beta EntitySlime.setEntityDead split gate (decompile-verified,
// 740c583:EntitySlime.java):
//     with size = getSlimeSize(),
//     in a non-multiplayer world, when size > 1 and health == 0,
//         4 children of size size/2 spawn.
//
// `size > 1` is exactly `slime_child_id(monster_id) >= 0` (a size-1 slime's
// child id is -1); `!multiplayerWorld` needs no mirror here -- the caller
// (mob_die_tick) only ever runs on the authoritative server_local tick, never
// a remote-world display. What this predicate adds is the STRICT `health == 0`
// term: deathTime reaching the flop-reap threshold only proves health <= 0 at
// some EARLIER tick, not health == 0 NOW -- overkill damage (health driven
// well negative by one big hit, per EntityLiving.damageEntity's unclamped
// subtraction of the damage) reaches the same reap edge with health < 0 and must NOT
// split. Parity fix 2026-09-02: the engine previously split on `child_id >= 0`
// alone, so ANY death (including overkill) always split -- denying the player
// the intended "a big enough hit skips the splits" cost.
static inline bool slime_should_split(int child_id, int health) {
	return child_id >= 0 && health == 0;
}

// Sum of all spawn weights; pass to a [0,total) RNG to drive monster_spawn_pick.
int monster_spawn_total_weight(void);

// Maps a roll in [0, monster_spawn_total_weight()) to a MONSTER_* id by walking
// the weight table. Pure + deterministic, so the spawn distribution is unit
// testable headless. Rolls outside the range clamp to the table ends.
int monster_spawn_pick(int roll);

// Dimension-aware variants (batch 1b): dim < 0 (WORLD_DIM_NETHER) walks the
// nether_weight column -- Beta BiomeGenHell's ghast/pig-zombie 10/10 list --
// while dim >= 0 walks spawn_weight, bit-identical to the two functions above
// (the overworld table and its consumed-roll stream are untouched).
int monster_spawn_total_weight_dim(int dim);
int monster_spawn_pick_dim(int dim, int roll);

// The passive-animal analogues of the two functions above (Beta 1.7.3
// EnumCreatureType.creature spawn list). animal_spawn_pick walks animal_weight, so it
// only ever returns passive animals; hostiles (animal_weight 0) are skipped. Used by the
// animal spawn pass (server_local_spawn_animals). Same pure/deterministic contract.
int animal_spawn_total_weight(void);
int animal_spawn_pick(int roll);

// Biome-aware creature-list extension. For every biome except forest/taiga,
// these are exactly the legacy animal functions roll-for-roll. Forest/taiga
// append the Beta weight-2 wolf entry after the unchanged base table.
// [740c583: BiomeGenForest/BiomeGenTaiga constructors]
int animal_spawn_total_weight_biome(int biome);
int animal_spawn_pick_biome(int biome, int roll);
int monster_max_spawned_in_chunk(int monster_id);

#endif
