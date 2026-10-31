/* libogc v3.1.0 sidecar allocator, host ABI adapter. The 8-byte boundary
 * tags, 32-byte pages, request rounding, first-fit order and free/coalescing
 * are Wii semantics. Only free-list pointers/sentinels are host-width.
 * The allocator bodies in lwp_heap.c.inc are verbatim, except includes.
 * Original SHA256: bb2dd70f3702051055dba663331495ae320728dda92dbfce8717e92aa4211be0.
 * Source: logc256kr/libogc/lwp_heap.c; helper authority: lwp_heap.inl.
 */
#include <stddef.h>
typedef uint32_t u32;
typedef bool BOOL;
#define TRUE true
#define FALSE false
#define HEAP_BLOCK_USED 1u
#define HEAP_DUMMY_FLAG 1u
#define HEAP_OVERHEAD 8u
#define HEAP_BLOCK_USED_OVERHEAD 8u
#define HEAP_MIN_SIZE (8u + sizeof(heap_block))
#define _CPU_ISR_Disable(level) ((level) = 0)
#define _CPU_ISR_Restore(level) ((void)(level))
typedef struct heap_block {
	u32 back_flag, front_flag;
	struct heap_block *next, *prev;
} heap_block;
typedef struct {
	heap_block *start, *final, *first, *perm_null, *last;
	u32 pg_size, reserved;
} heap_cntrl;
typedef struct { u32 free_blocks, free_size, used_blocks, used_size; } heap_iblock;
static heap_block* __lwp_heap_head(heap_cntrl* h) {
	return (heap_block*)((char*)&h->first - offsetof(heap_block, next));
}
static heap_block* __lwp_heap_tail(heap_cntrl* h) {
	return (heap_block*)((char*)&h->perm_null - offsetof(heap_block, next));
}
static heap_block* __lwp_heap_blockat(void* b, u32 n) { return (heap_block*)((char*)b + n); }
static u32 __lwp_heap_blocksize(heap_block* b) { return b->front_flag & ~1u; }
static heap_block* __lwp_heap_nextblock(heap_block* b) { return __lwp_heap_blockat(b, __lwp_heap_blocksize(b)); }
static heap_block* __lwp_heap_prevblock(heap_block* b) { return (heap_block*)((char*)b - (b->back_flag & ~1u)); }
static heap_block* __lwp_heap_usrblockat(void* p) { return (heap_block*)((char*)p - ((u32*)p)[-1] - 8); }
static bool __lwp_heap_blockfree(heap_block* b) { return !(b->front_flag & 1); }
static bool __lwp_heap_prev_blockfree(heap_block* b) { return !(b->back_flag & 1); }
static bool __lwp_heap_blockin(heap_cntrl* h, heap_block* b) { return (uintptr_t)b >= (uintptr_t)h->start && (uintptr_t)b <= (uintptr_t)h->final; }
static void* __lwp_heap_startuser(heap_block* b) { return &b->next; }
static bool __lwp_heap_pgsize_valid(u32 n) { return n && n % 8 == 0; }
static u32 __lwp_heap_buildflag(u32 n, u32 f) { return n | f; }
#include "lwp_heap.c.inc"
