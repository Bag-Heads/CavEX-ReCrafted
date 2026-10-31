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

#ifndef WINDOW_REGISTRY_H
#define WINDOW_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "../item/inventory.h"
#include "../item/window_container.h"

/* Single source of truth for container/window registration.
 *
 * Three consecutive parity slices (dispenser D8/D9/D10, minecart-chest) shipped
 * a bug because registering a window type meant editing six independent
 * hardcoded dispatch tables, and missing any one compiled cleanly, passed
 * isolated unit tests, and failed SILENTLY -- differently at each site
 * (CODESTYLE.md S3.3 lists the sites and their symptoms).
 *
 * The rows below now ARE those tables. The dispatch sites read this data:
 *   server_local_open_inventory_impl       logic/capacity/windowc/wire/linked
 *   server_local_open_entity_inventory     same, key == WINDOW_KEY_ENTITY
 *   server_local_project_post_close_inventory   main_slot/hotbar_slot
 *   server_local_plan_death_drops          windowc/death_first/death_count
 *   death_drop_journal.c container_slot_valid   journal_slots
 *   death_drop_recovery.c (3 functions)    recovery
 *   server_local_active_window_id          windowc
 *   server_local_reopen_active_inventory   wire_type
 *
 * Two facts a row cannot carry stay hand-written: the per-container
 * backing-existence probe in server_local_plan_death_drops (it reads the world
 * and the tile-entity dictionaries), and the tile-entity arms in
 * death_drop_recovery.c. Both are guarded -- the _Static_assert at the bottom
 * of this header stops the BUILD when a row is added or removed, and
 * death_drop_recovery.c asserts on WINDOW_RECOVERY_COUNT -- so a new window
 * type can no longer fail at runtime instead.
 *
 * Deliberately NOT in this table: server_local_spill_active_container and
 * server_local_active_container_at. Those are POSITION-keyed and exclude the
 * dispenser for a simulation reason (BlockDispenser's own scatter
 * distribution, review D6) and the minecart chest because it is entity-keyed.
 * Flattening them into a capability flag would erase a real distinction.
 */

enum window_key {
	WINDOW_KEY_NONE = 0, /* the player's own inventory: no backing owner */
	WINDOW_KEY_BLOCK,	 /* addressed by (x,y,z): tile entity / chest row */
	WINDOW_KEY_ENTITY,	 /* addressed by entity id: the storage minecart */
};

/* Which owner crash replay (death_drop_recovery.c) must resolve for a window
 * byte read back off disk. NONE = this window never reaches a journal. */
enum window_recovery {
	WINDOW_RECOVERY_NONE = 0,
	WINDOW_RECOVERY_TE_CHEST,
	WINDOW_RECOVERY_TE_FURNACE,
	WINDOW_RECOVERY_TE_DISPENSER,
};
#define WINDOW_RECOVERY_COUNT 4

/* Test-only injection point: tests/test_window_registry_unregistered.c defines
 * this to add a row WITHOUT touching the guard below, proving that an
 * unregistered window type fails the build rather than shipping. */
#ifndef WINDOW_REGISTRY_EXTRA_ROW
#define WINDOW_REGISTRY_EXTRA_ROW(X)
#endif

/* Columns:
 *   LOGIC        inventory_logic_<LOGIC> -- the key on every live-window site
 *   WINDOWC      gstate.players[0].windows[] routing id; the byte persisted in journals
 *   WIRE         enum window_type sent in CRPC_OPEN_WINDOW/CRPC_REOPEN_WINDOW
 *   CAP          slot count the open path must be handed; 0 = never opened as
 *                a container window (the player's own inventory)
 *   LINKED       1 = opened via server_local_open_linked_inventory (two halves)
 *   MAIN/HOTBAR  window slots mirroring INVENTORY_SLOT_MAIN/_HOTBAR, or -1 when
 *                the window owns no copy of the player's main/hotbar (sign)
 *   D_FIRST      first slot of the storage span server_local_plan_death_drops
 *   D_COUNT      length of that span; 0 = nothing journaled/dropped from here
 *   JOURNAL      owner_slot bound death_drop_journal accepts; 0 = never journaled
 *   RECOVERY     enum window_recovery
 *   KEY          enum window_key
 */
#define WINDOW_REGISTRY(X)                                                     \
	X(player, WINDOWC_INVENTORY, WINDOW_TYPE_INVENTORY, 0, 0, -1, -1, 0, 0, 0,  \
	  WINDOW_RECOVERY_NONE, WINDOW_KEY_NONE)                                   \
	X(crafting, WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE, 0,     \
	  CRAFTING_SLOT_MAIN, CRAFTING_SLOT_HOTBAR, 0, 0, 0, WINDOW_RECOVERY_NONE, \
	  WINDOW_KEY_BLOCK)                                                        \
	/* JOURNAL is IRON_CHEST_SIZE_STORAGE (54) for BOTH chest sizes, matching   \
	 * DEATH_DROP_JOURNAL_MAX_CONTAINER_SLOTS -- the pre-refactor bound. It is  \
	 * deliberately looser than the single chest's own 27; window_registry.c     \
	 * _Static_asserts the two constants stay equal. */                         \
	X(chest, WINDOWC_CHEST, WINDOW_TYPE_CHEST, CHEST_SIZE, 0, CHEST_SLOT_MAIN, \
	  CHEST_SLOT_HOTBAR, CHEST_SLOT_STORAGE, CHEST_SIZE_STORAGE,               \
	  IRON_CHEST_SIZE_STORAGE, WINDOW_RECOVERY_TE_CHEST, WINDOW_KEY_BLOCK)     \
	X(iron_chest, WINDOWC_IRON_CHEST, WINDOW_TYPE_IRON_CHEST, IRON_CHEST_SIZE, \
	  0, IRON_CHEST_SLOT_MAIN, IRON_CHEST_SLOT_HOTBAR, IRON_CHEST_SLOT_STORAGE,\
	  IRON_CHEST_SIZE_STORAGE, IRON_CHEST_SIZE_STORAGE,                        \
	  WINDOW_RECOVERY_TE_CHEST, WINDOW_KEY_BLOCK)                              \
	/* Double chest: WINDOW_TYPE_CHEST since 2026-07-29 (handoff P2(f)) --     \
	 * Beta serves both chest sizes from ONE GuiChest (BlockChest.java:199-211).\
	 * The routing INDEX stays WINDOWC_IRON_CHEST because it is persisted in    \
	 * death-drop journals; see block_chest.c onRightClick. */                  \
	X(double_chest, WINDOWC_IRON_CHEST, WINDOW_TYPE_CHEST, IRON_CHEST_SIZE, 1, \
	  IRON_CHEST_SLOT_MAIN, IRON_CHEST_SLOT_HOTBAR, IRON_CHEST_SLOT_STORAGE,   \
	  IRON_CHEST_SIZE_STORAGE, IRON_CHEST_SIZE_STORAGE,                        \
	  WINDOW_RECOVERY_TE_CHEST, WINDOW_KEY_BLOCK)                              \
	/* 3 = TileEntityFurnace input/fuel/output; bound to TE_FURNACE_SLOTS by a  \
	 * _Static_assert in window_registry.c. */                                  \
	X(furnace, WINDOWC_FURNACE, WINDOW_TYPE_FURNACE, FURNACE_SIZE, 0,          \
	  FURNACE_SLOT_MAIN, FURNACE_SLOT_HOTBAR, FURNACE_SLOT_INPUT, 3, 3,        \
	  WINDOW_RECOVERY_TE_FURNACE, WINDOW_KEY_BLOCK)                            \
	X(dispenser, WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER, DISPENSER_SIZE, 0,  \
	  DISPENSER_SLOT_MAIN, DISPENSER_SLOT_HOTBAR, DISPENSER_SLOT_STORAGE,      \
	  DISPENSER_SIZE_STORAGE, DISPENSER_SIZE_STORAGE,                          \
	  WINDOW_RECOVERY_TE_DISPENSER, WINDOW_KEY_BLOCK)                          \
	X(sign, WINDOWC_SIGN, WINDOW_TYPE_SIGN, SIGN_SIZE, 0, -1, -1, 0, 0, 0,     \
	  WINDOW_RECOVERY_NONE, WINDOW_KEY_BLOCK)                                  \
	/* Storage minecart: D_COUNT/JOURNAL stay 0 on purpose. The cart is a live  \
	 * world ENTITY that survives the player's death, so its 27 slots are        \
	 * neither dropped as death loot nor replayed by crash recovery; the         \
	 * residual gap is recorded in docs/deviations.md. See the long note in      \
	 * server_local_plan_death_drops. */                                        \
	X(minecart_chest, WINDOWC_MINECART_CHEST, WINDOW_TYPE_CHEST, CHEST_SIZE, 0,\
	  CHEST_SLOT_MAIN, CHEST_SLOT_HOTBAR, 0, 0, 0, WINDOW_RECOVERY_NONE,       \
	  WINDOW_KEY_ENTITY)                                                       \
	WINDOW_REGISTRY_EXTRA_ROW(X)

struct window_desc {
	const struct inventory_logic* logic;
	uint8_t windowc;
	enum window_type wire_type;
	uint16_t capacity;
	bool linked;
	int16_t main_slot;
	int16_t hotbar_slot;
	int16_t death_first;
	int16_t death_count;
	uint8_t journal_slots;
	enum window_recovery recovery;
	enum window_key key;
};

#define WINDOW_REGISTRY_COUNT_ROW(...) +1
#define WINDOW_REGISTRY_COUNT (WINDOW_REGISTRY(WINDOW_REGISTRY_COUNT_ROW))

/* THE GUARD. Adding or removing a row changes this count and stops the build
 * here, on purpose: the two dispatch sites that cannot be pure data must be
 * audited by hand before the number is bumped. Bumping it without walking both
 * is how D3/D9/D10 shipped.
 *
 *   1. server_local_plan_death_drops (server_local.c) -- does the new window
 *      have a backing owner whose disappearance turns its storage into death
 *      loot? Miss it and every lethal hit taken with that window open is
 *      REVERTED: the journal fails, health is restored, the window never
 *      closes, and the player is immortal in lava until they close the GUI.
 *   2. death_drop_recovery.c -- pick the row's `recovery` kind. If it needs a
 *      new one, add it to enum window_recovery, bump WINDOW_RECOVERY_COUNT,
 *      and add its arm to clear_container_sources,
 *      clear_legacy_container_sources and apply_container_slots. Miss it and
 *      crash replay HARD-FAILS world recovery on an unrecognised window.
 */
_Static_assert(
	WINDOW_REGISTRY_COUNT == 9,
	"window registry row added/removed: audit server_local_plan_death_drops "
	"and death_drop_recovery.c before bumping this count (CODESTYLE.md S3.3)");

/* NULL when the logic owns no window row (never registered). */
const struct window_desc* window_registry_find(
	const struct inventory_logic* logic);

/* Lookup by the persisted routing byte, for the journal/recovery paths that
 * only ever see that byte. WINDOWC_IRON_CHEST maps to two rows (iron chest and
 * double chest); they agree on every field this lookup is used for, which
 * test_window_registry.c pins. */
const struct window_desc* window_registry_by_windowc(uint8_t windowc);

#endif
