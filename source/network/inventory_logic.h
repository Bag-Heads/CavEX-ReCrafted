/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include "../item/inventory.h"

// One server-owned request; vanilla slotClick return for the post-mutation ACK.
struct item_data inventory_quick_move(struct inventory* inv, struct server_player* player,
	size_t slot, set_inv_slot_t changes);

bool inventory_collect(struct inventory* inv, struct item_data* item,
					   uint8_t* slot_priority, size_t slot_length,
					   set_inv_slot_t changes);

// Player-side hotbar/main writeback shared by every CHEST_SLOT_HOTBAR/_MAIN
// window (block chest, storage minecart). Defined in inventory_chest.c.
// player: the actor whose hotbar/main receives the writeback (pid seam).
bool inventory_chest_player_side_on_destroy(struct inventory* inv,
											struct server_player* player);

extern struct inventory_logic inventory_logic_player;
extern struct inventory_logic inventory_logic_crafting;
extern struct inventory_logic inventory_logic_furnace;
/* C2 tiles (Fable A-H1/A-M1): refresh a furnace / dispenser window's storage
 * slots from its backing TE, recording the changed slots. Called by the
 * SRPC_WINDOW_CLICK handler before `returned` is captured, for both the
 * ordinary click and Quick Move. false = no such window or no backing. */
bool inventory_furnace_refresh_window(struct inventory* inv,
									  set_inv_slot_t changes);
bool inventory_dispenser_refresh_window(struct inventory* inv,
										set_inv_slot_t changes);
extern struct inventory_logic inventory_logic_chest;
extern struct inventory_logic inventory_logic_dispenser;
extern struct inventory_logic inventory_logic_iron_chest;
extern struct inventory_logic inventory_logic_double_chest;
extern struct inventory_logic inventory_logic_sign;
bool inventory_sign_set_char(struct inventory* inv, size_t slot, uint8_t ch,
							 set_inv_slot_t changes);
// Storage minecart's entity-backed chest window (minecart-slice). Defined in
// inventory_minecart_chest.c.
extern struct inventory_logic inventory_logic_minecart_chest;
