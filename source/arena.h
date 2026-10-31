/*
	Copyright (c) 2026

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	Linear (bump) arena allocator. Hand it a fixed pre-allocated chunk of
	memory (typically a memalign'd 64 KB MEM2 block) and it serves
	short-lived scratch allocations from it. arena_reset is the only way
	to free space — there is no per-allocation free. The intended use is
	one arena per worker, reset at the start of each job.

	Why a custom arena instead of leaning on malloc:

	- On the Wii, the chunk mesher allocates O(20 KB) of scratch per
	  meshed chunk (block_data padded grid + vertex light cache +
	  visited bitmap). Doing that via malloc/free hits libogc's general
	  heap on every mesh, fragments MEM2 over time, and shows up as
	  heap-variance noise in profiling.
	- A bump allocator collapses N allocations into a single pointer
	  increment per request and a single reset per job. No free list,
	  no fragmentation, no realloc.
	- The arena lives where you put it (we put the mesher's arena in
	  MEM2 via memalign), so 32-byte alignment is preserved for any
	  buffer you ask for with align=32 — GX expects this.

	Failure mode: arena_alloc returns NULL if the arena is exhausted.
	Size the arena so this never fires in practice — NULL here signals a
	mis-sized arena, and the fix is bumping MESHER_ARENA_SIZE, not
	littering callers with recovery paths. Callers that CAN degrade
	safely must do it explicitly and loudly: chunk_mesher.c (audited
	2026-08-13) goes arena-first, then a LOGGED malloc fallback, then
	NULL-safe conservative defaults (full-bright vertex light /
	all-sides-reachable) — never a partial mesh, never a silent skip.
*/

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

struct arena {
	uint8_t* base;
	size_t size;
	size_t used;
	const char* name; // for diagnostics
};

// Wrap a caller-provided buffer in an arena. The buffer must outlive
// every pointer ever returned by arena_alloc against this arena.
void arena_init(struct arena* a, void* base, size_t size, const char* name);

// Allocate `size` bytes aligned up to `align` (power of two). Returns
// NULL on exhaustion. Pointers remain valid until arena_reset is called.
void* arena_alloc(struct arena* a, size_t size, size_t align);

// Mark every allocation in the arena as free. O(1). Does not touch the
// underlying memory.
void arena_reset(struct arena* a);

#endif
