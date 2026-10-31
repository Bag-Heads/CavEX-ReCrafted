/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

// Beta ContainerDispenser (decompile 740c583): a 3x3 (9-slot) storage grid,
// slots 0..8, plus the standard player main(27)/hotbar(9) rows -- added in
// EXACTLY that order (storage, main, hotbar), matching this engine's existing
// DISPENSER_SLOT_STORAGE(0)/_MAIN(9)/_HOTBAR(36) window convention already
// used by chest/furnace. Mirrors inventory_furnace.c's LIVE-SYNC shape (not
// chest's close-only sync): the dispenser's own redstone-driven dispenseItem
// reads server_dispenser.c's dispenser_te directly, so a player edit made
// while the GUI is open must reach the TE promptly, not only at close.

#include <string.h>

#include "../block/dispenser_logic.h"
#include "../item/window_container.h"
#include "inventory_logic.h"
#include "inventory_achievements.h"
#include "server_dispenser.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

/* A1b: resolved in the window's opening world (its binding). */
static struct dispenser_te* dispenser_backing(const struct inventory* inv,
											   w_coord_t x, w_coord_t y,
											   w_coord_t z) {
	struct server_world* w = server_local_window_world(inv);
	struct block_data block;
	if(!w || !server_world_get_block(w, x, y, z, &block)
	   || block.type != BLOCK_DISPENSER)
		return NULL;
	struct te_store st = server_local_window_tile_store(inv);
	int i = te_store_find(&st, TE_KIND_DISPENSER, x, y, z);
	return i < 0 ? NULL : &st.dispensers[i];
}

/* The window's TE row in its own world, or -1 (no row / no live world). */
static int dispenser_te_row(const struct inventory* inv) {
	if(!server_local_window_world(inv))
		return -1;
	struct te_store st = server_local_window_tile_store(inv);
	return te_store_find(&st, TE_KIND_DISPENSER, inv->x, inv->y, inv->z);
}

/* Does this window OWN its storage copy on close? Only when the tile is gone
 * AND a break path handed the contents to the window (the spill helpers mark
 * such a window invalidated before spawning, and on their failure the close
 * retries the orphan batch). A copy whose TE row is still retained (a failed
 * scatter the orphan sweep will finish) or whose row was released by some
 * other path without handing it over is NOT the owner: discarding it is the
 * only lossless choice for a shared tile (C2 tiles, Fable H-1 / F-A2). */
static bool dispenser_window_owns_storage(const struct inventory* inv) {
	return inv->invalidated && dispenser_te_row(inv) < 0;
}

static void dispenser_window_to_te(struct inventory* inv) {
	struct dispenser_te* d = dispenser_backing(inv, inv->x, inv->y, inv->z);
	if(!d)
		return;
	for(int k = 0; k < DISPENSER_TE_SLOTS; k++) {
		struct item_data it;
		d->items[k] = inventory_get_slot(inv, DISPENSER_SLOT_STORAGE + k, &it)
			? it
			: (struct item_data) {0};
	}
}

/* C2 tiles (Fable H-1): ONE backing inventory per dispenser, however many
 * windows view it -- Beta's ContainerDispenser slots all reference the same
 * TileEntityDispenser (ContainerDispenser.<init>). The TE is the single
 * source of truth: a click refreshes the window's storage from it first,
 * every processed click writes all nine slots through and publishes them to
 * every other window on the same tile in the same world, and a close writes
 * nothing back. */
static bool dispenser_same_tile(const struct inventory* a,
								const struct inventory* b) {
	return a != b && b->logic == &inventory_logic_dispenser && !b->invalidated
		&& a->x == b->x && a->y == b->y && a->z == b->z
		&& server_local_windows_share_world(a, b);
}

static void dispenser_te_to_window(struct inventory* inv,
								   const struct dispenser_te* d,
								   set_inv_slot_t changes) {
	for(int k = 0; k < DISPENSER_TE_SLOTS; k++) {
		size_t w = DISPENSER_SLOT_STORAGE + (size_t)k;
		if(memcmp(&inv->items[w], &d->items[k], sizeof(struct item_data))) {
			inv->items[w] = d->items[k];
			if(changes)
				set_inv_slot_push(changes, w);
		}
	}
}

static void dispenser_publish_peers(struct inventory* inv,
									const struct dispenser_te* d) {
	struct server_local* s = inv->user;
	for(unsigned i = 0; s && i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		struct inventory* peer = body ? body->active_inventory : NULL;
		if(!peer || peer == &body->inventory || !dispenser_same_tile(inv, peer))
			continue;
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		dispenser_te_to_window(peer, d, changes);
		server_local_send_inv_changes(changes, peer, WINDOWC_DISPENSER);
		set_inv_slot_clear(changes);
	}
}

/* C2 tiles (Fable A-H1/A-M1): the dispenser twin of
 * inventory_furnace_refresh_window -- called by the SRPC_WINDOW_CLICK
 * handler before `returned` is captured and before either branch. Declared
 * by the handoff v3 header hunk (inventory_logic.h). */
bool inventory_dispenser_refresh_window(struct inventory* inv,
										set_inv_slot_t changes) {
	if(!inv || inv->logic != &inventory_logic_dispenser)
		return false;
	struct dispenser_te* d = dispenser_backing(inv, inv->x, inv->y, inv->z);
	if(!d)
		return false;
	dispenser_te_to_window(inv, d, changes);
	return true;
}

static bool inv_pre_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
						   set_inv_slot_t changes) {
	/* C2 tiles: a window whose opening world instance is gone (bound, but
	 * the slot now holds another generation or none) has no backing tile --
	 * Beta's TileEntity.canInteractWith fails (getBlockTileEntity != this),
	 * so the Container is unusable: refuse the click, mutate nothing. */
	if(inv->user && inv->owner && inv->owner->window_world.generation != 0
	   && !server_local_window_world(inv))
		return false;
	/* No live backing dispenser: the storage copy is not manipulable. */
	struct dispenser_te* d = dispenser_backing(inv, inv->x, inv->y, inv->z);
	if(!d && inv->owner && inv->owner->window_world.generation != 0)
		return false; /* bound (real) windows only; hand-built views keep the old path */
	if(d)
		dispenser_te_to_window(inv, d, changes);
	(void)slot;
	(void)right;
	// Beta's Slot for a dispenser's 9 storage cells carries no isItemValid
	// override (unlike SlotFurnace's output-only restriction) -- any item may
	// be placed in any of the 9 slots.
	return true;
}

static void inv_post_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
							bool accepted, set_inv_slot_t changes) {
	(void)slot;
	(void)right;
	(void)accepted;
	(void)changes;
	/* All nine slots whatever slot was clicked (a Quick Move names its source
	 * row). The player's edit -> the authoritative TE -> every peer window. */
	struct dispenser_te* d = dispenser_backing(inv, inv->x, inv->y, inv->z);
	if(!d)
		return;
	dispenser_window_to_te(inv);
	dispenser_publish_peers(inv, d);
}

static bool inv_on_close(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	struct item_data picked_item;
	bool has_picked_item = inventory_get_picked_item(inv, &picked_item);
	/* F-A2: a window bound to a world instance that is GONE is not an
	 * orphan -- its dispenser was checkpointed with that world: discard the
	 * storage copy, drop only the cursor. */
	bool stale = inv->owner && inv->owner->window_world.generation != 0
		&& !server_local_window_world(inv);
	bool orphan = !stale && !dispenser_backing(inv, inv->x, inv->y, inv->z);
	if(orphan && dispenser_window_owns_storage(inv)) {
		// The block can disappear under an open GUI (explosion/support update).
		// Its onBreak path deliberately leaves the live window as owner (see
		// block_dispenser.c); atomically spill the orphaned storage slots now
		// instead of pinning the window or silently destroying them.
		struct item_data drops[DISPENSER_TE_SLOTS + 1];
		struct death_drop_provenance provenance[DISPENSER_TE_SLOTS + 1];
		size_t drop_count = 0;
		for(int k = 0; k < DISPENSER_TE_SLOTS; k++)
			if(inv->items[DISPENSER_SLOT_STORAGE + k].id != 0) {
				drops[drop_count] = inv->items[DISPENSER_SLOT_STORAGE + k];
				provenance[drop_count++] = (struct death_drop_provenance) {
					.kind = DEATH_DROP_OWNER_CONTAINER_STORAGE,
					.window = WINDOWC_DISPENSER,
					.slot = (int16_t)(DISPENSER_SLOT_STORAGE + k),
					.owner_slot = (int16_t)k,
					.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
				};
			}
		size_t storage_count = drop_count;
		if(has_picked_item) {
			drops[drop_count] = picked_item;
			provenance[drop_count++] = (struct death_drop_provenance) {
				.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
				.window = WINDOWC_DISPENSER,
				.slot = SPECIAL_SLOT_PICKED_ITEM,
				.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
			};
		}
		if(drop_count > 0) {
			bool spawned = server_local_spawn_sourced_item_batch_for(
				(vec3) {player->x, player->y, player->z}, drops,
				provenance, drop_count, true, storage_count > 0, s, player);
			if(!spawned)
				return false;
		}

		set_inv_slot_t orphan_changes;
		set_inv_slot_init(orphan_changes);
		for(int k = 0; k < DISPENSER_TE_SLOTS; k++)
			if(inv->items[DISPENSER_SLOT_STORAGE + k].id != 0) {
				inventory_clear_slot(inv, DISPENSER_SLOT_STORAGE + k);
				set_inv_slot_push(orphan_changes, DISPENSER_SLOT_STORAGE + k);
			}
		if(has_picked_item) {
			inventory_clear_picked_item(inv);
			set_inv_slot_push(orphan_changes, SPECIAL_SLOT_PICKED_ITEM);
		}
		server_local_send_inv_changes(orphan_changes, inv, WINDOWC_DISPENSER);
		set_inv_slot_clear(orphan_changes);
		inventory_destroy_owned(inv, player);
		return true;
	}
	if(has_picked_item) {
		struct death_drop_provenance source = {
			.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
			.window = WINDOWC_DISPENSER,
			.slot = SPECIAL_SLOT_PICKED_ITEM,
			.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
		};
		if(!server_local_spawn_sourced_item_batch_for(
			   (vec3) {player->x, player->y, player->z},
			   &picked_item, &source, 1, true, false, s, player))
			return false;
	}

	// The 9 storage slots belong to the DISPENSER (the tile entity), not the
	// player -- they stay in the block when the GUI closes. Every edit already
	// reached the TE (post_action writes through), so the close writes NOTHING
	// back: this copy may be older than the TE (a peer's click, or a dispense
	// that pushed only the host's window) and flushing it would roll the
	// dispenser back -- the two-window duplication (Fable H-1). Only the
	// cursor's picked item is the player's.

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	if(has_picked_item) {
		inventory_clear_picked_item(inv);
		set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
	}

	server_local_send_inv_changes(changes, inv, WINDOWC_DISPENSER);
	set_inv_slot_clear(changes);

	inventory_destroy_owned(inv, player);
	return true;
}

static bool inv_on_collect(struct inventory* inv, struct server_player* player, struct item_data* item) {
	uint8_t priorities[INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN];

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		priorities[k] = k + DISPENSER_SLOT_HOTBAR;

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		priorities[k + INVENTORY_SIZE_HOTBAR] = k + DISPENSER_SLOT_MAIN;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	/* Beta RetroMCP EntityItem.java:110-119: save id/count before drain. */
	struct item_data before = *item;
	bool success
		= inventory_collect(inv, item, priorities,
							sizeof(priorities) / sizeof(*priorities), changes);
	server_local_send_inv_changes(changes, inv, WINDOWC_DISPENSER);
	set_inv_slot_clear(changes);

	if(success) inventory_achievement_collected(inv, player, before);
	return success;
}

static bool inv_on_create(struct inventory* inv, struct server_player* player) {
	struct dispenser_te* d = dispenser_backing(inv, inv->x, inv->y, inv->z);
	if(!d)
		return false;

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		inv->items[k + DISPENSER_SLOT_HOTBAR]
			= player->inventory.items[k + INVENTORY_SLOT_HOTBAR];
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		inv->items[k + DISPENSER_SLOT_MAIN]
			= player->inventory.items[k + INVENTORY_SLOT_MAIN];
	}

	// Seed the 9 storage slots from the position's dispenser tile entity so
	// the GUI opens showing its actual contents (onRightClick registered the
	// TE before creating this window).
	for(int k = 0; k < DISPENSER_TE_SLOTS; k++)
		inv->items[DISPENSER_SLOT_STORAGE + k] = d->items[k];
	return true;
}

static bool inv_on_destroy(struct inventory* inv, struct server_player* player) {
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		player->inventory.items[k + INVENTORY_SLOT_HOTBAR]
			= inv->items[k + DISPENSER_SLOT_HOTBAR];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_HOTBAR);
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		player->inventory.items[k + INVENTORY_SLOT_MAIN]
			= inv->items[k + DISPENSER_SLOT_MAIN];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_MAIN);
	}

	server_local_send_inv_changes(changes, &player->inventory,
								  WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);
	return true;
}

struct inventory_logic inventory_logic_dispenser = {
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = inv_on_create,
	.on_destroy = inv_on_destroy,
	.on_close = inv_on_close,
};
