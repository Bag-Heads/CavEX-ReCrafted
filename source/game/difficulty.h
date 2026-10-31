/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Beta 1.7.3 difficulty -- the ONE contract every reader goes through
 * (next-gaps-0924 W1F; wave-2 P7 sleep spawns read server_local_difficulty).
 *
 * Spec = the decompile (mcb173-retromcp src_original/net/minecraft/src and
 * .../client). Every site that reads World.difficultySetting in Beta 1.7.3:
 *
 *   GameSettings.java:41      `public int difficulty = 2;` (default Normal)
 *   GameSettings.java:125     menu cycle `difficulty = difficulty + var2 & 3`
 *   GameSettings.java:234/287 persisted as options.txt "difficulty:<n>"
 *   Minecraft.java:1129-1132  world.difficultySetting = gameSettings.difficulty
 *                             EVERY runTick (a multiplayer CLIENT world pins 3;
 *                             the remote server owns the real value)
 *   Minecraft.java:1151       world.setAllowedMobSpawns(difficulty > 0, true)
 *   World.java:1744           sleep spawns need spawnHostileMobs && diff >= 1
 *   SpawnerAnimals.java:47    the monster category is skipped when
 *                             spawnHostileMobs is false (no draws at all)
 *   EntityMob.java:22         onUpdate: server world && diff == 0 -> dead
 *   EntityGhast.java:32       updatePlayerActionState: same removal (only
 *                             reached while alive, EntityLiving.java:618-624)
 *   EntityGhast.java:152      getCanSpawnHere ... && diff > 0
 *   EntityPigZombie.java:28   getCanSpawnHere diff > 0 && ...
 *   EntitySlime.java:136      getCanSpawnHere (size == 1 || diff > 0) && ...
 *   EntityLightningBolt.java:16  fire placement only when diff >= 2
 *   EntityPlayer.java:173     Peaceful regen (see difficulty_peaceful_regen_due)
 *   EntityPlayer.java:366-377 damage scaling (see difficulty_scale_mob_damage)
 *
 * Difficulty is a GAME SETTING in Beta (options.txt), not per-world state:
 * CaveWii persists it in config.json under DIFFICULTY_CONFIG_KEY and the local
 * server latches it once per tick, exactly like Minecraft.runTick's copy.
 * Nothing is ever sent to a REMOTE server: a multiplayer server owns its own
 * difficulty. Split-screen host and guest are two bodies in ONE local world,
 * so they share the one latched value by construction. */

#ifndef CAVEWII_GAME_DIFFICULTY_H
#define CAVEWII_GAME_DIFFICULTY_H

#include <stdbool.h>

#include "../entity/monsters_object.h"

enum difficulty {
	DIFFICULTY_PEACEFUL = 0,
	DIFFICULTY_EASY = 1,
	DIFFICULTY_NORMAL = 2,
	DIFFICULTY_HARD = 3,
	DIFFICULTY_COUNT = 4,
};

#define DIFFICULTY_DEFAULT DIFFICULTY_NORMAL /* GameSettings.java:41 */
#define DIFFICULTY_CONFIG_KEY "game.difficulty"

static inline bool difficulty_valid(int d) {
	return d >= DIFFICULTY_PEACEFUL && d <= DIFFICULTY_HARD;
}

/* A hand-edited / corrupt config value falls back to the Beta default. Beta
 * itself would Integer.parseInt the raw value and then index DIFFICULTIES[]
 * out of bounds; failing safe to Normal is the least-invention choice. */
static inline int difficulty_sanitize(int d) {
	return difficulty_valid(d) ? d : DIFFICULTY_DEFAULT;
}

/* GameSettings.java:125 `this.difficulty = this.difficulty + var2 & 3;`
 * (+ binds tighter than &): Peaceful -> Easy -> Normal -> Hard -> Peaceful. */
static inline int difficulty_cycle(int d, int step) {
	return (d + step) & 3;
}

/* en_US.lang options.difficulty.{peaceful,easy,normal,hard} (the keys are
 * GameSettings.java:13's DIFFICULTIES[]). */
static inline const char* difficulty_name(int d) {
	switch(difficulty_sanitize(d)) {
	case DIFFICULTY_PEACEFUL: return "Peaceful";
	case DIFFICULTY_EASY: return "Easy";
	case DIFFICULTY_HARD: return "Hard";
	default: return "Normal";
	}
}

/* Minecraft.java:1151 setAllowedMobSpawns(gameSettings.difficulty > 0, true):
 * the whole hostile (IMob) category is skipped on Peaceful. */
static inline bool difficulty_allows_hostile_spawns(int d) {
	return d > DIFFICULTY_PEACEFUL;
}

/* EntityLightningBolt.java:16 `if(var1.difficultySetting >= 2 && ...)`. */
static inline bool difficulty_lightning_ignites(int d) {
	return d >= DIFFICULTY_NORMAL;
}

/* `instanceof EntityMob` for the CaveWii monster roster. EntityMob subclasses
 * in 1.7.3: EntityZombie (EntityPigZombie extends it), EntitySkeleton,
 * EntityCreeper, EntitySpider, EntityGiantZombie. NOT EntityMob: EntitySlime
 * (EntityLiving implements IMob), EntityGhast (EntityFlying implements IMob),
 * EntityWolf (EntityAnimal) and every passive animal / squid. */
static inline bool monster_is_beta_entity_mob(int monster_id) {
	switch(monster_id) {
	case MONSTER_ZOMBIE:
	case MONSTER_CREEPER:
	case MONSTER_SKELETON:
	case MONSTER_SPIDER:
	case MONSTER_PIG_ZOMBIE:
	case MONSTER_GIANT:
		return true;
	default:
		return false;
	}
}

/* Peaceful removal (server world only). EntityMob.java:22 runs in onUpdate
 * after super.onUpdate, for EVERY EntityMob including one mid death-flop.
 * EntityGhast.java:32 lives in updatePlayerActionState, which
 * EntityLiving.onLivingUpdate only calls while !isMovementBlocked() i.e.
 * health > 0 (EntityLiving.java:618-624, 655-657). Slimes and wolves are NOT
 * removed -- neither class has the check. `health` is on the entity's own
 * (mob) scale; only its sign matters. */
static inline bool difficulty_removes_monster(int d, int monster_id,
											  int health) {
	if(d != DIFFICULTY_PEACEFUL)
		return false;
	if(monster_is_beta_entity_mob(monster_id))
		return true;
	return monster_id == MONSTER_GHAST && health > 0;
}

/* EntityPlayer.attackEntityFrom:366-377, in Beta half-hearts (int math):
 *
 *   when the source is an EntityMob or an EntityArrow:
 *     Peaceful (0): damage = 0;
 *     Easy (1): damage = damage / 3 + 1;
 *     Hard (3): damage = damage * 3 / 2;
 *
 *   then zero damage returns false   (before i-frames, armor, sound, knockback)
 *
 * NOTE Easy is `/ 3 + 1`, not `/ 2 + 1`. Which attackers qualify is the
 * CALLER's decision (monster_is_beta_entity_mob for melee and for an arrow's
 * shooter -- EntityArrow.java:163 passes the owner, not the arrow, so a
 * skeleton arrow scales and a dispensed (owner == null) one does not). */
static inline int difficulty_scale_mob_damage(int d, int half_hearts) {
	switch(d) {
	case DIFFICULTY_PEACEFUL: return 0;
	case DIFFICULTY_EASY: return half_hearts / 3 + 1;
	case DIFFICULTY_HARD: return half_hearts * 3 / 2;
	default: return half_hearts;
	}
}

/* EntityPlayer.onLivingUpdate:173
 *   if(difficultySetting == 0 && health < 20 && ticksExisted % 20 * 12 == 0)
 *       heal(1);
 * `%` and `*` share precedence and bind left-to-right, so the test is
 * (ticksExisted % 20) * 12 == 0, i.e. ticksExisted % 20 == 0: one half-heart
 * every 20 ticks (1 s), NOT every 240. ticksExisted is incremented in
 * Entity.onEntityUpdate (Entity.java:161) before onLivingUpdate runs, and
 * starts at 0 for every freshly constructed EntityPlayer. heal() adds only
 * while health > 0 (EntityLiving.java:296-305). `health`/`max_health` are on
 * the caller's own scale (Beta 20; CaveWii player x8 = 160) -- the test is a
 * pure comparison, so it is scale-free. */
#define DIFFICULTY_PEACEFUL_REGEN_HALF_HEARTS 1
static inline bool difficulty_peaceful_regen_due(int d, int health,
												 int max_health,
												 int ticks_existed) {
	return d == DIFFICULTY_PEACEFUL && health > 0 && health < max_health
		&& ticks_existed % 20 * 12 == 0;
}

/* ---- The local server's live value ---------------------------------------
 * server_local_request_difficulty: any thread (menu, boot). Stores the
 *   requested value; it takes effect at the start of the next server update
 *   (Minecraft.java:1129's per-tick copy). Invalid values are sanitized.
 * server_local_difficulty: the value latched for THIS server tick. Every
 *   server-side reader (damage, spawn, removal, regen, lightning, and wave-2
 *   sleep spawns) must use it. Defaults to DIFFICULTY_DEFAULT.
 * server_local_tick_actor_regen: the per-tick EntityPlayer living step for
 *   every present body (host first): ticksExisted++ then the Peaceful regen.
 * server_local_damage_actor_from_mob_result: server_local_damage_actor_result
 *   for a hit whose Beta attacker is an EntityMob (a skeleton arrow, a creeper
 *   blast): `damage` (player x8 scale, whole half-hearts) is scaled by the
 *   latched difficulty after the dead check / sleep wake; a scaled 0 is
 *   Beta's `return false` -- *accepted stays false and nothing is applied.
 *   Returns false only when the hit killed the body (same contract). */
struct server_local;
struct server_player;
void server_local_request_difficulty(int d);
int server_local_difficulty(const struct server_local* s);
void server_local_tick_actor_regen(struct server_local* s);
bool server_local_damage_actor_from_mob_result(struct server_local* s,
	struct server_player* player, int damage, float kx, float kz,
	bool* accepted);

#endif
