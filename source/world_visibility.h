/*
	Copyright (c) 2026

	This file is part of CavEX.
*/

#ifndef WORLD_VISIBILITY_H
#define WORLD_VISIBILITY_H

static inline int world_visibility_bfs_next_steps(int current_steps,
												  int neighbour_y) {
	return current_steps + ((neighbour_y < 64) ? 1 : 0);
}

#endif
