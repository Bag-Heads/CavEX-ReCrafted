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

#include "fluid_block.h"

#include <assert.h>

#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/audio.h" /* S2: the harden random.fizz */
#include "blocks.h"
#include "fluid_spread.h"

static uint32_t s_lava_mix_effect_total;

uint32_t fluid_block_lava_mix_effect_total(void) {
	return s_lava_mix_effect_total;
}

float fluid_block_lava_mix_effect_draws(struct server_local* s) {
	assert(s);
	/* CFR BlockFluid.java:217-222: the dispatch world's Java Random owns
	 * both fizz floats, in left-to-right order. Smoke uses Math.random. */
	struct server_world* w = block_world(s);
	G9B_NOTE(w, "fizz-draw", "before", "", 0, 0, NULL);
	float r1 = jrand_next_float(&w->rand);
	G9B_NOTE(w, "fizz-draw", "middle", "", 0, 0, NULL);
	float r2 = jrand_next_float(&w->rand);
	G9B_NOTE(w, "fizz-draw", "after", "", 0, 0, NULL);
	/* Eight largesmoke particles draw independent Math.random X and Z jitter
	 * (Y is fixed at y+1.2), two nextDouble calls apiece. */
	server_local_math_random_discard(16);
	return 2.6F + (r1 - r2) * 0.8F;
}

void fluid_block_lava_mix_effect_commit_staged(uint32_t count) {
	s_lava_mix_effect_total += count;
}

uint8_t fluid_block_harden_product(uint8_t raw_metadata) {
	if(raw_metadata == 0)
		return BLOCK_OBSIDIAN;
	if(raw_metadata <= 4)
		return BLOCK_COBBLESTONE;
	return BLOCK_AIR;
}

struct fw_tick_context {
	struct server_local* server;
	bool failed;
};

static uint8_t fw_get(void* ctx, int x, int y, int z, uint8_t* m) {
	struct fw_tick_context* tick = ctx;
	struct block_data bd;
	if(!server_world_get_block(block_world(tick->server), x, y, z,
							  &bd)) {
		if(m)
			*m = 0;
		return 1; // unloaded / out of world -> a solid wall that blocks flow
	}
	if(m)
		*m = bd.metadata;
	return bd.type;
}

static void fw_set(void* ctx, int x, int y, int z, uint8_t t, uint8_t m) {
	struct fw_tick_context* tick = ctx;
	struct server_local* s = tick->server;
	struct block_data old;
	if(!server_world_get_block(block_world(s), x, y, z, &old))
		return;
	if(old.type == t && old.metadata == m)
		return;

	const bool old_flow
		= old.type == FLUID_WATER_FLOW || old.type == FLUID_LAVA_FLOW;
	const bool old_still
		= old.type == FLUID_WATER_STILL || old.type == FLUID_LAVA_STILL;
	const bool new_flow = t == FLUID_WATER_FLOW || t == FLUID_LAVA_FLOW;
	const bool new_still = t == FLUID_WATER_STILL || t == FLUID_LAVA_STILL;
	const bool same_fluid_family
		= ((old.type == FLUID_WATER_FLOW || old.type == FLUID_WATER_STILL)
		   && (t == FLUID_WATER_FLOW || t == FLUID_WATER_STILL))
		|| ((old.type == FLUID_LAVA_FLOW || old.type == FLUID_LAVA_STILL)
			&& (t == FLUID_LAVA_FLOW || t == FLUID_LAVA_STILL));

	/*
		Beta BlockFlowing.updateFlow and BlockStationary.setNotStationary use
		setBlockAndMetadata (no World.notifyBlockChange), then only mark the
		cell dirty/client-visible. The pure core represents both ID swaps through
		this callback; map them to the lit quiet ID+metadata setter, which runs
		the Chunk-level onBlockAdded once (RetroMCP Chunk.java:239-246).
		A still->flow wake is also
		scheduled explicitly by fluid_block_reactivate, matching
		BlockStationary.setNotStationary's second (deduplicated) schedule.
	*/
	if(same_fluid_family
	   && ((old_flow && new_still) || (old_still && new_flow))) {
		tick->failed = !server_world_set_block_quiet(
			block_world(s), x, y, z, (struct block_data) {.type = t, .metadata = m});
		return;
	}

	/*
		A decay update keeps the moving id and changes only metadata. Beta order
		is setBlockMetadataWithNotify (client + first neighbour notify), schedule,
		then an explicit notifyBlocksOfNeighborChange (second notify). Do not run
		the generic onBlockAdded hook for this metadata-only path.
	*/
	if(old.type == t && new_flow) {
		server_world_set_block_silent(
			block_world(s), x, y, z, (struct block_data) {.type = t, .metadata = m});
		notifyNeighbours_from(s, x, y, z, t);
		if(server_world_light_halted(block_world(s)))
			return;
		server_world_schedule_block_tick(block_world(s), x, y, z, t);
		notifyNeighbours_from(s, x, y, z, t);
		return;
	}

	/* New flow, removal, hardening, and displacement all use WithNotify. A
	   newly placed moving fluid schedules itself in fluid_block_on_added. */
	server_world_set_block(
		block_world(s), x, y, z, (struct block_data) {.type = t, .metadata = m});
}

bool fluid_block_blocks_flow_id(uint8_t t) {
	/* Beta BlockFlowing.blockBlocksFlow, exact predicate. Air is open. Wood /
	   iron doors, standing sign, ladder, and reed are explicit blockers; for
	   every other id the block's Material.getIsSolid decides. Notably plants,
	   torches, rails, wire, snow layer, fire, and portal are displaceable even
	   though CaveWii's placement-only `place_ignore` flag does not model all of
	   those materials. [decompile 740c583 BlockFlowing.blockBlocksFlow] */
	if(t == BLOCK_AIR)
		return false;
	if(t == BLOCK_DOOR_WOOD || t == BLOCK_DOOR_IRON || t == 63 /* signPost */
	   || t == BLOCK_LADDER || t == BLOCK_REED)
		return true;
	return block_beta_material_is_solid(t);
}

bool fluid_block_material_is_solid_id(uint8_t t) {
	return block_beta_material_is_solid(t);
}

static bool fw_blocks_flow(void* ctx, uint8_t t) {
	(void)ctx;
	return fluid_block_blocks_flow_id(t);
}

static bool fw_material_is_solid(void* ctx, uint8_t t) {
	(void)ctx;
	return fluid_block_material_is_solid_id(t);
}

static int fw_next_int(void* ctx, int bound) {
	struct fw_tick_context* tick = ctx;
	return rand_gen_int(&tick->server->rand_src, bound);
}

static void fw_drop_block(void* ctx, int x, int y, int z, uint8_t type,
						  uint8_t meta) {
	struct fw_tick_context* tick = ctx;
	struct server_local* s = tick->server;
	struct block_data old = {.type = type, .metadata = meta};
	struct block_info info
		= {.block = &old, .neighbours = NULL, .x = x, .y = y, .z = z};
	server_local_spawn_block_drops(s, &info);
}

void fluid_block_lava_mix_effect(struct server_local* s, int x, int y, int z,
								 bool emit_client) {
	s_lava_mix_effect_total++;
	/* Beta computes the fizz pitch from two World.rand.nextFloat draws before
	   spawning its eight particles with the separate Math.random stream. The
	   draws run UNCONDITIONALLY (populate passes emit_client=false but must
	   still preserve the stream); the sound/smoke below are client-emit only. */
	float fizz_pitch = fluid_block_lava_mix_effect_draws(s);
	if(!emit_client)
		return;
	/* S2: Beta triggerLavaMixEffects plays "random.fizz" 0.5F at the cell
	   center with the pitch from those exact two draws -- the lava-water
	   HARDEN pop (source->obsidian / meta<=4->cobblestone). [740c583:
	   BlockFluid.triggerLavaMixEffects:287] */
	server_local_emit_sound(SOUND_RANDOM_FIZZ,
							(vec3) {x + 0.5F, y + 0.5F, z + 0.5F}, 0.5F,
							fizz_pitch);
	/* One render-thread RPC at intensity 2 emits the eight large-smoke particles;
	   their client-side jitter remains outside the simulation RNG. */
	clin_rpc_send(&(struct client_rpc) {
		.type = CRPC_SMOKE_PUFF,
		.payload.smoke_puff.pos = {x + 0.5F, y + 0.5F, z + 0.5F},
		.payload.smoke_puff.intensity = 2.0F,
	});
}

static void fw_lava_mix(void* ctx, int x, int y, int z) {
	fluid_block_lava_mix_effect(ctx, x, y, z, true);
}

static void fw_tick_lava_mix(void* ctx, int x, int y, int z) {
	struct fw_tick_context* tick = ctx;
	fw_lava_mix(tick->server, x, y, z);
}

void fluid_block_tick(struct server_local* s, struct block_info* this,
					  bool lava) {
	struct fw_tick_context tick = {.server = s};
	struct fluid_world w = {.ctx = &tick,
						   .get = fw_get,
						   .set = fw_set,
						   .blocks_flow = fw_blocks_flow,
						   .material_is_solid = fw_material_is_solid,
						   .next_int = fw_next_int,
						   .drop_block = fw_drop_block,
						   .lava_mix = fw_tick_lava_mix};
	// Beta: Nether lava steps 1 (reaches 7) vs overworld 2 (reaches 3). Read the
	// dimension here (this TU has the full struct via server_local.h), so the
	// block adapters stay decoupled.
	bool nether = block_world(s)->dimension == WORLD_DIM_NETHER;
	/* BlockFlowing.java:76-113 continues after the self write, then after
	 * each horizontal write. The resumable core yields at precisely those
	 * boundaries: do not resume a failed quiet write or a halted world into
	 * later displacement drops/fizz, RNG draws, callbacks or publication. */
	struct fluid_tick_state state;
	fluid_tick_state_init(&state, this->x, this->y, this->z, lava, nether);
	while(!tick.failed && !server_world_light_halted(block_world(s)))
		if(fluid_tick_state_step(&w, &state))
			break;
}

static bool fluid_is_water(uint8_t type) {
	return type == BLOCK_WATER_FLOW || type == BLOCK_WATER_STILL;
}

/* BlockFluid.checkForHarden, called synchronously from BOTH onBlockAdded and
   onNeighborBlockChange. Returns true iff the original lava id was replaced. */
static bool fluid_block_harden(struct server_local* s, struct block_info* this,
							   bool lava) {
	if(server_world_simulation_stopped(block_world(s)) || !lava)
		return false;

	struct block_data live;
	if(!server_world_get_block(block_world(s), this->x, this->y, this->z, &live)
	   || live.type != this->block->type)
		return false;

	static const int dx[5] = {0, 0, -1, 1, 0};
	static const int dy[5] = {0, 0, 0, 0, 1};
	static const int dz[5] = {-1, 1, 0, 0, 0};
	bool water = false;
	for(int i = 0; i < 5 && !water; i++) {
		struct block_data neighbour;
		water = server_world_get_block(block_world(s), this->x + dx[i],
										   this->y + dy[i], this->z + dz[i],
										   &neighbour)
			&& fluid_is_water(neighbour.type);
	}
	if(!water)
		return false;

	bool replaced = false;
	uint8_t product = fluid_block_harden_product(live.metadata);
	if(product != BLOCK_AIR) {
		/* CFR BlockFluid.java:207-210 uses World.setBlockWithNotify:
		 * a nested PLAIN setter even when the outer write supplied metadata. */
		replaced = server_world_set_block_plain(
			block_world(s), this->x, this->y, this->z, product);
	}
	if(server_world_simulation_stopped(block_world(s))) return replaced;
	/* Beta triggers fizz/smoke for every contact, including raw metadata 5..15
	   where no replacement occurs. */
	fw_lava_mix(s, this->x, this->y, this->z);
	return replaced;
}

void fluid_block_on_added(struct server_local* s, struct block_info* this,
						  bool lava, uint8_t flow_id) {
	if(server_world_simulation_stopped(block_world(s))) return;
	fluid_block_harden(s, this, lava);
	if(server_world_simulation_stopped(block_world(s))) return;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(server_world_test_fluid_tail_hook) server_world_test_fluid_tail_hook(block_world(s), false);
#endif
	struct block_data live;
	if(flow_id != 0
	   && server_world_get_block(block_world(s), this->x, this->y, this->z, &live)
	   && live.type == flow_id)
		server_world_schedule_block_tick(
			block_world(s), this->x, this->y, this->z, flow_id);
}

void fluid_block_on_neighbour(struct server_local* s, struct block_info* this,
							  bool lava, uint8_t flow_id) {
	if(server_world_simulation_stopped(block_world(s))) return;
	const bool replaced = fluid_block_harden(s, this, lava);
	if(server_world_simulation_stopped(block_world(s)) || replaced) return;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(server_world_test_fluid_tail_hook) server_world_test_fluid_tail_hook(block_world(s), true);
#endif
	if(flow_id != 0)
		fluid_block_reactivate(s, this, flow_id);
}

void fluid_block_reactivate(struct server_local* s, struct block_info* this,
							uint8_t flow_id) {
	struct block_data live;
	if(!server_world_get_block(block_world(s), this->x, this->y, this->z, &live)
	   || (flow_id == BLOCK_WATER_FLOW && live.type != BLOCK_WATER_STILL)
	   || (flow_id == BLOCK_LAVA_FLOW && live.type != BLOCK_LAVA_STILL))
		return;

	/* RetroMCP server BlockStationary.java:23-29: suppress nested neighbour
	   notifications during the lit quiet conversion and explicit schedule.
	   Restore an outer editing scope even on failure. */
	struct server_world* w = block_world(s);
	const bool previous_editing = w->editing_blocks;
	w->editing_blocks = true;
	const bool changed = server_world_set_block_quiet(
		w, this->x, this->y, this->z,
		(struct block_data) {.type = flow_id, .metadata = live.metadata});
	/* setBlockAndMetadata invokes BlockFlowing.onBlockAdded before the explicit
	   schedule below. Its harden check is observable even on this quiet path;
	   its first schedule and the explicit second one deduplicate in Beta's
	   pending-tick set (as they do in CaveWii's queue). */
	if(changed && !server_world_light_halted(w))
		server_world_schedule_block_tick(w, this->x, this->y, this->z, flow_id);
	w->editing_blocks = previous_editing;
}
