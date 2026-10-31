/* Shared wooden chest storage. Beta 1.7.3 [740c583 Slot.putStack/getStack and
 * InventoryLargeChest.setInventorySlotContents] accesses the backing inventory
 * on every operation. Our contiguous GUI arrays are views: commit and fan out
 * storage changes before processing the next actor's request. */
#include "inventory_chest_shared.h"
#include "inventory_logic.h"
#include "client_interface.h"
#include "server_local.h"
#include "server_local_state.h"
#include "../item/window_container.h"

static int half_at(const struct inventory* inv,
	w_coord_t x, w_coord_t y, w_coord_t z) {
	if(inv->logic != &inventory_logic_chest
	   && inv->logic != &inventory_logic_double_chest)
		return -1;
	if(inv->x == x && inv->y == y && inv->z == z)
		return 0;
	if(inv->logic == &inventory_logic_double_chest
	   && inv->x2 == x && inv->y2 == y && inv->z2 == z)
		return CHEST_SIZE_STORAGE;
	return -1;
}

static uint8_t window_id(const struct inventory* inv) {
	return inv->logic == &inventory_logic_chest ? WINDOWC_CHEST : WINDOWC_IRON_CHEST;
}

bool inventory_chest_sort_admitted(const struct inventory* inv) {
	if(!inv || !inv->user || !inv->owner || inv->invalidated
	   || (inv->logic != &inventory_logic_chest
		   && inv->logic != &inventory_logic_double_chest)) return false;
	const struct server_local* s = inv->user;
	bool registered = false;
	for(const struct inventory* peer = s->chest_views; peer; peer = peer->shared_next) {
		if(peer == inv) { registered = true; continue; }
		if(!server_local_windows_may_share_world(peer, inv)) continue; /* A1b */
		if(half_at(peer, inv->x, inv->y, inv->z) >= 0
		   || (inv->has_partner && half_at(peer, inv->x2, inv->y2, inv->z2) >= 0))
			return false;
	}
	return registered;
}

/* A1b: the open view of (x,y,z) in world `w` (views bound elsewhere are a
 * different chest). */
struct inventory* inventory_chest_view_at_w(struct server_local* s,
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z) {
	for(struct inventory* inv = s->chest_views; inv; inv = inv->shared_next) {
		int offset = half_at(inv, x, y, z);
		if(offset >= 0 && server_local_window_bound_to(inv, w)
		   && !(inv->orphaned_chest_halves & (1 << (offset / CHEST_SIZE_STORAGE))))
			return inv;
	}
	return NULL;
}

/* LEGACY slot-0 adapter (A1b inventory). */
struct inventory* inventory_chest_view_at(struct server_local* s,
	w_coord_t x, w_coord_t y, w_coord_t z) {
	return inventory_chest_view_at_w(s, &s->worlds[0], x, y, z);
}

static struct inventory* chest_view_same_world(struct server_local* s,
	const struct inventory* self, w_coord_t x, w_coord_t y, w_coord_t z) {
	struct server_world* w = server_local_window_world(self);
	return w ? inventory_chest_view_at_w(s, w, x, y, z) : NULL;
}

bool inventory_chest_register(struct inventory* inv) {
	struct server_local* s = inv->user;
	/* Retire invalidated views before opening a replacement at the same key.
	 * An old OOM orphan must never become the replacement chest's contents. */
	for(struct inventory* prior = s->chest_views; prior; prior = prior->shared_next)
		if(prior->invalidated && server_local_windows_may_share_world(prior, inv)
		   && (half_at(prior, inv->x, inv->y, inv->z) >= 0
		   || (inv->has_partner && half_at(prior, inv->x2, inv->y2, inv->z2) >= 0)))
			return false;
	/* A second opener must see even a pending first view's contents. This also
	 * accommodates authoritative server writes made before a GUI transaction. */
	for(int half = 0; half < (inv->has_partner ? 2 : 1); half++) {
		w_coord_t x = half ? inv->x2 : inv->x;
		w_coord_t y = half ? inv->y2 : inv->y;
		w_coord_t z = half ? inv->z2 : inv->z;
		struct inventory* prior = chest_view_same_world(s, inv, x, y, z);
		if(prior) {
			int offset = half_at(prior, x, y, z);
			for(int k = 0; k < CHEST_SIZE_STORAGE; k++)
				inv->items[half * CHEST_SIZE_STORAGE + k] = prior->items[offset + k];
		}
	}
	inv->shared_next = s->chest_views;
	s->chest_views = inv;
	return true;
}

void inventory_chest_unregister(struct inventory* inv) {
	struct server_local* s = inv->user;
	if(!s) return; /* logic-less mirrors in focused tests */
	struct inventory** link = &s->chest_views;
	while(*link && *link != inv)
		link = &(*link)->shared_next;
	if(*link) *link = inv->shared_next;
	inv->shared_next = NULL;
}

void inventory_chest_publish(struct inventory* inv, set_inv_slot_t changes) {
	struct server_local* s = inv->user;
	if(!s) return;
	size_t storage = inv->logic == &inventory_logic_double_chest
		? IRON_CHEST_SIZE_STORAGE : CHEST_SIZE_STORAGE;
	set_inv_slot_it_t it;
	for(set_inv_slot_it(it, changes); !set_inv_slot_end_p(it); set_inv_slot_next(it)) {
		size_t slot = *set_inv_slot_ref(it);
		if(slot >= storage) continue;
		bool second = slot >= CHEST_SIZE_STORAGE;
		w_coord_t x = second ? inv->x2 : inv->x;
		w_coord_t y = second ? inv->y2 : inv->y;
		w_coord_t z = second ? inv->z2 : inv->z;
		size_t local = slot % CHEST_SIZE_STORAGE;
		/* A1b: write through to the row the window's OPENING world owns, and
		 * fan out only to views bound to that same world. */
		struct te_store st = server_local_window_tile_store(inv);
		int k = te_store_find(&st, TE_KIND_CHEST, x, y, z);
		if(k >= 0)
			s->chest_items[k][local] = inv->items[slot];
		for(struct inventory* peer = s->chest_views; peer; peer = peer->shared_next) {
			int offset = half_at(peer, x, y, z);
			if(peer == inv || offset < 0
			   || !server_local_windows_share_world(peer, inv)) continue;
			size_t target = (size_t)offset + local;
			peer->items[target] = inv->items[slot];
			server_local_window_reply(peer, &(struct client_rpc) {
				.type = CRPC_INVENTORY_SLOT,
				.payload.inventory_slot = {.window = window_id(peer),
					.slot = (uint8_t)target, .item = peer->items[target]},
			});
		}
	}
}

void inventory_chest_invalidate_peers(struct inventory* owner,
	w_coord_t x, w_coord_t y, w_coord_t z) {
	struct server_local* s = owner->user;
	if(!s) return;
	int owner_offset = half_at(owner, x, y, z);
	if(owner_offset < 0) return;
	owner->orphaned_chest_halves |= 1 << (owner_offset / CHEST_SIZE_STORAGE);
	for(struct inventory* peer = s->chest_views; peer; peer = peer->shared_next) {
		int offset = half_at(peer, x, y, z);
		/* A1b: removal invalidates only views of THIS world's chest. */
		if(offset < 0 || !server_local_windows_share_world(peer, owner))
			continue;
		peer->invalidated = true;
		peer->orphaned_chest_halves |= 1 << (offset / CHEST_SIZE_STORAGE);
		if(peer == owner) continue;
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++) {
			size_t slot = (size_t)offset + k;
			if(!peer->items[slot].id) continue;
			inventory_clear_slot(peer, slot);
			set_inv_slot_push(changes, slot);
		}
		server_local_send_inv_changes(changes, peer, window_id(peer));
		set_inv_slot_clear(changes);
	}
}
