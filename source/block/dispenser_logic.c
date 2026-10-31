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

#include "dispenser_logic.h"

// [decompile 740c583: BlockDispenser.dispenseItem]
//   from the facing metadata:
//   dx and dz start at 0;
//   facing 3 -> dz = 1,
//   facing 2 -> dz = -1,
//   facing 5 -> dx = 1,
//   any other -> dx = -1.
void dispenser_direction(uint8_t metadata, int* dx, int* dz) {
	int x = 0, z = 0;
	if(metadata == 3)
		z = 1;
	else if(metadata == 2)
		z = -1;
	else if(metadata == 5)
		x = 1;
	else
		x = -1; // the catch-all -- NOT a 4th explicit metadata==4 branch
	if(dx)
		*dx = x;
	if(dz)
		*dz = z;
}

void dispenser_spawn_point(int x, int y, int z, int dx, int dz, float* sx,
						   float* sy, float* sz) {
	if(sx)
		*sx = (float)x + (float)dx * 0.6F + 0.5F;
	if(sy)
		*sy = (float)y + 0.5F;
	if(sz)
		*sz = (float)z + (float)dz * 0.6F + 0.5F;
}

int dispenser_reservoir_select(const bool occupied[DISPENSER_TE_SLOTS],
							   struct random_gen* rng) {
	int sel = -1;
	int n = 1;
	for(int i = 0; i < DISPENSER_TE_SLOTS; i++) {
		if(occupied[i]) {
			if(rand_gen_int(rng, n) == 0)
				sel = i;
			n++;
		}
	}
	return sel;
}

bool dispenser_take_random_stack(struct dispenser_te* d, struct item_data* out,
								 int* out_slot) {
	/* Refuse the scope before even advancing its RNG when a slot is pinned. */
	for(size_t k=0;k<DISPENSER_TE_SLOTS;k++)
		if(item_data_pinned(&d->items[k])) return false;
	bool occupied[DISPENSER_TE_SLOTS];
	for(int i = 0; i < DISPENSER_TE_SLOTS; i++)
		occupied[i] = d->items[i].count > 0;

	int sel = dispenser_reservoir_select(occupied, &d->dispenser_random);
	if(sel < 0)
		return false;

	// Beta decrStackSize(sel, 1): the ejected copy is always exactly 1 unit of
	// the source stack's id/damage; the source slot loses exactly 1 unit and
	// becomes the null ItemStack (id=0) if that empties it.
	if(out) {
		*out = d->items[sel];
		out->count = 1;
	}
	if(out_slot)
		*out_slot = sel;
	d->items[sel].count--;
	if(d->items[sel].count == 0)
		d->items[sel] = (struct item_data) {0};
	return true;
}

void dispenser_restore_stack(struct dispenser_te* d, int slot,
							 struct item_data stack) {
	if(slot < 0 || slot >= DISPENSER_TE_SLOTS)
		return; // defensive: an invalid slot has nothing to restore
	// Exact inverse of the decrStackSize(sel, 1) above: if the take emptied
	// the slot (id/count both cleared), put the taken unit back verbatim; if
	// the slot still holds the remainder of a bigger stack, just give back
	// the one unit.
	if(d->items[slot].count == 0)
		d->items[slot] = stack;
	else
		d->items[slot].count++;
}
