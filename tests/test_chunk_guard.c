/*
	MEM1 red-zone regression guard for struct chunk.

	The 2026-07-27 real-Wii hang was a main-thread spin over a corrupted intrusive-list chain
	whose nodes are the last members of struct chunk -- a plain-malloc MEM1 allocation that the
	MEM2 red zone (platform/mem2.c) structurally cannot see, which is why the canary read 0 and
	named nothing. chunk.c now lays the same red zone after the struct. These cases pin the
	three properties the diagnostic is worth anything for: it does not cry wolf on a healthy
	chunk, it FIRES on a one-byte overrun immediately past the struct, and the offset it reports
	is the real distance into the guard (that offset is the writer's fingerprint -- a stray
	writer with a fixed stride reproduces it across runs).

	The overrun is written as ~(current guard byte) so the cases stay correct whatever fill
	pattern chunk.c uses.
*/

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// blocks.h first, exactly as chunk.c orders it: world.h pulls in chunk.h from its own middle,
// so entering the cycle at chunk.h leaves world.h without CHUNK_SIZE.
#include "block/blocks.h"
#include "chunk.h"
#include "game/game_state.h"
#include "platform/displaylist.h"
#include "platform/gfx.h"
#include "platform/guard_scan.h"

// --- link stubs -------------------------------------------------------------
// chunk.c is one translation unit: its render/mesh/neighbour paths must resolve even though
// only chunk_alloc / chunk_init / chunk_ref / chunk_unref run here. None of these is reachable
// from an alloc/destroy cycle, so an abort body would be equally correct and less portable.
struct game_state gstate;

/* This narrow allocator fixture never installs a local column. */
void column_backing_ref(struct column_backing* backing) { assert(!backing); }
void column_backing_unref(struct column_backing* backing) { assert(!backing); }
void column_view_ref(struct column_view* view) { assert(!view); }
void column_view_unref(struct column_view* view) { assert(!view); }
struct block_data column_view_get(struct column_view* view, unsigned index) {
	(void)view; (void)index;
	abort(); // allocator-only fixture never reads a differential view
}
void column_view_set(struct column_view* view, unsigned index,
	struct block_data block, bool light_only) {
	(void)view; (void)index; (void)block; (void)light_only;
	abort();
}

struct chunk* world_find_chunk(struct world* w, w_coord_t x, w_coord_t y,
							   w_coord_t z) {
	(void)w;
	(void)x;
	(void)y;
	(void)z;
	return NULL;
}

struct chunk* world_find_chunk_neighbour(struct world* w, struct chunk* c,
										 enum side s) {
	(void)w;
	(void)c;
	(void)s;
	return NULL;
}

bool chunk_mesher_send(struct chunk* c) {
	(void)c;
	return false;
}

void displaylist_render(struct displaylist* l) {
	(void)l;
}

void displaylist_destroy(struct displaylist* l) {
	(void)l;
}

void gfx_matrix_modelview(mat4 m) {
	(void)m;
}

void gfx_fog(bool enable) {
	(void)enable;
}

static float fog_dx, fog_dz;
void gfx_fog_pos(float dx, float dz, float distance) {
	fog_dx = dx; fog_dz = dz;
	(void)distance;
}

void gfx_tint(bool enable) {
	(void)enable;
}

void gfx_cull_func(enum cull_func func) {
	(void)func;
}

/* DC-GRASS G1: the grass side-overlay pass toggles the alpha test. */
void gfx_alpha_test(bool enable) {
	(void)enable;
}

void gfx_alpha_test_ref(bool enable, float ref) {
	(void)enable;
	(void)ref;
}

// --- helpers ----------------------------------------------------------------
// chunk_init only stores the world pointer (its assert just rejects NULL), so a zeroed
// stand-in is enough and keeps the test free of world.c and the whole dict/section stack.
static struct world g_dummy_world;

static struct chunk* guard_test_alloc(void) {
	struct chunk* c = chunk_alloc();
	assert(c);
	chunk_init(c, &g_dummy_world, 0, 0, 0);
	chunk_ref(c); // chunk_unref destroys at 0, so take the one reference the cycle drops
	return c;
}

// Flip one guard byte at `off` past the end of the struct. Reading first, then writing the
// complement, makes the case independent of chunk.c's fill pattern.
static void guard_test_smash(struct chunk* c, size_t off) {
	uint8_t* g = (uint8_t*)c + sizeof(struct chunk) + off;
	*g = (uint8_t)~(*g);
}

static size_t guard_reference(const uint8_t* p, size_t n) {
	for(size_t i = 0; i < n; i++) if(p[i] != 0xA5) return i+1;
	return 0;
}
static void test_guard_scan_equiv(void) {
	uint8_t memory[16384+8];
	memset(memory, 0xA5, sizeof(memory));
	for(size_t align = 0; align < 4; align++) {
		uint8_t* p = memory+align;
		assert(guard_scan_a5(p, 16384) == 0);
		for(size_t i = 0; i < 16384; i++) {
			p[i] = 0;
			assert(guard_scan_a5(p, 16384) == guard_reference(p, 16384));
			p[i] = 0xA5;
		}
		for(size_t n = 0; n < 9; n++) {
			assert(guard_scan_a5(p,n) == guard_reference(p,n));
			for(size_t i = 0; i < n; i++)
				for(unsigned v = 0; v < 256; v++) {
					p[i] = v;
					assert(guard_scan_a5(p,n) == guard_reference(p,n));
					p[i] = 0xA5;
				}
		}
		p[2] = 0; p[5] = 255;
		assert(guard_scan_a5(p,16384) == 3);
		p[2] = p[5] = 0xA5;
	}
	puts("Guard scan equivalence PASS: every 16KB offset, four alignments, all byte values/tails, earliest of multiple faults");
}

int main(void) {
	test_guard_scan_equiv();
	{
		struct chunk* c = guard_test_alloc();
		c->has_displist[SIDE_TOP] = true;
		gstate.players[0].camera.x = 1000;
		gstate.players[0].camera.z = -1000;
		chunk_pre_render(c, GLM_MAT4_IDENTITY, true);
		chunk_render(c, false, -32, 8, 48);
		assert(fog_dx == 32 && fog_dz == -48);
		chunk_render(c, false, 17, 8, -23);
		assert(fog_dx == -17 && fog_dz == 23);
		chunk_unref(c);
		puts("Chunk fog uses each draw camera, independent of primary global");
	}
	// The guard must not eat the alignment slack chunk_alloc already depended on: the stashed
	// malloc base still has to sit below the struct, and the guard strictly above it. Both are
	// pointer-arithmetic invariants of the over-allocation, so check them on a live block
	// rather than trusting the comment.
	{
		chunk_guard_reset();
		struct chunk* c = guard_test_alloc();
		const uint8_t* stash = (const uint8_t*)((void**)c)[-1];
		assert(stash < (const uint8_t*)c);
		assert(stash + sizeof(void*) <= (const uint8_t*)c);

		// The guard has to FIT: chunk_alloc requests
		//   sizeof(struct chunk) + align + sizeof(void*) + CHUNK_GUARD
		// and the only unknown in "does the guard's last byte stay inside that" is how much
		// the over-alignment step consumed. Measure that slack on the live block instead of
		// re-deriving the request here -- a test that hand-copied the whole formula would
		// agree with a broken chunk.c by construction.
		size_t align = __alignof__(struct chunk); // same spelling chunk_alloc uses (C99-safe)
		if(align < sizeof(void*))
			align = sizeof(void*);
		assert((size_t)((const uint8_t*)c - stash) <= sizeof(void*) + align - 1);

		// ...and the whole guard is uniformly patterned, i.e. chunk_alloc really filled it.
		const uint8_t* g = (const uint8_t*)c + sizeof(struct chunk);
		for(size_t i = 1; i < CHUNK_GUARD; i++)
			assert(g[i] == g[0]);
		chunk_unref(c);
		assert(chunk_guard_hits() == 0);
	}

	// (2) A clean alloc/destroy cycle leaves the counter at 0 -- no false positive. Run before
	// the smash cases so a spurious hit cannot be mistaken for one of theirs.
	{
		chunk_guard_reset();
		for(int i = 0; i < 8; i++) {
			struct chunk* c = guard_test_alloc();
			chunk_unref(c);
		}
		assert(chunk_guard_hits() == 0);
		void* first = (void*)1;
		size_t off = 1;
		chunk_guard_first(&first, &off);
		assert(first == NULL);
		assert(off == 0);
		printf("clean cycles: hits=0 OK\n");
	}

	// (1) One byte written at EXACTLY the end of the struct -- the minimal overrun, and the
	// one that lands on whatever allocation follows this chunk in MEM1. Must fire, and must
	// name offset 0.
	{
		chunk_guard_reset();
		struct chunk* c = guard_test_alloc();
		uintptr_t expect = (uintptr_t)c; // compare by value; c is dangling after the destroy
		guard_test_smash(c, 0);
		chunk_unref(c);

		assert(chunk_guard_hits() == 1);
		void* first = NULL;
		size_t off = (size_t)-1;
		chunk_guard_first(&first, &off);
		assert((uintptr_t)first == expect);
		assert(off == 0);
		printf("overrun at +0: hits=%u off=%u OK\n", (unsigned)chunk_guard_hits(),
			   (unsigned)off);
	}

	// (3) A deeper overrun reports the DISTANCE, not just "something broke" -- this is the
	// number that fingerprints the writer's stride. Includes the last guard byte, which proves
	// the far end of the red zone is really covered by both the fill and the verify.
	{
		const size_t probes[] = {1, 4, 137, CHUNK_GUARD - 1};
		for(size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
			chunk_guard_reset();
			struct chunk* c = guard_test_alloc();
			guard_test_smash(c, probes[p]);
			chunk_unref(c);

			assert(chunk_guard_hits() == 1);
			size_t off = (size_t)-1;
			chunk_guard_first(NULL, &off);
			assert(off == probes[p]);
			printf("overrun at +%u: off=%u OK\n", (unsigned)probes[p],
				   (unsigned)off);
		}
	}

	// The verify reports the FIRST broken byte, so a wide smash still names the nearest edge --
	// the one that tells you where the writer started, not where it stopped.
	{
		chunk_guard_reset();
		struct chunk* c = guard_test_alloc();
		memset((uint8_t*)c + sizeof(struct chunk) + 16, 0x5A, CHUNK_GUARD - 16);
		chunk_unref(c);
		assert(chunk_guard_hits() == 1);
		size_t off = (size_t)-1;
		chunk_guard_first(NULL, &off);
		assert(off == 16);
	}

	// Hits accumulate across chunks while the first offender stays pinned to the first one.
	{
		chunk_guard_reset();
		struct chunk* a = guard_test_alloc();
		guard_test_smash(a, 8);
		uintptr_t expect = (uintptr_t)a;
		chunk_unref(a);

		struct chunk* b = guard_test_alloc();
		guard_test_smash(b, 200);
		chunk_unref(b);

		assert(chunk_guard_hits() == 2);
		void* first = NULL;
		size_t off = (size_t)-1;
		chunk_guard_first(&first, &off);
		assert((uintptr_t)first == expect);
		assert(off == 8);
	}

	// The cost the maintainer signed off on, restated against the real struct so a future
	// growth of struct chunk (or of the guard) shows up as a number in the log rather than as
	// silent MEM1 pressure. 968 chunks = the shipped far_view_distance=5 ceiling
	// ((2*5+1)^2 sections * 8 chunks/column).
	printf("sizeof(struct chunk)=%u guard=%u -> %.1f%% overhead, %u KB at 968 chunks\n",
		   (unsigned)sizeof(struct chunk), (unsigned)CHUNK_GUARD,
		   100.0 * (double)CHUNK_GUARD / (double)sizeof(struct chunk),
		   (unsigned)((968u * CHUNK_GUARD) / 1024u));

	printf("test_chunk_guard: PASS\n");
	return 0;
}
