/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S (astra-a2s-design-0926 "test_a2_restore_atomic"): every fallible step
 * of the flat-archive reload leaves the OLD state intact -- prior rows,
 * inventories, sign bytes, owners, and the other world's rows:
 *   1. every scratch allocation failure of prepare (all six, one at a time);
 *   2. a corrupt/unsupported SIGN archive after a VALID chest archive refuses
 *      the whole plan (both kinds prepared together: nothing installed);
 *   3. capacity: more incoming rows than (replaceable + unreserved free) rows
 *      refuses at preflight with nothing reserved;
 *   4. a reservation resists an intervening slot-1 claim between preflight and
 *      commit: the claim lands elsewhere, the commit installs where planned;
 *   5. a marked-empty replacement (empty image) releases only the target's
 *      rows; the other world's rows survive;
 *   6. abort after preflight releases the reservations and touches nothing.
 * RED by construction until TE_RESTORE_PLAN_API (tile_entities_store.h). */
#include "support/split_residency_fixture.h"

#include "item/item_types.h"
#include "network/complex_block_archive.h"
#include "network/tile_entities_store.h"

#include <stdio.h>

#ifndef TE_RESTORE_PLAN_API
int main(void) {
	puts("RED: TE_RESTORE_PLAN_API absent");
	return 1;
}
#else

static struct server_world* make_slot1(struct server_local* s) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, true);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

static void fill_chest(struct server_local* s, int row, uint16_t base) {
	for(int k = 0; k < MAX_CHEST_SLOTS; k++)
		s->chest_items[row][k] = (struct item_data) {
			.id = (uint16_t)(base + k), .count = (uint8_t)(1 + (k % 60))};
}

/* Whole-table snapshot: positions, owner keys, payloads, reservations. */
struct snap {
	struct complex_block_pos cpos[MAX_CHESTS], spos[MAX_SIGNS];
	/* IMP-TE-CAPS: owner keys are per-kind slabs; snapshot their bytes */
	uint32_t cgen[MAX_CHESTS], sgen[MAX_SIGNS];
	uint8_t cslot[MAX_CHESTS], sslot[MAX_SIGNS];
	struct item_data items[MAX_CHESTS][MAX_CHEST_SLOTS];
	char texts[MAX_SIGNS][SIGN_SIZE];
	uint8_t cres[MAX_CHESTS], sres[MAX_SIGNS];
};

static void take(struct server_local* s, struct snap* out) {
	memcpy(out->cpos, s->chest_pos, sizeof(out->cpos));
	memcpy(out->spos, s->sign_pos, sizeof(out->spos));
	/* an empty row is (y < 0) whatever x/z say (te_release_row keeps them) */
	for(int i = 0; i < MAX_CHESTS; i++)
		if(out->cpos[i].y < 0)
			out->cpos[i] = (struct complex_block_pos) {-1, -1, -1};
	for(int i = 0; i < MAX_SIGNS; i++)
		if(out->spos[i].y < 0)
			out->spos[i] = (struct complex_block_pos) {-1, -1, -1};
	memcpy(out->cgen, s->te_owners.chest_gen, sizeof(out->cgen));
	memcpy(out->sgen, s->te_owners.sign_gen, sizeof(out->sgen));
	memcpy(out->cslot, s->te_owners.chest_slot, sizeof(out->cslot));
	memcpy(out->sslot, s->te_owners.sign_slot, sizeof(out->sslot));
	memcpy(out->items, s->chest_items, sizeof(out->items));
	memcpy(out->texts, s->sign_texts, sizeof(out->texts));
	/* an empty row's payload is not state: the reload blanks it (items 0,
	 * text spaces -- the pre-A2-S full-image wipe) while a fresh table is
	 * zero-filled; OWNED rows compare byte-exact. */
	for(int i = 0; i < MAX_CHESTS; i++)
		if(out->cpos[i].y < 0)
			memset(out->items[i], 0, sizeof(out->items[i]));
	for(int i = 0; i < MAX_SIGNS; i++)
		if(out->spos[i].y < 0)
			memset(out->texts[i], ' ', sizeof(out->texts[i]));
	memcpy(out->cres, s->chest_reserved, sizeof(out->cres));
	memcpy(out->sres, s->sign_reserved, sizeof(out->sres));
}

static bool same(struct server_local* s, const struct snap* a) {
	static struct snap b;
	memset(&b, 0, sizeof(b));
	take(s, &b);
	return memcmp(a, &b, sizeof(b)) == 0;
}

static void* fail_alloc(size_t n) {
	(void)n;
	return NULL;
}

static bool no_reservations(struct server_local* s) {
	for(int i = 0; i < MAX_CHESTS; i++)
		if(s->chest_reserved[i])
			return false;
	for(int i = 0; i < MAX_SIGNS; i++)
		if(s->sign_reserved[i])
			return false;
	return true;
}

static void write_bytes(const char* path, const void* data, size_t n) {
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, 1, n, f) == n);
	fclose(f);
}

int main(void) {
	struct server_local* s = split_fixture();
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_slot1(s);
	struct te_store sa = server_world_tile_store(a);
	struct te_store sb = server_world_tile_store(b);
	string_t path;
	string_init_set_str(path, split_path);

	/* Scene: A owns chests 0..2 + signs 0..1 (archived), B owns chest 3 and
	 * sign 2 at colliding coordinates. */
	for(int i = 0; i < 3; i++) {
		assert(te_store_claim(&sa, TE_KIND_CHEST, 10 + i, 64, 10, NULL) == i);
		fill_chest(s, i, (uint16_t)(100 * (i + 1)));
	}
	for(int i = 0; i < 2; i++)
		assert(te_store_claim(&sa, TE_KIND_SIGN, 20 + i, 65, 20, NULL) == i);
	memcpy(s->sign_texts[0], "alpha", 5);
	assert(chest_archive_write_w(&sa, path, a->dimension));
	assert(sign_archive_write_w(&sa, path, a->dimension));
	assert(te_store_claim(&sb, TE_KIND_CHEST, 10, 64, 10, NULL) == 3);
	fill_chest(s, 3, 7000);
	assert(te_store_claim(&sb, TE_KIND_SIGN, 20, 65, 20, NULL) == 2);
	memcpy(s->sign_texts[2], "nether", 6);
	struct snap before;
	take(s, &before);
	struct tile_restore_plan plan;

	/* ---- 1. every scratch allocation failure refuses, nothing touched ---- */
	for(unsigned nth = 1; nth <= 6; nth++) {
		te_restore_test_fail_alloc(nth);
		assert(complex_archive_prepare_w(&sa, path, a->dimension, &plan)
			   == STORAGE_CAPACITY);
		assert(!plan.prepared && same(s, &before));
	}
	te_restore_test_fail_alloc(0);
	puts("PASS 1: six allocation failures, each refused intact");

	/* ---- 2. a corrupt sign archive after a valid chest archive ---- */
	{
		char bad[256];
		snprintf(bad, sizeof(bad), "%s/signs.dat", split_path);
		char keep[256];
		snprintf(keep, sizeof(keep), "%s/signs.keep", split_path);
		assert(rename(bad, keep) == 0);
		/* unsupported format version byte, then junk */
		uint8_t junk[64];
		memset(junk, 0xEE, sizeof(junk));
		write_bytes(bad, junk, sizeof(junk));
		/* also poison the _old fallback so the reader cannot recover */
		char old[256];
		snprintf(old, sizeof(old), "%s/signs.dat_old", split_path);
		write_bytes(old, junk, sizeof(junk));
		assert(complex_archive_prepare_w(&sa, path, a->dimension, &plan)
			   == STORAGE_CORRUPT);
		assert(!plan.prepared && same(s, &before)
			   && "the valid chest image never reaches the table alone");
		remove(bad);
		remove(old);
		assert(rename(keep, bad) == 0);
	}
	puts("PASS 2: corrupt sign after valid chest refuses both");

	/* ---- 3. capacity: fill every free chest row with B's rows so A's three
	 * incoming rows exceed (replaceable 3 + free 0)? -- make it exceed: also
	 * release one of A's rows so only 2 are replaceable and 0 free ---- */
	{
		int filled = 0;
		for(int i = 4; i < MAX_CHESTS; i++) {
			int r = te_store_claim(&sb, TE_KIND_CHEST, 1000 + i, 64, 1000, NULL);
			assert(r == i);
			filled++;
		}
		assert(filled == MAX_CHESTS - 4);
		te_store_free_row(&sa, TE_KIND_CHEST, 2); /* A owns 2 rows now */
		assert(te_store_claim(&sb, TE_KIND_CHEST, 999, 64, 999, NULL) == 2);
		static struct snap full;
		take(s, &full);
		int rows_before = s->te_rows[TE_KIND_CHEST];
		/* IMP-TE-CAPS: a shortfall now grows the table; only a growth whose
		 * allocation fails is a capacity refusal (lossless, as before). */
		void* (*real_alloc)(size_t) = s->te_grow.alloc;
		s->te_grow.alloc = fail_alloc;
		assert(complex_archive_prepare_w(&sa, path, a->dimension, &plan)
			   == STORAGE_OK);
		assert(te_restore_preflight(&sa, &plan) == STORAGE_CAPACITY);
		s->te_grow.alloc = real_alloc;
		assert(no_reservations(s) && same(s, &full)
			   && s->te_rows[TE_KIND_CHEST] == rows_before
			   && "OOM capacity refusal reserves and mutates nothing");
		te_restore_abort(&plan);
		assert(same(s, &full));
		assert(complex_archive_prepare_w(&sa, path, a->dimension, &plan)
			   == STORAGE_OK);
		assert(te_restore_preflight(&sa, &plan) == STORAGE_OK
			   && s->te_rows[TE_KIND_CHEST] > rows_before
			   && "the shortfall grew the shared table");
		te_restore_abort(&plan);
		sb = server_world_tile_store(b); /* re-point after the growth */
		assert(no_reservations(s) && same(s, &full)
			   && "growth kept every row's index and bytes");
		/* undo: give B's extra rows back */
		for(int i = 2; i < MAX_CHESTS; i++)
			if(i != 3)
				te_store_free_row(&sb, TE_KIND_CHEST, i);
		assert(te_store_claim(&sa, TE_KIND_CHEST, 12, 64, 10, NULL) == 2);
		fill_chest(s, 2, 300);
		assert(same(s, &before));
	}
	puts("PASS 3: shortfall grows; OOM refuses at preflight, nothing reserved");

	/* ---- 4. reservations resist an intervening slot-1 claim ---- */
	{
		/* Release A's rows: the incoming rows 0..2 map to FREE rows 0..2 and
		 * are reserved; B's claim in between must skip them. */
		for(int i = 0; i < 3; i++)
			te_store_free_row(&sa, TE_KIND_CHEST, i);
		assert(complex_archive_prepare_w(&sa, path, a->dimension, &plan)
			   == STORAGE_OK);
		assert(te_restore_preflight(&sa, &plan) == STORAGE_OK);
		assert(s->chest_reserved[0] && s->chest_reserved[1]
			   && s->chest_reserved[2] && !s->chest_reserved[3]);
		int rb = te_store_claim(&sb, TE_KIND_CHEST, 55, 64, 55, NULL);
		assert(rb == 4 && "the intervening claim skips the reserved rows");
		assert(te_restore_commit(&plan));
		assert(no_reservations(s));
		for(int i = 0; i < 3; i++)
			assert(te_store_row_owned(&sa, TE_KIND_CHEST, i)
				   && s->chest_items[i][0].id == (uint16_t)(100 * (i + 1)));
		assert(te_store_row_owned(&sb, TE_KIND_CHEST, 4)
			   && te_store_row_owned(&sb, TE_KIND_CHEST, 3)
			   && s->chest_items[3][0].id == 7000);
		te_store_free_row(&sb, TE_KIND_CHEST, 4);
		assert(same(s, &before));
	}
	puts("PASS 4: reserved rows survive an intervening slot-1 claim");

	/* ---- 6. abort after preflight: reservations released, table intact ---- */
	{
		assert(complex_archive_prepare_w(&sa, path, a->dimension, &plan)
			   == STORAGE_OK);
		assert(te_restore_preflight(&sa, &plan) == STORAGE_OK);
		assert(!no_reservations(s));
		te_restore_abort(&plan);
		assert(no_reservations(s) && same(s, &before));
		te_restore_abort(&plan); /* idempotent */
		assert(same(s, &before));
	}
	puts("PASS 6: abort releases reservations, touches nothing");

	/* ---- 5. marked-empty replacement: an empty image empties A only ---- */
	{
		string_t empty_root;
		string_init_printf(empty_root, "%s/empty2", split_path);
#ifdef _WIN32
		_mkdir(string_get_cstr(empty_root));
#else
		mkdir(string_get_cstr(empty_root), 0777);
#endif
		assert(complex_archive_prepare_w(&sa, empty_root, a->dimension, &plan)
			   == STORAGE_OK);
		assert(plan.prepared);
		assert(te_restore_preflight(&sa, &plan) == STORAGE_OK);
		assert(te_restore_commit(&plan));
		assert(te_store_count_owned(&sa, TE_KIND_CHEST) == 0
			   && te_store_count_owned(&sa, TE_KIND_SIGN) == 0);
		assert(te_store_row_owned(&sb, TE_KIND_CHEST, 3)
			   && s->chest_items[3][0].id == 7000
			   && te_store_row_owned(&sb, TE_KIND_SIGN, 2)
			   && memcmp(s->sign_texts[2], "nether", 6) == 0);
		assert(s->chest_items[0][0].id == 0 && "released payload is blank");
		string_clear(empty_root);
	}
	puts("PASS 5: empty image releases only the target's rows");

	string_clear(path);
	server_world_discard(b);
	split_destroy(s);
	return 0;
}
#endif
