/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef ITEM_INVENTORY_SORT_H
#define ITEM_INVENTORY_SORT_H

#include "inventory_click.h"

#define INVENTORY_SORT_WINDOW_SLOTS 90
#define INVENTORY_SORT_SCOPE_SLOTS 54
/* At most 2(n-1) consolidations (3 clicks each), then n-1 exchanges
 * (6 clicks each). 27:312; 54:636; hypothetical 36:420. Main is ONLY 27. */
#define INVENTORY_SORT_MAX_CLICKS (12 * (INVENTORY_SORT_SCOPE_SLOTS - 1))

enum inventory_sort_operation {
	INVENTORY_SORT_CHEST_GROUP,
	INVENTORY_SORT_CHEST_STACK,
	INVENTORY_SORT_MAIN_GROUP,
	INVENTORY_SORT_MAIN_STACK
};

enum inventory_sort_result {
	INVENTORY_SORT_READY,
	INVENTORY_SORT_UNCHANGED,
	INVENTORY_SORT_HELD,
	INVENTORY_SORT_STALE,
	INVENTORY_SORT_INVALID,
	INVENTORY_SORT_NEEDS_SPACE
};

struct inventory_sort_input {
	const struct item_data* slots;
	const uint8_t* max_stack; /* item table value for each known occupied slot */
	const bool* pinned; /* window-sized; opaque slots stay byte-identical */
	size_t capacity;
	enum window_type window;
	enum inventory_sort_operation operation;
	struct item_data cursor;
	uint32_t snapshot_generation, current_generation;
};

struct inventory_sort_click {
	uint16_t slot;
	bool right; /* always false in this planner; never shift */
	/* 740c583 Container.slotClick returns PRE-click slot for Packet102.
	 * These are deliberately separate from the post-click prediction. */
	struct item_data expected, slot_after, cursor_after;
};

/* Caller-owned buffer: 15,264 bytes on Wii. The Wii caller (slice 4) MUST
 * place it in a static or MEM2 buffer, never on the stack: main()'s frame
 * budget is constrained by the boot-death bisect (docs/build_architecture.md).
 * (click 22, input 40, existing inventory 120). No allocator or mutable
 * module globals. One per simultaneous plan; do not alias input with this.
 * Scratch is embedded so planning needs no second buffer or large stack frame.
 * Only READY exposes clicks; every refusal/UNCHANGED has count zero. */
struct inventory_sort_plan {
	struct inventory_sort_click clicks[INVENTORY_SORT_MAX_CLICKS];
	struct item_data target[INVENTORY_SORT_WINDOW_SLOTS];
	uint32_t generation;
	size_t count, begin, end;
	struct inventory simulation;
	struct item_data working[INVENTORY_SORT_WINDOW_SLOTS];
	uint8_t movable[INVENTORY_SORT_SCOPE_SLOTS];
};

#ifdef PLATFORM_WII
__extension__ _Static_assert(sizeof(struct inventory_sort_plan) == 15264,
	"Wii sort plan size changed; review caller static/MEM2 budget");
#endif

enum inventory_sort_result inventory_sort_build(
	const struct inventory_sort_input* input, struct inventory_sort_plan* plan);

/* Calls are synchronous. Input arrays must have capacity entries, stay
 * immutable for the call and not alias plan. The initialized item table must
 * also remain immutable. No pointers to live window state are retained.
 * Executor MUST compare plan.generation again before submitting clicks and
 * cancel on changes/corrections; this pure function cannot watch a live window.
 * Pinned includes every opaque/unsupported record from the persistence owner.
 * Ambiguous records, viewer admission and packet ACKs remain caller gates.
 * The embedded simulation.items pointer is scratch, not an executor handle:
 * do not copy this struct and subsequently use that pointer. */

#endif
