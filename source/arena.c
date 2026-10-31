/*
	Copyright (c) 2026

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include <assert.h>

#include "arena.h"

void arena_init(struct arena* a, void* base, size_t size, const char* name) {
	assert(a && base && size > 0);
	a->base = (uint8_t*)base;
	a->size = size;
	a->used = 0;
	a->name = name;
}

void* arena_alloc(struct arena* a, size_t size, size_t align) {
	assert(a && a->base && size > 0 && align > 0);
	// align must be a power of two — the bit-trick below depends on it.
	assert((align & (align - 1)) == 0);

	// Round `used` up to the next multiple of `align`.
	size_t aligned = (a->used + align - 1) & ~(align - 1);

	// Overflow-safe exhaustion check. Cap the addition first so an
	// adversarial caller passing SIZE_MAX can't wrap around past size.
	if(aligned > a->size || size > a->size - aligned)
		return NULL;

	void* p = a->base + aligned;
	a->used = aligned + size;
	return p;
}

void arena_reset(struct arena* a) {
	assert(a);
	a->used = 0;
}
