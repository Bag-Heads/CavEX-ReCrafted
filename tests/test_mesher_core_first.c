/*
	REL-MP-COREFIRST (2026-10-22) scheduling proof for world_build_chunks'
	remote-loading focus band (world_set_build_focus). Pure render SCHEDULING
	ORDER (T1): the same chunks mesh to the same bytes, only the order changes,
	and only while main.c enables the focus -- a REMOTE session's loading
	screen (maintainer scope 10-22: local loading and in-game order unchanged).

	Two worlds, identical content (16x16 single-chunk-scene columns):
	  wa -- focus never enabled: the LOCAL / in-game path.
	  wb -- focus enabled on a centre whose 3x3 core sits LATE in hash order.
	Driven in lockstep through the REAL async mesher (same harness as
	test_mesher_band_priority.c).

	  (L) LOCAL ORDER UNCHANGED: with the focus off (never set, and explicitly
	      set disabled), one send per frame (enqueue cap 1), the send sequence
	      is EXACTLY the dict (hash) walk order -- the pre-COREFIRST definition
	      of world_build_chunks with an empty render list and no urgent work --
	      and world_build_focus_wanted is false for every non-remote input.
	  (O) REMOTE ORDER: with the focus on, the send sequence is the focus disk
	      ring by ring (ring 0, then ring 1 = the 3x3 safe core, then rings
	      2..R), then the rest in that same hash order.
	  (S) STORM: every frame 16 non-core chunks are really edited (urgent-
	      marked, chunk_set_block) -- the server settle storm. With the full
	      production budget (8 tokens, queue 8) the focus world meshes its
	      core within ceil(core/8)+1 frames; the local world, whose core hashes
	      late, has not meshed it after STORM_FRAMES frames (the measured
	      18-36 s Wii join hold, in miniature).
	  (E) SAME OUTPUT: after the storm stops and both drain, every chunk's
	      mesh fingerprint is identical across the two worlds AND identical to
	      the same chunk re-meshed alone with no scheduling at all.
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

#define HANG_TIMEOUT_MS 10000
#define YF 64
#define GRID 16 /* columns per side, chunk coords [-8, 7] */
#define GMIN (-(GRID / 2))
#define FOCUS_R 5
#define MAX_CHUNKS (GRID * GRID * COLUMN_HEIGHT)
#define STORM_EDITS 16
#define STORM_FRAMES 20

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

static struct world wa, wb;

static void set(struct world* w, w_coord_t x, w_coord_t y, w_coord_t z,
				int type) {
	world_set_block(w, x, y, z,
					(struct block_data) {.type = (uint8_t)type,
										 .metadata = 0,
										 .sky_light = 15,
										 .torch_light = 0},
					false);
}

static void build_scene(struct world* w, int cx, int cz) {
	w_coord_t X0 = (w_coord_t)cx * CHUNK_SIZE, Z0 = (w_coord_t)cz * CHUNK_SIZE;
	int idx = ((cx - GMIN) * 7 + (cz - GMIN) * 3) % 11;
	for(w_coord_t x = X0; x < X0 + CHUNK_SIZE; x++)
		for(w_coord_t z = Z0; z < Z0 + CHUNK_SIZE; z++) {
			set(w, x, YF, z, BLOCK_STONE);
			if(((x - X0) + (z - Z0) + idx) % (3 + idx % 4) == 0)
				set(w, x, YF + 1, z, BLOCK_STONE);
		}
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

/* Every resident chunk of w in DICT order (k ascending inside a column):
 * exactly the order world_build_chunks' final w->sections walk visits. */
static size_t dict_order(struct world* w, struct chunk** out) {
	size_t n = 0;
	dict_wsection_it_t it;
	dict_wsection_it(it, w->sections);
	while(!dict_wsection_end_p(it)) {
		struct world_section* s = &dict_wsection_ref(it)->value;
		for(size_t k = 0; k < COLUMN_HEIGHT; k++)
			if(s->column[k]) {
				assert(n < MAX_CHUNKS);
				out[n++] = s->column[k];
			}
		dict_wsection_next(it);
	}
	return n;
}

static int ccx(const struct chunk* c) { return (int)(c->x / CHUNK_SIZE); }
static int ccz(const struct chunk* c) { return (int)(c->z / CHUNK_SIZE); }
static int cheb(const struct chunk* c, int cx, int cz) {
	int dx = ccx(c) - cx, dz = ccz(c) - cz;
	if(dx < 0) dx = -dx;
	if(dz < 0) dz = -dz;
	return dx > dz ? dx : dz;
}

static bool any_in_flight(struct chunk** all, size_t n) {
	for(size_t i = 0; i < n; i++)
		if(all[i]->mesh_in_flight)
			return true;
	return false;
}

/* Integrate every outstanding result for both worlds. */
static void settle_in_flight(struct chunk** a, size_t na, struct chunk** b,
							 size_t nb) {
	ptime_t t0 = time_get();
	while(any_in_flight(a, na) || any_in_flight(b, nb)) {
		chunk_mesher_receive();
		assert(time_diff_ms(t0, time_get()) < HANG_TIMEOUT_MS
			   && "mesher never returned an in-flight chunk");
		thread_msleep(1);
	}
}

static bool all_clean(struct chunk** all, size_t n) {
	for(size_t i = 0; i < n; i++)
		if(all[i]->rebuild_displist || all[i]->mesh_in_flight)
			return false;
	return true;
}

static void drain(struct world* w, struct chunk** all, size_t n) {
	ptime_t t0 = time_get();
	while(!all_clean(all, n) || w->mesh_urgent_count) {
		world_build_chunks(w, CHUNK_MESHER_QLENGTH, true);
		chunk_mesher_receive();
		assert(time_diff_ms(t0, time_get()) < HANG_TIMEOUT_MS
			   && "drain never converged");
		thread_msleep(1);
	}
}

static void dirty_all(struct chunk** all, size_t n) {
	for(size_t i = 0; i < n; i++) {
		all[i]->dirty_generation++;
		all[i]->rebuild_displist = true;
	}
}

/* One send per frame (enqueue cap 1, nothing in flight at frame start):
 * returns the exact send sequence. */
static size_t record_sequence(struct world* w, struct chunk** all, size_t n,
							  struct chunk** seq) {
	size_t len = 0;
	ptime_t t0 = time_get();
	while(!all_clean(all, n)) {
		world_build_chunks(w, CHUNK_MESHER_QLENGTH, true);
		struct chunk* sent = NULL;
		for(size_t i = 0; i < n; i++)
			if(all[i]->mesh_in_flight) {
				assert(!sent && "more than one send under enqueue cap 1");
				sent = all[i];
			}
		assert(sent && "a dirty world sent nothing this frame");
		assert(len < MAX_CHUNKS);
		seq[len++] = sent;
		settle_in_flight(all, n, NULL, 0);
		assert(time_diff_ms(t0, time_get()) < 4 * HANG_TIMEOUT_MS);
	}
	return len;
}

static struct chunk* all_a[MAX_CHUNKS];
static struct chunk* all_b[MAX_CHUNKS];
static struct chunk* seq_a[MAX_CHUNKS];
static struct chunk* seq_b[MAX_CHUNKS];
static struct chunk* expect[MAX_CHUNKS];
static uint64_t fp_ref[MAX_CHUNKS];

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	clin_init();
	struct thread drain_thread;
	thread_create(&drain_thread, drain_entry, NULL, 64);
	blocks_init();
	world_create(&wa);
	world_create(&wb);
	assert(!wa.build_focus && !wb.build_focus
		   && "world_create must leave the focus off");
	assert(chunk_mesher_init());

	for(int cx = GMIN; cx < GMIN + GRID; cx++)
		for(int cz = GMIN; cz < GMIN + GRID; cz++) {
			build_scene(&wa, cx, cz);
			build_scene(&wb, cx, cz);
		}
	size_t na = dict_order(&wa, all_a);
	size_t nb = dict_order(&wb, all_b);
	assert(na == nb && na >= (size_t)(GRID * GRID));
	drain(&wa, all_a, na);
	drain(&wb, all_b, nb);

	/* The focus centre: the full-disk-fitting centre whose 3x3 core reaches
	 * LATEST in wa's hash order -- the starved case the Wii tapes showed. */
	int best_cx = 0, best_cz = 0;
	size_t best_last = 0;
	for(int cx = GMIN + FOCUS_R; cx < GMIN + GRID - FOCUS_R; cx++)
		for(int cz = GMIN + FOCUS_R; cz < GMIN + GRID - FOCUS_R; cz++) {
			size_t last = 0;
			for(size_t i = 0; i < na; i++)
				if(cheb(all_a[i], cx, cz) <= 1 && i > last)
					last = i;
			if(last > best_last) {
				best_last = last;
				best_cx = cx;
				best_cz = cz;
			}
		}
	assert(best_last >= (size_t)(STORM_FRAMES * CHUNK_MESHER_QLENGTH / 4)
		   && "no centre's core hashes late enough to show starvation");
	printf("  focus centre (%d,%d): its 3x3 core's last chunk is #%zu of %zu "
		   "in hash order\n", best_cx, best_cz, best_last, na);

	/* (L) local order: the hash walk, exactly. */
	{
		assert(!world_build_focus_wanted(false, false));
		assert(!world_build_focus_wanted(false, true)
			   && "LOCAL loading must never take the focus band");
		assert(!world_build_focus_wanted(true, false)
			   && "in-game frames must never take the focus band");
		assert(world_build_focus_wanted(true, true));

		chunk_mesher_test_set_enqueue_cap(1);
		for(int pass = 0; pass < 2; pass++) {
			if(pass == 1) /* explicitly disabled == never set */
				world_set_build_focus(&wa, false, best_cx, best_cz, FOCUS_R);
			assert(!wa.build_focus);
			dirty_all(all_a, na);
			size_t len = record_sequence(&wa, all_a, na, seq_a);
			assert(len == na);
			for(size_t i = 0; i < na; i++)
				assert(seq_a[i] == all_a[i]
					   && "local (focus-off) send order diverged from the "
						  "pre-COREFIRST hash walk");
		}
		printf("  (L) focus off: %zu sends in exactly the hash-walk order "
			   "(twice: never set, and set disabled)\n", na);
	}

	/* (O) remote order: rings 0..R (row-major within a ring), then hash. */
	{
		world_set_build_focus(&wb, true, best_cx, best_cz, FOCUS_R);
		assert(wb.build_focus);
		size_t ne = 0;
		for(int ring = 0; ring <= FOCUS_R; ring++)
			for(int dz = -ring; dz <= ring; dz++)
				for(int dx = -ring; dx <= ring; dx++) {
					int adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
					if((adx > adz ? adx : adz) != ring)
						continue;
					for(size_t i = 0; i < nb; i++) /* k ascending == dict */
						if(ccx(all_b[i]) == best_cx + dx
						   && ccz(all_b[i]) == best_cz + dz)
							expect[ne++] = all_b[i];
				}
		size_t disk = ne;
		for(size_t i = 0; i < nb; i++)
			if(cheb(all_b[i], best_cx, best_cz) > FOCUS_R)
				expect[ne++] = all_b[i];
		assert(ne == nb);
		dirty_all(all_b, nb);
		size_t len = record_sequence(&wb, all_b, nb, seq_b);
		assert(len == nb);
		size_t core = 0;
		for(size_t i = 0; i < nb; i++) {
			assert(seq_b[i] == expect[i]
				   && "remote-loading send order is not core-first, "
					  "nearest-ring-first, then hash");
			if(i < disk && cheb(seq_b[i], best_cx, best_cz) <= 1)
				core++;
		}
		printf("  (O) focus on: the %zu core chunks went first, then rings "
			   "2..%d (%zu disk chunks), then %zu in hash order\n", core,
			   FOCUS_R, disk, nb - disk);
		chunk_mesher_test_set_enqueue_cap(-1);
	}

	/* (S) storm at the full production budget, lockstep. */
	{
		struct chunk* storm[STORM_EDITS];
		size_t ns = 0;
		for(size_t i = 0; i < na && ns < STORM_EDITS; i++)
			if(cheb(all_a[i], best_cx, best_cz) > 1)
				storm[ns++] = all_a[i];
		assert(ns == STORM_EDITS);
		dirty_all(all_a, na);
		dirty_all(all_b, nb);
		/* The core's own work at 8 sends a frame, plus one frame of slack. */
		size_t core_chunks = 0;
		for(size_t i = 0; i < nb; i++)
			if(cheb(all_b[i], best_cx, best_cz) <= 1)
				core_chunks++;
		int core_frames = (int)((core_chunks + CHUNK_MESHER_QLENGTH - 1)
								/ CHUNK_MESHER_QLENGTH) + 1;
		assert(core_frames < STORM_FRAMES);
		int core_frame_b = -1;
		bool core_done_a = false;
		for(int frame = 1; frame <= STORM_FRAMES; frame++) {
			/* The server's settle storm: real edits, urgent-marked, same
			 * cells in both worlds (interior cell: no neighbour effect). */
			for(size_t e = 0; e < ns; e++) {
				w_coord_t x = storm[e]->x + 8, z = storm[e]->z + 8;
				int type = (frame & 1) ? BLOCK_STONE : BLOCK_AIR;
				set(&wa, x, YF + 2, z, type);
				set(&wb, x, YF + 2, z, type);
			}
			/* One mesher queue serves both worlds: give each its own full
			 * production frame (empty queue, 8 tokens), one after the other. */
			world_build_chunks(&wa, CHUNK_MESHER_QLENGTH, true);
			settle_in_flight(all_a, na, all_b, nb);
			world_build_chunks(&wb, CHUNK_MESHER_QLENGTH, true);
			settle_in_flight(all_a, na, all_b, nb);
			int miss = 0, pend = 0;
			world_near_ring_status(&wb, best_cx, best_cz, 1, &miss, &pend);
			if(miss == 0 && pend == 0 && core_frame_b < 0)
				core_frame_b = frame;
			world_near_ring_status(&wa, best_cx, best_cz, 1, &miss, &pend);
			if(miss == 0 && pend == 0)
				core_done_a = true;
		}
		assert(core_frame_b > 0 && core_frame_b <= core_frames
			   && "remote focus did not mesh the safe core within "
				  "ceil(core/8)+1 frames under a 16-edit/frame storm");
		assert(!core_done_a
			   && "control: the hash-order world meshed a late-hashing core "
				  "under the storm -- the test no longer shows the hold");
		printf("  (S) %d-edit/frame storm: focus core (%zu chunks) meshed at "
			   "frame %d (<= %d); hash-order core unmeshed after %d frames\n",
			   STORM_EDITS, core_chunks, core_frame_b, core_frames, STORM_FRAMES);
	}

	/* (E) same output. */
	{
		world_set_build_focus(&wb, false, 0, 0, 0);
		drain(&wa, all_a, na);
		drain(&wb, all_b, nb);
		for(size_t i = 0; i < na; i++) {
			struct chunk* b = world_find_chunk(&wb, all_a[i]->x, all_a[i]->y,
											   all_a[i]->z);
			assert(b);
			fp_ref[i] = mesh_fingerprint(all_a[i]);
			assert(mesh_fingerprint(b) == fp_ref[i]
				   && "core-first scheduling changed a chunk's mesh bytes");
		}
		/* Each chunk re-meshed alone, no scheduling at all. */
		for(size_t i = 0; i < na; i++) {
			struct chunk* b = world_find_chunk(&wb, all_a[i]->x, all_a[i]->y,
											   all_a[i]->z);
			b->dirty_generation++;
			b->rebuild_displist = true;
			assert(chunk_check_built(b));
			settle_in_flight(all_b, nb, NULL, 0);
			assert(mesh_fingerprint(b) == fp_ref[i]
				   && "scheduled mesh differs from the same chunk meshed "
					  "alone");
		}
		printf("  (E) all %zu chunks byte-identical across local/remote "
			   "order and to a standalone re-mesh\n", na);
	}

	chunk_mesher_shutdown();
	g_drain_run = 0;
	thread_join(&drain_thread);
	world_destroy(&wa);
	world_destroy(&wb);
	printf("test_mesher_core_first: OK\n");
	return 0;
}
