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

#include "redstone_piston_server.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../entity/combat.h"
#include "../entity/entity.h"
#include "../entity/entity_minecart.h"
#include "../entity/entity_monster.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_guest.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../platform/audio.h"	// S2: tile.piston.out / tile.piston.in
#include "../platform/thread.h" // thread_yield (cascade pacing on the single-core Wii)
#include "blocks.h"
#include "blocks_data.h"
#include "piston_mobility.h"
#include "pmove_step_compare.h"
#include "redstone_piston.h"
#include "redstone_server.h" // redstone_server_indirect_power

// PISTON_SETTLE_TICKS (=2) is defined in redstone_piston_server.h so the NBT persistence bridge shares
// the exact constant for the runtime-ticks <-> Beta-progress conversion.

// WII re-entrancy backstop (RAIL 1; the THIRD such cap, after the wire flood's RS_WIRE_FLOOD_MAX_DEPTH
// and the worklist off-stack move). A synchronous piston move can set_block-notify into a neighbour
// piston/wire and recurse; each level costs the small ps_world frame + (capped) wire-flood frames. 8
// piston frames + 12 wire frames + the base stack fit the 64KB no-guard server stack with headroom.
// ignoreUpdates (RAIL 2) is the FAITHFUL termination guarantee; this cap only fires on a contrived chain
// real redstone never builds. MEASURED (devkitPPC -fstack-usage, powerpc-eabi-gcc 15.2.0, Wii CFLAGS):
// the binding consumer is the WIRE flood (~4.2KB/frame x its own cap of 12); a piston ancestor frame is
// the SHALLOW neighbour_change+set_block+notify (~0.6KB) because piston_extend runs under ignoreUpdates
// and the only wire-spawning notify (the line-222 base write) fires AFTER piston_extend returns. Worst
// case 8 piston + 12 wire frames = ~55KB = 84% of the 64KB no-guard server stack (the cliff is at
// caps 16/16 -> ~117% overflow). Do NOT raise either cap without re-measuring.
#define PISTON_MOVE_MAX_DEPTH 8
static int s_piston_move_depth = 0;
// Beta BlockPistonBase.ignoreUpdates is an INSTANCE field, and there are TWO instances -- pistonBase(33)
// and pistonStickyBase(29) -- each with its own flag. So a base move suppresses only same-id (base)
// neighbour re-entry; a sticky neighbour (different instance) still reacts. Model it as two flags keyed
// by stickiness. Set around a move (RAIL 2 -- the faithful termination guarantee) so the move's own
// notify cannot re-enter updatePistonState on the half-settled world; the retract REOPENS its flag
// around each air-clear (via the set_ignore_updates accessor) exactly as Beta does.
static bool s_piston_ignore[2] = {false, false}; // [0] = base(33), [1] = sticky(29)
static int s_piston_cur_idx = 0; // which piston id is mid-move now (for the set_ignore_updates accessor)
// piston_moving_te_tick "born this pass" tracking mirrors Beta
// World.field_30900_E/addedTileEntityList. The monotonic order captured at pass
// entry is an identity as well as an ordering key: a re-entrant creation always
// receives a larger order, even if it reuses a freed sparse slot, so it remains
// invisible and unvisited until the next pass without a fixed-size bitset.
static bool s_settle_pass_active = false;
static uint64_t s_settle_visible_order = UINT64_MAX;
static int piston_idx_of(int type) {
	return (type == RS_PISTON_STICKY_ID) ? 1 : 0;
}

// --- the injected world view over the live server (the rs_ctx pattern) -----------------------------
struct ps_ctx {
	struct server_local* s;
	struct piston_moving_store_reservation* moving_reservation;
};

static int ps_get_type(void* c, int x, int y, int z) {
	struct block_data b;
	/* Beta's ChunkProvider supplies in-range chunks; an unexpected Cave residency
	 * miss is therefore NOT air. Return bedrock as an immovable failure sentinel
	 * so a future caller cannot reopen the cross-chunk headless-base corruption.
	 * The update path provisions every actual move cell before mutation. */
	return server_world_get_block(block_world(((struct ps_ctx*)c)->s), x, y, z, &b)
		? b.type : BLOCK_BEDROCK;
}
static int ps_get_meta(void* c, int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(block_world(((struct ps_ctx*)c)->s), x, y, z, &b) ? (b.metadata & 0x0F) : 0;
}
static float ps_get_hardness(void* c, int id) {
	(void)c;
	// Beta Block.getHardness(); only the -1.0F (unbreakable: bedrock/portal/pistonMoving) sentinel is
	// load-bearing in canPushBlock. CaveWii stores -1 for those (block_bedrock/portal/the moving block),
	// so reading digging.hardness gives the exact sentinel without a special case.
	if(id >= 0 && id < 256 && blocks[id])
		return (float)blocks[id]->digging.hardness;
	return 0.0F;
}
static int ps_get_mobility(void* c, int id) {
	(void)c;
	return piston_mobility_flag(id);
}
static bool ps_has_tile_entity(void* c, int x, int y, int z) {
	// Beta canPushBlock's final getBlockTileEntity()==null gate. The block struct has no generic TE
	// pointer, so name the tile-entity ids explicitly (chest/furnace/dispenser/noteblock/spawner/sign +
	// the moving block itself, which carries its store entry). Keep in lockstep with the TE stores.
	int type = ps_get_type(c, x, y, z);
	if(block_is_any_sign((uint8_t)type))
		return true;
	switch(type) {
		case BLOCK_DISPENSER:
		case BLOCK_NOTEBLOCK:
		case BLOCK_MOB_SPAWNER:
		case BLOCK_CHEST:
		case BLOCK_IRON_CHEST:
		case BLOCK_FURNACE:
		case BLOCK_FURNACE_LIT:
		case BLOCK_JUKEBOX: // Beta BlockJukeBox : BlockContainer -> a TE -> canPushBlock false
		case BLOCK_PISTON_MOVING: return true;
		default: return false;
	}
}
static bool ps_power(void* c, int x, int y, int z, int side) {
	return redstone_server_indirect_power(((struct ps_ctx*)c)->s, x, y, z, side, /*wpp*/ true);
}

// --- the dynamically owned in-RAM moving-TE store ---------------------------------------------------
#define PMOVE_NO_INDEX SIZE_MAX

/* A1b: the piston server runs on the D4 callback bridge (slot 0), so every
 * lookup/insert is owned by slot 0's instance; another world's same-coordinate
 * moving entry is invisible here and never replaced. D4 swaps this world. */
static struct piston_moving_owner pmove_owner(const struct server_local* s) {
	return server_world_piston_owner(block_world(s));
}
static size_t pmove_find_raw(struct server_local* s, int x, int y, int z) {
	size_t index;
	return piston_moving_store_find_owned(&s->piston_moving, pmove_owner(s),
										  x, y, z, &index)
		? index : PMOVE_NO_INDEX;
}
static bool pmove_slot_visible(const struct piston_moving_store_entry* entry) {
	assert(entry && entry->used);
	/* During World.updateEntities, setBlockTileEntity queues newborns in
	 * addedTileEntityList; the chunk map/getBlockTileEntity cannot see them until
	 * the pass tail. The pass-entry order cutoff is independent of sparse slot
	 * reuse and therefore has no fixed-cap bitset. [740c583: World] */
	return !s_settle_pass_active || entry->order <= s_settle_visible_order;
}
static size_t pmove_find(struct server_local* s, int x, int y, int z) {
	size_t i = pmove_find_raw(s, x, y, z);
	return i != PMOVE_NO_INDEX
		&& pmove_slot_visible(&s->piston_moving.entries[i])
		? i : PMOVE_NO_INDEX;
}
static void pmove_read_slot(const struct server_local* s, size_t i,
							struct piston_moving_state* out) {
	assert(i < s->piston_moving.capacity
		   && s->piston_moving.entries[i].used);
	*out = s->piston_moving.entries[i].state;
}
static void pmove_write_progress(struct server_local* s, size_t i,
								 const struct piston_moving_state* state) {
	assert(i < s->piston_moving.capacity
		   && s->piston_moving.entries[i].used);
	s->piston_moving.entries[i].state.current_progress
		= state->current_progress;
	s->piston_moving.entries[i].state.last_progress = state->last_progress;
}
bool piston_moving_state_at(struct server_local* s, int x, int y, int z,
							struct piston_moving_state* out) {
	size_t i = s ? pmove_find(s, x, y, z) : PMOVE_NO_INDEX;
	if(i == PMOVE_NO_INDEX)
		return false;
	if(out)
		pmove_read_slot(s, i, out);
	return true;
}
bool piston_moving_state_lookup(void* ctx, int x, int y, int z,
								struct piston_moving_state* out) {
	return piston_moving_state_at((struct server_local*)ctx, x, y, z, out);
}
static void pmove_free(struct server_local* s, size_t i) {
	(void)piston_moving_store_remove_index(&s->piston_moving, i);
}
// Materialize slot i into its stored block (Beta TileEntityPiston.updateEntity settle: removeBlockTile
// Entity + setBlockAndMetadataWithNotify). Skips the write if the cell is no longer the moving block
// (broken/overwritten in the interim), then frees the slot.
static void pmove_settle_slot(struct server_local* s, size_t i) {
	assert(i < s->piston_moving.capacity
		   && s->piston_moving.entries[i].used);
	struct piston_moving_store_entry entry = s->piston_moving.entries[i];
	int x = entry.x, y = entry.y, z = entry.z;
	int stored_id = entry.state.stored_id;
	int stored_meta = entry.state.stored_meta;
	// Free the slot BEFORE the notify (Beta TileEntityPiston.updateEntity does removeBlockTileEntity +
	// invalidate() BEFORE setBlockAndMetadataWithNotify). Otherwise the notify's cascade can re-enter a
	// neighbour piston whose move ps_add_moving_te's onto THIS cell, reuse this still-occupied slot, and
	// then this function would free that fresh entry -> an orphaned id-36 phantom (unbreakable, persists
	// to disk). Freeing first means a re-entrant move re-occupies the cell with a valid new slot.
	pmove_free(s, i);
	struct block_data cur;
	if(server_world_get_block(block_world(s), x, y, z, &cur) && cur.type == BLOCK_PISTON_MOVING) {
		struct block_data b = {.type = (uint8_t)stored_id, .metadata = (uint8_t)(stored_meta & 0x0F)};
		server_world_set_block(block_world(s), x, y, z, b); // FULL NOTIFY: the settled block re-floods adjacent redstone
	}
}

// Beta BlockPistonMoving.onBlockRemoval -> TileEntityPiston.clearPistonTileEntity (bug-hunt D1).
// Dispatched from every lifecycle set_block that replaces a live id-36 cell (dig, the explosion
// carve, any overwrite). Beta's two clear guards both hold here [740c583 TileEntityPiston:93-101]:
// progress < 1 (piston_moving_force_finish refuses an already-finished TE, whose slot the settle
// pass owns) and getBlockId == pistonMoving (the dispatcher runs AFTER the replacement id is
// published, so pmove_settle_slot's re-check skips the stored-block write-back -- the move dies
// with its cell instead of ghost-materializing into the crater later). The ordinary settle path is
// unaffected: pmove_settle_slot frees its slot BEFORE its own write, so the removal that write
// dispatches for id 36 finds nothing here and no-ops.
void piston_moving_on_removal(struct server_local* s, int x, int y, int z) {
	size_t i = pmove_find(s, x, y, z);
	if(i == PMOVE_NO_INDEX)
		return;
	struct piston_moving_state state;
	pmove_read_slot(s, i, &state);
	if(piston_moving_force_finish(&state)) {
		pmove_write_progress(s, i, &state);
		pmove_settle_slot(s, i);
	}
}

// --- the materialization accessors (Beta setBlockAndMetadata / setBlockTileEntity / dropBlockAsItem) --
static void ps_set_block(void* c, int x, int y, int z, int id, int meta, bool notify) {
	struct server_local* s = ((struct ps_ctx*)c)->s;
	struct block_data b = {.type = (uint8_t)id, .metadata = (uint8_t)(meta & 0x0F)};
	if(notify)
		server_world_set_block(block_world(s), x, y, z, b);
	else
		/* Beta BlockPistonBase.java:87-88,122-123: quiet ID+metadata still lights. */
		server_world_set_block_quiet(block_world(s), x, y, z, b);
}
static void ps_add_moving_te(void* c, int x, int y, int z, int stored_id, int stored_meta, int facing,
							 bool extending, bool is_source) {
	struct server_local* s = ((struct ps_ctx*)c)->s;
	struct piston_moving_state state = {
		.stored_id = (uint8_t)stored_id,
		.stored_meta = (uint8_t)stored_meta,
		.facing = (uint8_t)facing,
		.flags = (uint8_t)((extending ? PISTON_MOVING_FLAG_EXTENDING : 0)
			| (is_source ? PISTON_MOVING_FLAG_SOURCE : 0)),
		.current_progress = 0.0F,
		.last_progress = 0.0F,
	};
	size_t slot;
	/* piston_server_update_state reserves the complete move before its first
	 * voxel write. Allocation here would violate that transaction boundary. */
	if(!piston_moving_store_upsert_claimed_owned(
		   &s->piston_moving,
		   ((struct ps_ctx*)c)->moving_reservation, pmove_owner(s),
		   x, y, z, &state, &slot)) {
		fprintf(stderr, "piston: reserved moving-TE insertion failed\n");
		return;
	}
	// One atomic cell+TE RPC replaces the ordinary SET_BLOCK. The raw write still performs Beta's
	// Chunk.setBlockIDWithMetadata lighting update, but deliberately has no neighbour notification.
	if(!server_world_set_piston_moving(block_world(s), x, y, z, &state)) {
		pmove_free(s, slot);
		fprintf(stderr, "piston: pre-provisioned id-36 write failed\n");
		return;
	}
}
static void ps_settle_moving_te(void* c, int x, int y, int z) {
	struct server_local* s = ((struct ps_ctx*)c)->s;
	size_t i = pmove_find(s, x, y, z);
	if(i != PMOVE_NO_INDEX) {
		struct piston_moving_state state;
		pmove_read_slot(s, i, &state);
		if(piston_moving_force_finish(&state)) {
			pmove_write_progress(s, i, &state);
			pmove_settle_slot(s, i); // no displacement in clearPistonTileEntity
		}
	}
}
static void ps_spawn_drops(void* c, int x, int y, int z, int id, int meta) {
	struct server_local* s = ((struct ps_ctx*)c)->s;
	struct block_data bd = {.type = (uint8_t)id, .metadata = (uint8_t)(meta & 0x0F)};
	struct block_info bi = {.block = &bd, .neighbours = NULL, .x = x, .y = y, .z = z};
	server_local_spawn_block_drops(s, &bi);
}
static void ps_set_ignore_updates(void* c, bool ignore) {
	(void)c;
	s_piston_ignore[s_piston_cur_idx] = ignore; // toggle the moving piston's own per-id flag (Beta)
}
static bool ps_moving_te_state(void* c, int x, int y, int z, int* facing, bool* extending) {
	struct server_local* s = ((struct ps_ctx*)c)->s;
	size_t i = pmove_find(s, x, y, z);
	if(i == PMOVE_NO_INDEX)
		return false;
	if(facing)
		*facing = s->piston_moving.entries[i].state.facing;
	if(extending)
		*extending = piston_moving_is_extending(
			&s->piston_moving.entries[i].state);
	return true;
}

static struct redstone_piston_world ps_world(struct ps_ctx* ctx) {
	return (struct redstone_piston_world) {
		.ctx = ctx,
		.get_type = ps_get_type,
		.get_meta = ps_get_meta,
		.get_hardness = ps_get_hardness,
		.get_mobility_flag = ps_get_mobility,
		.has_tile_entity = ps_has_tile_entity,
		.is_indirectly_providing_power = ps_power,
		.set_block = ps_set_block,
		.add_moving_te = ps_add_moving_te,
		.settle_moving_te = ps_settle_moving_te,
		.spawn_drops = ps_spawn_drops,
		.set_ignore_updates = ps_set_ignore_updates,
		.moving_te_state = ps_moving_te_state,
	};
}

static bool piston_server_load_block_chunk(
	struct server_local* s, int x, int z) {
	w_coord_t cx = WCOORD_CHUNK_OFFSET(x);
	w_coord_t cz = WCOORD_CHUNK_OFFSET(z);
	if(server_world_is_chunk_loaded(block_world(s), cx, cz))
		return true;
	struct server_chunk* chunk = NULL;
	return server_world_load_chunk(block_world(s), cx, cz, &chunk) && chunk;
}

static bool piston_server_prepare_power_chunks(
	struct server_local* s, int x, int z) {
	/* isIndirectlyPowered probes only this 3x3 block-coordinate footprint.
	 * Idle neighbour notifications must not pull in the 14-step push corridor. */
	for(int ox = -1; ox <= 1; ox++)
		for(int oz = -1; oz <= 1; oz++)
			if(!piston_server_load_block_chunk(s, x + ox, z + oz))
				return false;
	return true;
}

static bool piston_server_prepare_line_chunks(struct server_local* s,
	int x, int z, int facing, int last_step) {
	int dx, dy, dz;
	piston_facing_offset(facing, &dx, &dy, &dz);
	(void)dy;
	for(int step = 0; step <= last_step; step++)
		if(!piston_server_load_block_chunk(
			   s, x + dx * step, z + dz * step))
			return false;
	return true;
}

static bool piston_server_prepare_notify_chunks(struct server_local* s,
	int x, int z, int facing, int last_step) {
	int dx, dy, dz;
	piston_facing_offset(facing, &dx, &dy, &dz);
	(void)dy;
	/* Once a real edge is known, provision the one-cell neighbour-notify ring
	 * around only the cells that can actually change. */
	for(int step = 0; step <= last_step; step++) {
		int px = x + dx * step, pz = z + dz * step;
		for(int ox = -1; ox <= 1; ox++)
			for(int oz = -1; oz <= 1; oz++)
				if(!piston_server_load_block_chunk(s, px + ox, pz + oz))
					return false;
	}
	return true;
}

static bool piston_server_extension_ownership(
	const struct redstone_piston_world* w, int x, int y, int z, int facing,
	size_t* out_count, int* out_last_step) {
	int dx, dy, dz;
	piston_facing_offset(facing, &dx, &dy, &dz);
	int movable = 0;
	for(int step = 1; step <= 13; step++) {
		int px = x + dx * step, py = y + dy * step,
			pz = z + dz * step;
		int id = w->get_type(w->ctx, px, py, pz);
		if(id == BLOCK_AIR
		   || w->get_mobility_flag(w->ctx, id) == RS_MOBILITY_DESTROY) {
			*out_count = (size_t)movable + 1; /* shifted blocks + head */
			*out_last_step = step;
			return true;
		}
		movable++;
	}
	return false;
}

// --- the synchronous move dispatch (Beta BlockPistonBase.updatePistonState) -------------------------
static void piston_server_update_state(struct server_local* s, int x, int y, int z) {
	if(s_piston_move_depth >= PISTON_MOVE_MAX_DEPTH)
		return; // RAIL 1: bail before the frame on a pathological re-entrant chain
	struct block_data b;
	if(!server_world_get_block(block_world(s), x, y, z, &b)
	   || (b.type != RS_PISTON_BASE_ID && b.type != RS_PISTON_STICKY_ID))
		return;
	int initial_meta = b.metadata & 0x0F;
	if(initial_meta == 7)
		return; // Beta `if (meta != 7)` guard
	if(!piston_server_prepare_power_chunks(s, x, z))
		return;
	s_piston_move_depth++;
	if((s_piston_move_depth & 1) == 0)
		thread_yield(); // RAIL 7: let render take a frame every couple of nested levels

	/* Re-read after synchronous chunk provisioning: loading parsed ownership but
	 * may also service callbacks in future providers; never act on a stale base. */
	if(server_world_get_block(block_world(s), x, y, z, &b) && (b.type == RS_PISTON_BASE_ID || b.type == RS_PISTON_STICKY_ID)) {
		int meta = b.metadata & 0x0F;
		if(meta != 7) { // Beta `if (meta != 7)` guard
			int front = piston_orientation(meta);
			bool sticky = (b.type == RS_PISTON_STICKY_ID);
			int idx = piston_idx_of(b.type);
			int saved_idx = s_piston_cur_idx; // restore after a nested move re-entered via a reopen
			s_piston_cur_idx = idx;
			struct ps_ctx ctx = {.s = s, .moving_reservation = NULL};
			struct redstone_piston_world w = ps_world(&ctx);
			bool powered = piston_is_indirectly_powered(&w, x, y, z, front);
			bool is_powered = piston_is_powered(meta);
			if(powered && !is_powered) {
				/* The unresident corridor is neither air nor a refusal: provide the
				 * exact scan line, then make the Beta legality decision. */
				if(piston_server_prepare_line_chunks(s, x, z, front, 13)
				   && piston_can_extend(&w, x, y, z, front)) {
					size_t owners;
					int last_step;
					struct piston_moving_store_reservation reservation = {0};
					if(!piston_server_extension_ownership(
						   &w, x, y, z, front, &owners, &last_step)
					   || !piston_server_prepare_notify_chunks(
						   s, x, z, front, last_step)
					   || !piston_moving_store_reservation_begin(
						   &s->piston_moving, owners, &reservation))
						goto piston_update_done;
					ctx.moving_reservation = &reservation;
					// RAIL 6: latch the powered bit FIRST (silent, Beta setBlockMetadata) so a re-entrant
					// update reads it and bails; THEN materialize; THEN notify (Beta's tail
					// setBlockMetadataWithNotify) so the new output re-floods neighbours.
					b.metadata = (uint8_t)(front | 8);
					if(!server_world_set_block_metadata_quiet(
						   block_world(s), x, y, z, b.metadata)) {
						piston_moving_store_reservation_cancel(
							&s->piston_moving, &reservation);
						goto piston_update_done;
					}
					s_piston_ignore[idx] = true; // RAIL 2 (this piston's own per-id flag)
					bool extended = piston_extend(&w, x, y, z, front, sticky);
					(void)extended; // assert-only reader below; -DNDEBUG
									// compiles that out and orphans it
					ctx.moving_reservation = NULL;
					assert(extended && reservation.remaining == 0);
					piston_moving_store_reservation_cancel(
						&s->piston_moving, &reservation);
					// S2: Beta BlockPistonBase.playBlock (block event 0) plays
					// "tile.piston.out" 0.5F, rand*0.25F+0.6F at the base cell
					// center, only after tryExtend() succeeded -- exactly this
					// point (the tryExtend gate is the piston_can_extend +
					// materialization above). ONE emit per actual move
					// dispatch, never per notify. Roll from the dedicated
					// sound stream (worldObj.rand in Beta is cosmetic here).
					// [740c583: BlockPistonBase.playBlock:112-170]
					server_local_emit_sound(
						SOUND_TILE_PISTON_OUT,
						(vec3) {x + 0.5F, y + 0.5F, z + 0.5F}, 0.5F,
						rand_gen_flt(server_local_sound_rng()) * 0.25F + 0.6F);
					// Beta fires the tail setBlockMetadataWithNotify(front|8) WHILE ignoreUpdates is still
					// TRUE (cleared only as playBlock's last statement), so this notify suppresses a same-
					// kind neighbour piston's re-entry. Notify BEFORE lowering the guard.
					if(!server_world_set_block_metadata_with_notify(
						   block_world(s), x, y, z, b.metadata))
						fprintf(stderr,
							"piston: powered metadata tail notify failed at (%d,%d,%d)\n",
							x, y, z);
					s_piston_ignore[idx] = false;
				}
			} else if(!powered && is_powered) {
				/* A front-head force-settle notifies synchronously and can change the
				 * two-ahead candidate. Claim the direct upper bound (source + sticky
				 * pull) before that first mutation; unused capacity is canceled. */
				struct piston_moving_store_reservation reservation = {0};
				if(!piston_server_prepare_line_chunks(s, x, z, front, 2)
				   || !piston_server_prepare_notify_chunks(s, x, z, front, 2)
				   || !piston_moving_store_reservation_begin(
					   &s->piston_moving,
					   sticky ? 2u : 1u,
					   &reservation))
					goto piston_update_done;
				ctx.moving_reservation = &reservation;
				// Beta setBlockMetadata(front) clears the powered bit (immediately overwritten by the
				// retract turning the base cell into a moving-TE; kept for faithfulness).
				b.metadata = (uint8_t)front;
				if(!server_world_set_block_metadata_quiet(
					   block_world(s), x, y, z, b.metadata)) {
					piston_moving_store_reservation_cancel(
						&s->piston_moving, &reservation);
					goto piston_update_done;
				}
				s_piston_ignore[idx] = true; // RAIL 2; piston_retract REOPENS it around each air-clear
				piston_retract(&w, x, y, z, front, sticky);
				ctx.moving_reservation = NULL;
				assert(reservation.remaining == 0
					   || (sticky && reservation.remaining == 1));
				piston_moving_store_reservation_cancel(
					&s->piston_moving, &reservation);
				// S2: Beta BlockPistonBase.playBlock (block event 1) plays
				// "tile.piston.in" 0.5F, rand*0.15F+0.6F UNCONDITIONALLY at
				// the end of the retract branch -- with or without a pulled
				// block, sticky or not (this branch fires only on the
				// powered->unpowered edge, so it IS one emit per retract).
				// [740c583: BlockPistonBase.playBlock:112-170]
				server_local_emit_sound(
					SOUND_TILE_PISTON_IN,
					(vec3) {x + 0.5F, y + 0.5F, z + 0.5F}, 0.5F,
					rand_gen_flt(server_local_sound_rng()) * 0.15F + 0.6F);
				s_piston_ignore[idx] = false;
			}
	piston_update_done:
			s_piston_cur_idx = saved_idx;
		}
	}
	s_piston_move_depth--; // balanced with the increment (no early return past it)
}

void piston_server_neighbour_change(struct server_local* s, struct block_info* info) {
	// Beta onNeighborBlockChange reads THIS piston's own per-id ignoreUpdates. Read the notified block to
	// pick the flag (a base move does not suppress a sticky neighbour, and vice versa).
	struct block_data nb;
	if(server_world_get_block(block_world(s), info->x, info->y, info->z, &nb)
	   && (nb.type == RS_PISTON_BASE_ID || nb.type == RS_PISTON_STICKY_ID)
	   && s_piston_ignore[piston_idx_of(nb.type)])
		return; // this piston's flag is set (its own move is mid-materialization) -> suppress re-entry
	piston_server_update_state(s, info->x, info->y, info->z);
}

// Beta BlockPistonExtension.onBlockRemoval: mining an EXTENDED head (id 34) drops + clears the still-
// POWERED base behind it (so a powered headless base is never left in the world, and the player gets the
// piston item back). Called from the dig path with the head's pre-removal metadata. The head itself drops
// nothing (Beta quantityDropped==0). The move's OWN head removal never reaches here: it writes through the
// pure accessors, not the dig path, AND retract clears the base's powered bit first -- so no move re-entry.
void piston_head_on_removal(struct server_local* s, int x, int y, int z, int head_meta) {
	int dx, dy, dz;
	piston_facing_offset(piston_orientation(head_meta), &dx, &dy, &dz);
	int bx = x - dx, by = y - dy, bz = z - dz; // the base cell one step behind the head
	struct block_data base;
	if(server_world_get_block(block_world(s), bx, by, bz, &base)
	   && (base.type == RS_PISTON_BASE_ID || base.type == RS_PISTON_STICKY_ID)
	   && piston_is_powered(base.metadata & 0x0F)) {
		// Beta: Block.blocksList[base].dropBlockAsItem(...) THEN setBlockWithNotify(base, 0).
		server_local_spawn_block_drops(
			s, &(struct block_info) {.block = &base, .x = bx, .y = by, .z = bz});
		server_world_set_block(block_world(s), bx, by, bz, (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
	}
}

// Beta BlockPistonExtension.onNeighborBlockChange: if the cell behind the head (toward the base) is no
// longer a piston base, the now-baseless head self-removes (orphan cleanup -- e.g. the player dug the
// base out); else it forwards to the base's onNeighborBlockChange (which honours the base's own
// per-instance ignoreUpdates). Bound onto block 34. There is deliberately no global "some piston is
// moving" guard: a base move suppresses its own kind only, while an opposite-kind head/base cascade
// remains observable exactly as BlockPistonExtension forwards it. [740c583: BlockPistonExtension,
// BlockPistonBase.ignoreUpdates]
void piston_head_neighbour_change(struct server_local* s, struct block_info* info) {
	struct block_data head;
	if(!server_world_get_block(block_world(s), info->x, info->y, info->z, &head)
	   || head.type != RS_PISTON_HEAD_ID)
		return;
	int dx, dy, dz;
	piston_facing_offset(piston_orientation(head.metadata & 0x0F), &dx, &dy, &dz);
	int bx = info->x - dx, by = info->y - dy, bz = info->z - dz;
	struct block_data base;
	bool is_base = server_world_get_block(block_world(s), bx, by, bz, &base)
		&& (base.type == RS_PISTON_BASE_ID || base.type == RS_PISTON_STICKY_ID);
	if(!is_base)
		server_world_set_block(block_world(s), info->x, info->y, info->z,
							   (struct block_data) {.type = BLOCK_AIR, .metadata = 0}); // self-remove
	else
		piston_server_neighbour_change(s, &(struct block_info) {.x = bx, .y = by, .z = bz}); // forward to base
}

bool piston_server_place(struct server_local* s, struct server_player* actor,
		struct block_info* where, int facing, bool sticky) {
	struct block_data blk
		= {.type = (uint8_t)(sticky ? RS_PISTON_STICKY_ID : RS_PISTON_BASE_ID), .metadata = (uint8_t)(facing & 0x07)};
	struct block_info bi = *where;
	bi.block = &blk;
	if(block_place_obstructed_for(s, actor, &bi)) // Beta canBlockBePlacedAt entity gate (item not consumed)
		return false;
	server_world_set_block(block_world(s), where->x, where->y, where->z, blk);
	// Beta onBlockPlacedBy -> updatePistonState (extend at once if placed onto live power).
	piston_server_update_state(s, where->x, where->y, where->z);
	return true;
}

static bool pmove_strict_intersection(const struct AABB* a, const struct AABB* b) {
	return a->x2 > b->x1 && a->x1 < b->x2
		&& a->y2 > b->y1 && a->y1 < b->y2
		&& a->z2 > b->z1 && a->z1 < b->z2;
}

static float pmove_axis_min(const struct AABB* box, int axis) {
	assert(box && axis >= 0 && axis < 3);
	switch(axis) {
		case 0: return box->x1;
		case 1: return box->y1;
		case 2: return box->z1;
		default: return 0.0F;
	}
}

static float pmove_axis_max(const struct AABB* box, int axis) {
	assert(box && axis >= 0 && axis < 3);
	switch(axis) {
		case 0: return box->x2;
		case 1: return box->y2;
		case 2: return box->z2;
		default: return 0.0F;
	}
}

static void pmove_expand_axis(struct AABB* box, int axis, float amount) {
	assert(box && axis >= 0 && axis < 3);
	switch(axis) {
		case 0:
			if(amount < 0.0F) box->x1 += amount;
			else box->x2 += amount;
			break;
		case 1:
			if(amount < 0.0F) box->y1 += amount;
			else box->y2 += amount;
			break;
		case 2:
			if(amount < 0.0F) box->z1 += amount;
			else box->z2 += amount;
			break;
		default: break;
	}
}

/* AxisAlignedBB.calculate{X,Y,Z}Offset, with `obstacle` as the receiver. */
static float pmove_clip_one(const struct AABB* obstacle, const struct AABB* body,
							int axis, float amount) {
	int a = (axis + 1) % 3, b = (axis + 2) % 3;
	if(pmove_axis_max(body, a) <= pmove_axis_min(obstacle, a)
	   || pmove_axis_min(body, a) >= pmove_axis_max(obstacle, a)
	   || pmove_axis_max(body, b) <= pmove_axis_min(obstacle, b)
	   || pmove_axis_min(body, b) >= pmove_axis_max(obstacle, b))
		return amount;
	if(amount > 0.0F
	   && pmove_axis_max(body, axis) <= pmove_axis_min(obstacle, axis)) {
		float gap = pmove_axis_min(obstacle, axis) - pmove_axis_max(body, axis);
		if(gap < amount)
			amount = gap;
	} else if(amount < 0.0F
			  && pmove_axis_min(body, axis) >= pmove_axis_max(obstacle, axis)) {
		float gap = pmove_axis_max(obstacle, axis) - pmove_axis_min(body, axis);
		if(gap > amount)
			amount = gap;
	}
	return amount;
}

static void pmove_player_box(const struct server_player* player,
							 struct AABB* out) {
	assert(player && out);
	aabb_setsize_centered(out, 0.6F, PLAYER_BODY_HEIGHT, 0.6F);
	aabb_translate(out, 0.0F,
		PLAYER_BODY_HEIGHT * 0.5F - PLAYER_EYE_HEIGHT, 0.0F);
	aabb_translate(out, (float)player->x, (float)player->y,
		(float)player->z);
}

static void pmove_player_local_box(struct AABB* out) {
	assert(out);
	aabb_setsize_centered(out, 0.6F, PLAYER_BODY_HEIGHT, 0.6F);
	aabb_translate(out, 0.0F,
		PLAYER_BODY_HEIGHT * 0.5F - PLAYER_EYE_HEIGHT, 0.0F);
}

/* The body is committed to the world this moving-TE pass ticks (its slot; the
 * raw slot, so a fixture world without a generation still counts). */
static bool pmove_actor_in_pass_world(struct server_local* s,
									  const struct server_player* actor) {
	return actor->world_slot < SERVER_LOCAL_WORLD_SLOTS
		&& &s->worlds[actor->world_slot] == block_world(s);
}

/* World.getEntitiesWithinAABBExcludingEntity expands the chunk/section search
 * by two blocks, then Chunk walks its section lists. This moving-TE visit uses
 * keys snapshotted before any candidate moves; refreshed membership becomes
 * visible to the following query, never halfway through this one. */
static bool pmove_query_ref_visible(
	const struct server_entity_query_ref* ref, const struct AABB* query) {
	assert(ref && query);
	int min_cx = (int)floorf((query->x1 - 2.0F) / CHUNK_SIZE);
	int max_cx = (int)floorf((query->x2 + 2.0F) / CHUNK_SIZE);
	int min_cz = (int)floorf((query->z1 - 2.0F) / CHUNK_SIZE);
	int max_cz = (int)floorf((query->z2 + 2.0F) / CHUNK_SIZE);
	int min_cy = (int)floorf((query->y1 - 2.0F) / CHUNK_SIZE);
	int max_cy = (int)floorf((query->y2 + 2.0F) / CHUNK_SIZE);
	if(min_cy < 0)
		min_cy = 0;
	int section_max = WORLD_HEIGHT / CHUNK_SIZE - 1;
	if(max_cy > section_max)
		max_cy = section_max;
	return ref->chunk_x >= min_cx && ref->chunk_x <= max_cx
		&& ref->chunk_z >= min_cz && ref->chunk_z <= max_cz
		&& ref->chunk_y >= min_cy && ref->chunk_y <= max_cy;
}

/* Clip against the exact collision list selected by World.getCollidingBoundingBoxes
 * for `query`. Re-running the deterministic x,z,y scan for each axis is equivalent
 * to retaining Java's list and avoids a Wii-only fixed-cap approximation. */
static float pmove_clip_query(struct server_local* s, const struct AABB* query,
							  const struct AABB* body, int axis, float amount,
							  const struct entity* mover) {
	int min_x = (int)floorf(query->x1);
	/* World.getCollidingBoundingBoxes starts at floor(minY)-1. The extra row is
	 * load-bearing for blocks whose collision rises above their cell (a fence is
	 * 1.5 blocks tall). [740c583: World.getCollidingBoundingBoxes] */
	int min_y = (int)floorf(query->y1) - 1;
	int min_z = (int)floorf(query->z1);
	int max_x = (int)floorf(query->x2 + 1.0F);
	int max_y = (int)floorf(query->y2 + 1.0F);
	int max_z = (int)floorf(query->z2 + 1.0F);
	if(min_y < 0)
		min_y = 0;
	if(max_y > WORLD_HEIGHT)
		max_y = WORLD_HEIGHT;

	for(int x = min_x; x < max_x; x++)
		for(int z = min_z; z < max_z; z++)
			for(int y = min_y; y < max_y; y++) {
				struct block_data block;
				if(!server_world_get_block(block_world(s), x, y, z, &block)
				   || !blocks[block.type] || !blocks[block.type]->getBoundingBox)
					continue;
				struct block_info info = {
					.block = &block,
					.x = x,
					.y = y,
					.z = z,
					.get_piston_moving = piston_moving_state_lookup,
					.get_piston_moving_ctx = s,
				};
				size_t count = blocks[block.type]->getBoundingBox(&info, true, NULL);
				if(count == 0)
					continue;
				struct AABB boxes[count];
				blocks[block.type]->getBoundingBox(&info, true, boxes);
				for(size_t i = 0; i < count; i++) {
					aabb_translate(&boxes[i], (float)x, (float)y, (float)z);
					amount = pmove_clip_one(&boxes[i], body, axis, amount);
				}
			}

	/* Entity.getBoundingBox is null for Beta's ordinary entities. Minecart alone
	 * overrides getCollisionBox(other) to return the OTHER entity's physical box,
	 * so only a pushed minecart receives this second list branch. [740c583:
	 * World.getCollidingBoundingBoxes, Entity, EntityMinecart] */
	if(mover && mover->type == ENTITY_MINECART) {
		struct AABB broad = *query;
		broad.x1 -= 0.25F; broad.y1 -= 0.25F; broad.z1 -= 0.25F;
		broad.x2 += 0.25F; broad.y2 += 0.25F; broad.z2 += 0.25F;
		size_t ref_count = 0;
		const struct server_entity_query_ref* refs
			= server_local_entity_query_snapshot(s, &ref_count);
		for(size_t r = 0; r < ref_count; r++) {
			if(refs[r].player) {
				/* v5: each player body at its OWN section-list position
				 * (the guest is a section member too). */
				struct server_player* pl
					= server_local_query_ref_body(s, &refs[r]);
				if(!pl || !pl->has_pos || !pmove_actor_in_pass_world(s, pl))
					continue; /* only the bodies in this pass's world */
				struct AABB collision;
				pmove_player_box(pl, &collision);
				if(pmove_strict_intersection(&collision, &broad)
				   && pmove_strict_intersection(&collision, query))
					amount = pmove_clip_one(&collision, body, axis, amount);
				continue;
			}
			if(!pmove_query_ref_visible(&refs[r], &broad))
				continue;
			struct AABB collision;
			{
				const struct entity* other = refs[r].entity;
				if(!other || other == mover || other->delay_destroy == 0
				   || !other->getBoundingBox
				   || other->getBoundingBox(other, &collision) == 0)
					continue;
			}
			if(pmove_strict_intersection(&collision, &broad)
			   && pmove_strict_intersection(&collision, query))
				amount = pmove_clip_one(&collision, body, axis, amount);
		}
	}
	return amount;
}

static float pmove_move_axis(struct server_local* s, const struct AABB* query,
							 struct AABB* body, int axis, float amount,
							 const struct entity* mover) {
	float clipped = pmove_clip_query(s, query, body, axis, amount, mover);
	float delta[3] = {0.0F, 0.0F, 0.0F};
	delta[axis] = clipped;
	aabb_translate(body, delta[0], delta[1], delta[2]);
	return clipped;
}

struct pmove_move_result {
	float delta[3];
	float resolved[3];
	bool clipped[3];
	bool stepped;
	bool on_ground;
};

static void pmove_move_body(struct server_local* s, struct AABB* body,
							int axis, float amount, float step_height,
							bool was_on_ground, bool y_size_allows_step,
							const struct entity* mover,
							struct pmove_move_result* out) {
	assert(s && body && axis >= 0 && axis < 3 && out);
	struct AABB original = *body;
	float requested[3] = {0.0F, 0.0F, 0.0F};
	requested[axis] = amount;

	/* Ordinary Entity.moveEntity pass: select one collision list from addCoord,
	 * then resolve Y, X, Z against that same list. */
	struct AABB query = original;
	for(int k = 0; k < 3; k++)
		pmove_expand_axis(&query, k, requested[k]);
	float resolved[3];
	resolved[1] = pmove_move_axis(s, &query, body, 1, requested[1], mover);
	resolved[0] = pmove_move_axis(s, &query, body, 0, requested[0], mover);
	resolved[2] = pmove_move_axis(s, &query, body, 2, requested[2], mover);
	float flat_resolved[3] = {resolved[0], resolved[1], resolved[2]};
	bool grounded_for_step = was_on_ground
		|| (requested[1] != resolved[1] && requested[1] < 0.0F);

	/* EntityLiving.stepHeight == .5. Re-run from the original box with the
	 * step-height query, resolve up -> X -> Z -> down, and keep it only when its
	 * horizontal distance is STRICTLY greater than the flat candidate. */
	bool horizontal_clipped
		= requested[0] != resolved[0] || requested[2] != resolved[2];
	bool used_step_candidate = false;
	if(step_height > 0.0F && grounded_for_step && y_size_allows_step
	   && horizontal_clipped) {
		struct AABB stepped = original;
		struct AABB step_query = original;
		pmove_expand_axis(&step_query, 0, requested[0]);
		pmove_expand_axis(&step_query, 1, step_height);
		pmove_expand_axis(&step_query, 2, requested[2]);
		float step_resolved[3];
		step_resolved[1]
			= pmove_move_axis(s, &step_query, &stepped, 1, step_height, mover);
		step_resolved[0]
			= pmove_move_axis(s, &step_query, &stepped, 0, requested[0], mover);
		step_resolved[2]
			= pmove_move_axis(s, &step_query, &stepped, 2, requested[2], mover);
		/* Java reuses var3 for the final down component; collision/onGround and
		 * motionY zeroing compare this component, not the net box rise. */
		step_resolved[1]
			= pmove_move_axis(s, &step_query, &stepped, 1, -step_height, mover);
		/* Entity.moveEntity:398-402, compared in double as Beta does
		 * (pmove_step_compare.h); a tie keeps the flat pass. */
		if(pmove_step_route_wins(flat_resolved[0], flat_resolved[2],
								 step_resolved[0], step_resolved[2])) {
			*body = stepped;
			memcpy(resolved, step_resolved, sizeof resolved);
			used_step_candidate = true;
		}
	}

	memset(out, 0, sizeof(*out));
	out->delta[0] = body->x1 - original.x1;
	out->delta[1] = body->y1 - original.y1;
	out->delta[2] = body->z1 - original.z1;
	for(int k = 0; k < 3; k++)
		out->resolved[k] = resolved[k];
	for(int k = 0; k < 3; k++)
		out->clipped[k] = requested[k] != resolved[k];
	out->stepped = used_step_candidate;
	out->on_ground = requested[1] != resolved[1] && requested[1] < 0.0F;
}

static bool pmove_constant_state(void* ctx, int x, int y, int z,
								 struct piston_moving_state* out) {
	(void)x;
	(void)y;
	(void)z;
	*out = *(const struct piston_moving_state*)ctx;
	return true;
}

static bool pmove_push_box(const struct server_local* s, size_t i,
						   const struct piston_moving_state* state,
						   float progress, struct AABB* out) {
	struct piston_moving_state at_progress = *state;
	/* Make BlockPistonMoving's ordinary lag-progress transform equal func_31010_a's explicit one. */
	at_progress.last_progress = piston_moving_is_extending(state)
		? progress : progress - 1.0F;
	at_progress.current_progress = at_progress.last_progress;
	struct block_data marker = {
		.type = BLOCK_PISTON_MOVING,
		.metadata = (uint8_t)(state->stored_meta & 0x0F),
	};
	struct block_info info = {
		.block = &marker,
		.x = s->piston_moving.entries[i].x,
		.y = s->piston_moving.entries[i].y,
		.z = s->piston_moving.entries[i].z,
		.get_piston_moving = pmove_constant_state,
		.get_piston_moving_ctx = &at_progress,
	};
	if(blocks[BLOCK_PISTON_MOVING]->getBoundingBox(&info, true, out) == 0)
		return false;
	aabb_translate(out, (float)info.x, (float)info.y, (float)info.z);
	return true;
}

static void pmove_send_entity_correction(const struct entity* e) {
	clin_rpc_send(&(struct client_rpc) {
		.type = CRPC_ENTITY_PHYSICS_CORRECTION,
		.payload.entity_physics_correction = {
			.entity_id = e->id,
			.pos = {e->pos[0], e->pos[1], e->pos[2]},
			.vel = {e->vel[0], e->vel[1], e->vel[2]},
			.y_size = e->y_size,
			.fall_distance = e->fall_distance,
			.on_ground = e->on_ground,
			.collided_horizontally = e->collided_horizontally,
			.collided_vertically = e->collided_vertically,
			.in_web = e->in_web,
			/* A mob's server-side e->on_fire is only maintained for
			 * fire-immune mobs; publish the same flag the MOVE snapshot
			 * does, or the correction clears a live flame (G6d). */
			.on_fire = e->type == ENTITY_MONSTER
				? monster_published_on_fire(e) : e->on_fire,
			.minecart_speed = e->type == ENTITY_MINECART
				? e->data.minecart.speed : 0.0F,
			.minecart_hx = e->type == ENTITY_MINECART
				? e->data.minecart.hx : 0,
			.minecart_hz = e->type == ENTITY_MINECART
				? e->data.minecart.hz : 0,
			.minecart_rail_meta = e->type == ENTITY_MINECART
				? e->data.minecart.rail_meta : 0,
			.minecart_last_meta = e->type == ENTITY_MINECART
				? e->data.minecart.last_meta : 0,
			.fuse = e->type == ENTITY_PRIMED_TNT
				? (int8_t)e->data.primed_tnt.fuse : -1,
		},
	});
}

static void pmove_update_fall_state(struct entity* e, float resolved_y) {
	assert(e);
	if(e->on_ground) {
		/* Entity.updateFallState calls the virtual fall() before clearing the
		 * accumulator. EntityLiving.fall applies ceil(distance-3); the chicken
		 * override is empty. Other pushed entity classes inherit Entity.fall(). */
		if(e->type == ENTITY_MONSTER
		   && !monster_def_get(e->data.monster.id)->fall_immune) {
			int damage = mob_fall_damage(e->fall_distance);
			if(damage > 0)
				(void)entity_monster_apply_damage(e, damage, false);
		}
		e->fall_distance = 0.0F;
	} else if(resolved_y < 0.0F) {
		e->fall_distance -= resolved_y;
	}
	if(e->type == ENTITY_MONSTER)
		e->data.monster.fall_distance = e->fall_distance;
}

static bool pmove_update_player_fall_state(struct server_local* s,
	struct server_player* player, float resolved_y) {
	assert(s && player);
	/* 740c583 Entity.moveEntity passes the clipped Y component to
	 * updateFallState. Its landing branch calls fall() with the PRE-MOVE
	 * accumulator and then clears it; only an airborne negative component is
	 * added. Keep that order--the final clipped landing fraction is not billed. */
	if(player->on_ground) {
		int damage
			= player_fall_damage_hp(player->fall_distance, false);
		bool alive = damage <= 0
			|| player != server_local_damage_candidate(s, player != &s->player)
			|| server_local_damage_actor(s, player, damage, 0.0F, 0.0F);
		player->fall_distance = 0.0F;
		return alive;
	}
	if(resolved_y < 0.0F)
		player->fall_distance -= resolved_y;
	return true;
}

struct pmove_player_correction {
	bool needed;
	bool cancelled;
	struct actor_ident actor;
	uint8_t flags;
};

/* TileEntityPiston pushes EntityPlayer through Entity.moveEntity, whose tail
 * scans every block cell intersecting the final AABB (only when all touched
 * chunks exist), in X/Y/Z order, using the exact +/-0.001 bounds. Restrict this
 * bridge to BlockCactus, the damaging callback the server's later center-cell
 * approximation can miss on a flank. [740c583: TileEntityPiston.func_31010_a,
 * Entity.moveEntity, BlockCactus.onEntityCollidedWithBlock] */
static bool pmove_player_post_move_cactus_collisions(
	struct server_local* s, struct server_player* player,
	const struct AABB* box) {
	assert(s && player && box);
	/* A bound offer is physically pushed, but cannot receive gameplay damage. */
	if(player != server_local_damage_candidate(s, player != &s->player))
		return true;
	int x0 = (int)floorf(box->x1 + 0.001F);
	int y0 = (int)floorf(box->y1 + 0.001F);
	int z0 = (int)floorf(box->z1 + 0.001F);
	int x1 = (int)floorf(box->x2 - 0.001F);
	int y1 = (int)floorf(box->y2 - 0.001F);
	int z1 = (int)floorf(box->z2 - 0.001F);
	if(y1 < 0 || y0 >= WORLD_HEIGHT)
		return true;

	int cx0 = WCOORD_CHUNK_OFFSET(x0);
	int cz0 = WCOORD_CHUNK_OFFSET(z0);
	int cx1 = WCOORD_CHUNK_OFFSET(x1);
	int cz1 = WCOORD_CHUNK_OFFSET(z1);
	for(int cx = cx0; cx <= cx1; cx++)
		for(int cz = cz0; cz <= cz1; cz++)
			if(!server_world_is_chunk_loaded(block_world(s), cx, cz))
				return true;

	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++) {
				struct block_data block;
				if(server_world_get_block(block_world(s), x, y, z, &block)
				   && block.type == BLOCK_CACTUS
				   && !server_local_damage_actor(
					   s, player, COMBAT_HEALTH_PER_HEART / 2, 0.0F, 0.0F))
					return false;
			}
	return true;
}

static void pmove_push_player_candidate(
	struct server_local* s, struct server_player* player,
	const struct AABB* push_box, int axis,
	float requested, struct pmove_player_correction* player_correction) {
	if(player_correction->cancelled)
		return;
	struct AABB player_box;
	pmove_player_box(player, &player_box);
	if(!pmove_strict_intersection(&player_box, push_box))
		return;
	float player_requested = requested;
	if(axis != 1 && player->on_ground && player->sneaking) {
		/* Entity.moveEntity's ledge clamp is keyed to persistent pre-call
		 * state, not a geometry ground reprobe. */
		struct entity proxy;
		memset(&proxy, 0, sizeof proxy);
		proxy.on_server = true;
		proxy.world = block_world(s);
		proxy.pos[0] = (float)player->x;
		proxy.pos[1] = (float)player->y;
		proxy.pos[2] = (float)player->z;
		proxy.vel[axis] = player_requested;
		proxy.on_ground = true;
		struct AABB local_box;
		pmove_player_local_box(&local_box);
		entity_sneak_ledge_clamp(&proxy, &local_box);
		player_requested = proxy.vel[axis];
	}
	struct pmove_move_result move;
	pmove_move_body(s, &player_box, axis, player_requested, 0.5F,
		player->on_ground, true, NULL, &move);
	player_correction->needed = true;
	player_correction->flags |= PLAYER_CORRECTION_POSITION_ONLY
		| PLAYER_CORRECTION_SET_ON_GROUND;
	if(move.on_ground)
		player_correction->flags |= PLAYER_CORRECTION_ON_GROUND;
	else
		player_correction->flags
			&= (uint8_t)~PLAYER_CORRECTION_ON_GROUND;
	player->on_ground = move.on_ground;
	if(move.delta[0] != 0.0F || move.delta[1] != 0.0F
	   || move.delta[2] != 0.0F) {
		player->x += (double)move.delta[0];
		player->y += (double)move.delta[1];
		player->z += (double)move.delta[2];
	}
	if(move.clipped[0]) {
		player->vel_x = 0.0F;
		player_correction->flags |= PLAYER_CORRECTION_ZERO_VEL_X;
	}
	if(move.clipped[2]) {
		player->vel_z = 0.0F;
		player_correction->flags |= PLAYER_CORRECTION_ZERO_VEL_Z;
	}
	if(move.clipped[1]) {
		player->vel_y = 0.0F;
		player_correction->flags |= PLAYER_CORRECTION_ZERO_VEL_Y;
	}
	if(!pmove_update_player_fall_state(s, player, move.resolved[1])) {
		/* Death constructed a fresh player and may have armed a deferred world
		 * switch. Do not correct or push that replacement body from the old
		 * moving-tile pass. */
		player_correction->needed = false;
		player_correction->cancelled = true;
		player_correction->flags = 0;
		return;
	}
	if(!pmove_player_post_move_cactus_collisions(s, player, &player_box)) {
		/* Cave replaces a dead player in-place. Stop the old Entity.moveEntity
		 * callback phase so later cactus cells and the correction cannot target
		 * that fresh spawn. Beta's remaining callbacks still target its old dead
		 * EntityPlayer object and are absorbed there. */
		player_correction->needed = false;
		player_correction->cancelled = true;
		player_correction->flags = 0;
		return;
	}
	if(player == &s->player)
		server_local_refresh_player_membership(s);
	else
		server_local_refresh_guest_membership(s);
}

static void pmove_push_entity_candidate(
	struct server_local* s, struct entity* e, const struct AABB* push_box,
	int axis, float requested) {
	if(!e || e->delay_destroy == 0 || !e->getBoundingBox)
		return;
	struct AABB body;
	if(e->getBoundingBox(e, &body) == 0
	   || !pmove_strict_intersection(&body, push_box))
		return;
	if(e->type == ENTITY_MONSTER) {
		/* Mob motion units S2 (Fable S1 F1): TileEntityPiston delegates to
		 * Entity.moveEntity, and a mob's moveEntity is the authoritative
		 * double port (mob_move_entity_d: ySize *= 0.4F, web scaling, Y/X/Z
		 * clip, stepHeight retry, flags, per-axis motion zero). Then
		 * updateFallState on the applied dy, and the post-move block-callback
		 * scan, exactly as the mob's own move does. */
		struct mob_motion* md = &e->data.monster.motion_d;
		double d[3] = {0.0, 0.0, 0.0};
		d[axis] = (double)requested;
		struct mob_collide_env env = entity_monster_collide_env(s);
		struct mob_move_result mv
			= mob_move_entity_d(e, md, d[0], d[1], d[2], &env);
		e->data.monster.prev_on_ground = e->on_ground;
		e->data.monster.prev_collided_xz = e->collided_horizontally;
		if(e->on_ground) {
			if(!monster_def_get(e->data.monster.id)->fall_immune) {
				int damage = mob_fall_damage(e->data.monster.fall_distance);
				if(damage > 0)
					(void)entity_monster_apply_damage(e, damage, false);
			}
			e->data.monster.fall_distance = 0.0F;
		} else if(!mv.incomplete && mv.dy < 0.0) {
			e->data.monster.fall_distance
				= (float)((double)e->data.monster.fall_distance - mv.dy);
		}
		e->fall_distance = e->data.monster.fall_distance;
		entity_monster_post_move_cactus_collisions(e, s);
		if(e->health <= 0) {
			vec3 here = {e->pos[0], e->pos[1], e->pos[2]};
			entity_monster_begin_death(e, s, here);
		}
		server_local_refresh_entity_membership(s, e);
		pmove_send_entity_correction(e);
		return;
	}
	e->y_size *= 0.4F;
	float entity_requested = requested;
	if(e->in_web) {
		e->in_web = false;
		entity_requested *= axis == 1
			? 0.05000000074505806F : 0.25F;
		glm_vec3_zero(e->vel);
	}
	struct pmove_move_result move;
	pmove_move_body(s, &body, axis, entity_requested, e->step_height,
					 e->on_ground, e->y_size < 0.05F, e, &move);
	if(move.stepped) {
		float fractional_min_y = body.y1 - (float)(int)body.y1;
		if(fractional_min_y > 0.0F)
			e->y_size += fractional_min_y + 0.01F;
	}
	entity_set_position_from_bbox(e, &body);
	for(int k = 0; k < 3; k++)
		if(move.clipped[k])
			e->vel[k] = 0.0F;
	e->on_ground = move.on_ground;
	e->collided_horizontally = move.clipped[0] || move.clipped[2];
	e->collided_vertically = move.clipped[1];
	e->collided = e->collided_horizontally || e->collided_vertically;
	if(e->type == ENTITY_MONSTER) {
		e->data.monster.prev_on_ground = e->on_ground;
		e->data.monster.prev_collided_xz = e->collided_horizontally;
	}
	pmove_update_fall_state(e, move.resolved[1]);
	/* TileEntityPiston delegates to Entity.moveEntity. Its updateFallState is
	 * followed by the same final-AABB block callback scan as ordinary motion. */
	if(e->type == ENTITY_MONSTER) {
		entity_monster_post_move_cactus_collisions(e, s);
		if(e->health <= 0) {
			vec3 here = {e->pos[0], e->pos[1], e->pos[2]};
			entity_monster_begin_death(e, s, here);
		}
	}
	if(e->type == ENTITY_MINECART
	   && ((move.clipped[0] && e->data.minecart.hx != 0)
		   || (move.clipped[2] && e->data.minecart.hz != 0)))
		e->data.minecart.speed = 0.0F;
	if(e->type == ENTITY_MINECART)
		entity_minecart_mark_snapshot_dirty(e, true);
	server_local_refresh_entity_membership(s, e);
	pmove_send_entity_correction(e);
}

static void pmove_push_entities(struct server_local* s, size_t i,
								const struct piston_moving_state* state,
								float progress, float amount,
								struct pmove_player_correction corrections[2]) {
	struct AABB push_box;
	if(!pmove_push_box(s, i, state, progress, &push_box))
		return;
	int dx, dy, dz;
	piston_moving_facing_offset(state->facing, &dx, &dy, &dz);
	int axis = dx ? 0 : (dy ? 1 : 2);
	float requested = amount * (float)(dx ? dx : (dy ? dy : dz));
	size_t ref_count = 0;
	const struct server_entity_query_ref* refs
		= server_local_entity_query_snapshot(s, &ref_count);
	for(size_t r = 0; r < ref_count; r++) {
		if(refs[r].player) {
			/* v5: each player body at its OWN section-list position. */
			struct server_player* pl
				= server_local_query_ref_body(s, &refs[r]);
			if(!pl || !pmove_actor_in_pass_world(s, pl))
				continue; /* only the bodies in this pass's world */
			struct pmove_player_correction* correction
				= pl == &s->player ? &corrections[0] : &corrections[1];
			if(pl != &s->player
			   && correction->actor.generation != s->guest_generation)
				continue;
			pmove_push_player_candidate(s, pl, &push_box, axis, requested,
				correction);
			continue;
		}
		if(!pmove_query_ref_visible(&refs[r], &push_box))
			continue;
		else
			pmove_push_entity_candidate(
				s, refs[r].entity, &push_box, axis, requested);
	}
}

// --- the per-tick settle driver (Beta TileEntityPiston.updateEntity) --------------------------------
/* Ticks the moving TEs owned by block_world(s): every live world runs it in its
 * own tile phase (World.updateEntities is per World in Beta), the secondary
 * worlds under block_world_enter. Its entity query and the pushed bodies are
 * that world's only. */
void piston_moving_te_tick(struct server_local* s) {
	if(!s)
		return;
	/* Beta snapshots list membership at pass entry. Orders are monotonic, so the
	 * cutoff is both an unbounded born-during-pass marker and a stable list-order
	 * cursor; no stack worklist or fixed-width live mask is needed. */
	uint64_t cutoff = s->piston_moving.next_order;
	s_settle_pass_active = true;
	s_settle_visible_order = cutoff;
	struct pmove_player_correction player_corrections[2] = {
		{.actor = ACTOR_IDENT_HOST},
		{.actor = {.slot = ACTOR_SLOT_GUEST,
					   .generation = s->guest_actor ? s->guest_generation : 0}},
	};
	uint64_t visited_order = 0;
	/* A1b: this pass ticks slot 0's entries in their (shared, monotonic)
	 * loaded-list order; another world's entries keep their own relative
	 * order for that world's pass. */
	struct piston_moving_owner owner = pmove_owner(s);
	for(;;) {
		if(server_local_light_stopped(s))
			break; /* G9 R3: a light stop ends the loop at once */
		size_t i = PMOVE_NO_INDEX;
		uint64_t order = 0;
		bool found = false;
		for(size_t candidate = 0; candidate < s->piston_moving.capacity;
			candidate++) {
			const struct piston_moving_store_entry* entry
				= &s->piston_moving.entries[candidate];
			if(!entry->used || !piston_moving_owner_matches(entry, owner)
			   || entry->order <= visited_order
			   || entry->order > cutoff
			   || (found && entry->order >= order))
				continue;
			i = candidate;
			order = entry->order;
			found = true;
		}
		if(!found)
			break;
		visited_order = order;
		/* A prior settle notification can remove this entry and reuse its sparse
		 * slot. The replacement necessarily has a different, post-cutoff order. */
		if(!s->piston_moving.entries[i].used
		   || s->piston_moving.entries[i].order != order)
			continue;
		struct piston_moving_state state;
		struct piston_moving_tick_result result;
		pmove_read_slot(s, i, &state);
		piston_moving_advance(&state, &result);
		/* Capture unbounded Chunk-list comparator keys before the first
		 * candidate moves. If scratch growth fails, leave this TE's progress and
		 * settle state byte-for-byte intact so its whole visit retries. */
		if(result.push_entities
		   && !server_local_prepare_entity_query_snapshot_bounded_w(
			   s, block_world(s), INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX,
			   INT32_MIN, INT32_MAX)) {
			static bool warned = false;
			if(!warned) {
				warned = true;
				fprintf(stderr,
						"[piston] entity query snapshot allocation failed; "
						"moving TE deferred intact\n");
			}
			continue;
		}
		pmove_write_progress(s, i, &state);
		if(result.push_entities)
			pmove_push_entities(s, i, &state, result.push_progress,
								result.push_amount, player_corrections);
		if(result.settle && s->piston_moving.entries[i].used
		   && s->piston_moving.entries[i].order == order)
			pmove_settle_slot(s, i);
	}
	s_settle_pass_active = false;
	s_settle_visible_order = UINT64_MAX;
	if(player_corrections[0].needed) {
		/* The client owns ordinary walking, so re-author the final result after every TE in list order. */
		s->reject_player_pos_this_update = true;
		/* ...and the per-actor input latch every body's queued PLAYER_POS/
		 * PLAYER_STATE is now gated on (S1-a2 split the one flag in two;
		 * R-S1a2 F2). One call, both bodies -- see
		 * server_local_raise_actor_input_barriers. */
		server_local_raise_actor_input_barriers(s);
		server_local_next_player_correction(s);
		clin_rpc_send(&(struct client_rpc) {
			.type = CRPC_PLAYER_POS,
			.payload.player_pos.position = {
				s->player.x, s->player.y, s->player.z,
			},
			.payload.player_pos.rotation = {s->player.rx, s->player.ry},
			.payload.player_pos.correction_seq = s->player.correction_seq,
			.payload.player_pos.correction_flags = player_corrections[0].flags,
		});
	}
	if(player_corrections[1].needed)
		(void)server_local_guest_correct_position_for(
			s, player_corrections[1].actor.generation,
			player_corrections[1].flags);
}

bool piston_moving_stored(struct server_local* s, int x, int y, int z, struct item_data* out) {
	size_t i = pmove_find(s, x, y, z);
	if(i == PMOVE_NO_INDEX)
		return false;
	if(out) {
		out->id = s->piston_moving.entries[i].state.stored_id;
		out->durability = s->piston_moving.entries[i].state.stored_meta;
		out->count = 1;
	}
	return true;
}
