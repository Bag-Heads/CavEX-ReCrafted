/*
	Proves displaylist_vertex() is byte-for-byte identical to the
	displaylist_pos() + displaylist_color() + displaylist_texcoord() triplet it
	replaces in the chunk mesher's emit hot path -- on BOTH the Wii (S16/U8) and
	PC (float) vertex layouts.

	Like test_vertex_light_equiv.c / test_chunk_reachable_equiv.c, this carries
	self-contained copies of the reference (triplet) and optimized (merged
	vertex) byte-write logic and asserts they produce the identical buffer.
	The copies below MUST mirror source/platform/{wii,pc}/displaylist.c exactly;
	if you change those, change these. They therefore snap capacities through
	the backends own ladder header (displaylist_ladder.h) at init, presize and
	grow, so the identity proof runs through rung code rather than a stale copy
	of the pre-ladder arithmetic; the real finalize/compaction path is covered
	by test_wii_finalize_equiv, which compiles the actual backend. The merged primitive only hoists the
	capacity reserve / struct loads / index advance out of the three calls -- it
	must not move, drop, or reorder a single emitted byte, must leave l->index
	identical, and (since the reserve is per-vertex, same as displaylist_pos)
	must grow l->length at the identical points.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/platform/displaylist.h"
/* The ladder is the backend's own capacity table -- the mirror below snaps to
 * the SAME header the backends do, so "hand-mirrors the backend" stays true
 * after the 2026-09-08 ladder landed and the identity proof actually runs
 * through init / presize / grow rung code instead of a stale copy of the old
 * arithmetic. (Only the rung LOOKUP is shared; every emitted byte below is
 * still written by this file's own copy of the emitter.) */
#include "../source/platform/displaylist_ladder.h"
void test_wii_finalize_equiv(void);
void test_pc_finalize_equiv(void);

#define MEM_U8(b, i) (*((uint8_t*)(b) + (i)))
#define MEM_U16(b, i) (*(uint16_t*)((uint8_t*)(b) + (i)))
#define MEM_FLT(b, i) (*(float*)((uint8_t*)(b) + (i)))

struct tdl {
	void* data;
	size_t length;
	size_t index;
	bool tinted;
	uint8_t tint[3];
};

/* ---- Wii backend (mirrors source/platform/wii/displaylist.c) ------------- */
#define CLL 32
#define CACHE_LINES(v, s) (((v) * (s) + 3 + CLL - 1) / CLL * CLL)

static void w_init(struct tdl* l, size_t vertices, size_t vertex_size) {
	// LADDER: mirrors displaylist_init -- capacity is the smallest FULL-table
	// rung >= the request.
	l->length = displaylist_rung(CACHE_LINES(vertices, vertex_size));
	l->data = NULL;
	l->index = CLL + 3;
	l->tinted = false;
	l->tint[0] = l->tint[1] = l->tint[2] = 255;
}

// T1-c instrumentation: counts grow (realloc) cycles, mirroring the real
// g_displaylist_grow_count this file's w_ensure otherwise leaves untracked.
static int g_grows = 0;

static void w_ensure(struct tdl* l, int vtx_bytes) {
	if(!l->data) {
		l->data = malloc(l->length + CLL);
		assert(l->data);
	}
	if(l->index + vtx_bytes > l->length) {
		// LADDER: mirrors displaylist_pos/_vertex -- a GROW snaps to the
		// BACKBONE table, never the densified midpoints.
		l->length = displaylist_rung_grow(
			(l->length * 3 / 2 + vtx_bytes + CLL - 1) / CLL * CLL);
		l->data = realloc(l->data, l->length + CLL);
		assert(l->data);
		g_grows++;
	}
}

// T1-c: mirrors displaylist_init_presized (Wii backend). `length` and
// `index` already share one coordinate frame (both counted from l->data's
// offset 0), so raising `length` to cover a remembered prior final index
// (rounded up to the cache line) is enough to guarantee no grow is needed
// for a same-shape rebuild -- no header/pad re-derivation required.
// prior_index == 0 reproduces plain w_init exactly.
static void w_init_presized(struct tdl* l, size_t vertices, size_t vertex_size,
							size_t prior_index) {
	w_init(l, vertices, vertex_size);
	// LADDER: mirrors displaylist_init_presized -- the hint snaps to a FULL
	// rung, not to the 32 B cache line.
	if(prior_index > l->length) {
		size_t hinted = displaylist_rung(prior_index);
		if(hinted > l->length)
			l->length = hinted;
	}
}

static void w_pos(struct tdl* l, int16_t x, int16_t y, int16_t z) {
	w_ensure(l, l->tinted ? 12 : 9);
	MEM_U16(l->data, l->index) = x;
	l->index += 2;
	MEM_U16(l->data, l->index) = y;
	l->index += 2;
	MEM_U16(l->data, l->index) = z;
	l->index += 2;
}
static void w_color(struct tdl* l, uint8_t index) {
	MEM_U8(l->data, l->index++) = index;
	if(l->tinted) {
		MEM_U8(l->data, l->index++) = l->tint[0];
		MEM_U8(l->data, l->index++) = l->tint[1];
		MEM_U8(l->data, l->index++) = l->tint[2];
	}
}
static void w_texcoord(struct tdl* l, uint8_t s, uint8_t t) {
	MEM_U8(l->data, l->index++) = s;
	MEM_U8(l->data, l->index++) = t;
}
static void w_vertex(struct tdl* l, int16_t x, int16_t y, int16_t z,
					 uint8_t light, uint8_t s, uint8_t t) {
	w_ensure(l, l->tinted ? 12 : 9);
	uint8_t* restrict p = (uint8_t*)l->data + l->index;
	MEM_U16(p, 0) = x;
	MEM_U16(p, 2) = y;
	MEM_U16(p, 4) = z;
	MEM_U8(p, 6) = light;
	if(l->tinted) {
		MEM_U8(p, 7) = l->tint[0];
		MEM_U8(p, 8) = l->tint[1];
		MEM_U8(p, 9) = l->tint[2];
		MEM_U8(p, 10) = s;
		MEM_U8(p, 11) = t;
		l->index += 12;
	} else {
		MEM_U8(p, 7) = s;
		MEM_U8(p, 8) = t;
		l->index += 9;
	}
}

/* ---- PC backend (mirrors source/platform/pc/displaylist.c) --------------- */
static void p_init(struct tdl* l) {
	l->length = 4096;
	l->data = NULL;
	l->index = 0;
	l->tinted = false;
	l->tint[0] = l->tint[1] = l->tint[2] = 255;
}
static void p_ensure(struct tdl* l) {
	if(!l->data) {
		l->data = malloc(l->length);
		assert(l->data);
	}
	if(l->index + (l->tinted ? 25 : 22) > l->length) {
		l->length *= 2;
		l->data = realloc(l->data, l->length);
		assert(l->data);
	}
}
static void p_pos(struct tdl* l, int16_t x, int16_t y, int16_t z) {
	p_ensure(l);
	MEM_FLT(l->data, l->index) = (float)x / 256.0F;
	l->index += 4;
	MEM_FLT(l->data, l->index) = (float)y / 256.0F;
	l->index += 4;
	MEM_FLT(l->data, l->index) = (float)z / 256.0F;
	l->index += 4;
}
static void p_color(struct tdl* l, uint8_t index) {
	MEM_U8(l->data, l->index++) = index % 16;
	MEM_U8(l->data, l->index++) = index / 16;
	if(l->tinted) {
		MEM_U8(l->data, l->index++) = l->tint[0];
		MEM_U8(l->data, l->index++) = l->tint[1];
		MEM_U8(l->data, l->index++) = l->tint[2];
	}
}
static void p_texcoord(struct tdl* l, uint8_t s, uint8_t t) {
	MEM_FLT(l->data, l->index) = (float)s / 256.0F;
	l->index += 4;
	MEM_FLT(l->data, l->index) = (float)t / 256.0F;
	l->index += 4;
}
static void p_vertex(struct tdl* l, int16_t x, int16_t y, int16_t z,
					 uint8_t light, uint8_t s, uint8_t t) {
	p_ensure(l);
	uint8_t* restrict p = (uint8_t*)l->data + l->index;
	MEM_FLT(p, 0) = (float)x / 256.0F;
	MEM_FLT(p, 4) = (float)y / 256.0F;
	MEM_FLT(p, 8) = (float)z / 256.0F;
	MEM_U8(p, 12) = light % 16;
	MEM_U8(p, 13) = light / 16;
	if(l->tinted) {
		MEM_U8(p, 14) = l->tint[0];
		MEM_U8(p, 15) = l->tint[1];
		MEM_U8(p, 16) = l->tint[2];
		MEM_FLT(p, 17) = (float)s / 256.0F;
		MEM_FLT(p, 21) = (float)t / 256.0F;
		l->index += 25;
	} else {
		MEM_FLT(p, 14) = (float)s / 256.0F;
		MEM_FLT(p, 18) = (float)t / 256.0F;
		l->index += 22;
	}
}

// A spread of vertices that exercises every byte: extreme S16 positions, the
// full 0..255 light/texcoord range, and a tint that changes per vertex (so a
// vertex that read a stale tint, or read it at the wrong time vs the triplet,
// would diverge).
struct vtx {
	int16_t x, y, z;
	uint8_t light, s, t;
};
static const struct vtx V[] = {
	{0, 0, 0, 0, 0, 0},
	{32767, -32768, 1, 255, 255, 255},
	{-1, 1, -32768, 15, 16, 240},
	{256, 4096, -256, 200, 1, 254},
	{12345, -6789, 4242, 7, 128, 64},
	{-32768, 32767, -32768, 240, 200, 8},
	{100, 200, 300, 33, 99, 17},
	{-12, 34, -56, 78, 90, 123},
};
#define NV ((int)(sizeof(V) / sizeof(V[0])))

/* How many vertices each identity run emits. The 8-vertex table above is
 * cycled to reach it. THIS NUMBER IS LOAD-BEARING since the ladder landed: the
 * smallest Wii rung is 608 B, so a run of only NV=8 vertices (<= 96 emitted
 * bytes) never grows at all and the "identical grow points" half of the proof
 * would be vacuous. 200 vertices is 2,400 tinted / 1,800 untinted bytes, which
 * climbs 608 -> 928 -> 1408 -> 2144 -> 3232 on the backbone (and past 4096 on
 * the PC doubling chain), so every run below crosses several rungs. */
#define NRUN 200
#define VTX(i) V[(i) % NV]

static void set_tint(struct tdl* l, int i) {
	l->tint[0] = (uint8_t)(i * 37 + 5);
	l->tint[1] = (uint8_t)(i * 11 + 200);
	l->tint[2] = (uint8_t)(255 - i * 23);
}

/* prior_index 0 -> plain init; otherwise the presized path. Emits NRUN
 * vertices so the run crosses several ladder rungs by growing. */
static int run_wii_from(bool tinted, size_t prior_index, int* grows_out) {
	struct tdl a, b;
	w_init_presized(&a, 4, 12, prior_index);
	w_init_presized(&b, 4, 12, prior_index);
	a.tinted = b.tinted = tinted;
	g_grows = 0;
	for(int i = 0; i < NRUN; i++) {
		set_tint(&a, i);
		set_tint(&b, i);
		w_pos(&a, VTX(i).x, VTX(i).y, VTX(i).z); // reference triplet
		w_color(&a, VTX(i).light);
		w_texcoord(&a, VTX(i).s, VTX(i).t);
		w_vertex(&b, VTX(i).x, VTX(i).y, VTX(i).z, VTX(i).light, VTX(i).s,
				 VTX(i).t); // opt
	}
	if(grows_out)
		*grows_out = g_grows;
	// Every capacity either run ever holds must be ON the ladder -- the whole
	// point of the change, and the property a stale mirror would have missed.
	assert(a.length == displaylist_rung(a.length));
	assert(b.length == displaylist_rung(b.length));
	assert(a.index == b.index);
	assert(a.length == b.length); // identical grow points
	// Emitted region only (the CLL+3 front pad is GX_NOP, written at finalize).
	int rc = memcmp((uint8_t*)a.data + CLL + 3, (uint8_t*)b.data + CLL + 3,
					a.index - (CLL + 3));
	assert(rc == 0);
	free(a.data);
	free(b.data);
	return (int)(a.index - (CLL + 3));
}

static int run_wii(bool tinted) {
	int grows = 0;
	int emitted = run_wii_from(tinted, 0, &grows);
	// The plain-init run MUST grow, or "identical grow points" proves nothing.
	assert(grows > 0);
	// And the presized run reproduces the identical bytes from a capacity the
	// ladder chose off a REMEMBERED index -- init, presize and grow rung code
	// all on one emitted stream. The hint lands on 1152, a DENSIFIED rung and
	// not a backbone one, on purpose: it is the one start from which the
	// backbone-only grow step is observable.
	int presized = run_wii_from(tinted, 1140, NULL);
	assert(presized == emitted);
	return emitted;
}

static int run_pc(bool tinted) {
	struct tdl a, b;
	p_init(&a);
	p_init(&b);
	a.tinted = b.tinted = tinted;
	for(int i = 0; i < NRUN; i++) {
		set_tint(&a, i);
		set_tint(&b, i);
		p_pos(&a, VTX(i).x, VTX(i).y, VTX(i).z);
		p_color(&a, VTX(i).light);
		p_texcoord(&a, VTX(i).s, VTX(i).t);
		p_vertex(&b, VTX(i).x, VTX(i).y, VTX(i).z, VTX(i).light, VTX(i).s,
				 VTX(i).t);
	}
	assert(a.index == b.index);
	assert(a.length == b.length);
	int rc = memcmp(a.data, b.data, a.index);
	assert(rc == 0);
	free(a.data);
	free(b.data);
	return (int)a.index;
}

/* ---- T1-b: DCStoreRange flush range (mirrors displaylist_finalize) -------
   OLD (before this lane): DCStoreRange(l->data, l->length + CLL) -- flushes
   the WHOLE buffer, including any unused grown/pre-sized tail.
   NEW: DCStoreRange(l->data, round_up(l->index, CLL)) -- flushes only the
   bytes GX_CallDispList can ever read; that CALL's own size argument is
   independently derived from l->index (displaylist_render, mirrored below
   as call_range()), never from l->length. Proves: (1) the new size always
   covers at least the CALL's actual read window [base_off, base_off+size);
   (2) it never flushes MORE than the old formula; (3) the two coincide
   exactly when there is no slack (nothing wasted in the common small-list
   case), and diverge sharply once there IS slack (a grown or T1-c pre-sized
   buffer) -- which is exactly the scenario this fix targets. */
static void call_range(size_t data_addr, size_t index, size_t* base_off,
					   size_t* call_size) {
	size_t m = data_addr % CLL; // always 0 in production: mem2_alloc is
								// 32-byte aligned (see mem2.c's
								// __lwp_heap_init(..., 32) and the
								// memalign(32, ...) fallback), mirrored here
								// for completeness against a misaligned base.
	*base_off = CLL - m;
	*call_size = (index + m - 1) / CLL * CLL;
}

static size_t old_flush_size(size_t length) { return length + CLL; }
static size_t new_flush_size(size_t index) {
	return (index + CLL - 1) / CLL * CLL;
}

static void run_t1b_dcstorerange_range(void) {
	struct {
		size_t index, length;
		const char* label;
	} cases[] = {
		{CLL + 3 + 9 * 4, CACHE_LINES(4, 9), "no slack (never grew)"},
		{CLL + 3 + 9 * 4, CACHE_LINES(4, 9) * 3, "grown, slack left over"},
		{CLL + 3 + 9 * 4, 4096, "T1-c pre-sized far ahead of this build"},
	};
	for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t old_sz = old_flush_size(cases[i].length);
		size_t new_sz = new_flush_size(cases[i].index);
		size_t base_off, call_sz;
		call_range(0, cases[i].index, &base_off, &call_sz);

		assert(new_sz == base_off + call_sz); // exactly the CALL's own window
		assert(new_sz <= old_sz);			   // never flushes more than before
		printf("test_displaylist_vertex_equiv: T1-b case \"%s\": index=%zu "
			   "length=%zu -> old flush %zu B, new flush %zu B (CALL window "
			   "%zu B)\n",
			   cases[i].label, cases[i].index, cases[i].length, old_sz, new_sz,
			   call_sz);
	}
	// The no-slack case must flush the SAME bytes either way (nothing to
	// save when there was never any grow slack to begin with).
	assert(old_flush_size(cases[0].length) == new_flush_size(cases[0].index));
	// The pre-sized case is where T1-b pays off the most: T1-c intentionally
	// leaves `length` far ahead of `index`, and T1-b keeps the flush pinned
	// to `index` regardless.
	assert(new_flush_size(cases[2].index) < old_flush_size(cases[2].length));
}

/* ---- T1-c: pre-size from the previous build's index ----------------------
   A same-shape rebuild (the mesher re-meshing an unchanged chunk -- a
   relight, an LOD-tier flip and back, a re-dirty that resolves to the same
   geometry) presized from the prior build's final index must emit
   byte-identical output while triggering ZERO grows, and prior_index == 0
   (no history) must reproduce plain w_init's initial size exactly. */
static void run_t1c_presize_zero_grow(void) {
	struct tdl a;
	w_init(&a, 4, 12);
	a.tinted = false;
	g_grows = 0;
	for(int i = 0; i < NRUN; i++) {
		set_tint(&a, i); // harmless (untinted): exercises the same call shape
		w_vertex(&a, VTX(i).x, VTX(i).y, VTX(i).z, VTX(i).light, VTX(i).s,
				 VTX(i).t);
	}
	int baseline_grows = g_grows;
	assert(baseline_grows > 0); // the smallest ladder rung must NOT cover all
								// NRUN vertices, or this proof is vacuous
	size_t final_index = a.index, final_length = a.length;
	size_t emitted = final_index - (CLL + 3);
	uint8_t* first_bytes = malloc(emitted);
	memcpy(first_bytes, (uint8_t*)a.data + CLL + 3, emitted);
	free(a.data);

	// Rebuild the identical shape, presized from the first build's final
	// index -- exactly what chunk_mesher_send snapshots from c->mesh[k].index
	// before dispatching a rebuild.
	struct tdl b;
	w_init_presized(&b, 4, 12, final_index);
	b.tinted = false;
	g_grows = 0;
	for(int i = 0; i < NRUN; i++) {
		set_tint(&b, i);
		w_vertex(&b, VTX(i).x, VTX(i).y, VTX(i).z, VTX(i).light, VTX(i).s,
				 VTX(i).t);
	}
	assert(g_grows == 0); // pre-sized from history -> zero grows
	// LADDER: the presized capacity is a rung, and never above the capacity
	// the first build ended on -- the rebuild asks for the size of the block
	// it is reusing, which is what makes the refit exact instead of a
	// first-fit split leaving a 32 B crumb.
	assert(b.length == displaylist_rung(b.length) && b.length <= final_length);
	assert(b.index == final_index);
	assert(memcmp((uint8_t*)b.data + CLL + 3, first_bytes, emitted) == 0);
	free(b.data);
	free(first_bytes);

	// No-history fallback (prior_index == 0) must be IDENTICAL to plain
	// w_init -- today's initial size stays reachable.
	struct tdl c1, c2;
	w_init(&c1, 4, 12);
	w_init_presized(&c2, 4, 12, 0);
	assert(c1.length == c2.length);
	assert(c1.index == c2.index);

	printf("test_displaylist_vertex_equiv: T1-c pre-size-from-history -- "
		   "baseline rebuild took %d grow(s) over %d vertices; presized from "
		   "that build's final index, an identical rebuild takes 0 grows and "
		   "emits byte-identical output; prior_index=0 falls back to today's "
		   "initial size exactly\n",
		   baseline_grows, NRUN);
}

int main(void) {
	test_wii_finalize_equiv();
	test_pc_finalize_equiv();
	int wp = run_wii(false), wt = run_wii(true);
	int pp = run_pc(false), pt = run_pc(true);
	// Sanity on the byte counts: Wii 9/12 B/vertex, PC 22/25 B/vertex.
	assert(wp == NRUN * 9 && wt == NRUN * 12);
	assert(pp == NRUN * 22 && pt == NRUN * 25);
	printf("test_displaylist_vertex_equiv: all assertions passed "
		   "(merged vertex == pos+color+texcoord, byte-for-byte + identical "
		   "index/length/grow; Wii %d/%d B, PC %d/%d B over %d vertices, tint "
		   "varied per vertex, init + presized + multi-rung grow)\n",
		   wp, wt, pp, pt, NRUN);

	run_t1b_dcstorerange_range();
	printf("test_displaylist_vertex_equiv: T1-b assertions passed\n");

	run_t1c_presize_zero_grow();
	printf("test_displaylist_vertex_equiv: T1-c assertions passed\n");
	return 0;
}
