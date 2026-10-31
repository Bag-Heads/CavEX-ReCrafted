#ifndef INVENTORY_CHEST_SHARED_H
#define INVENTORY_CHEST_SHARED_H

#include "../item/inventory.h"

struct server_local;
/* Wooden single/double chest views only. The list lives on the server thread;
 * callbacks never share player-side slots or cursors. */
bool inventory_chest_register(struct inventory* inv);
/* Server-thread only: requester must own this registered view; overlap of
 * either half counts, even if the peer opened a differently shaped view. */
bool inventory_chest_sort_admitted(const struct inventory* inv);
void inventory_chest_unregister(struct inventory* inv);
void inventory_chest_publish(struct inventory* inv, set_inv_slot_t changes);
struct inventory* inventory_chest_view_at(struct server_local* s,
	w_coord_t x, w_coord_t y, w_coord_t z);
/* A1b: world-qualified form (the un-suffixed one is the slot-0 adapter). */
struct server_world;
struct inventory* inventory_chest_view_at_w(struct server_local* s,
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z);
/* One view retains orphaned storage if the break's atomic spill fails. Every
 * other view loses that half and is invalidated, so close cannot spill twice. */
void inventory_chest_invalidate_peers(struct inventory* owner,
	w_coord_t x, w_coord_t y, w_coord_t z);

#endif
