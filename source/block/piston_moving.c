#include "piston_moving.h"
#include "blocks_data.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const int OFF_X[6] = {0, 0, 0, 0, -1, 1};
static const int OFF_Y[6] = {-1, 1, 0, 0, 0, 0};
static const int OFF_Z[6] = {0, 0, -1, 1, 0, 0};

#ifdef CAVEWII_SERVER_LOCAL_TEST
static bool s_store_alloc_failure_armed;
static unsigned s_store_allocations_before_failure;

void piston_moving_store_test_fail_allocation_after(
	unsigned successful_allocations) {
	s_store_allocations_before_failure = successful_allocations;
	s_store_alloc_failure_armed = true;
}
#endif

static void* piston_moving_store_realloc(void* old, size_t size) {
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(s_store_alloc_failure_armed) {
		if(s_store_allocations_before_failure == 0) {
			s_store_alloc_failure_armed = false;
			return NULL;
		}
		s_store_allocations_before_failure--;
	}
#endif
	return realloc(old, size);
}

void piston_moving_store_init(struct piston_moving_store* store) {
	assert(store);
	memset(store, 0, sizeof(*store));
}

void piston_moving_store_clear(struct piston_moving_store* store) {
	assert(store);
	for(size_t i = 0; i < store->capacity; i++) {
		store->entries[i].used = false;
		store->entries[i].owner_slot = 0;
		store->entries[i].owner_generation = 0;
	}
	store->count = 0;
	store->reserved = 0;
	store->next_order = 0;
}

void piston_moving_store_destroy(struct piston_moving_store* store) {
	if(!store)
		return;
	free(store->entries);
	piston_moving_store_init(store);
}

bool piston_moving_store_reserve(
	struct piston_moving_store* store, size_t additional) {
	if(!store || store->reserved > SIZE_MAX - store->count)
		return false;
	size_t owned = store->count + store->reserved;
	if(additional > SIZE_MAX - owned)
		return false;
	size_t needed = owned + additional;
	if(needed <= store->capacity)
		return true;
	if(needed > SIZE_MAX / sizeof(*store->entries))
		return false;
	size_t capacity = store->capacity ? store->capacity : 16;
	while(capacity < needed) {
		if(capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	if(capacity > SIZE_MAX / sizeof(*store->entries))
		return false;
	size_t old_capacity = store->capacity;
	struct piston_moving_store_entry* entries = piston_moving_store_realloc(
		store->entries, capacity * sizeof(*entries));
	if(!entries)
		return false;
	memset(entries + old_capacity, 0,
		   (capacity - old_capacity) * sizeof(*entries));
	store->entries = entries;
	store->capacity = capacity;
	return true;
}

bool piston_moving_store_reservation_begin(
	struct piston_moving_store* store, size_t entries,
	struct piston_moving_store_reservation* reservation) {
	if(!store || !reservation || reservation->remaining != 0
	   || store->reserved > UINT64_MAX - store->next_order
	   || entries > UINT64_MAX - store->next_order - store->reserved
	   || entries > SIZE_MAX - store->reserved)
		return false;
	if(!piston_moving_store_reserve(store, entries))
		return false;
	store->reserved += entries;
	reservation->remaining = entries;
	return true;
}

void piston_moving_store_reservation_cancel(
	struct piston_moving_store* store,
	struct piston_moving_store_reservation* reservation) {
	if(!store || !reservation)
		return;
	assert(reservation->remaining <= store->reserved);
	store->reserved -= reservation->remaining;
	reservation->remaining = 0;
}

bool piston_moving_owner_matches(const struct piston_moving_store_entry* entry,
	struct piston_moving_owner owner) {
	/* Exact live instance only: an unworlded {0,0} entry (owner-agnostic API,
	 * client mirror store) belongs to no server world. */
	return entry && owner.generation != 0 && entry->owner_slot == owner.slot
		&& entry->owner_generation == owner.generation;
}

/* owner == NULL: the legacy owner-agnostic match. */
static bool store_find_impl(const struct piston_moving_store* store,
	const struct piston_moving_owner* owner, int32_t x, int32_t y, int32_t z,
	size_t* out_index) {
	if(!store)
		return false;
	for(size_t i = 0; i < store->capacity; i++)
		if(store->entries[i].used && store->entries[i].x == x
		   && store->entries[i].y == y && store->entries[i].z == z
		   && (!owner
			   || piston_moving_owner_matches(&store->entries[i], *owner))) {
			if(out_index)
				*out_index = i;
			return true;
		}
	return false;
}

bool piston_moving_store_find(const struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z, size_t* out_index) {
	return store_find_impl(store, NULL, x, y, z, out_index);
}

bool piston_moving_store_find_owned(const struct piston_moving_store* store,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z,
	size_t* out_index) {
	return store_find_impl(store, &owner, x, y, z, out_index);
}

static bool store_upsert_reserved_impl(struct piston_moving_store* store,
	const struct piston_moving_owner* owner, int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	if(!store || !state || store->next_order == UINT64_MAX
	   || store->reserved
		  > UINT64_MAX - store->next_order - UINT64_C(1))
		return false;
	size_t index;
	bool replacing = store_find_impl(store, owner, x, y, z, &index);
	if(!replacing) {
		/* Unclaimed insertions may use only capacity not protected by a
		 * synchronous move's outstanding reservation. */
		if(store->count >= store->capacity
		   || store->reserved >= store->capacity - store->count)
			return false;
		for(index = 0; index < store->capacity; index++)
			if(!store->entries[index].used)
				break;
		if(index == store->capacity)
			return false;
		store->count++;
	}
	store->entries[index] = (struct piston_moving_store_entry) {
		.x = x,
		.y = y,
		.z = z,
		.state = *state,
		.order = ++store->next_order,
		.used = true,
		.owner_slot = owner ? owner->slot : 0,
		.owner_generation = owner ? owner->generation : 0,
	};
	if(out_index)
		*out_index = index;
	return true;
}

bool piston_moving_store_upsert_reserved(struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	return store_upsert_reserved_impl(store, NULL, x, y, z, state, out_index);
}

bool piston_moving_store_upsert_reserved_owned(
	struct piston_moving_store* store, struct piston_moving_owner owner,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	return store_upsert_reserved_impl(store, &owner, x, y, z, state,
									  out_index);
}

static bool store_upsert_claimed_impl(struct piston_moving_store* store,
	struct piston_moving_store_reservation* reservation,
	const struct piston_moving_owner* owner,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	if(!store || !reservation || reservation->remaining == 0
	   || store->reserved == 0)
		return false;
	/* Convert one claimed future insertion into ordinary free capacity. This
	 * lets the shared upsert guard protect every other nested/outer claim. */
	store->reserved--;
	reservation->remaining--;
	if(store_upsert_reserved_impl(store, owner, x, y, z, state, out_index))
		return true;
	store->reserved++;
	reservation->remaining++;
	return false;
}

bool piston_moving_store_upsert_claimed(struct piston_moving_store* store,
	struct piston_moving_store_reservation* reservation,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	return store_upsert_claimed_impl(store, reservation, NULL, x, y, z, state,
									 out_index);
}

bool piston_moving_store_upsert_claimed_owned(
	struct piston_moving_store* store,
	struct piston_moving_store_reservation* reservation,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	return store_upsert_claimed_impl(store, reservation, &owner, x, y, z,
									 state, out_index);
}

static bool store_upsert_impl(struct piston_moving_store* store,
	const struct piston_moving_owner* owner, int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	if(!store || !state || store->next_order == UINT64_MAX)
		return false;
	size_t ignored;
	bool replacing = store_find_impl(store, owner, x, y, z, &ignored);
	if(!piston_moving_store_reserve(store, replacing ? 0 : 1))
		return false;
	return store_upsert_reserved_impl(store, owner, x, y, z, state,
									  out_index);
}

bool piston_moving_store_upsert(struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	return store_upsert_impl(store, NULL, x, y, z, state, out_index);
}

bool piston_moving_store_upsert_owned(struct piston_moving_store* store,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z,
	const struct piston_moving_state* state, size_t* out_index) {
	return store_upsert_impl(store, &owner, x, y, z, state, out_index);
}

bool piston_moving_store_remove_index(
	struct piston_moving_store* store, size_t index) {
	if(!store || index >= store->capacity || !store->entries[index].used)
		return false;
	store->entries[index].used = false;
	/* A freed entry is unstamped: owner keys are meaningful only while used. */
	store->entries[index].owner_slot = 0;
	store->entries[index].owner_generation = 0;
	assert(store->count > 0);
	store->count--;
	return true;
}

bool piston_moving_store_remove(struct piston_moving_store* store,
	int32_t x, int32_t y, int32_t z) {
	size_t index;
	return piston_moving_store_find(store, x, y, z, &index)
		&& piston_moving_store_remove_index(store, index);
}

bool piston_moving_store_remove_owned(struct piston_moving_store* store,
	struct piston_moving_owner owner, int32_t x, int32_t y, int32_t z) {
	size_t index;
	return piston_moving_store_find_owned(store, owner, x, y, z, &index)
		&& piston_moving_store_remove_index(store, index);
}

size_t piston_moving_store_allocated_bytes(
	const struct piston_moving_store* store) {
	return store ? store->capacity * sizeof(*store->entries) : 0;
}

bool piston_moving_is_extending(const struct piston_moving_state* state) {
	return state && (state->flags & PISTON_MOVING_FLAG_EXTENDING) != 0;
}

bool piston_moving_is_source(const struct piston_moving_state* state) {
	return state && (state->flags & PISTON_MOVING_FLAG_SOURCE) != 0;
}

float piston_moving_progress(const struct piston_moving_state* state,
							 float partial) {
	assert(state);
	if(partial > 1.0F)
		partial = 1.0F;
	return state->last_progress
		+ (state->current_progress - state->last_progress) * partial;
}

float piston_moving_render_scalar(const struct piston_moving_state* state,
							  float partial) {
	float progress = piston_moving_progress(state, partial);
	return piston_moving_is_extending(state) ? progress - 1.0F
											 : 1.0F - progress;
}

float piston_moving_collision_shift(const struct piston_moving_state* state) {
	assert(state);
	float progress = piston_moving_progress(state, 0.0F);
	return piston_moving_is_extending(state) ? 1.0F - progress : progress;
}

float piston_moving_push_shift(const struct piston_moving_state* state,
						   float progress) {
	assert(state);
	return piston_moving_is_extending(state) ? 1.0F - progress : progress - 1.0F;
}

void piston_moving_advance(struct piston_moving_state* state,
						   struct piston_moving_tick_result* result) {
	assert(state && result);
	memset(result, 0, sizeof(*result));

	/* TileEntityPiston.updateEntity exact order: lag <- leading, then test lag. */
	state->last_progress = state->current_progress;
	if(state->last_progress >= 1.0F) {
		result->push_entities = true;
		result->settle = true;
		result->push_progress = 1.0F;
		result->push_amount = 0.25F;
		return;
	}

	state->current_progress += 0.5F;
	if(state->current_progress >= 1.0F)
		state->current_progress = 1.0F;
	if(piston_moving_is_extending(state)) {
		result->push_entities = true;
		result->push_progress = state->current_progress;
		result->push_amount
			= state->current_progress - state->last_progress + 0.0625F;
	}
}

bool piston_moving_force_finish(struct piston_moving_state* state) {
	assert(state);
	if(state->last_progress >= 1.0F)
		return false;
	state->last_progress = 1.0F;
	state->current_progress = 1.0F;
	return true;
}

void piston_moving_facing_offset(uint8_t facing, int* x, int* y, int* z) {
	int valid = facing < 6;
	if(x)
		*x = valid ? OFF_X[facing] : 0;
	if(y)
		*y = valid ? OFF_Y[facing] : 0;
	if(z)
		*z = valid ? OFF_Z[facing] : 0;
}

void piston_moving_make_render_plan(const struct piston_moving_state* state,
									float partial,
									struct piston_moving_render_plan* out) {
	assert(state && out);
	memset(out, 0, sizeof(*out));
	out->progress = piston_moving_progress(state, partial);
	if(state->stored_id == 0 || out->progress >= 1.0F)
		return;
	if(state->stored_id == BLOCK_PISTON_HEAD && out->progress < 0.5F) {
		out->branch = PISTON_MOVING_RENDER_HEAD_SHORT;
		out->full_rod = false;
	} else if(piston_moving_is_source(state) && !piston_moving_is_extending(state)) {
		out->branch = PISTON_MOVING_RENDER_RETRACT_SOURCE;
		out->full_rod = out->progress < 0.5F;
		out->stationary_base = true;
	} else {
		out->branch = PISTON_MOVING_RENDER_STORED;
		out->full_rod = true;
	}
	int dx, dy, dz;
	piston_moving_facing_offset(state->facing, &dx, &dy, &dz);
	float scalar = piston_moving_render_scalar(state, partial);
	out->translation[0] = scalar * (float)dx;
	out->translation[1] = scalar * (float)dy;
	out->translation[2] = scalar * (float)dz;
}
