/*
	Copyright (c) 2026 CaveWii contributors

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

#include "window_registry.h"

#include <stddef.h>

#include "death_drop_journal.h"
#include "inventory_logic.h"
#include "tile_entities_nbt.h"

/* Constant bindings, same idiom as the redstone id bindings in
 * redstone_server.c: the registry rows spell these bounds in inventory.h terms
 * so the header stays free of the journal/tile-entity includes. If either pair
 * ever diverges, the mismatch stops the build here instead of silently
 * loosening or tightening a journal validation bound. */
_Static_assert(IRON_CHEST_SIZE_STORAGE
				   == DEATH_DROP_JOURNAL_MAX_CONTAINER_SLOTS,
			   "chest journal_slots bound drift");
_Static_assert(TE_FURNACE_SLOTS == 3, "furnace death/journal slot-count drift");
_Static_assert(TE_DISPENSER_SLOTS == DISPENSER_SIZE_STORAGE,
			   "dispenser death/journal slot-count drift");

#define WINDOW_REGISTRY_ROW(LOGIC, WINDOWC, WIRE, CAP, LINKED, MAIN, HOTBAR,  \
							D_FIRST, D_COUNT, JOURNAL, RECOVERY, KEY)         \
	{                                                                          \
		.logic = &inventory_logic_##LOGIC, .windowc = (WINDOWC),               \
		.wire_type = (WIRE), .capacity = (CAP), .linked = (bool)(LINKED),      \
		.main_slot = (MAIN), .hotbar_slot = (HOTBAR), .death_first = (D_FIRST),\
		.death_count = (D_COUNT), .journal_slots = (JOURNAL),                  \
		.recovery = (RECOVERY), .key = (KEY),                                  \
	},

static const struct window_desc window_registry[] = {
	WINDOW_REGISTRY(WINDOW_REGISTRY_ROW)
};

const struct window_desc* window_registry_find(
	const struct inventory_logic* logic) {
	if(!logic)
		return NULL;
	for(size_t i = 0; i < WINDOW_REGISTRY_COUNT; i++)
		if(window_registry[i].logic == logic)
			return &window_registry[i];
	return NULL;
}

const struct window_desc* window_registry_by_windowc(uint8_t windowc) {
	for(size_t i = 0; i < WINDOW_REGISTRY_COUNT; i++)
		if(window_registry[i].windowc == windowc)
			return &window_registry[i];
	return NULL;
}
