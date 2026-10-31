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


#ifndef ENTITY_MONSTER_H
#define ENTITY_MONSTER_H

#include <stdbool.h>
#include "../cglm/cglm.h"
#include "../block/block_slipperiness.h" // Block.slipperiness (0.6F / ice 0.98F)
#include "entity.h"
#include "monsters_object.h" // MONSTER_* ids + per-mob stat table

// ---- next-gaps-0924 W1B: Beta 1.7.3 EntityLiving movement parity helpers ----
// Pure, float-exact transcriptions so tests/test_mob_move_parity_golden.c can
// pin the chain against values derived from the Java, and server_tick_monster
// (entity_monster.c) consumes the identical arithmetic.

// EntityLiving.moveEntityWithHeading:479-486 / 491-497 (and EntityFlying.
// moveEntityWithHeading:25-31 / 36-42 -- the ghast has the same chain, applied
// to motionY as well at :47):
//   - the friction starts at 0.91 (airborne);
//   - on the ground it becomes the 546 x 0.1 x 0.1 x 0.1 float product, and
//     then, if the block id under the feet (floor x, floor of the box minimum
//     Y minus one, floor z) is nonzero, that block's slipperiness times 0.91.
//   No other input feeds it.
//
// Both javac and gcc constant-fold the 546 x 0.1 x 0.1 x 0.1 product left-to-right in
// float: 0.546000063f (NOT 0.546). Every block defaults to slipperiness 0.6
// (Block.java:143 -> 0.6 x 0.91 in float = 0.546000063f, the same value); ice is 0.98
// (BlockIce.java:8 -> 0.891800046f). `block_under_id` is the id at
// (floor x, floor(bb.minY)-1, floor z); 0 = air/unloaded (Beta getBlockId 0).
static inline float mob_beta_ground_friction(bool on_ground, int block_under_id) {
	float friction = 0.91F;
	if(on_ground) {
		friction = 546.0F * 0.1F * 0.1F * 0.1F;
		if(block_under_id > 0)
			friction = block_slipperiness(block_under_id) * 0.91F;
	}
	return friction;
}

// EntityLiving.moveEntityWithHeading:488-489:
//   the land acceleration scale is 0.16277136 divided by the cube of the
//   friction, and moveFlying gets 0.1 x that scale on the ground, else 0.02.
// The land walk push scales by that factor: 0.999999821f on a default block (Notch's
// 0.16277136 is only an approximation of 0.546^3, so Beta's own stone accel is
// 0.1 x 0.999999821 -- transcribed, not "fixed" to 1.0), 0.229496211f on ice.
static inline float mob_beta_land_accel_scale(float friction) {
	return 0.16277136F / (friction * friction * friction);
}

// EntityLiving.isOnLadder():550-555 -- the ladder test is a single block read:
//   is the block at (floor x, floor of the box minimum Y, floor z) a ladder?
// (mob yOffset is 0, so bb.minY is the feet + ySize). EntitySpider overrides
// isOnLadder() to isCollidedHorizontally (its wall-climb in entity_monster.c);
// EntityFlying (ghast) and EntitySquid override moveEntityWithHeading without
// the ladder branch, so those three never take the generic rule.

// EntityLiving.moveEntityWithHeading:499-524, the PRE-move ladder clamp:
//   while on a ladder, motion X and Z are each clamped to +-0.15, the fall
//   distance resets to 0, and downward motion Y is floored at -0.15 (a double
//   compare). A sneaking entity would also have downward motion Y zeroed, but
//   mobs never sneak, so that branch is not ported here.
//   Order: X low, X high, Z low, Z high, fall reset, then the Y floor.
//
//
#define MOB_LADDER_MOTION_CLAMP 0.15F
static inline void mob_ladder_premove_clamp(float* vx, float* vy, float* vz,
											float* fall_distance) {
	float rail = MOB_LADDER_MOTION_CLAMP;
	if(*vx < -rail)
		*vx = -rail;
	if(*vx > rail)
		*vx = rail;
	if(*vz < -rail)
		*vz = -rail;
	if(*vz > rail)
		*vz = rail;
	*fall_distance = 0.0F;
	if((double)*vy < -0.15)
		*vy = (float)-0.15;
}

// EntityLiving.moveEntityWithHeading:527-530 + 532-533, the POST-move ascend:
//   after the move, a horizontally collided entity on a ladder gets motion Y
//   0.2; later in the same method gravity subtracts 0.08 and the result is
//   multiplied by 0.98 (the float constant widened to double).
//
//
// Beta writes 0.2 BEFORE its own gravity, so the value the NEXT move actually
// consumes is (0.2 - 0.08) * 0.98 = 0.1176 blocks/tick -- the observable
// climb speed. This engine's vertical integrator is not Beta's (GRAVITY 2.0,
// *0.9 -- see the ySize/step-ascent notes in entity_monster.c), so the climb
// is written AFTER the engine gravity, exactly like the spider wall-climb, as
// the post-gravity Beta value so the per-tick rise matches Beta byte-for-byte.
static inline float mob_ladder_climb_vel_y(void) {
	double motion_y = 0.2;
	motion_y -= 0.08;
	motion_y *= (double)0.98F;
	return (float)motion_y;
}

// EntityLiving.onEntityUpdate:113-115 suffocation:
//   a living mob whose eye box is inside an opaque block takes 1 damage from a
//   null attacker,
// through the normal hurt-resistance window (EntityLiving.attackEntityFrom:317),
// so a mob buried in an opaque block loses 1 every MOB_HURT_RESIST_TICKS.
#define MOB_SUFFOCATION_DAMAGE 1

// T1 PERF: bounded fallback resend interval for server_tick_monster's
// trailing CRPC_ENTITY_MOVE change-gate (entity_monster.c). Exported (not a
// file-local #define) so tests can pin the exact boundary rather than
// hardcode a duplicate literal. See that call site for the full citation.
#define MOB_MOVE_RESEND_INTERVAL_TICKS 20

void entity_monster(uint32_t id, struct entity* e, bool server, void* world, int monster_id);

/* LIVING-SOUND-RNG: Beta's standard living-sound pitch roll
 * (first float minus second float, times 0.2, plus 1) draws BOTH
 * floats from the ENTITY's own Random (Entity.java:50), on the logical
 * (server) world -- EntityLiving.playLivingSound:96, attackEntityFrom:340/345
 * (server decompile line numbers), and the per-mob overrides (ghast, slime,
 * chicken, pig zombie, wolf). The authoritative local simulation therefore
 * draws them from e->gameplay_rng, in Beta order. The two draws are
 * explicitly sequenced (C leaves `f() - f()` operand order unspecified). */
static inline float mob_living_sound_pitch(struct entity* e) {
	float first = jrand_next_float(&e->gameplay_rng);
	float second = jrand_next_float(&e->gameplay_rng);
	return (first - second) * 0.2F + 1.0F;
}

/* One Beta EntityLiving.onEntityUpdate living-sound step on the mob's own
 * Random: a 1000-way roll under livingSoundTime (then +1) -> reset to minus getTalkInterval
 * and playLivingSound() (wolf getLivingSound nextInt(3); pitch floats only
 * for a non-null living sound). Called by the server mob tick; exported so
 * tests can pin the exact draw sequence. */
void entity_monster_living_sound_tick(struct entity* e);

enum monster_damage_result {
	MONSTER_DAMAGE_ABSORBED = 0,
	MONSTER_DAMAGE_EXCESS,
	MONSTER_DAMAGE_FRESH,
};

/* Beta EntityLiving.attackEntityFrom arbitration shared by every mob damage
 * source. `from_skeleton_arrow` is the killing-blow attribution of this event. */
enum monster_damage_result entity_monster_apply_damage(
	struct entity* e, int incoming, bool from_skeleton_arrow);
/* Null-attacker / environmental damage (lava, fire, lightning, suffocation):
 * identical to entity_monster_apply_damage; a wolf additionally gets the
 * EntityWolf.attackEntityFrom sit reset. NO halving: Beta halves only a
 * non-null attacker that is neither a player nor an arrow
 * (EntityWolf.attackEntityFrom:246-248), and environmental damage is the
 * null-attacker call. `s` is required only for the wolf path's routing. */
enum monster_damage_result monster_apply_environment_damage(
	struct entity* e, struct server_local* s, int incoming);
/* Entity.isEntityInsideOpaqueBlock (Entity.java:851-863) on explicit geometry:
 * eight corner samples of a (0.9*width) x 0.1 x (0.9*width) box around the eye
 * at (x, y + eye, z), World.isBlockNormalCube per cell. Exported so
 * tests/test_mob_move_parity_golden.c can sweep it against a naive 8-read
 * transcription: the production body reads each DISTINCT cell once (T1). */
bool entity_monster_inside_opaque_block_at(struct server_local* s, float x,
										   float y, float z, float width,
										   float eye);
/* The post-move Entity.moveEntity callback phase shared by ordinary and piston
 * displacement. The caller handles the death edge after this returns. */
void entity_monster_post_move_cactus_collisions(
	struct entity* e, struct server_local* s);
/* Run EntityLiving.onDeath/dropFewItems once without advancing deathTime. */
void entity_monster_begin_death(
	struct entity* e, struct server_local* s, vec3 drop_pos);
bool entity_monster_has_pending_explosion(const struct entity* e);
/* The client-visible burning flag of an ENTITY_MONSTER: Beta's mid-tick
 * Entity.java:226 flag for fire-immune mobs, end-of-tick fire_time > 0 for the
 * rest. Every server publish of a mob's on_fire must use this (G6c/G6d). */
bool monster_published_on_fire(const struct entity* e);
// Retry an immutable terminal-fuse creeper at its saved center. true means the
// blast committed and the caller must destroy the source entity; false retains it.
bool entity_monster_retry_pending_explosion(
	struct entity* e, struct server_local* s);

struct random_gen;
// Spider jockey (wolf/jockey parity lane): the 1% skeleton-on-spider roll
// [740c583: SpawnerAnimals.creatureSpecificInit] and the mount-linkage
// setter. See the definitions in entity_monster.c for full citations.
bool spider_jockey_roll(struct random_gen* rand_src);
void entity_monster_mount_jockey(struct entity* mount, struct entity* rider);

// Beta-faithful world LOS opacity predicate for los_segment_blocked (los.h): `ctx` is
// a `struct server_world*`. Despite living in this file (first written for mob AI's
// canEntityBeSeen), it is generic -- exported so other callers needing the identical
// Beta LOS rule (e.g. server_local.c's boat interact/attack reach gates) share one
// tested predicate instead of a second copy that could drift (CODESTYLE.md §3.2).
bool mob_los_cell_opaque(int x, int y, int z, void* ctx);

/* next-gaps-0924/P7: Pathfinder.createEntityPathTo from mob to target within range for a candidate mob
 * that is not an entity yet (SpawnerAnimals.performSleepSpawning [740c583 :171,225]), on the
 * same scratch pathfinder + world predicate as mob AI. false / length 0 == Beta's null path. */
bool monster_path_to_entity(struct server_local* s, float mob_x, float mob_y,
							float mob_z, float mob_width, float mob_height,
							float target_x, float target_y, float target_z,
							float search_range, struct path_entity* out_path);

/* The shared left-click attack handler (svin SRPC_ENTITY_ATTACK on e->id).
 * Lives in entity_monster.c historically; also wired onto remote players
 * (entity_remote_player.c) since Beta attacks both through the same
 * PlayerControllerMP.attackEntity path [740c583]. */
bool onLeftClick(struct entity* e);


/* Mob motion units S1: a mob's teleport = entity_default_teleport + the
 * authoritative double-state seed (mob_motion.h). */
void entity_monster_teleport(struct entity* e, vec3 pos);

/* Mob motion units S2: the collision context a mob's moveEntity uses
 * (blocks + moving pistons + boat/minecart boxes), for external movers of a
 * mob (the piston push). */
struct server_local;
struct mob_collide_env entity_monster_collide_env(struct server_local* s);

#endif

