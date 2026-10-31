/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef REDSTONE_SERVER_H
#define REDSTONE_SERVER_H

#include <stdbool.h>

#include "../item/item_types.h" // struct item_data
#include "blocks.h"		   // struct block_info, enum side
#include "redstone_logic.h" // REDSTONE_TORCH_TICK_RATE, the ids, the pure logic

struct server_local;

#include <stddef.h>
#include <stdint.h>

// Max C-level (recursive) wire-drain nesting. Deeper floods are NOT skipped (pre-G5 they were):
// they run as heap continuation frames drained by the enclosing drain loop, so the server stack
// never holds more than this many drain levels.
#define RS_WIRE_FLOOD_MAX_DEPTH 12

// Production glue binding the pure redstone_logic.c (wire flood, torch inverter + burnout) and
// the pure redstone_power.c (Beta's boolean power model) onto the live server world.
//
// POWER MODEL: FAITHFUL (the interim deviation was retired -- docs/redstone_plan.md step 3).
// The wire's flat-15 query and the torch's mount query now go through redstone_power.c, a
// verbatim transcription of Beta's boolean World.isBlockIndirectly{Getting,Providing}PowerTo
// + the per-block isPoweringTo/isIndirectlyPoweringTo (torch, wire, pressure plate), with the
// wiresProvidePower toggle (false for the wire's own flood, true for a torch mount). This models
// strong power re-emitted through solid cubes and the wire's connection-aware pointing -- not
// just adjacent torches/plates. Levers/buttons/repeaters are not yet placeable in-game; their
// ids are wired into the wire connection logic for when redstone step 6 lands.

// Wire: synchronous flood recompute, fired on a neighbour change.
void redstone_wire_neighbour_change(struct server_local* s, struct block_info* info);
// Beta BlockRedstoneWire.onBlockRemoval: depower the surviving network and
// notify its direct/sloped wire neighbourhood after the old cell becomes air.
void redstone_wire_removal(struct server_local* s, struct block_info* info);

// Wire item placement: default placement + an immediate self-flood so a freshly placed wire
// settles to its correct strength at once (Beta onBlockAdded).
bool redstone_wire_place(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side);

// Torch: the one-shot 2-game-tick scheduled inverter tick.
void redstone_torch_tick(struct server_local* s, struct block_info* info);

// Beta torch burnout effect draws [BlockRedstoneTorch.updateTick:126-131]:
// two nextFloat for the fizz pitch (returned), then 5 x (x,y,z) nextDouble
// smoke positions -- 17 draws on the SIMULATION stream passed in (production
// passes s->rand_src; Beta draws from World.rand). Exposed for the golden test.
#define REDSTONE_TORCH_BURNOUT_SMOKE 5
struct random_gen;
float redstone_torch_burnout_effect_draws(
	struct random_gen* g, int x, int y, int z,
	double smoke[REDSTONE_TORCH_BURNOUT_SMOKE][3]);

// Repeater (Beta BlockRedstoneRepeater): both ids 93/94 bind
// onWorldTick=redstone_repeater_server_tick (the self-armed metadata delay
// {2,4,6,8} is NOT a constant tick_delay -- the
// engine's constant auto-schedule is suppressed for it, see server_world_notify_block GAP #1) and
// onNeighbourBlockChange=redstone_repeater_server_neighbour_change (Beta's canBlockStay drop, then
// arm the delayed toggle). These wrap the pure redstone_repeater.c logic over the live world; named
// *_server_* to avoid clashing with the pure redstone_repeater_tick/_neighbour_change (different sig).
void redstone_repeater_server_tick(struct server_local* s, struct block_info* info);
void redstone_repeater_server_neighbour_change(struct server_local* s, struct block_info* info);

// Repeater item placement (Beta onBlockPlacedBy): place the idle repeater at `where` with `facing`
// (0-3, low nibble) and arm the delay-1 toggle if the input is already powered. Returns false if
// the cell is entity-obstructed. Used by item_repeater.c (the placeable ITEM_REDSTONE_REPEATER).
/* S1-a1b: `actor` is the placing player (canBlockBePlacedAt body sweep). */
bool redstone_repeater_place(struct server_local* s, struct server_player* actor,
		struct block_info* where, int facing);

// Beta World.isBlockIndirectlyProvidingPowerTo(x,y,z,side) over the live server world -- the SAME
// faithful boolean power view (incl. the TNT non-normal-cube carve-out) the torch mount + repeater
// input use, exposed so the PISTON power query reuses it instead of duplicating rs_is_normal_cube.
// wpp = wiresProvidePower (true for a non-wire querying a neighbour face, as the piston does).
bool redstone_server_indirect_power(struct server_local* s, int x, int y, int z, int side, bool wpp);
// Beta World.isBlockIndirectlyGettingPowered over the live server world (the door power query).
bool redstone_server_indirectly_getting_powered(struct server_local* s, int x, int y, int z);
// Beta World.isBlockGettingPowered over the live server world -- the WEAK/direct-adjacent-only
// check (noteblock-slice: BlockNote.onNeighborBlockChange), deliberately narrower than the
// indirectly-getting-powered query above (no "through a solid cube" hop).
bool redstone_server_getting_powered(struct server_local* s, int x, int y, int z);
// Wire-flood cascade diagnostics (next-gaps-0924/G5: the cascade is LOSSLESS -- see the drain in
// redstone_server.c). depth_max = deepest C-level drain nesting (<= RS_WIRE_FLOOD_MAX_DEPTH).
// cap_hits/continuations = floods reached past that depth, which run as explicit continuation
// frames (NOT skipped -- the pre-G5 guard dropped them).
int redstone_server_wire_flood_depth_max(void);
int redstone_server_wire_flood_depth_cap_hits(void);
int redstone_server_wire_flood_continuations(void);
void redstone_server_wire_flood_diag_reset(void);
unsigned redstone_server_suspends(void);		   // allocation-failure suspensions (explicit)
unsigned redstone_server_requests_queued(void);   // wire changes retained while suspended
unsigned redstone_server_requests_lost(void);	   // double fault: request ring full while OOM
unsigned redstone_server_marks_dedup_hits(void);  // Beta HashSet re-adds of a POSITION absorbed
size_t redstone_server_marks_peak(void);		   // peak deferred-notify POSITIONS live
size_t redstone_server_frames_peak(void);		   // peak continuation frames live
size_t redstone_server_store_heap_peak_bytes(void); // marks+dedup+frames heap (0 = static only)
size_t redstone_server_store_heap_bytes(void); // CURRENT heap of all redstone stores (0 when idle)
uint32_t redstone_server_drain_visits(void);  // Beta notifyBlockOfNeighborChange calls drained
size_t redstone_server_request_ring_count(void); // wire changes waiting in the suspend ring
// Test hook: called for every drain notification target, in order, before filtering.
void redstone_server_set_visit_hook(void (*hook)(int x, int y, int z));
// True while suspended cascade work is retained; redstone_server_service_pending finishes it (a
// no-op when nothing is pending or when called from inside a cascade). server_world_tick calls
// it first every tick; every redstone wire entry and the torch/repeater ticks call it too.
bool redstone_server_pending(void);
void redstone_server_service_pending(struct server_local* s);
// World teardown (server_world_destroy/discard): drop retained cascade work for the old world and
// release every heap store. Must not be called from inside a cascade.
void redstone_server_reset(void);

#endif
