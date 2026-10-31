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

#include "redstone_repeater.h"

#include "redstone_logic.h" // REDSTONE_WIRE_ID

// Beta field_22023_b = {1,2,3,4}; the scheduled delay is field_22023_b[idx] * 2. Pre-doubled here.
const int redstone_repeater_delay[4] = {2, 4, 6, 8};

// Per facing (meta & 3): the input cell offset and the side arg Beta's ignoreTick passes to
// isBlockIndirectlyProvidingPowerTo. Facing 0/1/2/3 reads from +Z / -X / -Z / +X.
static const int IN_DX[4] = {0, -1, 0, 1};
static const int IN_DZ[4] = {1, 0, -1, 0};
static const int IN_SIDE[4] = {3, 4, 2, 5};

bool redstone_repeater_input_powered(const struct redstone_repeater_world* w, int x, int y, int z) {
	int facing = w->get_meta(w->ctx, x, y, z) & 3;
	int ix = x + IN_DX[facing], iz = z + IN_DZ[facing];
	if(w->is_indirectly_providing_power(w->ctx, ix, y, iz, IN_SIDE[facing]))
		return true;
	// Beta also accepts a directly-adjacent powered wire on the input side (the wire's strong
	// power does not reach a non-cube repeater otherwise).
	return w->get_type(w->ctx, ix, y, iz) == REDSTONE_WIRE_ID
		&& w->get_meta(w->ctx, ix, y, iz) > 0;
}

static int repeater_delay(const struct redstone_repeater_world* w, int x, int y, int z) {
	return redstone_repeater_delay[(w->get_meta(w->ctx, x, y, z) & 12) >> 2];
}

void redstone_repeater_neighbour_change(const struct redstone_repeater_world* w, int x, int y,
										int z) {
	int id = w->get_type(w->ctx, x, y, z);
	bool active = (id == RS_REPEATER_ACTIVE_ID);
	bool input = redstone_repeater_input_powered(w, x, y, z);
	// Beta: schedule the toggle only when the output and input states disagree (active && input
	// off -> will turn off; idle && input on -> will turn on). Both edges go through the delay.
	if((active && !input) || (!active && input))
		w->schedule(w->ctx, x, y, z, id, repeater_delay(w, x, y, z));
}

void redstone_repeater_tick(const struct redstone_repeater_world* w, int x, int y, int z) {
	bool active = (w->get_type(w->ctx, x, y, z) == RS_REPEATER_ACTIVE_ID);
	bool input = redstone_repeater_input_powered(w, x, y, z);
	if(active && !input) {
		w->set_active(w->ctx, x, y, z, false); // active + input gone -> idle
	} else if(!active) {
		w->set_active(w->ctx, x, y, z, true); // idle -> active (completes the pulse)
		if(!input)
			// turned on but the input is already off (a pulse shorter than the delay): re-arm so
			// the active output is held for exactly one delay, then falls -- Beta's faithful behaviour.
			w->schedule(w->ctx, x, y, z, RS_REPEATER_ACTIVE_ID, repeater_delay(w, x, y, z));
	}
}
