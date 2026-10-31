/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#ifndef CHUNK_H
#define CHUNK_H

#include <m-lib/m-i-list.h>
#include <stdbool.h>
#include <stdint.h>

#include "cglm/cglm.h"
#include <m-lib/m-i-list.h>

#include "block/blocks.h"
#include "chunk_mesher.h"
#include "platform/displaylist.h"

// Step 7 interface split: chunk.h touches struct world only through
// pointers; including world.h back formed the world<->chunk cycle the
// header-hygiene gate now forbids (world.h -> chunk.h is the real,
// by-value direction -- struct world embeds this header's ilist heads).
struct world;
struct column_backing;
struct column_view;

enum chunk_ownership {
	OWNED_VOXEL = 0,
	LOCAL_SHARED_VIEW,
	FAR_SKIN,
};

// Size constants + coordinate macros live in chunk_defs.h (step 7 split:
// world.h consumes them without the chunk API).
#include "chunk_defs.h"

typedef uint32_t c_coord_t;

// S2 far-storage compression ("lod.far_storage":"skin"; world_set_far_storage,
// world.h). DESIGN -- read this before touching chunk_get_block/chunk_alloc_far_skin:
//
// A far-ring column's ENTIRE voxel content is a pure function of two 256-byte
// arrays already computed by world_build_far_section: the per-column surface
// height (skin_y, i.e. world_section.heightmap = skin_y+1) and the surface
// block id (top_id). The default "voxel" storage bakes that function's output
// into a real CHUNK_SIZE^3*5/2 = 10240 B MEM2 blocks buffer per Y-slice (up to
// COLUMN_HEIGHT=8 of them per column -- the "8 real struct chunks + block
// buffers" docs/stage2_lod_plan_2026-09-03.md calls out). "skin" storage keeps
// only this struct (513 B: two 256 B arrays + one chunk_min floor byte, see
// floor_y below) per Y-slice and lets chunk_get_block RECOMPUTE the cell on
// every read instead of indexing a buffer -- the "padded mesher input view"
// is synthesized per-cell, not materialized. Net: up to 8 * 513 B ~= 4.0 KiB
// worst case (a column spanning every Y-slice) vs 8 * 10240 B = 80 KiB
// today; a typical few-Y-slice column (most terrain) is closer to the ~1 KB
// network skin it is decompressing.
//
// LIFETIME/OWNERSHIP: each chunk shell in a skin-storage column owns a
// PRIVATE copy (chunk_alloc_far_skin mallocs it, chunk_destroy frees it) --
// never a pointer shared across sibling Y-slices or into the world_section
// dict. Two reasons this is deliberate, not an oversight: (1) struct
// world_section lives BY VALUE in dict_wsection's open-addressed table
// (DICT_DEF2 ... M_POD_OPLIST in world.h) and can be RELOCATED by any later
// insertion elsewhere in the world -- caching a struct world_section* on a
// long-lived chunk would silently dangle the next time an unrelated section
// streams in and grows the dict; (2) sibling Y-slices are refcounted
// independently (chunk_mesher_send/receive bumps one chunk's reference_count
// while its mesh is in flight) and are not guaranteed to be destroyed
// together, so a shared buffer would need its own refcount to free safely.
// A private per-slice copy sidesteps both: its address is a plain malloc,
// stable for exactly this chunk's lifetime, freed exactly once.
//
// COST BOUND (the maintainer's "far ring must never cost frame time" rule):
// chunk_get_block's skin branch is TWO array reads + one integer compare +
// one struct literal -- O(1), no branch-per-neighbour, no allocation -- and
// it runs at the EXACT call site the voxel path's packed-buffer unpack runs
// at today (chunk_lookup_block, invoked only from chunk_mesher_send's
// existing padded-gather loop, itself dispatched off the same throttled
// rebuild_displist/mesh_in_flight budget as every other chunk build -- see
// chunk_check_built). Nothing moves to a new thread, a new phase, or a new
// per-frame cost: the render draw path (chunk_render/chunk_pre_render, the
// steady per-frame cost) never reads far_skin at all, only the already-built
// display lists. See tests/test_far_storage_equiv.c for the measured
// gather-loop wall-time delta (voxel vs skin) and its bench_mesher context.
struct far_column_skin {
	// == world_section.heightmap's slice for this column's XZ footprint:
	// skin_y+1, or 0 for an all-air column (see world_build_far_section).
	uint8_t heightmap[CHUNK_SIZE * CHUNK_SIZE];
	// Block id at the surface. Meaningful only where heightmap[i] > 0 --
	// chunk_get_block never reads it otherwise (a solid column always
	// compares against floor_y first; an all-air column is unconditionally
	// open air, see below).
	uint8_t top_id[CHUNK_SIZE * CHUNK_SIZE];
	// world_build_far_section's chunk_min: the LOWEST surface across every
	// column in the whole XZ chunk, shared by all 256 columns (not a
	// per-column value). The voxel fill loop only ever writes a SOLID
	// column's filler starting at this floor, never below it -- a cell
	// between this chunk's own Y-slice floor and chunk_min (only possible
	// when chunk_min isn't 16-aligned, i.e. cy_lo*16 < chunk_min) is left at
	// chunk_init's plain BLOCK_AIR memset: UNLIT (sky=torch=0), not the
	// dimension's open-sky value. An all-air column (heightmap==0) has no
	// such gap -- its own voxel-path branch fills the WHOLE shell range with
	// lit air regardless of chunk_min -- so chunk_get_block checks
	// heightmap==0 FIRST and never applies this floor to it. Missing this
	// distinction reads back as an all-the-way-to-bedrock solid column
	// instead of the real short filler, which measurably changed emitted
	// vertex counts for a tree LOG hull in test_far_storage_equiv.c's CASE A
	// before this field was added -- kept here as the finding, not just the fix.
	uint8_t floor_y;
};

struct chunk {
	mat4 model_view;
	w_coord_t x, y, z;
	uint8_t* blocks; // allocated only by OWNED_VOXEL
	// S2 far-storage compression: this chunk's private column-skin copy (see
	// struct far_column_skin above). NULL for every ordinary (voxel-backed)
	// chunk -- the only extra per-chunk cost of the feature when it is unused.
	const struct far_column_skin* far_skin;
	enum chunk_ownership ownership;
	/* One lease per local shell, released only at its final unref: mesher jobs
	 * and the two-view/GPU union already ref the shell. view owns that lease. */
	struct column_backing* backing;
	struct column_view* view; // shared by the eight shells; includes its backing lease
	struct displaylist mesh[CHUNK_MESH_LISTS];
	bool has_displist[CHUNK_MESH_LISTS];
	/* M2 mesh blocks: the exact-size carve block holding this generation's
	 * lists (NULL when built outside block mode). Released as a whole when
	 * the generation retires (chunk_mesh_block_release). */
	void* mesh_block;
	size_t mesh_block_bytes;
	/* colcap-1006 (ASTRA-CC-R4 point 7): chunk_mesher_clock_ms() when the
	 * current mesh generation was installed (chunk_mesher_receive). Read
	 * only by the display-list owner census (world_dl_census); no render,
	 * mesh or simulation path reads it. */
	uint32_t mesh_installed_ms;
	bool rebuild_displist;
	uint32_t dirty_generation;
	// B2 first-mesh gate. A freshly-streamed chunk meshed before its 4 horizontal
	// neighbours arrive samples the missing-neighbour placeholder at its border ring
	// (chunk_lookup_block -> opaque below WORLD_HEIGHT -> border leaf/terrain faces
	// culled: the "leaves not generating"/"odd chunk borders" look) and then gets
	// re-meshed up to 4x as each neighbour lands. We hold the FIRST mesh until the 4
	// neighbours are resident, bounded by neighbour_wait frames so an edge-of-view
	// chunk (outer neighbour never streams) still meshes. first_meshed is set once the
	// first build is dispatched; afterwards re-dirties (a real neighbour arrival, via
	// chunk_set_block's neighbour trigger) mesh immediately to stay correct. Pure
	// scheduling -- the mesh OUTPUT once neighbours are present is identical.
	bool first_meshed;
	uint8_t neighbour_wait;
	// LOD tier (M1 far-ring draw distance). The DESIRED tier from the camera
	// distance, refreshed each frame by world_update_lod; on a tier *change* the
	// chunk is re-dirtied (rebuild_displist) so the worker re-meshes at the new
	// tier, and the value is snapshotted into the mesher request at send time.
	// 0 = full detail, 1 = surface-only "skin" (top faces only) for the far ring
	// -- a sanctioned better-than-Beta render LOD; the near field stays full-detail
	// + faithful. Trigger on change (not lod!=meshed) so an in-flight mesh is never
	// re-dirtied every frame (which would stale every result -> infinite re-send).
	// 2 = DORMANT (split RENDER-REACH, 2026-09-10, T2): beyond the split-screen
	// render reach of every camera; the chunk stays resident and simulated
	// (server residency untouched) but owns no display lists and is never
	// offered to the mesher. Fog is pulled to the reach so nothing pops. Only
	// set by world_update_lod_views when two cameras are active.
	uint8_t lod;
	// Foliage distance band (0=full, 1=small plants culled + leaf silhouette, 2=+grass),
	// set by world_update_lod from camera distance, snapshotted into the mesher request.
	// Re-dirtied on a band change like lod, so distant foliage detail sheds/restores as
	// the camera moves. Render-only -- the underlying blocks never change.
	uint8_t foliage_lod;
	// T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md):
	// this chunk's approx camera distance in BLOCKS, set by world_update_lod alongside
	// lod/foliage_lod above, snapshotted into the mesher request the same way. Only
	// meaningful (read) when the "lod.geomip" spike is enabled; otherwise dead data,
	// never touched by any existing render path. A single per-chunk value stands in for
	// every patch's true distance -- at far-ring range (>=3 chunks = 48+ blocks) the
	// within-chunk distance variance (<=16 blocks) is a small fraction of the total, an
	// approximation de Boer's own per-patch bounding-sphere distance also makes.
	uint16_t lod_dist_blocks;
	// In-flight guard. True between a successful chunk_mesher_send and the matching
	// chunk_mesher_receive for this chunk. While set, chunk_check_built will NOT issue
	// a second concurrent build for the chunk: under streaming churn a chunk gets
	// re-dirtied (lighting-flood stream, neighbour installs, lod/foliage band crossings)
	// while a build is still in flight; without this guard each re-dirty fired another
	// send, and the worker burned its scarce throughput on meshes that came back
	// stale-discarded (dirty_generation mismatch). Now at most ONE build per chunk is in
	// flight; a re-dirty just leaves rebuild_displist set and re-sends once the in-flight
	// result lands. Pure scheduling -- the final mesh for a given (blocks, generation,
	// lod) is byte-identical; only the wasted intermediate builds are removed.
	bool mesh_in_flight;
	// Consecutive current-generation display-list OOMs and the remaining eligibility
	// checks before another attempt. The receive side sets an 8/16/32/64-check
	// backoff; chunk_check_built consumes it without blocking unrelated chunks.
	uint8_t mesh_oom_streak;
	uint8_t mesh_retry_skip_checks;
	// H-2 band-0 priority (2026-09-03 maintainer ruling, responsiveness lane):
	// set on the player-edit chunk + its lateral neighbours
	// (chunk_trigger_neighbour_update, BLOCK edits only -- not light-only
	// updates) and on a just-installed section's lateral neighbours
	// (world_build_far_section / world_build_full_section). world_build_chunks
	// walks w->render for mesh_urgent chunks FIRST, ahead of the ordinary
	// near/far passes, so an edit-opened hole or a newly-installed section's
	// neighbour never waits behind an unrelated backlog. Cleared once the
	// chunk is fully settled (!rebuild_displist && !mesh_in_flight) -- pure
	// scheduling priority, never a content change (the final mesh is
	// identical regardless of when it was dispatched relative to other
	// chunks). mesh_urgent_since_frame records world->build_frame at the
	// moment this was FIRST set (not re-stamped by a later mark while still
	// urgent), letting world_build_chunks report the max frames an urgent
	// chunk waited (H-3's edit_to_visible_frames).
	bool mesh_urgent;
	uint32_t mesh_urgent_since_frame;
	struct world* world;
	uint8_t reachable[6];
	size_t reference_count;
	bool has_fog;
	struct chunk_step {
		// "visited this render-BFS frame" iff this == world->bfs_epoch (see
		// world.h). Replaces a bool that needed a per-frame O(loaded) reset.
		uint32_t visited_epoch;
		enum side from;
		uint8_t used_exit_sides;
		int steps;
	} tmp_data;
	ILIST_INTERFACE(ilist_chunks, struct chunk);
	ILIST_INTERFACE(ilist_chunks2, struct chunk);
};

static inline void chunk_mark_dirty(struct chunk* c) {
	c->dirty_generation++;
}

static inline bool chunk_dirty_generation_matches(const struct chunk* c,
												  uint32_t generation) {
	return c->dirty_generation == generation;
}

ILIST_DEF(ilist_chunks, struct chunk, M_POD_OPLIST)
ILIST_DEF(ilist_chunks2, struct chunk, M_POD_OPLIST)

// Alignment-honouring allocator for struct chunk (over-aligned via a SIMD member;
// plain malloc under-aligns it -> vmovdqa fault under -O3 -march=native). See chunk.c.
// Also lays a MEM1 red zone after the struct -- see the guard block in chunk.c.
struct chunk* chunk_alloc(void);
/* The caller holds a lease already. Attach before publishing a fresh shell. */
void chunk_retain_backing(struct chunk* c, struct column_backing* backing);

// Width of that red zone, in bytes after the struct. Same value as mem2.c's MEM2_GUARD, and
// the per-chunk memory cost of the mechanism: 256 B x 968 chunks = 242 KiB at the shipped
// far_view_distance=5, 256 B x 3528 = 882 KiB at distance 10 (chunks = (2d+1)^2 sections x
// COLUMN_HEIGHT), against 24 MB of MEM1. That is 36% on top of the 704-byte PPC struct, which
// buys more than symmetry with mem2.c: a CONTIGUOUS sweep past the struct is caught by any
// width, but the class this struct actually invites is a stale INDEX into its own arrays --
// mesh[CHUNK_MESH_LISTS] alone is 520 of those 704 bytes, so an index a few elements past the
// end lands tens to a couple hundred bytes beyond the struct and is a point write, not a
// sweep. 256 covers that reach; a 64-byte guard would save 180 KiB and miss it.
#define CHUNK_GUARD 256

// MEM1 red-zone diagnostics for struct chunk, the allocation whose embedded intrusive-list
// nodes were smashed in the 2026-07-27 hang. Same mechanism, width and fill byte as the MEM2
// canary (mem2.h), applied to the plain-malloc side of the heap that the MEM2 red zone cannot
// see. hits = guard violations caught at chunk_destroy; first = the first offender's chunk base
// and the byte offset INTO the guard where the pattern first broke -- that offset is what names
// the writer, since a fixed stride reproduces it across runs. The block size is not reported
// because it is always sizeof(struct chunk). Both out-params are optional (pass NULL).
// Surfaced on the F3 overlay's "dlbad dlcall ilist" row and in the watchdog hang report + its
// own cell-bar row, the only instrument a bare frozen console has.
uint32_t chunk_guard_hits(void);
void chunk_guard_first(void** ptr, size_t* guard_off);
// Test-only isolation hook (the world_ilist_overrun_reset precedent): clears the count AND the
// captured first offender so each case starts from a known state. Never called by the engine.
void chunk_guard_reset(void);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void chunk_test_fail_alloc_after(unsigned occurrence);
#endif
/* M2: free the chunk's current mesh block (after its lists were destroyed). */
void chunk_mesh_block_release(struct chunk* c);
void chunk_mesh_release(struct chunk* c);
void chunk_init(struct chunk* c, struct world* world, w_coord_t x, w_coord_t y,
				w_coord_t z);
struct chunk* chunk_alloc_shared(void);
void chunk_init_shared(struct chunk* c, struct world* world, w_coord_t x,
	w_coord_t y, w_coord_t z, struct column_view* view, struct column_backing* backing);
// S2 far-storage compression pair, mirroring chunk_alloc/chunk_init exactly
// (alloc can fail and returns NULL; init cannot fail) but producing a shell
// with blocks==NULL and a private far_skin copy instead of a 10 KB MEM2
// buffer -- see struct far_column_skin's design comment above. Never call
// chunk_set_block/_raw/_light on the result: it is synthesized, read-only
// (asserted in chunk.c).
struct chunk* chunk_alloc_far_skin(void);
// Far-ring VOXEL storage ("lod.far_storage":"voxel", the default): identical to
// chunk_alloc in every observable way, but its 10 KB block buffer comes from the
// far-column slot pool (platform/mem2.h) rather than the shared carve heap.
// Call it ONLY from the far-ring column install -- a near chunk allocated
// through it would take a far slot and push a real far column onto the shared
// heap. Freed by chunk_destroy like any other chunk.
struct chunk* chunk_alloc_far_voxel(void);
void chunk_init_far_skin(struct chunk* c, struct world* world, w_coord_t x,
						 w_coord_t y, w_coord_t z, const uint8_t* heightmap,
						 const uint8_t* top_id, uint8_t floor_y);
void chunk_ref(struct chunk* c);
void chunk_unref(struct chunk* c);
struct block_data chunk_get_block(struct chunk* c, c_coord_t x, c_coord_t y,
								  c_coord_t z);
// Branch-free packed-buffer read, skipping chunk_get_block's far_skin check --
// for a per-cell hot loop that already knows (checked ONCE, not per cell)
// that this chunk is owned voxel storage (ownership == OWNED_VOXEL). Asserts that in
// chunk.c; a one-off caller that hasn't already established it should call
// chunk_get_block instead. See chunk_mesher.c's chunk_mesher_send gather.
struct block_data chunk_get_block_voxel(struct chunk* c, c_coord_t x,
										c_coord_t y, c_coord_t z);
struct block_data chunk_lookup_block(struct chunk* c, w_coord_t x, w_coord_t y,
									 w_coord_t z);
void chunk_set_block(struct chunk* c, c_coord_t x, c_coord_t y, c_coord_t z,
					 struct block_data blk);
// Write a block into the packed buffer ONLY -- no chunk_mark_dirty, no
// rebuild_displist, no neighbour-trigger. For bulk fills (the far-ring synthetic
// chunk) that mark the chunk dirty + dirty neighbours ONCE at the end instead of
// per cell. The buffer write is byte-identical to chunk_set_block's.
void chunk_set_block_raw(struct chunk* c, c_coord_t x, c_coord_t y, c_coord_t z,
						 struct block_data blk);
/* Load one aligned 16-high slice from a complete server column's packed arrays.
 * Replaces every storage byte; caller owns publication and dirty state. */
void chunk_load_packed_slice(struct chunk* c, unsigned base_y,
							 const uint8_t* ids, const uint8_t* metadata,
							 const uint8_t* sky, const uint8_t* torch);
bool chunk_check_built(struct chunk* c);
// H-2 band-0 priority (2026-09-03 maintainer ruling): mark c as needing a
// mesh URGENTLY (see struct chunk's mesh_urgent doc comment). Idempotent --
// a chunk already urgent keeps its original mesh_urgent_since_frame stamp,
// so a second edit before the first is meshed does not reset its wait clock.
// Does NOT set rebuild_displist itself; callers that want a rebuild set that
// separately (this only affects scheduling PRIORITY, never whether a rebuild
// happens at all).
void chunk_mark_urgent(struct chunk* c);
void chunk_set_light(struct chunk* c, c_coord_t x, c_coord_t y, c_coord_t z,
					 uint8_t light);
void chunk_render(struct chunk* c, bool pass, float x, float y, float z);
void chunk_pre_render(struct chunk* c, mat4 view, bool has_fog);

#endif
