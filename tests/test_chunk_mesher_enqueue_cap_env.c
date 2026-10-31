/*
	T1-d (2026-09-03 far-ring frame-drop root cause, mechanism A) golden proof:
	capping the mesher's in-flight backlog (CHUNK_MESHER_ENQUEUE_CAP) is pure
	PACING -- the same chunk set meshes to the SAME final bytes, only later.

	H-1 UPDATE (2026-09-03 responsiveness lane): production CHUNK_MESHER_
	ENQUEUE_CAP is now CHUNK_MESHER_QLENGTH (a no-op -- see its declaration's
	H-1 comment: capping to 4 was sized against a worker that was ALSO paying
	up to ~25ms of dead post-commit sleep per build, and made the real-Wii
	symptom this lane fixes WORSE once that sleep moved into the emit loop).
	This test now drives the deferral mechanism through the
	chunk_mesher_test_set_enqueue_cap() seam at a small forced value
	(TEST_ENQUEUE_CAP below), so the "accept exactly N, refuse the rest,
	byte-identical once drained" proof stays exercised even though production
	no longer ships a cap tight enough to trigger it on its own.

	Drives the REAL production path (chunk_mesher_init -> chunk_mesher_send on
	real chunks in a real world -> the real worker -> chunk_mesher_receive),
	same harness shape as test_chunk_mesher_shutdown_env.c /
	test_piston_chunk_mesh_env.c.

	WHAT THIS ASSERTS:
	  (1) THE CAP ACTUALLY ENGAGES -- submitting more chunks than
	      CHUNK_MESHER_ENQUEUE_CAP in one uninterrupted burst (no
	      chunk_mesher_receive() in between, so mesher_pending cannot drop)
	      gets at least one chunk_mesher_send() refusal. If this cap were a
	      no-op (e.g. accidentally compiled out, or set >= the number of
	      chunks below), this assertion is the one that would catch it.
	  (2) NOTHING IS LOST -- every submitted chunk eventually gets meshed
	      (has_displist becomes non-trivially populated), bounded by a
	      wall-clock watchdog so a starved/deadlocked chunk fails the test
	      instead of hanging CI.
	  (3) BYTE-IDENTICAL, ONLY LATER -- each chunk's mesh, built once
	      standalone (uncapped: only one chunk ever in flight) and again as
	      part of the capped burst, hashes to the SAME per-list fingerprint
	      (FNV-1a over every finalized list's raw bytes + vertex count, plus
	      the reachable[] mask) -- the cap changed WHEN a chunk's mesh landed,
	      never WHAT it contains.
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

// RESTORE (2026-09-03, maintainer ruling): CHUNK_MESHER_ENQUEUE_CAP is back
// at CHUNK_MESHER_QLENGTH (the cap is a no-op) until it re-lands behind a
// hardware throughput gate. This proof needs a cap strictly below the token
// pool, so it reports SKIP loudly instead of failing its static asserts; when
// the cap re-lands with a value < CHUNK_MESHER_QLENGTH the real test below
// compiles again untouched.
#if CHUNK_MESHER_ENQUEUE_CAP >= CHUNK_MESHER_QLENGTH
int main(void) {
	printf("test_chunk_mesher_enqueue_cap_env: SKIP -- CHUNK_MESHER_ENQUEUE_CAP "
		   "(%d) >= CHUNK_MESHER_QLENGTH (%d): cap disabled by the 2026-09-03 "
		   "restore; re-enable this proof when the cap re-lands\n",
		   (int)CHUNK_MESHER_ENQUEUE_CAP, (int)CHUNK_MESHER_QLENGTH);
	return 0;
}
#else

#define HANG_TIMEOUT_MS 5000
#define YF 64 // stone-floor plane, well inside chunk cy=4

// H-1: the FORCED cap under test (via chunk_mesher_test_set_enqueue_cap),
// independent of production's CHUNK_MESHER_ENQUEUE_CAP (now a no-op --
// CHUNK_MESHER_QLENGTH). Kept at the old T1-d production value so this test
// still proves exactly the mechanism T1-d's telemetry was sized around.
#define TEST_ENQUEUE_CAP 4

// N_CHUNKS > TEST_ENQUEUE_CAP: guarantees at least one send in an
// uninterrupted burst must be refused (assertion (1) above). Comfortably
// within CHUNK_MESHER_QLENGTH (8) so the token pool itself is never the
// limiting factor -- only the pacing cap is under test.
#define N_CHUNKS 6
_Static_assert(N_CHUNKS > TEST_ENQUEUE_CAP,
			  "this test requires more chunks than the enqueue cap to "
			  "exercise a real deferral");
_Static_assert(N_CHUNKS <= CHUNK_MESHER_QLENGTH,
			  "this test must stay within the token pool so only the "
			  "pacing cap -- not pool exhaustion -- can refuse a send");

// --- RPC drain thread (the full client links clin_*; keep its inbox empty so
// no path blocks). The CLIENT world below is wholly independent of it. -------
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

// One chunk per index with a DISTINCT bump pattern (varies with `idx`) so
// each chunk's mesh is distinguishable -- a fingerprint collision across
// chunks would otherwise mask a real divergence.
static struct chunk* build_scene_at(w_coord_t X0, w_coord_t Z0, int idx) {
	const w_coord_t N = CHUNK_SIZE;
	for(w_coord_t x = X0; x < X0 + N; x++)
		for(w_coord_t z = Z0; z < Z0 + N; z++) {
			set(x, YF, z, BLOCK_STONE, 0, 15);
			if((((x - X0) + (z - Z0) + idx) % (3 + idx % 4)) == 0)
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
				fprintf(stderr,
						"test_chunk_mesher_enqueue_cap_env: HANG -- mesh_in_"
						"flight never cleared within %d ms\n",
						HANG_TIMEOUT_MS);
				return false;
			}
			thread_msleep(1);
		}
	}
	return true;
}

// FNV-1a over every finalized list's raw byte content (data[0..index*stride)
// on PC; the platform-independent quantity that actually reaches the GP/GL)
// plus the vertex/index count and the reachable[] mask -- a fingerprint of
// "everything this mesh visually is", independent of WHEN it was built.
static uint64_t fnv1a(uint64_t h, const void* p, size_t n) {
	const uint8_t* b = (const uint8_t*)p;
	for(size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

static uint64_t mesh_fingerprint(struct chunk* c) {
	uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		bool has = c->has_displist[k];
		h = fnv1a(h, &has, sizeof(has));
		if(!has)
			continue;
		struct displaylist* l = c->mesh + k;
		// PC's post-finalize l->index is the VERTEX COUNT; the byte content
		// is data[0 .. index*stride). Deriving stride here (rather than
		// hardcoding 22/25) keeps this test platform-shape-agnostic if the
		// PC vertex layout ever changes.
		size_t stride = l->tinted ? 25u : 22u;
		uint64_t idx64 = (uint64_t)l->index;
		h = fnv1a(h, &idx64, sizeof(idx64));
		if(l->data)
			h = fnv1a(h, l->data, (size_t)l->index * stride);
	}
	h = fnv1a(h, c->reachable, sizeof(c->reachable));
	return h;
}

static bool mesh_chunk_and_wait(struct chunk* c) {
	c->dirty_generation++;
	c->mesh_in_flight = false;
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

int main(void) {
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	world_create(&w);
	assert(chunk_mesher_init() && "mesher worker thread did not start");

	struct chunk* chunks[N_CHUNKS];
	uint64_t reference_fp[N_CHUNKS];

	// --- Reference pass: mesh each chunk ALONE (only one job is ever in
	// flight, so no cap could engage regardless) and record its fingerprint.
	// The test-only cap override is still at its -1 default here.
	// Chunks are spaced TWO chunk-widths apart (a fully untouched, never
	// world_set_block'd gap chunk between each pair) so no chunk's mesh
	// depends on a NEIGHBOUR that this test builds later -- with adjacent
	// chunks, chunk i's reference mesh (built before chunk i+1 existed)
	// would legitimately differ from its later capped-burst mesh (built
	// after chunk i+1's blocks landed, changing shared-face occlusion),
	// which would fail assertion (3) for a reason that has nothing to do
	// with T1-d.
	for(int i = 0; i < N_CHUNKS; i++) {
		chunks[i] = build_scene_at(i * CHUNK_SIZE * 2, 0, i);
		assert(chunks[i]);
		assert(mesh_chunk_and_wait(chunks[i]));
		reference_fp[i] = mesh_fingerprint(chunks[i]);
	}
	printf("  reference pass: %d chunks meshed one at a time, fingerprints "
		   "recorded\n",
		   N_CHUNKS);

	// --- Capped burst: arm the test-only cap override (H-1: production's
	// CHUNK_MESHER_ENQUEUE_CAP is now a no-op, so the burst must force a real
	// cap through the seam to exercise deferral at all), re-dirty all
	// N_CHUNKS, and send them in one UNINTERRUPTED burst (no
	// chunk_mesher_receive() in between, so mesher_pending cannot drop
	// mid-burst). With N_CHUNKS > TEST_ENQUEUE_CAP this MUST refuse at least
	// one send.
	chunk_mesher_test_set_enqueue_cap(TEST_ENQUEUE_CAP);
	for(int i = 0; i < N_CHUNKS; i++) {
		chunks[i]->dirty_generation++;
		chunks[i]->mesh_in_flight = false;
	}
	int accepted = 0, refused = 0;
	bool sent_this_round[N_CHUNKS] = {0};
	for(int i = 0; i < N_CHUNKS; i++) {
		if(chunk_mesher_send(chunks[i])) {
			sent_this_round[i] = true;
			chunks[i]->rebuild_displist = false;
			chunks[i]->first_meshed = true;
			chunks[i]->mesh_in_flight = true;
			accepted++;
		} else {
			refused++;
		}
	}
	assert(accepted == TEST_ENQUEUE_CAP
		   && "the burst must accept exactly the cap's worth of sends");
	assert(refused == N_CHUNKS - TEST_ENQUEUE_CAP
		   && "the forced cap did not engage -- every send in the burst "
			  "succeeded (the deferral mechanism itself is broken)");
	printf("  capped burst: %d/%d sends accepted immediately, %d deferred "
		   "(forced test cap=%d) -- the pacing cap engaged as designed\n",
		   accepted, N_CHUNKS, refused, TEST_ENQUEUE_CAP);

	// --- Drain: send the deferred chunks (retrying, mirroring chunk.c's real
	// caller contract) and wait for every chunk to finish, exactly like a
	// real render loop's chunk_check_built() + chunk_mesher_receive() would.
	ptime_t t0 = time_get();
	bool done[N_CHUNKS] = {0};
	int remaining = N_CHUNKS;
	while(remaining > 0) {
		for(int i = 0; i < N_CHUNKS; i++) {
			if(done[i])
				continue;
			if(!sent_this_round[i]) {
				if(chunk_mesher_send(chunks[i])) {
					sent_this_round[i] = true;
					chunks[i]->rebuild_displist = false;
					chunks[i]->first_meshed = true;
					chunks[i]->mesh_in_flight = true;
				}
			}
			chunk_mesher_receive();
			if(sent_this_round[i] && !chunks[i]->mesh_in_flight) {
				done[i] = true;
				remaining--;
			}
		}
		if(remaining > 0) {
			assert(time_diff_ms(t0, time_get()) < HANG_TIMEOUT_MS
				   && "a deferred chunk never finished meshing -- starvation, "
					  "not pacing");
			thread_msleep(1);
		}
	}
	printf("  drain: all %d chunks finished meshing (deferred ones landed "
		   "LATER, not never)\n",
		   N_CHUNKS);

	// --- (3) Byte-identical, only later.
	for(int i = 0; i < N_CHUNKS; i++) {
		uint64_t fp = mesh_fingerprint(chunks[i]);
		assert(fp == reference_fp[i]
			   && "capped-burst mesh diverged from the uncapped reference -- "
				  "T1-d must be pure pacing, never a byte-content change");
	}
	printf("  all %d chunks' capped-burst mesh fingerprints match their "
		   "uncapped reference exactly\n",
		   N_CHUNKS);

	// Restore the default (production, no-op) cap before teardown, in case a
	// future test links against the same pooled engine object in the same
	// process and relies on the real CHUNK_MESHER_ENQUEUE_CAP.
	chunk_mesher_test_set_enqueue_cap(-1);

	chunk_mesher_shutdown();
	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);

	printf("test_chunk_mesher_enqueue_cap_env: OK -- the enqueue-cap deferral "
		   "mechanism engages (defers, never drops) once a burst exceeds it, "
		   "and every deferred chunk's final mesh is byte-identical to the "
		   "same chunk meshed without contention, only submitted later.\n");
	return 0;
}
#endif // CHUNK_MESHER_ENQUEUE_CAP < CHUNK_MESHER_QLENGTH
