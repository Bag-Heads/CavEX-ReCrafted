/* Default preserves the original synthetic regression. Opt-in diagnostic
 * uses the measured boot carve; its failing assertion is evidence, not a
 * suite pass. See README for why no captured-order proof can be claimed. */
static size_t churn_bytes = 36u * 1024 * 1024;
static size_t churn_min_tail;
#define CHURN_BYTES churn_bytes
#define CHURN_SPILL_BYTES (4u * 1024 * 1024)
#define CHURN_SLOTS 12000
static bool churn_active, churn_shrink;
static heap_cntrl churn_heap;
static unsigned char* churn_arena;
static size_t churn_spill_live;
static unsigned churn_spills, churn_trunc, churn_tail_reuses;
static bool churn_tenant;
static unsigned churn_tail_index;
static struct { uintptr_t begin, end; } churn_tails[256];
static struct { void* p; size_t n; bool carve; } churn_allocs[CHURN_SLOTS];
static void* churn_allocate(size_t n) {
	unsigned i = 0;
	while(i < CHURN_SLOTS && churn_allocs[i].p) i++;
	assert(i < CHURN_SLOTS);
	void* p = __lwp_heap_allocate(&churn_heap, (u32)n + 256);
	bool carve = p != NULL;
	if(carve && churn_tenant) {
		for(unsigned t = 0; t < 256; t++)
			if((uintptr_t)p >= churn_tails[t].begin && (uintptr_t)p + n <= churn_tails[t].end) {
				churn_tail_reuses++; break;
			}
	}
	if(!p) {
		churn_spills++;
		if(churn_spill_live + n + 256 > CHURN_SPILL_BYTES) return NULL;
		p = malloc(n + 256); assert(p);
		churn_spill_live += n + 256;
	}
	churn_allocs[i].p = p; churn_allocs[i].n = n; churn_allocs[i].carve = carve;
	memset((char*)p + n, 0xa5, 256);
	return p;
}
static unsigned churn_find(void* p) {
	for(unsigned i = 0; i < CHURN_SLOTS; i++) if(churn_allocs[i].p == p) return i;
	assert(!"unknown churn pointer"); return 0;
}
static void churn_guard(unsigned i) {
	for(unsigned j = 0; j < 256; j++)
		assert(((unsigned char*)churn_allocs[i].p)[churn_allocs[i].n + j] == 0xa5);
}
static void churn_free(void* p, size_t n) {
	unsigned i = churn_find(p); assert(churn_allocs[i].n == n); churn_guard(i);
	if(churn_allocs[i].carve) assert(__lwp_heap_free(&churn_heap, p));
	else { churn_spill_live -= n + 256; free(p); }
	churn_allocs[i].p = NULL;
}
static void* churn_resize(void* p, size_t old, size_t n) {
	unsigned i = churn_find(p); assert(churn_allocs[i].n == old); churn_guard(i);
	if(!churn_shrink || old - n < churn_min_tail) return NULL; /* pre-lever retained capacity */
	/* The production fallback realloc may move. Keep its old buffer here:
	 * conservative for carve fragmentation; never invent extra carve room. */
	if(!churn_allocs[i].carve) return NULL;
	heap_block* b = __lwp_heap_usrblockat(p);
	uintptr_t end = (uintptr_t)b + __lwp_heap_blocksize(b);
	assert(mem2_heap_shrink(&churn_heap, p, n + 256));
	uintptr_t begin = (uintptr_t)b + __lwp_heap_blocksize(b);
	if(begin < end) {
		unsigned t = churn_tail_index++ % 256;
		churn_tails[t].begin = begin; churn_tails[t].end = end;
	}
	churn_allocs[i].n = n; memset((char*)p + n, 0xa5, 256);
	return p;
}
static void churn_info(size_t* largest, size_t* total) {
	heap_iblock info; assert(!__lwp_heap_getinfo(&churn_heap, &info));
	*largest = 0; *total = info.free_size;
	unsigned links = 0;
	for(heap_block* b = churn_heap.first; b != __lwp_heap_tail(&churn_heap); b = b->next) {
		assert(++links <= info.free_blocks);
		assert(!(b->front_flag & 1) && (__lwp_heap_nextblock(b)->front_flag & 1));
		assert(b->next->prev == b && b->prev->next == b);
		if(b->front_flag > *largest) *largest = b->front_flag;
	}
	assert(links == info.free_blocks);
	for(unsigned i = 0; i < CHURN_SLOTS; i++) if(churn_allocs[i].p) churn_guard(i);
}
