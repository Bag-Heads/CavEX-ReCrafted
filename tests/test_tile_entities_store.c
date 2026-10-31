/*
	Unit test for the chest/sign table <-> per-chunk bridge
	(source/network/tile_entities_store.c) -- Stage 2 of the chest-store
	residency redesign (docs/chest_store_redesign.md).

	Drives the three lifecycle helpers on a plain te_store view (no server_local),
	covering the parts most likely to corrupt data:
	  - the block->chunk filter, including negative coordinates (floor division),
	  - overwrite-by-position de-dup on insert (the NBT-wins / chests.dat-loses
	    rule that keeps a chest from doubling), and
	  - free-on-evict (release frees exactly one chunk's slots).
	The te <-> NBT serialization itself is covered by test_tile_entities_nbt.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks_data.h"
#include "network/tile_entities_nbt.h"
#include "network/tile_entities_store.h"
#include "world.h"

#define NC 4
#define NS 2
#define NF 3
#define NN 2
#define NRP 2
#define NSP 1 // FH-17
#define NP 4
#define SLOTS TE_CHEST_SLOTS
#define SSIZE TE_SIGN_SIZE

#define HOST_INDEX(X, Y, Z) \
	((size_t)(Y) + ((size_t)(Z) + (size_t)(X) * 16u) * WORLD_HEIGHT)

int main(void) {
	/* Beta Chunk.setChunkBlockTileEntity accepts a restored TE only when its
	 * voxel is a BlockContainer. A stale Piston record beside an ordinary block
	 * must be removed before capacity preflight/insert, while valid records keep
	 * their NBT order. [740c583 Chunk.setChunkBlockTileEntity] */
	static uint8_t host_ids[16 * 16 * WORLD_HEIGHT];
	struct te_piston_moving restored_pm[3] = {
		{.x = 1, .y = 40, .z = 2, .stored_id = 35, .facing = 5},
		{.x = 3, .y = 41, .z = 4, .stored_id = 1, .facing = 5},
		{.x = 5, .y = 42, .z = 6, .stored_id = 4, .facing = 5},
	};
	host_ids[HOST_INDEX(1, 40, 2)] = BLOCK_PISTON_MOVING;
	host_ids[HOST_INDEX(3, 41, 4)] = BLOCK_STONE; /* stale sidecar */
	host_ids[HOST_INDEX(5, 42, 6)] = BLOCK_PISTON_MOVING;
	struct chunk_tile_entities restored = {
		.piston_movings = restored_pm,
		.n_piston_moving = 3,
	};
	size_t stale_pistons = 0;
	assert(te_store_filter_piston_hosts(
		&restored, 0, 0, host_ids, &stale_pistons));
	assert(stale_pistons == 1 && restored.n_piston_moving == 2);
	assert(restored.piston_movings[0].x == 1);
	assert(restored.piston_movings[1].x == 5);
	memset(host_ids, 0, sizeof(host_ids));
	struct te_piston_moving negative_pm[2] = {
		{.x = -17, .y = 50, .z = -1, .stored_id = 35, .facing = 4},
		{.x = -18, .y = 51, .z = -2, .stored_id = 1, .facing = 4},
	};
	host_ids[HOST_INDEX(15, 50, 15)] = BLOCK_PISTON_MOVING;
	host_ids[HOST_INDEX(14, 51, 14)] = BLOCK_DIRT; /* stale sidecar */
	restored = (struct chunk_tile_entities) {
		.piston_movings = negative_pm,
		.n_piston_moving = 2,
	};
	assert(te_store_filter_piston_hosts(
		&restored, -2, -1, host_ids, &stale_pistons));
	assert(stale_pistons == 1 && restored.n_piston_moving == 1);
	assert(restored.piston_movings[0].x == -17
		   && restored.piston_movings[0].z == -1);

	struct te_pos cpos[NC];
	struct item_data citems[NC][SLOTS];
	struct te_pos spos[NS];
	char stext[NS][SSIZE];
	struct te_pos fpos[NF];
	struct furnace_te ftab[NF];
	struct te_pos npos[NN];
	int8_t note_vals[NN];
	bool note_prev_powered[NN];
	struct te_pos rppos[NRP];
	int32_t record_vals[NRP];
	struct te_pos sppos[NSP]; // FH-17
	struct spawner_te sptab[NSP];
	struct piston_moving_store pistons;

	memset(citems, 0, sizeof(citems));
	for(int i = 0; i < NC; i++)
		cpos[i].y = -1; // empty
	for(int i = 0; i < NS; i++)
		spos[i].y = -1;
	memset(stext, ' ', sizeof(stext));
	for(int i = 0; i < NF; i++)
		fpos[i].y = -1;
	memset(ftab, 0, sizeof(ftab));
	for(int i = 0; i < NN; i++)
		npos[i].y = -1;
	memset(note_vals, 0, sizeof(note_vals));
	// Pre-seed TRUE so the insert test below can prove the live sidecar is
	// unconditionally CLEARED on insert (Beta TileEntityNote.
	// previousRedstoneState -- never persisted, always false on a fresh
	// construction, which chunk load always is).
	for(int i = 0; i < NN; i++)
		note_prev_powered[i] = true;
	for(int i = 0; i < NRP; i++)
		rppos[i].y = -1;
	memset(record_vals, 0, sizeof(record_vals));
	for(int i = 0; i < NSP; i++)
		sppos[i].y = -1;
	memset(sptab, 0, sizeof(sptab));
	piston_moving_store_init(&pistons);
	/* A notifying outer piston move may synchronously run an opposite-kind inner
	 * move before publishing its own entries. Claims must compose instead of the
	 * inner move stealing the outer move's merely-free capacity. */
	{
		struct piston_moving_store_reservation outer = {0}, inner = {0};
		struct piston_moving_state moving = {
			.stored_id = BLOCK_STONE,
			.facing = 5,
			.flags = PISTON_MOVING_FLAG_EXTENDING,
		};
		assert(piston_moving_store_reservation_begin(&pistons, 13, &outer));
		assert(piston_moving_store_reservation_begin(&pistons, 13, &inner));
		for(int i = 0; i < 13; i++)
			assert(piston_moving_store_upsert_claimed(
				&pistons, &inner, i, 40, 0, &moving, NULL));
		for(int i = 0; i < 13; i++)
			assert(piston_moving_store_upsert_claimed(
				&pistons, &outer, i, 41, 0, &moving, NULL));
		assert(inner.remaining == 0 && outer.remaining == 0
			   && pistons.reserved == 0 && pistons.count == 26);
		piston_moving_store_clear(&pistons);
	}

	struct te_store st = {
		.chest_pos = cpos,
		.chest_items = &citems[0][0],
		.max_chest = NC,
		.chest_slots = SLOTS,
		.sign_pos = spos,
		.sign_texts = &stext[0][0],
		.max_sign = NS,
		.sign_size = SSIZE,
		.furnace_pos = fpos,
		.furnaces = ftab,
		.max_furnace = NF,
		.note_pos = npos,
		.notes = note_vals,
		.note_prev_powered = note_prev_powered,
		.max_note = NN,
		.record_player_pos = rppos,
		.records = record_vals,
		.max_record_player = NRP,
		.spawner_pos = sppos,
		.spawners = sptab,
		.max_spawner = NSP,
		.piston_moving = &pistons,
	};

	// --- insert a parsed chunk's chests/signs into free slots ---
	struct te_chest ch[2];
	memset(ch, 0, sizeof(ch));
	ch[0] = (struct te_chest) {.x = 3, .y = 70, .z = 5}; // chunk (0,0)
	ch[0].items[0] = (struct item_data) {.id = 265, .count = 7};
	ch[1] = (struct te_chest) {.x = -20, .y = 12, .z = 40}; // chunk (-2,2)
	ch[1].items[53] = (struct item_data) {.id = 351, .count = 1, .durability = 3};
	struct te_sign sg[1];
	memset(sg, 0, sizeof(sg));
	sg[0] = (struct te_sign) {.x = 3, .y = 64, .z = 6}; // chunk (0,0)
	strcpy(sg[0].text, "line one");

	struct te_furnace fr[1];
	memset(fr, 0, sizeof(fr));
	fr[0] = (struct te_furnace) {.x = 4, .y = 66, .z = 7}; // chunk (0,0)
	fr[0].items[0] = (struct item_data) {.id = 15, .count = 9};	// input
	fr[0].items[1] = (struct item_data) {.id = 263, .count = 4}; // fuel
	fr[0].items[2] = (struct item_data) {.id = 265, .count = 2}; // output
	fr[0].burn_time = 222;
	fr[0].cook_time = 50;
	struct te_piston_moving pm[2] = {
		{.x = 5, .y = 67, .z = 7, .stored_id = 35, .stored_meta = 13,
		 .facing = 5, .progress = 0.25F, .extending = 1},
		{.x = 6, .y = 67, .z = 7, .stored_id = 1, .stored_meta = 0,
		 .facing = 5, .progress = 0.75F, .extending = 0},
	};
	struct te_note nt[1] = {{.x = 7, .y = 68, .z = 8, .note = 19}}; // chunk (0,0)
	struct te_record_player rp[1]
		= {{.x = 8, .y = 69, .z = 9, .record = 2256}}; // chunk (0,0)
	struct te_spawner sp[1]; // FH-17
	memset(sp, 0, sizeof(sp));
	sp[0].x = 9;
	sp[0].y = 70;
	sp[0].z = 10; // chunk (0,0)
	strcpy(sp[0].mob_id, "Spider");
	sp[0].delay = 88;

	struct chunk_tile_entities in = {
		.chests = ch,
		.n_chest = 2,
		.signs = sg,
		.n_sign = 1,
		.furnaces = fr,
		.n_furnace = 1,
		.notes = nt,
		.n_note = 1,
		.record_players = rp,
		.n_record_player = 1,
		.spawners = sp,
		.n_spawner = 1,
		.piston_movings = pm,
		.n_piston_moving = 2,
	};
	assert(te_store_insert(&st, &in));

	int fn = -1;
	for(int i = 0; i < NN; i++)
		if(npos[i].x == 7 && npos[i].y == 68 && npos[i].z == 8)
			fn = i;
	assert(fn >= 0);
	assert(note_vals[fn] == 19);
	// D-discipline: the live-only previousRedstoneState sidecar is ALWAYS
	// cleared on insert, even though it was pre-seeded true above -- a fresh
	// TileEntityNote() construction (which chunk load always is) starts false.
	assert(note_prev_powered[fn] == false);

	int frp = -1;
	for(int i = 0; i < NRP; i++)
		if(rppos[i].x == 8 && rppos[i].y == 69 && rppos[i].z == 9)
			frp = i;
	assert(frp >= 0);
	assert(record_vals[frp] == 2256);

	int fsp = -1; // FH-17
	for(int i = 0; i < NSP; i++)
		if(sppos[i].x == 9 && sppos[i].y == 70 && sppos[i].z == 10)
			fsp = i;
	assert(fsp >= 0);
	assert(strcmp(sptab[fsp].mob_id, "Spider") == 0);
	assert(sptab[fsp].delay == 88);

	int found0 = -1, found1 = -1;
	for(int i = 0; i < NC; i++) {
		if(cpos[i].x == 3 && cpos[i].y == 70 && cpos[i].z == 5)
			found0 = i;
		if(cpos[i].x == -20 && cpos[i].y == 12 && cpos[i].z == 40)
			found1 = i;
	}
	assert(found0 >= 0 && found1 >= 0);
	assert(citems[found0][0].id == 265 && citems[found0][0].count == 7);
	assert(citems[found1][53].id == 351 && citems[found1][53].durability == 3);

	int fs = -1;
	for(int i = 0; i < NS; i++)
		if(spos[i].x == 3 && spos[i].y == 64 && spos[i].z == 6)
			fs = i;
	assert(fs >= 0);
	assert(memcmp(&stext[fs][0], "line one", 8) == 0);
	// space-padded, not nul-padded (the server's buffer convention)
	assert(stext[fs][8] == ' ' && stext[fs][SSIZE - 1] == ' ');

	int ff = -1;
	for(int i = 0; i < NF; i++)
		if(fpos[i].x == 4 && fpos[i].y == 66 && fpos[i].z == 7)
			ff = i;
	assert(ff >= 0);
	assert(ftab[ff].burn_time == 222 && ftab[ff].cook_time == 50);
	assert(ftab[ff].items[0].id == 15 && ftab[ff].items[1].id == 263
		   && ftab[ff].items[2].count == 2);
	// current_item_burn_time is NOT restored by the store (server recomputes it).
	assert(ftab[ff].current_item_burn_time == 0);
	size_t pfirst, psecond;
	assert(piston_moving_store_find(&pistons, 5, 67, 7, &pfirst));
	assert(piston_moving_store_find(&pistons, 6, 67, 7, &psecond));
	// readFromNBT restores BOTH floats verbatim, including fractions a countdown cannot represent.
	assert(pistons.entries[pfirst].state.current_progress == 0.25F
		   && pistons.entries[pfirst].state.last_progress == 0.25F);
	assert(pistons.entries[psecond].state.current_progress == 0.75F
		   && pistons.entries[psecond].state.last_progress == 0.75F);
	assert(pistons.entries[pfirst].order < pistons.entries[psecond].order);

	// Reuse the lower first slot, then prove gather follows deterministic runtime order rather than slot.
	assert(piston_moving_store_remove_index(&pistons, pfirst));
	struct te_piston_moving pm3 = {
		.x = 7, .y = 67, .z = 7, .stored_id = 35, .stored_meta = 3,
		.facing = 5, .progress = 0.5F, .extending = 1,
	};
	struct chunk_tile_entities pin3 = {.piston_movings = &pm3, .n_piston_moving = 1};
	assert(te_store_insert(&st, &pin3));

	// Capacity failure is all-or-nothing across every TE type. Three new chest
	// positions cannot fit the two remaining slots, so the sign overwrite must
	// not be applied either.
	struct te_chest overflow_chests[3] = {
		{.x = 8, .y = 70, .z = 8},
		{.x = 9, .y = 70, .z = 8},
		{.x = 10, .y = 70, .z = 8},
	};
	struct te_sign overflow_sign = {.x = 3, .y = 64, .z = 6};
	strcpy(overflow_sign.text, "must not apply");
	struct chunk_tile_entities overflow = {
		.chests = overflow_chests,
		.n_chest = 3,
		.signs = &overflow_sign,
		.n_sign = 1,
	};
	assert(!te_store_insert(&st, &overflow));
	assert(memcmp(&stext[fs][0], "line one", 8) == 0);
	int occupied_chests = 0;
	for(int i = 0; i < NC; i++)
		if(cpos[i].y >= 0)
			occupied_chests++;
	assert(occupied_chests == 2);
	struct chunk_tile_entities invalid = {.n_chest = -1};
	assert(!te_store_can_insert(&st, &invalid));
	invalid.n_chest = 1;
	assert(!te_store_can_insert(&st, &invalid)); // positive count, NULL array

	/* A marked chunk is a full replacement for flat-backed chest/sign rows in
	 * that chunk. Even a full table must admit new NBT positions by reclaiming
	 * target rows, while preserving rows in every other chunk. */
	{
		struct te_pos rcpos[2] = {{1, 70, 1}, {33, 70, 1}};
		struct item_data rcitems[2][SLOTS] = {{{0}}};
		struct te_pos rspos[2] = {{2, 70, 2}, {34, 70, 2}};
		char rstext[2][SSIZE];
		memset(rstext, ' ', sizeof(rstext));
		rcitems[0][0] = (struct item_data) {.id = 1, .count = 1};
		rcitems[1][0] = (struct item_data) {.id = 2, .count = 1};
		struct te_store replace_store = {
			.chest_pos = rcpos,
			.chest_items = &rcitems[0][0],
			.max_chest = 2,
			.chest_slots = SLOTS,
			.sign_pos = rspos,
			.sign_texts = &rstext[0][0],
			.max_sign = 2,
			.sign_size = SSIZE,
		};
		struct te_chest replacement_chest = {.x = 3, .y = 71, .z = 3};
		replacement_chest.items[0]
			= (struct item_data) {.id = 3, .count = 1};
		struct te_sign replacement_sign = {.x = 4, .y = 71, .z = 4};
		strcpy(replacement_sign.text, "marked");
		struct chunk_tile_entities replacement = {
			.legacy_flat_complete = true,
			.chests = &replacement_chest,
			.n_chest = 1,
			.signs = &replacement_sign,
			.n_sign = 1,
		};
		assert(!te_store_can_insert(&replace_store, &replacement));
		assert(te_store_can_replace_legacy_chunk(
			&replace_store, &replacement, 0, 0));
		te_store_release_legacy_chunk(&replace_store, 0, 0);
		assert(rcpos[0].y < 0 && rspos[0].y < 0);
		assert(rcpos[1].x == 33 && rcpos[1].y == 70);
		assert(rspos[1].x == 34 && rspos[1].y == 70);
		assert(te_store_insert(&replace_store, &replacement));
		assert(rcpos[0].x == 3 && rcpos[0].y == 71
			   && rcitems[0][0].id == 3);
		assert(rspos[0].x == 4 && rspos[0].y == 71
			   && memcmp(rstext[0], "marked", 6) == 0);
	}

	// --- overwrite-by-position: re-insert the (3,70,5) chest, new contents ---
	struct te_chest ch2[1];
	memset(ch2, 0, sizeof(ch2));
	ch2[0] = (struct te_chest) {.x = 3, .y = 70, .z = 5};
	ch2[0].items[1] = (struct item_data) {.id = 999, .count = 2};
	struct chunk_tile_entities in2 = {.chests = ch2, .n_chest = 1};
	assert(te_store_insert(&st, &in2));

	int cnt = 0, slot = -1;
	for(int i = 0; i < NC; i++)
		if(cpos[i].x == 3 && cpos[i].y == 70 && cpos[i].z == 5) {
			cnt++;
			slot = i;
		}
	assert(cnt == 1); // replaced in place, not duplicated
	assert(citems[slot][1].id == 999 && citems[slot][0].id == 0); // replaced, not merged

	// --- gather chunk (0,0): its chest + sign + furnace, NOT the (-20,..) chest ---
	struct chunk_tile_entities out = {0};
	assert(te_store_gather(&st, 0, 0, &out));
	assert(out.legacy_flat_complete);
	assert(out.n_chest == 1 && out.n_sign == 1 && out.n_furnace == 1
		   && out.n_note == 1 && out.n_record_player == 1
		   && out.n_spawner == 1 && out.n_piston_moving == 2);
	assert(out.chests[0].x == 3 && out.chests[0].items[1].id == 999);
	assert(out.signs[0].x == 3);
	assert(strncmp(out.signs[0].text, "line one", 8) == 0);
	assert(out.signs[0].text[8] == ' '); // padding round-trips through gather
	assert(out.furnaces[0].x == 4 && out.furnaces[0].y == 66
		   && out.furnaces[0].z == 7);
	assert(out.furnaces[0].burn_time == 222 && out.furnaces[0].cook_time == 50);
	assert(out.furnaces[0].items[0].id == 15 && out.furnaces[0].items[2].count == 2);
	assert(out.notes[0].x == 7 && out.notes[0].y == 68 && out.notes[0].z == 8);
	assert(out.notes[0].note == 19);
	assert(out.record_players[0].x == 8 && out.record_players[0].y == 69
		   && out.record_players[0].z == 9);
	assert(out.record_players[0].record == 2256);
	assert(out.spawners[0].x == 9 && out.spawners[0].y == 70
		   && out.spawners[0].z == 10);
	assert(strcmp(out.spawners[0].mob_id, "Spider") == 0);
	assert(out.spawners[0].delay == 88);
	assert(out.piston_movings[0].x == 6 && out.piston_movings[0].progress == 0.75F);
	assert(out.piston_movings[1].x == 7 && out.piston_movings[1].progress == 0.5F);
	tile_entities_parse_free(&out);

	// gather chunk (-2,2): only the (-20,12,40) chest (negative-coord filter)
	struct chunk_tile_entities out2 = {0};
	assert(te_store_gather(&st, -2, 2, &out2));
	assert(out2.n_chest == 1 && out2.n_sign == 0);
	assert(out2.chests[0].x == -20 && out2.chests[0].items[53].id == 351);
	tile_entities_parse_free(&out2);

	// gather an empty chunk: nothing
	struct chunk_tile_entities out3 = {0};
	assert(te_store_gather(&st, 99, 99, &out3));
	assert(out3.legacy_flat_complete);
	assert(out3.n_chest == 0 && out3.n_sign == 0 && !out3.chests && !out3.signs);
	tile_entities_parse_free(&out3);

	// A later allocation failure must free earlier arrays and report failure;
	// returning a partial/empty gather would overwrite live tile entities.
	struct chunk_tile_entities failed = {0};
	te_store_test_fail_allocation_after(1); // chest succeeds, sign fails
	assert(!te_store_gather(&st, 0, 0, &failed));
	assert(!failed.chests && !failed.signs && !failed.furnaces
		   && !failed.piston_movings);
	assert(failed.n_chest == 0 && failed.n_sign == 0 && failed.n_furnace == 0
		   && failed.n_piston_moving == 0);
	assert(!failed.legacy_flat_complete);

	// --- release chunk (0,0): frees its slots, leaves chunk (-2,2) intact ---
	te_store_release(&st, 0, 0);
	int rem = 0, rems = 0, remf = 0, remn = 0, remrp = 0, remsp = 0;
	for(int i = 0; i < NC; i++)
		if(cpos[i].y >= 0)
			rem++;
	for(int i = 0; i < NS; i++)
		if(spos[i].y >= 0)
			rems++;
	for(int i = 0; i < NF; i++)
		if(fpos[i].y >= 0)
			remf++;
	for(int i = 0; i < NN; i++)
		if(npos[i].y >= 0)
			remn++;
	for(int i = 0; i < NRP; i++)
		if(rppos[i].y >= 0)
			remrp++;
	for(int i = 0; i < NSP; i++)
		if(sppos[i].y >= 0)
			remsp++;
	assert(rem == 1 && rems == 0 && remf == 0 && remn == 0 && remrp == 0
		   && remsp == 0 && pistons.count == 0);
	assert(cpos[found1].x == -20 && cpos[found1].y == 12); // untouched
	piston_moving_store_destroy(&pistons);

	printf("test_tile_entities_store: insert/overwrite/gather/release OK "
		   "(chunk filter incl. negatives, position de-dup, free-on-evict; "
		   "chests+signs+furnaces+notes+record players+spawners+piston moving "
		   "order/progress; note_prev_powered cleared on insert)\n");
	return 0;
}
