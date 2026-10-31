/*
	P2-b / T1-e (2026-09-03 round-2 far-ring frame-drop root-cause doc, "#2
	Post-load spikes") golden proof: the cooperative slice added to
	chunk_mesher_build (thread_msleep(1) after each finalized list once the
	build has run past CHUNK_MESHER_SLICE_MS) is pure PACING -- the exact same
	chunk mesh, byte-for-byte, regardless of whether the slice actually fires
	mid-build.

	Drives the REAL production path (chunk_mesher_init -> chunk_mesher_send on
	a real chunk in a real world -> the real worker -> chunk_mesher_receive),
	same harness shape as test_chunk_mesher_enqueue_cap_env.c /
	test_chunk_mesher_shutdown_env.c.

	A real chunk on PC meshes in well under the 2 ms default threshold, so
	comparing "default on vs off" would never actually exercise the slice.
	Instead this uses the CAVEWII_DISPLAYLIST_TEST-gated
	chunk_mesher_test_set_slice_ms() seam to force the slice NEVER to fire
	(a huge threshold: reference build) and then to force it to fire after
	EVERY finalized list (threshold 0: sliced build), and asserts the two
	builds of the SAME chunk data produce identical finalized mesh bytes.
*/

#include <assert.h>
#include <limits.h>
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

// A varied floor + partial upper layer + a scattering of a second block type
// so every one of CHUNK_MESH_LISTS' 26 buckets (opaque/transparent x
// untinted/tinted x 13 side/double buckets) has a real shot at being
// exercised, not just the trivial all-air/all-stone cases.
static struct chunk* build_scene(w_coord_t X0, w_coord_t Z0) {
	const w_coord_t N = CHUNK_SIZE;
	for(w_coord_t x = X0; x < X0 + N; x++)
		for(w_coord_t z = Z0; z < Z0 + N; z++) {
			set(x, YF, z, BLOCK_STONE, 0, 15);
			if(((x - X0) + (z - Z0)) % 3 == 0)
				set(x, YF + 1, z, BLOCK_STONE, 0, 15);
			if(((x - X0) * 2 + (z - Z0)) % 5 == 0)
				set(x, YF + 2, z, BLOCK_GLASS, 0, 15);
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
						"test_chunk_mesher_slice_env: HANG -- mesh_in_flight "
						"never cleared within %d ms\n",
						HANG_TIMEOUT_MS);
				return false;
			}
			thread_msleep(1);
		}
	}
	return true;
}

// FNV-1a over every finalized list's raw byte content plus vertex count and
// the reachable[] mask -- same fingerprint shape as
// test_chunk_mesher_enqueue_cap_env.c's mesh_fingerprint.
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

	struct chunk* c = build_scene(0, 0);
	assert(c);

	// --- Reference build: force the slice threshold impossibly high so it
	// NEVER fires (reproduces today's un-sliced behaviour exactly).
	chunk_mesher_test_set_slice_ms(INT_MAX);
	assert(mesh_chunk_and_wait(c));
	uint64_t reference_fp = mesh_fingerprint(c);
	printf("  reference build (slice never fires): fingerprint recorded\n");

	// --- Sliced build: force the threshold to 0 so the cooperative sleep
	// fires after EVERY one of CHUNK_MESH_LISTS finalized lists (a much
	// heavier slice rate than production's 2 ms ever produces for a single
	// 16^3 chunk -- if slicing could ever perturb output, this is the
	// build that would prove it).
	chunk_mesher_test_set_slice_ms(0);
	assert(mesh_chunk_and_wait(c));
	uint64_t sliced_fp = mesh_fingerprint(c);
	printf("  sliced build (slice fires after every finalized list): "
		   "fingerprint recorded\n");

	assert(sliced_fp == reference_fp
		   && "the cooperative mesher slice changed the finalized mesh bytes "
			  "-- it must be pure pacing (thread_msleep between already-"
			  "committed list finalizes), never a content change");
	printf("  sliced build's mesh fingerprint matches the un-sliced "
		   "reference exactly\n");

	// Restore the default (production) behaviour before tearing down, in
	// case a future test links against the same pooled engine object in the
	// same process and relies on the real CHUNK_MESHER_SLICE_MS.
	chunk_mesher_test_set_slice_ms(-1);

	chunk_mesher_shutdown();
	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);

	printf("test_chunk_mesher_slice_env: OK -- the CHUNK_MESHER_SLICE_MS "
		   "cooperative slice is pure pacing; a build sliced after every "
		   "finalized list produces byte-identical mesh output to the same "
		   "chunk built with the slice never firing.\n");
	return 0;
}
