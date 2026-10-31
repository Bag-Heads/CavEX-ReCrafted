/*
	Regression test for the real-Wii main-thread HANG in world_build_chunks.

	The main thread SPUN FOREVER (it was not blocked) in the render-list walk inside
	world_build_chunks. M-lib's intrusive list ends a walk by IDENTITY against the list's
	sentinel head, so a chain whose next-pointers no longer reach that head never
	terminates; the node contracts that would have caught it are M_ASSERT, and the
	shipping build compiles them out with -DNDEBUG. The watchdog decoded main phase=6
	WDM_BUILD_CHUNKS, and the server was blocked in clin_chunk_slot_acquire only because
	main never came back to release chunk slots.

	WHY THIS TARGET DISABLES THE M-LIB CONTRACTS
	M_IL1ST_NODE_CONTRACT enforces next->prev == node && prev->next == node on every node
	a walk touches. Under that invariant a forward chain that is both reachable from the
	head and never returns to it cannot be constructed -- the corruption is only
	expressible in the configuration that actually ships. So this ONE target is built with
	-DM_ASSERT(...)=((void)0), which reproduces exactly what -DNDEBUG does to m-lib and
	nothing else: the project's own assert() stays live (CMakeLists forces -UNDEBUG on
	every test_ target), so every assertion below is a real check.

	WHAT IS PINNED
	  (1) world_build_chunks RETURNS on a cyclic render chain instead of hanging, leaves
	      the budget unspent, and reports the over-run via world_ilist_overrun_count().
	  (2) world_render_completed survives the same cyclic render chain across TWO
	      CONSECUTIVE FRAMES. One frame is not enough to see it: the hand-off walk hands
	      _push_back the same chunk over and over, and re-pushing the node that is
	      currently the tail SELF-LINKS it, so an uncleaned bail leaves the wreckage in
	      gpu_busy_chunks and the NEXT frame's drain is what pays -- it walks the
	      self-link forever, or, once _remove has nulled the node, unrefs the same chunk
	      twice and dereferences it after the free. The bail must therefore leave both
	      lists walkable and no chunk unref'd twice.
	  (3) The drain itself refuses to _remove a node that points at itself (m-lib unlinks
	      by writing through the node's own pointers), whatever put it there.
	  (4) A healthy list of the same chunks never trips the cap (no false positives), and
	      still moves each token exactly once and releases it exactly once.
	  (5) The section-scan budget holds: the inner COLUMN_HEIGHT loop used to spend up to
	      8 tokens per outer step while only the outer while re-tested `tokens > 0`, so
	      size_t wrapped 0 -> SIZE_MAX and the budgeted scan silently became a full scan
	      of every resident section, every frame.
	  (6) A DIMENSION SWITCH does not make the guard fire on a healthy list. The cap was
	      derived from the CURRENT section count, but world_unload_all empties the section
	      dictionary in one call while render and gpu_busy_chunks still hold the previous
	      frame's entries -- so on that frame the bound collapsed to the floor and any
	      render list longer than it took the deliberate-leak bail on a perfectly good
	      chain. Driven at the real boundary (one entry more than the floor) so the walk
	      actually crosses the collapsed cap.

	Not covered here: world_render's draw walks carry the identical bound but need a live
	GL context (gfx_bind_texture/gfx_fog) to drive, so they stay a review-only site rather
	than a reason to give this test a window. world_pre_render's fog partition is the one
	remaining walk that both REMOVES and re-pushes: on a corrupt chain it re-visits the
	node it just moved and then follows the it->next m-lib cached before the push_front,
	which is NULL by then -- a fault the cap cannot reach and a node check cannot see (the
	node looks healthy; it is the ITERATOR that is stale). It also cannot be driven from
	here, because world_pre_render re-inits the very list it walks before walking it.
*/

#ifndef M_ASSERT
#error "test_world_chunk_walk_guard must be built with the m-lib node contracts disabled (-DM_ASSERT(...)=((void)0)) so the shipping -DNDEBUG corruption is expressible; see CMakeLists.txt"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "world.h"
#include "game/game_state.h"

#define CYCLE_CHUNKS 3

// Three real chunks, linked exactly as world_bfs / world_render_completed leave them. No
// section owns them, so the walk cap for these worlds is WORLD_ILIST_WALK_CAP_MIN.
static void link_chunks(struct world* w, struct chunk** c, bool gpu_busy) {
	for(int i = 0; i < CYCLE_CHUNKS; i++) {
		c[i] = chunk_alloc();
		assert(c[i]);
		chunk_init(c[i], w, 0, i * CHUNK_SIZE, 0);
		chunk_ref(c[i]);
		if(gpu_busy)
			ilist_chunks2_push_back(w->gpu_busy_chunks, c[i]);
		else
			ilist_chunks_push_back(w->render, c[i]);
	}
	assert(dict_wsection_size(w->sections) == 0);
}

static void test_render_cycle_returns(void) {
	struct world w;
	world_create(&w);
	world_ilist_overrun_reset();

	struct chunk* c[CYCLE_CHUNKS];
	link_chunks(&w, c, false);

	// Control first: the healthy list this corruption is spliced into walks clean, so a
	// pass below is the guard firing on real corruption and not the cap misjudging a
	// legitimate render list.
	assert(world_build_chunks(&w, 4, true) == 4);
	assert(world_ilist_overrun_count() == 0);

	// The corruption: one embedded node's forward pointer leads nowhere but itself, so
	// the sentinel head is unreachable and _end_p can never fire.
	struct m_il1st_head_s* saved = c[1]->ilist_chunks.next;
	c[1]->ilist_chunks.next = &c[1]->ilist_chunks;

	// Before the fix this call never returned. Reaching the next line IS the assertion;
	// a regression fails by ctest timeout rather than by wedging the suite.
	size_t left = world_build_chunks(&w, 4, true);

	assert(world_ilist_overrun_count() == 1);
	assert(left == 4 && "an abandoned pass must not consume budget");

	c[1]->ilist_chunks.next = saved; // repair before teardown
	for(int i = 0; i < CYCLE_CHUNKS; i++)
		chunk_unref(c[i]);
	world_destroy(&w);
	puts("  render list: cyclic chain -> world_build_chunks returned, over-run reported");
}

// A second reference per chunk stands in for the one a resident section holds in
// production. It keeps every chunk alive -- and its reference_count readable -- past
// whatever the frames below do with the render token, so a chunk unref'd twice shows up as
// a wrong count and not only as a use-after-free that a lucky allocator might swallow.
static void ref_as_resident(struct chunk** c) {
	for(int i = 0; i < CYCLE_CHUNKS; i++)
		chunk_ref(c[i]);
}

// main.c's per-frame order for the two calls that matter here, minus the BFS: the render
// list is hand-built above instead. world_pre_render_clear IS the O(1) head reset
// world_pre_render opens with, and is the path main.c takes on a no-render frame.
static void run_frame(struct world* w, bool new_render) {
	if(!new_render)
		world_pre_render_clear(w);

	assert(world_build_chunks(w, 4, true) == 4); // nothing can mesh: no mesher yet
	world_render_completed(w, new_render);
}

static void test_render_to_gpu_busy_two_frames_healthy(void) {
	struct world w;
	world_create(&w);
	world_ilist_overrun_reset();

	struct chunk* c[CYCLE_CHUNKS];
	link_chunks(&w, c, false);
	ref_as_resident(c);

	// Frame 1 hands each render token to gpu_busy_chunks -- a MOVE, so the counts must
	// not budge. Frame 2 drains it and releases exactly those tokens. This is the control
	// for the cyclic case below: it fails if a bail-happy guard starts dropping the
	// hand-off (counts stay at 2) or drains it twice (counts fall to 0).
	run_frame(&w, true);
	for(int i = 0; i < CYCLE_CHUNKS; i++)
		assert(c[i]->reference_count == 2 && "the hand-off MOVES the token");

	run_frame(&w, false);
	for(int i = 0; i < CYCLE_CHUNKS; i++)
		assert(c[i]->reference_count == 1
			   && "the drain must release each moved token exactly once");

	assert(ilist_chunks2_empty_p(w.gpu_busy_chunks));
	assert(world_ilist_overrun_count() == 0);

	for(int i = 0; i < CYCLE_CHUNKS; i++)
		chunk_unref(c[i]);
	world_destroy(&w);
	puts("  render -> gpu-busy: healthy list -> token moved once, drained once");
}

static void test_render_to_gpu_busy_cycle_survives_next_frame(void) {
	struct world w;
	world_create(&w);
	world_ilist_overrun_reset();

	struct chunk* c[CYCLE_CHUNKS];
	link_chunks(&w, c, false);
	ref_as_resident(c);

	struct m_il1st_head_s* saved = c[1]->ilist_chunks.next;
	c[1]->ilist_chunks.next = &c[1]->ilist_chunks;

	// FRAME 1. The build pass abandons its walk, then world_render_completed's second walk
	// abandons the render -> gpu_busy hand-off. Nothing unlinks the render chain, so that
	// walk spins for exactly the same reason world_build_chunks did -- but unlike the build
	// pass it MUTATES as it goes, pushing the revisited chunk into gpu_busy_chunks on every
	// turn of the loop. That is what its bail has to clean up after.
	run_frame(&w, true);
	assert(world_ilist_overrun_count() == 2
		   && "one abandoned walk in the build pass, one in the hand-off");

	// FRAME 2, with the corruption left exactly as frame 1 saw it. Repairing the chain or
	// the hand-off's leftovers here is what would hide the defect: the cost of an
	// abandoned hand-off is not paid until the next frame's drain walks what it left.
	// Reaching the assertions below at all is half the check -- an uncleaned bail faults
	// inside the drain, in m-lib's _unlink, on a self-linked node.
	run_frame(&w, false);

	assert(world_ilist_overrun_count() == 2
		   && "frame 2 must find a coherent gpu_busy list, with nothing to trip over");
	assert(ilist_chunks2_empty_p(w.gpu_busy_chunks));

	// The other half: no chunk was unref'd twice. 2, not 1, is the DOCUMENTED cost of the
	// abandoned hand-off -- it dropped the tokens it was carrying rather than hand a
	// duplicate-laden list to the drain, and it cannot give them back because the only
	// record of who held them is the render chain it just proved unwalkable. Bounded to
	// one frame's visible set, reported by world_ilist_overrun_count(), and it is a leak
	// against a double free.
	for(int i = 0; i < CYCLE_CHUNKS; i++)
		assert(c[i]->reference_count == 2
			   && "a chunk was unref'd twice (or the token leak grew)");

	c[1]->ilist_chunks.next = saved; // repair before teardown
	for(int i = 0; i < CYCLE_CHUNKS; i++) {
		chunk_unref(c[i]); // the token the abandoned hand-off could not give back
		chunk_unref(c[i]); // the resident reference -> chunk_destroy
	}
	world_destroy(&w);
	puts("  render -> gpu-busy: cyclic chain -> hand-off abandoned, the NEXT frame's "
		 "drain walks a coherent list and no chunk is unref'd twice");
}

static void test_gpu_busy_self_linked_node_survives_drain(void) {
	struct world w;
	world_create(&w);
	world_ilist_overrun_reset();

	struct chunk* c[CYCLE_CHUNKS];
	link_chunks(&w, c, true);
	ref_as_resident(c);

	// The shape a duplicate _push_back leaves behind, injected directly so the drain is
	// pinned independently of whatever produced it. m-lib's _unlink detaches a node by
	// writing through the pointers stored IN it, so on this node it writes through itself
	// and then through the NULLs it just stored, and the iterator hands the same chunk
	// back for a second unref.
	c[1]->ilist_chunks2.next = &c[1]->ilist_chunks2;
	c[1]->ilist_chunks2.prev = &c[1]->ilist_chunks2;

	world_render_completed(&w, false);

	assert(world_ilist_overrun_count() == 1);
	assert(ilist_chunks2_empty_p(w.gpu_busy_chunks));
	assert(c[0]->reference_count == 1
		   && "the entries ahead of the bad node must still drain normally");
	assert(c[1]->reference_count == 2 && "the self-linked chunk must not be unref'd");
	assert(c[2]->reference_count == 2 && "nor anything the bail left behind it");

	chunk_unref(c[0]);
	for(int i = 1; i < CYCLE_CHUNKS; i++) {
		chunk_unref(c[i]); // the token the bail could not release
		chunk_unref(c[i]); // the resident reference -> chunk_destroy
	}
	world_destroy(&w);
	puts("  gpu-busy: self-linked node -> drain bails, list left empty, no double unref");
}

// One entry MORE than the floor a cap derived from an EMPTY section dictionary yields, so
// the walk really does cross the collapsed bound rather than merely disagree with it on
// paper. One chunk per section: the same chunks both raise the pre-switch cap (sections *
// COLUMN_HEIGHT) and fill the render list.
#define SWITCH_CHUNKS ((size_t)WORLD_ILIST_WALK_CAP_MIN + 1u)

static void test_dimension_switch_keeps_a_usable_walk_cap(void) {
	struct world w;
	world_create(&w);
	world_ilist_overrun_reset();

	// world_set_block creates the section and its single chunk lazily, which is far
	// cheaper than a full COLUMN_HEIGHT column and gives one section per chunk.
	for(size_t i = 0; i < SWITCH_CHUNKS; i++)
		world_set_block(&w, (w_coord_t)(i * CHUNK_SIZE), 0, 0,
						(struct block_data) {.type = BLOCK_STONE,
											 .metadata = 0,
											 .sky_light = 15,
											 .torch_light = 0},
						false);
	assert(dict_wsection_size(w.sections) == SWITCH_CHUNKS);

	// Hand those chunks to the render list exactly as world_bfs leaves them at the end of
	// a frame -- one reference token each, still resident.
	for(size_t i = 0; i < SWITCH_CHUNKS; i++) {
		struct chunk* c = world_find_chunk(&w, (w_coord_t)(i * CHUNK_SIZE), 0, 0);
		assert(c);
		ilist_chunks_push_back(w.render, c);
		chunk_ref(c);
	}

	const size_t cap_before = world_ilist_walk_cap_current(&w);
	assert(cap_before > WORLD_ILIST_WALK_CAP_MIN
		   && "the fixture did not raise the cap above its floor");
	assert(cap_before >= SWITCH_CHUNKS);

	// THE SWITCH. client_interface.c takes this path on WORLD_RESET, on CRPC_STREAM_RESET
	// and when a voxel-stream install fails. It does not drain either chunk list -- and
	// cannot: after the hand-off both lists name the same nodes while only gpu_busy_chunks
	// owns the tokens, so unref-ing both would double-free every chunk in flight.
	world_unload_all(&w);
	assert(dict_wsection_size(w.sections) == 0);
	assert(world_ilist_walk_cap_current(&w) >= cap_before
		   && "the walk bound collapsed with the section dictionary, below the entry count "
			  "the render list legitimately still holds");

	// FRAME 1 after the switch: the build walk crosses the whole render list, then the
	// hand-off walk moves all of it into gpu_busy_chunks. Every entry is healthy.
	assert(world_build_chunks(&w, 4, true) == 4); // nothing can mesh: no mesher yet
	world_render_completed(&w, true);
	assert(world_ilist_overrun_count() == 0
		   && "a healthy render list was abandoned after a dimension switch");

	// FRAME 2: the drain walks the same number of entries out of gpu_busy_chunks, still
	// with zero sections resident, and releases each token exactly once.
	world_pre_render_clear(&w);
	assert(world_build_chunks(&w, 4, true) == 4);
	world_render_completed(&w, false);
	assert(world_ilist_overrun_count() == 0
		   && "the post-switch gpu-busy drain was abandoned on a healthy list");
	assert(ilist_chunks2_empty_p(w.gpu_busy_chunks));

	// The drain released the last reference on every chunk (world_unload_all dropped the
	// resident one), so nothing is left to free by hand.
	world_destroy(&w);
	printf("  dimension switch: %u sections -> 0 with %u list entries in flight, cap held "
		   "at >= %u, no spurious over-run\n",
		   (unsigned)SWITCH_CHUNKS, (unsigned)SWITCH_CHUNKS, (unsigned)cap_before);
}

static void build_air_section(struct world* w, w_coord_t x, w_coord_t z) {
	const size_t cells = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	uint8_t* ids = calloc(cells, sizeof(*ids));
	uint8_t* metadata = calloc(cells / 2u, sizeof(*metadata));
	uint8_t* sky = calloc(cells / 2u, sizeof(*sky));
	uint8_t* torch = calloc(cells / 2u, sizeof(*torch));
	uint8_t* heightmap = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*heightmap));
	uint8_t* grass_i = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*grass_i));
	uint8_t* grass_j = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*grass_j));
	assert(ids && metadata && sky && torch && heightmap && grass_i && grass_j);

	assert(world_build_full_section(w, x, z, ids, metadata, sky, torch, heightmap,
									grass_i, grass_j));

	free(grass_j);
	free(grass_i);
	free(heightmap);
	free(torch);
	free(sky);
	free(metadata);
	free(ids);
}

// chunk_check_built sets mesh_in_flight on the builds it actually dispatched.
static size_t count_dispatched(struct world* w, w_coord_t x, w_coord_t z) {
	size_t n = 0;
	for(int cy = 0; cy < COLUMN_HEIGHT; cy++) {
		struct chunk* c = world_find_chunk(w, x, cy * CHUNK_SIZE, z);
		assert(c);
		if(c->mesh_in_flight)
			n++;
	}
	return n;
}

static void test_section_scan_budget_holds(void) {
	struct world w;
	world_create(&w);
	world_ilist_overrun_reset();

	// Two resident sections, every column entry buildable. w.render stays empty, so the
	// whole budget reaches the section scan.
	build_air_section(&w, 0, 0);
	build_air_section(&w, CHUNK_SIZE, 0);
	for(int cy = 0; cy < COLUMN_HEIGHT; cy++) {
		// An edge chunk that has waited out the B2 first-mesh gate is a real production
		// state, and it is what makes every column entry buildable on the first pass.
		world_find_chunk(&w, 0, cy * CHUNK_SIZE, 0)->first_meshed = true;
		world_find_chunk(&w, CHUNK_SIZE, cy * CHUNK_SIZE, 0)->first_meshed = true;
	}

	size_t left = world_build_chunks(&w, 1, true);

	// One token in, one build out. Before the fix the second buildable chunk in the same
	// column spent a token that was already 0, so `left` came back near SIZE_MAX and the
	// scan ran on through every remaining column and section.
	assert(left == 0 && "the budget wrapped -- the size_t underflow is back");
	size_t built = count_dispatched(&w, 0, 0) + count_dispatched(&w, CHUNK_SIZE, 0);
	assert(built == 1 && "a 1-token pass built more than one chunk");
	assert(world_ilist_overrun_count() == 0);

	// A larger budget still stops exactly on it, including across the section boundary.
	left = world_build_chunks(&w, 3, true);
	assert(left == 0);
	built = count_dispatched(&w, 0, 0) + count_dispatched(&w, CHUNK_SIZE, 0);
	assert(built == 4);
	assert(world_ilist_overrun_count() == 0);

	world_destroy(&w);
	printf("  section scan: 2 sections x %u-deep columns, 1 token -> 1 build, "
		   "3 tokens -> 3 more\n",
		   (unsigned)COLUMN_HEIGHT);
}

static void test_two_view_visibility_lifetime(void) {
	struct world w;
	world_create(&w);
	world_ilist_overrun_reset();
	/* All-air, fully reachable columns; the two cameras see overlapping but
	 * unequal sets. No GL context or worker is involved in visibility. */
	for(int x = -4; x <= 4; x++) {
		world_set_block(&w, x * CHUNK_SIZE, 0, 0,
			(struct block_data){.type=BLOCK_AIR,.sky_light=15}, false);
		struct chunk* c = world_find_chunk(&w, x * CHUNK_SIZE, 0, 0);
		assert(c);
		memset(c->reachable, 0x3f, sizeof(c->reachable));
	}
	struct camera cameras[2] = {{.x=-40,.y=8,.z=8}, {.x=40,.y=8,.z=8}};
	struct camera* views[] = {&cameras[0], &cameras[1]};
	for(unsigned v = 0; v < 2; v++) {
		glm_mat4_identity(cameras[v].view);
		for(unsigned p = 0; p < 6; p++) cameras[v].frustum_planes[p][3] = 1;
	}
	gstate.config.fog_distance = 48;
	struct chunk* expected[2][9];
	size_t expected_count[2];
	for(unsigned v = 0; v < 2; v++) {
		gstate.players[0].camera = cameras[v];
		world_pre_render(&w, views[v], views[v]->view);
		expected_count[v] = 0;
		ilist_chunks_it_t it;
		ilist_chunks_it(it, w.render);
		while(!ilist_chunks_end_p(it)) {
			assert(expected_count[v] < 9);
			expected[v][expected_count[v]++] = ilist_chunks_ref(it);
			ilist_chunks_next(it);
		}
		world_render_completed(&w, true);
		world_pre_render_clear(&w);
		world_render_completed(&w, false);
	}
	assert(expected_count[0] && expected_count[1]);
	/* Deliberately irrelevant primary global: secondary fog may not read it. */
	gstate.players[0].camera.x = 10000;
	for(unsigned frame = 0; frame < 128; frame++) {
		assert(world_pre_render_views(&w, views, 2));
		for(unsigned v = 0; v < 2; v++) {
			assert(w.render_views[v].count == expected_count[v]);
			assert(!memcmp(w.render_views[v].chunks, expected[v],
				expected_count[v] * sizeof(expected[v][0])));
		}
		bool seen[9] = {false};
		unsigned union_count = 0;
		ilist_chunks_it_t it;
		ilist_chunks_it(it, w.render);
		while(!ilist_chunks_end_p(it)) {
			struct chunk* c = ilist_chunks_ref(it);
			unsigned slot = c->x / CHUNK_SIZE + 4;
			assert(slot < 9 && !seen[slot]);
			seen[slot] = true;
			union_count++;
			assert(c->reference_count == (frame ? 3u : 2u));
			ilist_chunks_next(it);
		}
		assert(union_count == 9);
		assert(expected_count[0] + expected_count[1] > union_count);
		world_render_completed(&w, true);
		for(unsigned i = 0; i < 9; i++)
			assert(world_find_chunk(&w, ((int)i - 4) * CHUNK_SIZE, 0, 0)->reference_count == 2);
	}
	/* A stream reset drops world ownership while BOTH views still borrow the
	 * same frame. They survive until the next completed flip, then drain once. */
	world_unload_all(&w);
	for(unsigned v = 0; v < 2; v++)
		for(size_t i = 0; i < expected_count[v]; i++)
			assert(expected[v][i]->reference_count == 1);
	world_pre_render_clear(&w);
	world_render_completed(&w, false);
	assert(ilist_chunks2_empty_p(w.gpu_busy_chunks));
	assert(world_ilist_overrun_count() == 0);
	world_destroy(&w);
	puts("  two views: independent ordered visibility, overlap owns one GPU token, 128 flips and unload balanced");
}

static void test_two_view_lod_uses_nearest_owner(void) {
	struct world w;
	world_create(&w);
	for(int x = 0; x <= 160; x += 80)
		world_set_block(&w, x, 0, 0, (struct block_data){.type=BLOCK_STONE}, false);
	struct camera cameras[2] = {{.x=0}, {.x=160}};
	struct camera* views[] = {&cameras[0], &cameras[1]};
	world_update_lod_views(&w, views, 2);
	struct chunk* host = world_find_chunk(&w, 0, 0, 0);
	struct chunk* middle = world_find_chunk(&w, 80, 0, 0);
	struct chunk* guest = world_find_chunk(&w, 160, 0, 0);
	assert(host->lod == 0 && host->foliage_lod == 0);
	assert(guest->lod == 0 && guest->foliage_lod == 0);
	assert(middle->lod == 1 && middle->foliage_lod == 2);
	uint32_t generation = guest->dirty_generation;
	world_update_lod_views(&w, views, 2);
	assert(guest->dirty_generation == generation);
	cameras[1].x = 0;
	world_update_lod_views(&w, views, 2);
	assert(guest->lod == 1 && guest->foliage_lod == 2);
	world_destroy(&w);
	puts("  two views: terrain near either owner retains full mesh and foliage; unchanged views do not dirty meshes");
}

int main(void) {
	blocks_init();

	// The cycle cases run BEFORE the mesher exists: chunk_mesher_send then refuses every
	// send, so the walks terminate on the cap alone and nothing else can mask it.
	test_two_view_visibility_lifetime();
	test_two_view_lod_uses_nearest_owner();
	test_render_cycle_returns();
	test_render_to_gpu_busy_two_frames_healthy();
	test_render_to_gpu_busy_cycle_survives_next_frame();
	test_gpu_busy_self_linked_node_survives_drain();
	test_dimension_switch_keeps_a_usable_walk_cap();

	chunk_mesher_init(); // spawns the real worker thread; seeds the send tokens
	test_section_scan_budget_holds();

	puts("test_world_chunk_walk_guard: OK -- corrupt chunk chains degrade to a dropped "
		 "build/draw pass instead of hanging the main thread OR faulting a frame later, "
		 "and the section-scan budget no longer underflows");
	return 0;
}
