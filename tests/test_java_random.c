/* Java48 integer/float reference vectors and approximate Gaussian checks.
 * Minecraft Beta 1.7.3 generation depends on this PRNG's exact LCG output.
 * Gaussian tolerances below do not prove bit-exact StrictMath parity. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "worldgen/java_random.h"

/* Retained actual Oracle JRE 1.8.0_491 output, 2026-10-03:
 * rng-java/oracle-attempt1/stdout.json (reviewed in C01-review.md).
 * Every row uses the same mixed operation sequence. Bound 1073741825
 * rejects at seeds 2, 42, 12345 and 2147483648; subsequent values AND
 * post-call states guard cursor continuity. Floats are raw binary32 bits.
 * These literals cover only bounded integers/floats, not Gaussian parity. */
static void test_real_jre_mixed_vectors(void) {
	static const int32_t bounds[12]
		= {3, 0, 1073741825, 0, 20, 800, 16, 0, 2147483647, 1, 400, 0};
	static const struct {
		int64_t seed;
		uint64_t initial_state;
		struct {
			int32_t value_or_float_bits;
			uint64_t state;
		} draws[12];
	} rows[] = {
		{INT64_C(0), UINT64_C(25214903917), {
			{INT32_C(0), UINT64_C(205749139540596)},
			{INT32_C(1062525265), UINT64_C(234029835660143)},
			{INT32_C(516548029), UINT64_C(67704983304270)},
			{INT32_C(1058748784), UINT64_C(170671006977345)},
			{INT32_C(15), UINT64_C(179417057233592)},
			{INT32_C(253), UINT64_C(86990003003491)},
			{INT32_C(8), UINT64_C(154934246374450)},
			{INT32_C(1039114536), UINT64_C(32934430199125)},
			{INT32_C(1283218719), UINT64_C(168194043956028)},
			{INT32_C(0), UINT64_C(219982443966615)},
			{INT32_C(277), UINT64_C(93792642996822)},
			{INT32_C(1048669152), UINT64_C(71150164736681)},
		}},
		{INT64_C(1), UINT64_C(25214903916), {
			{INT32_C(0), UINT64_C(205723924636679)},
			{INT32_C(1036895456), UINT64_C(28280696119558)},
			{INT32_C(880641847), UINT64_C(115427488297881)},
			{INT32_C(1053858802), UINT64_C(114684108877360)},
			{INT32_C(14), UINT64_C(58466533952123)},
			{INT32_C(504), UINT64_C(10199355144298)},
			{INT32_C(5), UINT64_C(93651525288237)},
			{INT32_C(1059629957), UINT64_C(185454638771764)},
			{INT32_C(2078239978), UINT64_C(272399070520367)},
			{INT32_C(0), UINT64_C(200055422738958)},
			{INT32_C(169), UINT64_C(1721836393473)},
			{INT32_C(1042048736), UINT64_C(42991415896696)},
		}},
		{INT64_C(2), UINT64_C(25214903919), {
			{INT32_C(1), UINT64_C(205799569348430)},
			{INT32_C(1050031474), UINT64_C(82578161320001)},
			{INT32_C(8925067), UINT64_C(1169826466659)},
			{INT32_C(1056857990), UINT64_C(139843127085874)},
			{INT32_C(10), UINT64_C(240571298721877)},
			{INT32_C(206), UINT64_C(277499688546876)},
			{INT32_C(0), UINT64_C(9368989764503)},
			{INT32_C(1062956155), UINT64_C(241258967538006)},
			{INT32_C(1982395068), UINT64_C(259836486421929)},
			{INT32_C(0), UINT64_C(277934256203520)},
			{INT32_C(286), UINT64_C(127467662416651)},
			{INT32_C(1047110200), UINT64_C(64220749391290)},
		}},
		{INT64_C(42), UINT64_C(25214903879), {
			{INT32_C(2), UINT64_C(204790973191750)},
			{INT32_C(1029695648), UINT64_C(15386904305625)},
			{INT32_C(102948884), UINT64_C(13493716152507)},
			{INT32_C(1050546296), UINT64_C(86896799908266)},
			{INT32_C(5), UINT64_C(265170128171373)},
			{INT32_C(705), UINT64_C(77990658783604)},
			{INT32_C(11), UINT64_C(199202821746287)},
			{INT32_C(1059742058), UINT64_C(187335379451726)},
			{INT32_C(196118093), UINT64_C(25705590736961)},
			{INT32_C(0), UINT64_C(254276689308088)},
			{INT32_C(302), UINT64_C(127017615659875)},
			{INT32_C(1052561692), UINT64_C(103803162047282)},
		}},
		{INT64_C(12345), UINT64_C(25214899796), {
			{INT32_C(1), UINT64_C(101838520498639)},
			{INT32_C(1057186227), UINT64_C(144455641405230)},
			{INT32_C(80902084), UINT64_C(10603998046482)},
			{INT32_C(1051142100), UINT64_C(91894759549109)},
			{INT32_C(2), UINT64_C(35090963149596)},
			{INT32_C(301), UINT64_C(66294055135991)},
			{INT32_C(0), UINT64_C(9946187593014)},
			{INT32_C(1051901758), UINT64_C(98267236559881)},
			{INT32_C(2121822942), UINT64_C(278111576659424)},
			{INT32_C(0), UINT64_C(126122641363051)},
			{INT32_C(6), UINT64_C(257224973590426)},
			{INT32_C(1059282429), UINT64_C(179624085115037)},
		}},
		{INT64_C(2147483648), UINT64_C(23067420269), {
			{INT32_C(0), UINT64_C(79071226628724)},
			{INT32_C(1062184145), UINT64_C(228306791738223)},
			{INT32_C(903379039), UINT64_C(118407697435969)},
			{INT32_C(1061359433), UINT64_C(214470432819896)},
			{INT32_C(13), UINT64_C(135383547011171)},
			{INT32_C(635), UINT64_C(122487915936818)},
			{INT32_C(4), UINT64_C(86189877185877)},
			{INT32_C(1053484148), UINT64_C(111541277838140)},
			{INT32_C(6755254), UINT64_C(885424779415)},
			{INT32_C(0), UINT64_C(266233432447574)},
			{INT32_C(277), UINT64_C(238170705459881)},
			{INT32_C(1028838432), UINT64_C(14488034283520)},
		}},
		{INT64_C(4294967295), UINT64_C(22029736338), {
			{INT32_C(0), UINT64_C(47864939220789)},
			{INT32_C(1063455266), UINT64_C(249632672389020)},
			{INT32_C(997560331), UINT64_C(130752227784055)},
			{INT32_C(1059194623), UINT64_C(178150942092726)},
			{INT32_C(4), UINT64_C(259610882177673)},
			{INT32_C(550), UINT64_C(230350828356192)},
			{INT32_C(15), UINT64_C(268086281929451)},
			{INT32_C(1047694100), UINT64_C(66669788323866)},
			{INT32_C(545738213), UINT64_C(71530999105309)},
			{INT32_C(0), UINT64_C(50973627569508)},
			{INT32_C(80), UINT64_C(92356644746143)},
			{INT32_C(1048760278), UINT64_C(71914584364222)},
		}},
		{INT64_C(9007199254740993), UINT64_C(25214903916), {
			{INT32_C(0), UINT64_C(205723924636679)},
			{INT32_C(1036895456), UINT64_C(28280696119558)},
			{INT32_C(880641847), UINT64_C(115427488297881)},
			{INT32_C(1053858802), UINT64_C(114684108877360)},
			{INT32_C(14), UINT64_C(58466533952123)},
			{INT32_C(504), UINT64_C(10199355144298)},
			{INT32_C(5), UINT64_C(93651525288237)},
			{INT32_C(1059629957), UINT64_C(185454638771764)},
			{INT32_C(2078239978), UINT64_C(272399070520367)},
			{INT32_C(0), UINT64_C(200055422738958)},
			{INT32_C(169), UINT64_C(1721836393473)},
			{INT32_C(1042048736), UINT64_C(42991415896696)},
		}},
		{INT64_C(9223372036854775807), UINT64_C(281449761806738), {
			{INT32_C(2), UINT64_C(75700622266165)},
			{INT32_C(1054936644), UINT64_C(123725706106780)},
			{INT32_C(26349579), UINT64_C(3453692097911)},
			{INT32_C(1057770495), UINT64_C(154258039025078)},
			{INT32_C(4), UINT64_C(186360214944393)},
			{INT32_C(38), UINT64_C(169108889682528)},
			{INT32_C(6), UINT64_C(117216965722859)},
			{INT32_C(1053585290), UINT64_C(112389715189786)},
			{INT32_C(1774177765), UINT64_C(232545028065053)},
			{INT32_C(0), UINT64_C(15913809532260)},
			{INT32_C(208), UINT64_C(226879315424159)},
			{INT32_C(1050581462), UINT64_C(87191783036094)},
		}},
	};
	for(size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		struct java_random r;
		jrand_set_seed(&r, rows[i].seed);
		assert(r.seed == rows[i].initial_state);
		assert(!r.have_next_gaussian && r.next_gaussian_cached == 0.0);
		for(size_t j = 0; j < sizeof bounds / sizeof bounds[0]; j++) {
			if(bounds[j]) {
				assert(jrand_next_int_bound(&r, bounds[j])
					== rows[i].draws[j].value_or_float_bits);
			} else {
				float value = jrand_next_float(&r);
				uint32_t bits;
				memcpy(&bits, &value, sizeof bits);
				assert(bits == (uint32_t)rows[i].draws[j].value_or_float_bits);
			}
			assert(r.seed == rows[i].draws[j].state);
		}
	}
}

int main(void) {
	test_real_jre_mixed_vectors();
	struct java_random r;

	// new Random(0).nextInt() x5
	jrand_set_seed(&r, 0);
	const int32_t expect_int[5]
		= {-1155484576, -723955400, 1033096058, -1690734402, -1557280266};
	for(int i = 0; i < 5; i++)
		assert(jrand_next_int(&r) == expect_int[i]);

	// new Random(0).nextLong()
	jrand_set_seed(&r, 0);
	assert(jrand_next_long(&r) == (int64_t)-4962768465676381896LL);

	// new Random(0).nextDouble()
	jrand_set_seed(&r, 0);
	assert(fabs(jrand_next_double(&r) - 0.730967787376657) < 1e-12);

	// new Random(0).nextFloat()
	jrand_set_seed(&r, 0);
	assert(fabsf(jrand_next_float(&r) - 0.73096776f) < 1e-6f);

	// nextInt(bound): always in [0, bound) and deterministic for a given seed,
	// across power-of-two and arbitrary bounds.
	for(int32_t bound = 1; bound <= 1000; bound += 7) {
		struct java_random a, b;
		jrand_set_seed(&a, 12345);
		jrand_set_seed(&b, 12345);
		for(int i = 0; i < 50; i++) {
			int32_t va = jrand_next_int_bound(&a, bound);
			int32_t vb = jrand_next_int_bound(&b, bound);
			assert(va >= 0 && va < bound);
			assert(va == vb);
		}
	}

	// Distinct seeds diverge.
	struct java_random ra, rb;
	jrand_set_seed(&ra, 1);
	jrand_set_seed(&rb, 2);
	assert(jrand_next_long(&ra) != jrand_next_long(&rb));

	// nextGaussian(): render-slice snow-drift jitter (RENDER_DISPATCH.md).
	// Expected values come from an independent Python implementation of the
	// specified LCG + polar/Marsaglia algorithm and cache, not a real-JRE
	// Gaussian oracle. The unchanged 1e-9 tolerance is an APPROXIMATE check:
	// C libm sqrt/log need not match Java StrictMath bit-for-bit. Exact
	// Gaussian parity remains unproven; cache/no-draw/reseed controls remain.
	{
		// new Random(0).nextGaussian() x3: fresh, CACHED (no LCG draw), fresh.
		jrand_set_seed(&ra, 0);
		double g1 = jrand_next_gaussian(&ra);
		double g2 = jrand_next_gaussian(&ra);
		double g3 = jrand_next_gaussian(&ra);
		assert(fabs(g1 - 0.8025330637390305) < 1e-9);
		assert(fabs(g2 - (-0.9015460884175122)) < 1e-9);
		assert(fabs(g3 - 2.080920790428163) < 1e-9);

		// The cached draw must NOT advance the LCG: seed a second instance,
		// consume the fresh pair's underlying nextDouble() calls some OTHER
		// way (there isn't one visible from outside), so instead prove it
		// indirectly -- reseed and take a FRESH nextInt() right after the
		// fresh gaussian; it must match a Random that never called
		// nextGaussian a second (cached) time, i.e. calling nextGaussian
		// twice (fresh+cached) then nextInt() must give the SAME nextInt()
		// as calling nextGaussian ONCE (fresh) then nextInt() -- because the
		// cached call draws nothing.
		struct java_random rc, rd;
		jrand_set_seed(&rc, 0);
		(void)jrand_next_gaussian(&rc); // fresh
		(void)jrand_next_gaussian(&rc); // cached -- must not touch the LCG
		int32_t after_cached = jrand_next_int(&rc);

		jrand_set_seed(&rd, 0);
		(void)jrand_next_gaussian(&rd); // fresh (same as rc's first draw)
		int32_t after_fresh_only = jrand_next_int(&rd);
		assert(after_cached == after_fresh_only);

		// new Random(42).nextGaussian() x2: fresh, cached.
		jrand_set_seed(&ra, 42);
		g1 = jrand_next_gaussian(&ra);
		g2 = jrand_next_gaussian(&ra);
		assert(fabs(g1 - 1.141905315473055) < 1e-9);
		assert(fabs(g2 - 0.919407948982788) < 1e-9);

		// new Random(-1).nextGaussian() x2: fresh, cached (negative seed --
		// exercises jrand_set_seed's int64 sign handling too).
		jrand_set_seed(&ra, -1);
		g1 = jrand_next_gaussian(&ra);
		g2 = jrand_next_gaussian(&ra);
		assert(fabs(g1 - 1.7853314409882288) < 1e-9);
		assert(fabs(g2 - (-0.9204169061847902)) < 1e-9);

		// setSeed() clears the gaussian cache: a fresh draw, RESEED, then
		// another fresh draw must equal simply reseeding once and drawing
		// once (i.e. the reseed must not leave the old cache to be handed
		// out instead of a genuinely fresh computation under the new seed).
		jrand_set_seed(&ra, 0);
		(void)jrand_next_gaussian(&ra); // primes the cache under seed 0
		jrand_set_seed(&ra, 42); // MUST clear that cache
		g1 = jrand_next_gaussian(&ra);
		assert(fabs(g1 - 1.141905315473055) < 1e-9); // seed 42's fresh value, not seed 0's leftover cache
	}

	printf("test_java_random: all assertions passed (real-JRE bounded-int/float "
		   "vectors; approximate Gaussian checks)\n");
	return 0;
}
