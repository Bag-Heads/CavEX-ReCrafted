#include "piston_moving_transport.h"

#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define PISTON_WIRE_FACING_MASK 0x07u
#define PISTON_WIRE_FLAGS_SHIFT 3u
#define PISTON_WIRE_FLAGS_MASK  0x18u

static bool piston_moving_state_valid(const struct piston_moving_state* state) {
	return state && state->stored_meta <= 15 && state->facing < 6
		&& (state->flags
			& ~(PISTON_MOVING_FLAG_EXTENDING | PISTON_MOVING_FLAG_SOURCE)) == 0
		&& state->current_progress >= 0.0F
		&& state->current_progress <= 1.0F
		&& state->last_progress >= 0.0F && state->last_progress <= 1.0F;
}

bool piston_moving_wire_encode(struct piston_moving_wire_state* out,
							   const struct piston_moving_state* state) {
	if(!out || !piston_moving_state_valid(state))
		return false;
	out->stored_id = state->stored_id;
	out->stored_meta = state->stored_meta;
	out->facing_flags = (uint8_t)(state->facing
		| ((state->flags << PISTON_WIRE_FLAGS_SHIFT) & PISTON_WIRE_FLAGS_MASK));
	memcpy(out->current_progress_bits, &state->current_progress, sizeof(float));
	memcpy(out->last_progress_bits, &state->last_progress, sizeof(float));
	return true;
}

bool piston_moving_wire_decode(const struct piston_moving_wire_state* wire,
							   struct piston_moving_state* out) {
	if(!wire || !out || wire->stored_meta > 15
	   || (wire->facing_flags
		   & ~(PISTON_WIRE_FACING_MASK | PISTON_WIRE_FLAGS_MASK)) != 0)
		return false;
	struct piston_moving_state decoded = {
		.stored_id = wire->stored_id,
		.stored_meta = wire->stored_meta,
		.facing = (uint8_t)(wire->facing_flags & PISTON_WIRE_FACING_MASK),
		.flags = (uint8_t)((wire->facing_flags & PISTON_WIRE_FLAGS_MASK)
			>> PISTON_WIRE_FLAGS_SHIFT),
	};
	memcpy(&decoded.current_progress, wire->current_progress_bits, sizeof(float));
	memcpy(&decoded.last_progress, wire->last_progress_bits, sizeof(float));
	if(!piston_moving_state_valid(&decoded))
		return false;
	*out = decoded;
	return true;
}

bool piston_moving_record_encode(struct piston_moving_chunk_record* out,
								 w_coord_t chunk_origin_x,
								 w_coord_t chunk_origin_z,
								 w_coord_t x, w_coord_t y, w_coord_t z,
								 const struct piston_moving_state* state) {
	w_coord_t local_x = x - chunk_origin_x;
	w_coord_t local_z = z - chunk_origin_z;
	if(!out || local_x < 0 || local_x >= 16 || local_z < 0 || local_z >= 16
	   || y < 0 || y >= WORLD_HEIGHT)
		return false;
	out->xz = (uint8_t)((local_x << 4) | local_z);
	out->y = (uint8_t)y;
	return piston_moving_wire_encode(&out->state, state);
}

bool piston_moving_record_decode(const struct piston_moving_chunk_record* record,
								 w_coord_t chunk_origin_x,
								 w_coord_t chunk_origin_z,
								 w_coord_t* x, w_coord_t* y, w_coord_t* z,
								 struct piston_moving_state* state) {
	if(!record || !x || !y || !z || !state || record->y >= WORLD_HEIGHT
	   || !piston_moving_wire_decode(&record->state, state))
		return false;
	*x = chunk_origin_x + (record->xz >> 4);
	*y = record->y;
	*z = chunk_origin_z + (record->xz & 15);
	return true;
}

static w_coord_t piston_chunk_of(w_coord_t block) {
	return block < 0 ? (block + 1) / 16 - 1 : block / 16;
}

static bool piston_moving_snapshot_reserve(
	struct piston_moving_chunk_snapshot* snapshot, size_t needed) {
	if(!snapshot)
		return false;
	if(needed <= snapshot->capacity)
		return true;
	if(needed > SIZE_MAX / sizeof(*snapshot->records)
	   || needed > SIZE_MAX / sizeof(*snapshot->orders))
		return false;

	size_t capacity = snapshot->capacity ? snapshot->capacity : 16;
	while(capacity < needed) {
		if(capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	if(capacity > SIZE_MAX / sizeof(*snapshot->records)
	   || capacity > SIZE_MAX / sizeof(*snapshot->orders))
		return false;

	/* Allocate both replacements before publishing either pointer. This leaves
	 * the previous reusable buffer intact if Wii MEM1 cannot satisfy a growth. */
	struct piston_moving_chunk_record* records
		= malloc(capacity * sizeof(*records));
	uint64_t* orders = malloc(capacity * sizeof(*orders));
	if(!records || !orders) {
		free(records);
		free(orders);
		return false;
	}
	if(snapshot->count > 0) {
		memcpy(records, snapshot->records,
			   snapshot->count * sizeof(*records));
		memcpy(orders, snapshot->orders,
			   snapshot->count * sizeof(*orders));
	}
	free(snapshot->records);
	free(snapshot->orders);
	snapshot->records = records;
	snapshot->orders = orders;
	snapshot->capacity = capacity;
	return true;
}

void piston_moving_chunk_snapshot_destroy(
	struct piston_moving_chunk_snapshot* snapshot) {
	if(!snapshot)
		return;
	free(snapshot->records);
	free(snapshot->orders);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void piston_moving_snapshot_swap(
	struct piston_moving_chunk_snapshot* snapshot, size_t a, size_t b) {
	if(a == b)
		return;
	struct piston_moving_chunk_record record = snapshot->records[a];
	snapshot->records[a] = snapshot->records[b];
	snapshot->records[b] = record;
	uint64_t order = snapshot->orders[a];
	snapshot->orders[a] = snapshot->orders[b];
	snapshot->orders[b] = order;
}

static void piston_moving_snapshot_sift_down(
	struct piston_moving_chunk_snapshot* snapshot, size_t root, size_t end) {
	if(root >= end)
		return;
	while(root <= (end - 1) / 2) {
		size_t child = root * 2 + 1;
		if(child < end && snapshot->orders[child] < snapshot->orders[child + 1])
			child++;
		if(snapshot->orders[root] >= snapshot->orders[child])
			return;
		piston_moving_snapshot_swap(snapshot, root, child);
		root = child;
	}
}

static void piston_moving_snapshot_sort(
	struct piston_moving_chunk_snapshot* snapshot) {
	if(snapshot->count < 2)
		return;
	for(size_t start = snapshot->count / 2; start > 0; start--)
		piston_moving_snapshot_sift_down(
			snapshot, start - 1, snapshot->count - 1);
	for(size_t end = snapshot->count - 1; end > 0; end--) {
		piston_moving_snapshot_swap(snapshot, 0, end);
		piston_moving_snapshot_sift_down(snapshot, 0, end - 1);
	}
}

bool piston_moving_server_snapshot(
	struct server_local* server, w_coord_t chunk_index_x,
	w_coord_t chunk_index_z, struct piston_moving_chunk_snapshot* snapshot) {
	/* A1b: the near stream's chunk is slot 0's; only that world's moving
	 * entries belong to it. */
	return server && piston_moving_server_snapshot_w(
		server, &server->worlds[0], chunk_index_x, chunk_index_z, snapshot);
}

bool piston_moving_server_snapshot_w(
	struct server_local* server, struct server_world* w,
	w_coord_t chunk_index_x, w_coord_t chunk_index_z,
	struct piston_moving_chunk_snapshot* snapshot) {
	if(!server || !w || !snapshot)
		return false;
	struct piston_moving_owner owner = server_world_piston_owner(w);
	struct server_chunk* host = dict_server_chunks_get(
		w->chunks, S_CHUNK_ID(chunk_index_x, chunk_index_z));
	size_t count = 0;
	for(size_t i = 0; i < server->piston_moving.capacity; i++) {
		const struct piston_moving_store_entry* entry
			= &server->piston_moving.entries[i];
		if(!entry->used || !piston_moving_owner_matches(entry, owner)
		   || entry->y < 0 || entry->y >= WORLD_HEIGHT
		   || piston_chunk_of(entry->x) != chunk_index_x
		   || piston_chunk_of(entry->z) != chunk_index_z)
			continue;
		if(!host || !host->ids)
			continue;
		size_t host_index = (size_t)entry->y
			+ ((size_t)W2C_COORD(entry->z)
			   + (size_t)W2C_COORD(entry->x) * CHUNK_SIZE) * WORLD_HEIGHT;
		if(host->ids[host_index] == BLOCK_PISTON_MOVING)
			count++;
	}
	if(!piston_moving_snapshot_reserve(snapshot, count))
		return false;

	size_t at = 0;
	for(size_t i = 0; i < server->piston_moving.capacity; i++) {
		const struct piston_moving_store_entry* entry
			= &server->piston_moving.entries[i];
		if(!entry->used || !piston_moving_owner_matches(entry, owner)
		   || entry->y < 0 || entry->y >= WORLD_HEIGHT
		   || piston_chunk_of(entry->x) != chunk_index_x
		   || piston_chunk_of(entry->z) != chunk_index_z || !host || !host->ids)
			continue;
		size_t host_index = (size_t)entry->y
			+ ((size_t)W2C_COORD(entry->z)
			   + (size_t)W2C_COORD(entry->x) * CHUNK_SIZE) * WORLD_HEIGHT;
		if(host->ids[host_index] != BLOCK_PISTON_MOVING)
			continue;
		if(!piston_moving_record_encode(&snapshot->records[at],
			   chunk_index_x * 16, chunk_index_z * 16,
			   entry->x, entry->y, entry->z, &entry->state)) {
			snapshot->count = 0;
			return false;
		}
		snapshot->orders[at] = entry->order;
		at++;
	}
	assert(at == count);
	snapshot->count = at;
	piston_moving_snapshot_sort(snapshot);
	return true;
}

/* Split-dims B2: one client moving-piston STORE per world payload. Every
 * client_piston_moving_* entry point operates on the SELECTED store; the
 * primary is static and selected by default (the pre-B2 single store). */
static struct piston_moving_store s_client_store_primary;
static struct piston_moving_store* s_client_store = &s_client_store_primary;

void client_piston_moving_clear(void) {
	piston_moving_store_clear(s_client_store);
}

void client_piston_moving_tick(void) {
	for(size_t i = 0; i < s_client_store->capacity; i++) {
		struct piston_moving_store_entry* entry = &s_client_store->entries[i];
		if(!entry->used)
			continue;
		struct piston_moving_tick_result result;
		piston_moving_advance(&entry->state, &result);
		(void)result;
		/* `result.settle` deliberately does not release ownership here. The
		 * authoritative CRPC_SET_BLOCK both materializes the stored block and
		 * removes this entry atomically in clin_process. */
	}
}

static bool client_piston_moving_find(
	w_coord_t x, w_coord_t y, w_coord_t z, size_t* out_index) {
	return piston_moving_store_find(s_client_store, x, y, z, out_index);
}

bool client_piston_moving_upsert(w_coord_t x, w_coord_t y, w_coord_t z,
								const struct piston_moving_state* state) {
	if(y < 0 || y >= WORLD_HEIGHT || !piston_moving_state_valid(state))
		return false;
	return piston_moving_store_upsert(
		s_client_store, x, y, z, state, NULL);
}

bool client_piston_moving_remove(w_coord_t x, w_coord_t y, w_coord_t z) {
	size_t slot;
	if(!client_piston_moving_find(x, y, z, &slot))
		return false;
	return piston_moving_store_remove_index(s_client_store, slot);
}

void client_piston_moving_remove_chunk(w_coord_t chunk_index_x,
								   w_coord_t chunk_index_z) {
	for(size_t i = 0; i < s_client_store->capacity; i++) {
		struct piston_moving_store_entry* entry = &s_client_store->entries[i];
		if(entry->used && piston_chunk_of(entry->x) == chunk_index_x
		   && piston_chunk_of(entry->z) == chunk_index_z)
			(void)piston_moving_store_remove_index(s_client_store, i);
	}
}

struct piston_moving_decoded_record {
	w_coord_t x, y, z;
	struct piston_moving_state state;
	size_t source_index;
};

static int piston_moving_decoded_coord_cmp(const void* a, const void* b) {
	const struct piston_moving_decoded_record* aa = a;
	const struct piston_moving_decoded_record* bb = b;
	if(aa->x != bb->x)
		return aa->x < bb->x ? -1 : 1;
	if(aa->y != bb->y)
		return aa->y < bb->y ? -1 : 1;
	if(aa->z != bb->z)
		return aa->z < bb->z ? -1 : 1;
	return 0;
}

static int piston_moving_decoded_source_cmp(const void* a, const void* b) {
	const struct piston_moving_decoded_record* aa = a;
	const struct piston_moving_decoded_record* bb = b;
	if(aa->source_index == bb->source_index)
		return 0;
	return aa->source_index < bb->source_index ? -1 : 1;
}

bool client_piston_moving_replace_chunk(
	w_coord_t chunk_index_x, w_coord_t chunk_index_z,
	const struct piston_moving_chunk_record* records, size_t count) {
	if((count > 0 && !records)
	   || count > SIZE_MAX / sizeof(struct piston_moving_decoded_record))
		return false;
	struct piston_moving_decoded_record* decoded = count
		? malloc(count * sizeof(*decoded))
		: NULL;
	if(count > 0 && !decoded)
		return false;

	w_coord_t origin_x = chunk_index_x * 16;
	w_coord_t origin_z = chunk_index_z * 16;
	for(size_t i = 0; i < count; i++) {
		if(!piston_moving_record_decode(
			   &records[i], origin_x, origin_z, &decoded[i].x,
			   &decoded[i].y, &decoded[i].z, &decoded[i].state)) {
			free(decoded);
			return false;
		}
		decoded[i].source_index = i;
	}
	if(count > 1) {
		qsort(decoded, count, sizeof(*decoded), piston_moving_decoded_coord_cmp);
		for(size_t i = 1; i < count; i++)
			if(decoded[i - 1].x == decoded[i].x
			   && decoded[i - 1].y == decoded[i].y
			   && decoded[i - 1].z == decoded[i].z) {
				free(decoded);
				return false;
			}
		qsort(decoded, count, sizeof(*decoded), piston_moving_decoded_source_cmp);
	}

	/* Reserve the complete replacement before touching the old chunk. Account
	 * for slots which the old sidecar set will release, but separately prove the
	 * monotonic insertion-order counter has room for every incoming record. */
	size_t old_chunk_count = 0;
	for(size_t i = 0; i < s_client_store->capacity; i++) {
		const struct piston_moving_store_entry* entry = &s_client_store->entries[i];
		if(entry->used && piston_chunk_of(entry->x) == chunk_index_x
		   && piston_chunk_of(entry->z) == chunk_index_z)
			old_chunk_count++;
	}
	if(count > UINT64_MAX - s_client_store->next_order
	   || !piston_moving_store_reserve(s_client_store,
		   count > old_chunk_count ? count - old_chunk_count : 0)) {
		free(decoded);
		return false;
	}

	client_piston_moving_remove_chunk(chunk_index_x, chunk_index_z);
	for(size_t i = 0; i < count; i++) {
		bool inserted = piston_moving_store_upsert_reserved(
			s_client_store, decoded[i].x, decoded[i].y, decoded[i].z,
			&decoded[i].state, NULL);
		/* Validation rejected duplicates, the old chunk is gone, capacity and
		 * order range were reserved above: failure here is an internal invariant. */
		assert(inserted);
		if(!inserted) {
			free(decoded);
			return false;
		}
	}
	free(decoded);
	return true;
}

bool client_piston_moving_query(w_coord_t x, w_coord_t y, w_coord_t z,
							   struct piston_moving_state* out) {
	size_t slot;
	if(!client_piston_moving_find(x, y, z, &slot))
		return false;
	if(out)
		*out = s_client_store->entries[slot].state;
	return true;
}

size_t client_piston_moving_count(void) {
	return s_client_store->count;
}

void client_piston_moving_visit(client_piston_moving_visit_fn visit, void* user) {
	if(!visit)
		return;
	for(size_t i = 0; i < s_client_store->capacity; i++) {
		const struct piston_moving_store_entry* stored
			= &s_client_store->entries[i];
		if(!stored->used)
			continue;
		const struct piston_moving_client_entry entry = {
			.x = stored->x,
			.y = stored->y,
			.z = stored->z,
			.state = stored->state,
		};
		visit(&entry, user);
	}
}

/* ---- Split-dims B2: client moving-piston stores -------------------------- */

struct piston_moving_store* client_piston_moving_store_primary(void) {
	return &s_client_store_primary;
}

struct piston_moving_store* client_piston_moving_store_create(void) {
	return calloc(1, sizeof(struct piston_moving_store));
}

void client_piston_moving_store_destroy(struct piston_moving_store* store) {
	if(!store || store == &s_client_store_primary || store == s_client_store)
		return;
	piston_moving_store_destroy(store);
	free(store);
}

struct piston_moving_store* client_piston_moving_store_select(
	struct piston_moving_store* store) {
	struct piston_moving_store* previous = s_client_store;
	s_client_store = store ? store : &s_client_store_primary;
	return previous;
}

void client_piston_moving_store_clear(struct piston_moving_store* store) {
	if(store)
		piston_moving_store_clear(store);
}

size_t client_piston_moving_store_count(const struct piston_moving_store* store) {
	return store ? store->count : 0;
}
