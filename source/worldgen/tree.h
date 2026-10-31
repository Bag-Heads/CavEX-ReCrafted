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

#ifndef WORLDGEN_TREE_H
#define WORLDGEN_TREE_H

#include <stdbool.h>

#include "biome.h"

struct wgen_world;
struct java_random;

/*
	Beta 1.7.3 tree population -- the TREES step of ChunkProviderGenerate's
	populate(), run after the ore passes share the per-chunk decorate Random.

	It samples the biome at the chunk's +16,+16 corner (Beta's getBiomeGenAt,
	via `gen`) plus the treeDensity octave noise, derives the per-chunk tree
	count exactly as Beta does, and for each tree picks a biome-appropriate type
	and places it. The draw order is load-bearing -- every later decoration
	depends on the Random being advanced by the exact same amount.

	`corner_biome` is the biome at the chunk's +16,+16 corner and `tree_density`
	the treeDensity octave sample at the block origin -- both computed by the
	caller from the generator (this stays generator-free so it links into the
	worldgen unit tests). block_x/block_z are the chunk's block origin.
*/
void worldgen_trees(struct wgen_world* w, struct java_random* rand,
					enum biome corner_biome, double tree_density, int block_x,
					int block_z);

/*
	Beta 1.7.3 BlockSapling.growTree: grow the sapling at (px,py,pz) into a tree,
	species selected by `sapling_meta` & 3 exactly as Beta -- 1 = spruce
	(WorldGenTaiga2), 2 = birch (WorldGenForest, the +1-height oak variant), else
	oak (WorldGenTrees) with a 1-in-10 upgrade to the big oak (WorldGenBigTree).
	Beta clears the sapling to air FIRST (so the builder's clear-space check, which
	accepts only air/leaves, passes at the trunk base) and restores it on a failed
	generate; every draw comes from the single `rand` stream, mirroring Beta's use
	of world.rand (the nextInt(10) big-tree roll, then the builder's own draws).

	This is the pure worldgen-side entry (flat-buffer testable, no server deps); the
	runtime bridge that drives it over the live server world is server_grow_tree
	(tree_runtime.h). Returns whether a tree generated.
*/
bool worldgen_grow_sapling(struct wgen_world* w, struct java_random* rand,
						   int sapling_meta, int px, int py, int pz);

#endif
