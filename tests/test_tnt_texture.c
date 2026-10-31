/*
	Golden for the Beta 1.7.3 TNT face textures. Pins tnt_face_texat
	(source/block/tnt_texture.c) against the decompile (740c583): Block.java:638
	constructs BlockTNT with id 46, texture 8 (blockIndexInTexture=8) and
	BlockTNT.getBlockTextureFromSide returns idx+2 for the bottom (side 0), idx+1
	for the top (side 1), and idx for every side -- so side=8 (TEXAT_TNT_SIDE),
	top=9 (TEXAT_TNT_TOP), bottom=10 (TEXAT_TNT_BOTTOM). Guards the exact regression
	that shipped: the underside returning the fuse-ringed top tile instead of the
	plain crate bottom. Pure (no atlas), links only tnt_texture.c.
*/

#include <assert.h>
#include <stdio.h>

#include "block/blocks_data.h"
#include "block/tnt_texture.h"
#include "graphics/texture_atlas.h"

int main(void) {
	// --- unprimed (metadata 0): three faces -> three DISTINCT Beta tiles ----------
	assert(tnt_face_texat(0, SIDE_TOP) == TEXAT_TNT_TOP);		// idx 9  (+1)
	assert(tnt_face_texat(0, SIDE_BOTTOM) == TEXAT_TNT_BOTTOM);	// idx 10 (+2)
	assert(tnt_face_texat(0, SIDE_LEFT) == TEXAT_TNT_SIDE);		// idx 8
	assert(tnt_face_texat(0, SIDE_RIGHT) == TEXAT_TNT_SIDE);
	assert(tnt_face_texat(0, SIDE_FRONT) == TEXAT_TNT_SIDE);
	assert(tnt_face_texat(0, SIDE_BACK) == TEXAT_TNT_SIDE);

	// The shipped bug: SIDE_BOTTOM must NOT resolve to the same tile as SIDE_TOP.
	assert(tnt_face_texat(0, SIDE_BOTTOM) != tnt_face_texat(0, SIDE_TOP));
	// ...and the three are mutually distinct (no two faces collapse to one tile).
	assert(TEXAT_TNT_SIDE != TEXAT_TNT_TOP);
	assert(TEXAT_TNT_TOP != TEXAT_TNT_BOTTOM);
	assert(TEXAT_TNT_SIDE != TEXAT_TNT_BOTTOM);

	// Metadata must NEVER substitute another terrain tile. Beta's primed object
	// is EntityTNTPrimed: RenderTNTPrimed draws ordinary TNT first, then a
	// texture-disabled white overlay when fuse/5%2==0.
	for(int meta = 1; meta < 16; meta++) {
		assert(tnt_face_texat((uint8_t)meta, SIDE_TOP) == TEXAT_TNT_TOP);
		assert(tnt_face_texat((uint8_t)meta, SIDE_BOTTOM) == TEXAT_TNT_BOTTOM);
		assert(tnt_face_texat((uint8_t)meta, SIDE_LEFT) == TEXAT_TNT_SIDE);
		assert(tnt_face_texat((uint8_t)meta, SIDE_RIGHT) == TEXAT_TNT_SIDE);
		assert(tnt_face_texat((uint8_t)meta, SIDE_FRONT) == TEXAT_TNT_SIDE);
		assert(tnt_face_texat((uint8_t)meta, SIDE_BACK) == TEXAT_TNT_SIDE);
		assert(tnt_face_texat((uint8_t)meta, SIDE_TOP) != TEXAT_SNOW);
	}

	printf("test_tnt_texture: OK (Beta faces side/top/bottom = idx/+1/+2; "
		   "metadata never substitutes snow)\n");
	return 0;
}
