/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

/*
	Beta 1.7.3 EntityFish + ItemFishingRod pure decision helpers (fishing-slice,
	decompile 740c583). See entity_fish.c for the full onUpdate/catchFish port;
	this header exists so server_local.c's cast/reel dispatch AND
	tests/test_fishing_env.c can drive the exact production formulas without
	the full entity-tick machinery (CODESTYLE.md S3.2 -- "prefer pure, injected
	logic for anything testable").
*/

#ifndef ENTITY_FISH_H
#define ENTITY_FISH_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../cglm/cglm.h"

struct entity;
struct server_local;

/* EntityFish ctor: the hook's box is 0.25 wide and 0.25 tall [740c583]. */
#define FISH_BBOX_SIZE 0.25f

/* EntityFish(World,EntityPlayer) ctor: calculateVelocity with speed 1.5, spread 1
 * [740c583]. The 0.0075-sigma-per-axis gaussian heading spread is OMITTED,
 * matching the established bow/egg/snowball precedent (sub-degree cosmetic
 * jitter; entity_egg.c file header states the same reasoning). */
#define FISH_LAUNCH_SPEED 1.5f

/* EntityFish ctor position offset: X moves back by cos(yaw) x 0.16 and Z by
 * sin(yaw) x 0.16 [740c583]. */
#define FISH_CAST_XZ_OFFSET 0.16f
/* EntityFish ctor: the hook starts 0.1 below the eye height [740c583]. */
#define FISH_CAST_Y_OFFSET 0.1f

/* EntityFish.onUpdate liveness gate: a squared distance to the angler above 1024
 * kills the hook [740c583]. */
#define FISH_ANGLER_MAX_DIST_SQ 1024.0f

/* EntityFish.onUpdate: the hook dies once it has been stuck in the ground for 1200 ticks
 * [740c583]. See entity.h's entity_fish.ticks_in_ground comment for why the
 * xTile/yTile/zTile "already unstuck?" recheck ahead of this is unreachable
 * in this engine and therefore not ported. */
#define FISH_STUCK_DESPAWN_TICKS 1200

/* EntityFish.onUpdate bite roll [740c583]: one in 500 per tick, one in 300
 * when the cell above the hook can be rained on. */
#define FISH_BITE_CHANCE_BASE 500
#define FISH_BITE_CHANCE_RAIN 300

/* EntityFish.onUpdate: a bite stays catchable for 10 plus a 0..29 roll ticks [740c583]. */
#define FISH_BITE_DURATION_RANGE 30
#define FISH_BITE_DURATION_MIN 10

/* EntityFish.onUpdate bite kick: vertical motion drops by 0.2 [740c583]. */
#define FISH_BITE_TUG_Y (-0.2f)

/* EntityFish.onUpdate: the target/candidate box is expanded 0.3 on each axis before the
 * intercept test [740c583] -- the SAME value as ARROW_HIT_EXPAND
 * (combat.h), reused rather than redefined (both transcribe the identical
 * Beta box growth by 0.3 on every axis). */

/* EntityFish.onUpdate submersion probe: 5 equal-height horizontal slices of
 * the hook's own bounding box [740c583]. */
#define FISH_SUBMERSION_SLICES 5

/* EntityFish.onUpdate tail (the "not in water" / "in water" shared drag):
 * the drag is 0.92 normally, 0.5 on ground/collided horizontally; an extra
 * factor of 0.9 (drag) / 0.8 (vertical motion) applies whenever ANY part is submerged
 * [740c583]. */
#define FISH_DRAG_AIR 0.92f
#define FISH_DRAG_GROUND 0.5f
#define FISH_DRAG_WET_MULT 0.9f
#define FISH_MOTION_Y_WET_MULT 0.8f
/* Buoyancy: vertical motion gains 0.04 x (2 x submerged fraction - 1). */
#define FISH_BUOYANCY 0.04f

/* Pure: EntityFish.catchFish()'s return-value table [740c583]:
 *   the cost starts at 0; a hooked entity makes it 3, otherwise a catchable
 *   bite makes it 1 (an either/or chain). AFTER that chain a SEPARATE,
 *   UNCONDITIONAL in-ground check sets it to 2 -- it OVERRIDES either branch
 *   above, including "nothing" (0).
 *
 *
 * `hooked_entity` mirrors Beta's bobber field being set, `catchable` mirrors
 * a positive ticksCatchable, `in_ground` mirrors inGround. Order matters: this
 * is transcribed as the exact same two-step shape (chain, then override),
 * not collapsed into a single priority list, so the override reads the same
 * way in C as it does in the decompile. */
static inline uint16_t fish_catch_cost(bool hooked_entity, bool catchable,
									   bool in_ground) {
	uint16_t cost = 0;
	if(hooked_entity)
		cost = 3;
	else if(catchable)
		cost = 1;
	if(in_ground)
		cost = 2;
	return cost;
}

/* Pure: EntityFish.catchFish()'s toward-angler pull [740c583], used
 * IDENTICALLY by both non-empty branches (the hooked-entity yank adds this
 * to the target's existing motion; the caught-fish EntityItem assigns it
 * outright -- the caller decides which). dx/dy/dz = angler - target
 * (double precision, matching Beta's own double locals for this one
 * calculation -- CODESTYLE S2.4's "convert exactly once" boundary is the
 * final float store into `out`, not this intermediate math).
 *   dist is the STRAIGHT-LINE distance sqrt(dx^2+dy^2+dz^2) (NOT squared);
 *   the pull is dx x 0.1 on X and dz x 0.1 on Z; on Y it is dy x 0.1 plus
 *   sqrt(dist) x 0.08 -- the square root of the distance itself, not of
 *   dist^2 again -- an easy mistranscription.
 *
 *
 *
 */
static inline void fish_pull_toward_angler(double dx, double dy, double dz,
										   vec3 out) {
	double dist = sqrt(dx * dx + dy * dy + dz * dz);
	out[0] = (float)(dx * 0.1);
	out[1] = (float)(dy * 0.1 + sqrt(dist) * 0.08);
	out[2] = (float)(dz * 0.1);
}

/* Pure: EntityFish.onUpdate's bite-chance selection [740c583]:
 *   the chance is one in 500, or one in 300 when the world reports the cell
 *   (floor x, floor y + 1, floor z) as rained on (canBlockBeRainedOn).
 *
 * `rained_on` is the caller's ALREADY-EVALUATED canBlockBeRainedOn result --
 * kept as a separate step (not folded into this function) so the Y+1 query
 * site is visible and independently testable at the call site (the +1 is
 * the trap: querying the hook's OWN cell, not the one above it, silently
 * loses the rain speed-up). */
static inline int fish_bite_chance(bool rained_on) {
	return rained_on ? FISH_BITE_CHANCE_RAIN : FISH_BITE_CHANCE_BASE;
}

/* Pure: EntityFish.onUpdate: the catchable ticks are a 30-way roll plus 10
 * [740c583]. `roll` is the raw 30-way draw (0..29); result is 10..39. */
static inline int fish_bite_duration(int roll) {
	return roll + FISH_BITE_DURATION_MIN;
}

/* Pure: the splash-sound pitch on a bite [740c583]:
 *   the "random.splash" sound plays at volume 0.25 with pitch 1 plus (first
 *   float minus second float) x 0.4.
 * TWO nextFloat() draws, in this exact order. `first_draw`/`second_draw`
 * are the caller's ALREADY-DRAWN values (CODESTYLE S2.3: sequence multiple
 * RNG draws feeding one call through named locals at the call site, never
 * as sibling arguments -- C's evaluation order for two calls in one
 * expression is unspecified, and the SIGN here makes the order observable:
 * the first draw is added, the second is subtracted). Keeping this pure
 * (no RNG access) lets a test pin the arithmetic AND, separately, pin that
 * the caller drew in the right order. */
static inline float fish_splash_pitch(float first_draw, float second_draw) {
	return 1.0f + (first_draw - second_draw) * 0.4f;
}

/* Beta EntityFish(World,EntityPlayer) ctor's cast pose [740c583]. `eye`
 * is the angler's eye position (CaveWii's own player.x/y/z convention --
 * y is ALREADY the eye).
 *
 * CORRECTED CITATION (adversarial-review finding 8; the line this replaces
 * claimed EntityPlayer's yOffset is 0, which is FALSE -- EntityPlayer's ctor
 * and preparePlayerToSpawn both set the yOffset to 1.62
 * [740c583:EntityPlayer]). The real cancellation is TWO independent facts,
 * not a zero yOffset:
 *   1. Entity.setPosition puts the bounding box at
 *      a box minimum Y of posY minus yOffset plus ySize [740c583:Entity], so for
 *      a standing player (yOffset 1.62, ySize 0) the FEET sit at posY-1.62
 *      -- i.e. Beta's own `posY` is ALREADY at eye level, not the feet.
 *   2. This engine's `player.y` independently stores that SAME eye-level
 *      quantity by convention (combat.h: "the player network position is
 *      the EYE; feet are EYE_HEIGHT below it").
 * So the ctor's start height (angler posY + 1.62 - angler yOffset) is
 * posY plus 1.62 minus 1.62, i.e. posY, and that posY maps DIRECTLY onto this
 * engine's `player.y` with NO further +/-1.62 adjustment in either
 * direction -- the two facts above cancel independently of each other, not
 * because yOffset is 0. A maintainer who "corrects" the yOffset citation to
 * 1.62F and mechanically re-derives from THAT premise alone (without also
 * knowing fact 2) would plausibly land on `eye_y + 1.52`, reintroducing a
 * real ~1.62-block-too-high cast-spawn regression -- hence spelling out
 * both facts here instead of the shorthand "yOffset is 0" this replaces.
 * See also FISHING_SLICE_NOTES.md's post-review section (findings 3/5/8):
 * the SAME false "posY is the feet" premise this citation used to encode
 * was independently wrong in server_local_fishing_rod_reel's pull-distance
 * math and entity_fish.c's angler-range liveness check -- catchFish() and
 * getDistanceSqToEntity both use RAW posY-posY, no yOffset term at all.
 *
 * `yaw_deg`/`pitch_deg` are player.rx/player.ry, which map 1:1 onto Beta's
 * rotationYaw/rotationPitch (see server_local_drop_hotbar_item's citation
 * in server_local.c -- the same mapping this function reuses, DIRECTLY, no
 * crx/cry indirection). Writes the spawn position and the
 * (sub-degree-jitter-omitted) launch velocity. */
void fish_cast_pose(float eye_x, float eye_y, float eye_z, float yaw_deg,
					float pitch_deg, vec3 out_pos, vec3 out_vel);

void entity_fish(uint32_t id, struct entity* e, bool server, void* world);

#endif
