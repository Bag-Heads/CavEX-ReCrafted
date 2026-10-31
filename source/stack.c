/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stack.h"

// OOM-dropped pushes: realloc-grow failures that were refused instead of crashing
// (see stack_push). Non-zero means a stack hit a heap limit and some entries were
// dropped -- for a lighting flood that is a bounded relight gap, never the
// memcpy-through-NULL DSI crash it used to be. Surfaced in the watchdog dump.
static uint32_t s_stack_oom_drops = 0;
uint32_t stack_oom_drops(void) {
	return s_stack_oom_drops;
}

void stack_create(struct stack* stk, size_t inital_size, size_t element_size) {
	assert(stk != NULL && inital_size > 0 && element_size > 0);

	stk->element_size = element_size;
	stk->index = 0;
	stk->length = inital_size;
	stk->data = malloc(stk->length * stk->element_size);
	stk->fixed = false;
	assert(stk->data);
}

void stack_create_fixed(struct stack* stk, void* buffer, size_t capacity,
						size_t element_size) {
	assert(stk != NULL && buffer != NULL && capacity > 0 && element_size > 0);

	stk->element_size = element_size;
	stk->index = 0;
	stk->length = capacity;
	stk->data = buffer;
	stk->fixed = true;
}

void stack_push(struct stack* stk, void* obj) {
	assert(stk != NULL && obj != NULL);

	// Grow when full, OR when data is NULL (a malloc-failed stack_create -- its
	// assert is a NDEBUG no-op, so we must self-heal here rather than memcpy to NULL).
	if(stk->index >= stk->length || stk->data == NULL) {
		// A fixed-capacity stack cannot grow — sizing it too small is a
		// caller bug, not a recoverable condition.
		assert(!stk->fixed);
		size_t new_length = stk->length > 0 ? stk->length * 2 : 16;
		// CRITICAL: realloc on failure returns NULL but KEEPS the old buffer. The old
		// code assigned the NULL straight into stk->data (leaking the old buffer AND
		// nulling the pointer); under -DNDEBUG the assert(stk->data) was a no-op, so
		// the next memcpy wrote to (NULL + index*element_size) -- the far-ring
		// lighting flood's 0x00800000 DSI crash (a runaway flood grew the stack until
		// an 8-16MB realloc OOM'd on the 24MB MEM1 heap). Use a temp + refuse to grow
		// on failure: the old buffer stays valid and full, this push is DROPPED
		// (counted) instead of crashing -- a bounded relight gap, never a fault.
		void* nd = realloc(stk->data, new_length * stk->element_size);
		if(!nd) {
			s_stack_oom_drops++;
			return;
		}
		stk->data = nd;
		stk->length = new_length;
	}

	memcpy((uint8_t*)stk->data + (stk->index++) * stk->element_size, obj,
		   stk->element_size);
}

/* Grow the backing store so it holds at least `need` elements. Unlike
 * stack_push, a failure is REPORTED (false) and nothing is lost: the stack is
 * unchanged. A lossless producer (the G9 server light drain) reserves before
 * it pops, so its pushes can never be dropped. A fixed stack never grows. */
bool stack_reserve(struct stack* stk, size_t need) {
	assert(stk != NULL);
	if(need <= stk->length && stk->data != NULL)
		return true;
	if(stk->fixed)
		return false;
	size_t new_length = stk->length > 0 ? stk->length : 16;
	while(new_length < need)
		new_length *= 2;
	void* nd = realloc(stk->data, new_length * stk->element_size);
	if(!nd)
		return false;
	stk->data = nd;
	stk->length = new_length;
	return true;
}

bool stack_empty(struct stack* stk) {
	assert(stk != NULL);

	return stk->index == 0;
}

size_t stack_size(struct stack* stk) {
	assert(stk != NULL);

	return stk->index;
}

void stack_at(struct stack* stk, void* obj, size_t index) {
	assert(stk != NULL && obj != NULL);

	if(index < stk->index)
		memcpy(obj, (uint8_t*)stk->data + index * stk->element_size,
			   stk->element_size);
}

bool stack_pop(struct stack* stk, void* obj) {
	assert(stk != NULL && obj != NULL);

	if(stack_empty(stk))
		return false;

	memcpy(obj, (uint8_t*)stk->data + (--stk->index) * stk->element_size,
		   stk->element_size);

	/*if(stk->index * 4 <= stk->length) {
		stk->length /= 2;
		stk->data = realloc(stk->data, stk->length * stk->element_size);
		assert(stk->data);
	}*/

	return true;
}

void stack_clear(struct stack* stk) {
	assert(stk != NULL);

	stk->index = 0;
}

void stack_destroy(struct stack* stk) {
	assert(stk != NULL);

	// Fixed stacks borrow their buffer from the caller (arena/static) — the
	// owner frees it, not us.
	if(!stk->fixed && stk->data)
		free(stk->data);
}
