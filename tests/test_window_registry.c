/*
	test_window_registry -- pins the container/window registry (window_registry.h)
	that replaced the six hand-maintained dispatch tables of CODESTYLE.md S3.3.

	(A) Every row still carries the exact tuple the pre-refactor dispatch chains
	    hardcoded. This is the no-behaviour-change proof for the data half of
	    the refactor: if a row's windowc / wire type / capacity / slot offsets /
	    journal bound ever drifts, the corresponding site's behaviour has
	    changed even though every golden may still pass.
	(B) Routing ids are pairwise distinct EXCEPT the deliberate iron-chest /
	    double-chest pair, which shares WINDOWC_IRON_CHEST because that byte is
	    persisted in death-drop journals (block_chest.c onRightClick).
	(C) Rows sharing a windowc agree on journal_slots and recovery. The journal
	    and crash-recovery paths only ever see that byte, so
	    window_registry_by_windowc returning "the first match" must be
	    unambiguous for the fields those paths read.
	(D) A window that can be journaled must have a recovery kind, and one that
	    cannot must not -- the exact D9/D10 pairing (journal accepts the record,
	    recovery cannot resolve its owner).

	Pure macro expansion: this test reads the SAME WINDOW_REGISTRY rows the
	engine does, so it links nothing.
*/

#include <assert.h>
#include <stdio.h>

#include "../source/network/window_registry.h"

struct row {
	const char* name;
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

#define ROW(LOGIC, WINDOWC, WIRE, CAP, LINKED, MAIN, HOTBAR, D_FIRST, D_COUNT, \
			JOURNAL, RECOVERY, KEY)                                            \
	{#LOGIC, (WINDOWC), (WIRE),      (CAP),     (bool)(LINKED), (MAIN),         \
	 (HOTBAR), (D_FIRST), (D_COUNT), (JOURNAL), (RECOVERY),     (KEY)},

static const struct row rows[] = {WINDOW_REGISTRY(ROW)};

/* (A) The pre-refactor tuples, transcribed from the dispatch chains this
 * registry replaced (server_local.c open whitelist / post-close projection /
 * plan_death_drops / active_window_id / reopen, and death_drop_journal.c's
 * container_slot_valid) as they stood at a31b80c. */
static const struct row expected[] = {
	{"player", WINDOWC_INVENTORY, WINDOW_TYPE_INVENTORY, 0, false, -1, -1, 0, 0,
	 0, WINDOW_RECOVERY_NONE, WINDOW_KEY_NONE},
	{"crafting", WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE, false,
	 CRAFTING_SLOT_MAIN, CRAFTING_SLOT_HOTBAR, 0, 0, 0, WINDOW_RECOVERY_NONE,
	 WINDOW_KEY_BLOCK},
	{"chest", WINDOWC_CHEST, WINDOW_TYPE_CHEST, CHEST_SIZE, false,
	 CHEST_SLOT_MAIN, CHEST_SLOT_HOTBAR, CHEST_SLOT_STORAGE, CHEST_SIZE_STORAGE,
	 54, WINDOW_RECOVERY_TE_CHEST, WINDOW_KEY_BLOCK},
	{"iron_chest", WINDOWC_IRON_CHEST, WINDOW_TYPE_IRON_CHEST, IRON_CHEST_SIZE,
	 false, IRON_CHEST_SLOT_MAIN, IRON_CHEST_SLOT_HOTBAR,
	 IRON_CHEST_SLOT_STORAGE, IRON_CHEST_SIZE_STORAGE, 54,
	 WINDOW_RECOVERY_TE_CHEST, WINDOW_KEY_BLOCK},
	{"double_chest", WINDOWC_IRON_CHEST, WINDOW_TYPE_CHEST, IRON_CHEST_SIZE,
	 true, IRON_CHEST_SLOT_MAIN, IRON_CHEST_SLOT_HOTBAR,
	 IRON_CHEST_SLOT_STORAGE, IRON_CHEST_SIZE_STORAGE, 54,
	 WINDOW_RECOVERY_TE_CHEST, WINDOW_KEY_BLOCK},
	{"furnace", WINDOWC_FURNACE, WINDOW_TYPE_FURNACE, FURNACE_SIZE, false,
	 FURNACE_SLOT_MAIN, FURNACE_SLOT_HOTBAR, FURNACE_SLOT_INPUT, 3, 3,
	 WINDOW_RECOVERY_TE_FURNACE, WINDOW_KEY_BLOCK},
	{"dispenser", WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER, DISPENSER_SIZE,
	 false, DISPENSER_SLOT_MAIN, DISPENSER_SLOT_HOTBAR, DISPENSER_SLOT_STORAGE,
	 DISPENSER_SIZE_STORAGE, DISPENSER_SIZE_STORAGE,
	 WINDOW_RECOVERY_TE_DISPENSER, WINDOW_KEY_BLOCK},
	{"sign", WINDOWC_SIGN, WINDOW_TYPE_SIGN, SIGN_SIZE, false, -1, -1, 0, 0, 0,
	 WINDOW_RECOVERY_NONE, WINDOW_KEY_BLOCK},
	{"minecart_chest", WINDOWC_MINECART_CHEST, WINDOW_TYPE_CHEST, CHEST_SIZE,
	 false, CHEST_SLOT_MAIN, CHEST_SLOT_HOTBAR, 0, 0, 0, WINDOW_RECOVERY_NONE,
	 WINDOW_KEY_ENTITY},
};

int main(void) {
	const size_t n = sizeof(rows) / sizeof(rows[0]);
	assert(n == WINDOW_REGISTRY_COUNT);
	assert(n == sizeof(expected) / sizeof(expected[0]));

	// (A) every field of every row, against the pre-refactor tuple
	for(size_t i = 0; i < n; i++) {
		const struct row* a = &rows[i];
		const struct row* b = &expected[i];
		assert(a->windowc == b->windowc);
		assert(a->wire_type == b->wire_type);
		assert(a->capacity == b->capacity);
		assert(a->linked == b->linked);
		assert(a->main_slot == b->main_slot);
		assert(a->hotbar_slot == b->hotbar_slot);
		assert(a->death_first == b->death_first);
		assert(a->death_count == b->death_count);
		assert(a->journal_slots == b->journal_slots);
		assert(a->recovery == b->recovery);
		assert(a->key == b->key);
	}

	for(size_t i = 0; i < n; i++) {
		for(size_t j = i + 1; j < n; j++) {
			if(rows[i].windowc != rows[j].windowc)
				continue;
			// (B) the ONE deliberate id share
			assert(rows[i].windowc == WINDOWC_IRON_CHEST);
			// (C) ...and it must be unambiguous for the by-windowc lookups
			assert(rows[i].journal_slots == rows[j].journal_slots);
			assert(rows[i].recovery == rows[j].recovery);
		}
		// (D) journalable <=> recoverable
		assert((rows[i].journal_slots != 0)
			   == (rows[i].recovery != WINDOW_RECOVERY_NONE));
		// a journaled window's bound must cover the span plan_death_drops
		// actually writes
		assert(rows[i].death_count <= (int16_t)rows[i].journal_slots
			   || rows[i].journal_slots == 0);
		// a window the open path can create must name a real backing owner
		assert(rows[i].capacity == 0 || rows[i].key != WINDOW_KEY_NONE);
	}

	printf("test_window_registry: OK (%d rows)\n", (int)n);
	return 0;
}
