/*
	Regression for the real-Wii distant-tree failure: the surface LOD flattened
	leaves to top faces while its merged terrain skirt stretched an opaque log
	down to the ground. The fixture uses the exact production policy helpers and
	models the height scan/band arithmetic around one deterministic tree.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "chunk_lod_policy.h"
#include "network/far_lod_mode.h"

#define COLUMN_HEIGHT 8

static bool can_see_through(uint8_t type) {
	return type == BLOCK_AIR || type == BLOCK_LEAVES;
}

static int surface_height(const uint8_t column[COLUMN_HEIGHT]) {
	for(int y = COLUMN_HEIGHT - 1; y >= 0; y--)
		if(chunk_lod_skirt_surface_allowed(column[y],
										 can_see_through(column[y])))
			return y;
	return -1;
}

static int skirt_height(int surface, int neighbour_surface) {
	if(neighbour_surface >= surface)
		return 0;
	int ylo = neighbour_surface < 0 ? 0 : neighbour_surface + 1;
	return surface + 1 - ylo;
}

static void test_tree_hull_and_skirt(void) {
	/* Grass y=1, a three-block trunk, and a leaf cap. The tree and its
	 * same-height neighbour have identical terrain surfaces. */
	const uint8_t tree[COLUMN_HEIGHT]
		= {BLOCK_STONE, BLOCK_GRASS, BLOCK_LOG, BLOCK_LOG, BLOCK_LOG,
		   BLOCK_LEAVES, BLOCK_AIR, BLOCK_AIR};
	const uint8_t neighbour[COLUMN_HEIGHT]
		= {BLOCK_STONE, BLOCK_GRASS, BLOCK_AIR, BLOCK_AIR, BLOCK_LEAVES,
		   BLOCK_LEAVES, BLOCK_AIR, BLOCK_AIR};

	/* A surface skin must keep the complete visible tree hull, including the
	 * leaf underside that makes a canopy readable from below/side-on. */
	for(enum side side = SIDE_TOP; side < SIDE_MAX; side++) {
		assert(chunk_lod_face_allowed(true, BLOCK_LOG, side));
		assert(chunk_lod_face_allowed(true, BLOCK_LEAVES, side));
	}
	assert(chunk_lod_face_allowed(true, BLOCK_STONE, SIDE_TOP));
	assert(!chunk_lod_face_allowed(true, BLOCK_STONE, SIDE_BOTTOM));
	assert(!chunk_lod_face_allowed(true, BLOCK_STONE, SIDE_LEFT));

	/* Logs are real block faces, not terrain. The merged skirt sees the grass
	 * under both columns, so it cannot synthesize a log-textured tower. */
	assert(surface_height(tree) == 1);
	assert(surface_height(neighbour) == 1);
	assert(skirt_height(surface_height(tree), surface_height(neighbour)) == 0);

	/* Ordinary stepped terrain still receives its exact merged skirt band. */
	const uint8_t high[COLUMN_HEIGHT]
		= {BLOCK_STONE, BLOCK_DIRT, BLOCK_DIRT, BLOCK_GRASS,
		   BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR};
	const uint8_t low[COLUMN_HEIGHT]
		= {BLOCK_STONE, BLOCK_GRASS, BLOCK_AIR, BLOCK_AIR,
		   BLOCK_AIR, BLOCK_AIR, BLOCK_AIR, BLOCK_AIR};
	assert(surface_height(high) == 3);
	assert(surface_height(low) == 1);
	assert(skirt_height(surface_height(high), surface_height(low)) == 2);
}

static void test_lod0_gate_is_byte_path_equivalent(void) {
	/* The production change is only an early face gate. If lod_skin is false,
	 * every block/side must still enter the unchanged renderer. Exhaust all
	 * byte-sized IDs so no future special case can leak into LOD0. */
	for(unsigned type = 0; type <= UINT8_MAX; type++)
		for(enum side side = SIDE_TOP; side < SIDE_MAX; side++)
			assert(chunk_lod_face_allowed(false, (uint8_t)type, side));
}

static void test_far_mode_validation(void) {
	enum far_lod_mode mode = FAR_LOD_MODE_POPULATED;
	assert(far_lod_mode_parse("trees", &mode) && mode == FAR_LOD_MODE_TREES);
	assert(far_lod_mode_parse("terrain", &mode)
		   && mode == FAR_LOD_MODE_TERRAIN);
	assert(far_lod_mode_parse("populated", &mode)
		   && mode == FAR_LOD_MODE_POPULATED);

	/* The exact bad card value must be rejected, with the safe bald-terrain
	 * fallback rather than silently enabling approximate trunks. */
	assert(!far_lod_mode_parse("skin", &mode) && mode == FAR_LOD_MODE_TERRAIN);
	assert(!far_lod_mode_parse("", &mode) && mode == FAR_LOD_MODE_TERRAIN);
	assert(!far_lod_mode_parse(NULL, &mode) && mode == FAR_LOD_MODE_TERRAIN);
	assert(!far_lod_mode_parse("TREES", &mode) && mode == FAR_LOD_MODE_TERRAIN);
	assert(!far_lod_mode_parse("trees", NULL));
}

int main(void) {
	test_tree_hull_and_skirt();
	test_lod0_gate_is_byte_path_equivalent();
	test_far_mode_validation();
	puts("test_lod_tree_policy: tree hull + no log skirt + terrain skirt + "
		 "LOD0 gate equivalence + strict far mode OK");
	return 0;
}
