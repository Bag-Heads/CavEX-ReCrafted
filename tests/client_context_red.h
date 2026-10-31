/* Shared harness for the split-dims B1 client world-context contract tests
 * (b1-prep-0925).
 *
 * Each test pins one ratified contract (ASTRA-B1-RULINGS-0925) against
 * source/game/client_world_context.c. Exit codes are plain ctest semantics:
 *
 *   exit 0  every precondition held and every CONTRACT assert passed
 *   exit 1  every precondition held AND at least one CONTRACT assert failed
 *   exit 2  a precondition broke (UNEXPECTED: the foundation regressed or the
 *           scenario is wrong -- see the stderr line)
 *
 * The tests were authored RED (registered WILL_FAIL TRUE, repo convention cf.
 * test_window_registry_unregistered): that shape made ctest report Passed
 * ONLY when a test failed for its stated reason, so a broken PRE had to exit
 * 0 to register as a ctest failure. All five were promoted on 2026-09-25
 * (WILL_FAIL removed): a broken PRE now exits 2 so it can never read as a
 * pass. The file keeps its name so the git history of each contract stays
 * contiguous.
 *
 * PRE(x) is the green scaffolding the scenario relies on; CONTRACT(x, why) is
 * the behaviour under test. Neither aborts the run early except a broken PRE. */
#ifndef CLIENT_CONTEXT_RED_H
#define CLIENT_CONTEXT_RED_H

#include "../source/game/client_world_context.h"

#include <stdio.h>

static int b1_contract_failures;

#define PRE(expr) do { if(!(expr)) { \
	fprintf(stderr, "UNEXPECTED precondition %s:%d: %s\n", \
			__FILE__, __LINE__, #expr); \
	return 2; \
} } while(0)

#define CONTRACT(expr, why) do { if(!(expr)) { \
	fprintf(stderr, "B1 CONTRACT %s:%d: %s -- %s\n", \
			__FILE__, __LINE__, #expr, why); \
	b1_contract_failures++; \
} } while(0)

static int b1_contract_finish(const char* name) {
	if(b1_contract_failures > 0) {
		printf("%s: %d B1 contract assert(s) FAILED\n", name,
			   b1_contract_failures);
		return 1;
	}
	printf("PASS %s: every B1 contract assert holds\n", name);
	return 0;
}

#endif
