#ifndef RENDER_PISTON_MOVING_H
#define RENDER_PISTON_MOVING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../block/blocks_data.h"
#include "../block/piston_moving.h"
#include "../cglm/cglm.h"

struct displaylist;
struct piston_moving_client_entry;

struct piston_moving_render_access {
	bool (*get_block)(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* out);
	bool (*get_moving)(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					   struct piston_moving_state* out);
	uint32_t (*get_tint)(void* ctx, w_coord_t x, w_coord_t z,
						 enum biome_tint tint);
	void* ctx;
};

void render_piston_moving_init(void);
void render_piston_moving_all(mat4 view, float partial_tick);

/* Block emitters write section-local W2C_COORD vertices. Add the owning section
 * origin before the fractional TileEntityPiston motion transform. */
void render_piston_moving_model_offset(w_coord_t x, w_coord_t y, w_coord_t z,
									   const float motion[3], float out[3]);

#ifdef CAVEWII_PISTON_RENDER_TEST
/* Emits every branch batch without applying its matrix; returns quads. */
size_t render_piston_moving_test_emit(
	struct displaylist* list, const struct piston_moving_client_entry* entry,
	float partial_tick, const struct piston_moving_render_access* access,
	bool count_only);
#endif

#endif
