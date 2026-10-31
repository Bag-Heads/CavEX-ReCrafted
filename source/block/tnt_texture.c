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

#include "tnt_texture.h"

enum tex_atlas_entry tnt_face_texat(uint8_t metadata, enum side side) {
	(void)metadata; // primed TNT is an entity; its pulse is a white render overlay

	// Beta BlockTNT.getBlockTextureFromSide (blockIndexInTexture=8): side=8, top=9, bottom=10.
	switch(side) {
		case SIDE_TOP:    return TEXAT_TNT_TOP;	  // Beta idx 9  (blockIndexInTexture + 1)
		case SIDE_BOTTOM: return TEXAT_TNT_BOTTOM; // Beta idx 10 (blockIndexInTexture + 2)
		default:          return TEXAT_TNT_SIDE;   // Beta idx 8  (blockIndexInTexture)
	}
}
