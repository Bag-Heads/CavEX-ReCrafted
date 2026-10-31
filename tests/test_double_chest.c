/*
	Unit test for the Beta 1.7.3 double-chest adjacency rules (chest_link.c).

	The pairing + placement logic is the Beta-fidelity-critical part of double
	chests and cannot be observed easily in Dolphin (you'd have to place chests
	by hand and inspect the merged inventory). These assertions pin down the
	exact Beta behaviours:
	  - a lone chest is single; two adjacent chests form one pair
	  - the canonical "primary" half is deterministic (lexicographic x then z),
	    so both halves agree on slot ordering regardless of which is opened
	  - placement is rejected when it would create a chest cluster: more than
	    one chest neighbour, or a neighbour that is already half of a double
	    (Beta's no-triple invariant that keeps pairing unambiguous)

	Neighbour index order matches chest_link.h: 0 = -X, 1 = +X, 2 = -Z, 3 = +Z.
*/

#include <assert.h>
#include <stdbool.h>

#include "block/chest_link.h"

static const bool NONE[4] = {false, false, false, false};

int main(void) {
	// --- neighbour count ---
	assert(chest_neighbor_count(NONE) == 0);
	assert(chest_neighbor_count((bool[4]) {true, false, false, false}) == 1);
	assert(chest_neighbor_count((bool[4]) {true, false, true, false}) == 2);
	assert(chest_neighbor_count((bool[4]) {true, true, true, true}) == 4);

	// --- partner index: lone chest is single, one neighbour pairs ---
	assert(chest_partner_index(NONE) == -1);
	assert(chest_partner_index((bool[4]) {true, false, false, false}) == 0);
	assert(chest_partner_index((bool[4]) {false, true, false, false}) == 1);
	assert(chest_partner_index((bool[4]) {false, false, false, true}) == 3);
	// multiple chest neighbours (legacy world): lowest index, deterministic
	assert(chest_partner_index((bool[4]) {false, true, true, false}) == 1);

	// --- placement rule (Beta canPlaceBlockAt) ---
	// open space: always placeable
	assert(chest_can_place(NONE, NONE));
	// exactly one chest neighbour that is itself single: placeable -> forms a pair
	assert(chest_can_place((bool[4]) {true, false, false, false}, NONE));
	assert(chest_can_place((bool[4]) {false, false, false, true}, NONE));
	// two chest neighbours: rejected (would sit in the middle of a row)
	assert(!chest_can_place((bool[4]) {true, true, false, false}, NONE));
	assert(!chest_can_place((bool[4]) {true, false, true, false}, NONE));
	// one chest neighbour, but that neighbour is already part of a double:
	// rejected (would create a triple)
	assert(!chest_can_place((bool[4]) {true, false, false, false},
							(bool[4]) {true, false, false, false}));
	// the "already paired" flag only matters where there actually is a chest:
	// a paired-flag on a non-chest neighbour must not block placement
	assert(chest_can_place(NONE, (bool[4]) {true, true, true, true}));

	// --- canonical primary ordering (lexicographic x, then z) ---
	// X-axis pair: lower x is primary
	assert(chest_is_primary(10, 5, 11, 5));   // (10,5) < (11,5)
	assert(!chest_is_primary(11, 5, 10, 5));  // (11,5) is secondary
	// Z-axis pair: equal x, lower z is primary
	assert(chest_is_primary(7, 20, 7, 21));   // (7,20) < (7,21)
	assert(!chest_is_primary(7, 21, 7, 20));
	// negative coordinates behave correctly
	assert(chest_is_primary(-5, 0, -4, 0));   // -5 < -4
	assert(!chest_is_primary(-4, 0, -5, 0));

	// symmetry: for any real pair, exactly one half is primary
	int pairs[][4] = {
		{10, 5, 11, 5}, {7, 20, 7, 21}, {-5, 0, -4, 0}, {0, -3, 0, -2},
	};
	for(int i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); i++) {
		int ax = pairs[i][0], az = pairs[i][1];
		int bx = pairs[i][2], bz = pairs[i][3];
		bool a_primary = chest_is_primary(ax, az, bx, bz);
		bool b_primary = chest_is_primary(bx, bz, ax, az);
		assert(a_primary != b_primary); // exactly one is primary
	}

	return 0;
}
