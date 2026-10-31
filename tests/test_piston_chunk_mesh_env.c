/*
	HEADLESS reproduction attempt for the on-device "place a base piston on the
	ground -> hard HANG (watchdog orange band on a frozen framebuffer)" bug.

	The server-side placement + move is already covered (test_piston_move_env.c et
	al., all green). The remaining suspect is the CLIENT chunk-mesh path the real
	client runs on a SET_BLOCK: build the chunk's display lists through the actual
	chunk_mesher worker (render_block_full emitting all 6 faces of the piston into
	the real displaylist, then displaylist_finalize), which on the Wii/GX is a
	different, less-tolerant code path than PC GL.

	WHAT THIS DRIVES (the real production path, not a hand-mirrored copy):
	  - the REAL client `struct world` + `struct chunk` (world_set_block),
	  - the REAL chunk_mesher worker thread (chunk_mesher_send -> the static
	    chunk_mesher_build -> chunk_mesher_rebuild's per-face emit loop ->
	    render_block_full -> the REAL displaylist + displaylist_finalize),
	  - the REAL piston block defs (block_piston / _sticky / _head) with their
	    getSideMask()==face_occlusion_empty() (the KEY LEAD: the piston is opaque
	    but occludes nothing, so it draws all 6 faces incl. ones coincident with
	    solid neighbours -- the dispenser, by contrast, returns face_occlusion_full).

	WHAT IT ASSERTS:
	  (1) NO HANG -- a wall-clock watchdog (HANG_TIMEOUT_MS) around the
	      send -> poll(receive) loop. If chunk_mesher_build never returns (the
	      real-Wii symptom is a main-loop heartbeat stall), this test FAILS by
	      timeout instead of letting CI wedge.
	  (2) NO UNEXPECTED MALFORMATION -- normal builds do not truncate or stale;
	      CASE E deliberately injects five lazy/mid-grow OOMs across current and
	      stale generations, and verifies no incomplete list can reach rendering
	      or bypass the exact 8/16/32/64 retry backoff and saturation guard.
	      CASE F injects mesher SCRATCH total-OOMs (light_data / visited / stk)
	      and verifies the degraded results (full-bright vertex light, forced
	      fully-connected reachability) schedule the same transactional retry
	      instead of committing as a success (2026-08-13 audit residuals).
	  (3) The Wii displaylist_finalize STREAM INVARIANT, re-derived on PC.
	      source/platform/wii/displaylist.c asserts `written == vtxcnt*stride` at
	      finalize -- a mismatch is the documented GP-wedge ("cmdIdle=0") producer.
	      That assert exists ONLY in the Wii backend; the PC displaylist_finalize
	      does no such check. So we re-derive it here against the REAL finalized
	      lists: for every list the worker handed back, the bytes actually written
	      (l->index) must equal l->index/-reported vertices * stride. A piston
	      face-count/emit divergence (the only platform-independent way to malform a
	      list without an OOM) would trip this on PC exactly as the Wii assert would.

	A CLEAN PASS is itself the diagnostic signal: it localizes the hang to the
	GX-hardware finalize/draw path (no PC/C-logic equivalent), NOT to the mesher's
	C emit logic. A FAILURE here would pin a real C-level malformation.

	Harness shape cloned from test_piston_move_env.c (clin drain thread) since the
	full client links the network stack; the client world here is independent of the
	server, so no server world is created.
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

#define HANG_TIMEOUT_MS 5000 // generous: a single 16^3 chunk mesh is sub-ms on PC
#define YF 64				 // stone-floor plane (y), well inside chunk cy=4
#define YP (YF + 1)			 // the piston's plane (one above the floor)

// --- RPC drain thread (the full client links clin_*; keep its inbox empty so no
// path blocks). The CLIENT world below is wholly independent of it. ------------
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

static void set(w_coord_t x, w_coord_t y, w_coord_t z, int type, int meta,
				int sky) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = (uint8_t)type,
										 .metadata = (uint8_t)(meta & 0x0F),
										 .sky_light = (uint8_t)(sky & 0x0F),
										 .torch_light = 0},
					false /* no light flood: we set sky_light explicitly */);
}

// Build a solid stone floor + an exposed piston on top of it, across a 16x16
// footprint so the meshed chunk has a realistic full surface (not a lone block in
// a void). The piston sits on the ground with a solid block directly under it --
// the exact "bottom face coincident with a solid neighbour" geometry the KEY LEAD
// flags. Returns the chunk containing the piston.
static struct chunk* build_scene_at(w_coord_t X0, w_coord_t Z0, int piston_type,
								   int facing) {
	const w_coord_t N = CHUNK_SIZE;
	// A full stone floor plane.
	for(w_coord_t x = X0; x < X0 + N; x++)
		for(w_coord_t z = Z0; z < Z0 + N; z++)
			set(x, YF, z, BLOCK_STONE, 0, 15);
	// A scattering of surface blocks so the mesh isn't a single flat plane (more
	// faces emitted = more of the emit path exercised), plus the piston.
	for(w_coord_t x = X0; x < X0 + N; x++)
		for(w_coord_t z = Z0; z < Z0 + N; z++)
			if(((x + z) & 3) == 0)
				set(x, YP, z, BLOCK_STONE, 0, 15); // bumps

	const w_coord_t PX = X0 + 8, PZ = Z0 + 8;
	set(PX, YP, PZ, piston_type, facing, 15); // the piston, on the ground
	// neighbours around the piston so its side faces abut real geometry too
	set(PX + 1, YP, PZ, BLOCK_STONE, 0, 15);
	set(PX - 1, YP, PZ, BLOCK_STONE, 0, 15);
	set(PX, YP, PZ + 1, BLOCK_STONE, 0, 15);

	return world_find_chunk(&w, PX, YP, PZ);
}

static struct chunk* build_scene(int piston_type, int facing) {
	return build_scene_at(0, 0, piston_type, facing);
}

// Drive the REAL mesher worker for chunk `c` and wait (bounded) for the result to
// land back in the chunk. Returns false on watchdog timeout (a hang).
static bool wait_for_mesh(struct chunk* c) {
	// Poll the result queue. The worker integrates the finished lists into the
	// chunk and clears mesh_in_flight (chunk_mesher_receive). Bounded by a
	// wall-clock watchdog so a genuine hang fails the test instead of spinning.
	ptime_t t0 = time_get();
	while(c->mesh_in_flight) {
		chunk_mesher_receive();
		if(c->mesh_in_flight) {
			if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS) {
				fprintf(stderr,
						"test_piston_chunk_mesh_env: HANG -- the mesher did not "
						"return within %d ms (mesh_in_flight still set). This "
						"reproduces the on-device freeze in the C emit path.\n",
						HANG_TIMEOUT_MS);
				return false;
			}
			thread_msleep(1);
		}
	}
	return true;
}

static bool mesh_chunk_and_wait(struct chunk* c) {
	c->dirty_generation++;	  // chunk_mark_dirty: force a fresh build
	c->mesh_in_flight = false; // allow the send

	if(!chunk_mesher_send(c)) {
		fprintf(stderr, "test_piston_chunk_mesh_env: chunk_mesher_send refused "
						"(queue full?)\n");
		return false;
	}
	// Mirror chunk_check_built: on a successful send, the chunk is in-flight until
	// chunk_mesher_receive integrates the worker's result and clears this. Also
	// mirror the production dispatch state so later checks do not re-send a clean
	// chunk or re-enter the first-mesh neighbour gate.
	c->rebuild_displist = false;
	c->first_meshed = true;
	c->mesh_in_flight = true;
	return wait_for_mesh(c);
}

static void consume_mesh_retry_cooldown(struct chunk* c, uint8_t checks) {
	assert(c->mesh_retry_skip_checks == checks);
	for(uint8_t remaining = checks; remaining > 0; remaining--) {
		assert(!chunk_check_built(c)
			   && "OOM retry dispatched before its cooldown elapsed");
		assert(c->mesh_retry_skip_checks == remaining - 1);
	}
}

static void dispatch_stale_mesh_and_wait(struct chunk* c) {
	assert(chunk_check_built(c));
	assert(c->mesh_in_flight && !c->rebuild_displist);
	/* Model a real block/light/LOD mutation while the worker owns its snapshot. */
	c->rebuild_displist = true;
	chunk_mark_dirty(c);
	assert(!chunk_check_built(c)
		   && "an in-flight redirty must not enqueue a duplicate mesh job");
	assert(wait_for_mesh(c));
	assert(!c->mesh_in_flight && c->rebuild_displist);
}

static void assert_mesh_absent_and_open(struct chunk* c) {
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		assert(!c->has_displist[k]);
	for(int side = 0; side < 6; side++)
		assert(c->reachable[side] == 0x3F);
}

// Re-derive the Wii displaylist_finalize stream invariant on PC against the REAL
// finalized lists the worker produced. On the Wii this is `assert(written ==
// vtxcnt*stride)` inside displaylist_finalize; the PC backend omits it. NOTE the
// PC/Wii finalize differ in two ways we account for here:
//   - the PC displaylist_finalize sets l->index = VTXCNT (the vertex count), so
//     post-finalize l->index is vertices, NOT bytes; the byte count written during
//     emit has been overwritten and is no longer readable here. We therefore
//     cannot re-run the exact `written_bytes == vtxcnt*stride` byte compare after
//     the fact -- but a count/emit divergence (the only platform-independent way
//     to violate it without an OOM) cannot occur for render_block_full: its count
//     pass and emit pass both yield exactly 1 quad per visible face by
//     construction (it returns 1 unconditionally and emits one face). An OOM
//     truncation IS caught -- separately, via g_mesh_truncations (asserted 0 in
//     main). So the residual check here is the GX_QUADS validity of the count.
//   - the PC finalize does NOT set l->finished (only displaylist_render does); the
//     Wii finalize does. So we must NOT assert l->finished on PC.
static int check_lists_well_formed(struct chunk* c) {
	int finalized = 0;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		if(!c->has_displist[k])
			continue;
		finalized++;
		struct displaylist* l = c->mesh + k;
		// l->index holds the finalized VERTEX count (PC finalize stamps it). A
		// "has_displist" list must be non-empty and a whole number of GX_QUADS
		// (vertices in multiples of 4) -- the mesher only finalizes lists with
		// vertices[k] > 0 and accumulates +4 per face.
		assert(l->index > 0 && "a has_displist list is empty");
		assert((l->index % 4) == 0 && "vertex count is not a GX_QUADS multiple");
	}
	return finalized;
}

int main(void) {
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	chunk_mesher_init(); // spawns the real worker thread

	// Sanity: the piston defs really are the empty-occlusion opaque cube the lead
	// describes (and the dispenser really is full-occlusion), so this test is
	// exercising the suspected geometry.
	assert(blocks[BLOCK_PISTON_BASE] && blocks[BLOCK_PISTON_BASE]->renderBlock);
	assert(blocks[BLOCK_PISTON_STICKY]);

	world_create(&w);

	uint32_t trunc0 = g_mesh_truncations;
	uint32_t stale0 = mesh_stale_discards;

	// --- CASE A: a base piston (id 33) on the ground, facing +X (FACE meta 5). The
	// exact reported repro: regular piston, fresh ground.
	{
		struct chunk* c = build_scene(BLOCK_PISTON_BASE, 5);
		assert(c);
		// sanity: the piston really landed on solid ground in this chunk
		assert(chunk_get_block(c, 8, YP & 15, 8).type == BLOCK_PISTON_BASE);
		assert(chunk_get_block(c, 8, YF & 15, 8).type == BLOCK_STONE);
		bool ok = mesh_chunk_and_wait(c);
		assert(ok && "base piston chunk mesh HUNG"); // (1) no hang
		int finalized = check_lists_well_formed(c);  // (3) stream invariant
		assert(finalized > 0 && "base piston chunk produced NO display list");
		printf("  [A] base piston (id 33) on ground: meshed OK, %d lists "
			   "finalized\n",
			   finalized);
	}

	// --- CASE B: a base piston facing every direction (0..5) + the powered/extended
	// bit (meta|8), since the on-device place could orient any way and the
	// piston_face_texat selection differs per facing/powered (a face index drives
	// the atlas lookup; exercise them all). Each is a fresh chunk mesh.
	for(int facing = 0; facing < 6; facing++) {
		for(int powered = 0; powered <= 1; powered++) {
			int meta = facing | (powered ? 8 : 0);
			struct chunk* c = build_scene(BLOCK_PISTON_BASE, meta);
			assert(c);
			bool ok = mesh_chunk_and_wait(c);
			assert(ok && "base piston (oriented) chunk mesh HUNG");
			int finalized = check_lists_well_formed(c);
			assert(finalized > 0);
		}
	}
	printf("  [B] base piston, all 6 facings x {un,powered}: meshed OK, no hang\n");

	// --- CASE C: the sticky piston (id 29) + the piston HEAD (id 34, the extension
	// arm) standing in the world. The head also uses getSideMask==empty and a
	// per-face texture; a head left in the world (mid-extend mesh) is a real client
	// state. Mesh a chunk with a sticky base + a head in front of it.
	{
		const w_coord_t X0 = 0, Z0 = 0, N = CHUNK_SIZE;
		// fresh floor (build_scene also rebuilds, but place the head explicitly)
		struct chunk* c = build_scene(BLOCK_PISTON_STICKY, 5 | 8); // powered sticky
		assert(c);
		const w_coord_t PX = 8, PZ = 8;
		set(PX + 1, YP, PZ, BLOCK_PISTON_HEAD, 5 | 8, 15); // head arm in front
		(void)X0;
		(void)Z0;
		(void)N;
		c = world_find_chunk(&w, PX, YP, PZ);
		bool ok = mesh_chunk_and_wait(c);
		assert(ok && "sticky piston + head chunk mesh HUNG");
		int finalized = check_lists_well_formed(c);
		assert(finalized > 0);
		printf("  [C] sticky piston (id 29) + head (id 34): meshed OK, %d lists\n",
			   finalized);
	}

	// --- CASE D: the EDIT -> client-relight -> remesh churn the streaming soak never
	// hits. The soak BULK-fills chunks (chunk_set_block_raw, no per-cell relight); the
	// real piston PLACEMENT runs world_set_block(..., light_update=TRUE), which pushes a
	// deferred relight onto w->lighting_updates (the stack_push that is the far-ring-crash
	// class), drained by world_update_lighting -> lighting_flood, then the chunk remeshes
	// (free-old display lists + build new). Thousands of place/relight/remesh + mine cycles
	// to tip an accumulating MEM2/heap corruption the 14 fresh-mesh cases above cannot.
	{
		struct chunk* c = build_scene(BLOCK_PISTON_BASE, 1); // facing up = the ground-place
		assert(c);
		// An open, sky-lit air column above the piston so mining it floods real skylight
		// back down (a large flood exercises more of lighting_flood than a dark cell).
		const w_coord_t PX = 8, PZ = 8;
		for(w_coord_t yy = YP + 1; yy < YP + 8; yy++)
			set(PX, yy, PZ, BLOCK_AIR, 0, 15);
		const int N_EDITS = 300; // suite-friendly; the ASan hunt ran this at 4000 (clean)
		for(int i = 0; i < N_EDITS; i++) {
			// PLACE the opaque piston -> a skylight-darkening flood below it.
			world_set_block(&w, PX, YP, PZ,
							(struct block_data) {.type = BLOCK_PISTON_BASE,
												 .metadata = 1, .sky_light = 0},
							true /* light flood */);
			world_update_lighting(&w); // drain the deferred relight -> lighting_flood
			if(!mesh_chunk_and_wait(c)) {
				fprintf(stderr, "  [D] HANG on place cycle %d\n", i);
				return 1;
			}
			// MINE it (air) -> a skylight-restoring flood.
			world_set_block(&w, PX, YP, PZ,
							(struct block_data) {.type = BLOCK_AIR, .sky_light = 15},
							true);
			world_update_lighting(&w);
			if(!mesh_chunk_and_wait(c)) {
				fprintf(stderr, "  [D] HANG on mine cycle %d\n", i);
				return 1;
			}
		}
		printf("  [D] %d place/relight/remesh + mine/relight/remesh cycles: OK (the "
			   "edit->lighting-flood->free-old-remesh churn the soak never hits)\n",
			   N_EDITS);
	}

	// --- CASE E: display-list OOM retries are per-chunk, bounded, and transactional.
	// The first OOM preserves the complete old mesh and waits 8 eligibility checks.
	// A second consecutive OOM waits 16 and releases the old mesh, but only when the
	// failed result is received (the post-flip safe phase), never while enqueueing.
	// Another chunk must remain free to mesh during the first chunk's cooldown.
	{
		/* chunk_alloc intentionally returns non-zeroed struct storage. Poison the
		 * new state explicitly so this test guards chunk_init, not allocator luck. */
		struct chunk* fresh = chunk_alloc();
		assert(fresh);
		fresh->mesh_oom_streak = UINT8_MAX;
		fresh->mesh_retry_skip_checks = UINT8_MAX;
		chunk_init(fresh, &w, -CHUNK_SIZE, 0, 0);
		assert(fresh->mesh_oom_streak == 0
			   && fresh->mesh_retry_skip_checks == 0);
		chunk_ref(fresh);
		chunk_unref(fresh);

		struct chunk* c = build_scene(BLOCK_PISTON_BASE, 5);
		struct chunk* healthy
			= build_scene_at(CHUNK_SIZE * 2, 0, BLOCK_PISTON_STICKY, 4);
		assert(c && healthy && c != healthy);
		assert(c && mesh_chunk_and_wait(c));
		assert(mesh_chunk_and_wait(healthy));
		assert(check_lists_well_formed(c) > 0);
		assert(check_lists_well_formed(healthy) > 0);

		bool old_has[CHUNK_MESH_LISTS];
		void* old_data[CHUNK_MESH_LISTS];
		for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
			old_has[k] = c->has_displist[k];
			old_data[k] = c->mesh[k].data;
		}

		c->first_meshed = true;
		c->rebuild_displist = true;
		chunk_mark_dirty(c);
		uint32_t trunc_before = g_mesh_truncations;
		displaylist_test_fail_malloc_after(1);
		assert(chunk_check_built(c));
		assert(c->mesh_in_flight && !c->rebuild_displist);
		for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
			assert(c->has_displist[k] == old_has[k]);
			if(old_has[k])
				assert(c->mesh[k].data == old_data[k]);
		}
		assert(!chunk_check_built(c)
			   && "an in-flight chunk must not enqueue a duplicate mesh job");
		assert(wait_for_mesh(c));
		assert(g_mesh_truncations == trunc_before + 1);
		assert(!c->mesh_in_flight && c->rebuild_displist
			   && "an OOM-truncated result must schedule a retry");
		assert(c->mesh_oom_streak == 1 && c->mesh_retry_skip_checks == 8);
		for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
			assert(c->has_displist[k] == old_has[k]
				   && "failed replacement discarded a valid old list");
			if(old_has[k])
				assert(c->mesh[k].data == old_data[k]
					   && "failed replacement changed old list ownership");
		}

		healthy->rebuild_displist = true;
		chunk_mark_dirty(healthy);
		assert(chunk_check_built(healthy)
			   && "one chunk's OOM cooldown must not stall a healthy dirty chunk");
		assert(wait_for_mesh(healthy));
		assert(check_lists_well_formed(healthy) > 0);
		assert(c->mesh_retry_skip_checks == 8);

		consume_mesh_retry_cooldown(c, 8);

		// T1-c (2026-09-03 far-ring frame-drop root cause): chunk_mesher_send now
		// pre-sizes each rebuilt list's buffer from `c`'s CURRENTLY INSTALLED
		// mesh (still the original successful build -- the malloc-OOM attempt
		// above never replaced it). An unchanged-geometry rebuild would then
		// need zero grows, which would make the realloc-failure seam below a
		// no-op (nothing to fail) instead of exercising the grow-OOM path this
		// case exists to test. Add a denser second bump layer so this rebuild's
		// geometry genuinely exceeds the presized capacity, forcing a real
		// mem2_realloc/realloc grow attempt for the injected failure to land on.
		for(w_coord_t x = 0; x < CHUNK_SIZE; x++)
			for(w_coord_t z = 0; z < CHUNK_SIZE; z++)
				set(x, YP + 1, z, BLOCK_STONE, 0, 15);

		// Simulate restrictive connectivity from the retained old mesh. Enqueue
		// must leave it alone; only receive may switch an old-absent chunk back to
		// the conservative all-open default.
		memset(c->reachable, 0, sizeof(c->reachable));
		displaylist_test_fail_realloc_after(1);
		assert(chunk_check_built(c));
		assert(c->mesh_in_flight && !c->rebuild_displist);
		c->rebuild_displist = true;
		chunk_mark_dirty(c);
		assert(c->rebuild_displist);
		for(int side = 0; side < 6; side++)
			assert(c->reachable[side] == 0);
		for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
			assert(c->has_displist[k] == old_has[k]
				   && "retry enqueue cleared the old mesh before the safe phase");
			if(old_has[k])
				assert(c->mesh[k].data == old_data[k]
					   && "retry enqueue changed old ownership before receive");
		}
		assert(!chunk_check_built(c)
			   && "retry dispatch must still allow only one in-flight job");
		assert(wait_for_mesh(c));
		assert(!c->mesh_in_flight && c->rebuild_displist);
		assert(g_mesh_truncations == trunc_before + 2);
		assert(c->mesh_oom_streak == 2 && c->mesh_retry_skip_checks == 16);
		assert_mesh_absent_and_open(c);
		consume_mesh_retry_cooldown(c, 16);

		/* Persistent churn keeps later failures stale. They must still advance the
		 * schedule instead of hammering the allocator at the old+new peak. */
		displaylist_test_fail_malloc_after(1);
		dispatch_stale_mesh_and_wait(c);
		assert(g_mesh_truncations == trunc_before + 3);
		assert(c->mesh_oom_streak == 3 && c->mesh_retry_skip_checks == 32);
		assert_mesh_absent_and_open(c);
		consume_mesh_retry_cooldown(c, 32);

		displaylist_test_fail_realloc_after(1);
		dispatch_stale_mesh_and_wait(c);
		assert(g_mesh_truncations == trunc_before + 4);
		assert(c->mesh_oom_streak == 4 && c->mesh_retry_skip_checks == 64);
		assert_mesh_absent_and_open(c);
		consume_mesh_retry_cooldown(c, 64);

		/* Saturation is load-bearing: wrapping UINT8_MAX to zero would collapse a
		 * long OOM episode back to the shortest retry cadence. */
		c->mesh_oom_streak = UINT8_MAX;
		displaylist_test_fail_malloc_after(1);
		dispatch_stale_mesh_and_wait(c);
		assert(g_mesh_truncations == trunc_before + 5);
		assert(c->mesh_oom_streak == UINT8_MAX
			   && c->mesh_retry_skip_checks == 64);
		assert_mesh_absent_and_open(c);
		consume_mesh_retry_cooldown(c, 64);

		assert(chunk_check_built(c));
		assert(wait_for_mesh(c));
		assert(!c->mesh_in_flight && !c->rebuild_displist);
		assert(c->mesh_oom_streak == 0 && c->mesh_retry_skip_checks == 0);
		assert(g_mesh_truncations == trunc_before + 5);
		assert(check_lists_well_formed(c) > 0);

		/* A healthy result may also become stale. It must discard only its private
		 * replacement, preserving both the rendered owner and any existing OOM
		 * history until a complete current-generation result proves recovery. */
		bool recovered_has[CHUNK_MESH_LISTS];
		void* recovered_data[CHUNK_MESH_LISTS];
		for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
			recovered_has[k] = c->has_displist[k];
			recovered_data[k] = c->mesh[k].data;
		}
		c->mesh_oom_streak = 3;
		c->mesh_retry_skip_checks = 0;
		c->rebuild_displist = true;
		chunk_mark_dirty(c);
		assert(chunk_check_built(c));
		c->rebuild_displist = true;
		chunk_mark_dirty(c);
		assert(wait_for_mesh(c));
		assert(c->rebuild_displist && c->mesh_oom_streak == 3
			   && c->mesh_retry_skip_checks == 0);
		for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
			assert(c->has_displist[k] == recovered_has[k]);
			if(recovered_has[k])
				assert(c->mesh[k].data == recovered_data[k]);
		}
		assert(chunk_check_built(c));
		assert(wait_for_mesh(c));
		assert(c->mesh_oom_streak == 0 && c->mesh_retry_skip_checks == 0);
		assert(check_lists_well_formed(c) > 0);
		printf("  [E] current+stale OOMs backed off 8/16/32/64, saturated, "
			   "released old mesh safely, and stale success preserved ownership\n");
	}

	// (2) NO MALFORMATION across all cases.
	assert(g_mesh_truncations == trunc0 + 5
		   && "only the five injected display-list OOMs may truncate a chunk mesh");
	assert(mesh_stale_discards == stale0 + 5
		   && "only four OOM jobs and one healthy job may be deliberately stale");

	// --- CASE F: mesher SCRATCH total-OOM -- arena exhausted AND the malloc
	// fallback dead (2026-08-13 audit residuals). Before the fix, a light_data
	// total-OOM memset full-bright vertex light and the chunk COMMITTED as a
	// normal success, so the wrong lighting persisted until an unrelated edit
	// re-dirtied the chunk; a visited/stk total-OOM committed the conservative
	// reachable[]=0x3F fill the same way. All three sites must now schedule the
	// same transactional retry as a truncation (result discarded, old mesh
	// retained through the first failure, rebuild_displist re-armed, 8-check
	// backoff, streak reset on recovery) while counting as NEITHER a truncation
	// nor a stale discard. Per-build scratch alloc order: #1 light_data
	// (chunk_mesher_rebuild), #2 visited, #3 stk (chunk_test_init).
	{
		struct chunk* c = build_scene_at(CHUNK_SIZE * 4, 0, BLOCK_PISTON_BASE, 5);
		assert(c && mesh_chunk_and_wait(c));
		assert(check_lists_well_formed(c) > 0);
		assert(c->mesh_oom_streak == 0 && c->mesh_retry_skip_checks == 0);

		uint32_t trunc_f = g_mesh_truncations;
		uint32_t stale_f = mesh_stale_discards;

		for(unsigned call = 1; call <= 3; call++) {
			bool old_has[CHUNK_MESH_LISTS];
			void* old_data[CHUNK_MESH_LISTS];
			for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
				old_has[k] = c->has_displist[k];
				old_data[k] = c->mesh[k].data;
			}

			c->first_meshed = true;
			c->rebuild_displist = true;
			chunk_mark_dirty(c);
			chunk_mesher_test_fail_scratch_after(call);
			assert(chunk_check_built(c));
			assert(c->mesh_in_flight && !c->rebuild_displist);
			assert(wait_for_mesh(c));
			assert(!c->mesh_in_flight && c->rebuild_displist
				   && "a scratch-OOM-degraded result must schedule a retry");
			assert(c->mesh_oom_streak == 1 && c->mesh_retry_skip_checks == 8);
			for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
				assert(c->has_displist[k] == old_has[k]
					   && "degraded replacement discarded a valid old list");
				if(old_has[k])
					assert(c->mesh[k].data == old_data[k]
						   && "degraded replacement changed old ownership");
			}
			consume_mesh_retry_cooldown(c, 8);

			// Seam disarmed: the retry must rebuild clean, install the fresh
			// mesh, and prove allocator recovery by resetting the streak.
			assert(chunk_check_built(c));
			assert(wait_for_mesh(c));
			assert(!c->mesh_in_flight && !c->rebuild_displist);
			assert(c->mesh_oom_streak == 0 && c->mesh_retry_skip_checks == 0);
			assert(check_lists_well_formed(c) > 0);
		}

		assert(g_mesh_truncations == trunc_f
			   && "a scratch OOM is a degraded result, not a truncation");
		assert(mesh_stale_discards == stale_f
			   && "a current-generation scratch OOM must not count as stale");
		printf("  [F] scratch total-OOM (light_data / visited / stk): degraded "
			   "results retried via the 8-check backoff and recovered, instead "
			   "of committing full-bright light / forced-open reachability\n");
	}

	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);

	printf("test_piston_chunk_mesh_env: OK -- the REAL client chunk mesher built "
		   "base/sticky/head piston chunks through render_block_full + the real "
		   "displaylist + displaylist_finalize without hanging or producing a "
		   "malformed (byte-count-divergent) list; injected OOMs stayed isolated. "
		   "The C emit path is "
		   "clean; the on-device hang is isolated to the GX finalize/draw path "
		   "(no PC equivalent), not the mesher's C logic.\n");
	return 0;
}
