/*
	Copyright (c) 2026

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	Beta-parity smoke test for the scheduled-tick neighbour-fill filter.

	The Correctness milestone (Task 8, commit b6c3ffe) replaced a
	full-world scan with a scheduled tick queue and introduced
	server_world_block_needs_neighbours() to decide whether the
	caller-side neighbour array should be populated before dispatching
	to a block's onWorldTick handler.

	The function is a hand-maintained allowlist. If a future onWorldTick
	starts reading info->neighbours, but its block type is omitted from
	this list, the handler's `if (!info->neighbours) return;` NULL guard
	silently skips every scheduled tick: the block looks broken with no
	error message.

	This test pins down the contract for the current set of
	neighbour-reading onWorldTick handlers. Audit basis (grep
	'info->neighbours' across source/block/*.c with onWorldTick):

	  - block_pressure_plate.c   onWorldTick reads neighbours
	      types: BLOCK_WOOD_PRESSURE_PLATE, BLOCK_STONE_PRESSURE_PLATE
	  - block_redstone_wire.c    onWorldTick reads neighbours
	      type: BLOCK_REDSTONE_WIRE
	  - block_tnt.c              has no onWorldTick: primed TNT is an entity
	  - block_torch.c            onWorldTick does NOT read neighbours
	      types: BLOCK_TORCH, BLOCK_REDSTONE_TORCH,
	             BLOCK_REDSTONE_TORCH_LIT (all share the same
	             onWorldTick which only uses metadata for particle
	             facing — neighbours are not accessed)
	  - block_door.c             onWorldTick = NULL
	  - block_trapdoor.c         onWorldTick = NULL (uses neighbours
	                             only in onNeighbourBlockChange, which
	                             is a different code path with its own
	                             neighbour-fill in server_local.c)
	  - block_rail.c             no onWorldTick at all

	The filter is allowed to include extra block types beyond the
	strict minimum; it's a permissive allowlist. So this test asserts
	the *required* set is included, plus a few negative cases (AIR /
	STONE / DIRT should not be in the filter because they would
	waste a 6-neighbour fill on every scheduled tick).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "block/blocks_data.h"
#include "network/server_world.h"

/* Each row is one assertion on the filter. */
struct filter_case {
	const char* name;
	uint8_t type;
	bool required;  /* true = filter MUST return this value */
	bool expected;  /* the value it MUST return */
};

static int run_cases(const struct filter_case* cases, size_t n) {
	int fails = 0;
	for(size_t i = 0; i < n; i++) {
		bool got = server_world_block_needs_neighbours(cases[i].type);
		if(cases[i].required && got != cases[i].expected) {
			fprintf(stderr,
					"FAIL: server_world_block_needs_neighbours(%s = %u) = %s, "
					"required %s\n",
					cases[i].name, cases[i].type,
					got ? "true" : "false",
					cases[i].expected ? "true" : "false");
			fails++;
		}
	}
	return fails;
}

int main(void) {
	int fails = 0;

	/*
		REQUIRED-TRUE: blocks whose onWorldTick reads info->neighbours.
		Omitting any of these from the filter is a real bug; the block
		will appear to never tick (handler bails on NULL guard).
	*/
	const struct filter_case required_true[] = {
		{"BLOCK_REDSTONE_WIRE",        BLOCK_REDSTONE_WIRE,        true, true},
		{"BLOCK_WOOD_PRESSURE_PLATE",  BLOCK_WOOD_PRESSURE_PLATE,  true, true},
		{"BLOCK_STONE_PRESSURE_PLATE", BLOCK_STONE_PRESSURE_PLATE, true, true},
		// NOTE: the redstone repeater (93/94) is intentionally NOT required here -- its
		// onWorldTick reads input via the world accessor, not info->neighbours, so it does
		// not need the allowlist (see server_world_block_filter.c).
	};
	fails += run_cases(required_true,
					   sizeof(required_true) / sizeof(required_true[0]));

	/*
		REQUIRED-FALSE: blocks that absolutely do not need a neighbour
		array. Including them in the filter would cost a 6-block lookup
		on every scheduled tick for no benefit. AIR is the worst
		offender if accidentally included because it is by far the
		most-scheduled block type during chunk gen.
	*/
	const struct filter_case required_false[] = {
		{"BLOCK_AIR",        BLOCK_AIR,        true, false},
		{"BLOCK_STONE",      BLOCK_STONE,      true, false},
		{"BLOCK_DIRT",       BLOCK_DIRT,       true, false},
		{"BLOCK_COBBLESTONE",BLOCK_COBBLESTONE,true, false},
		{"BLOCK_SAND",       BLOCK_SAND,       true, false},
		{"BLOCK_PLANKS",     BLOCK_PLANKS,     true, false},
		{"BLOCK_GLASS",      BLOCK_GLASS,      true, false},
		{"BLOCK_LEAVES",     BLOCK_LEAVES,     true, false},
		{"BLOCK_LOG",        BLOCK_LOG,        true, false},
		{"BLOCK_TNT",        BLOCK_TNT,        true, false},
	};
	fails += run_cases(required_false,
					   sizeof(required_false) / sizeof(required_false[0]));

	/*
		PERMISSIVE: blocks whose onWorldTick exists but does NOT read
		neighbours (block_torch.c family). The filter is allowed to
		include them — costs one wasted neighbour fill per scheduled
		torch tick — but isn't required to. We assert nothing here;
		just document the audit observation. Same goes for doors and
		rails which have NULL onWorldTick — the filter currently
		includes them, which is harmless because the scheduled tick
		path skips early when b->onWorldTick is NULL (see
		server_world.c::server_world_fire_scheduled_tick).
	*/

	if(fails) {
		fprintf(stderr,
				"\ntest_scheduled_tick_parity: %d failure(s).\n"
				"If a REQUIRED-TRUE case failed, the missing block type's "
				"onWorldTick is silently broken — add it to "
				"server_world_block_filter.c.\n"
				"If a REQUIRED-FALSE case failed, the filter is being "
				"over-eager and will waste neighbour fills on every "
				"scheduled tick for that block — remove the entry.\n",
				fails);
		return 1;
	}

	printf("test_scheduled_tick_parity: all %zu cases passed.\n",
		   sizeof(required_true) / sizeof(required_true[0])
			   + sizeof(required_false) / sizeof(required_false[0]));
	return 0;
}
