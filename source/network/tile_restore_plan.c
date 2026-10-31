/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S tile_restore_plan (astra-a2s-design-0926): prepare/preflight/commit
 * reload of the flat chest/sign archives -- see tile_entities_store.h. Kept
 * in its own dependency-light TU (te_store inline helpers + item_data only)
 * so the standalone archive tests that compile complex_block_archive.c can
 * link it without the whole store module. */
#include "tile_entities_store.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ===== A2-S tile_restore_plan (astra-a2s-design-0926) ==================== */

#ifdef CAVEWII_TILE_ENTITIES_STORE_TEST
static unsigned s_restore_fail_alloc_nth = 0;
static unsigned s_restore_alloc_count = 0;
void te_restore_test_fail_alloc(unsigned nth) {
	s_restore_fail_alloc_nth = nth;
	s_restore_alloc_count = 0;
}
#endif

void* te_restore_alloc(size_t n) {
#ifdef CAVEWII_TILE_ENTITIES_STORE_TEST
	if(s_restore_fail_alloc_nth
	   && ++s_restore_alloc_count == s_restore_fail_alloc_nth) {
		s_restore_fail_alloc_nth = 0;
		return NULL;
	}
#endif
	return calloc(1, n ? n : 1);
}

static void te_restore_clear_reservations(struct tile_restore_plan* plan) {
	const struct te_store* st = plan->st;
	if(!st)
		return;
	if(st->reserved[TE_KIND_CHEST] && plan->chest_dest)
		for(int i = 0; i < plan->chest_max; i++)
			if(plan->chest_dest[i] >= 0)
				st->reserved[TE_KIND_CHEST][plan->chest_dest[i]] = 0;
	if(st->reserved[TE_KIND_SIGN] && plan->sign_dest)
		for(int i = 0; i < plan->sign_max; i++)
			if(plan->sign_dest[i] >= 0)
				st->reserved[TE_KIND_SIGN][plan->sign_dest[i]] = 0;
}

void te_restore_abort(struct tile_restore_plan* plan) {
	if(!plan)
		return;
	if(plan->preflighted)
		te_restore_clear_reservations(plan);
	free(plan->chest_pos);
	free(plan->chest_items);
	free(plan->sign_pos);
	free(plan->sign_texts);
	free(plan->chest_dest);
	free(plan->sign_dest);
	memset(plan, 0, sizeof(*plan));
}

/* Row classes for one kind: replaceable (owned by the target view), free
 * (empty and unreserved), or foreign (owned by another world / reserved). */
static bool te_restore_row_usable(const struct te_store* st, enum te_kind kind,
								  const struct te_pos* p, int i) {
	if(te_live(st, kind, p, i) || te_same_slot_stale(st, kind, p, i))
		return true; /* replaceable: the target's own (or its slot's stale) row */
	return p[i].y < 0 && !te_reserved(st, kind, i);
}

static enum storage_rc te_restore_map_kind(const struct te_store* st,
										   enum te_kind kind,
										   const struct te_pos* incoming,
										   int incoming_max, int16_t* dest,
										   int* incoming_count) {
	int max;
	struct te_pos* p = te_kind_pos(st, kind, &max);
	if(!p)
		return STORAGE_IO_ERROR;
	if(incoming_max > max)
		incoming_max = max; /* the image is never larger than the table */
	/* IMP-TE-CAPS: the table can outgrow the flat image (which keeps its
	 * fixed MAX_CHESTS/MAX_SIGNS rows). A shortfall of usable rows grows the
	 * table first (new rows are free), so a second world's foreign rows can
	 * no longer refuse a reload. */
	int incoming_rows = 0, usable_rows = 0;
	for(int i = 0; i < incoming_max; i++)
		if(incoming[i].y >= 0)
			incoming_rows++;
	for(int i = 0; i < max && usable_rows < incoming_rows; i++)
		if(te_restore_row_usable(st, kind, p, i))
			usable_rows++;
	if(usable_rows < incoming_rows) {
		if(!te_store_grow_via(st, kind, max + (incoming_rows - usable_rows)))
			return STORAGE_CAPACITY;
		p = te_kind_pos(st, kind, &max);
	}
	/* dest[] is int16_t (the flat archive's plan layout): only the first
	 * INT16_MAX+1 rows are addressable as restore destinations. */
	int dest_rows = max < 32768 ? max : 32768;
	uint8_t* used = calloc((size_t)max, 1);
	if(!used)
		return STORAGE_IO_ERROR;
	/* Two passes, so "original index when available" holds for EVERY row:
	 * 1. every incoming row whose own index is usable keeps it (the single-
	 *    world identity mapping); 2. the displaced rows take the remaining
	 *    usable rows in ascending order, in source order. Duplicates are
	 *    ordinary rows here (each gets its own destination). */
	int n = 0;
	for(int i = 0; i < incoming_max; i++) {
		dest[i] = -1;
		if(incoming[i].y < 0)
			continue;
		n++;
		if(te_restore_row_usable(st, kind, p, i)) {
			used[i] = 1;
			dest[i] = (int16_t)i;
		}
	}
	int next = 0;
	for(int i = 0; i < incoming_max; i++) {
		if(incoming[i].y < 0 || dest[i] >= 0)
			continue;
		while(next < dest_rows
			  && (used[next] || !te_restore_row_usable(st, kind, p, next)))
			next++;
		if(next >= dest_rows) {
			free(used);
			return STORAGE_CAPACITY;
		}
		used[next] = 1;
		dest[i] = (int16_t)next;
	}
	free(used);
	*incoming_count = n;
	return STORAGE_OK;
}

enum storage_rc te_restore_preflight(const struct te_store* st,
									 struct tile_restore_plan* plan) {
	if(!st || !plan || !plan->prepared || plan->preflighted)
		return STORAGE_IO_ERROR;
	if(!st->owner_keyed || !st->chest_pos || !st->chest_items || !st->sign_pos
	   || !st->sign_texts)
		return STORAGE_IO_ERROR;
	plan->st = st;
	enum storage_rc rc = te_restore_map_kind(st, TE_KIND_CHEST, plan->chest_pos,
											 plan->chest_max, plan->chest_dest,
											 &plan->chest_incoming);
	if(rc == STORAGE_OK)
		rc = te_restore_map_kind(st, TE_KIND_SIGN, plan->sign_pos,
								 plan->sign_max, plan->sign_dest,
								 &plan->sign_incoming);
	if(rc != STORAGE_OK) {
		plan->st = NULL;
		return rc; /* nothing reserved, nothing mutated */
	}
	/* Reserve every destination so an intervening claim cannot take it. */
	if(st->reserved[TE_KIND_CHEST])
		for(int i = 0; i < plan->chest_max; i++)
			if(plan->chest_dest[i] >= 0)
				st->reserved[TE_KIND_CHEST][plan->chest_dest[i]] = 1;
	if(st->reserved[TE_KIND_SIGN])
		for(int i = 0; i < plan->sign_max; i++)
			if(plan->sign_dest[i] >= 0)
				st->reserved[TE_KIND_SIGN][plan->sign_dest[i]] = 1;
	plan->preflighted = true;
	return STORAGE_OK;
}

static bool te_restore_revalidate_kind(const struct te_store* st,
									   enum te_kind kind, const int16_t* dest,
									   int incoming_max) {
	int max;
	struct te_pos* p = te_kind_pos(st, kind, &max);
	for(int i = 0; i < incoming_max; i++) {
		int d = dest[i];
		if(d < 0)
			continue;
		if(d >= max)
			return false;
		/* still the target's own (or its slot's stale) row, or still free (a
		 * reserved free row cannot have been claimed by anyone else) */
		if(!te_live(st, kind, p, d) && !te_same_slot_stale(st, kind, p, d)
		   && p[d].y >= 0)
			return false;
	}
	return true;
}

bool te_restore_commit(struct tile_restore_plan* plan) {
	if(!plan || !plan->prepared || !plan->preflighted || !plan->st) {
		te_restore_abort(plan);
		return false;
	}
	const struct te_store* st = plan->st;
	if(!te_restore_revalidate_kind(st, TE_KIND_CHEST, plan->chest_dest,
								   plan->chest_max)
	   || !te_restore_revalidate_kind(st, TE_KIND_SIGN, plan->sign_dest,
									  plan->sign_max)) {
		te_restore_abort(plan);
		return false;
	}
	/* ---- allocation-free commit ----
	 * 1. release every row this view owns and blank every NON-FOREIGN row's
	 *    payload (the pre-A2-S full-image read wiped the whole table; foreign
	 *    rows are the only bytes kept). */
	int cmax, smax;
	struct te_pos* cp = te_kind_pos(st, TE_KIND_CHEST, &cmax);
	struct te_pos* sp = te_kind_pos(st, TE_KIND_SIGN, &smax);
	for(int i = 0; i < cmax; i++) {
		bool foreign = cp[i].y >= 0 && !te_live(st, TE_KIND_CHEST, cp, i)
			&& !te_same_slot_stale(st, TE_KIND_CHEST, cp, i);
		if(foreign)
			continue;
		if(cp[i].y >= 0)
			te_release_row(st, TE_KIND_CHEST, cp, i);
		memset(st->chest_items + (size_t)i * st->chest_slots, 0,
			   (size_t)st->chest_slots * sizeof(struct item_data));
	}
	for(int i = 0; i < smax; i++) {
		bool foreign = sp[i].y >= 0 && !te_live(st, TE_KIND_SIGN, sp, i)
			&& !te_same_slot_stale(st, TE_KIND_SIGN, sp, i);
		if(foreign)
			continue;
		if(sp[i].y >= 0)
			te_release_row(st, TE_KIND_SIGN, sp, i);
		memset(st->sign_texts + (size_t)i * st->sign_size, ' ',
			   (size_t)st->sign_size);
	}
	/* 2. install the mapped rows, stamped as the target's. */
	int copy_slots = plan->chest_slots < st->chest_slots ? plan->chest_slots
														 : st->chest_slots;
	for(int i = 0; i < plan->chest_max; i++) {
		int d = plan->chest_dest[i];
		if(d < 0)
			continue;
		cp[d] = plan->chest_pos[i];
		memcpy(st->chest_items + (size_t)d * st->chest_slots,
			   plan->chest_items + (size_t)i * plan->chest_slots,
			   (size_t)copy_slots * sizeof(struct item_data));
		te_stamp(st, TE_KIND_CHEST, d);
	}
	int copy_text = plan->sign_size < st->sign_size ? plan->sign_size
													: st->sign_size;
	for(int i = 0; i < plan->sign_max; i++) {
		int d = plan->sign_dest[i];
		if(d < 0)
			continue;
		sp[d] = plan->sign_pos[i];
		memcpy(st->sign_texts + (size_t)d * st->sign_size,
			   plan->sign_texts + (size_t)i * plan->sign_size,
			   (size_t)copy_text);
		te_stamp(st, TE_KIND_SIGN, d);
	}
	te_restore_abort(plan); /* clears reservations, frees scratch */
	return true;
}
