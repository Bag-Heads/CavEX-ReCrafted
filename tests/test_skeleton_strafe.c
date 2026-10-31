/*
	Pins the skeleton keep-distance + circle-strafe steer (audit #17e), binding the
	SHARED source/entity/combat.h so the band constants + the pure skeleton_steer()
	helper here are the exact ones server_tick_monster's skeleton branch uses.

	FAITHFULNESS (stated plainly, per the research, with the magnitude caveat):
	  * The SIDEWAYS / circle STRAFE is FAITHFUL-IN-KIND Beta 1.7.3 -- EntityCreature.
	    updatePlayerActionState re-projects the moveSpeed walk sideways (moveStrafing =
	    -sin(var15)*moveForward) while EntitySkeleton.attackEntity holds hasAttacked
	    within 10 blocks, so a b1.7.3 skeleton strafes rather than walking dead-straight.
	    BUT Beta's strafe magnitude is path-curvature dependent (~straight-in on open
	    ground, var15 ~ 0); v2's constant in-band PURE-TANGENT orbit is a deliberate
	    AMPLIFICATION of that faithful strafe for the bow-only standoff. v2 previously
	    beelined (shared chase) and was MISSING the strafe entirely -- so the tangential
	    term is a faithfulness restoration in KIND.
	  * The min-distance BACK-OFF is a DOCUMENTED Wii-controller PLAYABILITY add (Beta
	    has NO retreat/min-distance): the skeleton is bow-only (melee structurally
	    excluded in entity_monster.c), so steering it OUT keeps it a ranged threat. It is
	    PURE RADIAL AWAY (max back-off) and HAZARD-GATED -- the caller passes
	    allow_backoff=false when the cell behind is lava / a drop, and the steer then
	    orbits in place instead of back-pedalling off a ledge. The back-off is
	    best-effort (Beta-parity move_speed only SLOWS a forward-pressing player, it does
	    not hold a hard standoff). See combat.h #17e + docs/wii_controller_playability.md #15.

	The pure steer is what we pin here (no engine state): too-close + safe -> PURE radial
	AWAY; too-close + hazard -> orbit in place; in-band -> pure tangent; beyond-band ->
	radial TOWARD + tangent; out of follow range -> zeroed + false; unit-length always;
	degenerate same-column -> finite, no NaN.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

// dot of the steer with the unit radial-toward-player (rx,rz). Positive => the steer
// has a component TOWARD the player (closing); negative => AWAY (backing off);
// ~0 => purely perpendicular (orbiting).
static float radial_component(float dx, float dz, float ox, float oz) {
	float len = sqrtf(dx * dx + dz * dz);
	if(len < 1.0e-6f)
		return 0.0f;
	float rx = dx / len, rz = dz / len;
	return ox * rx + oz * rz;
}

static void test_constants(void) {
	// The standoff band must NEST inside the fire range and OUTSIDE melee reach so
	// the skeleton always strafes where it can shoot and never sits in melee.
	assert(feq(SKELETON_STANDOFF_MIN, 4.5f));
	assert(feq(SKELETON_STANDOFF_MAX, 7.0f));
	assert(feq(SKELETON_STANDOFF_MIN_SQ, 20.25f)); // 4.5^2
	assert(feq(SKELETON_STANDOFF_MAX_SQ, 49.0f));  // 7.0^2
	// MIN < MAX gives the hysteresis band (pure-orbit zone between back-off & close-in).
	assert(SKELETON_STANDOFF_MIN_SQ < SKELETON_STANDOFF_MAX_SQ);
	// MAX must stay inside the 10-block fire range (else it backs out of firing range),
	// and with comfortable margin so the outward-drifting orbit still shoots (the steady
	// state rides the MAX boundary): 49 << 100.
	assert(SKELETON_STANDOFF_MAX_SQ < SKELETON_SHOOT_RANGE_SQ);
	// MIN must stay outside the 2-block melee reach (else it sits in the excluded zone).
	assert(SKELETON_STANDOFF_MIN_SQ > MOB_ATTACK_REACH_SQ);
}

// Helper: run skeleton_steer with the canonical band (back-off ALLOWED) and assert it
// returns true + unit.
static bool steer(float dx, float dz, int sign, float* ox, float* oz) {
	float dsq = dx * dx + dz * dz;
	bool r = skeleton_steer(dx, dz, dsq, MOB_FOLLOW_RANGE_SQ,
							SKELETON_STANDOFF_MIN_SQ, SKELETON_STANDOFF_MAX_SQ,
							sign, /*allow_backoff=*/true, ox, oz);
	if(r) {
		// Always a unit heading (the shared clamp relies on direction being unit).
		assert(feq(sqrtf(*ox * *ox + *oz * *oz), 1.0f));
	}
	return r;
}

// Same, but with the back-off SUPPRESSED (hazard behind the mob).
static bool steer_no_backoff(float dx, float dz, int sign, float* ox, float* oz) {
	float dsq = dx * dx + dz * dz;
	bool r = skeleton_steer(dx, dz, dsq, MOB_FOLLOW_RANGE_SQ,
							SKELETON_STANDOFF_MIN_SQ, SKELETON_STANDOFF_MAX_SQ,
							sign, /*allow_backoff=*/false, ox, oz);
	if(r)
		assert(feq(sqrtf(*ox * *ox + *oz * *oz), 1.0f));
	return r;
}

static void test_too_close_backs_off(void) {
	// Player at 3 blocks (dist^2 9 < MIN_SQ 20.25), back-off allowed: the steer must be
	// PURE RADIAL AWAY (max back-off, no tangent) so the retreat carries the full
	// move_speed straight away from the player. Player straight +X of the mob -> -X.
	float ox, oz;
	assert(steer(3.0f, 0.0f, +1, &ox, &oz));
	float rc = radial_component(3.0f, 0.0f, ox, oz);
	assert(feq(rc, -1.0f));     // fully AWAY: pure radial, no perpendicular component
	assert(feq(ox, -1.0f));     // away from +X is -X
	assert(feq(oz, 0.0f));
	// Sign of the (suppressed) tangent must NOT matter when backing off -> both signs
	// give the same pure-away heading.
	float bx, bz;
	assert(steer(3.0f, 0.0f, -1, &bx, &bz));
	assert(feq(ox, bx) && feq(oz, bz));
}

static void test_too_close_hazard_orbits(void) {
	// Same too-close geometry but back-off SUPPRESSED (lava / drop behind the mob): the
	// steer must ORBIT in place (pure tangent), NOT back-pedal into the hazard and NOT
	// close in. Player +X -> tangent is along +/-Z by handedness; zero radial.
	float ox, oz;
	assert(steer_no_backoff(3.0f, 0.0f, +1, &ox, &oz));
	float rc = radial_component(3.0f, 0.0f, ox, oz);
	assert(feq(rc, 0.0f));            // no retreat, no close-in: pure orbit
	assert(feq(fabsf(oz), 1.0f));     // tangent of +X radial is +/-Z
	// Opposite handedness -> opposite tangent (mirror orbit), still no radial.
	float bx, bz;
	assert(steer_no_backoff(3.0f, 0.0f, -1, &bx, &bz));
	assert(feq(ox, -bx) && feq(oz, -bz));
}

static void test_in_band_is_tangential(void) {
	// Player at 6 blocks (dist^2 36, between MIN_SQ 20.25 and MAX_SQ 49): the steer
	// must be PURE TANGENT (orbit) -- ~zero radial component, so it neither closes nor
	// backs off, it circles. This is the faithful-in-kind Beta strafe (amplified).
	float ox, oz;
	assert(steer(6.0f, 0.0f, +1, &ox, &oz));
	float rc = radial_component(6.0f, 0.0f, ox, oz);
	assert(feq(rc, 0.0f));                  // no radial: pure orbit
	assert(feq(fabsf(oz), 1.0f));           // tangent of +X radial is +/-Z
	// The whole heading is perpendicular -> its perpendicular magnitude is ~1.
	float tangmag = sqrtf(fmaxf(0.0f, 1.0f - rc * rc));
	assert(tangmag > 0.99f);
}

static void test_beyond_band_closes_in(void) {
	// Player at 9 blocks (dist^2 81 > MAX_SQ 49 but < follow 256): steer TOWARD to get
	// back into the shooting band, with a tangential arc (radial+tangent blend).
	float ox, oz;
	assert(steer(9.0f, 0.0f, +1, &ox, &oz));
	float rc = radial_component(9.0f, 0.0f, ox, oz);
	assert(rc > 0.0f);          // TOWARD the player (closing)
	// It is a 45-deg blend (unit radial + unit tangent, renormalized), so the radial
	// component is ~cos45 ~ 0.707 -- still arcing, not a dead-straight beeline.
	assert(feq(rc, 0.70710678f));
}

static void test_handedness_is_stable_and_opposite(void) {
	// The two handedness signs orbit OPPOSITE ways: in-band, sign=+1 and sign=-1 give
	// tangents that are negatives of each other (consistent, mirror-imaged circles).
	float ax, az, bx, bz;
	assert(steer(6.0f, 0.0f, +1, &ax, &az));
	assert(steer(6.0f, 0.0f, -1, &bx, &bz));
	assert(feq(ax, -bx) && feq(az, -bz));
	// And the choice is deterministic: same inputs -> same output (no RNG/jitter).
	float cx, cz;
	assert(steer(6.0f, 0.0f, +1, &cx, &cz));
	assert(feq(ax, cx) && feq(az, cz));
}

static void test_out_of_follow_range_wanders(void) {
	// Player past the 16-block follow range (dist^2 289 > 256): zeroed heading +
	// false, so the caller wanders -- same contract as mob_chase_dir.
	float ox = 9.0f, oz = 9.0f;
	float dx = 17.0f, dz = 0.0f;
	bool r = skeleton_steer(dx, dz, dx * dx + dz * dz, MOB_FOLLOW_RANGE_SQ,
							SKELETON_STANDOFF_MIN_SQ, SKELETON_STANDOFF_MAX_SQ,
							+1, /*allow_backoff=*/true, &ox, &oz);
	assert(!r);
	assert(feq(ox, 0.0f) && feq(oz, 0.0f));
}

static void test_degenerate_no_nan(void) {
	// Same-XZ-column (player directly above/on the mob, dist^2 ~0): must fall back to
	// a finite unit vector (no NaN/inf), matching mob_chase_dir's +X fallback. At dist
	// ~0 the band classifies it as "too close" -> (back-off allowed) pure away from +X
	// is -X.
	float ox, oz;
	bool r = skeleton_steer(0.0f, 0.0f, 0.0f, MOB_FOLLOW_RANGE_SQ,
							SKELETON_STANDOFF_MIN_SQ, SKELETON_STANDOFF_MAX_SQ,
							+1, /*allow_backoff=*/true, &ox, &oz);
	assert(r);
	assert(!isnan(ox) && !isnan(oz));
	assert(isfinite(ox) && isfinite(oz));
	assert(feq(sqrtf(ox * ox + oz * oz), 1.0f)); // still a unit vector
	// And the hazard-suppressed degenerate case is also finite + unit (orbit fallback).
	float hx, hz;
	bool hr = skeleton_steer(0.0f, 0.0f, 0.0f, MOB_FOLLOW_RANGE_SQ,
							 SKELETON_STANDOFF_MIN_SQ, SKELETON_STANDOFF_MAX_SQ,
							 +1, /*allow_backoff=*/false, &hx, &hz);
	assert(hr && isfinite(hx) && isfinite(hz));
	assert(feq(sqrtf(hx * hx + hz * hz), 1.0f));
}

static void test_diagonal_player_unit(void) {
	// A diagonal player (not axis-aligned) still yields a unit heading and the right
	// band sign. Player up-and-right at ~4.24 blocks (3,3) => dist^2 18 < MIN_SQ: pure
	// away (radial component -1).
	float ox, oz;
	assert(steer(3.0f, 3.0f, +1, &ox, &oz));
	assert(feq(radial_component(3.0f, 3.0f, ox, oz), -1.0f));
	// And an in-band diagonal: (4,4) => dist^2 32, in [20.25,49]: pure tangent (orbit).
	assert(steer(4.0f, 4.0f, -1, &ox, &oz));
	assert(feq(radial_component(4.0f, 4.0f, ox, oz), 0.0f));
	// And a beyond-band diagonal: (6,6) => dist^2 72 > 49: arcs TOWARD (radial > 0).
	assert(steer(6.0f, 6.0f, +1, &ox, &oz));
	assert(radial_component(6.0f, 6.0f, ox, oz) > 0.0f);
}

static void test_boundaries_classify_as_orbit(void) {
	// EXACT boundary distances must classify as in-band orbit (zero radial), not as
	// back-off/close-in: the steer uses `< standoff_in_sq` and `> standoff_out_sq`, so
	// dist^2 == MIN_SQ and == MAX_SQ are both the (inclusive) orbit zone -- no chatter.
	float ox, oz;
	// dist^2 exactly MIN_SQ (20.25): NOT < MIN_SQ -> orbit, not back-off.
	float dmin = SKELETON_STANDOFF_MIN; // 4.5 -> dist^2 = MIN_SQ
	assert(steer(dmin, 0.0f, +1, &ox, &oz));
	assert(feq(radial_component(dmin, 0.0f, ox, oz), 0.0f));
	// dist^2 exactly MAX_SQ (49): NOT > MAX_SQ -> orbit, not close-in.
	float dmax = SKELETON_STANDOFF_MAX; // 7.0 -> dist^2 = MAX_SQ
	assert(steer(dmax, 0.0f, +1, &ox, &oz));
	assert(feq(radial_component(dmax, 0.0f, ox, oz), 0.0f));
}

int main(void) {
	test_constants();
	test_too_close_backs_off();
	test_too_close_hazard_orbits();
	test_in_band_is_tangential();
	test_beyond_band_closes_in();
	test_handedness_is_stable_and_opposite();
	test_out_of_follow_range_wanders();
	test_degenerate_no_nan();
	test_diagonal_player_unit();
	test_boundaries_classify_as_orbit();
	printf("test_skeleton_strafe: keep-distance band pinned (back off <%.1f [pure away, "
		   "hazard-gated], orbit %.1f..%.1f, close in >%.1f; tangent strafe faithful-in-"
		   "kind, back-off documented Wii-playability)\n",
		   SKELETON_STANDOFF_MIN, SKELETON_STANDOFF_MIN, SKELETON_STANDOFF_MAX,
		   SKELETON_STANDOFF_MAX);
	return 0;
}
