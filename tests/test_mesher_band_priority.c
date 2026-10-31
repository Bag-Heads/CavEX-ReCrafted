/*
	H-2 (2026-09-03 maintainer ruling, responsiveness lane) golden + scheduling
	proof: world_build_chunks' band priority is pure ORDERING -- the same
	chunk data meshes to the SAME final bytes regardless of when band 2 (far,
	lod==1) was allowed to submit relative to band 1 (near, lod==0), and:

	  (1) STRICT FAR STARVATION -- while ANY near (lod==0) chunk still needs a
	      (re)build (rebuild_displist or mesh_in_flight set), a far (lod==1)
	      chunk is not even offered to chunk_mesher_send: it never becomes
	      mesh_in_flight and never gains a display list, however many frames
	      pass, as long as the caller keeps computing allow_far=false (which
	      main.c derives from world_near_ring_status's out_mesh_pending).
	  (2) FAR PROCEEDS ONCE NEAR SETTLES -- the same far chunk meshes
	      normally once the caller starts passing allow_far=true.
	  (3) BAND 0 JUMPS THE QUEUE -- when the mesher token budget is scarce (the
	      test-only enqueue-cap override forces this to 1), a chunk marked
	      urgent (chunk_mark_urgent -- the player-edit/neighbour-install
	      signal) is the one that gets the single available send, ahead of an
	      equally-dirty ordinary near chunk that was NOT marked urgent.
	  (4) BYTE-IDENTICAL, ONLY LATER -- both the near and the (starved, then
	      released) far chunk's final meshes are byte-for-byte identical to
	      the SAME chunk meshed alone with no gating at all.
	  (5) THE GATE'S ADMISSION RULE IS BOUNDED (2026-09-04) --
	      world_band2_allow_far admits band 2 while the near backlog fits in
	      one mesher queue, and admits it unconditionally at least once in
	      every WORLD_BAND2_STARVE_RELEASE_FRAMES frames however large the
	      near backlog is. Asserted directly on the pure function: there is
	      no reachable (pending, frame-window) pair that starves band 2
	      forever.
	  (6) THE OLD `pending == 0` RULE PINNED, AND THE NEW ONE CONVERGES
	      (2026-09-04 RCA of real-Wii run staged_far_walk_6m_20260904_083316,
	      where the heartbeat's near_missing sat at ~674 of 968 sub-chunks for
	      6 minutes and two thirds of the world's triangles never meshed).
	      A 5x5 disk is installed with its outer shell at lod==1 -- the
	      shipped Wii geometry, where the runtime reach dial (near_radius=2)
	      is SMALLER than the server's near-send radius MAX_VIEW_DISTANCE=5,
	      so band-2 chunks sit INSIDE the near-ring scan disk. Driven with the
	      OLD rule (allow_far = "no chunk in the disk, at ANY lod, is
	      pending") the far shell provably never meshes and the count never
	      falls: the gate was counting the very chunks it refuses to build.
	      Driven with the shipped rule (world_near_ring_status's lod==0
	      pending term + world_band2_allow_far) the SAME world converges to
	      zero pending with every far chunk meshed -- and a far chunk is
	      shown being admitted while the near pending count is still nonzero,
	      which the old all-or-nothing edge made impossible.

	Drives the REAL production path (chunk_mesher_init -> chunk_mesher_send on
	real chunks in a real world -> the real worker -> chunk_mesher_receive),
	same harness shape as test_chunk_mesher_enqueue_cap_env.c /
	test_chunk_mesher_slice_env.c. Chunks are NOT pushed into w->render (no
	camera/BFS setup needed): world_build_chunks' second pass (the
	dict_wsection walk) reaches any resident chunk regardless of render-list
	membership, and applies the SAME lod==1-vs-allow_far filter as the first
	pass -- see world.c's doc comment on world_build_chunks.
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
#define YF 64 // stone-floor plane, well inside chunk cy=4
#define STARVE_FRAMES 40 // far more than any legitimate settle would need
// (6): frames to drive the pinned old-rule world before declaring the pin
// reproduced, and to drive the fixed one before declaring it non-convergent.
// Both are far beyond any legitimate settle (the disk is 25 chunks and the
// mesher queue is 8 wide, so ~4 frames of real work).
#define PIN_FRAMES 150
#define CONVERGE_FRAMES 400
// (6) disk geometry: radius-2 Chebyshev disk of single-chunk columns, with
// everything at cheb == DISK_R held at lod==1 (the far band).
#define DISK_R 2
#define DISK_CX 100 // chunk coords, far from the parts (1)-(4) scratch chunks
#define DISK_CZ 100

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
					false /* no light flood: sky_light is set explicitly */);
}

static struct chunk* build_scene_at(w_coord_t X0, w_coord_t Z0, int idx) {
	const w_coord_t N = CHUNK_SIZE;
	for(w_coord_t x = X0; x < X0 + N; x++)
		for(w_coord_t z = Z0; z < Z0 + N; z++) {
			set(x, YF, z, BLOCK_STONE, 0, 15);
			if(((x - X0) + (z - Z0) + idx) % (3 + idx % 4) == 0)
				set(x, YF + 1, z, BLOCK_STONE, 0, 15);
		}
	return world_find_chunk(&w, X0 + 8, YF, Z0 + 8);
}

static bool wait_for_mesh(struct chunk* c) {
	ptime_t t0 = time_get();
	while(c->mesh_in_flight) {
		chunk_mesher_receive();
		if(c->mesh_in_flight) {
			if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS) {
				fprintf(stderr, "test_mesher_band_priority: HANG -- "
								"mesh_in_flight never cleared\n");
				return false;
			}
			thread_msleep(1);
		}
	}
	return true;
}

static uint64_t fnv1a(uint64_t h, const void* p, size_t n) {
	const uint8_t* b = (const uint8_t*)p;
	for(size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

static uint64_t mesh_fingerprint(struct chunk* c) {
	uint64_t h = 1469598103934665603ull;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		bool has = c->has_displist[k];
		h = fnv1a(h, &has, sizeof(has));
		if(!has)
			continue;
		struct displaylist* l = c->mesh + k;
		size_t stride = l->tinted ? 25u : 22u;
		uint64_t idx64 = (uint64_t)l->index;
		h = fnv1a(h, &idx64, sizeof(idx64));
		if(l->data)
			h = fnv1a(h, l->data, (size_t)l->index * stride);
	}
	h = fnv1a(h, c->reachable, sizeof(c->reachable));
	return h;
}

// Mesh standalone (chunk_mesher_send directly, uncapped -- mirrors the other
// mesher-integration tests' reference-build idiom).
static bool mesh_chunk_and_wait(struct chunk* c) {
	c->dirty_generation++;
	c->mesh_in_flight = false;
	c->rebuild_displist = true;
	ptime_t t0 = time_get();
	bool sent = false;
	while(!sent) {
		sent = chunk_mesher_send(c);
		if(!sent) {
			chunk_mesher_receive();
			thread_msleep(1);
			if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS)
				return false;
		}
	}
	c->rebuild_displist = false;
	c->first_meshed = true;
	c->mesh_in_flight = true;
	return wait_for_mesh(c);
}

static void test_urgent_residency(void) {
	struct world lifecycle;
	world_create(&lifecycle);
	for(int action = 0; action < 4; action++) {
		world_set_block(&lifecycle, 1, YF, 1,
			(struct block_data) {.type = BLOCK_STONE}, false);
		struct chunk* old = world_find_chunk(&lifecycle, 1, YF, 1);
		assert(old);
		chunk_mark_urgent(old);
		chunk_mark_urgent(old);
		assert(lifecycle.mesh_urgent_count == 1);
		/* Retain the owner just as a GPU/worker reference does. Retirement
		 * belongs to world residency, not the eventual final unref. */
		chunk_ref(old);
		if(action == 0)
			world_unload_section(&lifecycle, 0, 0);
		else if(action == 1) {
			uint8_t y[256], ids[256], tint[256] = {0};
			memset(y, YF, sizeof(y));
			memset(ids, BLOCK_STONE, sizeof(ids));
			assert(chunk_mesher_send(old));
			old->mesh_in_flight = true;
			assert(world_build_far_section(&lifecycle, 0, 0, y, ids, tint, tint));
		} else if(action == 2)
			world_unload_all(&lifecycle);
		else
			world_destroy(&lifecycle);
		assert(lifecycle.mesh_urgent_count == 0);
		assert(!old->mesh_urgent);
		if(action == 1) {
			struct chunk* fresh = world_find_chunk(&lifecycle, 1, YF, 1);
			assert(fresh && fresh != old);
			chunk_mark_urgent(fresh);
			assert(wait_for_mesh(old));
			assert(lifecycle.mesh_urgent_count == 1 && fresh->mesh_urgent);
			world_unload_section(&lifecycle, 0, 0);
			assert(lifecycle.mesh_urgent_count == 0);
		}
		chunk_unref(old);
	}
	puts("R3 PASS: urgency retired once on eviction, replacement, unload-all "
		 "and shutdown; stale worker completion preserves replacement urgency");
}

int main(void) {
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	world_create(&w);
	assert(chunk_mesher_init() && "mesher worker thread did not start");
	test_urgent_residency();

	// Two chunks, spaced two chunk-widths apart (like the enqueue-cap test)
	// so neither's mesh depends on the other's blocks via border occlusion.
	struct chunk* near_c = build_scene_at(0, 0, 0);
	struct chunk* far_c = build_scene_at(CHUNK_SIZE * 4, 0, 1);
	assert(near_c && far_c);
	far_c->lod = 1; // near_c stays lod==0 (chunk_init's default)

	// build_scene_at's world_set_block calls go through chunk_set_block,
	// which marks the EDITED chunk mesh_urgent (H-2 band 0 -- see chunk.c's
	// chunk_mark_urgent call there). That is correct production behaviour
	// (a real edit deserves priority), but it means both scratch chunks come
	// out of scene construction already band-0 urgent, which would let band 0
	// legitimately bypass the far-starvation gate under test below and mask
	// what this test is trying to isolate. Settle that construction-time
	// urgency here, mirroring what world_build_chunks's urgent pass would
	// have done had this test driven it during construction, before this
	// test starts marking things urgent ON PURPOSE for part (3).
	if(near_c->mesh_urgent) {
		near_c->mesh_urgent = false;
		w.mesh_urgent_count--;
	}
	if(far_c->mesh_urgent) {
		far_c->mesh_urgent = false;
		w.mesh_urgent_count--;
	}

	// --- Reference builds: mesh each ALONE (no gating at all) and record
	// fingerprints -- the "byte-identical" baseline.
	assert(mesh_chunk_and_wait(near_c));
	uint64_t near_ref_fp = mesh_fingerprint(near_c);
	assert(mesh_chunk_and_wait(far_c));
	uint64_t far_ref_fp = mesh_fingerprint(far_c);
	printf("  reference builds: near + far chunks meshed alone, fingerprints "
		   "recorded\n");

	// --- (1) Strict far starvation: re-dirty both, then drive
	// world_build_chunks with allow_far=false for many frames. far_c must
	// NEVER become mesh_in_flight or gain a display list while near_c is
	// still unsettled.
	near_c->dirty_generation++;
	near_c->mesh_in_flight = false;
	near_c->rebuild_displist = true;
	// first_meshed stays true from the reference build above -- a re-dirty
	// (real neighbour/edit churn) meshes immediately per chunk_check_built's
	// B2 comment, so this test isn't also exercising the neighbour-wait gate.
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		if(far_c->has_displist[k]) {
			displaylist_destroy(far_c->mesh + k);
			far_c->has_displist[k] = false;
		}
	far_c->dirty_generation++;
	far_c->mesh_in_flight = false;
	far_c->rebuild_displist = true;

	ptime_t t0 = time_get();
	while(near_c->rebuild_displist || near_c->mesh_in_flight) {
		world_build_chunks(&w, CHUNK_MESHER_QLENGTH, false);
		chunk_mesher_receive();
		assert(!far_c->mesh_in_flight
			   && "far (lod==1) chunk was submitted to the mesher while the "
				  "near ring was still unmeshed -- band 2 must be fully "
				  "starved by band 1, not merely deprioritized");
		assert(!far_c->has_displist[0]
			   && !far_c->has_displist[CHUNK_MESH_PARTS]
			   && "far chunk gained mesh output while starved");
		if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS) {
			fprintf(stderr, "test_mesher_band_priority: HANG -- near_c never "
							"settled under world_build_chunks\n");
			return 1;
		}
		thread_msleep(1);
	}
	printf("  (1) far chunk fully starved while the near chunk was unmeshed "
		   "(checked every frame)\n");

	// --- (2) Far proceeds once near settles: allow_far=true now converges
	// far_c within a bounded number of frames.
	bool far_done = false;
	for(int frame = 0; frame < STARVE_FRAMES && !far_done; frame++) {
		world_build_chunks(&w, CHUNK_MESHER_QLENGTH, true);
		chunk_mesher_receive();
		if(!far_c->rebuild_displist && !far_c->mesh_in_flight)
			far_done = true;
		else
			thread_msleep(1);
	}
	assert(far_done && "far chunk never settled once allow_far=true");
	printf("  (2) far chunk meshed once allow_far=true was passed\n");

	// --- (4) byte-identical.
	assert(mesh_fingerprint(near_c) == near_ref_fp
		   && "near chunk's gated mesh diverged from its ungated reference");
	assert(mesh_fingerprint(far_c) == far_ref_fp
		   && "far chunk's starved-then-released mesh diverged from its "
			  "ungated reference -- H-2 must be pure ordering, never a "
			  "content change");
	printf("  (4) both chunks' gated mesh fingerprints match their ungated "
		   "references exactly\n");

	// --- (3) Band 0 jumps the queue. Force the enqueue cap to 1 (test-only
	// seam) so exactly one of two equally-dirty near chunks can be sent this
	// call; mark ONE of them urgent and confirm IT is the one chosen.
	chunk_mesher_test_set_enqueue_cap(1);

	near_c->dirty_generation++;
	near_c->mesh_in_flight = false;
	near_c->rebuild_displist = true;
	near_c->lod = 0;
	far_c->lod = 0; // repurpose far_c as a second ordinary NEAR chunk here
	far_c->dirty_generation++;
	far_c->mesh_in_flight = false;
	far_c->rebuild_displist = true;

	chunk_mark_urgent(near_c); // near_c is the band-0 chunk; far_c is not

	size_t tokens_left = world_build_chunks(&w, 1, true);
	assert(tokens_left == 0
		   && "the single available token should have been spent on the "
			  "urgent chunk");
	assert(near_c->mesh_in_flight
		   && "the URGENT chunk must be the one that gets the scarce token");
	assert(!far_c->mesh_in_flight
		   && far_c->rebuild_displist
		   && "the non-urgent chunk must still be waiting -- band 0 jumped "
			  "ahead of it, not merely tied with it");
	printf("  (3) with a single available mesher token, the urgent (band 0) "
		   "chunk was sent ahead of an equally-dirty ordinary near chunk\n");

	// --- (5) The admission rule itself, asserted on the pure function so the
	// bound is proved rather than inferred from timing.
	assert(world_band2_allow_far(0, 1)
		   && "band 2 must be admitted when the near ring is fully settled");
	assert(world_band2_allow_far(WORLD_BAND2_NEAR_PENDING_SLACK, 1)
		   && "band 2 must be admitted while the near backlog still fits "
			  "inside one mesher queue -- those tokens are free");
	{
		// Beyond the slack, some frame must still deny -- otherwise band 1
		// has lost its priority entirely.
		bool found_deny = false;
		for(uint32_t f = 1; f <= (uint32_t)WORLD_BAND2_STARVE_RELEASE_FRAMES;
			f++)
			if(!world_band2_allow_far(WORLD_BAND2_NEAR_PENDING_SLACK + 1, f)) {
				found_deny = true;
				break;
			}
		assert(found_deny
			   && "the slack bound must still deny SOME frame -- otherwise "
				  "band 1 has lost its priority entirely");
		// ...but no window of RELEASE_FRAMES consecutive frames may deny
		// EVERY frame, at ANY backlog size. This is the anti-starvation
		// guarantee; it is exactly what the old `pending == 0` rule lacked.
		for(uint32_t start = 0;
			start < 4u * (uint32_t)WORLD_BAND2_STARVE_RELEASE_FRAMES; start++) {
			bool admitted = false;
			for(int k = 0; k < WORLD_BAND2_STARVE_RELEASE_FRAMES; k++)
				if(world_band2_allow_far(1000000, start + (uint32_t)k)) {
					admitted = true;
					break;
				}
			assert(admitted
				   && "band 2 was denied for a whole release period at a huge "
					  "near backlog -- indefinite starvation is reachable "
					  "again");
		}
	}
	printf("  (5) world_band2_allow_far: admits within the slack, and admits "
		   "at least once every %d frames at ANY near backlog\n",
		   WORLD_BAND2_STARVE_RELEASE_FRAMES);

	// --- (6) RCA: the old rule pins on the shipped reach<MAX_VIEW_DISTANCE
	// geometry; the shipped rule converges on the identical world.
	{
		struct chunk* disk[(2 * DISK_R + 1) * (2 * DISK_R + 1)];
		int n_disk = 0, n_far = 0;
		for(int dz = -DISK_R; dz <= DISK_R; dz++)
			for(int dx = -DISK_R; dx <= DISK_R; dx++) {
				int adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
				int cheb = adx > adz ? adx : adz;
				struct chunk* c
					= build_scene_at((w_coord_t)(DISK_CX + dx) * CHUNK_SIZE,
									 (w_coord_t)(DISK_CZ + dz) * CHUNK_SIZE,
									 (adx + adz) % 3);
				assert(c);
				// The reach dial: only the inner core is full-detail. This is
				// near_radius=1 against a scan radius of 2 -- the same shape
				// as the Wii default near_radius=2 vs MAX_VIEW_DISTANCE=5.
				c->lod = (cheb >= DISK_R) ? 1 : 0;
				if(c->lod == 1)
					n_far++;
				// Settle the construction-time band-0 urgency exactly as the
				// parts (1)-(4) scratch chunks did above, so band 0 is not
				// what is under test here.
				if(c->mesh_urgent) {
					c->mesh_urgent = false;
					w.mesh_urgent_count--;
				}
				disk[n_disk++] = c;
			}
		assert(n_disk == (2 * DISK_R + 1) * (2 * DISK_R + 1));

		// PHASE A -- the OLD rule, reproduced exactly: pending counted over
		// every resident chunk in the disk regardless of lod, allow_far =
		// (that count == 0).
		int legacy_pending = 0;
		for(int frame = 0; frame < PIN_FRAMES; frame++) {
			legacy_pending = 0;
			for(int i = 0; i < n_disk; i++)
				if(disk[i]->rebuild_displist || disk[i]->mesh_in_flight)
					legacy_pending++;
			world_build_chunks(&w, CHUNK_MESHER_QLENGTH, legacy_pending == 0);
			chunk_mesher_receive();
			thread_msleep(1);
		}
		legacy_pending = 0;
		for(int i = 0; i < n_disk; i++)
			if(disk[i]->rebuild_displist || disk[i]->mesh_in_flight)
				legacy_pending++;
		assert(legacy_pending == n_far
			   && "the old rule should pin at EXACTLY the far shell: the near "
				  "core meshes, the far shell is refused, and the refused "
				  "chunks are what keeps the count off zero");
		for(int i = 0; i < n_disk; i++)
			if(disk[i]->lod == 1)
				assert(!disk[i]->has_displist[0]
					   && !disk[i]->has_displist[CHUNK_MESH_PARTS]
					   && "a far chunk meshed under the old rule -- the pin "
						  "did not reproduce, so this test no longer proves "
						  "anything about the fix");
		// The counter fix, stated as an assertion: the SHIPPED near-ring
		// pending term reads 0 on this exact world state, because the only
		// outstanding work is band 2's own -- which is the whole point.
		{
			int miss = 0, pend = 0;
			world_near_ring_status(&w, DISK_CX, DISK_CZ, DISK_R, &miss, &pend);
			assert(miss == 0 && "the disk is fully resident");
			assert(pend == 0
				   && "world_near_ring_status must NOT count band-2 (lod==1) "
					  "chunks as near mesh-pending -- that is the self-"
					  "referential count that pinned the gate on hardware");
		}
		printf("  (6a) old rule PINNED: %d far chunks unmeshed after %d "
			   "frames, legacy pending stuck at %d, shipped near pending = 0\n",
			   n_far, PIN_FRAMES, legacy_pending);

		// PHASE B -- the shipped rule on the identical world state. Also
		// catches a far chunk being admitted while near pending is nonzero
		// (impossible under the old all-or-nothing edge): re-dirty a few
		// near-core chunks first so the near backlog is small-but-nonzero.
		int redirtied = 0;
		for(int i = 0; i < n_disk && redirtied < 3; i++)
			if(disk[i]->lod == 0) {
				disk[i]->dirty_generation++;
				disk[i]->rebuild_displist = true;
				redirtied++;
			}
		assert(redirtied == 3);

		bool far_admitted_while_near_pending = false;
		bool converged = false;
		for(int frame = 0; frame < CONVERGE_FRAMES && !converged; frame++) {
			int pend = 0;
			world_near_ring_status(&w, DISK_CX, DISK_CZ, DISK_R, NULL, &pend);
			world_build_chunks(&w, CHUNK_MESHER_QLENGTH,
							   world_band2_allow_far(pend, w.build_frame));
			// Sampled immediately after the build pass, before any result is
			// integrated: a far chunk in flight while the near term is still
			// nonzero is band 2 running CONCURRENTLY with band 1.
			int pend_after = 0;
			world_near_ring_status(&w, DISK_CX, DISK_CZ, DISK_R, NULL,
								   &pend_after);
			if(pend_after > 0)
				for(int i = 0; i < n_disk; i++)
					if(disk[i]->lod == 1 && disk[i]->mesh_in_flight)
						far_admitted_while_near_pending = true;
			chunk_mesher_receive();
			converged = true;
			for(int i = 0; i < n_disk; i++)
				if(disk[i]->rebuild_displist || disk[i]->mesh_in_flight)
					converged = false;
			if(!converged)
				thread_msleep(1);
		}
		assert(converged
			   && "the bounded rule must drain the whole disk -- near core AND "
				  "far shell -- on a quiescent world");
		assert(far_admitted_while_near_pending
			   && "no far chunk was ever built while the near pending count "
				  "was nonzero -- the gate is still all-or-nothing");
		{
			int miss = 0, pend = 0;
			world_near_ring_status(&w, DISK_CX, DISK_CZ, DISK_R, &miss, &pend);
			assert(miss == 0 && pend == 0
				   && "near-ring status must converge to 0/0 on a quiescent "
					  "world");
		}
		for(int i = 0; i < n_disk; i++)
			assert((disk[i]->has_displist[0]
					|| disk[i]->has_displist[CHUNK_MESH_PARTS])
				   && "every chunk in the disk -- far shell included -- must "
					  "have real mesh output once the world is quiescent");
		printf("  (6b) bounded rule CONVERGED: all %d chunks meshed, near "
			   "pending -> 0, and band 2 was admitted while band 1 still had "
			   "work outstanding\n",
			   n_disk);
	}

	// Drain both to a clean shutdown.
	chunk_mesher_test_set_enqueue_cap(-1);
	ptime_t t_drain = time_get();
	while(near_c->mesh_in_flight || far_c->mesh_in_flight
		  || near_c->rebuild_displist || far_c->rebuild_displist) {
		world_build_chunks(&w, CHUNK_MESHER_QLENGTH, true);
		chunk_mesher_receive();
		assert(time_diff_ms(t_drain, time_get()) < HANG_TIMEOUT_MS
			   && "final drain never converged");
		thread_msleep(1);
	}

	chunk_mesher_shutdown();
	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);

	printf("test_mesher_band_priority: OK -- band 2 (far) is starved while "
		   "band 1 (near) is unmeshed and allow_far is false, resumes once "
		   "released, band 0 (urgent) chunks jump ahead of ordinary band-1 "
		   "chunks under scarce tokens, every mesh produced under gating is "
		   "byte-identical to the same chunk meshed with no gating at all, "
		   "and the SHIPPED admission rule is bounded: it converges (and "
		   "admits band 2 alongside a nonzero near backlog) on the exact "
		   "reach-inside-scan-radius geometry where the old `pending == 0` "
		   "rule pins forever.\n");
	return 0;
}
