/*
	Shared Beta 1.7.3 TileEntityPiston state and progress math.

	Ground truth: decompile 740c583 TileEntityPiston. The two progress fields are
	deliberately floats, not a countdown: readFromNBT accepts any finite progress
	in [0,1], sets BOTH fields to that value, and updateEntity advances the leading
	field by 0.5 while the lagging field follows one visit behind.
*/

#ifndef PISTON_MOVING_H
#define PISTON_MOVING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PISTON_MOVING_FLAG_EXTENDING 0x01u
/* field_31023_j: true only for the retracting source base. It is not "is head". */
#define PISTON_MOVING_FLAG_SOURCE    0x02u

struct piston_moving_state {
	uint8_t stored_id;
	uint8_t stored_meta;
	uint8_t facing;
	uint8_t flags;
	float current_progress; /* field_31022_k */
	float last_progress;    /* field_31020_l */
};

/*
	Unbounded-by-policy runtime ownership for TileEntityPiston. The backing array
	grows geometrically with checked arithmetic; sparse slots keep indices stable
	through re-entrant settle notifications, while `order` preserves Beta's loaded
	tile-entity list order when a freed slot is reused. A store operation either
	reserves before publication or leaves the previous entries untouched.
*/
/* A1b (split-dimensions D9): an entry's owning world instance, the same
 * (slot, generation) key the fixed tile tables carry. It lives in the entry's
 * existing tail padding (sizeof stays 40 on Wii and PC), so the dynamic store
 * is still ONE shared payload array for both worlds. {0,0} is an unstamped
 * entry (client mirror store / owner-agnostic API only); no server world
 * owns it -- piston_moving_owner_matches requires an exact live instance. */
struct piston_moving_owner {
	uint8_t slot;
	uint32_t generation;
};

struct piston_moving_store_entry {
	int32_t x, y, z;
	struct piston_moving_state state;
	uint64_t order;
	bool used;
	uint8_t owner_slot;
	uint32_t owner_generation;
};

struct piston_moving_store {
	struct piston_moving_store_entry* entries;
	size_t count;
	size_t capacity;
	/* Future insertions claimed by active, possibly nested piston moves. */
	size_t reserved;
	uint64_t next_order;
};

struct piston_moving_store_reservation {
	size_t remaining;
};

void piston_moving_store_init(struct piston_moving_store* store);
void piston_moving_store_clear(struct piston_moving_store* store);
void piston_moving_store_destroy(struct piston_moving_store* store);
/* Reserve `additional` new entries without publishing any of them. */
bool piston_moving_store_reserve(
	struct piston_moving_store* store, size_t additional);
/* Scoped claim used across a synchronous move's notifying/re-entrant writes. */
bool piston_moving_store_reservation_begin(
	struct piston_moving_store* store, size_t entries,
	struct piston_moving_store_reservation* reservation);
void piston_moving_store_reservation_cancel(
	struct piston_moving_store* store,
	struct piston_moving_store_reservation* reservation);
bool piston_moving_store_find(const struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z, size_t* out_index);
/* Requires a preceding sufficient reserve; never allocates. Replacing a
	position deliberately assigns a fresh loaded-list order, matching insertion. */
bool piston_moving_store_upsert_reserved(struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index);
bool piston_moving_store_upsert_claimed(struct piston_moving_store* store,
	struct piston_moving_store_reservation* reservation,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index);
bool piston_moving_store_upsert(struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index);
bool piston_moving_store_remove_index(
	struct piston_moving_store* store, size_t index);
bool piston_moving_store_remove(struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z);

/* ===== A1b owner-qualified store API =====================================
 * The un-suffixed find/upsert/remove above are OWNER-AGNOSTIC (they match any
 * entry and stamp {0,0}); they remain for the client mirror store, which holds
 * one world by construction. Every SERVER caller uses the _owned forms: a
 * lookup never sees another world's same-coordinate entry, an upsert never
 * replaces one, and the inserted entry is stamped with `owner`. Reservation,
 * capacity growth, stable indices and the global monotonic `order` are shared
 * (per-world relative order is the subsequence of that one order). */
bool piston_moving_owner_matches(const struct piston_moving_store_entry* entry,
	struct piston_moving_owner owner);
bool piston_moving_store_find_owned(const struct piston_moving_store* store,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z,
	size_t* out_index);
bool piston_moving_store_upsert_reserved_owned(
	struct piston_moving_store* store, struct piston_moving_owner owner,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index);
bool piston_moving_store_upsert_claimed_owned(
	struct piston_moving_store* store,
	struct piston_moving_store_reservation* reservation,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index);
bool piston_moving_store_upsert_owned(struct piston_moving_store* store,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index);
bool piston_moving_store_remove_owned(struct piston_moving_store* store,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z);
/* Bytes the store's backing array actually holds (capacity, not count): the
 * figure the session's shared tile accounting charges. */
size_t piston_moving_store_allocated_bytes(
	const struct piston_moving_store* store);

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* One-shot allocation failure after N successful store allocations. */
void piston_moving_store_test_fail_allocation_after(
	unsigned successful_allocations);
#endif

struct piston_moving_tick_result {
	bool push_entities;
	bool settle;
	/* Argument passed to TileEntityPiston.func_31010_a before its direction transform. */
	float push_progress;
	float push_amount;
};

enum piston_moving_render_branch {
	PISTON_MOVING_RENDER_NONE = 0,
	PISTON_MOVING_RENDER_HEAD_SHORT,
	PISTON_MOVING_RENDER_RETRACT_SOURCE,
	PISTON_MOVING_RENDER_STORED,
};

struct piston_moving_render_plan {
	enum piston_moving_render_branch branch;
	float progress;
	float translation[3];
	bool full_rod;
	bool stationary_base;
};

bool piston_moving_is_extending(const struct piston_moving_state* state);
bool piston_moving_is_source(const struct piston_moving_state* state);

/* TileEntityPiston.getProgress(partial): clamps only partial > 1, exactly like Beta. */
float piston_moving_progress(const struct piston_moving_state* state,
							 float partial);

/* Render translation along the facing axis, before multiplying by the facing offset. */
float piston_moving_render_scalar(const struct piston_moving_state* state,
							  float partial);

/* BlockPistonMoving collision/selection shift, using getProgress(0) == last. */
float piston_moving_collision_shift(const struct piston_moving_state* state);

/* func_31010_a's AABB shift for its explicit progress argument. */
float piston_moving_push_shift(const struct piston_moving_state* state,
						   float progress);

/* One exact TileEntityPiston.updateEntity visit; world/entity side effects are returned. */
void piston_moving_advance(struct piston_moving_state* state,
						   struct piston_moving_tick_result* result);

/* TileEntityPiston.clearPistonTileEntity. False means last_progress was already >= 1. */
bool piston_moving_force_finish(struct piston_moving_state* state);

void piston_moving_facing_offset(uint8_t facing, int* x, int* y, int* z);

/* Exact TileEntityRendererPiston branch/transform decision. */
void piston_moving_make_render_plan(const struct piston_moving_state* state,
									float partial,
									struct piston_moving_render_plan* out);

#endif
