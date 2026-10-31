/*
	Memory-safety guard (bug_hunt_2026-06-26, DEFERRED row): the SRPC_WINDOW_CLICK
	dispatch in source/network/server_local.c forwards the UNTRUSTED, network-
	delayed client slot (a uint8_t, 0..255 -- server_interface.h window_click.slot)
	into inventory_action(), which reads AND writes inv->items[slot]. The only
	bound check inside inventory_action is an assert(slot < inv->capacity) -- and
	that compiles out under the shipping .dol's -DNDEBUG (only the test_ targets
	force -UNDEBUG). Window capacities differ (player 45, chest 63, iron chest up
	to 90), and because the client and server run on separate threads over a
	delayed reliable ring, a stale large-chest slot can apply AFTER active_inventory
	shrank back to the 45-slot player inventory (window closed) -> out-of-bounds
	heap read+write on the embedded target.

	The fix routes that decision through inventory_slot_valid(), which the dispatch
	now consults before inventory_action (mirroring the adjacent SRPC_HOTBAR_SLOT
	bound). This test pins that predicate -- the SAME static-inline the dispatch
	calls, so it is real production code, not a re-implementation:

	  - every in-range slot of each window is accepted (no off-by-one that would
	    silently reject a legitimate click),
	  - the exact bug -- a stale iron-chest slot (45..89) applied after the active
	    inventory shrank to the 45-slot player inventory -- is rejected,
	  - slot == capacity is the first invalid index, and the whole uint8_t range a
	    malformed client could send past capacity is rejected,
	  - a NULL active_inventory is rejected (never dereferenced).

	Header-only, like test_sign_slot_bound.c: inventory_slot_valid is a static
	inline in inventory.h, so no source is linked and the proof is the asserts,
	forced live by the CMake -UNDEBUG test rule.
*/

#include <assert.h>
#include <stddef.h>

#include "item/inventory.h"

/* A stand-in inventory whose items[] would be `cap` elements wide.
   inventory_slot_valid only reads ->capacity (never ->items), so a NULL items
   pointer is safe and keeps the test header-only (no inventory_create / item
   registry needed). */
static struct inventory make_inv(size_t cap) {
	struct inventory inv = {0};
	inv.capacity = cap;
	return inv; /* items stays NULL */
}

int main(void) {
	/* The three real window capacities the dispatch guards (inventory.h). Pinning
	   them means a future change to these caps re-checks this guard on purpose. */
	const size_t player_cap = INVENTORY_SIZE;	   /* 45 */
	const size_t chest_cap = CHEST_SIZE;		   /* 63 */
	const size_t iron_chest_cap = IRON_CHEST_SIZE; /* 90 */
	assert(player_cap == 45 && chest_cap == 63 && iron_chest_cap == 90);

	struct inventory player = make_inv(player_cap);
	struct inventory chest = make_inv(chest_cap);
	struct inventory iron = make_inv(iron_chest_cap);

	/* Every in-range slot of each window is accepted -- a legitimate click must
	   never be rejected (catches an off-by-one like slot <= capacity). */
	for(size_t s = 0; s < player_cap; s++)
		assert(inventory_slot_valid(&player, s));
	for(size_t s = 0; s < chest_cap; s++)
		assert(inventory_slot_valid(&chest, s));
	for(size_t s = 0; s < iron_chest_cap; s++)
		assert(inventory_slot_valid(&iron, s));

	/* The exact deferred bug: a stale click for an iron-chest slot (45..89)
	   arriving after active_inventory shrank back to the 45-slot player inventory
	   is rejected, so inventory_action never indexes player.items[] out of bounds.
	   The same slot was valid while the chest window was still open. */
	for(size_t s = player_cap; s < iron_chest_cap; s++) {
		assert(inventory_slot_valid(&iron, s));		/* valid while chest open */
		assert(!inventory_slot_valid(&player, s));	/* OOB once shrunk -> reject */
	}

	/* Boundary: slot == capacity is the first invalid index (items[] is 0..cap-1). */
	assert(!inventory_slot_valid(&player, player_cap));
	assert(!inventory_slot_valid(&chest, chest_cap));
	assert(!inventory_slot_valid(&iron, iron_chest_cap));

	/* The full uint8_t range a malformed client could send is rejected past cap. */
	for(size_t s = iron_chest_cap; s <= 255; s++)
		assert(!inventory_slot_valid(&iron, s));

	/* A NULL active_inventory is rejected, never dereferenced. */
	assert(!inventory_slot_valid(NULL, 0));
	assert(!inventory_slot_valid(NULL, 42));

	return 0;
}
