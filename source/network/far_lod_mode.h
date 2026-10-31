#ifndef FAR_LOD_MODE_H
#define FAR_LOD_MODE_H

#include <stdbool.h>
#include <string.h>

enum far_lod_mode {
	FAR_LOD_MODE_TERRAIN,
	FAR_LOD_MODE_TREES,
	FAR_LOD_MODE_POPULATED,
};

/* Unknown input deliberately falls back to terrain: it preserves the compact
 * far surface without silently enabling the approximate trunk overlay. */
static inline bool far_lod_mode_parse(const char* value,
									  enum far_lod_mode* out) {
	if(!out)
		return false;
	if(value && strcmp(value, "trees") == 0) {
		*out = FAR_LOD_MODE_TREES;
		return true;
	}
	if(value && strcmp(value, "terrain") == 0) {
		*out = FAR_LOD_MODE_TERRAIN;
		return true;
	}
	if(value && strcmp(value, "populated") == 0) {
		*out = FAR_LOD_MODE_POPULATED;
		return true;
	}
	*out = FAR_LOD_MODE_TERRAIN;
	return false;
}

#endif
