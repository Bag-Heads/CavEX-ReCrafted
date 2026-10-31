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

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

void rand_gen_seed(struct random_gen* g) {
	assert(g);
	/* |1: seed 0 zero-locks the xorshift permanently (audio review F6 -- a
	 * dead/unset Wii RTC returns 0 and every cosmetic roll degenerates to a
	 * constant). Forcing the low bit costs one bit of entropy, never locks. */
	g->seed = RAND_GEN_CLOCK_SEED();
	// Java Random.setSeed clears haveNextNextGaussian -- a reseed must not let a
	// gaussian cached under the OLD stream leak into the new one.
	g->has_next_gaussian = false;
	g->next_gaussian = 0.0f;
}

uint32_t rand_gen(struct random_gen* g) {
	// Self-heal a zero seed HERE, at the root, not in any one caller. Zero
	// permanently zero-locks xorshift32 (every subsequent state stays 0, so
	// every rand_gen_flt/_int/_range draw degenerates to a constant).
	// rand_gen_seed's own `|1u` already keeps PRODUCTION reseeds off zero,
	// but a hand-built `struct random_gen` (memset-zero then a SELECTIVE
	// reseed of only some fields -- an established test idiom in this
	// project, e.g. tests/test_dispenser_env.c's server_local fixture) can
	// reach here with seed==0 on a field nobody explicitly reseeded. A
	// zero-locked generator already produces nothing but zeros, so healing
	// it costs no faithful behaviour for any caller, and fixing it here
	// protects every rand_gen_* consumer at once. This is the ROOT fix for a
	// real regression adversarial review found: rand_gen_gaussian's
	// rejection loop (util.c) computes v1=v2=-1, s=2.0 forever from an
	// all-zero stream and never satisfies its `s < 1` accept condition --
	// an infinite hang with no diagnostic, the old stateless Box-Muller body
	// never had a loop to hang in.
	if(g->seed == 0)
		g->seed = 1u; // any fixed nonzero constant; matches rand_gen_seed's own |1u intent
	g->seed ^= g->seed << 13;
	g->seed ^= g->seed >> 17;
	g->seed ^= g->seed << 5;
	return g->seed;
}

int rand_gen_range(struct random_gen* g, int min, int max) {
	assert(g);
	return rand_gen(g) % (max - min) + min;
}

float rand_gen_flt(struct random_gen* g) {
	assert(g);
	// [0, 1) STRICTLY -- see rand_u32_to_flt (util.h) for why the old
	// (float)x / UINT32_MAX form could return exactly 1.0f and break every
	// `(int)(flt * n)` consumer's 0..n-1 contract.
	return rand_u32_to_flt(rand_gen(g));
}

int rand_gen_int(struct random_gen* g, int max) {
	assert(g);
	if (max <= 0) return 0;
	return (int)(rand_gen_flt(g) * max); // flt < 1.0 -> result in 0..max-1 always
}

// java.util.Random#nextGaussian, transcribed exactly (the polar/Marsaglia
// method, NOT Box-Muller -- the two are equal in DISTRIBUTION but not in
// per-call STREAM CONSUMPTION, and consumption order is what this project's
// RNG-order discipline cares about). Beta's setArrowHeading/setEggHeading/
// setSnowballHeading and BlockDispenser.dispenseItem's generic-item eject all
// call this several times in a row on a SHARED stream, so the real algorithm's
// pairing matters: a fresh call runs a rejection-sampling loop that produces
// TWO standard normals, returns the first, and CACHES the second; the very
// next call returns the cached value and draws NOTHING further from the
// stream. A naive "two uniforms in, one gaussian out" reimplementation (the
// prior Box-Muller body here) silently desyncs any caller drawing an odd
// count -- exactly the dispenser's three-gaussian (X,Y,Z) eject jitter.
// Kept in float (not Java's double) to match every other rand_gen_* primitive
// in this file; the algorithm SHAPE is what must be faithful, not the mantissa
// width (this whole generator is already xorshift32, not Java's 48-bit LCG).
// [java.util.Random#nextGaussian, documented JDK algorithm]
//
// NOT THE SAME THING AS, AND MUST NEVER BE MERGED WITH, source/worldgen/
// java_random.c's jrand_next_gaussian (a separate transcription of this same
// Java method, in double, over struct java_random's bit-exact 48-bit LCG --
// added on a sibling render-slice branch; not present in this worktree's
// java_random.c as of this comment, see docs/deviations.md's "rand_gen_
// gaussian: Box-Muller -> Java's polar method" entry). Worldgen's seed-to-
// terrain mapping is byte-verified against the mc173 oracle and REQUIRES
// jrand_next_gaussian's bit-exact LCG stream; gameplay callers here only
// require this function's DRAW SHAPE (count/order/caching) to match Beta,
// never its bit-for-bit output (this generator is xorshift32 and cannot
// produce Java's stream regardless of algorithm). A future cleanup that
// "de-duplicates" these two nextGaussian transcriptions into one shared
// implementation would silently break worldgen's byte-parity against real
// Beta chunks the moment it picked EITHER single output width/stream over
// the other -- keep them separate on purpose.
float rand_gen_gaussian(struct random_gen* g) {
	assert(g);
	if(g->has_next_gaussian) {
		g->has_next_gaussian = false;
		return g->next_gaussian;
	}

	float v1, v2, s;
#ifndef NDEBUG
	// Fenced with the assert tripwire below that consumes it: -DNDEBUG builds
	// compile the assert out, and GCC >= 16 flags the orphaned variable.
	int guard = 0;
#endif
	do {
		v1 = 2.0f * rand_gen_flt(g) - 1.0f;
		v2 = 2.0f * rand_gen_flt(g) - 1.0f;
		s = v1 * v1 + v2 * v2;
		// Tripwire, not a behavior change: rand_gen()'s zero-seed self-heal
		// above is the actual fix for the hang adversarial review found (an
		// all-zero stream drove v1=v2=-1, s=2.0 forever). This bound exists
		// so that IF some future change reintroduces a stream that can never
		// land inside the unit circle, the test suite gets a fast, located
		// assert instead of an unbounded hang -- a healthy RNG accepts
		// within a handful of iterations (P(accept) = pi/4 per draw; the
		// chance of needing 1000 is astronomically below any real run).
		assert(++guard < 1000);
	} while(s >= 1.0f || s == 0.0f);

	float multiplier = sqrtf(-2.0f * logf(s) / s);
	g->next_gaussian = v2 * multiplier;
	g->has_next_gaussian = true;
	return v1 * multiplier;
}

const char* path_from_sd_root(const char* path, char* buf, size_t buf_len) {
	assert(path && buf);

	// "dev:/..." (sd:/, usb:/, fat:/) or "/..." is already launcher-independent.
	if(strchr(path, ':') || path[0] == '/')
		return path;

	snprintf(buf, buf_len, "sd:/%s", path);
	return buf;
}

void* file_read(const char* name) {
	FILE* f = fopen(name, "rb");

	if(!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(len < 0) { // ftell failure -> (size_t)-1 would request a huge malloc / NULL-deref below
		fclose(f);
		return NULL;
	}
	size_t length = (size_t)len;
	char* res = malloc(length + 1);
	if(!res) { // honour the NULL-return contract rather than writing through NULL
		fclose(f);
		return NULL;
	}
	res[length] = 0;

	// CR-07 (docs/audit_2026-08-20_code_review.md): the old `(void)!fread(...)`
	// discarded the return value entirely, so a short read -- the file shrinking
	// or an I/O error striking between the ftell() above and this read -- silently
	// handed callers a truncated-but-"successful" buffer (res[length] is still 0,
	// so it even looks like a valid, if short, C string). For a caller like the PC
	// shader loader that is a truncated shader source that either fails to compile
	// with a confusing error or, worse, compiles into something silently wrong.
	// fread(ptr, size, nmemb, f) with nmemb==1 returns the number of *complete*
	// items read, i.e. 1 on success or 0 on any short/failed read; guard the
	// call with length>0 because size==0 (a legitimately empty file) is defined
	// to return 0 without that meaning failure. Fail loud (NULL) rather than
	// silently returning the partially-filled buffer.
	if(length > 0 && fread(res, length, 1, f) != 1) {
		fclose(f);
		free(res);
		return NULL;
	}

	fclose(f);
	return res;
}

uint32_t hash_u32(uint32_t x) {
	x = ((x >> 16) ^ x) * 0x45D9F3B;
	x = ((x >> 16) ^ x) * 0x45D9F3B;
	x = (x >> 16) ^ x;
	return x;
}

void hsv2rgb(float* h, float* s, float* v) {
	assert(h && s && v);

	int i = floor((*h) * 6.0F);
	float f = (*h) * 6.0F - i;
	float p = (*v) * (1.0F - *s);
	float q = (*v) * (1.0F - f * (*s));
	float t = (*v) * (1.0F - (1.0F - f) * (*s));

	float r = 0, g = 0, b = 0;
	// i can be negative when *h < 0; a plain `i % 6` then yields a negative index that
	// matches no case, leaving r/g/b uninitialised. Wrap to a positive 0..5 index.
	switch(((i % 6) + 6) % 6) {
		case 0: r = *v, g = t, b = p; break;
		case 1: r = q, g = *v, b = p; break;
		case 2: r = p, g = *v, b = t; break;
		case 3: r = p, g = q, b = *v; break;
		case 4: r = t, g = p, b = *v; break;
		case 5: r = *v, g = p, b = q; break;
	}

	*h = r;
	*s = g;
	*v = b;
}

inline uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (base[idx / 2] >> ((idx % 2) * 4)) & 0xF;
}

inline void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	base[idx / 2] = (base[idx / 2] & ~(0x0F << ((idx % 2) * 4)))
		| (data << ((idx % 2) * 4));
}

#ifdef CAVEWII_RAND_SEED_TEST
#include <ctype.h>
#include <errno.h>
/* Test-only (see util.h). Parsed on each call (no static state, no init-order
 * or thread race); a bad value is a harness error, reported and fatal. */
static uint32_t rand_test_seed_env(const char* name, uint32_t fallback) {
	const char* e = getenv(name);
	if(!e || !*e)
		return fallback;
	char* end = NULL;
	errno = 0;
	/* decimal, or 0x/0X hex -- never octal ("010" is ten, as the matrix tool reads it) */
	int hex = e[0] == '0' && (e[1] == 'x' || e[1] == 'X');
	unsigned long long v = strtoull(hex ? e + 2 : e, &end, hex ? 16 : 10);
	/* digits only: strtoull would also take leading blanks and a sign */
	if(e[0] < '0' || e[0] > '9' || (hex && !isxdigit((unsigned char)e[2])) || errno
	   || end == (hex ? e + 2 : e) || *end || v == 0
	   || v > 0xFFFFFFFFull) {
		fprintf(stderr,
				"%s='%s' rejected: need 1..0xFFFFFFFF (decimal or 0x hex); "
				"0 would zero-lock the xorshift stream\n",
				name, e);
		abort();
	}
	return (uint32_t)v;
}

/* The effective seed of every clock-seeded stream: the fixed default, or
 * CW_TEST_SEED (exploration). */
uint32_t rand_test_seed(void) {
	return rand_test_seed_env("CW_TEST_SEED", RAND_TEST_SEED_DEFAULT);
}

/* A scenario/golden test's own pinned seed stays authoritative: CW_TEST_SEED
 * does NOT move it. Only the explicit CW_TEST_SEED_FORCE replays a retained
 * regression seed through the scenario (tools/test_seed_matrix.py). */
uint32_t rand_test_scenario_seed(uint32_t pinned) {
	return rand_test_seed_env("CW_TEST_SEED_FORCE", pinned);
}
#endif
