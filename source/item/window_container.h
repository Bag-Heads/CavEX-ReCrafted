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

#ifndef WINDOW_CONTAINER_H
#define WINDOW_CONTAINER_H

#include <m-lib/m-i-list.h>
#include <stdint.h>

#include "inventory.h"
#include "../platform/time.h"

struct inventory_sort_plan;
#define WINDOW_SORT_PIPELINE 4
/* Ordinary remote clicks get WINDOWC_REPLY_TIMEOUT_MS (windowc_expire_unanswered,
 * driven by client_session_flush_window_transactions). Remote automation uses screen_load_world.c's 30s stall patience
 * (LWORLD_REMOTE_STALL_TIMEOUT_S). This exceeds the transport's 500ms full
 * event-queue guard (REMOTE_EVENT_BACKPRESSURE_MS) and main.c's 15s closing
 * provider guard (MP_SWAP_DEADLINE_MS); neither bounds a live PLAY connection
 * missing an ACK. Expiry fences mutation until authoritative correction,
 * without declaring a disconnect or inventing a server rejection. */
#define WINDOW_SORT_TIMEOUT_MS 30000
/* A Beta server answers every Packet102 it processes with a Packet106
 * (NetServerHandler.java:499-519) and silently ignores one only when the
 * window id mismatches or its canCraft gate is shut (:499). A click that
 * stays unanswered this long stops blocking: its prediction is committed,
 * exactly as Beta's client keeps every click (windowc_commit_pending). */
#define WINDOWC_REPLY_TIMEOUT_MS 5000
enum window_sort_status {
	WINDOW_SORT_IDLE, WINDOW_SORT_ADMISSION, WINDOW_SORT_RUNNING,
	WINDOW_SORT_INTERRUPTED, WINDOW_SORT_COMPLETE
};
struct window_sort_session {
	const struct inventory_sort_plan* plan; /* leased static buffer; never stack */
	enum window_sort_status status;
	size_t submitted, acknowledged;
	uint32_t generation, admission_ticket;
	uint16_t actions[WINDOW_SORT_PIPELINE];
	ptime_t deadlines[WINDOW_SORT_PIPELINE];
	bool chest, remote;
};

#define WINDOWC_INVENTORY 0

// local server use only -- these are gstate.players[0].windows[] routing slots and MUST be
// pairwise distinct. Id 0 is permanently reserved for the player inventory (in
// Beta windowId 0 is never handed to a container; getNextWindowId() cycles
// 1..100). WINDOWC_CHEST was 0, aliasing the inventory window: opening a single
// wooden chest sent CRPC_OPEN_WINDOW(window=0), whose handler destroys+recreates
// that slot -> the live inventory container (armor/items) was freed = data loss.
// It now uses the free slot 3, so a chest open leaves windows[0] intact, exactly
// as the iron chest at id 4 already does. (WINDOW_TYPE_DISPENSER=3 below is a
// different field -- the wire 'type', not this routing index -- and is unused.)
#define WINDOWC_CHEST 3
#define WINDOWC_CRAFTING 1
#define WINDOWC_FURNACE 2
#define WINDOWC_IRON_CHEST 4
#define WINDOWC_SIGN 5
#define WINDOWC_DISPENSER 6
// Storage minecart's entity-backed chest window (minecart-slice). Reuses
// WINDOW_TYPE_CHEST so the client opens the ordinary screen_chest -- Beta's
// own EntityPlayer.displayGUIChest(IInventory) is generic over a block OR
// entity IInventory, and this engine's screen_chest.c is already generic
// over the window's own item array (windowc_get_latest), not tied to a block
// position at all. Next free routing slot after WINDOWC_DISPENSER (both slices
// independently claimed 6; the dispenser merged first, so this took 7).
#define WINDOWC_MINECART_CHEST 7


enum window_type {
	WINDOW_TYPE_CHEST = 0,
	WINDOW_TYPE_WORKBENCH = 1,
	WINDOW_TYPE_FURNACE = 2,
	WINDOW_TYPE_DISPENSER = 3,
	WINDOW_TYPE_IRON_CHEST = 4,
	WINDOW_TYPE_SIGN = 5,
	WINDOW_TYPE_INVENTORY = 255,
};

struct window_container {
	uint32_t generation;
	struct window_sort_session sort;
	enum window_type type;
	size_t slot_count;
	uint16_t map_counter; // standalone fallback; sessions bind their world counter
	uint16_t next_action_id;
	uint16_t quick_action_id;
	bool quick_pending;
	bool correction_pending;
	bool transaction_reply_pending;
	uint16_t transaction_reply_action;
	// Furnace burn/cook progress (server CRPC_WINDOW_PROGRESS), CLIENT DISPLAY ONLY:
	// the flame + cook-arrow in screen_furnace.c. Mirrors Beta TileEntityFurnace's
	// furnaceCookTime / furnaceBurnTime / currentItemBurnTime; the server stays
	// authoritative (these are never used in simulation). Zero for non-furnace windows.
	uint16_t furnace_cook_time;			// 0..200 (getCookProgressScaled numerator)
	uint16_t furnace_burn_time;			// ticks of lit fuel remaining
	uint16_t furnace_current_item_burn; // lit fuel's full duration (flame scale)
	uint32_t rejection_serial;
	/* Cumulative count of server-accepted replies to tracked actions; bumped
	 * beside rejection_serial, reset only by windowc_create. Corrections and
	 * discards do not touch it (test drivers observe accepted results). */
	uint32_t accept_serial;
	/* Acceptance watermark: the newest action id a server Packet106 ACCEPTED
	 * (everything issued at or before it has committed). Moved ONLY by an
	 * accept -- never by windowc_commit_pending's reply-deadline commit -- so a
	 * success cue can never fire for a transfer the server has not confirmed. */
	uint16_t accepted_action;
	/* windowc_expire_unanswered's no-progress clock (armed while waiting). */
	ptime_t reply_deadline;
	bool reply_deadline_armed;
	ilist_inventory_t invs;
};

void windowc_sort_interrupt(struct window_container* wc);

bool windowc_create(struct window_container* wc, enum window_type type,
					size_t slot_count);
void windowc_destroy(struct window_container* wc);

struct inventory* windowc_get_latest(struct window_container* wc);
bool windowc_actions_pending(const struct window_container* wc);
/* Commit outstanding predictions as Beta's client holds them (no rollback on
 * silence); correction_pending untouched. */
void windowc_commit_pending(struct window_container* wc);
/* Commit clicks the server has not answered for WINDOWC_REPLY_TIMEOUT_MS of
 * no progress. Never while a Packet106 ack is still owed, never past a
 * pending correction. True iff it committed. */
bool windowc_expire_unanswered(struct window_container* wc, ptime_t now);
bool windowc_new_action(struct window_container* wc, uint16_t* action_id,
						bool action_type, size_t action_slot);
bool windowc_new_sign_set_action(struct window_container* wc,
								 uint16_t* action_id, size_t action_slot,
								 uint8_t ch);
// Both actions predict state and share ordinary clicks' action-ID sequence.
bool windowc_new_drop_action(struct window_container* wc, uint16_t* action_id,
	bool right);
bool windowc_new_quick_action(struct window_container* wc, uint16_t* action_id,
	size_t slot);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
// Allocation fault seam for the revision snapshot owned by windowc_create.
void windowc_set_alloc_hook(void* (*alloc_fn)(size_t));
#endif
void windowc_cancel_unsent(struct window_container* wc, uint16_t action_id);

void windowc_action_apply_result(struct window_container* wc,
								 uint16_t action_id, bool accept);
// Keep the authoritative first snapshot and discard every optimistic action
// layered after it. Used at player/world ownership handoffs after the server's
// full slot sync is FIFO-applied and pre-handoff clicks are rejected.
void windowc_discard_pending_actions(struct window_container* wc);
void windowc_hotbar_change(struct window_container* wc, size_t slot);
void windowc_slot_change(struct window_container* wc, size_t slot,
						 struct item_data item);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
size_t windowc_revision_count(const struct window_container* wc);
#endif

#endif
