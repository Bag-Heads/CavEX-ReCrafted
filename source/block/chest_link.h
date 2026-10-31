/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef CHEST_LINK_H
#define CHEST_LINK_H

#include <stdbool.h>

/*
	Pure Java Beta 1.7.3 double-chest adjacency rules, deliberately free of any
	world/engine dependency so they can be unit-tested headless. The caller
	gathers the four horizontal-neighbour facts from the world and feeds them
	in; the Beta-fidelity-critical decisions live here.

	A "double chest" in Beta is two horizontally-adjacent chests. Beta's
	placement rule guarantees a chest belongs to at most one pair (no triples
	or clusters), so the pairing is unambiguous and derived purely from block
	adjacency — never stored — which is why this is all the state we need.

	Horizontal neighbour index convention (the CHEST_LINK_DX/DZ tables below
	define it; caller and tests MUST use the same order):
	  0 = -X, 1 = +X, 2 = -Z, 3 = +Z
*/

// dx/dz for neighbour index 0..3. Exported so callers and tests agree on order.
extern const int CHEST_LINK_DX[4];
extern const int CHEST_LINK_DZ[4];

// How many of the four horizontal neighbours are chests.
int chest_neighbor_count(const bool neighbor_is_chest[4]);

/*
	Beta BlockChest.canPlaceBlockAt: a chest may be placed at a cell iff
	  (a) at most one of its four horizontal neighbours is a chest, AND
	  (b) none of those neighbour chests is already half of a double chest
	      (otherwise placing here would create a forbidden triple).

	neighbor_is_chest[i]       : is neighbour i a chest?
	neighbor_already_paired[i] : is neighbour i already part of a double chest?
	                             (only consulted where neighbor_is_chest[i]).
*/
bool chest_can_place(const bool neighbor_is_chest[4],
					 const bool neighbor_already_paired[4]);

/*
	Index (0..3) of the neighbour that forms the double chest, or -1 when the
	chest is single. If several neighbours are chests — only possible in a
	legacy/imported world predating the placement rule — the lowest index is
	chosen so the result is deterministic and never crashes.
*/
int chest_partner_index(const bool neighbor_is_chest[4]);

/*
	Canonical ordering of a pair: returns true when the chest at (ax,az) is the
	"primary" (first) half versus the chest at (bx,bz). Lexicographic by x then
	z, so both halves agree on which storage occupies window slots 0..26 vs
	27..53 regardless of which half the player actually opened.
*/
bool chest_is_primary(int ax, int az, int bx, int bz);

#endif
