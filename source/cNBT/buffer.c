/*
* -----------------------------------------------------------------------------
* "THE BEER-WARE LICENSE" (Revision 42):
* Lukas Niederbremer <webmaster@flippeh.de> and Clark Gaebel <cg.wowus.cg@gmail.com>
* wrote this file. As long as you retain this notice you can do whatever you
* want with this stuff. If we meet some day, and you think this stuff is worth
* it, you can buy us a beer in return.
* -----------------------------------------------------------------------------
*/
#include "buffer.h"

#include <assert.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifdef __GNUC__
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(  (x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

/* Allocates the buffer's single backing store directly at `cap`, skipping
 * the default-then-grow path. Shares lazy_init's degrade-on-OOM shape: on
 * failure b->data stays NULL (b->cap/len still get written, exactly as the
 * pre-existing lazy_init behaviour already did) so a later buffer_append's
 * "if(data==NULL) lazy_init(b)" check still fires and retries at the
 * default size, same as today. */
static int buffer_init_capacity(struct buffer* b, size_t cap)
{
    assert(b->data == NULL);

    *b = (struct buffer) {
        .data = malloc(cap),
        .len  = 0,
        .cap  = cap
    };

    if(unlikely(b->data == NULL))
        return 1;

    return 0;
}

static int lazy_init(struct buffer* b)
{
    return buffer_init_capacity(b, 1024);
}

/* T1 (2026-09-01 chunk-save pre-size fix): lets a caller that already knows
 * roughly how large the buffer will grow (e.g. nbt_dump_binary_hint for a
 * full chunk's ~82KB of fixed-size byte arrays) skip lazy_init's 1KB
 * default-and-double path -- ~18 realloc+copy cycles to reach ~82KB with a
 * ~192KB MEM1 transient peak across the doublings. Only meaningful on a
 * FRESH (BUFFER_INIT) buffer, matching buffer_reserve's own "grow, don't
 * shrink" contract -- callers that resize an in-use buffer keep using
 * buffer_reserve. `initial_cap == 0` degrades to the exact prior default
 * (lazy_init), so this changes nothing for any existing caller that never
 * asks for a hint. */
int buffer_reserve_exact(struct buffer* b, size_t initial_cap)
{
    assert(b);
    assert(b->data == NULL);

    if(unlikely(initial_cap == 0))
        return lazy_init(b);

    return buffer_init_capacity(b, initial_cap);
}

void buffer_free(struct buffer* b)
{
    assert(b);

    free(b->data);

    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int buffer_reserve(struct buffer* b, size_t reserved_amount)
{
    assert(b);

    if(unlikely(b->data == NULL) &&
       unlikely(lazy_init(b)))
        return 1;

    if(likely(b->cap >= reserved_amount))
        return 0;

    while(b->cap < reserved_amount)
        b->cap *= 2;

    unsigned char* temp = realloc(b->data, b->cap);

    if(unlikely(temp == NULL))
        return buffer_free(b), 1;

    b->data = temp;

    return 0;
}

int buffer_append(struct buffer* b, const void* data, size_t n)
{
    assert(b);

    if(unlikely(b->data == NULL) &&
       unlikely(lazy_init(b)))
        return 1;

    if(unlikely(buffer_reserve(b, b->len + n)))
        return 1;

    memcpy(b->data + b->len, data, n);
    b->len += n;

    return 0;
}

