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

#ifndef HASH_H
#define HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform/time.h"

struct random_gen {
	uint32_t seed;
	// Java java.util.Random#nextGaussian's cached second value (the polar/
	// Marsaglia method draws TWO gaussians per accepted rejection-sampling
	// loop and returns them one call at a time): the first call of a pair runs
	// the loop and caches the second value; the VERY NEXT call returns that
	// cache and draws NOTHING from the underlying stream. Must be cleared
	// whenever `seed` is reset (rand_gen_seed does this, matching Java
	// Random.setSeed clearing haveNextNextGaussian) -- an odd number of
	// gaussian draws on a shared stream (e.g. the dispenser's X/Y/Z eject
	// jitter) leaves a value cached here for whichever caller draws next.
	bool has_next_gaussian;
	float next_gaussian;
};

// Maps a raw 32-bit draw to a uniform float in [0, 1) -- STRICTLY below 1.
// Uses the top 24 bits over 2^24 so every result is exactly representable
// (max = (2^24-1)/2^24 = 0.99999994). The old (float)x / UINT32_MAX form
// rounded the top ~128 raw values to exactly 1.0f (~3e-8 per draw), which
// made rand_gen_int(g, n) return n -- one past its contract -- and (review
// find, 2026-06-11) let the creeper record roll produce unregistered item id
// 2258. Pure + shared so tests pin the bound on the exact production mapping.
static inline float rand_u32_to_flt(uint32_t x) {
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}

void rand_gen_seed(struct random_gen* g);
uint32_t rand_gen(struct random_gen* g);
int rand_gen_range(struct random_gen* g, int min, int max);
float rand_gen_flt(struct random_gen* g);
int rand_gen_int(struct random_gen* g, int max);
// Standard normal -- Beta's rand.nextGaussian() equivalent. Ported from the
// polar/Marsaglia method (java.util.Random#nextGaussian's real algorithm,
// NOT Box-Muller -- see the citation above rand_gen_gaussian's definition in
// util.c for why the distinction matters here): a rejection-sampling loop
// produces a PAIR of values, returns one, and CACHES the other in `g->
// has_next_gaussian`/`next_gaussian` for the very next call, which then
// consumes nothing further from the stream. rand_gen() self-heals a zero
// seed before stepping (see its definition in util.c) specifically so this
// function's rejection loop can never spin forever on an all-zero stream.
float rand_gen_gaussian(struct random_gen* g);


// Reads the whole file into a NUL-terminated malloc'd buffer (caller frees).
// Returns NULL on any failure -- missing file, ftell/malloc failure, OR a
// short read (CR-07, docs/audit_2026-08-20_code_review.md: a truncated read
// used to be silently handed back as if it had succeeded). Callers MUST
// check for NULL rather than relying on assert() alone, which -DNDEBUG
// strips from release/app builds.
void* file_read(const char* name);

// Anchors a relative path to the Wii SD root: returns `path` unchanged if it
// is already absolute (a "dev:/" device prefix or a leading '/'), otherwise
// writes "sd:/<path>" into buf and returns buf. Pure and platform-agnostic so
// it is unit-testable headless; the PLATFORM_WII gating lives in the callers
// (config_read_path). Rationale: the launch CWD differs by launcher (sd:/ for
// chainloaded/staged runs, sd:/apps/CavEX/ for HBC-direct), so a
// CWD-relative pack path silently resolves differently per launcher -- the
// two-config/two-CWD trap (PROJECT_MEMORY parts 9-11).
const char* path_from_sd_root(const char* path, char* buf, size_t buf_len);

uint32_t hash_u32(uint32_t x);

void hsv2rgb(float* h, float* s, float* v);

uint8_t nibble_read(uint8_t* base, size_t idx);
void nibble_write(uint8_t* base, size_t idx, uint8_t data);

/* ---- Clock seeds and the test seed policy (ASTRA-SEED-POLICY-0925) ----
 * Every clock-derived RNG seed in the engine goes through one of these two
 * macros. Production (no CAVEWII_RAND_SEED_TEST): exactly the previous
 * expressions, token for token -- rand_gen_seed stays time(NULL) | 1 and the
 * other clock seeds stay time(NULL). Under the shared TEST compile boundary
 * (CAVEWII_RAND_SEED_TEST: every cavex_test_* pool, the frontend test pool and
 * every test_ target via the CMakeLists sweep) they return rand_test_seed():
 * a fixed default, or CW_TEST_SEED from the environment (decimal or 0x hex,
 * 1..0xFFFFFFFF; malformed or zero aborts -- no silent fallback, no zero-lock).
 * Explicit seeds a test assigns itself stay authoritative (pinned scenario
 * seeds go through rand_test_scenario_seed so a retained failing seed can
 * still be replayed on purpose). */
#ifdef CAVEWII_RAND_SEED_TEST
#define RAND_TEST_SEED_DEFAULT 0x5EED0C0Du
uint32_t rand_test_seed(void);
/* A scenario test's pinned seed; CW_TEST_SEED_FORCE (same rules) overrides it
 * for explicit regression-seed replay only. */
uint32_t rand_test_scenario_seed(uint32_t pinned);
#define RAND_GEN_CLOCK_SEED() (rand_test_seed())
#define CLOCK_SEED_TIME() ((time_t)rand_test_seed())
#else
#define RAND_GEN_CLOCK_SEED() ((uint32_t)time(NULL) | 1u)
#define CLOCK_SEED_TIME() time(NULL)
#endif

#endif
