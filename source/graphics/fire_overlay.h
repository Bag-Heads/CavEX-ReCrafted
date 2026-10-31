/* Beta 1.7.3 ItemRenderer first-person fire geometry over anim.png's two baked
 * 28-frame flame strips (flame_fx_pair: quad 0 = fire, quad 1 = fire+16). */
#ifndef GRAPHICS_FIRE_OVERLAY_H
#define GRAPHICS_FIRE_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "flame_fx.h"

#define FIRE_OVERLAY_ALPHA 230 /* round(0.9 * 255) */

struct fire_overlay_quad {
	float x_offset;
	float rotation_y_deg;
	float uv[8];
};

static inline bool fire_overlay_visible(bool burning, bool sleeping,
											  bool rendering_world) {
	return burning && !sleeping && rendering_world;
}

/* Content origin (normalized) of `frame` of anim.png strip `cell` (0 = fire,
 * 1 = fire+16; a pack without fire+16 had cols 3/10 copied in at load): column FLAME_FX_ANIM_COL<cell> for frames 0..13, that + 7 for
 * 14..27; 18px cell pitch with the 16px content at +3 (world.c matrix_anim). */
static inline void fire_anim_cell_origin(int frame, int cell, float* u0,
										 float* v0) {
	int column = flame_fx_anim_col(cell) + (frame / 14) * 7;
	*u0 = (column * 18 + 3) / 256.0f;
	*v0 = ((frame % 14) * 18 + 3) / 256.0f;
}

/* Match world.c's anim.png scroll: 7 frames/800 ms, 14 rows per column,
 * then the second flame column seven atlas cells to the right. Both quads use
 * the same frame; quad `side` samples strip flame_fx_overlay_cell(side). */
static inline int fire_overlay_frame_ms(int64_t elapsed_ms) {
	if(elapsed_ms < 0) elapsed_ms = 0;
	return (int)(elapsed_ms * 7 / 800 % 28);
}

static inline struct fire_overlay_quad fire_overlay_quad(int frame, int side) {
	float u0, v0;
	fire_anim_cell_origin(frame, flame_fx_overlay_cell(side), &u0, &v0);
	float u1 = u0 + 15.99f / 256.0f;
	float v1 = v0 + 15.99f / 256.0f;
	struct fire_overlay_quad q = {
		.x_offset = side == 0 ? 0.24f : -0.24f,
		.rotation_y_deg = side == 0 ? -10.0f : 10.0f,
		.uv = {u1, v1, u0, v1, u0, v0, u1, v0},
	};
	return q;
}

#endif
