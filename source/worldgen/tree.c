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

/*
	Beta 1.7.3 tree generation, ported from the WorldGenTrees / WorldGenForest /
	WorldGenTaiga* builders (cross-checked against the BetrockServer reference).

	The whole populate pass shares ONE per-chunk Random, so every draw here is
	load-bearing: the small builders (oak/birch/taiga/taiga-alt) draw directly
	from that shared stream, so their draw ORDER must match Beta exactly. The big
	tree is the one exception -- it pulls a single nextLong() from the shared
	stream and re-seeds its own Random for everything else, so its geometry never
	advances the shared stream.

	Wood/leaf metadata is the load-bearing-for-look part: oak = 0, birch = 2,
	spruce (both taiga variants) = 1.
*/

#include <math.h> // sin/cos/sqrt/floor for the big tree
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h> // abs

#include "../block/blocks_data.h"
#include "biome.h"
#include "java_random.h"
#include "jmath.h"
#include "populate.h"
#include "tree.h"

#define TREE_WORLD_HEIGHT 128
#define TREE_LOG BLOCK_LOG		 // 17
#define TREE_LEAVES BLOCK_LEAVES // 18

// Beta Block.opaqueCubeLookup[id]: the STATIC table every tree generator
// consults before writing a leaf (WorldGenTrees.java:55, WorldGenForest:55,
// WorldGenTaiga1:54, WorldGenTaiga2:59 `!Block.opaqueCubeLookup[getBlockId]`).
// It is filled ONCE, in Block's constructor [740c583 Block.java:151
// `opaqueCubeLookup[var1] = this.isOpaqueCube()`], and for LEAVES that virtual
// call lands in BlockLeaves.isOpaqueCube [:173-175 `!this.graphicsLevel`]
// BEFORE BlockLeavesBase's constructor assigns graphicsLevel [BlockLeavesBase
// .java:6-9] -- the field still reads false, so opaqueCubeLookup[18] == TRUE
// and stays true (RenderGlobal.java:188's setGraphicsLevel only flips the
// instance field, never the table). Consequence: a canopy NEVER overwrites an
// existing leaf, so where two trees overlap the FIRST tree's species metadata
// survives (mixed forests keep their birch/pine leaves). The old list had
// leaves as non-opaque -- copied from mc173 material.rs:167, which is wrong on
// this point -- and let a later oak/pine canopy stamp its metadata over a
// neighbour's leaves (same id, so block ids and the Random stream were never
// affected: the nextInt(2) corner roll sits in the left conjunct and is drawn
// before this test either way). The exact set only matters for what a leaf
// overwrites, so the canopy-relevant non-opaque blocks are enough.
static bool tree_is_opaque(uint8_t id) {
	switch(id) {
		case BLOCK_AIR:			// 0
		case BLOCK_SAPLING:		// 6
		case BLOCK_WATER_FLOW:	// 8
		case BLOCK_WATER_STILL: // 9
		case BLOCK_LAVA_FLOW:	// 10
		case BLOCK_LAVA_STILL:	// 11
		// BLOCK_LEAVES (18) is OPAQUE here -- see the citation above.
		case BLOCK_GLASS:		// 20
		case BLOCK_TALL_GRASS:	// 31
		case 32:				// dead bush
		case BLOCK_FLOWER:		// 37
		case BLOCK_ROSE:		// 38
		case BLOCK_BROWM_MUSHROOM: // 39
		case BLOCK_RED_MUSHROOM:   // 40
		case BLOCK_SLAB:		// 44
		case BLOCK_TORCH:		// 50
		case BLOCK_FIRE:		// 51
		case BLOCK_WOODEN_STAIRS: // 53
		case BLOCK_REDSTONE_WIRE: // 55
		case BLOCK_CROPS:		// 59
		case BLOCK_LADDER:		// 65
		case BLOCK_RAIL:		// 66
		case BLOCK_STONE_STAIRS: // 67
		case BLOCK_SNOW:		// 78 (snow layer)
		case BLOCK_ICE:			// 79
		case BLOCK_REED:		// 83 (sugar cane)
		case BLOCK_FENCE:		// 85
		case BLOCK_PORTAL:		// 90
			return false;
		default: return true;
	}
}

// WorldGenTrees (oak) and the birch variant. `birch` adds 1 to the height and
// uses leaf/log metadata 2. Draws, in order: nextInt(3) for the height (always),
// then -- only after the space + ground checks pass -- one nextInt(2) at each of
// the four corners of every canopy layer (the Beta short-circuit: the corner
// roll is reached only when |dx|==w AND |dz|==w). A failed tree consumes just
// the one height draw.
static bool tree_oak(struct wgen_world* w, struct java_random* rand, int px,
					 int py, int pz, bool birch) {
	int height = jrand_next_int_bound(rand, 3) + 4;
	if(birch)
		height++;

	if(py < 1 || py + height + 1 > TREE_WORLD_HEIGHT)
		return false;

	// Clear-space check: a +-width column (0 at the base, 1 normally, 2 over the
	// top three layers) must be air or leaves all the way up.
	for(int cy = py; cy <= py + 1 + height; cy++) {
		int width = 1;
		if(cy == py)
			width = 0;
		if(cy >= py + 1 + height - 2)
			width = 2;
		for(int cx = px - width; cx <= px + width; cx++) {
			for(int cz = pz - width; cz <= pz + width; cz++) {
				if(cy < 0 || cy >= TREE_WORLD_HEIGHT)
					return false;
				uint8_t bt = w->get(w->ctx, cx, cy, cz);
				if(bt != BLOCK_AIR && bt != TREE_LEAVES)
					return false;
			}
		}
	}

	uint8_t root = w->get(w->ctx, px, py - 1, pz);
	if((root != BLOCK_GRASS && root != BLOCK_DIRT)
	   || py >= TREE_WORLD_HEIGHT - height - 1)
		return false;

	wgen_set_plain(w, px, py - 1, pz, BLOCK_DIRT);

	uint8_t meta = birch ? 2 : 0;

	// Canopy: four layers, the top two of radius 2, the bottom two of radius 1.
	for(int oy = py - 3 + height; oy <= py + height; oy++) {
		int width_base = oy - (py + height);	// 0, -1, -2, -3 top-down
		int tree_width = 1 - width_base / 2;	// 1, 1, 2, 2 (C trunc == Java)
		for(int ox = px - tree_width; ox <= px + tree_width; ox++) {
			int x_leaf = ox - px;
			for(int oz = pz - tree_width; oz <= pz + tree_width; oz++) {
				int z_leaf = oz - pz;
				// Corner roll: only the four far corners can be skipped, and the
				// nextInt(2) is drawn ONLY there (the || short-circuits exactly
				// like Beta), then only kept on a non-top layer.
				if((abs(x_leaf) != tree_width || abs(z_leaf) != tree_width
					|| (jrand_next_int_bound(rand, 2) != 0 && width_base != 0))
				   && !tree_is_opaque(w->get(w->ctx, ox, oy, oz))) {
					if(birch) w->set(w->ctx, ox, oy, oz, TREE_LEAVES, meta);
					else wgen_set_plain(w, ox, oy, oz, TREE_LEAVES);
				}
			}
		}
	}

	// Trunk: replace air/leaves up the centre column.
	for(int h = 0; h < height; h++) {
		uint8_t fl = w->get(w->ctx, px, py + h, pz);
		if(fl == BLOCK_AIR || fl == TREE_LEAVES) {
			if(birch) w->set(w->ctx, px, py + h, pz, TREE_LOG, meta);
			else wgen_set_plain(w, px, py + h, pz, TREE_LOG);
		}
	}

	return true;
}

// WorldGenTaiga1: the tall pine (nextInt(5)+7). Three size draws up front
// (nextInt(5), nextInt(2), nextInt(var8+1)) -- always consumed -- then the
// space/ground checks (no draws). Leaves + trunk are spruce metadata 1, drawn
// deterministically. NB: despite the historical "taiga" name, this BODY is Beta
// WorldGenTaiga1 -- BlockSapling.growTree never uses it (it grows spruce saplings
// with Taiga2 = tree_taiga2 below). worldgen_trees picks it 1/3 of the time in the
// taiga biome, exactly matching Beta BiomeGenTaiga.
static bool tree_taiga1(struct wgen_world* w, struct java_random* rand, int px,
					   int py, int pz) {
	int total = jrand_next_int_bound(rand, 5) + 7;			   // 7..11
	int bare = total - jrand_next_int_bound(rand, 2) - 3;	   // leaves start
	int leaf_span = total - bare;
	int max_radius = 1 + jrand_next_int_bound(rand, leaf_span + 1);

	if(py < 1 || py + total + 1 > TREE_WORLD_HEIGHT)
		return false;

	for(int cy = py; cy <= py + 1 + total; cy++) {
		int radius = ((cy - py) < bare) ? 0 : max_radius;
		for(int cx = px - radius; cx <= px + radius; cx++) {
			for(int cz = pz - radius; cz <= pz + radius; cz++) {
				if(cy < 0 || cy >= TREE_WORLD_HEIGHT)
					return false;
				uint8_t bt = w->get(w->ctx, cx, cy, cz);
				if(bt != BLOCK_AIR && bt != TREE_LEAVES)
					return false;
			}
		}
	}

	uint8_t root = w->get(w->ctx, px, py - 1, pz);
	if((root != BLOCK_GRASS && root != BLOCK_DIRT)
	   || py >= TREE_WORLD_HEIGHT - total - 1)
		return false;

	wgen_set_plain(w, px, py - 1, pz, BLOCK_DIRT);

	int radius = 0;
	for(int cy = py + total; cy >= py + bare; cy--) {
		for(int cx = px - radius; cx <= px + radius; cx++) {
			int dx = cx - px;
			for(int cz = pz - radius; cz <= pz + radius; cz++) {
				int dz = cz - pz;
				if(abs(dx) != radius || abs(dz) != radius || radius <= 0) {
					if(!tree_is_opaque(w->get(w->ctx, cx, cy, cz)))
						w->set(w->ctx, cx, cy, cz, TREE_LEAVES, 1);
				}
			}
		}
		if(radius >= 1 && cy == py + bare + 1)
			radius--;
		else if(radius < max_radius)
			radius++;
	}

	for(int h = 0; h < total - 1; h++) {
		uint8_t fl = w->get(w->ctx, px, py + h, pz);
		if(fl == BLOCK_AIR || fl == TREE_LEAVES)
			w->set(w->ctx, px, py + h, pz, TREE_LOG, 1);
	}

	return true;
}

// WorldGenTaiga2: the conical spruce (nextInt(4)+6). Beta BlockSapling.growTree
// uses THIS variant for a spruce sapling (meta&3==1). Three size draws up front
// (nextInt(4), nextInt(2), nextInt(2)); then -- ONLY if the space + ground checks
// pass -- two more draws (nextInt(2) for the initial canopy radius, nextInt(3) for
// the trunk-top trim). The two trailing draws being gated behind the checks is a
// real asymmetry vs the other builders. Spruce metadata 1.
static bool tree_taiga2(struct wgen_world* w, struct java_random* rand,
						   int px, int py, int pz) {
	int total = jrand_next_int_bound(rand, 4) + 6;	   // 6..9
	int bare = 1 + jrand_next_int_bound(rand, 2);	   // 1..2
	int leaf_span = total - bare;
	int max_radius = 2 + jrand_next_int_bound(rand, 2); // 2..3

	if(py < 1 || py + total + 1 > TREE_WORLD_HEIGHT)
		return false;

	for(int cy = py; cy <= py + 1 + total; cy++) {
		int radius = ((cy - py) < bare) ? 0 : max_radius;
		for(int cx = px - radius; cx <= px + radius; cx++) {
			for(int cz = pz - radius; cz <= pz + radius; cz++) {
				if(cy < 0 || cy >= TREE_WORLD_HEIGHT)
					return false;
				uint8_t bt = w->get(w->ctx, cx, cy, cz);
				if(bt != BLOCK_AIR && bt != TREE_LEAVES)
					return false;
			}
		}
	}

	uint8_t root = w->get(w->ctx, px, py - 1, pz);
	if((root != BLOCK_GRASS && root != BLOCK_DIRT)
	   || py >= TREE_WORLD_HEIGHT - total - 1)
		return false;

	wgen_set_plain(w, px, py - 1, pz, BLOCK_DIRT);

	int radius = jrand_next_int_bound(rand, 2); // GATED draw #4
	int radius_step = 1;
	int held = 0;
	for(int layer = 0; layer <= leaf_span; layer++) {
		int cy = py + total - layer;
		for(int cx = px - radius; cx <= px + radius; cx++) {
			int dx = cx - px;
			for(int cz = pz - radius; cz <= pz + radius; cz++) {
				int dz = cz - pz;
				if(abs(dx) != radius || abs(dz) != radius || radius <= 0) {
					if(!tree_is_opaque(w->get(w->ctx, cx, cy, cz)))
						w->set(w->ctx, cx, cy, cz, TREE_LEAVES, 1);
				}
			}
		}
		if(radius >= radius_step) {
			radius = held;
			held = 1;
			radius_step++;
			if(radius_step > max_radius)
				radius_step = max_radius;
		} else {
			radius++;
		}
	}

	int trim = jrand_next_int_bound(rand, 3); // GATED draw #5
	for(int h = 0; h < total - trim; h++) {
		uint8_t fl = w->get(w->ctx, px, py + h, pz);
		if(fl == BLOCK_AIR || fl == TREE_LEAVES)
			w->set(w->ctx, px, py + h, pz, TREE_LOG, 1);
	}

	return true;
}

// WorldGenBigTree (large oak). It pulls exactly ONE nextLong() from the shared
// populate Random, re-seeds its OWN Random, and draws everything else from that
// -- so its whole geometry never advances the shared stream (the populate
// sequence is byte-exact regardless of what the big tree does internally). The
// builder below is therefore a faithful structural port, not a draw-exact one:
// the shape matches Beta 1.7.3 WorldGenBigTree, but sin/cos/floor ULP differences
// are harmless since they don't touch the shared stream.
//
// Beta constants for the populate call BigTree(1,1,1): trunk thickness 5, max
// height 12, height factor 0.618, branch slope 0.381, single-column trunk
// (vanilla 1.7.3 branchDensity is 1, so its 2-wide-trunk path is dead code).
#define BT_TRUNK_THICKNESS 5
#define BT_MAX_HEIGHT 12
#define BT_HEIGHT_FACTOR 0.618
#define BT_BRANCH_SLOPE 0.381
#define BT_MAX_BRANCHES 64 // >= branchesPerLayer(<=2) * height(<=16) + slack

// branchOrientation: for a dominant axis, its two in-plane axes. X->{Z,Y},
// Y->{X,Z}, Z->{X,Y} (verbatim from the reference; not modular).
static const int bt_orient[6] = {2, 0, 0, 1, 2, 1};

struct bt_node {
	int pos[3];
	int trunk_y;
};

struct bigtree {
	struct wgen_world* w;
	struct java_random rand; // the tree's OWN Random (re-seeded, NOT the stream)
	int base[3];
	int total_height;
	int height; // trunk top, relative to base
	struct bt_node nodes[BT_MAX_BRANCHES];
	int node_count;
};

// floor_double(x) = true floor; java_double_to_int (jmath) = truncate toward 0.
static int bt_floori(double v) {
	return (int)floor(v);
}

static float bt_canopy_radius(struct bigtree* t, int y) {
	if((double)y < (double)((float)t->total_height) * 0.3)
		return -1.618f;
	float half = (float)t->total_height / 2.0f;
	float dist = (float)t->total_height / 2.0f - (float)y;
	float r;
	if(dist == 0.0f)
		r = half;
	else if(fabsf(dist) >= half)
		r = 0.0f;
	else
		r = (float)sqrt((double)(half * half) - (double)(dist * dist));
	return r * 0.5f;
}

static float bt_trunk_radius(int layer) {
	if(layer < 0 || layer >= BT_TRUNK_THICKNESS)
		return -1.0f;
	if(layer == 0 || layer == BT_TRUNK_THICKNESS - 1)
		return 2.0f;
	return 3.0f;
}

// Walk a straight line start->end (Beta's dominant-axis stepping). Returns -1 if
// every cell is air/leaves, else the clear length before the first obstruction.
static int bt_check_clear(struct bigtree* t, const int* start, const int* end) {
	int delta[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
	int dom = 0;
	for(int a = 0; a < 3; a++)
		if(abs(delta[a]) > abs(delta[dom]))
			dom = a;
	if(delta[dom] == 0)
		return -1;
	int sa = bt_orient[dom], sb = bt_orient[dom + 3];
	int step = delta[dom] > 0 ? 1 : -1;
	double ra = (double)delta[sa] / (double)delta[dom];
	double rb = (double)delta[sb] / (double)delta[dom];
	int total = delta[dom] + step, d = 0, p[3];
	for(; d != total; d += step) {
		p[dom] = start[dom] + d;
		p[sa] = bt_floori((double)start[sa] + (double)d * ra);
		p[sb] = bt_floori((double)start[sb] + (double)d * rb);
		uint8_t b = t->w->get(t->w->ctx, p[0], p[1], p[2]);
		if(b != BLOCK_AIR && b != TREE_LEAVES)
			break;
	}
	return (d == total) ? -1 : abs(d);
}

static void bt_draw_line(struct bigtree* t, const int* start, const int* end,
						 uint8_t block) {
	int delta[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
	int dom = 0;
	for(int a = 0; a < 3; a++)
		if(abs(delta[a]) > abs(delta[dom]))
			dom = a;
	if(delta[dom] == 0)
		return;
	int sa = bt_orient[dom], sb = bt_orient[dom + 3];
	int step = delta[dom] > 0 ? 1 : -1;
	double ra = (double)delta[sa] / (double)delta[dom];
	double rb = (double)delta[sb] / (double)delta[dom];
	int total = delta[dom] + step, d = 0, p[3];
	for(; d != total; d += step) {
		p[dom] = bt_floori((double)(start[dom] + d) + 0.5);
		p[sa] = bt_floori((double)start[sa] + (double)d * ra + 0.5);
		p[sb] = bt_floori((double)start[sb] + (double)d * rb + 0.5);
		wgen_set_plain(t->w, p[0], p[1], p[2], block); // big oak = meta 0
	}
}

// A filled disc of `block` in the plane normal to `axis`, over air/leaves only
// [740c583 WorldGenBigTree.java:114-118: a cell that is neither air (0) nor leaves (18) is skipped,
// otherwise it is set to the disc block]. Beta's setBlock is Chunk.setBlockID, which returns
// before writing anything when the id is unchanged [Chunk.java:279, an unchanged id
// returns false] -- so a LEAF disc cell that already holds a leaf keeps
// its species bits (a big oak over a birch canopy leaves the birch leaves
// birch) and, on the live-world growth path, never fires the metadata-change
// lifecycle (no spurious BlockLeaves.onBlockRemoval flags). The old
// unconditional set(LEAVES, 0) reset both. Block ids and the Random stream are
// unaffected (same id either way; no draw here).
static void bt_circular_layer(struct bigtree* t, const int* center, float radius,
							  int axis, uint8_t block) {
	int ir = java_double_to_int((double)radius + 0.618);
	int au = bt_orient[axis], av = bt_orient[axis + 3];
	int p[3];
	for(int du = -ir; du <= ir; du++) {
		p[axis] = center[axis];
		p[au] = center[au] + du;
		for(int dv = -ir; dv <= ir; dv++) {
			double su = (double)abs(du) + 0.5, sv = (double)abs(dv) + 0.5;
			if(sqrt(su * su + sv * sv) > (double)radius)
				continue;
			p[av] = center[av] + dv;
			uint8_t other = t->w->get(t->w->ctx, p[0], p[1], p[2]);
			/* CFR WorldGenBigTree.java:105-110 attempts the plain setter even
			 * over existing leaves. Chunk.setBlockID owns the no-op decision. */
			if(other == BLOCK_AIR || other == TREE_LEAVES)
				wgen_set_plain(t->w, p[0], p[1], p[2], block);
		}
	}
}

static void bt_leaves_around(struct bigtree* t, const int* base) {
	for(int y = base[1]; y < base[1] + BT_TRUNK_THICKNESS; y++) {
		int c[3] = {base[0], y, base[2]};
		bt_circular_layer(t, c, bt_trunk_radius(y - base[1]), 1 /*AXIS_Y*/,
						  TREE_LEAVES);
	}
}

static bool bt_valid(struct bigtree* t) {
	uint8_t below
		= t->w->get(t->w->ctx, t->base[0], t->base[1] - 1, t->base[2]);
	if(below != BLOCK_GRASS && below != BLOCK_DIRT)
		return false;
	int end[3] = {t->base[0], t->base[1] + t->total_height - 1, t->base[2]};
	int clear = bt_check_clear(t, t->base, end);
	if(clear == -1)
		return true; // fully clear
	if(clear < 6)
		return false; // too short
	t->total_height = clear;
	return true;
}

static void bt_branch_positions(struct bigtree* t) {
	t->height = java_double_to_int((double)t->total_height * BT_HEIGHT_FACTOR);
	if(t->height >= t->total_height)
		t->height = t->total_height - 1;

	// trunkShape is 1.0, so this is 1.382 + (totalHeight/13)^2.
	int per_layer = java_double_to_int(
		1.382 + pow((double)t->total_height / 13.0, 2.0));
	if(per_layer < 1)
		per_layer = 1;

	int cur_y = t->base[1] + t->total_height - BT_TRUNK_THICKNESS;
	int target_y = t->base[1] + t->height;
	int layer = cur_y - t->base[1];
	t->node_count = 1;
	t->nodes[0].pos[0] = t->base[0];
	t->nodes[0].pos[1] = cur_y;
	t->nodes[0].pos[2] = t->base[2];
	t->nodes[0].trunk_y = target_y;
	cur_y--;

	for(; layer >= 0; layer--) {
		float cr = bt_canopy_radius(t, layer);
		if(cr >= 0.0f) {
			for(int att = 0; att < per_layer; att++) {
				// branchLength is 1.0.
				double radial
					= (double)cr * ((double)jrand_next_float(&t->rand) + 0.328);
				double angle = (double)jrand_next_float(&t->rand) * 2.0 * 3.14159;
				int bx = bt_floori(radial * sin(angle) + (double)t->base[0] + 0.5);
				int bz = bt_floori(radial * cos(angle) + (double)t->base[2] + 0.5);
				int bbase[3] = {bx, cur_y, bz};
				int btop[3] = {bx, cur_y + BT_TRUNK_THICKNESS, bz};
				if(bt_check_clear(t, bbase, btop) != -1)
					continue;
				double horiz
					= sqrt((double)(abs(t->base[0] - bx) * abs(t->base[0] - bx))
						   + (double)(abs(t->base[2] - bz)
									  * abs(t->base[2] - bz)));
				double vdrop = horiz * BT_BRANCH_SLOPE;
				int trunk_y;
				if((double)cur_y - vdrop > (double)target_y)
					trunk_y = target_y;
				else
					trunk_y = java_double_to_int((double)cur_y - vdrop);
				int trunk_conn[3] = {t->base[0], trunk_y, t->base[2]};
				if(bt_check_clear(t, trunk_conn, bbase) != -1)
					continue;
				if(t->node_count < BT_MAX_BRANCHES) {
					t->nodes[t->node_count].pos[0] = bx;
					t->nodes[t->node_count].pos[1] = cur_y;
					t->nodes[t->node_count].pos[2] = bz;
					t->nodes[t->node_count].trunk_y = trunk_y;
					t->node_count++;
				}
			}
		}
		cur_y--;
	}
}

static bool tree_big(struct wgen_world* w, struct java_random* rand, int px,
					 int py, int pz) {
	struct bigtree t;
	t.w = w;
	int64_t seed = jrand_next_long(rand); // the ONE shared-stream draw
	jrand_set_seed(&t.rand, seed);		  // everything else is internal
	t.base[0] = px;
	t.base[1] = py;
	t.base[2] = pz;
	t.total_height = 5 + jrand_next_int_bound(&t.rand, BT_MAX_HEIGHT);

	if(!bt_valid(&t))
		return false;

	bt_branch_positions(&t);

	// Leaf clusters around every branch node...
	for(int i = 0; i < t.node_count; i++)
		bt_leaves_around(&t, t.nodes[i].pos);

	// ...the central trunk...
	int trunk_end[3] = {t.base[0], t.base[1] + t.height, t.base[2]};
	bt_draw_line(&t, t.base, trunk_end, TREE_LOG);

	// ...and a log line from the trunk out to each high-enough branch node.
	for(int i = 0; i < t.node_count; i++) {
		int rel = t.nodes[i].trunk_y - t.base[1];
		if((double)rel >= (double)t.total_height * 0.2) {
			int attach[3] = {t.base[0], t.nodes[i].trunk_y, t.base[2]};
			bt_draw_line(&t, attach, t.nodes[i].pos, TREE_LOG);
		}
	}
	return true;
}

enum tree_kind {
	TREE_KIND_OAK,
	TREE_KIND_BIRCH,
	TREE_KIND_BIG,
	TREE_KIND_TAIGA,
	TREE_KIND_TAIGA_ALT,
};

// Beta Block.lightOpacity[id] == 0 (Block.java:152 `isOpaqueCube() ? 255 : 0`,
// then the setLightOpacity overrides: water 3, lava 255, leaves 1, web 1, ice 3,
// farmland / stairs / step 255 -- all NON-zero, so they stop the scan). The
// zero set is every block whose isOpaqueCube() is false and that never calls
// setLightOpacity: sapling, glass (BlockBreakable), bed, rails, pistons, the
// BlockFlower family (tall grass, dead bush, flowers, mushrooms), torches,
// fire, mob spawner (BlockMobSpawner.isOpaqueCube false), wire, crops, signs,
// doors, ladder, lever, pressure plates, button, snow layer, cactus, reed,
// fence, portal, cake, repeaters, trapdoor. Independent of the runtime block
// table on purpose: worldgen must not inherit a render/lighting deviation there.
static bool tree_beta_light_opacity_zero(uint8_t id) {
	switch(id) {
		case BLOCK_AIR:
		case BLOCK_SAPLING:
		case BLOCK_GLASS:
		case BLOCK_BED:
		case BLOCK_POWERED_RAIL:
		case BLOCK_DETECTOR_RAIL:
		case BLOCK_PISTON_STICKY:
		case BLOCK_TALL_GRASS:
		case 32: // dead bush
		case BLOCK_PISTON_BASE:
		case BLOCK_PISTON_HEAD:
		case BLOCK_PISTON_MOVING:
		case BLOCK_FLOWER:
		case BLOCK_ROSE:
		case BLOCK_BROWM_MUSHROOM:
		case BLOCK_RED_MUSHROOM:
		case BLOCK_TORCH:
		case BLOCK_FIRE:
		case BLOCK_MOB_SPAWNER:
		case BLOCK_REDSTONE_WIRE:
		case BLOCK_CROPS:
		case BLOCK_SIGN_POST:
		case BLOCK_DOOR_WOOD:
		case BLOCK_LADDER:
		case BLOCK_RAIL:
		case BLOCK_SIGN:
		case BLOCK_LEVER:
		case BLOCK_STONE_PRESSURE_PLATE:
		case BLOCK_DOOR_IRON:
		case BLOCK_WOOD_PRESSURE_PLATE:
		case BLOCK_REDSTONE_TORCH:
		case BLOCK_REDSTONE_TORCH_LIT:
		case BLOCK_STONE_BUTTON:
		case BLOCK_SNOW:
		case BLOCK_CACTUS:
		case BLOCK_REED:
		case BLOCK_FENCE:
		case BLOCK_PORTAL:
		case BLOCK_CAKE:
		case BLOCK_REDSTONE_REPEATER:
		case BLOCK_REDSTONE_REPEATER_LIT:
		case BLOCK_TRAP_DOOR: return true;
		default: return false;
	}
}

// Beta World.getHeightValue(x, z) as populate sees it [World.java:603-614 ->
// Chunk.getHeightValue:60-62]. The chunk heightMap starts as a top-down scan
// for the first block with lightOpacity != 0 (Chunk.java:67-86 / the skylight
// generator :88-104: var4 from 127, test block var4-1) and Chunk.setBlockID
// [:236-273, :275-] keeps it exact on EVERY write (func_1003_g, :158-: rescan
// when an opacity!=0 block lands at/above the height or an opacity-0 block
// replaces the top cell). So at every read it equals a fresh scan over the
// CURRENT blocks -- including this populate's own earlier writes (lakes,
// dungeons, earlier trees' logs and leaves). The world-view's height() hook is
// NOT used: the live quadrant's hook returns the terrain-time heightmap, which
// no populate write updates (server_world.c quad_height / quad_set), and so
// placed trees under earlier canopies (laneC/TREE-HEIGHTMAP 2026-09-26,
// tests/test_tree_heightmap_live.c).
static int tree_height_value(struct wgen_world* w, int x, int z) {
	int y = 127;
	while(y > 0 && tree_beta_light_opacity_zero(w->get(w->ctx, x, y - 1, z)))
		y--;
	return y;
}

void worldgen_trees(struct wgen_world* w, struct java_random* rand,
					enum biome corner_biome, double tree_density, int block_x,
					int block_z) {
	// The biome at the chunk's +16,+16 corner (Beta's getBiomeGenAt) and the
	// treeDensity octave sample are computed by the caller from the generator;
	// this stays free of the generator so it links into the worldgen unit tests.
	enum biome b = corner_biome;

	// Tree count (ChunkProviderGenerate.populate). The density sample and the
	// nextInt(10) bump are ALWAYS drawn, even for biomes that discard them, so
	// the Random advances identically regardless of biome.
	int sample = java_double_to_int(
		(tree_density / 8.0 + jrand_next_double(rand) * 4.0 + 4.0) / 3.0);

	int count = 0;
	if(jrand_next_int_bound(rand, 10) == 0)
		count++;

	switch(b) {
		case BIOME_FOREST:
		case BIOME_RAINFOREST:
		case BIOME_TAIGA: count += sample + 5; break;
		case BIOME_SEASONAL_FOREST: count += sample + 2; break;
		case BIOME_DESERT:
		case BIOME_TUNDRA:
		case BIOME_PLAINS: count -= 20; break;
		default: break;
	}

	for(int i = 0; i < count; i++) {
		int x = block_x + jrand_next_int_bound(rand, 16) + 8;
		int z = block_z + jrand_next_int_bound(rand, 16) + 8;
		int y = tree_height_value(w, x, z);

		enum tree_kind kind;
		switch(b) {
			case BIOME_FOREST:
				if(jrand_next_int_bound(rand, 5) == 0)
					kind = TREE_KIND_BIRCH;
				else
					kind = (jrand_next_int_bound(rand, 3) == 0) ? TREE_KIND_BIG
																: TREE_KIND_OAK;
				break;
			case BIOME_RAINFOREST:
				kind = (jrand_next_int_bound(rand, 3) == 0) ? TREE_KIND_BIG
															: TREE_KIND_OAK;
				break;
			case BIOME_TAIGA:
				kind = (jrand_next_int_bound(rand, 3) == 0) ? TREE_KIND_TAIGA
															: TREE_KIND_TAIGA_ALT;
				break;
			default:
				kind = (jrand_next_int_bound(rand, 10) == 0) ? TREE_KIND_BIG
															 : TREE_KIND_OAK;
				break;
		}

		switch(kind) {
			case TREE_KIND_OAK: tree_oak(w, rand, x, y, z, false); break;
			case TREE_KIND_BIRCH: tree_oak(w, rand, x, y, z, true); break;
			case TREE_KIND_BIG: tree_big(w, rand, x, y, z); break;
			case TREE_KIND_TAIGA: tree_taiga1(w, rand, x, y, z); break;
			case TREE_KIND_TAIGA_ALT: tree_taiga2(w, rand, x, y, z); break;
		}
	}
}

// Beta 1.7.3 BlockSapling.growTree (see tree.h). Species == meta & 3:
//   1 -> WorldGenTaiga2 (tree_taiga2, the conical spruce, meta 1)
//   2 -> WorldGenForest  (tree_oak birch=true, +1 height, meta 2)
//   else WorldGenTrees   (tree_oak oak, meta 0), upgraded 1-in-10 to the big oak.
// The sapling is cleared to air first -- the builders' clear-space check accepts
// only air/leaves at the trunk base, so a still-present sapling (id 6, neither)
// would always fail it -- and restored on a failed generate. NB: Beta growTree
// uses WorldGenTaiga2 (tree_taiga2) for spruce, NOT the taller WorldGenTaiga1
// (tree_taiga1) -- the tall pine is a taiga-population-only variant growTree never
// reaches. (The two builders' internal names were historically inverted vs Beta;
// trusting the old name is exactly what made the first cut of this grow a Taiga1.)
bool worldgen_grow_sapling(struct wgen_world* w, struct java_random* rand,
						   int sapling_meta, int px, int py, int pz) {
	int species = sapling_meta & 3;
	wgen_set_plain(w, px, py, pz, BLOCK_AIR); // Beta setBlock(x, y, z, 0)

	bool ok;
	if(species == 1) {
		ok = tree_taiga2(w, rand, px, py, pz);
	} else if(species == 2) {
		ok = tree_oak(w, rand, px, py, pz, true);
	} else {
		if(jrand_next_int_bound(rand, 10) == 0)
			ok = tree_big(w, rand, px, py, pz);
		else
			ok = tree_oak(w, rand, px, py, pz, false);
	}

	if(!ok) // Beta setBlockAndMetadata(x, y, z, sapling, meta & 3)
		w->set(w->ctx, px, py, pz, BLOCK_SAPLING, (uint8_t)species);
	return ok;
}
