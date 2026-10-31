/*
	Golden for the Beta 1.7.3 piston DIRECTIONAL face textures (M4 pistons, phase 4b). Pins
	piston_face_texat (source/block/piston_texture.c) against the decompile (740c583,
	BlockPistonBase.getBlockTextureFromSideAndMetadata + BlockPistonExtension): the front face shows the
	plate (retracted) or the inner tile (extended), the opposite face the back tile, the rest the side
	tile -- for the base, the sticky base, and the head, across facings. Pure (no atlas), links only
	piston_texture.c.

	CaveWii side -> Beta side: TOP=+Y=1, BOTTOM=-Y=0, LEFT=-X=4, RIGHT=+X=5, FRONT=-Z=2, BACK=+Z=3.
*/

#include <assert.h>
#include <stdio.h>

#include "block/blocks_data.h"
#include "block/piston_texture.h"
#include "block/redstone_piston.h"
#include "graphics/texture_atlas.h"

#define BASE RS_PISTON_BASE_ID
#define STKY RS_PISTON_STICKY_ID
#define HEAD RS_PISTON_HEAD_ID

int main(void) {
	// --- regular piston facing +X (meta 5), RETRACTED ----------------------------------------------
	assert(piston_face_texat(BASE, 5, SIDE_RIGHT) == TEXAT_PISTON_TOP);	// +X == facing -> front plate
	assert(piston_face_texat(BASE, 5, SIDE_LEFT) == TEXAT_PISTON_BACK);	// -X == faceToSide[5] -> back
	assert(piston_face_texat(BASE, 5, SIDE_TOP) == TEXAT_PISTON_SIDE);	// the other four -> side
	assert(piston_face_texat(BASE, 5, SIDE_BOTTOM) == TEXAT_PISTON_SIDE);
	assert(piston_face_texat(BASE, 5, SIDE_FRONT) == TEXAT_PISTON_SIDE);
	assert(piston_face_texat(BASE, 5, SIDE_BACK) == TEXAT_PISTON_SIDE);

	// --- regular piston facing +X, EXTENDED/powered (meta 5|8 == 13): the front face -> INNER --------
	assert(piston_face_texat(BASE, 13, SIDE_RIGHT) == TEXAT_PISTON_INNER);
	assert(piston_face_texat(BASE, 13, SIDE_LEFT) == TEXAT_PISTON_BACK); // back unchanged
	assert(piston_face_texat(BASE, 13, SIDE_TOP) == TEXAT_PISTON_SIDE);

	// --- STICKY base facing +X, retracted: front shows the STICKY plate ------------------------------
	assert(piston_face_texat(STKY, 5, SIDE_RIGHT) == TEXAT_PISTON_TOP_STICKY);
	assert(piston_face_texat(STKY, 5, SIDE_LEFT) == TEXAT_PISTON_BACK);
	assert(piston_face_texat(STKY, 13, SIDE_RIGHT) == TEXAT_PISTON_INNER); // extended -> inner (not sticky)

	// --- HEAD (34) facing +X: the plate faces +X; the sticky bit selects the plate tile -------------
	assert(piston_face_texat(HEAD, 5, SIDE_RIGHT) == TEXAT_PISTON_TOP);		   // normal head plate
	assert(piston_face_texat(HEAD, 5, SIDE_TOP) == TEXAT_PISTON_SIDE);		   // head sides
	// Beta BlockPistonExtension: the BACK face (var1==faceToSide[dir]) returns 107 -- the head plate tile
	// (TEXAT_PISTON_TOP), NOT the side tile. -X == faceToSide[5]=4 for a +X-facing head.
	assert(piston_face_texat(HEAD, 5, SIDE_LEFT) == TEXAT_PISTON_TOP);		   // head back -> plate tile
	assert(piston_face_texat(HEAD, 13, SIDE_RIGHT) == TEXAT_PISTON_TOP_STICKY); // sticky head plate

	// --- a different facing: piston DOWN (facing 0 == -Y) -------------------------------------------
	assert(piston_face_texat(BASE, 0, SIDE_BOTTOM) == TEXAT_PISTON_TOP);	 // -Y == facing -> front plate
	assert(piston_face_texat(BASE, 0, SIDE_TOP) == TEXAT_PISTON_BACK);	 // +Y == faceToSide[0]=1 -> back
	assert(piston_face_texat(BASE, 0, SIDE_LEFT) == TEXAT_PISTON_SIDE);

	// --- ITEM-ICON facing (Beta RenderBlocks.renderBlockOnInventory forces metadata=1 for render type
	// 16): the inventory/held piston icon faces UP (+Y == facing 1), so the front plate is the VISIBLE
	// top face -- NOT the hidden bottom it landed on at the buggy metadata 0. This pins the texture-logic
	// half of the block_item default_metadata=1 fix; the data value itself is pinned in
	// test_piston_move_env.c (the only piston test that links the full registry).
	assert(piston_face_texat(BASE, 1, SIDE_TOP) == TEXAT_PISTON_TOP);		// +Y == facing 1 -> front plate (visible)
	assert(piston_face_texat(STKY, 1, SIDE_TOP) == TEXAT_PISTON_TOP_STICKY); // sticky plate -> base != sticky icon
	assert(piston_face_texat(BASE, 1, SIDE_BOTTOM) == TEXAT_PISTON_BACK);	// -Y == faceToSide[1]=0 -> back (hidden)
	assert(piston_face_texat(BASE, 1, SIDE_FRONT) == TEXAT_PISTON_SIDE);		// the four verticals -> side

	printf("test_piston_texture: OK (Beta directional faces -- front plate/inner, back, sides; sticky + "
		   "head; multiple facings; item-icon meta=1 plate-on-top)\n");
	return 0;
}
