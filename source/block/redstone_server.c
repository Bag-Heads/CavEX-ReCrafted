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

#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/client_interface.h"
#include "../platform/audio.h"
#include "../platform/thread.h" // thread_yield (cascade pacing for the wire flood on the Wii)
#include "blocks.h"
#include "blocks_data.h"
#include "redstone_logic.h"
#include "redstone_power.h"
#include "redstone_repeater.h"
#include "redstone_server.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The pure power model keeps its block ids local (so it links without the engine registry); pin
// them to the engine enum here so a future blocks_data.h renumber can never silently diverge.
_Static_assert(RSP_WIRE_ID == BLOCK_REDSTONE_WIRE, "redstone_power wire id drift");
_Static_assert(RSP_TORCH_IDLE_ID == BLOCK_REDSTONE_TORCH, "redstone_power idle-torch id drift");
_Static_assert(RSP_TORCH_LIT_ID == BLOCK_REDSTONE_TORCH_LIT, "redstone_power lit-torch id drift");
_Static_assert(RSP_STONE_PLATE_ID == BLOCK_STONE_PRESSURE_PLATE, "redstone_power stone-plate id drift");
_Static_assert(RSP_WOOD_PLATE_ID == BLOCK_WOOD_PRESSURE_PLATE, "redstone_power wood-plate id drift");
_Static_assert(RS_REPEATER_IDLE_ID == BLOCK_REDSTONE_REPEATER, "redstone_repeater idle id drift");
_Static_assert(RS_REPEATER_ACTIVE_ID == BLOCK_REDSTONE_REPEATER_LIT, "redstone_repeater active id drift");
_Static_assert(RSP_LEVER_ID == BLOCK_LEVER, "redstone_power lever id drift");
_Static_assert(RSP_BUTTON_ID == BLOCK_STONE_BUTTON, "redstone_power button id drift");
_Static_assert(RSP_DETECTOR_RAIL_ID == BLOCK_DETECTOR_RAIL,
			   "redstone_power detector-rail id drift");

struct rs_ctx {
	struct server_local* s;
};

static int rs_get_type(void* c, int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(block_world(((struct rs_ctx*)c)->s), x, y, z, &b) ? b.type : -1;
}
static int rs_get_meta(void* c, int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(block_world(((struct rs_ctx*)c)->s), x, y, z, &b)
		? (b.metadata & 0x0F)
		: 0;
}
static void rs_set_meta(void* c, int x, int y, int z, int meta) {
	struct server_local* s = ((struct rs_ctx*)c)->s;
	struct block_data b;
	if(!server_world_get_block(block_world(s), x, y, z, &b))
		return;
	b.metadata = (uint8_t)(meta & 0x0F);
	// QUIET write: the flood manages wire propagation; rs_mark notifies the components.
	server_world_set_block_silent(block_world(s), x, y, z, b);
}
static bool rs_is_normal_cube(void* c, int x, int y, int z) {
	struct block_data b;
	if(!server_world_get_block(block_world(((struct rs_ctx*)c)->s), x, y, z, &b))
		return false;
	// Beta World.isBlockNormalCube == material.getIsTranslucent() && renderAsNormalBlock(): the
	// blocks flagged setIsTranslucent() (leaves/glass/ice/snow = can_see_through) AND the lone
	// opaque non-cube TNT (Material.tnt.setIsTranslucent) are excluded. Shared with the
	// redstone-wire renderer via block_is_normal_cube so sim and render never drift.
	return block_is_normal_cube(b.type);
}
// FAITHFUL boolean power view over the server world: the three reads Beta's power model needs.
static struct redstone_power_world rs_power_view(void* c) {
	return (struct redstone_power_world) {
		.ctx = c,
		.get_type = rs_get_type,
		.get_meta = rs_get_meta,
		.is_normal_cube = rs_is_normal_cube,
	};
}
static bool rs_wire_is_powered(void* c, int x, int y, int z) {
	// Beta World.isBlockIndirectlyGettingPowered with wiresProvidePower forced FALSE -- the wire's
	// own calculateCurrentChanges context, so a wire never counts itself or any other wire as a
	// source (the 0-15 gradient is carried by the wire flood, not this boolean query). Replaces
	// the retired interim "adjacent lit torch / pressed plate" stand-in (redstone_power.c).
	struct redstone_power_world pw = rs_power_view(c);
	return redstone_is_indirectly_getting_powered(&pw, x, y, z, false);
}
static void rs_mark_visit(void* c, int x, int y, int z) {
	// One Beta notifyBlockOfNeighborChange from the drain (a neighbour of a set position). The
	// wire network is already settled by the flood, so wire cells are skipped: Beta's nested
	// updateAndPropagateCurrentStrength there finds no change and adds nothing.
	struct server_local* s = ((struct rs_ctx*)c)->s;
	struct block_data b;
	if(!server_world_get_block(block_world(s), x, y, z, &b))
		return;
	if(b.type == BLOCK_REDSTONE_WIRE)
		return;
	const struct block* blk = blocks[b.type];
	if(!blk)
		return;
	// A SELF-SCHEDULING component (defines onNeighbourBlockChange -- the repeater, the sole
	// both-hooks block per GAP #1) must be notified through THAT handler, exactly as Beta's wire
	// flood drains blocksNeedingUpdate into notifyBlockOfNeighborChange -> onNeighborBlockChange.
	// Its handler arms the correct metadata-varying delay {2,4,6,8} ONLY when its input/output
	// disagree. The constant-delay server_world_schedule_block_tick path below (right for the
	// idempotent torch, tickRate=2) would instead arm the repeater's TOGGLE at delay 1
	// unconditionally -- collapsing every wire-driven delay to 1 tick AND spuriously flipping a
	// stable idle repeater. (The torch has no onNeighbourBlockChange, so it keeps the fast path.)
	if(blk->onNeighbourBlockChange) {
		// Beta drains blocksNeedingUpdate via notifyBlocksOfNeighborChange(x,y,z, WIRE id):
		// publish the wire as the notify origin so origin-gated handlers (the door's
		// canProvidePower var5 gate) accept the dispatch. This branch also covers
		// HANDLER-ONLY components (the door: onNeighbourBlockChange, no onWorldTick) --
		// the old `&& blk->onWorldTick` condition silently skipped them, leaving
		// wire-driven doors inert.
		struct block_info info = {.block = &b, .neighbours = NULL, .x = x, .y = y, .z = z};
		server_world_notify_origin_set(BLOCK_REDSTONE_WIRE);
		blk->onNeighbourBlockChange(s, &info);
	} else if(blk->onWorldTick) {
		// Constant-delay tickers (torch): the existing auto-schedule at tick_delay. The
		// scheduled-tick queue de-dups (pos,type) across overlapping reaches.
		server_world_schedule_block_tick(block_world(s), x, y, z, b.type);
	}
}
// --- blocksNeedingUpdate: the LOSSLESS deferred-notify set (next-gaps-0924/G5) ------------------
// Beta BlockRedstoneWire collects, for every changed cell passing the `var8 == 0 || var9 == 0`
// gate, SEVEN POSITIONS -- the cell and its six face neighbours, in the order (x,y,z), x-1, x+1,
// y-1, y+1, z-1, z+1 -- into the java.util.HashSet blocksNeedingUpdate (BlockRedstoneWire.java:10,
// :160-168) DURING calculateCurrentChanges, and only AFTER the whole recursion settles
// (updateAndPropagateCurrentStrength :45-55) drains each distinct POSITION once through
// World.notifyBlocksOfNeighborChange -- its six neighbours in the order -X,+X,-Y,+Y,-Z,+Z
// (World.java:498-505). So a notified component always reads the FINAL wire metadata (a
// component dispatched mid-flood would see a half-settled cell and, on a falling edge into an
// idle repeater, schedule a phantom pulse Beta never emits), and a component beside several
// changed cells is notified once per adjacent set position -- exactly Beta's count.
//
// The set is UNCAPPED like Beta's (the pre-G5 256-record array dropped the tail, and every
// repeated mark consumed a slot): a file-static initial store grows on the heap, deduplicated per
// flood BY POSITION (the HashSet). Drain order is first-insertion; Java HashSet iteration order is
// NOT EMULATED. It is deterministic (ChunkPosition.hashCode x*8976890+y*981131+z, bucket-prepend)
// but hangs on the table capacity of Block.redstoneWire's set, which persists for the session
// (clear() never shrinks it) -- mirroring it would mean mirroring that session-long table state.
// Capacity for one mark's seven positions is secured through redstone_world.reserve BEFORE the
// flood pops a cell, so an allocation failure suspends the flood instead of losing a record.
struct rs_cell {
	int x, y, z;
};
#define RS_MARKS_INITIAL 512 // positions (a changed cell adds up to 7)
static struct rs_cell s_marks_static[RS_MARKS_INITIAL];
static struct rs_cell* s_marks = s_marks_static;
static size_t s_marks_cap = RS_MARKS_INITIAL;
static size_t s_marks_n = 0;
static size_t s_marks_flood_begin = 0; // first position of the flood currently accumulating

// Per-flood dedup: open addressing over position indices, generation-stamped so a new flood
// starts with an empty set in O(1). Load factor kept <= 1/2.
struct rs_hslot {
	uint32_t gen;
	uint32_t idx;
};
#define RS_HASH_INITIAL 1024
static struct rs_hslot s_hash_static[RS_HASH_INITIAL];
static struct rs_hslot* s_hash = s_hash_static;
static size_t s_hash_cap = RS_HASH_INITIAL;
static uint32_t s_hash_gen = 1; // slot.gen != s_hash_gen => empty

static size_t s_marks_peak = 0;
static size_t s_marks_heap_bytes = 0, s_hash_heap_bytes = 0, s_frames_heap_bytes = 0;
static size_t s_store_heap_peak = 0;
static unsigned s_marks_dedup_hits = 0;

static void rs_note_heap(void) {
	size_t b = s_marks_heap_bytes + s_hash_heap_bytes + s_frames_heap_bytes;
	if(b > s_store_heap_peak)
		s_store_heap_peak = b;
}

static uint32_t rs_hash(int x, int y, int z) {
	uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u ^ (uint32_t)z * 0xC2B2AE3Du;
	return h ^ (h >> 15);
}
static void rs_hash_insert(size_t idx) {
	const struct rs_cell* m = &s_marks[idx];
	size_t mask = s_hash_cap - 1;
	for(size_t i = rs_hash(m->x, m->y, m->z) & mask;; i = (i + 1) & mask)
		if(s_hash[i].gen != s_hash_gen) {
			s_hash[i].gen = s_hash_gen;
			s_hash[i].idx = (uint32_t)idx;
			return;
		}
}
static void rs_hash_new_flood(void) {
	if(++s_hash_gen == 0) { // 2^32 floods: re-zero once rather than alias a stale generation
		memset(s_hash, 0, s_hash_cap * sizeof(*s_hash));
		s_hash_gen = 1;
	}
}

#define RS_POSITIONS_PER_MARK 7
// redstone_world.reserve: room for ONE more mark = seven positions (array + hash load <= 1/2).
static bool rs_marks_reserve(void* c) {
	(void)c;
	if(s_marks_n + RS_POSITIONS_PER_MARK > s_marks_cap) {
		size_t ncap = s_marks_cap * 2;
		struct rs_cell* nm = redstone_alloc(ncap * sizeof(*nm));
		if(!nm)
			return false;
		memcpy(nm, s_marks, s_marks_n * sizeof(*nm));
		if(s_marks != s_marks_static)
			redstone_free(s_marks);
		s_marks = nm;
		s_marks_cap = ncap;
		s_marks_heap_bytes = ncap * sizeof(*nm);
		rs_note_heap();
	}
	size_t live = s_marks_n - s_marks_flood_begin + RS_POSITIONS_PER_MARK;
	if(live * 2 > s_hash_cap) {
		size_t ncap = s_hash_cap * 2;
		struct rs_hslot* nh = redstone_alloc(ncap * sizeof(*nh));
		if(!nh)
			return false;
		memset(nh, 0, ncap * sizeof(*nh));
		if(s_hash != s_hash_static)
			redstone_free(s_hash);
		s_hash = nh;
		s_hash_cap = ncap;
		s_hash_gen = 1;
		for(size_t i = s_marks_flood_begin; i < s_marks_n; i++)
			rs_hash_insert(i);
		s_hash_heap_bytes = ncap * sizeof(*nh);
		rs_note_heap();
	}
	return true;
}
static void rs_position_add(int x, int y, int z) {
	size_t mask = s_hash_cap - 1;
	for(size_t i = rs_hash(x, y, z) & mask;; i = (i + 1) & mask) {
		if(s_hash[i].gen != s_hash_gen) {
			assert(s_marks_n < s_marks_cap); // secured by rs_marks_reserve before the pop
			s_marks[s_marks_n] = (struct rs_cell) {x, y, z};
			s_hash[i].gen = s_hash_gen;
			s_hash[i].idx = (uint32_t)s_marks_n;
			s_marks_n++;
			if(s_marks_n > s_marks_peak)
				s_marks_peak = s_marks_n;
			return;
		}
		const struct rs_cell* m = &s_marks[s_hash[i].idx];
		if(m->x == x && m->y == y && m->z == z) {
			s_marks_dedup_hits++; // Beta HashSet.add of a present position: no second entry
			return;
		}
	}
}
// mark_needs_update: BlockRedstoneWire.java:161-167, the seven adds in Beta's order.
static void rs_mark_accumulate(void* c, int x, int y, int z) {
	(void)c;
	rs_position_add(x, y, z);
	rs_position_add(x - 1, y, z);
	rs_position_add(x + 1, y, z);
	rs_position_add(x, y - 1, z);
	rs_position_add(x, y + 1, z);
	rs_position_add(x, y, z - 1);
	rs_position_add(x, y, z + 1);
}

// --- torch accessors ---
static void rs_set_type(void* c, int x, int y, int z, int type) {
	struct server_local* s = ((struct rs_ctx*)c)->s;
	struct block_data b;
	if(!server_world_get_block(block_world(s), x, y, z, &b))
		return;
	b.type = (uint8_t)type;
	// Full notify: the torch toggling must re-flood adjacent wire + reschedule components.
	server_world_set_block(block_world(s), x, y, z, b);
}
static bool rs_mount_powered(void* c, int x, int y, int z, int side) {
	// Beta World.isBlockIndirectlyProvidingPowerTo(x,y,z,side): does the torch's mount block
	// deliver (strong) power onto the mount face? wiresProvidePower TRUE here -- a wire feeding
	// the mount block (directly, or through a solid block) DOES turn the torch off, which the
	// retired interim "any adjacent wire with strength>0" stand-in approximated only crudely
	// (it ignored the side, strong-through-cube, and the wire's connection-aware pointing).
	struct redstone_power_world pw = rs_power_view(c);
	return redstone_is_indirectly_providing_power_to(&pw, x, y, z, side, true);
}
static int64_t rs_world_time(void* c) {
	return (int64_t)block_world(((struct rs_ctx*)c)->s)->current_tick;
}

// --- the LOSSLESS drain: explicit continuation frames (next-gaps-0924/G5, Astra route c) --------
// Beta's drain is plain Java recursion: notifyBlocksOfNeighborChange -> a component's
// onNeighborBlockChange -> setBlockWithNotify -> an adjacent wire's onNeighborBlockChange -> a
// NESTED updateAndPropagateCurrentStrength (flood + its own drain) -> return -> the outer drain
// continues with its next notification. Every nested flood completes, synchronously, inside the
// originating tick. The pre-G5 port bounded that recursion with a depth guard that SKIPPED the
// nested flood beyond 12 levels ("settle next change") -- silent loss.
//
// Now every drain's state lives in a heap-growable FRAME (its position range, the position being
// drained and the next of its six directions), not in C locals. A nested wire change runs its
// flood IMMEDIATELY (wire metadata settles at the Beta point) and pushes its frame:
//   * below RS_WIRE_FLOOD_MAX_DEPTH C-level drains it drains that frame right there (plain
//     recursion: Beta's order -- the chain-12 golden pins the drop order);
//   * at/above it the frame is a CONTINUATION: the call returns without growing the C stack, and
//     the enclosing drain loop -- whose visit reached it -- drains that frame (and any siblings
//     requested during the same visit, in request order) BEFORE its own next notification.
//     Nothing is skipped; the only reordering is that the remainder of the handler which
//     triggered the flood (e.g. its set_block's other neighbour notifies) runs before the
//     continuation's component notifies instead of after. That residue exists only past depth 12
//     -- where the pre-G5 code lost the whole flood -- and is recorded in docs/deviations.md.
//
// COST MODEL (honest): the whole cascade runs inside ONE server tick, so a big circuit is a
// synchronous server-tick STALL bounded by the circuit's size, exactly like Beta. The drain calls
// thread_yield every 32 notifications and the flood every 64 pops, but on the Wii the server
// thread (SERVER_THREAD_PRIORITY 63) is already below the main/render thread (64), which preempts
// it on its own; LWP_YieldThread only hands the core to EQUAL-priority peers, so these yields do
// not protect render (the priority does) and cannot shorten the stall. They are harmless pacing:
// yielding never advances a tick or runs any other mutation.
//
// ALLOCATION FAILURE is explicit and recoverable: growth is retried after cooperative yields; a
// persistent failure SUSPENDS (frames, positions and the flood worklist all retained, counters
// bumped, one stderr line per episode) and the retained work is resumed FIRST by the next server
// tick (server_world_tick calls redstone_server_service_pending before advancing) or any earlier
// redstone entry. A wire change arriving while suspended is queued in a static request ring
// (drained after the retained frames). Only if that ring also overflows while memory stays
// exhausted is a request lost -- counted (redstone_server_requests_lost) and logged.
struct rs_frame {
	uint32_t mark_begin, mark_end; // this flood's positions [begin, end)
	uint32_t next_mark;			   // position being drained
	uint8_t dir;				   // next of its six neighbours (Beta -X,+X,-Y,+Y,-Z,+Z)
	bool flood_pending;			   // flood suspended on allocation failure (resume first)
};
#define RS_FRAMES_INITIAL 16
static struct rs_frame s_frames_static[RS_FRAMES_INITIAL];
static struct rs_frame* s_frames = s_frames_static;
static size_t s_frames_cap = RS_FRAMES_INITIAL;
static size_t s_frames_n = 0;
static size_t s_frames_peak = 0;

#define RS_REQ_MAX 256
static struct rs_cell s_req[RS_REQ_MAX];
static size_t s_req_head = 0, s_req_count = 0;
/* A2-T D4 residual: retained work carries its world. A frame stack only ever
 * holds ONE world's floods (a flood's notifications stay in block_world), so
 * the stack records the world of its first frame; each queued request records
 * the world it arrived from. rs_service replays each in its own world. */
static struct server_world* s_frames_world = NULL;
static struct server_world* s_req_world[RS_REQ_MAX];
static int8_t s_req_removed_meta[RS_REQ_MAX]; // -1 = wire request; else a removed cell's old meta

static int s_drain_depth = 0; // C-level drains currently on the stack
static bool s_suspended = false;
static unsigned s_drain_yield_ctr = 0;

static int s_wire_flood_depth_max = 0;
static int s_wire_flood_continuations = 0;
static unsigned s_suspend_count = 0;
static unsigned s_requests_queued = 0;
static unsigned s_requests_lost = 0;
static bool s_fail_logged = false; // one stderr line per suspension episode
static uint32_t s_drain_visits = 0;
static void (*s_visit_hook)(int x, int y, int z) = NULL;

int redstone_server_wire_flood_depth_max(void) { return s_wire_flood_depth_max; }
int redstone_server_wire_flood_depth_cap_hits(void) { return s_wire_flood_continuations; }
int redstone_server_wire_flood_continuations(void) { return s_wire_flood_continuations; }
unsigned redstone_server_suspends(void) { return s_suspend_count; }
unsigned redstone_server_requests_queued(void) { return s_requests_queued; }
unsigned redstone_server_requests_lost(void) { return s_requests_lost; }
unsigned redstone_server_marks_dedup_hits(void) { return s_marks_dedup_hits; }
uint32_t redstone_server_drain_visits(void) { return s_drain_visits; }
size_t redstone_server_request_ring_count(void) { return s_req_count; }
size_t redstone_server_marks_peak(void) { return s_marks_peak; }
size_t redstone_server_frames_peak(void) { return s_frames_peak; }
size_t redstone_server_store_heap_peak_bytes(void) { return s_store_heap_peak; }
size_t redstone_server_store_heap_bytes(void) {
	return s_marks_heap_bytes + s_hash_heap_bytes + s_frames_heap_bytes
		+ redstone_wire_update_heap_bytes();
}
void redstone_server_set_visit_hook(void (*hook)(int x, int y, int z)) { s_visit_hook = hook; }
bool redstone_server_pending(void) {
	return s_frames_n > 0 || s_req_count > 0 || redstone_wire_update_pending();
}
void redstone_server_wire_flood_diag_reset(void) {
	s_wire_flood_depth_max = 0;
	s_wire_flood_continuations = 0;
	s_marks_peak = s_marks_n;
	s_frames_peak = s_frames_n;
	s_marks_dedup_hits = 0;
	s_drain_visits = 0;
	s_store_heap_peak = s_marks_heap_bytes + s_hash_heap_bytes + s_frames_heap_bytes;
	redstone_wire_update_diag_reset();
}

// Idle: hand every heap-grown store back and return to the static ones, so a one-off giant
// cascade does not pin (or fragment) heap for the rest of the session.
static void rs_stores_release(void) {
	if(s_drain_depth != 0 || redstone_server_pending())
		return;
	s_marks_n = 0;
	s_marks_flood_begin = 0;
	s_fail_logged = false; // the episode (if any) is over: everything retained has completed
	if(s_marks != s_marks_static) {
		redstone_free(s_marks);
		s_marks = s_marks_static;
		s_marks_cap = RS_MARKS_INITIAL;
		s_marks_heap_bytes = 0;
	}
	if(s_hash != s_hash_static) {
		redstone_free(s_hash);
		s_hash = s_hash_static;
		s_hash_cap = RS_HASH_INITIAL;
		memset(s_hash_static, 0, sizeof s_hash_static); // stale generations must not alias
		s_hash_gen = 1;
		s_hash_heap_bytes = 0;
	}
	if(s_frames != s_frames_static) {
		redstone_free(s_frames);
		s_frames = s_frames_static;
		s_frames_cap = RS_FRAMES_INITIAL;
		s_frames_heap_bytes = 0;
	}
}


static void rs_report_failure(const char* what) {
	s_suspend_count++;
	if(s_fail_logged)
		return;
	s_fail_logged = true;
	fprintf(stderr,
			"redstone: allocation failed (%s): cascade SUSPENDED, %zu frame(s) + %zu queued "
			"request(s) retained for resume\n",
			what, s_frames_n, s_req_count);
}

static bool rs_frames_reserve(size_t need) {
	for(int attempt = 0;; attempt++) {
		if(need <= s_frames_cap)
			return true;
		size_t ncap = s_frames_cap * 2;
		struct rs_frame* nf = redstone_alloc(ncap * sizeof(*nf));
		if(nf) {
			memcpy(nf, s_frames, s_frames_n * sizeof(*nf));
			if(s_frames != s_frames_static)
				redstone_free(s_frames);
			s_frames = nf;
			s_frames_cap = ncap;
			s_frames_heap_bytes = ncap * sizeof(*nf);
			rs_note_heap();
			continue;
		}
		if(attempt >= 3)
			return false;
		thread_yield(); // transient pressure: let other threads run (no world state advances)
	}
}

static void rs_req_push(struct server_world* w, int x, int y, int z, int removed_meta) {
	if(s_req_count >= RS_REQ_MAX) {
		s_requests_lost++;
		fprintf(stderr,
				"redstone: request ring full while suspended -- wire update at %d %d %d LOST "
				"(%u total)\n",
				x, y, z, s_requests_lost);
		return;
	}
	s_req[(s_req_head + s_req_count) % RS_REQ_MAX] = (struct rs_cell) {x, y, z};
	s_req_world[(s_req_head + s_req_count) % RS_REQ_MAX] = w;
	s_req_removed_meta[(s_req_head + s_req_count) % RS_REQ_MAX] = (int8_t)removed_meta;
	s_req_count++;
	s_requests_queued++;
}

static struct redstone_world rs_flood_world(struct rs_ctx* ctx) {
	return (struct redstone_world) {
		.ctx = ctx,
		.get_type = rs_get_type,
		.get_meta = rs_get_meta,
		.set_meta = rs_set_meta,
		.is_normal_cube = rs_is_normal_cube,
		.is_indirectly_powered = rs_wire_is_powered,
		.mark_needs_update = rs_mark_accumulate,
		.reserve = rs_marks_reserve,
		.yield = thread_yield, // pacing only (see COST MODEL above)
	};
}

// redstone_logic.c (BlockRedstoneWire.onBlockRemoval:227); declared here because
// redstone_logic.h is outside PAR-WIRE-REMOVAL-ORDER's allowed paths.
bool redstone_wire_update_removed(struct redstone_world* w, int x, int y, int z, int old_meta);

// Push a frame (capacity already reserved) and run its flood. Sets s_suspended on failure.
// removed_meta >= 0: the flood is the removed cell's recalculation with that retained metadata.
static void rs_flood_start(struct server_local* s, int x, int y, int z, int removed_meta) {
	assert(s_frames_n < s_frames_cap);
	if(s_frames_n == 0)
		s_frames_world = block_world(s);
	struct rs_frame* f = &s_frames[s_frames_n++];
	if(s_frames_n > s_frames_peak)
		s_frames_peak = s_frames_n;
	f->mark_begin = f->mark_end = f->next_mark = (uint32_t)s_marks_n;
	f->dir = 0;
	f->flood_pending = false;
	s_marks_flood_begin = s_marks_n;
	rs_hash_new_flood();
	struct rs_ctx ctx = {s};
	struct redstone_world w = rs_flood_world(&ctx);
	bool done = removed_meta < 0 ? redstone_wire_update(&w, x, y, z)
								 : redstone_wire_update_removed(&w, x, y, z, removed_meta);
	f = &s_frames[s_frames_n - 1]; // (the flood never pushes frames; re-fetch for clarity)
	f->mark_end = (uint32_t)s_marks_n;
	if(!done) {
		f->flood_pending = true;
		s_suspended = true;
		rs_report_failure("wire flood worklist/positions");
	}
}
static bool rs_flood_resume(struct server_local* s, size_t fi) {
	struct rs_ctx ctx = {s};
	struct redstone_world w = rs_flood_world(&ctx);
	bool done = redstone_wire_update_resume(&w);
	s_frames[fi].mark_end = (uint32_t)s_marks_n;
	if(!done) {
		s_suspended = true;
		rs_report_failure("wire flood resume");
		return false;
	}
	s_frames[fi].flood_pending = false;
	return true;
}

static void rs_frame_pop(void) {
	s_frames_n--;
	// Continuation siblings are drained out of push order, so the frame being popped may not own
	// the TOP position block: keep every surviving frame's positions (truncate to the highest end).
	size_t top = 0;
	for(size_t i = 0; i < s_frames_n; i++)
		if(s_frames[i].mark_end > top)
			top = s_frames[i].mark_end;
	s_marks_n = top;
}

static void rs_frames_reverse(size_t lo, size_t hi) {
	while(hi > lo + 1) {
		struct rs_frame t = s_frames[lo];
		s_frames[lo] = s_frames[hi - 1];
		s_frames[hi - 1] = t;
		lo++;
		hi--;
	}
}

// World.notifyBlocksOfNeighborChange (World.java:498-505): -X, +X, -Y, +Y, -Z, +Z.
static const int8_t RS_NDX[6] = {-1, 1, 0, 0, 0, 0};
static const int8_t RS_NDY[6] = {0, 0, -1, 1, 0, 0};
static const int8_t RS_NDZ[6] = {0, 0, 0, 0, -1, 1};

// Drain frames above `base` (LIFO) until they are gone or the cascade suspends.
static void rs_drain(struct server_local* s, size_t base) {
	struct rs_ctx ctx = {s};
	while(!s_suspended && s_frames_n > base) {
		if(server_world_simulation_stopped(s_frames_world)) break;
		size_t fi = s_frames_n - 1;
		if(s_frames[fi].flood_pending) {
			if(!rs_flood_resume(s, fi))
				break;
			continue;
		}
		struct rs_frame* f = &s_frames[fi];
		if(f->dir >= 6) {
			f->dir = 0;
			f->next_mark++;
		}
		if(f->next_mark >= f->mark_end) {
			rs_frame_pop();
			continue;
		}
		// Headroom for the frame of a flood this visit may reach, secured BEFORE the visit.
		if(!rs_frames_reserve(s_frames_n + 1)) {
			s_suspended = true;
			rs_report_failure("continuation frames");
			break;
		}
		f = &s_frames[fi];
		const struct rs_cell p = s_marks[f->next_mark];
		const int d = f->dir++;
		const int tx = p.x + RS_NDX[d], ty = p.y + RS_NDY[d], tz = p.z + RS_NDZ[d];
		s_drain_visits++;
		if(s_visit_hook)
			s_visit_hook(tx, ty, tz);
		if((++s_drain_yield_ctr & 31u) == 0)
			thread_yield(); // pacing only: no tick advances, no unrelated mutation runs
		size_t before = s_frames_n;
		rs_mark_visit(&ctx, tx, ty, tz);
		if(server_world_simulation_stopped(s_frames_world)) break;
		// Continuations requested during this visit drain in REQUEST order (first on top).
		if(!s_suspended && s_frames_n > before + 1)
			rs_frames_reverse(before, s_frames_n);
	}
}

// Finish retained work (suspended frames, then queued requests) -- only from a top-level entry.
static void rs_service(struct server_local* s) {
	if(s_drain_depth != 0
	   || (s_frames_n && server_world_simulation_stopped(s_frames_world)))
		return;
	s_suspended = false;
	s_drain_depth++;
	if(s_frames_n) {
		/* retained frames replay in the world that started them */
		struct server_world* bw_prev = block_world_enter(s, s_frames_world);
		rs_drain(s, 0);
		block_world_leave(s, bw_prev);
	}
	while(!s_suspended && s_frames_n == 0 && s_req_count > 0) {
		if(server_world_simulation_stopped(s_req_world[s_req_head])) break;
		if(!rs_frames_reserve(1)) {
			s_suspended = true;
			rs_report_failure("request resume");
			break;
		}
		struct rs_cell r = s_req[s_req_head];
		struct server_world* rw = s_req_world[s_req_head];
		const int rmeta = s_req_removed_meta[s_req_head];
		s_req_head = (s_req_head + 1) % RS_REQ_MAX;
		s_req_count--;
		struct server_world* bw_prev = block_world_enter(s, rw);
		struct block_data b;
		if(rmeta >= 0
		   || (server_world_get_block(block_world(s), r.x, r.y, r.z, &b)
			   && b.type == BLOCK_REDSTONE_WIRE)) {
			rs_flood_start(s, r.x, r.y, r.z, rmeta);
			rs_drain(s, 0);
		} // else the wire is gone: its flood would be a no-op (redstone_wire_update)
		block_world_leave(s, bw_prev);
	}
	s_drain_depth--;
	rs_stores_release();
}

// World teardown (server_world_destroy/discard -- also every dimension switch, which recreates
// s->world): work retained by an allocation-failure suspension refers to the OLD world's cells and
// must never be replayed into the new one. Drop it, zero the ring and give every heap store back.
void redstone_server_reset(void) {
	assert(s_drain_depth == 0); // never torn down from inside a cascade
	if(s_frames_n || s_req_count || redstone_wire_update_pending())
		fprintf(stderr, "redstone: world teardown discards %zu retained frame(s) + %zu request(s)\n",
				s_frames_n, s_req_count);
	s_frames_n = 0;
	s_frames_world = NULL;
	s_req_head = s_req_count = 0;
	s_suspended = false;
	redstone_wire_update_reset();
	rs_stores_release();
	s_fail_logged = false;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Test seam: queue a wire request tagged with world `w`, exactly as a
 * suspended rs_service would have retained it. */
void redstone_server_test_queue_request(struct server_world* w, int x, int y,
										int z) {
	rs_req_push(w, x, y, z, -1);
}
#endif

void redstone_server_service_pending(struct server_local* s) {
	if(s_drain_depth == 0 && redstone_server_pending())
		rs_service(s);
}

// One wire flood + its drain (updateAndPropagateCurrentStrength :45-55); removed_meta as above.
static void rs_wire_flood_entry(struct server_local* s, int x, int y, int z, int removed_meta) {
	if(server_world_simulation_stopped(block_world(s))) return;
	if(s_drain_depth == 0 && redstone_server_pending())
		rs_service(s); // older retained work first
	if(server_world_simulation_stopped(block_world(s))
	   || (s_frames_n && server_world_simulation_stopped(s_frames_world))
	   || (s_req_count && server_world_simulation_stopped(s_req_world[s_req_head]))) return;
	if(s_suspended || redstone_wire_update_pending()) {
		rs_req_push(block_world(s), x, y, z, removed_meta); // cannot proceed in order right now: retain, never drop
		return;
	}
	if(!rs_frames_reserve(s_frames_n + 1)) {
		s_suspended = true;
		rs_req_push(block_world(s), x, y, z, removed_meta);
		rs_report_failure("continuation frames");
		return;
	}
	const size_t base = s_frames_n;
	rs_flood_start(s, x, y, z, removed_meta);
	if(s_drain_depth >= RS_WIRE_FLOOD_MAX_DEPTH) {
		// CONTINUATION: the enclosing drain loop (the visit that reached us) drains this frame
		// before its next notification. The C stack never holds more than
		// RS_WIRE_FLOOD_MAX_DEPTH drains.
		s_wire_flood_continuations++;
		return;
	}
	s_drain_depth++;
	if(s_drain_depth > s_wire_flood_depth_max)
		s_wire_flood_depth_max = s_drain_depth;
	rs_drain(s, base);
	s_drain_depth--;
	rs_stores_release();
}

void redstone_wire_neighbour_change(struct server_local* s, struct block_info* info) {
	rs_wire_flood_entry(s, info->x, info->y, info->z, -1);
}

static void rs_notify_wire_neighbours(
	struct server_local* s, int x, int y, int z) {
	struct block_data b;
	if(!server_world_get_block(block_world(s), x, y, z, &b)
	   || b.type != BLOCK_REDSTONE_WIRE)
		return;
	/* BlockRedstoneWire.notifyWireNeighborsOfNeighborChange: notify around the
	 * wire itself, then around each of its six face neighbours in Java order. */
	static const int dx[7] = {0, -1, 1, 0, 0, 0, 0};
	static const int dy[7] = {0, 0, 0, 0, 0, -1, 1};
	static const int dz[7] = {0, 0, 0, -1, 1, 0, 0};
	for(int i = 0; i < 7; i++)
		notifyNeighbours_from(
			s, x + dx[i], y + dy[i], z + dz[i], BLOCK_REDSTONE_WIRE);
}

void redstone_wire_removal(struct server_local* s, struct block_info* info) {
	assert(s && info);
	const int x = info->x, y = info->y, z = info->z;
	notifyNeighbours_from(s, x, y + 1, z, BLOCK_REDSTONE_WIRE);
	notifyNeighbours_from(s, x, y - 1, z, BLOCK_REDSTONE_WIRE);
	// :227 updateAndPropagateCurrentStrength at the removed cell, with the retained old metadata
	// (Chunk.setBlockID :278-289), BEFORE the horizontal notifies below.
	assert(info->block); // the captured OLD cell (server_world_dispatch_block_removal)
	rs_wire_flood_entry(s, x, y, z, info->block->metadata & 0x0F);

	struct rs_ctx ctx = {s};
	static const int sx[4] = {-1, 1, 0, 0};
	static const int sz[4] = {0, 0, -1, 1};
	for(int i = 0; i < 4; i++)
		rs_notify_wire_neighbours(s, x + sx[i], y, z + sz[i]);
	for(int i = 0; i < 4; i++) {
		int nx = x + sx[i], nz = z + sz[i];
		int ny = rs_is_normal_cube(&ctx, nx, y, nz) ? y + 1 : y - 1;
		rs_notify_wire_neighbours(s, nx, ny, nz);
	}
}

bool redstone_wire_place(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	if(!block_place_default(s, actor, it, where, on, on_side))
		return false;
	struct block_info self = *where; // flood the freshly placed wire so it settles at once
	redstone_wire_neighbour_change(s, &self);
	return true;
}

float redstone_torch_burnout_effect_draws(
	struct random_gen* g, int x, int y, int z,
	double smoke[REDSTONE_TORCH_BURNOUT_SMOKE][3]) {
	assert(g);
	/* Beta server BlockRedstoneTorch.updateTick:126-131. The fizz pitch takes
	 * two World.rand.nextFloat (first minus second); then five smoke
	 * particles each take three var5.nextDouble in x, y, z order, and var5 IS
	 * World.rand (World.java:833/1540 pass this.rand to updateTick). So one
	 * burnout costs 2 + 15 = 17 simulation draws, all on s->rand_src, drawn
	 * whether or not anyone hears/sees the effect (the dedicated server
	 * evaluates the arguments even though spawnParticle is a no-op there).
	 * nextDouble on this xorshift stream = one draw scaled by 2^-32, the
	 * spawner.c / dispenser precedent. [740c583: BlockRedstoneTorch:126-131] */
	const float r1 = rand_gen_flt(g);
	const float r2 = rand_gen_flt(g);
	for(int i = 0; i < REDSTONE_TORCH_BURNOUT_SMOKE; i++) {
		const double dx = (double)rand_gen(g) / 4294967296.0;
		const double dy = (double)rand_gen(g) / 4294967296.0;
		const double dz = (double)rand_gen(g) / 4294967296.0;
		smoke[i][0] = (double)x + dx * 0.6 + 0.2;
		smoke[i][1] = (double)y + dy * 0.6 + 0.2;
		smoke[i][2] = (double)z + dz * 0.6 + 0.2;
	}
	return 2.6F + (r1 - r2) * 0.8F;
}

static void rs_torch_burnout(void* c, int x, int y, int z) {
	struct server_local* s = ((struct rs_ctx*)c)->s;
	struct server_world* world = block_world(s);
	// Beta server BlockRedstoneTorch.updateTick:122-131: idle swap and
	// burnout check precede fizz, then five smoke particles. The 17 draws
	// are simulation-stream draws (World.rand), taken unconditionally.
	double smoke[REDSTONE_TORCH_BURNOUT_SMOKE][3];
	const float pitch = redstone_torch_burnout_effect_draws(&s->rand_src,
		x, y, z, smoke);
	server_local_emit_sound_w(world, SOUND_RANDOM_FIZZ,
		(vec3) {x + 0.5F, y + 0.5F, z + 0.5F}, 0.5F, pitch);
	// Existing client smoke puff: ceil(4*1.25) = five particles. It renders
	// around the cell center with its own client jitter/motion/size/lifetime,
	// NOT at the five Beta positions computed above (those are drawn for
	// stream parity only); see docs/deviations.md.
	if(!server_local_world_fx_ok(world))
		return;
	struct client_rpc puff = {
		.type = CRPC_SMOKE_PUFF,
		.payload.smoke_puff.pos = {x + 0.5F, y + 0.5F, z + 0.5F},
		.payload.smoke_puff.intensity = 1.25F,
	};
	clin_rpc_stamp(&puff, world->world_index, world->generation);
	clin_rpc_send(&puff);
}

void redstone_torch_tick(struct server_local* s, struct block_info* info) {
	redstone_server_service_pending(s); // retained (OOM-suspended) cascade work first
	struct rs_ctx ctx = {s};
	struct redstone_world w = {
		.ctx = &ctx,
		.get_type = rs_get_type,
		.get_meta = rs_get_meta,
		.set_type = rs_set_type,
		.is_indirectly_providing_power = rs_mount_powered,
		.get_world_time = rs_world_time,
		.on_burnout = rs_torch_burnout,
	};
	redstone_torch_update(&w, info->x, info->y, info->z);
}

// --- repeater accessors (Beta BlockRedstoneRepeater) ---
// Beta World.isBlockIndirectlyProvidingPowerTo on the repeater's INPUT side -- wiresProvidePower
// TRUE (a non-wire querying its back face, exactly like the torch mount, reusing the same model).
static bool rs_repeater_input_power(void* c, int x, int y, int z, int side) {
	struct redstone_power_world pw = rs_power_view(c);
	return redstone_is_indirectly_providing_power_to(&pw, x, y, z, side, true);
}
// Exposed (redstone_server.h): the piston power query reuses the faithful server-world power view rather
// than rebuilding rs_is_normal_cube's TNT carve-out.
bool redstone_server_indirect_power(struct server_local* s, int x, int y, int z, int side, bool wpp) {
	struct rs_ctx ctx = {s};
	struct redstone_power_world pw = rs_power_view(&ctx);
	return redstone_is_indirectly_providing_power_to(&pw, x, y, z, side, wpp);
}

// Beta World.isBlockIndirectlyGettingPowered(x,y,z) over the live server world -- "does ANY of
// the 6 neighbours deliver strong (cube-re-emittable) power onto this cell?" The door's power
// query (BlockDoor.onNeighborBlockChange reads it for the bottom AND top cell).
bool redstone_server_indirectly_getting_powered(struct server_local* s, int x, int y, int z) {
	struct rs_ctx ctx = {s};
	struct redstone_power_world pw = rs_power_view(&ctx);
	return redstone_is_indirectly_getting_powered(&pw, x, y, z, true);
}

// Beta World.isBlockGettingPowered(x,y,z) over the live server world -- the WEAK/direct-adjacent-
// only check (noteblock-slice: BlockNote.onNeighborBlockChange). wpp=true: the SAME "external
// non-wire component" case as the indirectly-getting-powered wrapper above (a note block counts
// an adjacent wire pointing at it, exactly like a door/dispenser would).
bool redstone_server_getting_powered(struct server_local* s, int x, int y, int z) {
	struct rs_ctx ctx = {s};
	struct redstone_power_world pw = rs_power_view(&ctx);
	return redstone_is_getting_powered(&pw, x, y, z, true);
}
// Swap the repeater id idle<->active, KEEPING the low metadata nibble (facing+delay), with a FULL
// neighbour-notify so the new output re-floods adjacent wire (Beta setBlockAndMetadataWithNotify).
static void rs_repeater_set_active(void* c, int x, int y, int z, bool active) {
	struct server_local* s = ((struct rs_ctx*)c)->s;
	struct block_data b;
	if(!server_world_get_block(block_world(s), x, y, z, &b))
		return;
	b.type = (uint8_t)(active ? RS_REPEATER_ACTIVE_ID : RS_REPEATER_IDLE_ID);
	server_world_set_block(block_world(s), x, y, z, b);
}
// Beta World.scheduleBlockUpdate(x,y,z,id,delay): arm the toggle `delay` game-ticks out. The
// (pos,block_type) dedup mirrors Beta's NextTickListEntry contains() gate.
static void rs_repeater_schedule(void* c, int x, int y, int z, int id, int delay) {
	struct server_local* s = ((struct rs_ctx*)c)->s;
	server_world_schedule_tick(block_world(s), x, y, z, (uint8_t)id, delay);
}
static struct redstone_repeater_world rs_repeater_world(struct rs_ctx* ctx) {
	return (struct redstone_repeater_world) {
		.ctx = ctx,
		.get_type = rs_get_type,
		.get_meta = rs_get_meta,
		.is_indirectly_providing_power = rs_repeater_input_power,
		.set_active = rs_repeater_set_active,
		.schedule = rs_repeater_schedule,
	};
}

// Beta BlockRedstoneRepeater.updateTick (the scheduled toggle half), dispatched by the
// scheduled-tick queue (onWorldTick) when the self-armed metadata delay elapses.
void redstone_repeater_server_tick(struct server_local* s, struct block_info* info) {
	redstone_server_service_pending(s); // retained (OOM-suspended) cascade work first
	struct rs_ctx ctx = {s};
	struct redstone_repeater_world w = rs_repeater_world(&ctx);
	redstone_repeater_tick(&w, info->x, info->y, info->z);
}

// Beta BlockRedstoneRepeater.onNeighborBlockChange: FIRST the canBlockStay drop (the block below
// must be a normal cube; else drop the item + set air), THEN the pure scheduling half (arm the
// delayed toggle when the input disagrees with the output).
void redstone_repeater_server_neighbour_change(struct server_local* s, struct block_info* info) {
	struct rs_ctx ctx = {s};
	if(!rs_is_normal_cube(&ctx, info->x, info->y - 1, info->z)) {
		server_local_spawn_block_drops(s, info); // dropBlockAsItem(meta)
		server_world_set_block(block_world(s), info->x, info->y, info->z,
							   (struct block_data) {.type = BLOCK_AIR}); // setBlockWithNotify(...,0)
		return;
	}
	struct redstone_repeater_world w = rs_repeater_world(&ctx);
	redstone_repeater_neighbour_change(&w, info->x, info->y, info->z);
}

// Beta BlockRedstoneRepeater.onBlockPlacedBy: place the IDLE repeater with the given facing (low
// nibble, delay index 0 = 2-tick), then -- if its input side is ALREADY powered -- arm the toggle
// at delay 1 so it turns on next tick. (Placing the block does not fire the repeater's own
// onNeighbourBlockChange, so this explicit schedule is what makes a repeater placed onto live
// power switch on, exactly as Beta does.) Returns false if the cell is entity-obstructed.
bool redstone_repeater_place(struct server_local* s, struct server_player* actor,
		struct block_info* where, int facing) {
	struct rs_ctx ctx = {s};
	// Beta canPlaceBlockAt: a repeater requires a normal cube directly below (it cannot float).
	// Beta refuses the placement outright (item not consumed); without this the place would
	// succeed and only self-correct on a later neighbour change via the canBlockStay drop.
	if(!rs_is_normal_cube(&ctx, where->x, where->y - 1, where->z))
		return false;
	struct block_data blk = {.type = RS_REPEATER_IDLE_ID, .metadata = (uint8_t)(facing & 0x0F)};
	struct block_info bi = *where;
	bi.block = &blk;
	if(block_place_obstructed_for(s, actor, &bi))
		return false;
	server_world_set_block(block_world(s), where->x, where->y, where->z, blk);
	struct redstone_repeater_world w = rs_repeater_world(&ctx);
	if(redstone_repeater_input_powered(&w, where->x, where->y, where->z))
		server_world_schedule_tick(block_world(s), where->x, where->y, where->z,
								   RS_REPEATER_IDLE_ID, 1);
	return true;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
int redstone_server_test_drain_depth(void) { return s_drain_depth; }
#endif

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Pointer-free retained ownership/order; drain depth is unwind, checked alone. */
size_t redstone_server_test_snapshot(uint64_t* out, size_t capacity) {
	size_t n = 0;
#define RS_SNAPSHOT(v) do { if(out && n < capacity) out[n] = (uint64_t)(v); n++; } while(0)
	RS_SNAPSHOT(s_frames_n); RS_SNAPSHOT(s_marks_n);
	RS_SNAPSHOT(s_req_count); RS_SNAPSHOT(s_suspended);
	RS_SNAPSHOT(s_frames_world != NULL);
	if(s_frames_world) {
		RS_SNAPSHOT(s_frames_world->world_index);
		RS_SNAPSHOT(s_frames_world->dimension);
	}
	for(size_t i=0;i<s_frames_n;i++) {
		const struct rs_frame* f=&s_frames[i];
		RS_SNAPSHOT(f->mark_begin); RS_SNAPSHOT(f->mark_end);
		RS_SNAPSHOT(f->next_mark); RS_SNAPSHOT(f->dir); RS_SNAPSHOT(f->flood_pending);
	}
	for(size_t i=0;i<s_marks_n;i++) {
		RS_SNAPSHOT(s_marks[i].x); RS_SNAPSHOT(s_marks[i].y); RS_SNAPSHOT(s_marks[i].z);
	}
	for(size_t i=0;i<s_req_count;i++) {
		size_t j=(s_req_head+i)%RS_REQ_MAX;
		RS_SNAPSHOT(s_req_world[j]->world_index); RS_SNAPSHOT(s_req_world[j]->dimension);
		RS_SNAPSHOT(s_req[j].x); RS_SNAPSHOT(s_req[j].y); RS_SNAPSHOT(s_req[j].z);
	}
#undef RS_SNAPSHOT
	return n;
}
#endif
