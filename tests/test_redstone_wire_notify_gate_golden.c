/*
	Golden behavior test for Beta 1.7.3 redstone-wire NEIGHBOUR-NOTIFY GATING (M4 redstone).

	Beta BlockRedstoneWire.calculateCurrentChanges only adds a wire (and its 6 axis neighbours)
	to blocksNeedingUpdate -- the deferred set drained into notifyBlocksOfNeighborChange, i.e.
	the cells whose components get re-evaluated -- inside the gate (old strength == 0 || scratch == 0).

	CRUCIAL QUIRK (decompile 740c583, read directly, not via a paraphrase): the scratch value in that
	gate is NOT the new strength. It is reused as scratch inside the recursion loop and ends as
	`(newStrength > 0 ? newStrength - 1 : 0)`. So scratch == 0 <=> newStrength <= 1. The faithful
	gate is therefore:

	    notify  <=>  old == 0  ||  newStrength <= 1

	A wire that merely dims/brightens between two strengths >= 2 (e.g. 14->13, or 1->2) notifies
	NOTHING in Beta. (The clean mc173 reimpl notifies on EVERY change -- less faithful here; the
	decompile is authoritative per CLAUDE.md's authority order.) This test drives the production
	flood (redstone_logic.c redstone_wire_update) over a flat stub run and records WHICH wire
	cells call mark_needs_update, asserting the exact gated set across rising / non-zero-shift /
	drop-to-1 / falling-to-0 transitions. It RED-fails the old unconditional-notify behaviour.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/redstone_logic.h"

// A straight run of wire at y=WY, z=WZ, x in [0, WLEN), on a solid floor at y=WY-1. Sources are
// modelled as "indirectly powered" wire cells (flat 15), togglable per cell to reshape the field.
#define WZ 5
#define WY 64
#define WLEN 16

static int g_meta[WLEN];
static bool g_source[WLEN];
static bool g_marked[WLEN];

static bool is_wire(int x, int y, int z) {
	return y == WY && z == WZ && x >= 0 && x < WLEN;
}
static int gt(void* c, int x, int y, int z) {
	(void)c;
	return is_wire(x, y, z) ? REDSTONE_WIRE_ID : 0;
}
static int gm(void* c, int x, int y, int z) {
	(void)c;
	return is_wire(x, y, z) ? g_meta[x] : 0;
}
static void sm(void* c, int x, int y, int z, int m) {
	(void)c;
	if(is_wire(x, y, z))
		g_meta[x] = m;
}
static bool inc(void* c, int x, int y, int z) {
	(void)c;
	(void)x;
	(void)z;
	return y == WY - 1; // solid floor; the wire row and the air above are not normal cubes
}
static bool ip(void* c, int x, int y, int z) {
	(void)c;
	return is_wire(x, y, z) && g_source[x];
}
static void mk(void* c, int x, int y, int z) {
	(void)c;
	(void)y;
	(void)z;
	if(x >= 0 && x < WLEN)
		g_marked[x] = true;
}

static struct redstone_world W;

static void clear_marks(void) {
	memset(g_marked, 0, sizeof(g_marked));
}

int main(void) {
	W = (struct redstone_world) {
		.get_type = gt,
		.get_meta = gm,
		.set_meta = sm,
		.is_normal_cube = inc,
		.is_indirectly_powered = ip,
		.mark_needs_update = mk,
	};
	memset(g_meta, 0, sizeof(g_meta));
	memset(g_source, 0, sizeof(g_source));

	// --- Phase A: rising edge. Source switches ON at x=0; the whole run floods 0 -> 15..0. ---
	g_source[0] = true;
	clear_marks();
	redstone_wire_update(&W, 0, WY, WZ);
	for(int x = 0; x < WLEN; x++)
		assert(g_meta[x] == (15 - x < 0 ? 0 : 15 - x)); // settled gradient 15,14,...,1,0
	// Every cell that changed went 0 -> something => old == 0 => MARKED. x=15 stayed 0 (no change).
	for(int x = 0; x <= 14; x++)
		assert(g_marked[x]); // x with strength 15..1 all changed from 0 -> marked
	assert(!g_marked[15]);	 // x=15 stayed at strength 0: no change, no notify

	// --- Phase B: add a SECOND source at x=2. Interior cells shift between non-zero strengths. ---
	// New settled: x0=15, x1=14, and x>=2 -> 17-x (x2=15, x3=14, ..., x15=2). Only x15 (0 -> 2)
	// crosses zero; every other changed cell moves between strengths >= 2 and must NOT notify.
	g_source[2] = true;
	clear_marks();
	redstone_wire_update(&W, 2, WY, WZ);
	assert(g_meta[0] == 15);
	assert(g_meta[1] == 14);
	for(int x = 2; x < WLEN; x++)
		assert(g_meta[x] == 17 - x); // 15,14,...,2
	for(int x = 0; x < WLEN; x++) {
		if(x == 15)
			assert(g_marked[x]); // 0 -> 2 : old == 0 => MARKED
		else
			assert(!g_marked[x]); // no-change (x0,x1) or >=2 -> >=2 shift => NOT marked
	}

	// --- Phase C: remove the x=2 source. The run relaxes back to source-at-x0 only (x -> 15-x). ---
	// From phase B: x2=15->13, x3=14->12, ... (all >=2 -> >=2, NOT marked); x14: 3 -> 1 (old!=0 but
	// new==1 => MARKED, the var9 quirk); x15: 2 -> 0 (new==0 => MARKED). x0,x1 unchanged.
	g_source[2] = false;
	clear_marks();
	redstone_wire_update(&W, 2, WY, WZ);
	for(int x = 0; x < WLEN; x++)
		assert(g_meta[x] == (15 - x < 0 ? 0 : 15 - x)); // back to 15,14,...,1,0
	for(int x = 0; x < WLEN; x++) {
		if(x == 14)
			assert(g_marked[x]); // 3 -> 1 : new strength <= 1 => MARKED (the quirk this pins)
		else if(x == 15)
			assert(g_marked[x]); // 2 -> 0 : new strength == 0 => MARKED
		else
			assert(!g_marked[x]); // x0/x1 unchanged; x2..x13 are >=2 -> >=2 => NOT marked
	}

	printf("test_redstone_wire_notify_gate: OK (Beta notifies only on old==0 || newStrength<=1; "
		   ">=2->>=2 dims suppressed; the var9-reuse new<=1 quirk pinned)\n");
	return 0;
}
