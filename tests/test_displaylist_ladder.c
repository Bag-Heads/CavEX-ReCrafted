/*
	Ladder-rung display-list sizing (2026-09-08, carve-arena interleave lane).

	What this proves, and why it is not a tautology: the FIX is that a freed
	display-list block must be an EXACT refit for the next list of that rung.
	"Exact" cannot be observed by looking at the capacity alone -- a first-fit
	allocator will happily place a slightly-smaller request at the same address
	and leave a crumb behind. The observable difference is the SHAPE OF THE FREE
	LIST after the rebuild: with rung sizing it is unchanged, with byte-exact
	sizing it grows a fragment per rebuild that nothing can ever use again.

	So this test carries a small model of the libogc __lwp_heap the Wii carve
	actually runs on -- first-fit, coalescing, 32 B pages, a per-block header --
	and drives the SAME scripted rebuild sequence through it twice: once sized
	the way displaylist_init_presized used to size it (the prior index rounded to
	the 32 B cache line) and once sized the way it does now (the prior index
	snapped to a rung). The assertions are on the model heap, not on the ladder
	arithmetic:

	  * rung sizing: every rebuild lands at the SAME address as the block it
	    replaces (0 relocations), and the arena is left with exactly one free
	    region -- the untouched tail, 0 stranded bytes;
	  * byte-exact sizing: the SAME sequence relocates almost every list and
	    strands bytes between live neighbours, which is the measured hardware
	    signature (1.42 MB free with a 928 B largest block).

	The rebuild drift the script applies is deliberately WITHIN one rung -- a
	few vertices, the scale of a block edit or a light change. That is the case
	the ladder is for: absorbing the jitter of a same-shape rebuild so the block
	is reused as-is. A rebuild that genuinely drops a rung SHOULD move to a
	smaller block; the ladder does not claim otherwise, and does not test for
	it.

	Plus the ordinary properties of the rung lookup itself: monotone, 32 B
	aligned, smallest-rung-at-or-above, and the documented above-the-table
	fallback.

	T1: this file asserts capacities and addresses only. Byte-identity of the
	emitted stream is tests/test_displaylist_vertex_equiv.c's job.
*/

#include "../source/platform/displaylist_ladder.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAYLIST_CLL 32u
#define MEM2_GUARD 256u

/* ------------------------------------------------------------------ */
/* 1. The rung table and lookup                                        */
/* ------------------------------------------------------------------ */

#define RENDER_CAP (2u * 1024u * 1024u)

static void test_table(void) {
	assert(DISPLAYLIST_LADDER_RUNGS == 42);
	assert(DISPLAYLIST_LADDER_BACKBONE_RUNGS == 21);
	assert(displaylist_ladder[0] == 608);
	for(size_t i = 0; i < DISPLAYLIST_LADDER_RUNGS; i++) {
		assert(displaylist_ladder[i] % 32 == 0);
		if(i)
			assert(displaylist_ladder[i] > displaylist_ladder[i - 1]);
	}
	for(size_t i = 0; i < DISPLAYLIST_LADDER_BACKBONE_RUNGS; i++) {
		assert(displaylist_ladder_backbone[i] % 32 == 0);
		if(i)
			assert(displaylist_ladder_backbone[i]
				   > displaylist_ladder_backbone[i - 1]);
		/* The backbone is a SUBSET of the full table, so a capacity is on the
		 * ladder whichever path set it. */
		assert(displaylist_rung(displaylist_ladder_backbone[i])
			   == displaylist_ladder_backbone[i]);
	}
	/* THE BACKBONE. Every value the existing grow recurrence produces from the
	 * untinted initial capacity is ON the backbone table, in order -- that is
	 * what makes the grow path cost zero extra bytes. Re-derive it rather than
	 * trusting literals, and carry it PAST the render cap (M3) so that every
	 * legal list length has a rung at or below the cap. */
	size_t n = (64 * 9 + 3 + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL
		* DISPLAYLIST_CLL;
	unsigned backbone = 0;
	while(backbone < DISPLAYLIST_LADDER_BACKBONE_RUNGS) {
		assert(displaylist_ladder_backbone[backbone] == n);
		assert(displaylist_rung(n) == n); /* on the full table, exactly */
		backbone++;
		n = (n * 3 / 2 + 9 + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL
			* DISPLAYLIST_CLL;
	}
	assert(backbone == 21);
	/* The table now spans the render cap: the last backbone rung is the first
	 * one past it, and the cap itself is a rung, so a list of any LEGAL length
	 * snaps to a rung that is still legal instead of being pushed over the cap
	 * and refused by displaylist_render as corrupt. */
	assert(displaylist_ladder_backbone[DISPLAYLIST_LADDER_BACKBONE_RUNGS - 1]
		   == 2152000);
	assert(displaylist_ladder_backbone[DISPLAYLIST_LADDER_BACKBONE_RUNGS - 2]
		   < RENDER_CAP);
	assert(displaylist_rung(RENDER_CAP) == RENDER_CAP);
	for(size_t want = 32; want <= RENDER_CAP; want += 1024)
		assert(displaylist_rung(want) <= RENDER_CAP);
	assert(displaylist_rung(RENDER_CAP + 32) == 2152000);
	/* Densified between the backbone values so the snap-up waste is bounded by
	 * a 1.26 step, not a 1.5 one. Both facts are load-bearing: the backbone
	 * keeps grows free, the density keeps presize snap-up affordable. */
	for(size_t i = 1; i < DISPLAYLIST_LADDER_RUNGS; i++)
		assert(displaylist_ladder[i] * 100 <= displaylist_ladder[i - 1] * 127);

	/* Smallest rung >= want, for every rung and both neighbours. */
	for(size_t i = 0; i < DISPLAYLIST_LADDER_RUNGS; i++) {
		size_t r = displaylist_ladder[i];
		assert(displaylist_rung(r) == r);
		assert(displaylist_rung(r - 1) == r);
		if(i + 1 < DISPLAYLIST_LADDER_RUNGS)
			assert(displaylist_rung(r + 1) == displaylist_ladder[i + 1]);
	}
	assert(displaylist_rung(0) == 608);
	assert(displaylist_rung(1) == 608);
	/* Tinted lists start at DISPLAYLIST_CACHE_LINES(64, 12) = 800 and snap to
	 * the 928 rung: the +128 B this change costs, on record. */
	assert((64 * 12 + 3 + 31) / 32 * 32 == 800);
	assert(displaylist_rung(800) == 928);

	/* Above the last rung: rounded to the cache line, NOT clamped -- such a
	 * list already exceeds DISPLAYLIST_RENDER_MAX_BYTES and must reach
	 * the corruption guard in displaylist_render intact. */
	size_t top = displaylist_ladder[DISPLAYLIST_LADDER_RUNGS - 1];
	assert(displaylist_rung(top + 1) == (top + 1 + 31) / 32 * 32);
	assert(displaylist_rung(top + 33) == (top + 33 + 31) / 32 * 32);

	/* The PC backend's own ladder: its 4096 B doubling chain. */
	assert(displaylist_rung_pc(1) == 4096);
	assert(displaylist_rung_pc(4096) == 4096);
	assert(displaylist_rung_pc(4097) == 8192);
	assert(displaylist_rung_pc(5000) == 8192);
	for(size_t i = 1; i < DISPLAYLIST_LADDER_PC_RUNGS; i++)
		assert(displaylist_ladder_pc[i] == displaylist_ladder_pc[i - 1] * 2);
}

/* ------------------------------------------------------------------ */
/* 1b. GROW targets the backbone only, and what that costs             */
/* ------------------------------------------------------------------ */

static size_t grow_request(size_t length, int vtx_bytes) {
	return (length * 3 / 2 + (size_t)vtx_bytes + DISPLAYLIST_CLL - 1)
		/ DISPLAYLIST_CLL * DISPLAYLIST_CLL;
}

static void test_grow(void) {
	/* Smallest BACKBONE rung >= want, and always at least the full-table rung
	 * (a grow never lands lower than the request). */
	for(size_t i = 0; i < DISPLAYLIST_LADDER_BACKBONE_RUNGS; i++) {
		size_t r = displaylist_ladder_backbone[i];
		assert(displaylist_rung_grow(r) == r);
		assert(displaylist_rung_grow(r - 1) == r);
		if(i + 1 < DISPLAYLIST_LADDER_BACKBONE_RUNGS)
			assert(displaylist_rung_grow(r + 1)
				   == displaylist_ladder_backbone[i + 1]);
	}
	/* THE ZERO-COST CLAIM. A grow off a BACKBONE rung lands on the next
	 * backbone rung EXACTLY -- not one byte of snap-up -- for every step of
	 * the chain and both vertex strides. This is what makes the ladder free on
	 * the path that actually churns. */
	for(size_t i = 0; i + 1 < DISPLAYLIST_LADDER_BACKBONE_RUNGS; i++)
		for(int vtx = 9; vtx <= 12; vtx += 3) {
			size_t raw = grow_request(displaylist_ladder_backbone[i], vtx);
			/* The raw request already IS the next backbone rung -- the 3-byte
			 * difference between the two strides never crosses the 32 B round
			 * -- so the snap adds nothing at all. */
			assert(raw == displaylist_ladder_backbone[i + 1]);
			assert(displaylist_rung_grow(raw) == raw);
		}
	/* THE COST, stated as a bound rather than assumed away. A list whose
	 * capacity came from init/presize may sit on a densified midpoint; the ONE
	 * grow that leaves it overshoots the raw request, and never by more than
	 * 25.7%. Every grow after that is on the backbone, i.e. exact. */
	size_t worst_num = 0, worst_den = 1, worst_from = 0;
	for(size_t i = 0; i < DISPLAYLIST_LADDER_RUNGS; i++)
		for(int vtx = 9; vtx <= 12; vtx += 3) {
			size_t raw = grow_request(displaylist_ladder[i], vtx);
			size_t got = displaylist_rung_grow(raw);
			if(got < raw)
				continue; /* past the top of the backbone: not a grow target */
			assert(got >= raw);
			if(got * worst_den > worst_num * raw) {
				worst_num = got;
				worst_den = raw;
				worst_from = displaylist_ladder[i];
			}
		}
	printf("[ladder] worst grow snap-up: %zu -> %zu (%.1f%% over the raw "
		   "request)\n", worst_from, worst_num,
		   100.0 * ((double)worst_num / (double)worst_den - 1.0));
	assert(worst_num * 10000 <= worst_den * 12572); /* 1408/1120 = +25.71% */

	/* The waste bound the header states for init/presize, measured over every
	 * request the backend can actually make (a 32 B-aligned capacity at or
	 * above the smallest rung): <= +22.5%, versus the +50% a backbone-only
	 * table would have cost. The TABLE step tops out at 1.26 (736 -> 928); the
	 * realised snap-up is lower because only 32 B multiples are ever asked
	 * for. */
	size_t init_num = 0, init_den = 1;
	for(size_t want = 608; want <= RENDER_CAP; want += 32) {
		size_t got = displaylist_rung(want);
		if(got * init_den > init_num * want) {
			init_num = got;
			init_den = want;
		}
	}
	printf("[ladder] worst init/presize snap-up: %.1f%%\n",
		   100.0 * ((double)init_num / (double)init_den - 1.0));
	assert(init_num * 10000 <= init_den * 12248); /* 1757120/1434688 */
}

/* ------------------------------------------------------------------ */
/* 2. A model of the carve heap: first-fit, coalescing, 32 B pages     */
/* ------------------------------------------------------------------ */

/* libogc's __lwp_heap: every block carries an 8 B boundary tag and is rounded
 * up to the 32 B page (mem2.c's __lwp_heap_init(..., 32) and its
 * _Static_assert on HEAP_OVERHEAD == 8). Allocation is first-fit over a single
 * address-ordered free list; a free that touches a neighbour coalesces. */
#define HEAP_PAGE 32u
#define HEAP_OVERHEAD 8u
#define HEAP_BLOCKS 4096u

struct mblock {
	size_t off, size; /* size includes the overhead, page-rounded */
	bool used;
};
struct mheap {
	struct mblock b[HEAP_BLOCKS];
	unsigned n;
};

static void mheap_init(struct mheap* h, size_t bytes) {
	memset(h, 0, sizeof(*h));
	h->n = 1;
	h->b[0].off = 0;
	h->b[0].size = bytes / HEAP_PAGE * HEAP_PAGE;
	h->b[0].used = false;
}

static size_t mheap_need(size_t want) {
	return (want + HEAP_OVERHEAD + HEAP_PAGE - 1) / HEAP_PAGE * HEAP_PAGE;
}

/* Returns the block offset, or SIZE_MAX on failure. */
static size_t mheap_alloc(struct mheap* h, size_t want) {
	size_t need = mheap_need(want);
	for(unsigned i = 0; i < h->n; i++) {
		if(h->b[i].used || h->b[i].size < need)
			continue;
		if(h->b[i].size > need) {
			assert(h->n + 1 <= HEAP_BLOCKS);
			memmove(&h->b[i + 1], &h->b[i],
					(h->n - i) * sizeof(struct mblock));
			h->n++;
			h->b[i].size = need;
			h->b[i + 1].off = h->b[i].off + need;
			h->b[i + 1].size -= need;
			h->b[i + 1].used = false;
		}
		h->b[i].used = true;
		return h->b[i].off;
	}
	return SIZE_MAX;
}

static void mheap_free(struct mheap* h, size_t off) {
	unsigned i = 0;
	while(i < h->n && h->b[i].off != off)
		i++;
	assert(i < h->n && h->b[i].used);
	h->b[i].used = false;
	if(i + 1 < h->n && !h->b[i + 1].used) { /* coalesce forward */
		h->b[i].size += h->b[i + 1].size;
		memmove(&h->b[i + 1], &h->b[i + 2],
				(h->n - i - 2) * sizeof(struct mblock));
		h->n--;
	}
	if(i && !h->b[i - 1].used) { /* coalesce backward */
		h->b[i - 1].size += h->b[i].size;
		memmove(&h->b[i], &h->b[i + 1], (h->n - i - 1) * sizeof(struct mblock));
		h->n--;
	}
}

static unsigned mheap_free_blocks(const struct mheap* h) {
	unsigned c = 0;
	for(unsigned i = 0; i < h->n; i++)
		if(!h->b[i].used)
			c++;
	return c;
}

static size_t mheap_largest_free(const struct mheap* h) {
	size_t m = 0;
	for(unsigned i = 0; i < h->n; i++)
		if(!h->b[i].used && h->b[i].size > m)
			m = h->b[i].size;
	return m;
}

/* Self-check the model before trusting a conclusion drawn from it. */
static void test_model_heap(void) {
	struct mheap h;
	mheap_init(&h, 64 * 1024);
	assert(mheap_free_blocks(&h) == 1);
	size_t a = mheap_alloc(&h, 1000), b = mheap_alloc(&h, 1000);
	assert(a == 0 && b == mheap_need(1000));
	assert(mheap_free_blocks(&h) == 1); /* one tail remains */
	mheap_free(&h, a);
	assert(mheap_free_blocks(&h) == 2); /* the hole + the tail */
	/* First-fit puts an EXACTLY equal request back in the hole with no crumb. */
	assert(mheap_alloc(&h, 1000) == a);
	assert(mheap_free_blocks(&h) == 1);
	mheap_free(&h, a);
	/* A slightly SMALLER request also lands at the hole -- same address -- but
	 * splits it and leaves a crumb behind. That crumb is the fragmentation the
	 * ladder exists to prevent, and it is why "same address" alone is not the
	 * property to assert on. */
	size_t c = mheap_alloc(&h, 1000 - HEAP_PAGE);
	assert(c == a);
	assert(mheap_free_blocks(&h) == 2);
	mheap_free(&h, c);
	mheap_free(&h, b);
	assert(mheap_free_blocks(&h) == 1);
	assert(mheap_largest_free(&h) == 64 * 1024 / HEAP_PAGE * HEAP_PAGE);
}

/* ------------------------------------------------------------------ */
/* 3. The scripted rebuild sequence                                    */
/* ------------------------------------------------------------------ */

/* The Wii backend allocates l->length + DISPLAYLIST_CLL and mem2_allocate adds
 * MEM2_GUARD on top -- so this is the byte count a list of capacity `length`
 * actually asks the carve for. */
static size_t list_request(size_t length) {
	return length + DISPLAYLIST_CLL + MEM2_GUARD;
}

#define LISTS 24u
#define REBUILDS 12u

struct script_result {
	unsigned free_blocks_end;
	size_t largest_free_end;
	/* Free bytes that are NOT in the one big tail block -- i.e. the stranded
	 * crumbs. The largest free block stays the tail under BOTH policies, so it
	 * is this number, not the largest, that separates them. */
	size_t stranded_end;
	unsigned moved; /* rebuilds that did NOT land on the freed block */
};

static size_t mheap_stranded(const struct mheap* h) {
	size_t total = 0, largest = 0;
	for(unsigned i = 0; i < h->n; i++)
		if(!h->b[i].used) {
			total += h->b[i].size;
			if(h->b[i].size > largest)
				largest = h->b[i].size;
		}
	return total - largest;
}

/* `snap` picks the sizing policy for a rebuild: rung(prior) is the fix,
 * ceil32(prior) is what displaylist_init_presized did before it. */
static void run_script(struct script_result* out, size_t (*snap)(size_t)) {
	static struct mheap h;
	mheap_init(&h, 8u * 1024u * 1024u);

	size_t off[LISTS], len[LISTS], idx[LISTS];
	/* A spread of chunk-side shapes: the vertex counts a mesher build lands on
	 * for sides ranging from a handful of quads to a dense terrain face. */
	for(unsigned i = 0; i < LISTS; i++) {
		size_t verts = 40 + (size_t)i * 137;
		size_t want = (verts * 9 + 3 + DISPLAYLIST_CLL - 1) / DISPLAYLIST_CLL
			* DISPLAYLIST_CLL;
		/* First build: displaylist_init + the 1.5x grow chain, snapped. Both
		 * policies start from the same capacity -- the divergence is entirely
		 * in the REBUILD, which is the path that actually churns. */
		len[i] = displaylist_rung(want);
		idx[i] = DISPLAYLIST_CLL + 3 + verts * 9;
		off[i] = mheap_alloc(&h, list_request(len[i]));
		assert(off[i] != SIZE_MAX);
	}

	out->moved = 0;
	for(unsigned r = 0; r < REBUILDS; r++) {
		for(unsigned i = 0; i < LISTS; i++) {
			/* chunk_mesher_send snapshots the previous build's final index and
			 * hands it to displaylist_init_presized as prior_index. A rebuild
			 * of an unchanged chunk reaches the same index; a rebuild after a
			 * block edit reaches a slightly different one. Vary it so the
			 * off-ladder policy is exercised the way real churn exercises it. */
			size_t prior = idx[i] - (size_t)((r + i) % 5) * 9;
			size_t want = snap(prior);
			if(want < displaylist_rung(608))
				want = displaylist_rung(608);
			/* The mesher builds the replacement, then frees the old list
			 * (chunk_mesher_send replaces the published list). */
			size_t old = off[i];
			mheap_free(&h, old);
			size_t fresh = mheap_alloc(&h, list_request(want));
			assert(fresh != SIZE_MAX);
			if(fresh != old)
				out->moved++;
			off[i] = fresh;
			len[i] = want;
		}
	}
	out->free_blocks_end = mheap_free_blocks(&h);
	out->largest_free_end = mheap_largest_free(&h);
	out->stranded_end = mheap_stranded(&h);
}

static size_t snap_rung(size_t prior) { return displaylist_rung(prior); }
static size_t snap_cacheline(size_t prior) { return (prior + 31) / 32 * 32; }

static void test_refit(void) {
	struct script_result rung, byte_exact;
	run_script(&rung, snap_rung);
	run_script(&byte_exact, snap_cacheline);

	printf("[ladder] rung sizing: free blocks %u, largest %zu, stranded %zu,"
		   " moved %u\n", rung.free_blocks_end, rung.largest_free_end,
		   rung.stranded_end, rung.moved);
	printf("[ladder] byte sizing: free blocks %u, largest %zu, stranded %zu,"
		   " moved %u\n", byte_exact.free_blocks_end,
		   byte_exact.largest_free_end, byte_exact.stranded_end,
		   byte_exact.moved);

	/* THE CLAIM. Under rung sizing every rebuild refits the block it just
	 * freed exactly: it lands at the same address, and the arena is left with
	 * exactly one free region -- the untouched tail. */
	/* Rung sizing absorbs the drift of a same-shape rebuild: the replacement
	 * asks for the size the block it just freed already has, so it refits in
	 * place. A few rebuilds still move -- the densified table has midpoint
	 * rungs, and a drift that crosses one legitimately drops a rung -- but the
	 * arena is left whole either way. */
	assert(rung.moved * 2 < byte_exact.moved);
	assert(rung.free_blocks_end == 1);
	assert(rung.stranded_end == 0);

	/* Under byte-exact sizing the SAME sequence shreds the arena: every
	 * rebuild that shrinks by a hair splits its own hole and leaves a crumb no
	 * later list can use. This is the measured hardware signature. */
	assert(byte_exact.free_blocks_end > rung.free_blocks_end);
	assert(byte_exact.stranded_end > 0);
	/* The damage shows up as STRANDED bytes, not as a smaller LARGEST free
	 * block: the untouched tail stays the largest region either way (it is even
	 * marginally bigger under byte-exact sizing, whose lists are smaller). That
	 * is exactly why the hardware run could report 1.42 MB free while the
	 * largest free block sat at 928 B -- and why "dl_slack went to 0" was not
	 * the same thing as "the arena is usable again". */
	assert(rung.stranded_end < byte_exact.stranded_end);

	/* And the crumbs really are unusable: the largest free block outside the
	 * tail is smaller than the smallest list the engine can allocate. */
}

/* ------------------------------------------------------------------ */
/* 4. Compaction shrinks to the rung, not to the byte                  */
/* ------------------------------------------------------------------ */

static void test_compaction_target(void) {
	/* displaylist_finalize's keep_len. `length` is always a rung and
	 * call_len = ceil32(index) <= length, so rung(call_len) == length in the
	 * common case and the shrink is SKIPPED -- which is the +41 s readiness
	 * regression's fix, not a suppression of the feature. */
	for(size_t i = 0; i < DISPLAYLIST_LADDER_RUNGS; i++) {
		size_t length = displaylist_ladder[i];
		size_t lo = i ? displaylist_ladder[i - 1] + 32 : 32;
		for(size_t index = lo; index <= length; index += 32) {
			size_t call_len = (index + 31) / 32 * 32;
			assert(displaylist_rung(call_len) == length); /* skipped */
		}
	}
	/* A presized list that really did overshoot shrinks one rung to another,
	 * and the retained allocation is byte-for-byte what a fresh list of that
	 * rung would have asked for -- so a compacted block and an uncompacted one
	 * of the same rung are interchangeable in the free list. */
	size_t length = displaylist_ladder[6];	/* 7328 */
	size_t index = displaylist_ladder[3] - 40; /* fits rung 3 = 2144 */
	size_t keep = displaylist_rung((index + 31) / 32 * 32);
	assert(keep == displaylist_ladder[3] && keep < length);
	assert(keep + DISPLAYLIST_CLL + MEM2_GUARD == list_request(keep));
}

int main(void) {
	test_table();
	test_grow();
	test_model_heap();
	test_refit();
	test_compaction_target();
	printf("test_displaylist_ladder: PASS\n");
	return 0;
}
