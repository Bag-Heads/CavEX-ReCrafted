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

#ifndef WORLD_H
#define WORLD_H

#include <m-lib/m-dict.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cglm/cglm.h"

#include "block/blocks_data.h"
#include "column_view.h"
#include "stack.h"

// WORLD_HEIGHT / COLUMN_HEIGHT live in chunk_defs.h with the other
// dimension constants (step 7 split -- chunk.h needs them without world.h).

enum world_dim {
	WORLD_DIM_NETHER = -1,
	WORLD_DIM_OVERWORLD = 0,
};

#include "block/aabb.h"
// Step 7 interface split: struct world embeds the ilist_chunks_t /
// ilist_chunks2_t HEAD types that chunk.h's M_ILIST_DEF generates, so the
// world->chunk include is a genuine by-value dependency and stays; the
// cycle breaks on the reverse edge instead (chunk.h forward-declares
// struct world). camera is pointer-only here -- forward-declared, include
// dropped (that edge closed the world<->camera half of the old 6-header
// cycle; header-hygiene gate keeps it closed).
#include "chunk.h"
#include "util.h"

struct camera;
struct lighting_remote_relight;

// Per-frame client relight pop budget. world_update_lighting runs on the render
// thread, so this must remain a hard cap on production driver work per frame.
#define LIGHTING_CLIENT_POPS_PER_FRAME 4096u

void world_local_revision(struct world* w, int32_t x, int32_t z,
	struct column_revision revision, uint32_t previous_content, bool install);
void world_local_publication(struct world* w,
	struct column_light_publication* publication, uint8_t world_index,
	uint32_t world_generation);

struct world_section {
	uint8_t heightmap[CHUNK_SIZE * CHUNK_SIZE];
	// Beta biome grass/foliage colormap indices per column, layout
	// [localX*16 + localZ] (as worldgen_climate produces them). The mesher
	// tints grass/leaf faces via biome_grass_color/biome_foliage_color(ci,cj).
	uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t grass_j[CHUNK_SIZE * CHUNK_SIZE];
	struct chunk* column[COLUMN_HEIGHT];
	// True for a far-ring SYNTHETIC LOD section (world_build_far_section). Its
	// light is a baked approximation, NOT a relightable real field -- the client
	// per-frame relight must STOP at the near/far seam instead of flooding across
	// the whole annulus (which used to OOM the BFS -> the 0x00800000 crash). Set
	// false by world_build_full_section (incl. the far->full promotion).
	bool is_far_lod;
	// LIGHT-CLIENT-LOSSLESS: this column holds client relight work that a parked
	// flood (500k ceiling or a failed frontier grow) handed over instead of
	// dropping; world_update_lighting re-relaxes the column to the fixed point
	// within its per-frame budget, then clears the flag.
	bool light_repair;
};

// Pack two 32-bit section coords into one 64-bit dict key. Shift in UNSIGNED then cast:
// `(int64_t)(z) << 32` is signed-shift UB for negative z (UBSan halts the CI ASan soak),
// while the bit pattern produced here is byte-identical to the old key for any 32-bit z.
#define SECTION_TO_ID(x, z)                                                    \
	((int64_t)(((uint64_t)(uint32_t)(z) << 32) | (uint32_t)(x)))

/*
 * A section insertion may grow M-lib's data and index buffers, then allocate
 * bounded collision scratch. Its default OOM policy aborts, and ordinary
 * realloc can already have freed an earlier buffer when a later allocation
 * fails. Arm this allocator only around resize: allocate copies, commit every
 * owner together, or restore the untouched dictionary and report false.
 */
void* world_wsection_dict_realloc(void* ptr, size_t old_size, size_t new_size);
void world_wsection_dict_free(void* ptr);
void world_wsection_dict_memory_full(void);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void world_test_set_section_dict_transaction_hook(
	void (*hook)(void*), void* arg);
void world_test_set_section_dict_contention_hook(
	void (*hook)(void*), void* arg);
#endif

#define WSECTION_DICT_REALLOC(ctx, type, ptr, old_count, new_count)            \
	((new_count) > SIZE_MAX / sizeof(type)                                      \
		 ? NULL                                                                  \
		 : (type*)world_wsection_dict_realloc(                                  \
			   (ptr), (old_count) * sizeof(type), (new_count) * sizeof(type)))
#define WSECTION_DICT_FREE(ctx, type, ptr, count)                              \
	((void)(count), world_wsection_dict_free(ptr))
#define WSECTION_DICT_KEY_OPLIST                                               \
	M_OPEXTEND(M_BASIC_OPLIST, REALLOC(WSECTION_DICT_REALLOC),                  \
			   FREE(WSECTION_DICT_FREE))

#undef M_MEMORY_FULL
#define M_MEMORY_FULL(type, number)                                           \
	do {                                                                        \
		(void)sizeof(type);                                                       \
		(void)(number);                                                           \
		world_wsection_dict_memory_full();                                       \
	} while(0)
DICT_DEF2(dict_wsection, int64_t, WSECTION_DICT_KEY_OPLIST, struct world_section,
		  M_POD_OPLIST)
#undef M_MEMORY_FULL
#define M_MEMORY_FULL(type, number)                                           \
	M_RAISE_FATAL(                                                             \
		"Cannot allocate %zu objects of type '%s' (size '%zu') at (%s:%s:%d).\n", \
		(size_t)(number), M_AS_STR(type), sizeof(type), __FILE__, __func__,       \
		__LINE__)

#undef WSECTION_DICT_KEY_OPLIST
#undef WSECTION_DICT_FREE
#undef WSECTION_DICT_REALLOC

#define WORLD_RENDER_VIEWS 2

/* Lists borrow one frame's union references, handed to gpu_busy_chunks at the
 * flip. They never own meshes or use a chunk's intrusive node for traversal. */
struct world_render_view {
	struct chunk** chunks;
	size_t count, capacity;
};

/* Split-dims B1 phase 2: the client world-context LEASE seam. The client
 * owner (client_interface.c) installs an owner on the world it renders; every
 * in-flight mesher job and the GPU's hold on the previous frame's render
 * hand-off take one lease against the world's CURRENT context and give it
 * back to that exact context (ASTRA-B1-RULINGS-0925 #2/#3). A context reset
 * or replaced while leases are out cannot free or re-admit anything until
 * they drain, and a completion whose epoch is no longer current may not
 * publish. The seam is plain data + function pointers so the world stratum
 * takes no game/ include. lease_owner NULL (headless tools and most tests) =
 * no leases, the pre-B1 behaviour byte for byte. Owner thread only. */
enum world_lease_kind {
	WORLD_LEASE_MESHER,
	WORLD_LEASE_GPU,
};
struct world_lease {
	void* ctx;      /* the exact context the lease was taken from */
	uint32_t epoch; /* that context's epoch at acquisition */
	bool held;
};
struct world_lease_owner {
	/* Take one lease against the current context. False: no context, or its
	 * producers are parked (quiescing) -- the caller defers, losslessly. */
	bool (*acquire)(void* user, enum world_lease_kind kind,
					struct world_lease* out);
	/* Give a held lease back to its own context. Returns true iff that
	 * context is still current with lease->epoch as its epoch, i.e. the
	 * completion may publish. Clears lease->held. */
	bool (*release)(void* user, enum world_lease_kind kind,
					struct world_lease* lease);
	void* user;
};

struct world {
	struct column_light_publication* settled_light; /* MAIN owns after ingress */
	uint32_t settled_light_generation;
	uint8_t settled_light_world_index;
	dict_wsection_t sections;
	struct chunk* world_chunk_cache;
	ilist_chunks_t render; /* unique union: mesh priority and GPU lifetime */
	struct world_render_view render_views[WORLD_RENDER_VIEWS];
	ilist_chunks2_t gpu_busy_chunks;
	ptime_t anim_timer;
	struct stack lighting_updates;
	// In-progress client relight frontier, carried across frames so a large surface
	// skylight flood is PACED within a per-frame pop budget instead of freezing the
	// render thread mid-flood (the far-ring watchdog freeze). See world_update_lighting.
	struct stack lighting_flood_queue;
	/* REL-MP-FIX3d: remote sessions only; caller-owned reservation, NULL in
	 * local mode (local seeds keep the two stacks above). */
	struct lighting_remote_relight* remote_relight;
	bool remote_relight_failed; /* last begin failed: refuse remote seeds */
	/* LIGHT-CLIENT-LOSSLESS column repair (world_light_repair_step): sections
	 * flagged light_repair are swept cell by cell, alternating direction, until
	 * a whole pass changes nothing. O(1) memory, so it works when the heap is
	 * too low for a flood frontier. hint = flags set and not yet cleared (an
	 * upper bound: an unloaded section takes its flag with it). */
	uint32_t light_repair_hint;
	bool light_repair_active; /* a column below is being swept */
	w_coord_t light_repair_cx, light_repair_cz;
	uint32_t light_repair_index; /* next cell of this pass, 0..32767 */
	uint32_t light_repair_pass;  /* passes over this column; odd = backward */
	bool light_repair_changed;	 /* a cell changed during this pass */
	/* A parked frontier is being spilled into column repairs, a budgeted slice
	 * per frame (lighting_flood_queue_spill); the flood does not resume. */
	bool light_spill_active;
	enum world_dim dimension;
	// Monotonic per-frame stamp for the render BFS. A chunk is "visited this
	// frame" iff chunk->tmp_data.visited_epoch == bfs_epoch, so the flood no
	// longer has to zero every loaded chunk's tmp_data each frame (that reset
	// was O(loaded chunks), ~1152 on Wii). Bumped once per world_bfs; never 0
	// (so a freshly-created chunk's visited_epoch == 0 is never a false hit).
	uint32_t bfs_epoch;
	// HIGH-WATER bound for the per-frame chunk-list walks (see the "corrupt chunk-chain
	// guard" block in world.c). Never lowered: render and gpu_busy_chunks hold entries
	// pushed under an EARLIER frame's section count, so a bound derived from the CURRENT
	// count can sit below the live entry count -- world_unload_all drops sections to 0 on
	// a dimension switch while both lists are still full -- and the guard would then take
	// its deliberate-leak bail on a perfectly healthy chain.
	size_t ilist_walk_cap_max;
	// H-2 (2026-09-03 responsiveness lane): bumped once per world_build_chunks
	// call. Used ONLY to timestamp struct chunk's mesh_urgent_since_frame (H-3's
	// edit_to_visible_frames instrument) -- not a wall clock, not persisted,
	// never read for simulation. 0 before the first world_build_chunks call.
	uint32_t build_frame;
	// H-2: live count of resident chunks currently marked mesh_urgent
	// (chunk_mark_urgent increments; world_build_chunks decrements when a
	// chunk settles and its mesh_urgent clears). world_build_chunks gates its
	// (otherwise O(all resident sections)) urgent dict-scan on this being
	// nonzero, so an ordinary frame with no pending edit pays nothing beyond
	// one integer compare -- the same "bounded, rare-cost" tradeoff
	// world_far_missing_count's throttled-cadence scan makes, just throttled
	// by edit RARITY instead of a heartbeat clock.
	uint32_t mesh_urgent_count;
	// H-2: running max of (the frame an urgent chunk finally settled) minus
	// (mesh_urgent_since_frame), across every urgent chunk this world has
	// ever fully resolved. Read by main.c's heartbeat as edit_to_visible_
	// frames; see world_build_chunks's urgent-drain pass in world.c.
	uint32_t edit_to_visible_frames_max;
	/* Split-dims B1 lease seam (see struct world_lease_owner). gpu_lease is
	 * held from a non-empty render hand-off until the next
	 * world_render_completed drains gpu_busy_chunks; gpu_lease_owner is the
	 * owner it was taken from (the owner pointer may change between). */
	const struct world_lease_owner* lease_owner;
	const struct world_lease_owner* gpu_lease_owner;
	struct world_lease gpu_lease;
	uint32_t gpu_lease_refused;
	/* REL-MP-COREFIRST (2026-10-22): remote (online) loading-screen mesh
	 * focus, set ONLY through world_set_build_focus and ONLY by main.c while
	 * the remote loading screen is up (maintainer scope 10-22: local loading
	 * and in-game order stay as they were). false = world_build_chunks takes
	 * exactly its pre-COREFIRST path. Main thread only; never simulation. */
	bool build_focus;
	w_coord_t build_focus_cx, build_focus_cz; /* chunk coords */
	int build_focus_radius;                   /* Chebyshev, >= 1 */
};

void world_create(struct world* w);
void world_destroy(struct world* w);
void world_unload_section(struct world* w, w_coord_t x, w_coord_t z);
void world_unload_all(struct world* w);
/* Main between frames, server stopped, world unloaded: fence GPU, receive
 * old worker jobs, release the local overlay pool before a provider swap.
 * False on a 500 ms worker timeout or retained storage: keep the provider. */
bool world_finish_local_teardown(struct world* w);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void world_test_fail_section_dict_realloc_after(unsigned occurrence);
#endif
w_coord_t world_get_height(struct world* w, w_coord_t x, w_coord_t z);
void world_copy_heightmap(struct world* w, struct chunk* c, uint8_t* heightmap);
// Store the per-column biome grass/foliage colormap indices (256 bytes each,
// layout [localX*16 + localZ]) into the section containing world column (x, z).
void world_set_section_tint(struct world* w, w_coord_t x, w_coord_t z,
							const uint8_t* grass_i, const uint8_t* grass_j);
// Fetch a single column's stored grass/foliage colormap indices (the inverse
// of world_set_section_tint, one column instead of a whole section). Returns
// false (leaving *out_grass_i/*out_grass_j untouched) when the owning section
// isn't resident, exactly like world_get_height's "not loaded" convention.
// render-slice (RENDER_DISPATCH.md): the client has no biome/generator handle
// of its own (see render_weather.h's precip_biome_from_tint), so this is the
// only per-arbitrary-column biome SIGNAL available to render code.
bool world_get_section_tint_at(struct world* w, w_coord_t x, w_coord_t z,
							   uint8_t* out_grass_i, uint8_t* out_grass_j);
// M1 deep reach: build a far-ring synthetic-voxel chunk directly from its compact
// per-column skin (skin_y/top_id) + tint, bypassing per-cell world_set_block. x,z
// = chunk-origin WORLD coords; arrays are [lx*CHUNK_SIZE + lz], skin_y >=
// WORLD_HEIGHT marks an all-air column. Returns false on dictionary/chunk OOM
// without replacing an existing section.
bool world_build_far_section(struct world* w, w_coord_t x, w_coord_t z,
							 const uint8_t* skin_y, const uint8_t* top_id,
							 const uint8_t* grass_i, const uint8_t* grass_j);
// BULK install a full near chunk column from a CRPC_CHUNK payload (byte-equivalent to
// the old per-cell clin_chunk, but ~20x faster: resolves section + chunks once).
// Returns false on dictionary/chunk OOM without replacing an existing section.
/* Differential local provider and atomic replacement, with one lease per shell.
 * The caller must keep backing alive for the complete call. */
bool world_build_full_section_leased(struct world* w, w_coord_t x, w_coord_t z,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky,
	const uint8_t* torch, const uint8_t* height, const uint8_t* grass_i,
	const uint8_t* grass_j, struct column_backing* backing);
/* Conservative peak general-heap request for initially admitting a local
 * shared-backing world with this many columns. Includes chunk shells,
 * transactional section-dictionary growth and render pointer arrays; the
 * server already owns the packed terrain backing. Subsequent gameplay and
 * relighting remain protected by the separate admission safety margin. */
bool world_local_shared_capacity_mem1_need(uint32_t columns, uint32_t* out);
bool world_build_full_section(struct world* w, w_coord_t x, w_coord_t z,
							  const uint8_t* ids, const uint8_t* metadata,
							  const uint8_t* lighting_sky,
							  const uint8_t* lighting_torch,
							  const uint8_t* heightmap, const uint8_t* grass_i,
							  const uint8_t* grass_j);
// Fetch a column's stored biome grass/foliage colormap indices (256 each,
// layout [localX*16 + localZ]). Returns false if no section is loaded there
// (the caller should then treat the column as untinted/white).
bool world_get_section_tint(struct world* w, w_coord_t x, w_coord_t z,
							uint8_t* grass_i, uint8_t* grass_j);
// allow_far: H-2 band-priority gate (2026-09-03 maintainer ruling). false
// means "starve band 2 entirely" -- no chunk with lod==1 is even offered to
// chunk_check_built this call, regardless of remaining tokens. Callers
// compute this from world_near_ring_status's out_mesh_pending (near ring not
// fully meshed yet -> false); pass true to reach the old unconditional
// behaviour (every test/tool call that predates this gate does).
size_t world_build_chunks(struct world* w, size_t tokens, bool allow_far);
// REL-MP-COREFIRST: render SCHEDULING ORDER only (T1: every chunk meshes
// to the same bytes, only the order changes). While enabled, world_build_chunks
// first offers the chunks of the Chebyshev disk of `radius` around (cx, cz)
// nearest ring first -- the 3x3 safe core (ring 0, ring 1) before anything
// else, including the band-0 urgent passes -- then falls through to the
// unchanged bands. Same chunk_check_built, same band-2 allow_far gate, same B2
// neighbour wait. Disabled (the default after world_create) = byte-identical
// pre-COREFIRST order. main.c enables it only while
// world_build_focus_wanted(...) holds.
void world_set_build_focus(struct world* w, bool enabled, w_coord_t cx,
						   w_coord_t cz, int radius);
// The one scope rule (maintainer 2026-10-22): a REMOTE (online) session whose
// loading screen is up. Local single-player loading and all in-game frames
// keep the original order.
bool world_build_focus_wanted(bool remote_session, bool loading_screen);
// H-3: see struct world's edit_to_visible_frames_max doc comment.
uint32_t world_edit_to_visible_frames_max(struct world* w);
// M1 far-ring LOD: refresh every loaded chunk's render tier from the camera
// distance (call once per frame, before world_build_chunks). Re-dirties chunks
// that crossed a tier boundary so the mesher rebuilds them as full or skin.
void world_update_lod(struct world* w);
// Runtime "reach dial" seed: chunks within this Chebyshev chunk-radius of the
// camera stay full-detail; beyond it they render the far-ring surface skin.
void world_lod_set_near_radius(int chunks);
int world_lod_near_radius(void);
/* Split render reach in chunks (0 = off); see world.c s_split_render_reach. */
void world_set_split_render_reach(int chunks);
int world_split_render_reach(void);
/* Effective server load radius mirror (0 = unset -> MAX_VIEW_DISTANCE). */
void world_set_server_view_radius(int chunks);
int world_server_view_radius(void);

// S2 far-storage compression (docs/stage2_lod_plan_2026-09-03.md S2). Config
// "lod.far_storage" ("voxel" default -- byte-identical to pre-S2 behaviour;
// "skin" -- world_build_far_section skips the per-cell voxel fill and each
// far chunk shell is allocated via chunk_alloc_far_skin/chunk_init_far_skin
// instead, see chunk.h's struct far_column_skin design comment). Read once at
// startup like world_lod_set_near_radius; never touched by the mesher itself.
enum world_far_storage {
	WORLD_FAR_STORAGE_VOXEL = 0, // faithful default: real per-cell block buffers
	WORLD_FAR_STORAGE_SKIN = 1,  // T2: synthesize cells from the column skin
};
void world_set_far_storage(enum world_far_storage mode);
enum world_far_storage world_get_far_storage(void);

// W1-b (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md
// "Fog-end clipping"): the BFS render-reach multiplier applied to
// gstate.config.fog_distance (world.c's FOG_DIST_LESS(neigh, mult), the
// sole call site). Config "lod.fog_clip": "1.13" (default, the historical
// value -- some margin past where the fog TEV/shader stage reaches full
// opacity) | "1.0" (clip exactly at the fog-opaque radius R; see
// docs/deviations.md for the analytic proof this is pixel-identical: the
// terrain fog blend AND the sky-box's own re-fogged geometry both collapse
// to the exact fog/clear colour at v_fog==1, i.e. at world distance ==
// fog_distance). Read once at startup like world_lod_set_near_radius;
// never touched by the mesher itself.
void world_set_fog_clip(float multiplier);
float world_get_fog_clip(void);

// Far-ring keep-up probe (2026-09-03, hw_telemetry.h's far_missing field):
// count sections missing from the annulus (inner_radius_excl, outer_radius_incl]
// (Chebyshev, chunk-INDEX space -- WCOORD_CHUNK_OFFSET(world coord), same
// units SECTION_TO_ID/world_unload_section already take) around
// (center_cx, center_cz). See world.c's doc comment for the cost bound and
// why this is a throttled scan, not an incremental counter.
int world_far_missing_count(struct world* w, w_coord_t center_cx,
							w_coord_t center_cz, int inner_radius_excl,
							int outer_radius_incl);

// Near-ring readiness probe (2026-09-03 maintainer ruling, responsiveness
// lane H-2/H-3): bounded disk scan (Chebyshev <= radius) around (center_cx,
// center_cz). *out_missing = sections with no dict_wsection entry at all
// (not yet streamed); *out_mesh_pending = resident FULL-DETAIL (lod==0)
// chunks in the disk still needing a (re)build (rebuild_displist or
// mesh_in_flight set). Either out-param may be NULL. See world.c's doc
// comment for the shared near-ring definition this feeds (the loading-screen
// gate and the mesher's band-priority far-admission gate both call this) --
// and, in particular, for WHY the lod==0 filter on the pending term is
// load-bearing rather than cosmetic (2026-09-04 RCA: without it the band-2
// gate counted the very chunks it was refusing to build, and pinned).
void world_near_ring_status(struct world* w, w_coord_t center_cx,
							w_coord_t center_cz, int radius, int* out_missing,
							int* out_mesh_pending);

// H-2 band-2 (far) admission bounds (2026-09-04). Documented defaults:
//
//   WORLD_BAND2_NEAR_PENDING_SLACK -- how many outstanding near (lod==0)
//     sub-chunk (re)builds may remain and still admit far chunks this frame.
//     Default 8 == CHUNK_MESHER_QLENGTH (chunk_mesher.h): the near band cannot
//     issue more sends than the mesher queue holds, so tokens beyond that are
//     free for band 2. Raise it to favour distant geometry; lower it (0
//     restores the old all-or-nothing edge) to favour near responsiveness.
//   WORLD_BAND2_STARVE_RELEASE_FRAMES -- period, in world_build_chunks frames,
//     of the unconditional band-2 release valve. Default 30 (~0.5s at 60fps,
//     the same frame scale as chunk.c's CHUNK_NEIGHBOUR_WAIT_MAX). 0 disables
//     the valve entirely (slack becomes the only admission path).
//
// Deliberately compile-time #defines, not runtime config: this is a scheduling
// INVARIANT ("band 2 is never starved indefinitely"), and a knob a config file
// can set back to never-admit re-creates the exact hardware stall these bounds
// exist to make impossible.
#define WORLD_BAND2_NEAR_PENDING_SLACK 8
#define WORLD_BAND2_STARVE_RELEASE_FRAMES 30

// Pure decision function for the above -- see world.c for the full rationale.
// `frame` is struct world's build_frame, read BEFORE world_build_chunks bumps
// it. Returns the `allow_far` argument world_build_chunks should be given.
bool world_band2_allow_far(int near_mesh_pending, uint32_t frame);

// T2 SPIKE (opt-in, PC-measured only; hardware-unconfirmed -- see
// docs/far_ring_geomip_design_2026-08-26.md): the geomipmapping error-metric far-
// ring merge. Config "lod.geomip" ("off" default) + "lod.geomip_error_px" (whole
// screen pixels, default 2). `fov_deg`/`screen_height_px` are read ONCE, after
// gfx_setup() resolves the real resolution (see main.c), to precompute the
// perspective-projection constant k (chunk_lod_geomip.h's
// lod_geomip_projection_k) -- this spike does not re-read them if FOV or
// resolution ever change at runtime (documented limitation, not a bug: neither
// changes today outside startup, apart from the underwater FOV scale, which
// this spike does not special-case either).
void world_lod_set_geomip(bool enabled, float error_budget_px, float fov_deg,
						  float screen_height_px);
bool world_lod_geomip_enabled(void);
float world_lod_geomip_error_budget_px(void);
float world_lod_geomip_projection_k(void);
void world_render_completed(struct world* w, bool new_render);
struct chunk* world_find_chunk_neighbour(struct world* w, struct chunk* c,
										 enum side s);
struct chunk* world_chunk_from_section(struct world* w, struct world_section* s,
									   w_coord_t y);
struct chunk* world_find_chunk(struct world* w, w_coord_t x, w_coord_t y,
							   w_coord_t z);
struct block_data world_get_block(struct world* w, w_coord_t x, w_coord_t y,
								  w_coord_t z);
void world_set_block(struct world* w, w_coord_t x, w_coord_t y, w_coord_t z,
					 struct block_data blk, bool light_update);
// REL-MP-FIX3b: true (and counted) when (x,y,z) is a resident, non-far cell whose
// stored id+metadata already equal blk's and the id is not 36 (piston moving).
// Beta's client drops such an edit with no write and no light work
// (Chunk.java:240-241). The remote CRPC_SET_BLOCK path calls it only in a
// remote session; local mode never does.
bool world_remote_block_unchanged(struct world* w, w_coord_t x, w_coord_t y,
								  w_coord_t z, struct block_data blk);
// Cumulative count of edits world_remote_block_unchanged dropped (heartbeat rskip).
uint32_t world_remote_unchanged_skips(void);
void world_update_lighting(struct world* w);
/* REL-MP-FIX3d: attach ONE caller reservation of lighting_remote_relight_bytes(
 * slots) bytes; in a remote session world_set_block seeds go there (bounded,
 * lossless) and world_update_lighting drains it within its budget. NULL/short
 * storage (a failed reservation) returns false with nothing attached. end
 * detaches (the caller frees the storage). */
bool world_remote_relight_begin(struct world* w, void* storage, size_t bytes,
								size_t slots);
void world_remote_relight_end(struct world* w);
bool world_remote_relight_pending(struct world* w);
// Cumulative remote seeds the reservation had no slot for (kept on the legacy
// stack, nothing dropped); heartbeat "rns", expected to stay 0.
uint32_t world_remote_relight_no_slots(void);
/* LIGHT-CLIENT-LOSSLESS telemetry: columns waiting for a client light repair
 * (upper bound), and columns repaired to the fixed point since boot. */
uint32_t world_light_repair_pending(const struct world* w);
uint32_t world_light_repairs_done(void);
// Hand column (cx,cz) to the client light repair (resident, non-far columns only).
void world_light_repair_mark(struct world* w, w_coord_t cx, w_coord_t cz);
// Absolute floor for the per-frame chunk-list walk bound (see the "corrupt chunk-chain
// guard" block in world.c). The real cap is one entry per resident chunk, kept as a
// high-water mark so unloading sections cannot drop it below the entries the lists still
// hold; this floor keeps a nearly-empty world's walk long enough to be worth bounding.
#define WORLD_ILIST_WALK_CAP_MIN 4096u

// Diagnostic counter for those bounded walks. Non-zero means a render/gpu-busy intrusive
// chain was abandoned mid-walk -- either it never reached its sentinel head within the cap
// (the condition that used to hang the main thread in world_build_chunks) or it offered up
// a node that cannot be safely unlinked. Either way this frame's chunk references were
// dropped rather than double-freed, so a rising count is a real leak, not just a warning.
// Surfaced beside g_displaylist_malformed on the F3 overlay (screen_ingame.c "dlbad
// dlcall ilist" row) and in the watchdog hang report -- both its "ilist=" counter and its
// own cell-bar row, the only instrument a bare frozen console has.
size_t world_ilist_overrun_count(void);
void world_ilist_overrun_reset(void);
// The bound those walks currently run under. Exposed so the dimension-switch regression is
// provable directly: world_unload_all empties w->sections while render/gpu_busy_chunks are
// still full, and a bound that tracked only the CURRENT section count collapsed below the
// live entry count there. Proving that behaviourally would mean allocating enough chunks
// to overflow WORLD_ILIST_WALK_CAP_MIN (4096+ real chunks) just to observe a number this
// returns; asserting the bound never shrinks pins the same invariant exactly.
size_t world_ilist_walk_cap_current(struct world* w);
void world_lighting_driver_diag_reset(void);
size_t world_lighting_driver_last_work(void);
size_t world_lighting_driver_max_work(void);
size_t world_lighting_driver_last_stale_drops(void);
size_t world_lighting_driver_max_stale_drops(void);
void world_preload(struct world* w,
				   void (*progress)(struct world* w, float percent));
bool world_block_intersection(struct world* w, struct ray* r, w_coord_t x,
							  w_coord_t y, w_coord_t z, enum side* s);
/* Prepare every camera before mesh dispatch/flip. Allocation failure returns
 * false before taking any frame references; caller must skip this frame's draw.
 * count is 1..WORLD_RENDER_VIEWS. No terrain is duplicated or distance capped. */
bool world_pre_render_views(struct world*, struct camera* const*, size_t count);
size_t world_render_view(struct world*, struct camera*, unsigned view, bool pass);
void world_update_lod_views(struct world*, struct camera* const*, size_t count);
void world_pre_render(struct world* w, struct camera* c, mat4 view);
void world_pre_render_clear(struct world* w);
void world_redraw_chunks(struct world* w);
size_t world_render(struct world* w, struct camera* c, bool pass);
bool world_aabb_intersection(struct world* w, struct AABB* a);
size_t world_loaded_chunks(struct world* w);
/* colcap-1006 (ASTRA-CC-R4 point 7): display-list owner census of one client
 * world, read-only, main thread. `installed` = finalized lists held by chunks
 * that are in the world's sections (bytes = summed l->length); `own_alloc` =
 * those with their own carve block (each carries the 256 B mem2 guard; lists
 * packed into an M2 mesh block do not); `age_bytes` = installed bytes whose
 * generation is < 5 s, < 30 s and >= 30 s old; `pinned` = lists of chunks on
 * the GPU-busy list that are NO LONGER in the sections (evicted, waiting for
 * the frame fence). Cost: one pass over the sections + one over the GPU-busy
 * list, no allocation, no lock (main owns both). */
struct world_dl_census {
	uint32_t installed_bytes, installed_lists, own_alloc_lists;
	uint32_t age_bytes[3];
	uint32_t pinned_bytes, pinned_lists;
};
void world_dl_census(struct world* w, uint32_t now_ms, struct world_dl_census* out);
/* Split-dims B2: MEASURED resident storage of one world (a client world
 * payload's terrain). mem1_bytes: section dict (capacity), chunk shells
 * (chunk_alloc's exact request), far skins, render-view arrays, lighting
 * stacks. mem2_bytes = voxel_bytes (OWNED_VOXEL block buffers) + mesh_bytes
 * (mesh blocks / finished display lists, in WII-EQUIVALENT bytes: on PC the
 * GX size is derived from each list's vertex count). The sizeof/pointer
 * terms are the BUILD's own (64-bit on PC): only a Wii build's figures are
 * the Wii budget; voxel bytes are platform-independent. LOCAL_SHARED_VIEW chunks read the
 * process-wide column-view pool, which is not per world: counted, not
 * billed. sizeof(struct world) itself is billed by whoever allocated it. */
struct world_memory {
	size_t sections, chunks, voxel_chunks, shared_view_chunks;
	size_t dict_bytes, voxel_bytes, mesh_bytes;
	size_t mem1_bytes, mem2_bytes;
};
void world_memory_usage(struct world* w, struct world_memory* out);

/* Split-dims B2: the world (and its entity store) of the VIEW being ticked or
 * drawn. Readers that used to name &gstate.world / gstate.entities directly
 * (display tick, piston/sign/weather renderers, entity subclasses, the ingame
 * screen) call these instead. Nothing sets a view world unless the client
 * owner enters a view scope with world-bound payloads ON
 * (clin_world_view_enter), so by default they return exactly &gstate.world /
 * gstate.entities. world_view_set(NULL, NULL) restores the default; it
 * returns nothing and is owner-thread only. */
struct dict_entity_s;
struct world* world_view_current(void);
struct dict_entity_s* world_view_entities(void);
void world_view_set(struct world* w, struct dict_entity_s* entities);
/* Wait until the GPU has finished every submitted draw (gfx_wait_idle), for
 * owners outside the render stratum that must fence before releasing a
 * world's frame references (client_frame_worlds.c's second-storage
 * teardown, the same order world_finish_local_teardown uses). */
void world_gpu_wait_idle(void);
const float* world_dimension_light(struct world* w);

/* Main-thread harness anchor; the second footprint uses ordinary full
 * sections, collision reads, mesh jobs and reference-counted GPU retirement. */
void world_set_anchor_sep(int blocks);
/* colcap-1006 R5 item 3: live synthetic-anchor offset (main thread). */
void world_set_anchor_offset(int blocks);
/* Experimental local split-screen: point the SECOND diagnostic anchor at
 * the admitted guest body instead of the synthetic +X offset. Inactive
 * (false) restores the offset exactly. */
void world_set_guest_anchor(bool active, double x, double z);
void world_anchor_status(struct world* w, double x, double y, double z,
						 int server_radius, int far_radius, int missing[2],
						 int pending[2], int far[2], int far_pending[2],
						 bool* collision);

#endif
