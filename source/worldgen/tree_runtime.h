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

#ifndef WORLDGEN_TREE_RUNTIME_H
#define WORLDGEN_TREE_RUNTIME_H

#include <stdbool.h>

struct server_local;

/*
	Runtime bridge for Beta 1.7.3 BlockSapling.growTree: grow the sapling at
	(x,y,z) -- whose species is `sapling_meta` & 3 -- into a tree in the LIVE
	server world. Wraps server_world_get/set_block in the worldgen `wgen_world`
	abstraction and drives worldgen_grow_sapling (tree.h) with a java_random
	seeded from the server's runtime RNG (s->rand_src).

	Faithfulness note: the runtime RNG (util.c) is xorshift32, NOT the Java LCG,
	and Beta's own runtime growth depends on the non-reproducible world.rand state
	at the moment of growth -- so this is deliberately NOT byte-exact to a Beta
	world.rand draw stream (nor can it be). The faithful target is the resulting
	TREE GEOMETRY + species/big-tree selection + restore-on-fail, which the seeded
	java_random reproduces deterministically (byte-exact geometry is pinned by
	tests/test_grow_tree.c on the pure worldgen entry). Shared by the bone-meal
	use (item_dye.c) and the random-tick growth (block_sapling.c).

	Returns whether a tree generated.
*/
bool server_grow_tree(struct server_local* s, int x, int y, int z,
					  int sapling_meta);

#endif
