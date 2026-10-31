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

#include <math.h>
#include <string.h>

#include "java_random.h"

// java.util.Random constants.
#define JR_MULT 0x5DEECE66DULL
#define JR_ADD 0xBULL
#define JR_MASK ((1ULL << 48) - 1)

static inline void jr_set_seed(struct java_random* r, int64_t seed) {
	r->seed = ((uint64_t)seed ^ JR_MULT) & JR_MASK;
	// java.util.Random.setSeed unconditionally clears the cached gaussian --
	// see the citation on struct java_random (java_random.h).
	r->have_next_gaussian = false;
	r->next_gaussian_cached = 0.0;
}

static inline int32_t jr_next(struct java_random* r, int bits) {
	r->seed = (r->seed * JR_MULT + JR_ADD) & JR_MASK;
	// Unsigned right shift of the 48-bit state, reinterpreted as a signed int
	// (matches Java's `(int)(seed >>> (48 - bits))`).
	return (int32_t)(uint32_t)(r->seed >> (48 - bits));
}

static inline int32_t jr_next_int(struct java_random* r) {
	return jr_next(r, 32);
}

static inline int32_t jr_next_int_bound(struct java_random* r, int32_t bound) {
	// Mirrors java.util.Random.nextInt(int bound) exactly, including the
	// power-of-two fast path and the rejection loop that removes modulo bias.
	if((bound & -bound) == bound) // bound is a power of two
		return (int32_t)(((int64_t)bound * (int64_t)jr_next(r, 31)) >> 31);

	int32_t bits, val;
	do {
		bits = jr_next(r, 31);
		val = bits % bound;
		// Java's `bits - val + (n-1) < 0` rejection test RELIES on 32-bit signed
		// overflow wrapping to negative; signed overflow is UB in C, so do the add in
		// uint32_t (wraps mod 2^32, well-defined) and reinterpret as int32_t for the sign
		// test -- byte-identical to Java's wrapping, UB-free.
	} while((int32_t)((uint32_t)bits - (uint32_t)val + (uint32_t)(bound - 1)) < 0);
	return val;
}

static inline int64_t jr_next_long(struct java_random* r) {
	// ((long)next(32) << 32) + next(32) — the low word is sign-extended, as in
	// Java, so the result can be negative.
	int32_t hi = jr_next(r, 32);
	int32_t lo = jr_next(r, 32);
	// `(long)hi << 32` in Java; hi can be negative and left-shifting a negative signed
	// value is UB in C. Shift in uint64_t (well-defined) then reinterpret -- the high 32
	// bits become hi's bits and the low 32 zero, byte-identical to Java's sign-extended
	// shift (the sign-extension bits are shifted out either way).
	return (int64_t)((uint64_t)hi << 32) + (int64_t)lo;
}

static inline float jr_next_float(struct java_random* r) {
	return (float)jr_next(r, 24) / (float)(1 << 24);
}

static inline double jr_next_double(struct java_random* r) {
	int64_t hi = (int64_t)jr_next(r, 26);
	int64_t lo = (int64_t)jr_next(r, 27);
	return (double)((hi << 27) + lo) / (double)(1LL << 53);
}

static inline bool jr_next_bool(struct java_random* r) {
	return jr_next(r, 1) != 0;
}

static inline double jr_next_gaussian(struct java_random* r) {
	// [740c583: Random.nextGaussian] -- the polar (Marsaglia) method, with
	// Java's own cached-second-value optimization reproduced exactly (see the
	// struct java_random citation in java_random.h): a call that finds the
	// cache primed returns it and draws NOTHING new from the LCG.
	if(r->have_next_gaussian) {
		r->have_next_gaussian = false;
		return r->next_gaussian_cached;
	}

	double v1, v2, s;
	do {
		v1 = 2.0 * jr_next_double(r) - 1.0;
		v2 = 2.0 * jr_next_double(r) - 1.0;
		s = v1 * v1 + v2 * v2;
	} while(s >= 1.0 || s == 0.0);

	double multiplier = sqrt(-2.0 * log(s) / s);
	r->next_gaussian_cached = v2 * multiplier;
	r->have_next_gaussian = true;
	return v1 * multiplier;
}


/* G9X-1: public API. Production builds call the unchanged implementations
 * above. The test pool (CAVEWII_SERVER_LOCAL_TEST) routes every caller through
 * the jrand_*_at() variants (java_random.h macros carry the semantic call
 * site's __FILE__/__LINE__) and may arm a read-only observer that records each
 * PUBLIC call: receiver, API, argument, pre/post 48-bit state, result and site.
 * Internal draws (rejection loops, nextLong's halves, nextGaussian's doubles)
 * stay inside one public call, exactly like java.util.Random's own public
 * methods, so both sides are observed at the same API boundary.
 *
 * setSeed never reads the receiver's previous state: callers legitimately
 * seed an uninitialised stack Random (worldgen.c worldgen_surface), and
 * java.util.Random.setSeed's meaning does not depend on it either. Its event
 * carries has_pre = false and pre = 0. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*jrand_test_hook)(const struct jrand_test_event*);

static void jr_observe(struct java_random* r, enum jrand_test_api api,
	int64_t arg, bool has_pre, uint64_t pre, uint64_t result,
	const char* file, int line, const void* caller) {
	const struct jrand_test_event e = {.rng = r, .api = api, .arg = arg,
		.has_pre = has_pre, .pre = pre, .post = r->seed, .result = result,
		.file = file, .line = line, .caller = caller};
	jrand_test_hook(&e);
}

static inline uint64_t jr_float_bits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

static inline uint64_t jr_double_bits(double d) {
	uint64_t u;
	memcpy(&u, &d, sizeof u);
	return u;
}

/* Observed draw: read the (initialised) pre-state, run the unchanged
 * implementation, report. Disarmed: just the implementation. */
#define JR_OBSERVED(api, arg, type, bits_of, call) \
	do { \
		if(!jrand_test_hook) \
			return call; \
		const uint64_t pre_ = r->seed; \
		const type v_ = call; \
		jr_observe(r, api, arg, true, pre_, bits_of(v_), file, line, \
			__builtin_return_address(0)); \
		return v_; \
	} while(0)
#define JR_U32(v) ((uint64_t)(uint32_t)(v))
#define JR_U64(v) ((uint64_t)(v))

void jrand_set_seed_at(struct java_random* r, int64_t seed, const char* file,
	int line) {
	jr_set_seed(r, seed);
	if(jrand_test_hook)
		jr_observe(r, JRAND_TEST_SET_SEED, seed, false, 0, 0, file, line,
			__builtin_return_address(0));
}

int32_t jrand_next_at(struct java_random* r, int bits, const char* file,
	int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_BITS, bits, int32_t, JR_U32, jr_next(r, bits));
}

int32_t jrand_next_int_at(struct java_random* r, const char* file, int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_INT, 0, int32_t, JR_U32, jr_next_int(r));
}

int32_t jrand_next_int_bound_at(struct java_random* r, int32_t bound,
	const char* file, int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_INT_BOUND, bound, int32_t, JR_U32,
		jr_next_int_bound(r, bound));
}

int64_t jrand_next_long_at(struct java_random* r, const char* file, int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_LONG, 0, int64_t, JR_U64, jr_next_long(r));
}

float jrand_next_float_at(struct java_random* r, const char* file, int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_FLOAT, 0, float, jr_float_bits,
		jr_next_float(r));
}

double jrand_next_double_at(struct java_random* r, const char* file,
	int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_DOUBLE, 0, double, jr_double_bits,
		jr_next_double(r));
}

bool jrand_next_bool_at(struct java_random* r, const char* file, int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_BOOLEAN, 0, bool, JR_U64, jr_next_bool(r));
}

double jrand_next_gaussian_at(struct java_random* r, const char* file,
	int line) {
	JR_OBSERVED(JRAND_TEST_NEXT_GAUSSIAN, 0, double, jr_double_bits,
		jr_next_gaussian(r));
}

/* The plain entry points stay defined (address-takers); in the test pool
 * they are observed with no source site. The parentheses stop the
 * java_random.h site macros from expanding these definitions. */
void (jrand_set_seed)(struct java_random* r, int64_t seed) {
	jrand_set_seed_at(r, seed, NULL, 0);
}

int32_t (jrand_next)(struct java_random* r, int bits) {
	return jrand_next_at(r, bits, NULL, 0);
}

int32_t (jrand_next_int)(struct java_random* r) {
	return jrand_next_int_at(r, NULL, 0);
}

int32_t (jrand_next_int_bound)(struct java_random* r, int32_t bound) {
	return jrand_next_int_bound_at(r, bound, NULL, 0);
}

int64_t (jrand_next_long)(struct java_random* r) {
	return jrand_next_long_at(r, NULL, 0);
}

float (jrand_next_float)(struct java_random* r) {
	return jrand_next_float_at(r, NULL, 0);
}

double (jrand_next_double)(struct java_random* r) {
	return jrand_next_double_at(r, NULL, 0);
}

bool (jrand_next_bool)(struct java_random* r) {
	return jrand_next_bool_at(r, NULL, 0);
}

double (jrand_next_gaussian)(struct java_random* r) {
	return jrand_next_gaussian_at(r, NULL, 0);
}
#else
void jrand_set_seed(struct java_random* r, int64_t seed) {
	jr_set_seed(r, seed);
}

int32_t jrand_next(struct java_random* r, int bits) {
	return jr_next(r, bits);
}

int32_t jrand_next_int(struct java_random* r) {
	return jr_next_int(r);
}

int32_t jrand_next_int_bound(struct java_random* r, int32_t bound) {
	return jr_next_int_bound(r, bound);
}

int64_t jrand_next_long(struct java_random* r) {
	return jr_next_long(r);
}

float jrand_next_float(struct java_random* r) {
	return jr_next_float(r);
}

double jrand_next_double(struct java_random* r) {
	return jr_next_double(r);
}

bool jrand_next_bool(struct java_random* r) {
	return jr_next_bool(r);
}

double jrand_next_gaussian(struct java_random* r) {
	return jr_next_gaussian(r);
}
#endif
