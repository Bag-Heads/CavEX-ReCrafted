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

#include "../block/furnace_logic.h"
#include "../item/inventory_click.h"
#include "../item/items.h" // direct include (was transitive via inventory.h pre-split)
#include "../item/recipe.h"
#include "../item/window_container.h"
#include <string.h>

#include "inventory_logic.h"
#include "inventory_achievements.h"
#include "server_furnace.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

// Window slots 0/1/2 (input/fuel/output) are a live mirror of the position's furnace
// tile entity (server_furnace.c). The per-tick furnace_tick does the smelting; here we
// only (a) keep the output take-only and (b) write the player's edits back into the TE
// so the next tick sees them.
static const int FURNACE_WSLOT[3]
	= {FURNACE_SLOT_INPUT, FURNACE_SLOT_FUEL, FURNACE_SLOT_OUTPUT};

/* A1b: resolved in the window's opening world (its binding). */
static struct furnace_te* furnace_backing(const struct inventory* inv,
										w_coord_t x, w_coord_t y,
										w_coord_t z) {
	struct server_world* w = server_local_window_world(inv);
	struct block_data block;
	if(!w || !server_world_get_block(w, x, y, z, &block)
	   || (block.type != BLOCK_FURNACE && block.type != BLOCK_FURNACE_LIT))
		return NULL;
	struct te_store st = server_local_window_tile_store(inv);
	int i = te_store_find(&st, TE_KIND_FURNACE, x, y, z);
	return i < 0 ? NULL : &st.furnaces[i];
}

/* The window's TE row in its own world, or -1 (no row / no live world). */
static int furnace_te_row(const struct inventory* inv) {
	if(!server_local_window_world(inv))
		return -1;
	struct te_store st = server_local_window_tile_store(inv);
	return te_store_find(&st, TE_KIND_FURNACE, inv->x, inv->y, inv->z);
}

/* Does this window OWN its storage copy on close? Only when the tile is gone
 * AND a break path handed the contents to the window (the spill helpers mark
 * such a window invalidated before spawning, and on their failure the close
 * retries the orphan batch). A copy whose TE row is still retained (a failed
 * scatter the orphan sweep will finish) or whose row was released by some
 * other path without handing it over is NOT the owner: discarding it is the
 * only lossless choice for a shared tile (C2 tiles, Fable H-1 / F-A2). */
static bool furnace_window_owns_storage(const struct inventory* inv) {
	return inv->invalidated && furnace_te_row(inv) < 0;
}

static void furnace_window_to_te(struct inventory* inv) {
	struct furnace_te* f = furnace_backing(inv, inv->x, inv->y, inv->z);
	if(!f)
		return;
	for(int k = 0; k < 3; k++) {
		struct item_data it;
		f->items[k] = inventory_get_slot(inv, FURNACE_WSLOT[k], &it)
			? it
			: (struct item_data) {0};
	}
}

/* C2 tiles (Fable H-1): ONE backing inventory per furnace, however many
 * windows view it. Beta aliases it outright -- every ContainerFurnace's slots
 * reference the same TileEntityFurnace (ContainerFurnace.<init>: `new
 * Slot(var2, 0, ...)` over the tile itself). Here each window is a copy, so
 * the TE is kept the single source of truth: a click first REFRESHES the
 * window's storage from the TE (a copy made stale by a peer's click or a
 * smelt tick can never write back), every processed click WRITES all three
 * slots through and PUBLISHES them to every other window on the same tile in
 * the same world, and a close writes nothing back. */
static bool furnace_same_tile(const struct inventory* a, const struct inventory* b) {
	return a != b && b->logic == &inventory_logic_furnace && !b->invalidated
		&& a->x == b->x && a->y == b->y && a->z == b->z
		&& server_local_windows_share_world(a, b);
}

static void furnace_te_to_window(struct inventory* inv,
								 const struct furnace_te* f,
								 set_inv_slot_t changes) {
	for(int k = 0; k < 3; k++)
		if(memcmp(&inv->items[FURNACE_WSLOT[k]], &f->items[k],
				  sizeof(struct item_data))) {
			inv->items[FURNACE_WSLOT[k]] = f->items[k];
			if(changes)
				set_inv_slot_push(changes, FURNACE_WSLOT[k]);
		}
}

static void furnace_publish_peers(struct inventory* inv,
								  const struct furnace_te* f) {
	struct server_local* s = inv->user;
	for(unsigned i = 0; s && i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		struct inventory* peer = body ? body->active_inventory : NULL;
		if(!peer || peer == &body->inventory || !furnace_same_tile(inv, peer))
			continue;
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		furnace_te_to_window(peer, f, changes);
		server_local_send_inv_changes(changes, peer, WINDOWC_FURNACE);
		set_inv_slot_clear(changes);
	}
}

/* C2 tiles (Fable A-H1/A-M1): refresh a furnace window's storage from its
 * backing TE and record the changed slots. The SRPC_WINDOW_CLICK handler
 * calls it BEFORE it captures the pre-click `returned` stack and before
 * either branch (ordinary click or Quick Move, which never runs pre_action),
 * so the transaction compares against the TRUE pre-click stack (Beta
 * NetServerHandler.handleWindowClick compares against the server's own
 * slot) and a Quick Move can never write a stale copy back. No backing: no
 * change (the caller's own gates refuse). Declared by the handoff v3 header
 * hunk (inventory_logic.h). */
bool inventory_furnace_refresh_window(struct inventory* inv,
									  set_inv_slot_t changes) {
	if(!inv || inv->logic != &inventory_logic_furnace)
		return false;
	struct furnace_te* f = furnace_backing(inv, inv->x, inv->y, inv->z);
	if(!f)
		return false;
	furnace_te_to_window(inv, f, changes);
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
	/* No live backing furnace (block gone, TE retained or released): the
	 * storage copy is not manipulable -- Beta's Container stops being usable
	 * once canInteractWith fails. */
	struct furnace_te* f = furnace_backing(inv, inv->x, inv->y, inv->z);
	if(!f && inv->owner && inv->owner->window_world.generation != 0)
		return false; /* bound (real) windows only; hand-built views keep the old path */
	if(f)
		furnace_te_to_window(inv, f, changes);
	struct item_data before = inv->items[slot];
	unsigned cursor_count = inv->picked_item.count;
	bool ordinary = inventory_click_pre(inv, WINDOW_TYPE_FURNACE, slot, right, changes, NULL);
	/* Beta RetroMCP SlotFurnace.java:15-25 / Container.java:143:
	 * occupied compatible cursor takes the entire result in pre_action.
	 * NULL server above is intentional; inv->owner identifies the actor. */
	if(!ordinary && inv->picked_item.id == before.id
	   && inv->picked_item.count > cursor_count && player == inv->owner)
		inventory_achievement_furnace_taken(inv, slot, before);
	return ordinary;
}

static void inv_post_action(struct inventory* inv, struct server_player* player, size_t slot, bool right,
							bool accepted, set_inv_slot_t changes) {
	(void)slot;
	(void)right;
	(void)accepted;
	(void)changes;
	/* All three slots, whatever slot was clicked: a Quick Move from the
	 * player's own rows lands in the input/fuel slot while `slot` names the
	 * source row. The player's edit -> the authoritative TE -> every peer. */
	struct furnace_te* f = furnace_backing(inv, inv->x, inv->y, inv->z);
	if(!f)
		return;
	furnace_window_to_te(inv);
	furnace_publish_peers(inv, f);
}

static bool inv_on_close(struct inventory* inv, struct server_player* player) {
	struct server_local* s = inv->user;
	struct item_data picked_item;
	bool has_picked_item = inventory_get_picked_item(inv, &picked_item);
	/* F-A2: a window bound to a world instance that is GONE (stale
	 * generation) is not an orphan: its furnace was checkpointed with that
	 * world, so the storage slots are discarded (Beta closeScreen leaves them
	 * in the TileEntity) and only the player's cursor drops. */
	bool stale = inv->owner && inv->owner->window_world.generation != 0
		&& !server_local_window_world(inv);
	bool orphan = !stale && !furnace_backing(inv, inv->x, inv->y, inv->z);
	if(orphan && furnace_window_owns_storage(inv)) {
		struct item_data drops[4];
		struct death_drop_provenance provenance[4];
		size_t drop_count = 0;
		for(int k = 0; k < 3; k++)
			if(inv->items[FURNACE_WSLOT[k]].id != 0) {
				drops[drop_count] = inv->items[FURNACE_WSLOT[k]];
				provenance[drop_count++] = (struct death_drop_provenance) {
					.kind = DEATH_DROP_OWNER_CONTAINER_STORAGE,
					.window = WINDOWC_FURNACE,
					.slot = FURNACE_WSLOT[k],
					.owner_slot = FURNACE_WSLOT[k],
					.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
				};
			}
		size_t storage_count = drop_count;
		if(has_picked_item) {
			drops[drop_count] = picked_item;
			provenance[drop_count++] = (struct death_drop_provenance) {
				.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
				.window = WINDOWC_FURNACE,
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
		for(int k = 0; k < 3; k++)
			if(inv->items[FURNACE_WSLOT[k]].id != 0) {
				inventory_clear_slot(inv, FURNACE_WSLOT[k]);
				set_inv_slot_push(orphan_changes, FURNACE_WSLOT[k]);
			}
		if(has_picked_item) {
			inventory_clear_picked_item(inv);
			set_inv_slot_push(orphan_changes, SPECIAL_SLOT_PICKED_ITEM);
		}
		server_local_send_inv_changes(orphan_changes, inv, WINDOWC_FURNACE);
		set_inv_slot_clear(orphan_changes);
		inventory_destroy_owned(inv, player);
		return true;
	}
	if(has_picked_item) {
		struct death_drop_provenance source = {
			.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR,
			.window = WINDOWC_FURNACE,
			.slot = SPECIAL_SLOT_PICKED_ITEM,
			.owner_x = inv->x, .owner_y = inv->y, .owner_z = inv->z,
		};
		if(!server_local_spawn_sourced_item_batch_for(
			   (vec3) {player->x, player->y, player->z},
			   &picked_item, &source, 1, true, false, s, player))
			return false;
	}

	// The input/fuel/output stacks belong to the FURNACE (the tile entity), not the
	// player -- they stay in the block when the GUI closes. Every edit already
	// reached the TE (post_action writes through all three slots), so the close
	// writes NOTHING back: this copy may be older than the TE (a peer window's
	// click, or a smelt tick that pushed only the host's window), and flushing
	// it would roll the furnace back -- the two-window duplication (Fable H-1).
	// Only the cursor's picked item is the player's.

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	if(has_picked_item) {
		inventory_clear_picked_item(inv);
		set_inv_slot_push(changes, SPECIAL_SLOT_PICKED_ITEM);
	}

	server_local_send_inv_changes(changes, inv, WINDOWC_FURNACE);
	set_inv_slot_clear(changes);

	inventory_destroy_owned(inv, player);
	return true;
}

static bool inv_on_collect(struct inventory* inv, struct server_player* player, struct item_data* item) {
	uint8_t priorities[INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN];

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
		priorities[k] = k + FURNACE_SLOT_HOTBAR;

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++)
		priorities[k + INVENTORY_SIZE_HOTBAR] = k + FURNACE_SLOT_MAIN;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	/* Beta RetroMCP EntityItem.java:110-119: save id/count before drain. */
	struct item_data before = *item;
	bool success
		= inventory_collect(inv, item, priorities,
							sizeof(priorities) / sizeof(*priorities), changes);
	server_local_send_inv_changes(changes, inv, WINDOWC_FURNACE);
	set_inv_slot_clear(changes);

	if(success) inventory_achievement_collected(inv, player, before);
	return success;
}

static bool inv_on_create(struct inventory* inv, struct server_player* player) {
	struct furnace_te* f = furnace_backing(inv, inv->x, inv->y, inv->z);
	if(!f)
		return false;

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		inv->items[k + FURNACE_SLOT_HOTBAR]
			= player->inventory.items[k + INVENTORY_SLOT_HOTBAR];
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		inv->items[k + FURNACE_SLOT_MAIN]
			= player->inventory.items[k + INVENTORY_SLOT_MAIN];
	}

	// Seed slots 0/1/2 from the position's furnace tile entity so the GUI opens
	// showing the furnace's actual input/fuel/output (onRightClick registered the
	// TE before creating this window). furnace_server_tick keeps them live after.
	for(int k = 0; k < 3; k++)
		inv->items[FURNACE_WSLOT[k]] = f->items[k];
	return true;
}

static bool inv_on_destroy(struct inventory* inv, struct server_player* player) {
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		player->inventory.items[k + INVENTORY_SLOT_HOTBAR]
			= inv->items[k + FURNACE_SLOT_HOTBAR];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_HOTBAR);
	}

	for(size_t k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		player->inventory.items[k + INVENTORY_SLOT_MAIN]
			= inv->items[k + FURNACE_SLOT_MAIN];
		set_inv_slot_push(changes, k + INVENTORY_SLOT_MAIN);
	}

	server_local_send_inv_changes(changes, &player->inventory,
								  WINDOWC_INVENTORY);
	set_inv_slot_clear(changes);
	return true;
}

struct inventory_logic inventory_logic_furnace = {
	.on_result_taken = inventory_achievement_furnace_taken,
	.pre_action = inv_pre_action,
	.post_action = inv_post_action,
	.on_collect = inv_on_collect,
	.on_create = inv_on_create,
	.on_destroy = inv_on_destroy,
	.on_close = inv_on_close,
};
