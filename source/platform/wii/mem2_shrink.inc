/* Private libogc heap tail release, included by mem2.c and the host proof.
 * Layout authority: devkitPro/libogc libogc/lwp_heap.c allocate/free and
 * gc/ogc/lwp_heap.h (installed SDK). User[-1] holds the alignment offset
 * from the 8-byte boundary tag. Adjacent tags include bit 0 = USED.
 * Caller owns ptr and holds the heap mutex with interrupts disabled.
 * No search, allocation, copy, or change to the retained user address.
 */
static bool mem2_heap_shrink(heap_cntrl* heap, void* ptr, size_t bytes) {
	uint32_t offset = ((uint32_t*)ptr)[-1];
	heap_block* block = (heap_block*)((uint8_t*)ptr - offset - 8);
	if(block < heap->start || block >= heap->final
	   || !(block->front_flag & 1)) return false;
	size_t old = block->front_flag & ~1u;
	size_t prefix = (uint8_t*)ptr - (uint8_t*)block;
	if(bytes > old || prefix > old - bytes) return false;
	heap_block* next = (heap_block*)((uint8_t*)block + old);
	if(next > heap->final || next->back_flag != block->front_flag)
		return false;
	/* Release whole pages; preserve the original block's residue/alignment.
	 * A smaller remainder stays allocator-internal slack, like allocate(). */
	size_t release = (old - prefix - bytes) / heap->pg_size * heap->pg_size;
	if(release < sizeof(heap_block)) return true;
	size_t keep = old - release;
	heap_block* tail = (heap_block*)((uint8_t*)block + keep);
	block->front_flag = (uint32_t)keep | 1u;
	tail->back_flag = block->front_flag;
	if(!(next->front_flag & 1)) {
		/* Same forward coalescing/link replacement as __lwp_heap_free. */
		tail->front_flag = (uint32_t)release + next->front_flag;
		tail->next = next->next;
		tail->prev = next->prev;
		if(heap->first == next) heap->first = tail;
	} else {
		tail->front_flag = (uint32_t)release;
		tail->next = heap->first;
		tail->prev = heap->first->prev;
		heap->first = tail;
	}
	tail->prev->next = tail;
	tail->next->prev = tail;
	((heap_block*)((uint8_t*)tail + tail->front_flag))->back_flag
		= tail->front_flag;
	return true;
}
