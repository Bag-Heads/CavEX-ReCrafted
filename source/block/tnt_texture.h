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

#ifndef TNT_TEXTURE_H
#define TNT_TEXTURE_H

#include <stdint.h>

#include "../graphics/texture_atlas.h" // enum tex_atlas_entry
#include "blocks_data.h"			   // enum side

// Beta BlockTNT.getBlockTextureFromSide (Block.java:638 -> new BlockTNT(46, 8),
// blockIndexInTexture=8, decompile 740c583): sides = idx (TEXAT_TNT_SIDE), top = idx+1
// (TEXAT_TNT_TOP), bottom = idx+2 (TEXAT_TNT_BOTTOM) -- the underside is the PLAIN crate
// bottom, distinct from the fuse-ringed top. Metadata never changes the base tile:
// RenderTNTPrimed draws these same faces plus a separate white alpha overlay; it never
// substitutes snow. PURE --
// returns the TEXAT_ enum, no atlas lookup -- so the face selection is golden-testable;
// block_tnt.c's getTextureIndex wraps it with tex_atlas_lookup.
enum tex_atlas_entry tnt_face_texat(uint8_t metadata, enum side side);

#endif
