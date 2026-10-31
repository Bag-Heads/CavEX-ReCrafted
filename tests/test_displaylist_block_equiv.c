/*
	M2 mesh blocks (docs/p2_storage_ruling_fable_2026-09-09.md, "Memory plan v2"):
	T1 equivalence proof for the Wii display-list emitter in BLOCK MODE.

	Compiles the REAL source/platform/wii/displaylist.c on the host (the same
	trick tests/support/displaylist_finalize_wii.c uses) over a malloc-backed
	mem2 model with injectable failure, and proves:

	  1. For every list kind (untinted / tinted, tiny / large, with and without
	     a presize hint) the GX command stream bytes [0, call_len) emitted in
	     block mode are BYTE-IDENTICAL to the legacy carve path, and call_len
	     (the GX_CallDispList length) is identical.
	  2. Block mode performs ZERO carve allocations during emission (the lists
	     grow inside the scratch arena) and exactly ONE carve allocation per
	     generation of exactly sum(call_len) bytes; the legacy path performs
	     one carve allocation per list plus a realloc per grow.
	  3. destroy on a block-owned list frees nothing; the block is freed once.
	  4. Pack failure (carve OOM) leaves every list destroyable with no carve
	     frees and no double free; the next build reuses the scratch.
	  5. Scratch overflow falls back to the carve for that list only and the
	     bytes still match.
*/
#ifdef PLATFORM_PC
#undef PLATFORM_PC
#endif
#define PLATFORM_WII
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- host model of the carve allocator ---- */
static unsigned s_allocs, s_frees, s_reallocs, s_fail_after;
static size_t s_live_bytes;
#define MODEL_MAX 4096
static struct { void* p; size_t n; } s_live[MODEL_MAX];

static void model_reset(void) {
	s_allocs = s_frees = s_reallocs = 0;
	s_fail_after = 0;
	s_live_bytes = 0;
	memset(s_live, 0, sizeof s_live);
}
static void model_note(void* p, size_t n) {
	for(unsigned i = 0; i < MODEL_MAX; i++)
		if(!s_live[i].p) { s_live[i].p = p; s_live[i].n = n; s_live_bytes += n; return; }
	assert(!"model full");
}
static void model_forget(void* p, size_t n) {
	for(unsigned i = 0; i < MODEL_MAX; i++)
		if(s_live[i].p == p) {
			assert(s_live[i].n == n && "free size must match the allocation");
			s_live[i].p = NULL;
			s_live_bytes -= n;
			return;
		}
	assert(!"free of an unknown pointer (double free or scratch pointer)");
}
void* mem2_alloc(size_t n) {
	if(s_fail_after && s_allocs + 1 >= s_fail_after)
		return NULL;
	s_allocs++;
	void* p = NULL;
#ifdef _WIN32
	p = _aligned_malloc(n, 32);
#else
	if(posix_memalign(&p, 32, n)) p = NULL;
#endif
	assert(p);
	model_note(p, n);
	return p;
}
void mem2_free_sized(void* p, size_t n) {
	if(!p) return;
	s_frees++;
	model_forget(p, n);
#ifdef _WIN32
	_aligned_free(p);
#else
	free(p);
#endif
}
void* mem2_realloc(void* p, size_t old, size_t n) {
	s_reallocs++;
	void* q = mem2_alloc(n);
	if(!q) return NULL;
	memcpy(q, p, old < n ? old : n);
	mem2_free_sized(p, old);
	return q;
}
void* mem2_shrink(void* p, size_t old, size_t n) { (void)p; (void)old; (void)n; return NULL; }

#include "support/gx_finalize/wii_names.h"
#include "../source/platform/wii/displaylist.c"
carve_callback mem2_carve_observe;

#define CLL 32

/* Deterministic synthetic geometry: `count` quads (4 vertices each). */
static void emit(struct displaylist* l, unsigned count, bool tinted, unsigned seed) {
	for(unsigned q = 0; q < count * 4; q++) {
		unsigned r = (q * 2654435761u + seed) ;
		int16_t x = (int16_t)(r & 0x7fff), y = (int16_t)((r >> 3) & 0x7fff),
				z = (int16_t)((r >> 7) & 0x7fff);
		uint8_t light = (uint8_t)(r >> 11), s = (uint8_t)(r >> 13), t = (uint8_t)(r >> 17);
		if(tinted) {
			displaylist_pos(l, x, y, z);
			displaylist_color(l, light);
			displaylist_texcoord(l, s, t);
		} else {
			displaylist_vertex(l, x, y, z, light, s, t);
		}
	}
}

struct kind { unsigned quads; bool tinted; size_t hint; };
static const struct kind kinds[] = {
	{1, false, 0}, {1, true, 0}, {13, false, 0}, {13, true, 0},
	{200, false, 0}, {200, true, 0}, {300, false, 0}, {300, true, 0},
	{50, false, 4096}, {50, true, 96}, {0, false, 0}, {0, true, 0},
};
#define NKINDS (sizeof kinds / sizeof *kinds)
#define NLISTS 26

static void build_generation(struct displaylist* lists, bool* has, bool block, unsigned seed,
							 void** blk, size_t* blk_bytes) {
	if(block) displaylist_scratch_begin();
	for(unsigned k = 0; k < NLISTS; k++) {
		const struct kind* kd = &kinds[k % NKINDS];
		size_t vsz = kd->tinted ? 12 : 9;
		displaylist_init_presized(&lists[k], 64, vsz, kd->tinted, kd->hint);
		displaylist_set_tinted(&lists[k], kd->tinted);
		if(block) displaylist_allow_scratch(&lists[k]);
		emit(&lists[k], kd->quads, kd->tinted, seed + k);
		has[k] = kd->quads > 0 && lists[k].data && !lists[k].truncated;
		if(has[k])
			displaylist_finalize(&lists[k], (uint16_t)(kd->quads * 4));
		else
			displaylist_destroy(&lists[k]);
	}
	*blk = NULL; *blk_bytes = 0;
	if(block) {
		bool ok = displaylist_pack_block(lists, has, NLISTS, blk, blk_bytes);
		assert(ok && "pack must succeed with the model allocator");
		displaylist_scratch_end();
	}
}

static size_t call_len_of(const struct displaylist* l) {
	return (l->index + CLL - 1) / CLL * CLL;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	/* ---- 1 + 2: byte identity and allocation shape ---- */
	struct displaylist legacy[NLISTS], blockl[NLISTS];
	bool has_l[NLISTS], has_b[NLISTS];
	void* blk = NULL; size_t blk_bytes = 0;

	model_reset();
	displaylist_set_block_mode(false);
	void* dummy; size_t dummy_n;
	build_generation(legacy, has_l, false, 7, &dummy, &dummy_n);
	unsigned legacy_allocs = s_allocs, legacy_reallocs = s_reallocs;

	displaylist_set_block_mode(true);
	build_generation(blockl, has_b, true, 7, &blk, &blk_bytes);
	unsigned block_allocs = s_allocs - legacy_allocs,
			 block_reallocs = s_reallocs - legacy_reallocs;

	size_t sum_call_len = 0;
	for(unsigned k = 0; k < NLISTS; k++) {
		assert(has_l[k] == has_b[k]);
		if(!has_l[k]) continue;
		assert(blockl[k].block_owned && !legacy[k].block_owned);
		size_t cl = call_len_of(&legacy[k]);
		assert(cl == call_len_of(&blockl[k]));
		assert(blockl[k].length == cl && "block-owned length is the exact call_len");
		assert(!memcmp(legacy[k].data, blockl[k].data, cl)
			   && "GX stream bytes must be identical in block mode");
		assert((uint8_t*)blockl[k].data >= (uint8_t*)blk
			   && (uint8_t*)blockl[k].data + cl <= (uint8_t*)blk + blk_bytes);
		assert(((uintptr_t)blockl[k].data % CLL) == 0);
		sum_call_len += cl;
	}
	assert(blk && blk_bytes == sum_call_len && "one block of exactly sum(call_len)");
	assert(block_allocs == 1 && block_reallocs == 0
		   && "block mode: exactly one carve allocation per generation, no reallocs");
	{
		uint32_t ov;
		displaylist_block_counters(NULL, &ov, NULL);
		assert(ov == 0 && "a whole generation of unhinted lists fits the scratch");
	}
	assert(legacy_allocs >= NLISTS - 2 && legacy_reallocs > 0
		   && "legacy: one carve allocation per list plus grows");
	printf("PASS 1/2: %u lists byte-identical; block mode 1 carve alloc (%zu B) vs legacy %u allocs + %u reallocs\n",
		   NLISTS, blk_bytes, legacy_allocs, legacy_reallocs);

	/* ---- 3: destroy frees nothing for block-owned lists; block freed once ---- */
	unsigned frees_before = s_frees;
	for(unsigned k = 0; k < NLISTS; k++)
		if(has_b[k]) displaylist_destroy(&blockl[k]);
	assert(s_frees == frees_before && "destroy of block-owned lists frees nothing");
	displaylist_block_free(blk, blk_bytes);
	assert(s_frees == frees_before + 1);
	for(unsigned k = 0; k < NLISTS; k++)
		if(has_l[k]) displaylist_destroy(&legacy[k]);
	assert(s_live_bytes == 0 && "everything the model handed out came back");
	printf("PASS 3: block-owned destroy is a no-op; the block is freed exactly once\n");

	/* ---- 4: pack failure = carve OOM; lists stay destroyable, no leaks ---- */
	model_reset();
	s_fail_after = 1; /* the very first carve allocation (the block) fails */
	displaylist_scratch_begin();
	for(unsigned k = 0; k < NLISTS; k++) {
		const struct kind* kd = &kinds[k % NKINDS];
		displaylist_init_presized(&blockl[k], 64, kd->tinted ? 12 : 9, kd->tinted, 0);
		displaylist_set_tinted(&blockl[k], kd->tinted);
		displaylist_allow_scratch(&blockl[k]);
		emit(&blockl[k], kd->quads, kd->tinted, 99 + k);
		has_b[k] = kd->quads > 0 && blockl[k].data && !blockl[k].truncated;
		if(has_b[k]) displaylist_finalize(&blockl[k], (uint16_t)(kd->quads * 4));
		else displaylist_destroy(&blockl[k]);
	}
	assert(s_allocs == 0 && "emission touched the carve (scratch not used)");
	bool ok = displaylist_pack_block(blockl, has_b, NLISTS, &blk, &blk_bytes);
	assert(!ok && !blk && blk_bytes == 0);
	for(unsigned k = 0; k < NLISTS; k++)
		if(has_b[k]) displaylist_destroy(&blockl[k]);
	displaylist_scratch_end();
	assert(s_frees == 0 && s_live_bytes == 0 && "no carve frees after a failed pack");
	uint32_t packed, overflow, pack_fail;
	displaylist_block_counters(&packed, &overflow, &pack_fail);
	assert(pack_fail == 1);
	printf("PASS 4: pack failure leaves no carve traffic and no leak (pack_fail=%u)\n", pack_fail);

	/* ---- 5: scratch overflow falls back to the carve for that list only ---- */
	model_reset();
	s_fail_after = 0;
	displaylist_scratch_begin();
	struct displaylist big[4]; bool has_big[4];
	for(unsigned k = 0; k < 4; k++) {
		displaylist_init_presized(&big[k], 64, 12, true, 0);
		displaylist_set_tinted(&big[k], true);
		displaylist_allow_scratch(&big[k]);
		emit(&big[k], 8000, true, 5 + k); /* 4 x ~384 KiB of vertices > 768 KiB scratch */
		has_big[k] = big[k].data && !big[k].truncated;
		assert(has_big[k]);
		displaylist_finalize(&big[k], (uint16_t)(8000 * 4));
	}
	ok = displaylist_pack_block(big, has_big, 4, &blk, &blk_bytes);
	assert(ok);
	displaylist_scratch_end();
	displaylist_block_counters(&packed, &overflow, &pack_fail);
	assert(overflow > 0 && "the arena overflowed at least once");
	struct displaylist_block_stats bs;
	displaylist_block_stats(&bs);
	assert(bs.mode_requested && bs.mode_effective && bs.scratch_fail == 0);
	assert(bs.spill_bytes > 0 && "overflowed lists are accounted in bytes");
	assert(bs.copy_bytes >= blk_bytes && "pack copies are accounted in bytes");
	printf("     stats: grow_copies=%u copy_bytes=%u spill_bytes=%u\n",
		   bs.grow_copies, bs.copy_bytes, bs.spill_bytes);
	struct displaylist ref; displaylist_set_block_mode(false);
	for(unsigned k = 0; k < 4; k++) {
		displaylist_init_presized(&ref, 64, 12, true, 0);
		displaylist_set_tinted(&ref, true);
		emit(&ref, 8000, true, 5 + k);
		displaylist_finalize(&ref, (uint16_t)(8000 * 4));
		size_t cl = call_len_of(&ref);
		assert(cl == call_len_of(&big[k]) && !memcmp(ref.data, big[k].data, cl));
		displaylist_destroy(&ref);
	}
	for(unsigned k = 0; k < 4; k++) displaylist_destroy(&big[k]);
	displaylist_block_free(blk, blk_bytes);
	assert(s_live_bytes == 0 && "overflowed lists were carve-owned and freed by destroy");
	printf("PASS 5: scratch overflow falls back per list, bytes still identical, no leak\n");

	/* ---- 6: INTERLEAVED emission (the real mesher shape: one block loop feeds
	 * all 26 lists), unhinted -- bytes identical, and the 768 KiB scratch
	 * absorbs the copy-on-grow waste for a full generation. ---- */
	model_reset();
	displaylist_set_block_mode(true);
	displaylist_scratch_begin();
	for(unsigned k = 0; k < NLISTS; k++) {
		const struct kind* kd = &kinds[k % NKINDS];
		displaylist_init_presized(&blockl[k], 64, kd->tinted ? 12 : 9, kd->tinted, 0);
		displaylist_set_tinted(&blockl[k], kd->tinted);
		displaylist_allow_scratch(&blockl[k]);
	}
	for(unsigned q = 0; q < 300 * 4; q++)
		for(unsigned k = 0; k < NLISTS; k++) {
			const struct kind* kd = &kinds[k % NKINDS];
			if(q >= kd->quads * 4) continue;
			unsigned r = (q * 2654435761u + 7 + k);
			int16_t x = (int16_t)(r & 0x7fff), y = (int16_t)((r >> 3) & 0x7fff),
					z = (int16_t)((r >> 7) & 0x7fff);
			uint8_t light = (uint8_t)(r >> 11), s = (uint8_t)(r >> 13), tt = (uint8_t)(r >> 17);
			if(kd->tinted) {
				displaylist_pos(&blockl[k], x, y, z);
				displaylist_color(&blockl[k], light);
				displaylist_texcoord(&blockl[k], s, tt);
			} else {
				displaylist_vertex(&blockl[k], x, y, z, light, s, tt);
			}
		}
	for(unsigned k = 0; k < NLISTS; k++) {
		const struct kind* kd = &kinds[k % NKINDS];
		has_b[k] = kd->quads > 0 && blockl[k].data && !blockl[k].truncated;
		if(has_b[k]) displaylist_finalize(&blockl[k], (uint16_t)(kd->quads * 4));
		else displaylist_destroy(&blockl[k]);
	}
	unsigned allocs_before_pack = s_allocs;
	ok = displaylist_pack_block(blockl, has_b, NLISTS, &blk, &blk_bytes);
	assert(ok);
	displaylist_scratch_end();
	uint32_t ov6_before = overflow;
	displaylist_block_counters(NULL, &overflow, NULL);
	assert(allocs_before_pack == 0 && "interleaved emission stayed inside the scratch");
	assert(overflow == ov6_before && "no overflow for an interleaved unhinted generation");
	displaylist_set_block_mode(false);
	for(unsigned k = 0; k < NLISTS; k++) {
		if(!has_b[k]) continue;
		/* same geometry sequentially through the legacy path */
		const struct kind* kd = &kinds[k % NKINDS];
		displaylist_init_presized(&ref, 64, kd->tinted ? 12 : 9, kd->tinted, 0);
		displaylist_set_tinted(&ref, kd->tinted);
		emit(&ref, kd->quads, kd->tinted, 7 + k);
		displaylist_finalize(&ref, (uint16_t)(kd->quads * 4));
		size_t cl = call_len_of(&ref);
		assert(cl == call_len_of(&blockl[k]) && !memcmp(ref.data, blockl[k].data, cl));
		displaylist_destroy(&ref);
	}
	for(unsigned k = 0; k < NLISTS; k++) if(has_b[k]) displaylist_destroy(&blockl[k]);
	displaylist_block_free(blk, blk_bytes);
	assert(s_live_bytes == 0);
	printf("PASS 6: interleaved 26-list emission -- bytes identical, zero carve traffic before pack, no overflow\n");

	/* ---- 7: a list NOT flagged by the worker never touches the scratch even
	 * while a window is open (HUD / item / entity lists on the main thread). */
	model_reset();
	displaylist_set_block_mode(true);
	displaylist_scratch_begin();
	struct displaylist hud;
	displaylist_init_presized(&hud, 64, 9, false, 0);
	displaylist_set_tinted(&hud, false);
	emit(&hud, 40, false, 3);
	displaylist_finalize(&hud, (uint16_t)(40 * 4));
	assert(s_allocs >= 1 && !dl_in_scratch(hud.data) && !hud.block_owned
		   && "an unflagged list allocates from the carve, never the scratch");
	displaylist_scratch_end();
	displaylist_destroy(&hud);
	assert(s_live_bytes == 0);
	printf("PASS 7: an unflagged (main-thread) list never allocates from the worker scratch\n");

	printf("test_displaylist_block_equiv: PASS\n");
	return 0;
}
