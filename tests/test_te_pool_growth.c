/* IMP-TE-CAPS (2026-10-21): growable tile-entity tables.
 *
 * Beta 1.7.3 keeps tile entities in unbounded maps (Chunk.chunkTileEntityMap,
 * World.loadedTileEntityList), so CaveWii's seven tile tables must never cap
 * a real world. This pins the growable-slab contract through the PRODUCTION
 * view (server_world_tile_store over server_local's slabs):
 *
 *  1. claims past the old fixed sizes (64 note blocks, 256 signs, ...) grow
 *     the table x1.5; every existing row keeps its index and bytes;
 *  2. row choice is unchanged: a growable table hands out exactly the rows a
 *     large fixed table does for the same claim/free sequence (append-only);
 *  3. a dense imported chunk (384 Music + 300 Sign + 300 Chest + 70 Furnace +
 *     70 Trap + 44 RecordPlayer + 70 MobSpawner) inserts whole, gathers back
 *     identical and in order, and releases;
 *  4. same-position records overwrite (resident row and in-list duplicate)
 *     exactly as before;
 *  5. allocation failure is bounded: the claim / insert is refused whole,
 *     nothing mutates, the failure is counted; it succeeds once memory is
 *     back;
 *  6. memory accounting follows the live rows. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network/server_local_state.h"
#include "network/tile_entities_store.h"

static struct server_local s, ref;

static void fresh(struct server_local* x) {
	memset(x, 0, sizeof(*x));
	if(!server_local_te_tables_init_heap(x))
		abort();
	piston_moving_store_init(&x->piston_moving);
	x->worlds[0].world_index = 0;
	x->worlds[0].session = x;
	x->worlds[0].generation = 7;
}

static struct te_store view(struct server_local* x) {
	return server_world_tile_store(&x->worlds[0]);
}

static bool fail_next;
static void* maybe_alloc(size_t n) {
	return fail_next ? NULL : malloc(n);
}

/* ---- 1 + 6: growth keeps rows, accounting follows -------------------- */
static void growth_keeps_rows(void) {
	fresh(&s);
	struct te_store st = view(&s);
	assert(st.max_note == MAX_NOTEBLOCKS && st.max_sign == MAX_SIGNS);
	size_t bytes0 = s.te_grow.bytes_live;
	assert(bytes0 > 0);
	uint32_t epoch0 = s.te_grow.epoch;
	for(int i = 0; i < 1000; i++) {
		int r = te_store_claim(&st, TE_KIND_NOTE, i, 64, 3, NULL);
		assert(r == i);
		st.notes[r] = (int8_t)(i % 25);
	}
	for(int i = 0; i < 600; i++) {
		int r = te_store_claim_free(&st, TE_KIND_SIGN, i, 65, 3);
		assert(r == i);
		memcpy(st.sign_texts + (size_t)r * (size_t)st.sign_size, "sign", 4);
	}
	assert(st.max_note >= 1000 && st.max_sign >= 600);
	assert(s.te_rows[TE_KIND_NOTE] == st.max_note);
	assert(s.te_grow.bytes_live == bytes0 + (size_t)(st.max_note - MAX_NOTEBLOCKS) * 19
		   + (size_t)(st.max_sign - MAX_SIGNS) * (12 + SIGN_SIZE + 5 + 1));
	assert(s.te_grow.epoch > epoch0 && st.grow_epoch == s.te_grow.epoch);
	/* x1.5 steps from 64: 64,96,144,216,324,486,729,1093 */
	assert(st.max_note == 1093);
	for(int i = 0; i < 1000; i++)
		assert(st.note_pos[i].x == i && st.notes[i] == i % 25
			   && te_store_row_owned(&st, TE_KIND_NOTE, i));
	for(int i = 0; i < 600; i++)
		assert(st.sign_pos[i].x == i
			   && memcmp(st.sign_texts + (size_t)i * (size_t)st.sign_size, "sign", 4) == 0);
	/* new rows are free and blank: positions -1, sign text spaces */
	assert(st.note_pos[1000].y < 0 && st.sign_pos[600].y < 0
		   && st.sign_texts[(size_t)600 * (size_t)st.sign_size] == ' ');
	assert(s.te_grow.bytes_live > bytes0
		   && s.te_grow.bytes_peak >= s.te_grow.bytes_live);
	/* release + reuse: the lowest free row is reused, no growth */
	int rows = st.max_note;
	te_store_free_row(&st, TE_KIND_NOTE, 17);
	te_store_free_row(&st, TE_KIND_NOTE, 900);
	assert(te_store_claim(&st, TE_KIND_NOTE, 5000, 64, 3, NULL) == 17);
	assert(te_store_claim(&st, TE_KIND_NOTE, 5001, 64, 3, NULL) == 900);
	assert(st.max_note == rows);
	printf("PASS growth: 1000 notes / 600 signs past the old 64/256, rows "
		   "stable, %zu -> %zu B\n", bytes0, s.te_grow.bytes_live);
}

/* ---- 2: same rows as a large fixed table ------------------------------ */
static void same_rows_as_fixed(void) {
	fresh(&s);
	fresh(&ref);
	struct te_store g = view(&s);
	struct te_store f = view(&ref);
	assert(te_store_grow(&f, TE_KIND_FURNACE, 4096)); /* "fixed" and huge */
	unsigned rng = 12345u;
	for(int op = 0; op < 20000; op++) {
		rng = rng * 1103515245u + 12345u;
		int x = (int)((rng >> 8) % 1500);
		if((rng >> 4) & 3) {
			bool e1, e2;
			int r1 = te_store_claim(&g, TE_KIND_FURNACE, x, 70, 0, &e1);
			int r2 = te_store_claim(&f, TE_KIND_FURNACE, x, 70, 0, &e2);
			assert(r1 == r2 && e1 == e2 && r1 >= 0);
		} else {
			int r1 = te_store_find(&g, TE_KIND_FURNACE, x, 70, 0);
			int r2 = te_store_find(&f, TE_KIND_FURNACE, x, 70, 0);
			assert(r1 == r2);
			if(r1 >= 0) {
				te_store_free_row(&g, TE_KIND_FURNACE, r1);
				te_store_free_row(&f, TE_KIND_FURNACE, r2);
			}
		}
	}
	assert(g.max_furnace < f.max_furnace);
	printf("PASS row choice: 20000 claim/free ops, growable == fixed-4096 "
		   "rows (%d vs %d rows allocated)\n", g.max_furnace, f.max_furnace);
}

/* ---- 3 + 4: dense chunk insert / gather / release ---------------------- */
static void dense_pos(int i, int32_t* x, int32_t* y, int32_t* z) {
	*x = 32 + i % 16; /* chunk (2,-1) */
	*z = -16 + (i / 16) % 16;
	*y = i / 256;
}

static void dense_chunk(void) {
	enum { NOTES = 384, SIGNS = 300, CHESTS = 300, FURNACES = 70,
		   DISPENSERS = 70, RECORDS = 44, SPAWNERS = 70 };
	fresh(&s);
	struct te_store st = view(&s);
	/* residents elsewhere first, so rows are interleaved with other chunks */
	for(int i = 0; i < 50; i++)
		assert(te_store_claim(&st, TE_KIND_NOTE, -100 - i, 10, 7, NULL) == i);
	struct chunk_tile_entities in = {0};
	in.notes = calloc(NOTES + 1, sizeof(*in.notes));
	in.signs = calloc(SIGNS, sizeof(*in.signs));
	in.chests = calloc(CHESTS, sizeof(*in.chests));
	in.furnaces = calloc(FURNACES, sizeof(*in.furnaces));
	in.dispensers = calloc(DISPENSERS, sizeof(*in.dispensers));
	in.record_players = calloc(RECORDS, sizeof(*in.record_players));
	in.spawners = calloc(SPAWNERS, sizeof(*in.spawners));
	int at = 0;
	for(int i = 0; i < NOTES; i++, at++) {
		dense_pos(at, &in.notes[i].x, &in.notes[i].y, &in.notes[i].z);
		in.notes[i].note = (int8_t)(i % 25);
	}
	/* an in-list duplicate of note 5: same row, later content wins */
	in.notes[NOTES] = in.notes[5];
	in.notes[NOTES].note = 24;
	for(int i = 0; i < SIGNS; i++, at++) {
		dense_pos(at, &in.signs[i].x, &in.signs[i].y, &in.signs[i].z);
		snprintf(in.signs[i].text, sizeof(in.signs[i].text), "s%d", i);
	}
	for(int i = 0; i < CHESTS; i++, at++) {
		dense_pos(at, &in.chests[i].x, &in.chests[i].y, &in.chests[i].z);
		in.chests[i].items[i % TE_CHEST_SLOTS]
			= (struct item_data) {.id = 1 + i % 90, .count = 1 + i % 64};
	}
	for(int i = 0; i < FURNACES; i++, at++) {
		dense_pos(at, &in.furnaces[i].x, &in.furnaces[i].y, &in.furnaces[i].z);
		in.furnaces[i].burn_time = (int16_t)i;
	}
	for(int i = 0; i < DISPENSERS; i++, at++)
		dense_pos(at, &in.dispensers[i].x, &in.dispensers[i].y,
				  &in.dispensers[i].z);
	for(int i = 0; i < RECORDS; i++, at++) {
		dense_pos(at, &in.record_players[i].x, &in.record_players[i].y,
				  &in.record_players[i].z);
		in.record_players[i].record = 2256 + i % 2;
	}
	for(int i = 0; i < SPAWNERS; i++, at++) {
		dense_pos(at, &in.spawners[i].x, &in.spawners[i].y, &in.spawners[i].z);
		strcpy(in.spawners[i].mob_id, "Pig");
		in.spawners[i].delay = (int16_t)i;
	}
	in.n_note = NOTES + 1;
	in.n_sign = SIGNS;
	in.n_chest = CHESTS;
	in.n_furnace = FURNACES;
	in.n_dispenser = DISPENSERS;
	in.n_record_player = RECORDS;
	in.n_spawner = SPAWNERS;

	/* OOM first: refused whole, nothing mutated, failure counted */
	size_t live = s.te_grow.bytes_live;
	int note_rows = st.max_note;
	s.te_grow.alloc = maybe_alloc;
	fail_next = true;
	uint32_t fails = s.te_grow.grow_failures;
	assert(!te_store_can_insert(&st, &in));
	assert(!te_store_insert(&st, &in));
	assert(s.te_grow.grow_failures > fails && st.max_note == note_rows
		   && s.te_grow.bytes_live == live
		   && te_store_count_owned(&st, TE_KIND_NOTE) == 50
		   && te_store_count_owned(&st, TE_KIND_SIGN) == 0);
	assert(te_store_claim(&st, TE_KIND_FURNACE, 1, 1, 1, NULL) >= 0); /* free rows: no alloc */
	te_store_free_row(&st, TE_KIND_FURNACE, te_store_find(&st, TE_KIND_FURNACE, 1, 1, 1));
	fail_next = false;

	assert(te_store_can_insert(&st, &in));
	assert(te_store_insert(&st, &in));
	assert(te_store_count_owned(&st, TE_KIND_NOTE) == 50 + NOTES
		   && te_store_count_owned(&st, TE_KIND_SIGN) == SIGNS
		   && te_store_count_owned(&st, TE_KIND_CHEST) == CHESTS
		   && te_store_count_owned(&st, TE_KIND_FURNACE) == FURNACES
		   && te_store_count_owned(&st, TE_KIND_DISPENSER) == DISPENSERS
		   && te_store_count_owned(&st, TE_KIND_RECORD_PLAYER) == RECORDS
		   && te_store_count_owned(&st, TE_KIND_SPAWNER) == SPAWNERS);
	/* the resident rows kept their indices; the chunk took the next rows */
	for(int i = 0; i < 50; i++)
		assert(st.note_pos[i].x == -100 - i);
	assert(te_store_find(&st, TE_KIND_NOTE, in.notes[0].x, in.notes[0].y,
						 in.notes[0].z) == 50);
	assert(st.notes[te_store_find(&st, TE_KIND_NOTE, in.notes[5].x,
								  in.notes[5].y, in.notes[5].z)] == 24);

	struct chunk_tile_entities out;
	assert(te_store_gather(&st, 2, -1, &out));
	assert(out.n_note == NOTES && out.n_sign == SIGNS && out.n_chest == CHESTS
		   && out.n_furnace == FURNACES && out.n_dispenser == DISPENSERS
		   && out.n_record_player == RECORDS && out.n_spawner == SPAWNERS);
	for(int i = 0; i < NOTES; i++)
		assert(out.notes[i].x == in.notes[i].x && out.notes[i].y == in.notes[i].y
			   && out.notes[i].z == in.notes[i].z
			   && out.notes[i].note == (i == 5 ? 24 : in.notes[i].note));
	for(int i = 0; i < SIGNS; i++)
		assert(strcmp(out.signs[i].text, in.signs[i].text) == 0
			   || strncmp(out.signs[i].text, in.signs[i].text,
						  strlen(in.signs[i].text)) == 0);
	for(int i = 0; i < CHESTS; i++)
		assert(memcmp(out.chests[i].items, in.chests[i].items,
					  sizeof(in.chests[i].items)) == 0);
	for(int i = 0; i < SPAWNERS; i++)
		assert(out.spawners[i].delay == i && !strcmp(out.spawners[i].mob_id, "Pig"));

	/* re-inserting the same chunk (resident rows present -> scan path)
	 * overwrites by position: no new rows */
	int notes_rows = st.max_note;
	out.notes[0].note = 3;
	assert(te_store_insert(&st, &out));
	assert(te_store_count_owned(&st, TE_KIND_NOTE) == 50 + NOTES
		   && st.max_note == notes_rows
		   && st.notes[50] == 3);
	tile_entities_parse_free(&out);

	te_store_release(&st, 2, -1);
	assert(te_store_count_owned(&st, TE_KIND_NOTE) == 50
		   && te_store_count_owned(&st, TE_KIND_SIGN) == 0
		   && te_store_count_owned(&st, TE_KIND_CHEST) == 0);
	tile_entities_parse_free(&in);
	printf("PASS dense chunk: %d Music + %d Sign + %d Chest + %d Furnace + %d Trap"
		   " + %d RecordPlayer + %d MobSpawner insert/gather/release; OOM "
		   "refused whole\n",
		   NOTES, SIGNS, CHESTS, FURNACES, DISPENSERS, RECORDS, SPAWNERS);
}


/* ---- 7: sign rows past 65535 stay addressable ------------------------ *
 * Review round 1 (Sol): the split-world sign publication cursor was a
 * uint16_t; with >= 65536 sign rows and back-pressure it wrapped and replayed
 * a prefix forever. The table now reaches such rows, and the cursor keeps
 * the exact next row index. */
static void sign_rows_past_uint16(void) {
	fresh(&s);
	struct te_store st = view(&s);
	assert(te_store_grow(&st, TE_KIND_SIGN, 70000) && st.max_sign >= 70000);
	int high = 69999;
	st.sign_pos[high] = (struct te_pos) {5, 64, 5};
	te_stamp(&st, TE_KIND_SIGN, high);
	assert(te_store_find(&st, TE_KIND_SIGN, 5, 64, 5) == high);
	struct server_chunk c = {0};
	unsigned k = (unsigned)high;
	c.a2w_sign_cursor = k + 1; /* the publication loop's resume store */
	assert(c.a2w_sign_cursor == 70000u
		   && "the resume cursor keeps rows past 65535 exactly");
	printf("PASS high sign rows: row %d found, resume cursor %u exact\n", high,
		   (unsigned)c.a2w_sign_cursor);
}

int main(void) {
	growth_keeps_rows();
	same_rows_as_fixed();
	dense_chunk();
	sign_rows_past_uint16();
	puts("test_te_pool_growth: OK");
	return 0;
}
