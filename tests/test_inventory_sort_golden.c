/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Click expectations: 740c583 Container.slotClick and ItemStack equality.
 * Target key: chest_sort_plan_2026-09-06.md item 3; scope: LOCKED_DESIGN.
 * Fixture comes from the frozen chest.html reset(), with a hand-built trace. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "item/items.h"
#include "item/inventory_sort.h"

static struct inventory_sort_plan plan;
static struct item_data slots[90], replay[90];
static uint8_t limits[90];
static bool pinned[90];
static struct inventory_sort_input input;

static struct item_data stack(unsigned id, unsigned count, unsigned damage) {
	return (struct item_data){.id = id, .count = count, .durability = damage};
}

static bool equal(struct item_data a, struct item_data b) {
	return a.id == b.id && (!a.id || (a.count == b.count && a.durability == b.durability));
}

static void reset(size_t capacity, enum inventory_sort_operation op) {
	memset(slots, 0, sizeof(slots));
	memset(pinned, 0, sizeof(pinned));
	input = (struct inventory_sort_input){.slots = slots, .max_stack = limits,
		.pinned = pinned, .capacity = capacity, .operation = op,
		.window = capacity == 45 ? WINDOW_TYPE_INVENTORY : WINDOW_TYPE_CHEST,
		.snapshot_generation = 7, .current_generation = 7};
}

static void set_limits(void) {
	for(size_t k = 0; k < input.capacity; k++) {
		struct item* def = item_get(&slots[k]);
		limits[k] = def ? def->max_stack : 0;
	}
}

static void conserved(const struct item_data* before, const struct item_data* after,
	struct item_data cursor, size_t begin, size_t end) {
	unsigned total_before = 0, total_after = cursor.id ? cursor.count : 0;
	for(size_t k = begin; k < end; k++) {
		total_before += before[k].id ? before[k].count : 0;
		total_after += after[k].id ? after[k].count : 0;
		if(!before[k].id) continue;
		unsigned a = 0, b = cursor.id == before[k].id
			&& cursor.durability == before[k].durability ? cursor.count : 0;
		for(size_t j = begin; j < end; j++) {
			if(before[j].id == before[k].id && before[j].durability == before[k].durability) a += before[j].count;
			if(after[j].id == before[k].id && after[j].durability == before[k].durability) b += after[j].count;
		}
		assert(a == b);
	}
	assert(total_before == total_after);
}

static enum inventory_sort_result build(void) {
	set_limits();
	return inventory_sort_build(&input, &plan);
}

/* Independent target oracle: stream original records into a compact list,
 * filling earlier exact identities, then select the smallest key repeatedly.
 * It does not use the planner's in-place consolidation or insertion sort. */
static void target_oracle(void) {
	struct item_data compact[54] = {{0}};
	bool used[54] = {false};
	size_t count = 0;
	for(size_t k = plan.begin; k < plan.end; k++) {
		if(pinned[k] || !slots[k].id) continue;
		struct item_data s = slots[k];
		for(size_t j = 0; j < count && s.count; j++) {
			if(compact[j].id != s.id || compact[j].durability != s.durability) continue;
			unsigned take = limits[k] - compact[j].count;
			if(take > s.count) take = s.count;
			compact[j].count += take; s.count -= take;
		}
		if(s.count) compact[count++] = s;
	}
	bool group = input.operation == INVENTORY_SORT_CHEST_GROUP
		|| input.operation == INVENTORY_SORT_MAIN_GROUP;
	for(size_t k = plan.begin, output = 0; k < plan.end; k++) {
		if(pinned[k]) continue;
		struct item_data expected = {0};
		if(output < count) {
			size_t best = 0;
			while(used[best]) best++;
			if(group) for(size_t j = best + 1; j < count; j++) {
				if(used[j]) continue;
				struct item_data a = compact[j], b = compact[best];
				if(a.id < b.id || (a.id == b.id && (a.durability < b.durability
					|| (a.durability == b.durability && a.count > b.count)))) best = j;
			}
			expected = compact[best]; used[best] = true; output++;
		}
		assert(equal(plan.target[k], expected));
	}
}

static void verify(enum inventory_sort_result result) {
	if(result != INVENTORY_SORT_READY && result != INVENTORY_SORT_UNCHANGED) {
		assert(!plan.count);
		return;
	}
	assert(plan.count <= 12 * (plan.end - plan.begin - 1));
	assert((result == INVENTORY_SORT_READY) == (plan.count != 0));
	target_oracle();
	memcpy(replay, slots, sizeof(replay));
	struct inventory inv = {.items = replay, .capacity = input.capacity};
	for(size_t k = 0; k < plan.count; k++) {
		const struct inventory_sort_click* c = &plan.clicks[k];
		assert(c->slot >= plan.begin && c->slot < plan.end && !pinned[c->slot]);
		assert(!c->right);
		assert(equal(replay[c->slot], c->expected));
		struct item_data expected = inventory_click_predict(&inv, input.window, c->slot, c->right, false);
		assert(equal(expected, c->expected));
		assert(equal(replay[c->slot], c->slot_after));
		assert(equal(inv.picked_item, c->cursor_after));
		conserved(slots, replay, inv.picked_item, plan.begin, plan.end);
	}
	assert(!inv.picked_item.id);
	for(size_t k = 0; k < input.capacity; k++) {
		assert(equal(replay[k], plan.target[k]));
		if(k < plan.begin || k >= plan.end || pinned[k])
			assert(!memcmp(&replay[k], &slots[k], sizeof(slots[k])));
	}
	/* Idempotence must return NO clicks, including stack-only and pinned. */
	memcpy(slots, replay, sizeof(slots));
	assert(build() == INVENTORY_SORT_UNCHANGED && !plan.count);
}

static void canonical(void) {
	reset(63, INVENTORY_SORT_CHEST_GROUP);
	const unsigned seed[][4] = {
		{0,4,32,0},{1,265,3,0},{2,15,16,0},{4,6,3,0},{6,319,1,0},
		{9,349,1,0},{11,302,1,0},{3,5,20,0},{5,263,8,0},{7,297,1,0},
		{10,4,12,0},{12,17,8,0},{13,50,4,0},{15,265,5,0},{18,5,40,0},
		{19,331,16,0},{21,50,16,0},{24,264,3,0},{8,274,1,17},{23,274,1,63}};
	for(size_t k = 0; k < sizeof(seed)/sizeof(*seed); k++)
		slots[seed[k][0]] = stack(seed[k][1], seed[k][2], seed[k][3]);
	assert(build() == INVENTORY_SORT_READY);
	/* Portable bytes, no ABI padding: slot BE16, button byte, then three
 * stacks (id BE16, count byte, damage BE16); null = ffff000000.
 * Expected stack is Packet102's pre-click stack, NOT slot_after. */
	FILE* f = fopen("tests/fixtures/inventory_sort_canonical.hex", "rb");
	assert(f);
	for(size_t k = 0; k < plan.count; k++) {
		const struct inventory_sort_click* c = &plan.clicks[k];
		unsigned bytes[18] = {c->slot >> 8, c->slot & 255, c->right};
		struct item_data s[] = {c->expected, c->slot_after, c->cursor_after};
		for(size_t j = 0; j < 3; j++) {
			unsigned id = s[j].id ? s[j].id : 65535;
			bytes[3+j*5] = id >> 8; bytes[4+j*5] = id & 255;
			bytes[5+j*5] = s[j].id ? s[j].count : 0;
			bytes[6+j*5] = s[j].id ? s[j].durability >> 8 : 0;
			bytes[7+j*5] = s[j].id ? s[j].durability & 255 : 0;
		}
		for(size_t j = 0; j < 18; j++) {
			unsigned expected;
			assert(fscanf(f, "%2x", &expected) == 1);
			assert(bytes[j] == expected);
		}
	}
	unsigned extra;
	assert(fscanf(f, "%2x", &extra) == EOF);
	fclose(f);
	const unsigned ids[] = {4,5,6,15,17,50,263,264,265,274,274,297,302,319,331,349};
	const unsigned counts[] = {44,60,3,16,8,20,8,3,8,1,1,1,1,1,16,1};
	for(size_t k = 0; k < 16; k++)
		assert(equal(plan.target[k], stack(ids[k], counts[k], k == 9 ? 17 : k == 10 ? 63 : 0)));
	verify(INVENTORY_SORT_READY);
	puts("PASS canonical frozen chest reset: byte-exact ordinary-click fixture + numeric target");
}

static void identity_goldens(void) {
	/* 740c583 ItemMap ctor: limit 1; damage identifies the map. Even
 * duplicate map IDs stay separate; the plan key orders by map ID. */
	reset(63, INVENTORY_SORT_CHEST_GROUP);
	slots[0] = stack(358,1,42); slots[1] = stack(358,1,7);
	slots[2] = stack(358,1,42);
	assert(item_get(&slots[0])->max_stack == 1);
	assert(build() == INVENTORY_SORT_READY);
	assert(equal(plan.target[0], stack(358,1,7)));
	assert(equal(plan.target[1], stack(358,1,42)));
	assert(equal(plan.target[2], stack(358,1,42)));
	assert(!plan.target[3].id);
	verify(INVENTORY_SORT_READY);
	puts("PASS map IDs: ordered, distinct and duplicate IDs remain single stacks");

	/* 740c583 ItemDye ctor/getItemNameIS: damage is the subtype. Merge
 * equal variants only, retaining overflow at the item-table limit. */
	reset(63, INVENTORY_SORT_CHEST_GROUP);
	slots[0] = stack(351,9,15); slots[1] = stack(351,60,1);
	slots[2] = stack(351,7,15); slots[3] = stack(351,7,1);
	assert(build() == INVENTORY_SORT_READY);
	assert(equal(plan.target[0], stack(351,64,1)));
	assert(equal(plan.target[1], stack(351,3,1)));
	assert(equal(plan.target[2], stack(351,16,15)));
	assert(!plan.target[3].id);
	verify(INVENTORY_SORT_READY);
	puts("PASS dye variants: equal damage merges, distinct damage stays separate");

	reset(63, INVENTORY_SORT_CHEST_GROUP);
	slots[0] = stack(257,1,63); slots[1] = stack(257,1,17);
	assert(build() == INVENTORY_SORT_READY);
	assert(equal(plan.target[0], stack(257,1,17)));
	assert(equal(plan.target[1], stack(257,1,63)));
	assert(!plan.target[2].id);
	verify(INVENTORY_SORT_READY);
	puts("PASS damaged pickaxes: separate stacks ordered by damage");
}

static void goldens(void) {
	/* All four actions in single, double and the actual player window. */
	for(int layout = 0; layout < 3; layout++) for(int op = 0; op < 4; op++) {
		if(layout == 2 && op < 2) continue;
		reset(layout == 0 ? 63 : layout == 1 ? 90 : 45, op);
		size_t begin = op < 2 ? 0 : input.capacity == 45 ? 9 : input.capacity - 36;
		slots[begin] = stack(357,7,0); slots[begin+1] = stack(35,9,2);
		slots[begin+2] = stack(60000,99,123); pinned[begin+2] = true;
		slots[begin+3] = stack(357,3,0); slots[begin+4] = stack(35,60,1);
		slots[begin+5] = stack(35,7,1);
		slots[input.capacity-1] = stack(1,44,7); /* hotbar witness */
		assert(build() == INVENTORY_SORT_READY);
		const struct item_data grouped[] = {{35,1,64},{35,1,3},{35,2,9},{357,0,8},{357,0,2}};
		const struct item_data stacked[] = {{357,0,8},{35,2,9},{357,0,2},{35,1,64},{35,1,3}};
		for(size_t k = 0; k < 5; k++)
			assert(equal(plan.target[begin+k+(k >= 2)], op % 2 ? stacked[k] : grouped[k]));
		verify(INVENTORY_SORT_READY);
	}
	/* Full destinations: tools of different wear cannot exchange without
 * an unpinned spare (Container.slotClick takes a zero-count merge). */
	reset(63, INVENTORY_SORT_CHEST_GROUP);
	for(size_t k = 0; k < 27; k++) slots[k] = stack(257,1,27-k);
	assert(build() == INVENTORY_SORT_NEEDS_SPACE && !plan.count);
	slots[26] = (struct item_data){0}; pinned[26] = true;
	assert(build() == INVENTORY_SORT_NEEDS_SPACE && !plan.count);
	pinned[26] = false;
	assert(build() == INVENTORY_SORT_READY); verify(INVENTORY_SORT_READY);
	/* A full scope with incompatible IDs DOES sort without a spare. */
	reset(63, INVENTORY_SORT_CHEST_GROUP);
	for(size_t k = 0; k < 27; k++) slots[k] = stack(k % 2 ? 1 : 3,64,0);
	assert(build() == INVENTORY_SORT_READY); verify(INVENTORY_SORT_READY);
	/* Non-subtype metadata is preserved even though vanilla would merge it. */
	reset(63, INVENTORY_SORT_CHEST_GROUP);
	slots[0] = stack(1,9,3); slots[1] = stack(1,7,2);
	assert(build() == INVENTORY_SORT_READY); verify(INVENTORY_SORT_READY);
	assert(slots[0].durability == 2 && slots[0].count == 7);
	input.cursor = stack(1,1,0);
	assert(build() == INVENTORY_SORT_HELD && !plan.count);
	input.cursor = (struct item_data){0}; input.current_generation++;
	assert(build() == INVENTORY_SORT_STALE && !plan.count);
	input.current_generation--; slots[0] = stack(357,9,0);
	assert(build() == INVENTORY_SORT_INVALID && !plan.count);
	slots[0] = stack(60000,1,0);
	assert(build() == INVENTORY_SORT_INVALID && !plan.count);
	pinned[0] = true;
	verify(build());
	set_limits(); limits[1] = 1;
	assert(inventory_sort_build(&input, &plan) == INVENTORY_SORT_INVALID);
	/* Consolidation creates the spare needed for an otherwise full tool
 * permutation. Also exercise a partially filled destination + return. */
	reset(63, INVENTORY_SORT_CHEST_GROUP);
	for(size_t k = 0; k < 25; k++) slots[k] = stack(257,1,25-k);
	slots[25] = stack(1,32,0); slots[26] = stack(1,32,0);
	assert(build() == INVENTORY_SORT_READY); verify(INVENTORY_SORT_READY);
	reset(90, INVENTORY_SORT_CHEST_STACK);
	for(size_t k = 0; k < 54; k++) pinned[k] = true;
	assert(build() == INVENTORY_SORT_UNCHANGED); verify(INVENTORY_SORT_UNCHANGED);
	input.capacity = 36;
	assert(build() == INVENTORY_SORT_INVALID && !plan.count);
	reset(45, INVENTORY_SORT_CHEST_GROUP);
	assert(build() == INVENTORY_SORT_INVALID && !plan.count);
	puts("PASS four operations / layouts / pinned / limits / metadata / zero-click refusal");
}

static uint32_t rng = 0x17350906;
static uint32_t random_u32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void properties(void) {
	const unsigned ids[] = {1,3,35,257,263,274,323,325,332,344,357};
	unsigned ready = 0, refused = 0;
	for(unsigned trial = 0; trial < 2000; trial++) {
		size_t capacity = trial % 3 == 0 ? 45 : trial % 3 == 1 ? 63 : 90;
		reset(capacity, capacity == 45 ? 2 + trial % 2 : trial % 4);
		for(size_t k = 0; k < capacity; k++) {
			if(random_u32() % 5 == 0) continue;
			unsigned id = ids[random_u32() % (sizeof(ids)/sizeof(*ids))];
			slots[k] = stack(id, 1, random_u32() % 4);
			slots[k].count = 1 + random_u32() % item_get(&slots[k])->max_stack;
			pinned[k] = random_u32() % 11 == 0;
		}
		if(trial % 13 == 0) input.cursor = stack(1,1,0);
		if(trial % 17 == 0) input.current_generation++;
		struct item_data original[90]; memcpy(original, slots, sizeof(slots));
		enum inventory_sort_result r = build();
		assert(!memcmp(original, slots, sizeof(slots)));
		if(r == INVENTORY_SORT_READY) ready++; else if(r != INVENTORY_SORT_UNCHANGED) refused++;
		verify(r);
	}
	assert(ready > 1000 && refused > 100);
	printf("PASS 2000 seeded cases: %u realizable, %u refused; per-click conservation, predictions, bounds, idempotence\n", ready, refused);
}

int main(void) {
	blocks_init(); items_init();
	canonical(); identity_goldens(); goldens(); properties();
	printf("sizeof plan=%zu click=%zu inventory=%zu; module static delta=0\n",
		sizeof(plan), sizeof(struct inventory_sort_click), sizeof(struct inventory));
	return 0;
}
