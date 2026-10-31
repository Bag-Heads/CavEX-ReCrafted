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

#include "redstone_logic.h"

#include <assert.h>
#include <stdlib.h>

// --- Faithfulness note (read before changing) -------------------------------------------
// Beta BlockRedstoneWire is SYNCHRONOUS: updateAndPropagateCurrentStrength runs
// calculateCurrentChanges (a recursion with an origin-cell exclusion) and then drains
// blocksNeedingUpdate into notifyBlocksOfNeighborChange -> onNeighborBlockChange -> a fresh
// updateAndPropagateCurrentStrength on each WIRE neighbour. The recursion's origin exclusion
// means it does NOT itself carry a straight run; the gradient forms through that synchronous
// notify cascade, which re-runs the per-cell calc (origin = self, reading ALL neighbours)
// until the network is stable. Because the whole thing settles within the ONE originating
// tick (0 game-ticks over any length), the ONLY observable is the settled state -- the unique
// FIXPOINT of Beta's per-cell rule (var9 = 15 if indirectly powered, else max over
// neighbour-wire metadata, minus one per hop). We compute that fixpoint directly with an
// iterative worklist. This is byte-equal to Beta's settled metadata for every wire and is
// Wii-stack-safe, where Beta's deep recursion is not (CLAUDE.md: a shortcut that is the right
// call, justified -- the trajectory is unobservable for an instant wire, only the fixpoint is).

// Beta BlockRedstoneWire.getMaxCurrentStrength: a non-wire neighbour contributes nothing; a
// wire neighbour contributes max(its metadata, running). The 0-15 gradient is read ONLY here.
static int get_max_current_strength(struct redstone_world* w, int x, int y, int z, int cur) {
	if(w->get_type(w->ctx, x, y, z) != REDSTONE_WIRE_ID)
		return cur;
	int m = w->get_meta(w->ctx, x, y, z);
	return m > cur ? m : cur;
}

// Beta calculateCurrentChanges step 1 (the per-cell target strength), with origin = self so
// ALL neighbours are read (the fixpoint is identical with/without the recursion's origin
// exclusion -- exclusion is a recursion optimisation, not a fixpoint property). Includes the
// vertical stair probes: a wire reads the cell ABOVE a solid horizontal neighbour (when the
// block above this wire is open) and the cell BELOW a non-solid horizontal neighbour.
static int compute_strength(struct redstone_world* w, int x, int y, int z) {
	if(w->is_indirectly_powered(w->ctx, x, y, z))
		return REDSTONE_MAX_STRENGTH; // a non-wire source injects a flat 15
	int var9 = 0;
	for(int side = 0; side < 4; side++) {
		int nx = x, nz = z;
		if(side == 0)
			nx = x - 1;
		else if(side == 1)
			nx = x + 1;
		else if(side == 2)
			nz = z - 1;
		else
			nz = z + 1;
		var9 = get_max_current_strength(w, nx, y, nz, var9);
		if(w->is_normal_cube(w->ctx, nx, y, nz)
		   && !w->is_normal_cube(w->ctx, x, y + 1, z))
			var9 = get_max_current_strength(w, nx, y + 1, nz, var9);
		else if(!w->is_normal_cube(w->ctx, nx, y, nz))
			var9 = get_max_current_strength(w, nx, y - 1, nz, var9);
	}
	return var9 > 0 ? var9 - 1 : 0; // the single --strength per hop
}

// The wire cells (x,y,z) that THIS wire connects to (its read-set per compute_strength): the
// 4 horizontal neighbours plus the diagonal-up/down stair cell on each side. Wire connection
// is symmetric, so re-enqueueing this set on a change covers every wire whose value depends
// on this cell. Returns the count; fills out[][3].
static int wire_links(struct redstone_world* w, int x, int y, int z, int out[][3]) {
	int n = 0;
	for(int side = 0; side < 4; side++) {
		int nx = x, nz = z;
		if(side == 0)
			nx = x - 1;
		else if(side == 1)
			nx = x + 1;
		else if(side == 2)
			nz = z - 1;
		else
			nz = z + 1;
		int vy; // the stair cell's y for this side (matches compute_strength's probes)
		if(w->is_normal_cube(w->ctx, nx, y, nz))
			vy = (!w->is_normal_cube(w->ctx, x, y + 1, z)) ? y + 1 : y;
		else
			vy = y - 1;
		const int cells[2][3] = {{nx, y, nz}, {nx, vy, nz}};
		for(int k = 0; k < 2; k++)
			if(w->get_type(w->ctx, cells[k][0], cells[k][1], cells[k][2])
			   == REDSTONE_WIRE_ID) {
				out[n][0] = cells[k][0];
				out[n][1] = cells[k][1];
				out[n][2] = cells[k][2];
				n++;
			}
	}
	return n;
}

// LOSSLESS WORKLIST (next-gaps-0924/G5, Astra route c). Beta's calculateCurrentChanges recursion
// has no work bound, so neither may the fixpoint that replaces it: the old fixed 4096-entry ring
// DROPPED links once full ("remaining converge on next entry" -- they did not, when a dropped link
// had no other parent), which is silent simulation loss. The ring now starts in a FILE-STATIC
// buffer (the old 48KB, so the common flood never allocates and the Wii stack story below is
// unchanged) and GROWS onto the heap by doubling, copying the pending entries in FIFO order -- so
// the pop order is exactly that of an unbounded FIFO and every flood that never filled the old
// ring is byte-identical. Capacity for the worst-case re-queue (8 links) and for one deferred
// notify record (w->reserve) is secured BEFORE a cell is popped, so an allocation failure can
// never strand a half-processed cell: the flood SUSPENDS with every pending entry retained
// (redstone_wire_update returns false; redstone_wire_update_resume continues it). Failure is
// counted, never silent.
//
// WII STACK SAFETY (Wii-verify workflow, 2026-06-17 -- devkitPPC -fstack-usage measured): the
// worklist must stay OFF the 64KB server-thread stack (as function locals the old arrays were 75%
// of it, and a re-entrant nested flood overflowed with SILENT MEM2 corruption -- libogc memalign
// stacks have no guard page). File-static/heap storage is safe because redstone is
// server-thread-only AND redstone_wire_update is non-reentrant within itself (its set_meta is the
// QUIET no-notify write and mark_needs_update only RECORDS, so it never nests another wire_update
// during its own loop; nesting happens only in the caller's drain, AFTER this flood has completed).
// At most one flood is ever live (or suspended) -> one shared queue is race-free.
struct rs_q_cell {
	int x, y, z;
};
static struct rs_q_cell s_q_static[REDSTONE_WIRE_WORK_MAX];
static struct rs_q_cell* s_q = s_q_static;
static size_t s_q_cap = REDSTONE_WIRE_WORK_MAX;
static size_t s_q_head = 0, s_q_count = 0;
static bool s_q_pending = false; // a flood suspended on allocation failure, entries retained
static unsigned s_q_pops = 0;	 // pops of the live flood (yield cadence + max_pops diag)
static int s_wire_update_max_pops = 0;
static int s_wire_update_full_hits = 0; // entries that could not be queued: now ALWAYS 0 (lossless)
static size_t s_q_peak_count = 0;
static size_t s_q_peak_bytes = 0;
static unsigned s_q_grows = 0;
static unsigned s_alloc_failures = 0;
static unsigned s_wire_update_suspends = 0;

// Injected allocator (tests force failures to prove the suspend/resume path).
static void* (*s_alloc_fn)(size_t) = malloc;
static void (*s_free_fn)(void*) = free;
void redstone_logic_set_alloc_hooks(void* (*alloc_fn)(size_t), void (*free_fn)(void*)) {
	s_alloc_fn = alloc_fn ? alloc_fn : malloc;
	s_free_fn = free_fn ? free_fn : free;
}
void* redstone_alloc(size_t bytes) {
	void* p = s_alloc_fn(bytes);
	if(!p)
		s_alloc_failures++;
	return p;
}
void redstone_free(void* p) {
	if(p)
		s_free_fn(p);
}
unsigned redstone_alloc_failures(void) { return s_alloc_failures; }

int redstone_wire_update_max_pops(void) { return s_wire_update_max_pops; }
int redstone_wire_update_full_hits(void) { return s_wire_update_full_hits; }
size_t redstone_wire_update_peak_entries(void) { return s_q_peak_count; }
size_t redstone_wire_update_peak_heap_bytes(void) { return s_q_peak_bytes; }
unsigned redstone_wire_update_grows(void) { return s_q_grows; }
unsigned redstone_wire_update_suspends(void) { return s_wire_update_suspends; }
bool redstone_wire_update_pending(void) { return s_q_pending; }
size_t redstone_wire_update_heap_bytes(void) {
	return s_q == s_q_static ? 0 : s_q_cap * sizeof(struct rs_q_cell);
}
void redstone_wire_update_diag_reset(void) {
	s_wire_update_max_pops = 0;
	s_wire_update_full_hits = 0;
	s_q_peak_count = s_q_count;
	s_q_peak_bytes = (s_q == s_q_static) ? 0 : s_q_cap * sizeof(struct rs_q_cell);
	s_q_grows = 0;
	s_wire_update_suspends = 0;
}

// Grow the ring (doubling) so it can hold `need` entries; pending entries are copied in FIFO
// order to the front of the new buffer. Returns false (nothing changed) on allocation failure.
static bool rs_q_reserve(size_t need) {
	if(need <= s_q_cap)
		return true;
	size_t ncap = s_q_cap;
	while(ncap < need) {
		if(ncap > ((size_t)-1) / 2 / sizeof(struct rs_q_cell))
			return false;
		ncap *= 2;
	}
	struct rs_q_cell* nq = redstone_alloc(ncap * sizeof(struct rs_q_cell));
	if(!nq)
		return false;
	for(size_t i = 0; i < s_q_count; i++)
		nq[i] = s_q[(s_q_head + i) % s_q_cap];
	if(s_q != s_q_static)
		redstone_free(s_q);
	s_q = nq;
	s_q_cap = ncap;
	s_q_head = 0;
	s_q_grows++;
	size_t bytes = ncap * sizeof(struct rs_q_cell);
	if(bytes > s_q_peak_bytes)
		s_q_peak_bytes = bytes;
	return true;
}
static void rs_q_push(int x, int y, int z) {
	// Capacity was reserved before the pop that produced this push (never fails here).
	assert(s_q_count < s_q_cap);
	s_q[(s_q_head + s_q_count) % s_q_cap] = (struct rs_q_cell) {x, y, z};
	s_q_count++;
	if(s_q_count > s_q_peak_count)
		s_q_peak_count = s_q_count;
}

// Secure the worst case one pop can need (8 links + 1 deferred-notify record). A transient
// failure is retried after a cooperative yield (another thread may release memory); only a
// persistent one suspends the flood -- with the cell still queued, so nothing is lost.
#define RS_WIRE_LINKS_MAX 8
#define RS_ALLOC_RETRIES 3
static bool rs_reserve_step(struct redstone_world* w) {
	for(int attempt = 0;; attempt++) {
		bool ok = rs_q_reserve(s_q_count + RS_WIRE_LINKS_MAX)
			&& (!w->reserve || w->reserve(w->ctx));
		if(ok)
			return true;
		if(attempt >= RS_ALLOC_RETRIES)
			return false;
		if(w->yield)
			w->yield(); // pauses only this thread: the world is neither advanced nor mutated
	}
}

// Removed-cell seed mark (redstone_wire_update_removed), recorded once capacity is secured.
static bool s_seed_mark = false;
static struct rs_q_cell s_seed_cell;

static bool rs_wire_run(struct redstone_world* w) {
	// Iterative worklist fixpoint (replaces Beta's recursion + synchronous notify cascade,
	// same settled result). A cell is (re)computed; if its strength changed, its connected
	// wire neighbours are re-queued and its non-wire axis neighbours are flagged for the
	// caller's neighbour-notify (torches / repeaters / doors react there).
	if(s_seed_mark) {
		if(!rs_reserve_step(w)) {
			s_q_pending = true;
			s_wire_update_suspends++;
			return false; // the seed mark and every entry retained for the resume
		}
		s_seed_mark = false;
		w->mark_needs_update(w->ctx, s_seed_cell.x, s_seed_cell.y, s_seed_cell.z);
	}
	while(s_q_count > 0) {
		if(!rs_reserve_step(w)) {
			s_q_pending = true;
			s_wire_update_suspends++;
			return false; // every entry (incl. the unpopped head) retained for the resume
		}
		struct rs_q_cell c = s_q[s_q_head];
		s_q_head = (s_q_head + 1) % s_q_cap;
		s_q_count--;
		int cx = c.x, cy = c.y, cz = c.z;
		// Pacing only: yields to EQUAL-priority peers. On the Wii the server (63) is already below
		// render (64), so this neither protects render nor shortens the synchronous settle; it never
		// advances a tick or mutates anything else. No-op on PC / when yield is NULL.
		if((++s_q_pops & 63) == 0 && w->yield)
			w->yield();

		if(w->get_type(w->ctx, cx, cy, cz) != REDSTONE_WIRE_ID)
			continue;
		int old = w->get_meta(w->ctx, cx, cy, cz);
		int nv = compute_strength(w, cx, cy, cz);
		if(nv == old)
			continue;
		w->set_meta(w->ctx, cx, cy, cz, nv);
		// Beta calculateCurrentChanges populates blocksNeedingUpdate (the deferred neighbour-notify
		// set, drained by updateAndPropagateCurrentStrength into notifyBlocksOfNeighborChange) ONLY
		// inside the old-is-zero-or-scratch-is-zero branch. CRUCIAL QUIRK (decompile 740c583, verified directly):
		// that scratch value is NOT the new strength -- it is REUSED as scratch in the recursion loop and
		// ends as `(newStrength > 0 ? newStrength - 1 : 0)`, so scratch zero <=> newStrength <= 1.
		// Net Beta gate: notify the components only on a 0-crossing OR a drop to strength 1, i.e.
		// `old == 0 || nv <= 1`. A wire dimming/brightening between strengths >= 2 (e.g. 14->13, or
		// 1->2) notifies NOTHING in Beta. (mc173 notifies on every change -- LESS faithful here; the
		// decompile is authoritative.) The wire re-queue below stays UNCONDITIONAL: it mirrors the
		// recursion, which is independent of this notify gate.
		if(w->mark_needs_update && (old == 0 || nv <= 1))
			w->mark_needs_update(w->ctx, cx, cy, cz);

		// re-queue connected wires (capacity secured above: never dropped)
		int links[RS_WIRE_LINKS_MAX][3];
		int ln = wire_links(w, cx, cy, cz, links);
		for(int i = 0; i < ln; i++)
			rs_q_push(links[i][0], links[i][1], links[i][2]);
	}
	s_q_pending = false;
	// Completed: give a grown ring back and return to the static one (no heap pinned while idle).
	if(s_q != s_q_static) {
		redstone_free(s_q);
		s_q = s_q_static;
		s_q_cap = REDSTONE_WIRE_WORK_MAX;
		s_q_head = 0;
	}
	if((int)s_q_pops > s_wire_update_max_pops)
		s_wire_update_max_pops = (int)s_q_pops;
	return true;
}

bool redstone_wire_update(struct redstone_world* w, int x, int y, int z) {
	// One flood at a time: a suspended flood must be resumed (redstone_wire_update_resume) before
	// a new one starts, or the new one would run ahead of older pending work.
	assert(!s_q_pending && s_q_count == 0);
	s_q_head = 0;
	s_q_pops = 0;
	rs_q_push(x, y, z); // the ring always has >= REDSTONE_WIRE_WORK_MAX slots
	return rs_wire_run(w);
}

// Beta BlockRedstoneWire.onBlockRemoval:227 -- updateAndPropagateCurrentStrength AT THE REMOVED
// CELL, before the horizontal notifies (:228-254). Chunk.setBlockID (:278-289) runs onBlockRemoval
// with the new id but the OLD metadata nibble, so func_21030_a (:57-171) compares old_meta (var8)
// with the strength the vacated cell computes (:61-106). Equal: nothing (:108). Different: the
// cell's own deferred-notify mark under the same gate as a wire (:160-167) and a re-flood of the
// wires it links to (:114-158). The nibble write (:110) is skipped: Chunk overwrites it (:289) and
// nothing reads a non-wire cell's metadata (getMaxCurrentStrength :259-260).
bool redstone_wire_update_removed(struct redstone_world* w, int x, int y, int z, int old_meta) {
	assert(!s_q_pending && s_q_count == 0);
	s_q_head = 0;
	s_q_pops = 0;
	const int nv = compute_strength(w, x, y, z);
	if(nv == old_meta)
		return true;
	if(w->mark_needs_update && (old_meta == 0 || nv <= 1)) {
		s_seed_mark = true;
		s_seed_cell = (struct rs_q_cell) {x, y, z};
	}
	int links[RS_WIRE_LINKS_MAX][3];
	const int ln = wire_links(w, x, y, z, links);
	for(int i = 0; i < ln; i++) // the empty ring has >= REDSTONE_WIRE_WORK_MAX slots
		rs_q_push(links[i][0], links[i][1], links[i][2]);
	return rs_wire_run(w);
}

void redstone_wire_update_reset(void) {
	// World teardown: the suspended flood's cells belong to the destroyed world -- drop them.
	s_seed_mark = false;
	s_q_count = 0;
	s_q_head = 0;
	s_q_pending = false;
	if(s_q != s_q_static) {
		redstone_free(s_q);
		s_q = s_q_static;
		s_q_cap = REDSTONE_WIRE_WORK_MAX;
	}
}

bool redstone_wire_update_resume(struct redstone_world* w) {
	assert(s_q_pending);
	return rs_wire_run(w);
}

// --- Beta BlockRedstoneTorch: the inverter + burnout ------------------------------------

// Beta's static torchUpdates: ONE list shared by all torches, keyed by position, each entry a
// lit->idle toggle stamped with the world time. Process-static, matching Beta.
#define RS_BURNOUT_MAX 256
static struct {
	int x, y, z;
	int64_t time;
} s_torch_updates[RS_BURNOUT_MAX];
static int s_torch_updates_n;

void redstone_torch_reset_burnout(void) {
	s_torch_updates_n = 0;
}

// Beta checkForBurnout(x,y,z,record): if record, append (x,y,z,time); then count entries at
// (x,y,z) and return true once the count reaches 8 (the burnt-out threshold).
static bool torch_check_burnout(int x, int y, int z, int64_t time, bool record) {
	if(record) {
		// DOCUMENTED bounded-memory deviation (docs/redstone_plan.md), mirroring the wire worklist
		// cap. Beta's torchUpdates is an UNBOUNDED ArrayList; on the fixed-RAM Wii we cap at
		// RS_BURNOUT_MAX. When the (already 100-tick-pruned) list is saturated -- a contrived load
		// of many fast clocks ticking at once -- we evict the OLDEST entry (index 0, FIFO, nearest
		// the prune boundary anyway) BEFORE appending, rather than SILENTLY DROPPING the new record.
		// Silently dropping would let a real clock skip its >=8 burnout (a sim-output divergence);
		// keeping the most-recent RS_BURNOUT_MAX records preserves the per-position burnout count.
		if(s_torch_updates_n >= RS_BURNOUT_MAX) {
			for(int i = 1; i < s_torch_updates_n; i++)
				s_torch_updates[i - 1] = s_torch_updates[i];
			s_torch_updates_n--;
		}
		s_torch_updates[s_torch_updates_n].x = x;
		s_torch_updates[s_torch_updates_n].y = y;
		s_torch_updates[s_torch_updates_n].z = z;
		s_torch_updates[s_torch_updates_n].time = time;
		s_torch_updates_n++;
	}
	int count = 0;
	for(int i = 0; i < s_torch_updates_n; i++)
		if(s_torch_updates[i].x == x && s_torch_updates[i].y == y
		   && s_torch_updates[i].z == z) {
			if(++count >= 8)
				return true;
		}
	return false;
}

// Beta isIndirectlyPowered: is the torch's SINGLE attachment block (selected by the mount
// metadata 5/3/4/1/2) carrying power into the mount face (sides 0/2/3/4/5)?
static bool torch_attachment_powered(struct redstone_world* w, int x, int y, int z, int meta) {
	switch(meta) {
		case 5: return w->is_indirectly_providing_power(w->ctx, x, y - 1, z, 0);
		case 3: return w->is_indirectly_providing_power(w->ctx, x, y, z - 1, 2);
		case 4: return w->is_indirectly_providing_power(w->ctx, x, y, z + 1, 3);
		case 1: return w->is_indirectly_providing_power(w->ctx, x - 1, y, z, 4);
		case 2: return w->is_indirectly_providing_power(w->ctx, x + 1, y, z, 5);
		default: return false;
	}
}

void redstone_torch_update(struct redstone_world* w, int x, int y, int z) {
	int meta = w->get_meta(w->ctx, x, y, z);
	bool powered = torch_attachment_powered(w, x, y, z, meta); // var6 = isIndirectlyPowered
	int64_t t = w->get_world_time(w->ctx);

	// Prune burnout entries older than the 100-tick window (Beta: worldTime-updateTime > 100L).
	// DEFENCE-IN-DEPTH for a non-monotonic clock: Beta's worldTime is persisted+monotonic, but the
	// port feeds world.current_tick, which resets to 0 on every world (re)create. An entry whose
	// .time is in the FUTURE (t < .time) is therefore a leftover from a prior session/world whose
	// clock ran ahead -- it can never satisfy `t - .time > 100` (the diff is negative), so it would
	// pin a stale burnout forever. Treat it as stale and drop it too. (The primary fix clears the
	// whole list at the world boundary; this guards any path that doesn't.)
	while(s_torch_updates_n > 0
		  && (t - s_torch_updates[0].time > 100 || t < s_torch_updates[0].time)) {
		for(int i = 1; i < s_torch_updates_n; i++)
			s_torch_updates[i - 1] = s_torch_updates[i];
		s_torch_updates_n--;
	}

	bool active = (w->get_type(w->ctx, x, y, z) == REDSTONE_TORCH_LIT_ID);
	if(active) {
		if(powered) {
			// input ON -> torch OFF (always); RECORD the toggle (return is cosmetic fizz here)
			w->set_type(w->ctx, x, y, z, REDSTONE_TORCH_IDLE_ID);
			if(torch_check_burnout(x, y, z, t, true) && w->on_burnout)
				w->on_burnout(w->ctx, x, y, z);
		}
	} else if(!powered && !torch_check_burnout(x, y, z, t, false)) {
		// input OFF and NOT burnt out (CHECK only, no record) -> relight
		w->set_type(w->ctx, x, y, z, REDSTONE_TORCH_LIT_ID);
	}
}

// --- Beta wire neighbour-notify reach (the two-hop blocksNeedingUpdate drain) ----------------

void redstone_wire_notify_targets(int x, int y, int z, void* ctx,
								  void (*visit)(void* ctx, int x, int y, int z)) {
	static const int dx[6] = {1, -1, 0, 0, 0, 0};
	static const int dy[6] = {0, 0, 0, 0, 1, -1};
	static const int dz[6] = {0, 0, 1, -1, 0, 0};
	// Beta blocksNeedingUpdate for a single changed wire cell = {the cell} U its 6 face-neighbours;
	// each is drained via notifyBlocksOfNeighborChange, which notifies THAT cell's 6 neighbours.
	// So the target set is the union of the face-neighbours of those 7 origin cells (<= 25 distinct
	// cells: 1 self + 6 face + 6 axis-2 + 12 edge). Dedup so visit() runs once per distinct cell.
	int seen[32][3];
	int n = 0;
	for(int a = -1; a < 6; a++) {
		int cx = x, cy = y, cz = z;
		if(a >= 0) { // a == -1 is the changed wire cell itself; a == 0..5 are its face-neighbours
			cx += dx[a];
			cy += dy[a];
			cz += dz[a];
		}
		for(int b = 0; b < 6; b++) {
			int mx = cx + dx[b], my = cy + dy[b], mz = cz + dz[b];
			bool dup = false;
			for(int k = 0; k < n; k++)
				if(seen[k][0] == mx && seen[k][1] == my && seen[k][2] == mz) {
					dup = true;
					break;
				}
			if(dup)
				continue;
			if(n < 32) {
				seen[n][0] = mx;
				seen[n][1] = my;
				seen[n][2] = mz;
				n++;
			}
			visit(ctx, mx, my, mz);
		}
	}
}
