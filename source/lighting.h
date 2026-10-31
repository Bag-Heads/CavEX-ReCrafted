/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#ifndef LIGHTING_H
#define LIGHTING_H

#include "world.h"

/* G9B R3: observations are test-pool only, synchronous and disarmed by
 * default. The array address identifies allocation, never residency. */
#ifdef CAVEWII_SERVER_LOCAL_TEST
struct lighting_history_event {
	const char* operation;
	const void* object;
	size_t index, before_depth, after_depth;
	w_coord_t x, y, z;
	w_coord_t x2, y2, z2;
	int channel;
	unsigned before, after;
	bool packed, forced;
	/* G9X-1: semantic emitting site (__FILE__/__LINE__ of the macro use),
	 * not the common emitter. NULL/0 when an emitter supplies none. */
	const char* file;
	int line;
	const void* caller; /* return address of a callback emitter, or NULL */
	/* G9X-1 setter attempts: callback nesting depth and the Beta-visible
	 * boolean result (1/0; -1 = target chunk absent from the quadrant). */
	unsigned depth;
	int result;
};
extern bool (*lighting_test_history_hook)(const struct lighting_history_event*);
void lighting_test_history_reset(void);
bool lighting_test_history_failed(void);
void lighting_test_history_emit(struct lighting_history_event event);
static inline void lighting_history_byte(uint8_t* data, size_t index,
	uint8_t value, const char* file, int line) {
	lighting_test_history_emit((struct lighting_history_event){
		.operation = "array-store", .object = data, .index = index,
		.before = data[index], .after = value, .file = file, .line = line});
	data[index] = value;
}
static inline void lighting_history_nibble(uint8_t* data, size_t index,
	uint8_t value, const char* file, int line) {
	lighting_test_history_emit((struct lighting_history_event){
		.operation = "array-store", .object = data, .index = index,
		.before = nibble_read(data, index), .after = value, .packed = true,
		.file = file, .line = line});
	nibble_write(data, index, value);
}
#define G9B_BYTE(data, index, value) lighting_history_byte(data, index, value, __FILE__, __LINE__)
#define G9B_NIBBLE(data, index, value) lighting_history_nibble(data, index, value, __FILE__, __LINE__)
#define G9B_ALLOC(sc) lighting_test_history_emit((struct lighting_history_event){ \
	.operation = "allocate", .object = sc, .file = __FILE__, .line = __LINE__})
#define G9B_QUEUE(op, q, px, py, pz, force, old, now) \
	lighting_test_history_emit((struct lighting_history_event){ \
		.operation = op, .object = q, .x = px, .y = py, .z = pz, \
		.forced = force, .before_depth = old, .after_depth = now, \
		.file = __FILE__, .line = __LINE__})
#else
#define G9B_BYTE(data, index, value) ((data)[index] = (value))
#define G9B_NIBBLE(data, index, value) nibble_write(data, index, value)
#define G9B_ALLOC(sc) ((void)0)
#define G9B_QUEUE(op, q, px, py, pz, force, old, now) ((void)0)
#endif

struct world_modification_entry {
	w_coord_t x, y, z;
	struct block_data blk;
};

void lighting_heightmap_update(uint8_t* heightmap, c_coord_t x, w_coord_t y,
							   c_coord_t z, uint8_t type,
							   bool (*get_block)(void* user, c_coord_t x,
												 w_coord_t y, c_coord_t z,
												 struct block_data* blk),
							   void* user);

void lighting_update_at_block(
	struct world_modification_entry source, bool ignore_sky_light,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user);

// Recompute the light field from MANY source cells in ONE flood -- equivalent to
// calling lighting_update_at_block once per source, but O(cells) instead of
// O(sources x cells). Used by the post-populate relight so a worldgen lava lake
// no longer floods per-block (which froze gen on lava-heavy chunks).
void lighting_update_at_blocks(
	const struct world_modification_entry* sources, size_t n,
	bool ignore_sky_light,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user);

/* Server Block work preserves stored Nether Sky (World.java:1300-1301).
 * The legacy client/remote update entry points retain their existing output. */
void lighting_update_at_blocks_server(
	const struct world_modification_entry* sources, size_t n,
	bool ignore_sky_light,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	void* user);

// A2 pacing: install a yield callback (the server passes thread_yield) that the flood
// invokes periodically so a long relight BFS doesn't freeze the single-core render
// thread. NULL (default) = no yield, byte-identical flood (used by tests + the client).
void lighting_set_yield(void (*yield)(void));

// Count of relight floods aborted at the work ceiling (LIGHTING_FLOOD_MAX_POPS) --
// a pathological annulus-spanning cascade, not real lighting. MUST stay 0; non-zero
// flags a relight-seam inconsistency (e.g. a synthetic far section storing light a
// relight disagrees with). Surfaced in the watchdog hang report.
// LIGHT-CLIENT-LOSSLESS: server / synchronous floods only; the client render relight
// parks instead of aborting and counts in lighting_client_ceiling_parks().
uint32_t lighting_flood_ceiling_hits(void);

// --- Resumable, budgeted relight for the client/main (render) thread ---
// The render thread cannot advance the main-loop watchdog heartbeat mid-flood, so an
// unbudgeted multi-second surface skylight flood froze the core (the far-ring freeze,
// watchdog row1 = WDM_LIGHTING). world.c carries ONE flood's BFS frontier across frames
// in a persistent queue and spends only a bounded pop budget per frame. Byte-exact: the
// frontier is preserved verbatim across the suspend, so the same cells pop in the same
// order to the same fixed point -- only the settle RATE is paced
// (tests/test_lighting_client_budget_equiv.c).
void lighting_flood_queue_create(struct stack* q);
void lighting_flood_queue_destroy(struct stack* q);
bool lighting_flood_queue_empty(const struct stack* q);
// Abandon an in-flight flood (world reset/unload): empty the frontier AND zero the
// per-flood pop accumulator. Use instead of a raw stack_clear so cflood can't go stale.
void lighting_flood_queue_clear(struct stack* q);
// Seed a NEW flood (resets the per-flood pop accumulator behind the max counter).
// LIGHT-CLIENT-LOSSLESS: false = the frontier could not grow, NOTHING was pushed;
// the caller must keep the work another way (world.c marks the column for repair).
bool lighting_flood_seed(struct stack* q, w_coord_t x, w_coord_t y, w_coord_t z,
						 bool forced);
// Process up to max_pops cells; returns cells popped this call. Flood is done when
// lighting_flood_queue_empty() is true.
size_t lighting_flood_resume(
	struct stack* q, bool ignore_sky_light, size_t max_pops,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user);
// Same budget/counters/frontier as resume; optional light-only neighbour reader.
size_t lighting_flood_resume_light(
	struct stack* q, bool ignore_sky_light, size_t max_pops,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	void* user);
// Largest single client relight flood seen (pops), surfaced on F3 to confirm the
// freeze cause: a value in the 100k-500k range = the multi-second flood the budget now
// paces; a small value = the freeze lies elsewhere.
uint32_t lighting_client_flood_max_pops(void);

// True iff a cell with block id `cell_id` and stored skylight `cell_sky` is an
// "under-lit" skylight-bleed frontier seed (D1 gen-time horizontal skylight occlusion):
// a transparent, below-full-sky cell some loaded neighbour can bleed light into, i.e.
// some neighbour_sky[k] - max(opacity,1) > cell_sky -- the EXACT donor test lighting_flood
// uses to recompute a cell. Opaque cells (the flood never propagates into them) and cells
// already at full sky 15 are never seeds. `neighbour_sky` carries the sky values of the
// in-world neighbours (out-of-world ones omitted; order irrelevant). Shared by
// server_world.c's bleed AND tests/test_skylight_occlusion.c so the seed-selection logic
// cannot drift; a bug here that under-seeds is caught by that test's independent
// brute-force fixed-point comparison.
static inline bool skylight_cell_underlit(uint8_t cell_id, int cell_sky,
										  const int* neighbour_sky, int count) {
	if(blocks[cell_id] && !blocks[cell_id]->can_see_through)
		return false; // opaque cells never take horizontal skylight
	if(cell_sky >= 15)
		return false; // already full open sky -- cannot be raised
	int opacity = blocks[cell_id] ? blocks[cell_id]->opacity : 1;
	if(opacity < 1)
		opacity = 1; // MAX(opacity, 1), exactly as lighting_flood
	for(int k = 0; k < count; k++)
		if(neighbour_sky[k] - opacity > cell_sky)
			return true; // a brighter neighbour can bleed in
	return false;
}

// --- G9 lossless server light drain (see lighting.c) ---
enum lighting_drain_result {
	LIGHTING_DRAIN_DONE = 0,
	LIGHTING_DRAIN_CEILING, // pop ceiling reached, frontier intact
	LIGHTING_DRAIN_OOM,		// frontier grow failed, frontier intact
};
// LIGHT-CLIENT-LOSSLESS: why the last resume stopped. DONE = budget spent or the
// flood finished; CEILING = this flood reached LIGHTING_FLOOD_MAX_POPS (500k) pops;
// OOM = the frontier could not grow. On CEILING/OOM the frontier is INTACT in q.
enum lighting_drain_result lighting_flood_last_status(void);
// Spill up to max_entries of a parked frontier: call mark(cx, cz) for each column
// holding a popped frontier cell (consecutive duplicates folded; a forced entry also
// marks its side neighbours' columns). Once q is empty its storage is freed (it
// regrows on demand). Returns the entries spilled this call; the caller charges them.
size_t lighting_flood_queue_spill(struct stack* q,
	void (*mark)(void* user, w_coord_t cx, w_coord_t cz), void* user,
	size_t max_entries);
// One queue-free relaxation of the client light equation at a cell (the flood
// body, client form): 1 = changed and written, 0 = already settled, -1 = absent.
int lighting_relax_cell(bool ignore_sky_light, w_coord_t x, w_coord_t y,
	w_coord_t z,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	void* user);
// Client relight counters, split from the server/process ones above so a client
// fault is never reported as a server-queue fault: floods parked at the 500k
// ceiling, and frontier/seed grows that failed (each turned into a column repair).
uint32_t lighting_client_ceiling_parks(void);
uint32_t lighting_client_oom_parks(void);
void lighting_client_note_oom_park(void); // a caller-side reserve failed (world.c)
// Client relight only: hook(user, x, y, z) true = the cell touches a non-resident
// column; there the relight may raise light but never lower it (see lighting.c).
void lighting_client_set_seam_hook(
	bool (*hook)(void*, w_coord_t, w_coord_t, w_coord_t));
// Grow a client queue to hold need entries; false = unchanged, nothing lost.
bool lighting_client_reserve(struct stack* q, size_t need);
#ifdef CAVEWII_SERVER_LOCAL_TEST
// Fail the n-th following CLIENT queue grow (0 = the next one; -1 = off).
void lighting_test_client_fail_reserve_after(long n);
#endif
// The per-seed pop budget of the old per-edit floods; a drain of n seeds gets
// n times this (saturating), so batching never tightens the old budget.
#define LIGHTING_DRAIN_CEILING_PER_SEED 500000u
// An empty drain queue with no storage yet (grows on demand).
void lighting_drain_queue_init(struct stack* q);
size_t lighting_drain_queue_bytes(const struct stack* q);
// Push n forced seeds, all or nothing (false = nothing pushed).
bool lighting_drain_seed(struct stack* q,
						 const struct world_modification_entry* sources,
						 size_t n);
// I07/G9 drain telemetry (ASTRA-G9-DRAIN-CAP item 4: unsaturated per-drain
// work): counted inside the flood at one compare/increment per pop, push and
// write. Storage only -- the flood's pops, order and values are untouched.
struct lighting_drain_stats {
	uint32_t pushes;	 // neighbour entries pushed by this flood call
	uint32_t writes;	 // set_light calls (pops whose light changed / forced)
	uint32_t depth_peak; // largest frontier (entries) seen before a pop
};
// Run the pre-seeded flood to completion or to an explicit stop; *pops gets
// the cells popped. A stop leaves the remaining frontier in q. `st` (may be
// NULL) is zeroed and filled with this call's work counts.
// `get_light` (may be NULL) is the drain's light-only neighbour read: the
// flood needs just a neighbour's sky and block light (never its id or
// metadata), so a caller can answer those two nibbles without the full
// block fetch. It MUST agree with get_block on existence and on both light
// values for every cell (the server's does: same chunk resolve, same
// nibble planes); NULL falls back to get_block, byte-identical.
enum lighting_drain_result lighting_drain_flood(
	struct stack* q, bool ignore_sky_light, size_t ceiling, size_t* pops,
	struct lighting_drain_stats* st,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	bool (*get_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t* sky, uint8_t* torch),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user);
typedef enum lighting_drain_result (*lighting_drain_kernel_fn)(
	struct stack* q, bool ignore_sky_light, size_t ceiling, size_t* pops,
	struct lighting_drain_stats* st,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	bool (*get_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t* sky, uint8_t* torch),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user);
/* Shared stop/pacing seams for the specialised server kernel and frozen oracle.
 * None changes frontier contents, pop order or relaxation arithmetic. */
bool lighting_drain_reserve(struct stack* q, size_t need);
void lighting_drain_yield(void);
void lighting_set_drain_allocation(void (*hook)(bool before));
void lighting_drain_ceiling_hit(void);

/* REL-MP-FIX3c (remote sessions only): a pending-cell bitmap per bounded
 * column slot in ONE caller-supplied reservation (caller picks MEM1/MEM2);
 * no allocation after init and a bit is set or not, so nothing is dropped. */
struct lighting_remote_relight;
enum lighting_remote_status {
	LIGHTING_REMOTE_IDLE = 0, // nothing pending
	LIGHTING_REMOTE_PENDING,  // budget spent, every pending bit retained
	LIGHTING_REMOTE_NO_SLOT,  // slots < loaded columns: nothing lost, retry
	LIGHTING_REMOTE_UNINIT,   // no valid reservation: nothing done
};
size_t lighting_remote_relight_bytes(size_t slots); // exact reservation size
// Caller's "column (cx,cz) is loaded" test: cells in unloaded columns are never
// marked (Beta lights only existing chunks: MetadataChunkBlock.java:43,
// World.java:319-329, World.java:1689), so slot demand <= loaded columns.
typedef bool (*lighting_column_loaded_fn)(void* user, w_coord_t cx,
	w_coord_t cz);
// NULL/short storage (a failed allocation) returns NULL: explicit failure.
struct lighting_remote_relight* lighting_remote_relight_init(void* storage,
	size_t bytes, size_t slots);
// Forced seed: marks (x,y,z) and its loaded six neighbours, all or nothing.
enum lighting_remote_status lighting_remote_relight_admit(
	struct lighting_remote_relight* r, w_coord_t x, w_coord_t y, w_coord_t z,
	lighting_column_loaded_fn loaded, void* user);
// Relax pending cells (existing light equation), round-robin from retained
// column cursors; scans + expansion charged to `budget`, reported in *spent.
enum lighting_remote_status lighting_remote_relight_drain(
	struct lighting_remote_relight* r, bool ignore_sky_light, size_t budget,
	size_t* spent,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t,
		struct block_data*, uint8_t*),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t),
	lighting_column_loaded_fn loaded, void* user);
// Drop one column's obligations (full install / unload) or all (reset).
void lighting_remote_relight_invalidate_column(
	struct lighting_remote_relight* r, w_coord_t cx, w_coord_t cz);
void lighting_remote_relight_reset(struct lighting_remote_relight* r);
// REL-MP-FIX3d: drop one cell's obligation (a partial Packet51 overwrote it).
void lighting_remote_relight_clear_cell(struct lighting_remote_relight* r,
	w_coord_t x, w_coord_t y, w_coord_t z);
bool lighting_remote_relight_active(const struct lighting_remote_relight* r);

#ifdef CAVEWII_SERVER_LOCAL_TEST
extern lighting_drain_kernel_fn server_world_test_light_flood_override;
extern void (*lighting_test_drain_yield_hook)(void);
// Fail the n-th following drain-queue grow (0 = the next one; -1 = off).
void lighting_test_drain_fail_reserve_after(long n);
#endif

#endif
