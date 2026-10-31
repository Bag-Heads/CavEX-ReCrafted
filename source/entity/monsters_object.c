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

#include "monsters_object.h"

#include "../item/items.h"
#include "../worldgen/biome.h"

// Beta 1.7.3 hostile-mob definitions. Sourced from the Beta 1.7.3 client/server
// entity classes:
//   - Health: EntityLiving defaults max health to 20 in Beta; none of these
//     four override it (modern spiders are 16, but Beta spiders are 20).
//   - Drops: the dropFewItems() of each Beta 1.7.3 entity. Note these are the
//     *Beta 1.7.3* drops, which predate later additions:
//       zombie   -> feather   (rotten flesh was added in Beta 1.8)
//       skeleton -> arrow AND bone (PARITY FIX 2026-09-01: bones are NOT a
//                   post-1.7.3 addition -- the 1.7.3-LTS decompile (740c583)
//                   shows EntitySkeleton.dropFewItems already dropping BOTH,
//                   via two independent nextInt(3) rolls, a complete override
//                   of the generic single-item EntityMob.dropFewItems this
//                   `.drop` field feeds. The `.drop = ITEM_ARROW` below only
//                   covers the arrow half; the bone half is a skeleton-specific
//                   special case in entity_monster.c's death-drop site, same
//                   pattern as the sheep/squid/pig cases there.)
//       spider   -> string     (spider eyes were added in Beta 1.9)
//       creeper  -> gunpowder
//   - Spawn weight: Beta biomes register each of these four in the monster
//     spawn list with weight 10 (uniform), so the table is uniform here.
//
// drop_count: AUDIT #18 ITEM 4 -- this static field is now just a fallback/default;
// the ACTUAL drop count is rolled at the death-drop site (entity_monster.c mob_die_tick)
// as Beta's EntityMob.dropFewItems does: `rand.nextInt(3)` -> 0, 1, or 2 of the item,
// uniform, with the SEEDED rand_src (never libc rand). It is rolled at the drop site, not
// here, because the factory has no seeded RNG in scope (see the CAVEAT in mob_die_tick).
// Kept at 1 as a harmless default for the item_data the factory stamps; the real count
// overrides it on death. (No looting/Fortune exists in b1.7.3 -- a flat nextInt(3).)
//
// move_speed is the Beta 1.7.3 EntityCreature.moveSpeed, byte-sourced from two
// independent decompiles (PeytonPlayz595 + betasharp): EntityLiving defaults to
// 0.7; EntityZombie OVERRIDES to 0.5; EntitySpider OVERRIDES to 0.8; EntitySkeleton
// and EntityCreeper INHERIT 0.7. (The mc173 reimpl's flat 0.5 for skeleton/creeper
// is an error.) The Wii ground-speed cap (combat.h mob_speed_cap_wii) keeps this
// relative ordering -- zombie slowest, spider fastest -- under the playability nerf.
static const struct monster_def monster_defs[MONSTER_COUNT] = {
	// Geometry: biped hostiles inherit Entity's 0.6 x 1.8 box + the 1.53 eye
	// (= 1.8 * 0.85); the spider is 1.4 x 0.9 with a 0.765 eye
	// (= 0.9 * 0.85). These mirror combat.h's MOB_BODY_HEIGHT / SPIDER_BBOX_* /
	// MOB_EYE_HEIGHT_* and stay within the 1e-4 the geometry tests assert. passive +
	// fall_immune are false for all four hostiles.
	[MONSTER_ZOMBIE] = {
		.name = "Zombie",
		.health = 20,
		.drop = ITEM_FEATHER,
		.drop_count = 1,
		.spawn_weight = 10,
		.move_speed = 0.5f,
		.box_w = 0.6f, .box_h = 1.8f, .eye_height = 1.53f,
	},
	[MONSTER_CREEPER] = {
		.name = "Creeper",
		.health = 20,
		.drop = ITEM_GUNPOWDER,
		.drop_count = 1,
		.spawn_weight = 10,
		.move_speed = 0.7f,
		.box_w = 0.6f, .box_h = 1.8f, .eye_height = 1.53f,
	},
	[MONSTER_SKELETON] = {
		.name = "Skeleton",
		.health = 20,
		.drop = ITEM_ARROW,
		.drop_count = 1,
		.spawn_weight = 10,
		.move_speed = 0.7f,
		.box_w = 0.6f, .box_h = 1.8f, .eye_height = 1.53f,
	},
	[MONSTER_SPIDER] = {
		.name = "Spider",
		.health = 20,
		.drop = ITEM_STRING,
		.drop_count = 1,
		.spawn_weight = 10,
		.move_speed = 0.8f,
		.box_w = 1.4f, .box_h = 0.9f, .eye_height = 0.765f,
	},
	// Beta 1.7.3 passive animal (EntityAnimal). hp 10, box 0.9 x 0.9 (setSize 0.9F,0.9F),
	// eye 0.765 (= 0.9 * 0.85), move_speed 0.7 (EntityCreature default). Drops raw
	// PORKCHOP 0-2 (-> COOKED if killed while on fire, swapped at the death site).
	// spawn_weight 0: animals are NOT in the hostile spawn table -- they come from the
	// separate animal spawn pass, so the hostile picker (which skips weight <= 0) is
	// byte-unchanged. passive=true gates off targeting/attack/retaliation/sun-burn.
	[MONSTER_PIG] = {
		.name = "Pig",
		.health = 10,
		.drop = ITEM_PORKCHOP,
		.drop_count = 1,
		.spawn_weight = 0,
		.animal_weight = 10,
		.move_speed = 0.7f,
		.box_w = 0.9f, .box_h = 0.9f, .eye_height = 0.765f,
		.passive = true,
	},
	// Beta 1.7.3 cow (EntityCow extends EntityAnimal). Verified against the
	// PeytonPlayz595/Beta-1.7.3 decompile: hp 10 (EntityLiving default), setSize(0.9F,1.3F)
	// -> box 0.9 x 1.3, eye 1.105 (= 1.3 * 0.85), EntityCreature move_speed 0.7. Drops
	// LEATHER 0-2 ONLY -- raw beef was added in Beta 1.8, so a faithful 1.7.3 cow drops no
	// meat. animal_weight 8 (Beta creature spawn list). Milking (EntityCow.interact:
	// empty bucket -> whole-slot ITEM_MILK_BUCKET swap) is LIVE via the M4
	// SRPC_ENTITY_USE dispatch in server_local.c.
	[MONSTER_COW] = {
		.name = "Cow",
		.health = 10,
		.drop = ITEM_LEATHER,
		.drop_count = 1,
		.spawn_weight = 0,
		.animal_weight = 8,
		.move_speed = 0.7f,
		.box_w = 0.9f, .box_h = 1.3f, .eye_height = 1.105f,
		.passive = true,
	},
	// Beta 1.7.3 chicken (EntityChicken extends EntityAnimal). hp 4 (EXPLICIT override --
	// not the default 10), setSize(0.3F,0.4F) -> box 0.3 x 0.4 (the smallest mob), eye 0.34
	// (= 0.4 * 0.85), move_speed 0.7. Drops FEATHER 0-2 ONLY -- raw chicken meat was added
	// in Beta 1.8. fall_immune: EntityChicken overrides fall() to empty (no fall damage; it
	// flaps down). animal_weight 10. Egg-laying (timeUntilNextEgg 6000-11999 -> drop 1
	// ITEM_EGG + re-roll) is LIVE in the chicken branch of server_tick_monster (M4 S3).
	[MONSTER_CHICKEN] = {
		.name = "Chicken",
		.health = 4,
		.drop = ITEM_FEATHER,
		.drop_count = 1,
		.spawn_weight = 0,
		.animal_weight = 10,
		.move_speed = 0.7f,
		.box_w = 0.3f, .box_h = 0.4f, .eye_height = 0.34f,
		.passive = true,
		.fall_immune = true,
	},
	// Beta 1.7.3 sheep (EntitySheep extends EntityAnimal). hp 10 (EntityLiving default),
	// setSize(0.9F,1.3F) -> box 0.9 x 1.3, eye 1.105 (= 1.3 * 0.85), move_speed 0.7. animal_weight
	// 12 (Beta creature spawn list: sheep 12 -- the HIGHEST of the animals, so a herd skews sheep).
	// DROP: Beta dropFewItems drops EXACTLY 1 BLOCK_WOOL of the fleece colour (NOT nextInt(3)) if
	// !sheared; the .drop id is set here but the COLOUR (durability) + flat count=1 are applied at
	// the death site (mob_die_tick), since the colour is the per-entity id-hash. SHEARING is LIVE
	// (M4 S2, SRPC_ENTITY_USE dispatch): shears -> sheared=true + 2-4 wool; a sheared sheep drops
	// nothing on death and NEVER regrows (b1.7.3 has no regrowth). See sheep_fleece_color().
	[MONSTER_SHEEP] = {
		.name = "Sheep",
		.health = 10,
		.drop = BLOCK_WOOL,
		.drop_count = 1,
		.spawn_weight = 0,
		.animal_weight = 12,
		.move_speed = 0.7f,
		.box_w = 0.9f, .box_h = 1.3f, .eye_height = 1.105f,
		.passive = true,
	},
	// Beta 1.7.3 squid (EntitySquid extends EntityWaterMob). Verified vs the
	// PeytonPlayz595 decompile (2026-06-11 fetch): hp 10 (EntityLiving default, no
	// override), setSize(0.95F, 0.95F) -> box 0.95 x 0.95, eye 0.8075 (= 0.95*0.85).
	// Drops 1-3 INK SACS -- dropFewItems is `rand.nextInt(3) + 1` (the only +1 drop
	// roll in the mob set; the death site adds the +1, keeping the universal
	// nextInt(3) stream shape) of `new ItemStack(Item.dyePowder, 1, 0)` -> ITEM_DYE
	// with durability 0 (the sheep wool-colour-via-durability precedent; the factory
	// already stamps durability 0). move_speed 0.7 is UNUSED (the squid bypasses the
	// walk push entirely -- its swim-vector physics is its own branch); kept at the
	// EntityCreature default for table uniformity. spawn_weight AND animal_weight 0:
	// the squid sits in NEITHER land picker -- Beta puts it ALONE in
	// spawnableWaterCreatureList (weight 10, cap 5), so it spawns from its own water
	// pass. water_creature drives that pass + excludes it from the animal cap count.
	// NOT fall_immune: b1.7.3 EntityWaterMob does NOT override fall() -- a beached
	// squid takes fall damage (the no-fall-damage squid is a later version).
	[MONSTER_SQUID] = {
		.name = "Squid",
		.health = 10,
		.drop = ITEM_DYE,
		.drop_count = 1,
		.spawn_weight = 0,
		.animal_weight = 0,
		.move_speed = 0.7f,
		.box_w = 0.95f, .box_h = 0.95f, .eye_height = 0.8075f,
		.passive = true,
		.water_creature = true,
	},
	// Beta 1.7.3 slime (EntitySlime extends EntityLiving + IMob -- NOT EntityMob:
	// no light spawn gate, no peaceful auto-kill). Beta's ONE slime entity rolls
	// size = 1 << nextInt(3) (1/2/4, uniform) at construction; v2 encodes the SIZE
	// IN THE ID so the const def table drives box/health/drops with zero per-entity
	// state: setSlimeSize(n) -> setSize(0.6n, 0.6n), health = n*n (1/4/16), eye =
	// 0.6n * 0.85. Drops: getDropItemId returns slimeBall (341) ONLY at size 1
	// (0-2 via the universal nextInt(3)); sizes 2/4 return 0 -> .drop = 0 and the
	// death site skips both the count roll and item spawn. SLIME_SMALL is the
	// SENTINEL row for the hostile picker: Beta
	// registers ONE weight-10 EntitySlime entry in every biome's monster list, so
	// only this row carries spawn weight -- the picker returning SLIME_SMALL means
	// "a slime pack", and the spawn pass re-rolls each member's ACTUAL size
	// (1 << nextInt(3)), exactly as every Beta pack member constructs its own
	// EntitySlime. spawn_weight is 0 UNTIL the spawn-pass commit lands the slime
	// gate (chunk-seeded jrand + y<16 + the difficulty clause) -- flipping it on
	// without that gate would spawn slimes in any dark cell, which is NOT Beta.
	// passive=false (default): slimes acquire + retaliate like any hostile.
	[MONSTER_SLIME_SMALL] = {
		.name = "Slime",
		.health = 1,
		.drop = ITEM_SLIME_BALL,
		.drop_count = 1,
		.spawn_weight = 10, // Beta's single weight-10 EntitySlime list entry;
		                    // the spawn pass gates it with the slime-chunk
		                    // check + y<16 and re-rolls each member's size
		.animal_weight = 0,
		.move_speed = 0.7f,
		.box_w = 0.6f, .box_h = 0.6f, .eye_height = 0.51f,
	},
	[MONSTER_SLIME_MEDIUM] = {
		.name = "Slime",
		.health = 4,
		.drop = 0, // Beta getDropItemId: 0 at size > 1 (no drop)
		.drop_count = 0,
		.spawn_weight = 0, // never picked directly; only split/size-reroll spawns it
		.animal_weight = 0,
		.move_speed = 0.7f,
		.box_w = 1.2f, .box_h = 1.2f, .eye_height = 1.02f,
	},
	[MONSTER_SLIME_BIG] = {
		.name = "Slime",
		.health = 16,
		.drop = 0, // Beta getDropItemId: 0 at size > 1 (no drop)
		.drop_count = 0,
		.spawn_weight = 0, // never picked directly; only the size re-roll spawns it
		.animal_weight = 0,
		.move_speed = 0.7f,
		.box_w = 2.4f, .box_h = 2.4f, .eye_height = 2.04f,
	},
	// === NETHER NATIVES (batch 1b) ==========================================
	// [740c583: EntityPigZombie] zombie chassis (health 20, 0.6x1.8, eye 1.53,
	// moveSpeed 0.5F base -- 0.95F only while angry, applied in the AI), drops
	// COOKED porkchop, fire-immune, NO light spawn gate (its getCanSpawnHere
	// override drops EntityMob's), neutral-until-hit (angerLevel state in the
	// AI). Absent from the overworld table (spawn_weight 0); Nether weight 10
	// [BiomeGenHell].
	[MONSTER_PIG_ZOMBIE] = {
		.name = "PigZombie", // Beta EntityList name
		.health = 20,
		.drop = ITEM_PORKCHOP_COOKED,
		.drop_count = 1,
		.spawn_weight = 0,
		.nether_weight = 10,
		.move_speed = 0.5f,
		.box_w = 0.6f, .box_h = 1.8f, .eye_height = 1.53f,
		.fire_immune = true,
		.no_light_gate = true,
	},
	// [740c583: EntityGhast] setSize(4.0F, 4.0F), health 10, drops gunpowder,
	// fire-immune, flying (EntityFlying: no gravity, no fall damage -- the AI
	// branch owns locomotion; move_speed is nominal, the ghast steers by the
	// 0.1/dist waypoint acceleration). Eye = 4.0 * 0.85 (EntityLiving
	// getEyeHeight). No light gate; its own extra spawn roll (nextInt(20)==0)
	// + max 1 per pack live in the spawn pass. Nether weight 10 [BiomeGenHell].
	[MONSTER_GHAST] = {
		.name = "Ghast",
		.health = 10,
		.drop = ITEM_GUNPOWDER,
		.drop_count = 1,
		.spawn_weight = 0,
		.nether_weight = 10,
		.move_speed = 0.5f,
		.box_w = 4.0f, .box_h = 4.0f, .eye_height = 3.4f,
		.fire_immune = true,
		.no_light_gate = true,
	},
	// Beta EntityWolf constructor: size .8x.8, moveSpeed 1.1, health 8;
	// getEyeHeight returns height*.8 = .64; no drop. It belongs to the creature
	// category/cap, but its conditional combat AI is handled by the wolf slice.
	// [740c583: EntityWolf constructor/getEyeHeight/getDropItemId]
	[MONSTER_WOLF] = {
		.name = "Wolf",
		.health = 8,
		.drop = 0,
		.drop_count = 0,
		.spawn_weight = 0,
		.animal_weight = 0, // biome-specific weight 2 lives in the picker below
		.move_speed = 1.1f,
		.box_w = 0.8f, .box_h = 0.8f, .eye_height = 0.64f,
		.passive = true, // creature cap; wolf-specific retaliation bypasses this AI gate
	},
	// Beta EntityGiantZombie [740c583]: extends EntityMob DIRECTLY -- NOT
	// EntityZombie. Ctor: moveSpeed 0.5F; attackStrength 50 (combat.h
	// GIANT_ATTACK_DAMAGE); `health *= 10` on EntityLiving's 20 -> 200;
	// setSize(width*6, height*6) on the biped 0.6x1.8 -> 3.6 x 10.8, eye =
	// 10.8 * 0.85 = 9.18. EVERY spawn weight is 0: the giant is in no Beta
	// spawn list (overworld, nether, creature, water) and must NEVER be
	// picked naturally -- pinned by test_giant. No getDropItemId override on
	// the EntityMob lineage -> drops nothing (the zombie's feather is
	// EntityZombie's own override, which the giant does not inherit). Not
	// passive, no fire immunity, keeps EntityMob's darkness spawn gate
	// (getCanSpawnHere is not overridden) -- moot while unspawnable
	// naturally, but kept faithful for the spawner path.
	[MONSTER_GIANT] = {
		.name = "Giant",
		.health = 200,
		.drop = 0,
		.drop_count = 0,
		.spawn_weight = 0,
		.animal_weight = 0,
		.move_speed = 0.5f,
		// Beta's FLOAT products of the Entity defaults 0.6F/1.8F [740c583:
		// EntityGiantZombie.java:11, Entity.java:37-38]: 3.60000014F x
		// 10.7999992F. The literals 3.6f/10.8f are one ulp off each way and
		// floor into different cells at box edges (test_giant_box_golden).
		// Eye = EntityLiving.getEyeHeight `height * 0.85F` on that product:
		// 9.17999935F (0x4112E147), not the 9.18f literal (0x4112E148).
		.box_w = 0.6f * 6.0f, .box_h = 1.8f * 6.0f,
		.eye_height = (1.8f * 6.0f) * 0.85f,
	},
};

enum wolf_living_sound wolf_living_sound_pick(uint8_t flags,
										 int watched_health, int roll3) {
	if(wolf_flag_get(flags, WOLF_FLAG_ANGRY))
		return WOLF_LIVING_GROWL;
	if(roll3 == 0)
		return wolf_flag_get(flags, WOLF_FLAG_TAMED) && watched_health < 10
			? WOLF_LIVING_WHINE : WOLF_LIVING_PANTING;
	return WOLF_LIVING_BARK;
}

float wolf_tail_angle(uint8_t flags, int watched_health) {
	if(wolf_flag_get(flags, WOLF_FLAG_ANGRY))
		return 1.5393804f;
	if(wolf_flag_get(flags, WOLF_FLAG_TAMED))
		return (0.55f - (float)(20 - watched_health) * 0.02f) * 3.1415927f;
	return 0.62831855f;
}

float wolf_shading_while_shaking(float previous, float current, float partial) {
	return 0.75f + (previous + (current - previous) * partial) / 2.0f * 0.25f;
}

float wolf_shake_angle(float previous, float current, float partial,
					   float offset) {
	float t = (previous + (current - previous) * partial + offset) / 1.8f;
	if(t < 0.0f) t = 0.0f;
	else if(t > 1.0f) t = 1.0f;
	return sinf(t * 3.1415927f) * sinf(t * 3.1415927f * 11.0f)
		* 0.15f * 3.1415927f;
}

float wolf_interested_angle(float previous, float current, float partial) {
	return (previous + (current - previous) * partial) * 0.15f * 3.1415927f;
}

int wolf_adjust_incoming_damage(int damage, bool attacker_is_player,
								 bool attacker_is_arrow) {
	if(attacker_is_player || attacker_is_arrow)
		return damage;
	/* Algebraically (damage+1)/2 for the non-negative damage domain, without
	 * signed-overflow UB if a corrupt caller supplies INT_MAX. */
	return damage >= 0 ? damage / 2 + damage % 2 : (damage + 1) / 2;
}

bool wolf_can_despawn(uint8_t flags) {
	return !wolf_flag_get(flags, WOLF_FLAG_TAMED);
}

const struct monster_def* monster_def_get(int monster_id) {
	if(monster_id < 0 || monster_id >= MONSTER_COUNT)
		monster_id = MONSTER_ZOMBIE; // crash-safe fallback
	return &monster_defs[monster_id];
}

/* Weight-column selector for the two hostile spawn tables: dim < 0 (the
 * Nether, WORLD_DIM_NETHER == -1) reads Beta BiomeGenHell's list via
 * nether_weight; every other dim reads the overworld spawn_weight column. */
static int hostile_weight(int dim, int i) {
	return dim < 0 ? monster_defs[i].nether_weight
				   : monster_defs[i].spawn_weight;
}

int monster_spawn_total_weight_dim(int dim) {
	int total = 0;
	for(int i = 0; i < MONSTER_COUNT; i++)
		total += hostile_weight(dim, i);
	return total;
}

int monster_spawn_pick_dim(int dim, int roll) {
	if(roll < 0)
		roll = 0;
	for(int i = 0; i < MONSTER_COUNT; i++) {
		if(hostile_weight(dim, i) <= 0)
			continue;
		roll -= hostile_weight(dim, i);
		if(roll < 0)
			return i;
	}
	// roll >= total weight: clamp to the last mob carrying weight.
	for(int i = MONSTER_COUNT - 1; i >= 0; i--) {
		if(hostile_weight(dim, i) > 0)
			return i;
	}
	return MONSTER_ZOMBIE;
}

int monster_spawn_total_weight(void) {
	return monster_spawn_total_weight_dim(0);
}

int monster_spawn_pick(int roll) {
	return monster_spawn_pick_dim(0, roll);
}

int animal_spawn_total_weight(void) {
	int total = 0;
	for(int i = 0; i < MONSTER_COUNT; i++)
		total += monster_defs[i].animal_weight;
	return total;
}

int animal_spawn_pick(int roll) {
	if(roll < 0)
		roll = 0;
	for(int i = 0; i < MONSTER_COUNT; i++) {
		if(monster_defs[i].animal_weight <= 0)
			continue; // hostiles (weight 0) are never animal-picked
		roll -= monster_defs[i].animal_weight;
		if(roll < 0)
			return i;
	}
	// roll >= total weight: clamp to the last animal carrying weight.
	for(int i = MONSTER_COUNT - 1; i >= 0; i--) {
		if(monster_defs[i].animal_weight > 0)
			return i;
	}
	return MONSTER_PIG;
}

int animal_spawn_total_weight_biome(int biome) {
	int total = animal_spawn_total_weight();
	return (biome == BIOME_FOREST || biome == BIOME_TAIGA) ? total + 2 : total;
}

int animal_spawn_pick_biome(int biome, int roll) {
	int base = animal_spawn_total_weight();
	if((biome == BIOME_FOREST || biome == BIOME_TAIGA) && roll >= base)
		return MONSTER_WOLF;
	return animal_spawn_pick(roll);
}

int monster_max_spawned_in_chunk(int monster_id) {
	/* [740c583: EntityWolf.getMaxSpawnedInChunk] */
	return monster_id == MONSTER_WOLF ? 8 : 4;
}
