#include <assert.h>
#include <string.h>

#include "world.h"

int main(void) {
	struct chunk chunk;
	memset(&chunk, 0, sizeof(chunk));

	uint32_t captured = chunk.dirty_generation;
	assert(chunk_dirty_generation_matches(&chunk, captured));

	chunk_mark_dirty(&chunk);
	assert(!chunk_dirty_generation_matches(&chunk, captured));

	captured = chunk.dirty_generation;
	assert(chunk_dirty_generation_matches(&chunk, captured));

	chunk_mark_dirty(&chunk);
	assert(!chunk_dirty_generation_matches(&chunk, captured));
	return 0;
}
