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

#include "tile_entities_store.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../block/piston_moving.h"
#include "../world.h"

#define TE_STORE_CHUNK_SIZE 16

// Block -> chunk coordinate, floor division (correct for negatives). Mirrors
// chunk.h's WCOORD_CHUNK_OFFSET; redefined here so this pure-logic unit needn't
// pull chunk.h, which drags in the platform display headers.
static int te_chunk_of(int b) {
	return b < 0 ? (b + 1) / TE_STORE_CHUNK_SIZE - 1 : b / TE_STORE_CHUNK_SIZE;
}

static int min_int(int a, int b) {
	return a < b ? a : b;
}

static bool te_piston_find(const struct te_store* st, int32_t x, int32_t y,
						   int32_t z, size_t* index) {
	return st->owner_keyed
		? piston_moving_store_find_owned(st->piston_moving,
			  te_store_piston_owner(st), x, y, z, index)
		: piston_moving_store_find(st->piston_moving, x, y, z, index);
}

static bool te_piston_entry_owned(const struct te_store* st,
	const struct piston_moving_store_entry* entry) {
	return !st->owner_keyed
		|| piston_moving_owner_matches(entry, te_store_piston_owner(st));
}


bool te_store_filter_piston_hosts(struct chunk_tile_entities* te, int cx,
								 int cz, const uint8_t* ids,
								 size_t* removed) {
	if(!te || !ids || !removed || te->n_piston_moving < 0
	   || (te->n_piston_moving > 0 && !te->piston_movings))
		return false;

	int write = 0;
	size_t stale = 0;
	for(int read = 0; read < te->n_piston_moving; read++) {
		const struct te_piston_moving* piston = &te->piston_movings[read];
		bool in_chunk = piston->y >= 0 && piston->y < WORLD_HEIGHT
			&& te_chunk_of(piston->x) == cx && te_chunk_of(piston->z) == cz;
		bool valid_host = false;
		if(in_chunk) {
			/* Conversion to unsigned before masking is defined for negative world
			 * coordinates and matches W2C_COORD without pulling chunk.h into this
			 * dependency-light store module. */
			size_t local_x = (uint32_t)piston->x & (TE_STORE_CHUNK_SIZE - 1u);
			size_t local_z = (uint32_t)piston->z & (TE_STORE_CHUNK_SIZE - 1u);
			size_t index = (size_t)piston->y
				+ (local_z + local_x * TE_STORE_CHUNK_SIZE) * WORLD_HEIGHT;
			valid_host = ids[index] == BLOCK_PISTON_MOVING;
		}
		if(valid_host) {
			if(write != read)
				te->piston_movings[write] = te->piston_movings[read];
			write++;
		} else {
			stale++;
		}
	}
	te->n_piston_moving = write;
	*removed = stale;
	return true;
}

// newlib (devkitPPC) does not declare strnlen, so provide a local one.
static size_t te_strnlen(const char* s, size_t max) {
	size_t n = 0;
	while(n < max && s[n])
		n++;
	return n;
}

#ifdef CAVEWII_TILE_ENTITIES_STORE_TEST
static bool s_te_alloc_failure_armed;
static unsigned s_te_allocations_before_failure;

void te_store_test_fail_allocation_after(unsigned successful_allocations) {
	s_te_allocations_before_failure = successful_allocations;
	s_te_alloc_failure_armed = true;
}
#endif

static void* te_store_calloc(size_t count, size_t size) {
#ifdef CAVEWII_TILE_ENTITIES_STORE_TEST
	if(s_te_alloc_failure_armed) {
		if(s_te_allocations_before_failure == 0) {
			s_te_alloc_failure_armed = false;
			return NULL;
		}
		s_te_allocations_before_failure--;
	}
#endif
	return calloc(count, size);
}

/* ===== IMP-TE-CAPS growable slabs ========================================= */

static const char* const s_te_kind_name[TE_KIND_FIXED_COUNT] = {
	"chest", "sign", "furnace", "dispenser", "note", "record_player", "spawner",
};

static void** te_slab_owner_ptr(const struct te_grow_ctx* g,
								const struct te_slab_field* f) {
	return (void**)((unsigned char*)g->owner + f->owner_offset);
}

static int* te_slab_cap_ptr(const struct te_grow_ctx* g,
							const struct te_slab_kind* k) {
	return (int*)((unsigned char*)g->owner + k->cap_offset);
}

static size_t te_slab_row_bytes(const struct te_slab_kind* k) {
	size_t n = 0;
	for(int f = 0; f < TE_SLAB_FIELDS_MAX; f++)
		n += k->fields[f].elem;
	return n;
}

/* Copy one kind's owner pointers + capacity into the view. */
static void te_store_point_kind(struct te_store* st, enum te_kind kind) {
	const struct te_grow_ctx* g = st->grow;
	const struct te_slab_kind* k = &g->kinds[kind];
	for(int f = 0; f < TE_SLAB_FIELDS_MAX; f++) {
		const struct te_slab_field* fd = &k->fields[f];
		if(fd->elem == 0 || fd->view_offset == TE_SLAB_NO_VIEW)
			continue;
		*(void**)((unsigned char*)st + fd->view_offset)
			= *te_slab_owner_ptr(g, fd);
	}
	*(int*)((unsigned char*)st + k->view_max_offset) = *te_slab_cap_ptr(g, k);
}

static void te_store_sync(struct te_store* st) {
	if(!st->grow || st->grow_epoch == st->grow->epoch)
		return;
	for(int kind = 0; kind < TE_KIND_FIXED_COUNT; kind++)
		te_store_point_kind(st, (enum te_kind)kind);
	st->grow_epoch = st->grow->epoch;
}

/* Allocate `rows` rows for every field of kind k, copying `old_rows` rows
 * from the current slabs and filling the rest. All-or-nothing. */
static bool te_slab_resize(struct te_grow_ctx* g, const struct te_slab_kind* k,
						   int old_rows, int rows) {
	void* fresh[TE_SLAB_FIELDS_MAX] = {0};
	for(int f = 0; f < TE_SLAB_FIELDS_MAX; f++) {
		const struct te_slab_field* fd = &k->fields[f];
		if(fd->elem == 0)
			continue;
		size_t bytes = fd->elem * (size_t)rows;
		fresh[f] = g->alloc(bytes ? bytes : 1);
		if(!fresh[f]) {
			for(int u = 0; u < f; u++)
				if(fresh[u])
					g->release(fresh[u]);
			return false;
		}
		void* old = *te_slab_owner_ptr(g, fd);
		size_t keep = fd->elem * (size_t)old_rows;
		if(old && keep)
			memcpy(fresh[f], old, keep);
		memset((unsigned char*)fresh[f] + keep, fd->fill, bytes - keep);
	}
	for(int f = 0; f < TE_SLAB_FIELDS_MAX; f++) {
		const struct te_slab_field* fd = &k->fields[f];
		if(fd->elem == 0)
			continue;
		void** slot = te_slab_owner_ptr(g, fd);
		if(*slot)
			g->release(*slot);
		*slot = fresh[f];
	}
	*te_slab_cap_ptr(g, k) = rows;
	size_t row_bytes = te_slab_row_bytes(k);
	g->bytes_live = g->bytes_live - row_bytes * (size_t)old_rows
		+ row_bytes * (size_t)rows;
	if(g->bytes_live > g->bytes_peak)
		g->bytes_peak = g->bytes_live;
	return true;
}

bool te_store_grow(const struct te_store* cst, enum te_kind kind,
				   int min_rows) {
	/* Views are caller-owned locals; growth re-points the one it came in
	 * through (the const in every helper's signature predates growth). */
	struct te_store* st = (struct te_store*)cst;
	if(!st || !st->grow || !st->grow->kinds || kind < 0
	   || kind >= TE_KIND_FIXED_COUNT)
		return false;
	struct te_grow_ctx* g = st->grow;
	te_store_sync(st);
	const struct te_slab_kind* k = &g->kinds[kind];
	int cap = *te_slab_cap_ptr(g, k);
	if(min_rows <= cap)
		return true;
	if(min_rows > TE_STORE_ROWS_LIMIT) {
		g->grow_failures++;
		fprintf(stderr,
				"[te] %s table cannot grow to %d rows (limit %d): chunk "
				"load deferred\n",
				s_te_kind_name[kind], min_rows, TE_STORE_ROWS_LIMIT);
		return false;
	}
	int64_t want = (int64_t)cap + cap / 2;
	if(want < min_rows)
		want = min_rows;
	if(want < 16)
		want = 16;
	if(want > TE_STORE_ROWS_LIMIT)
		want = TE_STORE_ROWS_LIMIT;
	if(!te_slab_resize(g, k, cap, (int)want)) {
		g->grow_failures++;
		/* Bounded failure: nothing changed; the caller refuses this one
		 * claim / chunk load (the chunk stays on disk untouched and is
		 * retried at the normal load cadence once memory frees). */
		if(g->grow_failures <= 8 || (g->grow_failures & 255) == 0)
			fprintf(stderr,
					"[te] OUT OF MEMORY growing %s table %d -> %d rows "
					"(+%zu B, live %zu B, failure #%u): claim/chunk load "
					"refused, disk data untouched\n",
					s_te_kind_name[kind], cap, (int)want,
					te_slab_row_bytes(k) * (size_t)(want - cap),
					g->bytes_live, (unsigned)g->grow_failures);
		return false;
	}
	g->epoch++;
	g->grow_events++;
	st->grow_epoch = g->epoch;
	te_store_point_kind(st, kind);
	fprintf(stderr, "[te] %s table %d -> %d rows (live %zu B, peak %zu B)\n",
			s_te_kind_name[kind], cap, (int)want, g->bytes_live,
			g->bytes_peak);
	return true;
}

static bool te_store_grow_kind(const struct te_store* st, int kind,
							   int min_rows) {
	return te_store_grow(st, (enum te_kind)kind, min_rows);
}

bool te_store_reserve_free(const struct te_store* st, enum te_kind kind,
						   int free_rows) {
	if(free_rows <= 0)
		return true;
	te_store_sync((struct te_store*)st);
	int cap;
	struct te_pos* p = te_kind_pos(st, kind, &cap);
	int have = 0;
	for(int i = 0; p && i < cap && have < free_rows; i++)
		if(p[i].y < 0 && !te_reserved(st, kind, i))
			have++;
	if(have >= free_rows)
		return true;
	if((int64_t)cap + (free_rows - have) > TE_STORE_ROWS_LIMIT)
		return te_store_grow(st, kind, TE_STORE_ROWS_LIMIT + 1);
	return te_store_grow(st, kind, cap + (free_rows - have));
}

bool te_grow_ctx_init(struct te_grow_ctx* g, void* owner,
					  const struct te_slab_kind* kinds,
					  void* (*alloc)(size_t), void (*release)(void*)) {
	if(!g || !owner || !kinds || !alloc || !release)
		return false;
	*g = (struct te_grow_ctx) {
		.owner = owner, .kinds = kinds, .alloc = alloc, .release = release,
		.epoch = 1, .grow_fn = te_store_grow_kind,
	};
	for(int kind = 0; kind < TE_KIND_FIXED_COUNT; kind++) {
		const struct te_slab_kind* k = &kinds[kind];
		for(int f = 0; f < TE_SLAB_FIELDS_MAX; f++)
			if(k->fields[f].elem)
				*te_slab_owner_ptr(g, &k->fields[f]) = NULL;
		*te_slab_cap_ptr(g, k) = 0;
	}
	for(int kind = 0; kind < TE_KIND_FIXED_COUNT; kind++)
		if(!te_slab_resize(g, &kinds[kind], 0, kinds[kind].initial_rows)) {
			fprintf(stderr,
					"[te] OUT OF MEMORY allocating the initial %s table "
					"(%d rows)\n",
					s_te_kind_name[kind], kinds[kind].initial_rows);
			te_grow_ctx_free(g);
			return false;
		}
	return true;
}

void te_grow_ctx_free(struct te_grow_ctx* g) {
	if(!g || !g->kinds)
		return;
	for(int kind = 0; kind < TE_KIND_FIXED_COUNT; kind++) {
		const struct te_slab_kind* k = &g->kinds[kind];
		for(int f = 0; f < TE_SLAB_FIELDS_MAX; f++) {
			if(!k->fields[f].elem)
				continue;
			void** slot = te_slab_owner_ptr(g, &k->fields[f]);
			if(*slot)
				g->release(*slot);
			*slot = NULL;
		}
		*te_slab_cap_ptr(g, k) = 0;
	}
	g->bytes_live = 0;
	g->epoch++;
	g->kinds = NULL;
}

/* IMP-TE-CAPS: a chunk-load insertion normally lands in a chunk where this
 * view owns no row of the kind yet. One O(rows) pass proves that, after which
 * the per-record "already present?" test only has to look at the incoming
 * list itself instead of the whole (now unbounded) resident table. */
static bool te_incoming_single_chunk(const void* incoming, int count,
									 size_t stride, int* cx, int* cz) {
	for(int i = 0; i < count; i++) {
		const int32_t* pos
			= (const int32_t*)((const unsigned char*)incoming + (size_t)i * stride);
		int x = te_chunk_of(pos[0]), z = te_chunk_of(pos[2]);
		if(i == 0) {
			*cx = x;
			*cz = z;
		} else if(x != *cx || z != *cz)
			return false;
	}
	return count > 0;
}

static bool te_owns_rows_in_chunk(const struct te_store* st, enum te_kind kind,
								  const struct te_pos* p, int cap, int cx,
								  int cz) {
	for(int i = 0; i < cap; i++)
		if(te_live(st, kind, p, i) && te_chunk_of(p[i].x) == cx
		   && te_chunk_of(p[i].z) == cz)
			return true;
	return false;
}

/* Must insertion of `incoming` search the resident rows for a same-position
 * row? Only when some owned row could match (see above). */
static bool te_must_scan_resident(const struct te_store* st, enum te_kind kind,
								  const struct te_pos* p, int cap,
								  const void* incoming, int count,
								  size_t stride) {
	int cx, cz;
	if(!te_incoming_single_chunk(incoming, count, stride, &cx, &cz))
		return true;
	return te_owns_rows_in_chunk(st, kind, p, cap, cx, cz);
}

static bool te_incoming_dup_before(const void* incoming, int i, size_t stride) {
	const int32_t* pos
		= (const int32_t*)((const unsigned char*)incoming + (size_t)i * stride);
	for(int j = 0; j < i; j++) {
		const int32_t* earlier = (const int32_t*)(
			(const unsigned char*)incoming + (size_t)j * stride);
		if(earlier[0] == pos[0] && earlier[1] == pos[1]
		   && earlier[2] == pos[2])
			return true;
	}
	return false;
}

static bool te_store_can_insert_positions(const struct te_store* st,
										  enum te_kind kind,
										  const void* incoming, int count,
										  size_t stride) {
	if(count < 0)
		return false;
	if(count == 0)
		return true;
	if(!incoming)
		return false;
	int capacity;
	const struct te_pos* current = te_kind_pos(st, kind, &capacity);
	if(capacity < 0 || (!current && !st->grow))
		return false;
	bool scan = current
		&& te_must_scan_resident(st, kind, current, capacity, incoming, count,
								 stride);
	int needed = 0;
	for(int i = 0; i < count; i++) {
		const int32_t* pos
			= (const int32_t*)((const unsigned char*)incoming + (size_t)i * stride);
		bool present = false;
		for(int j = 0; scan && j < capacity; j++)
			if(te_live(st, kind, current, j) && current[j].x == pos[0]
			   && current[j].y == pos[1] && current[j].z == pos[2]) {
				present = true;
				break;
			}
		if(!present && !te_incoming_dup_before(incoming, i, stride))
			needed++;
	}
	/* Grow (capacity reservation only, like the piston preflight below): the
	 * parsed set must fit completely or the chunk is refused unchanged. */
	return te_store_reserve_free(st, kind, needed);
}

static bool te_store_can_replace_legacy_positions(const struct te_store* st,
	enum te_kind kind, const void* incoming, int count, size_t stride, int cx,
	int cz) {
	if(count < 0)
		return false;
	if(count == 0)
		return true;
	int capacity;
	const struct te_pos* current = te_kind_pos(st, kind, &capacity);
	if(capacity < 0 || !current || !incoming)
		return false;

	/* Every existing row THIS WORLD owns in this chunk will be released at
	 * commit. Ordinary free slots are the only other available capacity; a
	 * same-coordinate row another world owns is never reclaimable. */
	int available = 0;
	for(int i = 0; i < capacity; i++)
		if((current[i].y < 0 && !te_reserved(st, kind, i))
		   || (te_owned(st, kind, i) && te_chunk_of(current[i].x) == cx
			   && te_chunk_of(current[i].z) == cz))
			available++;

	int needed = 0;
	for(int i = 0; i < count; i++) {
		const int32_t* pos
			= (const int32_t*)((const unsigned char*)incoming
				+ (size_t)i * stride);
		if(te_chunk_of(pos[0]) != cx || te_chunk_of(pos[2]) != cz)
			return false;
		bool present = false;
		/* A same-position row outside (cx,cz) is impossible for valid world
		 * coordinates, but keep the generic capacity proof complete. */
		for(int j = 0; j < capacity; j++)
			if(te_live(st, kind, current, j)
			   && (te_chunk_of(current[j].x) != cx
				   || te_chunk_of(current[j].z) != cz)
			   && current[j].x == pos[0] && current[j].y == pos[1]
			   && current[j].z == pos[2]) {
				present = true;
				break;
			}
		for(int j = 0; !present && j < i; j++) {
			const int32_t* earlier = (const int32_t*)(
				(const unsigned char*)incoming + (size_t)j * stride);
			if(earlier[0] == pos[0] && earlier[1] == pos[1]
			   && earlier[2] == pos[2])
				present = true;
		}
		if(!present)
			needed++;
	}
	/* IMP-TE-CAPS: a shortfall grows the table (new rows are free). */
	if(needed <= available)
		return true;
	return te_store_grow(st, kind, capacity + (needed - available));
}

static bool te_store_shape_valid(const struct te_store* st,
								 const struct chunk_tile_entities* te) {
	return st && te && !(te->n_chest > 0 && !st->chest_items)
		&& !(te->n_sign > 0 && !st->sign_texts)
		&& !(te->n_furnace > 0 && !st->furnaces)
		&& !(te->n_dispenser > 0 && !st->dispensers)
		&& !(te->n_note > 0 && !st->notes)
		&& !(te->n_record_player > 0 && !st->records)
		&& !(te->n_spawner > 0 && !st->spawners)
		&& te->n_piston_moving >= 0
		&& !(te->n_piston_moving > 0
			&& (!te->piston_movings || !st->piston_moving));
}

static bool te_store_can_insert_pistons(const struct te_store* st,
									 const struct chunk_tile_entities* te) {
	if(te->n_piston_moving == 0)
		return true;
	struct piston_moving_store* store = st->piston_moving;
	if((uint64_t)te->n_piston_moving
	   > UINT64_MAX - store->next_order)
		return false;
	size_t needed = 0;
	for(int i = 0; i < te->n_piston_moving; i++) {
		const struct te_piston_moving* incoming = &te->piston_movings[i];
		if(te_piston_find(st, incoming->x, incoming->y, incoming->z, NULL))
			continue;
		bool duplicate = false;
		for(int earlier = 0; earlier < i; earlier++) {
			const struct te_piston_moving* prior
				= &te->piston_movings[earlier];
			if(prior->x == incoming->x && prior->y == incoming->y
			   && prior->z == incoming->z) {
				duplicate = true;
				break;
			}
		}
		if(!duplicate) {
			if(needed == SIZE_MAX)
				return false;
			needed++;
		}
	}
	/* Capacity reservation is the only mutation in this preflight. No logical
	 * entry is published, so allocation failure leaves every TE table intact. */
	return piston_moving_store_reserve(store, needed);
}

bool te_store_can_insert(const struct te_store* st,
						 const struct chunk_tile_entities* te) {
	if(!te_store_shape_valid(st, te))
		return false;
	if(!(te_store_can_insert_positions(st, TE_KIND_CHEST,
									 te->chests, te->n_chest,
									 sizeof(struct te_chest))
		&& te_store_can_insert_positions(st, TE_KIND_SIGN,
									 te->signs, te->n_sign,
									 sizeof(struct te_sign))
		&& te_store_can_insert_positions(st, TE_KIND_FURNACE,
									 te->furnaces, te->n_furnace,
									 sizeof(struct te_furnace))
		&& te_store_can_insert_positions(st, TE_KIND_DISPENSER, te->dispensers,
									 te->n_dispenser,
									 sizeof(struct te_dispenser))
		&& te_store_can_insert_positions(st, TE_KIND_NOTE,
									 te->notes, te->n_note,
									 sizeof(struct te_note))
		&& te_store_can_insert_positions(st, TE_KIND_RECORD_PLAYER,
									 te->record_players,
									 te->n_record_player,
									 sizeof(struct te_record_player))
		&& te_store_can_insert_positions(st, TE_KIND_SPAWNER,
									 te->spawners, te->n_spawner,
									 sizeof(struct te_spawner))))
		return false;
	return te_store_can_insert_pistons(st, te);
}

bool te_store_can_replace_legacy_chunk(
	const struct te_store* st, const struct chunk_tile_entities* te,
	int cx, int cz) {
	if(!te_store_shape_valid(st, te))
		return false;
	return te_store_can_replace_legacy_positions(
			   st, TE_KIND_CHEST, te->chests, te->n_chest,
			   sizeof(struct te_chest), cx, cz)
		&& te_store_can_replace_legacy_positions(
			   st, TE_KIND_SIGN, te->signs, te->n_sign,
			   sizeof(struct te_sign), cx, cz)
		&& te_store_can_insert_positions(st, TE_KIND_FURNACE, te->furnaces,
			   te->n_furnace, sizeof(struct te_furnace))
		&& te_store_can_insert_positions(st, TE_KIND_DISPENSER, te->dispensers,
			   te->n_dispenser, sizeof(struct te_dispenser))
		&& te_store_can_insert_positions(st, TE_KIND_NOTE,
									 te->notes, te->n_note,
									 sizeof(struct te_note))
		&& te_store_can_insert_positions(st, TE_KIND_RECORD_PLAYER,
			   te->record_players, te->n_record_player,
			   sizeof(struct te_record_player))
		&& te_store_can_insert_positions(st, TE_KIND_SPAWNER,
									 te->spawners, te->n_spawner,
									 sizeof(struct te_spawner))
		&& te_store_can_insert_pistons(st, te);
}

/* IMP-TE-CAPS insertion cursor for one kind of one te_store_insert call.
 * Same row choice as the historical single scan ("the owned row already at
 * this position, else the lowest free unreserved row"), without rescanning
 * the whole resident table per record: the lowest free row only moves up
 * while one call claims rows, and a same-position row can only exist when
 * this view owns a row in the incoming chunk or the incoming list repeats a
 * position (see te_must_scan_resident). */
struct te_insert_cursor {
	enum te_kind kind;
	struct te_pos* p;
	int max;
	bool scan;		  /* resident rows may match: full scan per record */
	int next_free;	  /* no free unreserved row below this index */
	int first_claim;  /* lowest row claimed by this call (or max) */
};

static void te_insert_cursor_init(struct te_insert_cursor* c,
								  const struct te_store* st, enum te_kind kind,
								  const void* incoming, int count,
								  size_t stride) {
	c->kind = kind;
	c->p = te_kind_pos(st, kind, &c->max);
	c->scan = c->p && te_must_scan_resident(st, kind, c->p, c->max, incoming,
											count, stride);
	c->next_free = 0;
	c->first_claim = c->max;
}

static int te_insert_cursor_slot(struct te_insert_cursor* c,
								 const struct te_store* st,
								 const void* incoming, int k, size_t stride) {
	const int32_t* pos
		= (const int32_t*)((const unsigned char*)incoming + (size_t)k * stride);
	struct te_pos* p = c->p;
	int slot = -1;
	if(c->scan) {
		for(int i = 0; i < c->max; i++)
			if(te_live(st, c->kind, p, i) && p[i].x == pos[0]
			   && p[i].y == pos[1] && p[i].z == pos[2]) {
				slot = i;
				break;
			}
	} else if(te_incoming_dup_before(incoming, k, stride)) {
		/* the earlier copy's row was claimed by this call */
		for(int i = c->first_claim; i < c->max; i++)
			if(te_live(st, c->kind, p, i) && p[i].x == pos[0]
			   && p[i].y == pos[1] && p[i].z == pos[2]) {
				slot = i;
				break;
			}
	}
	if(slot < 0) {
		while(c->next_free < c->max
			  && (p[c->next_free].y >= 0
				  || te_reserved(st, c->kind, c->next_free)))
			c->next_free++;
		slot = c->next_free;
		assert(slot < c->max); // guaranteed by te_store_can_insert
		if(slot < c->first_claim)
			c->first_claim = slot;
	}
	p[slot] = (struct te_pos) {pos[0], pos[1], pos[2]};
	te_stamp(st, c->kind, slot);
	return slot;
}

bool te_store_insert(const struct te_store* st,
					 const struct chunk_tile_entities* te) {
	if(!te_store_can_insert(st, te))
		return false;
	struct te_insert_cursor cur;

	int slots = min_int(st->chest_slots, TE_CHEST_SLOTS);
	te_insert_cursor_init(&cur, st, TE_KIND_CHEST, te->chests, te->n_chest,
						  sizeof(struct te_chest));
	for(int k = 0; k < te->n_chest; k++) {
		const struct te_chest* src = &te->chests[k];
		int slot = te_insert_cursor_slot(&cur, st, te->chests, k,
									 sizeof(struct te_chest));
		memcpy(&st->chest_items[(size_t)slot * st->chest_slots], src->items,
			   (size_t)slots * sizeof(struct item_data));
	}

	int ssz = min_int(st->sign_size, TE_SIGN_SIZE);
	te_insert_cursor_init(&cur, st, TE_KIND_SIGN, te->signs, te->n_sign,
						  sizeof(struct te_sign));
	for(int k = 0; k < te->n_sign; k++) {
		const struct te_sign* src = &te->signs[k];
		int slot = te_insert_cursor_slot(&cur, st, te->signs, k,
									 sizeof(struct te_sign));
		// The runtime buffer is space-padded (the server's convention); copy the
		// text and leave the remainder as spaces, not nuls.
		char* dst = &st->sign_texts[(size_t)slot * st->sign_size];
		memset(dst, ' ', st->sign_size);
		size_t n = te_strnlen(src->text, (size_t)ssz);
		memcpy(dst, src->text, n);
	}

	te_insert_cursor_init(&cur, st, TE_KIND_FURNACE, te->furnaces, te->n_furnace,
						  sizeof(struct te_furnace));
	for(int k = 0; k < te->n_furnace; k++) {
		const struct te_furnace* src = &te->furnaces[k];
		int slot = te_insert_cursor_slot(&cur, st, te->furnaces, k,
									 sizeof(struct te_furnace));
		struct furnace_te* f = &st->furnaces[slot];
		*f = (struct furnace_te) {0};
		f->burn_time = src->burn_time;
		f->cook_time = src->cook_time;
		// current_item_burn_time stays 0; the server load fixup recomputes it from
		// the fuel slot (Beta readFromNBT). The 3 furnace_te slots are exactly
		// te_furnace's input/fuel/output, so a straight copy is faithful.
		int n = min_int(TE_FURNACE_SLOTS, 3);
		for(int j = 0; j < n; j++)
			f->items[j] = src->items[j];
	}

	int dslots = min_int(TE_DISPENSER_SLOTS, DISPENSER_TE_SLOTS);
	te_insert_cursor_init(&cur, st, TE_KIND_DISPENSER, te->dispensers, te->n_dispenser,
						  sizeof(struct te_dispenser));
	for(int k = 0; k < te->n_dispenser; k++) {
		const struct te_dispenser* src = &te->dispensers[k];
		int slot = te_insert_cursor_slot(&cur, st, te->dispensers, k,
									 sizeof(struct te_dispenser));
		struct dispenser_te* d = &st->dispensers[slot];
		// dispenser_random is deliberately left at its zeroed default here --
		// dispenser_server_load_fixup reseeds every dispenser in the just-
		// inserted chunk right after te_store_insert returns (mirrors the
		// furnace fixup's currentItemBurnTime recompute), keeping this
		// dependency-light module free of util.h's time()-based rand_gen_seed.
		*d = (struct dispenser_te) {0};
		for(int j = 0; j < dslots; j++)
			d->items[j] = src->items[j];
	}

	te_insert_cursor_init(&cur, st, TE_KIND_NOTE, te->notes, te->n_note,
						  sizeof(struct te_note));
	for(int k = 0; k < te->n_note; k++) {
		const struct te_note* src = &te->notes[k];
		int slot = te_insert_cursor_slot(&cur, st, te->notes, k,
									 sizeof(struct te_note));
		st->notes[slot] = src->note;
		// Beta TileEntityNote.previousRedstoneState is never persisted -- a
		// fresh TileEntityNote() construction (which chunk load always is)
		// starts at false. Clearing here, not in a separate load-fixup pass
		// like dispenser_random, because it needs no external entropy source.
		if(st->note_prev_powered)
			st->note_prev_powered[slot] = false;
	}

	te_insert_cursor_init(&cur, st, TE_KIND_RECORD_PLAYER, te->record_players, te->n_record_player,
						  sizeof(struct te_record_player));
	for(int k = 0; k < te->n_record_player; k++) {
		const struct te_record_player* src = &te->record_players[k];
		int slot = te_insert_cursor_slot(&cur, st, te->record_players, k,
									 sizeof(struct te_record_player));
		st->records[slot] = src->record;
	}

	te_insert_cursor_init(&cur, st, TE_KIND_SPAWNER, te->spawners, te->n_spawner,
						  sizeof(struct te_spawner));
	for(int k = 0; k < te->n_spawner; k++) {
		const struct te_spawner* src = &te->spawners[k];
		int slot = te_insert_cursor_slot(&cur, st, te->spawners, k,
									 sizeof(struct te_spawner));
		struct spawner_te* sp = &st->spawners[slot];
		*sp = (struct spawner_te) {0};
		// Defensive: bound by whichever buffer (persisted te_spawner vs. live
		// spawner_te) is smaller, matching this file's own chest_slots/
		// sign_size cross-struct-size-mismatch precedent, even though both
		// are TE_SPAWNER_MOB_ID_MAX/SPAWNER_MOB_ID_MAX == 24 today.
		int cap = min_int((int)sizeof(sp->mob_id), (int)sizeof(src->mob_id));
		size_t n = te_strnlen(src->mob_id, (size_t)(cap > 0 ? cap - 1 : 0));
		memcpy(sp->mob_id, src->mob_id, n);
		sp->mob_id[n] = '\0';
		sp->delay = src->delay;
	}

	for(int k = 0; k < te->n_piston_moving && st->piston_moving; k++) {
		const struct te_piston_moving* src = &te->piston_movings[k];
		// readFromNBT sets BOTH progress fields to the saved lagging progress. The comparison form catches
		// NaN/Inf as well as range errors without needing a platform-specific isfinite implementation.
		float p = src->progress;
		if(!(p >= 0.0F && p <= 1.0F))
			p = 0.0F;
		struct piston_moving_state state = {
			.stored_id = (uint8_t)src->stored_id,
			.stored_meta = (uint8_t)src->stored_meta,
			.facing = (uint8_t)src->facing,
			.flags = (uint8_t)(src->extending
				? PISTON_MOVING_FLAG_EXTENDING : 0),
			.current_progress = p,
			.last_progress = p,
		};
		// Preserve the order this runtime installs parsed records. Beta's chunk activation obtains
		// TileEntities from Java HashMap.values(), whose iteration is not the serialized list order;
		// reproducing that cross-reload ordering requires the tracked HashMap/order redesign.
		bool inserted = st->owner_keyed
			? piston_moving_store_upsert_reserved_owned(st->piston_moving,
				  te_store_piston_owner(st), src->x, src->y, src->z, &state,
				  NULL)
			: piston_moving_store_upsert_reserved(
				  st->piston_moving, src->x, src->y, src->z, &state, NULL);
		assert(inserted); /* guaranteed by the complete preflight above */
		if(!inserted)
			return false;
	}
	return true;
}

bool te_store_gather(const struct te_store* st, int cx, int cz,
					 struct chunk_tile_entities* out) {
	memset(out, 0, sizeof(*out));
	/* The live store already contains the legacy flat image plus every resident
	 * chunk insertion, so this gather is a complete replacement snapshot. */
	out->legacy_flat_complete = true;

	int nc = 0, ns = 0, nf = 0, nd = 0, nn = 0, nrp = 0, nsp = 0;
	size_t np = 0;
	for(int i = 0; i < st->max_chest; i++) {
		struct te_pos* p = &st->chest_pos[i];
		if(te_live(st, TE_KIND_CHEST, st->chest_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			nc++;
	}
	for(int i = 0; i < st->max_sign; i++) {
		struct te_pos* p = &st->sign_pos[i];
		if(te_live(st, TE_KIND_SIGN, st->sign_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			ns++;
	}
	for(int i = 0; i < st->max_furnace; i++) {
		struct te_pos* p = &st->furnace_pos[i];
		if(te_live(st, TE_KIND_FURNACE, st->furnace_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			nf++;
	}
	for(int i = 0; i < st->max_dispenser; i++) {
		struct te_pos* p = &st->dispenser_pos[i];
		if(te_live(st, TE_KIND_DISPENSER, st->dispenser_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			nd++;
	}
	for(int i = 0; i < st->max_note; i++) {
		struct te_pos* p = &st->note_pos[i];
		if(te_live(st, TE_KIND_NOTE, st->note_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			nn++;
	}
	for(int i = 0; i < st->max_record_player; i++) {
		struct te_pos* p = &st->record_player_pos[i];
		if(te_live(st, TE_KIND_RECORD_PLAYER, st->record_player_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			nrp++;
	}
	for(int i = 0; i < st->max_spawner; i++) {
		struct te_pos* p = &st->spawner_pos[i];
		if(te_live(st, TE_KIND_SPAWNER, st->spawner_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			nsp++;
	}
	if(st->piston_moving)
		for(size_t i = 0; i < st->piston_moving->capacity; i++) {
			const struct piston_moving_store_entry* entry
				= &st->piston_moving->entries[i];
			if(entry->used && te_piston_entry_owned(st, entry)
			   && te_chunk_of(entry->x) == cx && te_chunk_of(entry->z) == cz)
				np++;
		}
	if(np > INT_MAX)
		return false;
	if(nc)
		out->chests = te_store_calloc((size_t)nc, sizeof(struct te_chest));
	if(ns)
		out->signs = te_store_calloc((size_t)ns, sizeof(struct te_sign));
	if(nf)
		out->furnaces = te_store_calloc((size_t)nf, sizeof(struct te_furnace));
	if(nd)
		out->dispensers
			= te_store_calloc((size_t)nd, sizeof(struct te_dispenser));
	if(nn)
		out->notes = te_store_calloc((size_t)nn, sizeof(struct te_note));
	if(nrp)
		out->record_players = te_store_calloc(
			(size_t)nrp, sizeof(struct te_record_player));
	if(nsp)
		out->spawners = te_store_calloc((size_t)nsp, sizeof(struct te_spawner));
	if(np)
		out->piston_movings
			= te_store_calloc((size_t)np, sizeof(struct te_piston_moving));
	if((nc && !out->chests) || (ns && !out->signs)
	   || (nf && !out->furnaces) || (nd && !out->dispensers)
	   || (nn && !out->notes) || (nrp && !out->record_players)
	   || (nsp && !out->spawners)
	   || (np && !out->piston_movings)) {
		tile_entities_parse_free(out);
		return false;
	}

	int slots = min_int(st->chest_slots, TE_CHEST_SLOTS);
	for(int i = 0; i < st->max_chest && out->chests; i++) {
		struct te_pos* p = &st->chest_pos[i];
		if(!te_live(st, TE_KIND_CHEST, st->chest_pos, i) || te_chunk_of(p->x) != cx
		   || te_chunk_of(p->z) != cz)
			continue;
		struct te_chest* d = &out->chests[out->n_chest++];
		d->x = p->x;
		d->y = p->y;
		d->z = p->z;
		memcpy(d->items, &st->chest_items[(size_t)i * st->chest_slots],
			   (size_t)slots * sizeof(struct item_data));
	}

	int ssz = min_int(st->sign_size, TE_SIGN_SIZE);
	for(int i = 0; i < st->max_sign && out->signs; i++) {
		struct te_pos* p = &st->sign_pos[i];
		if(!te_live(st, TE_KIND_SIGN, st->sign_pos, i) || te_chunk_of(p->x) != cx
		   || te_chunk_of(p->z) != cz)
			continue;
		struct te_sign* d = &out->signs[out->n_sign++];
		d->x = p->x;
		d->y = p->y;
		d->z = p->z;
		memcpy(d->text, &st->sign_texts[(size_t)i * st->sign_size], (size_t)ssz);
		d->text[ssz] = '\0'; // te_sign.text is [TE_SIGN_SIZE+1]: room for the nul
	}

	int fslots = min_int(TE_FURNACE_SLOTS, 3);
	for(int i = 0; i < st->max_furnace && out->furnaces; i++) {
		struct te_pos* p = &st->furnace_pos[i];
		if(!te_live(st, TE_KIND_FURNACE, st->furnace_pos, i) || te_chunk_of(p->x) != cx
		   || te_chunk_of(p->z) != cz)
			continue;
		struct furnace_te* f = &st->furnaces[i];
		struct te_furnace* d = &out->furnaces[out->n_furnace++];
		d->x = p->x;
		d->y = p->y;
		d->z = p->z;
		d->burn_time = (int16_t)f->burn_time;
		d->cook_time = (int16_t)f->cook_time;
		for(int j = 0; j < fslots; j++)
			d->items[j] = f->items[j];
	}

	int dslots = min_int(TE_DISPENSER_SLOTS, DISPENSER_TE_SLOTS);
	for(int i = 0; i < st->max_dispenser && out->dispensers; i++) {
		struct te_pos* p = &st->dispenser_pos[i];
		if(!te_live(st, TE_KIND_DISPENSER, st->dispenser_pos, i) || te_chunk_of(p->x) != cx
		   || te_chunk_of(p->z) != cz)
			continue;
		struct dispenser_te* dp = &st->dispensers[i];
		struct te_dispenser* d = &out->dispensers[out->n_dispenser++];
		d->x = p->x;
		d->y = p->y;
		d->z = p->z;
		for(int j = 0; j < dslots; j++)
			d->items[j] = dp->items[j];
	}

	for(int i = 0; i < st->max_note && out->notes; i++) {
		struct te_pos* p = &st->note_pos[i];
		if(!te_live(st, TE_KIND_NOTE, st->note_pos, i) || te_chunk_of(p->x) != cx
		   || te_chunk_of(p->z) != cz)
			continue;
		struct te_note* d = &out->notes[out->n_note++];
		d->x = p->x;
		d->y = p->y;
		d->z = p->z;
		d->note = st->notes[i];
	}

	for(int i = 0; i < st->max_record_player && out->record_players; i++) {
		struct te_pos* p = &st->record_player_pos[i];
		if(!te_live(st, TE_KIND_RECORD_PLAYER, st->record_player_pos, i) || te_chunk_of(p->x) != cx
		   || te_chunk_of(p->z) != cz)
			continue;
		struct te_record_player* d
			= &out->record_players[out->n_record_player++];
		d->x = p->x;
		d->y = p->y;
		d->z = p->z;
		d->record = st->records[i];
	}

	for(int i = 0; i < st->max_spawner && out->spawners; i++) {
		struct te_pos* p = &st->spawner_pos[i];
		if(!te_live(st, TE_KIND_SPAWNER, st->spawner_pos, i) || te_chunk_of(p->x) != cx
		   || te_chunk_of(p->z) != cz)
			continue;
		struct spawner_te* sp = &st->spawners[i];
		struct te_spawner* d = &out->spawners[out->n_spawner++];
		d->x = p->x;
		d->y = p->y;
		d->z = p->z;
		int cap = min_int((int)sizeof(d->mob_id), (int)sizeof(sp->mob_id));
		size_t n = te_strnlen(sp->mob_id, (size_t)(cap > 0 ? cap - 1 : 0));
		memcpy(d->mob_id, sp->mob_id, n);
		d->mob_id[n] = '\0';
		d->delay = sp->delay;
	}

	/* Serialize deterministic loaded-list order with an order cursor. This is
	 * intentionally heap/stack-allocation-free and has no ownership cap. */
	uint64_t emitted_order = 0;
	for(size_t emitted = 0; emitted < np && out->piston_movings; emitted++) {
		size_t i = SIZE_MAX;
		uint64_t next_order = 0;
		bool found = false;
		for(size_t candidate = 0;
			candidate < st->piston_moving->capacity; candidate++) {
			const struct piston_moving_store_entry* entry
				= &st->piston_moving->entries[candidate];
			if(!entry->used || !te_piston_entry_owned(st, entry)
			   || te_chunk_of(entry->x) != cx
			   || te_chunk_of(entry->z) != cz
			   || entry->order <= emitted_order
			   || (found && entry->order >= next_order))
				continue;
			i = candidate;
			next_order = entry->order;
			found = true;
		}
		assert(found && i != SIZE_MAX);
		emitted_order = next_order;
		const struct piston_moving_store_entry* entry
			= &st->piston_moving->entries[i];
		struct te_piston_moving* d = &out->piston_movings[out->n_piston_moving++];
		d->x = entry->x;
		d->y = entry->y;
		d->z = entry->z;
		d->stored_id = entry->state.stored_id;
		d->stored_meta = entry->state.stored_meta;
		d->facing = entry->state.facing;
		d->extending
			= (int8_t)piston_moving_is_extending(&entry->state);
		// writeToNBT persists field_31020_l verbatim, including arbitrary valid reloaded fractions.
		d->progress = entry->state.last_progress;
	}
	return true;
}

void te_store_release_legacy_chunk(const struct te_store* st, int cx, int cz) {
	for(int i = 0; i < st->max_chest; i++) {
		struct te_pos* p = &st->chest_pos[i];
		if(te_live(st, TE_KIND_CHEST, st->chest_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			te_release_row(st, TE_KIND_CHEST, st->chest_pos, i);
	}
	for(int i = 0; i < st->max_sign; i++) {
		struct te_pos* p = &st->sign_pos[i];
		if(te_live(st, TE_KIND_SIGN, st->sign_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			te_release_row(st, TE_KIND_SIGN, st->sign_pos, i);
	}
}

void te_store_release(const struct te_store* st, int cx, int cz) {
	te_store_release_legacy_chunk(st, cx, cz);
	for(int i = 0; i < st->max_furnace; i++) {
		struct te_pos* p = &st->furnace_pos[i];
		if(te_live(st, TE_KIND_FURNACE, st->furnace_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			te_release_row(st, TE_KIND_FURNACE, st->furnace_pos, i);
	}
	for(int i = 0; i < st->max_dispenser; i++) {
		struct te_pos* p = &st->dispenser_pos[i];
		if(te_live(st, TE_KIND_DISPENSER, st->dispenser_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			te_release_row(st, TE_KIND_DISPENSER, st->dispenser_pos, i);
	}
	for(int i = 0; i < st->max_note; i++) {
		struct te_pos* p = &st->note_pos[i];
		if(te_live(st, TE_KIND_NOTE, st->note_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			te_release_row(st, TE_KIND_NOTE, st->note_pos, i);
	}
	for(int i = 0; i < st->max_record_player; i++) {
		struct te_pos* p = &st->record_player_pos[i];
		if(te_live(st, TE_KIND_RECORD_PLAYER, st->record_player_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			te_release_row(st, TE_KIND_RECORD_PLAYER, st->record_player_pos, i);
	}
	for(int i = 0; i < st->max_spawner; i++) {
		struct te_pos* p = &st->spawner_pos[i];
		if(te_live(st, TE_KIND_SPAWNER, st->spawner_pos, i) && te_chunk_of(p->x) == cx
		   && te_chunk_of(p->z) == cz)
			te_release_row(st, TE_KIND_SPAWNER, st->spawner_pos, i);
	}
	if(st->piston_moving)
		for(size_t i = 0; i < st->piston_moving->capacity; i++) {
			const struct piston_moving_store_entry* entry
				= &st->piston_moving->entries[i];
			if(entry->used && te_piston_entry_owned(st, entry)
			   && te_chunk_of(entry->x) == cx && te_chunk_of(entry->z) == cz)
				(void)piston_moving_store_remove_index(
					st->piston_moving, i);
		}
}
