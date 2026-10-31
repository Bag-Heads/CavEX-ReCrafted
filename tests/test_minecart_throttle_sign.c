/*
	Rider intent -> signed axis step (entity_minecart_resolve_throttle).

	THE BUG THIS PINS. `speed` is signed along the cart's heading (hx,hz), and
	seed_heading always points that axis at +X / +Z. The client used to put the
	raw key on the wire, so FORWARD literally meant "toward +X/+Z" no matter
	which way the cart or the rider was pointing. On a northbound cart
	(hz = +1, speed = -0.20) holding FORWARD decelerated it, stopped it at ~20
	ticks, and had it heading SOUTH by ~40. Exactly half of all headings were
	backwards and the rider had no way to tell which half they were on -- the
	residue of the original "minecarts only go in one direction" report.

	THE RULE, and why it is not simply "use the player's facing":
	  * MOVING -> forward = FASTER along the current direction of travel. A
	    facing test flaps when the rider looks near-perpendicular to the rail:
	    a few degrees of stick wobble around 90 deg would invert the throttle
	    between ticks while the player is just glancing around.
	  * AT REST -> there is no direction of travel, so fall back to where the
	    rider is looking along the rail axis. That is what lets you set off the
	    way you are facing.

	NOT A BETA MECHANIC. Beta's EntityMinecart couples no rider motion at all
	(EntityBoat does; the minecart deliberately does not) [740c583]. The whole
	throttle is the documented Wii-controller playability deviation -- see
	docs/deviations.md and docs/wii_controller_playability.md #19. What this
	file pins is that the deviation behaves predictably in BOTH directions.

	Player forward in world XZ is (sin yaw, cos yaw) -- the same basis
	entity_local_player.c:186 uses to turn stick input into walking motion.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "entity/entity_minecart.h"

#define YAW_NORTH ((float)M_PI) /* (sin,cos) = (0,-1) -> looking toward -Z */
#define YAW_SOUTH 0.0f			/* (sin,cos) = (0,+1) -> looking toward +Z */
#define YAW_EAST ((float)M_PI / 2.0f)  /* (sin,cos) = (+1,0) -> toward +X */
#define YAW_WEST (-(float)M_PI / 2.0f) /* (sin,cos) = (-1,0) -> toward -X */

int main(void) {
	/* ---------------------------------------------------------------
	 * MOVING: forward always ACCELERATES, back always BRAKES, and the
	 * rider's facing is irrelevant.
	 * --------------------------------------------------------------- */
	/* Southbound on an NS rail (heading +Z, speed positive). */
	assert(entity_minecart_resolve_throttle(+1, 0.20f, 0, 1, YAW_SOUTH) == +1);
	assert(entity_minecart_resolve_throttle(-1, 0.20f, 0, 1, YAW_SOUTH) == -1);

	/* NORTHBOUND -- the case that was broken. speed is NEGATIVE along the
	 * +Z heading, so "faster" means MORE negative. Before the fix this
	 * returned +1 and the cart braked to a halt and reversed. */
	assert(entity_minecart_resolve_throttle(+1, -0.20f, 0, 1, YAW_NORTH) == -1
		   && "FORWARD on a northbound cart must ACCELERATE it north");
	assert(entity_minecart_resolve_throttle(-1, -0.20f, 0, 1, YAW_NORTH) == +1
		   && "BACK on a northbound cart must brake it, not speed it up");

	/* Facing must not matter while moving: a rider looking backwards, or
	 * sideways, still gets "forward = faster". This is the anti-flap
	 * property -- all four of these are the same moving cart. */
	assert(entity_minecart_resolve_throttle(+1, -0.20f, 0, 1, YAW_SOUTH) == -1);
	assert(entity_minecart_resolve_throttle(+1, -0.20f, 0, 1, YAW_EAST) == -1);
	assert(entity_minecart_resolve_throttle(+1, -0.20f, 0, 1, YAW_WEST) == -1);
	assert(entity_minecart_resolve_throttle(+1, -0.20f, 0, 1, YAW_NORTH) == -1);

	/* Same on the EW axis (heading +X). */
	assert(entity_minecart_resolve_throttle(+1, -0.15f, 1, 0, YAW_EAST) == -1);
	assert(entity_minecart_resolve_throttle(+1, +0.15f, 1, 0, YAW_WEST) == +1);

	/* ---------------------------------------------------------------
	 * AT REST: facing decides which way you set off.
	 * --------------------------------------------------------------- */
	assert(entity_minecart_resolve_throttle(+1, 0.0f, 0, 1, YAW_SOUTH) == +1
		   && "facing +Z at rest, forward must set off toward +Z");
	assert(entity_minecart_resolve_throttle(+1, 0.0f, 0, 1, YAW_NORTH) == -1
		   && "facing -Z at rest, forward must set off toward -Z");
	assert(entity_minecart_resolve_throttle(-1, 0.0f, 0, 1, YAW_NORTH) == +1
		   && "back is the mirror of forward at rest");
	assert(entity_minecart_resolve_throttle(+1, 0.0f, 1, 0, YAW_EAST) == +1);
	assert(entity_minecart_resolve_throttle(+1, 0.0f, 1, 0, YAW_WEST) == -1);

	/* Speeds inside the rest threshold count as at rest, so a cart creeping
	 * to a stop does not hand out a direction from numerical noise. */
	assert(entity_minecart_resolve_throttle(+1, 0.0005f, 0, 1, YAW_NORTH) == -1
		   && "below the rest threshold, facing still decides");
	assert(entity_minecart_resolve_throttle(+1, -0.0005f, 0, 1, YAW_SOUTH) == +1);

	/* ---------------------------------------------------------------
	 * DEGENERATE INPUTS must be total, never undefined.
	 * --------------------------------------------------------------- */
	assert(entity_minecart_resolve_throttle(0, 0.20f, 0, 1, YAW_SOUTH) == 0
		   && "no intent must produce no step");
	/* Heading not seeded yet (a cart before its very first on-rail tick):
	 * resolve to a definite +1 rather than flapping. NOTE (audit F3): this
	 * fallback is one frame deep AT MOST -- the first on-rail tick seeds a
	 * heading EVEN AT REST (the seed was moved above the spd==0 early-out;
	 * test_minecart_authority (0b) drives that full production sequence).
	 * Before that fix this comment's claim was false and this very case was
	 * the one-way standstill start. */
	assert(entity_minecart_resolve_throttle(+1, 0.0f, 0, 0, YAW_NORTH) == +1);
	/* Looking exactly ACROSS the rail: dot == 0, also a definite +1. */
	assert(entity_minecart_resolve_throttle(+1, 0.0f, 0, 1, YAW_EAST) == +1);
	/* The result is always a unit step -- never 0 for a real intent, and
	 * never a magnitude the apply point would scale into a jump. */
	const float speeds[] = {-0.3f, -0.05f, 0.0f, 0.05f, 0.3f};
	const float yaws[] = {YAW_NORTH, YAW_SOUTH, YAW_EAST, YAW_WEST};
	for(size_t si = 0; si < sizeof(speeds) / sizeof(speeds[0]); si++)
		for(size_t yi = 0; yi < sizeof(yaws) / sizeof(yaws[0]); yi++)
			for(int intent = -1; intent <= 1; intent += 2) {
				int8_t r = entity_minecart_resolve_throttle(
					(int8_t)intent, speeds[si], 0, 1, yaws[yi]);
				assert((r == 1 || r == -1) && "must always be a unit step");
			}

	printf("test_minecart_throttle_sign: rider intent maps to the cart axis "
		   "predictably in both directions OK\n");
	return 0;
}
