#include "settle_observer.h"
settle_callback chunk_settle_observe;
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

#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "block/blocks.h"
#include "chunk.h"
#include "column_observer.h"
#include "column_view.h"
#include "game/game_state.h"
#include "network/server_world.h"
#include "platform/gfx.h"
#include "platform/mem2.h"
#include "platform/displaylist_scene_hash.h"
#include "stack.h"
#include "graphics/gfx_settings.h"
#include "graphics/render_block.h"

#define CHUNK_INDEX(x, y, z) ((x) + ((z) + (y) * CHUNK_SIZE) * CHUNK_SIZE)
#define CHUNK_LIGHT_INDEX(x, y, z)                                             \
	((x) + ((z) + (y) * (CHUNK_SIZE + 2)) * (CHUNK_SIZE + 2))

// Allocate a struct chunk honouring its FULL alignment. The struct is over-aligned
// (>16 bytes -- a SIMD/cglm member raises _Alignof(struct chunk) to 32), but plain
// malloc only guarantees 16-byte alignment on x86-64. With -O3 -march=native, gcc
// trusts the type's 32-byte alignment and vectorises chunk_init's field stores into
// 32-byte ALIGNED AVX writes (vmovdqa); on a 16-byte-aligned chunk those fault -- the
// Linux-CI / x86 segfault (Wii/PowerPC has no such instruction, so it never showed
// there). aligned_alloc/memalign are NOT portable to the MSYS toolchain, so over-
// allocate and stash the base pointer just below the aligned address; chunk_destroy
// frees that base. Pair every chunk_alloc with chunk_destroy.
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static unsigned s_chunk_alloc_fail_countdown;

void chunk_test_fail_alloc_after(unsigned occurrence) {
	s_chunk_alloc_fail_countdown = occurrence;
}
#endif

// --- MEM1 red zone on struct chunk ------------------------------------------
// The 2026-07-27 real-Wii hang (fixed in 1447ffb) was the main thread spinning forever in an
// m-i-list walk over a chain whose links no longer reached its sentinel head. Those links --
// the two ILIST_INTERFACE nodes -- are the LAST members of struct chunk, and struct chunk is
// plain malloc from the newlib MEM1 heap. platform/mem2.c's red zone guards mem2_alloc blocks
// ONLY, so it read 0 for the whole investigation and named nothing; the MEM1 allocation that
// actually got smashed was completely unguarded. This is that same red zone mirrored onto it:
// trailing slack after the struct, patterned at alloc, VERIFIED at release, with the first
// offender captured (block base + the offset inside the guard where the pattern first broke --
// a stable offset across runs fingerprints the writer's stride).
//
// NOT Wii-only, unlike mem2.c's canary. The guard bytes live INSIDE our own malloc block, so
// ASan cannot see a write into them on PC either -- gating this on PLATFORM_WII would make the
// mechanism both untestable and unobservable on the only platform that runs the suite.
//
// WHAT IT CANNOT SEE: verification happens at chunk_destroy, so a chunk still LIVE when the
// console wedges is never checked. Streaming churns the resident set continuously (every
// evicted chunk is destroyed), so a persistent stray writer is caught within seconds of play --
// but a zero count at a hang means "not caught", not "no corruption", exactly as documented for
// the MEM2 canary cell-bar row.
//
// CHUNK_GUARD (chunk.h) and CHUNK_CANARY_BYTE are deliberate duplicates of mem2.c's MEM2_GUARD
// / MEM2_CANARY_BYTE: those are private to that translation unit (mem2.h exports neither), and
// keeping the width and the fill byte identical is what makes a photographed F3 row or hang
// report read the same way for both heaps. The width lives in the HEADER rather than here --
// the one shape difference from mem2.c -- because tests/test_chunk_guard.c has to poke the last
// guard byte to prove the far end of the red zone is really covered, and a test that hard-coded
// 256 would silently stop testing the boundary the day the width changed.
#define CHUNK_CANARY_BYTE 0xA5

static uint32_t s_chunk_guard_hits = 0;
static void* s_chunk_guard_first_ptr = NULL;
static size_t s_chunk_guard_first_off = 0;

static void chunk_guard_fill(struct chunk* c) {
	memset((uint8_t*)c + sizeof(struct chunk), CHUNK_CANARY_BYTE, CHUNK_GUARD);
}

static void chunk_guard_verify(struct chunk* c) {
	const uint8_t* g = (const uint8_t*)c + sizeof(struct chunk);
	for(size_t i = 0; i < CHUNK_GUARD; i++) {
		if(g[i] != CHUNK_CANARY_BYTE) {
			if(s_chunk_guard_hits++ == 0) {
				s_chunk_guard_first_ptr = c;
				s_chunk_guard_first_off = i;
			}
			return; // first broken byte is the diagnostic; the rest adds nothing
		}
	}
}

uint32_t chunk_guard_hits(void) {
	return s_chunk_guard_hits;
}

void chunk_guard_first(void** ptr, size_t* guard_off) {
	if(ptr)
		*ptr = s_chunk_guard_first_ptr;
	if(guard_off)
		*guard_off = s_chunk_guard_first_off;
}

void chunk_guard_reset(void) {
	s_chunk_guard_hits = 0;
	s_chunk_guard_first_ptr = NULL;
	s_chunk_guard_first_off = 0;
}

// Shared alignment/guard setup for BOTH chunk_alloc and chunk_alloc_far_skin
// (S2 far-storage compression). Leaves c->blocks and c->far_skin UNSET --
// each caller decides which backing (a real block buffer or a synthesized
// column skin) this shell gets. Factored out verbatim from the pre-S2
// chunk_alloc body; the GUARD ARITHMETIC reasoning below is unchanged by the
// split.
static struct chunk* chunk_alloc_shell(void) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_chunk_alloc_fail_countdown > 0
	   && --s_chunk_alloc_fail_countdown == 0)
		return NULL;
#endif
	size_t align = __alignof__(struct chunk);
	if(align < sizeof(void*))
		align = sizeof(void*);

	// GUARD ARITHMETIC, worked out explicitly -- the alignment maths below is delicate and the
	// red zone has to fit in the slack WITHOUT disturbing it. With
	//     request = sizeof(struct chunk) + align + sizeof(void*) + CHUNK_GUARD
	// and `aligned` = roundup(base + sizeof(void*), align), so that
	//     base + sizeof(void*)  <=  aligned  <=  base + sizeof(void*) + align - 1:
	//  (a) the LOWER bound gives aligned - sizeof(void*) >= base, so the base pointer stashed
	//      at ((void**)aligned)[-1] always lands inside the block, never before it;
	//  (b) the UPPER bound puts the guard's last byte at
	//        aligned + sizeof(struct chunk) + CHUNK_GUARD - 1
	//          <= base + sizeof(void*) + align - 1 + sizeof(struct chunk) + CHUNK_GUARD - 1
	//          =  base + request - 2,
	//      i.e. inside the block with one byte to spare -- the same single byte of slack this
	//      allocation had before the guard existed, so the guard consumes no alignment headroom;
	//  (c) the guard STARTS at aligned + sizeof(struct chunk), strictly above the stash at
	//      aligned - sizeof(void*) (sizeof(struct chunk) > 0), so filling or verifying it can
	//      never touch the pointer chunk_destroy frees.
	// chunk_destroy is therefore unchanged: it still frees ((void**)c)[-1] == base.
	void* base = malloc(sizeof(struct chunk) + align + sizeof(void*) + CHUNK_GUARD);
	if(!base)
		return NULL;

	uintptr_t aligned = ((uintptr_t)base + sizeof(void*) + (align - 1))
		& ~(uintptr_t)(align - 1);
	((void**)aligned)[-1] = base; // stash the malloc base for chunk_destroy

	struct chunk* c = (struct chunk*)aligned;
	chunk_guard_fill(c); // pattern the red zone before any caller can write past the struct
	return c;
}

// Shared body of chunk_alloc / chunk_alloc_far_voxel: the ~10 KB/chunk block
// buffer (CPU data the mesher reads) lives in MEM2. Allocated HERE so an OOM
// surfaces through the NULL return -- handled by every caller's existing
// `if(!c)` guard -- instead of chunk_init asserting (a no-op under NDEBUG on
// the Wii) and then memset'ing through NULL.
static struct chunk* chunk_alloc_voxel(enum mem2_pool_id pool) {
	struct chunk* c = chunk_alloc_shell();
	if(!c)
		return NULL;
	c->blocks = mem2_tag(mem2_alloc_pooled(CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2, pool), CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2, CARVE_VOXEL);
	if(!c->blocks) {
		free(((void**)c)[-1]);
		return NULL;
	}
	c->far_skin = NULL;
	return c;
}

struct chunk* chunk_alloc(void) { return chunk_alloc_voxel(MEM2_POOL_NONE); }

// FAR-RING COLUMN STORE (mem2.h, FIXED-TENANT POOLS). Byte-for-byte the same
// chunk as chunk_alloc; the ONLY difference is that its block buffer is
// requested from the far-column slot pool instead of the shared first-fit
// carve, so a far column cannot land between two display lists. The tenant
// tag is what makes the pool work: 10,240 B is EVERY chunk's block buffer, so
// a pool keyed on the size alone would be drained by the near field during the
// initial load and the far ring would interleave exactly as before. A full
// pool is not an error -- the request falls through to the shared heap and is
// counted (pool_far_over), i.e. today's behaviour for the excess only.
// Released by the ordinary chunk_destroy path: mem2_free recognises a pooled
// pointer by range.
struct chunk* chunk_alloc_far_voxel(void) {
	return chunk_alloc_voxel(MEM2_POOL_FAR);
}

struct chunk* chunk_alloc_shared(void) {
	struct chunk* c = chunk_alloc_shell();
	if(c) { c->blocks = NULL; c->far_skin = NULL; }
	return c;
}

// S2 far-storage compression: same shell as chunk_alloc, but skips the 10 KB
// MEM2 blocks buffer entirely and allocates the far_column_skin the shell
// will own instead (see chunk.h). blocks stays NULL -- chunk_get_block routes
// every read through far_skin for a chunk in this state.
struct chunk* chunk_alloc_far_skin(void) {
	struct chunk* c = chunk_alloc_shell();
	if(!c)
		return NULL;
	c->blocks = NULL;
	struct far_column_skin* skin = malloc(sizeof(struct far_column_skin));
	if(!skin) {
		free(((void**)c)[-1]);
		return NULL;
	}
	c->far_skin = skin;
	return c;
}

// Bookkeeping shared by chunk_init and chunk_init_far_skin -- everything
// EXCEPT the backing store (blocks buffer or far_skin copy), which each
// caller fills in its own way immediately before/after this call. Factored
// out verbatim from the pre-S2 chunk_init body.
void (*chunk_column_observe_lifetime)(struct chunk*, bool);

static void chunk_init_shell(struct chunk* c, struct world* world, w_coord_t x,
							 w_coord_t y, w_coord_t z) {
	c->x = x;
	c->y = y;
	c->z = z;

	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		c->has_displist[k] = false;
	c->rebuild_displist = false;
	c->dirty_generation = 0;
	c->mesh_block = NULL;
	c->mesh_block_bytes = 0;
	c->first_meshed = false; // B2: gate the first mesh on neighbour presence
	c->neighbour_wait = 0;
	// Full-detail by default until world_update_lod sets the tier from camera distance.
	// (Was previously left uninitialised; B2's gate keys off lod==0, so init it.)
	c->lod = 0;
	c->foliage_lod = 0; // full foliage until world_update_lod bands it by distance
	c->lod_dist_blocks = 0; // T2 geomip spike; dead data unless "lod.geomip" is on
	c->mesh_in_flight = false; // no build dispatched yet
	c->mesh_oom_streak = 0;
	c->mesh_retry_skip_checks = 0;
	// H-2 band-0 priority (2026-09-03 responsiveness lane): not urgent yet.
	c->mesh_urgent = false;
	c->mesh_urgent_since_frame = 0;
	c->world = world;
	c->reference_count = 0;
	c->ownership = c->far_skin ? FAR_SKIN : OWNED_VOXEL;
	c->backing = NULL;
	c->view = NULL;
	// 0 = "never visited by the render BFS"; world->bfs_epoch is never 0, so a
	// fresh chunk is correctly treated as unvisited on its first frame.
	c->tmp_data.visited_epoch = 0;
	// Conservative visibility default: every side traversable until the mesher computes
	// the real occlusion (chunk_mesher_receive overwrites reachable[]). This was left
	// UNINITIALISED -- the garbage made the render BFS skip not-yet-meshed chunks near
	// the camera, so close foliage/terrain popped out for a frame or two (and B2's first-
	// mesh gate, which keeps a chunk unmeshed longer, widened that window). 0x3F = all 6
	// sides open; never under-renders, and the mesher tightens it once the chunk meshes.
	memset(c->reachable, 0x3F, sizeof(c->reachable));

	ilist_chunks_init_field(c);
	ilist_chunks2_init_field(c);
}

/* Split render reach: drop every display list (and the M2 block) of a chunk
 * demoted to the dormant tier, keeping the chunk itself. The next promotion
 * re-dirties it and the mesher rebuilds from the (unchanged) blocks. */
void chunk_mesh_release(struct chunk* c) {
	assert(c);
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		if(c->has_displist[k])
			displaylist_destroy(c->mesh + k);
		c->has_displist[k] = false;
	}
	chunk_mesh_block_release(c);
}

void chunk_mesh_block_release(struct chunk* c) {
	assert(c);
	/* Wii slabs share the last-use fence of every member list. PC has no
	 * production slabs (keep the narrow chunk-guard fixture independent). */
	if(c->mesh_block)
#ifdef PLATFORM_WII
		displaylist_block_free(c->mesh_block, c->mesh_block_bytes);
#else
		mem2_free_sized(c->mesh_block, c->mesh_block_bytes);
#endif
	c->mesh_block = NULL;
	c->mesh_block_bytes = 0;
}

void chunk_init(struct chunk* c, struct world* world, w_coord_t x, w_coord_t y,
				w_coord_t z) {
	assert(c && world && c->blocks); // c->blocks is allocated + NULL-checked by chunk_alloc

	memset(c->blocks, BLOCK_AIR, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2);
	c->far_skin = NULL;
	chunk_init_shell(c, world, x, y, z);
	if(chunk_column_observe_lifetime) chunk_column_observe_lifetime(c, true);
}

void chunk_init_shared(struct chunk* c, struct world* world, w_coord_t x,
	w_coord_t y, w_coord_t z, struct column_view* view, struct column_backing* backing) {
	assert(c && !c->blocks && !c->far_skin && view && backing);
	chunk_init_shell(c, world, x, y, z);
	column_view_ref(view);
	c->ownership = LOCAL_SHARED_VIEW;
	c->view = view;
	c->backing = backing;
	if(chunk_column_observe_lifetime) chunk_column_observe_lifetime(c, true);
}

// S2 far-storage compression companion to chunk_init. c must come from
// chunk_alloc_far_skin (blocks==NULL, far_skin already malloc'd); heightmap/
// top_id are the column's compact skin (world_build_far_section's skin_y+1 /
// top_id), copied into the shell's PRIVATE far_skin so the caller's arrays
// (typically function-local) need not outlive this call. floor_y is
// world_build_far_section's chunk_min (see struct far_column_skin's design
// comment in chunk.h) -- the SAME value for every Y-slice of this column.
void chunk_init_far_skin(struct chunk* c, struct world* world, w_coord_t x,
						 w_coord_t y, w_coord_t z, const uint8_t* heightmap,
						 const uint8_t* top_id, uint8_t floor_y) {
	assert(c && world && !c->blocks && c->far_skin && heightmap && top_id);

	struct far_column_skin* skin = (struct far_column_skin*)c->far_skin;
	memcpy(skin->heightmap, heightmap, sizeof(skin->heightmap));
	memcpy(skin->top_id, top_id, sizeof(skin->top_id));
	skin->floor_y = floor_y;
	chunk_init_shell(c, world, x, y, z);
}

static void chunk_destroy(struct chunk* c) {
	SETTLE(chunk_settle_observe, SET_EVICT, c, NULL, 0, 0, 0, 0);
	assert(c);
	if(chunk_column_observe_lifetime) chunk_column_observe_lifetime(c, false);

	// Verify the MEM1 red zone FIRST, while every byte of this allocation is still ours.
	// Diagnostic only: a hit is counted and the first offender recorded, then the chunk is
	// released down the normal path. Aborting or leaking here would trade a named,
	// survivable symptom for an unnamed one, which is the opposite of what this is for.
	chunk_guard_verify(c);

	// Size-aware free verifies the trailing red-zone canary (Wii; plain free on PC).
	// NULL-safe (mem2_free_sized returns immediately on NULL) -- a skin-storage
	// far chunk has no blocks buffer at all.
	mem2_free_sized(c->blocks, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2);
	// S2 far-storage compression: the private column-skin copy chunk_alloc_far_skin
	// mallocs (see chunk.h). free(NULL) is a no-op, so this is safe for every
	// ordinary (voxel-backed) chunk too.
	free((void*)c->far_skin);
	if(c->view) column_view_unref(c->view);
	else column_backing_unref(c->backing);

	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		if(c->has_displist[k])
			displaylist_destroy(c->mesh + k);
	}
	chunk_mesh_block_release(c);

	free(((void**)c)[-1]); // free the malloc base stashed by chunk_alloc, not c itself
}

void chunk_ref(struct chunk* c) {
	assert(c);
	c->reference_count++;
}

void chunk_retain_backing(struct chunk* c, struct column_backing* backing) {
	assert(c && !c->backing && c->ownership == OWNED_VOXEL);
	column_backing_ref(backing);
	c->backing = backing;
}

void chunk_unref(struct chunk* c) {
	assert(c);
	c->reference_count--;

	if(!c->reference_count)
		chunk_destroy(c);
}

// Branch-free packed-buffer read -- today's pre-S2 chunk_get_block body,
// UNCHANGED, split out so the mesher's per-cell gather can call it directly
// once it already knows (checked ONCE per chunk, not per cell) that c has no
// far_skin. Caller's responsibility: c must be voxel-backed (c->blocks != NULL,
// c->far_skin == NULL) -- chunk_get_block itself remains the correct,
// self-checking entry point for anyone who hasn't already established that.
struct block_data chunk_get_block_voxel(struct chunk* c, c_coord_t x,
										c_coord_t y, c_coord_t z) {
	assert(c && x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE
		   && c->ownership == OWNED_VOXEL && c->blocks);

	size_t idx = CHUNK_INDEX(x, y, z) / 2 * 5;
	size_t off = CHUNK_INDEX(x, y, z) % 2;

	/* storage layout:
		type 0
		type 1
		light 0
		light 1
		meta 1/0
	*/

	struct block_data result;
	memset(&result, 0, sizeof(result)); // deterministic unused bitfield padding
	result.type = c->blocks[idx + off + 0];
	result.metadata = (c->blocks[idx + 4] >> (off * 4)) & 0xF;
	result.sky_light = c->blocks[idx + off + 2] & 0xF;
	result.torch_light = c->blocks[idx + off + 2] >> 4;
	return result;
}

// Every synthesized cell goes through this, like chunk_get_block_voxel's memset:
// struct block_data has four unused padding bits after torch_light, and a
// compound literal leaves them indeterminate (C11 6.2.6.1p6). Linux gcc -O2 was
// observed carrying a stray 0x40 into them on the far-skin halo, so the owned
// and generic mesher gathers differed byte-wise (test_chunk_gather_equiv) even
// though every field agreed. Fields are unchanged; only the padding is pinned.
static inline struct block_data block_data_make(uint8_t type, uint8_t metadata,
												 uint8_t sky_light,
												 uint8_t torch_light) {
	struct block_data result;
	memset(&result, 0, sizeof(result));
	result.type = type;
	result.metadata = metadata;
	result.sky_light = sky_light;
	result.torch_light = torch_light;
	return result;
}

struct block_data chunk_get_block(struct chunk* c, c_coord_t x, c_coord_t y,
								  c_coord_t z) {
	assert(c && x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE);

	if(c->ownership == LOCAL_SHARED_VIEW)
		return column_view_get(c->view, (x * CHUNK_SIZE + z) * WORLD_HEIGHT + c->y + y);
	if(c->ownership == FAR_SKIN) {
		// S2 far-storage compression: no physical buffer exists for this chunk --
		// synthesize the cell exactly as world_build_far_section's voxel-fill
		// loop would have written it (see struct far_column_skin in chunk.h).
		// O(1): 1-3 array reads + 1-2 compares -- no more expensive than the
		// packed-buffer unpack below, so running this every mesh-gather cell
		// does not change that phase's cost class (measured in
		// tests/test_far_storage_equiv.c's CASE E).
		size_t i = (size_t)x * CHUNK_SIZE + z;
		uint8_t hm = c->far_skin->heightmap[i];
		w_coord_t wy = c->y + (w_coord_t)y;
		uint8_t open_sky
			= (c->world->dimension == WORLD_DIM_OVERWORLD) ? 15 : 0;
		if(hm == 0) {
			// All-air column: the voxel path's OWN branch for this case fills
			// the WHOLE shell range with lit air regardless of floor_y/chunk_min
			// (see far_column_skin's design comment) -- checked first so it is
			// never shadowed by the floor_y test below.
			return block_data_make(BLOCK_AIR, 0, open_sky, 0);
		}
		if(wy < (w_coord_t)c->far_skin->floor_y)
			// Below world_build_far_section's chunk_min: the voxel fill loop
			// never reaches here for a solid column (it starts AT chunk_min),
			// so this cell is chunk_init's untouched plain-air memset --
			// unlit, not the dimension's open-sky value.
			return block_data_make(BLOCK_AIR, 0, 0, 0);
		if(wy < (w_coord_t)hm)
			return block_data_make(c->far_skin->top_id[i], 0, 0, 0);
		return block_data_make(BLOCK_AIR, 0, open_sky, 0);
	}

	return chunk_get_block_voxel(c, x, y, z);
}

// for global world lookup
struct block_data chunk_lookup_block(struct chunk* c, w_coord_t x, w_coord_t y,
									 w_coord_t z) {
	assert(c);
	struct chunk* other = c;

	if(x < 0 || y < 0 || z < 0 || x >= CHUNK_SIZE || y >= CHUNK_SIZE
	   || z >= CHUNK_SIZE)
		other = world_find_chunk(c->world, c->x + x, c->y + y, c->z + z);

	return other ?
		chunk_get_block(other, W2C_COORD(x), W2C_COORD(y), W2C_COORD(z)) :
		block_data_make(
			// Missing-neighbour placeholder used by the mesher's padded border view.
			// type stays opaque below WORLD_HEIGHT so a chunk at the load frontier does
			// NOT draw a wall of faces toward the unloaded side (those faces are culled).
			// Overworld sky_light is 15: the previous 0 dragged the
			// mesher's smooth-lighting average DOWN on the faces that ARE drawn near the
			// border, producing the dark "blackout line" along the render frontier. The
			// visible frontier is the open surface (genuinely lit to sky), so 15 is the
			// closer approximation; once the real neighbour streams in the chunk re-meshes
			// with its true light. Hell hasNoSky, so applying the same placeholder there
			// creates a bright annulus edge; keep it zero in the Nether. Render-only --
			// no stored light or simulation changes.
			(y < WORLD_HEIGHT) ? 1 : 0, 0,
			(c->world->dimension == WORLD_DIM_OVERWORLD) ? 15 : 0, 0);
}

/* `invalidate_inflight`: also bump the neighbour's dirty generation, which makes
 * any mesh ALREADY IN FLIGHT for it fail chunk_mesher's generation check and be
 * discarded instead of integrated.
 *
 * Setting rebuild_displist alone (what this always did) queues a rebuild but
 * does NOT stop the stale in-flight mesh landing first, so the neighbour draws
 * one frame built from a border it no longer has. Passed true for BLOCK edits,
 * where that frame shows real geometry error -- a hole or a doubled face along
 * the chunk seam. Passed false for LIGHT edits: chunk_set_light runs PER LIT
 * CELL under the lighting flood, so invalidating there would multiply the
 * mesh build/discard churn across a whole flood for a one-frame difference in
 * border SHADING, which is not worth it on a single 729MHz core. Fixed
 * 2026-07-31 (adversarial review); the split is the deliberate part.
 *
 * H-2 band-0 priority (2026-09-03 maintainer ruling): `invalidate_inflight`
 * doubles as the "mark urgent" signal -- it is already exactly "true for a
 * real BLOCK edit, false for a per-cell light flood", which is precisely the
 * band-0 boundary the maintainer specified ("player-edit chunk + its lateral
 * neighbours (from chunk_set_block)"). A light-only flood does NOT jump the
 * queue: it can touch hundreds of cells per flood, and every one already
 * gets a mesh rebuild queued (just not an URGENT one) via rebuild_displist
 * below. */
void chunk_mark_urgent(struct chunk* c) {
	assert(c);
	if(!c->mesh_urgent) {
		c->mesh_urgent = true;
		c->mesh_urgent_since_frame = c->world->build_frame;
		c->world->mesh_urgent_count++;
	}
}

static void chunk_trigger_neighbour_update(struct chunk* c, c_coord_t x,
										   c_coord_t y, c_coord_t z,
										   bool invalidate_inflight) {
	// TODO: diagonal chunks, just sharing edge or single point

	bool cond[6] = {
		x == 0, x == CHUNK_SIZE - 1, y == 0, y == CHUNK_SIZE - 1,
		z == 0, z == CHUNK_SIZE - 1,
	};

	int offset[6][3] = {
		{-1, 0, 0},			{CHUNK_SIZE, 0, 0}, {0, -1, 0},
		{0, CHUNK_SIZE, 0}, {0, 0, -1},			{0, 0, CHUNK_SIZE},
	};

	for(int k = 0; k < 6; k++) {
		if(cond[k]) {
			struct chunk* other
				= world_find_chunk(c->world, c->x + offset[k][0],
								   c->y + offset[k][1], c->z + offset[k][2]);
			if(other) {
				other->rebuild_displist = true;
				if(invalidate_inflight) {
					chunk_mark_dirty(other);
					chunk_mark_urgent(other);
				}
				SETTLE(chunk_settle_observe, SET_DIRTY_NEIGHBOUR, other, NULL,
					   0, 0, 0, 0);
			}
		}
	}
}

void chunk_set_light(struct chunk* c, c_coord_t x, c_coord_t y, c_coord_t z,
					 uint8_t light) {
	assert(c && x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE);
	// A skin-storage far chunk is synthesized read-only from far_skin (chunk_get_block)
	// and has no blocks buffer to write into. The client relight flood must never reach
	// one (is_far_lod stops it at the near/far seam) -- this fires loudly if that
	// invariant is ever violated instead of corrupting memory through a NULL c->blocks.
	assert(!c->far_skin
		   && "chunk_set_light on a skin-storage far chunk (relight crossed the seam)");

	size_t idx = CHUNK_INDEX(x, y, z) / 2 * 5;
	size_t off = CHUNK_INDEX(x, y, z) % 2;

	if(c->view)
		column_view_set(c->view, (x * CHUNK_SIZE + z) * WORLD_HEIGHT + c->y + y,
			(struct block_data){.sky_light = light & 15, .torch_light = light >> 4}, true);
	else
		c->blocks[idx + off + 2] = light;
	chunk_mark_dirty(c);
	SETTLE(chunk_settle_observe, SET_DIRTY_LIGHT, c, NULL, 0, 0, 0, 0);
	c->rebuild_displist = true;

	chunk_trigger_neighbour_update(c, x, y, z, false);
}

void chunk_set_block_raw(struct chunk* c, c_coord_t x, c_coord_t y, c_coord_t z,
						 struct block_data blk) {
	assert(c && x < CHUNK_SIZE && y < CHUNK_SIZE && z < CHUNK_SIZE);
	// A skin-storage far chunk is never per-cell filled -- world_build_far_section's
	// skin branch bakes the whole column via chunk_init_far_skin instead (see
	// chunk.h). Catch a stray caller here rather than writing through NULL c->blocks.
	assert(!c->far_skin
		   && "chunk_set_block_raw on a skin-storage far chunk (read-only)");
	if(c->view) {
		column_view_set(c->view, (x * CHUNK_SIZE + z) * WORLD_HEIGHT + c->y + y, blk, false);
		return;
	}

	size_t idx = CHUNK_INDEX(x, y, z) / 2 * 5;
	size_t off = CHUNK_INDEX(x, y, z) % 2;

	c->blocks[idx + off + 0] = blk.type;
	c->blocks[idx + off + 2] = (blk.torch_light << 4) | blk.sky_light;
	c->blocks[idx + 4] = (c->blocks[idx + 4] & ~(0x0F << (off * 4)))
		| (blk.metadata << (off * 4));
}

/* Transpose a server Y-contiguous column slice into the client X-paired
 * five-byte layout. Each pair is written once, including both metadata nibbles.
 * No dirty/neighbour effects, like chunk_set_block_raw. */
void chunk_load_packed_slice(struct chunk* c, unsigned base_y,
							 const uint8_t* ids, const uint8_t* metadata,
							 const uint8_t* sky, const uint8_t* torch) {
	assert(c && c->blocks && !c->far_skin && ids && metadata && sky && torch);
	assert(base_y % CHUNK_SIZE == 0 && base_y <= WORLD_HEIGHT - CHUNK_SIZE);
	uint8_t* dst = c->blocks;
	for(unsigned y = base_y; y < base_y + CHUNK_SIZE; y++) {
		unsigned shift = (y & 1) * 4;
		for(unsigned z = 0; z < CHUNK_SIZE; z++) {
			size_t a = z * WORLD_HEIGHT + y;
			for(unsigned x = 0; x < CHUNK_SIZE; x += 2) {
				size_t b = a + CHUNK_SIZE * WORLD_HEIGHT;
				dst[0] = ids[a];
				dst[1] = ids[b];
				dst[2] = (((torch[a/2] >> shift) & 15) << 4)
					| ((sky[a/2] >> shift) & 15);
				dst[3] = (((torch[b/2] >> shift) & 15) << 4)
					| ((sky[b/2] >> shift) & 15);
				dst[4] = ((metadata[a/2] >> shift) & 15)
					| (((metadata[b/2] >> shift) & 15) << 4);
				dst += 5;
				a += 2 * CHUNK_SIZE * WORLD_HEIGHT;
			}
		}
	}
}

void chunk_set_block(struct chunk* c, c_coord_t x, c_coord_t y, c_coord_t z,
					 struct block_data blk) {
	chunk_set_block_raw(c, x, y, z, blk);
	chunk_mark_dirty(c);
	SETTLE(chunk_settle_observe, SET_DIRTY_EDIT, c, NULL, 0, 0, 0, 0);
	c->rebuild_displist = true;
	// H-2 band 0: the edited chunk itself, not just the neighbours
	// chunk_trigger_neighbour_update marks below.
	chunk_mark_urgent(c);

	chunk_trigger_neighbour_update(c, x, y, z, true);
}

// B2: are all four horizontal neighbours of this chunk resident? Vertical neighbours
// always co-load with the column (clin_chunk fills the whole column at once), so only
// the four lateral columns stream independently and need checking before a clean mesh.
static bool chunk_horizontal_neighbours_ready(struct chunk* c) {
	return world_find_chunk_neighbour(c->world, c, SIDE_LEFT)
		&& world_find_chunk_neighbour(c->world, c, SIDE_RIGHT)
		&& world_find_chunk_neighbour(c->world, c, SIDE_FRONT)
		&& world_find_chunk_neighbour(c->world, c, SIDE_BACK);
}

// Max frames to hold a chunk's first mesh waiting for its neighbours before meshing
// anyway. ~30 frames (~0.5s @ 60fps) bounds the wait so an edge-of-view chunk (whose
// outer neighbour never streams) is never left invisible; with A4's faster streaming
// the neighbours of an interior chunk usually arrive within a few frames.
#define CHUNK_NEIGHBOUR_WAIT_MAX 30

bool chunk_check_built(struct chunk* c) {
	assert(c);

	/* Dormant (split render reach): nothing to build, never offered. */
	if(c->lod == 2)
		return false;
	if(c->rebuild_displist) {
		// In-flight guard: a build for this chunk is already dispatched and not yet
		// integrated. Don't issue a second concurrent send (it would just race to the
		// same generation and one result would be stale-discarded). Leave rebuild_displist
		// set; chunk_mesher_receive clears mesh_in_flight when the in-flight result lands
		// and we re-send next frame with the latest generation. (Also skips the B2
		// neighbour-wait increment below so the wait counter isn't spuriously advanced.)
		if(c->mesh_in_flight) {
			SETTLE(chunk_settle_observe, SET_GATE_INFLIGHT, c, NULL, 0, 0, 0,
				   0);
			return false;
		}

		// A failed build re-arms rebuild_displist in chunk_mesher_receive, but
		// retries must not hammer the allocator every frame. Count only checks at
		// which this chunk is otherwise eligible (not calls while a job is in
		// flight), and leave all other chunks free to use the worker meanwhile.
		if(c->mesh_retry_skip_checks > 0) {
			c->mesh_retry_skip_checks--;
			SETTLE(chunk_settle_observe, SET_GATE_RETRY, c, NULL, 0, 0, 0, 0);
			return false;
		}

		// B2: hold the FIRST mesh until the 4 horizontal neighbours are resident, so
		// the mesher's padded border samples real neighbour blocks instead of the
		// missing-neighbour placeholder (which culls border leaf/terrain faces and
		// forces an up-to-4x re-mesh as each neighbour later lands). Bounded so an edge
		// chunk still meshes. Re-dirties after the first build skip this (first_meshed)
		// and mesh at once -- a real neighbour change must be reflected immediately.
		// Only full-detail (lod==0) near chunks are gated: far-ring skins (lod==1) are a
		// coarse LOD meant to fill the distance fast and stream at their own slower rate,
		// so gating them on neighbours would just time out and waste a re-mesh.
		if(c->lod == 0 && !c->first_meshed
		   && c->neighbour_wait < CHUNK_NEIGHBOUR_WAIT_MAX
		   && !chunk_horizontal_neighbours_ready(c)) {
			c->neighbour_wait++;
			SETTLE(chunk_settle_observe, SET_GATE_NEIGHBOUR, c, NULL, 0, 0, 0,
				   0);
			return false; // leave rebuild_displist set; retry next frame
		}

		if(chunk_mesher_send(c)) {
			c->rebuild_displist = false;
			c->first_meshed = true;
			c->mesh_in_flight = true; // cleared in chunk_mesher_receive
			return true;
		}
	}

	return false;
}

void chunk_pre_render(struct chunk* c, mat4 view, bool has_fog) {
	assert(c && view);

	glm_translate_to(view, (vec3) {c->x, c->y, c->z}, c->model_view);
	c->has_fog = has_fog;
}

static void check_matrix_set(struct chunk* c, bool* needs_matrix, float x, float z) {
	assert(c && needs_matrix);

	if(*needs_matrix) {
		gfx_matrix_modelview(c->model_view);
		gfx_fog(c->has_fog);
		gfx_fog_pos(c->x - x, c->z - z,
					gstate.config.fog_distance);
		*needs_matrix = false;
	}
}

// Draw one tint-group of a chunk's per-side display lists, back-face culled by
// camera position. base = 0 for the untinted (9-byte GX_VTXFMT0) lists, or
// CHUNK_MESH_PARTS for the biome-tinted (12-byte GX_VTXFMT4) lists. The caller
// brackets the tinted group with the matching gfx_tint() state.
static void chunk_render_group(struct chunk* c, int base, bool pass, float x,
							   float y, float z, bool* needs_matrix) {
	int offset = pass ? 6 : 0;

	if(y < c->y + CHUNK_SIZE && c->has_displist[base + SIDE_BOTTOM + offset]) {
		check_matrix_set(c, needs_matrix, x, z);
		SETTLE(chunk_settle_observe, SET_DRAW, c, NULL, 0, 0, 0, 0);
		displaylist_render(c->mesh + base + SIDE_BOTTOM + offset);
	}

	if(y > c->y && c->has_displist[base + SIDE_TOP + offset]) {
		check_matrix_set(c, needs_matrix, x, z);
		SETTLE(chunk_settle_observe, SET_DRAW, c, NULL, 0, 0, 0, 0);
		displaylist_render(c->mesh + base + SIDE_TOP + offset);
	}

	if(x < c->x + CHUNK_SIZE && c->has_displist[base + SIDE_LEFT + offset]) {
		check_matrix_set(c, needs_matrix, x, z);
		SETTLE(chunk_settle_observe, SET_DRAW, c, NULL, 0, 0, 0, 0);
		displaylist_render(c->mesh + base + SIDE_LEFT + offset);
	}

	if(x > c->x && c->has_displist[base + SIDE_RIGHT + offset]) {
		check_matrix_set(c, needs_matrix, x, z);
		SETTLE(chunk_settle_observe, SET_DRAW, c, NULL, 0, 0, 0, 0);
		displaylist_render(c->mesh + base + SIDE_RIGHT + offset);
	}

	if(z < c->z + CHUNK_SIZE && c->has_displist[base + SIDE_FRONT + offset]) {
		check_matrix_set(c, needs_matrix, x, z);
		SETTLE(chunk_settle_observe, SET_DRAW, c, NULL, 0, 0, 0, 0);
		displaylist_render(c->mesh + base + SIDE_FRONT + offset);
	}

	if(z > c->z && c->has_displist[base + SIDE_BACK + offset]) {
		check_matrix_set(c, needs_matrix, x, z);
		SETTLE(chunk_settle_observe, SET_DRAW, c, NULL, 0, 0, 0, 0);
		displaylist_render(c->mesh + base + SIDE_BACK + offset);
	}

	// DC-GRASS G2: the TINTED double-sided list (base == CHUNK_MESH_PARTS,
	// list 12) holds only the grass tile-38 side overlays: no other block is
	// both double_sided and biome-tinted (leaves are tinted but single-sided,
	// so they stay in the tinted side lists above with today's alpha state --
	// test_mesher_nongrass_equiv pins that list empty for every non-grass
	// block). That isolates Beta's overlay alpha cut, glAlphaFunc(GL_GREATER,
	// 0.1F) (client EntityRenderer.java:773), to exactly the overlay draw --
	// see RENDER_GRASS_OVERLAY_ALPHA_REF for the strict a > 0.1 ref per
	// backend -- restored to world_render's gfx_alpha_test(true) after. It is
	// drawn last in the opaque pass, after the untinted base sides (equal
	// depth, LEQUAL), with the opaque pass's BACK-FACE culling left on (Beta
	// draws the overlay with culling enabled): overlay quads have their base
	// quad's exact corners and winding, so base and overlay are culled
	// together from any viewpoint -- camera inside a grass cube, or a far
	// face seen through a transparent pack's texels. Drawn even without
	// GFX_DOUBLESIDED (overlays are not "double-sided decorations"). Unlike
	// the per-side base lists this one list is always submitted (lead/Astra
	// ruling 10-11, option A); faces it would have skipped are back-facing
	// and culled by the GPU, so pixels match -- cost measured in
	// perf_stress_5m.
	bool grass_overlay = base == CHUNK_MESH_PARTS;
	#ifndef GFX_DOUBLESIDED
	if(grass_overlay)
	#endif
	if(!pass && c->has_displist[base + 12]) {
		check_matrix_set(c, needs_matrix, x, z);
		SETTLE(chunk_settle_observe, SET_DRAW, c, NULL, 0, 0, 0, 0);
		if(grass_overlay) {
			gfx_alpha_test_ref(true, RENDER_GRASS_OVERLAY_ALPHA_REF);
			displaylist_render(c->mesh + base + 12);
			gfx_alpha_test(true);
		} else {
			gfx_cull_func(MODE_NONE);
			displaylist_render(c->mesh + base + 12);
			gfx_cull_func(MODE_BACK);
		}
	}
}

// Does this chunk have any biome-tinted list that could draw in this pass? Lets
// the caller skip the gfx_tint() bracket entirely for the common all-untinted
// chunk (no wasted TEV reconfig).
static bool chunk_has_tinted(struct chunk* c, bool pass) {
	int offset = pass ? 6 : 0;
	for(int s = 0; s < 6; s++)
		if(c->has_displist[CHUNK_MESH_PARTS + s + offset])
			return true;
	// (list 12 of the tinted group = grass overlays, drawn with or without
	// GFX_DOUBLESIDED -- see chunk_render_group)
	if(!pass && c->has_displist[CHUNK_MESH_PARTS + 12])
		return true;
	return false;
}

void chunk_render(struct chunk* c, bool pass, float x, float y, float z) {
	assert(c);

	bool needs_matrix = true;
	/* colcap-1006 (ASTRA-CC-R5 item 2b): bracket this chunk's lists for the
	 * pool self-test's ordered GX scene hash (displaylist_scene_hash.h);
	 * one load and branch when it is not armed. */
	if(g_dl_scene_hash_armed)
		dl_scene_hash_chunk_begin(c->x, c->y, c->z, pass, c);

	// Untinted (9-byte) lists -- the bulk of the geometry. world_render runs the
	// chunk passes with gfx_tint disabled, so the CLR1 biome-tint channel + its
	// TEV stage are off (tex*light, no multiply). That is pixel-identical to the
	// old all-tinted path multiplying by a white tint (white == identity).
	chunk_render_group(c, 0, pass, x, y, z, &needs_matrix);

	// Tinted (12-byte) lists -- grass tops, leaves, tall grass. Only a minority
	// of chunks have any, so gate the gfx_tint toggles on their presence.
	if(chunk_has_tinted(c, pass)) {
		gfx_tint(true);
		chunk_render_group(c, CHUNK_MESH_PARTS, pass, x, y, z, &needs_matrix);
		gfx_tint(false);
	}
	if(g_dl_scene_hash_armed)
		dl_scene_hash_chunk_end();
}
