/*
	Golden behavior test for Beta 1.7.3 redstone-wire power spread (M4 redstone),
	authored BEFORE the wire rewrite per the project's golden-suite-first rule.

	Drives the production wire flood (source/block/redstone_logic.c
	redstone_wire_update) over a flat STUB cell grid injected via struct redstone_world
	-- no server_world/get_block, no gfx (the furnace_logic / test_relight precedent).
	Pins, against values transcribed from the decompile (740c583):

	  - BlockRedstoneWire.calculateCurrentChanges: a non-wire source injects a flat 15
	    (isBlockIndirectlyGettingPowered => strength 15); each hop applies ONE decrement
	    (minus one while positive, floored at 0); getMaxCurrentStrength reads ONLY neighbour
	    WIRE metadata. So a straight run fades 15,14,...,1,0.
	  - updateAndPropagateCurrentStrength runs the recompute SYNCHRONOUSLY and
	    RECURSIVELY within the originating update: a SINGLE redstone_wire_update call
	    settles the WHOLE run (0 game-ticks). This deliberately FAILS the old 1-cell/tick
	    crawl, documenting synchronous as canonical (CLAUDE.md: prove equivalence, no drift).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/redstone_logic.h"

// Flat headless world: a straight run of redstone wire at y=WY, z=WZ, x in [WX0, WX0+L),
// on a solid stone floor at y=WY-1. The near end (WX0) is flat-15 powered by an (implicit)
// non-wire source. Everything else is air. meta[] is indexed by x (sparse; x stays < 64).
#define WX0 5
#define WZ 5
#define WY 64
#define WLEN 16 // x = 5..20 -> settled strengths 15..0 (the full fade to 0)

struct grid {
	int meta[64];
};

static bool is_wire_cell(int x, int y, int z) {
	return y == WY && z == WZ && x >= WX0 && x < WX0 + WLEN;
}

static int g_get_type(void* c, int x, int y, int z) {
	(void)c;
	return is_wire_cell(x, y, z) ? REDSTONE_WIRE_ID : 0; // 0 = air (and the floor block id is
														 // irrelevant: only is_normal_cube matters)
}
static int g_get_meta(void* c, int x, int y, int z) {
	struct grid* g = c;
	return is_wire_cell(x, y, z) ? g->meta[x] : 0;
}
static void g_set_meta(void* c, int x, int y, int z, int m) {
	struct grid* g = c;
	if(is_wire_cell(x, y, z))
		g->meta[x] = m;
}
static bool g_is_normal_cube(void* c, int x, int y, int z) {
	(void)c;
	(void)x;
	(void)z;
	return y == WY - 1; // solid stone floor; wire (WY) and air (WY+1) are not normal cubes
}
static bool g_is_indirectly_powered(void* c, int x, int y, int z) {
	(void)c;
	return x == WX0 && y == WY && z == WZ; // a non-wire source at the near end -> flat 15
}
static int g_updates;
static void g_mark(void* c, int x, int y, int z) {
	(void)c;
	(void)x;
	(void)y;
	(void)z;
	g_updates++;
}

int main(void) {
	struct grid g;
	memset(&g, 0, sizeof(g)); // every wire starts unpowered (strength 0)
	struct redstone_world w = {
		.ctx = &g,
		.get_type = g_get_type,
		.get_meta = g_get_meta,
		.set_meta = g_set_meta,
		.is_normal_cube = g_is_normal_cube,
		.is_indirectly_powered = g_is_indirectly_powered,
		.mark_needs_update = g_mark,
	};

	// The source switches ON: it notifies the adjacent wire, whose
	// updateAndPropagateCurrentStrength floods the whole run in ONE synchronous call.
	redstone_wire_update(&w, WX0, WY, WZ);

	// Settled gradient: strength at x = max(0, 15 - (x - WX0)).
	for(int x = WX0; x < WX0 + WLEN; x++) {
		int expect = 15 - (x - WX0);
		if(expect < 0)
			expect = 0;
		assert(g.meta[x] == expect);
	}
	assert(g.meta[WX0] == 15);				// flat-15 at the source end
	assert(g.meta[WX0 + WLEN - 1] == 0);	// faded to 0 after 15 hops
	assert(g.meta[WX0 + 1] == 14);			// one decrement per hop

	// A re-flood with no input change must be idempotent (settled stays settled).
	redstone_wire_update(&w, WX0, WY, WZ);
	for(int x = WX0; x < WX0 + WLEN; x++) {
		int expect = 15 - (x - WX0);
		if(expect < 0)
			expect = 0;
		assert(g.meta[x] == expect);
	}

	printf("test_redstone_wire_attenuation: OK (Beta flat-15 source, -1/hop fade 15..0, "
		   "synchronous single-call settle, idempotent)\n");
	return 0;
}
