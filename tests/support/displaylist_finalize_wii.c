/* Execute the actual Wii emitter/finalizer/render range calculation on host.
 * The heap fixture has libogc's boundary tags and offset word. Its native
 * host pointers only affect the minimum free-block size, not the Wii bytes.
 * Production tail splicing itself is included, not reimplemented. */
#ifdef PLATFORM_PC
#undef PLATFORM_PC
#endif
#define PLATFORM_WII
#include "gx_finalize/wii_names.h"
#include "../../source/platform/wii/displaylist.c"
#include <stdio.h>
#include <stdlib.h>

/* Match the installed libogc SDK's -fno-strict-aliasing: its heap sentinels
 * alias heap_cntrl pointer fields through heap_block views. */
#if defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("no-strict-aliasing")
#endif
#include "libogc_heap/host.h"
#if defined(__GNUC__)
#pragma GCC pop_options
#endif
#include "../../source/platform/wii/mem2_heap_validate.h"
#include "../../source/platform/wii/mem2_shrink.inc"
#include "libogc_heap/churn_alloc.h"

static void test_mem2_heap_validator(void) {
	uint8_t* raw = malloc(4096 + 32), image[4096];
	assert(raw);
	uintptr_t base = ((uintptr_t)raw + 31u) & ~(uintptr_t)31u;
	heap_cntrl h = {0}, saved;
	assert(__lwp_heap_init(&h, (void*)base, 4096, 32));
	struct mem2_heap_check result;
	#define CHECK_HEAP() (mem2_heap_validate(&h, __lwp_heap_head(&h), \
		__lwp_heap_tail(&h), base, 4096, &result), \
		assert(result.physical_steps <= 4096 / sizeof(heap_block) + 1), \
		assert(result.list_steps <= 4096 / sizeof(heap_block) + 1), \
		result.bad == MEM2_HEAP_OK)
	assert(CHECK_HEAP() && result.list_steps == 1); /* pristine */
	void* a = __lwp_heap_allocate(&h, 96);
	void* b = __lwp_heap_allocate(&h, 96);
	void* c = __lwp_heap_allocate(&h, 96);
	assert(a && b && c);
	assert(__lwp_heap_free(&h, b));
	memcpy(image, (void*)base, sizeof(image)); saved = h;
	assert(CHECK_HEAP() && result.bad == MEM2_HEAP_OK);
	assert(!memcmp(image, (void*)base, sizeof(image))
		&& !memcmp(&saved, &h, sizeof(h)));
	#define RESTORE_HEAP() do { memcpy((void*)base, image, sizeof(image)); h = saved; } while(0)
	h.start->front_flag = 0;
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_PHYSICAL_SIZE);
	RESTORE_HEAP();
	h.start->front_flag = 4096u;
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_PHYSICAL_SIZE);
	RESTORE_HEAP();
	h.start = (heap_block*)(base + 32);
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_CONTROL);
	RESTORE_HEAP();
	h.final = (heap_block*)(base + 4096);
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_CONTROL);
	RESTORE_HEAP();
	heap_block* second = __lwp_heap_nextblock(h.start);
	second->back_flag ^= 4u;
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_BACK_TAG);
	RESTORE_HEAP();
	h.first->next = (heap_block*)(base + 4096 + 64);
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_LIST_RANGE);
	RESTORE_HEAP();
	h.first->next = h.first; /* short free-list cycle */
	assert(!CHECK_HEAP() && (result.bad == MEM2_HEAP_LIST_LINK
		|| result.bad == MEM2_HEAP_LIST_COUNT));
	RESTORE_HEAP();
	h.first->prev = __lwp_heap_tail(&h);
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_LIST_LINK);
	RESTORE_HEAP();
	h.perm_null = h.first;
	assert(!CHECK_HEAP() && result.bad == MEM2_HEAP_SENTINEL);
	RESTORE_HEAP();
	assert(CHECK_HEAP());
	assert(__lwp_heap_free(&h, a) && __lwp_heap_free(&h, c));
	assert(CHECK_HEAP() && result.list_steps == 1); /* fully coalesced */
	void* whole = __lwp_heap_allocate(&h, 4032);
	assert(whole && CHECK_HEAP() && result.list_steps == 0);
	assert(__lwp_heap_free(&h, whole) && CHECK_HEAP()
		&& result.list_steps == 1);
	#undef RESTORE_HEAP
	#undef CHECK_HEAP
	free(raw);
}

struct allocation {
	void *raw, *data;
	size_t requested, initial_block;
	heap_cntrl heap;
	heap_block head, end;
};
static struct allocation allocations[4];
static bool free_neighbor, fail_shrink, move_shrink;
static unsigned fail_allocate;
static unsigned pointer_prefix = 32;
static uint64_t released;
carve_callback mem2_carve_observe;

static struct allocation* lookup(void* p) {
	for(unsigned i = 0; i < 4; i++)
		if(allocations[i].data == p) return &allocations[i];
	assert(!"unknown allocation"); return NULL;
}
static void verify(struct allocation* a) {
	uint8_t* guard = (uint8_t*)a->data + a->requested;
	for(unsigned i = 0; i < 256; i++) assert(guard[i] == 0xa5);
	heap_block* b = a->heap.start;
	unsigned blocks = 0, free_blocks = 0;
	while(b != a->heap.final) {
		assert(++blocks < 10);
		unsigned size = b->front_flag & ~1u;
		assert(size >= sizeof(heap_block));
		heap_block* next = (heap_block*)((uint8_t*)b + size);
		assert(next <= a->heap.final && next->back_flag == b->front_flag);
		if(!(b->front_flag & 1)) {
			free_blocks++;
			assert(next->front_flag & 1); // no adjacent uncoalesced free blocks
		}
		b = next;
	}
	unsigned links = 0;
	for(b = a->heap.first; b != &a->end; b = b->next) {
		assert(++links <= free_blocks && !(b->front_flag & 1));
		assert(b->next->prev == b && b->prev->next == b);
	}
	assert(links == free_blocks && a->head.next == a->heap.first);
}
void* mem2_alloc(size_t n) {
	if(churn_active) return churn_allocate(n);
	if(fail_allocate && --fail_allocate == 0) return NULL;
	struct allocation* a = NULL;
	for(unsigned i = 0; i < 4; i++) if(!allocations[i].raw) { a = &allocations[i]; break; }
	assert(a);
	/* libogc allocate: rounded request + page + 8-byte tag. */
	size_t block = ((n + 256 + 31) & ~(size_t)31) + 40;
	a->raw = malloc(block + 256 + 64);
	assert(a->raw);
	uint8_t* user = (uint8_t*)(((uintptr_t)a->raw + 95) & ~(uintptr_t)31);
	uint8_t* base = user - pointer_prefix;
	a->heap.start = (heap_block*)base;
	a->heap.pg_size = 32;
	a->initial_block = block;
	a->heap.start->back_flag = 1;
	a->heap.start->front_flag = (uint32_t)block | 1;
	a->data = user;
	((uint32_t*)a->data)[-1] = pointer_prefix - 8;
	a->requested = n;
	a->head.next = &a->end; a->end.prev = &a->head;
	heap_block* next = (heap_block*)(base + block);
	next->back_flag = (uint32_t)block | 1;
	if(free_neighbor) {
		next->front_flag = 128;
		next->prev = &a->head; next->next = &a->end;
		a->head.next = next; a->end.prev = next;
		next = (heap_block*)((uint8_t*)next + 128);
		next->back_flag = 128;
	}
	next->front_flag = 1;
	a->heap.final = next; a->heap.first = a->head.next;
	memset((uint8_t*)a->data + n, 0xa5, 256);
	verify(a);
	return a->data;
}
void mem2_free_sized(void* p, size_t n) {
	if(!p) return;
	if(churn_active) { churn_free(p, n); return; }
	struct allocation* a = lookup(p);
	assert(n == a->requested); verify(a);
	free(a->raw); memset(a, 0, sizeof(*a));
}
void* mem2_realloc(void* p, size_t old, size_t n) {
	void* q = mem2_alloc(n);
	if(q) { memcpy(q, p, old < n ? old : n); mem2_free_sized(p, old); }
	return q;
}
void* mem2_shrink(void* p, size_t old, size_t n) {
	if(churn_active) return churn_resize(p, old, n);
	struct allocation* a = lookup(p);
	assert(old == a->requested && n <= old); verify(a);
	if(fail_shrink) return NULL;
	if(move_shrink) return mem2_realloc(p, old, n);
	assert(mem2_heap_shrink(&a->heap, p, n + 256));
	released += a->initial_block - (a->heap.start->front_flag & ~1u);
	a->requested = n; memset((uint8_t*)p + n, 0xa5, 256); verify(a);
	/* Repeated shrink/no-op cannot corrupt links or move the canary. */
	assert(mem2_heap_shrink(&a->heap, p, n + 256)); verify(a);
	assert(!mem2_heap_shrink(&a->heap, p, old + 1024)); verify(a);
	return p;
}

#include "libogc_heap/churn.h"
void test_wii_finalize_equiv(void) {
	test_mem2_heap_validator();
	assert(!displaylist_compact_enabled());
	displaylist_set_compact(true);
	test_shared_carve_churn();
	uint64_t before = 0, after = 0;
	unsigned cases = 0;
	for(unsigned tinted = 0; tinted < 2; tinted++)
	for(unsigned count = 1; count <= 1025; count++)
	for(unsigned hint = 0; hint < 3; hint++)
	for(unsigned path = 0; path < 6; path++) {
		pointer_prefix = 16 + 8 * (count % 4); // all libogc user/tag residues
		free_neighbor = path & 1;
		fail_shrink = path == 2; move_shrink = path >= 3;
		displaylist_set_compact(path != 5);
		bool retained = path == 2 || path == 4 || path == 5;
		struct displaylist l;
		size_t index = 35 + count * (tinted ? 12 : 9);
		displaylist_init_presized(&l, 4, 12, tinted,
			hint == 0 ? 0 : hint == 1 ? index : index * 3);
		displaylist_set_tinted(&l, tinted);
		for(unsigned i = 0; i < count; i++) {
			displaylist_set_tint(&l, i, i * 3, i * 7);
			displaylist_vertex(&l, i * 13, -i, i * 31, i, i * 5, i * 11);
		}
		assert(!l.truncated && l.index == index);
		size_t rounded = (index + 31) & ~(size_t)31, capacity = l.length + 32;
		uint8_t* reference = calloc(1, rounded);
		assert(reference);
		memcpy(reference + 35, (uint8_t*)l.data + 35, index - 35);
		reference[32] = 0x80 | (tinted ? 4 : 0);
		uint16_t count16 = count; memcpy(reference + 33, &count16, 2);
		void* original = l.data;
		fail_allocate = path == 4 ? 1 : 0; // general-heap move allocation fails
		displaylist_finalize(&l, count);
		assert(l.finished && !l.truncated && l.index == index);
		assert(l.length == (retained ? capacity : rounded));
		if(!move_shrink || retained) assert(l.data == original);
		assert(!memcmp(l.data, reference, rounded));
		assert(gx_flush_ptr == l.data && gx_flush_bytes == rounded);
		displaylist_render(&l);
		assert(gx_call_ptr == (uint8_t*)l.data + 32);
		assert(gx_call_bytes == (index - 1) / 32 * 32);
		assert((uint8_t*)gx_call_ptr + gx_call_bytes <= (uint8_t*)l.data + l.length);
		assert(gx_flush_bytes <= l.length);
		uint32_t n, slack, pad;
		displaylist_storage_stats(&n, &slack, &pad);
		assert(n == 1 && slack == l.length - rounded);
		assert(pad == (retained ? 32 : 0));
		if(path < 2) { before += capacity - rounded; after += slack; cases++; }
		struct displaylist published = l; // mesher publication transfers by value
		displaylist_destroy(&published); displaylist_destroy(&published);
		free(reference);
		displaylist_storage_stats(&n, &slack, &pad);
		assert(!n && !slack && !pad);
	}
	for(unsigned grow = 0; grow < 2; grow++) {
		struct displaylist l;
		displaylist_init(&l, 4, 12); fail_allocate = grow ? 2 : 1;
		for(unsigned i = 0; i < 1025; i++)
			displaylist_vertex(&l, i, i, i, i, i, i);
		assert(l.truncated); displaylist_destroy(&l);
	}
	printf("PASS Wii actual finalizer/GX 9/12 strides, boundaries/hints/"
		"in-place/coalesce/move/NULL/canaries: %u in-place lists; summed "
		"dls %llu -> %llu B, dlp %u -> 0 B, heap released %llu B\n",
		cases, (unsigned long long)before, (unsigned long long)after,
		cases * 32, (unsigned long long)released);
}
