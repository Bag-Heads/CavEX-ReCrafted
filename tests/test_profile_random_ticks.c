#include <assert.h>

#include "profile.h"

int main(void) {
	assert(FCAVEX_RANDOM_TICKS_PER_CHUNK == EXPECTED_RANDOM_TICKS);
	assert(FCAVEX_MAX_EXPLOSIONS_PER_TICK == EXPECTED_MAX_EXPLOSIONS);
	return 0;
}
