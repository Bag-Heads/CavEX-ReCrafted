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

#ifndef REDSTONE_LOGIC_H
#define REDSTONE_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure Beta 1.7.3 redstone logic (decompile 740c583), isolated behind an injected world
// accessor so the SAME code runs over the server world (production) and a flat stub cell
// grid (headless golden tests) -- the furnace_logic.c / fluid_spread callback precedent.
//
// CENTRAL FAITHFULNESS NOTE: BlockRedstoneWire power spread is SYNCHRONOUS and RECURSIVE
// (updateAndPropagateCurrentStrength -> calculateCurrentChanges) -- it settles the whole run
// within the originating block update (0 game-ticks over any length), NOT a per-tick crawl.
// Beta has NO integer power query: isBlockIndirectlyGettingPowered is BOOLEAN; the 0-15
// gradient lives ONLY in wire metadata (getMaxCurrentStrength reads neighbour WIRE metadata)
// with a single --strength per recursion hop, and a non-wire source injects a flat 15.

#define REDSTONE_WIRE_ID 55		 // BLOCK_REDSTONE_WIRE (matches blocks_data.h)
#define REDSTONE_TORCH_IDLE_ID 75	 // torchRedstoneIdle  (BLOCK_REDSTONE_TORCH, unlit)
#define REDSTONE_TORCH_LIT_ID 76	 // torchRedstoneActive(BLOCK_REDSTONE_TORCH_LIT)
#define REDSTONE_MAX_STRENGTH 15
#define REDSTONE_TORCH_TICK_RATE 2 // BlockRedstoneTorch.tickRate() (the 0.1s toggle delay)
#define REDSTONE_WIRE_UPDATE_LOSSLESS 1 // G5 API marker (growable worklist + suspend/resume)
#define REDSTONE_WIRE_WORK_MAX 4096 // INITIAL (static) worklist capacity; grows on the heap (G5)

// Injected world view. Coordinates are absolute block coords. get_type returns the block id
// at a cell (any non-negative id; a sentinel < 0 for "outside the world"). is_normal_cube is
// Beta's isBlockNormalCube (a full opaque solid cube -- wire stair-steps over these).
// is_indirectly_powered is Beta's BOOLEAN World.isBlockIndirectlyGettingPowered for THIS cell
// with wiresProvidePower forced false (so a wire never counts itself/other wire as a source).
// mark_needs_update accumulates a cell into the deferred blocksNeedingUpdate set that the
// caller drains into neighbour-notifies AFTER the flood (production) or ignores (golden).
struct redstone_world {
	void* ctx;
	int (*get_type)(void* ctx, int x, int y, int z);
	int (*get_meta)(void* ctx, int x, int y, int z);
	void (*set_meta)(void* ctx, int x, int y, int z, int meta);
	bool (*is_normal_cube)(void* ctx, int x, int y, int z);
	bool (*is_indirectly_powered)(void* ctx, int x, int y, int z);
	void (*mark_needs_update)(void* ctx, int x, int y, int z);
	// Optional (NULL = unbounded/none): secure capacity for ONE more mark_needs_update record
	// BEFORE the flood pops a cell. Returning false (allocation failure) suspends the flood with
	// every pending entry retained -- the record can never be dropped mid-cell.
	bool (*reserve)(void* ctx);
	// Optional pacing yield (NULL = none; the production wire flood passes thread_yield), called
	// every 64 pops. It only offers the core to EQUAL-priority peers: on the Wii the server thread
	// (63) is already below render (64), so render is protected by priority and a large settle is
	// a synchronous server-tick stall that yielding cannot shorten. Yielding never advances a tick
	// or mutates the world. No-op on PC.
	void (*yield)(void);
	// Torch-only accessors (NULL for the wire; wire never calls them):
	void (*set_type)(void* ctx, int x, int y, int z, int type); // swap block id (lit<->idle)
	// Beta World.isBlockIndirectlyProvidingPowerTo(x,y,z,side): BOOLEAN strong-power query of
	// the torch's attachment block (does that block carry power into the torch's mount face).
	bool (*is_indirectly_providing_power)(void* ctx, int x, int y, int z, int side);
	int64_t (*get_world_time)(void* ctx); // for the burnout 100-tick window
	// Optional burnout effect after the lit->idle swap and burnout record/check
	// (Beta fizz + smoke). The production hook draws 17 World.rand values from
	// the simulation stream; NULL keeps pure-world callers silent and draw-free.
	// It never changes torch state or timing.
	void (*on_burnout)(void* ctx, int x, int y, int z);
};

// BlockRedstoneTorch.updateTick (decompile 740c583): the inverter. Reads whether the torch's
// single attachment block is powered (isIndirectlyPowered, keyed by the mount metadata) and
// swaps lit<->idle accordingly -- input ON => torch OFF. The lit<->idle swap is what the
// caller schedules at REDSTONE_TORCH_TICK_RATE (2) game-ticks after a neighbour change.
// Burnout: a static list records each lit->idle toggle; >=8 toggles at one position within a
// 100-tick window blocks the relight (the torch stays dark) until those entries age out.
void redstone_torch_update(struct redstone_world* w, int x, int y, int z);

// Reset the static burnout list (test-only; Beta's torchUpdates is process-static).
void redstone_torch_reset_burnout(void);

// BlockRedstoneWire.updateAndPropagateCurrentStrength: run the synchronous recursive current
// recompute starting from the wire at (x,y,z) and commit the settled 0-15 strength to every
// connected wire's metadata via set_meta. Cells whose strength changed are passed to
// mark_needs_update (the caller drains them to neighbour notifies after this returns).
//
// LOSSLESS (next-gaps-0924/G5): the worklist grows on the heap instead of dropping links. Returns
// true when the flood reached its fixpoint; false only when an allocation failed persistently
// (after cooperative-yield retries) -- the flood is then SUSPENDED with all pending work retained
// (redstone_wire_update_pending() is true) and must be continued with redstone_wire_update_resume
// before any new flood starts. Nothing is ever discarded.
bool redstone_wire_update(struct redstone_world* w, int x, int y, int z);
bool redstone_wire_update_resume(struct redstone_world* w);
bool redstone_wire_update_pending(void);
// World teardown only: discard a suspended flood and give a grown ring back.
void redstone_wire_update_reset(void);
int redstone_wire_update_max_pops(void);
int redstone_wire_update_full_hits(void); // queue entries dropped: always 0 since G5 (kept for diag)
size_t redstone_wire_update_peak_entries(void);	   // peak pending entries since diag reset
size_t redstone_wire_update_peak_heap_bytes(void); // peak heap bytes of the grown ring (0 = static)
unsigned redstone_wire_update_grows(void);
size_t redstone_wire_update_heap_bytes(void); // current heap bytes of the ring (0 = static)
unsigned redstone_wire_update_suspends(void);
void redstone_wire_update_diag_reset(void);

// Allocator shared by the redstone growable stores (worklist here, the deferred-notify marks and
// continuation frames in redstone_server.c). NULL hooks restore malloc/free. Tests inject failures.
void redstone_logic_set_alloc_hooks(void* (*alloc_fn)(size_t), void (*free_fn)(void*));
void* redstone_alloc(size_t bytes); // counts failures
void redstone_free(void* p);
unsigned redstone_alloc_failures(void);

// NOTE (G5 F4): production no longer drains through this 25-cell reach -- redstone_server.c keeps
// Beta's deduplicated seven-POSITION set and notifies each position's six neighbours. Kept as the
// pure, golden-tested description of one isolated changed cell's reach.
// Enumerate the cells Beta's wire flood would fire onNeighborBlockChange on for a SINGLE changed
// wire cell at (x,y,z). Beta adds the wire cell PLUS its 6 face-neighbours to blocksNeedingUpdate,
// then drains each via World.notifyBlocksOfNeighborChange, which notifies THAT cell's 6 neighbours
// -- a TWO-hop reach (the face-neighbours of {(x,y,z)} U its 6 face-neighbours). Each DISTINCT
// target cell is passed to visit(ctx,...) exactly once (deduped). Pure coordinate math (no world
// access); the production caller filters the targets to components and schedules their ticks. A
// torch mounted on a solid block adjacent to the wire (the canonical NOT/NOR gate) sits two hops
// from the wire and is reached ONLY via the 2nd hop -- a one-hop reach leaves it un-armed and the
// gate silently never inverts. The full distinct reach is 25 cells (1 + 6 face + 6 axis-2 + 12 edge).
void redstone_wire_notify_targets(int x, int y, int z, void* ctx,
								  void (*visit)(void* ctx, int x, int y, int z));

#endif
