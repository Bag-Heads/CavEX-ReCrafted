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
	Glue between the fluid_spread engine and the live server world, shared by
	block_water.c and block_lava.c so the fluid_world accessor exists once.
*/

#ifndef FLUID_BLOCK_H
#define FLUID_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Beta 1.7.3 BlockFluid.tickRate(): water = 5, lava = 30 (lava is 30 in all dims, and CaveWii only
// generates the overworld). These are the scheduled-tick delays for the FLOWING fluid structs
// (block_water_flowing / block_lava_flowing); the still structs have no onWorldTick. Without them the
// queue defaults to 1 (server_world_tick_queue.c), making water 5x and lava 30x too fast.
#define FLUID_WATER_TICK_RATE 5
#define FLUID_LAVA_TICK_RATE  30

struct server_local;
struct block_info;

// Exact Beta BlockFlowing predicates exposed for the raw populate-time fluid
// adapter. They deliberately remain separate: ladder/reed block flow but are
// not Material-solid (the distinction controls infinite-water formation).
bool fluid_block_blocks_flow_id(uint8_t type);
bool fluid_block_material_is_solid_id(uint8_t type);
// Monotonic server-thread diagnostic/test seam: each Beta triggerLavaMixEffects
// dispatch (hardening contact or horizontal lava replacement) increments once.
uint32_t fluid_block_lava_mix_effect_total(void);
// Beta triggerLavaMixEffects: advance World.rand for the two fizz-pitch
// nextFloat draws, count the dispatch, and optionally emit the client smoke.
// Raw populate uses emit_client=false because those chunks are not sent yet,
// but must still preserve the server RNG stream.
void fluid_block_lava_mix_effect(struct server_local* s, int x, int y, int z,
								 bool emit_client);
/* Populate's immediate-fluid adapter journals the whole decorate quadrant.
 * Consume Beta's two World.rand fizz draws at the exact action point, but defer
 * the diagnostic effect count until that quadrant commits.  This keeps a failed
 * populate retry byte-identical without needing to decrement global state.
 * S2: returns the Beta fizz PITCH those two draws produce -- 2.6F + (r1-r2)
 * * 0.8F [740c583: BlockFluid.triggerLavaMixEffects:287] -- so the emitting
 * caller plays the sound from the very draws that already preserve stream
 * parity (no extra RNG). Silent callers just ignore the return. */
float fluid_block_lava_mix_effect_draws(struct server_local* s);
void fluid_block_lava_mix_effect_commit_staged(uint32_t count);
// BlockFluid.checkForHarden's raw-metadata product: source 0 -> obsidian,
// 1..4 -> cobblestone, 5..15 -> air sentinel (effect only, no replacement).
uint8_t fluid_block_harden_product(uint8_t raw_metadata);

// onWorldTick body for a flowing fluid block: run the Beta spread over the live
// world. `lava` selects the lava kind (no infinite source) instead of water. The
// dimension (for lava's Nether 1-level step: reaches 7, not 3) is read from
// s->world.dimension inside, so block_lava/water.c need no server_local include.
void fluid_block_tick(struct server_local* s, struct block_info* this, bool lava);

// Beta BlockFlowing.onBlockAdded: run BlockFluid's synchronous lava-harden
// check, then schedule this flowing id at its tickRate if it survived.
void fluid_block_on_added(struct server_local* s, struct block_info* this,
						  bool lava, uint8_t flow_id);

// Beta BlockFluid/BlockStationary onNeighborBlockChange. Every fluid runs the
// synchronous lava-harden check. A surviving still fluid then re-wakes
// UNCONDITIONALLY through BlockStationary.setNotStationary; pass flow_id 0 for
// a moving fluid (harden-only), or the corresponding moving id for a still one.
void fluid_block_on_neighbour(struct server_local* s, struct block_info* this,
							  bool lava, uint8_t flow_id);

// BlockStationary.setNotStationary: quiet still->flowing ID swap + one explicit
// tickRate schedule. No "has somewhere to go" guard exists in Beta.
void fluid_block_reactivate(struct server_local* s, struct block_info* this,
							uint8_t flow_id);

#endif
