/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
/* Parked 2026-10-25 (maintainer): not shown in release; revive as a documented addition. */
#include <string.h>

#include "inventory_sort.h"
#include "items.h"

static bool identity(struct item_data a, struct item_data b) {
	return a.id == b.id && a.durability == b.durability;
}

/* 740c583 ItemStack.areItemStacksEqual/isItemStackEqual: null/null or
 * exact id, count AND damage. Do not use vanilla's broader merge predicate. */
static bool equal(struct item_data a, struct item_data b) {
	return a.id == b.id && (!a.id || (identity(a, b) && a.count == b.count));
}

/* Plan 2026-09-06, agreed design 3: id, damage/variant, count DESC.
 * Stable insertion below preserves original order for equal keys (wear is
 * durability, never discarded). LOCKED_DESIGN "Chests and sorting" owns
 * the four operations/scope; its frozen preview used an older name key. */
static bool before(struct item_data a, struct item_data b) {
	if(!a.id || !b.id) return a.id && !b.id;
	if(a.id != b.id) return a.id < b.id;
	if(a.durability != b.durability) return a.durability < b.durability;
	return a.count > b.count;
}

static bool click(struct inventory_sort_plan* p, enum window_type type,
	size_t slot) {
	if(p->count == INVENTORY_SORT_MAX_CLICKS) return false;
	struct inventory_sort_click* c = &p->clicks[p->count++];
	c->slot = slot;
	c->right = false;
	c->expected = inventory_click_predict(&p->simulation, type, slot, false, false);
	c->slot_after = p->working[slot];
	c->cursor_after = p->simulation.picked_item;
	return true;
}

static bool exchange(struct inventory_sort_plan* p, enum window_type type,
	size_t a, size_t b, size_t n) {
	struct item_data old_a = p->working[a], old_b = p->working[b];
	size_t start = p->count;
	if(!click(p, type, b) || !click(p, type, a)) return false;
	if(p->simulation.picked_item.id && !click(p, type, b)) return false;
	if(equal(p->working[a], old_b) && equal(p->working[b], old_a)
	   && !p->simulation.picked_item.id) return true;
	/* Probe uses the SAME simulator, not a duplicate SWAP predicate. A
 * failed probe is private: restore it before trying the empty-slot route. */
	p->working[a] = old_a;
	p->working[b] = old_b;
	p->simulation.picked_item = (struct item_data){0};
	p->count = start;
	for(size_t k = 0; k < n; k++) {
		size_t spare = p->movable[k];
		if(spare == a || spare == b || p->working[spare].id) continue;
		return click(p, type, a) && click(p, type, spare)
			&& click(p, type, b) && click(p, type, a)
			&& click(p, type, spare) && click(p, type, b);
	}
	return false;
}

static enum inventory_sort_result refuse(struct inventory_sort_plan* p,
	enum inventory_sort_result result) {
	/* No partial trace can accidentally be executed after a refusal. */
	memset(p, 0, sizeof(*p));
	return result;
}

enum inventory_sort_result inventory_sort_build(
	const struct inventory_sort_input* in, struct inventory_sort_plan* p) {
	if(!p) return INVENTORY_SORT_INVALID;
	memset(p, 0, sizeof(*p));
	if(!in || !in->slots || !in->max_stack || !in->pinned)
		return INVENTORY_SORT_INVALID;
	if(in->snapshot_generation != in->current_generation) return INVENTORY_SORT_STALE;
	if(in->cursor.id) return INVENTORY_SORT_HELD;
	bool chest = in->operation == INVENTORY_SORT_CHEST_GROUP
		|| in->operation == INVENTORY_SORT_CHEST_STACK;
	bool group = in->operation == INVENTORY_SORT_CHEST_GROUP
		|| in->operation == INVENTORY_SORT_MAIN_GROUP;
	if(in->operation < INVENTORY_SORT_CHEST_GROUP
	   || in->operation > INVENTORY_SORT_MAIN_STACK) return INVENTORY_SORT_INVALID;
	/* LOCKED_DESIGN Chests and sorting; plan items 6/9; 740c583
 * ContainerChest constructor: storage N, main N..N+26, hotbar N+27..N+35.
 * Player ContainerPlayer main 9..35. Never accept a caller's arbitrary
 * range that could accidentally include armor, outputs or the hotbar. */
	size_t main;
	if(in->window == WINDOW_TYPE_CHEST && (in->capacity == 63 || in->capacity == 90))
		main = in->capacity - 36;
	else if(in->window == WINDOW_TYPE_INVENTORY && in->capacity == 45 && !chest)
		main = 9;
	else return INVENTORY_SORT_INVALID;
	p->begin = chest ? 0 : main;
	p->end = chest ? main : main + 27;
	p->generation = in->snapshot_generation;
	memcpy(p->target, in->slots, in->capacity * sizeof(*in->slots));
	memcpy(p->working, in->slots, in->capacity * sizeof(*in->slots));
	p->simulation.items = p->working;
	p->simulation.capacity = in->capacity;
	size_t n = 0;
	for(size_t k = p->begin; k < p->end; k++) {
		if(in->pinned[k]) continue;
		struct item_data s = in->slots[k];
		struct item* def = item_get(&s);
		if(s.id && (!def || !def->max_stack || in->max_stack[k] != def->max_stack
			|| !s.count || s.count > def->max_stack))
			return refuse(p, INVENTORY_SORT_INVALID);
		p->movable[n++] = k;
	}
	/* Consolidate only exact identities, earlier slots first. Vanilla
 * 740c583 Container.mergeItemStack: same id and (!hasSubtypes || same
 * damage), subject to isStackable/maxStack. Ordinary slotClick is broader
 * still. We intentionally NEVER merge different damage: that would erase
 * imported metadata despite being a legal vanilla click. */
	for(size_t a = 0; a < n; a++) {
		size_t dest = p->movable[a];
		if(!p->working[dest].id) continue;
		unsigned limit = in->max_stack[dest];
		for(size_t b = a + 1; b < n && p->working[dest].count < limit; b++) {
			size_t source = p->movable[b];
			if(!p->working[source].id || !identity(p->working[dest], p->working[source])) continue;
			if(!click(p, in->window, source) || !click(p, in->window, dest)
			   || (p->simulation.picked_item.id && !click(p, in->window, source)))
				return refuse(p, INVENTORY_SORT_NEEDS_SPACE);
		}
	}
	/* Stable compaction for stack-only, stable insertion sort for group. */
	size_t filled = 0;
	for(size_t k = 0; k < n; k++) {
		struct item_data s = p->working[p->movable[k]];
		if(!s.id) continue;
		size_t at = filled++;
		while(group && at && before(s, p->target[p->movable[at - 1]])) {
			p->target[p->movable[at]] = p->target[p->movable[at - 1]];
			at--;
		}
		p->target[p->movable[at]] = s;
	}
	for(size_t k = filled; k < n; k++) p->target[p->movable[k]] = (struct item_data){0};
	for(size_t a = 0; a < n; a++) {
		size_t dest = p->movable[a];
		if(equal(p->working[dest], p->target[dest])) continue;
		size_t b = a + 1;
		while(b < n && !equal(p->working[p->movable[b]], p->target[dest])) b++;
		if(b == n || !exchange(p, in->window, dest, p->movable[b], n))
			return refuse(p, INVENTORY_SORT_NEEDS_SPACE);
	}
	/* Final dry-run gate: target, empty cursor, untouched outside scope and
 * pinned records, plus conserved totals for EVERY initial identity. No IO. */
	if(p->simulation.picked_item.id) return refuse(p, INVENTORY_SORT_NEEDS_SPACE);
	for(size_t k = 0; k < in->capacity; k++) {
		if(!equal(p->working[k], p->target[k])) return refuse(p, INVENTORY_SORT_NEEDS_SPACE);
		if(k < p->begin || k >= p->end || in->pinned[k]) {
			if(memcmp(&p->working[k], &in->slots[k], sizeof(in->slots[k])))
				return refuse(p, INVENTORY_SORT_NEEDS_SPACE);
			continue;
		}
		if(!in->slots[k].id) continue;
		unsigned before_count = 0, after_count = 0;
		for(size_t j = p->begin; j < p->end; j++) {
			if(identity(in->slots[j], in->slots[k])) before_count += in->slots[j].count;
			if(identity(p->working[j], in->slots[k])) after_count += p->working[j].count;
		}
		if(before_count != after_count) return refuse(p, INVENTORY_SORT_NEEDS_SPACE);
	}
	return p->count ? INVENTORY_SORT_READY : INVENTORY_SORT_UNCHANGED;
}
