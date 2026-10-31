/* ASTRA-SEED-POLICY-0925: the test seed policy (source/util.h).
 *   - no CW_TEST_SEED: rand_gen_seed == RAND_TEST_SEED_DEFAULT (deterministic);
 *   - CW_TEST_SEED=<1..0xFFFFFFFF, decimal or 0x hex>: that exact seed;
 *   - malformed / zero / out of range / signed / blank-led / octal-looking
 *     hex garbage: the harness aborts (never a silent fallback, never zero-lock);
 *   - a pinned scenario seed ignores CW_TEST_SEED and moves only under
 *     CW_TEST_SEED_FORCE (same acceptance rules).
 * The rejection cases run in a child process of this same executable (the
 * policy aborts), each with its own environment. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifndef CAVEWII_RAND_SEED_TEST
#error "test_rand_seed_policy must compile under the shared test seed boundary"
#endif

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
	char buf[256];
	snprintf(buf, sizeof buf, "%s=%s", name, value ? value : "");
	assert(_putenv(buf) == 0);
#else
	if(value)
		assert(setenv(name, value, 1) == 0);
	else
		assert(unsetenv(name) == 0);
#endif
}

static uint32_t seeded(void) {
	struct random_gen g = {.seed = 0, .has_next_gaussian = true};
	rand_gen_seed(&g);
	assert(!g.has_next_gaussian);
	return g.seed;
}

/* Child: parse the named variable once and exit 0; a rejection aborts. */
static int child(const char* which) {
	if(!strcmp(which, "force"))
		(void)rand_test_scenario_seed(7u);
	else
		(void)rand_test_seed();
	return 0;
}

static int run_child(const char* self, const char* var, const char* value) {
	set_env("CW_TEST_SEED", NULL);
	set_env("CW_TEST_SEED_FORCE", NULL);
	set_env(var, value);
	char cmd[1024];
	snprintf(cmd, sizeof cmd, "\"%s\" --child %s", self,
			 strcmp(var, "CW_TEST_SEED_FORCE") ? "seed" : "force");
#ifdef _WIN32
	/* cmd.exe strips the outer quotes of a quoted command line */
	char wrapped[1100];
	snprintf(wrapped, sizeof wrapped, "\"%s\"", cmd);
	int rc = system(wrapped);
#else
	int rc = system(cmd);
#endif
	set_env(var, NULL);
	return rc;
}

int main(int argc, char** argv) {
	if(argc == 3 && !strcmp(argv[1], "--child"))
		return child(argv[2]);

	set_env("CW_TEST_SEED", NULL);
	set_env("CW_TEST_SEED_FORCE", NULL);
	assert(seeded() == RAND_TEST_SEED_DEFAULT);
	assert(seeded() == RAND_TEST_SEED_DEFAULT); /* stable across calls */
	assert(rand_test_scenario_seed(0x1234u) == 0x1234u);

	set_env("CW_TEST_SEED", "12345");
	assert(seeded() == 12345u);
	assert(rand_test_scenario_seed(0x1234u) == 0x1234u); /* pinned stays authoritative */
	set_env("CW_TEST_SEED", "0xDEADBEEF");
	assert(seeded() == 0xDEADBEEFu);
	set_env("CW_TEST_SEED", "010"); /* decimal ten, never octal eight */
	assert(seeded() == 10u);
	set_env("CW_TEST_SEED", "4294967295");
	assert(seeded() == 0xFFFFFFFFu);
	set_env("CW_TEST_SEED", "2"); /* even is fine: only zero locks xorshift */
	assert(seeded() == 2u);
	set_env("CW_TEST_SEED", NULL);

	set_env("CW_TEST_SEED_FORCE", "0x5");
	assert(rand_test_scenario_seed(0x1234u) == 5u);
	assert(seeded() == RAND_TEST_SEED_DEFAULT); /* FORCE never moves the default */
	set_env("CW_TEST_SEED_FORCE", NULL);
	printf("PASS default/override/pinned seeds\n");

	/* A valid child must exit 0 (proves the child harness itself works). */
	assert(run_child(argv[0], "CW_TEST_SEED", "77") == 0);
	assert(run_child(argv[0], "CW_TEST_SEED_FORCE", "0x77") == 0);
	static const char* const bad[] = {
		"0", "0x0", "00", "-5", "+5", " 5", "5 ", "12abc", "0x", "0xG1",
		"4294967296", "0x100000000", "99999999999999999999999", "abc", "1e3"};
	for(size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		int rc = run_child(argv[0], "CW_TEST_SEED", bad[i]);
		if(rc == 0)
			fprintf(stderr, "RED: CW_TEST_SEED='%s' was accepted\n", bad[i]);
		assert(rc != 0);
		rc = run_child(argv[0], "CW_TEST_SEED_FORCE", bad[i]);
		if(rc == 0)
			fprintf(stderr, "RED: CW_TEST_SEED_FORCE='%s' was accepted\n", bad[i]);
		assert(rc != 0);
	}
	printf("PASS %zu malformed/zero/out-of-range values rejected for both "
		   "variables\n",
		   sizeof bad / sizeof bad[0]);
	return 0;
}
