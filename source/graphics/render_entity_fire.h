#ifndef RENDER_ENTITY_FIRE_H
#define RENDER_ENTITY_FIRE_H

#include <stdbool.h>
#include <stddef.h>

struct entity;
struct camera;

struct entity_fire_vertex {
	float x, y, z;
	float u, v; /* Beta terrain.png UV, before animated-atlas remap. */
};

struct entity_fire_quad {
	struct entity_fire_vertex v[4];
};

/* Pure [Beta 1.7.3 Render.renderEntityOnFire] geometry. A NULL output queries
 * the required count; capacity only limits stored quads, never that count. */
size_t render_entity_fire_build(bool on_fire, float width, float height,
	float pos_y_minus_min_y, float camera_yaw, struct entity_fire_quad* out,
	size_t capacity);

void render_entity_fire_draw(const struct entity* e, const struct camera* c,
	float tick_delta);

#endif
