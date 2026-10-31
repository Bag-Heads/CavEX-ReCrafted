/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S (astra-a2s-design-0926 "test_a2_slot0_reload"): a slot-0 reload of the
 * flat chest/sign archives while slot 1 RETAINS rows in the shared table.
 * Slot 1 owns rows at indices and coordinates that COLLIDE with slot 0's
 * archived rows; the reload (complex_archive_prepare_w -> te_restore_preflight
 * -> te_restore_commit) reinstalls every slot-0 row losslessly -- all 54 chest
 * slots, the full 64-byte sign text -- at the original index when it is
 * available and otherwise at the first replaceable/free row, keeps every
 * slot-1 row byte-for-byte at its index, and, with no foreign rows at all
 * (single world), reproduces the pre-A2-S full-image layout exactly.
 *
 * RED by construction until TE_RESTORE_PLAN_API (tile_entities_store.h). */
#include "support/split_residency_fixture.h"

#include "item/item_types.h"
#include "network/complex_block_archive.h"
#include "network/tile_entities_store.h"

#include <stdio.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR_COMPAT(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR_COMPAT(p) mkdir(p, 0777)
#endif

#ifndef TE_RESTORE_PLAN_API
int main(void) {
	puts("RED: TE_RESTORE_PLAN_API absent -- flat archives still reload through "
		 "the full-image reader");
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

static bool chest_matches(struct server_local* s, int row, uint16_t base) {
	for(int k = 0; k < MAX_CHEST_SLOTS; k++)
		if(s->chest_items[row][k].id != (uint16_t)(base + k)
		   || s->chest_items[row][k].count != (uint8_t)(1 + (k % 60)))
			return false;
	return true;
}

static void fill_sign(struct server_local* s, int row, char seed) {
	for(int k = 0; k < SIGN_SIZE; k++)
		s->sign_texts[row][k] = (char)('!' + ((seed + k) % 90));
}

static bool sign_matches(struct server_local* s, int row, char seed) {
	for(int k = 0; k < SIGN_SIZE; k++)
		if(s->sign_texts[row][k] != (char)('!' + ((seed + k) % 90)))
			return false;
	return true;
}

static bool reload(struct te_store* st, string_t path, enum world_dim dim) {
	struct tile_restore_plan plan;
	if(complex_archive_prepare_w(st, path, dim, &plan) != STORAGE_OK)
		return false;
	if(te_restore_preflight(st, &plan) != STORAGE_OK) {
		te_restore_abort(&plan);
		return false;
	}
	return te_restore_commit(&plan);
}

int main(void) {
	struct server_local* s = split_fixture();
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_slot1(s);
	struct te_store sa = server_world_tile_store(a);
	struct te_store sb = server_world_tile_store(b);
	string_t path;
	string_init_set_str(path, split_path);

	/* ---- 1. single world: slot 0 owns rows 0..3 (chests) and 0..2 (signs),
	 * archives written, table released, reloaded -> identity layout ---- */
	int ca[4], ga[3];
	for(int i = 0; i < 4; i++) {
		ca[i] = te_store_claim(&sa, TE_KIND_CHEST, 10 + i, 64, 10, NULL);
		assert(ca[i] == i);
		fill_chest(s, ca[i], (uint16_t)(100 * (i + 1)));
	}
	for(int i = 0; i < 3; i++) {
		ga[i] = te_store_claim(&sa, TE_KIND_SIGN, 20 + i, 65, 20, NULL);
		assert(ga[i] == i);
		fill_sign(s, ga[i], (char)(7 * i));
	}
	assert(chest_archive_write_w(&sa, path, a->dimension));
	assert(sign_archive_write_w(&sa, path, a->dimension));
	struct complex_block_pos chest_before[MAX_CHESTS], sign_before[MAX_SIGNS];
	memcpy(chest_before, s->chest_pos, sizeof(chest_before));
	memcpy(sign_before, s->sign_pos, sizeof(sign_before));
	/* scramble: free the rows and dirty their payload, as a reset would */
	for(int i = 0; i < 4; i++) {
		te_store_free_row(&sa, TE_KIND_CHEST, ca[i]);
		fill_chest(s, ca[i], 9000);
	}
	for(int i = 0; i < 3; i++) {
		te_store_free_row(&sa, TE_KIND_SIGN, ga[i]);
		fill_sign(s, ga[i], 'z');
	}
	assert(reload(&sa, path, a->dimension));
	assert(memcmp(chest_before, s->chest_pos, sizeof(chest_before)) == 0
		   && memcmp(sign_before, s->sign_pos, sizeof(sign_before)) == 0
		   && "single world: the mapping is the identity");
	for(int i = 0; i < 4; i++)
		assert(te_store_row_owned(&sa, TE_KIND_CHEST, i)
			   && chest_matches(s, i, (uint16_t)(100 * (i + 1))));
	for(int i = 0; i < 3; i++)
		assert(te_store_row_owned(&sa, TE_KIND_SIGN, i)
			   && sign_matches(s, i, (char)(7 * i)));
	/* every non-owned row's payload is blank, like the old full-image wipe */
	assert(s->chest_items[4][0].id == 0 && s->sign_texts[3][0] == ' ');
	puts("PASS single world: identity layout, 54-slot payloads, sign bytes");

	/* ---- 2. slot 1 takes rows that COLLIDE with slot 0's archived indices
	 * AND coordinates; slot 0's rows are then released (its reset) and the
	 * reload must keep slot 1's rows and map slot 0's around them ---- */
	for(int i = 0; i < 4; i++)
		te_store_free_row(&sa, TE_KIND_CHEST, i);
	for(int i = 0; i < 3; i++)
		te_store_free_row(&sa, TE_KIND_SIGN, i);
	/* B claims rows 0 and 1 (first free), one at the SAME coordinates as A's
	 * archived chest 0, and sign row 1 */
	int cb0 = te_store_claim(&sb, TE_KIND_CHEST, 10, 64, 10, NULL);
	int cb1 = te_store_claim(&sb, TE_KIND_CHEST, 77, 64, 77, NULL);
	assert(cb0 == 0 && cb1 == 1);
	fill_chest(s, cb0, 5000);
	fill_chest(s, cb1, 6000);
	int gb0 = te_store_claim(&sb, TE_KIND_SIGN, 21, 65, 20, NULL);
	int gb1 = te_store_claim(&sb, TE_KIND_SIGN, 88, 65, 88, NULL);
	assert(gb0 == 0 && gb1 == 1);
	fill_sign(s, gb0, 'B');
	fill_sign(s, gb1, 'C');
	assert(reload(&sa, path, a->dimension));
	/* B's rows: same index, same bytes, still B's */
	assert(te_store_row_owned(&sb, TE_KIND_CHEST, 0)
		   && te_store_row_owned(&sb, TE_KIND_CHEST, 1)
		   && chest_matches(s, 0, 5000) && chest_matches(s, 1, 6000));
	assert(te_store_row_owned(&sb, TE_KIND_SIGN, 0)
		   && te_store_row_owned(&sb, TE_KIND_SIGN, 1) && sign_matches(s, 0, 'B')
		   && sign_matches(s, 1, 'C'));
	assert(!te_store_row_owned(&sa, TE_KIND_CHEST, 0)
		   && !te_store_row_owned(&sa, TE_KIND_CHEST, 1));
	/* A's four chests: rows 2,3 keep their original index; rows 0,1 (taken
	 * by B) land on the first free rows in source order: 4 then 5 */
	for(int i = 0; i < 4; i++) {
		int r = te_store_find(&sa, TE_KIND_CHEST, 10 + i, 64, 10);
		assert(r >= 0 && chest_matches(s, r, (uint16_t)(100 * (i + 1))));
		int expect = i >= 2 ? i : 4 + i;
		fprintf(stderr, "DBG chest %d -> row %d (expect %d)\n", i, r, expect);
		assert(r == expect && "original index if available, else first free");
	}
	/* A's own chest at (10,64,10) and B's at the same coordinates coexist:
	 * per-world lookup answers each world its own row */
	assert(te_store_find(&sb, TE_KIND_CHEST, 10, 64, 10) == 0);
	assert(te_store_find(&sa, TE_KIND_CHEST, 10, 64, 10) == 4);
	for(int i = 0; i < 3; i++) {
		int r = te_store_find(&sa, TE_KIND_SIGN, 20 + i, 65, 20);
		assert(r >= 0 && sign_matches(s, r, (char)(7 * i)));
		int expect = i == 2 ? 2 : 3 + i;
		assert(r == expect);
	}
	assert(te_store_count_owned(&sa, TE_KIND_CHEST) == 4
		   && te_store_count_owned(&sb, TE_KIND_CHEST) == 2);
	assert(te_store_count_owned(&sa, TE_KIND_SIGN) == 3
		   && te_store_count_owned(&sb, TE_KIND_SIGN) == 2);
	puts("PASS two worlds: slot 1 rows retained at their indices, slot 0 "
		 "remapped losslessly around them");

	/* ---- 3. a second reload of slot 0 (its rows now owned again) is
	 * idempotent: owned rows are replaceable, so the same layout results ---- */
	struct complex_block_pos again[MAX_CHESTS];
	memcpy(again, s->chest_pos, sizeof(again));
	assert(reload(&sa, path, a->dimension));
	assert(memcmp(again, s->chest_pos, sizeof(again)) == 0);
	assert(chest_matches(s, 0, 5000) && chest_matches(s, 4, 100));
	puts("PASS reload idempotent with owned rows replaceable");

	/* ---- 4. no chest/sign files at all: an EMPTY image releases slot 0's
	 * rows and installs nothing; slot 1 untouched (marked-empty replacement)
	 * ---- */
	{
		string_t empty_root;
		string_init_printf(empty_root, "%s/empty", split_path);
		MKDIR_COMPAT(string_get_cstr(empty_root));
		assert(reload(&sa, empty_root, a->dimension));
		assert(te_store_count_owned(&sa, TE_KIND_CHEST) == 0
			   && te_store_count_owned(&sa, TE_KIND_SIGN) == 0);
		assert(te_store_count_owned(&sb, TE_KIND_CHEST) == 2
			   && chest_matches(s, 0, 5000) && chest_matches(s, 1, 6000));
		string_clear(empty_root);
	}
	puts("PASS empty image: slot 0 emptied, slot 1 retained");

	/* ---- 5. R2-H1: slot 0 is RE-CREATED (new generation) and reloaded N
	 * times -- the rows of every previous generation of slot 0 are the new
	 * generation's own stale rows: released and replaced, never kept as
	 * foreign. Occupied counts and layout stay constant across trips. ---- */
	{
		struct te_store sa0 = server_world_tile_store(a);
		/* start from a clean slot-0 image of 4 chests + 3 signs on disk */
		for(int i = 0; i < 4; i++) {
			int r = te_store_claim(&sa0, TE_KIND_CHEST, 10 + i, 64, 10, NULL);
			assert(r >= 0);
			fill_chest(s, r, (uint16_t)(100 * (i + 1)));
		}
		for(int i = 0; i < 3; i++)
			assert(te_store_claim(&sa0, TE_KIND_SIGN, 20 + i, 65, 20, NULL) >= 0);
		assert(chest_archive_write_w(&sa0, path, a->dimension));
		assert(sign_archive_write_w(&sa0, path, a->dimension));
		int occupied_c = 0, occupied_s = 0;
		for(int i = 0; i < MAX_CHESTS; i++)
			occupied_c += s->chest_pos[i].y >= 0;
		for(int i = 0; i < MAX_SIGNS; i++)
			occupied_s += s->sign_pos[i].y >= 0;
		struct complex_block_pos layout[MAX_CHESTS];
		uint32_t gen_prev = a->generation;
		for(int trip = 0; trip < 4; trip++) {
			/* re-create slot 0: a new generation in the same slot */
			server_world_discard(a);
			string_t wp;
			string_init_set_str(wp, split_path);
			assert(server_world_create_with_populate_scratch(
				a, wp, WORLD_DIM_OVERWORLD, 12345, true,
				s->world_populate_scratch));
			string_clear(wp);
			a->local = s;
			a->session = s;
			a->world_index = 0;
			assert(a->generation != gen_prev);
			gen_prev = a->generation;
			struct te_store san = server_world_tile_store(a);
			assert(te_store_count_owned(&san, TE_KIND_CHEST) == 0
				   && "the new generation owns nothing yet");
			assert(reload(&san, path, a->dimension));
			int oc = 0, os = 0;
			for(int i = 0; i < MAX_CHESTS; i++)
				oc += s->chest_pos[i].y >= 0;
			for(int i = 0; i < MAX_SIGNS; i++)
				os += s->sign_pos[i].y >= 0;
			assert(oc == occupied_c && os == occupied_s
				   && "no accumulation: previous generations' rows released");
			assert(te_store_count_owned(&san, TE_KIND_CHEST) == 4
				   && te_store_count_owned(&san, TE_KIND_SIGN) == 3);
			assert(te_store_count_owned(&sb, TE_KIND_CHEST) == 2
				   && chest_matches(s, 0, 5000) && chest_matches(s, 1, 6000));
			if(trip == 0)
				memcpy(layout, s->chest_pos, sizeof(layout));
			else
				assert(memcmp(layout, s->chest_pos, sizeof(layout)) == 0
					   && "layout constant across trips");
		}
		printf("PASS R2-H1: 4 slot-0 re-creations, occupied stays %d chests / "
			   "%d signs\n",
			   occupied_c, occupied_s);
	}

	string_clear(path);
	server_world_discard(b);
	split_destroy(s);
	return 0;
}
#endif
