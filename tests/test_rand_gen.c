/*
	Regression test for D2 (dispenser-slice adversarial review, 2026-08-13):
	a memset-zero `struct random_gen` used to hang the process the first time
	anything drew a gaussian from it.

	Root cause: xorshift32 zero-locks at seed==0 (every subsequent state stays
	0), so rand_gen_flt(g) == 0.0f forever, so rand_gen_gaussian's rejection
	loop computed v1 = 2*0-1 = -1, v2 = -1, s = v1*v1+v2*v2 = 2.0, which never
	satisfies the `0 < s < 1` accept condition -- an infinite loop with no
	diagnostic. `memset(&s, 0, sizeof(s))` then a SELECTIVE reseed of only
	some fields is this project's own established test idiom (e.g.
	tests/test_dispenser_env.c's server_local fixture), so any test reaching
	a gaussian draw on an unreseeded stream hung CI outright.

	The fix is in rand_gen() itself (util.c): self-heal seed==0 to a fixed
	nonzero constant before stepping. This test proves (a) a zero-seeded
	generator is no longer degenerate for rand_gen()/rand_gen_flt(), (b)
	rand_gen_gaussian() on a zero-seeded generator RETURNS (this test
	completing at all is the hang-regression proof) with the EXACT value an
	explicitly seed=1 generator would produce (self-heal maps seed 0 -> 1,
	so the two streams must be bit-identical from the first draw on), and (c)
	the fix does not perturb any ALREADY-seeded (nonzero) stream's output --
	re-pins the exact golden already used by tests/test_dispenser_env.c's
	generic-item RNG-order test.

	Golden values independently hand-derived (numpy float32 reference model
	of the xorshift32 + polar-method algorithm, run from the algorithm spec,
	not by reading back this file's own C implementation under test):
	  zero-seeded first rand_gen() raw draw: seed becomes 0x00042021 (bit-
	    identical to an explicitly seed=1 generator's first step -- proves
	    the self-heal literally maps 0 -> 1, not some other constant).
	  zero-seeded first gaussian: 0.180894822 (2 rejection-loop iterations),
	    caching 2nd value -0.665715396 for the immediately following call.
	  seed=1, three gaussians drawn immediately (X,Y,Z order, no leading
	    nextDouble-equivalent draw -- test_dispenser_env.c's generic-item
	    eject golden has ONE rand_gen_flt() draw before its three gaussians,
	    a DIFFERENT stream position; this is the plain "gaussian is the very
	    first draw" case): 0.180894822, -0.665715396, 0.226029351.

	Ratification follow-up (ratification-gate rejection, 2026-08-13): the
	four functions above all exercise the D2 zero-seed guard on the
	primitive in isolation; none pinned what a PRE-EXISTING rand_gen_
	gaussian CALLER now draws, now that Box-Muller (fixed 2 uniforms per
	call, no cache) was replaced by Java's polar method (a rejection loop
	producing a pair, one returned, one cached -- see docs/deviations.md's
	"rand_gen_gaussian: Box-Muller -> Java polar method" entry for the full
	faithfulness argument). test_skeleton_arrow_spread_caller below closes
	that gap: it drives the REAL entity_monster.c skeleton-arrow-fire RNG
	sequence (three rand_gen_gaussian() draws, X,Y,Z, no leading draw --
	identical shape to test_nonzero_seed_unaffected above) into the REAL,
	PRODUCTION arrow_apply_spread() (source/entity/combat.h, already an
	accepted "pure math, testable headless" precedent per
	test_arrow_spread.c) using the REAL SKELETON_ARROW_DEVIATION constant --
	the exact combination entity_monster.c's D5 site executes, minus only
	the aim-geometry/entity/world plumbing that surrounds it and is
	orthogonal to which RNG values reach arrow_apply_spread. Deliberately
	NOT a re-derivation of the skeleton fire site's full tick; it is the
	smallest test that would catch a future regression in the shared
	primitive AT a real caller, not just in isolation.
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "entity/combat.h" // arrow_apply_spread, SKELETON_ARROW_DEVIATION --
						   // the real production consumer at the D5 site
#include "util.h"

static bool feq(float a, float b, float tol) { return fabsf(a - b) < tol; }

// (a) A zero-seeded generator is no longer degenerate.
static void test_zero_seed_self_heals(void) {
	struct random_gen g = {0}; // memset-zero idiom: seed=0, no explicit reseed
	assert(g.seed == 0);

	uint32_t r1 = rand_gen(&g);
	assert(r1 != 0);				  // NOT the old zero-locked degenerate case
	assert(r1 == 0x00042021u);		  // == an explicitly seed=1 generator's first step

	struct random_gen g2 = {0};
	float f1 = rand_gen_flt(&g2);
	assert(f1 > 0.0f);				  // NOT stuck at 0.0 forever

	printf("  zero-seeded rand_gen()/_flt() self-heal: first raw=0x%08x "
		  "(== seed=1's first step), flt=%.9f (nonzero) OK\n",
		  r1, f1);
}

// (b) rand_gen_gaussian on a zero-seeded generator RETURNS (this test
// completing at all is the anti-hang proof -- there is no timeout wrapper
// here on purpose: a regression should hang the RUNNER, which ctest reports
// as a timeout, not a silent false-pass) and matches an explicitly seed=1
// generator bit-for-bit (proving the heal target really is 1, not merely
// "some nonzero value").
static void test_zero_seed_gaussian_matches_seed_one(void) {
	struct random_gen gz = {0};
	struct random_gen go = {.seed = 1};

	float vz1 = rand_gen_gaussian(&gz);
	float vo1 = rand_gen_gaussian(&go);
	assert(feq(vz1, vo1, 1.0e-6f));
	assert(feq(vz1, 0.180894822f, 1.0e-4f));

	// Both cached the SAME second value; both return it with no further draw.
	assert(gz.has_next_gaussian && go.has_next_gaussian);
	assert(feq(gz.next_gaussian, go.next_gaussian, 1.0e-6f));
	assert(feq(gz.next_gaussian, -0.665715396f, 1.0e-4f));

	float vz2 = rand_gen_gaussian(&gz);
	float vo2 = rand_gen_gaussian(&go);
	assert(feq(vz2, vo2, 1.0e-6f));
	assert(!gz.has_next_gaussian && !go.has_next_gaussian);

	printf("  zero-seeded gaussian returns (no hang), bit-identical to an "
		  "explicit seed=1 stream: %.9f then cached %.9f OK\n",
		  vz1, vz2);
}

// (c) An already-seeded (nonzero) stream is byte-for-byte unaffected by the
// fix -- re-pins test_dispenser_env.c's generic-item eject golden in
// isolation so a regression here is caught by the cheapest possible test.
static void test_nonzero_seed_unaffected(void) {
	struct random_gen g = {.seed = 1};
	float gx = rand_gen_gaussian(&g);
	float gy = rand_gen_gaussian(&g);
	float gz = rand_gen_gaussian(&g);
	assert(feq(gx, 0.180894822f, 1.0e-4f));
	assert(feq(gy, -0.665715396f, 1.0e-4f));
	assert(feq(gz, 0.226029351f, 1.0e-4f));

	printf("  already-seeded (seed=1) gaussian sequence unperturbed by the "
		  "zero-seed self-heal OK\n");
}

// The rejection loop's bounded-iteration tripwire (assert(++guard < 1000) in
// util.c) must not fire spuriously for ordinary healthy draws across many
// distinct seeds -- prove it stays quiet (no assert abort) for a spread of
// seeds, including edge values.
static void test_guard_does_not_false_trip(void) {
	uint32_t seeds[] = {0, 1, 2, 0xFFFFFFFFu, 0x80000000u, 12345, 999999,
					   0xCAFEBABEu, 0x2545F491u, 0x9E3779B9u};
	for(size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		struct random_gen g = {.seed = seeds[i]};
		for(int k = 0; k < 20; k++) {
			float v = rand_gen_gaussian(&g);
			assert(isfinite(v));
		}
	}
	printf("  gaussian guard: 10 seeds x 20 draws, all finite, no spurious "
		  "trip OK\n");
}

// (d) Caller-level pin (ratification-gate follow-up): the REAL
// entity_monster.c skeleton-arrow-fire draw sequence -- three
// rand_gen_gaussian() calls, X,Y,Z, no leading draw, on a pinned seed=1
// stream -- fed into the REAL, PRODUCTION arrow_apply_spread() with the
// REAL SKELETON_ARROW_DEVIATION constant. This is the exact
// gaussian-primitive-to-consumer pipeline the D5 fix (named locals,
// pinning the evaluation order of these three calls) protects; a future
// regression in rand_gen_gaussian's shape (draw count, cache behavior, or
// value) breaks THIS test even if test_rand_gen's other four functions,
// which only exercise the primitive in isolation, stayed green.
//
// Golden values independently derived from the SAME from-spec float32
// numpy reference model as every other golden in this file (xorshift32 +
// polar-method nextGaussian, run from the algorithm spec) -- gx/gy/gz are
// the identical seed=1 three-immediate-draws values test_nonzero_seed_
// unaffected already pins above; the final dir[] is that same model
// applying arrow_apply_spread's OWN cited formula (dir[i] += g[i] *
// ARROW_SPREAD_PER_DEVIATION(0.0075) * deviation(12.0), i.e. sigma 0.09/
// axis) to dir={1,0,0} -- never by reading back this file's own C
// implementation or combat.h's arrow_apply_spread under test.
static void test_skeleton_arrow_spread_caller(void) {
	struct random_gen g = {.seed = 1};
	// Identical shape to entity_monster.c's D5 site: three named-local
	// draws in X,Y,Z source order, no leading nextDouble-equivalent draw.
	float gx = rand_gen_gaussian(&g);
	float gy = rand_gen_gaussian(&g);
	float gz = rand_gen_gaussian(&g);
	assert(feq(gx, 0.180894822f, 1.0e-4f));
	assert(feq(gy, -0.665715396f, 1.0e-4f));
	assert(feq(gz, 0.226029351f, 1.0e-4f));

	float dir[3] = {1.0f, 0.0f, 0.0f};
	arrow_apply_spread(dir, gx, gy, gz, SKELETON_ARROW_DEVIATION);

	assert(feq(dir[0], 1.016280532f, 1.0e-4f));
	assert(feq(dir[1], -0.059914384f, 1.0e-4f));
	assert(feq(dir[2], 0.020342641f, 1.0e-4f));

	printf("  caller-level pin (entity_monster.c skeleton arrow spread via "
		  "the REAL arrow_apply_spread): dir=(%.9f, %.9f, %.9f) OK\n",
		  dir[0], dir[1], dir[2]);
}

int main(void) {
	printf("test_rand_gen:\n");
	test_zero_seed_self_heals();
	test_zero_seed_gaussian_matches_seed_one();
	test_nonzero_seed_unaffected();
	test_guard_does_not_false_trip();
	test_skeleton_arrow_spread_caller();
	printf("PASS test_rand_gen\n");
	return 0;
}
