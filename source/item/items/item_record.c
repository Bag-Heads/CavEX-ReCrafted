/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../../block/blocks.h"
#include "../../network/server_jukebox.h"
#include "../../network/server_local.h"

// Beta 1.7.3 ItemRecord.onItemUse (jukebox-slice, decompile 740c583): insert
// the held record into a targeted jukebox that has NO record inserted yet
// (target block == jukebox AND metadata == 0). On success:
// BlockJukeBox.ejectRecord (misnamed -- it INSERTS: sets the TE's record and
// flips metadata to 1) then playAuxSFXAtEntity(null, 1005, x,y,z,
// shiftedIndex) then --stackSize. The dispatch that calls onUseOnBlock hands
// us the REAL held stack (server_local.c's "onUseOnBlock mutates the real
// held stack itself" convention, the item_hoe.c precedent) -- decrementing it
// here matches Beta's `--var1.stackSize` exactly; the caller syncs
// CRPC_INVENTORY_SLOT automatically whenever `used` comes back true.
bool item_record_onUseOnBlock(struct server_local* s, struct server_player* actor, struct item_data* it,
							  struct block_info* on, enum side on_side) {
	if(!on || !on->block || on->block->type != BLOCK_JUKEBOX
	   || on->block->metadata != 0)
		return false;

	if(jukebox_te_register(s, on->x, on->y, on->z) < 0)
		return false;

	server_local_jukebox_insert(s, on->x, on->y, on->z, it->id);
	it->count--;
	return true;
}
