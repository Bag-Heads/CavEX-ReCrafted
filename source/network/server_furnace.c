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

#include <stdlib.h>
#include <string.h>

#include "../block/blocks.h"
#include "../block/furnace_logic.h"
#include "../block/furnace_recipes.h"
#include "../item/window_container.h"
#include "client_interface.h"
#include "inventory_logic.h"
#include "server_furnace.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

// --- the production furnace_env (recipe + Beta getItemBurnTime + max-stack) -------

// Beta TileEntityFurnace.getItemBurnTime (decompile 740c583), in TICKS.
static int env_fuel_ticks(struct item_data it) {
	if(it.count == 0)
		return 0;
	// id < 256 && block material == wood  -> 300 (planks, log, fence, chest, ...)
	if(it.id < 256 && blocks[it.id] && blocks[it.id]->getMaterial) {
		struct block_data bd = {.type = it.id};
		struct block_info bi = {.block = &bd};
		if(blocks[it.id]->getMaterial(&bi) == MATERIAL_WOOD)
			return 300;
	}
	switch(it.id) {
		case ITEM_STICK: return 100;
		case ITEM_COAL: return 1600;
		case ITEM_BUCKET_LAVA: return 20000;
		case BLOCK_SAPLING: return 100;
		default: return 0;
	}
}

static int env_max_stack(struct item_data it) {
	struct item* t = item_get(&it);
	return t ? t->max_stack : 64;
}

static const struct furnace_env ENV = {
	.fuel_ticks = env_fuel_ticks,
	.smelt_result = furnace_smelt_result,
	.max_stack = env_max_stack,
};
const struct furnace_env* furnace_server_env(void) {
	return &ENV;
}

// --- position-keyed TE table (mirrors the chest table) ---------------------------
// A1b: every operation names its world; the shared table's rows are filtered
// by owner (server_world_tile_store). Slot choice (first match, else first
// free) is unchanged, so a single world assigns exactly the rows it did.

static int furnace_te_register_st(struct te_store st, w_coord_t x, w_coord_t y,
						  w_coord_t z) {
	bool existed;
	int i = te_store_claim(&st, TE_KIND_FURNACE, x, y, z, &existed);
	if(i < 0) {
		puts("Too many furnaces (timed-furnace state dropped)");
		return -1;
	}
	if(!existed)
		st.furnaces[i] = (struct furnace_te) {0};
	return i;
}

int furnace_te_register_w(struct server_world* w, w_coord_t x, w_coord_t y,
						  w_coord_t z) {
	return furnace_te_register_st(server_world_tile_store(w), x, y, z);
}

static struct furnace_te* furnace_te_lookup_st(struct te_store st, w_coord_t x,
									   w_coord_t y, w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_FURNACE, x, y, z);
	return i < 0 ? NULL : &st.furnaces[i];
}

struct furnace_te* furnace_te_lookup_w(struct server_world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z) {
	return furnace_te_lookup_st(server_world_tile_store(w), x, y, z);
}

static void furnace_te_free_st(struct te_store st, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_FURNACE, x, y, z);
	if(i < 0)
		return;
	te_store_free_row(&st, TE_KIND_FURNACE, i);
	st.furnaces[i] = (struct furnace_te) {0};
}

void furnace_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	furnace_te_free_st(server_world_tile_store(w), x, y, z);
}

/* LEGACY slot-0 adapters (A1b inventory; consumers: tests, D4 bridge). */
int furnace_te_register(struct server_local* s, w_coord_t x, w_coord_t y,
						w_coord_t z) {
	return furnace_te_register_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

struct furnace_te* furnace_te_lookup(struct server_local* s, w_coord_t x,
									 w_coord_t y, w_coord_t z) {
	return furnace_te_lookup_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

void furnace_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					 w_coord_t z) {
	furnace_te_free_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

// --- per-tick driver --------------------------------------------------------------

// Push furnace_te.items[0..2] into an open furnace window at this pos + send diffs,
// so live smelting (output appearing, fuel/input shrinking) is visible.
static void furnace_push_window(struct server_local* s,
								struct server_player* body,
								struct server_world* w, w_coord_t x,
								w_coord_t y, w_coord_t z,
								const struct furnace_te* f) {
	struct inventory* inv = body->active_inventory;
	if(!inv || inv == &body->inventory
	   || inv->logic != &inventory_logic_furnace || inv->invalidated)
		return;
	if(inv->x != x || inv->y != y || inv->z != z)
		return;
	/* A1b: a window opened in the other world at the same coordinates is not
	 * this furnace's window. */
	if(!server_local_window_bound_to(inv, w))
		return;

	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	const int wslot[3]
		= {FURNACE_SLOT_INPUT, FURNACE_SLOT_FUEL, FURNACE_SLOT_OUTPUT};
	bool any = false;
	for(int k = 0; k < 3; k++) {
		if(memcmp(&inv->items[wslot[k]], &f->items[k], sizeof(struct item_data))
		   != 0) {
			inv->items[wslot[k]] = f->items[k];
			set_inv_slot_push(changes, wslot[k]);
			any = true;
		}
	}
	if(any)
		server_local_send_inv_changes(changes, inv, WINDOWC_FURNACE);
	set_inv_slot_clear(changes);

	// Push the burn/cook progress for the flame + cook arrow (Beta
	// sendProgressBarUpdate). DISPLAY-ONLY; sent each tick while the furnace GUI is
	// open (bounded to that window, ~20/s of three uint16) so the client always has
	// the current values without tracking transitions -- no staleness, no missed edge.
	server_local_window_reply(inv, &(struct client_rpc) {
		.type = CRPC_WINDOW_PROGRESS,
		.payload.window_progress.window = WINDOWC_FURNACE,
		.payload.window_progress.cook_time = (uint16_t)f->cook_time,
		.payload.window_progress.burn_time = (uint16_t)f->burn_time,
		.payload.window_progress.current_item_burn_time
		= (uint16_t)f->current_item_burn_time,
	});
}

/* C2 tiles: every present body's furnace window on THIS furnace (bound to
 * this world) sees the live smelting -- Beta ContainerFurnace.
 * updateCraftingResults runs per crafter. Host first, then the guest
 * (server_local_damage_candidate order). The progress bar reaches each
 * window's own route (server_local_window_reply). */
static void furnace_push_open_window(struct server_local* s,
									 struct server_world* w, w_coord_t x,
									 w_coord_t y, w_coord_t z,
									 const struct furnace_te* f) {
	(void)s;
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		if(body)
			furnace_push_window(s, body, w, x, y, z, f);
	}
}

// Block -> chunk coordinate, floor division (correct for negatives). 16 == CHUNK_SIZE;
// inlined (like tile_entities_store.c) so this needn't pull chunk.h's display headers.
static int furnace_chunk_of(w_coord_t b) {
	return b < 0 ? (b + 1) / 16 - 1 : b / 16;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* hw1029c test seam: CW_TEST_FURNACE_LOAD_FIXUP=1 restores the pre-10-29
 * block-id normalisation on load (below) so tests can prove the client relight
 * stays bounded even under that SET_BLOCK storm. Never on in production. */
static int s_test_force_block_fixup = -1;
static bool furnace_test_force_block_fixup(void) {
	if(s_test_force_block_fixup < 0) {
		const char* e = getenv("CW_TEST_FURNACE_LOAD_FIXUP");
		s_test_force_block_fixup = e && !strcmp(e, "1");
	}
	return s_test_force_block_fixup == 1;
}
#endif

void furnace_server_load_fixup_w(struct server_world* w, w_coord_t cx,
								 w_coord_t cz) {
	struct te_store st = server_world_tile_store(w);
	for(int i = 0; i < st.max_furnace; i++) {
		if(!te_store_row_owned(&st, TE_KIND_FURNACE, i))
			continue;
		w_coord_t x = st.furnace_pos[i].x, y = st.furnace_pos[i].y,
				  z = st.furnace_pos[i].z;
		if(furnace_chunk_of(x) != cx || furnace_chunk_of(z) != cz)
			continue;
		struct furnace_te* f = &st.furnaces[i];

		// Beta readFromNBT [740c583 TileEntityFurnace.java:89-91]:
		// currentItemBurnTime = getItemBurnTime(fuel stack). That is ALL a load
		// does to a furnace. The block id is never touched on load: the only
		// Beta writer of stoneOvenIdle/Active is updateFurnaceBlockState, called
		// from updateEntity [:146-148] when `burnTime > 0` CHANGES during a tick.
		// A furnace saved as 62 with BurnTime 0 (Dream Journal's y=0 light
		// sources, 365 of them) therefore stays lit forever in Beta, and our
		// furnace_tick mirrors that (was_burning == is_burning -> no toggle).
		// hw1029c: the former "normalize idle/lit block id on load" here was
		// purely defensive (e95a9c4b9: "new saves are already consistent"),
		// not a cover for any CaveWii save bug -- block and TE are saved from
		// the same in-memory state. On a real Beta world it rewrote every such
		// furnace (62->61, waking the still water above it) during the load
		// stream, seeding the client relight storm behind the far_walk heap OOM.
		f->current_item_burn_time = env_fuel_ticks(f->items[FURNACE_SLOT_FUEL_TE]);

#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(furnace_test_force_block_fixup()) {
			struct block_data blk;
			if(server_world_get_block(w, x, y, z, &blk)
			   && (blk.type == BLOCK_FURNACE || blk.type == BLOCK_FURNACE_LIT)) {
				uint8_t want
					= furnace_is_burning(f) ? BLOCK_FURNACE_LIT : BLOCK_FURNACE;
				if(blk.type != want) {
					blk.type = want; // facing metadata untouched
					server_world_set_block(w, x, y, z, blk);
					if(server_world_light_halted(w))
						return; /* G9 R3: a light stop ends the loop at once */
				}
			}
		}
#endif
	}
}

/* IMP-TE-CAPS: is any body's furnace window (bound to this world) open?
 * Exactly furnace_push_window's gate minus the position test. */
static bool furnace_any_window_open(struct server_local* s,
									struct server_world* w) {
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		if(!body)
			continue;
		struct inventory* inv = body->active_inventory;
		if(inv && inv != &body->inventory
		   && inv->logic == &inventory_logic_furnace && !inv->invalidated
		   && server_local_window_bound_to(inv, w))
			return true;
	}
	return false;
}

void furnace_server_tick_w(struct server_world* w) {
	struct server_local* s = w ? w->session : NULL;
	if(!s)
		return;
	struct te_store st = server_world_tile_store(w);
	/* IMP-TE-CAPS T1 dormant skip (golden: test_furnace_burn.c
	 * dormant_skip_*): an idle furnace's tick is an exact fixed point
	 * (furnace_tick_is_idle), so with no furnace window open -- the only other
	 * per-furnace effect of this loop, the progress push -- skipping it (and
	 * its block read) is byte-identical to ticking it. Idleness is decided
	 * afresh every tick, so a window edit wakes a furnace on the next tick.
	 * While any furnace window is open nothing is skipped. */
	bool skip_idle = !furnace_any_window_open(s, w);
	for(int i = 0; i < st.max_furnace; i++) {
		/* G9 R3 (ASTRA-G9-REVIEW-2 item 1): a light stop -- this furnace
		 * ignition write's own failed reservation, or any earlier stop --
		 * ends the pass: no later furnace advances, no window is pushed. */
		if(server_world_light_halted(w))
			return;
		if(!te_store_row_owned(&st, TE_KIND_FURNACE, i))
			continue;
		if(skip_idle && furnace_tick_is_idle(&st.furnaces[i], &ENV))
			continue;
		w_coord_t x = st.furnace_pos[i].x, y = st.furnace_pos[i].y,
				  z = st.furnace_pos[i].z;
		struct furnace_te* f = &st.furnaces[i];
		// A broken furnace whose atomic content spill hit OOM deliberately
		// retains this TE as the no-loss owner until the retry succeeds. It is
		// not a live furnace anymore: freeze burn/cook state instead of consuming
		// fuel or producing output in an air block.
		struct block_data blk;
		if(!server_world_get_block(w, x, y, z, &blk)
		   || (blk.type != BLOCK_FURNACE && blk.type != BLOCK_FURNACE_LIT))
			continue;

		bool toggled = furnace_tick(f, &ENV);

		if(toggled) {
			// Swap idle<->active block id, PRESERVING the facing metadata. Verify the
			// block is still a furnace (validated before the tick).
			uint8_t want
				= furnace_is_burning(f) ? BLOCK_FURNACE_LIT : BLOCK_FURNACE;
			if(blk.type != want) {
				blk.type = want; // metadata (facing) untouched
				server_world_set_block(w, x, y, z, blk);
				if(server_world_light_halted(w))
					return; /* the ignition/extinction write stopped */
			}
		}

		furnace_push_open_window(s, w, x, y, z, f);
	}
}

/* LEGACY slot-0 adapters (A1b inventory). */
void furnace_server_load_fixup(struct server_local* s, w_coord_t cx,
							   w_coord_t cz) {
	furnace_server_load_fixup_w(&s->worlds[0], cx, cz);
}

void furnace_server_tick(struct server_local* s) {
	furnace_server_tick_w(&s->worlds[0]);
}
