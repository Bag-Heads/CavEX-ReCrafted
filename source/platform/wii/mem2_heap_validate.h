/* Temporary, read-only Wii heap integrity probe. Include after lwp_heap.h (or
 * the host ABI adapter). No allocator, I/O, locks or IRQ changes here. */
#ifndef CW_MEM2_HEAP_VALIDATE_H
#define CW_MEM2_HEAP_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mem2_heap_bad {
	MEM2_HEAP_OK = 0, MEM2_HEAP_CONTROL, MEM2_HEAP_FIRST_TAG,
	MEM2_HEAP_PHYSICAL_SIZE, MEM2_HEAP_BACK_TAG,
	MEM2_HEAP_FINAL_TAG, MEM2_HEAP_LIST_RANGE,
	MEM2_HEAP_LIST_LINK,
	MEM2_HEAP_LIST_COUNT, MEM2_HEAP_SENTINEL
};

struct mem2_heap_check {
	enum mem2_heap_bad bad;
	uintptr_t address;
	size_t physical_steps, list_steps;
};

static inline bool mem2_heap_node_in_arena(uintptr_t p, uintptr_t base,
									uintptr_t final) {
	return p >= base && p <= final && final - p >= sizeof(heap_block)
		&& (p & 3u) == 0;
}

static inline bool mem2_heap_check_fail(struct mem2_heap_check* out,
						enum mem2_heap_bad why, uintptr_t addr) {
	out->bad = why;
	out->address = addr;
	return false;
}

static inline bool mem2_heap_validate(const heap_cntrl* heap,
		const heap_block* head, const heap_block* tail,
		uintptr_t base, size_t arena_size, struct mem2_heap_check* out) {
	*out = (struct mem2_heap_check){0};
	if(!heap || !head || !tail || arena_size < sizeof(heap_block) + 8u
	   || base > UINTPTR_MAX - arena_size)
		return mem2_heap_check_fail(out, MEM2_HEAP_CONTROL, base);
	uintptr_t final = base + arena_size - 8u;
	if((uintptr_t)heap->start != base || (uintptr_t)heap->final != final
	   || heap->pg_size != 32u || (base & 3u) != 0)
		return mem2_heap_check_fail(out, MEM2_HEAP_CONTROL, base);
	const heap_block* first = (const heap_block*)base;
	if(first->back_flag != HEAP_DUMMY_FLAG)
		return mem2_heap_check_fail(out, MEM2_HEAP_FIRST_TAG, base);

	size_t free_count = 0;
	uintptr_t p = base;
	size_t max_steps = arena_size / sizeof(heap_block) + 1u;
	for(size_t step = 0; step < max_steps; step++) {
		const heap_block* b = (const heap_block*)p;
		out->physical_steps++;
		if(p == final) {
			if(b->front_flag != HEAP_DUMMY_FLAG)
				return mem2_heap_check_fail(out, MEM2_HEAP_FINAL_TAG, p);
			break;
		}
		if(!mem2_heap_node_in_arena(p, base, final))
			return mem2_heap_check_fail(out, MEM2_HEAP_PHYSICAL_SIZE, p);
		size_t size = (size_t)(b->front_flag & ~HEAP_BLOCK_USED);
		if(size < sizeof(heap_block) || size > final - p || (size & 3u))
			return mem2_heap_check_fail(out, MEM2_HEAP_PHYSICAL_SIZE, p);
		uintptr_t next = p + size;
		if(((const heap_block*)next)->back_flag != b->front_flag)
			return mem2_heap_check_fail(out, MEM2_HEAP_BACK_TAG, next);
		if(!(b->front_flag & HEAP_BLOCK_USED)) free_count++;
		p = next;
	}
	if(p != final)
		return mem2_heap_check_fail(out, MEM2_HEAP_PHYSICAL_SIZE, p);
	/* On PPC32 head->next and tail->prev alias first/last themselves; the
	 * independent sentinel invariant is perm_null, then terminal links. */
	if(heap->perm_null != NULL)
		return mem2_heap_check_fail(out, MEM2_HEAP_SENTINEL, (uintptr_t)head);

	const heap_block* prev = head;
	const heap_block* node = heap->first;
	for(size_t i = 0; i <= free_count; i++) {
		if(node == tail) {
			if(i != free_count || heap->last != prev)
				return mem2_heap_check_fail(out, MEM2_HEAP_LIST_COUNT,
					(uintptr_t)node);
			return true;
		}
		uintptr_t addr = (uintptr_t)node;
		if(!mem2_heap_node_in_arena(addr, base, final))
			return mem2_heap_check_fail(out, MEM2_HEAP_LIST_RANGE, addr);
		/* Linear structural check only: a forged aligned interior node with
		 * self-consistent links can escape this diagnostic. Do not interpret
		 * an OK result as full heap integrity proof. */
		if(node->front_flag & HEAP_BLOCK_USED)
			return mem2_heap_check_fail(out, MEM2_HEAP_LIST_LINK, addr);
		const heap_block* next = node->next;
		if(next != tail
		   && !mem2_heap_node_in_arena((uintptr_t)next, base, final))
			return mem2_heap_check_fail(out, MEM2_HEAP_LIST_RANGE,
				(uintptr_t)next);
		bool prev_ok = prev == head ? heap->first == node : prev->next == node;
		bool next_ok = next == tail ? heap->last == node : next->prev == node;
		if(node->prev != prev || !prev_ok || !next_ok)
			return mem2_heap_check_fail(out, MEM2_HEAP_LIST_LINK, addr);
		out->list_steps++;
		prev = node;
		node = next;
	}
	return mem2_heap_check_fail(out, MEM2_HEAP_LIST_COUNT, (uintptr_t)node);
}

#endif
