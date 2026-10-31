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

#ifndef WORLDGEN_JAVA_RANDOM_H
#define WORLDGEN_JAVA_RANDOM_H

#include <stdbool.h>
#include <stdint.h>

// Bit-exact reimplementation of java.util.Random. Minecraft Beta 1.7.3 world
// generation is fully determined by this PRNG (and the Perlin noise built on
// it), so matching Java's 48-bit LCG byte-for-byte is REQUIRED to reproduce a
// given seed's terrain faithfully. Verified against known java.util.Random
// output in tests/test_java_random.c.
struct java_random {
	uint64_t seed; // 48-bit state
	// java.util.Random's OWN nextGaussian() cache (haveNextNextGaussian /
	// nextNextGaussian): the polar (Marsaglia) method produces TWO independent
	// gaussians per accepted (v1,v2) pair, returns v1*multiplier immediately and
	// banks v2*multiplier for the very next nextGaussian() call (no fresh LCG
	// draws on that call). setSeed() ALWAYS clears this cache (real Random's
	// setSeed sets haveNextNextGaussian=false unconditionally) -- see
	// jrand_set_seed below. render-slice: EntityRenderer.renderRainSnow calls
	// nextGaussian() TWICE per column with no intervening setSeed, so the second
	// call's cached-vs-fresh behaviour is directly observable render output, not
	// just a perf detail. [740c583: Random.nextGaussian]
	bool have_next_gaussian;
	double next_gaussian_cached;
};

// setSeed(seed): scramble + mask to 48 bits. Also clears the nextGaussian
// cache (matches java.util.Random.setSeed, which unconditionally resets
// haveNextNextGaussian -- a Random reseeded mid-column must not leak a
// gaussian computed under the PREVIOUS seed).
void jrand_set_seed(struct java_random* r, int64_t seed);

// protected next(bits): advance the LCG and return the top `bits` bits as a
// signed 32-bit int (Java's (int) cast of an unsigned shift). 1 <= bits <= 32.
int32_t jrand_next(struct java_random* r, int bits);

int32_t jrand_next_int(struct java_random* r); // nextInt()
int32_t jrand_next_int_bound(struct java_random* r, int32_t bound); // nextInt(n)
int64_t jrand_next_long(struct java_random* r); // nextLong()
float jrand_next_float(struct java_random* r); // nextFloat()
double jrand_next_double(struct java_random* r); // nextDouble()
bool jrand_next_bool(struct java_random* r); // nextBoolean()
// nextGaussian(): Java's polar (Marsaglia) method --
//   do { v1=2*nextDouble()-1; v2=2*nextDouble()-1; s=v1*v1+v2*v2; } while(s>=1||s==0);
//   multiplier = sqrt(-2*log(s)/s); cache v2*multiplier; return v1*multiplier.
// A call that finds the cache primed (see struct java_random above) returns the
// cached value and draws NOTHING new from the LCG. [740c583: Random.nextGaussian]
double jrand_next_gaussian(struct java_random* r);

/* G9X-1 test-pool-only observer (CAVEWII_SERVER_LOCAL_TEST). Disarmed by
 * default; it reads state only and never changes a draw. One event per PUBLIC
 * call. The macros below give every test-pool caller its semantic call site
 * (__FILE__/__LINE__); `caller` is the return address (addr2line context).
 * setSeed events carry has_pre = false: seeding never reads prior state. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
enum jrand_test_api {
	JRAND_TEST_SET_SEED = 1, JRAND_TEST_NEXT_INT, JRAND_TEST_NEXT_INT_BOUND,
	JRAND_TEST_NEXT_LONG, JRAND_TEST_NEXT_FLOAT, JRAND_TEST_NEXT_DOUBLE,
	JRAND_TEST_NEXT_BOOLEAN, JRAND_TEST_NEXT_GAUSSIAN, JRAND_TEST_NEXT_BITS,
};
struct jrand_test_event {
	const struct java_random* rng;
	enum jrand_test_api api;
	int64_t arg;
	bool has_pre;
	uint64_t pre, post, result;
	const char* file;
	int line;
	const void* caller;
};
extern void (*jrand_test_hook)(const struct jrand_test_event*);

void jrand_set_seed_at(struct java_random* r, int64_t seed, const char* file,
	int line);
int32_t jrand_next_at(struct java_random* r, int bits, const char* file,
	int line);
int32_t jrand_next_int_at(struct java_random* r, const char* file, int line);
int32_t jrand_next_int_bound_at(struct java_random* r, int32_t bound,
	const char* file, int line);
int64_t jrand_next_long_at(struct java_random* r, const char* file, int line);
float jrand_next_float_at(struct java_random* r, const char* file, int line);
double jrand_next_double_at(struct java_random* r, const char* file,
	int line);
bool jrand_next_bool_at(struct java_random* r, const char* file, int line);
double jrand_next_gaussian_at(struct java_random* r, const char* file,
	int line);

#define jrand_set_seed(r, seed) jrand_set_seed_at(r, seed, __FILE__, __LINE__)
#define jrand_next(r, bits) jrand_next_at(r, bits, __FILE__, __LINE__)
#define jrand_next_int(r) jrand_next_int_at(r, __FILE__, __LINE__)
#define jrand_next_int_bound(r, bound) \
	jrand_next_int_bound_at(r, bound, __FILE__, __LINE__)
#define jrand_next_long(r) jrand_next_long_at(r, __FILE__, __LINE__)
#define jrand_next_float(r) jrand_next_float_at(r, __FILE__, __LINE__)
#define jrand_next_double(r) jrand_next_double_at(r, __FILE__, __LINE__)
#define jrand_next_bool(r) jrand_next_bool_at(r, __FILE__, __LINE__)
#define jrand_next_gaussian(r) jrand_next_gaussian_at(r, __FILE__, __LINE__)
#endif

#endif
