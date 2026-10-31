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

#include "../item/recipe.h"
#include "../item/window_container.h"
#include "client_interface.h" // CRPC_SIGN_TEXT edit-echo (inv_on_close)
#include "inventory_logic.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

#include <string.h>

/* A1b: resolved in the window's opening world (see inventory_chest.c). */
static int find_sign(const struct inventory* inv, w_coord_t x, w_coord_t y,
					 w_coord_t z) {
	struct server_world* w = server_local_window_world(inv);
	struct te_store st = server_local_window_tile_store(inv);
	int i = w ? te_store_find(&st, TE_KIND_SIGN, x, y, z) : -1;
	if(i < 0)
		return -1;
	struct block_data block;
	return server_world_get_block(w, x, y, z, &block)
		&& block_is_any_sign(block.type) ? i : -1;
}

static bool inv_pre_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
						   set_inv_slot_t changes) {
	// [maintainer ruling 2026-08-17]: the d-pad/IR char-cycle is a SECOND
	// live route onto the same 64-cell sign window SRPC_SIGN_SET writes
	// (screen_sign.c wires IB_GUI_UP/DOWN to this callback for every slot,
	// OSK grid included) -- without this gate it could still cycle column 15
	// (the 16th cell of any line) away from ' ' and reopen the exact hole
	// inventory_sign_set_char below closes. Beta has no character-cycle
	// action at all [740c583 GuiEditSign has no such method]; this is
	// CaveWii's own Wii-controller-floor input (aca219c commit body), so the
	// fix is simply: that column never moves.
	if((slot % SIGN_LINE_COLS) >= SIGN_BETA_MAX_CHARS)
		return false;

	struct item_data chr;

	// text chars are stored as item counts
	if(inventory_get_slot(inv, slot, &chr)) {
		if(right) {
			chr.count++;
			if (chr.count > '~') chr.count = ' ';
		} else {
			chr.count--;
			if (chr.count < ' ') chr.count = '~';
		}
		inventory_set_slot(inv, slot, chr);
		set_inv_slot_push(changes, slot);
	}

	// do not move "char items" around
	return false;
}

bool inventory_sign_set_char(struct inventory* inv, size_t slot, uint8_t ch,
							 set_inv_slot_t changes) {
	if(!inv || slot >= SIGN_SIZE || slot >= inv->capacity
	   || ch < ' ' || ch > '~')
		return false;

	// [maintainer ruling 2026-08-17] server-authoritative half of the Beta-15
	// clamp: [740c583 GuiEditSign.keyTyped] gates a new character on
	// `signText[editLine].length() < 15` -- character-count only, no
	// pixel-width term (verified against the decompile). Column 15 of every
	// line (slot % SIGN_LINE_COLS == SIGN_BETA_MAX_CHARS, the 16th grid
	// cell) is past that ceiling and must never hold a printed character,
	// even from a client that skips the OSK's own gate. ' ' still succeeds
	// there: it is the diff-feed's self-heal write when a pre-fix line
	// already had 16 real characters (sign_osk_diff_cells emits a clearing
	// edit for that cell once the shadow's length caps at 15), and it is a
	// legitimate no-op against the space seed_sign()/inv_on_create already
	// put there -- not a hole back in.
	if((slot % SIGN_LINE_COLS) >= SIGN_BETA_MAX_CHARS && ch != ' ')
		return false;

	struct item_data chr;
	if(!inventory_get_slot(inv, slot, &chr))
		return false;

	chr.count = ch;
	inventory_set_slot(inv, slot, chr);
	set_inv_slot_push(changes, slot);
	return true;
}

static void inv_post_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
							bool accepted, set_inv_slot_t changes) {
}

static bool inv_on_close(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	int backing = find_sign(inv, inv->x, inv->y, inv->z);
	if(backing < 0) {
		struct item_data picked;
		bool has_picked = inventory_get_picked_item(inv, &picked);
		if(has_picked) {
			struct death_drop_provenance source = {
				.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
				.window = WINDOWC_SIGN,
				.slot = SPECIAL_SLOT_PICKED_ITEM,
				.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
			};
			if(!server_local_spawn_sourced_item_batch_for(
				   (vec3) {player->x, player->y, player->z},
				   &picked, &source, 1, true, false, s, player))
				return false;
		}
		if(has_picked) {
			inventory_clear_picked_item(inv);
			set_inv_slot_t changes;
			set_inv_slot_init(changes);
			set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
			server_local_send_inv_changes(changes, inv, WINDOWC_SIGN);
			set_inv_slot_clear(changes);
		}
		// Text belongs to the removed sign block, not to the player. With no
		// backing row left there is nothing durable to write; release the window
		// after preserving the only possible player-owned cursor stack.
		inventory_destroy_owned(inv, player);
		return true;
	}

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	// TODO: save sign text
	for(size_t k = 0; k < SIGN_SIZE; k++)
		s->sign_texts[backing][k] = inv->items[k].count;

	// Client-side sign-text store edit echo (source/game/sign_text_store.h):
	// Beta commits a sign's text on GUI close [740c583 GuiEditSign.
	// onGuiClosed], which is exactly this call -- s->sign_texts[backing] just
	// became the new durable truth, so tell the client (unconditionally; a
	// close with no edits re-sends the unchanged text, a harmless overwrite,
	// same as the s->sign_texts write two lines up).
	struct client_rpc sign_echo = {
		.type = CRPC_SIGN_TEXT,
		.payload.sign_text = {.x = inv->x, .y = inv->y, .z = inv->z},
	};
	/* A sign window can outlive its actor's dimension change. Publish into the
	 * world that owns the backing row, not the actor's current world. */
	struct server_world* w = server_local_window_world(inv);
	clin_rpc_stamp(&sign_echo, w->world_index, w->generation);
	memcpy(sign_echo.payload.sign_text.grid, s->sign_texts[backing],
		   SIGN_SIZE);
	clin_rpc_send(&sign_echo);

	server_local_send_inv_changes(changes, inv, WINDOWC_SIGN);
	set_inv_slot_clear(changes);

	inventory_destroy_owned(inv, player);
	return true;
}

static bool inv_on_collect(struct inventory* inv, struct server_player* player, struct item_data* item) {
	return false;
}

static bool inv_on_create(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	int backing = find_sign(inv, inv->x, inv->y, inv->z);
	if(backing < 0)
		return false;

	// TODO: restore sign text
	for(size_t k = 0; k < SIGN_SIZE; k++) {
		inv->items[k].id = 1;
		inv->items[k].count = s->sign_texts[backing][k];
	}
	return true;
}

static bool inv_on_destroy(struct inventory* inv, struct server_player* player) {
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	server_local_send_inv_changes(changes, &player->inventory,
								  WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);
	return true;
}

struct inventory_logic inventory_logic_sign = {
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = inv_on_create,
	.on_destroy = inv_on_destroy,
	.on_close = inv_on_close,
};
