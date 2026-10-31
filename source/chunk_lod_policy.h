#ifndef CHUNK_LOD_POLICY_H
#define CHUNK_LOD_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "block/blocks_data.h"

/*
	Shared, allocation-free policy for the surface-only chunk mesh. Keeping these
	tiny decisions here lets the deterministic regression fixture exercise the
	exact predicates used by the worker without linking the threaded mesher.
*/
static inline bool chunk_lod_face_allowed(bool lod_skin, uint8_t type,
										 enum side side) {
	/* Terrain stays top-only, while a tree keeps its real outer block hull.
	 * Normal face occlusion still removes log/log and leaf/leaf interiors. */
	return !lod_skin || side == SIDE_TOP || type == BLOCK_LOG
		|| type == BLOCK_LEAVES;
}

static inline bool chunk_lod_skirt_surface_allowed(uint8_t type,
											bool can_see_through) {
	/* Logs are decoration above the terrain surface. Their real faces are
	 * emitted by chunk_lod_face_allowed; a merged skirt would stretch one log
	 * tile from the treetop down to the neighbouring ground. */
	return !can_see_through && type != BLOCK_LOG;
}

#endif
