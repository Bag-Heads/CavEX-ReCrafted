/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "piston_texture.h"

#include "redstone_piston.h" // RS_PISTON_HEAD_ID / _STICKY_ID / _BASE_ID

// CaveWii enum side -> Beta side numbering (0=-Y,1=+Y,2=-Z,3=+Z,4=-X,5=+X). CaveWii sides:
// TOP=+Y, BOTTOM=-Y, LEFT=-X, RIGHT=+X, FRONT=-Z, BACK=+Z (blocks_side_offset).
static const int PISTON_BETA_SIDE[SIDE_MAX] = {
	[SIDE_TOP] = 1, [SIDE_BOTTOM] = 0, [SIDE_LEFT] = 4, [SIDE_RIGHT] = 5, [SIDE_FRONT] = 2, [SIDE_BACK] = 3};
// Beta PistonBlockTextures.faceToSide -- the opposite face of a given facing.
static const int PISTON_FACE_TO_SIDE[6] = {1, 0, 3, 2, 5, 4};

enum tex_atlas_entry piston_face_texat(int block_type, int meta, enum side side) {
	int bs = (side >= 0 && side < SIDE_MAX) ? PISTON_BETA_SIDE[side] : 0;
	meta &= 0x0F;
	int facing = meta & 7;

	if(block_type == RS_PISTON_HEAD_ID) {
		// Beta BlockPistonExtension.getBlockTextureFromSideAndMetadata: front (var1==dir) = plate
		// (blockIndexInTexture 107, or 106 when bit3 set), BACK (var1==faceToSide[dir]) = 107 (the head
		// plate tile), the rest = 108 (side). The back face is the connecting-rod plate, NOT the side.
		if(bs == facing)
			return (meta & 8) ? TEXAT_PISTON_TOP_STICKY : TEXAT_PISTON_TOP;
		if(facing < 6 && bs == PISTON_FACE_TO_SIDE[facing]) // back face -> 107 (TEXAT_PISTON_TOP)
			return TEXAT_PISTON_TOP;
		return TEXAT_PISTON_SIDE;
	}

	// base (33) / sticky base (29): front = plate (retracted) or inner (extended/powered).
	bool powered = (meta & 8) != 0;
	bool sticky = (block_type == RS_PISTON_STICKY_ID);
	if(bs == facing)
		return powered ? TEXAT_PISTON_INNER : (sticky ? TEXAT_PISTON_TOP_STICKY : TEXAT_PISTON_TOP);
	if(facing < 6 && bs == PISTON_FACE_TO_SIDE[facing]) // the back face (guard an invalid facing 6/7)
		return TEXAT_PISTON_BACK;
	return TEXAT_PISTON_SIDE;
}
