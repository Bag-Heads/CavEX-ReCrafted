/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef TEST_WII_INPUT_HEAP_H
#define TEST_WII_INPUT_HEAP_H
#include <gccore.h>
typedef struct { u32 unused; } heap_cntrl;
typedef struct { u32 free_size, used_size, free_blocks; } heap_iblock;
u32 __lwp_heap_getinfo(heap_cntrl* heap, heap_iblock* info);
#endif
