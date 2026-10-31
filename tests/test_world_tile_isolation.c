/* A1b (split-dimensions-plan-0924, Astra D9): same-coordinate tile isolation
 * across world slots through the REAL production view,
 * server_world_tile_store(w) (server_local_state.h). One shared backing table
 * per tile kind; per-row owner keys (slot, generation) select the world.
 *
 * RED history: the scaffold (e62a081f) built a position-only view that
 * mirrored the pre-A1b tables; its nine checks failed (world 1 saw world 0's
 * chest/sign/furnace, world-0 save captured world-1 values, world-0 unload
 * erased world-1 tiles). This file now uses the production constructor; every
 * check below must pass. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network/server_local_state.h"
#include "network/tile_entities_store.h"

static struct server_local s;
static int failures;

#define CHECK(label, condition) do { \
	if(!(condition)) { \
		fprintf(stderr, "A1b FAIL %s at line %d\n", label, __LINE__); \
		failures++; \
	} \
} while(0)

static void reset_tables(void) {
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init_heap(&s))
		abort(); /* fresh slabs: every row free, owner keys {0,0} */
	piston_moving_store_destroy(&s.piston_moving);
}

/* Valid identities: slot i of THIS session, live, distinct generations. */
static void bind_world(int slot, uint32_t generation) {
	s.worlds[slot].world_index = (uint8_t)slot;
	s.worlds[slot].session = &s;
	s.worlds[slot].generation = generation;
}

/* One tile of every kind at (3, 70..77, 5), tagged by `tag`. */
struct image {
	struct te_chest chest;
	struct te_sign sign;
	struct te_furnace furnace;
	struct te_dispenser dispenser;
	struct te_note note;
	struct te_record_player record;
	struct te_spawner spawner;
	struct te_piston_moving piston;
};

static void make_image(struct image* im, int tag) {
	memset(im, 0, sizeof(*im));
	im->chest = (struct te_chest) {.x = 3, .y = 70, .z = 5};
	im->chest.items[0] = (struct item_data) {.id = (uint16_t)(260 + tag), .count = 7};
	im->sign = (struct te_sign) {.x = 3, .y = 71, .z = 5};
	snprintf(im->sign.text, sizeof(im->sign.text), "world%d", tag);
	im->furnace = (struct te_furnace) {.x = 3, .y = 72, .z = 5};
	im->furnace.burn_time = (int16_t)(100 + tag);
	im->furnace.items[0] = (struct item_data) {.id = 15, .count = (uint8_t)(1 + tag)};
	im->dispenser = (struct te_dispenser) {.x = 3, .y = 73, .z = 5};
	im->dispenser.items[4] = (struct item_data) {.id = 262, .count = (uint8_t)(10 + tag)};
	im->note = (struct te_note) {.x = 3, .y = 74, .z = 5, .note = (int8_t)(5 + tag)};
	im->record = (struct te_record_player) {.x = 3, .y = 75, .z = 5, .record = 2256 + tag};
	im->spawner = (struct te_spawner) {.x = 3, .y = 76, .z = 5, .delay = (int16_t)(20 + tag)};
	snprintf(im->spawner.mob_id, sizeof(im->spawner.mob_id), tag ? "Zombie" : "Skeleton");
	im->piston = (struct te_piston_moving) {.x = 3, .y = 77, .z = 5,
		.stored_id = 1, .stored_meta = (int8_t)tag, .facing = 2, .extending = 1,
		.progress = 0.5F};
}

static struct chunk_tile_entities as_chunk(struct image* im) {
	return (struct chunk_tile_entities) {
		.chests = &im->chest, .n_chest = 1,
		.signs = &im->sign, .n_sign = 1,
		.furnaces = &im->furnace, .n_furnace = 1,
		.dispensers = &im->dispenser, .n_dispenser = 1,
		.notes = &im->note, .n_note = 1,
		.record_players = &im->record, .n_record_player = 1,
		.spawners = &im->spawner, .n_spawner = 1,
		.piston_movings = &im->piston, .n_piston_moving = 1,
	};
}

static bool image_matches(const struct chunk_tile_entities* g, int tag) {
	return g->n_chest == 1 && g->chests[0].items[0].id == 260 + tag
		&& g->n_sign == 1 && strncmp(g->signs[0].text, tag ? "world1" : "world0", 6) == 0
		&& g->n_furnace == 1 && g->furnaces[0].burn_time == 100 + tag
		&& g->n_dispenser == 1 && g->dispensers[0].items[4].count == 10 + tag
		&& g->n_note == 1 && g->notes[0].note == 5 + tag
		&& g->n_record_player == 1 && g->record_players[0].record == 2256 + tag
		&& g->n_spawner == 1 && g->spawners[0].delay == 20 + tag
		&& g->n_piston_moving == 1 && g->piston_movings[0].stored_meta == tag;
}

static bool empty(const struct chunk_tile_entities* g) {
	return !g->n_chest && !g->n_sign && !g->n_furnace && !g->n_dispenser
		&& !g->n_note && !g->n_record_player && !g->n_spawner
		&& !g->n_piston_moving;
}

/* The scaffold's nine RED checks plus all eight kinds, save and unload. */
static void same_coordinate_isolation(void) {
	reset_tables();
	struct server_world* overworld = &s.worlds[0];
	struct server_world* nether = &s.worlds[1];
	struct te_store a = server_world_tile_store(overworld);
	struct te_store b = server_world_tile_store(nether);
	CHECK("views keyed", a.owner_keyed && b.owner_keyed
		  && a.owner_slot == 0 && b.owner_slot == 1);
	/* ONE shared backing table: both views alias the same arrays. */
	CHECK("shared backing", a.chest_pos == b.chest_pos
		  && a.chest_items == b.chest_items && a.furnaces == b.furnaces
		  && a.piston_moving == b.piston_moving);

	struct image i0, i1;
	make_image(&i0, 0);
	make_image(&i1, 1);
	struct chunk_tile_entities c0 = as_chunk(&i0), c1 = as_chunk(&i1);
	assert(te_store_insert(&a, &c0));

	struct chunk_tile_entities other = {0};
	assert(te_store_gather(&b, 0, 0, &other));
	CHECK("world 1 sees none of world 0's eight kinds", empty(&other));
	tile_entities_parse_free(&other);

	assert(te_store_insert(&b, &c1));
	struct chunk_tile_entities saved_a = {0}, saved_b = {0};
	assert(te_store_gather(&a, 0, 0, &saved_a));
	assert(te_store_gather(&b, 0, 0, &saved_b));
	CHECK("world-0 save = world-0 image only", image_matches(&saved_a, 0));
	CHECK("world-1 save = world-1 image only", image_matches(&saved_b, 1));
	tile_entities_parse_free(&saved_a);
	tile_entities_parse_free(&saved_b);

	/* Row helpers see only their world's row at identical coordinates. */
	int ra = te_store_find(&a, TE_KIND_CHEST, 3, 70, 5);
	int rb = te_store_find(&b, TE_KIND_CHEST, 3, 70, 5);
	CHECK("distinct rows", ra >= 0 && rb >= 0 && ra != rb);
	CHECK("owned-row predicate", te_store_row_owned(&a, TE_KIND_CHEST, ra)
		  && !te_store_row_owned(&a, TE_KIND_CHEST, rb));
	/* A free of the other world's row through this view is a no-op. */
	te_store_free_row(&a, TE_KIND_CHEST, rb);
	CHECK("free_row cannot reclaim another world's row",
		  te_store_find(&b, TE_KIND_CHEST, 3, 70, 5) == rb);

	te_store_release(&a, 0, 0);
	struct chunk_tile_entities remaining_b = {0}, remaining_a = {0};
	assert(te_store_gather(&b, 0, 0, &remaining_b));
	assert(te_store_gather(&a, 0, 0, &remaining_a));
	CHECK("world-0 unload preserves all eight world-1 kinds",
		  image_matches(&remaining_b, 1));
	CHECK("world-0 unload released world 0", empty(&remaining_a));
	tile_entities_parse_free(&remaining_b);
	tile_entities_parse_free(&remaining_a);
	/* Released rows are world-neutral again. */
	CHECK("released owner keys reset", s.te_owners.chest_slot[ra] == 0
		  && s.te_owners.chest_gen[ra] == 0 && s.chest_pos[ra].y < 0);
	puts("PASS same-coordinate isolation: 8 kinds, save + unload, both directions");
}

/* A re-created instance in the same slot (new generation) owns nothing the
 * old instance stamped. */
static void stale_generation(void) {
	reset_tables();
	struct image i1;
	make_image(&i1, 1);
	struct chunk_tile_entities c1 = as_chunk(&i1);
	struct te_store old_b = server_world_tile_store(&s.worlds[1]);
	assert(te_store_insert(&old_b, &c1));
	uint32_t old_gen = s.worlds[1].generation;
	s.worlds[1].generation = old_gen + 100; /* discard + re-create */
	struct te_store new_b = server_world_tile_store(&s.worlds[1]);
	struct chunk_tile_entities g = {0};
	assert(te_store_gather(&new_b, 0, 0, &g));
	CHECK("stale generation rows invisible to the new instance", empty(&g));
	tile_entities_parse_free(&g);
	CHECK("stale row not found", te_store_find(&new_b, TE_KIND_FURNACE, 3, 72, 5) < 0);
	/* Slot 0 never sees slot 1's rows either. */
	struct te_store a = server_world_tile_store(&s.worlds[0]);
	CHECK("slot 0 blind to slot 1", te_store_find(&a, TE_KIND_FURNACE, 3, 72, 5) < 0);
	/* A non-live / foreign world yields a view that owns and holds nothing. */
	struct server_world stray = {0};
	struct te_store none = server_world_tile_store(&stray);
	CHECK("invalid world view is empty", none.owner_keyed && none.max_chest == 0
		  && !te_store_insert(&none, &c1));
	s.worlds[1].generation = 0;
	none = server_world_tile_store(&s.worlds[1]);
	CHECK("non-live slot view is empty", none.max_chest == 0);
	s.worlds[1].generation = old_gen;
	puts("PASS stale generation: re-created slot and invalid worlds own nothing");
}

/* An unstamped occupied row belongs to NO world (no slot-0 fallback). */
static void unstamped_rows(void) {
	reset_tables();
	s.chest_pos[7] = (struct complex_block_pos) {1, 64, 1};
	struct te_store a = server_world_tile_store(&s.worlds[0]);
	struct te_store b = server_world_tile_store(&s.worlds[1]);
	CHECK("unstamped row invisible to slot 0", te_store_find(&a, TE_KIND_CHEST, 1, 64, 1) < 0);
	CHECK("unstamped row invisible to slot 1", te_store_find(&b, TE_KIND_CHEST, 1, 64, 1) < 0);
	bool existed = true;
	int r = te_store_claim(&b, TE_KIND_CHEST, 1, 64, 1, &existed);
	CHECK("claim takes a new stamped row", r >= 0 && r != 7 && !existed
		  && s.te_owners.chest_slot[r] == 1
		  && s.te_owners.chest_gen[r] == s.worlds[1].generation);
	uint32_t g0 = s.worlds[0].generation;
	s.worlds[0].generation = 0;
	struct te_store dead = server_world_tile_store(&s.worlds[0]);
	CHECK("non-live slot 0 view is empty", dead.max_chest == 0);
	s.worlds[0].generation = g0;
	puts("PASS unstamped rows belong to no world; non-live slot 0 owns nothing");
}

/* Shared capacity (IMP-TE-CAPS): ONE shared, growable table. When world 0
 * fills every row, world 1's insertion grows the shared table instead of
 * being refused (Beta has no tile-entity cap), world 0's rows keep their
 * indices and bytes, and capacity is never duplicated per world. A growth
 * whose allocation fails refuses the insertion whole, losslessly. */
static void* (*s_real_alloc)(size_t);
static void* failing_alloc(size_t n) {
	(void)n;
	return NULL;
}

static void shared_capacity(void) {
	reset_tables();
	struct te_store a = server_world_tile_store(&s.worlds[0]);
	int initial = a.max_chest;
	for(int i = 0; i < initial; i++) {
		bool existed;
		int r = te_store_claim(&a, TE_KIND_CHEST, 16 + (i % 16), 10 + i / 16, 5, &existed);
		assert(r == i && !existed);
	}
	struct te_store b = server_world_tile_store(&s.worlds[1]);
	CHECK("table full", te_store_count_free(&a, TE_KIND_CHEST) == 0
		  && te_store_count_owned(&a, TE_KIND_CHEST) == initial
		  && te_store_count_owned(&b, TE_KIND_CHEST) == 0);
	struct image i1;
	make_image(&i1, 1);
	struct chunk_tile_entities c1 = as_chunk(&i1);
	/* OOM: the growth fails -> refused whole, nothing mutated */
	struct complex_block_pos* chest_before = malloc(sizeof(*chest_before) * initial);
	uint32_t* gen_before = malloc(sizeof(uint32_t) * initial);
	assert(chest_before && gen_before);
	memcpy(chest_before, s.chest_pos, sizeof(*chest_before) * initial);
	memcpy(gen_before, s.te_owners.chest_gen, sizeof(uint32_t) * initial);
	size_t pistons_before = s.piston_moving.count;
	s_real_alloc = s.te_grow.alloc;
	s.te_grow.alloc = failing_alloc;
	uint32_t failures_before = s.te_grow.grow_failures;
	CHECK("OOM preflight refuses", !te_store_can_insert(&b, &c1));
	CHECK("OOM insert refuses", !te_store_insert(&b, &c1));
	s.te_grow.alloc = s_real_alloc;
	CHECK("OOM refusal is lossless and counted",
		  s.te_rows[TE_KIND_CHEST] == initial
		  && memcmp(chest_before, s.chest_pos, sizeof(*chest_before) * initial) == 0
		  && memcmp(gen_before, s.te_owners.chest_gen, sizeof(uint32_t) * initial) == 0
		  && s.piston_moving.count == pistons_before
		  && s.te_grow.grow_failures > failures_before);
	CHECK("no world-1 furnace leaked", te_store_find(&b, TE_KIND_FURNACE, 3, 72, 5) < 0);
	/* With memory available the same insertion grows the shared table. */
	CHECK("insert grows the shared table", te_store_insert(&b, &c1));
	struct te_store a2 = server_world_tile_store(&s.worlds[0]);
	CHECK("one shared table grew x1.5", s.te_rows[TE_KIND_CHEST] == initial + initial / 2
		  && te_store_count_owned(&a2, TE_KIND_CHEST) == initial
		  && te_store_count_owned(&b, TE_KIND_CHEST) == 1
		  && te_store_find(&b, TE_KIND_CHEST, c1.chests[0].x, c1.chests[0].y,
						   c1.chests[0].z) == initial);
	CHECK("world 0 rows kept index and bytes",
		  memcmp(chest_before, s.chest_pos, sizeof(*chest_before) * initial) == 0
		  && memcmp(gen_before, s.te_owners.chest_gen, sizeof(uint32_t) * initial) == 0);
	/* The legacy full-image replacement preflight never counts another world's
	 * rows in (cx,cz) as reclaimable; a shortfall now grows instead. */
	struct te_chest c17 = {.x = 17, .y = 99, .z = 5}; /* chunk (1,0) */
	struct chunk_tile_entities c2 = {.chests = &c17, .n_chest = 1};
	CHECK("legacy replace preflight: foreign rows untouched",
		  te_store_can_replace_legacy_chunk(&b, &c2, 1, 0)
		  && te_store_count_owned(&a2, TE_KIND_CHEST) == initial
		  && te_store_count_owned(&b, TE_KIND_CHEST) == 1);
	te_store_release_legacy_chunk(&b, 1, 0); /* b owns nothing there */
	CHECK("legacy release never frees a foreign row",
		  te_store_count_owned(&a2, TE_KIND_CHEST) == initial);
	free(chest_before);
	free(gen_before);
	puts("PASS shared capacity: one growable shared table; OOM refuses losslessly");
}

/* Moving pistons: owners live in the dynamic entries; both worlds keep their
 * own entry at identical coordinates, stable indices, and per-world relative
 * (monotonic) order; charged capacity is the real allocation. */
static void pistons(void) {
	reset_tables();
	struct piston_moving_state st = {.stored_id = 1, .facing = 2};
	struct piston_moving_owner o0 = server_world_piston_owner(&s.worlds[0]);
	struct piston_moving_owner o1 = server_world_piston_owner(&s.worlds[1]);
	size_t i0a, i1a, i0b;
	assert(piston_moving_store_upsert_owned(&s.piston_moving, o0, 1, 60, 1, &st, &i0a));
	assert(piston_moving_store_upsert_owned(&s.piston_moving, o1, 1, 60, 1, &st, &i1a));
	assert(piston_moving_store_upsert_owned(&s.piston_moving, o0, 2, 60, 1, &st, &i0b));
	CHECK("two entries at identical coordinates", i0a != i1a && s.piston_moving.count == 3);
	size_t f;
	CHECK("owned find world 0", piston_moving_store_find_owned(&s.piston_moving, o0, 1, 60, 1, &f) && f == i0a);
	CHECK("owned find world 1", piston_moving_store_find_owned(&s.piston_moving, o1, 1, 60, 1, &f) && f == i1a);
	CHECK("per-world order monotonic", s.piston_moving.entries[i0a].order
		  < s.piston_moving.entries[i0b].order);
	/* Replacing world 1's entry never touches world 0's (stable index). */
	st.stored_id = 4;
	size_t again;
	assert(piston_moving_store_upsert_owned(&s.piston_moving, o1, 1, 60, 1, &st, &again));
	CHECK("stable index on replace", again == i1a
		  && s.piston_moving.entries[i0a].state.stored_id == 1);
	CHECK("remove owned only", piston_moving_store_remove_owned(&s.piston_moving, o1, 1, 60, 1)
		  && s.piston_moving.entries[i0a].used && !s.piston_moving.entries[i1a].used
		  && s.piston_moving.entries[i1a].owner_generation == 0);
	CHECK("allocated bytes = capacity", piston_moving_store_allocated_bytes(&s.piston_moving)
		  == s.piston_moving.capacity * sizeof(struct piston_moving_store_entry));
	puts("PASS pistons: owned entries, stable indices, per-world order");
}

/* ABI: owner keys stay 5 bytes per row (IMP-TE-CAPS: per-kind growable
 * slabs now); piston entries did not grow. The INITIAL row counts are the
 * historical fixed sizes: 256 chests, 256 signs, 64 furnaces/dispensers/note
 * blocks/jukeboxes (768 rows) plus 2 * MAX_CHUNKS spawners. */
static void memory_layout(void) {
	reset_tables();
	CHECK("spawner rows = 2 x MAX_CHUNKS", MAX_SPAWNER_TES == 2 * MAX_CHUNKS);
	int rows = 0;
	for(int k = 0; k < TE_KIND_FIXED_COUNT; k++)
		rows += s.te_rows[k];
	CHECK("initial rows = 768 fixed + spawners", rows == 768 + 2 * MAX_CHUNKS);
	CHECK("owner keys 5 B per row",
		  sizeof(*s.te_owners.chest_gen) + sizeof(*s.te_owners.chest_slot) == 5);
	CHECK("piston entry unchanged", sizeof(struct piston_moving_store_entry) == 40);
	printf("PASS memory layout: owner keys=%zu B, piston entry=%zu B, "
		   "server_world_handle=%zu B, active_spawner=%zu B\n",
		   (size_t)rows * 5, sizeof(struct piston_moving_store_entry),
		   sizeof(struct server_world_handle), sizeof(struct active_spawner));
}

int main(void) {
	bind_world(0, 101);
	bind_world(1, 202);
	assert(&s.worlds[0] != &s.worlds[1]);
	same_coordinate_isolation();
	stale_generation();
	unstamped_rows();
	shared_capacity();
	pistons();
	memory_layout();
	reset_tables();
	if(failures)
		fprintf(stderr, "A1b: %d check(s) failed\n", failures);
	return failures ? 1 : 0;
}
