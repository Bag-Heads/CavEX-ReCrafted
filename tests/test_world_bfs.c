#include <assert.h>

#include "world_visibility.h"

struct bfs_test_chunk {
	int y;
	int steps;
};

int main(void) {
	struct bfs_test_chunk chain[6] = {{.y = 16, .steps = 0},
									  {.y = 16, .steps = 0},
									  {.y = 16, .steps = 0},
									  {.y = 16, .steps = 0},
									  {.y = 16, .steps = 0},
									  {.y = 16, .steps = 0}};

	for(int i = 1; i < 6; i++) {
		chain[i].steps
			= world_visibility_bfs_next_steps(chain[i - 1].steps, chain[i].y);
	}

	assert(chain[5].steps == 5);
	assert(chain[5].steps != 1);
	return 0;
}
