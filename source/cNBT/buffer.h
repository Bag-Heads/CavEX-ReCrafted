/*
 * -----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Lukas Niederbremer <webmaster@flippeh.de> and Clark Gaebel <cg.wowus.cg@gmail.com>
 * wrote this file. As long as you retain this notice you can do whatever you
 * want with this stuff. If we meet some day, and you think this stuff is worth
 * it, you can buy us a beer in return.
 * -----------------------------------------------------------------------------
 */
#ifndef NBT_BUFFER_H
#define NBT_BUFFER_H

#include <stddef.h>

/*
 * A buffer is 'unlimited' storage for raw data. As long as buffer_append is
 * used to add data, it will automatically resize to make room. To read the
 * data, just access `data' directly.
 */
struct buffer {
    unsigned char* data; /* You can access the buffer's raw bytes through this pointer */
    size_t len;          /* Only accesses in the interval [data, data + len) are defined */
    size_t cap;          /* Internal use. The allocated size of the buffer. */
};

/*
 * Initialize a buffer with this macro.
 *
 * Usage:
 *   struct buffer b = BUFFER_INIT;
 * OR
 *   struct buffer b;
 *   b = BUFFER_INIT;
 */
#define BUFFER_INIT (struct buffer) { NULL, 0, 0 }

/*
 * Frees all memory associated with the buffer. The same buffer may be freed
 * multiple times without consequence.
 */
void buffer_free(struct buffer* b);

/*
 * Ensures there's enough room in the buffer for at least `reserved_amount'
 * bytes. Returns non-zero on failure. If such a failure occurs, the buffer
 * is deallocated and set to one which can be passed to buffer_free. Any other
 * usage is undefined.
 */
int buffer_reserve(struct buffer* b, size_t reserved_amount);

/*
 * Pre-sizes a FRESH (BUFFER_INIT) buffer's single backing allocation to
 * `initial_cap` bytes in one shot, instead of letting the first buffer_append
 * lazily start it at the small default and double its way up. `initial_cap`
 * of 0 is the exact prior default-init behaviour. Only valid on a buffer
 * whose `data` is still NULL -- use buffer_reserve to grow an in-use buffer.
 * Returns non-zero on failure (out of memory); the buffer is left usable
 * either way (a failed hint just falls back to the default-size path on the
 * next append, same recovery buffer_reserve already relies on).
 */
int buffer_reserve_exact(struct buffer* b, size_t initial_cap);

/*
 * Copies `n' bytes from `data' into the buffer. Returns non-zero if an
 * out-of-memory failure occured. If such a failure occurs, further usage of the
 * buffer results in undefined behavior.
 */
int buffer_append(struct buffer* b, const void* data, size_t n);

#endif
