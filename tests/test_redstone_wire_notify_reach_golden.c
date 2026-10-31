/*
	Golden for the wire neighbour-notify REACH (M4 redstone, R3).

	Beta BlockRedstoneWire, on a wire cell changing, adds the cell PLUS its 6 face-neighbours to
	blocksNeedingUpdate and drains each via notifyBlocksOfNeighborChange -- which fires
	onNeighborBlockChange on THAT cell's 6 neighbours. The reach is therefore TWO hops: the union
	of the face-neighbours of {(x,y,z)} U its 6 face-neighbours. A redstone torch mounted on a
	SOLID block that is face-adjacent to the wire (the canonical NOT/NOR gate) sits two hops from
	the wire and is armed ONLY by the 2nd hop. The original production rs_mark scanned just the
	wire's 6 direct neighbours (one hop), so it never armed such a torch and the gate silently
	never inverted.

	This pins the pure reach enumerator redstone_wire_notify_targets: the full distinct set is 25
	cells (1 self + 6 face + 6 axis-2 + 12 edge), every cell visited exactly once, the canonical
	torch-on-mount cell IS included, and a distance-3 cell is NOT.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "block/redstone_logic.h"

#define WX 5
#define WY 64
#define WZ 5

struct collector {
	int cells[64][3];
	int n;
};

static void collect(void* ctx, int x, int y, int z) {
	struct collector* c = ctx;
	assert(c->n < 64);
	c->cells[c->n][0] = x;
	c->cells[c->n][1] = y;
	c->cells[c->n][2] = z;
	c->n++;
}

static int count_at(struct collector* c, int x, int y, int z) {
	int n = 0;
	for(int i = 0; i < c->n; i++)
		if(c->cells[i][0] == x && c->cells[i][1] == y && c->cells[i][2] == z)
			n++;
	return n;
}

int main(void) {
	struct collector c = {.n = 0};
	redstone_wire_notify_targets(WX, WY, WZ, &c, collect);

	// Exactly the 25-cell two-hop reach, each visited once (deduped).
	assert(c.n == 25);
	for(int i = 0; i < c.n; i++)
		assert(count_at(&c, c.cells[i][0], c.cells[i][1], c.cells[i][2]) == 1);

	// Self (the changed wire cell) is reached once (it is a neighbour of each of its neighbours).
	assert(count_at(&c, WX, WY, WZ) == 1);

	// All 6 face-neighbours (one hop) are reached -- includes a solid mount block beside the wire.
	assert(count_at(&c, WX + 1, WY, WZ) == 1); // +x mount block B
	assert(count_at(&c, WX - 1, WY, WZ) == 1);
	assert(count_at(&c, WX, WY + 1, WZ) == 1);
	assert(count_at(&c, WX, WY - 1, WZ) == 1);
	assert(count_at(&c, WX, WY, WZ + 1) == 1);
	assert(count_at(&c, WX, WY, WZ - 1) == 1);

	// THE canonical NOT-gate cell: torch floor-mounted on top of mount block B=(WX+1,WY,WZ) lives
	// at (WX+1, WY+1, WZ) -- two hops from the wire. MUST be reached (the bug this fixes).
	assert(count_at(&c, WX + 1, WY + 1, WZ) == 1);

	// Other 2nd-hop cells: an axis-2 cell and the other edge cells around the mount block.
	assert(count_at(&c, WX + 2, WY, WZ) == 1);	   // axis-2 (neighbour of the +x face cell)
	assert(count_at(&c, WX + 1, WY - 1, WZ) == 1); // torch on the BOTTOM of B (ceiling mount)
	assert(count_at(&c, WX + 1, WY, WZ + 1) == 1); // torch on a SIDE of B

	// Distance-3 cells are NOT reached (the reach is exactly two hops, not three).
	assert(count_at(&c, WX + 2, WY + 1, WZ) == 0);
	assert(count_at(&c, WX + 3, WY, WZ) == 0);
	assert(count_at(&c, WX + 1, WY + 1, WZ + 1) == 0);

	printf("test_redstone_wire_notify_reach: OK (Beta two-hop blocksNeedingUpdate reach = 25 "
		   "distinct cells; torch-on-mount armed; distance-3 excluded)\n");
	return 0;
}
