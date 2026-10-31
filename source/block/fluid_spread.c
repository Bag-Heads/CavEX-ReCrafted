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

#include <stddef.h>

#include "fluid_spread.h"

// Beta searches up to 4 blocks for a downhill drop when choosing flow directions.
#define MAX_FLOW_DIST 4
// metadata bit 0x8 marks a falling (vertical) column.
#define FALLING 0x8

// Horizontal directions: -X, +X, -Z, +Z. opp(d) = d ^ 1 (the reverse).
static const int DX[4] = {-1, 1, 0, 0};
static const int DZ[4] = {0, 0, -1, 1};

// What distinguishes one fluid from another: its block ids, how many levels it
// loses per horizontal step (water 1 -> reaches 7; lava 2 -> reaches 3 in the
// overworld), and whether two adjacent sources form a new (infinite) source.
struct fluid_kind {
	uint8_t flow;
	uint8_t still;
	int step;
	bool infinite;
};
static const struct fluid_kind WATER = {.flow = FLUID_WATER_FLOW,
										.still = FLUID_WATER_STILL,
										.step = 1,
										.infinite = true};
static const struct fluid_kind LAVA = {.flow = FLUID_LAVA_FLOW,
									   .still = FLUID_LAVA_STILL,
									   .step = 2,
									   .infinite = false};

static bool is_same(uint8_t t, const struct fluid_kind* k) {
	return t == k->flow || t == k->still;
}
// Beta getFlowDecay: RAW metadata for this fluid, or -1 for another material.
// Do not normalize falling metadata here: BlockFlowing preserves 8..15 through
// vertical columns and uses raw zero (not effective decay zero) as the source gate.
static int raw_decay(const struct fluid_world* w, int x, int y, int z,
					 const struct fluid_kind* k) {
	uint8_t m;
	uint8_t t = w->get(w->ctx, x, y, z, &m);
	if (!is_same(t, k))
		return -1;
	return m;
}

// Beta getSmallestFlowDecay: fold one neighbour's level into `smallest`, counting
// it as a source only if its metadata is exactly 0 (a falling column reads decay
// 0 for levelling but is NOT a source).
static int smallest_decay(const struct fluid_world* w, int x, int y, int z,
						  int smallest, int* n_sources,
						  const struct fluid_kind* k) {
	uint8_t m;
	uint8_t t = w->get(w->ctx, x, y, z, &m);
	if (!is_same(t, k))
		return smallest;
	if (m == 0)
		(*n_sources)++;
	int d = (m >= 8) ? 0 : m;
	if (smallest >= 0 && d >= smallest)
		return smallest;
	return d;
}

static bool block_blocks_flow(const struct fluid_world* w, int x, int y,
							  int z) {
	uint8_t m;
	uint8_t t = w->get(w->ctx, x, y, z, &m);
	return w->blocks_flow(w->ctx, t);
}

// Beta's path search is less restrictive than final displacement: a same-fluid
// non-source cell is traversable, even though flowIntoBlock later refuses to
// overwrite it. This quirk affects which tied direction wins near existing pools.
static bool path_can_enter(const struct fluid_world* w, int x, int y, int z,
						   const struct fluid_kind* k) {
	uint8_t m;
	uint8_t t = w->get(w->ctx, x, y, z, &m);
	return !w->blocks_flow(w->ctx, t) && (!is_same(t, k) || m != 0);
}

// Beta liquidCanDisplaceBlock is asymmetric: neither fluid overwrites itself;
// no fluid overwrites lava; lava may overwrite water.
static bool liquid_can_displace(const struct fluid_world* w, int x, int y,
								int z, const struct fluid_kind* k,
								uint8_t* type, uint8_t* meta) {
	uint8_t m;
	uint8_t t = w->get(w->ctx, x, y, z, &m);
	if (type)
		*type = t;
	if (meta)
		*meta = m;
	if (is_same(t, k) || t == FLUID_LAVA_FLOW || t == FLUID_LAVA_STILL)
		return false;
	return !w->blocks_flow(w->ctx, t);
}

static bool material_is_solid(const struct fluid_world* w, uint8_t type) {
	return w->material_is_solid ?
		w->material_is_solid(w->ctx, type) :
		w->blocks_flow(w->ctx, type);
}

static int next_int(const struct fluid_world* w, int bound) {
	return w->next_int ? w->next_int(w->ctx, bound) : 0;
}

// Beta calculateFlowCost: recursive search for the distance to the nearest cell
// from which the fluid can fall, never reversing `last_dir`. 1000 == no drop.
static int flow_cost(const struct fluid_world* w, int x, int y, int z, int dist,
					 int last_dir, const struct fluid_kind* k) {
	int cost = 1000;
	for (int d = 0; d < 4; d++) {
		if (d == (last_dir ^ 1))
			continue;
		int nx = x + DX[d], nz = z + DZ[d];
		if (!path_can_enter(w, nx, y, nz, k))
			continue;
		if (!block_blocks_flow(w, nx, y - 1, nz))
			return dist;
		if (dist < MAX_FLOW_DIST) {
			int c = flow_cost(w, nx, y, nz, dist + 1, d, k);
			if (c < cost)
				cost = c;
		}
	}
	return cost;
}

// Beta getOptimalFlowDirections: the horizontal directions whose path to a drop
// is shortest (ties all flow). Directions with no path share cost 1000.
static void optimal_dirs(const struct fluid_world* w, int x, int y, int z,
						 bool out[4], const struct fluid_kind* k) {
	int cost[4];
	int min = 1000;
	for (int d = 0; d < 4; d++) {
		int nx = x + DX[d], nz = z + DZ[d];
		if (!path_can_enter(w, nx, y, nz, k)) {
			cost[d] = 1000;
			continue;
		}
		cost[d] = !block_blocks_flow(w, nx, y - 1, nz) ?
			0 :
			flow_cost(w, nx, y, nz, 1, d, k);
		if (cost[d] < min)
			min = cost[d];
	}
	for (int d = 0; d < 4; d++)
		out[d] = (cost[d] == min);
}

// Beta flowIntoBlock: place flowing fluid at (x,y,z) with `meta`, if it can
// enter. Returns whether it actually placed anything.
static bool flow_into(const struct fluid_world* w, int x, int y, int z, int meta,
					  const struct fluid_kind* k) {
	uint8_t old_meta, old_type;
	if (!liquid_can_displace(w, x, y, z, k, &old_type, &old_meta))
		return false;
	if (old_type > 0) {
		if (k == &LAVA) {
			if (w->lava_mix)
				w->lava_mix(w->ctx, x, y, z);
		} else if (w->drop_block) {
			w->drop_block(w->ctx, x, y, z, old_type, old_meta);
		}
	}
	w->set(w->ctx, x, y, z, k->flow, (uint8_t)meta);
	return true;
}

enum fluid_tick_phase {
	FLUID_TICK_START,
	FLUID_TICK_AFTER_SELF_SET,
	FLUID_TICK_HORIZONTAL,
	FLUID_TICK_DONE,
};

enum fluid_tick_kind {
	FLUID_TICK_WATER,
	FLUID_TICK_LAVA,
};

void fluid_tick_state_init(struct fluid_tick_state* state, int x, int y, int z,
						   bool lava, bool nether) {
	*state = (struct fluid_tick_state) {
		.x = x,
		.y = y,
		.z = z,
		.phase = FLUID_TICK_START,
		.kind = lava ? FLUID_TICK_LAVA : FLUID_TICK_WATER,
		.nether = nether,
	};
}

bool fluid_tick_state_step(const struct fluid_world* w,
						  struct fluid_tick_state* state) {
	const struct fluid_kind* k
		= state->kind == FLUID_TICK_LAVA ? &LAVA : &WATER;
	const int x = state->x, y = state->y, z = state->z;
	/* Beta: lava loses one level in Hell and two outside it; water is one. */
	const int step = (k == &LAVA && state->nether) ? 1 : k->step;

	if(state->phase == FLUID_TICK_START) {
		uint8_t m0;
		uint8_t t0 = w->get(w->ctx, x, y, z, &m0);
		if(!is_same(t0, k)) {
			state->phase = FLUID_TICK_DONE;
			return true;
		}

		/* BlockFlowing.updateTick below stays in recovered-decompile order.
		   `decay` is raw metadata; normalize only inside smallest_decay. */
		int decay = m0;
		bool settle = true;
		bool set_self = false;
		uint8_t set_type = FLUID_AIR;
		uint8_t set_meta = 0;
		if(decay > 0) {
			int n_sources = 0;
			int smallest = -100;
			smallest
				= smallest_decay(w, x - 1, y, z, smallest, &n_sources, k);
			smallest
				= smallest_decay(w, x + 1, y, z, smallest, &n_sources, k);
			smallest
				= smallest_decay(w, x, y, z - 1, smallest, &n_sources, k);
			smallest
				= smallest_decay(w, x, y, z + 1, smallest, &n_sources, k);

			int next = smallest + step;
			if(next >= 8 || smallest < 0)
				next = -1;

			int above = raw_decay(w, x, y + 1, z, k);
			if(above >= 0)
				next = above >= FALLING ? above : above + FALLING;

			if(k->infinite && n_sources >= 2) {
				uint8_t ignored_meta;
				uint8_t bt = w->get(w->ctx, x, y - 1, z, &ignored_meta);
				/* Beta checks CURRENT metadata in the second clause. */
				if(material_is_solid(w, bt) || (is_same(bt, k) && m0 == 0))
					next = 0;
			}

			if(k == &LAVA && decay < FALLING && next < FALLING && next > decay
			   && next_int(w, 4) != 0) {
				next = decay;
				settle = false;
			}

			if(next != decay) {
				decay = next;
				set_self = true;
				set_type = next < 0 ? FLUID_AIR : k->flow;
				set_meta = next < 0 ? 0 : (uint8_t)next;
			} else if(settle) {
				set_self = true;
				set_type = k->still;
				set_meta = (uint8_t)decay;
			}
		} else {
			set_self = true;
			set_type = k->still;
			set_meta = (uint8_t)decay;
		}

		state->decay = decay;
		state->phase = FLUID_TICK_AFTER_SELF_SET;
		if(set_self) {
			w->set(w->ctx, x, y, z, set_type, set_meta);
			return false;
		}
	}

	if(state->phase == FLUID_TICK_AFTER_SELF_SET) {
		int decay = state->decay;
		/* Downward displacement is first and bypasses flowIntoBlock. */
		if(liquid_can_displace(w, x, y - 1, z, k, NULL, NULL)) {
			int down_meta = decay >= FALLING ? decay : decay + FALLING;
			state->phase = FLUID_TICK_DONE;
			w->set(w->ctx, x, y - 1, z, k->flow, (uint8_t)down_meta);
			return false;
		}
		if(decay < 0
		   || (decay != 0 && !block_blocks_flow(w, x, y - 1, z))) {
			state->phase = FLUID_TICK_DONE;
			return true;
		}
		optimal_dirs(w, x, y, z, w->shared_directions ? w->shared_directions : state->dirs, k);
		int out_level = decay >= FALLING ? 1 : decay + step;
		if(out_level >= 8) {
			state->phase = FLUID_TICK_DONE;
			return true;
		}
		/* Preserve this local in `decay`: horizontal mode needs only out_level. */
		state->decay = out_level;
		state->next_dir = 0;
		state->phase = FLUID_TICK_HORIZONTAL;
	}

	if(state->phase == FLUID_TICK_HORIZONTAL) {
		while(state->next_dir < 4) {
			int d = state->next_dir++;
			if((w->shared_directions ? w->shared_directions[d] : state->dirs[d])
			   && flow_into(w, x + DX[d], y, z + DZ[d], state->decay, k)) {
				/* flow_into's set has returned; let an immediate host run its child. */
				return false;
			}
		}
		state->phase = FLUID_TICK_DONE;
	}
	return true;
}

static void fluid_tick(const struct fluid_world* w, int x, int y, int z,
					   bool lava, bool nether) {
	struct fluid_tick_state state;
	fluid_tick_state_init(&state, x, y, z, lava, nether);
	while(!fluid_tick_state_step(w, &state))
		;
}

void fluid_water_tick(const struct fluid_world* w, int x, int y, int z,
					  bool nether) {
	fluid_tick(w, x, y, z, false, nether); // nether ignored (water step always 1)
}

void fluid_lava_tick(const struct fluid_world* w, int x, int y, int z,
					 bool nether) {
	fluid_tick(w, x, y, z, true, nether);
}
