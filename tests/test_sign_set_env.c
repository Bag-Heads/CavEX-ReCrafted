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

// Pins the sign-specific set operation and durable close copy. Like
// test_sign_charcycle, only inventory_sign.c is linked and every other symbol
// is stubbed below. Expected characters are hand-written, never captured from
// CaveWii (POISONED-GOLDEN rule). Beta accepts only its allowed characters and
// appends/backspaces the active line [740c583 GuiEditSign.keyTyped]; the fixed
// 64-cell window and close-time copy are standing CaveWii deviations.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../source/game/gui/sign_osk_model.h"
#include "../source/network/inventory_logic.h"
#include "../source/network/server_local.h"
#include "../source/network/server_local_state.h" // full server state (split from server_local.h)

static struct server_local stub_server;
/* A1b: the window's owner carries its opening-world binding. */
static struct server_player stub_owner;
static struct item_data stub_items[SIGN_SIZE];

bool inventory_get_slot(struct inventory* inv, size_t slot,
						struct item_data* item) {
	if(!inv || slot >= inv->capacity || inv->items[slot].id == 0)
		return false;
	if(item)
		*item = inv->items[slot];
	return true;
}

void inventory_set_slot(struct inventory* inv, size_t slot,
						struct item_data item) {
	assert(inv && slot < inv->capacity);
	inv->items[slot] = item;
}

void inventory_destroy(struct inventory* inv,
					   struct server_player* player) {
	(void)inv;
	(void)player;
}

void inventory_destroy_owned(struct inventory* inv,
							 struct server_player* player) {
	inventory_destroy(inv, player);
}

bool inventory_get_picked_item(struct inventory* inv, struct item_data* item) {
	(void)inv;
	(void)item;
	return false;
}

void inventory_clear_picked_item(struct inventory* inv) {
	(void)inv;
}

bool server_world_get_block(struct server_world* world, w_coord_t x,
							w_coord_t y, w_coord_t z,
							struct block_data* block) {
	(void)world;
	(void)x;
	(void)y;
	(void)z;
	block->type = BLOCK_SIGN;
	return true;
}

bool server_local_spawn_item_batch(vec3 pos, const struct item_data* items,
								 size_t count, bool throw,
								 struct server_local* s) {
	(void)pos;
	(void)items;
	(void)count;
	(void)throw;
	(void)s;
	return false;
}

bool server_local_spawn_sourced_item_batch_for(
	vec3 pos, const struct item_data* items,
	const struct death_drop_provenance* provenance, size_t count, bool throw,
	bool container_rng, struct server_local* s, struct server_player* player) {
	(void)player;
	(void)provenance;
	(void)container_rng;
	return server_local_spawn_item_batch(pos, items, count, throw, s);
}

void server_local_send_inv_changes(set_inv_slot_t changes,
								   struct inventory* inv, uint8_t window) {
	(void)changes;
	(void)inv;
	(void)window;
}

// inv_on_close (exercised below) now sends a CRPC_SIGN_TEXT edit-echo to the
// client-side sign_text_store (source/game/sign_text_store.h) after the
// durable sign_texts write this file already asserts on -- that echo path is
// covered by tests/test_sign_text_store.c's integration case, not here, so
// this stub stays a no-op like server_local_send_inv_changes above.
struct client_rpc;
void clin_rpc_send(struct client_rpc* call) {
	(void)call;
}

static bool changes_is_only(set_inv_slot_t changes, size_t slot) {
	if(set_inv_slot_size(changes) != 1)
		return false;
	set_inv_slot_it_t it;
	for(set_inv_slot_it(it, changes); !set_inv_slot_end_p(it);
		set_inv_slot_next(it))
		if(*set_inv_slot_ref(it) == slot)
			return true;
	return false;
}

static void expect_edits(
	const struct sign_osk_edit* actual, size_t actual_count,
	const struct sign_osk_edit* expected, size_t expected_count) {
	assert(actual_count == expected_count);
	for(size_t k = 0; k < expected_count; k++) {
		assert(actual[k].slot == expected[k].slot);
		assert(actual[k].ch == expected[k].ch);
	}
}

static void test_shadow_model(void) {
	uint8_t cells[SIGN_OSK_SIZE];
	char shadow[SIGN_OSK_COLS + 1];
	struct sign_osk_edit edits[SIGN_OSK_COLS];

	/* Beta edits append-only strings and backspace removes their final
	 * character [740c583 GuiEditSign.keyTyped]. The expected strings/lists
	 * below are hand-computed from that model, never captured from this helper. */
	memset(cells, ' ', sizeof(cells));
	cells[16] = 'A';
	cells[17] = 'B';
	assert(sign_osk_shadow_from_cells(cells, 1, shadow) == 2);
	assert(strcmp(shadow, "AB") == 0); // trailing spaces alone are stripped

	memset(cells, ' ', sizeof(cells));
	cells[33] = 'A';
	cells[35] = 'B';
	assert(sign_osk_shadow_from_cells(cells, 2, shadow) == 4);
	assert(strcmp(shadow, " A B") == 0); // leading/interior spaces survive

	memset(cells, ' ', sizeof(cells));
	cells[0] = 'A';
	cells[1] = 'B';
	cells[2] = 'X';
	cells[3] = 'Y';
	static const struct sign_osk_edit pad_expected[] = {
		{.slot = 2, .ch = ' '},
		{.slot = 3, .ch = ' '},
	};
	expect_edits(
		edits, sign_osk_diff_cells(cells, 0, "AB", edits),
		pad_expected, sizeof(pad_expected) / sizeof(pad_expected[0]));

	memset(cells, ' ', sizeof(cells));
	static const struct sign_osk_edit clamp_expected[] = {
		{.slot = 0, .ch = '1'}, {.slot = 1, .ch = '2'},
		{.slot = 2, .ch = '3'}, {.slot = 3, .ch = '4'},
		{.slot = 4, .ch = '5'}, {.slot = 5, .ch = '6'},
		{.slot = 6, .ch = '7'}, {.slot = 7, .ch = '8'},
		{.slot = 8, .ch = '9'}, {.slot = 9, .ch = '0'},
		{.slot = 10, .ch = 'a'}, {.slot = 11, .ch = 'b'},
		{.slot = 12, .ch = 'c'}, {.slot = 13, .ch = 'd'},
		{.slot = 14, .ch = 'e'}, {.slot = 15, .ch = 'f'},
	};
	expect_edits(
		edits, sign_osk_diff_cells(cells, 0, "1234567890abcdefZ", edits),
		clamp_expected,
		sizeof(clamp_expected) / sizeof(clamp_expected[0]));

	memset(cells, ' ', sizeof(cells));
	static const struct sign_osk_edit ordered_expected[] = {
		{.slot = 48, .ch = 'A'},
		{.slot = 50, .ch = 'B'},
	};
	expect_edits(
		edits, sign_osk_diff_cells(cells, 3, "A B", edits),
		ordered_expected,
		sizeof(ordered_expected) / sizeof(ordered_expected[0]));

	memset(cells, ' ', sizeof(cells));
	cells[32] = 'H';
	cells[33] = 'i';
	assert(sign_osk_diff_cells(cells, 2, "Hi", edits) == 0);
}

static void seed_sign(struct inventory* inv) {
	memset(stub_items, 0, sizeof(stub_items));
	for(size_t slot = 0; slot < SIGN_SIZE; slot++) {
		stub_items[slot].id = 1;
		stub_items[slot].count = ' ';
	}
	*inv = (struct inventory) {
		.items = stub_items,
		.capacity = SIGN_SIZE,
		.x = 7,
		.y = 8,
		.z = 9,
		.user = &stub_server,
		.owner = &stub_owner,
	};
}

int main(void) {
	test_shadow_model();

	struct inventory inv;
	seed_sign(&inv);

	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	assert(inventory_sign_set_char(&inv, 18, 'A', changes));
	assert(stub_items[18].count == 'A');
	assert(changes_is_only(changes, 18));
	set_inv_slot_clear(changes);

	// Both inclusive printable-ASCII endpoints are valid at an ordinary slot.
	// Slot 62 = line 3 col 14, the LAST column Beta's 15-char gate allows --
	// this doubles as "the 15th character is accepted".
	set_inv_slot_init(changes);
	assert(inventory_sign_set_char(&inv, 0, ' ', changes));
	assert(inventory_sign_set_char(&inv, 62, '~', changes));
	assert(stub_items[0].count == ' ' && stub_items[62].count == '~');
	set_inv_slot_clear(changes);

	// Hand-computed rejection boundaries: 0x1f, 0x7f, and slot 64.
	set_inv_slot_init(changes);
	assert(!inventory_sign_set_char(&inv, 18, 0x1f, changes));
	assert(!inventory_sign_set_char(&inv, 18, 0x7f, changes));
	assert(!inventory_sign_set_char(&inv, SIGN_SIZE, 'B', changes));
	assert(stub_items[18].count == 'A');
	assert(set_inv_slot_size(changes) == 0);
	set_inv_slot_clear(changes);

	// [maintainer ruling 2026-08-17] server-authoritative Beta-15 clamp: slot
	// % SIGN_LINE_COLS == SIGN_BETA_MAX_CHARS is column 15, the 16th grid
	// cell, on EVERY line -- past [740c583 GuiEditSign.keyTyped]'s
	// `signText[line].length() < 15` (character-count only, no pixel-width
	// term). A printable char is rejected there even though ch itself is
	// in-range and the slot itself is occupied -- an oversized line coming
	// through SRPC_SIGN_SET (this IS the handler's accept gate) must not
	// persist. ' ' still succeeds: it is the space seed_sign() already put
	// there, so writing it back is a legitimate no-op / self-heal, not a
	// hole back in.
	set_inv_slot_init(changes);
	assert(!inventory_sign_set_char(&inv, 15, 'Z', changes)); // line 0
	assert(!inventory_sign_set_char(&inv, 31, 'Z', changes)); // line 1
	assert(!inventory_sign_set_char(&inv, 47, 'Z', changes)); // line 2
	assert(!inventory_sign_set_char(&inv, 63, '~', changes)); // line 3
	assert(set_inv_slot_size(changes) == 0);
	assert(stub_items[15].count == ' ' && stub_items[31].count == ' '
		   && stub_items[47].count == ' ' && stub_items[63].count == ' ');
	assert(inventory_sign_set_char(&inv, 63, ' ', changes)); // clearing stays legal
	assert(changes_is_only(changes, 63));
	set_inv_slot_clear(changes);

	struct inventory short_inv = inv;
	short_inv.capacity = 8;
	set_inv_slot_init(changes);
	assert(!inventory_sign_set_char(&short_inv, 8, 'B', changes));
	assert(set_inv_slot_size(changes) == 0);
	set_inv_slot_clear(changes);

	// The legacy cycle callback still consumes the action without moving a
	// character item; test_sign_charcycle pins its exact wrap arithmetic.
	set_inv_slot_init(changes);
	assert(!inventory_logic_sign.pre_action(&inv, NULL, 18, true, changes));
	assert(changes_is_only(changes, 18));
	set_inv_slot_clear(changes);

	/* IMP-TE-CAPS: the sign table is a slab now; this dependency-light stub
	 * hand-allocates just the sign rows (fixed view: no growth needed). */
	stub_server.te_rows[TE_KIND_SIGN] = MAX_SIGNS;
	stub_server.sign_pos = malloc(MAX_SIGNS * sizeof(*stub_server.sign_pos));
	stub_server.sign_texts = calloc(MAX_SIGNS, sizeof(*stub_server.sign_texts));
	stub_server.te_owners.sign_slot = calloc(MAX_SIGNS, 1);
	stub_server.te_owners.sign_gen = calloc(MAX_SIGNS, sizeof(uint32_t));
	assert(stub_server.sign_pos && stub_server.sign_texts
		   && stub_server.te_owners.sign_slot && stub_server.te_owners.sign_gen);
	memset(stub_server.sign_pos, 0xff, MAX_SIGNS * sizeof(*stub_server.sign_pos));
	stub_server.sign_pos[3]
		= (struct complex_block_pos) {.x = 7, .y = 8, .z = 9};
	/* A1b: a live slot-0 world instance owns the row and binds the window. */
	stub_server.worlds[0].world_index = 0;
	stub_server.worlds[0].session = &stub_server;
	stub_server.worlds[0].chunks_ready = true;
	stub_server.worlds[0].generation = 7;
	stub_server.te_owners.sign_slot[3] = 0;
	stub_server.te_owners.sign_gen[3] = 7;
	stub_owner.window_world = (struct server_world_handle) {0, 7};
	set_inv_slot_init(changes);
	assert(inventory_sign_set_char(&inv, 5, 'Q', changes));
	assert(inventory_sign_set_char(&inv, 46, 'z', changes));
	set_inv_slot_clear(changes);
	struct server_player player = {0};
	assert(inventory_logic_sign.on_close(&inv, &player));
	assert(stub_server.sign_texts[3][5] == 'Q');
	assert(stub_server.sign_texts[3][46] == 'z');
	// Column 15 of line 3 never accepted a printable char above (rejected at
	// 63, cleared back to ' '), so the durable copy carries the space, not a
	// 16th character.
	assert(stub_server.sign_texts[3][63] == ' ');

	return 0;
}
