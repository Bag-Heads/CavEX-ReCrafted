#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "arena.h"

int main(void) {
	static uint8_t buf[1024] __attribute__((aligned(32)));
	struct arena a;
	arena_init(&a, buf, sizeof(buf), "test");

	assert(a.base == buf);
	assert(a.size == 1024);
	assert(a.used == 0);

	// Simple allocation.
	void* p1 = arena_alloc(&a, 100, 4);
	assert(p1 == buf);
	assert(a.used == 100);

	// Subsequent allocation gets aligned forward.
	void* p2 = arena_alloc(&a, 50, 4);
	assert((uintptr_t)p2 == (uintptr_t)(buf + 100));
	assert(a.used == 150);

	// 16-byte alignment should round 150 up to 160.
	void* p3 = arena_alloc(&a, 8, 16);
	assert((uintptr_t)p3 == (uintptr_t)(buf + 160));
	assert(a.used == 168);

	// 32-byte alignment should round 168 up to 192.
	void* p4 = arena_alloc(&a, 4, 32);
	assert((uintptr_t)p4 == (uintptr_t)(buf + 192));
	assert(((uintptr_t)p4 & 31) == 0);
	assert(a.used == 196);

	// Writes through returned pointers are valid (no overlap, in-bounds).
	memset(p1, 0xAA, 100);
	memset(p2, 0xBB, 50);
	memset(p3, 0xCC, 8);
	memset(p4, 0xDD, 4);
	for(int i = 0; i < 100; i++)
		assert(buf[i] == 0xAA);
	for(int i = 100; i < 150; i++)
		assert(buf[i] == 0xBB);
	for(int i = 160; i < 168; i++)
		assert(buf[i] == 0xCC);
	for(int i = 192; i < 196; i++)
		assert(buf[i] == 0xDD);

	// Exhaustion returns NULL without modifying used.
	size_t before = a.used;
	void* too_big = arena_alloc(&a, sizeof(buf), 4);
	assert(too_big == NULL);
	assert(a.used == before);

	// Reset rewinds to zero. Subsequent alloc returns the base again.
	arena_reset(&a);
	assert(a.used == 0);
	void* p5 = arena_alloc(&a, 200, 4);
	assert(p5 == buf);

	// Overflow-safe exhaustion: aligned offset itself past end.
	arena_reset(&a);
	(void)arena_alloc(&a, 1024 - 4, 4); // fills nearly the whole arena
	void* tail = arena_alloc(&a, 8, 32); // alignment pushes past end
	assert(tail == NULL);

	return 0;
}
