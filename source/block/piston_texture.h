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

#ifndef PISTON_TEXTURE_H
#define PISTON_TEXTURE_H

#include "../graphics/texture_atlas.h" // enum tex_atlas_entry
#include "blocks_data.h"			   // enum side

// Beta BlockPistonBase.getBlockTextureFromSideAndMetadata + BlockPistonExtension: which piston atlas
// tile a rendered face shows, by block id (base 33 / sticky 29 / head 34) + metadata (facing bits 0-2,
// powered/sticky bit 3). PURE -- returns the TEXAT_ enum, no atlas lookup -- so the faithful directional
// selection is golden-testable; block_piston.c's getTextureIndex wraps it with tex_atlas_lookup. The
// extended-arm sub-cube geometry + per-face UV rotation are deferred render polish (a sanctioned
// render-layer simplification); this pins the TEXTURE selection, which is the visible faithfulness.
enum tex_atlas_entry piston_face_texat(int block_type, int meta, enum side side);

#endif
