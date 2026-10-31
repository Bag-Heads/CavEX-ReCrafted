/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

/*
	Bridge between the server's resident chest/sign tables and a per-chunk
	chunk_tile_entities (Stage 2 of the chest-store residency redesign,
	docs/chest_store_redesign.md).

	The server keeps chests/signs as Structure-of-Arrays in server_local. These
	helpers take that layout as an explicit `te_store` view rather than a
	`server_local*`, so the lifecycle logic (chunk filter, overwrite-by-position
	de-dup, free-on-evict) is free of server_local's heavy dependencies and is
	unit-testable (tests/test_tile_entities_store.c).
*/

#ifndef TILE_ENTITIES_STORE_H
#define TILE_ENTITIES_STORE_H

#include "../block/dispenser_logic.h" // struct dispenser_te (dep-light: items.h + util.h)
#include "../block/furnace_logic.h" // struct furnace_te (dep-light: items.h only)
#include "../block/piston_moving.h"
#include "spawner_mob_id.h"	// struct spawner_te (dependency-light; not spawner.h's burst core)
#include "tile_entities_nbt.h" // chunk_tile_entities, te_chest/te_sign, item_data
#include "storage_rc.h" // A2-S: prepare/preflight/commit result convention

#include <assert.h>
#include <stddef.h>

// A position triple. Layout-compatible with server_local's complex_block_pos
// ({int x,y,z}); y < 0 marks an empty slot (the server's convention).
struct te_pos {
	int x, y, z;
};

/* A1b (split-dimensions D9): the seven FIXED tile tables, in the order the
 * session's owner-key table lays them out. Piston moving blocks are dynamic
 * and carry their owner inside each store entry instead. */
enum te_kind {
	TE_KIND_CHEST = 0,
	TE_KIND_SIGN,
	TE_KIND_FURNACE,
	TE_KIND_DISPENSER,
	TE_KIND_NOTE,
	TE_KIND_RECORD_PLAYER,
	TE_KIND_SPAWNER,
	TE_KIND_FIXED_COUNT
};

/* Parallel owner keys for one fixed table: slot[i]/generation[i] name the
 * world instance (server_local.worlds[] slot + its process-unique generation)
 * that owns occupied row i. A FREE row (pos.y < 0) always carries {0,0}; the
 * release paths reset it. An OCCUPIED row is always stamped by its writer;
 * an unstamped occupied row belongs to no world. Dimension is NOT part of this key: it belongs to the persistent
 * archive identity; generation is the transient runtime identity. */
struct te_owner_view {
	uint8_t* slot;
	uint32_t* generation;
};

/* Is a row stamped (row_slot,row_gen) owned by world (slot,gen)? */
static inline bool te_owner_matches(uint8_t row_slot, uint32_t row_gen,
									uint8_t slot, uint32_t gen) {
	/* Exact live instance: generation 0 is never a live world, so an
	 * unstamped row belongs to nobody (no slot-0 fallback). */
	return gen != 0 && row_slot == slot && row_gen == gen;
}

/* ===== IMP-TE-CAPS growable backing (2026-10-21) ===========================
 * Beta keeps tile entities in an unbounded HashMap (Chunk.chunkTileEntityMap,
 * World.loadedTileEntityList), so the seven fixed kinds are no longer fixed-
 * size arrays: each kind's parallel arrays are heap slabs (MEM2 on Wii, via
 * the owner's allocator) that grow x1.5 when a claim or a chunk-load
 * preflight needs a row and none is free. Rows are append-only: a row index
 * never moves, so windows and lookups that hold a row index stay valid; a raw
 * POINTER into a slab is valid only until the next growth (no caller keeps
 * one across a claim/load -- the view epoch below asserts it).
 *
 * The owner (server_local) describes its slabs once with a te_slab_kind per
 * kind: where each array pointer and the capacity live inside the owner, the
 * element size, the fill byte for new rows, and which te_store field mirrors
 * the array. te_store_grow reallocates every array of one kind together
 * (all-or-nothing: on allocation failure nothing changes and the failure is
 * logged and counted), then re-points the view it was called through. */
#define TE_SLAB_FIELDS_MAX 6
#define TE_SLAB_NO_VIEW ((size_t)-1)
struct te_slab_field {
	size_t elem;		 /* bytes per row; 0 = unused entry */
	size_t owner_offset; /* offsetof(owner, the array pointer) */
	size_t view_offset;	 /* offsetof(struct te_store, mirror) or TE_SLAB_NO_VIEW */
	unsigned char fill;	 /* byte pattern of a fresh row */
};
struct te_slab_kind {
	size_t cap_offset;		/* offsetof(owner, int row capacity) */
	size_t view_max_offset; /* offsetof(struct te_store, max_<kind>) */
	int initial_rows;
	struct te_slab_field fields[TE_SLAB_FIELDS_MAX];
};
/* Overflow guard only (rows x bytes must fit int/size_t); never reachable by
 * a real world: 2^22 chest rows would be 1.4 GB. */
#define TE_STORE_ROWS_LIMIT (1 << 22)
struct te_store;
struct te_grow_ctx {
	void* owner;
	const struct te_slab_kind* kinds; /* [TE_KIND_FIXED_COUNT]; NULL = fixed */
	void* (*alloc)(size_t bytes);
	void (*release)(void* p);
	uint32_t epoch; /* bumped by every growth; views snapshot it */
	/* = te_store_grow, set by te_grow_ctx_init. Header-only users (the
	 * static inline claim helpers) grow through this pointer, so a target
	 * that never links tile_entities_store.c (a fixed hand-built view)
	 * still links. */
	bool (*grow_fn)(const struct te_store* st, int kind, int min_rows);
	/* diagnostics (memory report / tests) */
	size_t bytes_live, bytes_peak;
	uint32_t grow_events, grow_failures;
};

// A view over the server's chest/sign tables. *_items / *_texts are row-major:
// chest c's items start at chest_items[c * chest_slots], sign g's text at
// sign_texts[g * sign_size]. chest_slots should equal TE_CHEST_SLOTS and
// sign_size equal TE_SIGN_SIZE; the helpers clamp the copy length either way so
// a mismatch can never read or write out of bounds.
struct te_store {
	struct te_pos* chest_pos;
	struct item_data* chest_items;
	int max_chest;
	int chest_slots;
	struct te_pos* sign_pos;
	char* sign_texts;
	int max_sign;
	int sign_size;
	// Furnaces: furnace_pos[i] is layout-compatible with the runtime
	// complex_block_pos (y < 0 == empty); furnaces[i] is the live furnace_te
	// (burn/cook + the 3 ItemStacks). Insert copies the persisted te_furnace into
	// it (leaving current_item_burn_time 0 -- the server recomputes it from the
	// fuel slot on load, mirroring Beta readFromNBT); gather copies it back out.
	// max_furnace == 0 (NULL arrays) cleanly disables furnace handling for a view
	// that doesn't track them (e.g. the chest/sign-only store unit test).
	struct te_pos* furnace_pos;
	struct furnace_te* furnaces;
	int max_furnace;
	// Dispensers (dispenser-slice): dispenser_pos[i] is layout-compatible with
	// the runtime complex_block_pos (y < 0 == empty); dispensers[i] is the live
	// dispenser_te (the 9-slot inventory + its own dispenserRandom -- NOT
	// persisted, see dispenser_logic.h). Insert copies the persisted
	// te_dispenser's items into it and reseeds dispenser_random (mirrors the
	// furnace fixup, done by dispenser_server_load_fixup after the insert, not
	// inline here -- this module stays free of util.h's time()-based entropy).
	// max_dispenser == 0 (NULL arrays) cleanly disables dispenser handling for
	// a view that doesn't track them.
	struct te_pos* dispenser_pos;
	struct dispenser_te* dispensers;
	int max_dispenser;
	// Note blocks (noteblock-slice): note_pos[i] is layout-compatible with the
	// runtime complex_block_pos (y < 0 == empty); notes[i] is the persisted
	// 0..24 pitch value. note_prev_powered[i] is a LIVE-ONLY sidecar (Beta
	// TileEntityNote.previousRedstoneState -- never persisted, see
	// tile_entities_nbt.h's te_note); insert ALWAYS clears it to false,
	// matching a fresh TileEntityNote() construction on every chunk load.
	// max_note == 0 (NULL arrays) cleanly disables note handling.
	struct te_pos* note_pos;
	int8_t* notes;
	bool* note_prev_powered;
	int max_note;
	// Jukeboxes (jukebox-slice): record_player_pos[i]/records[i] mirror the
	// shape above; records[i] == 0 means no record inserted (matches Beta's
	// Record NBT tag being written only when > 0, see te_record_player).
	// max_record_player == 0 disables.
	struct te_pos* record_player_pos;
	int32_t* records;
	int max_record_player;
	// FH-17: MobSpawner tile entities: spawner_pos[i]/spawners[i] mirror the
	// shape above (layout-compatible complex_block_pos + a position-less
	// live record, server_spawner.c). max_spawner == 0 disables.
	struct te_pos* spawner_pos;
	struct spawner_te* spawners;
	int max_spawner;
	// Piston moving blocks use the server's dynamic sparse ownership directly.
	// NBT persists last progress; readFromNBT restores both floats. NULL disables
	// piston handling for chest/sign-only store views.
	struct piston_moving_store* piston_moving;
	/* A1b owner filter. owner_keyed == true (every production view, built by
	 * server_world_tile_store): every lookup, replacement, preflight, gather,
	 * release and piston operation sees only rows owned by
	 * (owner_slot, owner_generation), and every insertion stamps that owner.
	 * owner_keyed == false is the legacy UNKEYED view the pure lifecycle unit
	 * tests build by hand: every row is visible, nothing is stamped. */
	bool owner_keyed;
	uint8_t owner_slot;
	uint32_t owner_generation;
	struct te_owner_view owners[TE_KIND_FIXED_COUNT];
	/* A2-S restore reservations (astra-a2s-design-0926 "reserved free-row
	 * masks"): one byte per row of the SHARED table, set by
	 * te_restore_preflight on the free rows a pending reload will install
	 * into, cleared by commit/abort. Every claim path treats a reserved row
	 * as NOT free, so an intervening claim by the other world cannot steal a
	 * row the plan counted. NULL for a kind without a mask (never reserved). */
	uint8_t* reserved[TE_KIND_FIXED_COUNT];
	/* IMP-TE-CAPS: growable backing (NULL = fixed-size view, e.g. a unit
	 * test's hand-built arrays) and the growth epoch this view was built or
	 * last re-pointed at. A view whose epoch lags its ctx points at freed
	 * slabs: every row helper asserts against that. */
	struct te_grow_ctx* grow;
	uint32_t grow_epoch;
};

/* Grow every array of `kind` to at least `min_rows` rows (x1.5 steps),
 * preserving every existing row and filling new rows as free. Re-points
 * `st` (the const is cast away: views are always caller-owned locals). False,
 * with nothing changed, when the view has no growable backing, min_rows
 * exceeds TE_STORE_ROWS_LIMIT, or an allocation fails (logged with reason). */
bool te_store_grow(const struct te_store* st, enum te_kind kind, int min_rows);
/* Make sure `kind` has at least `free_rows` claimable (free, unreserved)
 * rows, growing once if not. Same failure contract as te_store_grow. */
bool te_store_reserve_free(const struct te_store* st, enum te_kind kind,
						   int free_rows);
/* Allocate the initial slabs of every kind (initial_rows each, all free).
 * False (logged) on allocation failure; whatever was allocated is freed. */
bool te_grow_ctx_init(struct te_grow_ctx* g, void* owner,
					  const struct te_slab_kind* kinds,
					  void* (*alloc)(size_t), void (*release)(void*));
/* Free every slab and zero the capacities (the owner is unusable after). */
void te_grow_ctx_free(struct te_grow_ctx* g);

static inline bool te_store_grow_via(const struct te_store* st,
									 enum te_kind kind, int min_rows) {
	return st && st->grow && st->grow->grow_fn
		&& st->grow->grow_fn(st, (int)kind, min_rows);
}

static inline void te_store_assert_fresh(const struct te_store* st) {
	assert(!st->grow || st->grow_epoch == st->grow->epoch);
	(void)st;
}

static inline bool te_reserved(const struct te_store* st, enum te_kind kind,
							   int i) {
	return st->reserved[kind] && st->reserved[kind][i] != 0;
}

/* A2S-P1 review R2-H1: an occupied row stamped for THIS view's slot by ANY
 * generation (a previous incarnation of the same world slot: a portal trip,
 * a respawn, a reload) is the target's own stale row -- replaceable and
 * released by a restore commit, exactly like the pre-A2-S per-slot release
 * did regardless of generation. It is never "foreign". */
static inline bool te_same_slot_stale(const struct te_store* st,
									  enum te_kind kind,
									  const struct te_pos* p, int i) {
	if(!st->owner_keyed || p[i].y < 0)
		return false;
	const struct te_owner_view* o = &st->owners[kind];
	if(!o->slot || !o->generation)
		return false;
	return o->slot[i] == st->owner_slot
		&& o->generation[i] != st->owner_generation;
}

/* A2-S (astra-a2s-design-0926 "tile_restore_plan"): a flat-archive reload
 * decoded into SCRATCH with an exact destination-row mapping, so the shared
 * table is mutated only in an allocation-free commit -- and never at all when
 * any fallible step refuses. Chest and sign images are prepared TOGETHER
 * (complex_archive_prepare_w), preflighted against the target world's view
 * (te_restore_preflight: mapping + reservations), then committed
 * (te_restore_commit) or aborted (te_restore_abort). Foreign rows (owned by
 * another world) keep their index and bytes; incoming rows take their
 * original index when it is replaceable (owned by the target) or free, else
 * the first such row in ascending order; duplicates and source order are
 * preserved. Single world: every row is replaceable or free, so the mapping
 * is the identity and the table bytes match the pre-A2-S full-image read. */
#define TE_RESTORE_PLAN_API 1
struct tile_restore_plan {
	bool prepared;
	bool preflighted;
	int dim; /* enum world_dim (kept int: world.h is not visible here) */
	/* decoded scratch images (row-major like the store) */
	struct te_pos* chest_pos;
	struct item_data* chest_items;
	int chest_max, chest_slots;
	struct te_pos* sign_pos;
	char* sign_texts;
	int sign_max, sign_size;
	/* per incoming row: destination row, or -1 for an empty incoming row */
	int16_t* chest_dest;
	int16_t* sign_dest;
	int chest_incoming, sign_incoming;
	/* preflight target */
	const struct te_store* st;
};

/* Map every occupied incoming row onto `st`'s table and reserve the free
 * rows it will take. STORAGE_CAPACITY when the incoming rows do not fit into
 * (replaceable + unreserved free) rows: nothing reserved, nothing mutated. */
enum storage_rc te_restore_preflight(const struct te_store* st,
									 struct tile_restore_plan* plan);
/* Revalidate every destination (still replaceable or free), then release the
 * target's rows (and blank every non-foreign row's payload, the old
 * full-image wipe minus foreign rows) and install the mapped rows. False =
 * revalidation failed; the table is untouched and the plan is aborted. */
bool te_restore_commit(struct tile_restore_plan* plan);
/* Release reservations and scratch; the table is untouched. Idempotent. */
void te_restore_abort(struct tile_restore_plan* plan);
#ifdef CAVEWII_TILE_ENTITIES_STORE_TEST
/* Fail the Nth scratch allocation of the next prepare (0 = disarm). */
void te_restore_test_fail_alloc(unsigned nth);
#endif
void* te_restore_alloc(size_t n); /* prepare's allocator (test-failable) */

/* ===== A1b row helpers (owner-filtered positional access) ================
 * The positional row API every tile kind shares. All of them honour the
 * view's owner filter: another world's same-coordinate row is invisible, is
 * never replaced, and is never freed. */
// Row index of the occupied, owned row at (x,y,z), or -1.
/* Existing owned row at (x,y,z), else the first free row, claimed: its
 * position is written and its owner stamped. Payload is NOT touched (callers
 * initialize it, exactly as the pre-A1b register helpers did). -1 when the
 * shared table is full. *existed (optional) reports which case applied. */
/* The first FREE row, claimed for (x,y,z) even if this world already owns a
 * row there (the historical sign-placement scan's exact semantics). -1 when
 * the shared table is full. */
// Mark an owned row free (pos.y = -1, owner reset). No-op if not owned.
// Is row occupied AND owned by this view's world?
// Occupied rows of `kind` owned by this view's world.
// Free rows of `kind` in the SHARED table (capacity left for either world).
// This view's piston owner key ({0,0} for an unkeyed view).

/* ===== A1b owner filter + row helpers (static inline: linked by every
 * store user, including dependency-light test targets) ============== */

static inline struct te_pos* te_kind_pos(const struct te_store* st,
								  enum te_kind kind, int* max) {
	te_store_assert_fresh(st);
	switch(kind) {
		case TE_KIND_CHEST: *max = st->max_chest; return st->chest_pos;
		case TE_KIND_SIGN: *max = st->max_sign; return st->sign_pos;
		case TE_KIND_FURNACE: *max = st->max_furnace; return st->furnace_pos;
		case TE_KIND_DISPENSER:
			*max = st->max_dispenser;
			return st->dispenser_pos;
		case TE_KIND_NOTE: *max = st->max_note; return st->note_pos;
		case TE_KIND_RECORD_PLAYER:
			*max = st->max_record_player;
			return st->record_player_pos;
		case TE_KIND_SPAWNER: *max = st->max_spawner; return st->spawner_pos;
		default: *max = 0; return NULL;
	}
}

// Owner test only (the caller checks occupancy).
static inline bool te_owned(const struct te_store* st, enum te_kind kind, int i) {
	if(!st->owner_keyed)
		return true;
	const struct te_owner_view* o = &st->owners[kind];
	if(!o->slot || !o->generation)
		return false; /* a keyed view without keys can own nothing */
	return te_owner_matches(o->slot[i], o->generation[i], st->owner_slot,
							st->owner_generation);
}

static inline bool te_live(const struct te_store* st, enum te_kind kind,
					const struct te_pos* p, int i) {
	return p[i].y >= 0 && te_owned(st, kind, i);
}

static inline void te_stamp(const struct te_store* st, enum te_kind kind, int i) {
	if(!st->owner_keyed)
		return;
	const struct te_owner_view* o = &st->owners[kind];
	if(o->slot && o->generation) {
		o->slot[i] = st->owner_slot;
		o->generation[i] = st->owner_generation;
	}
}

static inline void te_unstamp(const struct te_store* st, enum te_kind kind, int i) {
	const struct te_owner_view* o = &st->owners[kind];
	if(st->owner_keyed && o->slot && o->generation) {
		o->slot[i] = 0;
		o->generation[i] = 0;
	}
}

// Release one row: free marker plus owner reset (free rows are {0,0}).
static inline void te_release_row(const struct te_store* st, enum te_kind kind,
						   struct te_pos* p, int i) {
	p[i].y = -1;
	te_unstamp(st, kind, i);
}

static inline struct piston_moving_owner te_store_piston_owner(const struct te_store* st) {
	if(!st || !st->owner_keyed)
		return (struct piston_moving_owner) {0, 0};
	return (struct piston_moving_owner) {st->owner_slot, st->owner_generation};
}

static inline int te_store_find(const struct te_store* st, enum te_kind kind, int x, int y,
				  int z) {
	if(!st)
		return -1;
	int max;
	struct te_pos* p = te_kind_pos(st, kind, &max);
	if(!p)
		return -1;
	for(int i = 0; i < max; i++)
		if(te_live(st, kind, p, i) && p[i].x == x && p[i].y == y
		   && p[i].z == z)
			return i;
	return -1;
}

static inline int te_store_claim(const struct te_store* st, enum te_kind kind, int x, int y,
				   int z, bool* existed) {
	if(existed)
		*existed = false;
	if(!st || y < 0)
		return -1;
	int max;
	struct te_pos* p = te_kind_pos(st, kind, &max);
	if(!p)
		return -1;
	int freeslot = -1;
	for(int i = 0; i < max; i++) {
		if(te_live(st, kind, p, i)) {
			if(p[i].x == x && p[i].y == y && p[i].z == z) {
				if(existed)
					*existed = true;
				return i;
			}
		} else if(freeslot < 0 && p[i].y < 0 && !te_reserved(st, kind, i)) {
			freeslot = i;
		}
	}
	/* IMP-TE-CAPS: no free row -> grow; the first free row is then the old
	 * capacity (append-only), exactly the row a larger fixed table gave. */
	if(freeslot < 0 && te_store_grow_via(st, kind, max + 1)) {
		int old_max = max;
		p = te_kind_pos(st, kind, &max);
		for(int i = old_max; i < max; i++)
			if(p[i].y < 0 && !te_reserved(st, kind, i)) {
				freeslot = i;
				break;
			}
	}
	if(freeslot >= 0) {
		p[freeslot] = (struct te_pos) {x, y, z};
		te_stamp(st, kind, freeslot);
	}
	return freeslot;
}

static inline int te_store_claim_free(const struct te_store* st, enum te_kind kind, int x,
						int y, int z) {
	if(!st || y < 0)
		return -1;
	int max;
	struct te_pos* p = te_kind_pos(st, kind, &max);
	for(int pass = 0; pass < 2; pass++) {
		for(int i = 0; p && i < max; i++)
			if(p[i].y < 0 && !te_reserved(st, kind, i)) {
				p[i] = (struct te_pos) {x, y, z};
				te_stamp(st, kind, i);
				return i;
			}
		/* IMP-TE-CAPS: full -> grow once; the rescan then finds row `max`. */
		if(pass || !te_store_grow_via(st, kind, max + 1))
			break;
		p = te_kind_pos(st, kind, &max);
	}
	return -1;
}

static inline void te_store_free_row(const struct te_store* st, enum te_kind kind,
					   int row) {
	if(!st)
		return;
	int max;
	struct te_pos* p = te_kind_pos(st, kind, &max);
	if(p && row >= 0 && row < max && te_live(st, kind, p, row))
		te_release_row(st, kind, p, row);
}

static inline bool te_store_row_owned(const struct te_store* st, enum te_kind kind,
						int row) {
	if(!st)
		return false;
	int max;
	struct te_pos* p = te_kind_pos(st, kind, &max);
	return p && row >= 0 && row < max && te_live(st, kind, p, row);
}

static inline int te_store_count_owned(const struct te_store* st, enum te_kind kind) {
	int max, n = 0;
	struct te_pos* p = st ? te_kind_pos(st, kind, &max) : NULL;
	for(int i = 0; p && i < max; i++)
		if(te_live(st, kind, p, i))
			n++;
	return n;
}

static inline int te_store_count_free(const struct te_store* st, enum te_kind kind) {
	int max, n = 0;
	struct te_pos* p = st ? te_kind_pos(st, kind, &max) : NULL;
	for(int i = 0; p && i < max; i++)
		if(p[i].y < 0)
			n++;
	return n;
}



// Insert a parsed chunk's chests/signs into the tables. Overwrite-by-position:
// an entry already at a chest/sign's (x,y,z) is replaced (the per-chunk NBT
// record is authoritative for a freshly loaded chunk, which also de-dups a chest
// a legacy chests.dat already supplied); otherwise the first free slot (y<0) is
// claimed. Entries that don't fit (tables full == loaded working set overflow)
// are dropped with a warning -- never silently.
// Non-mutating capacity/shape preflight. Under the server's single-threaded
// world ownership, a successful preflight guarantees the following insert.
bool te_store_can_insert(const struct te_store* st,
						 const struct chunk_tile_entities* te);

/* Capacity preflight for a marked full-image chunk. Existing flat-backed
 * chest/sign rows in (cx,cz) are treated as reclaimable, so a full table cannot
 * falsely reject the authoritative replacement. Furnace/piston owners are not
 * legacy-backed and retain ordinary merge preflight semantics. */
bool te_store_can_replace_legacy_chunk(
	const struct te_store* st, const struct chunk_tile_entities* te,
	int cx, int cz);
// Returns false without mutating any table if the complete parsed set cannot
// fit. Partial insertion would make a later save erase the omitted disk TEs.
bool te_store_insert(const struct te_store* st,
					 const struct chunk_tile_entities* te);

// Mirror Beta Chunk.setChunkBlockTileEntity's host-block gate for restored
// TileEntityPiston records. `ids` is one 16x16xWORLD_HEIGHT chunk in the
// server archive's x-major/z/y layout. Invalid/stale piston records are
// compacted out in place before capacity preflight and insertion; the relative
// order of retained records is preserved. Returns false only for malformed
// arguments and reports how many stale records were removed.
bool te_store_filter_piston_hosts(struct chunk_tile_entities* te, int cx,
								 int cz, const uint8_t* ids,
								 size_t* removed);

// Gather every chest/sign whose (x,z) falls in chunk (cx,cz) [chunk coords] into
// *out (calloc'd arrays; free with tile_entities_parse_free). Read-only on the
// tables -- the runtime slots are freed separately by te_store_release on evict.
bool te_store_gather(const struct te_store* st, int cx, int cz,
					 struct chunk_tile_entities* out);

#ifdef CAVEWII_TILE_ENTITIES_STORE_TEST
// Fail one allocation after `successful_allocations` successful gather
// allocations. The failure is one-shot and production builds omit the hook.
void te_store_test_fail_allocation_after(unsigned successful_allocations);
#endif

// Free (mark empty) the table slots for every chest/sign in chunk (cx,cz). Call
// on evict AFTER the chunk's tile entities are persisted, so the resident set
// shrinks with the loaded chunk set.
void te_store_release(const struct te_store* st, int cx, int cz);

/* Remove only flat-archive-backed chest/sign rows before inserting a marked
 * chunk full image. Call only after every fallible load/restore step succeeds;
 * te_store_can_replace_legacy_chunk guarantees the following insert. */
void te_store_release_legacy_chunk(const struct te_store* st, int cx, int cz);

#endif
