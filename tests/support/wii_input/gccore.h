/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef TEST_WII_INPUT_GCCORE_H
#define TEST_WII_INPUT_GCCORE_H
#include <stdint.h>
typedef uint32_t u32;
typedef int32_t s32;
typedef uint16_t u16;
typedef int8_t s8;
typedef uint32_t lwp_t, mqbox_t, mutex_t;
lwp_t LWP_GetSelf(void);
typedef struct { uint32_t unused[8]; } GXTexObj;
#endif
