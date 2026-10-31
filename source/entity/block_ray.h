#ifndef CAVEWII_BLOCK_RAY_H
#define CAVEWII_BLOCK_RAY_H

#include <stdbool.h>

/* World.func_28105_a's cell facts, independent of world/block registries.
 * Bounds are the one block-local Block.collisionRayTrace box. */
struct block_ray_cell {
	bool present;
	bool collidable;
	bool has_collision_box;
	bool has_ray_box;
	double x1, y1, z1, x2, y2, z2;
};

typedef bool (*block_ray_cell_fn)(int x, int y, int z, bool stop_on_liquid,
								  struct block_ray_cell* out, void* ctx);

struct block_ray_hit {
	int x, y, z;
	int side;
	double hit[3];
};

struct block_ray_stats {
	unsigned cells;
	unsigned box_tests;
};

bool block_ray_collision_trace(const struct block_ray_cell* box,
							   const double from[3], const double to[3],
							   struct block_ray_hit* out);
bool block_ray_trace(const double from[3], const double to[3],
					 bool stop_on_liquid, bool skip_no_collision_box,
					 block_ray_cell_fn cell_fn, void* ctx,
					 struct block_ray_hit* out, struct block_ray_stats* stats);

#endif
