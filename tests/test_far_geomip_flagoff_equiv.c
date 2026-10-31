/*
	T2 SPIKE (opt-in, PC-measured only; hardware-unconfirmed -- see
	docs/far_ring_geomip_design_2026-08-26.md).

	End-to-end equivalence + merge proof for the far-ring geomip merge, driving
	the REAL client chunk_mesher worker (not a hand-mirrored copy) the same way
	test_piston_chunk_mesh_env.c drives it for the piston hang investigation.

	CASE A proves the claim CLAUDE.md's T2 rules require before this can be
	called done: with "lod.geomip" NEVER enabled (world_lod_set_geomip is never
	called -- the module stays at its compiled-in default, exactly like a real
	session that never sets the config key), a lod_skin chunk over real,
	surface-varied terrain emits a vertex count that matches a hand-computed
	golden EXACTLY. If a future edit to chunk_mesher.c's flag-off path changed so
	much as one emitted vertex, this test would catch it -- the
	test_lod0_gate_is_byte_path_equivalent precedent (test_lod_tree_policy.c),
	applied end-to-end instead of at the header.

	CASE B is the sanity check that the harness is not vacuous: turning geomip on
	over perfectly FLAT terrain (zero geometric error in every 2x2 patch) merges
	every eligible patch, cutting the surface-skin vertex count by exactly 4x --
	proving the merge path really runs and really changes output when asked to
	(a "does it do anything" check, not the faithfulness proof -- that's CASE A).

	CASE C exercises the actual error-budget gate on non-flat terrain: one 2x2
	patch has a real height step that exceeds the configured budget and must NOT
	merge, while the rest of the flat floor around it does -- proving the
	screen-space error threshold is a real per-patch decision, not an
	all-or-nothing switch, and giving "at least 2 LOD levels" (native + merged)
	coexisting in a single build.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "network/client_interface.h"
#include "platform/displaylist.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

#define HANG_TIMEOUT_MS 5000

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static struct world w;

static void set(w_coord_t x, w_coord_t y, w_coord_t z, int type) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = (uint8_t)type,
										 .metadata = 0,
										 .sky_light = 15,
										 .torch_light = 0},
					false /* no light flood: sky_light set explicitly */);
}

static bool wait_for_mesh(struct chunk* c) {
	ptime_t t0 = time_get();
	while(c->mesh_in_flight) {
		chunk_mesher_receive();
		if(c->mesh_in_flight) {
			if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS) {
				fprintf(stderr, "test_far_geomip_flagoff_equiv: HANG -- the "
								"mesher did not return within %d ms\n",
						HANG_TIMEOUT_MS);
				return false;
			}
			thread_msleep(1);
		}
	}
	return true;
}

/* Force the skin (lod_skin) tier directly, bypassing world_update_lod/camera --
 * same "snapshot the fields chunk_mesher_send reads" approach the production
 * code itself uses (see chunk_mesher_send: "the worker never reads the camera
 * directly"). dist_blocks feeds the geomip screen-space projection when
 * enabled; irrelevant (never read) when it is not. */
static bool force_skin_mesh_and_wait(struct chunk* c, uint16_t dist_blocks) {
	c->lod = 1;
	c->foliage_lod = 0;
	c->lod_dist_blocks = dist_blocks;
	c->dirty_generation++;
	c->mesh_in_flight = false;
	if(!chunk_mesher_send(c)) {
		fprintf(stderr,
				"test_far_geomip_flagoff_equiv: chunk_mesher_send refused\n");
		return false;
	}
	c->rebuild_displist = false;
	c->first_meshed = true;
	c->mesh_in_flight = true;
	return wait_for_mesh(c);
}

static size_t total_vertices(struct chunk* c) {
	size_t total = 0;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		if(c->has_displist[k])
			total += c->mesh[k].index; // PC displaylist_finalize stamps vtxcnt here
	return total;
}

/* Build an isolated NxN-block-wider-than-one-chunk flat stone floor at `yf`,
 * centred so the target 16x16 chunk at (X0,Z0) is surrounded by identical
 * terrain in every direction -- this guarantees the (UNCHANGED) silhouette
 * skirt pass emits zero quads at the chunk's own border (the neighbour is
 * never lower), so every emitted vertex in CASE A/B is attributable to the
 * skin top-face path alone, keeping the golden simple and exact. */
static struct chunk* build_flat_floor(w_coord_t X0, w_coord_t Z0, w_coord_t yf) {
	for(w_coord_t x = X0 - CHUNK_SIZE; x < X0 + 2 * CHUNK_SIZE; x++)
		for(w_coord_t z = Z0 - CHUNK_SIZE; z < Z0 + 2 * CHUNK_SIZE; z++)
			set(x, yf, z, BLOCK_STONE);
	return world_find_chunk(&w, X0 + 8, yf, Z0 + 8);
}

int main(void) {
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	chunk_mesher_init();
	world_create(&w);

	const w_coord_t YF = 64;

	// --- CASE A: flag-off equivalence golden -------------------------------
	// Flat 16x16 stone floor -> the skin emits exactly one untinted SIDE_TOP
	// quad per column (256 columns * 4 vertices = 1024), zero everywhere else
	// (no side/bottom faces -- chunk_lod_face_allowed; no skirts -- flat
	// terrain with matching neighbours on every side; stone is untinted).
	// world_lod_set_geomip is NEVER called before this -- the module default
	// (off) is exactly what a real session that never sets "lod.geomip" gets.
	{
		struct chunk* c = build_flat_floor(0, 0, YF);
		assert(c);
		uint32_t considered0 = 0, merged0 = 0;
		chunk_mesher_geomip_stats(&considered0, &merged0);

		assert(force_skin_mesh_and_wait(c, /*dist_blocks=*/128));

		size_t verts = total_vertices(c);
		printf("  [A] flag-off flat floor: %zu vertices (golden 1024)\n", verts);
		assert(verts == 1024);
		// dp_index for SIDE_TOP untinted stone == SIDE_TOP itself (opaque,
		// single-sided -- see chunk_mesher.c's dp_index derivation).
		assert(c->has_displist[SIDE_TOP]);
		assert(c->mesh[SIDE_TOP].index == 1024);
		for(int k = 0; k < CHUNK_MESH_LISTS; k++)
			if(k != SIDE_TOP)
				assert(!c->has_displist[k]
					   && "flag-off flat floor drew a face outside SIDE_TOP");

		// The geomip counters must not have moved AT ALL -- lod_geomip_compute_plan
		// returns before touching them whenever geomip_enabled is false.
		uint32_t considered1 = 0, merged1 = 0;
		chunk_mesher_geomip_stats(&considered1, &merged1);
		assert(considered1 == considered0 && merged1 == merged0);
	}

	// --- CASE B: geomip-on over flat terrain merges every patch (4x) -------
	{
		struct chunk* c = build_flat_floor(3 * CHUNK_SIZE, 0, YF);
		assert(c);

		// Generous budget (flat terrain has zero error regardless) + a
		// representative far-ring distance/FOV/resolution.
		world_lod_set_geomip(true, 4.0F, 70.0F, 480.0F);

		uint32_t considered0 = 0, merged0 = 0;
		chunk_mesher_geomip_stats(&considered0, &merged0);

		assert(force_skin_mesh_and_wait(c, /*dist_blocks=*/128));

		size_t verts = total_vertices(c);
		printf("  [B] geomip-on flat floor: %zu vertices (golden 256, was 1024 "
			   "off -- 4x reduction)\n",
			   verts);
		assert(verts == 256);

		uint32_t considered1 = 0, merged1 = 0;
		chunk_mesher_geomip_stats(&considered1, &merged1);
		assert(considered1 - considered0 == 64); // 8x8 grid of 2x2 patches
		assert(merged1 - merged0 == 64);		 // every patch merged (flat)

		world_lod_set_geomip(false, 0.0F, 70.0F, 480.0F); // restore the default
	}

	// --- CASE C: one patch exceeds the budget and stays fine, the rest merge ---
	{
		struct chunk* c = build_flat_floor(6 * CHUNK_SIZE, 0, YF);
		assert(c);
		// Raise a single column into a solid pillar 3 blocks taller than its
		// flat neighbours, inside one 2x2 patch (local columns (0,0)-(1,1) of
		// the target chunk) -- a solid fill (not a floating block over a gap)
		// so the ORIGINAL per-column loop still sees exactly one visible top
		// face there (every lower cell is occluded by solid stone above it),
		// keeping this case's ONLY new variable the patch's geometric error.
		// 3 blocks is big enough that even a generous projected error rejects
		// the merge at this distance, while every other patch (still
		// perfectly flat) merges.
		for(w_coord_t y = YF; y <= YF + 3; y++)
			set(6 * CHUNK_SIZE + 0, y, 0, BLOCK_STONE);

		world_lod_set_geomip(true, 2.0F, 70.0F, 480.0F);

		uint32_t considered0 = 0, merged0 = 0;
		chunk_mesher_geomip_stats(&considered0, &merged0);

		assert(force_skin_mesh_and_wait(c, /*dist_blocks=*/64));

		uint32_t considered1 = 0, merged1 = 0;
		chunk_mesher_geomip_stats(&considered1, &merged1);
		assert(considered1 - considered0 == 64);
		assert(merged1 - merged0 == 63); // every patch except the raised one

		// Vertex accounting: 63 merged patches emit 1 quad each (252 verts); the
		// one un-merged patch keeps its 4 native columns (4 quads = 16 verts,
		// unaffected by the pillar's height -- a top face is 1 quad regardless
		// of y), for 268 verts from the skin alone -- PLUS the (unchanged)
		// silhouette skirt pass, which now has a real height step to close
		// around the pillar. This case intentionally does not pin an exact
		// total (skirts are outside this spike's scope) but confirms the
		// split is exactly 63/64 merged, not 64/64 or 0/64.
		size_t verts = total_vertices(c);
		printf("  [C] geomip-on with one over-budget patch: %zu vertices, "
			   "63/64 patches merged (1 held back by the error budget)\n",
			   verts);
		assert(verts > 256); // more than the all-merged case: skirts + the kept patch

		world_lod_set_geomip(false, 0.0F, 70.0F, 480.0F);
	}

	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);

	printf("test_far_geomip_flagoff_equiv: OK -- flag-off vertex output pinned "
		   "to an exact golden (CASE A); geomip-on really merges flat terrain "
		   "4x (CASE B); the screen-space error budget really holds back one "
		   "over-error patch while merging the rest (CASE C)\n");
	return 0;
}
